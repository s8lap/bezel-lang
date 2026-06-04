// Implicit numeric widening (§5.1, §6.2) — issue #408.
//
// §5.1 admits `int → long, float, double, decimal` as implicit conversions
// ("no programmer-visible information is lost"). §6.2 mixed-type arithmetic:
// "the smaller-range operand is implicitly promoted to the larger type using
// §5.1. The result has the larger type." §22.6 overload binding treats a §5.1
// conversion as an admissible (Convert) match, so an `int` argument binds to a
// `double` parameter via widening.
//
// Before this fix three faults were observed:
//   [15] `var f: float := n;` (n: int) crashed the compiler ("Cannot select")
//        and silently produced wrong values: the int was stored into the FP
//        slot without an SIToFP conversion.
//   [16] `7 + 2.5` was statically typed `int` (the left operand) rather than
//        `double`, corrupting every type-driven consumer (e.g. interpolation)
//        — the spec result is the double `9.5`.
//   [17] an `int` argument bound to a `double` parameter (plain method or
//        operator overload) was passed as raw integer bits — no SIToFP — so
//        `Twice(5)` with `Twice(x: double)` returned 0 instead of 10.
//
// The harness compares int exit codes, so floating results are encoded as
// integers, e.g. `int(x * 10)` so the double 9.5 surfaces as 95.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

int runMod(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// §6.2 / §5.1 — mixed-type arithmetic: `int + double` promotes to `double`.
// The canonical spec case: `7 + 2.5` is the double `9.5` (encoded ×10 = 95).
// Before the fix the result was statically typed `int`, yielding garbage.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_numeric_conv, int_plus_double_is_9_5_spec_6_2) {
    // 7 + 2.5 == 9.5  →  int(9.5 * 10) == 95
    BEZEL_EXPECT_EQ(run("var r := 7 + 2.5; return int(r * 10.0);"), 95);
}

BEZEL_TEST(codegen_numeric_conv, int_plus_double_compares_equal_9_5) {
    BEZEL_EXPECT_EQ(run("var r := 7 + 2.5; return (1 if r = 9.5 else 0);"), 1);
}

// Symmetry: `double + int` (double on the left) must also be 9.5.
BEZEL_TEST(codegen_numeric_conv, double_plus_int_is_9_5_spec_6_2) {
    BEZEL_EXPECT_EQ(run("var r := 2.5 + 7; return int(r * 10.0);"), 95);
}

// §6.2 — the *static type* of `int + double` is `double` (the wider operand),
// not `int` (the left operand). This drives §22.6 overload resolution: `7 +
// 2.5` binds the `double` overload (an exact match), returning 2. Before the
// fix sema recorded the left operand's `int` type, selecting the wrong
// overload (returning 1). This is the int-observable manifestation of the
// mis-typing in finding [16].
BEZEL_TEST(codegen_numeric_conv, mixed_binop_static_type_is_double_spec_6_2) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; class C "
        "  public static method Tag(x: int): int => 1; "
        "  public static method Tag(x: double): int => 2; "
        "  public static method Main(): int "
        "    var r := 7 + 2.5; return Tag(r); "  // r: double → Tag(double) → 2
        "end Main; end C;"), 2);
}

// An int local mixed with a double literal (§6.2).
BEZEL_TEST(codegen_numeric_conv, int_local_plus_double_literal) {
    BEZEL_EXPECT_EQ(run("var n := 4; var r := n + 0.5; return int(r * 10.0);"),
                    45);
}

// §6.2: the result type is the wider operand — int/double multiply yields a
// double, so `5 * 0.5` is 2.5 (not integer 2).
BEZEL_TEST(codegen_numeric_conv, int_times_double_keeps_fraction) {
    BEZEL_EXPECT_EQ(run("var r := 5 * 0.5; return int(r * 10.0);"), 25);
}

// ---------------------------------------------------------------------------
// §5.1 — int assigned to a `float`/`double` local is widened (SIToFP) on the
// store, and the slot is sized to the declared type. Finding [15].
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_numeric_conv, int_init_double_local_spec_5_1) {
    // var d: double := n;  d must hold 7.0, not the bit pattern of 7.
    BEZEL_EXPECT_EQ(run("var n: int := 7; var d: double := n; "
                        "return int(d * 10.0);"), 70);
}

BEZEL_TEST(codegen_numeric_conv, int_init_float_local_spec_5_1) {
    // var f: float := n;  f must hold 7.0f.
    BEZEL_EXPECT_EQ(run("var n: int := 7; var f: float := n; "
                        "return int(f * 10.0);"), 70);
}

// The float local read at full f64 width: pass it to a `double` parameter.
// Before the fix the f32 slot held raw i32 bits and this returned 0.
BEZEL_TEST(codegen_numeric_conv, float_local_from_int_read_as_double) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; class C "
        "public static method Use(x: double): double => x; "
        "public static method Main(): int "
        "  var n: int := 7; var f: float := n; "
        "  var r := Use(f); return int(r * 10.0); "
        "end Main; end C;"), 70);
}

// §5.1 — int assigned (plain `:=`) to a double local is widened on the store.
BEZEL_TEST(codegen_numeric_conv, int_assigned_to_double_local) {
    BEZEL_EXPECT_EQ(run("var n: int := 7; var d := 0.0; d := n; "
                        "return int(d * 10.0);"), 70);
}

// ---------------------------------------------------------------------------
// §5.1 / §22.6 — an int argument bound to a `double` parameter is widened.
// Finding [17] (plain static method).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_numeric_conv, int_arg_to_double_param_method_spec_22_6) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; class C "
        "public static method Twice(x: double): double => x * 2.0; "
        "public static method Main(): int "
        "  var n: int := 5; var r := Twice(n); return int(r); "
        "end Main; end C;"), 10);
}

// An int literal bound directly to a double parameter.
BEZEL_TEST(codegen_numeric_conv, int_literal_arg_to_double_param) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; class C "
        "public static method Half(x: double): double => x / 2.0; "
        "public static method Main(): int "
        "  var r := Half(5); return int(r * 10.0); "
        "end Main; end C;"), 25);
}

// ---------------------------------------------------------------------------
// §5.1 / §22.6 / §25 — an int operand bound to a `double` operator-overload
// parameter is widened. Finding [17] (operator overload).
// `v * 2` scales v.X (==3) by the double 2.0 → int(3 * 2.0) == 6.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_numeric_conv, int_arg_to_double_operator_overload_spec_25) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator *(a: V, s: double): int => "
        "    int(a.X * s); "
        "end V; "
        "class C public static method Main(): int "
        "  using var a := new V(); a.X := 3; "
        "  return a * 2; "  // 3 * 2.0 == 6
        "end Main; end C;"), 6);
}

// ---------------------------------------------------------------------------
// §5.1 — an int initializer for a `double` instance field is widened.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_numeric_conv, int_init_double_field) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class P "
        "  public x: double := 5; "  // int literal initializer for double field
        "  public constructor() end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  return int(p.x * 10.0); "  // 50
        "end Main; end C;"), 50);
}

// ---------------------------------------------------------------------------
// §5.1 — mixed int/double comparison: the int operand is promoted before the
// ordered FP compare (§6.2). `7 > 6.5` is true; `7 < 6.5` is false.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_numeric_conv, mixed_int_double_comparison) {
    BEZEL_EXPECT_EQ(run("var n := 7; return (1 if n > 6.5 else 0);"), 1);
}

BEZEL_TEST(codegen_numeric_conv, mixed_double_int_comparison_false) {
    BEZEL_EXPECT_EQ(run("var n := 7; return (1 if 6.5 > n else 0);"), 0);
}

// ---------------------------------------------------------------------------
// Regression guard — pure integer arithmetic must remain integer (no spurious
// FP promotion). `7 + 2` is the int 9; integer `/` truncates (`7 / 2` == 3).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_numeric_conv, pure_int_addition_unchanged) {
    BEZEL_EXPECT_EQ(run("var r := 7 + 2; return r;"), 9);
}

BEZEL_TEST(codegen_numeric_conv, pure_int_division_truncates) {
    BEZEL_EXPECT_EQ(run("var r := 7 / 2; return r;"), 3);
}

// Regression: int argument to an int parameter is unchanged (no SIToFP).
BEZEL_TEST(codegen_numeric_conv, int_arg_to_int_param_unchanged) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; class C "
        "public static method Inc(x: int): int => x + 1; "
        "public static method Main(): int return Inc(41); "
        "end Main; end C;"), 42);
}
