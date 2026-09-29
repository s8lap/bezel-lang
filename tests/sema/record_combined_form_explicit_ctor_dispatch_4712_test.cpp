// fix #4712 (§9.3 "Combined form" / §22.5 / §22.6, v0.307):
// `new T(...)` against a combined-form record (§9.3: positional fields +
// body with an explicit constructor of a DIFFERENT parameter list) ignored
// the body constructor and considered only the auto-generated primary. The
// diagnostic naming on the rejection named the primary's parameter list —
// `required parameter Y is not supplied (§22.5)` for a 1-arg call against
// `record Point2(X: int, Y: int)` + `constructor(scale: int) { X := scale;
// Y := scale; }` — even though `new Point2(5)` matches the body ctor and
// not the primary.
//
// fix-forward #5132 (§9.3 / §11.1 / §22.6, v0.308): the v0.308 ruling on
// the "same parameter list" clash sense (§11.3 / §22.6 overload identity;
// parameter names and defaults do not distinguish) means the body ctor's
// parameter list must DIFFER from the primary under §22.6 for the
// combined-form defer path to apply. Tests (3) and (4) pinned body ctors
// whose parameter lists differed from the primary only by name — under
// v0.307 those were admissible (the predicate included names); under
// v0.308 they clash at the declaration and the defer never runs. They
// are flipped here in this commit (rows (3) REJECT, (4) replaced with a
// truly different-arity/type shape that keeps the overload-selection
// oracle alive).
//
// Sema path: `ExprTC::checkNewExprArgs` (src/sema/ExprTypecheck_Calls.cpp
// at the `t->kind == ast::TypeDeclKind::Record` arm of
// `t->hasPositionalFields`) unconditionally called `checkArgsAgainstParams`
// on `td->positionalFields` for ANY record with primary fields, never
// inspecting `t->members` for a `ConstructorSymbol`. The class side's
// equivalent arm — same function, `t->kind` is the `else` — already
// explicitly handles combined-form by probing explicit ctors first
// (lines 14070-14093, the LOCAL defer; lines 14094-14229, the IMPORTED
// probe-then-fall-through-to-primary-fields shape, established by
// fix #3196/#3625). The record path never got the symmetric widening.
//
// THE FIX: at the record branch, when `t->members` carries at least one
// non-static `ConstructorSymbol` (a combined-form record per §9.3), defer
// constructor selection to codegen — `buildRecordValue`'s
// `ctorForArgs(t, args)` (src/codegen/LlvmBackend_Exprs2.cpp:10326) runs
// before the positional fast path and falls back to the positional fast
// path when no explicit ctor matches the call's arity. Symmetric with the
// LOCAL class combined-form defer one arm above.
//
// Pre-existing tests exercising combined-form records:
//   * tests/sema/record_combined_form_ctor_conflicts_4702_test.cpp
//     — narrowly avoided `new BodyCtor(args)` constructions, calling
//       via the primary's own shape, deliberately so until this fix;
//     tests (2) and (4) in that file now exercise (and pass with) the
//     fix on this file's primary-shape path.
//   * tests/lit/sema/sema_serialization_550__record_combined_form_
//     explicit_ctor_match_accepted_spec_9_8_v0182.bzl — FLIPPED in this
//     commit (was a declaration-side accept under v0.307; under v0.308
//     the same shape clashes and is now a REJECT control that names the
//     §9.3 diagnostic).
//
// FAIL-WITHOUT / PASS-WITH (proven mechanically): with the new
// `if (hasExplicitCtor) return;` line reverted (and the record branch
// restored to its current unconditional positional-field check), test (1)
// below reports the §22.5 "required parameter `Y` is not supplied" error
// the issue itself measures; restored, it constructs `X=5 Y=5` and
// returns 10 — the body ctor's own assignment runs. (3) is REJECTED under
// the v0.308 predicate at the declaration and never reaches the (4712)
// defer-or-positional dispatch; the new reject oracle there names the
// §9.3 redeclares-primary diagnostic. (4) is replaced with a truly
// different-arity/type shape that keeps the overload-selection oracle
// alive. (2) stays green on both sides.
//
// Filter run: `--area sema record_combined_form_explicit_ctor_dispatch`.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── (1) pin THE BUG: the issue's exact §9.3 repro — `new Point2(5)` must
//        resolve to the 1-parameter body ctor (5 → X := scale; Y := scale)
//        and return X + Y = 10; pre-fix, sema rejects with
//        "required parameter `Y` is not supplied (§22.5)" because the
//        primary's 2-param signature is the only candidate ever considered
//        ────────────────────────────────────────────────────────────────────
BEZEL_TEST(record_combined_form_explicit_ctor_dispatch_4712,
           body_ctor_resolution_spec_9_3) {
    auto r = runAot(
        "module M; "
        "public record Point2(X: int, Y: int) "
        "  public constructor(scale: int) "
        "    X := scale; "
        "    Y := scale; "
        "  end constructor; "
        "end Point2; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point2(5); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ── (2) regression guard: the same record's PRIMARY-shape construction
//        (`new Point2(1, 2)`) must still resolve to the auto-generated
//        primary and return 3 — the §9.3 rule "the body adds further
//        members" means the primary remains; the bug fix must not retire
//        it. The fix widens the record branch's gate (defers when an
//        explicit ctor is present); codegen's positional fast path
//        (`buildRecordValue`, lines 10715-10718 — `args.size() ==
//        recPosFieldCount && explicitCs == nullptr`) handles this case
//        because `ctorForArgs` finds no body-ctor match for the 2-arg
//        call ──────────────────────────────────────────────────────────────
BEZEL_TEST(record_combined_form_explicit_ctor_dispatch_4712,
           primary_shape_still_works_spec_9_3) {
    auto r = runAot(
        "module M; "
        "public record Point2(X: int, Y: int) "
        "  public constructor(scale: int) "
        "    X := scale; "
        "    Y := scale; "
        "  end constructor; "
        "end Point2; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point2(1, 2); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── (3) FLIPPED for fix #5132 (§9.3 / §22.6, v0.308): body ctor with a
//        single-int parameter of a DIFFERENT NAME than the primary's
//        positional field is no longer accepted. Under §22.6 overload
//        identity (arity, types, modifiers), the primary `record R(Val:
//        int)` and body `constructor(v: int)` are indistinguishable — a
//        §9.3 clash. The reject oracle names the §9.3 diagnostic and
//        pins that body-ctor deferral (the fix in 4712) does not save
//        the shape: the declaration is rejected outright. ────────────────
BEZEL_TEST(record_combined_form_explicit_ctor_dispatch_4712,
           body_ctor_differing_only_by_name_rejected_spec_9_3) {
    auto r = runAot(
        "module M; "
        "public record R(Val: int) "
        "  public constructor(v: int) "
        "    Val := v * 10; "
        "  end constructor; "
        "end R; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new R(7); "
        "    return r.Val; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "body constructor redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.3"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── (4) FLIPPED for fix #5132: the previous row pinned two overload
//        candidates for `new Box(...)` — `(n: int)` and `(s: string)`,
//        against `record Box(Count: int)`. The `(n: int)` body ctor
//        clashes with the primary under §22.6 (same arity 1, same type
//        int, same modifier). The shape is REJECTED at the declaration
//        before any overload-selection oracle can run. To keep the
//        overload-selection idea, the row is REPLACED with a non-clashing
//        shape: the primary is `Box(Count: int)` and the body ctor is
//        `(s: string)` (different types) and `(s: string, n: int)` (more
//        parameters) — both legitimately overload `Box` and exercise
//        `ctorForArgs`'s arity/type rank. ──────────────────────────────
BEZEL_TEST(record_combined_form_explicit_ctor_dispatch_4712,
           overloaded_body_ctors_distinct_types_arity_accepted_spec_22_6) {
    auto r = runAot(
        "module M; "
        "public record Box(Count: int) "
        "  public constructor(s: string) "
        "    Count := s.Length; "
        "  end constructor; "
        "  public constructor(s: string, n: int) "
        "    Count := s.Length + n * 10; "
        "  end constructor; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b1 := new Box(\"hi\"); "
        "    var b2 := new Box(\"hi\", 3); "
        "    return b1.Count + b2.Count; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 34);  // 2 + (2 + 30)
}
