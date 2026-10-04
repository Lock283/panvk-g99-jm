#!/usr/bin/env python3
"""mkstagepatches.py <c080268-unix-time> <outdir>

Backups in phase4/backup are copies of a file taken right before a change,
named <path-suffix>.before_<stage>. For every tracked file, the oldest backup
newer than the last repo commit is that file's state at the commit. The
backups of one stage give its "before" state; the "after" state is the file's
next backup or the live tree. Diffing those per stage gives patches that
stack exactly from the committed state to the live tree.

Files changed after the commit without any backup are reported (their change
cannot be attributed to a stage).
"""
import os, subprocess, sys, collections

H = '/data/data/com.termux/files/home'
MESA = H + '/panvk-g57/mesa'
BK = H + '/panvk-g57/phase4/backup'
T = int(sys.argv[1])
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)

def run(*a):
    return subprocess.run(a, capture_output=True, text=True, cwd=MESA).stdout

tracked = [p for p in run('git', 'diff', '--name-only').split() if p]
tracked += ['src/panfrost/lib/pan_v9_trace.h']      # new file after the commit
tracked += ['src/panfrost/vulkan/panvk_v9_nir_lower_minmax.h']  # untracked, from 0046
tracked = sorted(set(tracked))

# backup name -> path: the name is a "_"-joined suffix of the path
def match(bname, path):
    stem = bname.rsplit('.before_', 1)[0]
    parts = path.split('/')
    for k in range(1, len(parts) + 1):
        if '_'.join(parts[-k:]) == stem:
            return True
    return False

backs = []
for b in os.listdir(BK):
    if '.before_' not in b:
        continue
    m = int(os.stat(os.path.join(BK, b)).st_mtime)
    if m <= T:
        continue
    hits = [p for p in tracked if match(b, p)]
    if len(hits) == 1:
        backs.append((m, b.rsplit('.before_', 1)[1], hits[0], os.path.join(BK, b)))
    elif len(hits) > 1:
        print('AMBIGUOUS', b, hits)

per_file = collections.defaultdict(list)
for m, st, p, f in sorted(backs):
    per_file[p].append((m, st, f))

# Files that no repo patch touches were at the base commit when the repo was
# committed. If their oldest backup differs from base, the backup was taken
# after (part of) the edit; use the base content as the stage's "before".
BASE = '6598829019c0746aa8e473b4ae1c980cbfa6ea4b'
RP = H + '/panvk-g99-jm/patches'
in_repo = set()
for fn in os.listdir(RP):
    if fn.endswith('.patch'):
        for line in open(os.path.join(RP, fn), errors='replace'):
            if line.startswith('+++ b/'):
                in_repo.add(line[6:].strip())
basedir = os.path.join(OUT, 'base_copies')
for p in list(per_file):
    if p in in_repo:
        continue
    r = subprocess.run(['git', 'show', BASE + ':' + p], capture_output=True, cwd=MESA)
    if r.returncode != 0:
        continue
    bf = os.path.join(basedir, p)
    os.makedirs(os.path.dirname(bf), exist_ok=True)
    open(bf, 'wb').write(r.stdout)
    m0, st0, f0 = per_file[p][0]
    if open(f0, 'rb').read() != r.stdout:
        print('FIXUP %s: oldest backup (%s) != base, using base' % (p, st0))
        per_file[p][0] = (m0, st0, bf)

unbacked = []
for p in tracked:
    full = os.path.join(MESA, p)
    if p not in per_file and os.path.exists(full) and os.stat(full).st_mtime > T:
        unbacked.append(p)

# stage -> list of (path, before_file, after_file)
stages = collections.OrderedDict()
order = sorted({(min(m for m, s2, _ in per_file[p] if s2 == st), st)
                for p in per_file for _, st, _ in per_file[p]})
for _, st in order:
    stages[st] = []
for p, lst in per_file.items():
    for i, (m, st, f) in enumerate(lst):
        after = lst[i + 1][2] if i + 1 < len(lst) else os.path.join(MESA, p)
        stages[st].append((p, f, after))

newfiles = {'src/panfrost/lib/pan_v9_trace.h': 'trace'}

for n, (st, items) in enumerate(stages.items()):
    out = []
    for p, before, after in sorted(items):
        d = subprocess.run(['diff', '-u', '--label', 'a/' + p, '--label', 'b/' + p,
                            before, after], capture_output=True, text=True).stdout
        if d:
            out.append('diff --git a/%s b/%s\n' % (p, p) + d)
    for p, nst in newfiles.items():
        if nst == st and p not in per_file:
            d = subprocess.run(['diff', '-u', '--label', '/dev/null', '--label', 'b/' + p,
                                '/dev/null', os.path.join(MESA, p)], capture_output=True, text=True).stdout
            out.append('diff --git a/%s b/%s\nnew file mode 100644\n' % (p, p) + d)
    fn = os.path.join(OUT, '%02d-%s.diff' % (n, st))
    open(fn, 'w').write(''.join(out))
    print('%-14s files=%d lines=%d' % (st, len(out), sum(x.count('\n') for x in out)))

# state of every file at the commit (oldest post-commit backup, or live file)
base = os.path.join(OUT, 'at_commit')
for p in tracked:
    src = per_file[p][0][2] if p in per_file else os.path.join(MESA, p)
    if p in newfiles and p not in per_file:
        continue
    dst = os.path.join(base, p)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subprocess.run(['cp', src, dst])
print('UNBACKED (changed after commit, no backup):', unbacked)
gap = []
for p in tracked:
    if p in in_repo or p in per_file:
        continue
    r = subprocess.run(['git', 'show', BASE + ':' + p], capture_output=True, cwd=MESA)
    full = os.path.join(MESA, p)
    if r.returncode == 0 and os.path.exists(full) and open(full, 'rb').read() != r.stdout:
        gap.append(p)
print('DIFFERS FROM BASE, in no repo patch and no backup:', gap)
