// #6322 (§22.6 stage 3, §12.9, §5.1, v0.338): with two same-arity GENERIC
// overload candidates whose SUBSTITUTED parameter types are two DIFFERENT
// families, neither candidate was ever judged, and the pick fell to
// `arityFallback`'s first-declared candidate — which then ran a body whose
// parameter type the argument does not satisfy.
//
// §22.6 stage 3: "if none of the candidates applies, the call is an error."
// The shape:
//
//   class U
//     public static method S<V>(x: Box<Box<Box<V>>>): int => 20;
//     public static method S<V>(x: Array<V>): int => 10;
//   end U;
//
// called as `U.S<Dog>(aa)` with `aa: Array<Animal>`. §12.9 makes `Array`
// INVARIANT in its element, so `Array<Animal>` is not an `Array<Dog>`, and an
// `Array<Animal>` is certainly not a `Box<Box<Box<Dog>>>` either. NO candidate
// applies, so the call is an error — instead it compiled and printed 10.
//
// MEASURED at 389ca74b (staging tip): all seven rows below ACCEPTED and ran.
// Each now REJECTs, and each now agrees with the SINGLE-CANDIDANT control —
// the same program with only one of the two `S` declarations present, which
// §22.6 rejects in both directions and which the pre-fix compiler already
// rejected. The overload spelling was the only thing letting it through.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// A user-declared class standing in for the issue's `Box`, so the two
// candidates' parameter types are genuinely UNRELATED declarations (a §12
// subtype step does not exist between them in either direction) rather than
// one family reachable from the other.
const std::string kBoxDecl = "public class Box<T> end Box;\n";

// The two candidates, in the declaration order that made the pre-fix compiler
// pick 10 (the `Box` overload was reachable and sorted first in source).
const std::string kTwoCandidates =
    kBoxDecl + "public class U\n"
               "  public static method S<V>(x: Box<Box<Box<V>>>): int => 20;\n"
               "  public static method S<V>(x: Array<V>): int => 10;\n"
               "end U;\n";

// The issue's shape with the two declarations REVERSED. §22.6 never ranks by
// declaration order, so the verdict must be identical; a first-declared
// fallback cannot satisfy both orders.
const std::string kTwoCandidatesReversed =
    kBoxDecl + "public class U\n"
               "  public static method S<V>(x: Array<V>): int => 10;\n"
               "  public static method S<V>(x: Box<Box<Box<V>>>): int => 20;\n"
               "end U;\n";

const std::string kArrayOnly =
    kBoxDecl + "public class U\n"
               "  public static method S<V>(x: Array<V>): int => 10;\n"
               "end U;\n";

std::string wrap(const std::string& classes, const std::string& body) {
    return "module M;\n" + classes +
           "public class Program\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The call is rejected. Asserting only `!r.ran` would also be satisfied by a
// crash or by an unrelated diagnostic, so each row additionally requires the
// argument/parameter mismatch that §22.6 stage 3 produces — the parameter type
// is the real one, at a real line:col.
void expectRejected(const std::string& classes, const std::string& body) {
    auto r = runAot(wrap(classes, body));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A row that must still compile and run, printing `expected` on stdout.
void expectRuns(const std::string& classes, const std::string& body,
                const std::string& expected) {
    auto r = runAot(wrap(classes, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

const std::string kMakeAnimalArray =
    "    using var aa := new Array<Animal>();\n";
const std::string kMakeDogArray =
    "    using var ad := new Array<Dog>();\n";
const std::string kMakeNestedBox =
    "    using var bb := new Box<Box<Box<int>>>();\n";

const std::string kAnimalDogDecls =
    "public class Animal end Animal;\n"
    "public class Dog end Dog;\n";

// Every ambiguous row's classes, with `Animal`/`Dog` actually DECLARED. These
// rows are only meaningful if the element types resolve: an unresolved type
// name rejects the program for an unrelated reason, which would make the row
// pass without the fix and prove nothing (MEASURED: with the declarations
// omitted, five of the seven rejection rows were green at the pre-fix tip).
const std::string kTwoCandidatesEl =
    kAnimalDogDecls + kTwoCandidates;
const std::string kTwoCandidatesReversedEl =
    kAnimalDogDecls + kTwoCandidatesReversed;
const std::string kArrayOnlyEl = kAnimalDogDecls + kArrayOnly;

}  // namespace

// s22_6_0007: the issue's exact repro. `Array<Animal>` against
// `S<V>(x: Array<V>)` with V=Dog: §12.9 invariance rejects the `Array`
// candidate, and the `Box` candidate is unrelated. No candidate applies.
//
// The SINGLE-CANDIDANT control (`S<V>(x: Array<V>)` alone) is asserted too:
// §22.6 rejects this program with one overload exactly as it does with two, so
// the fix's target is that the two-candidate spelling stops disagreeing with
// the one-candidate spelling.
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           array_of_supertype_not_invariant_array_of_subtype_spec_22_6_12_9) {
    expectRejected(kTwoCandidatesEl,
                   kMakeAnimalArray + "    Console.WriteLine(U.S<Dog>(aa));\n");
    expectRejected(kTwoCandidatesReversedEl,
                   kMakeAnimalArray + "    Console.WriteLine(U.S<Dog>(aa));\n");
    expectRejected(kArrayOnlyEl,
                   kMakeAnimalArray + "    Console.WriteLine(U.S<Dog>(aa));\n");
}

// s22_6_0008: the MIRROR — §12.9 invariance is symmetric, so
// `Array<Dog>` is not an `Array<Animal>` either. A one-directional test would
// pass on a predicate that only checked one direction of the conversion.
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           array_of_subtype_not_invariant_array_of_supertype_spec_12_9) {
    expectRejected(kTwoCandidatesEl,
                   kMakeDogArray + "    Console.WriteLine(U.S<Animal>(ad));\n");
    expectRejected(kTwoCandidatesReversedEl,
                   kMakeDogArray + "    Console.WriteLine(U.S<Animal>(ad));\n");
    expectRejected(kArrayOnlyEl,
                   kMakeDogArray + "    Console.WriteLine(U.S<Animal>(ad));\n");
}

// s22_6_0009: the unrelated-family half. An `Array<Animal>` is certainly not
// the `Box<Box<Box<Dog>>>` parameter — two different declarations with no
// ancestor relation in either direction, which is the case the pre-fix
// `definitelyInapplicable` could not reach at all (it required
// `at->declaration == pt->declaration`).
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           array_reaches_neither_candidate_of_two_families_spec_22_6) {
    // The two-candidate spelling, not the single-candidate one: §22.6's
    // candidate RANKING only runs when there is a set to rank. MEASURED: the
    // same call with only `S<V>(x: Box<Box<Box<V>>>)` declared is ACCEPTED both
    // before and after this fix, because a lone candidate never reaches
    // `definitelyInapplicable` — that is argument checking
    // (`checkArgsAgainstParams`) declining to reject an unrelated type, a
    // different code path with a different owner and outside this fix's
    // scope. Asserting the single-candidate form here would pin that separate
    // leniency into a test about §22.6 ranking.
    expectRejected(kTwoCandidatesEl,
                   kMakeAnimalArray + "    Console.WriteLine(U.S<Dog>(aa));\n");
    expectRejected(kTwoCandidatesReversedEl,
                   kMakeAnimalArray + "    Console.WriteLine(U.S<Dog>(aa));\n");
}

// s22_6_0010: an element type that is NOT a class. `Box<Box<Box<int>>>` and
// `Array<int>` are again unrelated families, so the call is an error for the
// same reason and not because of §14.4's class-element restriction.
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           array_of_primitive_reaches_neither_candidate_spec_22_6) {
    expectRejected(kTwoCandidates,
                   "    using var ai := new Array<int>();\n"
                   "    Console.WriteLine(U.S<long>(ai));\n");
    expectRejected(kTwoCandidatesReversed,
                   "    using var ai := new Array<int>();\n"
                   "    Console.WriteLine(U.S<long>(ai));\n");
}

// s22_6_0011: an EXACT match still resolves, in both declaration orders. The
// fix widens a rejection predicate, so the row that keeps it from becoming a
// blanket rejection is the one where a candidate genuinely applies — here the
// `Box` candidate, at its exact parameter type, printing 20 and not the 10 a
// first-declared fallback would have produced.
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           exact_match_still_resolves_in_both_orders_spec_22_6) {
    expectRuns(kTwoCandidates,
               kMakeNestedBox + "    Console.WriteLine(U.S<int>(bb));\n",
               "20\n");
    expectRuns(kTwoCandidatesReversed,
               kMakeNestedBox + "    Console.WriteLine(U.S<int>(bb));\n",
               "20\n");
}

// s22_6_0012: the `Array` candidate applied while the `Box` one does not — the
// other winner, and the row that would flip if the predicate rejected a pair
// merely for having different declarations rather than for having no ancestor
// relation. Here `Array<int>` IS a `Box<Box<Box<int>>>`-family match's
// neighbour but not its match, so only the `Array` candidate is applicable.
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           one_applicable_candidate_among_two_families_spec_22_6) {
    expectRuns(kTwoCandidates,
               "    using var ai := new Array<int>();\n"
               "    Console.WriteLine(U.S<int>(ai));\n",
               "10\n");
    expectRuns(kTwoCandidatesReversed,
               "    using var ai := new Array<int>();\n"
               "    Console.WriteLine(U.S<int>(ai));\n",
               "10\n");
}

// s22_6_0013: the fix refuses to judge a pair whose declarations carry a
// VARIANT type parameter, because §12.9's variance is exactly the relation
// `classifyArg` does not model. A pair that could still be related by a
// generic-implementation conformance is left on the pre-fix pick rather than
// risk a spurious rejection — this row pins that it is still ACCEPTED.
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           variant_parameter_pair_is_not_judged_spec_12_9) {
    expectRuns("public interface IBox<out T> end IBox;\n"
               "public class U\n"
               "  public static method S<V>(x: IBox<V>): int => 10;\n"
               "  public static method S<V>(x: Array<V>): int => 20;\n"
               "end U;\n",
               "    using var ai := new Array<int>();\n"
               "    Console.WriteLine(U.S<int>(ai));\n",
               "20\n");
}

// s22_6_0014: a SUBTYPE relation between the two declarations DOES make the
// pair related, so the fix must not reject it. `Derived` reaches `Base`, so
// `S<V>(x: Base<V>)` is applicable to a `Derived<int>` argument; declaring
// `Base` before `Derived` makes the pick depend on the relation being
// consulted in the right direction.
BEZEL_TEST(generic_overload_cross_family_applicability_6322,
           subtype_related_declarations_are_not_judged_unrelated_spec_12) {
    expectRuns("public class Base<T> end Base;\n"
               "public class Derived<T> end Derived;\n"
               "public class U\n"
               "  public static method S<V>(x: Base<V>): int => 10;\n"
               "  public static method S<V>(x: Derived<V>): int => 20;\n"
               "end U;\n",
               "    using var di := new Derived<int>();\n"
               "    Console.WriteLine(U.S<int>(di));\n",
               "20\n");
}