#!/bin/zsh
# Нативная сборка SMCSuperIO.kext на маке (Command Line Tools). Результат: kext/build/SMCSuperIO.kext
set -euo pipefail
setopt null_glob   # пустой каталог obj/ на первой сборке — не ошибка
K=${0:A:h}; B=$K/build; D=$B/deps
mkdir -p $D $B/obj
[ -d $D/VirtualSMC ] || git clone -q --depth 1 --branch 1.3.8 https://github.com/acidanthera/VirtualSMC.git $D/VirtualSMC
[ -d $D/MacKernelSDK ] || git clone -q --depth 1 https://github.com/acidanthera/MacKernelSDK.git $D/MacKernelSDK
[ -d $D/Lilu ] || git clone -q --depth 1 https://github.com/acidanthera/Lilu.git $D/Lilu
SRC=$D/VirtualSMC/Sensors/SMCSuperIO
cp $K/src/NuvotonDevice.hpp $K/src/NuvotonDevice.cpp $SRC/
VER=1.3.8
EXTRA=()
if xcrun clang -mllvm -disable-atexit-based-global-dtor-lowering -x c -c /dev/null -o /dev/null 2>/dev/null; then
  EXTRA=(-mllvm -disable-atexit-based-global-dtor-lowering)
fi
CFLAGS=(-arch x86_64 -mmacosx-version-min=10.6 -mkernel -nostdlibinc -fno-builtin -fno-common -fno-exceptions -fno-rtti
  -fno-asynchronous-unwind-tables -fvisibility=hidden -fstack-protector -mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse -O3 $EXTRA
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DMODULE_VERSION=$VER -DPRODUCT_NAME=SMCSuperIO
  -I$D/MacKernelSDK/Headers -I$D/Lilu/Lilu -I$D/VirtualSMC -I$SRC -Wno-deprecated-declarations)
rm -f $B/obj/*.o
for f in ITEDevice FintekDevice ECDeviceNUC WinbondDevice WinbondFamilyDevice ECDeviceGeneric NuvotonDevice ECDeviceDebug ECDevice SMCSuperIO SuperIODevice Devices; do
  xcrun clang++ -x c++ -std=c++14 -fapple-kext $CFLAGS -c $SRC/$f.cpp -o $B/obj/$f.o
done
cat > $B/obj/info.c <<EOC
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t SMCSuperIO_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t SMCSuperIO_kern_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(ru.joedm.SMCSuperIO, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = SMCSuperIO_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = SMCSuperIO_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOC
xcrun clang -x c -std=c11 $CFLAGS -c $B/obj/info.c -o $B/obj/info.o
LD=(xcrun ld); xcrun -f ld-classic >/dev/null 2>&1 && LD=(xcrun ld-classic)
$LD -arch x86_64 -kext -static -dead_strip -macos_version_min 10.6 -o $B/SMCSuperIO.unstripped $B/obj/*.o -L$D/MacKernelSDK/Library/x86_64 -lkmod
mkdir -p $B/SMCSuperIO.kext/Contents/MacOS
xcrun strip -x -o $B/SMCSuperIO.kext/Contents/MacOS/SMCSuperIO $B/SMCSuperIO.unstripped
cp $K/Info.plist $B/SMCSuperIO.kext/Contents/
if nm -u $B/SMCSuperIO.kext/Contents/MacOS/SMCSuperIO | grep -qE "cxa_atexit|withPhysicalAddressEjjj"; then
  echo "!! недопустимые импорты:"; nm -u $B/SMCSuperIO.kext/Contents/MacOS/SMCSuperIO | grep -E "cxa_atexit|withPhysicalAddressEjjj"; exit 1
fi
echo "OK: $B/SMCSuperIO.kext"
