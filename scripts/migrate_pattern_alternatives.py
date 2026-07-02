#!/usr/bin/env python3
"""Migrate the Bezel corpus to the v0.167 `|`-spelled pattern alternatives
(spec §19.3 / Appendix A.7 AltPattern / A.6 CatchPattern, #1371).

Through v0.166 the comma did triple duty in pattern context — arm/slot
separator AND alternative separator (`1, 2 => "low"`, `catch A, B => …`,
`catch var e: A, B => …`) — disambiguated by an `=>` lookahead. v0.167 gives
alternatives their own spelling `|`; the comma is now *exclusively* the
arm/slot separator. This script rewrites the alternative-separating commas to
`|` across:

  (i)  embedded Bezel programs inside C++ string literals in tests/**/*.cpp
  (ii) Bezel source in tests/conformance/cases/*.bzl

Only the commas that separate the members of ONE arm's pattern (a case-arm
constant/enum alternatives list, or a catch arm's multi-type list) are
rewritten. Arm-separating commas, positional-destructuring commas (`Point(x,
y)`), collection-literal commas, and argument-list commas are all left alone.

Approach (shared logical-text machinery with migrate_case_of.py)
----------------------------------------------------------------
Build a "logical" Bezel text (for .cpp: the decoded contents of C++
string-literal groups; for .bzl: the whole file) with a map back to physical
offsets, blanking Bezel comments and string interiors so a `,`/`=>`/keyword
inside them is never matched. Then walk the logical text with a construct stack
and a paren/bracket depth counter:

  * A `case`/`when`(construct)/`try` opens a frame; `catch`/`finally` switch a
    `try` frame's mode; `do`(block)/`if`/`while`/`for`/`loop`/`lock`/`unsafe`
    open opaque frames; `end` pops. The first `do` after `for`/`while`/`loop`
    is the loop-header separator, not a block opener.
  * Inside a `case`/`when`/`catch`-mode frame we track whether we are in the
    arm's PATTERN region (before its `=>`) or BODY region (after). A depth-0
    comma in PATTERN region is an *alternative* separator → rewrite to `|`; a
    depth-0 comma in BODY region is the *arm* separator → keep, and the next
    arm's PATTERN region begins. A depth-0 `=>` switches PATTERN → BODY.
  * A `when` seen inside a case/catch PATTERN region is a *guard*, not a
    construct — it does not open a frame.

Because arm bodies (expressions, single statements, `do … end` blocks) never
contain a depth-0 comma of their own, this cleanly separates alternative commas
from arm commas without a full parse. Any mis-migration is caught downstream:
the migrated corpus is recompiled by the test suite.
"""

import sys
import os

# ---- logical-text builders (verbatim shape from migrate_case_of.py) --------


def build_logical_bzl(text):
    """Whole .bzl file is Bezel source. Blank comments and string interiors."""
    logical = []
    endmap = []  # endmap[i] = physical offset just past source of logical[i]
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j < 0:
                j = n
            for k in range(i, j):
                logical.append(" ")
                endmap.append(k + 1)
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                logical.append(" ")
                endmap.append(k + 1)
            i = j
            continue
        if c in ('"', "'"):
            quote = c
            logical.append(" ")
            endmap.append(i + 1)
            i += 1
            while i < n:
                d = text[i]
                if d == "\\" and i + 1 < n:
                    logical.append(" ")
                    endmap.append(i + 1)
                    logical.append(" ")
                    endmap.append(i + 2)
                    i += 2
                    continue
                logical.append(" ")
                endmap.append(i + 1)
                i += 1
                if d == quote:
                    break
            continue
        logical.append(c)
        endmap.append(i + 1)
        i += 1
    return "".join(logical), endmap


def build_logical_cpp(text):
    """Concatenate the decoded contents of C++ string-literal groups into a
    logical Bezel text, keeping a physical-offset map. Groups are separated by a
    newline. Bezel comments/string interiors within a group are blanked."""
    import re

    logical = []
    endmap = []
    i, n = 0, len(text)
    in_bezel_string = False
    bezel_string_quote = ""
    bezel_line_comment = False
    bezel_block_comment = False
    last_group_content_end = None
    gap_start = 0

    def emit(ch, phys_end):
        logical.append(ch)
        endmap.append(phys_end)

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        # C++ raw string literal `R"delim( ... )delim"` (optionally with an
        # encoding prefix `L`/`u`/`U`/`u8`). The body is verbatim Bezel with no
        # C++ escapes, so decode it with the .bzl logical builder (blanking Bezel
        # comments/string interiors) and splice its physical-offset map in.
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            k = i + 2
            while k < n and text[k] != "(":
                k += 1
            delim = text[i + 2:k]
            close = ")" + delim + '"'
            body_start = k + 1
            body_end = text.find(close, body_start)
            if body_end < 0:
                body_end = n
            if last_group_content_end is not None:
                emit("\n", i)  # a raw string is its own group
            sub_logical, sub_endmap = build_logical_bzl(text[body_start:body_end])
            for ch, rel_end in zip(sub_logical, sub_endmap):
                emit(ch, body_start + rel_end)
            i = body_end + len(close) if body_end < n else n
            last_group_content_end = i
            gap_start = i
            continue
        if c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == '"':
            if last_group_content_end is not None:
                gap = text[gap_start:i]
                if re.fullmatch(r"[\s]*", gap):
                    pass  # adjacent literals — one logical string, no hidden text
                elif re.fullmatch(
                    r"[\s]*(\+[\s]*[A-Za-z_][\w.]*(\([^()]*\))?[\s]*)*\+?[\s]*",
                    gap,
                ):
                    # A `"..." + variable + "..."` splice: the variable's Bezel
                    # text is hidden from us and commonly carries a full arm
                    # (`PATTERN => BODY`). Emit a SENTINEL so the comma classifier
                    # conservatively assumes a body ended here — otherwise the
                    # arm-separator comma after the splice would be misread as an
                    # alternative and wrongly rewritten. (Under-migration is safe:
                    # a stale comma fails the recompile and is fixed by hand.)
                    emit("\x01", i)
                else:
                    emit("\n", i)
            i += 1
            in_bezel_string = False
            bezel_line_comment = False
            bezel_block_comment = False
            while i < n:
                d = text[i]
                if d == "\\" and i + 1 < n:
                    esc = text[i + 1]
                    decoded = {
                        "n": "\n", "t": "\t", "r": "\r",
                        '"': '"', "\\": "\\", "'": "'", "0": "\0",
                    }.get(esc, esc)
                    phys_end = i + 2
                    if bezel_line_comment:
                        emit(" ", phys_end)
                        if decoded == "\n":
                            bezel_line_comment = False
                    elif bezel_block_comment:
                        emit(" ", phys_end)
                    elif in_bezel_string:
                        emit(" ", phys_end)
                        if decoded == bezel_string_quote:
                            in_bezel_string = False
                    else:
                        emit(decoded, phys_end)
                        if decoded in ('"', "'"):
                            in_bezel_string = True
                            bezel_string_quote = decoded
                    i += 2
                    continue
                if d == '"':
                    i += 1
                    break
                phys_end = i + 1
                if bezel_line_comment:
                    emit(" ", phys_end)
                    if d == "\n":
                        bezel_line_comment = False
                elif bezel_block_comment:
                    emit(" ", phys_end)
                elif in_bezel_string:
                    emit(" ", phys_end)
                    if d == bezel_string_quote:
                        in_bezel_string = False
                else:
                    if d == "/" and i + 1 < n and text[i + 1] == "/":
                        bezel_line_comment = True
                        emit(" ", phys_end)
                    elif d in ('"', "'"):
                        in_bezel_string = True
                        bezel_string_quote = d
                        emit(" ", phys_end)
                    else:
                        emit(d, phys_end)
                i += 1
            last_group_content_end = i
            gap_start = i
            continue
        i += 1
    return "".join(logical), endmap


# ---- construct-stack comma classifier --------------------------------------

# Frame modes.
CASE = "case"      # case/when arms — has PATTERN/BODY region
CATCH = "catch"    # catch arms — has PATTERN/BODY region
TRY = "try"        # try body (opaque until `catch`)
OPAQUE = "opaque"  # do/if/while/for/loop/lock/unsafe body

PATTERN = 0
BODY = 1

WORD_CHARS = set(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
)


def tokenize_words(logical):
    """Yield (start, word) for identifier/keyword words in the logical text."""
    n = len(logical)
    i = 0
    while i < n:
        c = logical[i]
        if c in WORD_CHARS and not c.isdigit():
            j = i
            while j < n and logical[j] in WORD_CHARS:
                j += 1
            yield i, logical[i:j]
            i = j
        else:
            i += 1


def find_alt_commas(logical):
    """Return a sorted list of logical indices of commas that separate pattern
    alternatives (case-arm constant lists or catch multi-type lists)."""
    # Pre-index word starts so we can recognize keywords while scanning chars.
    word_at = {}
    for start, word in tokenize_words(logical):
        word_at[start] = word

    inserts = []
    stack = []  # list of dicts: {mode, region, pending_loop_do}
    depth = 0   # paren/bracket/brace depth
    n = len(logical)
    i = 0
    while i < n:
        c = logical[i]
        if c in "([{":
            depth += 1
            i += 1
            continue
        if c in ")]}":
            if depth > 0:
                depth -= 1
            i += 1
            continue
        if c == "=" and i + 1 < n and logical[i + 1] == ">":
            if depth == 0 and stack and stack[-1]["mode"] in (CASE, CATCH):
                if stack[-1]["region"] == PATTERN:
                    stack[-1]["region"] = BODY
            i += 2
            continue
        if c == ",":
            if depth == 0 and stack and stack[-1]["mode"] in (CASE, CATCH):
                if stack[-1]["region"] == PATTERN:
                    inserts.append(i)            # alternative separator
                else:
                    stack[-1]["region"] = PATTERN  # arm separator
            i += 1
            continue
        if c == "\x01":  # C++ variable-splice sentinel: hidden Bezel text
            if depth == 0 and stack and stack[-1]["mode"] in (CASE, CATCH):
                stack[-1]["region"] = BODY  # conservatively assume an arm body
            i += 1
            continue
        if i in word_at:
            word = word_at[i]
            wlen = len(word)
            _handle_word(word, stack)
            i += wlen
            continue
        i += 1
    return inserts


def _handle_word(word, stack):
    top = stack[-1] if stack else None
    if word == "case" or word == "when":
        # `when` inside a case/catch PATTERN region is a GUARD, not a construct.
        if (word == "when" and top is not None
                and top["mode"] in (CASE, CATCH)
                and top["region"] == PATTERN):
            return
        stack.append({"mode": CASE, "region": PATTERN, "pending_loop_do": False})
        return
    if word == "try":
        stack.append({"mode": TRY, "region": PATTERN, "pending_loop_do": False})
        return
    if word == "catch":
        # Belongs to the enclosing try frame: switch it to catch-arm mode.
        if top is not None and top["mode"] in (TRY, CATCH):
            top["mode"] = CATCH
            top["region"] = PATTERN
        else:
            # Defensive: a bare `catch` — treat as a catch frame.
            stack.append(
                {"mode": CATCH, "region": PATTERN, "pending_loop_do": False})
        return
    if word == "finally":
        if top is not None and top["mode"] in (TRY, CATCH):
            top["mode"] = TRY  # opaque finally body
        return
    if word in ("if", "unsafe", "lock"):
        stack.append({"mode": OPAQUE, "region": PATTERN, "pending_loop_do": False})
        return
    if word in ("while", "for", "loop"):
        stack.append({"mode": OPAQUE, "region": PATTERN, "pending_loop_do": True})
        return
    if word == "do":
        # The header `do` of a for/while/loop is a separator, not a block.
        if top is not None and top.get("pending_loop_do"):
            top["pending_loop_do"] = False
            return
        stack.append({"mode": OPAQUE, "region": PATTERN, "pending_loop_do": False})
        return
    if word == "end":
        if stack:
            stack.pop()
        return


def migrate_file(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if path.endswith(".bzl"):
        logical, endmap = build_logical_bzl(text)
    else:
        logical, endmap = build_logical_cpp(text)
    alt_commas = find_alt_commas(logical)
    if not alt_commas:
        return 0
    # Map each logical comma index to its physical offset and replace the
    # alternative comma with ` | `. endmap[k] is the physical offset JUST PAST
    # logical[k]; the comma is one physical char, so its start is endmap[k]-1.
    # A single following space (the common `X, Y` layout) is absorbed so the
    # result reads `X | Y` rather than `X |  Y`.
    phys_positions = sorted({endmap[k] - 1 for k in alt_commas}, reverse=True)
    out = text
    for phys in phys_positions:
        if out[phys] != ",":
            raise RuntimeError(
                f"{path}: expected ',' at physical offset {phys}, "
                f"found {out[phys]!r}")
        end = phys + 1
        if end < len(out) and out[end] == " ":
            end += 1
        out = out[:phys] + " | " + out[end:]
    with open(path, "w", encoding="utf-8") as f:
        f.write(out)
    return len(phys_positions)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in argv
    roots = args or ["tests"]
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, _dirs, names in os.walk(root):
            for name in names:
                if name.endswith(".cpp") or name.endswith(".bzl"):
                    files.append(os.path.join(dirpath, name))
    total_sites = 0
    total_files = 0
    for path in sorted(files):
        try:
            if dry:
                with open(path, "r", encoding="utf-8") as f:
                    text = f.read()
                if path.endswith(".bzl"):
                    logical, _ = build_logical_bzl(text)
                else:
                    logical, _ = build_logical_cpp(text)
                k = len(find_alt_commas(logical))
            else:
                k = migrate_file(path)
        except Exception as e:  # noqa
            print(f"ERROR {path}: {e}", file=sys.stderr)
            continue
        if k:
            total_files += 1
            total_sites += k
            print(f"{k:3d}  {path}")
    verb = "would rewrite" if dry else "rewrote"
    print(f"\n{verb} {total_sites} alternative commas across {total_files} files")


if __name__ == "__main__":
    main(sys.argv)
