#!/bin/bash
# Build SMCSuperIO.kext (VM: zig clang + cctools ld64). Usage: build-kext.sh <outdir>
set -euo pipefail
D=$HOME/deps; SRC=$D/VirtualSMC/Sensors/SMCSuperIO; SDK=$D/MacKernelSDK; OUT=${1:-$HOME/kout}
CC="python3 -m ziglang clang"; LD=$HOME/tc/cct/bin/x86_64-apple-darwin-ld; STRIP=$HOME/tc/cct/bin/x86_64-apple-darwin-strip
VER=1.3.8
CFLAGS=(-target x86_64-apple-macos10.6 -mkernel -nostdlibinc -fno-builtin -fno-common -fno-exceptions -fno-rtti
  -fno-asynchronous-unwind-tables -fvisibility=hidden -fstack-protector -mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse
  -O3 -g0 -mllvm -disable-atexit-based-global-dtor-lowering
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DMODULE_VERSION=$VER -DPRODUCT_NAME=SMCSuperIO
  -I$SDK/Headers -I$D/Lilu/Lilu -I$D/VirtualSMC -I$SRC
  -Wall -Wno-unknown-warning-option -Wno-ossharedptr-misuse -Wno-vla -Wno-stdlibcxx-not-found -Wno-deprecated-declarations -Wno-unused-private-field -Wno-missing-braces -Wno-mismatched-tags)
mkdir -p $OUT/obj; rm -f $OUT/obj/*.o
for f in ITEDevice FintekDevice ECDeviceNUC WinbondDevice WinbondFamilyDevice ECDeviceGeneric NuvotonDevice ECDeviceDebug ECDevice SMCSuperIO SuperIODevice Devices; do
  $CC -x c++ -std=c++14 -fapple-kext "${CFLAGS[@]}" -c $SRC/$f.cpp -o $OUT/obj/$f.o
done
cat > $OUT/obj/info.c <<EOC
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t SMCSuperIO_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t SMCSuperIO_kern_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(ru.joedm.SMCSuperIO, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = SMCSuperIO_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = SMCSuperIO_kern_stop;
__private_extern__ int _kext_apple_cc = 6000;
EOC
$CC -x c -std=c11 "${CFLAGS[@]}" -c $OUT/obj/info.c -o $OUT/obj/info.o
$LD -arch x86_64 -kext -static -dead_strip -platform_version macos 10.6 10.6 -o $OUT/SMCSuperIO.unstripped $OUT/obj/*.o -L$SDK/Library/x86_64 -lkmod 2>&1 | grep -v "directory not found" || true
$STRIP -x -o $OUT/SMCSuperIO $OUT/SMCSuperIO.unstripped
echo BUILT $(wc -c < $OUT/SMCSuperIO)
