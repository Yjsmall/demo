cmake_minimum_required(VERSION 3.28)

project(
    ContextReader
    VERSION 0.1.0
    DESCRIPTION "Kernel-first contextual PDF reader"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ArchitectureChecks)
include(CompilerWarnings)
include(Asio)

option(CONTEXT_READER_BUILD_TESTS "Build Context Reader tests" ON)
option(CONTEXT_READER_BUILD_NODE "Build the Node-API binding" OFF)
option(CONTEXT_READER_BUILD_TOOLS "Build Context Reader engineering tools" OFF)
option(CONTEXT_READER_ENABLE_MUPDF "Build the MuPDF adapter" OFF)
option(CONTEXT_READER_ENABLE_WORKSPACE "Build the SQLite workspace adapter" OFF)

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
    list(APPEND _context_reader_core_allowed_targets SQLite::SQLite3 bcrypt)
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

if(TARGET sqlite_workspace_test)
    context_reader_assert_target_links_only(sqlite_workspace_test reader_core SQLite::SQLite3)
endif()
