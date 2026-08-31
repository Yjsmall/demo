set(CONTEXT_READER_MUPDF_VERSION "1.28.3")
set(CONTEXT_READER_MUPDF_COMMIT "e85b44bee98e322a81d91be2535c2b089f74ebb4")

if(NOT CONTEXT_READER_MUPDF_ROOT AND DEFINED ENV{CONTEXT_READER_MUPDF_ROOT})
    set(CONTEXT_READER_MUPDF_ROOT "$ENV{CONTEXT_READER_MUPDF_ROOT}")
endif()
if(CONTEXT_READER_MUPDF_ROOT AND NOT IS_ABSOLUTE "${CONTEXT_READER_MUPDF_ROOT}")
    cmake_path(
        ABSOLUTE_PATH CONTEXT_READER_MUPDF_ROOT
        BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
        NORMALIZE
    )
endif()
set(
    CONTEXT_READER_MUPDF_ROOT
    "${CONTEXT_READER_MUPDF_ROOT}"
    CACHE PATH
    "Path to the external MuPDF ${CONTEXT_READER_MUPDF_VERSION} source checkout"
)

if(NOT CONTEXT_READER_MUPDF_ROOT)
    message(FATAL_ERROR "CONTEXT_READER_MUPDF_ROOT is required when MuPDF is enabled")
endif()

set(_context_reader_mupdf_header "${CONTEXT_READER_MUPDF_ROOT}/include/mupdf/fitz/version.h")
set(_context_reader_mupdf_library_dir "${CONTEXT_READER_MUPDF_ROOT}/build/context-reader-release")
set(_context_reader_mupdf_library "${_context_reader_mupdf_library_dir}/libmupdf.a")
set(_context_reader_mupdf_third_library "${_context_reader_mupdf_library_dir}/libmupdf-third.a")

foreach(_context_reader_mupdf_file IN ITEMS
    "${_context_reader_mupdf_header}"
    "${_context_reader_mupdf_library}"
    "${_context_reader_mupdf_third_library}"
)
    if(NOT EXISTS "${_context_reader_mupdf_file}")
        message(FATAL_ERROR "Required MuPDF artifact was not found: ${_context_reader_mupdf_file}")
    endif()
endforeach()

file(READ "${_context_reader_mupdf_header}" _context_reader_mupdf_version_header)
if(NOT _context_reader_mupdf_version_header MATCHES "#define FZ_VERSION \"1\\.28\\.3\"")
    message(FATAL_ERROR "MuPDF headers do not report version ${CONTEXT_READER_MUPDF_VERSION}")
endif()

add_library(MuPDF::Core STATIC IMPORTED GLOBAL)
set_target_properties(
    MuPDF::Core
    PROPERTIES
        IMPORTED_LOCATION "${_context_reader_mupdf_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${CONTEXT_READER_MUPDF_ROOT}/include"
)

add_library(MuPDF::ThirdParty STATIC IMPORTED GLOBAL)
set_target_properties(
    MuPDF::ThirdParty
    PROPERTIES
        IMPORTED_LOCATION "${_context_reader_mupdf_third_library}"
)

add_library(MuPDF::MuPDF INTERFACE IMPORTED GLOBAL)
set_target_properties(
    MuPDF::MuPDF
    PROPERTIES
        INTERFACE_LINK_LIBRARIES "MuPDF::Core;MuPDF::ThirdParty"
)

unset(_context_reader_mupdf_file)
unset(_context_reader_mupdf_header)
unset(_context_reader_mupdf_library)
unset(_context_reader_mupdf_library_dir)
unset(_context_reader_mupdf_third_library)
unset(_context_reader_mupdf_version_header)
