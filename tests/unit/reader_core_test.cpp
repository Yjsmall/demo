#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "context_reader/runtime/reader_runtime.hpp"
#include "context_reader/shared/error.hpp"
#include "context_reader/shared/result.hpp"
#include "context_reader/shared/stable_id.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if(!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    using namespace context_reader;

    auto runtime_result = ReaderRuntime::create();
    check(runtime_result.has_value(), "runtime creation succeeds");

    if(runtime_result) {
        auto runtime = std::move(runtime_result).value();
        const auto info = runtime->application().runtime_info();
        check(info.version == RuntimeVersion{0, 1, 0}, "runtime reports semantic version");
        check(info.application_api_version == 2U, "runtime reports facade API version");
    }

    const auto failure = Result<int>::failure(Error(ErrorCode::not_found, "missing"));
    check(!failure.has_value(), "result preserves failure state");
    check(failure.error().code() == ErrorCode::not_found, "result preserves error code");

    DocumentId::Bytes document_bytes{};
    document_bytes.back() = std::uint8_t{1};
    const auto document_id = DocumentId::from_bytes(document_bytes);
    check(!document_id.is_nil(), "stable ID distinguishes non-nil values");
    check(document_id == DocumentId::from_bytes(document_bytes), "stable ID equality is value based");

    if(failures == 0) {
        std::cout << "reader_core_test passed\n";
    }

    return failures == 0 ? 0 : 1;
}
