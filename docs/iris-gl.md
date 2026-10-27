# IRIS GL under QuickTransit

Status (2026-10-26): without help, IRIS GL programs (atlantis, buttonfly,
flight, newton, `cycol`, ...) crash. An experimental IRIX-side shim,
`libfakeci.so` (installed by `install-extras.sh`, which preloads it into
every IRIX program), gets them running:
- a test program draws correctly in both RGB and colour-index mode on Xorg
  with amdgpu;
- atlantis, buttonfly and newton draw (checked on Xvfb);
- flight still dies, with a `double free` inside QuickTransit.

The shim and the problems it works around are summarised under
[The shim](#the-shim) below.

## SGI's position

SGI never supported IRIS GL under QuickTransit. The 1.2.1 README's
"Unsupported IRIX Functionality" list includes "IRIS GL(TM)", and the
vendor's `QTBindIRIX.sh` deletes the o32 IGLOO link
(`/irix/var/arch/lib/libgl.so`), though not the n32 one.

The user guide documents `QUICKTRANSIT_EMULATE_12_BIT_CI=y`, a "partial
emulation" of 12-bit colour-index visuals for OpenGL programs (on by default
in 1.0, off in 1.2). It doesn't help IGLOO: with it set, IRIS GL programs
still crash in `RC_glXCreateContext` (tested 2026-10-26).

## IRIX already implements IRIS GL on OpenGL

On machines without IRIS GL in hardware, IRIX's `libgl.so` is **IGLOO**
("IRIS GL on OpenGL"): `/usr/gfx/arch/IGLOO32/libgl.so` (n32) and
`/usr/gfx/arch/IGLOO/libgl.so` (o32). It's an ordinary client library that
needs `libGL.so`, `libGLU.so`, `libX11.so.1`, `libXext.so`, `libXi.so`,
`libXmu.so` and `libXsgivc.so`. A real IRIX 6.5.30 Octane (ESI graphics) uses
it; its `/var/arch/lib32/libgl.so` points there.

The QuickTransit IRIX tree already has both IGLOO libraries.
`/irix/usr/lib32/libgl.so` → `/irix/var/arch/lib32/libgl.so` → IGLOO32, so
n32 IRIS GL programs load IGLOO, and IGLOO's OpenGL calls go to
QuickTransit's GL bridge. The o32 link (`/irix/var/arch/lib/libgl.so`) is
missing. The demos listed above are all n32.

## Why it crashes

    --- PROCESS CRASHED WITH SIGBUS (7)
    Crash caught ... at ip 0x400000000023ee31 [RC_glXCreateContext + 0xaf1],
    caused accessing address 0x14.

The visual argument to `glXCreateContext` is NULL; the crash is reading a
field at offset 0x14 of it. A logging interposer (preloaded with
`FU_RLDN32_LIST`, see [qt-internals.md](qt-internals.md)) on atlantis shows:

    glXChooseVisual(GLX_USE_GL)  -> visual 0x354
    glXChooseVisual(GLX_RGBA)    -> visual 0x354
    glXCreateContext(vis=…)      ok
    glXCreateContext(vis=NULL)   crash

IGLOO picks its other visual by walking `XGetVisualInfo` and calling
`glXGetConfig` on each one. On the host (Xorg, amdgpu, depth 24), every
visual reports `GLX_RGBA = 1` and `GLX_LEVEL = 0`. There's no colour-index
visual and no overlay visual. IGLOO doesn't check the result of its search
and passes NULL on.

IRIS GL windows start in colour-index mode (`cmode`) until the program calls
`RGBmode(); gconfig()`, so a colour-index visual is wanted at `winopen` time.
Experiment: the interposer substituted an RGBA visual for the NULL. IGLOO
then got past `glXCreateContext` and failed at `XCreateColormap` with
`BadMatch`. That's a writable colormap requested on a TrueColor visual,
which confirms IGLOO is setting up a colour-index (PseudoColor) window.

Notes:
- QuickTransit's own `glXChooseVisual` quietly maps colour-index requests
  (`GLX_BUFFER_SIZE 8`, no `GLX_RGBA`) to a TrueColor visual. IGLOO doesn't
  go through that path for this window.
- `glXChooseVisual(GLX_LEVEL 1)` (overlay) returns NULL.
- The crash is the same on Xvfb (softpipe) and on Xorg with amdgpu.

## The shim

`libfakeci.so` (source `extras/src/fakeci.c`) is preloaded into every
IRIX n32 program, ahead of `libglxsync.so` and `libGL.so`, by the
`FU_RLDN32_LIST` line `install-extras.sh` adds to `QuickTransit.conf` (see
[qt-internals.md](qt-internals.md#irix-dynamic-linker-preload-list)), so
IRIS GL programs run from any IRIX shell, the toolchest or Linux.

It only acts in IRIS GL programs: the first time it is needed it looks up
`gconfig` (exported by IGLOO's `libgl.so`, not wrapped by the shim) in the
program's global scope. Elsewhere every wrapper passes straight through.
Globally, the fake PseudoColor visuals would otherwise be picked by old
programs that prefer 8-bit visuals, and turning `RetainPermanent` into
"keep the connection open" would lose what tools like background setters
leave on the server.

Both preload libraries wrap `glXMakeCurrent`, `glXSwapBuffers` and
`XChangeProperty`. IRIX 6.5 rld has no `RTLD_NEXT`, so `libglxsync.so`
also exports its versions as `glxsync_<name>`, and `libfakeci.so` calls
those when present (otherwise the libGL/libX11 originals).

Setting `DYNAMITE_CONFIG_FILE` only works from Linux: an IRIX process that
sets it before `exec` keeps its own config. It
works around:

1. **No colour-index visuals.** It adds PseudoColor clones of the TrueColor
   visuals. Each clone has its own visual ID, because IGLOO looks visuals up
   by ID and by window. It keeps the IRIS GL colour map as a palette in the
   process, and in contexts created on a clone it turns index calls
   (`glIndex*`, `glClearIndex`, `GL_COLOR_INDEX` pixels, fog/lighting
   indexes, mode queries) into RGB.
2. **Shared colormaps.** IGLOO publishes per-visual colormaps on the root
   window (`_SGI_GL_DEFAULT_COLORMAPS`, `_SGI_IGLOO_DEFAULT_COLORMAPS`) from
   a RetainPermanent helper connection. The shim hides and skips these and
   keeps the helper connection open instead.
3. **Swaps on the wrong connection.** IGLOO calls `glXSwapBuffers` on a
   different `Display` from the one the context is current on, so the swap
   showed nothing. The shim swaps on the current context's display.
4. **Fork deadlock.** `winopen()` forks into the background unless the
   program called `foreground()`. That fork comes after GL contexts exist,
   and the child deadlocks on Mesa's driver-thread locks (the same problem
   as perfly's multi-process mode). The shim calls `foreground()` first.

Not handled: colormap animation (`mapcolor` doesn't recolour pixels already
drawn), overlay/pop-up planes, and real colour-index lighting.

## Ways forward (as considered before the shim)

1. **Fake a colour-index visual for IGLOO** with an IRIX-side interposer
   library built with the IRIX gcc ([irix-gcc.md](irix-gcc.md)):
   - make `glXGetConfig` report one TrueColor visual as `GLX_RGBA = 0`,
     `GLX_BUFFER_SIZE = 8`;
   - turn `XCreateColormap(AllocAll)` on it into `AllocNone`, and turn
     `XStoreColors` into no-ops or a lookup table.

   Programs that switch to `RGBmode` straight away (most demos) might then
   run. Real colour-index drawing would still come out wrong.
2. **Proper colour-index emulation:** keep IGLOO's colour map in the
   interposer and turn `glIndex*` and `glClearIndex` into RGB. A lot of work,
   but still entirely IRIX-side.
3. **Host-side colour-index visual:** an X server running at depth 8
   PseudoColor offers real CI visuals, but current Mesa drivers don't
   provide GLX on them.
4. **Replace IGLOO:** a stub `libgl.so` of SCall markers calling a native
   IRIS GL library on the host
   ([qt-internals.md](qt-internals.md#native-library-binding-scall)). By far
   the most work; only worth it if IGLOO turns out to be a dead end.

Option 1 is what the shim does.
