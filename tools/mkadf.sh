#!/bin/sh
# mkadf.sh - the install floppy: dist/PrismNet-<VER>.adf, an 880 KB (DD) FFS
# disk named PrismNet. Since 1.0.9 the whole install drawer is on it packed
# with PrismPack (C:\projects\PrismPack, github.com/lainejones/PrismPack):
# PrismNet.ppk + PrismUnpack + Install_PrismNet (unpacks to a drawer on the
# hard disk, runs the installer there, deletes it) + its icon, so the floppy
# carries exactly what the .lha does, manual and servers included. Run in WSL
# after dist.sh; needs python3, amitools' xdftool (~/amitools-venv) and a
# built PrismPack (its out/PrismUnpack).
#
# GPL-3.0-only, see LICENSE.
set -e
cd "$(dirname "$0")/.."
X=${XDFTOOL:-$HOME/amitools-venv/bin/xdftool}
PP=${PRISMPACK:-/mnt/c/projects/PrismPack}
VER=$(sed -n 's/^VER=//p' dist.sh)
D=dist/PrismNet
ADF=$(pwd)/dist/PrismNet-$VER.adf
[ -d "$D" ] || { echo "mkadf.sh: no $D - run dist.sh first" >&2; exit 1; }
[ -x "$PP/out/PrismUnpack" ] || { echo "mkadf.sh: no $PP/out/PrismUnpack - build PrismPack first" >&2; exit 1; }

S=$(mktemp -d)
python3 "$PP/host/pack.py" floppy "$D" "$S" --info dist/PrismNet.info \
    --icon dist-src/icons/Install_PrismNet.info --unpacker "$PP/out/PrismUnpack"
cp dist-src/README dist-src/icons/README.info "$S/"
cp dist-src/icons/Disk.info "$S/Disk.info"

rm -f "$ADF"
"$X" "$ADF" create + format PrismNet ffs >/dev/null
cd "$S"
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
