#!/usr/bin/env python3
r"""
Pre-deploy test gate for PrismNet.  No build goes to a real Amiga unless

    python tools/gate.py

exits 0.  It builds everything (amiga-gcc in WSL), deploys the stack and the
test tools to the WinUAE machine "PrismNet test" (DH0: = C:\Amiga\PrismNet-test),
then boots that machine once per test script in test/amiga and checks what the
scripts left behind:

  * the boot finished in time (a guru or a hang never writes the done marker);
  * every "-> PASS" / "-> FAIL" line, and at least the expected number of PASSes;
  * "stack used at most N of M bytes" from PrismNet STATUS: N <= M/2;
  * "N refused" (lwIP allocations) from PrismNet STATUS: N == 0;
  * test-specific markers (all four of pingstop.log, "PrismNet stopped.", ...).

slirp's internet access in WinUAE is occasionally flaky, so a network test that
failed only on tool PASS/FAIL results is run once more; passing then is reported
as FLAKY (not a gate failure, but look at it).  Timeouts, stack, allocation and
marker failures are never retried.

The emulator is driven with the winuae MCP server's own code
(C:\projects\tools\mcp-winuae\winuae_mcp.py): it injects a marked block into
S/Startup-Sequence, boots headless, waits for the done marker, screenshots a
hang, kills only its own emulator and restores Startup-Sequence.

Results: a table on stdout, docs/gate-last.txt, and each test's logs under
build/gate/<test>/.  Options: --skip-build, --only t-a,t-b, --no-retry.
"""

import argparse
import datetime
import re
import shutil
import subprocess
import sys
import types
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
TESTS_SRC = ROOT / "test" / "amiga"
GATE_DIR = ROOT / "build" / "gate"
REPORT = ROOT / "docs" / "gate-last.txt"

CONFIG = "PrismNet test"
MCP_DIR = Path(r"C:\projects\tools\mcp-winuae")
BUILD_CMD = "cd /mnt/c/projects/PrismNet && export PATH=/opt/amiga/bin:$PATH && make"


def _load_winuae():
    """Import winuae_mcp; stub FastMCP if the mcp package isn't installed here."""
    try:
        import mcp.server.fastmcp  # noqa: F401
    except ImportError:
        class FastMCP:
            def __init__(self, *a, **k):
                pass

            def tool(self, *a, **k):
                return lambda fn: fn
        fm = types.ModuleType("mcp.server.fastmcp")
        fm.FastMCP = FastMCP
        for name in ("mcp", "mcp.server"):
            sys.modules.setdefault(name, types.ModuleType(name))
        sys.modules["mcp.server.fastmcp"] = fm
    sys.path.insert(0, str(MCP_DIR))
    import winuae_mcp
    return winuae_mcp


@dataclass
class Test:
    name: str
    logs: list                  # files the script writes in DH0:PrismNet/
    timeout: int                # seconds for boot + script
    min_pass: int               # "-> PASS" lines it must produce
    network: bool = False       # talks to the internet (aminet.net): retry once
    need_stack: bool = False    # must report "stack used at most"
    markers: list = field(default_factory=list)   # strings that must all appear
    no_lib: int = 0             # expected "cannot open bsdsocket.library" lines


TESTS = [
    Test("t-pingstop", ["pingstop.log", "st.log"], 180, 1,
         markers=["pinged", "stopped", "waited", "rsquery done"]),
    Test("t-stack", ["hw.log"], 300, 2, network=True, need_stack=True,
         markers=["PrismNet stopped."]),
    Test("t-rsq", ["rsq.log"], 180, 2, need_stack=True,
         markers=["PrismNet stopped.", "--- not running"]),
    # Roadshow's configuration calls (0.5); needs the internet for the gateway checks
    Test("t-rscfg", ["rscfg.log"], 240, 1, network=True, need_stack=True,
         markers=["PrismNet stopped.", "route 192.168.77.0 added via",
                  "10.0.2.0/24        -                eth0       local subnet",
                  "default            10.0.2.2         eth0       default", "10.0.2.2         52:54:00:12:35:02  eth0"]),
    # NetCtl (1.0.8): routes, default gateway, name servers from the Shell
    Test("t-netctl", ["netctl.log"], 180, 7, need_stack=True, markers=["PrismNet stopped."]),
    # A, B and C each run bsdtest; D (AUTOSTART=NO) must NOT get the library.
    Test("t-config", [], 300, 3, network=True,
         markers=["=== D:"], no_lib=1),
    Test("t-stress", [], 300, 2, network=True, need_stack=True),
]

# What goes on DH0: (relative to C:\Amiga\PrismNet-test).
DEPLOY = [
    ("PrismNet", "C"),
    ("PrismNet", "PrismNet"),
    ("bsdsocket.library", "Libs"),
    ("bsdtest", "PrismNet"),
    ("bsdstress", "PrismNet"),
    ("rsquery", "PrismNet"),
    ("rscfg", "PrismNet"),
    ("NetCtl", "PrismNet"),
    ("ping", "PrismNet/net"),
]

RE_VERDICT = re.compile(r"->\s*(PASS|FAIL)\b")
RE_STACK = re.compile(r"stack used at most (\d+) of (\d+) bytes")
# "N refused" since run-time allocation (0.5), "N failed allocations" before
RE_ALLOC = re.compile(r"(\d+) (?:refused|failed allocations)")


def log(msg=""):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# build and deploy
# --------------------------------------------------------------------------

def build():
    log("== build (make in WSL)")
    res = subprocess.run(["wsl", "-e", "bash", "-lc", BUILD_CMD], cwd=str(Path.home()),
                         capture_output=True, text=True, errors="replace")
    (GATE_DIR / "build.log").write_text(res.stdout + res.stderr, encoding="utf-8")
    if res.returncode != 0:
        tail = "\n".join((res.stdout + res.stderr).splitlines()[-25:])
        return f"make failed (rc {res.returncode}):\n{tail}"
    return ""


HOST_TESTS_CMD = ("cd /mnt/c/projects/PrismNet && make cryptotest smbunit xdrtest >/dev/null "
                  "&& build/cryptotest | tail -1 && build/smbunit | tail -1 && build/xdrtest | tail -1")


def host_tests():
    """Published crypto vectors and the parser checks with crafted replies (native
    gcc in WSL, AddressSanitizer): all three must print 'all passed'."""
    log("== host tests (cryptotest, smbunit, xdrtest)")
    res = subprocess.run(["wsl", "-e", "bash", "-lc", HOST_TESTS_CMD], cwd=str(Path.home()),
                         capture_output=True, text=True, errors="replace")
    (GATE_DIR / "host-tests.log").write_text(res.stdout + res.stderr, encoding="utf-8")
    passed = res.stdout.count("all passed")
    if res.returncode != 0 or passed != 3:
        tail = "\n".join((res.stdout + res.stderr).splitlines()[-15:])
        return f"host tests: {passed}/3 passed (rc {res.returncode}):\n{tail}"
    return ""


def check_binaries():
    for src, _ in DEPLOY:
        p = OUT / src
        if not p.is_file():
            return f"missing out/{src}"
        if p.read_bytes()[:4] != b"\x00\x00\x03\xf3":
            return f"out/{src} is not an AmigaOS hunk executable"
    return ""


def deploy(host):
    log(f"== deploy to {host}")
    for src, dest in DEPLOY:
        d = host / dest
        d.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(OUT / src, d / src)
    dst = host / "PrismNet"
    for f in TESTS_SRC.iterdir():
        if f.is_file() and f.name.startswith("t-"):
            shutil.copyfile(f, dst / f.name)
    shutil.copytree(TESTS_SRC / "cfg", dst / "cfg", dirs_exist_ok=True)


# --------------------------------------------------------------------------
# running and checking one test
# --------------------------------------------------------------------------

def run_boot(uae, test, host):
    """One boot of the test machine. Returns (finished, text, report)."""
    pdir = host / "PrismNet"
    for name in test.logs:
        (pdir / name).unlink(missing_ok=True)       # never judge a stale log
    report = uae.run_cli_test(CONFIG, f"Execute DH0:PrismNet/{test.name}",
                              timeout_sec=test.timeout)
    finished = report.startswith("command finished")
    out = report.split("--- output ---", 1)[1] if "--- output ---" in report else ""
    texts = [f"### execute output\n{out}"]
    keep = GATE_DIR / test.name
    keep.mkdir(parents=True, exist_ok=True)
    (keep / "report.txt").write_text(report, encoding="utf-8")
    for name in test.logs:
        p = pdir / name
        body = p.read_text(encoding="latin-1", errors="replace") if p.is_file() else ""
        if p.is_file():
            shutil.copyfile(p, keep / name)
        texts.append(f"### {name}\n{body}")
    return finished, "\n".join(texts), report


def check(test, finished, text, report):
    """Returns (hard failures, network-ish failures, notes)."""
    hard, soft, notes = [], [], []
    if not finished:
        shot = next((ln for ln in report.splitlines() if ln.startswith("screenshot")), "")
        hard.append("TIMEOUT (guru or hang)" + (f"; {shot}" if shot else ""))

    verdicts = RE_VERDICT.findall(text)
    passes, fails = verdicts.count("PASS"), verdicts.count("FAIL")
    for ln in text.splitlines():
        m = RE_VERDICT.search(ln)
        if m and m.group(1) == "FAIL":
            soft.append(f"tool: {ln.strip()}")
    if passes < test.min_pass:
        soft.append(f"{passes} PASS lines, expected {test.min_pass}")
    notes.append(f"{passes} pass/{fails} fail")

    stacks = [(int(n), int(m)) for n, m in RE_STACK.findall(text)]
    for n, m in stacks:
        if n > m // 2:
            hard.append(f"stack used {n} of {m} bytes (> half)")
    if test.need_stack and not stacks:
        hard.append('no "stack used at most" line in STATUS')
    if stacks:
        notes.append(f"stack peak {max(n for n, _ in stacks)}/{stacks[0][1]}")

    # Idle and light-load STATUS must show no failed lwIP allocations. After
    # bsdstress's 8 parallel downloads the packet pool and heap run full and
    # lwIP drops (and TCP retransmits) - counted, but not a failure.
    stress_at = text.find("bsdstress:")
    load_fails = []
    if stacks and not RE_ALLOC.search(text):
        # STATUS printed, but no allocation count matched: the wording changed
        # and this check would silently pass everything.
        hard.append("STATUS has no lwIP allocation count (update RE_ALLOC in gate.py)")
    for m in RE_ALLOC.finditer(text):
        n = int(m.group(1))
        if not n:
            continue
        if 0 <= stress_at < m.start():
            load_fails.append(n)
        else:
            hard.append(f"{n} failed allocations outside the stress load (must be 0)")
    if load_fails:
        notes.append(f"{max(load_fails)} failed allocs under 8-task load")

    for mk in test.markers:
        if mk not in text:
            hard.append(f'missing "{mk}"')
    # The stack not coming up is never a slirp hiccup: don't let it be retried.
    if "bytes of stack, need" in text:
        hard.append("core refused to start: stack too small (swapstack.o not linked?)")
    elif "failed to start" in text:
        hard.append("PrismNet failed to start")
    no_lib = text.count("cannot open bsdsocket.library") + text.count("no bsdsocket.library")
    if no_lib != test.no_lib:
        hard.append(f"bsdsocket.library not available {no_lib}x (expected {test.no_lib})")
    return hard, soft, notes


def run_test(uae, test, host, retry):
    log(f"== {test.name}")
    finished, text, report = run_boot(uae, test, host)
    hard, soft, notes = check(test, finished, text, report)
    if not hard and not soft:
        return "PASS", [], notes
    if retry and test.network and not hard:
        log(f"   network-only failure, retrying once: {soft}")
        # keep the failed attempt's logs: a FLAKY result is still worth reading
        first = GATE_DIR / (test.name + ".1st")
        shutil.rmtree(first, ignore_errors=True)
        shutil.copytree(GATE_DIR / test.name, first)
        (first / "output.txt").write_text(text, encoding="utf-8")
        finished, text, report = run_boot(uae, test, host)
        hard2, soft2, notes2 = check(test, finished, text, report)
        if not hard2 and not soft2:
            return "FLAKY", [f"1st run: {s}" for s in soft], notes2
        return "FAIL", ["1st run: " + "; ".join(soft)] + hard2 + soft2, notes2
    return "FAIL", hard + soft, notes


# --------------------------------------------------------------------------

def git_rev():
    try:
        rev = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True).stdout.strip()
        return rev + (" (+ uncommitted changes)" if dirty else "")
    except OSError:
        return "?"


def main():
    ap = argparse.ArgumentParser(description="PrismNet pre-deploy test gate")
    ap.add_argument("--skip-build", action="store_true", help="use the existing out/")
    ap.add_argument("--only", default="", help="comma-separated test names")
    ap.add_argument("--no-retry", action="store_true", help="never retry network tests")
    args = ap.parse_args()

    tests = TESTS
    if args.only:
        want = [t.strip() for t in args.only.split(",") if t.strip()]
        unknown = [w for w in want if w not in {t.name for t in TESTS}]
        if unknown:
            ap.error(f"unknown test(s): {', '.join(unknown)}")
        tests = [t for t in TESTS if t.name in want]

    GATE_DIR.mkdir(parents=True, exist_ok=True)
    started = datetime.datetime.now()
    results = []            # (name, verdict, details, notes)
    fatal = ""

    uae = _load_winuae()
    cfg, err = uae._resolve_config(CONFIG)
    mount = None
    if not err:
        mount, err = uae._require_host_dir(cfg, "DH0")
    if err:
        fatal = f"WinUAE config: {err}"
    elif any(uae._runs_config(cmd, cfg) for _, cmd in uae._winuae_procs()):
        fatal = f'WinUAE is already running "{CONFIG}" (someone else testing?) - not touching it'

    if not fatal and not args.skip_build:
        fatal = build()
    if not fatal:
        fatal = host_tests()
    if not fatal:
        fatal = check_binaries()
    if not fatal:
        host = Path(mount.host_path)
        deploy(host)
        try:
            for t in tests:
                verdict, details, notes = run_test(uae, t, host, not args.no_retry)
                results.append((t.name, verdict, details, notes))
                log(f"   {verdict}  {', '.join(notes)}")
                for d in details:
                    log(f"     - {d}")
        finally:
            uae.stop_emulator(CONFIG)     # always: kill ours, restore Startup-Sequence

    ok = not fatal and all(v != "FAIL" for _, v, _, _ in results)
    lines = [
        f"PrismNet pre-deploy gate: {'PASSED' if ok else 'FAILED'}",
        f"date {started:%Y-%m-%d %H:%M}, commit {git_rev()}, "
        f"{'existing out/' if args.skip_build else 'fresh build'}, machine \"{CONFIG}\"",
        "",
    ]
    if fatal:
        lines += ["FATAL: " + fatal, ""]
    if results:
        w = max(len(n) for n, *_ in results)
        lines.append(f"{'test'.ljust(w)}  result  notes")
        lines.append(f"{'-' * w}  ------  -----")
        for name, verdict, details, notes in results:
            lines.append(f"{name.ljust(w)}  {verdict.ljust(6)}  {', '.join(notes)}")
            lines += [f"{' ' * w}          - {d}" for d in details]
        lines.append("")
    lines.append("Do NOT deploy to real hardware." if not ok else
                 "OK to deploy; then on one real machine: start, ping, STOP, restart, "
                 "a download, STATUS.")
    if any(v == "FLAKY" for _, v, _, _ in results):
        lines.append("A FLAKY test failed once and passed on retry - its first attempt is in build/gate/<test>.1st/.")
    text = "\n".join(lines) + "\n"
    REPORT.write_text(text, encoding="utf-8", newline="\n")
    log("")
    log(text)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
