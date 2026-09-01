#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "context_reader/pdf/mupdf_engine.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/shared/stable_id.hpp"
#include "context_reader/workspace/sqlite_workspace.hpp"

namespace {

using namespace context_reader;

std::string json_string(std::string_view value) {
    std::string result = "\"";
    for(const char character : value) {
        switch(character) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += character; break;
        }
    }
    result += '"';
    return result;
}

const char* error_code_name(ErrorCode code) {
    switch(code) {
        case ErrorCode::invalid_argument: return "INVALID_ARGUMENT";
        case ErrorCode::not_found: return "NOT_FOUND";
        case ErrorCode::already_exists: return "ALREADY_EXISTS";
        case ErrorCode::conflict: return "CONFLICT";
        case ErrorCode::workspace_busy: return "WORKSPACE_BUSY";
        case ErrorCode::unsupported_document: return "UNSUPPORTED_DOCUMENT";
        case ErrorCode::password_required: return "PASSWORD_REQUIRED";
        case ErrorCode::cancelled: return "CANCELLED";
        case ErrorCode::resource_exhausted: return "RESOURCE_EXHAUSTED";
        case ErrorCode::storage_failure: return "STORAGE_FAILURE";
        case ErrorCode::pdf_failure: return "PDF_FAILURE";
        case ErrorCode::internal: return "INTERNAL";
    }
    return "INTERNAL";
}

int print_error(const Error& error) {
    std::cerr << "{\"ok\":false,\"error\":{\"code\":"
              << json_string(error_code_name(error.code())) << ",\"message\":"
              << json_string(error.message()) << "}}\n";
    return error.code() == ErrorCode::workspace_busy ? 4 : 3;
}

void print_inspection(std::string_view command, const WorkspaceInspection& inspection) {
    std::cout << "{\"ok\":true,\"command\":" << json_string(command)
              << ",\"workspaceId\":" << json_string(stable_id_to_hex(inspection.id))
              << ",\"schemaVersion\":" << inspection.schema_version
              << ",\"targetSchemaVersion\":" << inspection.target_schema_version
              << ",\"migrationRequired\":"
              << (inspection.migration_required ? "true" : "false") << "}\n";
}

int usage() {
    std::cerr << "Usage: reader-workspace inspect <workspace>\n"
                 "       reader-workspace verify <workspace>\n"
                 "       reader-workspace verify-package <backup.readerpkg>\n";
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if(argc < 3) return usage();
    const std::wstring command(argv[1]);
    const std::filesystem::path workspace(argv[2]);

    if(command == L"inspect") {
        if(argc != 3) return usage();
        auto inspection = SqliteWorkspace::inspect(workspace);
        if(!inspection) return print_error(inspection.error());
        print_inspection("inspect", inspection.value());
        return 0;
    }
    if(command == L"verify-package") {
        if(argc != 3) return usage();
        auto inspection = SqliteWorkspace::inspect_package(workspace);
        if(!inspection) return print_error(inspection.error());
        std::cout << "{\"ok\":true,\"command\":\"verify-package\",\"valid\":"
                  << (inspection.value().valid ? "true" : "false")
                  << ",\"formatVersion\":" << inspection.value().format_version
                  << ",\"fileCount\":" << inspection.value().file_count
                  << ",\"totalUncompressedBytes\":" << inspection.value().total_uncompressed_bytes
                  << "}\n";
        return inspection.value().valid ? 0 : 5;
    }
    if(command == L"verify") {
        if(argc != 3) return usage();
        MuPdfEngine pdf_engine;
        auto opened = SqliteWorkspace::open(workspace, pdf_engine);
        if(!opened) return print_error(opened.error());
        auto verification = opened.value()->verify();
        if(!verification) return print_error(verification.error());
        const auto& result = verification.value();
        std::cout << "{\"ok\":true,\"command\":\"verify\",\"valid\":"
                  << (result.valid ? "true" : "false")
                  << ",\"documentCount\":" << result.document_count
                  << ",\"documentVersionCount\":" << result.document_version_count
                  << ",\"referencedObjectCount\":" << result.referenced_object_count
                  << ",\"orphanedObjectCount\":" << result.orphaned_object_count
                  << ",\"issues\":[";
        for(std::size_t index = 0; index < result.issues.size(); ++index) {
            if(index != 0U) std::cout << ',';
            std::cout << json_string(result.issues[index]);
        }
        std::cout << "]}\n";
        return result.valid ? 0 : 5;
    }
    return usage();
}
