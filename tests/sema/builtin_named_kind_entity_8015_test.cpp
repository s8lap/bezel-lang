// fix #8015 (spec §5.1, stdlib §2.15, §2.5, §3.2, §3.3, §4.4, §9.1, §9.6,
// v0.350) — a NAMED §5.1 kind bit matched the DECLARED TYPE by BARE NAME, so
// a USER type that merely SHARES the name was admitted where the standard
// library's type is declared.
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// spec §5.1 "Implicit conversions": the implicit conversions are the identity,
// the widening table, and the CONSTANT narrowing.  A type reaches a parameter
// of a RECORD type only when it IS that type — §5.1's reference conversions
// are the subtyping ones, and a record has no subtype relation to another
// record.
//
// stdlib §2.15 declares, verbatim:
//
//   public class JsonSerializer: ISerializer
//     public constructor();
//     public constructor(options: JsonOptions);
//   public record JsonOptions(
//     Indented: bool, IndentWidth: int, AllowTrailingCommas: bool,
//     IgnoreUnknownFields: bool, MaxDepth: int
//   );
//
// stdlib §3.3 declares `public record ReadOnlyStringSpan(Source: string,
// Start: int, Length: int)`; stdlib §3.2 declares
// `public abstract method GetBytes(s: string, destination: Span<byte>): int;`;
// stdlib §9.1 declares `public static pure method Parse(s: ReadOnlyStringSpan):
// int;`; stdlib §9.6 declares `AsSpan(r: Range<int>): ReadOnlyStringSpan` and
// `Split(separators: Array<char>): owned Array<string>`; stdlib §2.5
// declares `public method Add(span: TimeSpan): DateTime;`.
//
// --- THE DEFECT (MEASURED on this lane's base) ------------------------------
//
// The ONE built-in parameter table (`src/common/BuiltinRegistry.cpp`'s
// `kBuiltinParams`) names the DECLARED TYPE at a position with a vocabulary
// bit — and every NAMED bit was matched by the type's NAME, not by its
// IDENTITY.  MEASURED on this lane's base, each row = a program that declares
// the user's own type of that name and passes it:
//
//   record JsonOptions(X: int)          -> new JsonSerializer(o)   COMPILED
//   record ReadOnlyStringSpan(V: string)-> int.Parse(s)            COMPILED
//   record Span(V: int)                 -> GetBytes("ab", s)       ADMITTED
//                                          at the §5.1 gate (the codegen
//                                          arm, #7603, refused it later)
//   record Range(V: int)                -> "abc".AsSpan(r)         COMPILED
//                                          (and reached CODEGEN, which
//                                          emitted "Both operands to ICmp
//                                          instruction are not of the same
//                                          type!")
//   record Array<T>(Item: T)            -> "abc".Split(a)          COMPILED
//
// A NAME is not an identity: the standard library's `JsonOptions` is a
// five-field options record and the user's is whatever the user declared, so
// admitting the user's where the library's is declared hands the callee an
// object whose layout the callee does not expect.
//
// --- THE FIX ----------------------------------------------------------------
//
// The gate compares the DECLARED ENTITY, not the head name: the standard
// library's built-in types are the compiler's OWN intrinsic types, and a
// USER's type always carries its own declaration — so "the standard
// library's `TimeSpan`" is exactly "a Named type that is not a user type's".
// The rule is ONE predicate (`builtinArgIsDeclaredEntity`), used on BOTH
// sides of the call: the member side and the constructor side.
//
// The diagnostic says what was ACTUALLY VERIFIED — that the argument's type
// is not the declared parameter type — and it names the parameter.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A program that declares the USER's own type of the given name and then
// uses it.
std::string progWithUserType(const std::string& decl,
                             const std::string& body) {
    return "module M; import System; import System.Text; "
           "import System.Collections.Generic; import System.IO; " +
           decl +
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const char* needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    return diagnosticMessageContains(r, needle);
}

}  // namespace

// --- THE CONSTRUCTOR SIDE: the JSON agents' `options` parameter -------------

// stdlib §2.15 declares `JsonSerializer(options: JsonOptions)` — the
// STANDARD LIBRARY's `JsonOptions`, and the user's own `record JsonOptions`
// is a different type that merely shares the name.  The §5.1 gate refuses it,
// and it says what was verified.
BEZEL_TEST(builtin_named_kind_entity_8015, user_jsonoptions_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        progWithUserType(
            "public record JsonOptions(Indented: bool); ",
            "    using var s := new JsonSerializer(new JsonOptions(true)); "),
        "the `options` argument of the `JsonSerializer` constructor has a type "
        "that is not implicitly convertible to the target type (\xC2\xA7""5.1): "
        "the declared type is the standard library's `JsonOptions`, and the "
        "argument's type is not that type"));
}

// The STANDARD LIBRARY's own `JsonOptions` is still accepted, and the
// serializer still works — a fix that refused the library's own type along
// with the user's is a fix that broke the feature.
BEZEL_TEST(builtin_named_kind_entity_8015, stdlib_jsonoptions_is_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; import System.Text; "
            "import System.Serialization; "
            "class C "
            "  public static method Main(): int "
            "    using var s := new JsonSerializer("
            "      new JsonOptions(true, 2, false, false, 3)); "
            "    Console.WriteLine(1); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        "1");
}

// --- THE MEMBER SIDE: the `ReadOnlyStringSpan` arm --------------------------

// stdlib §9.1 declares `int.Parse(s: ReadOnlyStringSpan): int` — the user's
// own `record ReadOnlyStringSpan` is a different record, and §5.1's identity
// is what admits it (a record has no subtype relation to another record).
BEZEL_TEST(builtin_named_kind_entity_8015,
           user_readonlystringspan_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        progWithUserType(
            "public record ReadOnlyStringSpan(Value: string); ",
            "    var v := int.Parse(new ReadOnlyStringSpan(\"42\")); "),
        "no matching overload of `int.Parse` — stdlib \xC2\xA7""9.1 declares "
        "`Parse(s: string): int` / `Parse(s: ReadOnlyStringSpan): int`, so the "
        "`s` argument must be a `ReadOnlyStringSpan` — the argument's type is a "
        "`ReadOnlyStringSpan` that is NOT the standard library's — a different "
        "type (\xC2\xA7""5.1)"));
}

// The STANDARD LIBRARY's own `ReadOnlyStringSpan` — the one §9.6's `AsSpan()`
// hands out — is still admitted, and `int.Parse` still parses it.
BEZEL_TEST(builtin_named_kind_entity_8015,
           stdlib_readonlystringspan_is_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "class C "
            "  public static method Main(): int "
            "    Console.WriteLine(int.Parse(\"42\".AsSpan())); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        "42");
}

// --- THE MEMBER SIDE: the byte-span arm ------------------------------------

// stdlib §3.2 declares the `destination` as a `Span<byte>` — §4.14's /
// §4.15's span views over `byte`.  A user's `record Span` merely shares the
// name, and the §5.1 gate refuses it (the codegen arm, #7603, refused it
// later on the base).
BEZEL_TEST(builtin_named_kind_entity_8015, user_span_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        progWithUserType(
            "public record Span(Value: int); ",
            "    var s := new Span(3); "
            "    var n := Encoding.UTF8.GetBytes(\"ab\", s); "),
        "`Encoding.GetBytes` expects a `Span<byte>` destination (stdlib "
        "\xC2\xA7""3.2); got `Span` — `Span` is never implicitly convertible "
        "to Span<byte> (\xC2\xA7""5.1) (#7999)"));
}

// --- THE MEMBER SIDE: the `Range<int>` arm (stdlib §9.6) --------------------

// stdlib §9.6 declares `AsSpan(r: Range<int>)` — the compiler's own `Range`
// (§16.3 / stdlib §9.8).  A user's `record Range` is a different type, and on
// the base it reached CODEGEN, which emitted "Both operands to ICmp
// instruction are not of the same type!".
BEZEL_TEST(builtin_named_kind_entity_8015, user_range_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        progWithUserType(
            "public record Range(Value: int); ",
            "    var r := new Range(3); "
            "    var s := \"abc\".AsSpan(r); "),
        "no matching overload of `string.AsSpan` — stdlib \xC2\xA7""9.6 "
        "declares `AsSpan(start: int): ReadOnlyStringSpan` / "
        "`AsSpan(r: Range<int>): ReadOnlyStringSpan`, so the `start` argument "
        "must be an `int` or a `Range<int>` — the argument has type `Range`, "
        "which is never implicitly convertible to it (\xC2\xA7""5.1)"));
}

// --- THE MEMBER SIDE: the `Array<char>` arm (stdlib §9.6) -------------------

// stdlib §9.6 declares `Split(separators: Array<char>)` — the compiler's own
// `Array<T>` (stdlib §4.4).  A user's `record Array<T>` is a different type.
BEZEL_TEST(builtin_named_kind_entity_8015, user_array_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        progWithUserType(
            "public record Array<T>(Item: T); ",
            "    var a := new Array<char>('x'); "
            "    var parts := \"abc\".Split(a); "),
        "no matching overload of `string.Split` — stdlib \xC2\xA7""9.6 "
        "declares `Split(separator: char): owned Array<string>` / "
        "`Split(separator: string): owned Array<string>` / "
        "`Split(separators: Array<char>): owned Array<string>`, so the "
        "`separator` argument must be a `string` or a `char` or an "
        "`Array<char>` — the argument has type `Array`, which is never "
        "implicitly convertible to it (\xC2\xA7""5.1)"));
}

// --- THE MEMBER SIDE: a §2.5 declared type ---------------------------------

// stdlib §2.5 declares `DateTime.Add(span: TimeSpan): DateTime` — the
// standard library's `TimeSpan`.  The user's own `record TimeSpan` is a
// different record, and §2.5's position admits only the declared entity.
BEZEL_TEST(builtin_named_kind_entity_8015, user_timespan_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        progWithUserType(
            "public record TimeSpan(Value: int); ",
            "    var dt := new DateTime(2024, 1, 2); "
            "    var ts := new TimeSpan(5); "
            "    const x := dt.Add(ts); "),
        "no matching overload of `DateTime.Add` — stdlib \xC2\xA7""2.5 "
        "declares `Add(span: TimeSpan): DateTime`, so the argument at position "
        "0 must be a `TimeSpan` — the argument's type is a `TimeSpan` that is "
        "NOT the standard library's — a different type (\xC2\xA7""5.1)"));
}

// --- THE CONTROLS: every legal named-kind spelling still works -------------

// Every named kind the table declares — `Array<char>`, `Range<int>`,
// `ReadOnlyStringSpan`, `Span<byte>`, `IEnumerable<string>` — is still
// admitted, and the programs still run.  These rows are GREEN before the fix
// and GREEN after it: they are the admitting twins of the refusals above, so
// they show the refusals fire on the IDENTITY and not on the shape of the
// call.
BEZEL_TEST(builtin_named_kind_entity_8015, named_kinds_are_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            bezel::test::spliceDataPaths(
                "module M; import System; import System.Text; import System.IO; "
                "import System.Collections.Generic; "
                "class C "
                "  public static method Main(): int "
                "    var s := \"abcdef\"; "
                "    var a := s.AsSpan(1..3); "
                "    var r: Range<int> := 1..3; "
                "    var b := s.AsSpan(r); "
                "    var chars: Array<char> := ['x', 'y']; "
                "    var parts := s.Split(chars); "
                "    var trimmed := s.Trim(chars); "
                "    var lines: Array<string> := [\"a\", \"b\"]; "
                "    File.WriteAllLines(\"@DATA@/bezel_8015_lines.txt\", lines); "
                "    File.Delete(\"@DATA@/bezel_8015_lines.txt\"); "
                "    using var buf := new OwningBuffer<byte>(8); "
                "    var n := Encoding.UTF8.GetBytes(\"ab\", buf.AsSpan()); "
                "    Console.WriteLine(a.Length); "
                "    Console.WriteLine(b.Length); "
                "    Console.WriteLine(parts.Count); "
                "    Console.WriteLine(trimmed.Length); "
                "    Console.WriteLine(n); "
                "    Console.WriteLine(int.Parse(\"42\".AsSpan())); "
                "    return 0; "
                "  end Main; "
                "end C;", "bzt8015")),
        "3\n3\n1\n6\n2\n42");
}
