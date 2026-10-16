// #6332 (§22.6 "Generic candidates", §14.5, v0.338) — §22.6's LAST
// tie-break before ambiguity: "When a non-generic candidate and a generic
// candidate are both applicable, and neither the ranking nor the
// defaulted-arity tie-break separates them, the non-generic candidate is
// selected." The rule exists; on the `Undecidable` path it never ran.
//
// WHY `Undecidable` IS REACHED AT ALL. §22.6's ranking needs a decidable
// argument type, and `resolveOverload`'s `judgeable` predicate admits a
// `TypeRef::Kind::Named` only when `t->declaration != nullptr`. A built-in
// class reference such as `Array<int>` is declaration-less (§2.3/§31.4), so
// it fails `judgeable`, `resolveOverload` returns `Undecidable` with
// `applicable` all false WITHOUT evaluating a single `classifyArg` cell, and
// `resolveMethodOverload`'s `Undecidable` arm falls through to
// `return initial` — `lookupMember`'s FIRST-IN-SOURCE candidate. So the
// verdict was decided by declaration order.
//
// MEASURED before the fix, on this tree: `Pick<V>(x: V)` declared first and
// `Pick(x: Array<int>)` declared second, called as `U.Pick(a)` with
// `a: Array<int>`, printed 10 — the generic. With the two declarations
// SWAPPED the identical call printed 5. §22.6 requires 5 in both, because the
// generic candidate's substituted signature `(Array<int>)` COINCIDES with
// the non-generic declaration's, so neither is strictly better and the
// non-generic preference is exactly the comparison that decides it.
//
// Each row below calls the SAME call in BOTH declaration orders and requires
// the same answer. A first-in-source pick passes one order and fails the
// other, so the pair is the test.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The two candidates, in the two DECLARATION ORDERS. §22.6's answer must not
// depend on this order; each row spells both out rather than asserting one.
const std::string kGenericFirst =
    "class U\n"
    "  public static method Pick<V>(x: V): int => 10;\n"
    "  public static method Pick(x: Array<int>): int => 5;\n"
    "end U;\n";

const std::string kNonGenericFirst =
    "class U\n"
    "  public static method Pick(x: Array<int>): int => 5;\n"
    "  public static method Pick<V>(x: V): int => 10;\n"
    "end U;\n";

const std::string kMakeArray =
    "    using var a := new Array<int>();\n"
    "    a.Add(3);\n";

// The call under test. The expected value is 5 in BOTH orders: the non-generic
// `Pick(x: Array<int>)`, which prints its own constant, not the generic's 10.
void expectNonGenericInBothOrders() {
    for (const std::string& classes : {kGenericFirst, kNonGenericFirst}) {
        auto r = runAotChecked(
            "module M;\n" + classes +
            "public class Program\n"
            "  public static method Main(): int\n" + kMakeArray +
            "    Console.WriteLine(U.Pick(a));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
    }
}

}  // namespace

// s22_6_0101: THE ISSUE'S REPRO, both orders. This row is the fail-without
// proof: without the fix the generic-first order prints 10 and this fails.
BEZEL_TEST(non_generic_preference_undecidable_6332,
           generic_last_declared_still_loses_to_non_generic_spec_22_6) {
    expectNonGenericInBothOrders();
}

// s22_6_0102: the INSTANCE spelling. §22.6 collects the candidate set from the
// receiver's class hierarchy (§12.1), so the fix must reach `c.Pick(a)` and
// not only the static form. Constructed here rather than assumed.
BEZEL_TEST(non_generic_preference_undecidable_6332,
           instance_receiver_same_verdict_in_both_orders_spec_12_1) {
    const std::string kGenericFirstInst =
        "class U\n"
        "  public method Pick<V>(x: V): int => 10;\n"
        "  public method Pick(x: Array<int>): int => 5;\n"
        "end U;\n";
    const std::string kNonGenericFirstInst =
        "class U\n"
        "  public method Pick(x: Array<int>): int => 5;\n"
        "  public method Pick<V>(x: V): int => 10;\n"
        "end U;\n";
    for (const std::string& classes : {kGenericFirstInst,
                                       kNonGenericFirstInst}) {
        auto r = runAotChecked(
            "module M;\n" + classes +
            "public class Program\n"
            "  public static method Main(): int\n"
            "    using var u := new U();\n" + kMakeArray +
            "    Console.WriteLine(u.Pick(a));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
    }
}

// s22_6_0103: BOUNDARY — a candidate set with NO non-generic candidate must be
// completely unchanged. The fix's block requires exactly one non-generic
// candidate of the right arity; with only generic candidates in the set it
// finds none and the arm takes its pre-existing path. `U.Pick(a)` here has a
// single generic candidate `Pick<V>(x: V)`, which applies and must print 10.
// A fix that picked "the only candidate" instead of "the only NON-generic
// candidate" would print 5 here.
BEZEL_TEST(non_generic_preference_undecidable_6332,
           all_generic_candidate_set_is_unchanged_spec_22_6) {
    auto r = runAotChecked(
        "module M;\n"
        "class U\n"
        "  public static method Pick<V>(x: V): int => 10;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" + kMakeArray +
        "    Console.WriteLine(U.Pick(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "10\n");
}

// s22_6_0104: BOUNDARY — TWO non-generic candidates is a genuine tie, and
// §22.6's non-generic preference does not separate it: with no ranking
// applicable the pre-existing first-in-source path stands. This row pins that
// the fix declines to guess here rather than collapsing the tie. It also pins
// that the candidates are DISTINCT in arity from nothing else, so the row is
// about the two-non-generic case alone.
BEZEL_TEST(non_generic_preference_undecidable_6332,
           two_non_generic_candidates_still_take_the_pre_existing_path_spec_22_6) {
    auto r = runAotChecked(
        "module M;\n"
        "class U\n"
        "  public static method Pick(x: Array<int>): int => 5;\n"
        "  public static method Take(x: Array<int>): int => 5;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" + kMakeArray +
        "    Console.WriteLine(U.Pick(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// s22_6_0105: BOUNDARY — a non-generic candidate the ARGUMENT CANNOT REACH
// must not be preferred. `Pick(x: string)` is non-generic and has the right
// arity, but `Array<int>` is not a `string` and does not reach it, so §5.1
// forbids selecting it. §22.6's non-generic preference only ever chooses
// BETWEEN APPLICABLE candidates, so the preference must decline and the
// generic candidate — the one that does apply — must still win, printing 10.
// This row is the guard that keeps the fix additive: the admissibility test
// has to run BEFORE the preference is allowed to fire, or the fix would
// prefer an unreachable non-generic candidate and call a `string` parameter
// with an array.
BEZEL_TEST(non_generic_preference_undecidable_6332,
           non_generic_candidate_the_argument_cannot_reach_does_not_win_spec_5_1) {
    auto r = runAotChecked(
        "module M;\n"
        "class U\n"
        "  public static method Pick(x: string): int => 5;\n"
        "  public static method Pick<V>(x: V): int => 10;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" + kMakeArray +
        "    Console.WriteLine(U.Pick(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "10\n");
}