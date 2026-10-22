// Regression tests for #6980 (spec v0.340): §8.4 "`null = nonNull` and
// `nonNull = null` are `false`. `<>` is the negation ... over a nullable
// operand, `a = b` ... decide the absent cases first ... and only when both
// operands are present dispatch to the overloaded operator". `p = null` with
// `p` a non-nullable record crashed bezelc (SIGSEGV in emitStructEquality,
// which extracted fields from the null literal's `ptr`).
//
// ORACLE: runAot / runAotChecked (real AOT path), exact stdout.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
const char* kDecls =
    "public record Pt(X: int, Y: int) end Pt; "
    "public class C end C; "
    "public record U(X: int) "
    "  public static method Say(m: string, v: bool): bool Console.WriteLine(m); return v; end Say; "
    "  public static method operator =(a: U, b: U): bool => Say(\"user=\", a.X = b.X); "
    "  public static method operator <>(a: U, b: U): bool => Say(\"user<>\", a.X <> b.X); "
    "  public method Equals(o: U): bool => X = o.X; "
    "  public method GetHashCode(): int => X; "
    "end U; ";
std::string out(const std::string& body) {
    auto r = runAot(std::string("module M; ") + kDecls +
        "public class P public static method G<T>(t: T): bool return t = null; end G; "
        "public static method Main(): int " + body + " return 0; end Main; end P;");
    return r.ran && r.exitCode == 0 ? r.stdoutText : "RC!=0";
}
}  // namespace

BEZEL_TEST(record_null_equality_6980, spec_8_4_record_eq_null_right) {
    BEZEL_EXPECT_EQ(out("var p := new Pt(1, 1); Console.WriteLine(p = null);"), std::string("False\n"));
}
BEZEL_TEST(record_null_equality_6980, spec_8_4_record_ne_null_right) {
    BEZEL_EXPECT_EQ(out("var p := new Pt(1, 1); Console.WriteLine(p <> null);"), std::string("True\n"));
}
BEZEL_TEST(record_null_equality_6980, spec_8_4_null_left_both_ops) {
    BEZEL_EXPECT_EQ(out("var p := new Pt(1, 1); Console.WriteLine(null = p); Console.WriteLine(null <> p);"),
                    std::string("False\nTrue\n"));
}
BEZEL_TEST(record_null_equality_6980, spec_8_4_class_operand) {
    BEZEL_EXPECT_EQ(out("var c := new C(); Console.WriteLine(c = null); Console.WriteLine(null <> c);"),
                    std::string("False\nTrue\n"));
}
// The user operator must NOT run (no "user=" / "user<>" sentinel line).
BEZEL_TEST(record_null_equality_6980, spec_8_4_user_operator_not_invoked_against_null) {
    BEZEL_EXPECT_EQ(out("var u := new U(1); Console.WriteLine(u = null); Console.WriteLine(u <> null); "
                        "Console.WriteLine(null = u); Console.WriteLine(null <> u);"),
                    std::string("False\nTrue\nFalse\nTrue\n"));
}
BEZEL_TEST(record_null_equality_6980, spec_8_4_generic_param_bound_to_record) {
    BEZEL_EXPECT_EQ(out("var p := new Pt(1, 1); Console.WriteLine(G<Pt>(p));"), std::string("False\n"));
}
// Owned temporary against null: nothing leaks (#6940 release path).
BEZEL_TEST(record_null_equality_6980, spec_8_4_owned_temporary_vs_null_no_leak) {
    auto r = runAotChecked(
        "module M; record S(N: string) end S; public class P public static method Main(): int "
        "var c := 0; for var i in 0 ..< 100 do "
        "if new S(\"a\" + i.ToString()) = null then c := c + 1; end; "
        "if new S(\"b\" + i.ToString()) <> null then c := c + 2; end; end; "
        "Console.WriteLine(c); return 0; end Main; end P;");
    BEZEL_EXPECT_EQ(r.ran && r.exitCode == 0 ? r.stdoutText : std::string("RC!=0"), std::string("200\n"));
}
