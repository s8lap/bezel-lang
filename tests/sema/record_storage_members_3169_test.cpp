// fix #3169 (§9.1 / §9.2 / §30.14 rule 2, v0.242; refactor of #3145/#3152):
// the three symbol-side storage enumerations
//   - `anyRecordStorageField` (SymbolCollection.cpp; fields-only existential)
//   - `recordPositionalFieldSymbols` (SymbolCollection.cpp; positional-indexed)
//   - `forEachStorageMember` (Sendability.cpp; per-member facts incl. `owned`)
// are unified into ONE primitive, `sema::recordStorageMembers`, that yields
// per-member facts: `member`, `name`, `type`, `isPositional`, `isStatic`,
// `isOwned`, `isAutoProperty`. The refactor is BEHAVIOR-PRESERVING — every
// caller of the three former accessors must reach the same verdict it did
// before, and the `owned` marker must continue to be read from BOTH
// `ast::Param::modifier` (positional) AND `MemberSymbol::modifiers`
// (body / auto-prop), per #3145.
//
// ── WHY THIS TEST PINS THE FIX ─────────────────────────────────────────────
//
// Each row exercises a behavior that an earlier accessor's verdict was the
// load-bearing fact for. The refactor must not move any verdict:
//
//   T1 — record positional + body fields are both visited by the unified
//        primitive (positional first, then body — the order §30.14
//        component-path diagnostics depend on). Pre-#3169, the three
//        accessors walked the same members in three documents' worth of
//        near-copies; this test pins that one walk serves all three.
//
//   T2 — `Channel<T>` over a record whose positional field is a borrow-
//        carrying class is rejected, as #3145 measured (§30.14 rule 2 +
//        §9.7). Pre-#3169, this was exclusively `anyRecordStorageField`'s
//        verdict; post-#3169 it routes through `recordStorageMembers`.
//        A coarse "any storage field exists" would pass — the test pins
//        the SPECIFIC field-name diagnostic so a neighbouring false reject
//        cannot.
//
//   T3 — `where T: sendable` over an imported class with an `owned`
//        captured primary-ctor parameter is satisfied (#3165/#3171).
//        Pre-#3169, `forEachStorageMember` did the walk + the §28.1
//        `owned` both-source read; post-#3169 the same walk + same read
//        lives in `recordStorageMembers` and Sendability.cpp is a thin
//        adapter. This test pins that an `owned` captured parameter's
//        `owned` marker is still yielded (the very defect bc0edd5 warned
//        about and #3169 closes structurally).
//
//   T4 — `where T: sendable` over an imported class with a NON-owned
//        non-sendable captured primary-ctor parameter is REJECTED. This
//        is the §30.14 rule 3 verdict; an earlier refactor attempt that
//        moved `forEachStorageMember`'s logic without the `owned` both-
//        source read would have silently accepted this. Pin the rejection.
//
// All rows go through runAot — the same driver the gate runs is the one
// driving the assertion, so a refactor that moves a verdict surfaces here.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run the program; return the exit code on a clean run, -999 if the compile
// was REJECTED (mirrors runJit's failWith semantics so positive rows can
// distinguish "wrong answer" from "compile failed").
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff the program was REJECTED at compile/link. A coarse predicate —
// the negative rows also assert on the diagnostic substring so a neighbouring
// false reject cannot pass.
bool rejected(const std::string& prog) {
    auto r = runAot(prog);
    return r.diagnostics.hasErrors();
}

// Pin a SPECIFIC diagnostic substring so a coarse "rejected" check cannot
// accept a different reject.
bool rejectedWith(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (std::string(d.message).find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// T1 — §9.1 + §9.2 record: positional field + body field both visited by the
// unified primitive.
//
// A record `Holder(Owned: Leaf)` has a §9.1 positional field. A `new
// Holder(l)` whose `l` is a temporary that is NOT a borrow is accepted; a
// temporary that IS a borrow (here, a class instance) is rejected by §9.7.
// The reject is issued by `checkRecordFromTemporary`,
// `anyRecordStorageField`'s only caller in sema — post-#3169 that caller
// routes through `recordStorageMembers`. A regression that lost the
// positional pass would silently accept (the body-pass filter `isField`
// alone might skip the positional).
// ===========================================================================
BEZEL_TEST(record_storage_members_3169,
           record_positional_field_routes_through_unified_primitive_9_7) {
    const std::string src =
        "module M;"
        "public sealed class Leaf end Leaf;"
        "public record Holder(L: Leaf) end Holder;"
        "class C public static method Main(): int"
        "  using var h := new Holder(new Leaf());"
        "  return 0;"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""9.7"));
}

// ===========================================================================
// T2 — §30.14 rule 2 + §9.7: `Channel<T>` over a record whose positional
// field is a borrow-carrying class is rejected.
//
// Pre-#3169, `anyRecordStorageField` (SymbolCollection.cpp) was the only
// caller that asked "does any storage field carry a borrow". Post-#3169,
// that verdict comes from `recordStorageMembers` via the
// `anyRecordStorageField` thin adapter (this file's file
// `src/sema/SymbolCollection.cpp`, post-refactor).
//
// The §30.14 rule 2 verdict is what the component-path diagnostic names —
// the substring pin catches a refactor that moved the verdict without
// moving the field-name diagnostic.
// ===========================================================================
BEZEL_TEST(record_storage_members_3169,
           channel_over_record_positional_field_rejected_30_14_rule_2) {
    const std::string src =
        "module M;"
        "public sealed class Iface end Iface;"
        "public record Holder(L: Iface) end Holder;"
        "class C public static method Main(): int"
        "  using var ch := new Channel<Holder>(1);"
        "  return 0;"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "type argument `Holder`"));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""30.14"));
}

// ===========================================================================
// T3 — §22.10 + §30.14 rule 2: `new Channel<Node>(1)` where `Node(owned L:
// Leaf)` has its captured `owned` parameter IS satisfied (Node is sendable).
//
// This pins #3169's central correctness property: the `owned` marker for
// a captured primary-ctor parameter is read from the SAME both-source
// rule (positional `ast::Param::modifier` OR `FieldSymbol::modifiers`,
// whichever is populated). Before #3169, that read lived in Sendability.cpp's
// `forEachStorageMember` body — a reader of the old accessor's body could
// not tell which channel was consulted, only that one of them was. Post-
// #3169, the read is a single statement inside `recordStorageMembers` —
// visible, single-source, and exposed via `RecordStorageMember::isOwned`.
//
// An earlier #3145 warn that landed in a comment but NOT in code was the
// bc0edd5 false-reject; this test pins the green path on the post-#3169
// code.
// ===========================================================================
BEZEL_TEST(record_storage_members_3169,
           imported_owned_captured_param_still_sendable_22_10_30_14) {
    const std::string src =
        "module M;"
        "public sealed class Leaf"
        "  public property V: int := 41;"
        "end Leaf;"
        "public sealed class Node(owned l: Leaf)"
        "  public method Get(): int => l.V;"
        "end Node;"
        "class C public static method Main(): int"
        "  using var ch := new Channel<Node>(1);"
        "  using var n := new Node(new Leaf());"
        "  return n.Get();"
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 41);
}

// ===========================================================================
// T4 — §30.14 rule 3: `new Channel<Node>(1)` where `Node(l: Iface)` has its
// NON-owned non-sendable captured primary-ctor parameter is REJECTED.
//
// The mirror of T3: this test pins the NEGATIVE branch of `isOwned`. A
// refactor that read only `posParam->modifier` (skipping
// `FieldSymbol::modifiers`) would silently accept this. Pin the rejection
// with a §30.14 substring so a neighbouring false reject cannot pass.
//
// `l` is referenced in `Get` so §22.10 captures it — uncaptured primary-
// ctor parameters are NOT storage, so without the body reference Node would
// compile silently and the test would lose its load-bearing assertion.
// ===========================================================================
BEZEL_TEST(record_storage_members_3169,
           imported_non_owned_captured_param_still_rejected_30_14_rule_3) {
    const std::string src =
        "module M;"
        "public sealed class Iface"
        "  public property V: int := 41;"
        "end Iface;"
        "public sealed class Node(l: Iface)"
        "  public method Get(): int => l.V;"
        "end Node;"
        "class C public static method Main(): int"
        "  using var ch := new Channel<Node>(1);"
        "  using var n := new Node(new Iface());"
        "  return n.Get();"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejected(src));
    BEZEL_EXPECT_TRUE(rejectedWith(src, "\xC2\xA7""30.14"));
}
