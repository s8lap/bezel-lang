// Spec-derived sema tests for v0.167 (#1371) the constants-only membership rule
// for `|` pattern alternatives — §19.3: "every member must resolve to a
// constant ... a member that resolves to a type is a compile error in a `case`
// pattern; grouped type tests are written as separate arms." A binding member
// is likewise rejected. The rule applies "in any position" — at arm level and
// inside a positional destructuring slot.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff a diagnostic mentions the §19.3 alternative-membership rule.
bool hasAltMembershipError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("alternative list") != std::string::npos)
            return true;
    return false;
}

} // namespace

// ---- a TYPE member in a `|` alternative list is an error -------------------

BEZEL_TEST(sema_alt_1371, type_member_in_alternative_rejected) {
    // `Dog | Cat` are type tests, not constants — §19.3 requires separate arms.
    auto r = semaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "class Cat: Animal end Cat; "
        "class C public static method M(a: Animal): int "
        "  return case a of Dog | Cat => 1, _ => 0 end; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAltMembershipError(r));
}

BEZEL_TEST(sema_alt_1371, type_member_in_per_slot_alternative_rejected) {
    // §19.3 "in any position": a type inside a positional-slot alternative list.
    auto r = semaAll(
        "module M; "
        "class Animal end Animal; "
        "class Dog: Animal end Dog; "
        "record Box(a: Animal); "
        "class C public static method M(b: Box): int "
        "  return case b of Box(Dog | Animal) => 1, _ => 0 end; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAltMembershipError(r));
}

// ---- a BINDING member in a `|` alternative list is an error ----------------

BEZEL_TEST(sema_alt_1371, binding_member_in_alternative_rejected) {
    // `1 | var y` — a binding may not appear in an alternative list (caught at
    // parse; the compile still fails).
    auto r = semaAll(
        "module M; class C public static method M(x: int): int "
        "  return case x of 1 | var y => 1, _ => 0 end; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- a valid constant/enum alternative list is accepted --------------------

BEZEL_TEST(sema_alt_1371, enum_constant_alternatives_accepted) {
    // Enum constants ARE constants — `Red | Blue` is a valid alternatives list.
    auto r = semaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C public static method M(c: Color): int "
        "  return case c of Red | Blue => 1, Green => 2 end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAltMembershipError(r));
}

BEZEL_TEST(sema_alt_1371, qualified_enum_constant_not_flagged_as_type) {
    // `Color.Red | Color.Blue` — the head `Color` is a type but the members are
    // enum constants, not types, so the membership rule does NOT fire.
    auto r = semaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C public static method M(c: Color): int "
        "  return case c of Color.Red | Color.Blue => 1, _ => 0 end; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(hasAltMembershipError(r));
}
