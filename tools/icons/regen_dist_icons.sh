#!/bin/sh
# Regenerate the release icons into dist-src/icons/ (and the two GUI icons in
# icons/). Run by hand when an icon design changes, look at the result, then
# COMMIT the .info files: dist.sh only copies committed icons.
set -e
cd "$(dirname "$0")/../.."
T=/c/projects/tools
I=dist-src/icons
mkdir -p $I

python tools/icons/makeicon_prismnet_glow.py icons >/dev/null             # PrismNetPrefs.info, PrismMon.info
python $T/makeicon_install.py --tool Installer $I/Install_PrismNet.info
python $T/makeicon_doc.py --tool SYS:Utilities/MultiView $I/README.info
python $T/makeicon_doc.py --tool SYS:Utilities/MultiView $I/PrismNet.guide.info
python $T/makeicon_doc.py --tool C:Mount $I/PNFS0.info
python $T/makeicon_doc.py --tool C:Mount $I/PSMB0.info
python tools/icons/make_prism_drawers.py net $I/PrismNet.info             # the archive's own drawer
python tools/icons/makeicon_wbdrawer.py $I/drawer.info                    # every drawer inside it

