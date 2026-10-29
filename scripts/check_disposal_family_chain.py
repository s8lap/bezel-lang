#!/usr/bin/env python3
"""check_disposal_family_chain.py — ratchet: the built-in disposal-family chain
table (`kBuiltinDisposalFamilyHeads`, src/common/BuiltinRegistry.cpp) against
the STANDARD LIBRARY DOCUMENT.

THE RESIDUAL THIS CLOSES. #7424's own final paragraph recorded it: "a FUTURE
standard-library interface extending `IDisposable` would silently miss again —
which is why the MECHANICAL DERIVATION is the remedy, not a two-row patch".
The table was still HAND-WRITTEN when the residual was handed to this lane:
ten (head, parent) rows, transcribed by hand from the stdlib document, with
no mechanism that would notice a new stdlib interface extending `IDisposable`.

WHAT THIS CHECKER DOES. It parses `docs/Bezel_Standard_Library.md` and DERIVES
the disposal family from the document itself:

  * every `public interface` declaration in the document is read, with its
    extension clause (its bases), its `where` constraints (which are NOT
    extension clauses and are discarded), and the § it is declared under;
  * the family is the TRANSITIVE CLOSURE of `IDisposable` over the extension
    graph — the interfaces that extend `IDisposable` "directly or transitively"
    (stdlib §2.10);
  * each member's CHAIN PARENT is the base it extends that is ITSELF in the
    family — the nearest ancestor the §28.12 diagnostic walks
    ("`X` extends `parent`, which extends `IDisposable`").

It then compares that derivation against the source, in three independent
places, and FAILS CLOSED on any disagreement:

  1. the CODE TABLE `kBuiltinDisposalFamilyHeads` — the machine-readable
     (head, parent) pairs, plus the `std::array<..., N>` size;
  2. the CODE TABLE's declared SIZE — a family of N members must be declared
     with N, so a table that gained a row without gaining a size (or the
     reverse) is caught;
  3. the SOURCE COMMENT BLOCK above the table — the hand-written list of the
     family with the § each member is declared under. That block is a SECOND
     hand-written transcription of the same fact, and it is the one a reader
     consults, so it is checked against the document too.

This is a BUILD-FREE SOURCE ratchet: it reads the document and the source as
text. It does not compile anything.

EXIT CODES:
  0  the document and the source agree
  1  they disagree (a member is missing, extra, or has a different parent;
     a § attribution is wrong)
  2  the checker could not parse its inputs (FAIL CLOSED — never a green).
     That includes a declaration the parser cannot see: the number of
     `interface` declarations a LOOSE detector counts must equal the number
     the parser read (#7504); a declaration header is joined across its
     continuation lines (Appendix A.3's `Inheritance` is not line-bounded).

Usage: scripts/check_disposal_family_chain.py [--strict] [--self-test]

  --strict    the mode the gate runs (the verdict is the same either way;
              the flag exists so the gate's invocation shape is uniform).
  --self-test prove the checker FIRES: run the comparison against mutated
              copies of the two inputs in which a member is removed, a parent
              is changed, a row is deleted from the comment block, and a §
              attribution is changed — and require every mutation to be
              detected. A checker that cannot detect a mismatch proves
              nothing when it reports a match.
"""

import contextlib
import io
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STDLIB = os.path.join(REPO, "docs", "Bezel_Standard_Library.md")
SRC = os.path.join(REPO, "src", "common", "BuiltinRegistry.cpp")
HPP = os.path.join(REPO, "src", "common", "BuiltinRegistry.hpp")

ROOT = "IDisposable"

# The declaration: `public interface Name<params>: Base, Base`. The generic
# parameter list may contain a variance (`out T`) and commas, so it is
# matched as a balanced-ish `<...>` chunk rather than word by word.
IFACE_RE = re.compile(
    r"^public interface\s+([A-Za-z_][A-Za-z0-9_]*)\s*(<[^<>]*>)?\s*(.*)$"
)

# A `where` continuation line of a declaration (stdlib §2.19 spells two of
# them on the lines below the declaration).
WHERE_CONT_RE = re.compile(r"^\s+where\b")

# A numbered stdlib section heading — `### 4.2 Owning collection interfaces`.
SECTION_RE = re.compile(r"^#{3,4}\s+(\d+\.\d+)\b")

# The code table and its declared size.
TABLE_RE = re.compile(
    r"std::array<BuiltinDisposalFamilyHead,\s*(\d+)>\s*"
    r"kBuiltinDisposalFamilyHeads\s*=\s*\{\{(.*?)\}\}\s*;",
    re.S,
)
ROW_RE = re.compile(r"\{\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*\}")

# The comment block's table header — the rows BELOW it are the family list.
CMT_HEADER_RE = re.compile(r"^\s*//\s+interface\s+extends\s+stdlib\s*$")

# A comment line carrying a trailing § attribution.
CMT_ROW_RE = re.compile(r"^\s*//(?P<body>.*?)\s+§(?P<sec>\d+(?:\.\d+)*)\s*$")

# The header's family rows (#7496): `//   Name<..>   (root)   §2.10` or
# `//   Name<..>   extends Parent<..>   §4.2`, three spaces of indent.
HPP_ROW_RE = re.compile(
    r"^//   (?P<name>[A-Za-z_][A-Za-z0-9_]*(?:<[^<>]*>)?)\s+"
    r"(?:\(root\)|extends\s+(?P<parent>[A-Za-z_][A-Za-z0-9_]*(?:<[^<>]*>)?))"
    r"\s+§(?P<sec>\d+(?:\.\d+)*)\s*$"
)
HPP_ANCHOR = "The family, and where each member is declared"

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def die(msg, code=2):
    sys.stderr.write("check_disposal_family_chain: %s\n" % msg)
    sys.exit(code)


def read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        die("cannot read %s: %s" % (path, exc))


def split_top_level(text, sep=","):
    """Split `text` on `sep`, ignoring separators nested inside `<...>`."""
    out, depth, cur = [], 0, []
    for ch in text:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        if ch == sep and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


# A LOOSE declaration detector, independent of IFACE_RE: any line that opens
# with optional modifiers and `interface Name`. It exists only to CROSS-CHECK
# the parser's count (#7504) — a declaration the strict parser did not see is a
# FAIL-CLOSED error, never a silent skip.
LOOSE_IFACE_RE = re.compile(
    r"^\s*(?:(?:public|private|internal|sealed|abstract)\s+)*interface"
    r"(?:\s+[A-Za-z_][A-Za-z0-9_]*\s*(?:<|:|where\b|$)|\s*$)"
)


def _unbalanced(text):
    return text.count("<") > text.count(">")


def header_continues(header, nxt):
    """Does the declaration header `header` continue on the line `nxt`?

    Spec Appendix A.3: `Inheritance ::= ":" InheritItem ("," InheritItem)*
    ","?` is NOT line-bounded, so the extension clause may start on a
    continuation line (`:` first on the line), the base list may be split
    after a `,`, and a generic parameter list may span lines (#7504).
    """
    h = header.rstrip()
    s = nxt.strip()
    if not s:
        return False
    if _unbalanced(h):
        return True
    if h.endswith(",") or h.endswith(":"):
        return True
    if s.startswith(":"):
        return True
    return False


def parse_declarations(text):
    """Every `public interface` declaration in the document.

    Returns a list of dicts: name (bare), spelling (as declared), bases
    (bare names of the EXTENSION CLAUSE), section (the § it is declared
    under), line (1-based). A declaration HEADER is joined across its
    continuation lines before it is parsed (#7504), and the number of
    declarations found is cross-checked against a looser detector so a shape
    the parser cannot see FAILS CLOSED instead of being skipped.
    """
    lines = text.splitlines()
    decls = []
    section = None
    i = 0
    while i < len(lines):
        line = lines[i]
        m = SECTION_RE.match(line)
        if m:
            section = m.group(1)
        dm = IFACE_RE.match(line)
        if not dm:
            i += 1
            continue
        j = i + 1
        # Rebuild the header across its continuation lines. The first line is
        # re-matched after joining so a generic list split over lines works.
        joined = line
        while j < len(lines) and header_continues(joined, lines[j]):
            joined = joined.rstrip() + " " + lines[j].strip()
            j += 1
        dm = IFACE_RE.match(joined)
        name = dm.group(1)
        params = dm.group(2) or ""
        rest = dm.group(3)
        spelling = name + params
        # A `where` clause may continue on the following lines; it is a
        # CONSTRAINT, not an extension clause, so it is dropped — but it must
        # be consumed so that a `where T: X` line is never mistaken for one.
        while j < len(lines) and WHERE_CONT_RE.match(lines[j]):
            j += 1
        header = rest
        wm = re.search(r"\bwhere\b", header)
        if wm:
            header = header[:wm.start()]
        header = header.strip()
        bases = []
        if header.startswith(":"):
            for piece in split_top_level(header[1:]):
                piece = piece.strip()
                if not piece:
                    continue
                bm = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(<[^<>]*>)?$", piece)
                if not bm:
                    die("unparsed base %r in the extension clause of `%s` "
                        "(stdlib line %d) — FAIL CLOSED" % (piece, name, i + 1))
                bases.append(bm.group(1))
        elif header:
            die("unparsed extension clause of `%s` (stdlib line %d): %r — "
                "FAIL CLOSED" % (name, i + 1, header))
        decls.append({
            "name": name,
            "spelling": spelling,
            "bases": bases,
            "section": section,
            "line": i + 1,
        })
        i = j
    loose = sum(1 for ln in lines if LOOSE_IFACE_RE.match(ln))
    if loose != len(decls):
        die("the loose interface detector sees %d declarations but the "
            "parser read %d — a declaration shape the parser cannot see "
            "(stdlib document) — FAIL CLOSED" % (loose, len(decls)))
    return decls


def derive_family(decls):
    """The disposal family, derived from the document.

    Returns (family, sections, spellings) where `family` maps a bare member
    name to its CHAIN PARENT (the base it extends that is itself in the
    family; "" for the root `IDisposable`).
    """
    by_name = {}
    for d in decls:
        if d["name"] in by_name:
            die("`%s` is declared more than once in the stdlib document — "
                "FAIL CLOSED" % d["name"])
        by_name[d["name"]] = d
    if ROOT not in by_name:
        die("the stdlib document declares no `%s` — FAIL CLOSED" % ROOT)

    family = {ROOT: ""}
    changed = True
    while changed:
        changed = False
        for d in decls:
            if d["name"] in family:
                continue
            for b in d["bases"]:
                if b in family:
                    family[d["name"]] = b
                    changed = True
                    break
    # The chain parent of each member is the base it extends that is ITSELF a
    # family member — the nearest ancestor the §28.12 diagnostic walks.
    parents = {}
    for d in decls:
        if d["name"] not in family:
            continue
        if d["name"] == ROOT:
            parents[d["name"]] = ""
            continue
        ins = [b for b in d["bases"] if b in family]
        if not ins:
            die("`%s` is in the disposal family but extends no family member — "
                "FAIL CLOSED" % d["name"])
        if len(ins) > 1:
            die("`%s` extends %d family members (%s) — the chain table can "
                "hold only one parent, so this needs a DECISION — FAIL CLOSED"
                % (d["name"], len(ins), ", ".join(ins)))
        parents[d["name"]] = ins[0]
    # The chain must terminate at the root.
    for name in parents:
        seen, cur = set(), name
        while cur:
            if cur in seen:
                die("the disposal chain through `%s` is cyclic — FAIL CLOSED"
                    % name)
            seen.add(cur)
            cur = parents.get(cur, "")
    sections = {d["name"]: d["section"] for d in decls}
    spellings = {d["name"]: d["spelling"] for d in decls}
    return parents, sections, spellings


def parse_code_table(text):
    """The code table's (head, parent) pairs and its declared size."""
    m = TABLE_RE.search(text)
    if not m:
        die("no `kBuiltinDisposalFamilyHeads` table found in "
            "src/common/BuiltinRegistry.cpp — FAIL CLOSED")
    size = int(m.group(1))
    rows = ROW_RE.findall(m.group(2))
    if not rows:
        die("`kBuiltinDisposalFamilyHeads` has no rows — FAIL CLOSED")
    table = {}
    for head, parent in rows:
        if head in table:
            die("`%s` appears twice in `kBuiltinDisposalFamilyHeads` — "
                "FAIL CLOSED" % head)
        table[head] = parent
    return table, size


def comment_block(text):
    """The contiguous comment block directly above the code table."""
    lines = text.splitlines()
    idx = None
    for i, line in enumerate(lines):
        if "kBuiltinDisposalFamilyHeads" in line and "=" in line:
            idx = i
            break
    if idx is None:
        die("no `kBuiltinDisposalFamilyHeads` declaration found in "
            "src/common/BuiltinRegistry.cpp — FAIL CLOSED")
    start = idx
    while start > 0 and re.match(r"^\s*//", lines[start - 1]):
        start -= 1
    return lines[start:idx]


def comment_rows(text):
    """The comment block's family rows: (body, section) for each §-row."""
    block = comment_block(text)
    header = None
    for i, line in enumerate(block):
        if CMT_HEADER_RE.match(line):
            header = i
            break
    if header is None:
        die("no family table header found in the comment block above "
            "`kBuiltinDisposalFamilyHeads` — FAIL CLOSED")
    rows = []
    for line in block[header + 1:]:
        m = CMT_ROW_RE.match(line)
        if not m:
            continue
        rows.append((m.group("body"), m.group("sec")))
    if not rows:
        die("the comment block above `kBuiltinDisposalFamilyHeads` lists no "
            "family rows — FAIL CLOSED")
    return rows


def mention_index(body, spelling):
    """The index of the first WHOLE-name mention of `spelling` in `body`.

    -1 when there is none. "Whole name" means the characters on either side
    are not identifier characters, so `IDisposable` does not match inside
    `IAsyncDisposable`.
    """
    start = 0
    while True:
        k = body.find(spelling, start)
        if k < 0:
            return -1
        before = body[k - 1] if k > 0 else " "
        after = body[k + len(spelling):k + len(spelling) + 1] or " "
        if not (before.isalnum() or before == "_") and \
           not (after.isalnum() or after == "_"):
            return k
        start = k + 1


def row_owner(body, spellings):
    """The member a comment row is ABOUT: its FIRST named member.

    A row's own name is its first column; the second column names the
    member's PARENT, which is a family member too and would otherwise be
    attributed to the row. The leftmost mention wins.
    """
    best, best_at = None, None
    for name, spelling in spellings.items():
        k = mention_index(body, spelling)
        if k < 0:
            continue
        if best_at is None or k < best_at:
            best, best_at = name, k
    return best


def header_rows(text):
    """The header comment's family rows: (name, parent-or-None, section).

    The block in src/common/BuiltinRegistry.hpp is a SECOND hand-written
    transcription of the family, in a different style from the .cpp block
    (#7496). It starts at the anchor sentence and is the run of `//` lines
    that follows; rows are the ones HPP_ROW_RE reads.
    """
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if HPP_ANCHOR in line:
            start = i
            break
    if start is None:
        die("no `%s` anchor in src/common/BuiltinRegistry.hpp — FAIL CLOSED"
            % HPP_ANCHOR)
    rows = []
    for line in lines[start + 1:]:
        if not line.startswith("//"):
            break
        m = HPP_ROW_RE.match(line)
        if m:
            rows.append((m.group("name"), m.group("parent"), m.group("sec")))
    if not rows:
        die("the family block in src/common/BuiltinRegistry.hpp lists no "
            "rows — FAIL CLOSED")
    return rows


def compare_header(spellings, parents, sections, hpp_text):
    """Compare the .hpp family block against the derived family."""
    problems = []
    seen = {}
    for name, parent, sec in header_rows(hpp_text):
        bare = IDENT_RE.match(name).group(0)
        if bare in seen:
            problems.append("the BuiltinRegistry.hpp family block lists `%s` "
                            "twice" % bare)
        seen[bare] = (name, parent, sec)
    for bare in sorted(parents):
        if bare not in seen:
            problems.append(
                "`%s` is in the disposal family but is MISSING from the "
                "family block in src/common/BuiltinRegistry.hpp"
                % spellings[bare])
            continue
        name, parent, sec = seen[bare]
        if name != spellings[bare]:
            problems.append(
                "the BuiltinRegistry.hpp family block spells `%s` as `%s`"
                % (spellings[bare], name))
        want_parent = parents[bare]
        got_parent = IDENT_RE.match(parent).group(0) if parent else ""
        if got_parent != want_parent:
            problems.append(
                "the BuiltinRegistry.hpp family block gives `%s` the parent "
                "`%s` but the stdlib document says `%s`"
                % (spellings[bare], got_parent or "(root)",
                   want_parent or "(root)"))
        if sec != sections[bare]:
            problems.append(
                "the BuiltinRegistry.hpp family block attributes `%s` to §%s "
                "but the stdlib document declares it in §%s"
                % (spellings[bare], sec, sections[bare]))
    for bare in sorted(set(seen) - set(parents)):
        problems.append(
            "the BuiltinRegistry.hpp family block lists `%s`, which does not "
            "extend `IDisposable` in the stdlib document" % bare)
    return problems


def compare(doc_text, src_text, hpp_text=None):
    """The whole comparison. Returns a list of problem strings (empty = clean)."""
    problems = []
    decls = parse_declarations(doc_text)
    if len(decls) < 20:
        die("only %d `public interface` declarations parsed from the stdlib "
            "document — the parser has probably broken — FAIL CLOSED"
            % len(decls))
    parents, sections, spellings = derive_family(decls)
    table, size = parse_code_table(src_text)

    # 1 + 2: the code table must BE the derived family.
    derived = set(parents)
    listed = set(table)
    for missing in sorted(derived - listed):
        problems.append(
            "`%s` extends `IDisposable` in the stdlib document "
            "(stdlib §%s) but is NOT in `kBuiltinDisposalFamilyHeads` — a "
            "record/enum/choice listing it would be ADMITTED although §28.12 "
            "forbids it" % (spellings[missing], sections[missing]))
    for extra in sorted(listed - derived):
        problems.append(
            "`%s` is in `kBuiltinDisposalFamilyHeads` but the stdlib document "
            "does not extend `IDisposable` — the table over-claims" % extra)
    for name in sorted(derived & listed):
        if table[name] != parents[name]:
            problems.append(
                "`%s`'s chain parent is `%s` in the stdlib document but "
                "`%s` in `kBuiltinDisposalFamilyHeads`"
                % (name, parents[name] or "(root)", table[name] or "(root)"))
    if size != len(table):
        problems.append(
            "`kBuiltinDisposalFamilyHeads` declares %d entries but carries %d "
            "rows" % (size, len(table)))
    if size != len(derived):
        problems.append(
            "`kBuiltinDisposalFamilyHeads` declares %d entries but the stdlib "
            "document's disposal family has %d members"
            % (size, len(derived)))

    # 3: the comment block is a second hand-written transcription.
    rows = comment_rows(src_text)
    observed = {}
    for body, sec in rows:
        owner = row_owner(body, {n: spellings[n] for n in derived})
        if owner is None:
            continue
        observed.setdefault(owner, set()).add(sec)
    for name in sorted(derived):
        if name not in observed:
            problems.append(
                "`%s` is in the disposal family but is MISSING from the "
                "comment block above `kBuiltinDisposalFamilyHeads`"
                % spellings[name])
            continue
        if observed[name] != {sections[name]}:
            problems.append(
                "the comment block attributes `%s` to §%s but the stdlib "
                "document declares it in §%s"
                % (spellings[name], "/".join(sorted(observed[name])),
                   sections[name]))
    # 4: the header's block is a third hand-written transcription (#7496).
    if hpp_text is not None:
        problems.extend(compare_header(spellings, parents, sections,
                                       hpp_text))
    return problems


def self_test():
    """Prove the checker FIRES on mutated inputs."""
    doc = read(STDLIB)
    src = read(SRC)
    hpp = read(HPP)
    problems = compare(doc, src, hpp)
    if problems:
        die("SELF-TEST FAILED: the real document and the real source already "
            "disagree:\n  " + "\n  ".join(problems))
    decls = parse_declarations(doc)
    parents, sections, spellings = derive_family(decls)
    victim = sorted(n for n in parents if n != ROOT)[0]

    # (a) a member removed from the code table.
    m = TABLE_RE.search(src)
    victim_row = '{"%s", "%s"}' % (victim, parents[victim])
    body = m.group(2).replace(victim_row, "")
    dropped = src[:m.start(2)] + body + src[m.end(2):]
    if dropped == src:
        die("SELF-TEST FAILED: could not delete the code-table row for "
            "`%s`" % victim)
    if not compare(doc, dropped):
        die("SELF-TEST FAILED: a code table missing `%s` was NOT detected"
            % victim)
    # (b) a parent changed.
    wrong = src.replace('{"%s", "%s"}' % (victim, parents[victim]),
                        '{"%s", "%s"}' % (victim, "IDisposable"
                                          if parents[victim] != "IDisposable"
                                          else "IAsyncDisposable"), 1)
    if wrong == src:
        wrong = src.replace('{"%s", "%s"}' % (victim, parents[victim]),
                            '{"%s", ""}' % victim, 1)
    if not compare(doc, wrong):
        die("SELF-TEST FAILED: a changed chain parent for `%s` was NOT "
            "detected" % victim)
    # (c) a row deleted from the comment block.
    sp = spellings[victim]
    nodoc = re.sub(r"^\s*//.*%s.*§[\d.]+\s*$\n" % re.escape(sp), "", src,
                   count=1, flags=re.M)
    if nodoc == src:
        die("SELF-TEST FAILED: could not delete the comment-block row for "
            "`%s`" % sp)
    if not compare(doc, nodoc):
        die("SELF-TEST FAILED: a comment block missing `%s` was NOT detected"
            % sp)
    # (d) a § attribution changed (the member's OWN row only).
    sec = sections[victim]
    other = "4.2" if sec != "4.2" else "4.13"
    moved_lines = []
    hit = 0
    for line in src.splitlines(True):
        stripped = line.lstrip()
        if (hit == 0 and stripped.startswith("//")
                and mention_index(stripped, sp) >= 0
                and stripped.rstrip().endswith("§" + sec)):
            line = line.rstrip("\n").rstrip()[:-len("§" + sec)] + "§" + other + "\n"
            hit = 1
        moved_lines.append(line)
    moved = "".join(moved_lines)
    if hit != 1:
        die("SELF-TEST FAILED: could not change the § attribution of `%s`"
            % sp)
    if not compare(doc, moved):
        die("SELF-TEST FAILED: a wrong § attribution for `%s` was NOT "
            "detected" % sp)
    # (e) PARSER FIXTURES (#7504): legal Bezel spellings of a declaration
    # header that a line-bounded parser MISSES. Each fixture is appended to
    # the real document as a NEW interface extending `IDisposable`; the
    # comparison must report it as absent from the code table. A parser that
    # skips the shape reports agreement — the exact fail-open #7504 measured.
    fixtures = {
        "G1 extension clause on a continuation line":
            "public interface IFakeProbe\n  : IDisposable\n"
            "  method Dispose();\nend IFakeProbe;\n",
        "G4 base list split after a comma":
            "public interface IFakeProbe: IReadOnlyCollection<int>,\n"
            "    IDisposable\n  method Dispose();\nend IFakeProbe;\n",
        "G5 trailing comma then a continuation":
            "public interface IFakeProbe: IDisposable,\n    IEquatable<int>\n"
            "  method Dispose();\nend IFakeProbe;\n",
        "G6 generic parameter list split over two lines":
            "public interface IFakeProbe<out T,\n    U>: IDisposable\n"
            "  method Dispose();\nend IFakeProbe;\n",
        "G7 single line (control)":
            "public interface IFakeProbe: IDisposable\n"
            "  method Dispose();\nend IFakeProbe;\n",
    }
    base_n = len(decls)
    for label, snippet in fixtures.items():
        mutated = doc.rstrip("\n") + "\n\n" + snippet
        try:
            got = parse_declarations(mutated)
        except SystemExit:
            die("SELF-TEST FAILED: fixture %s made the parser die instead of "
                "parsing it" % label)
        if len(got) != base_n + 1:
            die("SELF-TEST FAILED: fixture %s parsed %d declarations, "
                "expected %d" % (label, len(got), base_n + 1))
        try:
            found = compare(mutated, src)
        except SystemExit:
            die("SELF-TEST FAILED: fixture %s made the comparison die" % label)
        if not any("IFakeProbe" in p for p in found):
            die("SELF-TEST FAILED: fixture %s (a new interface extending "
                "`IDisposable`) was NOT detected" % label)
    # (f) a header shape the parser cannot read must FAIL CLOSED (rc=2), not
    # be skipped: the name on one line, the whole declaration unreadable.
    blind = doc.rstrip("\n") + "\n\npublic interface\n  IFakeProbe: IDisposable\n"
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            parse_declarations(blind)
    except SystemExit as exc:
        if exc.code != 2:
            die("SELF-TEST FAILED: an unreadable header exited %r, not 2"
                % (exc.code,))
    else:
        die("SELF-TEST FAILED: an interface whose header the parser cannot "
            "read was silently accepted")
    # (g) the .hpp family block (#7496): a row deleted, a parent changed and a
    # § attribution changed must each be detected.
    hrows = header_rows(hpp)
    hv = [r for r in hrows if r[1] is not None]
    name, parent, sec = hv[0]
    vline = [ln for ln in hpp.splitlines(True)
             if HPP_ROW_RE.match(ln.rstrip("\n")) and
             ln.startswith("//   " + name)][0]
    hmut = {
        "a deleted row": hpp.replace(vline, "", 1),
        "a changed parent": hpp.replace(vline, vline.replace(
            "extends " + parent, "extends IDisposableX"), 1),
        "a changed section": hpp.replace(vline, vline.replace(
            "§" + sec, "§9.9"), 1),
    }
    for label, mutated in hmut.items():
        if mutated == hpp:
            die("SELF-TEST FAILED: could not apply the .hpp mutation (%s)"
                % label)
        if not compare(doc, src, mutated):
            die("SELF-TEST FAILED: %s in the BuiltinRegistry.hpp family "
                "block was NOT detected" % label)
    print("check_disposal_family_chain --self-test: OK (the .hpp family block "
          "is compared too: %d rows read; a removed member, a "
          "changed parent, a deleted comment row and a wrong § attribution "
          "are each detected; %d declaration-header fixtures (continuation "
          "line, split base list, split generic list) are parsed and "
          "detected, and an unreadable header fails closed; the real "
          "document and the real source agree on %d family members out of "
          "%d parsed interfaces)" % (len(hrows), len(fixtures), len(parents),
                                     base_n))
    return 0


def main():
    if "--self-test" in sys.argv:
        return self_test()
    problems = compare(read(STDLIB), read(SRC), read(HPP))
    if problems:
        out = ["check_disposal_family_chain: MISMATCH between "
               "docs/Bezel_Standard_Library.md and "
               "src/common/BuiltinRegistry.cpp / .hpp"]
        out.extend("  " + p for p in problems)
        sys.stderr.write("\n".join(out) + "\n")
        return 1
    decls = parse_declarations(read(STDLIB))
    parents, sections, spellings = derive_family(decls)
    print("check_disposal_family_chain: clean (%d `public interface` "
          "declarations in the stdlib document; %d of them extend "
          "`IDisposable` directly or transitively and agree with "
          "`kBuiltinDisposalFamilyHeads` on every head and every chain "
          "parent; the %d-row family block in BuiltinRegistry.hpp agrees "
          "too)" % (len(decls), len(parents), len(header_rows(read(HPP)))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
