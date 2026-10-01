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
#include "support/emit_object_support.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "support/unique_stem.hpp"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& program) {
    auto r = runAot(program);
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

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_383_lsan");
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
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
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

// (Removed in v0.196, #1815.) `kBorrowCallNotDisposed` compiled a static
// method returning a static-field read (`public static method Get(): R =>
// _shared;`) and observed at runtime that the `using var`-bound borrow was not
// disposed. v0.196 retires the static-method static-storage re-exposure arm
// (§28.3 ~L5898 / §28.6 ~L6085): that program is now rejected by the ownership
// pass before codegen, so a runtime observation is no longer reachable. The
// reject is pinned in tests/ownership/sourceless_static_factory_1499_test.cpp
// (`static_owned_field_reexposed_by_method_rejected` and the flipped
// `borrow_field_return_stays_borrowed`). Re-exposure is now the static property
// getter only; the paired `borrow_return_call_using_var_not_disposed_28_10`
// test below is removed with it.

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
    "class AppError: Exception "
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
    "module M; class E: Exception end E; "
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
//   D — bare re-raise `throw;` (v0.182, #1569 — the direct `throw ex;` of the
//       catch binding is now a §31.1 compile error, so the re-raise is the bare
//       `throw;`): the same exception object re-enters the unwind path. It must
//       NOT be freed by the inner handler — the outer handler catches and frees
//       it exactly once. Res._n == 1, no double-free. (§31.1 / §31.2.4)
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
    "class Inner: Exception "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class Wrap: Exception end Wrap; "
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
    "class Inner: Exception "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class Wrap: Exception end Wrap; "
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
    "class Inner: Exception "
    "  public Code: int := 7; "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class WrapCode: Exception "
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

// Case D — bare re-raise `throw;` of the caught instance (§31.1 / §31.2.4).
// The inner arm re-raises the SAME caught Inner via the bare re-throw. (v0.182,
// #1569: the direct-identifier form `throw e;` re-raising a caught binding is
// now a compile error — the borrowed operand is rejected; the bare `throw;` is
// the runtime-equivalent re-raise and the correct source form.) The inner
// handler must NOT free Inner — the same object re-enters the unwind path and is
// caught+freed by the outer handler exactly once. The outer arm falls through
// (body = `()`), triggering the caughtException disposal at arm-end.
// `return Res._n` is read AFTER the outer arm completes so it sees 1 (Inner
// freed exactly once). No double-free.
const char* kDirectExplicitRethrow_FreeOnce =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; "
    "class Inner: Exception "
    "  private owned _r: Res; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Inner; "
    "class R public static method Main(): int "
    "  try "
    "    try throw new Inner(); "
    "    catch Inner => throw; end; "
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

// (Removed in v0.196, #1815: `borrow_return_call_using_var_not_disposed_28_10`.)
// Its program returned a static-field read from a static method — a
// static-storage re-exposure the v0.196 retirement now rejects before codegen
// (§28.3 ~L5898 / §28.6 ~L6085). See the breadcrumb at the former
// `kBorrowCallNotDisposed` definition above; the reject is pinned in
// tests/ownership/sourceless_static_factory_1499_test.cpp.

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
    // §31.1 / §31.2.4 / #397 (v0.182, #1569: the caught-binding re-throw is the
    // bare `throw;` — `throw ex;` is now a compile error): the bare re-raise
    // from a catch arm re-circulates the SAME caught exception object. The #397
    // relaxation must NOT free the caught object on this re-raise — the object
    // re-enters the unwind path and is caught+freed by the outer handler once.
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
        "  _ := x; "
        "  return R._n; "            // 1 — disposed once, x not double-freed
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 1);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// §17.13 / §28.3 (#1009, UPDATED for v0.184 / #1651) SOUNDNESS: `_ := x` where
// `x` is a `using var` — a named owned binding, per the v0.184 "Who may call
// Dispose()" list (spec:6036, which names `using var` alongside a plain owned
// local/parameter as an eligible discard/dispose receiver) — is now an
// ANNOUNCED CONSUME: `x` is disposed at the discard statement, not at its own
// `using var` scope end. Before v0.184 the discard of a `using var` alias was
// a no-op borrow read (this test's original premise); #1651 fixed the
// ownership pass to treat ANY tracked-Owned named binding — not only a plain
// `out owned var` local — as an announced consume. No double-free either way:
// the discard's explicit-dispose codegen path (explicitDisposeOwnedReceiver-
// Slot) nulls `x`'s slot, so the `using var`'s own (null-guarded) scope-end
// disposal then finds null and is a no-op — exactly the same null-guard
// interplay an early explicit `x.Dispose()` on a `using var` already relied
// on. This test's ASSERTION (no leak / no double-free / no UAF) is unaffected
// by the timing change; only the disposal's TIMING moved earlier. R has no
// instance-level refcounted field, so its disposer-path reclaim is exact. See
// tests/{ownership,codegen}/discard_named_owned_binding_1651*_test.cpp for the
// dedicated #1651 regression coverage.
BEZEL_TEST(undisposed_leak, discard_owned_local_alias_not_double_freed_1009) {
    const char* src =
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var x := new R(); "
        "  _ := x; "                 // v0.184: announced consume — disposed HERE
        "  return 0; "
        "end Main; end C;";
    // detectLeaks=true: x is disposed exactly once (at the discard, v0.184) —
    // no leak; the `using var`'s own scope-end disposal is null-guarded and
    // skipped — no double-free.
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

// §17.13 / §15.3 (#1016): a discard `_ := a` of a move-only owning inline array
// is now LEGAL — it disposes each slot at end-of-statement (previously rejected
// as deferred; #1016 implements the slot-disposal codegen path). The dtor
// counter Cell.n increments once per disposed Cell, so returning Cell.n after
// the discard yields 2 — both Cells disposed exactly once. The test was renamed
// from the #1009 form that asserted hasErrors() (the deferred reject); that
// assertion is now inverted to confirm acceptance and correct disposal.
// per §17.13 bullet 2, §15.3, #1016
BEZEL_TEST(undisposed_leak, discard_move_only_array_disposed_slots_1016) {
    // Formerly discard_move_only_array_still_rejected_1009: was BEZEL_EXPECT_TRUE
    // (r.diagnostics.hasErrors()). Now accepted — slots disposed at statement end.
    const char* src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Cell.n := Cell.n + 1; end destructor; end Cell; "
        "class C public static method Main(): int "
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  _ := a; "
        "  return Cell.n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 2);   // both Cells disposed before return
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);  // clean: no leak, no double-free
}

// §17.13 / §15.3 (#1016): non-nullable `[N owned]T` discard also disposes all
// slots. Three Cells created; `_ := a` disposes all three at statement end.
// Cell.n = 3 at return. ASan-clean (no leak, no double-free). per §17.13, #1016
BEZEL_TEST(undisposed_leak, discard_nonnullable_inline_owned_all_slots_1016) {
    const char* src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Cell.n := Cell.n + 1; end destructor; end Cell; "
        "class C public static method Main(): int "
        "  var a: [3 owned]Cell := [new Cell(), new Cell(), new Cell()]; "
        "  _ := a; "
        "  return Cell.n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 3);   // all 3 slots disposed before return
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// §17.13 / §15.3 (#1016) SOUNDNESS: the moved-from array `a` is NOT disposed
// again at scope end (no double-free). After `_ := a`, `a` is consumed — the
// MoveOnlyArrayTransferWalker marks it as transferred, so
// registerInlineOwnedArrayDisposal skips the scope-end path. Cell.n = 2, not 4.
// ASan confirms no double-free. per §17.13, §15.3, #1016
BEZEL_TEST(undisposed_leak, discard_inline_owned_no_double_free_1016) {
    const char* src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Cell.n := Cell.n + 1; end destructor; end Cell; "
        "class C public static method Main(): int "
        "  var a: [2 owned]Cell? := [new Cell(), new Cell()]; "
        "  _ := a; "
        "  return Cell.n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 2);    // 2, not 4 — no double-dispose
    int lsan = runUnderLeakSanitizer(src, /*detectLeaks=*/false);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);  // ASan: no double-free / UAF
}

// §17.13 / §15.3 (#1016): when a slot has already been moved out before the
// discard, only the remaining non-null slots are disposed. Three Cells created;
// `move a[1]` takes one out (bound to `using var c`), `_ := a` disposes a[0]
// and a[2] at statement end — 2 disposals by the discard. The return statement
// captures Cell.n = 2 (pre-scope-exit); `c` is disposed at scope end (after
// return captures the value). All 3 objects eventually freed — no leak.
// per §17.13, §15.3, #1016
BEZEL_TEST(undisposed_leak, discard_inline_owned_partial_null_slots_1016) {
    const char* src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Cell.n := Cell.n + 1; end destructor; end Cell; "
        "class C public static method Main(): int "
        "  var a: [3 owned]Cell? := [new Cell(), new Cell(), new Cell()]; "
        "  using var c := move a[1]; "
        "  _ := a; "
        "  return Cell.n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 2);  // a[0]+a[2] disposed by discard; c after return
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);  // no leak (all 3 cells freed total)
}

// §25 / §28.3 (#965): a user-defined BINARY operator overload declared with an
// `owned` return produces a FRESH owned value. Bound to a plain `var` (whether
// or not the binding is ever read), that operator result is an owned producer
// that
// auto-drops at scope end (§28.3 affine auto-drop) — disposed exactly once. Before
// the producerState fix the operator result classified Borrowed → no auto-drop →
// the result leaked. Cell.n returns to 0; ASan confirms no leak / no double-free.
namespace {
const char* kOpCell =
    "class Cell public static n: int := 0; public V: int; "
    "  public constructor(v: int) Cell.n:=Cell.n+1; V:=v; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => "
    "    new Cell(a.V + b.V); "
    "end Cell; ";
}  // namespace

// `var d := a + b` with `d` never read — the #965 part-2 repro. The owned
// operator result is auto-dropped (discharged) at scope end; Cell.n returns
// to 0. (Spelled `var _ := a + b` until v0.244: `_` is now the reserved
// discard token and is never declarable, §1.3/§17.13, so the never-read
// binding gets an ordinary name. The spelling matters — the discard
// `_ := a + b;` would dispose at end of STATEMENT as an unbound owned
// temporary, which is the sibling of this path, not this path.)
BEZEL_TEST(undisposed_leak, operator_owned_result_discard_auto_dropped_965) {
    std::string src = std::string("module M; ") + kOpCell +
        "class C public static method Main(): int "
        "  do using var a := new Cell(1); using var b := new Cell(2); "
        "     var d := a + b; end; "   // owned operator result auto-dropped
        "  return Cell.n; "                 // 0 — disposed exactly once
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 0);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// A plain `var r := a + b` that is USED then dropped also auto-drops the owned
// operator result — and the borrowed operands `a`/`b` are disposed once by their
// own `using var` (no double-free).
BEZEL_TEST(undisposed_leak, operator_owned_result_plain_var_auto_dropped_965) {
    std::string src = std::string("module M; ") + kOpCell +
        "class C public static method Main(): int "
        "  do using var a := new Cell(1); using var b := new Cell(2); "
        "     var r := a + b; var z := r.V; end; "
        "  return Cell.n; "                 // 0
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 0);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// Chained `var r := a + b + c`: the intermediate `(a+b)` is disposed by the
// #912 chained-operand path and the final result auto-drops — each disposed
// exactly once, no double-free.
BEZEL_TEST(undisposed_leak, operator_owned_chained_result_auto_dropped_965) {
    std::string src = std::string("module M; ") + kOpCell +
        "class C public static method Main(): int "
        "  do using var a := new Cell(1); using var b := new Cell(2); "
        "     using var c := new Cell(3); var r := a + b + c; var z := r.V; end; "
        "  return Cell.n; "                 // 0
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 0);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// §28.3 (#1019): a fresh owned CLASS temporary passed to a BORROW parameter is
// borrowed for the call duration and must be disposed at scope end (normal AND
// unwind). Before the fix the #993 arg-temp discharge erased the disposal
// assuming the callee took ownership — true only for an `owned` parameter; a
// borrow parameter does not, so the temp leaked on the normal path.
namespace {
const char* kArgCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; "
    "  public static method operator +(a: Cell, b: Cell): owned Cell => new Cell(); "
    "  public static method Make(): owned Cell => new Cell(); "
    "end Cell; "
    "class P "
    "  public static method Use(c: Cell): int return 0; end Use; "       // BORROW
    "  public static method Take(owned c: Cell): int c.Dispose(); return 0; end Take; "
    "end P; ";
}  // namespace

// `P.Use(new Cell())` — fresh `new` temp to a borrow param: disposed once.
BEZEL_TEST(undisposed_leak, fresh_new_arg_to_borrow_param_disposed_1019) {
    std::string src = std::string("module M; ") + kArgCell +
        "class C public static method Main(): int "
        "  do var k := P.Use(new Cell()); end; return Cell.n; "  // 0
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 0);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// `P.Use(a + b)` (owned operator result) and `P.Use(Cell.Make())` (owned call)
// as borrow args are likewise disposed once.
BEZEL_TEST(undisposed_leak, fresh_operator_and_call_args_to_borrow_param_disposed_1019) {
    std::string src = std::string("module M; ") + kArgCell +
        "class C public static method Main(): int "
        "  do using var a := new Cell(); using var b := new Cell(); "
        "     var k := P.Use(a + b); var m := P.Use(Cell.Make()); end; "
        "  return Cell.n; "                                       // 0
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(src), 0);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// SOUNDNESS controls — no double-free:
//   * an `owned` parameter takes ownership (callee disposes); the caller does NOT
//     also dispose the temp.
//   * a BOUND-local borrow argument (`P.Use(x)`) is not a fresh producer, so it is
//     never disposed by the arg path — its own `using var` disposes it once.
BEZEL_TEST(undisposed_leak, owned_param_and_bound_local_args_no_double_free_1019) {
    std::string srcOwned = std::string("module M; ") + kArgCell +
        "class C public static method Main(): int "
        "  do var k := P.Take(new Cell()); end; return Cell.n; "  // 0 — callee disposes
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(srcOwned), 0);
    int l1 = runUnderLeakSanitizer(srcOwned);
    if (l1 >= 0) BEZEL_EXPECT_EQ(l1, 0);

    std::string srcBound = std::string("module M; ") + kArgCell +
        "class C public static method Main(): int "
        "  do using var x := new Cell(); var k := P.Use(x); end; return Cell.n; "  // 0
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runOwn(srcBound), 0);
    int l2 = runUnderLeakSanitizer(srcBound);
    if (l2 >= 0) BEZEL_EXPECT_EQ(l2, 0);
}
