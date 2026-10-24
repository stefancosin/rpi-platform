# Cross-compiles rpi-platform (amd64 host -> aarch64 target) using Debian's
# multiarch packages instead of a separate sysroot: the RPi and this
# desktop run the identical Debian 12 (bookworm) release, so the :arm64
# apt packages (SQLiteCpp, cpp-httplib, PahoMqttCpp, OpenSSL) are ABI-
# compatible drop-ins for the ones already installed on the RPi.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Debian multiarch: headers are architecture-independent and shared under
# /usr/include, only compiled libraries/pkgconfig/cmake-config files live
# under the arch-specific /usr/lib/aarch64-linux-gnu
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
list(APPEND CMAKE_PREFIX_PATH /usr/lib/aarch64-linux-gnu/cmake)
list(APPEND CMAKE_LIBRARY_PATH /usr/lib/aarch64-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# pkg-config (used for cpp-httplib) must resolve against the arm64 .pc
# files only - otherwise it happily hands back the native amd64 flags
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
