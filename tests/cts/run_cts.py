#!/usr/bin/env python3
"""Resumable deqp-vk runner for the PanVK v9 bring-up.

Runs a case list through deqp-vk. If the process crashes or stops printing for
HANG_S seconds, the case that was running is recorded as Crash/Timeout and the
run resumes from the next case, so one bad case cannot hide the rest.

Usage: run_cts.py <group-name> <caselist.txt>
Writes  runs/<group>/results.tsv  (case<TAB>status<TAB>detail)
        runs/<group>/summary.txt
"""
import os, re, select, subprocess, sys, time, collections

C = "/data/data/com.termux/files/home/cts"
DEQP_DIR = C + "/build/external/vulkancts/modules/vulkan"
SHIM = C + "/shim/libvkshim.so"
HANG_S = int(os.environ.get("CTS_HANG_S", "60"))

group, listfile = sys.argv[1], sys.argv[2]
out = f"{C}/runs/{group}"
os.makedirs(out, exist_ok=True)
cases = [l.strip() for l in open(listfile) if l.strip()]
res_path = f"{out}/results.tsv"
done = {}
if os.path.exists(res_path):
    for l in open(res_path):
        p = l.rstrip("\n").split("\t")
        if len(p) >= 2:
            done[p[0]] = p[1]
resf = open(res_path, "a")
status_re = re.compile(r"^\s+(Pass|Fail|NotSupported|QualityWarning|CompatibilityWarning|"
                       r"InternalError|Crash|Timeout|ResourceError|Waiver)\s*\((.*)\)\s*$")
start_re = re.compile(r"^Test case '([^']+)'\.\.")
env = dict(os.environ, PAN_I_WANT_A_BROKEN_VULKAN_DRIVER="1", MESA_SHADER_CACHE_DISABLE="true")
import json
def status(cur, running=True, qpa=None):
    tmp = C + "/runs/live_status.json.tmp"
    json.dump({"group": group, "current": cur or "", "total": len(cases),
               "running": running, "qpa": qpa}, open(tmp, "w"))
    os.replace(tmp, C + "/runs/live_status.json")
run_no = 0

while True:
    todo = [c for c in cases if c not in done]
    if not todo:
        break
    before = len(done)
    run_no += 1
    lst = f"{out}/todo_{run_no}.txt"
    open(lst, "w").write("\n".join(todo) + "\n")
    cmd = [DEQP_DIR + "/deqp-vk", f"--deqp-vk-library-path={SHIM}",
           f"--deqp-caselist-file={lst}", f"--deqp-log-filename={out}/run_{run_no}.qpa",
           "--deqp-log-images=enable", "--deqp-log-shader-sources=disable",
           "--deqp-watchdog=disable"]
    p = subprocess.Popen(cmd, cwd=DEQP_DIR, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    cur, last = None, time.time()
    buf = ""
    while True:
        r, _, _ = select.select([p.stdout], [], [], 5)
        if r:
            line = p.stdout.readline()
            if line == "":
                break
            last = time.time()
            m = start_re.match(line)
            if m:
                cur = m.group(1)
                status(cur, True, f"{out}/run_{run_no}.qpa")
                continue
            m = status_re.match(line)
            if m and cur:
                done[cur] = m.group(1)
                resf.write(f"{cur}\t{m.group(1)}\t{m.group(2)[:200]}\n"); resf.flush()
                cur = None
        elif time.time() - last > HANG_S:
            p.kill(); p.wait()
            if cur:
                done[cur] = "Timeout"
                resf.write(f"{cur}\tTimeout\tno output for {HANG_S}s, process killed\n"); resf.flush()
            break
        if p.poll() is not None and not r:
            break
    rc = p.wait()
    if cur and cur not in done:
        done[cur] = "Crash"
        resf.write(f"{cur}\tCrash\tprocess exited rc={rc} during this case\n"); resf.flush()
    if len(done) == before:
        # The process ended without finishing a single case: blame the first
        # pending one so the loop always moves forward.
        done[todo[0]] = "Crash"
        resf.write(f"{todo[0]}\tCrash\tprocess rc={rc} before any result\n"); resf.flush()

resf.close()
status("selesai", False, None)
cnt = collections.Counter(done[c] for c in cases if c in done)
total = len(cases)
with open(f"{out}/summary.txt", "w") as s:
    s.write(f"group {group}: {total} cases, {run_no} process run(s)\n")
    for k in ["Pass", "Fail", "NotSupported", "QualityWarning", "CompatibilityWarning",
              "InternalError", "Crash", "Timeout", "ResourceError", "Waiver"]:
        if cnt.get(k):
            s.write(f"  {k:22s} {cnt[k]}\n")
    ran = total - cnt.get("NotSupported", 0)
    if ran:
        s.write(f"  pass rate of supported: {cnt.get('Pass',0)}/{ran} = {100.0*cnt.get('Pass',0)/ran:.1f}%\n")
print(open(f"{out}/summary.txt").read(), end="")
