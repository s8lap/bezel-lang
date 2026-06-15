// Spec-derived tests for Sema slice S17 — class modifier mutual-
// exclusions (§11.7). `abstract`+`static` and `sealed`+`static` are
// compile errors; `sealed abstract` is permitted.

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

BEZEL_TEST(sema_class_mods, abstract_static_diagnoses) {
    auto r = parseSemaAll(
        "module M; abstract static class C end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, sealed_static_diagnoses) {
    auto r = parseSemaAll(
        "module M; sealed static class C end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, sealed_abstract_ok) {
    auto r = parseSemaAll(
        "module M; sealed abstract class Shape end Shape;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, plain_abstract_ok) {
    auto r = parseSemaAll(
        "module M; abstract class Shape end Shape;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, plain_static_ok) {
    auto r = parseSemaAll(
        "module M; static class Utils end Utils;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, plain_sealed_ok) {
    auto r = parseSemaAll(
        "module M; sealed class C end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §12.1 (R39): inheritance modifiers on a MEMBER are mutually exclusive ────
// `abstract`, `virtual`, `override`, and `sealed override` — at most one may
// appear. Previously `virtual abstract` / `virtual override` etc. were silently
// accepted, producing contradictory/ABI-corrupt dispatch.
namespace {
bool anyContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}
} // namespace

BEZEL_TEST(sema_class_mods, member_virtual_abstract_diagnoses_r39) {
    auto r = parseSemaAll(
        "module M; class B public virtual abstract method F(): int; end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyContains(r, "mutually exclusive"));
}

BEZEL_TEST(sema_class_mods, member_virtual_override_diagnoses_r39) {
    auto r = parseSemaAll(
        "module M; "
        "class B public virtual method F(): int return 1; end F; end B; "
        "class D: B public virtual override method F(): int return 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyContains(r, "mutually exclusive"));
}

BEZEL_TEST(sema_class_mods, member_abstract_override_diagnoses_r39) {
    auto r = parseSemaAll(
        "module M; "
        "class B public virtual method F(): int return 1; end F; end B; "
        "class D: B public abstract override method F(): int return 2; end F; end D;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyContains(r, "mutually exclusive"));
}

// `sealed` on a member is valid only as part of `sealed override`.
BEZEL_TEST(sema_class_mods, member_sealed_alone_diagnoses_r39) {
    auto r = parseSemaAll(
        "module M; class B public sealed method F(): int return 1; end F; end B;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyContains(r, "sealed"));
}

// Valid combinations still compile.
BEZEL_TEST(sema_class_mods, member_sealed_override_ok_r39) {
    auto r = parseSemaAll(
        "module M; "
        "class B public virtual method F(): int return 1; end F; end B; "
        "class D: B public sealed override method F(): int return 2; end F; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, member_plain_override_ok_r39) {
    auto r = parseSemaAll(
        "module M; "
        "class B public virtual method F(): int return 1; end F; end B; "
        "class D: B public override method F(): int return 2; end F; end D;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_class_mods, member_plain_virtual_ok_r39) {
    auto r = parseSemaAll(
        "module M; class B public virtual method F(): int return 1; end F; end B;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
