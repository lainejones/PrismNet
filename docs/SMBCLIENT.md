# PrismNet SMB client: design and scope

Audit 2026-10-18 (docs/AUDIT.md): reply parsing hardened (READ/QUERY_DIRECTORY/
FsFullSize/negotiate/share-list bounds, NTLM TargetInfo), `ENCRYPT=YES` refuses
unencrypted sessions, listing slots survive deep tree walks; `make smbunit`
runs the parser checks under AddressSanitizer.

Status: **stages 0-2 done; stage 3 (SMB 3.x) working** - 2.0.2 to 3.1.1, signed, encrypted when a share demands it, share list, NetBIOS names (2026-10-18).
Design written the same day.

**Stage 1 so far (guest, read-only).** `L:prismsmb-handler` 0.1, 38.6 KB:
the shared core (built with `FS_FHSIZE=260`: a handle holds the share path)
+ `tools/smb/backend.c` + `smb2.c` (SMB 2.0.2/2.1, NTLMv2 in SPNEGO,
anonymous/guest, HMAC-SHA256 signing, compound CREATE+CLOSE and
CREATE+QUERY_INFO+CLOSE) + `crypto.c` (MD4, MD5, SHA-256, HMAC: all RFC/FIPS
vectors and the MS-NLMP NTLMv2 example pass, `make cryptotest`).
`make smbtool` runs the same SMB code on the PC.

| Check (Drobo `public`, guest, dialect 2.1) | Result |
|---|---|
| smbtool from WSL: info, ls, cat 512 KB ROM image | MD5 `2359a890...` = Windows' own SMB client |
| Synology as guest | refused cleanly: NTSTATUS c000006d -> "not a DOS disk" path |
| Amiga ("PrismNet test"): Lock/Examine/ExNext, ParentDir, Open/Read/Seek | correct; byte sum 42713520 |
| Amiga: `Copy` the 512 KB file | ~1 s, MD5 identical |
| `ShareUnmount PS0:` | unmounted |
| smb2fs 53.9 as baseline | its `DROBO` mount now fails (TREE_CONNECT access denied with its stored settings), so no side-by-side yet |

**Large files** (2026-10-17, Amiga `Copy` to the host drive, MD5 on the PC):

| File (Drobo `public`, guest) | Result |
|---|---|
| Publisher 2016 ISO, 531,355,648 bytes | 15 min 24 s (~575 KB/s), **MD5 identical** to Windows' SMB client |
| Windows Server 2016 ISO, **6,006,587,392 bytes** | first 2.1 GB checked at 11 offsets: identical. (smbtool had shown 1.71 GB: it printed only the low 32 bits - fixed.) AmigaDOS positions are 32-bit, so the core now ends such a file at 4 GB instead of wrapping to 0 |

**The 15 s stalls: found and fixed (in PrismNet, not the SMB client).** Every
long run had a read that got no reply for 15 s; the first run did not recover
in time. Logging `PrismNet STATUS` every 10 s showed "tx dropped" climbing
all through a transfer (~2% of sends): WinUAE's emulated NE2000
(`cnet.device`) completes writes slowly, all 16 write requests were often
busy, and `sanaif_linkoutput` then dropped the frame - mostly TCP ACKs. When
the ACKs for the end of a window were lost, the server waited out its
retransmit timer (backing off), and the reads stalled. Fixes:

- `port/sanaif.c`: frames are **queued** (32, 8 in the small build) while
  every write is busy and sent as writes complete; dropped only if that
  queue is full too. This helps every protocol, not just SMB.
- The device watchdog's back-off is capped at 8 s (was 64 s): a card that
  really stops answering is reset sooner.
- The network-filesystem core is four times as patient (`RETRY` x 4) while
  files are open, so a card reset mid-copy does not end the copy.
- NBSS keepalive frames (type 0x85, Samba) are skipped, not treated as errors.

After the fix the 849 MB `af-dvd.iso` copied in 25 min 26 s (~556 KB/s) with
**0 dropped sends, no stall, no reconnect**, MD5 `54133e78...` identical to
Windows' SMB client (before: ~5,000 dropped sends and a 15 s stall).

**Authenticated and signed** (2026-10-17, Synology, account `smbuser`, the
password typed by Laine each time; `tools/smb/logintest.sh` on the PC,
`test/amiga/t-smbauth` + `test/amiga/smb/PS1` on the Amiga):

| Check | Result |
|---|---|
| NTLMv2 against MS-NLMP 4.2.4: NT hash, NTOWFv2, NTProofStr, SessionBaseKey | all match (`make cryptotest`) |
| smbtool, user session, no domain and domain `nuzzie.ca` | both accepted |
| smbtool with signing forced: listing, 304825-byte read | server accepted every signed request, every signed reply verified, MD5 `131156c5...` |
| Amiga, `USER=smbuser SIGN=YES`: password window on first access | opened, masked input, login worked |
| Amiga: List, Examine/ExNext, Open/Read/Seek, `Copy` | correct; copy MD5 `131156c5...` |

(A first try failed with LOGON_FAILURE; the NAS showed no such local user
at the time. With the account in place it worked.)

**Signing speed** (`tools/smb/hashbench`, HMAC-SHA256 over 16 KB messages,
cycle-exact WinUAE at ~28 MHz):

| Machine | HMAC-SHA256 | MD5 |
|---|---|---|
| 68030, fast RAM ("PrismNet test") | ~136 KB/s | ~220 KB/s |
| 68020, 2 MB chip RAM only ("PrismNet a500") | ~60 KB/s | ~85 KB/s |

So a signed session caps reads near 136 KB/s on that 68030 (unsigned:
~575 KB/s), which is why `SIGN=AUTO` signs only when the server requires it
(Windows does); a 7 MHz 68000 would manage roughly 10-20 KB/s signed.
An assembly SHA-256 remains a stage-3 option.

**2 MB machine** ("PrismNet a500": 68020, 2 MB chip, no fast RAM, ~190 KB
free after Workbench and PrismNet): guest read of the 512 KB ROM image from
the Drobo, ~100 KB/s, MD5 identical, twice in a row. The first attempt hung
with ~85 KB free; the handler now sizes its buffers to the machine (send
buffer 2 KB while read-only, receive buffer RSIZE + 1 KB, listing buffers on
first use instead of two 8 KB ones up front).

**Windows share** (this PC, `amiga` as `laine`, signing required): listing
and a signed read, MD5 identical to the PC's own. That closes stage 1.

**Installer (2026-10-18).** The release drawer carries `L/prismsmb-handler`,
`C/SMBShares` and a commented mount file `Storage/DOSDrivers/PSMB0` (+ icon,
default tool `C:Mount`; `dist.sh` refuses stale binaries for both). The
installer's "SMB client" option copies the handler, SMBShares, ShareUnmount
(now installed for either client) and PSMB0 (an existing one is kept). Remove
deletes both handlers, ShareUnmount and SMBShares while they are PrismNet's,
and PNFS0/PSMB0 while they still carry the "PrismNet NFS/SMB client" text.
Clicked through over NetHarness on "PrismNet install": SMB only installed all
four files (handler identical to `out/`) and the closing message names it; a
full Remove from the same machine took every file off again.

**Stage 3 (SMB 3.x and extras).** `L:prismsmb-handler` 0.3, 54 KB.
- **Dialects 3.0, 3.0.2, 3.1.1** offered next to 2.0.2/2.1 (`MAXPROTO=` caps
  them). 3.1.1 sends the pre-auth integrity context (SHA-512 over every
  NEGOTIATE/SESSION_SETUP message) and derives its keys from that hash;
  3.0.x uses the fixed SP800-108 labels. 3.x signs with AES-CMAC; the
  final SESSION_SETUP reply's signature is checked (required at 3.1.1),
  and 3.1.1 always signs TREE_CONNECT.
- **Encryption**: AES-128-GCM (3.1.1) or AES-128-CCM (3.0.x) in a
  TRANSFORM_HEADER. `ENCRYPT=AUTO` (default) offers it only after a share
  has refused us without it - AES on a 68000 is slow, and Samba set to
  "desired" would otherwise encrypt everything. `ENCRYPT=YES` always,
  `ENCRYPT=NO` never. Plain-text replies on an encrypted session are dropped.
- **Crypto** (`crypto.c`): SHA-512, AES-128 (forward only; S-box and table
  computed on first use, 1.25 KB BSS), CMAC, CCM, GCM (4-bit tables), KDF.
  29 vectors in `make cryptotest`: FIPS 180-4/197, RFC 4493, the GCM
  spec, and odd lengths checked against Python's `cryptography`.
- **Share list**: SRVSVC NetrShareEnum (level 1) over the `srvsvc` pipe on
  IPC$ (`smb2_shares`). `C:SMBShares HOST [USER=] [ALL]` lists them on the
  Amiga (the password is asked in the shell, not echoed); `smbtool host/-
  shares` on the PC. The Shares tab's *Browse...* will use the same call.
- **NetBIOS names**: when DNS does not know a bare name of up to 15
  characters, a name query is broadcast on UDP 137 (`tools/netfs/nbname.c`,
  in both handlers). The Drobo and the Synology answer; Windows 11 does not
  run NetBIOS, so a PC needs its DNS name.
- Found on the way: request bodies were not zeroed, so the NEGOTIATE's
  dialect count was left in SESSION_SETUP's Flags byte (3 dialects =
  BINDING, refused) and the client GUID in PreviousSessionId. `unit()` now
  clears the first 64 body bytes. The client GUID is random per connection.

| Server | 2.1 signed | 3.0 | 3.0.2 | 3.1.1 | 3.0.2 CCM | 3.1.1 GCM |
|---|---|---|---|---|---|---|
| Synology (smbuser) | MD5 ok | ok | ok | ok | ok, 1 MB write ok | ok, 1 MB write ok |
| Windows 11 (laine, signing required) | ok | ok | ok | ok | ok | ok |
| Drobo (guest) | ok | ok | ok | ok | - | - |

Share list (3.1.1): Synology as smbuser 8 shares (disks, the printer, homes),
Windows as laine `Amiga` plus ADMIN$/C$/IPC$ (hidden), Drobo as guest
Public and DroboApps.

Amiga ("PrismNet test", 68030): 3.1.1 guest mount of the Drobo, 512 KB ROM
image in about a second, MD5 identical; `SMBShares` lists the Drobo's shares.
`hashbench` per 1 MB in 16 KB messages, built as the handler is:
HMAC-SHA256 128 KB/s, AES-CMAC 89 KB/s, AES-CCM 43 KB/s, AES-GCM 45 KB/s,
MD5 177 KB/s. (The first AES version did 82 / 40 / 25; it now keeps the
block as four 32-bit words across a message, uses four round tables, and
GHASH runs on 32-bit words instead of 64-bit shifts. -O2 made AES no
faster than -Os on a 68030, only SHA-256 gains from it, so just that
function gets -O2.) Signing still costs more at 3.x than at 2.1:
`MAXPROTO=2.1` is the faster choice where 3.1.1's downgrade protection is
not wanted.

**Stage 2 (writes).** `L:prismsmb-handler` 0.2, 42 KB. Read/write unless
`READONLY`. New requests in `smb2.c`: CREATE for writing (OPEN, CREATE,
OVERWRITE_IF), WRITE, FLUSH, and compound CREATE+SET_INFO+CLOSE for rename
(FileRenameInformation, never replacing), delete (FileDispositionInformation),
size (FileEndOfFileInformation), times and the read-only attribute
(FileBasicInformation). The backend reopens a cached read handle for
writing on first write, flushes on COMMIT, and closes any cached opens on a
path before deleting, renaming or recreating it. Send buffer WSIZE + 1 KB
(2 KB while READONLY, so the 2 MB machine keeps its read-only footprint).

- PC (`tools/smb/writetest.sh`, Synology `amigatest` as `smbuser`, signed):
  1 MB put and read back MD5 identical; mv, touch, read-only, writable,
  truncate, rm, rmdir; nothing left behind.
- Amiga (`test/amiga/t-smbwrite`, `PS1:`, password window): MakeDir, 1 MB
  Copy in (~95 KB/s signed at 68030) and back out MD5 identical,
  `Echo >`/`>>`, Rename, Protect, SetDate, Rename onto an existing name
  refused, Delete of a non-empty folder refused, Delete ALL FORCE.
- Found on the way: a delete-on-close CREATE on a non-empty folder made
  Samba report success and keep the folder, so delete now uses
  FileDispositionInformation, which is refused with DIRECTORY_NOT_EMPTY.
- Mod times follow the Workbench locale's GMT offset (as NFS), `TZ=` overrides.

**Stage 0 result.** `tools/nfs/handler.c` is now `tools/netfs/core.c`
(protocol-neutral: packets, locks, ExNext/ExAll, links, write-behind,
IDLE/RETRY, volume) + `tools/nfs/backend.c` (the NFS `Backend_ops`:
portmap/MOUNT, the NFS3 calls, DEPTH pipelines). Shared pieces live in
`tools/netfs/` (`netfs.h` - state and the backend table, `fstypes.h`,
`names.*`, `net*`, `nfstest`, `ShareUnmount`). Same binary, 33 KB. Re-run
on "PrismNet test" against the Synology: read, case-insensitive names,
`List PAT`, soft link, the full write set (1 MB MD5-identical, also at
DEPTH=4), rename refusal, ShareUnmount + restart, STOP stays stopped and the
next access restarts the stack - all as before. Part of the plan in
[SHARES.md](SHARES.md): PrismNet's own network-share clients, the NFS client
first ([NFSCLIENT.md](NFSCLIENT.md), done: stages 1-3), SMB second, both
optional at install. The PrismNetPrefs **Shares tab comes after this
client**, so it can manage NFS and SMB shares together.

## Goal

`L:prismsmb-handler`: an SMB2/SMB3 client filesystem that mounts a Windows,
macOS, Samba or NAS share as an ordinary Amiga volume.

- PrismNet's own code (GPL-3.0), written from scratch from Microsoft's open specifications ([MS-SMB2],
  [MS-NLMP], [MS-SPNG]; Microsoft Open Specification Promise) and the RFCs
  for the crypto. libsmb2 (LGPL, used by smb2fs) is not used or read; smb2fs
  is only the behaviour baseline in tests.
- OS 2.04+, 68000-clean, like the NFS client. No filesysbox.library.
- Same DOSDriver shape and option names as the NFS client where they mean
  the same thing.
- Optional: the installer's "SMB client" checkbox (live since 2026-10-18).

## What the servers here speak

One SMB2 NEGOTIATE (dialects 2.0.2-3.0.2 offered, no session set up) sent
from the PC on 2026-10-17:

| Server | Picks | Signing | Notes |
|---|---|---|---|
| Synology DS1821+, DSM 7.4.1 (192.168.50.2) | 3.0.2 | enabled, not required | `min protocol=SMB2`, `max protocol=SMB3`; large MTU, leasing, encryption offered |
| Drobo (192.168.50.3) | 3.0.2 | enabled, not required | Samba-style capabilities; smb2fs already mounts its `public` share over PrismNet (`test/amiga/t-smb`) |
| This PC, Windows 11 Pro build 26300 | 3.0.2 | **required** | Windows 11 24H2 and later require signing by default |

All three allow 8 MB reads and writes. Consequences: **SMB1 is not needed**
(nothing here offers it), and **signing must be there from the first stage**,
or Windows machines are out of reach.

## Decisions

| Question | Choice | Why |
|---|---|---|
| Dialects | **2.0.2 and 2.1 in stage 1; 3.0, 3.0.2 and 3.1.1 in stage 3** | 2.x signs with HMAC-SHA256 only. 3.x needs AES-CMAC signing and key derivation (SP800-108), and 3.1.1 adds pre-auth integrity (SHA-512) and negotiate contexts. All three servers accept 2.1. |
| Transport | **Direct TCP on port 445** | No NetBIOS session layer (port 139) is needed by any current server. |
| Authentication | **NTLMv2 inside SPNEGO**, plus **guest/anonymous** | It is what every home server takes. Kerberos needs a domain: out of scope. NTLMv1/LM are refused. |
| Signing | **Always when the server enables it; never refused** | Required by Windows. Costs CPU on small machines (see *Speed*). |
| Encryption | **Stage 3, only when a share demands it** | AES-128-CCM/GCM over every byte is too slow for a 68000 to be the default. |
| Oplocks / leases | **Never requested** | Simpler and safe: no break notifications to handle. |
| Code shared with NFS | **Yes: split the handler into a core and protocol backends** (stage 0) | Packet loop, locks and paths, ExAll, soft links, IDLE, RETRY, the private packet port, name mapping and the read cache are protocol-neutral, and have been debugged once already. |

## Stage 0: a shared filesystem core

`tools/nfs/handler.c` becomes `tools/netfs/core.c` plus a backend table:

```c
struct Backend {
    long (*connect)(void), (*lookup)(...), (*getattr)(...), (*readdir)(...),
         (*open)(...), (*read)(...), (*write)(...), (*close)(...),
         (*create)(...), (*remove)(...), (*rename)(...), (*mkdir)(...),
         (*setattr)(...), (*statfs)(...), (*readlink)(...), (*symlink)(...);
};
```

The NFS client becomes `netfs/core.c` + `netfs/nfs.c` (same binary,
`L:prismnfs-handler`). Its stage 1-3 tests must still pass before SMB code is
added. The SMB backend fills the same table with `netfs/smb2.c`, plus
`netfs/ntlm.c` and `netfs/crypto.c`.

## How SMB maps onto AmigaDOS

SMB is **stateful** (every open is a FileId on the server) where NFS is not.

- **Locks stay path-based**: an AmigaDOS lock does not hold a server open,
  so Workbench's many idle locks cost the server nothing. Opens happen only
  for reads, writes and directory listings.
- **Fewer round trips through compounding**: "examine this path" is one
  compound CREATE + QUERY_INFO + CLOSE. A listing is CREATE (directory) +
  QUERY_DIRECTORY (FileIdBothDirectoryInformation, many entries per reply),
  and CLOSE at the end. This matters more than raw speed on a slow Amiga.
- **Files**: FINDINPUT/OUTPUT/UPDATE open the file and keep its FileId in the
  file handle. READ/WRITE use it with sizes up to the negotiated limit (large
  MTU with credits on 2.1+), and END closes it.
- **Credits**: start with what NEGOTIATE grants, ask for 8-16 more in each
  request, and never send without a credit.
- **Names**: UTF-16LE on the wire, converted from and to Latin-1. Servers
  are case-insensitive, which suits AmigaDOS.
- **Times**: FILETIME (100 ns since 1601, UTC) <-> DateStamp with `TZ=`,
  as for NFS.
- **Attributes**:
  - READONLY maps to Amiga W/D cleared.
  - The Amiga A (archive) bit is the inverse of DOS ARCHIVE.
  - HIDDEN maps to the Amiga H bit.
  - There is no Unix mode, and no comments (alternate data streams are out
    of scope).
- **Renames, deletes and sizes** go through SET_INFO
  (FileRenameInformation, FileDispositionInformation,
  FileEndOfFileInformation, FileBasicInformation for times and attributes).
  RENAME never replaces an existing file, as AmigaDOS expects.
- **Links**: Windows symlinks and reparse points are listed as files; stage 3
  may read them.
- **Info**: QUERY_INFO FileFsFullSizeInformation feeds Info and Workbench,
  cached and never fetched by a poll, as in the NFS fix.

**NTSTATUS to AmigaDOS errors**, for example:

| NTSTATUS | AmigaDOS error |
|---|---|
| OBJECT_NAME_NOT_FOUND, OBJECT_PATH_NOT_FOUND | OBJECT_NOT_FOUND / DIR_NOT_FOUND |
| ACCESS_DENIED | READ_PROTECTED, or WRITE_PROTECTED for writes |
| SHARING_VIOLATION | OBJECT_IN_USE |
| OBJECT_NAME_COLLISION | OBJECT_EXISTS |
| DIRECTORY_NOT_EMPTY | DIRECTORY_NOT_EMPTY |
| DISK_FULL | DISK_FULL |
| LOGON_FAILURE, ACCOUNT_RESTRICTION | a requester asking again; then NOT_A_DOS_DISK |

## Crypto, from the specs

| Piece | Used for | Spec |
|---|---|---|
| MD4 | NT password hash | RFC 1320 |
| MD5, HMAC-MD5 | NTLMv2 response, session keys | RFC 1321, RFC 2104 |
| RC4 | NTLM key exchange (when negotiated) | public algorithm |
| SHA-256, HMAC-SHA256 | SMB 2.x signing | FIPS 180-4 |
| AES-128, AES-CMAC, SP800-108 KDF | SMB 3.x signing keys and signing (stage 3) | FIPS 197, RFC 4493 |
| SHA-512 | SMB 3.1.1 pre-auth integrity (stage 3) | FIPS 180-4 |
| AES-CCM/GCM | encryption (stage 3, optional) | NIST SP800-38C/D |

All of it is plain C, about 10-14 KB, with host test vectors from the RFCs in
`make smbtool`.

## Speed on small machines (to be measured in stage 1)

With signing, every message is hashed in full, data included. So **signing
speed caps read and write speed** whatever the network does. Rough
expectation for HMAC-SHA256 in plain C:

| CPU | Expected signing ceiling |
|---|---|
| 68000 7 MHz | ~10-20 KB/s |
| 68020 14 MHz | ~40-70 KB/s |
| 68030 50 MHz | ~150-250 KB/s |
| 68060 / PiStorm | above the stack's own limit |

So: sign only when the server enables it. The Synology and the Drobo enable
but do not require it, and the client can then decline, if [MS-SMB2]
§3.2.4.1.1 allows that for the dialect (checked in stage 1). Windows always
gets signing. A 68000 with Windows shares will be slow but correct. An
assembly SHA-256 for 68000 is a stage-3 option if it matters.

## Credentials

- `USER=` in the DOSDriver names the account; `GUEST` connects without any.
- **Type it once.** The first access opens a small window asking for the
  password, with "Remember this password" ticked. Once the server has
  accepted it, it is saved as a plain file `ENVARC:PrismNet/smb/<host>.<user>`
  (and the `ENV:` copy) and every later mount reads it from there, so the
  window never comes back. A rejected saved password is deleted and the
  window asks again. Untick the box and it stays in memory only until the
  handler exits. An Amiga is one person's machine; there is no reason to
  hide a file from its owner.
- `PASSWORD=secret` in the DOSDriver works too (as in smbfs/smb2fs), for
  scripts and people who edit mountlists anyway.
- Tests use guest shares, or an account Laine sets up, with the password typed
  in on the Amiga by Laine. The assistant never writes a password into a file.

## Names and finding servers

- **Host names**: PrismNet's resolver (DNS and hosts file), as for NFS.
- **NetBIOS names** (`\\DROBO`): a NetBIOS name query, UDP 137 broadcast, as
  a fallback when DNS has no answer (stage 3).
- **Share list**: SRVSVC NetShareEnumAll over the IPC$ named pipe
  (DCE/RPC). Not needed to mount, but the Shares tab's *Browse...* wants it
  (stage 3).

## DOSDriver

```
Handler   = L:prismsmb-handler
StackSize = 8192
GlobVec   = -1
Startup   = "nas.lan/public VOLUME=NAS GUEST"
```

| Option | Default | Notes |
|---|---|---|
| `host/share[/path]` | required | or `//host/share`; the host is a name or dotted quad |
| `USER=` `DOMAIN=` | asked at mount | `DOMAIN=` defaults to WORKGROUP (PrismNet's workgroup setting) |
| `GUEST` | off | anonymous/guest session |
| `VOLUME=` `TZ=` `READONLY` `RSIZE=` `WSIZE=` `IDLE=` `RETRY=` `TIMEOUT=` | as NFS | shared core |
| `SIGN=AUTO\|YES` | AUTO | YES forces signing even when optional |
| `ENCRYPT=AUTO\|YES\|NO` | AUTO | AUTO: only when a share demands it |
| `MAXPROTO=2.1\|3.0\|3.0.2\|3.1.1` | 3.1.1 | cap the dialect (odd servers, or speed on a 68000) |
| `PORT=` | 445 | |

Unmount: `ShareUnmount PSMB0:` (the same tool as NFS).

## Robustness

- **Lost connection**: reconnect, NEGOTIATE, SESSION_SETUP (with the kept
  credentials), TREE_CONNECT. Open files are opened again by path and keep
  their position. Writes in flight are sent again, since SMB writes are
  idempotent at an offset.
- **Durable handles** are out of scope: a server restart during a write
  shows up as a failed Close if the reopen finds the file changed (size or
  time).
- **Clean shutdown**: ECHO keeps an idle session alive only while files are
  open. After `IDLE=` the handler sends LOGOFF and drops the connection.
- **Proven parts reused**: the private packet port, globals cleared at
  start, and no reconnect from disk-info polls, all from the NFS client.

## Test plan

1. **Host (WSL)**: crypto test vectors (RFC 1320/1321/2104/4231/4493);
   `smbtool` built natively. It runs negotiate / session (guest) / tree / ls /
   cat / put against all three servers, and the MD5 of the data is compared
   over SSH (Synology) or on the PC (Windows share).
2. **Amiga, read-only** ("PrismNet test", WinUAE):
   - `nfstest`-style DOS checks against a guest share on the Synology, and
     against the Drobo `public` share.
   - Side by side with **smb2fs 53.9** (installed on the test machine) as the
     baseline: the same listings and checksums.
3. **Windows with signing required**: a share on this PC that Laine creates.
   Reads must work signed; the signing cost is measured at 68030 and on the
   2 MB "PrismNet a500" machine.
4. **Writes** (stage 2), only in test folders Laine names: create, append,
   rename, delete, mkdir, times; a 1 MB round trip with MD5 check.
5. **Robustness**: server restart during a read, PrismNet STOP/START, IDLE,
   a wrong password (requester, then a clean failure).
6. **Gate**: crypto vectors and the shared-core NFS tests join
   `tools/gate.py`.

## Stages and effort

| Stage | Contents | Estimate |
|---|---|---|
| 0. Shared core | split handler.c into core + NFS backend; all NFS tests pass unchanged | 3 days |
| 1. Read-only, SMB 2.0.2/2.1 | NEGOTIATE, NTLMv2/SPNEGO, guest, HMAC-SHA256 signing, TREE_CONNECT, compound examine, QUERY_DIRECTORY, open/read/close, credits, FsFullSize, password window, smbtool, crypto vectors | 8-10 days |
| 2. Writes | create/overwrite/append, SET_INFO (rename, delete, size, times, attributes), flush on Close, reconnect-and-reopen | 5 days |
| 3. SMB 3.x and extras | 3.0/3.0.2/3.1.1, AES-CMAC signing, KDF, pre-auth SHA-512, optional encryption, NetBIOS names, share list, reparse points | 7-9 days |
| Installer, template `PSMB0`, docs | the SMB checkbox goes live | 1 day |
| **Total** | | **24-28 days** |

That is about **twice the NFS client**: authentication, signing and SMB's
statefulness are the extra weight. Stage 1 alone already reaches all three
servers here (signed 2.1), read-only.

## Out of scope

SMB1/CIFS, Kerberos and domains, DFS referrals, multichannel, oplocks and
leases, printing, change notifications, alternate data streams and an SMB
server.

## Open questions

1. A test account on the Synology (and maybe a share on this PC) for signed,
   authenticated tests. Laine creates it and types the password on the Amiga.
2. Handler name: `prismsmb-handler` (proposed); smb2fs is `smb2-handler`, so
   no clash.
3. In stage 1, sign only when required, or always when the server enables
   it? Proposed: only when required, for speed, with `SIGN=YES` for those who
   want it.
