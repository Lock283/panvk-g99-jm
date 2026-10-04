#!/bin/sh
# percase.sh <group> <caselist> : one deqp-vk process per case (large groups OOM on tree build)
C=/data/data/com.termux/files/home/cts; B=$C/build/external/vulkancts/modules/vulkan; R=$C/runs; G=$1; L=$2
. /data/data/com.termux/files/home/panvk-g57/phase4/x11/live.sh; live_start "$G" $(wc -l < $L)
mkdir -p $R/$G; cd $B
while read -r c; do
  live_status "$c" true
  out=$(PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_SHADER_CACHE_DISABLE=true timeout ${CTS_HANG_S:-60} ./deqp-vk --deqp-vk-library-path=$C/shim/libvkshim.so --deqp-case="$c" --deqp-log-filename=$R/$G/last.qpa --deqp-watchdog=disable 2>&1 | grep -aE '^\s+(Pass|Fail|NotSupported|QualityWarning|CompatibilityWarning|InternalError|Crash) \(' | head -1)
  e=$?; st=$(echo "$out" | awk '{print $1}'); [ -z "$st" ] && st=Crash
  printf '%s\t%s\t%s\n' "$c" "$st" "$(echo "$out" | sed -E 's/^\s+[A-Za-z]+ \((.*)\)$/\1/')" >> $R/$G/results.tsv
done < $L
cut -f2 $R/$G/results.tsv | sort | uniq -c > $R/$G/summary.txt; live_end
