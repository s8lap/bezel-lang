// Spec-derived regression tests for #2368 — "bare owned-field read through an
// `owned` return is accepted — an implicit terminal move without restoration
// (spec §28.6 bullet 2, K9.1, §28.8.1)".
//
// §28.6 bullet 2 (Bezel_Language_Spec.md ~L6695, v0.236): "Transferring an owned
// field's value out — a `move` expression (§28.8.1), the only owned read (K9.1)
// — is the only way to hand ownership onward: to a *callee* when consumed in
// place by an `owned` parameter, to the *caller* only as `return move _f;`
// through an `owned` return type". A BARE read of an owned field returned
// through an `owned` return (`public method Take(): owned T => _f;`) is therefore
// REJECTED — neither bullet 1 (which admits a bare read through a NON-`owned`
// return as a BORROW) nor bullet 2 (which requires `move`) covers it.
//
// §28.8.1 (the field-restoration rule, Bezel_Language_Spec.md ~L6840, v0.236):
// "there is no terminal-move dispensation: a terminal move — `return move x.f;`,
// or any return whose operand evaluation performs the move with no restoring
// assignment before the exit — is rejected, the return edge being itself an
// unrestored exit". The bare read is the implicit-terminal-move twin: the field
// is left UN-MOVED-FROM (the holder's destructor still disposes it), while the
// caller now also owns the returned value and disposes it — a double-free (the
// #1814 ruling's terminal-move-without-restoration shape).
//
// The implementation gap (#2368): `checkOwnedFieldReturn`
// (src/ownership/Ownership.cpp) early-returned when the declared return was
// `owned` (`if (returnOwned_) return;`), and `checkBorrowReturnedThroughOwningType`
// did the same — so a bare owned-FIELD read flowed out through an `owned` return
// while the holder's field was never marked moved-from. Confirmed on HEAD via
// probe (`build/bezelc -c`): the issue's exact repro and the three variants
// below all compiled clean; under indirect dispatch (interface / virtual-override
// / imported / function-value) the caller cannot resolve the concrete method,
// producerState falls to the name-head Owned fallback, and the caller disposes
// the borrow — a HARD double-free of the field's real owner.
//
// The fix (§28.6 bullet 2): the `owned`-return path of `checkOwnedFieldReturn`
// is no longer blanket-exempt. It now REJECTS a bare owned-FIELD read (`_f`,
// `this._f`, `obj._f`, `Registry.Slot`) and exempts only the spec's sanctioned
// transfer form (`return move _f;` — whose §28.8.1 restoration obligation is
// enforced separately) plus every non-field-read expression (fresh `new`, owned
// local/parameter, owned-returning call result, ...). The remedy the diagnostic
// names is the spec's own: write `return move <field>;` and satisfy §28.8.1.
//
// All shapes below were hand-verified against `build/bezelc -c` on HEAD (the
// reject cases compiled clean — the hole) and with the fix (rejected at the def
// site — no compiling program can then contain the bare-read-through-owned-return
// method, and no caller can double-free).

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

// The #2368 diagnostic's stable substring (names §28.6 bullet 2).
constexpr std::string_view kRepro = "bare read of an owned field";

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
// REJECT — the issue's exact repro shape (bare owned receiver-field read
// through an `owned` return). Each compiled clean on HEAD; each is rejected at
// the def site with the fix.
// ===========================================================================

// §28.6 bullet 2 / §28.8.1 (#2368 repro): a BARE `OwningArray<Cell>` receiver-
// field read through an `owned` return — the issue's exact shape, formerly
// pinned as ACCEPTED at `owned_owning_array_return_via_receiver_field_accepted`
// (owning_collection_borrow_return_indirect_dispatch_1760_test.cpp ~L230, now
// flipped to REJECTED).
BEZEL_TEST(bare_owned_field_owned_return_2368,
           owning_array_receiver_field_bare_read_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() _items := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned OwningArray<Cell> => _items; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kRepro));
}

// The same bare-read-through-owned-return shape on a plain (non-owning-
// collection) class type — the issue's shape is not specific to OwningArray;
// §28.6 bullet 2 governs every owned field. Block body.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           cell_receiver_field_bare_read_blockbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell "
        "    return _c; "
        "  end Take; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kRepro));
}

// The bare read via explicit `this._c` (expression body) — same double-free
// shape; the explicit-`this` spelling must not bypass the check.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           cell_this_field_bare_read_exprbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell => this._c; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kRepro));
}

// A bare owned-field read through a NON-`this` receiver (`h._c` where `h` is a
// parameter) is the same §28.6 bullet 2 violation — `h._c` is a borrow of `h`'s
// field, and returning it as `owned` launders the borrow into a second owner
// (the caller) while `h`'s own destructor still cascades to the field — the
// same double-free under a different holder.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           cell_param_field_bare_read_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class Extractor "
        "  public static method Take(h: Holder): owned Cell => h._c; "
        "end Extractor;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, kRepro));
}

// ===========================================================================
// ACCEPT — shapes the fix must NOT regress. Every shape below is a legitimate
// `owned`-return expression that is NOT a bare owned-field read.
// ===========================================================================

// §28.6 bullet 2 sanctioned transfer: `move _f` through an `owned` return is
// the spec's ONLY transfer-to-caller form. The §28.8.1 field-restoration
// obligation is enforced separately (a terminal `move _f` with no restoration
// is rejected by the §28.8.1 check on block bodies); `checkOwnedFieldReturn`
// itself must NOT fire on the `move` form. Expression body.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           move_form_owned_return_exprbody_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Take(): owned Cell => move _c; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A fresh `new` through an `owned` return — a genuine owned producer, not a
// field read. Must compile.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           fresh_new_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Maker "
        "  public static method Make(): owned Cell => new Cell(); "
        "end Maker;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An OWNED PARAMETER passthrough through an `owned` return — the legal owned-
// transfer form (§28.8). Must compile.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           owned_param_passthrough_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Pass(owned c: Cell): owned Cell => c; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An OWNED LOCAL returned through an `owned` return — a legal owned transfer
// (the local is a named owned temporary; its scope-end discharge is discharged
// by the return). Must compile.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           owned_local_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): owned Cell "
        "    var c := new Cell(); "
        "    return c; "
        "  end Make; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An owned-returning CALL RESULT through an `owned` return (with `move`) — the
// fresh-producer passthrough; not a field read. Must compile.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           move_call_result_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Fresh(): owned Cell => new Cell(); "
        "  public static method Take(): owned Cell => move P.Fresh(); "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6 bullet 1 admission: a BARE owned-field read through a NON-`owned`
// return is a legal BORROW (the cross-module `t3` canary shape
// `GetConn(): Connection => Conn`). The fix targets the OWNED-return path only
// — this shape must still compile.
BEZEL_TEST(bare_owned_field_owned_return_2368,
           bare_read_nonowned_return_borrow_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _c: Cell; "
        "  public constructor() _c := new Cell(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Cell => _c; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A BARE read of a NON-owned field through an `owned` return — `f` is not
// owned, so §28.6 bullet 2 does not fire; #2368's check is
// `exprIsOwnedFieldRead`-gated and correctly stays silent here.
//
// UPDATED for issue #3993: this row originally asserted "must compile
// through #2368's check" — true of #2368's OWN narrow scope, but the row's
// comment already flagged the gap ("a borrow-laundering shape caught
// elsewhere if at all") and #3993 is that "elsewhere": `f` is a plain
// (non-owned) field — a BORROW of whatever `Holder`'s constructor was handed
// — so `Take(): owned Foo => f;` hands the caller a second owner of an
// object `f0`'s real owner also disposes, exactly the issue's shape one
// level removed through a field instead of a bare parameter. The new
// `checkOwnedReturnDirectBorrow` (§28.3 *Borrow lifetime rules*, §28.6
// *Methods returning `this`*) now rejects it — measured via ASan on an
// analogous concrete repro (an `OwningArray<Cell>` borrowed field returned
// the same way SEGVs in `bz_array_free` on a second, disposed-already
// container).
BEZEL_TEST(bare_owned_field_owned_return_2368,
           nonowned_field_owned_return_rejected_spec_3993) {
    auto r = parseAndCheckOwnership(mod(
        "class Foo end Foo; "
        "class Holder "
        "  public f: Foo; "
        "  public constructor(f0: Foo) f := f0; end constructor; "
        "  public method Take(): owned Foo => f; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        errorSays(r, "a borrow cannot be returned as an `owned` value"));
    // #2368's OWN check must stay silent — this is #3993's diagnostic, not a
    // duplicate of #2368's "bare read of an owned field" one (the field here
    // is not `owned`).
    BEZEL_EXPECT_FALSE(errorSays(r, std::string(kRepro)));
}
