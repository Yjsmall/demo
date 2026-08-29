#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "context_reader/pdf/document_session.hpp"
#include "context_reader/pdf/mupdf_engine.hpp"
#include "context_reader/pdf/pdf_engine.hpp"
#include "context_reader/shared/error.hpp"

namespace {

constexpr std::string_view tool_version = "0.1.0";
constexpr int exit_usage = 2;
constexpr int exit_input = 3;
constexpr int exit_processing = 4;
constexpr int exit_output = 5;
constexpr int exit_validation = 6;

class AlgorithmHandle final {
public:
    AlgorithmHandle() = default;
    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
    ~AlgorithmHandle() {
        if(value_ != nullptr) {
            BCryptCloseAlgorithmProvider(value_, 0);
        }
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE* address() noexcept { return &value_; }
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }

private:
    BCRYPT_ALG_HANDLE value_ = nullptr;
};

class HashHandle final {
public:
    HashHandle() = default;
    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    ~HashHandle() {
        if(value_ != nullptr) {
            BCryptDestroyHash(value_);
        }
    }

    [[nodiscard]] BCRYPT_HASH_HANDLE* address() noexcept { return &value_; }
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }

private:
    BCRYPT_HASH_HANDLE value_ = nullptr;
};

struct Options final {
    enum class Command {
        inspect,
        render,
        text,
        roundtrip,
        compare,
    };

    Command command = Command::inspect;
    std::filesystem::path document_path;
    std::filesystem::path output_directory;
    std::filesystem::path comparison_directory;
    std::size_t page_number = 1;
    double scale = 1.0;
    double device_pixel_ratio = 1.0;
};

[[nodiscard]] bool nt_success(NTSTATUS status) noexcept {
    return status >= 0;
}

[[nodiscard]] std::string hex_string(const std::array<std::uint8_t, 32>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for(const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

[[nodiscard]] bool sha256_file(
    const std::filesystem::path& path,
    std::string& digest,
    std::string& error
) {
    AlgorithmHandle algorithm;
    if(!nt_success(BCryptOpenAlgorithmProvider(
           algorithm.address(),
           BCRYPT_SHA256_ALGORITHM,
           nullptr,
           0
       ))) {
        error = "could not initialize Windows SHA-256 provider";
        return false;
    }

    DWORD object_size = 0;
    DWORD result_size = 0;
    if(!nt_success(BCryptGetProperty(
           algorithm.get(),
           BCRYPT_OBJECT_LENGTH,
           reinterpret_cast<PUCHAR>(&object_size),
           sizeof(object_size),
           &result_size,
           0
       ))) {
        error = "could not query Windows SHA-256 provider";
        return false;
    }

    std::vector<std::uint8_t> hash_object(object_size);
    HashHandle hash;
    if(!nt_success(BCryptCreateHash(
           algorithm.get(),
           hash.address(),
           hash_object.data(),
           object_size,
           nullptr,
           0,
           0
       ))) {
        error = "could not create SHA-256 state";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if(!input) {
        error = "could not open input document for hashing";
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while(input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = input.gcount();
        if(bytes_read > 0 && !nt_success(BCryptHashData(
                               hash.get(),
                               reinterpret_cast<PUCHAR>(buffer.data()),
                               static_cast<ULONG>(bytes_read),
                               0
                           ))) {
            error = "could not update SHA-256 state";
            return false;
        }
    }
    if(!input.eof()) {
        error = "could not read input document for hashing";
        return false;
    }

    std::array<std::uint8_t, 32> hash_bytes{};
    if(!nt_success(BCryptFinishHash(
           hash.get(),
           hash_bytes.data(),
           static_cast<ULONG>(hash_bytes.size()),
           0
       ))) {
        error = "could not finish SHA-256 digest";
        return false;
    }

    digest = hex_string(hash_bytes);
    return true;
}

[[nodiscard]] unsigned int rotation_degrees(context_reader::PageRotation rotation) noexcept {
    return static_cast<unsigned int>(rotation);
}

[[nodiscard]] std::string document_json(
    std::string_view document_sha256,
    const std::vector<context_reader::PageInfo>& pages
) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(15);
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"document\": {\"sha256\": \"" << document_sha256
           << "\", \"page_count\": " << pages.size() << "},\n"
           << "  \"coordinate_system\": {\n"
           << "    \"page\": {\"unit\": \"point\", \"origin\": \"crop_box_top_left\", "
              "\"x_axis\": \"right\", \"y_axis\": \"down\"}\n"
           << "  },\n"
           << "  \"pages\": [";

    for(std::size_t index = 0; index < pages.size(); ++index) {
        const auto& page = pages[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"index\": " << page.index << ", \"number\": " << (page.index + 1)
               << ", \"width_points\": " << page.size.width
               << ", \"height_points\": " << page.size.height
               << ", \"rotation_degrees\": " << rotation_degrees(page.rotation) << "}";
    }
    if(!pages.empty()) {
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

[[nodiscard]] std::string run_manifest_json(
    std::string_view document_sha256,
    std::string_view invocation_arguments_json,
    std::chrono::milliseconds duration
) {
    std::ostringstream output;
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"tool\": {\"name\": \"reader-probe\", \"version\": \"" << tool_version
           << "\"},\n"
           << "  \"kernel\": {\"version\": \"0.1.0\", \"build_id\": \"development\", "
              "\"abi_version\": 1},\n"
           << "  \"dependencies\": {\"mupdf\": \"" << context_reader::MuPdfEngine::version()
           << "\"},\n"
           << "  \"platform\": {\"os\": \"windows\", \"arch\": \"x86_64\", \"compiler\": \""
           << CONTEXT_READER_COMPILER_NAME << "\"},\n"
           << "  \"input\": {\"document_sha256\": \"" << document_sha256 << "\"},\n"
           << "  \"invocation\": {\"arguments\": " << invocation_arguments_json
           << ", \"seed\": null},\n"
           << "  \"result\": {\"status\": \"passed\", \"duration_ms\": " << duration.count()
           << "}\n"
           << "}\n";
    return output.str();
}

[[nodiscard]] std::string json_string(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for(const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch(character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if(byte < 0x20U) {
                    output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                           << static_cast<unsigned int>(byte) << std::dec;
                } else {
                    output << character;
                }
                break;
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] std::string page_stem(std::size_t page_number) {
    std::ostringstream output;
    output << "page-" << std::setw(4) << std::setfill('0') << page_number;
    return output.str();
}

[[nodiscard]] bool write_text_file(
    const std::filesystem::path& path,
    std::string_view content,
    std::string& error
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        error = "could not create output file";
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if(!output) {
        error = "could not write output file";
        return false;
    }
    return true;
}

[[nodiscard]] bool write_binary_file(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& content,
    std::string& error
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        error = "could not create binary output file";
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(content.data()),
        static_cast<std::streamsize>(content.size())
    );
    if(!output) {
        error = "could not write binary output file";
        return false;
    }
    return true;
}

[[nodiscard]] bool ensure_output_directory(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path, filesystem_error);
    return !filesystem_error;
}

[[nodiscard]] bool parse_positive_double(const wchar_t* text, double& value) {
    wchar_t* end = nullptr;
    value = std::wcstod(text, &end);
    return end != text && end != nullptr && *end == L'\0' && std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool parse_page_number(const wchar_t* text, std::size_t& value) {
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoull(text, &end, 10);
    if(end == text || end == nullptr || *end != L'\0' || parsed == 0 ||
       parsed > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

[[nodiscard]] std::string text_page_json(
    std::string_view document_sha256,
    std::size_t page_number,
    const context_reader::PageText& page_text
) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(15)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"document_sha256\": \"" << document_sha256 << "\",\n"
           << "  \"page\": " << page_number << ",\n"
           << "  \"coordinate_system\": {\"unit\": \"point\", \"origin\": "
              "\"crop_box_top_left\", \"x_axis\": \"right\", \"y_axis\": \"down\"},\n"
           << "  \"text\": " << json_string(page_text.text) << ",\n"
           << "  \"lines\": [";
    for(std::size_t index = 0; index < page_text.lines.size(); ++index) {
        const auto& line = page_text.lines[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"text\": " << json_string(line.text)
               << ", \"vertical\": " << (line.vertical ? "true" : "false")
               << ", \"bounds\": {\"x\": " << line.bounds.x << ", \"y\": " << line.bounds.y
               << ", \"width\": " << line.bounds.width << ", \"height\": "
               << line.bounds.height << "}}";
    }
    if(!page_text.lines.empty()) {
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

[[nodiscard]] std::string render_json(
    std::string_view document_sha256,
    std::size_t page_number,
    const context_reader::EncodedPageImage& image,
    std::string_view png_sha256,
    std::string_view png_name
) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(15)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"document_sha256\": \"" << document_sha256 << "\",\n"
           << "  \"page\": " << page_number << ",\n"
           << "  \"pixels_per_point\": " << image.pixels_per_point << ",\n"
           << "  \"width_pixels\": " << image.width_pixels << ",\n"
           << "  \"height_pixels\": " << image.height_pixels << ",\n"
           << "  \"png\": {\"path\": \"" << png_name << "\", \"sha256\": \""
           << png_sha256 << "\"}\n"
           << "}\n";
    return output.str();
}

[[nodiscard]] int open_session(
    const Options& options,
    std::string& document_sha256,
    std::unique_ptr<context_reader::DocumentSession>& session
) {
    std::string error_message;
    if(!sha256_file(options.document_path, document_sha256, error_message)) {
        std::cerr << "reader-probe: " << error_message << '\n';
        return exit_input;
    }

    context_reader::MuPdfEngine engine;
    auto session_result = context_reader::DocumentSession::open(engine, options.document_path);
    if(!session_result) {
        const auto error_code = session_result.error().code();
        std::cerr << "reader-probe: " << session_result.error().message() << '\n';
        return error_code == context_reader::ErrorCode::not_found ? exit_input : exit_processing;
    }
    session = std::move(session_result).value();
    return 0;
}

void print_help() {
    std::cout << "Usage:\n"
              << "  reader-probe inspect <pdf> [--format json] [--output <directory>]\n"
              << "  reader-probe render <pdf> --page <1-based> [--scale <n>] [--dpr <n>] --output <directory>\n"
              << "  reader-probe text <pdf> --page <1-based> [--output <directory>]\n"
              << "  reader-probe roundtrip <pdf> --page <1-based> [--scale <n>] [--dpr <n>] [--output <directory>]\n"
              << "  reader-probe compare <actual-directory> <baseline-directory> [--format json]\n"
              << "  reader-probe --help\n"
              << "  reader-probe --version\n\n"
              << "Exit codes: 0 success, 2 usage, 3 input, 4 PDF processing, 5 output, 6 validation.\n";
}

[[nodiscard]] bool parse_options(int argc, wchar_t** argv, Options& options) {
    if(argc < 3) {
        return false;
    }
    const std::wstring_view command(argv[1]);
    if(command == L"inspect") {
        options.command = Options::Command::inspect;
    } else if(command == L"render") {
        options.command = Options::Command::render;
    } else if(command == L"text") {
        options.command = Options::Command::text;
    } else if(command == L"roundtrip") {
        options.command = Options::Command::roundtrip;
    } else if(command == L"compare") {
        options.command = Options::Command::compare;
    } else {
        return false;
    }
    options.document_path = std::filesystem::path(argv[2]);
    int first_option = 3;
    if(options.command == Options::Command::compare) {
        if(argc < 4) {
            return false;
        }
        options.comparison_directory = std::filesystem::path(argv[3]);
        first_option = 4;
    }

    for(int index = first_option; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if(argument == L"--format") {
            if(index + 1 >= argc || std::wstring_view(argv[index + 1]) != L"json") {
                return false;
            }
            ++index;
        } else if(argument == L"--output") {
            if(index + 1 >= argc) {
                return false;
            }
            options.output_directory = std::filesystem::path(argv[index + 1]);
            ++index;
        } else if(argument == L"--page") {
            if(index + 1 >= argc || !parse_page_number(argv[index + 1], options.page_number)) {
                return false;
            }
            ++index;
        } else if(argument == L"--scale") {
            if(index + 1 >= argc || !parse_positive_double(argv[index + 1], options.scale)) {
                return false;
            }
            ++index;
        } else if(argument == L"--dpr") {
            if(index + 1 >= argc || !parse_positive_double(argv[index + 1], options.device_pixel_ratio)) {
                return false;
            }
            ++index;
        } else {
            return false;
        }
    }
    if(options.command == Options::Command::inspect &&
       (options.page_number != 1 || options.scale != 1.0 || options.device_pixel_ratio != 1.0)) {
        return false;
    }
    if(options.command == Options::Command::compare &&
       (!options.output_directory.empty() || options.page_number != 1 || options.scale != 1.0 ||
        options.device_pixel_ratio != 1.0)) {
        return false;
    }
    return options.command != Options::Command::render || !options.output_directory.empty();
}

[[nodiscard]] int inspect_document(const Options& options) {
    const auto started_at = std::chrono::steady_clock::now();
    std::string document_sha256;
    std::string error_message;
    std::unique_ptr<context_reader::DocumentSession> session;
    const auto open_result = open_session(options, document_sha256, session);
    if(open_result != 0) {
        return open_result;
    }

    std::vector<context_reader::PageInfo> pages;
    pages.reserve(session->page_count());
    for(std::size_t page_index = 0; page_index < session->page_count(); ++page_index) {
        auto page_result = session->page_info(page_index);
        if(!page_result) {
            std::cerr << "reader-probe: " << page_result.error().message() << '\n';
            return exit_processing;
        }
        pages.push_back(page_result.value());
    }

    const auto document = document_json(document_sha256, pages);
    if(!options.output_directory.empty()) {
        if(!ensure_output_directory(options.output_directory)) {
            std::cerr << "reader-probe: could not create output directory\n";
            return exit_output;
        }

        if(!write_text_file(options.output_directory / "document.json", document, error_message)) {
            std::cerr << "reader-probe: " << error_message << '\n';
            return exit_output;
        }
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at
        );
        const auto run_manifest = run_manifest_json(
            document_sha256,
            "[\"inspect\", \"<document>\", \"--format\", \"json\", \"--output\", \"<output>\"]",
            duration
        );
        if(!write_text_file(
               options.output_directory / "run-manifest.json",
               run_manifest,
               error_message
           )) {
            std::cerr << "reader-probe: " << error_message << '\n';
            return exit_output;
        }
    }

    std::cout << document;
    return 0;
}

[[nodiscard]] std::string invocation_arguments(const Options& options) {
    std::ostringstream output;
    const char* command = "inspect";
    switch(options.command) {
        case Options::Command::inspect:
            command = "inspect";
            break;
        case Options::Command::render:
            command = "render";
            break;
        case Options::Command::text:
            command = "text";
            break;
        case Options::Command::roundtrip:
            command = "roundtrip";
            break;
        case Options::Command::compare:
            command = "compare";
            break;
    }
    output.imbue(std::locale::classic());
    output << std::setprecision(15) << "[\"" << command << "\", \"<document>\"";
    if(options.command != Options::Command::inspect) {
        output << ", \"--page\", \"" << options.page_number << "\"";
    }
    if(options.command == Options::Command::render || options.command == Options::Command::roundtrip) {
        output << ", \"--scale\", \"" << options.scale << "\", \"--dpr\", \""
               << options.device_pixel_ratio << "\"";
    }
    output << ", \"--format\", \"json\"";
    if(!options.output_directory.empty()) {
        output << ", \"--output\", \"<output>\"";
    }
    output << ']';
    return output.str();
}

[[nodiscard]] bool write_run_manifest(
    const Options& options,
    std::string_view document_sha256,
    std::chrono::steady_clock::time_point started_at,
    std::string& error_message
) {
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at
    );
    const auto manifest = run_manifest_json(
        document_sha256,
        invocation_arguments(options),
        duration
    );
    return write_text_file(options.output_directory / "run-manifest.json", manifest, error_message);
}

[[nodiscard]] int render_document(const Options& options) {
    const auto started_at = std::chrono::steady_clock::now();
    std::string document_sha256;
    std::unique_ptr<context_reader::DocumentSession> session;
    const auto open_result = open_session(options, document_sha256, session);
    if(open_result != 0) {
        return open_result;
    }

    const auto pixels_per_point = options.scale * options.device_pixel_ratio;
    auto image_result = session->render_page_png(options.page_number - 1, pixels_per_point);
    if(!image_result) {
        std::cerr << "reader-probe: " << image_result.error().message() << '\n';
        return exit_processing;
    }
    const auto& image = image_result.value();
    if(!ensure_output_directory(options.output_directory)) {
        std::cerr << "reader-probe: could not create output directory\n";
        return exit_output;
    }

    std::string error_message;
    const auto stem = page_stem(options.page_number);
    const auto png_name = stem + ".png";
    const auto png_path = options.output_directory / png_name;
    if(!write_binary_file(png_path, image.png, error_message)) {
        std::cerr << "reader-probe: " << error_message << '\n';
        return exit_output;
    }
    std::string png_sha256;
    if(!sha256_file(png_path, png_sha256, error_message)) {
        std::cerr << "reader-probe: " << error_message << '\n';
        return exit_output;
    }

    const auto result = render_json(
        document_sha256,
        options.page_number,
        image,
        png_sha256,
        png_name
    );
    if(!write_text_file(options.output_directory / (stem + ".render.json"), result, error_message) ||
       !write_run_manifest(options, document_sha256, started_at, error_message)) {
        std::cerr << "reader-probe: " << error_message << '\n';
        return exit_output;
    }
    std::cout << result;
    return 0;
}

[[nodiscard]] int text_document(const Options& options) {
    const auto started_at = std::chrono::steady_clock::now();
    std::string document_sha256;
    std::unique_ptr<context_reader::DocumentSession> session;
    const auto open_result = open_session(options, document_sha256, session);
    if(open_result != 0) {
        return open_result;
    }

    auto text_result = session->extract_text(options.page_number - 1);
    if(!text_result) {
        std::cerr << "reader-probe: " << text_result.error().message() << '\n';
        return exit_processing;
    }
    const auto result = text_page_json(document_sha256, options.page_number, text_result.value());
    if(!options.output_directory.empty()) {
        if(!ensure_output_directory(options.output_directory)) {
            std::cerr << "reader-probe: could not create output directory\n";
            return exit_output;
        }
        std::string error_message;
        const auto stem = page_stem(options.page_number);
        if(!write_text_file(
               options.output_directory / (stem + ".layout.json"),
               result,
               error_message
           ) ||
           !write_text_file(
               options.output_directory / (stem + ".text.txt"),
               text_result.value().text,
               error_message
           ) ||
           !write_run_manifest(options, document_sha256, started_at, error_message)) {
            std::cerr << "reader-probe: " << error_message << '\n';
            return exit_output;
        }
    }
    std::cout << result;
    return 0;
}

struct RoundtripArtifacts final {
    std::string json;
    std::string jsonl;
    double maximum_error = 0.0;
};

[[nodiscard]] RoundtripArtifacts roundtrip_artifacts(
    std::string_view document_sha256,
    std::size_t page_number,
    const context_reader::PageInfo& page,
    const context_reader::PageTransform& transform,
    double scale,
    double device_pixel_ratio
) {
    const std::array<context_reader::PagePoint, 5> samples{
        context_reader::PagePoint{0.0, 0.0},
        context_reader::PagePoint{page.size.width, 0.0},
        context_reader::PagePoint{page.size.width, page.size.height},
        context_reader::PagePoint{0.0, page.size.height},
        context_reader::PagePoint{page.size.width / 2.0, page.size.height / 2.0},
    };

    RoundtripArtifacts result;
    std::ostringstream points;
    std::ostringstream jsonl;
    points.imbue(std::locale::classic());
    jsonl.imbue(std::locale::classic());
    points << std::setprecision(15);
    jsonl << std::setprecision(15);
    for(std::size_t index = 0; index < samples.size(); ++index) {
        const auto source = samples[index];
        const auto device = transform.to_device(source);
        const auto recovered = transform.to_page(device);
        const auto error = std::hypot(recovered.x - source.x, recovered.y - source.y);
        result.maximum_error = std::max(result.maximum_error, error);
        std::ostringstream record;
        record.imbue(std::locale::classic());
        record << std::setprecision(15)
               << "{\"sample\": " << index << ", \"page\": {\"x\": " << source.x
               << ", \"y\": " << source.y << "}, \"device\": {\"x\": " << device.x
               << ", \"y\": " << device.y << "}, \"recovered_page\": {\"x\": "
               << recovered.x << ", \"y\": " << recovered.y << "}, \"error_points\": "
               << error << '}';
        points << (index == 0 ? "\n    " : ",\n    ") << record.str();
        jsonl << record.str() << '\n';
    }
    points << '\n';

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(15)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"document_sha256\": \"" << document_sha256 << "\",\n"
           << "  \"page\": " << page_number << ",\n"
           << "  \"scale\": " << scale << ",\n"
           << "  \"device_pixel_ratio\": " << device_pixel_ratio << ",\n"
           << "  \"tolerance_points\": 1e-9,\n"
           << "  \"maximum_error_points\": " << result.maximum_error << ",\n"
           << "  \"passed\": " << (result.maximum_error <= 1.0e-9 ? "true" : "false") << ",\n"
           << "  \"samples\": [" << points.str() << "  ]\n"
           << "}\n";
    result.json = output.str();
    result.jsonl = jsonl.str();
    return result;
}

[[nodiscard]] int roundtrip_document(const Options& options) {
    const auto started_at = std::chrono::steady_clock::now();
    std::string document_sha256;
    std::unique_ptr<context_reader::DocumentSession> session;
    const auto open_result = open_session(options, document_sha256, session);
    if(open_result != 0) {
        return open_result;
    }
    auto page_result = session->page_info(options.page_number - 1);
    if(!page_result) {
        std::cerr << "reader-probe: " << page_result.error().message() << '\n';
        return exit_processing;
    }
    auto transform_result = context_reader::PageTransform::create(
        page_result.value().size,
        context_reader::Scale{options.scale},
        context_reader::DevicePixelRatio{options.device_pixel_ratio},
        page_result.value().rotation
    );
    if(!transform_result) {
        std::cerr << "reader-probe: " << transform_result.error().message() << '\n';
        return exit_processing;
    }
    const auto artifacts = roundtrip_artifacts(
        document_sha256,
        options.page_number,
        page_result.value(),
        transform_result.value(),
        options.scale,
        options.device_pixel_ratio
    );
    if(!options.output_directory.empty()) {
        if(!ensure_output_directory(options.output_directory)) {
            std::cerr << "reader-probe: could not create output directory\n";
            return exit_output;
        }
        std::string error_message;
        if(!write_text_file(
               options.output_directory / "roundtrip.json",
               artifacts.json,
               error_message
           ) ||
           !write_text_file(
               options.output_directory / "coordinates.jsonl",
               artifacts.jsonl,
               error_message
           ) ||
           !write_run_manifest(options, document_sha256, started_at, error_message)) {
            std::cerr << "reader-probe: " << error_message << '\n';
            return exit_output;
        }
    }
    std::cout << artifacts.json;
    return artifacts.maximum_error <= 1.0e-9 ? 0 : exit_processing;
}

using ArtifactHashes = std::map<std::string, std::string>;

[[nodiscard]] std::string generic_utf8_path(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] bool collect_artifact_hashes(
    const std::filesystem::path& root,
    ArtifactHashes& hashes,
    std::string& error_message
) {
    std::error_code filesystem_error;
    if(!std::filesystem::is_directory(root, filesystem_error) || filesystem_error) {
        error_message = "comparison input is not a readable directory";
        return false;
    }

    std::filesystem::recursive_directory_iterator iterator(root, filesystem_error);
    const std::filesystem::recursive_directory_iterator end;
    while(!filesystem_error && iterator != end) {
        if(iterator->is_regular_file(filesystem_error) && !filesystem_error &&
           iterator->path().filename() != "run-manifest.json") {
            const auto relative = std::filesystem::relative(iterator->path(), root, filesystem_error);
            if(filesystem_error) {
                break;
            }
            std::string hash;
            if(!sha256_file(iterator->path(), hash, error_message)) {
                return false;
            }
            hashes.emplace(generic_utf8_path(relative), std::move(hash));
        }
        iterator.increment(filesystem_error);
    }
    if(filesystem_error) {
        error_message = "could not enumerate comparison directory";
        return false;
    }
    return true;
}

[[nodiscard]] int compare_directories(const Options& options) {
    ArtifactHashes actual;
    ArtifactHashes baseline;
    std::string error_message;
    if(!collect_artifact_hashes(options.document_path, actual, error_message) ||
       !collect_artifact_hashes(options.comparison_directory, baseline, error_message)) {
        std::cerr << "reader-probe: " << error_message << '\n';
        return exit_input;
    }

    std::vector<std::string> added;
    std::vector<std::string> removed;
    struct ChangedArtifact final {
        std::string path;
        std::string actual_sha256;
        std::string baseline_sha256;
    };
    std::vector<ChangedArtifact> changed;
    std::size_t unchanged = 0;

    for(const auto& [path, actual_hash] : actual) {
        const auto baseline_entry = baseline.find(path);
        if(baseline_entry == baseline.end()) {
            added.push_back(path);
        } else if(actual_hash != baseline_entry->second) {
            changed.push_back(ChangedArtifact{path, actual_hash, baseline_entry->second});
        } else {
            ++unchanged;
        }
    }
    for(const auto& [path, baseline_hash] : baseline) {
        static_cast<void>(baseline_hash);
        if(!actual.contains(path)) {
            removed.push_back(path);
        }
    }

    const bool passed = added.empty() && removed.empty() && changed.empty();
    auto write_paths = [](std::ostringstream& output, const std::vector<std::string>& paths) {
        output << '[';
        for(std::size_t index = 0; index < paths.size(); ++index) {
            output << (index == 0 ? "" : ", ") << json_string(paths[index]);
        }
        output << ']';
    };

    std::ostringstream output;
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"status\": \"" << (passed ? "passed" : "failed") << "\",\n"
           << "  \"unchanged_count\": " << unchanged << ",\n"
           << "  \"added\": ";
    write_paths(output, added);
    output << ",\n  \"removed\": ";
    write_paths(output, removed);
    output << ",\n  \"changed\": [";
    for(std::size_t index = 0; index < changed.size(); ++index) {
        const auto& artifact = changed[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"path\": " << json_string(artifact.path)
               << ", \"actual_sha256\": \"" << artifact.actual_sha256
               << "\", \"baseline_sha256\": \"" << artifact.baseline_sha256 << "\"}";
    }
    if(!changed.empty()) {
        output << '\n';
    }
    output << "  ],\n"
           << "  \"ignored\": [\"run-manifest.json\"]\n"
           << "}\n";
    std::cout << output.str();
    return passed ? 0 : exit_validation;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if(argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        print_help();
        return 0;
    }
    if(argc == 2 && std::wstring_view(argv[1]) == L"--version") {
        std::cout << "reader-probe " << tool_version << " (Kernel 0.1.0, MuPDF "
                  << context_reader::MuPdfEngine::version() << ")\n";
        return 0;
    }

    Options options;
    if(!parse_options(argc, argv, options)) {
        print_help();
        return exit_usage;
    }
    switch(options.command) {
        case Options::Command::inspect:
            return inspect_document(options);
        case Options::Command::render:
            return render_document(options);
        case Options::Command::text:
            return text_document(options);
        case Options::Command::roundtrip:
            return roundtrip_document(options);
        case Options::Command::compare:
            return compare_directories(options);
    }
    return exit_usage;
}
