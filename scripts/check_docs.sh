#!/usr/bin/env bash
# check_docs.sh — the one-command docs gate for the ten normative documents
# plus the derived code-generation reference (eleven files in all).
#
# Symmetric with scripts/gate.sh (the compiler gate): a single blessed signal
# that a docs revision is internally consistent before it lands. Runs nine
# mechanical checks and exits non-zero if any fails.
#
#   0. Derived-document pins — every derived document declares the revision it
#                            was validated against, and Bezel_For_Code_Generation.md
#                            fails past three revisions stale (v0.269). The
#                            guides are reported, not failed. A missing pin on
#                            the code-generation reference is itself a failure.
#   1. Anchor resolution   — every intra/cross-doc [...](file#anchor) link whose
#                            target is an explicit <a id="..."> resolves.
#   2. Version consistency — the spec/stdlib/rationale/changelog headers agree
#                            on the revision, and Bezel_Changelog.md has exactly
#                            one entry for it.
#   3. §-citation sanity   — flags §N.M references to section numbers that have
#                            no heading anywhere in that document (typos only;
#                            prose mentions of other docs' sections are ignored).
#   3b. Statics-facts derivation — the stdlib's per-type **Facts** lines
#                            (stdlib §1, v0.245) are the single normative
#                            carrier of the statics/sharing sets; cross-check
#                            every formula carrier (stdlib §1 symbolic, spec
#                            prose, the script's own derivations) against one
#                            hardcoded triple, recompute the spec's three
#                            tagged membership parentheticals (§22.12 rules
#                            2(i)/4, §28.13), assert the fact-line roster
#                            (typed lines + named none-declaring types), and
#                            fail on divergence.
#   3c. §3.6 property table — the spec's derived-type-property registry (the
#                            | Property | Defined in | Classifies | table)
#                            keeps its row names pairwise distinguishable —
#                            no shared first component, no equal component
#                            multiset — and each row's Defined-in cell links
#                            to an anchor whose section names the property
#                            (v0.257; proposal 0095 Q4, as respecified by the
#                            v0.257 gate round).
#   3d. Clause-region body separation — the docs follow the style §22.16
#                            recommends (v0.260, proposal 0100): in every
#                            fenced code block of the non-changelog documents,
#                            a member whose contract-clause region
#                            (`requires`/`ensures`, with or without `static`)
#                            is followed by a body statement rather than by
#                            `end` carries a blank line between the last
#                            clause and that statement. Docs-only style
#                            conformance — neither a compiler diagnostic nor
#                            a lint over user code or tests/; it exists so the
#                            documents follow the style they recommend.
#                            Skipped by --quick, like check 4.
#   3e. Fenced-code tagging — every OPENING code fence in the non-changelog
#                            documents carries a language tag (v0.269). An
#                            untagged fence is invisible to any tag-keyed
#                            sweep, which is how fourteen of them escaped the
#                            v0.269 example audit's first pass. The changelog
#                            is exempt: its fences are pinned historical
#                            quotations.
#   4. Example extraction  — scripts/extract_doc_examples.py runs clean over the
#                            CURRENT docs into a scratch tree (never the committed
#                            corpus, per #1380); any parse failure is a finding.
#
# Usage:  scripts/check_docs.sh            # full gate
#         scripts/check_docs.sh --quick    # skip extraction (skips checks 3d and 4)
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
    'codegen':   'docs/Bezel_For_Code_Generation.md',
    'guide01':   'docs/Bezel_Guide_01_Memory_and_Ownership.md',
    'guide02':   'docs/Bezel_Guide_02_Errors_and_Nullability.md',
    'guide03':   'docs/Bezel_Guide_03_Modeling_Data.md',
    'guide04':   'docs/Bezel_Guide_04_Concurrency.md',
    'guide05':   'docs/Bezel_Guide_05_Collections_and_Iteration.md',
    'guide06':   'docs/Bezel_Guide_06_Contracts_and_Purity.md',
    'guide07':   'docs/Bezel_Guide_07_Style.md',
}
text = {}
for k, p in DOCS.items():
    try:
        text[k] = open(p, encoding='utf-8').read()
    except FileNotFoundError:
        print(f"check_docs: missing {p}", file=sys.stderr); sys.exit(2)

fail = 0
base = {os.path.basename(p): k for k, p in DOCS.items()}

# ---- 0. PIN: derived documents declare the revision they were validated at ----
# Added v0.269. The guides carry pins and nothing checked them, which is how
# Guide 03 sat NINETEEN revisions stale (v0.250 against a v0.269 spec) with both
# gates green. A pin is an assertion that someone re-traced the document; an
# unchecked assertion is the failure mode this project keeps rediscovering.
#
# Policy: report every pin and its staleness. FAIL only for documents whose
# staleness produces WRONG CODE rather than stale prose -- currently
# Bezel_For_Code_Generation.md, which exists to be copied from. Guides are
# reported, not failed: a guide teaching a still-valid idiom at an older pin is
# imprecise, not incorrect, and forcing a re-trace every revision would buy
# ceremony rather than accuracy.
PIN_RE = re.compile(r're-validated against v(\d+)\.(\d+)|[Vv]alidated against v(\d+)\.(\d+)')
CODEGEN = 'docs/Bezel_For_Code_Generation.md'
CODEGEN_MAX_STALE = 3      # revisions; beyond this the document is a liability

m = re.search(r'^\*\*Version (\d+)\.(\d+)', text['spec'], re.M)
if not m:
    print("PIN: cannot parse the spec version header"); fail = 1
else:
    cur = int(m.group(2))
    rows, pin_fail = [], []
    for key, path in sorted(DOCS.items()):
        if key in ('spec', 'stdlib', 'rationale', 'changelog'):
            continue                      # versioned headers, covered by VERSION
        head = '\n'.join(text[key].split('\n')[:6])
        pm = PIN_RE.search(head)
        if not pm:
            # A missing pin is a violation for CODEGEN, not a free pass: the one
            # document this check exists to fail must not become unfailable by
            # deleting the assertion (found by the v0.269 composition gate).
            if path == CODEGEN:
                pin_fail.append(f"  {os.path.basename(path)} has NO pin line -- it must declare "
                                f"the revision it was validated against, in line 3")
            else:
                rows.append(f"{os.path.basename(path)}: no pin")
            continue
        minor = int(pm.group(2) or pm.group(4))
        stale = cur - minor
        rows.append(f"{os.path.basename(path)}: v0.{minor} ({stale} behind)" if stale else
                    f"{os.path.basename(path)}: v0.{minor} (current)")
        if path == CODEGEN and stale > CODEGEN_MAX_STALE:
            pin_fail.append(f"  {os.path.basename(path)} is {stale} revisions stale "
                            f"(v0.{minor} vs v0.{cur}); max {CODEGEN_MAX_STALE} -- "
                            f"re-validate it and move its pin, or the code it teaches is wrong")
    if pin_fail:
        fail = 1
        print(f"PIN: {len(pin_fail)} violation(s):"); print("\n".join(pin_fail))
    else:
        print("PIN: clean (" + "; ".join(rows) + ")")

# ---- 0b. FENCE: every opening code fence carries a language tag ---------------
# Added v0.269. A bare ``` opening fence is ambiguous in a way that hides defects:
# nothing distinguishes "untagged because it is not Bezel" from "untagged because
# somebody forgot". The 2026-08-20 example sweep hit two separate holes. Its
# partition matched ```bezel at column zero, so 8 INDENTED Bezel blocks (inside
# bullet lists) were assigned to no lane. Independently, 14 fences carried NO tag
# at all and so were invisible to a tag-keyed sweep by construction; the first
# pass surfaced 10 of them and this census found the remaining 4. All 14 turned
# out to be EBNF or schematics -- but that was luck, not a guarantee, and the
# next one might be real Bezel that no reviewer and no extractor ever looks at.
# (8 and 14 count different sets; they are not two estimates of one number.)
#
# Closing fences are bare by definition and are not checked; only openers are.
fbad = []
for key, path in DOCS.items():
    # The changelog is exempt: it is pinned history, written at each revision and
    # never retroactively edited (spec-change-process §5). Its fences are historical
    # quotations, and re-tagging them would edit a record that is meant to be fixed.
    if key == 'changelog':
        continue
    lines = text[key].split("\n")
    inblock = False
    for i, ln in enumerate(lines, 1):
        m = re.match(r'^\s*```(\S*)\s*$', ln)
        if not m:
            continue
        if inblock:                     # this is the closer
            inblock = False
            continue
        inblock = True
        if m.group(1) == "":
            fbad.append(f"  {os.path.basename(path)}:{i}: opening code fence has no language tag "
                        f"(use `bezel` for Bezel, or `text`/`ebnf`/`json` for anything else)")
if fbad:
    fail = 1
    print(f"FENCE: {len(fbad)} violation(s):")
    print("\n".join(fbad))
else:
    print("FENCE: clean (every opening code fence is tagged)")

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
        # The lookback is measured in COLLAPSED text, not raw characters: an
        # indented, wrapped citation puts "(rationale" at the end of one line and
        # "§5.18, §5.22)" at the start of the next, behind 30+ spaces of comment
        # indentation, so a raw 24-char window sees only whitespace. That single
        # site in Appendix A.3 produced both standing advisories for many
        # revisions -- a check crying wolf, which is the failure mode this file
        # keeps rediscovering.
        pre = ' '.join(text[k][max(0, m.start()-160):m.start()].split()).lower()[-24:]
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

# ---- 3b. Statics-facts derivation (v0.245; proposal 0087, #3053) ----------
# The stdlib's per-type '**Facts** (...): `Type` — **fact**, ...' lines are the
# single normative carrier of the statics/sharing taxonomy (stdlib §1). The
# three derivation formulas are stated in three places — stdlib §1 (symbolic),
# the spec's three sites (prose), and this script — so every carrier is
# cross-checked against the one hardcoded triple below: (a) stdlib §1's
# symbolic formulas are literally present and equal; (b) each spec site's
# prose formula phrase is present; (c) the spec's three tagged membership
# parentheticals — 'derived membership (<set>), informative — currently ...'
# — match the sets recomputed from the fact lines (the tag names the enclosing
# rule, so no positional lookback is needed); and (d) the fact-line roster is
# complete (the typed lines, Channel<T>'s explicit '— none' included, plus the
# named 'declares no facts' types), so a deleted line fails loudly. Any
# divergence is a gate failure.
bad = []
facts = {}
for m in re.finditer(r'^\*\*Facts\*\*[^:]*:\s*(.+)$', text['stdlib'], re.M):
    payload = m.group(1)
    if ' — ' not in payload:
        continue
    types_part, facts_part = payload.split(' — ', 1)
    fs = set() if re.match(r'\s*none\b', facts_part) else set(re.findall(r'\*\*([a-z-]+)\*\*', facts_part))
    for n in re.findall(r'`([^`]+)`', types_part):
        facts[n] = fs
# Prose 'declares no facts' clauses are roster entries too: capture the run of
# backticked names immediately before the phrase.
none_named = set()
for m in re.finditer(r'((?:`[^`]+`[/,\s]*)+)declares? no facts', text['stdlib']):
    none_named |= set(re.findall(r'`([^`]+)`', m.group(1)))

EXPECT_TYPED = 12   # the 10 fact-granted types (CancellationTokenSource added v0.263, stdlib §6.1) + the explicit '— none' lines: Channel<T> and ScopeGuard (v0.259, stdlib §2.21)
EXPECT_NONE_NAMED = {'Lock', 'SemaphoreToken', 'EventSource<T>', 'AsyncEventSource<T>'}
if len(facts) != EXPECT_TYPED:
    bad.append(f"  Facts-line roster: expected {EXPECT_TYPED} typed entries, found {len(facts)}: {sorted(facts)}")
if 'Channel<T>' not in facts or facts.get('Channel<T>'):
    bad.append("  `Channel<T>` must carry an explicit '— none' Facts line (stdlib §6.9)")
if none_named != EXPECT_NONE_NAMED:
    bad.append(f"  'declares no facts' roster: expected {sorted(EXPECT_NONE_NAMED)}, found {sorted(none_named)}")

# The one hardcoded triple every carrier must agree with.
FORMULAS = {
    'form-(i)':    'synchronized ∧ ¬displacement-unbounded',
    'suppression': 'synchronized ∨ immutable',
    'shared':      '(synchronized ∨ immutable) ∧ ¬self-distributing',
}
derived = {
    'form-(i)':    {t for t, f in facts.items() if 'synchronized' in f and 'displacement-unbounded' not in f},
    'suppression': {t for t, f in facts.items() if 'synchronized' in f or 'immutable' in f},
    'shared':      {t for t, f in facts.items() if ('synchronized' in f or 'immutable' in f) and 'self-distributing' not in f},
}
# (a) stdlib §1's symbolic formulas, literal and equal to the triple.
for key, pat in (('form-(i)',    r'\*\*form-\(i\)\*\* set = ([^;.]+)'),
                 ('suppression', r'\*\*warning-suppression set\*\* = ([^;.]+)'),
                 ('shared',      r'\*\*`Shared`-payload synchronized set\*\* = ([^;.]+)')):
    fm = re.search(pat, text['stdlib'])
    got = fm.group(1).strip() if fm else None
    if got != FORMULAS[key]:
        bad.append(f"  stdlib §1 symbolic formula for {key}: expected '{FORMULAS[key]}', found '{got}'")
# (b) each spec site's prose formula phrase, present verbatim — the three rule
# sites (each anchored to its own surrounding wording, so a flip at one site is
# never masked by another carrier) plus Appendix C.11's two restating rows.
SPEC_PHRASES = {
    'form-(i) (rule 2(i))':   'declares the **synchronized** fact and not the **displacement-unbounded** fact',
    'suppression (rule 4)':   'derived, not curated: every type declaring the **synchronized** or the **immutable** fact (stdlib',
    'shared (§28.13)':        'declaring the **synchronized** or the **immutable** fact and not the **self-distributing** fact',
    'form-(i) (C.11)':        'declaring the **synchronized** and not the **displacement-unbounded** fact (stdlib §1)',
    'suppression (C.11)':     'the types declaring the **synchronized** or the **immutable** fact (stdlib §1)',
}
for key, phrase in SPEC_PHRASES.items():
    if phrase not in text['spec']:
        bad.append(f"  spec prose formula for {key} missing its phrase: '{phrase}'")
# (c) the tagged membership parentheticals, recomputed from the fact lines.
TAGMAP = {'form-(i)': 'form-(i)', 'warning-suppression': 'suppression', 'shared-payload': 'shared'}
stated = {}
for m in re.finditer(r'derived membership \((form-\(i\)|warning-suppression|shared-payload)\), informative — currently (.*?) — stdlib', text['spec']):
    stated[TAGMAP[m.group(1)]] = set(re.findall(r'`([^`]+)`', m.group(2)))
n_dm = len(re.findall(r'derived membership', text['spec']))
if n_dm != 3:
    bad.append(f"  expected exactly 3 tagged 'derived membership' parentheticals in the spec, found {n_dm} mention(s)")
if not facts:
    bad.append('  no **Facts** lines found in the stdlib (stdlib §1 expects them)')
if set(stated) != set(derived):
    bad.append(f"  expected one tagged spec parenthetical per derived set {sorted(derived)}, found {sorted(stated)}")
for k in sorted(set(stated) & set(derived)):
    if stated[k] != derived[k]:
        bad.append(f"  {k}: spec states {sorted(stated[k])} but the stdlib fact lines derive {sorted(derived[k])}")
if bad:
    fail = 1
    print("FACTS: stdlib fact lines, formulas, and spec derivation parentheticals disagree:")
    print("\n".join(bad))
else:
    print(f"FACTS: clean ({len(facts)} typed fact entries + {len(none_named)} named none-types; formulas agree at every carrier; all 3 derived-set parentheticals match)")

# ---- 3c. §3.6 derived-type-property table (v0.257; proposal 0095 Q4) -------
# The spec's §3.6 '| Property | Defined in | Classifies |' table is the single
# registry of the derived type properties, and its row names are load-bearing
# (diagnostics name them "in the words of the table", §3.6 rule 4). Three arms,
# all hard failures:
#   1. no two row names share their FIRST name component;
#   2. no two row names have equal normalized component multisets;
#   3. each row's Defined-in cell carries at least one ](...#anchor) link that
#      resolves to an <a id="..."> anchor whose section body names the row's
#      property (lowercased, non-alpha stripped, substring).
# The section body is sliced from the anchor's immediately following markdown
# heading to the next heading of the SAME OR SHALLOWER level — never to the
# next <a id= (internal sub-anchors would spuriously truncate auto-hashable,
# serialization-eligible, and element category). An anchor with no heading on
# the following line is skipped for that row (the stdlib's
# sec-owned-buffers-and-spans is one); the row passes on its other link.
# Name normalization: strip bold/italic markers and a trailing '(formerly ...)',
# split on '-' and whitespace, lowercase, strip one trailing 's' per component.
# (0095 §7 item 3's as-worded invariant — no two rows sharing ANY component —
# fails on the landed table twice (borrow-bearing/holds-borrows,
# auto-hashable/hashable); the arms above are the v0.257 gate round's
# respecification, recorded in the v0.257 changelog entry.)
bad = []
spec_t = text['spec']
rows36 = []          # (display name, defined-in cell)
mtab = re.search(r'^\| Property \| Defined in \| Classifies \|\s*$', spec_t, re.M)
if not mtab:
    bad.append("  the '| Property | Defined in | Classifies |' header row was not found in the spec (§3.6)")
else:
    for line in spec_t[mtab.end():].lstrip('\n').split('\n'):
        if not line.startswith('|'):
            break               # rows run until the first non-| line
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if not cells or set(cells[0]) <= set('-: '):
            continue            # the separator row
        if len(cells) < 2:
            bad.append(f"  malformed §3.6 row (fewer than 2 cells): {line.strip()}")
            continue
        raw = re.sub(r'\(formerly[^)]*\)\s*$', '', cells[0]).strip()
        name = raw.replace('**', '').replace('*', '').strip()
        rows36.append((name, cells[1]))
    if not rows36:
        bad.append('  the §3.6 table header was found but no rows parsed beneath it')

def components36(name):
    parts = re.split(r'[-\s]+', name.lower())
    return [p[:-1] if p.endswith('s') else p for p in parts if p]

# Arm 1: first components pairwise distinct.
firsts = {}
for name, _ in rows36:
    comps = components36(name)
    if comps:
        firsts.setdefault(comps[0], []).append(name)
for comp, hits in sorted(firsts.items()):
    if len(hits) > 1:
        bad.append(f"  arm 1 — rows share first name component '{comp}': " + ', '.join(f"'{h}'" for h in hits))

# Arm 2: normalized component multisets pairwise distinct.
msets = {}
for name, _ in rows36:
    msets.setdefault(tuple(sorted(components36(name))), []).append(name)
for key, hits in sorted(msets.items()):
    if len(hits) > 1:
        bad.append(f"  arm 2 — rows have equal component multiset {sorted(key)}: " + ', '.join(f"'{h}'" for h in hits))

# Arm 3: Defined-in resolution, property named in the linked section.
def section_body36(doc, anchor):
    t = text[doc]
    am = re.search(r'<a id="' + re.escape(anchor) + r'"\s*>\s*</a>[ \t]*\n', t)
    if not am:
        return None             # anchor missing: this link is unresolved
    rest = t[am.end():]
    hm = re.match(r'(#{1,6})[ \t]+\S', rest)
    if not hm:
        return ''               # no heading on the following line: skip link
    level = len(hm.group(1))
    nxt = re.search(r'\n#{1,' + str(level) + r'}[ \t]', rest)
    return rest[:nxt.start()] if nxt else rest

for name, cell in rows36:
    pname = re.sub(r'[^a-z]', '', name.lower())
    named = False
    notes = []
    links = list(re.finditer(r'\]\(([^)#\s]*)#([^)\s]+)\)', cell))
    if not links:
        bad.append(f"  arm 3 — row '{name}': Defined-in cell has no ](…#anchor) link: {cell}")
        continue
    for lm in links:
        fpart, anchor = lm.group(1), lm.group(2)
        target = 'spec' if not fpart else base.get(os.path.basename(fpart))
        if target is None:
            notes.append(f"{fpart}#{anchor}: not a normative doc"); continue
        body = section_body36(target, anchor)
        if body is None:
            notes.append(f"unresolved anchor {target}#{anchor}"); continue
        if body == '':
            notes.append(f"{target}#{anchor}: no heading follows the anchor (skipped)"); continue
        if pname in re.sub(r'[^a-z]', '', body.lower()):
            named = True; break
        notes.append(f"{target}#{anchor}: section does not name '{name}'")
    if not named:
        bad.append(f"  arm 3 — row '{name}' has no resolving Defined-in link that names it: " + '; '.join(notes))

if bad:
    fail = 1
    print('TABLE: the spec §3.6 derived-type-property table violates its invariants:')
    print('\n'.join(bad))
else:
    print(f"TABLE: clean ({len(rows36)} rows; first components and component multisets pairwise distinct; every row's Defined-in resolves to a section naming it)")

# ---- 3e. SETS: a normative set's restatements agree with it ------------------
# Added v0.275 (#3323). The FACTS check above cross-checks every FORMULA carrier
# against the spec's phrasing. It is blind, BY CONSTRUCTION, to a passage that
# restates a normative set's MEMBERSHIP in its own words elsewhere -- which is
# what a teaching-oriented stdlib section does, and how the `Shared`-payload
# synchronized set sat at seven members in the stdlib while the spec listed
# eight, from v0.263 (which granted CancellationTokenSource and enumerated the
# five sentences it amended, missing this one) until v0.274 found it by hand.
#
# This keys on MEMBERS, not phrasing: find every passage naming the set and
# enumerating it after "currently", extract the backticked type names, and fail
# when a restatement's member set differs from the specification's. The changelog
# is exempt -- pinned history, whose enumerations are quotations of their own
# revision.
SET_MARK = 'synchronized set'

def _set_members(seg):
    """The membership a passage ENUMERATES, or None if it only mentions the set.

    An enumeration is 'currently' followed immediately by three or more
    comma-separated backticked names. Requiring the run (rather than any
    backticks after any nearby 'currently') is what separates a restatement
    from a passing reference -- e.g. the rationale's "joining the §28.13
    synchronized set", which names the set and enumerates nothing."""
    m = re.search(r'currently\s+((?:`[^`]+`(?:,\s*(?:and\s+)?)?){3,})', seg[:700], re.S)
    if not m:
        return None
    return frozenset(re.findall(r'`([A-Za-z_][A-Za-z0-9_]*(?:<[^`>]*>)?)`', m.group(1)))

set_sites = {}
for _k, _p in DOCS.items():
    if _k == 'changelog':
        continue
    for _m in re.finditer(re.escape(SET_MARK), text[_k]):
        _mem = _set_members(text[_k][_m.start():_m.start() + 700])
        if _mem:
            set_sites.setdefault(_k, []).append((text[_k][:_m.start()].count('\n') + 1, _mem))

if 'spec' not in set_sites:
    print("SETS: cannot find the specification's enumeration of the "
          f"'{SET_MARK}' -- the check has lost its anchor")
    fail = 1
else:
    _norm = set_sites['spec'][0][1]
    _bad = []
    for _k, _sites in set_sites.items():
        for _line, _mem in _sites:
            if _mem != _norm:
                _miss = ', '.join(sorted(_norm - _mem)) or '(none)'
                _extra = ', '.join(sorted(_mem - _norm)) or '(none)'
                _bad.append(f"  [{_k}] line {_line}: missing {_miss}; unexpected {_extra}")
    _total = sum(len(v) for v in set_sites.values())
    if _bad:
        print(f"SETS: {len(_bad)} restatement(s) disagree with the specification's "
              f"{len(_norm)}-member set:")
        print("\n".join(_bad))
        fail = 1
    else:
        print(f"SETS: clean ({_total} enumeration(s) of the {SET_MARK} across "
              f"{len(set_sites)} document(s); all agree on {len(_norm)} members)")


# ---- 3f. REGISTER: the §14.2 conditionally-admitted register polices itself --
# Added post-v0.285 (#3332, the 0116 §5.2 follow-through). The register is a
# record, not an enforcement: nothing stopped a new "admitted only where"
# passage from never adding its row, and nothing flagged a row whose member the
# stdlib no longer declares -- the failure that made Array.Sort()'s v0.274
# deletion invisible for a revision. Two directions, keyed on the TERM OF ART
# ("conditionally-admitted member", any case/hyphenation), never on paraphrased
# English (the SETS tuning lesson):
#   A. every positive use of the term in spec/stdlib names a register member
#      within its paragraph (negated uses -- "not a ...", "neither is a ..." --
#      are the recorded exemption shape and are skipped);
#   B. every register row's members are (i) declared in the stdlib and
#      (ii) claimed conditional by a positive term-use near one of its names.
REG_TERM = re.compile(r'conditionall?y[-\s/]+admitted\s+member', re.I)  # [-\s/]+ spans fence-comment line wraps ('...LY\n// ADMITTED')
_regsec = re.search(r'<a id="sec-conditionally-admitted-members"></a>(.*?)\n### ',
                    text['spec'], re.S)
if not _regsec:
    print('REGISTER: cannot find spec section sec-conditionally-admitted-members '
          '-- the check has lost its anchor')
    fail = 1
else:
    _rows = []
    for _r in re.finditer(r'\n\| (`[^|]+`) \|', _regsec.group(1)):
        _names = re.findall(r'`([A-Za-z_][A-Za-z0-9_<>, ]*?)\.?([A-Za-z_][A-Za-z0-9_]*)(?:\(\))?`', _r.group(1))
        _mem = [n[1] for n in _names if n[1] not in ('T', 'E', 'U', 'F')]
        if _mem:
            _rows.append((_r.group(1)[:60], set(_mem)))
    _bad = []
    if not _rows:
        _bad.append('  no rows parsed from the register table')
    # direction A: positive term uses name a register member nearby
    _all_members = set().union(*[m for _, m in _rows]) if _rows else set()
    _pos_sites = []
    for _k in ('spec', 'stdlib'):
        for _m in REG_TERM.finditer(text[_k]):
            _pre = text[_k][max(0, _m.start()-60):_m.start()].lower()
            if re.search(r'(?:not|never|neither\s+is)\s+a\s+(?:\*\*)?$|is one$', _pre):
                continue
            _ln = text[_k][:_m.start()].count('\n') + 1
            _para = text[_k][max(0, _m.start()-1500):_m.start()+1500]
            _named = {n for n in _all_members if n in _para}
            if _m.start() >= (_regsec.start() if _k == 'spec' else 10**12) and \
               _m.start() <= (_regsec.end() if _k == 'spec' else -1):
                continue  # the register section itself
            if not _named:
                _bad.append(f'  [A] [{_k}] line {_ln}: the term appears with no '
                            'register member named in its paragraph -- add the row '
                            'or the exemption phrasing')
            else:
                _pos_sites.append((_k, _named))
    # direction B: every row declared + claimed
    for _label, _mem in _rows:
        _decl = any(re.search(r'method\s+' + re.escape(n) + r'\s*[(<]', text['stdlib'])
                    for n in _mem)
        if not _decl:
            _bad.append(f'  [B-i] register row {_label}...: no stdlib declaration '
                        'found for any of its members -- stale row (the v0.274 class)')
        _claimed = any(_mem & _named for _k, _named in _pos_sites)
        if not _claimed:
            _bad.append(f'  [B-ii] register row {_label}...: no stdlib/spec passage '
                        'claims these members conditionally admitted')
    if _bad:
        print(f'REGISTER: {len(_bad)} violation(s):')
        print('\n'.join(_bad))
        fail = 1
    else:
        print(f'REGISTER: clean ({len(_rows)} register rows; {len(_pos_sites)} '
              'positive term-sites, all mapped; negated exemptions skipped)')

sys.exit(1 if fail else 0)
PY
STRUCT=$?

if [ "$QUICK" = "1" ]; then
    [ "$STRUCT" -eq 0 ] && echo "check_docs: OK (--quick; extraction and style skipped)" || echo "check_docs: FAIL"
    exit "$STRUCT"
fi

# ---- 3d. Clause-region body separation (style, docs-only; v0.260) ----------
# §22.16's recommended style (non-normative for user code; proposal 0100): where
# a member's clause region is followed by a body, a blank line separates the
# last clause from the first body statement — the promises above the gap, the
# doing below it. This check makes the documents follow their own
# recommendation; it is neither a compiler diagnostic nor a lint over user code
# or tests/, and it must not be wired into gate.sh or agent_check.sh, which
# gate the compiler, not the prose. Scope: every fenced code block (any tag) of
# the ten non-changelog documents — the proposal's "six live normative
# documents" implemented as the full non-changelog set, a superset that is a
# no-op wherever no clause-bearing fence exists (recorded deviation, the
# 0095-Q4 shape: a hardcoded six would drift as guides gain or lose contract
# examples). A clause line is `requires(...)`/`ensures(...)` (optionally
# `static`-prefixed, optionally comment-trailed) ending in `)` — a line ending
# `);` is a body-less member or an `invariant` member, which has no seam. The
# run's next line is a violation exactly when it is non-blank and is not:
# another clause, an `end` closer, an expression body (`=>`), a lone `;`
# (body-less), or the closing fence. Every other block is a no-op.
# Two shapes sit silently outside the check's reach, recorded as its bound:
# a member written on ONE line (clause and first body statement together —
# the clause regex requires the line to end at the clause, so the line never
# matches), and a clause whose EXPRESSION WRAPS past its own line (the scan
# is line-wise, so a wrapped clause never matches and its seam is never
# judged). Zero corpus instances of either shape today; a future instance is
# invisible to this check, not blessed by it.
python3 - <<'PY'
import re, sys
DOCS = ['docs/Bezel_Language_Spec.md', 'docs/Bezel_Standard_Library.md',
        'docs/Bezel_Design_Rationale.md',
        'docs/Bezel_Guide_01_Memory_and_Ownership.md',
        'docs/Bezel_Guide_02_Errors_and_Nullability.md',
        'docs/Bezel_Guide_03_Modeling_Data.md',
        'docs/Bezel_Guide_04_Concurrency.md',
        'docs/Bezel_Guide_05_Collections_and_Iteration.md',
        'docs/Bezel_Guide_06_Contracts_and_Purity.md',
        'docs/Bezel_Guide_07_Style.md']
clause = re.compile(r'^\s*(?:static\s+)?(?:requires|ensures)\(.*\)\s*(?://.*)?$')
member = re.compile(r'\b(?:method|constructor)\s*(\w*)')
bad = []
checked = 0
for p in DOCS:
    lines = open(p, encoding='utf-8').read().split('\n')
    infence = False
    for i, l in enumerate(lines):
        if l.lstrip().startswith('```'):
            infence = not infence
            continue
        if not infence or not clause.match(l):
            continue
        if i + 1 >= len(lines) or clause.match(lines[i + 1]):
            continue                      # run continues; judge at its last line
        checked += 1
        s = lines[i + 1].strip()
        if (s == '' or s.startswith('```') or re.match(r'end\b', s)
                or s.startswith('=>') or s == ';'):
            continue                      # styled, or no body statement follows
        name = '?'
        for back in range(i, max(-1, i - 12), -1):
            mm = member.search(lines[back])
            if mm:
                name = mm.group(1) or 'constructor'
                break
        bad.append(f"  {p}:{i + 2}  member '{name}': no blank line between the last clause and the first body statement")
if bad:
    print(f"STYLE: {len(bad)} clause-region/body adjacency violation(s) (§22.16 recommended style, v0.260):")
    print('\n'.join(bad))
    sys.exit(1)
print(f"STYLE: clean ({checked} clause-region seam(s) checked across the ten non-changelog docs)")
PY
[ $? -ne 0 ] && STRUCT=1

# ---- 4. Example extraction (scratch tree; never the committed corpus) ------
SCRATCH=/var/tmp/bezel-scratch/check_docs.$$
mkdir -p "$SCRATCH/docs" "$SCRATCH/tests/conformance"
cp docs/*.md "$SCRATCH/docs/"
EX=$(python3 scripts/extract_doc_examples.py --repo-root "$SCRATCH" 2>&1)
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
