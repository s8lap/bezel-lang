// fix #924 / §5.4 / §10: an enum is NOT implicitly convertible to or from its
// underlying integer type. Both directions (`var n: int := e` and
// `var e: E := n`) must be rejected with a §5.1 "not implicitly convertible"
// diagnostic; the explicit casts `int(e)` / `E(n)` remain accepted (§5.4).
//
// Before the fix the mixed Named(enum)↔Primitive case fell through both the
// Primitive-Primitive and Named-Named branches of checkAssignable and was
// silently accepted (and miscompiled). Enum→Object boxing is unaffected (it is
// accepted via the Object-target early-return).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {
bool hasNotConvertible(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("not implicitly convertible") != std::string::npos)
            return true;
    return false;
}
const char* kEnum = "module M; enum E A, B, C; end E; ";
}  // namespace

// §10: enum → underlying int is rejected.
BEZEL_TEST(enum_int_conversion_924, enum_to_int_diagnoses) {
    auto r = parseAndCheckOwnership(
        std::string(kEnum) +
        "class C public static method Main(): int "
        "  var e: E := E.C; var n: int := e; return n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotConvertible(r));
}

// §10: int → enum is rejected.
BEZEL_TEST(enum_int_conversion_924, int_to_enum_diagnoses) {
    auto r = parseAndCheckOwnership(
        std::string(kEnum) +
        "class C public static method Main(): int "
        "  var n: int := 2; var e: E := n; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotConvertible(r));
}

// §5.4: the explicit casts `int(e)` and `E(n)` remain accepted.
BEZEL_TEST(enum_int_conversion_924, explicit_casts_accepted) {
    auto r = parseAndCheckOwnership(
        std::string(kEnum) +
        "class C public static method Main(): int "
        "  var e: E := E.C; var n: int := int(e); var e2: E := E(2); "
        "  return n + int(e2); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.x: enum → Object boxing remains accepted (Object-target early-return).
BEZEL_TEST(enum_int_conversion_924, enum_to_object_boxing_accepted) {
    auto r = parseAndCheckOwnership(
        std::string(kEnum) +
        "class C public static method Main(): int "
        "  var e: E := E.B; var o: Object := e; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
