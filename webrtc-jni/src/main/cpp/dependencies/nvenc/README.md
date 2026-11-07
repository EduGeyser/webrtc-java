# NVENC header

`include/nvEncodeAPI.h` is the NVIDIA Video Codec SDK header for NVENC, API
version 13.0, taken unchanged from FFmpeg's `nv-codec-headers` at tag
`n13.0.19.1`:

https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.1/include/ffnvcodec/nvEncodeAPI.h

The library loads NVENC and the CUDA driver at run time, so the header is all
the build needs. API 13.0 needs NVIDIA driver 570.0 or newer on Windows and on
Linux, the minimum the README of `nv-codec-headers` names for this tag:

https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.1/README

An older driver reports an older maximum API version, and the library then
does not use NVENC. Moving to a newer header raises that minimum.
