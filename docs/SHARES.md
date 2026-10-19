# Network shares (SMB / NFS): scope

Status: **Shares tab implemented 2026-10-18** (first cut; the design below is
from 2026-10-17 and stands where it is not contradicted here).

**What exists.** PrismNet Prefs has a fifth tab, *Shares*: a list of shares
with Device, Type (NFS/SMB), Server, Share/export, Volume, User (SMB) or
UID/GID (NFS), *Mount at boot*, *Read-only*, *Browse...*, *Mount now* and
*Dismount*. Each share is one DOSDriver file - `DEVS:DOSDrivers/<Device>`
when it mounts at boot, else `SYS:Storage/DOSDrivers/<Device>` - with a
project icon, so `Mount` and Workbench use the same file. On load the tab
lists every file in both drawers whose `Handler` is `L:prismnfs-handler`,
`L:prismsmb-handler`, `L:nfs-handler` (AmiNFSv3) or `L:smb2-handler`
(smb2fs); options it does not edit are kept. New shares get PrismNet's own
handler when it is installed. *Browse...* lists the server's disk shares
through `C:SMBShares` (guest, or the user with the saved password) and sets
the share and volume from the pick. Passwords: the SMB handler saves them
after the first successful mount (see SMBCLIENT.md), so a share set up
here asks once, on the first access. Tested on the WinUAE machine with
NetHarness: SMB share browsed, mounted, saved, reopened, dismounted; NFS
share mounted from the written file; a boot-mounted share starts the stack
on its first access after a plain reboot.

Goal: let a PrismNet user mount a NAS or PC share as an ordinary Amiga volume,
set up from PrismNet Prefs, with the filesystem drivers as an **optional**
install. PrismNet does not write its own network filesystems: two good ones
already exist and work over any `bsdsocket.library`.

## The two drivers

| | AmiNFSv3 | smb2fs |
|---|---|---|
| Author / source | Tobias Karlsson, [github.com/creep-ltx/AmiNFSv3](https://github.com/creep-ltx/AmiNFSv3), Aminet `comm/net/AmiNFSv3` | Fredrik Wikstrom (salass00), [github.com/salass00/smb2fs](https://github.com/salass00/smb2fs), Aminet `disk/misc/smb2fs.m68k-amigaos` |
| Version | 0.2b2 (pre-release, 2026-10) | 53.10 (2025-06) |
| Licence | **MIT** | **GPL-2.0**; the bundled libsmb2 has its own licence (`LICENCE-libsmb2` in the archive) |
| Protocol | NFSv3 over TCP (SunRPC, MOUNT3; server ports 111, 2049, mountd) | SMB2/SMB3 (libsmb2) |
| Needs | OS 2.04+, any 68000, `bsdsocket.library` V4 | OS 3.0+, `filesysbox.library` 54.3+, optional ReqTools (password requester) |
| Files | `L:nfs-handler`, `C:NFSDismount`, `Storage/DOSDrivers/NFS0` | `L:smb2-handler` (.000/.020/.060 builds), sample DOSDriver |
| Memory | ~130 KB per mount by default; ~40 KB with `RSIZE=16384 WSIZE=16384` | not documented; filesysbox + libsmb2, StackSize 65536 |
| Auth | AUTH_UNIX: `UID=`/`GID=` sent with each request; no password | user + password; password in the Startup line, the URL, or a ReqTools requester at mount |
| Host names | **dotted-quad IP only** for now | URL host (uses the resolver) |
| Tested here | yes (2026-10-17): mounted the Synology `amigatest` export over PrismNet in WinUAE, listed, clean `NFSDismount` | yes: `test/amiga/t-smb` mounts a Drobo share over PrismNet |

The whole configuration lives in the DOSDriver's `Startup` line:

```
; AmiNFSv3: "host:/export [VOLUME=name] [UID=n] [GID=n] [TZ=+H[:MM]]
;            [RSIZE=n] [WSIZE=n] [CHARSET=UTF8|RAW] [DEPTH=1..8]"
Handler   = L:nfs-handler
Startup   = "192.168.1.20:/volume1/amiga VOLUME=NAS UID=1026 GID=100"

; smb2fs: URL/A,USER,PASSWORD,VOLUME,DOMAIN/K,READONLY/S,NOPASSWORDREQ/S,
;         NOHANDLESRCV/S,RECONNECTREQ/S
Handler   = L:smb2-handler
StackSize = 65536
Priority  = 5
GlobVec   = -1
Startup   = "smb://192.168.50.3/public VOLUME=Drobo DOMAIN=WORKGROUP NOPASSWORDREQ"
```

A Linux NFS server needs one export line (Synology needs nothing):
`/srv/amiga 192.168.1.0/24(rw,insecure,no_subtree_check,all_squash,anonuid=1000,anongid=1000)`
(`insecure` because Amiga stacks do not use privileged source ports).

### SMB or NFS on a slow Amiga?

NFS is the better fit for small machines: one compact RPC per operation, no
session negotiation, a 68000-clean handler that runs on OS 2.04, and a
documented low-memory setting. Directory listings with many small `.info`
files are where SMB's round trips hurt most. SMB is what most people already
have switched on (Windows, macOS, every NAS), needs no server-side export
line, and handles user/password. So: **offer both; recommend NFS for a NAS or
Linux box and for 68000/low-memory machines, SMB for Windows/macOS shares.**

### First live test (2026-10-17)

AmiNFSv3 0.2b2 (Aminet `AmiNFSv3.lha`, 30908 bytes) on "PrismNet test"
(68030, OS 3.2, slirp) against the Synology DS1821+ (DSM 7.4.1), share
`amigatest` exported read-only, squash to guest, non-privileged ports allowed:

```
Startup = "192.168.50.2:/volume1/amigatest VOLUME=amigatest RSIZE=16384 WSIZE=16384"
Mount DH0:PrismNet/nfs/NFS0      -> List NFS0: shows the share
PrismNet NETSTAT                 -> tcp 10.0.2.15:50888 192.168.50.2:2049 ESTABLISHED NFS0
NFSDismount NFS0:                -> "NFS0: dismounted"
```

Notes: `Info NFS0:` answers "object is not of required type"; dates show
UTC unless `TZ=` is set (Prefs should fill it from the Amiga's locale). Its
template says the defaults are now RSIZE 131072 / WSIZE 65536 and DEPTH
1..4; Prefs should write explicit sizes rather than rely on defaults. On the
Synology the export needs DSM's *NFS Permissions* tab, which appears only
after NFS is enabled in File Services; tick *Allow connections from
non-privileged ports*.

Copy-back check: `Copy NFS0:smb2fs.m68k-amigaos.lha` (304825 bytes) to the
host drive took about a second (WinUAE 68030, RSIZE 16384); MD5
`131156c5c3f56c4512a00743de74ffb4` on both sides, identical.

smb2fs needs two Aminet archives: `smb2fs.m68k-amigaos.lha` and
`filesysbox.m68k-amigaos.lha` (installed on the test machine:
smb2-handler 53.9, filesysbox.library 54.6).

## Licences and packaging

- **Decision (2026-10-17): link, do not bundle.** AmiNFSv3 could be bundled
  (MIT) but PrismNet links to both drivers on Aminet instead, and plans its
  own NFS and SMB components later (see "PrismNet's own services" below).
- **smb2fs stays a separate download.** Shipping GPL binaries is allowed but
  carries source-offer obligations for every PrismNet release; not worth it.
  The installer and Prefs detect it (`L:smb2-handler`,
  `LIBS:filesysbox.library`) and otherwise say where to get it (Aminet
  `disk/misc/smb2fs.m68k-amigaos` and `util/libs/filesysbox`).
- PrismNet only *writes DOSDriver text files* for these handlers, which
  creates no licence tie to either.

## PrismNet Prefs: a fifth tab, "Shares"

Fits the existing tab system (`tools/tabs.c`, `page_labels[]` in
`tools/prismprefs.c`) and the same frame height as the Interfaces page.

```
 [Interfaces][Network][Users][Startup][Shares]
 +- Network shares ------------------------------------------------+
 | NAS     NFS  |  Type    [NFS   v]   Volume   [NAS        ]       |
 | Drobo   SMB  |  Server  [192.168.1.20      ] [Resolve]           |
 |              |  Share   [/volume1/amiga             ]            |
 |              |  User    [         ]  (NFS: UID [1026] GID [100])  |
 | [Add][Remove]|  [x] Mount at boot  [ ] Read-only  [Options...]    |
 |              |  [Mount now] [Dismount]   Status: mounted          |
 +-----------------------------------------------------------------+
   NFS handler: installed   SMB handler: not installed (see Aminet)
```

- **Type** cycles NFS / SMB; the fields relabel (Share vs Export, User vs
  UID/GID). A type whose handler is missing is greyed with the "not
  installed" note, but existing entries still load and save.
- **Options...** opens a small window like *Routes...*: NFS `RSIZE`, `WSIZE`,
  `DEPTH`, `TZ`, `CHARSET`; SMB `DOMAIN`, `NOHANDLESRCV`, `RECONNECTREQ`.
  Defaults follow the machine: under ~8 MB free, or the small build, NFS
  gets `RSIZE=16384 WSIZE=16384`.
- **Resolve** looks the server name up with the PrismNet resolver and fills in
  the address, because AmiNFSv3 accepts only an IP. The name is kept in a
  comment in the DOSDriver so it can be re-resolved later. (Better: hostname
  support upstream in AmiNFSv3; worth offering a patch.)
- **Passwords** (as built): PrismNet's SMB handler asks once and saves the
  password itself (`ENVARC:PrismNet/smb/<host>.<user>`); the tab writes no
  password into the DOSDriver. For smb2fs the requester at mount time
  remains. NFS has no password (AUTH_UNIX).
- **Mount now / Dismount** run `Mount <path-to-DOSDriver>` (AmiNFSv3's
  DOSDriver format is not accepted by `Mount X: FROM file`) and, for NFS,
  `NFSDismount <dev>:`; for SMB, send `ACTION_DIE` to the handler (to check
  how filesysbox handles it; KillDev is the fallback smb2fs documents).
- **Status** shows mounted / not mounted (is the device in the DOS list?).

### Where the settings live

The DOSDriver files are the only copy, so Prefs, the Mount command and
hand-edits never disagree:

- *Mount at boot* -> `DEVS:DOSDrivers/<name>` (mounted by the
  Startup-Sequence); otherwise `SYS:Storage/DOSDrivers/<name>` (mount on
  demand). Toggling moves the file and its icon.
- Files Prefs creates start with a marker comment, `/* PrismNet share */`,
  plus `/* PrismNet host=nas.lan */` when a name was resolved. On load,
  Prefs lists every DOSDriver in both drawers whose `Handler` is
  `L:nfs-handler` or `L:smb2-handler`, so hand-made ones (like the Drobo
  entry) show up too; unknown keys and comments in them are kept on save.
- New files get a project icon (default tool `C:Mount`) so they also mount
  from Workbench.
- The page writes its files on Save; *Use* has no meaning here (a DOSDriver
  has no ENV: copy), which the page says.

### Boot order and on-demand start

`DEVS:DOSDrivers` is mounted in the Startup-Sequence *before* User-Startup
starts PrismNet. That works: both handlers open `bsdsocket.library` on first
access, and PrismNet's *Start when a program needs the network* brings the
stack up then. smb2fs must not get `ACTIVATE=1` (it would connect during the
mount); Prefs never writes it. A share shown in an open Workbench window is
touched at boot, which starts the stack: say so in the help text.

### PrismMon

Small follow-up: list mounted shares (volume, server, type) on the Status
tab, with *Dismount* in the right-click menu. Not needed for the first cut.

## Installer

No third-party files are installed. The installer reports whether AmiNFSv3
(`L:nfs-handler`) and smb2fs (`L:smb2-handler`, `LIBS:filesysbox.library`)
are present and, if not, where to get them on Aminet
(`comm/net/AmiNFSv3`, `disk/misc/smb2fs.m68k-amigaos`,
`util/libs/filesysbox`).

## PrismNet's own services

Decided 2026-10-17: PrismNet gets its **own NFS client and SMB client**
(filesystem handlers), clients first; servers are not planned for now. Both
ship in the PrismNet archive under its licence (GPL-3.0), and both are **optional**:
the installer asks

    Install network-share clients?
      [ ] NFS client (NAS / Linux shares)
      [ ] SMB client (Windows / macOS / NAS shares)

and copies only the chosen handlers (plus a DOSDriver template in
`SYS:Storage/DOSDrivers`); Remove takes them out again. Until they exist the
installer only points to AmiNFSv3 and smb2fs on Aminet.

Suggested order: NFS client first (NFSv3 over TCP, AUTH_UNIX: small,
stateless, fits a 68000 with little memory), SMB2/3 client second (sessions,
NTLMv2, signing: several times larger). The Shares tab writes DOSDrivers the
same way for whichever handler is installed, PrismNet's own or the linked
ones, so the tab does not change when PrismNet's handlers arrive. Each client
gets its own scope document: [NFSCLIENT.md](NFSCLIENT.md) for NFS, [SMBCLIENT.md](SMBCLIENT.md) for SMB.

## Out of scope

- A PrismNet-written NFS/SMB handler, NFSv4, Kerberos, SMB1, and server mode
  (sharing the Amiga's own disks).

## Test plan

1. **Parse/write round trip, no emulator**: a CLI mode `PrismNetPrefs
   SHARES=dir` loads every share DOSDriver from `dir` and writes them back;
   run under vamos on fixtures (the Drobo file, AmiNFSv3's `NFS0`, a
   hand-edited file with extra keys) and diff.
2. **GUI**: the tab renders with `PrismNet/prefspage=4`; NetHarness click
   test of Add / Type / Options / Save, as for the other tabs.
3. **NFS live**: AmiNFSv3 in WinUAE ("PrismNet test", slirp to the LAN)
   against the Synology, on a read-only or scratch export and **only with
   Laine's OK before anything is written to the NAS**: mount, `List`, copy
   a file to RAM:, compare, dismount. Then the A500 config with the
   low-memory sizes.
4. **SMB live**: extend `t-smb` (Drobo): mount from a Prefs-written file,
   the password-requester path, dismount.
5. **Boot order**: a *Mount at boot* share with on-demand stack start; the
   stack comes up on first access and not before.
6. The gate stays green; add the round-trip test to `tools/gate.py`.

## Effort

| Piece | Estimate |
|---|---|
| DOSDriver parse/write, CLI round trip, fixtures | 1 day |
| Shares tab, Options window, Resolve, Mount/Dismount, status | 2 days |
| Icons, moving between DOSDrivers/Storage, help text | 0.5 day |
| Installer step (detect drivers, Aminet pointers) | 0.5 day |
| Live tests (NFS on Synology, SMB on Drobo, boot order, A500) | 1 day |
| **Total** | **about 5 days** |

The PrismMon share list and an upstream AmiNFSv3 hostname patch are extra.

## Open questions

1. ~~Bundle AmiNFSv3 or link?~~ Link (decided 2026-10-17).
2. OK to test against the Synology with a scratch export (that means writing to it)?
