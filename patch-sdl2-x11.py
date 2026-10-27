#!/usr/bin/env python3
"""
Make an IRIX SDL2 program (or libSDL2) use QuickTransit's hooked X11.

QuickTransit preloads /usr/QuickTransit/lib32/libX11_hook.so into IRIX
programs.  Its XOpenDisplay registers the Display with QuickTransit's GL
bridge.  SDL2 built with dynamic X11 loading dlopen()s libX11 itself and
dlsym()s XOpenDisplay from that handle, which gets the real, unhooked
XOpenDisplay.  The first GL call on that display then exits the program with

    libRCX11.so:Subject display without map entry encountered

This script rewrites the name SDL dlopen()s ("libX11.so.1" or
"/usr/lib32/libX11.so.1", in .rodata) to "libX11_hook.so".  rld matches that
soname to the hook that is already loaded and initialised.  Symbols the hook
doesn't export fall through to SDL's next library, libXext, which pulls in
libX11.  A symlink to the hook under another name doesn't work: it loads a
second, uninitialised copy.

Works on statically linked SDL2 programs (sgug pt2-clone, ft2-clone) and on
libSDL2-2.0.so itself (sgug-flx).  The NEEDED entry in .dynstr, if the
program links libX11 directly, is left alone.  See docs/sdl2.md.

Usage (as root, on the QuickTransit machine):

    patch-sdl2-x11.py <file>...

The original is kept as <file>.orig (once; a re-run doesn't overwrite it) and
the patched file replaces <file> by rename, so running copies are unaffected.
Already patched files are reported and skipped.
"""
import hashlib
import os
import shutil
import struct
import sys

HOOK = b"libX11_hook.so\0"
NAMES = (b"/usr/lib32/libX11.so.1\0", b"libX11.so.1\0")
SDL_MARK = b"SDL_X11_LoadSymbols"


def rodata_ranges(data):
    """(start, end) file ranges of .rodata* in a 32-bit big-endian ELF."""
    if data[:6] != b"\x7fELF\x01\x02":
        raise ValueError("not a 32-bit big-endian (IRIX n32/o32) ELF file")
    shoff, = struct.unpack_from(">I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x2E)
    def sh(i):
        return struct.unpack_from(">IIIIII", data, shoff + i * shentsize)
    _, _, _, _, stroff, _ = sh(shstrndx)
    out = []
    for i in range(shnum):
        name, typ, _, _, off, size = sh(i)
        n = data[stroff + name:data.index(b"\0", stroff + name)]
        if n.startswith(b".rodata") and typ != 8:      # 8 = SHT_NOBITS
            out.append((off, off + size))
    return out


def find_all(data, needle, ranges):
    hits = []
    for lo, hi in ranges:
        i = data.find(needle, lo, hi)
        while i >= 0:
            hits.append(i)
            i = data.find(needle, i + 1, hi)
    return hits


def patch(path):
    data = bytearray(open(path, "rb").read())
    if SDL_MARK not in data:
        raise ValueError("no SDL2 X11 code (SDL_X11_LoadSymbols) in it")
    ranges = rodata_ranges(data)
    hits = []
    for name in NAMES:
        # skip "libX11.so.1" hits that are the tail of the path form
        hits += [(i, name) for i in find_all(data, name, ranges)
                 if not any(j < i < j + len(NAMES[0]) for j, _ in hits)]
    if not hits:
        if find_all(data, HOOK, ranges):
            return "already patched"
        raise ValueError("no libX11.so.1 string in .rodata")
    if len(hits) > 1:
        raise ValueError("more than one libX11 name in .rodata at "
                         + ", ".join(hex(i) for i, _ in hits))
    off, name = hits[0]
    # the hook name is longer than "libX11.so.1": the bytes after it must
    # be the string's NUL padding (SDL's name table uses 16-byte slots)
    if len(HOOK) > len(name) and \
            any(data[off + len(name):off + len(HOOK)]):
        raise ValueError(f"no room for {HOOK[:-1].decode()} at {off:#x}")
    old = hashlib.md5(data).hexdigest()
    new = HOOK.ljust(len(name), b"\0")
    data[off:off + len(new)] = new

    if not os.path.exists(path + ".orig"):
        shutil.copy2(path, path + ".orig")
    tmp = path + ".new"
    with open(tmp, "wb") as f:
        f.write(data)
    shutil.copystat(path, tmp)
    os.rename(tmp, path)
    return (f"{name[:-1].decode()} at {off:#x} -> {HOOK[:-1].decode()}; "
            f"md5 {old} -> {hashlib.md5(data).hexdigest()}")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: patch-sdl2-x11.py <file>...")
    status = 0
    for path in sys.argv[1:]:
        try:
            print(f"{path}: {patch(path)}")
        except (OSError, ValueError) as e:
            print(f"{path}: {e}", file=sys.stderr)
            status = 1
    sys.exit(status)


if __name__ == "__main__":
    main()
