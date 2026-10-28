#!/usr/bin/env python3
"""check_builtin_declared_params.py — ratchet: the DECLARED PARAMETER TYPES.

THE DERIVATION. `src/common/BuiltinRegistry.cpp` holds `kBuiltinParams` — the
ONE built-in parameter table, the table every built-in gate reads for a
member's parameter NAME (§22.5), parameter TYPE (§5.1) and parameter
NULLABILITY (§7.3).  Its §9.6 `String` rows and its §2.5 `DateTime` /
`TimeSpan` rows carry the DECLARED PARAMETER TYPES of the standard library's
own listing, transcribed by hand.

This ratchet is the CHECK on that transcription: it parses the standard
library out of `docs/Bezel_Standard_Library.md` and asserts that every
declared form's parameter TYPES and NULLABILITY are the ones the table
carries.  A change to the document therefore fails the gate until the table
is brought back into step with it — so the table is a CHECKED transcription
of the document, not a second opinion about it.

It is a BUILD-FREE SOURCE ratchet: it reads the document and the source as
text.  It compiles nothing.  It is wired into `scripts/gate_full_cycle.sh`
beside `check_string_arity_table.py` and `check_string_mint_methods.py`, the
two ratchets that check the other §9.6 transcriptions the same way.

WHY THE TABLE AND NOT A NEW ONE.  A hand-maintained SIXTH mirror of the
standard library is not acceptable (the R36 campaign-2 anchor's ruling), and
this ratchet is what makes the ONE table safe to extend: the derivation is
mechanical, and the check is mechanical too.  The ratchet is the "WHO CHECKS
IT IN" half of the answer.

Exit codes:
  0  the table agrees with the standard library
  1  they disagree
  2  the checker could not parse its inputs (FAIL CLOSED — never a green)
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STDLIB = os.path.join(REPO, "docs", "Bezel_Standard_Library.md")
REGISTRY = os.path.join(REPO, "src", "common", "BuiltinRegistry.cpp")

# ── the standard library's blocks ──────────────────────────────────────────

# A method declaration inside a class block:
#   public [pure|override|static|partial] method Name(params): Ret;
# The parameter text is captured so the declared types can be derived from
# it.  `operator` declarations are not methods.
METHOD_RE = re.compile(
    r"^\s*public\s+(?:(?:pure|override|static|partial)\s+)*method\s+"
    r"(?!operator\b)([A-Za-z_][A-Za-z0-9_]*)\s*\("
)

# A parameter whose type is a FUNCTION TYPE carries parentheses of its own
# (`comparer: (T, T) -> T`), so the parameter list is split at the TOP-LEVEL
# commas only.
def split_params(text):
    out = []
    depth = 0
    cur = ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        cur += ch
    if cur.strip():
        out.append(cur)
    return out


def balanced(text):
    """Is the parenthesis nesting of `text` closed?"""
    depth = 0
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
    return depth == 0


def method_params(text, name):
    """The parameter text of the declaration of `name` on this line."""
    m = METHOD_RE.match(text)
    if not m or m.group(1) != name:
        return None
    i = m.end()
    depth = 1
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[m.end():i]
        i += 1
    die("unterminated parameter list on the line declaring `%s`" % name)

# The blocks this ratchet checks.
BLOCKS = [
    # (the class declaration that opens the block, the CLASS name the block
    # ends with, the head the table is keyed by)
    ("public class String:", "String", "string"),
    ("public record DateTime:", "DateTime", "DateTime"),
    ("public record TimeSpan:", "TimeSpan", "TimeSpan"),
]

# ── the ONE built-in parameter table ───────────────────────────────────────

# A row in the table:
#   {"string", "Trim", 1, 1, 0x01, {"chars", ...}, {kBPTEnumerableChar, ...},
#    "Trim(chars: IEnumerable<char>): string"},
ROW_RE = re.compile(
    r'\{\s*"(?P<head>[A-Za-z_][A-Za-z0-9_]*)"\s*,\s*'
    r'"(?P<member>[A-Za-z_][A-Za-z0-9_]*)"\s*,\s*'
    r'(?P<total>\d+)\s*,\s*(?P<required>\d+)\s*,\s*0x(?P<nn>[0-9A-Fa-f]{2})\s*,\s*'
    r'\{(?P<names>[^}]*)\}\s*,\s*\{(?P<kinds>[^}]*)\}\s*,\s*'
    r'"(?P<sig>[^"]*)"'
)

# The declared type a kind bit names.  A bit the vocabulary cannot express is
# a position with NO type opinion (the fail-safe direction: a miss costs a
# rejection that does not fire, never a false one).
KIND_BITS = {
    "kBPTString": "string",
    "kBPTInt": "int",
    "kBPTLong": "long",
    "kBPTUInt": "uint",
    "kBPTULong": "ulong",
    "kBPTByte": "byte",
    "kBPTSByte": "sbyte",
    "kBPTShort": "short",
    "kBPTUShort": "ushort",
    "kBPTFloat": "float",
    "kBPTDouble": "double",
    "kBPTDecimal": "decimal",
    "kBPTBool": "bool",
    "kBPTChar": "char",
    "kBPTObject": "Object",
    "kBPTSpan": "ReadOnlyStringSpan",
    # `kBPTOut` marks an `out` slot (§22.3): the slot's type is the
    # out-variable machinery's, not the declaration's, so it is no type
    # opinion about the declared parameter type.
    "kBPTOut": "",
    "kBPTEnumerableChar": "IEnumerable<char>",
    "kBPTArrayChar": "Array<char>",
    "kBPTEnumerableString": "IEnumerable<string>",
    "kBPTRangeInt": "Range<int>",
}

# The positions the table DELIBERATELY carries no type opinion for, each with
# the reason.  A position that is neither covered by the table nor listed here
# FAILS the ratchet — so a new standard-library declaration cannot slip in
# untranscribed.
#
#   (head, member, position) -> why the position carries no type opinion
# The members the standard library declares and the table carries NO row
# for, each with the reason.  A member that is neither transcribed nor
# listed here FAILS the ratchet.
NOT_TRANSCRIBED = {
    ("DateTime", "CompareTo"):
        "stdlib §2.5 declares `other: DateTime`, a RECORD, and the vocabulary "
        "names the scalar and interface kinds only",
    ("DateTime", "Subtract"):
        "stdlib §2.5 declares `other: DateTime` / `span: TimeSpan`, RECORDS, "
        "and the vocabulary names the scalar and interface kinds only",
    ("DateTime", "Add"):
        "stdlib §2.5 declares `span: TimeSpan`, a RECORD, and the vocabulary "
        "names the scalar and interface kinds only",
    ("TimeSpan", "CompareTo"):
        "stdlib §2.5 declares `other: TimeSpan`, a RECORD, and the vocabulary "
        "names the scalar and interface kinds only",
}

# The positions the table DELIBERATELY carries no type opinion for, each with
# the reason.
NO_OPINION = {
    ("DateTime", "FromTicks", 1):
        "stdlib §2.5 declares `kind: DateTimeKind`, an ENUM, and the "
        "vocabulary names the scalar and interface kinds only",
    ("DateTime", "TryParse", 1):
        "stdlib §2.5 declares `out result: DateTime`, an `out` SLOT (§22.3); "
        "the slot's type is the out-variable machinery's, not the "
        "declaration's",
    ("string", "From", 0):
        "stdlib §9.6 declares THREE one-argument forms — `From(value: char)`, "
        "`From(value: CString)` and `From(value: WString)` — and the FFI types "
        "have no bit in the vocabulary; the compiler's own `String.From` gate "
        "owns that form's argument type",
    ("string", "FromCStringPointer", 0):
        "stdlib §9.6 declares `p: IntPtr` and `IntPtr` has no bit in the "
        "vocabulary",
    ("string", "FromWStringPointer", 0):
        "stdlib §9.6 declares `p: IntPtr` and `IntPtr` has no bit in the "
        "vocabulary",
    ("string", "Join", 0):
        "the dedicated `string.Join` gate (fix #7150) already asks the §5.1 "
        "question against the DECLARED `separator: string`",
    ("string", "Join", 1):
        "the dedicated `string.Join` gate (fix #7167 / #7191) already asks the "
        "§5.1 / §12.9 question against the DECLARED `items: "
        "IEnumerable<string>`, and a weaker second test would refuse a legal "
        "argument that gate admits",
}

# The positions where the table carries a type the standard library does not
# name, each with the §-sentence that admits it.
EXTRA_ALLOWED = {
    ("string", "Equals", 0):
        "stdlib §9.6's own comment on `Equals(other: string)` — \"a `string?` "
        "or `Object?` argument reaches it through the slot\" — and spec §8.3's "
        "Object slot (\"the `Object` slot `Equals` has a second spelling, at "
        "the overriding class's own type\")",
}

# The positions the table declares NULLABLE where the standard library spells
# the parameter without `?`, each with the §-sentence that admits it.
NULLABLE_ALLOWED = {
    ("string", "Equals", 0):
        "stdlib §9.6's own comment on `Equals(other: string)` — \"a `string?` "
        "or `Object?` argument reaches it through the slot\" — so the "
        "one-argument form admits the null the two-argument form spells "
        "`other: string?`",
}


def die(msg, code=2):
    sys.stderr.write("check_builtin_declared_params: %s\n" % msg)
    sys.exit(code)


def read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        die("cannot read %s: %s" % (path, exc))


# ── the standard library ───────────────────────────────────────────────────

def parse_params(text):
    """The parameters of one declaration, as (name, type, has_default)."""
    out = []
    for part in split_params(text):
        part = part.strip()
        if not part:
            continue
        default = ":=" in part
        if default:
            part = part.split(":=", 1)[0].strip()
        if ":" not in part:
            die("cannot parse the parameter %r (in %r)" % (part, text))
        name, ty = part.split(":", 1)
        out.append((name.strip(), ty.strip(), default))
    return out


def parse_stdlib(text):
    """The declared members of each checked block.

    Returns {(head, member): [overload]} where an overload is a list of
    (name, type, has_default).
    """
    lines = text.splitlines()
    out = {}
    for opener, cls, head in BLOCKS:
        start = None
        for i, line in enumerate(lines):
            if line.startswith(opener):
                start = i
                break
        if start is None:
            die("no %r declaration found in the stdlib" % opener)
        # The block ends at its own `end <Class>;` — a declaration AFTER it
        # belongs to another class.
        end = None
        for j in range(start + 1, len(lines)):
            if lines[j].strip() == "end %s;" % cls:
                end = j
                break
        if end is None:
            die("no `end %s;` found after %r" % (head, opener))
        i = start
        while i <= end:
            line = lines[i]
            m = METHOD_RE.match(line)
            if not m:
                i += 1
                continue
            member = m.group(1)
            # A declaration may span several lines — its parameter list is
            # closed on a later one.
            buf = line
            while not balanced(buf) and i + 1 <= end:
                i += 1
                buf += " " + lines[i].strip()
            params = method_params(buf, member)
            if params is None:
                i += 1
                continue
            out.setdefault((head, member), []).append(parse_params(params))
            i += 1
    return out


# ── the table ──────────────────────────────────────────────────────────────

def parse_table(text):
    """The table's rows, as {(head, member, total): [(kinds, non_nullable)]}.

    `kinds` is the SET of declared type names at each position; a position
    with no bit is the empty set (no type opinion).
    """
    out = {}
    for m in ROW_RE.finditer(text):
        head = m.group("head")
        member = m.group("member")
        total = int(m.group("total"))
        nn = int(m.group("nn"), 16)
        kinds = []
        for raw in m.group("kinds").split(","):
            raw = raw.strip()
            if not raw or raw == "0":
                kinds.append(frozenset())
                continue
            names = set()
            for bit in raw.split("|"):
                bit = bit.strip()
                name = KIND_BITS.get(bit)
                if name is None:
                    die("the table uses the unknown kind bit %r" % bit)
                if name:
                    names.add(name)
            kinds.append(frozenset(names))
        if len(kinds) < total:
            kinds += [frozenset()] * (total - len(kinds))
        out.setdefault((head, member, total), []).append((kinds, nn))
    return out


# ── the comparison ─────────────────────────────────────────────────────────

def declared_at(stdlib, head, member, arity):
    """The declared types at each position of `member` at `arity`.

    The union over the standard library's overloads of that argument count:
    a position whose declared overload set is `char` OR `string` is the union
    of the two.
    """
    overloads = stdlib.get((head, member))
    if not overloads:
        return None
    acc = None
    for params in overloads:
        required = sum(1 for p in params if not p[2])
        total = len(params)
        if not (required <= arity <= total):
            continue
        kinds = []
        for p in range(arity):
            # the declared type at position `p` of THIS overload: `T?` is the
            # type `T`, and the NULLABILITY is read separately.
            kinds.append(frozenset([params[p][1].rstrip("?")]))
        if acc is None:
            acc = kinds
        else:
            merged = []
            for i in range(len(acc)):
                merged.append(acc[i] | kinds[i])
            acc = merged
    return acc


def compare(stdlib, table):
    """The disagreements between the standard library and the table."""
    errors = []

    # 1. Every declared form with a parameter must be transcribed.
    for (head, member), overloads in sorted(stdlib.items()):
        arities = set()
        for params in overloads:
            required = sum(1 for p in params if not p[2])
            for arity in range(required, len(params) + 1):
                arities.add(arity)
        for arity in sorted(arities):
            if arity == 0:
                continue  # a no-parameter form carries no type to check
            if (head, member) in NOT_TRANSCRIBED:
                continue
            rows = table.get((head, member, arity))
            if not rows:
                errors.append(
                    "stdlib declares `%s.%s` at %d argument(s) but the table "
                    "carries no row for it" % (head, member, arity))
                continue
            want = declared_at(stdlib, head, member, arity)
            if want is None:
                continue
            # the union of the table's rows at this arity
            got = [set() for _ in want]
            nn_union = 0
            nn_all = 0xFF
            for kinds, nn in rows:
                for i in range(len(want)):
                    got[i] |= kinds[i]
                nn_union |= nn
                nn_all &= nn
            for i, want_set in enumerate(want):
                if not want_set:
                    continue
                got_set = got[i]
                extra = got_set - want_set
                for e in sorted(extra):
                    key = (head, member, i)
                    if key in EXTRA_ALLOWED:
                        continue
                    errors.append(
                        "stdlib declares `%s.%s` at position %d as %s, but the "
                        "table also declares `%s` there"
                        % (head, member, i, " / ".join(sorted(want_set)), e))
                missing = want_set - got_set
                if missing and (head, member, i) not in NO_OPINION:
                    errors.append(
                        "stdlib declares `%s.%s` at position %d as %s, but the "
                        "table carries no type opinion there"
                        % (head, member, i, " / ".join(sorted(want_set))))
                # NULLABILITY: a parameter the standard library spells with `?`
                # is nullable; every other spelling is not.  A position the
                # table carries no type opinion for is not checked here — the
                # nullability belongs to the DECLARED TYPE, and there is none.
                if (head, member, i) in NO_OPINION:
                    continue
                nullable_doc = any(
                    params[i][1].endswith("?")
                    for params in stdlib[(head, member)]
                    if len(params) > i)
                if nullable_doc:
                    if nn_all & (1 << i):
                        errors.append(
                            "stdlib declares `%s.%s` at position %d as a "
                            "NULLABLE type (`%s`), but the table marks it "
                            "non-nullable"
                            % (head, member, i,
                               "/".join(sorted(want_set)) or "?"))
                else:
                    if not (nn_all & (1 << i)) and \
                            (head, member, i) not in NULLABLE_ALLOWED:
                        errors.append(
                            "stdlib declares `%s.%s` at position %d as a "
                            "NON-nullable type, but the table marks it "
                            "nullable" % (head, member, i))

    # 2. Every row the table carries for a checked head must be a form the
    #    standard library declares.
    for (head, member, total), rows in sorted(table.items()):
        if head not in (b[2] for b in BLOCKS):
            continue
        if (head, member) in NOT_TRANSCRIBED:
            continue
        if not stdlib.get((head, member)):
            errors.append(
                "the table carries a row for `%s.%s` at %d argument(s), which "
                "stdlib §9.6 / §2.5 declares no member of" %
                (head, member, total))
            continue
        if total == 0:
            continue
        want = declared_at(stdlib, head, member, total)
        if want is None:
            errors.append(
                "the table carries a row for `%s.%s` at %d argument(s), which "
                "is no declared form of that member" %
                (head, member, total))

    return errors


def self_test():
    """The checker must DETECT a mismatch — a checker that cannot is a
    green that means nothing (the R35 #7144 reviewer's "S")."""
    stdlib = parse_stdlib(read(STDLIB))
    table = parse_table(read(REGISTRY))
    if compare(stdlib, table):
        die("the checker reports a mismatch on the tree it lives in", 1)
    # (1) a declared type the table does not carry.
    mutated = {k: list(v) for k, v in table.items()}
    rows = mutated[("string", "Contains", 1)]
    rows[0] = ([frozenset()] + list(rows[0][0][1:]), rows[0][1])
    if not compare(stdlib, mutated):
        die("the checker does not detect a MISSING declared type", 1)
    # (2) a declared type the standard library does not name.
    mutated = {k: list(v) for k, v in table.items()}
    rows = mutated[("string", "Contains", 1)]
    rows[0] = ([frozenset(["int"])] + list(rows[0][0][1:]), rows[0][1])
    if not compare(stdlib, mutated):
        die("the checker does not detect a WRONG declared type", 1)
    # (3) a nullability the standard library does not spell.
    mutated = {k: list(v) for k, v in table.items()}
    rows = mutated[("string", "Trim", 1)]
    rows[0] = (rows[0][0], 0x00)
    if not compare(stdlib, mutated):
        die("the checker does not detect a WRONG nullability", 1)
    sys.stdout.write("check_builtin_declared_params: self-test OK\n")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    stdlib = parse_stdlib(read(STDLIB))
    table = parse_table(read(REGISTRY))
    errors = compare(stdlib, table)
    if errors:
        sys.stderr.write("check_builtin_declared_params: %d disagreement(s) "
                         "between the standard library and the ONE built-in "
                         "parameter table:\n" % len(errors))
        for e in errors:
            sys.stderr.write("  - %s\n" % e)
        sys.exit(1)
    sys.stdout.write("check_builtin_declared_params: the table agrees with "
                     "the standard library\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
