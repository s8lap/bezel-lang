"""Cheap (stat-only, no build) check that the built binaries are not older than
the build inputs a gate would test.

R39 gate 1: build/bezelc had been rebuilt but build/tests/bezel_tests_support
predated the test-token hook, so four gate self-tests skipped and the cycle
failed 8 minutes in. pregate.sh runs this as its `binaries` row.

A binary is STALE when it is missing, or its mtime is older than the newest
MTIME of an actual build input: every file under src/ and cmake/, the C++
sources under tests/ (*.cpp *.hpp *.h), and every CMakeLists.txt. Commit time
is deliberately NOT used: a commit that touches only non-build files (corpus,
lit, docs) must not turn the row red when the build is up to date (the remedy
would be a no-op and the row would stay FAIL forever), and edit, build, then
commit is the normal lane order. Same idea as pregate's `find src -newer` row.

  python3 scripts/gate/binary_freshness.py [--root DIR] [--build-dir build]
Exit 0 fresh, 1 stale/missing (one line per offender on stdout), 2 usage.
"""
import argparse
import datetime
import os
import sys

REMEDY = "scripts/dev_heavy.sh build cmake --build build --target bezel_tests bezelc"
_CPP = (".cpp", ".hpp", ".h")


def _inputs(root):
    """Yield every build-input path under root (build/ and .git excluded)."""
    for top in ("src", "cmake", "tests"):
        for dp, dns, fns in os.walk(os.path.join(root, top)):
            for fn in fns:
                if fn == "CMakeLists.txt" or top != "tests" or fn.endswith(_CPP):
                    yield os.path.join(dp, fn)
    yield os.path.join(root, "CMakeLists.txt")


def newest_input_time(root):
    """Return (epoch seconds, description) of the newest build input's mtime."""
    newest, what = 0.0, "no build inputs found"
    for p in _inputs(root):
        try:
            m = os.stat(p).st_mtime
        except OSError:
            continue
        if m > newest:
            newest, what = m, "newest build input " + os.path.relpath(p, root)
    return newest, what


def binaries(root, build_dir="build"):
    """The binaries to check: build/bezelc plus every manifest entry."""
    bins = [os.path.join(root, build_dir, "bezelc")]
    manifest = os.path.join(root, build_dir, "tests", "test_binaries.txt")
    try:
        with open(manifest) as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 2 and parts[1]:
                    bins.append(parts[1])
    except OSError:
        bins.append(manifest)  # a missing manifest is itself a failure
    return bins


def _fmt(t):
    return datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S")


def check(bins, threshold, what=""):
    """Return a list of human-readable problems (empty = all fresh)."""
    problems = []
    for b in bins:
        try:
            m = os.stat(b).st_mtime
        except OSError:
            problems.append("%s is MISSING" % b)
            continue
        if m < threshold:
            problems.append("%s built %s is OLDER than %s (%s)"
                            % (b, _fmt(m), _fmt(threshold), what))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..")))
    ap.add_argument("--build-dir", default="build")
    a = ap.parse_args(argv)
    threshold, what = newest_input_time(a.root)
    problems = check(binaries(a.root, a.build_dir), threshold, what)
    for p in problems:
        print(p)
    if problems:
        print("remedy: " + REMEDY)
        return 1
    print("%d binaries newer than %s" % (len(binaries(a.root, a.build_dir)), _fmt(threshold)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
