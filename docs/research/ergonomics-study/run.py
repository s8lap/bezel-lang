#!/usr/bin/env python3
"""Re-run the ergonomics study against a compiler (python3 stdlib only).

Usage: run.py BEZELC [--drafts] [--only PATTERN] [--keep] [--timeout SECONDS] [--workdir DIR]

Compiles and runs every final.bzl of both rounds (programs/ and r42/programs/), and with
--drafts every v*.bzl too, against inputs/<task>.cases; then compiles and runs every
repros/*.bzl against its `// expect:` header. See README.md, "Re-running the study".
Exit status: 0 after a measurement (whatever the results), 2 on a usage error or a
missing compiler.
"""
import argparse
import csv
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH_PREFIX = "/var/tmp/bezel-scratch/"   # hardcoded by the programs; redirected per item
SCRATCH_DIRS = ("ergo/bugs", "ergo2/run")
COMPILE_TIMEOUT = 180
BASE_FLAGS = ["--checked-disposal"]

# recorded verdict word -> runner result word (r42/data/recheck-tasks.tsv)
VERDICT = {"ok": "PASS", "wrong": "FAIL", "reject": "REJECTED", "half": "FAIL"}


# ---------------------------------------------------------------- expectations

def parse_cases(path):
    """inputs/<task>.cases: `flags:` at top, then [case] blocks with args/exit and
    `|`-prefixed stdin/stdout/stderr blocks."""
    flags, cases, cur, block = [], [], None, None
    for raw in open(path, encoding="utf-8").read().split("\n"):
        if raw.startswith("#") or (not raw.strip() and block is None):
            continue
        if raw.startswith("[case"):
            cur = {"name": raw[5:].rstrip("]").strip(), "args": [], "exit": 0,
                   "stdin": None, "stdout": None, "stderr": None}
            cases.append(cur)
            block = None
            continue
        if raw.startswith("|"):
            if cur is None or block is None:
                raise ValueError(f"{path}: `|` line outside a block")
            cur[block].append(raw[2:] if raw.startswith("| ") else raw[1:])
            continue
        if not raw.strip():
            continue
        key, _, val = raw.partition(":")
        key, val = key.strip(), val.strip()
        if cur is None:
            if key == "flags":
                flags = shlex.split(val)
                continue
            raise ValueError(f"{path}: `{key}` before the first [case]")
        if key == "args":
            cur["args"] = shlex.split(val)
            block = None
        elif key == "exit":
            cur["exit"] = int(val)
            block = None
        elif key in ("stdin", "stdout", "stderr"):
            cur[key] = []
            block = key
        else:
            raise ValueError(f"{path}: unknown key `{key}`")
    for c in cases:
        if c["stdin"] is not None:
            c["stdin"] = "".join(l + "\n" for l in c["stdin"])
        c["stdout"] = [("out", l) for l in (c["stdout"] or [])]
        if c["stderr"] is not None:
            c["stderr"] = [("out", l) for l in c["stderr"]]
    if not cases:
        raise ValueError(f"{path}: no [case]")
    return flags, cases


def parse_repro(path):
    """The `// key: value` header of a repros/*.bzl file."""
    h = {"expect": None, "stdout": [], "diagnostic": [], "flags": [], "order": "exact",
         "bug": os.path.basename(path)[:-4], "title": "", "note": ""}
    for line in open(path, encoding="utf-8"):
        if not line.startswith("//"):
            break
        key, _, val = line[2:].strip().partition(":")
        val = val[1:] if val.startswith(" ") else val
        val = val.rstrip("\n")
        if key == "stdout":
            h["stdout"].append(("out", val))
        elif key == "stdout-re":
            h["stdout"].append(("re", val))
        elif key == "diagnostic":
            h["diagnostic"].append(val)
        elif key == "flags":
            h["flags"] = shlex.split(val)
        elif key in ("expect", "order", "bug", "title", "note", "issue", "timeout"):
            h[key] = val.strip() if key != "title" else val
    if h["expect"] not in ("run", "reject", "quiet"):
        raise ValueError(f"{path}: missing or bad `// expect:` (run | reject | quiet)")
    stdin = path[:-4] + ".stdin"
    h["stdin"] = open(stdin, encoding="utf-8").read() if os.path.exists(stdin) else None
    return h


def recorded_baselines():
    """Recorded r42 verdicts, for the table's `r42` column (informative only)."""
    base = {}
    p = os.path.join(HERE, "r42", "data", "recheck-tasks.tsv")
    if os.path.exists(p):
        for row in csv.DictReader(open(p, encoding="utf-8"), delimiter="\t"):
            for kv in row["per_draft_r42"].split():
                d, _, v = kv.partition(":")
                base[f"{row['task']}/{d}"] = VERDICT.get(v, v)
    p = os.path.join(HERE, "r42", "data", "recheck-bugs.tsv")
    if os.path.exists(p):
        for row in csv.DictReader(open(p, encoding="utf-8"), delimiter="\t"):
            base[row["bug"]] = row["disposition"]
    for d in sorted(os.listdir(os.path.join(HERE, "r42", "programs"))):
        base.setdefault(f"{d}/final", "PASS")
    return base


# ---------------------------------------------------------------- running

def norm(text):
    lines = [l.rstrip() for l in text.replace("\r\n", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    return lines


def compare(actual, expected, order="exact"):
    """None if `actual` (lines) matches `expected` ([(kind, text)]), else a short reason."""
    exp = [(k, v.rstrip()) for k, v in expected]
    while exp and exp[-1] == ("out", ""):
        exp.pop()
    if order == "any":
        want = sorted(v for _, v in exp)
        got = sorted(actual)
        if want == got:
            return None
        missing = [l for l in want if l not in got or want.count(l) > got.count(l)]
        extra = [l for l in got if l not in want or got.count(l) > want.count(l)]
        return f"lines differ: missing {short(missing)}, extra {short(extra)}"
    for i, (k, v) in enumerate(exp):
        if i >= len(actual):
            return f"line {i + 1}: missing, expected {v!r}"
        a = actual[i]
        ok = re.fullmatch(v, a) is not None if k == "re" else a == v
        if not ok:
            return f"line {i + 1}: got {a[:60]!r}, expected {v[:60]!r}"
    if len(actual) > len(exp):
        return f"line {len(exp) + 1}: extra output {actual[len(exp)][:60]!r}"
    return None


def short(xs):
    s = ", ".join(repr(x) for x in xs[:3])
    return f"[{s}{', …' if len(xs) > 3 else ''}]"


def run_proc(cmd, stdin, cwd, timeout):
    """(rc, stdout, stderr, timed_out); rc < 0 is a signal."""
    p = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        out, err = p.communicate(stdin.encode() if stdin is not None else None, timeout=timeout)
        return p.returncode, out.decode("utf-8", "replace"), err.decode("utf-8", "replace"), False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        out, err = p.communicate()
        return None, out.decode("utf-8", "replace"), err.decode("utf-8", "replace"), True


def first_line(text, pat):
    for l in text.splitlines():
        if re.search(pat, l):
            return re.sub(r"^\S*\.bzl:", "", l).strip()[:110]
    return ""


def crash_reason(rc, err):
    if rc is not None and rc < 0:
        try:
            return f"signal {signal.Signals(-rc).name}"
        except ValueError:
            return f"signal {-rc}"
    m = first_line(err, r"Unhandled exception|fatal:|panic|Segmentation|Aborted")
    return m or None


class Item:
    def __init__(self, name, kind, src, flags, cases=None, repro=None):
        self.name, self.kind, self.src, self.flags = name, kind, src, flags
        self.cases, self.repro = cases, repro


def collect(drafts, only):
    items = []
    for rnd, pdir, kind in (("t", os.path.join(HERE, "programs"), "final"),
                            ("n", os.path.join(HERE, "r42", "programs"), "final")):
        for task in sorted(os.listdir(pdir)):
            cases_path = os.path.join(HERE, "inputs", f"{task}.cases")
            flags, cases = parse_cases(cases_path)
            files = ["final.bzl"]
            if drafts:
                vs = [f for f in os.listdir(os.path.join(pdir, task)) if re.fullmatch(r"v\d+\.bzl", f)]
                files = sorted(vs, key=lambda f: int(f[1:-4])) + files
            for f in files:
                items.append(Item(f"{task}/{f[:-4]}", "final" if f == "final.bzl" else "draft",
                                  os.path.join(pdir, task, f), flags, cases=cases))
    rdir = os.path.join(HERE, "repros")
    for f in sorted(os.listdir(rdir), key=lambda f: (f.startswith("N"), f)):
        if f.endswith(".bzl"):
            h = parse_repro(os.path.join(rdir, f))
            items.append(Item(f[:-4], "repro", os.path.join(rdir, f), h["flags"], repro=h))
    if only:
        rx = re.compile(only)
        items = [i for i in items if rx.search(i.name)]
    return items


def evaluate(item, bezelc, work, timeout):
    """-> (result, reason, seconds, log)"""
    t0 = time.time()
    d = os.path.join(work, "items", item.name.replace("/", "__"))
    fs = os.path.join(d, "fs")
    for sub in SCRATCH_DIRS:
        os.makedirs(os.path.join(fs, sub), exist_ok=True)
    src = open(item.src, encoding="utf-8").read().replace(SCRATCH_PREFIX, fs + "/")
    copy = os.path.join(d, os.path.basename(item.src))
    open(copy, "w", encoding="utf-8").write(src)
    exe = os.path.join(d, "a.out")
    cmd = [bezelc] + BASE_FLAGS + item.flags + ["-o", exe, copy]
    log = [f"$ {' '.join(shlex.quote(c) for c in cmd)}"]
    rc, out, err, to = run_proc(cmd, None, d, COMPILE_TIMEOUT)
    diag = out + err
    log.append(diag.rstrip())
    secs = lambda: round(time.time() - t0, 2)
    if to:
        return "TIMEOUT", f"compile exceeded {COMPILE_TIMEOUT}s", secs(), log
    if rc < 0:
        return "CRASH", f"compiler {crash_reason(rc, err)}", secs(), log
    rep = item.repro
    if rc != 0 or not os.path.exists(exe):
        if rep and rep["expect"] == "reject":
            miss = [s for s in rep["diagnostic"] if s not in diag]
            if miss:
                return "FAIL", f"rejected, but no diagnostic contains {miss[0]!r}: " + first_line(diag, r"error:"), secs(), log
            return "PASS", "rejected as the spec requires", secs(), log
        why = (first_line(diag, r"^\S*\.bzl:\d+:\d+: (?!warning:)")
               or re.sub(r"^.*?(?=bezelc: )", "", first_line(diag, r"bezelc: .*error"))
               or first_line(diag, r"error"))
        return "REJECTED", why or f"compiler exit {rc}", secs(), log
    if rep and rep["expect"] == "reject":
        return "FAIL", "accepted; the spec requires a compile-time rejection", secs(), log
    if rep and rep["expect"] == "quiet":
        w = first_line(diag, r"warning:")
        if w:
            return "FAIL", f"compiled with a warning: {w}", secs(), log
    cases = item.cases or [{"name": "", "args": [], "exit": 0, "stdin": rep["stdin"],
                            "stdout": rep["stdout"], "stderr": None}]
    order = rep["order"] if rep else "exact"
    rtimeout = int(rep.get("timeout") or timeout) if rep else timeout
    for c in cases:
        label = f" [{c['name']}]" if len(cases) > 1 else ""
        rc, out, err, to = run_proc([exe] + c["args"], c["stdin"], d, rtimeout)
        log.append(f"$ a.out {' '.join(c['args'])}  (exit {rc})")
        log.append(out.rstrip())
        if err.strip():
            log.append("[stderr] " + err.rstrip())
        if to:
            got = norm(out)
            return "TIMEOUT", f"no exit after {rtimeout}s{label}; {len(got)} line(s) printed", secs(), log
        cr = crash_reason(rc, err)
        if cr and rc != c["exit"]:
            return "CRASH", f"{cr} (exit {rc}){label}", secs(), log
        why = compare(norm(out), c["stdout"], order)
        if why:
            return "FAIL", f"stdout {why}{label}", secs(), log
        if c["stderr"] is not None:
            why = compare(norm(err), c["stderr"])
            if why:
                return "FAIL", f"stderr {why}{label}", secs(), log
        if rc != c["exit"]:
            return "FAIL", f"exit {rc}, expected {c['exit']}{label}", secs(), log
    return "PASS", "", secs(), log


def default_workdir():
    base = os.environ.get("TMPDIR") or "/var/tmp"
    real = os.path.realpath(base)
    if real == "/tmp" or real.startswith("/tmp/"):
        base = "/var/tmp"   # never /tmp by default (a quota'd tmpfs on the study box)
    return os.path.join(base, "bezel-ergo-run")


def main(argv):
    ap = argparse.ArgumentParser(prog="run.sh", description=__doc__.split("\n")[0])
    ap.add_argument("bezelc")
    ap.add_argument("--drafts", action="store_true", help="also run every v*.bzl draft")
    ap.add_argument("--only", metavar="PATTERN", help="regex over item names (t04-grep/final, n22-todo/v2, B05, NB11)")
    ap.add_argument("--keep", action="store_true", help="keep binaries and per-item scratch")
    ap.add_argument("--timeout", type=int, default=20, help="seconds per program run (default 20)")
    ap.add_argument("--workdir", help="parent of the run directory (default ${TMPDIR:-/var/tmp}/bezel-ergo-run, never /tmp)")
    a = ap.parse_args(argv)

    bezelc = shutil.which(a.bezelc) if os.sep not in a.bezelc else a.bezelc
    if not bezelc or not os.path.isfile(bezelc) or not os.access(bezelc, os.X_OK):
        print(f"run.sh: compiler not found or not executable: {a.bezelc}", file=sys.stderr)
        return 2
    bezelc = os.path.abspath(bezelc)
    parent = os.path.abspath(a.workdir or default_workdir())
    repo = subprocess.run(["git", "-C", HERE, "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True).stdout.strip()
    if repo and (parent + "/").startswith(os.path.realpath(repo) + "/"):
        print(f"run.sh: the work directory must be outside the repository: {parent}", file=sys.stderr)
        return 2
    try:
        items = collect(a.drafts, a.only)
    except (ValueError, OSError, re.error) as e:
        print(f"run.sh: {e}", file=sys.stderr)
        return 2
    if not items:
        print(f"run.sh: --only {a.only!r} matches nothing", file=sys.stderr)
        return 2
    os.makedirs(parent, exist_ok=True)
    work = tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-"), dir=parent)
    ver = subprocess.run([bezelc, "--version"], capture_output=True, text=True)
    version = (ver.stdout or ver.stderr).strip().splitlines()[0] if (ver.stdout or ver.stderr).strip() else "?"
    base = recorded_baselines()

    print(f"compiler: {bezelc}\nversion:  {version}\nwork dir: {work}\n")
    hdr = f"{'item':<24} {'kind':<6} {'result':<9} {'r42':<9} reason"
    print(hdr)
    print("-" * len(hdr))
    rows, t0 = [], time.time()
    logs = os.path.join(work, "logs")
    os.makedirs(logs)
    for it in items:
        res, why, secs, log = evaluate(it, bezelc, work, a.timeout)
        rec = base.get(it.repro["bug"], "") if it.repro else base.get(it.name, "")
        print(f"{it.name:<24} {it.kind:<6} {res:<9} {rec[:9]:<9} {why}", flush=True)
        rows.append((it.name, it.kind, res, rec, why, secs))
        open(os.path.join(logs, it.name.replace("/", "__") + ".log"), "w", encoding="utf-8").write(
            "\n".join(log) + "\n")
        if not a.keep:
            shutil.rmtree(os.path.join(work, "items", it.name.replace("/", "__")), ignore_errors=True)
    if not a.keep:
        shutil.rmtree(os.path.join(work, "items"), ignore_errors=True)

    print()
    kinds = [k for k in ("final", "draft", "repro") if any(r[1] == k for r in rows)]
    for k in kinds:
        sub = [r for r in rows if r[1] == k]
        counts = {}
        for r in sub:
            counts[r[2]] = counts.get(r[2], 0) + 1
        detail = ", ".join(f"{n} {res}" for res, n in sorted(counts.items(), key=lambda kv: -kv[1]))
        if k == "final":
            for label, pre in (("first study", "t"), ("r42 tasks", "n")):
                s = [r for r in sub if r[0].startswith(pre)]
                if s:
                    print(f"finals, {label}: {sum(r[2] == 'PASS' for r in s)}/{len(s)} PASS")
        print(f"{k + 's':<7} {sum(r[2] == 'PASS' for r in sub)}/{len(sub)} PASS  ({detail})")
    tsv = os.path.join(work, "report.tsv")
    with open(tsv, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(["item", "kind", "result", "recorded_r42", "reason", "seconds"])
        w.writerows(rows)
    print(f"\nreport: {tsv}\nlogs:   {logs}\nelapsed {time.time() - t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
