#!/bin/bash
# verify_wrapper.sh -- our Android (Winlator) driver loaded the way Winlator
# loads it: Winlator's libvulkan_wrapper.so (GameNative Unified Vulkan Wrapper
# 1.3.303, from the installed APK) as ICD, adrenotools hook path, driver from
# a flat folder, no PAN_I_WANT_A_BROKEN_VULKAN_DRIVER. Runs live on
# Termux:X11. Every positive case 3x, every fix has a negative control.
#
# usage: verify_wrapper.sh <drvdir> [logdir]
H=/data/data/com.termux/files/home
W=$H/wltest
F=$W/imagefs/usr/lib
DRV=${1:?drvdir}
L=${2:-$H/cts/runs/wrapper/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$L"
cd $H/mesa || exit 1
pass=0; fail=0; out=""

# common env for the wrapper path (used as: run name expect "${BASE[@]}" VAR=.. ./prog)
BASE=(env -u PAN_I_WANT_A_BROKEN_VULKAN_DRIVER DISPLAY=:0 LD_LIBRARY_PATH=$F
      ADRENOTOOLS_DRIVER_PATH=$DRV/ ADRENOTOOLS_HOOKS_PATH=$F/
      ADRENOTOOLS_DRIVER_NAME=libvulkan_panfrost.so MESA_SHADER_CACHE_DISABLE=true)

# name expect(PASS|FAIL) cmd...
run() {
  local name=$1 expect=$2; shift 2
  local log=$L/$name.log
  timeout 240 "$@" > "$log" 2>&1
  local rc=$?
  local got=FAIL
  grep -qE 'verdict=PASS|^RESULT ok' "$log" && [ $rc -eq 0 ] && got=PASS
  local tag=$( [ "$got" = "$expect" ] && echo OK || echo UNEXPECTED )
  [ "$tag" = OK ] && pass=$((pass+1)) || fail=$((fail+1))
  local why=$(grep -hE 'WSISUM|^RESULT|FAILED:|invalid dmabuf|DEVICES' "$log" | head -2 | tr '\n' ' ')
  printf '%-34s expect=%-4s got=%-4s rc=%-3s %-10s %s\n' "$name" "$expect" "$got" "$rc" "$tag" "$why" | tee -a "$L/summary.txt"
}

echo "driver: $DRV  sha256 $(sha256sum $DRV/libvulkan_panfrost.so | cut -c1-16)" | tee "$L/summary.txt"
echo "wrapper: $(cat /tmp/wlapk/wrapper/version.txt)" | tee -a "$L/summary.txt"

# 1. launcher query (adrenotools, like GPUInformation.*), wrapper device query
for i in 1 2 3; do
  run launcher_query_$i PASS env -u PAN_I_WANT_A_BROKEN_VULKAN_DRIVER MESA_SHADER_CACHE_DISABLE=true \
      ./adrenoload $W/at/libadrenotools.so $W/hooks/ $DRV/ libvulkan_panfrost.so
  run wrapper_device_$i PASS "${BASE[@]}" ./wrapload $F/libvulkan_wrapper.so 1103
done
# exact launcher code: adrenotools_open_libvulkan from the APK's libwinlator.so
# (libEGL preloaded, as zygote does for every app process)
for i in 1 2 3; do
  run launcher_libwinlator_$i PASS env -u PAN_I_WANT_A_BROKEN_VULKAN_DRIVER LD_LIBRARY_PATH=$W/apklib \
      ADRENOLOAD_PRELOAD=libEGL.so MESA_SHADER_CACHE_DISABLE=true \
      ./adrenoload $W/apklib/libwinlator.so $W/apklib/ $DRV/ libvulkan_panfrost.so
done
run ctl_launcher_require_optin FAIL env PANVK_V9_REQUIRE_OPTIN=1 MESA_SHADER_CACHE_DISABLE=true \
    ./adrenoload $W/at/libadrenotools.so $W/hooks/ $DRV/ libvulkan_panfrost.so

# 2. swapchain through the wrapper, screen read back from the X server
for pm in fifo mailbox immediate; do
  for i in 1 2 3; do
    run wsi_${pm}_$i PASS "${BASE[@]}" PANVK_ICD_SO=$F/libvulkan_wrapper.so CUBE_PRESENT=$pm \
        CUBE_FRAMES=300 CUBE_CHECK_EVERY=30 CUBE_DIM=600 ./wsi_cube
  done
done
for i in 1 2 3; do
  run wsi_resize_$i PASS "${BASE[@]}" PANVK_ICD_SO=$F/libvulkan_wrapper.so CUBE_PRESENT=fifo \
      CUBE_FRAMES=240 CUBE_CHECK_EVERY=20 CUBE_RESIZE_EVERY=40 CUBE_DIM=600 ./wsi_cube
done
# a mid-run screenshot as evidence (PNG)
( sleep 3; DISPLAY=:0 import -window root "$L/wsi_live.png" 2>/dev/null ) &
run wsi_fifo_shot PASS "${BASE[@]}" PANVK_ICD_SO=$F/libvulkan_wrapper.so CUBE_PRESENT=fifo \
    CUBE_FRAMES=300 CUBE_CHECK_EVERY=30 CUBE_DIM=600 ./wsi_cube
wait

# 3. negative controls
run ctl_wsi_lie FAIL "${BASE[@]}" PANVK_ICD_SO=$F/libvulkan_wrapper.so CUBE_LIE=1 \
    CUBE_FRAMES=60 CUBE_CHECK_EVERY=30 CUBE_DIM=600 ./wsi_cube
run ctl_gralloc_fd0 FAIL "${BASE[@]}" PANVK_ICD_SO=$F/libvulkan_wrapper.so PANVK_GRALLOC_FD0=1 \
    CUBE_FRAMES=60 CUBE_CHECK_EVERY=30 CUBE_DIM=600 ./wsi_cube
run ctl_ahb_no_linear FAIL "${BASE[@]}" PANVK_ICD_SO=$F/libvulkan_wrapper.so PANVK_AHB_NO_LINEAR_FIX=1 \
    CUBE_FRAMES=60 CUBE_CHECK_EVERY=30 CUBE_DIM=600 ./wsi_cube
run ctl_no_syncfd FAIL "${BASE[@]}" PANVK_ICD_SO=$F/libvulkan_wrapper.so PANVK_KBASE_NO_SYNCFD=1 \
    CUBE_FRAMES=60 CUBE_CHECK_EVERY=30 CUBE_DIM=600 ./wsi_cube
[ -d $W/drv ] && run ctl_alpha1_driver FAIL env -u PAN_I_WANT_A_BROKEN_VULKAN_DRIVER DISPLAY=:0 \
    LD_LIBRARY_PATH=$F ADRENOTOOLS_DRIVER_PATH=$W/drv/ ADRENOTOOLS_HOOKS_PATH=$F/ \
    ADRENOTOOLS_DRIVER_NAME=libvulkan_panfrost.so MESA_SHADER_CACHE_DISABLE=true \
    ./wrapload $F/libvulkan_wrapper.so 1103

echo "TOTAL as-expected=$pass unexpected=$fail  logs=$L" | tee -a "$L/summary.txt"
[ $fail -eq 0 ]
