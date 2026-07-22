#!/usr/bin/env python3
"""select_tests.py — diff -> affected-test selector for the gate canary tier (#2100).

Maps the working tree's diff against a base ref to the gtest subset most likely
to catch a regression from that diff, emitting exact `suite.name` lines for
BEZEL_TEST_LIST (tests/test_main.cpp). Selection signals, in the precedence the
2026-07-22 signal audit measured (issue #2100 carries the back-test evidence —
recall 7/9 on reconstructed historical regressions at a median 21% subset):

  1. ISSUE-NUMBER cross-link: '#<n>' (n >= 130) in the base..HEAD commit
     messages -> tests whose suite/name carries n. (68.5% of tests carry an
     issue id; ~80% of fix commits map, at tens-of-tests precision.)
  2. FINE §N.M citations: § tokens harvested from the diff's CHANGED LINES ONLY
     (-U0; ±30-line context over-selects ~5x, measured) plus the commit
     messages -> tests citing §N.M in their name (spec_N_M / trailing _N_M with
     N <= 130) or in a comment within ±5 lines of the BEZEL_TEST macro.
  3. COARSE §N: the same sources at whole-section granularity (a §N citation
     with no .M still selects the §N-tagged tests).
  4. DIAGNOSTIC-STRING match: prose string literals added/removed in src/
     changed lines, substring-matched against test bodies (a literal matching
     more than MAX_DIAG_HITS tests is non-distinctive and dropped).
  5. DIRECTORY fallback (gate variant only): changed src/<phase>/ selects
     tests/<phase>/ plus the phase's pipeline-DOWNSTREAM dirs. tests/codegen is
     NEVER dir-selected (it is ~70% of the suite; dir-selection there destroys
     selectivity — the §/issue signals must carry codegen changes).
  Changed/untracked tests/**/*.cpp always select their own tests. tests/lit is
  not selected here — the gate always runs the whole lit track (~6s).

THE SELECTION IS FAIL-FAST EVIDENCE ONLY. It is never acceptance: the full
sharded pass remains the gate's sole authority (see scripts/gate.sh, #2100).

Usage:
  scripts/select_tests.py [--repo DIR] [--base REF] [--variant gate|lane]
                          [-o FILE] [--check]

  --base REF     diff base (default: merge-base HEAD origin/main). The diff is
                 WORKTREE-vs-REF (committed + uncommitted + untracked), i.e.
                 the whole unpushed batch when the default base is used.
  --variant      'gate' (default): all signals incl. downstream-dir fallback —
                 the back-tested rec0 operating point (median ~21% of suite).
                 'lane': issue + fine-§ + diag only — the tight subset for a
                 producer lane's own change (median ~2.5% of suite).
  -o FILE        write the selected `suite.name` lines there (default stdout).
  --check        self-test: parse the universe and assert the invariants the
                 selector's soundness rests on (multiline macro parsing, signal
                 index sizes). Exit 0 iff all hold. Run by hand or CI; costs
                 ~2s and needs no diff.

Read-only over git (diff/log/ls-files/rev-parse/merge-base); never checkouts.
Exit codes: 0 ok (including an EMPTY selection — the caller decides), 2 usage,
3 environment (not a git repo / unresolvable base).
"""

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict

SEC_RE = re.compile(r"§\s*(\d+)(?:\.(\d+))?")
# Multiline-aware on purpose: \s* crosses newlines. A line-anchored regex
# silently drops the ~13% of macros whose test name sits on the next line —
# guarded by --check below.
BT_RE = re.compile(r"BEZEL_TEST\s*\(\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*\)")
STR_LIT_RE = re.compile(r'"((?:[^"\\\n]|\\.){10,})"')
MAX_TOP_SEC = 130   # a number above this in a test name is an issue id, not a §
MAX_DIAG_HITS = 50  # a literal matching more tests than this is non-distinctive
MAX_DIAG_LITS = 200  # bound the literal-matching work on huge batches

# Pipeline-downstream dir map (compiler phase order: lexer -> parser -> sema ->
# flow -> ownership -> codegen). codegen is deliberately ABSENT from every
# closure: tests/codegen is ~70% of the suite, so dir-selecting it is
# non-selective by construction (#2100).
DOWNSTREAM = {
    "lexer": {"lexer", "parser"},
    "parser": {"parser", "sema"},
    "sema": {"sema", "flow", "ownership"},
    "flow": {"flow", "ownership"},
    "ownership": {"ownership"},
    "module": {"module"},
    "runtime": {"runtime"},
}

VARIANTS = {
    "gate": dict(issue=True, fine=True, coarse=True, diag=True, dirs=True),
    "lane": dict(issue=True, fine=True, coarse=False, diag=True, dirs=False),
}


def sh(repo, *args):
    # Explicit UTF-8, mirroring build_universe's file reads: the §-signal is
    # U+00A7, and a C-locale text=True decode would either raise or silently
    # destroy the fine/coarse recall signal.
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def name_sigs(tok):
    """§/issue signals recoverable from a suite or test identifier."""
    fine, coarse, issues = set(), set(), set()
    for m in re.finditer(r"spec_(\d+)_(\d+)", tok):
        n, mm = int(m.group(1)), int(m.group(2))
        if n <= MAX_TOP_SEC:
            fine.add((n, mm))
    for m in re.finditer(r"spec_(\d+)(?!_?\d)", tok):
        n = int(m.group(1))
        if n <= MAX_TOP_SEC:
            coarse.add(n)
    for m in re.finditer(r"sec(\d+)_(\d+)", tok):
        n, mm = int(m.group(1)), int(m.group(2))
        if n <= MAX_TOP_SEC:
            fine.add((n, mm))
    m = re.search(r"_(\d+)_(\d+)$", tok)
    if m and int(m.group(1)) <= MAX_TOP_SEC:
        fine.add((int(m.group(1)), int(m.group(2))))
    else:
        m2 = re.search(r"_(\d+)$", tok)
        if m2 and int(m2.group(1)) <= MAX_TOP_SEC:
            coarse.add(int(m2.group(1)))
    for t in re.findall(r"\d+", tok):
        if int(t) >= MAX_TOP_SEC:
            issues.add(int(t))
    return fine, coarse, issues


def build_universe(repo):
    """Parse tests/**/*.cpp (worktree state, so untracked files count) into an
    indexed universe: per-test §/issue signals, per-file and per-dir sets."""
    tests = []
    by_file = defaultdict(set)
    by_dir = defaultdict(set)
    fine_map = defaultdict(set)
    coarse_map = defaultdict(set)
    issue_map = defaultdict(set)
    multiline_macros = 0
    for root, _dirs, files in os.walk(os.path.join(repo, "tests")):
        if os.sep + "lit" in root:
            continue
        for fn in sorted(files):
            if not fn.endswith(".cpp"):   # *.cpp.disabled correctly excluded
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, repo)
            try:
                text = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            lines = text.split("\n")
            matches = list(BT_RE.finditer(text))
            for i, m in enumerate(matches):
                # Skip line-commented-out macros (`// BEZEL_TEST(...)`) — they
                # are not registered and would surface as phantom names in the
                # binary's listed-vs-matched warning.
                line_start = text.rfind("\n", 0, m.start()) + 1
                if "//" in text[line_start:m.start()]:
                    continue
                if "\n" in m.group(0):
                    multiline_macros += 1
                lineno = text.count("\n", 0, m.start())  # 0-based
                body_end = (matches[i + 1].start()
                            if i + 1 < len(matches) else len(text))
                body = text[m.start():body_end]
                suite, name = m.group(1), m.group(2)
                fine, coarse, issues = set(), set(), set()
                for tok in (suite, name):
                    f, c, iss = name_sigs(tok)
                    fine |= f
                    coarse |= c
                    issues |= iss
                lo, hi = max(0, lineno - 5), min(len(lines), lineno + 6)
                for ln in lines[lo:hi]:
                    for sm in SEC_RE.finditer(ln):
                        n = int(sm.group(1))
                        if n > MAX_TOP_SEC:
                            continue
                        if sm.group(2) is not None:
                            fine.add((n, int(sm.group(2))))
                        else:
                            coarse.add(n)
                parts = rel.split(os.sep)
                d = parts[1] if len(parts) > 2 else "root"
                idx = len(tests)
                tests.append(dict(suite=suite, name=name, file=rel, dir=d,
                                  body=body))
                by_file[rel].add(idx)
                by_dir[d].add(idx)
                for k in fine:
                    fine_map[k].add(idx)
                    coarse_map[k[0]].add(idx)  # a §N.M cite also answers §N
                for n in coarse:
                    coarse_map[n].add(idx)
                for v in issues:
                    issue_map[v].add(idx)
    return dict(tests=tests, by_file=by_file, by_dir=by_dir,
                fine_map=fine_map, coarse_map=coarse_map, issue_map=issue_map,
                multiline_macros=multiline_macros)


def secs_in(texts):
    fine, coarse = set(), set()
    for t in texts:
        for m in SEC_RE.finditer(t):
            n = int(m.group(1))
            if n > MAX_TOP_SEC:
                continue
            if m.group(2) is not None:
                fine.add((n, int(m.group(2))))
            else:
                coarse.add(n)
    return fine, coarse


def gather_diff(repo, base):
    """Worktree-vs-base diff decomposed into per-file changed lines, plus the
    base..HEAD commit messages and untracked src/tests files."""
    msgs = sh(repo, "log", "--format=%B", f"{base}..HEAD")
    diff = sh(repo, "diff", "-U0", base)
    files = defaultdict(list)  # rel -> changed (+/-) lines, prefix stripped
    cur, in_hunk = None, False
    for raw in diff.split("\n"):
        if raw.startswith("diff --git "):
            m = re.search(r" b/(.+)$", raw)
            cur = m.group(1) if m else None
            in_hunk = False
            continue
        if raw.startswith("@@ "):
            in_hunk = True
            continue
        if cur is None or not in_hunk or raw.startswith("\\"):
            continue
        if raw.startswith(("+", "-")):
            files[cur].append(raw[1:])
    untracked = [p for p in sh(repo, "ls-files", "--others",
                               "--exclude-standard", "--", "src",
                               "tests").splitlines() if p]
    for rel in untracked:
        if rel.endswith((".cpp", ".hpp", ".h", ".py", ".sh")):
            try:
                files[rel].extend(
                    open(os.path.join(repo, rel), encoding="utf-8",
                         errors="replace").read().split("\n"))
            except OSError:
                pass
    return msgs, files


def select(universe, msgs, files, variant):
    v = VARIANTS[variant]
    by_signal = defaultdict(set)

    # (a) changed/untracked test files -> their tests, unconditionally
    for rel in files:
        if rel.startswith("tests/") and rel.endswith(".cpp"):
            by_signal["testfile"] |= universe["by_file"].get(rel, set())

    src_lines, src_dirs, diag_lits = [], set(), []
    for rel, changed in files.items():
        if not rel.startswith("src/"):
            continue
        src_lines += changed
        parts = rel.split("/")
        if len(parts) > 2:
            src_dirs.add(parts[1])
        for ln in changed:
            for lm in STR_LIT_RE.finditer(ln):
                lit = lm.group(1)
                if " " in lit and sum(ch.isalpha() for ch in lit) >= 8:
                    diag_lits.append(lit)

    fine, coarse = secs_in(src_lines + [msgs])

    if v["issue"]:
        for n in {int(x) for x in re.findall(r"#(\d+)", msgs)
                  if int(x) >= MAX_TOP_SEC}:
            by_signal["issue"] |= universe["issue_map"].get(n, set())
    if v["fine"]:
        for k in fine:
            by_signal["fine_sec"] |= universe["fine_map"].get(k, set())
    if v["coarse"]:
        for n in coarse | {n for n, _ in fine}:
            by_signal["coarse_sec"] |= universe["coarse_map"].get(n, set())
    if v["diag"]:
        lits = list(dict.fromkeys(diag_lits))[:MAX_DIAG_LITS]
        if len(diag_lits) > MAX_DIAG_LITS:
            print(f"note: diagnostic-literal matching capped at "
                  f"{MAX_DIAG_LITS} of {len(set(diag_lits))} literals",
                  file=sys.stderr)
        for lit in lits:
            hits = {i for i, t in enumerate(universe["tests"])
                    if lit in t["body"]}
            if 0 < len(hits) <= MAX_DIAG_HITS:
                by_signal["diag"] |= hits
    if v["dirs"]:
        tdirs = set()
        for d in src_dirs:
            tdirs |= DOWNSTREAM.get(d, set())
        for td in tdirs:
            by_signal["dir"] |= universe["by_dir"].get(td, set())

    sel = set()
    for s in by_signal.values():
        sel |= s
    info = dict(fine=sorted(fine), coarse=sorted(coarse | {n for n, _ in fine}),
                src_dirs=sorted(src_dirs),
                by_signal={k: len(s) for k, s in sorted(by_signal.items())})
    return sel, info


def run_check(repo):
    """Self-test (#2100): the invariants the selector's soundness rests on.
    Thresholds sit far below current reality (11.6k tests, 1.5k multiline
    macros, 300+ fine-§ keys, 1000+ issue keys) so suite growth never trips
    them, while a parsing regression (e.g. a line-anchored macro regex, which
    silently drops ~13% of the suite) fails immediately."""
    failures = []
    u = build_universe(repo)
    n = len(u["tests"])

    def chk(cond, what):
        if not cond:
            failures.append(what)
        print(f"  [{'ok' if cond else 'FAIL'}] {what}", file=sys.stderr)

    chk(n >= 5000, f"universe >= 5000 tests (got {n})")
    chk(len(u["by_file"]) >= 500,
        f"universe >= 500 test files (got {len(u['by_file'])})")
    chk(u["multiline_macros"] >= 200,
        f"multiline BEZEL_TEST macros parsed >= 200 (got "
        f"{u['multiline_macros']}) — a line-anchored regex drops these")
    chk(len(u["fine_map"]) >= 100,
        f"fine §N.M index >= 100 keys (got {len(u['fine_map'])})")
    chk(len(u["issue_map"]) >= 300,
        f"issue-number index >= 300 keys (got {len(u['issue_map'])})")
    f1, _, _ = name_sigs("spec_28_6_example_2")
    chk((28, 6) in f1, "name_sigs finds spec_28_6 -> fine (28,6)")
    _, _, i1 = name_sigs("owned_dict_drain_628")
    chk(628 in i1, "name_sigs finds trailing issue id 628")
    f2, _, _ = name_sigs("case_arm_meet_17_4")
    chk((17, 4) in f2, "name_sigs finds trailing _17_4 -> fine (17,4)")
    if failures:
        print(f"select_tests --check: {len(failures)} FAILURE(S)",
              file=sys.stderr)
        return 1
    print(f"select_tests --check: all invariants hold "
          f"({n} tests, {u['multiline_macros']} multiline macros)",
          file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--repo", default=None)
    ap.add_argument("--base", default=None)
    ap.add_argument("--variant", choices=sorted(VARIANTS), default="gate")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    repo = args.repo
    if repo is None:
        try:
            repo = sh(os.getcwd(), "rev-parse", "--show-toplevel").strip()
        except RuntimeError as e:
            print(f"select_tests: not in a git repo: {e}", file=sys.stderr)
            return 3

    if args.check:
        return run_check(repo)

    base = args.base
    try:
        if base is None:
            base = sh(repo, "merge-base", "HEAD", "origin/main").strip()
        base = sh(repo, "rev-parse", "--verify", f"{base}^{{commit}}").strip()
    except RuntimeError as e:
        print(f"select_tests: cannot resolve base: {e}", file=sys.stderr)
        return 3

    # Environment failures past this point (a git hiccup, an unwritable -o
    # path) are exit 3 per the documented contract, not an uncaught traceback.
    try:
        universe = build_universe(repo)
        n = len(universe["tests"])
        print(f"universe: {n} tests over {len(universe['by_file'])} files",
              file=sys.stderr)

        msgs, files = gather_diff(repo, base)
        sel, info = select(universe, msgs, files, args.variant)

        names = sorted(f"{universe['tests'][i]['suite']}."
                       f"{universe['tests'][i]['name']}" for i in sel)
        out = "\n".join(names) + ("\n" if names else "")
        if args.out:
            with open(args.out, "w") as f:
                f.write(out)
        else:
            sys.stdout.write(out)
    except (RuntimeError, OSError) as e:
        print(f"select_tests: environment failure: {e}", file=sys.stderr)
        return 3

    pct = 100.0 * len(names) / n if n else 0.0
    print(f"selected: {len(names)} of {n} ({pct:.1f}%) — variant={args.variant} "
          f"base={base[:9]}", file=sys.stderr)
    print(f"signals: {info['by_signal']}; §fine={info['fine']}; "
          f"§coarse={info['coarse']}; src_dirs={info['src_dirs']}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
