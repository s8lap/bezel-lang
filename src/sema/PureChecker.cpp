#include "sema/PureChecker.hpp"

#include <string>
#include <vector>

#include "common/BuiltinRegistry.hpp"
#include "common/OwnershipFacts.hpp"  // fix #3588: the ONE §28.9 trivial-dispose walk
#include "sema/NestedTypeWalk.hpp"  // fix #3597: the shared §11.6 nested-type walk
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"  // typeRefEqual (§22.6 exact match)
#include "sema/TypeRef.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::sema {

namespace {

// ---------------------------------------------------------------------------
// §22.15 rule 6 — the STANDARD LIBRARY's `pure` markers (the ONE source).
//
// WHY THIS TABLE EXISTS. The compiler knows a USER member's purity from its
// declaration (the `pure` modifier) and an IMPORTED member's from the
// artifact interface (`MetaMember::isPure` → `MemberSymbol::isPure`). A
// STANDARD-LIBRARY member has neither: the compiler-built-in classes carry
// NO AST declaration (`TypeRef::declaration` is null) and no artifact, so the
// callee resolves to NO symbol at all and `callTargetIsPure`'s final arm
// (`return true;` — "Intrinsic / builtin calls (no AST decl) are admitted")
// admitted EVERY standard-library call. The same gap admitted every
// standard-library CONSTRUCTION: the `NewExpr` arm rejected a class only
// when `rt->declaration->kind == Class`, and a built-in class has no
// declaration.
//
// THE SPEC'S ANSWER. §22.15 "Intrinsic purity (the stdlib contract)": "A
// standard-library member whose body is runtime-provided may carry `pure` …
// The marker is the library's *stated contract*, normative for the
// implementation … and trusted by the checker rather than verified from
// source." Rule 6 then admits a call iff the member is "`pure`-declared or
// pure-classified", and the prohibited list rejects "class construction —
// `new C(…)`" outright (admitted only at the throw operand).
//
// WHAT A ROW ADMITS — THREE SOURCES, one per `StdlibPurity` value, because
// §22.15 admits a built-in member access for three different reasons and
// rule 6's `pure` marker is only one of them:
//
//   * `Const` — §22.15 rule 1: "static field, `static readonly`, and `const`
//     reads … Reads are not effects". `Math.Pi`, `int.MaxValue`,
//     `double.NaN` are `static const` declarations (stdlib §9.1-§9.5; Math.Pi: §2.2),
//     not calls, so rule 6 does not reach them at all.
//   * `Declared` — §22.15 rule 6: the member carries the library's own
//     `pure` marker, transcribed from `docs/Bezel_Standard_Library.md`
//     (v0.355). Every row cites the section that declares it.
//   * `Classified` — §22.15 *Members classified by rule (synthesized
//     bodies)*: "an auto-property getter is pure (a backing-field read) …
//     a record's synthesized members — positional constructor,
//     `Equals`/`GetHashCode` (§9.2), `ToString`, `with`-copy — are pure iff
//     every constituent operation is pure … (for equality and hash: iff
//     every field's equality dispatch target is pure — primitives,
//     `string`, enums, and qualifying records …)". That reaches the
//     stored-slot getters of the stdlib's body records (`Range<T>`, stdlib
//     §9.8) and the synthesized `Equals`/`GetHashCode` of the primitive
//     records (stdlib §9.1: "`Equals(other: int): bool` — auto-generated
//     from `IEquatable<int>` per language spec §9.2 … The same convention
//     applies to every primitive declaration in §9.1–§9.5"). The
//     positional components of the built-in POSITIONAL records
//     (`ReadOnlySpan<T>(Buffer, Start, Length)`, stdlib §4.14;
//     `JsonOptions(…)`, §2.15; …) are classified too, but keyed by the
//     RECORD, not the member: `kStdlibPositionalRecords` below.
//
// THE KEY. A row is keyed by the type's STANDARD-LIBRARY name (the stdlib's
// own System-namespace spelling, stdlib §9: "declarations use the
// System-namespace PascalCase names (`Int32`, `String`, `Double`, …)") — so
// the keyword spelling and the System spelling of one type (`int` /
// `Int32`, spec §3.4's keyword aliases) read the SAME row — and the eight
// integer types and the two floating types key ONE family row each, because
// the library declares them once: stdlib §9.1 "`SByte`, `Byte`, `Int16`,
// `UInt16`, `Int32`, `UInt32`, `Int64`, `UInt64`. The example below shows
// `Int32`; the others have analogous shapes with their respective
// `MinValue`/`MaxValue`", and stdlib §9.2 "`Float` has the analogous shape".
// `purityTableHeadOf` does both normalisations.
//
// A standard-library member that is not in this table is neither const,
// `pure`-declared nor pure-classified, and §22.15 rule 6 rejects the call —
// the rule's "iff" reading.
//
// A note on the ARCHITECTURAL HOME: the compiler's ONE built-in table
// (`src/common/BuiltinRegistry.cpp`'s `kBuiltinParams`) transcribes only the
// members that carry a §22.5 parameter surface, so it cannot answer the
// question for a const or a property; a `pure` column there would still
// leave those rows here. The (head, member) keys below are the registry's
// own (System-spelled heads), so folding the table into it is a MOVE.
// ---------------------------------------------------------------------------
enum class StdlibPurity : unsigned char {
    Const,       // §22.15 rule 1 — a `static const` read
    Declared,    // §22.15 rule 6 — the library states `pure`
    Classified,  // §22.15 "Members classified by rule"
};

struct StdlibPureMember {
    std::string_view head;
    std::string_view member;
    StdlibPurity why;
};

// The two FAMILY keys: stdlib §9.1 and §9.2 each declare one shape for
// several types.
constexpr std::string_view kIntegerFamily = "<stdlib §9.1 integer>";
constexpr std::string_view kFloatingFamily = "<stdlib §9.2 floating>";

constexpr StdlibPurity kConst = StdlibPurity::Const;
constexpr StdlibPurity kDecl = StdlibPurity::Declared;
constexpr StdlibPurity kClsf = StdlibPurity::Classified;

constexpr StdlibPureMember kStdlibPureMembers[] = {
    // stdlib §2.2 `Math` — the two `static const` reads and the pure
    // arithmetic surface (`Min`/`Max`/`Clamp`/`Sign`/`DivRem` carry no
    // marker).
    {"Math", "Pi", kConst},          {"Math", "Tau", kConst},
    {"Math", "Abs", kDecl},          {"Math", "Sqrt", kDecl},
    {"Math", "Pow", kDecl},          {"Math", "Floor", kDecl},
    {"Math", "Ceiling", kDecl},      {"Math", "Round", kDecl},
    {"Math", "Truncate", kDecl},     {"Math", "Fmod", kDecl},
    {"Math", "IEEERemainder", kDecl},
    // stdlib §3.1 `StringBuilder` — the stored-extent read.
    {"StringBuilder", "Capacity", kDecl},
    // stdlib §3.3 `ReadOnlyStringSpan(Source, Start, Length)` — the `pure`
    // members (the three positional components are classified by
    // `kStdlibPositionalRecords` below; stdlib §3.3 declares no `Capacity`).
    {"ReadOnlyStringSpan", "IsEmpty", kDecl},
    {"ReadOnlyStringSpan", "Slice", kDecl},
    {"ReadOnlyStringSpan", "IndexOf", kDecl},
    {"ReadOnlyStringSpan", "Contains", kDecl},
    {"ReadOnlyStringSpan", "StartsWith", kDecl},
    {"ReadOnlyStringSpan", "EndsWith", kDecl},
    {"ReadOnlyStringSpan", "SequenceEqual", kDecl},
    {"ReadOnlyStringSpan", "ToString", kDecl},
    // stdlib §4.1 / §4.2 — the collection interfaces' Count (the one
    // `pure`-declared member of the borrowing and owning interfaces).
    {"IReadOnlyCollection", "Count", kDecl},
    {"IReadOnlyList", "Count", kDecl},
    {"ICollection", "Count", kDecl},
    {"IList", "Count", kDecl},
    {"IOwningCollection", "Count", kDecl},
    {"IOwningList", "Count", kDecl},
    // stdlib §4.8 `DictionaryEntry<K, V>(Key, Value)` — its two positional
    // components are classified by `kStdlibPositionalRecords` below.
    // stdlib §4.4 `Array<T>` / §4.5 `OwningArray<T>`.
    {"Array", "Count", kDecl},       {"Array", "Capacity", kDecl},
    {"OwningArray", "Count", kDecl}, {"OwningArray", "Capacity", kDecl},
    // stdlib §4.6 `LinkedList<T>` / §4.7 `OwningLinkedList<T>`.
    {"LinkedList", "Count", kDecl},      {"LinkedList", "FirstNode", kDecl},
    {"LinkedList", "LastNode", kDecl},   {"LinkedList", "First", kDecl},
    {"LinkedList", "Last", kDecl},
    {"OwningLinkedList", "Count", kDecl},
    {"OwningLinkedList", "FirstNode", kDecl},
    {"OwningLinkedList", "LastNode", kDecl},
    {"OwningLinkedList", "First", kDecl},
    {"OwningLinkedList", "Last", kDecl},
    // stdlib §4.8 `Dictionary<K,V>` / §4.9 `OwningDictionary<K,V>`.
    {"Dictionary", "Count", kDecl}, {"OwningDictionary", "Count", kDecl},
    // stdlib §4.10 `Set<T>` / `OwningSet<T>` — the `Count` they inherit from
    // §4.1's `IReadOnlyCollection<T>`.
    {"Set", "Count", kDecl}, {"OwningSet", "Count", kDecl},
    // stdlib §4.11 — the stack/queue surfaces (`Count` inherited; `Peek`
    // pure-declared on the borrowing and the owning family alike).
    {"Stack", "Count", kDecl},       {"Stack", "Peek", kDecl},
    {"OwningStack", "Count", kDecl}, {"OwningStack", "Peek", kDecl},
    {"Queue", "Count", kDecl},       {"Queue", "Peek", kDecl},
    {"OwningQueue", "Count", kDecl}, {"OwningQueue", "Peek", kDecl},
    // stdlib §4.14 `OwningBuffer<T>` — the immutable-once-allocated extent.
    {"OwningBuffer", "Length", kDecl},
    // stdlib §4.14 `Span<T>(Buffer, Start, Length)` /
    // `ReadOnlySpan<T>(Buffer, Start, Length)` — the positional components
    // are classified by `kStdlibPositionalRecords` below (their `IsEmpty`/
    // `Slice`/`IndexOf` carry no marker); these are the two bit-tier entry
    // points the library marks (`ReadOnlySpan<byte>.AsBitReader`,
    // `Span<byte>.AsBitWriter`; `Span<byte>.AsBitReader` delegates through
    // the unmarked `AsReadOnly` and is not `pure`).
    {"Span", "AsBitWriter", kDecl},
    {"ReadOnlySpan", "AsBitReader", kDecl},
    // stdlib §4.14 bit tier — `BitReader`/`BitWriter(Source, BitStart,
    // BitLength)`: the members its *Purity* paragraph names ("`IsEmpty`,
    // `IsByteAligned`, the three `Slice` overloads, and `AlignToByte` on
    // every type of this tier"); the components are classified below.
    {"BitReader", "IsEmpty", kDecl},
    {"BitReader", "IsByteAligned", kDecl}, {"BitReader", "Slice", kDecl},
    {"BitReader", "AlignToByte", kDecl},
    {"BitWriter", "IsEmpty", kDecl},
    {"BitWriter", "IsByteAligned", kDecl}, {"BitWriter", "Slice", kDecl},
    {"BitWriter", "AlignToByte", kDecl},
    // stdlib §4.15 `ArraySpan<T>` / `ReadOnlyArraySpan<T>(Source, Start,
    // Length)` and the mirrored bit tier `ArrayBitReader`/`ArrayBitWriter`
    // ("Every rule of §4.14's bit tier holds here verbatim"); the
    // components are classified below.
    {"ArraySpan", "AsBitWriter", kDecl},
    {"ReadOnlyArraySpan", "AsBitReader", kDecl},
    {"ArrayBitReader", "IsEmpty", kDecl},
    {"ArrayBitReader", "IsByteAligned", kDecl},
    {"ArrayBitReader", "Slice", kDecl},
    {"ArrayBitReader", "AlignToByte", kDecl},
    {"ArrayBitWriter", "IsEmpty", kDecl},
    {"ArrayBitWriter", "IsByteAligned", kDecl},
    {"ArrayBitWriter", "Slice", kDecl},
    {"ArrayBitWriter", "AlignToByte", kDecl},
    // stdlib §4.17 `SortedDictionary<K,V>` / §4.18 `SortedSet<T>`.
    {"SortedDictionary", "Count", kDecl},
    {"SortedDictionary", "FirstKey", kDecl},
    {"SortedDictionary", "LastKey", kDecl},
    {"SortedSet", "Count", kDecl}, {"SortedSet", "First", kDecl},
    {"SortedSet", "Last", kDecl},
    // stdlib §4.19 `FrozenArray<T>` declares no `Count` (it conforms to
    // `IEnumerable<T>` alone), so it carries no row here.
    // stdlib §4.20 `Counter<K>` / §4.21 `OwningSlotMap<T>`.
    {"Counter", "Count", kDecl}, {"Counter", "Total", kDecl},
    {"OwningSlotMap", "Count", kDecl},
    // stdlib §8.2 `Debug` declares only `Assert` and `WriteLine`, neither
    // `pure`, so it carries no row here.
    //
    // stdlib §9.1 — the eight integer types, ONE family ("the others have
    // analogous shapes"): the `MinValue`/`MaxValue` consts, the `pure`
    // `Parse`/`ParseOrNull`/`ToString`/`CompareTo` (`TryParse` is not
    // `pure`: it takes `out`), and the record-synthesized `Equals`/
    // `GetHashCode` (classified: a machine-integer equality).
    {kIntegerFamily, "MinValue", kConst},
    {kIntegerFamily, "MaxValue", kConst},
    {kIntegerFamily, "Parse", kDecl},
    {kIntegerFamily, "ParseOrNull", kDecl},
    {kIntegerFamily, "ToString", kDecl},
    {kIntegerFamily, "CompareTo", kDecl},
    {kIntegerFamily, "Equals", kClsf},
    {kIntegerFamily, "GetHashCode", kClsf},
    // stdlib §9.2 `Double` and `Float` ("`Float` has the analogous shape").
    {kFloatingFamily, "MinValue", kConst},
    {kFloatingFamily, "MaxValue", kConst},
    {kFloatingFamily, "Epsilon", kConst},
    {kFloatingFamily, "PositiveInfinity", kConst},
    {kFloatingFamily, "NegativeInfinity", kConst},
    {kFloatingFamily, "NaN", kConst},
    {kFloatingFamily, "IsNaN", kDecl},
    {kFloatingFamily, "IsInfinity", kDecl},
    {kFloatingFamily, "IsPositiveInfinity", kDecl},
    {kFloatingFamily, "IsNegativeInfinity", kDecl},
    {kFloatingFamily, "IsFinite", kDecl},
    {kFloatingFamily, "Parse", kDecl},
    {kFloatingFamily, "ParseOrNull", kDecl},
    {kFloatingFamily, "IeeeEquals", kDecl},
    {kFloatingFamily, "BitwiseEquals", kDecl},
    {kFloatingFamily, "ToString", kDecl},
    {kFloatingFamily, "CompareTo", kDecl},
    {kFloatingFamily, "Equals", kClsf},
    {kFloatingFamily, "GetHashCode", kClsf},
    // stdlib §9.3 `Decimal`.
    {"Decimal", "MinValue", kConst}, {"Decimal", "MaxValue", kConst},
    {"Decimal", "Zero", kConst},     {"Decimal", "One", kConst},
    {"Decimal", "Parse", kDecl},     {"Decimal", "ParseOrNull", kDecl},
    {"Decimal", "ToString", kDecl},  {"Decimal", "CompareTo", kDecl},
    {"Decimal", "Round", kDecl},
    {"Decimal", "Equals", kClsf},    {"Decimal", "GetHashCode", kClsf},
    // stdlib §9.4 `Boolean`.
    {"Boolean", "Parse", kDecl},     {"Boolean", "ParseOrNull", kDecl},
    {"Boolean", "ToString", kDecl},
    {"Boolean", "Equals", kClsf},    {"Boolean", "GetHashCode", kClsf},
    // stdlib §9.5 `Char`.
    {"Char", "MinValue", kConst},    {"Char", "MaxValue", kConst},
    {"Char", "IsDigit", kDecl},      {"Char", "IsLetter", kDecl},
    {"Char", "IsLetterOrDigit", kDecl}, {"Char", "IsWhiteSpace", kDecl},
    {"Char", "IsUpper", kDecl},      {"Char", "IsLower", kDecl},
    {"Char", "IsControl", kDecl},    {"Char", "IsPunctuation", kDecl},
    {"Char", "ToUpper", kDecl},      {"Char", "ToLower", kDecl},
    {"Char", "Parse", kDecl},        {"Char", "ParseOrNull", kDecl},
    {"Char", "ToString", kDecl},     {"Char", "CompareTo", kDecl},
    {"Char", "Equals", kClsf},       {"Char", "GetHashCode", kClsf},
    // stdlib §9.6 `String` (a class: its `Equals`/`GetHashCode`/`ToString`
    // are declared `pure`, not synthesized).
    {"string", "Length", kDecl},     {"string", "IsEmpty", kDecl},
    {"string", "IsAscii", kDecl},    {"string", "From", kDecl},
    {"string", "Repeat", kDecl},     {"string", "IsNullOrWhiteSpace", kDecl},
    {"string", "Contains", kDecl},   {"string", "StartsWith", kDecl},
    {"string", "EndsWith", kDecl},   {"string", "IndexOf", kDecl},
    {"string", "LastIndexOf", kDecl}, {"string", "Substring", kDecl},
    {"string", "AsSpan", kDecl},     {"string", "Trim", kDecl},
    {"string", "TrimStart", kDecl},  {"string", "TrimEnd", kDecl},
    {"string", "ToUpper", kDecl},    {"string", "ToLower", kDecl},
    {"string", "Replace", kDecl},    {"string", "PadLeft", kDecl},
    {"string", "PadRight", kDecl},   {"string", "PadCenter", kDecl},
    {"string", "Insert", kDecl},     {"string", "Remove", kDecl},
    {"string", "CompareTo", kDecl},  {"string", "Equals", kDecl},
    {"string", "GetHashCode", kDecl}, {"string", "ToString", kDecl},
    // stdlib §9.8 `Range<T>` — the recomputed reads carry the marker; the
    // stored-slot reads are pure by §22.15's classification rule ("All six
    // declared properties are the type's auto-property surface").
    {"Range", "IsOpenEnded", kDecl}, {"Range", "IsEmpty", kDecl},
    {"Range", "Contains", kDecl},
    {"Range", "Start", kClsf},       {"Range", "End", kClsf},
    {"Range", "Step", kClsf},        {"Range", "IsEndInclusive", kClsf},
};

// ---------------------------------------------------------------------------
// §22.15 "Members classified by rule" — the built-in POSITIONAL RECORDS
// (fix-forward #7453, the third attempt).
//
// A positional record's components are read through its generated
// read-only properties — spec §9.1: "Public read-only properties, one per
// positional field, with the same names — auto-properties in §23.1's
// sense" — and §22.15 classifies that getter: "an **auto-property getter**
// is pure (a backing-field read)". So EVERY component read of EVERY
// built-in positional record is admitted by classification, and the
// admission is a property of the record's DECLARATION HEADER, not of a
// per-member marker. Transcribing the components as per-member rows above
// (the previous attempt) had the wrong shape twice over: the earlier
// review found `Span<T>`'s components missing, and the second found
// `JsonOptions`'s — each a declaration the row table had not reached. This
// table is keyed by the record instead, ONE entry per `record X(…)` header
// the documents declare, so a record is either here whole or not at all.
//
// THE POPULATION is every `public record X(…)` declaration of a BUILT-IN
// (compiler-known, declaration-less) type in the standard library (v0.355:
// `grep -n -E '^ *public (readonly )?record [A-Za-z]+(<[^>]*>)?\('
// docs/Bezel_Standard_Library.md`, minus the user-record examples `Point`,
// `User`, `ConfigError`, `ClickedEvent`, `PriceTick`, `JobResult`) plus the
// one the language spec itself declares, §31.4's `ExceptionInfo`. The
// stdlib's body records (`Range<T>`, stdlib §9.8, whose declared stored
// slots are classified by rows above; the §2.5 temporal and identity
// values) are not positional and are not here.
// ---------------------------------------------------------------------------
struct StdlibPositionalRecord {
    std::string_view head;
    std::string_view components[5];  // "" past the declared arity
};

constexpr StdlibPositionalRecord kStdlibPositionalRecords[] = {
    // stdlib §2.15 `record JsonOptions(Indented: bool, IndentWidth: int,
    // AllowTrailingCommas: bool, IgnoreUnknownFields: bool, MaxDepth: int)`.
    {"JsonOptions", {"Indented", "IndentWidth", "AllowTrailingCommas",
                     "IgnoreUnknownFields", "MaxDepth"}},
    // stdlib §3.3 `record ReadOnlyStringSpan(Source: string, Start: int,
    // Length: int)`.
    {"ReadOnlyStringSpan", {"Source", "Start", "Length"}},
    // stdlib §4.8 `record DictionaryEntry<K, V>(Key: K, Value: V)`.
    {"DictionaryEntry", {"Key", "Value"}},
    // stdlib §4.14 `record Span<T>(Buffer: OwningBuffer<T>, Start: int,
    // Length: int)` / `record ReadOnlySpan<T>(Buffer, Start, Length)`.
    {"Span", {"Buffer", "Start", "Length"}},
    {"ReadOnlySpan", {"Buffer", "Start", "Length"}},
    // stdlib §4.14 `record BitReader(Source: ReadOnlySpan<byte>, BitStart:
    // int, BitLength: int)` / `record BitWriter(Source: Span<byte>, …)`.
    {"BitReader", {"Source", "BitStart", "BitLength"}},
    {"BitWriter", {"Source", "BitStart", "BitLength"}},
    // stdlib §4.15 `record ArraySpan<T>(Source: Array<T>, Start: int,
    // Length: int)` / `record ReadOnlyArraySpan<T>(Source, Start, Length)`,
    // and the mirrored bit tier `record ArrayBitReader(Source:
    // ReadOnlyArraySpan<byte>, BitStart: int, BitLength: int)` /
    // `record ArrayBitWriter(Source: ArraySpan<byte>, …)`.
    {"ArraySpan", {"Source", "Start", "Length"}},
    {"ReadOnlyArraySpan", {"Source", "Start", "Length"}},
    {"ArrayBitReader", {"Source", "BitStart", "BitLength"}},
    {"ArrayBitWriter", {"Source", "BitStart", "BitLength"}},
    // spec §31.4 `record ExceptionInfo(TypeName: string, Message: string,
    // StackTrace: string)`.
    {"ExceptionInfo", {"TypeName", "Message", "StackTrace"}},
};

const StdlibPositionalRecord* stdlibPositionalRecordOf(
    std::string_view head) {
    for (const auto& r : kStdlibPositionalRecords)
        if (r.head == head) return &r;
    return nullptr;
}

// Is `member` a positional component of the built-in record `head`? True
// iff the record's declaration header names it: the read is then the
// generated auto-property's getter, pure by classification (§22.15).
bool stdlibPositionalComponentOf(std::string_view head,
                                 std::string_view member) {
    const StdlibPositionalRecord* r = stdlibPositionalRecordOf(head);
    if (!r) return false;
    for (std::string_view c : r->components)
        if (!c.empty() && c == member) return true;
    return false;
}

// The table key of a standard-library type head: the stdlib's own
// System-namespace spelling (`int` → `Int32`, `String` → `string`, the
// registry's one normalisation), then the §9.1 / §9.2 family.
std::string_view purityTableHeadOf(std::string_view head) {
    head = common::builtinParamHeadSpelling(head);
    if (head == "SByte" || head == "Byte" || head == "Int16" ||
        head == "UInt16" || head == "Int32" || head == "UInt32" ||
        head == "Int64" || head == "UInt64")
        return kIntegerFamily;
    if (head == "Double" || head == "Float") return kFloatingFamily;
    return head;
}

bool stdlibHeadHasRows(std::string_view head) {
    const std::string_view key = purityTableHeadOf(head);
    for (const auto& e : kStdlibPureMembers)
        if (e.head == key) return true;
    return stdlibPositionalRecordOf(key) != nullptr;
}

// Is the standard-library member `head.member` admitted in a pure body —
// a const read (rule 1), a `pure`-declared member (rule 6), or a member
// classified by rule: a row of `kStdlibPureMembers`, or a positional
// component of a `kStdlibPositionalRecords` record?
bool stdlibMemberIsPure(std::string_view head, std::string_view member) {
    const std::string_view key = purityTableHeadOf(head);
    for (const auto& e : kStdlibPureMembers)
        if (e.head == key && e.member == member) return true;
    return stdlibPositionalComponentOf(key, member);
}

// Is a declaration-less, single-part Named head a STANDARD-LIBRARY type? A
// declaration-less Named TypeRef is one of exactly two things — a reserved
// built-in, or a TYPE PARAMETER (`common::isReservedBuiltinTypeName`'s own
// contract) — and a type parameter is not the standard library's: §22.15
// *Checking and dispatch* routes a call on a constraint-typed value to the
// constraint interface's declared members, which is not this arm's
// question. So the head must be NAMED as a built-in by one of the
// registry's tables (or by this file's own rows, for the few stdlib types —
// `Counter`, `BitReader` — no registry table lists).
bool isStdlibTypeHead(std::string_view h) {
    return common::isReservedBuiltinTypeName(h) ||
           !common::builtinTypeHomeModuleOf(h).empty() ||
           common::isBuiltinExceptionName(h) ||
           common::isKeywordAliasBuiltinName(h) || stdlibHeadHasRows(h);
}

// The head of a STANDARD-LIBRARY receiver, or "" when the receiver is a
// user-declared or imported type (whose declaration-based predicates carry
// the `pure` modifier) or a type parameter.
//
// THREE SHAPES. A PRIMITIVE-typed instance receiver (`b` of type `byte`,
// `x` of type `Int32`) is named by its `PrimitiveType` — never by the
// spelling the source used, so `int` and `Int32` read the same row (spec
// §3.4: the keyword ALIASES the type). Another instance receiver (`sb` of
// type `StringBuilder`, `a` of type `Array<int>`) resolves to a Named
// TypeRef whose `declaration` is null; its head is the TypeRef's own
// `headName()` (alias-transparent, so `type Al := Array<int>` reads the
// same), provided `isStdlibTypeHead` names it. A STATIC-CLASS receiver
// (`Console`, `Math`) has no value type at all: `ctx_.exprType` is null and
// the identifier carries the built-in CLASS SYMBOL — declaration-less and
// with no declaring module (an IMPORTED user type is declaration-less too,
// but carries its module; `TypeSymbol::isBuiltinRangeSym`'s test).
std::string_view builtinReceiverHead(const TypeRef* rt, Symbol* recvSym) {
    if (rt) {
        if (rt->kind == TypeRef::Kind::Primitive) {
            if (rt->primitive == PrimitiveType::None) return {};
            return common::primitiveBuiltinSpelling(
                static_cast<int>(rt->primitive));
        }
        if (rt->kind != TypeRef::Kind::Named || rt->declaration) return {};
        const std::string_view h = rt->headName();
        return isStdlibTypeHead(h) ? h : std::string_view{};
    }
    if (auto* ts = dynamic_cast<TypeSymbol*>(recvSym)) {
        if (ts->decl || !ts->moduleName.empty()) return {};
        // An enum constant is a §22.15 rule 1 read ("enum constants").
        if (ts->kind == ast::TypeDeclKind::Enum) return {};
        return ts->name;
    }
    return {};
}

// The head of a KEYWORD-TYPE receiver — the STATIC spelling of a primitive
// type's own members: `string.Join(...)`, `int.MaxValue`, `double.Parse(...)`.
//
// WHY A FOURTH SHAPE. §3.4's keyword alias names a TYPE, not a value, so the
// identifier `string` in `string.Join(...)` is a type name: the typecheck
// pass attaches no value type to it and no TypeSymbol either (the
// primitive-keyword static branch in ExprTypecheck_Calls.cpp reads the NAME
// and dispatches on it — "The target is an identifier that names a primitive
// type; there's no TypeSymbol in scope for it"). `builtinReceiverHead`
// therefore answered "" for that receiver, and the standard-library arm
// stood down: the static member of a keyword-aliased type was never read
// against the library's `pure` markers, so `string.Join(",", xs)` — whose
// `Join` the stdlib does NOT mark `pure` (stdlib §9.6) — compiled in a
// `pure` body.
//
// THE KEY. The name is the §3.4 KEYWORD ALIAS (`common::isKeywordAliasBuiltinName`
// — the fourteen lowercase spellings) or its PascalCase twin `String`
// ("the keyword `string` and the standard-library type name `String` name the
// same type — they are interchangeable in source", §1.4/§3.4). The table
// normalises both spellings itself (`purityTableHeadOf`), so the raw name is
// what this returns.
//
// THE GUARD. The receiver must NAME a type, not hold a value: a local
// spelled `String` carries a value type, and `recvTy` is then non-null, so
// the keyword reading never swallows it. (The fourteen lowercase aliases
// cannot be shadowed at all — "a reserved keyword ... cannot be used as an
// identifier", Appendix B.1.)
std::string_view keywordTypeReceiverHead(const ast::Expr* target,
                                         const TypeRef* recvTy) {
    if (recvTy) return {};
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(target);
    if (!id) return {};
    const std::string_view name = id->name;
    if (common::isKeywordAliasBuiltinName(name)) return name;  // §3.4
    if (name == "String") return name;                         // §3.4's alias
    return {};
}

// §22.15 (v0.202, #2052) — the closed-body checker. A `PureBodyWalker` is
// constructed per pure member and walks its statements/expressions.
//
// Slice 2 covers the load-bearing rules (frame-local mutation root; calls
// to non-pure members; class construction outside the throw carve-out; the
// consuming discard; IEnumerable for-source rejection). Lambda bodies are
// NOT checked (§22.15 rule 4: a lambda body defined inside a pure body is
// a separate function whose later invocation happens at the caller's
// effect discipline).
class PureBodyWalker {
public:
    PureBodyWalker(SemaContext& ctx, MemberSymbol& member,
                   const std::string& memberLabel, bool isConstructor)
        : ctx_(ctx), member_(member), memberLabel_(memberLabel),
          isConstructor_(isConstructor) {}

    void checkStmts(const std::vector<std::unique_ptr<ast::Stmt>>& body);
    void checkOneStmt(const ast::Stmt* s);
    void checkExpr(const ast::Expr* e);

    // fix #8134 (§22.15 / §A.7, v0.355): the ONE walk of a `case` arm's
    // sub-expressions, shared by the two arm-bearing forms — the
    // expression-position `ast::CaseExpr` and the statement-position
    // `ast::CaseStmt`. An arm carries three sub-expressions, and §22.15's
    // list is closed ("A pure body may contain exactly the following"), so
    // all three are subject to the purity rules:
    //   * the `when` GUARD (`ast::CaseArm::guard`) — an ordinary expression;
    //   * the arm's EXPRESSION body (`ast::CaseArm::exprBody`) — the
    //     expression-position form's arm value;
    //   * the arm's STATEMENT body (`ast::CaseArm::stmts`) — the
    //     statement-position form's `do … end` body.
    // Before this helper the expression-position form walked NONE of them
    // and the statement-position form walked only the third.
    void checkCaseArms(const std::vector<ast::CaseArm>& arms);

    // §22.15 "Pure constructors" (v0.202, #2052, #2061): a pure constructor's
    // explicit `base(…)`/`this(…)` chain call (§11.1) must target a pure
    // constructor — the chain target's purity propagates upward. The chain
    // surfaces as the body's first ExpressionStmt whose expression is a
    // CallExpr on a `this` or `base` identifier. We resolve the target
    // constructor by CLASS IDENTITY (the same class for `this(…)`; the
    // class-typed first base for `base(…)`) and arity — there is no
    // ambiguity in the pure-classification call: the class narrows the
    // search, the call's argument count picks the single overload. Reports
    // a §22.15 diagnostic when the resolved target is not pure-declared.
    void checkPureCtorChainPurity(const ast::ConstructorDecl& cd,
                                  const ConstructorSymbol& cs);

    // fix #8163 (§22.15, v0.355): the CONSTRUCTION CONTENT of a class's
    // PRIMARY constructor — the inheritance clause's base-call arguments and
    // the primary's parameter DEFAULT expressions — walked under the
    // pure-body rules when a pure constructor chains to that primary.
    //
    // §12.8: a `this(...)` chain in a primary-ctor class ends at the primary.
    // §22.8 step 0-2 / §22.10: the primary's construction runs the base call
    // and then the class's field initializers, and a parameter default
    // evaluates when the argument is omitted (§22.5) — i.e. all of it runs as
    // part of the construction a pure constructor chains to. §22.15's rule
    // ("A pure constructor's `base(…)`/`this(…)` chain target must itself be
    // pure") therefore reaches this content.
    //
    // The class's FIELD INITIALIZERS are deliberately NOT walked here: the
    // class-level birth-store walk (`checkOneType`) already enforces them for
    // every class with a pure constructor ("a class with a pure constructor
    // must have field initializers satisfying the pure-body rules"), and
    // walking them twice would report the same impurity twice.
    //
    // fix #8163b (§22.15, v0.355): `cls` is the class symbol of `td`; the
    // base constructor the inheritance clause INVOKES is walked too.
    void checkPrimaryCtorConstructionContent(const ast::TypeDecl& td,
                                             const TypeSymbol* cls,
                                             int depth = 0);

    // §22.16 (v0.203, #2053): walk each contract clause's expression under
    // the §22.15 pure-body rules APPLIED EXPRESSION-WISE, with the clause-
    // specific amendments: `throw` is rejected (the carve-out's reverse —
    // the mechanism, message, and blame of a violation belong to §22.16's
    // machinery, not to a clause's own throw), and `await` is rejected on
    // every member (a clause is not part of an async body's statement
    // sequence). Purity's other rules (frame-local mutation, calls to
    // pure members, etc.) apply unchanged. Stores and owned-consuming
    // operations cannot be spelled in an expression, so the borrow-only
    // guarantee is the sum of these classifications (§22.16 "Clause typing:
    // bool, pure, borrow-only").
    void checkContractClauses(
        const std::vector<ast::ContractClause>& clauses);

    // §22.16: walk an `invariant` member's expression under the same clause
    // rules (throw/await rejected, pure-body rules expression-wise).
    void checkInvariantExpr(const ast::Expr& e);

private:
    SemaContext& ctx_;
    MemberSymbol& member_;
    std::string memberLabel_;       // "method `M`" / "property `P`" / etc.
    bool isConstructor_;            // §22.15 birth-store rule admissions
    // §22.16 (v0.203, #2053): when true, this walker is checking a contract
    // clause / invariant expression — `throw` is rejected (the carve-out does
    // NOT apply to clauses), and the violation's `operation` prefix is
    // "a contract clause's expression" rather than "a pure body".
    bool clauseMode_ = false;

    // fix #7453 (§22.15 rule 6): true while the walker is inside a CALL's
    // CALLEE expression, so a member access reached that way is reported as
    // a "call to a standard-library member" rather than as an accessor
    // read — the same access, named by the position it was reached from.
    // Saved and restored around the callee recursion, so an argument's own
    // member access is not read as a callee.
    bool inCallee_ = false;

    // §22.15 "the closed list's contrapositive": any store whose target
    // root leaves the frame is rejected (instance/static field, indexer
    // slot, dereferenced class reference). In a pure constructor the
    // birth-store rule admits stores to fields of `this`.
    void checkAssignmentTarget(const ast::Expr& target);

    // Walk an access-path root: returns the Symbol* the path is anchored on
    // (LocalSymbol / ParameterSymbol / FieldSymbol / etc.), or nullptr if
    // the path doesn't have an identifier-anchored root.
    Symbol* targetRootSymbol(const ast::Expr& target);

    void reportPureViolation(SourceLocation loc,
                             const std::string& operation,
                             const std::string& detail);

    // §22.15 throw carve-out: `new E(args)` admitted iff (a) E's INVOKED
    // constructor is `pure`-declared and (b) E is trivial-dispose. Returns
    // true iff admitted; *why (when non-null) receives the one-clause
    // explanation of a refusal, for the diagnostic.
    bool throwCarveOutAdmits(const ast::NewExpr& ne, std::string* why);

    // The ONE walk of a `throw` operand, shared by the `ThrowStmt` and the
    // `ThrowExpr` arms (fix-forward #7453): §22.15's carve-out admits `new
    // E(args)` "only as the immediate operand of a `throw` statement or
    // `throw` expression", so both spellings take a `NewExpr` operand
    // through `throwCarveOutAdmits` (reporting at `throwLoc` when it does
    // not admit) and check only its arguments; any other operand (a bare
    // re-raise, a caught binding read) is walked as an ordinary expression.
    void checkThrowOperand(const ast::Expr* operand, SourceLocation throwLoc);

    // §22.15 trivial-dispose — the exact inverse of
    // `common::isNonTrivialDisposeClass`, which is what it now calls (#3588).
    // A class is trivial-dispose iff it has no destructor, no owning inline
    // array (§15.1.1), no captured `owned` primary-ctor parameter (§22.10),
    // and no non-static `owned` field OF NON-TRIVIAL-DISPOSE TYPE (§28.9's
    // recursive clause, fix #3647), anywhere in its base chain.
    bool isTrivialDisposeClass(const TypeRef* rt) const;

    // §22.15 rule 6: a call to a member not pure-declared / pure-classified
    // is rejected. Returns true iff admitted.
    bool callTargetIsPure(Symbol* calleeSym);

    // fix #8133 (§22.15 rule 6, v0.355): the head of a STANDARD-LIBRARY
    // receiver for the two member-access arms (`.` and `?.`). It is the
    // receiver's own built-in type head (`builtinReceiverHead`) and, when
    // the receiver names a TYPE rather than holding a value, the
    // KEYWORD-TYPE spelling (`keywordTypeReceiverHead` — `string.Join(...)`,
    // `int.MaxValue`).
    std::string_view stdlibReceiverHead(const ast::Expr* target) const;

    // narrow #3610 (§22.15 rule 6, v0.286): rule 6's admitted call forms name
    // "method, **operator**, accessor read, or constructor" — the operator
    // form was the one this walker never asked about. A §25 user-declared
    // overload is a call to a `MethodSymbol`, and `ExprTC::typeBinary` /
    // `typeUnary` ALREADY record that symbol on the operator node
    // (`setExprSymbol(&be, opm)` — ExprTypecheck_Exprs.cpp:4688, :4809, :5894),
    // so the question is answerable from a fact the compiler already computes.
    // Routed through `callTargetIsPure`, the one authority fix #3609 left
    // behind, rather than a sixth spelling of "is this member pure".
    //
    // `opSym` is null for every BUILT-IN operator on primitives, enums, and
    // `string` (`findBinaryOperator`/`findUnaryOperator` resolve no symbol
    // there) — rule 3 material, not a call, and deliberately untouched.
    void checkOperatorOverloadPurity(Symbol* opSym, std::string_view opTok,
                                     SourceLocation loc);

    // §22.15 "Members classified by rule" (#2061, item 2): a record's
    // synthesized Equals/GetHashCode/ToString are pure-classified iff every
    // field's equality dispatch target is pure (auto-hashable form).
    // The synthesized-positional-ctor / with-copy forms are matched by
    // `recordConstructionIsPure`. Returns true when the call IS to a
    // synthesized record member AND the record is auto-hashable
    // (class-identity check); false otherwise. Sets
    // *outIsSynthesizedRecordMember to true when the call resolves to a
    // record-synthesized member regardless of purity (so the caller can
    // distinguish "non-record call — fall through" from "record-synth
    // but not pure-classified — report").
    bool recordSynthesizedCallIsPure(const ast::CallExpr& ce,
                                     bool* outIsSynthesizedRecordMember);

    // §22.15 "Pure constructors" + "Members classified by rule" (#2061,
    // item 2): record construction `new R(args)` is admitted in a pure body
    // iff the invoked constructor is compiler-synthesized-pure (a POSITIONAL
    // record — class-identity check, not arity-based) OR a user-declared
    // pure constructor with matching arity (body records have no synthesized
    // ctor). Returns true iff the construction is provably pure-classified;
    // returns false on a record construction that does not satisfy this
    // rule (caller may then report).
    bool recordConstructionIsPure(const ast::NewExpr& ne);

    // §22.15 rule 5: a `for` loop source must not construct an enumerator.
    // Range<T> of primitive steppable type, inline arrays, spans, both
    // OwningBuffer<T> presentations, a CONCRETE Array<T>/OwningArray<T>
    // source, and a `string` source are admitted; an IEnumerable<T> source
    // and an Array<T> whose element is a type parameter are rejected.
    bool forSourceAdmitsEnumerator(const ast::Expr& iterable) const;

    // §22.15 rule 5: is `rt` a source that the `for` desugaring expands
    // into an enumerator-constructing loop?
    bool isEnumeratorConstructingSource(const TypeRef* rt) const;

    // fix #3279 (v0.263, stdlib §9.8 + spec §22.15): the `in` membership
    // operator (§17.5) is semantically `r.Contains(value)`. This helper
    // reports whether the receiver `r`'s `Contains` member is pure-
    // classified for the purposes of §22.15's calls-to-pure-members rule.
    // Admitted shapes: Range<T> (intrinsic-purity contract; its Contains is
    // pure-declared in stdlib §9.8), string and the various ReadOnlySpan /
    // ArraySpan variants (Contains is pure-declared in stdlib §3.3 and
    // §4.14), and any user-declared type whose `Contains(value: T)` member
    // carries the `pure` modifier (looked up on the receiver's member
    // list). All other receivers (Array<T>, OwningArray<T>, user-defined
    // non-pure Contains, unresolved) are rejected — the operator is
    // semantically an impure call and §22.15 admits it only when its
    // callee is pure-classified.
    bool inOperatorCalleeIsPure(const ast::BinaryExpr& be) const;
};

bool PureBodyWalker::isTrivialDisposeClass(const TypeRef* rt) const {
    // fix #3588 (§22.15 / §28.9 / §2.8.2, v0.286): ONE body, in `common/`.
    //
    // What stood here was a second, hand-maintained copy of
    // `common::isNonTrivialDisposeClass` written inverted — same cycle guard,
    // same 4096-iteration cap, same member scan (destructor / owning inline
    // array §15.1.1 / captured `owned` primary-ctor parameter §22.10 /
    // non-static `owned` field), same base-chain step, same carried-verdict
    // short-circuit. A normalizing diff of the two bodies at 11920b8 found NO
    // classification arm that differed: every hunk was `return false` where the
    // shared predicate writes `reject = true; break;`. The comment that used to
    // stand on that copy claimed it was "kept in sync with the predicate's
    // shape by construction" — it was kept in sync by hand, and the two edits
    // that had to touch it (#2666's inline-array arm, #3223's carried verdict)
    // are the record of that. Delegation makes the claim true.
    //
    // §22.15's throw carve-out wants the CARRIED imported verdict — that is
    // fix #3223's whole subject (an imported destructor-bearing exception type
    // must not be admitted at `throw new E()`), and it is the shared
    // predicate's default.
    //
    // fix #3647 (§28.9 / §3.6 rule 2, v0.286): the TypeRef+SemaContext
    // overload — the sole caller (`throwCarveOutAdmits`) already has the
    // resolved TypeRef (`rt`, from `ctx_.resolvedType(*ne.type)`) in scope,
    // so this passes it straight through rather than fabricating a synthetic
    // one; `ctx_` is this walker's own SemaContext member.
    return !common::isNonTrivialDisposeClass(rt, ctx_);
}

// fix #8133 (§22.15 rule 6, v0.355): the head of a STANDARD-LIBRARY receiver
// for the two member-access arms (`.` and `?.`). It is the receiver's own
// built-in type head, and — when the receiver NAMES a type rather than
// holding a value — the KEYWORD-TYPE spelling (`string.Join(...)`,
// `int.MaxValue`). The two are disjoint: `builtinReceiverHead` answers ""
// for a receiver that is a type name, and `keywordTypeReceiverHead` answers
// "" for a receiver that holds a value.
std::string_view PureBodyWalker::stdlibReceiverHead(
    const ast::Expr* target) const {
    if (!target) return {};
    const TypeRef* rt = ctx_.exprType(*target);
    Symbol* rs = ctx_.exprSymbol(*target);
    const std::string_view head = builtinReceiverHead(rt, rs);
    if (!head.empty()) return head;
    return keywordTypeReceiverHead(target, rt);
}

bool PureBodyWalker::callTargetIsPure(Symbol* calleeSym) {
    // §22.15 rule 6: a call is admitted iff the declared member on the
    // receiver's static type is pure-declared or pure-classified. For
    // Slice 2 we check the explicit `pure` marker (declared) —
    // pure-CLASSIFICATION (auto-getters, record-synthesized members) lands
    // in a later slice.
    if (!calleeSym) return false;  // conservatively reject unknown
    // fix #3609 (§22.15, v0.286): through the one predicate. This arm was the
    // WIDEST of the five spellings of "is this member pure" — symbol field, the
    // `ast::OperatorDecl` bool, and the modifier scan — so `memberIsPure` is it
    // verbatim plus a `ConstructorDecl` arm that a `MethodSymbol` can never
    // reach. No admitted program changes verdict here.
    if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym))
        return memberIsPure(ms);
    // fix #3174 (§22.15, v0.254): through the one predicate. This site was
    // already both-sources; it is routed here so the chain-purity site below
    // cannot drift away from it again.
    if (auto* cs = dynamic_cast<ConstructorSymbol*>(calleeSym))
        return constructorIsPure(cs);
    // Property/indexer accessor reads — §22.15 rule 6's accessor-read arm. Tight
    // before #3630 this arm admitted EVERY property read unconditionally, on
    // the rationale that pure-classification (auto-getters, record-synthesized
    // members) lands later. With #3609 the declared-purity check
    // (`propertyGetterIsPure` — both spellings: property-level `pure`, the
    // accessor-level `pure get`) is now both-sources — local AST via the
    // `pd->accessors` walk, imported via the new `ps->accessorIsPure` slot
    // populated by `ModuleLoader` from `MetaMember::accessorIsPure` (#3618) —
    // so the declared side of rule 6 can fire here without depending on the
    // still-deferred classification side. The MemberAccessExpr site in
    // `checkExpr` is the OTHER arm (the property READ spelling — `c.Value`
    // inside a pure body); together they close the accessor-read arm on
    // every spelling of an accessor reach.
    if (auto* ps = dynamic_cast<PropertySymbol*>(calleeSym))
        return propertyGetterIsPure(ps);
    // Intrinsic / builtin calls (no AST decl) are admitted — §22.15
    // "Intrinsic purity (the stdlib contract)" trusts stdlib markers, and
    // Slice 4 covers the stdlib annotation wave.
    return true;
}

// narrow #3610 (§22.15 rule 6, v0.286): the operator arm of rule 6.
//
// WHAT WAS MISSING. Rule 6 admits "a call — method, **operator**, accessor
// read, or constructor in the admitted positions — … iff the member declared
// on the receiver's static type … is `pure`-declared or pure-classified".
// `checkExpr`'s BinaryExpr arm implemented rule 3 (built-in operators on
// primitives, enums and `string`) by walking the operands, and the only
// operator it ever classified was `in` — which is not an overload at all but
// the §17.5 `Contains` call. A §25 user-declared `operator -` therefore ran
// inside a pure body, locally and across the artifact boundary alike.
//
// WHY THIS IS NOT A SIXTH PREDICATE. The overload is already resolved and
// already recorded: `ExprTC::typeBinary` calls `ctx_.setExprSymbol(&be, opm)`
// for a named-type overload and for a `where T: I<T>` constraint static
// operator, and `ExprTC::typeUnary` does the same for `&ue`. Nothing else
// ever puts a symbol on a BinaryExpr/UnaryExpr node, so "the node carries a
// MethodSymbol" IS "this operator is a user-declared overload". The purity
// question then goes to `callTargetIsPure`, unchanged.
//
// NARROW, DELIBERATELY. This covers the operator EXPRESSION forms only.
// The STATEMENT forms — `a += b` and `t++` through a §25 overload — record no
// symbol anywhere (their overload is re-looked-up by hand at five separate
// sites), so closing them needs the resolution recorded first: #3628.
// Rule 6's "accessor read" arm stays as it was — `callTargetIsPure` still
// admits every `PropertySymbol` unconditionally — because an accessor-level
// `pure get` has no `MetaMember` slot (#3618), so tightening it today would
// reject a cross-module property read whose local twin compiles: #3630.
void PureBodyWalker::checkOperatorOverloadPurity(Symbol* opSym,
                                                 std::string_view opTok,
                                                 SourceLocation loc) {
    auto* ms = dynamic_cast<MethodSymbol*>(opSym);
    if (!ms) return;   // rule 3: a built-in operator resolves no symbol.
    if (callTargetIsPure(ms)) return;
    std::string owner = ms->parent ? std::string(ms->parent->name)
                                   : std::string("<type>");
    reportPureViolation(
        loc, std::string("call to `operator ") + std::string(opTok) + "`",
        std::string("\xC2\xA7""25's `") + std::string(opTok) +
        "` on this operand type is the call `" + owner + "::operator " +
        std::string(opTok) + "`, and \xC2\xA7""22.15's calls-to-pure-members rule "
        "admits an operator call only when the resolved overload is "
        "pure-declared \xE2\x80\x94 declare it `pure`, or use a `pure` overload");
}

// §22.15 "Members classified by rule" (#2061, item 2): a record's
// synthesized Equals/GetHashCode/ToString are pure-classified iff the
// record class is auto-hashable — every constituent operation
// (field read + the dispatched equality/hash/ToString on each field's
// type) is itself pure. The §9.2/§9.3 admissibility helpers in
// SymbolCollection.cpp already implement this recursive field rule;
// PureChecker reuses them rather than duplicating the walk.
//
// Match BY RECORD CLASS IDENTITY — not by arity. A record's synthesized
// Equals/GetHashCode/ToString exist for the WHOLE record class (every
// positional or body record has them); the receiver's static type alone
// determines whether the call targets a synthesized record member. The
// arity of the call is irrelevant (these members take `Equals(R)` /
// `int GetHashCode()` / `string ToString()`, all fixed signatures).
//
// Returns true when the call IS to a record-synthesized member AND the
// record is auto-hashable. Sets *outIsSynthesizedRecordMember
// to true when the call resolves to a synthesized record member
// REGARDLESS of purity, so the caller can distinguish "non-record call —
// fall through" from "record-synthesized but not pure-classified — report".
bool PureBodyWalker::recordSynthesizedCallIsPure(
    const ast::CallExpr& ce, bool* outIsSynthesizedRecordMember) {
    if (outIsSynthesizedRecordMember) *outIsSynthesizedRecordMember = false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get());
    if (!ma || !ma->target) return false;
    // §22.15: synthesized set here is Equals/GetHashCode/ToString. The
    // synthesized positional ctor is reached via `new R(...)` (handled
    // by recordConstructionIsPure) and the with-copy via `r with { ... }`
    // (a pure-body WithExpr walks its source + values recursively).
    if (ma->member != "Equals" && ma->member != "GetHashCode" &&
        ma->member != "ToString")
        return false;
    const TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != TypeRef::Kind::Named ||
        !recvTy->declaration) return false;
    TypeSymbol* rec = recvTy->declaration;
    // Class-identity check: a synthesized record member exists for the
    // WHOLE record class (pos or body); the receiver's record-typing
    // confirms the call shape.
    if (rec->kind != ast::TypeDeclKind::Record) return false;
    if (outIsSynthesizedRecordMember) *outIsSynthesizedRecordMember = true;
    // fix #3631 (§9.1 / §9.2, v0.286): route through the ONE both-sources
    // positional-record accessor (see its doc comment in Symbol.hpp) rather
    // than re-deriving the `hasPositionalFields` / AST-decl-fallback shape
    // here. BEHAVIOR-FREE: this site already checked `hasPositionalFields`
    // first with the same `ast::TypeDecl` fallback, so `recordIsPositional`
    // admits exactly what the inline check admitted, nothing more — pure
    // mechanical dedup, verified byte-for-byte against the old inline form
    // before routing (§6b).
    if (recordIsPositional(rec))
        return isPositionalRecordEqualityBearing(rec, ctx_);
    return isAutoEqualityBearingBodyRecord(rec, ctx_);
}

// Resolve a body record's user-declared constructor of matching arity, or
// a positional record's user-declared override of the synthesized ctor.
// Returns nullptr when no ctor of that arity exists OR when the overload
// set is ambiguous (handled by the overload-resolution pass, not here).
// The pure-classification rule keys on existence + purity, not on
// overload tie-breaking.
//
// fix #3219 (§22.15, v0.254): the candidate walk used to `continue` on any
// `ConstructorSymbol` whose `decl` was null — fine for a local record, but
// FAILS for an imported record whose ctor names a SIBLING type:
// `parseAndAttachCtorBody` re-runs `resolveTypes` over a synthetic source that
// declares only the one type (#2290 bug 2 / #710), so a sibling-naming ctor's
// `decl` is left null and this walk would drop it silently. The drop produced
// two measured defects:
//
//   * a body record with a `pure` ctor naming a sibling  — FALSE REJECT
//     (the ctor is `pure` but the walk misses it, so `recordConstructionIsPure`
//      returns false and §22.15 fires).
//   * a positional record with an impure shadowing ctor naming a sibling —
//     FALSE ACCEPT (§22.15 soundness: the synthesized positional ctor's
//     auto-hashable classification is used instead of the impure user-declared
//     shadow).
//
// (For the positional case, an arity-mismatch with the synthesized positional
// ctor also fails the §22.5 arity check at the call site — `checkNewExprArgs`
// does not consult user-declared ctors on a positional record — so the second
// shape is now unreachable for the positional case. The widening below closes
// it wherever it IS reachable, e.g. body records, future shapes that route
// through this walk.)
//
// The widening is GATED: when any decl-attached candidate of matching arity
// exists, the decl-attached path takes precedence so every program the old
// code answered keeps that answer. When all decl-attached candidates are
// absent, the symbol-side reading (`constructorIsPure`) is consulted;
// `applyBehavior` sets `cs->isPure` on every imported ctor (ModuleLoader.cpp,
// family M8). The single new admission is: "a matching, non-static ctor
// SYMBOL with no attached declaration".
//
// The matching concern that previously kept this walk unconverted — widening
// could turn a single resolved match into an ambiguity — is real but narrow:
// `matching` exceeds 1 only when a record declares two same-arity ctors,
// which is already an overload ambiguity.
static const ConstructorSymbol* findUserCtorByArity(
    const TypeSymbol* rec, std::size_t arity) {
    int matching = 0;
    const ConstructorSymbol* declMatch = nullptr;
    int symMatching = 0;
    const ConstructorSymbol* symMatch = nullptr;
    for (Symbol* m : rec->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs || cs->isStatic) continue;
        if (cs->params.size() != arity) continue;
        if (dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) {
            ++matching;
            declMatch = cs;
        } else {
            ++symMatching;
            symMatch = cs;
        }
    }
    if (matching > 0) return matching == 1 ? declMatch : nullptr;
    return symMatching == 1 ? symMatch : nullptr;
}

// fix-forward #8134 (§22.15 / §31.4, v0.355): the INVOKED constructor of a
// `new E(args)` the typecheck pass did not publish.
//
// WHY THE PASS SOMETIMES PUBLISHES NOTHING. `ExprTC::checkNewExprArgs` (the
// LOCAL path) and `ExprTC::checkImportedNewExprArgs` (the IMPORTED twin)
// publish the constructor they selected on `SemaContext::resolvedNewCtor` in
// their OVERLOAD arms only — the §22.6 winner, the §22.6-filtered single
// candidate, and the imported twin's winner. Their SINGLE-constructor arms
// (the LOCAL `ctors.size() == 1` tail, and the imported tail) bind the
// arguments and check them, but publish nothing. So for every class with
// exactly ONE constructor the selection is not on the table, and §22.15's
// carve-out has to identify the invoked constructor itself.
//
// WHY THE OLD ARITY MATCH WAS NOT ENOUGH. It matched
// `cs->params.size() == args.size()` — an EXACT parameter count — and §22.5
// makes a shorter call legal against a longer parameter list when the
// trailing formals declare defaults:
//
//   public pure constructor(message: string, code: int := 0) base(message);
//
// invoked as `throw new MyError("neg")` — ONE argument, TWO formals, the
// second filled from its default. The arity match does not see that
// constructor at all, so the carve-out refused a program whose invoked
// constructor IS `pure`-declared (§22.15 (a)) — a rejection the base did not
// make, and one this fix-forward removes.
//
// THE TWO RULES, in order.
//
//   (a) ONE non-static constructor: that constructor IS the invoked one. A
//       class with a single constructor has no other candidate to select,
//       and the typecheck pass has already rejected the program if the
//       arguments do not bind to it (§22.5) or do not convert (§5.1) — so
//       naming it here cannot mis-identify a legal call.
//
//   (b) SEVERAL constructors: bind the call's arguments to each candidate's
//       formals (§22.5) and take the unique candidate that binds. Binding
//       is what honours a DEFAULTED formal left unbound and a NAMED
//       argument — the two shapes the arity match could not see. Two
//       candidates that bind leave the choice to §22.6's ranking, which the
//       typecheck pass runs; if it published no winner, the invoked
//       constructor is not identifiable here, and the carve-out does not
//       admit it (the same verdict the pre-#8134 "no pure ctor on the class"
//       answer gave).
//
// A STATIC constructor is never invoked by a `new` expression, so it is not a
// candidate here — the same exclusion `findUserCtorByArity` makes.

// §22.5 argument-to-parameter binding, SILENT (no diagnostics): the
// APPLICABILITY half of `ExprTC::bindArgsToParams` — the same rule it
// applies, reduced to the three facts `sema::FormalsView` carries (how many
// formals, each formal's NAME, and whether it may be left unbound).
//
// WHY A SECOND COPY. `bindArgsToParams` is an `ExprTC` private member, and
// `PureChecker.cpp` is a separate translation unit with no `ExprTC`; the
// binder's DIAGNOSTIC half needs the typechecker's context, but its
// APPLICABILITY half needs nothing but the formal list and the argument
// list — both of which the caller already holds. The rule applied here is
// the rule the binder applies:
//
//   * positional arguments fill the formals in order;
//   * a named argument binds to the formal of that name;
//   * a positional argument may not follow a named one;
//   * no formal may be bound twice;
//   * every formal left unbound must declare a default.
//
// The binder also checks the arguments' TYPES against the formals' (§5.1);
// that half is not repeated here — a candidate this predicate admits may
// still be inapplicable by type, and §22.6's ranking (the typecheck pass)
// decides between such candidates.
static bool ctorBindsArgs(const sema::FormalsView& formals,
                           const std::vector<ast::CallArgument>& args) {
    const std::size_t n = formals.size();
    std::vector<bool> bound(n, false);
    bool sawNamed = false;
    std::size_t pos = 0;  // the next positional formal
    for (const ast::CallArgument& a : args) {
        if (a.name.empty()) {
            // per §22.5: positional arguments precede named arguments.
            if (sawNamed) return false;
            if (pos >= n) return false;  // more arguments than formals
            bound[pos] = true;
            ++pos;
            continue;
        }
        sawNamed = true;
        std::size_t idx = n;
        for (std::size_t j = 0; j < n; ++j)
            if (formals.name(j) == a.name) {
                idx = j;
                break;
            }
        if (idx >= n) return false;     // no formal of that name
        if (bound[idx]) return false;   // bound twice
        bound[idx] = true;
    }
    // per §22.5: every formal left unbound takes its declared default.
    for (std::size_t j = 0; j < n; ++j)
        if (!bound[j] && !formals.hasDefault(j)) return false;
    return true;
}

// fix-forward #8134: the invoked constructor of `new E(args)`, for a class
// whose selection the typecheck pass did not publish. See the note above for
// the two rules and why the old arity match was not enough.
static const ConstructorSymbol* findInvokedUserCtor(
    const TypeSymbol* cls, const std::vector<ast::CallArgument>& args) {
    std::vector<const ConstructorSymbol*> ctors;
    for (Symbol* m : cls->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs || cs->isStatic) continue;
        ctors.push_back(cs);
    }
    if (ctors.empty()) return nullptr;
    // (a) ONE non-static constructor: it is the invoked one.
    if (ctors.size() == 1) return ctors[0];
    // (b) SEVERAL: the unique candidate whose formals the call binds.
    const ConstructorSymbol* bound = nullptr;
    for (const ConstructorSymbol* cs : ctors) {
        if (!ctorBindsArgs(sema::FormalsView::of(*cs), args)) continue;
        if (bound) return nullptr;  // two candidates bind: §22.6 decides
        bound = cs;
    }
    return bound;
}

// §22.15 "Members classified by rule" (#2061, item 2): record construction
// `new R(args)` is admitted in a pure body iff the invoked constructor is
// compiler-synthesized-pure (positional record — class-identity check) OR
// `pure`-declared. The synthesized positional ctor copies args into
// positional fields — a birth-store, pure for the same reason a pure
// ctor's field stores are — and is pure-classified iff the record class
// is auto-hashable. A body record has no synthesized ctor: its
// authored ctor of matching arity must carry `pure`. The arity match here
// is for resolving WHICH user-declared ctor is invoked (a class-identity
// question per record class); the PURITY of the construction is the
// CLASS's classification, not an arity-dependent verdict.
//
// Returns true iff the NewExpr constructs a record AND the invoked ctor is
// pure-classified; false otherwise (caller may then report a §22.15
// violation).
bool PureBodyWalker::recordConstructionIsPure(const ast::NewExpr& ne) {
    if (!ne.type) return false;
    const TypeRef* rt = ctx_.resolvedType(*ne.type);
    if (!rt || rt->kind != TypeRef::Kind::Named || !rt->declaration)
        return false;
    TypeSymbol* rec = rt->declaration;
    if (rec->kind != ast::TypeDeclKind::Record) return false;
    // CLASS-IDENTITY: a positional record has a synthesized ctor whose
    // purity is the record-class's auto-hashable classification.
    //
    // fix #3631 (§9.1 / §9.2, v0.286): route through the ONE both-sources
    // positional-record accessor rather than re-deriving the
    // `hasPositionalFields` / AST-decl-fallback shape here (see Symbol.hpp's
    // doc comment on `recordIsPositional`). BEHAVIOR-FREE: this site already
    // read `hasPositionalFields` first with the identical `ast::TypeDecl`
    // fallback, so this is a pure mechanical dedup, not a widening (§6b).
    const bool isPositional = recordIsPositional(rec);
    const std::size_t arity = ne.args.size();
    const ConstructorSymbol* userCtor =
        findUserCtorByArity(rec, arity);
    if (isPositional) {
        // When a user declared a ctor with matching arity, it shadows the
        // synthesized positional one (§9.5); that ctor must carry `pure`.
        // Otherwise the synthesized positional ctor is invoked — its
        // purity follows from the record's CLASS identity, not from any
        // arity-specific check.
        if (userCtor) return constructorIsPure(userCtor);
        return isPositionalRecordEqualityBearing(rec, ctx_);
    }
    // Body record: there is no synthesized ctor, so the invoked user-
    // declared ctor must be `pure`-declared.
    if (!userCtor) return false;
    return constructorIsPure(userCtor);
}

// §22.15 "Pure constructors" (#2061, item 1): a pure ctor's explicit
// `base(...)`/`this(...)` chain target must itself be pure (§22.15:
// "A pure constructor's `base(…)`/`this(…)` chain target must itself be
// pure"). The chain call surfaces as the body's first ExpressionStmt whose
// expression is a CallExpr on `this` or `base` (Node.hpp CtorChain).
//
// Resolution narrows by class: `this(...)` targets a ctor in the SAME
// class as this ctor; `base(...)` targets a ctor in the first class base.
// Within the narrowed class, the call's argument count picks the single
// overload — there is no further overload ambiguity in the chain call.
void PureBodyWalker::checkPureCtorChainPurity(
    const ast::ConstructorDecl& cd, const ConstructorSymbol& cs) {
    if (cd.body.empty() || !cd.body.front()) return;
    auto* es = dynamic_cast<const ast::ExpressionStmt*>(cd.body.front().get());
    if (!es || !es->expr) return;
    auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
    if (!call || !call->callee) return;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(call->callee.get());
    if (!id) return;
    const bool isThisChain = (id->name == "this");
    const bool isBaseChain = (id->name == "base");
    if (!isThisChain && !isBaseChain) return;  // not a chain call
    // CLASS-IDENTITY narrowing: `this(args)` → same class (cs.parent);
    // `base(args)` → first class base (mirrors codegen's chain target
    // resolution). The implicit Object base has no constructor symbols —
    // admit silently when no explicit base exists.
    TypeSymbol* targetClass = nullptr;
    if (isThisChain) {
        targetClass = cs.parent;
    } else {
        for (TypeSymbol* b : cs.parent->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) {
                targetClass = b;
                break;
            }
    }
    if (!targetClass) return;
    const std::size_t arity = call->args.size();
    // fix #3174 (§22.15, v0.254 — family M8 of umbrella #3149/#3152): resolve
    // the chain target to its SYMBOL, not to an `ast::ConstructorDecl*`.
    //
    // `ModuleLoader` attaches a re-parsed `ast::ConstructorDecl` only for a
    // GENERIC type or a RECORD, so for a non-generic imported base class every
    // candidate's `decl` is null; the old `if (!cd2) continue;` therefore
    // dropped the entire candidate set and the "no matching ctor" early-out
    // below admitted the program silently. MEASURED on `ec6a02f`: a local
    // `pure constructor` chaining `base(n)` to an imported IMPURE base ctor
    // compiled with exit 0, while the character-identical single-module twin
    // was rejected with this function's own diagnostic.
    //
    // ⚠ THE WIDENING, EXACTLY. `declTarget` keeps the OLD selection verbatim —
    // last matching candidate that carries an `ast::ConstructorDecl` — and is
    // preferred whenever it exists, so every program in which the old code
    // found a target keeps that target and that verdict. `symTarget` is the
    // strictly larger set (it drops only the `decl != nullptr` filter), and it
    // is consulted ONLY when the old selection found nothing. So the single new
    // admission is: "a matching, non-static, non-self constructor SYMBOL with
    // no attached declaration" — which is exactly the imported non-generic
    // class, the case this fix exists for. In particular the old code's
    // last-match-wins tie-break is untouched wherever it had a match to break.
    const ast::ConstructorDecl* declTarget = nullptr;
    const ConstructorSymbol* symTarget = nullptr;
    for (Symbol* m : targetClass->members) {
        auto* ct = dynamic_cast<ConstructorSymbol*>(m);
        if (!ct || ct->isStatic) continue;
        if (ct == &cs) continue;  // `this(...)` cannot recurse to itself
        if (ct->params.size() != arity) continue;
        symTarget = ct;
        if (const auto* cd2 =
                dynamic_cast<const ast::ConstructorDecl*>(ct->decl))
            declTarget = cd2;
    }
    if (!symTarget) {
        // No matching ctor — a primary-ctor class chain (no
        // ConstructorSymbol: the parser synthesizes the chain's body), or
        // an arity mismatch already caught by sema.
        //
        // fix #8163 (§22.15, v0.355): the PRIMARY-ctor case is NOT a
        // free pass. §22.15 "Pure constructors" — "A pure constructor's
        // `base(…)`/`this(…)` chain target must itself be pure" — reaches
        // a class primary, because §12.8's chain in a primary-ctor class
        // ENDS at the primary (the primary is that class's only
        // constructor that does not chain with `this(...)`), and the
        // primary's CONSTRUCTION runs as part of every construction of the
        // class: §22.10's inheritance-clause base call (§22.8 step 0), then
        // the class's field initializers (§22.8 step 2).
        //
        // ⚠ THE OLD COMMENT HERE WAS FALSE. It claimed "the chain target's
        // purity is provable by the primary-ctor birth-store rule". The
        // birth-store rule (§22.15 "Pure constructors") covers the class's
        // field INITIALIZERS — and the class-level walk in `checkOneType`
        // enforces them ("a class with a pure constructor must have field
        // initializers satisfying the pure-body rules"). It says NOTHING
        // about the primary's BASE-CALL ARGUMENTS or its PARAMETER DEFAULTS,
        // which are ordinary user expressions evaluated during that very
        // construction. So `class MyError(message: string): Exception(
        // Helper.Msg(message))` + `pure constructor() this("default")`
        // chained to a primary that runs an impure call, and the §22.15
        // rule never fired.
        //
        // The field initializers are NOT walked again here: the class-level
        // birth-store walk already reports them, at the initializer, and a
        // second walk would double-report the same impurity.
        //
        // An arity mismatch is still admitted silently — sema owns that
        // diagnostic (§22.5 / §22.6), not this rule.
        //
        // ⚠ This branch's own comment used to justify an outcome it did not
        // describe: before #3174 it also ran for every imported base class,
        // which is neither a primary-ctor chain nor an arity mismatch. It now
        // means what it says.
        if (isThisChain) {
            // `this(...)` in a class with a primary constructor chains to
            // THAT primary (§12.8: the chain must end; §22.10: a body
            // constructor of a primary-ctor class chains with `this(...)`).
            // An IMPORTED class carries no `ast::TypeDecl` here, so its
            // primary's content is not re-checkable in this compilation —
            // the same admission the pre-#8163 code made.
            const auto* td =
                // SEAM-DEBT: #8182 an imported primary's content is admitted unchecked
                dynamic_cast<const ast::TypeDecl*>(targetClass->decl);
            if (td && td->kind == ast::TypeDeclKind::Class &&
                td->hasPositionalFields) {
                checkPrimaryCtorConstructionContent(*td, targetClass);
            }
        }
        return;
    }
    if (declTarget) {
        if (declTarget->isPure) return;  // chain target is pure — OK
    } else if (constructorIsPure(symTarget)) {
        return;  // imported chain target is pure — OK
    }
    reportPureViolation(
        call->span.begin, "constructor chain call",
        std::string(isThisChain ? "this(" : "base(") + "...) target is "
        "not pure — a pure constructor's chain target must itself be "
        "declared `pure`");
}

// fix #8163 (§22.15, v0.355): the construction content of a class PRIMARY
// constructor, walked under the pure-body rules.
//
// THE TWO EXPRESSION GROUPS.
//
//   * The inheritance clause's BASE-CALL ARGUMENTS (§22.10 / §22.8 step 0):
//     `class D(v: int): Base(v)` — the arguments are evaluated as part of
//     the construction, before the field initializers run.
//
//   * The primary's PARAMETER DEFAULT expressions (§22.5): a default-value
//     expression evaluates when the argument is omitted, i.e. inside the
//     construction this chain reaches.
//
// Both are ordinary user expressions, and §22.15's pure-body rules apply to
// them exactly as they apply to a pure member's body — the same rules the
// class-level birth-store walk applies to this class's field initializers.
//
// THE DIAGNOSTIC. The walker's own §22.15 diagnostics fire here, verbatim —
// "call is not admitted in a pure body — the resolved target `X` is not
// pure-declared (§22.15)" and the rest of the closed list's contrapositive.
// No new wording is introduced: the same impurity, in the same class, is
// already reported in exactly this text when it sits in a field initializer
// of a class with a pure constructor (the class-level birth-store walk), and
// a class primary's construction content is the same kind of material.
//
// THE SCOPE. §22.15's rule is about the CHAIN TARGET. For a `this(...)`
// chain in a primary-ctor class the target IS the primary (§12.8: the chain
// must end, and the primary is the only constructor of such a class that
// does not chain with `this(...)`), and the primary's construction runs for
// EVERY instance of the class — so this content is part of what a pure
// constructor in that class promises. The purity of the base constructor
// the primary's clause invokes is checked below (#8163); the BASE class's
// own field initializers are a further step up the hierarchy and are NOT
// walked here.
void PureBodyWalker::checkPrimaryCtorConstructionContent(
    const ast::TypeDecl& td, const TypeSymbol* cls, int depth) {
    for (const ast::CallArgument& a : td.primaryBaseCtorArgs) {
        if (!a.value) continue;
        checkExpr(a.value.get());
    }
    for (const ast::Param& p : td.positionalFields) {
        if (!p.defaultValue) continue;
        checkExpr(p.defaultValue.get());
    }
    // fix #8163b (§22.15, v0.355): the BASE-CONSTRUCTOR TARGET the
    // inheritance clause invokes (§22.10 / §22.8 step 0) runs as part of this
    // construction too, so it must itself be pure ("A pure constructor's
    // `base(...)`/`this(...)` chain target must itself be pure").
    if (!cls || depth > 16) return;
    const TypeSymbol* base = nullptr;
    for (const TypeSymbol* b : cls->baseTypes)
        if (b && b->kind == ast::TypeDeclKind::Class) {
            base = b;
            break;
        }
    if (!base) return;
    const SourceLocation at = !td.primaryBaseCtorArgs.empty() &&
                                      td.primaryBaseCtorArgs.front().value
                                  ? td.primaryBaseCtorArgs.front().value->span.begin
                                  : td.span.begin;
    // The invoked base constructor, read off the SYMBOLS (an imported base
    // carries no AST). fix-forward #8163 (§22.10 / §11.1 / §22.5 / §22.6,
    // v0.355): a primary-ctor base's PRIMARY is a member of its constructor
    // overload set (§11.1: body constructors are declared "in addition" to
    // it) yet has no ConstructorSymbol, so the candidate set is the base's
    // non-static body constructors PLUS its primary formal list, and a
    // candidate is the invoked one only when the clause's arguments BIND it
    // (§22.5) — never merely because it is the only body constructor (the
    // §22.10 idiom `class MyError(message: string): Exception(message)` with
    // a convenience `constructor() this("default")` is invoked through its
    // PRIMARY by `MyError(message)`). Several binding candidates are
    // narrowed by an exact argument-type match (§22.6's identity rank).
    //
    // Outcomes: a BODY constructor -> its purity decides (a pure one's own
    // chain is checked where it is declared, §22.15); the PRIMARY, or an
    // implicit default constructor (no constructor at all) -> climb to that
    // class's base through the both-sources clause accessor; UNDECIDED (no
    // unique candidate) -> stop silently unless every candidate is an impure
    // body constructor. The climbed primaries' own argument expressions are
    // not walked (that would need their AST; SEAM-DEBT #8182).
    //
    // An IMPORTED primary-ctor class whose clause arguments did not replay
    // (`primaryBaseCtorArgsOf` null; MEASURED for a non-generic imported
    // middle primary) leaves the next step's arguments UNKNOWN, not empty:
    // there every non-static constructor (and the primary) is a candidate,
    // and only a UNIQUE one decides.
    auto argMatchesType = [&](const ast::CallArgument& a,
                              const TypeRef* formal) {
        if (!a.value || !formal) return true;  // unknown: cannot exclude
        const TypeRef* at = ctx_.exprType(*a.value);
        if (!at) return true;
        return typeRefEqual(at, formal);
    };
    const std::vector<ast::CallArgument>* args = &td.primaryBaseCtorArgs;
    static const std::vector<ast::CallArgument> kNoArgs;
    bool argsKnown = true;
    for (; base && depth <= 16; ++depth) {
        std::vector<const ConstructorSymbol*> bodyCands;
        std::size_t nonStatic = 0;
        for (Symbol* m : base->members) {
            auto* cs = dynamic_cast<ConstructorSymbol*>(m);
            if (!cs || cs->isStatic) continue;
            ++nonStatic;
            if (!argsKnown ||
                ctorBindsArgs(sema::FormalsView::of(*cs), *args))
                bodyCands.push_back(cs);
        }
        const std::vector<const Symbol*> primaryFields =
            base->hasPositionalFields ? recordPositionalFieldSymbols(base)
                                      : std::vector<const Symbol*>{};
        bool primaryCand =
            base->hasPositionalFields &&
            (!argsKnown ||
             ctorBindsArgs(sema::FormalsView::of(primaryFields), *args));
        const bool implicitDefault =
            nonStatic == 0 && !base->hasPositionalFields;
        if (argsKnown && !implicitDefault &&
            bodyCands.size() + (primaryCand ? 1u : 0u) > 1) {
            // §22.6: narrow by exact type, all-positional clauses of a
            // NON-generic base only (a formal spelled with a type parameter
            // is not compared against the substituted argument type).
            bool narrow = base->genericParams.empty();
            for (const ast::CallArgument& a : *args)
                if (!a.name.empty()) narrow = false;
            if (narrow) {
                std::vector<const ConstructorSymbol*> exact;
                for (const ConstructorSymbol* cs : bodyCands) {
                    bool ok = true;
                    for (std::size_t i = 0; ok && i < args->size(); ++i) {
                        const TypeRef* ft = formalParamResolvedType(*cs, i);
                        if (!ft)
                            if (const ast::Type* dt =
                                    formalParamDeclType(*cs, i))
                                ft = ctx_.resolvedType(*dt);
                        ok = argMatchesType((*args)[i], ft);
                    }
                    if (ok) exact.push_back(cs);
                }
                bool primaryExact = false;
                if (primaryCand) {
                    primaryExact = true;
                    for (std::size_t i = 0;
                         primaryExact && i < args->size() &&
                         i < primaryFields.size();
                         ++i) {
                        auto* fs =
                            dynamic_cast<const FieldSymbol*>(primaryFields[i]);
                        const TypeRef* ft =
                            fs && fs->posParam && fs->posParam->type
                                ? ctx_.resolvedType(*fs->posParam->type)
                                : nullptr;
                        primaryExact = argMatchesType((*args)[i], ft);
                    }
                }
                if (exact.size() + (primaryExact ? 1u : 0u) == 1) {
                    bodyCands = exact;
                    primaryCand = primaryExact;
                }
            }
        }
        const std::size_t nCands = bodyCands.size() + (primaryCand ? 1u : 0u);
        if (!implicitDefault && nCands != 1) {
            // UNDECIDED: report only what every reading agrees on — every
            // candidate an impure body constructor.
            if (primaryCand || bodyCands.empty()) return;
            for (const ConstructorSymbol* cs : bodyCands)
                if (constructorIsPure(cs)) return;
            reportPureViolation(
                at, "constructor chain call",
                "base(...) target is not pure \xE2\x80\x94 a pure "
                "constructor's chain target must itself be declared "
                "`pure`");
            return;
        }
        if (!implicitDefault && !primaryCand) {
            if (!constructorIsPure(bodyCands.front()))
                reportPureViolation(
                    at, "constructor chain call",
                    "base(...) target is not pure \xE2\x80\x94 a pure "
                    "constructor's chain target must itself be declared "
                    "`pure`");
            return;
        }
        // The PRIMARY (or an implicit default constructor): climb.
        const auto* baseArgs = primaryBaseCtorArgsOf(base);
        const TypeSymbol* next = nullptr;
        for (const TypeSymbol* b : base->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) {
                next = b;
                break;
            }
        // No clause arguments: KNOWN empty for a local class or an implicit
        // default constructor; UNKNOWN for an imported primary (see above).
        argsKnown = baseArgs || base->decl || implicitDefault;
        args = baseArgs ? baseArgs : &kNoArgs;
        base = next;
    }
}

bool PureBodyWalker::isEnumeratorConstructingSource(
    const TypeRef* rt) const {
    // §22.15 rule 5: an IEnumerable<T> source constructs an enumerator class
    // instance. Range<T> of primitive steppable type, inline arrays, spans,
    // and both OwningBuffer<T> presentations use direct counted/indexed
    // loops. A CONCRETE Array<T>/OwningArray<T> source, a class-typed
    // OwningBuffer<T>, and a `string` source use the direct indexed loop,
    // which is the NORMATIVE semantics inside a pure body; an Array<T> whose
    // element is a type parameter is not concrete at this check site and is
    // rejected.
    if (!rt) return false;
    std::string_view head = rt->headName();
    // Concrete Array<T> and OwningArray<T> with a non-type-parameter element
    // type use direct indexed loops — admitted in pure bodies. Array<T> with
    // a type-parameter element type is not concrete and is rejected.
    if (head == "Array" || head == "OwningArray") {
        if (rt->genericArgs.size() == 1 && rt->genericArgs[0] &&
            rt->genericArgs[0]->kind == TypeRef::Kind::Named &&
            !rt->genericArgs[0]->declaration) {
            // Element type is a type parameter — not concrete, rejected.
            return true;
        }
        // Concrete element type — use direct indexed loop, admitted.
        return false;
    }
    // §22.15 rule 5: "A source whose static type is an interface
    // (IEnumerable<T> included) or a generic type parameter — even one
    // instantiated at a concrete sequence type — remains rejected."
    //
    // A bare type parameter (e.g. `T` in `method Foo<T>(source: T)`) has
    // kind==Named and declaration==null.  Its headName() is the parameter's
    // own name ("T"), which matches no arm below, so it falls through to
    // `return false` and the for is incorrectly admitted — later dying in
    // codegen with "not yet implemented: codegen for this for statement".
    //
    // Reuse the same Named+null-declaration sentinel the Array<T> arm uses
    // for a type-parameter element type.  Unlike Array<T> (which reaches
    // this point with a non-null head that matched no arm), a bare type
    // parameter IS the head — so we must additionally check that it has
    // NO generic args, to avoid incorrectly rejecting an instantiated
    // builtin type (OwningBuffer<Dog>, etc.) whose declaration is also null.
    // // per §22.15
    if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
        rt->genericArgs.empty())
        return true;
    // Any iterable INTERFACE head: always constructs an enumerator, rejected
    // even with a concrete element type. §22.15 rule 5 rejects "a source whose
    // static type is an interface (IEnumerable<T> included)".
    //
    // Read the shared classifier rather than a private name list: the raw
    // `head == "IEnumerable" || head == "IEnumerator"` spelling missed the
    // async and owning members of the same 8-member family
    // (IAsyncEnumerable, IAsyncEnumerator, IOwningEnumerable, …), which fell
    // through to the final `return false` and were therefore ADMITTED. // per §22.15
    if (common::isIterableInterfaceHead(head))
        return true;
    // OwningBuffer<T> (stdlib §4.14): BOTH presentations are admitted, and
    // for different reasons the clause states separately.
    //
    // Value-typed T is one of §29.2's "Direct-indexed sources" — its
    // desugaring is already the direct indexed loop, and §22.15 rule 5
    // lists it among the sources admitted on that ground.
    //
    // Class-typed T is the enumerator-constructing presentation (stdlib
    // §4.14 declares `class OwningBuffer<T>: IEnumerable<T?>` for class-typed
    // T, and its table note 5 says "the type implements IEnumerable<T?> and
    // the standard enumerator path applies, with the elision rule below").
    // It is admitted because §22.15 rule 5 NAMES it in the elision clause:
    // "over a source whose static type at this body's own check site is
    // concretely Array<T>, OwningArray<T>, class-typed OwningBuffer<T>, or
    // string, §29.2's admitted protocol elision is likewise the normative
    // semantics inside a pure body". The clause's stated reason is that
    // `OwningBuffer<T>.Length` is `pure`-declared as an
    // immutable-once-allocated extent read (stdlib §4.14), so the bounds
    // read is rule-1 material.
    //
    // The two presentations are ONE generic class (stdlib §4.14: "The two
    // presentations are different specializations of the same generic class.
    // The compiler resolves T's category at instantiation"), so `head` does
    // not — and must not — discriminate between them here: both are admitted,
    // and stating the arm makes that a decision of this clause rather than the
    // fall-through below.
    if (head == "OwningBuffer") return false;
    // The remaining collection families. `string` is NOT here: it is one of
    // the four types the elision clause names concretely, so it is admitted
    // by falling through (the compiler lowers a `string` source to a direct
    // indexed loop — `lowerStringFor` reads `Length` and per-code-point reads
    // and constructs no enumerator, §29.2).
    if (head == "List" || head == "OwningList" ||
        head == "OwningDictionary" || head == "OwningSet" ||
        head == "Dictionary" || head == "Set" || head == "Stack" ||
        head == "Queue" || head == "LinkedList" || head == "OwningStack" ||
        head == "OwningQueue" || head == "OwningLinkedList")
        return true;
    return false;
}

// fix #3279 (v0.263, stdlib §9.8 + spec §22.15): the `in` membership
// operator's callee is `r.Contains(value)` (spec §17.5). The
// calls-to-pure-members rule (spec §22.15) admits the operator iff the
// callee is pure-classified.
//
// Pure-classified callees:
//   - Range<T>: pure-declared on the stdlib intrinsic (stdlib §9.8;
//     carried by the `pure` modifier on the Range declaration).
//   - string: pure-declared on `string.Contains` (stdlib §9.6).
//   - ReadOnlyStringSpan: pure-declared on `Contains` for the three
//     `ReadOnlyStringSpan` overloads (stdlib §3.3).
//   - User-declared types: pure iff a member named `Contains` exists on
//     the receiver's symbol-side member list AND that member is
//     pure-declared (carries `pure` in its modifiers list — the same
//     check `callTargetIsPure` runs at a CallExpr to `Contains`).
//
// Rejected:
//   - Array<T>, OwningArray<T> (Contains not pure-declared in stdlib).
//   - Any user-declared type whose `Contains` does NOT carry `pure`.
//   - Unresolved receivers (the resolution error was reported elsewhere;
//     this check does not double-report).
//
// The helper does NOT descend into codegen's `in`-operator lowering
// (which is inlined to a range-membership scan in LlvmBackend_Exprs.cpp).
// The §22.15 rule fires on the SEMANTIC callee — what the operator means,
// not how codegen lowers it — so a future lowering change cannot
// silently relax this check.
bool PureBodyWalker::inOperatorCalleeIsPure(const ast::BinaryExpr& be) const {
    if (!be.right) return true;  // resolution error elsewhere; no double-report
    const TypeRef* recvTy = ctx_.exprType(*be.right);
    if (!recvTy) return true;
    // The receiver's identity for stdlib-type membership testing. Builtin
    // TypeRefs constructed in-place (RangeExpr's Range, the string
    // builtin, ReadOnlyStringSpan) have no astNode and no importedName,
    // so headName() returns empty and we fall back to declaration->name.
    // headName() is the alias-transparent spelling per TypeRef.hpp, which
    // suffices for user-declared Named types and alias-resolved heads.
    std::string_view head = recvTy->headName();
    if (head.empty() && recvTy->declaration)
        head = recvTy->declaration->name;
    // Special cases by receiver head — these are the stdlib-declared
    // pure-Contains types whose purity is a documentation fact, not a
    // symbol-table lookup (the members may not exist as MemberSymbols on
    // the builtin TypeSymbol after ModuleLoader's import-side filtering
    // — see the #3152 umbrella, family F11/F13).
    //
    // Only types whose Contains is `public pure method Contains(...)`
    // in stdlib belong in this list. As of v0.277 the set is exactly
    // {Range<T>, string, ReadOnlyStringSpan}. The Span<T> / ReadOnlySpan<T>
    // family (§4.14) and ArraySpan<T> / ReadOnlyArraySpan<T> (§4.15)
    // declare Contains as a plain `method`, NOT `pure`, so they are
    // EXCLUDED from this list — `x in spanVal` over those receivers is
    // REJECTED by §22.15's calls-to-pure-members rule (the same way
    // `x in arr` over Array<T> is rejected). The BitReader/BitWriter
    // views declare no Contains member at all; their `in` operator is
    // rejected earlier by §17.5's structural lookup, so adding them to
    // this list would be unreachable in practice and misleading.
    if (head == "Range") return true;            // stdlib §9.8
    if (head == "string") return true;           // stdlib §9.6
    if (head == "ReadOnlyStringSpan") return true; // stdlib §3.3
    // Generic user-declared types: walk the receiver's symbol-side member
    // list looking for a `Contains` member and check its `pure` modifier.
    // This matches what `callTargetIsPure` does for an explicit
    // `r.Contains(x)` CallExpr, so the two paths agree (fix #3279's
    // stated invariant: "the marker is what admits a range test inside a
    // contract clause; the same marker admits `r.Contains(x)` in the
    // same position").
    if (recvTy->kind == TypeRef::Kind::Named && recvTy->declaration) {
        for (Symbol* m : recvTy->declaration->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms) continue;
            if (ms->name != "Contains") continue;
            // fix #3609 (§22.15, v0.286): the same predicate as
            // callTargetIsPure's MethodSymbol arm — now literally the same
            // function, which is what the comment here used to claim. It had
            // dropped the `ast::OperatorDecl` arm; that is unreachable behind
            // the `name != "Contains"` filter (an operator symbol is named for
            // its token), so restoring it admits nothing new.
            return memberIsPure(ms);
        }
    }
    // No matching Contains found, or non-Named receiver. Conservatively
    // reject: the operator's semantic callee is unprovably pure.
    return false;
}

bool PureBodyWalker::forSourceAdmitsEnumerator(
    const ast::Expr& iterable) const {
    // Admitted if NOT enumerator-constructing. Range<T>, inline arrays,
    // spans, and value-typed buffers are admitted.
    const TypeRef* rt = ctx_.exprType(iterable);
    return !isEnumeratorConstructingSource(rt);
}

// fix #8134 (§31.4, v0.355): §31.4's constructor-purity rule for a BUILT-IN
// exception type.
//
// §31.4, verbatim: "Every built-in exception type's `()` and `(message)`-
// form constructors (and `ArgumentException`/`ArgumentOutOfRangeException`'s
// `(message, paramName)` forms) are **`pure`** (§22.15 — the throw-carve-out
// enablers; all built-ins are trivial-dispose …). The `(message, cause)`
// chaining constructor is not pure (its snapshot builds a fresh `Array`)."
//
// So the invoked constructor's SHAPE decides:
//
//   * `()` — the parameterless form — `pure` for every built-in exception;
//   * `(message: string)` — the one-argument form — `pure` for every
//     built-in exception;
//   * two arguments — the `(message, paramName)` form for the two types
//     §31.4 NAMES (pure), and the `(message, cause)` chaining form for every
//     other built-in exception (NOT pure);
//   * any other arity — no such constructor (§31.4's closed constructor
//     surface), so no `pure` constructor is invoked.
//
// THE `()` FORM. §31.4's closing paragraph notes that `new
// ArgumentException()` is a compile error under §17.7's
// closed-constructor-surface rule — a SEPARATE defect from this one, and
// one this lane does not touch. The `()` and `(message)` forms are pure
// for EVERY built-in exception type here, so the pure-body rule does not
// change that program's verdict.
bool builtinExceptionCtorIsPure(std::string_view head, std::size_t arity) {
    if (arity == 0 || arity == 1) return true;
    if (arity == 2)
        return head == "ArgumentException" ||
               head == "ArgumentOutOfRangeException";
    return false;
}

bool PureBodyWalker::throwCarveOutAdmits(const ast::NewExpr& ne,
                                         std::string* why) {
    // §22.15 throw carve-out: (a) E's INVOKED constructor is `pure`-declared,
    // and (b) E is trivial-dispose.
    //
    // fix #8134 (§22.15 / §31.4, v0.355): the check now asks about the
    // INVOKED constructor. It used to ask "does E have a `pure` constructor
    // SOMEWHERE", which admitted two shapes §22.15's carve-out excludes:
    //
    //   * a USER class with a `pure` one-argument constructor AND an impure
    //     two-argument one: `throw new E("m", cause)` ran the IMPURE
    //     constructor and was admitted;
    //   * a BUILT-IN exception's `(message, cause)` chaining constructor,
    //     which §31.4 states is not pure ("The `(message, cause)` chaining
    //     constructor is not pure (its snapshot builds a fresh `Array`).").
    //
    // WHAT THE SPEC SAYS. §22.15's carve-out, verbatim: "`new E(args)` is
    // admitted only as the immediate operand of a `throw` statement or
    // `throw` expression, and only when **(a)** the invoked constructor of
    // `E` is `pure`-declared — covering, transitively, its chained
    // constructors and `E`'s field initializers — and **(b)** `E` is a
    // trivial-dispose class". The subject is the INVOKED constructor.
    if (!ne.type) return false;
    const TypeRef* rt = ctx_.resolvedType(*ne.type);
    if (!rt || rt->kind != TypeRef::Kind::Named) {
        // An unresolved target: the typecheck pass owns that diagnostic.
        // Conservatively admit here so the rule does not double-report.
        return true;
    }
    if (!rt->declaration) {
        // A BUILT-IN class — it carries no AST declaration and no artifact,
        // so §22.15's "Intrinsic purity (the stdlib contract)" makes the
        // LIBRARY'S OWN marker the authority (§31.4).
        const std::string_view head = rt->headName();
        if (!common::isBuiltinExceptionName(head)) {
            // Not an exception at all: §31.1's "the thrown value must be an
            // exception" owns this construction. Admit here so the
            // diagnostic blames the right line.
            return true;
        }
        if (builtinExceptionCtorIsPure(head, ne.args.size())) return true;
        if (why) {
            *why = std::string("the invoked constructor of `") +
                   std::string(head) +
                   "` is not `pure`-declared — §31.4 marks only the `()` and "
                   "`(message)` forms pure (and `ArgumentException`/"
                   "`ArgumentOutOfRangeException`'s `(message, paramName)` "
                   "form); ";
            if (ne.args.size() == 2)
                *why +=
                    "the `(message, cause)` chaining constructor is not pure";
            else
                *why += "no `pure` constructor of that arity exists";
        }
        return false;
    }
    TypeSymbol* targetDecl = rt->declaration;
    if (targetDecl->kind != ast::TypeDeclKind::Class) return false;
    // fix #3647: pass the resolved TypeRef `rt` itself (not just its
    // declaration `targetDecl`), so §3.6 rule 2's per-instantiation
    // substitution can run.
    if (!isTrivialDisposeClass(rt)) {
        if (why)
            *why = "`" + std::string(targetDecl->name) +
                   "` is not a trivial-dispose class — §22.15's throw "
                   "carve-out requires one (the after-arm disposal must "
                   "run no user code)";
        return false;
    }
    // (a) the INVOKED constructor must be pure.
    //
    // The typecheck pass already SELECTS the constructor the call invokes
    // and publishes it — `SemaContext::resolvedNewCtor`, the #4853 table
    // keyed on the `NewExpr`'s argument list. That is the authoritative
    // answer, so it is read first. The publication happens in the pass's
    // OVERLOAD arms only: the §22.6 winner of an overloaded set and the
    // imported twin's winner (`ExprTC::checkNewExprArgs` and
    // `ExprTC::checkImportedNewExprArgs`). The pass's SINGLE-constructor
    // arms — the LOCAL `ctors.size() == 1` tail and the imported twin —
    // bind the arguments and check them but publish nothing, so for every
    // class with exactly one constructor the selection is NOT on the table
    // and the carve-out has to identify the invoked constructor itself.
    //
    // The fallback is `findInvokedUserCtor` (below): ONE non-static
    // constructor is the invoked one, and otherwise the arguments are BOUND
    // to each candidate's formals (§22.5) so a defaulted formal left
    // unbound — and a named argument — are honoured.
    //
    // A class with NO identifiable constructor (an implicit default
    // constructor, a primary constructor, an ambiguous overload set) has no
    // pure constructor the carve-out can point at, so the carve-out does
    // not admit it — the same verdict the pre-#8134 "no pure ctor on the
    // class" answer gave.
    const ConstructorSymbol* invoked = ctx_.resolvedNewCtor(&ne.args);
    if (!invoked) invoked = findInvokedUserCtor(targetDecl, ne.args);
    if (!invoked) {
        if (why)
            *why = "the invoked constructor of `" +
                   std::string(targetDecl->name) +
                   "` could not be identified, so no `pure` constructor is "
                   "provable for it";
        return false;
    }
    // fix #3174 (§22.15, v0.254): through the one predicate — the same
    // both-sources purity predicate the CallExpr arm uses.
    if (constructorIsPure(invoked)) return true;
    if (why)
        *why = "the invoked constructor of `" + std::string(targetDecl->name) +
               "` is not `pure`-declared";
    return false;
}

void PureBodyWalker::checkThrowOperand(const ast::Expr* operand,
                                       SourceLocation throwLoc) {
    if (!operand) return;
    // §22.15 throw carve-out: `new E(args)` admitted iff trivial-dispose +
    // pure ctor. Any other throw operand is admitted (bare re-raise, a
    // caught binding read).
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(operand)) {
        std::string why;
        if (!throwCarveOutAdmits(*ne, &why)) {
            std::string detail =
                "the throw carve-out admits only `new E(...)` with a pure "
                "constructor of a trivial-dispose class";
            if (!why.empty()) detail += " — " + why;
            reportPureViolation(throwLoc, "class construction at `throw`",
                                detail);
        }
        // The new-expression's args still need to be checked.
        for (const auto& a : ne->args)
            if (a.value) checkExpr(a.value.get());
        return;
    }
    checkExpr(operand);
}

void PureBodyWalker::reportPureViolation(SourceLocation loc,
                                         const std::string& operation,
                                         const std::string& detail) {
    std::string msg = operation + " is not admitted in a pure body";
    if (!detail.empty()) msg += " — " + detail;
    msg += " (\xC2\xA7""22.15)";
    ctx_.diagnostics().report(Severity::Error, loc, msg);
}

Symbol* PureBodyWalker::targetRootSymbol(const ast::Expr& target) {
    // Walk the access path: peel `.member` / `[index]` / `!` to find the
    // identifier-rooted base. Mirrors checkLValueChainNotCallBased.
    const ast::Expr* e = &target;
    for (;;) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            e = ma->target.get();
            continue;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            e = ix->target.get();
            continue;
        }
        if (auto* bang = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            e = bang->operand.get();
            continue;
        }
        break;
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
        return ctx_.exprSymbol(*id);
    return nullptr;
}

void PureBodyWalker::checkAssignmentTarget(const ast::Expr& target) {
    // §22.15 rule 2 + contrapositive: the target root must be a local
    // variable or by-value parameter, and the access path must traverse
    // only value-typed storage. The first class-typed dereference leaves
    // the frame.
    //
    // Slice 2 implementation: classify by the root symbol's kind. A
    // LocalSymbol or ParameterSymbol root is admitted (frame-local). A
    // FieldSymbol root is admitted only in a pure constructor (birth-store
    // rule, §22.15 "Pure constructors"); otherwise it is a store whose
    // target root is an instance/static field — rejected.
    Symbol* rootSym = targetRootSymbol(target);
    if (!rootSym) {
        // `this.field := ...` is parsed as MemberAccess(IdentifierExpr this,
        // field) — the root is whatever `this` resolves to. Slice 2 keeps
        // this conservative: an unresolvable root in a non-ctor pure body
        // is rejected; the rule blame is on the assignment site.
        if (isConstructor_) return;  // birth-store admission
        reportPureViolation(
            target.span.begin, "assignment",
            "the target root is not a frame-local of this body");
        return;
    }
    if (dynamic_cast<LocalSymbol*>(rootSym)) return;
    if (dynamic_cast<ParameterSymbol*>(rootSym)) return;
    if (auto* fs = dynamic_cast<FieldSymbol*>(rootSym)) {
        // §22.15 birth-store rule: pure-constructor stores to fields of
        // `this` are admitted; static-field stores and instance-field
        // stores outside a pure ctor are rejected.
        if (isConstructor_) {
            bool isStatic = false;
            for (auto mod : fs->modifiers)
                if (mod == "static") isStatic = true;
            if (!isStatic) return;
        }
        reportPureViolation(
            target.span.begin, "assignment",
            std::string("the target root `") + std::string(fs->name) +
            "` is a field (a store outside the frame; pure-constructor "
            "birth-store admits only fields of `this`)");
        return;
    }
    if (auto* ps = dynamic_cast<PropertySymbol*>(rootSym)) {
        // §22.15 birth-store rule (fix #3208): the rule is stated over the
        // target ROOT, not over its spelling — `this.N := n;` and `N := n;`
        // are the same store (§2.7), and the spec admits both spellings of
        // a property write under the same rationale (the property's backing
        // storage is a field of `this`). The previous birth-store arm
        // recognised only FieldSymbol roots, so the bare property spelling
        // fell through to the "not a frame-local" reject even though the
        // `this.`-qualified spelling was silently admitted via the
        // unresolved-root branch above. Admit the PropertySymbol root under
        // the same gate the FieldSymbol arm uses.
        if (isConstructor_) {
            bool isStatic = false;
            for (auto mod : ps->modifiers)
                if (mod == "static") isStatic = true;
            if (!isStatic) return;
        }
        reportPureViolation(
            target.span.begin, "assignment",
            std::string("the target root `") + std::string(ps->name) +
            "` is a property (a store outside the frame; pure-constructor "
            "birth-store admits only properties of `this`)");
        return;
    }
    reportPureViolation(
        target.span.begin, "assignment",
        "the target root is not a frame-local of this body");
}

void PureBodyWalker::checkStmts(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (s) checkOneStmt(s.get());
    }
}

void PureBodyWalker::checkCaseArms(const std::vector<ast::CaseArm>& arms) {
    // fix #8134 (§22.15 / §A.7, v0.355): see the declaration's comment for
    // the three sub-expressions an arm carries and why all three are
    // subject to the closed pure-body list.
    for (const auto& arm : arms) {
        if (arm.guard) checkExpr(arm.guard.get());
        if (arm.exprBody) checkExpr(arm.exprBody.get());
        checkStmts(arm.stmts);
    }
}

void PureBodyWalker::checkOneStmt(const ast::Stmt* s) {
    if (!s) return;
    // Recurse through control flow.
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
        checkAssignmentTarget(*as->target);
        checkExpr(as->value.get());
        // §22.15 contrapositive: set(owned) stores — `owned` value
        // marker — are disposal-running.
        if (as->valueHasOwnedMarker) {
            reportPureViolation(
                as->span.begin, "owned-consumption assignment",
                "an `owned` marker on the RHS is a transfer (disposal-"
                "running)");
        }
        // fix #3628 (§22.15 rule 6 / §25.1, v0.286): §25.1, verbatim — "the
        // compound forms desugar mechanically to read-modify-write using the
        // underlying binary operator's overload." `t += b` therefore IS the
        // call `T::operator +`, and rule 6 governs it exactly as it governs
        // the spelled-out `t := t + b` (narrow #3610). `ExprTypecheck.cpp`'s
        // compound-assignment existence checks already resolve that overload
        // (including through inheritance and either operand's declaring
        // type, §25.2) and publish it via `setStmtOperatorSymbol` — read here
        // rather than re-resolved, so this is not a sixth predicate. Null for
        // every compound form that resolves no user overload: `:=`, `??=`,
        // a primitive/primitive pair, and an event `+=`/`-=` (§24 — not a
        // real compound assignment, resolves no 2-param `operator`).
        if (Symbol* opSym = ctx_.stmtOperatorSymbol(*as)) {
            std::string_view underlying = as->op;
            if (!underlying.empty() && underlying.back() == '=')
                underlying.remove_suffix(1);  // "+=" -> "+", "mod=" -> "mod"
            checkOperatorOverloadPurity(opSym, underlying, as->span.begin);
        }
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(s)) {
        checkAssignmentTarget(*is->target);
        // fix #3628 (§22.15 rule 6 / §17.6, v0.286): §17.6, verbatim — "the
        // statement `x++` is equivalent to `x := T.operator++(x)`" — a call,
        // exactly as rule 6 governs the binary/unary EXPRESSION operator
        // forms (narrow #3610). `ExprTypecheck.cpp`'s IncrementStmt overload-
        // existence check already resolves the `operator ++`/`operator --`
        // and publishes it via `setStmtOperatorSymbol`; null when the target
        // is a built-in numeric (§17.6 category (2) — no overload resolved).
        if (Symbol* opSym = ctx_.stmtOperatorSymbol(*is))
            checkOperatorOverloadPurity(opSym, is->op, is->span.begin);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
        checkExpr(es->expr.get());
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        if (rs->value) checkExpr(rs->value.get());
        return;
    }
    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
        // §22.16 (v0.203, #2053): `throw` is REJECTED in a contract clause
        // (clauseMode_), even when the operand would otherwise satisfy the
        // §22.15 throw carve-out — the carve-out remains available to
        // *callees* of the clause, not to the clause itself.
        if (clauseMode_) {
            reportPureViolation(
                ts->span.begin, "`throw`",
                "a contract clause states a proposition — the mechanism, "
                "message, and blame of a violation belong to §22.16's "
                "machinery, not to the clause's own throw (§22.16)");
        }
        // The operand is walked the same way in both modes: the `throw` is
        // the clause's ONE violation, and its `new E(...)` operand is
        // still the carve-out's business (§22.15: admitted "as the
        // immediate operand of a `throw` statement or `throw` expression"),
        // not a second report (fix-forward #7453).
        checkThrowOperand(ts->value.get(), ts->span.begin);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
        // §22.15 contrapositive: the consuming discard is rejected
        // (disposal-running / marker-free-position consume).
        reportPureViolation(
            ds->span.begin, "the consuming discard",
            "`_ := expr` is a marker-free consume (§28.3)");
        if (ds->value) checkExpr(ds->value.get());
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s)) {
        if (ifs->ifBranch.condition)
            checkExpr(ifs->ifBranch.condition.get());
        checkStmts(ifs->ifBranch.body);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) checkExpr(el.condition.get());
            checkStmts(el.body);
        }
        if (ifs->hasElse) checkStmts(ifs->elseBody);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        if (ws->condition) checkExpr(ws->condition.get());
        checkStmts(ws->body);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
        // §22.15 rule 5: a `for` loop's source must not construct an
        // enumerator. The awaited form is excluded by `pure`+`async`
        // already.
        if (fs->iterable &&
            !forSourceAdmitsEnumerator(*fs->iterable)) {
            reportPureViolation(
                fs->span.begin, "`for` loop source",
                "the source constructs an enumerator (an "
                "IEnumerable<T>-typed source, or an "
                "`Array<T>` whose element is a type parameter); declare "
                "the source at a concrete sequence type, or use an index "
                "loop `for var i in 0..<xs.Count`");
        }
        if (fs->iterable) checkExpr(fs->iterable.get());
        if (fs->withExpr) checkExpr(fs->withExpr.get());
        checkStmts(fs->body);
        return;
    }
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s)) {
        checkStmts(db->body);
        return;
    }
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
        // §22.15 rule 2: local declarations are admitted. The
        // initializer expression still needs to be checked.
        if (ld->initializer) checkExpr(ld->initializer.get());
        // §22.15 contrapositive: `using var` (sync) is disposal-running.
        if (ld->isUsing && !ld->isAwaitUsing) {
            reportPureViolation(
                ld->span.begin, "`using var`",
                "scope-end disposal is disposal-running user code");
        }
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
        if (lc->value) checkExpr(lc->value.get());
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
        // §22.15 "Where pure is rejected": iterator methods (yield)
        // cannot be pure — Slice 1 already rejected at signature level.
        // A yield surviving into the body check is unreachable in a
        // well-formed program.
        if (ys->value) checkExpr(ys->value.get());
        return;
    }
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(s)) {
        checkStmts(trys->body);
        for (const auto& arm : trys->catches) {
            if (arm.guard) checkExpr(arm.guard.get());
            // CatchArm::body is a single Stmt (usually a DoEndBlock);
            // unwrap one level so its inner statements are visited.
            if (arm.body) {
                if (auto* db = dynamic_cast<ast::DoEndBlock*>(
                        arm.body.get()))
                    checkStmts(db->body);
                else
                    checkOneStmt(arm.body.get());
            }
        }
        if (trys->hasFinally) checkStmts(trys->finallyBody);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
        if (cs->scrutinee) checkExpr(cs->scrutinee.get());
        // fix #8134 (§22.15 / §A.7, v0.355): the statement-position `case`
        // walked its arms' STATEMENT bodies but not their `when` GUARDS — the
        // same missing-sub-expression the expression-position form had. Both
        // forms now share `checkCaseArms`.
        checkCaseArms(cs->arms);
        return;
    }
    // Break / Continue / etc. — no effect.
}

void PureBodyWalker::checkExpr(const ast::Expr* e) {
    if (!e) return;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        // §22.15 rule 6 + contrapositive: invoke of function-typed value
        // is prohibited. Detect by: callee has function-typed exprType but
        // no resolved member symbol (i.e. a local/parameter holding a
        // function value).
        Symbol* calleeSym = ctx_.exprSymbol(*ce->callee);
        const TypeRef* calleeTy = ctx_.exprType(*ce->callee);
        const bool isFunctionTypedValue =
            !dynamic_cast<MethodSymbol*>(calleeSym) &&
            !dynamic_cast<ConstructorSymbol*>(calleeSym) &&
            calleeTy && calleeTy->kind == TypeRef::Kind::Function;
        if (isFunctionTypedValue) {
            reportPureViolation(
                ce->span.begin, "invocation of a function-typed value",
                "the value's type carries no effect information");
        } else if (!calleeSym) {
            // §22.15 "Members classified by rule" (#2061, item 2): the
            // callee resolves to no symbol when the call targets a record's
            // synthesized member — codegen synthesizes the body, so sema
            // leaves no MethodSymbol on the callee. Classify the call by
            // the receiver's record-class identity (the synthesized
            // member's purity is a class property, NOT an arity match):
            //   - positional record: synthesized Equals/GetHashCode/
            //     ToString are pure iff the record is auto-hashable
            //     (every field's dispatch is pure under §22.15 rules 1+
            //     3+6, via the §9.2/§9.3 admissibility helpers).
            //   - non-record calls or unresolved callees of other kinds
            //     (codegen fallthrough / member-not-found) are admitted:
            //     the typecheck pass already reported the lookup failure,
            //     and the pure-body checker does not double-report.
            bool isSynthRecMember = false;
            const bool recSynthPure =
                recordSynthesizedCallIsPure(*ce, &isSynthRecMember);
            if (isSynthRecMember && !recSynthPure) {
                reportPureViolation(
                    ce->span.begin,
                    "call to a record-synthesized member",
                    "the receiver record is not auto-hashable — "
                    "a class-typed field's virtual dispatch disqualifies "
                    "the synthesized Equals/GetHashCode/ToString; declare "
                    "`=`/`<>` with `pure` overrides, or simplify the "
                    "record to the auto-hashable form (§22.15 "
                    "\"Members classified by rule\")");
            }
        } else if (!callTargetIsPure(calleeSym)) {
            // §22.15 rule 6: a call to a member not pure-declared or pure-
            // classified. Admit when the callee is unresolved (the
            // typecheck pass already reported the lookup failure).
            if (calleeSym) {
                std::string name;
                if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym))
                    name = std::string(ms->name);
                else if (auto* cs2 =
                             dynamic_cast<ConstructorSymbol*>(calleeSym))
                    name = std::string(cs2->parent ? cs2->parent->name
                                                   : std::string_view());
                reportPureViolation(
                    ce->span.begin, "call",
                    std::string("the resolved target `") + name +
                    "` is not pure-declared");
            }
        }
        // Recurse into callee + args. The callee itself may be a member
        // access (which involves reads — fine).
        {
            const bool savedCallee = inCallee_;
            inCallee_ = true;
            checkExpr(ce->callee.get());
            inCallee_ = savedCallee;
        }
        for (const auto& a : ce->args)
            if (a.value) checkExpr(a.value.get());
        // §22.15 contrapositive: an `owned` argument is owned-consuming.
        for (const auto& a : ce->args) {
            if (a.modifier == "owned" || a.modifier == "scoped" ||
                a.modifier == "ref" || a.modifier == "out" ||
                a.modifier == "out owned" || a.modifier == "out var" ||
                a.modifier == "out using var" ||
                a.modifier == "out owned var") {
                reportPureViolation(
                    ce->span.begin, "call argument",
                    std::string("a `") + std::string(a.modifier) +
                    "` argument is not admitted in a pure call");
            }
        }
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
        // §22.15 contrapositive: class construction outside the throw
        // carve-out is rejected. The throw-carve-out arm in checkStmts
        // handles the `throw new E(...)` case directly; a NewExpr reached
        // here is outside that position.
        // Slice 2 admits value/record/enum construction and trivial-dispose
        // box construction; class construction with a non-pure ctor is the
        // load-bearing rejection. Slice 2's record path was a silent
        // admit; #2061 refines it to require a pure-classified record ctor
        // (synthesized positional ctor when admissible; `pure`-declared
        // user ctor otherwise).
        if (ne->type) {
            const TypeRef* rt = ctx_.resolvedType(*ne->type);
            if (rt && rt->kind == TypeRef::Kind::Named &&
                rt->declaration &&
                rt->declaration->kind == ast::TypeDeclKind::Class) {
                reportPureViolation(
                    ne->span.begin, "class construction",
                    "`new C(...)` runs the constructor and destructor of "
                    "user code (admitted only at the throw operand; the "
                    "throw carve-out requires a pure ctor + trivial-"
                    "dispose class)");
            } else if (rt && rt->kind == TypeRef::Kind::Named &&
                       rt->declaration &&
                       rt->declaration->kind == ast::TypeDeclKind::Record) {
                // §22.15 "Members classified by rule" (#2061, item 2):
                // record construction `new R(args)` is admitted iff the
                // invoked ctor is compiler-synthesized-pure (positional
                // record auto-hashable) or `pure`-declared. The
                // classification is a class-identity property of the
                // record, not an arity match — see
                // recordConstructionIsPure above.
                if (!recordConstructionIsPure(*ne)) {
                    reportPureViolation(
                        ne->span.begin, "record construction",
                        "the invoked constructor is not pure-declared and "
                        "the record is not of the synthesized-pure "
                        "positional form (§22.15 \"Members classified by "
                        "rule\" — declare the ctor `pure`, or drop the "
                        "disqualifying class-typed field so the record is "
                        "auto-hashable)");
                }
            } else if (rt && rt->kind == TypeRef::Kind::Named &&
                       !rt->declaration &&
                       !common::builtinClassModulePathOf(rt->headName())
                            .empty()) {
                // fix #7453 (§22.15, the prohibited list): "class
                // construction — `new C(…)`" is rejected in a pure body,
                // "admitted only at the throw operand". The arm above
                // reaches only a USER class (a built-in class carries no
                // AST declaration, so `rt->declaration` is null), which
                // left every standard-library construction — `new
                // StringBuilder()`, `new Array<int>()` — admitted. §22.15's
                // prohibition is on CLASS construction as such, not on
                // user-written constructors: a standard-library class's
                // constructor is runtime code just as a user class's is,
                // and its instance drags in the same discharge machinery.
                //
                // The test is the compiler's own built-in CLASS table
                // (`common::builtinClassModulePathOf`, the 88-row §8.5
                // table), so a built-in RECORD (`Range<T>`,
                // `ReadOnlyStringSpan`) and the primitive records are NOT
                // caught here — rule 3 admits record construction, and the
                // record's own classification decides it.
                reportPureViolation(
                    ne->span.begin, "standard-library class construction",
                    std::string("`new ") + std::string(rt->headName()) +
                        "(...)` constructs a standard-library class — "
                        "§22.15's prohibited list rejects class "
                        "construction in a pure body (admitted only at the "
                        "throw operand, whose carve-out requires a pure "
                        "ctor + a trivial-dispose class)");
            }
        }
        for (const auto& a : ne->args)
            if (a.value) checkExpr(a.value.get());
        return;
    }
    if (auto* ce2 = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        checkExpr(ce2->callee.get());
        for (const auto& a : ce2->args)
            if (a.value) checkExpr(a.value.get());
        return;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
        // fix #8134 (§22.15 / §17.8, v0.355): the postfix null-forgiving
        // operator's OPERAND is a sub-expression of the pure body, and
        // §22.15's list is closed — "A pure body may contain exactly the
        // following". The operand was never walked, so `Console.ReadLine()!
        // .Length` compiled in a `pure` body: the `Console.ReadLine()` call
        // inside the operand ran unobserved, and §22.15 rule 6 (a call to a
        // member that is not `pure`-declared) never fired.
        //
        // WHAT IS AND IS NOT CHECKED. The `!` ITSELF is not an effect:
        // §17.8, verbatim — "The operator is purely a type-system assertion
        // (with a runtime null check); it does not move, copy, or borrow
        // beyond what reading `x` already does." The runtime null check
        // raises `NullReferenceException`, a built-in exception that is
        // trivial-dispose (§31.4) — the runtime-raised built-in §22.15's
        // carve-out admits. So the arm walks the OPERAND (where the
        // user-visible call lives) and admits the operator itself.
        checkExpr(pb->operand.get());
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        // §22.15 rule 6 accessor-read arm (fix #3630, v0.286): a property READ
        // spelled as `c.Value` (a MemberAccessExpr, NOT a CallExpr — property
        // getters are NOT spelled as `c.Value()` in Bezel) is a call to the
        // property's getter accessor. The CALL-shape case `c.Foo()` reaches
        // `callTargetIsPure` via the CallExpr arm above; this is the
        // MEMBER-ACCESS shape, which the previous implementation silently
        // admitted by descending only into the target. Resolve the
        // MemberAccessExpr's member via `ctx_.exprSymbol` — ExprTypecheck
        // populates it from `resolveMemberAccess` (ExprTypecheck_Exprs.cpp
        // ~3437, which mirrors the call-resolution path). For an INDEXER read
        // (`obj[i]`, the IndexExpr arm below), the route through here is NOT
        // taken — indexer reads are themselves IndexExpr nodes, and get
        // their OWN rule-6 accessor-read check in the IndexExpr arm below
        // (fix #3721), asking the SAME `propertyGetterIsPure` predicate. A
        // non-property member (field, method-of-target-as-value, etc.) is
        // NOT this rule's concern — field reads are the frame's
        // responsibility, and methods are reached via CallExpr, never
        // MemberAccessExpr.
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*e))) {
            if (!propertyGetterIsPure(ps)) {
                reportPureViolation(
                    e->span.begin, "accessor read",
                    std::string("the resolved target `") +
                        std::string(ps->name) +
                        "` is not pure-declared (§22.15 rule 6 / #3630)");
            }
        } else if (!ctx_.exprSymbol(*e)) {
            // §22.15 rule 6, the STANDARD-LIBRARY arm (fix #7453). A
            // member access on a COMPILER-BUILT-IN receiver resolves to NO
            // symbol — the built-in carries no AST declaration — so the
            // declaration-based predicates above cannot answer rule 6's
            // question, and the member was admitted unconditionally. The
            // standard library's OWN `pure` marker is the authority
            // (§22.15 "Intrinsic purity (the stdlib contract)"), so the
            // member is admitted only where that marker is stated.
            //
            // The `else` keeps the two arms disjoint: a member access that
            // DOES resolve to a symbol is a user-declared or imported
            // member, whose purity the `PropertySymbol` arm above (and the
            // CallExpr arm's `callTargetIsPure`) reads from the declaration
            // or the artifact interface.
            const std::string_view head = stdlibReceiverHead(ma->target.get());
            if (!head.empty() &&
                !stdlibMemberIsPure(head, ma->member)) {
                reportPureViolation(
                    e->span.begin,
                    inCallee_ ? "call to a standard-library member"
                               : "accessor read of a standard-library member",
                    std::string("the standard library does not declare `") +
                        std::string(head) + "." + std::string(ma->member) +
                        "` pure — §22.15 rule 6 admits a call only when the "
                        "resolved member is pure-declared (a standard-library "
                        "member carries the marker the library states for it; "
                        "§22.15 \"Intrinsic purity (the stdlib contract)\")");
                // The access chain is rejected; do not descend into the
                // target (a second report on the same chain would blame
                // the wrong link).
                return;
            }
        }
        checkExpr(ma->target.get());
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
        // fix #8133 (§22.15 rule 6, v0.355): the `?.` SPELLING of a member
        // access. §22.15 rule 6 admits "a call — method, operator, accessor
        // read, or constructor in the admitted positions" iff the member is
        // `pure`-declared or pure-classified, and §22.15 "Intrinsic purity
        // (the stdlib contract)" makes the standard library's own marker the
        // authority for a standard-library member. The rule says nothing
        // about the RECEIVER SPELLING — `sb?.Append("x")` is the same
        // member of the same type as `sb.Append("x")`.
        //
        // WHAT WAS MISSING. A `?.` member access is its own AST node
        // (`ast::NullCondMemberExpr`), and this walker had no arm for it: the
        // `MemberAccessExpr` arm above — which carries the standard-library
        // check fix #7453 landed — never saw it. So the standard-library
        // member reached through `?.` was admitted unconditionally, while
        // the SAME member spelled with a dot was rejected
        // (`sb?.Append("x")` compiled in a `pure` body; `sb.Append("x")`
        // did not). The user-member twin was already rejected, because the
        // CallExpr arm resolves the callee symbol (`ctx_.exprSymbol` on the
        // `NullCondMemberExpr` node) and asks `callTargetIsPure` — but a
        // STANDARD-LIBRARY member resolves to no symbol at all (a built-in
        // carries no AST declaration), so the declaration-based predicates
        // had nothing to ask and the call was admitted.
        //
        // WHAT IS CHECKED. The same two questions the `.` arm asks, of the
        // same node: an accessor read of a USER property (the `PropertySymbol`
        // arm — `propertyGetterIsPure`, the predicate #3630 introduced and
        // #3721 shares with the indexer arm), and a member of a
        // STANDARD-LIBRARY receiver (the built-in table). A `?.` CALL
        // (`x?.M()`) is a `CallExpr` whose callee is this node: the CallExpr
        // arm has already asked `callTargetIsPure` about the resolved
        // symbol, so the `else if` below stands down for it and this arm
        // adds the standard-library question the CallExpr arm cannot ask
        // (a built-in member resolves to no symbol).
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*e))) {
            if (!propertyGetterIsPure(ps)) {
                reportPureViolation(
                    e->span.begin, "accessor read",
                    std::string("the resolved target `") +
                        std::string(ps->name) +
                        "` is not pure-declared (§22.15 rule 6 / #3630)");
            }
        } else if (!ctx_.exprSymbol(*e)) {
            const std::string_view head = stdlibReceiverHead(nm->target.get());
            if (!head.empty() && !stdlibMemberIsPure(head, nm->member)) {
                reportPureViolation(
                    e->span.begin,
                    inCallee_ ? "call to a standard-library member"
                               : "accessor read of a standard-library member",
                    std::string("the standard library does not declare `") +
                        std::string(head) + "." + std::string(nm->member) +
                        "` pure — §22.15 rule 6 admits a call only when the "
                        "resolved member is pure-declared (a standard-library "
                        "member carries the marker the library states for it; "
                        "§22.15 \"Intrinsic purity (the stdlib contract)\")");
                // The access chain is rejected; do not descend into the
                // target (a second report on the same chain would blame
                // the wrong link).
                return;
            }
        }
        checkExpr(nm->target.get());
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        // fix #3721 (§22.15 rule 6, v0.286): the INDEXER twin of the
        // MemberAccessExpr arm above. `obj[i]` on a class receiver with a
        // user-declared indexer is a call to that indexer's `get` accessor
        // — rule 6's "accessor read" form — exactly like `c.Value` is a
        // call to a named property's getter, just spelled with brackets
        // instead of a dot. The previous implementation here only
        // descended into the target and the index expressions, so a
        // non-`pure` indexer getter's side effects ran unobserved inside a
        // `pure` body (issue's measured repro: compile exit 0, run exit 4
        // — the getter's `Calls := Calls + 1` executed inside `pure P`).
        //
        // NO NEW RECORDING SITE, NO SIXTH PREDICATE. #3721's own
        // structural note claimed no writer ever attaches a symbol to an
        // `ast::IndexExpr` — REFUTED: `ExprTypecheck_Exprs.cpp`'s
        // `visitExpr` IndexExpr arm already calls
        // `ctx_.setExprSymbol(&e, ps)` (fix #2530, §22.3/§23) for the
        // user-declared-indexer branch — the grep the issue based that
        // claim on (`setExprSymbol(&ix\|setExprSymbol(ix`) missed it
        // because the call spells the node `&e` (the shared `ast::Expr&`
        // parameter), not `&ix`. That attachment already reaches BOTH a
        // local and an imported indexer identically (the same
        // `tt->declaration->members` walk resolves both — #2414's comment
        // there records the imported-candidate fix), so
        // `ctx_.exprSymbol(*e)` already returns the resolved
        // `PropertySymbol` here, same as the MemberAccessExpr arm. The
        // purity classification itself is also already both-sources:
        // `PropertySymbol::accessorIsPure` (Symbol.hpp) is populated
        // identically for a Property or an Indexer by
        // SymbolCollection.cpp's single `PropertyDecl` arm (`pd->isIndexer`
        // is just a bit on the same node) locally, and by
        // `ModuleLoader.cpp`'s Indexer case (`p->accessorIsPure =
        // mm.accessorIsPure`, mirroring the Property case) across the
        // module boundary — `ModuleMetadataBuilder.cpp`'s exporter walk for
        // `MetaMember::accessorIsPure` (#3618) already says explicitly
        // "Covers both Property AND Indexer". So `propertyGetterIsPure`,
        // the SAME authority the MemberAccessExpr arm asks, already
        // answers the indexer question correctly on both sides of the
        // module boundary — this arm only needed to ASK it. A builtin
        // collection index (Array<T>, Dictionary<K,V>, a span, an inline
        // array, a string) resolves no user `PropertySymbol` at this site
        // (the ExprTypecheck_Exprs.cpp branch that calls `setExprSymbol`
        // is gated on `tt->kind == TypeRef::Kind::Named && tt->declaration`
        // with a matching indexer candidate), so `ctx_.exprSymbol(*e)`
        // stays null there and this arm is a no-op for every builtin/array
        // shape — see the commit body's §6b enumeration.
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(*e))) {
            if (!propertyGetterIsPure(ps)) {
                reportPureViolation(
                    e->span.begin, "indexer read",
                    "the resolved indexer's `get` accessor is not "
                    "pure-declared (§22.15 rule 6 / #3721)");
            }
        }
        // NOTE (fix #7453): a BUILTIN indexer read (`a[0]` over an
        // `Array<T>`, a span, a string) is deliberately NOT checked here.
        // #3721's own structural note established that a builtin index
        // resolves no user `PropertySymbol`, so rule 6's accessor-read arm
        // is a no-op for every builtin shape, and its T3 control row
        // (`builtin_array_index_admitted_in_pure_body_3721_control`) pins
        // that admit. Extending rule 6 to the built-in indexers is a
        // SEPARATE question from #7453's (a standard-library CALL and a
        // standard-library CONSTRUCTION); it is not taken here.
        checkExpr(ix->target.get());
        for (const auto& i : ix->indices)
            if (i.value) checkExpr(i.value.get());
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e)) {
        // fix #3279 (v0.263, stdlib §9.8 + spec §22.15): `x in r` is the
        // §17.5 membership operator, semantically equivalent to the call
        // `r.Contains(x)`. §22.15's calls-to-pure-members rule requires
        // the called member to be pure-classified, so the operator's
        // receiver's Contains member must be pure for the operator to be
        // admitted in a pure body or contract clause. Range<T>.Contains is
        // pure-classified via its `pure` modifier (stdlib §9.8,
        // intrinsic-purity contract); string/span Contains members are
        // pure-declared in stdlib; user types' Contains members are
        // admitted iff `pure`-declared on the receiving type. Any other
        // receiver (Array<T>, OwningArray<T>, user types without a pure
        // Contains, etc.) is rejected — the operator is rejected at this
        // site, named "in" over the receiver, citing §22.15 and the issue.
        //
        // Pre-fix state: the walker only descended into left and right
        // operands, never inspecting the operator itself, so an `in` over
        // Array<T> was admitted silently in pure bodies even though
        // Array<T>.Contains is not pure-declared. The fix closes the gap.
        if (be->op == "in") {
            if (!inOperatorCalleeIsPure(*be)) {
                const TypeRef* recvTy = be->right ? ctx_.exprType(*be->right)
                                                  : nullptr;
                std::string recvName = recvTy ? std::string(recvTy->headName())
                                              : std::string("<expr>");
                if (recvName.empty()) recvName = "<expr>";
                reportPureViolation(
                    be->span.begin,
                    std::string("`in` over `") + recvName + "`",
                    "the `in` operator is semantically equivalent to the "
                    "callee `Contains(value)` (\xC2\xA7""17.5); "
                    "\xC2\xA7""22.15's calls-to-pure-members rule requires "
                    "the receiver's Contains member to be "
                    "pure-classified \xE2\x80\x94 `" + recvName +
                    ".Contains` is not declared `pure` "
                    "(\xC2\xA7""22.15, #3279, stdlib \xC2\xA7""9.8)");
            }
        }
        // narrow #3610 (§22.15 rule 6, v0.286): every OTHER binary operator
        // token may be a §25 user-declared overload — including the §25.2
        // equality forms `=` / `<>` — and an overload is a call. `in` is
        // excluded because it is not an overload (§17.5 `Contains`, handled
        // above); `and` / `or` never resolve one.
        checkOperatorOverloadPurity(ctx_.exprSymbol(*be), be->op,
                                    be->span.begin);
        checkExpr(be->left.get());
        checkExpr(be->right.get());
        return;
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e)) {
        // §22.15 contrapositive: `await` is rejected (it interleaves with
        // the domain's continuation queue). `await` is admitted only in
        // async contexts, and `pure` excludes async — but defend here too.
        if (ue->op == "await") {
            reportPureViolation(
                ue->span.begin, "`await`",
                "interleaves with the domain's continuation queue");
        }
        // narrow #3610 (§22.15 rule 6, v0.286): the unary half. `typeUnary`
        // records a resolved §25 unary overload on the node exactly as the
        // binary path does; `await` is rejected above and resolves none.
        checkOperatorOverloadPurity(ctx_.exprSymbol(*ue), ue->op,
                                    ue->span.begin);
        checkExpr(ue->operand.get());
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(e)) {
        checkExpr(tt->operand.get());
        return;
    }
    if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        checkExpr(cond->thenValue.get());
        checkExpr(cond->condition.get());
        checkExpr(cond->elseValue.get());
        return;
    }
    if (auto* ce3 = dynamic_cast<const ast::CaseExpr*>(e)) {
        // fix #8134 (§22.15 / §A.7, v0.355): the arms of a `case` EXPRESSION
        // are sub-expressions of the pure body and were never walked, so an
        // impure call inside an arm (`(case x of 0 => Console.ReadLine(),
        // _ => "")`) compiled in a `pure` body — §22.15 rule 6 never fired.
        //
        // The statement-position twin (`ast::CaseStmt`) walked its arms'
        // STATEMENT bodies but not their `when` GUARDS; the two forms share
        // the `ast::CaseArm` shape, so both now route through the ONE
        // `checkCaseArms` helper — a guard is an arm's own sub-expression
        // wherever the arm is spelled.
        if (ce3->scrutinee) checkExpr(ce3->scrutinee.get());
        checkCaseArms(ce3->arms);
        return;
    }
    if (auto* te = dynamic_cast<const ast::ThrowExpr*>(e)) {
        // §22.16 (v0.203, #2053): `throw` is REJECTED in a contract clause
        // (clauseMode_), mirroring the throw-statement rule above.
        if (clauseMode_) {
            reportPureViolation(
                te->span.begin, "`throw`",
                "a contract clause states a proposition — the mechanism, "
                "message, and blame of a violation belong to §22.16's "
                "machinery, not to the clause's own throw (§22.16)");
        }
        // §22.15 throw carve-out: a throw expression follows the same rule
        // as the throw statement — "admitted only as the immediate operand
        // of a `throw` statement or `throw` expression". Walking the
        // operand as an ordinary expression (the previous spelling) sent
        // `s ?? throw new ArgumentException(…)` through the `NewExpr`
        // arm's class-construction rejections, which the carve-out exists
        // to admit (fix-forward #7453; §21.8 makes the statement "the
        // expression form followed by `;`").
        checkThrowOperand(te->operand.get(), te->span.begin);
        return;
    }
    if (auto* le = dynamic_cast<const ast::LambdaExpr*>(e)) {
        // §22.15 rule 4: a lambda body defined inside a pure body is NOT
        // checked under these rules — it is a separate function whose
        // later invocation happens at the caller's effect discipline.
        // Intentionally do not recurse into the lambda body.
        return;
    }
    // §17.15 / §22.16 (v0.216, #2098): the binding atom is ADMITTED in
    // contract clauses — it is the form's motivating position (§22.16: "its
    // bindings are clause-internal names, letting a clause name a subresult
    // instead of recomputing it"). Clause initializers and the body are
    // checked under this walker's rules expression-wise. One clause-specific
    // rejection: an atom clause spelled as a MARKED TRANSFER (`var y :=
    // owned x;` — a K3.4 consuming position elsewhere) would break §22.16's
    // borrow-only guarantee and is rejected in a contract clause (§17.15;
    // owned productions cannot arise in the pure fragment, so clause atoms
    // never carry disposals).
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(e)) {
        for (const auto& cl : bx->clauses) {
            if (!cl) continue;
            if (clauseMode_ && cl->initializerHasOwnedMarker) {
                reportPureViolation(
                    cl->initializer ? cl->initializer->span.begin
                                    : bx->span.begin,
                    "marked binding transfer",
                    "a binding-atom clause spelled `var y := owned x;` is a "
                    "K3.4 consuming position and would break the contract "
                    "clause's borrow-only guarantee (§22.16 / §17.15, "
                    "v0.216)");
            }
            if (cl->initializer) checkExpr(cl->initializer.get());
        }
        if (bx->body) checkExpr(bx->body.get());
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        checkExpr(we->source.get());
        for (const auto& a : we->assigns)
            checkExpr(a.value.get());
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& el : cl->elements) checkExpr(el.get());
        for (const auto& k : cl->keys) checkExpr(k.get());
        return;
    }
    if (auto* ie = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& part : ie->parts)
            if (part.expr) checkExpr(part.expr.get());
        return;
    }
    // Literals, IdentifierExpr, etc. — admitted as reads (§22.15 rule 1).
}

void PureBodyWalker::checkContractClauses(
    const std::vector<ast::ContractClause>& clauses) {
    // §22.16 (v0.203, #2053) — the clause-region equivalent of checkStmts.
    // Each clause expression is walked under §22.15's pure-body rules with
    // the clause-specific throw rejection (clauseMode_); the bool typing and
    // the per-amendment `result`/`old` placement checks happen elsewhere
    // (the bool typing lands in ExprTypecheck; result/old are addressed in
    // a later contract slice alongside lowering).
    if (clauses.empty()) return;
    const bool saveClauseMode = clauseMode_;
    clauseMode_ = true;
    for (const auto& c : clauses) {
        if (c.expr) checkExpr(c.expr.get());
    }
    clauseMode_ = saveClauseMode;
}

void PureBodyWalker::checkInvariantExpr(const ast::Expr& e) {
    // §22.16 (v0.203, #2053): an `invariant E;` member declares a predicate
    // checked under this section's clause rules in the context of the
    // declaring class. Throw/await are rejected (clauseMode_); the pure-body
    // rules apply expression-wise.
    const bool saveClauseMode = clauseMode_;
    clauseMode_ = true;
    checkExpr(&e);
    clauseMode_ = saveClauseMode;
}

// Walk the SourceFile: for every pure member, walk its body.
void checkOneMember(SemaContext& ctx, MemberSymbol& member) {
    // Determine purity of this member.
    bool isPure = false;
    bool isCtor = false;
    std::string label;
    if (auto* ms = dynamic_cast<MethodSymbol*>(&member)) {
        // fix #3609 (§22.15, v0.286): through the one predicate. This arm read
        // the `ast::OperatorDecl` bool and the modifiers but NOT the symbol
        // field; `checkPureBodies` walks only this module's own types and
        // extension blocks, so the field was structurally always false here and
        // adding it changes nothing.
        isPure = memberIsPure(ms);
        if (ms->isOperator)
            label = std::string("operator `") + std::string(ms->operatorToken) +
                    "`";
        else
            label = std::string("method `") + std::string(ms->name) + "`";
        // §22.16 (v0.203, #2053): contract clauses are type/purity-checked
        // on EVERY method regardless of `pure` — a non-pure method may
        // carry clauses, and each clause expression must satisfy the pure-
        // body rules expression-wise (clauses never have effects). Run the
        // clause walker before the body walker so a clause diagnostic is
        // reported before any body diagnostic.
        const auto* md = dynamic_cast<const ast::MethodDecl*>(member.decl);
        const auto* od = dynamic_cast<const ast::OperatorDecl*>(member.decl);
        PureBodyWalker w(ctx, member, label, /*isConstructor=*/false);
        if (md) w.checkContractClauses(md->contractClauses);
        // §22.15 body walk runs only for pure methods/operators.
        if (!isPure) return;
        if (md) {
            if (md->bodyKind == ast::MethodBodyKind::Expression &&
                md->exprBody) {
                w.checkExpr(md->exprBody.get());
            } else if (md->bodyKind == ast::MethodBodyKind::Block) {
                w.checkStmts(md->stmts);
            }
        } else if (od) {
            if (od->bodyKind == ast::MethodBodyKind::Expression &&
                od->exprBody) {
                w.checkExpr(od->exprBody.get());
            } else if (od->bodyKind == ast::MethodBodyKind::Block) {
                w.checkStmts(od->stmts);
            }
        }
        return;
    }
    if (auto* cs = dynamic_cast<ConstructorSymbol*>(&member)) {
        const auto* cd = dynamic_cast<const ast::ConstructorDecl*>(member.decl);
        // §22.16 (v0.203, #2053): clauses are checked on every constructor;
        // the §22.15 body walk runs only for pure ctors.
        if (cd) {
            label = std::string("constructor of `") +
                    std::string(cs->parent ? cs->parent->name
                                           : std::string_view()) +
                    "`";
            PureBodyWalker w(ctx, member, label, /*isConstructor=*/true);
            w.checkContractClauses(cd->contractClauses);
            if (!cd->isPure) return;
            isCtor = true;
            isPure = true;
            // §22.15 "Pure constructors" (#2061, item 1): a pure
            // constructor's explicit `base(...)`/`this(...)` chain target
            // must itself be pure. Run before the body walk so the chain
            // diagnostic is reported first (the chain's purity shapes the
            // whole ctors contract).
            w.checkPureCtorChainPurity(*cd, *cs);
            w.checkStmts(cd->body);
        }
        return;
    }
    if (auto* ps = dynamic_cast<PropertySymbol*>(&member)) {
        // Property-level pure applies to the getter. For Slice 2, walk
        // the getter body (expression-bodied / block-getter) — auto-
        // properties have no authored body.
        // fix #3609 (§22.15, v0.286): through the one predicate — the
        // GETTER-level one, which keeps this arm's accessor-level `pure get`
        // source. Routing a property through the member-level `memberIsPure`
        // alone would stop walking a `pure get` body, turning a §22.15
        // diagnostic into silence.
        const auto* pd = dynamic_cast<const ast::PropertyDecl*>(member.decl);
        if (!propertyGetterIsPure(ps)) return;
        label = std::string("property `") + std::string(ps->name) + "`";
        PureBodyWalker w(ctx, member, label, /*isConstructor=*/false);
        if (pd) {
            if (pd->bodyKind == ast::PropertyBodyKind::Expression &&
                pd->exprBody) {
                w.checkExpr(pd->exprBody.get());
            } else if (pd->bodyKind == ast::PropertyBodyKind::Block) {
                for (const auto& a : pd->accessors) {
                    if (a.isSet) continue;  // getter only
                    if (a.bodyKind == ast::PropertyAccessorBodyKind::Expression
                        && a.exprBody) {
                        w.checkExpr(a.exprBody.get());
                    } else if (a.bodyKind ==
                               ast::PropertyAccessorBodyKind::Block) {
                        w.checkStmts(a.stmts);
                    }
                }
            }
        }
        return;
    }
}

// §22.16 (v0.240, #2941): walk every source-defined invariant member's
// predicate under the same clause rules as `requires`/`ensures` (clauseMode:
// throw/await rejected, pure-body rules expression-wise, `old`/`result`
// rejected). Invariants are emitted at codegen time and evaluated at the
// construction-exit / public-entry / public-exit boundaries §22.16 fixes
// (see LlvmBackend_Setup.cpp `emitInvariantCheck`); the sema walk here
// validates the predicate's purity, name-resolves it, and type-checks it.
// The walker takes a `MemberSymbol&` only for diagnostic context; it does
// not dereference it. We pass the FIRST member symbol of the class (any
// real member works; ctor would also work) so the reference is valid.
// Imported classes are skipped (td == nullptr): their invariant records
// are declaration facts in the artifact interface, not something this
// compilation re-evaluates, and re-purity-checking them would break every
// importer of a pre-built library.
static void checkClassInvariants(SemaContext& ctx, const TypeSymbol& t) {
    const auto* td = dynamic_cast<const ast::TypeDecl*>(t.decl);
    if (!td) return;  // imported type: no AST in this compilation
    // Find any real member symbol to satisfy the walker's constructor; the
    // walker doesn't dereference the reference, only stores it.
    MemberSymbol* anchor = nullptr;
    for (const auto& m : t.members) {
        if (auto* mem = dynamic_cast<MemberSymbol*>(m)) {
            anchor = mem;
            break;
        }
    }
    if (!anchor) {
        // Empty class with only invariants and no other members — skip
        // (the class couldn't be instantiated anyway).
        return;
    }
    for (const auto& member : td->members) {
        const auto* inv =
            dynamic_cast<const ast::InvariantDecl*>(member.get());
        if (!inv || !inv->expr) continue;
        PureBodyWalker w(ctx, *anchor,
                         std::string("invariant of `") +
                             std::string(t.name) + "`",
                         /*isConstructor=*/false);
        w.checkInvariantExpr(*inv->expr);
    }
}

// fix #3597 (§22.15 "Pure-body rules" / §22.16 "Contract clauses" / §11.6
// "Nested types", v0.286): ALL of the per-type work `checkPureBodies` used
// to run inline for a module-level type, now ONE function so the shared
// recursive nested-type walker (fix #3574's `forEachTypeAndNested`,
// promoted to sema/NestedTypeWalk.hpp for exactly this caller) can drive it
// at every nesting depth instead of the one-level hand-copy this replaces.
// That one-level copy escaped along TWO independent axes measured on
// d8e07d7: §22.15's pure-body rule (the member loop below) never reached a
// type nested two levels deep (1/1/0 over a depth-0/1/2 ladder), and the
// pure-constructor field-initializer rule (the walk at the end of this
// function) was never applied to a nested type's OWN field initializers AT
// ALL -- it ran only for the outer `t` the old loop iterated, so a nested
// class with a `pure constructor` escaped it already at depth 1 (1/0 over
// depth 0/1).
//
// Diagnostic-order note: the old code interleaved a nested type's
// diagnostics with its outer type's remaining sibling members (nested type N
// declared between members A and B emitted A's diagnostic, then N's, then
// B's). This walker is PRE-ORDER over TYPES, not members: it runs this
// entire function for `t` (invariants, every member, then the field-init
// walk) before descending into any of `t`'s nested types, so a nested type's
// diagnostics now follow ALL of its outer type's own-member diagnostics
// rather than appearing at the nested type's declaration position among
// them. Measured over the in-tree lit corpus: no existing CHECK/CHECK-NEXT
// sequence assumes the old interleaving (the one lit pin that exercises
// invariant-plus-member-body diagnostics together,
// invariant_member_not_evaluated_2941.bzl, already uses CHECK-DAG "so the
// order of diagnostic emission doesn't matter"), and no gtest indexes
// `diagnostics()` by position.
static void checkOneType(SemaContext& ctx, TypeSymbol& t) {
    // §22.16 (v0.240, #2941): purity-check every source-defined invariant
    // member BEFORE the member-purity walk, so an invariant diagnostic is
    // reported ahead of any body diagnostic (matching the clause-before-body
    // ordering at the checkOneMember call site).
    checkClassInvariants(ctx, t);
    for (Symbol* m : t.members) {
        auto* mem = dynamic_cast<MemberSymbol*>(m);
        if (!mem) continue;
        checkOneMember(ctx, *mem);
    }
    // §22.15 "Pure constructors" (v0.202, #2052): the birth-store rule
    // extends to the class's field initializers — they execute as part
    // of construction, so a class with a pure constructor must have
    // field initializers satisfying the pure-body rules. Walk every
    // field initializer under a synthesized pure-ctor context.
    bool hasPureCtor = false;
    MemberSymbol* pureCtorMember = nullptr;
    for (Symbol* m : t.members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs) continue;
        // fix #3609 (§22.15, v0.286): through the one predicate. This gate
        // read `ast::ConstructorDecl::isPure` alone — the NARROWEST of the
        // five spellings — so a ctor whose purity arrived on any other
        // channel left the class's field initializers unwalked. `t` is a
        // local type here, so the widening is inert today; it is the
        // divergence that mattered.
        if (constructorIsPure(cs)) {
            hasPureCtor = true;
            pureCtorMember = cs;
            break;
        }
    }
    if (hasPureCtor && pureCtorMember) {
        const std::string label =
            std::string("field initializer of class `") +
            std::string(t.name) +
            "` (pure constructor birth-store rule)";
        for (Symbol* m : t.members) {
            auto* fs = dynamic_cast<FieldSymbol*>(m);
            if (!fs) continue;
            const auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->initializer) continue;
            PureBodyWalker w(ctx, *pureCtorMember, label,
                             /*isConstructor=*/true);
            w.checkExpr(fd->initializer.get());
        }
    }
}

}  // namespace

bool checkPureBodies(const ast::SourceFile& src, SemaContext& ctx) {
    (void)src;
    if (!ctx.module()) return !ctx.diagnostics().hasErrors();
    // fix #3597 (§11.6, v0.286): every type this module declares, at every
    // nesting depth -- the shared recursive walker (fix #3574,
    // sema/NestedTypeWalk.hpp) replaces the old one-level hand-copy.
    forEachTypeAndNested(ctx.module(),
                         [&](TypeSymbol* t) { checkOneType(ctx, *t); });
    // §22.15 (fix #3175, v0.254): extension blocks (`extension T`) live at
    // module scope as `ExtensionSymbol`s, NOT inside a `TypeSymbol`'s members
    // (§A.3, SymbolCollection.cpp:439; Symbol.hpp:793). A `pure` extension
    // method or expression-bodied extension property carries the same
    // purity obligation as a class member — the body MUST satisfy the §22.15
    // closed-list rules, and the import-side artifact carries the `isPure`
    // bit (`ModuleMetadataBuilder.cpp:870` writes it from the member's
    // modifiers with no body check behind it). The earlier
    // `checkPureBodies` did not iterate extension blocks; the comment that
    // asserted it did (former line 1224) was the cause. Run the same
    // `checkOneMember` path used for class members. `ExtensionSymbol` is
    // only ever created by `collectMember` from a local `ast::ExtensionDecl`
    // (SymbolCollection.cpp:440) — there is no import path that synthesizes
    // one, so imported-extension handling is a non-issue. Per the issue's
    // "fix shape" note: the receiver-form distinction (by-value for value/
    // primitive extended types, by-pointer for class extended types) is
    // `MethodSymbol::extensionDecl`'s concern at codegen — `checkOneMember`
    // works on the body's AST and does not depend on `parent`.
    std::vector<ExtensionSymbol*> extensions;
    for (Symbol* child : ctx.module()->children) {
        if (auto* e = dynamic_cast<ExtensionSymbol*>(child))
            extensions.push_back(e);
    }
    for (ExtensionSymbol* ext : extensions) {
        for (Symbol* m : ext->members) {
            auto* mem = dynamic_cast<MemberSymbol*>(m);
            if (!mem) continue;
            checkOneMember(ctx, *mem);
        }
    }
    return !ctx.diagnostics().hasErrors();
}

}  // namespace bezel::sema
