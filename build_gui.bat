@echo off
REM Builds the GUI installer (mctde-Installer.exe). Runs from its own dir.
pushd "%~dp0"
call "Z:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist build mkdir build

rc /nologo /fo build\gui.res gui.rc
if errorlevel 1 ( echo RC failed & popd & exit /b 1 )

cl /nologo /std:c++17 /EHsc /MT /W3 /DUNICODE /D_UNICODE /I src /I third_party ^
   src\Gui.cpp src\Bhd5.cpp src\Bnd3.cpp src\C4110Header.cpp src\Detect.cpp src\Dcx.cpp ^
   src\Download.cpp src\Extract.cpp src\ExePatch.cpp src\ExePatchTable.cpp src\Installer.cpp ^
   src\NameHash.cpp src\NamelistData.cpp src\Sha256.cpp src\Unpacker.cpp src\Update.cpp third_party\miniz.c build\gui.res ^
   /Fe:build\mctde-Installer.exe /Fo"build\\" ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO
popd
