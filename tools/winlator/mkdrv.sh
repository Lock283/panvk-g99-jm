#!/bin/sh
# mkdrv.sh <outdir> [debug]
# Lay out a Winlator/adrenotools driver folder from build-android: the driver
# with DT_RUNPATH blanked (stripped unless "debug"), the three bundled libs,
# and meta.json. Flat: Winlator's installer cannot unpack subfolders.
set -e
H=/data/data/com.termux/files/home
M=$H/panvk-g57/mesa
S=$H/panvk-g57/phase4/src
P=/data/data/com.termux/files/usr
OUT=$1
[ -n "$OUT" ] || { echo "usage: $0 outdir [debug]"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$M/build-android/src/panfrost/vulkan/libvulkan_panfrost.so" "$OUT/"
python3 "$S/blank_runpath.py" "$OUT/libvulkan_panfrost.so" > /dev/null
[ "$2" = debug ] || llvm-strip --strip-unneeded "$OUT/libvulkan_panfrost.so"
for l in libc++_shared.so libdrm.so libz.so.1; do
   cp -L "$P/lib/$l" "$OUT/$l"
   python3 "$S/blank_runpath.py" "$OUT/$l" > /dev/null
done
cp "$S/winlator_meta.json" "$OUT/meta.json"
ls -la "$OUT"
