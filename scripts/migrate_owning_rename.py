#!/usr/bin/env python3
"""Migrate the Bezel corpus and compiler to the v0.168 owning-rename (#1372).

The owning *family* of stdlib types read as the wrong ownership axis:
`OwnedArray<T>` grammatically says "an array that *is owned*" (the container
qualifier axis) while it actually denotes "an array that *owns its elements*"
(the element axis). v0.168 splits the two: `owned` stays the container
qualifier; **`Owning`** becomes the element prefix. The owning *interfaces*
additionally gain the standard `I` prefix (they were the stdlib's only
unprefixed interfaces), and `GetOwnedEnumerator()` follows as
`GetOwningEnumerator()`. See rationale §3.9 and the v0.168 changelog.

This script applies the SAME word-boundary rename map the docs sweep used to:

  (i)   the Bezel compiler under src/ — the load-bearing sites are the `"..."`
        string literals compared against user type names and the diagnostic
        texts (type registries, codegen `n == "OwnedStack"` checks, the
        `isCollectionTypeName` / `isDictTypeName` / `builtinTypeArity` tables,
        etc.); comments carrying the names are updated for accuracy too.
  (ii)  embedded Bezel programs inside C++ string literals in tests/**/*.cpp
  (iii) Bezel source in tests/conformance/cases/*.bzl

The map is applied as case-sensitive **word-boundary** (`\b...\b`) token
replacements, most-specific first. Word boundaries mean:

  * `"OwnedArray"` (string literal)      -> `"OwningArray"`          [rewritten]
  * a bare `OwnedArray` token / comment  -> `OwningArray`            [rewritten]
  * a compound C++ identifier such as `emitOwnedTask` / `ownedTaskType`
    is NOT a boundary match (no `\b` between `t` and `O`, or the leading
    lowercase `owned`) and is left ALONE — internal C++ spellings may keep
    `owned`.
  * the lowercase `owned` qualifier and lowercase C runtime symbols
    (`bz_owned_buffer_alloc`, `bz_task_when_all_owned`) never match (the keys
    are all capitalised) and are left ALONE.

Deliberately NOT in the map (they name an owned ARGUMENT — the qualifier axis,
correctly spelled): `AppendOwned`, `FromOwnedResult`. The `owned` keyword is
untouched. C runtime `bz_*` ABI symbols are untouched.

The rename is a pure token substitution, so unlike the case-of / pattern-
alternative migrations it needs no construct-stack parse: `\b` boundaries make
it safe to run over whole files. Any accidental over/under-migration is caught
downstream — the migrated corpus is recompiled by the test suite.
"""

import os
import re
import sys

# Most-specific first (identical to the docs sweep). Strict `\b...\b` word
# boundaries make the ordering non-load-bearing (a longer key is never a
# boundary-substring of a shorter one), but we keep the documented order.
RENAME_MAP = [
    ("GetOwnedAsyncEnumerator", "GetOwningAsyncEnumerator"),
    ("GetOwnedEnumerator", "GetOwningEnumerator"),
    ("IOwnedList", "IOwningList"),
    ("IOwnedCollection", "IOwningCollection"),
    ("OwnedAsyncEnumerable", "IOwningAsyncEnumerable"),
    ("OwnedAsyncEnumerator", "IOwningAsyncEnumerator"),
    ("OwnedEnumerable", "IOwningEnumerable"),
    ("OwnedEnumerator", "IOwningEnumerator"),
    ("OwnedTaskCompletionSource", "OwningTaskCompletionSource"),
    ("OwnedTask", "OwningTask"),
    ("OwnedArray", "OwningArray"),
    ("OwnedBuffer", "OwningBuffer"),
    ("OwnedDictionary", "OwningDictionary"),
    ("OwnedSet", "OwningSet"),
    ("OwnedStack", "OwningStack"),
    ("OwnedQueue", "OwningQueue"),
    ("OwnedLinkedList", "OwningLinkedList"),
]

_PATTERNS = [(re.compile(r"\b" + re.escape(old) + r"\b"), new)
             for old, new in RENAME_MAP]

SRC_EXTS = (".cpp", ".hpp", ".h", ".cc", ".cxx")
CORPUS_EXTS = (".cpp", ".hpp", ".h", ".cc", ".cxx", ".bzl")


def migrate_text(text):
    """Apply the ordered word-boundary map. Returns (new_text, n_sites)."""
    total = 0
    for pat, new in _PATTERNS:
        text, k = pat.subn(new, text)
        total += k
    return text, total


def count_text(text):
    return sum(len(pat.findall(text)) for pat, _ in _PATTERNS)


def collect_files(roots):
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, _dirs, names in os.walk(root):
            for name in names:
                if name.endswith(CORPUS_EXTS):
                    files.append(os.path.join(dirpath, name))
    return sorted(files)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in argv
    roots = args or ["src", "tests"]
    files = collect_files(roots)
    total_sites = 0
    total_files = 0
    for path in files:
        try:
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
        except (UnicodeDecodeError, OSError) as e:
            print(f"SKIP {path}: {e}", file=sys.stderr)
            continue
        if dry:
            k = count_text(text)
        else:
            new_text, k = migrate_text(text)
            if k:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(new_text)
        if k:
            total_files += 1
            total_sites += k
            print(f"{k:5d}  {path}")
    verb = "would rewrite" if dry else "rewrote"
    print(f"\n{verb} {total_sites} occurrences across {total_files} files")


if __name__ == "__main__":
    main(sys.argv)
