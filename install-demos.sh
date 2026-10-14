#!/bin/bash
# Install IRIX demo products from IRIX 6.5 install media into /irix, using
# irix-dist-extract.py (no IRIX inst needed).  Run as root after install.sh:
#
#   ./install-demos.sh <dist-dir>
#
# <dist-dir> is an IRIX 6.5 "dist" directory holding the products below
# (<product>.idb plus its .sw/.data archives); tested with 6.5.30.
#
# Products (subsystems) installed:
#   demos           demos.sw.*         /usr/demos (General_Demos, Buttonfly ...)
#   performer_eoe   performer_eoe.sw.* Performer runtime (Intersim, VisFly)
#   performer_demo  performer_demo.{sw,data}.*  Performer demos + town/city data
#   inventor_eoe    inventor_eoe.{demo,data}.*  Inventor demo programs + sample models
# Install scripts (exitop) are not run. They only rebuild the demos web index
# and the desktop filetype database.
set -euo pipefail
[ $# = 1 ] || { echo "usage: install-demos.sh <dist-dir>" >&2; exit 2; }
DIST=$1
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d /irix ] || { echo "run install.sh first" >&2; exit 1; }
# staged on the root filesystem: ~820 MB, too big for a tmpfs /tmp
STAGE=$(mktemp -d /var/tmp/irix-demos.XXXXXX); trap 'rm -rf "$STAGE"' EXIT
chmod 755 "$STAGE"

x() { python3 -I "$HERE/irix-dist-extract.py" "$DIST" "$1" "$STAGE" "$2" | sed -n 1p; }
x demos          '^demos\.sw\.'
x performer_eoe  '^performer_eoe\.sw\.'
x performer_demo '^performer_demo\.(sw|data)\.'
x inventor_eoe   '^inventor_eoe\.(demo|data)\.'

echo ">>> copying $(du -sh "$STAGE" | cut -f1) to /irix"
tar -C "$STAGE" --owner=0 --group=0 -cf - . | tar -C /irix --no-overwrite-dir -xf -
