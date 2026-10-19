#!/bin/sh
# PrismNet SMB client: dialect, signing and encryption matrix (stage 3).
#   smb3test.sh host/share user file [write]
# Asks for the password once (not echoed, memory only). For each dialect
# 2.1, 3.0, 3.0.2 and 3.1.1: connect signed and read <file> (MD5); then
# encrypted at 3.0.2 (AES-CCM) and 3.1.1 (AES-GCM); the share list. With "write": a 1 MB
# encrypted put + read back in folder pn-smb, removed afterwards.
cd "$(dirname "$0")/../.." || exit 1
target=$1; user=$2; file=$3; write=$4
[ -n "$file" ] || { echo "usage: smb3test.sh host/share user file [write]"; exit 1; }
make smbtool >/dev/null || exit 1
printf 'Password for %s on %s: ' "$user" "$target"
stty -echo; read -r pass; stty echo; echo
T() { SMBUSER="$user" SMBPASS="$pass" build/smbtool "$target" "$@"; }
for p in 210 300 302 311; do
    printf '== %s signed:    ' $p
    SMBPROTO=$p SMBSIGN=1 T cat "$file" 2>/tmp/pn-smb3.err | md5sum | cut -c1-32
    head -1 /tmp/pn-smb3.err | sed 's/^/     /'
done
for p in 302 311; do
    printf '== %s encrypted: ' $p
    SMBPROTO=$p SMBENC=YES T cat "$file" 2>/tmp/pn-smb3.err | md5sum | cut -c1-32
    head -1 /tmp/pn-smb3.err | sed 's/^/     /'
    grep bytes /tmp/pn-smb3.err | head -1 | sed 's/^/     /'
done
echo "== share list (3.1.1):"
T shares 2>&1 | sed 's/^/     /'
if [ "$write" = write ]; then
    head -c 1048576 /dev/urandom > /tmp/pn-smb3-1m.bin
    echo "== local MD5:  $(md5sum < /tmp/pn-smb3-1m.bin | cut -c1-32)"
    for p in 302 311; do
        SMBPROTO=$p SMBENC=YES T mkdir pn-smb 2>/dev/null
        SMBPROTO=$p SMBENC=YES T put /tmp/pn-smb3-1m.bin 'pn-smb\enc.bin' 2>&1 | tail -1
        echo "== $p read back: $(SMBPROTO=$p SMBENC=YES T cat 'pn-smb\enc.bin' 2>/dev/null | md5sum | cut -c1-32)"
        SMBPROTO=$p SMBENC=YES T rm 'pn-smb\enc.bin' 2>&1
        SMBPROTO=$p SMBENC=YES T rmdir pn-smb 2>&1
    done
    rm -f /tmp/pn-smb3-1m.bin
fi
rm -f /tmp/pn-smb3.err
pass=
echo "== done"
