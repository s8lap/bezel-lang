// #6334 (§22.6 stage 3, §5.1, §14.5, v0.338) — a LONE overload candidate
// is still §22.6 stage 3's "applicability" subject: with one candidate and
// no ranking to run, an ARITY match is not applicability, and an argument the
// substituted parameter does not admit must be an error rather than a call
// that runs the wrong body.
//
// WHAT WAS BROKEN. `resolveMethodOverload` short-circuited on
// `cands.size() == 1` ABOVE the §14.5 phase-1 substitution and the
// substituted parameter rows, so a sole candidate returned before anything
// had substituted its signature. Nothing downstream re-checked it:
// `checkCallArity`'s per-argument `checkAssignable` compares against the
// candidate's UNSUBSTITUTED formal (a bare `V`), which a §5.1 check defers
// on; and #6312's guard reaches only the EXPLICIT type-argument list.
//
// MEASURED before the fix (this tree, `bezelc` from a clean HEAD):
//   * `U.S(a)` with `a: Array<int>` against `S<V>(x: Box<V>)` compiled, ran,
//     and printed the Box body's own result — the `Box<V>` field slot read
//     off an `Array<int>`. Compiled with `--sanitize=address` the dispatch
//     through that slot faults:
//       AddressSanitizer: SEGV on unknown address 0x000000000000
//       The signal is caused by a READ memory access.
//     pc points to the zero page — the `Array<int>` object's `Box<V>` slot
//     held a null, not a reference.
//   * the same hole on the `c.S(a)` INSTANCE spelling.
//   * and §14.5's Agreement paragraph: `S<V>(x: Array<V>, tag: V)` called as
//     `(Array<Dog>, Animal)` bound nothing and still ran the body.
//
// THE TWO OUTCOMES. With the substitution now computed before the
// short-circuit, a sole candidate reaches one of two checks:
//   * inference BOUND the signature — the call gets the check
//     `checkCallArity` gives a non-generic call, against the SUBSTITUTED row
//     (the same call #6312 makes for an explicit list); or
//   * inference bound NOTHING and the pair is decidably unrelated — §14.5
//     "a candidate whose inference fails is inapplicable and drops out", so
//     with one candidate none survives and §22.6 stage 3 makes the call an
//     error.
//
// WHY THE SECOND CHECK IS NARROW, AND WHAT IT LEAVES OPEN. "Inference bound
// nothing" is NOT by itself a failed match. MEASURED on this tree before the
// check was narrowed: `unifyArgSubst` binds only on a STRUCTURAL head match,
// never through a §12 subtype step, so it also leaves the map empty for
// `string` and for a user `class Str: IEnumerable<char>` against
// `S<V>(x: IEnumerable<V>)`. Both are valid, both compile today, and
// `explicit_type_arg_list_overload_6312.string_to_ienumerable_char_runs_spec_4_6`
// PINS the first — a fix keyed on the empty map breaks it, and a fix keyed on
// a hand-rolled "definitely inapplicable" over `implicitlyConvertible` breaks
// it differently (`hasNamedAncestor` does not resolve `Str`'s conformance for
// the declaration-less `IEnumerable<T>`, so it reports the VALID call as
// certainly inapplicable). MEASURED both rejections before rejecting them.
//
// So the check fires only where the verdict is certain without a ruling: the
// parameter's HEAD is a user-declared class or record and the argument reaches
// it in neither direction. §5.1's named-to-named conversions are identity, a
// §12 subtype step, or (from a primitive) boxing — boxing is out with both
// sides named, and a stdlib built-in cannot be a declared base of, nor declare
// a base of, a user's own class, so for a USER-declared parameter head the
// subtype step is decidable by declaration identity alone. A user-declared
// INTERFACE head is excluded: `Impl : IFace<int>` given to `S<V>(x: IFace<V>)`
// goes through that very subtype step and MEASURED runs, so it is left alone.
//
// WHAT REMAINS OPEN. Whether §14.5 inference is supposed to see through a
// §12 subtype step — which would close the `IEnumerable<V>`-from-a-subtype
// half generally, rather than only where the heads are unrelated — is a
// spec question, filed as #6342. This test pins the subset that does not
// depend on its answer.
//
// Every row is on `runAotChecked`, the ASan-checked AOT oracle. The reject
// rows must NOT run. The accept rows must run and print a value read out of
// the arguments, so a row whose body is a constant could not tell a working
// call from a mis-dispatched one.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// The §22.6 stage-3 needle this fix emits for an inference failure. Quoted
// from the source that emits it; a rename here is a behaviour flip for every
// reject row below.
const char* const kNoApplicableNeedle =
    "no type argument binds this argument to the candidate's parameter, "
    "and the two are unrelated";

// The pre-existing §14.1 Agreement diagnostic (`typeCall`, #896), which the
// #6334 check must NOT double-report.
const char* const kAgreementNeedle =
    "inconsistent type-argument inference: type parameter `V` cannot be bound "
    "to both `Dog` and a different type";

// `Box<T>` is the shape the issue's repro uses: a user generic class whose
// single field sits where `Array<int>`'s own layout puts something else.
const std::string kBox =
    "class Box<T>\n"
    "  public V: T := default(T);\n"
    "end Box;\n";

const std::string kMakeArray =
    "    using var a := new Array<int>();\n";

// Assert the call is REJECTED, and rejected with THIS fix's diagnostic — not
// by crashing, and not by some unrelated error.
void expectRejected(const std::string& body) {
    auto r = runAotChecked("module M;\n" + body);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoApplicableNeedle));
}

// Assert the call still COMPILES and runs, printing the expected value.
void expectRuns(const std::string& body, const std::string& expected) {
    auto r = runAotChecked("module M;\n" + body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// s22_6_0101: THE ISSUE'S REPRO. A lone `S<V>(x: Box<V>)` given an
// `Array<int>`. Inference binds nothing — `Array<int>` is not a `Box<V>` for
// any V — so the row stays unsubstituted, §14.5 drops the candidate, and with
// one candidate §22.6 stage 3 is an error. Before the fix this compiled, ran,
// and read the Box field slot off the array.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           lone_generic_candidate_box_v_rejects_array_spec_22_6_14_5) {
    expectRejected(
        kBox +
        "class U\n"
        "  public static method S<V>(x: Box<V>): int => 77;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" +
        kMakeArray +
        "    Console.WriteLine(U.S(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
}

// s22_6_0102: the INSTANCE spelling of the same hole (`c.S(a)`). §22.6
// collects the candidate set from the receiver's class hierarchy, so a
// static-only fix would leave this one open — constructed here rather than
// assumed.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           instance_receiver_lone_generic_rejects_array_spec_22_6_12_1) {
    expectRejected(
        kBox +
        "class C\n"
        "  public method S<V>(x: Box<V>): int => 88;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var c := new C();\n" +
        kMakeArray +
        "    Console.WriteLine(c.S(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
}

// s22_6_0103: §14.5 "Agreement". `Array<Dog>` binds V=Dog and `Animal` binds
// V=Animal, so inference fails — but by DISAGREEMENT, which `typeCall`'s
// #896 diagnostic already reports. This row pins that #6334 adds no second
// message for the same call: a doubled diagnostic at one location is a
// behaviour change of its own.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           agreement_conflict_keeps_the_single_896_diagnostic_spec_14_1) {
    auto r = runAotChecked(
        "module M;\n"
        "public class Animal end Animal;\n"
        "public class Dog: Animal end Dog;\n"
        "class U\n"
        "  public static method S<V>(x: Array<V>, tag: V): int => 7;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<Dog>();\n"
        "    using var an := new Animal();\n"
        "    Console.WriteLine(U.S(a, an));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAgreementNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoApplicableNeedle));
}

// s22_6_0104: BOUNDARY — the same lone `S<V>(x: Box<V>)` given an actual
// `Box<int>` must still apply. This is the row that would go green on a fix
// that rejected every sole candidate; it prints the value STORED in the box,
// so it cannot pass unless the call really ran the body on its argument.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           lone_generic_candidate_box_v_still_applies_to_box_int_spec_22_6) {
    expectRuns(
        kBox +
        "class U\n"
        "  public static method S<V>(x: Box<V>): int => 1;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var b := new Box<int>();\n"
        "    b.V := 41;\n"
        "    Console.WriteLine(U.S(b) + b.V);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "42\n");
}

// s22_6_0105: BOUNDARY — a §12 SUBTYPE step still applies. `Array<int>`
// reaches `IEnumerable<int>` (stdlib §4.1), so `S<V>(x: IEnumerable<V>)`
// is applicable and must keep compiling. This is the row that fails on a
// fix which compares instantiation head names without consulting the real
// §5.1 relation — MEASURED as the failure mode of the hand-rolled
// predicate this fix deliberately does not use.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           subtype_step_to_enumerable_still_applies_spec_12_1_5_1) {
    expectRuns(
        "class U\n"
        "  public static method S<V>(x: IEnumerable<V>): int => 1;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" +
        kMakeArray +
        "    Console.WriteLine(U.S(a) + a.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "1\n");
}

// s22_6_0106: BOUNDARY — the identity arm, and a substituted row the call
// READS. `S<V>(x: Array<V>)` given `Array<int>` binds V=int, and the row
// prints `a.Count`, which only a genuinely substituted call can satisfy.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           substituted_array_row_still_applies_spec_14_5_22_6) {
    expectRuns(
        "class U\n"
        "  public static method S<V>(x: Array<V>): int => 1;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" +
        kMakeArray +
        "    a.Add(6);\n"
        "    Console.WriteLine(U.S(a) + a.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "2\n");
}

// s22_6_0107: BOUNDARY — the Agreement twin of s22_6_0103. The same two
// declarations with arguments that AGREE must compile and run: a fix that
// rejected on "inference left the row unsubstituted" without checking the
// §14.1 failure would reject this too. The value printed is the count of
// the array the call actually received.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           agreeing_inference_still_applies_spec_14_1_14_5) {
    expectRuns(
        "public class Animal end Animal;\n"
        "public class Dog: Animal end Dog;\n"
        "class U\n"
        "  public static method S<V>(x: Array<V>, tag: V): int => 1;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<Dog>();\n"
        "    using var d := new Dog();\n"
        "    Console.WriteLine(U.S(a, d) + a.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "1\n");
}

// s22_6_0108: BOUNDARY — the USER-declared subtype step, which is the case a
// head-name comparison gets wrong in the other direction. `Sub<int>` reaches
// `Box<int>` by §12.1, so it reaches `Box<V>` with V=int even though
// `unifyArgSubst` binds nothing (the heads `Sub` and `Box` differ structurally).
// This row is what makes the check's "neither direction reaches" clause
// load-bearing: drop it and this valid call is rejected alongside the repro.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           user_declared_subtype_reaches_the_generic_parameter_spec_12_1) {
    expectRuns(
        kBox +
        "class Sub<T>: Box<T>\n"
        "end Sub;\n"
        "class U\n"
        "  public static method S<V>(x: Box<V>): int => 10;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := new Sub<int>();\n"
        "    Console.WriteLine(U.S(s));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "10\n");
}

// s22_6_0109: the check is not specific to `Box`, and it reaches the
// declaration-less argument type. `Array<int>` cannot be a `Base<V>` for any
// V — a stdlib built-in collection type is neither a declared base of, nor
// declares a base of, a user's own class — so this is the same rejection as
// s22_6_0101 by a different pair. It is the row that shows the narrowing is a
// decision procedure and not a `Box`-shaped special case.
BEZEL_TEST(lone_candidate_unrelated_argument_6334,
           lone_generic_against_unrelated_user_class_rejects_array_spec_5_1) {
    expectRejected(
        "class Base<T>\n"
        "end Base;\n"
        "class U\n"
        "  public static method S<V>(x: Base<V>): int => 10;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" +
        kMakeArray +
        "    Console.WriteLine(U.S(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
}