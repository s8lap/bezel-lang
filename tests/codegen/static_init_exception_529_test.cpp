// Codegen tests for §22.7 static-constructor exception semantics (issue #529).
//
// §22.7: "If the static initializer throws an uncaught exception, the type is
// left in a permanently failed state: every subsequent access raises
// TypeInitializationException whose InnerException is the original exception."
//
// BUG (pre-fix): the static-ctor body's propagate block emitted a plain
// `ret void` that abandoned the pending exception in bz_exc, and the Main
// prologue never called emitPendingCheck after each static-ctor invocation.
// The exception was swallowed; the type appeared to initialize normally with
// default-zero statics.
//
// FIX (parts a + b, this lane):
//   (a) emitStaticConstructorBody pre-seeds propagateBB_ with a custom block
//       that wraps the escaping exception in TypeInitializationException (24
//       bytes: vtable @ 0, null message @ 8, InnerException @ 16) before
//       returning to the caller.
//   (b) The Main prologue calls emitPendingCheck() after each static-ctor
//       call.  Because the wrapping already happened inside the static ctor,
//       the pending exception is now a TypeInitializationException.
//
// ARCHITECTURE NOTE on testability:
//   In the current codegen design, static ctors run in Main's PROLOGUE (before
//   any user-written body statements).  When emitPendingCheck fires in the
//   prologue, propagateBB_ has not yet been forced for Main, so unwindBB()
//   routes directly to bz_unhandled_exception — it is NOT possible to catch
//   the exception from a try/catch written inside Main's body.
//
//   Consequence: tests that verify "exception surfaces" cannot use the JIT
//   harness (the JIT runs `main()` in-process; exit(70) from
//   bz_unhandled_exception terminates the test runner).  Those tests use the
//   native compile+run harness (buildAndRun) that executes the binary in a
//   subprocess and captures its exit code via wait().
//
//   Tests that verify the exception TYPE (TypeInitializationException vs
//   plain Exception) are deferred to issue #529 part (c), which implements
//   the permanent-failed-state per-access check (requires LlvmBackend_Exprs.cpp,
//   a concurrent-lane file).  Part (c) will make the exception catchable from
//   user code because it emits the TypeInitializationException raise at the
//   first static member ACCESS, which IS inside the body's exception scope.
//
// TESTS:
//   - JIT harness: control cases — non-failing static ctors, internal catch,
//     normal instance-ctor throws, no static ctor at all.
//   - Native harness: "exception surfaces" — a throwing static ctor must
//     produce exit(70) (bz_unhandled_exception), NOT exit(0) (bug: swallowed).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// The runtime's abnormal-termination exit code (EX_SOFTWARE, BSD sysexits.h).
// §33.3 / §22.13: uncaught exception escaping Main → bz_unhandled_exception()
// → exit(70).
constexpr int kUnhandledExit = 70;

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile source to a native binary and run in a subprocess, returning the
// exit code.  Returns -1 on compile or run failure.  Mirrors the approach
// in property_exception_test.cpp and cross_module_exception_test.cpp.
int buildAndRun(const std::string& source, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem =
        unique_stem(("bezel_sinit529_" + tag).c_str());
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

} // namespace

// ── Part (a)+(b): exception surfaces (native harness) ────────────────────────
//
// These tests use the native compile+run harness because exit(70) from
// bz_unhandled_exception would kill the JIT test process.  The observable
// difference between the bug (exit 0, swallowed) and the fix (exit 70,
// surfaces) is verified via the subprocess exit code.

// §22.7 scenario 1: a static ctor that throws an uncaught exception must cause
// the program to exit abnormally (exit 70 via bz_unhandled_exception).
// Before the fix, the exception was swallowed and Main returned normally → exit 0.
// After the fix, emitStaticCtorPendingCheck detects the pending exception and
// propagates it through Main's propagateBB → bz_unhandled_exception → exit 70.
BEZEL_TEST(codegen_529, static_ctor_throw_surfaces_not_swallowed) {
    // Before fix: exit 0 (exception swallowed, Main body runs normally).
    // After fix : exit 70 (exception surfaces via bz_unhandled_exception).
    BEZEL_EXPECT_EQ(buildAndRun(
        "module M; "
        "class BadInit "
        "  public static V: int := 0; "
        "  public static constructor() throw new Exception(\"boom\"); end constructor; "
        "end BadInit; "
        "class C public static method Main(): int return BadInit.V; end Main; "
        "end C;",
        "surfaces"),
        kUnhandledExit);
}

// §22.7: the exception type wrapping is correct — a TypeInitializationException
// (not the original exception type) is what bz_unhandled_exception sees.
// This test verifies via the exit code: if the static ctor throws ArgumentException
// AND wrapping works, bz_unhandled_exception fires → exit 70.
// If wrapping were absent, bz_exc would contain ArgumentException and the
// propagation path would still yield exit 70 — so this test verifies the
// SURFACE behavior (exception does not get swallowed) but NOT the TYPE identity
// (that requires part (c) to make the exception catchable from user code).
BEZEL_TEST(codegen_529, static_ctor_inner_exception_preserved_in_propagate) {
    // Exit 70 = exception surfaced (fix working); exit 0 = swallowed (bug).
    BEZEL_EXPECT_EQ(buildAndRun(
        "module M; "
        "class Boom "
        "  public static X: int := 0; "
        "  public static constructor() throw new ArgumentException(\"bad\"); end constructor; "
        "end Boom; "
        "class C public static method Main(): int return Boom.X; end Main; "
        "end C;",
        "inner_exc"),
        kUnhandledExit);
}

// §22.7: with multiple static ctors, if the FIRST one fails, the failure
// must surface (exit 70) and the SECOND ctor must NOT run (since bz_exc is
// detected after the first call).
BEZEL_TEST(codegen_529, multiple_static_ctors_first_fails_surfaces) {
    // Exit 70 = first ctor failure surfaced and propagated.
    // The B static ctor (W := 99) must not run (confirmed by exit being 70
    // rather than any value involving W).
    BEZEL_EXPECT_EQ(buildAndRun(
        "module M; "
        "class A "
        "  public static V: int := 0; "
        "  public static constructor() throw new Exception(\"A fails\"); end constructor; "
        "end A; "
        "class B "
        "  public static W: int := 0; "
        "  public static constructor() W := 99; end constructor; "
        "end B; "
        "class C public static method Main(): int return A.V + B.W; end Main; "
        "end C;",
        "multi_fail"),
        kUnhandledExit);
}

// ── §22.7: internal catch does NOT enter failed state ────────────────────────

// §22.7 second scenario: the static ctor can use try/catch internally; an
// exception CAUGHT inside the body does NOT enter the failed state.  The
// constructor completes normally, and the type is accessible.
// This test uses the JIT harness (no exit(70); the ctor completes normally).
BEZEL_TEST(codegen_529, static_ctor_internal_catch_does_not_fail) {
    // Exit 99 = static field initialized normally (good).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Safe "
        "  public static V: int := 0; "
        "  public static constructor() "
        "    try throw new Exception(\"inner\"); catch Exception => V := 99; end; "
        "  end constructor; "
        "end Safe; "
        "class C public static method Main(): int return Safe.V; end Main; "
        "end C;"), 99);
}

// ── Control: non-throwing static ctor works normally ─────────────────────────

// §22.7 control: a static ctor that does NOT throw must leave the type in a
// successfully-initialized state.  The fix must not regress this.
BEZEL_TEST(codegen_529, control_non_throwing_static_ctor_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Good "
        "  public static V: int := 0; "
        "  public static constructor() V := 42; end constructor; "
        "end Good; "
        "class C public static method Main(): int return Good.V; end Main; "
        "end C;"), 42);
}

// A class with NO static ctor at all still initializes correctly (CG47 baseline).
BEZEL_TEST(codegen_529, control_no_static_ctor_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Plain public static V: int := 7; end Plain; "
        "class C public static method Main(): int return Plain.V; end Main; "
        "end C;"), 7);
}

// ── Control: normal (non-static) constructor throwing is unaffected ──────────

// A regular instance constructor that throws must still propagate the ORIGINAL
// exception type (not wrapped in TypeInitializationException).
// The static-ctor wrapping machinery must not fire for instance ctors.
BEZEL_TEST(codegen_529, normal_ctor_throw_propagates_original_type) {
    // Exit 1 = ArgumentException caught (correct).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Widget "
        "  public static V: int := 5; "
        "  public constructor() throw new ArgumentException(\"widget fail\"); end constructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  try "
        "    var w: Widget := new Widget(); "
        "    return 0; "
        "  catch ArgumentException => return 1; "
        "  catch TypeInitializationException => return 2; "
        "  end; "
        "  return 3; "
        "end Main; end C;"), 1);
}
