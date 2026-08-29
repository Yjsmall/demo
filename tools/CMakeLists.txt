if(NOT CONTEXT_READER_ENABLE_MUPDF)
    message(FATAL_ERROR "Context Reader PDF tools require CONTEXT_READER_ENABLE_MUPDF=ON")
endif()
if(NOT WIN32)
    message(FATAL_ERROR "The P1 reader-probe target currently supports Windows only")
endif()

add_executable(reader-probe probe/main.cpp)
target_link_libraries(reader-probe PRIVATE context_reader::core bcrypt)
target_compile_features(reader-probe PRIVATE cxx_std_20)
target_compile_definitions(
    reader-probe
    PRIVATE
        CONTEXT_READER_COMPILER_NAME="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)
target_link_options(reader-probe PRIVATE -municode)
context_reader_enable_warnings(reader-probe)
