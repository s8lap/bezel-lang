// fix #775 / §16.3: a `Range<T>` selector in `SelectMany` (the per-element
// inner sequence is a range) is rejected with a clean diagnostic instead of
// crashing.
//
// `arr.SelectMany((n) => (1..n+1))` compiled clean but SIGSEGV'd at runtime: the
// selector lowers to a 32-byte `Range` struct return, while the flatten
// trampoline (`bz$linq$flatsel`) calls it via a pointer-return prototype — the
// struct bits are reinterpreted as a pointer and fed to `bz_lazy_from_array`.
// Per §16.3 a `Range<T>` implements `IEnumerable<T>`, so SelectMany SHOULD
// support it, but the lazy range source is not yet implemented. Until then the
// front end rejects it with a clear §16.3 diagnostic rather than miscompiling.
// The legitimate paths (Select returning a sequence of ranges, an ordinary
// non-range SelectMany selector, for-in over a range, range slicing) are
// untouched.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
bool rejects(const std::string& program) {
    return runJit(program).diagnostics.hasErrors();
}
}  // namespace

// §16.3 REJECT: the #775 repro — a Range selector in SelectMany. Was SIGSEGV;
// now a clean compile error.
BEZEL_TEST(selectmany_range_selector_775, range_selector_rejected) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := new Array<int>(2); "
        "    arr.Add(1); arr.Add(3); "
        "    var total: int := arr.SelectMany((n) => (1..n + 1)).Count(); "
        "    return total mod 100; "
        "  end Main; "
        "end C;"), true);
}

// §16.3 ACCEPT: `Select` returning a Range yields a sequence OF ranges (no
// flatten/iteration of the inner range) — does not hit the trampoline, must
// still compile and run. arr=[1,3] → 2 range values.
BEZEL_TEST(selectmany_range_selector_775, select_returning_range_ok) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := new Array<int>(2); "
        "    arr.Add(1); arr.Add(3); "
        "    return arr.Select((x) => (1..x)).Count(); "  // 2
        "  end Main; "
        "end C;"), 2);
}

// §16.3 ACCEPT: an ordinary non-range SelectMany selector (Array inner) is
// unaffected by the gate. arr=[1,2], selector returns arr → 2*2 = 4.
BEZEL_TEST(selectmany_range_selector_775, array_selector_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := new Array<int>(2); "
        "    arr.Add(1); arr.Add(2); "
        "    return arr.SelectMany((x) => arr).Count(); "  // 4
        "  end Main; "
        "end C;"), 4);
}

// §16.3 ACCEPT: for-in over a range is a different (counted-loop) path — the
// gate must not touch it. 1..4 is inclusive ⇒ 4 iterations.
BEZEL_TEST(selectmany_range_selector_775, for_in_range_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var k: int := 0; "
        "    for var i in 1..4 do k := k + 1; end; "  // 4 (inclusive)
        "    return k; "
        "  end Main; "
        "end C;"), 4);
}
