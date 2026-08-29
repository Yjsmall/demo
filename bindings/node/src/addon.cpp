#include <node_api.h>

#include <cstdio>
#include <exception>
#include <memory>

#include "context_reader/runtime/reader_runtime.hpp"

namespace {

using context_reader::ReaderRuntime;

bool set_named_property(
    napi_env environment,
    napi_value object,
    const char* name,
    napi_value value
) {
    return napi_set_named_property(environment, object, name, value) == napi_ok;
}

napi_value runtime_info_impl(napi_env environment) {
    auto runtime_result = ReaderRuntime::create();
    if(!runtime_result) {
        napi_throw_error(environment, "RUNTIME_CREATE_FAILED", "ReaderRuntime creation failed");
        return nullptr;
    }

    auto runtime = std::move(runtime_result).value();
    const auto info = runtime->application().runtime_info();

    char version[32]{};
    const int written = std::snprintf(
        version,
        sizeof(version),
        "%u.%u.%u",
        info.version.major,
        info.version.minor,
        info.version.patch
    );

    if(written < 0 || static_cast<std::size_t>(written) >= sizeof(version)) {
        napi_throw_error(environment, "VERSION_FORMAT_FAILED", "Runtime version formatting failed");
        return nullptr;
    }

    napi_value result = nullptr;
    napi_value version_value = nullptr;
    napi_value api_version_value = nullptr;

    if(napi_create_object(environment, &result) != napi_ok
       || napi_create_string_utf8(environment, version, NAPI_AUTO_LENGTH, &version_value) != napi_ok
       || napi_create_uint32(environment, info.application_api_version, &api_version_value) != napi_ok
       || !set_named_property(environment, result, "version", version_value)
       || !set_named_property(
           environment,
           result,
           "applicationApiVersion",
           api_version_value
       )) {
        napi_throw_error(environment, "NAPI_RESULT_FAILED", "Failed to create runtime info result");
        return nullptr;
    }

    return result;
}

napi_value runtime_info(napi_env environment, napi_callback_info /* callback_info */) {
    try {
        return runtime_info_impl(environment);
    } catch(const std::exception&) {
        napi_throw_error(environment, "NATIVE_EXCEPTION", "Native runtime operation failed");
    } catch(...) {
        napi_throw_error(environment, "UNKNOWN_NATIVE_EXCEPTION", "Unknown native failure");
    }

    return nullptr;
}

napi_value initialize(napi_env environment, napi_value exports) {
    napi_value runtime_info_function = nullptr;

    if(napi_create_function(
           environment,
           "runtimeInfo",
           NAPI_AUTO_LENGTH,
           runtime_info,
           nullptr,
           &runtime_info_function
       ) != napi_ok
       || !set_named_property(environment, exports, "runtimeInfo", runtime_info_function)) {
        napi_throw_error(environment, "NAPI_INIT_FAILED", "Failed to initialize reader_node");
        return nullptr;
    }

    return exports;
}

}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, initialize)
