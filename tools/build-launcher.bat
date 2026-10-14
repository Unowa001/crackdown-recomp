@echo off
setlocal
cd /d "%~dp0.."
if not defined LAUNCHER_VS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "LAUNCHER_VS=%%i"
if not defined LAUNCHER_VS exit /b 1
call "%LAUNCHER_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%LAUNCHER_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%LAUNCHER_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S crackdown/launcher -B crackdown/launcher/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
if errorlevel 1 exit /b 1
cmake --build crackdown/launcher/out
if errorlevel 1 exit /b 1
ctest --test-dir crackdown/launcher/out --output-on-failure
if errorlevel 1 exit /b 1
copy /y tools\game-manifest.txt crackdown\launcher\out\game-manifest.txt >nul
exit /b %errorlevel%
