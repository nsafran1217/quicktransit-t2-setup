#!/usr/bin/env python3
"""
Binary patch for QuickTransit 1.2.1 (/usr/QuickTransit/bin/QuickTransit,
md5 d6ece54e5fff0cf01da7047343f466f7): fix IRIX utime()/utimets() on
current Linux kernels.

The bug
-------
IRIX_utime and IRIX_utimets issue the ia64 `utimes` syscall (1036) directly
(their own `break 0x100000` stub, ia64SC_2arg), which takes
`struct timeval tv[2]`.  But the helpers that build the argument,

    mangleIRIXToLinuxUtimbuf(timeval*, guest_addr)     @ 0x40000000005924f0
    mangleIRIXToLinuxUtimetspec(timeval*, guest_addr)  @ 0x4000000000592610

store only two longs, {atime_sec, mtime_sec}, so the kernel sees
tv[0] = {atime, usec = mtime} and tv[1] = whatever was on the stack.
Kernels from 2004 copied tv_usec into the inode without checking it.
Current kernels reject tv_usec >= 1000000 with EINVAL, so `touch -t`,
`touch -r`, `cp -p`, `tar x` and the rest fail with "Invalid argument".

The fix
-------
Make both helpers write a proper timeval[2]: {atime, 0}, {mtime, 0}.
Nothing in either function moves.  In each one, three early bundles have
free nop.f / nop.i slots, which are used to zero the two tv_usec words, and
the post-increment of the first tv_sec store goes from 8 to 16:

  bundle   before                               after
  +0x10    MFI  mov r35=r1; nop.f; mov rX=r..;; MMI  mov r35=r1; adds r2=8,r32; (same);;
  +0x20    MI;;I ld8 ..; nop.i;; nop.i          MM;;I ld8 ..; st8 [r2]=r0,16;; nop.i
  +0x30    MFB  ld8 ..; nop.f; br.call          MMB  ld8 ..; st8 [r2]=r0; br.call
  store    st8 [rOut]=rSec,8                    st8 [rOut]=rSec,16

r2 is scratch and not otherwise live at those points.  r32 is the timeval*
argument.  The caller's buffer is sp+16 inside a 512-byte scratch area
(the path buffer starts at sp+528), so 32 bytes fit.

Second fix: IRIX sysconf(_SC_AUDIT) / sysconf(_SC_INF)
-------------------------------------------------------
syssgi(SGI_SYSCONF, name) dispatches names 0..31 through a table of
self-relative offsets at 0x400000000079f188 (loaded via gp+13304).
QuickTransit maps the other Trusted-IRIX names (_SC_ACL 25, _SC_MAC 28,
_SC_CAP 29, _SC_IP_SECOPTS 30) to a stub returning 0 (0x4000000000616b00),
but _SC_AUDIT 26 and _SC_INF 27 hit "Unsupported SGI_SYSCONF request" and
the process exits.  IRIX fam calls sysconf(_SC_AUDIT) at startup, so fam,
and with it the fm file manager, dies.  Entries 26 and 27 are pointed at the
same return-0 stub, which is what IRIX itself returns without SAT.

Third fix: unsupported syssgi() subcalls are no longer fatal
------------------------------------------------------------
IRIX_syssgi's default case (+0x3e0, 0x4000000000615da0) calls
reportError(severity=1, ...), which prints "Unsupported subcall: N" and
exits the process.  fam calls satwrite() -> syssgi(SGI_SATWRITE=75)
unconditionally; a SAT-less IRIX kernel just fails that call.  Severity 0
prints the same message and returns.  IRIX_syssgi then returns through its
common exit with the values set at entry (result 0, errno 0).  So unknown
syssgi subcalls now log and return 0 instead of killing the program.
(The SGI_SYSCONF / SGI_PATHCONF error paths join later at +0x410 with their
own severity and are unchanged.)

Fourth fix: clamp FBConfig lists to 255 instead of aborting
------------------------------------------------------------
RC_glXGetFBConfigs and RC_glXChooseFBConfigSGIX call the host function, then
abort ("GLbindStub: ... returning too many FBConfigs (%d), aborting!") if it
returned more than 255 configs, the size of QuickTransit's tables.  The
Radeon/Mesa X server offers 272, so `glxinfo -v`, Performer and anything else
listing FBConfigs died.  The first two bundles of each abort path are replaced
with:
      st4 [count_ptr]=r_max ; mov r_count=r_max ;;
      br <the compare bundle>
so the caller sees the first 255 configs and carries on.

Usage:  patch-quicktransit.py <in> <out>
"""
import hashlib
import sys

ORIG_MD5 = "d6ece54e5fff0cf01da7047343f466f7"
TEXT_VADDR = 0x4000000000000000          # first PT_LOAD, file offset 0

# ---- ia64 bundle helpers ---------------------------------------------------
def split(b):
    v = int.from_bytes(b, "little")
    return v & 0x1F, [(v >> 5) & (2**41 - 1), (v >> 46) & (2**41 - 1),
                      (v >> 87) & (2**41 - 1)]

def join(tmpl, slots):
    v = tmpl | slots[0] << 5 | slots[1] << 46 | slots[2] << 87
    return v.to_bytes(16, "little")

def st8_imm(r3, r2, imm9, qp=0):      # M5: st8 [r3]=r2,imm9
    imm = imm9 & 0x1FF
    return (5 << 37 | (imm >> 8) << 36 | 0x33 << 30 | ((imm >> 7) & 1) << 27 |
            r3 << 20 | r2 << 13 | (imm & 0x7F) << 6 | qp)

def st8(r3, r2, qp=0):                # M4: st8 [r3]=r2
    return 4 << 37 | 0x33 << 30 | r3 << 20 | r2 << 13 | qp

def adds(r1, imm14, r3, qp=0):        # A4: adds r1=imm14,r3
    imm = imm14 & 0x3FFF
    return (8 << 37 | (imm >> 13) << 36 | 2 << 34 | ((imm >> 7) & 0x3F) << 27 |
            r3 << 20 | (imm & 0x7F) << 13 | r1 << 6 | qp)

NOP_F = 0x8000000      # F-unit nop (opcode 0, x6=1)
NOP_I = 0x8000000      # I-unit nop.i 0 (opcode 0, x6=1)

T_MMI_S = 0x09         # M M I ;;
T_MM_S_I = 0x0A        # M M ;; I
T_MMB_S = 0x19         # M M B ;;

# ---- the patch --------------------------------------------------------------
def patch_func(data, base, out_reg, sec_reg, store_off):
    """base: function address; store_off: offset of bundle with
    `st8 [out_reg]=sec_reg,8` (slot 1)."""
    def get(a):
        o = a - TEXT_VADDR
        return split(bytes(data[o:o + 16]))

    def put(a, tmpl, slots):
        o = a - TEXT_VADDR
        data[o:o + 16] = join(tmpl, slots)

    # bundle +0x10: MFI;; -> MMI;;, slot1 nop.f -> adds r2=8,r32
    t, s = get(base + 0x10)
    assert t == 0x0D and s[1] == NOP_F, hex(base + 0x10)
    put(base + 0x10, T_MMI_S, [s[0], adds(2, 8, 32), s[2]])

    # bundle +0x20: MI;;I -> MM;;I, slot1 nop.i -> st8 [r2]=r0,16
    t, s = get(base + 0x20)
    assert t == 0x02 and s[1] == NOP_I and s[2] == NOP_I, hex(base + 0x20)
    put(base + 0x20, T_MM_S_I, [s[0], st8_imm(2, 0, 16), s[2]])

    # bundle +0x30: MFB;; -> MMB;;, slot1 nop.f -> st8 [r2]=r0
    t, s = get(base + 0x30)
    assert t == 0x1D and s[1] == NOP_F, hex(base + 0x30)
    put(base + 0x30, T_MMB_S, [s[0], st8(2, 0), s[2]])

    # first tv_sec store: post-increment 8 -> 16
    a = base + store_off
    t, s = get(a)
    assert s[1] == st8_imm(out_reg, sec_reg, 8), hex(a)
    s[1] = st8_imm(out_reg, sec_reg, 16)
    put(a, t, s)


SYSCONF_TABLE = 0x400000000079f188
SYSCONF_RET0 = 0x4000000000616b00
SYSCONF_FATAL = 0x4000000000616bc0

def patch_sysconf(data, name):
    import struct
    e = SYSCONF_TABLE + 8 * name
    o = e - TEXT_VADDR
    (rel,) = struct.unpack("<q", data[o:o + 8])
    assert e + rel == SYSCONF_FATAL, hex(e)
    data[o:o + 8] = struct.pack("<q", SYSCONF_RET0 - e)


def addl(r1, imm22, r3, qp=0):        # A5: addl r1=imm22,r3 (r3 in r0..r3)
    imm = imm22 & 0x3FFFFF
    return (9 << 37 | (imm >> 21) << 36 | ((imm >> 7) & 0x1FF) << 27 |
            ((imm >> 16) & 0x1F) << 22 | r3 << 20 | (imm & 0x7F) << 13 |
            r1 << 6 | qp)


def st4(r3, r2, qp=0):                # M4: st4 [r3]=r2
    return 4 << 37 | 0x32 << 30 | r3 << 20 | r2 << 13 | qp


def br_rel(src_bundle, target, qp=0):  # B1: br.cond.sptk.few target
    d = (target - src_bundle) >> 4
    imm = d & 0x1FFFFF
    return 4 << 37 | (imm >> 20) << 36 | (imm & 0xFFFFF) << 13 | qp


NOP_M = 0x8000000
T_MIB_S = 0x11         # M I B ;;

# (compare bundle, abort bundle, count_ptr, count, max, msg reg, msg gp-offset)
FBCONFIG_CLAMPS = [
    (0x4000000000241c10, 0x4000000000241e90, 34, 45, 9, 10, 27848),
    (0x4000000000243e90, 0x4000000000244120, 35, 47, 19, 20, 28016),
]


def patch_fbconfig_clamp(data, cmp_b, abort_b, rp, rc, rm, rmsg, gpoff):
    def get(a):
        o = a - TEXT_VADDR
        return split(bytes(data[o:o + 16]))

    def put(a, t, sl):
        o = a - TEXT_VADDR
        data[o:o + 16] = join(t, sl)
    # compare bundle: slot 1 is a conditional branch to the abort path
    t, sl = get(cmp_b)
    assert t == 0x13, hex(cmp_b)
    b = sl[1]
    disp = ((b >> 36) & 1) << 20 | (b >> 13) & 0xFFFFF
    assert b >> 37 == 4 and disp == ((abort_b - cmp_b) >> 4) & 0x1FFFFF, hex(cmp_b)
    # abort path loads the "returning too many FBConfigs" message
    t, sl = get(abort_b)
    assert sl[1] == addl(rmsg, gpoff, 1), hex(abort_b)
    put(abort_b, T_MMI_S, [st4(rp, rm), adds(rc, 0, rm), NOP_I])
    put(abort_b + 0x10, T_MIB_S, [NOP_M, NOP_I, br_rel(abort_b + 0x10, cmp_b)])


def main():
    src, dst = sys.argv[1:3]
    data = bytearray(open(src, "rb").read())
    md5 = hashlib.md5(data).hexdigest()
    if md5 != ORIG_MD5:
        sys.exit(f"{src}: md5 {md5}, expected unpatched {ORIG_MD5}")

    # self-check the encoders against instructions present in the binary
    _, s = split(bytes(data[0x592540:0x592550]))
    assert s[1] == st8_imm(32, 2, 8)                 # st8 [r32]=r2,8
    _, s = split(bytes(data[0x592530:0x592540]))
    assert s[2] == adds(3, 4, 33)                    # adds r3=4,r33
    _, s = split(bytes(data[0x592570:0x592580]))
    assert s[0] == st8(32, 2)                        # st8 [r32]=r2

    # mangleIRIXToLinuxUtimbuf: out=r32, sec in r2, store bundle +0x50
    patch_func(data, 0x40000000005924f0, 32, 2, 0x50)
    # mangleIRIXToLinuxUtimetspec: out=r34 (copy of r32), sec in r11, +0xa0
    patch_func(data, 0x4000000000592610, 34, 11, 0xA0)
    # SGI_SYSCONF: _SC_AUDIT (26), _SC_INF (27) -> return 0
    import struct
    for ok in (25, 28, 29, 30):                      # sanity: existing stubs
        e = SYSCONF_TABLE + 8 * ok
        o = e - TEXT_VADDR
        assert e + struct.unpack("<q", data[o:o + 8])[0] == SYSCONF_RET0
    patch_sysconf(data, 26)
    patch_sysconf(data, 27)
    # IRIX_syssgi default case: reportError severity 1 (fatal) -> 0 (warn)
    a = 0x4000000000615da0
    o = a - TEXT_VADDR
    t, sl = split(bytes(data[o:o + 16]))
    assert sl[0] == addl(55, 1, 0), hex(a)           # mov r55=1
    sl[0] = addl(55, 0, 0)                           # mov r55=0
    data[o:o + 16] = join(t, sl)

    # encoder self-checks against instructions in the binary
    _, sl = split(bytes(data[0x243fc0:0x243fd0]))
    assert sl[0] == st4(35, 33), "st4 encoding"          # st4 [r35]=r33
    _, sl = split(bytes(data[0x241e80:0x241e90]))
    assert sl[2] == br_rel(0x4000000000241e80, 0x4000000000241d10), "br encoding"
    for c in FBCONFIG_CLAMPS:
        patch_fbconfig_clamp(data, *c)

    open(dst, "wb").write(data)
    print(f"{dst}: md5 {hashlib.md5(data).hexdigest()}")


if __name__ == "__main__":
    main()
