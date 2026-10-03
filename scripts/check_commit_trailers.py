#!/usr/bin/env python3
"""Every commit in a range must end with exactly ONE trailer, and it must be
the Co-Authored-By line CLAUDE.md mandates, naming the model that wrote it.

CLAUDE.md: "End every commit message with exactly one trailer line and nothing
else -- no `Signed-off-by`, no second trailer."

This exists because the rule is violated *systematically*, not occasionally:
five lanes in cycle 17 emitted `Co-Authored-By: Claude Sonnet 5` plus a second
`Claude-Session:` line, because a host-injected reminder tells them to and they
followed it over CLAUDE.md. CLAUDE.md wins (the reminder itself says so), but a
rule that is re-violated by every fresh agent is not enforceable by prose -- the
same lesson `check_deprecated_jit_oracle.py` records: "A prose ratchet is not a
ratchet."

The model name is not pinned (it was "Claude Opus 5" verbatim until 2026-10-04):
an exact string went stale with every model release and made every Sonnet
lane's commit claim Opus authorship. The checked invariants are the ones the
rule exists for: one trailer, last line, Co-Authored-By to a Claude model at
the noreply address.

Usage:
    check_commit_trailers.py [--range origin/main..HEAD] [--self-test]
Exit 0 clean, 1 on a violation.
"""
import argparse
import re
import subprocess
import sys

# e.g. "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
WANT_RE = re.compile(r"^Co-Authored-By: Claude [A-Z][a-z]+ \d+(\.\d+)*"
                     r"( \([^()]*\))? <noreply@anthropic\.com>$")
EXAMPLE = "Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>"
TRAILER_RE = re.compile(r"^(Co-Authored-By|Signed-off-by|Claude-Session|Reviewed-by|"
                        r"Acked-by|Tested-by|Reported-by):", re.I)


def violations(msg):
    """Return a list of problems with one commit message. Pure, so it is testable."""
    lines = msg.rstrip("\n").split("\n")
    found = [l for l in lines if TRAILER_RE.match(l)]
    out = []
    if len(found) == 0:
        out.append("no trailer at all")
    elif len(found) > 1:
        out.append("%d trailers, expected exactly 1: %s"
                   % (len(found), " | ".join(found)))
    elif not WANT_RE.match(found[0].strip()):
        out.append("wrong trailer: %r (want e.g. %r)" % (found[0].strip(), EXAMPLE))
    if found and not WANT_RE.match(lines[-1].strip()):
        out.append("the mandated trailer is not the LAST line")
    return out


def self_test():
    WANT = EXAMPLE
    cases = [
        ("subject\n\nbody\n\n" + WANT, []),
        ("subject\n\nbody\n\nCo-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>",
         []),
        ("subject\n\nbody\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>", []),
        ("subject\n\nbody\n\nCo-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>", []),
        ("subject\n\nbody\n\nCo-Authored-By: Claude Opus 5.5 (1M context) <me@example.com>",
         ["wrong trailer"]),
        ("subject\n\nbody\n\nCo-Authored-By: MiniMax-M3 <noreply@anthropic.com>",
         ["wrong trailer"]),
        ("subject\n\nbody\n\nCo-Authored-By: Claude <noreply@anthropic.com>",
         ["wrong trailer"]),
        ("subject\n\nbody\n\nCo-Authored-By: Claude Opus <noreply@anthropic.com>",
         ["wrong trailer"]),
        ("subject\n\nbody\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com.evil>",
         ["wrong trailer"]),
        ("subject\n\nbody\n\nSigned-off-by: Claude Opus 5.5 <noreply@anthropic.com>",
         ["wrong trailer"]),
        ("subject\n\nbody\n\n" + WANT + "\nClaude-Session: https://x", ["2 trailers"]),
        ("subject\n\nbody\n\nCo-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>\n" + WANT,
         ["2 trailers"]),
        ("subject\n\nbody", ["no trailer at all"]),
        ("subject\n\n" + WANT + "\n\ntrailing prose", ["not the LAST line"]),
    ]
    bad = 0
    for msg, expect in cases:
        got = violations(msg)
        ok = (not expect and not got) or (expect and got and
                                          any(e.split()[0] in " ".join(got) for e in expect))
        if not ok:
            print("  self-test FAIL: %r -> %r (wanted %r)" % (msg[:40], got, expect))
            bad = 1
    print("check_commit_trailers: self-test %s" % ("FAILED" if bad else "PASS"))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--range", default="origin/main..HEAD")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--strict", action="store_true", help="accepted for sweep uniformity")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    try:
        shas = subprocess.run(["git", "rev-list", args.range], capture_output=True,
                              text=True, check=True).stdout.split()
    except subprocess.CalledProcessError as e:
        print("check_commit_trailers: cannot list %s: %s" % (args.range, e))
        return 1
    bad = 0
    for sha in shas:
        msg = subprocess.run(["git", "log", "-1", "--format=%B", sha],
                             capture_output=True, text=True).stdout
        subj = msg.split("\n")[0][:60]
        for v in violations(msg):
            print("  %s %s -- %s" % (sha[:8], subj, v))
            bad = 1
    if not bad:
        print("check_commit_trailers: clean (%d commit(s) in %s)" % (len(shas), args.range))
    return bad


if __name__ == "__main__":
    sys.exit(main())
