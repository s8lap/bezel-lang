// Spec-derived tests for Sema slice S36 — constructor chain-call
// placement (§12.8): at most one `base(...)`/`this(...)`, only as the
// first statement.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

BEZEL_TEST(sema_ctor_chain, base_call_first_ok) {
    auto r = parseSemaAll(
        "module M; class B public constructor() end constructor; end B; "
        "class D : B "
        "  public _x: int := 0; "
        "  public constructor() base(); this._x := 1; end constructor; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ctor_chain, this_call_first_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public constructor() this._x := 1; end constructor; "
        "  public constructor(seed: int) this(); end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ctor_chain, chain_call_not_first_diagnoses) {
    auto r = parseSemaAll(
        "module M; class B public constructor() end constructor; end B; "
        "class D : B "
        "  public _x: int := 0; "
        "  public constructor() this._x := 1; base(); end constructor; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ctor_chain, two_chain_calls_diagnoses) {
    auto r = parseSemaAll(
        "module M; class B public constructor() end constructor; end B; "
        "class D : B "
        "  public constructor() base(); base(); end constructor; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_ctor_chain, no_chain_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public constructor() this._x := 1; end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
