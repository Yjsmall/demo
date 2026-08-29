set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_context_reader_msys2_candidates)

if(DEFINED ENV{CONTEXT_READER_MSYS2_ROOT})
    list(APPEND _context_reader_msys2_candidates "$ENV{CONTEXT_READER_MSYS2_ROOT}")
endif()

if(DEFINED ENV{MSYS2_ROOT})
    list(APPEND _context_reader_msys2_candidates "$ENV{MSYS2_ROOT}")
endif()

if(DEFINED ENV{USERPROFILE})
    list(APPEND _context_reader_msys2_candidates "$ENV{USERPROFILE}/scoop/apps/msys2/current")
endif()

list(APPEND _context_reader_msys2_candidates "C:/msys64")

foreach(_context_reader_candidate IN LISTS _context_reader_msys2_candidates)
    file(TO_CMAKE_PATH "${_context_reader_candidate}" _context_reader_candidate)
    if(
        EXISTS "${_context_reader_candidate}/ucrt64/bin/clang++.exe"
        OR EXISTS "${_context_reader_candidate}/ucrt64/bin/g++.exe"
    )
        set(CONTEXT_READER_MSYS2_ROOT "${_context_reader_candidate}")
        break()
    endif()
endforeach()

if(NOT CONTEXT_READER_MSYS2_ROOT)
    message(
        FATAL_ERROR
        "MSYS2 UCRT64 was not found. Set CONTEXT_READER_MSYS2_ROOT to the standalone MSYS2 root."
    )
endif()

set(_context_reader_ucrt_bin "${CONTEXT_READER_MSYS2_ROOT}/ucrt64/bin")

if(NOT DEFINED CONTEXT_READER_COMPILER AND DEFINED ENV{CONTEXT_READER_COMPILER})
    set(CONTEXT_READER_COMPILER "$ENV{CONTEXT_READER_COMPILER}")
endif()
set(
    CONTEXT_READER_COMPILER
    "${CONTEXT_READER_COMPILER}"
    CACHE STRING
    "UCRT64 compiler selection: auto, clang, or gcc"
)
if(NOT CONTEXT_READER_COMPILER)
    set(CONTEXT_READER_COMPILER "auto" CACHE STRING "" FORCE)
endif()
set_property(CACHE CONTEXT_READER_COMPILER PROPERTY STRINGS auto clang gcc)

if(CONTEXT_READER_COMPILER STREQUAL "auto")
    if(EXISTS "${_context_reader_ucrt_bin}/clang++.exe")
        set(_context_reader_selected_compiler "clang")
    else()
        set(_context_reader_selected_compiler "gcc")
    endif()
elseif(CONTEXT_READER_COMPILER STREQUAL "clang" OR CONTEXT_READER_COMPILER STREQUAL "gcc")
    set(_context_reader_selected_compiler "${CONTEXT_READER_COMPILER}")
else()
    message(FATAL_ERROR "CONTEXT_READER_COMPILER must be auto, clang, or gcc")
endif()

if(_context_reader_selected_compiler STREQUAL "clang")
    set(_context_reader_c_compiler "${_context_reader_ucrt_bin}/clang.exe")
    set(_context_reader_cxx_compiler "${_context_reader_ucrt_bin}/clang++.exe")
else()
    set(_context_reader_c_compiler "${_context_reader_ucrt_bin}/gcc.exe")
    set(_context_reader_cxx_compiler "${_context_reader_ucrt_bin}/g++.exe")
endif()

if(NOT EXISTS "${_context_reader_cxx_compiler}")
    message(
        FATAL_ERROR
        "Requested UCRT64 ${_context_reader_selected_compiler} compiler was not found: ${_context_reader_cxx_compiler}"
    )
endif()

set(CMAKE_C_COMPILER "${_context_reader_c_compiler}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_context_reader_cxx_compiler}" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${_context_reader_ucrt_bin}/windres.exe" CACHE FILEPATH "" FORCE)
set(
    CONTEXT_READER_SELECTED_COMPILER
    "${_context_reader_selected_compiler}"
    CACHE INTERNAL
    "Selected Context Reader UCRT64 compiler"
    FORCE
)

set(CMAKE_FIND_ROOT_PATH "${CONTEXT_READER_MSYS2_ROOT}/ucrt64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

unset(_context_reader_candidate)
unset(_context_reader_c_compiler)
unset(_context_reader_cxx_compiler)
unset(_context_reader_msys2_candidates)
unset(_context_reader_selected_compiler)
unset(_context_reader_ucrt_bin)
