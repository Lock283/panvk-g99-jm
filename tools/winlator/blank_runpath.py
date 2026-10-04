#!/usr/bin/env python3
"""blank_runpath.py <elf>: overwrite the DT_RUNPATH / DT_RPATH string of a
64-bit little-endian ELF with NUL bytes, in place (no patchelf on Termux).
The dynamic entry stays, but points at an empty string, so the loader
searches nothing extra."""
import struct, sys

p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
assert b[:4] == b'\x7fELF' and b[4] == 2 and b[5] == 1, 'need ELF64 LE'
phoff = struct.unpack_from('<Q', b, 0x20)[0]
phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
loads, dyn = [], None
for i in range(phnum):
    t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', b, phoff + i * phentsize)
    if t == 1:
        loads.append((va, off, fsz))
    elif t == 2:
        dyn = (off, fsz)

def v2o(va):
    for v, o, sz in loads:
        if v <= va < v + sz:
            return va - v + o
    raise ValueError(hex(va))

strtab, names = None, []
off, sz = dyn
for i in range(0, sz, 16):
    tag, val = struct.unpack_from('<qQ', b, off + i)
    if tag == 0:
        break
    if tag == 5:
        strtab = val
    elif tag in (15, 29):
        names.append((tag, val))
n = 0
for tag, val in names:
    o = v2o(strtab) + val
    e = b.index(0, o)
    print('blank %s %r' % ('RUNPATH' if tag == 29 else 'RPATH', bytes(b[o:e]).decode()))
    b[o:e] = b'\0' * (e - o)
    n += 1
open(p, 'wb').write(b)
print('done, %d entries' % n)
