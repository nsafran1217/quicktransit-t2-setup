# SDL2 programs (SGUG)

IRIX SDL2 programs from SGUG (`/usr/sgug`, `/usr/sgug-flx`) need two fixes
under QuickTransit:

1. **`patch-sdl2-x11.py` on the program, or on its libSDL2**, so SDL uses
   QuickTransit's hooked X11. Without it the program exits on its first GL
   call.
2. **`install-extras.sh`** (QuickTransit binary fix 7), for SDL2's OpenGL
   renderer. Without it the program exits on its first frame.

Sound needs nothing extra: SDL's Irix audio driver plays through the kit's
libaudio ([irix-audio.md](irix-audio.md)).

Tested:

| File | SDL2 | md5 before → after `patch-sdl2-x11.py` | Result |
|---|---|---|---|
| `/irix/usr/sgug/bin/pt2-clone` (ProTracker 2 clone 1.78) | static | `136c3516…` → `462f2a3f…` | runs, OpenGL renderer |
| `/irix/usr/sgug/bin/ft2-clone` (Fasttracker II clone 2.23) | static | `4460865e…` → `db9f5f09…` | runs, OpenGL renderer |
| `/irix/usr/sgug-flx/lib32/libSDL2-2.0.so.0.14.0` | shared, used by `secondreality` and other sgug-flx programs | `1a961ed5…` → `7903b05c…` | runs |

## 1. X11: "Subject display without map entry"

```
libRCX11.so:Subject display without map entry encountered
```

QuickTransit preloads `/usr/QuickTransit/lib32/libX11_hook.so` into every
IRIX program. The hook's `XOpenDisplay` registers each Display with
QuickTransit's GL bridge, and GL calls on a Display it doesn't know end the
program with the message above.

SGUG's SDL2 is built with dynamic X11 loading. It `dlopen()`s libX11 itself
and `dlsym()`s `XOpenDisplay` from that handle, which skips the hook. The
library it opens is named in a table in `.rodata`:

| Build | Names |
|---|---|
| sgug-flx `libSDL2-2.0.so` | `/usr/lib32/libX11.so.1`, `/usr/lib32/libXext.so`, ... |
| static SDL2 in sgug programs | `libX11.so.1`, `libXext.so`, `libXsgivc.so` (16-byte slots) |

`patch-sdl2-x11.py` renames the libX11 entry to `libX11_hook.so`. rld
matches that soname to the hook that is already loaded and set up. SDL then
finds the hooked `XOpenDisplay`. The functions the hook doesn't export are
found in SDL's next entry, `libXext.so`, which depends on libX11.

Things that don't work instead:
- A symlink to the hook under another name. rld loads a second copy, which
  QuickTransit never set up ("hooklib couldn't dlsym XOpenDisplay with
  handle: 0").
- `LD_LIBRARYN32_PATH`. The static programs carry an RPATH
  (`/usr/lib32:/usr/sgug/lib32`), which wins, and the shared library uses
  an absolute path.

### Running the script

On the QuickTransit machine, as root:

    ./patch-sdl2-x11.py /irix/usr/sgug/bin/pt2-clone /irix/usr/sgug/bin/ft2-clone

It:
- checks that the file is a 32-bit big-endian ELF containing SDL2's X11
  code (`SDL_X11_LoadSymbols`),
- looks for exactly one `libX11.so.1` or `/usr/lib32/libX11.so.1` in
  `.rodata`. A program that also links libX11 directly has the same name
  in `.dynstr` (ft2-clone does), and that one is left alone.
- checks that the string's NUL padding has room for the longer name,
- keeps the original as `<file>.orig` (only the first time), then writes
  `<file>.new` and renames it over the file, so programs already running
  are unaffected.

It reports files that are already patched and skips them. To undo, move
`<file>.orig` back.

To find other candidates:

    grep -l SDL_X11_LoadSymbols /irix/usr/sgug/bin/* /irix/usr/sgug-flx/bin/* \
        /irix/usr/sgug-flx/lib32/libSDL2*

A program linked against the shared libSDL2 doesn't need patching itself:
patch the library once.

## 2. OpenGL renderer: "swapCopy byte stride unimplemented"

```
GLbindStub: swapCopy byte stride unimplemented
```

From 2.0.18 on, SDL2's OpenGL renderer draws with interleaved vertex
arrays: float positions, `GL_UNSIGNED_BYTE` colours, float texture
coordinates. QuickTransit byte-swaps client arrays when they are drawn. In
1.2.1 its swap-copy routine for byte arrays is a stub that prints the
message above (or "Byte swapcopy unimplemented" for packed arrays) and
exits. Any IRIX GL program using byte colour or normal arrays hits it, not
just SDL.

Binary fix 7 in `patch-quicktransit.py`, applied by `install-extras.sh`,
adds the missing routine: a plain byte copy, since bytes don't need
swapping. The details are in the script's header.

Don't use `SDL_RENDER_DRIVER=software` as a workaround. It starts, but
colours are wrong: the blue channel comes out as 0, apparently because SDL's
big-endian X image isn't byte-swapped. That path wasn't investigated
further.

## Running SGUG programs

`/usr/sgug/bin/sgugshell` sets SGUG's `PATH` and library paths and starts
an interactive bash. It ignores its arguments. Its `#!/usr/sgug/bin/bash` is
an IRIX path, so from a Linux shell, start it through the IRIX bash:

    /irix/usr/sgug/bin/bash /usr/sgug/bin/sgugshell

or pipe commands in:

    echo pt2-clone | DISPLAY=:0 /irix/usr/sgug/bin/bash /usr/sgug/bin/sgugshell
