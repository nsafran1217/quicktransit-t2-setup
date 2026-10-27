#!/usr/bin/env python3
"""
Extract an IRIX inst(1) product image (<dist>/<product>.idb + .sw/.man/...)
into a directory tree, without IRIX inst.

  irix-dist-extract.py <dist-dir> <product> <out-root> [subsystem-regex]

  e.g. irix-dist-extract.py .../dist demos /tmp/stage 'demos\\.sw\\.'

Format, as found on the 6.5 media:
  * <product>.idb: one line per entry:
        type mode owner group dstpath srcpath subsystem [attrs...]
    type d = directory, f = file, l = symlink (target in symval(...)).
    For files: off(N) size(S) cmpsize(C).  The archive is the product name
    plus the subsystem's second component: demos.sw.atlantis -> demos.sw.
    Overlay products keep the base subsystem names (desktop_eoe_6530m.idb
    lists desktop_eoe.sw.envm, stored in desktop_eoe_6530m.sw).
  * archive: 13-byte header ("im001V630P00\\0"), then at each off():
    2-byte big-endian name length, the name, then the data: cmpsize bytes of
    compress(1) LZW data if cmpsize > 0, else size raw bytes.

  * mach(...) limits an entry to some machines, e.g.
    mach(GFXBOARD!=VOYAGER && GFXBOARD!=ODYSSEY) or mach(CPUBOARD=IP19
    CPUBOARD=IP21) (spaces and || mean "or").  Several entries can install the
    same file for different machines.  Entries are kept if they match MACHINE
    below: QuickTransit reports an IP30 (Octane), taken here with MGRAS
    graphics, as on a real Octane SI/SSI/MXI.

Ownership is not applied (the tree is pushed as root:root afterwards).
exitop()/postop() scripts are not run; they are printed for review.
"""
import os
import re
import subprocess
import sys

ATTR = re.compile(r"(\w+)\(((?:[^()']|'[^']*')*)\)")
MACHINE = {"CPUBOARD": "IP30", "CPUARCH": "R10000", "GFXBOARD": "MGRAS"}


def mach_ok(expr):
    """Evaluate an idb mach() expression against MACHINE."""
    def term(t):
        k, op, v = re.fullmatch(r"(\w+)(!=|=)(\S+)", t).groups()
        return (MACHINE.get(k) == v) == (op == "=")
    expr = re.sub(r"\s*&&\s*", "&", expr.strip())     # a && b -> a&b
    alts = re.split(r"\|\||\s+", expr)
    return any(all(term(t) for t in a.split("&")) for a in alts)


def main():
    dist, product, out = sys.argv[1:4]
    want = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
    archives = {}
    nfile = nlink = ndir = 0
    ops = []
    for line in open(os.path.join(dist, product + ".idb"), encoding="latin1"):
        f = line.split()
        if len(f) < 7:
            continue
        typ, mode, dst, subsys = f[0], int(f[1], 8), f[4], f[6]
        if want and not want.search(subsys):
            continue
        attrs = dict(ATTR.findall(line))
        if "mach" in attrs and not mach_ok(attrs["mach"]):
            continue
        for k in ("exitop", "postop", "preop", "removeop"):
            if k in attrs:
                ops.append(f"{subsys} {k}: {attrs[k]}")
        path = os.path.join(out, dst)
        if typ == "d":
            os.makedirs(path, exist_ok=True)
            os.chmod(path, mode or 0o755)
            ndir += 1
        elif typ == "l":
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if os.path.lexists(path):
                os.unlink(path)
            os.symlink(attrs["symval"], path)
            nlink += 1
        elif typ == "f":
            # overlay images: product desktop_eoe_6530m, subsystem
            # desktop_eoe.sw.x, archive desktop_eoe_6530m.sw
            arch = product + "." + subsys.split(".")[1]
            if arch not in archives:
                archives[arch] = open(os.path.join(dist, arch), "rb")
            a = archives[arch]
            off, size = int(attrs["off"]), int(attrs["size"])
            csize = int(attrs.get("cmpsize", "0"))
            a.seek(off)
            nlen = int.from_bytes(a.read(2), "big")
            name = a.read(nlen).decode("latin1")
            assert name == dst, (name, dst)
            if csize:
                data = subprocess.run(["gzip", "-dc"], input=a.read(csize),
                                      capture_output=True).stdout
            else:
                data = a.read(size)
            assert len(data) == size, (dst, len(data), size)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            if os.path.lexists(path):
                os.unlink(path)
            with open(path, "wb") as o:
                o.write(data)
            os.chmod(path, mode)
            nfile += 1
    print(f"{product}: {nfile} files, {nlink} symlinks, {ndir} dirs -> {out}")
    if ops:
        print("install scripts NOT run:")
        for op in sorted(set(ops)):
            print("  " + op)


if __name__ == "__main__":
    main()
