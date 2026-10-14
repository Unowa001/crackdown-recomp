@echo off
rem Double-click: builds whatever is missing, then runs Crackdown. Output also goes to run.log.
setlocal
cd /d "%~dp0"

if not exist "game\default.xex" (
  echo Put your extracted game files in "%~dp0game\" first ^(Default.xex must be inside^).
  pause & exit /b 1
)

rem Load the VS 2022 x64 toolchain (clang, cmake, ninja)
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%i"
if not defined VS ( echo Visual Studio 2022 not found. & pause & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || ( pause & exit /b 1 )
rem vcvars doesn't put Clang on PATH; the presets call plain "clang". Standalone LLVM wins over VS's bundled one.
set "PATH=%ProgramFiles%\LLVM\bin;%VS%\VC\Tools\Llvm\x64\bin;%PATH%"
where clang >nul 2>&1 || (
  echo Clang not found. Run:  winget install LLVM.LLVM   then run this again.
  pause & exit /b 1
)
rem a failed configure leaves a cache pinned to the missing compiler; start clean
if exist "sdk\out\build\win-amd64\CMakeCache.txt" if not exist "sdk\out\install\win-amd64\bin\rexglue.exe" rmdir /s /q "sdk\out\build\win-amd64"

if not exist "sdk\CMakeLists.txt" git submodule update --init --recursive || ( pause & exit /b 1 )

rem Two SDK deps contain git symlinks. Without Developer Mode + core.symlinks, Windows checks them out
rem as one-line text files and the build fails (e.g. lzxd.c: "expected identifier"). Re-checkout as real links.
dir /AL "sdk\thirdparty\libmspack\cabextract\mspack\lzxd.c" >nul 2>&1 || (
  for %%m in (libmspack o1heap) do (
    git -C sdk\thirdparty\%%m config core.symlinks true
    git -C sdk\thirdparty\%%m rm -r -q --cached .
    git -C sdk\thirdparty\%%m reset -q --hard
  )
  dir /AL "sdk\thirdparty\libmspack\cabextract\mspack\lzxd.c" >nul 2>&1 || (
    echo Git could not create symlinks. Turn on Developer Mode:
    echo   Settings ^> System ^> For developers ^> Developer Mode = On
    echo then run this again.
    pause & exit /b 1
  )
)

rem SDK fixes not upstream yet (patches\sdk\*.patch, applied in name order). A patch that already
rem reverse-applies is in. A newly applied one forces an SDK rebuild and fresh codegen: the codegen
rem stamp fingerprints only the SDK version string, which patching doesn't change.
set "SDK_PATCHED="
for %%p in ("%~dp0patches\sdk\*.patch") do (
  git -C sdk apply --unidiff-zero --reverse --check "%%~fp" >nul 2>&1 || (
    echo Applying SDK patch %%~nxp
    git -C sdk apply --unidiff-zero "%%~fp" || ( echo Could not apply --unidiff-zero %%~nxp. Is sdk\ on the pinned tag? & pause & exit /b 1 )
    set "SDK_PATCHED=1"
  )
)
rem Every patch must reverse-apply on its own with all of them in, or the next run re-applies one and fails.
for %%p in ("%~dp0patches\sdk\*.patch") do (
  git -C sdk apply --unidiff-zero --reverse --check "%%~fp" >nul 2>&1 || (
    echo %%~nxp no longer reverse-applies with all SDK patches in. Regenerate the later patch that edits next to it.
    pause & exit /b 1
  )
)
if defined SDK_PATCHED (
  del /q "sdk\out\install\win-amd64\bin\rexglue.exe" 2>nul
  del /q "crackdown\generated\default\codegen.stamp" "crackdown\generated\default\codegen.build.stamp" 2>nul
  rem SDK DLLs (the GPU plugin) are copied next to the exe only when it relinks
  del /q "crackdown\out\build\win-amd64-release\crackdown.exe" 2>nul
)

rem ponytail: SDK builds once; delete sdk\out to force a rebuild after bumping the SDK
if not exist "sdk\out\install\win-amd64\bin\rexglue.exe" (
  echo === Building ReXGlue SDK ^(one-time, slow^) ===
  pushd sdk
  cmake --preset win-amd64 || ( popd & pause & exit /b 1 )
  cmake --build out\build\win-amd64 --config Release --target install || ( popd & pause & exit /b 1 )
  popd
)

pushd crackdown
echo === Codegen ===
..\sdk\out\install\win-amd64\bin\rexglue.exe codegen crackdown_manifest.toml || ( popd & pause & exit /b 1 )
echo === Building Crackdown ===
if not exist "out\build\win-amd64-release" cmake --preset win-amd64-release || ( popd & pause & exit /b 1 )
cmake --build --preset win-amd64-release || ( popd & pause & exit /b 1 )

echo === Running ^(log: run.log^) ===
powershell -NoProfile -Command "& '.\out\build\win-amd64-release\crackdown.exe' --game_data_root=..\game --gpu_plugin=xenos 2>&1 | Tee-Object -FilePath ..\run.log"
popd
echo.
echo Exited. Send the last lines of run.log.
pause
