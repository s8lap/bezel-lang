// Regression tests for #1082 (§28.3): the borrow-into-field escape check
// (#386/#402) must fire for an UNQUALIFIED field name, not only the qualified
// `this.f` / `obj.f` forms. Storing a borrow of a scope-bound owned local into
// a bare field (`_f := localBorrow`, implicit `this`, or an unqualified static
// field) outlives the borrow's source — a use-after-free — and must be rejected
// with the same §28.3 diagnostic the qualified form already produces.
// checkBorrowStoreEscape classified an IdentifierExpr target only as an
// out/ref parameter or an outer-block local, never as a FieldSymbol, so the
// bare form silently escaped the check.
//
// parseAndCheckOwnership(src).diagnostics.hasErrors() is the reject signal.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {
bool rejects(const std::string& src) {
    return parseAndCheckOwnership(src).diagnostics.hasErrors();
}
const char* kCfg =
    "module M; class Cfg public _v: int := 0; "
    "  public constructor(v: int) _v := v; end constructor; end Cfg; ";
}  // namespace

// The #1082 repro: a bare instance-field borrow store is rejected.
BEZEL_TEST(borrow_into_bare_field_1082, bare_instance_field_borrow_rejected) {
    BEZEL_EXPECT_TRUE(rejects(std::string(kCfg) +
        "class Holder public _cfg: Cfg?; "
        "  public method Set() "
        "    using var cfg := new Cfg(99); "
        "    _cfg := cfg; "                     // bare `this._cfg` — UAF, reject
        "  end Set; end Holder;"));
}

// The qualified `this._cfg := cfg` was already rejected — still is.
BEZEL_TEST(borrow_into_bare_field_1082, qualified_this_field_borrow_rejected) {
    BEZEL_EXPECT_TRUE(rejects(std::string(kCfg) +
        "class Holder public _cfg: Cfg?; "
        "  public method Set() "
        "    using var cfg := new Cfg(99); "
        "    this._cfg := cfg; "
        "  end Set; end Holder;"));
}

// A bare unqualified STATIC field borrow store is rejected too.
BEZEL_TEST(borrow_into_bare_field_1082, bare_static_field_borrow_rejected) {
    BEZEL_EXPECT_TRUE(rejects(std::string(kCfg) +
        "class C public static _cfg: Cfg?; "
        "  public static method Set() "
        "    using var cfg := new Cfg(99); "
        "    _cfg := cfg; "
        "  end Set; end C;"));
}

// A bare OWNED field receiving a `move` of an owned local is a legal transfer,
// NOT a borrow store — it must still be accepted (the owned-field move-on-read
// skip recognises a bare-identifier owned field).
BEZEL_TEST(borrow_into_bare_field_1082, bare_owned_field_move_accepted) {
    BEZEL_EXPECT_FALSE(rejects(std::string(kCfg) +
        "class Holder public owned _c: Cfg?; "
        "  public method Set() "
        "    var c := new Cfg(5); "
        "    _c := move c; "                    // owned transfer — legal
        "  end Set; end Holder;"));
}

// Storing a borrow of a PARAMETER (which outlives the method) into a bare field
// is legal — the param's referent is the caller's, not a scope-bound local.
BEZEL_TEST(borrow_into_bare_field_1082, bare_field_borrow_of_param_accepted) {
    BEZEL_EXPECT_FALSE(rejects(std::string(kCfg) +
        "class Holder public _cfg: Cfg?; "
        "  public method Set(c: Cfg) "
        "    _cfg := c; "                        // borrow of a param — admitted
        "  end Set; end Holder;"));
}
