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

Exit 1 on any unresolvable citation. Build-free: it is a text cross-reference,
so it belongs in the fast gate phase, not the compile path.
"""
import glob, io, os, re, sys

DOCS = (['docs/Bezel_Language_Spec.md', 'docs/Bezel_Standard_Library.md',
         'docs/Bezel_Design_Rationale.md'] + sorted(glob.glob('docs/Bezel_Guide_*.md')))
HEADING = re.compile(r'^#{2,4}\s+([A-Za-z]?\.?\d+(?:\.\d+)*)\.?\s', re.M)
CITE = re.compile('§(\\d+(?:\\.\\d+)*)')
SRC_EXT = ('.cpp', '.hpp', '.h', '.cc')

def defined_sections(root='.'):
    out = set()
    for doc in DOCS:
        p = os.path.join(root, doc)
        if not os.path.exists(p): continue
        for m in HEADING.finditer(io.open(p, encoding='utf-8').read()):
            out.add(m.group(1).lstrip('.'))
    return out

def scan(root='.'):
    known = defined_sections(root)
    if not known:
        print("check_src_citations: no document headings found -- wrong root?"); sys.exit(2)
    bad = []
    for base, _, files in os.walk(os.path.join(root, 'src')):
        for f in sorted(files):
            if not f.endswith(SRC_EXT): continue
            p = os.path.join(base, f)
            for i, ln in enumerate(io.open(p, encoding='utf-8', errors='replace'), 1):
                for m in CITE.finditer(ln):
                    s = m.group(1)
                    if s in known: continue
                    hint = (" -- an issue number? write `#%s`" % s
                            if '.' not in s and len(s) >= 3 else "")
                    bad.append(f"  {os.path.relpath(p, root)}:{i}: §{s} resolves nowhere{hint}")
    return bad

def selftest():
    import tempfile, textwrap
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, 'docs')); os.makedirs(os.path.join(d, 'src'))
        io.open(os.path.join(d, 'docs/Bezel_Language_Spec.md'), 'w', encoding='utf-8').write(
            "# S\n\n### 9.7 Records\n\n### 24. Events (retired member kind)\n")
        io.open(os.path.join(d, 'docs/Bezel_Design_Rationale.md'), 'w', encoding='utf-8').write(
            "# R\n\n### 5.11 Ownership\n")
        src = os.path.join(d, 'src/a.cpp')
        io.open(src, 'w', encoding='utf-8').write(
            "// §9.7 ok\n// §5.11 ok (rationale)\n// §24 ok\n"
            "// §24.3 dangling\n// §2923 issue\n")
        bad = scan(d)
        assert len(bad) == 2, bad
        assert '§24.3' in bad[0], bad
        assert 'issue number' in bad[1], bad
        io.open(src, 'w', encoding='utf-8').write("// §9.7 ok\n// §5.11 ok\n")
        assert scan(d) == [], scan(d)
    print("check_src_citations: self-test OK")

if __name__ == '__main__':
    if '--self-test' in sys.argv: selftest(); sys.exit(0)
    bad = scan()
    if bad:
        print(f"check_src_citations: {len(bad)} unresolvable §-citation(s):")
        print("\n".join(bad[:40]))
        if len(bad) > 40: print(f"  ... and {len(bad)-40} more")
        sys.exit(1)
    print("check_src_citations: clean (every §-citation in src/ resolves)")
