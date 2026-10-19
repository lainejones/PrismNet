AmigaDOS test scripts for the WinUAE `PrismNet test` machine. Copy to `DH0:PrismNet/`
along with `out/PrismNet`, `out/bsdtest`, `out/bsdstress` (`out/bsdsocket.library` goes in `LIBS:`)
and run with `Execute DH0:PrismNet/t-<name>`. `t-aweb` is a GUI test (screenshot it).

Benchmarks and soak (see `docs/PERFORMANCE.md`; `tools/bench/benchserver.py` must run on
the host, `out/httpbench` and `out/ping` go in `DH0:PrismNet/net/`): `t-bench` (PrismNet),
`t-rsbench` (Roadshow staged in `DH0:PrismNet/rs/`), `t-calls` (per-call cost),
`t-regress` (t-config, t-stress, t-usergroup, t-wget), `t-soak` (3 benchmark rounds with
`t-watch` probing a `make diag` core every 5 s through `DH0:PrismNet/pndiag`).
