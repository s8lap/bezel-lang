#!/usr/bin/env python3
"""pregate_stale_needles.py — find test needles for diagnostic text a batch removed.

WHY. A diagnostic-text rename is a behaviour flip (CLAUDE.md, "A diagnostic-text
rename is a behaviour-flip"). A gtest needle for the old text fails at the gate.
A lit `CHECK-NOT` for it passes vacuously forever. Both used to be caught only by
an integrator remembering to grep. This makes the grep mechanical.

WHAT. For the range BASE..HEAD, collect every C++ string literal on a removed
(`-`) line under src/ that is at least MIN_LEN characters long and has at least
three words, so it reads like message text rather than an identifier. Drop
literals that still occur anywhere in the HEAD src/ tree, since a moved line is
not a removal. Adjacent literals are joined first (`"a " "b"` reads as `"a b"`),
so reflowing a long message across different line breaks is not a removal
either. Then report every tests/ file that still contains a surviving literal.

A hit is a CANDIDATE, not a verdict. The text may be deliberately pinned as
absent, or the literal may be a fragment of a longer message. Exit status is 0
unless --strict is given and there are hits.

Usage: scripts/pregate_stale_needles.py [--base origin/main] [--head HEAD]
                                        [--min-len 20] [--strict]
                                        [--repo PATH] [--from-ref]

By default src/ and tests/ are read from the working tree of this script's own
checkout. --from-ref reads them from the --head commit instead, so a caller in
one worktree can scan another branch without checking it out
(scripts/handback_check.sh does this); --repo names the repository to use.
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# `"...a " \n   "b..."` -> `"...a b..."`: C++ adjacent-literal concatenation.
_ADJ = re.compile(r'"\s*\n?\s*"')


def join_adjacent(text):
    """Collapse C++ adjacent string literals so reflowed messages still match."""
    return _ADJ.sub('', text)


def removed_literals(diff_text, min_len=20):
    """String literals on removed lines of a unified diff (not the --- header)."""
    out = set()
    for line in diff_text.splitlines():
        if not line.startswith('-') or line.startswith('---'):
            continue
        for m in _LIT.finditer(line[1:]):
            s = m.group(1)
            if len(s) >= min_len and len(s.split()) >= 3:
                out.add(s)
    return out


def still_present(literals, tree_texts):
    """Subset of literals that still occur verbatim in any of tree_texts."""
    joined = [join_adjacent(t) for t in tree_texts]
    return {s for s in literals if any(s in t for t in joined)}


def _git(*args):
    return subprocess.run(['git', '-C', REPO] + list(args), check=True,
                          capture_output=True, text=True).stdout


def _read_tree_at(ref, rel):
    """Like _read_tree, but from a commit: {path: text} for every blob under rel."""
    names = _git('ls-tree', '-r', '--name-only', ref, '--', rel).split('\n')
    names = [n for n in names if n]
    if not names:
        return {}
    req = ''.join(f'{ref}:{n}\n' for n in names).encode()
    out = subprocess.run(['git', '-C', REPO, 'cat-file', '--batch'], input=req,
                         check=True, capture_output=True).stdout
    texts, pos = {}, 0
    for n in names:
        nl = out.index(b'\n', pos)
        header = out[pos:nl].split()
        pos = nl + 1
        if len(header) < 3 or header[1] == b'missing':
            continue
        size = int(header[2])
        texts[n] = out[pos:pos + size].decode('utf-8', errors='replace')
        pos += size + 1
    return texts


def _read_tree(rel):
    texts = {}
    for root, _dirs, files in os.walk(os.path.join(REPO, rel)):
        for f in files:
            p = os.path.join(root, f)
            try:
                with open(p, encoding='utf-8', errors='replace') as fh:
                    texts[os.path.relpath(p, REPO)] = fh.read()
            except OSError:
                pass
    return texts


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--base', default='origin/main')
    ap.add_argument('--head', default='HEAD')
    ap.add_argument('--min-len', type=int, default=20)
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--repo', help='repository to scan (default: this checkout)')
    ap.add_argument('--from-ref', action='store_true',
                    help='read src/ and tests/ from --head, not the working tree')
    a = ap.parse_args()
    global REPO
    if a.repo:
        REPO = os.path.abspath(a.repo)
    read = (lambda rel: _read_tree_at(a.head, rel)) if a.from_ref else _read_tree

    diff = _git('diff', '--unified=0', f'{a.base}..{a.head}', '--', 'src/')
    removed = removed_literals(diff, a.min_len)
    src = read('src')
    gone = removed - still_present(removed, src.values())
    tests = read('tests')
    hits = []
    for s in sorted(gone):
        for path, text in sorted(tests.items()):
            if s in text:
                ln = text[:text.index(s)].count('\n') + 1
                hits.append((path, ln, s))

    print(f'stale_needles: {len(removed)} removed literal(s) in src/, '
          f'{len(gone)} gone from HEAD, {len(hits)} test hit(s)')
    for path, ln, s in hits:
        print(f'  CANDIDATE {path}:{ln}: "{s[:90]}"')
    return 1 if (a.strict and hits) else 0


if __name__ == '__main__':
    sys.exit(main())
