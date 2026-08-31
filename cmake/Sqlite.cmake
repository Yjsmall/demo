include(FetchContent)

set(CONTEXT_READER_SQLITE_VERSION "3.53.4")
set(CONTEXT_READER_SQLITE_ARCHIVE_VERSION "3530400")
set(
    CONTEXT_READER_SQLITE_SHA3_256
    "628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"
)

FetchContent_Declare(
    context_reader_sqlite
    URL
        "https://sqlite.org/2026/sqlite-amalgamation-${CONTEXT_READER_SQLITE_ARCHIVE_VERSION}.zip"
    URL_HASH "SHA3_256=${CONTEXT_READER_SQLITE_SHA3_256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR context-reader-no-cmake-project
)
FetchContent_MakeAvailable(context_reader_sqlite)

add_library(context_reader_sqlite3 STATIC "${context_reader_sqlite_SOURCE_DIR}/sqlite3.c")
add_library(SQLite::SQLite3 ALIAS context_reader_sqlite3)
target_include_directories(
    context_reader_sqlite3
    PUBLIC "${context_reader_sqlite_SOURCE_DIR}"
)
target_compile_features(context_reader_sqlite3 PRIVATE c_std_11)
target_compile_definitions(
    context_reader_sqlite3
    PRIVATE
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_DQS=0
        SQLITE_OMIT_DEPRECATED=1
        SQLITE_OMIT_LOAD_EXTENSION=1
        SQLITE_THREADSAFE=1
)
set_target_properties(
    context_reader_sqlite3
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)
