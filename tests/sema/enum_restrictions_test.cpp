// Spec-derived tests for Sema slice S32 — enum member restrictions
// (§10.5): no instance fields, constructors, nested types, indexers;
// no virtual/abstract/override methods. Static members allowed (§10.4).

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

BEZEL_TEST(sema_enum, plain_enum_ok) {
    auto r = parseSemaAll(
        "module M; enum Color Red, Green, Blue; end Color;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, enum_static_method_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public static method Count(): int => 2; "
        "end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, instance_field_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public _x: int := 0; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, static_field_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public static _x: int := 0; end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, constructor_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public constructor() end constructor; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, virtual_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public virtual method M() _ := 1; end M; "
        "end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, instance_method_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public method Describe(): int => 1; "
        "end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.3 (#740): a member value that exceeds the declared underlying type's
// range is a compile error (it previously wrapped silently at codegen).
BEZEL_TEST(sema_enum, byte_member_over_255_rejected) {
    auto r = parseSemaAll("module M; enum E: byte A := 256; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §10.3: an unsigned underlying type rejects a negative value.
BEZEL_TEST(sema_enum, byte_member_negative_rejected) {
    auto r = parseSemaAll("module M; enum E: byte A := -1; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §10.3: a value that overflows a `short` (but fits `int`) is rejected.
BEZEL_TEST(sema_enum, short_member_over_range_rejected) {
    auto r = parseSemaAll("module M; enum E: short A := 40000; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §10.3 regression guard: in-range values, an unsigned boundary, a signed
// minimum, and AUTO-INCREMENT continuation are all accepted.
BEZEL_TEST(sema_enum, in_range_and_auto_increment_ok) {
    auto r = parseSemaAll(
        "module M; "
        "enum B: byte Lo := 0, Mid := 200, Hi := 255; end B; "
        "enum S: short A := -32768, Z := 32767; end S; "
        "enum Inc: byte X := 253, Y, Z; end Inc;");  // 253, 254, 255 — all fit
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.3: auto-increment that overflows the underlying type is rejected
// (`byte X := 255, Y;` → Y is 256, out of range).
BEZEL_TEST(sema_enum, auto_increment_overflow_rejected) {
    auto r = parseSemaAll(
        "module M; enum E: byte X := 255, Y; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
