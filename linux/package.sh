#!/bin/sh
# Package a stripped glibc driver: libvulkan_radeon.so, its Vulkan ICD file, install.sh,
# README.txt and the third-party notices (NOTICE.txt).
#
# usage: linux/package.sh <stripped libvulkan_radeon.so> <output dir>
set -eu

LIB="$1"
OUT="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if command -v readelf >/dev/null 2>&1 && readelf -S "$LIB" | grep -q -E '\.symtab|\.debug_'; then
   echo "error: $LIB is not stripped" >&2
   exit 1
fi

VERSION=$(tr -d '\r\n' < "$ROOT/VERSION")
HDR="$ROOT/include/vulkan/vulkan_core.h"
MAJOR_MINOR=$(sed -n 's/.*VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION(0, \([0-9]*\), \([0-9]*\), VK_HEADER_VERSION).*/\1.\2/p' "$HDR")
PATCH=$(tr -d '\r' < "$HDR" | sed -n 's/^#define VK_HEADER_VERSION \([0-9]*\)$/\1/p')
COMMIT=""
if [ "$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null)" = "$ROOT" ]; then
   COMMIT=$(git -C "$ROOT" rev-parse --short HEAD)
fi
NAME="radv-xclipse-glibc-$VERSION${COMMIT:+-$COMMIT}"

STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$LIB" "$STAGE/libvulkan_radeon.so"
cat > "$STAGE/radeon_xclipse_icd.aarch64.json" <<EOF
{
    "ICD": {
        "api_version": "$MAJOR_MINOR.$PATCH",
        "library_arch": "64",
        "library_path": "/usr/lib/xclipse/libvulkan_radeon.so"
    },
    "file_format_version": "1.0.1"
}
EOF
cp "$ROOT/linux/install.sh" "$ROOT/linux/README.txt" "$ROOT/linux/NOTICE.txt" "$STAGE/"
chmod 755 "$STAGE/install.sh"
rm -f "$OUT/$NAME.zip"
(cd "$OUT" && python3 -m zipfile -c "$NAME.zip" "$NAME")
rm -rf "$STAGE"
echo "wrote $OUT/$NAME.zip"
