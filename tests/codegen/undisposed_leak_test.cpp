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
#include "native_test_harness.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
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

// JIT a program with the FULL front-end INCLUDING the ownership pass, then run
// `main` and return its exit code (-999 if it did not compile/run). The shared
// runJit helper skips ownership::checkOwnership; #1009's discard-of-owned
// disposal is FLAGGED by that pass (DiscardStmt::disposeOwnedValue), so a value
// test for it must run the pass (mirrors iterator_owned_param_945's runner).
int runOwn(const std::string& src) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -999;
    bezel::test::ensureNativeTargetInitialized();
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
    if (!module) return -999;
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) { llvm::consumeError(jitOrErr.takeError()); return -999; }
    auto jit = std::move(*jitOrErr);
    if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
    } else {
        llvm::consumeError(gen.takeError());
    }
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(llvmCtx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    return sym->toPtr<int (*)()>()();
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
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_383_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
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
        std::remove(exe.c_str());
    };
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

// --- #387 / #397: throw <newExpr> from a catch arm (§31.1 exception
// ownership). The original #387 guard freed the caught exception only when the
// thrown expression provably did not reference the catch binding. #397 relaxes
// that: now that #430's compile-time diagnostic rejects aliasing-escape cases
// at the source level, the caught exception is freed at every `throw <newExpr>`
// from a catch arm, EXCEPT a direct re-throw `throw ex;` of the same binding
// (which re-circulates the exact same object). Four cases:
//
//   A — `throw new Wrap()` (no reference to the caught binding): unchanged.
//       The caught Inner is freed at the throw → Res._n == 1. (§31.1 / #387)
//
//   B — bare-type arm (no binding): no catch variable to reference, so the
//       caught object is always freed. Unchanged. (§31.1 / #387)
//
//   C — `throw new WrapCode(e.Code)` (value projection, the REFERENCE case
//       that #397 fixes): `e.Code` is an int field — a value copy, not the
//       exception object itself. The old #387 guard saw `ex` referenced in the
//       expression via member-access and conservatively leaked Inner. After
//       #397 the guard is replaced by the direct-re-throw check: Inner IS
//       freed → Res._n == 1 (was 0 before #397). ASan/LSan-clean. (§31.1)
//
//   D — `throw ex;` (direct re-throw of the catch binding): the same exception
//       object re-enters the unwind path. It must NOT be freed by the inner
//       handler — the outer handler catches and frees it exactly once.
//       Res._n == 1, no double-free. (§31.1 / §31.2.4)
//
//   E — `throw new Wrap(e)` (catch borrow passed by reference): COMPILE ERROR
//       (#430). #430's ownership diagnostic rejects programs that pass a catch
//       borrow to a constructor of an object that escapes the arm (thrown or
//       returned), because the arm's free of `ex` at handler-end would then
//       dangle the aliasing reference in Wrap. This path never reaches codegen.
//       Verified in tests/ownership/catch_borrow_escaping_via_ctor_test.cpp.

// Case A — throw new Wrap() (no reference to caught binding).
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

// Case B — bare-type arm (no binding) — caught object always freed.
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

// Case C — #397 repro: throw new WrapCode(e.Code) where Code is an int field.
// The thrown expression DOES reference the catch binding (e.Code), but only as
// a value projection — the int is copied before any free. After #397 the caught
// Inner IS freed at the throw → Res._n == 1 (was 0 = leak before #397).
// §31.1 / #397.
const char* kThrowNewValueRef_FreesCaught =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; "
    "class Inner "
    "  public Code: int := 7; "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class WrapCode "
    "  private _code: int; "
    "  public constructor(code: int) this._code := code; end constructor; "
    "end WrapCode; "
    "class R public static method Main(): int "
    "  try "
    "    try throw new Inner(); "
    "    catch var e: Inner => throw new WrapCode(e.Code); end; "
    "  catch WrapCode => return Res._n; end; "
    "  return -1; "
    "end Main; end R;";

// Case D — direct re-throw `throw ex;` (§31.1 / §31.2.4).
// The inner arm re-throws the SAME caught Inner via a direct identifier
// re-throw. The inner handler must NOT free Inner — the same object re-enters
// the unwind path and is caught+freed by the outer handler exactly once.
// The outer arm falls through (body = `()`), triggering the caughtException
// disposal at arm-end. `return Res._n` is read AFTER the outer arm completes
// so it sees 1 (Inner freed exactly once). No double-free.
const char* kDirectExplicitRethrow_FreeOnce =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; "
    "class Inner "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class R public static method Main(): int "
    "  try "
    "    try throw new Inner(); "
    "    catch var e: Inner => throw e; end; "
    "  catch Inner => (); end; "
    "  return Res._n; "
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

// ---- #387 / #397: throw <newExpr> from a catch arm (§31.1) ---------------

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

// ---- #397: relax #387 guard — value-projection reference now frees (§31.1) -

BEZEL_TEST(codegen_throw_new_from_catch_397, value_proj_ref_frees_caught_31_1) {
    // §31.1 / #397: `throw new WrapCode(e.Code)` — the thrown expression
    // references the catch binding via a MEMBER ACCESS (`e.Code`), but only
    // as a value copy (int). The old #387 guard conservatively leaked Inner
    // because it saw `e` in the expression tree. After #397 (and relying on
    // #430 to have rejected true aliasing escapes), Inner IS freed at the
    // throw → Res._n == 1. ASan/LSan-clean (the #387 single-object leak is
    // gone — this is the primary regression gate for #397).
    expectDisposedAndLeakFree(kThrowNewValueRef_FreesCaught, 1);
}

BEZEL_TEST(codegen_throw_new_from_catch_397, direct_explicit_rethrow_no_double_free_31_1) {
    // §31.1 / §31.2.4 / #397: `throw ex;` from a catch arm is a direct
    // re-throw of the SAME caught exception object. The #397 relaxation must
    // NOT free the caught object on a direct re-throw — the object re-enters
    // the unwind path and is caught+freed by the outer handler exactly once.
    // The outer arm falls through; `return Res._n` reads the counter AFTER
    // the outer arm's caughtException disposal → Res._n == 1. ASan/LSan leg
    // verifies no double-free and no UAF — the cardinal risk of #397.
    expectDisposedAndLeakFree(kDirectExplicitRethrow_FreeOnce, 1);
}

// §17.13 v0.152 (#1009): discarding a NON-trivial-dispose value (`_ := expr;`)
// is now PERMITTED — it disposes the value at end-of-statement (the "unbound
// owned temporary" form, §28.3). The ownership pass marks an owned-PRODUCER
// class/interface discard and codegen disposes it via the object's vtable. The
// static counter _n (dtor +1) returns 1, proving the destructor ran EXACTLY
// once; ASan confirms no leak and no double-free. (Pre-v0.150 this was a reject.)
BEZEL_TEST(undisposed_leak, discard_class_with_destructor_disposed_724_1009) {
    const char* src =
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  _ := new R(); "
        "  return R._n; "            // 1 — disposed exactly once at statement end
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 1);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// §17.13 v0.152 (#1009): a derived class inheriting a destructor is non-trivial-
// dispose; `_ := new Derived()` disposes via the most-derived vtable dispatch
// (the base destructor runs), exactly once. ASan clean.
BEZEL_TEST(undisposed_leak, discard_derived_inherits_destructor_disposed_724_1009) {
    const char* src =
        "module M; "
        "class Base public static _n: int := 0; public constructor() end constructor; "
        "  destructor _n := _n + 1; end destructor; end Base; "
        "class Derived: Base public constructor() base(); end constructor; end Derived; "
        "class C public static method Main(): int "
        "  _ := new Derived(); "
        "  return Base._n; "         // 1 — disposed once via dynamic dispatch
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 1);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// §17.13 v0.152 (#1009): `_ := <call returning owned T>` disposes the result at
// statement end — the explicit "dispose this owned result now" form. Exactly once.
BEZEL_TEST(undisposed_leak, discard_owned_returning_call_disposed_1009) {
    const char* src =
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C "
        "  public static method Mk(): owned R return new R(); end Mk; "
        "  public static method Main(): int "
        "    _ := C.Mk(); "
        "    return R._n; "          // 1 — disposed exactly once
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 1);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// §17.13 / §28.8.1 (#1009): `_ := move x` discharges the owned local `x` by
// disposing the moved value at statement end. `x` is NOT double-freed at scope
// end (the move leaves it moved-from). Disposed exactly once; ASan clean.
BEZEL_TEST(undisposed_leak, discard_move_local_disposed_once_1009) {
    const char* src =
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  var x := new R(); "
        "  _ := move x; "
        "  return R._n; "            // 1 — disposed once, x not double-freed
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 1);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// §17.13 (#1009) SOUNDNESS: discarding a BORROW (`_ := x` where x is an owned
// local) does NOT dispose at the discard — `x` is the sole owner and disposes
// the value exactly once at its own scope end. No double-free. The ownership
// pass classifies the alias read as Borrowed, so the discard is a no-op read.
BEZEL_TEST(undisposed_leak, discard_owned_local_alias_not_double_freed_1009) {
    const char* src =
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var x := new R(); "
        "  _ := x; "                 // borrow read+discard — NOT disposed here
        "  return 0; "
        "end Main; end C;";
    // detectLeaks=true: x is disposed exactly once at scope end → no leak; the
    // borrow discard adds no second disposal → no double-free.
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// #724: a TRIVIAL-dispose class (no destructor) and a primitive are still
// discardable — no over-rejection (§17.13).
BEZEL_TEST(undisposed_leak, discard_trivial_class_and_int_allowed_724) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Trivial public constructor() end constructor; end Trivial; "
        "class C public static method Main(): int "
        "  _ := new Trivial(); "
        "  _ := 42; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §17.13 (#1009): a discard of a MOVE-ONLY owning inline array stays a reject —
// end-of-statement disposal of its owned slots is a deferred follow-up (its
// slot-disposal codegen differs from the class/interface vtable path).
BEZEL_TEST(undisposed_leak, discard_move_only_array_still_rejected_1009) {
    auto r = runJit(
        "module M; "
        "class Cell public constructor() end constructor; "
        "  destructor end destructor; end Cell; "
        "class C public static method Main(): int "
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  _ := a; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
