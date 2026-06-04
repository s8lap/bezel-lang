// Codegen — disposal of objects whose generated Dispose never ran (issue #383,
// §28 / §31). Before this fix, four kinds of owned class object were created but
// never disposed (their destructor never ran AND, since #380 makes dispose free
// the block, the block leaked):
//
//   1. an interface-typed `using var` (`using var s: IShape := new Square()`):
//      classOfLocal resolves an interface, not in disposerFns_, so no disposal
//      was registered. FIX: dispose interface-typed using-vars dynamically (the
//      object's own vtable slot 1 needs no concrete class).
//   2. a `using var` initialized by a CALL returning `owned T` (not a `new`):
//      classOfLocal only handled type annotations + NewExpr. FIX: register such
//      a call-initialized using-var for dynamic disposal. A borrow return (no
//      `owned`, §28.10) still assumes NO disposal.
//   3. a `using var u := move x.f;` (§28.8.1): the moved-out value was not
//      discharged. FIX: dispose it at scope end (dynamically). Suppressed inside
//      a destructor body, where the field auto-dispose already runs.
//   4. a thrown-and-caught exception object (§31.1): `calloc`'d at the throw and
//      never reclaimed. FIX: free it when the matching catch arm completes
//      normally (the runtime owns it after the arm; the binding is a borrow).
//      A re-raise (`throw;`) or a `throw <expr>` from the arm keeps the object
//      in flight (the enclosing handler frees it / the wrapping case is the
//      deferred entangled corner — never double-freed).
//
// Two complementary check kinds (mirroring free_on_dispose_test.cpp):
//   * destructor-count JIT tests (always run): the destructor bumps a static
//     counter, observable via the int exit code — proves Dispose now runs the
//     right number of times.
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower
//     the program to an object file, link against an ASan-instrumented runtime,
//     run under LSan — proves the block is reclaimed with no leak / no
//     double-free / no use-after-free.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

namespace fs = std::filesystem;

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Path to the Bezel runtime source (compiled with ASan for the LSan leg).
std::string runtimeSourcePath() {
    fs::path here(__FILE__);                       // tests/codegen/<this>
    fs::path root = here.parent_path().parent_path().parent_path();
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// LeakSanitizer verification. Lowers `source` to an object file (full front-end
// incl. ownership/definite-assignment), links against an ASan-instrumented
// runtime, runs under LSan. With `detectLeaks` (the default) the run reports a
// leak as a sanitizer failure; with `detectLeaks=false` only the memory-safety
// checks (use-after-free, double-free, heap overflow) remain active — used to
// assert that a path which intentionally leaves one object in flight (the #387
// wrapped/referenced case) still has NO double-free / use-after-free. Returns:
//   0  — clean (no problem of the kinds being detected).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_383_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    std::string rtCmd =
        "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
        runtimeSourcePath() + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(rtCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the destructor-count (always) and, if the toolchain is present, that
// LeakSanitizer reports no leak/corruption.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// #387 / §31.1: assert the destructor-count (always) and, with leak detection
// OFF, that there is NO double-free / use-after-free. Used for the wrapped/
// referenced re-raise case, which intentionally leaves the caught exception in
// flight (a residual single-object leak that is explicitly acceptable) — the
// soundness property to guard is that the conservatively-not-freed object is
// never double-freed and never used after free.
void expectNoUseAfterFreeOrDoubleFree(const std::string& source,
                                      int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int asan = runUnderLeakSanitizer(source, /*detectLeaks=*/false);
    if (asan == -1) return;       // toolchain unavailable: skip the ASan leg
    BEZEL_EXPECT_EQ(asan, 0);     // 0 == no UAF / no double-free / no overflow
}

// --- Gap 1: interface-typed using var (§28). ------------------------------

// `using var s: IShape := new Square()` — the concrete Square's destructor runs
// once at scope end (dynamic dispatch through the object's vtable), and the
// block is reclaimed. Before #383, classOfLocal resolved the interface (no
// disposerFns_ entry) → never disposed (leak, destructor count 0).
const char* kIfaceNew =
    "module M; interface IShape method Area(): int; end IShape; "
    "class Square : IShape "
    "  public static _n: int := 0; "
    "  public method Area(): int => 16; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do using var s: IShape := new Square(); end; return _n; "
    "  end Main; "
    "end Square;";

// `using var s := Make()` where Make(): owned IShape — the inferred local owns
// the returned object (interface-typed) and disposes it dynamically.
const char* kIfaceOwnedCall =
    "module M; interface IShape method Area(): int; end IShape; "
    "class Square : IShape "
    "  public static _n: int := 0; "
    "  public method Area(): int => 16; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Make(): owned IShape => new Square(); "
    "  public static method Main(): int "
    "    do using var s := Make(); end; return _n; "
    "  end Main; "
    "end Square;";

// --- Gap 2: using var initialized by a call returning owned T (§28.10). ----

// `using var r := Make()` where Make(): owned R — owns the result, disposes it.
const char* kOwnedCallClass =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Make(): owned R => new R(); "
    "  public static method Main(): int "
    "    do using var r := Make(); end; return _n; "
    "  end Main; "
    "end R;";

// §28.10 (negative): `using var r := Get()` where Get(): R is a BORROW return
// (no `owned`) — the using var assumes NO disposal. The shared object's
// destructor must NOT run here (it is process-bound static state). Count stays
// 0. This guards against over-disposal (which would double-free the shared
// instance). No LSan leg — the shared static R intentionally outlives main.
const char* kBorrowCallNotDisposed =
    "module M; class R "
    "  public static _n: int := 0; "
    "  public static _shared: R := new R(); "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Get(): R => _shared; "
    "  public static method Main(): int "
    "    do using var r := Get(); end; return _n; "
    "  end Main; "
    "end R;";

// --- Gap 3: using var := move owned field (§28.8.1). ----------------------

// `using var t := move _r!;` followed by `_r := null;` (field restoration). The
// moved-out R is discharged by the using var (destructor +1); the owner's field
// is null at the owner's own disposal, so it is not disposed again (no
// double-free). R._n must be 1.
const char* kMoveField =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class Owner "
    "  private owned _r: R?; "
    "  public constructor() this._r := new R(); end constructor; "
    "  public method Take() "
    "    do using var t := move _r!; _r := null; end; "
    "  end Take; "
    "  public static method Main(): int "
    "    do using var o := new Owner(); o.Take(); end; return R._n; "
    "  end Main; "
    "end Owner;";

// --- Gap 4: thrown-and-caught exception object (§31.1). -------------------

// A user exception is thrown and caught; after the arm completes (the arm falls
// through, not returning inside itself) the exception object is reclaimed — its
// owned Res field's destructor runs exactly once. The count is read AFTER the
// try (the field is disposed at arm exit, which is after the arm body's
// statements), so Res._n is 1. Before #383 the exception leaked → Res._n was 0.
const char* kExcOwnedField =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; "
    "class AppError "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end AppError; "
    "class R public static method Main(): int "
    "  try throw new AppError(); "
    "  catch AppError => (); "
    "  end; return Res._n; "
    "end Main; end R;";

// A built-in exception (DivideByZeroException) thrown by `/` and caught — its
// 16-byte block is reclaimed after the arm (no destructor to count; the LSan
// leg verifies the free). Returns 7 on the caught path.
const char* kBuiltinExcFreed =
    "module M; class R public static method Main(): int "
    "  var z := 0; "
    "  try var q := 10 / z; "
    "  catch DivideByZeroException => return 7; "
    "  end; return -1; "
    "end Main; end R;";

// Bare re-raise chain: A re-raises, B re-raises, Main catches. The single
// exception object propagates through three handlers and is freed exactly once
// (at the final handler) — the re-raise path must NOT free (no double-free).
const char* kRethrowChain =
    "module M; class E end E; "
    "class R "
    "  public static method A() try throw new E(); catch E => throw; end; end A; "
    "  public static method B() try A(); catch E => throw; end; end B; "
    "  public static method Main(): int "
    "    try B(); catch E => return 0; end; return -1; "
    "  end Main; "
    "end R;";

// --- #387: throw <newExpr> from a catch arm (§31.1 exception ownership). ---
//
// §31.1: a caught exception is a BORROW whose lifetime is the arm body; the
// spec forbids retaining it (assigning to an owned field, returning as owned,
// capturing). So constructing a NEW exception in the arm never transfers
// ownership of the caught one. Two paths, both verified:
//
//   Case A — the new exception's expression provably does NOT reference the
//   caught binding (`throw new Wrap()`): the caught object's lifetime is ending
//   with nothing retaining it, so it is freed at the throw. Here the caught
//   Inner owns a Res whose destructor runs exactly once → Res._n is 1, read by
//   the OUTER handler that catches the re-raised Wrap. Before #387 it leaked
//   (Res._n was 0). The LSan leg confirms no residual leak (and no double-free).
const char* kThrowNewUnref_FreesCaught =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; "
    "class Inner "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class Wrap end Wrap; "
    "class R public static method Main(): int "
    "  try "
    "    try throw new Inner(); "
    "    catch var e: Inner => throw new Wrap(); end; "
    "  catch Wrap => return Res._n; end; "
    "  return -1; "
    "end Main; end R;";

//   A bare-type arm (no binding) cannot name the caught exception, so a
//   `throw new Wrap()` from it always frees the caught object — same expected
//   result (Res._n == 1) via a different (no-binding) arm shape.
const char* kThrowNewBareTypeArm_FreesCaught =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; "
    "class Inner "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class Wrap end Wrap; "
    "class R public static method Main(): int "
    "  try "
    "    try throw new Inner(); "
    "    catch Inner => throw new Wrap(); end; "
    "  catch Wrap => return Res._n; end; "
    "  return -1; "
    "end Main; end R;";

//   Case B — the new exception's expression DOES reference the caught binding
//   (`throw new Wrap(e)`, e passed as a constructor arg): codegen conservatively
//   does NOT free the caught object (it might be referenced by the new one), so
//   the caught Inner stays in flight. Its Res destructor does NOT run before the
//   outer handler reads the count → Res._n is 0. This is the accepted residual
//   single-object leak; the safety property (asserted under ASan with leak
//   detection off) is NO double-free and NO use-after-free.
const char* kThrowNewRef_KeepsCaught =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; "
    "class Inner "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class Wrap "
    "  public constructor(c: Inner) (); end constructor; "
    "end Wrap; "
    "class R public static method Main(): int "
    "  try "
    "    try throw new Inner(); "
    "    catch var e: Inner => throw new Wrap(e); end; "
    "  catch Wrap => return Res._n; end; "
    "  return -1; "
    "end Main; end R;";

}  // namespace

// ---- Gap 1 ---------------------------------------------------------------

BEZEL_TEST(codegen_undisposed_383, iface_using_var_new_disposed_28) {
    // Square.destructor runs once via the interface slot; block reclaimed.
    expectDisposedAndLeakFree(kIfaceNew, 1);
}

BEZEL_TEST(codegen_undisposed_383, iface_using_var_owned_call_disposed_28) {
    expectDisposedAndLeakFree(kIfaceOwnedCall, 1);
}

// ---- Gap 2 ---------------------------------------------------------------

BEZEL_TEST(codegen_undisposed_383, owned_return_call_using_var_disposed_28_10) {
    expectDisposedAndLeakFree(kOwnedCallClass, 1);
}

BEZEL_TEST(codegen_undisposed_383, borrow_return_call_using_var_not_disposed_28_10) {
    // §28.10: a borrow-return using var assumes no disposal — destructor does
    // not run (guards against double-disposing a borrowed/shared object).
    BEZEL_EXPECT_EQ(run(kBorrowCallNotDisposed), 0);
}

// ---- Gap 3 ---------------------------------------------------------------

BEZEL_TEST(codegen_undisposed_383, move_out_of_field_into_using_var_disposed_28_8_1) {
    // Moved-out R disposed once by the using var; field null at owner dispose
    // (no double-free). LSan leg confirms the single free.
    expectDisposedAndLeakFree(kMoveField, 1);
}

// ---- Gap 4 ---------------------------------------------------------------

BEZEL_TEST(codegen_undisposed_383, caught_user_exception_owned_field_freed_31_1) {
    // The caught AppError's owned Res field is disposed once after the arm.
    expectDisposedAndLeakFree(kExcOwnedField, 1);
}

BEZEL_TEST(codegen_undisposed_383, caught_builtin_exception_freed_31_1) {
    // DivideByZeroException block reclaimed after the catch arm (LSan leg).
    expectDisposedAndLeakFree(kBuiltinExcFreed, 7);
}

BEZEL_TEST(codegen_undisposed_383, bare_rethrow_chain_freed_once_31_2_4) {
    // One exception through three handlers; freed exactly once at the last,
    // never on a re-raise edge (the LSan leg would flag a double-free).
    expectDisposedAndLeakFree(kRethrowChain, 0);
}

// ---- #387: throw <newExpr> from a catch arm (§31.1) ----------------------

BEZEL_TEST(codegen_throw_new_from_catch_387, unreferenced_new_frees_caught_31_1) {
    // §31.1: `throw new Wrap()` (does not reference the caught `e`) — the caught
    // Inner is freed at the throw, so its owned Res destructor runs once
    // (Res._n == 1). LSan leg: no residual leak, no double-free.
    expectDisposedAndLeakFree(kThrowNewUnref_FreesCaught, 1);
}

BEZEL_TEST(codegen_throw_new_from_catch_387, bare_type_arm_new_frees_caught_31_1) {
    // §31.1: a no-binding arm cannot reference the caught exception, so
    // `throw new Wrap()` from it always frees the caught Inner (Res._n == 1).
    expectDisposedAndLeakFree(kThrowNewBareTypeArm_FreesCaught, 1);
}

BEZEL_TEST(codegen_throw_new_from_catch_387, referenced_new_keeps_caught_no_uaf_31_1) {
    // §31.1: `throw new Wrap(e)` passes the caught `e` into the new exception.
    // Codegen conservatively does NOT free the caught Inner (it might be
    // referenced), so Res._n == 0 — the accepted residual single-object leak.
    // The safety property (ASan, leak detection off): no double-free, no UAF.
    expectNoUseAfterFreeOrDoubleFree(kThrowNewRef_KeepsCaught, 0);
}
