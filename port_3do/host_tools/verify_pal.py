import struct, sys
def rd32(b,o): return struct.unpack(">I", b[o:o+4])[0]
def c555(u): return ((u>>10)&31,(u>>5)&31,u&31)
def is_grey(rgb,tol=1): return max(rgb)-min(rgb)<=tol

atl = open(sys.argv[1],"rb").read()
pal = open(sys.argv[2],"rb").read()
assert atl[:4]==b"ATL1"; assert pal[:4]==b"PPAL"
nf=rd32(atl,4); plut_total=rd32(atl,8)
atl_blob_off = 16 + nf*16*4
atl_blob = atl[atl_blob_off:atl_blob_off+plut_total]
pal_blob = pal[16:16+plut_total]
assert len(atl_blob)==len(pal_blob)==plut_total, (len(atl_blob),len(pal_blob),plut_total)

def sent(i):
    d0=i&3; d1=(i>>2)&3; d2=(i>>4)&3
    return (d0*8+3, d1*8+5, d2*8+1)
sentinels = {sent(i) for i in range(32,48)}

def near_sentinel(rgb,tol=2):
    # is this within tol (5-bit) of ANY primary sentinel?
    for s in sentinels:
        if abs(rgb[0]-s[0])+abs(rgb[1]-s[1])+abs(rgb[2]-s[2])<=tol: return True
    return False

n=plut_total//2
changed=0; changed_colors={}; atl_sent_hits=0; unchanged_grey=0; new_nongrey=set()
chg_src_sent=0; chg_src_near=0; chg_src_grey=0; chg_src_other=0
for k in range(n):
    a=struct.unpack(">H",atl_blob[k*2:k*2+2])[0]
    p=struct.unpack(">H",pal_blob[k*2:k*2+2])[0]
    ar=c555(a); pr=c555(p)
    if ar in sentinels: atl_sent_hits+=1
    if a!=p:
        changed+=1
        changed_colors[pr]=changed_colors.get(pr,0)+1
        if not is_grey(pr): new_nongrey.add(pr)
        # classify the SOURCE (atlas) color of each recolored entry
        if ar in sentinels: chg_src_sent+=1
        elif near_sentinel(ar): chg_src_near+=1
        elif is_grey(ar): chg_src_grey+=1
        else: chg_src_other+=1
    else:
        if is_grey(ar): unchanged_grey+=1

print("atlas %s vs %s" % (sys.argv[1].split('/')[-1], sys.argv[2].split('/')[-1]))
print("  total PLUT entries: %d" % n)
print("  atlas entries that ARE primary sentinels: %d" % atl_sent_hits)
print("  entries CHANGED by recolor: %d" % changed)
print("  distinct new colors: %d  (expect ~<=16 = one pilot ramp)" % len(changed_colors))
print("  new colors non-grey: %d" % len(new_nongrey))
print("  recolored-entry SOURCE classification:")
print("    exact sentinel: %d | near sentinel(<=2): %d | GREY(BAD): %d | other(BAD): %d"
      % (chg_src_sent, chg_src_near, chg_src_grey, chg_src_other))
top=sorted(changed_colors.items(), key=lambda x:-x[1])[:16]
print("  top new RGB555 colors (count): %s" % top)
