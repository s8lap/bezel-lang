// Regression tests for #2805 — an operand of a short-circuit operator that
// FAILED TO LOWER degraded in two different wrong ways depending on its
// position, and both were reachable from one underlying condition.
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §8.6 (v0.240): "When a value type is used
// where `Object` (or an interface implemented by the value type) is expected,
// the value is **boxed**", and its "Boxing of inline arrays" paragraph:
// "Non-owning inline arrays (`[N]T` and `[N]T?`) are ordinary value types and
// box per the general rule."  Codegen does not box them yet (#2804), so a type
// test against `[3]int` has no runtime descriptor and `lowerTypeTest` DROPS it,
// reporting the #2775 warning.  A dropped test reads as `false` — that is what
// `lowerIf` already does with a null condition, and #2775 names it.
//
// THE DEFECT.  `lowerShortCircuit` propagated the null outward:
//
//   * a null LEFT operand made the whole `and`/`or` expression null, which
//     `lowerIf` then turned into a silent `false` — so a disjunction with a
//     TRUE operand answered false.  MEASURED pre-fix:
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
// ⚠ MAINTENANCE NOTE.  These tests need an expression that fails to lower, and
// they use a non-owning inline array because #2804 leaves that unboxed.  If
// inline arrays gain §8.6 boxing, these stop exercising the degrade path and
// will pass vacuously — re-point them at whatever value-type category remains
// unboxed rather than deleting them.  The assertions themselves stay correct
// either way: with boxing, `o is [3]int` becomes genuinely true and the
// expected values below would change, which is a loud failure, not a silent
// one.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// Deliberately runAot, not runAotChecked: the programs below intentionally
// contain an unboxed inline array in an `Object` slot, which #2804 leaks.
// Using the checked variant would redden on that unrelated defect.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// `o` holds a value §8.6 requires to be boxed and codegen does not box, so
// `o is [3]int` is the dropped test.  `oi` is an ordinary boxed int, so
// `oi is int` is genuinely TRUE and can distinguish "the chain evaluated" from
// "the chain was poisoned to false".
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int "
           "  var ia: [3]int := [1, 2, 3]; "
           "  var o: Object := ia; "
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
        "  if o is [3]int or oi is int then return 8; end; "
        "  return 0;")), 8);
}

// `not` of a dropped test must be TRUE — the polarity the pre-fix degrade got
// backwards by degrading the whole `not (...)` instead of its operand.
// Pre-fix: 0.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           not_of_a_dropped_test_is_true_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  if not (o is [3]int) and oi is int then return 4; end; "
        "  return 0;")), 4);
}

// Both together, as the issue reported them.  Pre-fix: 0.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           or_and_negated_chains_combined_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  var code := 0; "
        "  if not (o is [3]int) and oi is int then code := code + 4; end; "
        "  if o is [3]int or oi is int then code := code + 8; end; "
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
        "  if oi is int and o is [3]int and oi is int then code := 1; end; "
        "  return code;")), 0);
}

// ACCEPT-CONTROL: a dropped test as a plain `and` conjunct is false, which is
// the one shape that was accidentally right before the fix.  It must STAY
// right — a degrade that flipped to `true` would break it.
BEZEL_TEST(short_circuit_dropped_operand_2805,
           dropped_left_operand_of_and_stays_false_spec_8_6) {
    BEZEL_EXPECT_EQ(run(prog(
        "  if o is [3]int and oi is int then return 1; end; "
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
