// Spec-derived tests for Sema slice S23 — record restrictions (§9.7).
// Records may not have a destructor, an `owned` field, an owning
// inline-array field, or implement `IDisposable`.

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

BEZEL_TEST(sema_record, owned_field_diagnoses) {
    auto r = parseSemaAll(
        "module M; class T end T; "
        "record R public owned _t: T; end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, destructor_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "record R public _x: int := 0; destructor end destructor; end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, implements_idisposable_diagnoses) {
    auto r = parseSemaAll(
        "module M; "
        "record R : IDisposable public _x: int := 0; end R;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, plain_record_ok) {
    auto r = parseSemaAll(
        "module M; "
        "record R public _x: int := 0; end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_record, class_owned_field_ok) {
    // The restriction is record-specific; a class may own a field.
    auto r = parseSemaAll(
        "module M; class T end T; "
        "class C private owned _t: T := new T(); end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
