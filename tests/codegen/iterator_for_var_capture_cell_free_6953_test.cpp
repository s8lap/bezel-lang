// Codegen — issue #6953 (§27.5 Closures, §21.4 The `for` statement, §29.3
// Iterator methods, spec v0.359): a lambda capturing a `for` loop variable
// inside an ITERATOR body leaked the variable's capture cell on every
// iteration (4 B for an `int`), allocated in the generated MoveNext.
//
// Mechanism: lowerForBody pre-boxes a captured for-var into a fresh
// per-iteration cell and, when collectCellFreeEligible proves every capturing
// lambda non-escaping, frees it at the iteration's body end (#793). That
// pre-boxing was gated on `!smStateStruct_`, so in a state-machine body the
// for-var was instead boxed lazily by lowerLambda, and nothing freed that
// cell. The gate existed because a state-machine for-var is a suspend-local:
// a holder created in lowerForBody would not survive a `yield` unless it took
// over the slot's frame field. The fix performs that same field move (the one
// lowerLambda does for its lazy boxing), so the holder is spilled and restored
// across a suspension and the body-end free runs after the resume. The gate
// covered every state machine, so async methods, async iterators and async
// lambdas leaked the same way; their rows are at the end. The pre-boxing
// slot's own frame field (needed for the loop to survive the suspension at
// all) is #6211's fix, made in the same commit.
//
// Every non-CONTROL row runs under runAotChecked (generated-code ASan +
// LeakSanitizer) and is RED on the base (leak report, and in one row a wrong
// value too). The two CONTROL rows use runAotCheckedLenient: green on the base,
// they pin the escape side (a yielded lambda keeps its cell) against a fix that
// frees the cell without consulting cellFreeEligible_.
// per §27.5, §21.4, §29.3 (issue #6953)

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
std::string lenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// The issue's exact repro: 100 iterations of an iterator `for` body, each
// binding a lambda over the for-var and yielding its result. Base: prints
// 49500 and leaks 400 B / 100 (one 4-B cell per iteration).
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           issue_repro_capture_for_var_in_iterator_x100_spec_27_5_21_4) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      yield return f(); "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var v in L(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "49500\n");
}

// The lambda is formed BEFORE a `yield` and invoked AFTER the resume, in the
// same iteration: the cell must survive the suspension (freed only at the
// iteration's body end). Base: leaks 400 B / 100.
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           lambda_formed_before_yield_invoked_after_resume_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      yield return 1; "
        "      yield return f(); "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var v in L(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "49600\n");
}

// The capture is inside an `if` arm, reached on even iterations only. §21.4:
// each lambda sees the value of that specific iteration. Base: prints 26950
// (WRONG; 24500 + 2500 = 27000) and leaks 200 B / 50.
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           conditional_capture_reads_this_iterations_value_spec_21_4_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      if y mod 2 = 0 then "
        "        var f := lambda(): int => y * 10; "
        "        yield return f(); "
        "      else "
        "        yield return y; "
        "      end; "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var v in L(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "27000\n");
}

// A `string` for-in variable over an Array, captured and read across two
// yields, 100 enumerations of 3 elements. Base: leaks 2400 B / 300.
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           string_for_in_capture_x100_enumerations_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(xs: Array<string>): IEnumerable<int> "
        "    for var s in xs do "
        "      var f := lambda(): int => s.Length; "
        "      yield return f(); "
        "      yield return f(); "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var xs: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      for var v in L(xs) do total := total + v; end; "
        "    end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "1200\n");
}

// Nested `for` loops; the lambda captures both for-vars. Base: leaks
// 2400 B / 600 (one cell per for-var per inner iteration).
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           nested_for_both_vars_captured_spec_21_4_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      for var z in 0..<3 do "
        "        var f := lambda(): int => y * 10 + z; "
        "        yield return f(); "
        "      end; "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var v in L(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "148800\n");
}

// The for-var is read by a `yield return y` and only then captured, after the
// resume. Base: leaks the 400 B / 100 cells.
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           capture_after_a_yield_in_the_same_iteration_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      yield return y; "
        "      var f := lambda(): int => y * 10; "
        "      yield return f(); "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var v in L(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "54450\n");
}

// Two lambdas capture the same for-var in one iteration (§27.5: they share the
// cell) and are invoked across two yields. One free per iteration, not two.
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           two_lambdas_share_one_iteration_cell_spec_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      var g := lambda(): int => y + 1; "
        "      yield return f(); "
        "      yield return g(); "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var v in L(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "54550\n");
}

// `yield break` at y = 50, after the lambda is formed: the cell of the
// iteration that leaves via `yield break` is freed too.
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           yield_break_mid_loop_frees_the_cell_spec_29_3_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      if y = 50 then yield break; end; "
        "      yield return f(); "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var v in L(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "12250\n");
}

// The body throws at y = 7, after forming the lambda; 50 enumerations, each
// caught by the consumer. The cell must be freed once on the throw edge
// (no double-free with the per-state cleanup).
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           throw_after_capture_x50_spec_31_1_27_5) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<int> "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      if y = 7 then throw new InvalidOperationException(\"x\"); end; "
        "      yield return f(); "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var i in 0..<50 do "
        "      try "
        "        for var v in L(100) do total := total + v; end; "
        "      catch "
        "        InvalidOperationException => total := total + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "10550\n");
}

// BOUNDARY CONTROL 1 (green on base). The lambda itself is yielded out and
// invoked by the consumer: it escapes, so its cell must NOT be freed at the
// iteration end. Leak detection is off (the escaping closure's own leak is
// pre-existing and out of scope); the generated-code ASan is on. RED under the
// plausible wrong fix that frees the cell without the escape check
// (forVarFreshCell set regardless of cellFreeEligible_).
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           CONTROL_escaping_yielded_lambda_keeps_its_cell_spec_27_5) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<() -> int> "
        "    for var y in 0..<n do "
        "      yield return lambda(): int => y * 10; "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    for var g in L(100) do total := total + g(); end; "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "49500\n");
}

// BOUNDARY CONTROL 2 (green on base). The consumer holds a yielded lambda
// across two further MoveNext calls (past the iteration's body end) before
// invoking it. Same wrong fix as control 1 makes this a heap-use-after-free.
// per §27.5, §21.4, §29.3 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           CONTROL_escaping_lambda_held_across_two_advances_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module P; "
        "public class Program "
        "  public static method L(n: int): IEnumerable<() -> int> "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      yield return f; "
        "      yield return lambda(): int => 1; "
        "    end; "
        "  end L; "
        "  public static method Main() "
        "    var total := 0; "
        "    var e := L(100).GetEnumerator(); "
        "    while e.MoveNext() do "
        "      var g := e.Current; "
        "      if e.MoveNext() then "
        "        if e.MoveNext() then "
        "          total := total + g() + e.Current(); "
        "        else "
        "          total := total + g(); "
        "        end; "
        "      end; "
        "    end; "
        "    e.Dispose(); "
        "    Console.WriteLine(total); "
        "  end Main; "
        "end Program; "),
        "33066\n");
}

// The same leak in an ASYNC method (the gate was `!smStateStruct_`, so it
// covered every state machine): the loop body has no `await`. Base: 400 B / 100.
// per §27.5, §21.4, §30.1 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           async_body_capture_for_var_no_suspend_in_loop_x100_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static async method Sum(n: int): Task<int> "
        "    var total := 0; "
        "    await Task.Delay(0); "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      total := total + f(); "
        "    end; "
        "    await Task.Delay(0); "
        "    return total; "
        "  end Sum; "
        "  public static async method Main(): Task<int> "
        "    var t := await Sum(100); "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        "49500\n");
}

// Async: `await` first, then capture the for-var in the same iteration.
// Base: 400 B / 100.
// per §27.5, §21.4, §30.1 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           async_await_then_capture_for_var_x100_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module P; "
        "public class Program "
        "  public static async method Sum(n: int): Task<int> "
        "    var total := 0; "
        "    for var y in 0..<n do "
        "      await Task.Delay(0); "
        "      var f := lambda(): int => y * 10; "
        "      total := total + f(); "
        "    end; "
        "    return total; "
        "  end Sum; "
        "  public static async method Main(): Task<int> "
        "    var t := await Sum(100); "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; "),
        "49500\n");
}

// Async iterator: capture, `await`, then `yield return f()`. Base: 400 B / 100.
// per §27.5, §21.4, §30.1 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           async_iterator_capture_await_yield_x100_spec_27_5_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      await Task.Delay(0); "
        "      yield return f(); "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    await for var v in Gen(100) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "49500\n");
}

// An async LAMBDA's body (its own state machine). Base: 400 B / 100.
// per §27.5, §21.4, §30.1 (issue #6953)
BEZEL_TEST(iterator_for_var_capture_cell_free_6953,
           async_lambda_body_capture_for_var_x100_spec_27_5_30_6) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var g := async lambda(n: int): Task<int> => do "
        "        var t := 0; "
        "        for var y in 0..<n do "
        "          var f := lambda(): int => y * 10; "
        "          await Task.Delay(0); "
        "          t := t + f(); "
        "        end; "
        "        return t; "
        "      end; "
        "    var r := await g(100); "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "49500\n");
}
