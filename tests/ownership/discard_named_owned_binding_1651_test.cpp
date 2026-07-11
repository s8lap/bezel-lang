// fix #1651 / §17.13 / §28.3 (v0.184): `_ := x;` on a NAMED OWNED BINDING —
// an owned local (any decl form) or an `owned` parameter — is an ANNOUNCED
// CONSUME, not a borrow read.
//
// Spec (docs/Bezel_Language_Spec.md, "Discard of a named owned binding",
// v0.184, cited by the discard-statement subsection §17.13):
//
//   "`_ := x;` where `x` is a named owned binding — an owned local or an
//   `owned` parameter — is an announced consume: the value is disposed at
//   the end of the discard statement, and `x` is thereafter treated exactly
//   as explicitly disposed (§28.3 — the scope-end pass skips it; a later
//   read falls under the use-after-dispose regime of §28.2, and a join with
//   a still-owned path yields maybe-consumed) ... For the same reason the
//   consume-site freeze does not apply ... By contrast, a discard whose
//   operand is in borrow state ... disposes nothing (v0.184): the discard
//   is purely cosmetic ... and the value's owner keeps the obligation."
//
// Before the fix, `_ := x;` on a plain owned LOCAL (a regular `var`
// LocalDecl) or on an `owned` PARAMETER fell through the ownership pass's
// DiscardStmt handling untouched: `x` stayed in the Owned state, so (a) it
// was disposed at SCOPE END rather than at the discard statement, and (b)
// the affine auto-drop must-use warning still fired at scope end,
// nonsensically recommending `_ := x;` as its own remedy (the exact defect
// reported in #1651). The fix generalizes the pre-existing `out owned var`-
// only special case (added for #1387) to any tracked-Owned named binding,
// and switches its state transition from moved-from (markMovedByName) to
// explicitly-disposed (markDisposedByName) — the latter is what the spec's
// "no freeze participation" / "use-after-dispose regime, not the harder
// moved-from-read error" language requires.
//
// These are SEMA-level (ownership-pass) tests: they check diagnostics only.
// The disposal-TIMING claim (dispose at end of discard statement, not scope
// end) is a codegen-observable fact and is pinned separately in
// tests/codegen/discard_named_owned_binding_1651_codegen_test.cpp.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

// A non-trivial-dispose class with one int field, mirroring the shape used
// throughout tests/sema/owned_discard_stmt_743_test.cpp (the sibling #743/
// §17.13 discard-legality suite this test complements).
const char* kR =
    "class R public _h: int := 0; destructor _h := 1; end destructor; end R; ";

bool hasUseAfterDisposeWarning(const bezel::test::OwnershipResult& r,
                               const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("after a dominating explicit Dispose() call") ==
                std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #1 (the headline #1651 defect): discarding a
// plain owned LOCAL with nothing left to do afterward must NOT draw the
// affine auto-drop must-use warning at scope end. Before the fix `x` stayed
// Owned through the discard (a no-op read), reached scope end still Owned,
// and the auto-drop pass fired its must-use warning — which even recommends
// `_ := x;` as the remedy, though that is exactly what the program already
// wrote (the literal symptom reported in #1651).
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651, plain_local_discard_no_autodrop_warning) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  var x := new R(); "
        "  _ := x; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "x"));
}

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #2: a read of `x` AFTER the discard must draw the
// §28.2 use-after-dispose warning (must-level per v0.184, straight-line
// dominated) — proving `x` was left explicitly-disposed by the discard, not
// merely borrow-read. Before the fix `x` stayed Owned across the discard, so
// this read drew NO diagnostic at all (a false "still valid" acceptance).
// Not a hard error: §28.2 downgrades a statically-provable, dominating
// use-after-dispose to a warning (the checked-disposal runtime is the error
// path).
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651, plain_local_read_after_discard_draws_28_2_warning) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  var x := new R(); "
        "  _ := x; "
        "  return x._h; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r, "x"));
}

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #3: a CONDITIONAL discard — `x` discarded on one
// branch, left Owned on the other — followed by a read after the join is a
// compile ERROR (maybe-consumed, §28.3). Before the fix the discard branch
// left `x` Owned (identically to the not-taken branch), so there was no
// state disagreement at the join and the program wrongly compiled clean.
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651, conditional_discard_join_then_read_is_error) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kR +
        "class C public static method Main(cond: bool): int "
        "  var x := new R(); "
        "  if cond then "
        "    _ := x; "
        "  end; "
        "  return x._h; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// CONTROL (unaffected by the fix either way): a discard whose operand is in
// BORROW state — here, a plain (non-`owned`) parameter — disposes nothing
// (v0.184, "borrow-state operands dispose nothing"); it is purely cosmetic,
// so a later read of the same borrow through the same binding remains valid.
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651, borrow_parameter_discard_disposes_nothing) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kR +
        "class C public static method Touch(b: R): int "
        "  _ := b; "
        "  return b._h; "
        "end Touch; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeWarning(r, "b"));
}

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH #4: the `owned` PARAMETER form. Before the fix,
// the special-case branch this generalizes gated on `sema::LocalSymbol` —
// a ParameterSymbol never matched the dynamic_cast, so an `owned` parameter
// discard was unhandled exactly like the plain-local case: `r` stayed Owned
// to method end and drew the "owned parameter ... implicitly disposed at the
// end of the method" auto-drop must-use warning despite the explicit
// discard.
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651, owned_parameter_discard_no_autodrop_warning) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kR +
        "class C public static method Consume(owned r: R): int "
        "  _ := r; "
        "  return 0; "
        "end Consume; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "r"));
}

// ===========================================================================
// CONTROL: re-discarding an already-Disposed binding is diagnosed the same
// way any other post-dispose read is (§28.2) — not a crash, not silently
// accepted as a fresh consume. (The gate is `state == Owned`; a second
// discard sees `Disposed` and falls through to the ordinary read check.)
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651, double_discard_is_diagnosed_not_silently_reconsumed) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  var x := new R(); "
        "  _ := x; "
        "  _ := x; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r, "x"));
}
