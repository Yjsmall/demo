include(FetchContent)

set(CONTEXT_READER_ASIO_VERSION "1.38.2")
set(
    CONTEXT_READER_ASIO_SHA256
    "9f2648fa483e58a6bf848d970ee0ea650ca19ed7769dfa520ed4f7b8d27af1db"
)

FetchContent_Declare(
    context_reader_asio_source
    URL
        "https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-2.tar.gz"
    URL_HASH "SHA256=${CONTEXT_READER_ASIO_SHA256}"
    DOWNLOAD_DIR "${CONTEXT_READER_DOWNLOAD_CACHE}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR context-reader-no-cmake-project
)
FetchContent_MakeAvailable(context_reader_asio_source)

add_library(context_reader_asio INTERFACE)
add_library(Asio::Asio ALIAS context_reader_asio)
target_include_directories(
    context_reader_asio
    SYSTEM INTERFACE "${context_reader_asio_source_SOURCE_DIR}/include"
)
target_compile_definitions(
    context_reader_asio
    INTERFACE
        ASIO_NO_DEPRECATED
        ASIO_STANDALONE
)
