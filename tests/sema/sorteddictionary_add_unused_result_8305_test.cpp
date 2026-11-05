// Spec-derived regression test for issue #8305.
//
// AUTHORITY. spec §21.9 (v0.355): "Method calls returning () ... are the common
// cases; no warning is emitted for these." Stdlib §4.17 (v0.355) declares
//
//     public method Add(key: K, value: V);   // no result
//     public method Clear();                 // no result
//     public method Remove(key: K): bool;    // bool result
//     public method TryAdd(key: K, value: V): bool;
//
// so the unused-result warning must fire exactly for the bool-returning
// members and never for the unit-returning ones.
//
// THE DEFECT. The SortedDictionary arm of the method-typing block in
// ExprTypecheck_Calls.cpp lists Count, the bool members, GetOrNull, FirstKey /
// LastKey and GetEnumerator, then falls through to a catch-all that types the
// call as the VALUE type V ("Indexer / Value return: V"). `Add` and `Clear` are
// in neither list, so `d.Add(2, 5);` and `d.Clear();` were typed `int` and the
// §21.9 warning fired. The Dictionary arm has no catch-all and leaves both
// untyped, which is why `Dictionary.Add` drew no warning.
//
// The result is read through the SemaResult harness, which keeps warnings on
// successful compiles (runAot fills diagnostics only on reject).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// Number of §21.9 unused-result warnings in the diagnostics list.
int unusedResultWarnings(const bezel::test::SemaResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("the result of this expression statement is unused") !=
                std::string::npos)
            ++n;
    }
    return n;
}

bezel::test::SemaResult semaAll(const std::string& src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

}  // namespace

// ── §21.9 / stdlib §4.17: the issue's repro compiles with NO unused-result warning ──

BEZEL_TEST(sorteddictionary_8305, add_statement_no_unused_result_warning_spec_21_9) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var d := new SortedDictionary<int, int>(); "
        "  d.Add(2, 5); "
        "  var e := new Dictionary<int, int>(); "
        "  e.Add(2, 5); "
        "  Console.WriteLine(d.Count); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 0);
}

BEZEL_TEST(sorteddictionary_8305, clear_statement_no_unused_result_warning_spec_21_9) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var d := new SortedDictionary<int, int>(); "
        "  d.Add(2, 5); "
        "  d.Clear(); "
        "  return d.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 0);
}

// ── control: declared-result members still warn ────────────────────────────────

BEZEL_TEST(sorteddictionary_8305, remove_statement_still_warns_spec_21_9) {
    // stdlib §4.17: `Remove(key: K): bool` — a discarded bool result is diagnosed.
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var d := new SortedDictionary<int, int>(); "
        "  d.Remove(2); "
        "  return d.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 1);
}

BEZEL_TEST(sorteddictionary_8305, try_add_statement_still_warns_spec_21_9) {
    // stdlib §4.17: `TryAdd(key: K, value: V): bool` — a discarded bool result is diagnosed.
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var d := new SortedDictionary<int, int>(); "
        "  d.TryAdd(2, 5); "
        "  return d.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 1);
}
