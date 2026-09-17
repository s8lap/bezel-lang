"""conformance.py — the spec-example conformance harness, ported from
run_conformance.sh (#3010 gate-infra migration; per Plan-A 5.1).

WHAT THIS DOES
  For each non-`skip` case in manifest.tsv:
    - classify the fragment by a content heuristic and wrap it in the standard
      module/class/Main harness (or compile it as a library / as-is program),
    - compile it under a MemoryMax/swap=0 cgroup cage with a 30 s timeout,
    - check the manifest expectation: `error` ⇒ must compile-fail;
      `compiles` ⇒ no crash/signal; `run:X` ⇒ stdout first line; `rc:N` ⇒ exit.
  Exit code 70 (runtime unhandled-exception) is acceptable for `compiles`;
  >=128 / 139 / 134 is a crash/signal ⇒ FAIL.

WHY PYTHON
  The bash harness had no unit tests and — the load-bearing gap, per Plan-A 5.1
  — NO `PASS>0` guard: if the manifest loop read nothing (manifest moved/empty,
  or every case `skip`), then PASS=0, FAIL=0 ⇒ exit 0, a SILENT GREEN over a run
  that validated nothing. main() now FAILS LOUD when PASS==0. The wrap
  heuristics, crash classification, and expectation evaluation are pure
  functions of the source/exit text, so they are unit-tested directly
  (test_conformance.py); the subprocess cage is the only non-pure seam and is
  injectable via the `run` callable.

MEMORY DISCIPLINE
  Every bezelc call AND every compiled-program run is wrapped in
  `systemd-run --user --scope -q -p MemoryMax=4G -p MemorySwapMax=0 -- timeout
  30`, exactly as the bash did. Cases are processed SEQUENTIALLY.

Stdlib only (subprocess/tempfile/re/csv); no pytest, no new deps (#3010).
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

# -----------------------------------------------------------------------
# Wrap modes (mirror run_conformance.sh wrap_fragment's emitted mode strings)
# -----------------------------------------------------------------------
WRAP_AS_IS_PROGRAM = "as-is-program"  # has `module` + `method Main` → run as-is
WRAP_AS_IS_LIB = "as-is-lib"          # has `module`, no Main → compile as library
WRAP_LIB = "lib"                      # wrapped with `module Conf;`, compile -c
WRAP_STMT = "stmt"                    # wrapped as Main-body statements

# Per-case compile/run timeout (seconds), inside the systemd-run scope. per the
# bash harness comment block.
CASE_TIMEOUT_S = 30

# The MemoryMax/MemorySwapMax cage every bezelc invocation and every
# compiled-program run is wrapped in (#3010 memory discipline; anti-freeze
# cgroup policy). Sequential, never parallel.
_CAGE_ARGV = ["systemd-run", "--user", "--scope", "-q",
              "-p", "MemoryMax=4G", "-p", "MemorySwapMax=0",
              "--", "timeout", str(CASE_TIMEOUT_S)]

# -----------------------------------------------------------------------
# Wrap heuristic — PURE function of the source text.
# Mirrors run_conformance.sh wrap_fragment()'s grep cascade.
# -----------------------------------------------------------------------

# Step 1: already-declared module (grep -qE over the whole body).
_MODULE_RE = re.compile(r'^\s*module\s+\w', re.MULTILINE)
_MAIN_RE = re.compile(r'\bmethod\s+Main\b')

# `first_nonblank`: the first line matching `^[[:space:]]*[^[:space:]/]` — the
# first non-blank line that does not start with `/` (a comment leader). grep -m1.
_FNS_LINE_RE = re.compile(r'^[ \t]*[^ \t/]')

# Steps 2-6: applied to the first-nonblank line. Translations of the bash EREs.
# v0.247 corpus regeneration: `choice` joined the type-declaration alternation,
# and TypeAliasDecl (`type X := T;`) / ExternBlock (`extern "lib" ... end;`)
# gained lib-wrap dispatch — all three are TopDecls per spec Appendix A.1, but
# the original cascade predates their appearance as fragment-leading forms in
# the docs, so such fragments fell through to the stmt wrap and could never
# compile.
# v0.242 (#3052): `closed` joined A.3's `Modifier` production as the dedicated
# spelling of the retired `sealed abstract` pair, and the docs' §11.7 worked
# example leads with `public closed class Shape`. Without it here the cascade
# fell through to the stmt wrap and the fragment could never compile.
_TYPE_DECL_RE = re.compile(
    r'^\s*(?:(?:public|private|internal)\s+)?'
    r'(?:(?:sealed|abstract|closed|static)\s+)*'
    r'(?:class|record|enum|interface|choice)\s+\w')
# Visibility prefix admitted since the v0.247 regeneration: §26.1 defaults
# extensions to internal, so the docs' fragments now spell `public extension`.
_EXT_RE = re.compile(r'^\s*(?:(?:public|private|internal)\s+)?extension\b')
_TYPEALIAS_RE = re.compile(r'^\s*type\s+\w')
_EXTERN_BLOCK_RE = re.compile(r'^\s*extern\b')
_IMPORT_RE = re.compile(r'^\s*import\s+')
_METHOD_RE = re.compile(
    r'^\s*(?:(?:public|private|internal|protected)\s+)?'
    r'(?:(?:static|virtual|override|abstract|async|pure)\s+)*'
    r'(?:method|property|constructor|destructor)\s+')
_FIELD_RE = re.compile(
    r'^\s*(?:(?:public|private|internal)\s+)?'
    r'(?:(?:owned|readonly|static)\s+)?_?\w+\s*:\s*\w')


def first_nonblank_line(code):
    """The first non-blank, non-`/`-leading line, mirroring
    `grep -m1 -E '^[[:space:]]*[^[:space:]/]'`. None when no line qualifies
    (the bash then falls through every first-nonblank check to the default)."""
    for line in code.splitlines():
        if _FNS_LINE_RE.match(line):
            return line
    return None


def _indent_four(code):
    """Replicate `sed 's/^/    /'`: prepend four spaces to every line. sed
    treats a trailing newline as a line terminator (no phantom final empty
    line), so one trailing newline is stripped and re-added. The compiler does
    not care about a trailing newline; matching sed keeps the wrapped source
    byte-close to the bash harness for the equivalence proof."""
    trailing_nl = code.endswith("\n")
    body = code[:-1] if trailing_nl else code
    indented = "\n".join("    " + line for line in body.split("\n"))
    return indented + ("\n" if trailing_nl else "")


def classify_wrap(code):
    """Return (wrap_mode, wrapped_src) for a fragment body. A pure translation
    of run_conformance.sh wrap_fragment()'s heuristic cascade; the wrapped_src
    concatenations mirror the bash printf/cat templates exactly."""
    fn = first_nonblank_line(code)

    # 1. Already has a module declaration?
    if _MODULE_RE.search(code):
        if _MAIN_RE.search(code):
            return WRAP_AS_IS_PROGRAM, code
        return WRAP_AS_IS_LIB, code

    # 2. Type declaration at module level → wrap as library.
    if fn is not None and _TYPE_DECL_RE.match(fn):
        return WRAP_LIB, "module Conf;\n\n" + code

    # 3. Extension block at module scope (§26.x) → wrap as library.
    if fn is not None and _EXT_RE.match(fn):
        return WRAP_LIB, "module Conf;\n\n" + code

    # 3b. Type alias / extern block at module scope (TopDecls per spec
    # Appendix A.1) → wrap as library. Added at the v0.247 regeneration.
    if fn is not None and (_TYPEALIAS_RE.match(fn) or _EXTERN_BLOCK_RE.match(fn)):
        return WRAP_LIB, "module Conf;\n\n" + code

    # 4. Import statement → hoist the leading import block and dispatch on the
    # remainder (v0.247 regeneration). The original template appended an empty
    # `class W`, which only compiles when everything after the imports is a
    # declaration; an import-led fragment whose remainder is STATEMENTS (the
    # docs' LINQ examples) then leaves them stranded at module level. Splitting
    # lets the remainder take whichever wrap it needs, with the imports re-homed
    # directly after the module line (SourceFile ::= ModuleDecl Import* TopDecl*,
    # spec A.1).
    if fn is not None and _IMPORT_RE.match(fn):
        lines = code.splitlines(keepends=True)
        i = 0
        while i < len(lines) and (
                _IMPORT_RE.match(lines[i]) or not lines[i].strip()
                or lines[i].lstrip().startswith("//")):
            i += 1
        lead, rest = "".join(lines[:i]), "".join(lines[i:])
        if not lead.endswith("\n"):
            lead += "\n"
        if not rest.strip():
            return (WRAP_STMT,
                    "module Conf;\n" + code
                    + "\nclass W\n  public static method Main() end Main;\nend W;\n")
        mode, wrapped_rest = classify_wrap(rest)
        # insert the import block right after the synthesized module line
        rl = wrapped_rest.splitlines(keepends=True)
        for j, line in enumerate(rl):
            if _MODULE_RE.match(line):
                wrapped = "".join(rl[:j + 1]) + lead + "".join(rl[j + 1:])
                break
        else:
            wrapped = lead + wrapped_rest
        return mode, wrapped

    # 5. Method/property/ctor/dtor declaration → wrap inside a class, library.
    if fn is not None and _METHOD_RE.match(fn):
        return WRAP_LIB, "module Conf;\nclass W\n" + code + "\nend W;\n"

    # 6. Field declaration → wrap inside a class (with a Main), library.
    if fn is not None and _FIELD_RE.match(fn):
        return (WRAP_LIB,
                "module Conf;\nclass W\n" + code
                + "\n  public static method Main() end Main;\nend W;\n")

    # 7. Default: treat as statements in Main.
    return (WRAP_STMT,
            "module Conf;\nclass W\n  public static method Main()\n"
            + _indent_four(code) + "\n  end Main;\nend W;\n")


# -----------------------------------------------------------------------
# The composite ("mixed") wrap — opt-in via the manifest note `wrap:mixed`.
#
# WHY: classify_wrap dispatches the WHOLE block on its FIRST line, so a block
# that mixes top-level declarations with top-level statements (the docs'
# standard "here is the type, here is a call on it, here is what it resolves
# to" shape) can never compile: whichever wrap the first line picks strands the
# other half. The v0.247 corpus regeneration recorded exactly that as skip
# rule R7 — `skip:mixed`, "no single wrap can hold both" — for 36 cases, and
# #683 found the same thing for extension blocks.
#
# One wrap CAN hold both, because SourceFile ::= ModuleDecl Import* TopDecl*
# (spec Appendix A.1) admits declarations and a class beside each other: sort
# the block's top-level items into imports / module declarations / class
# members / statements, and emit
#
#     module Conf;  <imports>  <module decls>
#     class W  <members>  public static method Main()  <statements>  end Main;
#     end W;
#
# This adds only ENCLOSING scaffolding. No token inside the block is rewritten,
# reordered within its group, or dropped — the corpus's value is that it runs
# the documents verbatim, so a case is never made to compile by editing what
# the document says.
#
# Item boundaries use the column-0 invariant the docs' fenced blocks hold to: a
# top-level item starts in column 0 and its body is indented. A column-0
# continuation keyword (`end`/`else`/`elif`/`catch`/`finally`) belongs to the
# item in progress, and an absorbed `end` closes it.
# -----------------------------------------------------------------------

_COL0_RE = re.compile(r'^\S')
_ITEM_CONT_RE = re.compile(r'^(?:end\b|else\b|elif\b|catch\b|finally\b)')
_ITEM_END_RE = re.compile(r'^end\b')

_ITEM_MODDECL_RE = re.compile(
    r'^\s*(?:(?:public|private|internal)\s+)?'
    r'(?:(?:sealed|abstract|closed|static)\s+)*'
    r'(?:class|record|enum|interface|choice|extension|type|extern)\b')
_ITEM_MEMBER_RE = re.compile(
    r'^\s*(?:(?:public|private|internal|protected)\s+)?'
    r'(?:(?:static|virtual|override|abstract|async|pure|sealed|readonly|owned'
    r'|scoped)\s+)*'
    r'(?:method|property|constructor|destructor|const)\s+')
_ITEM_FIELD_RE = re.compile(
    r'^\s*(?:public|private|internal|protected)\s+'
    r'(?:(?:owned|readonly|static|scoped)\s+)*_?\w+\s*:\s*\S')


def _is_lead_in(lines):
    """True when everything accumulated so far is blank or a `//` comment — a
    lead-in that belongs to the item it precedes, not an item of its own.
    Without this a doc comment above a declaration would be split off and, being
    codeless, sorted into the statement group: legal, but it would tear the
    document's own annotation away from what it annotates."""
    for line in lines:
        s = line.strip()
        if s and not s.startswith("//"):
            return False
    return True


def split_top_level_items(code):
    """Split a doc block into its top-level items, preserving text exactly.
    ''.join(split_top_level_items(code)) == code."""
    lines = code.splitlines(keepends=True)
    items = []
    cur = []
    for line in lines:
        if not cur or not line.strip() or not _COL0_RE.match(line):
            cur.append(line)
            continue
        if _ITEM_CONT_RE.match(line):
            cur.append(line)
            if _ITEM_END_RE.match(line):
                items.append("".join(cur))
                cur = []
            continue
        if _is_lead_in(cur):
            cur.append(line)
            continue
        items.append("".join(cur))
        cur = [line]
    if cur:
        items.append("".join(cur))
    return items


def classify_top_level_item(item):
    """'import' | 'moddecl' | 'member' | 'stmt' for one top-level item. Judged
    on the item's first non-blank, non-comment line (comments and blank lines
    ride along with the item they precede)."""
    head = ""
    for line in item.splitlines():
        s = line.strip()
        if s and not s.startswith("//"):
            head = line
            break
    if not head.strip():
        return "stmt"
    if _IMPORT_RE.match(head):
        return "import"
    if _ITEM_MODDECL_RE.match(head):
        return "moddecl"
    if _ITEM_MEMBER_RE.match(head) or _ITEM_FIELD_RE.match(head):
        return "member"
    return "stmt"


def wrap_mixed(code):
    """The composite wrap. Always yields a `Main`, so the case is an executable
    program (WRAP_STMT) and its run phase is checked like any other."""
    groups = {"import": [], "moddecl": [], "member": [], "stmt": []}
    for item in split_top_level_items(code):
        groups[classify_top_level_item(item)].append(item)
    out = ["module Conf;\n"]
    out += groups["import"]
    out += groups["moddecl"]
    out.append("\nclass W\n")
    out += groups["member"]
    out.append("  public static method Main()\n")
    out += [_indent_four(item) for item in groups["stmt"]]
    out.append("\n  end Main;\nend W;\n")
    return WRAP_STMT, "".join(out)


# -----------------------------------------------------------------------
# Ambient bindings — env.bzl, opt-in per case via the note `env:a,b,c`.
#
# WHY: the corpus's largest skip class by far is `skip:incomplete-fragment`
# (176 rows at the v0.247 regeneration). Measured against this tree's bezelc,
# 160 of those 176 fail on an UNBOUND NAME — a prose-context local or helper
# the surrounding text assumes and the block does not declare. The wrap is not
# the problem; the fragment is a sentence with a pronoun in it.
#
# preamble.bzl already establishes the answer for illustrative *types*
# ("Many spec example fragments reference illustrative helper TYPES that the
# surrounding prose assumes exist … without defining them in the fragment").
# env.bzl is the same mechanism one level down, for the values and members
# those fragments assume, with two disciplines that keep it honest:
#
#   1. Each entry is opted into BY NAME, per case. Nothing is ambient by
#      default, so a reader sees exactly which assumption each row was granted.
#   2. Each entry's type must be FORCED BY THE FRAGMENT'S OWN TEXT (or stated
#      in the prose), and env.bzl records that derivation beside the entry.
#      Choosing a binding by trying spellings until the compiler stops
#      complaining is circular, and worse than circular here: it measures
#      accept-anything holes rather than conformance. That is not hypothetical
#      — an auto-solver written to size this bucket "solved" cases by binding
#      called names to `int` (#3153, a call on a non-callable compiles and is
#      dropped) and by binding `?.` receivers to `int` (#3155, `?.` never
#      resolves its member). Both are filed; neither is a binding.
#
# An entry whose anchor is missing FAILS the case loudly. A silently-unapplied
# ambient would make its row pass for a reason nobody chose.
# -----------------------------------------------------------------------

ENV_LOCAL = "local"      # a statement at the top of the Main body
ENV_MEMBER = "member"    # a member of the synthesized class W
ENV_TYPE = "type"        # a top-level declaration beside the module's others

_ENV_HEADER_RE = re.compile(r'^//@\s+(local|member|type)\s+(\S+)\s*$')
_ENV_MAIN_ANCHOR_RE = re.compile(r'^(\s*)public static method Main\(\)\s*$')
_ENV_CLASS_ANCHOR_RE = re.compile(r'^class W\s*$')


class EnvError(Exception):
    """An env entry could not be applied — an unknown name or a missing anchor.
    Raised rather than swallowed: see the module comment above."""


def parse_env(text):
    """Parse env.bzl into {name: (kind, text)}. Entries are introduced by a
    `//@ <kind> <name>` header line and run to the next header or EOF; the
    prose between the header and the code is the derivation record and rides
    along into the emitted source as comments."""
    entries = {}
    name = kind = None
    body = []
    for line in (text or "").splitlines(keepends=True):
        m = _ENV_HEADER_RE.match(line.rstrip("\n"))
        if m:
            if name is not None:
                entries[name] = (kind, "".join(body))
            kind, name = m.group(1), m.group(2)
            body = []
            continue
        if name is not None:
            body.append(line)
    if name is not None:
        entries[name] = (kind, "".join(body))
    return entries


def env_names(notes):
    """The ordered, de-duplicated env entry names a manifest note asks for."""
    out = []
    for token in note_tokens(notes):
        if not token.startswith("env:"):
            continue
        for n in token[len("env:"):].split(","):
            n = n.strip()
            if n and n not in out:
                out.append(n)
    return sorted(out)


def splice_env(wrapped_src, names, entries):
    """Insert the named ambient bindings at their anchors. `type` entries go
    beside the module's other top-level declarations (after the module line and
    any imports, per `SourceFile ::= ModuleDecl Import* TopDecl*`, spec Appendix
    A.1), `member` entries directly inside `class W`, and `local` entries at the
    top of the `Main` body, ahead of the fragment's own statements."""
    by_kind = {ENV_TYPE: [], ENV_MEMBER: [], ENV_LOCAL: []}
    for n in names:
        if n not in entries:
            raise EnvError("no env entry named `%s`" % n)
        kind, body = entries[n]
        by_kind[kind].append(body)

    if by_kind[ENV_TYPE]:
        wrapped_src = splice_preamble(wrapped_src, "".join(by_kind[ENV_TYPE]))

    lines = wrapped_src.splitlines(keepends=True)
    out = []
    placed = {ENV_MEMBER: not by_kind[ENV_MEMBER], ENV_LOCAL: not by_kind[ENV_LOCAL]}
    for line in lines:
        out.append(line)
        if not placed[ENV_MEMBER] and _ENV_CLASS_ANCHOR_RE.match(line.rstrip("\n")):
            out.extend(by_kind[ENV_MEMBER])
            placed[ENV_MEMBER] = True
            continue
        m = _ENV_MAIN_ANCHOR_RE.match(line.rstrip("\n"))
        if not placed[ENV_LOCAL] and m:
            out.extend(_indent_four(b) for b in by_kind[ENV_LOCAL])
            placed[ENV_LOCAL] = True
    for kind, ok in placed.items():
        if not ok:
            raise EnvError(
                "no anchor for the `%s` env entries in the wrapped source "
                "(a `%s` binding needs the mixed wrap, which always emits "
                "`class W` and a `Main`)"
                % (kind, kind))
    return "".join(out)


# -----------------------------------------------------------------------
# Manifest `notes` is a `;`-separated token list. Tokens the harness acts on:
#   preamble    — splice preamble.bzl (existing behaviour)
#   wrap:mixed  — use wrap_mixed instead of the classify_wrap cascade
#   env:a,b,c   — splice these env.bzl ambient bindings
# Any other token (`chained:1`, a skip rationale, free prose) is provenance
# only. Exact-token matching keeps prose that merely CONTAINS a keyword — e.g.
# `name-collides-with-preamble (Shape/…)` — from switching behaviour on.
# -----------------------------------------------------------------------

def note_tokens(notes):
    return {t.strip() for t in (notes or "").split(";") if t.strip()}


# -----------------------------------------------------------------------
# Preamble splice — for notes=preamble cases. Mirrors the awk splice.
# -----------------------------------------------------------------------

_SPLICE_MODULE_LINE_RE = re.compile(r'^[ \t]*module[ \t]')


_SPLICE_IMPORT_LINE_RE = re.compile(r'^[ \t]*import[ \t]')


def splice_preamble(wrapped_src, preamble_text):
    """Insert preamble_text after the first `module ...` line AND any imports
    that immediately follow it (SourceFile ::= ModuleDecl Import* TopDecl*,
    spec Appendix A.1 — a declaration spliced between the module line and an
    import would make the import ill-placed). Derived from the awk splice in
    run_conformance.sh for `notes=preamble` cases; the import hop was added at
    the v0.247 regeneration. awk's `getline` + `print` line-terminates every
    preamble line, so a missing trailing newline on the preamble file is
    supplied here."""
    pad = preamble_text if preamble_text.endswith("\n") else preamble_text + "\n"
    lines = wrapped_src.splitlines(keepends=True)
    out = []
    i = 0
    spliced = False
    while i < len(lines):
        out.append(lines[i])
        if not spliced and _SPLICE_MODULE_LINE_RE.match(lines[i]):
            # hop over the contiguous import block (blank lines included)
            j = i + 1
            while j < len(lines) and (
                    _SPLICE_IMPORT_LINE_RE.match(lines[j])
                    or not lines[j].strip()):
                out.append(lines[j])
                j += 1
            out.append(pad)
            spliced = True
            i = j
            continue
        i += 1
    return "".join(out)


# -----------------------------------------------------------------------
# Crash classification + run-expectation evaluation — PURE.
# -----------------------------------------------------------------------

def is_crash(rc):
    """per run_conformance.sh: SIGSEGV/SIGABRT/etc. surface as 128+signal
    (>= 128) or the core-dump codes 139 (SIGSEGV) / 134 (SIGABRT) explicitly.
    Exit 70 is the runtime unhandled-exception code and is NOT a crash."""
    return rc >= 128 or rc in (134, 139)


def _first_line(text):
    """`cat | tr -d '\\r' | head -1`: first line of text with all CR stripped."""
    no_cr = text.replace("\r", "")
    nl = no_cr.find("\n")
    return no_cr if nl < 0 else no_cr[:nl]


def _snippet(log, cut_chars, head_bytes):
    """`head -c N | tr '\\n' ' ' | cut -c1-M`: a flat one-line preview of the
    compile log. ASCII diagnostics only — byte vs char slicing coincides."""
    flat = log[:head_bytes].replace("\n", " ")
    return flat[:cut_chars]


def evaluate_run(expectation, run_rc, run_stdout):
    """Evaluate a stmt/program case's RUN phase against its expectation. Pure.
    Returns (passed, kind, detail): passed is True/False; for a failure `kind`
    is the bracketed category and `detail` the text after it; for a pass both
    are empty. Mirrors run_conformance.sh's run-phase decision tree."""
    crash = is_crash(run_rc)
    if expectation == "compiles":
        if crash:
            return False, "crash/signal", "exit code {}".format(run_rc)
        return True, "", ""
    if expectation.startswith("run:"):
        expected = expectation[len("run:"):]
        actual = _first_line(run_stdout)
        if actual == expected:
            return True, "", ""
        return False, "wrong-result", "expected='{}' got='{}'".format(expected, actual)
    m = re.match(r'^rc:(\d+)$', expectation)
    if m:
        expected_rc = int(m.group(1))
        if run_rc == expected_rc:
            return True, "", ""
        return False, "wrong-result", "expected exit {} got {}".format(expected_rc, run_rc)
    # Unknown expectation — pass iff it ran without crashing.
    if crash:
        return False, "crash/signal", "exit code {}".format(run_rc)
    return True, "", ""


# -----------------------------------------------------------------------
# Manifest reading — faithful TSV split (no quote processing, like bash read).
# -----------------------------------------------------------------------

def read_manifest(path):
    """Read manifest.tsv into a list of case dicts. Splits on TAB with no quote
    handling (mirrors `IFS=$'\\t' read -r ...`); skips the `case-id` header row
    and blank lines. Fields shorter than 6 are padded so `notes` is always
    present (empty when the manifest omits it)."""
    rows = []
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n").rstrip("\r")
            if line == "":
                continue
            fields = line.split("\t")
            if fields[0] == "case-id":
                continue
            while len(fields) < 6:
                fields.append("")
            rows.append({
                "case_id": fields[0], "doc": fields[1], "section": fields[2],
                "cls": fields[3], "expectation": fields[4], "notes": fields[5],
            })
    return rows


# -----------------------------------------------------------------------
# The caged subprocess seam.
# -----------------------------------------------------------------------

def caged_run(cmd, cwd=None):
    """Run cmd under the systemd-run MemoryMax/swap=0 cage + `timeout`, merging
    stderr into stdout. Returns (rc, combined_text). timeout's exit is 124.
    This is the ONLY non-pure seam in a case run; run_case accepts it via the
    `run` parameter so tests inject a fake and never touch bezelc/systemd."""
    # stdin is /dev/null, not inherited: a case program that reads console
    # input must see end-of-input, or its verdict becomes a property of the
    # machine the gate runs on (a TTY blocks to the shard ceiling; a pipe
    # feeds it data the case never asked for).
    proc = subprocess.run(_CAGE_ARGV + list(cmd), cwd=cwd,
                          stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    return proc.returncode, text


# -----------------------------------------------------------------------
# Run one case. `run` is injectable (production: caged_run; tests: a fake).
# Returns (status, kind, detail) — see evaluate_run for the shape.
# -----------------------------------------------------------------------

def run_case(case, bezelc, cases_dir, tmpdir, preamble_text, run=caged_run,
             env_entries=None):
    case_id = case["case_id"]
    src_path = os.path.join(cases_dir, case_id + ".bzl")
    if not os.path.isfile(src_path):
        return "FAIL", "compile-error", "source file missing"

    with open(src_path, encoding="utf-8") as f:
        code = f.read()

    tokens = note_tokens(case.get("notes"))
    if "wrap:mixed" in tokens:
        wrap_mode, wrapped = wrap_mixed(code)
    else:
        wrap_mode, wrapped = classify_wrap(code)
    if "preamble" in tokens and preamble_text is not None:
        wrapped = splice_preamble(wrapped, preamble_text)
    names = env_names(case.get("notes"))
    if names:
        try:
            wrapped = splice_env(wrapped, names, env_entries or {})
        except EnvError as e:
            return "FAIL", "env-error", str(e)

    wrap_file = os.path.join(tmpdir, case_id + "_wrapped.bzl")
    with open(wrap_file, "w", encoding="utf-8") as f:
        f.write(wrapped)

    out_exe = os.path.join(tmpdir, case_id + "_out")
    flags = ["-c", "-o", out_exe] if wrap_mode in (WRAP_LIB, WRAP_AS_IS_LIB) \
        else ["-o", out_exe]

    compile_rc, compile_log = run([bezelc] + flags + [wrap_file])

    # timeout check FIRST (before the error-class branch), as in the bash.
    if compile_rc == 124:
        return "FAIL", "timeout", "compile timed out"

    if case["cls"] == "error":
        if compile_rc != 0:
            return "PASS", "", ""
        snippet = _snippet(compile_log, 80, 120)
        return "FAIL", "missing-diagnostic", "compiled without error: " + snippet

    if compile_rc != 0:
        snippet = _snippet(compile_log, 120, 200)
        return "FAIL", "compile-error", snippet

    if wrap_mode in (WRAP_LIB, WRAP_AS_IS_LIB):
        # Library mode: compile success is the whole check. (The bash prints a
        # distinct "[lib-only; no run check]" suffix when the expectation was
        # neither compiles nor n/a; reproduce it for output parity.)
        if case["expectation"] in ("compiles", "n/a"):
            return "PASS", "", ""
        return "PASS", "", "[lib-only; no run check]"

    if not (os.path.exists(out_exe) and os.access(out_exe, os.X_OK)):
        return "PASS", "", "[compiled; no runnable artifact]"

    run_rc, run_log = run([out_exe])
    if run_rc == 124:
        return "FAIL", "timeout", "run timed out"

    passed, kind, detail = evaluate_run(case["expectation"], run_rc, run_log)
    if passed:
        return "PASS", "", ""
    return "FAIL", kind, detail


def _format_line(status, case_id, kind, detail):
    """The per-case print line, matching run_conformance.sh exactly."""
    if status == "PASS":
        return "PASS " + case_id + ((" " + detail) if detail else "")
    return "FAIL " + case_id + " [" + kind + "] " + detail


# -----------------------------------------------------------------------
# CLI main. exit 1 on FAIL>0 AND on PASS==0 (the Plan-A 5.1 silent-green guard).
# -----------------------------------------------------------------------

def _here_dir():
    return os.path.dirname(os.path.abspath(__file__))


def main(argv, base_dir=None, run=caged_run):
    """Run the conformance sweep and print the per-case + summary report.

    `base_dir` (default: this module's directory) locates manifest.tsv, cases/,
    and preamble.bzl; `run` (default: caged_run) is the subprocess seam run_case
    uses. Both are injectable so test_conformance.py can drive a synthetic
    manifest under a fake `run` without touching bezelc/systemd — that is how
    the Plan-A 5.1 PASS>0 silent-green guard is unit-pinned.

    Returns 0 iff every non-skip case passed AND at least one passed; 1
    otherwise (FAIL>0 OR PASS==0)."""
    if len(argv) < 1 or argv[0] in ("-h", "--help"):
        print("Usage: run_conformance.py <bezelc-binary>", file=sys.stderr)
        return 1
    bezelc = argv[0]
    if not os.access(bezelc, os.X_OK):
        print("ERROR: bezelc binary not found or not executable: " + bezelc,
              file=sys.stderr)
        return 1

    here = base_dir or _here_dir()
    cases_dir = os.path.join(here, "cases")
    manifest = os.path.join(here, "manifest.tsv")
    preamble_file = os.path.join(here, "preamble.bzl")
    preamble_text = None
    if os.path.isfile(preamble_file):
        with open(preamble_file, encoding="utf-8") as f:
            preamble_text = f.read()
    env_file = os.path.join(here, "env.bzl")
    env_entries = {}
    if os.path.isfile(env_file):
        with open(env_file, encoding="utf-8") as f:
            env_entries = parse_env(f.read())

    if not os.path.isfile(manifest):
        print("ERROR: manifest not found: " + manifest, file=sys.stderr)
        return 1

    scratch = "/var/tmp/bezel-scratch"
    try:
        os.makedirs(scratch, exist_ok=True)
    except OSError:
        scratch = None  # fall back to the system temp dir below
    tmpdir = tempfile.mkdtemp(
        prefix="conf_run.", dir=(scratch if scratch and os.path.isdir(scratch)
                                 else None))

    counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    fail_kinds = {}

    print("=== Conformance sweep ===")
    print("bezelc: " + bezelc)
    print("")

    try:
        for case in read_manifest(manifest):
            if case["cls"] == "skip":
                counts["SKIP"] += 1
                continue
            status, kind, detail = run_case(
                case, bezelc, cases_dir, tmpdir, preamble_text, run=run,
                env_entries=env_entries)
            counts[status] += 1
            if status == "FAIL":
                fail_kinds[kind] = fail_kinds.get(kind, 0) + 1
            print(_format_line(status, case["case_id"], kind, detail))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("")
    print("=== Results ===")
    print("PASS:    {}".format(counts["PASS"]))
    print("FAIL:    {}".format(counts["FAIL"]))
    print("SKIP:    {}".format(counts["SKIP"]))
    print("")
    print("Failure breakdown:")
    # Mirror the bash's fixed ordering of failure categories.
    for kind in ("compile-error", "wrong-result", "crash/signal",
                 "missing-diagnostic", "timeout", "env-error"):
        print("  {:20} {}".format(kind + ":", fail_kinds.get(kind, 0)))

    if counts["FAIL"] > 0:
        return 1
    # per Plan-A 5.1: a green over a run that validated NOTHING is not green.
    # If the manifest loop read no passable case (manifest moved/empty, or every
    # case `skip`), FAIL LOUD rather than exit 0 over PASS=0/FAIL=0.
    if counts["PASS"] == 0:
        print("## CONFORMANCE FAILURE: PASS=0 — no case passed. The manifest is "
              "empty, missing, or every case is 'skip'; a green over a run that "
              "validated nothing is NOT green (Plan-A 5.1).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
