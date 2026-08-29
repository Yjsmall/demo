@echo off

if defined CONTEXT_READER_MSYS2_ROOT goto validate

set "CONTEXT_READER_MSYS2_ROOT=%USERPROFILE%\scoop\apps\msys2\current"
if exist "%CONTEXT_READER_MSYS2_ROOT%\ucrt64\bin\g++.exe" goto activate

set "CONTEXT_READER_MSYS2_ROOT=C:\msys64"

:validate
if exist "%CONTEXT_READER_MSYS2_ROOT%\ucrt64\bin\g++.exe" goto activate

echo MSYS2 UCRT64 was not found. Set CONTEXT_READER_MSYS2_ROOT to the standalone MSYS2 root. 1>&2
exit /b 1

:activate
set "PATH=%CONTEXT_READER_MSYS2_ROOT%\ucrt64\bin;%PATH%"
