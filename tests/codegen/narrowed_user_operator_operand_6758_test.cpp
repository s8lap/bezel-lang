// fix #6758 (§7.11, §7.4, v0.340): a nullable record operand that §7.4 has
// narrowed non-null is a non-nullable operand at a USER-DEFINED operator
// position.
//
//   §7.11 (v0.340): "an operand that §7.4 has narrowed non-null is a
//   non-nullable operand and the ordinary operator applies to it unchanged";
//   a user-defined operator "is likewise not defined on a nullable operand:
//   such an operand at a user-defined-operator position is the same compile
//   error, naming the same remedies".
//   §7.4 (v0.340): "a null test in either spelling -- `x <> null` or
//   `x is not null` (narrowing the true branch)", early exit, `and`
//   composition, and the declaration-initializer form.
//
// Before the fix the gate keyed on the declared `Pt?` type alone and rejected
// `p + new Pt(10, 10)` inside `if p <> null then`, although the built-in `int?`
// is admitted there. Admitting the operand also needs the `{i8, Pt}` carrier
// unwrapped in codegen: the first draft of the sema change alone died in IR
// verification ("Call parameter type does not match function signature").
//
// Each accepted row asserts the exact stdout (the sum is read, so a carrier
// passed in place of the payload cannot hide); each NEGATIVE row must still
// be rejected with the §7.11 diagnostic.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPt =
    "public record Pt(X: int, Y: int)\n"
    "  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);\n"
    "  public static pure method operator -(a: Pt, b: Pt): Pt => new Pt(a.X - b.X, a.Y - b.Y);\n"
    "  public static pure method operator *(a: Pt, k: int): Pt => new Pt(a.X * k, a.Y * k);\n"
    "  public static pure method operator <(a: Pt, b: Pt): bool => a.X < b.X;\n"
    "  public static pure method operator >(a: Pt, b: Pt): bool => a.X > b.X;\n"
    "  public static pure method operator <=(a: Pt, b: Pt): bool => a.X <= b.X;\n"
    "  public static pure method operator >=(a: Pt, b: Pt): bool => a.X >= b.X;\n"
    "end Pt;\n";

const char* kHelpers =
    "  public static method Pick(b: bool): Pt?\n"
    "    if b then return new Pt(2, 3); end if;\n"
    "    return null;\n"
    "  end Pick;\n"
    "  public static method Clear(ref p: Pt?)\n"
    "    p := null;\n"
    "  end Clear;\n";

std::string program(const std::string& members, const std::string& mainBody,
                    const std::string& extraTypes = "") {
    return std::string("module M;\n") + kPt + extraTypes +
           "public class P\n" + kHelpers + members +
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

// Rejected AND by the §7.11 user-operator diagnostic (not an unrelated error).
bool rejectedByUserOperatorRule(const std::string& src) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("is not defined on a nullable value type") !=
                std::string::npos &&
            m.find("narrow first") != std::string::npos &&
            m.find("7.11") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ---- the issue's own repro (c14_r1.bzl), verbatim ---------------------------

// Expected: prints 11. Before: compile error at `p + new Pt(10, 10)`.
BEZEL_TEST(codegen_narrowed_user_operator_6758, issue_repro_verbatim) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var p: Pt? := new Pt(1, 2);\n"
        "    if p <> null then\n"
        "      var r := p + new Pt(10, 10);\n"
        "      Console.WriteLine(r.X);\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("11\nexit=0"));
}

// ---- accepted rows: the fact holds on every path to the operator -----------

BEZEL_TEST(codegen_narrowed_user_operator_6758, right_operand_narrowed) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(1, 2);\n"
        "    if p <> null then var r := new Pt(10, 10) + p; Console.WriteLine(r.X); end if;\n")),
        std::string("11\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, both_operands_narrowed_and) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(1, 2);\n"
        "    var q: Pt? := new Pt(5, 5);\n"
        "    if p <> null and q <> null then var r := p + q; Console.WriteLine(r.X); end if;\n")),
        std::string("6\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, is_not_null_spelling_minus) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(1, 2);\n"
        "    if p is not null then var r := p - new Pt(10, 10); Console.WriteLine(r.X); end if;\n")),
        std::string("-9\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, early_return_narrows) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := Pick(true);\n"
        "    if p = null then return 1; end if;\n"
        "    var r := p + p;\n"
        "    Console.WriteLine(r.X);\n")),
        std::string("4\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, and_composition_operand) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := Pick(true);\n"
        "    if p <> null and (p + new Pt(1, 1)).X = 3 then Console.WriteLine(\"hit\"); end if;\n")),
        std::string("hit\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, declaration_initializer_narrows) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(1, 2);\n"
        "    var r := p + p;\n"
        "    Console.WriteLine(r.X);\n")),
        std::string("2\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, comparison_operators) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(1, 2);\n"
        "    if p <> null then\n"
        "      Console.WriteLine(p < new Pt(5, 5));\n"
        "      Console.WriteLine(p > new Pt(5, 5));\n"
        "      Console.WriteLine(new Pt(1, 9) >= p);\n"
        "    end if;\n")),
        std::string("True\nFalse\nTrue\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, operator_with_primitive_second_param) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(3, 2);\n"
        "    if p <> null then var r := p * 4; Console.WriteLine(r.X); end if;\n")),
        std::string("12\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, parameter_both_paths) {
    BEZEL_EXPECT_EQ(runOut(program(
        "  public static method F(p: Pt?): int\n"
        "    if p <> null then var r := p + p; return r.X; end if;\n"
        "    return -1;\n"
        "  end F;\n",
        "    Console.WriteLine(F(new Pt(4, 4)));\n"
        "    Console.WriteLine(F(null));\n")),
        std::string("8\n-1\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, readonly_field_narrows) {
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var c := new Cn(new Pt(7, 7));\n"
        "    c.Run();\n",
        "public class Cn\n"
        "  public readonly Q: Pt?;\n"
        "  public constructor(q: Pt?)\n"
        "    this.Q := q;\n"
        "  end constructor;\n"
        "  public method Run(): int\n"
        "    if this.Q <> null then\n"
        "      var r := this.Q + new Pt(1, 1);\n"
        "      Console.WriteLine(r.X);\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Run;\n"
        "end Cn;\n")),
        std::string("8\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, while_condition_and_loop_carried) {
    // The fact is re-established by the `for` body's own test each iteration.
    BEZEL_EXPECT_EQ(runOut(program("",
        "    var p: Pt? := new Pt(1, 1);\n"
        "    for var i in 0..3 do\n"
        "      if p = null then break; end if;\n"
        "      p := p + new Pt(i, i);\n"
        "      Console.WriteLine(p.X);\n"
        "    end for;\n")),
        std::string("1\n2\n4\n7\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, generic_record_operator) {
    BEZEL_EXPECT_EQ(runOut(std::string(
        "module M;\n"
        "public record Box<T>(V: T)\n"
        "  public static pure method operator +(a: Box<T>, b: Box<T>): Box<T> => new Box<T>(b.V);\n"
        "end Box;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var p: Box<int>? := new Box<int>(5);\n"
        "    if p <> null then var r := p + new Box<int>(6); Console.WriteLine(r.V); end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n")),
        std::string("6\nexit=0"));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, class_operand_owned_result) {
    BEZEL_EXPECT_EQ(runOut(std::string(
        "module M;\n"
        "public class V\n"
        "  public readonly N: int;\n"
        "  public constructor(n: int)\n"
        "    this.N := n;\n"
        "  end constructor;\n"
        "  public static method operator +(a: V, b: V): owned V => new V(a.N + b.N);\n"
        "end V;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    using var p: V? := new V(3);\n"
        "    if p <> null then\n"
        "      using var q := new V(4);\n"
        "      using var r := p + q;\n"
        "      Console.WriteLine(r.N);\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n")),
        std::string("7\nexit=0"));
}

// The null-safe equality (§8.4) is unchanged by the fix.
BEZEL_TEST(codegen_narrowed_user_operator_6758, equality_unchanged_control) {
    BEZEL_EXPECT_EQ(runOut(std::string(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var p: Pt? := new Pt(1, 2);\n"
        "    var q: Pt? := null;\n"
        "    if p <> null then Console.WriteLine(p = new Pt(1, 2)); end if;\n"
        "    Console.WriteLine(q = null);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n")),
        std::string("True\nTrue\nexit=0"));
}

// ---- NEGATIVE rows: the fact does NOT hold, the rejection must remain ------

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_never_narrowed) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    var r := p + new Pt(10, 10);\n"
        "    Console.WriteLine(r.X);\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_reassigned_nullable_after_test) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    if p <> null then p := Pick(false); var r := p + new Pt(1, 1); Console.WriteLine(r.X); end if;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_else_branch) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    if p <> null then Console.WriteLine(1); else var r := p + p; Console.WriteLine(r.X); end if;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_after_if_join) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    if p <> null then Console.WriteLine(1); end if;\n"
        "    var r := p + p;\n"
        "    Console.WriteLine(r.X);\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_or_does_not_narrow) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    if p <> null or true then var r := p + p; Console.WriteLine(r.X); end if;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_other_operand_not_narrowed) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    var q: Pt? := Pick(true);\n"
        "    if p <> null then var r := p + q; Console.WriteLine(r.X); end if;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_ref_argument_invalidates) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    if p <> null then Clear(ref p); var r := p + p; Console.WriteLine(r.X); end if;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_mutably_captured_then_call) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    var f := lambda() => do p := null; end;\n"
        "    if p <> null then f(); var r := p + p; Console.WriteLine(r.X); end if;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_null_branch_of_equality_test) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(false);\n"
        "    if p = null then var r := p + p; Console.WriteLine(r.X); end if;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_mutable_field_never_narrows) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var c := new Mc();\n"
        "    c.Run();\n",
        "public class Mc\n"
        "  public Q: Pt?;\n"
        "  public method Run(): int\n"
        "    this.Q := new Pt(1, 1);\n"
        "    if this.Q <> null then\n"
        "      var r := this.Q + new Pt(1, 1);\n"
        "      Console.WriteLine(r.X);\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Run;\n"
        "end Mc;\n")));
}

BEZEL_TEST(codegen_narrowed_user_operator_6758, neg_comparison_never_narrowed) {
    BEZEL_EXPECT_TRUE(rejectedByUserOperatorRule(program("",
        "    var p: Pt? := Pick(true);\n"
        "    Console.WriteLine(p < new Pt(1, 1));\n")));
}
