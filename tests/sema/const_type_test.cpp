// Spec-derived tests for Sema slice S30 — a const member must have a
// primitive value type or string (§22.12).

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
    return std::string("module M; class T end T; class C ") + member +
        " end C;";
}

} // namespace

BEZEL_TEST(sema_const, int_const_ok) {
    auto r = parseSemaAll(cls("public const K: int := 5;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_const, string_const_ok) {
    auto r = parseSemaAll(cls("public const Name: string := \"hi\";"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_const, double_const_ok) {
    auto r = parseSemaAll(cls("public const Pi: double := 3.14;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_const, bool_const_ok) {
    auto r = parseSemaAll(cls("public const Flag: bool := true;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_const, named_type_const_diagnoses) {
    auto r = parseSemaAll(cls("public const X: T := null;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_const, object_const_diagnoses) {
    auto r = parseSemaAll(cls("public const O: Object := null;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
