add_library(
    reader_core
    STATIC
        src/application/reader_application.cpp
        src/runtime/reader_runtime.cpp
)

add_library(context_reader::core ALIAS reader_core)

target_include_directories(
    reader_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_features(reader_core PUBLIC cxx_std_20)
context_reader_enable_warnings(reader_core)
