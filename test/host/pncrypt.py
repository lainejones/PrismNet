"""PrismNet's crypt() (lib/usergroup.c) in Python, for test accounts:
python test/host/pncrypt.py <password> [salt]  ->  the 13-character hash for ENV:PrismNet/passwd"""
import sys

B64 = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
M = (1 << 64) - 1


def pncrypt(key, salt="ab"):
    s0, s1 = (salt + "..")[0], (salt + "..")[1]
    h = 0xcbf29ce484222325
    for _ in range(64):
        for c in (s0 + s1).encode("latin-1") + key.encode("latin-1"):
            h = ((h ^ c) * 0x100000001b3) & M
    out = s0 + s1
    for _ in range(11):
        out += B64[h & 63]
        h >>= 6
    return out


if __name__ == "__main__":
    print(pncrypt(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "ab"))
