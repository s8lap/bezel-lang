// Spec-derived regression tests for #2518 — the `!` force-unwrap twin of
// #2368 / #1005: `checkOwnedFieldReturn`'s two arms did not look through a
// leading `PostfixBangExpr`, so the force-unwrap spellings of the §28.6
// owned-field-read-through-return violations were silently admitted.
//
// §28.6 bullet 2 (Bezel_Language_Spec.md ~L6695, v0.236): "Transferring an
// owned field's value out — a `move` expression (§28.8.1), the only owned read
// (K9.1) — is the only way to hand ownership onward: ... to the caller only as
// `return move _f;` through an `owned` return type." The force-unwrap `!` is a
// null-forgiving postfix operator (§A.7) on the FIELD reference; it does NOT
// make the read a `move`, and `move _f!` is still a `move` of the underlying
// owned field. So the `!`-wrapped spellings must be classified IDENTICALLY to
// their bare forms:
//
//   - HOLE 1 (§28.6 bullet 2, the #2368 twin): a bare `return _f!` through an
//     `owned` return is the same double-free shape as `return _f` — the field
//     is left un-moved-from (the holder's destructor still disposes it), while
//     the caller now also owns the value and disposes it too. The `!` strips
//     the §7 nullable-vs-nonnullable mismatch that would otherwise fire first
//     on the bare nullable form, exposing the very hole #2368 closed.
//
//   - HOLE 2 (§28.6, the #1005 twin): a `return move _f!` through a NON-
//     `owned` return leaks the moved value out with no owner exactly like
//     `return move _f` — the §28.8.1 line-3529 canonical nullable-owned-field
//     detach is `move _f!` followed by `_f := null`, and on the non-`owned`-
//     return path that moved value escapes the body with no owner.
//
// The implementation gap (#2518): `checkOwnedFieldReturn`
// (src/ownership/Ownership.cpp ~L7642) tested the AST directly — the #2368
// bare-read arm called `exprIsOwnedFieldRead(e)` and the #1005 move arm called
// `exprIsOwnedFieldRead(*un->operand)` — without consulting the existing
// `lookThroughForceUnwrap` helper (src/ownership/Ownership_Checks.cpp, already
// used by every other §28.8.1 path). So both `!`-wrapped spellings compiled
// clean on HEAD.
//
// The fix: thread `lookThroughForceUnwrap` into BOTH arms so the field-read
// recognizer sees through the leading `!`. The reject cases below compiled
// clean on HEAD (the hole); each is rejected at the def site with the fix (no
// compiling program can contain the shape, so no caller can double-free/leak).
// The §28.8.1 canonical nullable detach (`move _f!` through an `owned` return,
// followed by `_f := null`) and the legal force-unwrap borrow (`_f!` through a
// non-`owned` return) stay clean — the rejection targets ONLY the two hole
// shapes, matching the #2368 / #1005 scoping on the force-unwrap axis.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const bezel::test::OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// HOLE 1 fires the #2368 diagnostic — pin its stable substring (names §28.6
// bullet 2). HOLE 2 fires the #1005 diagnostic — pin its stable substring.
constexpr std::string_view kHole1 = "bare read of an owned field";
constexpr std::string_view kHole2 = "an owned field's `move`d value through a non-`owned` return";

// A nullable owned `Cell?` field is the shape the `!` force-unwrap applies to
// (force-unwrap of a non-nullable is not a `!`-bearing shape). The `Cell` class
// has an empty ctor + dtor so the holder's destructor cascades to the field
// (the double-free precondition).
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; ") +
        body;
}

}  // namespace

// ===========================================================================
// HOLE 1 REJECT — `return _f!` through an `owned` return (§28.6 bullet 2,
// the #2368 twin). Each compiled clean on HEAD; each is rejected at the def
// site with the #2368 diagnostic with the fix.
// ===========================================================================

// §28.6 bullet 2 / §28.8.1 (#2518 HOLE 1 repro): a bare force-unwrap `_c!` of
// the nullable owned field through an `owned` return — the issue's exact
// shape. Expression body.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           bare_force_unwrap_owned_return_exprbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell?; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell => _c!; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kHole1));
}

// The same `_c!`-through-`owned`-return shape in a BLOCK body — same double-
// free; the block-body spelling must not bypass the check.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           bare_force_unwrap_owned_return_blockbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell?; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell "
        "    return _c!; "
        "  end Take; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kHole1));
}

// The bare force-unwrap via explicit `this._c!` — same double-free shape; the
// explicit-`this` spelling must not bypass the check.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           bare_force_unwrap_this_field_owned_return_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell?; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell => this._c!; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kHole1));
}

// ===========================================================================
// HOLE 2 REJECT — `return move _f!` through a NON-`owned` return (§28.6, the
// #1005 twin). Each compiled clean on HEAD; each is rejected at the def site
// with the #1005 diagnostic with the fix.
// ===========================================================================

// §28.6 / §28.8.1 (#2518 HOLE 2 repro): `move _c!` of the nullable owned field
// through a non-`owned` return — the issue's exact shape. The `!` yields a non-
// nullable `Cell`, so the §7 mismatch that would mask the bare form does not
// fire, and the #1005 leak diagnostic must. Expression body.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           move_force_unwrap_nonowned_return_exprbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell?; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Leak(): Cell => move _c!; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kHole2));
}

// The same `move _c!` through a non-`owned` return in a BLOCK body — same
// leak; the block-body spelling must not bypass the check.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           move_force_unwrap_nonowned_return_blockbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell?; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Leak(): Cell "
        "    return move _c!; "
        "  end Leak; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kHole2));
}

// ===========================================================================
// ACCEPT — shapes the fix must NOT regress. Every shape below is a legitimate
// force-unwrap expression that is NOT one of the two hole shapes.
// ===========================================================================

// §28.8.1 line 3529 (spec-endorsed nullable-owned-field detach): `move _c!`
// through an OWNED return is THE canonical transfer spelling for a nullable
// owned field — the `!` strips the nullability so the value binds to the non-
// nullable `owned Cell` return, and `move` is the §28.6 bullet 2 transfer
// operator. The §28.8.1 restoration obligation is enforced separately.
// `checkOwnedFieldReturn` exempts the `move` form on the owned-return path
// (whether or not the operand is `!`-wrapped) — must compile.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           move_force_unwrap_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell?; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell => move _c!; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6 bullet 1 admission: a bare force-unwrap `_c!` (no `move`) through a
// NON-`owned` return is a legal BORROW — the field's owner is unchanged, the
// receiver's destructor still disposes it. The non-owned-return path fires
// ONLY on `move`, so the bare force-unwrap borrow must still compile.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           bare_force_unwrap_nonowned_return_borrow_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell?; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Peek(): Cell => _c!; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A force-unwrap of a NON-owned field through an `owned` return — `f` is not
// `owned`, so `exprIsOwnedFieldRead` is false regardless of the `!`, and
// §28.6 bullet 2 does not fire. (The `!` is harmless on a non-nullable too;
// this cell pins that the `!` unwrap alone does not cause a false reject when
// the underlying field is not owned.) Must compile through the #2518 check.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           force_unwrap_nonowned_field_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Foo end Foo; "
        "class Holder "
        "  public f: Foo; "
        "  public constructor(f0: Foo) f := f0; end constructor; "
        "  public method Take(): owned Foo => new Foo(); "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A force-unwrap of an OWNED PARAMETER through an `owned` return — the
// parameter is not a FieldSymbol, so `exprIsOwnedFieldRead` is false and the
// `!` does not turn it into a field read. Confirms `lookThroughForceUnwrap`
// does not over-match non-field owned places. Must compile.
BEZEL_TEST(force_unwrap_owned_field_return_2518,
           force_unwrap_owned_param_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Pass(owned c: Cell?): owned Cell => c!; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
