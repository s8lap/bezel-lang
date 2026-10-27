// Spec-derived regression test for issue #7289 — stdlib §2.2's
// `Math.Floor` / `Math.Ceiling` / `Math.Truncate` declare a `double` form AND
// a `decimal` form, and the §22.6 ambiguity rule was enforced for
// `Convert.ToString` (#7266) but NOT for them.
//
// stdlib §2.2 `Math` (v0.344); spec §22.6 "Overloading" (v0.344);
// spec §5.1 "Implicit conversions" (v0.344); spec §5.2 (v0.344);
// spec §14.5 (v0.344). The rule's own landing record: docs/Bezel_Changelog.md,
// v0.228's migration note.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// stdlib §2.2 declares, for each of the three members, two declared forms:
//
//   public static pure method Floor(x: double): double;
//   public static pure method Floor(x: decimal): decimal;
//   public static pure method Ceiling(x: double): double;
//   public static pure method Ceiling(x: decimal): decimal;
//   public static pure method Truncate(x: double): double;
//   public static pure method Truncate(x: decimal): decimal;
//
// spec §22.6 (v0.344), "Implicit conversions and tie-breaking":
//
//   "An exact match is the only relation that distinguishes one applicable
//    candidate from another at an argument position; two candidates that both
//    require an implicit conversion at the same position are equally ranked
//    there, regardless of which conversion each uses."
//
//   "If no candidate is unambiguously better — because each of two candidates
//    is exact on a different argument, or because the remaining candidates all
//    rely on implicit conversions that this rule treats as equally ranked —
//    the call is ambiguous and is a compile error."
//
// spec §5.1's table gives EVERY integer type and `char` a conversion to BOTH
// `double` and `decimal`, and to neither exactly. So at an integer or `char`
// argument the two declared candidates are both APPLICABLE and EQUALLY RANKED:
// the call is ambiguous.
//
// THE RULE'S OWN LANDING RECORD (docs/Bezel_Changelog.md, v0.228 — the
// revision that added the `decimal` forms, "Math decimal symmetry"). Its
// migration note states the consequence in terms:
//
//   "Newly rejected: a `Math.Floor`/`Ceiling`/`Truncate` call whose argument
//    is an integer type or `char` — previously legal via the sole `double`
//    overload's implicit conversion, now ambiguous under §22.6's equal-ranking
//    rule (the `double` and `decimal` candidates tie); the remedy is an
//    explicit conversion (`Math.Floor(double(n))`) — or reconsidering the
//    call, since floor/ceiling of an already-integral value is a no-op
//    (`Abs`/`Sign` are unaffected: their `int` overloads give exact
//    matches)."
//
// => THE CLASS IS "AN INTEGER TYPE OR `char`" — `int` AND `long` INCLUDED,
// NOT ONLY THE NARROW TYPES. `Abs` is unaffected because it DECLARES an `int`
// overload (an exact match); `Floor`/`Ceiling`/`Truncate` declare none.
//
// WHAT THE DEFECT WAS.
//
// The probe that type-checks a built-in `Math` / `Convert` argument
// (`ExprTC::checkBuiltinMathConvertCallArgs`, src/sema/ExprTypecheck_Calls.cpp)
// answered §22.6's APPLICABILITY step and not its RANKING step. So
// `Math.Floor(b)` over a `byte` compiled, ran and printed "200" — while
// `Math.Floor("x")` was refused. #7266 added the ranking step for the
// `Convert` class; the `Math` class was left out.
//
// WHAT THE FIX DOES.
//
// The SAME ranking step now reaches the `Math` class: it ranks the declared
// forms the probe already collects and reports the tie as §22.6's ambiguity
// error. There is no list of types anywhere in it — the verdict follows from
// the declared forms and §5.1's table, so `byte`, `sbyte`, `short`, `ushort`,
// `int`, `uint`, `long`, `ulong` and `char` are all judged by the same
// question, and the next type to reach the position will be too.
//
// THE PROOF HIERARCHY.
//
// Every refusal row below FAILS on the base arm: the program compiled, ran
// and printed the value, so there was NO diagnostic at all. Each row pins the
// refusal AND the diagnostic's wording AND the absence of the pre-fix
// symptoms, so a fix that merely turned the acceptance into a different
// failure would not pass.
//
// The acceptance rows are the CONTROLS: green on BOTH arms, and they prove
// the fix did not over-reject. They cover the three shapes that are NOT
// ambiguous (a `double`, a `decimal` and a `float` — a `float` reaches only
// the `double` form, §5.1 making `float`→`decimal` never implicit), the
// result TYPE of each surviving form, and §2.2's / the v0.228 record's own
// remedy (an explicit conversion).
//
// THE DOUBLE-REPORT CONTROL.
//
// `Math.Abs` declares four forms (`int`, `long`, `double`, `decimal`) and
// makes the same tie for the same narrow types, but #6955's dedicated §22.6
// check owns that member. The row below asserts that `Math.Abs` still
// produces exactly ONE diagnostic — a call that produced two would be a
// defect.
//
// THE EXPLICIT-TYPE-ARGUMENT CONTROL.
//
// `Math.Floor<int>(b)` is §14.5's all-or-nothing rule's business (no member
// of §2.2's `Math` declares a type parameter), and that rule REPORTS. The
// ranking step stands down there rather than adding a SECOND diagnostic to
// the same call.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "import System; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Every diagnostic's message, joined — the AOT oracle reports the driver's
// captured stderr as one blob, so a COUNT of a needle is the way to assert
// "exactly one diagnostic".
std::string allMessages(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics())
        out += d.message + "\n";
    return out;
}

int countOf(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The §22.6 AMBIGUITY refusal: the program did not run, the driver rejected
// it at a REAL source location (never a crash / signal / timeout / ASan
// report), the diagnostic is the ambiguity one, and the §5.1 "no matching
// overload" wording is ABSENT — the ambiguity rule owns this call, so the
// applicability probe must add nothing.
void expectAmbiguous(const JitResult& r, const std::string& member) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("ambiguous call to `Math." + member + "`").c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no overload is unambiguously better (§22.6)"));
    // The §22.6 shape: the two declared candidates are cited.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "x: double"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "x: decimal"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §2.2"));
    // The applicability probe must not double-report: an integer argument
    // DOES convert to `double` and to `decimal`, so no candidate is
    // inapplicable.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, ("no matching overload of `Math." + member + "`").c_str()));
    // The pre-fix symptoms: the program compiled, ran and printed.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    // Exactly ONE ambiguity diagnostic — the two rules must not meet.
    BEZEL_EXPECT_EQ(countOf(allMessages(r),
                            "ambiguous call to `Math." + member + "`"),
                    1);
}

// The accepting shape: the program RAN and produced exactly this text.
void expectValue(const std::string& body, const std::string& expected) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty() && out.back() == '\r') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// ===========================================================================
// THE REFUSAL ROWS — an integer or `char` argument ties the two declared
// forms. Every one of them compiled, ran and printed on the base arm.
// ===========================================================================

// The issue's own repro: a `byte` converts to BOTH `double` and `decimal`
// (§5.1's table) and matches neither exactly.
BEZEL_TEST(math_ambiguity_7289, floor_byte_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Math.Floor(b));")),
        "Floor");
}

// The sibling narrow types: `sbyte`, `ushort`, `short`.
BEZEL_TEST(math_ambiguity_7289, floor_sbyte_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var sb: sbyte := -100; "
        "    Console.WriteLine(Math.Floor(sb));")),
        "Floor");
}

BEZEL_TEST(math_ambiguity_7289, floor_ushort_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var us: ushort := 60000; "
        "    Console.WriteLine(Math.Floor(us));")),
        "Floor");
}

BEZEL_TEST(math_ambiguity_7289, floor_short_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var sh: short := -300; "
        "    Console.WriteLine(Math.Floor(sh));")),
        "Floor");
}

// `int` AND `long` ARE IN THE CLASS. The v0.228 record says "an integer type
// or `char`", and `Abs` is unaffected only because it DECLARES an `int`
// overload — an exact match. `Floor` declares none, so an `int` argument
// converts to `double` and to `decimal` and matches neither exactly.
BEZEL_TEST(math_ambiguity_7289, floor_int_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var i: int := 16; "
        "    Console.WriteLine(Math.Floor(i));")),
        "Floor");
}

BEZEL_TEST(math_ambiguity_7289, floor_long_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var l: long := 16L; "
        "    Console.WriteLine(Math.Floor(l));")),
        "Floor");
}

BEZEL_TEST(math_ambiguity_7289, floor_uint_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var u: uint := 4000000000u; "
        "    Console.WriteLine(Math.Floor(u));")),
        "Floor");
}

BEZEL_TEST(math_ambiguity_7289, floor_ulong_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var ul: ulong := 18446744073709551615UL; "
        "    Console.WriteLine(Math.Floor(ul));")),
        "Floor");
}

// `char` is named FIRST in the v0.228 record's class ("an integer type or
// `char`"): §5.1's table gives it `int`, `uint`, `long`, `ulong`, `float`,
// `double` and `decimal`.
BEZEL_TEST(math_ambiguity_7289, floor_char_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var c: char := 'a'; "
        "    Console.WriteLine(Math.Floor(c));")),
        "Floor");
}

// The two sibling members, at the same shape.
BEZEL_TEST(math_ambiguity_7289, ceiling_byte_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Math.Ceiling(b));")),
        "Ceiling");
}

BEZEL_TEST(math_ambiguity_7289, truncate_byte_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Math.Truncate(b));")),
        "Truncate");
}

// A COMPILE-TIME INTEGER CONSTANT is read at its own static type (§22.6: "a
// compile-time integer constant is read at its own static type too — `int` for
// an unsuffixed literal that fits `int`"), so it ties exactly as an `int`
// variable does.
BEZEL_TEST(math_ambiguity_7289, floor_int_constant_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    Console.WriteLine(Math.Floor(5));")),
        "Floor");
}

BEZEL_TEST(math_ambiguity_7289, floor_uint_constant_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    Console.WriteLine(Math.Floor(5u));")),
        "Floor");
}

// The NAMED-ARGUMENT spelling: §22.5 binds the argument to the parameter it
// names, and the ranking step judges the bound position, so the verdict is
// the positional one's.
BEZEL_TEST(math_ambiguity_7289, floor_named_argument_is_ambiguous_2_2) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Math.Floor(x := b));")),
        "Floor");
}

// ===========================================================================
// THE CONTROLS — the shapes that are NOT ambiguous (green on both arms).
// ===========================================================================

// A `double` is an EXACT match at the `double` form.
BEZEL_TEST(math_ambiguity_7289, floor_double_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Floor(2.5));", "2");
}

// A `decimal` is an EXACT match at the `decimal` form, and §5.1 makes
// `decimal`→`double` never implicit, so exactly one candidate is applicable.
BEZEL_TEST(math_ambiguity_7289, floor_decimal_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Floor(2.5m));", "2");
}

// A `float` converts to `double` and to NOTHING else (§5.1: "Between
// `decimal` and binary floats (`float`, `double`)"), so exactly one candidate
// is applicable and the call resolves to the `double` form.
BEZEL_TEST(math_ambiguity_7289, floor_float_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Floor(2.5f));", "2");
}

BEZEL_TEST(math_ambiguity_7289, ceiling_double_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Ceiling(2.5));", "3");
}

BEZEL_TEST(math_ambiguity_7289,
           ceiling_decimal_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Ceiling(2.5m));", "3");
}

BEZEL_TEST(math_ambiguity_7289, ceiling_float_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Ceiling(2.5f));", "3");
}

BEZEL_TEST(math_ambiguity_7289, truncate_double_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Truncate(2.5));", "2");
}

BEZEL_TEST(math_ambiguity_7289,
           truncate_decimal_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Truncate(2.5m));", "2");
}

BEZEL_TEST(math_ambiguity_7289, truncate_float_argument_is_unambiguous_2_2) {
    expectValue("    Console.WriteLine(Math.Truncate(2.5f));", "2");
}

// THE RESULT TYPE of each surviving form — the floating-point hazard. The
// `double` form's result is a `double` and the `decimal` form's is a
// `decimal`, and the two must not be confused.
BEZEL_TEST(math_ambiguity_7289, floor_result_type_follows_the_declared_form) {
    expectValue("    var d: double := Math.Floor(2.5); "
                "    var m: decimal := Math.Floor(2.5m); "
                "    Console.WriteLine(d); "
                "    Console.WriteLine(m);",
                "2\n2");
}

// THE REMEDY the v0.228 record names: "an explicit conversion
// (`Math.Floor(double(n))`)". The value is the SAME one the base arm printed
// for the implicit call — "200" — so the remedy changes the spelling, not
// the answer.
BEZEL_TEST(math_ambiguity_7289, floor_explicit_conversion_is_the_remedy) {
    expectValue("    var b: byte := 200; "
                "    Console.WriteLine(Math.Floor(double(b))); "
                "    Console.WriteLine(Math.Floor(decimal(b)));",
                "200\n200");
}

// ===========================================================================
// THE OVER-REJECTION CONTROL — the members of §2.2 that are NOT in the class.
// `Sqrt`, `Pow`, `Round`, `Fmod` and `IEEERemainder` declare a SINGLE
// parameter type, so there is no ambiguity to decide; `Sign`, `Min`, `Max` and
// `Clamp` are generic, so §22.6's applicability has no concrete parameter
// type there. None of them is affected by this fix.
// ===========================================================================

BEZEL_TEST(math_ambiguity_7289, single_parameter_type_members_are_unchanged) {
    expectValue("    var b: byte := 200; "
                "    Console.WriteLine(Math.Sqrt(b)); "
                "    Console.WriteLine(Math.Pow(b, 2.0)); "
                "    Console.WriteLine(Math.Round(b)); "
                "    Console.WriteLine(Math.Fmod(b, 3)); "
                "    Console.WriteLine(Math.IEEERemainder(b, 3)); "
                "    Console.WriteLine(Math.Sign(b));",
                "14.142135623730951\n40000\n200\n2\n-1\n1");
}

// The generic selectors at an UNSIGNED argument are untouched: `Min`/`Max`/
// `Clamp` declare a type parameter, so an unsigned operand is its own `T`.
BEZEL_TEST(math_ambiguity_7289, generic_selectors_are_unchanged) {
    expectValue("    var u: uint := 4000000000u; "
                "    Console.WriteLine(Math.Max(u, 3u)); "
                "    Console.WriteLine(Math.Min(u, 3u)); "
                "    Console.WriteLine(Math.Clamp(u, 3u, 4000000001u));",
                "4000000000\n3\n4000000000");
}

// ===========================================================================
// THE DOUBLE-REPORT CONTROL — `Math.Abs` keeps EXACTLY ONE diagnostic.
// ===========================================================================

BEZEL_TEST(math_ambiguity_7289, math_abs_still_reports_exactly_one) {
    auto r = runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Math.Abs(b));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "ambiguous call to `Math.Abs`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.6"));
    // ONE diagnostic, not two: #6955's dedicated check owns `Math.Abs`, and
    // the ranking step must not meet it.
    BEZEL_EXPECT_EQ(countOf(allMessages(r), "ambiguous call to"), 1);
}

// ===========================================================================
// THE EXPLICIT-TYPE-ARGUMENT CONTROL — §14.5's all-or-nothing rule owns a
// call that carries an explicit type-argument list, and it REPORTS. The
// ranking step stands down there rather than adding a second diagnostic.
// ===========================================================================

BEZEL_TEST(math_ambiguity_7289, explicit_type_argument_reports_once) {
    auto r = runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Math.Floor<int>(b));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "but it declares no type parameters"));
    // ONE diagnostic: §14.5's, not §22.6's.
    BEZEL_EXPECT_EQ(countOf(allMessages(r), "ambiguous call to"), 0);
    BEZEL_EXPECT_EQ(countOf(allMessages(r), "explicit type argument(s)"), 1);
}
