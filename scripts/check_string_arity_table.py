#!/usr/bin/env python3
"""check_string_arity_table.py — ratchet: §9.6's string member ARITIES.

The R35 #7144 reviewer required this ratchet (its "S"). Its ruling:

  "S = the same derivation over builtinStringInstanceArities()
   (src/sema/ExprTypecheck_Calls.cpp) — a SECOND hand-maintained
   transcription of the same document."

`src/sema/ExprTypecheck_Calls.cpp` holds THREE tables transcribed from
stdlib §9.6: `builtinStringInstanceArities`, `builtinStringStaticArities`
and `builtinStringUndeclaredArities`. This ratchet checks the first two
against the document: every §9.6 instance/static member's NAME and its
declared ARITIES must be transcribed correctly, and every transcribed name
must be a name §9.6 declares.

This is a BUILD-FREE SOURCE ratchet: it reads the document and the source
as text. It does not compile anything.

Exit codes:
  0  the tables agree with §9.6
  1  they disagree
  2  the checker could not parse its inputs (FAIL CLOSED — never a green)
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STDLIB = os.path.join(REPO, "docs", "Bezel_Standard_Library.md")
CALLS_SRC = os.path.join(REPO, "src", "sema", "ExprTypecheck_Calls.cpp")

CLASS_RE = re.compile(r"^public class String\b")
END_RE = re.compile(r"^\s*end String;")

# A method declaration inside the §9.6 `String` block. The parameter text
# is captured so the arities can be derived from it.
METHOD_RE = re.compile(
    r"^\s*public\s+(?:(?:pure|override|static|partial)\s+)*method\s+"
    r"(?!operator\b)([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)"
)

# A row in the C++ arity table: {"Name", {1, 2}, "..."}.
ROW_RE = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*\{([^}]*)\}')

# The §9.6 members the compiler does not LOWER, and so does not transcribe.
# `GetEnumerator` is the only one: the instance arity table has no row for
# it because there is no concrete member dispatch for it (the #7153
# reviewer's enumeration of §9.6's 26 instance members found it the one
# member with no lowering). The set is asserted BY NAME here so a NEW
# unlowered member cannot be added silently — the checker fails when the
# document declares an instance member that is in neither the table nor
# this list.
NO_LOWERING = {"GetEnumerator"}

# The §9.6 STATIC members the arity table deliberately omits. The source
# says why, in a comment naming each of them:
#
#   "`FromCStringPointer` / `FromWStringPointer` are NOT listed here: the
#    static-primitive path already rejects their wrong arity with 'no
#    matching overload of `String.FromCStringPointer` — it takes exactly
#    one `IntPtr`', and a second diagnostic would report one mistake
#    twice."
#
# So each excluded member must be NAMED in a comment in the source — the
# exclusion is asserted against the source's own words, not against a
# silent hand-maintained list.
NOT_TRANSCRIBED_STATIC = {"FromCStringPointer", "FromWStringPointer"}


def die(msg, code=2):
    sys.stderr.write("check_string_arity_table: %s\n" % msg)
    sys.exit(code)


def read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        die("cannot read %s: %s" % (path, exc))


def string_block(text):
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


def arities_of(params):
    """The arities one overload admits.

    `params` is the text between the parentheses. A parameter with a
    default (`:=`) is optional, so the overload admits every count from
    the number of REQUIRED parameters up to the total.
    """
    if not params.strip():
        return {0}
    required = 0
    total = 0
    for part in params.split(","):
        part = part.strip()
        if not part:
            continue
        total += 1
        if ":=" not in part:
            required += 1
    return set(range(required, total + 1))


def derive_from_stdlib(lines):
    """(instance_arity_map, static_arity_map) derived from §9.6."""
    instance = {}
    static = {}
    for line in lines:
        m = METHOD_RE.match(line)
        if not m:
            continue
        name, params = m.group(1), m.group(2)
        head = line.split("method")[0]
        arities = arities_of(params)
        if "static" in head:
            static.setdefault(name, set()).update(arities)
        else:
            instance.setdefault(name, set()).update(arities)
    if not instance:
        die("no instance methods parsed out of §9.6's `String` block")
    return instance, static


def table_rows(text, fn_name):
    """The {name, {arities}} rows of one C++ table.

    The body is bounded to that ONE function's `kRows = { ... }` — the
    three tables are adjacent in the source, so an unbounded scan would
    merge them and compare a member against the wrong table.
    """
    fm = re.search(r"\b" + re.escape(fn_name) + r"\s*\(\s*\)\s*\{", text)
    if not fm:
        die("no `%s` table found in src/sema/ExprTypecheck_Calls.cpp" % fn_name)
    rest = text[fm.end():]
    km = re.search(r"kRows\s*=\s*\{", rest)
    if not km:
        die("no `kRows = {` in `%s` — the table's shape changed" % fn_name)
    body = rest[km.end():]
    end = re.search(r"\n\s*\};", body)
    if not end:
        die("no closing `};` for `%s`'s kRows — the table's shape changed"
            % fn_name)
    body = body[:end.start()]
    rows = {}
    for name, arities in ROW_RE.findall(body):
        vals = set()
        for a in re.findall(r"\d+", arities):
            vals.add(int(a))
        if name in rows:
            rows[name] = rows[name] | vals
        else:
            rows[name] = vals
    if not rows:
        die("`%s` parsed as EMPTY — the checker would compare nothing" % fn_name)
    return rows


def compare(label, derived, table, allow_missing):
    problems = []
    for name in sorted(set(derived) | set(table)):
        if name in allow_missing:
            if name in table:
                problems.append("%s: `%s` is on the no-lowering list but IS "
                                "transcribed" % (label, name))
            continue
        if name not in table:
            problems.append("%s: §9.6 declares `%s` but the table does not "
                            "transcribe it" % (label, name))
            continue
        if name not in derived:
            problems.append("%s: the table transcribes `%s` but §9.6 declares "
                            "no such member" % (label, name))
            continue
        if table[name] != derived[name]:
            problems.append("%s: `%s` — the table says arities %s, §9.6 says %s"
                            % (label, name,
                               sorted(table[name]), sorted(derived[name])))
    return problems


def self_test():
    """Prove the checker FIRES on a synthetic mismatch.

    A checker that cannot detect a mismatch proves nothing when it
    reports a match, so the gate runs this first.
    """
    derived_instance, derived_static = derive_from_stdlib(string_block(read(STDLIB)))
    src_text = read(CALLS_SRC)
    inst_table = table_rows(src_text, "builtinStringInstanceArities")
    static_table = table_rows(src_text, "builtinStringStaticArities")

    # A table missing one member must be reported as a mismatch.
    victim = None
    for name in sorted(derived_instance):
        if name not in NO_LOWERING:
            victim = name
            break
    if victim is None:
        die("SELF-TEST FAILED: no instance member to remove", code=2)
    broken = {k: v for k, v in inst_table.items() if k != victim}
    problems = compare("self-test", derived_instance, broken, NO_LOWERING)
    if not problems:
        die("SELF-TEST FAILED: a table missing `%s` was NOT detected" % victim)
    # And the real tables, unchanged, must be clean.
    if compare("self-test", derived_instance, inst_table, NO_LOWERING):
        die("SELF-TEST FAILED: the real instance table was reported as a "
            "mismatch")
    if compare("self-test", derived_static, static_table,
               NOT_TRANSCRIBED_STATIC):
        die("SELF-TEST FAILED: the real static table was reported as a "
            "mismatch")
    print("check_string_arity_table --self-test: OK (a table missing `%s` is "
          "detected; the real tables are clean)" % victim)
    return 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    stdlib_text = read(STDLIB)
    derived_instance, derived_static = derive_from_stdlib(string_block(stdlib_text))

    src_text = read(CALLS_SRC)
    inst_table = table_rows(src_text, "builtinStringInstanceArities")
    static_table = table_rows(src_text, "builtinStringStaticArities")

    # Every deliberately-not-transcribed member must be NAMED in a comment
    # in the source that explains the omission. The exclusion is asserted
    # against the source's own words, so a member cannot be dropped from
    # the table silently.
    problems = []
    for name in sorted(NOT_TRANSCRIBED_STATIC):
        if name not in src_text:
            problems.append("static: `%s` is on the not-transcribed list but "
                            "the source does not NAME it in a comment" % name)
        elif name in static_table:
            problems.append("static: `%s` is on the not-transcribed list but "
                            "IS transcribed" % name)

    problems += compare("instance", derived_instance, inst_table, NO_LOWERING)
    problems += compare("static", derived_static, static_table,
                        NOT_TRANSCRIBED_STATIC)

    if problems:
        out = ["check_string_arity_table: MISMATCH between stdlib §9.6 and the "
               "transcribed arity tables"] + ["  " + p for p in problems]
        sys.stderr.write("\n".join(out) + "\n")
        return 1

    print("check_string_arity_table: clean (%d §9.6 instance members and %d "
          "static members agree with the transcribed arity tables)"
          % (len(derived_instance) - len(NO_LOWERING), len(derived_static)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
