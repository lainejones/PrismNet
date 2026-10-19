# PrismNet performance

Measured 2026-10-16 on the WinUAE config `PrismNet test` (68030, `cpu_speed=real`,
NE2000 PCMCIA on slirp via `cnet.device`), against `tools/bench/benchserver.py` on the
host (10.0.2.2:8080/8081). Same machine, same driver, same server for all three stacks.

    httpbench 10.0.2.2 PORT=8080 PATH=/4m COUNT=3 CONNECTS=100 UPLOAD=2048
    httpbench 10.0.2.2 PORT=8080 PATH=/4m COUNT=2 BUFSIZE=4096
    ping 10.0.2.2 COUNT=10
    httpbench 10.0.2.2 PORT=8080 COUNT=0 CALLS=2000      (per-call cost)

Single runs scatter by ±5 % between emulator boots (a boot tends to be "fast" or
"slow" throughout), so the table gives the **median** of all runs: 10 for 0.4 before,
14 for after, 6 for Roadshow 1.14.

| Metric                    | PrismNet 0.4 before | PrismNet after | Roadshow 1.14 |
|---------------------------|--------------------:|---------------:|--------------:|
| Download, 16 KB buffer    |          601 KB/s   |   **650 KB/s** |     587 KB/s  |
| Download, 4 KB buffer     |          554 KB/s   |   **640 KB/s** |     610 KB/s  |
| Connections/s             |              68     |       77.5     |    **80**     |
| Upload                    |          732 KB/s   |   **844 KB/s** |     788 KB/s  |
| Ping RTT                  |          4.3 ms     |     **2.2 ms** |     2.6 ms    |
| Numeric gethostbyname     |          3.5 ms     |     **0.4 ms** |     0.6 ms    |
| One library call (getsockopt) |       814 µs    |       292 µs   |   **~130 µs** |
| Memory used by the running core | 504 KB        |       547 KB   |       -       |

## Real hardware (2026-10-16)

**A2000**, 68030, X-Surf (first generation, `x-surf.device` 1.16), 100 Mbit LAN,
against `benchserver.py` on a PC. Roadshow 4.364 measured on the same machine the
same evening.

| Metric                  | PrismNet before | PrismNet now | Roadshow 4.364 |
|-------------------------|----------------:|-------------:|---------------:|
| Download, 16 KB buffer  |      675 KB/s   |   **845 KB/s** |   880 KB/s   |
| Download, 4 KB buffer   |      640 KB/s   |     670 KB/s |        -       |
| Upload                  |      720 KB/s   |   **719 KB/s** |   640 KB/s   |
| Connections/s           |        122      |    **127**   |      124       |
| Ping to the router      |       3.8 ms    |   **3.2 ms** |     3.5 ms     |
| One library call        |      322 µs     |      322 µs  |   **131 µs**   |
| bsdtest                 |       58/58     |      58/58   |      55/58     |

The profiling build showed where a received frame's time went: lwIP input about
0.8 ms, the checksum 0.33 ms at only 4.5 MB/s, and an ACK for every 1.3 frames at
**0.7 ms each** inside `x-surf.device`. What helped, in order:

1. **Longword-aligned packets** (`ETH_PAD_SIZE 2`, `LINK_ROOM` in `port/sanaif.c`).
   With the 14-byte Ethernet header first, every IP header, TCP header and payload
   sat two bytes off a longword, and a 68020/030 needs two bus cycles for each such
   longword - in the checksum, in lwIP's header reads, in the copy to the program.
2. **Fewer ACKs on Ethernet**: every 8 full segments instead of 2 (`PN_ACK_EVERY`,
   `tcp_ack()` in `tcp_priv.h`), but at once for a segment with PSH, so a sender
   with a small window never waits for the delayed-ACK timer. Serial links keep
   lwIP's 2.
3. **A 24-segment receive window with 32 driver reads queued** (was 8 and 8). A
   bigger window with too few reads made the driver drop bursts (290-460 KB/s);
   the read count alone changed nothing.
4. **A per-connection window limit for serial links** (`PN_SET_WND`, 8 segments).
   On the CD32 behind an ESP SLIP bridge, a 24-segment window let aminet's server
   overrun the bridge: the download took 26 s instead of 18 s.

Tried and dropped: delaying window updates to half the window (the sender stalls,
470 KB/s) and windows of 32+ segments (no gain over 24). Skipping the TCP checksum
on receive gains another ~5%, but stays on.

Memory: the running core grew from ~583 KB to ~646 KB (96 pool pbufs instead of 64).

**CD32** over SLIP (Rhialto `slip.device`, 115200 baud, ESP bridge): sending 160 KB
takes 15 s (Roadshow 16 s), receiving 277 KB 43 s (Roadshow 38 s). The serial port
itself is the limit there.

## Memory: run-time allocation (2026-10-17)

lwIP's heap and pools used to be static arrays (298 KB of BSS); they are now
allocated on demand within a budget (port/pnmem.c, per-size free lists so
packet buffers are reused). Emulated A1200/030, slirp, host bench server,
`httpbench PATH=/4m COUNT=3 UPLOAD=2048` and `CALLS=2000`:

| core | idle memory | download | upload | per call |
|---|---|---|---|---|
| standard, static buffers | 502 KB | 652 KB/s | 820 KB/s | 312 us |
| standard, run-time | ~290 KB | 642 KB/s | 800 KB/s | 308 us |
| small, static buffers | 218 KB | 597 KB/s | 786 KB/s | 321 us |
| small, run-time | ~175 KB | 603 KB/s | 762 KB/s | 318 us |

The differences are within run-to-run noise. On a stock 2 MB chip-only A1200
the small core now starts and passes bsdtest with 135 KB of the machine free.

## What changed, and what each bought

Measured with the profiling build (`make PROFILE=1`, see below) unless noted.

1. **Numeric host names resolved in the library** (`numeric_host()` in
   `lib/bsdsocket.c`): `gethostbyname("10.0.2.2")` no longer goes to the core,
   whose hosts-file lookup (open, 16 KB read, parse) cost 3 ms. 3.2 → 0.4 ms.
   `getaddrinfo()` already did this.
2. **Clock read once per wakeup** (`port/pnsys.c`). `sys_now()` used to read the
   E-clock (CIA access under Disable) and do a 64-bit soft divide on every call,
   and lwIP calls it many times per pass: ~190 µs per loop pass. Now the first
   `sys_now()` after a `Wait()` reads the clock, the rest of the pass reuses it,
   and the tick-to-ms conversion uses a reciprocal instead of a divide.
3. **Wakeup timer re-armed only when needed.** The loop aborted and re-sent its
   timer.device request on every pass. Now it re-arms only after the timer fired,
   when a request got a deadline, or when the timer isn't pending. Every lwIP
   timeout is at least 250 ms ahead and the timer never sleeps longer than that,
   so the timer always wakes the loop in time. A pass for a bare client request
   now does no time keeping at all; lwIP timeouts are only checked when the timer
   has fired. Together with 2: ping 4.3 → 3.1 ms, one call 381 → 302 µs.
4. **Core's port cached in the library.** `FindPort()` (a string compare per
   public port) ran on every call. The core's own `OpenLibrary()` now registers its
   port in the master base and its `CloseLibrary()` (under Forbid, right after
   `RemPort`) clears it. SANA-II port polled only when its signal arrived.
5. **68000 asm checksum** (`port/chksum.S`, `LWIP_CHKSUM`): 32 bytes per pass with
   `addx.l`, odd start/length handled like lwIP's own. Tested against lwIP's C
   routine on 1908 buffers (all offsets 0-3, lengths 0-3000, carry-heavy patterns).
6. **Core built `-O2`** (was `-Os`; libraries and tools stay `-Os`). +17 KB.
7. **TCP window 4 → 8 MSS, pbuf pool 48 → 64.** The biggest single step for
   downloads with small read buffers: the per-call cost is paid less often when
   each `recv()` finds more data. 4 KB-buffer download 595 → 659 KB/s, connections
   73 → 84/s in the same session. A slow reader could now pin more pool pbufs
   (which are also the SANA-II receive buffers), so when fewer than 16 pool pbufs
   are free, newly received TCP data is copied to heap pbufs (`keep_rx()` in
   `core/csock.c`; tested by forcing it on for every segment).
8. **68020 build** (`make core020`, `out/020/PrismNet`): measured no faster than the
   68000 build on the emulated 68030 (619 / 612 KB/s, 71 conn/s, 870 KB/s up). The
   core is no longer the bottleneck there, so it is not shipped.

### Direct calls (2026-10-17)

A library call used to be a message round trip to the core task: PutMsg,
switch to the core, one loop pass, ReplyMsg, switch back, about 290-316 µs.
Socket calls now run the core's request handler in the caller's own task
(struct PnDirect in pnmsg.h): the core holds a public semaphore whenever it
is not asleep, the library takes it, runs the handler on a private 6 KB stack,
and lets go. A request that has to wait still goes onto the core's pending
list and is answered by message. Same session, emulated A1200/030, slirp,
`httpbench CALLS=2000` and `PATH=/4m COUNT=2 CONNECTS=100 UPLOAD=2048`:

| | messages | direct calls |
|---|---|---|
| per call (getsockopt / idle recv) | 316 µs | 101-104 µs |
| connections/s | 73.2 | 80.5 |
| download | 601 KB/s | 607 KB/s |
| upload | 794 KB/s | 797 KB/s |

A call that only reads state, or finds nothing to do (a recv that would
block), skips the rest of a loop pass (pending requests, socket events); that
took the per-call figure from 129 to about 102 µs.

## Tools

- `httpbench ... CALLS=n`: n× `getsockopt(SO_TYPE)` and n× `recv(MSG_DONTWAIT)` on
  an idle UDP socket, the per-call cost of any stack.
- `make PROFILE=1`: the core logs E-clock ticks per main-loop section every 16384
  passes (`PrismNet LOG`), and the first `LOG` request also logs pbuf pool use,
  queued SANA-II reads, every TCP pcb's window state and every pending request.
  The profiling marks themselves cost ~26 ticks (~37 µs) each.
- `make diag`: `out/diag/PrismNet` publishes its loop state in a public port
  `PrismNet.diag`; `out/diag/pndiag` reads it **without** messaging the core (so it
  can't wake a core that has lost a wakeup). Run it from a background loop.

## Known issue: cnet.device stops answering (WinUAE NE2000)

In heavy test sessions (`t-bench`, `t-regress`) the emulated NE2000/`cnet.device`
occasionally stops completing I/O: every write stays with the driver, received
counters freeze, the core keeps running (TCP retries, `tx dropped` climbs). Seen
in 8 of about 45 heavy sessions with the new code, and in none of 8 with 0.4 or
2 with Roadshow, mostly in the first minute after boot. Our use of the
driver did not change (same requests, same hooks); only timing did. The likely
cause is a lost PCMCIA interrupt in the driver, which more back-to-back I/O hits
more often. Not root-caused.

`sanaif_watchdog()` (called on timer passes) treats a write held for 2 s as a
stall: it aborts all I/O, collects what comes back within 1 s (the rest is marked
lost and never reused), sends `S2_OFFLINE` + `S2_ONLINE` and requeues the reads,
logging `eth0: cnet.device stopped answering, restarted it`. The limit doubles
after each reset (up to 64 s) and returns to 2 s when a write completes. The reset
path is tested by forcing it every ~5 s during downloads, connects and uploads
(`-DSANAIF_TEST_RESET=20`): all transfers complete. Whether it revives a genuinely
stuck `cnet.device` has not been observed yet.
