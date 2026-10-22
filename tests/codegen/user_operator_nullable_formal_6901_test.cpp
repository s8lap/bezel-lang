// fix #6901 (§5.1, §7.11, v0.340): a non-nullable record argument to a
// user-defined binary operator whose parameter is a nullable value type.
//
//   §5.1 (v0.340), nullable conversions: "For any type T, T implicitly
//   converts to T?".
//
// Before the fix `p + new Pt(4, 4)` against `operator +(a: Pt?, b: Pt?)`
// passed the bare `%record.Pt` where the operator takes `{i8, %record.Pt}`:
// "Call parameter type does not match function signature!" / IR verification
// failed. Each row reads the operator's own null/present answer, so a missing
// or wrong carrier tag cannot hide.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kPt =
    "public record Pt(X: int, Y: int)\n"
    "  public static pure method operator +(a: Pt?, b: Pt?): int => 200 + (a?.X ?? 50) + (b?.X ?? 50);\n"
    "  public static pure method operator *(a: Pt, b: Pt?): int => 1000 + a.X + (b?.X ?? 50);\n"
    "end Pt;\n";

std::string program(const std::string& mainBody) {
    return std::string("module M;\n") + kPt +
           "public class P\n"
           "  public static method Pick(b: bool): Pt? if b then return new Pt(1, 1); end; return null; end Pick;\n"
           "  public static method Main(): int\n" + mainBody +
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

std::string runOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::string s = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) s += " | " + d.message;
        return s;
    }
    return r.stdoutText + "exit=" + std::to_string(r.exitCode);
}

}  // namespace

// The issue's repro, verbatim (c04c.bzl). 200 + 1 + 4.
BEZEL_TEST(codegen_user_operator_nullable_formal_6901, issue_repro_verbatim) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "  public static pure method operator +(a: Pt?, b: Pt?): int => 200 + (a?.X ?? 50) + (b?.X ?? 50);\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Pick(b: bool): Pt? if b then return new Pt(1, 1); end; return null; end Pick;\n"
        "  public static method Main(): int\n"
        "    var p: Pt? := P.Pick(true); var q: Pt? := P.Pick(false);\n"
        "    if p <> null then Console.WriteLine(p + new Pt(4, 4)); end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("205\nexit=0"));
}

BEZEL_TEST(codegen_user_operator_nullable_formal_6901, both_operands_bare) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var a := new Pt(4, 4);\n"
        "    Console.WriteLine(a + new Pt(5, 5));\n")),
        std::string("209\nexit=0"));
}

BEZEL_TEST(codegen_user_operator_nullable_formal_6901, bare_left_narrowed_right) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var p: Pt? := P.Pick(true);\n"
        "    if p <> null then Console.WriteLine(new Pt(4, 4) + p); end;\n")),
        std::string("205\nexit=0"));
}

// Mixed formals: a bare `Pt` first parameter and a `Pt?` second one.
BEZEL_TEST(codegen_user_operator_nullable_formal_6901, mixed_formals) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var a := new Pt(7, 7); var q: Pt? := P.Pick(false); var p: Pt? := P.Pick(true);\n"
        "    Console.WriteLine(a * q); Console.WriteLine(a * a); Console.WriteLine(a * p);\n"
        "    if p <> null then Console.WriteLine(p * a); end;\n")),
        std::string("1057\n1014\n1008\n1008\nexit=0"));
}

// Control: nullable operands at nullable formals (unchanged by the fix).
BEZEL_TEST(codegen_user_operator_nullable_formal_6901, nullable_operands_control) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var p: Pt? := P.Pick(true); var q: Pt? := P.Pick(false);\n"
        "    Console.WriteLine(p + q); Console.WriteLine(q + q);\n")),
        std::string("251\n300\nexit=0"));
}

BEZEL_TEST(codegen_user_operator_nullable_formal_6901, bare_temporaries_in_loop) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var s := 0;\n"
        "    for var i in 0 ..< 300 do s := s + (new Pt(i, i) + new Pt(1, 1)) - 200; end;\n"
        "    Console.WriteLine(s);\n")),
        std::string("45150\nexit=0"));
}
