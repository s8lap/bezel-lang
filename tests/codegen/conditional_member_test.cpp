// Regression for the `if`-expression result type (§20). A conditional
// `(a if c else b)` whose branches are reference types must itself be
// typed, so member access on its result resolves. Observable via the int
// exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runProg(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
const char* kN = "module M; class N public V: int := 0; end N; ";
} // namespace

BEZEL_TEST(codegen_if_member, member_on_conditional_result_then) {
    BEZEL_EXPECT_EQ(runProg(std::string(kN) +
        "class C public static method Main(): int "
        "  var a := new N(); a.V := 3; var b := new N(); b.V := 9; "
        "  var ok := true; var r := (a if ok else b); return r.V; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(codegen_if_member, member_on_conditional_result_else) {
    BEZEL_EXPECT_EQ(runProg(std::string(kN) +
        "class C public static method Main(): int "
        "  var a := new N(); a.V := 3; var b := new N(); b.V := 9; "
        "  var ok := false; var r := (a if ok else b); return r.V; "
        "end Main; end C;"), 9);
}

BEZEL_TEST(codegen_if_member, member_directly_on_conditional) {
    BEZEL_EXPECT_EQ(runProg(std::string(kN) +
        "class C public static method Main(): int "
        "  var a := new N(); a.V := 7; var b := new N(); b.V := 1; "
        "  var ok := true; return (a if ok else b).V; "
        "end Main; end C;"), 7);
}
