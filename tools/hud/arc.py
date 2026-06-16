import struct, zlib, sys, os
def unpack(path, outdir=None, list_only=True):
    d=open(path,"rb").read()
    assert d[:4]==b"ARC\0", "not an ARC: %r"%d[:4]
    ver,count=struct.unpack_from("<HH",d,4)
    print("ARC ver=%d files=%d size=%d"%(ver,count,len(d)))
    off=8
    ENTRY=80  # v7 PC: 64 name + 4 hash + 4 comp + 4 decomp + 4 offset
    rows=[]
    for i in range(count):
        e=d[off:off+ENTRY]; off+=ENTRY
        name=e[:64].split(b"\0")[0].decode("latin1")
        exthash,comp,decomp,foff=struct.unpack_from("<IIII",e,64)
        rows.append((name,exthash,comp,decomp&0x1FFFFFFF,foff))
    # extension hash -> known names
    KNOWN={0x241F5DEB:"tex",0x2749C8A8:"mrl",0x10C460E6:"msg",0x232E228C:"rtex",
           0x046D7AAC:"sdl",0x2CE309AB:"gui",0x07F768AF:"gii",0x12191BA1:"efl"}
    from collections import Counter
    extc=Counter(r[1] for r in rows)
    print("ext-hash histogram:")
    for h,c in extc.most_common():
        print("  %08X x%-4d %s"%(h,c,KNOWN.get(h,"?")))
    print("\nfirst 30 entries:")
    for name,h,comp,decomp,foff in rows[:30]:
        print("  %-40s ext=%08X comp=%-8d dec=%-8d @%d"%(name,h,comp,decomp,foff))
    if not list_only and outdir:
        os.makedirs(outdir,exist_ok=True)
        for name,h,comp,decomp,foff in rows:
            raw=d[foff:foff+comp]
            try: data=zlib.decompress(raw)
            except Exception as ex: data=raw; print("  [raw] %s (%s)"%(name,ex))
            ext=KNOWN.get(h,"%08X"%h)
            safe=name.replace("\\","/")
            p=os.path.join(outdir,safe+"."+ext)
            os.makedirs(os.path.dirname(p),exist_ok=True)
            open(p,"wb").write(data)
        print("extracted -> %s"%outdir)
    return rows
if __name__=="__main__":
    unpack(sys.argv[1], sys.argv[2] if len(sys.argv)>2 else None, list_only=(len(sys.argv)<=2))
