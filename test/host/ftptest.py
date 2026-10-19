"""Exercise PrismNet's ftpd from the PC: python test/host/ftptest.py <amiga-address> [password]
Needs on the Amiga: inetd running with ftp, a user ftptest with that password (default
"ftptest-pw"; test/host/pncrypt.py makes the hash) and home RAM:, a user nopass with an
empty password, and ANONYMOUS=RAM:ftpanon holding readme.txt "hello from the A4000"
(see docs/SERVERS.md)."""
import ftplib, io, os, sys, time, hashlib

host = sys.argv[1]
pw = sys.argv[2] if len(sys.argv) > 2 else "ftptest-pw"
fails = 0
def check(ok, what):
    global fails
    print(f"  {what:<55} {'ok' if ok else 'FAIL'}")
    if not ok: fails += 1

def expect_error(fn, *a):
    try:
        fn(*a); return False
    except ftplib.error_perm:
        return True

print("[login]")
f = ftplib.FTP(host, timeout=30)
print("  banner:", f.getwelcome())
check(expect_error(f.login, "nosuchuser", "x"), "unknown user refused (530)")
f = ftplib.FTP(host, timeout=30)
check(expect_error(f.login, "ftptest", "wrong"), "wrong password refused (530)")
f = ftplib.FTP(host, timeout=30)
check(expect_error(f.login, "nopass", "anything"), "account without a password refused")
f = ftplib.FTP(host, timeout=30)
f.login("ftptest", pw)
check(True, "ftptest logs in")
print("  SYST:", f.sendcmd("SYST"), "| PWD:", f.pwd())
check(f.pwd() == "/RAM Disk" or f.pwd().lower().startswith("/ram"), "starts in its home (RAM:)")

print("[volume list]")
f.cwd("/")
vols = f.nlst()
print("  volumes:", vols)
check(any(v.lower().startswith("ram") for v in vols), "/ lists the volumes")
lines = []
f.retrlines("LIST", lines.append)
check(all(l.startswith("d") for l in lines), "LIST / shows directories")

print("[upload / download]")
home = [v for v in vols if v.lower().startswith("ram")][0]
f.cwd("/" + home)
data = os.urandom(1024 * 1024 + 123)
t = time.time(); f.storbinary("STOR up.bin", io.BytesIO(data)); up = time.time() - t
check(f.size("up.bin") == len(data), f"STOR 1 MB, SIZE matches ({len(data)/up/1024:.0f} KB/s up)")
buf = io.BytesIO(); t = time.time(); f.retrbinary("RETR up.bin", buf.write); down = time.time() - t
check(buf.getvalue() == data, f"RETR identical ({len(data)/down/1024:.0f} KB/s down)")
buf = io.BytesIO(); f.retrbinary("RETR up.bin", buf.write, rest=1000000)
check(buf.getvalue() == data[1000000:], "REST 1000000 resumes the download")
f.storbinary("APPE up.bin", io.BytesIO(b"tail"))
check(f.size("up.bin") == len(data) + 4, "APPE appends")
mdtm = f.sendcmd("MDTM up.bin")
check(mdtm.startswith("213 20"), f"MDTM ({mdtm})")

print("[ASCII mode]")
f.storlines("STOR text.txt", io.BytesIO(b"line one\r\nline two\r\n"))
check(f.size("text.txt") == len(b"line one\nline two\n"), "STOR in ASCII stores Amiga LF line ends")
got = []; f.retrlines("RETR text.txt", got.append)
check(got == ["line one", "line two"], "RETR in ASCII gives the lines back")

print("[files and directories]")
f.mkd("ftpdir")
check("ftpdir" in f.nlst(), "MKD")
f.rename("text.txt", "ftpdir/moved.txt")
check("moved.txt" in f.nlst("ftpdir"), "RNFR/RNTO into a directory")
f.cwd("ftpdir"); check(f.pwd().endswith("/ftpdir"), "CWD"); f.cwd("..");
f.delete("ftpdir/moved.txt"); f.rmd("ftpdir")
check("ftpdir" not in f.nlst(), "DELE + RMD")
f.delete("up.bin")
check("up.bin" not in f.nlst(), "DELE up.bin")
check(expect_error(f.delete, "nosuchfile"), "deleting a missing file: 550")

print("[active mode]")
f.set_pasv(False)
lines = []
try:
    f.retrlines("LIST", lines.append); check(True, "PORT + LIST")
except Exception as e:
    check(False, f"PORT + LIST ({e})")
f.set_pasv(True)
f.quit()

print("[anonymous]")
a = ftplib.FTP(host, timeout=30)
a.login("anonymous", "me@example.com")
check(a.pwd() == "/", "anonymous starts at its own /")
check(a.nlst() == ["readme.txt"], "sees only RAM:ftpanon")
buf = io.BytesIO(); a.retrbinary("RETR readme.txt", buf.write)
check(b"hello from the A4000" in buf.getvalue(), "can download")
check(expect_error(a.storbinary, "STOR x", io.BytesIO(b"x")), "upload refused (read-only)")
check(expect_error(a.delete, "readme.txt"), "delete refused")
a.cwd(".."); check(a.pwd() == "/", "can't climb out of its root")
check(expect_error(a.retrbinary, "RETR /ENVARC:PrismNet/passwd", io.BytesIO().write), "no way out by a volume name (RETR /ENVARC:...)")
check(expect_error(a.cwd, "/SYS:"), "no way out by a volume name (CWD /SYS:)")
check(expect_error(a.retrbinary, "RETR /:S/Startup-Sequence", io.BytesIO().write), "no way out by ':' (RETR /:S/...)")
print("[same connection: anonymous, then a user]")
a.sendcmd("USER ftptest")
a.sendcmd("PASS " + pw)
a.storbinary("STOR /RAM Disk/rw.bin" if False else "STOR rwtest.bin", io.BytesIO(b"rw"))
check(True, "a real login after anonymous can write")
a.delete("rwtest.bin")
a.quit()

print("[failed transfers and PORT]")
f = ftplib.FTP(host, timeout=30)
f.login("ftptest", pw)
f.storbinary("STOR keep.txt", io.BytesIO(b"keep me"))
try:
    r = f.sendcmd("STOR keep.txt")          # no PASV/PORT before it
    f.getresp()
except ftplib.Error as e:
    r = str(e)
check(f.size("keep.txt") == 7, "STOR without a data connection leaves the file alone")
check(expect_error(f.sendcmd, "PORT 10,9,8,7,4,1"), "PORT to another address refused")
f.delete("keep.txt")
f.quit()
print(f"ftptest: {fails} failed -> {'FAIL' if fails else 'PASS'}")
