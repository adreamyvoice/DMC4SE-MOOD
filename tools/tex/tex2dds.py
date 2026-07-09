#!/usr/bin/env python3
"""MT Framework TEX (rev 0x9E, DMC4SE) <-> DDS converter, with PNG preview.
Usage:
  tex2dds.py <in.tex> [out_dir]        # tex -> dds (+ png if Pillow present)
Format: magic 'TEX\0'; h1 ver; h2 = mips(6)|width(13)<<6|height(13)<<19;
        h3 fmt byte (0x13=DXT1/BC1, 0x17=DXT5/BC3); then mips*u32 offset table;
        data is raw DXT block stream (all mips contiguous) starting at offset[0].
"""
import struct, sys, os

FOURCC = {0x13: b"DXT1", 0x17: b"DXT5", 0x15: b"DXT3"}

def parse_tex(d):
    assert d[:4] == b"TEX\0", "not a TEX: %r" % d[:4]
    h1, h2, h3 = struct.unpack_from("<III", d, 4)
    ver = h1 & 0xFFF
    mips = h2 & 0x3F
    width = (h2 >> 6) & 0x1FFF
    height = (h2 >> 19) & 0x1FFF
    fmt = (h3 >> 8) & 0xFF
    data_off = struct.unpack_from("<I", d, 16)[0]
    return dict(ver=ver, mips=mips, width=width, height=height, fmt=fmt,
                data_off=data_off, data=d[data_off:])

def dds_header(width, height, mips, fourcc, mip0_size):
    DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps,h,w,pixfmt,mip,linsize
    h  = b"DDS "
    h += struct.pack("<I", 124)              # dwSize
    h += struct.pack("<I", DDSD)             # dwFlags
    h += struct.pack("<I", height)
    h += struct.pack("<I", width)
    h += struct.pack("<I", mip0_size)        # linear size of mip0
    h += struct.pack("<I", 0)                # depth
    h += struct.pack("<I", max(1, mips))
    h += b"\0" * 44                          # reserved[11]
    # DDS_PIXELFORMAT
    h += struct.pack("<I", 32)               # size
    h += struct.pack("<I", 0x4)              # DDPF_FOURCC
    h += fourcc
    h += b"\0" * 20                          # masks unused
    h += struct.pack("<I", 0x1000 | 0x8 | 0x400000)  # TEXTURE|COMPLEX|MIPMAP
    h += b"\0" * 16                          # caps2..4 + reserved
    return h

def mip0_linear(width, height, fmt):
    blocks = max(1, (width + 3) // 4) * max(1, (height + 3) // 4)
    return blocks * (8 if fmt == 0x13 else 16)

def tex_to_dds(tex_path, out_dir):
    d = open(tex_path, "rb").read()
    t = parse_tex(d)
    fourcc = FOURCC.get(t["fmt"])
    if not fourcc:
        raise SystemExit("unsupported tex fmt 0x%02x" % t["fmt"])
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.splitext(os.path.basename(tex_path))[0]
    dds_path = os.path.join(out_dir, base + ".dds")
    hdr = dds_header(t["width"], t["height"], t["mips"], fourcc,
                     mip0_linear(t["width"], t["height"], t["fmt"]))
    open(dds_path, "wb").write(hdr + t["data"])
    print("  %s  %dx%d %s mips=%d -> %s" %
          (os.path.basename(tex_path), t["width"], t["height"],
           fourcc.decode(), t["mips"], os.path.basename(dds_path)))
    # optional PNG preview
    try:
        from PIL import Image
        png_path = os.path.join(out_dir, base + ".png")
        Image.open(dds_path).convert("RGBA").save(png_path)
        print("      png preview -> %s" % os.path.basename(png_path))
    except Exception as e:
        print("      (png skipped: %s)" % e)
    return dds_path

if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(sys.argv[1]))
    tex_to_dds(sys.argv[1], out)
