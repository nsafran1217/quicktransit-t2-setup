#!/bin/sh
# Remove everything install.sh and install-extras.sh put on the system.  Run
# as root on it:
#   ./uninstall.sh        (also installed as /usr/QuickTransit/uninstall.sh)
set -x
for e in irix32 irix64; do
    [ -e /proc/sys/fs/binfmt_misc/$e ] && echo -1 > /proc/sys/fs/binfmt_misc/$e
done
# Stop all IRIX programs.  Interactive IRIX shells ignore SIGTERM, and on an
# NFS root a file still open stays as .nfs* and keeps its directory.
P='^/usr/QuickTransit/bin/QuickTransit'
pkill -f "$P" && sleep 2
pkill -KILL -f "$P" && sleep 1
rm -f /etc/binfmt.d/quicktransit.conf
rm -f /usr/lib/libstdc++.so.5 /usr/lib/libunwind.so.1 /usr/lib/libunwind-ia64.so.1 \
      /usr/lib/libunwind-ia64.so /usr/lib/nptl/librt.so
rmdir /usr/lib/nptl 2>/dev/null || true
if [ -f /etc/X11/xorg.conf.pre-quicktransit ]; then
    mv /etc/X11/xorg.conf.pre-quicktransit /etc/X11/xorg.conf
fi
if [ -f /etc/services.pre-quicktransit ]; then
    mv /etc/services.pre-quicktransit /etc/services
fi
rm -rf /etc/flexlm /etc/init.d/QuickTransit /usr/QuickTransit \
       /usr/share/doc/QuickTransit-1.2.1 /irix
