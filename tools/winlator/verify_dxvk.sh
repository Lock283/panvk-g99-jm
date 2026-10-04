#!/bin/bash
# verify_dxvk.sh <drvdir> <logdir> -- real DXVK through the Winlator stack.
# Each bundled DXVK version 3x with d3d11_tri.exe (pixel readback checks),
# plus negative controls. Verdict per run from D3D11SUM and the DXVK log.
H=/data/data/com.termux/files/home
S=$H/panvk-g57/phase4/src
W=$H/wltest
DRV=${1:?drvdir}; L=${2:?logdir}; mkdir -p "$L"
EXE=$W/d3d11_tri.exe
pass=0; fail=0
note() { echo "$*" | tee -a "$L/summary.txt"; }

# name expect version drvdir [env...]
run() {
  local name=$1 expect=$2 ver=$3 drv=$4; shift 4
  local d=$L/$name; rm -rf "$d"; mkdir -p "$d"
  ( cd $W && DXVK_LOG_PATH=$d timeout 300 $S/run_dxvk.sh "$ver" "$drv" "$EXE" D3D11TRI_FRAMES=180 "$@" ) > "$d/out.log" 2>&1
  local rc=$?
  local sum=$(grep -ah 'D3D11SUM' "$d/out.log" | tail -1)
  local why=$(grep -ahE 'FAILED:|DXVK(-Sarek)?: v|Driver version|feature level|Required Vulkan|Failed to create|unsupported' "$d/out.log" "$d"/*.log 2>/dev/null | sort -u | head -4 | tr '\n' ' ')
  local got=FAIL
  echo "$sum" | grep -q 'verdict=PASS' && [ $rc -eq 0 ] && got=PASS
  # identity: must be our Mesa driver, not the Mali blob fallback
  if [ $got = PASS ] && ! grep -aqhE 'Driver version +: 26\.|Driver : [^ ]+ 26\.' "$d"/*_d3d11.log 2>/dev/null; then got=FAIL; why="NOT-OUR-DRIVER $why"; fi
  local tag=OK; [ $got = $expect ] || tag=UNEXPECTED
  [ $tag = OK ] && pass=$((pass+1)) || fail=$((fail+1))
  note "$(printf '%-26s expect=%-4s got=%-4s rc=%-3s %-10s %s | %s' "$name" "$expect" "$got" "$rc" "$tag" "$sum" "$why" | cut -c1-330)"
}

if [ -n "$ONLY" ]; then for i in 1 2 3; do run v231_$i PASS v2.3.1-stripped $DRV; done; run ctl_v231_lie FAIL v2.3.1-stripped $DRV D3D11TRI_LIE=1; note "TOTAL as-expected=$pass unexpected=$fail"; exit $fail; fi
note "driver $DRV sha256 $(sha256sum $DRV/libvulkan_panfrost.so | cut -c1-16)"
for i in 1 2 3; do run sarek112_$i PASS 1.12.1-sarek $DRV; done
# PNG while sarek 1.12 renders (evidence)
( sleep 12; DISPLAY=:0 import -window root "$L/dxvk_live.png" 2>/dev/null ) &
run sarek112_shot PASS 1.12.1-sarek $DRV
wait
for i in 1 2 3; do run v231_$i PASS v2.3.1-stripped $DRV; done
for v in 1.10.3 1.11.1-sarek; do
  for i in 1 2 3; do run ${v}_$i FAIL $v $DRV; done
done
# controls
run ctl_lie FAIL 1.12.1-sarek $DRV D3D11TRI_LIE=1
[ -d $W/drv ] && run ctl_alpha1 FAIL 1.12.1-sarek $W/drv
note "TOTAL as-expected=$pass unexpected=$fail logs=$L"
[ $fail -eq 0 ]
