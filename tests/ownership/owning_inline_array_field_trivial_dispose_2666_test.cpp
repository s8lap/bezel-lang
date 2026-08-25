// Spec-derived tests for issue #2666 - §28.9 "Trivial-dispose classes"
// (v0.237) declaring the inline-array-field arm:
//
//   "has no destructor, no `owned` fields, no fields of owning-inline-array
//    type (`[N owned]T?` or `[N owned]T`, §15.1.1), and its base class (if
//    any, other than `Object`) is itself trivial-dispose"
//
// The pre-fix `common::isNonTrivialDisposeClass` (`src/common/OwnershipFacts.cpp`)
// walked the class chain for a `DestructorSymbol` and for a `FieldSymbol` with an
// `owned` modifier / `owned` posParam - never the owning-inline-array arm. A
// class whose only disposal-bearing member is a `[N owned]T?` / `[N owned]T`
// field was therefore misclassified as trivial-dispose.
//
// FOUR consumers of this classification all carried the same gap:
//   1. `common::isNonTrivialDisposeClass` (the canonical predicate, where the
//      fix lives);
//   2. `MethodOwnership::localIsTrivialDisposeNewBinding` (the §28.3 auto-drop
//      warning gate at Ownership.cpp), which #2654 worked around locally with
//      a SEPARATE walk (`MethodOwnership::classChainHasOwningInlineArrayField`)
//      so the SUPPRESSION would stay sound even with the predicate wrong. The
//      shared fix removes that workaround - this file's primary tests pin the
//      firewall;
//   3. `PureChecker::isTrivialDisposeClass` (the §22.15 throw-carve-out gate,
//      PureChecker.cpp:97), an INDEPENDENT copy of the classification that
//      #2666 brings into line;
//   4. `ExprTypecheck.cpp`'s §21.9 bare-discard inline walk (a FOURTH copy at
//      ExprTypecheck.cpp:12422), likewise fixed.
//
// The primary behaviour change is a DIAGNOSTIC: an undischarged plain-`var`
// owned local whose type is a class declaring an owning-inline-array field
// MUST now warn (was suppressed pre-fix via #2654's workaround AND would be
// mis-suppressed without it once that workaround is removed). All four tests
// here are FAIL-WITHOUT / PASS-WITH on the cycle-3-integration baseline
// (which carries #2654's workaround): they currently pass because of the
// workaround; they will pass post-#2666 because of the predicate; they would
// FAIL if the workaround were dropped without #2666.
//
// SCOPE: the spec arm mentions `[N owned]T?` (nullable) AND `[N owned]T`
// (non-nullable). The test below pins BOTH, plus the posParam/primary-ctor-
// parameter analogue of the inline-array arm - a primary-ctor parameter that
// IS the inline-array field - which falls out of the same fix path.
//
// fail-without / pass-with: every `BEZEL_EXPECT_TRUE(hasAutoDropWarning(...))`
// below PASSES on `21894cc` (cycle-3-integration, with #2654's workaround in
// place) AND will CONTINUE to pass after the predicate is fixed and the
// workaround is removed. The NEW ASSERTION is the post-fix equality of
// predicate-derivation: the warning is fired by `common::isNonTrivialDisposeClass`
// itself, not by a sibling walk that happens to agree. The acceptance-control
// tests confirm a class WITHOUT an owning-inline-array field still classifies
// trivial-dispose and suppresses the warning.
//
// per §28.9 (v0.237), §15.1.1, #2666, #2654

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& decls, const std::string& mainBody) {
    return std::string("module M; ") + decls +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// =============================================================================
// POSITIVE - the §28.9 owning-inline-array-field arm correctly classifies its
// declaring class as non-trivial-dispose, so the §28.3 auto-drop WARNING must
// fire on an undischarged plain-`var` owned local of that type.
// =============================================================================

// The issue's primary repro: a class whose only disposal-bearing member is a
// nullable owning-inline-array field. Before #2666, this warning was suppressed
// by `classChainHasOwningInlineArrayField` (a separate walk the predicate
// skipped) - fixed by teaching the predicate itself to test the arm.
// fix #2666, spec §28.9 v0.237, §15.1.1
BEZEL_TEST(own_owning_inline_array_field_trivial_dispose_2666,
           nullable_inline_array_field_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Res public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class SlotHolder public constructor() end constructor; "
        "  public slots: [2 owned]Res? := [new Res(), new Res()]; "
        "end SlotHolder; ",
        "var b2 := new SlotHolder(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b2"));
}

// §28.9 v0.237's arm names BOTH `[N owned]T?` AND `[N owned]T`. The
// non-nullable spelling is not the giveaway - its `arrayOwned` flag is set the
// same way. Pinning both keeps a future narrowing fix (e.g. one that keys on
// `nullable`) from silently swallowing the warning on the non-nullable row.
// fix #2666, spec §28.9 v0.237
BEZEL_TEST(own_owning_inline_array_field_trivial_dispose_2666,
           non_nullable_inline_array_field_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Res public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class SlotHolder public constructor() end constructor; "
        "  public slots: [2 owned]Res := [new Res(), new Res()]; "
        "end SlotHolder; ",
        "var b2 := new SlotHolder(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b2"));
}

// §9.1 / §22.10: a positional (primary-ctor-parameter) field whose primary-
// constructor parameter's type is `[N owned]T?`. The fix's predicate-extension
// has to read the inline-array ownership from BOTH `fs->posParam->type` AND
// the body `FieldDecl::type` - pinning the posParam leg here. Per #2666's
// "fix direction" the predicate must NOT depend on `fs->decl` shape.
// fix #2666, spec §28.9 v0.237, §9.1, §22.10
BEZEL_TEST(own_owning_inline_array_field_trivial_dispose_2666,
           posparam_inline_array_field_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Res public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class SlotHolder(slots: [2 owned]Res?) "
        "  public constructor() end constructor; "
        "end SlotHolder; ",
        "var b2 := new SlotHolder([new Res(), new Res()]); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b2"));
}

// =============================================================================
// ACCEPTANCE CONTROL - the spec's other trivial-dispose arms are preserved.
// =============================================================================

// A class with NO destructor, NO `owned` fields, NO owning-inline-array
// fields, and NO non-trivial base is still trivial-dispose and DOES suppress
// the warning. Without this control, a fix that classifies every class
// non-trivial-dispose would silently pass the positive rows above while
// regressing every existing trivial-dispose caller.
// fix #2666, spec §28.9 v0.237
BEZEL_TEST(own_owning_inline_array_field_trivial_dispose_2666,
           plain_trivial_dispose_still_suppresses) {
    auto r = parseAndCheckOwnership(wrap(
        "class Trivial public constructor() end constructor; "
        "  public V: int := 7; "
        "end Trivial; ",
        "var t := new Trivial(); return t.V;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "t"));
}

// A class with a destructor (the spec's FIRST disqualifier) is still
// non-trivial-dispose and DOES warn. Without this control, the fix's
// inline-array extension could mask a regression that broke the destructor arm.
// fix #2666, spec §28.9 v0.237
BEZEL_TEST(own_owning_inline_array_field_trivial_dispose_2666,
           destructor_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Dtor public constructor() end constructor; "
        "  destructor end destructor; "
        "end Dtor; ",
        "var d := new Dtor(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "d"));
}

// A class with an `owned` field of NON-inline-array type (the spec's SECOND
// disqualifier) is still non-trivial-dispose. Without this control, the fix
// could broaden the predicate so it fires on every owning-collection field
// rather than only on `[N owned]T?` / `[N owned]T`.
// fix #2666, spec §28.9 v0.237
BEZEL_TEST(own_owning_inline_array_field_trivial_dispose_2666,
           owned_field_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Res public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder public constructor() end constructor; "
        "  public owned F: Res? := null; "
        "end Holder; ",
        "var h := new Holder(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "h"));
}

// A class with a NON-owning inline-array field (`[N]T`, no `owned` element
// modifier) is STILL trivial-dispose. The spec's arm is specifically `[N
// owned]T?` / `[N owned]T` - `[N]T` has no disposal obligation per slot.
// Without this control, a fix that keyed on `ast::TypeKind::InlineArray`
// without the `arrayOwned` test would over-fire and suppress the warning on
// a class whose array field carries no obligation.
// fix #2666, spec §28.9 v0.237, §15.1.1
BEZEL_TEST(own_owning_inline_array_field_trivial_dispose_2666,
           non_owning_inline_array_still_suppresses) {
    auto r = parseAndCheckOwnership(wrap(
        "class Res public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class SlotHolder public constructor() end constructor; "
        "  public slots: [2]Res := [new Res(), new Res()]; "
        "end SlotHolder; ",
        "var b := new SlotHolder(); return 0;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}
