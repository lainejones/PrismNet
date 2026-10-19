# Developing PrismNet

How the source is laid out, how to build it, test it and make a release.
The architecture and design decisions are in [DESIGN.md](DESIGN.md); the
user manual is in [manual/](manual/README.md).

## Layout

| Path | What |
|---|---|
| `lwip/` | lwIP 2.2.1, unmodified (`src/` only); see `lwip/VERSION` |
| `port/include/lwipopts.h`, `port/include/arch/cc.h` | lwIP configuration for AmigaOS (`PN_SMALL`: the small core) |
| `port/pnsys.c` | `sys_now()` from the E-clock, wakeup timer, RNG |
| `port/sanaif.c` | lwIP netif over a SANA-II device (cooked mode, CopyTo/FromBuff hooks), built-in SLIP |
| `port/include/prismnet/sana2defs.h` | SANA-II definitions written from the public spec (no NDK/SDK header needed) |
| `core/` | the core task `PrismNet`: request dispatch, BSD socket semantics on lwIP (`csock.c`), resolver, `ENV:PrismNet` config (`config.c`), the commands |
| `lib/` | `bsdsocket.library` and `usergroup.library` (per-opener bases), shared 68000 helpers in `support.S` |
| `port/include/prismnet/pnmsg.h` | message protocol between library and core |
| `port/pnclient.c` | tools' direct line to the core (STATUS/LOG/NETSTAT) |
| `tools/prismprefs.c` (+ `prefsshares_*.c`, `prefsservices_*.c`), `tools/prismmon.c`, `tools/tabs.c` | the GUIs (GadTools, hand-drawn tabs); `icons/` their `.info` files |
| `tools/net/` | ping, traceroute, nslookup, httpbench |
| `tools/nfs/`, `tools/smb/`, `tools/netfs/` | the NFS and SMB filesystem handlers (shared core in `netfs/`), SMBShares, ShareUnmount |
| `tools/srv/` | inetd, ftpd, telnetd |
| `tools/bsdtest.c`, `tools/bsdstress.c`, `tools/ugtest.c`, `tools/rsquery.c`, `tools/rscfg.c` | API, concurrency, usergroup and Roadshow-API tests (plain AmiTCP clients) |
| `tools/mkguide.py` | `docs/manual/*.md` to `PrismNet.guide` |
| `config/` | commented sample `ENVARC:PrismNet` settings |
| `dist-src/` | the installer script, the package README, mount-file samples, release icons |
| `test/amiga/`, `test/host/`, `tools/gate.py` | the test gate (WinUAE) and the host-side server tests |

## Build

Bebbo amiga-gcc under WSL:

```
wsl -e bash -lc 'cd /mnt/c/projects/PrismNet && PATH=/opt/amiga/bin:$PATH make'
```

Output in `out/`. Default CPU is 68000.

| Target | Builds |
|---|---|
| `make` | everything: core, libraries, GUIs, tools, handlers, servers |
| `make small` | the small-memory core into `out/small/` (shipped as `Tools/Small/PrismNet`) |
| `make CPU=-m68020`, `make core020` | a 68020+ build (not shipped: no faster, see PERFORMANCE.md) |
| `make DEBUG=1` | lwIP debug output |
| `make PROFILE=1` | a core that logs where its main loop spends time |
| `make diag` | a core plus the `pndiag` probe for hang hunting |
| `make cryptotest` | the SMB crypto against the MS-NLMP/MS-SMB2 test vectors |

Benchmarks against Roadshow and what made PrismNet faster:
[PERFORMANCE.md](PERFORMANCE.md).

## Testing

**The gate.** `python tools/gate.py` boots the WinUAE machine
*PrismNet test* and runs the Amiga-side tests in `test/amiga/` (stack,
Roadshow queries, Roadshow configuration, config files, stress, ping/stop).
`--only t-x` runs one, `--no-retry` shows flaky ones.

**The WinUAE machine.** *PrismNet test* (OS 3.2.3 A1200/030, `DH0:` =
`C:\Amiga\PrismNet-test`) has a slirp-backed NIC. Add one of these pairs to
the `.uae`:

```
ne2000pcmcia_rom_file=:ENABLED                      # use cnet.device
ne2000pcmcia_rom_options=inserted=true,netmode=slirp
xsurf_rom_file=:ENABLED                             # use x-surf.device
xsurf_rom_options=netmode=slirp
```

Slirp provides DHCP (10.0.2.15), gateway/ping target 10.0.2.2, DNS
10.0.2.3, and NAT to the internet. The config must also have
`bsdsocket_emu=false`, or WinUAE's own built-in bsdsocket.library answers
`OpenLibrary` and PrismNet is never used.

**Shares.** The NFS and SMB tests (`test/amiga/t-nfs`, `t-smb*`, the mount
files in `test/amiga/nfs` and `smb`, and the `nh-shares-*` scripts) use example
servers: `192.168.1.10` (a NAS with an export and share `amigatest`, user
`smbuser`) and `192.168.1.11` (a guest share `public`). Put your own servers'
addresses in before running them.

**TCP sequence wrap.** `SetEnv PrismNet/testisn 7FF00000` before the core starts
makes every connection begin at that sequence number, so a transfer crosses 2^31
after 1 MB (the point where some stacks on the ZZ9000 stalled: BlitterStudio
zz9000-firmware issue 29). PrismNet 0.6.1 on the A4000 passed a 100 MB upload and
a 16 MB download across it at full speed, and a 1 GB single-connection upload.
Remove the variable afterwards: it is a test aid, not a setting.

**Servers.** `python test/host/ftptest.py <amiga> [password]` and
`python test/host/telnettest.py <amiga> [user] [password]` run against a
machine with the servers on; see [SERVERS.md](SERVERS.md) for the accounts
they need. `test/host/pncrypt.py` makes password hashes.

**pntest**, a developer self-test, runs lwIP alone on a driver:

```
pntest DEVICE=cnet.device [UNIT=0] [IP=a.b.c.d NETMASK=.. GATEWAY=..] [DNS=..]
       [PING=10.0.2.2 COUNT=4] [LOOKUP=name] [GET=host/path] [TIME=secs]
```

## The manual

The user manual is Markdown in `docs/manual/`, one file per chapter
(`README.md` is the contents page). GitHub shows it as it is;
`python tools/mkguide.py docs/manual PrismNet.guide` makes the AmigaGuide
version, and `dist.sh` puts it in the package. Keep lines in code blocks
under 77 columns, and link between chapters with relative links
(`[Servers](10-servers.md)`, `[...](09-shares.md#smb-options)`): the
converter turns those into buttons.

## Release

`./dist.sh` (after `make` and `make small`) assembles `dist/PrismNet/`,
laid out like SYS: (`C`, `Libs`, `Prefs`, `Tools`, `Storage`, `Docs`, each
with a drawer icon), with `Install_PrismNet`, the README, `PrismNet.guide`
and their icons. It refuses to package a
binary older than its sources. Icons are committed `.info` files
(`tools/icons/regen_dist_icons.sh` makes them).

**Installer test matrix** (all passed on 0.6.1, 2026-10-19, WinUAE *PrismNet
install*, driven over NetHarness): (1) everything yes: both share clients, both
servers, DHCP, start when needed, network time, boot entry, start now; (2) another
stack's `bsdsocket.library` in `LIBS:`, Cancel: nothing changed; (3) the same,
Replace, then everything no: keep your own `ping`, no shares or servers, a fixed
address, no autostart, no clock, no boot entry, start later; (4) update keeping
the settings: the network questions are skipped; (5) Remove keeping the settings:
the other stack's library comes back from `C:Backup`, your `ping` stays; Remove
deleting them after (1): only empty drawers stay (lazy `ENV:`); (6) a 2.5 MB
machine: the small-core question, answered Standard; New settings rewrites the
files. A stand-in for "another stack" is any `LIBS:bsdsocket.library` (and
`C:ping`) without the word PrismNet in it.

The `.lha` is built on an Amiga from that drawer
(`lha -r a PrismNet-<ver>.lha PrismNet PrismNet.info`) so protection bits are
real. Installer tests run on the WinUAE config *PrismNet install* (a fresh
copy of the clean 3.2.3 machine).
