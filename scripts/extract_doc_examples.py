#!/usr/bin/env python3
"""
Extract fenced code blocks from the four Bezel docs.

Produces:
  tests/conformance/cases/<doc-tag>_<seq>.bzl  — one per block
  tests/conformance/manifest.tsv               — case metadata

Classification logic:
  error    — surrounding prose signals ill-formed/rejected (ERROR keyword in
              block, or prose contains error/rejected/ill-formed/does not compile)
  skip     — EBNF grammar, shell commands, pseudo-code, pure API signatures,
              or other non-runnable content
  program  — has a 'method Main' at top level
  fragment — everything else (needs wrapper)

Usage:
  python3 scripts/extract_doc_examples.py [repo-root]
"""

import os
import re
import sys

DOCS = [
    ("spec",   "docs/Bezel_Language_Spec.md"),
    ("guide1", "docs/Bezel_Guide_01_Memory_and_Ownership.md"),
    ("guide2", "docs/Bezel_Guide_02_Errors_and_Nullability.md"),
    ("stdlib", "docs/Bezel_Standard_Library.md"),
]

REPO_ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES_DIR = os.path.join(REPO_ROOT, "tests", "conformance", "cases")
MANIFEST   = os.path.join(REPO_ROOT, "tests", "conformance", "manifest.tsv")

os.makedirs(CASES_DIR, exist_ok=True)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def slugify_heading(h):
    """Convert a markdown heading to a short slug for the manifest."""
    h = h.strip().lstrip("#").strip()
    h = re.sub(r'`([^`]*)`', r'\1', h)     # remove backticks
    h = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', h)  # remove links
    h = re.sub(r'[^\w\s-]', '', h)
    h = re.sub(r'\s+', '-', h.strip())
    return h[:60]


ERROR_PROSE_RE = re.compile(
    r'\b(ill[-\s]formed|rejected|does not compile|compile[-\s]time error|✗|✘)\b',
    re.IGNORECASE,
)
# More conservative: only "is a compile error" right before the block
CLOSE_ERROR_RE = re.compile(
    r'(?:is a compile error|compile error\s*[:.]|the compiler (rejects|refuses|will reject)|'
    r'rejected at compile|is rejected|is ill.formed)\s*[:]?\s*$',
    re.IGNORECASE,
)

# Markers inside the code block itself that flag it as an error case
INLINE_ERROR_RE = re.compile(r'//\s*(ERROR|error)\b')

# Heuristics to classify as skip
SKIP_HINTS = [
    re.compile(r'^\s*[A-Za-z_][A-Za-z0-9_]*\s*::='),   # EBNF rule
    re.compile(r'^\s*[A-Za-z_][A-Za-z0-9_]*\s*:'),       # signature-style
]

# Grammar keywords that strongly suggest a runnable file
HAS_MODULE_RE  = re.compile(r'^\s*module\s+\w', re.MULTILINE)
HAS_CLASS_RE   = re.compile(r'^\s*(public\s+|private\s+|internal\s+)?(static\s+)?class\s+\w', re.MULTILINE)
HAS_METHOD_MAIN_RE = re.compile(r'\bmethod\s+Main\b', re.MULTILINE)


def classify_block(code, lang, prev_prose, notes_out):
    """Return (class, expectation, notes)."""

    # 1. Non-bezel fences → skip
    if lang and lang.strip() not in ('bezel', ''):
        notes_out.append(f"lang:{lang.strip()}")
        return 'skip', 'n/a', f"non-bezel language: {lang.strip()}"

    # 2. Empty
    if not code.strip():
        return 'skip', 'n/a', 'empty block'

    # 3. EBNF / grammar
    lines = code.splitlines()
    ebnf_count = sum(1 for l in lines if re.match(r'^\s*\w[\w\s]*::=', l))
    if ebnf_count > len(lines) * 0.4:
        return 'skip', 'n/a', 'ebnf-grammar'

    # 4. Pure signature listing (stdlib): class with only method/property/const declarations
    # Detect: block has no method bodies (no `end <name>` or `begin`/statement inside body)
    # A rough heuristic: starts with `public ... class` or `public ... interface` and
    # has no statements (assignments, calls, control flow) outside of signatures.
    is_sig = _is_signature_block(code)
    if is_sig:
        return 'skip', 'n/a', 'skip:signature'

    # 5. Error?
    # Inline // ERROR or // error marker is the most reliable signal.
    inline_has_error = bool(INLINE_ERROR_RE.search(code))
    # Prose: only the LAST non-empty line immediately before the block.
    prose_lines_list = [l for l in (prev_prose or '').splitlines() if l.strip()]
    last_prose_line = prose_lines_list[-1] if prose_lines_list else ''
    # Only trigger on prose if the very last line says "is rejected:" / "is a compile error:"
    # i.e. the code block IS the example of the rejected thing
    prose_has_close_error = bool(CLOSE_ERROR_RE.search(last_prose_line))
    # Also check for ✗/✘ symbols in last_prose_line
    prose_has_symbol = bool(re.search(r'[✗✘]', last_prose_line))
    is_error = inline_has_error or prose_has_close_error or prose_has_symbol

    if is_error:
        return 'error', 'diagnose', ''

    # 6. Program vs fragment
    has_module = bool(HAS_MODULE_RE.search(code))
    has_class = bool(HAS_CLASS_RE.search(code))

    # Only classify as program if it has a module + class + Main body
    # (not just a list of Main signatures)
    has_main_body = bool(re.search(
        r'\bmethod\s+Main\b(?:.*?\n)+?.*?\bend\s+Main\b',
        code, re.DOTALL
    ))
    is_program = has_module and has_class and has_main_body

    if is_program:
        expectation = _extract_expected_output(code, prev_prose)
        return 'program', expectation, ''

    # fragment
    return 'fragment', 'compiles', ''


def _is_signature_block(code):
    """
    Heuristic: is this a pure API signature listing (skip:signature)?

    Criteria: starts with a class/interface declaration, has many declaration
    lines (method/property/const stubs), and no executable statements or
    constructor/method *bodies* (i.e. no 'end <method-name>;' closing body lines).
    """
    lines = [l for l in code.splitlines() if l.strip() and not l.strip().startswith('//')]
    if not lines:
        return False

    first = lines[0].strip()

    # Must start with a class/interface declaration
    is_class_like = bool(re.match(
        r'(public\s+|private\s+|internal\s+)?(static\s+|abstract\s+|sealed\s+)*'
        r'(class|interface)\s+\w',
        first
    ))
    if not is_class_like:
        return False

    # Require multiple lines (single-liner records/classes skip this path)
    if len(lines) < 3:
        return False

    # Count declaration-style lines (stubs without bodies)
    stub_re = re.compile(
        r'^\s*(public|private|internal|protected)?\s*(static|virtual|override|abstract|readonly)?\s*'
        r'(method|property|constructor|const|field|destructor)\s+'
    )
    stub_count = sum(1 for l in lines if stub_re.match(l))

    # Count executable/body lines
    stmt_re = re.compile(
        r'^\s*(var\s|using\s|if\s|while\s|for\s|return\s|return;|throw\s|try\s*$|'
        r'case\s|switch\s|Console\.|:=\s*new\b|do\s+Console|\._\w+\s*:=)'
    )
    stmt_count = sum(1 for l in lines if stmt_re.match(l))

    # A constructor or method body contains assignments of the form `Field := param`
    assign_re = re.compile(r'^\s*[A-Z]\w+\s*:=\s*\w')
    body_assign_count = sum(1 for l in lines if assign_re.match(l))

    # Expression-bodied methods with non-trivial bodies (=> <something non-trivial>)
    # These indicate actual implementation, not just signatures
    expr_body_re = re.compile(r'=>\s*.{5,}')   # => followed by 5+ chars of content
    expr_body_count = sum(1 for l in lines if expr_body_re.search(l)
                         and not l.strip().startswith('//'))

    # Multiple class/record declarations in one block (e.g. Shape hierarchy)
    type_decl_re = re.compile(
        r'^\s*(public\s+|private\s+|internal\s+)?(sealed\s+|abstract\s+|static\s+)*'
        r'(class|record|enum|interface)\s+\w'
    )
    type_decl_count = sum(1 for l in lines if type_decl_re.match(l))

    # If it's mostly stubs with no statement/body content → signature
    has_bodies = (stmt_count > 0 or body_assign_count > 0 or
                  expr_body_count > 0 or type_decl_count > 1)
    is_sig = (stub_count >= 3 and not has_bodies)
    return is_sig


def _extract_expected_output(code, prose):
    """Try to extract expected stdout from prose or trailing comments."""
    # Look for // => ... or // prints: ... in code
    m = re.search(r'//\s*=>\s*(.+)', code)
    if m:
        return f"run:{m.group(1).strip()}"
    m = re.search(r'//\s*prints?:?\s*(.+)', code, re.IGNORECASE)
    if m:
        return f"run:{m.group(1).strip()}"
    # Prose: "prints X" or "outputs X"
    if prose:
        m = re.search(r'(?:prints?|outputs?|produces?)\s+"([^"]+)"', prose, re.IGNORECASE)
        if m:
            return f"run:{m.group(1)}"
    return 'compiles'


def parse_doc(doc_path, doc_tag):
    """
    Parse a markdown file; yield dicts with:
      block_idx, heading, code, lang, prev_prose
    """
    with open(doc_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    current_heading = f"{doc_tag} top"
    in_fence = False
    fence_lang = ''
    fence_lines = []
    prose_lines = []
    block_idx = 0
    results = []

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.rstrip('\n')

        if not in_fence:
            # Track headings
            hm = re.match(r'^(#{1,6})\s+(.*)', stripped)
            if hm:
                current_heading = stripped
                prose_lines = []
            else:
                prose_lines.append(stripped)

            # Fence open?
            fm = re.match(r'^```(\w*)', stripped)
            if fm:
                in_fence = True
                fence_lang = fm.group(1)
                fence_lines = []
        else:
            if stripped.startswith('```') and not re.match(r'^```\w', stripped):
                # Fence close
                in_fence = False
                code = '\n'.join(fence_lines)
                prev_prose = '\n'.join(prose_lines)  # all prose since last heading or prior block
                results.append({
                    'idx': block_idx,
                    'heading': current_heading,
                    'code': code,
                    'lang': fence_lang,
                    'prev_prose': prev_prose,
                })
                block_idx += 1
                prose_lines = []
                fence_lines = []
                fence_lang = ''
            else:
                fence_lines.append(stripped)

        i += 1

    return results


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

manifest_rows = []

for doc_tag, doc_rel in DOCS:
    doc_path = os.path.join(REPO_ROOT, doc_rel)
    if not os.path.exists(doc_path):
        print(f"WARN: {doc_path} not found, skipping", file=sys.stderr)
        continue

    blocks = parse_doc(doc_path, doc_tag)
    print(f"{doc_tag}: {len(blocks)} blocks", file=sys.stderr)

    for b in blocks:
        seq = f"{b['idx']:04d}"
        case_id = f"{doc_tag}_{seq}"
        notes_acc = []
        cls, expectation, notes = classify_block(
            b['code'], b['lang'], b['prev_prose'], notes_acc
        )
        if notes_acc:
            if notes:
                notes = notes + '; ' + '; '.join(notes_acc)
            else:
                notes = '; '.join(notes_acc)

        heading_slug = slugify_heading(b['heading'])

        # Write .bzl file
        case_path = os.path.join(CASES_DIR, f"{case_id}.bzl")
        with open(case_path, 'w', encoding='utf-8') as f:
            f.write(b['code'])
            if not b['code'].endswith('\n'):
                f.write('\n')

        manifest_rows.append((case_id, doc_tag, heading_slug, cls, expectation, notes))

# Write manifest
with open(MANIFEST, 'w', encoding='utf-8') as f:
    f.write("case-id\tdoc\tsection\tclass\texpectation\tnotes\n")
    for row in manifest_rows:
        f.write('\t'.join(str(x) for x in row) + '\n')

print(f"Wrote {len(manifest_rows)} cases to {CASES_DIR}", file=sys.stderr)
print(f"Wrote manifest to {MANIFEST}", file=sys.stderr)

# Summary
from collections import Counter
counts = Counter(r[3] for r in manifest_rows)
print("Classification summary:", file=sys.stderr)
for cls, cnt in sorted(counts.items()):
    print(f"  {cls}: {cnt}", file=sys.stderr)
