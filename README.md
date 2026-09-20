# RADV for Samsung Xclipse

A port of Mesa's RADV Vulkan driver to Samsung Xclipse GPUs on Android, based on Mesa 26.2.3.
The Xclipse 920 (Exynos 2200) is supported. The Xclipse 530 (Exynos 1480) is highly experimental
and broken.

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

## Compatibility

Currently, the driver fully works only on the Xclipse 920. As for other models, they're not
compatible for now. There's partial compatibility for the Xclipse 530, but it's currently broken
and in current development, and if you really want to try it, set `TITAN_EXPERIMENTAL=1` in any
app that can set local environment variables (`adb shell setprop debug.radv_titan_experimental 1` 
for the apps that can't set environment variables).

## Runtime switches

Each switch is an environment variable or, for apps that cannot set one, an Android property.

| Property | Environment | Effect |
|---|---|---|
| `debug.radv_xclipse_log` | `RADV_XCLIPSE_LOG` | `1` logs diagnostic markers to logcat, `2` adds the verbose trace |
| `debug.radv_xclipse_perf` | `RADV_XCLIPSE_PERF` | `1` logs whether the GPU or the app's CPU limits frame time |
| `debug.radv_xclipse_no_bc_emu` | `RADV_XCLIPSE_NO_BC_EMU` | `1` hides BC4-BC7 (no GPU decode at upload) |
| `debug.radv_xclipse_uf` | `RADV_XCLIPSE_UF` | `1` restores the user fence on graphics submits |
| `debug.radv_xclipse_mtype` | `RADV_XCLIPSE_MTYPE` | VA map MTYPE: `0` default, `3` upstream |
| `debug.radv_xclipse_pal_heaps` | `RADV_XCLIPSE_PAL_HEAPS` | `0` restores the single memory heap |

## Disclaimer

This project was developed with heavy use of AI tools. Every change is built and tested on a
Galaxy S22 Ultra (SM-S908B, Xclipse 920) before it is published.

## License

The Xclipse changes are MIT licensed (`LICENSE`). Files from Mesa keep the license in their
headers, mostly MIT; `THIRD_PARTY_NOTICES.md` lists the files under other licenses and `LICENSES/`
has the full texts. The notices for everything compiled into the driver ship in the package as
`NOTICE.txt`.

This project is not affiliated with or endorsed by Samsung, AMD or the Mesa project. Samsung,
Exynos and Xclipse are trademarks of Samsung Electronics. AMD and Radeon are trademarks of Advanced
Micro Devices.
