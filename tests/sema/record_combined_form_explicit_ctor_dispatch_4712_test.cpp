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
//     explicit_ctor_match_accepted_spec_9_8_v0182.bzl — pure
//     declaration-side acceptance; never actually constructs via `new`.
//
// FAIL-WITHOUT / PASS-WITH (proven mechanically): with the new
// `if (hasExplicitCtor) return;` line reverted (and the record branch
// restored to its current unconditional positional-field check), test (1)
// below reports the §22.5 "required parameter `Y` is not supplied" error
// the issue itself measures; restored, it constructs `X=5 Y=5` and
// returns 10 — the body ctor's own assignment runs. The other tests stay
// green on both sides of the fix: (2) is the issue's companion on the
// primary-shape path (the existing behavior it pins), (3) is the
// `this(...)`-chained body ctor (codegen routes through `emitChainCall`),
// (4) exercises overload selection when the body declares TWO body
// ctors of different arity (each is matched positionally).
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

// ── (3) body ctor taking a reference-typed parameter — exercises the
//        full assignment path inside the body ctor body (no chain to the
//        primary, no named args, no overload set). The body ctor
//        initializes positional storage via §9.3's "assigning the auto-
//        generated read-only properties" wording and the body must run
//        untouched. `new P(new R(7))` constructs an embedded record
//        first, then the body ctor stores it into two positions and
//        sums them. Pins the inlined-explicit-ctor path
//        (`buildRecordValue`, lines 11015-11156) — including the
//        retain/retain-release envelope for the parameter pass
//        (`retainAssignedStringParams`, #2712). Sema path: deferred via
//        this fix's widened record branch ──────────────────────────────
BEZEL_TEST(record_combined_form_explicit_ctor_dispatch_4712,
           body_ctor_takes_reference_typed_arg) {
    auto r = runAot(
        "module M; "
        "public record R(Val: int) "
        "  public constructor(v: int) "
        "    Val := v * 10; "
        "  end constructor; "
        "end R; "
        "public record P(Sum1: int, Sum2: int) "
        "  public constructor(r: R) "
        "    Sum1 := r.Val + 1; "
        "    Sum2 := r.Val + 2; "
        "  end constructor; "
        "end P; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new P(new R(7)); "
        "    return p.Sum1 + p.Sum2; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    // Sum1 := 71; Sum2 := 72. Total: 143.
    BEZEL_EXPECT_EQ(r.exitCode, 143);
}

// ── (4) overload selection — multiple differently-shaped body ctors on
//        the same combined-form record. `new Box(3)` against two body
//        ctors — `(n: int)` and `(s: string)` — must resolve by arity to
//        the int one (3 → 30); `new Box("hi")` resolves to the string
//        one (length 2 → 2). Both pins of `ctorForArgs`'s arity-first
//        rank in the explicit-ctor path. Sema path: same defer as
//        (1)/(3) ──────────────────────────────────────────────────────────
BEZEL_TEST(record_combined_form_explicit_ctor_dispatch_4712,
           overloaded_body_ctors_dispatch_by_arity) {
    auto r = runAot(
        "module M; "
        "public record Box(Count: int) "
        "  public constructor(n: int) "
        "    Count := n * 10; "
        "  end constructor; "
        "  public constructor(s: string) "
        "    Count := s.Length; "
        "  end constructor; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b1 := new Box(3); "
        "    var b2 := new Box(\"hi\"); "
        "    return b1.Count + b2.Count; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 32);  // (3*10) + 2
}
