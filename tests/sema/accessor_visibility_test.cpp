// Spec-derived tests for Sema slice S28 — accessor visibility must not
// be less restrictive than the property's overall visibility (§13.6).

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

std::string cls(const std::string& member) {
    return std::string("module M; class C ") + member + " end C;";
}

} // namespace

BEZEL_TEST(sema_acc_vis, private_set_on_public_prop_ok) {
    auto r = parseSemaAll(cls("public property X: int private set := 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_acc_vis, public_set_on_internal_prop_diagnoses) {
    auto r = parseSemaAll(cls("internal property X: int public set := 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_acc_vis, public_get_on_internal_prop_diagnoses) {
    auto r = parseSemaAll(cls("internal property X: int public get := 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_acc_vis, no_accessor_vis_ok) {
    auto r = parseSemaAll(cls("public property X: int get => 0; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_acc_vis, equal_rank_not_flagged) {
    // internal accessor on an internal property — equal restrictiveness;
    // not flagged (the rule only rejects *less* restrictive).
    auto r = parseSemaAll(cls("internal property X: int internal set := 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_acc_vis, private_set_on_internal_prop_ok) {
    auto r = parseSemaAll(cls("internal property X: int private set := 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
