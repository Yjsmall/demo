#include <node_api.h>

#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "context_reader/runtime/reader_runtime.hpp"

namespace {

using namespace context_reader;

enum class Operation {
    create_workspace,
    open_workspace,
    close_workspace,
    import_document,
    list_documents,
    verify_workspace,
};

struct RuntimeState final {
    explicit RuntimeState(std::unique_ptr<ReaderRuntime> value) : runtime(std::move(value)) {}
    std::unique_ptr<ReaderRuntime> runtime;
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
              {this, Operation::verify_workspace},
          }} {}

    std::shared_ptr<RuntimeState> state;
    std::array<FunctionData, 6> functions;
};

using Payload = std::variant<
    std::monostate,
    WorkspaceInfo,
    ImportDocumentResult,
    std::vector<DocumentRecord>,
    WorkspaceVerification>;

struct AsyncWork final {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::shared_ptr<RuntimeState> state;
    Operation operation;
    std::filesystem::path path;
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

bool set_boolean(napi_env env, napi_value object, const char* name, bool value) {
    napi_value property = nullptr;
    return napi_get_boolean(env, value, &property) == napi_ok
        && set_property(env, object, name, property);
}

template <typename Tag>
std::string id_string(const StableId<Tag>& id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(32, '0');
    for(std::size_t index = 0; index < id.bytes().size(); ++index) {
        const auto byte = id.bytes()[index];
        result[index * 2] = digits[byte >> 4U];
        result[index * 2 + 1] = digits[byte & 0x0FU];
    }
    return result;
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
       || !set_string(env, result, "id", id_string(info.id))
       || napi_create_uint32(env, info.schema_version, &version) != napi_ok
       || !set_property(env, result, "schemaVersion", version)) {
        return nullptr;
    }
    return result;
}

napi_value document_value(napi_env env, const DocumentRecord& document) {
    napi_value result = nullptr;
    if(napi_create_object(env, &result) != napi_ok
       || !set_string(env, result, "documentId", id_string(document.document_id))
       || !set_string(env, result, "versionId", id_string(document.version_id))
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
            case Operation::import_document: store(app.import_document(work.path), work); break;
            case Operation::list_documents: store(app.list_documents(), work); break;
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
            } else if constexpr(std::is_same_v<T, std::vector<DocumentRecord>>) {
                return documents_value(env, value);
            } else {
                return verification_value(env, value);
            }
        },
        payload
    );
}

void complete(napi_env env, napi_status status, void* data) {
    std::unique_ptr<AsyncWork> work(static_cast<AsyncWork*>(data));
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

std::optional<std::string> path_argument(
    napi_env env,
    napi_callback_info info,
    void** function_data
) {
    std::size_t count = 1;
    napi_value argument = nullptr;
    if(napi_get_cb_info(env, info, &count, &argument, nullptr, function_data) != napi_ok
       || count != 1) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "Expected one path argument");
        return std::nullopt;
    }
    napi_valuetype type = napi_undefined;
    if(napi_typeof(env, argument, &type) != napi_ok || type != napi_string) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "Path must be a string");
        return std::nullopt;
    }
    std::size_t size = 0;
    if(napi_get_value_string_utf8(env, argument, nullptr, 0, &size) != napi_ok) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "Path must be valid UTF-8");
        return std::nullopt;
    }
    std::string value(size + 1, '\0');
    std::size_t written = 0;
    if(napi_get_value_string_utf8(env, argument, value.data(), value.size(), &written) != napi_ok) {
        napi_throw_type_error(env, "INVALID_ARGUMENT", "Path must be valid UTF-8");
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

napi_value schedule(napi_env env, napi_callback_info info) {
    void* raw_data = nullptr;
    std::size_t count = 0;
    if(napi_get_cb_info(env, info, &count, nullptr, nullptr, &raw_data) != napi_ok) {
        napi_throw_error(env, "NAPI_ARGUMENT_FAILED", "Failed to read arguments");
        return nullptr;
    }
    const auto& function = *static_cast<FunctionData*>(raw_data);
    std::optional<std::string> path;
    if(function.operation == Operation::create_workspace
       || function.operation == Operation::open_workspace
       || function.operation == Operation::import_document) {
        path = path_argument(env, info, &raw_data);
        if(!path) {
            return nullptr;
        }
    }

    auto work = std::make_unique<AsyncWork>();
    work->state = function.context->state;
    work->operation = function.operation;
    if(path) {
        work->path = utf8_path(*path);
    }
    napi_value promise = nullptr;
    napi_value resource_name = nullptr;
    if(napi_create_promise(env, &work->deferred, &promise) != napi_ok
       || napi_create_string_utf8(env, "context_reader.operation", NAPI_AUTO_LENGTH, &resource_name) != napi_ok
       || napi_create_async_work(
           env, nullptr, resource_name, execute, complete, work.get(), &work->work
       ) != napi_ok
       || napi_queue_async_work(env, work->work) != napi_ok) {
        if(work->work != nullptr) {
            napi_delete_async_work(env, work->work);
        }
        napi_throw_error(env, "NAPI_ASYNC_FAILED", "Failed to schedule native operation");
        return nullptr;
    }
    work.release();
    return promise;
}

napi_value runtime_info(napi_env env, napi_callback_info info) {
    try {
        void* raw_data = nullptr;
        std::size_t count = 0;
        if(napi_get_cb_info(env, info, &count, nullptr, nullptr, &raw_data) != napi_ok) {
            napi_throw_error(env, "NAPI_ARGUMENT_FAILED", "Failed to read arguments");
            return nullptr;
        }
        const auto& context = *static_cast<AddonContext*>(raw_data);
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
            napi_throw_error(env, "VERSION_FORMAT_FAILED", "Runtime version formatting failed");
            return nullptr;
        }
        napi_value result = nullptr;
        napi_value version_value = nullptr;
        napi_value api_version = nullptr;
        napi_value binding_version = nullptr;
        if(napi_create_object(env, &result) != napi_ok
           || napi_create_string_utf8(env, version, NAPI_AUTO_LENGTH, &version_value) != napi_ok
           || napi_create_uint32(env, runtime.application_api_version, &api_version) != napi_ok
           || napi_create_uint32(env, NAPI_VERSION, &binding_version) != napi_ok
           || !set_property(env, result, "version", version_value)
           || !set_property(env, result, "applicationApiVersion", api_version)
           || !set_property(env, result, "bindingNapiVersion", binding_version)) {
            napi_throw_error(env, "NAPI_RESULT_FAILED", "Failed to create runtime info");
            return nullptr;
        }
        return result;
    } catch(...) {
        napi_throw_error(env, "NATIVE_EXCEPTION", "Native runtime operation failed");
        return nullptr;
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

napi_value initialize(napi_env env, napi_value exports) {
    auto runtime = ReaderRuntime::create();
    if(!runtime) {
        napi_throw_error(env, "RUNTIME_CREATE_FAILED", "ReaderRuntime creation failed");
        return nullptr;
    }
    auto context = std::make_unique<AddonContext>(
        std::make_shared<RuntimeState>(std::move(runtime).value())
    );
    constexpr std::array<const char*, 6> names{
        "createWorkspace",
        "openWorkspace",
        "closeWorkspace",
        "importDocument",
        "listDocuments",
        "verifyWorkspace",
    };
    if(!export_function(env, exports, "runtimeInfo", runtime_info, context.get())) {
        napi_throw_error(env, "NAPI_INIT_FAILED", "Failed to initialize reader_node");
        return nullptr;
    }
    for(std::size_t index = 0; index < names.size(); ++index) {
        if(!export_function(env, exports, names[index], schedule, &context->functions[index])) {
            napi_throw_error(env, "NAPI_INIT_FAILED", "Failed to initialize reader_node");
            return nullptr;
        }
    }
    if(napi_add_env_cleanup_hook(env, cleanup, context.get()) != napi_ok) {
        napi_throw_error(env, "NAPI_INIT_FAILED", "Failed to register cleanup hook");
        return nullptr;
    }
    context.release();
    return exports;
}

}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, initialize)
