#include "context_reader/workspace/sqlite_workspace.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <sqlite3.h>
#include <miniz.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "context_reader/annotation/annotation.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/shared/result.hpp"

namespace context_reader {

namespace {

constexpr std::uint32_t current_schema_version = 4;

class WorkspaceLock final {
public:
    WorkspaceLock() = default;
    explicit WorkspaceLock(HANDLE handle) noexcept : handle_(handle) {}
    WorkspaceLock(const WorkspaceLock&) = delete;
    WorkspaceLock& operator=(const WorkspaceLock&) = delete;
    WorkspaceLock(WorkspaceLock&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    WorkspaceLock& operator=(WorkspaceLock&& other) noexcept {
        if(this != &other) {
            close();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~WorkspaceLock() { close(); }

private:
    void close() noexcept {
        if(handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

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

class Statement final {
public:
    Statement() = default;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Statement& operator=(Statement&& other) noexcept {
        if(this != &other) {
            if(value_ != nullptr) {
                sqlite3_finalize(value_);
            }
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    ~Statement() {
        if(value_ != nullptr) {
            sqlite3_finalize(value_);
        }
    }

    [[nodiscard]] sqlite3_stmt** address() noexcept { return &value_; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return value_; }

private:
    sqlite3_stmt* value_ = nullptr;
};

class Transaction final {
public:
    explicit Transaction(sqlite3* database) noexcept : database_(database) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if(active_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void begin() noexcept { active_ = true; }
    void commit() noexcept { active_ = false; }

private:
    sqlite3* database_;
    bool active_ = false;
};

[[nodiscard]] bool nt_success(NTSTATUS status) noexcept {
    return status >= 0;
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] std::string generic_utf8_path(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] Result<WorkspaceLock> acquire_workspace_lock(
    const std::filesystem::path& root
) {
    const auto lock_path = root / "workspace.lock";
    const auto handle = CreateFileW(
        lock_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if(handle != INVALID_HANDLE_VALUE) {
        return Result<WorkspaceLock>::success(WorkspaceLock(handle));
    }
    const auto error = GetLastError();
    if(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
        return Result<WorkspaceLock>::failure(
            Error(ErrorCode::workspace_busy, "Workspace is already open for writing")
        );
    }
    return Result<WorkspaceLock>::failure(
        Error(ErrorCode::storage_failure, "Workspace lock could not be acquired")
    );
}

[[nodiscard]] std::string hex_string(const std::array<std::uint8_t, 32>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for(const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

[[nodiscard]] Result<std::string> sha256_file(
    const std::filesystem::path& path,
    const CancellationToken& cancellation = CancellationToken{}
) {
    AlgorithmHandle algorithm;
    if(!nt_success(BCryptOpenAlgorithmProvider(
           algorithm.address(),
           BCRYPT_SHA256_ALGORITHM,
           nullptr,
           0
       ))) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 provider initialization failed")
        );
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
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 provider query failed")
        );
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
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 state creation failed")
        );
    }

    std::ifstream input(path, std::ios::binary);
    if(!input) {
        return Result<std::string>::failure(
            Error(ErrorCode::not_found, "source file could not be opened")
        );
    }
    std::array<char, 64 * 1024> buffer{};
    while(input) {
        if(cancellation.is_cancellation_requested()) {
            return Result<std::string>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = input.gcount();
        if(bytes_read > 0 && !nt_success(BCryptHashData(
                               hash.get(),
                               reinterpret_cast<PUCHAR>(buffer.data()),
                               static_cast<ULONG>(bytes_read),
                               0
                           ))) {
            return Result<std::string>::failure(
                Error(ErrorCode::storage_failure, "SHA-256 update failed")
            );
        }
    }
    if(!input.eof()) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "source file could not be read")
        );
    }

    std::array<std::uint8_t, 32> bytes{};
    if(!nt_success(BCryptFinishHash(
           hash.get(),
           bytes.data(),
           static_cast<ULONG>(bytes.size()),
           0
       ))) {
        return Result<std::string>::failure(
            Error(ErrorCode::storage_failure, "SHA-256 finalization failed")
        );
    }
    return Result<std::string>::success(hex_string(bytes));
}

struct ImageMetadata final {
    std::string media_type;
    std::string extension;
    std::uint32_t width;
    std::uint32_t height;
};

[[nodiscard]] Result<ImageMetadata> inspect_image(const std::filesystem::path& path) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if(FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return Result<ImageMetadata>::failure(
            Error(ErrorCode::storage_failure, "Windows Imaging Component could not be initialized")
        );
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    const auto release = [&] {
        if(frame != nullptr) frame->Release();
        if(decoder != nullptr) decoder->Release();
        if(factory != nullptr) factory->Release();
        if(uninitialize) CoUninitialize();
    };
    if(FAILED(CoCreateInstance(
           CLSID_WICImagingFactory,
           nullptr,
           CLSCTX_INPROC_SERVER,
           IID_PPV_ARGS(&factory)
       )) || FAILED(factory->CreateDecoderFromFilename(
           path.c_str(),
           nullptr,
           GENERIC_READ,
           WICDecodeMetadataCacheOnLoad,
           &decoder
       ))) {
        release();
        return Result<ImageMetadata>::failure(
            Error(ErrorCode::invalid_argument, "Asset is not a decodable PNG or JPEG image")
        );
    }
    GUID container{};
    UINT width = 0;
    UINT height = 0;
    if(FAILED(decoder->GetContainerFormat(&container))
       || FAILED(decoder->GetFrame(0, &frame))
       || FAILED(frame->GetSize(&width, &height))) {
        release();
        return Result<ImageMetadata>::failure(
            Error(ErrorCode::invalid_argument, "Asset image metadata could not be read")
        );
    }
    std::string media_type;
    std::string extension;
    if(IsEqualGUID(container, GUID_ContainerFormatPng)) {
        media_type = "image/png";
        extension = ".png";
    } else if(IsEqualGUID(container, GUID_ContainerFormatJpeg)) {
        media_type = "image/jpeg";
        extension = ".jpg";
    } else {
        release();
        return Result<ImageMetadata>::failure(
            Error(ErrorCode::invalid_argument, "Only PNG and JPEG note assets are supported")
        );
    }
    if(width == 0 || height == 0 || width > 8192 || height > 8192
       || static_cast<std::uint64_t>(width) * height > 40'000'000ULL) {
        release();
        return Result<ImageMetadata>::failure(
            Error(ErrorCode::invalid_argument, "Asset image dimensions exceed the supported limits")
        );
    }
    release();
    return Result<ImageMetadata>::success(ImageMetadata{
        .media_type = std::move(media_type),
        .extension = std::move(extension),
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
    });
}

[[nodiscard]] Result<void> copy_file_cancellable(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const CancellationToken& cancellation
) {
    auto partial = destination;
    partial += ".partial";
    std::error_code filesystem_error;
    std::filesystem::remove(partial, filesystem_error);

    std::ifstream input(source, std::ios::binary);
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if(!input || !output) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "PDF object could not be stored")
        );
    }

    std::array<char, 64 * 1024> buffer{};
    while(input) {
        if(cancellation.is_cancellation_requested()) {
            input.close();
            output.close();
            std::filesystem::remove(partial, filesystem_error);
            return Result<void>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = input.gcount();
        if(bytes_read > 0) {
            output.write(buffer.data(), bytes_read);
        }
    }
    output.close();
    if(!input.eof() || !output) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "PDF object could not be stored")
        );
    }
    if(cancellation.is_cancellation_requested()) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::cancelled, "Document import was cancelled")
        );
    }

    std::filesystem::rename(partial, destination, filesystem_error);
    if(filesystem_error) {
        std::filesystem::remove(partial, filesystem_error);
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "PDF object could not be stored")
        );
    }
    return Result<void>::success();
}

[[nodiscard]] bool fault_point_configured(const char* variable, std::string_view point) {
    std::array<char, 64> configured{};
    const auto length = GetEnvironmentVariableA(
        variable,
        configured.data(),
        static_cast<DWORD>(configured.size())
    );
    return length > 0 && length < configured.size()
        && std::string_view(configured.data(), length) == point;
}

void terminate_at_import_fault_point(std::string_view point) {
    if(fault_point_configured("CONTEXT_READER_TEST_IMPORT_FAULT", point)) {
        std::_Exit(86);
    }
}

[[nodiscard]] Result<void> fail_at_fault_point(
    const char* variable,
    std::string_view point,
    std::string_view message
) {
    if(fault_point_configured(variable, point)) {
        return Result<void>::failure(Error(ErrorCode::storage_failure, std::string(message)));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> execute(sqlite3* database, const char* sql) {
    if(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, std::string("SQLite command failed: ") + sqlite3_errmsg(database))
        );
    }
    return Result<void>::success();
}

[[nodiscard]] Result<Statement> prepare(sqlite3* database, const char* sql) {
    Statement statement;
    if(sqlite3_prepare_v2(database, sql, -1, statement.address(), nullptr) != SQLITE_OK) {
        return Result<Statement>::failure(
            Error(ErrorCode::storage_failure, std::string("SQLite statement preparation failed: ") + sqlite3_errmsg(database))
        );
    }
    return Result<Statement>::success(std::move(statement));
}

[[nodiscard]] bool bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
    return sqlite3_bind_text64(
               statement,
               index,
               value.data(),
               static_cast<sqlite3_uint64>(value.size()),
               SQLITE_TRANSIENT,
               SQLITE_UTF8
           ) == SQLITE_OK;
}

template <typename Id>
[[nodiscard]] bool bind_id(sqlite3_stmt* statement, int index, const Id& id) {
    return sqlite3_bind_blob(
               statement,
               index,
               id.bytes().data(),
               static_cast<int>(id.bytes().size()),
               SQLITE_TRANSIENT
           ) == SQLITE_OK;
}

template <typename Id>
[[nodiscard]] Result<Id> read_id(sqlite3_stmt* statement, int column) {
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, column));
    const auto size = sqlite3_column_bytes(statement, column);
    if(data == nullptr || size != 16) {
        return Result<Id>::failure(
            Error(ErrorCode::storage_failure, "SQLite stable ID is invalid")
        );
    }
    typename Id::Bytes bytes{};
    std::copy_n(data, bytes.size(), bytes.begin());
    return Result<Id>::success(Id::from_bytes(bytes));
}

[[nodiscard]] Result<std::pair<DocumentId, DocumentVersionId>> create_id_pair(sqlite3* database) {
    auto statement_result = prepare(database, "SELECT randomblob(16), randomblob(16)");
    if(!statement_result) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(
            Error(ErrorCode::storage_failure, "SQLite ID generation failed")
        );
    }
    auto document_id = read_id<DocumentId>(statement.get(), 0);
    auto version_id = read_id<DocumentVersionId>(statement.get(), 1);
    if(!document_id) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(document_id.error());
    }
    if(!version_id) {
        return Result<std::pair<DocumentId, DocumentVersionId>>::failure(version_id.error());
    }
    return Result<std::pair<DocumentId, DocumentVersionId>>::success(
        std::make_pair(document_id.value(), version_id.value())
    );
}

template <typename Id>
[[nodiscard]] Result<Id> create_id(sqlite3* database) {
    auto statement_result = prepare(database, "SELECT randomblob(16)");
    if(!statement_result) {
        return Result<Id>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<Id>::failure(Error(ErrorCode::storage_failure, "SQLite ID generation failed"));
    }
    return read_id<Id>(statement.get(), 0);
}

[[nodiscard]] const char* color_name(HighlightColor color) noexcept {
    switch(color) {
        case HighlightColor::yellow: return "yellow";
        case HighlightColor::green: return "green";
        case HighlightColor::blue: return "blue";
        case HighlightColor::pink: return "pink";
    }
    return "yellow";
}

[[nodiscard]] Result<HighlightColor> read_color(std::string_view color) {
    if(color == "yellow") return Result<HighlightColor>::success(HighlightColor::yellow);
    if(color == "green") return Result<HighlightColor>::success(HighlightColor::green);
    if(color == "blue") return Result<HighlightColor>::success(HighlightColor::blue);
    if(color == "pink") return Result<HighlightColor>::success(HighlightColor::pink);
    return Result<HighlightColor>::failure(
        Error(ErrorCode::storage_failure, "SQLite annotation color is invalid")
    );
}

[[nodiscard]] Result<DocumentRecord> read_document_record(sqlite3_stmt* statement) {
    auto document_id = read_id<DocumentId>(statement, 0);
    auto version_id = read_id<DocumentVersionId>(statement, 1);
    if(!document_id) {
        return Result<DocumentRecord>::failure(document_id.error());
    }
    if(!version_id) {
        return Result<DocumentRecord>::failure(version_id.error());
    }

    const auto byte_length = sqlite3_column_int64(statement, 6);
    const auto page_count = sqlite3_column_int64(statement, 7);
    if(byte_length < 0 || page_count < 0) {
        return Result<DocumentRecord>::failure(
            Error(ErrorCode::storage_failure, "SQLite document metadata is invalid")
        );
    }

    const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
    const auto* content_hash = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
    const auto* object_key = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
    if(title == nullptr || content_hash == nullptr || object_key == nullptr) {
        return Result<DocumentRecord>::failure(
            Error(ErrorCode::storage_failure, "SQLite document text metadata is invalid")
        );
    }

    return Result<DocumentRecord>::success(DocumentRecord{
        .document_id = document_id.value(),
        .version_id = version_id.value(),
        .title = title,
        .content_sha256 = content_hash,
        .object_key = object_key,
        .byte_length = static_cast<std::uint64_t>(byte_length),
        .page_count = static_cast<std::size_t>(page_count),
    });
}

[[nodiscard]] Result<WorkspaceInfo> read_workspace_info(sqlite3* database) {
    auto statement_result = prepare(
        database,
        "SELECT workspace_id, schema_version FROM workspace_metadata WHERE singleton = 1"
    );
    if(!statement_result) {
        return Result<WorkspaceInfo>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<WorkspaceInfo>::failure(
            Error(ErrorCode::storage_failure, "Workspace metadata is missing")
        );
    }
    auto workspace_id = read_id<WorkspaceId>(statement.get(), 0);
    const auto schema_version = sqlite3_column_int64(statement.get(), 1);
    if(!workspace_id) {
        return Result<WorkspaceInfo>::failure(workspace_id.error());
    }
    if(schema_version != current_schema_version) {
        return Result<WorkspaceInfo>::failure(
            Error(ErrorCode::unsupported_document, "Workspace schema version is unsupported")
        );
    }
    return Result<WorkspaceInfo>::success(WorkspaceInfo{
        .id = workspace_id.value(),
        .schema_version = current_schema_version,
    });
}

[[nodiscard]] Result<void> configure_database(sqlite3* database) {
    if(sqlite3_busy_timeout(database, 5000) != SQLITE_OK) {
        return Result<void>::failure(
            Error(ErrorCode::storage_failure, "SQLite busy timeout configuration failed")
        );
    }
    for(const auto* command : {
            "PRAGMA foreign_keys = ON",
            "PRAGMA journal_mode = WAL",
            "PRAGMA synchronous = FULL",
        }) {
        auto result = execute(database, command);
        if(!result) {
            return result;
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> attach_search_index(
    sqlite3* database,
    const std::filesystem::path& index_path
) {
    std::error_code filesystem_error;
    if(!std::filesystem::exists(index_path, filesystem_error)) {
        std::ofstream index_file(index_path, std::ios::binary | std::ios::app);
        index_file.close();
        if(filesystem_error || !index_file) {
            return Result<void>::failure(
                Error(ErrorCode::storage_failure, "Search index database could not be created")
            );
        }
    }
    auto attach = prepare(database, "ATTACH DATABASE ?1 AS search_index");
    const auto path = utf8_path(index_path);
    if(!attach || !bind_text(attach.value().get(), 1, path)
       || sqlite3_step(attach.value().get()) != SQLITE_DONE) {
        return Result<void>::failure(
            Error(
                ErrorCode::storage_failure,
                std::string("Search index database could not be attached: ") + sqlite3_errmsg(database)
            )
        );
    }
    constexpr const char* schema = R"sql(
CREATE TABLE IF NOT EXISTS search_index.metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    version INTEGER NOT NULL,
    status TEXT NOT NULL
) STRICT;
INSERT OR IGNORE INTO search_index.metadata(singleton, version, status) VALUES(1, 1, 'not_built');
CREATE VIRTUAL TABLE IF NOT EXISTS search_index.entries USING fts5(
    kind UNINDEXED,
    document_version_id UNINDEXED,
    note_id UNINDEXED,
    page_index UNINDEXED,
    title,
    content,
    tokenize='trigram'
);
)sql";
    return execute(database, schema);
}

[[nodiscard]] Result<std::size_t> query_count(sqlite3* database, const char* sql);

[[nodiscard]] Result<void> apply_initial_migration(sqlite3* database) {
    constexpr const char* migration = R"sql(
BEGIN IMMEDIATE;
CREATE TABLE workspace_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    workspace_id BLOB NOT NULL CHECK (length(workspace_id) = 16),
    schema_version INTEGER NOT NULL,
    created_at INTEGER NOT NULL
) STRICT;
CREATE TABLE documents (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    title TEXT NOT NULL,
    active_version_id BLOB,
    created_at INTEGER NOT NULL,
    FOREIGN KEY (active_version_id) REFERENCES document_versions(id)
) STRICT;
CREATE TABLE document_versions (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    document_id BLOB NOT NULL REFERENCES documents(id),
    content_hash TEXT NOT NULL UNIQUE CHECK (length(content_hash) = 64),
    object_key TEXT NOT NULL UNIQUE,
    byte_length INTEGER NOT NULL CHECK (byte_length >= 0),
    page_count INTEGER NOT NULL CHECK (page_count >= 0),
    created_at INTEGER NOT NULL
) STRICT;
CREATE INDEX document_versions_document_id_idx ON document_versions(document_id);
CREATE TABLE annotations (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    document_version_id BLOB NOT NULL REFERENCES document_versions(id) ON DELETE CASCADE,
    page_index INTEGER NOT NULL CHECK (page_index >= 0),
    quote_exact TEXT NOT NULL,
    quote_prefix TEXT NOT NULL,
    quote_suffix TEXT NOT NULL,
    layout_version TEXT NOT NULL,
    anchor_version INTEGER NOT NULL CHECK (anchor_version IN (1, 2)),
    text_start INTEGER CHECK (text_start IS NULL OR text_start >= 0),
    text_end INTEGER CHECK (text_end IS NULL OR text_end >= text_start),
    direction TEXT NOT NULL CHECK (direction IN ('ltr', 'rtl', 'ttb')),
    color TEXT NOT NULL CHECK (color IN ('yellow', 'green', 'blue', 'pink')),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
) STRICT;
CREATE INDEX annotations_version_page_idx ON annotations(document_version_id, page_index);
CREATE TABLE annotation_quads (
    annotation_id BLOB NOT NULL REFERENCES annotations(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0),
    x REAL NOT NULL,
    y REAL NOT NULL,
    width REAL NOT NULL CHECK (width > 0),
    height REAL NOT NULL CHECK (height > 0),
    PRIMARY KEY(annotation_id, ordinal)
) STRICT;
CREATE TABLE notes (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    annotation_id BLOB NOT NULL UNIQUE REFERENCES annotations(id) ON DELETE CASCADE,
    markdown_source TEXT NOT NULL,
    revision INTEGER NOT NULL CHECK (revision > 0),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
) STRICT;
CREATE TABLE assets (
    id BLOB PRIMARY KEY CHECK (length(id) = 16),
    content_hash TEXT NOT NULL UNIQUE CHECK (length(content_hash) = 64),
    object_key TEXT NOT NULL UNIQUE,
    media_type TEXT NOT NULL CHECK (media_type IN ('image/png', 'image/jpeg')),
    byte_length INTEGER NOT NULL CHECK (byte_length >= 0),
    width INTEGER NOT NULL CHECK (width > 0),
    height INTEGER NOT NULL CHECK (height > 0),
    created_at INTEGER NOT NULL
) STRICT;
CREATE TABLE annotation_assets (
    annotation_id BLOB NOT NULL REFERENCES annotations(id) ON DELETE CASCADE,
    asset_id BLOB NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    PRIMARY KEY(annotation_id, asset_id)
) STRICT;
INSERT INTO workspace_metadata(singleton, workspace_id, schema_version, created_at)
VALUES (1, randomblob(16), 4, unixepoch());
PRAGMA user_version = 4;
COMMIT;
)sql";

    auto result = execute(database, migration);
    if(!result) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    return result;
}

[[nodiscard]] Result<void> migrate_database(sqlite3* database) {
    auto version_result = query_count(database, "PRAGMA user_version");
    if(!version_result) {
        return Result<void>::failure(version_result.error());
    }
    if(version_result.value() == current_schema_version) {
        return Result<void>::success();
    }
    return Result<void>::failure(
        Error(ErrorCode::unsupported_document, "Workspace schema version is unsupported in this development build")
    );
}

[[nodiscard]] Result<std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>> open_database(
    const std::filesystem::path& database_path,
    bool create
) {
    sqlite3* raw_database = nullptr;
    const auto path = utf8_path(database_path);
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX |
                      (create ? SQLITE_OPEN_CREATE : 0);
    if(sqlite3_open_v2(path.c_str(), &raw_database, flags, nullptr) != SQLITE_OK) {
        if(raw_database != nullptr) {
            sqlite3_close_v2(raw_database);
        }
        return Result<std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>>::failure(
            Error(ErrorCode::storage_failure, "Workspace database could not be opened")
        );
    }
    return Result<std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>>::success(
        std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>(raw_database, sqlite3_close_v2)
    );
}

[[nodiscard]] Result<std::size_t> query_count(sqlite3* database, const char* sql) {
    auto statement_result = prepare(database, sql);
    if(!statement_result) {
        return Result<std::size_t>::failure(statement_result.error());
    }
    auto statement = std::move(statement_result).value();
    if(sqlite3_step(statement.get()) != SQLITE_ROW) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::storage_failure, "SQLite count query failed")
        );
    }
    const auto value = sqlite3_column_int64(statement.get(), 0);
    if(value < 0) {
        return Result<std::size_t>::failure(
            Error(ErrorCode::storage_failure, "SQLite count is invalid")
        );
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(value));
}

struct PackageManifestEntry final {
    std::string path;
    std::string sha256;
    std::uint64_t size;
};

struct ParsedPackageManifest final {
    std::uint32_t version;
    std::vector<PackageManifestEntry> entries;
};

[[nodiscard]] bool safe_package_path(std::string_view value) {
    if(value.empty() || value.front() == '/' || value.find('\\') != std::string_view::npos
       || value.find(':') != std::string_view::npos) return false;
    const std::filesystem::path path(value);
    if(path.is_absolute() || generic_utf8_path(path.lexically_normal()) != value) return false;
    for(const auto& part : path) {
        if(part == "." || part == "..") return false;
    }
    return value == "workspace.db" || value.starts_with("objects/pdf/")
        || value.starts_with("objects/assets/");
}

[[nodiscard]] Result<ParsedPackageManifest> parse_package_manifest(std::string_view json) {
    if(json.empty() || json.size() > 1024U * 1024U) {
        return Result<ParsedPackageManifest>::failure(Error(ErrorCode::invalid_argument, "Package manifest size is invalid"));
    }
    sqlite3* raw = nullptr;
    if(sqlite3_open(":memory:", &raw) != SQLITE_OK) {
        if(raw != nullptr) sqlite3_close(raw);
        return Result<ParsedPackageManifest>::failure(Error(ErrorCode::resource_exhausted, "Manifest parser could not be created"));
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(raw, sqlite3_close_v2);
    auto header = prepare(
        database.get(),
        "SELECT json_extract(?1, '$.format'), json_extract(?1, '$.version'), json_type(?1, '$.entries') "
        "WHERE json_valid(?1)"
    );
    if(!header || !bind_text(header.value().get(), 1, json)
       || sqlite3_step(header.value().get()) != SQLITE_ROW) {
        return Result<ParsedPackageManifest>::failure(Error(ErrorCode::invalid_argument, "Package manifest is not valid JSON"));
    }
    const auto* format = reinterpret_cast<const char*>(sqlite3_column_text(header.value().get(), 0));
    const auto version = sqlite3_column_int64(header.value().get(), 1);
    const auto* entries_type = reinterpret_cast<const char*>(sqlite3_column_text(header.value().get(), 2));
    if(format == nullptr || std::string_view(format) != "readerpkg" || version != 1
       || entries_type == nullptr || std::string_view(entries_type) != "array") {
        return Result<ParsedPackageManifest>::failure(Error(ErrorCode::invalid_argument, "Package manifest format is unsupported"));
    }
    auto rows = prepare(
        database.get(),
        "SELECT json_extract(value, '$.path'), json_extract(value, '$.sha256'), "
        "json_extract(value, '$.size') FROM json_each(?1, '$.entries')"
    );
    if(!rows || !bind_text(rows.value().get(), 1, json)) {
        return Result<ParsedPackageManifest>::failure(Error(ErrorCode::invalid_argument, "Package manifest entries are invalid"));
    }
    ParsedPackageManifest manifest{.version = 1};
    std::unordered_set<std::string> paths;
    int step = SQLITE_ROW;
    while((step = sqlite3_step(rows.value().get())) == SQLITE_ROW) {
        const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(rows.value().get(), 0));
        const auto* sha = reinterpret_cast<const char*>(sqlite3_column_text(rows.value().get(), 1));
        const auto size = sqlite3_column_int64(rows.value().get(), 2);
        if(path == nullptr || sha == nullptr || size < 0 || std::string_view(sha).size() != 64
           || !safe_package_path(path) || !paths.insert(path).second) {
            return Result<ParsedPackageManifest>::failure(Error(ErrorCode::invalid_argument, "Package manifest entry is invalid"));
        }
        manifest.entries.push_back(PackageManifestEntry{
            .path = path,
            .sha256 = sha,
            .size = static_cast<std::uint64_t>(size),
        });
    }
    if(step != SQLITE_DONE || manifest.entries.empty()) {
        return Result<ParsedPackageManifest>::failure(Error(ErrorCode::invalid_argument, "Package manifest entries are invalid"));
    }
    return Result<ParsedPackageManifest>::success(std::move(manifest));
}

[[nodiscard]] Result<BackupInspection> validate_and_extract_package(
    const std::filesystem::path& package_path,
    const std::filesystem::path& extraction_root
) {
    if(!std::filesystem::is_regular_file(package_path)) {
        return Result<BackupInspection>::failure(Error(ErrorCode::not_found, "Backup package was not found"));
    }
    mz_zip_archive archive{};
    const auto package_utf8 = utf8_path(package_path);
    if(!mz_zip_reader_init_file(&archive, package_utf8.c_str(), 0)) {
        return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup is not a readable ZIP archive"));
    }
    const auto close_archive = [&] { mz_zip_reader_end(&archive); };
    if(!mz_zip_is_zip64(&archive)) {
        close_archive();
        return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup must use the ZIP64 archive format"));
    }
    // Each declared entry is fully inflated below and checked against its manifest
    // size and SHA-256. miniz 3.1.2's aggregate validator rejects small archives
    // written with forced ZIP64 even though its reader and external ZIP readers
    // accept the same archive.
    const auto file_count = mz_zip_reader_get_num_files(&archive);
    std::unordered_map<std::string, mz_uint> archive_entries;
    std::uint64_t total_size = 0;
    mz_uint manifest_index = 0;
    bool manifest_found = false;
    for(mz_uint index = 0; index < file_count; ++index) {
        mz_zip_archive_file_stat stat{};
        if(!mz_zip_reader_file_stat(&archive, index, &stat)) {
            close_archive();
            return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup entry metadata is invalid"));
        }
        const std::string name(stat.m_filename);
        const auto unix_type = (stat.m_external_attr >> 16U) & 0170000U;
        if(name.empty() || stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported
           || unix_type == 0120000U || name.find('\\') != std::string::npos
           || !archive_entries.emplace(name, index).second
           || stat.m_uncomp_size > 100ULL * 1024ULL * 1024ULL * 1024ULL
           || total_size > 100ULL * 1024ULL * 1024ULL * 1024ULL - stat.m_uncomp_size) {
            close_archive();
            return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup contains an unsafe entry"));
        }
        total_size += stat.m_uncomp_size;
        if(name == "manifest.json") {
            manifest_found = true;
            manifest_index = index;
        }
    }
    if(!manifest_found) {
        close_archive();
        return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup manifest is missing"));
    }
    size_t manifest_size = 0;
    void* manifest_data = mz_zip_reader_extract_to_heap(&archive, manifest_index, &manifest_size, 0);
    if(manifest_data == nullptr) {
        close_archive();
        return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup manifest could not be read"));
    }
    const std::string manifest_json(static_cast<const char*>(manifest_data), manifest_size);
    mz_free(manifest_data);
    auto manifest = parse_package_manifest(manifest_json);
    if(!manifest || manifest.value().entries.size() + 1U != archive_entries.size()) {
        close_archive();
        return Result<BackupInspection>::failure(
            manifest ? Error(ErrorCode::invalid_argument, "Backup contains undeclared files") : manifest.error()
        );
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(extraction_root, filesystem_error);
    if(filesystem_error) {
        close_archive();
        return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Backup validation directory could not be created"));
    }
    for(const auto& entry : manifest.value().entries) {
        const auto found = archive_entries.find(entry.path);
        mz_zip_archive_file_stat stat{};
        if(found == archive_entries.end() || !mz_zip_reader_file_stat(&archive, found->second, &stat)
           || stat.m_uncomp_size != entry.size) {
            close_archive();
            return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup entry size does not match the manifest"));
        }
        const auto destination = extraction_root / std::filesystem::path(entry.path);
        std::filesystem::create_directories(destination.parent_path(), filesystem_error);
        const auto destination_utf8 = utf8_path(destination);
        if(filesystem_error || !mz_zip_reader_extract_to_file(&archive, found->second, destination_utf8.c_str(), 0)) {
            close_archive();
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Backup entry could not be extracted"));
        }
        auto hash = sha256_file(destination);
        if(!hash || hash.value() != entry.sha256) {
            close_archive();
            return Result<BackupInspection>::failure(Error(ErrorCode::invalid_argument, "Backup entry hash does not match the manifest"));
        }
    }
    close_archive();
    return Result<BackupInspection>::success(BackupInspection{
        .valid = true,
        .format_version = 1,
        .file_count = manifest.value().entries.size(),
        .total_uncompressed_bytes = total_size,
        .issues = {},
    });
}

}  // namespace

class SqliteWorkspace::Impl final {
public:
    Impl(
        std::filesystem::path root,
        WorkspaceLock workspace_lock,
        std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database,
        PdfEngine& pdf_engine,
        WorkspaceInfo info
    ) noexcept
        : root_(std::move(root)),
          workspace_lock_(std::move(workspace_lock)),
          database_(std::move(database)),
          pdf_engine_(pdf_engine),
          info_(info) {}

    [[nodiscard]] WorkspaceInfo info() const noexcept { return info_; }

    [[nodiscard]] Result<ImportDocumentResult> import_pdf(
        const std::filesystem::path& source,
        const CancellationToken& cancellation
    ) {
        const std::scoped_lock lock(mutex_);
        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        std::error_code filesystem_error;
        if(!std::filesystem::is_regular_file(source, filesystem_error) || filesystem_error) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::not_found, "PDF source file was not found")
            );
        }

        auto document_result = pdf_engine_.open(source);
        if(!document_result) {
            return Result<ImportDocumentResult>::failure(document_result.error());
        }
        const auto page_count = document_result.value()->page_count();

        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }

        auto hash_result = sha256_file(source, cancellation);
        if(!hash_result) {
            return Result<ImportDocumentResult>::failure(hash_result.error());
        }
        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        const auto& content_hash = hash_result.value();
        auto existing = find_by_hash(content_hash);
        if(!existing) {
            return Result<ImportDocumentResult>::failure(existing.error());
        }
        if(existing.value().has_value()) {
            if(cancellation.is_cancellation_requested()) {
                return Result<ImportDocumentResult>::failure(
                    Error(ErrorCode::cancelled, "Document import was cancelled")
                );
            }
            return Result<ImportDocumentResult>::success(
                ImportDocumentResult{.document = std::move(*existing.value()), .reused_existing = true}
            );
        }

        const auto byte_length = std::filesystem::file_size(source, filesystem_error);
        if(filesystem_error || byte_length > static_cast<std::uintmax_t>(
                                               std::numeric_limits<sqlite3_int64>::max()
                                           ) ||
           page_count > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "PDF source metadata is out of range")
            );
        }

        const std::filesystem::path object_key = std::filesystem::path("objects") / "pdf" /
                                                 content_hash.substr(0, 2) /
                                                 (content_hash + ".pdf");
        const auto object_path = root_ / object_key;
        std::filesystem::create_directories(object_path.parent_path(), filesystem_error);
        if(filesystem_error) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "PDF object directory could not be created")
            );
        }
        if(std::filesystem::exists(object_path, filesystem_error)) {
            auto object_hash = sha256_file(object_path, cancellation);
            if(!object_hash || object_hash.value() != content_hash) {
                return Result<ImportDocumentResult>::failure(
                    Error(ErrorCode::conflict, "Existing PDF object does not match its content key")
                );
            }
        } else {
            auto copy_result = copy_file_cancellable(source, object_path, cancellation);
            if(!copy_result) return Result<ImportDocumentResult>::failure(copy_result.error());
        }

        auto ids_result = create_id_pair(database_.get());
        if(!ids_result) {
            return Result<ImportDocumentResult>::failure(ids_result.error());
        }
        const auto [document_id, version_id] = ids_result.value();
        auto title = utf8_path(source.stem());
        if(title.empty()) {
            title = "Untitled";
        }

        Transaction transaction(database_.get());
        auto begin_result = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin_result) {
            return Result<ImportDocumentResult>::failure(begin_result.error());
        }
        transaction.begin();

        auto document_insert = prepare(
            database_.get(),
            "INSERT INTO documents(id, title, active_version_id, created_at) "
            "VALUES(?1, ?2, NULL, unixepoch())"
        );
        if(!document_insert || !bind_id(document_insert.value().get(), 1, document_id) ||
           !bind_text(document_insert.value().get(), 2, title) ||
           sqlite3_step(document_insert.value().get()) != SQLITE_DONE) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "Document record could not be inserted")
            );
        }

        const auto object_key_utf8 = generic_utf8_path(object_key);
        auto version_insert = prepare(
            database_.get(),
            "INSERT INTO document_versions("
            "id, document_id, content_hash, object_key, byte_length, page_count, created_at"
            ") VALUES(?1, ?2, ?3, ?4, ?5, ?6, unixepoch())"
        );
        if(!version_insert || !bind_id(version_insert.value().get(), 1, version_id) ||
           !bind_id(version_insert.value().get(), 2, document_id) ||
           !bind_text(version_insert.value().get(), 3, content_hash) ||
           !bind_text(version_insert.value().get(), 4, object_key_utf8) ||
           sqlite3_bind_int64(
               version_insert.value().get(),
               5,
               static_cast<sqlite3_int64>(byte_length)
           ) != SQLITE_OK ||
           sqlite3_bind_int64(
               version_insert.value().get(),
               6,
               static_cast<sqlite3_int64>(page_count)
           ) != SQLITE_OK ||
           sqlite3_step(version_insert.value().get()) != SQLITE_DONE) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "Document version could not be inserted")
            );
        }

        auto activate = prepare(
            database_.get(),
            "UPDATE documents SET active_version_id = ?1 WHERE id = ?2"
        );
        if(!activate || !bind_id(activate.value().get(), 1, version_id) ||
           !bind_id(activate.value().get(), 2, document_id) ||
           sqlite3_step(activate.value().get()) != SQLITE_DONE ||
           sqlite3_changes(database_.get()) != 1) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::storage_failure, "Document version could not be activated")
            );
        }

        if(cancellation.is_cancellation_requested()) {
            return Result<ImportDocumentResult>::failure(
                Error(ErrorCode::cancelled, "Document import was cancelled")
            );
        }
        terminate_at_import_fault_point("before-commit");
        auto commit_result = execute(database_.get(), "COMMIT");
        if(!commit_result) {
            return Result<ImportDocumentResult>::failure(commit_result.error());
        }
        transaction.commit();
        terminate_at_import_fault_point("after-commit");

        return Result<ImportDocumentResult>::success(ImportDocumentResult{
            .document = DocumentRecord{
                .document_id = document_id,
                .version_id = version_id,
                .title = std::move(title),
                .content_sha256 = content_hash,
                .object_key = object_key_utf8,
                .byte_length = static_cast<std::uint64_t>(byte_length),
                .page_count = page_count,
            },
            .reused_existing = false,
        });
    }

    [[nodiscard]] Result<std::vector<DocumentRecord>> list_documents() {
        const std::scoped_lock lock(mutex_);
        auto statement_result = prepare(
            database_.get(),
            "SELECT d.id, v.id, d.title, v.content_hash, v.object_key, d.active_version_id, "
            "v.byte_length, v.page_count "
            "FROM documents d JOIN document_versions v ON v.id = d.active_version_id "
            "ORDER BY d.created_at, d.rowid"
        );
        if(!statement_result) {
            return Result<std::vector<DocumentRecord>>::failure(statement_result.error());
        }
        auto statement = std::move(statement_result).value();
        std::vector<DocumentRecord> documents;
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto record = read_document_record(statement.get());
            if(!record) {
                return Result<std::vector<DocumentRecord>>::failure(record.error());
            }
            documents.push_back(std::move(record).value());
        }
        if(step_result != SQLITE_DONE) {
            return Result<std::vector<DocumentRecord>>::failure(
                Error(ErrorCode::storage_failure, "Document records could not be listed")
            );
        }
        return Result<std::vector<DocumentRecord>>::success(std::move(documents));
    }

    [[nodiscard]] Result<ResolvedDocumentObject> resolve_document(DocumentId document_id) {
        const std::scoped_lock lock(mutex_);
        auto statement_result = prepare(
            database_.get(),
            "SELECT d.id, v.id, d.title, v.content_hash, v.object_key, d.active_version_id, "
            "v.byte_length, v.page_count "
            "FROM documents d JOIN document_versions v ON v.id = d.active_version_id "
            "WHERE d.id = ?1 LIMIT 1"
        );
        if(!statement_result) {
            return Result<ResolvedDocumentObject>::failure(statement_result.error());
        }
        auto statement = std::move(statement_result).value();
        if(!bind_id(statement.get(), 1, document_id)) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document ID could not be bound")
            );
        }
        const auto step_result = sqlite3_step(statement.get());
        if(step_result == SQLITE_DONE) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::not_found, "Document was not found")
            );
        }
        if(step_result != SQLITE_ROW) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document lookup failed")
            );
        }
        auto record_result = read_document_record(statement.get());
        if(!record_result) {
            return Result<ResolvedDocumentObject>::failure(record_result.error());
        }
        auto record = std::move(record_result).value();
        const std::filesystem::path object_key(record.object_key);
        bool unsafe_path = object_key.empty() || object_key.is_absolute();
        for(const auto& component : object_key) {
            if(component == "..") {
                unsafe_path = true;
            }
        }
        if(unsafe_path) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document object path is invalid")
            );
        }
        const auto object_path = root_ / object_key;
        std::error_code filesystem_error;
        if(!std::filesystem::is_regular_file(object_path, filesystem_error) || filesystem_error) {
            return Result<ResolvedDocumentObject>::failure(
                Error(ErrorCode::storage_failure, "Document object is missing")
            );
        }
        return Result<ResolvedDocumentObject>::success(
            ResolvedDocumentObject{
                .document = std::move(record),
                .path = object_path,
            }
        );
    }

    [[nodiscard]] Result<AnnotationRecord> create_annotation(const CreateAnnotation& command) {
        const std::scoped_lock lock(mutex_);
        if(command.quads.empty() || command.quote.exact.empty() || command.layout_version.empty()
           || command.page_index > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())
           || (command.direction != "ltr" && command.direction != "rtl" && command.direction != "ttb")
           || (command.anchor_version != 1U && command.anchor_version != 2U)
           || (command.anchor_version == 2U && (!command.text_start || !command.text_end
               || *command.text_end <= *command.text_start))) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::invalid_argument, "Annotation anchor is incomplete")
            );
        }
        for(const auto& quad : command.quads) {
            if(!std::isfinite(quad.x) || !std::isfinite(quad.y) || !std::isfinite(quad.width)
               || !std::isfinite(quad.height) || quad.width <= 0.0 || quad.height <= 0.0) {
                return Result<AnnotationRecord>::failure(
                    Error(ErrorCode::invalid_argument, "Annotation quad is invalid")
                );
            }
        }

        auto version_lookup = prepare(
            database_.get(),
            "SELECT page_count FROM document_versions WHERE id = ?1"
        );
        if(!version_lookup || !bind_id(version_lookup.value().get(), 1, command.document_version_id)) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::storage_failure, "Document version lookup could not be prepared")
            );
        }
        const auto version_step = sqlite3_step(version_lookup.value().get());
        if(version_step == SQLITE_DONE) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::not_found, "Document version was not found")
            );
        }
        if(version_step != SQLITE_ROW
           || command.page_index >= static_cast<std::size_t>(sqlite3_column_int64(version_lookup.value().get(), 0))) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::invalid_argument, "Annotation page index is out of range")
            );
        }

        auto id_result = create_id<AnnotationId>(database_.get());
        if(!id_result) {
            return Result<AnnotationRecord>::failure(id_result.error());
        }
        const auto id = id_result.value();
        Transaction transaction(database_.get());
        auto begin_result = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin_result) {
            return Result<AnnotationRecord>::failure(begin_result.error());
        }
        transaction.begin();

        auto annotation_insert = prepare(
            database_.get(),
            "INSERT INTO annotations(id, document_version_id, page_index, quote_exact, "
            "quote_prefix, quote_suffix, layout_version, color, anchor_version, text_start, text_end, "
            "direction, created_at, updated_at) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, unixepoch(), unixepoch())"
        );
        if(!annotation_insert || !bind_id(annotation_insert.value().get(), 1, id)
           || !bind_id(annotation_insert.value().get(), 2, command.document_version_id)
           || sqlite3_bind_int64(annotation_insert.value().get(), 3, static_cast<sqlite3_int64>(command.page_index)) != SQLITE_OK
           || !bind_text(annotation_insert.value().get(), 4, command.quote.exact)
           || !bind_text(annotation_insert.value().get(), 5, command.quote.prefix)
           || !bind_text(annotation_insert.value().get(), 6, command.quote.suffix)
           || !bind_text(annotation_insert.value().get(), 7, command.layout_version)
           || !bind_text(annotation_insert.value().get(), 8, color_name(command.color))
           || sqlite3_bind_int(annotation_insert.value().get(), 9, static_cast<int>(command.anchor_version)) != SQLITE_OK
           || (command.text_start
               ? sqlite3_bind_int64(annotation_insert.value().get(), 10, static_cast<sqlite3_int64>(*command.text_start))
               : sqlite3_bind_null(annotation_insert.value().get(), 10)) != SQLITE_OK
           || (command.text_end
               ? sqlite3_bind_int64(annotation_insert.value().get(), 11, static_cast<sqlite3_int64>(*command.text_end))
               : sqlite3_bind_null(annotation_insert.value().get(), 11)) != SQLITE_OK
           || !bind_text(annotation_insert.value().get(), 12, command.direction)
           || sqlite3_step(annotation_insert.value().get()) != SQLITE_DONE) {
            return Result<AnnotationRecord>::failure(
                Error(ErrorCode::storage_failure, "Annotation could not be inserted")
            );
        }

        auto quad_insert = prepare(
            database_.get(),
            "INSERT INTO annotation_quads(annotation_id, ordinal, x, y, width, height) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
        );
        if(!quad_insert) {
            return Result<AnnotationRecord>::failure(quad_insert.error());
        }
        for(std::size_t index = 0; index < command.quads.size(); ++index) {
            const auto& quad = command.quads[index];
            auto* statement = quad_insert.value().get();
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if(!bind_id(statement, 1, id)
               || sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(index)) != SQLITE_OK
               || sqlite3_bind_double(statement, 3, quad.x) != SQLITE_OK
               || sqlite3_bind_double(statement, 4, quad.y) != SQLITE_OK
               || sqlite3_bind_double(statement, 5, quad.width) != SQLITE_OK
               || sqlite3_bind_double(statement, 6, quad.height) != SQLITE_OK
               || sqlite3_step(statement) != SQLITE_DONE) {
                return Result<AnnotationRecord>::failure(
                    Error(ErrorCode::storage_failure, "Annotation quad could not be inserted")
                );
            }
        }
        auto commit_result = execute(database_.get(), "COMMIT");
        if(!commit_result) {
            return Result<AnnotationRecord>::failure(commit_result.error());
        }
        transaction.commit();
        return Result<AnnotationRecord>::success(AnnotationRecord{
            .id = id,
            .document_version_id = command.document_version_id,
            .page_index = command.page_index,
            .quads = command.quads,
            .quote = command.quote,
            .layout_version = command.layout_version,
            .color = command.color,
            .anchor_version = command.anchor_version,
            .text_start = command.text_start,
            .text_end = command.text_end,
            .direction = command.direction,
        });
    }

    [[nodiscard]] Result<std::vector<AnnotationRecord>> list_annotations(
        DocumentVersionId document_version_id
    ) {
        const std::scoped_lock lock(mutex_);
        auto annotations_result = prepare(
            database_.get(),
            "SELECT id, document_version_id, page_index, quote_exact, quote_prefix, quote_suffix, "
            "layout_version, color, anchor_version, text_start, text_end, direction "
            "FROM annotations WHERE document_version_id = ?1 "
            "ORDER BY page_index, created_at, rowid"
        );
        if(!annotations_result || !bind_id(annotations_result.value().get(), 1, document_version_id)) {
            return Result<std::vector<AnnotationRecord>>::failure(
                Error(ErrorCode::storage_failure, "Annotations could not be listed")
            );
        }
        auto annotations = std::move(annotations_result).value();
        std::vector<AnnotationRecord> records;
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(annotations.get())) == SQLITE_ROW) {
            auto id = read_id<AnnotationId>(annotations.get(), 0);
            auto version_id = read_id<DocumentVersionId>(annotations.get(), 1);
            const auto page_index = sqlite3_column_int64(annotations.get(), 2);
            const auto* exact = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 3));
            const auto* prefix = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 4));
            const auto* suffix = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 5));
            const auto* layout = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 6));
            const auto* color_text = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 7));
            const auto anchor_version = sqlite3_column_int64(annotations.get(), 8);
            const auto text_start = sqlite3_column_int64(annotations.get(), 9);
            const auto text_end = sqlite3_column_int64(annotations.get(), 10);
            const auto* direction = reinterpret_cast<const char*>(sqlite3_column_text(annotations.get(), 11));
            if(!id || !version_id || page_index < 0 || exact == nullptr || prefix == nullptr
               || suffix == nullptr || layout == nullptr || color_text == nullptr || direction == nullptr
               || (anchor_version != 1 && anchor_version != 2)) {
                return Result<std::vector<AnnotationRecord>>::failure(
                    Error(ErrorCode::storage_failure, "SQLite annotation record is invalid")
                );
            }
            auto color = read_color(color_text);
            if(!color) {
                return Result<std::vector<AnnotationRecord>>::failure(color.error());
            }
            auto quads_result = prepare(
                database_.get(),
                "SELECT x, y, width, height FROM annotation_quads WHERE annotation_id = ?1 ORDER BY ordinal"
            );
            if(!quads_result || !bind_id(quads_result.value().get(), 1, id.value())) {
                return Result<std::vector<AnnotationRecord>>::failure(
                    Error(ErrorCode::storage_failure, "Annotation quads could not be listed")
                );
            }
            std::vector<PageRect> quads;
            int quad_step = SQLITE_ROW;
            while((quad_step = sqlite3_step(quads_result.value().get())) == SQLITE_ROW) {
                quads.push_back(PageRect{
                    .x = sqlite3_column_double(quads_result.value().get(), 0),
                    .y = sqlite3_column_double(quads_result.value().get(), 1),
                    .width = sqlite3_column_double(quads_result.value().get(), 2),
                    .height = sqlite3_column_double(quads_result.value().get(), 3),
                });
            }
            if(quad_step != SQLITE_DONE || quads.empty()) {
                return Result<std::vector<AnnotationRecord>>::failure(
                    Error(ErrorCode::storage_failure, "SQLite annotation quads are invalid")
                );
            }
            records.push_back(AnnotationRecord{
                .id = id.value(),
                .document_version_id = version_id.value(),
                .page_index = static_cast<std::size_t>(page_index),
                .quads = std::move(quads),
                .quote = QuoteAnchor{.exact = exact, .prefix = prefix, .suffix = suffix},
                .layout_version = layout,
                .color = color.value(),
                .anchor_version = static_cast<std::uint32_t>(anchor_version),
                .text_start = sqlite3_column_type(annotations.get(), 9) == SQLITE_NULL
                    ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(text_start)),
                .text_end = sqlite3_column_type(annotations.get(), 10) == SQLITE_NULL
                    ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(text_end)),
                .direction = direction,
            });
        }
        if(step_result != SQLITE_DONE) {
            return Result<std::vector<AnnotationRecord>>::failure(
                Error(ErrorCode::storage_failure, "Annotations could not be listed")
            );
        }
        return Result<std::vector<AnnotationRecord>>::success(std::move(records));
    }

    [[nodiscard]] Result<void> delete_annotation(AnnotationId annotation_id) {
        const std::scoped_lock lock(mutex_);
        Transaction transaction(database_.get());
        const auto begin = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin) return begin;
        transaction.begin();
        auto statement_result = prepare(database_.get(), "DELETE FROM annotations WHERE id = ?1");
        if(!statement_result || !bind_id(statement_result.value().get(), 1, annotation_id)
           || sqlite3_step(statement_result.value().get()) != SQLITE_DONE) {
            return Result<void>::failure(
                Error(ErrorCode::storage_failure, "Annotation could not be deleted")
            );
        }
        if(sqlite3_changes(database_.get()) != 1) {
            return Result<void>::failure(Error(ErrorCode::not_found, "Annotation was not found"));
        }
        std::vector<std::filesystem::path> reclaimed_assets;
        const auto synchronize = synchronize_note_assets(annotation_id, "", reclaimed_assets);
        if(!synchronize) return synchronize;
        const auto commit = execute(database_.get(), "COMMIT");
        if(!commit) return commit;
        transaction.commit();
        std::error_code filesystem_error;
        for(const auto& path : reclaimed_assets) {
            std::filesystem::remove(path, filesystem_error);
            filesystem_error.clear();
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<NoteRecord> update_note(const UpdateNote& command) {
        const std::scoped_lock lock(mutex_);
        auto lookup_result = prepare(
            database_.get(),
            "SELECT n.id, n.revision FROM annotations a LEFT JOIN notes n ON n.annotation_id = a.id "
            "WHERE a.id = ?1"
        );
        if(!lookup_result || !bind_id(lookup_result.value().get(), 1, command.annotation_id)) {
            return Result<NoteRecord>::failure(
                Error(ErrorCode::storage_failure, "Note lookup could not be prepared")
            );
        }
        auto lookup = std::move(lookup_result).value();
        const auto step_result = sqlite3_step(lookup.get());
        if(step_result == SQLITE_DONE) {
            return Result<NoteRecord>::failure(Error(ErrorCode::not_found, "Annotation was not found"));
        }
        if(step_result != SQLITE_ROW) {
            return Result<NoteRecord>::failure(Error(ErrorCode::storage_failure, "Note lookup failed"));
        }

        Transaction transaction(database_.get());
        auto begin_result = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin_result) {
            return Result<NoteRecord>::failure(begin_result.error());
        }
        transaction.begin();
        std::vector<std::filesystem::path> reclaimed_assets;

        if(sqlite3_column_type(lookup.get(), 0) == SQLITE_NULL) {
            if(command.expected_revision != 0U) {
                return Result<NoteRecord>::failure(
                    Error(ErrorCode::conflict, "Note revision does not match")
                );
            }
            auto id_result = create_id<NoteId>(database_.get());
            if(!id_result) {
                return Result<NoteRecord>::failure(id_result.error());
            }
            auto insert_result = prepare(
                database_.get(),
                "INSERT INTO notes(id, annotation_id, markdown_source, revision, created_at, updated_at) "
                "VALUES(?1, ?2, ?3, 1, unixepoch(), unixepoch())"
            );
            if(!insert_result || !bind_id(insert_result.value().get(), 1, id_result.value())
               || !bind_id(insert_result.value().get(), 2, command.annotation_id)
               || !bind_text(insert_result.value().get(), 3, command.markdown_source)
               || sqlite3_step(insert_result.value().get()) != SQLITE_DONE) {
                return Result<NoteRecord>::failure(
                    Error(ErrorCode::storage_failure, "Note could not be created")
                );
            }
            auto assets = synchronize_note_assets(
                command.annotation_id,
                command.markdown_source,
                reclaimed_assets
            );
            if(!assets) return Result<NoteRecord>::failure(assets.error());
            auto fault = fail_at_fault_point(
                "CONTEXT_READER_TEST_AUTOSAVE_FAULT",
                "before-commit",
                "Injected note autosave failure before commit"
            );
            if(!fault) return Result<NoteRecord>::failure(fault.error());
            auto commit_result = execute(database_.get(), "COMMIT");
            if(!commit_result) return Result<NoteRecord>::failure(commit_result.error());
            transaction.commit();
            for(const auto& path : reclaimed_assets) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
            fault = fail_at_fault_point(
                "CONTEXT_READER_TEST_AUTOSAVE_FAULT",
                "after-commit",
                "Injected note autosave failure after commit"
            );
            if(!fault) return Result<NoteRecord>::failure(fault.error());
            return Result<NoteRecord>::success(NoteRecord{
                .id = id_result.value(),
                .annotation_id = command.annotation_id,
                .markdown_source = command.markdown_source,
                .revision = 1U,
            });
        }

        auto note_id = read_id<NoteId>(lookup.get(), 0);
        const auto revision = sqlite3_column_int64(lookup.get(), 1);
        if(!note_id || revision <= 0 || static_cast<std::uint64_t>(revision) != command.expected_revision
           || revision == std::numeric_limits<sqlite3_int64>::max()) {
            return Result<NoteRecord>::failure(
                Error(ErrorCode::conflict, "Note revision does not match")
            );
        }
        const auto next_revision = revision + 1;
        auto update_result = prepare(
            database_.get(),
            "UPDATE notes SET markdown_source = ?1, revision = ?2, updated_at = unixepoch() "
            "WHERE id = ?3 AND revision = ?4"
        );
        if(!update_result || !bind_text(update_result.value().get(), 1, command.markdown_source)
           || sqlite3_bind_int64(update_result.value().get(), 2, next_revision) != SQLITE_OK
           || !bind_id(update_result.value().get(), 3, note_id.value())
           || sqlite3_bind_int64(update_result.value().get(), 4, revision) != SQLITE_OK
           || sqlite3_step(update_result.value().get()) != SQLITE_DONE
           || sqlite3_changes(database_.get()) != 1) {
            return Result<NoteRecord>::failure(
                Error(ErrorCode::conflict, "Note revision does not match")
            );
        }
        auto assets = synchronize_note_assets(
            command.annotation_id,
            command.markdown_source,
            reclaimed_assets
        );
        if(!assets) return Result<NoteRecord>::failure(assets.error());
        auto fault = fail_at_fault_point(
            "CONTEXT_READER_TEST_AUTOSAVE_FAULT",
            "before-commit",
            "Injected note autosave failure before commit"
        );
        if(!fault) return Result<NoteRecord>::failure(fault.error());
        auto commit_result = execute(database_.get(), "COMMIT");
        if(!commit_result) return Result<NoteRecord>::failure(commit_result.error());
        transaction.commit();
        for(const auto& path : reclaimed_assets) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        fault = fail_at_fault_point(
            "CONTEXT_READER_TEST_AUTOSAVE_FAULT",
            "after-commit",
            "Injected note autosave failure after commit"
        );
        if(!fault) return Result<NoteRecord>::failure(fault.error());
        return Result<NoteRecord>::success(NoteRecord{
            .id = note_id.value(),
            .annotation_id = command.annotation_id,
            .markdown_source = command.markdown_source,
            .revision = static_cast<std::uint64_t>(next_revision),
        });
    }

    [[nodiscard]] Result<std::vector<NoteRecord>> list_notes(
        DocumentVersionId document_version_id
    ) {
        const std::scoped_lock lock(mutex_);
        auto statement_result = prepare(
            database_.get(),
            "SELECT n.id, n.annotation_id, n.markdown_source, n.revision FROM notes n "
            "JOIN annotations a ON a.id = n.annotation_id WHERE a.document_version_id = ?1 "
            "ORDER BY n.created_at, n.rowid"
        );
        if(!statement_result || !bind_id(statement_result.value().get(), 1, document_version_id)) {
            return Result<std::vector<NoteRecord>>::failure(
                Error(ErrorCode::storage_failure, "Notes could not be listed")
            );
        }
        auto statement = std::move(statement_result).value();
        std::vector<NoteRecord> notes;
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto note_id = read_id<NoteId>(statement.get(), 0);
            auto annotation_id = read_id<AnnotationId>(statement.get(), 1);
            const auto* markdown = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2));
            const auto revision = sqlite3_column_int64(statement.get(), 3);
            if(!note_id || !annotation_id || markdown == nullptr || revision <= 0) {
                return Result<std::vector<NoteRecord>>::failure(
                    Error(ErrorCode::storage_failure, "SQLite note record is invalid")
                );
            }
            notes.push_back(NoteRecord{
                .id = note_id.value(),
                .annotation_id = annotation_id.value(),
                .markdown_source = markdown,
                .revision = static_cast<std::uint64_t>(revision),
            });
        }
        if(step_result != SQLITE_DONE) {
            return Result<std::vector<NoteRecord>>::failure(
                Error(ErrorCode::storage_failure, "Notes could not be listed")
            );
        }
        return Result<std::vector<NoteRecord>>::success(std::move(notes));
    }

    [[nodiscard]] Result<AssetRecord> import_note_asset(
        AnnotationId annotation_id,
        const std::filesystem::path& source,
        const CancellationToken& cancellation
    ) {
        const std::scoped_lock lock(mutex_);
        if(cancellation.is_cancellation_requested()) {
            return Result<AssetRecord>::failure(Error(ErrorCode::cancelled, "Asset import was cancelled"));
        }
        std::error_code filesystem_error;
        if(!std::filesystem::is_regular_file(source, filesystem_error) || filesystem_error) {
            return Result<AssetRecord>::failure(Error(ErrorCode::not_found, "Asset source file was not found"));
        }
        const auto byte_length = std::filesystem::file_size(source, filesystem_error);
        if(filesystem_error || byte_length == 0 || byte_length > 10ULL * 1024ULL * 1024ULL) {
            return Result<AssetRecord>::failure(
                Error(ErrorCode::invalid_argument, "Asset byte length exceeds the supported limit")
            );
        }
        auto metadata = inspect_image(source);
        if(!metadata) return Result<AssetRecord>::failure(metadata.error());
        auto annotation_lookup = prepare(database_.get(), "SELECT 1 FROM annotations WHERE id = ?1");
        if(!annotation_lookup || !bind_id(annotation_lookup.value().get(), 1, annotation_id)
           || sqlite3_step(annotation_lookup.value().get()) != SQLITE_ROW) {
            return Result<AssetRecord>::failure(Error(ErrorCode::not_found, "Annotation was not found"));
        }
        auto hash = sha256_file(source, cancellation);
        if(!hash) return Result<AssetRecord>::failure(hash.error());

        std::optional<AssetRecord> record;
        std::string object_key;
        bool new_asset = false;
        auto existing = prepare(
            database_.get(),
            "SELECT id, content_hash, object_key, media_type, byte_length, width, height "
            "FROM assets WHERE content_hash = ?1"
        );
        if(!existing || !bind_text(existing.value().get(), 1, hash.value())) {
            return Result<AssetRecord>::failure(Error(ErrorCode::storage_failure, "Asset lookup failed"));
        }
        const auto existing_step = sqlite3_step(existing.value().get());
        if(existing_step == SQLITE_ROW) {
            auto id = read_id<AssetId>(existing.value().get(), 0);
            const auto* stored_hash = reinterpret_cast<const char*>(sqlite3_column_text(existing.value().get(), 1));
            const auto* stored_key = reinterpret_cast<const char*>(sqlite3_column_text(existing.value().get(), 2));
            const auto* media_type = reinterpret_cast<const char*>(sqlite3_column_text(existing.value().get(), 3));
            if(!id || stored_hash == nullptr || stored_key == nullptr || media_type == nullptr) {
                return Result<AssetRecord>::failure(Error(ErrorCode::storage_failure, "Stored asset is invalid"));
            }
            object_key = stored_key;
            record = AssetRecord{
                .id = id.value(),
                .content_sha256 = stored_hash,
                .media_type = media_type,
                .byte_length = static_cast<std::uint64_t>(sqlite3_column_int64(existing.value().get(), 4)),
                .width = static_cast<std::uint32_t>(sqlite3_column_int64(existing.value().get(), 5)),
                .height = static_cast<std::uint32_t>(sqlite3_column_int64(existing.value().get(), 6)),
            };
        } else if(existing_step == SQLITE_DONE) {
            auto id = create_id<AssetId>(database_.get());
            if(!id) return Result<AssetRecord>::failure(id.error());
            object_key = generic_utf8_path(
                std::filesystem::path("objects") / "assets" / hash.value().substr(0, 2) /
                (hash.value() + metadata.value().extension)
            );
            const auto object_path = root_ / std::filesystem::path(object_key);
            std::filesystem::create_directories(object_path.parent_path(), filesystem_error);
            if(filesystem_error) {
                return Result<AssetRecord>::failure(Error(ErrorCode::storage_failure, "Asset object directory could not be created"));
            }
            auto copied = copy_file_cancellable(source, object_path, cancellation);
            if(!copied) return Result<AssetRecord>::failure(copied.error());
            new_asset = true;
            record = AssetRecord{
                .id = id.value(),
                .content_sha256 = hash.value(),
                .media_type = metadata.value().media_type,
                .byte_length = static_cast<std::uint64_t>(byte_length),
                .width = metadata.value().width,
                .height = metadata.value().height,
            };
        } else {
            return Result<AssetRecord>::failure(Error(ErrorCode::storage_failure, "Asset lookup failed"));
        }

        Transaction transaction(database_.get());
        auto begin = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin) return Result<AssetRecord>::failure(begin.error());
        transaction.begin();
        if(new_asset) {
            auto insert = prepare(
                database_.get(),
                "INSERT INTO assets(id, content_hash, object_key, media_type, byte_length, width, height, created_at) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, unixepoch())"
            );
            if(!insert || !bind_id(insert.value().get(), 1, record->id)
               || !bind_text(insert.value().get(), 2, record->content_sha256)
               || !bind_text(insert.value().get(), 3, object_key)
               || !bind_text(insert.value().get(), 4, record->media_type)
               || sqlite3_bind_int64(insert.value().get(), 5, static_cast<sqlite3_int64>(record->byte_length)) != SQLITE_OK
               || sqlite3_bind_int64(insert.value().get(), 6, record->width) != SQLITE_OK
               || sqlite3_bind_int64(insert.value().get(), 7, record->height) != SQLITE_OK
               || sqlite3_step(insert.value().get()) != SQLITE_DONE) {
                std::filesystem::remove(root_ / std::filesystem::path(object_key), filesystem_error);
                return Result<AssetRecord>::failure(Error(ErrorCode::storage_failure, "Asset record could not be inserted"));
            }
        }
        auto associate = prepare(
            database_.get(),
            "INSERT OR IGNORE INTO annotation_assets(annotation_id, asset_id) VALUES(?1, ?2)"
        );
        if(!associate || !bind_id(associate.value().get(), 1, annotation_id)
           || !bind_id(associate.value().get(), 2, record->id)
           || sqlite3_step(associate.value().get()) != SQLITE_DONE) {
            return Result<AssetRecord>::failure(Error(ErrorCode::storage_failure, "Asset could not be associated with the annotation"));
        }
        auto commit = execute(database_.get(), "COMMIT");
        if(!commit) return Result<AssetRecord>::failure(commit.error());
        transaction.commit();
        return Result<AssetRecord>::success(std::move(*record));
    }

    [[nodiscard]] Result<AssetData> read_asset(AssetId asset_id) {
        const std::scoped_lock lock(mutex_);
        auto lookup = prepare(
            database_.get(),
            "SELECT content_hash, object_key, media_type, byte_length, width, height FROM assets WHERE id = ?1"
        );
        if(!lookup || !bind_id(lookup.value().get(), 1, asset_id)) {
            return Result<AssetData>::failure(Error(ErrorCode::storage_failure, "Asset lookup failed"));
        }
        const auto step = sqlite3_step(lookup.value().get());
        if(step == SQLITE_DONE) return Result<AssetData>::failure(Error(ErrorCode::not_found, "Asset was not found"));
        const auto* hash = reinterpret_cast<const char*>(sqlite3_column_text(lookup.value().get(), 0));
        const auto* object_key = reinterpret_cast<const char*>(sqlite3_column_text(lookup.value().get(), 1));
        const auto* media_type = reinterpret_cast<const char*>(sqlite3_column_text(lookup.value().get(), 2));
        const auto byte_length = sqlite3_column_int64(lookup.value().get(), 3);
        if(step != SQLITE_ROW || hash == nullptr || object_key == nullptr || media_type == nullptr
           || byte_length < 0 || byte_length > 10LL * 1024LL * 1024LL) {
            return Result<AssetData>::failure(Error(ErrorCode::storage_failure, "Stored asset is invalid"));
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byte_length));
        std::ifstream input(root_ / std::filesystem::path(object_key), std::ios::binary);
        if(!input || (byte_length > 0 && !input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        ))) {
            return Result<AssetData>::failure(Error(ErrorCode::storage_failure, "Asset object could not be read"));
        }
        return Result<AssetData>::success(AssetData{
            .asset = AssetRecord{
                .id = asset_id,
                .content_sha256 = hash,
                .media_type = media_type,
                .byte_length = static_cast<std::uint64_t>(byte_length),
                .width = static_cast<std::uint32_t>(sqlite3_column_int64(lookup.value().get(), 4)),
                .height = static_cast<std::uint32_t>(sqlite3_column_int64(lookup.value().get(), 5)),
            },
            .bytes = std::move(bytes),
        });
    }

    [[nodiscard]] Result<BackupInspection> export_package(
        const std::filesystem::path& destination,
        const CancellationToken& cancellation
    ) {
        const std::scoped_lock lock(mutex_);
        if(destination.empty() || cancellation.is_cancellation_requested()) {
            return Result<BackupInspection>::failure(
                Error(cancellation.is_cancellation_requested() ? ErrorCode::cancelled : ErrorCode::invalid_argument,
                      cancellation.is_cancellation_requested() ? "Workspace export was cancelled" : "Backup destination is empty")
            );
        }
        std::error_code filesystem_error;
        if(std::filesystem::exists(destination, filesystem_error)) {
            return Result<BackupInspection>::failure(Error(ErrorCode::already_exists, "Backup destination already exists"));
        }
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        const auto temporary_root = root_ / "backups" / (".readerpkg-" + std::to_string(stamp));
        std::filesystem::create_directories(temporary_root, filesystem_error);
        const auto cleanup = [&] {
            std::error_code ignored;
            std::filesystem::remove_all(temporary_root, ignored);
        };
        if(filesystem_error) {
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Backup staging directory could not be created"));
        }
        const auto snapshot_path = temporary_root / "workspace.db";
        sqlite3* raw_snapshot = nullptr;
        const auto snapshot_utf8 = utf8_path(snapshot_path);
        if(sqlite3_open_v2(
               snapshot_utf8.c_str(),
               &raw_snapshot,
               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_FULLMUTEX,
               nullptr
           ) != SQLITE_OK) {
            if(raw_snapshot != nullptr) sqlite3_close_v2(raw_snapshot);
            cleanup();
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Workspace snapshot could not be created"));
        }
        std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> snapshot(raw_snapshot, sqlite3_close_v2);
        auto* backup = sqlite3_backup_init(snapshot.get(), "main", database_.get(), "main");
        const auto backup_step = backup == nullptr ? SQLITE_ERROR : sqlite3_backup_step(backup, -1);
        const auto backup_finish = backup == nullptr ? SQLITE_ERROR : sqlite3_backup_finish(backup);
        snapshot.reset();
        if(backup_step != SQLITE_DONE || backup_finish != SQLITE_OK) {
            cleanup();
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Workspace snapshot failed"));
        }

        std::vector<std::pair<std::string, std::filesystem::path>> sources{
            {"workspace.db", snapshot_path},
        };
        auto objects = prepare(
            database_.get(),
            "SELECT object_key FROM document_versions UNION SELECT object_key FROM assets ORDER BY object_key"
        );
        if(!objects) {
            cleanup();
            return Result<BackupInspection>::failure(objects.error());
        }
        int step = SQLITE_ROW;
        while((step = sqlite3_step(objects.value().get())) == SQLITE_ROW) {
            const auto* key = reinterpret_cast<const char*>(sqlite3_column_text(objects.value().get(), 0));
            if(key == nullptr || !safe_package_path(key)) {
                cleanup();
                return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Workspace object key is unsafe"));
            }
            sources.emplace_back(key, root_ / std::filesystem::path(key));
        }
        if(step != SQLITE_DONE) {
            cleanup();
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Workspace objects could not be listed"));
        }

        std::vector<PackageManifestEntry> entries;
        entries.reserve(sources.size());
        std::uint64_t total_size = 0;
        for(const auto& [archive_path, source_path] : sources) {
            if(cancellation.is_cancellation_requested()) {
                cleanup();
                return Result<BackupInspection>::failure(Error(ErrorCode::cancelled, "Workspace export was cancelled"));
            }
            const auto size = std::filesystem::file_size(source_path, filesystem_error);
            auto hash = sha256_file(source_path, cancellation);
            if(filesystem_error || !hash || size > std::numeric_limits<std::uint64_t>::max() - total_size) {
                cleanup();
                return Result<BackupInspection>::failure(
                    hash ? Error(ErrorCode::storage_failure, "Workspace object metadata could not be read") : hash.error()
                );
            }
            total_size += static_cast<std::uint64_t>(size);
            entries.push_back(PackageManifestEntry{
                .path = archive_path,
                .sha256 = hash.value(),
                .size = static_cast<std::uint64_t>(size),
            });
        }
        const auto manifest_path = temporary_root / "manifest.json";
        std::ofstream manifest(manifest_path, std::ios::binary | std::ios::trunc);
        manifest << "{\"format\":\"readerpkg\",\"version\":1,\"entries\":[";
        for(std::size_t index = 0; index < entries.size(); ++index) {
            if(index != 0) manifest << ',';
            manifest << "{\"path\":\"" << entries[index].path
                     << "\",\"sha256\":\"" << entries[index].sha256
                     << "\",\"size\":" << entries[index].size << '}';
        }
        manifest << "]}";
        manifest.close();
        if(!manifest) {
            cleanup();
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Backup manifest could not be written"));
        }

        auto partial = destination;
        partial += ".partial";
        std::filesystem::remove(partial, filesystem_error);
        mz_zip_archive archive{};
        const auto partial_utf8 = utf8_path(partial);
        if(!mz_zip_writer_init_file_v2(&archive, partial_utf8.c_str(), 0, MZ_ZIP_FLAG_WRITE_ZIP64)) {
            cleanup();
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Backup archive could not be created"));
        }
        const auto manifest_utf8 = utf8_path(manifest_path);
        bool written = mz_zip_writer_add_file(
            &archive, "manifest.json", manifest_utf8.c_str(), nullptr, 0, MZ_BEST_COMPRESSION
        ) != 0;
        for(std::size_t index = 0; written && index < sources.size(); ++index) {
            const auto source_utf8 = utf8_path(sources[index].second);
            written = mz_zip_writer_add_file(
                &archive,
                sources[index].first.c_str(),
                source_utf8.c_str(),
                nullptr,
                0,
                MZ_BEST_COMPRESSION
            ) != 0;
        }
        written = written && mz_zip_writer_finalize_archive(&archive) != 0;
        const auto ended = mz_zip_writer_end(&archive) != 0;
        cleanup();
        if(!written || !ended) {
            std::filesystem::remove(partial, filesystem_error);
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Backup archive could not be finalized"));
        }
        std::filesystem::rename(partial, destination, filesystem_error);
        if(filesystem_error) {
            std::filesystem::remove(partial, filesystem_error);
            return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Backup archive could not be committed"));
        }
        return Result<BackupInspection>::success(BackupInspection{
            .valid = true,
            .format_version = 1,
            .file_count = entries.size(),
            .total_uncompressed_bytes = total_size,
            .issues = {},
        });
    }

    [[nodiscard]] Result<void> rebuild_search_index(const CancellationToken& cancellation) {
        const std::scoped_lock lock(mutex_);
        if(cancellation.is_cancellation_requested()) {
            return Result<void>::failure(Error(ErrorCode::cancelled, "Search index rebuild was cancelled"));
        }
        Transaction transaction(database_.get());
        auto begin = execute(database_.get(), "BEGIN IMMEDIATE");
        if(!begin) return begin;
        transaction.begin();
        if(!execute(database_.get(), "UPDATE search_index.metadata SET status = 'building' WHERE singleton = 1")
           || !execute(database_.get(), "DELETE FROM search_index.entries")) {
            return Result<void>::failure(Error(ErrorCode::storage_failure, "Search index could not be reset"));
        }
        auto insert = prepare(
            database_.get(),
            "INSERT INTO search_index.entries(kind, document_version_id, note_id, page_index, title, content) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
        );
        auto documents = prepare(
            database_.get(),
            "SELECT v.id, d.title, v.object_key, v.page_count FROM documents d "
            "JOIN document_versions v ON v.id = d.active_version_id ORDER BY d.rowid"
        );
        if(!insert || !documents) {
            return Result<void>::failure(Error(ErrorCode::storage_failure, "Search index statements could not be prepared"));
        }
        int step = SQLITE_ROW;
        while((step = sqlite3_step(documents.value().get())) == SQLITE_ROW) {
            if(cancellation.is_cancellation_requested()) {
                return Result<void>::failure(Error(ErrorCode::cancelled, "Search index rebuild was cancelled"));
            }
            auto version_id = read_id<DocumentVersionId>(documents.value().get(), 0);
            const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(documents.value().get(), 1));
            const auto* object_key = reinterpret_cast<const char*>(sqlite3_column_text(documents.value().get(), 2));
            const auto page_count = sqlite3_column_int64(documents.value().get(), 3);
            if(!version_id || title == nullptr || object_key == nullptr || page_count < 0) {
                return Result<void>::failure(Error(ErrorCode::storage_failure, "Search source metadata is invalid"));
            }
            auto opened = pdf_engine_.open(root_ / std::filesystem::path(object_key));
            if(!opened) return Result<void>::failure(opened.error());
            for(std::size_t page_index = 0; page_index < static_cast<std::size_t>(page_count); ++page_index) {
                if(cancellation.is_cancellation_requested()) {
                    return Result<void>::failure(Error(ErrorCode::cancelled, "Search index rebuild was cancelled"));
                }
                auto page_text = opened.value()->extract_text(page_index);
                if(!page_text) return Result<void>::failure(page_text.error());
                auto* statement = insert.value().get();
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                const auto page_text_index = std::to_string(page_index);
                const auto version_hex = stable_id_to_hex(version_id.value());
                if(!bind_text(statement, 1, "pdf") || !bind_text(statement, 2, version_hex)
                   || !bind_text(statement, 3, "") || !bind_text(statement, 4, page_text_index)
                   || !bind_text(statement, 5, title) || !bind_text(statement, 6, page_text.value().text)
                   || sqlite3_step(statement) != SQLITE_DONE) {
                    return Result<void>::failure(Error(ErrorCode::storage_failure, "PDF search entry could not be indexed"));
                }
            }
        }
        if(step != SQLITE_DONE) {
            return Result<void>::failure(Error(ErrorCode::storage_failure, "Search source documents could not be listed"));
        }

        auto notes = prepare(
            database_.get(),
            "SELECT n.id, a.document_version_id, a.page_index, d.title, n.markdown_source FROM notes n "
            "JOIN annotations a ON a.id = n.annotation_id "
            "JOIN document_versions v ON v.id = a.document_version_id "
            "JOIN documents d ON d.id = v.document_id ORDER BY n.rowid"
        );
        if(!notes) return Result<void>::failure(notes.error());
        while((step = sqlite3_step(notes.value().get())) == SQLITE_ROW) {
            auto note_id = read_id<NoteId>(notes.value().get(), 0);
            auto version_id = read_id<DocumentVersionId>(notes.value().get(), 1);
            const auto page_index = sqlite3_column_int64(notes.value().get(), 2);
            const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(notes.value().get(), 3));
            const auto* markdown = reinterpret_cast<const char*>(sqlite3_column_text(notes.value().get(), 4));
            if(!note_id || !version_id || page_index < 0 || title == nullptr || markdown == nullptr) {
                return Result<void>::failure(Error(ErrorCode::storage_failure, "Note search source is invalid"));
            }
            auto* statement = insert.value().get();
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            const auto page_text_index = std::to_string(page_index);
            const auto version_hex = stable_id_to_hex(version_id.value());
            const auto note_hex = stable_id_to_hex(note_id.value());
            if(!bind_text(statement, 1, "note") || !bind_text(statement, 2, version_hex)
               || !bind_text(statement, 3, note_hex) || !bind_text(statement, 4, page_text_index)
               || !bind_text(statement, 5, title) || !bind_text(statement, 6, markdown)
               || sqlite3_step(statement) != SQLITE_DONE) {
                return Result<void>::failure(Error(ErrorCode::storage_failure, "Note search entry could not be indexed"));
            }
        }
        if(step != SQLITE_DONE
           || !execute(database_.get(), "UPDATE search_index.metadata SET version = 1, status = 'ready' WHERE singleton = 1")) {
            return Result<void>::failure(Error(ErrorCode::storage_failure, "Search index could not be finalized"));
        }
        auto commit = execute(database_.get(), "COMMIT");
        if(!commit) return commit;
        transaction.commit();
        return Result<void>::success();
    }

    [[nodiscard]] Result<SearchResponse> search(std::string_view query, std::size_t limit) {
        const std::scoped_lock lock(mutex_);
        if(query.empty() || query.size() > 1024 || limit == 0 || limit > 200) {
            return Result<SearchResponse>::failure(
                Error(ErrorCode::invalid_argument, "Search query or limit is outside the supported range")
            );
        }
        auto status_statement = prepare(database_.get(), "SELECT status FROM search_index.metadata WHERE singleton = 1");
        if(!status_statement || sqlite3_step(status_statement.value().get()) != SQLITE_ROW) {
            return Result<SearchResponse>::failure(Error(ErrorCode::storage_failure, "Search index status is unavailable"));
        }
        const auto* status_text = reinterpret_cast<const char*>(sqlite3_column_text(status_statement.value().get(), 0));
        SearchResponse response{.index_status = status_text == nullptr ? "not_built" : status_text};
        if(response.index_status != "ready") {
            return Result<SearchResponse>::success(std::move(response));
        }
        const auto unicode_length = static_cast<std::size_t>(std::count_if(
            query.begin(), query.end(), [](unsigned char byte) { return (byte & 0xC0U) != 0x80U; }
        ));
        const bool short_query = unicode_length <= 2;
        const char* sql = short_query
            ? "SELECT kind, document_version_id, note_id, page_index, title, "
              "substr(content, max(1, instr(lower(content), lower(?1)) - 40), 160) "
              "FROM search_index.entries WHERE instr(lower(content), lower(?1)) > 0 LIMIT ?2"
            : "SELECT kind, document_version_id, note_id, page_index, title, "
              "substr(content, max(1, instr(lower(content), lower(?2)) - 40), 160) "
              "FROM search_index.entries WHERE entries MATCH ?1 LIMIT ?3";
        auto statement_result = prepare(database_.get(), sql);
        if(!statement_result) return Result<SearchResponse>::failure(statement_result.error());
        auto statement = std::move(statement_result).value();
        std::string match_query;
        if(short_query) {
            if(!bind_text(statement.get(), 1, query)
               || sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
                return Result<SearchResponse>::failure(Error(ErrorCode::storage_failure, "Search query could not be bound"));
            }
        } else {
            match_query.push_back('"');
            for(const auto character : query) {
                if(character == '"') match_query.push_back('"');
                match_query.push_back(character);
            }
            match_query.push_back('"');
            if(!bind_text(statement.get(), 1, match_query) || !bind_text(statement.get(), 2, query)
               || sqlite3_bind_int64(statement.get(), 3, static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
                return Result<SearchResponse>::failure(Error(ErrorCode::storage_failure, "Search query could not be bound"));
            }
        }
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const auto* kind = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
            const auto* version = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
            const auto* note = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2));
            const auto* page = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 3));
            const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 4));
            const auto* excerpt = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 5));
            const auto version_id = version == nullptr ? std::nullopt : stable_id_from_hex<DocumentVersionIdTag>(version);
            if(kind == nullptr || !version_id || page == nullptr || title == nullptr || excerpt == nullptr) {
                return Result<SearchResponse>::failure(Error(ErrorCode::storage_failure, "Search index entry is invalid"));
            }
            SearchResultItem item{
                .kind = std::string_view(kind) == "note" ? SearchResultKind::note : SearchResultKind::pdf_page,
                .document_version_id = *version_id,
                .page_index = static_cast<std::size_t>(std::strtoull(page, nullptr, 10)),
                .title = title,
                .excerpt = excerpt,
            };
            if(note != nullptr && *note != '\0') {
                item.note_id = stable_id_from_hex<NoteIdTag>(note);
            }
            response.results.push_back(std::move(item));
        }
        if(step_result != SQLITE_DONE) {
            return Result<SearchResponse>::failure(Error(ErrorCode::storage_failure, "Search query failed"));
        }
        return Result<SearchResponse>::success(std::move(response));
    }

    [[nodiscard]] Result<WorkspaceVerification> verify() {
        const std::scoped_lock lock(mutex_);
        WorkspaceVerification verification{
            .valid = false,
            .document_count = 0,
            .document_version_count = 0,
            .referenced_object_count = 0,
            .orphaned_object_count = 0,
            .issues = {},
        };

        auto document_count = query_count(database_.get(), "SELECT count(*) FROM documents");
        auto version_count = query_count(database_.get(), "SELECT count(*) FROM document_versions");
        if(!document_count) {
            return Result<WorkspaceVerification>::failure(document_count.error());
        }
        if(!version_count) {
            return Result<WorkspaceVerification>::failure(version_count.error());
        }
        verification.document_count = document_count.value();
        verification.document_version_count = version_count.value();

        auto quick_check = prepare(database_.get(), "PRAGMA quick_check");
        if(!quick_check) {
            return Result<WorkspaceVerification>::failure(quick_check.error());
        }
        if(sqlite3_step(quick_check.value().get()) != SQLITE_ROW ||
           std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(quick_check.value().get(), 0))) !=
               "ok") {
            verification.issues.emplace_back("sqlite_quick_check_failed");
        }

        auto active_mismatch = query_count(
            database_.get(),
            "SELECT count(*) FROM documents d "
            "LEFT JOIN document_versions v ON v.id = d.active_version_id AND v.document_id = d.id "
            "WHERE d.active_version_id IS NULL OR v.id IS NULL"
        );
        if(!active_mismatch) {
            return Result<WorkspaceVerification>::failure(active_mismatch.error());
        }
        if(active_mismatch.value() != 0) {
            verification.issues.emplace_back("document_active_version_mismatch");
        }

        auto foreign_keys = prepare(database_.get(), "PRAGMA foreign_key_check");
        if(!foreign_keys) {
            return Result<WorkspaceVerification>::failure(foreign_keys.error());
        }
        const auto foreign_key_step = sqlite3_step(foreign_keys.value().get());
        if(foreign_key_step == SQLITE_ROW) {
            verification.issues.emplace_back("sqlite_foreign_key_violation");
        } else if(foreign_key_step != SQLITE_DONE) {
            return Result<WorkspaceVerification>::failure(
                Error(ErrorCode::storage_failure, "SQLite foreign key check failed")
            );
        }

        auto missing_quads = query_count(
            database_.get(),
            "SELECT count(*) FROM annotations a LEFT JOIN annotation_quads q ON q.annotation_id = a.id "
            "WHERE q.annotation_id IS NULL"
        );
        auto page_mismatch = query_count(
            database_.get(),
            "SELECT count(*) FROM annotations a JOIN document_versions v ON v.id = a.document_version_id "
            "WHERE a.page_index >= v.page_count"
        );
        if(!missing_quads || !page_mismatch) {
            return Result<WorkspaceVerification>::failure(
                !missing_quads ? missing_quads.error() : page_mismatch.error()
            );
        }
        if(missing_quads.value() != 0U) {
            verification.issues.emplace_back("annotation_anchor_missing_quads");
        }
        if(page_mismatch.value() != 0U) {
            verification.issues.emplace_back("annotation_page_out_of_range");
        }

        auto objects_result = prepare(
            database_.get(),
            "SELECT content_hash, object_key, byte_length FROM document_versions ORDER BY object_key"
        );
        if(!objects_result) {
            return Result<WorkspaceVerification>::failure(objects_result.error());
        }
        auto objects = std::move(objects_result).value();
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(objects.get())) == SQLITE_ROW) {
            ++verification.referenced_object_count;
            const auto* hash_text = reinterpret_cast<const char*>(sqlite3_column_text(objects.get(), 0));
            const auto* key_text = reinterpret_cast<const char*>(sqlite3_column_text(objects.get(), 1));
            const auto expected_size = sqlite3_column_int64(objects.get(), 2);
            if(hash_text == nullptr || key_text == nullptr || expected_size < 0) {
                verification.issues.emplace_back("document_object_metadata_invalid");
                continue;
            }

            const std::string content_hash(hash_text);
            const std::filesystem::path object_key(key_text);
            bool unsafe_path = object_key.is_absolute();
            for(const auto& component : object_key) {
                if(component == "..") {
                    unsafe_path = true;
                }
            }
            if(unsafe_path) {
                verification.issues.emplace_back("document_object_key_unsafe");
                continue;
            }

            const auto object_path = root_ / object_key;
            std::error_code filesystem_error;
            if(!std::filesystem::is_regular_file(object_path, filesystem_error) || filesystem_error) {
                verification.issues.emplace_back("document_object_missing:" + generic_utf8_path(object_key));
                continue;
            }
            const auto actual_size = std::filesystem::file_size(object_path, filesystem_error);
            if(filesystem_error || actual_size != static_cast<std::uintmax_t>(expected_size)) {
                verification.issues.emplace_back("document_object_size_mismatch:" + generic_utf8_path(object_key));
                continue;
            }
            auto actual_hash = sha256_file(object_path);
            if(!actual_hash) {
                return Result<WorkspaceVerification>::failure(actual_hash.error());
            }
            if(actual_hash.value() != content_hash) {
                verification.issues.emplace_back("document_object_hash_mismatch:" + generic_utf8_path(object_key));
            }
        }
        if(step_result != SQLITE_DONE) {
            return Result<WorkspaceVerification>::failure(
                Error(ErrorCode::storage_failure, "Document objects could not be verified")
            );
        }

        auto orphaned_objects = find_orphaned_objects();
        if(!orphaned_objects) {
            return Result<WorkspaceVerification>::failure(orphaned_objects.error());
        }
        verification.orphaned_object_count = orphaned_objects.value().size();
        for(const auto& path : orphaned_objects.value()) {
            verification.issues.emplace_back(
                "orphaned_document_object:" + generic_utf8_path(path.lexically_relative(root_))
            );
        }

        verification.valid = verification.issues.empty();
        return Result<WorkspaceVerification>::success(std::move(verification));
    }

    [[nodiscard]] Result<OrphanCleanupResult> cleanup_orphaned_objects() {
        const std::scoped_lock lock(mutex_);
        auto orphaned_objects = find_orphaned_objects();
        if(!orphaned_objects) {
            return Result<OrphanCleanupResult>::failure(orphaned_objects.error());
        }
        OrphanCleanupResult cleanup{.removed_object_count = 0, .reclaimed_bytes = 0};
        for(const auto& path : orphaned_objects.value()) {
            std::error_code filesystem_error;
            const auto size = std::filesystem::file_size(path, filesystem_error);
            if(filesystem_error || !std::filesystem::remove(path, filesystem_error)
               || filesystem_error) {
                return Result<OrphanCleanupResult>::failure(
                    Error(ErrorCode::storage_failure, "Orphaned PDF object could not be removed")
                );
            }
            ++cleanup.removed_object_count;
            cleanup.reclaimed_bytes += static_cast<std::uint64_t>(size);
        }
        return Result<OrphanCleanupResult>::success(cleanup);
    }

private:
    [[nodiscard]] Result<std::unordered_set<std::string>> referenced_object_keys() {
        auto statement_result = prepare(database_.get(), "SELECT object_key FROM document_versions");
        if(!statement_result) {
            return Result<std::unordered_set<std::string>>::failure(statement_result.error());
        }
        auto statement = std::move(statement_result).value();
        std::unordered_set<std::string> keys;
        int step_result = SQLITE_ROW;
        while((step_result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const auto* key = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
            if(key == nullptr) {
                return Result<std::unordered_set<std::string>>::failure(
                    Error(ErrorCode::storage_failure, "SQLite object key is invalid")
                );
            }
            keys.emplace(generic_utf8_path(std::filesystem::path(key).lexically_normal()));
        }
        if(step_result != SQLITE_DONE) {
            return Result<std::unordered_set<std::string>>::failure(
                Error(ErrorCode::storage_failure, "Referenced PDF objects could not be listed")
            );
        }
        return Result<std::unordered_set<std::string>>::success(std::move(keys));
    }

    [[nodiscard]] Result<std::vector<std::filesystem::path>> find_orphaned_objects() {
        auto referenced = referenced_object_keys();
        if(!referenced) {
            return Result<std::vector<std::filesystem::path>>::failure(referenced.error());
        }
        const auto object_root = root_ / "objects" / "pdf";
        std::error_code filesystem_error;
        if(!std::filesystem::exists(object_root, filesystem_error)) {
            if(filesystem_error) {
                return Result<std::vector<std::filesystem::path>>::failure(
                    Error(ErrorCode::storage_failure, "PDF object directory could not be inspected")
                );
            }
            return Result<std::vector<std::filesystem::path>>::success({});
        }

        std::vector<std::filesystem::path> orphaned;
        std::filesystem::recursive_directory_iterator iterator(object_root, filesystem_error);
        const std::filesystem::recursive_directory_iterator end;
        while(!filesystem_error && iterator != end) {
            const auto path = iterator->path();
            if(iterator->is_regular_file(filesystem_error)) {
                const auto key = generic_utf8_path(path.lexically_relative(root_).lexically_normal());
                if(!referenced.value().contains(key)) {
                    orphaned.push_back(path);
                }
            }
            iterator.increment(filesystem_error);
        }
        if(filesystem_error) {
            return Result<std::vector<std::filesystem::path>>::failure(
                Error(ErrorCode::storage_failure, "PDF object directory could not be inspected")
            );
        }
        std::sort(orphaned.begin(), orphaned.end());
        return Result<std::vector<std::filesystem::path>>::success(std::move(orphaned));
    }

    [[nodiscard]] Result<void> synchronize_note_assets(
        AnnotationId annotation_id,
        std::string_view markdown,
        std::vector<std::filesystem::path>& reclaimed
    ) {
        std::vector<AssetId> referenced;
        std::unordered_set<std::string> unique;
        constexpr std::string_view prefix = "reader-asset:";
        std::size_t offset = 0;
        while((offset = markdown.find(prefix, offset)) != std::string_view::npos) {
            const auto id_start = offset + prefix.size();
            if(id_start + 32 > markdown.size()) {
                return Result<void>::failure(Error(ErrorCode::invalid_argument, "Note contains an invalid reader-asset reference"));
            }
            const auto text = markdown.substr(id_start, 32);
            auto id = stable_id_from_hex<AssetIdTag>(text);
            if(!id) {
                return Result<void>::failure(Error(ErrorCode::invalid_argument, "Note contains an invalid reader-asset reference"));
            }
            const auto key = std::string(text);
            if(unique.insert(key).second) referenced.push_back(*id);
            offset = id_start + 32;
        }
        auto exists = prepare(database_.get(), "SELECT 1 FROM assets WHERE id = ?1");
        if(!exists) return Result<void>::failure(exists.error());
        for(const auto& id : referenced) {
            auto* statement = exists.value().get();
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if(!bind_id(statement, 1, id) || sqlite3_step(statement) != SQLITE_ROW) {
                return Result<void>::failure(Error(ErrorCode::invalid_argument, "Note references an unknown Asset ID"));
            }
        }
        auto remove_links = prepare(database_.get(), "DELETE FROM annotation_assets WHERE annotation_id = ?1");
        if(!remove_links || !bind_id(remove_links.value().get(), 1, annotation_id)
           || sqlite3_step(remove_links.value().get()) != SQLITE_DONE) {
            return Result<void>::failure(Error(ErrorCode::storage_failure, "Note asset references could not be reset"));
        }
        auto insert_link = prepare(
            database_.get(),
            "INSERT INTO annotation_assets(annotation_id, asset_id) VALUES(?1, ?2)"
        );
        if(!insert_link) return Result<void>::failure(insert_link.error());
        for(const auto& id : referenced) {
            auto* statement = insert_link.value().get();
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if(!bind_id(statement, 1, annotation_id) || !bind_id(statement, 2, id)
               || sqlite3_step(statement) != SQLITE_DONE) {
                return Result<void>::failure(Error(ErrorCode::storage_failure, "Note asset reference could not be stored"));
            }
        }
        auto orphaned = prepare(
            database_.get(),
            "SELECT object_key FROM assets a WHERE NOT EXISTS ("
            "SELECT 1 FROM annotation_assets aa WHERE aa.asset_id = a.id)"
        );
        if(!orphaned) return Result<void>::failure(orphaned.error());
        int step = SQLITE_ROW;
        while((step = sqlite3_step(orphaned.value().get())) == SQLITE_ROW) {
            const auto* key = reinterpret_cast<const char*>(sqlite3_column_text(orphaned.value().get(), 0));
            if(key == nullptr) return Result<void>::failure(Error(ErrorCode::storage_failure, "Orphaned asset path is invalid"));
            reclaimed.push_back(root_ / std::filesystem::path(key));
        }
        if(step != SQLITE_DONE || !execute(
            database_.get(),
            "DELETE FROM assets WHERE NOT EXISTS (SELECT 1 FROM annotation_assets aa WHERE aa.asset_id = assets.id)"
        )) {
            return Result<void>::failure(Error(ErrorCode::storage_failure, "Orphaned assets could not be reclaimed"));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<std::optional<DocumentRecord>> find_by_hash(std::string_view hash) {
        auto statement_result = prepare(
            database_.get(),
            "SELECT d.id, v.id, d.title, v.content_hash, v.object_key, d.active_version_id, "
            "v.byte_length, v.page_count "
            "FROM document_versions v JOIN documents d ON d.id = v.document_id "
            "WHERE v.content_hash = ?1 LIMIT 1"
        );
        if(!statement_result) {
            return Result<std::optional<DocumentRecord>>::failure(statement_result.error());
        }
        auto statement = std::move(statement_result).value();
        if(!bind_text(statement.get(), 1, hash)) {
            return Result<std::optional<DocumentRecord>>::failure(
                Error(ErrorCode::storage_failure, "Document hash could not be bound")
            );
        }
        const auto step_result = sqlite3_step(statement.get());
        if(step_result == SQLITE_DONE) {
            return Result<std::optional<DocumentRecord>>::success(std::nullopt);
        }
        if(step_result != SQLITE_ROW) {
            return Result<std::optional<DocumentRecord>>::failure(
                Error(ErrorCode::storage_failure, "Document hash lookup failed")
            );
        }
        auto record = read_document_record(statement.get());
        if(!record) {
            return Result<std::optional<DocumentRecord>>::failure(record.error());
        }
        return Result<std::optional<DocumentRecord>>::success(
            std::optional<DocumentRecord>(std::move(record).value())
        );
    }

    std::filesystem::path root_;
    WorkspaceLock workspace_lock_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database_;
    PdfEngine& pdf_engine_;
    WorkspaceInfo info_;
    std::mutex mutex_;
};

SqliteWorkspace::SqliteWorkspace(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

SqliteWorkspace::~SqliteWorkspace() = default;

Result<std::unique_ptr<SqliteWorkspace>> SqliteWorkspace::create(
    const std::filesystem::path& root,
    PdfEngine& pdf_engine
) {
    if(root.empty()) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::invalid_argument, "Workspace path is empty")
        );
    }
    std::error_code filesystem_error;
    const auto absolute_root = std::filesystem::absolute(root, filesystem_error);
    if(filesystem_error) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::storage_failure, "Workspace path could not be resolved")
        );
    }
    const auto database_path = absolute_root / "workspace.db";
    if(std::filesystem::exists(database_path, filesystem_error)) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::already_exists, "Workspace database already exists")
        );
    }

    for(const auto& directory : {
            absolute_root,
            absolute_root / "objects" / "pdf",
            absolute_root / "objects" / "assets",
            absolute_root / "cache" / "render",
            absolute_root / "cache" / "thumbnail",
            absolute_root / "backups",
        }) {
        std::filesystem::create_directories(directory, filesystem_error);
        if(filesystem_error) {
            return Result<std::unique_ptr<SqliteWorkspace>>::failure(
                Error(ErrorCode::storage_failure, "Workspace directory could not be created")
            );
        }
    }

    auto lock_result = acquire_workspace_lock(absolute_root);
    if(!lock_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(lock_result.error());
    }
    auto workspace_lock = std::move(lock_result).value();
    if(std::filesystem::exists(database_path, filesystem_error)) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::already_exists, "Workspace database already exists")
        );
    }

    auto database_result = open_database(database_path, true);
    if(!database_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(database_result.error());
    }
    auto database = std::move(database_result).value();
    auto configuration = configure_database(database.get());
    if(!configuration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(configuration.error());
    }
    auto migration = apply_initial_migration(database.get());
    if(!migration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(migration.error());
    }
    auto info_result = read_workspace_info(database.get());
    if(!info_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(info_result.error());
    }
    auto index_result = attach_search_index(database.get(), absolute_root / "index.db");
    if(!index_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(index_result.error());
    }

    return Result<std::unique_ptr<SqliteWorkspace>>::success(
        std::unique_ptr<SqliteWorkspace>(new SqliteWorkspace(std::make_unique<Impl>(
            absolute_root,
            std::move(workspace_lock),
            std::move(database),
            pdf_engine,
            info_result.value()
        )))
    );
}

Result<std::unique_ptr<SqliteWorkspace>> SqliteWorkspace::open(
    const std::filesystem::path& root,
    PdfEngine& pdf_engine
) {
    if(root.empty()) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::invalid_argument, "Workspace path is empty")
        );
    }
    std::error_code filesystem_error;
    const auto absolute_root = std::filesystem::absolute(root, filesystem_error);
    const auto database_path = absolute_root / "workspace.db";
    if(filesystem_error || !std::filesystem::is_regular_file(database_path, filesystem_error)) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(
            Error(ErrorCode::not_found, "Workspace database was not found")
        );
    }

    auto lock_result = acquire_workspace_lock(absolute_root);
    if(!lock_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(lock_result.error());
    }
    auto workspace_lock = std::move(lock_result).value();

    auto database_result = open_database(database_path, false);
    if(!database_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(database_result.error());
    }
    auto database = std::move(database_result).value();
    auto configuration = configure_database(database.get());
    if(!configuration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(configuration.error());
    }
    auto version_result = query_count(database.get(), "PRAGMA user_version");
    if(!version_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(version_result.error());
    }
    auto migration = migrate_database(database.get());
    if(!migration) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(migration.error());
    }
    auto info_result = read_workspace_info(database.get());
    if(!info_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(info_result.error());
    }
    auto index_result = attach_search_index(database.get(), absolute_root / "index.db");
    if(!index_result) {
        return Result<std::unique_ptr<SqliteWorkspace>>::failure(index_result.error());
    }

    return Result<std::unique_ptr<SqliteWorkspace>>::success(
        std::unique_ptr<SqliteWorkspace>(new SqliteWorkspace(std::make_unique<Impl>(
            absolute_root,
            std::move(workspace_lock),
            std::move(database),
            pdf_engine,
            info_result.value()
        )))
    );
}

Result<WorkspaceInspection> SqliteWorkspace::inspect(const std::filesystem::path& root) {
    if(root.empty()) {
        return Result<WorkspaceInspection>::failure(
            Error(ErrorCode::invalid_argument, "Workspace path is empty")
        );
    }
    std::error_code filesystem_error;
    const auto absolute_root = std::filesystem::absolute(root, filesystem_error);
    const auto database_path = absolute_root / "workspace.db";
    if(filesystem_error || !std::filesystem::is_regular_file(database_path, filesystem_error)) {
        return Result<WorkspaceInspection>::failure(
            Error(ErrorCode::not_found, "Workspace database was not found")
        );
    }
    auto lock_result = acquire_workspace_lock(absolute_root);
    if(!lock_result) {
        return Result<WorkspaceInspection>::failure(lock_result.error());
    }

    sqlite3* raw_database = nullptr;
    const auto path = utf8_path(database_path);
    if(sqlite3_open_v2(
           path.c_str(),
           &raw_database,
           SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
           nullptr
       ) != SQLITE_OK) {
        if(raw_database != nullptr) sqlite3_close_v2(raw_database);
        return Result<WorkspaceInspection>::failure(
            Error(ErrorCode::storage_failure, "Workspace database could not be inspected")
        );
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(
        raw_database,
        sqlite3_close_v2
    );
    auto version_result = query_count(database.get(), "PRAGMA user_version");
    if(!version_result || version_result.value() > std::numeric_limits<std::uint32_t>::max()) {
        return Result<WorkspaceInspection>::failure(
            version_result ? Error(ErrorCode::storage_failure, "Workspace schema version is invalid")
                           : version_result.error()
        );
    }
    auto metadata_result = prepare(
        database.get(),
        "SELECT workspace_id, schema_version FROM workspace_metadata WHERE singleton = 1"
    );
    if(!metadata_result || sqlite3_step(metadata_result.value().get()) != SQLITE_ROW) {
        return Result<WorkspaceInspection>::failure(
            Error(ErrorCode::storage_failure, "Workspace metadata is missing")
        );
    }
    auto workspace_id = read_id<WorkspaceId>(metadata_result.value().get(), 0);
    const auto metadata_version = sqlite3_column_int64(metadata_result.value().get(), 1);
    if(!workspace_id || metadata_version < 0
       || static_cast<std::uint64_t>(metadata_version) != version_result.value()) {
        return Result<WorkspaceInspection>::failure(
            Error(ErrorCode::storage_failure, "Workspace schema metadata does not match")
        );
    }
    const auto version = static_cast<std::uint32_t>(version_result.value());
    if(version != current_schema_version) {
        return Result<WorkspaceInspection>::failure(
            Error(ErrorCode::unsupported_document, "Workspace schema version is unsupported")
        );
    }
    return Result<WorkspaceInspection>::success(WorkspaceInspection{
        .id = workspace_id.value(),
        .schema_version = version,
        .target_schema_version = current_schema_version,
        .migration_required = false,
    });
}

Result<BackupInspection> SqliteWorkspace::inspect_package(
    const std::filesystem::path& package_path
) {
    std::error_code filesystem_error;
    const auto temporary_root = std::filesystem::temp_directory_path(filesystem_error) /
        ("context-reader-package-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if(filesystem_error) {
        return Result<BackupInspection>::failure(Error(ErrorCode::storage_failure, "Temporary directory is unavailable"));
    }
    auto result = validate_and_extract_package(package_path, temporary_root);
    std::filesystem::remove_all(temporary_root, filesystem_error);
    return result;
}

Result<WorkspaceInfo> SqliteWorkspace::restore_package(
    const std::filesystem::path& package_path,
    const std::filesystem::path& empty_target
) {
    if(empty_target.empty()) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::invalid_argument, "Restore target is empty"));
    }
    std::error_code filesystem_error;
    const auto absolute_target = std::filesystem::absolute(empty_target, filesystem_error);
    if(filesystem_error) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::storage_failure, "Restore target could not be resolved"));
    }
    if(std::filesystem::exists(absolute_target, filesystem_error)
       && (!std::filesystem::is_directory(absolute_target, filesystem_error)
           || !std::filesystem::is_empty(absolute_target, filesystem_error))) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::conflict, "Restore target must be an empty directory"));
    }
    std::filesystem::create_directories(absolute_target.parent_path(), filesystem_error);
    if(filesystem_error) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::storage_failure, "Restore parent directory could not be created"));
    }
    auto temporary_root = absolute_target;
    temporary_root += ".restore-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if(std::filesystem::exists(temporary_root, filesystem_error)) {
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::conflict, "Restore staging directory already exists"));
    }
    auto inspection = validate_and_extract_package(package_path, temporary_root);
    if(!inspection) {
        std::filesystem::remove_all(temporary_root, filesystem_error);
        return Result<WorkspaceInfo>::failure(inspection.error());
    }
    const auto space = std::filesystem::space(absolute_target.parent_path(), filesystem_error);
    if(filesystem_error || space.available < inspection.value().total_uncompressed_bytes) {
        std::filesystem::remove_all(temporary_root, filesystem_error);
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::resource_exhausted, "Insufficient disk space for workspace restore"));
    }
    auto database = open_database(temporary_root / "workspace.db", false);
    if(!database) {
        std::filesystem::remove_all(temporary_root, filesystem_error);
        return Result<WorkspaceInfo>::failure(database.error());
    }
    auto info = read_workspace_info(database.value().get());
    database.value().reset();
    if(!info) {
        std::filesystem::remove_all(temporary_root, filesystem_error);
        return Result<WorkspaceInfo>::failure(info.error());
    }
    if(std::filesystem::exists(absolute_target, filesystem_error)) {
        std::filesystem::remove(absolute_target, filesystem_error);
        if(filesystem_error) {
            std::filesystem::remove_all(temporary_root, filesystem_error);
            return Result<WorkspaceInfo>::failure(Error(ErrorCode::storage_failure, "Empty restore target could not be prepared"));
        }
    }
    std::filesystem::rename(temporary_root, absolute_target, filesystem_error);
    if(filesystem_error) {
        std::filesystem::remove_all(temporary_root, filesystem_error);
        return Result<WorkspaceInfo>::failure(Error(ErrorCode::storage_failure, "Restored workspace could not be committed"));
    }
    return info;
}

WorkspaceInfo SqliteWorkspace::info() const noexcept {
    return implementation_->info();
}

Result<ImportDocumentResult> SqliteWorkspace::import_pdf(
    const std::filesystem::path& source,
    const CancellationToken& cancellation
) {
    return implementation_->import_pdf(source, cancellation);
}

Result<std::vector<DocumentRecord>> SqliteWorkspace::list_documents() {
    return implementation_->list_documents();
}

Result<ResolvedDocumentObject> SqliteWorkspace::resolve_document(DocumentId document_id) {
    return implementation_->resolve_document(document_id);
}

Result<AnnotationRecord> SqliteWorkspace::create_annotation(const CreateAnnotation& command) {
    return implementation_->create_annotation(command);
}

Result<std::vector<AnnotationRecord>> SqliteWorkspace::list_annotations(
    DocumentVersionId document_version_id
) {
    return implementation_->list_annotations(document_version_id);
}

Result<void> SqliteWorkspace::delete_annotation(AnnotationId annotation_id) {
    return implementation_->delete_annotation(annotation_id);
}

Result<NoteRecord> SqliteWorkspace::update_note(const UpdateNote& command) {
    return implementation_->update_note(command);
}

Result<std::vector<NoteRecord>> SqliteWorkspace::list_notes(
    DocumentVersionId document_version_id
) {
    return implementation_->list_notes(document_version_id);
}

Result<void> SqliteWorkspace::rebuild_search_index(const CancellationToken& cancellation) {
    return implementation_->rebuild_search_index(cancellation);
}

Result<SearchResponse> SqliteWorkspace::search(std::string_view query, std::size_t limit) {
    return implementation_->search(query, limit);
}

Result<AssetRecord> SqliteWorkspace::import_note_asset(
    AnnotationId annotation_id,
    const std::filesystem::path& source,
    const CancellationToken& cancellation
) {
    return implementation_->import_note_asset(annotation_id, source, cancellation);
}

Result<AssetData> SqliteWorkspace::read_asset(AssetId asset_id) {
    return implementation_->read_asset(asset_id);
}

Result<BackupInspection> SqliteWorkspace::export_package(
    const std::filesystem::path& destination,
    const CancellationToken& cancellation
) {
    return implementation_->export_package(destination, cancellation);
}

Result<WorkspaceVerification> SqliteWorkspace::verify() {
    return implementation_->verify();
}

Result<OrphanCleanupResult> SqliteWorkspace::cleanup_orphaned_objects() {
    return implementation_->cleanup_orphaned_objects();
}

}  // namespace context_reader
