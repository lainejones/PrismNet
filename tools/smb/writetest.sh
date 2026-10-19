#!/bin/sh
# PrismNet SMB client: write test from the PC (WSL), signing forced.
#   writetest.sh [host/share] [user]
# Asks for the password once (not echoed, memory only), then in a folder
# pn-smb on the share: write 1 MB, read it back, rename, truncate, set the
# date and the read-only bit, delete everything. Prints MD5s to compare.
cd "$(dirname "$0")/../.." || exit 1
target=${1:-192.168.50.2/amigatest}
user=${2:-smbuser}
make smbtool >/dev/null || exit 1
printf 'Password for %s on %s: ' "$user" "$target"
stty -echo; read -r pass; stty echo; echo
T() { SMBUSER="$user" SMBPASS="$pass" SMBSIGN=1 build/smbtool "$target" "$@"; }
head -c 1048576 /dev/urandom > /tmp/pn-smb-1m.bin
echo "== local MD5:  $(md5sum < /tmp/pn-smb-1m.bin)"
T mkdir pn-smb
T put /tmp/pn-smb-1m.bin 'pn-smb\big.bin'
echo "== read back:  $(T cat 'pn-smb\big.bin' 2>/dev/null | md5sum)"
T mv 'pn-smb\big.bin' 'pn-smb\Big2.bin'
T touch 'pn-smb\Big2.bin' 1735732800
T readonly 'pn-smb\Big2.bin'
T ls pn-smb
T writable 'pn-smb\Big2.bin'
T truncate 'pn-smb\Big2.bin' 1000
T ls pn-smb
T rm 'pn-smb\Big2.bin'
T rmdir pn-smb
T ls 2>/dev/null | grep -c pn-smb | sed 's/^/pn-smb left: /'
rm -f /tmp/pn-smb-1m.bin
pass=
echo "== done"
