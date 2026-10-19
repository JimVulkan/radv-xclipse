# Third-party notices

This repository is a subset of Mesa 27.0.0-devel with changes by JimVulkan. Every file keeps the
license stated in its header, and most are MIT (`LICENSE`); `docs/license.rst` is Mesa's own
overview. The files below are under other licenses. Full license texts are in `LICENSES/`.

Meson fetches these at configure time (not in this repository): libdrm 2.4.133 (MIT) and
zlib 1.3.1 (Zlib), both linked into the driver, wayland-protocols 1.41 (MIT), whose
generated code is compiled in, and Expat 2.5.0 (MIT), which is not. The notices for
everything compiled into the driver are in `linux/NOTICE.txt`, which the package includes.

## Apache-2.0

Text: `LICENSES/Apache-2.0.txt`

- `include/CL/cl.h`
- `include/CL/cl_d3d10.h`
- `include/CL/cl_d3d11.h`
- `include/CL/cl_dx9_media_sharing.h`
- `include/CL/cl_dx9_media_sharing_intel.h`
- `include/CL/cl_egl.h`
- `include/CL/cl_ext.h`
- `include/CL/cl_ext_intel.h`
- `include/CL/cl_function_types.h`
- `include/CL/cl_gl.h`
- `include/CL/cl_gl_ext.h`
- `include/CL/cl_half.h`
- `include/CL/cl_icd.h`
- `include/CL/cl_layer.h`
- `include/CL/cl_platform.h`
- `include/CL/cl_va_api_media_sharing_intel.h`
- `include/CL/cl_version.h`
- `include/CL/opencl.h`
- `include/GLES/glplatform.h`
- `include/GLES2/gl2platform.h`
- `include/GLES3/gl3platform.h`
- `include/vulkan/vk_android_native_buffer.h`

## Apache-2.0 OR MIT

Text: `LICENSES/Apache-2.0.txt`, `LICENSES/MIT.txt`

- `include/vk_video/vulkan_video_codec_av1std.h`
- `include/vk_video/vulkan_video_codec_av1std_decode.h`
- `include/vk_video/vulkan_video_codec_av1std_encode.h`
- `include/vk_video/vulkan_video_codec_h264std.h`
- `include/vk_video/vulkan_video_codec_h264std_decode.h`
- `include/vk_video/vulkan_video_codec_h264std_encode.h`
- `include/vk_video/vulkan_video_codec_h265std.h`
- `include/vk_video/vulkan_video_codec_h265std_decode.h`
- `include/vk_video/vulkan_video_codec_h265std_encode.h`
- `include/vk_video/vulkan_video_codec_vp9std.h`
- `include/vk_video/vulkan_video_codec_vp9std_decode.h`
- `include/vk_video/vulkan_video_codecs_common.h`
- `include/vulkan/vk_icd.h`
- `include/vulkan/vk_layer.h`
- `include/vulkan/vk_platform.h`
- `include/vulkan/vulkan.h`
- `include/vulkan/vulkan_android.h`
- `include/vulkan/vulkan_core.h`
- `include/vulkan/vulkan_wayland.h`
- `include/vulkan/vulkan_xcb.h`
- `include/vulkan/vulkan_xlib.h`
- `include/vulkan/vulkan_xlib_xrandr.h`
- `src/vulkan/registry/vk.xml`

## BSD-2-Clause

Text: `LICENSES/BSD-2-Clause.txt`

- `src/util/xxhash.h`

## BSD-3-Clause

Text: `LICENSES/BSD-3-Clause.txt`

- `src/compiler/glsl/float32.glsl`
- `src/compiler/glsl/float64.glsl`
- `src/gtest/include/gtest/gtest-assertion-result.h`
- `src/gtest/include/gtest/gtest-death-test.h`
- `src/gtest/include/gtest/gtest-matchers.h`
- `src/gtest/include/gtest/gtest-message.h`
- `src/gtest/include/gtest/gtest-param-test.h`
- `src/gtest/include/gtest/gtest-printers.h`
- `src/gtest/include/gtest/gtest-spi.h`
- `src/gtest/include/gtest/gtest-test-part.h`
- `src/gtest/include/gtest/gtest-typed-test.h`
- `src/gtest/include/gtest/gtest.h`
- `src/gtest/include/gtest/gtest_pred_impl.h`
- `src/gtest/include/gtest/gtest_prod.h`
- `src/gtest/include/gtest/internal/custom/gtest-port.h`
- `src/gtest/include/gtest/internal/custom/gtest-printers.h`
- `src/gtest/include/gtest/internal/custom/gtest.h`
- `src/gtest/include/gtest/internal/gtest-death-test-internal.h`
- `src/gtest/include/gtest/internal/gtest-filepath.h`
- `src/gtest/include/gtest/internal/gtest-internal.h`
- `src/gtest/include/gtest/internal/gtest-param-util.h`
- `src/gtest/include/gtest/internal/gtest-port-arch.h`
- `src/gtest/include/gtest/internal/gtest-port.h`
- `src/gtest/include/gtest/internal/gtest-string.h`
- `src/gtest/include/gtest/internal/gtest-type-util.h`
- `src/gtest/include/gtest/internal/gtest-type-util.h.pump`
- `src/gtest/src/gtest-all.cc`
- `src/gtest/src/gtest-assertion-result.cc`
- `src/gtest/src/gtest-death-test.cc`
- `src/gtest/src/gtest-filepath.cc`
- `src/gtest/src/gtest-internal-inl.h`
- `src/gtest/src/gtest-matchers.cc`
- `src/gtest/src/gtest-port.cc`
- `src/gtest/src/gtest-printers.cc`
- `src/gtest/src/gtest-test-part.cc`
- `src/gtest/src/gtest-typed-test.cc`
- `src/gtest/src/gtest.cc`
- `src/gtest/src/gtest_main.cc`
- `src/util/softfloat.c`
- `src/util/softfloat.h`

## BSL-1.0

Text: `LICENSES/BSL-1.0.txt`

- `src/c11/impl/threads_posix.c`
- `src/c11/impl/threads_win32.c`
- `src/c11/impl/time.c`
- `src/c11/threads.h`

## GPL-2.0 WITH Linux-syscall-note

Text: `LICENSES/GPL-2.0-only.txt`, `LICENSES/Linux-syscall-note.txt`

- `include/drm-uapi/dma-buf.h`

## Public domain

- `src/util/perf/gpuvis_trace_utils.h`
