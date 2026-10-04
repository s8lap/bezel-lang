// Regression tests for issue #5394 — §11.3/§14.5 (v0.314):
// `checkTypeOverloads` / `signaturesIndistinguishable` never read
// `genericParams`, so two methods differing ONLY in generic arity are
// falsely rejected as indistinguishable overloads.
//
// Spec §11.3 (v0.314, from the #5364 ruling):
//   "Two methods declaring different numbers of type parameters of their own
//    never clash, whatever their parameters: `M<T>(x: T)` beside
//    `M<T, U>(x: T)`, or `FromException<T>(owned exception: Exception)`
//    beside the non-generic `FromException(owned exception: Exception)`
//    ([§30.10], stdlib [§6.7])."
//
// FALSE REJECT (the bug): `M<T>(x: T)` vs `M<T,U>(x: T)` and
// `N(x: int)` vs `N<T>(x: int)` are rejected as indistinguishable because
// `signaturesIndistinguishable` compares only parameter types (both `T` / `int`
// after resolution), without considering that the method-level generic
// arities differ.
//
// THE FIX: in `checkTypeOverloads`, compare `MethodSymbol::genericParams`
// size before calling `signaturesIndistinguishable`. If sizes differ,
// the pair is distinguishable (no clash).
//
// FAIL-WITHOUT (MEASURED, sema change reverted to r23-c28 base):
// T1, T2, T3 compile cleanly (accept oracle) — the bug causes them to
// emit the "indistinguishable" diagnostic and fail before codegen.
// T4, T5 (same-count clash cases) reject as expected — verifying the
// positional correspondence rule is unchanged for same-count generics.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
//                                          // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Convenience: assert the program is accepted (compiles cleanly, never runs).
void expectAccepted(const std::string& program) {
    auto r = runAot(program);
    // Compilation must fail (r.ran == false) but NOT due to a compile error.
    // A clean compile also has r.ran == false (compile succeeded, program never
    // ran). The discriminator is whether error diagnostics were emitted.
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
}

// Convenience: assert the program is rejected as indistinguishable overload.
void expectClashRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "indistinguishable") ||
        diagnosticMessageContains(r, "same signature"));
}

}  // namespace

// ── T1 — §11.3 (v0.314, #5364 ruling): M<T>(x: T) vs M<T,U>(x: T).
// Different generic arity (1 vs 2) — must ACCEPT (the bug rejects). ────
BEZEL_TEST(generic_overload_arity_distinct_5394,
           one_vs_two_type_params_accepted_spec_11_3) {
    expectAccepted(
        "module M;\n"
        "class C\n"
        "  public method M<T>(x: T): T => x;\n"
        "  public method M<T, U>(x: T): T => x;\n"
        "  public static method Main(): int => 0;\n"
        "end C;\n");
}

// ── T2 — §11.3 (v0.314): N(x: int) vs N<T>(x: int).
// Non-generic vs generic (0 vs 1) — must ACCEPT (the bug rejects). ───
BEZEL_TEST(generic_overload_arity_distinct_5394,
           non_generic_vs_one_type_param_accepted_spec_11_3) {
    expectAccepted(
        "module M;\n"
        "class C\n"
        "  public method N(x: int): int => x;\n"
        "  public method N<T>(x: int): int => x;\n"
        "  public static method Main(): int => 0;\n"
        "end C;\n");
}

// ── T3 — §11.3 (v0.314): Both 2-arity, different positions.
// M<T,U>(a: T, b: U) vs M<T,U>(a: U, b: T) — must ACCEPT.
// (Same count, different positional correspondence.) ────────────────────
BEZEL_TEST(generic_overload_arity_distinct_5394,
           same_count_permuted_positions_accepted_spec_11_3) {
    expectAccepted(
        "module M;\n"
        "class C\n"
        "  public method M<T, U>(a: T, b: U): int => 0;\n"
        "  public method M<T, U>(a: U, b: T): int => 0;\n"
        "  public static method Main(): int => 0;\n"
        "end C;\n");
}

// ── T4 — §11.3 (v0.314): Same count (1), same position.
// M<T>(x: T) vs M<U>(x: U) — both single-arity, positionally equivalent.
// Must REJECT (the two are indistinguishable per §11.3). ───────────────
BEZEL_TEST(generic_overload_arity_distinct_5394,
           same_count_same_position_clashes_spec_11_3) {
    expectClashRejected(
        "module M;\n"
        "class C\n"
        "  public method M<T>(x: T): T => x;\n"
        "  public method M<U>(x: U): U => x;\n"
        "  public static method Main(): int => 0;\n"
        "end C;\n");
}

// ── T5 — §11.3 (v0.314): Same count (2), same position (T,U vs V,W).
// M<T, U>(x: T) vs M<V, W>(x: V) — must REJECT. ───────────────────
BEZEL_TEST(generic_overload_arity_distinct_5394,
           same_count_same_signature_clashes_spec_11_3) {
    expectClashRejected(
        "module M;\n"
        "class C\n"
        "  public method M<T, U>(x: T): T => x;\n"
        "  public method M<V, W>(x: V): V => x;\n"
        "  public static method Main(): int => 0;\n"
        "end C;\n");
}
