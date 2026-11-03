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

WHAT IT CHECKS, AND WHAT IT DOES NOT (fix #8021, fix #8023).  Two more
columns and one more kind of claim are now checked.  The §-CITATION of every
block — the section of the standard library (or of the specification) its
declaration was transcribed from — used to be PROSE: a trailing comment on
the `BLOCKS` rows, or a sentence inside a reason.  Nothing could check it,
and eleven of them were WRONG while the checker printed them as provenance
in every CI run.  The citation is now DATA (`BLOCK_CITATIONS`), the reasons
DERIVE their §-numbers from it, and `check_citations` verifies every one of
them against the documents: the cited section must EXIST, and it must
DECLARE the block it claims.  The `required` column — the DECLARED MINIMUM
of an overload, the count of its parameters that declare no default — was
never compared at all, while the checker's output named it; it is now
compared, and the rows where the table and the document disagree are
REPORTED, with the document's own declaration as the provenance.  #8034
corrected those rows in the TABLE, so the registry of known disagreements is
gone and a `required` disagreement now FAILS the checker.
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
#
# fix #8021: the §-section each block's declaration comes from is NOT a
# trailing comment here — a comment cannot be checked, and four of them were
# WRONG while being printed in this checker's CI output.  The citation lives
# in `BLOCK_CITATIONS` below, which the checker READS and VERIFIES against
# the document: the cited section must exist, and it must declare the block.
BLOCKS = [
    # the §9.6 / §2.5 blocks the checker has always covered
    ("public class String:", "String", "string"),
    ("public record DateTime:", "DateTime", "DateTime"),
    ("public record TimeSpan:", "TimeSpan", "TimeSpan"),
    # fix #8013: the blocks the #8005 commit transcribed
    ("public static class File", "File", "File"),
    ("public static class Directory", "Directory", "Directory"),
    ("public static class Path", "Path", "Path"),
    ("public static class Environment", "Environment", "Environment"),
    ("public abstract class Encoding", "Encoding", "Encoding"),
    ("public abstract class TextWriter", "TextWriter", "TextWriter"),
    ("public static class Math", "Math", "Math"),
    ("public class JsonSerializer:", "JsonSerializer", "JsonSerializer"),
    ("public class JsonDeserializer:", "JsonDeserializer", "JsonDeserializer"),
    ("public class BinaryDeserializer:", "BinaryDeserializer",
     "BinaryDeserializer"),
    ("public class StringBuilder", "StringBuilder", "StringBuilder"),
    ("public class FileStream:", "FileStream", "FileStream"),
    ("public class CString", "CString", "CString"),
    ("public class WString", "WString", "WString"),
    ("public class Array<T>:", "Array", "Array"),
    ("public class LinkedList<T>:", "LinkedList", "LinkedList"),
    ("public class Shared<T>:", "Shared", "Shared"),
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
    # fix #7999 (stdlib §3.2, §4.14, §5.1, v0.350): the byte span —
    # `Span<byte>`, stdlib §3.2's `Encoding.GetBytes` destination.
    "kBPTSpanByte": "Span<byte>",
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

# ── the §-citations (fix #8021) ────────────────────────────────────────────

# The §-section each block's DECLARATION is transcribed from.
#
# fix #8021: the citations used to be PROSE — trailing comments on the
# `BLOCKS` rows and sentences inside the reasons below — so nothing could
# check them, and eleven of them were WRONG while the checker printed them
# as provenance in every CI run.  Here they are DATA, and `check_citations`
# reads this map against the documents: the cited section must EXIST, and it
# must contain the declaration it claims.
#
#   head -> (document, section number, "declares" | "mentions")
#
# "declares" — the section's body carries the type's own declaration.
# "mentions" — the document has NO block of its own for the type (the §9.1
#              integer types are declared by ANALOGY to `Int32`), so the
#              section must at least NAME the type.
#
# The UNCOVERED heads are here too: the known-uncovered list PRINTS their
# §-numbers, so they are citations as much as the covered ones' are.
BLOCK_CITATIONS = {
    # the twenty-two standard-library blocks and the three specification ones
    "string": ("stdlib", "9.6", "declares"),
    "DateTime": ("stdlib", "2.5", "declares"),
    "TimeSpan": ("stdlib", "2.5", "declares"),
    "File": ("stdlib", "7.3", "declares"),
    "Directory": ("stdlib", "7.4", "declares"),
    "Path": ("stdlib", "7.4", "declares"),
    "Environment": ("stdlib", "2.4", "declares"),
    "Encoding": ("stdlib", "3.2", "declares"),
    "TextWriter": ("stdlib", "7.4", "declares"),
    "Math": ("stdlib", "2.2", "declares"),
    "JsonSerializer": ("stdlib", "2.15", "declares"),
    "JsonDeserializer": ("stdlib", "2.15", "declares"),
    "BinaryDeserializer": ("stdlib", "2.15", "declares"),
    "StringBuilder": ("stdlib", "3.1", "declares"),
    "FileStream": ("stdlib", "7.2", "declares"),
    "CString": ("stdlib", "10.1", "declares"),
    "WString": ("stdlib", "10.2", "declares"),
    "Array": ("stdlib", "4.4", "declares"),
    "LinkedList": ("stdlib", "4.6", "declares"),
    "Shared": ("stdlib", "2.16", "declares"),
    "TimeoutException": ("stdlib", "6.1", "declares"),
    "ArgumentOutOfRangeException": ("stdlib", "2.5", "declares"),
    "Exception": ("spec", "31.4", "declares"),
    "ArgumentException": ("spec", "31.4", "declares"),
    "ExceptionInfo": ("spec", "31.4", "declares"),
    # the heads the checker does NOT cover — their §-numbers are printed too
    "Boolean": ("stdlib", "9.4", "declares"),
    "Byte": ("stdlib", "9.1", "mentions"),
    "Char": ("stdlib", "9.5", "declares"),
    "Convert": ("stdlib", "2.3", "declares"),
    "DateOnly": ("stdlib", "2.5", "declares"),
    "Debug": ("stdlib", "8.2", "declares"),
    "Decimal": ("stdlib", "9.3", "declares"),
    "Double": ("stdlib", "9.2", "declares"),
    "Float": ("stdlib", "9.2", "declares"),
    "Guid": ("stdlib", "2.6", "declares"),
    "Int16": ("stdlib", "9.1", "mentions"),
    "Int32": ("stdlib", "9.1", "declares"),
    "Int64": ("stdlib", "9.1", "mentions"),
    "SByte": ("stdlib", "9.1", "mentions"),
    "TimeOnly": ("stdlib", "2.5", "declares"),
    "UInt16": ("stdlib", "9.1", "mentions"),
    "UInt32": ("stdlib", "9.1", "mentions"),
    "UInt64": ("stdlib", "9.1", "mentions"),
}

# The name a head's DECLARATION spells — the class name the block ends with.
HEAD_CLASS = dict(
    [(b[2], b[1]) for b in BLOCKS] +
    [(b[2], b[1]) for b in SPEC_BLOCKS] +
    [(n, n) for n in SPEC_RECORDS])

# The LANGUAGE-SPECIFICATION sections the checker names that are not a
# head's own block — the RULES it cites beside the declarations.
#
#   name -> (document, section number)
NAMED_CITATIONS = {
    "exceptions": ("spec", "31.4"),   # the standard exception types
    "equals_slot": ("spec", "8.3"),   # `Equals` and `GetHashCode`
    "kind": ("spec", "5.1"),          # the §5.1 admissibility question
    "nullability": ("spec", "7.3"),   # assigning `null` to a target
    "out_slot": ("spec", "22.3"),     # parameter modifiers
    "optional": ("spec", "22.5"),     # optional and named parameters
    "variance": ("spec", "12.9"),     # variance
}


def sec(head):
    """A head's own §-citation, as `stdlib §2.2`.

    The reasons below DERIVE their §-numbers from this, so a reason can
    never name a section the map does not claim: ONE source of truth, and
    the map is the thing `check_citations` verifies against the document.
    """
    doc, number, _kind = BLOCK_CITATIONS[head]
    return "%s §%s" % (doc, number)


def named(name):
    """A cited language-specification section, as `spec §31.4`."""
    doc, number = NAMED_CITATIONS[name]
    return "%s §%s" % (doc, number)


def secno(head):
    """A head's own §-NUMBER, as `§3.1` — for a sentence that already
    names the document."""
    return "§%s" % BLOCK_CITATIONS[head][1]


def _bind(reasons):
    """Fill each reason's `{sec}` with the head's OWN §-citation.

    fix #8021: a reason that names a section it cannot check is a pointer
    that can be wrong without anybody noticing — and eleven of them were,
    while the checker printed them as provenance.  Every §-number a reason
    names now comes from `BLOCK_CITATIONS` / `NAMED_CITATIONS`, which
    `check_citations` verifies against the documents.
    """
    out = {}
    for key, reason in reasons.items():
        head = key if isinstance(key, str) else key[0]
        fields = dict((k, named(k)) for k in NAMED_CITATIONS)
        fields["sec"] = sec(head)
        fields["secno"] = secno(head)
        if isinstance(reason, tuple):
            # a (value, value, reason) entry: only the REASON names a section
            out[key] = tuple(reason[:-1]) + (reason[-1].format(**fields),)
        else:
            out[key] = reason.format(**fields)
    return out

# The members the standard library declares and the table carries NO row
# for, each with the reason.  A member that is neither transcribed nor
# listed here FAILS the ratchet.
NOT_TRANSCRIBED = _bind({
    ("DateTime", "CompareTo"):
        "{sec} declares `other: DateTime`, a RECORD, and the vocabulary "
        "names the scalar and interface kinds only",
    ("DateTime", "Subtract"):
        "{sec} declares `other: DateTime` / `span: TimeSpan`, RECORDS, "
        "and the vocabulary names the scalar and interface kinds only",
    ("DateTime", "Add"):
        "{sec} declares `span: TimeSpan`, a RECORD, and the vocabulary "
        "names the scalar and interface kinds only",
    ("TimeSpan", "CompareTo"):
        "{sec} declares `other: TimeSpan`, a RECORD, and the vocabulary "
        "names the scalar and interface kinds only",
    # fix #8013: the §10.1 `CString` block declares `FromBytes(bytes:
    # Array<byte>)` beside `From(s: string)`, and the table carries a row
    # for `From` only — the `FromBytes` surface is the §10.1 factory's own
    # and the compiler's FFI gate owns its argument type.
    ("CString", "FromBytes"):
        "{sec} declares `bytes: Array<byte>`, and the table carries "
        "no row for the `FromBytes` factory — the FFI gate owns that "
        "surface's argument type",
})

# The CONSTRUCTOR forms the standard library declares and the table
# deliberately carries NO row for, each with the reason.  Spec §31.4: every
# standard exception type declares `constructor(message: string)`, and the
# registry answers that ONE form from the hierarchy rather than from a row
# per name (`builtinCtorParamNameCandidates`), so a new built-in exception
# cannot silently lose its named-argument surface.
#
#   (head, argument count) -> why the form carries no row
CTOR_FORMS_NOT_TRANSCRIBED = _bind({
    ("TimeoutException", 1):
        "{exceptions} declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
    ("ArgumentOutOfRangeException", 1):
        "{exceptions} declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
    ("Exception", 1):
        "{exceptions} declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
    ("ArgumentException", 1):
        "{exceptions} declares `constructor(message: string)` for every "
        "standard exception type, and the registry answers that one form "
        "from the exception hierarchy rather than from a row per name",
})

# The heads whose MEMBER surface the table carries NO row for at all (only
# the CONSTRUCTOR surface), each with the reason.  The block is still
# checked — for the CONSTRUCTOR rows the table does carry — but the
# document's member declarations are not demanded of a table that carries
# none of them.  Without this the ratchet would report every member of
# §4.4's `Array` and §4.6's `LinkedList` as "not transcribed", which says
# nothing the table does not already say by carrying no row for them.
MEMBERS_NOT_TRANSCRIBED = _bind({
    "Array": "the table carries NO member row for this head — only its "
             "CONSTRUCTOR surface; {sec}'s member surface is the collection "
             "protocol's, not the parameter table's",
    "LinkedList": "the table carries NO member row for this head — only its "
                  "CONSTRUCTOR surface",
})

# The positions the table DELIBERATELY carries no type opinion for, each with
# the reason.
NO_OPINION = _bind({
    ("DateTime", "FromTicks", 1):
        "{sec} declares `kind: DateTimeKind`, an ENUM, and the "
        "vocabulary names the scalar and interface kinds only",
    ("DateTime", "TryParse", 1):
        "{sec} declares `out result: DateTime`, an `out` SLOT ({out_slot}); "
        "the slot's type is the out-variable machinery's, not the "
        "declaration's",
    # fix #8013: the §2.5 `DateTime` CONSTRUCTOR's eighth position — the row
    # the old spelling of the table pattern could not even READ (a
    # constructor row spells its member name EMPTY).
    ("DateTime", "", 7):
        "{sec} declares `kind: DateTimeKind`, an ENUM, and the "
        "vocabulary names the scalar and interface kinds only",
    ("string", "From", 0):
        "{sec} declares THREE one-argument forms — `From(value: char)`, "
        "`From(value: CString)` and `From(value: WString)` — and the FFI types "
        "have no bit in the vocabulary; the compiler's own `String.From` gate "
        "owns that form's argument type",
    ("string", "FromCStringPointer", 0):
        "{sec} declares `p: IntPtr` and `IntPtr` has no bit in the "
        "vocabulary",
    ("string", "FromWStringPointer", 0):
        "{sec} declares `p: IntPtr` and `IntPtr` has no bit in the "
        "vocabulary",
    ("string", "Join", 0):
        "the dedicated `string.Join` gate (fix #7150) already asks the {kind} "
        "question against the DECLARED `separator: string`",
    ("string", "Join", 1):
        "the dedicated `string.Join` gate (fix #7167 / #7191) already asks the "
        "{kind} / {variance} question against the DECLARED `items: "
        "IEnumerable<string>`, and a weaker second test would refuse a legal "
        "argument that gate admits",
    # ── fix #8013: the positions the #8005 transcription left with NO type
    # opinion.  Each is listed with the reason, so the gap is VISIBLE in the
    # checker's own output instead of silent.
    ("File", "WriteAllBytes", 1):
        "{sec} declares `contents: Array<byte>` AND `contents: "
        "ReadOnlySpan<byte>` (two overloads), and the vocabulary names the "
        "scalar and the `char` collections only — the two `contents` types "
        "have no bit between them",
    ("File", "ReadAllTextAsync", 1):
        "{sec} declares `token: CancellationToken`, a STRUCT the "
        "vocabulary does not name",
    ("File", "WriteAllTextAsync", 2):
        "{sec} declares `token: CancellationToken`, a STRUCT the "
        "vocabulary does not name",
    ("Encoding", "GetBytes", 1):
        "{sec} declares `destination: Span<byte>`, and `Span<byte>` has "
        "no bit in the vocabulary",
    ("Encoding", "GetString", 0):
        "{sec} declares `bytes: Array<byte>` / `ReadOnlySpan<byte>` / "
        "`ReadOnlyArraySpan<byte>` (three overloads), and the byte-span types "
        "have no bit in the vocabulary",
    ("Math", "Min", 0):
        "{sec} declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Min", 1):
        "{sec} declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Max", 0):
        "{sec} declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Max", 1):
        "{sec} declares `a: T` / `b: T`, TYPE PARAMETERS, and the "
        "vocabulary names the concrete kinds only",
    ("Math", "Clamp", 0):
        "{sec} declares `value: T` / `low: T` / `high: T`, TYPE "
        "PARAMETERS, and the vocabulary names the concrete kinds only",
    ("Math", "Clamp", 1):
        "{sec} declares `value: T` / `low: T` / `high: T`, TYPE "
        "PARAMETERS, and the vocabulary names the concrete kinds only",
    ("Math", "Clamp", 2):
        "{sec} declares `value: T` / `low: T` / `high: T`, TYPE "
        "PARAMETERS, and the vocabulary names the concrete kinds only",
    ("Math", "Sign", 0):
        "{sec} declares `x: T`, a TYPE PARAMETER, and the vocabulary "
        "names the concrete kinds only",
    ("Math", "DivRem", 0):
        "{sec} declares `a: int` / `a: long` (two overloads), and the "
        "overload grid owns the integer width — the position carries no "
        "single declared type",
    ("Math", "DivRem", 1):
        "{sec} declares `b: int` / `b: long` (two overloads), and the "
        "overload grid owns the integer width — the position carries no "
        "single declared type",
    ("Math", "DivRem", 2):
        "{sec} declares `out remainder: int` / `out remainder: long`, "
        "an `out` SLOT ({out_slot}) whose type is the out-variable machinery's",
    ("StringBuilder", "Append", 0):
        "{sec} declares SEVEN `Append` overloads (`string` / `char` / "
        "`Object` / `bool` / the numeric primitives / `ReadOnlyStringSpan`) "
        "and the {secno} member gate can express exactly ONE declared type at a "
        "position — a bit set here would make that gate read `string` and "
        "REFUSE the six other declared overloads",
    ("StringBuilder", "Insert", 1):
        "{sec} declares exactly ONE `Insert` form, but #6807's own "
        "dedicated check already rejects a non-`string` value at that "
        "position — a bit set here would give one argument two diagnostics, "
        "which the table's own comment calls noise",
    ("StringBuilder", "AppendFormat", 1):
        "{sec} declares `args: Array<Object>`, and the object ARRAY has "
        "no bit in the vocabulary",
    ("FileStream", "", 1):
        "{sec} declares `mode: FileMode`, an ENUM, and the vocabulary "
        "names the scalar and interface kinds only",
    ("FileStream", "", 2):
        "{sec} declares `access: FileAccess`, an ENUM, and the "
        "vocabulary names the scalar and interface kinds only",
    ("JsonDeserializer", "", 0):
        "{sec} declares `input: ReadOnlySpan<byte>` / "
        "`ReadOnlyArraySpan<byte>` / `string` (three overloads), and the "
        "byte-span types have no bit in the vocabulary",
    ("BinaryDeserializer", "", 0):
        "{sec} declares `input: ReadOnlySpan<byte>` / "
        "`ReadOnlyArraySpan<byte>` (two overloads), and the byte-span types "
        "have no bit in the vocabulary",
    ("LinkedList", "", 0):
        "{sec} declares `source: IEnumerable<T>`, a TYPE PARAMETER, "
        "and the vocabulary names the concrete kinds only",
    ("Shared", "", 0):
        "{sec} declares `owned value: T`, a TYPE PARAMETER, and the "
        "vocabulary names the concrete kinds only",
    ("TimeoutException", "", 1):
        "{sec} declares `inner: Exception`, a CLASS, and the "
        "vocabulary names the scalar and interface kinds only",
    ("Exception", "", 1):
        "{sec} declares `cause: Exception`, a CLASS, and the vocabulary "
        "names the scalar and interface kinds only",
    ("Array", "", 0):
        "{sec} declares `capacity: int`; the capacity form's own "
        "admission is owned by #6893's dedicated check, and the table "
        "carries no type opinion there",
    ("Array", "", 1):
        "{sec} declares `fill: T`, a TYPE PARAMETER, and the "
        "vocabulary names the concrete kinds only",
})

# The positions where the table carries a type the standard library does not
# name, each with the §-sentence that admits it.
EXTRA_ALLOWED = _bind({
    ("string", "Equals", 0):
        "{sec}'s own comment on `Equals(other: string)` — \"a `string?` "
        "or `Object?` argument reaches it through the slot\" — and {equals_slot}'s "
        "Object slot (\"the `Object` slot `Equals` has a second spelling, at "
        "the overriding class's own type\")",
})

# The positions the table declares NULLABLE where the standard library spells
# the parameter without `?`, each with the §-sentence that admits it.
NULLABLE_ALLOWED = _bind({
    ("string", "Equals", 0):
        "{sec}'s own comment on `Equals(other: string)` — \"a `string?` "
        "or `Object?` argument reaches it through the slot\" — so the "
        "one-argument form admits the null the two-argument form spells "
        "`other: string?`",
})

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
# optional value type, so the document's spelling IS a nullability claim.
#
# fix #8023: the GATE that decides these is NOT the §5.1 kind gate — the
# reason this text gave was WRONG.  `isBuiltinValueParamHead`
# (`src/sema/ExprTypecheck_Calls.cpp`) does NOT include `StringBuilder`, so
# the §5.1 kind gate never reaches these positions at all; the DEDICATED
# `ExprTC::checkBuiltinStringBuilderCall` gate refuses the null itself,
# through its `checkAssignable` path against the DECLARED `int` (declared
# non-nullable).  So no null reaches these positions, and the mask's bit is
# not what decides the argument.  The mask is nevertheless INCONSISTENT with
# the rest of the table (every other `int` parameter the table carries
# asserts its bit), so the disagreement is listed rather than hidden.
NULLABILITY_DEVIATION = _bind({
    # fix #8020 (stdlib §3.1, §7.3, v0.350): EMPTY — the three recorded
    # deviations are RETIRED.
    #
    # The entries this list held were:
    #   ("StringBuilder", "Insert", 0) — the `index: int` position's §7.3 bit
    #   ("StringBuilder", "Remove", 0) — the `startIndex: int` position's bit
    #   ("StringBuilder", "Remove", 1) — the `length: int` position's bit
    # and each of them recorded that the table's `nonNullable` mask left the
    # position UNASSERTED while stdlib §3.1 spells the parameter WITHOUT `?`.
    #
    # The table now states the document's own nullability (`Insert`'s mask is
    # 0x03 and `Remove`'s is 0x03), so the deviations no longer hold and the
    # checker's own retirement rule ("the recorded §7.3 deviation no longer
    # holds: the table now states the document's own nullability ... so this
    # entry can be deleted") says to delete them.
    #
    # THEY ARE DELETED, and that is the POINT of #8020: a recorded deviation
    # is a SUPPRESSION (the checker reports it and does not fail on it), so a
    # stale entry would HIDE a re-introduced deviation at exactly the
    # positions the ratchet is asked to cover — "reintroduce the deviation
    # (clear the §7.3 bit at one of these positions) and the checker must
    # exit non-zero and name the member and the position". With the entries
    # gone, the §7.3 comparison compares the mask against the document with
    # nothing suppressed, and the self-test's new mutation
    # ("StringBuilder", "Remove", 2, 0, ("nullable",)) PROVES it.
})

def nullability_deviation_holds(stdlib, table, head, member, i):
    """Does the recorded §7.3 deviation STILL hold — i.e. does the table's
    `nonNullable` mask still disagree with the DOCUMENT's spelling?

    fix #8020: the recorded deviations are a SUPPRESSION list: while a
    deviation holds, the checker reports it and does not fail on it (the
    table is the subject of this ratchet, not its property). Once the table
    is CORRECTED, the entry must be RETIRED — a stale suppression would hide
    a re-introduced deviation, which is exactly what the ratchet exists to
    catch. So the suppression is conditional on the deviation holding, and
    the retirement is reported.
    """
    rows = table.get((head, member))
    if not rows:
        return False
    nn_all = 0xFF
    for _key, _idx, _kinds, nn in rows:
        nn_all &= nn
    params = stdlib.get((head, member))
    if not params:
        return False
    nullable_doc = any(
        params[i][1].endswith("?")
        for params in stdlib[(head, member)]
        if len(params) > i)
    if nullable_doc:
        return bool(nn_all & (1 << i))
    return not (nn_all & (1 << i))


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
DOC_NOTE_REASON = _bind({
    ("TextWriter", "Write", 0):
        "{sec}'s #1355 note — \"per-type Write/WriteLine overloads for "
        "bool and the numeric primitives\"",
    ("TextWriter", "WriteLine", 0):
        "{sec}'s #1355 note — \"per-type Write/WriteLine overloads for "
        "bool and the numeric primitives\" (the `char` overload is `Write`'s "
        "alone, and the table agrees)",
})

# The heads the table keys a row on that the checker does NOT cover, each
# with the reason it cannot be covered.  fix #8013: the gap is printed, not
# silent.
KNOWN_UNCOVERED_HEADS = _bind({
    "Boolean": "{sec}'s `Boolean` block declares no member whose "
               "declared parameter type the table carries a row for; the "
               "primitive heads' rows are the parse / format surface "
               "surface, transcribed by #7421 and not yet re-checked here",
    "Byte": "{sec} declares the integer types' shapes by ANALOGY to "
            "`Int32` (\"the others have analogous shapes\"), so there is no "
            "`Byte` block to compare a row against",
    "Char": "{sec}'s `Char` block is not yet in the checker's covered "
            "set",
    "Convert": "{sec}'s `Convert` block is not yet in the checker's "
               "covered set",
    "DateOnly": "{sec}'s `DateOnly` block is not yet in the checker's "
                 "covered set",
    "Debug": "{sec}'s `Debug` block is not yet in the checker's covered "
             "set",
    "Decimal": "{sec}'s `Decimal` block is not yet in the checker's "
               "covered set",
    "Double": "{sec}'s `Double` block is not yet in the checker's "
              "covered set",
    # fix #8021: the `ArgumentOutOfRangeException` entry that used to be
    # here said the block "is not yet in the checker's covered set" — it
    # IS in it (`BLOCKS`: one of the twenty-five covered blocks), so the
    # entry was DEAD prose that also made a false claim about the covered
    # set.  Removed.
    "Float": "{sec}'s `Float` block is not yet in the checker's "
             "covered set",
    "Guid": "{sec}'s `Guid` block is not yet in the checker's covered "
            "set",
    "Int16": "{sec} declares the integer types' shapes by ANALOGY to "
             "`Int32`, so there is no `Int16` block to compare a row against",
    "Int32": "{sec}'s `Int32` block is not yet in the checker's "
             "covered set",
    "Int64": "{sec} declares the integer types' shapes by ANALOGY to "
             "`Int32`, so there is no `Int64` block to compare a row against",
    "SByte": "{sec} declares the integer types' shapes by ANALOGY to "
             "`Int32`, so there is no `SByte` block to compare a row against",
    "TimeOnly": "{sec}'s `TimeOnly` block is not yet in the checker's "
                "covered set",
    "UInt16": "{sec} declares the integer types' shapes by ANALOGY to "
              "`Int32`, so there is no `UInt16` block to compare a row "
              "against",
    "UInt32": "{sec} declares the integer types' shapes by ANALOGY to "
              "`Int32`, so there is no `UInt32` block to compare a row "
              "against",
    "UInt64": "{sec} declares the integer types' shapes by ANALOGY to "
              "`Int32`, so there is no `UInt64` block to compare a row "
              "against",
})


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

# A document's numbered section heading: `## 2.2 `Math`` and friends.
SECTION_RE = re.compile(r"^(#{2,4})\s+([0-9]+(?:\.[0-9]+)*)\s+(.*)$")

# A type's own declaration: `public class Foo`, `public record Foo(...)`,
# `public static class Foo`, `public class Foo<T>`, `public enum Foo`.
#
# The `.*?` is LAZY on purpose: a declaration line can carry a comment that
# mentions the word "class" ("`public class Shared<T>: IDisposable where
# T: class      // proper class types only;`"), and a greedy match would
# take the LAST "class" in the line — "types" — instead of the type's own.
DECL_RE = re.compile(r"^\s*(?:public|internal)\b.*?\b(?:class|record|enum|struct)"
                     r"\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\b")


def parse_sections(text):
    """The document's numbered sections, as [(number, title, body)].

    The `body` is the section's own text: from its heading up to the next
    heading, so a declaration that a section OWNS is inside it.
    """
    lines = text.splitlines()
    marks = []
    for i, line in enumerate(lines):
        mt = SECTION_RE.match(line)
        if mt:
            marks.append((i, mt.group(2), mt.group(3).strip()))
    out = []
    for k, (i, num, title) in enumerate(marks):
        end = marks[k + 1][0] if k + 1 < len(marks) else len(lines)
        out.append((num, title, "\n".join(lines[i:end])))
    return out


def check_citations(stdlib_text, spec_text, citations=None):
    """The §-citation of every block must be a section that EXISTS and that
    DECLARES the block.

    fix #8021: the citations used to be PROSE, so nothing could check them —
    and eleven of them were WRONG while the checker printed them as
    provenance in every CI run.  A citation is a pointer to the authority: a
    pointer that names a section which does not exist, or which does not
    declare the type, is a wrong pointer, and it is now an ERROR.
    """
    errors = []
    citations = BLOCK_CITATIONS if citations is None else citations
    documents = {"stdlib": parse_sections(stdlib_text),
                 "spec": parse_sections(spec_text)}

    def body_of(doc, number):
        for (num, _title, body) in documents[doc]:
            if num == number:
                return body
        return None

    # (1) every covered block carries a citation.
    for head in sorted(covered_heads()):
        if head not in citations:
            errors.append("the covered block `%s` records no §-citation" % head)

    # (2) every citation names a section that EXISTS.
    for head in sorted(citations):
        doc, number, _kind = citations[head]
        if body_of(doc, number) is None:
            errors.append("the §-citation of `%s` names %s §%s, which is not "
                          "a section of that document"
                          % (head, doc, number))

    # (3) every citation names a section that DECLARES (or, for the types
    #     the document declares by ANALOGY, MENTIONS) the head.
    for head in sorted(citations):
        doc, number, kind = citations[head]
        body = body_of(doc, number)
        if body is None:
            continue
        name = HEAD_CLASS.get(head, head)
        if kind == "declares":
            declared = False
            for line in body.splitlines():
                mt = DECL_RE.match(line)
                if mt and mt.group("name") == name:
                    declared = True
                    break
            if not declared:
                errors.append("the §-citation of `%s` names %s §%s, but that "
                              "section declares no `%s`"
                              % (head, doc, number, name))
        else:
            if not re.search(r"\b%s\b" % re.escape(name), body):
                errors.append("the §-citation of `%s` names %s §%s, but that "
                              "section does not mention `%s`"
                              % (head, doc, number, name))

    # (4) every language-specification section the checker names exists.
    for name in sorted(NAMED_CITATIONS):
        doc, number = NAMED_CITATIONS[name]
        if body_of(doc, number) is None:
            errors.append("the checker cites %s §%s (%s), which is not a "
                          "section of that document" % (doc, number, name))

    return errors


# THE `required` (DECLARED ARITY) COLUMN IS NOW COMPARED WITH THE DOCUMENT
# FOR EVERY COVERED ROW, and a disagreement FAILS the checker.
#
# #8023's ratchet made the comparison and found EIGHT rows whose `required`
# column contradicted the document's own declaration — the table's own
# contract (`src/common/BuiltinRegistry.cpp`) is that "`required` counts the
# LEADING parameters that declare no default, so one row answers for every
# admissible argument count of an overload with §22.5 optional parameters",
# and the `File.ReadAllTextAsync` comment spells it out: "The `required`
# column is the declared MINIMUM, not the arity."  Those rows were REPORTED
# rather than failed on, because the table is the SUBJECT of this ratchet:
# correcting a row is an edit to `src/common/BuiltinRegistry.cpp`, whose
# `required` column drives the §22.5 admission window.
#
# fix #8034 corrected all EIGHT rows in the table — `FileStream`'s
# constructor (`access` is DEFAULTED → 2), `Math.DivRem` (an `out` SLOT IS
# AN ARGUMENT, spec §22.3 → 3), `TextWriter.Write` / `TextWriter.WriteLine`
# (one parameter, no default → 1) and `string.IndexOf` / `PadLeft` /
# `PadRight` / `PadCenter` (`startIndex` / `paddingChar` are DEFAULTED → 1).
# The registry of KNOWN disagreements that served them is therefore GONE: a
# row whose `required` is not the document's declared minimum is now a
# CORRUPTION and FAILS the checker, exactly as a wrong parameter TYPE or a
# wrong §7.3 mask does.  Nothing here is weakened by the deletion — the
# COMPARISON is untouched, and the checker's own mutation leg (see
# `self_test`) still corrupts a `required` count on every covered block and
# asserts the corruption is DETECTED.


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


def declared_required(stdlib, head, member, arity):
    """The document's DECLARED MINIMUM for the form at `arity`.

    The minimum over the document's overloads of `head`/`member` that ADMIT
    `arity` arguments, of that overload's count of parameters that declare
    no default.  It is the registry's OWN admission rule read from the
    document's side: `required <= argCount <= total`.

    An `out` SLOT counts: it is an ARGUMENT (§22.3), so a form that takes
    one needs that argument.
    """
    overloads = stdlib.get((head, member))
    if not overloads:
        return None
    best = None
    for params in overloads:
        required = sum(1 for p in params if not p[2])
        if required <= arity <= len(params):
            if best is None or required < best:
                best = required
    return best


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
                        not nullability_deviation_holds(
                            stdlib, table, head, member, i):
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

    # 3. The `required` (DECLARED ARITY) column.  fix #8023: this column was
    #    never compared, and it names itself in the checker's own output.
    for (head, member, total), rows in sorted(table.items()):
        if head not in covered_heads():
            continue
        if (head, member) in NOT_TRANSCRIBED:
            continue
        want = declared_required(stdlib, head, member, total)
        if want is None:
            # the document declares no form at this argument count; the
            # comparison above reports that, and there is no document's
            # minimum to compare the column against.
            continue
        for _idx, (_kinds, _nn, required) in enumerate(rows):
            if required == want:
                continue
            # fix #8034: the `required` column of EVERY covered row is
            # compared with the document's declared minimum, and a
            # disagreement FAILS the checker — the eight rows that used to
            # disagree are corrected in the table itself.
            errors.append(
                "stdlib declares `%s%s` at %d argument(s) with %d required "
                "parameter(s), but the table's `required` column says %d"
                % (head, ("." + member) if member else "'s constructor",
                   total, want, required))

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
        # fix #8020: a RECORDED deviation is RETIRED once the table carries
        # the document's own value — the same shape as the `required`
        # deviation's retirement below — so a corrected row stops SUPPRESSING
        # the §7.3 comparison and a re-introduced deviation is caught.
        if not nullability_deviation_holds(stdlib, table, head, member, i):
            out.append(("deviation",
                        "`%s%s` at position %d — the recorded §7.3 deviation "
                        "no longer holds: the table now states the document's "
                        "own nullability (the parameter is declared WITHOUT "
                        "`?`), so this entry can be deleted"
                        % (head, ("." + member) if member else
                           "'s constructor", i)))
            continue
        out.append(("deviation",
                    "`%s%s` at position %d — the table's §7.3 mask and the "
                    "document's spelling DISAGREE, and the checker reports it "
                    "rather than failing on it (the table is the subject of "
                    "this ratchet, not its property): %s"
                    % (head, ("." + member) if member else "'s constructor",
                       i, reason)))

    # (h) fix #8034: the rows whose `required` (DECLARED ARITY) column
    #     disagreed with the document are CORRECTED in the table, so there
    #     is no known `required` gap left to report: a row whose `required`
    #     is not the document's declared minimum now FAILS the checker
    #     (`compare`), exactly as a wrong parameter TYPE or a wrong §7.3
    #     mask does.

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
    # fix #8020 (stdlib §3.1, §7.3, v0.350): the class the issue asks the
    # ratchet to cover — a CLEARED §7.3 bit at one of the three positions
    # whose nullability the table now states (the `startIndex: int`
    # position of `Remove`). The mutation clears the bit, so the table
    # disagrees with stdlib §3.1's spelling again, and the checker must
    # DETECT it.
    ("StringBuilder", "Remove", 2, 0, ("nullable",)),
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


def apply_mutation(table, head, member, arity, position, mutation, index=0):
    """A copy of `table` with one position mutated."""
    mutated = {k: list(v) for k, v in table.items()}
    key = (head, member, arity)
    rows = mutated.get(key)
    if not rows:
        die("the self-test names a row the table does not carry: %r" % (key,))
    kinds, nn, required = rows[index]
    new_kinds = list(kinds)
    new_nn = nn
    new_required = required
    kind = mutation[0]
    if kind == "extra":
        new_kinds[position] = frozenset([mutation[1]])
    elif kind == "missing":
        new_kinds[position] = frozenset()
    elif kind == "nonnullable":
        new_nn |= (1 << position)
    elif kind == "nullable":
        new_nn &= ~(1 << position) & 0xFF
    elif kind == "required":
        # fix #8023: the DECLARED ARITY column.
        new_required = mutation[1]
    else:
        die("the self-test names an unknown mutation %r" % (mutation,))
    rows[index] = (new_kinds, new_nn, new_required)
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

    # (5) fix #8021: a WRONG §-citation must be DETECTED.  The citations used
    #     to be prose, so a wrong one was invisible; now the checker reads
    #     the citation map and refuses a citation that names a section which
    #     does not exist, or which does not declare the block.
    citations = dict(BLOCK_CITATIONS)
    citations["Math"] = ("stdlib", "2.6", "declares")
    errors = check_citations(read(STDLIB), read(SPEC), citations)
    if not errors:
        die("the checker does not detect a WRONG §-citation", 1)
    named = [e for e in errors if "`Math`" in e]
    if not named:
        die("the checker detects a wrong §-citation but does not NAME the "
            "block: %s" % "; ".join(errors), 1)
    # a citation that names a section that does not exist at all.
    citations = dict(BLOCK_CITATIONS)
    citations["FileStream"] = ("stdlib", "7.99", "declares")
    errors = check_citations(read(STDLIB), read(SPEC), citations)
    if not errors:
        die("the checker does not detect a §-citation that names a section "
            "that does not exist", 1)
    # the real map must be clean.
    errors = check_citations(read(STDLIB), read(SPEC))
    if errors:
        die("the checker reports a wrong §-citation on the tree it lives in: "
            "%s" % "; ".join(errors), 1)

    # (6) fix #8023: a CORRUPTED `required` count must be DETECTED — for
    #     EVERY covered block.  The corrupted value is neither the
    #     document's declared minimum nor the value a KNOWN deviation
    #     records, so a known deviation cannot HIDE a corruption.
    for head in sorted(covered):
        candidates = [(key, idx) for key, rows in table.items()
                      if key[0] == head for idx in range(len(rows))]
        if not candidates:
            continue
        key, idx = candidates[0]
        required = table[key][idx][2]
        want = declared_required(stdlib, key[0], key[1], key[2])
        bad = required + 1
        if want is not None and bad == want:
            bad += 1
        mutated = apply_mutation(table, key[0], key[1], key[2], 0,
                                 ("required", bad), index=idx)
        errors = compare(stdlib, mutated)
        if not errors:
            die("the checker does not detect a corrupted `required` count "
                "(%d) for `%s`" % (bad, head), 1)
        named = [e for e in errors
                 if ("`%s.%s`" % (key[0], key[1])) in e or
                 ("`%s'" % key[0]) in e]
        if not named:
            die("the checker detects a corrupted `required` count for `%s` but "
                "does not NAME the member: %s" % (head, "; ".join(errors)), 1)
        if not any("`required` column" in e for e in named):
            die("the checker detects a corrupted `required` count for `%s` but "
                "does not NAME the column: %s" % (head, "; ".join(errors)), 1)

    sys.stdout.write("check_builtin_declared_params: self-test OK "
                     "(%d mutation(s) over %d block(s); the §-citations and "
                     "the `required` column are checked too)\n"
                     % (len(SELF_TEST_MUTATIONS), len(covered)))
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    stdlib_text = read(STDLIB)
    spec_text = read(SPEC)
    stdlib = parse_stdlib(stdlib_text, spec_text)
    table = parse_table(read(REGISTRY))
    errors = compare(stdlib, table)
    # fix #8021: the §-citations are CHECKED, not printed as prose — a
    # citation that names a section which does not exist, or which does not
    # declare the block, is an error and fails the ratchet.
    errors += check_citations(stdlib_text, spec_text)
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
                     "%d row(s) read (%d row(s) sit in the covered blocks) — "
                     "the parameter TYPE, the §7.3 NULLABILITY and the "
                     "`required` (DECLARED ARITY) columns are compared, and "
                     "the §-citation of all %d block(s) is verified against "
                     "the document; %d known-uncovered item(s) are listed "
                     "above, and every covered row's `required` column "
                     "matches the document's declared MINIMUM (a disagreement "
                     "FAILS this checker)\n"
                     % (len(BLOCKS), len(SPEC_BLOCKS) + len(SPEC_RECORDS),
                        forms, rows_read, covered_rows, len(BLOCK_CITATIONS),
                        len(gaps)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
