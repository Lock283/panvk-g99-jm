#!/bin/bash
# run_dxvk.sh <dxvk-version> <drvdir> <exe> [extra env...]
# Real DXVK (x86_64 DLLs from the Winlator APK) on Hangover wine 11.16
# (arm64 wine, x86_64 code via the FEX arm64ec emulator), Vulkan through the
# Termux loader -> Winlator libvulkan_wrapper.so -> adrenotools -> our driver.
# Same Vulkan stack as a Winlator container. Live on Termux:X11 (DISPLAY=:0).
H=/data/data/com.termux/files/home
P=/data/data/com.termux/files/usr
R=$H/hangover/root$P
O=$R/opt/hangover-wine
W=$H/wltest
F=$W/imagefs/usr/lib
V=${1:?dxvk version}; DRV=${2:?drvdir}; EXE=${3:?exe}; shift 3
D=/tmp/dxvk/$V/system32
[ -f "$D/d3d11.dll" ] || { echo "no DXVK $V in $D"; exit 2; }
S32=$W/wpfx/drive_c/windows/system32
for dll in d3d11 dxgi d3d9 d3d10core; do [ -f "$D/$dll.dll" ] && cp -f "$D/$dll.dll" "$S32/$dll.dll"; done
# ICD json pointing at the Winlator wrapper (absolute path)
cat > $W/wrapper_icd.json <<EOF
{ "file_format_version": "1.0.0",
  "ICD": { "library_path": "$F/libvulkan_wrapper.so", "api_version": "1.3.303" } }
EOF
exec env -u PAN_I_WANT_A_BROKEN_VULKAN_DRIVER \
  WINEPREFIX=$W/wpfx WINEDEBUG=${WINEDEBUG:--all} DISPLAY=:0 \
  LD_LIBRARY_PATH=$R/lib:$F \
  VK_ICD_FILENAMES=$W/wrapper_icd.json \
  ADRENOTOOLS_DRIVER_PATH=$DRV/ ADRENOTOOLS_HOOKS_PATH=$F/ ADRENOTOOLS_DRIVER_NAME=libvulkan_panfrost.so \
  WINEDLLOVERRIDES="d3d11,dxgi,d3d9,d3d10core=n" \
  DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-info} DXVK_LOG_PATH=${DXVK_LOG_PATH:-$W} \
  "$@" $O/bin/wine "$EXE" ${EXE_ARGS}
