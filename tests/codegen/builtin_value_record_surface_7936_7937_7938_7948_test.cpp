// Regression tests for the four built-in VALUE-TYPE surface defects of the
// R41 lane `r41-fix-timevalues` (#7936, #7937, #7938, #7948). Every row below
// is POSITIVE: it asserts that the program COMPILES, RUNS, and PRINTS the
// expected value. A program the compiler refuses (or crashes) yields the
// harness's "<did-not-run>" sentinel, which no expected string matches — so a
// refusal or a crash is observed as a FAILURE of these rows, not asserted as
// an outcome.
//
// THE FIVE DEFECTS THESE ROWS PIN.
//
// 1. #7936 — an `extension Object` member on a CONSTRUCTED built-in value
//    record. §2 ("every type derives from Object") and §12 ("if no base class
//    is specified, the implicit base is Object") make `Object` a base type of
//    every type, so §26.5 rule 2's search of "the extension blocks on `x`'s
//    type, base types, or implemented interfaces" must reach it.
//
//    The built-in value records reach sema in TWO shapes, and only ONE of
//    them reached the `Object` base: a value obtained from a static factory
//    (`DateTime.FromTicks(…)`) carries the compiler's `builtinClassSym`, and
//    `extensionFindMatchingAncestor`'s first arm admits it; a value obtained
//    from a CONSTRUCTION (`new DateTime(…)`) or from a written annotation
//    (`var d: DateTime := …`) keeps `declaration == nullptr` (§2.5 / #1463:
//    the built-in value types keep a null declaration so the compiler's `new`
//    path can recognise them), so NO arm matched and the member stayed
//    unresolved — "«Describe» on built-in type «DateTime» produced no value,
//    and no dispatch arm emitted any code for this call" (#2457).
//
// 2. #7948(a) — an IMPORTED user type that shares a built-in's spelling.
//    §11.3 reserves no names, so a dependency module is free to export
//    `public record TimeSpan: IComparable<TimeSpan>`. The importer lowered
//    that record as the BUILT-IN `TimeSpan` (a scalar i64 tick count) while
//    the exporter laid it out as a record (`%record.TimeSpan = { i32 }`),
//    so the importer declared `bz$Lib.TimeSpan$CompareTo` with an `i64`
//    second parameter while its own call site passed the record — "Call
//    parameter type does not match function signature!", an IR-verification
//    failure with no diagnostic naming the cause.
//
// 3. #7948(b) — a QUALIFIED type argument. §27.6 admits one
//    (`Math.Max<Lib.DateTime>`), and §11.6 makes the qualified spelling the
//    ordinary one for another module's type. The parser's generic-argument
//    scan rejected the `.` of a qualified name, so the generic reading never
//    committed and the `<` fell to the RELATIONAL parser: the program was a
//    parse error ("expected expression") on a program the grammar admits.
//
// 4. #7937 — the §8.3 value-equality pair (`Equals` / `GetHashCode`) of the
//    built-in value records `TimeSpan` / `DateOnly` / `TimeOnly`. They are
//    RECORDS in stdlib §2.5, so the pair is theirs; `builtinClassSym` stamps
//    the symbol `Class`, so the record arm never saw them and both members
//    degraded to a zero (`False` / `0`) behind an `internal:` warning.
//
// 5. #7937 / #7938 — the §1 *OrNull tier's `ParseOrNull` for the built-in
//    value records (`DateTime` / `DateOnly` / `TimeOnly` / `Guid`): the
//    nullable form of `Parse`, null exactly where `Parse` raises
//    FormatException. The member was typed but had no lowering arm, so the
//    call degraded to a zero-initialised value — and a `DateTime?` local
//    built from it threw a NullReferenceException at run time.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out — the value a REFUSED (or crashing)
// program produces here, and the reason every row below fails before the
// fix.
std::string out(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C "
           "public static method Main(): int "
           + body +
           " return 0; end Main; end C;";
}

// The same, with a MODULE-LEVEL prefix — an `extension` block is a
// top-level declaration (§26.2), so it cannot live inside the class body
// `prog` builds.
std::string progWith(const std::string& prefix, const std::string& body) {
    return "module M; " + prefix + " class C "
           "public static method Main(): int "
           + body +
           " return 0; end Main; end C;";
}

// The §2.5 value-record shape the extension block is written against.
const char* kObjectExtension =
    "extension Object "
    "  public method Describe(): string => \"obj\"; "
    "end; ";

}  // namespace

// #7936 / §2 + §12 + §26.5 rule 2: an `extension Object` member is
// REACHED by a CONSTRUCTED `DateTime`.
//
// Before the fix the program was REJECTED: "`Describe` on built-in type
// `DateTime` produced no value, and no dispatch arm emitted any code for
// this call. `DateTime` has no user declaration, so its member surface is
// exactly what the standard library documents for it". The identical
// extension over a `DateTime.FromTicks(…)` value already worked — the two
// receivers differ only in whether the built-in's `builtinClassSym` rides
// along on the type.
BEZEL_TEST(builtin_value_record_surface, extension_object_reaches_constructed_datetime) {
    BEZEL_EXPECT_EQ(
        out(progWith(kObjectExtension,
                 "var d := new DateTime(2024, 3, 15); "
                 "Console.WriteLine(d.Describe()); ")),
        "obj\n");
}

// #7936: the same for the other two CONSTRUCTIBLE built-in value records,
// `DateOnly` and `TimeOnly` (stdlib §2.5) — and for the ANNOTATED spelling
// of `DateTime`, which is the same declaration-less shape.
BEZEL_TEST(builtin_value_record_surface, extension_object_reaches_dateonly_timeonly_and_annotated) {
    BEZEL_EXPECT_EQ(
        out(progWith(kObjectExtension,
                 "var d := new DateOnly(2024, 3, 15); "
                 "Console.WriteLine(d.Describe()); "
                 "var t := new TimeOnly(10, 20, 30); "
                 "Console.WriteLine(t.Describe()); "
                 "var a: DateTime := new DateTime(2024, 3, 15); "
                 "Console.WriteLine(a.Describe()); ")),
        "obj\nobj\nobj\n");
}

// #7936: the built-in CLASS `Random` (stdlib §2.7) is reached the same way
// — it is a by-name built-in whose CONSTRUCTION form (`new Random(seed)`)
// keeps a null declaration, so an `extension Object` member did not reach it
// either.
BEZEL_TEST(builtin_value_record_surface, extension_object_reaches_constructed_random) {
    BEZEL_EXPECT_EQ(
        out(progWith(kObjectExtension,
                 "Console.WriteLine(new Random(1L).Describe()); ")),
        "obj\n");
}

// #7948(a) / §11.3 + §2.8.3 rule 3: a dependency module's OWN
// `public record TimeSpan: IComparable<TimeSpan>` is laid out as a RECORD
// on both sides of the module boundary.
//
// Before the fix the importer declared `bz$Lib.TimeSpan$CompareTo` with an
// `i64` second parameter (the BUILT-IN TimeSpan's tick word) while its own
// call site passed the record — "Call parameter type does not match function
// signature!", and the program was REJECTED.
//
// The QUALIFIED construction `new Lib.TimeSpan(5)` is the spelling that
// discriminates: §11.6 makes the qualified spelling the ordinary one for
// another module's type, and the importer's `lowerTypeRef` lowered THAT
// spelling as the BUILT-IN TimeSpan (a scalar `i64` tick word) while the
// exporter laid the record out as `%record.TimeSpan = { i32 }`. A factory
// call (`Mk.TS(5)`) carries the record correctly, so a program that only
// used the factory would NOT see the defect — this row uses both, and the
// qualified construction is the one that breaks.
//
// The `CompareTo` here is the ordinary (non-reversed) one, so `Math.Max`
// answers the LARGER value and `Math.Min` the smaller one.
BEZEL_TEST(builtin_value_record_surface, imported_record_named_timespan_keeps_its_own_layout) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public record TimeSpan: IComparable<TimeSpan>
  public v: int;
  public method CompareTo(other: TimeSpan): int
    return this.v - other.v;
  end CompareTo;
end TimeSpan;
public class Mk
  public static method TS(v: int): TimeSpan
    return new TimeSpan(v);
  end TS;
end Mk;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import Lib;
class C
  public static method Main(): int
    var x := new Lib.TimeSpan(5);
    var y := new Lib.TimeSpan(9);
    Console.WriteLine(Math.Max(x, y).v);
    Console.WriteLine(Math.Min(x, y).v);
    var a := Mk.TS(1);
    var b := Mk.TS(9);
    Console.WriteLine(Math.Max(a, b).v);
    return 0;
  end Main;
end C;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n5\n9\n");
}

// #7948(b) / §27.6 + §11.6: a QUALIFIED type argument is a GenericArg.
//
// Before the fix the program was a PARSE ERROR ("expected expression" at the
// `<`), because the parser's generic-argument scan rejected the `.` of the
// qualified type name and the `<` fell to the relational-operator parser.
//
// The module's record implements `IComparable<DateTime>`, so the constraint
// `where T: IComparable<T>` is satisfied and the call is admitted.
BEZEL_TEST(builtin_value_record_surface, qualified_type_argument_parses_and_binds) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", R"BZ(
module Lib;
public record DateTime: IComparable<DateTime>
  public v: int;
  public method CompareTo(other: DateTime): int
    return this.v - other.v;
  end CompareTo;
end DateTime;
public class Mk
  public static method DT(v: int): DateTime
    return new DateTime(v);
  end DT;
end Mk;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import Lib as L;
class C
  public static method Main(): int
    var f := Math.Max<L.DateTime>;
    var a := L.Mk.DT(1);
    var b := L.Mk.DT(9);
    Console.WriteLine(f(a, b).v);
    return 0;
  end Main;
end C;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// #7937 / §8.3 + stdlib §2.5: a `TimeSpan`'s `Equals` and `GetHashCode`.
//
// Before the fix both members DEGRADED to a zero behind an `internal:`
// warning, so the two `Console.WriteLine` calls emitted NOTHING at all (not
// even a line terminator). A `TimeSpan` is equal to another when they carry
// the same tick count (§2.5's "CompareTo = tick order" over the same word).
BEZEL_TEST(builtin_value_record_surface, timespan_equals_and_gethashcode_are_value_semantics) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var t := TimeSpan.FromTicks(123L); "
            "var u := TimeSpan.FromTicks(123L); "
            "var v := TimeSpan.FromTicks(456L); "
            "Console.WriteLine(t.Equals(u)); "
            "Console.WriteLine(t.Equals(v)); "
            "Console.WriteLine(t.GetHashCode() = u.GetHashCode()); ")),
        "True\nFalse\nTrue\n");
}

// #7937 / §8.3 + stdlib §2.5: the same pair for `DateOnly` (an i32 day
// number) and `TimeOnly` (an i32 millisecond-of-day count).
//
// The `TimeOnly` hash is the millisecond word itself: 10h20m30s is
// 37,230,000 ms.
BEZEL_TEST(builtin_value_record_surface, dateonly_timeonly_equals_and_gethashcode_are_value_semantics) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var d := new DateOnly(2024, 3, 15); "
            "Console.WriteLine(d.Equals(new DateOnly(2024, 3, 15))); "
            "Console.WriteLine(d.Equals(new DateOnly(2025, 1, 1))); "
            "var t := new TimeOnly(10, 20, 30); "
            "Console.WriteLine(t.Equals(new TimeOnly(10, 20, 30))); "
            "Console.WriteLine(t.GetHashCode()); ")),
        "True\nFalse\nTrue\n37230000\n");
}

// #7937 / #7938 / §1 *OrNull tier + stdlib §2.5: `DateTime.ParseOrNull` is
// the nullable form of `DateTime.Parse` — null exactly where `Parse` raises
// FormatException, and the parsed value otherwise.
//
// Before the fix the member DEGRADED to a zero-initialised value behind an
// `internal:` warning, so `po = null` answered "False" for an unparseable
// string (the degraded value was a PRESENT zero) and a `DateTime?` local
// built from it threw a NullReferenceException.
BEZEL_TEST(builtin_value_record_surface, datetime_parseornull_is_null_exactly_where_parse_raises) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var good := DateTime.ParseOrNull(\"2024-03-15\"); "
            "Console.WriteLine(good = null); "
            "var bad := DateTime.ParseOrNull(\"zz\"); "
            "Console.WriteLine(bad = null); "
            "var v: DateTime? := DateTime.ParseOrNull(\"2024-03-15\"); "
            "if v <> null then Console.WriteLine(v.Year); end if; ")),
        "False\nTrue\n2024\n");
}

// #7937 / #7938 / §1 *OrNull tier + stdlib §2.5-§2.6: the same for
// `DateOnly`, `TimeOnly` and `Guid`.
//
// A `Guid` is the §2.6 128-bit value; the parsed value is read back through
// its own `ToString()`, so the row pins the PAYLOAD of the nullable carrier
// (not merely the tag).
BEZEL_TEST(builtin_value_record_surface, dateonly_timeonly_guid_parseornull_are_null_exactly_where_parse_raises) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "Console.WriteLine(DateOnly.ParseOrNull(\"zz\") = null); "
            "Console.WriteLine(TimeOnly.ParseOrNull(\"zz\") = null); "
            "Console.WriteLine(Guid.ParseOrNull(\"zz\") = null); "
            "var g: Guid? := Guid.ParseOrNull("
            "\"00000000-0000-0000-0000-000000000000\"); "
            "if g <> null then Console.WriteLine(g.ToString()); end if; ")),
        "True\nTrue\nTrue\n00000000-0000-0000-0000-000000000000\n");
}
