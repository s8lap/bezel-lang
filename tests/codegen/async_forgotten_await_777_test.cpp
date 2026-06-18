// fix #777 / §30.6: a forgotten `await` in an `async` lambda OR `async` method
// body is rejected with a clean diagnostic instead of crashing.
//
// `async () => GetSeven()` (declared `() -> Task<int>`) and the symmetric
// `async method Relay(): Task<int> return GetSeven();` produce a `Task<int>`
// value where the un-wrapped element `int` is expected. Both were silently
// accepted (checkAssignable's §5.1 path falls through for the declaration-less
// `Task` type) and then crashed: codegen stored the 8-byte task pointer into the
// element-sized async result slot (emitAsyncComplete) → SIGSEGV. The fix rejects
// the forgotten `await` in sema, on BOTH paths (the method path via the
// ReturnStmt check using the peeled element type; the lambda path via the
// pendingLambdaReturnType_ hint threaded from the function-typed local decl).
// The legitimate `await` and literal forms, and a genuine `Task<Task<int>>`
// result (element is itself Task-shaped), are unaffected.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
// True when the program is REJECTED at compile time (a diagnostic fired).
bool rejects(const std::string& program) {
    return runJit(program).diagnostics.hasErrors();
}
}  // namespace

// §30.6 REJECT: an expression-bodied async lambda whose body produces a Task
// value (the forgotten-await crash repro). Was SIGSEGV; now a compile error.
BEZEL_TEST(async_forgotten_await_777, lambda_forgotten_await_rejected) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; "
        "class C "
        "  public static method GetSeven(): Task<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<int> := async () => GetSeven(); "  // forgot await
        "    return await f(); "
        "  end Main; "
        "end C;"), true);
}

// §30.6 REJECT: the symmetric async METHOD forgotten-await (same root cause,
// same shared emitAsyncComplete crash site). Was SIGSEGV; now a compile error.
BEZEL_TEST(async_forgotten_await_777, method_forgotten_await_rejected) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; "
        "class C "
        "  public static method GetSeven(): Task<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Relay(): Task<int> "
        "    return GetSeven(); "                                  // forgot await
        "  end Relay; "
        "  public static async method Main(): Task<int> "
        "    return await Relay(); "
        "  end Main; "
        "end C;"), true);
}

// §30.6 ACCEPT: the corrected lambda form `async () => await GetSeven()` — body
// is the awaited `int`. Must compile and run (regression guard).
BEZEL_TEST(async_forgotten_await_777, lambda_with_await_accepted) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static method GetSeven(): Task<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<int> := async () => await GetSeven(); "
        "    return await f(); "
        "  end Main; "
        "end C;"), 7);
}

// §30.6 ACCEPT: a literal-bodied async lambda `async () => 7` — body is the
// element `int`. Must not be flagged.
BEZEL_TEST(async_forgotten_await_777, lambda_literal_body_accepted) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<int> := async () => 7; "
        "    return await f(); "
        "  end Main; "
        "end C;"), 7);
}

// §30.6 ACCEPT: the corrected method form `return await GetSeven()`.
BEZEL_TEST(async_forgotten_await_777, method_with_await_accepted) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static method GetSeven(): Task<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Relay(): Task<int> "
        "    return await GetSeven(); "
        "  end Relay; "
        "  public static async method Main(): Task<int> "
        "    return await Relay(); "
        "  end Main; "
        "end C;"), 7);
}

// §30.6 ACCEPT (the exemption): a genuine `Task<Task<int>>` result — the element
// type IS itself Task-shaped, so `return GetSeven()` (a Task<int> value) is
// correct, not a forgotten await. Must compile and run.
BEZEL_TEST(async_forgotten_await_777, nested_task_result_not_rejected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class C "
        "  public static method GetSeven(): Task<int> "
        "    return Task.FromResult<int>(7); end GetSeven; "
        "  public static async method Outer(): Task<Task<int>> "
        "    return GetSeven(); "                          // element is Task<int>
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var inner := await Outer(); "
        "    return await inner; "
        "  end Main; "
        "end C;"), 7);
}
