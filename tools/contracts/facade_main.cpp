#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "context_reader/annotation/annotation.hpp"
#include "context_reader/runtime/reader_runtime.hpp"
#include "context_reader/shared/error.hpp"

namespace {

using namespace context_reader;

const char* error_code_name(ErrorCode code) {
    switch(code) {
        case ErrorCode::not_found: return "NOT_FOUND";
        case ErrorCode::conflict: return "CONFLICT";
        case ErrorCode::cancelled: return "CANCELLED";
        case ErrorCode::invalid_argument: return "INVALID_ARGUMENT";
        default: return "OTHER";
    }
}

template <typename T>
bool require_result(const Result<T>& result, std::string_view operation) {
    if(result) return true;
    std::cerr << operation << " failed: " << result.error().message() << '\n';
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace context_reader;
    if(argc != 3) {
        std::cerr << "Usage: reader-facade-contract <workspace> <fixture.pdf>\n";
        return 2;
    }

    auto runtime_result = ReaderRuntime::create();
    if(!require_result(runtime_result, "create runtime")) return 1;
    auto runtime = std::move(runtime_result).value();
    auto& app = runtime->application();
    const auto workspace = app.create_workspace(std::filesystem::path(argv[1]));
    if(!require_result(workspace, "create workspace")) return 1;
    const auto imported = app.import_document(std::filesystem::path(argv[2]));
    if(!require_result(imported, "import document")) return 1;
    const auto documents = app.list_documents();
    if(!require_result(documents, "list documents")) return 1;
    const auto opened = app.open_document(imported.value().document.document_id);
    if(!require_result(opened, "open document")) return 1;
    const auto rendered = app.render_page(0U, 1.0);
    if(!require_result(rendered, "render page")) return 1;
    constexpr std::array<std::uint8_t, 8> png_signature{
        137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U,
    };
    const bool png_valid = rendered.value().png.size() >= png_signature.size()
        && std::equal(
            png_signature.begin(),
            png_signature.end(),
            rendered.value().png.begin()
        );
    const auto annotation = app.create_annotation(CreateAnnotation{
        .document_version_id = imported.value().document.version_id,
        .page_index = 0U,
        .quads = {{.x = 72.0, .y = 96.0, .width = 180.0, .height = 18.0}},
        .quote = {.exact = "Context Reader P1", .prefix = "", .suffix = " fixture"},
        .layout_version = "mupdf-1.28.3",
        .color = HighlightColor::yellow,
    });
    if(!require_result(annotation, "create annotation")) return 1;
    const auto note = app.update_note(UpdateNote{
        .annotation_id = annotation.value().id,
        .expected_revision = 0U,
        .markdown_source = "Boundary behavior contract",
    });
    if(!require_result(note, "create note")) return 1;
    const auto annotations = app.list_annotations(imported.value().document.version_id);
    const auto notes = app.list_notes(imported.value().document.version_id);
    const auto verification = app.verify_workspace();
    if(!require_result(annotations, "list annotations") || !require_result(notes, "list notes")
       || !require_result(verification, "verify workspace")) {
        return 1;
    }
    if(!app.close_document() || !app.close_workspace()) return 1;
    const auto closed_document = app.page_info(0U);
    const auto closed_workspace = app.list_documents();
    if(closed_document || closed_workspace) {
        std::cerr << "closed application boundary accepted an operation\n";
        return 1;
    }

    std::cout << "{\"runtimeApiVersion\":" << app.runtime_info().application_api_version
              << ",\"workspaceSchemaVersion\":" << workspace.value().schema_version
              << ",\"documentCount\":" << documents.value().size()
              << ",\"pngValid\":" << (png_valid ? "true" : "false")
              << ",\"annotationCount\":" << annotations.value().size()
              << ",\"noteCount\":" << notes.value().size()
              << ",\"noteRevision\":" << note.value().revision
              << ",\"workspaceValid\":" << (verification.value().valid ? "true" : "false")
              << ",\"closedDocumentCode\":\""
              << error_code_name(closed_document.error().code())
              << "\",\"closedWorkspaceCode\":\""
              << error_code_name(closed_workspace.error().code()) << "\"}\n";
    return 0;
}
