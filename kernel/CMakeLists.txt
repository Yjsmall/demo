add_library(
    reader_core
    STATIC
        src/application/reader_application.cpp
        src/pdf/document_session.cpp
        src/pdf/page_geometry.cpp
        src/runtime/runtime_executor.cpp
        src/runtime/reader_runtime.cpp
)

add_library(context_reader::core ALIAS reader_core)

target_include_directories(
    reader_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_features(reader_core PUBLIC cxx_std_20)
target_link_libraries(reader_core PRIVATE Asio::Asio)
context_reader_enable_warnings(reader_core)

if(CONTEXT_READER_ENABLE_MUPDF)
    target_sources(reader_core PRIVATE src/pdf/mupdf_engine.cpp)
    target_link_libraries(reader_core PRIVATE MuPDF::MuPDF)
    target_compile_definitions(reader_core PUBLIC CONTEXT_READER_HAS_MUPDF=1)
endif()

if(CONTEXT_READER_ENABLE_WORKSPACE)
    target_sources(
        reader_core
        PRIVATE
            src/workspace/sqlite_workspace.cpp
    )
    target_link_libraries(reader_core PRIVATE SQLite::SQLite3 bcrypt)
    target_compile_definitions(reader_core PUBLIC CONTEXT_READER_HAS_WORKSPACE=1)
endif()
