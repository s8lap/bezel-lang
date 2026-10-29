// Codegen — issue #7629 (§31.2, v0.350): a `DisposeAsync` that FAULTS under
// `await using`, inside a catch arm or inside a `finally` entered on an
// exception edge, must dispose the CAUGHT exception it supersedes.
//
//     try
//       throw new ArgumentException("outer");
//     catch var e: ArgumentException =>
//       do
//         await using var d := new N();   // DisposeAsync throws "boom"
//       end;
//     end;
//
// §31.2, v0.350, verbatim: "when a `finally` entered during exception
// propagation itself completes by throwing (directly, or via a call that
// propagates), the new exception replaces the one being handled; the
// superseded exception is **disposed by the runtime exactly once**, as if its
// handling had completed". A throw out of a catch arm supersedes the caught
// exception the same way (the catch-arm's own caught-exception disposal is
// skipped on the unwind). The synchronous routes — a direct `throw`, a throwing
// call, a `using var` destructor — all dispose it (#1216); the `await using`
// disposal's two fault edges (the `await.fault` block for an `async`
// DisposeAsync, the synchronous-throw edge of #3483 for a plain one) did not:
// 4000 B per 100 iterations (MEASURED, review probes
// /var/tmp/bezel-scratch/r37-rev-l4c/p/a1_catch_*.bzl, a2_finally_exc_*.bzl).
//
// Four heads per context: the static `new N()` head (a SYNCHRONOUS throw), the
// run-time `MkI()` head (an `owned IAsyncDisposable` call, the #3483 itab arm,
// synchronous throw), the static `new NA()` head (an `async` DisposeAsync: a
// FAULTED TASK) and the run-time `MkAI()` head (the same, through the itab).
// Each loops 100x under generated-code ASan (runAotChecked): the new exception
// propagates (`ok=100`), the caught one never survives (`outer=0`), the owned
// Cell is cascaded (`cells=0`), and LeakSanitizer is silent.
//
// The shared tail is also where §28.3 *The return slot* (v0.338) is honoured on
// these edges: a `return c;` of an owned local whose cleanup edge then faults
// "is abandoned ... and the slot's value is disposed by the unwind" (the #6052
// slot unwind emitPendingCheckIf runs). The `await using` fault edges skipped it
// (MEASURED on the r30-c1 base: 1600 B / 100 allocations, `cells=100`; the
// `return_slot_*` rows pin it).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"BZ(
module M;
public class Cell
  public static n: int := 0;
  public constructor() Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
public class K
  public static ok: int := 0;
  public static bad: int := 0;
  public static outer: int := 0;
  public constructor() end constructor;
end K;
public class N: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public method DisposeAsync(): Task
    throw new InvalidOperationException("boom");
  end DisposeAsync;
end N;
public class NA: IAsyncDisposable
  public owned _c: Cell;
  public constructor() _c := new Cell(); end constructor;
  public async method DisposeAsync(): Task
    throw new InvalidOperationException("aboom");
  end DisposeAsync;
end NA;
public class Program
  public static method MkI(): owned IAsyncDisposable return new N(); end MkI;
  public static method MkAI(): owned IAsyncDisposable return new NA(); end MkAI;
  public static method Tally(m: string, want: string)
    if m = want then K.ok := K.ok + 1; else K.bad := K.bad + 1; end;
  end Tally;
)BZ";

// `ctx` is the body of R(): it holds `HEAD` where the `await using` head goes.
std::string build(const char* tag, const char* head, const char* want,
                  const char* ctx) {
    std::string r = ctx;
    for (std::size_t p; (p = r.find("HEAD")) != std::string::npos;)
        r.replace(p, 4, head);
    return std::string(kPrelude) + "  public static async method R(): Task\n" +
           r + "  end R;\n" + R"BZ(
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R();
      catch var e: InvalidOperationException =>
        Program.Tally(e.Message, ")BZ" + want + R"BZ(");
      catch var e: ArgumentException =>
        K.outer := K.outer + 1;
      end;
    end for;
    Console.WriteLine($")BZ" + tag +
           R"BZ(: ok={K.ok} bad={K.bad} outer={K.outer} cells={Cell.n}");
  end Main;
end Program;
)BZ";
}

// The faulting disposal inside a catch arm.
constexpr const char* kCatchCtx = R"BZ(
    try
      throw new ArgumentException("outer");
    catch var e: ArgumentException =>
      do
        await using var d := HEAD;
      end;
    end;
)BZ";

// The faulting disposal inside a `finally` entered on an exception edge.
constexpr const char* kFinallyCtx = R"BZ(
    try
      throw new ArgumentException("outer");
    finally
      do
        await using var d := HEAD;
      end;
    end;
)BZ";

// The return-slot shape: `return c;` moves the owned local into the async
// return slot, then the `await using` binding's disposal faults.
std::string buildReturnSlot(const char* tag, const char* head,
                            const char* want) {
    std::string src = kPrelude;
    src += R"BZ(
  public static async method RS(): OwningTask<Cell>
    var c := new Cell();
    await using var d := )BZ";
    src += head;
    src += R"BZ(;
    return c;
  end RS;
  public static async method R(): Task
    using var x := await Program.RS();
    K.outer := K.outer + 1;
  end R;
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R();
      catch var e: InvalidOperationException =>
        Program.Tally(e.Message, ")BZ";
    src += want;
    src += R"BZ(");
      end;
    end for;
    Console.WriteLine($")BZ";
    src += tag;
    src += R"BZ(: ok={K.ok} bad={K.bad} outer={K.outer} cells={Cell.n}");
  end Main;
end Program;
)BZ";
    return src;
}

void expectClean(const std::string& src, const char* tag) {
    bezel::test::JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string(tag) + ": ok=100 bad=0 outer=0 cells=0\n");
}

}  // namespace

// fix #7629 (§31.2, v0.350): catch arm, static head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_supersede,
           catch_arm_static_head_sync_throw_disposes_superseded_caught_exc_spec_31_2_7629) {
    expectClean(build("s", "new N()", "boom", kCatchCtx), "s");
}

// fix #7629 (§31.2, v0.350): catch arm, run-time head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_supersede,
           catch_arm_runtime_head_sync_throw_disposes_superseded_caught_exc_spec_31_2_7629) {
    expectClean(build("r", "Program.MkI()", "boom", kCatchCtx), "r");
}

// fix #7629 (§31.2, v0.350): catch arm, static head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_supersede,
           catch_arm_static_head_faulted_task_disposes_superseded_caught_exc_spec_31_2_7629) {
    expectClean(build("fa", "new NA()", "aboom", kCatchCtx), "fa");
}

// fix #7629 (§31.2, v0.350): catch arm, run-time head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_supersede,
           catch_arm_runtime_head_faulted_task_disposes_superseded_caught_exc_spec_31_2_7629) {
    expectClean(build("far", "Program.MkAI()", "aboom", kCatchCtx), "far");
}

// fix #7629 (§31.2, v0.350): exception-entered finally, static head, sync throw.
BEZEL_TEST(await_using_fault_tail_supersede,
           finally_exc_edge_static_head_sync_throw_disposes_superseded_exc_spec_31_2_7629) {
    expectClean(build("s", "new N()", "boom", kFinallyCtx), "s");
}

// fix #7629 (§31.2, v0.350): exception-entered finally, run-time head, sync throw.
BEZEL_TEST(await_using_fault_tail_supersede,
           finally_exc_edge_runtime_head_sync_throw_disposes_superseded_exc_spec_31_2_7629) {
    expectClean(build("r", "Program.MkI()", "boom", kFinallyCtx), "r");
}

// fix #7629 (§31.2, v0.350): exception-entered finally, static head, faulted task.
BEZEL_TEST(await_using_fault_tail_supersede,
           finally_exc_edge_static_head_faulted_task_disposes_superseded_exc_spec_31_2_7629) {
    expectClean(build("fa", "new NA()", "aboom", kFinallyCtx), "fa");
}

// fix #7629 (§31.2, v0.350): exception-entered finally, run-time head, faulted task.
BEZEL_TEST(await_using_fault_tail_supersede,
           finally_exc_edge_runtime_head_faulted_task_disposes_superseded_exc_spec_31_2_7629) {
    expectClean(build("far", "Program.MkAI()", "aboom", kFinallyCtx), "far");
}

// fix #7629 (§28.3 *The return slot*, v0.338 / §31.2, v0.350): the faulting
// disposal abandons a `return c;`; the slot's value is disposed at the raise.
// Static head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_supersede,
           return_slot_static_head_sync_throw_disposes_the_returned_local_spec_28_3_7629) {
    expectClean(buildReturnSlot("s", "new N()", "boom"), "s");
}

// Static head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_supersede,
           return_slot_static_head_faulted_task_disposes_the_returned_local_spec_28_3_7629) {
    expectClean(buildReturnSlot("fa", "new NA()", "aboom"), "fa");
}

// Run-time head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_supersede,
           return_slot_runtime_head_sync_throw_disposes_the_returned_local_spec_28_3_7629) {
    expectClean(buildReturnSlot("r", "Program.MkI()", "boom"), "r");
}

// Run-time head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_supersede,
           return_slot_runtime_head_faulted_task_disposes_the_returned_local_spec_28_3_7629) {
    expectClean(buildReturnSlot("far", "Program.MkAI()", "aboom"), "far");
}
