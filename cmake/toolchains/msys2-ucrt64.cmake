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
    if(EXISTS "${_context_reader_candidate}/ucrt64/bin/g++.exe")
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

set(CMAKE_C_COMPILER "${_context_reader_ucrt_bin}/gcc.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_context_reader_ucrt_bin}/g++.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${_context_reader_ucrt_bin}/windres.exe" CACHE FILEPATH "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${CONTEXT_READER_MSYS2_ROOT}/ucrt64")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

unset(_context_reader_candidate)
unset(_context_reader_msys2_candidates)
unset(_context_reader_ucrt_bin)
