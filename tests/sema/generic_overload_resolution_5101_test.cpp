// Spec-derived regression tests for issue #5101 — §22.6/§14.5: two same-arity
// overloads where at least one is GENERIC made the call unresolvable
// ("no overload ... is applicable") even when exactly one candidate's
// per-candidate type-argument inference (§14.5 phase 1) actually succeeds and
// yields an exact match.
//
// §22.6 (v0.307, "Generic candidates"), verbatim:
//   "A generic method enters overload resolution only after per-candidate
//    type-argument inference (§14.5): inference runs independently for each
//    generic candidate, and a candidate whose inference fails is
//    inapplicable — the call resolves among the remaining candidates without
//    error from the failed one. Surviving candidates enter the ranking below
//    with their substituted signatures. When a non-generic candidate and a
//    generic candidate are both applicable, and neither the ranking below
//    nor the defaulted-arity tie-break separates them, the non-generic
//    candidate is selected ... The non-generic preference is the last
//    comparison before ambiguity; in particular the defaulted-arity
//    tie-break applies first: `M(n: int, m: int := 0)` and `M<T>(x: T)` on
//    the call `M(5)` resolve to the generic candidate — it fills no default
//    ... while `Show(x: int)` and `Show<T>(x: T)` on `Show(5)` resolve to
//    the non-generic. Two generic candidates remain subject to the ordinary
//    ambiguity rule."
//
// ROOT CAUSE (measured by tracing classifyArg/implicitlyConvertible/TypeRef):
// `resolveMethodOverload`'s ranking loop fed `classifyArg` each candidate's
// BARE, unsubstituted declared parameter type (`T`, a `Kind::Named` TypeRef
// with `declaration == nullptr`) instead of the §14.5-phase-1-inferred
// concrete type. `classifyArg`/`implicitlyConvertible` have no rule admitting
// a concrete argument against such a bare type-parameter reference, so EVERY
// generic candidate whose argument reached a bare-type-parameter position was
// unconditionally `NotApplicable` — regardless of whether inference would
// actually succeed. `EqF<T>(a: T, b: string)` / `EqF<T>(a: T, b: T)` called
// as `EqF(5, 5)` therefore rejected BOTH candidates (the first legitimately,
// on `b`; the second spuriously, since `T = int` unifies cleanly and the
// substituted signature `(int, int)` is an exact match) and reported
// "no overload ... is applicable" — the exact symptom #5101 reports.
//
// A SECOND, previously-latent defect surfaced while fixing the first: with
// generic candidates correctly ranked, `Show(x: int)` / `Show<T>(x: T)`
// called `Show(5)` now has TWO fully-exact-ranked applicable candidates
// (before the fix the generic one was always inapplicable, so this pair
// never reached a tie) — §22.6's own "non-generic preference" last
// tie-break had never been implemented, since it was previously unreachable.
//
// THE FIX (§14.5 phase 1 / §22.6, v0.307):
//   1. `resolveMethodOverload` computes each GENERIC candidate's phase-1
//      argument-driven-unification substitution (reusing the established
//      `unifyArgSubst` helper) BEFORE building the ranking loop's parameter
//      types, and ranks against the SUBSTITUTED signature. A candidate whose
//      inference disagrees across occurrences (§14.5 "Agreement") is left
//      unsubstituted, which — unchanged from before the fix — ranks it
//      `NotApplicable` and drops it out silently, exactly as §22.6's
//      "Interaction with overloading" requires.
//   2. `resolveOverload` gained the missing non-generic-preference tie-break,
//      applied strictly AFTER the exact-match ranking and the defaulted-
//      arity tie-break (an `isGeneric` vector, empty and therefore a no-op
//      at every call site that can never have a generic candidate —
//      constructor overloading, §14.5's own "Scope" paragraph).
//
// FAIL-WITHOUT (mechanically verified — copy aside, `git checkout --`,
// rebuild, confirm FAIL; restore, rebuild, confirm PASS):
//   - `issue_repro_...` and `only_generic_candidate_applicable_...`: FAIL
//     before the fix with "no overload of `...` is applicable" (root cause
//     #1 above); PASS after.
//   - `non_generic_preferred_over_generic_...`: compiled and returned the
//     WRONG value (1, the generic branch) before EITHER half of the fix,
//     because pre-fix the generic candidate was always inapplicable so the
//     non-generic candidate won by being the sole applicable one — i.e. this
//     row passes on the UNFIXED tree for the wrong reason. It is included
//     as a spec-fidelity / non-regression control for fix #2, and is backed
//     by `defaulted_arity_precedes_non_generic_preference_...` below, which
//     DOES fail-without (the generic candidate is the only one that fills no
//     default, so it must WIN — pre-fix it was inapplicable and the call
//     mis-resolved to the non-generic candidate, an observable wrong value).
//
// Two of #5101's own reported variants (a decoy differing by `virtual`) hit
// an entirely separate codegen defect in virtual generic-method dispatch —
// filed as #5113 and deliberately NOT exercised here; every case below uses
// a non-virtual (or static) method so it isolates the §22.6/§14.5 sema fix.

#include "support/aot_support.hpp"  // runAot — AOT test harness (never runJit)
#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// AOT runner: compiles a program to native, runs it, returns its exit code.
// -999 on any compile/link/run failure so an "exit 0" assertion can't pass by
// accident when the compiler rejected the program outright.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// PASS — the defect #5101 reported: FAILS-WITHOUT with "no overload ... is
// applicable".
// ---------------------------------------------------------------------------

// §22.6 / §14.5 (#5101): the issue's exact repro shape (instance method, the
// `where T: new()` constrained decoy included), minus `virtual` (see #5113
// above) — `EqF<T>(a: T, b: string)` is inapplicable (arg 2 is `int`, no
// implicit conversion to `string`); `EqF<T>(a: T, b: T)` unifies `T = int`
// from BOTH occurrences (Agreement holds) and its substituted signature
// `(int, int)` is an exact match. FAILS-WITHOUT with "no overload of `EqF`
// is applicable to these arguments" (§22.6).
BEZEL_TEST(sema_generic_overload_5101,
           issue_repro_TT_wins_over_T_string_decoy_spec_22_6_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B "
        "  public constructor() end constructor; "
        "  public method EqF<T>(a: T, b: string): int where T: new() => 63; "
        "  public method EqF<T>(a: T, b: T): int => 61; "
        "end B; "
        "static class C "
        "  public static method Main(): int "
        "    var g: B := new B(); "
        "    return g.EqF(5, 5); "
        "  end Main; "
        "end C;"),
        61);
}

// §22.6 "Generic candidates" (#5101, discovered scope-widening while
// diagnosing the issue): a NON-generic candidate whose parameter type simply
// does not match (`Foo(x: string)` against an `int` argument) is inapplicable
// regardless of generics; the ONLY applicable candidate is the generic one,
// `Foo<T>(x: T)` at `T = int`. This is not literally #5101's 2-generic shape,
// but the same root cause: pre-fix, `Foo<T>` was ALSO always inapplicable
// (bare-`T` vs `int`), so this call was rejected outright even though only
// one candidate was ever in contention. FAILS-WITHOUT with "no overload of
// `Foo` is applicable".
BEZEL_TEST(sema_generic_overload_5101,
           only_generic_candidate_applicable_spec_22_6_14_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Foo(x: string): int => 1; "
        "  public static method Foo<T>(x: T): int => 2; "
        "  public static method Main(): int "
        "    return C.Foo(5); "
        "  end Main; "
        "end C;"),
        2);
}

// §22.6 "Generic candidates" (v0.307), the spec's OWN worked example
// verbatim: "`Show(x: int)` and `Show<T>(x: T)` on `Show(5)` resolve to the
// non-generic." Exercises fix #2 (the new non-generic-preference tie-break):
// once fix #1 makes `Show<T>` correctly applicable (T = int, exact), both
// candidates rank fully exact and tie; the non-generic-preference tie-break
// must select `Show(int)`. See the file header for why this row alone does
// not prove fail-without; `defaulted_arity_precedes_non_generic_preference`
// below is the fail-without-carrying sibling for the same tie-break.
BEZEL_TEST(sema_generic_overload_5101,
           non_generic_preferred_over_generic_spec_22_6_generic_candidates) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Show(x: int): int => 1; "
        "  public static method Show<T>(x: T): int => 2; "
        "  public static method Main(): int "
        "    return C.Show(5); "
        "  end Main; "
        "end C;"),
        1);
}

// §22.6 "Generic candidates" (v0.307), the spec's OWN worked example
// verbatim: "the defaulted-arity tie-break applies first: `M(n: int,
// m: int := 0)` and `M<T>(x: T)` on the call `M(5)` resolve to the generic
// candidate — it fills no default." FAILS-WITHOUT: pre-fix, `M<T>(x: T)` was
// unconditionally inapplicable (bare `T` vs `int`), so the ONLY applicable
// candidate was the non-generic one and the call wrongly resolved to it
// (11, not 22) — an observable wrong VALUE, not a rejection, which is why
// this row (unlike the `Show` row above) does carry fail-without: this is
// the ordering assertion that fix #2's tie-break must run its
// defaulted-arity check BEFORE its non-generic-preference check, exactly as
// §22.6 states ("the non-generic preference is the LAST comparison").
BEZEL_TEST(sema_generic_overload_5101,
           defaulted_arity_precedes_non_generic_preference_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method M(n: int, m: int := 0): int => 11; "
        "  public static method M<T>(x: T): int => 22; "
        "  public static method Main(): int "
        "    return C.M(5); "
        "  end Main; "
        "end C;"),
        22);
}

// ---------------------------------------------------------------------------
// REJECT — the load-bearing controls. The fix must not over-widen acceptance
// past what §22.6/§14.5 actually admit.
// ---------------------------------------------------------------------------

// §22.6 "Generic candidates": "Two generic candidates remain subject to the
// ordinary ambiguity rule." `Eq<T>(a: T, b: T)` and `Eq<T>(a: T, b: int)`
// called `Eq(5, 5)` both unify `T = int` and both rank fully exact — no
// tie-break applies (both are generic, so fix #2's non-generic-preference
// branch is correctly silent), so the call must be REJECTED as ambiguous,
// not silently resolved to whichever candidate the phase-1 substitution
// happens to iterate first.
BEZEL_TEST(sema_generic_overload_5101,
           two_generic_candidates_stay_ambiguous_spec_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static method Eq<T>(a: T, b: T): int => 1; "
        "  public static method Eq<T>(a: T, b: int): int => 2; "
        "  public static method Main(): int "
        "    return C.Eq(5, 5); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "ambiguous call"));
}

// §14.5 "Agreement" + §22.6 "Interaction with overloading": in an OVERLOADED
// context, a generic candidate whose two occurrences of `T` disagree
// (`M(5, "hi")` against `M<T>(a: T, b: T)`) must fall out of contention
// SILENTLY (dropped as inapplicable, no separate error attributed to it) —
// it must not crash, and it must not be spuriously accepted by ranking
// against its stale bare-`T` signature. The sibling non-generic candidate
// (`M(a: string, b: string)`) is independently inapplicable (`int` does not
// convert to `string`), so the call is correctly rejected overall. Pins that
// `phase1Subst[c].clear()` on a conflict (this file's fix) composes safely
// with a second candidate in the SAME call, not just the single-candidate
// (non-overloaded) generic-inference-conflict path that already existed.
BEZEL_TEST(sema_generic_overload_5101,
           agreement_conflict_drops_out_silently_when_overloaded_spec_14_5_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static method M<T>(a: T, b: T): int => 1; "
        "  public static method M(a: string, b: string): int => 2; "
        "  public static method Main(): int "
        "    return C.M(5, \"hi\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.1 / §22.6 non-regression: a plain (non-generic) overload set is
// unaffected by `phase1Subst`/`isGeneric` — both are computed only for
// candidates with a non-empty `genericParams` list, so this is exactly the
// pre-#5101 code path with no substitution and no tie-break contribution.
BEZEL_TEST(sema_generic_overload_5101,
           non_generic_overload_set_unaffected_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Bar(a: int, b: string): int => 77; "
        "  public static method Bar(a: int, b: int): int => 71; "
        "  public static method Main(): int "
        "    return C.Bar(5, 5); "
        "  end Main; "
        "end C;"),
        71);
}

// ---------------------------------------------------------------------------
// Non-author review (2026-09-28) of 868e2675: MEASURED defect — the
// `phase1Subst` loop paired `ce.args[pi]` with `md->params[pi]` by raw
// POSITION, so a NAMED argument written out of declaration order unified
// against the WRONG parameter's declared type. §22.5 (v0.307) requires
// resolution to be independent of the order named arguments are written in.
// The three rows below are the reviewer's requested coverage: named
// arguments reversed, mixed positional+named, and a defaulted param omitted
// — each against a generic overload pair with a distinct observable result.
// ---------------------------------------------------------------------------

// §22.5 / §14.5 (#5101 follow-up): `Foo<T>(a: T, b: int)` / `Foo<T>(a: T,
// b: string)` called with the named arguments REVERSED in writing order:
// `C.Foo(b := "text", a := 5)`. Correctly bound, `a = 5` (T = int) and
// `b = "text"`: the `int`-`b` candidate is inapplicable (`"text"` does not
// convert to `int`); the `string`-`b` candidate is the unique exact match.
//
// FAILS-WITHOUT: the raw-positional pairing zips call-site position 0
// (`b := "text"`, a `string`) against declared parameter 0 (`a: T`),
// wrongly inferring `T = string`; the ranking loop below (already correctly
// bound via `paramToArg`) then classifies the REAL `a` argument (`5`, an
// `int`) against the wrongly-substituted parameter type `string` — Not
// Applicable — for the one candidate that should have won, so BOTH
// candidates end up inapplicable and the call is mis-rejected with
// "no overload of `Foo` is applicable" (§22.6) instead of returning 2.
BEZEL_TEST(sema_generic_overload_5101,
           named_arguments_reversed_bind_by_declared_position_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Foo<T>(a: T, b: int): int => 1; "
        "  public static method Foo<T>(a: T, b: string): int => 2; "
        "  public static method Main(): int "
        "    return C.Foo(b := \"text\", a := 5); "
        "  end Main; "
        "end C;"),
        2);
}

// §22.5 / §14.5 (#5101 follow-up): mixed positional + named, with the named
// PORTION written out of declaration order and the GENERIC parameter itself
// in the reordered span (`x` positional, then `b` before `a` though `a` is
// declared before `b`). `Foo<T>(x: int, a: T, b: string)` / `Foo<T>(x: int,
// a: T, b: int)` called as `C.Foo(5, b := "hey", a := 9)`: correctly bound,
// `x = 5`, `a = 9` (T = int), `b = "hey"` — only the `string`-`b` candidate
// is applicable.
//
// FAILS-WITHOUT: raw-positional pairing zips call position 1 (`b := "hey"`,
// a `string`) against declared parameter 1 (`a: T`, the GENERIC slot),
// wrongly inferring `T = string` for both candidates; the ranking loop then
// classifies the REAL `a` argument (`9`, an `int`) against the wrongly-
// substituted `string` — Not Applicable — for both, so the call is
// mis-rejected with "no overload of `Foo` is applicable" instead of
// returning 5.
BEZEL_TEST(sema_generic_overload_5101,
           mixed_positional_and_named_reordered_generic_slot_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Foo<T>(x: int, a: T, b: string): int => 5; "
        "  public static method Foo<T>(x: int, a: T, b: int): int => 6; "
        "  public static method Main(): int "
        "    return C.Foo(5, b := \"hey\", a := 9); "
        "  end Main; "
        "end C;"),
        5);
}

// §22.5 / §14.5 (#5101 follow-up): named arguments reversed AND a defaulted
// third parameter omitted entirely from the call. `Foo<T>(a: T, b: string,
// c: string := "z")` / `Foo<T>(a: T, b: int, c: string := "q")` called as
// `C.Foo(b := "hi", a := 9)` (both `b`- and `a`-bearing candidates are
// applicable to the ARITY — `c` is defaulted on both). Correctly bound,
// `a = 9` (T = int), `b = "hi"`, `c` filled by its default: only the
// `string`-`b` candidate is applicable (`"hi"` does not convert to `int`).
// This also exercises the `paramToArg[j] < 0` skip for the omitted `c`
// slot — it must be skipped, not read as if it aligned with a real
// argument.
//
// FAILS-WITHOUT: raw-positional pairing zips call position 0 (`b := "hi"`,
// a `string`) against declared parameter 0 (`a: T`), wrongly inferring
// `T = string` for BOTH candidates (this loop never reaches position 2, so
// the defaulted `c` slot is not itself where the corruption happens — the
// corruption is entirely from `a` and `b` swapping meaning). The ranking
// loop then classifies the REAL `a` argument (`9`, an `int`) against the
// wrongly-substituted `string` for both candidates — Not Applicable for
// both — mis-rejecting the call outright (`run` returns -999) instead of
// resolving to 30.
BEZEL_TEST(sema_generic_overload_5101,
           named_arguments_reversed_with_defaulted_param_omitted_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static method Foo<T>(a: T, b: string, c: string := \"z\"): "
        "int => 30; "
        "  public static method Foo<T>(a: T, b: int, c: string := \"q\"): "
        "int => 99; "
        "  public static method Main(): int "
        "    return C.Foo(b := \"hi\", a := 9); "
        "  end Main; "
        "end C;"),
        30);
}
