include(FetchContent)

set(CONTEXT_READER_CATCH2_VERSION "3.15.0")
set(
    CONTEXT_READER_CATCH2_SHA256
    "9650c55e497759cc39b977e45524bc8acb15256061c112080916ab6cb0b1ea66"
)

FetchContent_Declare(
    Catch2
    URL "https://github.com/catchorg/Catch2/archive/refs/tags/v3.15.0.tar.gz"
    URL_HASH "SHA256=${CONTEXT_READER_CATCH2_SHA256}"
    DOWNLOAD_DIR "${CONTEXT_READER_DOWNLOAD_CACHE}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
