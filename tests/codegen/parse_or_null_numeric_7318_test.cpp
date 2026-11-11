// Spec-derived regression tests for fix #7318 — the numeric `ParseOrNull`
// family (`int`/`Int32`, `long`/`Int64`, the rest of the integer tower,
// `double`/`Double`, `float`/`Float`, `decimal`/`Decimal`).
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §9.1 (Integer types, v0.357) declares, on Int32:
//       public static pure method ParseOrNull(s: string): int?;   // (§1 *OrNull tier)
//   and says "SByte, Byte, Int16, UInt16, Int32, UInt32, Int64, UInt64 … the
//   others have analogous shapes with their respective MinValue/MaxValue".
//   §9.1's closing note scopes the member: "The `ParseOrNull` members are the
//   §1 `*OrNull` convenience tier's parse form — string input only, no radix
//   or span duals — and share the span forms' per-type, non-constraint status."
//
//   stdlib §9.2 (Double and Float, v0.357) declares
//       public static pure method ParseOrNull(s: string): double?;
//       public static pure method ParseOrNull(s: string): float?;
//
//   stdlib §9.3 (Decimal, v0.357) declares
//       public static pure method ParseOrNull(s: string): decimal?;
//
//   stdlib §1's `*OrNull` tier and stdlib §2.5's parse contract (v0.357)
//   state the ONE rule every row below rests on:
//       "Every `TryParse` returns `false`, and every `ParseOrNull` returns
//        `null` (the §1 `*OrNull` tier), exactly where its `Parse` would
//        raise."
//
//   docs/Bezel_For_Code_Generation.md §5.9 (v0.357) lists the member for the
//   whole integer tower — "static pure ParseOrNull(s: string): T?" — beside
//   `Double`/`Float`/`Decimal`/`Boolean`/`Char`.
//
// THE DEFECT (issue #7318).  `int.ParseOrNull("42")` was REJECTED by sema:
//     "type 'int' has no static method or property 'ParseOrNull' (§3.4)"
// — for the keyword spelling AND the PascalCase spelling, for `int`,
// `long`, `double`, `float` and `decimal` alike, while `bool.ParseOrNull` and
// `char.ParseOrNull` were admitted and lowered.  §3.4 is "Keyword aliases for
// built-in types", NOT a closed-set rule: whether the member is admissible is
// decided by what the stdlib declares, and the stdlib declares it.  The
// compiler's own tables already knew the member — `src/common/BuiltinRegistry.cpp`
// carries a `ParseOrNull` row for every one of these heads, and
// `src/sema/PureChecker.cpp` carries the `kIntegerFamily` / `kFloatingFamily`
// rows — so the gap was in the LOOKUP, not in the tables.
//
// THE FIX.
//   * sema (`ExprTypecheck_Calls.cpp::resolveMemberAccess`): the primitive
//     static closed set now admits `ParseOrNull` for the integer tower, for
//     double/float and for decimal, typed as the receiver's own nullable
//     primitive (`int?`, `double?`, `decimal?`, …).
//   * codegen (`LlvmBackend_Calls2.cpp::tryLowerPrimitiveStaticCall`): a
//     `ParseOrNull` arm per family, sharing the ONE parse check its `TryParse`
//     sibling uses (the integer tower's `emitIntTowerParse`, the
//     floating-point `emitFpParse`, and the decimal runtime entry
//     `bz_decimal_try_parse`), so the two forms agree on every input BY
//     CONSTRUCTION and the *OrNull tier's "null exactly where Parse raises" is
//     the Try form's "false exactly where Parse raises" with the failure
//     carried by the null instead of an out-parameter.
//
// THE ORACLE.  A row that asserted only "the program compiles" would be green
// on the pre-fix compiler for the wrong reason (the program would not compile
// AT ALL, so the row must assert the RUN and the VALUE).  Every row here
// asserts either
//   * the program's exit status, which is the value the parse produced (the
//     issue's own acceptance criterion: "the test must pin that
//     `ParseOrNull` on a VALID input returns the PARSED VALUE — not that it
//     'compiles' and not that it 'does not degrade'"), or
//   * the program's exact stdout, which for the matrix rows records, per
//     input, whether the result was null or not AND whether `TryParse`
//     agreed (`null/False`, `some/True`) — the §1 contract in one line.
//
// Without the fix every `ParseOrNull` row fails: the program does not compile
// (the run never happens, so the harness sees "did not run").

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The exit status of a program that RAN, or -999 when it did not run (a
// front-end rejection, a failed compile, a crash).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// A program that must be REJECTED by the front end.
bool hasSemaError(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// A program that must RUN CLEANLY and print exactly `want`.
void expectOutput(const std::string& src, const std::string& want) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The one-line program shape used by the exit-status rows.
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

// The matrix program for one receiver type.
//
// For each input it prints
//     [<input>] -> null/<TryParse ok>      (the *OrNull result is null)
//     [<input>] -> some/<TryParse ok>      (the *OrNull result is present)
// so that each row pins BOTH halves of the §1 contract: the *OrNull tier
// returns null exactly where the Try form says "false" (i.e. exactly where
// `Parse` would raise), and it returns a VALUE (not null) everywhere else.
//
// `type` is the receiver's keyword spelling ("int", "long", "double",
// "float", "decimal"); the value type of the TryParse out-parameter is the
// same name.
std::string matrixProg(const std::string& type,
                        const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs)
        body += "    Show(\"" + in + "\");\n";
    return "module Conf;\n"
           "public class Program\n"
           "  private static method Show(s: string)\n"
           "    var v: " + type + ";\n"
           "    var ok := " + type + ".TryParse(s, out v);\n"
           "    var o := " + type + ".ParseOrNull(s);\n"
           "    if o = null then\n"
           "      Console.WriteLine(\"[\" + s + \"] -> null/\" + ok.ToString());\n"
           "    else\n"
           "      Console.WriteLine(\"[\" + s + \"] -> some/\" + ok.ToString());\n"
           "    end;\n"
           "  end Show;\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The expected stdout of `matrixProg`: one line per input.
std::string wantMatrix(const std::vector<std::string>& inputs,
                       const std::vector<std::string>& results) {
    std::string want;
    for (std::size_t i = 0; i < inputs.size(); ++i)
        want += "[" + inputs[i] + "] -> " + results[i] + "\n";
    return want;
}

// The matrix program's expected stdout for `admitted` inputs (each "some/True")
// followed by `rejected` ones (each "null/False").
std::string wantSplit(const std::vector<std::string>& admitted,
                      const std::vector<std::string>& rejected) {
    std::vector<std::string> inputs = admitted;
    std::vector<std::string> results;
    for (std::size_t i = 0; i < admitted.size(); ++i)
        results.push_back("some/True");
    for (std::size_t i = 0; i < rejected.size(); ++i) {
        inputs.push_back(rejected[i]);
        results.push_back("null/False");
    }
    return wantMatrix(inputs, results);
}

// ---------------------------------------------------------------------------
// The matrix inputs, per type.
//
// Every list carries the SAME five shapes, so the rows are comparable across
// types:
//   1. a VALID input           -> the parsed value (not null)
//   2. an INVALID input        -> null (Parse would raise FormatException)
//   3. the EMPTY string        -> null
//   4. a WHITESPACE-padded one -> null (stdlib §9.1/§9.2/§9.3: "no
//                                 whitespace tolerance"; the strict-fault model)
//   5. an OUT-OF-RANGE one     -> null (Parse would raise ParseRangeException,
//                                 an OverflowException)
//
// The `long` list is the ONE exception: it carries the first FOUR shapes
// only.  Its out-of-range cell belongs to #8540 (see the row below).
// ---------------------------------------------------------------------------
const std::vector<std::string> kIntInputs = {
    "42", "abc", "", " 42", "2147483648"};
const std::vector<std::string> kLongInputs = {
    "42", "abc", "", " 42"};
const std::vector<std::string> kDoubleInputs = {
    "1.5", "abc", "", " 1.5", "1e999"};
const std::vector<std::string> kFloatInputs = {
    "1.5", "abc", "", " 1.5", "3.5e38"};
const std::vector<std::string> kDecimalInputs = {
    "1.5", "abc", "", " 1.5", "79228162514264337593543950336"};

}  // namespace

// ===========================================================================
// A. THE ISSUE'S OWN REPRO — a VALID input returns the PARSED VALUE.
// ===========================================================================

// The issue's exact repro: `int.ParseOrNull("42")`. Before the fix this was
// REJECTED ("type 'int' has no static method or property 'ParseOrNull'
// (§3.4)"); after it, the program runs and returns the PARSED value 42.
BEZEL_TEST(parse_or_null_7318, int_parse_or_null_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := int.ParseOrNull(\"42\"); "
        "if a = null then return -1; end; "
        "if a! = 42 then return 42; end; "
        "return 0; "
        )), 42);
}

// The PascalCase spelling of the same member (stdlib §9.1 declares the member
// on Int32; §3.4 admits either spelling of the built-in type).
BEZEL_TEST(parse_or_null_7318, int32_parse_or_null_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := Int32.ParseOrNull(\"42\"); "
        "if a = null then return -1; end; "
        "if a! = 42 then return 42; end; "
        "return 0; "
        )), 42);
}

// stdlib §9.1's `Int64` (the `long` keyword) — the integer tower's other
// 64-bit member.
BEZEL_TEST(parse_or_null_7318, long_parse_or_null_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := long.ParseOrNull(\"42\"); "
        "if a = null then return -1; end; "
        "if a! = 42L then return 42; end; "
        "return 0; "
        )), 42);
}

BEZEL_TEST(parse_or_null_7318, int64_parse_or_null_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := Int64.ParseOrNull(\"42\"); "
        "if a = null then return -1; end; "
        "if a! = 42L then return 42; end; "
        "return 0; "
        )), 42);
}

// stdlib §9.2 — `double.ParseOrNull("1.5")` is Some(1.5), NOT null.
BEZEL_TEST(parse_or_null_7318, double_parse_or_null_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := double.ParseOrNull(\"1.5\"); "
        "if a = null then return -1; end; "
        "if a! = 1.5 then return 42; end; "
        "return 0; "
        )), 42);
}

BEZEL_TEST(parse_or_null_7318, double_name_spelling_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := Double.ParseOrNull(\"1.5\"); "
        "if a = null then return -1; end; "
        "if a! = 1.5 then return 42; end; "
        "return 0; "
        )), 42);
}

// stdlib §9.2 — `float.ParseOrNull("1.5")` is Some(1.5), NOT null.
BEZEL_TEST(parse_or_null_7318, float_parse_or_null_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := float.ParseOrNull(\"1.5\"); "
        "if a = null then return -1; end; "
        "if a! = 1.5f then return 42; end; "
        "return 0; "
        )), 42);
}

BEZEL_TEST(parse_or_null_7318, float_name_spelling_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := Float.ParseOrNull(\"1.5\"); "
        "if a = null then return -1; end; "
        "if a! = 1.5f then return 42; end; "
        "return 0; "
        )), 42);
}

// stdlib §9.3 — `decimal.ParseOrNull("1.5")` is Some(1.5m), NOT null.
BEZEL_TEST(parse_or_null_7318, decimal_parse_or_null_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := decimal.ParseOrNull(\"1.5\"); "
        "if a = null then return -1; end; "
        "if a! = 1.5m then return 42; end; "
        "return 0; "
        )), 42);
}

BEZEL_TEST(parse_or_null_7318, decimal_name_spelling_returns_the_parsed_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := Decimal.ParseOrNull(\"1.5\"); "
        "if a = null then return -1; end; "
        "if a! = 1.5m then return 42; end; "
        "return 0; "
        )), 42);
}

// ===========================================================================
// B. THE MATRIX — valid / invalid / empty / whitespace / overflow, per type.
// ===========================================================================

// stdlib §9.1 — int: "42" parses, "abc" / "" / " 42" / "2147483648" (out of
// int's range) are null.  "2147483648" is one past int.MaxValue, so `Parse`
// raises ParseRangeException and `ParseOrNull` is null.
BEZEL_TEST(parse_or_null_7318, int_matrix) {
    expectOutput(matrixProg("int", kIntInputs),
                 wantMatrix(kIntInputs,
                            {"some/True", "null/False", "null/False",
                             "null/False", "null/False"}));
}

// stdlib §9.1 — long.  The FOUR cells here are the valid / invalid / empty /
// whitespace shapes.
//
// The 64-bit width's OUT-OF-RANGE shape is NOT pinned here, and that is
// deliberate.  stdlib §9.1 says an out-of-range value raises
// ParseRangeException, so the §1 `*OrNull` tier's rule ("null exactly where
// its `Parse` would raise") makes `ParseOrNull` NULL on an out-of-range
// 64-bit input.  But the 64-bit widths' `Parse`/`TryParse` arms SATURATE
// there instead of raising (MEASURED on the base compiler:
// `long.Parse("9223372036854775808")` returns 9223372036854775807 and does
// not raise, and `long.TryParse` of the same input returns true), so the
// *OrNull tier — which shares the ONE parse check with its Try sibling —
// currently returns the SATURATED value there, NOT null.
//
// That saturation is a pre-existing defect in the 64-bit `Parse`/`TryParse`
// arms, filed as #8540 and OUT of this issue's scope: the case belongs to
// #8540's fix.  Pinning the saturated value here would freeze the wrong
// answer as if it were the specified one, and pinning null would fail
// against the compiler as it stands — so the out-of-range cell is left
// out of this row until #8540 makes the 64-bit arms raise, at which point
// #8540's own fix re-adds it (as "null/False") with the rest.
BEZEL_TEST(parse_or_null_7318, long_matrix) {
    expectOutput(matrixProg("long", kLongInputs),
                 wantMatrix(kLongInputs,
                            {"some/True", "null/False", "null/False",
                             "null/False"}));
}

// stdlib §9.2 — double: "1e999" spells a finite number that is not
// representable, so `Parse` raises ParseRangeException and `ParseOrNull` is
// null (§9.2's range paragraph).
BEZEL_TEST(parse_or_null_7318, double_matrix) {
    expectOutput(matrixProg("double", kDoubleInputs),
                 wantMatrix(kDoubleInputs,
                            {"some/True", "null/False", "null/False",
                             "null/False", "null/False"}));
}

// stdlib §9.2 — float: "3.5e38" is out of float's range (§9.2 names it).
BEZEL_TEST(parse_or_null_7318, float_matrix) {
    expectOutput(matrixProg("float", kFloatInputs),
                 wantMatrix(kFloatInputs,
                            {"some/True", "null/False", "null/False",
                             "null/False", "null/False"}));
}

// stdlib §9.3 — decimal: "79228162514264337593543950336" is one past
// decimal.MaxValue.
BEZEL_TEST(parse_or_null_7318, decimal_matrix) {
    expectOutput(matrixProg("decimal", kDecimalInputs),
                 wantMatrix(kDecimalInputs,
                            {"some/True", "null/False", "null/False",
                             "null/False", "null/False"}));
}

// ===========================================================================
// C. THE `??` COMPOSITION — the reason the *OrNull tier exists (§1).
// ===========================================================================

// The *OrNull tier exists so the caller can compose with `??`: the fallback
// is taken exactly when the parse failed.
BEZEL_TEST(parse_or_null_7318, int_parse_or_null_coalesce_with_fallback) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := int.ParseOrNull(\"abc\") ?? 7; "
        "var b := int.ParseOrNull(\"42\") ?? 7; "
        "if a <> 7 then return 1; end; "
        "if b <> 42 then return 2; end; "
        "return 0; "
        )), 0);
}

BEZEL_TEST(parse_or_null_7318, long_parse_or_null_coalesce_with_fallback) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := long.ParseOrNull(\"abc\") ?? 7L; "
        "var b := long.ParseOrNull(\"42\") ?? 7L; "
        "if a <> 7L then return 1; end; "
        "if b <> 42L then return 2; end; "
        "return 0; "
        )), 0);
}

BEZEL_TEST(parse_or_null_7318, double_parse_or_null_coalesce_with_fallback) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := double.ParseOrNull(\"abc\") ?? 1.5; "
        "var b := double.ParseOrNull(\"2.5\") ?? 1.5; "
        "if a <> 1.5 then return 1; end; "
        "if b <> 2.5 then return 2; end; "
        "return 0; "
        )), 0);
}

BEZEL_TEST(parse_or_null_7318, float_parse_or_null_coalesce_with_fallback) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := float.ParseOrNull(\"abc\") ?? 1.5f; "
        "var b := float.ParseOrNull(\"2.5\") ?? 1.5f; "
        "if a <> 1.5f then return 1; end; "
        "if b <> 2.5f then return 2; end; "
        "return 0; "
        )), 0);
}

BEZEL_TEST(parse_or_null_7318, decimal_parse_or_null_coalesce_with_fallback) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := decimal.ParseOrNull(\"abc\") ?? 1.5m; "
        "var b := decimal.ParseOrNull(\"2.5\") ?? 1.5m; "
        "if a <> 1.5m then return 1; end; "
        "if b <> 2.5m then return 2; end; "
        "return 0; "
        )), 0);
}

// ===========================================================================
// D. THE WHOLE INTEGER TOWER — stdlib §9.1's "analogous shapes".
// ===========================================================================

// Every one of §9.1's eight integer types carries the member, and each parses
// its own valid input (a PRESENT result, not null).
BEZEL_TEST(parse_or_null_7318, integer_tower_every_member_parses) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := sbyte.ParseOrNull(\"-8\"); "
        "if b = null then return 1; end; "
        "var y := byte.ParseOrNull(\"255\"); "
        "if y = null then return 2; end; "
        "var s := short.ParseOrNull(\"-32768\"); "
        "if s = null then return 3; end; "
        "var u := ushort.ParseOrNull(\"65535\"); "
        "if u = null then return 4; end; "
        "var i := int.ParseOrNull(\"-2147483648\"); "
        "if i = null then return 5; end; "
        "var n := uint.ParseOrNull(\"4294967295\"); "
        "if n = null then return 6; end; "
        "var l := long.ParseOrNull(\"-9223372036854775808\"); "
        "if l = null then return 7; end; "
        "var g := ulong.ParseOrNull(\"18446744073709551615\"); "
        "if g = null then return 8; end; "
        "return 0; "
        )), 0);
}

// The tower's members are null on an invalid input, like every other one.
BEZEL_TEST(parse_or_null_7318, integer_tower_every_member_null_on_garbage) {
    BEZEL_EXPECT_EQ(run(prog(
        "var n := 0; "
        "var b := sbyte.ParseOrNull(\"x\"); "
        "if b = null then n := n + 1; end; "
        "var y := byte.ParseOrNull(\"x\"); "
        "if y = null then n := n + 1; end; "
        "var s := short.ParseOrNull(\"x\"); "
        "if s = null then n := n + 1; end; "
        "var u := ushort.ParseOrNull(\"x\"); "
        "if u = null then n := n + 1; end; "
        "var i := int.ParseOrNull(\"x\"); "
        "if i = null then n := n + 1; end; "
        "var q := uint.ParseOrNull(\"x\"); "
        "if q = null then n := n + 1; end; "
        "var l := long.ParseOrNull(\"x\"); "
        "if l = null then n := n + 1; end; "
        "var g := ulong.ParseOrNull(\"x\"); "
        "if g = null then n := n + 1; end; "
        "if n <> 8 then return n; end; "
        "return 0; "
        )), 0);
}

// ===========================================================================
// E. §9.2's NON-FINITE SPELLINGS AND THE GRAMMAR'S REJECTIONS.
// ===========================================================================

// stdlib §9.2: exactly "NaN", "Infinity" and "-Infinity" are admitted (they
// stand outside the numeric grammar); "nan", "inf", "+Infinity" and the bare
// points ".5" / "+.5" / "5." are NOT.  Each admitted spelling yields a
// PRESENT result (not null) — the *OrNull tier's null is reserved for the
// inputs `Parse` raises on.
BEZEL_TEST(parse_or_null_7318, double_non_finite_spellings_and_grammar) {
    expectOutput(matrixProg("double",
                            {"NaN", "Infinity", "-Infinity", "nan", "inf",
                             "infinity", "INFINITY", "+Infinity", "-NaN",
                             ".5", "+.5", "5.", " 5", "5 ", "1_000", "0x1p3"}),
                 wantSplit({"NaN", "Infinity", "-Infinity"},
                           {"nan", "inf", "infinity", "INFINITY", "+Infinity",
                            "-NaN", ".5", "+.5", "5.", " 5", "5 ", "1_000",
                            "0x1p3"}));
}

// The same grammar on `float` (§9.2: "Float has the analogous shape").
BEZEL_TEST(parse_or_null_7318, float_non_finite_spellings_and_grammar) {
    expectOutput(matrixProg("float",
                            {"NaN", "Infinity", "-Infinity", "nan", "inf",
                             "+Infinity", ".5", "+.5", "5.", " 5", "5 "}),
                 wantSplit({"NaN", "Infinity", "-Infinity"},
                           {"nan", "inf", "+Infinity", ".5", "+.5", "5.",
                            " 5", "5 "}));
}

// stdlib §9.2's range paragraph: "A value that rounds to zero or to a
// subnormal is in range: `double.Parse("1e-400")` is `0`."  So the *OrNull
// tier returns a PRESENT 0.0 there, not null.
BEZEL_TEST(parse_or_null_7318, double_underflow_is_in_range) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := double.ParseOrNull(\"1e-400\"); "
        "if a = null then return -1; end; "
        "if a! = 0.0 then return 42; end; "
        "return 0; "
        )), 42);
}

// stdlib §9.2's range paragraph: a FINITE spelling that rounds to a
// non-finite value is out of range (`ParseRangeException`), so the *OrNull
// tier is null there — and a spelling the GRAMMAR rejects is null too
// ("An input the grammar does not admit raises FormatException whatever
// number a part of it spells: `"1_000e999"` raises FormatException, not the
// ParseRangeException below").
BEZEL_TEST(parse_or_null_7318, double_range_and_grammar_failures_are_null) {
    const std::vector<std::string> inputs = {"1e999", "-1e999", "1_000e999"};
    expectOutput(matrixProg("double", inputs),
                 wantMatrix(inputs,
                            {"null/False", "null/False", "null/False"}));
}

// stdlib §9.1's integer grammar: an optional `+`/`-`, then digits, and
// nothing else — no whitespace, no separators, no `0x` prefix.  A signed
// input IS admitted.
BEZEL_TEST(parse_or_null_7318, int_grammar_accepts_sign_and_rejects_the_rest) {
    expectOutput(matrixProg("int",
                            {"42", "+42", "-42", "007", "42 ", " 42", "4 2",
                             "1_000", "0x2A", "", "4.0"}),
                 wantSplit({"42", "+42", "-42", "007"},
                           {"42 ", " 42", "4 2", "1_000", "0x2A", "", "4.0"}));
}

// stdlib §9.3's decimal grammar: an optional sign, digits, and an optional
// `.` with digits on both sides — no exponent, no bare point.
BEZEL_TEST(parse_or_null_7318, decimal_grammar_rows) {
    expectOutput(matrixProg("decimal",
                            {"1.5", "+1.5", "-1.5", "007.50", ".5", "5.", ".",
                             "1.2.3", "1e3", "1_000", " 1.5"}),
                 wantSplit({"1.5", "+1.5", "-1.5", "007.50"},
                           {".5", "5.", ".", "1.2.3", "1e3", "1_000", " 1.5"}));
}

// ===========================================================================
// F. THE DECLARED SIGNATURE — string input only, no radix and no span dual.
// ===========================================================================

// stdlib §9.1: "The `ParseOrNull` members are the §1 `*OrNull` convenience
// tier's parse form — string input only, no radix or span duals".  The
// declared parameter is a `string` (NOT a `string?`), so a nullable string
// argument is NOT admitted — the §7.3 nullability gate refuses it, exactly as
// it refuses one for `int.Parse`.
BEZEL_TEST(parse_or_null_7318, string_nullable_argument_is_not_admitted) {
    BEZEL_EXPECT_TRUE(hasSemaError(prog(
        "var s: string? := null; "
        "var a := int.ParseOrNull(s); "
        "return 0; "
        )));
}

// The radix form is NOT declared for `ParseOrNull` (only `Parse`/`TryParse`
// carry it), so a two-argument call is refused.
BEZEL_TEST(parse_or_null_7318, radix_dual_is_not_declared) {
    BEZEL_EXPECT_TRUE(hasSemaError(prog(
        "var a := int.ParseOrNull(\"ff\", 16); "
        "return 0; "
        )));
}

// The span form is NOT declared for `ParseOrNull` either (§9.1: "no radix or
// span duals") — only `Parse`/`TryParse` carry the `ReadOnlyStringSpan`
// overload.
BEZEL_TEST(parse_or_null_7318, span_dual_is_not_declared) {
    BEZEL_EXPECT_TRUE(hasSemaError(prog(
        "var sp := new ReadOnlyStringSpan(\"42\", 0, 2); "
        "var a := int.ParseOrNull(sp); "
        "return 0; "
        )));
}

// ===========================================================================
// G. CONTROLS — the members that already worked must not regress, and the
// members that are NOT declared must stay rejected.
// ===========================================================================

// stdlib §9.4 / §9.5 — `bool.ParseOrNull` and `char.ParseOrNull` (the #1792
// arms) still work.  This fix must not regress them.
BEZEL_TEST(parse_or_null_7318, bool_and_char_parse_or_null_still_work) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := bool.ParseOrNull(\"true\"); "
        "if b = null then return 1; end; "
        "if b! <> true then return 2; end; "
        "var c := char.ParseOrNull(\"z\"); "
        "if c = null then return 3; end; "
        "if c! <> 'z' then return 4; end; "
        "var bad := bool.ParseOrNull(\"nope\"); "
        "if bad = null then return 0; end; "
        "return 5; "
        )), 0);
}

// §3.4's closed set is unchanged for everything the stdlib does NOT declare:
// `Object.ParseOrNull` and `string.ParseOrNull` are not members of any
// stdlib type, so they stay rejected.
BEZEL_TEST(parse_or_null_7318, undeclared_receivers_stay_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(prog(
        "var a := Object.ParseOrNull(\"42\"); "
        "return 0; "
        )));
    BEZEL_EXPECT_TRUE(hasSemaError(prog(
        "var a := string.ParseOrNull(\"42\"); "
        "return 0; "
        )));
}

// The members the stdlib declares on OTHER primitive receivers are still
// refused by name: `int.ParseOrNull` is a member, `int.ParseOrNulls` is not.
BEZEL_TEST(parse_or_null_7318, unknown_member_name_stays_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(prog(
        "var a := int.ParseOrNulls(\"42\"); "
        "return 0; "
        )));
}
