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

// §6.1 / Appendix C.11 (#958): a const initializer is a compile-time constant;
// an integer const whose folded value does not fit its declared type is a
// compile error (it raised OverflowException at runtime before).
BEZEL_TEST(sema_const, int_const_overflow_rejected_958) {
    auto r = parseSemaAll(cls("public const X: int := 2147483647 + 1;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not fit its declared type") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §6.1 (#958): a byte const out of range is rejected.
BEZEL_TEST(sema_const, byte_const_overflow_rejected_958) {
    auto r = parseSemaAll(cls("public const B: byte := 300;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// In-range integer consts (incl. a const-of-const and a long literal) are fine.
BEZEL_TEST(sema_const, in_range_int_consts_accepted_958) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public const Y: int := 1000 + 1; "
        "  public const Z: int := Y * 2; "
        "  public const Big: long := 5000000000L; "
        "  public const Neg: sbyte := -100; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
