// Codegen — issue #7527 (§28.12, v0.350): an `await using var` whose scope has
// an UNTAKEN early exit (`return`, `break`, `continue`) before the normal exit
// must still complete after its `DisposeAsync` genuinely suspends.
//
//     await using var d := new AsyncRes();
//     if k = 0 then return 1; end;          // untaken
//     await using var e := new AsyncRes();
//     return 3;
//
// §28.12, v0.350, verbatim: `await using`'s "implicit `finally` awaits
// `DisposeAsync` on both the normal and exception edges of an async body".
// Each exit edge of the scope therefore carries its own `await DisposeAsync()`,
// i.e. its own suspend point and resume state (§30.2). The resume switch is
// sized from a static count of ONE state per `await using` head, so every
// further exit edge allocated a state with no case in the switch: the
// continuation registered for it landed on the switch default and the task
// never completed. The program printed "DA DA" and hung (exit 124, MEASURED on
// the r37-u-awaitfault base for the issue's own repro, and for the single-head
// `if k = 0 then return; end;` spelling). A taken early exit and a
// `DisposeAsync` that finishes inline (no suspend) both worked, which is why the
// existing rows never saw it: `DisposeAsync` here awaits `Task.Delay(0)`, which
// really suspends.
//
// Every row compares stdout, so a hang (the harness's run deadline) or a
// swallowed disposal fails the row. runAotChecked also runs the generated code
// under ASan/LSan.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"BZ(
module M;
public class AR: IAsyncDisposable
  public static n: int := 0;
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    AR.n := AR.n + 1;
  end DisposeAsync;
  destructor AR.n := AR.n + 1000; end destructor;
end AR;
public class Program
)BZ";

std::string program(const char* members, const char* mainBody) {
    return std::string(kPrelude) + members +
           "  public static async method Main(): Task\n" + mainBody +
           "  end Main;\nend Program;\n";
}

void expectOut(const std::string& src, const char* want) {
    bezel::test::JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// The issue's repro: two heads, an untaken early return between them.
BEZEL_TEST(await_using_resume_state_per_exit_edge,
           two_heads_untaken_early_return_between_completes_spec_28_12_7527) {
    expectOut(program(R"BZ(
  public static async method Run(k: int): Task<int>
    await using var d := new AR();
    if k = 0 then return 1; end;
    await using var e := new AR();
    return 3;
  end Run;
)BZ", R"BZ(
    var s := await Program.Run(1);
    Console.WriteLine($"s={s} n={AR.n}");
)BZ"), "s=3 n=2\n");
}

// One head, `Task` result, a bare untaken `return;`.
BEZEL_TEST(await_using_resume_state_per_exit_edge,
           single_head_untaken_bare_return_completes_spec_28_12_7527) {
    expectOut(program(R"BZ(
  public static async method Run(k: int): Task
    await using var d := new AR();
    if k = 0 then return; end;
    Console.WriteLine("body");
  end Run;
)BZ", R"BZ(
    await Program.Run(1);
    Console.WriteLine($"n={AR.n}");
)BZ"), "body\nn=1\n");
}

// Three exit edges (two untaken returns and the final one).
BEZEL_TEST(await_using_resume_state_per_exit_edge,
           three_exit_edges_two_untaken_returns_complete_spec_28_12_7527) {
    expectOut(program(R"BZ(
  public static async method Run(k: int): Task<int>
    await using var d := new AR();
    if k = 7 then return 1; end;
    if k = 8 then return 2; end;
    return 4;
  end Run;
)BZ", R"BZ(
    var a := await Program.Run(0);
    var b := await Program.Run(7);
    var c := await Program.Run(8);
    Console.WriteLine($"a={a} b={b} c={c} n={AR.n}");
)BZ"), "a=4 b=1 c=2 n=3\n");
}

// A loop body: untaken `break`, `continue` and `return` before the iteration end.
BEZEL_TEST(await_using_resume_state_per_exit_edge,
           loop_body_untaken_break_continue_return_complete_spec_28_12_7527) {
    expectOut(program(R"BZ(
  public static async method Run(k: int): Task<int>
    var t := 0;
    for var i in 0..<4 do
      await using var d := new AR();
      if i = 10 then break; end;
      if i = 11 then continue; end;
      if k = 99 then return -1; end;
      t := t + 1;
    end for;
    return t;
  end Run;
)BZ", R"BZ(
    var a := await Program.Run(0);
    Console.WriteLine($"a={a} n={AR.n}");
)BZ"), "a=4 n=4\n");
}

// Inside a try with a finally: the return edge and the fall-off edge.
BEZEL_TEST(await_using_resume_state_per_exit_edge,
           try_finally_untaken_return_completes_spec_28_12_7527) {
    expectOut(program(R"BZ(
  public static async method Run(k: int): Task<int>
    try
      await using var d := new AR();
      if k = 7 then return 1; end;
      return 2;
    finally
      AR.n := AR.n + 100;
    end;
  end Run;
)BZ", R"BZ(
    var a := await Program.Run(0);
    Console.WriteLine($"a={a} n={AR.n}");
)BZ"), "a=2 n=101\n");
}

// Repeated: the state allocation is per emission, not per run.
BEZEL_TEST(await_using_resume_state_per_exit_edge,
           untaken_early_return_in_a_loop_of_calls_spec_28_12_7527) {
    expectOut(program(R"BZ(
  public static async method Run(k: int): Task<int>
    await using var d := new AR();
    if k = 0 then return 1; end;
    return 3;
  end Run;
)BZ", R"BZ(
    var t := 0;
    for var i in 0..<100 do
      t := t + await Program.Run(1);
    end for;
    Console.WriteLine($"t={t} n={AR.n}");
)BZ"), "t=300 n=100\n");
}
