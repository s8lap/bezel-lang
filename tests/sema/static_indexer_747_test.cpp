// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT + miscompile) — a
// `static` indexer.
//
// Authority: §23.5 — "Static indexers are not supported." Bug: a static indexer
// (`public static property [i: int]: int => ...`) was silently accepted and
// then miscompiled (the access lowered to garbage). The fix rejects it at the
// declaration; a normal instance indexer is unaffected.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasStaticIndexerError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("static indexers are not supported") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// §23.5: a static indexer is rejected.
BEZEL_TEST(static_indexer_747, static_indexer_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static property [i: int]: int => i * 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasStaticIndexerError(r));
}

// §23.5: an ordinary instance indexer is fine — no error.
BEZEL_TEST(static_indexer_747, instance_indexer_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _d: Array<int> := [10, 20, 30]; "
        "  public property [i: int]: int => _d[i]; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasStaticIndexerError(r));
}
