# ffnvcodec headers

`include/ffnvcodec` holds the headers of FFmpeg's `nv-codec-headers` at tag `n13.0.19.1`, taken
unchanged:

https://github.com/FFmpeg/nv-codec-headers/tree/n13.0.19.1/include/ffnvcodec

- `dynlink_cuda.h`, `dynlink_cuviddec.h`, `dynlink_nvcuvid.h` and `dynlink_loader.h`: what FFmpeg
  needs to decode with NVDEC (the `h264_nvdec` and `vp9_nvdec` hwaccels). FFmpeg loads the CUDA
  driver (`libcuda.so.1`) and `libnvcuvid.so.1` with `dlopen` when a device is created, so the
  build needs the headers only and a machine without an NVIDIA driver loads the libraries all the
  same.
- `nvEncodeAPI.h`: the same file `webrtc-jni` vendors for NVENC. FFmpeg's configure checks for it
  too, so it is part of the set.

`ffnvcodec.pc.in` is the pkg-config file FFmpeg's configure looks for; CMake fills in the path and
puts the result where configure finds it. Version 13.0.19.1 is one FFmpeg n9.0 accepts
(`ffnvcodec >= 12.1.14.0`), and the one the NVENC header comes from. Its README names NVIDIA driver
570.0 or newer, on Linux and on Windows, as the minimum for this version:

https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.1/README

`LICENSE` gathers the notices at the head of the files. It is installed into the Linux platform
jars under `META-INF/licenses/ffnvcodec`.
