// Cross-module rows for #6905: a `Range<T>` value returned by an IMPORTED
// method must be the same `Range<T>` the importer would build itself (spec
// §2.5, v0.340: an imported declaration has the same meaning in the importer;
// §16.3 / §16.5 define the Range members and `for` over a Range value).
//
// ── THE BUG ────────────────────────────────────────────────────────────────
//
// A LOCAL `Range<T>`, annotated or inferred from a range expression, carries
// `declaration = builtinClassSym("Range")` (TypeResolution_Names.cpp,
// ExprTypecheck_Exprs.cpp), and the Range checks in sema and codegen key on
// that pointer (`isRangeType`, the `in` operand typing, Contains / ToString /
// `.Start`, `for` over a Range value). ModuleLoader rebuilt an imported
// `Range<uint>` as a declaration-less Named TypeRef with the head only in
// `importedName`, so it matched none of them: `for var x in q` was "not yet
// implemented", `q.Contains(x)` and `q.ToString()` produced no value, `x in q`
// was "not yet implemented", and `q.Start` read 0. The loader now attaches
// the builtin Range symbol, as #2776 does for the synthesized interfaces.
//
// ── ORACLE ─────────────────────────────────────────────────────────────────
//
// `runAotModules`: the two-module AOT path, the only one that produces the
// imported TypeRef. Every row asserts the exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib6905 =
    "module Lib6905; "
    "public class U6905 "
    "  public static method MakeR(lo: uint, hi: uint): Range<uint> "
    "    return lo..hi; "
    "  end MakeR; "
    "  public static method Stepped(): Range<int> return 1..5 step 2; end Stepped; "
    "end U6905;";

std::string app6905(const std::string& body) {
    return "module App6905; "
           "import Lib6905; "
           "class C6905 public static method Main(): int "
           "  " + body + " "
           "  return 0; "
           "end Main; end C6905;";
}

test::JitResult run6905(const std::string& body) {
    return test::runAotModules(
        {{"Lib6905", kLib6905}, {"App6905", app6905(body)}}, "App6905");
}

}  // namespace

// §16.5: `for` over a Range value visits each element. The issue's repro: an
// imported `Range<uint>` at the top of the uint domain. At c0a1928b this is
// "not yet implemented: codegen for this for statement".
BEZEL_TEST(imported_range_identity_6905,
           imported_range_value_in_for_statement_spec_16_5) {
    auto r = run6905(
        "var q := U6905.MakeR(4294967290U, 4294967295U); "
        "for var x in q do Console.WriteLine(x); end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("4294967290\n4294967291\n4294967292\n"
                                "4294967293\n4294967294\n4294967295\n"));
}

// §16.3: `Contains` and the `in` operator on an imported Range. The normative
// formula is extent-only (`Step` matters only through emptiness), so 3 is in
// `1..5 step 2` and 6 is not. At c0a1928b `Contains` "produced no value" and
// `in` was "not yet implemented".
BEZEL_TEST(imported_range_identity_6905,
           imported_range_contains_and_in_spec_16_3) {
    auto r = run6905(
        "var q := U6905.Stepped(); "
        "Console.WriteLine($\"{q.Contains(3)} {q.Contains(6)}\"); "
        "Console.WriteLine(5 in q);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True False\nTrue\n"));
}

// §16.3: the `Start` / `End` / `Step` / `IsEndInclusive` properties. At
// c0a1928b each read 0.
BEZEL_TEST(imported_range_identity_6905,
           imported_range_properties_spec_16_3) {
    auto r = run6905(
        "var q := U6905.Stepped(); "
        "Console.WriteLine($\"{q.Start} {q.End} {q.Step} {q.IsEndInclusive}\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 5 2 True\n"));
}

// §16.3 / §8.5: `ToString()` and an interpolation hole render the record
// form. At c0a1928b `ToString` was rejected as not implemented (#2457) and the
// hole printed nothing.
BEZEL_TEST(imported_range_identity_6905,
           imported_range_to_string_and_interpolation_spec_16_3) {
    auto r = run6905(
        "var q := U6905.Stepped(); var s := q.ToString(); "
        "Console.WriteLine(s); Console.WriteLine($\"r={q}\");");
    const std::string form =
        "Range(Start: 1, End: 5, Step: 2, IsEndInclusive: True, "
        "IsOpenEnded: False, IsEmpty: False)";
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, form + "\nr=" + form + "\n");
}

}  // namespace bezel::module
