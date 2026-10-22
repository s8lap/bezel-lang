// Spec-derived regression test for issue #7006.
//
// Spec §30.2 (v0.340): an `async` body is a state machine whose locals survive
// an `await`; §19.3: a case-pattern binding (`var n`, a destructured component
// at any depth) is a local of its arm, and a case EXPRESSION arm (§19.7)
// introduces the same bindings as a case statement arm.
//
// THE DEFECT. #6976 reserved frame fields for bindings of case STATEMENT arms
// only; a binding introduced by a case-EXPRESSION arm and read after an
// `await` inside that arm came from a fresh stack slot (garbage; the issue
// repro printed `33|-30` / a random int, expected `33|-27` / `47`).
//
// Oracle: `runAotChecked`, exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}
}  // namespace

BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, issue_repro_var_binding_guard_and_positional) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "record Pair(A: Pt, B: string);\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method F(k: int): Task<int>\n"
            "    var r := (case k of var n when n > 0 => n + await T(n), var n => await T(n) - n);\n"
            "    return r;\n"
            "  end F;\n"
            "  public static async method G(p: Pt): Task<int>\n"
            "    var a := await T(1);\n"
            "    var r := (case p of Pt(x, y) => (await T(x)) + y + x, _ => 0);\n"
            "    return r + a;\n"
            "  end G;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await F(3));\n"
            "    Console.WriteLine(await F(-3));\n"
            "    Console.WriteLine(await G(new Pt(3, 4)));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("33\n-27\n47\n"));
}

BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, record_nested_guard_multi_await_alt) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "record Pair(A: Pt, B: string);\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method A(p: Pt): Task<int>\n"
            "    return (case p of var q => (await T(q.X)) + q.Y);\n"
            "  end A;\n"
            "  public static async method B(p: Pair): Task<int>\n"
            "    return (case p of Pair(Pt(x, y), b) => b.Length * 1000 + (await T(x)) + y, _ => 0);\n"
            "  end B;\n"
            "  public static async method C(p: Pt): Task<int>\n"
            "    return (case p of Pt(x, y) when x > y => (await T(x)) + y, Pt(x, y) => (await T(y)) - x, _ => 0);\n"
            "  end C;\n"
            "  public static async method D(k: int): Task<int>\n"
            "    return (case k of var n => (await T(n)) + (await T(n + 1)) + n);\n"
            "  end D;\n"
            "  public static async method E(k: int): Task<int>\n"
            "    return (case k of 1 | 2 => await T(k), var n => (await T(n)) + n);\n"
            "  end E;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await A(new Pt(2, 5)));\n"
            "    Console.WriteLine(await B(new Pair(new Pt(1, 2), \"z\")));\n"
            "    Console.WriteLine(await C(new Pt(5, 2)));\n"
            "    Console.WriteLine(await C(new Pt(2, 5)));\n"
            "    Console.WriteLine(await D(3));\n"
            "    Console.WriteLine(await E(2));\n"
            "    Console.WriteLine(await E(9));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("25\n1012\n52\n48\n73\n20\n99\n"));
}

BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, await_in_guard_nested_case_call_argument) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "record Pair(A: Pt, B: string);\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method A(k: int): Task<int>\n"
            "    return (case k of var n when (await T(n)) > 50 => n + 1000, var n => n + 2000);\n"
            "  end A;\n"
            "  public static async method B(k: int): Task<int>\n"
            "    return (case k of var n => (case n + 1 of var m => (await T(m)) + n + m));\n"
            "  end B;\n"
            "  public static async method C(k: int): Task<int>\n"
            "    return await T((case k of var n => n + (await T(n))));\n"
            "  end C;\n"
            "  public static async method D(p: Pt): Task<int>\n"
            "    var a := (case p of Pt(x, y) => (await T(x)) + y, _ => 0);\n"
            "    var b := (case a of var n => (await T(n)) - n);\n"
            "    return a * 1000 + b;\n"
            "  end D;\n"
            "  public static async method E(n: int): Task<int>\n"
            "    var tot := 0;\n"
            "    for var i in 0 .. n do\n"
            "      tot := tot + (case i of var j => (await T(j)) + j);\n"
            "    end for;\n"
            "    return tot;\n"
            "  end E;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await A(6));\n"
            "    Console.WriteLine(await A(2));\n"
            "    Console.WriteLine(await B(3));\n"
            "    Console.WriteLine(await C(2));\n"
            "    Console.WriteLine(await D(new Pt(1, 2)));\n"
            "    Console.WriteLine(await E(5));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("1006\n2002\n47\n220\n12108\n165\n"));
}

BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, string_binding_across_await_100x) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "record Pair(A: Pt, B: string);\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method F(k: string): Task<int>\n"
            "    var r := (case k of var s when s.Length > 3 => s.Length + (await T(s.Length)) + s.Length, var s => s.Length);\n"
            "    return r;\n"
            "  end F;\n"
            "  public static async method Main(): Task<int>\n"
            "    var tot := 0;\n"
            "    for var i in 0 .. 100 do\n"
            "      tot := tot + await F(\"abc\" + i.ToString());\n"
            "    end for;\n"
            "    Console.WriteLine(tot);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("5952\n"));
}

BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, case_statement_and_no_suspend_controls) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "record Pair(A: Pt, B: string);\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method F(k: int): Task<int>\n"
            "    var acc := 0;\n"
            "    case k of\n"
            "      var n when n > 0 => do acc := n + await T(n); end,\n"
            "      var n => do acc := await T(n) - n; end\n"
            "    end case;\n"
            "    return acc;\n"
            "  end F;\n"
            "  public static async method G(k: int): Task<int>\n"
            "    var a := await T(1);\n"
            "    var r := (case k of var n => n * 2);\n"
            "    return r + a;\n"
            "  end G;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await F(3));\n"
            "    Console.WriteLine(await F(-3));\n"
            "    Console.WriteLine(await G(4));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("33\n-27\n18\n"));
}

// #7006 review REJECT (q10, a REGRESSION of d9507134): §4.7 (v0.340) gives the
// unit type one value; a `var x` binding of a unit scrutinee in a case
// STATEMENT arm (§19.3) that stays live across an `await` (§30.2) needs a
// sized frame field. d9507134's reserveCaseBindingSlots typed it with a bare
// lowerTypeRef (void) instead of the shared slot rule (unitStorageTy, #3453),
// so the frame struct had a void member and bezelc died of SIGILL.
BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, unit_binding_case_statement_across_await_q10) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "public class Box(v: string)\n"
            "  public V: string := v;\n"
            "end Box;\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method S(x: string): Task<string>\n"
            "    await Task.Delay(1);\n"
            "    return x + \"!\";\n"
            "  end S;\n"
            "  public static async method F(k: int): Task<int>\n"
            "    var u: () := ();\n"
            "    case u of\n"
            "      var x => do\n"
            "        await Task.Delay(1);\n"
            "        Console.WriteLine(\"unit-stmt\");\n"
            "      end,\n"
            "    end;\n"
            "    return k;\n"
            "  end F;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await F(3));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("unit-stmt\n3\n"));
}

// #7006 review REJECT (q11): the case-EXPRESSION spelling (§19.3, §4.7, §30.2)
// of the q10 shape — same void frame field, same SIGILL on d9507134.
BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, unit_binding_case_expression_across_await_q11) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "record Pt(X: int, Y: int);\n"
            "public class Box(v: string)\n"
            "  public V: string := v;\n"
            "end Box;\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method S(x: string): Task<string>\n"
            "    await Task.Delay(1);\n"
            "    return x + \"!\";\n"
            "  end S;\n"
            "  public static async method F(k: int): Task<int>\n"
            "    var u: () := ();\n"
            "    var r := (case u of var x => (await T(k)));\n"
            "    return r;\n"
            "  end F;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await F(3));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("30\n"));
}

// #7006 fix-forward control: a unit case binding captured by a lambda across
// an await (§27.5 capture, §4.7, §19.3, §30.2), statement and expression form;
// the shared slot rule's captured-local widening + pre-box record apply.
// The lambdas capture only the binding: a captured PARAMETER across an await
// in a nested block leaks its cell independently of case (#3466 / #4522).
BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, unit_binding_lambda_capture_across_await_stmt_and_expr) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method F(k: int): Task<int>\n"
            "    var u: () := ();\n"
            "    case u of\n"
            "      var x => do\n"
            "        var f := lambda(): int => do var y: () := x; return 7; end;\n"
            "        await Task.Delay(1);\n"
            "        Console.WriteLine(f());\n"
            "      end,\n"
            "    end;\n"
            "    return k;\n"
            "  end F;\n"
            "  public static async method G(k: int): Task<int>\n"
            "    var u: () := ();\n"
            "    var r := (case u of var x => (lambda(): int => do var y: () := x; return 7; end)() + (await T(k)));\n"
            "    return r;\n"
            "  end G;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await F(3));\n"
            "    Console.WriteLine(await G(3));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("7\n3\n37\n"));
}

// #7006 review fix-forward (q12/q13): a case binding (§19.3) captured by a
// lambda (§27.5) BEFORE an `await` (§30.2) and read through the closure after
// it, statement and expression form. The capture moves the binding onto a
// cell, so its frame field must be the ptr-widened one with the pre-box type
// recorded (#808 / #3484); d9507134's bare lowerTypeRef slot dropped both and
// the resumed closure read a clobbered cell (SIGSEGV at run time).
BEZEL_TEST(codegen_async_case_expr_binding_across_await_7006, lambda_capture_before_await_stmt_and_expr) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P;\n"
            "public class Program\n"
            "  public static async method T(x: int): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return x * 10;\n"
            "  end T;\n"
            "  public static async method F(k: int): Task<int>\n"
            "    var r := 0;\n"
            "    case k of\n"
            "      var n => do\n"
            "        var f := lambda(): int => n * 2;\n"
            "        var a := await T(n);\n"
            "        r := a + f() + n;\n"
            "      end,\n"
            "    end;\n"
            "    return r;\n"
            "  end F;\n"
            "  public static async method G(k: int): Task<int>\n"
            "    var r := (case k of var n => (lambda(): int => n * 2)() + (await T(n)) + n);\n"
            "    return r;\n"
            "  end G;\n"
            "  public static async method Main(): Task<int>\n"
            "    Console.WriteLine(await F(3));\n"
            "    Console.WriteLine(await G(3));\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        std::string("39\n39\n"));
}
