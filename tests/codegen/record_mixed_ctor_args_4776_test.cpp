// Regression test for issue #4776 — mixed positional+named record ctor args
// in `buildRecordValue` (§22.5, v0.307).
//
// THE BUG: `buildRecordValue`'s primary-ctor fast path (§22.5, v0.307)
// set `mappingOk = false` whenever any argument was named, then fell through
// to the explicit-constructor path ( LlvmBackend_Exprs2.cpp:10968).
// Since #4712 sema defers combined-form record `new` resolution to codegen,
// a mixed call such as `new P(1, Y := 2)` on a combined-form record reached
// this fast path and was incorrectly rejected. Even on a plain positional
// record, a mixed call was silently mishandled.
//
// THE FIX: when a positional arg appears in a mixed call, map it to the
// positional field at the same index — the same rule the all-positional arm
// uses. Named args continue to bind by name. The completeness check (every
// positional field receives an argument) guards against mismatched arity.
//
// Related fixes: #4712 (combined-form record deferral), #2434 (zero-fill
// fallback for unrecognised arg shapes).
//
// SPEC §22.5 (v0.307): named arguments may appear in any order after
// positional arguments. A positional argument fills the positional slot at
// its own argument index.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── COMBINED-FORM RECORD — mixed positional+named ──────────────────────────
//
// Primary repro: a record with positional fields AND a body (combined form,
// introduced by #4712) receives a mixed call. Sema defers it to codegen,
// which previously set `mappingOk = false` for any named arg and fell through.

BEZEL_TEST(record_mixed_ctor_args_4776,
           combined_form_record_new_P_1_Y_2_fields_not_zero) {
    // Combined form: positional fields + body property.
    // `new P(1, Y := 2)` — positional arg 1 fills X; named Y := 2 fills Y.
    // Expected: X=1, Y=2, Z=99 (body field keeps its initializer).
    auto r = runAot(
        "module M; "
        "record P(X: int, Y: int) public property Z: int := 99; end P; "
        "class C public static method Main(): int "
        "  var p := new P(1, Y := 2); "
        "  if p.X <> 1 then return 1; end; "
        "  if p.Y <> 2 then return 2; end; "
        "  if p.Z <> 99 then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── PLAIN POSITIONAL RECORD — mixed positional+named (regression) ─────────
//
// Same mixed spelling on a plain positional record (no body). Before the fix
// this also set `mappingOk = false` and fell through, triggering the #2434
// zero-fill warning and producing a record with all-zero fields.

BEZEL_TEST(record_mixed_ctor_args_4776,
           plain_positional_record_new_Q_10_Y_20_fields_not_zero) {
    // Plain positional: no body.
    // `new Q(10, Y := 20)` — positional 10 fills X; named Y := 20 fills Y.
    auto r = runAot(
        "module M; "
        "record Q(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var q := new Q(10, Y := 20); "
        "  if q.X <> 10 then return 1; end; "
        "  if q.Y <> 20 then return 2; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── ALL-POSITIONAL CONTROL — must not regress ──────────────────────────────
//
// `new P(1, 2)` — all positional. This arm has worked since #2464 and must
// not be broken by the mixed-form fix.

BEZEL_TEST(record_mixed_ctor_args_4776,
           all_positional_still_works_combined_form) {
    auto r = runAot(
        "module M; "
        "record P(X: int, Y: int) public property Z: int := 99; end P; "
        "class C public static method Main(): int "
        "  var p := new P(1, 2); "
        "  if p.X <> 1 then return 1; end; "
        "  if p.Y <> 2 then return 2; end; "
        "  if p.Z <> 99 then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(record_mixed_ctor_args_4776,
           all_positional_still_works_plain_positional) {
    auto r = runAot(
        "module M; "
        "record Q(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var q := new Q(10, 20); "
        "  if q.X <> 10 then return 1; end; "
        "  if q.Y <> 20 then return 2; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── ALL-NAMED CONTROL — must not regress ──────────────────────────────────
//
// `new P(X := 1, Y := 2)` — all named. This has worked since #540.

BEZEL_TEST(record_mixed_ctor_args_4776,
           all_named_still_works_combined_form) {
    auto r = runAot(
        "module M; "
        "record P(X: int, Y: int) public property Z: int := 99; end P; "
        "class C public static method Main(): int "
        "  var p := new P(X := 1, Y := 2); "
        "  if p.X <> 1 then return 1; end; "
        "  if p.Y <> 2 then return 2; end; "
        "  if p.Z <> 99 then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(record_mixed_ctor_args_4776,
           all_named_still_works_plain_positional) {
    auto r = runAot(
        "module M; "
        "record Q(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var q := new Q(X := 10, Y := 20); "
        "  if q.X <> 10 then return 1; end; "
        "  if q.Y <> 20 then return 2; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── #2434 REGRESSION GUARD — mixed form must NOT zero-fill ───────────────
//
// The pre-fix behaviour was: `mappingOk = false` → body path taken → slot
// zeroed at line 10409 and no field-init loop ran (no explicit ctor) → all
// fields zero. This row uses distinct non-zero values per field.

BEZEL_TEST(record_mixed_ctor_args_4776,
           mixed_form_combined_record_no_zero_fill_2434_guard) {
    auto r = runAot(
        "module M; "
        "record R(A: int, B: int, C: int) public property D: int := 42; end R; "
        "class C public static method Main(): int "
        "  var r := new R(7, B := 11, C := 13); "
        "  if r.A <> 7  then return r.A; end; "
        "  if r.B <> 11 then return r.B; end; "
        "  if r.C <> 13 then return r.C; end; "
        "  if r.D <> 42 then return r.D; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── NAMED ARG REORDERING IN MIXED CALL IS LEGAL (§22.5) ──────────────────
//
// `new P(Y := 2, X := 1)` — all named but in reverse order.

BEZEL_TEST(record_mixed_ctor_args_4776,
           all_named_reversed_order_combined_form) {
    auto r = runAot(
        "module M; "
        "record P(X: int, Y: int) public property Z: int := 99; end P; "
        "class C public static method Main(): int "
        "  var p := new P(Y := 2, X := 1); "
        "  if p.X <> 1 then return 1; end; "
        "  if p.Y <> 2 then return 2; end; "
        "  if p.Z <> 99 then return 3; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── THREE-POSITIONAL + ONE-NAMED MIXED ───────────────────────────────────

BEZEL_TEST(record_mixed_ctor_args_4776,
           mixed_three_positional_one_named) {
    auto r = runAot(
        "module M; "
        "record S(A: int, B: int, C: int, D: int); "
        "class C public static method Main(): int "
        "  var s := new S(1, 2, D := 4, C := 3); "
        "  if s.A <> 1 then return 1; end; "
        "  if s.B <> 2 then return 2; end; "
        "  if s.C <> 3 then return 3; end; "
        "  if s.D <> 4 then return 4; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}
