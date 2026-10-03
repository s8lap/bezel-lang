// Regression tests for #5330 (spec §5.2 + §5.3, v0.313): a `decimal` operand
// reaching either of the two cast paths that build a RAW numeric LLVM cast
// instead of routing through the §5.2 decimal helpers.
//
//   (a) the §5.3 lifted cast `T?(x)` (LlvmBackend_Exprs.cpp, the
//       NullCondCallExpr branch): lowerPrimCastScalar saw a 16-byte
//       `%bz.decimal` STRUCT as its "int" source and emitted
//       `sitofp %bz.decimal %nv.val to float`;
//   (b) the explicit conversion `T(x)` inside a generic instantiated at
//       `T = decimal` (LlvmBackend_Exprs2.cpp, lowerConversion): the decimal
//       block was gated on the SEMANTIC source primitive, which is the type
//       PARAMETER inside a generic template body, so the block was skipped and
//       the scalar int->FP path emitted `sitofp %bz.decimal %2 to float`.
//
// Both aborted the compiler with
//   SIToFP source must be integer or integer vector
//     %6 = sitofp %bz.decimal %nv.val to float
//   bezelc: codegen error: IR verification failed
// so every program below failed to COMPILE before the fix — the failure is a
// codegen abort, not a wrong value, and the pinned observable is both "ran" and
// the printed value.
//
// Spec:
//   §5.2 (v0.313) "`double(m)` and `float(m)` for `decimal` `m` round to the
//          nearest representable binary value and never fail".
//   §5.3 (v0.313) "`T?(x)` is admitted exactly when the corresponding
//          non-nullable conversion `T(v)` is admitted ... a present value `v`
//          converts by `T(v)` under that conversion's own semantics", and null
//          stays null.
//
// EXPECTED VALUES are computed independently in python3 with exact
// `fractions.Fraction` arithmetic (nearest binary value, ties to even), never
// by the compiler. For the witness literal
//   16777217.0000000000000000001  (exactly 16777217 + 1e-19):
//   nearest float32 = 16777218 (its neighbouring float32 values are 16777216
//                     and 16777218, at -1.0000000000000000001 and +1 resp.)
//   nearest float64 = 16777217 (the 1e-19 tail is far below a double's
//                     2.2e-16 ulp, so it rounds to 16777217 exactly)
// Decimal -> double -> float would give 16777216, so the float rows are the
// double-rounding witness (#5307's bug must not come back through these paths)
// and the double rows are the mirror witness: a decimal -> float -> double
// path would give 16777216, not 16777217.
//
// Every USER row converts in a 200-iteration loop and prints how many
// iterations matched the expected literal, so a one-shot repro cannot hide a
// value that only settles after the first iteration.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// runAotChecked compiles through the forked Driver::compileFile with
// opts.sanitizeAddress = true, so the GENERATED code is ASan-instrumented and
// the program runs under LeakSanitizer; a leak, use-after-free or
// double-free surfaces as ran == false. A codegen abort (the IR-verification
// failure this file pins) also surfaces as ran == false. Either way the row
// sees "<did-not-run>" and fails on the value, which is the same sentinel
// shape support/aot_support.hpp's own runAotOut uses.
std::string checkedOut(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return out;
}

void expectOut(const std::string& src, const std::string& expected) {
    BEZEL_EXPECT_EQ(checkedOut(src), expected);
}

std::string main0(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// ── (a) §5.3 lifted cast `T?(x)` on a `decimal?` ────────────────────────────

// `float?(n)` where n: decimal? holds 16777217.0000000000000000001m, converted
// once per iteration for 200 iterations.
// Pre-fix: `sitofp %bz.decimal %nv.val to float`, IR verification failed.
std::string u1() {
    return main0("var n: decimal? := 16777217.0000000000000000001m; "
                 "var ok := 0; var i := 0; while i < 200 do "
                 "var nf := float?(n); "
                 "if nf <> null then "
                 "if nf! = 16777218.0f then ok := ok + 1; end; end; "
                 "i := i + 1; end; Console.WriteLine(ok); ");
}

// Same path, double target: the correct single-rounded value is 16777217.0
// (a decimal -> float -> double path would give 16777216).
std::string u2() {
    return main0("var n: decimal? := 16777217.0000000000000000001m; "
                 "var ok := 0; var i := 0; while i < 200 do "
                 "var nd := double?(n); "
                 "if nd <> null then "
                 "if nd! = 16777217.0 then ok := ok + 1; end; end; "
                 "i := i + 1; end; Console.WriteLine(ok); ");
}

// §5.3 "null stays null": a null `decimal?` lifts to a null `float?`, and the
// conversion must not run on the null payload.
std::string u3() {
    return main0("var n: decimal? := null; var nf := float?(n); "
                 "Console.WriteLine(nf = null); Console.WriteLine(nf); ");
}

std::string u4() {
    return main0("var n: decimal? := null; var nd := double?(n); "
                 "Console.WriteLine(nd = null); Console.WriteLine(nd); ");
}

// ── (b) explicit `T(x)` in a generic instantiated at T = decimal ────────────

// `G<decimal>(m)` returning `float` / `double`, one conversion per iteration.
std::string genLoop(const std::string& ret, const std::string& expectedLit) {
    return "module M; class C "
           "public static method G<T>(x: T): " + ret + " return " + ret +
           "(x); end G; "
           "public static method Main(): int "
           "var ok := 0; var i := 0; while i < 200 do "
           "var v := G<decimal>(16777217.0000000000000000001m); "
           "if v = " + expectedLit + " then ok := ok + 1; end; "
           "i := i + 1; end; Console.WriteLine(ok); "
           "return 0; end Main; end C;";
}

// The same generic, printing the value directly.
std::string genPrint(const std::string& ret) {
    return "module M; class C "
           "public static method G<T>(x: T): " + ret + " return " + ret +
           "(x); end G; "
           "public static method Main(): int "
           "Console.WriteLine(G<decimal>(16777217.0000000000000000001m)); "
           "return 0; end Main; end C;";
}

}  // namespace

// ── USER rows: each one failed to COMPILE before the fix ────────────────────

BEZEL_TEST(decimal_nullable_cast_and_generic_5330,
           user_nullable_float_double_rounding) {
    expectOut(u1(), "200");
    expectOut(u2(), "200");
}
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, user_nullable_float_prints) {
    expectOut(main0("var n: decimal? := 16777217.0000000000000000001m; "
                    "var nf := float?(n); Console.WriteLine(nf!); "
                    "Console.WriteLine(nf! = 16777218.0f); "),
             "16777218\nTrue");
}
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, user_nullable_double_prints) {
    expectOut(main0("var n: decimal? := 16777217.0000000000000000001m; "
                    "var nd := double?(n); Console.WriteLine(nd!); "
                    "Console.WriteLine(nd! = 16777217.0); "),
             "16777217\nTrue");
}
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, user_nullable_float_null_stays_null) {
    expectOut(u3(), "True\nnull");
}
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, user_nullable_double_null_stays_null) {
    expectOut(u4(), "True\nnull");
}
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, user_generic_decimal_to_float) {
    expectOut(genLoop("float", "16777218.0f"), "200");
    expectOut(genPrint("float"), "16777218");
}
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, user_generic_decimal_to_double) {
    expectOut(genLoop("double", "16777217.0"), "200");
    expectOut(genPrint("double"), "16777217");
}

// ── CONTROL rows: unchanged paths, green before AND after the fix ──────────

// G<int>(42): float — int -> float, the generic path's pre-existing `sitofp` on
// an i32 (untouched; the new decimal branch is not taken).
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, ctrl_generic_int_to_float) {
    expectOut("module M; class C "
              "public static method G<T>(x: T): float return float(x); end G; "
              "public static method Main(): int "
              "Console.WriteLine(G<int>(42)); return 0; end Main; end C;",
             "42");
}
// G<double>(42.0): float — the fp -> fp arm (FPTrunc), untouched.
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, ctrl_generic_double_to_float) {
    expectOut("module M; class C "
              "public static method G<T>(x: T): float return float(x); end G; "
              "public static method Main(): int "
              "Console.WriteLine(G<double>(42.0)); return 0; end Main; end C;",
             "42");
}
// `int?(n)` — the nullable lift's integer identity arm, untouched.
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, ctrl_nullable_int_identity) {
    expectOut(main0("var n: int? := 42; var nf := int?(n); "
                    "if nf <> null then Console.WriteLine(nf!); end; "),
             "42");
}
// `double?(n)` — the nullable lift's fp identity arm, untouched.
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, ctrl_nullable_double_identity) {
    expectOut(main0("var n: double? := 42.0; var nd := double?(n); "
                    "if nd <> null then Console.WriteLine(nd!); end; "),
             "42");
}
// A real int -> float lifted cast (lowerPrimCastScalar's own sitofp arm), so
// the new decimal branch is not taken by an ordinary widened nullable cast.
BEZEL_TEST(decimal_nullable_cast_and_generic_5330, ctrl_nullable_int_to_double) {
    expectOut(main0("var n: int? := 42; var nd := double?(n); "
                    "if nd <> null then Console.WriteLine(nd!); end; "),
             "42");
}
