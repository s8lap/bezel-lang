#!/usr/bin/env python3
"""Run the rule corpus against a bezelc, under a baseline ratchet.

    runner.py <bezelc> [--baseline PATH] [--update] [--classify]
              [--filter SUBSTR] [--jobs N] [--report PATH]

WHY A BASELINE. Every case states the verdict the SPEC requires, written from
the document and reviewed against it, not from the compiler's behaviour. So on
the day the corpus lands a large fraction of it fails — each failure is a real
gap between the implementation and the spec, and that list is the point of the
corpus, not a defect in it. Gating on "all green" would therefore be gating on
the compiler being finished. Instead `baseline.tsv` records, per case, the
verdict the compiler produces TODAY, and the gate ratchets that:

    baseline pass -> pass    OK
    baseline pass -> fail    REGRESSION            (cycle fails)
    baseline fail -> fail    KNOWN divergence      (OK, listed)
    baseline fail -> pass    FIXED, unbanked       (cycle fails: bank it)
    no baseline row          UNCLASSIFIED          (reported; --strict-new fails)

The last-but-one rule is the same exact-not-ceiling ratchet the deprecated-JIT
check uses: unbanked headroom is indistinguishable from a rise someone already
spent. Bank a fix with `--update` in the same commit that fixes it.

`--classify` ignores the baseline, runs everything, and rewrites it from what it
observed — the regeneration protocol, run on a box with a current bezelc.
"""

import argparse
import atexit
import os
import re
import shutil
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import conformance  # noqa: E402  (sibling doc-example harness: the caged seam)
import corpus  # noqa: E402

BASELINE_COLUMNS = ("case-id", "verdict", "note")

WARNING_RE = __import__("re").compile(r'\bwarn(?:ing)?\b', __import__("re").I)

OK = "OK"
REGRESSION = "REGRESSION"
KNOWN = "KNOWN"
UNBANKED = "UNBANKED"
UNCLASSIFIED = "UNCLASSIFIED"


def _choose_workroot():
    """Where the per-run work directory lives. Per #5196, never /tmp.

    /tmp on the project's boxes is a quota-limited tmpfs: rule-corpus work
    directories accumulate when the runner is killed (SIGKILL, timeout, OOM)
    before the `finally` block can `shutil.rmtree` them, filling the quota and
    pushing swap out. The CLAUDE.md "Build toolchain" rule ("Scratch never
    goes in `/tmp`") forbids it. The default lives under the project scratch
    root at /var/tmp/bezel-scratch/rulecorpus; tests and lanes can redirect
    via BEZEL_RULECORPUS_TMPDIR (same discipline as BEZEL_DIFF_WORKROOT in
    scripts/behaviour_differential.py).
    """
    override = os.environ.get('BEZEL_RULECORPUS_TMPDIR')
    if override:
        return override
    return '/var/tmp/bezel-scratch/rulecorpus'


def read_baseline(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            cells = line.split("\t")
            if cells[0] == "case-id":
                continue
            while len(cells) < len(BASELINE_COLUMNS):
                cells.append("")
            rows[cells[0]] = {"verdict": cells[1], "note": cells[2]}
    return rows


def render_baseline(rows, header_note=""):
    out = []
    if header_note:
        for line in header_note.strip().split("\n"):
            out.append("# " + line if line else "#")
    out.append("\t".join(BASELINE_COLUMNS))
    for case_id in sorted(rows, key=corpus._sort_key):
        row = rows[case_id]
        out.append("\t".join([case_id, row["verdict"], row.get("note", "")]))
    return "\n".join(out) + "\n"


def evaluate(row, compile_rc, compile_log, run_rc, run_out, cases_dir=None):
    """Did the compiler produce the verdict the case states? Pure.
    Returns (verdict, detail) where verdict is 'pass' or 'fail'.

    `cases_dir` (optional) is the directory the case's source file was
    compiled from. A bezelc diagnostic opens with the absolute path of that
    file, so every `_snippet` call below passes it through as `strip_dir`: the
    character budget goes to the diagnostic instead of to however deep the
    caller's checkout happens to live (#4556) — the case id is already the
    row's first column, so the path carries no information the report lacks."""
    if compile_rc == 124:
        return "fail", "compile timed out"
    if conformance.is_crash(compile_rc):
        return "fail", "compiler crashed (exit {})".format(compile_rc)

    expect = row["expect"]
    if expect == "reject":
        if compile_rc != 0:
            return "pass", ""
        return "fail", "compiled, but the spec rejects this: " + \
            conformance._snippet(compile_log, 90, 200, strip_dir=cases_dir)
    if compile_rc != 0:
        return "fail", conformance._snippet(compile_log, 120, 240,
                                            strip_dir=cases_dir)
    if expect == "accept":
        return "pass", ""
    if expect == "quiet":
        # Several rules are stated as SUPPRESSIONS — "the warning is suppressed
        # when the local is declared `using var`, when it is explicitly disposed
        # or discarded, and when it is moved or returned" (§28.3). `accept`
        # cannot tell a suppressed warning from an emitted one.
        if WARNING_RE.search(compile_log or ""):
            return "fail", "the spec suppresses the diagnostic here, and the " \
                "compiler emitted one: " + conformance._snippet(
                    compile_log, 90, 200, strip_dir=cases_dir)
        return "pass", ""
    if expect == "warn":
        # The case compiles AND the compiler says something about it. Which
        # wording is the diagnostic report's business, not the gate's — same
        # tier as a reject case's, and for the same reason.
        if WARNING_RE.search(compile_log or ""):
            return "pass", ""
        return "fail", "compiled silently; the spec requires a warning here"

    # expect: run — optionally ALSO asserting a diagnostic.
    #
    # Some rules have two halves that only one program shows: §28.2's second
    # `Dispose()` is a runtime no-op AND draws the compile-time use-after-
    # dispose warning (v0.299, #4399). `warn` throws away the runtime
    # observation; `run` cannot see the diagnostic; two cases would duplicate
    # the program and let the halves drift apart. A `run` case carrying
    # `warns:` asserts both.
    if row.get("warns"):
        if not WARNING_RE.search(compile_log or ""):
            return "fail", "compiled silently; the spec requires a warning " \
                "here as well as the stated output"
    if run_rc is None:
        return "fail", "no runnable artifact was produced"
    if run_rc == 124:
        return "fail", "run timed out"
    if conformance.is_crash(run_rc):
        return "fail", "program crashed (exit {})".format(run_rc)
    want = row["check"]
    if want.startswith("all:"):
        # `output:` — every line, not just the first. Disposal ORDER is the
        # corpus's most valuable observation and it takes more than one line.
        want = want[len("all:"):].replace("\\n", "\n")
        actual = (run_out or "").replace("\r", "").rstrip("\n")
        if actual == want:
            return "pass", ""
        return "fail", "output: expected {!r}, got {!r}".format(want, actual)
    actual = conformance._first_line(run_out or "")
    if actual == want:
        return "pass", ""
    return "fail", "stdout: expected {!r}, got {!r}".format(want, actual)


def diagnostic_matches(row, compile_log):
    """A reject case's authored `diagnostic:` is the spec's wording for the rule
    it breaks, not a transcript of the compiler. A mismatch is reported, never
    gated: it means either the diagnostic does not name the rule it enforced, or
    the case tripped a different rule than it meant to. Both are work items."""
    want = (row.get("check") or "").strip().lower()
    if not want:
        return True
    return want in (compile_log or "").lower()


# per #4859: a directory case may carry .bzl files that main.bzl never
# references — extension candidates the case sets up but does not select,
# siblings of the chosen module, library fragments not on the import path.
# Linking them compiles and links symbols main.bzl never uses, and
# duplicates extension members across unselected candidates; the linker
# then errors with a multiple-definition of the shared extension symbol
# (s26_5_0016's symptom: exta.bzl and extb.bzl both define
# `bz$ext$Shapes.Shape$Describe`; only ExtA is imported; linking both .o
# files duplicates the symbol). The link list is therefore reduced to the
# *symbol-referenced* subset — main.bzl's transitive import closure, plus
# any dep file whose module name is named as a top-level identifier in
# main.bzl or in any of its imports, plus any dep file that re-declares
# main.bzl's own module name (so §2.8.1's duplicate-name rejection still
# fires rather than being silently hidden).
_MODULE_DECL_RE = re.compile(
    r'^\s*module\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\s*;', re.MULTILINE)
_IMPORT_DECL_RE = re.compile(
    r'^\s*import\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)'
    r'(?:\s+as\s+[A-Za-z_]\w*)?\s*;',
    re.MULTILINE)
_IDENT_RE = re.compile(r'[A-Za-z_]\w*')


def _module_info(path):
    """Parse a .bzl file: returns (module_name, set of imported module names,
    set of identifier tokens) or (None, set(), set()) on read failure."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except (OSError, IOError):
        return None, set(), set()
    m = _MODULE_DECL_RE.search(text)
    name = m.group(1) if m else None
    imports = set(_IMPORT_DECL_RE.findall(text))
    refs = set(_IDENT_RE.findall(text))
    return name, imports, refs


def _symbol_referenced_deps(main_path, dep_paths):
    """Return the subset of `dep_paths` that should appear on the link line.

    The link list contains every .bzl whose module name is *referenced* from
    main.bzl — directly via `import X;`, transitively through that import's
    own imports, or as a top-level identifier that names a module in the case
    directory. A dotted-name reference such as `Data.Item` names module
    `Data` via its first segment, so the segment lookup covers that case.
    Any .bzl file declaring the same module name as main.bzl is kept by the
    identifier walk (its `module X;` line names X in main's token set), so
    §2.8.1's rule-1 rejection of a root+dep module-name collision is not
    silently hidden when the dep declares the same module as main."""
    all_paths = [main_path] + list(dep_paths)
    parsed = {p: _module_info(p) for p in all_paths}
    name_to_file = {n: p for p, (n, _, _) in parsed.items() if n}
    first_seg_to_files = {}
    for p, (n, _, _) in parsed.items():
        if n:
            seg = n.split(".")[0]
            first_seg_to_files.setdefault(seg, []).append(p)

    visited = {main_path}
    queue = [main_path]
    while queue:
        path = queue.pop()
        _n, imports, refs = parsed.get(path, (None, set(), set()))
        # walk imports: include the named module (full match) and any dep
        # whose module-name first segment the import statement names
        for imp in imports:
            target = name_to_file.get(imp)
            if target and target not in visited:
                visited.add(target)
                queue.append(target)
            for seg in imp.split("."):
                for target in first_seg_to_files.get(seg, []):
                    if target not in visited:
                        visited.add(target)
                        queue.append(target)
        # walk identifier tokens: a top-level identifier that names a module
        # (full name OR first segment of a dotted module name) is a
        # reference to that module
        for ref in refs:
            target = name_to_file.get(ref)
            if target and target not in visited:
                visited.add(target)
                queue.append(target)
            for target in first_seg_to_files.get(ref, []):
                if target not in visited:
                    visited.add(target)
                    queue.append(target)
    return visited


def run_case(row, bezelc, cases_dir, tmpdir, run=conformance.caged_run):
    deps = [d for d in (row.get("deps") or "").split(";") if d]
    if deps:
        src = os.path.join(cases_dir, row["case-id"], "main.bzl")
    else:
        src = os.path.join(cases_dir, row["case-id"] + ".bzl")
    if not os.path.isfile(src):
        return "fail", "source file missing", ""
    work = os.path.join(tmpdir, row["case-id"])
    os.makedirs(work, exist_ok=True)

    # per #4859: filter the link list to the symbol-referenced subset. A
    # directory case's .bzl files include every sibling — extension
    # candidates the case sets up but does not select, library fragments
    # not on main.bzl's import path. Compiling and linking the unreferenced
    # files duplicates extension symbols across unselected candidates and
    # trips a multiple-definition error at the linker (s26_5_0016).
    if deps:
        case_dir = os.path.join(cases_dir, row["case-id"])
        dep_paths = [os.path.join(case_dir, d) for d in deps]
        reachable = _symbol_referenced_deps(src, dep_paths)
        deps = [d for d in deps
                if os.path.join(case_dir, d) in reachable]

    # A dependency is compiled to a library artifact and imported by everything
    # after it. A dependency that does not compile is a broken CASE, never a
    # passing reject: the rule under test lives in main.bzl.
    imports = []
    for dep in deps:
        artifact = os.path.join(work, dep[:-len(".bzl")] + ".o")
        dep_rc, dep_log = run([bezelc] + imports + ["-c", "-o", artifact,
                                                   os.path.join(cases_dir,
                                                                row["case-id"],
                                                                dep)])
        if dep_rc != 0:
            return "fail", "dependency {} did not compile: {}".format(
                dep, conformance._snippet(dep_log, 90, 200,
                                          strip_dir=cases_dir)), dep_log
        imports += ["--import", artifact]

    out_exe = os.path.join(work, "out")
    mode = [f for f in (row.get("flags") or "").split() if f]
    flags = ["-c", "-o", out_exe] if row["shape"] == "lib" else ["-o", out_exe]
    compile_rc, compile_log = run([bezelc] + mode + imports + flags + [src])

    run_rc, run_out = None, ""
    if compile_rc == 0 and row["shape"] == "program" and row["expect"] == "run":
        if os.path.exists(out_exe) and os.access(out_exe, os.X_OK):
            run_rc, run_out = run([out_exe])
    verdict, detail = evaluate(row, compile_rc, compile_log, run_rc, run_out,
                               cases_dir=cases_dir)
    return verdict, detail, compile_log


def compare(verdict, baseline_row):
    if baseline_row is None:
        return UNCLASSIFIED
    banked = baseline_row["verdict"]
    if banked == "pass":
        return OK if verdict == "pass" else REGRESSION
    return UNBANKED if verdict == "pass" else KNOWN


def main(argv, base_dir=None, run=conformance.caged_run):
    base = base_dir or os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(prog="runner.py")
    ap.add_argument("bezelc")
    ap.add_argument("--baseline", default=os.path.join(base, "baseline.tsv"))
    ap.add_argument("--update", action="store_true",
                    help="bank the observed verdicts into the baseline")
    ap.add_argument("--classify", action="store_true",
                    help="bank observed verdicts into the baseline (starts from "
                         "existing baseline; --filter X narrows the run, only "
                         "matching rows are updated — #4814)")
    ap.add_argument("--strict-new", action="store_true",
                    help="fail on any case with no baseline row")
    ap.add_argument("--filter", default="",
                    help="only cases whose id or section contains this")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--report", default="")
    args = ap.parse_args(argv)

    if not os.access(args.bezelc, os.X_OK):
        print("ERROR: bezelc not executable: " + args.bezelc, file=sys.stderr)
        return 2

    manifest = os.path.join(base, "manifest.tsv")
    if not os.path.exists(manifest):
        print("ERROR: manifest.tsv missing — run `corpus.py build`", file=sys.stderr)
        return 2
    rows = corpus.read_manifest(manifest)
    if args.filter:
        rows = [r for r in rows
                if args.filter in r["case-id"] or args.filter in r["section"]]
    if not rows:
        print("ERROR: no cases selected", file=sys.stderr)
        return 2

    baseline = read_baseline(args.baseline)
    cases_dir = os.path.join(base, "cases")
    workroot = _choose_workroot()
    os.makedirs(workroot, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="rulecorpus-", dir=workroot)
    # Belt-and-braces: the try/finally below handles every exit path inside
    # main(), but atexit fires at interpreter shutdown too (sys.exit,
    # uncaught exception, KeyboardInterrupt). Together they cover every
    # path except SIGKILL, which no Python can recover from. The primary fix
    # is moving the work dir off /tmp (#5196); this is the cleanup side.
    atexit.register(shutil.rmtree, tmpdir, ignore_errors=True)
    results = {}
    diag_misses = []
    try:
        def work(row):
            return row, run_case(row, args.bezelc, cases_dir, tmpdir, run=run)
        if args.jobs > 1:
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                done = list(pool.map(work, rows))
        else:
            done = [work(row) for row in rows]
        for row, (verdict, detail, log) in done:
            status = UNCLASSIFIED if args.classify else \
                compare(verdict, baseline.get(row["case-id"]))
            results[row["case-id"]] = (row, verdict, detail, status)
            if row["expect"] in ("reject", "warn") and verdict == "pass" \
                    and not diagnostic_matches(row, log):
                diag_misses.append((row["case-id"], row["check"],
                                    conformance._first_line(log)))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    counts = {k: 0 for k in (OK, REGRESSION, KNOWN, UNBANKED, UNCLASSIFIED)}
    lines = []
    for case_id in sorted(results, key=corpus._sort_key):
        row, verdict, detail, status = results[case_id]
        counts[status] += 1
        if status in (OK, KNOWN) and not args.report:
            continue
        lines.append("{:<12} {:<14} {:<10} {}".format(
            status, case_id, row["expect"], detail or row["title"]))
    for line in lines:
        print(line)

    if args.classify or args.update:
        banked = dict(baseline)
        for case_id, (row, verdict, detail, _status) in results.items():
            note = "" if verdict == "pass" else \
                (row["issue"] and "#" + row["issue"] or "divergence")
            banked[case_id] = {"verdict": verdict, "note": note}
        note = ("Observed verdicts, banked by runner.py. A `fail` row is a "
                "recorded divergence between the spec and the implementation, "
                "not a case to edit. See README.md.")
        with open(args.baseline, "w", encoding="utf-8") as f:
            f.write(render_baseline(banked, note))
        print("banked {} verdicts into {}".format(len(banked), args.baseline))

    if diag_misses:
        print("\n{} reject/warn cases whose diagnostic does not carry the "
              "rule's wording:".format(len(diag_misses)))
        for case_id, want, got in diag_misses[:40]:
            print("  {}  want ~{!r}  got {!r}".format(case_id, want, got))

    total = len(results)
    print("\n{} cases: {} ok, {} known-divergent, {} unclassified, "
          "{} REGRESSION, {} UNBANKED".format(
              total, counts[OK], counts[KNOWN], counts[UNCLASSIFIED],
              counts[REGRESSION], counts[UNBANKED]))

    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            for case_id in sorted(results, key=corpus._sort_key):
                row, verdict, detail, status = results[case_id]
                f.write("\t".join([case_id, row["section"], row["expect"],
                                   verdict, status, detail.replace("\t", " "),
                                   row["title"]]) + "\n")
        print("report written to " + args.report)

    if args.classify:
        return 0
    if counts[REGRESSION] or counts[UNBANKED]:
        return 1
    if args.strict_new and counts[UNCLASSIFIED]:
        return 1
    return 0


if __name__ == "__main__":
    conformance.enter_run_cage(__file__, sys.argv[1:])
    sys.exit(main(sys.argv[1:]))
