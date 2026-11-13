#!/usr/bin/env python3
"""
Extract IRIX inst(1) product images (<dist>/<product>.idb + .sw/.man/...)
into a directory tree, without IRIX inst.

  irix-dist-extract.py <dist-dir> <product> <out-root> [subsystem-regex]
      One product (install-extras.sh), e.g.
      irix-dist-extract.py .../dist demos /tmp/stage 'demos\\.sw\\.'

  irix-dist-extract.py --install <out-root> <dist-dir>...
      Every product in the dist directories, the way inst would install
      them all (install-irix6530.sh):
        * a full image of a subsystem (eoe.sw.base in eoe.idb, product name
          = subsystem prefix) replaces older full images of it: the
          Applications August 2006 desktop_eoe replaces June 1998's, DF 1.3's
          compiler_dev 7.4 replaces the 7.3 one;
        * overlay images (eoe.sw.base in eoe_6530m.idb) only list the files
          that changed, and go on top of the newest full image if they are
          newer than it;
        * images are laid down oldest first (by .idb date), so where two
          subsystems ship the same file the newer one wins;
        * absolute symlinks are made relative (/usr/lib/sendmail from
          usr/bsd/mailq becomes ../lib/sendmail), as in QuickTransit's own
          /irix tree: QuickTransit leaves symlink targets to the host kernel,
          which would resolve them against the host's /;
        * nothing is written through a symlink that leads out of <out-root>.

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
  * Older images (Applications June 1998, some Foundation products) have no
    off(): their entries are stored one after another in .idb order.
  * Entries marked delhist only remove an old entry from inst's history;
    they are skipped.

  * mach(...) limits an entry to some machines, e.g.
    mach(GFXBOARD!=VOYAGER && GFXBOARD!=ODYSSEY), mach(CPUBOARD=IP19
    CPUBOARD=IP21) or mach(GFXBOARD=MGRAS SUBGR=RACER) (see mach_ok).
    Several entries can install the same file for different machines.
    Entries are kept if they match MACHINE below: QuickTransit reports an
    IP30 (Octane) running IRIX64, taken here with MGRAS graphics (SUBGR
    RACER), as on a real Octane SI/SSI/MXI.

Ownership is not applied (the tree is pushed as root:root afterwards).
exitop()/postop() scripts are not run; they are printed for review.
"""
import os
import re
import shutil
import subprocess
import sys

MACHINE = {"CPUBOARD": "IP30", "CPUARCH": "R10000", "GFXBOARD": "MGRAS",
           "SUBGR": "RACER", "MODE": "64bit", "VIDEO": "none",
           "TARGOS": 1289662620}     # 6.5.30m (its sh: "IRIX 6.5:1289662620")
HEADER = 13
DIRMODES = {}           # directory -> mode, applied by set_dir_modes()


def mach_ok(expr):
    """Evaluate an idb mach() expression against MACHINE.  || and && are
    or/and; terms only separated by spaces are or'ed when they test the same
    key (CPUBOARD=IP19 CPUBOARD=IP21) and and'ed otherwise
    (GFXBOARD=MGRAS SUBGR=RACER).  A bare word is a CPUBOARD (IP27 MODE=64bit).
    TARGOS is compared as a number.  Keys not in MACHINE don't exclude
    anything."""
    expr = re.sub(r"\s*(>=|<=|!=|=|<|>)\s*", r"\1", expr.strip())
    def group(g):
        keys = {}
        for t in g.split():
            m = re.fullmatch(r"(\w+)(>=|<=|!=|=|<|>)(\S+)", t)
            k, op, v = m.groups() if m else ("CPUBOARD", "=", t)
            if k not in MACHINE:
                continue
            if isinstance(MACHINE[k], int):
                x, y = MACHINE[k], int(v)
                if not {"=": x == y, "!=": x != y, "<": x < y, "<=": x <= y,
                        ">": x > y, ">=": x >= y}[op]:
                    return False
                continue
            keys.setdefault(k, []).append((op, v))
        ok = True
        for k, terms in keys.items():
            eq = [v for op, v in terms if op == "="]
            ne = [v for op, v in terms if op == "!="]
            ok &= (not eq or MACHINE[k] in eq) and MACHINE[k] not in ne
        return ok
    return any(all(group(g) for g in alt.split("&&"))
               for alt in expr.split("||"))


def split_idb_line(line):
    """Bare words and attr(value) pairs of one .idb line.  Values are taken
    up to the matching ')', skipping parentheses inside '...' or "..."
    (exitop("(chroot $rbase ...)") and the like)."""
    words, attrs, i, n = [], {}, 0, len(line)
    while i < n:
        if line[i].isspace():
            i += 1
            continue
        j = i
        while j < n and not line[j].isspace() and line[j] != "(":
            j += 1
        if j < n and line[j] == "(":
            depth, quote, k = 0, None, j
            while k < n:
                c = line[k]
                if quote:
                    if c == quote:
                        quote = None
                elif c in "'\"":
                    quote = c
                elif c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            attrs[line[i:j]] = line[j + 1:k]
            i = k + 1
        else:
            words.append(line[i:j])
            i = j
    return words, attrs


class Entry:
    __slots__ = ("typ", "mode", "dst", "subsys", "attrs", "delhist",
                 "arch", "off")


def read_idb(dist, product):
    """All entries of <dist>/<product>.idb, in file order."""
    entries = []
    nextoff = {}            # archive -> offset of the next entry (no off())
    for line in open(os.path.join(dist, product + ".idb"), encoding="latin1"):
        f, attrs = split_idb_line(line)
        if len(f) < 7:
            continue
        e = Entry()
        e.typ, e.mode, e.dst = f[0], int(f[1], 8), f[4]
        e.attrs = attrs
        # After type/mode/owner/group/dst/src come attributes, flags
        # (nohist, needrqs, delhist, ...) and the subsystem, in any order:
        # the subsystem is the bare word with dots.
        e.subsys = next(w for w in f[6:] if "." in w)
        e.delhist = "delhist" in f[6:]
        e.arch = e.off = None
        if e.typ == "f":
            # overlay images: product desktop_eoe_6530m, subsystem
            # desktop_eoe.sw.x, archive desktop_eoe_6530m.sw
            e.arch = product + "." + e.subsys.split(".")[1]
            size = int(e.attrs["size"])
            csize = int(e.attrs.get("cmpsize", "0"))
            if "off" in e.attrs:
                e.off = int(e.attrs["off"])
            else:
                e.off = nextoff.get(e.arch, HEADER)
                nextoff[e.arch] = (e.off + 2 + len(e.dst.encode("latin1")) +
                                   (csize or size))
        entries.append(e)
    return entries


class Writer:
    def __init__(self, dist, out, relative_links=False, confine=False):
        self.dist, self.out = dist, out
        self.relative_links = relative_links
        self.root = os.path.realpath(out) if confine else None
        self.archives = {}
        self.nfile = self.nlink = self.ndir = 0
        self.problems = []

    def inside(self, path):
        if self.root is None:
            return True
        r = os.path.realpath(path)
        return r == self.root or r.startswith(self.root + "/")

    def close(self):
        for a in self.archives.values():
            a.close()

    def dir_to_link(self, e, dst):
        """A product made usr/adm a directory before eoe.sw.base's symlink
        usr/adm -> ../var/adm came: move what is in it to the link's target
        (where inst would have put it) and remove it, so the link can go in."""
        path = os.path.join(self.out, dst)
        t = e.attrs["symval"]
        t = t.lstrip("/") if t.startswith("/") else \
            os.path.join(os.path.dirname(dst), t)
        real = os.path.realpath(os.path.join(self.out, t))
        if not self.inside(real) or real == os.path.realpath(path):
            return False

        def merge(src, to):
            os.makedirs(to, exist_ok=True)
            for n in os.listdir(src):
                a, b = os.path.join(src, n), os.path.join(to, n)
                if os.path.isdir(a) and not os.path.islink(a) and \
                        os.path.isdir(b):
                    merge(a, b)
                elif not os.path.lexists(b):
                    os.rename(a, b)
            for n in os.listdir(src):       # left over: already at target
                a = os.path.join(src, n)
                if os.path.isdir(a) and not os.path.islink(a):
                    shutil.rmtree(a)
                else:
                    os.unlink(a)
            os.rmdir(src)
        merge(path, real)
        return True

    def write(self, e):
        dst = os.path.normpath(e.dst)       # usr/gfx/ucode/MG/../ge11_*.bin
        if dst.startswith(".."):
            self.problems.append(f"{e.dst}: leads out of the root, skipped")
            return
        path = os.path.join(self.out, dst)
        parent = os.path.dirname(path)
        if not self.inside(path if e.typ == "d" else parent):
            self.problems.append(f"{e.dst}: leads out of the root, skipped")
            return
        # Directories are made where symlinks lead (usr/mail -> ../var/mail
        # before var/mail exists); inside() checked that this is in the root.
        if e.typ == "d":
            if os.path.lexists(path) and not os.path.islink(path) and \
                    not os.path.isdir(path):
                os.unlink(path)
            real = os.path.realpath(path)
            os.makedirs(real, exist_ok=True)
            DIRMODES[real] = e.mode or 0o755    # set at the end, like tar
            self.ndir += 1
            return
        os.makedirs(os.path.realpath(parent), exist_ok=True)
        if os.path.isdir(path) and not os.path.islink(path):
            if e.typ == "l" and self.relative_links and self.dir_to_link(e, dst):
                pass
            else:
                self.problems.append(
                    f"{e.dst}: is a directory, {e.subsys} "
                    f"{'symlink' if e.typ == 'l' else 'file'} skipped")
                return
        if e.typ == "l":
            target = e.attrs["symval"]
            if self.relative_links and target.startswith("/"):
                target = os.path.relpath(target.lstrip("/") or ".",
                                         os.path.dirname(dst) or ".")
            if os.path.lexists(path):
                os.unlink(path)
            os.symlink(target, path)
            self.nlink += 1
        elif e.typ == "f":
            if e.arch not in self.archives:
                self.archives[e.arch] = open(os.path.join(self.dist, e.arch),
                                             "rb")
            a = self.archives[e.arch]
            size = int(e.attrs["size"])
            csize = int(e.attrs.get("cmpsize", "0"))
            a.seek(e.off)
            nlen = int.from_bytes(a.read(2), "big")
            name = a.read(nlen).decode("latin1")
            assert name == e.dst, (e.arch, e.off, name, e.dst)
            if csize:
                data = subprocess.run(["gzip", "-dc"], input=a.read(csize),
                                      capture_output=True).stdout
            else:
                data = a.read(size)
            assert len(data) == size, (e.dst, len(data), size)
            if os.path.lexists(path):
                os.unlink(path)
            with open(path, "wb") as o:
                o.write(data)
            os.chmod(path, e.mode)
            self.nfile += 1


def set_dir_modes():
    for path in sorted(DIRMODES, reverse=True):
        if os.path.isdir(path) and not os.path.islink(path):
            os.chmod(path, DIRMODES[path])


def wanted(e):
    return not e.delhist and ("mach" not in e.attrs or
                              mach_ok(e.attrs["mach"]))


def ops_of(entries):
    return {f"{e.subsys} {k}: {e.attrs[k]}" for e in entries
            for k in ("exitop", "postop", "preop", "removeop")
            if k in e.attrs}


def one_product(dist, product, out, regex=None):
    want = re.compile(regex) if regex else None
    w = Writer(dist, out)
    ops = set()
    for e in read_idb(dist, product):
        if (want and not want.search(e.subsys)) or not wanted(e):
            continue
        ops |= ops_of([e])
        w.write(e)
    w.close()
    set_dir_modes()
    print(f"{product}: {w.nfile} files, {w.nlink} symlinks, {w.ndir} dirs "
          f"-> {out}")
    if ops:
        print("install scripts NOT run:")
        for op in sorted(ops):
            print("  " + op)


def install(out, dists):
    os.makedirs(out, exist_ok=True)
    images = []                 # (mtime, argument order, dist, product)
    for i, d in enumerate(dists):
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".idb"):
                st = os.stat(os.path.join(d, fn))
                images.append((st.st_mtime, i, d, fn[:-4]))
    images.sort()
    entries, full = {}, {}
    for img in images:
        entries[img] = read_idb(img[2], img[3])
        for e in entries[img]:
            if e.subsys.split(".")[0] == img[3]:
                full[e.subsys] = img        # later (newer) images override

    def used(img, e):
        f = full.get(e.subsys)
        if e.subsys.split(".")[0] == img[3]:
            return f is img
        return f is None or img[:2] > f[:2]  # overlay newer than full image

    ops, problems = set(), []
    for img in images:
        mine = [e for e in entries[img] if used(img, e) and wanted(e)]
        subsystems = {e.subsys for e in entries[img]}
        used_ = {e.subsys for e in entries[img] if used(img, e)}
        where = os.path.relpath(img[2], os.path.commonpath(dists) or "/")
        if not mine:
            why = "superseded" if not used_ else "nothing for this machine"
            print(f"{img[3]} ({where}): {why}", flush=True)
            continue
        w = Writer(img[2], out, relative_links=True, confine=True)
        for e in mine:
            w.write(e)
        w.close()
        old = len(subsystems - used_)
        print(f"{img[3]} ({where}): {w.nfile} files, {w.nlink} symlinks, "
              f"{w.ndir} dirs" + (f"; {old} subsystems superseded"
                                  if old else ""), flush=True)
        ops |= ops_of(mine)
        problems += w.problems
    set_dir_modes()
    if problems:
        print(f"{len(problems)} entries not installed:")
        for p in problems:
            print("  " + p)
    if ops:
        print("install scripts NOT run:")
        for op in sorted(ops):
            print("  " + op)


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "--install":
        install(a[1], a[2:])
    elif len(a) in (3, 4) and not a[0].startswith("-"):
        one_product(*a)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
