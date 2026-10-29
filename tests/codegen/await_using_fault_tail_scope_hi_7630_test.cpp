// Codegen — issue #7630 (§28.4 / §28.2, v0.350): a `DisposeAsync` that FAULTS
// under `await using` must not release a local declared AFTER the binding a
// second time.
//
//     public static async method R(i: int): Task
//       await using var d := new NA();   // DisposeAsync faults
//       var s1 := $"post{i}";            // declared AFTER the binding
//     end R;
//
// A scope's entries are disposed in reverse declaration order, so by the time
// the `await using` entry's `DisposeAsync` runs, `s1` (declared after it) has
// already been released by the pass. The fault edge then re-ran the WHOLE
// scope's unwind (`emitUnwindDisposals`), releasing `s1` again: abort with
// "refcount over-release" (MEASURED, review probes
// /var/tmp/bezel-scratch/r37-rev-l4c/p/b4_locals_after_*.bzl,
// b5_string_after_only_*.bzl; b6_string_before_only_* is the control — a string
// declared BEFORE the binding is still pending and is released exactly once).
//
// §28.2, v0.350: a value is disposed exactly once, on every path, including the
// exception path ("An owned local is disposed exactly once on every
// control-flow path", §28.3.10; §28.4's cleanup pass runs every remaining
// disposal). So on the fault edge the unwind must dispose exactly what the
// pass has NOT reached yet — the entries declared before the faulting one and
// the scopes outside — and nothing the pass already discharged.
//
// Heads: the static `new N()` head (a SYNCHRONOUS throw: the #3483 edge), the
// run-time `MkI()` head (itab arm, synchronous throw), the static `new NA()`
// head (an `async` DisposeAsync: a FAULTED TASK, the `await.fault` edge) and
// the run-time `MkAI()` head. Each loops 100x under generated-code ASan.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

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

// `ctx` is the body of R(i: int): it holds `HEAD` where the head goes.
std::string build(const char* tag, const char* head, const char* want,
                  const char* ctx) {
    std::string r = ctx;
    for (std::size_t p; (p = r.find("HEAD")) != std::string::npos;)
        r.replace(p, 4, head);
    return std::string(kPrelude) +
           "  public static async method R(i: int): Task\n" + r +
           "  end R;\n" + R"BZ(
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
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

// Locals on BOTH sides of the binding, in the same scope: strings and `using`
// cells declared before are pending; those declared after were discharged.
constexpr const char* kAroundCtx = R"BZ(
    var s0 := $"pre{i}";
    using var c0 := new Cell();
    await using var d := HEAD;
    var s1 := $"post{i}";
    using var c1 := new Cell();
    using var u := new Cell();
    K.outer := K.outer + s0.Length + s1.Length - s0.Length - s1.Length;
)BZ";

// A string declared only AFTER the binding.
constexpr const char* kAfterCtx = R"BZ(
    await using var d := HEAD;
    var s1 := $"post{i}";
    K.outer := K.outer + s1.Length - s1.Length;
)BZ";

// The control: a string declared only BEFORE the binding.
constexpr const char* kBeforeCtx = R"BZ(
    var s0 := $"pre{i}";
    await using var d := HEAD;
    K.outer := K.outer + s0.Length - s0.Length;
)BZ";

// Both fixes at once: the faulting disposal in a catch arm, a string after it.
constexpr const char* kCatchAfterCtx = R"BZ(
    try
      throw new ArgumentException("outer");
    catch var e: ArgumentException =>
      do
        var s0 := $"pre{i}";
        await using var d := HEAD;
        var s1 := $"post{i}";
        K.outer := K.outer + s0.Length + s1.Length - s0.Length - s1.Length;
      end;
    end;
)BZ";

void expectClean(const std::string& src, const char* tag) {
    bezel::test::JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string(tag) + ": ok=100 bad=0 outer=0 cells=0\n");
}

// A `return` / `break` out of a `try` whose enclosing scope (OUTSIDE the try)
// holds the faulting `await using` AND a string declared before it. The
// binding's scope lies below the try's unwind depth, so the fault tail must
// not dispose the scope's pending entries: the string is released by the exit
// edge and used again after the (mis-routed, issue #7708) inner catch. With the
// pending disposal the program read a freed string (heap-use-after-free) or
// released it twice ("refcount over-release"). The inner catch is taken
// instead of the outer one (issue #7708, pre-existing wrong routing), so these
// rows assert ONLY that the program runs to completion memory-clean and do not
// pin the routed count.
std::string buildExitInTry(const char* head, const char* exit, bool useAfter,
                           bool loop, bool stringInTry = false) {
    std::string body = loop ? "    for var j in 0..<2 do\n" : "";
    if (!stringInTry) body += "    var s := $\"s{i}\";\n";
    body += "    await using var d := ";
    body += head;
    body += ";\n    try\n      K.outer := K.outer + 0;\n      ";
    if (stringInTry)
        body += "var s3 := $\"u{i}\";\n      "
                "K.outer := K.outer + s3.Length - s3.Length;\n      ";
    body += exit;
    body += ";\n    catch var e: InvalidOperationException =>\n"
            "      K.bad := K.bad + 1;\n    end;\n";
    if (useAfter) body += "    K.outer := K.outer + s.Length - s.Length;\n";
    if (loop) body += "    end for;\n";
    std::string r = kPrelude;
    r += "  public static async method R(i: int): Task\n" + body +
         "  end R;\n" + R"BZ(
  public static async method Main(): Task
    for var i in 0..<100 do
      try
        await Program.R(i);
      catch var e: InvalidOperationException =>
        K.ok := K.ok + 1;
      end;
    end for;
    Console.WriteLine("done");
  end Main;
end Program;
)BZ";
    return r;
}

void expectRunsMemoryClean(const std::string& src) {
    bezel::test::JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

}  // namespace

// fix #7630 (§28.2 / §28.4, v0.350): locals on both sides of the binding, static head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           locals_around_binding_static_head_sync_throw_releases_each_once_spec_28_2_7630) {
    expectClean(build("s", "new N()", "boom", kAroundCtx), "s");
}

// fix #7630 (§28.2 / §28.4, v0.350): locals on both sides of the binding, run-time head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           locals_around_binding_runtime_head_sync_throw_releases_each_once_spec_28_2_7630) {
    expectClean(build("r", "Program.MkI()", "boom", kAroundCtx), "r");
}

// fix #7630 (§28.2 / §28.4, v0.350): locals on both sides of the binding, static head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           locals_around_binding_static_head_faulted_task_releases_each_once_spec_28_2_7630) {
    expectClean(build("fa", "new NA()", "aboom", kAroundCtx), "fa");
}

// fix #7630 (§28.2 / §28.4, v0.350): locals on both sides of the binding, run-time head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           locals_around_binding_runtime_head_faulted_task_releases_each_once_spec_28_2_7630) {
    expectClean(build("far", "Program.MkAI()", "aboom", kAroundCtx), "far");
}

// fix #7630 (§28.2 / §28.4, v0.350): a string declared after the binding, static head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           string_after_binding_static_head_sync_throw_releases_once_spec_28_2_7630) {
    expectClean(build("s", "new N()", "boom", kAfterCtx), "s");
}

// fix #7630 (§28.2 / §28.4, v0.350): a string declared after the binding, run-time head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           string_after_binding_runtime_head_sync_throw_releases_once_spec_28_2_7630) {
    expectClean(build("r", "Program.MkI()", "boom", kAfterCtx), "r");
}

// fix #7630 (§28.2 / §28.4, v0.350): a string declared after the binding, static head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           string_after_binding_static_head_faulted_task_releases_once_spec_28_2_7630) {
    expectClean(build("fa", "new NA()", "aboom", kAfterCtx), "fa");
}

// fix #7630 (§28.2 / §28.4, v0.350): a string declared after the binding, run-time head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           string_after_binding_runtime_head_faulted_task_releases_once_spec_28_2_7630) {
    expectClean(build("far", "Program.MkAI()", "aboom", kAfterCtx), "far");
}

// fix #7630 (§28.2 / §28.4, v0.350): CONTROL, a string declared before the binding, static head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           control_string_before_binding_static_head_sync_throw_releases_once_spec_28_2_7630) {
    expectClean(build("s", "new N()", "boom", kBeforeCtx), "s");
}

// fix #7630 (§28.2 / §28.4, v0.350): CONTROL, a string declared before the binding, static head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           control_string_before_binding_static_head_faulted_task_releases_once_spec_28_2_7630) {
    expectClean(build("fa", "new NA()", "aboom", kBeforeCtx), "fa");
}

// fix #7630 (§28.2 / §28.4, v0.350): catch arm AND a string after the binding, static head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           catch_arm_string_after_binding_static_head_sync_throw_clean_spec_28_2_7630) {
    expectClean(build("s", "new N()", "boom", kCatchAfterCtx), "s");
}

// fix #7630 (§28.2 / §28.4, v0.350): catch arm AND a string after the binding, run-time head, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           catch_arm_string_after_binding_runtime_head_sync_throw_clean_spec_28_2_7630) {
    expectClean(build("r", "Program.MkI()", "boom", kCatchAfterCtx), "r");
}

// fix #7630 (§28.2 / §28.4, v0.350): catch arm AND a string after the binding, static head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           catch_arm_string_after_binding_static_head_faulted_task_clean_spec_28_2_7630) {
    expectClean(build("fa", "new NA()", "aboom", kCatchAfterCtx), "fa");
}

// fix #7630 (§28.2 / §28.4, v0.350): catch arm AND a string after the binding, run-time head, faulted DisposeAsync task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           catch_arm_string_after_binding_runtime_head_faulted_task_clean_spec_28_2_7630) {
    expectClean(build("far", "Program.MkAI()", "aboom", kCatchAfterCtx), "far");
}

// fix #7630 (§28.2 / §28.4 v0.350, §31.2): `return` in a try, faulted-task binding outside the try, string used after.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           return_in_try_outer_binding_faulted_task_string_used_after_runs_clean_spec_28_2_7630) {
    expectRunsMemoryClean(buildExitInTry("new NA()", "return", true, false));
}

// Same, string NOT used afterwards (the refcount over-release arm, p08e).
BEZEL_TEST(await_using_fault_tail_scope_hi,
           return_in_try_outer_binding_faulted_task_string_unused_after_runs_clean_spec_28_2_7630) {
    expectRunsMemoryClean(buildExitInTry("new NA()", "return", false, false));
}

// Same, synchronous throw (p08f).
BEZEL_TEST(await_using_fault_tail_scope_hi,
           return_in_try_outer_binding_sync_throw_string_used_after_runs_clean_spec_28_2_7630) {
    expectRunsMemoryClean(buildExitInTry("new N()", "return", true, false));
}

// `break` in a try inside a loop (p14).
BEZEL_TEST(await_using_fault_tail_scope_hi,
           break_in_try_outer_binding_faulted_task_string_used_after_runs_clean_spec_28_2_7630) {
    expectRunsMemoryClean(buildExitInTry("new NA()", "break", true, true));
}

// fix #7630 (§28.2 / §28.4 v0.350): a string declared INSIDE the try body is
// already discharged by the `return` exit when the outer binding's disposal
// faults, so the fault tail must not release it again. `return`, faulted task.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           return_in_try_string_in_try_body_outer_binding_faulted_task_runs_clean_spec_28_2_7630) {
    expectRunsMemoryClean(
        buildExitInTry("new NA()", "return", false, false, true));
}

// Same, synchronous throw.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           return_in_try_string_in_try_body_outer_binding_sync_throw_runs_clean_spec_28_2_7630) {
    expectRunsMemoryClean(
        buildExitInTry("new N()", "return", false, false, true));
}

// Same, `break` out of the try inside a loop.
BEZEL_TEST(await_using_fault_tail_scope_hi,
           break_in_try_string_in_try_body_outer_binding_faulted_task_runs_clean_spec_28_2_7630) {
    expectRunsMemoryClean(
        buildExitInTry("new NA()", "break", false, true, true));
}
