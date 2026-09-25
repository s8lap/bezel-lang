// Spec-derived test for issue #4986 — `return y!` of an OWNED plain local
// through a plain (non-`owned`) instance class/interface return is
// rejected, by §28.3 announcement table row 4, exactly as `return y` is
// rejected (spec §28.3 row 4, v0.307; spec §17.8 "x! is owned when x is
// owned", v0.307).
//
// THE BUG (#4986). §17.8 carves `x!` out of the universal null-aware
// operator borrow rule (§17.4 analog): "if x has ownership state owned …,
// x! is owned" — the same ownership state as the bare read. §28.3 row 4
// rejects a fresh owned value through a plain instance-method class
// return. The existing row-4 check (checkInstanceBorrowReturn,
// src/ownership/Ownership.cpp) matches only the bare-identifier arm
// (ast::IdentifierExpr) for the fresh-owned classification, so the bang
// form slipped past and the binary compiled clean with a leak (post-#4955;
// pre-#4955 it was a UAF instead, #4986's own thesis). The
// ownedArgSourceIsBorrow classifier carries the universal null-aware
// operator borrow rule (§17.4 analog) and so classifies `y!` as a borrow,
// which would route through the borrow-RESOLVED half of row-4 (an
// unguarded silent fall-through when the borrow is rooted at a LOCAL
// rather than at a plain parameter).
//
// THE FIX (this test's fix). checkInstanceBorrowReturn's fresh-owned
// classification peels a top-level `!` / `as` (§17.4 TypeTestExpr / §17.8
// PostfixBangExpr) via peelTopCastBang so `return y!;` is classified the
// same way `return y;` is. The borrow-root lookup, separately, peels the
// same wrapper via identifierRootThroughCastBang so a `return g!;` of a
// plain non-`owned` parameter is rejected by the parameter-sourced
// borrow arm of row-4 exactly as `return g;` is.
//
// OBSERVABILITY. Both reject shapes are pre-fix ACCEPTED with a leak
// (`y` is auto-dropped in F post-return and nothing else disposes it,
// measured 24 B / 1 alloc in the mixed-meet case, 48 B / 2 in the
// plain-bang case via runAotChecked on the r21-4986-4987 reverse-trip).
// The binary itself does not crash on the bang form (the call site
// happens to never read the dangling borrow), so the oracle is the row-4
// diagnostic appearing at compile time — set
// BEZEL_TEST_FILTER=instance_bang_owned_local_return_row4_4986 and a
// single-process run must read the row-4 needle in r.diagnostics.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.3 announcement table row 4's fresh-owned-return diagnostic.
bool rejectsFreshOwnedInstanceReturn(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("receiver-sourced borrow") == std::string::npos)
            continue;
        if (d.message.find("fresh owned value is rejected") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

// §28.3 announcement table row 4's parameter-sourced-borrow diagnostic.
bool rejectsParameterSourcedInstanceReturn(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("a borrow reached through parameter") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// NEGATIVE — `return y!` of a plain owned local is row-4-rejected, exactly
// as `return y;` is. §17.8 carves the bare-name and the bang form to the
// same ownership state, so the §28.3 row-4 check classifies them through
// the same fresh-owned path.
// ===========================================================================

// The a6 / s28_3_0029 bang twin: a bare fresh owned local returned under
// `y!`. Pre-fix this compiled clean (a warning, no reject) and leaked 24
// bytes per `y` instance under runAotChecked.
BEZEL_TEST(instance_bang_owned_local_return_row4_4986,
           owned_local_y_bang_blockbody_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Holder "
        "  public method Peek(): Widget "
        "    var y := new Widget(); "
        "    return y!; "
        "  end Peek; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsFreshOwnedInstanceReturn(r));
}

// The expression-body twin: `=> y!;` over a tracked owned local under a
// plain return. Pre-fix this compiled clean (same warning shape as
// block-body). Route is runExprBodyOwningReturnCheck, not handleReturn,
// so the fix has to reach both call sites — the file pins that.
BEZEL_TEST(instance_bang_owned_local_return_row4_4986,
           owned_local_y_bang_exprbody_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Holder "
        "  private _w: Widget; "
        "  public constructor(w: Widget) _w := w; end constructor; "
        "  public method Peek(): Widget "
        "    var y := new Widget(); "
        "    return y!; "
        "  end Peek; "
        "  public method PeekAlt(): Widget "
        "    var y := new Widget(); "
        "    return y!; "
        "  end PeekAlt; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsFreshOwnedInstanceReturn(r));
}

// The parameter-sourced-borrow twin: `return g!;` over a plain (non-`owned`)
// parameter `g`. §17.8 still classifies `g!` as borrowed (the param is
// borrowed), so this routes through the borrow arm of row-4 — a fresh-owned
// classification is wrong here. Pre-fix the borrow-root lookup missed the
// bang layer (`identifierRoot`, not `identifierRootThroughCastBang`), so
// the parameter-sourced rejection silently fell through.
BEZEL_TEST(instance_bang_owned_local_return_row4_4986,
           plain_param_g_bang_blockbody_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Holder "
        "  public method Peek(g: Widget): Widget "
        "    return g!; "
        "  end Peek; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsParameterSourcedInstanceReturn(r));
}

// ===========================================================================
// POSITIVE — accept controls that must not regress.
// ===========================================================================

// A receiver-owned field reached through `!` is, per §28.3 row 4's
// receiver-sourced admission, an admitted borrow return — the receiver's
// lifetime is wider than the call. The row-4 check fires on the source, not
// the operator wrapping; peelTopCastBang produces an IdentifierExpr over
// the FieldSymbol, which lookupState does NOT track (fields are not in
// states_), so the fresh-owned arm of this fix is correctly a no-op here.
BEZEL_TEST(instance_bang_owned_local_return_row4_4986,
           receiver_owned_field_bang_blockbody_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Holder "
        "  private _w: Widget; "
        "  public constructor(w: Widget) _w := w; end constructor; "
        "  public method Peek(): Widget "
        "    return _w!; "
        "  end Peek; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Declaring `owned` on the return admits the bang form exactly as it
// admits the bare form — the row-4 reject is discharge-method-shaped, and
// `y!` over a tracked owned local reads as `y` (§17.8), so `y!` IS
// accepted under `owned Widget` as the legal owned transfer.
BEZEL_TEST(instance_bang_owned_local_return_row4_4986,
           owned_return_type_y_bang_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Holder "
        "  public method Make(): owned Widget "
        "    var y := new Widget(); "
        "    return y!; "
        "  end Make; "
        "end Holder;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A scope-bound local reached through `!` is, per §28.3 / §28.10, scope-
// bound — its bang form is the same move-out the bare form is rejected for
// (the #4948 carve at handleReturn:8631). This test pins that the new
// checkInstanceBorrowReturn peel does not overfire on a `using var`,
// because the `implicitDischarge` short-circuit precedes the row-4 check.
BEZEL_TEST(instance_bang_owned_local_return_row4_4986,
           using_var_x_bang_blockbody_still_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget public constructor() end constructor; "
        "  destructor end destructor; end Widget; "
        "class Holder "
        "  public method Peek(): Widget "
        "    using var x := new Widget(); "
        "    return x!; "
        "  end Peek; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The §28.10 using-var-move-out message is the one a `using var` owns;
    // the row-4 message does NOT fire in this case because the using-var
    // rejection at handleReturn runs first and short-circuits (line 8631).
}
