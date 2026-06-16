#!/usr/bin/env python3
# Inject loose extracted files back into an MT Framework .arc (DMC4SE v7).
# Usage: inject.py <orig.arc> <loose_dir> <out.arc>
# For every TOC entry, if a loose file matching its internal name+ext exists under
# loose_dir, that entry's data is replaced (recompressed); otherwise the original
# compressed bytes are copied verbatim. TOC layout preserved (data starts at 0x8000,
# files contiguous). This is the reliable way to ship a texture mod -- the game reads
# skin textures out of the arc bundle in memory, never as loose files, so a whole
# modified arc is the only thing it will honor.
import struct, zlib, sys, os

KNOWN = {0x241F5DEB:"tex",0x2749C8A8:"mrl",0x10C460E6:"msg",0x232E228C:"rtex",
         0x046D7AAC:"sdl",0x2CE309AB:"gui",0x07F768AF:"gii",0x12191BA1:"efl"}
ENTRY = 80
DATA_START = 0x8000

def main(src, loose_dir, out):
    d = open(src, "rb").read()
    assert d[:4] == b"ARC\0", "not an ARC"
    ver, count = struct.unpack_from("<HH", d, 4)
    off = 8
    rows = []
    for i in range(count):
        e = d[off:off+ENTRY]; off += ENTRY
        name = e[:64].split(b"\0")[0].decode("latin1")
        exthash, comp, decompfield, foff = struct.unpack_from("<IIII", e, 64)
        rows.append([name, exthash, comp, decompfield, foff])

    assert 8 + count*ENTRY <= DATA_START, "TOC too big for 0x8000 data start"
    out_entries = []
    blob = bytearray()
    cur = DATA_START
    replaced = []
    for name, exthash, comp, decompfield, foff in rows:
        ext = KNOWN.get(exthash, "%08X" % exthash)
        # loose path: name uses '\' separators, append the resolved extension
        rel = name.replace("\\", "/") + "." + ext
        lpath = os.path.join(loose_dir, rel)
        if os.path.isfile(lpath):
            raw = open(lpath, "rb").read()
            cdata = zlib.compress(raw, 9)
            ncomp = len(cdata)
            ndecomp = (decompfield & 0xE0000000) | (len(raw) & 0x1FFFFFFF)  # keep flag bits
            out_entries.append((name, exthash, ncomp, ndecomp, cur))
            blob += cdata
            cur += ncomp
            replaced.append(name)
        else:
            cdata = d[foff:foff+comp]                 # copy original compressed bytes verbatim
            out_entries.append((name, exthash, comp, decompfield, cur))
            blob += cdata
            cur += comp

    # rebuild
    buf = bytearray()
    buf += b"ARC\0" + struct.pack("<HH", ver, count)
    for name, exthash, comp, decompfield, foff in out_entries:
        nb = name.encode("latin1")[:63].ljust(64, b"\0")
        buf += nb + struct.pack("<IIII", exthash, comp, decompfield, foff)
    buf += b"\0" * (DATA_START - len(buf))            # pad TOC region to data start
    buf += blob
    open(out, "wb").write(buf)
    print("injected %d/%d files; out=%s (%d bytes)" % (len(replaced), count, out, len(buf)))
    for r in replaced: print("  replaced:", r)
    if not replaced: print("  (no matching loose files found -- check names/extensions)")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
