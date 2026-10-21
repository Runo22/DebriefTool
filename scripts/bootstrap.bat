@echo off
setlocal EnableDelayedExpansion

rem All dependencies are vendored in third_party\ -- no network access needed.
echo [afteraction] Configuring CMake (Visual Studio 2022, x64)...
if not exist build mkdir build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 (echo ERROR: CMake configure failed && exit /b 1)

echo.
echo [afteraction] Done.  Open  build\afteraction.sln  in Visual Studio, or run:
echo    cmake --build build --config RelWithDebInfo --parallel
