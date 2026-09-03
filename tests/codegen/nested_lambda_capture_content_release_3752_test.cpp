// Codegen: §28.2's checked-disposal backstop reached THROUGH a closure whose
// capture was made from a strictly deeper scope, and the §27.5 capture-lifetime
// defect underneath it (issue #3752).
//
// WHAT THE ISSUE CLAIMED, AND WHAT IT ACTUALLY IS.  #3752 was filed as a
// §28.2 backstop hole: the method-level spelling of a program raises a clean
// `DisposedAccessException` under `--checked-disposal`, while the spelling that
// makes the SAME store from inside a nested lambda body SIGSEGVs.  Measured on
// `r12-cycle-1` (89eb914) the disposal is not the discriminator at all — the
// same program with the `Dispose()` REMOVED also SIGSEGVs, in BOTH modes, and
// so does a program with no disposable object in it anywhere:
//
//     var f := lambda(): int => 11;
//     do var g := lambda(): int => f(); total := g(); end;
//     return total + f();      <- heap-use-after-free, freed by bz$lam$N$envdtor
//
// §27.5 says "Captured variables are heap-allocated in a closure object.
// Modifications to a captured variable inside the lambda are visible outside,
// and vice versa" — the captured local remains the programmer's live variable.
// Codegen's capture model (#672/#429) makes the FIRST capturing env's envdtor
// the captured local's DEFERRED scope-end release: escape detection suppresses
// the local's own release and the env performs it instead.  That is sound only
// while the env outlives the local's scope.  When the capture site sits in a
// STRICTLY DEEPER scope than the local's declaration, the closure holding that
// env is released at the INNER scope's end and the envdtor frees a string /
// closure the outer local still owns and still reads.
//
// The §28.2 consequence is what #3752 saw: the backstop cannot fire through a
// closure the envdtor already freed.  §28.3 *K4 — Kills, site-announcement, and
// the backstop posture*, clause 1 (v0.286): "Disposal is idempotent and
// flag-setting; in checked disposal mode (§28.2) any later use through any
// alias faults deterministically as `DisposedAccessException`, and unchecked
// mode is the same language with the check elided.  This is the runtime backstop
// every trust admission names." — and the consume table's row K3.10 (explicit
// `Dispose()`) is marked **unfrozen** precisely because that flag is its safety
// net.  §28.2 *Use after disposal; checked disposal mode* is the named mode.
//
// THE FIX routes the release to the scope that owns the local: the envdtor's
// content release is suppressed for such a capture and ONE release is registered
// in the owner's scope, under the same `cellFreeEligible_` / `declScopeDepth_`
// gate #3508 already uses to defer the CELL free.  Accounting stays exact — the
// ×500 rows below are ASan+LSan clean, so this is not a leak trade.
//
// Every row asserts a VALUE (a returned int or a printed string), never a crash
// signature: per the two-oracle rule (#2633) a `WIFSIGNALED` assertion is
// oracle-dependent, while a wrong/absent value is not.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;
using bezel::test::runAotOut;

namespace {

// Run in §28.2 checked-disposal MODE (`bezelc --checked-disposal`), NOT the
// ASan memory net (that is runAotChecked). Returns the exit code, or -999 when
// the program failed to compile or died without a normal exit — which is what
// the pre-fix compiler produced for the nested-capture row (SIGSEGV).
int runChecked(const std::string& program) {
    auto r = runAotCheckedDisposal(program);
    return r.ran ? r.exitCode : -999;
}

int runPlain(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// The disposable used by the two §28.2 rows: an ordinary class with one
// instance method, so the method-entry disposed-flag guard (§28.2, issue #601)
// is the thing that fires.
const char* kR =
    "module M; import System; "
    "class R "
    "  private v: int := 5; "
    "  public method V(): int return this.v; end V; "
    "end R; ";

} // namespace


// ── §28.2 / §28.3 K4 clause 1 (v0.286): the backstop through a closure
//    assigned from inside a NESTED LAMBDA BODY — issue #3752's repro B ──────

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_28_2_backstop_through_nested_lambda_store) {
    // The store `f := lambda(): int => r.V();` is made from inside `mk`'s body,
    // so `f` (a method-level local) is captured by `mk`, whose closure dies at
    // the `do … end` block's end. Pre-fix the envdtor freed `f` there and the
    // later `f()` was a use-after-free (SIGSEGV, rc=139, -999 here) in BOTH
    // modes. With the release routed to `f`'s own scope, `f` is alive at the
    // call and §28.2's method-entry guard on the disposed `r` raises
    // DisposedAccessException, which the arm turns into 1.
    BEZEL_EXPECT_EQ(runChecked(
        std::string(kR) +
        "class C "
        "  public static method Take(): int "
        "    var r := new R(); "
        "    var f := lambda(): int => 0; "
        "    var total := 0; "
        "    do "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      total := mk(); "
        "    end; "
        "    r.Dispose(); "
        "    try "
        "      return total + f(); "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Take; "
        "  public static method Main(): int return Take(); end Main; "
        "end C;"), 1);
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_28_2_method_level_twin_is_the_control) {
    // #3752's repro A — the same program with the store made at METHOD level.
    // This was already correct before the fix and must stay correct: it is the
    // control that says the fix did not move the method-level behaviour.
    BEZEL_EXPECT_EQ(runChecked(
        std::string(kR) +
        "class C "
        "  public static method Take(): int "
        "    var r := new R(); "
        "    var f := lambda(): int => 0; "
        "    f := lambda(): int => r.V(); "
        "    r.Dispose(); "
        "    try "
        "      return f(); "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Take; "
        "  public static method Main(): int return Take(); end Main; "
        "end C;"), 1);
}


// ── §27.5 (v0.286): the defect underneath — a captured local read after the
//    capturing closure's scope. No disposal, no disposable object. ──────────

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_closure_captured_in_do_block_outlives_it) {
    // `f` is declared at method level and captured by `g`, declared inside a
    // `do … end`. Pre-fix `g`'s envdtor released `f` at the block's end and the
    // trailing `f()` read freed memory (SIGSEGV). 11 (inside) + 11 (after).
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    do "
        "      var g := lambda(): int => f(); "
        "      total := g(); "
        "    end; "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "22");
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_string_captured_in_do_block_outlives_it) {
    // The `string` half of the same capture model (the envdtor's other content
    // branch). A heap-built string, so no literal/sentinel shortcut hides it.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Take(): string "
        "    var s := \"he\" + \"llo\"; "
        "    var total := 0; "
        "    do "
        "      var g := lambda(): int => s.Length; "
        "      total := g(); "
        "    end; "
        "    return s + \"/\" + total.ToString(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "hello/5");
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_capture_inside_if_branch_outlives_it) {
    // The scope need not be a `do … end`: an `if` branch pushes a dispose scope
    // too, so the same premature envdtor fired there.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    if true then "
        "      var g := lambda(): int => f(); "
        "      total := g(); "
        "    end; "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "22");
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_capture_inside_while_body_outlives_the_loop) {
    // A loop body is the same shape with a backward edge above the capture:
    // ONE guarded cell backs every iteration's closure, so pre-fix each
    // iteration's envdtor released the same reference again. Two iterations
    // (11 + 11) plus the post-loop read (11).
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 2 do "
        "      var g := lambda(): int => f(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "33");
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_same_scope_capture_is_unchanged) {
    // The capture site at the SAME depth as the declaration is outside the fix's
    // gate (`declaredShallower` is false) and was always correct: the env dies
    // at the same scope end, after the local's last read. The control that says
    // the deferral did not disturb the ordinary case.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var g := lambda(): int => f(); "
        "    var total := g(); "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "22");
}


// ── §28.9 / §27.5: the accounting is EXACT, not a leak trade ──────────────
//    runAotChecked = generated-code ASan + LeakSanitizer. Loop-amplified to
//    500 rounds: a one-shot row would pass on both sides of the fix for a leak
//    (the last round's allocation is still reachable at exit), and a one-shot
//    UAF can land in mapped memory.

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_28_9_closure_capture_x500_is_memory_clean) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Round(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    do "
        "      var g := lambda(): int => f(); "
        "      total := g(); "
        "    end; "
        "    return total + f(); "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 11000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_28_9_string_capture_x500_is_memory_clean) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Round(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var total := 0; "
        "    do "
        "      var g := lambda(): int => s.Length; "
        "      total := g(); "
        "    end; "
        "    return total + s.Length; "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 5000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}


// ── §28.2 unchecked mode is "the same language with the check elided" ─────
//    A CORRECT program (no use-after-dispose) must behave identically in both
//    modes — §28.2's own conformance statement, and the property the fix must
//    not disturb now that it changes codegen in both modes alike.

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_28_2_correct_program_agrees_in_both_modes) {
    const std::string prog =
        "module M; import System; "
        "class C "
        "  public static method Take(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    do "
        "      var g := lambda(): int => f(); "
        "      total := g(); "
        "    end; "
        "    return total + f(); "
        "  end Take; "
        "  public static method Main(): int return Take(); end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runPlain(prog), 22);
    BEZEL_EXPECT_EQ(runChecked(prog), 22);
}


// ── The VENUES the fix repaired that the first round of rows did not pin ──
//    Each was measured `heap-use-after-free ... in bz$lam$N$envdtor` on the
//    parent and clean with the fix; without a row the repair is unprotected.
//    All are ×200 under runAotChecked (generated-code ASan + LeakSanitizer):
//    a one-shot row cannot see a leak (the last round is still reachable at
//    exit) and a one-shot UAF can land in mapped memory.

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_30_7_async_method_venue_x200_is_memory_clean) {
    // The §30 async state machine lowers the same capture through its resume
    // function. Parent: `heap-use-after-free ... in bz$async$M.C$Take$Resume`.
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static async method Round(): Task<int> "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    do "
        "      var g := lambda(): int => f(); "
        "      total := g(); "
        "    end; "
        "    return total + f(); "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 200 do "
        "      using var t := Round(); "
        "      t.Wait(); "
        "      acc := acc + t.Result; "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 4400 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_29_3_sync_iterator_full_drain_x200_is_memory_clean) {
    // The §29 sync iterator lowers the capture through MoveNext. Parent:
    // `heap-use-after-free ... in bz$iter$M.C$Gen$MoveNext`. Fully DRAINED —
    // the abandonment edge keeps the pre-existing §27.5 residue by design (the
    // yield-handler exclusion list this fix's kind joins), so a `break` here
    // would pin a different, unfixed thing.
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var f := lambda(): int => 11; "
        "    do "
        "      var g := lambda(): int => f(); "
        "      yield return g(); "
        "    end; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 200 do "
        "      for var x in Gen() do "
        "        acc := acc + x; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 4400 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_in_loop_capture_x200_is_memory_clean) {
    // The LOOP shape, pinned for its SECOND-ORDER effect as much as the UAF.
    // Suppressing the envdtor release flips `hasOwnerDisposal` true at
    // `consumeLastLambdaCapturedCells` (the new kind is deliberately NOT on
    // that whitelist), which registers a `boxedCellFreeDeferred` for a cell
    // #3508's `loopStack_.empty()` narrowing had left unreclaimed. So this row
    // asserts BOTH halves at once: the parent's use-after-free is gone AND the
    // cell it would have leaked is reclaimed — a clean LSan exit proves the
    // second, which no value assertion could.
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Round(): int "
        "    var f := lambda(): int => 11; "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 3 do "
        "      var g := lambda(): int => f(); "
        "      total := total + g(); "
        "      i := i + 1; "
        "    end; "
        "    return total + f(); "
        "  end Round; "
        "  public static method Main(): int "
        "    var k := 0; "
        "    var acc := 0; "
        "    while k < 200 do "
        "      acc := acc + Round(); "
        "      k := k + 1; "
        "    end; "
        "    if acc <> 8800 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_inside_a_sync_lambda_body_x200_is_memory_clean) {
    // THE POST-MERGE INTERACTION, pinned because it is invisible in either
    // commit alone. #3757/#3762 retarget `cellFreeEligibleFn_` to a sync
    // lambda's OWN function, which opens this fix's gate (and #3508's) INSIDE
    // a nested lambda body — a venue where `disposeStack_` was cleared and
    // rebuilt for the lambda while `declScopeDepth_` still holds the ENCLOSING
    // function's depths. The two must not be compared across that boundary.
    //
    // They are not, and the reason is structural rather than lucky: a local
    // DECLARED in the lambda body records its depth against the lambda's own
    // (rebuilt) `disposeStack_`, so the comparison is within one frame; and an
    // OUTER function's local reachable inside the lambda body is already boxed
    // by that lambda's own capture, so the inner capture is never the FIRST one
    // and the `!alreadyBoxed` clause (Exprs2.cpp ~1220) declines it before any
    // depth is read (`collectLambdaCaptures` applies no declaration-site filter). This row pins the first half — the one that would
    // register an owner-scope release into the WRONG dispose stack if the
    // depths ever crossed.
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Round(): int "
        "    var outer := lambda(): int => do "
        "      var f := lambda(): int => 11; "
        "      var total := 0; "
        "      do "
        "        var g := lambda(): int => f(); "
        "        total := g(); "
        "      end; "
        "      return total + f(); "
        "    end; "
        "    return outer(); "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 200 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 4400 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}


// ── The CROSS-MODULE twin ────────────────────────────────────────────────
//    The capture path is module-agnostic by construction — it keys on the
//    captured local's own sema type (`string` / function) and its declaration
//    depth, neither of which records where a class came from. But "by
//    construction" is what an untested claim always says, and the importing
//    venue is the one where the disposed object's layout comes from loaded
//    metadata rather than from this compilation. So: an IMPORTED class held by
//    `using var` in the importer, alongside a function-typed AND a `string`
//    local captured from a deeper scope, built and run with generated-code
//    ASan + LeakSanitizer on BOTH halves.

namespace {

namespace n3752_fs = std::filesystem;

struct N3752TempFile {
    std::string path;
    explicit N3752TempFile(std::string p) : path(std::move(p)) {}
    ~N3752TempFile() { std::remove(path.c_str()); }
};

void n3752Write(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an ASan-instrumented object, compile `mainSrc` against it
// as an executable with the same instrumentation, run it under LeakSanitizer,
// and return the exit code. -1 for any compile/link/spawn failure, so a broken
// scaffold can never be read as a pass (the rows below assert == 0).
int n3752BuildRunAsan(const std::string& libSrc, const std::string& mainSrc,
                      const std::string& tag, bool detectLeaks) {
    auto dir = n3752_fs::temp_directory_path();
    const std::string stem = "bezel_3752_" + tag + "_" + unique_stem("");
    N3752TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    N3752TempFile lObj((dir / (stem + "_lib.o")).string());
    N3752TempFile mSrc((dir / (stem + "_main.bzl")).string());
    N3752TempFile mExe((dir / stem).string());

    n3752Write(lSrc.path, libSrc);
    n3752Write(mSrc.path, mainSrc);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;   // generated-code ASan on the IMPORTER

    bezel::Driver driver;
    // The library half is built WITHOUT the sanitizer: `--sanitize=address`
    // is an executable-build option ("it is not supported with -c (library
    // object)", main.cpp), and an instrumented `-c` compile emits no loadable
    // `.bezel.meta` — the importer then fails with "cannot load module
    // metadata", which this helper would report as -1. The instrumentation
    // that matters is the IMPORTER's: every load this fix governs is emitted
    // there, in `Round`.
    bezel::codegen::CodegenOptions libOpts;
    libOpts.isMainModule = false;
    bezel::CompileResult rLib =
        driver.compileObject(lSrc.path, lObj.path, {}, libOpts);
    if (!rLib.success) return -1;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    deps.push_back(d);

    bezel::CompileResult rMain =
        driver.compileFile(mSrc.path, mExe.path, deps, opts);
    if (!rMain.success) return -1;

    // exitcode=99 distinguishes a sanitizer finding from the program's own
    // status; abort_on_error=0 keeps it a normal exit so WIFEXITED holds under
    // either oracle (a crash-signature assertion would be oracle-dependent —
    // #2633 / R10-G). `detectLeaks` is the caller's choice because the two
    // findings have different owners here; see the row.
    const std::string cmd = std::string("ASAN_OPTIONS=detect_leaks=") +
                            (detectLeaks ? "1" : "0") +
                            ":exitcode=99:abort_on_error=0 " + mExe.path;
    int status = std::system(cmd.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

const char* kN3752Lib =
    "module Lib; "
    "public class Res "
    "  private v: int := 11; "
    "  public method V(): int return this.v; end V; "
    "end Res;";

}  // namespace

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_27_5_cross_module_capture_x200_is_memory_clean) {
    // `f` (function-typed) and `s` (`string`) are locals of the IMPORTER,
    // captured by a lambda declared one dispose scope deeper, and read after
    // that scope closes — the fix's exact shape — while an imported `Lib.Res`
    // is held by `using var` in the same frame so the disposal machinery of a
    // metadata-loaded class runs beside it. 200 rounds under generated-code
    // ASan.
    //
    // LEAKSANITIZER IS OFF HERE, DELIBERATELY, AND THIS IS THE ONE ROW WHERE
    // IT IS. Measured across the fix boundary on this exact program:
    //     parent (3b751a8)   heap-use-after-free in bz$M.C$Round
    //     with the fix       no fault; 1600 B leaked in 200 allocations
    // The residue is 8 B per round — ONE CAPTURE CELL, not a content
    // reference. `g` captures TWO locals, and the first capture's #2303
    // null-guard splits the block, which pushes the second onto boxLocalSlot's
    // GUARDED path; #3455's cell-reclaim arms then decline it. That is issue
    // 3532's family ("declared shallower than its capture site and read
    // afterwards, never reclaims its cell"), it is untouched by this commit,
    // and on the parent it was INVISIBLE because the use-after-free fired
    // first. Asserting leak-freedom here would pin somebody else's open bug
    // and fail; asserting ASan-cleanliness pins exactly what this commit
    // repaired. The single-capture twins DO assert leak-freedom (the x500 rows
    // above), so the accounting claim is not weakened — only this shape's
    // pre-existing cell residue is excluded, by name.
    int code = n3752BuildRunAsan(
        kN3752Lib,
        "module M; import System; import Lib; "
        "class C "
        "  public static method Round(): int "
        "    using var r := new Res(); "
        "    var f := lambda(): int => r.V(); "
        "    var s := \"he\" + \"llo\"; "
        "    var total := 0; "
        "    do "
        "      var g := lambda(): int => f() + s.Length; "
        "      total := g(); "
        "    end; "
        "    return total + f() + s.Length; "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 200 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 6400 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;",
        "xmod", /*detectLeaks=*/false);
    BEZEL_EXPECT_EQ(code, 0);
}


// ── The #2706 interaction, pinned ────────────────────────────────────────
//    This fix assigns `suppressContentRcInEnvdtor` AFTER
//    `mutatedSharedCaptureCell` has already decided it, so a cell #2706
//    deliberately left unreleased can now take an owner-scope release instead.
//    That ordering is the only sound one — #2706 suppresses the RETAIN too, so
//    the cell's final value carries exactly the local's own single +1 and the
//    owner-scope release consumes exactly that one — but "sound on paper" is
//    what an unpinned second-order effect always claims, and the failure mode
//    if the reasoning is wrong is a DOUBLE FREE, not a leak.
//
//    The shape that reaches #2706: a `string` local ASSIGNED after the capture
//    (`assignedCaptureVars_`) and captured by TWO lambda nodes (so
//    `singleCaptureLocals_` declines and `manyEnvsShareOneCell` holds), both
//    declared one dispose scope deeper than the local. Measured x200:
//        parent (3b751a8)   7600 B leaked in 400 allocations
//        with the fix       clean, value unchanged
//    So the #2718 residual SHRINKS in this shape rather than moving, and no
//    over-release appears.

BEZEL_TEST(codegen_nested_lambda_capture_3752, spec_28_9_mutated_shared_cell_x200_is_memory_clean) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Round(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var total := 0; "
        "    do "
        "      var g1 := lambda(): int => s.Length; "
        "      var g2 := lambda(): int => s.Length + 1; "
        "      total := g1() + g2(); "
        "    end; "
        "    s := \"wor\" + \"ld\"; "
        "    return total + s.Length; "
        "  end Round; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 200 do "
        "      acc := acc + Round(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 3200 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
