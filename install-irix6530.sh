#!/bin/bash
# Install IRIX 6.5.30 from the IRIX CDs as a second IRIX root, /irix6530,
# next to QuickTransit's own IRIX 6.5.27m tree in /irix (which is left as it
# is).  Run as root, after install.sh (and install-extras.sh, if used):
#
#   ./install-irix6530.sh <irix-media>...
#
# <irix-media> are IRIX CDs, each either a tar archive of the CD (.tar,
# .tar.gz, ...) or a directory (the CD's dist directory, or a directory above
# it).  Every product on them is installed, the way inst would install them
# all: the newest full image of each subsystem, with newer overlays on top
# (see irix-dist-extract.py).  Give each CD once, base CDs and overlays
# alike; docs/irix-6530.md lists a tested set.  Afterwards
#
#   irix6530 [<command> [<args>...]]
#
# runs IRIX programs in the 6.5.30 tree (an IRIX shell without arguments).
# Safe to re-run: the tree is rebuilt from scratch and the previous one is
# kept as /irix6530.old.  Re-run it after install.sh or install-extras.sh,
# since its QuickTransit config is a copy of theirs.  See docs/irix-6530.md.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "install-irix6530.sh: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "must be run as root"
QT=/usr/QuickTransit
[ -f $QT/bin/QuickTransit.orig ] || die "run install.sh first"
[ $# -gt 0 ] || die "usage: install-irix6530.sh <irix-media>..."
for m in "$@"; do [ -e "$m" ] || die "$m: not found"; done
ROOT=/irix6530
CONF=$QT/etc/QuickTransit-6530.conf

# --- Media.  CD archives are unpacked (their dist directories only) on the
# root filesystem: several GB for the whole set, too big for a tmpfs /tmp.
WORK=$(mktemp -d /var/tmp/qt-irix6530.XXXXXX); trap 'rm -rf "$WORK"' EXIT
DISTS=() n=0
for m in "$@"; do
    if [ -f "$m" ]; then
        n=$((n + 1)); d=$WORK/media$n; mkdir "$d"
        echo ">>> reading $m"
        tar --warning=no-alone-zero-block -tf "$m" > "$WORK/list" || die "$m: can't read"
        grep -E '(^|/)dist[0-9.]*/[^/]+$' "$WORK/list" > "$d.list" || true
        [ -s "$d.list" ] &&
            tar --warning=no-alone-zero-block -C "$d" -xf "$m" -T "$d.list"
    else
        d=$m
    fi
    # The directories holding .idb files.  A CD with images for several
    # IRIX releases (MIPSpro All-Compiler: dist6.3, dist6.4, dist6.5) gives
    # only its dist6.5.
    mapfile -t ds < <(find "$d" -maxdepth 4 -name '*.idb' -printf '%h\n' | sort -u)
    six=$(printf '%s\n' "${ds[@]}" | grep '/dist6\.5$' || true)
    [ -z "$six" ] || mapfile -t ds <<< "$six"
    if [ ${#ds[@]} = 0 ] || [ -z "${ds[0]}" ]; then
        echo "    $m: no IRIX products, skipped"; continue
    fi
    DISTS+=("${ds[@]}")
done
[ ${#DISTS[@]} -gt 0 ] || die "no IRIX products on the media"

# --- The IRIX tree, built in $ROOT.new.  irix-dist-extract.py prints a line
# per product and, at the end, the install scripts (exitops) it did not run.
echo ">>> IRIX products -> $ROOT"
rm -rf $ROOT.new
python3 -I "$HERE/irix-dist-extract.py" --install $ROOT.new "${DISTS[@]}" > "$WORK/log" 2>&1 ||
    { tail -20 "$WORK/log"; die "extracting the products failed"; }
sed -n '/^install scripts NOT run:/q; p' "$WORK/log" | sed 's/^/    /'
mkdir -p $ROOT.new/var/adm
cp "$WORK/log" $ROOT.new/var/adm/install-irix6530.log
echo "    full log: $ROOT/var/adm/install-irix6530.log"

# --- Host glue, as in QuickTransit's /irix: devices, users, hosts and the
# directories IRIX programs share with the host.  QuickTransit maps IRIX
# paths into the root, but the host kernel follows symlinks, so these
# absolute links reach the host's files.
echo ">>> host links"
cd $ROOT.new
rm -rf dev && mkdir dev
for l in console:console log:log mmem:zero null:null stdin:stdin tty:tty \
         ttyd1:ttyIOC4/0 ttyd2:ttyIOC4/1 ttys:pts zero:zero; do
    ln -s "/dev/${l#*:}" "dev/${l%%:*}"
done
: > dev/kmem
for f in aliases ftpusers group hosts hosts.equiv inetd.conf nsswitch.conf \
         passwd resolv.conf services shadow; do
    rm -rf "etc/$f"; ln -s "/etc/$f" "etc/$f"
done
for d in home mnt root tmp; do rm -rf "$d"; ln -s "/$d" "$d"; done
rm -rf var/tmp && mkdir -m 1777 var/tmp
# Files IRIX writes at boot, which are on no CD; QuickTransit's /irix ships
# them.  fam recurses until it crashes when it can't read /etc/mtab.
printf '%s\n' '/dev/root / xfs rw,raw=/dev/rroot 0 0' '/hw /hw hwgfs rw 0 0' \
    '/proc /proc proc rw 0 0' '/dev/fd /dev/fd fd rw 0 0' > etc/mtab
echo '/dev/root / xfs rw,raw=/dev/rroot 0 0' > etc/fstab
: > var/adm/utmp
cd /

# --- QuickTransit %post on the new tree (QTBindIRIX.sh): its GL/MPI libs,
# wrapper scripts (su, lp, at, ...) and "not supported" stubs.  One stub is
# /usr/lib/desktop/waitmap, which works and which the toolchest needs
# (install-extras.sh restores it in /irix too).
cp -p $ROOT.new/usr/lib/desktop/waitmap "$WORK/waitmap" 2>/dev/null || true
$QT/scripts/QTBindIRIX.sh $ROOT.new $QT $QT/scripts $QT/scripts
[ -f "$WORK/waitmap" ] && install -m 755 "$WORK/waitmap" $ROOT.new/usr/lib/desktop/waitmap

# --- install-extras.sh's IRIX-side files, if it was run: fam.conf and the
# ALSA libaudio (docs/irix-audio.md).
if [ -f $QT/lib/irix/lib32/libaudio.so ]; then
    echo ">>> install-extras.sh files"
    install -m 644 "$HERE/extras/target/irix/etc/fam.conf" $ROOT.new/etc/fam.conf
    A=$ROOT.new/usr/lib32/libaudio.so.1
    [ -e $A ] && mv $A $A.sgi
    cp $QT/lib/irix/lib32/libaudio.so $A
fi

# --- Swap the trees.  The previous one is kept as $ROOT.old.
if [ -d $ROOT ]; then rm -rf $ROOT.old; mv $ROOT $ROOT.old; fi
mv $ROOT.new $ROOT

# --- QuickTransit config for this root: the current one plus
#   FU_IRIX_ROOT                the IRIX root (default /irix)
#   FE_INTERPRETER_FSROOT       where the programs' ELF interpreter
#                               (/usr/lib32/libc.so.1) is loaded from: a
#                               separate setting, also /irix by default
#   FU_PLATFORM_RELEASEREVNAME  what uname -R reports (default 6.5.27m)
# QuickTransit reads it instead of QuickTransit.conf when
# DYNAMITE_CONFIG_FILE names it (the irix6530 launcher sets that).
{
    cat $QT/etc/QuickTransit.conf
    echo "# install-irix6530.sh"
    echo "FU_IRIX_ROOT=$ROOT"
    echo "FE_INTERPRETER_FSROOT=$ROOT"
    echo "FU_PLATFORM_RELEASEREVNAME=30m"   # printed after "6.5."
} > $CONF

# --- Launcher
cat > /usr/bin/irix6530 <<EOF
#!/bin/sh
# irix6530 [<command> [<args>...]]: run IRIX programs in the IRIX 6.5.30 tree
# $ROOT (install-irix6530.sh from the QuickTransit kit).  The command is
# looked up in the IRIX PATH; no command: an IRIX shell.  IRIX programs only
# see $ROOT and the host's /home, /root, /tmp and /mnt, so the working
# directory has to be in one of them.
export DYNAMITE_CONFIG_FILE=$CONF
export PATH=/usr/sbin:/usr/bsd:/sbin:/usr/bin:/usr/bin/X11:/etc:/usr/etc
[ \$# = 0 ] && exec $ROOT/sbin/sh -i
exec $ROOT/sbin/sh -c 'exec "\$@"' sh "\$@"
EOF
chmod 755 /usr/bin/irix6530

# --- Desktop session launcher, if install-extras.sh (irix-desktop,
# start4dwm-x11) was run.
if [ -x $QT/bin/irix-desktop ]; then
    cat > /usr/bin/start4dwm-6530 <<EOF
#!/bin/sh
# start4dwm-x11 with the IRIX 6.5.30 tree ($ROOT): an X server running the
# IRIX desktop session.  Arguments as for start4dwm-x11 (default :0 vt7).
export IRIX_ROOT=$ROOT DYNAMITE_CONFIG_FILE=$CONF
exec start4dwm-x11 "\$@"
EOF
    chmod 755 /usr/bin/start4dwm-6530
fi

# --- Two kinds of the products' install scripts, which only make files in
# the tree: Motif's links that make Motif 1.2 the default (/usr/include/Xm,
# /usr/lib32/libXm.so, ...; run by motif_dev, ViewKit_dev, ...), and the
# desktop's file-type database (make in /usr/lib/filetype, run by most
# desktop products).  The rest are listed in the log; see docs/irix-6530.md.
echo ">>> Motif links, desktop file types"
M=$ROOT/usr/Motif-1.2/lib/mksymlinks
[ ! -f $M ] || sh $M $ROOT > /dev/null
(cd /root && /usr/bin/irix6530 sh -c 'cd /usr/lib/filetype && make' > /dev/null) ||
    echo "    WARNING: building the desktop file types failed" >&2

echo ">>> done.  $(du -sh $ROOT | cut -f1) in $ROOT.  Try: irix6530 uname -aR"
