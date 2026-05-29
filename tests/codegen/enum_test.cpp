// Codegen slice CG30 — enums (§10). An enum is a value type backed by an
// integer; each constant lowers to its computed value (explicit `:= N`,
// else previous + 1 from 0). Enum-typed locals are integers, and enum
// equality is an integer comparison. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& enumDecl, const std::string& body) {
    auto r = runJit(
        "module M; " + enumDecl +
        " class C public static method Main(): int " + body +
        " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

const char* kColor = "enum Color Red := 1, Green := 2, Blue := 3; end Color;";

} // namespace

BEZEL_TEST(codegen_cg30, constant_equality) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Green; return (7 if c = Color.Green else 0);"), 7);
}

BEZEL_TEST(codegen_cg30, distinct_constants_not_equal) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Red; return (1 if c = Color.Green else 9);"), 9);
}

BEZEL_TEST(codegen_cg30, inequality_operator) {
    BEZEL_EXPECT_EQ(run(kColor,
        "return (4 if Color.Red <> Color.Blue else 0);"), 4);
}

BEZEL_TEST(codegen_cg30, reassignment) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Red; c := Color.Blue; "
        "return (3 if c = Color.Blue else 0);"), 3);
}

BEZEL_TEST(codegen_cg30, used_in_if_condition) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Blue; "
        "if c = Color.Blue then return 11; end; return 0;"), 11);
}

BEZEL_TEST(codegen_cg30, implicit_auto_increment_values) {
    // A, B, C with no explicit values are 0, 1, 2 — so A <> C and they
    // round-trip through a local.
    BEZEL_EXPECT_EQ(run(
        "enum E A, B, C; end E;",
        "var x := E.C; return (8 if x = E.C else 0);"), 8);
}

BEZEL_TEST(codegen_cg30, explicit_then_implicit_values) {
    // A := 5, B (implicit → 6): B and A are distinct, B equals itself.
    BEZEL_EXPECT_EQ(run(
        "enum E A := 5, B, C; end E;",
        "var x := E.B; "
        "return (6 if (x = E.B) and (x <> E.A) else 0);"), 6);
}
