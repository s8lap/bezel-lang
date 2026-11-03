// Regression tests for issue #7811 — `Array<T>.IndexOf` and
// `Array<T>.LastIndexOf` skipped stdlib §4's Equality-search admission gate.
//
// stdlib §4 (v0.350) *Equality search at an element type with no equality*:
//
//   "The members that compare an argument with stored elements by `Equals` are
//    the equality search: `Contains` and `Remove(item)` of `ICollection<T>`
//    and of its implementations here at an unconstrained element type
//    (`Array<T>`, `LinkedList<T>`), `Contains` of `Stack<T>` and `Queue<T>`,
//    `Array<T>.IndexOf` and `LastIndexOf`, `LinkedList<T>.Find`, and the span
//    family's `IndexOf`, `Contains` and `SequenceEqual`. Their signatures are
//    unconstrained, and they are conditionally-admitted members (language spec
//    §14.2): a call is admitted only where the element type, nullability
//    stripped, has a member for the comparison language spec §9.2 calls the
//    first kind, `Equals` with an argument of that type."
//
// `Array<T>.Contains` carried the gate; `IndexOf` and `LastIndexOf` did not.
// The SAME element type — a record with a function-typed field, neither
// auto-hashable (§9.2) nor declaring custom equality (§25.2) — was therefore
// REFUSED by `Contains` and ACCEPTED by `IndexOf` / `LastIndexOf`, which
// searched an element type that has no first-kind equality at all. The
// collection type itself stays well-formed (stdlib §4): only its equality
// search is refused.
//
// The REJECT rows assert the specific diagnostic (the operator, the
// `T: hashable` requirement, the §4 *Equality search* citation and the
// element type). The ACCEPT rows read the data: an element type that DOES
// declare equality searches correctly and prints its index, and a FUNCTION
// element type stays admitted (§27.7.1's first-kind equality, #5827), so an
// over-broad rejection cannot hide.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\nimport System;\n" + decls +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

// A record with a FUNCTION-typed field: neither auto-hashable (§9.2) nor
// declaring custom equality (§25.2), so it has no member for §9.2's first
// kind of equality — the element type stdlib §4's equality search refuses.
const char* kNoEqualityRecord =
    "public record Rec(F: () -> int);\n";

// A record that DOES declare the equality pair: §3.6's hashable row makes it
// equality-bearing, so the equality search is admitted and must find the
// element.
const char* kEqualityRecord =
    "public record Rec(X: int)\n"
    "  public method Equals(other: Rec): bool => X = other.X;\n"
    "  public method GetHashCode(): int => X;\n"
    "  public static method operator =(a: Rec, b: Rec): bool => a.X = b.X;\n"
    "  public static method operator <>(a: Rec, b: Rec): bool => a.X <> b.X;\n"
    "end Rec;\n";

// The call is REFUSED with the equality-search diagnostic naming the
// operator, the `T: hashable` requirement, the cited section and the element
// type. The citation differs per member: the §4 family cites §4's
// *Equality search*, while `Contains` is gated by the §5 constrained block
// the house has cited since #3331.
void expectEqualitySearchRejected(const std::string& decls,
                                   const std::string& body,
                                   const std::string& op,
                                   const std::string& citation) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("operator `" + op + "`") == std::string::npos)
            continue;
        if (d.message.find("requires `T: hashable`") == std::string::npos)
            continue;
        if (d.message.find(citation) == std::string::npos) continue;
        if (d.message.find("`Rec` is neither auto-hashable") ==
            std::string::npos)
            continue;
        found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}

void expectStdout(const std::string& decls, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(decls, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char* kNoEqualityArray =
    "    var a: Array<Rec> := [new Rec(F := lambda() => 1)];\n";
const char* kEqualityArray =
    "    var a: Array<Rec> := [new Rec(X := 1), new Rec(X := 2), "
    "new Rec(X := 1)];\n";

}  // namespace

// ---- #7811: the two members that skipped the gate -------------------------

BEZEL_TEST(equality_search_indexof_7811, indexof_at_a_record_with_no_equality_is_rejected) {
    // per stdlib §4 (v0.350) *Equality search*, spec §14.2 / §9.2
    expectEqualitySearchRejected(kNoEqualityRecord,
                                 std::string(kNoEqualityArray) +
                                     "    Console.WriteLine(a.IndexOf(a[0]));",
                                 "IndexOf", "stdlib §4 (*Equality search*)");
}

BEZEL_TEST(equality_search_indexof_7811, lastindexof_at_a_record_with_no_equality_is_rejected) {
    // per stdlib §4 (v0.350) *Equality search*, spec §14.2 / §9.2
    expectEqualitySearchRejected(kNoEqualityRecord,
                                 std::string(kNoEqualityArray) +
                                     "    Console.WriteLine(a.LastIndexOf(a[0]));",
                                 "LastIndexOf", "stdlib §4 (*Equality search*)");
}

// The CONTROL: `Contains` was already gated, and the gate must fire the same
// way for the two members that share its condition — the same element type,
// one verdict.
BEZEL_TEST(equality_search_indexof_7811, contains_at_the_same_element_type_is_rejected) {
    expectEqualitySearchRejected(kNoEqualityRecord,
                                 std::string(kNoEqualityArray) +
                                     "    Console.WriteLine(a.Contains(a[0]));",
                                 "Contains", "stdlib §5, proposal 0116 D9");
}

// ---- controls: an admitted equality search still searches ------------------

BEZEL_TEST(equality_search_indexof_7811, indexof_at_an_equality_bearing_element_type_searches) {
    // per stdlib §4.4 (v0.350) `IndexOf(item: T, startIndex: int := 0): int`
    expectStdout(kEqualityRecord,
                 std::string(kEqualityArray) +
                     "    Console.WriteLine(a.IndexOf(new Rec(X := 1)));\n"
                     "    Console.WriteLine(a.IndexOf(new Rec(X := 9)));",
                 "0\n-1\n");
}

BEZEL_TEST(equality_search_indexof_7811, lastindexof_at_an_equality_bearing_element_type_searches) {
    // per stdlib §4.4 (v0.350) `LastIndexOf(item: T): int` — the LAST occurrence
    expectStdout(kEqualityRecord,
                 std::string(kEqualityArray) +
                     "    Console.WriteLine(a.LastIndexOf(new Rec(X := 1)));\n"
                     "    Console.WriteLine(a.LastIndexOf(new Rec(X := 2)));",
                 "2\n1\n");
}

// A FUNCTION element type stays admitted: §27.7.1 gives it a first-kind
// equality, so the §4 search is admitted there (#5827), and the gate must not
// refuse it.
BEZEL_TEST(equality_search_indexof_7811, indexof_at_a_function_element_type_stays_admitted) {
    expectStdout("",
                 "    var f: (int) -> int := lambda(x) => x;\n"
                 "    var g: (int) -> int := lambda(x) => x + 1;\n"
                 "    var a: Array<(int) -> int> := [f, g];\n"
                 "    Console.WriteLine(a.IndexOf(g));\n"
                 "    Console.WriteLine(a.LastIndexOf(g));",
                 "1\n1\n");
}
