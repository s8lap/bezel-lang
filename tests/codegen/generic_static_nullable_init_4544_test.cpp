// Codegen fix #4544 — a static field of NULLABLE VALUE type with a
// compile-time initializer, declared on a GENERIC class and read from that
// same generic class's own method, reads back as `null`: the initializer
// never reaches the per-instantiation storage, so `SF!` raises
// NullReferenceException instead of yielding the initialized value.
//
// §7.2:  "A value type wrapped in `?` is an optional: it either holds a
//        value or `null`." A nullable VALUE type lowers to a two-field
//        tagged aggregate `{i8 hasValue; T value}` (contrast a nullable
//        REFERENCE type, which is a bare possibly-null pointer).
// §7.8:  "`x!` asserts non-nullness. If `x` is null at runtime,
//        NullReferenceException is raised." The `!` here is over a value
//        the field's own initializer set — it must never fire.
// §22.12: "Each distinct instantiation has its own static state" — and rule
//        2 form (iii) admits a mutable primitive/nullable-primitive static
//        with a declaration-site warning (expected, not a symptom); a
//        `static readonly` of the same declared type is unrestricted
//        (rule 3).
// §22.13: "Within a single type, static fields are initialized in textual
//        declaration order."
//
// Root cause: genericInstSfinitFn (LlvmBackend_Setup.cpp) is the ONLY
// runtime-store path that ever reaches a LOCALLY-declared generic class's
// per-instantiation static global — see that function's own block comment:
// the template-level #709 loop (emitStaticConstructorBody) runs BEFORE any
// per-instantiation global exists (those are created lazily, during method-
// body emission), so for a local generic its genericInstStaticGlobals_ walk
// finds nothing to store into. genericInstSfinitFn's field-init loop lowered
// the initializer expression and, unlike its sibling in
// emitStaticConstructorBody (fix #2903), stored the BARE lowerExpr result
// straight into the per-instantiation global with no §7.2 tagged-slot wrap.
// For `int? := 105` this is a plain `i32` versus the global's `{i8, i32}`
// aggregate type; the very next line's #1116 type-match guard
// (`val->getType() != instGV->getValueType()`) then silently skipped the
// store, leaving the global at its zero-initialized `{0, 0}` — `null`.
//
// The fix adds the same coerceInitializerToSlot / coerceReturnToNullableValue
// pair emitStaticConstructorBody already applies (fix #2903, §7.2), in the
// same spot, before the #1116 type-match guard.
//
// Measured axes (establishing what is load-bearing, per the issue):
//   generic + nullable int?    -> FAILS pre-fix (NullReferenceException, 70)
//   non-generic + nullable int? -> already correct pre-fix (#2903 covers it)
//   generic + non-nullable int  -> already correct pre-fix (i32 == i32, no
//                                   coercion was ever needed)
//   non-generic + non-nullable int -> already correct (baseline)
// So the declaring class being GENERIC, combined with the field being a
// NULLABLE VALUE type, is exactly the gap; this file's regression tests
// below pin the fix on that combination while guarding the three shapes
// that were never broken.
//
// Spec references:
//   §7.2   — nullable value types / the tagged-slot representation.
//   §7.8   — the null-forgiving operator.
//   §22.12 — per-instantiation static state; rule 2 form (iii) / rule 3.
//   §22.13 — static-field initialization order.
//
// Related: #2903 (the non-generic sibling fix this mirrors), #709/#1513
// (the per-instantiation static-store machinery this loop belongs to).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── Core repro: mutable nullable-int static on a generic class ──────────────

// §7.2 / §7.8 / §22.12 / §22.13 / #4544: the exact issue repro. `PK<int>.SF`
// must read 105, not null — the initializer must reach the per-instantiation
// storage in the tagged-slot shape.
BEZEL_TEST(codegen_generic_static_nullable_init_4544,
           mutable_nullable_int_static_reads_initializer_spec_7_2_7_8) {
    BEZEL_EXPECT_EQ(run(
        "module SK1; "
        "public class PK<T> "
        "  public static SF: int? := 105; "
        "  public constructor() end constructor; "
        "  public method Get(): int return PK<T>.SF!; end Get; "
        "end PK; "
        "class MK public static method Main(): int "
        "  using var c := new PK<int>(); "
        "  return c.Get(); "
        "end Main; end MK;"), 105);
}

// §22.12 / §22.13 / #4544: a SECOND, independent instantiation must also see
// its own initialized value — per-instantiation static state, per §22.12.
BEZEL_TEST(codegen_generic_static_nullable_init_4544,
           mutable_nullable_int_static_second_instantiation_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module SK1b; "
        "public class PK<T> "
        "  public static SF: int? := 105; "
        "  public constructor() end constructor; "
        "  public method Get(): int return PK<T>.SF!; end Get; "
        "end PK; "
        "class MK public static method Main(): int "
        "  using var c := new PK<string>(); "
        "  return c.Get(); "
        "end Main; end MK;"), 105);
}

// ── static readonly with a non-constant initializer takes the same path ─────

// §7.2 / §22.12 rule 3 / §22.13 / #4544: `static readonly SF: int? :=
// Helper.Compute();` is NOT baked (only a constant-LITERAL readonly bakes,
// isBakedStaticFieldInit) so it runs through the exact same runtime-store
// loop as the mutable case above. This pins that the fix is not
// mutability-specific.
BEZEL_TEST(codegen_generic_static_nullable_init_4544,
           readonly_nullable_int_static_computed_init_spec_22_12_rule3) {
    BEZEL_EXPECT_EQ(run(
        "module SK2; "
        "class Helper4544 "
        "  public static method Compute(): int return 105; end Compute; "
        "end Helper4544; "
        "public class PK<T> "
        "  public static readonly SF: int? := Helper4544.Compute(); "
        "  public constructor() end constructor; "
        "  public method Get(): int return PK<T>.SF!; end Get; "
        "end PK; "
        "class MK public static method Main(): int "
        "  using var c := new PK<int>(); "
        "  return c.Get(); "
        "end Main; end MK;"), 105);
}

// ── Regression guards: the three shapes that were never broken ──────────────

// §22.13 / #4544 / #2903 regression guard: a NON-generic class's nullable-int
// static must remain correct (the sibling fix #2903 already covers this
// path; #4544 must not disturb it).
BEZEL_TEST(codegen_generic_static_nullable_init_4544,
           nongeneric_nullable_int_static_regression_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module SK3; "
        "public class PK "
        "  public static SF: int? := 105; "
        "  public constructor() end constructor; "
        "  public method Get(): int return PK.SF!; end Get; "
        "end PK; "
        "class MK public static method Main(): int "
        "  using var c := new PK(); "
        "  return c.Get(); "
        "end Main; end MK;"), 105);
}

// §22.12 / §22.13 / #4544 regression guard: the NON-nullable twin on a
// generic class (`int` rather than `int?`) was already correct pre-fix
// (lowerExpr's bare i32 already equals the slot's i32 — no coercion was ever
// needed) and must remain correct: coerceInitializerToSlot is a documented
// no-op when `v->getType() == want`.
BEZEL_TEST(codegen_generic_static_nullable_init_4544,
           generic_nonnullable_int_static_regression_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module SK4; "
        "public class PK<T> "
        "  public static SF: int := 105; "
        "  public constructor() end constructor; "
        "  public method Get(): int return PK<T>.SF; end Get; "
        "end PK; "
        "class MK public static method Main(): int "
        "  using var c := new PK<int>(); "
        "  return c.Get(); "
        "end Main; end MK;"), 105);
}

// §22.12 / §22.13 / #4544 regression guard: the existing #709 non-constant
// generic-static suite (a DIFFERENT runtime-store path — the template-level
// loop, reached when the instantiation is first touched from OUTSIDE the
// generic class's own method body) must be unaffected by this fix, which
// only touches genericInstSfinitFn's loop. Mirrors
// generic_static_nonconst_init_709_test.cpp's own repro shape.
BEZEL_TEST(codegen_generic_static_nullable_init_4544,
           external_touch_nonconst_generic_static_regression_spec_709) {
    BEZEL_EXPECT_EQ(run(
        "module SK5; "
        "class Helper4544b "
        "  public static method Compute(): int return 7; end Compute; "
        "end Helper4544b; "
        "class Registry4544<T> "
        "  public static count: int := Helper4544b.Compute(); "
        "end Registry4544; "
        "class Program4544 "
        "  public static method Main(): int "
        "    return Registry4544<int>.count; "
        "  end Main; "
        "end Program4544;"), 7);
}
