// Codegen — §30.14 / stdlib §6.8 (#1490): a non-capturing lambda spawn entry
// with a VALUE-typed spawn argument must not crash.
//
// Regression for #1490: `Domain.Spawn((n: int) => n * n, 12)` compiled clean
// but crashed at runtime (glibc pthread `tpp.c` assertion / a spurious
// `std::system_error`). The control form — the same computation via a static
// method reference, `Domain.Spawn(Program.Sq, 12)` — ran correctly, isolating
// the defect to the lambda-entry adapter specifically.
//
// Root cause (fixed): the lambda-entry spawn adapter added by #1228 only
// handled the ZERO-extra-arg shape `Domain.Spawn(() => …)` — it built the
// lambda's indirect-call type as `(ptr env) -> R` unconditionally and bailed
// out (returned nullptr, falling through to unrelated generic call codegen)
// whenever `call.args.size() != 1`. For a spawn entry with a value parameter
// the lambda actually lowers to a closure fn of type `(ptr env, T) -> R`
// (LlvmBackend_Exprs2.cpp's lowerLambda), so calling it as `(ptr env) -> R`
// through a mismatched function-type indirect call is undefined behavior —
// it corrupted the stack/registers the pthread trampoline that runs the
// spawned domain's start routine reads, producing the glibc-level crash.
// The fix teaches the adapter to build the lambda's TRUE fn type (env plus
// each user parameter's LLVM type) and to pack the closure cell together
// with the extra Domain.Spawn argument(s) into one malloc'd array that the
// adapter unpacks before invoking the lambda — the same shape the sibling
// static-method-entry adapter already uses for its own extra-argument path.
//
// stdlib §6.8's Spawn overload set admits exactly this shape:
//   Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>  where A: notnull, R: notnull
//
// These run through the in-process JIT (bezel::test::runJit) — the load-
// bearing correctness check — and additionally through an ASan-linked native
// binary, mirroring tests/codegen/domain_spawn_lambda_result_1491_test.cpp.

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

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile, link with ASan, run.  Returns 0 (clean), 1 (sanitizer), -1 (skip).
// Mirrors tests/codegen/domain_spawn_lambda_result_1491_test.cpp::runUnderASan.
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_1490_asan");
}

// JIT logic check (load-bearing) + ASan memory check.
void expectCleanAndEq(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
    int san = runUnderASan(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);  // 0 = ASan/LSan clean
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────────
// #1490: the exact issue repro — a single `int` value-arg lambda entry.
// `(n: int) => n * n` with arg 12 : (int) -> int → Spawn<A,R>(entry: (A) -> R,
// arg: A): owned Task<R>, A = R = int. Before the fix this crashed at runtime
// (glibc pthread assertion / std::system_error) rather than joining 144.
// ──────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_lambda_valuearg_1490, int_valuearg_lambda_entry_no_crash) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda(n: int) => n * n, 12); "
        "    var v := await t; "
        "    return v; "        // exit == joined result; must be 144, not a crash.
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 144);
}

// Control from the issue: the same computation via a static method reference
// must keep working (it already did before the fix — a non-regression check
// that the lambda-entry fix did not disturb the sibling adapter).
BEZEL_TEST(domain_spawn_lambda_valuearg_1490, static_method_valuearg_control) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static method Sq(n: int): int "
        "    return n * n; "
        "  end Sq; "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Program.Sq, 12); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 144);
}

// A `string` value-arg (a pointer-typed lambda parameter, exercising the
// packed-array's ptr-passthrough slot rather than the int truncation slot).
// stdlib §6.8's `Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>` with
// A = string, R = int (string.Length).
BEZEL_TEST(domain_spawn_lambda_valuearg_1490, string_valuearg_lambda_entry) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda(s: string) => s.Length, \"hello\"); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 5);
}

// Guard against a regression on the #1228/#1491 zero-extra-arg lambda-entry
// shape the value-arg fix's restructuring sits alongside.
BEZEL_TEST(domain_spawn_lambda_valuearg_1490, zero_arg_lambda_entry_still_works) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda() => 42); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 42);
}
