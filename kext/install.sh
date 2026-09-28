#!/bin/zsh
# Установка SMCSuperIO с управлением вентиляторами в EFI/OC/Kexts. После установки — перезагрузка.
set -euo pipefail
K=${0:A:h}
KEXT=$K/build/SMCSuperIO.kext

[ -f "$KEXT/Contents/MacOS/SMCSuperIO" ] || { echo "кекст не собран, собираю"; zsh "$K/build.sh"; }
plutil -lint "$KEXT/Contents/Info.plist" >/dev/null

EFI=/Volumes/EFI
mount | grep -q " on $EFI " || sudo diskutil mount disk0s1
[ -d "$EFI/EFI/OC/Kexts" ] || { echo "не найден $EFI/EFI/OC/Kexts"; exit 1; }

DST=$EFI/EFI/OC/Kexts/SMCSuperIO.kext
if [ -d "$DST" ]; then
  BAK=$EFI/EFI/OC/Kexts/SMCSuperIO.kext.bak-$(date +%Y%m%d-%H%M%S)
  sudo mv "$DST" "$BAK"
  echo "прежний кекст сохранён: ${BAK:t} (для отката переименовать обратно)"
fi
sudo ditto --norsrc --noextattr "$KEXT" "$DST"
sudo xattr -cr "$DST" 2>/dev/null || true

# SMCSuperIO должен быть включён и стоять после Lilu и VirtualSMC.
/usr/bin/python3 - "$EFI/EFI/OC/config.plist" <<'PY'
import plistlib, sys
cfg = plistlib.load(open(sys.argv[1], 'rb'))
kexts = cfg['Kernel']['Add']
names = [k.get('BundlePath') for k in kexts]
idx = lambda n: names.index(n) if n in names else -1
i_lilu, i_vsmc, i_sio = idx('Lilu.kext'), idx('VirtualSMC.kext'), idx('SMCSuperIO.kext')
ok = True
if i_sio < 0:
    print('!! SMCSuperIO.kext нет в Kernel->Add'); ok = False
else:
    e = kexts[i_sio]
    print('SMCSuperIO: Enabled=%s ExecutablePath=%s' % (e.get('Enabled'), e.get('ExecutablePath')))
    if not e.get('Enabled'):
        print('!! Enabled=false'); ok = False
    if e.get('ExecutablePath') != 'Contents/MacOS/SMCSuperIO':
        print('!! ExecutablePath должен быть Contents/MacOS/SMCSuperIO'); ok = False
    if not (0 <= i_lilu < i_sio and 0 <= i_vsmc < i_sio):
        print('!! порядок: Lilu и VirtualSMC должны идти раньше SMCSuperIO'); ok = False
print('config.plist: ок' if ok else 'config.plist: исправить вручную')
PY

echo
echo "Готово. Перезагрузись, затем: ./tools/gpufan doctor"
