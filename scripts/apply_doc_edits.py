#!/usr/bin/env python3
"""Apply a revision's edits as one dry-run sequence.

Kept in `scripts/` rather than a scratch dir because the guards below were each
bought by a defect: two lanes moving the same pin (v0.299), and eight fenced
lines landing at column 0 in the code-generation reference (v0.300).

The process (spec-change-process.md §4) requires every `old` to be unique and to
apply clean against the *combined intermediate state*, not against the pristine
file — two lanes editing neighbouring prose can each be unique alone and
ambiguous together. So this applies in order and re-checks uniqueness each time.

Usage:
    apply.py edits.json            # dry run: report only
    apply.py edits.json --write    # apply

edits.json is a list of {"id", "file", "old", "new"}; `id` is the lane and issue
the edit came from, so a failure names who to go back to.
"""
import json
import sys


def main(argv):
    path = argv[0]
    write = "--write" in argv
    edits = json.load(open(path))
    bufs, problems, applied = {}, [], 0
    for i, e in enumerate(edits):
        f = e["file"]
        if f not in bufs:
            bufs[f] = open(f, encoding="utf-8").read()
        n = bufs[f].count(e["old"])
        if n == 0:
            problems.append("{} [{}] {}: OLD not found (an earlier edit in this "
                            "sequence may have consumed it)".format(i, e["id"], f))
        elif n > 1:
            problems.append("{} [{}] {}: OLD occurs {} times — not unique"
                            .format(i, e["id"], f, n))
        else:
            # An `old` that starts mid-line inherits the line's indentation for
            # its FIRST line only; a multi-line `new` then lands flush-left from
            # line two. That is invisible to every structural check — Bezel is
            # not indentation-sensitive and the fence stays balanced — and it
            # put eight lines at column 0 inside fenced class bodies in v0.300,
            # in the document whose whole job is to be copied from.
            at = bufs[f].index(e["old"])
            bol = bufs[f].rfind("\n", 0, at) + 1
            indent = bufs[f][bol:at]
            if indent and indent.isspace() and "\n" in e["new"]:
                head, *rest = e["new"].split("\n")
                if any(r and not r.startswith(indent) for r in rest):
                    problems.append(
                        "{} [{}] {}: multi-line NEW at an indented site would "
                        "land flush-left from line 2 (indent {!r})"
                        .format(i, e["id"], f, indent))
                    continue
            bufs[f] = bufs[f].replace(e["old"], e["new"], 1)
            applied += 1
    for p in problems:
        print("FAIL " + p)
    print("{}/{} edits apply clean across {} files"
          .format(applied, len(edits), len(bufs)))
    if problems:
        return 1
    if write:
        for f, text in bufs.items():
            with open(f, "w", encoding="utf-8") as fh:
                fh.write(text)
        print("written")
    else:
        print("(dry run — pass --write to apply)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
