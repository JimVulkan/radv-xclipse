# RADV for Samsung Xclipse (glibc Linux)

This branch builds Mesa's RADV Vulkan driver for Samsung Xclipse GPUs as a regular glibc Linux
driver, for Debian, Ubuntu and other distributions running on the phone under Termux and
proot-distro. It is the same driver as the Android build on `main` (Mesa 27.0.0-devel), with
X11 and Wayland presentation instead of Android's.

## Requirements

On the phone, inside the distribution (Debian 12 or Ubuntu 24.04 and newer have every package):

```sh
apt install build-essential pkg-config python3-venv glslang-tools \
   libx11-xcb-dev libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-randr0-dev \
   libxcb-shm0-dev libxcb-sync-dev libxcb-xfixes0-dev libxrandr-dev libxshmfence-dev \
   libwayland-dev libwayland-bin
python3 -m venv ~/mesa-venv
~/mesa-venv/bin/pip install meson ninja mako packaging
export PATH=~/mesa-venv/bin:$PATH
```

Meson 1.4.0 or newer is needed, which is why it comes from pip. The first configure downloads
libdrm 2.4.133, zlib 1.3.1, Expat 2.5.0 and wayland-protocols 1.41, so it needs network access.
libdrm, zlib and Expat are linked statically.

## Building

```sh
./build.sh
```

The build goes to `build-glibc/` (set `BUILD_DIR` to change it; `MESON_ARGS` adds options to
the configure step). The script builds the driver,
strips it, and writes `dist/radv-xclipse-glibc-<version>-<commit>.zip` with the driver, its
Vulkan ICD file, `install.sh`, a short README and the third-party notices.

### Cross building

From an x86-64 Linux machine, build against an arm64 Debian or Ubuntu root filesystem that has
the packages above. Pass a Meson cross file:

```ini
[binaries]
c = ['clang', '--target=aarch64-linux-gnu', '--sysroot=/path/to/arm64-root']
cpp = ['clang++', '--target=aarch64-linux-gnu', '--sysroot=/path/to/arm64-root']
c_ld = 'lld'
cpp_ld = 'lld'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'

[properties]
sys_root = '/path/to/arm64-root'
pkg_config_libdir = ['/path/to/arm64-root/usr/lib/aarch64-linux-gnu/pkgconfig',
                     '/path/to/arm64-root/usr/share/pkgconfig']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
```

```sh
./build.sh aarch64-linux-gnu.ini
```

The build machine's `wayland-scanner` must be the same version as the root filesystem's
libwayland; if its `.pc` file is not in the default search path, add
`MESON_ARGS=-Dbuild.pkg_config_path=/its/pkgconfig/dir`.

The driver needs the glibc of the root filesystem it was built against, or newer: build against
Debian 11 to run on Debian 11, Ubuntu 20.04 and everything newer.

## Installing and using

Inside the distribution, as root:

```sh
unzip radv-xclipse-glibc-*.zip
sh radv-xclipse-glibc-*/install.sh
```

This installs `/usr/lib/xclipse/libvulkan_radeon.so` and
`/usr/share/vulkan/icd.d/radeon_xclipse_icd.aarch64.json`. If the distribution's own Mesa
Vulkan drivers are installed as well, select this one:

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_xclipse_icd.aarch64.json
vulkaninfo --summary
```

X11, with Termux:X11 (start `termux-x11 :0 &` in Termux, then open the Termux:X11 app):

```sh
proot-distro login debian --shared-tmp
DISPLAY=:0 vkcube
```

Wayland: start a compositor inside the distribution (for example weston or labwc on the X
display), then run Vulkan apps with `WAYLAND_DISPLAY` set to its socket.

### Presentation

- X servers with DRI3, such as Termux:X11: the GPU renders into a buffer the X server reads
  directly.
- X servers without DRI3 (VNC and similar), and Wayland compositors without linux-dmabuf (most
  compositors inside PRoot): each frame is copied by the CPU and sent with PutImage or `wl_shm`.

The choice is made per window. `MESA_VK_WSI_DEBUG=sw` forces the CPU copy everywhere.

## Compatibility

| GPU | Codename | Phones and tablets, for example | Ray tracing |
|---|---|---|---|
| Xclipse 920 | VANGOGHLITE | Galaxy S22 series (Exynos 2200) | yes |
| Xclipse 530 | TITAN | Galaxy A55 (Exynos 1480) | no hardware |
| Xclipse 940 | MAGELLAN CARL | Galaxy S24 series (Exynos 2400) | yes |
| Xclipse 540 | ARIEL | Galaxy A56, Galaxy Tab S10 FE | no hardware |
| Xclipse 550 | TRITON | Galaxy A57 | no hardware |
| Xclipse 950 | GALILEO | Galaxy Z Flip7 | broken: exposed, but tracing rays hangs the GPU |
| Xclipse 960 | JUNO | Galaxy S26 series | broken: exposed, but it hangs the GPU and can crash the phone |

## Runtime switches

The switches of the Android build (see `main`) work here as environment variables: drop the
`debug.` prefix and use upper case, for example `RADV_XCLIPSE_DCC=0`. `RADV_XCLIPSE_LOG=1`
prints the `[BUILD]` line and the diagnostic markers to stderr; `2` adds the verbose trace.

## Licenses

The driver is MIT licensed (Mesa); see `LICENSE`, `THIRD_PARTY_NOTICES.md` and
`linux/NOTICE.txt`. License texts are in `LICENSES/`.
