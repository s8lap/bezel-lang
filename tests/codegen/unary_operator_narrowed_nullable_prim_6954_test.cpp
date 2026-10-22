// fix #6954 (§7.11, §7.4, v0.340): a BUILT-IN unary `-` / `~` / `+` over a
// nullable primitive that §7.4 has narrowed non-null.
//
//   §7.11 (v0.340): "The rule reads the operand's static type, so an operand
//   that §7.4 has narrowed non-null is a non-nullable operand and the ordinary
//   operator applies to it unchanged". §7.4: a declaration initializer, an
//   `if x <> null`, and `is not null` narrow.
//
// Before the fix `lowerUnary`'s stale "lifted" arm built a `{i8, T}` result for
// an expression sema types `T`: `-i` over `var i: int? := 5` printed `1` (the
// has-value flag), and `-d` / `-m` over `double?` / `decimal?` failed IR
// verification. The un-narrowed control stays a §7.11 compile error.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

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

std::string prog(const std::string& body) {
    return "module M;\npublic class P\n  public static method Main(): int\n" +
           body + "    return 0;\n  end Main;\nend P;\n";
}

// One operator over one nullable type, narrowed three ways, printed through
// WriteLine and an interpolation hole.
std::string rows(const std::string& ty, const std::string& lit,
                 const std::string& op) {
    return prog(
        "    var a: " + ty + " := " + lit + ";\n"
        "    Console.WriteLine(" + op + "a);\n"
        "    Console.WriteLine($\"{" + op + "a}\");\n"
        "    var b: " + ty + " := " + lit + ";\n"
        "    if b <> null then Console.WriteLine(" + op + "b); end if;\n"
        "    var c: " + ty + " := " + lit + ";\n"
        "    if c is not null then Console.WriteLine($\"[{" + op + "c}]\"); end if;\n");
}

}  // namespace

BEZEL_TEST(codegen_unary_narrowed_nullable_6954, issue_repro) {
    BEZEL_EXPECT_EQ(runOut(prog(
        "    var i: int? := 5;\n"
        "    Console.WriteLine(-i ?? 0);\n"
        "    Console.WriteLine(-i);\n"
        "    Console.WriteLine(-(i ?? 0));\n"
        "    var k: int := -i;\n"
        "    Console.WriteLine(k);\n")),
        std::string("-5\n-5\n-5\n-5\nexit=0"));
}

BEZEL_TEST(codegen_unary_narrowed_nullable_6954, neg_int)     { BEZEL_EXPECT_EQ(runOut(rows("int?", "5", "-")),     std::string("-5\n-5\n-5\n[-5]\nexit=0")); }
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, neg_long)    { BEZEL_EXPECT_EQ(runOut(rows("long?", "7L", "-")),   std::string("-7\n-7\n-7\n[-7]\nexit=0")); }
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, neg_double)  { BEZEL_EXPECT_EQ(runOut(rows("double?", "2.5", "-")), std::string("-2.5\n-2.5\n-2.5\n[-2.5]\nexit=0")); }
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, neg_decimal) { BEZEL_EXPECT_EQ(runOut(rows("decimal?", "3.5m", "-")), std::string("-3.5\n-3.5\n-3.5\n[-3.5]\nexit=0")); }
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, tilde_int)   { BEZEL_EXPECT_EQ(runOut(rows("int?", "6", "~")),     std::string("-7\n-7\n-7\n[-7]\nexit=0")); }
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, tilde_long)  { BEZEL_EXPECT_EQ(runOut(rows("long?", "6L", "~")),   std::string("-7\n-7\n-7\n[-7]\nexit=0")); }
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, plus_int)    { BEZEL_EXPECT_EQ(runOut(rows("int?", "5", "+")),     std::string("5\n5\n5\n[5]\nexit=0")); }
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, plus_double) { BEZEL_EXPECT_EQ(runOut(rows("double?", "2.5", "+")), std::string("2.5\n2.5\n2.5\n[2.5]\nexit=0")); }

// A parameter narrowed by `if p <> null`, and the OverflowException of §6.1
// that the narrowed operand still carries.
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, narrowed_parameter) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\npublic class P\n"
        "  public static method F(p: int?): int\n"
        "    if p <> null then return -p; end if;\n"
        "    return 0;\n"
        "  end F;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(F(9));\n"
        "    Console.WriteLine(F(null));\n"
        "    return 0;\n"
        "  end Main;\nend P;\n"),
        std::string("-9\n0\nexit=0"));
}

BEZEL_TEST(codegen_unary_narrowed_nullable_6954, narrowed_min_value_still_overflows) {
    auto r = runAot(prog(
        "    var i: int? := int.MinValue;\n"
        "    var j: int := -i;\n"
        "    Console.WriteLine(j);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode != 0);
}

// Control: the un-narrowed operand stays a §7.11 compile error.
BEZEL_TEST(codegen_unary_narrowed_nullable_6954, unnarrowed_control_rejected) {
    auto r = runAot(prog(
        "    var i: int? := null;\n"
        "    Console.WriteLine(-i);\n"));
    BEZEL_EXPECT_TRUE(!r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not lift over `T?`") != std::string::npos ||
            d.message.find("is not defined on a nullable value type") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}
