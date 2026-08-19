// Spec-derived runtime tests for §22.16 contract clauses (v0.250, #3038) —
// `result` / `old(E)` binding + capture. Each test exercises the AOT path
// (fork-isolated Driver::compileFile + clang link + native exec) and
// asserts on the program's exit code (or the value a try/catch
// recovered after a ContractViolationException).
//
// §22.16 verbatim (v0.250):
//   * "Within an `ensures` clause of a member with a declared return type,
//     `result` denotes the value being returned, typed as the declared
//     return type, in a read-only borrow position" — the §22.16
//     evaluation point is "after the return value is computed and bound
//     and after every `out` parameter's callee-local slot holds its
//     final value, and BEFORE the frame's scope-end disposals."
//   * "`old(E)` … denotes the value `E` had at the member's entry,
//     defined by desugaring: for each `old` operand across the member's
//     `ensures` clauses, in textual order, the compiler introduces a
//     hidden entry binding equivalent to `var __oldN := E;`" — capture
//     at the §22.16 "after `requires` clauses" evaluation point.
//   * "a **move-only value operand** is a compile error at the contract
//     clause" — the class-typed reject that this slice also enforces.
//
// #3038 (Option B) supersedes the #2942 "not yet implemented" Option-A
// rejection. Each test is mechanically proved to fail without the fix
// and pass with it: pre-#3038 the program would reject with the
// "the contextual keyword `result` / `old` is not yet implemented in
// `ensures` clauses" diagnostic and never reach the runtime; the
// assertion is on the runtime triple.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

int runFull(const std::string& program) {
    auto r = bezel::test::runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Returns the joined messages of every Error diagnostic the AOT driver
// recorded, or empty string when the program compiled and ran. Used by
// the class-typed-`old` reject test to assert the diagnostic TEXT (the
// §22.16 ownership-restriction message that #3038 introduces, not the
// pre-#3038 "not yet implemented" message from #2942).
std::string compileErrorMessages(const std::string& program) {
    auto r = bezel::test::runAot(program);
    if (r.ran) return "";
    std::string joined;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (!joined.empty()) joined += "\n";
        joined += d.message;
    }
    return joined;
}

}  // namespace

// =========================================================================
// §22.16: `result` borrows the method's return value in an `ensures` clause
// — satisfied clause does NOT fault.
// =========================================================================
BEZEL_TEST(ensures_result_old_binding_3038,
           result_borrow_in_ensures_satisfied_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (result = x * 2) "
        "    return x * 2; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; r := c.M(21); return r; "
        "  end Main; "
        "end C;"), 42);
}

// =========================================================================
// §22.16: `result` — a violated clause faults with ContractViolationException
// at the §22.16 evaluation point (after the return value is bound).
// =========================================================================
BEZEL_TEST(ensures_result_old_binding_3038,
           result_borrow_in_ensures_violated_raises_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (result = 99) "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(42); "
        "    catch ContractViolationException => r := 77; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 77);
}

// =========================================================================
// §22.16: `old(x)` captures the entry value — a parameter untouched
// since entry satisfies an `old(x) = x` clause, even when the body
// overwrites x with a different value. Proves the capture happens at
// method entry, not at the ensures evaluation point (the §22.16
// "value `E` had at the member's entry" semantics).
// =========================================================================
BEZEL_TEST(ensures_result_old_binding_3038,
           old_captures_entry_value_even_when_param_overwritten_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (old(x) = 42) "
        "    x := 100; "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    r := c.M(42); "
        "    return r; "
        "  end Main; "
        "end C;"), 100);
}

// =========================================================================
// §22.16: `old(x)` — the entry value differs from the post-state, so
// the clause faults. Catches the inverse of the previous test (a
// regression that re-evaluated E at the ensures point would silently
// pass this one).
// =========================================================================
BEZEL_TEST(ensures_result_old_binding_3038,
           old_capture_fails_when_entry_differs_from_post_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(x: int): int "
        "    ensures (old(x) = 99) "
        "    x := 100; "
        "    return x; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try "
        "      r := c.M(42); "
        "    catch ContractViolationException => r := 88; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 88);
}

// =========================================================================
// §22.16: a class-typed `old(E)` operand is REJECTED at compile time —
// a frame-resident borrow of a class reference would outlive the method
// body, which the spec forbids. The diagnostic names the copyable-
// projection remedy (a value-typed projection of the pre-state, e.g.
// `old(d.v)` instead of `old(d)`, or a `pure` member's result over a
// borrowed receiver).
//
// Pre-#3038 the program would also have been rejected, but by the
// "not yet implemented in `ensures` clauses" diagnostic from #2942 —
// NOT the §22.16 ownership-restriction diagnostic that #3038 emits. A
// pass-without-#3038 / fail-with-#3038 reversal on the diagnostic
// TEXT is the mechanical proof this test pins.
// =========================================================================
BEZEL_TEST(ensures_result_old_binding_3038,
           class_typed_old_operand_rejected_spec_22_16) {
    std::string err = compileErrorMessages(
        "module M; class D "
        "  public v: int := 0; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "end D; "
        "class C "
        "  public method M(d: D): int "
        "    ensures (old(d).v = 42) "
        "    return 0; "
        "  end M; "
        "end C;");
    // Must be rejected (no runtime).
    BEZEL_EXPECT_TRUE(!err.empty());
    // The §22.16 ownership-restriction diagnostic — NOT the pre-#3038
    // "not yet implemented" message from #2942.
    BEZEL_EXPECT_TRUE(err.find("would hold a borrow across the entire method body")
                      != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("copyable projection") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("22.16") != std::string::npos);
    // And the pre-#3038 message must NOT appear (regression guard
    // against the #2942 diagnostic being re-emitted after #3038).
    BEZEL_EXPECT_EQ(err.find("not yet implemented in `ensures` clauses"),
                    std::string::npos);
}
