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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

int runMod(const std::string& src) {
    auto r = runAot(src);
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
    BEZEL_EXPECT_EQ(run("var r := 7 + 2.5; return (if r = 9.5 then 1 else 0);"), 1);
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
    BEZEL_EXPECT_EQ(run("var n := 7; return (if n > 6.5 then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_numeric_conv, mixed_double_int_comparison_false) {
    BEZEL_EXPECT_EQ(run("var n := 7; return (if 6.5 > n then 1 else 0);"), 0);
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

// ===========================================================================
// §5.1 — unsigned-source int→FP widening uses UIToFP, not SIToFP (issue #410,
// deferred from #408). For an unsigned source (uint/ulong/ushort/byte/char),
// the integer→floating-point widening must zero-interpret the source bits.
// SIToFP on a high-bit-set unsigned value would sign-interpret it as negative.
//
// The widening is exercised at the field-initializer store site (lowerNew →
// coerceNumericToType), which threads the initializer's source signedness.
// ===========================================================================

// A `uint` value with the high bit set (3_000_000_000 > int.MaxValue) widened
// to a `double` field. UIToFP yields +3e9; the old SIToFP yielded ≈ −1.29e9.
BEZEL_TEST(codegen_numeric_conv, uint_init_double_field_uitofp_spec_5_1) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class P "
        "  public d: double := 3000000000u; "  // uint → double field
        "  public constructor() end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  return (if p.d > 2000000000.0 then 1 else 0); "  // +3e9 > 2e9 → 1
        "end Main; end C;"), 1);
}

// §5.1 — unsigned integer widening uses ZExt, not SExt. A `uint` value with
// the high bit set (3_000_000_000) widened to a `long` field must zero-extend
// to the positive +3e9; SExt would sign-interpret the i32 as negative.
BEZEL_TEST(codegen_numeric_conv, uint_init_long_field_zext_spec_5_1) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class P "
        "  public n: long := 3000000000u; "  // uint → long field (ZExt)
        "  public constructor() end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  return (if p.n > 2000000000L then 1 else 0); "  // +3e9 > 2e9 → 1
        "end Main; end C;"), 1);
}

// Regression: a SIGNED negative int initializer for a double field must still
// sign-extend correctly (SIToFP) — the unsigned refinement must not flip it.
BEZEL_TEST(codegen_numeric_conv, negative_int_init_double_field_sitofp) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class P "
        "  public d: double := -7; "  // signed int → double field
        "  public constructor() end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  return (if p.d < 0.0 then 1 else 0); "  // −7.0 < 0 → 1
        "end Main; end C;"), 1);
}

// Regression: a signed positive int initializer for a double field still
// widens to the right magnitude (the common #408 path).
BEZEL_TEST(codegen_numeric_conv, positive_int_init_double_field_value) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class P "
        "  public d: double := 5; "
        "  public constructor() end constructor; "
        "end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  return int(p.d * 10.0); "  // 50
        "end Main; end C;"), 50);
}

// ===========================================================================
// §5.1 — unsigned-source widening at the remaining store sites (issue #421,
// deferred from #410). The field-initializer site already threaded source
// signedness; #421 extends the same to the LOCAL declaration, plain
// ASSIGNMENT, and method-ARGUMENT binding sites. Before #421 each used the
// signed-default 2-arg coerceNumericToType wrapper, so a high-bit-set uint
// widened via SIToFP/SExt — reading negative instead of its true magnitude.
//
// A uint of 3_000_000_000 (> int.MaxValue) widened to a double:
//   * UIToFP (correct): +3e9   * SIToFP (old bug): ≈ −1.29e9
// ===========================================================================

// Local declaration site (LlvmBackend_Setup): `var d: double := someUint`.
BEZEL_TEST(codegen_numeric_conv, uint_init_double_local_uitofp_421) {
    BEZEL_EXPECT_EQ(run(
        "var u: uint := 3000000000u; var d: double := u; "
        "return (if d > 2000000000.0 then 1 else 0);"), 1);  // +3e9 > 2e9 → 1
}

// Local declaration site, widen uint → long (ZExt, not SExt).
BEZEL_TEST(codegen_numeric_conv, uint_init_long_local_zext_421) {
    BEZEL_EXPECT_EQ(run(
        "var u: uint := 3000000000u; var n: long := u; "
        "return (if n > 2000000000L then 1 else 0);"), 1);  // +3e9 > 2e9 → 1
}

// Plain assignment site (LlvmBackend_Stmts): `d := someUint`.
BEZEL_TEST(codegen_numeric_conv, uint_assigned_to_double_local_uitofp_421) {
    BEZEL_EXPECT_EQ(run(
        "var u: uint := 3000000000u; var d := 0.0; d := u; "
        "return (if d > 2000000000.0 then 1 else 0);"), 1);  // +3e9 > 2e9 → 1
}

// Method-argument binding site (LlvmBackend_Calls): a uint argument bound to a
// double parameter widens via UIToFP. Use(u) returns u; positive +3e9 > 2e9.
BEZEL_TEST(codegen_numeric_conv, uint_arg_to_double_param_uitofp_421) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; class C "
        "public static method Use(x: double): double => x; "
        "public static method Main(): int "
        "  var u: uint := 3000000000u; var r := Use(u); "
        "  return (if r > 2000000000.0 then 1 else 0); "  // +3e9 > 2e9 → 1
        "end Main; end C;"), 1);
}

// Regression at the new sites: a SIGNED negative source must still sign-extend
// (the unsigned refinement must not flip a signed value).
BEZEL_TEST(codegen_numeric_conv, negative_int_assigned_to_double_local_sitofp_421) {
    BEZEL_EXPECT_EQ(run(
        "var n: int := -7; var d := 0.0; d := n; "
        "return (if d < 0.0 then 1 else 0);"), 1);  // −7.0 < 0 → 1
}

// ---------------------------------------------------------------------------
// §5.2 (issue #739, WR6/WR7/WR8): "Floating-point to integer truncates toward
// zero; NaN, ±Infinity, and out-of-range values raise OverflowException." A
// bare fptosi/fptoui used to be emitted with no guard, so these three cases
// silently produced the integer-indefinite value (0x80000000 on x86) instead
// of raising. The conversion now guards an in-range predicate and raises
// OverflowException; valid sources still truncate toward zero. Each test
// catches the exception and returns a sentinel so the int harness observes it.
// ---------------------------------------------------------------------------

// In-range truncation toward zero is unchanged: 3.7 → 3, −3.7 → −3.
BEZEL_TEST(codegen_numeric_conv, float_to_int_truncates_toward_zero_739) {
    BEZEL_EXPECT_EQ(run(
        "var a: double := 3.7; var n: double := -3.7; "
        "return int(a) * 10 + (0 - int(n));"), 33);  // 30 + 3
}

// NaN → int raises OverflowException (§5.2).
BEZEL_TEST(codegen_numeric_conv, double_nan_to_int_overflows_739) {
    BEZEL_EXPECT_EQ(run(
        "var a: double := double.NaN; "
        "try return int(a); catch OverflowException => return 7; end; return 1;"),
        7);
}

// +Infinity → int raises OverflowException (§5.2).
BEZEL_TEST(codegen_numeric_conv, double_pos_inf_to_int_overflows_739) {
    BEZEL_EXPECT_EQ(run(
        "var a: double := double.PositiveInfinity; "
        "try return int(a); catch OverflowException => return 7; end; return 1;"),
        7);
}

// −Infinity → int raises OverflowException (§5.2).
BEZEL_TEST(codegen_numeric_conv, double_neg_inf_to_int_overflows_739) {
    BEZEL_EXPECT_EQ(run(
        "var a: double := double.NegativeInfinity; "
        "try return int(a); catch OverflowException => return 7; end; return 1;"),
        7);
}

// A finite value well past int.MaxValue raises OverflowException (§5.2).
BEZEL_TEST(codegen_numeric_conv, double_too_large_to_int_overflows_739) {
    BEZEL_EXPECT_EQ(run(
        "var a: double := 1e20; "
        "try return int(a); catch OverflowException => return 7; end; return 1;"),
        7);
}

// The upper bound is strict at the truncation boundary: int.MaxValue exactly
// (2147483647.0) is in range; int.MaxValue + 1 (2147483648.0) overflows.
BEZEL_TEST(codegen_numeric_conv, double_int_max_boundary_739) {
    BEZEL_EXPECT_EQ(run(
        "var ok: double := 2147483647.0; "
        "var over: double := 2147483648.0; "
        "var v := int(ok); "  // valid: int.MaxValue
        "try var w := int(over); return 1; "
        "catch OverflowException => return (if v = 2147483647 then 1 else 2); end;"),
        1);
}

// byte (unsigned 8-bit): 200.0 fits, 300.0 overflows; negative also overflows.
// #4724 (§5.2 constant-operand rule, v0.307): route the overflow operand
// through a `double` local so the runtime conversion throws
// `OverflowException` (the test's intent), not a compile-time reject.
BEZEL_TEST(codegen_numeric_conv, double_to_byte_range_739) {
    BEZEL_EXPECT_EQ(run(
        "var ok: double := 200.0; var v := byte(ok); "
        "var overflow: double := 300.0; "
        "try var w: byte := byte(overflow); return 1; "
        "catch OverflowException => return int(v); end;"),  // 200
        200);
}

// long (signed 64-bit): a value below long.MaxValue casts; 1e19 overflows.
// #4724 (§5.2 constant-operand rule, v0.307): route the overflow operand
// through a `double` local so the runtime conversion throws.
BEZEL_TEST(codegen_numeric_conv, double_to_long_range_739) {
    BEZEL_EXPECT_EQ(run(
        "var a: double := 9.0e18; var b: long := long(a); "
        "var inrange := (if b > 8999999000000000000L then 1 else 0); "
        "var overflow: double := 1.0e19; "
        "try var c: long := long(overflow); return 1; "
        "catch OverflowException => return inrange; end;"),  // 1
        1);
}

// ── #1133 / §5.2: integer narrowing across a signedness mismatch ─────────────
// `int(ulong.MaxValue)` must raise OverflowException, not silently yield -1.
// The old round-trip check re-extended the truncated value with the DEST's
// signedness and compared bit patterns — for an unsigned source this conflated
// the huge unsigned value with a negative dest value sharing low bits.

// ulong.MaxValue → int overflows (was: silently returned -1).
BEZEL_TEST(codegen_numeric_conv, ulong_max_to_int_overflows_1133) {
    BEZEL_EXPECT_EQ(run(
        "var big: ulong := 18446744073709551615; "
        "try var r: int := int(big); return 0; "          // no throw = bug
        "catch OverflowException => return 7; end;"),      // caught = correct
        7);
}

// An unsigned value just past int.MaxValue (whose low 32 bits set the dest sign
// bit) also overflows — the case a zero-extend round-trip would miss.
BEZEL_TEST(codegen_numeric_conv, ulong_intmax_plus_one_to_int_overflows_1133) {
    BEZEL_EXPECT_EQ(run(
        "var u: ulong := 2147483648; "                     // int.MaxValue + 1
        "try var r: int := int(u); return 0; "
        "catch OverflowException => return 7; end;"),
        7);
}

// A fitting unsigned→signed narrowing still succeeds (no spurious throw):
// ulong int.MaxValue → int is exactly in range.
BEZEL_TEST(codegen_numeric_conv, ulong_intmax_to_int_in_range_1133) {
    BEZEL_EXPECT_EQ(run(
        "var u: ulong := 2147483647; return int(u) - 2147483605;"),  // 42
        42);
}

// A negative signed value → unsigned narrowing overflows (cannot fit unsigned).
BEZEL_TEST(codegen_numeric_conv, negative_long_to_uint_overflows_1133) {
    BEZEL_EXPECT_EQ(run(
        "var n: long := -1L; "
        "try var r: uint := uint(n); return 0; "
        "catch OverflowException => return 7; end;"),
        7);
}

// Signed→signed narrowing is unaffected: in-range keeps its value, out-of-range
// still throws.
BEZEL_TEST(codegen_numeric_conv, long_to_int_signed_unaffected_1133) {
    BEZEL_EXPECT_EQ(run(
        "var ok := 0; "
        "var a: long := -5L; if int(a) = -5 then ok := ok + 1; end; "        // in range
        // The out-of-range operand is bound to a LOCAL first, deliberately.
        // §5.2 (v0.300) rejects a faulting COMPILE-TIME-CONSTANT operand at
        // the conversion site rather than deferring it to a runtime fault, so
        // the inline `int(5000000000L)` this row used to spell is now a
        // compile error and could never reach the `catch`. Binding through a
        // local keeps the operand non-constant, so the conversion still raises
        // `OverflowException` at runtime — which is the behaviour this row
        // exists to pin. Measured: exit 6, the catch arm taken.
        "var big: long := 5000000000L; "
        "try var b: int := int(big); catch OverflowException => ok := ok + 6; end; "
        "return ok;"),                                     // 1 + 6 = 7
        7);
}

// NOTE: an UNCAUGHT NaN/Inf/out-of-range→int conversion propagates via the
// runtime's unhandled-exception path (prints "Unhandled exception", exit 70).
// That path calls exit() in-process, so it cannot be exercised under the JIT
// harness without terminating the test runner; it is verified by AOT-compiling
// and running a standalone binary instead (see the #739 lane notes).
