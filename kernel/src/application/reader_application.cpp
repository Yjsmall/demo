#include "context_reader/application/reader_application.hpp"

namespace context_reader {

RuntimeInfo ReaderApplication::runtime_info() const noexcept {
    return RuntimeInfo{
        .version = RuntimeVersion{.major = 0, .minor = 1, .patch = 0},
        .application_api_version = 1,
    };
}

}  // namespace context_reader
