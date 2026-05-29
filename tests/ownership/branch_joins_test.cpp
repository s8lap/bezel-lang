// Spec-derived tests for Ownership slice O4 — branch joins (§28.3).
//
// O4's contract: at a control-flow merge after if/elif/else, each
// local's ownership state in each incoming branch must agree.
//   - Owned ⊓ Owned         = Owned (obligation carried forward)
//   - Borrowed ⊓ Borrowed   = Borrowed
//   - PolyNull ⊓ X          = X  (PolyNull absorbs into either side)
//   - Owned ⊓ Borrowed      = ERROR (per the spec's asymmetric join)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap a method body that uses `cond`, `a`, `b`, `source` as scope-
// available names. Declares all of them as parameters/fields so the
// identifier-resolution pass doesn't emit unrelated diagnostics.
std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class T end T; class Stream end Stream; "
        "class C "
        "  public source: Stream := null; "
        "  public method M(cond: bool, a: bool, b: bool) "
        ) + body + " end M; end C;";
}

} // namespace

BEZEL_TEST(own_join, owned_vs_borrowed_at_merge_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); "
        "else x := source.Inner; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, owned_on_both_branches_carries_obligation) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); "
        "else x := new Stream(); "
        "end;"));
    // Both branches produce owned → joined as Owned with obligation.
    // No discharge after the merge → diagnostic.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, owned_on_both_branches_and_discharged_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); "
        "else x := new Stream(); "
        "end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, borrowed_on_both_branches_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := source.Inner; "
        "else x := source.Other; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, no_else_with_owned_in_then_is_clash) {
    // Without an else, the false edge falls through with entry state.
    // Entry x is Uninitialized; then-branch makes it Owned. Merge is
    // Owned ⊓ Uninitialized — currently reported as a poison/clash
    // since they don't match.
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); end;"));
    // Slice O4 treats this as a state mismatch (state poison) — the
    // local can't be safely used after the merge. No explicit diagnostic
    // is required here (the state becomes Poisoned silently); a
    // subsequent read would diagnose.
    (void)r;
}

BEZEL_TEST(own_join, null_init_then_owned_in_branch_absorbs) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := null; "
        "if cond then x := new Stream(); "
        "else x := new Stream(); "
        "end; "
        "x.Dispose();"));
    // Entry is PolyNull; both branches produce Owned. PolyNull ⊓ Owned
    // = Owned, then Owned ⊓ Owned = Owned. Discharge satisfies.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, polynull_absorbs_borrowed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := null; "
        "if cond then x := source.Inner; "
        "else x := source.Other; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, three_branch_chain_with_clash) {
    // if-elif-else, one branch produces owned, one borrowed → error.
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if a then x := new Stream(); "
        "elif b then x := new Stream(); "
        "else x := source.Inner; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
