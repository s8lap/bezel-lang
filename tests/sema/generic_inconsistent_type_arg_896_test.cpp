// Spec-derived sema tests for fix #896 — a multi-parameter generic method
// with conflicting type-argument inference (e.g. Merge<T>(a: int?, b: bool?)
// binding T to two different types) must be rejected with a §14.1 compile
// error rather than silently accepted.  Before the fix, the "first-binding-
// wins" path in unifyArgSubst dropped the second incompatible binding;
// codegen then mixed int and bool in a PHINode → SIGABRT (rc=134).
//
// Authority: §14.1 — all occurrences of a type parameter must agree across
// the arguments of a single call.  #896.

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

// Check that the diagnostics contain the §14.1/#896 inconsistent-inference error.
bool hasInconsistentInferenceError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("inconsistent type-argument inference") !=
                std::string::npos &&
            d.message.find("14.1") != std::string::npos &&
            d.message.find("896") != std::string::npos)
            return true;
    return false;
}

// Shared generic-method template used in several tests.
const char* kMerge =
    "static class P "
    "  public static method Merge<T>(a: T?, b: T?): T where T: value "
    "    return a ?? b ?? default(T); "
    "  end Merge; ";

}  // namespace

// §14.1 / #896: Merge<T>(int?, bool?) — T cannot be both int and bool.
// This is the primary repro; bezelc crashed (SIGABRT) before the fix.
BEZEL_TEST(generic_inconsistent_type_arg_896, inconsistent_int_bool_rejected) {
    auto r = parseSemaAll(
        "module M; " + std::string(kMerge) +
        "  public static method Main(): int "
        "    var x: int? := 1; "
        "    var y: bool? := true; "
        "    var r := Merge(x, y); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(hasInconsistentInferenceError(r));
}

// §14.1 / #896: Merge<T>(int?, int?) — both args agree on T=int. ACCEPT.
BEZEL_TEST(generic_inconsistent_type_arg_896, consistent_int_int_accepted) {
    auto r = parseSemaAll(
        "module M; " + std::string(kMerge) +
        "  public static method Main(): int "
        "    var x: int? := 1; "
        "    var z: int? := 2; "
        "    return Merge(x, z); "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(hasInconsistentInferenceError(r));
}

// §14.1 / #896: explicit <int> with a bool? second arg is still a compile
// error (the argument type is inconsistent with the substituted T?=int?).
BEZEL_TEST(generic_inconsistent_type_arg_896, explicit_type_arg_with_bool_rejected) {
    auto r = parseSemaAll(
        "module M; " + std::string(kMerge) +
        "  public static method Main(): int "
        "    var x: int? := 1; "
        "    var y: bool? := true; "
        "    var r := Merge<int>(x, y); "
        "    return 0; "
        "  end Main; "
        "end P;");
    // The inconsistent-inference error fires when the inference loop is run
    // for explicit-typeArgs calls too (T=int from x, then bool from y).
    BEZEL_EXPECT_TRUE(hasInconsistentInferenceError(r));
}

// §14.1 / #896: a two-type-parameter method Pair<A,B>(a:A, b:B) given
// (int, bool) — A=int, B=bool, no conflict. ACCEPT.
BEZEL_TEST(generic_inconsistent_type_arg_896, two_type_params_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class Q "
        "  public static method Pair<A, B>(a: A, b: B): int "
        "    return 0; "
        "  end Pair; "
        "  public static method Main(): int "
        "    var x: int := 1; "
        "    var y: bool := true; "
        "    return Pair(x, y); "
        "  end Main; "
        "end Q;");
    BEZEL_EXPECT_FALSE(hasInconsistentInferenceError(r));
}

// §14.1 / #896 / #850: a single-param nullable peel Identity<T>(v:T?):T
// with one int? arg — T=int, consistent. ACCEPT.
BEZEL_TEST(generic_inconsistent_type_arg_896, single_nullable_peel_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "static class R "
        "  public static method Identity<T>(v: T?): T where T: value "
        "    return v ?? default(T); "
        "  end Identity; "
        "  public static method Main(): int "
        "    var x: int? := 5; "
        "    return Identity(x); "
        "  end Main; "
        "end R;");
    BEZEL_EXPECT_FALSE(hasInconsistentInferenceError(r));
}

// §22.4 / §14.1 / #1164: a generic method `G<T>(a: T, out o: T)` called with an
// inline `out var s` must NOT be flagged as inconsistent inference. The inline
// `out var` has no type of its own — its type is INFERRED FROM the matched out
// parameter (§22.4), so it must not contribute an independent binding to T
// (which would clash with T=int from the value argument `5`). ACCEPT.
BEZEL_TEST(generic_inconsistent_type_arg_896, inline_out_var_not_inconsistent_1164) {
    auto r = parseSemaAll(
        "module M; "
        "static class U "
        "  public static method G<T>(a: T, out o: T): T where T: value "
        "    o := a; return a; "
        "  end G; "
        "  public static method Main(): int "
        "    var r := U.G<int>(5, out var s); return r + s; "
        "  end Main; "
        "end U;");
    BEZEL_EXPECT_FALSE(hasInconsistentInferenceError(r));
}

// §22.4 / §14.1 / #1164 negative guard: a GENUINE conflict between two value
// arguments (`G<T>(a: T, b: T)` called `G(5, "x")`) is STILL correctly rejected
// — the #1164 fix only skips inline out-var args, not real value arguments.
BEZEL_TEST(generic_inconsistent_type_arg_896, genuine_conflict_still_rejected_1164) {
    auto r = parseSemaAll(
        "module M; "
        "static class U "
        "  public static method G<T>(a: T, b: T): T where T: value "
        "    return a; "
        "  end G; "
        "  public static method Main(): int "
        "    var r := U.G(5, \"x\"); return 0; "
        "  end Main; "
        "end U;");
    BEZEL_EXPECT_TRUE(hasInconsistentInferenceError(r));
}
