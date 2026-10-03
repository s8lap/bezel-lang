// Regression tests for #5337 (spec §5.2 + §5.3, v0.313): the integer-target
// sibling of #5330.
//
// #5330 fixed the §5.3 lifted cast `T?(x)` for `float?`/`double?` targets
// (LlvmBackend_Exprs.cpp, the NullCondCallExpr branch): a `decimal` payload is
// the 16-byte `%bz.decimal` STRUCT, so lowerPrimCastScalar classified it as
// neither integer nor floating point, fell into its int->FP arm and emitted
// `sitofp %bz.decimal ... to float`. #5337 is the same defect with an INTEGER
// target: lowerPrimCastScalar's int->int arm emitted
//
//   SExt only operates on integer
//     %4 = sext %bz.decimal %nv.val to i32
//   bezelc: codegen error: IR verification failed
//
// and the UNSIGNED targets failed even earlier, in lowerPrimCastScalar's
// signed-widening guard (#1159), which built
//
//   Both operands to ICmp instruction are not of the same type!
//     %s2u.widen.oor = icmp slt %bz.decimal %nv.val, i1 false
//
// So `int?(d)`, `long?(d)`, `short?(d)`, `uint?(d)`, `ulong?(d)`, `byte?(d)`,
// `ushort?(d)` and `sbyte?(d)` on a `decimal?` ALL aborted the compiler.
// MEASURED pre-fix on r23-c28 = 67bab85c, this file's exact repros.
//
// §5.3, v0.313: "`T?(x)` is admitted exactly when the corresponding
// non-nullable conversion `T(v)` is admitted ... a present value `v` converts
// by `T(v)` under that conversion's own semantics, including its runtime
// checks (`OverflowException` on a non-fitting present value ...)". So the
// lifted path must convert through the SAME §5.2 conversion the explicit
// `T(d)` does — `bz_decimal_to_int64` / `bz_decimal_to_uint64` plus the
// range guards. The fix routes both through one shared helper
// (`Emitter::lowerDecimalToInteger`), which is what makes the two paths agree
// by construction rather than by coincidence.
//
// §5.2, v0.313: "Floating-point to integer truncates toward zero; `NaN`,
// `±Infinity`, and out-of-range values raise `OverflowException`." A
// `decimal` has neither non-finite value (§4.4), so the bound check is the
// whole story: 42.7m is 42, -42.7m is -42, and 40000m does not fit a `short`.
//
// EXPECTED VALUES are computed independently in python3 with exact
// `fractions.Fraction` arithmetic (`int(Fraction(Decimal(v)))` truncates
// toward zero), never by the compiler.
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

// A 200-iteration loop over the lifted cast `T?(n)` on a `decimal?`, counting
// the iterations whose unwrapped value equals `expectedLit`. `target` is the
// integer primitive keyword, `expectedLit` the Bezel literal of the §5.2
// truncated value.
std::string liftedLoop(const std::string& target, const std::string& srcLit,
                       const std::string& expectedLit) {
    return main0("var n: decimal? := " + srcLit + "; var ok := 0; "
                 "var i := 0; while i < 200 do "
                 "var v := " + target + "?(n); "
                 "if v <> null then if v! = " + expectedLit +
                 " then ok := ok + 1; end; end; "
                 "i := i + 1; end; Console.WriteLine(ok); ");
}

// The same lifted cast, printing the value directly.
std::string liftedPrint(const std::string& target, const std::string& srcLit) {
    return main0("var n: decimal? := " + srcLit + "; var v := " + target +
                 "?(n); Console.WriteLine(v!); "
                 "Console.WriteLine(v! = v!); ");
}

// A lifted cast whose value does not fit the target, wrapped in a catch so
// the §5.2 OverflowException is the OBSERVED outcome rather than a crash.
std::string liftedOverflow(const std::string& target,
                           const std::string& srcLit,
                           const std::string& catchBody) {
    return main0("var n: decimal? := " + srcLit +
                 "; try var v := " + target + "?(n); "
                 "if v <> null then Console.WriteLine(v!); end; "
                 "catch OverflowException => " + catchBody + " end try; ");
}

}  // namespace

// ── USER rows: each one FAILED TO COMPILE before the fix ────────────────────

// The issue's own repro: `int?(n)` on a `decimal?`.
// Pre-fix: `SExt only operates on integer / %4 = sext %bz.decimal %nv.val to
// i32` — IR verification failed.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_int_truncates) {
    expectOut(liftedLoop("int", "42.7m", "42"), "200");
    expectOut(liftedPrint("int", "42.7m"), "42\nTrue");
}

// `long?(n)` — the 64-bit target, the other repro named in #5337. Its
// pre-fix diagnostic differs only in the destination width
// (`sext %bz.decimal %nv.val to i64`).
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_long_truncates) {
    expectOut(liftedLoop("long", "42.7m", "42"), "200");
    expectOut(liftedPrint("long", "42.7m"), "42\nTrue");
}

// `short?(n)` — a NARROWER-than-int32 target, so the fix must also emit the
// §5.2 sub-range guard (trunc + roundtrip check) that `int?` does not need.
// Pre-fix: `sext %bz.decimal %nv.val to i16`.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_short_truncates) {
    expectOut(liftedLoop("short", "42.7m", "42"), "200");
    expectOut(liftedPrint("short", "42.7m"), "42\nTrue");
}

// The UNSIGNED targets failed EARLIER than the signed ones, in
// lowerPrimCastScalar's signed-widening guard rather than its int->int arm:
//   Both operands to ICmp instruction are not of the same type!
//     %s2u.widen.oor = icmp slt %bz.decimal %nv.val, i1 false
// so they are a distinct pre-fix diagnostic and get their own row.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted,
           user_nullable_unsigned_targets_truncate) {
    expectOut(liftedLoop("uint", "42.7m", "42"), "200");
    expectOut(liftedLoop("ulong", "42.7m", "42"), "200");
    expectOut(liftedLoop("byte", "42.7m", "42"), "200");
    expectOut(liftedLoop("ushort", "42.7m", "42"), "200");
    expectOut(liftedLoop("sbyte", "42.7m", "42"), "200");
}

// Truncation is TOWARD ZERO, so the sign of the source carries to the result
// (-42.7m is -42, not -43). A path that rounded to nearest, or that
// round-tripped through a float, would differ here.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_negative_truncates_toward_zero) {
    expectOut(liftedLoop("int", "-42.7m", "-42"), "200");
    expectOut(liftedLoop("long", "-42.7m", "-42"), "200");
}

// `ulong` of a value ABOVE int64.MaxValue. §5.2: the `ulong` target routes
// through `bz_decimal_to_uint64` (#4785), not the signed helper, so the top
// half of `ulong`'s domain is reachable. 12345678901234567890m is
// < uint64.MaxValue (18446744073709551615) and > int64.MaxValue, so the
// signed helper would reject it — the row pins that it does not.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_ulong_above_int64_max) {
    expectOut(liftedPrint("ulong", "12345678901234567890.0m"),
              "12345678901234567890\nTrue");
}

// The same value into a `uint` — 12345678901234567890 > uint32.MaxValue, so
// §5.2's bound check must raise. This is the #4802 upper bound (UINT32_MAX,
// not INT32_MAX); had the fix reused the signed bound the value would have
// been rejected either way, but the paired `uint` row below pins the half of
// `uint`'s domain the signed bound would have thrown away.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_uint_above_uint32_max_raises) {
    expectOut(liftedOverflow("uint", "12345678901234567890.0m",
                             "Console.WriteLine(\"OVE-uint\");"),
              "OVE-uint");
}

// `uint` in the (INT32_MAX, UINT32_MAX] band — the domain #4802 restored.
// A `uint` signedness check (the pre-#4802 bound) rejects all of it. The
// expected literal carries the `U` suffix because 3000000000 does not fit
// `int` (§4.2's target-typed integer constant).
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_uint_above_int32_max_in_range) {
    expectOut(liftedLoop("uint", "3000000000.0m", "3000000000U"), "200");
}

// §5.2 / §5.3: a present value that does not fit raises OverflowException
// from the LIFTED cast, exactly as the non-nullable `T(v)` does — §5.3
// converts "under that conversion's own semantics, including its runtime
// checks". 40000m does not fit a `short` (max 32767); 1e19m does not fit an
// `int` (and not even an int64, so it is the `bz_decimal_to_int64` vtable
// path that raises, #872).
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_overflow_raises) {
    expectOut(liftedOverflow("short", "40000.0m", "Console.WriteLine(\"OVE-short\");"),
              "OVE-short");
    expectOut(liftedOverflow("int", "1e19m", "Console.WriteLine(\"OVE-int\");"),
              "OVE-int");
}

// §5.3: "A `null` operand yields `null`." The conversion sits behind the
// null flag, so a null source must NOT be converted (and must not raise) —
// the null arm of the emitted PHI is never observed.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nullable_null_stays_null) {
    expectOut(main0("var n: decimal? := null; "
                    "Console.WriteLine(int?(n) = null); "
                    "Console.WriteLine(long?(n) = null); "
                    "Console.WriteLine(short?(n) = null); "
                    "Console.WriteLine(ulong?(n) = null); "),
             "True\nTrue\nTrue\nTrue");
}

// The non-nullable SOURCE shape: `int?(d)` where `d: decimal`. There is no
// null flag to test, so hasV is a constant and the fix emits no branch (a
// PHI with a non-predecessor incoming block fails verification — the same
// constraint #5330 had to handle for `float?`/`double?`). Pre-fix:
// `sext %bz.decimal %3 to i32`.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, user_nonnullable_source_truncates) {
    expectOut(main0("var d: decimal := 42.7m; "
                    "var v := int?(d); Console.WriteLine(v!); "
                    "var w := long?(d); Console.WriteLine(w!); "
                    "var s := short?(d); Console.WriteLine(s!); "),
             "42\n42\n42");
}

// ── CONTROL rows: unchanged paths, green BEFORE AND AFTER the fix ──────────

// The non-nullable TARGET `int(d)` — lowerConversion's Case B, which the fix
// refactored into the shared helper. It was already correct (Case B predates
// #5330); this row pins that the refactor did not change it, and that §5.3's
// "`T?(x)` ... converts by `T(v)`" makes the two agree.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, ctrl_nonnullable_target_unchanged) {
    expectOut(main0("var d: decimal := 42.7m; "
                    "Console.WriteLine(int(d)); "
                    "Console.WriteLine(long(d)); "
                    "Console.WriteLine(short(d)); "
                    "Console.WriteLine(65.0m);"),
             "42\n42\n42\n65");
}

// The non-nullable target's OverflowException, pre-existing and unchanged:
// 40000m does not fit a `short`.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted,
           ctrl_nonnullable_target_overflow_unchanged) {
    expectOut(main0("var d: decimal := 40000.0m; "
                    "try Console.WriteLine(short(d)); "
                    "catch OverflowException => Console.WriteLine(\"OVE\"); end try;"),
             "OVE");
}

// `int?(n)` on an `int?` — lowerPrimCastScalar's identity arm, untouched (the
// new decimal branch is gated on a `%bz.decimal` payload).
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, ctrl_nullable_int_identity) {
    expectOut(main0("var n: int? := 42; var v := int?(n); "
                    "if v <> null then Console.WriteLine(v!); end; "
                    "Console.WriteLine(v = null);"),
             "42\nFalse");
}

// `int?(n)` on a `double?` — lowerPrimCastScalar's real FP->int arm (its own
// range check + fptosi), so the new decimal branch is not taken by an
// ordinary narrowed nullable cast. Pre-fix green, post-fix must stay green.
// The `long?` source is initialized through `long(42.9)` because a `double`
// literal is not implicitly convertible to a numeric `long?` (§5.1).
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, ctrl_nullable_double_to_int) {
    expectOut(main0("var n: double? := 42.9; var v := int?(n); "
                    "if v <> null then Console.WriteLine(v!); end; "
                    "var m: long? := long(42.9); var w := long?(m); "
                    "if w <> null then Console.WriteLine(w!); end;"),
             "42\n42");
}

// `long?(n)` widening a narrower integer source — lowerPrimCastScalar's
// int->int widening arm, untouched.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, ctrl_nullable_int_to_long) {
    expectOut(main0("var n: int? := 42; var v := long?(n); "
                    "if v <> null then Console.WriteLine(v!); end;"),
             "42");
}

// #5330's own paths — the float/double lifted cast this lane's fix sits
// beside. The `else if` reordering must not have disturbed them.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, ctrl_5330_float_double_lifted_unchanged) {
    expectOut(main0("var n: decimal? := 16777217.0000000000000000001m; "
                    "var f := float?(n); Console.WriteLine(f!); "
                    "var d := double?(n); Console.WriteLine(d!); "
                    "var z: decimal? := null; "
                    "Console.WriteLine(float?(z) = null); "
                    "Console.WriteLine(double?(z) = null);"),
             "16777218\n16777217\nTrue\nTrue");
}

// A generic instantiated at T = decimal returning `int` — lowerConversion's
// Case B reached through the type-parameter path #5330's shape-gate fix
// opened. Untouched by this lane, and it must stay green.
BEZEL_TEST(issue_5337_decimal_int_cast_lifted, ctrl_generic_decimal_to_int) {
    expectOut("module M; class C "
              "public static method G<T>(x: T): int return int(x); end G; "
              "public static method Main(): int "
              "Console.WriteLine(G<decimal>(42.7m)); return 0; end Main; end C;",
             "42");
}
