# PrismNet — design spec

PrismNet is an open-source (GPL-3.0) TCP/IP stack suite for classic AmigaOS
(m68k), a sibling of PrismRTG. It provides a drop-in, AmiTCP-compatible
`bsdsocket.library` over any SANA-II network driver, plus Prefs and Monitor
GUIs and the usual CLI tools.

## Goals

- **Drop-in compatibility.** Programs that run on AmiTCP / Roadshow / Miami
  (AWeb, IBrowse, Amelinium, smb2fs, wget, YAM, FTP/telnet clients, AmiSSL
  users) run unmodified. The API surface is the AmiTCP 4 `bsdsocket.library`
  LVO table; the compatibility list in `docs/COMPAT.md` (to be built from
  a314bsd's experience) is the acceptance spec.
- **Runs everywhere.** OS 3.1+ (3.1 and 3.2), plain 68000 upward,
  usable on a 2 MB machine. Faster code paths (asm checksum, 020+ builds)
  are optional extras, never requirements.
- **Full suite, not just a library.** Prefs GUI, Monitor GUI, CLI tools,
  installer that respects an existing stack.
- **Clean licence.** All PrismNet code is GPL-3.0-only. The protocol core is lwIP
  (BSD-3, vendored unmodified in `lwip/`). No code from closed stacks; the
  Amiga Inc. SANA-II / AmiTCP SDK headers are *not* copied — PrismNet carries
  its own definitions written from the public specifications.

## Non-goals (for 1.0)

- IPv6 (lwIP supports it; enable after 1.0).
- Being a router/firewall. Single host, possibly several interfaces.
- Kernel-level BSD 4.4 compatibility beyond what Amiga programs use.

## Architecture

```
 applications (AmiTCP API)
        │  OpenLibrary("bsdsocket.library", 4) — per-task SocketBase
 ┌──────▼─────────────────────────────┐
 │ bsdsocket.library  (front-end)     │  socket table, errno/h_errno,
 │                                    │  WaitSelect + signal masks,
 │                                    │  ObtainSocket/ReleaseSocket, tags
 └──────┬─────────────────────────────┘
        │  Exec messages (request/reply), one stack task
 ┌──────▼─────────────────────────────┐
 │ PrismNet core task                 │  owns ALL lwIP state (NO_SYS=1):
 │   lwIP raw API: TCP UDP RAW ICMP   │  timers, DHCP, DNS, ARP
 │   netif drivers ↓                  │
 └──────┬──────────────┬──────────────┘
   SANA-II netif   a314 proxy backend (later: today's a314bsd)
   (Ethernet, PPP/SLIP via lwIP, …)
```

Key decisions:

1. **lwIP runs `NO_SYS=1` inside one dedicated core task.** Only that task
   ever touches lwIP. The library functions run on the caller's task, build a
   request message, `PutMsg()` it to the core task and `Wait()` for the reply
   (or for `SIGBREAKF_CTRL_C` / the caller's WaitSelect signals). This is the
   Exec equivalent of lwIP's `tcpip_thread`, but without lwIP's netconn /
   sockets layers: PrismNet implements the BSD-socket semantics itself on top
   of the raw API, which keeps the core small and lets us match AmiTCP
   behaviour exactly (errno values, `sin_len` sockaddr, `WaitSelect`).
2. **Per-task SocketBase from day one.** Every `OpenLibrary()` returns a new
   base holding that task's socket table, errno pointer/size, h_errno,
   signal masks and syslog settings. (Lesson from AmiSSL-Tunnel/a314bsd: a
   shared base breaks concurrent clients.)
3. **SANA-II in cooked (non-raw) mode.** The netif strips the Ethernet header
   on transmit (dest MAC → `ios2_DstAddr`, ethertype → `ios2_PacketType`)
   and rebuilds it on receive, queuing separate reads per ethertype (IPv4,
   ARP; IPv6 later). This works with every SANA-II driver, unlike raw mode.
   Buffer management uses the standard `S2_CopyToBuff`/`S2_CopyFromBuff`
   hooks; receive copies straight into a pre-allocated pbuf, transmit copies
   straight out of the pbuf chain (pbuf held with `pbuf_ref` until the write
   completes). DMA hooks (`S2_DMACopy*`) are a later optimisation.
4. **Time.** `sys_now()` comes from `ReadEClock()`; one `timer.device`
   request wakes the core task for the next lwIP timeout
   (`sys_timeouts_sleeptime()`).
5. **Configuration is plain text** in `ENVARC:PrismNet/` (copied to
   `ENV:PrismNet/` at start), so it can be edited by hand, by the Prefs GUI,
   or by scripts. See "Configuration" below.

## Suite components

| Component | What |
|---|---|
| `LIBS:bsdsocket.library` | AmiTCP-compatible API front-end |
| `LIBS:usergroup.library` | AmiTCP user/group API (`getuid`, `getpwnam`...); many ports (e.g. wget) refuse to run without it |
| `C:PrismNet` | the core task: `PrismNet START\|STOP\|ONLINE\|OFFLINE\|STATUS` (also started by the library on first open if configured) |
| `SYS:Prefs/PrismNet` | **Prefs GUI** (GadTools; OS 3.1+) |
| `SYS:Tools/PrismMon` | **Monitor GUI**: online/offline, per-interface stats, connection list, log viewer (grows out of a314bsd's NetBridge) |
| CLI tools | `pnconfig` (ifconfig-like), `ping`, `netstat`, `host`, `traceroute` |
| `pntest` | developer self-test: lwIP alone (raw API) doing ping/DNS/HTTP over one SANA-II device |
| Installer | Amiga Installer script; backs up an existing stack's `bsdsocket.library` (Roadshow/AmiTCP/Miami/a314bsd) |

### Prefs GUI (pages)

- **Interfaces** — list; add/edit: SANA-II device (scanned from
  `DEVS:Networks`), unit, MTU override, enabled; "Test" button opens the
  device and shows hardware address / MTU / link speed.
- **Addressing** — per interface: DHCP or static (IP, netmask, gateway).
- **DNS & names** — DNS servers, search domain, host name, `hosts` editor.
- **Startup** — start at boot (adds/removes the line in `S:User-Startup`),
  go online at start, log level.
- **Import** — read an existing Roadshow (`DEVS:NetInterfaces`,
  `DEVS:Internet/name_resolution`, `routes`) or AmiTCP (`AmiTCP:db/`)
  setup.
- Buttons: Save / Use / Cancel (classic Prefs semantics: ENVARC vs ENV).

## Configuration (`ENV:PrismNet/`)

```
interfaces/eth0      DEVICE=cnet.device  UNIT=0  CONFIG=DHCP  [MTU=1500]
                     # static: CONFIG=STATIC ADDRESS=192.168.50.40 NETMASK=255.255.255.0
routes               DEFAULT 192.168.50.1
dns                  NAMESERVER 192.168.50.1
                     DOMAIN lan
hostname             amiga
hosts                (standard hosts format)
startup              ONLINE=YES LOG=INFO
time                 SERVER=DHCP|host [SETCLOCK=YES]   # SNTP; the file's presence switches it on
```

## Testing

- **WinUAE**: config `PrismNet test` = clean OS 3.2.3 A1200/030 with
  an NE2000 PCMCIA card on slirp (`cnet.device`) or an X-Surf (`x-surf.device`);
  config lines in the README. Slirp gives DHCP (10.0.2.15),
  gateway/ICMP echo at 10.0.2.2, DNS at 10.0.2.3, NAT to the internet.
- **Real hardware** via NetHarness (TCP) and the A314 harness — note the
  harness itself needs a TCP stack, so PrismNet must not replace the stack a
  harness is using until it is stable; test on a second interface or boot.
