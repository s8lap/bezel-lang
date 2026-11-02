#!/usr/bin/env python3
"""check_builtin_declared_params.py — ratchet: the DECLARED PARAMETER TYPES.

THE DERIVATION. `src/common/BuiltinRegistry.cpp` holds `kBuiltinParams` — the
ONE built-in parameter table, the table every built-in gate reads for a
member's parameter NAME (§22.5), parameter TYPE (§5.1) and parameter
NULLABILITY (§7.3).  Its §9.6 `String` rows, its §2.5 `DateTime` /
`TimeSpan` rows and the §2.3–§7.4 `File` / `Directory` / `Path` /
`Environment` / `Encoding` / `TextWriter` / `Math` / `Json*` /
`StringBuilder` / `FileStream` / `CString` / `WString` / `Array` /
`LinkedList` / `Shared` rows carry the DECLARED PARAMETER TYPES of the
standard library's own listing, transcribed by hand.

This ratchet is the CHECK on that transcription: it parses the standard
library out of `docs/Bezel_Standard_Library.md` — and, for the exception
hierarchy's base types, the SPECIFICATION's §31.4 out of
`docs/Bezel_Language_Spec.md`, which is where those types are declared —
and asserts that every declared form's parameter TYPES and NULLABILITY are
the ones the table carries.  A change to the document therefore fails the
gate until the table is brought back into step with it — so the table is a
CHECKED transcription of the document, not a second opinion about it.

It is a BUILD-FREE SOURCE ratchet: it reads the document and the source as
text.  It compiles nothing.  It is wired into `scripts/gate_full_cycle.sh`
beside `check_string_arity_table.py` and `check_string_mint_methods.py`, the
two ratchets that check the other §9.6 transcriptions the same way.

WHY THE TABLE AND NOT A NEW ONE.  A hand-maintained SIXTH mirror of the
standard library is not acceptable (the R36 campaign-2 anchor's ruling), and
this ratchet is what makes the ONE table safe to extend: the derivation is
mechanical, and the check is mechanical too.  The ratchet is the "WHO CHECKS
IT IN" half of the answer.

WHAT IT CHECKS, AND WHAT IT DOES NOT (fix #8013).  fix #8013 measured that
this checker covered THREE blocks (`String` / `DateTime` / `TimeSpan`) while
the #8005 commit had transcribed FIFTY-FOUR rows over the `File` /
`Directory` / `Path` / `Environment` / `Json*` / `StringBuilder` /
`Encoding` / `TextWriter` / `Math` heads — so the ratchet could not see a
single one of the rows it was supposed to guard (a corrupted `File`
row still exited 0 with "the table agrees with the standard library").
The covered set is now the twenty-two standard-library blocks and the
three specification blocks below — every one of the fifty-four #8005 rows
is inside it — and, the other half of #8013, the checker PRINTS the gap it
still has: the blocks it does not cover, and the positions whose declared
type the vocabulary cannot name.  A checker that says "the table agrees
with the standard library" while covering three of twenty-five blocks is
worse than one that says which blocks it does not cover.

Exit codes:
  0  the table agrees with the standard library over the covered blocks
  1  they disagree
  2  the checker could not parse its inputs (FAIL CLOSED — never a green)
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STDLIB = os.path.join(REPO, "docs", "Bezel_Standard_Library.md")
SPEC = os.path.join(REPO, "docs", "Bezel_Language_Spec.md")
REGISTRY = os.path.join(REPO, "src", "common", "BuiltinRegistry.cpp")

# ── the standard library's blocks ──────────────────────────────────────────

# A method declaration inside a class block:
#   public [pure|override|static|partial|async|abstract|virtual] method
#       Name(params): Ret;
# The parameter text is captured so the declared types can be derived from
# it.  `operator` declarations are not methods.  A GENERIC method spells its
# type parameters between the name and the parameter list
# (`Min<T>(a: T, b: T)`), and those type parameters are part of the
# declaration — the parameters' declared types are read against them.
#
# fix #8013: the modifier list carries `abstract` and `virtual` too.  The
# §3.2 `Encoding` block and the §7.4 `TextWriter` block declare their
# members `abstract`, and a declaration the parser cannot see is a
# declaration the ratchet cannot check — the blindness fix #8013 is about.
METHOD_RE = re.compile(
    r"^\s*public\s+(?:(?:pure|override|static|partial|async|abstract|virtual)"
    r"\s+)*method\s+"
    r"(?!operator\b)([A-Za-z_][A-Za-z0-9_]*)(?:<[^>]*>)?\s*\("
)

# A CONSTRUCTOR declaration inside a class block:
#   public [pure|override|static|partial] constructor(params);
# The table keys a constructor row on the EMPTY member name (the one table
# answers both the member and the constructor question with), so the
# constructor's parameter list is read under that key.
CTOR_RE = re.compile(
    r"^\s*public\s+(?:(?:pure|override|static|partial)\s+)*constructor\s*\("
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


def ctor_params(text):
    """The parameter text of a CONSTRUCTOR declaration on this line."""
    i = text.index("(")
    depth = 1
    j = i + 1
    while j < len(text):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
        j += 1
    die("unterminated parameter list on a constructor declaration")

# The blocks this ratchet checks.
#
# (the class declaration that opens the block, the CLASS name the block
#  ends with, the head the table is keyed by)
#
# fix #8013 (v0.350): the twenty blocks below are the ones the table's own
# rows live in.  Every head the table keys a row on and the standard library
# declares a block for is here; the heads the table carries but the
# standard library has no block for are REPORTED as known-uncovered (the
# `KNOWN-UNCOVERED` list the checker prints), never silently ignored.
BLOCKS = [
    # the §9.6 / §2.5 blocks the checker has always covered
    ("public class String:", "String", "string"),
    ("public record DateTime:", "DateTime", "DateTime"),
    ("public record TimeSpan:", "TimeSpan", "TimeSpan"),
    # fix #8013: the blocks the #8005 commit transcribed
    ("public static class File", "File", "File"),                  # §7.3
    ("public static class Directory", "Directory", "Directory"),  # §7.4
    ("public static class Path", "Path", "Path"),                  # §7.4
    ("public static class Environment", "Environment", "Environment"),  # §2.4
    ("public abstract class Encoding", "Encoding", "Encoding"),   # §3.2
    ("public abstract class TextWriter", "TextWriter", "TextWriter"),  # §7.4
    ("public static class Math", "Math", "Math"),                  # §2.6
    ("public class JsonSerializer:", "JsonSerializer", "JsonSerializer"),  # §2.15
    ("public class JsonDeserializer:", "JsonDeserializer", "JsonDeserializer"),
    ("public class BinaryDeserializer:", "BinaryDeserializer",
     "BinaryDeserializer"),
    ("public class StringBuilder", "StringBuilder", "StringBuilder"),  # §3.1
    ("public class FileStream:", "FileStream", "FileStream"),     # §7.1
    ("public class CString", "CString", "CString"),               # §10.1
    ("public class WString", "WString", "WString"),               # §10.2
    ("public class Array<T>:", "Array", "Array"),                 # §4.4
    ("public class LinkedList<T>:", "LinkedList", "LinkedList"),  # §4.12
    ("public class Shared<T>:", "Shared", "Shared"),              # §4.19
    ("public class TimeoutException:", "TimeoutException", "TimeoutException"),
    ("public class ArgumentOutOfRangeException:", "ArgumentOutOfRangeException",
     "ArgumentOutOfRangeException"),
]

# The blocks whose authority is the SPECIFICATION and not the standard
# library's own listing.  Spec §31.4 declares the exception hierarchy's
# base types, and `docs/Bezel_Standard_Library.md` has NO block for them —
# so a ratchet that read only the standard library could not see the three
# constructor rows the #8005 commit transcribed from the spec.
#
#   (the class declaration that opens the block, the CLASS name the block
#    ends with, the head the table is keyed by)
SPEC_BLOCKS = [
    ("public class Exception", "Exception", "Exception"),
    ("public class ArgumentException:", "ArgumentException", "ArgumentException"),
]

# The RECORDs whose POSITIONAL parameters are their constructor's — spec
# §31.4's `ExceptionInfo(TypeName: string, Message: string, StackTrace:
# string)`.  The table keys the row on the empty member name, the way it
# keys every constructor row.
SPEC_RECORDS = ["ExceptionInfo"]


def covered_heads():
    """The table heads this ratchet checks.

    The standard library's blocks and the SPECIFICATION's blocks (spec
    §31.4's exception hierarchy, which the standard library has no block
    for) together name every head the comparison walks.
    """
    return (set(b[2] for b in BLOCKS) | set(b[2] for b in SPEC_BLOCKS) |
            set(SPEC_RECORDS))

# ── the ONE built-in parameter table ───────────────────────────────────────

# A row in the table:
#   {"string", "Trim", 1, 1, 0x01, {"chars", ...}, {kBPTEnumerableChar, ...},
#    "Trim(chars: IEnumerable<char>): string"}
#
# A CONSTRUCTOR row spells the member name EMPTY (the table answers the
# member question and the constructor question with one lookup, so the
# constructor's member name is ""), which the older spelling of this
# pattern did not accept — fix #8013 widened it, and with it the twenty
# CONSTRUCTOR rows of the table came into the ratchet's sight at all.
ROW_RE = re.compile(
    r'\{\s*"(?P<head>[A-Za-z_][A-Za-z0-9_]*)"\s*,\s*'
    r'"(?P<member>[A-Za-z_][A-Za-z0-9_]*)?"\s*,\s*'
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
    # fix #8005 (stdlib §2.15, §5.1, v0.350): the JSON agents' `options`
    # parameter — a NAMED (record) type.  The bit is a vocabulary EXTENSION
    # (one more declared type the table can name); it relaxes nothing the
    # checker decides.
    "kBPTJsonOptions": "JsonOptions",
    # fix #7958 (stdlib §4.8–§4.10, §5.1, v0.350): the hash collections'
    # `comparer` parameter — a NAMED interface type.  fix #8013 added the
    # bit to the vocabulary so the TABLE PARSER can read the row that
    # carries it: a bit the parser does not know is a hard parse failure
    # (exit 2), and the row is on a head this ratchet does not cover, so
    # the name is never compared against a declaration.
    "kBPTComparer": "IEqualityComparer<T>",
}

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
    # fix #8013: the §10.1 `CString` block declares `FromBytes(bytes:
    # Array<byte>)` beside `From(s: string)`, and the table carries a row
    # for `From` only — the `FromBytes` surface is the §10.1 factory's own
    # and the compiler's FFI gate owns its argument type.
    ("CString", "FromBytes"):
        "stdlib §10.1 declares `bytes: Array<byte>`, and the table carries "
        "no row for the `FromBytes` factory — the FFI gate owns that "
        "surface's argument type",
}

# The CONSTRUCTOR forms the standard library declares and the table
# deliberately carries NO row for, each with the reason.  Spec §31.4: every
# standard exception type declares `constructor(message: string)`, and the
# registry answers that ONE form from the hierarchy rather than from a row
# per name (`builtinCtorParamNameCandidates`), so a new built-in exception
# cannot silently lose its named-argument surface.
#
#   (head, argument count) -> why the form carries no row
CTOR_FORMS_NOT_TRANSCRIBED = {
    ("TimeoutException", 1):
        "spec §31.4 declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
    ("ArgumentOutOfRangeException", 1):
        "spec §31.4 declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
    ("Exception", 1):
        "spec §31.4 declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
    ("ArgumentException", 1):
        "spec §31.4 declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
}

# The heads whose MEMBER surface the table carries NO row for at all (only
# the CONSTRUCTOR surface), each with the reason.  The block is still
# checked — for the CONSTRUCTOR rows the table does carry — but the
# document's member declarations are not demanded of a table that carries
# none of them.  Without this the ratchet would report every member of
# §4.4's `Array` and §4.12's `LinkedList` as "not transcribed", which says
# nothing the table does not already say by carrying no row for them.
MEMBERS_NOT_TRANSCRIBED = {
    "Array": "the table carries NO member row for this head — only its "
             "CONSTRUCTOR surface; §4.4's member surface is the collection "
             "protocol's, not the parameter table's",
    "LinkedList": "the table carries NO member row for this head — only its "
                  "CONSTRUCTOR surface",
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
    # fix #8013: the §2.5 `DateTime` CONSTRUCTOR's eighth position — the row
    # the old spelling of the table pattern could not even READ (a
    # constructor row spells its member name EMPTY).
    ("DateTime", "", 7):
        "stdlib §2.5 declares `kind: DateTimeKind`, an ENUM, and the "
        "vocabulary names the scalar and interface kinds only",
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
    # ── fix #8013: the positions the #8005 transcription left with NO type
    # opinion.  Each is listed with the reason, so the gap is VISIBLE in the
    # checker's own output instead of silent.
    ("File", "WriteAllBytes", 1):
        "stdlib §7.3 declares `contents: Array<byte>` AND `contents: "
        "ReadOnlySpan<byte>` (two overloads), and the vocabulary names the "
        "scalar and the `char` collections only — the two `contents` types "
        "have no bit between them",
    ("File", "ReadAllTextAsync", 1):
        "stdlib §7.3 declares `token: CancellationToken`, a STRUCT the "
        "vocabulary does not name",
    ("File", "WriteAllTextAsync", 2):
        "stdlib §7.3 declares `token: CancellationToken`, a STRUCT the "
        "vocabulary does not name",
    ("Encoding", "GetBytes", 1):
        "stdlib §3.2 declares `destination: Span<byte>`, and `Span<byte>` has "
        "no bit in the vocabulary",
    ("Encoding", "GetString", 0):
        "stdlib §3.2 declares `bytes: Array<byte>` / `ReadOnlySpan<byte>` / "
        "`ReadOnlyArraySpan<byte>` (three overloads), and the byte-span types "
        "have no bit in the vocabulary",
    ("Math", "Min", 0):
        "stdlib §2.6 declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Min", 1):
        "stdlib §2.6 declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Max", 0):
        "stdlib §2.6 declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Max", 1):
        "stdlib §2.6 declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Clamp", 0):
        "stdlib §2.6 declares `value: T` / `low: T` / `high: T`, TYPE "
        "PARAMETERS, and the vocabulary names the concrete kinds only",
    ("Math", "Clamp", 1):
        "stdlib §2.6 declares `value: T` / `low: T` / `high: T`, TYPE "
        "PARAMETERS, and the vocabulary names the concrete kinds only",
    ("Math", "Clamp", 2):
        "stdlib §2.6 declares `value: T` / `low: T` / `high: T`, TYPE "
        "PARAMETERS, and the vocabulary names the concrete kinds only",
    ("Math", "Sign", 0):
        "stdlib §2.6 declares `x: T`, a TYPE PARAMETER, and the vocabulary "
        "names the concrete kinds only",
    ("Math", "DivRem", 0):
        "stdlib §2.6 declares `a: int` / `a: long` (two overloads), and the "
        "overload grid owns the integer width — the position carries no "
        "single declared type",
    ("Math", "DivRem", 1):
        "stdlib §2.6 declares `b: int` / `b: long` (two overloads), and the "
        "overload grid owns the integer width — the position carries no "
        "single declared type",
    ("Math", "DivRem", 2):
        "stdlib §2.6 declares `out remainder: int` / `out remainder: long`, "
        "an `out` SLOT (§22.3) whose type is the out-variable machinery's",
    ("StringBuilder", "Append", 0):
        "stdlib §3.1 declares SEVEN `Append` overloads (`string` / `char` / "
        "`Object` / `bool` / the numeric primitives / `ReadOnlyStringSpan`) "
        "and the §3.1 member gate can express exactly ONE declared type at a "
        "position — a bit set here would make that gate read `string` and "
        "REFUSE the six other declared overloads",
    ("StringBuilder", "Insert", 1):
        "stdlib §3.1 declares exactly ONE `Insert` form, but #6807's own "
        "dedicated check already rejects a non-`string` value at that "
        "position — a bit set here would give one argument two diagnostics, "
        "which the table's own comment calls noise",
    ("StringBuilder", "AppendFormat", 1):
        "stdlib §3.1 declares `args: Array<Object>`, and the object ARRAY has "
        "no bit in the vocabulary",
    ("FileStream", "", 1):
        "stdlib §7.1 declares `mode: FileMode`, an ENUM, and the vocabulary "
        "names the scalar and interface kinds only",
    ("FileStream", "", 2):
        "stdlib §7.1 declares `access: FileAccess`, an ENUM, and the "
        "vocabulary names the scalar and interface kinds only",
    ("JsonDeserializer", "", 0):
        "stdlib §2.15 declares `input: ReadOnlySpan<byte>` / "
        "`ReadOnlyArraySpan<byte>` / `string` (three overloads), and the "
        "byte-span types have no bit in the vocabulary",
    ("BinaryDeserializer", "", 0):
        "stdlib §2.15 declares `input: ReadOnlySpan<byte>` / "
        "`ReadOnlyArraySpan<byte>` (two overloads), and the byte-span types "
        "have no bit in the vocabulary",
    ("LinkedList", "", 0):
        "stdlib §4.12 declares `source: IEnumerable<T>`, a TYPE PARAMETER, "
        "and the vocabulary names the concrete kinds only",
    ("Shared", "", 0):
        "stdlib §4.19 declares `owned value: T`, a TYPE PARAMETER, and the "
        "vocabulary names the concrete kinds only",
    ("TimeoutException", "", 1):
        "stdlib §2.5 declares `inner: Exception`, a CLASS, and the "
        "vocabulary names the scalar and interface kinds only",
    ("Exception", "", 1):
        "spec §31.4 declares `cause: Exception`, a CLASS, and the vocabulary "
        "names the scalar and interface kinds only",
    ("Array", "", 0):
        "stdlib §4.4 declares `capacity: int`; the capacity form's own "
        "admission is owned by #6893's dedicated check, and the table "
        "carries no type opinion there",
    ("Array", "", 1):
        "stdlib §4.4 declares `fill: T`, a TYPE PARAMETER, and the "
        "vocabulary names the concrete kinds only",
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

# The positions where the table's §7.3 NULLABILITY mask and the standard
# library's spelling DISAGREE, and the checker does NOT fail on them.  The
# table is the SUBJECT of this ratchet, not its property: a lane that finds
# such a disagreement REPORTS it (here, in the known-uncovered list, and in
# a follow-up issue) rather than editing the table to make the ratchet
# green.  Each entry says what the disagreement is and why it has no
# consequence today.
#
# The three positions below are §3.1's `StringBuilder.Insert` / `Remove`
# VALUE-TYPE positions: the document spells them `int` (not `int?`), and the
# table's mask leaves the bit unasserted.  Spec §7.2 makes `int?` a legal
# optional value type, so the document's spelling IS a nullability claim —
# but the §5.1 kind gate (`kBPTInt`) refuses a non-`int` argument at those
# positions, so no null reaches them and the mask's bit is not what decides
# the argument.  The mask is nevertheless INCONSISTENT with the rest of the
# table (every other `int` parameter the table carries asserts its bit), so
# the disagreement is listed rather than hidden.
NULLABILITY_DEVIATION = {
    ("StringBuilder", "Insert", 0):
        "the table's §3.1 `Insert` row leaves the `index: int` position's "
        "§7.3 bit UNASSERTED (the mask is 0x02 — the VALUE position only) "
        "while stdlib §3.1 spells `index: int` without `?`; the DECLARED "
        "TYPE there is `int` and the §5.1 kind gate refuses a non-`int` "
        "argument at that position, so no null reaches it",
    ("StringBuilder", "Remove", 0):
        "the table's §3.1 `Remove` row leaves the `startIndex: int` position's "
        "§7.3 bit UNASSERTED (the mask is 0x00) while stdlib §3.1 spells "
        "`startIndex: int` without `?`; the DECLARED TYPE there is `int` and "
        "the §5.1 kind gate refuses a non-`int` argument at that position, "
        "so no null reaches it",
    ("StringBuilder", "Remove", 1):
        "the table's §3.1 `Remove` row leaves the `length: int` position's "
        "§7.3 bit UNASSERTED (the mask is 0x00) while stdlib §3.1 spells "
        "`length: int` without `?`; the DECLARED TYPE there is `int` and the "
        "§5.1 kind gate refuses a non-`int` argument at that position, so "
        "no null reaches it",
}

# The declared types the document declares in a NOTE BESIDE its listing —
# stdlib §7.4's \"#1355\": per-type `Write` / `WriteLine` overloads for
# `bool` and the numeric primitives, which the listing does not spell out
# as separate declarations.  Without these the table's `kinds` column would
# read as a set of types the document never declares.
#
#   (head, member, position) -> the types the note declares
DOC_NOTE_TYPES = {
    ("TextWriter", "Write", 0): frozenset([
        "bool", "byte", "sbyte", "short", "ushort", "int", "uint", "long",
        "ulong", "float", "double", "decimal"]),
    ("TextWriter", "WriteLine", 0): frozenset([
        "bool", "byte", "sbyte", "short", "ushort", "int", "uint", "long",
        "ulong", "float", "double", "decimal"]),
}

# The reason each `DOC_NOTE_TYPES` entry exists, for the known-uncovered
# report.
DOC_NOTE_REASON = {
    ("TextWriter", "Write", 0):
        "stdlib §7.4's #1355 note — \"per-type Write/WriteLine overloads for "
        "bool and the numeric primitives\"",
    ("TextWriter", "WriteLine", 0):
        "stdlib §7.4's #1355 note — \"per-type Write/WriteLine overloads for "
        "bool and the numeric primitives\" (the `char` overload is `Write`'s "
        "alone, and the table agrees)",
}

# The heads the table keys a row on that the checker does NOT cover, each
# with the reason it cannot be covered.  fix #8013: the gap is printed, not
# silent.
KNOWN_UNCOVERED_HEADS = {
    "Boolean": "stdlib §9.5's `Boolean` block declares no member whose "
               "declared parameter type the table carries a row for; the "
               "primitive heads' rows are the §9.1–§9.5 parse / format "
               "surface, transcribed by #7421 and not yet re-checked here",
    "Byte": "stdlib §9.1 declares the integer types' shapes by ANALOGY to "
            "`Int32` (\"the others have analogous shapes\"), so there is no "
            "`Byte` block to compare a row against",
    "Char": "stdlib §9.5's `Char` block is not yet in the checker's covered "
            "set",
    "Convert": "stdlib §2.3's `Convert` block is not yet in the checker's "
               "covered set",
    "DateOnly": "stdlib §2.5's `DateOnly` block is not yet in the checker's "
                 "covered set",
    "Debug": "stdlib §11's `Debug` block is not yet in the checker's covered "
             "set",
    "Decimal": "stdlib §9.4's `Decimal` block is not yet in the checker's "
               "covered set",
    "Double": "stdlib §9.3's `Double` block is not yet in the checker's "
              "covered set",
    "ArgumentOutOfRangeException": "stdlib §2.5 declares the block, and it "
                                   "is not yet in the checker's covered set",
    "Float": "stdlib §9.3's `Float` block is not yet in the checker's "
             "covered set",
    "Guid": "stdlib §2.8's `Guid` block is not yet in the checker's covered "
            "set",
    "Int16": "stdlib §9.1 declares the integer types' shapes by ANALOGY to "
             "`Int32`, so there is no `Int16` block to compare a row against",
    "Int32": "stdlib §9.1's `Int32` block is not yet in the checker's "
             "covered set",
    "Int64": "stdlib §9.1 declares the integer types' shapes by ANALOGY to "
             "`Int32`, so there is no `Int64` block to compare a row against",
    "SByte": "stdlib §9.1 declares the integer types' shapes by ANALOGY to "
             "`Int32`, so there is no `SByte` block to compare a row against",
    "TimeOnly": "stdlib §2.5's `TimeOnly` block is not yet in the checker's "
                "covered set",
    "UInt16": "stdlib §9.1 declares the integer types' shapes by ANALOGY to "
              "`Int32`, so there is no `UInt16` block to compare a row "
              "against",
    "UInt32": "stdlib §9.1 declares the integer types' shapes by ANALOGY to "
              "`Int32`, so there is no `UInt32` block to compare a row "
              "against",
    "UInt64": "stdlib §9.1 declares the integer types' shapes by ANALOGY to "
              "`Int32`, so there is no `UInt64` block to compare a row "
              "against",
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


def is_out_slot(param):
    """Is this parameter an `out` SLOT (§22.3)?

    An `out` slot's type is the out-variable machinery's, not the
    declaration's — the declaration's own type is not what the argument
    carries.  The position therefore carries NO declared-type opinion, in
    either direction: the ratchet neither demands one nor forbids one.
    """
    words = param[0].split()
    return bool(words) and words[0] == "out"


def parse_block(lines, opener, cls):
    """The declared members of one block, as {member: [overload]}.

    A CONSTRUCTOR is keyed on the EMPTY member name, the same key the
    table uses for its constructor rows.
    """
    start = None
    for i, line in enumerate(lines):
        if line.startswith(opener):
            start = i
            break
    if start is None:
        return None
    # The block ends at its own `end <Class>;` — a declaration AFTER it
    # belongs to another class.
    end = None
    for j in range(start + 1, len(lines)):
        if lines[j].strip() == "end %s;" % cls:
            end = j
            break
    if end is None:
        return None
    out = {}
    i = start
    while i <= end:
        line = lines[i]
        m = METHOD_RE.match(line)
        ctor = None if m else CTOR_RE.match(line)
        if not m and not ctor:
            i += 1
            continue
        member = m.group(1) if m else ""
        # A declaration may span several lines — its parameter list is
        # closed on a later one.
        buf = line
        while not balanced(buf) and i + 1 <= end:
            i += 1
            buf += " " + lines[i].strip()
        if m:
            params = method_params(buf, member)
            if params is None:
                i += 1
                continue
        else:
            params = ctor_params(buf)
        out.setdefault(member, []).append(parse_params(params))
        i += 1
    return out


def parse_record(lines, name):
    """The POSITIONAL parameters of a record declaration, as one overload.

    A record's positional parameters are its constructor's, and the table
    keys the row on the EMPTY member name — the way it keys every
    constructor row.
    """
    prefix = "public record %s(" % name
    for i, line in enumerate(lines):
        if not line.startswith(prefix):
            continue
        buf = line
        j = i
        while not balanced(buf) and j + 1 < len(lines):
            j += 1
            buf += " " + lines[j].strip()
        return parse_params(ctor_params(buf))
    return None


def parse_stdlib(text, spec_text=None):
    """The declared members of each checked block.

    Returns {(head, member): [overload]} where an overload is a list of
    (name, type, has_default).
    """
    out = {}
    lines = text.splitlines()
    for opener, cls, head in BLOCKS:
        block = parse_block(lines, opener, cls)
        if block is None:
            die("no %r declaration found in the stdlib" % opener)
        for member, overloads in block.items():
            out.setdefault((head, member), []).extend(overloads)
    if spec_text is not None:
        spec_lines = spec_text.splitlines()
        for opener, cls, head in SPEC_BLOCKS:
            block = parse_block(spec_lines, opener, cls)
            if block is None:
                die("no %r declaration found in the spec" % opener)
            for member, overloads in block.items():
                out.setdefault((head, member), []).extend(overloads)
        for name in SPEC_RECORDS:
            params = parse_record(spec_lines, name)
            if params is None:
                die("no `public record %s(` declaration found in the spec"
                    % name)
            out.setdefault((name, ""), []).append(params)
    return out


# ── the table ──────────────────────────────────────────────────────────────

def parse_table(text):
    """The table's rows, as {(head, member, total): [(kinds, nn, required)]}.

    `kinds` is the SET of declared type names at each position; a position
    with no bit is the empty set (no type opinion).
    """
    out = {}
    for m in ROW_RE.finditer(text):
        head = m.group("head")
        member = m.group("member") or ""
        total = int(m.group("total"))
        required = int(m.group("required"))
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
        out.setdefault((head, member, total), []).append((kinds, nn, required))
    return out


# ── the comparison ─────────────────────────────────────────────────────────

def declared_at(stdlib, head, member, arity):
    """The declared types at each position of `member` at `arity`.

    The union over the standard library's overloads of that argument count:
    a position whose declared overload set is `char` OR `string` is the union
    of the two.  An `out` SLOT contributes NO declared type (§22.3), and a
    position the document declares in a NOTE (stdlib §7.4's #1355 note)
    contributes the types that note declares.
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
            if is_out_slot(params[p]):
                kinds.append(frozenset())
                continue
            # the declared type at position `p` of THIS overload: `T?` is the
            # type `T`, and the NULLABILITY is read separately.
            kinds.append(frozenset([params[p][1].rstrip("?")]) |
                         DOC_NOTE_TYPES.get((head, member, p), frozenset()))
        if acc is None:
            acc = kinds
        else:
            merged = []
            for i in range(len(acc)):
                merged.append(acc[i] | kinds[i])
            acc = merged
    return acc


def declared_forms(stdlib):
    """The (head, member, arity) forms the ratchet compares.

    A form is a DECLARED form of a member (or of a constructor — the empty
    member name) at one argument count.  A no-argument form carries no type
    to check, and the forms the table deliberately carries no row for are
    not compared (each with its reason, in the lists above).
    """
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
            if member and head in MEMBERS_NOT_TRANSCRIBED:
                continue
            if not member and (head, arity) in CTOR_FORMS_NOT_TRANSCRIBED:
                continue
            yield (head, member, arity)


def covering_rows(table, head, member, arity):
    """The table's rows that admit `arity` arguments of `member`.

    A MEMBER row is keyed on the EXACT argument count (the §5.1 admission
    gate reads `total == argCount`); a CONSTRUCTOR row admits every count
    from `required` to `total` (the §7.3 nullability gate's own rule).

    Each row is returned as (key, index, kinds, nn), so the report can say
    how many DISTINCT rows the ratchet actually read.
    """
    out = []
    for key, rows in table.items():
        h, m, total = key
        if h != head or m != member:
            continue
        for idx, (kinds, nn, required) in enumerate(rows):
            if member:
                if total != arity:
                    continue
            else:
                if not (required <= arity <= total):
                    continue
            out.append((key, idx, kinds, nn))
    return out


def compare(stdlib, table):
    """The disagreements between the standard library and the table."""
    errors = []

    # 1. Every declared form with a parameter must be transcribed.
    for (head, member, arity) in declared_forms(stdlib):
        rows = covering_rows(table, head, member, arity)
        if not rows:
            errors.append(
                "stdlib declares `%s%s` at %d argument(s) but the table "
                "carries no row for it"
                % (head, ("." + member) if member else "'s constructor",
                   arity))
            continue
        want = declared_at(stdlib, head, member, arity)
        if want is None:
            continue
        # the union of the table's rows that admit this argument count
        got = [set() for _ in want]
        nn_union = 0
        nn_all = 0xFF
        for _key, _idx, kinds, nn in rows:
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
                    "stdlib declares `%s%s` at position %d as %s, but the "
                    "table also declares `%s` there"
                    % (head, ("." + member) if member else "'s constructor",
                       i, " / ".join(sorted(want_set)), e))
            missing = want_set - got_set
            if missing and (head, member, i) not in NO_OPINION:
                errors.append(
                    "stdlib declares `%s%s` at position %d as %s, but the "
                    "table carries no type opinion there"
                    % (head, ("." + member) if member else "'s constructor",
                       i, " / ".join(sorted(want_set))))
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
                        "stdlib declares `%s%s` at position %d as a "
                        "NULLABLE type (`%s`), but the table marks it "
                        "non-nullable"
                        % (head, ("." + member) if member else
                           "'s constructor", i,
                           "/".join(sorted(want_set)) or "?"))
            else:
                if not (nn_all & (1 << i)) and \
                        (head, member, i) not in NULLABLE_ALLOWED and \
                        (head, member, i) not in NULLABILITY_DEVIATION:
                    errors.append(
                        "stdlib declares `%s%s` at position %d as a "
                        "NON-nullable type, but the table marks it "
                        "nullable"
                        % (head, ("." + member) if member else
                           "'s constructor", i))

    # 2. Every row the table carries for a checked head must be a form the
    #    standard library declares.
    for (head, member, total), rows in sorted(table.items()):
        if head not in covered_heads():
            continue
        if (head, member) in NOT_TRANSCRIBED:
            continue
        if not stdlib.get((head, member)):
            errors.append(
                "the table carries a row for `%s%s` at %d argument(s), which "
                "stdlib declares no member of"
                % (head, ("." + member) if member else "'s constructor",
                   total))
            continue
        if total == 0:
            continue
        want = declared_at(stdlib, head, member, total)
        if want is None:
            errors.append(
                "the table carries a row for `%s%s` at %d argument(s), which "
                "is no declared form of that member"
                % (head, ("." + member) if member else "'s constructor",
                   total))

    return errors


# ── the known-uncovered report (fix #8013) ────────────────────────────────

def known_uncovered(stdlib, table):
    """The gaps the checker KNOWS it has, as a list of (kind, text).

    A checker that reports "the table agrees with the standard library"
    while covering a handful of the table's blocks is worse than one that
    says which blocks it does not cover — so the gaps are computed and
    printed, never assumed.
    """
    out = []
    covered = covered_heads()

    # (a) the table's heads the checker does not cover at all
    heads = {}
    for (head, member, total), rows in table.items():
        heads.setdefault(head, 0)
        heads[head] += len(rows)
    for head in sorted(heads):
        if head in covered:
            continue
        reason = KNOWN_UNCOVERED_HEADS.get(
            head, "the standard library declares this type in a block, but "
                  "this checker does not parse that block — the head's rows "
                  "are NOT checked")
        out.append(("block", "`%s` — %d row(s) of the table are NOT checked; "
                              "%s" % (head, heads[head], reason)))

    # (b) the positions of a COVERED block whose declared type the
    #     vocabulary cannot name
    for (head, member, i), reason in sorted(NO_OPINION.items()):
        if head not in covered:
            continue
        if not stdlib.get((head, member)):
            continue
        out.append(("position",
                    "`%s%s` at position %d — %s"
                    % (head, ("." + member) if member else "'s constructor",
                       i, reason)))

    # (c) the members the document declares and the table carries no row for
    for (head, member), reason in sorted(NOT_TRANSCRIBED.items()):
        if head not in covered:
            continue
        out.append(("member", "`%s%s` — %s"
                    % (head, ("." + member) if member else "'s constructor",
                       reason)))

    # (d) the document's NOTE-declared types the table carries
    for (head, member, i), reason in sorted(DOC_NOTE_REASON.items()):
        if head not in covered:
            continue
        out.append(("note", "`%s.%s` at position %d — the declared types the "
                             "table carries here come from a NOTE, not from a "
                             "spelled declaration: %s"
                    % (head, member, i, reason)))

    # (e) the heads whose MEMBER surface the table carries no row for
    for head, reason in sorted(MEMBERS_NOT_TRANSCRIBED.items()):
        if head not in covered:
            continue
        out.append(("members", "`%s` — %s" % (head, reason)))

    # (f) the CONSTRUCTOR forms the table deliberately carries no row for
    for (head, arity), reason in sorted(CTOR_FORMS_NOT_TRANSCRIBED.items()):
        if head not in covered:
            continue
        out.append(("form", "`%s`'s constructor at %d argument(s) — %s"
                    % (head, arity, reason)))

    # (g) the positions where the table's §7.3 mask and the document's
    #     spelling DISAGREE, which the checker reports rather than fails on
    for (head, member, i), reason in sorted(NULLABILITY_DEVIATION.items()):
        if head not in covered:
            continue
        out.append(("deviation",
                    "`%s%s` at position %d — the table's §7.3 mask and the "
                    "document's spelling DISAGREE, and the checker reports it "
                    "rather than failing on it (the table is the subject of "
                    "this ratchet, not its property): %s"
                    % (head, ("." + member) if member else "'s constructor",
                       i, reason)))

    return out


def coverage(stdlib, table):
    """What the ratchet actually READS, as (forms, rows).

    `forms` is the number of declared forms (a member or a constructor at
    one argument count) the comparison walks; `rows` is the number of
    DISTINCT table rows those forms read.  The two numbers are what the
    checker's verdict is about — they are what it can catch a drift in.
    """
    forms = 0
    rows = set()
    for (head, member, arity) in declared_forms(stdlib):
        forms += 1
        for key, idx, _kinds, _nn in covering_rows(table, head, member, arity):
            rows.add((key, idx))
    return forms, len(rows)


# ── the self-test ─────────────────────────────────────────────────────────

# One mutation per covered block, each asserted DETECTED.  A new block with
# no mutation is a block that can be silently wrong — fix #8013's third
# requirement.
#
#   (head, member, arity, position, the mutation)
#
# The mutations are:
#   ("extra", type)  — the position's kind set becomes {type}
#   ("missing",)     — the position's kind set becomes empty
#   ("nonnullable",) — the position's non-nullable bit is SET
#   ("nullable",)    — the position's non-nullable bit is CLEARED
SELF_TEST_MUTATIONS = [
    # the blocks the checker has always covered
    ("string", "Contains", 1, 0, ("extra", "int")),
    ("string", "Contains", 1, 0, ("missing",)),
    ("string", "Trim", 1, 0, ("nullable",)),
    ("DateTime", "AddYears", 1, 0, ("extra", "string")),
    ("TimeSpan", "FromTicks", 1, 0, ("extra", "int")),
    # fix #8013: one mutation for each newly covered block
    ("File", "ReadAllText", 1, 0, ("extra", "int")),
    ("Directory", "Delete", 2, 1, ("extra", "int")),
    ("Path", "Combine", 2, 0, ("extra", "int")),
    ("Environment", "Set", 2, 1, ("nonnullable",)),
    ("Encoding", "GetByteCount", 1, 0, ("extra", "int")),
    ("TextWriter", "Write", 1, 0, ("extra", "long")),
    ("Math", "Sqrt", 1, 0, ("extra", "int")),
    ("JsonSerializer", "", 1, 0, ("extra", "int")),
    ("JsonDeserializer", "", 2, 1, ("extra", "int")),
    ("BinaryDeserializer", "", 2, 1, ("extra", "int")),
    ("StringBuilder", "AppendLine", 1, 0, ("extra", "int")),
    ("FileStream", "", 2, 0, ("extra", "int")),
    ("CString", "From", 1, 0, ("extra", "int")),
    ("WString", "From", 1, 0, ("extra", "int")),
    ("Array", "", 1, 0, ("extra", "long")),
    ("LinkedList", "", 1, 0, ("extra", "int")),
    ("Shared", "", 1, 0, ("extra", "int")),
    ("TimeoutException", "", 2, 0, ("extra", "int")),
    ("ArgumentOutOfRangeException", "", 2, 0, ("extra", "int")),
    # the three heads whose authority is the SPECIFICATION (spec §31.4)
    ("Exception", "", 2, 0, ("extra", "int")),
    ("ArgumentException", "", 2, 0, ("extra", "int")),
    ("ExceptionInfo", "", 3, 0, ("extra", "int")),
]


def apply_mutation(table, head, member, arity, position, mutation):
    """A copy of `table` with one position mutated."""
    mutated = {k: list(v) for k, v in table.items()}
    key = (head, member, arity)
    rows = mutated.get(key)
    if not rows:
        die("the self-test names a row the table does not carry: %r" % (key,))
    kinds, nn, required = rows[0]
    new_kinds = list(kinds)
    new_nn = nn
    kind = mutation[0]
    if kind == "extra":
        new_kinds[position] = frozenset([mutation[1]])
    elif kind == "missing":
        new_kinds[position] = frozenset()
    elif kind == "nonnullable":
        new_nn |= (1 << position)
    elif kind == "nullable":
        new_nn &= ~(1 << position) & 0xFF
    else:
        die("the self-test names an unknown mutation %r" % (mutation,))
    rows[0] = (new_kinds, new_nn, required)
    return mutated


def self_test():
    """The checker must DETECT a mismatch — a checker that cannot is a
    green that means nothing (the R35 #7144 reviewer's "S")."""
    stdlib = parse_stdlib(read(STDLIB), read(SPEC))
    table = parse_table(read(REGISTRY))
    if compare(stdlib, table):
        die("the checker reports a mismatch on the tree it lives in", 1)
    # (1) a declared type the table does not carry.
    mutated = {k: list(v) for k, v in table.items()}
    rows = mutated[("string", "Contains", 1)]
    rows[0] = ([frozenset()] + list(rows[0][0][1:]), rows[0][1], rows[0][2])
    if not compare(stdlib, mutated):
        die("the checker does not detect a MISSING declared type", 1)
    # (2) a declared type the standard library does not name.
    mutated = {k: list(v) for k, v in table.items()}
    rows = mutated[("string", "Contains", 1)]
    rows[0] = ([frozenset(["int"])] + list(rows[0][0][1:]), rows[0][1],
               rows[0][2])
    if not compare(stdlib, mutated):
        die("the checker does not detect a WRONG declared type", 1)
    # (3) a nullability the standard library does not spell.
    mutated = {k: list(v) for k, v in table.items()}
    rows = mutated[("string", "Trim", 1)]
    rows[0] = (rows[0][0], 0x00, rows[0][2])
    if not compare(stdlib, mutated):
        die("the checker does not detect a WRONG nullability", 1)
    # (4) fix #8013: EVERY covered block must have a mutation the checker
    #     asserts is DETECTED — including the blocks the #8005 commit
    #     transcribed, which the checker could not see at all before.
    covered = covered_heads()
    seen = set()
    for (head, member, arity, position, mutation) in SELF_TEST_MUTATIONS:
        seen.add(head)
        mutated = apply_mutation(table, head, member, arity, position,
                                 mutation)
        errors = compare(stdlib, mutated)
        if not errors:
            die("the checker does not detect a corrupted `%s.%s` at position "
                "%d (%s)" % (head, member, position, mutation[0]), 1)
        named = [e for e in errors
                 if ("`%s.%s`" % (head, member)) in e or
                 ("`%s'" % head) in e]
        if not named:
            die("the checker detects a corrupted `%s.%s` at position %d but "
                "does not NAME the member: %s"
                % (head, member, position, "; ".join(errors)), 1)
    missing = covered - seen
    if missing:
        die("the covered blocks %s have no self-test mutation — a block with "
            "no mutation can be silently wrong"
            % ", ".join("`%s`" % h for h in sorted(missing)), 1)
    sys.stdout.write("check_builtin_declared_params: self-test OK "
                     "(%d mutation(s) over %d block(s))\n"
                     % (len(SELF_TEST_MUTATIONS), len(covered)))
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    stdlib = parse_stdlib(read(STDLIB), read(SPEC))
    table = parse_table(read(REGISTRY))
    errors = compare(stdlib, table)
    gaps = known_uncovered(stdlib, table)
    if errors:
        sys.stderr.write("check_builtin_declared_params: %d disagreement(s) "
                         "between the standard library (and the "
                         "specification) and the ONE built-in parameter "
                         "table:\n" % len(errors))
        for e in errors:
            sys.stderr.write("  - %s\n" % e)
        sys.exit(1)
    # The known-uncovered report comes FIRST, so the LAST line is the
    # verdict the gate prints.
    sys.stdout.write("check_builtin_declared_params: KNOWN-UNCOVERED — the "
                     "checker does NOT check the following (%d item(s)):\n"
                     % len(gaps))
    for kind, text in gaps:
        sys.stdout.write("  - [%s] %s\n" % (kind, text))
    covered_rows = sum(len(rows) for (h, m, t), rows in table.items()
                        if h in covered_heads())
    forms, rows_read = coverage(stdlib, table)
    sys.stdout.write("check_builtin_declared_params: the table agrees with "
                     "the standard library over %d standard-library block(s) "
                     "and %d specification block(s), %d declared form(s) and "
                     "%d row(s) read (%d row(s) sit in the covered blocks); "
                     "%d known-uncovered item(s) are listed above\n"
                     % (len(BLOCKS), len(SPEC_BLOCKS) + len(SPEC_RECORDS),
                        forms, rows_read, covered_rows, len(gaps)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
