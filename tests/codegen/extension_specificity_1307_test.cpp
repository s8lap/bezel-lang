// Codegen tests for §26.5.1 rule-1 subtype-specificity ranking over
// applicable extension candidates (issue #1307).
//
// Before this fix, when more than one `extension` block was applicable to a
// receiver and declared a same-named member, the compiler picked whichever
// block's ancestor-match loop happened to run first — pure declaration
// order, with no ranking and no ambiguity detection (proposal 0039's
// graduation, Bezel_Changelog.md v0.226: "the compiler applies first-
// declared-wins (#1307)"). §26.5.1 (spec v0.228) requires ranking applicable
// candidates by subtype ordering on their RECEIVER-SUBSTITUTED heads: "An
// extension on E1 is more specific than one on E2 when E1 is a subtype of E2
// and E2 is not a subtype of E1 ... Derived class beats base class; class
// beats interface implemented by that class; derived interface beats
// extended interface." The spec's own worked example (§26.5.1): for a
// receiver of type `Array<int>`, `extension Array<T>` (substituted head
// `Array<int>`) is more specific than `extension IEnumerable<T>` (substituted
// head `IEnumerable<int>`), because `Array<int>` is a subtype of
// `IEnumerable<int>`.
//
// Each ranking test below is run in BOTH declaration orders with candidates
// returning DIFFERENT sentinel values, so a passing test actually proves
// SPECIFICITY-BASED ranking rather than accidentally matching declaration
// order (the historical first-declared-wins bug passed a one-order test by
// luck — swapping the blocks flips the wrong answer to the right one, which
// is exactly how issue #1307 was discovered: "Swapping the two extension
// blocks (int declared first) yields rc=200 ... by luck").
//
// The tied-candidate → compile-error half of §26.5 rule 4 (the ambiguity
// diagnostic) is covered by the sema-level tests in
// tests/sema/extension_resolution_test.cpp (checking the diagnostic TEXT,
// which this runtime harness cannot do once compilation has already failed).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run a whole module source and return Main's exit code, or a sentinel.
int runMod(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ===========================================================================
// §26.5.1 rule 1 — `extension Array<T>` vs `extension IEnumerable<T>` for an
// `Array<int>` receiver: this is the spec's own worked example's shape (with
// distinguishing sentinel returns instead of `First()`, whose result is
// identical either way for a non-empty array and so cannot prove WHICH
// block won). `Array<int>` is a subtype of `IEnumerable<int>`, so the
// Array<T> block must win regardless of declaration order.
// ===========================================================================

// IEnumerable<T> declared FIRST — the order that exposed #1307 (first-
// declared-wins previously selected the LESS specific block here, rc=100).
BEZEL_TEST(codegen_extension_specificity,
           rule1_array_beats_ienumerable_ienumerable_declared_first_1307) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension IEnumerable<T> public method Pick(): int => 100; end; "
        "extension Array<T> public method Pick(): int => 200; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Pick(); "
        "end Main; end C;"), 200);
}

// Array<T> declared FIRST — the control order that previously "worked" only
// by accident. Same expected answer as the swapped order above: this is the
// pair that actually proves ranking (not declaration order) decides it.
BEZEL_TEST(codegen_extension_specificity,
           rule1_array_beats_ienumerable_array_declared_first_1307) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension Array<T> public method Pick(): int => 200; end; "
        "extension IEnumerable<T> public method Pick(): int => 100; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Pick(); "
        "end Main; end C;"), 200);
}

// ===========================================================================
// §26.5.1 rule 1 — "class beats interface implemented by that class": a
// concrete class extension outranks an extension on an interface the class
// implements, for a receiver of the class's own type.
// ===========================================================================

BEZEL_TEST(codegen_extension_specificity,
           rule1_class_beats_interface_interface_declared_first_1307) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IFoo end IFoo; "
        "class Widget: IFoo end Widget; "
        "extension IFoo public method Pick(): int => 10; end; "
        "extension Widget public method Pick(): int => 20; end; "
        "class Runner public static method Main(): int "
        "  var w := new Widget(); "
        "  return w.Pick(); "
        "end Main; end Runner;"), 20);
}

BEZEL_TEST(codegen_extension_specificity,
           rule1_class_beats_interface_class_declared_first_1307) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "interface IFoo end IFoo; "
        "class Widget: IFoo end Widget; "
        "extension Widget public method Pick(): int => 20; end; "
        "extension IFoo public method Pick(): int => 10; end; "
        "class Runner public static method Main(): int "
        "  var w := new Widget(); "
        "  return w.Pick(); "
        "end Main; end Runner;"), 20);
}

// ===========================================================================
// Control (§26.5 step 2/3, unaffected by #1307): a SINGLE applicable
// candidate must still resolve exactly as before — the ranking/ambiguity
// machinery must be a no-op when there is nothing to rank.
// ===========================================================================
BEZEL_TEST(codegen_extension_specificity,
           single_candidate_resolution_unchanged_1307) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension Array<T> public method Pick(): int => 42; end; "
        "class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2, 3]; "
        "  return xs.Pick(); "
        "end Main; end C;"), 42);
}
