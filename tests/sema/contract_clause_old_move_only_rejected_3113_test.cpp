// fix #3113 (§22.16 / §15.3 / §35.4): a move-only value operand in an
// `old(...)` contract clause was admitted with no diagnostic, even though
// §22.16 says — under the "old(E) capture taxonomy" rules — "A move-only
// value operand is a compile error at the contract clause". Such an operand
// (an owning inline array, an owning-classified choice, or the nullable
// wrapper of one) admits no sound capture: a copy is the operation
// move-only-ness forbids (§15.3), and the literal desugaring would not copy
// but consume — every store of a move-only value into an owning location is
// a move, a contract clause mutating the state it exists to observe.
//
// Spec citations: §22.16 "old(E) capture taxonomy", §15.3 "Move-only value
// types", §35.4 "Choice classification". The case at s22_16_0015 pins the
// spec rule against an owning-classified choice; this gtest exercises the
// SAME shape plus an owning inline array so that both arms of the
// `isMoveOnlyValueType` taxonomy are pinned, against AOT.
//
// Root cause (sema/ExprTypecheck_Calls.cpp's `old(...)` handler, around
// line 2176): the §22.16 "old" arm rejected class- and interface-typed
// operands (which would hold a borrow across the entire method body) but
// never tested for the move-only taxonomy. The fix extends the handler with
// `common::isMoveOnlyValueType` and rejects with a diagnostic that names
// §22.16, §15.3, and §35.4 and points at the copyable-projection remedy,
// matching the class-arm's wording exactly so both arms are catchable by
// the same diagnostic matchers.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Was this JitResult a SKIP (toolchain unavailable) rather than a real
// failure? A SKIP is reported via ran=false with a diagnostic containing
// "SKIP".
bool wasSkipped(const bezel::test::JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Helper: assert the program was rejected AND the diagnostic names the
// move-only rejection (not some unrelated downstream error).
bool rejectedByMoveOnlyOldRule(const bezel::test::JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`old` over") != std::string::npos &&
            d.message.find("move-only") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =========================================================================
// Repros reproducing the spec rule over the two move-only taxonomy kinds.
// =========================================================================

// §22.16 "old(E) ... a move-only value operand is a compile error" over an
// owning-classified choice (the case s22_16_0015 pins). Pre-fix: the
// program compiles, the move-only operand slips past the class/interface
// class arm, and the contract-clause lowering produces either an IR-
// verifier failure or — with everything else in place — silently passes the
// move-only value across the contract boundary. Post-fix: a real rejection
// at the `old(...)` token with the §22.16/§15.3/§35.4 attribution.
BEZEL_TEST(contract_clause_old_move_only_rejected_3113,
           owning_choice_at_old_rejected_spec_22_16) {
    auto r = runAot(
        "module M;\n"
        "class Box public constructor() end constructor; end Box;\n"
        "choice Basket\n"
        "  Full(owned item: Box),\n"
        "  Empty;\n"
        "end Basket;\n"
        "class C\n"
        "  public method Peek(b: Basket): int\n"
        "    ensures((case old(b) of Full(_) => true, Empty => false))\n"
        "    return 0;\n"
        "  end Peek;\n"
        "end C;\n");
    // SKIP only if AOT toolchain unavailable; that is not a real PASS.
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(rejectedByMoveOnlyOldRule(r));
}

// §22.16 "old(E) ... move-only value operand" applied to an owning inline
// array — the other arm of `isMoveOnlyValueType`. The operand type is
// `[2 owned]int`. Pre-fix: the program compiles cleanly with no
// diagnostic at `old(...)` (the class/interface arm tests neither inline
// arrays nor choice classifications).
BEZEL_TEST(contract_clause_old_move_only_rejected_3113,
           owning_inline_array_at_old_rejected_spec_22_16) {
    auto r = runAot(
        "module M;\n"
        "class C\n"
        "  public method Sink(a: [2 owned]int): int\n"
        "    ensures(old(a)[0] = old(a)[0])\n"
        "    return 0;\n"
        "  end Sink;\n"
        "end C;\n");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(rejectedByMoveOnlyOldRule(r));
}

// =========================================================================
// Accept-side controls: the §22.16 capture taxonomy admits copyable
// operands, and this fix MUST NOT regress those paths.
// =========================================================================

// §22.16 "value, record, enum, `string`, and function-typed operands ... are
// captured by copy": an `old(int)` is the prototypical admitted case — no
// rejection, the program compiles, and an `ensures(...)` over `old(x)`
// evaluates normally. (`old` is itself an `ensures`-only contextual keyword
// per §22.16 — `requires`-clause use is rejected even for admitted types,
// so an `ensures`-shaped accept control avoids contaminating the signal.)
//
// Pre-fix acceptance of this shape is the test's purpose: pin that the new
// rejection does not over-reject the common primitive case.
BEZEL_TEST(contract_clause_old_move_only_rejected_3113,
           old_over_int_in_ensures_still_admitted) {
    auto r = runAot(
        "module M;\n"
        "public class Helper\n"
        "  public static method Increment(x: int): int\n"
        "    ensures(result = old(x) + 1)\n"
        "    return x + 1;\n"
        "  end Increment;\n"
        "end Helper;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Helper.Increment(5));\n"
        "  end Main;\n"
        "end Program;\n");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}
