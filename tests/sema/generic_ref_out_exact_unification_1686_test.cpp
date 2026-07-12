// Spec-derived sema tests for fix #1686 — §14.5 (v0.184) "ref/out inference
// positions must unify exactly."
//
// §14.5 verbatim (docs/Bezel_Language_Spec.md, the phase-1 argument-driven-
// unification bullet list):
//   "a `ref` or `out` parameter position unifies **exactly**: the parameter
//    aliases the caller's location, so no conversion is admitted at the
//    binding or later at the applicability check;"
// and the Agreement paragraph:
//   "All bindings a type parameter receives — across arguments, lambda
//    returns, and the target — must be the **same type**. Two different
//    bindings are an inference failure: a compile error naming the type
//    parameter and both types ... The compiler computes no common
//    supertype and no widened join."
//
// §22.3's `ref`/`out` argument-locations section names `Swap<T>(ref a, ref
// b)` as the canonical example of `ref`'s true-alias semantics — the same
// method this fix's primary repro uses (issue #1686's own repro).
//
// Before the fix, bezelc's argument-driven type-parameter inference and its
// #896 conflicting-binding check ran ONLY as a side effect of substituting a
// call's RETURN type (inside `if (md->returnType) { ... }` in
// ExprTC::typeCall), so a VOID-returning generic method's ref/out arguments
// were never unified at all:
//   - `Swap<T>(ref a: T, ref b: T)` called `Swap(ref anInt, ref aLong)` was
//     silently accepted — each `ref` argument was checked independently
//     with no cross-argument agreement check, then aliased through
//     mismatched-width storage;
//   - separately, even where inference DID run (a non-void method, or an
//     explicit type-argument list), nothing checked a ref/out ARGUMENT's
//     own static type against the fully-substituted parameter type, so
//     `StoreW<long>(v, ref anIntSlot)` (T fixed at `long` by the explicit
//     list) was also silently accepted, even though a plain `int` local
//     cannot be aliased through a `long` reference.
//
// These tests are load-bearing: the REJECTED cases FAIL (no diagnostic) on
// pre-fix bezelc and PASS (the §14.5 diagnostic fires) post-fix; the
// ACCEPTED cases (exact ref/out, and ordinary by-value widening) must stay
// diagnostic-free both before and after — controls against over-rejection.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// The §14.5 / #1686 "conflicting ref/out inference" diagnostic — a type
// parameter bound to two different types across its occurrences
// (checkGenericRefOutExactUnification's inference-agreement half).
bool hasRefOutConflictError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot be bound to both") != std::string::npos &&
            d.message.find("14.5") != std::string::npos)
            return true;
    return false;
}

// The §14.5 / #1686 "ref/out does not unify exactly" diagnostic — a single
// ref/out argument's own static type disagreeing with the (otherwise fully
// determined) substituted parameter type (the exactness half).
bool hasRefOutExactnessError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not unify exactly with the inferred type") !=
                std::string::npos &&
            d.message.find("14.5") != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

// §22.3's own canonical `ref`/`out` aliasing example, and issue #1686's
// primary repro: a VOID generic method with two `ref T` parameters.
const char* kSwap =
    "static class Utils "
    "  public static method Swap<T>(ref a: T, ref b: T) "
    "    var temp := a; "
    "    a := b; "
    "    b := temp; "
    "  end Swap; "
    "end Utils; ";

}  // namespace

// §14.5 / #1686 primary repro: `Swap<T>(ref a: T, ref b: T)` called with a
// `ref int` and a `ref long` — the two `ref` occurrences of `T` disagree.
// Before the fix this compiled silently (inference never ran for a void
// method); it must be REJECTED with the conflicting-bindings diagnostic.
BEZEL_TEST(generic_ref_out_exact_unification_1686, void_swap_mismatched_types_rejected) {
    auto r = parseSemaAll(
        "module M; " + std::string(kSwap) +
        "class C "
        "  public static method Main(): int "
        "    var x: int := 1; "
        "    var y: long := 2; "
        "    Utils.Swap(ref x, ref y); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasRefOutConflictError(r));
}

// §14.5 / #1686 control: the SAME void `Swap<T>` called with two `ref int`
// arguments — both `ref` occurrences agree exactly. Must remain accepted
// (no over-rejection of the exact ref/out case).
BEZEL_TEST(generic_ref_out_exact_unification_1686, void_swap_exact_types_accepted) {
    auto r = parseSemaAll(
        "module M; " + std::string(kSwap) +
        "class C "
        "  public static method Main(): int "
        "    var x: int := 1; "
        "    var y: int := 2; "
        "    Utils.Swap(ref x, ref y); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §14.5 / #1686 regression guard: the same conflict, but on a NON-void
// overload (`SwapR<T>(ref a: T, ref b: T): T`) — the pre-fix code already
// caught this shape (inference ran as a side effect of the return-type
// substitution); confirms the new unconditional check doesn't regress or
// double-diagnose it.
BEZEL_TEST(generic_ref_out_exact_unification_1686, nonvoid_swap_mismatched_types_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method SwapR<T>(ref a: T, ref b: T): T "
        "    var temp := a; "
        "    a := b; "
        "    b := temp; "
        "    return temp; "
        "  end SwapR; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 1; "
        "    var y: long := 2; "
        "    var r := Utils.SwapR(ref x, ref y); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasRefOutConflictError(r));
}

// §14.5 / #1686: the ref/out-exactness half, isolated from inference
// agreement. `StoreW<T>(value: T, ref slot: T)` called with an EXPLICIT
// `<long>` type argument (so inference never runs) and a `ref` argument
// whose OWN static type is `int`, not `long` — the by-value argument
// itself agrees exactly (its type IS `long`), so only the ref argument's
// exactness is at stake. Must be REJECTED: a `long`-typed generic
// substitution cannot alias an `int` storage location.
BEZEL_TEST(generic_ref_out_exact_unification_1686, explicit_type_arg_ref_widen_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method StoreW<T>(value: T, ref slot: T) "
        "    slot := value; "
        "  end StoreW; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    var v: long := 5; "
        "    var s: int := 1; "
        "    Utils.StoreW<long>(v, ref s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasRefOutExactnessError(r));
}

// §14.5 / #1686 control: same explicit-`<long>` call, but the `ref`
// argument's own static type is ALSO `long` — an exact match. Must remain
// accepted.
BEZEL_TEST(generic_ref_out_exact_unification_1686, explicit_type_arg_ref_exact_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method StoreW<T>(value: T, ref slot: T) "
        "    slot := value; "
        "  end StoreW; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    var v: long := 5; "
        "    var s: long := 1; "
        "    Utils.StoreW<long>(v, ref s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §14.5 / #1686 control: ordinary BY-VALUE argument widening at a generic
// call's target-typed completion (phase 3) — `Identity<T>(v: T): T` has no
// ref/out parameter at all, so this check's fast bail must leave it
// completely untouched: `Identity(5)` infers `T = int`, and assigning the
// `int` result to an `Object`-typed local converts by the ordinary §5.1
// assignment conversion (the ROOT-doc §14.5 worked example). Must remain
// accepted — the fix must not reach for by-value positions at all.
BEZEL_TEST(generic_ref_out_exact_unification_1686, byvalue_widening_at_target_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "static class Utils "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := Utils.Identity(5); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §14.5 / #1686 / §22.4 regression guard: a PRE-DECLARED `out s: int`
// (not the inline `out var` form) against `H<T>(out o: T)` inferred SOLELY
// from that out argument — a single ref/out occurrence, trivially exact.
// Must remain accepted (mirrors #1164's predeclared-out inference guard).
BEZEL_TEST(generic_ref_out_exact_unification_1686, predeclared_out_single_occurrence_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class U "
        "  public static method H<T>(out o: T): int where T: value "
        "    o := 7; return 3; "
        "  end H; "
        "end U; "
        "class C "
        "  public static method Main(): int "
        "    var s: int := 0; "
        "    var r := U.H(out s); "
        "    return r + s; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §14.5 / #1686: the same pre-declared-out shape, but with an EXPLICIT
// `<int>` type argument and a MISMATCHED pre-declared `out` local (`long`,
// not `int`) — must be rejected via the exactness check (T is fixed by the
// explicit list; only the out argument's own type disagrees).
BEZEL_TEST(generic_ref_out_exact_unification_1686, predeclared_out_explicit_mismatch_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "static class U "
        "  public static method H<T>(out o: T): int where T: value "
        "    o := 7; return 3; "
        "  end H; "
        "end U; "
        "class C "
        "  public static method Main(): int "
        "    var s: long := 0; "
        "    var r := U.H<int>(out s); "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasRefOutExactnessError(r));
}

// §14.5 / #1686 / §22.4 control: an INLINE `out var b` argument against a
// nullable-out generic parameter (`SetIt<T>(x: T, out v: T?)`) — the
// out-var's type is inferred FROM the parameter (§22.4), so it carries no
// static type of its own to compare and must be skipped entirely by both
// halves of this check. Must remain accepted.
BEZEL_TEST(generic_ref_out_exact_unification_1686, inline_out_var_nullable_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "static class U "
        "  public static method SetIt<T>(x: T, out v: T?) where T: value "
        "    v := x; "
        "  end SetIt; "
        "end U; "
        "class C "
        "  public static method Main(): int "
        "    U.SetIt(88, out var b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}
