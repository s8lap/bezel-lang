#!/usr/bin/env python3
"""Every §-citation in src/ must name a section some document defines (#4455).

A citation is how a reader finds the authority for what the code does
(CLAUDE.md, "Comments"). One that resolves nowhere is worse than none: it
claims an authority and spends the reader's time proving the claim false.

Three defect classes were found the first time this ran, at 298 sites:
  * 165 cited §24.1-§24.7. Spec §24 is now a tombstone -- "Events (retired
    member kind)" -- whose own text keeps its anchor "for citation stability"
    but which has no subsections at all.
  * 113 were ISSUE numbers wearing a section sigil: `§2923's fix` means #2923.
  * 20 named sections no document has (§35.4.1-.3, §17.1.1, §18.9, §20.5).

Six more looked dangling and were NOT: §3.8, §3.9, §5.6, §5.11, §5.13 and
§5.20 are RATIONALE sections. Any checker here must read every document's
headings, not the spec's alone, or it manufactures false positives -- which
is exactly what the first measurement did before this file existed.

Additionally, detect misattributed clause citations (#5439): a comment citing
"§N.N clause M" (or "rule", "step", "item") where the spec's §N.N has no
numbered clause M.  The check is conservative: it only flags when the cited
clause number EXCEEDS the highest numbered item found in that section -- a
well-formed citation to a section that genuinely has no items at all passes
silently.

Exit 1 on any unresolvable citation or misattributed clause.  Build-free: it
is a text cross-reference, so it belongs in the fast gate phase, not the
compile path.
"""
import glob, io, os, re, sys

DOCS = (['docs/Bezel_Language_Spec.md', 'docs/Bezel_Standard_Library.md',
         'docs/Bezel_Design_Rationale.md'] + sorted(glob.glob('docs/Bezel_Guide_*.md')))
HEADING = re.compile(r'^#{2,4}\s+([A-Za-z]?\.?\d+(?:\.\d+)*)\.?\s', re.M)
# Matches a numbered clause/item at the start of a line within a section body.
# Requires: start of line / whitespace, digit(s), period, space, letter start
# of content.  Excludes version strings (v0.301), section anchors (§14.2),
# footnote-like [1] patterns, and em-dashes.
NUMBERED_ITEM = re.compile(r'(?m)^(\s*)(\d+)\. ([a-zA-Z**])')
CITE = re.compile('§(\\d+(?:\\.\\d+)*)')
# Matches a clause-style citation: §N.N followed by clause/rule/step/item + number
CLAUSE_CITE = re.compile(
    r'§(\d+(?:\.\d+)*)\s+(cl(?:ause)?|rule|step|item)\s+(\d+)',
    re.IGNORECASE)
SRC_EXT = ('.cpp', '.hpp', '.h', '.cc')

# Narrow allowlist for clause citations that are imprecise but whose behaviour
# is correct.  Each entry is a (file, line, section, kind, n) -> reason map.
# Only the exact citation string is allowlisted -- a different citation from
# the same line would still be checked.
# Format: section, kind, n are all strings (kind is lowercased).
_IMPRECISE_ALLOWLIST = {
    # (empty -- kept as mechanism for future narrow suppressions)
}

def defined_sections(root='.'):
    out = set()
    for doc in DOCS:
        p = os.path.join(root, doc)
        if not os.path.exists(p): continue
        for m in HEADING.finditer(io.open(p, encoding='utf-8').read()):
            out.add(m.group(1).lstrip('.'))
    return out

def section_max_clause_number(content):
    """Return the highest numbered item found in section content.

    Numbers must start at the beginning of a line (allowing leading whitespace)
    to avoid false positives from version strings like v0.301, anchors like
    §14.2, footnote markers [1], or em-dash lists.
    """
    max_n = 0
    for m in NUMBERED_ITEM.finditer(content):
        indent, digits, first_char = m.group(1), int(m.group(2)), m.group(3)
        # Reject footnote-like lines where the digit is inside [brackets].
        # e.g. "   [1] foo" — the digit is preceded by '['.
        line_start = m.start() - len(indent)
        if line_start >= 0:
            ch = content[line_start] if content[line_start] != '\n' else ''
            if ch == '[': continue
        max_n = max(max_n, digits)
    return max_n

def build_section_items_map(root='.'):
    """Return {section_id: max_clause_number} for each doc section."""
    section_items = {}
    for doc in DOCS:
        p = os.path.join(root, doc)
        if not os.path.exists(p): continue
        text = io.open(p, encoding='utf-8').read()
        # Find all heading matches and their positions
        headings = [(m.group(1).lstrip('.'), m.start(), m.end())
                    for m in HEADING.finditer(text)]
        for i, (sec_id, start, end) in enumerate(headings):
            # Content spans from end of this heading to start of next heading (or EOF)
            next_start = headings[i+1][1] if i+1 < len(headings) else len(text)
            content = text[end:next_start]
            max_n = section_max_clause_number(content)
            if max_n > 0:
                section_items[sec_id] = max_n
    return section_items

def scan(root='.'):
    known = defined_sections(root)
    section_items = build_section_items_map(root)
    if not known:
        print("check_src_citations: no document headings found -- wrong root?"); sys.exit(2)
    bad = []
    for base, _, files in os.walk(os.path.join(root, 'src')):
        for f in sorted(files):
            if not f.endswith(SRC_EXT): continue
            p = os.path.join(base, f)
            for i, ln in enumerate(io.open(p, encoding='utf-8', errors='replace'), 1):
                # Existing section-resolution check
                for m in CITE.finditer(ln):
                    s = m.group(1)
                    if s in known: continue
                    hint = (" -- an issue number? write `#%s`" % s
                            if '.' not in s and len(s) >= 3 else "")
                    bad.append(f"  {os.path.relpath(p, root)}:{i}: §{s} resolves nowhere{hint}")
                # Clause-citation check
                for m in CLAUSE_CITE.finditer(ln):
                    sec = m.group(1)
                    kind = m.group(2).lower()
                    n_str = m.group(3)
                    n = int(n_str)
                    rel = os.path.relpath(p, root)
                    # Check allowlist first
                    key = (rel, str(i), sec, kind, n_str)
                    if key in _IMPRECISE_ALLOWLIST:
                        continue
                    # Only check clauses for sections that have numbered items
                    if sec in section_items:
                        max_n = section_items[sec]
                        if n > max_n:
                            bad.append(
                                f"  {rel}:{i}: "
                                f"§{sec} has no {kind} {n} (highest: {max_n})")
    return bad

def selftest():
    import tempfile, textwrap
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, 'docs')); os.makedirs(os.path.join(d, 'src'))
        io.open(os.path.join(d, 'docs/Bezel_Language_Spec.md'), 'w', encoding='utf-8').write(
            "# S\n\n### 9.7 Records\n\n"
            "1. First item\n"
            "2. Second item\n"
            "3. Third item\n\n"
            "### 24. Events (retired member kind)\n\n"
            "### 14.2 Constraints\n\n"
            "1. Class constraint\n"
            "2. Interface constraint\n"
            "3. Value constraint\n"
            "4. Notnull constraint\n"
            "5. Sendable constraint\n"
            "6. Hashable constraint\n"
            "7. New constraint\n\n"
            "### 2.8.2 The artifact interface\n\n"
            "1. **The build supplies the transitive closure.**\n"
            "2. **Use without import.**\n"
            "3. **Program-wide type identity.**\n\n"
            "### 2.8.3 Transitive closure\n\n"
            "1. Build supplies closure.\n"
            "2. Use without import.\n"
            "3. Program-wide type identity.\n"
            "4. Staleness error.\n\n"
            "### 2.8.4 Program assembly\n\n"
            "1. Artifact kind by build.\n"
            "2. Main module defined.\n"
            "3. Entry-point search scope.\n"
            "4. Dependency is library.\n"
            "5. One copy per process.\n"
            "6. Static init across artifacts.\n\n"
            "### 32.13 Foreign-memory\n\n"
            "   1. First sub-item\n"
            "   2. Second sub-item\n"
        )
        io.open(os.path.join(d, 'docs/Bezel_Design_Rationale.md'), 'w', encoding='utf-8').write(
            "# R\n\n### 5.11 Ownership\n"
        )
        src = os.path.join(d, 'src/a.cpp')
        # Test 1: valid section citations
        io.open(src, 'w', encoding='utf-8').write(
            "// §9.7 ok\n"
            "// §5.11 ok (rationale)\n"
            "// §24 ok (no numbered items, passes silently)\n"
            "// §9.7 ok (rationale)\n"
        )
        bad = scan(d)
        assert bad == [], f"expected [] for valid sections, got {bad}"

        # Test 2: unresolvable section
        io.open(src, 'w', encoding='utf-8').write("// §2923 issue\n")
        bad = scan(d)
        assert len(bad) == 1 and '§2923' in bad[0] and 'issue number' in bad[0], bad

        # Test 3: valid clause citations
        io.open(src, 'w', encoding='utf-8').write(
            "// §9.7 rule 1 ok\n"
            "// §14.2 clause 5 ok\n"
            "// §2.8.2 rule 3 ok\n"
            "// §14.2 step 7 ok\n"
        )
        bad = scan(d)
        assert bad == [], f"expected [] for valid clauses, got {bad}"

        # Test 4: out-of-range clause (the fabricated-citation pattern)
        io.open(src, 'w', encoding='utf-8').write(
            "// §14.2 clause 8 out-of-range (max 7)\n"
        )
        bad = scan(d)
        assert len(bad) == 1 and 'no clause 8' in bad[0] and '§14.2' in bad[0], bad

        # Test 5: section with no numbered items (passes silently)
        io.open(src, 'w', encoding='utf-8').write(
            "// §5.11 rule 99 passes (no items in rationale section)\n"
        )
        bad = scan(d)
        assert bad == [], f"expected [] for section with no items, got {bad}"

        # Test 6: out-of-range clause in §2.8.2 (max 3)
        io.open(src, 'w', encoding='utf-8').write(
            "// §2.8.2 clause 5 out-of-range (max 3)\n"
        )
        bad = scan(d)
        assert len(bad) == 1 and 'no clause 5' in bad[0] and '§2.8.2' in bad[0], bad

        # Test 7: mixed valid and invalid in same file
        io.open(src, 'w', encoding='utf-8').write(
            "// §14.2 clause 5 ok\n"
            "// §14.2 clause 20 out-of-range\n"
        )
        bad = scan(d)
        assert len(bad) == 1 and 'no clause 20' in bad[0], bad

        # Test 8: the bug-class this found — adjacent-section item N (#5439).
        # §2.8.3 has rules 1-4; §2.8.4 has rules 1-6 including rule 5.
        # Citing "§2.8.3 rule 5" should fail (rule 5 is in §2.8.4, not §2.8.3).
        io.open(src, 'w', encoding='utf-8').write(
            "// §2.8.3 rule 5 out-of-range (rule 5 is in §2.8.4)\n"
        )
        bad = scan(d)
        assert len(bad) == 1 and 'no rule 5' in bad[0] and '§2.8.3' in bad[0], bad

        # And the corrected form (§2.8.4 rule 5) should pass.
        io.open(src, 'w', encoding='utf-8').write(
            "// §2.8.4 rule 5 ok\n"
        )
        bad = scan(d)
        assert bad == [], f"expected [] for §2.8.4 rule 5, got {bad}"

    print("check_src_citations: self-test OK")

if __name__ == '__main__':
    if '--self-test' in sys.argv: selftest(); sys.exit(0)
    bad = scan()
    if bad:
        print(f"check_src_citations: {len(bad)} unresolvable §-citation(s) or misattributed clause(s):")
        print("\n".join(bad[:40]))
        if len(bad) > 40: print(f"  ... and {len(bad)-40} more")
        sys.exit(1)
    print("check_src_citations: clean (every §-citation in src/ resolves, "
          "and every clause citation is within the section's numbered items)")
