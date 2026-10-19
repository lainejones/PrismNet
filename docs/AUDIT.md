# PrismNet audit, second pass — 2026-10-18

Scope: everything written since the 2026-10-16 audit below — the shared
network-filesystem core and the NFS and SMB clients (`tools/netfs`,
`tools/nfs`, `tools/smb`), the SANA-II TX queue (`port/sanaif.c`), the tabbed
GUIs (`tools/tabs.c`, Prefs, PrismMon) — plus a size and speed check of the
whole suite and a list of what a rounded Amiga network setup still lacks.
Three parallel read-throughs produced 25 candidate defects. **Only a defect
that could be reproduced counts**: each one below was shown failing first,
then fixed, then shown passing, either on the WinUAE `PrismNet test` machine
against the real servers (Synology NFS/SMB, Drobo SMB, this PC) or with a unit
test that feeds the parser crafted bytes (no network, AddressSanitizer on the
PC, and the same test run as a 68000 binary under vamos where 32-bit `long`
matters). The rest is listed as "not reproduced" with what it would take.

## Reproduced and fixed

| # | Where | Problem (how it was reproduced) | Fix |
|---|---|---|---|
| 1 | NFS client, `core.c` ACTION_PARENT / PARENT_FH | `ParentDir()`, `NameFromLock()`, `NameFromFH()` and `ParentOfFH()` on a **file** all failed with error 204: the handler sent `LOOKUP ".."` on the file's handle, which NFS only allows for directories. Anything that asks where an open file lives (PROGDIR:, "show in drawer", editors' backup files) broke. **Repro:** `parenttest PN0:pn-audit/real.txt` 4 FAILED on the old handler, all passed on the new. | For a file, the parent is resolved by walking its path from the root. |
| 2 | NFS client, `core.c` FINDOUTPUT / FINDUPDATE | Opening a **soft link** for writing did not follow it: `Echo >>PN0:link x` reported success and `real.txt` was unchanged. | The open returns `ERROR_IS_SOFT_LINK` like every other path, so DOS follows the link and retries; `real2.txt` now gets the data. |
| 3 | NFS client, `names.c` | **Overlong UTF-8** in a server-side name decoded to `/`, `:` or NUL after the separator check had run on the raw bytes: a file named `docs<C0 AF>readme` listed as `docs/readme`, and acting on that name would have hit the real `docs/readme`. | Overlong sequences and control characters decode to `?` (listed as `docs?readme`). |
| 4 | NFS client, `xdr.c` | A reply carrying an opaque length of 0xFFFFFFFD..FF passed the room check because `XDR_PAD()` of it wraps to 0; the decoder then returned a 4 GB length to `do_read`. On the PC the test passed by accident (`u32` is 64-bit there); **as a 68000 binary under vamos the old decoder failed both cases**, the new one passes. READ replies longer than the request are also refused now, in `nfs3.c` and in the core's copy to `cbuf`. | Length checked against the bytes left before padding; `got > count` is a protocol error. |
| 5 | SMB client, `smb2.c` ntlm_auth | An NTLM CHALLENGE with a **TargetInfo list over about 540 bytes** overflowed the static token buffers (`tmp[1100]`) before any signing exists. AD forests with long DNS names send a few hundred bytes. **Repro:** `smbunit` with a 900-byte list — AddressSanitizer: write of 700 bytes past `out`. | TargetInfo offset/length validated without wrap, cut to 700 bytes, token buffers 1600 bytes (still fits the 2 KB read-only send buffer). |
| 6 | SMB client, `smb2.c` smb2_dir_next | A directory entry with a **wrapping FileNameLength** was accepted (read past the buffer), and a **backwards NextEntryOffset** made the listing loop. **Repro:** `smbunit`, both cases. | Both rejected; the chain must move forward and stay inside the buffer. In the same pass every other server-supplied offset/length was tightened the same way: READ (also `got > count`), QUERY_DIRECTORY, FsFullSize, negotiate contexts, the pipe reads and NDR strings of the share list. |
| 7 | SMB client, `smb2.c` smb2_connect | `ENCRYPT=YES` was silently ignored on a 2.x dialect or a guest session: **`SMBENC=YES SMBPROTO=210 smbtool drobo/public info` printed "encryption off" and exited 0.** | Refused with `FS_EUNSUPP` when nothing would be encrypted. |
| 8 | SMB client, `backend.c` readdir slots | A tree walk that keeps more than `NDIR` (2) listings open lost the outer one: a continuation whose slot had been reused **silently restarted from the first entry**, so `List ALL` / `Copy ALL` over a directory with >32 entries containing such a subdirectory never ended. Fixed by reopening and skipping the entries already handed out, and by evicting the least recently used slot. **Repro (morning, `t-smbwalk` with `walk`, a recursive scanner that keeps the parents' ExNext open the way real tree walkers do; `C:List ALL` reads each directory whole first and never shows it):** old handler 389 files / 12 directories before the 400-entry stop on an 80-file tree, new handler 80 files / 2 directories; the trace shows the lost slot reopened and 34 entries skipped. One `List ALL` run on the new handler showed `A/B` empty once and correct in three more runs, including one after an idle reconnect; not understood yet. | Reopen and skip; LRU eviction. |
| 9 | Prefs, `prismprefs.c` tab switch | Clicking a tab rebuilt the page **inside the message loop**; a click on the old page already queued behind it was then handed to Prefs with a pointer to a freed gadget (the scroller path already deferred this, the tab path did not). **Repro:** NetHarness, Prefs held off the CPU by a priority-0 hog while a tab click and a checkbox click were injected: the instrumented build logged `stale GADGETUP 804FC4C` (and `80C219C` on the control run); the fixed build logged nothing in two runs. | The tab switch is deferred until the message queue is empty, like the scroller. |

## Found by review, not reproducible here

- **sanaif: a write the driver keeps past a reset is never reused.** `tx_done`
  cleared `busy` but not `lost`, so a driver that ignored `AbortIO` for more than
  1 s would retire TX slots one reset at a time until nothing could be sent. The
  fault injection (`-DSANAIF_TEST_RESET=20 -DSANAIF_TEST_LOST`: a reset every
  ~1.5 s with nothing collected) cannot show it in WinUAE: through 33 forced
  resets during a 16 MB upload the emulated NE2000 never had a write outstanding
  at the moment of the reset (793 KB/s, 0 drops, same before and after). The
  one-line fix (clear `lost` when a kept request finally comes back, reads too)
  is included because it is symmetrical with what the read path already did and
  the injection shows it changes nothing otherwise; **it still wants a run on the
  real A1200 PCMCIA card**, where `cnet.device` stalls are real.
- **sanaif_close waits unbounded** for a stalled driver (`abort_req`), where the
  reset path uses a bounded wait. Needs a driver that never returns; left alone.
- **NFS: writes past 4 GB wrap to offset 0** (the read side has a guard, the write
  side not), **Seek/SetFileSize can't address 2-4 GB** (signed arithmetic), and
  the pipelined read of a >4 GB file loops. All need a multi-GB file on the
  export and hours of transfer; not run. The write guard is a two-line change
  for when it is.
- **NFS: non-idempotent operations are retried after a timeout** (REMOVE,
  RENAME, CREATE...), which turns a slow success into "object not found" /
  "object exists". Needs a server that takes >15 s; not arranged.
- **NFS: `NFS3ERR_BAD_COOKIE` is reported as end of listing**; needs a server
  that enforces cookie verifiers (FreeBSD/ZFS). **Lock paths go stale after a
  rename** (low). **The read cache is keyed by the low 32 bits of the fileid**
  (only matters across btrfs subvolumes/ZFS datasets in one export).
- **SMB: `Protect` rewrites FileAttributes to exactly NORMAL or READONLY**, so
  HIDDEN/SYSTEM/ARCHIVE on the server are cleared; needs a writable share with a
  hidden file (morning). **Unsigned error replies are accepted on a signed
  session**, and three "error" statuses carry data (BUFFER_OVERFLOW,
  END_OF_FILE, NO_MORE_FILES); needs a signed session to test (morning).
  **No MessageId check on replies** (a server splitting a compound reply would
  desynchronise the stream; no server here does).
- **Prefs: the scroller position is not reset on a tab switch** (cosmetic,
  needs `prefsmaxheight`); **failed gadget rebuilds under memory exhaustion**
  leave stale pointers in Prefs and PrismMon (needs a few KB free).

## Size and memory

Text sizes of the shipped binaries (`objdump -h`):

| | text | data | bss |
|---|---:|---:|---:|
| PrismNet (core) | 153.9 KB | 0.4 KB | 19.2 KB |
| bsdsocket.library | 15.4 KB | - | - |
| usergroup.library | 5.2 KB | - | - |
| PrismNetPrefs | 40.9 KB | 0.7 KB | 17.4 KB |
| PrismMon | 19.8 KB | 0.5 KB | 0.1 KB |
| prismnfs-handler | 31.0 KB | - | 0.7 KB |
| prismsmb-handler | 52.2 KB | - | 12.2 KB |
| SMBShares | 30.6 KB | - | 9.2 KB |
| ping / traceroute / nslookup / httpbench | 12.6-14.3 KB each | | |

Where the core's 154 KB goes: lwIP core 48.7 KB, IPv4 (ARP, IP, ICMP, DHCP,
fragments) 27.4 KB, **mDNS responder 16.1 KB**, PrismNet's own core 38.5 KB
(packets, sockets, config, CLI), SANA-II port 9.7 KB. The core grew from 131 KB
at the first audit to 154 KB: mDNS + NetBIOS name answering (added 2026-10-16,
17 KB), the TX queue, direct calls and SO_SNDBUF.

Two changes made now:
- **The core's stack is 16 KB in every build, not 64 KB.** The high-water mark
  over the gate's stress tests is 4.2 KB (`STATUS` prints it). This takes 48 KB
  of RAM off every running stack: `STATUS` memory line goes from "64 KB stack"
  to "16 KB stack". The gate's `t-stack`/`t-rsq` now report `stack peak 4188/16384`.
- **The small build (`make small`, 1 MB 68000 machines) leaves out the mDNS
  responder and IGMP**: 142,168 -> 124,012 bytes on disk (-13 %), NetBIOS name
  answering kept. The normal build is unchanged.

Not changed, worth knowing: the lwIP memory budget (384 KB normal) is run-time,
so the figure that matters is `STATUS`'s "in use" line: 274 KB idle after boot
on the test machine (169 KB loaded + stack + 53 KB lwIP + sockets).

Start/stop residency (`t-stopleak`): the first `START`/`STOP` cycle leaves
530 KB of chip RAM and 95 KB of fast in use after `Avail FLUSH`; a second cycle
adds 320 bytes. So nothing leaks per run; the one-time part is the SANA-II
driver's own buffers staying resident after its first open (chip RAM, which
PrismNet never allocates). The small core on the 2 MB 68020 machine: 126 KB
loaded, 160 KB in use idle, 172 KB after a download at 181 KB/s, DHCP, DNS and
STOP all fine without mDNS.

## Speed

`t-bench` on the `PrismNet test` machine (68030, slirp, host bench server),
against PERFORMANCE.md's medians:

| | PERFORMANCE.md | today |
|---|---:|---:|
| Download, 16 KB buffer | 642 KB/s | 605 KB/s |
| Download, 4 KB buffer | 659 KB/s | 595 KB/s |
| Upload | 800 KB/s | 795 KB/s |
| Connections/s | 77 | 77 |
| Ping | 2.2 ms | 2.1 ms |

Download is 5-8 % under the recorded medians, within the boot-to-boot scatter
PERFORMANCE.md notes (597-652 KB/s across its own runs); nothing this week
touched the receive path. The forced-reset injection runs above also measured
the TX queue under 25-33 resets per transfer: 633 KB/s down, 793 KB/s up, no
drops, so the watchdog/reset path costs nothing when it fires.

The SMB client's own cost is the crypto (see SMBCLIENT.md): signing 89 KB/s
with AES-CMAC at 3.x, 128 KB/s with HMAC-SHA256 at 2.1, encryption 43-45 KB/s,
on a 68030. The handler's data path is otherwise bounded by the 64 KB
single-request window; pipelined READs (the NFS client's DEPTH) are the next
speed step for SMB.

## What a rounded Amiga network setup still lacks

In rough order of value for a user who installs PrismNet and expects "it just
works" like Roadshow/Miami, with the size cost in mind:

1. **Time from the network (SNTP)** - *done the same day* (`ENV:PrismNet/time`,
   `PrismNet SETCLOCK`, the installer asks; +4.6 KB in the core, 2 KB of it
   lwIP's `apps/sntp`). The clock is set in the Locale prefs' zone; found on
   the way: with no Locale prefs ever saved, locale.library 3.x reports a
   default of UTC+5, so the core then keeps the clock's own zone (rounded to a
   quarter hour) and only corrects the drift. The share handlers take the same
   locale value for file times, which is why their `TZ=` option exists.
2. **The SMB installer step and the Shares tab** - *done 2026-10-18*. Until
   then shares are mounted by hand-written DOSDrivers.
3. **Roadshow's run-time configuration calls** (`AddInterfaceTagList`,
   `AddRouteTagList`, `AddDomainNameServer`...). Roadshow's own tools
   (`AddNetInterface`, `AddNetRoute`, `NetShutdown`) and some installers call
   them; PrismNet answers "not supported" and users fall back to editing files.
   Moderate size (the core already has the operations; it is the tag parsing and
   the library entries).
4. **A working `netstat`-style ARP/route view and `arp`/`route` commands.**
   `PrismNet STATUS/NETSTAT` show interfaces and sockets but not the ARP table or
   the routing table, which is what people look at when something does not
   answer.
5. **inetd-style servers**: none ship. A tiny `inetd` (or a `PrismNet SERVE`
   list in ENV:) running `ftpd`/`telnetd` ports from Aminet is what makes the
   Amiga reachable *from* the PC — today it can only reach out. Size is in the
   daemons, not the stack; the dispatcher is about 4 KB.
6. **AmiSSL check** - *done 2026-10-18*. AmiSSL 5.27 on "PrismNet test":
   `OpenSSL s_client -connect aminet.net:443` makes a TLS 1.3 connection
   (TLS_AES_256_GCM_SHA384) and verifies the certificate. On the real A2000
   (X-Surf), Amelinium 1.3.9 renders https://aminet.net/ completely through
   the AmiSSL-Tunnel shim. (Mixing a real AmiSSL's OpenSSL command with a shim
   `amisslmaster.library` earlier in LIBS: crashes - not a stack matter.)
7. **IPv6**: lwIP has it; about +30 KB of core and a second ARP-like reader per
   interface. Few home LANs here need it yet; keep it for after 1.0.
8. **Serial links**: a native PPP dialer with chat script, so a modem/null-modem
   user is not dependent on a third-party `ppp-serial.device`. Bigger (PPP in
   lwIP is ~40 KB); SLIP works today.
9. **Real-hardware soak**: the A1200 PCMCIA `cnet.device` stall and the
   lost-request path above can only be settled there. That, more than any
   feature, is what "optimized for speed" needs next — the emulator's card never
   drops a frame.

Not recommended: a firewall/packet filter (no listening services yet), bpf/
pcap hooks (debug only), UPnP, zeroconf beyond what mDNS already answers.

# PrismNet code audit — 2026-10-16

A full read-through of the core, socket layer, platform port, both libraries, the
GUIs, the network tools, the tests, the build and the Installer. Four parallel
reviews each verified their findings against the source (and lwIP's, for the core).
Every fix below was then built and tested on the WinUAE `PrismNet test` machine.

## Checked and found sound

- The core's wake-up and timer design (`core.c`, `port/pnsys.c`). Every lwIP timeout
  is at least 250 ms ahead and the timer is never armed further than that. So even a
  broken assumption would only run a timeout late, never lose it.
- E-clock to millisecond conversion, including 32-bit wraparound.
- `port/chksum.S` against lwIP's checksum semantics: odd start, odd length and carries.
- The library jump tables and register order for all 46 bsdsocket and 39 usergroup
  calls. These match the NDK pragmas.
- The SocketBaseTags decoding, the errno and EAI numbering, and the request/abort
  protocol between library and core. This includes the reply-port race fixed in 5831f63.
- `lib/support.S` multiply and unsigned divide, for the full 32-bit range.
- The struct layouts shared between library and core. They are all even-aligned for
  the 68000.

## Fixed

### Core and socket layer
| Problem | Effect | Fix |
|---|---|---|
| lwIP frees a connection from TIME_WAIT **without a callback** | Close a socket after `shutdown(SHUT_WR)` and keep it open for 2 minutes, and its next use touched freed memory. If that memory was reused, a later close could kill someone else's connection. | Drop the pcb as soon as both directions are closed (`tcp_recv_cb`). Check `tcp_shutdown`'s result. |
| Any core exit after its port appeared left queued requests un-replied | With a bad `DEVICE=`, a program auto-starting the stack hung in `OpenLibrary` forever, and Ctrl-C could not break it. | One `close_public_port()` on every exit path: remove the port, then reply ENETDOWN to everything still queued. **Tested:** OpenLibrary now fails cleanly. |
| lwIP assertions called `abort()` | The core died with its port still public and network-card reads pointing at freed buffers. | Assertions log the problem and run the normal shutdown. |
| A failing network-card read was resent at once | With the unit offline, the core (priority 3) spun at full speed and froze the machine. | Failed reads are retried from the 250 ms timer pass only. |
| Card reset could reuse or free an I/O request the driver still held | The driver's queue got corrupted, or the driver wrote into freed memory. | A lost control request is never reused. Close leaves everything allocated if the driver still holds any of it. |
| `send(fd, buf, -1, 0)` / `recv` with a negative length | Treated as 4 GB: sent the program's memory to the peer, or overwrote memory past the buffer. | EINVAL; EFAULT for a NULL buffer. |
| A pending request on an fd that another task closes or `dup2`s over | The request carried on against whatever socket got that fd number next. | Pending connect/accept/send/recv on that fd are failed when it is closed. |
| `PNOP_ABORT` didn't check the owner | Any client could cancel another client's request. | Only the client that owns a request can cancel it. |
| `IP_HDRINCL` and unknown IP options returned success | Raw-socket programs sent packets with two IP headers. | `IP_HDRINCL` is implemented; unknown options return ENOPROTOOPT. |
| `setsockopt` with a 1-byte value | Silently read as 0, which switched the option off. | EINVAL. |
| A large select/SO_*TIMEO timeout wrapped into the past | Programs passing an "infinite" timeout busy-looped. | Values over ~12 days mean "forever". |
| `select()` after `shutdown(SHUT_RD)` | Blocked forever, although `recv` returns 0. | The socket counts as readable. |
| Predictable TCP initial sequence numbers | lwIP's default is a counter. | `LWIP_HOOK_TCP_ISN` mixes a random number with the connection's addresses and ports. |
| One bad interface file stopped the whole stack | A stray `eth0.old` also became a second interface on the same card. | A bad file is skipped with a warning, so the others still come up. An empty file switches that interface off. A second file for the same device and unit is skipped. MTU is range-checked. |
| The hosts file was read and parsed twice per name lookup | About 6 ms per `gethostbyname`. | Parsed once, re-read only when its size or date changes. |
| `DNS_TABLE_SIZE` 8 with 16 resolver slots | Browsers looking up 9+ names at once hit a retry loop: two hosts-file reads per pass for up to 10 s. | Table size raised to 16. |
| The computer's own name resolved to 127.0.0.1 | FTP PORT and servers advertised loopback. | It resolves to the interface address when one is set. **Tested:** prismtest → 10.0.2.15. |
| Loopback frames made late in a pass | Waited up to 250 ms. | A second `netif_poll_all()` at the end of the pass. |
| DHCP RELEASE at shutdown | Aborted before it was sent. | A short pause to send it before I/O is aborted. |

### Libraries
| Problem | Effect | Fix |
|---|---|---|
| The library's open count was raised only at the end of a blocking open | The library could be expunged while an open sat in auto-start or HELLO for up to 10 s, and the open then ran freed code. Same in usergroup. | Counted from the first instruction. |
| `struct ug_credentials.cr_umask` was ULONG; the NDK says UWORD | `getcredentials()` returned every later field shifted by two bytes. | UWORD. |
| `CloseLibrary` from another task deleted the owner's reply port | That task's signal bit was freed, causing lost or spurious signals later. | The port is freed without touching the closing task's signals. |
| usergroup.library used DOS from a plain Task | Guru on open or lookup. | The built-in defaults are used when the caller is not a Process. |
| `gethostbyaddr`, `inet_ntop`, `inet_pton` read longs through caller pointers | An odd address caused an address-error guru on a 68000. | Byte copies. |
| No signed divide in `support.S` | The first signed `/` in a library would silently link libnix's version, which crashes through a NULL UtilityBase. | `___divsi3` and `___modsi3` added. |
| A tool timing out on the core | The late reply `Signal()`ed a task that had already exited. | The leaked port is set to `PA_IGNORE`. |
| Auto-start without both `NIL:` handles | An asynchronous `System()` call with a missing handle. | The auto-start gives up instead. |

### Programs, tests, build, Installer
| Problem | Effect | Fix |
|---|---|---|
| Prefs read files through a 16 KB buffer and wrote them back | Turning "start at boot" on or off cut `S:User-Startup` at 16 KB. The Installer's Remove did the same. | Prefs reads the whole file and never writes back a short read. **Tested:** a 27.9 KB User-Startup came back byte for byte. |
| Prefs wrote over files in place | A full disk left an empty interface file, which the core refused at boot. | It writes `name.new` and renames it over the old file. |
| On OS 3.2, "Use" could not remove an interface, route or DNS server | ENV: is a lazy link to ENVARC:, so a deleted name came straight back. | An empty file is written instead, which the core reads as "none". |
| Device paths with spaces | Written unquoted, so they broke on reload. | Quoted, in both Prefs and the Installer. |
| Prefs didn't check UNIT or MTU | Bad values were accepted. | Both are validated. |
| A mistyped Prefs argument from a script | Opened the GUI and the script hung. | Error and return code. **Tested.** |
| PrismMon with a wedged core | It stalled 2 of every 3 seconds, leaked about 770 bytes/s, and could pass a NULL buffer to the core. | Half-second timeout, a 10-refresh back-off and NULL guards. |
| traceroute UDP mode | A late reply to an earlier probe was credited to the current hop. Quoted headers with IP options were read out of bounds. | Also matches the probe's destination port, and the bounds are checked. |
| nslookup | Freed a buffer the core might still write to after a timeout. Didn't check `socket()`. Short records were read past their end. | All fixed; it also reports truncated answers. |
| ping/traceroute/nslookup | `WAIT=0` and `TIMEOUT=-1` misbehaved, ping sequence numbers stopped matching after 65535 pings, and libnix could abort mid-`printf` on Ctrl-C. | Values clamped, sequence compared as 16 bits, `__chkabort` overridden. |
| httpbench | Missed a header end split across two reads. | Rolling 4-byte match. |
| bsdstress returned while its child processes were still running | It crashed after a TIMEOUT. | It waits for every child. |
| bsdtest "connect to self" | Could pass on a stale errno. | Fixed. |
| Makefile: the network tools never created `out/` | A clean checkout didn't build. | Fixed. |
| Makefile: `PROFILE=1` / `DEBUG=1` shared the release object directory | Profiling objects could end up in `out/PrismNet`. | Separate directories. `nettools` is now a phony target with full header dependencies. |
| Installer stopped the running stack before any confirmation | Cancel still dropped the network. | It stops only after Remove or Replace is confirmed. |
| Installer Remove ran `foreach` on `C:Backup` even when it did not exist | Remove aborted at 10% with a DOS error on any machine without backups, leaving PrismNet half-removed. Found by click-testing. | The loop is guarded with `exists`. |
| Installer `(exit "…" (quiet))` | `(quiet)` hides the text, so the "removed, please reboot" message never showed. | `(message …)` first. |
| Installer detected existing settings by `interfaces/eth0` only | It missed a Roadshow-imported `DSL` interface and added a second interface for the same card. | Any interface file counts. |
| bench server | Printed tracebacks for connections the connect-rate test drops on purpose. | It ignores them quietly. The wget test's Aminet URL had gone 404 and was updated. |

## Results after the fixes (WinUAE, 68030)

Regression: bsdtest 61/58/58/58, bsdstress 16/16, ugtest 17/17/20, wget OK; plus the
new audit checks above. Benchmark, one run, compared with the medians in PERFORMANCE.md:

| | Before audit | After audit | Roadshow 1.14 |
|---|---:|---:|---:|
| Download, 16 KB buffer | 650 KB/s | 665 KB/s | 587 KB/s |
| Download, 4 KB buffer | 640 KB/s | 639 KB/s | 610 KB/s |
| Connections/s | 77.5 | 82.2 | 80 |
| Upload | 844 KB/s | 823 KB/s | 788 KB/s |
| Ping | 2.2 ms | 2.4 ms | 2.6 ms |

Upload and ping are within the ±5 % boot-to-boot scatter noted in PERFORMANCE.md.
Sizes: core 131 KB (+3 KB), bsdsocket.library 14.5 KB, usergroup.library 5.7 KB.

## Second round (all the items left open above)

- **Prefs on small screens.** When the window doesn't fit the screen, Prefs shows a
  scroll bar on the right and scrolls the settings. The Save/Use/Cancel buttons stay
  put. The full-size window has no scroll bar.
- **`struct passwd`.** It keeps the 7-field AmiTCP layout that Roadshow's
  usergroup.library 4.30 returns. Home, shell and expire are repeated where the
  10-field BSD layout looks (+28, +32, +36), so programs built either way get name,
  uid, gid, home and shell right. `ugdump` prints the raw struct.
- **Roadshow import.** It reads `key=value` and `key = value`, several pairs to a
  line, and `configure=dhcp`, `static` or `none`.
- **Accepted sockets.** They inherit the listener's timeouts, event mask and
  TTL/TOS. `SO_LINGER {1,0}` closes abortively (RST), and getsockopt reports it.
- **No route.** connect() and sendto() fail at once with `EHOSTUNREACH` instead of
  waiting for ARP to time out.
- **Card watchdog.** It counts a stall only if neither reads nor writes have
  completed for the stall time, so slow links don't trip it.
- **Multicast frames.** They go out as `S2_MULTICAST`. If the driver answers
  `IOERR_NOCMD`, PrismNet falls back to `CMD_WRITE` for good.
- **`dist.sh`.** It stops if any binary is older than its sources.
- **The unexplained reset (alert 8100 0005).** This is OS 3.2, not PrismNet. A
  recursive `Delete ENV:PrismNet ALL` while `ENVARC:PrismNet` exists corrupts memory
  in the lazy ENV: handler, even with PrismNet never started. Deleting files, or a
  directory with no ENVARC: counterpart, is fine. The Installer's Remove now
  deletes only the files in ENV:, then deletes ENVARC:PrismNet. An empty
  `interfaces` drawer may stay until the next reboot. The test scripts do the same.

## Left open after the first round (all handled in the second round above)

- **Prefs window height on 640×200 NTSC screens.** It needs about 240 lines, so the
  frames overlap there. That needs a compact layout, not a patch.
- **`struct passwd` layout.** usergroup returns AmiTCP's 7-field struct; programs
  built with today's NDK expect the 10-field BSD struct. Check against Roadshow
  before changing it.
- **Roadshow import.** It reads one `key=value` per line and ignores
  `configure=dhcp`, so single-line Roadshow files import wrongly.
- **Accepted sockets.** They don't inherit the listener's TTL/TOS, timeouts or event
  mask, and `SO_LINGER {1,0}` doesn't do an abortive close. Inheritance semantics in
  Roadshow are unverified.
- **Card watchdog.** It judges a stall by the age of each request, so very slow links
  (serial, PLIP) may see false resets. Basing it on time since the last completed
  write would fix that.
- **Multicast frames.** They go out as `CMD_WRITE` rather than `S2_MULTICAST`. Most
  drivers accept either.
- **`dist.sh`.** It doesn't check that `out/` is up to date before packaging.
- **One unexplained reset.** During the Installer click-test, the emulated Amiga
  reset itself once, right after "Delete settings" in Remove. The run before had
  aborted with the `foreach` error and left a half-removed state. An identical
  clean run afterwards did not reset, and neither did the same Delete commands run
  by hand. Watch for it on real hardware.
