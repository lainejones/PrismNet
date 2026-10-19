#!/bin/sh
# PrismNet SMB client: read test against a Windows share (signing required).
#   wintest.sh [host/share] [user] [file]
# Asks for the password once (not echoed, memory only); lists the share and
# reads one file, printing its MD5 to compare with the PC's own.
cd "$(dirname "$0")/../.." || exit 1
target=${1:-192.168.1.20/amiga}
user=${2:-amiga}
file=${3:-PrismNet-test\\PrismNet\\nfstest}
make smbtool >/dev/null || exit 1
printf 'Password for %s on %s: ' "$user" "$target"
stty -echo; read -r pass; stty echo; echo
T() { SMBUSER="$user" SMBPASS="$pass" build/smbtool "$target" "$@"; }
echo "== list"
T ls | head -15
echo "== read $file"
T cat "$file" | md5sum
pass=
echo "== done"
