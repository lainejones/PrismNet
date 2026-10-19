# PrismNet — project notes

Open-source TCP/IP stack suite for AmigaOS m68k (lwIP core + own
`bsdsocket.library`). Design: `docs/DESIGN.md`.

## Rules
- `lwip/` is vendored **unmodified** (lwIP 2.2.1). Configure via
  `port/include/lwipopts.h` / `arch/cc.h`; never patch lwIP sources. If a
  change is unavoidable, record it in `lwip/VERSION`.
- GPL-3.0-only. Do **not** copy the NDK/SDK `sana2.h`, AmiTCP SDK headers or any
  Roadshow material into the repo; write definitions from the public spec
  (`port/include/prismnet/sana2defs.h`).
- lwIP is `NO_SYS=1` and owned by ONE task. The SANA-II CopyTo/FromBuff hooks
  may run in driver/interrupt context: they only copy bytes, never call lwIP.

## Toolchain quirks hit here
- **Don't use `m68k-amigaos-strip`** — it corrupted pntest's relocs even as a
  single file (crash in libnix `__INIT_LIST__` before `main`). Strip at link
  time with `-s` (the Makefile does).
- This NDK's `proto/cardres.h` includes a missing `clib/card_protos.h`; use
  `clib/cardres_protos.h` + `inline/cardres.h`.
- `OpenDevice()` only searches `DEVS:`; SANA-II drivers are in
  `DEVS:Networks/`. `sanaif_open()` tries `DEVS:Networks/<name>` for bare names.

## Lessons from the bsdsocket work (don't repeat)
- The core walks `core.pending` and re-queues still-pending requests: take the
  list first (see `service_pending`). Re-adding to the list being walked looped
  forever, and at core priority 3 that froze the whole machine (only showed with
  2+ concurrent clients: AWeb, bsdstress - never with bsdtest).
- The core opens `bsdsocket.library` itself at start (no client) so concurrent
  first `OpenLibrary` calls don't race ramlib's load. `pn_open` recognises the
  core by its port's task.
- The library is built `-nostartfiles`: libnix's `__mulsi3`/`__udivsi3` call
  `UtilityBase` (opened by libnix startup) - our own helpers live in
  `lib/lib_start.S`. Check the link map: no libnix/libstubs objects.
- Core stack: `__stack = 65536` (Run gives ~4 KB).
- lwIP DNS allows few outstanding lookups; resolver re-tries instead of failing.
  A failed lookup is also asked once more (slirp/routers SERVFAIL cold queries and
  lwIP reports that like NXDOMAIN) - `PnReq.pad` is the retry flag.
- Roadshow's `EAI_*` codes are NEGATIVE (netdb.h); `gai_strerror` takes its code in a0.
- Socket events are edge-triggered: `sock_check_events()` reports a condition once and
  re-arms it when it stops holding; setting `SO_EVENTMASK` reports what holds now.

- `call()` returns -1 when the core is absent: never store that as a client handle
  (fixed in `pn_open`; `AUTOSTART=NO` must make `OpenLibrary` fail).
- Auto-start runs `SystemTags` from inside `OpenLibrary` (exec holds Forbid; Wait/Delay
  break it). Only for DOS processes; plain tasks just get no library.

- **OS 3.2 `ENV:` is lazy**: the 3.2 Startup-Sequence does `MakeLink RAM:ENV to ENVARC:`
  and the RAM disk copies a file in only when it's opened BY NAME. Right after boot
  `ExNext` on `ENV:PrismNet/interfaces` lists NOTHING. Core (`config.c`) and Prefs therefore
  list names from ENV: *and* ENVARC: and open each via ENV: (fallback ENVARC:). Never rely
  on listing an ENV: drawer. (Found when Prefs opened empty and auto-start failed.)

## Performance work (2026-10-16, see docs/PERFORMANCE.md)
- The emulated 030 (`cpu_speed=real`) is slow: an E-clock read is ~40 us, a library
  call round trip ~290 us. Measure with `httpbench ... CALLS=n` and `make PROFILE=1`.
- `sys_now()` is a lazily refreshed clock (first call after each `Wait` reads it) and
  the core loop skips time keeping on passes that weren't timer wakeups. Anything new
  that sets a request deadline must go through `core_set_deadline()`, or the timer is
  not re-armed for it. A new lwIP timeout shorter than 250 ms would also break the rule.
- The library caches the core's port in the master base (set by the core's own
  `OpenLibrary`, cleared by its `CloseLibrary` under Forbid right after `RemPort`).
  Keep that order in the core's shutdown.
- Benchmarks scatter ±5 % between boots: compare medians of several boots, and A/B
  against Roadshow in the same session (`t-rsbench`).
- WinUAE NE2000/`cnet.device` sometimes stops completing I/O under load (more often
  with the faster stack). `sanaif_watchdog()` resets it; use `make diag` + `pndiag`
  (passive, doesn't wake the core) and a background watcher to investigate hangs.

- **slip.device 37.4 (Rhialto, from Commodore's SANA-II example) takes the unit from
  A3 in its AbortIO**; exec's `AbortIO()` sets only A1/A6. Plain `AbortIO()` aborted
  nothing, the old core hung in `WaitIO()` after STOP said "stopped", and its 34 reads
  ate the next 34 packets (SLIP dead ~1 min after a restart, found 2026-10-17). Use
  `abort_io()` in sanaif.c (A3 = io_Unit) and never wait unbounded for an abort.
  `out/diag/s2stat` prints a unit's S2_GETGLOBALSTATS; counters that survive a
  STOP/START mean the old opener never closed the device.

## Testing
- **`python tools/gate.py` must exit 0 before any deploy to real hardware.** It
  builds, deploys to `PrismNet test`, boots once per script below and checks the
  logs (PASS/FAIL lines, stack < half, 0 failed allocations outside the 8-task
  bsdstress load, markers); a network-only failure is retried once = FLAKY.
  Result in `docs/gate-last.txt`, logs in `build/gate/<test>/`. A new check goes
  in its `TESTS` table. The scripts it runs (added 2026-10-17 after a guru
  that shipped to three machines): `Execute DH0:PrismNet/t-pingstop` (ping,
  STOP, restart; a guru shows as a harness timeout), `t-stack` (STATUS must
  show "stack used at most" well under the stack size, today ~4.2 KB of 64 KB),
  `t-rsq` (Roadshow API incl. GetNetworkStatistics), `t-config`. Then on one
  real machine: start, ping, STOP, restart, a download, STATUS.
- The core's stack comes from `__stack` and only works because the Makefile
  links libnix's swapstack.o (`-Wl,-u,___stkinit`). Without it the core ran on
  the 4 KB default stack (peak use is ~4.2 KB) and corrupted memory: 8100 0005
  at exit. The core now refuses to start with under 16 KB.
- Build: `wsl -e bash -lc 'cd /mnt/c/projects/PrismNet && PATH=/opt/amiga/bin:$PATH make'`
- WinUAE config **`PrismNet test`** (own DH0 `C:\Amiga\PrismNet-test`, copy of
  the clean 3.2.3 machine) with NE2000 PCMCIA on slirp → `cnet.device`; X-Surf
  (`x-surf.device`) also works — see README for the config lines. Use the
  winuae MCP `deploy` + `run_cli_test`:
  `DH0:PrismNet/pntest DEVICE=cnet.device PING=10.0.2.2 LOOKUP=aminet.net GET=aminet.net/`
- The WinUAE config needs `bsdsocket_emu=false` or WinUAE's own built-in
  bsdsocket.library answers instead of PrismNet (check the id string!).
- winuae MCP `run_cli_test` breaks on lines with their own redirection (`Run >NIL:`):
  write an AmigaDOS script to `DH0:PrismNet/t-*` and `Execute` it.
- GUI and tool tests: `t-tools` (NETSTAT/LOG under load), `t-prefs` (Prefs headless:
  round trip, Roadshow import from `DH0:PrismNet/rs/`, validation), `t-mon` /
  `t-prefsgui` / `t-icons` are `run_gui_test` screenshots. Buttons can't be
  clicked from here - GUI behaviour needs Laine. Click-tested 2026-10-15: all OK
  except Prefs Save (ENVARC + S:User-Startup block), still untried.
- Config tests: `t-config` (DHCP+hosts, static, auto-start) and `t-config2`
  (AUTOSTART=NO, no config, re-enable); configs in `DH0:PrismNet/cfg/` (also
  `test/amiga/cfg`); `C:PrismNet` on the test drive is what auto-start launches.
- bsdsocket tests: `Execute DH0:PrismNet/t-bsdtest` / `t-stress` / `t-wget`; AWeb via
  `run_gui_test` + `t-aweb` (AWeb needs ~60 s to start on the emulated 030).
- wget needs `usergroup.library` - the test machine now runs ours (`t-usergroup` tests it).
- Building clients against usergroup: libnix ships its OWN internal `inline/usergroup.h`
  (uses `lss->`) that shadows the NDK one, and `clib/usergroup_protos.h` clashes with
  libnix `grp.h` - include `<../ndk-include/inline/usergroup.h>` (see ugtest.c).
- Libraries: initialise `SysBase`/`DOSBase` globals (`= NULL`) so they aren't COMMON,
  else the linker pulls libnix auto-open stubs.
- vamos can run the tools only up to `OpenDevice` (no devices there).
- Don't test on real machines whose NetHarness/a314 link depends on the stack
  being replaced.
