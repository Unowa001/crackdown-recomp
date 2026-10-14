@echo off
setlocal
cd /d "%~dp0\..\.."
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "AP_VS=%%i"
call "%AP_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%AP_VS%\VC\Tools\Llvm\x64\bin;%PATH%"
rem Uses existing generated sources and installed SDK; never patches/builds the SDK.
cmake --build crackdown/out/build/win-amd64-release --target crackdown
