// Codegen + runtime — boxed capture cell free at outer-scope exit (issue #429,
// CELL-FREE sub-slice).
//
// BACKGROUND (#429 cell-free sub-slice):
// A boxed capture cell is an 8-byte heap allocation produced by `boxLocalSlot`
// in LlvmBackend_Layout.cpp.  It is the shared heap object that holds the
// actual value of a by-reference captured local: the outer scope's `newSlot`
// alloca points to it, AND the closure env block holds a pointer to it too.
//
// The env-disposer (#672) and capture-retain sub-slices already ensured that:
//   (a) the closure env is freed when the closure is released, and
//   (b) string/function retainables in the env are reference-counted correctly.
//
// The REMAINING LEAK was the cell itself: after every closureRelease / envdtor
// fires, the outer scope's slot alloca still holds a stale pointer to the (now
// dead) cell, and the cell is never freed.  This is 8 bytes × N cells.
//
// DESIGN (b) — non-escaping subset:
// A cell is freed at outer-scope exit iff EVERY lambda that captured the local
// is proven non-escaping (i.e. its owning `var` local is in
// closureReleaseEligible_ — per-design, that means the lambda is never returned
// or stored into a longer-lived location).  Escaping lambdas hold a raw pointer
// into the cell via their env block; freeing the cell while the env lives is a
// UAF.  Non-escaping lambdas are guaranteed to be fully released BEFORE scope
// exit (via the closureRelease disposal), so after the closureRelease fires the
// cell is dead.
//
// Ordering guarantee (no UAF, per §27.5 reverse-declaration):
//   emitDisposals iterates in reverse-declaration order (LIFO).  The lambda
//   local is declared AFTER the captured variable, so its `closureRelease`
//   disposal fires FIRST → bz_closure_release → envdtor reads the cell values →
//   THEN the `boxedCellFree` disposal fires and frees the cell.  The cell is
//   never read after this point.
//
// SHAPES TESTED:
//   Shape A1: non-escaping capture of int — cell freed at scope exit.
//             LSan-CLEAN with detect_leaks=1.  Value correct.
//   Shape A2: non-escaping capture of string — cell freed at scope exit.
//             LSan-CLEAN.  String also freed via envdtor (combined).
//   Shape A3: two non-escaping lambdas sharing one cell — cell freed once.
//             LSan-CLEAN.  No double-free (second lambda's closureRelease
//             fires first, then cell freed once).
//   Shape A4: non-escaping capture in nested do-block — cell freed at block exit.
//             LSan-CLEAN.
//   Shape A5: param captured by non-escaping lambda — cell freed.
//             LSan-CLEAN.
//   Shape B1: escaping lambda (returned) capturing int — cell NOT freed.
//             ASan detect_leaks=0 (UAF canary): no UAF/DF, clean execution.
//             The cell leaks (design (b) boundary — documented).
//   Shape B2: escaping lambda (stored in field) — cell NOT freed.
//             ASan-clean (no UAF/DF).
//
// EXCLUDED SHAPES (documented non-goals for design (b)):
//   - For-loop variables (fresh cell each iteration; scope-exit free would
//     only reclaim the last cell; excluded from cellFreeEligible_).
//   - Any local captured by BOTH escaping and non-escaping lambdas.
//
// per §27.5 / §28.3 / §28.9 / #429 cell-free sub-slice.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir429cell() {
    return wtScratchDir("wt-429cell");
}

// Compile `source` to a temp native executable via the real Driver pipeline
// and run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429cell();
    std::string stem = unique_stem("bezel_429cell");
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

// Compile, ASan-link, and run under LeakSanitizer (detect_leaks=1).
// Returns:
//   0  — correct exit code AND LSan-clean (no leak, no UAF/DF).
//   1  — sanitizer reported a problem (leak or memory error).
//  -1  — toolchain unavailable / build step failed: SKIP.
int runUnderLeakSanitizer(const std::string& source, int expectedExitCode) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir429cell();
    std::string stem = unique_stem("bezel_429cell_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    // detect_leaks=1: any surviving allocation is a LSan failure (exit 99).
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (!WIFEXITED(status)) return 1;                 // signal → failure
    int ec = WEXITSTATUS(status);
    if (ec == 99) return 1;                           // LSan leak → failure
    if (ec != expectedExitCode) return 1;             // wrong result → failure
    return 0;
}

// Assert correct exit code AND no memory-safety error, tolerating leaks.
// Used for escaping-capture shapes where cells legitimately survive scope.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;
    auto dir = wtDir429cell();
    std::string stem = unique_stem("bezel_429cell_safe");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return;
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return;
    }
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// ---------------------------------------------------------------------------
// Non-escaping capture shapes (design (b) cell-free subset)
// ---------------------------------------------------------------------------

// ===========================================================================
// Shape A1: non-escaping int capture — cell freed at scope exit.
//
// §27.5 / #429 cell-free: `k` is captured by a lambda assigned to `f`.
// `f` is non-escaping (closureReleaseEligible_): it never leaves the scope.
// Pre-scan (collectCellFreeEligible): `k`'s cell is in cellFreeEligible_.
// At lowerLocalDecl(f): boxedCellFree disposal registered for `k`'s cell.
// emitDisposals at scope exit: closureRelease(f) fires FIRST (f declared after k),
// THEN free(cell_of_k).  No UAF — f is already released before the cell is freed.
// Returns 0 (r = k*2 = 14).
// LSan-CLEAN: the 8-byte cell is freed at scope exit.
// ===========================================================================
const char* kNonEscapingIntCaptureSimple =
    "module M; class C "
    "  public static method Main(): int "
    "    var k: int := 7; "
    "    var f: () -> int := lambda() => k * 2; "  // captures k (non-escaping)
    "    var r: int := f(); "                 // r = 14
    "    if r = 14 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape A2: non-escaping string capture — both cell AND string freed.
//
// §27.5 / §28.9 / #429: `s` (a +1 string) is captured by a non-escaping lambda.
// The envdtor (capture-retain sub-slice) frees the string.
// The boxedCellFree (this sub-slice) frees the 8-byte cell.
// Both leaks plugged.  LSan-CLEAN.
// Returns 0 (length > 0).
// ===========================================================================
const char* kNonEscapingStringCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"ab\" + \"cd\"; "     // +1 producer, rc=1
    "    var f: () -> int := lambda() => s.Length; "    // captures s (non-escaping)
    "    var r: int := f(); "                      // r = 4
    "    if r = 4 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape A3: two non-escaping lambdas sharing ONE cell.
//
// §27.5 / #429: `n` is captured by both `f` and `g`.  Both are non-escaping.
// Pre-scan: the first lambda to box `n` creates the cell; the second finds it
// already in cellVars_ (already-boxed path) and does NOT add a second entry
// to lastLambdaCapturedCells_.  The boxedCellFree is registered ONCE.
// At scope exit: closureRelease(g) fires, then closureRelease(f), then
// free(cell_of_n).  No double-free.
// LSan-CLEAN: one cell freed once.
// Returns 0.
// ===========================================================================
const char* kTwoNonEscapingLambdasSameCell =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 5; "
    "    var f: () -> int := lambda() => n + 1; "     // first to box n
    "    var g: () -> int := lambda() => n * 2; "     // also captures n (already-boxed)
    "    var r: int := f() + g(); "             // (5+1)+(5*2)=16
    "    if r = 16 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape A4: non-escaping capture in a nested do-end block.
//
// §27.5 / #429: `x` declared inside a `do ... end` block; captured by a
// non-escaping lambda.  The block's disposeStack scope triggers boxedCellFree
// at block exit (not method exit).  Inner-scope freeing still sound because
// the lambda is also scoped to the block.
// Returns 0 (total > 0).
// LSan-CLEAN.
// ===========================================================================
const char* kNonEscapingCaptureInDoBlock =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    do "
    "      var x: int := 42; "
    "      var f: () -> int := lambda() => x; "   // non-escaping inside do-block
    "      total := total + f(); "
    "    end; "
    "    if total = 42 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape A5: method parameter captured by non-escaping lambda.
//
// §27.5 / §6.2 / #429: a parameter `p` is captured by a non-escaping lambda.
// Parameters are boxed with boxLocalSlot just like locals; the cell is freed
// at method exit when closureRelease fires for the lambda, then boxedCellFree.
// Returns 0 (f(p) = 2*p = 10).
// LSan-CLEAN.
// ===========================================================================
const char* kParamCaptureNonEscaping =
    "module M; class C "
    "  public static method Doubled(p: int): int "
    "    var f: () -> int := lambda() => p * 2; "    // captures param p (non-escaping)
    "    return f(); "
    "  end Doubled; "
    "  public static method Main(): int "
    "    if C.Doubled(5) = 10 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape A6: non-escaping capture churn (loop — NOT a for-loop variable).
//
// A while loop that creates a fresh non-escaping lambda every iteration.
// The captured local (`n`) is a `var` declared inside the loop body (each
// iteration introduces a new declaration), so each iteration boxes a fresh
// cell and the cell is freed at the end of each iteration's scope.
// LSan-CLEAN (no accumulating leak).
// Returns 0 (sum > 0).
// ===========================================================================
const char* kNonEscapingChurnLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 5000 do "
    "      var n: int := i; "                     // fresh n each iter
    "      var f: () -> int := lambda() => n + 1; "     // non-escaping capture
    "      total := total + f(); "
    "      i := i + 1; "
    "    end; "
    "    if total > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// Escaping capture shapes (design (b) boundary — cells survive, no UAF)
// ---------------------------------------------------------------------------

// ===========================================================================
// Shape B1: escaping lambda (returned from method) — cell NOT freed early.
//
// §27.5 / #429 design (b) boundary: `tag` is captured by a lambda that is
// RETURNED from the method → the lambda escapes.  Pre-scan classifies it as
// escaping.  boxedCellFree is NOT registered.  The cell leaks (8 bytes) — but
// there is no UAF: the cell pointer in the env is valid for the lifetime of
// the returned closure.  ASan-clean (detect_leaks=0 tolerates the 8B leak).
// Returns 0 (closure result > 0).
// ===========================================================================
const char* kEscapingLambdaReturned =
    "module M; class C "
    "  public static method Make(v: int): () -> int "
    "    var tag: int := v * 3; "
    "    return lambda() => tag; "          // escaping: returned from method
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(4); "        // f holds escaping closure; tag cell leaks
    "    var r := f(); "              // r = 12
    "    if r = 12 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape B2: escaping lambda (stored into a class field) — cell NOT freed.
//
// §27.5 / #429 design (b) boundary: `x` captured by a lambda stored into
// H.fn (a class field).  The lambda escapes via field-store.  Cell NOT freed.
// After field-release (when `using var h` is disposed), the closure's
// bz_closure_release fires → envdtor (capture-retain sub-slice) releases the
// captured string correctly.  No UAF on the cell (still valid when the closure
// accesses it before release).  ASan-clean.
// Returns 0 (correct result).
// ===========================================================================
const char* kEscapingLambdaStoredInField =
    "module M; class H "
    "  public fn: () -> int := lambda() => 0; "
    "  destructor end destructor; "
    "  public static method Main(): int "
    "    var result: int := 0; "
    "    do "
    "      using var h := new H(); "
    "      var x: int := 99; "
    "      h.fn := lambda() => x; "         // lambda escapes via field-store; cell leaks
    "      result := h.fn(); "         // result = 99
    "    end; "
    "    if result = 99 then return 0; end; return 1; "
    "  end Main; "
    "end H;";

// ===========================================================================
// #2182 / §28.3: IIFE — `(lambda() => …)()` — non-escaping capture shapes.
//
// An IIFE's callee-lambda is non-escaping: Calls2.cpp emits the closure call
// then emitClosureRelease within the same expression, so the closure env never
// outlives the enclosing scope. Before #2182, collectCellFreeEligible's walker
// reached every CallExpr's callee via the generic `expr(callee, false)` arm
// (topPos=false), misclassifying the IIFE's lambda as escaping — its captures
// landed in capturedByEscaping, the cellFreeEligible_ gate failed for them in
// lowerLambda, and the boxed capture cell was never registered for a scope-end
// boxedCellFree. Worse, even when a later path DID record the cell in
// lastLambdaCapturedCells_, the IIFE has no LocalDecl to drive the consumption
// in lowerLocalDecl, so the registration never happened. Result: per-invocation
// leak of the boxed capture cell (16B for a record value, 8B for an int cell),
// unbounded in a loop.
//
// #2182 adds:
//   (a) collectCellFreeEligible's CallExpr arm special-cases a LambdaExpr
//       callee and collects its captures as non-escaping;
//   (b) lowerCall's IIFE arm calls the extracted consumeLastLambdaCapturedCells()
//       helper (the same per-cell soundness loop lowerLocalDecl runs for
//       `var f := lambda`) right after lowerLambda.
//
// All four shapes below LEAKED without the fix (Direct leak of N bytes) and are
// LSan-CLEAN with it. Spec: §28.3 (scope-end auto-disposal of owned/boxed
// capture cells), §28.9 (closure captures are borrows — only the cell, not the
// captured value, is reclaimed here), §27.5 (closure env / cell layout).
// ===========================================================================

// #2182 / §28.3 Shape I1: IIFE capturing an int — the simplest variant.
//   Without fix: 8B per-invocation leak of the int capture cell.
//   With fix: cell freed at scope end → LSan-CLEAN.
// Returns 0 (r = 7*2 = 14).
const char* kIifeIntCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var k: int := 7; "
    "    var r: int := (lambda() => k * 2)(); "   // IIFE captures k (non-escaping)
    "    if r = 14 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// #2182 / §28.3 Shape I2: IIFE capturing a RECORD value (the issue's repro 2).
//   A `record Holder` is a value type; capturing it boxes the value into a
//   heap cell so the env can hold a pointer. The cell (16B for a record
//   holding a ptr + 2 ints) leaked per-invocation before #2182; the envdtor
//   releases only string/function captures, never a plain boxed value type.
//   With the fix, the cell enters cellFreeEligible_ and is freed at scope end.
// Returns 0 (r = h.S[0] = 7).
const char* kIifeRecordCapture =
    "module M; record Holder(S: ArraySpan<int>); "
    "class C public static method Main(): int "
    "  var local := new Array<int>(); local.Add(7); "
    "  var h := new Holder(local.AsSpan()); "
    "  var r := (lambda(): int => h.S[0])(); "
    "  if r = 7 then return 0; end; return 1; "
    "end Main; end C;";

// #2182 / §28.3 Shape I3: IIFE capturing a record value in a LOOP — the
//   per-invocation leak class (the issue's "PER-INVOCATION leak … unbounded
//   in a loop"). 1000 iterations; without the fix LSan reports
//   "Direct leak of 16000 byte(s) in 1000 object(s)". With the fix: LSan-CLEAN.
// Each iteration declares a fresh `h` (block-scoped to the loop body), so the
// capture cell is allocated and freed per-iteration — the steady-state test.
// Returns 0 (sum = 7000).
const char* kIifeRecordCaptureLoop =
    "module M; record Holder(S: ArraySpan<int>); "
    "class C public static method Main(): int "
    "  var sum: int := 0; var i: int := 0; "
    "  while i < 1000 do "
    "    var local := new Array<int>(); local.Add(7); "
    "    var h := new Holder(local.AsSpan()); "
    "    sum := sum + (lambda(): int => h.S[0])(); "
    "    i := i + 1; "
    "  end while; "
    "  if sum = 7000 then return 0; end; return 1; "
    "end Main; end C;";

// #2182 / §28.3 Shape I4: IIFE capturing a string — exercises the envdtor's
//   string-release branch in the IIFE position. The +1 string is released by
//   the envdtor at closure last-drop; the boxed cell is freed at scope end.
//   Without #2182 the cell leaked (8B) while the string was correctly released
//   (envdtor path was unaffected). With the fix: both clean.
// Returns 0 (r = s.Length = 4).
const char* kIifeStringCapture =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"ab\" + \"cd\"; "
    "    var r: int := (lambda() => s.Length)(); "
    "    if r = 4 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// Tests — non-escaping shapes: cells freed → LSan-CLEAN
// ===========================================================================

// §27.5 / #429 cell-free Shape A1: non-escaping int capture.
// Pre-scan classifies lambda as non-escaping; boxedCellFree registered for `k`.
// closureRelease fires first (lambda released at scope exit), THEN cell freed.
// LSan-CLEAN: no 8-byte cell leak.  Value correct: r=14.
BEZEL_TEST(boxed_cell_free_429, spec_27_5_non_escaping_int_capture_lsan_clean) {
    BEZEL_EXPECT_EQ(compileAndRun(kNonEscapingIntCaptureSimple), 0);
    int lsan = runUnderLeakSanitizer(kNonEscapingIntCaptureSimple, 0);
    if (lsan == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §27.5 / §28.9 / #429 cell-free Shape A2: non-escaping string capture.
// Cell freed AND string freed via envdtor (both leaks plugged).  LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_27_5_28_9_non_escaping_string_capture_lsan_clean) {
    BEZEL_EXPECT_EQ(compileAndRun(kNonEscapingStringCapture), 0);
    int lsan = runUnderLeakSanitizer(kNonEscapingStringCapture, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §27.5 / #429 cell-free Shape A3: two non-escaping lambdas sharing one cell.
// boxedCellFree registered ONCE (already-boxed path skips second registration).
// Cell freed exactly once — no double-free.  LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_27_5_two_non_escaping_lambdas_shared_cell_lsan_clean) {
    BEZEL_EXPECT_EQ(compileAndRun(kTwoNonEscapingLambdasSameCell), 0);
    int lsan = runUnderLeakSanitizer(kTwoNonEscapingLambdasSameCell, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §27.5 / #429 cell-free Shape A4: non-escaping capture in a do-end block.
// Cell freed at block exit (block's disposeStack scope).  LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_27_5_non_escaping_capture_do_block_lsan_clean) {
    BEZEL_EXPECT_EQ(compileAndRun(kNonEscapingCaptureInDoBlock), 0);
    int lsan = runUnderLeakSanitizer(kNonEscapingCaptureInDoBlock, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §27.5 / §6.2 / #429 cell-free Shape A5: method parameter captured by
// non-escaping lambda.  Cell freed at method exit.  LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_27_5_6_2_param_non_escaping_capture_lsan_clean) {
    BEZEL_EXPECT_EQ(compileAndRun(kParamCaptureNonEscaping), 0);
    int lsan = runUnderLeakSanitizer(kParamCaptureNonEscaping, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §27.5 / #429 cell-free Shape A6: while-loop churn with non-escaping captures.
// Each iteration's cell freed at iteration's block exit.  No accumulated leak.
// LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_27_5_non_escaping_capture_loop_lsan_clean) {
    BEZEL_EXPECT_EQ(compileAndRun(kNonEscapingChurnLoop), 0);
    int lsan = runUnderLeakSanitizer(kNonEscapingChurnLoop, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ===========================================================================
// Tests — escaping capture shapes (design (b) boundary): no UAF, cells survive
// ===========================================================================

// §27.5 / #429 design (b) boundary Shape B1: returned escaping lambda.
// Pre-scan marks lambda as escaping; boxedCellFree NOT registered for `tag`.
// Cell leaks 8 bytes — documented design (b) residual.  No UAF: cell is still
// alive when the returned closure accesses it.  ASan-clean (detect_leaks=0).
BEZEL_TEST(boxed_cell_free_429, spec_27_5_escaping_returned_lambda_no_uaf) {
    expectValueAndNoCorruption(kEscapingLambdaReturned, 0);
}

// §27.5 / #429 design (b) boundary Shape B2: lambda stored in a field.
// Lambda escapes via field-store; cell NOT freed.  Field-release fires closure's
// bz_closure_release which triggers envdtor if applicable.  No UAF on cell.
// ASan-clean (detect_leaks=0, cell leaks 8 bytes — documented residual).
BEZEL_TEST(boxed_cell_free_429, spec_27_5_escaping_field_stored_lambda_no_uaf) {
    expectValueAndNoCorruption(kEscapingLambdaStoredInField, 0);
}

// ===========================================================================
// #2182 / §28.3 Tests — IIFE non-escaping captures (the bug this fix ships)
// ===========================================================================

// #2182 / §28.3 Shape I1: IIFE capturing an int — the boxed int capture cell
// (8 bytes) leaked per-invocation before #2182; collectCellFreeEligible
// misclassified the IIFE callee-lambda as escaping. With the fix the cell is
// freed at scope end. LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_28_3_iife_int_capture_cell_freed_2182) {
    BEZEL_EXPECT_EQ(compileAndRun(kIifeIntCapture), 0);
    int lsan = runUnderLeakSanitizer(kIifeIntCapture, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// #2182 / §28.3 Shape I2: IIFE capturing a record value (the issue's repro 2).
// A boxed record value (16 bytes) leaked per-invocation. With the fix the cell
// enters cellFreeEligible_ and is freed at scope end. LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_28_3_iife_record_capture_cell_freed_2182) {
    BEZEL_EXPECT_EQ(compileAndRun(kIifeRecordCapture), 0);
    int lsan = runUnderLeakSanitizer(kIifeRecordCapture, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// #2182 / §28.3 Shape I3: IIFE capturing a record value in a 1000-iteration
// loop — the per-invocation leak class. Without the fix LSan reported
// "Direct leak of 16000 byte(s) in 1000 object(s)". With the fix each
// iteration's capture cell is freed at the loop-body block exit. LSan-CLEAN.
BEZEL_TEST(boxed_cell_free_429, spec_28_3_iife_record_capture_loop_lsan_clean_2182) {
    BEZEL_EXPECT_EQ(compileAndRun(kIifeRecordCaptureLoop), 0);
    int lsan = runUnderLeakSanitizer(kIifeRecordCaptureLoop, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// #2182 / §28.3 Shape I4: IIFE capturing a string — confirms the envdtor's
// string-release branch still fires in the IIFE position (the boxed cell holding
// the captured string local is freed at scope end; the +1 string is released
// by the envdtor at closure last-drop). Without #2182 the cell leaked 8B while
// the string itself was already correctly released. With the fix both are clean.
BEZEL_TEST(boxed_cell_free_429, spec_28_3_iife_string_capture_cell_freed_2182) {
    BEZEL_EXPECT_EQ(compileAndRun(kIifeStringCapture), 0);
    int lsan = runUnderLeakSanitizer(kIifeStringCapture, 0);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}
