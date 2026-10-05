// fix-forward #5362 (stdlib §5 v0.259 / §4.10): `Array<T>.Take(n)` silently
// failed to type as a lazy `IEnumerable<T>`, degrading the whole enclosing
// statement to nothing.
//
// THE DEFECT. #5362 added a loud reject for the removed `OwningSet<T>.Take` /
// `TryTake` names, and did it by rewriting the lazy-typing arm's guard from
//
//     if (common::isLinqLazyPassthroughOperatorName(m) ||
//         (m == "Take" && colName != "OwningSet")) {
//
// to
//
//     if (common::isLinqLazyPassthroughOperatorName(m) &&
//         !((m == "Take" || m == "TryTake") && colName == "OwningSet")) {
//
// The rewritten form reads as if it still admits `Take` on every non-OwningSet
// receiver. It admits NONE. `Take` is deliberately absent from
// kLinqLazyPassthroughOperatorNames (BuiltinRegistry.cpp — 11 names, no
// `Take`; its applicability is receiver-dependent, the reason `Reverse` also
// keeps its own arm), so `isLinqLazyPassthroughOperatorName("Take")` is false,
// the negated conjunct evaluates true for EVERY `Take` call whatever the
// receiver, and the arm falls through for all of them.
//
// THE SILENCE. Falling through leaves the call untyped rather than rejected, so
// the compiler continues and the failure lands two layers down as two
// "internal:" warnings:
//   #2434 — `lowerLocalDecl`'s initializer failed to lower, degraded to a
//           zero-initialized value;
//   #3079 — the following `Console.WriteLine`'s argument failed to lower, so
//           THE WHOLE STATEMENT EMITS NOTHING — not even the line terminator.
// A program that prints a correct prefix and then prints nothing still exits 0.
// Conformance `l5_0006` (`xs.Take(-5).ToArray()`) did exactly that.
//
// WHY EVERY ROW PINS stdoutText. `runAot` runs the program as a subprocess and
// captures its output in `JitResult::stdoutText` (codegen_support.hpp) — the
// value channel the AOT migration uses for observables that cannot round-trip a
// process exit status. Here it is load-bearing rather than merely available:
// the program's exit status is 0 BOTH with and without the fix, because the
// degrade path does not fail the run. An exit-code assertion would therefore
// pass on the broken compiler and prove nothing. The #3079 signature is an
// ABSENT LINE, so stdoutText is the only place the defect is visible.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A row asserts on the program's captured stdout. An empty capture where output
// was expected IS the defect (#3079 emitted no line at all), so this is
// deliberately a substring test rather than an equality test: the assertion
// text carries the reason, and equality would be ambiguous between "printed
// the wrong number" and "printed nothing".
bool printed(const JitResult& r, const std::string& want) {
    return r.stdoutText.find(want) != std::string::npos;
}

}  // anonymous namespace

// ===========================================================================
// The conformance case itself: §5's negative-count clamp.
// `xs.Take(-5)` clamps to zero elements, so `.ToArray()` yields an empty array
// and `.Count` is 0. `l5_0006` expects the line "0".
// FAILS-WITHOUT: stdoutText empty (exit still 0) — the statement emitted nothing.
// PASSES-WITH: stdoutText contains "0".
// ===========================================================================
BEZEL_TEST(array_take_lazy_typing_5362_ff, negative_count_clamps_to_zero) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    using var ys := xs.Take(-5).ToArray(); "
        "    Console.WriteLine(ys.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    // An EMPTY stdout is the #2434/#3079 degrade: the initializer failed to
    // lower, and the whole statement emitted nothing. The macro stringifies
    // the condition, so the reason lives in this comment.
    BEZEL_EXPECT_TRUE(printed(r, "0"));
}

// ===========================================================================
// A POSITIVE count, to pin that `Take` still truncates rather than merely
// typing. Without this row a fix that made `Take` an identity — e.g. one that
// admitted EVERY `Take` and ignored the count — would pass the row above while
// being wrong.
// FAILS-WITHOUT: empty stdout (#3079).
// PASSES-WITH: "2".
// ===========================================================================
BEZEL_TEST(array_take_lazy_typing_5362_ff, positive_count_truncates) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3, 4, 5]; "
        "    using var ys := xs.Take(2).ToArray(); "
        "    Console.WriteLine(ys.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(printed(r, "2"));
}

// ===========================================================================
// A count BEYOND the source takes everything (§5: they are range operations,
// not magnitude constructors). Pins that the arm does not clamp the other way.
// FAILS-WITHOUT: empty stdout (#3079).
// PASSES-WITH: "3".
// ===========================================================================
BEZEL_TEST(array_take_lazy_typing_5362_ff, count_beyond_source_takes_all) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    using var ys := xs.Take(99).ToArray(); "
        "    Console.WriteLine(ys.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(printed(r, "3"));
}

// ===========================================================================
// `Take` must COMPOSE with the other lazy operators, not merely terminate.
// This is the row that separates "Take is typed as a lazy sequence" from "Take
// compiles at all": `Where(...).Take(1)` needs `Take` to produce the lazy
// sentinel that `ToArray` later consumes, which is a stronger requirement than
// the single terminal call in the rows above.
// FAILS-WITHOUT: empty stdout (#3079).
// PASSES-WITH: "1".
// ===========================================================================
BEZEL_TEST(array_take_lazy_typing_5362_ff, composes_with_where) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3, 4]; "
        "    using var ys := xs.Where(lambda(x: int): bool => x mod 2 = 0)"
        "                        .Take(1).ToArray(); "
        "    Console.WriteLine(ys.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(printed(r, "1"));
}

// ===========================================================================
// The control #5362 actually wanted must STILL hold: `OwningSet<T>.Take` is
// gone (renamed to `Pop`/`TryPop`, stdlib §4.10, v0.259) and must be REJECTED
// with a diagnostic naming it. Without this row, a fix that simply widened the
// disjunct to every receiver would re-admit the name #5362 removed and still
// pass the four rows above — it would trade one regression for another.
// Green both with and without this fix-forward: the reject is #5362's own.
// ===========================================================================
BEZEL_TEST(array_take_lazy_typing_5362_ff, owning_set_take_still_rejected) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method Main(): int "
        "    using var s: OwningSet<int> := new OwningSet<int>(); "
        "    using var taken := s.Take(2); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    // A real rejection, not a crash/timeout/ASan report (fix #3892: a bare
    // `!r.ran` cannot tell the four outcomes apart).
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "OwningSet"));
}
