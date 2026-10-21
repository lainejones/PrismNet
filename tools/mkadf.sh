#!/bin/sh
# mkadf.sh - the install floppy: dist/PrismNet-<VER>.adf, an 880 KB (DD) FFS
# disk named PrismNet with the install drawer at its root (insert it, open it,
# double-click Install_PrismNet). Run in WSL after dist.sh; needs amitools'
# xdftool (~/amitools-venv).
#
# The whole drawer is 972 KB, so the floppy leaves out what installing does
# not need: the manual (it is in the .lha and on GitHub - the installer skips
# it when it is missing), Tools/Net (the same ping/traceroute/nslookup are in
# C, plus httpbench and its PC-side scripts) and the bsdtest/ugtest test
# programs. Everything the installer installs is on it, the small core for
# 1-2 MB machines included.
#
# GPL-3.0-only, see LICENSE.
set -e
cd "$(dirname "$0")/.."
X=${XDFTOOL:-$HOME/amitools-venv/bin/xdftool}
VER=$(sed -n 's/^VER=//p' dist.sh)
D=dist/PrismNet
ADF=$(pwd)/dist/PrismNet-$VER.adf
[ -d "$D" ] || { echo "mkadf.sh: no $D - run dist.sh first" >&2; exit 1; }

S=$(mktemp -d)
cp -r "$D/." "$S/"
rm -rf "$S/Tools/Net" "$S/Tools/Net.info" "$S/Tools/bsdtest" "$S/Tools/ugtest" \
       "$S/PrismNet.guide" "$S/PrismNet.guide.info"
cp dist-src/icons/Disk.info "$S/Disk.info"

rm -f "$ADF"
"$X" "$ADF" create + format PrismNet ffs >/dev/null
cd "$S"
for d in $(find . -mindepth 1 -type d | sort); do
    "$X" "$ADF" makedir "${d#./}" >/dev/null
done
for f in $(find . -type f | sort); do
    if ! "$X" "$ADF" write "$f" "${f#./}" >/dev/null 2>&1; then
        echo "mkadf.sh: $f does not fit on the floppy" >&2
        rm -f "$ADF"
        exit 1
    fi
done
cd - >/dev/null
rm -rf "$S"
"$X" "$ADF" info | sed -n 's/^\(used\|free\):/  &/p'
echo "mkadf.sh: $ADF"
