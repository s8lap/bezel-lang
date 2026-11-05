// Spec-derived regression tests for issue #7812: the PLAIN `.` spelling of a
// predicate `Count` on a concrete collection that declares `Count` as a
// property. `SortedSet<T>` (and `SortedDictionary<K, V>`) were the gap: the
// `?.` twin was refused by #7670, and the `.` spelling was accepted.
//
// Spec §26.5 rule 1 (v0.355): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability,
// or accessibility — instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver. If the committed
// instance lookup then fails ..., the call is a compile error citing the
// instance member(s); it does not fall back to an extension."
// Stdlib §4.18 (v0.355) declares `public pure readonly property Count: int`
// on `SortedSet<T>`; §4.17 does the same on `SortedDictionary<K, V>`. So
// `s.Count(lambda(x: int) => x > 1)` is the arity error, and the composed
// spelling `s.Where(pred).Count()` (stdlib §5) is the remedy.
//
// The ORACLE is the specific diagnostic (the house's arity wording). The
// CONTROLS — the property read, the composed spelling, and the §5 predicate
// count on an `IEnumerable<T>` receiver, which declares no `Count` — must
// stay accepted and must RUN.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& decl, const std::string& body) {
    return "module M;\n"
           "import System.Linq;\n"
           "import System.Collections.Generic;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s: SortedSet<int> := new SortedSet<int>();\n"
           "    s.Add(1); s.Add(2); s.Add(3);\n"
           "    var sd: SortedDictionary<int, int> := new SortedDictionary<int, int>();\n"
           "    sd.Add(1, 10); sd.Add(2, 20); sd.Add(3, 30);\n"
           "    var st: Set<int> := [1,2,3];\n"
           "    var sk: Stack<int> := new Stack<int>();\n"
           "    sk.Push(1); sk.Push(2); sk.Push(3);\n" +
           decl +
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char kArityNeedle[] = "The instance member takes no arguments, but this "
                            "call supplies 1";

}  // namespace

// ── #7812: the plain `.` predicate Count on a sorted collection ───────────

// The issue's repro: `SortedSet<int>.Count(pred)` was accepted and printed 1.
BEZEL_TEST(sema_sortedset_plain_count_predicate_7812,
           sortedset_count_predicate_rejected) {
    // per §26.5 rule 1 (v0.355) + stdlib §4.18 (`Count` property)
    expectRejected("    var c := s.Count(lambda(x: int) => x > 1);\n"
                   "    Console.WriteLine(c);",
                   kArityNeedle);
}

// The `SortedDictionary<K, V>` twin (stdlib §4.17 declares `Count`).
BEZEL_TEST(sema_sortedset_plain_count_predicate_7812,
           sorteddictionary_count_predicate_rejected) {
    // per §26.5 rule 1 (v0.355) + stdlib §4.17
    expectRejected("    var c := sd.Count(lambda(k: int, v: int) => v > 10);\n"
                   "    Console.WriteLine(c);",
                   kArityNeedle);
}

// Already refused by #3342 / #7700 before this fix; kept as a guard so the
// sorted-head arm does not regress the unsorted heads it sits beside.
BEZEL_TEST(sema_sortedset_plain_count_predicate_7812,
           set_count_predicate_still_rejected) {
    // per §26.5 rule 1 (v0.355) + stdlib §4.10
    expectRejected("    var c := st.Count(lambda(x: int) => x > 1);\n"
                   "    Console.WriteLine(c);",
                   kArityNeedle);
}

BEZEL_TEST(sema_sortedset_plain_count_predicate_7812,
           stack_count_predicate_still_rejected) {
    // per §26.5 rule 1 (v0.355) + stdlib §4.11
    expectRejected("    var c := sk.Count(lambda(x: int) => x > 1);\n"
                   "    Console.WriteLine(c);",
                   kArityNeedle);
}

// CONTROL: the arity-0 property READ on the sorted head is the instance
// member and keeps working (stdlib §4.18 live slot count).
BEZEL_TEST(sema_sortedset_plain_count_predicate_7812,
           control_sortedset_count_property_read_accepted) {
    // per §4.18 + §26.5 rule 1
    expectStdout("    Console.WriteLine(s.Count);", "3\n");
}

// CONTROL: the composed spelling reaches the §5 operator pipeline (stdlib §1
// "Block-by-name": `coll.Where(pred).Count()`).
BEZEL_TEST(sema_sortedset_plain_count_predicate_7812,
           control_sortedset_where_count_accepted) {
    // per stdlib §1 "Block-by-name" + §5
    expectStdout("    Console.WriteLine(s.Where(lambda(x: int) => x > 1).Count());",
                 "2\n");
}

// CONTROL: `IEnumerable<T>` declares no `Count`, so the §5 predicate count
// stays reachable through it (§26.5 rule 2 — no instance member blocks it).
BEZEL_TEST(sema_sortedset_plain_count_predicate_7812,
           control_ienumerable_count_predicate_accepted) {
    // per §26.5 rule 1 (no declared `Count`) + stdlib §5
    expectStdout("    var e: IEnumerable<int> := s;\n"
                 "    Console.WriteLine(e.Count(lambda(x: int) => x > 1));",
                 "2\n");
}
