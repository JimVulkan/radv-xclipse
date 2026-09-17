@echo off
rem Build the driver for Android (arm64, API 34) and package it into dist\.
rem
rem usage: build.cmd [path-to-android-ndk]
rem   The NDK can also come from ANDROID_NDK_HOME or ANDROID_NDK_ROOT.
rem   BUILD_DIR overrides the build directory (default: build-android).
rem Requires: meson, ninja, python (mako, packaging), glslangValidator, and the NDK's llvm-strip.
setlocal

cd /d "%~dp0"

set "NDK=%~1"
if "%NDK%"=="" set "NDK=%ANDROID_NDK_HOME%"
if "%NDK%"=="" set "NDK=%ANDROID_NDK_ROOT%"
if "%NDK%"=="" goto :no_ndk
if not exist "%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android34-clang.cmd" goto :no_ndk

if not exist "%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" (
   echo error: llvm-strip.exe not found in the NDK 1>&2
   exit /b 1
)

set "BIN=%NDK:\=/%/toolchains/llvm/prebuilt/windows-x86_64/bin"

for %%T in (meson ninja python glslangValidator) do (
   where %%T >nul 2>nul || (
      echo error: %%T not found in PATH 1>&2
      exit /b 1
   )
)

if "%BUILD_DIR%"=="" (set "BUILD=build-android") else (set "BUILD=%BUILD_DIR%")
if not exist "%BUILD%" mkdir "%BUILD%"

> "%BUILD%\cross.ini" (
   echo [binaries]
   echo c = '%BIN%/aarch64-linux-android34-clang.cmd'
   echo cpp = '%BIN%/aarch64-linux-android34-clang++.cmd'
   echo ar = '%BIN%/llvm-ar.exe'
   echo strip = '%BIN%/llvm-strip.exe'
   echo.
   echo [properties]
   echo cpp_link_args = ['-static-libstdc++']
   echo.
   echo [host_machine]
   echo system = 'android'
   echo cpu_family = 'aarch64'
   echo cpu = 'aarch64'
   echo endian = 'little'
)

if exist "%BUILD%\build.ninja" goto :build
call meson setup "%BUILD%" --cross-file "%BUILD%\cross.ini" ^
   -Dbuildtype=debugoptimized -Db_ndebug=true ^
   -Dplatforms=android -Dplatform-sdk-version=34 -Dandroid-stub=true -Dandroid-strict=false ^
   -Dandroid-libbacktrace=disabled ^
   -Dvulkan-drivers=amd -Dgallium-drivers= -Dvulkan-layers= -Dtools= ^
   -Dllvm=disabled -Dzstd=disabled -Dlmsensors=disabled -Dperfetto=false ^
   -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled ^
   -Dallow-fallback-for=libdrm,perfetto --force-fallback-for=expat,libdrm,zlib ^
   -Dlibdrm:default_library=static -Dexpat:default_library=static -Dzlib:default_library=static ^
   -Dc_args=-march=armv8.2-a -Dcpp_args=-march=armv8.2-a
if errorlevel 1 exit /b 1

:build
ninja -C "%BUILD%" src/amd/vulkan/libvulkan_radeon.so
if errorlevel 1 exit /b 1

if not exist dist mkdir dist
"%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" -o "%BUILD%\vulkan.radeon.so" "%BUILD%\src\amd\vulkan\libvulkan_radeon.so"
if errorlevel 1 exit /b 1
python android\package.py "%BUILD%\vulkan.radeon.so" dist
exit /b %errorlevel%

:no_ndk
echo error: pass the Android NDK path or set ANDROID_NDK_HOME 1>&2
exit /b 1
