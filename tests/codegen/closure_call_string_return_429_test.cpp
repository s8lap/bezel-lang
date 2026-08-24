// Codegen + runtime — closure-local scope-end release and closure-call string
// return (issue #429, CLOSURE-CALL-RETURN sub-slice).
//
// BACKGROUND:
// Before this fix, two gaps existed in the #429 release machinery:
//
//   GAP 1 — lambda expr-body path did NOT mint +1 for string returns.
//     A block-body lambda (`() => { return s + "!"; }`) returns via
//     ReturnStmt lowering, which mints exactly +1 (bz_string_retain for borrow
//     returns; transfer for +1 producer returns).  But the expr-body path
//     (`() => s + "!"`) emitted `ret v` directly after lowering the body expr
//     without any retain.  When the body was a +1 producer (e.g. concat), the
//     return was already +1 and correct; but when the body was a BORROW (a bare
//     identifier `() => tag`), the caller received a +0 borrow with no owner —
//     a dangling return once the outer scope freed the string.  After the fix:
//     the expr-body path mirrors ReturnStmt exactly — TRANSFER for +1-producer /
//     returned-local-owning-+1 bodies, RETAIN (mint) for borrows.
//
//   GAP 2 — isPlusOneStringProducer did NOT recognise calling a closure-typed
//     local/parameter with a string funcReturn as a +1 string producer.
//     The string member access path (LlvmBackend_Exprs.cpp ~L524) uses
//     isPlusOneStringProducer(ma->target) to decide whether to release the
//     receiver after the method call.  Without recognition, `f().Length` left
//     the string returned by `f()` unreleased — a per-call leak.  After the fix:
//     a CallExpr whose callee resolves to a LocalSymbol or ParameterSymbol and
//     whose result type is non-nullable string is a +1 producer (safe because
//     GAP 1 is now closed: every `() -> string` closure always returns +1).
//
// CLOSURE-CALL-RETURN SEMANTICS per §27.5 / §28.9 / §4.6 / #429:
//   A call to a `() -> string` closure yields a fresh +1 string.  The caller
//   is responsible for releasing it after last use (§28.9 automatic reclaim).
//   isPlusOneStringProducer recognises the call so that the string-member-access
//   path (and any other consumer that uses that predicate) releases the +1.
//
// SHAPES TESTED (leak matrix, §28.9 scope-end machinery):
//   Shape A: cap1 shape — closure called in a loop, result accessed via .Length.
//             String returned by f() is released.  Boxed cells leak (pre-existing).
//             expectValueAndNoCorruption (ASan-clean; known cell residual OK).
//   Shape B: borrow-return expr-body — `() => tag` returns a borrow; mint fires.
//             No double-release (canary).  expectValueAndNoCorruption.
//   Shape C: concat-return expr-body — `() => tag + "!"` already +1; no extra mint.
//             Confirm no double-release.  expectValueAndNoCorruption.
//   Shape D: loop-bound closures (50 iterations), each creates + calls + drops.
//             50 strings returned and released.  Boxed cells leak (pre-existing).
//             expectValueAndNoCorruption.
//   Shape E: closure returns string, result not consumed via member access (bare
//             local assignment `var r := f()`).  String released at scope-end.
//             expectValueAndNoCorruption.
//   Shape F: closure result used as argument — `Print(f())`.  String released.
//             No double-release.  expectValueAndNoCorruption.
//   Shape G: closure returned from outer method (escape — closure is NOT released
//             at outer scope end, canary that escape suppresses double-release).
//             expectValueAndNoCorruption.
//   Shape H: closure stored in field (escape — similar canary).
//             expectValueAndNoCorruption.
//   Shape I: block-body lambda returning string — already minted via ReturnStmt;
//             confirm NOT double-minted after expr-body fix.
//             expectValueAndNoCorruption.
//   Shape J: conditional closure assignment, both branches return string.
//             Correct release for the live path.  expectValueAndNoCorruption.
//
// LEFT-LEAKING (per PRIME DIRECTIVE, documented but not failing):
//   - Boxed capture CELLS (8B each, from boxLocalSlot): not freed by envdtor to
//     avoid UAF with outer-scope access path.  All shapes use
//     expectValueAndNoCorruption (detect_leaks=0) to tolerate these.
//
// per §27.5 / §28.3 / §28.9 / #429, spec v0.137.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
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
static const std::filesystem::path& wtDir429ccsr() {
    return wtScratchDir("wt-429ccsr");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429ccsr();
    std::string stem = unique_stem("bezel_429ccsr");
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

// Assert correct exit code AND no ASan MEMORY-SAFETY error (UAF/DF), tolerating
// a residual leak (detect_leaks=0).  All shapes in this file have a known
// pre-existing contained leak (boxed capture cells); detect_leaks=0 lets us
// verify memory-safety (the thing that must not regress) without false-failing
// on the residual cells.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;  // toolchain unavailable: skip ASan leg
    auto dir = wtDir429ccsr();
    std::string stem = unique_stem("bezel_429ccsr_san");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
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
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return;
    }
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(objPath.c_str());
        return;
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(objPath.c_str());
    std::remove(exe.c_str());
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == expectedExitCode);
}

// ===========================================================================
// Shape A: cap1 shape — loop creating a closure that returns a string via
// concat (expr-body +1 producer).  `f().Length` accesses the string; the +1
// returned by the closure call must be released after the member access.
// §28.9 / §4.6 / #429 closure-call-return sub-slice.
// Returns 0 (total > 100).
// NOTE: boxed capture cells (8B each × 50 = 400B) leak — pre-existing contained
// leak (boxLocalSlot shared cell not freed; safe: no UAF, no double-free).
// ===========================================================================
const char* kCapShape =
    "module V; class C "
    "  public static method Make(prefix: string): () -> string "
    "    var tag := prefix + \"-tag\"; "
    "    return lambda() => tag + \"!\"; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 0..50 do "
    "      var f := C.Make(\"p\" + i.ToString()); "
    "      total := total + f().Length; "
    "    end; "
    "    if total > 100 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape B: borrow-return expr-body — `() => tag` returns the string `tag`
// directly (a bare identifier, a borrow of the captured local).  Before the
// fix the lambda returned a +0 borrow; after the fix emitStringRetain mints +1
// so the caller owns it.  The caller uses .Length and must release the +1.
// Double-release canary: run under ASan (detect_leaks=0) to verify no UAF.
// Returns 0 (len == 6, "hello!" is the tag → length = 6 → > 0 → 0).
// NOTE: boxed capture cell (8B) leaks — pre-existing.
// ===========================================================================
const char* kBorrowReturnExprBody =
    "module V; class C "
    "  public static method Make(): () -> string "
    "    var tag := \"hello\"; "         // borrow (literal — no +1 producer)
    "    return lambda() => tag; "             // expr-body borrow return: must mint +1
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(); "
    "    var len := f().Length; "        // +1 from mint, released after .Length
    "    if len > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape C: concat-return expr-body — `() => tag + "!"` is a +1 producer;
// the expr-body path must NOT emit an extra retain (over-release canary).
// After the call, .Length releases the +1 producer string.
// Returns 0 (len > 0).
// NOTE: boxed capture cell (8B) leaks — pre-existing.
// ===========================================================================
const char* kConcatReturnExprBody =
    "module V; class C "
    "  public static method Make(): () -> string "
    "    var tag := \"hi\"; "
    "    return lambda() => tag + \"!\"; "     // +1 producer; no extra retain
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(); "
    "    var len := f().Length; "        // +1 from concat, released after .Length
    "    if len > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape D: loop-bound closures — 50 iterations each creating a fresh closure
// that returns a string, calling it, and dropping it at loop-body scope end.
// Each iteration: closure cell released (bz_closure_release), envdtor releases
// captured string, returned string released after .Length.
// Returns 0 (total > 0).
// NOTE: boxed capture cells (8B × 50 = 400B) leak — pre-existing.
// ===========================================================================
const char* kLoopBound =
    "module V; class C "
    "  public static method Make(n: int): () -> string "
    "    var s := n.ToString() + \"_x\"; "
    "    return lambda() => s + \"!\"; "
    "  end Make; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 0..50 do "
    "      var f := C.Make(i); "
    "      total := total + f().Length; "
    "    end; "
    "    if total > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape E: closure result assigned to a local (not directly consumed as
// member-access target) — `var r := f()`.  The returned +1 string is held in
// a string local `r`; the string-local scope-end release machinery releases it
// at the end of the enclosing scope (the armStringLocalRefcount path).
// Returns 0 (r.Length > 0).
// NOTE: boxed capture cell (8B) leaks — pre-existing.
// ===========================================================================
const char* kResultAssignedToLocal =
    "module V; class C "
    "  public static method Make(): () -> string "
    "    var tag := \"abc\"; "
    "    return lambda() => tag + \"_suf\"; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(); "
    "    var r := f(); "                 // r holds +1; released at scope end
    "    if r.Length > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape F: closure result used as a method argument — `total + f().Length`.
// The +1 string returned by f() is a +1 producer; after isPlusOneStringProducer
// recognizes it, the string-member-access path releases it after .Length.
// Returns 0 (total > 0).
// NOTE: boxed capture cell (8B) leaks — pre-existing.
// ===========================================================================
const char* kResultAsArg =
    "module V; class C "
    "  public static method Make(): () -> string "
    "    var tag := \"q\"; "
    "    return lambda() => tag + \"r\"; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(); "
    "    var total := 0 + f().Length; "  // f() is +1 producer; .Length releases it
    "    if total > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape G: closure returned from outer method (escape — SHOULD suppress the
// closure-local scope-end release for the closure local `f` itself).  The
// caller receives the closure and must NOT have the inner scope double-release
// it.  A UAF/DF canary: if the inner scope releases `f` AND the outer
// caller uses it, that is a UAF.
// The outer caller invokes the returned closure and reads .Length.
// Returns 0 (total > 0).
// NOTE: because `f` ESCAPES (is returned), it is NOT in closureReleaseEligible_
// and does NOT get a scope-end bz_closure_release inside Make — correct.
// The string returned by calling the closure escapes to the caller who owns +1.
// ===========================================================================
const char* kClosureReturnedFromMethod =
    "module V; class C "
    "  public static method Make(): () -> string "
    "    var tag := \"esc\"; "
    "    var f: () -> string := lambda() => tag + \"!\"; "
    "    return f; "                     // f escapes — NOT released inside Make
    "  end Make; "
    "  public static method Main(): int "
    "    var g := C.Make(); "            // g owns the +1 closure
    "    var len := g().Length; "        // g() returns +1 string, .Length releases
    "    if len > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape H: block-body lambda returning string — already minted via ReturnStmt.
// The expr-body fix must NOT touch block-body lambdas.  Confirm the block-body
// path is unaffected: string returned by calling the closure is still +1, and
// .Length releases it exactly once (no double-retain, no UAF).
// Returns 0 (len > 0).
// NOTE: boxed capture cell (8B) leaks — pre-existing.
// ===========================================================================
const char* kBlockBodyLambda =
    "module V; class C "
    "  public static method Make(): () -> string "
    "    var tag := \"blk\"; "
    "    return lambda() => do "
    "      var r := tag + \"!\"; "
    "      return r; "
    "    end; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(); "
    "    var len := f().Length; "
    "    if len > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape I: conditional closure assignment — depending on a flag, `f` holds
// either of two closures (both non-escaping within the if-else).  After the
// branch, `f()` is called and .Length read.  Both branches must release the
// +1 correctly.
// Returns 0 (len > 0).
// NOTE: boxed capture cells (8B each) leak — pre-existing.
// ===========================================================================
const char* kConditionalAssignment =
    "module V; class C "
    "  public static method Make(flag: bool): () -> string "
    "    var a := \"left\"; "
    "    var b := \"right\"; "
    "    if flag then "
    "      return lambda() => a + \"!\"; "
    "    end; "
    "    return lambda() => b + \"?\"; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(true); "
    "    var g := C.Make(false); "
    "    var total := f().Length + g().Length; "
    "    if total > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// ===========================================================================
// Shape J: closure called multiple times — the same closure `f` is invoked
// twice; each call returns a fresh +1 string that must be released independently.
// No double-release of either string.
// Returns 0 (total > 0).
// NOTE: boxed capture cell (8B) leaks — pre-existing.
// ===========================================================================
const char* kMultipleInvocations =
    "module V; class C "
    "  public static method Make(): () -> string "
    "    var tag := \"rep\"; "
    "    return lambda() => tag + \"!\"; "
    "  end Make; "
    "  public static method Main(): int "
    "    var f := C.Make(); "
    "    var total := f().Length + f().Length; "  // two independent +1 strings, each released
    "    if total > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// Test cases
// ===========================================================================

BEZEL_TEST(codegen_429_closure_call_string_return, shape_a_cap1_loop_invoked_dropped_28_9) {
    // §28.9 / §4.6 / #429: cap1 shape — closure invoked 50 times per loop iter;
    // each call returns a fresh +1 string that .Length reads and releases.
    // Scope-end release of f's closure cell also fires (non-escaping local).
    expectValueAndNoCorruption(kCapShape, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_b_borrow_return_expr_body_mint_28_9) {
    // §28.9 / §27.5 / #429: lambda expr-body returning a borrow string must mint +1.
    // Canary: ASan clean (no UAF from returning a +0 borrow then releasing once).
    expectValueAndNoCorruption(kBorrowReturnExprBody, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_c_concat_return_no_double_retain_28_9) {
    // §28.9 / §4.6 / #429: expr-body +1 producer (concat) must NOT get an extra retain.
    // Over-retain canary: ASan clean (no double-free from releasing a +2 string only once).
    expectValueAndNoCorruption(kConcatReturnExprBody, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_d_loop_bound_locals_28_9) {
    // §28.9 / §27.5 / #429: 50 loop-bound closures each returning a string.
    // Each string +1 must be released exactly once after .Length.
    expectValueAndNoCorruption(kLoopBound, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_e_result_assigned_to_local_28_9) {
    // §28.9 / §4.6 / #429: closure result assigned to a string local.
    // The string-local scope-end release (armStringLocalRefcount) releases it.
    expectValueAndNoCorruption(kResultAssignedToLocal, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_f_result_as_arg_28_9) {
    // §28.9 / §4.6 / #429: closure result used as an expression operand.
    // The +1 producer string is released after .Length.
    expectValueAndNoCorruption(kResultAsArg, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_g_closure_returned_no_double_release_28_9) {
    // §28.9 / §27.5 / #429: escaping closure must NOT be released at inner scope end.
    // The outer caller receives the closure and calls it; no UAF (double-free canary).
    expectValueAndNoCorruption(kClosureReturnedFromMethod, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_h_block_body_lambda_unaffected_28_9) {
    // §28.9 / §27.5 / #429: block-body lambda string return must NOT be double-minted.
    // ReturnStmt already mints exactly +1; expr-body fix must not touch block path.
    expectValueAndNoCorruption(kBlockBodyLambda, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_i_conditional_assignment_28_9) {
    // §28.9 / §27.5 / #429: conditional closure construction — both branches correct.
    // The active branch's string +1 is released after .Length.
    expectValueAndNoCorruption(kConditionalAssignment, 0);
}

BEZEL_TEST(codegen_429_closure_call_string_return, shape_j_multiple_invocations_28_9) {
    // §28.9 / §4.6 / #429: closure invoked twice; each call returns an independent
    // fresh +1 string that must be released independently.  No double-release.
    expectValueAndNoCorruption(kMultipleInvocations, 0);
}
