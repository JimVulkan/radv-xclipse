# RADV for Samsung Xclipse

A port of Mesa's RADV Vulkan driver to Samsung Xclipse GPUs on Android, based on Mesa
27.0.0-devel. Every Xclipse GPU so far is supported: the 920, 530, 940, 540, 550, 950 and 960 (see
[Compatibility](#compatibility)).

One source tree builds three packages:

| Package | Script | For |
|---|---|---|
| Android | `build.sh`, `build.cmd` | emulators that load a custom Vulkan driver from a zip |
| Android with Wayland | `build-wayland.sh` | Bannerlator's Wayland mode |
| Linux (glibc) | `build-glibc.sh` | Termux PRoot distributions and DroidDeck |

## Requirements

Tools:

| Tool | Version | Notes |
|---|---|---|
| Android NDK | r29 tested | the API 34 aarch64 compiler and `llvm-strip` |
| Meson | 1.4.0 or newer | |
| Ninja | any recent | |
| Python | 3.10 or newer | with `mako` (0.8.0 or newer) and `packaging` |
| glslangValidator | 12.2 or newer | from the Vulkan SDK or your distribution |

```sh
pip install mako packaging
```

Dependencies are fetched by Meson on the first configure, so that step needs network access:

| Library | Version | Use |
|---|---|---|
| libdrm | 2.4.133 | linked statically, with `subprojects/packagefiles/libdrm-xclipse-mmr-cache.patch` |
| zlib | 1.3.1 | linked statically |
| Expat | 2.5.0 | required by the configure step, not linked into the driver |

No Android platform libraries are needed: the build uses Mesa's Android stubs.

## Building

```sh
./build.sh /path/to/android-ndk      # Linux, macOS
build.cmd C:\path\to\android-ndk     # Windows
```

The NDK path can also come from `ANDROID_NDK_HOME` or `ANDROID_NDK_ROOT`. The build goes to
`build-android/` (set `BUILD_DIR` to change it). The script builds the driver, strips it, and
writes a package to `dist/`: a zip with `meta.json`, `vulkan.radeon.so` and `NOTICE.txt`, for
emulators that load custom Vulkan drivers from a zip.

### Bannerlator (Wayland)

```sh
./build-wayland.sh /path/to/android-ndk
```

Builds the same driver with the Wayland window-system integration as well, for Bannerlator's
Wayland mode, and writes `dist/radv-xclipse-wayland-<version>-<commit>.zip`. Import that zip in
Bannerlator both as a GPU driver and as the Wayland game driver: it is one file for both. It
contains the driver as `libvulkan_freedreno_radv.so` (the only file name Bannerlator's Wayland
importer accepts) and `libwayland-client.so`, `libffi.so` and `libandroid-support.so` from the
Termux repository, which the script downloads.

This build needs a Linux x86-64 machine (WSL works) with `patchelf`, `curl`, `ar` and `tar` on top
of the tools above. The build goes to `build-android-wayland/` (`BUILD_DIR` changes it).

### Linux (glibc): Termux PRoot distributions and DroidDeck

```sh
./build-glibc.sh                        # inside the distribution on the phone
./build-glibc.sh aarch64-linux-gnu.ini  # cross build from an x86-64 Linux machine
```

Builds the driver as a regular glibc Linux driver with X11 and Wayland presentation, for Debian,
Ubuntu and other distributions under Termux and proot-distro, and for DroidDeck. It writes
`dist/radv-xclipse-glibc-<version>-<commit>.zip` with the driver, its Vulkan ICD file, `install.sh`,
`meta.json` (DroidDeck imports the zip as a Linux Vulkan driver), a short README and the notices.
The build goes to `build-glibc/` (`BUILD_DIR` changes it; `MESON_ARGS` adds configure options).

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

This build also downloads wayland-protocols 1.41 at the first configure.

#### Cross building

Build against an arm64 Debian or Ubuntu root filesystem that has the packages above, with a Meson
cross file:

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

The `wayland-scanner` that generates the protocol code has to be the version of the root
filesystem's libwayland: code from a newer scanner calls functions an older libwayland does not
have, and the driver then fails to load. If the build machine's own scanner is a different version,
build that version of `wayland-scanner` and name it:

```sh
WAYLAND_SCANNER=/path/to/wayland-scanner ./build-glibc.sh aarch64-linux-gnu.ini
```

The driver needs the glibc and the libwayland-client of the root filesystem it was built against,
or newer: build against Debian 11 (glibc 2.31, libwayland 1.18) to run on Debian 11, Ubuntu 20.04
and everything newer. The packaged `README.txt` covers installing and running it.

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

Other Xclipse models are not compatible for now. On the 950 and 960, avoid ray tracing settings in
games until a later release fixes them.

## Runtime switches

Each switch is an environment variable or, for apps that cannot set one, an Android property. The
glibc build reads only the environment variables, and `RADV_XCLIPSE_LOG` prints to stderr there.

| Property | Environment | Effect |
|---|---|---|
| `debug.radv_xclipse_log` | `RADV_XCLIPSE_LOG` | `1` logs diagnostic markers to logcat, `2` adds the verbose trace |
| `debug.radv_xclipse_perf` | `RADV_XCLIPSE_PERF` | `1` logs whether the GPU or the app's CPU limits frame time |
| `debug.radv_xclipse_no_bc_emu` | `RADV_XCLIPSE_NO_BC_EMU` | `1` hides BC4-BC7 (no GPU decode at upload) |
| `debug.radv_xclipse_uf` | `RADV_XCLIPSE_UF` | `1` restores the user fence on graphics submits |
| `debug.radv_xclipse_mtype` | `RADV_XCLIPSE_MTYPE` | VA map MTYPE: `0` default, `3` upstream |
| `debug.radv_xclipse_pal_heaps` | `RADV_XCLIPSE_PAL_HEAPS` | `0` restores the single memory heap |
| `debug.radv_xclipse_dcc` | `RADV_XCLIPSE_DCC` | `0` turns off render target compression (DCC) |
| `debug.radv_xclipse_dcc_small` | `RADV_XCLIPSE_DCC_SMALL` | `0` turns off DCC for render targets of 512x512 and smaller only |
| `debug.radv_xclipse_fillclear` | | `1` clears a whole render target that has no compression by filling its memory instead of drawing (experimental) |
| `debug.radv_xclipse_bc5_alias` | `RADV_XCLIPSE_BC5_ALIAS` | `0` stores BC5 textures separately from their converted (EAC) copy again: twice the memory and a slower upload, but copying such a texture back out as BC5 returns the original data |
| `debug.radv_xclipse_bc7_alias` | `RADV_XCLIPSE_BC7_ALIAS` | `0` stores BC7 textures separately from their converted (BC3) copy again: twice the memory, but copying such a texture back out as BC7 returns the original data |
| `debug.radv_xclipse_bc7srgb_rgba8` | `RADV_XCLIPSE_BC7SRGB_RGBA8` | `1` decodes sRGB BC7 textures to RGBA8 again (exact, but five times the memory) instead of sharing the BC7 texture's memory |
| `debug.radv_xclipse_bcsnorm_exact` | `RADV_XCLIPSE_BCSNORM_EXACT` | `1` decodes signed BC4 and BC5 textures exactly again (to R8/RG8) instead of converting them in place to EAC |
| `debug.radv_xclipse_bc4_bc3` | `RADV_XCLIPSE_BC4_BC3` | `1` stores unsigned BC4 textures as BC3 again (within 2 levels of the original instead of within 9, but three times the memory and slower to sample) instead of EAC |
| `debug.radv_xclipse_bc7_astc` | `RADV_XCLIPSE_BC7_ASTC` | `1` converts BC7 textures to ASTC instead of BC3: much closer to the original (39 dB against 32 on a test set from a real game), same memory and sampling speed, but loading a BC7 texture takes about twice as long on the GPU |
| `debug.mesa_gralloc_linear` | `MESA_GRALLOC_LINEAR` | `0` stops asking for linear (CPU-readable) layout on swapchain and shared buffers; on by default on Samsung, where the vendor's tiled layout reads back black to everyone else |
| `debug.radv_xclipse_pace` | `RADV_XCLIPSE_PACE` | Xclipse 920 only: `0` stops holding back the app's GPU work when it runs uncapped (vsync off), which otherwise keeps the Android UI responsive; `2` holds it back even for short jobs |
| `debug.radv_xclipse_nopf` | `RADV_XCLIPSE_NOPF` | `1` stops prefetching shaders and vertex descriptors into the GPU cache, `0` prefetches them; off by default on the Xclipse 940, 540, 550, 950 and 960 (prefetching froze the 940, and their vendor driver never prefetches) |
| `debug.radv_xclipse_bc6h_rgba16f` | `RADV_XCLIPSE_BC6H_RGBA16F` | `1` decodes BC6H (unsigned) textures to RGBA16F again instead of the half-size B10G11R11 format |
| `debug.radv_xclipse_pbb` | `RADV_XCLIPSE_PBB` | `0` bins draws with upstream's settings instead of the vendor driver's (used by default on the Xclipse 920; `1` also turns them on for the 530) |
| `debug.radv_xclipse_tfq` | `RADV_XCLIPSE_TFQ` | `0` turns off the vendor driver's texture filtering optimisations (on by default: the same samples as the vendor driver, and faster mipmapped and anisotropic texturing) for exact filtering |
| `debug.radv_xclipse_mesh` | `RADV_XCLIPSE_MESH` | `0` hides `VK_EXT_mesh_shader` |
| `debug.radv_xclipse_dbflush` | `RADV_XCLIPSE_DBFLUSH` | `0` flushes the depth caches the upstream way again instead of the vendor driver's way |
| `debug.radv_xclipse_llc` | `RADV_XCLIPSE_LLC` | Xclipse 960 only: `1` restates the vendor driver's GPU cache policy on every submit |
| `debug.mesa.radv.perftest` | `RADV_PERFTEST` | `nggc` turns NGG culling back on (off by default on Xclipse: slower with many draws) |
| `debug.radv_debug` | `RADV_DEBUG` | `hang` waits for the GPU after every submit and, when the GPU hangs, logs a report to logcat (tag `RADV_HANG`) and writes the shaders and command buffers involved to the app's `Android/data/<package>/files/radv_hang_<pid>_<n>/`; very slow, for bug reports only |
| | `MESA_VK_WSI_FORMAT_FILTER` | `nofp16` hides the 16-bit float (HDR) window formats, `bgra8` offers only B8G8R8A8; for compositors that show some formats wrongly |
| `debug.mesa_xclipse_prof` | `MESA_XCLIPSE_PROF` | `<delay>,<seconds>` profiles CPU and GPU time per frame and per render pass, then writes `mesa_prof_<pid>.txt` to `MESA_XCLIPSE_PROF_DIR` or the app's `Android/data/<package>/files`. `t,<seconds>` instead profiles `<seconds>` each time `debug.mesa_xclipse_prof_go` changes (`t,<seconds>,g` without CPU sampling), writes to `debug.mesa_xclipse_prof_dir` if set, and also prints the summary to logcat (tag `XPROF`) |

## Disclaimer

This project was developed with heavy use of AI tools. Changes are built and tested before they
are published, on a Galaxy S22 Ultra (SM-S908B, Xclipse 920) and, through Samsung Remote Test Lab,
on phones with the other Xclipse GPUs.

## License

The Xclipse changes are MIT licensed (`LICENSE`). Files from Mesa keep the license in their
headers, mostly MIT; `THIRD_PARTY_NOTICES.md` lists the files under other licenses and `LICENSES/`
has the full texts. The notices for everything compiled into the driver ship in the package as
`NOTICE.txt`.

This project is not affiliated with or endorsed by Samsung, AMD or the Mesa project. Samsung,
Exynos and Xclipse are trademarks of Samsung Electronics. AMD and Radeon are trademarks of Advanced
Micro Devices.
