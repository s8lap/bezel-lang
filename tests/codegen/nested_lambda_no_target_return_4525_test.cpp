// Codegen — issue #4525 / spec §27.3 (v0.338): a lambda formed INSIDE another
// lambda's body, at a position that supplies no target type, takes its return
// type from its own body — never from the enclosing lambda's target.
//
// THE DEFECT. `lowerLambda` resolves an "unresolved" (`i32`-sentinel) return
// type through `expectedFunctionType_`, the target the enclosing expression
// armed for the lambda it is lowering. That member stayed armed while the
// lambda's BODY was lowered, so a second lambda formed in the body with no
// target of its own (`var g := lambda() => k;`) read the OUTER lambda's target
// and took the outer's return type whenever its own was `int`:
//
//   * outer at `(…) -> ()` (an event handler, any procedure slot): the inner
//     closure was DECLARED `void` and CALLED as `i32`. The call site read the
//     result register as left by whatever ran last — the issue's "130 on the
//     plain build, 18 under ASan, where 5 is correct", and an OverflowException
//     once looped. No sanitizer sees it: nothing is freed or out of bounds.
//   * outer at `(…) -> string`: `ret i32` in a `ptr` function — the IR verifier
//     ("Function return type does not match operand type of return inst").
//
// The issue's title blames the `this`-bounded environment outliving `Hook`'s
// frame. Measured, the lifetime is fine (the #4509 row beside it passes and the
// cell is live at `Fire`); the same wrong value reproduces with no `this`, no
// event and no escape at all — rows 3–5 below — because the only ingredient
// is "an int-returning lambda with no target, inside a lambda that has one".
//
// §27.3: "Expression-bodied lambdas need no inference rule: the body
// expression's type is the return type", and for a block body *Return-type
// inference without a target*: "In a context with no target type, a
// value-returning block-bodied lambda's return type is inferred only when
// every `return e;` expression in the body has the same type".
//
// Every row asserts a value that READS the captured data through the inner
// closure, and every row runs under `runAotChecked` (generated code ASan).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Rows 3+ share this prelude: a procedure slot, a string slot, and a slot
// that returns a function.
const char* const kPrelude =
    "module M; "
    "class C "
    "  public static method Run(f: (int) -> ()) f(2); end Run; "
    "  public static method RunS(f: (int) -> string): string return f(2); end RunS; "
    "  public static method RunF(f: (int) -> () -> string): string "
    "    return f(2)(); "
    "  end RunF; "
    "  public static method Main(): int "
    "    var k := 3; ";
const char* const kEpilogue = "    return 0; end Main; end C;";

std::string prog(const std::string& body) {
    return std::string(kPrelude) + body + kEpilogue;
}

} // namespace

// Row 1 — THE ISSUE'S OWN REPRO, verbatim (it spells the event with the
// retired `event` member kind, which the compiler still admits — #1727).
// Pre-fix: exit 130 plain / 18 under ASan. 2 (raised) + 3 (`k`, read through
// the nested closure `g`) = 5.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           issue_repro_this_bounded_handler_with_one_nested_lambda_spec_27_3) {
    auto r = runAotChecked(
        "module M; "
        "record Ev(N: int); "
        "class Widget "
        "  public event Clicked: Ev; "
        "  public owned sub: Subscription?; "
        "  public T: int := 0; "
        "  public method Tally(x: int) T := T + x; end Tally; "
        "  public method Hook() "
        "    var k := 3; "
        "    this.sub := this.Clicked.Subscribe(lambda(e) => do "
        "      var g := lambda() => k; "
        "      this.Tally(e.N + g()); "
        "    end); "
        "  end Hook; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  w.Hook(); "
        "  w.Fire(2); "
        "  return w.T; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Row 2 — the same shape in the stdlib §6.5 migration spelling, looped 200x
// with three raises per widget (the issue's "wrapped in a `for` loop raises
// OverflowException" variant). Each raise adds 2 + 3; 200 * 3 * 5 = 3000.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           event_source_handler_nested_lambda_looped_spec_27_3) {
    auto r = runAotChecked(
        "module M; "
        "record Ev(N: int); "
        "class Widget "
        "  private owned readonly _clicked: EventSource<Ev> := new(); "
        "  public owned sub: Subscription?; "
        "  public T: int := 0; "
        "  public method Tally(x: int) T := T + x; end Tally; "
        "  public method Hook() "
        "    var k := 3; "
        "    this.sub := _clicked.Subscribe(lambda(e) => do "
        "      var g := lambda() => k; "
        "      this.Tally(e.N + g()); "
        "    end); "
        "  end Hook; "
        "  public method Fire(n: int) _clicked.Raise(new Ev(n)); end Fire; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  for var it in 1..200 do "
        "    using var w := new Widget(); "
        "    w.Hook(); "
        "    w.Fire(2); w.Fire(2); w.Fire(2); "
        "    total := total + w.T; "
        "  end; "
        "  Console.WriteLine(total); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3000\n"));
}

// Row 3 — no `this`, no event, no escape: the outer lambda is a plain argument
// at a `(int) -> ()` parameter and is invoked during the call. Pre-fix the
// program printed a garbage number. Three inner spellings — an expression
// body, a typed parameter, a block body with a `return` — one per line.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           procedure_slot_outer_nested_int_lambda_three_spellings_spec_27_3) {
    auto r = runAotChecked(prog(
        "Run(lambda(e) => do "
        "  var g := lambda() => k; "
        "  Console.WriteLine(e + g()); "
        "end); "
        "Run(lambda(e) => do "
        "  var g := lambda(x: int) => x + k; "
        "  Console.WriteLine(e + g(10)); "
        "end); "
        "Run(lambda(e) => do "
        "  var g := lambda() => do if e > 0 then return k; end; return 0; end; "
        "  Console.WriteLine(e + g()); "
        "end); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n15\n5\n"));
}

// Row 4 — three lambdas deep, the innermost reading its own parameter, the
// middle lambda's parameter and the captured local: 2 + (100 + 10 + 3) = 115.
// Pre-fix both inner closures were declared `void`.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           procedure_slot_outer_three_deep_reads_every_level_spec_27_3) {
    auto r = runAotChecked(prog(
        "Run(lambda(e) => do "
        "  var g := lambda(a: int) => do "
        "    var h := lambda(b: int) => a + b + k; "
        "    return h(10); "
        "  end; "
        "  Console.WriteLine(e + g(100)); "
        "end); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("115\n"));
}

// Row 5 — the other target-less positions inside a procedure-slot lambda: an
// immediately-invoked lambda and a lambda returned by a lambda. Pre-fix these
// printed 8250 and a garbage number.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           procedure_slot_outer_iife_and_lambda_returning_lambda_spec_27_3) {
    auto r = runAotChecked(prog(
        "Run(lambda(e) => do "
        "  var g := lambda() => (lambda() => k)(); "
        "  Console.WriteLine(e + g()); "
        "end); "
        "Run(lambda(e) => do "
        "  var g := lambda() => lambda() => k + 1; "
        "  Console.WriteLine(e + g()()); "
        "end); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n6\n"));
}

// Row 6 — outer at a `-> string` slot. Pre-fix the inner closure was declared
// `ptr` over an `int` body and the compile died in the IR verifier.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           string_slot_outer_nested_int_lambda_compiles_and_reads_spec_27_3) {
    auto r = runAotChecked(prog(
        "var s := RunS(lambda(e) => do "
        "  var g := lambda() => k; "
        "  return $\"v{e + g()}\"; "
        "end); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v5\n"));
}

// Row 7 — outer at a slot that RETURNS a function. The body's function-typed
// result position legitimately keeps a target (the returned lambda takes
// `() -> string`), but a helper closure declared ahead of it has none: the
// local-declaration half of the fix. Pre-fix: IR verifier, as row 6.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           function_returning_slot_outer_helper_local_lambda_spec_27_3) {
    auto r = runAotChecked(prog(
        "var s := RunF(lambda(e) => do "
        "  var g := lambda() => k; "
        "  var n := e + g(); "
        "  return lambda() => $\"v{n}\"; "
        "end); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v5\n"));
}

// CONTROL (unchanged by the fix) — the target-driven readings the fallback
// exists for still hold when the target IS the lambda's own: an untyped
// parameter takes the slot's parameter type, an `int` expression body at a
// procedure slot is the §27.3 discarded-result form, and a nested lambda at a
// call ARGUMENT takes that parameter's function type.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           control_own_target_still_drives_the_lambda_spec_27_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Run(f: (int) -> ()) f(2); end Run; "
        "  public static method Ap(f: (int) -> int): int return f(40); end Ap; "
        "  public static method Main(): int "
        "    var k := 3; "
        "    Run(lambda(e) => do Console.WriteLine(e + Ap(lambda(x) => x + k)); end); "
        "    Run(lambda(e) => e + k); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("45\n"));
}

// fix-forward #4525/#6284 (§27.5 / §28.14 / §28.9, v0.338): the control's
// shape looped 200x. `k`'s capture cell is reached by the outer procedure-slot
// lambda and, through it, by a lambda literal at a call-bounded argument
// position of its body. That inner lambda is dropped when `Ap` returns, so
// the cell is freed at `Main`'s scope end. Under LeakSanitizer (runAotChecked)
// a cell nobody frees is 200 reported leaks, not one hidden by a stale pointer.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           call_bounded_nested_lambda_in_procedure_slot_frees_the_cell_looped_spec_27_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Run(f: (int) -> ()) f(2); end Run; "
        "  public static method Ap(f: (int) -> int): int return f(40); end Ap; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      var k := i; "
        "      Run(lambda(e) => do total := total + e + Ap(lambda(x) => x + k); end); "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("28300\n"));
}

// fix-forward #4525 (§27.3 / §27.5 / §28.9, v0.338): row 5's IIFE shape looped
// 200x. `(lambda() => k)()` sits in the body of `g`, itself bound by `var g :=`
// inside the procedure-slot lambda. The IIFE closure is released right after
// its call, so `k`'s cell is freed once, at the loop body's scope end. Before
// the fix the #1135 widening marked `k` escaping and nobody freed the cell.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           nested_iife_in_procedure_slot_frees_the_cell_looped_spec_27_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Run(f: (int) -> ()) f(2); end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      var k := i; "
        "      Run(lambda(e) => do "
        "        var g := lambda() => (lambda() => k)(); "
        "        total := total + e + g(); "
        "      end); "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("20300\n"));
}

// fix-forward #4525 (§27.3 / §27.5 / §28.9, v0.338): row 5's lambda-returning-
// lambda shape looped 200x. Every call of `g` is invoked at once (`g()()`), so
// each closure `g` returns is released inside the expression and `k`'s cell is
// freed once, at the loop body's scope end.
BEZEL_TEST(nested_lambda_no_target_return_4525,
           lambda_returning_lambda_invoked_at_once_frees_the_cell_looped_spec_27_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Run(f: (int) -> ()) f(2); end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      var k := i; "
        "      Run(lambda(e) => do "
        "        var g := lambda() => lambda() => k + 1; "
        "        total := total + e + g()(); "
        "      end); "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("20500\n"));
}

// fix-forward #4525 (§27.5 / §28.9, v0.338): the use-after-free guard. The
// closure `g` returns is STORED in `h` and called twice after `g`'s own frame
// is gone, then `k` is read again in `Main` after `Run` returns. The cell must
// outlive `h` and every read of `k`, and still be freed exactly once (ASan:
// no heap-use-after-free, no double free, no leak over 200 iterations).
BEZEL_TEST(nested_lambda_no_target_return_4525,
           returned_inner_lambda_stored_and_called_later_no_uaf_looped_spec_27_5) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Run(f: (int) -> ()) f(2); end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      var k := i; "
        "      Run(lambda(e) => do "
        "        var g := lambda() => lambda() => k + 1; "
        "        var h := g(); "
        "        total := total + e + h() + h(); "
        "      end); "
        "      total := total + k; "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("60500\n"));
}
