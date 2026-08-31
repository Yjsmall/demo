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
    extract_page_text,
    create_annotation,
    list_annotations,
    delete_annotation,
    update_note,
    list_notes,
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
              {this, Operation::extract_page_text},
              {this, Operation::create_annotation},
              {this, Operation::list_annotations},
              {this, Operation::delete_annotation},
              {this, Operation::update_note},
              {this, Operation::list_notes},
              {this, Operation::verify_workspace},
          }} {}

    std::shared_ptr<RuntimeState> state;
    std::array<FunctionData, 16> functions;
};

using Payload = std::variant<
    std::monostate,
    WorkspaceInfo,
    ImportDocumentResult,
    DocumentRecord,
    std::vector<DocumentRecord>,
    PageInfo,
    EncodedPageImage,
    PageText,
    AnnotationRecord,
    std::vector<AnnotationRecord>,
    NoteRecord,
    std::vector<NoteRecord>,
    WorkspaceVerification>;

struct AsyncWork final {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::shared_ptr<RuntimeState> state;
    Operation operation;
    std::filesystem::path path;
    std::optional<DocumentId> document_id;
    std::optional<DocumentVersionId> document_version_id;
    std::optional<AnnotationId> annotation_id;
    std::optional<CreateAnnotation> annotation;
    std::optional<UpdateNote> note;
    std::size_t page_index = 0;
    double pixels_per_point = 1.0;
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

napi_value verification_value(napi_env env, const WorkspaceVerification& check) {
    napi_value result = nullptr;
    napi_value issues = nullptr;
    if(check.issues.size() > std::numeric_limits<std::uint32_t>::max()
       || napi_create_object(env, &result) != napi_ok
       || !set_boolean(env, result, "valid", check.valid)
       || !set_number(env, result, "documentCount", check.document_count)
       || !set_number(env, result, "documentVersionCount", check.document_version_count)
       || !set_number(env, result, "referencedObjectCount", check.referenced_object_count)
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
       || napi_create_array_with_length(env, annotation.quads.size(), &quads) != napi_ok
       || napi_create_object(env, &quote) != napi_ok
       || !set_string(env, quote, "exact", annotation.quote.exact)
       || !set_string(env, quote, "prefix", annotation.quote.prefix)
       || !set_string(env, quote, "suffix", annotation.quote.suffix)
       || !set_property(env, result, "quote", quote)) {
        return nullptr;
    }
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
            case Operation::extract_page_text:
                store(app.extract_page_text(work.page_index), work);
                break;
            case Operation::create_annotation: store(app.create_annotation(*work.annotation), work); break;
            case Operation::list_annotations:
                store(app.list_annotations(*work.document_version_id), work);
                break;
            case Operation::delete_annotation: store(app.delete_annotation(*work.annotation_id), work); break;
            case Operation::update_note: store(app.update_note(*work.note), work); break;
            case Operation::list_notes: store(app.list_notes(*work.document_version_id), work); break;
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
            } else if constexpr(std::is_same_v<T, PageText>) {
                return page_text_value(env, value);
            } else if constexpr(std::is_same_v<T, AnnotationRecord>) {
                return annotation_value(env, value);
            } else if constexpr(std::is_same_v<T, std::vector<AnnotationRecord>>) {
                return annotations_value(env, value);
            } else if constexpr(std::is_same_v<T, NoteRecord>) {
                return note_value(env, value);
            } else if constexpr(std::is_same_v<T, std::vector<NoteRecord>>) {
                return notes_value(env, value);
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
    return CreateAnnotation{
        .document_version_id = *version_id,
        .page_index = *page_index,
        .quads = std::move(quads),
        .quote = QuoteAnchor{.exact = *exact, .prefix = *prefix, .suffix = *suffix},
        .layout_version = *layout_version,
        .color = color,
    };
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
        case Operation::page_info:
        case Operation::extract_page_text: {
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
        Napi::TypeError::New(env, "Job ID must be a string").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    const auto id = info[0].As<Napi::String>().Utf8Value();
    if(id.empty()) {
        Napi::TypeError::New(env, "Job ID must not be empty").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    const auto& context = *static_cast<AddonContext*>(info.Data());
    return Napi::Boolean::New(env, context.state->cancel_job(id));
}

Napi::Value runtime_info(const Napi::CallbackInfo& info) {
    const auto env = info.Env();
    try {
        if(info.Length() != 0) {
            Napi::TypeError::New(env, "This operation does not accept arguments")
                .ThrowAsJavaScriptException();
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
        result.Set("bindingNapiVersion", Napi::Number::New(env, NAPI_VERSION));
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
    constexpr std::array<const char*, 16> names{
        "createWorkspace",
        "openWorkspace",
        "closeWorkspace",
        "importDocument",
        "listDocuments",
        "openDocument",
        "closeDocument",
        "pageInfo",
        "renderPage",
        "extractPageText",
        "createAnnotation",
        "listAnnotations",
        "deleteAnnotation",
        "updateNote",
        "listNotes",
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
