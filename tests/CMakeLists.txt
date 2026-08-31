add_executable(reader_core_test unit/reader_core_test.cpp)
target_link_libraries(
    reader_core_test
    PRIVATE
        context_reader::core
        Catch2::Catch2WithMain
)
target_compile_features(reader_core_test PRIVATE cxx_std_20)
context_reader_enable_warnings(reader_core_test)

include(Catch)
catch_discover_tests(reader_core_test TEST_PREFIX "reader_core.")

add_executable(pdf_foundation_test unit/pdf_foundation_test.cpp)
target_link_libraries(pdf_foundation_test PRIVATE context_reader::core)
target_compile_features(pdf_foundation_test PRIVATE cxx_std_20)
context_reader_enable_warnings(pdf_foundation_test)

add_test(NAME pdf_foundation_test COMMAND pdf_foundation_test)

if(CONTEXT_READER_ENABLE_MUPDF)
    add_executable(mupdf_engine_test integration/mupdf_engine_test.cpp)
    target_link_libraries(mupdf_engine_test PRIVATE context_reader::core)
    target_compile_features(mupdf_engine_test PRIVATE cxx_std_20)
    target_compile_definitions(
        mupdf_engine_test
        PRIVATE CONTEXT_READER_TEST_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/corpus"
    )
    context_reader_enable_warnings(mupdf_engine_test)

    add_test(NAME mupdf_engine_test COMMAND mupdf_engine_test)

    find_program(CONTEXT_READER_NODE_EXECUTABLE node REQUIRED)
    add_test(
        NAME fixture_manifest_test
        COMMAND
            "${CONTEXT_READER_NODE_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/verify-pdf-fixtures.cjs"
    )

    if(TARGET reader-probe)
        find_program(CONTEXT_READER_PWSH_EXECUTABLE pwsh REQUIRED)
        add_test(
            NAME reader_probe_test
            COMMAND
                "${CONTEXT_READER_PWSH_EXECUTABLE}"
                -NoProfile
                -File "${CMAKE_SOURCE_DIR}/scripts/check-reader-probe.ps1"
                -ProbePath "$<TARGET_FILE:reader-probe>"
                -CorpusRoot "${CMAKE_CURRENT_SOURCE_DIR}/corpus"
                -OutputRoot "${CMAKE_BINARY_DIR}/test-output/reader-probe"
        )
    endif()
endif()

if(CONTEXT_READER_ENABLE_WORKSPACE AND CONTEXT_READER_ENABLE_MUPDF)
    add_executable(sqlite_workspace_test integration/sqlite_workspace_test.cpp)
    target_link_libraries(sqlite_workspace_test PRIVATE context_reader::core)
    target_compile_features(sqlite_workspace_test PRIVATE cxx_std_20)
    target_compile_definitions(
        sqlite_workspace_test
        PRIVATE
            CONTEXT_READER_TEST_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/corpus"
            CONTEXT_READER_TEST_OUTPUT="${CMAKE_BINARY_DIR}/test-output/sqlite-workspace"
    )
    context_reader_enable_warnings(sqlite_workspace_test)
    add_test(NAME sqlite_workspace_test COMMAND sqlite_workspace_test)
endif()
