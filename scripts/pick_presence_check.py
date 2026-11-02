#!/usr/bin/env python3
"""Is a commit's CONTENT already in a tip? Fails LOUDLY when a file is not in the tip.

WHY THIS EXISTS. R36 (2026-10-28): a coordinator asked "is this commit's content
already in the tip?" with a script that built a dict of the tip's text for a
FIXED list of four files and then, for each added line of a commit, checked
`if cur in merged and t not in merged[cur]`.

=> THE `cur in merged` GUARD MADE AN UNKNOWN FILE COUNT AS PRESENT. The commit
`b85f4503` (the imported-symbol SEAM fix, `src/sema/TypeResolution_Checks.cpp`)
was NOT in the four-file list, so its eight added lines were never compared
against anything, and the script printed `MISSING FROM TIP=0` -- A FALSE "IT IS
ALREADY PICKED". The seam fix was the GATE BLOCKER
(`scripts/check_imported_symbol_reads.py` is a gate phase), so the false answer
would have shipped a RED GATE.

THE FIX, and this script's shape:
  * BUILD THE FILE SET FROM THE COMMITS THEMSELVES (`git show --name-only`),
    NOT from a hand-written list -- so the set cannot be narrower than the
    input; and
  * MAKE AN UNKNOWN FILE FATAL rather than silently skipped. A check that
    silently skips what it was not given reports a clean pass for the very
    thing it did not examine. A checker that can only say "clean" for a subset
    of its inputs is worse than one that says "I did not look".

Usage:
    scripts/pick_presence_check.py <tip-ref> <commit> [<commit> ...]
    scripts/pick_presence_check.py --help
    scripts/pick_presence_check.py --self-test

Exit 0 = every added line of every commit is present in the tip's version of
that file. Exit 1 = at least one added line is missing (printed, with the file
and the line). Exit 2 = a file the commit touches does not exist at the tip, or
a commit does not exist -- the check refuses to answer, it does not answer
"present".
"""
import argparse
import os
import subprocess
import sys
import tempfile


def git(args, cwd=None):
    return subprocess.run(["git"] + args, capture_output=True, text=True, cwd=cwd)


def files_of(commits, cwd=None):
    """The set of files the commits touch -- built from the commits, not a list."""
    out = set()
    for c in commits:
        r = git(["show", "--name-only", "--format=", c], cwd=cwd)
        if r.returncode != 0:
            sys.stderr.write("pick_presence_check: cannot read commit %s: %s\n"
                             % (c, r.stderr.strip()))
            raise SystemExit(2)
        for line in r.stdout.split("\n"):
            if line.strip():
                out.add(line.strip())
    return sorted(out)


def tip_text(tip, path, cwd=None):
    """The tip's version of a path, or None if the tip has no such path."""
    if git(["cat-file", "-e", "%s:%s" % (tip, path)], cwd=cwd).returncode != 0:
        return None
    return git(["show", "%s:%s" % (tip, path)], cwd=cwd).stdout


def added_lines(commit, cwd=None):
    """-> [(path, line)] for every non-blank line the commit adds."""
    r = git(["show", commit, "--unified=0"], cwd=cwd)
    if r.returncode != 0:
        sys.stderr.write("pick_presence_check: cannot read commit %s: %s\n"
                         % (commit, r.stderr.strip()))
        raise SystemExit(2)
    cur = None
    out = []
    for line in r.stdout.split("\n"):
        if line.startswith("+++ b/"):
            cur = line[6:]
            continue
        if line.startswith("+") and not line.startswith("+++"):
            text = line[1:].rstrip()
            if text.strip():
                out.append((cur, text))
    return out


def check(tip, commits, cwd=None):
    files = files_of(commits, cwd=cwd)
    tip_files = {}
    for f in files:
        text = tip_text(tip, f, cwd=cwd)
        if text is None:
            print("!! %s DOES NOT EXIST AT %s -- refusing to call anything present"
                  % (f, tip))
            return 2
        tip_files[f] = text

    print("tip=%s  files checked: %d" % (tip, len(files)))
    rc = 0
    for c in commits:
        lines = added_lines(c, cwd=cwd)
        missing = []
        for path, text in lines:
            if path not in tip_files:
                # Unreachable by construction -- the file set comes from the
                # commits -- and kept deliberately: a file that is not in the
                # dict must be FATAL, never quietly treated as present.
                print("!! %s: FILE %s WAS NOT IN THE DICT -- REFUSING TO CALL IT PRESENT"
                      % (c, path))
                return 2
            if text not in tip_files[path]:
                missing.append((path, text))
        print("%-12s added=%-4d MISSING FROM TIP=%d" % (c, len(lines), len(missing)))
        for path, text in missing[:4]:
            print("      ", path, "|", text[:100])
        if missing:
            rc = 1
    return rc


def self_test():
    """Pin the incident: an unknown file must be FATAL, not silently 'present'."""
    with tempfile.TemporaryDirectory() as d:
        def g(*args):
            r = git(list(args), cwd=d)
            if r.returncode != 0:
                sys.stderr.write(r.stderr)
                raise AssertionError("git %s failed" % (args,))
            return r.stdout

        g("init", "-q")
        g("config", "user.email", "t@example.com")
        g("config", "user.name", "t")
        os.makedirs(os.path.join(d, "src"))
        with open(os.path.join(d, "src", "a.txt"), "w") as f:
            f.write("one\n")
        g("add", "-A")
        g("commit", "-q", "-m", "first")
        tip = g("rev-parse", "HEAD").strip()

        # A commit that touches a file the tip does NOT have.
        with open(os.path.join(d, "src", "b.txt"), "w") as f:
            f.write("two\n")
        g("add", "-A")
        g("commit", "-q", "-m", "second")
        other = g("rev-parse", "HEAD").strip()

        ok = True

        # 1. The commit whose content IS in the tip: no missing lines.
        rc = check(tip, [other], cwd=d)
        # b.txt does not exist at `tip`, so this must refuse, not pass.
        if rc != 2:
            print("FAIL: a file absent from the tip must exit 2, got %d" % rc)
            ok = False
        else:
            print("ok: a file absent from the tip is FATAL (exit 2), not 'present'")

        # 2. The first commit IS in the tip by construction -> 0 missing.
        first = g("rev-list", "--max-parents=0", "HEAD").strip()
        rc = check(tip, [first], cwd=d)
        if rc != 0:
            print("FAIL: a commit already in the tip must exit 0, got %d" % rc)
            ok = False
        else:
            print("ok: a commit already in the tip reports 0 missing")

        # 3. A commit whose added line is absent from the tip -> exit 1, and
        #    the missing line is NAMED (never a bare count).
        with open(os.path.join(d, "src", "a.txt"), "w") as f:
            f.write("one\ntwo\n")
        g("add", "-A")
        g("commit", "-q", "-m", "third")
        third = g("rev-parse", "HEAD").strip()
        rc = check(tip, [third], cwd=d)
        if rc != 1:
            print("FAIL: a missing line must exit 1, got %d" % rc)
            ok = False
        else:
            print("ok: a missing line is reported as missing (exit 1)")

    print("pick_presence_check self-test: %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(
        description="Is a commit's content already in a tip? Fails loudly on an unknown file.")
    ap.add_argument("--self-test", action="store_true",
                    help="run the self-test and exit")
    ap.add_argument("tip", nargs="?", help="the tip ref to compare against")
    ap.add_argument("commits", nargs="*", help="the commits to look for")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.tip or not args.commits:
        ap.print_usage(sys.stderr)
        sys.stderr.write("pick_presence_check: need a tip ref and at least one commit\n")
        return 2
    return check(args.tip, args.commits)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
