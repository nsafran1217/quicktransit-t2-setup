# Building IRIX programs under QuickTransit

The SGUG gcc 9.2 (`/usr/sgug/bin/gcc` in the IRIX tree, from the
`gcc-9.2.0-4.sgug.mips` RPM) runs under QuickTransit. The compiler, the
assembler and the linker all work, and the n32 programs it builds run.
Tested 2026-10-26: `hello.c` with `-lm` builds in ~5 s, and a GLX/Xlib
program builds and runs against QuickTransit's `libGL.so`.

## What it needs

The QuickTransit IRIX environment (`IRIX-019-6.5.27m` RPM) is runtime-only.
It has no `/usr/include` system headers and no `crt1.o`/`crtn.o`, so gcc stops
with `stdio.h: No such file or directory`. Copy them from an IRIX system or
install media that has the development products:

| Needed | IRIX product |
|---|---|
| `/usr/include` | `irix_dev` (plus `c_dev` / `compiler_dev` headers) |
| `/usr/lib32/mips3/crt1.o`, `crtn.o`; `/usr/lib32/mips4/crt1.o`, `crtn.o` | `c_dev` / `compiler_dev` |
| `/usr/lib32/crt1.o`, `crtn.o` | symlinks to `mips4/` |

On an IRIX machine with those installed (IRIX 6.5.30 used here; 25 MB,
6.7 MB compressed):

    cd / && tar cf /usr/tmp/irixdev.tar usr/include \
        usr/lib32/mips3/crt1.o usr/lib32/mips3/crtn.o \
        usr/lib32/mips4/crt1.o usr/lib32/mips4/crtn.o \
        usr/lib32/crt1.o usr/lib32/crtn.o

On the ia64 machine, unpack it into `/irix`. Use `-k` so the headers already
in the RPM (X11, MediaViewer, ...) aren't replaced:

    cd /irix && bsdtar -xkf /path/to/irixdev.tar

The 6.5.30 headers work against the 6.5.27m runtime libraries.

## Running it

- **Call gcc by its IRIX path, from an IRIX shell.** gcc finds `cc1`
  relative to its own path. Run as `/irix/usr/sgug/bin/gcc` from Linux, it
  looks under `/irix/irix/...` and fails with `error trying to exec 'cc1'`.

      /irix/bin/sh -c 'cd /root/src && PATH=/usr/sgug/bin:/usr/bin:/bin gcc -O2 -o hello hello.c -lm'

- **Don't build or run in `/tmp`.** T2 mounts `/tmp` `noexec`, and
  QuickTransit maps IRIX executables with `PROT_EXEC`, so a program there
  fails with `ELFFileLoader: Failed to simple-mmap section!` (see
  [qt-internals.md](qt-internals.md#elf-loader)). `/root` or `/home` work.
- `timeout` isn't an IRIX command. Wrap the outer `/irix/bin/sh` with the
  Linux `timeout` instead.
- Shared libraries: `gcc -shared -fPIC -o libfoo.so foo.c` works. Use
  `FU_RLDN32_LIST` to preload one into a program (see
  [qt-internals.md](qt-internals.md#irix-dynamic-linker-preload-list)).
