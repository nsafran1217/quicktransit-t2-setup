# QuickTransit 1.2.1 on T2 Linux / ia64

QuickTransit is SGI's IRIX → Linux/ia64 binary translator. This kit installs
it on a current **T2 Linux ia64** system (an SGI Altix, an HP Itanium box...)
that has no `rpm`, and fixes what broke between 2005 and now. With it you can
run IRIX MIPS programs, including X/Motif and OpenGL GUI apps (4Dwm, Inventor,
Performer). GL is passed to the host's Mesa, so it is hardware accelerated
when the X server is (QuickTransit's "Prism" graphics path).

Reference system (everything below was tested there): SGI Altix with 2×
Itanium 2 (Madison), T2 26-svn, kernel 7.3, glibc 2.44, systemd, Xorg with a
Radeon R7 240 (Mesa 26.2 radeonsi).

## What you need

**On the ia64 machine (T2):** root, `bsdtar`, `python3`, `gcc` (all in a
normal T2 install), about 2 GB free, and an X server for the GUI apps.

**Input files**, all in one directory (subdirectories one level down are
searched too):

| File | Where from | md5 |
|---|---|---|
| `IRIX-019-6.5.27m.ia64.rpm` | QuickTransit 1.2 ISO (`QuickTransit_V1_2_Linux_IA-64.iso`) [https://archive.org/details/QuickTransitV12LinuxIA64](https://archive.org/details/QuickTransitV12LinuxIA64) | `ab4bacd843a174785404513f193163ae` |
| `libunwind-0.97-4.ia64.rpm` | same ISO | `b9ee298554b7c45c800f6fe48b206f0e` |
| `QuickTransit-1.2.1-004.ia64.rpm` | SGI update SG0010228 (`patch10228.tar.gz`, `RPMS/`) [https://archive.org/details/patch10228.tar](https://archive.org/details/patch10228.tar)| `c90883fd6952f4d24d89ac33791f5da7` |
| `libstdc++.so.5` (or `.gz`) | included in this kit (from SUSE Linux Enterprise Server 9 for IA-64, gcc 3.3); only needed in the inputs if missing from the kit | `4828a95f55665e3adbd74521d8906b47` (uncompressed) |

The ISO also carries `QuickTransit-1.2-001.ia64.rpm`. It isn't needed: 1.2.1
is a complete replacement. The FlexLM license is in this kit
(`target/etc/flexlm/license.dat`). It expired in 2005, which is handled below.

Getting the RPMs out of the ISO and the update with `bsdtar` (or loop-mount
the ISO and copy them):

    mkdir inputs && cd inputs
    bsdtar -xof /path/to/QuickTransit_V1_2_Linux_IA-64.iso
    bsdtar -xof /path/to/patch10228.tar.gz RPMS/QuickTransit-1.2.1-004.ia64.rpm

## Install

As root on the ia64 machine:

    ./install.sh /path/to/inputs          # ~3 min
    /irix/bin/sh -c 'uname -a'            # IRIX64 <host> 6.5 07141529 IP30

`install.sh` can be re-run safely. To remove everything:
`./uninstall.sh` (a copy is also left at `/usr/QuickTransit/uninstall.sh`).


### Optional: IRIX demos

`install-demos.sh` installs demos from IRIX 6.5 install media (tested with
6.5.30). Point it at a `dist` directory containing the products
`demos`, `performer_eoe`, `performer_demo` and `inventor_eoe`
(`<product>.idb` plus its `.sw` / `.data` / `.demo` files):

    ./install-demos.sh /path/to/dist      # ~3 min, ~820 MB into /irix

`irix-dist-extract.py` unpacks IRIX `inst` images without IRIX. Each `.idb`
line gives a file's offset in the `.sw`/`.data` archive; the data there is
raw or `compress`-LZW. The products'
install scripts aren't run; they only rebuild the web demo index and the
desktop filetype database.

## Using it

    modprobe amdgpu                       # if your GPU driver isn't loaded
    Xorg :0 vt7 &                         # or startx / your display manager
    export DISPLAY=:0
    /irix/usr/bin/X11/4Dwm &              # IRIX window manager
    /irix/usr/sbin/ivview scene.iv        # Open Inventor viewer
    /irix/usr/sbin/glxinfo                # should name your GPU
    /irix/bin/sh                          # IRIX shell

IRIX binaries can also be run directly; binfmt_misc hands them to
QuickTransit. Paths given to IRIX programs are IRIX paths: `/` is `/irix`,
and `/home`, `/root`, `/tmp` and `/mnt` are passed through. Logs go to
`/usr/QuickTransit/log/trlog.*`.

Run demos from their own directory, as their `RUN` scripts do:

    /irix/bin/sh -c "cd /usr/demos/General_Demos/ideas && ./ideas"
    /irix/bin/sh -c "cd /usr/share/Performer/data && /usr/sbin/perfly -m 0 -P 2 town.perfly"

| Status | Programs |
|---|---|
| Work | `sh`, `bash`, `uname`, `xterm`, `xclock`, `jot`, `4Dwm`, `touch`, `cp -p`, `tar` through pipes, `glxinfo`, `ivview`, `noof`, `bongo`, `glfalls`; demos `ideas`, `cycles`, `tex_cube` |
| Work with options | `perfly`: needs `-m 0 -P 2` (see known issues). In its window, `p` cycles the frame phase and `f`/`F` change the target rate. |
| Untested | bzflag, mirrors, MIDIblob, vroom, pointblank, mekton, Intersim/VisFly; imaging tools (`imgview`, `movieplayer`, ...) |
| Don't work | IRIS GL programs (buttonfly, atlantis, flight, newton, `cycol`, ...): QuickTransit has no IRIS GL. `gr_osview` (reads kernel memory). `fm` (see known issues). |

## Kit contents

| Path | What |
|---|---|
| `install.sh`, `uninstall.sh` | install / remove (run on the ia64 machine) |
| `libstdc++.so.5.gz` | gcc 3.3 C++ runtime QuickTransit links against (from SLES 9 ia64) |
| `patch-quicktransit.py` | binary fixes to the QuickTransit executable (problems 6, 7, 9) |
| `install-demos.sh`, `irix-dist-extract.py` | optional IRIX demos |
| `target/` | files copied as-is to `/` |


## What the install puts on the system

From the RPMs (unpacked with `bsdtar`):
- `/irix/`: the IRIX 6.5.27m environment (~840 MB)
- `/usr/QuickTransit/`: the translator
- `/usr/share/doc/QuickTransit-1.2.1/`: vendor docs, including the user guide
- The RPM's `/etc/init.d/QuickTransit` is skipped: it needs LSB init
  functions, and binfmt is registered through systemd instead.

Added or changed by `install.sh`:
- `/usr/QuickTransit/bin/QuickTransit`: binary-patched by
  `patch-quicktransit.py` (md5 `d6ece54e…` → `4ef6fa5a…`). The vendor
  binary is kept as `QuickTransit.orig`.
- `/usr/QuickTransit/compat-lib/`: `libstdc++.so.5`, `libunwind.so.1*` and
  `libunwind-ia64.so.1*` (from the libunwind RPM), plus the shim
  `libunwind-ia64-timefake.so`, built from `src/libtimefake.c` and
  `src/qtcompat.c`
- `/usr/lib/` symlinks into `compat-lib`: `libstdc++.so.5`,
  `libunwind.so.1`, `libunwind-ia64.so.1`, and `libunwind-ia64.so` (→ the
  shim). Also `/usr/lib/nptl/librt.so` → `/lib/librt.so.1`.
- `/etc/binfmt.d/quicktransit.conf`: `irix32`/`irix64` → `irixrun`, loaded
  by `systemd-binfmt` at boot
- `/etc/flexlm/license.dat`
- `/etc/X11/xorg.conf`: `FontPath` lines for `/usr/QuickTransit/fonts` and
  the IRIX fonts (`misc`, `75dpi`, `100dpi`, `Type1`). The original is kept as
  `xorg.conf.pre-quicktransit`.
- `/etc/services`: `portmapper rpcbind` aliases on the `sunrpc` lines. The
  original is kept as `services.pre-quicktransit`.
- `/usr/QuickTransit/fonts/`: Utopia-bold aliases
- `/irix/etc/fam.conf` (replaces the RPM's symlink to the host's
  `/etc/fam.conf`) and `/irix/usr/lib/X11/Xcms.txt` (SGI colour names)
- The vendor's `QTBindIRIX.sh` (the RPM's own `%post`) replaces the IRIX
  libGL/libmpi/libsma with links to QuickTransit's bridging versions.
- Modes: `/usr/QuickTransit/log` world-writable, `setuid_irixrun` setuid
  root (as in the RPM `%post`)

## Problems fixed, and how

1. **Missing old libraries.** QuickTransit needs `libstdc++.so.5` and the
   *unversioned* `libunwind-ia64.so`. ldconfig never caches unversioned
   names, so the libraries live in `compat-lib` and are symlinked into
   `/usr/lib`. (Debian's `libstdc++5_3.3.6-15_ia64.deb` from
   archive.debian.org also provides a working `libstdc++.so.5`.)

2. **Expired license (11-apr-2005).** `libtimefake.c` makes `time()` return
   a 2004 date. It isn't loaded with `LD_PRELOAD` (ignored for setuid
   launches through `setuid_irixrun`, and inherited by every child).
   Instead it is compiled *as* the `libunwind-ia64.so` that QuickTransit
   already loads, chained to the real `libunwind-ia64.so.1`. That library
   comes before libc in QuickTransit's lookup order, so this `time()` wins,
   but only inside QuickTransit. IRIX programs (`date`) still see the real
   time. The license has two lines; QuickTransit reports "0 Graphics
   Pipe(s)" on current systems, so the Altix line (`<= 256 CPU, 0 PIPE`) is
   the one that matches. GL acceleration doesn't depend on which line is used.

3. **4Dwm crashes** with `IA64Signalhandler::classify faulty instruction:
   Unknown instruction`. 4Dwm calls IRIX `stat(NULL)`, and QuickTransit then
   runs `strncat(cwd, NULL, n)`. Guest page 0 raises SIGBUS, and
   QuickTransit's handler emulates the faulting load, but it only decodes
   plain `ld1 rX=[rY]`. Current glibc's `strncat` uses post-increment loads.
   `qtcompat.c` adds a simple byte-loop `strncat` (built at `-O0`) to the shim.

4. **`Unable to dlopen librt! - /usr/lib/nptl/librt.so`.** That's a
   hard-coded RHEL3/SLES9 path; QuickTransit needs it for `clock_gettime`
   (IRIX `/dev/mmem`, used by 4Dwm). Fixed with a symlink to glibc's
   `librt.so.1`.

5. **SGI colour names and fonts.** The RPM added SGI colours to the
   server's `rgb.txt`, but modern Xorg has a built-in colour table. IRIX
   Xlib checks the client-side Xcms database first, so they now go in
   `/irix/usr/lib/X11/Xcms.txt`. The IRIX font directories go on the X font
   path (Speedo and CID are skipped because libXfont2 can't read them).

6. **Setting file times fails with "Invalid argument"** (`touch -t`,
   `touch -r`, `cp -p`, `tar x`, ...). QuickTransit makes the raw ia64
   `utimes` syscall itself, so no library can intercept it. Its argument
   builders store `{atime, mtime}` as two longs where the kernel expects
   `timeval[2]`, so `mtime` lands in `tv[0].tv_usec`. Kernels from 2004
   ignored that; current kernels return `EINVAL`. `patch-quicktransit.py`
   rewrites a few instruction bundles so `{atime,0},{mtime,0}` is written
   instead. The script checks the input md5 and every original instruction
   before changing anything.

7. **Fatal "Unsupported" syssgi calls** (hit by IRIX `fam`), also fixed in
   `patch-quicktransit.py`:
   - `sysconf(_SC_AUDIT)` and `sysconf(_SC_INF)` now return 0, like the
     other Trusted-IRIX names do.
   - Any other unsupported `syssgi` subcall is logged and returns 0,
     instead of killing the program.

8. **rpcbind** (needed by IRIX `fam`) won't open its TCP/UDP ports on T2:
   it looks up the service names `rpcbind` and `portmapper`, and T2's
   `/etc/services` has neither. The installer adds them as aliases.
   rpcbind is not started at boot.

9. **GL fixes:**
   - **Too many FBConfigs:** QuickTransit aborted ("returning too many
     FBConfigs (272), aborting!") because its tables hold 255 and current
     X servers offer more. `patch-quicktransit.py` caps the count at 255.
   - **Mesa JIT:** some legacy GL paths use Mesa's "draw" module, which
     wants an LLVM JIT that ia64 doesn't have. `qtcompat.c` sets
     `DRAW_USE_LLVM=0` for QuickTransit processes only.

10. **IRIX Type1 fonts crash the X server.** T2's ia64 glibc `memchr`
    reads past the end of its buffer (a bug in glibc's `memchr.S`), which
    makes FreeType crash on *any* PFA font, and it took Xorg down with it.
    The IRIX `Type1/*.pfa` entries are absolute symlinks that the Linux X
    server can't resolve, so they fail harmlessly and are left that way.
    Don't "fix" those links until T2's glibc has a fixed `memchr`.
    Performer's GUI wants `utopia-bold`, so `/usr/QuickTransit/fonts`
    aliases it to IRIX's bitmap `utopia-medium`.

## Known issues

- **Performer multi-process mode hangs** (black window). QuickTransit
  emulates IRIX `sproc` by forking a new host process. That happens after
  Mesa has started worker threads, so the child can inherit a Mesa lock
  held by a thread that doesn't exist in it, and its first GL call blocks
  forever. Workaround: `perfly -m 0` (single process).
- **`-P 2` for perfly**: Performer locks its frame rate to the video rate
  it measures at startup. That measurement is unreliable here (one run got
  1 Hz and drew one frame per second). `-P 2` (free run) doesn't use it.
- **Leftover QuickTransit helper processes**: QuickTransit forks a helper
  (for IRIX shared arenas, `/tmp/.QuickTransit-<id>.ctrl`) that outlives a
  killed program. Kill leftovers before rerunning; `ss -xlp | grep
  QuickTransit` lists them.
- **`fm` (file manager)** opens but shows no files. IRIX `fam` starts
  (`rpcbind; /irix/usr/etc/fam -f &`) but exits when a client connects:
  it calls `setgroups()`, which QuickTransit doesn't implement.
- **`tar cf file` / `tar xf file`** fail with `IRIX_ioctl: 't' subcall
  unsupported: 97`. Use pipes instead: `tar cf - ... > file`,
  `tar xf - < file`.
- **IRIS GL** (the pre-OpenGL API) isn't supported by QuickTransit.
- **No toolchest** binary in the IRIX environment RPM.


