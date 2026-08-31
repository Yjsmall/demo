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

if(CONTEXT_READER_ENABLE_WORKSPACE)
    add_executable(reader-workspace workspace/main.cpp)
    target_link_libraries(reader-workspace PRIVATE context_reader::core)
    target_compile_features(reader-workspace PRIVATE cxx_std_20)
    target_link_options(reader-workspace PRIVATE -municode)
    context_reader_enable_warnings(reader-workspace)

    add_executable(reader-facade-contract contracts/facade_main.cpp)
    target_link_libraries(reader-facade-contract PRIVATE context_reader::core)
    target_compile_features(reader-facade-contract PRIVATE cxx_std_20)
    target_link_options(reader-facade-contract PRIVATE -municode)
    context_reader_enable_warnings(reader-facade-contract)
endif()
