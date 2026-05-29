// Codegen slice CG23 — the postfix null-forgiving operator `!` raises
// NullReferenceException when its operand is null (§17.12). The `null`
// literal lowers to a null reference. Built-in exception support (CG22)
// makes the raise catchable. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class T end T; "
                    "class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg23, bang_on_null_throws) {
    BEZEL_EXPECT_EQ(run(
        "var x: T? := null; "
        "try var y := x!; return 0; "
        "catch NullReferenceException => return 55; end;"), 55);
}

BEZEL_TEST(codegen_cg23, bang_on_null_caught_by_exception_root) {
    BEZEL_EXPECT_EQ(run(
        "var x: T? := null; "
        "try var y := x!; return 0; "
        "catch Exception => return 7; end;"), 7);
}

BEZEL_TEST(codegen_cg23, bang_on_non_null_passes_through) {
    // A non-null reference passes through `!` without throwing.
    BEZEL_EXPECT_EQ(run(
        "var p := new T(); "
        "try var q := p!; return 1; "
        "catch NullReferenceException => return 0; end;"), 1);
}

BEZEL_TEST(codegen_cg23, bang_null_propagates_uncaught) {
    // No matching handler here; the NullReferenceException reaches the
    // outer try.
    BEZEL_EXPECT_EQ(run(
        "var x: T? := null; "
        "try "
        "  try var y := x!; return 0; "
        "  catch T => return 1; end; "
        "catch NullReferenceException => return 8; end;"), 8);
}
