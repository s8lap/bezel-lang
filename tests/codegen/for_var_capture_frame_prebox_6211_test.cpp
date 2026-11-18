// Codegen — issue #6211 (§27.5 Closures, §21.4 The `for` statement, §30.1
// Async methods, §29.3 Iterator methods, spec v0.359): a lambda capturing a
// `for` loop variable, then a suspension (`await` / `yield`) in the same
// iteration, lost the loop's own counter: the loop hung, stopped early, or
// the closure read a value that was not the iteration's.
//
// Mechanism: collectSuspendLocals widens a captured for-var's frame field to
// pointer width, because the field is MOVED onto the variable's cell holder
// when the for-var is boxed. For a captured LocalDecl `suspendLocalSlotTy`
// also records the pre-boxing type (#3484 follow-up), so the frame reserves a
// second field that carries the pre-boxing slot across the suspension. The
// ForStmt arm did not, so after the move the range loop's induction slot
// (read by incBB) had no frame field at all and a resume read whatever the
// new activation's stack held. The fix records it in the ForStmt arm too.
//
// The rows use runAotCheckedLenient (generated-code ASan, LeakSanitizer off)
// so that they pin the control flow and the values independently of the
// separate per-iteration cell leak (#6953, the same commit, its own suite).
// per §27.5, §21.4, §30.1, §29.3 (issue #6211)

#include "support/aot_support.hpp"   // runAotCheckedLenient
#include "test_harness.hpp"

using bezel::test::runAotCheckedLenient;

namespace {
std::string lenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// The issue's exact async repro: capture `i`, `await`, then call the closure.
// Base: the loop never advances (prints 5, then a wrong value, or hangs).
// per §27.5, §21.4, §30.1, §29.3 (issue #6211)
BEZEL_TEST(for_var_capture_frame_prebox_6211,
           issue_repro_async_await_after_capture_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module M; "
        "class C "
        "  public static async method Make(): Task "
        "    for var i in 5..6 do "
        "      var f: () -> int := lambda() => i; "
        "      await Task.Delay(1); "
        "      Console.WriteLine(f()); "
        "    end; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    await Make(); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "5\n6\n");
}

// The issue's sync-iterator twin: `yield return 1` in place of the `await`.
// Base: prints 1 5 and the second iteration never runs.
// per §27.5, §21.4, §30.1, §29.3 (issue #6211)
BEZEL_TEST(for_var_capture_frame_prebox_6211,
           issue_repro_iterator_twin_yield_after_capture_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module M; "
        "class C "
        "  public static method Make(): IEnumerable<int> "
        "    for var i in 5..6 do "
        "      var f: () -> int := lambda() => i; "
        "      yield return 1; "
        "      Console.WriteLine(f()); "
        "    end; "
        "  end Make; "
        "  public static method Main(): int "
        "    for var x in Make() do Console.WriteLine(x); end; "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "1\n5\n1\n6\n");
}

// 100 iterations, an `await` between forming the lambda and calling it; the
// sum is §21.4's (each closure sees its own iteration). Base: prints 0.
// per §27.5, §21.4, §30.1, §29.3 (issue #6211)
BEZEL_TEST(for_var_capture_frame_prebox_6211,
           async_sum_x100_await_between_capture_and_call_spec_21_4_30_1) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module P; "
        "public class Program "
        "  public static async method Sum(n: int): Task<int> "
        "    var total := 0; "
        "    for var y in 0..<n do "
        "      var f := lambda(): int => y * 10; "
        "      await Task.Delay(0); "
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

// Two yields per iteration, the closure called between them. Base: the
// loop stops after the first iteration.
// per §27.5, §21.4, §30.1, §29.3 (issue #6211)
BEZEL_TEST(for_var_capture_frame_prebox_6211,
           iterator_two_yields_per_iteration_body_reads_closure_spec_21_4_29_3) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module M; "
        "class C "
        "  public static method Make(): IEnumerable<int> "
        "    for var i in 5..<7 do "
        "      var f := lambda(): int => i; "
        "      yield return 1; "
        "      Console.WriteLine(f()); "
        "      yield return 2; "
        "    end; "
        "  end Make; "
        "  public static method Main(): int "
        "    for var x in Make() do Console.WriteLine(x); end; "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "1\n5\n2\n1\n6\n2\n");
}
