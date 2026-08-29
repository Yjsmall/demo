if(NOT CMAKE_JS_INC)
    message(FATAL_ERROR "reader_node must be configured through CMake.js")
endif()

if(MINGW)
    string(REPLACE "/DELAYLOAD:NODE.EXE" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
endif()

add_library(
    reader_node
    SHARED
        src/addon.cpp
        ${CMAKE_JS_SRC}
)

target_include_directories(reader_node PRIVATE ${CMAKE_JS_INC})
target_link_libraries(reader_node PRIVATE context_reader::core)
target_compile_features(reader_node PRIVATE cxx_std_20)
target_compile_definitions(reader_node PRIVATE NODE_GYP_MODULE_NAME=reader_node NAPI_VERSION=8)
context_reader_enable_warnings(reader_node)

set_target_properties(
    reader_node
    PROPERTIES
        PREFIX ""
        SUFFIX ".node"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MINGW)
    find_program(
        CONTEXT_READER_DLLTOOL
        NAMES dlltool
        HINTS "${CONTEXT_READER_MSYS2_ROOT}/ucrt64/bin"
        REQUIRED
    )

    set(_context_reader_electron_import_library "${CMAKE_CURRENT_BINARY_DIR}/libelectron_napi.dll.a")

    add_custom_command(
        OUTPUT "${_context_reader_electron_import_library}"
        COMMAND
            "${CONTEXT_READER_DLLTOOL}"
            --input-def "${CMAKE_CURRENT_SOURCE_DIR}/electron_napi.def"
            --output-lib "${_context_reader_electron_import_library}"
            --dllname electron.exe
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/electron_napi.def"
        VERBATIM
    )

    add_custom_target(
        reader_electron_napi_import
        DEPENDS "${_context_reader_electron_import_library}"
    )
    add_dependencies(reader_node reader_electron_napi_import)
    target_link_libraries(reader_node PRIVATE "${_context_reader_electron_import_library}")
    target_link_options(reader_node PRIVATE -static-libgcc -static-libstdc++)
else()
    target_link_libraries(reader_node PRIVATE ${CMAKE_JS_LIB})
endif()
