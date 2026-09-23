// Spec-derived regression tests for §11.5 modifier-order enforcement
// (v0.307, #4719). The spec mandates:
//
//   visibility static (async|pure) (abstract|virtual|override|sealed override)
//                (owned|scoped) readonly member-kind Name ...
//
//   "The compiler rejects out-of-order modifiers."
//
// Prior to this fix, the parser collected modifiers in a vector preserving
// source order but the sema passes never validated the order; the compiler
// silently admitted any permutation. Two rule-corpus rows diverged as a
// result:
//
//   s11_5_0002  public static readonly owned _instance := ...   # reject
//   s11_5_0003  public owned static readonly _instance := ...   # reject
//
// Both rows now surface a single "out of order" diagnostic.
//
// The fix lives in src/sema/TypeResolution_Checks.cpp
// (Resolver::checkMemberModifierOrder + the per-member slot walk in
// checkTypeMemberModifierOrder), wired in Resolver::run() right after
// checkStaticNotVirtual() so an out-of-order modifier doesn't cascade into
// false positives from the dispatch-modifier checks.

#include <string>

#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndResolveGated;

namespace {

// True iff some reported diagnostic's message contains `needle` (case-sensitive,
// substring match). Mirrors the helper inlined by other modifier tests; kept
// local so this file's failure messages read in isolation.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// §11.5 control: the canonical order on a static owned readonly field
// (s11_5_0001's shape) parses and sema-checks cleanly. The §22.12 rule 4
// warning is expected and is NOT an error, so the row is `accept`-class
// in the corpus.
BEZEL_TEST(modifier_order_4719,
           canonical_static_owned_readonly_field_accepted_spec_11_5) {
    auto r = parseAndResolveGated(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "end Widget; "
        "class Cache "
        "  public static owned readonly _instance: Widget := new Widget(); "
        "  public constructor() end constructor; "
        "end Cache;");
    BEZEL_EXPECT_FALSE(hasMsg(r, std::string("out of order")));
}

// §11.5 reject (corpus s11_5_0002): `readonly` before `owned` is rejected.
// `static readonly owned _instance` puts the `readonly` modifier at slot 6
// and `owned` at slot 5; the walk sees the decreasing pair and reports it.
BEZEL_TEST(modifier_order_4719,
           readonly_before_owned_rejected_spec_11_5_s11_5_0002) {
    auto r = parseAndResolveGated(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "end Widget; "
        "class Cache "
        "  public static readonly owned _instance: Widget := new Widget(); "
        "  public constructor() end constructor; "
        "end Cache;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, std::string("out of order")));
    // The diagnostic must name the misplaced modifier (the corpus's
    // `diagnostic: order` field matches on the substring `order`; this
    // assert pins the precise wording so a future edit that drops the
    // word regresses the test rather than the corpus's substring search).
    BEZEL_EXPECT_TRUE(hasMsg(r, std::string("`owned` out of order")));
}

// §11.5 reject (corpus s11_5_0003): `owned` before `static` is rejected.
// `owned static readonly _instance` puts `owned` at slot 5 and `static`
// at slot 2; the decreasing pair reports the misplaced `static`.
BEZEL_TEST(modifier_order_4719,
           owned_before_static_rejected_spec_11_5_s11_5_0003) {
    auto r = parseAndResolveGated(
        "module M; "
        "class Widget "
        "  public constructor() end constructor; "
        "end Widget; "
        "class Cache "
        "  public owned static readonly _instance: Widget := new Widget(); "
        "  public constructor() end constructor; "
        "end Cache;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, std::string("out of order")));
    BEZEL_EXPECT_TRUE(hasMsg(r, std::string("`static` out of order")));
}

// §11.5 control: the canonical order on a method — `public override
// method Speak(): string => "Woof";` — parses and sema-checks cleanly.
// Mirrors s11_5_0005's shape; ensures the order check is slot-driven and
// does not over-reject on member kinds where only one §11.5 slot is
// occupied (here just the virtual-modifier slot, `override` at slot 4).
BEZEL_TEST(modifier_order_4719,
           canonical_override_method_accepted_spec_11_5) {
    auto r = parseAndResolveGated(
        "module M; "
        "class Animal "
        "  public constructor() end constructor; "
        "  public virtual method Speak(): string => \"generic\"; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor() end constructor; "
        "  public override method Speak(): string => \"Woof\"; "
        "end Dog;");
    BEZEL_EXPECT_FALSE(hasMsg(r, std::string("out of order")));
}

// §11.5 reject on a method: `pure virtual method` is canonical (pure in
// slot 3, virtual in slot 4 — increasing), but `virtual pure method` is
// not (slot 4 then slot 3 — decreasing). The §12.1 mutual-exclusion
// check is NOT the gate here (only one of abstract/virtual/override/sealed
// override appears, so it does not fire); the §11.5 order check is the
// only one that catches it.
BEZEL_TEST(modifier_order_4719,
           virtual_before_pure_rejected_spec_11_5) {
    auto r = parseAndResolveGated(
        "module M; "
        "class Animal "
        "  public constructor() end constructor; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor() end constructor; "
        "  public virtual pure method Speak(): string => \"Woof\"; "
        "end Dog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, std::string("out of order")));
}
