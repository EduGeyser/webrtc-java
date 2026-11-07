# NVDEC headers

`include/ffnvcodec` holds three headers of FFmpeg's `nv-codec-headers` at tag `n13.0.19.1`, taken
unchanged:

https://github.com/FFmpeg/nv-codec-headers/tree/n13.0.19.1/include/ffnvcodec

- `dynlink_cuda.h`: the types and function signatures of the CUDA driver API that decoding needs.
- `dynlink_cuviddec.h` and `dynlink_nvcuvid.h`: the decoder and the bitstream parser of NVDEC
  (`libnvcuvid`).

The library loads the CUDA driver (`libcuda.so.1`) and `libnvcuvid.so.1` at run time, so the headers
are all the build needs, and a machine without an NVIDIA driver is unaffected. Version 13.0.19.1 is
the one the NVENC header (`../nvenc`) comes from. `webrtc-java-media` carries the same files for
FFmpeg's NVDEC hwaccels. The README of `nv-codec-headers` names NVIDIA driver 570.0 or newer, on
Linux and on Windows, as the minimum for this version:

https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.1/README

`LICENSE` gathers the notices at the head of the files. It is installed into the Linux platform jars
under `META-INF/licenses/nvdec`.
