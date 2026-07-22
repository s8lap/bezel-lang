// Codegen — v0.213 sound replacements for the retired transit exception (#2081).
//
// Spec §28.6/§28.11 (v0.213) retire axiom A8 (the transit exception) and its
// bulk form `DisposeElements()`: `Dispose()` through a borrowing-family
// collection element is now a compile error (pinned by the lit rejects
// dispose_transit_exception_1653.bzl / dispose_dict_entry_retired_2081.bzl /
// dispose_elements_retired_2081.bzl). This test pins that the spec's prescribed
// REPLACEMENTS run correctly:
//   - selective / interleaved cleanup: take the element OUT first
//     (`move coll[i]`, the `Pop*` family, `Drain()`) into an owned binding, whose
//     disposal the code controls — and the owning collection's cascade then
//     disposes the remainder with NO double-dispose of the taken (moved-from)
//     slot;
//   - whole-collection ownership of dictionary VALUES (the retired asymmetric
//     `for var entry in d do entry.Value.Dispose()` loop): declare
//     `OwningDictionary<K, V>`, whose `Dispose()` cascades over the values.
//
// Element disposal is observed through a static counter bumped by the element
// class's destructor and returned as the exit code.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runResource(const std::string& mainBody) {
    return run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;");
}

}  // namespace

// §28.11 v0.213 (#2081): the selective-cleanup replacement for the retired
// transit per-element dispose. `PopFirst()` takes the element out into an owned
// binding `taken`, disposed by hand (→ 1); the array's scope-end cascade then
// disposes the remaining element, and the popped slot is NOT re-disposed → the
// take-out and the cascade together dispose each element exactly once. Encoded:
// <count after the hand-dispose>*10 + <count after the array's cascade> = 1*10 +
// 2 = 12. (The `move coll[i]` take-out form is the equivalent idiom, but leaks
// the extracted element's storage under ASan today — the pre-existing #2119 — so
// this pin uses the `Pop*` family, which the same §28.11 clause blesses.)
BEZEL_TEST(dispose_transit_retired_2081, pop_out_then_dispose_no_double) {
    BEZEL_EXPECT_EQ(runResource(
        "var afterTake := -1; "
        "do "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); "
        "  a.Add(move (new R())); "
        "  var taken := a.PopFirst(); "
        "  taken.Dispose(); "
        "  afterTake := R._n; "
        "end; "
        "return afterTake * 10 + R._n;"), 12);
}

// §28.11 v0.213 (#2081): the replacement for the retired asymmetric-dictionary
// per-value disposal loop (`for var entry in d do entry.Value.Dispose()`). A
// holder that owns its dictionary values declares `OwningDictionary<K, V>`; its
// scope-end `Dispose()` cascades over the values → both destructors run → 2.
BEZEL_TEST(dispose_transit_retired_2081, owning_dictionary_cascade_disposes_values) {
    BEZEL_EXPECT_EQ(runResource(
        "do "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d[1] := move (new R()); "
        "  d[2] := move (new R()); "
        "end; "
        "return R._n;"), 2);
}
