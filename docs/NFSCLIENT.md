# PrismNet NFS client: design and scope

Audit 2026-10-18 (docs/AUDIT.md): ParentDir/NameFromFH on files, soft links opened
for writing, overlong UTF-8 in names, XDR length wrap (`make xdrtest`).

Status: **stage 1 (read-only) working** in WinUAE against the Synology,
2026-10-17. Design written the same day.

## Stage 1 result

`L:prismnfs-handler` 0.1, **17 KB**, code in `tools/nfs/`, built by `make nfs`.
`tools/nfs/nfstool.c` runs the same protocol code on the PC (`make nfstool`,
WSL) and `tools/nfs/nfstest.c` drives a filesystem through DOS calls step by
step. Against `192.168.1.10:/volume1/amigatest` on "PrismNet test" (68030,
OS 3.2), side by side with AmiNFSv3 0.2b2:

| Check | PrismNet | AmiNFSv3 |
|---|---|---|
| Lock / Examine / ExNext of the root | same entries | same entries |
| Open + Read 304825 bytes (4 KB reads) | byte sum 40082838 | byte sum 40082838 |
| `Copy` to the host drive, MD5 | `131156c5...` identical | identical |
| Copy time | ~1 s | ~1 s |
| ExamineFH, Seek to end, ParentDir of root | correct | correct |
| Case-insensitive name (`SMB2FS.m68k...`) | works | (not tried) |
| `Info PN0:` | 103 TB, Read Only | - |
| Source port | **1021** (reserved) | above 1024 |
| Memory while mounted (RSIZE 32768) | about 110 KB incl. buffers | |

Found on the way: with no startup code, libnix's 32-bit divide helpers call
utility.library through a `UtilityBase` nobody had opened (address error
80000003 on the first date conversion); the handler now opens it itself.
`locale.library`'s `loc_GMTOffset` is taken as minutes **west** of GMT;
set `TZ=` in the DOSDriver when the Amiga's locale is not set up.

**PrismNet STOP/START** (same day): with the share mounted and Workbench
showing it, `PrismNet STOP` stays stopped (Workbench's disk-info polls are
answered from cache and never reconnect); the next real access reopens
`bsdsocket.library`, which starts PrismNet again, and the read is identical.
Two fixes came out of it: packets now arrive on a private port (DOS calls in
the handler process - PrismNet's on-demand start - wait on `pr_MsgPort`, and
a client packet arriving there was dead-end alert 07000004), and disk-info
no longer connects.

**Small machine** ("PrismNet a500" config: 68020, 2 MB chip, no fast RAM,
PCMCIA NE2000): after Workbench and PrismNet about 190 KB is left. With
`RSIZE=4096` the mounted share costs **46 KB** in all; the 304825-byte copy
took about 4 s (~75 KB/s), MD5 identical. With under 1 MB free the handler
now picks 4 KB transfers by itself (8 KB under 4 MB, 32 KB above). On that
machine `PrismNet START` gave up waiting before the stack was up (it came up
with `Run PrismNet RUN`), and `PrismNet STATUS` cannot load with so little
memory - stack-side notes, not NFS ones.

**NAS NFS service restarted during reads** (`test/amiga/t-nfsrestart`):
the Amiga read the 304825-byte file every ~5 s from 16:27:12 to 16:33:17
while NFS was turned off and on in DSM; the NAS's `mountd`/`rpcbind`
restarted at 16:27:36 and `nfsd` at 16:28:10. **73 reads, all with the right
byte sum, none failed or stalled.** The handler retries a lost server for
`RETRY=` seconds (default 30) before reporting "No disk present".

**Reserved port** (Synology export with *Allow non-privileged ports*
turned **off**): a MOUNT sent from Windows from source port 1001 is accepted
(status 0) and from port 57541 refused (status 13, access). So binding a port
below 1024, as the handler does, is what a "secure" export wants. It cannot be
shown end to end in WinUAE: slirp (and WSL2) re-originate the connection from
an ordinary high port on the PC, so the NAS refuses both PrismNet and
AmiNFSv3 there; on a real Amiga on the LAN the port arrives unchanged. The
test turned up one handler bug, now fixed: a refused MOUNT was retried for
30 s and ended in "No disk present"; it now fails at once with "file is read
protected".

Stage 1 is complete.

## Stage 2 result (writes)

`prismnfs-handler` 0.2, **24 KB**. Writes go through a write-behind buffer of
`WSIZE` bytes as UNSTABLE WRITEs; Close sends COMMIT and fails (IoErr "seek
error") if the server's write verifier changed, i.e. it lost data in a
restart. A read, seek or examine of a file being written flushes it first.
New packets: FINDOUTPUT/FINDUPDATE (existing name in any case is reused;
new files 0644 via GUARDED CREATE), WRITE, SET_FILE_SIZE, DELETE_OBJECT
(REMOVE/RMDIR), RENAME_OBJECT (refuses to replace an existing object, as
AmigaDOS does; a change of case is allowed), CREATE_DIR (0755), SET_PROTECT
(owner bits from RWED; group/other only when given), SET_DATE, SET_COMMENT
(only an empty comment). `READONLY` refuses all writes locally; `WSIZE=`
sets the write size (default = RSIZE, bounded by the server's FSINFO).

Tested 2026-10-17 on "PrismNet test" against `amigatest` (read/write,
squash to guest), checked on the NAS over SSH (`test/amiga/t-nfswrite`):

| Check | Result |
|---|---|
| `Copy` 1 MB random file to the NAS | ~2 s; NAS MD5 = original (`3bb8d034...`) |
| Copy it back | MD5 identical |
| `Echo >` then `Echo >>` (create, append) | both lines in the file |
| `Rename` (incl. new case) | works; onto an existing name: "object already exists" |
| `Protect r` | NAS mode 477 (owner r--, group/other kept) |
| `SetDate 01-Jan-25 12:00` with `TZ=-5` | NAS mtime 17:00 UTC |
| `MakeDir`; `Delete` non-empty drawer | made; "directory not empty" |
| `Delete ALL FORCE` | all gone |

Also done with the protocol layer from the PC (`nfstool put/mkdir/mv/rm/rmdir`):
1 MB written UNSTABLE + COMMIT, verifier matched, MD5 identical on the NAS.

Not yet tried for writes: a server restart in the middle of a write (the
verifier path).

## Large files (2026-10-17)

1 GiB (1,073,741,824 bytes) of random data, Amiga `Copy` both ways through
`prismnfs-handler` (shared core) to the Synology `amigatest` export:

| Step | Time | Speed |
|---|---|---|
| Copy host drive -> NFS (write) | 27 min 58 s | ~625 KB/s |
| Copy NFS -> host drive (read back) | 30 min 44 s | ~570 KB/s |

MD5 `40835f6e...` identical for the original, the file on the NAS (checked
over SSH) and the copy read back.

## Stage 3 result (speed, polish, installer)

`prismnfs-handler` 0.3, **32 KB**.

| Feature | Result (2026-10-17, "PrismNet test" + Synology) |
|---|---|
| `IDLE=s` (default 60, 0 = never): with no open files the handler closes its connection and its `bsdsocket.library` base after a quiet spell (timer.device) | `IDLE=10`: gone from NETSTAT after 25 s, the next `List` reconnected |
| ACTION_EXAMINE_ALL (+ EXAMINE_ALL_END), pattern and hook matching | `List` and `List PAT #?.lha` (OS 3.2 List uses ExAll) |
| Soft links: a link in a path answers ERROR_IS_SOFT_LINK and READ_LINK returns the path with the link replaced; relative targets become Amiga parent steps (`../x` -> `/x`), absolute ones inside the export become `VOLUME:...`; MAKE_LINK makes soft links (hard: not supported) | links made on the NAS (relative and absolute) read through correctly; `MakeLink ... SOFT` from the Amiga shows as `pntest:...`; deleting a link deletes the link |
| `DEPTH=1..4`: READs/WRITEs of whole RSIZE/WSIZE chunks pipelined on one connection (replies matched by XID, out of order allowed; any trouble falls back to one call at a time) | 1 MB both ways MD5-identical at DEPTH=4; **no speed gain measured** in WinUAE (8 MB: write 13 s, read 16 s at both depths) - there the stack's own TCP throughput (~600-780 KB/s, see CONFORMANCE.md) is the limit, not round trips. Default stays 1; worth trying on a slower or higher-latency link |
| Restart after ACTION_DIE | fixed: the next access starts a new process on the same segment, so all globals are now cleared at start (stale "mounted" state crashed it) |
| Small machine (2 MB chip, 68020) | list, write 200 KB (~3 s), read back MD5-identical; mounted cost now 64 KB (code grew 17 -> 32 KB) |

**ShareUnmount** (`C:ShareUnmount PNFS0:`, 2.5 KB): sends ACTION_DIE; "object
is in use" while files or locks are open; the next access mounts again.

**Installer**: a new optional step "Install network-share clients?" (NFS
client; SMB shown as not available yet). NFS copies `L:prismnfs-handler`,
`C:ShareUnmount` and `SYS:Storage/DOSDrivers/PNFS0` (+ icon, default tool
`C:Mount`; an existing PNFS0 is kept). Remove deletes the handler, and PNFS0
only while it still carries the "PrismNet NFS client" text; ShareUnmount
goes with the other PrismNet C: tools. Tested by clicking through the
Installer over NetHarness on the "PrismNet install" machine: the install
step put all four files in place and showed the SMB note; the Remove lines,
run on their own, deleted all four. The full Remove stalled at its
"stop PrismNet" step in that harness (Installer started over NetHarness with
the stack running from a drawer via a LIBS: assign); `PrismNet STOP` with a
client connected returns at once on its own, so this looks harness-specific
- **a plain Remove from Workbench still needs one run** to confirm. Part of the plan in
[SHARES.md](SHARES.md): PrismNet's own network-share clients, NFS first,
offered as an optional install.

## Goal

`L:prismnfs-handler`: an NFSv3 client filesystem that mounts a NAS or Linux
export as an ordinary Amiga volume.

- **PrismNet's own code, written from scratch.** It is written from the RFCs (5531 RPC,
  4506 XDR, 1833 portmap, 1813 NFSv3/MOUNT3). AmiNFSv3's code is not used or
  read; AmiNFSv3 is only the behaviour baseline in tests.
- **Small machines first:** a 68000 on OS 2.04 with 1-2 MB must be able to
  use it.
- **Same DOSDriver shape as AmiNFSv3**, so the Shares tab writes either one.
- **Optional:** one checkbox in the installer.

## Decisions

| Question | Choice | Why |
|---|---|---|
| Plain DOS handler or filesysbox? | **Plain packet handler** | filesysbox needs OS 3.0 and is an outside library under its own licence. A plain handler runs on 2.04, has no dependency and costs nothing in memory beyond itself. The packet set we need is modest. |
| Transport | **TCP only** (RPC record marking) | UDP NFS loses whole 8-32 KB requests on any drop; TCP retransmits segments. Every NAS since ~2010 does NFS over TCP. |
| Auth | **AUTH_UNIX** (AUTH_SYS), `UID=`/`GID=` | It is all a home NAS uses; Kerberos is out of scope. |
| Source port | **Reserved port (1023 downwards) by default** | lwIP lets PrismNet bind below 1024, so the server's default `secure` export rule works. A Synology then needs no *Allow non-privileged ports* tick, and Linux needs no `insecure` option. `RESVPORT=NO` turns it off. No other Amiga stack client does this. |
| Host names | **Allowed** (`gethostbyname`) | Resolved at mount and at every reconnect, so a NAS that changes address under DHCP keeps working. Dotted quads still work. |
| Writes | **UNSTABLE + COMMIT on Close** | Fast, and NFSv3's write verifier makes it safe: see *Robustness*. |
| Case | **Case-insensitive lookups** | AmigaDOS names are case-insensitive and servers are not. Try the exact name, then fall back to a directory scan with a Latin-1 case-insensitive compare. Create keeps the case as typed. |

## Protocol subset

- **portmap v2** `GETPORT`: find the MOUNT and NFS ports. This is skipped with
  `PORT=`/`MOUNTPORT=`, since NFS is almost always 2049.
- **MOUNT3** `MNT` (export path to root file handle), `UMNT` on dismount.
- **NFS3, read-only stage:** `GETATTR`, `LOOKUP`, `ACCESS`, `READ`,
  `READDIRPLUS` (falls back to `READDIR` + `LOOKUP`), `FSSTAT`, `FSINFO`
  (server's preferred sizes), `PATHCONF`, `READLINK`.
- **NFS3, write stage:** `SETATTR`, `WRITE`, `COMMIT`, `CREATE` (guarded),
  `MKDIR`, `REMOVE`, `RMDIR`, `RENAME`, later `SYMLINK`/`LINK`.
- **RPC:** one TCP connection per mount. It uses record marking, XIDs
  matched to replies and a timeout per call (`TIMEOUT=`, default 15 s). Stage 1
  has one call in flight; the later `DEPTH=` keeps up to 4 READ/WRITE calls in
  flight on the same connection.

## The handler

**Process model.** One handler process per mounted device, started by
`Mount` (DOSDriver with `GlobVec = -1`, `Startup = "host:/export options"`).
It opens its **own** `bsdsocket.library` base, because bases are per task.
While a call is outstanding it waits with `WaitSelect()` on the socket and on
its packet port's signal. Packets that arrive meanwhile queue on the port, and
`ACTION_DIE` / Ctrl-C can always get through.

**Lazy start.** Nothing touches the network until the first packet that
needs it. A share in `DEVS:DOSDrivers` is mounted by the Startup-Sequence
before PrismNet is up, and the first access opens `bsdsocket.library`, which
starts PrismNet on demand (`AUTOSTART`).

**Volume.** On the first successful `MNT` the handler adds a volume node: name
from `VOLUME=` (default: last part of the export path), date from the export
root's mtime. Until then `ACTION_DISK_INFO` reports "no disk", so Workbench
shows the device without hanging at boot.

**Packets.**

| Group | Packets |
|---|---|
| Locks | `LOCATE_OBJECT`, `FREE_LOCK`, `COPY_DIR`, `PARENT`, `SAME_LOCK`, `CHANGE_MODE` |
| Examine | `EXAMINE_OBJECT`, `EXAMINE_NEXT`, `EXAMINE_FH`, `EXAMINE_ALL` (with `ExAllEnd`) |
| Files | `FINDINPUT`, `FINDOUTPUT`, `FINDUPDATE`, `READ`, `WRITE`, `SEEK`, `END`, `SET_FILE_SIZE`, `FH_FROM_LOCK`, `COPY_DIR_FH`, `PARENT_FH` |
| Changes | `DELETE_OBJECT`, `RENAME_OBJECT`, `CREATE_DIR`, `SET_PROTECT`, `SET_DATE`, `SET_COMMENT` (refused: NFS has no comments) |
| Volume | `DISK_INFO`, `INFO` (from `FSSTAT`), `IS_FILESYSTEM`, `CURRENT_VOLUME`, `FLUSH`, `DIE` |
| Later | `READ_LINK`, `MAKE_LINK` (symlinks), record locks (refused) |

**Locks and paths.**
- A lock carries the NFS file handle (up to 64 bytes) and the access mode.
  Shared and exclusive locks are enforced locally, per handle; NFSv3 has no
  server-side locking, and NLM is out of scope.
- A path is walked one part at a time with `LOOKUP`, using a small cache
  (directory handle + name -> handle + attributes, 32 entries, a few
  seconds' life). This cuts the round trips that make `List` and Workbench
  slow over a network.

**Examine.**
- `READDIRPLUS` returns the names, attributes and handles together.
- The cookie and cookie verifier live in the lock's examine state.
- One reply fills many `EXAMINE_NEXT` / `ExAll` calls.

**Attribute mapping.**
- Owner `rwx` maps to Amiga `RWED`: x is E, w is W and D; the Amiga bits are
  inverted.
- Group and other bits map to the OS 3.x group/other protection bits.
- `SET_PROTECT` maps back through `SETATTR mode`, keeping the bits that
  have no Amiga meaning.
- Sizes are 64-bit on the wire. Up to 2 GB is exact. From 2 to 4 GB
  `fib_Size` is a ULONG that some programs show as negative. Above 4 GB the
  size is reported as 0xFFFFFFFF and reads still work up to 4 GB; the
  32-bit `Seek` packet can't go further.
- Times are UTC on the wire.
  - **Default time zone:** from `locale.library` (OS 2.1+) when present,
    else `TZ=`, else 0.
  - **Conversion:** 1970 epoch to the Amiga's 1978 `DateStamp`.
  - **`SET_DATE`:** sends mtime.
- Names are UTF-8 on the server and Latin-1 on the Amiga (`CHARSET=UTF8`, the
  default, or `RAW`).
  - **Characters with no Latin-1 equivalent:** shown as `?` and marked in
    the cache, so such a file can still be opened from a listing.
  - **Names longer than 107 bytes:** shortened with a `~n` tail mapped
    through the same cache.
  - **`/` and `:`** can't appear in Amiga names, so files with them are
    skipped.

## Memory (68000, 1-2 MB)

| Part | Size |
|---|---|
| Handler code | about 25-35 KB (target; plain C, no libnix stdio) |
| Per mount: RPC buffer | `RSIZE`/`WSIZE` + 200 bytes header. The default on machines with under 4 MB free is **8 KB/8 KB** (about 18 KB); with plenty of memory, the server's `FSINFO` preference up to 32 KB/32 KB |
| Per mount: lookup cache | 32 entries x ~100 bytes = 3 KB |
| Per open file | about 120 bytes; data goes through the mount's buffer (stage 1) and a write-behind buffer of `WSIZE` while writing (stage 2) |
| PrismNet side | one TCP socket; its window comes from the stack's own budget |

So about **55 KB for a mounted share on a small machine**, against AmiNFSv3's
~130 KB default (~40 KB with small sizes).

## Robustness

- **Connection lost** (server restart, Wi-Fi, PrismNet STOP).
  - **What the handler does:** it reconnects, resolving the name again, and
    retries. File handles survive, because NFSv3 handles are stable across
    server restarts.
  - **Calls repeated after a reconnect:**
    - *Idempotent calls* (`GETATTR`, `LOOKUP`, `READ`, `READDIRPLUS`,
      `WRITE`, `COMMIT`, `SETATTR`) are simply re-sent.
    - *Non-idempotent calls* (`CREATE`, `MKDIR`, `REMOVE`, `RENAME`) are
      re-sent once. A reply of "already exists" / "not found" after a
      re-send is checked with `LOOKUP`, not taken as failure.
- **PrismNet stopped:** calls fail with `ENETDOWN`. The handler closes its
  library base and reopens it on the next access, which auto-starts the stack
  again if *Start when needed* is on.
- **Idle:** after `IDLE=` seconds (default 60) with no open files, the handler
  closes the TCP connection and its library base. A stopped stack can then
  really stop, and a sleeping NAS is not kept awake.
- **Write verifier:** the handler keeps unstable data until `COMMIT` succeeds.
  If the verifier from `COMMIT` differs from the one returned by `WRITE`, the
  server lost the data in a restart.
  - **Data still in the buffer:** it is written again.
  - **Data no longer buffered:** `Close` fails with `ERROR_WRITE_PROTECTED`
    plus a retry requester ("NFS server restarted, file may be incomplete"),
    never silent loss.
- **Server gone at boot:** no hang. The handler gives up after `TIMEOUT=` and
  shows a requester once per mount ("NFS0: server 192.168.1.10 not
  answering"). Workbench sees a device with no disk.

## DOSDriver

Option names match AmiNFSv3 where both exist, so the Shares tab can write
either handler:

```
Handler   = L:prismnfs-handler
StackSize = 8192
Priority  = 5
GlobVec   = -1
Startup   = "nas.lan:/volume1/amigatest VOLUME=NAS READONLY"
```

| Option | Default | Notes |
|---|---|---|
| `host:/export` | required | host name or dotted quad |
| `VOLUME=` | last part of the export path | AmiNFSv3 name |
| `UID=` `GID=` | 1000 / 1000 | AmiNFSv3 names |
| `TZ=` | from locale | AmiNFSv3 name |
| `RSIZE=` `WSIZE=` | from FSINFO, capped by free memory | AmiNFSv3 names |
| `CHARSET=UTF8\|RAW` | UTF8 | AmiNFSv3 name |
| `DEPTH=1..4` | 1 | stage 3 |
| `READONLY` | off | refuse writes locally |
| `RESVPORT=YES\|NO` | YES | bind a port below 1024 |
| `PORT=` `MOUNTPORT=` | portmap | skip portmap |
| `TIMEOUT=` | 15 | seconds per call |
| `IDLE=` | 60 | seconds before an idle disconnect, 0 = never |

Unmounting: `C:ShareUnmount NFS0:`, a small PrismNet tool that sends
`ACTION_DIE` (later reused by the SMB client). PrismNetPrefs' *Dismount*
button does the same.

## Installer

`dist-src/Install_PrismNet` gains one `askoptions` step after PrismMon:

```
Install network-share clients?
  [ ] NFS client (NAS / Linux shares)
  [ ] SMB client (Windows / macOS / NAS shares)    <- greyed until it exists
```

NFS copies `L:prismnfs-handler`, `C:ShareUnmount` and a template
`SYS:Storage/DOSDrivers/NFS0` (with icon, default tool `C:Mount`). Remove
deletes exactly those files. If `L:nfs-handler` (AmiNFSv3) is already there,
it is left alone; the two can coexist.

## Code layout

```
tools/nfs/xdr.c      XDR encode/decode into a flat buffer
tools/nfs/rpc.c      record marking, XID, AUTH_UNIX, call/reply, reconnect
tools/nfs/proto.c    portmap, MOUNT3, NFS3 calls -> C structs
tools/nfs/cache.c    lookup cache, name mapping (Latin-1 <-> UTF-8, long names)
tools/nfs/handler.c  packet loop, locks, file handles, volume node
tools/shareunmount.c ACTION_DIE tool
```

`xdr.c`, `rpc.c`, `proto.c` and `cache.c` have no Amiga dependencies beyond a
socket shim, so they **also build natively in WSL**. That lets the protocol
layer be tested against the Synology from the PC before any Amiga run.

## Test plan

1. **Host unit tests (WSL):** XDR round trips; name mapping (Latin-1, `?`,
   long names); RPC framing against canned byte streams.
2. **Host protocol test (WSL to Synology):** portmap, MNT, LOOKUP, READDIRPLUS,
   READ of the `amigatest` export; MD5 of a file read through our code
   matches.
3. **Amiga, read-only** ("PrismNet test", WinUAE, slirp to the LAN):
   - A new `nfstest` CLI program drives DOS calls: Lock/Examine/ExAll,
     Open/Read/Seek, parent walks, case-insensitive names and Latin-1 names.
   - The same `nfstest` run against **AmiNFSv3 as the baseline** must give
     the same listings and checksums.
   - Copy-back MD5 check, as done with AmiNFSv3 on 2026-10-17
     (304825-byte file, identical).
4. **Reserved port:** with the Synology's *Allow non-privileged ports* turned
   **off**, our handler mounts and AmiNFSv3 is refused.
5. **Robustness:** PrismNet STOP/START during a `List`, the NAS's NFS service
   restarted during a read, and the idle disconnect. Any test that needs the
   NAS's NFS service restarted is done by hand.
6. **Writes (stage 2), only after setting `amigatest` read/write**:
   create, write, append, rename, delete, mkdir; a 1 MB write with MD5 check;
   write-verifier path (NAS restart mid-file).
7. **Small machine:** the A500 config (68000, 1 MB chip + some fast) mounts
   and copies with 8 KB sizes; memory measured with `Avail` before and after.
8. **Gate:** the host unit tests join `tools/gate.py`; the Amiga read-only
   test joins as `t-nfs` once the Synology export can be assumed. It is
   skipped with a note when the NAS can't be reached.

## Effort

| Stage | Contents | Estimate |
|---|---|---|
| 1. Read-only | XDR/RPC/portmap/MOUNT3, reconnect, read-side packets, volume node, lookup cache, names, TZ, `Info`, host tests, `nfstest` | 7-8 days |
| 2. Read/write | write-side packets, UNSTABLE+COMMIT, verifier, `SET_PROTECT`/`SET_DATE`, guarded create | 4-5 days |
| 3. Speed and polish | `DEPTH=` pipelining, symlinks, `IDLE=`, requesters, small-machine tuning | 3 days |
| Installer, `ShareUnmount`, template, docs | | 1 day |
| **Total** | | **15-17 days** |

Stage 1 alone is already useful: a read-only NAS browser that works on a
68000, for copying games and files *to* the Amiga.

## Out of scope

- NFSv2, NFSv4, UDP, Kerberos, NLM byte-range locking.
- An NFS server.

## Open questions

1. Handler name: `prismnfs-handler` (proposed) or plain `nfs-handler`? A plain
   name would clash with AmiNFSv3.
2. OK to make `amigatest` read/write for stage 2 testing?
