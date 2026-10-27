# The IRIX desktop (4Dwm, toolchest, fm) under QuickTransit

Status (2026-10-27, pople, Xorg + amdgpu): the full IRIX 6.5 desktop
session (`/var/X11/xdm/Xsession.dt`) runs. That includes 4Dwm, the
toolchest (its menus launch programs, e.g. *Open Unix Shell* → winterm, and
*Log Out* works), the file manager (`fm`), the desktop background icons
(`fm -b`), Icon Catalog, fam, the image backgrounds and Twilight.

Everything it needs beyond `install.sh` is done by `install-extras.sh`:

    ./install-extras.sh /path/to/irix/dist
    startx /usr/QuickTransit/bin/irix-desktop -- :0

That is:

1. desktop files the QuickTransit IRIX RPM leaves out, installed from IRIX
   media, and the real `waitmap` ([Missing desktop files](#missing-desktop-files));
2. QuickTransit binary fixes for fam and IRIX `sh`
   ([QuickTransit fixes](#quicktransit-fixes));
3. rpcbind and fam ([fam](#fam));
4. `libglxsync.so`, an IRIX-side preload library that keeps icon labels
   from being painted over and makes GL backgrounds like Twilight visible
   ([Icon labels](#icon-labels-mixed-gl-and-x-drawing),
   [Twilight](#twilight-and-other-gl-backgrounds)).

## Missing desktop files

The IRIX environment in the QuickTransit RPM is 6.5.27m, and parts of
`desktop_eoe` are missing. The gaps that matter:

| Subsystem | Missing | Effect |
|---|---|---|
| `desktop_eoe.sw.toolchest` | `toolchest`, `tellwm`, toolchest icons | no toolchest |
| `desktop_eoe.sw.control_panels` | most app-defaults, including `Desktop` (fm's resources), `Background`, `Scheme`, `Windows`; the control-panel programs; `system.backgrounds` | fm and panels use built-in defaults |
| `desktop_eoe.sw.envm` | `var/X11/xdm/Xsession.dt` and its icon lists | no desktop session script |
| `desktop_eoe.sw.Desks`, `.sw.share` | Overview (extra desks), session warning messages | |

`install-extras.sh` extracts these subsystems from `desktop_eoe` and, if
present, the `desktop_eoe_6530m` overlay, with `irix-dist-extract.py`, and
copies only files `/irix` doesn't already have, so the RPM's 6.5.27m files
stay as they are (110 files plus the toolchest from 6.5.30 media). Overlay
images keep the base subsystem names (`desktop_eoe_6530m.idb` lists
`desktop_eoe.sw.envm`) but store the data in `<product>.sw`. Some files
come in several variants for different hardware (`mach(...)` in the idb);
the extractor picks the ones for an Octane (IP30, MGRAS graphics), which
is what QuickTransit reports. `system.backgrounds` is one: the VOYAGER
variant lacks Twilight.

**waitmap.** QuickTransit's postinstall (`QTBindIRIX.sh`) replaces the
programs in `/usr/QuickTransit/scripts/unsupported-IRIX-binary-list` with a
script that prints "IRIX application ... is not currently supported" and
exits 1. One of them is `/usr/lib/desktop/waitmap`, which the toolchest runs
most of its commands through (`waitmap /bin/sh -c "endsession&"` for Log
Out), so those menu items silently did nothing. The real waitmap works and
`install-extras.sh` puts it back. The other stubbed programs (`xhost`,
`dmplay`, `SceneViewer`, ...) are left alone.

## QuickTransit fixes

In `patch-quicktransit.py --extras` (fixes 5 and 6):

- **fam exits on the first client.** IRIX `fam` calls `setgroups()`
  (`syssgi(SGI_SETGROUPS)`) for each client connection. QuickTransit
  logs "SGI_SETGROUPS ignored" but returns -1, and fam exits ("failed to set
  groups: Function not implemented"). fm then shows "The File Alteration
  Monitor has stopped responding" and empty windows. The patch makes the
  call return 0, as the message says. Supplementary groups don't change.
- **"too many jobs".** QuickTransit answers `sysconf(_SC_CHILD_MAX)` with 6
  (and `_SC_OPEN_MAX` with 16), the old POSIX minimums. IRIX `sh` sizes its
  job table from CHILD_MAX, so a script with more than five background jobs
  running at once dies. `Xsession.dt` is such a script. Real IRIX 6.5 says
  1034 (CHILD_MAX) and 200 (OPEN_MAX, its default `rlimit_nofile`). The
  patch returns 1034 and 1024 (QuickTransit reports RLIMIT_NOFILE as 1024).

## fam

fm, the background icons and Icon Catalog need fam, which needs rpcbind
(E2 in the README). `irix-desktop` starts both when needed; by hand:

    rpcbind
    /irix/usr/etc/fam -f &          # -d for a debug log

fam then polls (there's no `/dev/imon`), every 6 s by default
(`nfs_polling_interval` in `/irix/etc/fam.conf`). rpcbind keeps the
registration of a fam that was killed, and fm then reports that fam has
stopped responding; `irix-desktop` checks for the fam process and drops a
stale registration (`rpcinfo -d 391002 2`) before starting a new one.

## Icon labels (mixed GL and X drawing)

The desktop icon views (fm windows, the background, Icon Catalog) draw icons
with OpenGL and their labels with X, in the same single-buffered window,
without `glXWaitGL()` between the two. On SGI hardware GL and X draw into
the same framebuffer, so that works. Current Mesa renders single-buffered
windows into a "fake front" buffer and copies all of it to the window on
flush. The icon panel releases its context after drawing the labels, the
flush copies the buffer, and every label disappears.

A second problem appeared once waits were added: QuickTransit's GL runs on
its own host X connection, so `glXWaitX()` syncs that connection, not the
program's. The program's X requests can still be sitting in the IRIX Xlib
buffer when Mesa copies the window. An `XSync()` on the program's display
before `glXWaitX()` fixes that. (Test program:
`tools/irix-probes/glxmix.c`. It loses the X text under QuickTransit
unless it syncs first; native ia64 programs don't need the sync.)

`libglxsync.so` (source in `extras/src/glxsync.c`) is preloaded into every
IRIX n32 program through `FU_RLDN32_LIST` in `QuickTransit.conf`
([qt-internals.md](qt-internals.md#irix-dynamic-linker-preload-list)).
It records the current GLX drawable. When an X drawing call targets that
drawable, it calls `glXWaitGL()` first, then the X call, then `XSync()` and
`glXWaitX()`. With it, fm draws its labels as IRIX does. Programs that
don't use GL only pay for one comparison per wrapped X call.

## Twilight and other GL backgrounds

Backgrounds are chosen in the Background panel (toolchest *Desktop >
Customize > Background*). Image backgrounds work as they are: 4Dwm renders
the image into a pixmap and publishes it as `_SGI_DESKS_BACKGROUND` on the
root window, and `fm -b` makes that pixmap the background of its
full-screen icon window.

"-execute" backgrounds (Twilight, `/usr/sbin/twilight`) are separate
programs. 4Dwm starts them and deletes `_SGI_DESKS_BACKGROUND`. Twilight
names its full-screen GL window in `_SGI_ROOTPAINTER_ID` and lowers it to
the bottom of the stack. On SGI hardware fm's icons are in the overlay
planes, so Twilight shows through. Xorg has no overlay visuals
(`_SGI_ICONS_VISUALS` is all zeros), so fm's window is an ordinary opaque
one, and it covered Twilight with a solid colour.

`libglxsync.so` handles this too: when a program sets `_SGI_ROOTPAINTER_ID`,
each frame it swaps on that window (at most one a second) is read back
with `glReadPixels` into a pixmap. That pixmap becomes the root window's
background and `_SGI_DESKS_BACKGROUND`, so fm shows it like an image
background. Twilight redraws slowly, so the cost is small. When 4Dwm
switches to another background it kills Twilight and sets its own pixmap.

## Starting the session

    startx /usr/QuickTransit/bin/irix-desktop -- :0

or, on an X server already running without a window manager,
`DISPLAY=:0 /usr/QuickTransit/bin/irix-desktop`. The launcher starts
rpcbind and fam if needed, sets HOME/USER/LOGNAME and `SHELL=/bin/sh`, and
runs `/var/X11/xdm/Xsession.dt` under IRIX `/bin/sh` (it uses IRIX paths).
That starts 4Dwm, toolchest, `fm -b`, Icon Catalog on first login,
soundscheme and so on, and ends with `exec /usr/bin/X11/reaper`, which only
sets `_SGI_SESSION_PROPERTY` on the root window and exits. IRIX xdm ends
the session when that property goes away, which is what Log Out
(`endsession` → `tellwm end_session`) does. The launcher waits for that
with IRIX `xprop -root -exists _SGI_SESSION_PROPERTY`, then kills the
session's remaining IRIX processes (fmserv outlives the X server
otherwise) and exits, which ends startx. fam keeps running.

The background icons show `$HOME/Desktop`.

## Known issues

- **Two background icons have labels but no picture:** the home directory
  and the host's `/root/Desktop/.directory` (a KDE file). Not investigated.
- **No IRIX audio driver:** `audiostate` reports "This machine has no
  audio driver at all", and the audio control panels don't work. Programs
  using the Audio Library play through ALSA with `install-extras.sh`
  ([irix-audio.md](irix-audio.md)).
- **"Eject Media"** in the toolchest is greyed out (no sysadm catalog
  installed).
- **Don't overwrite a library in place** while IRIX programs use it (e.g.
  `cp new.so /usr/QuickTransit/lib/irix/lib32/libglxsync.so` during a
  session): QuickTransit maps it, and every program using it crashes.
  Write a new file and `mv` it over the old one (the installers do the
  equivalent).
