# bsdsocket.library conformance (bsdsocktest)

[bsdsocktest](https://github.com/tbdye/bsdsocktest) 0.2.3 (commit cb08680) is an
open-source conformance suite for Amiga `bsdsocket.library`: 142 tests in 12
categories. Run on 2026-10-17 in WinUAE ("PrismNet test": 68030, OS 3.2,
cnet.device over slirp), with its host helper on the Windows host.

## Result

**138 passed, 1 failed, 3 skipped (142 total).**

| | Before | After |
|---|---|---|
| Loopback only | 122/127, 5 failed | all pass but #64 |
| With host helper | 135/139, 4 failed | 138/139, 1 failed |

The suite identifies the stack as `PrismNet bsdsocket.library 4.2 (lwIP 2.2.1)`.

## Fixed in PrismNet

| Test | Problem | Fix |
|---:|---|---|
| (header) | `SBTC_RELEASESTRPTR` (tag 29) returned 0; PrismNet had it as "COMPAT43". Programs that tell stacks apart by the release string saw nothing. | Returns the release string. |
| 76, 77 | `SBTC_ERRNOLONGPTR` / `SBTC_HERRNOLONGPTR` could be set but not read. | Reading gives back the pointer in use. |
| 54 | `SO_SNDBUF` was accepted and ignored (always 11680). | Honoured per TCP socket by moving lwIP's per-pcb `snd_buf`, from 2 MSS up to the segment queue (46720 normal build); inherited by accepted sockets. Memory still comes out of the run-time budget. |
| 131 | Only ~30 TCP sockets could be open, though `getdtablesize()` says 128. | `MEMP_NUM_TCP_PCB` 32 -> 64 in the normal build. It is a limit only: pcbs are allocated when used, within the budget. The small build keeps 12. |

## Remaining

| Test | Why |
|---:|---|
| 64 fail | WaitSelect `exceptfds` for out-of-band data. lwIP has no TCP urgent-data support: `MSG_OOB` bytes are sent and received inline, so nothing is ever "exceptional". Fixing it means changing lwIP's TCP input/output; OOB is rare (telnet/ftp abort). |
| 70 skip | Needs 65 sockets open at once; the normal build opens about 61. |
| 101, 102 skip | No `networks` database (`getnetbyname`/`getnetbyaddr`). |

UDP throughput shows `16/200 received`: the test blasts 200 datagrams at a
socket that is not reading, and PrismNet queues at most 16 per socket
(`PN_MAX_DGRAMS`) before dropping, which UDP allows. Kept small on purpose
for low-memory machines.

Throughput (68030 in WinUAE, slirp): TCP loopback 891 KB/s, TCP network
776 KB/s, 256 KB echo 306 KB/s; ICMP RTT 0.45 ms network, 0.90 ms loopback.

## Test-setup problems (not PrismNet)

- **#41** (incoming connection from the helper) needs an inbound port forward
  in slirp: start WinUAE with `-s slirp_redir=tcp:7861:7861:10.0.2.15`
  (port = 7700 + 161). The winuae test tool cannot pass that, so boot by hand.
- **#42** (256 KB echo) failed with the stock helper on a **Windows** host:
  accepted sockets inherit the listener's non-blocking mode on Windows, so the
  echo's `sendall()` raises "would block" once the Amiga's window is full and
  the helper silently closes the connection (the Amiga sees EOF after ~44 KB).
  On Linux accepted sockets are blocking, as the helper assumes. With the echo
  socket set blocking it passes (262144/262144). Worth reporting upstream.

## Running it

```
python3 host/bsdsocktest_helper.py --bind 127.0.0.1      ; on the host
bsdsocktest HOST 10.0.2.2 NOPAGE LOG RAM:bst.log          ; on the Amiga
```
`test/amiga/t-bsdsocktest` runs the loopback tests on the test machine.
