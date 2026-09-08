// Codegen — §5.1 / #4046: `Emitter::ctorForArgs`'s `typeClassOf`
// (LlvmBackend_impl.hpp) classified integer primitives (`int`, `long`, `byte`,
// `short`, `char`, …) as type class 1 and the floating family (`float`,
// `double`, `decimal`) as type class 2 — two DIFFERENT classes. The
// `ctorTypeMatches` check then rejected an integer-literal argument against
// an explicit constructor parameter typed `decimal` (or `float`/`double`) as
// a class mismatch, even though §5.1's implicit-conversion table admits
// every integer primitive to `float`/`double`/`decimal` (and the
// "Constant narrowing" rule admits compile-time integer constants to any
// integer type whose range contains the value).
//
// For a BODY-ONLY record whose sole construction route is the explicit
// constructor, this made `ctorForArgs` return null, `buildRecordValue` fall
// to its final "no constructor accepts them" arm, and `lowerLocalDecl`'s
// pre-existing #2434 fallback silently zero-fill the local — a record
// whose fields would have been initialized with the int literal read as
// zero instead, and a record's validating throw in its explicit ctor body
// never ran (the same mis-compile shape as #4039).
//
// The fix widens `ctorTypeMatches` to admit argCls=1 (integer) against
// paramCls=2 (floating) as a one-directional widening: integer → floating
// is §5.1-implicit, accepted; the reverse (a `decimal` arg against an
// `int` parameter) is NOT in the table (would lose information, requires an
// explicit cast) and remains rejected, so the class disambiguation #3760
// introduced for `this(...)` chain-target selection still holds for the
// float → int shape.
//
// T1 — the issue's own repro, observed at runtime: with the fix the
// validating ctor body runs, Width/Height are written, and the rectangle
// fields read as 3 and 4. Pre-fix, the ctor body is silently skipped and
// the local is zero-initialised, so Width == 0 and the assertion fails.
// T2 — int literal against a `double` parameter: same §5.1 implicit
// conversion, the same one-directional widening admits it.
// T3 — int literal against a `float` parameter: the third row of the
// §5.1 table, included so the fix is not decimal-specific.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — the issue's own repro at runtime. Pre-fix: zero-fill → sum = 0m
// (not 7m) → return 0. Post-fix: validating ctor body runs → Width = 3,
// Height = 4 → sum = 7m → return 1.
// ===========================================================================
BEZEL_TEST(codegen_ctor_for_args_int_to_decimal_4046,
           int_literal_against_decimal_ctor_param_runs_body_spec_5_1) {
    BEZEL_EXPECT_EQ(
        run("module M; "
            "record Rectangle4046 "
            "  public property Width: decimal; "
            "  public property Height: decimal; "
            "  public constructor(width: decimal, height: decimal) "
            "    Width := width; "
            "    Height := height; "
            "  end constructor; "
            "end Rectangle4046; "
            "class C public static method Main(): int "
            "  var a := new Rectangle4046(3, 4); "
            "  var s: decimal := a.Width + a.Height; "
            "  if s = 7m then return 1; else return 0; end; "
            "end Main; end C;"),
        1);
}

// ===========================================================================
// T2 — int literal against a `double` parameter. §5.1 table row "int:
// long, float, double, decimal" — the same one-directional widening.
// ===========================================================================
BEZEL_TEST(codegen_ctor_for_args_int_to_decimal_4046,
           int_literal_against_double_ctor_param_spec_5_1) {
    BEZEL_EXPECT_EQ(
        run("module M; "
            "record Boxed4046 "
            "  public property V: double; "
            "  public constructor(v: double) V := v; end constructor; "
            "end Boxed4046; "
            "class C public static method Main(): int "
            "  var b := new Boxed4046(5); "
            "  if b.V = 5.0 then return 1; else return 0; end; "
            "end Main; end C;"),
        1);
}

// ===========================================================================
// T3 — int literal against a `float` parameter. §5.1 table row "int:
// long, float, double, decimal" — the third family.
// ===========================================================================
BEZEL_TEST(codegen_ctor_for_args_int_to_decimal_4046,
           int_literal_against_float_ctor_param_spec_5_1) {
    BEZEL_EXPECT_EQ(
        run("module M; "
            "record FloatBox4046 "
            "  public property V: float; "
            "  public constructor(v: float) V := v; end constructor; "
            "end FloatBox4046; "
            "class C public static method Main(): int "
            "  var b := new FloatBox4046(7); "
            "  if b.V = 7.0 then return 1; else return 0; end; "
            "end Main; end C;"),
        1);
}
