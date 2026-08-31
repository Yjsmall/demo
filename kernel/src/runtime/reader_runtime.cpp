#include "context_reader/runtime/reader_runtime.hpp"

#include <memory>

namespace context_reader {

Result<std::unique_ptr<ReaderRuntime>> ReaderRuntime::create() {
    auto executor_result = RuntimeExecutor::create();
    if(!executor_result) {
        return Result<std::unique_ptr<ReaderRuntime>>::failure(executor_result.error());
    }

    return Result<std::unique_ptr<ReaderRuntime>>::success(
        std::unique_ptr<ReaderRuntime>(
            new ReaderRuntime(std::move(executor_result).value())
        )
    );
}

ReaderRuntime::ReaderRuntime(std::unique_ptr<RuntimeExecutor> executor)
    : executor_(std::move(executor)) {}

}  // namespace context_reader
