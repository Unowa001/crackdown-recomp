@echo off
setlocal
cd /d "%~dp0\..\.."
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "AP_VS=%%i"
call "%AP_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%AP_VS%\VC\Tools\Llvm\x64\bin;%PATH%"
cmake -S crackdown/src/ap -B out/ap-state -G Ninja -DCMAKE_CXX_COMPILER=clang++ || exit /b 1
cmake --build out/ap-state || exit /b 1
ctest --test-dir out/ap-state --output-on-failure
