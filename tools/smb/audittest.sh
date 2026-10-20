#!/bin/sh
# PrismNet SMB client: the 2026-10-18 audit findings, from the PC (WSL).
#   audittest.sh [host/share] [user]
# Asks for the password once (not echoed, memory only), then, signing forced
# and encryption off (so the signatures are what is tested):
#  1. are error replies on a signed session signed? (signlog lines)
#  2. does setting read-only keep a file's other attributes (HIDDEN, ARCHIVE)?
# Everything it creates is in a folder pn-attr on the share and is deleted.
cd "$(dirname "$0")/../.." || exit 1
target=${1:-192.168.50.2/amigatest}
user=${2:-smbuser}
make smbtool >/dev/null || exit 1
printf 'Password for %s on %s: ' "$user" "$target"
stty -echo; read -r pass; stty echo; echo
T() { SMBUSER="$user" SMBPASS="$pass" SMBSIGN=1 SMBENC=NO build/smbtool "$target" "$@"; }
host=${target%%/*}
echo "== 1. signing: a missing file, a missing folder, the share list"
T attrs 'pn-no-such-file' 2>&1 | grep -E "signlog|attrs"
T ls 'pn-no-such-folder' 2>&1 | grep -E "signlog|ls:" | tail -4
SMBUSER="$user" SMBPASS="$pass" SMBSIGN=1 SMBENC=NO build/smbtool "$host/-" shares 2>&1 | grep -E "signlog" | sort | uniq -c
echo "== 2. attributes"
printf 'audit\n' > /tmp/pn-attr.txt
T mkdir pn-attr 2>&1 | grep -v signlog
T put /tmp/pn-attr.txt 'pn-attr\f.txt' 2>&1 | grep -v signlog
T attrs 'pn-attr\f.txt' 2>/dev/null
T setattrs 'pn-attr\f.txt' 22 2>&1 | grep -v signlog
echo "-- after HIDDEN + ARCHIVE (22 expected):"
T attrs 'pn-attr\f.txt' 2>/dev/null
T readonly 'pn-attr\f.txt' 2>&1 | grep -v signlog
echo "-- after Protect -w (23 = kept, 01 = HIDDEN and ARCHIVE wiped):"
T attrs 'pn-attr\f.txt' 2>/dev/null
T writable 'pn-attr\f.txt' 2>&1 | grep -v signlog
echo "-- after Protect +w (22 = kept, 80 = wiped):"
T attrs 'pn-attr\f.txt' 2>/dev/null
T setattrs 'pn-attr\f.txt' 80 >/dev/null 2>&1
T rm 'pn-attr\f.txt' 2>&1 | grep -v signlog
T rmdir pn-attr 2>&1 | grep -v signlog
rm -f /tmp/pn-attr.txt
pass=
echo "== done"
