// Codegen / §33.4, §33.2, §33.3 (v0.338) — #6231: a block lambda (e.g. a
// `Domain.Spawn` body) in `async Main(args: Array<string>)` failed to compile
// with "Referring to an instruction in another function!", and every async
// `Main(args)` leaked its `args` array.
//
// Spec §33.4 (v0.338) gives `Main` an optional `args: Array<string>`
// parameter; §33.2 allows `Main` to be `async`.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Async.cpp). The async-Main entry (the C
// `main`) builds `args` into an alloca and armed `pendingArgsDisposal_`, which
// the synchronous path drains into the body's ROOT scope in
// `lowerScopedBlock`. The async body lowers in the separate Resume function,
// whose root dispose frame is pushed by hand, so nothing drained it: `args`
// leaked, and the first nested body that opened its own root scope (a block
// lambda) drained it and emitted a free of `main`'s alloca from inside the
// lambda function.
//
// FIX: the async entry does not arm the pending disposal; `main` frees `args`
// itself after the Main task settles and the worker pool is joined.
//
// Every row is on runAotChecked (generated-code ASan/LSan) and asserts stdout
// (and the exit code). Fail-without: every row not marked CONTROL fails on the
// pre-fix compiler.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The program's stdout with the trailing line ending removed, followed by
// " exit=<code>" when the exit code is not 0. A program that was rejected,
// crashed, hung or tripped the sanitizer yields a marker that can never equal
// an expectation.
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did not run clean>";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (r.exitCode != 0) s += " exit=" + std::to_string(r.exitCode);
    return s;
}

}  // namespace

// The issue's program, verbatim. Before the fix: "Referring to an
// instruction in another function!" (compile failure).
BEZEL_TEST(async_main_args_lambda_6231,
           issue_repro_spawn_block_lambda_spec_33_4) {
    const char* src = R"BZL(module T;
public static class Program
  public static async method Main(args: Array<string>): Task<int>
    using var u := Domain.Spawn(lambda() => do Console.WriteLine("side"); end);
    await u;
    return 0;
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("side"));
}

// The same failure without `Domain.Spawn`: any block lambda in an async
// `Main(args)` opened the first nested root scope and drained the armed args
// disposal.
BEZEL_TEST(async_main_args_lambda_6231,
           plain_block_lambda_twin_spec_33_4) {
    const char* src = R"BZL(module T;
public static class Program
  public static async method Main(args: Array<string>): Task<int>
    var f := lambda() => do Console.WriteLine("side"); end;
    f();
    await Task.Delay(1);
    return 0;
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("side"));
}

// The fault edge: the body throws after the lambda and an `await`. §33.3:
// the uncaught exception exits the process with code 70. Before the fix: the
// same compile failure.
BEZEL_TEST(async_main_args_lambda_6231,
           block_lambda_then_fault_spec_33_3) {
    const char* src = R"BZL(module T;
public static class Program
  public static async method Main(args: Array<string>): Task<int>
    var f := lambda() => do Console.WriteLine("side"); end;
    f();
    await Task.Delay(1);
    if args.Count = 0 then
      throw new InvalidOperationException("boom");
    end if;
    return 0;
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("side exit=70"));
}

// No lambda at all: `args` was never freed (the armed disposal was never
// drained), which LSan reports (before the fix: 56 bytes in 1 allocation).
// After the fix `args` is freed once, after the Main task settles.
BEZEL_TEST(async_main_args_lambda_6231,
           args_array_freed_after_await_spec_33_4) {
    const char* src = R"BZL(module T;
public static class Program
  public static async method Main(args: Array<string>): Task<int>
    await Task.Delay(1);
    Console.WriteLine($"n={args.Count}");
    return 0;
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("n=0"));
}

// `args` iterated before an `await`, plus an expression-bodied lambda
// (before the fix: the args leak).
BEZEL_TEST(async_main_args_lambda_6231,
           args_loop_and_expression_lambda_spec_33_4) {
    const char* src = R"BZL(module T;
public static class Program
  public static async method Main(args: Array<string>): Task<int>
    var n := 0;
    for var a in args do n := n + a.Length; end for;
    var g := lambda(x: int): int => x + 1;
    await Task.Delay(1);
    Console.WriteLine($"n={g(n)}");
    return 0;
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("n=1"));
}

// `args` borrowed by an async callee that spawns; Main awaits it, so the
// borrow ends before `args` is freed (before the fix: the args leak).
BEZEL_TEST(async_main_args_lambda_6231,
           args_forwarded_to_async_callee_spec_33_4) {
    const char* src = R"BZL(module T;
public static class Program
  public static async method Run(args: Array<string>): Task<int>
    using var u := Domain.Spawn(lambda() => do Console.WriteLine("side"); end);
    await u;
    return args.Count;
  end;
  public static async method Main(args: Array<string>): Task<int>
    return await Program.Run(args);
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("side"));
}

// CONTROL: async `Main()` without `args` (nothing to dispose).
BEZEL_TEST(async_main_args_lambda_6231,
           control_async_main_without_args_spec_33_2) {
    const char* src = R"BZL(module T;
public static class Program
  public static async method Main(): Task<int>
    using var u := Domain.Spawn(lambda() => do Console.WriteLine("side"); end);
    await u;
    return 0;
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("side"));
}

// CONTROL: synchronous `Main(args)`; its root scope drains the args
// disposal.
BEZEL_TEST(async_main_args_lambda_6231,
           control_sync_main_with_args_spec_33_4) {
    const char* src = R"BZL(module T;
public static class Program
  public static method Main(args: Array<string>): int
    using var u := Domain.Spawn(lambda() => do Console.WriteLine("side"); end);
    u.Wait();
    return 0;
  end;
end;
)BZL";
    BEZEL_EXPECT_EQ(out(src), std::string("side"));
}
