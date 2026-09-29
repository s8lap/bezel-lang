// Spec-ahead-of-impl regression for #5139 (§14.2 clause 2, v0.309): a record's
// synthesized `Equals(T)` satisfies `IEquatable<T>` whether the record lists
// `IEquatable<T>` itself OR an interface that extends `IEquatable<T>`.
//
// The pre-#5139 compiler rejected the indirect listing (`record Point: IMy`
// where `IMy: IEquatable<Point>`) at `src/sema/TypeResolution_Checks.cpp`
// L7253-7256 + L7065: the abstract `Equals` obligation reached through `IMy`
// was treated as needing a real written witness, even when the spec says
// the synthesized one is enough. Per §14.2 clause 2 (v0.309): "the
// synthesized `Equals(T)` satisfies `IEquatable<T>` whether the record lists
// `IEquatable<T>` itself or an interface extending it".
//
// Both sides change in one commit. Sema is widened to admit the indirect
// listing when the type argument is exactly Self. Codegen's
// `isSynthesizedIEquatableEquals` and the itable fill at L41458 reach the
// inherited Equals slot because the BFS-expanded `supers` includes the
// transitively-extended `IEquatable<Self>` for every record listing
// `IMy` (or any interface extending `IEquatable<Self>`).
//
// Every row compiles its own program (per #5095 review): a reject row could
// not otherwise be told apart from a sibling's failure.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True when r diagnostics contain a §12.5 "does not implement" error.
bool hasDoesNotImplementError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not implement") != std::string::npos)
            return true;
    return false;
}

// True when r carries the §12.5 reject for the record not implementing the
// inherited abstract `Equals` — the exact pre-#5139 diagnostic.
bool hasEqualsNotImplemented(const bezel::test::JitResult& r,
                             const std::string& record) {
    const std::string needle = "concrete record `" + record +
        "` does not implement inherited abstract member `Equals` (§12.5)";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ================================================================
// ACCEPT ROWS — §14.2 rule 2 acceptance through an extending interface.
// Use runAot: a program that compiled but SEGFAULT'd at the itable lookup
// would PASS a pure-sema test (false positive). runAot compiles, links,
// and runs — if the itable fill at L41458 doesn't reach the synthesized
// Equals through the inherited `IMy`, dispatch blows up.
// ================================================================

// T1: indirect listing via a non-generic extending interface. The record
// lists `IMy`; `IMy` lists `IEquatable<Point>`. The synthesized
// `Equals(Point)` is the §14.2 cl 2 witness, and `myIMy.Equals(other)`
// dispatches through `IMy`'s interface to the itable's `Equals` slot.
// Pre-#5139: rejected with the §12.5 "concrete record `Point` does not
// implement inherited abstract member `Equals`" error.
BEZEL_TEST(record_iequatable_extending_interface_5139,
           t1_indirect_listing_accepted_dispatch_through_imy_spec_14_2_rule_2) {
    auto r = runAot(
        "module M; "
        "interface IMy "
        "  : IEquatable<Point> "
        "end IMy; "
        "record Point(X: int, Y: int): IMy; "
        "class Pgm "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var q := new Point(1, 2); "
        "    var r := new Point(1, 3); "
        "    var imyRef: IMy := p; "
        "    if not imyRef.Equals(q) then return 1; end if; "
        "    if imyRef.Equals(r) then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end Pgm;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// T2: direct-plus-indirect shape. The record lists BOTH `IEquatable<Self>`
// AND the extending interface `IMy`. Per §14.2 cl 2 both listings resolve
// to the same `IEquatable<Point>.Equals` obligation, witnessed once by
// the synthesized `Equals(Point)`. Pre-#5139: rejected with §12.5.
BEZEL_TEST(record_iequatable_extending_interface_5139,
           t2_direct_plus_indirect_listing_accepted_spec_14_2_rule_2) {
    auto r = runAot(
        "module M; "
        "interface IMy "
        "  : IEquatable<Point> "
        "end IMy; "
        "record Point(X: int, Y: int): IEquatable<Point>, IMy; "
        "class Pgm "
        "  public static method Main(): int "
        "    var p := new Point(1, 2); "
        "    var q := new Point(1, 2); "
        "    var imyRef: IMy := p; "
        "    if not imyRef.Equals(q) then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end Pgm;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasDoesNotImplementError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ================================================================
// #5048 R1-R4 NON-REGRESSION ROWS — the §14.2 reject set from #5048
// must keep its §12.5 reject under #5139's widening. Pre-#5139 these
// were rejected; #5139's relaxation must NOT relax them.
// ================================================================

// T3 (R1 from #5048): a generic record listing `IEquatable<Pair<int>>` at a
// non-Self type argument. The synthesized `Equals(Pair<T>)` does NOT
// witness `Equals(Pair<int>)`. §12.5 reject must hold. Pre-#5139: rejected.
BEZEL_TEST(record_iequatable_extending_interface_5139,
           t3_non_self_iequatable_argument_direct_listing_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "record Pair<T>(A: T, B: T): IEquatable<Pair<int>>; "
        "class Pgm "
        "  public static method Main(): int return 0; "
        "end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasEqualsNotImplemented(r, "Pair"));
}

// T4: the new negative case for #5139 — a record listing `IEquatable<U>` at
// U != Self through an EXTENDING INTERFACE. `IMy: IEquatable<int>`, then
// `record Point: IMy` (no IEquatable<int> implementation anywhere on Point).
// The synthesized `Equals(Point)` does NOT witness `Equals(int)`. §12.5
// reject must hold (and is the only case not already covered by #5048 R1).
// Pre-#5139: rejected (via the blanket `equalsNeedsRealWitness = true`).
// Post-#5139: still rejected — the Self-ness check on the extending
// interface's type argument finds `int != Point` and preserves the §12.5
// reject.
BEZEL_TEST(record_iequatable_extending_interface_5139,
           t4_non_self_iequatable_argument_via_extending_interface_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "interface IMy "
        "  : IEquatable<int> "
        "end IMy; "
        "record Point(X: int, Y: int): IMy; "
        "class Pgm "
        "  public static method Main(): int return 0; "
        "end Main; "
        "end Pgm;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasEqualsNotImplemented(r, "Point"));
}
