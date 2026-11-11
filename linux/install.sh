#!/bin/sh
# Inside the PRoot distribution, as root: installs RADV Xclipse as a Vulkan driver.
set -e
dir="$(cd "$(dirname "$0")" && pwd)"
mkdir -p /usr/lib/xclipse /usr/share/vulkan/icd.d
cp "$dir/libvulkan_radeon.so" /usr/lib/xclipse/
cp "$dir/radeon_xclipse_icd.aarch64.json" /usr/share/vulkan/icd.d/
echo "installed /usr/lib/xclipse/libvulkan_radeon.so"
echo "and /usr/share/vulkan/icd.d/radeon_xclipse_icd.aarch64.json"
