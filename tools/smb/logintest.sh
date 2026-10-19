#!/bin/sh
# PrismNet SMB client: authenticated login test from the PC (WSL).
#   logintest.sh [host/share] [user] [domain]
# Asks for the password (not echoed, kept only in this shell's memory), then
# logs in without a domain and with the given one (default: WORKGROUP), signing as negotiated and forced, and reads a file.
cd "$(dirname "$0")/../.." || exit 1
target=${1:-192.168.1.10/share}
user=${2:-smbuser}
dom=${3:-WORKGROUP}
[ -x build/smbtool ] || make smbtool >/dev/null || exit 1
printf 'Password for %s on %s: ' "$user" "$target"
stty -echo; read -r pass; stty echo; echo
for d in "" "$dom"; do
    echo "== domain '${d}', signing as negotiated"
    SMBUSER="$user" SMBPASS="$pass" SMBDOMAIN="$d" build/smbtool "$target" ls
    echo "== domain '${d}', signing forced"
    SMBUSER="$user" SMBPASS="$pass" SMBDOMAIN="$d" SMBSIGN=1 build/smbtool "$target" ls
done
echo "== signed read of a file"
SMBUSER="$user" SMBPASS="$pass" SMBSIGN=1 build/smbtool "$target" cat smb2fs.m68k-amigaos.lha | md5sum
pass=
echo "== done"
