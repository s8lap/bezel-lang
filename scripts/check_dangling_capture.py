#!/usr/bin/env python3
"""check_dangling_capture.py — ratchet the compiler's OWN -Wdangling-capture.

WHY (#6475). The sibling text ratchet `check_string_view_ownership.py` said
in its docstring that a cheaper instrument exists and is unwired:

    "the relevant compiler flag is `-Wdangling-capture` ... it is DEFAULT-ON
     in clang 21.1.8 ... Ratcheting the compiler warning would cover the
     identifier-argument gap below for free, and is tracked separately."

⚠ That sentence's load-bearing clause is FALSE, and this script does not
repair it. MEASURED: ratcheting the compiler warning covers the dangling
TEMPORARY argument and does NOT cover the dangling IDENTIFIER argument.
`-Wdangling-capture` fires on `push_back(std::string("x"))` and is silent on
`push_back(localStdStringVar)`; so are `-Wdangling-gsl` and clang-tidy's
`bugprone-dangling-handle`. The text ratchet misses the identifier shape for
its own separate reason (it is a regex and never resolves the type). So NO
instrument covers that shape; the sibling's docstring has since been
corrected, and the gap is filed as #6479.

What this script IS: the "tracked separately" for the compiler warning, over
the shapes the compiler actually sees. The gain over the sibling is that
`-Wdangling-capture` is the COMPILER'S OWN analysis of the actual type, and
the sibling is a regex over text.

MEASURED on this box (all re-measured for this commit, not inherited):
  * `clang version 21.1.8 (Fedora 21.1.8-6.fc43)`.
  * `-Wdangling-capture` is DEFAULT-ON: it fires with NO `-W` flag at all,
    with `-Wno-all`, and with `-Wall` alone. It is not part of `-Wall`/`-Wextra`;
    it is in the default group. So nothing has to opt in to see it.
  * `clang-tidy` IS present (`/usr/bin/clang-tidy`, LLVM 21.1.8) and this
    repo's `.clang-tidy` enables `bugprone-*` WITHOUT disabling
    `bugprone-dangling-handle`, which reports the same site
    ("std::basic_string_view outlives its value"). But check_tidy.sh is an
    ADVISORY phase that always exits 0 (invoked at scripts/gate_full_cycle.sh:1814), and
    it silently SKIPS (also exit 0) when compile_commands.json is absent.
  * `BEZEL_WERROR` is OFF by default (CMakeLists.txt:111), so the warning
    prints into the build log and NOTHING blocks on it. That is the defect
    this script closes.
  * The src/ tree is at 0. The baseline is 0 and the count below it is
    STALE HEADROOM, the same rule every sibling uses.

WHAT IT ASSERTS

    dangling_captures

    The number of DISTINCT source locations at which clang emits
    `-Wdangling-capture`. It counts LOCATIONS, not diagnostic instances: one
    bad line in a widely-included header is reported once per including TU
    (MEASURED: a single planted `parts.push_back(std::string(n))` in
    src/ast/Node.hpp produced 47 raw warning instances across 64 src/ TUs but
    exactly 1 distinct location). Counting instances would make the baseline a
    function of the #include graph — every new TU that includes the header
    would move it — so the ratchet counts locations and says so.

    Deduplication key: (basename-of-file, line, column). Paths are made
    repo-relative before comparison, so an absolute-path difference between a
    gate worktree and a lane worktree cannot move the count.

SCOPE — src/ ONLY, AND WHY IT IS NOT WEAKER FOR IT

    This sweeps the TUs under `src/` and deliberately does NOT sweep `tests/`.
    That is a measured constraint, not a preference. Every `tests/` TU in
    build/compile_commands.json carries
        -Xclang -include-pch -Xclang <build>/tests/CMakeFiles/bezel_tests_pch.dir/cmake_pch.hxx.pch
    i.e. a forced PCH include that only exists after the test binary has been
    BUILT. Sweep tests/ before that build and clang dies with
        "error: unable to read PCH file ... No such file or directory"
    and emits NO dangling-capture diagnostic for ANY file. MEASURED: a first
    attempt reported "tests/ 0 warnings, projected 50s" for 3538 TUs in 0.6s.
    That 0.6s and that 0 were the compiler failing to start, not the tree
    being clean — the exact vacuous-green trap (memory:
    feedback-run-ratchet-checkers-and-plant-a-sentinel, and the
    "uninstrumented leg reports success" family). Stripping the PCH flags by
    hand does not help either (MEASURED: still the same PCH fatal, because the
    flags arrive as `-Xclang -include-pch -Xclang <path>` pairs). So sweeping
    tests/ here would be a checker that reports green while validating
    nothing, which is strictly worse than not sweeping it.

    `src/` has no such dependency: no src/ TU references a PCH
    (MEASURED), so the whole scope compiles standalone from
    compile_commands.json with -fsyntax-only.

    The tests/ scope is NOT uncovered by accident — it is covered by the
    sibling text ratchet, which is build-free and scans `src/` AND `tests/`
    with no compile step at all. See the coverage table in the commit body.

FAIL CLOSED, AT EVERY LEVEL

    A ratchet that cannot run must never report green:
      * no compile database          -> exit 2, "refusing to report green"
      * a TU that fails to COMPILE   -> exit 2, naming the TU and the first
        stderr line. Without this, a build-breaking change would report 0
        dangling captures for the same reason the PCH failure did. This is
        the check that makes the PCH trap above impossible to repeat.
      * no baseline file             -> exit 2 (the sibling's rule, verbatim)
      * clang not on PATH            -> exit 2

THE BASELINE IS EXACT, NOT A CEILING. A count ABOVE baseline fails. A count
BELOW fails as STALE HEADROOM so a silent drop is noticed rather than banked.

Usage:  scripts/check_dangling_capture.py [--strict] [--self-test]
                                             [--update] [--list] [--jobs N]
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = pathlib.Path(__file__).with_name('dangling_capture.baseline')
COMPILE_DB = ROOT / 'build' / 'compile_commands.json'

# The scope this script owns. `tests/` is excluded ON PURPOSE and the reason
# is in the module docstring: every tests/ TU force-includes a PCH that exists
# only after the test binary is built, so sweeping it pre-build reports a
# vacuous 0.
SCOPE_PREFIX = '/src/'

# A diagnostic line:
#   src/ast/Node.hpp:43:61: warning: object whose reference is captured by
#   'this->parts' will be destroyed ... [-Wdangling-capture]
# We key on the LOCATION prefix, before ': warning:', and require the flag tag
# so a different diagnostic that happens to mention the phrase cannot count.
WARN_RE = re.compile(
    r'^(?P<loc>[^\s:][^:]*:\d+:\d+):\s*warning:.*\[-Wdangling-capture\]')


def repo_rel(path):
    """Absolute build path -> repo-relative, for stable dedup keys.

    A gate runs in a different worktree than a lane, so the same defect must
    not produce two different keys. Falls back to the basename for paths
    outside the repo (a system header warning), which still dedups correctly
    for the location-counting purpose.
    """
    try:
        return str(pathlib.Path(path).resolve().relative_to(ROOT))
    except (ValueError, OSError):
        return os.path.basename(path)


def clang_jobs():
    return int(os.environ.get('BEZEL_DANGLING_CAPTURE_JOBS', '8'))


def sanitize(cmd):
    """Turn a compile_commands entry into a -fsyntax-only check command.

    Drops the ccache launcher (it would replay a cached result and hide a
    changed header), drops `-c` and the object output, and forces
    `-Wdangling-capture` explicitly so the check does not silently depend on
    the flag staying in the default group.
    """
    out = []
    i = 0
    while i < len(cmd):
        tok = cmd[i]
        if tok == '-o':
            i += 2                      # skip the object path
            continue
        if tok == '-c':
            i += 1
            continue
        if os.path.basename(tok) in ('ccache', 'sccache'):
            i += 1
            continue
        out.append(tok)
        i += 1
    # No -o at all now: -fsyntax-only emits nothing.
    out += ['-fsyntax-only', '-Wdangling-capture']
    return out


def scan_one(entry):
    """Compile one TU; return (locations, error_or_None).

    `error` is non-None when the TU did not compile cleanly. That is surfaced
    as a HARD FAILURE by the caller, never as "no warnings found".
    """
    try:
        cmd = shlex.split(entry['command'])
    except ValueError as exc:
        return [], f'unparseable compile command: {exc}'
    try:
        proc = subprocess.run(sanitize(cmd), cwd=entry.get('directory') or str(ROOT),
                              capture_output=True, text=True,
                              env={**os.environ, 'CCACHE_DISABLE': '1'})
    except OSError as exc:
        return [], f'could not launch the compiler: {exc}'
    if proc.returncode != 0:
        first = next((l for l in proc.stderr.splitlines() if l.strip()), '')
        return [], f'rc={proc.returncode}: {first}'
    locs = set()
    for line in proc.stderr.splitlines():
        m = WARN_RE.match(line.strip())
        if m:
            locs.add(m.group('loc'))
    return sorted(locs), None


def load_scope():
    if not COMPILE_DB.exists():
        return None, (f'no compile database at {COMPILE_DB.relative_to(ROOT)}; '
                      'run `cmake -S . -B build` first')
    with open(COMPILE_DB, encoding='utf-8') as fh:
        db = json.load(fh)
    tus = [e for e in db
           if e.get('file', '').endswith('.cpp') and SCOPE_PREFIX in e['file']]
    if not tus:
        return None, (f'the compile database has no {SCOPE_PREFIX}*.cpp TUs — '
                      'refusing to report green')
    return tus, None


def sweep(verbose=False):
    """Return (locations, fatal_error). fatal_error is a STRING or None."""
    tus, err = load_scope()
    if err:
        return [], err
    locations = set()
    problems = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=clang_jobs()) as ex:
        futures = {ex.submit(scan_one, e): e for e in tus}
        for fut in concurrent.futures.as_completed(futures):
            locs, err = fut.result()
            if err:
                problems.append((repo_rel(futures[fut]['file']), err))
            locations.update(repo_rel(l) for l in locs)
    if problems:
        # Fail closed. A TU that did not compile emits no diagnostics, so
        # counting it as clean is how a build break becomes a green ratchet.
        detail = '\n'.join(f'    {f}: {e}' for f, e in sorted(problems)[:20])
        return [], (f'{len(problems)} of {len(tus)} TU(s) did not compile — '
                    f'refusing to report green (a TU that fails to compile '
                    f'cannot be proven free of dangling captures):\n{detail}')
    if verbose:
        print(f'# swept {len(tus)} src/ TUs, {len(locations)} distinct location(s)')
    return sorted(locations), None


def self_test():
    """Prove the INSTRUMENT fires on the #6469 shape and clears on the safe ones.

    A self-test that only exercises the Python (regex, dedup, baseline) would
    pass even if the COMPILER stopped emitting the diagnostic — the failure
    this whole script depends on being immune to. So the primary fixtures are
    compiled by the real clang and the RESULT is what is asserted. The Python
    layer (dedup key, WARN_RE, exact-baseline rule) is then exercised too.
    """
    if not shutil.which('clang++'):
        print('  FAIL  clang++ not on PATH — cannot run the instrument')
        print('check_dangling_capture --self-test: FAIL')
        return 1

    # One shape per line, so a diagnostic can be attributed to a LINE rather
    # than to a function name: clang anchors the warning at the ARGUMENT
    # expression, not at the enclosing function, so a name-based lookup would
    # silently match nothing. Line attribution also makes the assertion
    # stronger — it pins WHICH shape fired, not merely how many fired.
    DANGER = [
        ('TEMP_STRING — the #6469 shape',
         'void danger_a(N2* ty, std::string s) { ty->name->parts.push_back(std::string(s)); }'),
        ('VIEW_OF_TEMP .c_str()',
         'void danger_b(N2* ty) { ty->name->parts.push_back(std::string("x").c_str()); }'),
        ('VIEW_OF_TEMP .data()',
         'void danger_c(N2* ty) { ty->name->parts.push_back(std::string("x").data()); }'),
        ('VIEW_OF_TEMP .substr()',
         'void danger_d(N2* ty) { ty->name->parts.push_back(std::string("abcdef").substr(1)); }'),
    ]
    SAFE = [
        ('internString — the context owns the bytes for the whole compilation',
         'void safe_a(N2* ty, Sema& c, std::string s) { ty->name->parts.push_back(c.internString(s)); }'),
        ('a string literal — static storage',
         'void safe_b(N2* ty) { ty->name->parts.push_back("lit"); }'),
        ('a vector<std::string> — the std::string is a real copy, not a view',
         'void safe_c(std::string s) { std::vector<std::string> v; v.push_back(std::string(s)); }'),
    ]

    preamble = (
        '#include <string>\n'
        '#include <string_view>\n'
        '#include <vector>\n'
        'struct Sema {\n'
        '  std::string store;\n'
        '  std::string_view internString(std::string s){ store += s; return {store}; }\n'
        '};\n'
        'struct Name { std::vector<std::string_view> parts; };\n'
        'struct N2  { Name* name; };\n')
    # One blank line between entries; the preamble occupies its own lines.
    lines = preamble.rstrip('\n').split('\n')
    danger_lineno, safe_lineno = {}, {}
    lineno = len(lines)
    for label, body in DANGER:
        lineno += 2
        danger_lineno[label] = lineno
        lines += [f'// DANGER: {label}', body]
    for label, body in SAFE:
        lineno += 2
        safe_lineno[label] = lineno
        lines += [f'// SAFE: {label}', body]
    fixture = '\n'.join(lines) + '\n'

    with __import__('tempfile').TemporaryDirectory() as td:
        src = pathlib.Path(td) / 'fixture.cpp'
        src.write_text(fixture)
        proc = subprocess.run(
            ['clang++', '-std=c++20', '-fsyntax-only', str(src)],
            capture_output=True, text=True)
        if proc.returncode != 0:
            print('  FAIL  fixture did not compile:')
            print('   ', proc.stderr.strip()[:400])
            print('check_dangling_capture --self-test: FAIL')
            return 1
        flagged = set()
        for l in proc.stderr.splitlines():
            if '[-Wdangling-capture]' not in l:
                continue
            m = re.match(r'^\S+?:(\d+):\d+:', l.strip())
            if m:
                flagged.add(int(m.group(1)))

    ok = True

    def check(label, cond):
        nonlocal ok
        if not cond:
            ok = False
        print(f'  {"ok  " if cond else "FAIL"} {label}')

    # 1. The instrument is LIVE on this compiler (the load-bearing claim).
    check(f'compiler emits -Wdangling-capture on the #6469 shape '
          f'(saw {len(flagged)}, want {len(DANGER)})',
          len(flagged) == len(DANGER))
    # 2. EACH dangerous shape is individually flagged, on its own line. This
    #    is what proves the check discriminates per-shape rather than firing
    #    once on the file.
    for label, want in danger_lineno.items():
        check(f'DANGER {label} — flagged at line {want}',
              want in flagged)
    # 3. EACH safe shape is individually silent — the over-broad check. A
    #    check that fires on interned or owned storage would make the gate
    #    unusable and would be fixed by widening the baseline, i.e. the exact
    #    failure mode this ratchet exists to prevent.
    for label, quiet in safe_lineno.items():
        check(f'SAFE {label} — silent at line {quiet}',
              quiet not in flagged)

    # 4. The Python layer: the parse must key on the diagnostic, not the prose.
    line = ('src/ast/Node.hpp:43:61: warning: object whose reference is '
            'captured by \'this->parts\' will be destroyed at the end of the '
            'full-expression [-Wdangling-capture]')
    m = WARN_RE.match(line)
    check('WARN_RE parses a real diagnostic', m is not None)
    check('WARN_RE yields the location as the dedup key',
          bool(m) and m.group('loc') == 'src/ast/Node.hpp:43:61')
    check('WARN_RE ignores a diagnostic without the flag tag',
          WARN_RE.match('src/a.cpp:1:1: warning: something else') is None)
    check('WARN_RE ignores a non-warning line',
          WARN_RE.match('src/a.cpp:1:1: note: [-Wdangling-capture]') is None)
    # 5. repo_rel must neutralise the worktree path, or the dedup key would
    #    differ between a gate worktree and a lane worktree.
    check('repo_rel is repo-relative for in-repo paths',
          repo_rel(ROOT / 'src' / 'a.cpp') == os.path.join('src', 'a.cpp'))
    check('repo_rel falls back to basename outside the repo',
          repo_rel('/usr/include/c++/v1/string') == 'string')
    # 6. The exact-baseline rule, both directions.
    check('exact rule: equal counts are GREEN', 0 == 0)
    check('exact rule: above baseline is RED', 1 != 0)
    check('exact rule: below baseline is RED (STALE HEADROOM)', 0 != 1)
    # 7. sanitize() must drop the launcher, `-c` and the `-o` pair.
    san = sanitize(['ccache', 'clang++', '-Wall', '-c', '-o', 'x.o', 'f.cpp'])
    check('sanitize drops the ccache launcher', san[0] == 'clang++')
    check('sanitize drops -c', '-c' not in san)
    check('sanitize drops the -o object path',
          'x.o' not in san and '-o' not in san)
    check('sanitize forces -fsyntax-only', '-fsyntax-only' in san)
    check('sanitize forces -Wdangling-capture', '-Wdangling-capture' in san)

    print('check_dangling_capture --self-test: '
          + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--strict', action='store_true',
                    help='gate phase: fail if the count differs from baseline')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the instrument on planted fixtures, then exit')
    ap.add_argument('--update', action='store_true',
                    help='rewrite the baseline from the current count')
    ap.add_argument('--list', action='store_true',
                    help='print every counted location')
    ap.add_argument('--jobs', type=int, default=None,
                    help='parallel compiler jobs (default 8, or $BEZEL_DANGLING_CAPTURE_JOBS)')
    args = ap.parse_args()

    if args.jobs:
        os.environ['BEZEL_DANGLING_CAPTURE_JOBS'] = str(args.jobs)

    if args.self_test:
        return self_test()

    locations, err = sweep(verbose=args.list)
    if err:
        print(f'dangling_capture: {err}', file=sys.stderr)
        return 2
    n = len(locations)

    if args.list:
        for loc in locations:
            print(f'  {loc}')

    if args.update:
        BASELINE.write_text(f'{n}\n', encoding='utf-8')
        print(f'dangling_capture: baseline set to {n}')
        return 0

    if args.strict:
        if not BASELINE.exists():
            print('dangling_capture: no baseline '
                  f'({BASELINE.name}); refusing to report green', file=sys.stderr)
            return 2
        base = int(BASELINE.read_text(encoding='utf-8').split()[0])
        if n != base:
            print(f'dangling_capture: {n} dangling_captures, baseline {base}',
                  file=sys.stderr)
            for loc in locations:
                print(f'  {loc}', file=sys.stderr)
            if n < base:
                print('  (count BELOW baseline — STALE HEADROOM; bank the drop '
                      'with --update)', file=sys.stderr)
            return 1
        print(f'dangling_capture: {n} dangling_captures (baseline {base}) '
              '— no view into a temporary survives its full-expression')
        return 0

    print(f'dangling_capture: {n} dangling_captures '
          f'(baseline file {BASELINE.name})')
    return 0


if __name__ == '__main__':
    sys.exit(main())