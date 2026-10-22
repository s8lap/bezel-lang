// fix #6830 (§7.11, §7.4, v0.340): a USER-DEFINED UNARY operator (`-`, `+`,
// `~`) on a nullable record/class operand.
//
//   §7.11 (v0.340): "A user-declared `operator` on a record or class type
//   (§25) is likewise not defined on a nullable operand: such an operand at a
//   user-defined-operator position is the same compile error, naming the same
//   remedies." and "an operand that §7.4 has narrowed non-null is a
//   non-nullable operand and the ordinary operator applies to it unchanged".
//
// Before the fix the unary path had no §7.11 gate at all (the binary path's
// `nullableUserOperand` has none of a twin here): `-p` on an un-narrowed
// `p: Pt?` reached codegen and died in IR verification ("Call parameter type
// does not match function signature!"), and so did the NARROWED spelling,
// because the `{i8, Pt}` carrier was passed to the bare-`Pt` operator.
//
// Accepted rows assert exact stdout under runAotChecked; negative rows assert
// the §7.11 diagnostic itself (not merely a failed compile).

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPt =
    "public record Pt(X: int, Y: int)\n"
    "  public static pure method operator -(a: Pt): Pt => new Pt(0 - a.X, 0 - a.Y);\n"
    "  public static pure method operator ~(a: Pt): Pt => new Pt(a.Y, a.X);\n"
    "  public static pure method operator +(a: Pt): int => a.X + a.Y;\n"
    "  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);\n"
    "end Pt;\n";

std::string program(const std::string& members, const std::string& mainBody) {
    return std::string("module M;\n") + kPt +
           "public class P\n"
           "  public static method Pick(b: bool): Pt?\n"
           "    if b then return new Pt(2, 3); end if;\n"
           "    return null;\n"
           "  end Pick;\n" +
           members +
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

// Rejected AND by the §7.11 user-operator diagnostic.
bool rejectedByUserOperatorRule(const std::string& src, const std::string& op) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("operator `" + op + "` is not defined on a nullable value "
                   "type") != std::string::npos &&
            m.find("narrow first") != std::string::npos &&
            m.find("user-defined operators do not lift") != std::string::npos &&
            m.find("7.11") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ---- the issue's own repros (u1.bzl, u2.bzl) ---------------------------------

// u1: un-narrowed. Before: IR verification failure, no diagnostic.
BEZEL_TEST(codegen_user_unary_operator_nullable_6830, issue_repro_unnarrowed_rejected) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "  public static pure method operator -(a: Pt): Pt => new Pt(0 - a.X, 0 - a.Y);\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Pick(): Pt?\n"
        "    return new Pt(2, 3);\n"
        "  end Pick;\n"
        "  public static method Main(): int\n"
        "    var p: Pt? := Pick();\n"
        "    var r := -p;\n"
        "    Console.WriteLine(r.X);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n", "-"));
}

// u2: narrowed under `if p <> null`. Expected -2. Before: IR verification failure.
BEZEL_TEST(codegen_user_unary_operator_nullable_6830, issue_repro_narrowed_runs) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "  public static pure method operator -(a: Pt): Pt => new Pt(0 - a.X, 0 - a.Y);\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var p: Pt? := new Pt(2, 3);\n"
        "    if p <> null then\n"
        "      var r := -p;\n"
        "      Console.WriteLine(r.X);\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("-2\nexit=0"));
}

// ---- accepted: narrowed by every §7.4 form ------------------------------------

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, tilde_and_plus_narrowed) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := P.Pick(true);\n"
        "    if p <> null then var r := ~p; Console.WriteLine(r.X); Console.WriteLine(+p); end if;\n")),
        std::string("3\n5\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, early_exit_narrows) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := P.Pick(true);\n"
        "    if p = null then return 1; end if;\n"
        "    var r := -p; Console.WriteLine(r.Y);\n")),
        std::string("-3\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, and_composition_narrows) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := P.Pick(true);\n"
        "    if p <> null and (-p).X = -2 then Console.WriteLine(\"ok\"); end if;\n")),
        std::string("ok\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, declaration_initializer_narrows) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(5, 6);\n"
        "    var r := -p; Console.WriteLine(r.X);\n")),
        std::string("-5\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, parameter_narrowed_both_paths) {
    BEZEL_EXPECT_EQ(runOut(program(
        "  public static method F(q: Pt?): int\n"
        "    if q = null then return 99; end if;\n"
        "    var r := -q; return r.X;\n"
        "  end F;\n",
        "    Console.WriteLine(P.F(P.Pick(true))); Console.WriteLine(P.F(P.Pick(false)));\n")),
        std::string("-2\n99\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, narrowed_in_loop_and_chain) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := P.Pick(true); var s := 0;\n"
        "    if p <> null then\n"
        "      for var i in 0 ..< 200 do s := s + (-p).X; end;\n"
        "      var z := -(-p + p); Console.WriteLine(z.X);\n"
        "    end if;\n"
        "    Console.WriteLine(s);\n")),
        std::string("0\n-400\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, readonly_field_narrows) {
    BEZEL_EXPECT_EQ(runOut(std::string("module M;\n") + kPt +
        "public class H\n"
        "  public readonly R: Pt?;\n"
        "  public constructor() R := new Pt(1, 2); end constructor;\n"
        "  public method UseR(): int if R <> null then return (-R).X; end if; return 0; end UseR;\n"
        "end H;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    using var h := new H();\n"
        "    Console.WriteLine(h.UseR());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("-1\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, generic_record_narrowed) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record Box<T>(V: T)\n"
        "  public static pure method operator -(a: Box<T>): Box<T> => a;\n"
        "end Box;\n"
        "public class P\n"
        "  public static method Pick(b: bool): Box<int>? if b then return new Box<int>(9); end if; return null; end Pick;\n"
        "  public static method Main(): int\n"
        "    var p: Box<int>? := P.Pick(true);\n"
        "    if p <> null then var r := -p; Console.WriteLine(r.V); end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("9\nexit=0"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, class_operand_narrowed) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public class C\n"
        "  public readonly V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "  public static method operator -(a: C): int => 0 - a.V;\n"
        "end C;\n"
        "public class P\n"
        "  public static method Pick(b: bool): owned C? if b then return new C(4); end if; return null; end Pick;\n"
        "  public static method Main(): int\n"
        "    using var p := P.Pick(true);\n"
        "    if p <> null then Console.WriteLine(-p); end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("-4\nexit=0"));
}

// A `T?` formal takes the nullable operand as an ordinary argument (the binary
// candidate search's nullability gate admits the same pairing), and a bare
// operand at that formal is wrapped (§5.1 `T` -> `T?`).
BEZEL_TEST(codegen_user_unary_operator_nullable_6830, nullable_formal_takes_any_operand) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "  public static pure method operator -(a: Pt?): int => 0 - (a?.X ?? 50);\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Pick(b: bool): Pt? if b then return new Pt(2, 3); end if; return null; end Pick;\n"
        "  public static method Main(): int\n"
        "    var p: Pt? := P.Pick(true); var q: Pt? := P.Pick(false);\n"
        "    Console.WriteLine(-p); Console.WriteLine(-q); Console.WriteLine(-new Pt(7, 7));\n"
        "    if p <> null then Console.WriteLine(-p); end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("-2\n-50\n-7\n-2\nexit=0"));
}

// Control: the remedies the diagnostic names compile and run.
BEZEL_TEST(codegen_user_unary_operator_nullable_6830, remedies_bang_and_coalesce) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := P.Pick(true); var q: Pt? := P.Pick(false);\n"
        "    var a := -p!; var b := -(q ?? new Pt(7, 8));\n"
        "    Console.WriteLine(a.X); Console.WriteLine(b.X);\n")),
        std::string("-2\n-7\nexit=0"));
}

// ---- negative: still nullable at the operator ----------------------------------

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, neg_tilde_unnarrowed) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := P.Pick(true);\n"
        "    var r := ~p; Console.WriteLine(r.X);\n"), "~"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, neg_plus_unnarrowed) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := P.Pick(true);\n"
        "    Console.WriteLine(+p);\n"), "+"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, neg_parameter_unnarrowed) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program(
        "  public static method F(q: Pt?): int\n"
        "    var r := -q; return r.X;\n"
        "  end F;\n",
        "    Console.WriteLine(P.F(P.Pick(true)));\n"), "-"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, neg_reassigned_after_test) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := P.Pick(true);\n"
        "    if p <> null then p := P.Pick(false); var r := -p; Console.WriteLine(r.X); end if;\n"), "-"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, neg_mutable_field_never_narrows) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(std::string("module M;\n") + kPt +
        "public class H\n"
        "  public M: Pt?;\n"
        "  public constructor() M := new Pt(3, 4); end constructor;\n"
        "  public method UseM(): int if M <> null then return (-M).X; end if; return 0; end UseM;\n"
        "end H;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    using var h := new H();\n"
        "    Console.WriteLine(h.UseM());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n", "-"));
}

BEZEL_TEST(codegen_user_unary_operator_nullable_6830, neg_class_operand_unnarrowed) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(
        "module M;\n"
        "public class C\n"
        "  public readonly V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "  public static method operator -(a: C): int => 0 - a.V;\n"
        "end C;\n"
        "public class P\n"
        "  public static method Pick(b: bool): owned C? if b then return new C(4); end if; return null; end Pick;\n"
        "  public static method Main(): int\n"
        "    using var p := P.Pick(true);\n"
        "    Console.WriteLine(-p);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n", "-"));
}
