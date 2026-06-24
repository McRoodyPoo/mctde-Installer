@echo off
REM Preview build (asInvoker, so it launches without a UAC prompt) for visual checks only.
pushd "%~dp0"
call "Z:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist build mkdir build
rc /nologo /fo build\gui_preview.res gui_preview.rc
if errorlevel 1 ( echo RC failed & popd & exit /b 1 )
cl /nologo /std:c++17 /EHsc /W3 /DUNICODE /D_UNICODE /I src /I third_party ^
   src\Gui.cpp src\Bhd5.cpp src\Bnd3.cpp src\C4110Header.cpp src\Detect.cpp src\Dcx.cpp ^
   src\Download.cpp src\Extract.cpp src\ExePatch.cpp src\ExePatchTable.cpp src\Installer.cpp ^
   src\NameHash.cpp src\NamelistData.cpp src\Sha256.cpp src\Unpacker.cpp third_party\miniz.c build\gui_preview.res ^
   /Fe:build\mctde-gui-preview.exe /Fo"build\\" ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO
popd
