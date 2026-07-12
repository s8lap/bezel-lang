// Regression: member access on the result of a `case` expression (§19) and
// an `as` cast (§17.4). Both must be typed so the member resolves.
// Observable via the int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runProg(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
const char* kN = "module M; class N public V: int := 0; "
                 "public method Self(): N => this; end N; ";
} // namespace

BEZEL_TEST(codegen_expr_member, member_on_case_result) {
    BEZEL_EXPECT_EQ(runProg(std::string(kN) +
        "class C public static method Main(): int "
        "  var a := new N(); a.V := 5; var b := new N(); b.V := 9; "
        "  var k := 1; var r := case k of 1 => a, _ => b, end; return r.V; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_expr_member, member_on_as_result) {
    BEZEL_EXPECT_EQ(runProg(std::string(kN) +
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 7; var o: N := n; "
        "  var r := (o as N); return r!.V; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_expr_member, case_result_else_branch) {
    BEZEL_EXPECT_EQ(runProg(std::string(kN) +
        "class C public static method Main(): int "
        "  var a := new N(); a.V := 5; var b := new N(); b.V := 9; "
        "  var k := 3; var r := case k of 1 => a, _ => b, end; return r.V; "
        "end Main; end C;"), 9);
}
