# Small machines

## How PrismNet uses memory

Nothing is reserved when PrismNet starts. Packet buffers, connections and
queues are taken as traffic needs them and given back when it is over, up
to a **budget**: 384 KB by default. When the budget is reached, further
allocations are refused (PrismMon counts them) instead of taking the
machine's last memory, and TCP slows down until buffers are free again.

When idle, the standard core uses about 290 KB in all (the program, its
stack and the stack's own state).

`PrismNet STATUS` and PrismMon's *Memory* tab show what is in use, the
peak, the budget and how much the machine has left. If PrismNet can't
start, `PrismNet START` says so, with the free memory and the largest free
block.

## Changing the budget

PrismNet Prefs, *Startup* tab, **Memory budget (KB)**, or `MEMORY=` in
`ENV:PrismNet/startup`. A smaller budget means slower big transfers and
fewer connections at once; a bigger one helps a fast machine move data
faster.

## The small core

For a 68000 with 1 to 2 MB (an A500, an A2000, or an A1200 without fast
RAM) there is a second build of the PrismNet command: `Tools/Small/PrismNet`
in the archive. The installer offers it when the machine has less than 3 MB in all;
or copy it over `C:PrismNet` yourself. It has a smaller receive window,
buffer limits and stack, is built for size, and uses about 175 KB when
idle, with a 96 KB budget. Everything else (the libraries, Prefs, the
tools) is the same.

## Other savings

- Don't start PrismNet at boot; let the first program that needs it start
  it (*Start when needed*).
- PrismMon refreshes every second; close it when you don't need it.
- An NFS share costs about 64 KB while mounted; set `RSIZE=` and `WSIZE=`
  lower to save more.
