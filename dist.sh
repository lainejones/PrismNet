#!/bin/sh
# Assemble the PrismNet release drawer in dist/PrismNet (run after "make").
# Layout mirrors SYS: (C, Libs, Prefs, Tools, Storage, Docs). The .lha is built
# on an Amiga from this drawer (real protection bits); dist.sh makes the .zip.
set -e
cd "$(dirname "$0")"
VER=0.6
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
            "out/inetd:tools/srv/inetd.c" "out/ftpd:tools/srv/ftpd.c" "out/telnetd:tools/srv/telnetd.c" \
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

rm -rf dist
mkdir -p $D/C $D/L $D/Libs $D/Prefs $D/Tools/Net $D/Tools/Small $D/Storage/PrismNet $D/Storage/DOSDrivers $D/Docs

cp dist-src/Install_PrismNet dist-src/README $D/
cp LICENSE                               $D/Docs/
cp lwip/COPYING                          $D/Docs/lwIP-COPYING
cp out/PrismNet out/ping out/traceroute out/nslookup $D/C/
cp out/bsdsocket.library out/usergroup.library $D/Libs/
cp out/prismnfs-handler                  $D/L/            # optional NFS client
cp out/prismsmb-handler                  $D/L/            # optional SMB client
cp out/ShareUnmount out/SMBShares        $D/C/
cp out/inetd out/ftpd out/telnetd        $D/C/            # optional servers
cp dist-src/PNFS0 dist-src/PSMB0         $D/Storage/DOSDrivers/
PN_GUIDEVER="$VER ($(date +%d.%m.%Y))" python tools/mkguide.py docs/manual $D/PrismNet.guide >/dev/null   # the manual
cp out/PrismNetPrefs                     $D/Prefs/PrismNet
cp icons/PrismNetPrefs.info              $D/Prefs/PrismNet.info
cp out/PrismMon icons/PrismMon.info      $D/Tools/
cp out/bsdtest out/ugtest                $D/Tools/
cp out/small/PrismNet                    $D/Tools/Small/  # the core for 1-2 MB machines
cp out/ping out/traceroute out/nslookup out/httpbench $D/Tools/Net/
cp tools/bench/benchserver.py tools/bench/namequery.py $D/Tools/Net/
cp -r config/ENVARC-PrismNet/.           $D/Storage/PrismNet/

# Icons: committed .info files only (tools/icons/regen_dist_icons.sh makes them)
I=dist-src/icons
cp $I/Install_PrismNet.info $I/README.info $I/PrismNet.guide.info $D/
cp $I/PNFS0.info $I/PSMB0.info           $D/Storage/DOSDrivers/
cp $I/PrismNet.info                      dist/PrismNet.info      # the archive's own drawer
for dr in C L Libs Prefs Tools Storage Docs Storage/PrismNet Storage/DOSDrivers Tools/Net Tools/Small; do
    cp $I/drawer.info $D/$dr.info
done

(cd dist && rm -f PrismNet-$VER.zip && powershell -NoProfile -Command \
  "Compress-Archive -Path PrismNet,PrismNet.info -DestinationPath PrismNet-$VER.zip" )
find dist -type f | sort
