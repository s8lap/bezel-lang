#!/usr/bin/env bash
# check_docs.sh — the one-command docs gate for the six normative documents.
#
# Symmetric with scripts/gate.sh (the compiler gate): a single blessed signal
# that a docs revision is internally consistent before it lands. Runs four
# mechanical checks and exits non-zero if any fails.
#
#   1. Anchor resolution   — every intra/cross-doc [...](file#anchor) link whose
#                            target is an explicit <a id="..."> resolves.
#   2. Version consistency — the spec/stdlib/rationale/changelog headers agree
#                            on the revision, and Bezel_Changelog.md has exactly
#                            one entry for it.
#   3. §-citation sanity   — flags §N.M references to section numbers that have
#                            no heading anywhere in that document (typos only;
#                            prose mentions of other docs' sections are ignored).
#   4. Example extraction  — scripts/extract_doc_examples.py runs clean over the
#                            CURRENT docs into a scratch tree (never the committed
#                            corpus, per #1380); any parse failure is a finding.
#
# Usage:  scripts/check_docs.sh            # full gate
#         scripts/check_docs.sh --quick    # skip extraction (checks 1-3 only)
#
# This is a lint, not a proof: it catches the mechanical drift class that has
# actually bitten docs revisions (dangling anchors, stale version headers,
# mistyped section numbers, malformed examples). It does NOT check meaning —
# that is what the adversarial critique step in spec-change-process.md is for.
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "check_docs: not in a git repo" >&2; exit 2; }

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

python3 - <<'PY'
import re, sys, os
DOCS = {
    'spec':      'docs/Bezel_Language_Spec.md',
    'stdlib':    'docs/Bezel_Standard_Library.md',
    'rationale': 'docs/Bezel_Design_Rationale.md',
    'changelog': 'docs/Bezel_Changelog.md',
    'guide01':   'docs/Bezel_Guide_01_Memory_and_Ownership.md',
    'guide02':   'docs/Bezel_Guide_02_Errors_and_Nullability.md',
}
text = {}
for k, p in DOCS.items():
    try:
        text[k] = open(p, encoding='utf-8').read()
    except FileNotFoundError:
        print(f"check_docs: missing {p}", file=sys.stderr); sys.exit(2)

fail = 0
base = {os.path.basename(p): k for k, p in DOCS.items()}

# ---- 1. Anchor resolution -------------------------------------------------
# The repo uses TWO anchor styles: explicit <a id="sec-..."> for cross-refs, and
# GitHub-auto heading slugs for the table of contents ('## 1. Foo' -> #1-foo).
# A link resolves if its anchor is in EITHER set.
def gh_slug(heading):
    s = heading.strip().lower()
    s = re.sub(r'[^\w\s-]', '', s)   # drop punctuation (incl. the dot in '1.')
    s = re.sub(r'\s+', '-', s.strip())
    return s
valid = {}
for k, t in text.items():
    ids = set(re.findall(r'<a id="([^"]+)"', t))
    for m in re.finditer(r'^#{1,6}\s+(.+?)\s*$', t, re.M):
        ids.add(gh_slug(m.group(1)))
    valid[k] = ids

broken = []
for k, t in text.items():
    for m in re.finditer(r'\]\(([^)#\s]*)#([^)\s]+)\)', t):
        fpart, anchor = m.group(1), m.group(2)
        target = k if not fpart else base.get(os.path.basename(fpart))
        if target is None:
            continue  # link to a non-normative file; out of scope
        if anchor in valid[target]:
            continue
        broken.append(f"  [{k}] -> {target}#{anchor}")
if broken:
    fail = 1
    print(f"ANCHOR: {len(broken)} unresolved link(s):")
    print("\n".join(broken[:40]))
    if len(broken) > 40:
        print(f"  ... and {len(broken)-40} more")
else:
    print("ANCHOR: clean")

# ---- 2. Version consistency ----------------------------------------------
ver = {}
for k in ('spec', 'stdlib', 'rationale', 'changelog'):
    # spec: '**Version 0.184 ...**'; stdlib/rationale: '**Companion ... v0.184 ...**'
    m = re.search(r'\*\*Version (\d+\.\d+)', text[k]) or re.search(r'\bv(\d+\.\d+) —', text[k])
    ver[k] = m.group(1) if m else None
if len(set(ver.values())) != 1 or None in ver.values():
    fail = 1
    print(f"VERSION: header mismatch {ver}")
else:
    v = ver['spec']
    if f"### v{v} changelog" not in text['changelog']:
        fail = 1
        print(f"VERSION: headers agree on v{v} but Bezel_Changelog.md has no '### v{v} changelog' entry")
    else:
        n = text['changelog'].count(f"### v{v} changelog")
        if n != 1:
            fail = 1
            print(f"VERSION: v{v} changelog entry appears {n} times (expected 1)")
        else:
            print(f"VERSION: clean (v{v}, single changelog entry)")

# ---- 3. §-citation sanity (typo lint) ------------------------------------
# For each doc, gather the section numbers it actually defines as headings,
# then flag same-doc-looking bare '§N' / '§N.M' citations to numbers that have
# no heading. Cross-doc citations (e.g. spec citing 'stdlib §6.5') are ignored
# because the number space differs; we only lint citations with no doc word
# nearby, treated as intra-doc.
def heading_numbers(t):
    nums = set()
    for m in re.finditer(r'^#{1,4}\s+`?(\d+(?:\.\d+)*)', t, re.M):
        nums.add(m.group(1))
        parts = m.group(1).split('.')
        for i in range(1, len(parts)):
            nums.add('.'.join(parts[:i]))  # parent numbers count as present
    return nums
missing = []
# Spec-only: the spec's intra-doc §-refs dominate, so a §N.M with no such heading
# is likely a typo. The stdlib/rationale cite spec sections in prose constantly
# (their own number space is disjoint), which makes the same lint pure noise there.
for k in ('spec',):
    defined = heading_numbers(text[k])
    if not defined:
        continue
    for m in re.finditer(r'§(\d+(?:\.\d+){0,3})\b', text[k]):
        # skip cross-doc citations: a doc name in the 24 chars before, OR a
        # markdown link to a different doc file in the 60 chars after (the
        # '[§5.11](Bezel_Design_Rationale.md#...)' form — doc name follows the §).
        pre = text[k][max(0, m.start()-24):m.start()].lower()
        if any(w in pre for w in ('stdlib', 'standard library', 'rationale', 'guide')):
            continue
        post = text[k][m.end():m.end()+60]
        lm = re.match(r'[^\s\]]*\]\(([^)#]+)#', post)
        if lm and os.path.basename(lm.group(1)) != os.path.basename(DOCS[k]):
            continue  # link points at another doc
        num = m.group(1)
        if num not in defined:
            missing.append(f"  [{k}] §{num}")
# de-dupe, cap
seen = set(); uniq = []
for x in missing:
    if x not in seen:
        seen.add(x); uniq.append(x)
if uniq:
    # This is advisory (typos vs. legitimately-cross-doc-but-unmarked); report
    # but do NOT fail the gate on it — it is too noisy to block on.
    print(f"CITATION (advisory): {len(uniq)} §-refs to undefined section numbers:")
    print("\n".join(uniq[:20]))
    if len(uniq) > 20:
        print(f"  ... and {len(uniq)-20} more")
else:
    print("CITATION: clean")

sys.exit(1 if fail else 0)
PY
STRUCT=$?

if [ "$QUICK" = "1" ]; then
    [ "$STRUCT" -eq 0 ] && echo "check_docs: OK (--quick; extraction skipped)" || echo "check_docs: FAIL"
    exit "$STRUCT"
fi

# ---- 4. Example extraction (scratch tree; never the committed corpus) ------
SCRATCH=/var/tmp/bezel-scratch/check_docs.$$
mkdir -p "$SCRATCH/docs" "$SCRATCH/tests/conformance"
cp docs/*.md "$SCRATCH/docs/"
EX=$(python3 scripts/extract_doc_examples.py "$SCRATCH" 2>&1)
EXRC=$?
# A genuine failure is a non-zero exit or a Python traceback. The extractor's
# "error: N" classification line counts examples that are EXPECTED to compile-
# error (a normal category), so it must not be matched here.
TRACE=$(printf '%s\n' "$EX" | grep -c 'Traceback\|SyntaxError:\|Exception:' || true)
rm -rf "$SCRATCH"
if [ "$EXRC" -ne 0 ] || [ "$TRACE" -ne 0 ]; then
    echo "EXTRACT: FAIL (exit=$EXRC, tracebacks=$TRACE)"
    printf '%s\n' "$EX" | tail -8
    STRUCT=1
else
    NCASES=$(printf '%s\n' "$EX" | grep -oE 'fragment: [0-9]+' | grep -oE '[0-9]+' | head -1)
    echo "EXTRACT: clean (${NCASES:-?} fragments extracted, extractor exit 0)"
fi

[ "$STRUCT" -eq 0 ] && echo "check_docs: OK" || echo "check_docs: FAIL"
exit "$STRUCT"
