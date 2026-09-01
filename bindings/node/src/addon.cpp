#include <napi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <unordered_map>

#include "context_reader/runtime/reader_runtime.hpp"

namespace {

using namespace context_reader;

enum class Operation {
    create_workspace,
    open_workspace,
    close_workspace,
    import_document,
    list_documents,
    open_document,
    close_document,
    page_info,
    render_page,
    render_tile,
    extract_page_text,
    page_text_layout,
    select_text,
    create_annotation,
    list_annotations,
    delete_annotation,
    update_note,
    list_notes,
    rebuild_search_index,
    search,
    import_note_asset,
    read_asset,
    export_workspace,
    inspect_backup,
    restore_workspace,
    verify_workspace,
};

struct RuntimeState final {
    explicit RuntimeState(std::unique_ptr<ReaderRuntime> value) : runtime(std::move(value)) {}

    struct Job final {
        CancellationSource cancellation;
        napi_env environment = nullptr;
        napi_async_work work = nullptr;
    };

    [[nodiscard]] bool register_job(
        const std::string& id,
        const CancellationSource& cancellation,
        napi_env environment,
        napi_async_work work
    ) {
        const std::scoped_lock lock(jobs_mutex);
        return jobs.emplace(
            id,
            Job{.cancellation = cancellation, .environment = environment, .work = work}
        ).second;
    }

    [[nodiscard]] bool cancel_job(const std::string& id) {
        const std::scoped_lock lock(jobs_mutex);
        const auto found = jobs.find(id);
        if(found == jobs.end()) return false;
        found->second.cancellation.request_cancellation();
        if(found->second.work != nullptr) {
            static_cast<void>(napi_cancel_async_work(
                found->second.environment,
                found->second.work
            ));
        }
        return true;
    }

    void finish_job(const std::string& id) {
        const std::scoped_lock lock(jobs_mutex);
        jobs.erase(id);
    }

    std::unique_ptr<ReaderRuntime> runtime;
    std::mutex jobs_mutex;
    std::unordered_map<std::string, Job> jobs;
};

struct AddonContext;
struct FunctionData final {
    AddonContext* context;
    Operation operation;
};

struct AddonContext final {
    explicit AddonContext(std::shared_ptr<RuntimeState> value)
        : state(std::move(value)),
          functions{{
              {this, Operation::create_workspace},
              {this, Operation::open_workspace},
              {this, Operation::close_workspace},
              {this, Operation::import_document},
              {this, Operation::list_documents},
              {this, Operation::open_document},
              {this, Operation::close_document},
              {this, Operation::page_info},
              {this, Operation::render_page},
              {this, Operation::render_tile},
              {this, Operation::extract_page_text},
              {this, Operation::page_text_layout},
              {this, Operation::select_text},
              {this, Operation::create_annotation},
              {this, Operation::list_annotations},
              {this, Operation::delete_annotation},
              {this, Operation::update_note},
              {this, Operation::list_notes},
              {this, Operation::rebuild_search_index},
              {this, Operation::search},
              {this, Operation::import_note_asset},
              {this, Operation::read_asset},
              {this, Operation::export_workspace},
              {this, Operation::inspect_backup},
              {this, Operation::restore_workspace},
              {this, Operation::verify_workspace},
          }} {}

    std::shared_ptr<RuntimeState> state;
    std::array<FunctionData, 26> functions;
};

using Payload = std::variant<
    std::monostate,
    WorkspaceInfo,
    ImportDocumentResult,
    DocumentRecord,
    std::vector<DocumentRecord>,
    PageInfo,
    EncodedPageImage,
    RenderedTile,
    PageText,
    PageTextLayout,
    TextSelection,
    AnnotationRecord,
    std::vector<AnnotationRecord>,
    NoteRecord,
    std::vector<NoteRecord>,
    SearchResponse,
    AssetRecord,
    AssetData,
    BackupInspection,
    WorkspaceVerification>;

struct AsyncWork final {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::shared_ptr<RuntimeState> state;
    Operation operation;
    std::filesystem::path path;
    std::filesystem::path target_path;
    std::optional<DocumentId> document_id;
    std::optional<DocumentVersionId> document_version_id;
    std::optional<AnnotationId> annotation_id;
    std::optional<AssetId> asset_id;
    std::optional<CreateAnnotation> annotation;
    std::optional<UpdateNote> note;
    std::string search_query;
    std::size_t search_limit = 50;
    std::size_t page_index = 0;
    double pixels_per_point = 1.0;
    std::optional<TileRequest> tile_request;
    PagePoint start_point{};
    PagePoint end_point{};
    CancellationToken cancellation;
    std::optional<std::string> job_id;
    Payload payload;
    std::optional<Error> error;
};

bool set_property(napi_env env, napi_value object, const char* name, napi_value value) {
    return napi_set_named_property(env, object, name, value) == napi_ok;
}

bool make_string(napi_env env, const std::string& value, napi_value* result) {
    return napi_create_string_utf8(env, value.data(), value.size(), result) == napi_ok;
}

bool set_string(napi_env env, napi_value object, const char* name, const std::string& value) {
    napi_value property = nullptr;
    return make_string(env, value, &property) && set_property(env, object, name, property);
}

bool set_number(napi_env env, napi_value object, const char* name, std::uint64_t value) {
    napi_value property = nullptr;
    return napi_create_double(env, static_cast<double>(value), &property) == napi_ok
        && set_property(env, object, name, property);
}

bool set_double(napi_env env, napi_value object, const char* name, double value) {
    napi_value property = nullptr;
    return napi_create_double(env, value, &property) == napi_ok
        && set_property(env, object, name, property);
}

bool set_boolean(napi_env env, napi_value object, const char* name, bool value) {
    napi_value property = nullptr;
    return napi_get_boolean(env, value, &property) == napi_ok
        && set_property(env, object, name, property);
}

const char* error_code(ErrorCode code) {
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

std::filesystem::path utf8_path(const std::string& value) {
#if defined(_WIN32)
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()),
        reinterpret_cast<const char8_t*>(value.data() + value.size())
    ));
#else
    return std::filesystem::path(value);
#endif
}

napi_value workspace_value(napi_env env, const WorkspaceInfo& info) {
    napi_value result = nullptr;
    napi_value version = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "id", stable_id_to_hex(info.id))
       || napi_create_uint32(env, info.schema_version, &version) != napi_ok
       || !set_property(env, result, "schemaVersion", version)) {
        return nullptr;
    }
    return result;
}

napi_value document_value(napi_env env, const DocumentRecord& document) {
    napi_value result = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "documentId", stable_id_to_hex(document.document_id))
       || !set_string(env, result, "versionId", stable_id_to_hex(document.version_id))
       || !set_string(env, result, "title", document.title)
       || !set_string(env, result, "contentSha256", document.content_sha256)
       || !set_string(env, result, "objectKey", document.object_key)
       || !set_number(env, result, "byteLength", document.byte_length)
       || !set_number(env, result, "pageCount", document.page_count)) {
        return nullptr;
    }
    return result;
}

napi_value import_value(napi_env env, const ImportDocumentResult& imported) {
    napi_value result = nullptr;
    const auto document = document_value(env, imported.document);
    if(document == nullptr || napi_create_object(env, &result) != napi_ok
       || !set_property(env, result, "document", document)
       || !set_boolean(env, result, "reusedExisting", imported.reused_existing)) {
        return nullptr;
    }
    return result;
}

napi_value documents_value(napi_env env, const std::vector<DocumentRecord>& documents) {
    napi_value result = nullptr;
    if(documents.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_array_with_length(env, documents.size(), &result) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < documents.size(); ++index) {
        const auto document = document_value(env, documents[index]);
        if(document == nullptr
           || napi_set_element(env, result, static_cast<std::uint32_t>(index), document)
                  != napi_ok) {
            return nullptr;
        }
    }
    return result;
}

napi_value page_info_value(napi_env env, const PageInfo& page) {
    napi_value result = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_number(env, result, "index", page.index)
       || !set_double(env, result, "widthPoints", page.size.width)
       || !set_double(env, result, "heightPoints", page.size.height)
       || !set_number(
           env,
           result,
           "rotation",
           static_cast<std::uint16_t>(page.rotation)
       )) {
        return nullptr;
    }
    return result;
}

napi_value rendered_page_value(napi_env env, const EncodedPageImage& rendered) {
    napi_value result = nullptr;
    napi_value png = nullptr;
    void* copied_data = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_number(env, result, "widthPixels", rendered.width_pixels)
       || !set_number(env, result, "heightPixels", rendered.height_pixels)
       || !set_double(env, result, "pixelsPerPoint", rendered.pixels_per_point)
       || napi_create_buffer_copy(
           env,
           rendered.png.size(),
           rendered.png.data(),
           &copied_data,
           &png
       ) != napi_ok
       || !set_property(env, result, "png", png)) {
        return nullptr;
    }
    return result;
}

napi_value rendered_tile_value(napi_env env, const RenderedTile& rendered) {
    napi_value result = nullptr;
    napi_value rgba = nullptr;
    void* copied_data = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_number(env, result, "pageIndex", rendered.page_index)
       || !set_number(env, result, "xPixels", rendered.x_pixels)
       || !set_number(env, result, "yPixels", rendered.y_pixels)
       || !set_number(env, result, "widthPixels", rendered.width_pixels)
       || !set_number(env, result, "heightPixels", rendered.height_pixels)
       || !set_double(env, result, "pixelsPerPoint", rendered.pixels_per_point)
       || !set_number(env, result, "generation", rendered.generation)
       || napi_create_arraybuffer(env, rendered.rgba.size(), &copied_data, &rgba) != napi_ok) {
        return nullptr;
    }
    if(!rendered.rgba.empty()) {
        std::copy(rendered.rgba.begin(), rendered.rgba.end(), static_cast<std::uint8_t*>(copied_data));
    }
    return set_property(env, result, "rgba", rgba) ? result : nullptr;
}

napi_value rect_value(napi_env env, const PageRect& bounds) {
    napi_value result = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_double(env, result, "x", bounds.x)
       || !set_double(env, result, "y", bounds.y)
       || !set_double(env, result, "width", bounds.width)
       || !set_double(env, result, "height", bounds.height)) {
        return nullptr;
    }
    return result;
}

napi_value page_text_value(napi_env env, const PageText& page_text) {
    napi_value result = nullptr;
    napi_value lines = nullptr;
    if(page_text.lines.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "text", page_text.text)
       || napi_create_array_with_length(env, page_text.lines.size(), &lines) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < page_text.lines.size(); ++index) {
        const auto& source = page_text.lines[index];
        napi_value line = nullptr;
        const auto bounds = rect_value(env, source.bounds);
        if(bounds == nullptr || napi_create_object(env, &line) != napi_ok
           || !set_string(env, line, "text", source.text)
           || !set_property(env, line, "bounds", bounds)
           || !set_boolean(env, line, "vertical", source.vertical)
           || napi_set_element(env, lines, static_cast<std::uint32_t>(index), line)
                  != napi_ok) {
            return nullptr;
        }
    }
    return set_property(env, result, "lines", lines) ? result : nullptr;
}

const char* direction_name(TextDirection direction) {
    switch(direction) {
        case TextDirection::left_to_right: return "ltr";
        case TextDirection::right_to_left: return "rtl";
        case TextDirection::top_to_bottom: return "ttb";
    }
    return "ltr";
}

napi_value point_value(napi_env env, PagePoint point) {
    napi_value result = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_double(env, result, "x", point.x)
       || !set_double(env, result, "y", point.y)) {
        return nullptr;
    }
    return result;
}

napi_value page_quad_value(napi_env env, const PageQuad& quad) {
    napi_value result = nullptr;
    const auto upper_left = point_value(env, quad.upper_left);
    const auto upper_right = point_value(env, quad.upper_right);
    const auto lower_left = point_value(env, quad.lower_left);
    const auto lower_right = point_value(env, quad.lower_right);
    if(upper_left == nullptr || upper_right == nullptr || lower_left == nullptr || lower_right == nullptr
       || napi_create_object(env, &result) != napi_ok
       || !set_property(env, result, "upperLeft", upper_left)
       || !set_property(env, result, "upperRight", upper_right)
       || !set_property(env, result, "lowerLeft", lower_left)
       || !set_property(env, result, "lowerRight", lower_right)) {
        return nullptr;
    }
    return result;
}

napi_value page_text_layout_value(napi_env env, const PageTextLayout& layout) {
    napi_value result = nullptr;
    napi_value units = nullptr;
    napi_value lines = nullptr;
    if(layout.units.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_object(env, &result) != napi_ok
       || !set_number(env, result, "pageIndex", layout.page_index)
       || !set_number(env, result, "layoutVersion", layout.layout_version)
       || !set_string(env, result, "text", layout.text)
       || napi_create_array_with_length(env, layout.units.size(), &units) != napi_ok
       || napi_create_array_with_length(env, layout.lines.size(), &lines) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < layout.units.size(); ++index) {
        const auto& source = layout.units[index];
        napi_value unit = nullptr;
        const auto quad = page_quad_value(env, source.quad);
        if(quad == nullptr || napi_create_object(env, &unit) != napi_ok
           || !set_number(env, unit, "logicalStart", source.logical_start)
           || !set_number(env, unit, "logicalEnd", source.logical_end)
           || !set_string(env, unit, "text", source.text)
           || !set_string(env, unit, "direction", direction_name(source.direction))
           || !set_number(env, unit, "lineIndex", source.line_index)
           || !set_property(env, unit, "quad", quad)
           || napi_set_element(env, units, static_cast<std::uint32_t>(index), unit) != napi_ok) {
            return nullptr;
        }
    }
    for(std::size_t index = 0; index < layout.lines.size(); ++index) {
        napi_value line = nullptr;
        const auto bounds = rect_value(env, layout.lines[index].bounds);
        if(bounds == nullptr || napi_create_object(env, &line) != napi_ok
           || !set_string(env, line, "text", layout.lines[index].text)
           || !set_property(env, line, "bounds", bounds)
           || !set_boolean(env, line, "vertical", layout.lines[index].vertical)
           || napi_set_element(env, lines, static_cast<std::uint32_t>(index), line) != napi_ok) {
            return nullptr;
        }
    }
    return set_property(env, result, "units", units)
        && set_property(env, result, "lines", lines) ? result : nullptr;
}

napi_value text_selection_value(napi_env env, const TextSelection& selection) {
    napi_value result = nullptr;
    napi_value quads = nullptr;
    napi_value quote = nullptr;
    if(selection.quads.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_object(env, &result) != napi_ok
       || !set_number(env, result, "pageIndex", selection.page_index)
       || !set_number(env, result, "layoutVersion", selection.layout_version)
       || !set_number(env, result, "logicalStart", selection.logical_start)
       || !set_number(env, result, "logicalEnd", selection.logical_end)
       || !set_string(env, result, "direction", direction_name(selection.direction))
       || !set_string(env, result, "text", selection.text)
       || napi_create_array_with_length(env, selection.quads.size(), &quads) != napi_ok
       || napi_create_object(env, &quote) != napi_ok
       || !set_string(env, quote, "exact", selection.text)
       || !set_string(env, quote, "prefix", selection.quote_prefix)
       || !set_string(env, quote, "suffix", selection.quote_suffix)) {
        return nullptr;
    }
    for(std::size_t index = 0; index < selection.quads.size(); ++index) {
        const auto quad = page_quad_value(env, selection.quads[index]);
        if(quad == nullptr || napi_set_element(env, quads, static_cast<std::uint32_t>(index), quad) != napi_ok) {
            return nullptr;
        }
    }
    return set_property(env, result, "quads", quads)
        && set_property(env, result, "quote", quote) ? result : nullptr;
}

napi_value verification_value(napi_env env, const WorkspaceVerification& check) {
    napi_value result = nullptr;
    napi_value issues = nullptr;
    if(check.issues.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_object(env, &result) != napi_ok
       || !set_boolean(env, result, "valid", check.valid)
       || !set_number(env, result, "documentCount", check.document_count)
       || !set_number(env, result, "documentVersionCount", check.document_version_count)
       || !set_number(env, result, "referencedObjectCount", check.referenced_object_count)
       || !set_number(env, result, "orphanedObjectCount", check.orphaned_object_count)
       || napi_create_array_with_length(env, check.issues.size(), &issues) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < check.issues.size(); ++index) {
        napi_value issue = nullptr;
        if(!make_string(env, check.issues[index], &issue)
           || napi_set_element(env, issues, static_cast<std::uint32_t>(index), issue)
                  != napi_ok) {
            return nullptr;
        }
    }
    return set_property(env, result, "issues", issues) ? result : nullptr;
}

const char* color_name(HighlightColor color) {
    switch(color) {
        case HighlightColor::yellow: return "yellow";
        case HighlightColor::green: return "green";
        case HighlightColor::blue: return "blue";
        case HighlightColor::pink: return "pink";
    }
    return "yellow";
}

napi_value annotation_value(napi_env env, const AnnotationRecord& annotation) {
    napi_value result = nullptr;
    napi_value quads = nullptr;
    napi_value quote = nullptr;
    if(annotation.quads.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "id", stable_id_to_hex(annotation.id))
       || !set_string(env, result, "documentVersionId", stable_id_to_hex(annotation.document_version_id))
       || !set_number(env, result, "pageIndex", annotation.page_index)
       || !set_string(env, result, "layoutVersion", annotation.layout_version)
       || !set_string(env, result, "color", color_name(annotation.color))
       || !set_number(env, result, "anchorVersion", annotation.anchor_version)
       || !set_string(env, result, "direction", annotation.direction)
       || napi_create_array_with_length(env, annotation.quads.size(), &quads) != napi_ok
       || napi_create_object(env, &quote) != napi_ok
       || !set_string(env, quote, "exact", annotation.quote.exact)
       || !set_string(env, quote, "prefix", annotation.quote.prefix)
       || !set_string(env, quote, "suffix", annotation.quote.suffix)
       || !set_property(env, result, "quote", quote)) {
        return nullptr;
    }
    if(annotation.text_start && !set_number(env, result, "textStart", *annotation.text_start)) return nullptr;
    if(annotation.text_end && !set_number(env, result, "textEnd", *annotation.text_end)) return nullptr;
    for(std::size_t index = 0; index < annotation.quads.size(); ++index) {
        const auto quad = rect_value(env, annotation.quads[index]);
        if(quad == nullptr || napi_set_element(env, quads, static_cast<std::uint32_t>(index), quad) != napi_ok) {
            return nullptr;
        }
    }
    return set_property(env, result, "quads", quads) ? result : nullptr;
}

napi_value annotations_value(napi_env env, const std::vector<AnnotationRecord>& annotations) {
    napi_value result = nullptr;
    if(annotations.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_array_with_length(env, annotations.size(), &result) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < annotations.size(); ++index) {
        const auto value = annotation_value(env, annotations[index]);
        if(value == nullptr || napi_set_element(env, result, static_cast<std::uint32_t>(index), value) != napi_ok) {
            return nullptr;
        }
    }
    return result;
}

napi_value note_value(napi_env env, const NoteRecord& note) {
    napi_value result = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "id", stable_id_to_hex(note.id))
       || !set_string(env, result, "annotationId", stable_id_to_hex(note.annotation_id))
       || !set_string(env, result, "markdownSource", note.markdown_source)
       || !set_number(env, result, "revision", note.revision)) {
        return nullptr;
    }
    return result;
}

napi_value notes_value(napi_env env, const std::vector<NoteRecord>& notes) {
    napi_value result = nullptr;
    if(notes.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_array_with_length(env, notes.size(), &result) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < notes.size(); ++index) {
        const auto value = note_value(env, notes[index]);
        if(value == nullptr || napi_set_element(env, result, static_cast<std::uint32_t>(index), value) != napi_ok) {
            return nullptr;
        }
    }
    return result;
}

napi_value search_value(napi_env env, const SearchResponse& response) {
    napi_value result = nullptr;
    napi_value entries = nullptr;
    if(response.results.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "indexStatus", response.index_status)
       || napi_create_array_with_length(env, response.results.size(), &entries) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < response.results.size(); ++index) {
        const auto& source = response.results[index];
        napi_value entry = nullptr;
        if(napi_create_object(env, &entry) != napi_ok
           || !set_string(env, entry, "kind", source.kind == SearchResultKind::note ? "note" : "pdfPage")
           || !set_string(env, entry, "documentVersionId", stable_id_to_hex(source.document_version_id))
           || !set_string(env, entry, "title", source.title)
           || !set_string(env, entry, "excerpt", source.excerpt)) {
            return nullptr;
        }
        if(source.note_id && !set_string(env, entry, "noteId", stable_id_to_hex(*source.note_id))) {
            return nullptr;
        }
        if(source.page_index && !set_number(env, entry, "pageIndex", *source.page_index)) {
            return nullptr;
        }
        if(napi_set_element(env, entries, static_cast<std::uint32_t>(index), entry) != napi_ok) {
            return nullptr;
        }
    }
    return set_property(env, result, "results", entries) ? result : nullptr;
}

napi_value asset_record_value(napi_env env, const AssetRecord& asset) {
    napi_value result = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "id", stable_id_to_hex(asset.id))
       || !set_string(env, result, "contentSha256", asset.content_sha256)
       || !set_string(env, result, "mediaType", asset.media_type)
       || !set_number(env, result, "byteLength", asset.byte_length)
       || !set_number(env, result, "width", asset.width)
       || !set_number(env, result, "height", asset.height)) {
        return nullptr;
    }
    return result;
}

napi_value asset_data_value(napi_env env, const AssetData& data) {
    napi_value result = nullptr;
    napi_value bytes = nullptr;
    void* copied_data = nullptr;
    const auto asset = asset_record_value(env, data.asset);
    if(asset == nullptr || napi_create_object(env, &result) != napi_ok
       || napi_create_arraybuffer(env, data.bytes.size(), &copied_data, &bytes) != napi_ok) {
        return nullptr;
    }
    if(!data.bytes.empty()) {
        std::copy(data.bytes.begin(), data.bytes.end(), static_cast<std::uint8_t*>(copied_data));
    }
    return set_property(env, result, "asset", asset)
        && set_property(env, result, "bytes", bytes) ? result : nullptr;
}

napi_value backup_inspection_value(napi_env env, const BackupInspection& inspection) {
    napi_value result = nullptr;
    napi_value issues = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_boolean(env, result, "valid", inspection.valid)
       || !set_number(env, result, "formatVersion", inspection.format_version)
       || !set_number(env, result, "fileCount", inspection.file_count)
       || !set_number(env, result, "totalUncompressedBytes", inspection.total_uncompressed_bytes)
       || napi_create_array_with_length(env, inspection.issues.size(), &issues) != napi_ok) {
        return nullptr;
    }
    for(std::size_t index = 0; index < inspection.issues.size(); ++index) {
        napi_value issue = nullptr;
        if(!make_string(env, inspection.issues[index], &issue)
           || napi_set_element(env, issues, static_cast<std::uint32_t>(index), issue) != napi_ok) return nullptr;
    }
    return set_property(env, result, "issues", issues) ? result : nullptr;
}

template <typename T>
void store(Result<T> result, AsyncWork& work) {
    if(result) {
        work.payload = std::move(result).value();
    } else {
        work.error = result.error();
    }
}

void store(Result<void> result, AsyncWork& work) {
    if(result) {
        work.payload = std::monostate{};
    } else {
        work.error = result.error();
    }
}

void execute(napi_env, void* data) {
    auto& work = *static_cast<AsyncWork*>(data);
    try {
        auto& app = work.state->runtime->application();
        switch(work.operation) {
            case Operation::create_workspace: store(app.create_workspace(work.path), work); break;
            case Operation::open_workspace: store(app.open_workspace(work.path), work); break;
            case Operation::close_workspace: store(app.close_workspace(), work); break;
            case Operation::import_document:
                store(app.import_document(work.path, work.cancellation), work);
                break;
            case Operation::list_documents: store(app.list_documents(), work); break;
            case Operation::open_document:
                store(app.open_document(*work.document_id, work.cancellation), work);
                break;
            case Operation::close_document: store(app.close_document(), work); break;
            case Operation::page_info: store(app.page_info(work.page_index), work); break;
            case Operation::render_page:
                store(
                    app.render_page(
                        work.page_index,
                        work.pixels_per_point,
                        work.cancellation
                    ),
                    work
                );
                break;
            case Operation::render_tile:
                store(app.render_tile(*work.tile_request, work.cancellation), work);
                break;
            case Operation::extract_page_text:
                store(app.extract_page_text(work.page_index), work);
                break;
            case Operation::page_text_layout:
                store(app.page_text_layout(work.page_index), work);
                break;
            case Operation::select_text:
                store(app.select_text(work.page_index, work.start_point, work.end_point), work);
                break;
            case Operation::create_annotation: store(app.create_annotation(*work.annotation), work); break;
            case Operation::list_annotations:
                store(app.list_annotations(*work.document_version_id), work);
                break;
            case Operation::delete_annotation: store(app.delete_annotation(*work.annotation_id), work); break;
            case Operation::update_note: store(app.update_note(*work.note), work); break;
            case Operation::list_notes: store(app.list_notes(*work.document_version_id), work); break;
            case Operation::rebuild_search_index:
                store(app.rebuild_search_index(work.cancellation), work);
                break;
            case Operation::search:
                store(app.search(work.search_query, work.search_limit), work);
                break;
            case Operation::import_note_asset:
                store(app.import_note_asset(*work.annotation_id, work.path, work.cancellation), work);
                break;
            case Operation::read_asset:
                store(app.read_asset(*work.asset_id), work);
                break;
            case Operation::export_workspace:
                store(app.export_workspace(work.path, work.cancellation), work);
                break;
            case Operation::inspect_backup:
                store(app.inspect_backup(work.path), work);
                break;
            case Operation::restore_workspace:
                store(app.restore_workspace(work.path, work.target_path, work.cancellation), work);
                break;
            case Operation::verify_workspace: store(app.verify_workspace(), work); break;
        }
    } catch(const std::exception& exception) {
        work.error = Error(ErrorCode::internal, exception.what());
    } catch(...) {
        work.error = Error(ErrorCode::internal, "Unknown native failure");
    }
}

napi_value payload_value(napi_env env, const Payload& payload) {
    return std::visit(
        [env](const auto& value) -> napi_value {
            using T = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T, std::monostate>) {
                napi_value result = nullptr;
                return napi_get_undefined(env, &result) == napi_ok ? result : nullptr;
            } else if constexpr(std::is_same_v<T, WorkspaceInfo>) {
                return workspace_value(env, value);
            } else if constexpr(std::is_same_v<T, ImportDocumentResult>) {
                return import_value(env, value);
            } else if constexpr(std::is_same_v<T, DocumentRecord>) {
                return document_value(env, value);
            } else if constexpr(std::is_same_v<T, std::vector<DocumentRecord>>) {
                return documents_value(env, value);
            } else if constexpr(std::is_same_v<T, PageInfo>) {
                return page_info_value(env, value);
            } else if constexpr(std::is_same_v<T, EncodedPageImage>) {
                return rendered_page_value(env, value);
            } else if constexpr(std::is_same_v<T, RenderedTile>) {
                return rendered_tile_value(env, value);
            } else if constexpr(std::is_same_v<T, PageText>) {
                return page_text_value(env, value);
            } else if constexpr(std::is_same_v<T, PageTextLayout>) {
                return page_text_layout_value(env, value);
            } else if constexpr(std::is_same_v<T, TextSelection>) {
                return text_selection_value(env, value);
            } else if constexpr(std::is_same_v<T, AnnotationRecord>) {
                return annotation_value(env, value);
            } else if constexpr(std::is_same_v<T, std::vector<AnnotationRecord>>) {
                return annotations_value(env, value);
            } else if constexpr(std::is_same_v<T, NoteRecord>) {
                return note_value(env, value);
            } else if constexpr(std::is_same_v<T, std::vector<NoteRecord>>) {
                return notes_value(env, value);
            } else if constexpr(std::is_same_v<T, SearchResponse>) {
                return search_value(env, value);
            } else if constexpr(std::is_same_v<T, AssetRecord>) {
                return asset_record_value(env, value);
            } else if constexpr(std::is_same_v<T, AssetData>) {
                return asset_data_value(env, value);
            } else if constexpr(std::is_same_v<T, BackupInspection>) {
                return backup_inspection_value(env, value);
            } else {
                return verification_value(env, value);
            }
        },
        payload
    );
}

void complete(napi_env env, napi_status status, void* data) {
    std::unique_ptr<AsyncWork> work(static_cast<AsyncWork*>(data));
    if(work->job_id) {
        work->state->finish_job(*work->job_id);
    }
    if(status != napi_ok && !work->error) {
        work->error = Error(ErrorCode::cancelled, "Native operation was cancelled");
    }
    if(work->error) {
        napi_value code = nullptr;
        napi_value message = nullptr;
        napi_value error = nullptr;
        napi_create_string_utf8(env, error_code(work->error->code()), NAPI_AUTO_LENGTH, &code);
        make_string(env, work->error->message(), &message);
        napi_create_error(env, code, message, &error);
        napi_reject_deferred(env, work->deferred, error);
    } else {
        const auto result = payload_value(env, work->payload);
        if(result != nullptr) {
            napi_resolve_deferred(env, work->deferred, result);
        } else {
            napi_value message = nullptr;
            napi_value error = nullptr;
            napi_create_string_utf8(env, "Failed to create native result", NAPI_AUTO_LENGTH, &message);
            napi_create_error(env, nullptr, message, &error);
            napi_reject_deferred(env, work->deferred, error);
        }
    }
    napi_delete_async_work(env, work->work);
}

std::optional<std::string> string_argument(
    napi_env env,
    napi_value argument,
    const char* message
) {
    napi_valuetype type = napi_undefined;
    if(napi_typeof(env, argument, &type) != napi_ok || type != napi_string) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", message);
        return std::nullopt;
    }
    std::size_t size = 0;
    if(napi_get_value_string_utf8(env, argument, nullptr, 0, &size) != napi_ok) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "String must be valid UTF-8");
        return std::nullopt;
    }
    std::string value(size + 1, '\0');
    std::size_t written = 0;
    if(napi_get_value_string_utf8(env, argument, value.data(), value.size(), &written) != napi_ok) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "String must be valid UTF-8");
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

std::optional<double> number_argument(
    napi_env env,
    napi_value argument,
    const char* message
) {
    napi_valuetype type = napi_undefined;
    double value = 0.0;
    if(napi_typeof(env, argument, &type) != napi_ok || type != napi_number
       || napi_get_value_double(env, argument, &value) != napi_ok) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", message);
        return std::nullopt;
    }
    return value;
}

std::optional<std::size_t> page_index_argument(napi_env env, napi_value argument) {
    const auto value = number_argument(env, argument, "Page index must be a non-negative integer");
    constexpr double maximum_safe_integer = 9'007'199'254'740'991.0;
    const auto maximum_index = std::min(
        maximum_safe_integer,
        static_cast<double>(std::numeric_limits<std::size_t>::max())
    );
    if(!value || !std::isfinite(*value) || *value < 0.0 || std::floor(*value) != *value
       || *value > maximum_index) {
        if(value) {
            napi_throw_range_error(
                env, "INVALID_ARGUMENT", "Page index must be a non-negative integer"
            );
        }
        return std::nullopt;
    }
    return static_cast<std::size_t>(*value);
}

std::optional<napi_value> named_property(
    napi_env env,
    napi_value object,
    const char* name,
    const char* message
) {
    napi_valuetype type = napi_undefined;
    bool present = false;
    napi_value value = nullptr;
    if(napi_typeof(env, object, &type) != napi_ok || type != napi_object
       || napi_has_named_property(env, object, name, &present) != napi_ok || !present
       || napi_get_named_property(env, object, name, &value) != napi_ok) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", message);
        return std::nullopt;
    }
    return value;
}

template <typename Tag>
std::optional<StableId<Tag>> id_property(
    napi_env env,
    napi_value object,
    const char* name,
    const char* message
) {
    const auto property = named_property(env, object, name, message);
    if(!property) return std::nullopt;
    const auto text = string_argument(env, *property, message);
    if(!text) return std::nullopt;
    auto id = stable_id_from_hex<Tag>(*text);
    if(!id) {
        napi_throw_range_error(env, "INVALID_ARGUMENT", message);
    }
    return id;
}

std::optional<std::string> string_property(
    napi_env env,
    napi_value object,
    const char* name,
    const char* message
) {
    const auto property = named_property(env, object, name, message);
    return property ? string_argument(env, *property, message) : std::nullopt;
}

std::optional<double> number_property(
    napi_env env,
    napi_value object,
    const char* name,
    const char* message
) {
    const auto property = named_property(env, object, name, message);
    return property ? number_argument(env, *property, message) : std::nullopt;
}

std::optional<std::size_t> size_property(
    napi_env env,
    napi_value object,
    const char* name,
    const char* message
) {
    const auto property = named_property(env, object, name, message);
    return property ? page_index_argument(env, *property) : std::nullopt;
}

std::optional<PagePoint> point_argument(napi_env env, napi_value argument, const char* message) {
    const auto x = number_property(env, argument, "x", message);
    const auto y = number_property(env, argument, "y", message);
    if(!x || !y || !std::isfinite(*x) || !std::isfinite(*y)) {
        if(x && y) napi_throw_range_error(env, "INVALID_ARGUMENT", message);
        return std::nullopt;
    }
    return PagePoint{.x = *x, .y = *y};
}

std::optional<TileRequest> tile_request_argument(napi_env env, napi_value argument) {
    const auto page_index = size_property(env, argument, "pageIndex", "Tile pageIndex must be a non-negative integer");
    const auto scale = number_property(env, argument, "pixelsPerPoint", "Tile pixelsPerPoint must be a number");
    const auto x = size_property(env, argument, "xPixels", "Tile xPixels must be a non-negative integer");
    const auto y = size_property(env, argument, "yPixels", "Tile yPixels must be a non-negative integer");
    const auto width = size_property(env, argument, "widthPixels", "Tile widthPixels must be a positive integer");
    const auto height = size_property(env, argument, "heightPixels", "Tile heightPixels must be a positive integer");
    const auto generation = size_property(env, argument, "generation", "Tile generation must be a non-negative integer");
    if(!page_index || !scale || !x || !y || !width || !height || !generation) return std::nullopt;
    if(!std::isfinite(*scale) || *scale <= 0.0 || *scale > 16.0
       || *width == 0 || *height == 0 || *width > 512 || *height > 512) {
        napi_throw_range_error(env, "INVALID_ARGUMENT", "Tile dimensions or scale are outside the supported range");
        return std::nullopt;
    }
    return TileRequest{
        .page_index = *page_index,
        .pixels_per_point = *scale,
        .x_pixels = *x,
        .y_pixels = *y,
        .width_pixels = *width,
        .height_pixels = *height,
        .generation = *generation,
    };
}

std::optional<CreateAnnotation> annotation_argument(napi_env env, napi_value argument) {
    const auto version_id = id_property<DocumentVersionIdTag>(
        env, argument, "documentVersionId", "Document version ID must be 32 hexadecimal characters"
    );
    const auto page_property = named_property(
        env, argument, "pageIndex", "Annotation pageIndex is required"
    );
    const auto layout_version = string_property(
        env, argument, "layoutVersion", "Annotation layoutVersion must be a string"
    );
    const auto color_text = string_property(env, argument, "color", "Annotation color must be a string");
    const auto quote_property = named_property(env, argument, "quote", "Annotation quote is required");
    const auto quads_property = named_property(env, argument, "quads", "Annotation quads are required");
    if(!version_id || !page_property || !layout_version || !color_text || !quote_property || !quads_property) {
        return std::nullopt;
    }
    const auto page_index = page_index_argument(env, *page_property);
    const auto exact = string_property(env, *quote_property, "exact", "Quote exact must be a string");
    const auto prefix = string_property(env, *quote_property, "prefix", "Quote prefix must be a string");
    const auto suffix = string_property(env, *quote_property, "suffix", "Quote suffix must be a string");
    if(!page_index || !exact || !prefix || !suffix) return std::nullopt;

    HighlightColor color = HighlightColor::yellow;
    if(*color_text == "green") color = HighlightColor::green;
    else if(*color_text == "blue") color = HighlightColor::blue;
    else if(*color_text == "pink") color = HighlightColor::pink;
    else if(*color_text != "yellow") {
        napi_throw_range_error(env, "INVALID_ARGUMENT", "Annotation color is unsupported");
        return std::nullopt;
    }

    bool is_array = false;
    std::uint32_t length = 0;
    if(napi_is_array(env, *quads_property, &is_array) != napi_ok || !is_array
       || napi_get_array_length(env, *quads_property, &length) != napi_ok || length == 0U) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "Annotation quads must be a non-empty array");
        return std::nullopt;
    }
    std::vector<PageRect> quads;
    quads.reserve(length);
    for(std::uint32_t index = 0; index < length; ++index) {
        napi_value quad = nullptr;
        if(napi_get_element(env, *quads_property, index, &quad) != napi_ok) {
            napi_throw_type_error(env, "INVALID_ARGUMENT", "Annotation quad could not be read");
            return std::nullopt;
        }
        const auto x = number_property(env, quad, "x", "Annotation quad x must be a number");
        const auto y = number_property(env, quad, "y", "Annotation quad y must be a number");
        const auto width = number_property(env, quad, "width", "Annotation quad width must be a number");
        const auto height = number_property(env, quad, "height", "Annotation quad height must be a number");
        if(!x || !y || !width || !height) return std::nullopt;
        quads.push_back(PageRect{.x = *x, .y = *y, .width = *width, .height = *height});
    }
    CreateAnnotation command{
        .document_version_id = *version_id,
        .page_index = *page_index,
        .quads = std::move(quads),
        .quote = QuoteAnchor{.exact = *exact, .prefix = *prefix, .suffix = *suffix},
        .layout_version = *layout_version,
        .color = color,
    };
    bool anchor_present = false;
    if(napi_has_named_property(env, argument, "anchorVersion", &anchor_present) != napi_ok) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "Annotation anchorVersion could not be read");
        return std::nullopt;
    }
    if(anchor_present) {
        const auto anchor_version = number_property(env, argument, "anchorVersion", "Annotation anchorVersion must be 2");
        const auto text_start = size_property(env, argument, "textStart", "Annotation textStart must be a non-negative integer");
        const auto text_end = size_property(env, argument, "textEnd", "Annotation textEnd must be a positive integer");
        const auto direction = string_property(env, argument, "direction", "Annotation direction must be a string");
        if(!anchor_version || *anchor_version != 2.0 || !text_start || !text_end || !direction
           || *text_end <= *text_start
           || (*direction != "ltr" && *direction != "rtl" && *direction != "ttb")) {
            if(anchor_version && text_start && text_end && direction) {
                napi_throw_range_error(env, "INVALID_ARGUMENT", "Character annotation anchor is invalid");
            }
            return std::nullopt;
        }
        command.anchor_version = 2;
        command.text_start = *text_start;
        command.text_end = *text_end;
        command.direction = *direction;
    }
    return command;
}

std::optional<UpdateNote> note_argument(napi_env env, napi_value argument) {
    const auto annotation_id = id_property<AnnotationIdTag>(
        env, argument, "annotationId", "Annotation ID must be 32 hexadecimal characters"
    );
    const auto revision = number_property(
        env, argument, "expectedRevision", "Expected revision must be a non-negative integer"
    );
    const auto markdown = string_property(
        env, argument, "markdownSource", "Markdown source must be a string"
    );
    constexpr double maximum_safe_integer = 9'007'199'254'740'991.0;
    if(!annotation_id || !revision || !markdown || !std::isfinite(*revision) || *revision < 0.0
       || std::floor(*revision) != *revision || *revision > maximum_safe_integer) {
        if(annotation_id && revision && markdown) {
            napi_throw_range_error(env, "INVALID_ARGUMENT", "Expected revision must be a non-negative integer");
        }
        return std::nullopt;
    }
    return UpdateNote{
        .annotation_id = *annotation_id,
        .expected_revision = static_cast<std::uint64_t>(*revision),
        .markdown_source = *markdown,
    };
}

napi_value schedule(napi_env env, napi_callback_info info) {
    void* raw_data = nullptr;
    std::size_t count = 0;
    if(napi_get_cb_info(env, info, &count, nullptr, nullptr, &raw_data) != napi_ok) {
        napi_throw_error(env, "NAPI_ARGUMENT_FAILED", "Failed to read arguments");
        return nullptr;
    }
    if(count > 3U) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "Too many arguments");
        return nullptr;
    }
    std::array<napi_value, 3> arguments{};
    auto copied_count = count;
    if(napi_get_cb_info(
           env, info, &copied_count, arguments.data(), nullptr, &raw_data
       ) != napi_ok
       || copied_count != count) {
        napi_throw_error(env, "NAPI_ARGUMENT_FAILED", "Failed to read arguments");
        return nullptr;
    }
    const auto& function = *static_cast<FunctionData*>(raw_data);
    auto work = std::make_unique<AsyncWork>();
    work->state = function.context->state;
    work->operation = function.operation;
    const auto expect_count = [env, count](std::size_t expected, const char* message) {
        if(count == expected) {
            return true;
        }
        napi_throw_type_error(env, "INVALID_ARGUMENT", message);
        return false;
    };
    switch(function.operation) {
        case Operation::create_workspace:
        case Operation::open_workspace: {
            if(!expect_count(1, "Expected one path argument")) {
                return nullptr;
            }
            const auto value = string_argument(env, arguments[0], "Path must be a string");
            if(!value) {
                return nullptr;
            }
            work->path = utf8_path(*value);
            break;
        }
        case Operation::import_document: {
            if(count != 1U && count != 2U) {
                napi_throw_type_error(
                    env,
                    "INVALID_ARGUMENT",
                    "Expected a path and optional Job ID"
                );
                return nullptr;
            }
            const auto value = string_argument(env, arguments[0], "Path must be a string");
            if(!value) return nullptr;
            work->path = utf8_path(*value);
            if(count == 2U) {
                work->job_id = string_argument(
                    env,
                    arguments[1],
                    "Job ID must be a non-empty string"
                );
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::open_document: {
            if(count != 1U && count != 2U) {
                napi_throw_type_error(
                    env,
                    "INVALID_ARGUMENT",
                    "Expected a document ID and optional Job ID"
                );
                return nullptr;
            }
            const auto value = string_argument(
                env, arguments[0], "Document ID must be a hexadecimal string"
            );
            if(!value) {
                return nullptr;
            }
            work->document_id = stable_id_from_hex<DocumentIdTag>(*value);
            if(!work->document_id) {
                napi_throw_range_error(
                    env,
                    "INVALID_ARGUMENT",
                    "Document ID must contain exactly 32 hexadecimal characters"
                );
                return nullptr;
            }
            if(count == 2U) {
                work->job_id = string_argument(
                    env,
                    arguments[1],
                    "Job ID must be a non-empty string"
                );
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::list_annotations:
        case Operation::list_notes: {
            if(!expect_count(1, "Expected one document version ID argument")) return nullptr;
            const auto value = string_argument(
                env, arguments[0], "Document version ID must be a hexadecimal string"
            );
            if(!value) return nullptr;
            work->document_version_id = stable_id_from_hex<DocumentVersionIdTag>(*value);
            if(!work->document_version_id) {
                napi_throw_range_error(
                    env, "INVALID_ARGUMENT", "Document version ID must contain exactly 32 hexadecimal characters"
                );
                return nullptr;
            }
            break;
        }
        case Operation::delete_annotation: {
            if(!expect_count(1, "Expected one annotation ID argument")) return nullptr;
            const auto value = string_argument(env, arguments[0], "Annotation ID must be a hexadecimal string");
            if(!value) return nullptr;
            work->annotation_id = stable_id_from_hex<AnnotationIdTag>(*value);
            if(!work->annotation_id) {
                napi_throw_range_error(
                    env, "INVALID_ARGUMENT", "Annotation ID must contain exactly 32 hexadecimal characters"
                );
                return nullptr;
            }
            break;
        }
        case Operation::create_annotation: {
            if(!expect_count(1, "Expected one annotation argument")) return nullptr;
            work->annotation = annotation_argument(env, arguments[0]);
            if(!work->annotation) return nullptr;
            break;
        }
        case Operation::update_note: {
            if(!expect_count(1, "Expected one note argument")) return nullptr;
            work->note = note_argument(env, arguments[0]);
            if(!work->note) return nullptr;
            break;
        }
        case Operation::rebuild_search_index: {
            if(count > 1U) {
                napi_throw_type_error(env, "INVALID_ARGUMENT", "Expected an optional Job ID");
                return nullptr;
            }
            if(count == 1U) {
                work->job_id = string_argument(env, arguments[0], "Job ID must be a non-empty string");
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::search: {
            if(!expect_count(1, "Expected one search request")) return nullptr;
            const auto query = string_property(env, arguments[0], "query", "Search query must be a string");
            const auto limit = size_property(env, arguments[0], "limit", "Search limit must be a positive integer");
            if(!query || !limit || query->empty() || *limit == 0 || *limit > 200) {
                if(query && limit) napi_throw_range_error(env, "INVALID_ARGUMENT", "Search query or limit is outside the supported range");
                return nullptr;
            }
            work->search_query = *query;
            work->search_limit = *limit;
            break;
        }
        case Operation::import_note_asset: {
            if(count != 2U && count != 3U) {
                napi_throw_type_error(env, "INVALID_ARGUMENT", "Expected annotation ID, asset path and optional Job ID");
                return nullptr;
            }
            const auto annotation = string_argument(env, arguments[0], "Annotation ID must be a hexadecimal string");
            const auto source = string_argument(env, arguments[1], "Asset path must be a string");
            if(!annotation || !source) return nullptr;
            work->annotation_id = stable_id_from_hex<AnnotationIdTag>(*annotation);
            if(!work->annotation_id) {
                napi_throw_range_error(env, "INVALID_ARGUMENT", "Annotation ID must contain exactly 32 hexadecimal characters");
                return nullptr;
            }
            work->path = utf8_path(*source);
            if(count == 3U) {
                work->job_id = string_argument(env, arguments[2], "Job ID must be a non-empty string");
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::read_asset: {
            if(!expect_count(1, "Expected one Asset ID")) return nullptr;
            const auto asset = string_argument(env, arguments[0], "Asset ID must be a hexadecimal string");
            if(!asset) return nullptr;
            work->asset_id = stable_id_from_hex<AssetIdTag>(*asset);
            if(!work->asset_id) {
                napi_throw_range_error(env, "INVALID_ARGUMENT", "Asset ID must contain exactly 32 hexadecimal characters");
                return nullptr;
            }
            break;
        }
        case Operation::export_workspace: {
            if(count != 1U && count != 2U) {
                napi_throw_type_error(env, "INVALID_ARGUMENT", "Expected backup destination and optional Job ID");
                return nullptr;
            }
            const auto destination = string_argument(env, arguments[0], "Backup destination must be a string");
            if(!destination) return nullptr;
            work->path = utf8_path(*destination);
            if(count == 2U) {
                work->job_id = string_argument(env, arguments[1], "Job ID must be a non-empty string");
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::inspect_backup: {
            if(!expect_count(1, "Expected one backup path")) return nullptr;
            const auto package_path = string_argument(env, arguments[0], "Backup path must be a string");
            if(!package_path) return nullptr;
            work->path = utf8_path(*package_path);
            break;
        }
        case Operation::restore_workspace: {
            if(count != 2U && count != 3U) {
                napi_throw_type_error(env, "INVALID_ARGUMENT", "Expected backup path, empty target and optional Job ID");
                return nullptr;
            }
            const auto package_path = string_argument(env, arguments[0], "Backup path must be a string");
            const auto target = string_argument(env, arguments[1], "Restore target must be a string");
            if(!package_path || !target) return nullptr;
            work->path = utf8_path(*package_path);
            work->target_path = utf8_path(*target);
            if(count == 3U) {
                work->job_id = string_argument(env, arguments[2], "Job ID must be a non-empty string");
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::page_info:
        case Operation::extract_page_text:
        case Operation::page_text_layout: {
            if(!expect_count(1, "Expected one page index argument")) {
                return nullptr;
            }
            const auto page_index = page_index_argument(env, arguments[0]);
            if(!page_index) {
                return nullptr;
            }
            work->page_index = *page_index;
            break;
        }
        case Operation::render_page: {
            if(count != 2U && count != 3U) {
                napi_throw_type_error(
                    env,
                    "INVALID_ARGUMENT",
                    "Expected page index, pixels-per-point and optional Job ID"
                );
                return nullptr;
            }
            const auto page_index = page_index_argument(env, arguments[0]);
            const auto pixels_per_point = number_argument(
                env, arguments[1], "Pixels per point must be a number"
            );
            if(!page_index || !pixels_per_point) {
                return nullptr;
            }
            if(!std::isfinite(*pixels_per_point) || *pixels_per_point <= 0.0
               || *pixels_per_point > 64.0) {
                napi_throw_range_error(
                    env,
                    "INVALID_ARGUMENT",
                    "Pixels per point must be finite and in the range (0, 64]"
                );
                return nullptr;
            }
            work->page_index = *page_index;
            work->pixels_per_point = *pixels_per_point;
            if(count == 3U) {
                work->job_id = string_argument(
                    env,
                    arguments[2],
                    "Job ID must be a non-empty string"
                );
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::render_tile: {
            if(count != 1U && count != 2U) {
                napi_throw_type_error(env, "INVALID_ARGUMENT", "Expected a Tile request and optional Job ID");
                return nullptr;
            }
            work->tile_request = tile_request_argument(env, arguments[0]);
            if(!work->tile_request) return nullptr;
            if(count == 2U) {
                work->job_id = string_argument(env, arguments[1], "Job ID must be a non-empty string");
                if(!work->job_id || work->job_id->empty()) return nullptr;
            }
            break;
        }
        case Operation::select_text: {
            if(!expect_count(3, "Expected page index, start point and end point")) return nullptr;
            const auto page_index = page_index_argument(env, arguments[0]);
            const auto start = point_argument(env, arguments[1], "Selection start point must contain finite x and y");
            const auto end = point_argument(env, arguments[2], "Selection end point must contain finite x and y");
            if(!page_index || !start || !end) return nullptr;
            work->page_index = *page_index;
            work->start_point = *start;
            work->end_point = *end;
            break;
        }
        case Operation::close_workspace:
        case Operation::list_documents:
        case Operation::close_document:
        case Operation::verify_workspace:
            if(!expect_count(0, "This operation does not accept arguments")) {
                return nullptr;
            }
            break;
    }
    CancellationSource cancellation_source;
    work->cancellation = cancellation_source.token();
    napi_value promise = nullptr;
    napi_value resource_name = nullptr;
    if(napi_create_promise(env, &work->deferred, &promise) != napi_ok
       || napi_create_string_utf8(env, "context_reader.operation", NAPI_AUTO_LENGTH, &resource_name) != napi_ok
       || napi_create_async_work(
           env, nullptr, resource_name, execute, complete, work.get(), &work->work
       ) != napi_ok) {
        if(work->work != nullptr) {
            napi_delete_async_work(env, work->work);
        }
        napi_throw_error(env, "NAPI_ASYNC_FAILED", "Failed to schedule native operation");
        return nullptr;
    }
    if(work->job_id && !work->state->register_job(
                           *work->job_id,
                           cancellation_source,
                           env,
                           work->work
                       )) {
        napi_delete_async_work(env, work->work);
        napi_throw_error(env, "CONFLICT", "Job ID is already active");
        return nullptr;
    }
    if(napi_queue_async_work(env, work->work) != napi_ok) {
        if(work->job_id) work->state->finish_job(*work->job_id);
        napi_delete_async_work(env, work->work);
        napi_throw_error(env, "NAPI_ASYNC_FAILED", "Failed to schedule native operation");
        return nullptr;
    }
    work.release();
    return promise;
}

Napi::Value cancel_job(const Napi::CallbackInfo& info) {
    const auto env = info.Env();
    if(info.Length() != 1 || !info[0].IsString()) {
        auto error = Napi::TypeError::New(env, "Job ID must be a string");
        error.Set("code", Napi::String::New(env, "INVALID_ARGUMENT"));
        error.ThrowAsJavaScriptException();
        return env.Undefined();
    }
    const auto id = info[0].As<Napi::String>().Utf8Value();
    if(id.empty()) {
        auto error = Napi::TypeError::New(env, "Job ID must not be empty");
        error.Set("code", Napi::String::New(env, "INVALID_ARGUMENT"));
        error.ThrowAsJavaScriptException();
        return env.Undefined();
    }
    const auto& context = *static_cast<AddonContext*>(info.Data());
    return Napi::Boolean::New(env, context.state->cancel_job(id));
}

Napi::Value runtime_info(const Napi::CallbackInfo& info) {
    const auto env = info.Env();
    try {
        if(info.Length() != 0) {
            auto error = Napi::TypeError::New(env, "This operation does not accept arguments");
            error.Set("code", Napi::String::New(env, "INVALID_ARGUMENT"));
            error.ThrowAsJavaScriptException();
            return env.Undefined();
        }
        const auto& context = *static_cast<AddonContext*>(info.Data());
        const auto runtime = context.state->runtime->application().runtime_info();
        char version[32]{};
        const int written = std::snprintf(
            version,
            sizeof(version),
            "%u.%u.%u",
            runtime.version.major,
            runtime.version.minor,
            runtime.version.patch
        );
        if(written < 0 || static_cast<std::size_t>(written) >= sizeof(version)) {
            Napi::Error::New(env, "Runtime version formatting failed")
                .ThrowAsJavaScriptException();
            return env.Undefined();
        }
        auto result = Napi::Object::New(env);
        result.Set("version", Napi::String::New(env, version));
        result.Set(
            "applicationApiVersion",
            Napi::Number::New(env, runtime.application_api_version)
        );
        result.Set("buildId", Napi::String::New(env, runtime.build_id));
        result.Set("bindingNapiVersion", Napi::Number::New(env, NAPI_VERSION));
        auto capabilities = Napi::Array::New(env, runtime.capabilities.size());
        for(std::size_t index = 0; index < runtime.capabilities.size(); ++index) {
            capabilities.Set(
                static_cast<std::uint32_t>(index),
                Napi::String::New(env, runtime.capabilities[index])
            );
        }
        result.Set("capabilities", capabilities);
        return result;
    } catch(const std::exception& exception) {
        Napi::Error::New(env, exception.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    } catch(...) {
        Napi::Error::New(env, "Native runtime operation failed")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

bool export_function(
    napi_env env,
    napi_value exports,
    const char* name,
    napi_callback callback,
    void* data
) {
    napi_value function = nullptr;
    return napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, data, &function) == napi_ok
        && set_property(env, exports, name, function);
}

void cleanup(void* data) {
    delete static_cast<AddonContext*>(data);
}

Napi::Object initialize(Napi::Env env, Napi::Object exports) {
    auto runtime = ReaderRuntime::create();
    if(!runtime) {
        Napi::Error::New(env, "ReaderRuntime creation failed").ThrowAsJavaScriptException();
        return exports;
    }
    auto context = std::make_unique<AddonContext>(
        std::make_shared<RuntimeState>(std::move(runtime).value())
    );
    constexpr std::array<const char*, 26> names{
        "createWorkspace",
        "openWorkspace",
        "closeWorkspace",
        "importDocument",
        "listDocuments",
        "openDocument",
        "closeDocument",
        "pageInfo",
        "renderPage",
        "renderTile",
        "extractPageText",
        "pageTextLayout",
        "selectText",
        "createAnnotation",
        "listAnnotations",
        "deleteAnnotation",
        "updateNote",
        "listNotes",
        "rebuildSearchIndex",
        "search",
        "importNoteAsset",
        "readAsset",
        "exportWorkspace",
        "inspectBackup",
        "restoreWorkspace",
        "verifyWorkspace",
    };
    exports.Set(
        "runtimeInfo",
        Napi::Function::New(env, runtime_info, "runtimeInfo", context.get())
    );
    exports.Set(
        "cancelJob",
        Napi::Function::New(env, cancel_job, "cancelJob", context.get())
    );
    for(std::size_t index = 0; index < names.size(); ++index) {
        if(!export_function(env, exports, names[index], schedule, &context->functions[index])) {
            napi_throw_error(env, "NAPI_INIT_FAILED", "Failed to initialize reader_node");
            return exports;
        }
    }
    if(napi_add_env_cleanup_hook(env, cleanup, context.get()) != napi_ok) {
        napi_throw_error(env, "NAPI_INIT_FAILED", "Failed to register cleanup hook");
        return exports;
    }
    context.release();
    return exports;
}

}  // namespace

NODE_API_MODULE(NODE_GYP_MODULE_NAME, initialize)
