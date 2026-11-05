// Spec-derived regression test for issue #8289.
//
// §27.3 (v0.355, *Admission at a target-typed non-call position*): a lambda's
// body is typed and its inferred return type "must equal the target's return
// slot or implicitly convert to it (§5.1)". §5.1: `Array<T>` (and the other
// collection types) implicitly convert to `IEnumerable<T>`, which §29.2
// requires to be a vtable-backed enumerator.
//
// THE DEFECT. A lambda whose body yields an `Array<T>` at an `IEnumerable<T>`
// return slot handed the RAW array pointer on as the enumerable, and the
// caller's for-in read the array header as a vtable (SIGSEGV), while a plain
// method (control row) converted correctly. Fix: lowerLambda's expression body
// and a block lambda's `return` run wrapAsIEnumerableForTarget like a method.
//
// The leak-checked rows (runAotChecked) hand the lambda to a method whose
// parameter is the function slot, so the collection local is never the
// argument of a closure call in the function that owns it: a closure call on
// an owned Array local leaks that local once whenever the function declares
// another local (separate defect, filed as a candidate in the handback). The
// issue's literal repros (c02, c03) run under runAot, stdout only.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kHelpers =
    "public class H "
    "  public static method RunA(xs: Array<int>, "
    "      f: (Array<int>) -> IEnumerable<int>): int "
    "    var n := 0; for var v in f(xs) do n := n + v; end; return n; "
    "  end RunA; "
    "  public static method RunSet(s: Set<int>, "
    "      f: (Set<int>) -> IEnumerable<int>): int "
    "    var n := 0; for var v in f(s) do n := n + v; end; return n; "
    "  end RunSet; "
    "  public static method RunStack(s: Stack<int>, "
    "      f: (Stack<int>) -> IEnumerable<int>): string "
    "    var t := \"\"; for var v in f(s) do t := t + $\"{v}\"; end; return t; "
    "  end RunStack; "
    "  public static method RunQueue(q: Queue<int>, "
    "      f: (Queue<int>) -> IEnumerable<int>): string "
    "    var t := \"\"; for var v in f(q) do t := t + $\"{v}\"; end; return t; "
    "  end RunQueue; "
    "  public static method Cnt<U>(xs: Array<U>, "
    "      f: (Array<U>) -> IEnumerable<U>): int "
    "    var n := 0; for var v in f(xs) do n := n + 1; end; return n; "
    "  end Cnt; "
    "  public static method Go(xs: Array<int>): int "
    "    => Cnt(xs, lambda(l) => l); "
    "  public static method Id(l: Array<int>): IEnumerable<int> => l; "
    "end H; ";

JitResult run(const char* mod, const std::string& body) {
    return runAotChecked(std::string("module ") + mod + "; " + kHelpers +
                         "public class M public static method Main(): int " +
                         body + " return 0; end Main; end M;");
}

}  // namespace

#define ROW(NAME, MOD, BODY, EXPECT)                                          \
    BEZEL_TEST(codegen_lambda_array_ienumerable_return_8289, NAME) {          \
        JitResult r = run(MOD, BODY);                                         \
        BEZEL_EXPECT_TRUE(r.ran);                                             \
        BEZEL_EXPECT_EQ(r.exitCode, 0);                                       \
        BEZEL_EXPECT_EQ(r.stdoutText, std::string(EXPECT));                   \
    }

ROW(method_control_spec_5_1, "M8289a",
    "var xs: Array<int> := [1, 2, 3]; "
    "Console.WriteLine($\"{H.RunA(xs, lambda(l) => H.Id(l))}\");", "6\n")
ROW(slot_expr_body_spec_27_3, "M8289b",
    "var xs: Array<int> := [1, 2, 3]; "
    "Console.WriteLine($\"{H.RunA(xs, lambda(l) => l)}\");", "6\n")
ROW(ascribed_expr_body_spec_27_3, "M8289c",
    "var xs: Array<int> := [1, 2, 3]; "
    "Console.WriteLine($\"{H.RunA(xs, "
    "lambda(l: Array<int>): IEnumerable<int> => l)}\");", "6\n")
ROW(slot_block_body_spec_27_3, "M8289d",
    "var xs: Array<int> := [1, 2, 3]; "
    "Console.WriteLine($\"{H.RunA(xs, lambda(l) => do return l; end)}\");",
    "6\n")
ROW(ascribed_block_body_spec_27_3, "M8289e",
    "var xs: Array<int> := [1, 2, 3]; "
    "Console.WriteLine($\"{H.RunA(xs, "
    "lambda(l: Array<int>): IEnumerable<int> => do return l; end)}\");",
    "6\n")
ROW(lazy_where_body_spec_27_3, "M8289f",
    "var xs: Array<int> := [1, 2, 3]; "
    "Console.WriteLine($\"{H.RunA(xs, "
    "lambda(l) => l.Where(lambda(x) => x > 1))}\");", "5\n")
ROW(generic_slot_spec_27_3, "M8289g",
    "var xs: Array<int> := [1, 2, 3]; "
    "Console.WriteLine($\"{H.Go(xs)}\");", "3\n")
ROW(set_body_spec_27_3, "M8289h",
    "var s := new Set<int>(); s.Add(4); s.Add(5); "
    "Console.WriteLine($\"{H.RunSet(s, lambda(l) => l)}\");", "9\n")
ROW(stack_body_spec_27_3, "M8289i",
    "var s := new Stack<int>(); s.Push(1); s.Push(2); s.Push(3); "
    "Console.WriteLine(H.RunStack(s, lambda(l) => l));", "321\n")
ROW(queue_body_spec_27_3, "M8289j",
    "var q := new Queue<int>(); q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
    "Console.WriteLine(H.RunQueue(q, lambda(l) => l));", "123\n")
ROW(loop_100x_no_leak_spec_27_3, "M8289k",
    "var xs: Array<int> := [1, 2, 3]; var t := 0; var i := 0; "
    "while i < 100 do "
    "t := t + H.RunA(xs, lambda(l) => l) + "
    "H.RunA(xs, lambda(l) => do return l; end) + "
    "H.RunA(xs, lambda(l) => l.Where(lambda(x) => x > 1)); "
    "i := i + 1; end; "
    "Console.WriteLine($\"{t}\");", "1700\n")

// The issue's literal repros (c02, c03): a local function value called in
// Main. runAot (stdout only): see the header on the unrelated local-closure
// leak.
BEZEL_TEST(codegen_lambda_array_ienumerable_return_8289,
           issue_repro_ascribed_local_c02) {
    JitResult r = runAot(
        "module M8289m; public class C public static method Main(): int "
        "var xs: Array<int> := [1, 2, 3]; "
        "var f := lambda(l: Array<int>): IEnumerable<int> => l; "
        "var n := 0; for var v in f(xs) do n := n + 1; end; "
        "Console.WriteLine(n); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(codegen_lambda_array_ienumerable_return_8289,
           issue_repro_slot_local_c03) {
    JitResult r = runAot(
        "module M8289n; public class C public static method Main(): int "
        "var xs: Array<int> := [1, 2, 3]; "
        "var f: (Array<int>) -> IEnumerable<int> := lambda(l) => l; "
        "var n := 0; for var v in f(xs) do n := n + 1; end; "
        "Console.WriteLine(n); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(codegen_lambda_array_ienumerable_return_8289,
           issue_repro_lazy_where_local_p13) {
    JitResult r = runAot(
        "module M8289o; public class C public static method Main(): int "
        "var xs: Array<int> := [1, 2, 3]; "
        "var f: (Array<int>) -> IEnumerable<int> := "
        "lambda(l) => l.Where(lambda(x) => x > 1); "
        "for var v in f(xs) do Console.WriteLine(v); end; "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n3\n"));
}
