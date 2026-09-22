#!/usr/bin/env python3
"""
Extract fenced code blocks from the four Bezel docs.
Version 2 — re-extraction for v0.138 docs.

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

v2 improvements over v1:
  1. skip:elided — blocks containing '...' pseudo-code (e.g. `method X() ... end;`
     or `class Foo ... end Foo;`) are skipped rather than sent to the compiler as
     broken syntax.  This is distinct from `...` inside a string literal
     ("Connection(...)") or purely inside a comment line (// ...).
  2. Context-chaining — for each fragment/error case, the extractor inspects
     earlier blocks in the SAME SECTION of the SAME DOC.  If those earlier blocks
     define types (class/record/interface) that the current block references, they
     are prepended to the .bzl file so the block has its full context.  Chained
     cases are annotated with "chained:<n>" in the manifest notes column where <n>
     is the number of prepended blocks.
  3. Re-extracted from the current docs (v0.138).

Usage:
  python3 scripts/extract_doc_examples.py [--repo-root PATH]
"""

import os
import re
import sys

DOCS = [
    ("spec",   "docs/Bezel_Language_Spec.md"),
    ("codegen", "docs/Bezel_For_Code_Generation.md"),
    ("guide1", "docs/Bezel_Guide_01_Memory_and_Ownership.md"),
    ("guide2", "docs/Bezel_Guide_02_Errors_and_Nullability.md"),
    ("guide3", "docs/Bezel_Guide_03_Modeling_Data.md"),
    ("guide4", "docs/Bezel_Guide_04_Concurrency.md"),
    ("guide5", "docs/Bezel_Guide_05_Collections_and_Iteration.md"),
    ("guide6", "docs/Bezel_Guide_06_Contracts_and_Purity.md"),
    ("stdlib", "docs/Bezel_Standard_Library.md"),
]

_DEFAULT_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Argument handling: the script used to take sys.argv[1] verbatim as the output
# root, which silently turned `python3 scripts/extract_doc_examples.py --help`
# into an extraction to `./--help/cases` and `./--help/manifest.tsv` and littered
# the repo root three times across two sessions. argparse refuses flag shapes
# upfront with exit code 2; only a `--repo-root PATH` (or its absence) is
# admitted. Default behavior (no flag, default repo-root) is unchanged. (per #3262)
import argparse
_argparser = argparse.ArgumentParser(
    description="Extract fenced bezel code blocks from the normative docs into "
                "tests/conformance/cases/<doc-tag>_<seq>.bzl and write "
                "tests/conformance/manifest.tsv.",
    usage="%(prog)s [--repo-root PATH]",
)
_argparser.add_argument(
    "--repo-root", default=_DEFAULT_REPO_ROOT, metavar="PATH",
    help="Repository root to extract into (default: parent of this script).",
)
_args = _argparser.parse_args()
REPO_ROOT = os.path.abspath(_args.repo_root)
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

# Grammar keywords that strongly suggest a runnable file
HAS_MODULE_RE  = re.compile(r'^\s*module\s+\w', re.MULTILINE)
HAS_CLASS_RE   = re.compile(r'^\s*(public\s+|private\s+|internal\s+)?(static\s+)?class\s+\w', re.MULTILINE)

# Type declaration pattern — used to identify context-defining blocks
TYPE_DECL_RE = re.compile(
    r'^\s*(public\s+|private\s+|internal\s+)?(sealed\s+|abstract\s+|static\s+)*'
    r'(class|record|interface)\s+([A-Z]\w*)',
    re.MULTILINE
)

# Identifier pattern for extracting referenced type names (PascalCase)
PASCAL_REF_RE = re.compile(r'\b([A-Z][A-Za-z0-9_]*)\b')


def has_pseudo_elision(code):
    """
    Return True if the code block uses '...' as pseudo-code (a placeholder for
    omitted implementation), NOT merely inside a string literal or comment.

    Elision patterns that indicate skippable pseudo-code:
      - `method M() ... end;`  (abbreviated method body)
      - `class Foo ... end Foo;`  (abbreviated class body)
      - `do ... end`  (abbreviated block)
      - standalone line consisting only of `...` or `...;`
      - EBNF-style / syntax diagram that uses `...` to mean "more syntax"
      - `- var name := expression; ...; name.Dispose();`  (spec prose pattern)

    Non-elision uses of '...' that must NOT trigger skip:
      - Inside a string literal: `new Connection("...")`
      - Purely inside a line comment: `// ...`
      - Open-ended range expressions (but these use `10..` not `...`)
    """
    lines = code.splitlines()
    for line in lines:
        stripped = line.strip()
        # Skip blank lines and pure comment lines
        if not stripped or stripped.startswith('//'):
            continue
        # Remove string literals (naive but sufficient)
        no_strings = re.sub(r'"[^"]*"', '""', stripped)
        no_strings = re.sub(r"'[^']*'", "''", no_strings)
        # Remove the comment tail
        no_strings = re.sub(r'//.*$', '', no_strings)

        if '...' not in no_strings:
            continue

        # Pattern 1: `... end` or `... end X` — abbreviated body
        if re.search(r'\.\.\.\s*(end\b|;)', no_strings):
            return True
        # Pattern 2: standalone line is just `...` or `...;`
        if re.match(r'^\s*\.\.\.;?\s*$', stripped):
            return True
        # Pattern 3: EBNF / syntax diagram header with ...
        if re.search(r'^visibility\s+', stripped):
            return True
        # Pattern 4: list item pseudo-code pattern `- ... ; name.Method()`
        if re.match(r'^\s*-\s+', stripped) and '...' in no_strings:
            return True
        # Pattern 5: class/method/interface header line ending in ...
        if re.search(r'(class|method|interface|record)\s+\w.*\.\.\.\s*$', no_strings):
            return True

    return False


def _types_defined_in(code):
    """Return the set of PascalCase type names defined (class/record/interface) in code."""
    return {m.group(4) for m in TYPE_DECL_RE.finditer(code)}


def _types_referenced_in(code):
    """Return the set of PascalCase identifiers referenced in code."""
    return set(PASCAL_REF_RE.findall(code))


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

    # 4. skip:elided — pseudo-code blocks with '...' elision (v2)
    if has_pseudo_elision(code):
        return 'skip', 'n/a', 'skip:elided'

    # 5. Pure signature listing (stdlib): class with only method/property/const declarations
    is_sig = _is_signature_block(code)
    if is_sig:
        return 'skip', 'n/a', 'skip:signature'

    # 6. Error?
    # Inline // ERROR or // error marker is the most reliable signal.
    inline_has_error = bool(INLINE_ERROR_RE.search(code))
    # Prose: only the LAST non-empty line immediately before the block.
    prose_lines_list = [l for l in (prev_prose or '').splitlines() if l.strip()]
    last_prose_line = prose_lines_list[-1] if prose_lines_list else ''
    # Only trigger on prose if the very last line says "is rejected:" / "is a compile error:"
    prose_has_close_error = bool(CLOSE_ERROR_RE.search(last_prose_line))
    # Also check for ✗/✘ symbols in last_prose_line
    prose_has_symbol = bool(re.search(r'[✗✘]', last_prose_line))
    is_error = inline_has_error or prose_has_close_error or prose_has_symbol

    if is_error:
        return 'error', 'diagnose', ''

    # 7. Program vs fragment
    has_module = bool(HAS_MODULE_RE.search(code))
    has_class = bool(HAS_CLASS_RE.search(code))

    # Only classify as program if it has a module + class + Main body
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
    constructor/method *bodies*.
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
    expr_body_re = re.compile(r'=>\s*.{5,}')
    expr_body_count = sum(1 for l in lines if expr_body_re.search(l)
                         and not l.strip().startswith('//'))

    # Multiple class/record declarations in one block (e.g. Shape hierarchy)
    type_decl_re = re.compile(
        r'^\s*(public\s+|private\s+|internal\s+)?(sealed\s+|abstract\s+|static\s+)*'
        r'(class|record|enum|interface)\s+\w'
    )
    type_decl_count = sum(1 for l in lines if type_decl_re.match(l))

    # Expression-body continuation lines: when a method signature ends with `=>`
    # and the body appears on the NEXT line (e.g. `new Vector2(...);`), the
    # expr_body_re above misses it.  Detect any indented line that starts with
    # `new <UpperCase>` — these only appear in real method/constructor bodies.
    new_body_re = re.compile(r'^\s{2,}new\s+[A-Z]')
    new_body_count = sum(1 for l in lines if new_body_re.match(l))

    # If it's mostly stubs with no statement/body content → signature.
    # NOTE: type_decl_count > 1 is intentionally NOT included in has_bodies.
    # A block that lists multiple class/interface declarations whose members are
    # ALL bodyless stubs (e.g. IEvent<T> + EventSource<T> API listing) is still
    # a pure signature block.  The old code counted multiple type decls as
    # evidence of "has bodies", which incorrectly forced spec_0150/stdlib_0083/
    # stdlib_0026/stdlib_0027/stdlib_0038 to fragment/compiles (issue #683 sub-2).
    has_bodies = (stmt_count > 0 or body_assign_count > 0 or
                  expr_body_count > 0 or new_body_count > 0)
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
                prev_prose = '\n'.join(prose_lines)
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
# Context-chaining (v2)
# ---------------------------------------------------------------------------

def build_section_context(blocks):
    """
    For each block in the list, compute a 'context_prefix' — the concatenation
    of earlier blocks in the same section that define types referenced by this
    block.

    Returns a parallel list of (context_code, chain_count) tuples:
      context_code  — string to prepend (empty if no chaining needed)
      chain_count   — number of blocks prepended (0 if none)

    Algorithm:
      - Iterate blocks in document order.
      - Maintain a per-section list of (code, defined_types) for non-skipped
        blocks that define at least one PascalCase type.
      - For each block, check which PascalCase identifiers it references.
        Collect ALL earlier type-defining blocks in the same section whose
        exported types overlap with the references.
      - Prepend those blocks' code (in order) to form the context prefix.
    """
    # Map: heading -> list of (idx, code, defined_types_set)
    # We only track blocks that define at least one type (non-elided, non-skip source)
    section_type_providers = {}  # heading -> [(block_idx, code, types_set)]

    chain_info = []  # parallel to blocks: (context_code, chain_count)

    for b in blocks:
        heading = b['heading']
        code = b['code']

        # What types does THIS block define?
        my_types = _types_defined_in(code)
        # What types does THIS block reference?
        my_refs = _types_referenced_in(code)

        # Look up earlier providers in same section
        providers = section_type_providers.get(heading, [])

        # Find which providers supply types that THIS block references but does
        # NOT itself declare.
        #
        # The `- my_types` term is load-bearing: the docs routinely re-show a
        # type's declaration in a later block of the same section (a positional
        # record restated beside its serialization form, a class restated with
        # doc comments). Such a block both DEFINES and REFERENCES the name, so
        # the original `ptypes & my_refs` test chained the earlier declaration in
        # ahead of the block's own — and the case then failed on
        #   "a top-level type named `X` is already declared in this module
        #    — top-level type names must be unique (§11.3 well-formedness)"
        # which is an artifact of the extractor's scaffolding, not a claim of the
        # document. Skipping a provider whose types the block redeclares is the
        # narrow fix; a provider supplying some OTHER type the block needs is
        # still chained. (Measured on the v0.278 corpus: spec_0021, spec_0040 and
        # stdlib_0019 fail exactly this way.)
        my_needs = my_refs - my_types
        needed_providers = []
        for (pidx, pcode, ptypes) in providers:
            if ptypes & my_types:
                continue          # the block redeclares it; chaining would clash
            if ptypes & my_needs:
                needed_providers.append((pidx, pcode))

        if needed_providers:
            # Sort by block index (they should already be in order, but be safe)
            needed_providers.sort(key=lambda x: x[0])
            ctx_code = '\n\n'.join(c for _, c in needed_providers)
            chain_info.append((ctx_code, len(needed_providers)))
        else:
            chain_info.append(('', 0))

        # Register this block as a type provider for its section (if it defines types)
        if my_types:
            if heading not in section_type_providers:
                section_type_providers[heading] = []
            section_type_providers[heading].append((b['idx'], code, my_types))

    return chain_info


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

    # Compute context chains for the whole doc at once
    chain_info = build_section_context(blocks)

    for b, (ctx_code, chain_count) in zip(blocks, chain_info):
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

        # Build the .bzl content.
        # For non-skip blocks that have a context chain, prepend the context.
        if cls != 'skip' and ctx_code:
            # Prepend context, separated by a blank line
            file_content = ctx_code.rstrip('\n') + '\n\n' + b['code']
            if chain_count:
                chain_note = f"chained:{chain_count}"
                notes = (notes + '; ' + chain_note) if notes else chain_note
        else:
            file_content = b['code']

        # Write .bzl file
        case_path = os.path.join(CASES_DIR, f"{case_id}.bzl")
        with open(case_path, 'w', encoding='utf-8') as f:
            f.write(file_content)
            if not file_content.endswith('\n'):
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

chained_count = sum(1 for r in manifest_rows if 'chained:' in r[5])
elided_count = sum(1 for r in manifest_rows if 'skip:elided' in r[5])
print(f"  chained cases: {chained_count}", file=sys.stderr)
print(f"  skip:elided cases: {elided_count}", file=sys.stderr)
