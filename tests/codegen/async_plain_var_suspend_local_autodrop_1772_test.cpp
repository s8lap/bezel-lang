// Codegen / ownership — issue #1772 (spec §28.3.10 "Owned-local discharge
// requirement", §28.9 "Trivial-dispose classes", §30.5 "Result lifetime and task
// disposal"; all v0.357).
//
// DEFECT: inside an `async` method body, a plain-`var` owned CLASS local whose
// live range crosses an `await` (a suspend-local) and is never discharged was a
// HARD §28.3 error ("owned value in `f` would be dropped here without disposal"),
// where a synchronous body gets the affine auto-drop. §28.3.10 states the rule
// with no body-kind condition: an undischarged owned local "is disposed
// implicitly at that scope end", the must-use lint is "a warning (not an error)",
// and "The rule is uniform across body kinds". §30.5's consumer-side rules say an
// owned local "keeps its ordinary §28.3 discharge (auto-drop at scope end)". The
// owner's ruling on #1772 (2026-07-24) settles that this includes locals held
// across a suspend. For a trivial-dispose class (§28.9) not even the warning
// attaches: "The §28.3 must-use warning is not emitted at the implicit-drop site
// of a plain-`var` owned local ... whose type is trivial-dispose".
//
// FIX: the ownership pass's async endState arm now arms `autoDropAtScopeEnd` for
// every plain-`var` owned LocalDecl in a non-iterator async body (not only the
// non-suspend-locals and task-family locals it admitted before), and applies
// the §28.9 exemption there as the synchronous arm does. Codegen routes the flag
// through registerUsingDisposal — the path an admitted `using var` suspend-local
// already takes — so the drop runs on the normal exit, an early `return`, a
// throw after resumption, a faulted await's re-raise, and cancellation (the
// runtime faults the awaited task, which resumes the frame and unwinds).
//
// ORACLE: runAotChecked (ASan + LSan). A leak, double-free or use-after-free
// fails the row (ran=false). Each row also asserts the program's STDOUT, so the
// disposal order and the exactly-once count are value-checked, not only
// leak-checked. The leak rows LOOP (100 iterations), per the one-shot-leak trap.
//
// FAILS WITHOUT / PASSES WITH: on the base every row marked [regress] below is
// REJECTED at compile time (ran=false, empty stdout). The rows marked [edge] are
// throw-only shapes the base already compiled (no normal-exit drop site); they
// pin that the new admission did not disturb those edges.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT harness (ASan/LSan)
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A destructor-bearing (non-trivial-dispose, §28.9) class that prints when it
// is disposed, so the disposal point and count are read off stdout.
const char* kRes =
    "public class Res(name: string) "
    "  public property Name: string => name; "
    "  destructor "
    "    Console.WriteLine($\"disposed {name}\"); "
    "  end destructor; "
    "end Res; ";

// stdout of a clean run, or a sentinel naming why there is none.
std::string outOf(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<not ran>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// [regress] THE ISSUE'S OWN REPRO (#1772 comment, ergonomics study B12),
// verbatim. `Flaky` is trivial-dispose (§28.9: no destructor, no owned fields),
// held across `await Task.Delay(1)`. Base: rejected. Fixed: compiles, prints
// the field's value, and the instance is freed at scope end (LSan-clean).
// // per §28.3.10, §28.9, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, issue_repro_trivial_dispose_across_await) {
    BEZEL_EXPECT_EQ(outOf(
        "module P; "
        "public class Flaky "
        "  private _calls: int := 0; "
        "  public method Get(): int => _calls; "
        "end Flaky; "
        "public class Program "
        "  public static async method Main(): Task "
        "    var f := new Flaky(); "
        "    await Task.Delay(1); "
        "    Console.WriteLine(f.Get()); "
        "  end Main; "
        "end Program;"),
        std::string("0\n"));
}

// [regress] The issue repro LOOPED 100x, so a missing free is a 100-block leak
// LSan cannot miss, and the field is read after resumption every time.
// // per §28.3.10, §28.9, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, trivial_dispose_across_await_looped_freed) {
    BEZEL_EXPECT_EQ(outOf(
        "module P; "
        "public class Flaky(k: int) "
        "  public method Get(): int => k; "
        "end Flaky; "
        "public class Program "
        "  public static async method Main(): Task "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 100 do "
        "      var f := new Flaky(i); "
        "      await Task.Delay(1); "
        "      sum := sum + f.Get(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(sum); "
        "  end Main; "
        "end Program;"),
        std::string("4950\n"));
}

// [regress] A destructor-bearing class across an await: the auto-drop runs the
// destructor exactly once, at scope end, AFTER the post-await use.
// // per §28.3.10, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, nontrivial_across_await_disposed_at_scope_end) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; ") + kRes +
        "public class Program "
        "  public static async method Main(): Task "
        "    var r := new Res(\"r\"); "
        "    await Task.Delay(1); "
        "    Console.WriteLine(r.Name); "
        "  end Main; "
        "end Program;"),
        std::string("r\ndisposed r\n"));
}

// [regress] Per-iteration scope end in a loop whose body suspends: each
// iteration's local is disposed at that iteration's body end (3 lines of
// output each in order), and the 100-iteration count proves exactly-once.
// // per §28.3.10, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, loop_body_local_disposed_each_iteration) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; ") +
        "public class Cell "
        "  public static n: int := 0; "
        "  public static made: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; Cell.made := Cell.made + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "public class Program "
        "  public static async method Main(): Task "
        "    var i := 0; "
        "    var peak := 0; "
        "    while i < 100 do "
        "      var c := new Cell(); "
        "      await Task.Delay(1); "
        "      if Cell.n > peak then peak := Cell.n; end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"{Cell.made} {Cell.n} {peak}\"); "
        "  end Main; "
        "end Program;"),
        std::string("100 0 1\n"));
}

// [regress] Reverse declaration order across a plain `var`, a `using var` and a
// later plain `var`, with awaits between them: one establishment-ordered cleanup
// stack (§28.3.10 *Exception-path discharge*'s ordering paragraph).
// // per §28.3.10, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, reverse_declaration_order_with_using_var) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; ") + kRes +
        "public class Program "
        "  public static async method Main(): Task "
        "    var a := new Res(\"a\"); "
        "    using var b := new Res(\"b\"); "
        "    await Task.Delay(1); "
        "    var c := new Res(\"c\"); "
        "    await Task.Delay(1); "
        "    Console.WriteLine(a.Name + b.Name + c.Name); "
        "  end Main; "
        "end Program;"),
        std::string("abc\ndisposed c\ndisposed b\ndisposed a\n"));
}

// [regress] An early `return` after resumption disposes the local on that exit;
// the fall-through exit disposes it there. Exactly once on each path.
// // per §28.3.10, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, early_return_after_await_disposed_once) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; ") + kRes +
        "public class Program "
        "  public static async method Worker(k: int): Task<int> "
        "    var r := new Res($\"w{k}\"); "
        "    await Task.Delay(1); "
        "    if k > 0 then "
        "      Console.WriteLine(\"early\"); "
        "      return 1; "
        "    end if; "
        "    await Task.Delay(1); "
        "    Console.WriteLine(r.Name); "
        "    return 2; "
        "  end Worker; "
        "  public static async method Main(): Task "
        "    Console.WriteLine(await Program.Worker(1)); "
        "    Console.WriteLine(await Program.Worker(0)); "
        "  end Main; "
        "end Program;"),
        std::string("early\ndisposed w1\n1\nw0\ndisposed w0\n2\n"));
}

// [regress] FAULTED AWAIT edge: the awaited task faults after resumption; the
// re-raise unwinds the frame and disposes the local before the caller's catch
// arm runs. (The local has a normal exit too, so the base rejected it.)
// // per §28.3.10 *Exception-path discharge*, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, faulted_await_edge_disposed_once) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; ") + kRes +
        "public class Program "
        "  public static async method Fails(): Task<int> "
        "    await Task.Delay(1); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Fails; "
        "  public static async method Worker(): Task<int> "
        "    var r := new Res(\"w\"); "
        "    await Task.Delay(1); "
        "    var v := await Program.Fails(); "
        "    Console.WriteLine(r.Name); "
        "    return v; "
        "  end Worker; "
        "  public static async method Main(): Task "
        "    try "
        "      var x := await Program.Worker(); "
        "      Console.WriteLine(x); "
        "    catch "
        "      InvalidOperationException => Console.WriteLine(\"caught\"), "
        "    end try; "
        "    Console.WriteLine(\"end\"); "
        "  end Main; "
        "end Program;"),
        std::string("disposed w\ncaught\nend\n"));
}

// [regress] CANCELLATION edge: the frame is cancelled at `await Task.Delay(long,
// token)`; the runtime faults the delay task, the frame resumes and unwinds, and
// the local is disposed exactly once.
// // per §28.3.10, §30.5, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, cancellation_edge_disposed_once) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; import System.Threading; ") + kRes +
        "public class Program "
        "  public static async method Worker(token: CancellationToken): Task<int> "
        "    var r := new Res(\"w\"); "
        "    await Task.Delay(10000, token); "
        "    Console.WriteLine(r.Name); "
        "    return 0; "
        "  end Worker; "
        "  public static async method Main(): Task "
        "    using var cts := CancellationTokenSource.WithCancelAfter("
        "        TimeSpan.FromMilliseconds(20)); "
        "    try "
        "      var x := await Program.Worker(cts.Token); "
        "      Console.WriteLine(x); "
        "    catch "
        "      Exception => Console.WriteLine(\"caught\"), "
        "    end try; "
        "    Console.WriteLine(\"end\"); "
        "  end Main; "
        "end Program;"),
        std::string("disposed w\ncaught\nend\n"));
}

// [edge] THROW after resumption with no normal exit: the base already compiled
// this (there is no normal-exit drop site to reject). Pins that the unwind
// disposal still runs exactly once alongside the new admission.
// // per §28.3.10 *Exception-path discharge*, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, throw_after_await_edge_disposed_once) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; ") + kRes +
        "public class Program "
        "  public static async method Worker(): Task<int> "
        "    var r := new Res(\"w\"); "
        "    await Task.Delay(1); "
        "    Console.WriteLine(r.Name); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Worker; "
        "  public static async method Main(): Task "
        "    try "
        "      var x := await Program.Worker(); "
        "      Console.WriteLine(x); "
        "    catch "
        "      InvalidOperationException => Console.WriteLine(\"caught\"), "
        "    end try; "
        "    Console.WriteLine(\"end\"); "
        "  end Main; "
        "end Program;"),
        std::string("w\ndisposed w\ncaught\nend\n"));
}

// [regress] Pattern B: `var x := await Make()` binds the transferred owned
// result in a plain `var`; held across a later await and never discharged, it
// is auto-dropped at scope end (§30.5: "A bare `var x := await someTask;`
// compiles ... auto-drops at scope exit with the must-use warning").
// // per §30.5, §28.3.10, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, pattern_b_plain_var_auto_dropped) {
    BEZEL_EXPECT_EQ(outOf(
        std::string("module P; ") + kRes +
        "public class Program "
        "  public static async method Make(n: string): OwningTask<Res> "
        "    await Task.Delay(1); "
        "    return new Res(n); "
        "  end Make; "
        "  public static async method Main(): Task "
        "    var x := await Program.Make(\"b\"); "
        "    await Task.Delay(1); "
        "    Console.WriteLine(x.Name); "
        "  end Main; "
        "end Program;"),
        std::string("b\ndisposed b\n"));
}

// [regress] §28.12 "implicit discharge is synchronous": an IAsyncDisposable
// class held across an await in a plain `var` auto-drops through its
// synchronous destructor (+1), never DisposeAsync (+10).
// // per §28.3.10, §28.12, v0.357
BEZEL_TEST(async_plain_var_suspend_local_1772, async_disposable_auto_drop_is_synchronous) {
    BEZEL_EXPECT_EQ(outOf(
        "module P; "
        "public class AsyncRes: IAsyncDisposable "
        "  public static disposed: int := 0; "
        "  public async method DisposeAsync(): Task "
        "    AsyncRes.disposed := AsyncRes.disposed + 10; "
        "  end DisposeAsync; "
        "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
        "end AsyncRes; "
        "public class Program "
        "  public static async method Work(): Task "
        "    var r := new AsyncRes(); "
        "    await Task.Delay(1); "
        "  end Work; "
        "  public static async method Main(): Task "
        "    await Program.Work(); "
        "    Console.WriteLine(AsyncRes.disposed); "
        "  end Main; "
        "end Program;"),
        std::string("1\n"));
}
