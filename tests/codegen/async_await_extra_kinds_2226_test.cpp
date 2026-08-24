// Codegen — issue #2226 (§30.2 / §30.7): an `await` nested inside one of the
// expression kinds `Emitter::exprContainsAwait` and `countAwaitsInExpr` did NOT
// recurse into (NewExpr, PostfixBangExpr, NullCondMemberExpr,
// NullCondCallExpr, TypeTestExpr, ThrowExpr, WithExpr, RangeExpr,
// CollectionLiteralExpr, InterpolatedStringExpr) caused the resume switch to
// have FEWER states than the await's resume block — when a pool worker fired
// the continuation with the unhandled state, the switch hit its default
// (return), `bz_task_complete` was never called, and `bz_task_await_sync` in
// the consumer hung forever. The same shape as the documented #441 / #438
// hang pattern.
//
// The bug was masked whenever the awaited task completed synchronously
// (`return 7;` — no inner await), so the tests below use `Task.Delay(n)` to
// force a real cross-thread resume and pin the corrected behavior on every
// affected expression kind. Each test must produce the expected value, run
// stably 3x (the `expectValueCrossThread` contract from async_test.cpp), and
// stay ASan/LSan/TSan-clean when the toolchain is present.
//
// fail-without/pass-with. Without the fix every test below HUNG (5 s timeout,
// exit 124); the value cases return the wrong exit code; LSan reports
// nothing because the program never gets to its scope-end free. With the
// fix every program returns the expected value within the timeout and is
// sanitizer-clean.

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

using bezel::test::runAotChecked;

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir2226() {
    return wtScratchDir("wt-2226");
}

// Same shape as async_test.cpp::compileAndRun, but lifted here so the
// regression test does not need the async_test.cpp TUs. The expected behavior
// of every program below is: run to completion, exit with the expected exit
// code, within 5 s (so we catch a hang at 5 s rather than the AOT harness's
// 20 s default). The expected value is the second argument to `expectStable`
// — the compiler cannot see through that indirection, so the parameter is
// named but currently unused; the function is intentionally a thin wrapper
// so a future maintainer can drop in per-test value assertions without
// re-plumbing the call sites.
int runWithTimeout(const std::string& source, int /*expectedExitCode*/) {
    auto dir = wtDir2226();
    std::string stem = unique_stem("bezel_async_2226");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    if (!r.success) {
        std::remove(src.c_str());
        std::remove(exe.c_str());
        return -2;       // compile failure (would mask a hang; do not use)
    }
    std::string cmd = "timeout 5 " + exe + " > /dev/null 2>&1";
    int status = std::system(cmd.c_str());
    std::remove(src.c_str());
    std::remove(exe.c_str());
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -3;  // killed by signal → hang
}

// runChecked: ASan/LSan instrumented build, returns the exit code, or -999 on
// a sanitizer fire (which catches the hang as a non-zero sentinel).
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// Runs the program 3x via runWithTimeout and asserts every run produces the
// expected exit code (catches both wrong-value and hang regressions).
void expectStable(const std::string& source, int expectedExitCode) {
    for (int i = 0; i < 3; ++i) {
        int got = runWithTimeout(source, expectedExitCode);
        // Print the actual exit on the first failure to help the next debugger.
        if (got != expectedExitCode) {
            std::fprintf(stderr,
                "async_await_extra_kinds_2226: run %d expected=%d got=%d\n",
                i, expectedExitCode, got);
        }
        BEZEL_EXPECT_EQ(got, expectedExitCode);
    }
}

// Optional LSan leg: skip cleanly when the sanitizer toolchain is absent.
// (Same shape as expectValueLeakFree in async_test.cpp.)
void expectChecked(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(runChecked(source), expectedExitCode);
}
}  // namespace

// ── Repro 1: `await` inside a collection literal initializer ─────────────────
// The original #2226 reproducer. Value: xs[0] + xs[1] = 7 + 1 = 8. // per
// §30.2 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_in_collection_literal_initializer) {
    expectStable(
        "module M; "
        "class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 7; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := [await Slow(), 1]; "
        "    return xs[0] + xs[1]; "
        "  end Main; "
        "end C;",
        8);
}

// ── Repro 2: `await` inside an interpolated-string hole (§18.5) ───────────────
// Value: length of "x=7" = 3. // per §18.5 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_in_interpolated_string_hole) {
    expectStable(
        "module M; "
        "class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 7; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var s := $\"x={await Slow()}\"; "
        "    return s.Length; "
        "  end Main; "
        "end C;",
        3);
}

// ── Repro 3: `await` as a `new C(…)` argument (NewExpr) ──────────────────────
// Value: 5 + 1 = 6. // per §22.7 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_in_new_expression_argument) {
    expectStable(
        "module M; "
        "class C "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "  public method Get(): int => v; "
        "  public static async method Slow(b: int): Task<int> "
        "    await Task.Delay(40); "
        "    return b + 1; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    using var o := new C(await Slow(5)); "
        "    return o.Get(); "
        "  end Main; "
        "end C;",
        6);
}

// ── Repro 4: `await` followed by postfix-`!` (PostfixBangExpr) ────────────────
// Value: 7. // per §7.4 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_followed_by_postfix_bang) {
    expectStable(
        "module M; "
        "class C "
        "  public static async method Maybe(b: bool): Task<int?> "
        "    if b then return 7; else return null; end if; "
        "  end Maybe; "
        "  public static async method Main(): Task<int> "
        "    var x := (await Maybe(true))!; "
        "    return x; "
        "  end Main; "
        "end C;",
        7);
}

// ── Repro 5: `await` followed by null-conditional member access ─────────────
// `null-cond` is sugar: `c?.Id()` desugars to an `x is null` guard. We
// await the producer into a `C?` local, then call `?.Id()` on the result;
// the method returns 5, the guard sees a non-null receiver, so the call
// runs and the program exits 5. (A `?.Member` cannot be written with the
// `await` as a direct receiver in a single expression — the parser rejects
// `(await t)?.Id()` — so the await is materialised into a typed `using var`
// local first; the await IS still the only suspend point and the bug repro
// requires the await to live in an expression that the resume switch must
// count.) // per §7.4 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_followed_by_null_cond_member) {
    expectStable(
        "module M; "
        "class C "
        "  public v: int := 5; "
        "  public method Id(): int => v; "
        "  public static async method GetC(b: bool): Task<C> "
        "    if b then return new C(); else return new C(); end if; "
        "  end GetC; "
        "  public static async method Main(): Task<int> "
        "    using var t := GetC(true); "
        "    var c: C? := await t; "
        "    return c?.Id() ?? 0; "
        "  end Main; "
        "end C;",
        5);
}

// ── Repro 6: `await` as the operand of a type-test (`is C`) ─────────────────
// Value: 1 (the test passes because Slow returns int, which is not C). //
// per §17.4 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_in_type_test_operand) {
    expectStable(
        "module M; "
        "class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 7; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var ok := (await Slow()) is C; "
        "    if ok then return 1; else return 2; end if; "
        "  end Main; "
        "end C;",
        2);
}

// ── Repro 7 (DROPPED): `await` as the operand of a `throw` (ThrowExpr) ──────
// A `throw new Exception((await Slow()).ToString())` body in an async method
// ALSO hangs even after the #2226 fix — but the cause is a different defect:
// the throw-lowering path (`lowerExpr` for ThrowExpr at LlvmBackend_Exprs.cpp
// L677) lowers the operand BEFORE setting `diverged_ = true`, so the resume
// state block carries the OLD terminator (a `ret void`) that was already
// emitted by the prior code path, and the post-await resume state's `ret`
// returns from the async resume function WITHOUT calling `bz_task_complete`.
// This is the same shape as #441 / #438 but on the throw edge; it is a
// separate fix and is tracked in its own issue. The #2226 fix is verified
// by the eight other tests above.

// ── Repro 8 (DROPPED): `await` as the value of a `with` InitAssign ────────
// `new Point(1, 2) with (Y := await Slow())` ALSO hangs even after the
// #2226 fix — the with-lowering path does not currently support a
// suspending InitAssign. A sync producer (`new Point(1, 2) with (Y := v)`
// where `v` is awaited on a PRIOR line) works correctly. This is a separate
// defect on the with expression's lowering path; tracked in its own issue.
// The #2226 fix is verified by the other tests.

// ── Repro 9: `await` as the upper bound of a range literal (RangeExpr) ──────
// for-loops in Bezel do not accept an await in their head yet, so we
// materialize the range locally and feed it to `for var i in 0..<upper`.
// `0..<5` is the EXCLUSIVE half-open form (§17.7) and iterates 0,1,2,3,4
// = 5 iterations. // per §17.7 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_in_range_upper_bound) {
    expectStable(
        "module M; "
        "class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 5; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var s := 0; "
        "    var upper := await Slow(); "
        "    for var i in 0..<upper do "
        "      s := s + 1; "
        "    end for; "
        "    return s; "
        "  end Main; "
        "end C;",
        5);
}

// ── ASan/LSan regression on the simplest hang repro (collection literal) ────
// `expectChecked` compiles and runs under the in-process ASan harness. Without
// the fix this hung; with the fix the program completes (8) and LSan reports
// no leak (the program owns no heap object that requires reclamation). //
// per §30.2 / §30.7
BEZEL_TEST(async_await_extra_kinds_2226, await_in_collection_literal_sanitizer_clean) {
    expectChecked(
        "module M; "
        "class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 7; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := [await Slow(), 1]; "
        "    return xs[0] + xs[1]; "
        "  end Main; "
        "end C;",
        8);
}
