// Codegen — §30.14 / stdlib §6.8 (#2333): concurrent `Domain.Spawn` calls
// through the generic (A) -> R overload must both run and have their results
// awaited without hanging.
//
// Regression for #2333: the repro
//   using var t1 := Domain.Spawn(Toucher.Touch, 0);
//   using var t2 := Domain.Spawn(Toucher.Touch, 0);
//   _ := await t1; _ := await t2;
// compiled clean, ran clean on a single spawn, and ran clean on two
// concurrent no-arg/no-result spawns — but hung indefinitely (the spawned
// domains' entry ran, but the awaiter never observed the completion: the
// outer Task never settled, so bz_task_await_sync blocked forever) when
// two concurrent spawns went through the GENERIC (A) -> R value-arg /
// value-result overload with two `await` statements using the discard
// pattern `_ := await t;`.
//
// Root cause (fixed): the codegen helper `countAwaits` in
// LlvmBackend_Async.cpp walked the body looking for `await` inside
// LocalDecl / LocalConstDecl / AssignmentStmt / ExpressionStmt /
// ReturnStmt / YieldStmt / ForStmt / WhileStmt / IfStmt / DoEndBlock /
// TryStmt, but not `DiscardStmt` — the AST node the parser produces for
// `_ := expr` (Parser_Stmts.cpp:124-126 and 614). Two `await`s in two
// DiscardStmts therefore counted as 0 awaits, so `numAwaits` passed to
// `emitResumeDispatch` was 0, so the resume switch was built with just
// the state=0 case and a default that does `bz_task_complete` (the
// `mn.done` block). When the first spawn's completion fired the
// continuation with state=1, the switch fell through to default — the
// async state machine was effectively "stuck" because there was no
// state=1 work block to execute. `bz_task_await_sync` (the main thread's
// outer-Task await) never observed a completion → the test program
// hung.
//
// The fix adds a `DiscardStmt` case to `countAwaits` (counting awaits in
// the discard's `value` expression, exactly the same way the
// `AssignmentStmt` case already does). With the fix, `numAwaits` is 2,
// the resume switch has cases 0/1/2 plus default, the continuation
// properly resumes into the post-await t1 work (which contains the
// await t2) and from there into the post-await t2 work and `return 0`.
//
// The run-time exit code is the int returned by `Main(): Task<int>`; the
// test asserts that value is exactly the expected sum, not the
// run-timeout sentinel `r.exitCode == -1` (which `runAot` produces for a
// SIGKILL'd hang at the 30s AOT deadline) — so the test FAILS (with a
// clean -1 sentinel) on the unpatched compiler, and PASSES with the fix.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <cstdlib>
#include <sys/wait.h>

#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "codegen/LlvmBackend.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// Run the program; return the process exit code (8-bit), or -1 if the
// program was killed for exceeding the AOT run-time deadline (the hang
// case this test exists to detect). Mirrors runAot's exitCode semantics.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile, link with ASan, run.  Returns 0 (clean), 1 (sanitizer), -1 (skip).
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_2333_asan");
}

// JIT logic check (load-bearing) + ASan memory check. Mirrors
// tests/codegen/domain_spawn_lambda_valuearg_1490_test.cpp::expectCleanAndEq.
void expectCleanAndEq(const std::string& src, int expectedExit) {
    int observed = run(src);
    // The headline assertion: a hang comes back as -1 (runAot's WIFSIGNALED
    // sentinel for a SIGKILLed program past the 30s deadline). A genuinely
    // correct program returns the int the user wrote.
    BEZEL_EXPECT_EQ(observed, expectedExit);
    int san = runUnderASan(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);  // 0 = ASan/LSan clean
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────────
// #2333 headline case: the exact issue repro. Two concurrent spawns through
// `Spawn<A, R>(entry: (A) -> R, arg: A)`, both awaited via the discard
// pattern `_ := await t;`. Before the fix the resume switch was missing
// the state=1 work block, so the second await never ran and the outer
// task never completed; the program hung at the 30s AOT deadline and
// runAot returned -1.
// ──────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_concurrent_generic_await_2333, two_concurrent_spawns_awaited_via_discard) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Toucher "
        "  public static method Touch(unused: int): int "
        "    return unused + 1; "
        "  end Touch; "
        "end Toucher; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Domain.Spawn(Toucher.Touch, 0); "
        "    using var t2 := Domain.Spawn(Toucher.Touch, 0); "
        "    _ := await t1; _ := await t2; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 0);
}

// The value-returning variant: both spawned entries add a unique
// amount; the await sites use a binding (not a discard) — this exercises
// the same `countAwaits` code path for LocalDecl init expressions and
// would have hung for the same reason before the fix. Touch(5) -> 106,
// Touch(7) -> 108, sum 214. Distinct from 0/42/105 to make a wrong
// "stuck-at-constant" regression easy to spot.
BEZEL_TEST(domain_spawn_concurrent_generic_await_2333, two_concurrent_spawns_values_via_local) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Toucher "
        "  public static method Touch(unused: int): int "
        "    return unused + 100; "
        "  end Touch; "
        "end Toucher; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Domain.Spawn(Toucher.Touch, 6); "
        "    using var t2 := Domain.Spawn(Toucher.Touch, 8); "
        "    var v1 := await t1; "
        "    var v2 := await t2; "
        "    return v1 + v2; "      // 106 + 108 = 214
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 214);
}

// Mixed: the first await uses a binding, the second uses a discard.
// The fix must count awaits in BOTH shapes — LocalDecl init AND
// DiscardStmt value. (A regression that "fixed" only DiscardStmt would
// still pass this case, but the previous test catches the LocalDecl
// side; this case catches a regression that picked the wrong AST node
// for the count.)
BEZEL_TEST(domain_spawn_concurrent_generic_await_2333, mixed_binding_and_discard_awaits) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Toucher "
        "  public static method Touch(unused: int): int "
        "    return unused * 10; "
        "  end Touch; "
        "end Toucher; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Domain.Spawn(Toucher.Touch, 3); "
        "    using var t2 := Domain.Spawn(Toucher.Touch, 4); "
        "    var v1 := await t1; "
        "    _ := await t2; "
        "    return v1; "           // 30
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 30);
}

// Lambda-entry variant: same shape as the static-method case but the
// entry is a non-capturing lambda (the path fixed by #1490/#1491). The
// countAwaits call site is the same, so this exercises the SAME
// DiscardStmt fix from the lambda path. 5*2 + 6*3 = 10+18 = 28.
BEZEL_TEST(domain_spawn_concurrent_generic_await_2333, two_concurrent_lambda_entries_awaited_via_discard) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Domain.Spawn(lambda(n: int) => n * 2, 5); "
        "    using var t2 := Domain.Spawn(lambda(n: int) => n * 3, 6); "
        "    _ := await t1; "
        "    var v2 := await t2; "
        "    return v2; "           // 18
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 18);
}
