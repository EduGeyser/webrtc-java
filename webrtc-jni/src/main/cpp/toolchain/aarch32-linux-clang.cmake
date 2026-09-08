set(CMAKE_SYSTEM_NAME       Linux)
set(CMAKE_SYSTEM_PROCESSOR  arm)

# The Chromium clang of the WebRTC branch, installed into /opt/clang by the
# build workflow, or whichever clang is on the path.
find_program(CLANG_C clang HINTS /opt/clang/bin)
find_program(CLANG_CXX clang++ HINTS /opt/clang/bin)
find_program(LLVM_AR llvm-ar HINTS /opt/clang/bin)

if(NOT CLANG_C OR NOT CLANG_CXX OR NOT LLVM_AR)
    message(FATAL_ERROR "clang, clang++ and llvm-ar are required, in /opt/clang/bin or on the path")
endif()

set(CMAKE_C_COMPILER        ${CLANG_C})
set(CMAKE_CXX_COMPILER      ${CLANG_CXX})
set(CMAKE_AR                ${LLVM_AR})

set(TARGET_TRIPLE           arm-linux-gnueabihf)

set(CMAKE_C_FLAGS           "--target=${TARGET_TRIPLE} -march=armv7-a -mfloat-abi=hard -mfpu=neon")
set(CMAKE_CXX_FLAGS         "${CMAKE_C_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} -v -Wl,--verbose")

foreach(LINKER SHARED_LINKER)
    set(CMAKE_${LINKER}_FLAGS "-fuse-ld=lld -Wl,-s -v -Wl,--verbose")
endforeach()

# The Debian sysroot for armhf, installed by the build workflow into
# /opt/sysroot, or by dependencies/webrtc/linux/sysroot/install-sysroot.py
# into its own directory. Found here, applied after project() in the main
# CMakeLists.txt.
file(GLOB LINUX_SYSROOT
    "/opt/sysroot/debian*armhf*"
    "${CMAKE_CURRENT_LIST_DIR}/../dependencies/webrtc/linux/debian*armhf*"
)
if(NOT LINUX_SYSROOT)
    message(FATAL_ERROR "No debian armhf sysroot found in /opt/sysroot or dependencies/webrtc/linux, run dependencies/webrtc/linux/sysroot/install-sysroot.py --arch=armhf")
endif()
list(GET LINUX_SYSROOT 0 LINUX_SYSROOT)

set(DEFERRED_SYSROOT ${LINUX_SYSROOT})

set(TARGET_CPU              "arm")
