RADV for Samsung Xclipse -- glibc Linux build (Termux PRoot)
=============================================================

The Vulkan driver for Samsung Xclipse GPUs (920, 530, 940, 540, 550, 950, 960), built for glibc
Linux distributions running on the phone: Debian, Ubuntu and others under Termux + proot-distro.
Same driver as the Android radv-xclipse package.

Needs glibc 2.29 or newer (Debian 11+, Ubuntu 20.04+), the Vulkan loader (libvulkan1) and the
X11/xcb and Wayland client libraries, which any desktop install already has. The driver opens
/dev/dri/renderD128 directly; proot-distro binds /dev by default.

Install (inside the distribution, as root)
------------------------------------------
    unzip radv-xclipse-glibc-*.zip
    sh radv-xclipse-glibc-*/install.sh

This copies the driver to /usr/lib/xclipse/ and its ICD file to
/usr/share/vulkan/icd.d/radeon_xclipse_icd.aarch64.json.

Use
---
If the distribution's own Mesa Vulkan drivers are installed too, point the loader at this one:

    export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_xclipse_icd.aarch64.json
    vulkaninfo --summary        # AMD Radeon Graphics (RADV <codename>)

X11, with Termux:X11 (in Termux: termux-x11 :0 &, then open the Termux:X11 app):

    proot-distro login debian --shared-tmp
    DISPLAY=:0 vkcube

Wayland: run any compositor inside the distribution (for example weston or labwc on the X
display), then start Vulkan apps with WAYLAND_DISPLAY set to its socket.

Presentation
------------
- X servers with DRI3 (Termux:X11): the GPU renders into a buffer the X server reads directly.
- X servers without DRI3 (VNC and similar) and Wayland compositors without linux-dmabuf (most
  compositors inside PRoot): the frame is copied by the CPU and sent with PutImage or wl_shm.
  Chosen automatically per window; MESA_VK_WSI_DEBUG=sw forces the CPU copy everywhere.

Diagnostics
-----------
The Android build's debug.* properties are environment variables here: drop "debug.", use upper
case. RADV_XCLIPSE_LOG=1 prints the [BUILD] line and the diagnostic markers to stderr;
RADV_XCLIPSE_LOG=2 adds the verbose trace.

Licenses: see NOTICE.txt. The driver is MIT licensed (Mesa).
