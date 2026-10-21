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
# its classic image: the base drawer (OS 3.2's def_drawer.info) has only an 8x8
# placeholder there, a dot on an OS 3.0/3.1 Workbench - give it a real one
python tools/icons/classicimg.py $I/PrismNet.info $I/PrismNet.info     OVERRIDES=7:3,1:0,10:0,16:1,18:2,19:2,20:3,29:3,21:3,22:1
# the install floppy's icon: the same, as a disk (do_Type 1)
python -c "import sys; d=bytearray(open(sys.argv[1],'rb').read()); d[0x30]=1; open(sys.argv[2],'wb').write(d)"     $I/PrismNet.info $I/Disk.info
python tools/icons/makeicon_wbdrawer.py $I/drawer.info                    # every drawer inside it

