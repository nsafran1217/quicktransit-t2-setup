#!/bin/bash
# Optional additions on top of install.sh: the IRIX desktop session (4Dwm,
# toolchest, fm, fam), IRIS GL programs and IRIX audio.  Run as root, after
# install.sh:
#
#   ./install-extras.sh [<irix-dist-dir>]
#
# <irix-dist-dir> is an IRIX 6.5 "dist" directory (desktop_eoe.idb, ...;
# 6.5.30 with the _6530m overlays was tested).  Without it the desktop files
# are skipped.  Safe to re-run; re-run it after install.sh, which restores the
# vendor QuickTransit.conf and the base-patched binary.  See README.md
# ("Optional extras") and docs/irix-desktop.md.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "install-extras.sh: $*" >&2; exit 1; }
[ $# -le 1 ] || die "usage: install-extras.sh [<irix-dist-dir>]"
[ "$(id -u)" = 0 ] || die "must be run as root"
QT=/usr/QuickTransit
[ -f $QT/bin/QuickTransit.orig ] || die "run install.sh first"
DIST=
if [ $# = 1 ]; then
    DIST=$(cd "$1" && pwd)
    [ -f "$DIST/desktop_eoe.idb" ] || die "$DIST: no desktop_eoe.idb"
fi

# --- Binary fixes 3-7 (fam, IRIX sh job control, GL byte arrays; see
# patch-quicktransit.py), applied to the vendor binary along with the base
# ones.
echo ">>> QuickTransit binary: base + extra fixes"
python3 -I "$HERE/patch-quicktransit.py" --extras $QT/bin/QuickTransit.orig $QT/bin/QuickTransit.new
chmod 755 $QT/bin/QuickTransit.new
mv -f $QT/bin/QuickTransit.new $QT/bin/QuickTransit

# --- Kit files: launchers, IRIX-side shims (prebuilt, sources in
# extras/src), fam.conf
echo ">>> extras files"
tar -C "$HERE/extras/target" --owner=0 --group=0 -cf - . |
    tar -C / --no-overwrite-dir -xf -

# --- Preload list.  The IRIX path /usr/QuickTransit/lib32 is the host's
# /usr/QuickTransit/lib/irix/lib32.  QuickTransit sets the IRIX rld preload
# list from FU_RLDN32_LIST on every exec; the last such line wins, and the
# vendor config has none, so DEFAULT is the stock hooks.  Both libraries go
# into every n32 program:
#   libfakeci  - colour-index visuals for IRIS GL.  Does nothing in programs
#                that aren't linked with IRIS GL (IGLOO's libgl.so).
#   libglxsync - X drawing on a GL window waits for GL (icon labels in fm,
#                the background, Icon Catalog), and GL backgrounds like
#                Twilight show behind fm's icons.
# libfakeci has to come first: it chains to libglxsync's glxsync_* exports
# for the calls both wrap.
L=/usr/QuickTransit/lib32
CONF=$QT/etc/QuickTransit.conf
sed -i '/^# install-extras.sh$/d; /^FU_RLDN32_LIST=/d' $CONF
printf '%s\n' "# install-extras.sh" \
    "FU_RLDN32_LIST=$L/libfakeci.so:$L/libglxsync.so:$L/libX11_hook.so:$L/libXi_hook.so:DEFAULT" >> $CONF

# --- IRIX audio.  SGI's libaudio drives the hdsp kernel driver, which
# QuickTransit doesn't emulate.  The kit's libaudio.so (IRIX n32, prebuilt
# in extras/target) calls libRCAudio.so (native, built here) through
# QuickTransit's native-call binding, and that plays through ALSA's default
# device.  SGI's library is kept as libaudio.so.1.sgi.  docs/irix-audio.md.
echo ">>> IRIX audio"
if [ -f /usr/include/alsa/asoundlib.h ]; then
    gcc -O2 -Wall -shared -fPIC -o $QT/lib/ia64/libRCAudio.so.new \
        "$HERE/extras/src/irixaudio/rcaudio.c" -lasound
    mv -f $QT/lib/ia64/libRCAudio.so.new $QT/lib/ia64/libRCAudio.so
    A=/irix/usr/lib32/libaudio.so.1
    [ -e $A.sgi ] || mv $A $A.sgi
    cp $QT/lib/irix/lib32/libaudio.so $A.new
    mv -f $A.new $A
else
    echo "    no ALSA headers (alsa-lib): IRIX audio skipped"
fi

# --- T2's rpcbind can't start its TCP/UDP listeners: it looks up the service
# names "rpcbind" and "portmapper", but /etc/services has no aliases on the
# sunrpc lines.  Add them (original kept as /etc/services.pre-quicktransit).
# Needed by IRIX fam (and so the fm file manager).
if ! grep -qE '^sunrpc[[:space:]]+111/tcp[[:space:]]+portmapper' /etc/services; then
    cp -p /etc/services /etc/services.pre-quicktransit
    sed -i -E 's|^(sunrpc[[:space:]]+111/(tcp\|udp))([[:space:]])|\1 portmapper rpcbind\3|' /etc/services
fi

# --- Desktop files missing from the RPM's 6.5.27m tree (toolchest,
# Xsession.dt, app-defaults, control panels, ...), from IRIX media.  Only
# files /irix doesn't have are added; the _6530m overlay goes over the base.
if [ -n "$DIST" ]; then
    echo ">>> IRIX desktop files from $DIST"
    STAGE=$(mktemp -d /var/tmp/qt-desktop.XXXXXX); trap 'rm -rf "$STAGE"' EXIT
    R='desktop_eoe\.sw\.(toolchest|control_panels|envm|Desks|share)$'
    for p in desktop_eoe desktop_eoe_6530m; do
        [ -f "$DIST/$p.idb" ] && python3 -I "$HERE/irix-dist-extract.py" "$DIST" $p "$STAGE/root" "$R"
    done
    (cd "$STAGE/root" && find . \( -type f -o -type l \) | sed 's|^\./||' |
        while read -r f; do [ -e "/irix/$f" ] || [ -L "/irix/$f" ] || echo "$f"; done) > "$STAGE/add"
    tar -C "$STAGE/root" --owner=0 --group=0 -cf - -T "$STAGE/add" |
        tar -C /irix --no-overwrite-dir -xf -
    echo "    $(wc -l < "$STAGE/add") files added to /irix"
    # QuickTransit's postinstall replaces the programs in
    # scripts/unsupported-IRIX-binary-list with a script that says
    # "not currently supported".  One is /usr/lib/desktop/waitmap, which the
    # toolchest runs most commands through (Log Out among them); it works.
    for f in $(cat $QT/scripts/unsupported-IRIX-binary-list); do
        if [ -f "$STAGE/root/$f" ] && grep -q "is not currently supported" "/irix/$f" 2>/dev/null; then
            install -m 755 "$STAGE/root/$f" "/irix/$f"
            echo "    restored $f"
        fi
    done
else
    echo ">>> no IRIX dist given: desktop files skipped"
fi

echo ">>> done.  Desktop: startx $QT/bin/irix-desktop -- :0"
