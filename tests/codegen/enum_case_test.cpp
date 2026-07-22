// Codegen slice CG43 — enum constants as `case` patterns (§19/§10). A bare
// qualified name `E.Red` (a NamedPattern) and enum-constant alternatives
// (`E.Red, E.Green` — a ConstantPattern) are resolved against the
// scrutinee's enum type by name and tested for equality. Observable via the
// int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runProg(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
const char* kE = "module M; enum E Red, Green, Blue; end E; ";
} // namespace

BEZEL_TEST(codegen_cg43, expr_case_selects_matching_arm) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var e := E.Green; "
        "  return (case e of E.Red => 1, E.Green => 2, E.Blue => 3,); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(codegen_cg43, expr_case_first_arm) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var e := E.Red; return (case e of E.Red => 5, _ => 0,); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg43, expr_case_discard_fallback) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var e := E.Blue; return (case e of E.Red => 1, _ => 8,); "
        "end Main; end C;"), 8);
}

BEZEL_TEST(codegen_cg43, expr_case_alternatives) {
    // `E.Red, E.Green => …` — a ConstantPattern with enum-constant
    // alternatives; either matches.
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var e := E.Green; "
        "  return (case e of E.Red | E.Green => 4, E.Blue => 9,); "
        "end Main; end C;"), 4);
}

BEZEL_TEST(codegen_cg43, explicitly_typed_scrutinee) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var e: E := E.Blue; "
        "  return (case e of E.Red => 1, E.Green => 2, E.Blue => 7,); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg43, statement_position_enum_case) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var e := E.Green; var r := 0; "
        "  case e of E.Red => do r := 1; end, E.Green => do r := 6; end, end; "
        "  return r; "
        "end Main; end C;"), 6);
}

BEZEL_TEST(codegen_cg43, guarded_enum_arm) {
    BEZEL_EXPECT_EQ(runProg(std::string(kE) +
        "class C public static method Main(): int "
        "  var e := E.Green; "
        "  return (case e of E.Green when false => 1, E.Green => 3, _ => 0,); "
        "end Main; end C;"), 3);
}
