import struct, zlib, sys
SRC=sys.argv[1]; OUT=sys.argv[2]
d=open(SRC,"rb").read(); assert d[:4]==b"ARC\0"
ver,count=struct.unpack_from("<HH",d,4)
ENTRY=80; off=8; entries=[]
for i in range(count):
    e=d[off:off+ENTRY]; off+=ENTRY
    nm=e[:64]; h,comp,decf,fo=struct.unpack_from("<IIII",e,64)
    entries.append({'nm':nm,'name':nm.split(b'\0')[0].decode('latin1'),'h':h,'comp':comp,'decf':decf,'fo':fo})
RED=(0.78,0.04,0.04)
def bloody_tex(dec):
    hdr,px=dec[:20],bytearray(dec[20:])
    for i in range(0,len(px),4):
        r,g,b=px[i],px[i+1],px[i+2]; L=(r*299+g*587+b*114)//1000
        px[i]=min(255,40+(L*86)//100); px[i+1]=min(255,(L*14)//100); px[i+2]=min(255,(L*10)//100)
    return hdr+bytes(px)
def redden_gui(dec):
    b=bytearray(dec); n=0; i=0
    while i<=len(b)-16:
        v=struct.unpack_from("<4f",b,i)
        if all(0.0<=x<=1.0 for x in v) and v[3]>=0.9 and (max(v[:3])-min(v[:3]))>0.3 and any(x>0.6 for x in v[:3]):
            if not (v[0]>0.6 and v[1]<0.3 and v[2]<0.3):
                struct.pack_into("<3f",b,i,*RED); n+=1; i+=16; continue
        i+=4
    return bytes(b),n
payloads=[]; did=[]
for en in entries:
    raw=d[en['fo']:en['fo']+en['comp']]
    is_frame_tex = en['h']==0x241F5DEB and "main01_NOMIP_eng" in en['name']
    is_pause_gui = en['h']==0x22948394 and "pause" in en['name']
    if is_frame_tex:
        dec=zlib.decompress(raw); a,b,c,doff=struct.unpack_from("<IIII",dec,4)
        if c==0x00010701 and (len(dec)-20)%4==0:           # uncompressed RGBA8 only
            dec=bloody_tex(dec); raw=zlib.compress(dec,9); en['comp']=len(raw); did.append("tex:"+en['name'])
    elif is_pause_gui:
        dec=zlib.decompress(raw); dec,nn=redden_gui(dec)
        if nn: raw=zlib.compress(dec,9); en['comp']=len(raw); did.append("gui:%s(%d)"%(en['name'],nn))
    payloads.append(raw)
FIRST=0x8000
buf=bytearray(b"ARC\0"+struct.pack("<HH",ver,count)); cur=FIRST; offs=[]
for en in entries: offs.append(cur); cur+=en['comp']
for en,o in zip(entries,offs): buf+=en['nm']+struct.pack("<IIII",en['h'],en['comp'],en['decf'],o)
buf+=b"\0"*(FIRST-len(buf))
for p in payloads: buf+=p
open(OUT,"wb").write(buf)
print("%s -> %s (%d files, modified: %s)"%(SRC.split('/')[-1],OUT.split('/')[-1],count,", ".join(did) or "NONE"))
