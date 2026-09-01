cmake_minimum_required(VERSION 3.28)

project(
    ContextReader
    VERSION 0.1.0
    DESCRIPTION "Kernel-first contextual PDF reader"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(
    CONTEXT_READER_DOWNLOAD_CACHE
    "${CMAKE_SOURCE_DIR}/build/download-cache"
    CACHE PATH
    "Shared cache for verified third-party source archives"
)

include(ArchitectureChecks)
include(CompilerWarnings)
include(Asio)

option(CONTEXT_READER_BUILD_TESTS "Build Context Reader tests" ON)
option(CONTEXT_READER_BUILD_NODE "Build the Node-API binding" OFF)
option(CONTEXT_READER_BUILD_TOOLS "Build Context Reader engineering tools" OFF)
option(CONTEXT_READER_ENABLE_MUPDF "Build the MuPDF adapter" OFF)
option(CONTEXT_READER_ENABLE_WORKSPACE "Build the SQLite workspace adapter" OFF)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _context_reader_git_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(_context_reader_git_commit STREQUAL "")
    set(_context_reader_git_commit "unknown")
endif()
file(SHA256 "${CMAKE_SOURCE_DIR}/package-lock.json" _context_reader_lock_hash)
set(_context_reader_ci_run_id "$ENV{GITHUB_RUN_ID}")
if(_context_reader_ci_run_id STREQUAL "")
    set(_context_reader_ci_run_id "$ENV{CI_RUN_ID}")
endif()
if(_context_reader_ci_run_id STREQUAL "")
    set(_context_reader_ci_run_id "local")
endif()
string(
    SHA256 _context_reader_build_hash
    "${_context_reader_git_commit}|${_context_reader_lock_hash}|win32-x64|${_context_reader_ci_run_id}"
)
string(SUBSTRING "${_context_reader_build_hash}" 0 20 _context_reader_build_hash_short)
set(
    CONTEXT_READER_BUILD_ID
    "cr-${_context_reader_build_hash_short}"
    CACHE STRING
    "Generated Build ID embedded in runtime artifacts"
    FORCE
)
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
file(WRITE "${CMAKE_SOURCE_DIR}/build/build-id.txt" "${CONTEXT_READER_BUILD_ID}\n")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CONTEXT_READER_SELECTED_COMPILER STREQUAL "clang")
    add_link_options(-fuse-ld=lld)
endif()

if(CONTEXT_READER_ENABLE_MUPDF)
    include(MuPdf)
endif()

if(CONTEXT_READER_ENABLE_WORKSPACE)
    enable_language(C)
    include(Sqlite)
    include(Miniz)
endif()

add_subdirectory(kernel)

if(CONTEXT_READER_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(CONTEXT_READER_BUILD_NODE)
    add_subdirectory(bindings/node)
endif()

if(CONTEXT_READER_BUILD_TESTS)
    include(CTest)
    include(Catch2)
    add_subdirectory(tests)
endif()

set(_context_reader_core_allowed_targets Asio::Asio)
if(CONTEXT_READER_ENABLE_MUPDF)
    list(APPEND _context_reader_core_allowed_targets MuPDF::MuPDF)
endif()
if(CONTEXT_READER_ENABLE_WORKSPACE)
    list(APPEND _context_reader_core_allowed_targets SQLite::SQLite3 Miniz::Miniz bcrypt ole32 windowscodecs)
endif()
context_reader_assert_target_links_only(reader_core ${_context_reader_core_allowed_targets})
unset(_context_reader_core_allowed_targets)

if(TARGET reader_node)
    context_reader_assert_target_links_only(reader_node reader_core)
endif()

if(TARGET reader_core_test)
    context_reader_assert_target_links_only(
        reader_core_test
        reader_core
        Catch2::Catch2WithMain
    )
endif()

if(TARGET pdf_foundation_test)
    context_reader_assert_target_links_only(pdf_foundation_test reader_core)
endif()

if(TARGET mupdf_engine_test)
    context_reader_assert_target_links_only(mupdf_engine_test reader_core)
endif()

if(TARGET reader-probe)
    context_reader_assert_target_links_only(reader-probe reader_core)
endif()

if(TARGET reader-workspace)
    context_reader_assert_target_links_only(reader-workspace reader_core)
endif()

if(TARGET reader-facade-contract)
    context_reader_assert_target_links_only(reader-facade-contract reader_core)
endif()

if(TARGET reader-bench)
    context_reader_assert_target_links_only(reader-bench reader_core psapi)
endif()

if(TARGET sqlite_workspace_test)
    context_reader_assert_target_links_only(
        sqlite_workspace_test
        reader_core
        SQLite::SQLite3
        Miniz::Miniz
    )
endif()
