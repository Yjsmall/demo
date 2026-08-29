#include "context_reader/runtime/reader_runtime.hpp"

#include <memory>

namespace context_reader {

Result<std::unique_ptr<ReaderRuntime>> ReaderRuntime::create() {
    return Result<std::unique_ptr<ReaderRuntime>>::success(
        std::unique_ptr<ReaderRuntime>(new ReaderRuntime())
    );
}

}  // namespace context_reader
