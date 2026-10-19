#!/bin/sh
# Build the driver for glibc Linux on arm64 (Termux PRoot distributions) and package it into dist/.
#
# usage: ./build.sh                  native build, inside the distribution on the phone
#        ./build.sh <cross-file>     cross build from another Linux machine (see README.md)
#   BUILD_DIR overrides the build directory (default: build-glibc); MESON_ARGS adds meson setup
#   options.
# Requires: meson (1.4.0 or newer), ninja, python3 with mako and packaging, glslangValidator,
# pkg-config, a C/C++ compiler, and the X11/xcb and Wayland development packages (README.md).
set -eu

cd "$(dirname "$0")"

CROSS="${1:-}"
if [ -n "$CROSS" ] && [ ! -f "$CROSS" ]; then
   echo "error: cross file $CROSS not found" >&2
   exit 1
fi
if [ -z "$CROSS" ] && [ "$(uname -m)" != "aarch64" ]; then
   echo "error: this is not an arm64 machine; pass a cross file (see README.md)" >&2
   exit 1
fi

for tool in meson ninja python3 glslangValidator pkg-config; do
   if ! command -v "$tool" >/dev/null 2>&1; then
      echo "error: $tool not found in PATH" >&2
      exit 1
   fi
done

BUILD="${BUILD_DIR:-build-glibc}"

if [ ! -f "$BUILD/build.ninja" ]; then
   set -- -Dbuildtype=release -Db_ndebug=true -Dprefix=/usr \
      -Dplatforms=x11,wayland -Dvulkan-drivers=amd -Dgallium-drivers= -Dvulkan-layers= -Dtools= \
      -Dllvm=disabled -Dzstd=disabled -Dlmsensors=disabled -Dperfetto=false -Dvalgrind=disabled \
      -Dlibunwind=disabled -Dxmlconfig=enabled \
      -Dglx=disabled -Dgbm=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
      -Dallow-fallback-for=libdrm --force-fallback-for=expat,libdrm,zlib,wayland-protocols \
      -Dlibdrm:default_library=static -Dexpat:default_library=static -Dzlib:default_library=static \
      -Dc_args=-march=armv8.2-a -Dcpp_args=-march=armv8.2-a
   if [ -n "$CROSS" ]; then
      set -- --cross-file "$CROSS" "$@"
   fi
   # shellcheck disable=SC2086
   meson setup "$BUILD" "$@" ${MESON_ARGS:-}
fi

ninja -C "$BUILD" src/amd/vulkan/libvulkan_radeon.so

STRIP=strip
if [ -n "$CROSS" ]; then
   STRIP=$(sed -n "s/^strip *= *'\(.*\)'.*/\1/p" "$CROSS" | head -n 1)
   [ -n "$STRIP" ] || STRIP=llvm-strip
fi
"$STRIP" -o "$BUILD/libvulkan_radeon.so" "$BUILD/src/amd/vulkan/libvulkan_radeon.so"
sh linux/package.sh "$BUILD/libvulkan_radeon.so" dist
