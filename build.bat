@echo off
REM Standalone MSVC build (no CMake required). Runs from its own directory.
pushd "%~dp0"
call "Z:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Failed to initialize MSVC environment.
    exit /b 1
)
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W3 /I src /I third_party ^
   src\main.cpp src\Bhd5.cpp src\Bnd3.cpp src\C4110Header.cpp src\Detect.cpp src\Dcx.cpp src\Download.cpp src\Extract.cpp src\ExePatch.cpp src\ExePatchTable.cpp src\Installer.cpp src\NameHash.cpp src\NamelistData.cpp src\Sha256.cpp src\Unpacker.cpp third_party\miniz.c ^
   /Fe:build\mctde-installer.exe /Fo"build\\" ^
   /link /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:app.manifest
popd
