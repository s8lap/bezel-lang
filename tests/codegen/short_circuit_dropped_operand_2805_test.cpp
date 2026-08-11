// Regression tests for #2805 — an operand of a short-circuit operator that
// FAILED TO LOWER degraded in two different wrong ways depending on its
// position, and both were reachable from one underlying condition.
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §8.6 (v0.240): "When a value type is used
// where `Object` (or an interface implemented by the value type) is expected,
// the value is **boxed**".  When codegen cannot mint a runtime identity for
// such a value, the type test against it has no descriptor to compare and
// `lowerTypeTest` DROPS it, reporting the #2775 warning.  A dropped test reads
// as `false` — that is what `lowerIf` already does with a null condition, and
// #2775 names it.  Which value types are in that state changes over time; see
// the maintenance note below for the one pinned here and why.
//
// THE DEFECT.  `lowerShortCircuit` propagated the null outward:
//
//   * a null LEFT operand made the whole `and`/`or` expression null, which
//     `lowerIf` then turned into a silent `false` — so a disjunction with a
//     TRUE operand answered false.  MEASURED pre-fix, on the shape pinned AT
//     THE TIME (`[3]int`, before #2804 made it boxable — these two lines are a
//     historical measurement and are deliberately NOT rewritten to match the
//     current pin):
//         `o is [3]int or oi is int`          -> false, required true
//         `not (o is [3]int) and oi is int`   -> false, required true
//     The `not` case is the sharper one: the whole `not (...)` was degraded to
//     `false`, i.e. the WRONG POLARITY, because `lowerUnary` also returned null
//     and the degrade happened one level too high.
//
//   * a null RIGHT operand returned after `sc.rhs` had already been branched
//     into, leaving `sc.rhs` and `sc.end` with no terminator:
//         Basic Block in function 'main' does not have terminator!
//         label %sc.end
//         bezel codegen: IR verification failed
//     i.e. the compiler ABORTED for the operand position while silently
//     answering wrong for the other.
//
// THE FIX degrades the operand itself to a well-formed `false` (and negates it
// for `not`, preserving polarity).  `false` is not an arbitrary choice: it is
// exactly what the same dropped test already reads as when it is NOT part of a
// chain, so the chained and unchained spellings now agree instead of the chain
// being accidentally right in one shape and wrong in the others.
//
// ⚠ MAINTENANCE NOTE — AND IT HAS NOW FIRED ONCE, EXACTLY AS WRITTEN.
// These tests need an expression that fails to lower.  The original pin used a
// non-owning inline array, because `[3]int` was unboxed when this landed.
// #2804 then gave non-owning inline arrays their §8.6 boxing, `o is [2](int) -> int`
// became genuinely TRUE, and four of these cases went red in the gate — a LOUD
// failure, which is what the note below predicted and the reason it was worth
// writing.  Re-pointed rather than deleted, per its own instruction.
//
// The pin now sits on the category that remains unboxed, and durably so: an
// inline array of runtime-ERASED elements.  §15.1 admits function values as
// elements, but §12.9 / #2012 make two function types runtime-
// indistinguishable, so no box identity may be minted for `[2](int) -> int` —
// minting one would collapse it and `[2](string) -> string` onto a single
// descriptor, the #2001 type-confusion class.  This is the same shape
// tests/lit/type_test_dropped_warns_2775.bzl was re-pointed at by #2804's lane,
// deliberately: the two tests pin the same dropped-lowering condition from
// different angles (that one asserts the DIAGNOSTIC, this one the VALUE), so
// they should move together.
//
// If a later change ever reifies function types, these go red again and the
// right response is the same: re-point at whatever value-type category remains
// unboxed (`unit`, §4.7 / §8.6 "Boxing of the unit value", is the next one),
// NOT to delete the assertions.  They stay correct either way — a newly boxable
// operand changes the expected values, which fails loudly rather than silently.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// Deliberately runAot, not runAotChecked: the programs below intentionally
// contain a value in an `Object` slot that codegen cannot box, which leaks.
// Using the checked variant would redden on that unrelated defect.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// `o` holds a value §8.6 requires to be boxed and codegen cannot mint an
// identity for (see the maintenance note), so `o is [2](int) -> int` is the
// DROPPED test.  `oi` is an ordinary boxed int, so `oi is int` is genuinely
// TRUE and can distinguish "the chain evaluated" from "the chain was poisoned
// to false" — without it, every assertion below would hold vacuously for a
// chain that simply always answers false.
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int "
           "  var fs: [2](int) -> int := "
           "      [lambda(x: int) => x, lambda(x: int) => x + 1]; "
           "  var o: Object := fs; "
           "  var oi: Object := 7; " +
           body +
           " end Main; end C;";
}
} // namespace

// The `or` chain: a dropped LEFT operand must read as a false DISJUNCT, not
// poison the disjunction.  Pre-fix: 0.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           dropped_left_operand_of_or_is_a_false_disjunct_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  if o is [2](int) -> int or oi is int then return 8; end; "
        "  return 0;")), 8);
}

// `not` of a dropped test must be TRUE — the polarity the pre-fix degrade got
// backwards by degrading the whole `not (...)` instead of its operand.
// Pre-fix: 0.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           not_of_a_dropped_test_is_true_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  if not (o is [2](int) -> int) and oi is int then return 4; end; "
        "  return 0;")), 4);
}

// Both together, as the issue reported them.  Pre-fix: 0.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           or_and_negated_chains_combined_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  var code := 0; "
        "  if not (o is [2](int) -> int) and oi is int then code := code + 4; end; "
        "  if o is [2](int) -> int or oi is int then code := code + 8; end; "
        "  return code;")), 12);
}

// The IR-verification failure: a dropped test in a NON-FIRST conjunct left
// `sc.rhs`/`sc.end` unterminated.  Pre-fix this did not run at all — the
// COMPILE aborted.  The value is 0 (the conjunction is correctly false); what
// this pins is that it compiles and runs.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           dropped_test_in_a_non_first_conjunct_compiles_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  var code := 0; "
        "  if oi is int and o is [2](int) -> int and oi is int then code := 1; end; "
        "  return code;")), 0);
}

// ACCEPT-CONTROL: a dropped test as a plain `and` conjunct is false, which is
// the one shape that was accidentally right before the fix.  It must STAY
// right — a degrade that flipped to `true` would break it.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           dropped_left_operand_of_and_stays_false_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  if o is [2](int) -> int and oi is int then return 1; end; "
        "  return 0;")), 0);
}

// ACCEPT-CONTROL: ordinary short-circuit logic on operands that lower normally
// is untouched, including the skip semantics (`and` must not evaluate its RHS
// when the LHS is false, `or` must not when the LHS is true).  Without this a
// degrade that fired too eagerly would go unnoticed.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           ordinary_short_circuit_semantics_unchanged_spec_17_7) {
    BEZEL_EXPECT_EQ(run(prog(
        "  var n := 0; "
        "  if oi is int and oi is int then n := n + 1; end; "
        "  if oi is string and oi is int then n := n + 2; end; "
        "  if oi is string or oi is int then n := n + 4; end; "
        "  if oi is string or oi is string then n := n + 8; end; "
        "  if not (oi is string) then n := n + 16; end; "
        "  if not (oi is int) then n := n + 32; end; "
        "  return n;")), 21);
}
