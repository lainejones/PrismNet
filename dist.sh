#!/bin/sh
# Assemble the PrismNet release drawer in dist/PrismNet (run after "make").
# Layout mirrors SYS: (C, Libs, Prefs, Tools, Storage, Docs). The .lha is built
# on an Amiga from this drawer (real protection bits).
set -e
cd "$(dirname "$0")"
VER=1.0.7
D=dist/PrismNet

# Refuse to package binaries older than their sources (a forgotten "make").
stale=""
for pair in "out/PrismNet:core port lwip/src" "out/small/PrismNet:core port lwip/src" \
            "out/bsdsocket.library:lib/bsdsocket.c lib/lib_start.S lib/support.S port/include/prismnet/pnmsg.h" \
            "out/usergroup.library:lib/usergroup.c lib/ug_start.S lib/support.S" \
            "out/PrismNetPrefs:tools/prismprefs.c tools/prefsshares_io.c tools/prefsshares_gui.c tools/prefsservices_io.c tools/prefsservices_gui.c tools/tabs.c" \
            "out/PrismMon:tools/prismmon.c" "out/ping:tools/net" "out/nslookup:tools/net" \
            "out/prismnfs-handler:tools/nfs tools/netfs" "out/ShareUnmount:tools/netfs/shareunmount.c" \
            "out/prismsmb-handler:tools/smb/smb_start.S tools/netfs/core.c tools/netfs/names.c tools/netfs/net_amiga.c tools/netfs/nbname.c tools/smb/backend.c tools/smb/smb2.c tools/smb/crypto.c tools/smb/passreq.c tools/netfs/*.h tools/smb/*.h" \
            "out/inetd:tools/srv/inetd.c" "out/ftpd:tools/srv/ftpd.c" "out/telnetd:tools/srv/telnetd.c" "out/httpd:tools/srv/httpd.c"\
            "out/PrismNetUpdate:tools/update" \
            "out/SMBShares:tools/smb/smbshares.c tools/smb/smb2.c tools/smb/crypto.c tools/netfs/net_amiga.c tools/netfs/nbname.c tools/netfs/*.h tools/smb/*.h"; do
    bin=${pair%%:*}; srcs=${pair#*:}
    if [ ! -f "$bin" ] || [ -n "$(find $srcs -newer "$bin" \( -name '*.c' -o -name '*.h' -o -name '*.S' \) 2>/dev/null | head -1)" ]; then
        stale="$stale $bin"
    fi
done
if [ -n "$stale" ]; then
    echo "dist.sh: out of date, run make first:$stale" >&2
    exit 1
fi

# everything but dist/aminet: the upload pair there may have been edited by
# hand (the uploader's address) and must survive a rebuild
rm -rf dist/PrismNet dist/PrismNet.info dist/PrismNet.readme
mkdir -p $D/C $D/L $D/Libs $D/Prefs $D/Tools/Net $D/Tools/Small $D/Storage/PrismNet $D/Storage/DOSDrivers $D/Docs

# the installer says which version it installs
sed "s/@VER@/$VER/" dist-src/Install_PrismNet > $D/Install_PrismNet
# the Aminet readme goes next to the archive (dist/PrismNet.readme), not in it
sed "s/@VER@/$VER/" dist-src/aminet.readme > dist/PrismNet.readme
cp dist-src/README $D/
# the update tool must name the same version (tools/update/upd_cfg.h)
grep -q "PrismNetUpdate $VER " out/PrismNetUpdate || { echo "dist.sh: out/PrismNetUpdate is not $VER" >&2; exit 1; }
cp LICENSE                               $D/Docs/
cp lwip/COPYING                          $D/Docs/lwIP-COPYING
cp out/PrismNet out/ping out/traceroute out/nslookup $D/C/
cp out/bsdsocket.library out/usergroup.library $D/Libs/
cp out/prismnfs-handler                  $D/L/            # optional NFS client
cp out/prismsmb-handler                  $D/L/            # optional SMB client
cp out/ShareUnmount out/SMBShares        $D/C/
cp out/inetd out/ftpd out/telnetd out/httpd $D/C/            # optional servers
cp out/PrismNetUpdate                    $D/C/            # the update check (68020+)
cp dist-src/PNFS0 dist-src/PSMB0         $D/Storage/DOSDrivers/
PN_GUIDEVER="$VER ($(date +%d.%m.%Y))" python tools/mkguide.py docs/manual $D/PrismNet.guide >/dev/null   # the manual
cp out/PrismNetPrefs                     $D/Prefs/PrismNet
cp icons/PrismNetPrefs.info              $D/Prefs/PrismNet.info
cp out/PrismMon icons/PrismMon.info      $D/Tools/
cp out/bsdtest out/ugtest                $D/Tools/
cp dist-src/WHDLoadHooks.rexx            $D/Tools/        # WHDLoad hooks (installer + by hand)
cp out/small/PrismNet                    $D/Tools/Small/  # the core for 1-2 MB machines
cp out/ping out/traceroute out/nslookup out/httpbench $D/Tools/Net/
cp tools/bench/benchserver.py tools/bench/namequery.py $D/Tools/Net/
cp -r config/ENVARC-PrismNet/.           $D/Storage/PrismNet/

# Icons: the committed .info files in dist-src/icons and icons/, copied as they are
I=dist-src/icons
cp $I/Install_PrismNet.info $I/README.info $I/PrismNet.guide.info $D/
cp $I/PNFS0.info $I/PSMB0.info           $D/Storage/DOSDrivers/
cp $I/PrismNet.info                      dist/PrismNet.info      # the archive's own drawer
# the top-level drawers: one icon each, each with its place in the window
for dr in C L Libs Prefs Tools Storage Docs; do
    cp $I/$dr.info $D/$dr.info
done
for dr in Storage/PrismNet Storage/DOSDrivers Tools/Net Tools/Small; do
    cp $I/drawer.info $D/$dr.info
done

rm -f dist/PrismNet-*.zip                 # (no zip: it cannot keep the protection bits)
find dist -type f | sort
