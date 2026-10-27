#!/bin/bash
# Install QuickTransit 1.2.1 (1.2 + update SG0010228) and its IRIX 6.5.27m
# environment on T2 Linux / ia64.  Run as root on the ia64 machine:
#
#   ./install.sh <input-dir>
#
# <input-dir> holds the files listed in README.md: RPMs from the QuickTransit
# ISO and the SG0010228 update, plus libstdc++.so.5[.gz] if this kit doesn't
# include it.  Subdirectories one level down are searched too.  T2 has no rpm,
# so the RPMs are unpacked with bsdtar and their %post scriptlets are
# reimplemented below.  Safe to re-run; re-run install-extras.sh afterwards if
# it was used, since this restores the vendor QuickTransit.conf and binary.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "install.sh: $*" >&2; exit 1; }
[ $# = 1 ] || die "usage: install.sh <input-dir>"
SRC=$(cd "$1" && pwd)
[ "$(id -u)" = 0 ] || die "must be run as root"
[ "$(uname -m)" = ia64 ] || die "QuickTransit needs ia64, this is $(uname -m)"
for c in bsdtar gcc python3; do
    command -v $c >/dev/null || die "$c not found"
done

need() {
    local f; f=$(find "$SRC" -maxdepth 2 -type f -name "$1" -print -quit)
    [ -n "$f" ] || die "$1 not found in $SRC"
    if [ -n "${2:-}" ]; then
        [ "$(md5sum < "$f" | cut -d' ' -f1)" = "$2" ] || die "$f: md5 mismatch"
    fi
    echo "$f"
}
IRIX_RPM=$(need IRIX-019-6.5.27m.ia64.rpm ab4bacd843a174785404513f193163ae)
UNWIND_RPM=$(need libunwind-0.97-4.ia64.rpm b9ee298554b7c45c800f6fe48b206f0e)
QT_RPM=$(need QuickTransit-1.2.1-004.ia64.rpm c90883fd6952f4d24d89ac33791f5da7)
# libstdc++.so.5, plain or gzipped, in the kit directory or the inputs
STDCXX5=$(find "$HERE" -maxdepth 1 -type f -name 'libstdc++.so.5*' -print -quit)
[ -n "$STDCXX5" ] || STDCXX5=$(need 'libstdc++.so.5*')

QT=/usr/QuickTransit
CL=$QT/compat-lib

# --- RPM payloads (bsdtar keeps owners, setuid bits and FIFOs as root)
echo ">>> IRIX 6.5.27m environment -> /irix (~840 MB)"
bsdtar -xpf "$IRIX_RPM" -C /
echo ">>> QuickTransit 1.2.1 -> $QT"
# Its SysV init script is left out: it needs LSB init functions T2 doesn't
# have (binfmt is registered via systemd below), and T2's /etc/init.d is a
# symlink, which bsdtar refuses to extract through.
bsdtar -xpf "$QT_RPM" -C / --exclude etc/init.d

# --- Kit files: license, binfmt config, shim sources, fonts
echo ">>> kit files"
tar -C "$HERE/target" --owner=0 --group=0 -cf - . |
    tar -C / --no-overwrite-dir -xf -
cp "$HERE/uninstall.sh" "$QT/uninstall.sh"

# --- Binary fixes (see patch-quicktransit.py; install-extras.sh re-patches
# with --extras).  The vendor binary was just re-extracted from the RPM; it is
# kept as QuickTransit.orig.
cp -p $QT/bin/QuickTransit $QT/bin/QuickTransit.orig
python3 -I "$HERE/patch-quicktransit.py" $QT/bin/QuickTransit.orig $QT/bin/QuickTransit
chmod 755 $QT/bin/QuickTransit

# --- QuickTransit %post: bind QT's GL/MPI libs + wrapper scripts into /irix
echo ">>> postinstall"
$QT/scripts/QTBindIRIX.sh /irix $QT $QT/scripts $QT/scripts
chmod a+w $QT/log
chmod u+s $QT/bin/setuid_irixrun

# --- Old runtime libs.  QuickTransit has DT_NEEDED libstdc++.so.5 and the
# unversioned "libunwind-ia64.so" (which ldconfig never caches), so they are
# kept in compat-lib and symlinked into /usr/lib.  T2 has none of these names
# (it ships libunwind.so.7 and libstdc++.so.6).
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
bsdtar -xf "$UNWIND_RPM" -C "$WORK" './usr/lib/libunwind*'
mkdir -p $CL
cp -a "$WORK"/usr/lib/libunwind.so.1* "$WORK"/usr/lib/libunwind-ia64.so.1* $CL/
case $STDCXX5 in
    *.gz) gzip -dc "$STDCXX5" > $CL/libstdc++.so.5 ;;
    *) cp "$STDCXX5" $CL/libstdc++.so.5 ;;
esac
chmod 755 $CL/libstdc++.so.5
for l in libstdc++.so.5 libunwind.so.1 libunwind-ia64.so.1; do
    ln -sfn $CL/$l /usr/lib/$l
done

# --- Host-side shim: libtimefake.c + qtcompat.c, built as "libunwind-ia64.so".
#   src/libtimefake.c - time() returns a 2004 date; the license expires
#                       11-apr-2005.  Only QuickTransit's own libc time() calls
#                       are affected; IRIX programs still see the real clock.
#   src/qtcompat.c    - strncat() QuickTransit's SIGBUS handler can emulate
#                       (fixes 4Dwm), and DRAW_USE_LLVM=0 for Mesa.
# It chains to the real libunwind-ia64.so.1, and because it comes before libc
# in QuickTransit's NEEDED list its symbols win.  This is used instead of
# LD_PRELOAD, which is ignored for setuid launches (setuid_irixrun) and leaks
# into every child.  -O0 is deliberate (qtcompat.c needs plain ld1 loads).
gcc -shared -fPIC -O0 -o $CL/libunwind-ia64-timefake.so \
    $CL/src/libtimefake.c $CL/src/qtcompat.c \
    -Wl,-soname,libunwind-ia64.so \
    -Wl,--no-as-needed $CL/libunwind-ia64.so.1
ln -sfn $CL/libunwind-ia64-timefake.so /usr/lib/libunwind-ia64.so

# --- QuickTransit dlopen()s "/usr/lib/nptl/librt.so" (RHEL3/SLES9 path) for
# clock_gettime, used to emulate IRIX /dev/mmem (4Dwm needs it).  glibc 2.34+
# librt.so.1 is a stub that resolves into libc, which is fine for dlsym.
mkdir -p /usr/lib/nptl
ln -sfn /lib/librt.so.1 /usr/lib/nptl/librt.so

# --- Register binfmt_misc handlers (what /etc/init.d/QuickTransit start did)
systemctl restart systemd-binfmt.service
ls /proc/sys/fs/binfmt_misc/irix32 /proc/sys/fs/binfmt_misc/irix64

# --- SGI colour names ("sgi gray 71", SGIVeryLightGray, ...).  The RPM's %post
# appended these to the X server's rgb.txt, but modern Xorg has a compiled-in
# colour table and no rgb.txt.  IRIX Xlib consults the client-side Xcms colour
# database before asking the server, so publish them there instead.
X=/irix/usr/lib/X11
{
    echo "XCMS_COLORDB_START 0.1"
    grep -i sgi $X/rgb.txt | grep -v '^!' | awk '{
        name = $4; for (i = 5; i <= NF; i++) name = name " " $i
        printf "%s\trgb:%02x/%02x/%02x\n", name, $1, $2, $3 }'
    echo "XCMS_COLORDB_END"
} > $X/Xcms.txt

# --- X font path (the RPM's %post used chkfontpath, which T2 doesn't have).
# Added to the Files section of xorg.conf; the original is kept as
# xorg.conf.pre-quicktransit.  /usr/QuickTransit/fonts (Utopia-bold aliases)
# comes first.  Speedo/CID are skipped because libXfont2 can't read them.
if ! grep -q '/irix/usr/lib/X11/fonts' /etc/X11/xorg.conf; then
    cp -p /etc/X11/xorg.conf /etc/X11/xorg.conf.pre-quicktransit
    awk '/^Section "Files"/ { infiles = 1 }
         infiles && /^EndSection/ {
             print "    # QuickTransit: IRIX fonts"
             print "    FontPath   \"/usr/QuickTransit/fonts/\""
             print "    FontPath   \"/irix/usr/lib/X11/fonts/misc/\""
             print "    FontPath   \"/irix/usr/lib/X11/fonts/75dpi/\""
             print "    FontPath   \"/irix/usr/lib/X11/fonts/100dpi/\""
             print "    FontPath   \"/irix/usr/lib/X11/fonts/Type1/\""
             infiles = 0 }
         { print }' /etc/X11/xorg.conf.pre-quicktransit > /etc/X11/xorg.conf
    grep -q '/irix/usr/lib/X11/fonts' /etc/X11/xorg.conf ||
        echo "WARNING: no Files section in /etc/X11/xorg.conf, font path not added" >&2
fi

echo ">>> done.  Test: /irix/bin/sh -c 'uname -a'"
echo "    Optional desktop/fam/IRIS GL extras: ./install-extras.sh <irix-dist-dir>"
