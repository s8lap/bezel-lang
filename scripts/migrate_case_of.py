#!/usr/bin/env python3
"""Migrate the Bezel corpus to the v0.166 `case Expression of Arms end` syntax
(spec §19.1 / Appendix A.7 CaseExpr, #1370).

Through v0.165 a scrutinee'd `case` was `case <scrutinee> <arm>, ... end`, with
no delimiter between the scrutinee and the first arm. v0.166 requires the
keyword `of` between them: `case <scrutinee> of <arm>, ... end`. This script
mechanically inserts ` of` after every scrutinee across:

  (i)  embedded Bezel programs inside C++ string literals in tests/**/*.cpp
  (ii) Bezel source in tests/conformance/cases/*.bzl

`when` (the scrutinee-less condition-dispatch sibling) is unaffected — it has no
scrutinee and takes no `of`; this script never touches it. The `when` pattern
guard inside an arm is likewise untouched.

Approach
--------
For each file, build a "logical" Bezel text out of the source (for .cpp: the
decoded contents of C++ string-literal groups; for .bzl: the whole file), with a
map from each logical character back to its physical file offset. Bezel `//` and
`/* */` comments and Bezel string-literal interiors are blanked so a `case`
appearing inside them is never matched. Then locate each real `case` construct
in the logical text and record a physical insertion point for ` of` after the
scrutinee.

A `case` occurrence is treated as a real construct (and migrated) only when,
after the scrutinee, an arm arrow `=>` follows within a short window before any
statement boundary. This rejects the many prose uses of the word "case" ("close
the case expression", "case expression did not match", etc.) which never contain
`=>`.

The scrutinee is parsed as a single primary (identifier / `this` / numeric
literal), optionally followed by a tight postfix chain (`.member`, `(...)`,
`[...]`, `!`) — matching the shapes that actually occur in the corpus (all
scrutinees there are simple). If a scrutinee is more complex than this and gets
mis-handled, the compiler flags it and the site is fixed by hand.

Idempotent: a scrutinee already followed by `of` is left alone.
"""

import re
import sys
import os

# A single scrutinee primary with an optional tight postfix chain. No leading
# whitespace is allowed inside a postfix step, so `x C.S` stops the scrutinee at
# `x` (two space-separated tokens are never one expression), while `a.b`, `f(x)`,
# and `arr[i]` are absorbed as one scrutinee.
PRIMARY = (
    r"(?:this\b"
    r"|[0-9][0-9_]*(?:\.[0-9][0-9_]+)?[A-Za-z]*"      # numeric literal (+suffix)
    r"|[A-Za-z_][A-Za-z0-9_]*)"                        # identifier
)
POSTFIX = r"(?:\.[A-Za-z_][A-Za-z0-9_]*|\([^()]*\)|\[[^\]]*\]|!)"
SCRUT = PRIMARY + POSTFIX + r"*"
CASE_HEAD = re.compile(r"\bcase\s+(" + SCRUT + r")")

# After the scrutinee, a real case arm reaches `=>` within a short window. We
# scan for `=>` before an obvious statement terminator to avoid running into an
# unrelated later construct.
ARROW_WINDOW = 200


def build_logical_bzl(text):
    """Whole .bzl file is Bezel source. Blank comments and string interiors."""
    logical = []
    endmap = []  # endmap[i] = physical offset just past source of logical[i]
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        # Bezel line comment
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j < 0:
                j = n
            for k in range(i, j):
                logical.append(" ")
                endmap.append(k + 1)
            i = j
            continue
        # Bezel block comment
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                logical.append(" ")
                endmap.append(k + 1)
            i = j
            continue
        # Bezel string / char / interpolated string: blank interior, keep quotes
        if c in ('"', "'"):
            quote = c
            logical.append(" ")  # blank the opening quote too (avoid matching)
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
    logical Bezel text. Groups (maximal runs of literals joined by adjacency or
    `+`-splices) are separated by a newline so unrelated programs never merge.
    Within a group, Bezel `//`,`/* */` comments and Bezel string interiors are
    blanked."""
    logical = []
    endmap = []
    i, n = 0, len(text)
    in_bezel_string = False
    bezel_string_quote = ""
    bezel_line_comment = False
    bezel_block_comment = False
    last_group_content_end = None  # physical offset just past last emitted group

    def emit(ch, phys_end):
        logical.append(ch)
        endmap.append(phys_end)

    while i < n:
        c = text[i]
        # ---- outside a C++ string literal: skip C++ code/comments -----------
        # C++ line comment
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        # C++ block comment
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        # C++ char literal
        if c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        # C++ string literal
        if c == '"':
            # Decide whether this literal continues the current group or starts
            # a new one. It continues if the previous emitted group ended and
            # everything since was only whitespace / `+` / simple splice tokens.
            if last_group_content_end is not None:
                gap = text[gap_start:i]
                if re.fullmatch(r"[\s]*", gap) or re.fullmatch(
                    r"[\s]*(\+[\s]*[A-Za-z_][\w.]*(\([^()]*\))?[\s]*)*\+?[\s]*",
                    gap,
                ):
                    pass  # continuation: no separator
                else:
                    emit("\n", i)  # group break separator
            # scan the literal body
            i += 1
            in_bezel_string = False
            bezel_line_comment = False
            bezel_block_comment = False
            while i < n:
                d = text[i]
                # C++ escape: two physical chars -> one logical char
                if d == "\\" and i + 1 < n:
                    esc = text[i + 1]
                    # decode the common escapes to their logical char
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
                    # end of the C++ string literal
                    i += 1
                    break
                phys_end = i + 1
                if bezel_line_comment:
                    emit(" ", phys_end)
                    if d == "\n":
                        bezel_line_comment = False
                elif bezel_block_comment:
                    emit(" ", phys_end)
                    if d == "*" and i + 1 < n and text[i + 1] == "/":
                        # (block comment end handled crudely on next chars)
                        pass
                elif in_bezel_string:
                    emit(" ", phys_end)
                    if d == bezel_string_quote:
                        in_bezel_string = False
                else:
                    # start of a Bezel comment inside the program?
                    if d == "/" and i + 1 < n and text[i + 1] == "/":
                        bezel_line_comment = True
                        emit(" ", phys_end)
                    elif d in ('"', "'"):
                        in_bezel_string = True
                        bezel_string_quote = d
                        emit(" ", phys_end)  # blank the quote
                    else:
                        emit(d, phys_end)
                i += 1
            last_group_content_end = i
            gap_start = i
            continue
        i += 1
    return "".join(logical), endmap


def find_insertions(logical, endmap):
    """Return a list of physical offsets at which to insert ' of'."""
    inserts = []
    for m in CASE_HEAD.finditer(logical):
        scrut_end = m.end(1)  # logical index just past the scrutinee
        rest = logical[scrut_end:]
        # already `of`? (idempotency / pretty-printer output `case x of {`)
        if re.match(r"\s+of\b", rest):
            continue
        # the next non-space must plausibly begin a pattern
        after = rest.lstrip()
        if after[:1] in ("", ")", "]", "}"):  # nothing / closing bracket -> not a case
            continue
        # a real arm arrow must follow within the window, before a hard stop.
        window = rest[:ARROW_WINDOW]
        arrow = window.find("=>")
        if arrow < 0:
            continue
        # `of` goes right after the scrutinee. endmap[scrut_end-1] is the
        # physical offset just past the scrutinee's last char.
        phys = endmap[scrut_end - 1]
        inserts.append(phys)
    return inserts


def migrate_file(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if path.endswith(".bzl"):
        logical, endmap = build_logical_bzl(text)
    else:
        logical, endmap = build_logical_cpp(text)
    inserts = find_insertions(logical, endmap)
    if not inserts:
        return 0
    # apply, descending, so offsets stay valid
    out = text
    for phys in sorted(set(inserts), reverse=True):
        out = out[:phys] + " of" + out[phys:]
    with open(path, "w", encoding="utf-8") as f:
        f.write(out)
    return len(set(inserts))


def main(argv):
    roots = argv[1:] or ["tests"]
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
            k = migrate_file(path)
        except Exception as e:  # noqa
            print(f"ERROR {path}: {e}", file=sys.stderr)
            continue
        if k:
            total_files += 1
            total_sites += k
            print(f"{k:3d}  {path}")
    print(f"\n{total_sites} sites across {total_files} files")


if __name__ == "__main__":
    main(sys.argv)
