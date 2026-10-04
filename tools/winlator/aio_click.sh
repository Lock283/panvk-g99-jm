#!/bin/bash
# aio_click.sh <drvdir> <scene:tess|gs> <logdir> [ENV=..]
# Starts the AIO-Graphics-Test v2 shell (DXVK v2.3.1-stripped) live on X11,
# scrolls the menu to "DX11 scenes", clicks the scene tile, waits, then reports
# ALIVE (app still running) or DIED, saves a screenshot of the viewport, and
# stops wine. Tile positions are for the list view at 1080x1577.
H=/data/data/com.termux/files/home
P=/data/data/com.termux/files/usr
R=$H/hangover/root$P
S=$H/panvk-g57/phase4/src
W=$H/wltest
DRV=${1:?drv}; SC=${2:?scene}; L=${3:?logdir}; shift 3
case $SC in tess) Y=574;; gs) Y=643;; *) echo "scene?"; exit 2;; esac
mkdir -p $L; cd $W
env DXVK_LOG_PATH=$L WINEDEBUG=err+all timeout 300 $S/run_dxvk.sh v2.3.1-stripped $DRV $W/AIO-64.exe MESA_SHADER_CACHE_DISABLE=true "$@" > $L/out.log 2>&1 &
PID=$!
sleep 35
export DISPLAY=:0
xdotool mousemove 960 700; for i in 1 2 3; do xdotool click 5; sleep 0.3; done; sleep 1
xdotool mousemove 920 $Y click 1
sleep 15
if kill -0 $PID 2>/dev/null; then ST=ALIVE; else ST=DIED; fi
timeout 15 import -window root -crop 980x690+100+130 $L/view.png 2>/dev/null
env WINEPREFIX=$W/wpfx LD_LIBRARY_PATH=$R/lib $R/opt/hangover-wine/bin/wineserver -k 2>/dev/null
wait $PID 2>/dev/null
EXC=$(grep -ac 'Exception 0xc0000005' $L/out.log)
GRD=$(grep -ac 'EXPERIMENTAL guard\|failing the pipeline link' $L/out.log)
UNH=$(grep -ac 'Unhandled intrinsic' $L/out.log)
echo "AIOCLICK scene=$SC status=$ST segv=$EXC guard_msgs=$GRD unhandled=$UNH"
