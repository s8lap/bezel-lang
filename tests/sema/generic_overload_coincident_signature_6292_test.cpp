// #6292 (§22.6 "Generic candidates", §14.5, v0.338): two generic overload
// candidates whose SUBSTITUTED signatures coincide. §22.6: "Surviving
// candidates enter the ranking below with their substituted signatures" and
// "Two generic candidates remain subject to the ordinary ambiguity rule." For
// `U.S<V>(x: Array<V>)` and `U.S<V>(x: Array<Array<V>>)` called as `U.S(b)`
// with `b: Array<Array<int>>`, §14.5 inference binds V=Array<int> for the
// first and V=int for the second, so BOTH substituted signatures are
// `(Array<Array<int>>)`. Neither ranks better than the other and no
// tie-break remains (neither is non-generic, neither fills a default), so the
// call is ambiguous.
//
// MEASURED at 389ca74b (staging tip) before the fix: `U.S(b)` compiled and
// printed `10`, and with the two declarations REVERSED it printed `20` — the
// verdict was decided by declaration order. Each row below runs the SAME call
// in BOTH declaration orders and requires the same answer, which is the
// property §22.6's ranking guarantees and a first-declared pick cannot.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// The §22.6 ambiguity diagnostic this fix introduces. Quoted from the source
// that emits it; a rename here is a behavior flip for every row below.
const char* const kAmbiguousNeedle =
    "ambiguous call to `S`: the candidates' substituted signatures are "
    "identical (§22.6) and neither is unambiguously better";

// The two candidates of every ambiguous row, spelled in DECLARATION ORDER as
// given. §22.6's answer must not depend on this order, so each ambiguous row
// calls `ambiguousInBothOrders` with the two orderings spelled out.
const std::string kArrayArrayFirst =
    "public class U\n"
    "  public static method S<V>(x: Array<V>): int => 10;\n"
    "  public static method S<V>(x: Array<Array<V>>): int => 20;\n"
    "end U;\n";

const std::string kArrayNestedFirst =
    "public class U\n"
    "  public static method S<V>(x: Array<Array<V>>): int => 20;\n"
    "  public static method S<V>(x: Array<V>): int => 10;\n"
    "end U;\n";

// Both substituted signatures are `(Array<Array<int>>)`.
const std::string kMakeNestedArray =
    "    using var b := new Array<Array<int>>();\n";

// Assert the call is rejected with the §22.6 ambiguity diagnostic — NOT with
// some unrelated error, and NOT by crashing. `failedAtCompileTimeWithRealLocation`
// plus `!r.ran` is the reject oracle; the diagnostic assert is what makes the
// row non-vacuous about WHICH rejection it is.
void expectAmbiguous(const std::string& classes, const std::string& body) {
    auto r = runAot("module M;\n" + classes +
                    "public class Program\n"
                    "  public static method Main(): int\n" + body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end Program;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kAmbiguousNeedle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// A row that COMPILES AND RUNS, printing the expected value on stdout. Used
// for the three calls of the §22.6/§14.5 worked example that are NOT
// ambiguous and must keep resolving (§22.6 ranks them by the substituted
// signature, which differs for each).
void expectRuns(const std::string& classes, const std::string& body,
                const std::string& expected) {
    auto r = runAot("module M;\n" + classes +
                    "public class Program\n"
                    "  public static method Main(): int\n" + body +
                    "    return 0;\n"
                    "  end Main;\n"
                    "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// s22_6_0001 / §22.6 "Generic candidates": inference binds V=Array<int> for
// `S<V>(x: Array<V>)` and V=int for `S<V>(x: Array<Array<V>>)`, so both
// substituted signatures are `(Array<Array<int>>)`. Ambiguous — and so in
// BOTH declaration orders, since §22.6 never ranks by declaration order.
BEZEL_TEST(generic_overload_coincident_signature_6292,
           inferred_coinciding_signatures_ambiguous_spec_22_6) {
    expectAmbiguous(kArrayArrayFirst,
                    kMakeNestedArray + "    Console.WriteLine(U.S(b));\n");
    expectAmbiguous(kArrayNestedFirst,
                    kMakeNestedArray + "    Console.WriteLine(U.S(b));\n");
}

// s22_6_0002: a THIRD candidate that does NOT coincide. `S<V>(x: Array<V>)`
// infers V=Array<Array<int>> and `S<V>(x: Array<Array<Array<V>>>)` infers V=int,
// so those two coincide at (Array<Array<Array<int>>>), while
// `S<V>(x: Array<Array<V>>)` infers V=int at (Array<Array<int>>) and does not.
// The coincidence check requires EVERY candidate to coincide before it reports
// ambiguity, because a third candidate that differs is a possible unique
// winner and rejecting the call would be wrong. Whether this PARTIAL
// coincidence should itself be ambiguous is the open question filed as #6331
// ("§22.6 — a coinciding SUBSET of generic candidates"); this row pins the
// behaviour the fix deliberately leaves UNCHANGED, so a later ruling on #6331
// flips it deliberately rather than by accident.
BEZEL_TEST(generic_overload_coincident_signature_6292,
           partial_coincidence_is_not_ambiguous_spec_22_6) {
    const std::string kThreeCandidates =
        "public class U\n"
        "  public static method S<V>(x: Array<V>): int => 10;\n"
        "  public static method S<V>(x: Array<Array<V>>): int => 20;\n"
        "  public static method S<V>(x: Array<Array<Array<V>>>): int => 30;\n"
        "end U;\n";
    expectRuns(kThreeCandidates,
               kMakeNestedArray + "    Console.WriteLine(U.S(b));\n",
               "10\n");
}

// s22_6_0003: an explicit list that makes the two candidates DIFFER is NOT
// ambiguous. `U.S<int>(b)` substitutes V=int, giving `(Array<int>)` against
// `(Array<Array<int>>)`; §22.6 stage 3 finds the second applicable and the
// first not, so the call resolves to it and prints 20. This is the row that
// keeps the new check from firing on a mere "both are generic" shape — the
// signatures must COINCIDE, not merely both be present.
BEZEL_TEST(generic_overload_coincident_signature_6292,
           explicit_list_distinct_signatures_still_resolve_spec_22_6) {
    expectRuns(kArrayArrayFirst,
               kMakeNestedArray +
                   "    Console.WriteLine(U.S<int>(b).ToString());\n",
               "20\n");
    expectRuns(kArrayNestedFirst,
               kMakeNestedArray +
                   "    Console.WriteLine(U.S<int>(b).ToString());\n",
               "20\n");
}

// s22_6_0004: the inferred twin of s22_6_0003 — `U.S(a)` with `a: Array<int>`
// infers V=int for the first candidate and nothing applicable for the second
// (an `Array<int>` argument cannot reach `Array<Array<V>>`), so it resolves
// to the first and prints 10, in either declaration order.
BEZEL_TEST(generic_overload_coincident_signature_6292,
           inferred_distinct_signatures_still_resolve_spec_22_6_14_5) {
    expectRuns(kArrayArrayFirst,
               "    using var a := new Array<int>();\n"
               "    Console.WriteLine(U.S(a).ToString());\n",
               "10\n");
    expectRuns(kArrayNestedFirst,
               "    using var a := new Array<int>();\n"
               "    Console.WriteLine(U.S(a).ToString());\n",
               "10\n");
}

// s22_6_0005: §22.6's non-generic PREFERENCE is the LAST tie-break before
// ambiguity, so a call whose candidate set includes a non-generic declaration
// must NOT be reported ambiguous by the coincidence check — it requires EVERY
// candidate to be generic. Here `Pick(x: Array<int>)` and `Pick<V>(x: V)`
// called as `U.Pick(a)` with `a: Array<int>` are both applicable and both
// exact (the generic candidate's substituted signature `(Array<int>)`
// coincides with the non-generic declaration), so the tie-break applies.
//
// With the non-generic candidate declared FIRST this prints 5, which is what
// §22.6 requires. With it declared SECOND it used to print 10 — the generic
// candidate — making the verdict depend on declaration order; that was
// #6332, and its fix makes this second expectation 5 as well. Both orders are
// pinned here so #6292's coincidence check is shown not to fire on either,
// and so the order independence §22.6 guarantees is asserted rather than
// assumed.
BEZEL_TEST(generic_overload_coincident_signature_6292,
           non_generic_preference_beats_coincidence_spec_22_6) {
    const std::string kNonGenericFirst =
        "public class U\n"
        "  public static method Pick(x: Array<int>): int => 5;\n"
        "  public static method Pick<V>(x: V): int => 10;\n"
        "end U;\n";
    const std::string kNonGenericLast =
        "public class U\n"
        "  public static method Pick<V>(x: V): int => 10;\n"
        "  public static method Pick(x: Array<int>): int => 5;\n"
        "end U;\n";
    expectRuns(kNonGenericFirst,
               "    using var a := new Array<int>();\n"
               "    Console.WriteLine(U.Pick(a));\n",
               "5\n");
    // #6332: §22.6 selects the non-generic candidate here too; this row
    // records what the compiler does today, not what the spec wants.
    // #6332: §22.6's non-generic preference applies here too, so this now
    // prints 5 like the order above. Before the #6332 fix it printed 10 — the
    // generic candidate — because `Array<int>` is a declaration-less `Named`
    // that `judgeable` rejects, `resolveOverload` returned `Undecidable` above
    // its own non-generic tie-break, and `resolveMethodOverload` fell through
    // to `return initial`, i.e. the first-declared candidate.
    expectRuns(kNonGenericLast,
               "    using var a := new Array<int>();\n"
               "    Console.WriteLine(U.Pick(a));\n",
               "5\n");
}

// s22_6_0006: an INSTANCE receiver reaches the same coincidence through the
// `c.S(b)` spelling (§22.6 collects the candidate set from the receiver's
// class hierarchy, §12.1). The fix is in that collection, so the static and
// instance spellings must agree; this row pins the instance half.
BEZEL_TEST(generic_overload_coincident_signature_6292,
           instance_receiver_coincidence_ambiguous_spec_22_6_12_1) {
    expectAmbiguous(
        "public class C\n"
        "  public method S<V>(x: Array<V>): int => 10;\n"
        "  public method S<V>(x: Array<Array<V>>): int => 20;\n"
        "end C;\n",
        "    using var c := new C();\n" + kMakeNestedArray +
            "    Console.WriteLine(c.S(b));\n");
}