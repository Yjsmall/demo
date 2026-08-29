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

option(CONTEXT_READER_BUILD_TESTS "Build Context Reader tests" ON)
option(CONTEXT_READER_BUILD_NODE "Build the Node-API binding" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(kernel)

if(CONTEXT_READER_BUILD_NODE)
    add_subdirectory(bindings/node)
endif()

if(CONTEXT_READER_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

context_reader_assert_target_links_only(reader_core)

if(TARGET reader_node)
    context_reader_assert_target_links_only(reader_node reader_core)
endif()

if(TARGET reader_core_test)
    context_reader_assert_target_links_only(reader_core_test reader_core)
endif()
