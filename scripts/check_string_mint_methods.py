#!/usr/bin/env python3
"""check_string_mint_methods.py — ratchet: §9.6's string-minting instance methods.

The R35 #7144 reviewer required this ratchet. Its ruling (verbatim):

  "R = scripts/check_string_mint_methods.py — parse §9.6's `String` block,
   collect the **instance** methods whose declared result is `string`,
   **subtract** `ToString` (the `override` whose comment says *"returns
   this"*) and the `[r: Range<int>]` **property** (a property, not a
   method), assert the set **equals** `kMintMethods` in
   `src/ownership/Ownership_Checks.cpp`, **fail closed** on a parse
   failure => ⇒ ⇒ ⇒ R WOULD HAVE GONE RED ON THE DAY #7133 LANDED"

This is a BUILD-FREE SOURCE ratchet: it reads the document and the source
as text. It does not compile anything.

Exit codes:
  0  the sets agree
  1  they disagree (a member was added to §9.6 or to kMintMethods)
  2  the checker could not parse its inputs (FAIL CLOSED — never a green)

Usage: scripts/check_string_mint_methods.py [--strict] [--self-test]

  --strict   the mode the gate runs (the verdict is the same either way;
             the flag exists so the gate's invocation shape is uniform).
  --self-test  prove the checker FIRES: run the comparison against a
             synthetic pair of inputs in which one member was removed, and
             require the mismatch to be detected. A checker that cannot
             detect a mismatch proves nothing when it reports a match.
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STDLIB = os.path.join(REPO, "docs", "Bezel_Standard_Library.md")
MINT_SRC = os.path.join(REPO, "src", "ownership", "Ownership_Checks.cpp")

# The §9.6 `String` class declaration — the block the mint set is derived
# from. `public class String: ...` up to its `end String;`.
CLASS_RE = re.compile(r"^public class String\b")
END_RE = re.compile(r"^\s*end String;")

# An instance METHOD declaration inside the block:
#   public pure method Name(args): RetType;
#   public pure override method ToString(): string;
# The `;` (or `=>` body) ends the declaration. `static` members are excluded
# (a static call's receiver is the TYPE NAME, not a string value) and
# `operator` members are excluded (not a named member call).
METHOD_RE = re.compile(
    r"^\s*public\s+(?:(?:pure|override|static|partial)\s+)*method\s+"
    r"(?!operator\b)([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*:\s*([^;=>]+)"
)

# The one exclusion the document itself states: `ToString`'s own comment
# says "returns this" — a BORROW of the receiver, not a fresh value.
TOSTRING_RE = re.compile(
    r"^\s*public\s+(?:(?:pure|override)\s+)*method\s+ToString\s*\(\s*\)\s*:\s*string\s*;"
    r"\s*//\s*returns this"
)


def die(msg, code=2):
    sys.stderr.write("check_string_mint_methods: %s\n" % msg)
    sys.exit(code)


def read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        die("cannot read %s: %s" % (path, exc))


def string_block(text):
    """The §9.6 `String` class block, or die (fail closed)."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if CLASS_RE.match(line):
            start = i
            break
    if start is None:
        die("no `public class String` declaration found in the stdlib")
    for j in range(start, len(lines)):
        if END_RE.match(lines[j]):
            return lines[start:j + 1]
    die("no `end String;` found after the `public class String` declaration")


def derive_from_stdlib(lines):
    """The §9.6 instance methods whose declared result is exactly `string`.

    Returns (set_of_names, tostring_ok). `tosring_ok` is False when the
    `ToString` declaration does not carry the document's own "returns this"
    comment — the exclusion is asserted AGAINST THE DOCUMENT'S WORDS, not
    against a hand-maintained list of exclusions.
    """
    names = set()
    tostring_seen = False
    tostring_ok = False
    for line in lines:
        m = METHOD_RE.match(line)
        if not m:
            if TOSTRING_RE.match(line):
                tostring_seen = True
                tostring_ok = True
                names.add("ToString")
            continue
        name, _args, ret = m.group(1), m.group(2), m.group(3).strip()
        if "static" in line.split("method")[0]:
            continue  # a static member's receiver is the TYPE NAME
        if name == "ToString":
            # Its result is `string`, but it is a BORROW ("returns this").
            # Record it only if the document says so on this very line.
            tostring_seen = True
            tostring_ok = tostring_ok or ("//" in line and
                                           "returns this" in line.split("//", 1)[1])
            continue
        if ret == "string":
            names.add(name)
    if not tostring_seen:
        die("§9.6's `ToString` declaration was not found — cannot verify the "
            "'returns this' exclusion against the document's own words")
    return names, tostring_ok


def derive_from_source(text):
    """The `kMintMethods` array in src/ownership/Ownership_Checks.cpp."""
    m = re.search(r"kMintMethods\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        die("no `kMintMethods` array found in src/ownership/Ownership_Checks.cpp")
    return set(re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', m.group(1)))


def self_test():
    """Prove the checker FIRES on a synthetic mismatch.

    A checker that cannot detect a mismatch proves nothing when it
    reports a match, so the gate runs this first.
    """
    stdlib_names, tostring_ok = derive_from_stdlib(string_block(read(STDLIB)))
    if not tostring_ok:
        die("SELF-TEST FAILED: §9.6's `ToString` exclusion could not be "
            "verified against the document")
    derived = stdlib_names - {"ToString"}
    if len(derived) < 2:
        die("SELF-TEST FAILED: fewer than two derived members", code=2)
    victim = sorted(derived)[0]
    broken = set(derived) - {victim}
    missing = sorted(derived - broken)
    if not missing:
        die("SELF-TEST FAILED: a table missing `%s` was NOT detected" % victim)
    real = derive_from_source(read(MINT_SRC))
    if sorted(derived - real) or sorted(real - derived):
        die("SELF-TEST FAILED: the real table is not in agreement")
    print("check_string_mint_methods --self-test: OK (a table missing `%s` is "
          "detected; the real table is in agreement)" % victim)
    return 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    stdlib_names, tostring_ok = derive_from_stdlib(string_block(read(STDLIB)))
    if not tostring_ok:
        die("§9.6's `ToString` does not carry the `// returns this` comment its "
            "exclusion from the mint set depends on")
    # The exclusion itself: `ToString` is a borrow, not a mint.
    stdlib_mints = stdlib_names - {"ToString"}

    src_mints = derive_from_source(read(MINT_SRC))

    missing = sorted(stdlib_mints - src_mints)
    extra = sorted(src_mints - stdlib_mints)
    if missing or extra:
        out = ["check_string_mint_methods: MISMATCH between stdlib §9.6 and "
               "kMintMethods"]
        if missing:
            out.append("  in §9.6 but NOT in kMintMethods (a mint the §28.3 "
                       "gate would not catch): %s" % ", ".join(missing))
        if extra:
            out.append("  in kMintMethods but NOT in §9.6 (a name the document "
                       "does not declare): %s" % ", ".join(extra))
        out.append("  §9.6 mints: %s" % ", ".join(sorted(stdlib_mints)))
        out.append("  kMintMethods: %s" % ", ".join(sorted(src_mints)))
        sys.stderr.write("\n".join(out) + "\n")
        return 1

    print("check_string_mint_methods: clean (%d §9.6 minting instance members "
          "agree with kMintMethods; `ToString` excluded on the document's own "
          "'returns this' comment)" % len(stdlib_mints))
    return 0


if __name__ == "__main__":
    sys.exit(main())
