// Codegen — exception-path owned-local/parameter disposal (§28.3) and
// compiler-generated Dispose idempotency (§28.2 / §28.4 / §28.6). Four
// findings:
//
//   [21] LEAK: a plain `var` owned local that is live when an exception unwinds
//        out of its block was NOT auto-disposed. §28.3 (line 4910): "Owned
//        locals and owned parameters that are live when an exception propagates
//        out of their enclosing block are disposed implicitly during stack
//        unwinding." The fix registers plain owned locals for unwind disposal.
//
//   [22] LEAK: an `owned` PARAMETER live when an exception propagates out of its
//        method was NOT auto-disposed. Same §28.3 rule, parameter manifestation.
//
//   [02] DOUBLE-FREE: the generated Dispose was not idempotent. An explicit
//        `.Dispose()` followed by the scope-end auto-dispose of the same `using
//        var` re-ran the destructor and double-freed the block (SIGSEGV). §28.2
//        (line 4758): "Dispose() on any class instance is idempotent: the first
//        call performs the disposal; subsequent calls are no-ops." §28.6 (line
//        5624): "The runtime tracks completion at the granularity of the
//        object."
//
//   [03] DOUBLE-FREE: a destructor body that explicitly disposes an owned field,
//        plus the §28.4-step-2 auto-dispose of that same field, disposed it
//        twice (SIGSEGV). §28.5 (line 5066) endorses the explicit-
//        `field.Dispose()` idiom precisely because Dispose is idempotent (§28.2).
//
// Two complementary kinds of check, mirroring free_on_dispose_test (#380):
//
//   1. Destructor-count tests (JIT, always run): the destructor bumps a static
//      counter; the exit code observes how many times it ran. For [21]/[22] the
//      count proves the owned value's destructor ran on the unwind path
//      (count==1; the leak left it 0). For [02]/[03] the count proves the
//      destructor ran EXACTLY once across the two dispose sites (count==1; the
//      pre-fix double-run made it 2, or crashed before reading it).
//
//   2. LeakSanitizer legs (native, only when an ASan toolchain is present): for
//      the no-throw idempotency programs ([02]/[03]) we additionally assert the
//      program is ASan/LSan-clean — no double-free, no use-after-free, and no
//      leak. The throwing leak programs ([21]/[22]) deliberately skip the LSan
//      leg: the thrown exception object has its own pre-existing lifecycle
//      (exception objects are calloc'd and not reclaimed — see #380's
//      free_on_dispose_test note), so LSan would report that unrelated leak and
//      mask the (now-correct) owned-value reclamation. Their destructor-count
//      leg is the load-bearing assertion.

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
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

namespace fs = std::filesystem;

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 if compilation/link/run failed. Mirrors
// free_on_dispose_test.cpp.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_excdisp_" + std::to_string(counter++);
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

// LeakSanitizer verification for one program. Lowers to an object file, links
// against an ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP.
// Identical mechanism to free_on_dispose_test.cpp.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_excdisp_lsan_" + std::to_string(counter++);
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the destructor-count exit code AND (if the toolchain is present) assert
// LeakSanitizer reports no leak / no double-free / no use-after-free.
void expectExitAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;          // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);        // 0 == clean (no leak, no double-free/UAF)
}

// --------------------------------------------------------------------------
// [21] §28.3 — plain `var` owned local disposed on the exception-unwind path.
// --------------------------------------------------------------------------

// The plain `var r := new R()` is live when the throw unwinds out of its block.
// Per §28.3 it is disposed implicitly during unwinding (count == 1). The leak
// left _n at 0.
const char* kPlainLocalUnwind =
    "module M; class AppError end AppError; "
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    try "
    "      do "
    "        var r := new R(); "
    "        throw new AppError(); "
    "      end; "
    "    catch AppError => return _n; end; "
    "    return -1; "
    "  end Main; "
    "end R;";

BEZEL_TEST(codegen_excdisp_21, plain_owned_local_disposed_on_unwind) {
    BEZEL_EXPECT_EQ(run(kPlainLocalUnwind), 1);
}

// A plain owned local + a using var both live at the throw: both disposed on the
// unwind (count == 2). Confirms the plain var joins the existing using-var
// unwind disposal rather than displacing it.
const char* kMixedPlainAndUsing =
    "module M; class AppError end AppError; "
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    try "
    "      do "
    "        using var u := new R(); "
    "        var p := new R(); "
    "        throw new AppError(); "
    "      end; "
    "    catch AppError => return _n; end; "
    "    return -1; "
    "  end Main; "
    "end R;";

BEZEL_TEST(codegen_excdisp_21, plain_and_using_both_disposed_on_unwind) {
    BEZEL_EXPECT_EQ(run(kMixedPlainAndUsing), 2);
}

// --------------------------------------------------------------------------
// [22] §28.3 — owned PARAMETER disposed when an exception propagates out.
// --------------------------------------------------------------------------

// Consume takes `owned x`; the caller's `new R()` is moved into the parameter
// (discharging the caller). Consume throws before discharging x, so x is live
// at the unwind out of Consume and is auto-disposed (count == 1).
const char* kOwnedParamUnwind =
    "module M; class AppError end AppError; "
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Consume(owned x: R) "
    "    throw new AppError(); "
    "  end Consume; "
    "  public static method Main(): int "
    "    try "
    "      Consume(new R()); "
    "    catch AppError => return _n; end; "
    "    return -1; "
    "  end Main; "
    "end R;";

BEZEL_TEST(codegen_excdisp_22, owned_param_disposed_on_unwind) {
    BEZEL_EXPECT_EQ(run(kOwnedParamUnwind), 1);
}

// --------------------------------------------------------------------------
// [02] §28.2 / §28.6 — explicit Dispose() then scope-end auto-dispose: the
// destructor runs exactly once; the block is freed exactly once.
// --------------------------------------------------------------------------

// `using var r := new Res(); r.Dispose();` then scope-end. The explicit Dispose
// performs the disposal (destructor: _n == 1); the scope-end auto-dispose is a
// no-op (§28.2). Pre-fix: re-ran the destructor and double-freed (SIGSEGV).
const char* kExplicitThenScopeEnd =
    "module M; "
    "class Res "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public constructor() end constructor; "
    "end Res; "
    "class R "
    "  public static method Main(): int "
    "    do "
    "      using var r := new Res(); "
    "      r.Dispose(); "
    "    end; "
    "    return Res._n; "
    "  end Main; "
    "end R;";

BEZEL_TEST(codegen_excdisp_02, explicit_dispose_then_scope_end_runs_dtor_once) {
    BEZEL_EXPECT_EQ(run(kExplicitThenScopeEnd), 1);
}

BEZEL_TEST(codegen_excdisp_02, explicit_dispose_then_scope_end_no_double_free) {
    // Destructor once (exit 1) AND ASan/LSan-clean: no double-free, no UAF, and
    // the block is reclaimed exactly once (no leak). No exception is thrown, so
    // the LSan leg is meaningful (nothing else leaks).
    expectExitAndLeakFree(kExplicitThenScopeEnd, 1);
}

// --------------------------------------------------------------------------
// [03] §28.4 / §28.5 — destructor body omits explicit dispose; §28.4-step-2
// auto-disposes the owned field after the destructor body runs: the field's
// destructor fires exactly once; the block is freed exactly once.
// (The old pattern `destructor _p.Dispose(); end destructor;` was a double-free
// and is now a sema error per §28.5 / #1230; the correct pattern is to omit the
// explicit call and rely on the auto-cleanup.)
// --------------------------------------------------------------------------

// MyError's destructor body is empty; §28.4 auto-disposes `_p` after the body,
// running Payload's destructor exactly once → Payload._n == 1 at scope end.
const char* kDtorDisposesFieldCount =
    "module M; "
    "class Payload "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public constructor() end constructor; "
    "end Payload; "
    "class MyError "
    "  public owned _p: Payload; "
    "  public constructor(owned p: Payload) _p := p; end constructor; "
    "  destructor end destructor; "
    "end MyError; "
    "class R "
    "  public static method Main(): int "
    "    do using var e := new MyError(new Payload()); end; "
    "    return Payload._n; "
    "  end Main; "
    "end R;";

BEZEL_TEST(codegen_excdisp_03, dtor_field_dispose_plus_autodispose_runs_once) {
    BEZEL_EXPECT_EQ(run(kDtorDisposesFieldCount), 1);
}

// §28.5 auto-cleanup minimal repro: `using var e := new MyError(new Payload());
// return 0;` — destructor body is empty, auto-dispose handles `_p`.
// Exit 0 AND ASan/LSan-clean: no double-free / UAF.
const char* kDtorDisposesFieldRepro =
    "module M; "
    "class Payload "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public constructor() end constructor; "
    "end Payload; "
    "class MyError "
    "  public owned _p: Payload; "
    "  public constructor(owned p: Payload) _p := p; end constructor; "
    "  destructor end destructor; "
    "end MyError; "
    "class R "
    "  public static method Main(): int "
    "    using var e := new MyError(new Payload()); "
    "    return 0; "
    "  end Main; "
    "end R;";

BEZEL_TEST(codegen_excdisp_03, dtor_field_dispose_plus_autodispose_no_double_free) {
    // Exit 0 AND ASan/LSan-clean: Payload disposed exactly once via §28.5
    // auto-cleanup, both blocks (MyError and Payload) reclaimed exactly once.
    expectExitAndLeakFree(kDtorDisposesFieldRepro, 0);
}

// --------------------------------------------------------------------------
// [04] §22.9 / §28.3 — a constructor body that throws AFTER assigning an owned
// field: the in-flight `new T(...)` instance and the assigned owned field are
// disposed on the unwind (the field's destructor runs, both heap blocks are
// reclaimed). Pre-fix: the partially-constructed instance and its owned field
// leaked (Res destructor count == 0; ASan: 24B instance + 16B field).
// --------------------------------------------------------------------------

// §22.9 step-3 bullet: "An exception in the constructor body (step 3) triggers
// disposal of every owned field assigned during initialization ... Owned fields
// assigned imperatively in the body are also disposed if assigned successfully
// before the throwing statement." Holder's ctor assigns `_r := new Res()` then
// throws; on the unwind out of `new Holder()` the assigned `_r` is disposed
// (Res destructor: count == 1) and the Holder instance block is reclaimed.
const char* kCtorThrowAfterAssignCount =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Boom end Boom; "
    "class Holder "
    "  private owned _r: Res; "
    "  public constructor() "
    "    _r := new Res(); "
    "    throw new Boom(); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var h := new Holder(); "
    "    catch Boom => Console.WriteLine(\"caught\"); end; "
    "    return Res.count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_04, ctor_throw_after_assign_disposes_owned_field) {
    // Res destructor runs exactly once for the assigned owned field on unwind.
    BEZEL_EXPECT_EQ(run(kCtorThrowAfterAssignCount), 1);
}

BEZEL_TEST(codegen_excdisp_04, ctor_throw_after_assign_leak_free) {
    // ASan/LSan-clean: the assigned owned field (16B) and the partially-built
    // instance (24B) are both reclaimed exactly once; the caught Boom is freed
    // when the handler completes. No double-free on the assigned field.
    int lsan = runUnderLeakSanitizer(kCtorThrowAfterAssignCount);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §22.9: a ctor that throws BEFORE assigning any owned field still reclaims the
// partially-constructed instance (no owned-field disposal — the field is null).
// Pre-fix: the 24-byte instance leaked. The owned field is declared NULLABLE
// (`Res?`) so the program satisfies §7.9 definite-assignment with no body
// assignment — the throw-before-assign scenario the finding describes — and the
// full sema pipeline (DA + ownership) used by the ASan leg accepts it.
const char* kCtorThrowBeforeAssign =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Boom end Boom; "
    "class Holder "
    "  private owned _r: Res?; "
    "  public constructor() "
    "    throw new Boom(); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var h := new Holder(); "
    "    catch Boom => Console.WriteLine(\"caught\"); end; "
    "    return Res.count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_04, ctor_throw_before_assign_no_field_dispose) {
    // No owned field was assigned → no disposal (the null `_r` is skipped).
    BEZEL_EXPECT_EQ(run(kCtorThrowBeforeAssign), 0);
}

BEZEL_TEST(codegen_excdisp_04, ctor_throw_before_assign_instance_reclaimed) {
    // ASan/LSan-clean: the partially-built instance (24B) is reclaimed; the
    // null owned field is correctly skipped (no spurious dispatch / free).
    int lsan = runUnderLeakSanitizer(kCtorThrowBeforeAssign);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §22.9: a ctor that assigns TWO owned fields then throws — both are disposed,
// in reverse declaration order, exactly once. The counter (2) proves both
// destructors ran; ASan proves no double-free.
const char* kCtorThrowAfterTwoFields =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Boom end Boom; "
    "class Holder "
    "  private owned _a: Res; "
    "  private owned _b: Res; "
    "  public constructor() "
    "    _a := new Res(); "
    "    _b := new Res(); "
    "    throw new Boom(); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var h := new Holder(); "
    "    catch Boom => Console.WriteLine(\"caught\"); end; "
    "    return Res.count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_04, ctor_throw_after_two_fields_both_disposed) {
    // Both assigned owned fields disposed (count == 2), each exactly once.
    BEZEL_EXPECT_EQ(run(kCtorThrowAfterTwoFields), 2);
}

BEZEL_TEST(codegen_excdisp_04, ctor_throw_after_two_fields_leak_free) {
    // ASan/LSan-clean: both owned fields and the instance reclaimed, no
    // double-free (reverse-order disposal, each null-guarded slot once).
    int lsan = runUnderLeakSanitizer(kCtorThrowAfterTwoFields);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// CONTROL (§28.4): a ctor that COMPLETES normally → the instance + its owned
// field are disposed exactly once at scope end, with NO double-free introduced
// by the new unwind-cleanup path (it must fire ONLY on the throw edge). The
// owned field's destructor runs once when the `using var` is disposed.
const char* kCtorCompletesControl =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Holder "
    "  private owned _r: Res; "
    "  public constructor() "
    "    _r := new Res(); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var h := new Holder(); "
    "    end; "
    "    return Res.count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_04, ctor_completes_normally_field_disposed_once) {
    // Normal completion: the owned field's destructor runs exactly once at
    // scope end (count == 1) — the new throw-path cleanup did NOT also fire.
    BEZEL_EXPECT_EQ(run(kCtorCompletesControl), 1);
}

BEZEL_TEST(codegen_excdisp_04, ctor_completes_normally_no_double_free) {
    // Exit 1 AND ASan/LSan-clean: no exception is thrown, so the unwind-cleanup
    // path must NOT fire — the field + instance are disposed exactly once at
    // scope end (no double-free, the cardinal risk of this fix).
    expectExitAndLeakFree(kCtorCompletesControl, 1);
}

// §22.9: a throwing ctor whose `new` result is DROPPED (not bound to a `using
// var`) is still cleaned up — the unwind cleanup lives in the ctor / call site,
// independent of how the caller would have bound the result.
const char* kCtorThrowResultDropped =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Boom end Boom; "
    "class Holder "
    "  private owned _r: Res; "
    "  public constructor() "
    "    _r := new Res(); "
    "    throw new Boom(); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class C "
    "  public static method Use() "
    // §21.9 (#743): a bare `new Holder();` discarding the owned (non-trivial-
    // dispose) result is now a hard error; the spec-prescribed discard form is
    // `using var _ := expression;`. On a ctor throw the bind never completes, so
    // this exercises the SAME ctor-unwind cleanup of the partial object.
    "    using var _ := new Holder(); "
    "  end Use; "
    "  public static method Main(): int "
    "    try "
    "      Use(); "
    "    catch Boom => Console.WriteLine(\"caught\"); end; "
    "    return Res.count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_04, ctor_throw_result_dropped_still_cleaned) {
    // The discarded `new Holder()` whose ctor throws still disposes its assigned
    // owned field (count == 1) on the unwind. (§21.9: the discard is written
    // `using var _ := …`; on a ctor throw the bind never completes.)
    BEZEL_EXPECT_EQ(run(kCtorThrowResultDropped), 1);
}

BEZEL_TEST(codegen_excdisp_04, ctor_throw_result_dropped_leak_free) {
    // ASan/LSan-clean even with the result dropped.
    int lsan = runUnderLeakSanitizer(kCtorThrowResultDropped);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §22.9 / §28.4 + §7.1: a ctor that assigns a NULLABLE owned reference field
// (`owned _r: Res?`) NON-null, then throws — the assigned non-null field is
// disposed on the unwind, exactly once. This composes the §22.9 ctor-unwind
// cleanup with the nullable-owned-field classification: a reference `T?` is a
// nullable pointer (§7.1), so `isOwnedReferenceField` classifies `Res?` by its
// inner class (the `nullable` flag rides on the same Named TypeRef, not a
// separate wrapper), and the unwind cleanup reuses `emitOwnedFieldDisposals`,
// whose load-then-null-guard disposes the non-null slot. The pre-existing
// `kCtorThrowBeforeAssign` test pins the complementary NULL case (a
// never-assigned `Res?` disposes nothing); this pins the non-null case the
// ctor-throw lane surfaced.
const char* kCtorThrowAfterNullableAssign =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Boom end Boom; "
    "class Holder "
    "  private owned _r: Res?; "
    "  public constructor() "
    "    _r := new Res(); "
    "    throw new Boom(); "
    "  end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var h := new Holder(); "
    "    catch Boom => Console.WriteLine(\"caught\"); end; "
    "    return Res.count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_04, ctor_throw_after_nullable_assign_field_disposed) {
    // The assigned non-null nullable owned field is disposed on the unwind
    // (count == 1) — the §22.9 cleanup classifies and disposes `Res?` too.
    BEZEL_EXPECT_EQ(run(kCtorThrowAfterNullableAssign), 1);
}

BEZEL_TEST(codegen_excdisp_04, ctor_throw_after_nullable_assign_leak_free) {
    // ASan/LSan-clean: the assigned nullable owned field and the partial
    // instance are reclaimed exactly once; no double-free on the field slot.
    int lsan = runUnderLeakSanitizer(kCtorThrowAfterNullableAssign);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// --------------------------------------------------------------------------
// [467] §28.3 — an outer `using var` is disposed on the exception path even
// when an inner `try`/`finally` sits in the same scope.
//
// §28.3 (line 4910): owned locals live when an exception propagates out of
// their block are auto-disposed during unwinding. (line 4914): when the
// exception passes through nested scopes, the inner scope's locals are disposed
// before the outer scope's. (line 4922): the implicit auto-dispose composes
// with an explicit `finally` — the explicit `finally` runs first, then the
// implicit auto-dispose runs as the scope unwinds further.
//
// BUG: an outer `using var r` declared in an outer `try` scope was NOT disposed
// on the exception path when an inner `try`/`finally` was present in the same
// scope. The inner `finally`'s unwind block re-propagated straight to the outer
// catch dispatch WITHOUT first disposing the scopes between the inner try and
// the outer handler (where `r` lives), so `r` leaked. Without the inner
// `finally`, disposal worked (the throw site disposed down to the outer try's
// depth). The fix disposes the intervening scopes [savedDepth, thisTryDepth) at
// the inner `finally`'s re-propagation, AFTER the finally body (§28.3 ordering).
// --------------------------------------------------------------------------

// [467-1] The exact repro: outer `using var r` + inner `try`/`finally` + throw +
// outer `catch`. `r` is disposed exactly once on the exception path (count == 1).
// Pre-fix: r leaked, count == 0.
const char* kOuterUsingInnerFinallyThrow =
    "module M; "
    "class R "
    "  public static _count: int := 0; "
    "  destructor _count := _count + 1; end destructor; "
    "end R; "
    "class E end E; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var r := new R(); "
    "      try "
    "        throw new E(); "
    "      finally "
    "        var dummy := 0; "
    "      end; "
    "    catch E => var dummy2 := 0; "
    "    end; "
    "    return R._count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_467, outer_using_inner_finally_throw_disposes_once) {
    BEZEL_EXPECT_EQ(run(kOuterUsingInnerFinallyThrow), 1);
}

BEZEL_TEST(codegen_excdisp_467, outer_using_inner_finally_throw_leak_free) {
    // ASan/LSan-clean: the exception is caught (its object is reclaimed by the
    // catch arm), and `r` is disposed/reclaimed exactly once — no leak, no
    // double-free, no use-after-free on the exception path.
    int lsan = runUnderLeakSanitizer(kOuterUsingInnerFinallyThrow);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// [467-2] §28.3 (line 4922) ORDER: the inner explicit `finally` runs FIRST, then
// the outer `using var`'s implicit auto-dispose. A shared decimal log records
// each step (finally appends 1, the destructor appends 2); the result is 12 iff
// the finally ran before the dispose. (21 would mean the dispose ran first.)
const char* kInnerFinallyBeforeOuterDispose =
    "module M; "
    "class Log "
    "  public static _seq: int := 0; "
    "end Log; "
    "class R "
    "  destructor Log._seq := Log._seq * 10 + 2; end destructor; "
    "end R; "
    "class E end E; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var r := new R(); "
    "      try "
    "        throw new E(); "
    "      finally "
    "        Log._seq := Log._seq * 10 + 1; "
    "      end; "
    "    catch E => var d := 0; "
    "    end; "
    "    return Log._seq; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_467, inner_finally_runs_before_outer_dispose) {
    BEZEL_EXPECT_EQ(run(kInnerFinallyBeforeOuterDispose), 12);
}

// [467-3] NO DOUBLE-DISPOSE on the NORMAL (no-throw) path with the same shape.
// The body falls through; `r` is disposed exactly once at the outer scope end
// (count == 1, not 2). Guards against the fix introducing a second disposal on
// the normal path.
const char* kOuterUsingInnerFinallyNoThrow =
    "module M; "
    "class R "
    "  public static _count: int := 0; "
    "  destructor _count := _count + 1; end destructor; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var r := new R(); "
    "      try "
    "        var x := 0; "
    "      finally "
    "        var dummy := 0; "
    "      end; "
    "    catch Exception => var dummy2 := 0; "
    "    end; "
    "    return R._count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_467, normal_path_inner_finally_disposes_once) {
    BEZEL_EXPECT_EQ(run(kOuterUsingInnerFinallyNoThrow), 1);
}

BEZEL_TEST(codegen_excdisp_467, normal_path_inner_finally_no_double_free) {
    // ASan/LSan-clean: no exception is thrown, so nothing leaks and the single
    // scope-end disposal of `r` reclaims it exactly once (no double-free).
    int lsan = runUnderLeakSanitizer(kOuterUsingInnerFinallyNoThrow);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// [467-4] NESTED: two `using var`s in the outer scope + inner `try`/`finally` +
// throw. Both are disposed exactly once, in REVERSE declaration order (§28.3
// line 4914). A is declared first, B second; reverse order disposes B (appends
// 2) before A (appends 1), so the log reads 21.
const char* kTwoOuterUsingInnerFinallyThrow =
    "module M; "
    "class Log "
    "  public static _seq: int := 0; "
    "end Log; "
    "class A "
    "  destructor Log._seq := Log._seq * 10 + 1; end destructor; "
    "end A; "
    "class B "
    "  destructor Log._seq := Log._seq * 10 + 2; end destructor; "
    "end B; "
    "class E end E; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var a := new A(); "
    "      using var b := new B(); "
    "      try "
    "        throw new E(); "
    "      finally "
    "        var d := 0; "
    "      end; "
    "    catch E => var d2 := 0; "
    "    end; "
    "    return Log._seq; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_467, two_outer_using_inner_finally_reverse_order) {
    BEZEL_EXPECT_EQ(run(kTwoOuterUsingInnerFinallyThrow), 21);
}

BEZEL_TEST(codegen_excdisp_467, two_outer_using_inner_finally_leak_free) {
    // ASan/LSan-clean: both `using var`s reclaimed exactly once on the exception
    // path; the caught exception object reclaimed by the catch arm. No leak, no
    // double-free.
    int lsan = runUnderLeakSanitizer(kTwoOuterUsingInnerFinallyThrow);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// [467-5] REGRESSION: the WITHOUT-inner-`finally` case still disposes `r` exactly
// once on the throw (count == 1). This path was correct pre-fix (the throw site
// disposed down to the outer try's depth) and must stay correct.
const char* kOuterUsingNoInnerFinallyThrow =
    "module M; "
    "class R "
    "  public static _count: int := 0; "
    "  destructor _count := _count + 1; end destructor; "
    "end R; "
    "class E end E; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var r := new R(); "
    "      throw new E(); "
    "    catch E => var dummy2 := 0; "
    "    end; "
    "    return R._count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_467, no_inner_finally_throw_still_disposes_once) {
    BEZEL_EXPECT_EQ(run(kOuterUsingNoInnerFinallyThrow), 1);
}

// [467-6] CROSS-METHOD propagation: the inner `try`/`finally` and outer
// `using var` live in a method (Work) that has NO enclosing handler — the
// exception propagates OUT of Work to Main's catch. Here the inner finally's
// re-propagation targets the function's propagate path (savedDepth == 0), and
// the intervening-scope discharge must still dispose `r` before Work returns
// with the exception in flight (count == 1). Same bug shape as [467-1] but the
// enclosing handler is a different stack frame.
const char* kCrossMethodUsingInnerFinallyThrow =
    "module M; "
    "class R "
    "  public static _count: int := 0; "
    "  destructor _count := _count + 1; end destructor; "
    "end R; "
    "class E end E; "
    "class C "
    "  public static method Work(): int "
    "    using var r := new R(); "
    "    try "
    "      throw new E(); "
    "    finally "
    "      var a := 0; "
    "    end; "
    "    return 0; "
    "  end Work; "
    "  public static method Main(): int "
    "    try "
    "      var z := Work(); "
    "    catch E => var d := 0; "
    "    end; "
    "    return R._count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_467, cross_method_inner_finally_throw_disposes_once) {
    BEZEL_EXPECT_EQ(run(kCrossMethodUsingInnerFinallyThrow), 1);
}

BEZEL_TEST(codegen_excdisp_467, cross_method_inner_finally_throw_leak_free) {
    // ASan/LSan-clean: `r` reclaimed exactly once as the exception leaves Work;
    // the exception object reclaimed by Main's catch arm. No leak, no double-free.
    int lsan = runUnderLeakSanitizer(kCrossMethodUsingInnerFinallyThrow);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// --------------------------------------------------------------------------
// [822] §28.3 — an owned local in an OUTER try body is disposed on the
// unwind path when an INNER try/catch (no finally) re-propagates an unmatched
// exception. The #467 intervening-scope discharge was only emitted for the
// try-WITH-finally re-propagation path; the pure try/catch (no finally)
// no-arm-matched path branched straight to the enclosing handler without it,
// leaking the outer `using var`.
// --------------------------------------------------------------------------

// Outer `using var r`; inner try throws E1 which the inner catch (E2) does not
// match → re-propagates to the outer catch. r must be disposed during the
// unwind, so R._n == 1 in the outer arm (was 0).
const char* kNestedTryNoFinallyReprop =
    "module M; "
    "class R public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; end R; "
    "class E1 end E1; "
    "class E2 end E2; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var r := new R(); "
    "      try "
    "        throw new E1(); "
    "      catch E2 => var d := 0; "
    "      end; "
    "    catch E1 => return R._n; "       // 1 (r disposed on re-propagation)
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_822, outer_using_var_disposed_on_nested_reprop) {
    BEZEL_EXPECT_EQ(run(kNestedTryNoFinallyReprop), 1);
}

BEZEL_TEST(codegen_excdisp_822, outer_using_var_nested_reprop_leak_free) {
    // ASan/LSan-clean: r reclaimed exactly once during the unwind to the outer
    // handler; no leak (the bug) and no double-free.
    int lsan = runUnderLeakSanitizer(kNestedTryNoFinallyReprop);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Two outer owned locals → both disposed (reverse order) on re-propagation: 2.
const char* kNestedTryTwoOuterLocals =
    "module M; "
    "class R public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; end R; "
    "class E1 end E1; "
    "class E2 end E2; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var r1 := new R(); "
    "      using var r2 := new R(); "
    "      try "
    "        throw new E1(); "
    "      catch E2 => var d := 0; "
    "      end; "
    "    catch E1 => return R._n; "       // 2
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_822, two_outer_using_vars_both_disposed) {
    BEZEL_EXPECT_EQ(run(kNestedTryTwoOuterLocals), 2);
}

// Regression guard: when the INNER catch MATCHES (no re-propagation), the outer
// `using var` is disposed exactly ONCE (not double-freed by the new range
// discharge). R._n == 1.
const char* kNestedTryInnerMatches =
    "module M; "
    "class R public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; end R; "
    "class E1 end E1; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var r := new R(); "
    "      try "
    "        throw new E1(); "
    "      catch E1 => var d := 0; "
    "      end; "
    "    catch E1 => return 99; "
    "    end; "
    "    return R._n; "                   // 1 (disposed once, no double-free)
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_822, inner_match_outer_local_disposed_once) {
    expectExitAndLeakFree(kNestedTryInnerMatches, 1);
}

// --------------------------------------------------------------------------
// [818] §28.8.1 / Guide 1 §6 — the owned-field move-and-transfer codegen.
// `move _field` transfers the field's object out; the field slot must be
// nulled so the owner's destructor (null-guarded owned-field disposal) skips it
// instead of disposing the already-transferred object again. Before the fix the
// move codegen nulled only LOCAL using-var slots, not fields, so the owner's
// destructor double-freed the moved-out object → SIGSEGV.
// After fix #825 (§28.8.1), a bare `move _field` without restoration is a
// compile error. These tests use the SWAP pattern — move-and-replace — which
// satisfies §28.8.1 (the field is restored before method exit) and exercises
// the same codegen property: when `move _v` transfers the field out, the slot
// must be nulled in the IR so the owner's destructor skips it. // per §28.8.1
// --------------------------------------------------------------------------

// Swap a Cell out of a Box: take the old Cell, replace with a fresh one, return
// the old. Both Cells get disposed exactly once (Cell.n returns to 0). The old
// Cell's disposal occurs via `using var taken` scope-end; the fresh Cell's
// disposal occurs via Box's destructor (which sees a non-null _v → disposes it
// once, not twice).
const char* kDetachOwnedFieldMove =
    "module M; "
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
    "class Box "
    "  private owned _v: Cell; "
    "  public constructor(owned x: Cell) _v := x; end constructor; "
    "  destructor end destructor; "
    "  public method Swap(owned fresh: Cell): owned Cell "
    "    var old := move _v; _v := fresh; return old; "
    "  end Swap; "
    "end Box; "
    "class C public static method Main(): int "
    "  do "
    "    var c1 := new Cell(); "
    "    using var b := new Box(move c1); "
    "    var c2 := new Cell(); "
    "    using var taken := b.Swap(move c2); "
    "  end; "
    "  return Cell.n; "                 // 0 — both disposed once, no double-free
    "end Main; end C;";

BEZEL_TEST(codegen_excdisp_818, detached_field_disposed_once_no_double_free) {
    expectExitAndLeakFree(kDetachOwnedFieldMove, 0);
}

// The Guide 1 §6 owned-field transfer idiom: swap the current Logfile for a
// fresh one, returning the old (id=5). Uses the move-and-restore swap pattern
// (§28.8.1 compliant). The returned value carries the original id (5).
const char* kDetachLogIdiom =
    "module M; "
    "class Logfile public id: int; "
    "  public constructor(i: int) id := i; end constructor; "
    "  destructor end destructor; end Logfile; "
    "class Logger "
    "  private owned _log: Logfile; "
    "  public constructor(owned l: Logfile) _log := l; end constructor; "
    "  destructor end destructor; "
    "  public method SwapLog(owned fresh: Logfile): owned Logfile "
    "    var old := move _log; _log := fresh; return old; "
    "  end SwapLog; "
    "end Logger; "
    "class C public static method Main(): int "
    "  using var lg := new Logger(new Logfile(5)); "
    "  using var d := lg.SwapLog(new Logfile(0)); "
    "  return d.id; "                   // 5
    "end Main; end C;";

BEZEL_TEST(codegen_excdisp_818, guide_detachlog_idiom_value) {
    BEZEL_EXPECT_EQ(run(kDetachLogIdiom), 5);
}

BEZEL_TEST(codegen_excdisp_818, guide_detachlog_idiom_leak_free) {
    int lsan = runUnderLeakSanitizer(kDetachLogIdiom);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// --------------------------------------------------------------------------
// [821] §29.7 — a `using var` inside an iterator body must be disposed both on
// NORMAL completion (the body runs off its end) and on EARLY abandonment (the
// consumer stops iterating and Disposes the enumerator while it is suspended at
// a yield). Before the fix the iterator's generated Dispose only re-ran explicit
// try/finally chains, and MoveNext's normal-completion path freed the frame
// without disposing active using-vars → leak.
// --------------------------------------------------------------------------

// Full iteration to completion: the using-var r is disposed once (R.count = 1).
const char* kIterUsingVarFullIteration =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C "
    "  public static method Gen(): IEnumerable<int> "
    "    using var r := new R(); "
    "    yield return 1; "
    "  end Gen; "
    "  public static method Main(): int "
    "    for var x in Gen() do end; "
    "    return R.count; "                 // 1
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_821, iterator_using_var_full_iteration_disposed) {
    expectExitAndLeakFree(kIterUsingVarFullIteration, 1);
}

// Early break: the consumer stops after the first yield; Dispose disposes the
// still-active using-var (R.count = 1).
const char* kIterUsingVarEarlyBreak =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C "
    "  public static method Gen(): IEnumerable<int> "
    "    using var r := new R(); "
    "    yield return 1; "
    "    yield return 2; "
    "  end Gen; "
    "  public static method Main(): int "
    "    for var x in Gen() do break; end; "
    "    return R.count; "                 // 1
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_821, iterator_using_var_early_break_disposed) {
    expectExitAndLeakFree(kIterUsingVarEarlyBreak, 1);
}

// Two using-vars in the iterator body: both disposed (R.count = 2).
const char* kIterTwoUsingVars =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C "
    "  public static method Gen(): IEnumerable<int> "
    "    using var a := new R(); "
    "    using var b := new R(); "
    "    yield return 1; "
    "  end Gen; "
    "  public static method Main(): int "
    "    for var x in Gen() do end; "
    "    return R.count; "                 // 2
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_821, iterator_two_using_vars_disposed) {
    expectExitAndLeakFree(kIterTwoUsingVars, 2);
}

// --------------------------------------------------------------------------
// [N] §29.7 / §30.7 — the ASYNC iterator sibling of [821]. The async iterator's
// generated Dispose only re-ran explicit try/finally chains (smIterFinallyAt-
// State_) and its MoveNextAsync normal-completion path freed the frame without
// disposing active using-vars → leak. The fix ports the smIterDisposalsAtState_
// handling (sync iterator's Dispose + normal-completion) into the async path.
// --------------------------------------------------------------------------

// Full async iteration to completion: the using-var r is disposed once
// (R.count = 1) on the normal-completion (run-off-the-end) path of the async
// iterator's MoveNextAsync.
const char* kAsyncIterUsingVarFullIteration =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C "
    "  public async static method Gen(): IAsyncEnumerable<int> "
    "    using var r := new R(); "
    "    yield return 1; "
    "  end Gen; "
    "  public static async method Main(): Task<int> "
    "    await for var x in Gen() do end; "
    "    return R.count; "                 // 1
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_821, async_iterator_using_var_full_iteration_disposed) {
    expectExitAndLeakFree(kAsyncIterUsingVarFullIteration, 1);
}

// Early break: the async consumer stops after the first yield; the async
// iterator's Dispose disposes the still-active using-var (R.count = 1) via the
// union of smIterFinallyAtState_ + smIterDisposalsAtState_.
const char* kAsyncIterUsingVarEarlyBreak =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C "
    "  public async static method Gen(): IAsyncEnumerable<int> "
    "    using var r := new R(); "
    "    yield return 1; "
    "    yield return 2; "
    "  end Gen; "
    "  public static async method Main(): Task<int> "
    "    await for var x in Gen() do break; end; "
    "    return R.count; "                 // 1
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_821, async_iterator_using_var_early_break_disposed) {
    expectExitAndLeakFree(kAsyncIterUsingVarEarlyBreak, 1);
}

// --------------------------------------------------------------------------
// [831] §4.5 / §4.7 / §4.9 — the TRY-pop family on owning collections removes
// the element WITHOUT disposing it (ownership transfers to the out param). The
// popped element must be disposed exactly once (by the caller, via the out
// param) and the elements left behind disposed at the collection's scope-end —
// no leak, no double-free. (The bug was that these calls were not lowered at
// all; this verifies the transfer is ownership-correct, not just that it runs.)
// --------------------------------------------------------------------------

// OwnedArray<R>: 2 elements; TryPopFirst transfers one out (disposed via
// `using var _`); the other disposed at scope-end. Each disposed once → 2.
const char* kArrTryPopFirstTransfer =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C public static method Main(): int "
    "  do "
    "    using var a := new OwnedArray<R>(); "
    "    a.Add(move (new R())); a.Add(move (new R())); "
    "    var got: R? := null; var ok := a.TryPopFirst(out got); "
    "    if ok then if got <> null then "
    "      do using var _ := got; end; end; end; "
    "  end; "
    "  return R.count; "                  // 2
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_831, owned_array_try_pop_first_transfer_disposed_once) {
    expectExitAndLeakFree(kArrTryPopFirstTransfer, 2);
}

// OwnedLinkedList<R>: same transfer + dispose-once invariant.
const char* kLListTryPopFirstTransfer =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C public static method Main(): int "
    "  do "
    "    using var l := new OwnedLinkedList<R>(); "
    "    l.AddLast(move (new R())); l.AddLast(move (new R())); "
    "    var got: R? := null; var ok := l.TryPopFirst(out got); "
    "    if ok then if got <> null then "
    "      do using var _ := got; end; end; end; "
    "  end; "
    "  return R.count; "                  // 2
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_831, owned_linkedlist_try_pop_first_transfer_disposed_once) {
    expectExitAndLeakFree(kLListTryPopFirstTransfer, 2);
}

// OwnedDictionary<string,R>: TryPopValue transfers one value out (disposed via
// `using var _`); the other entry disposed at scope-end. Each disposed once → 2.
const char* kDictTryPopValueTransfer =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C public static method Main(): int "
    "  do "
    "    using var d := new OwnedDictionary<string, R>(); "
    "    d.Add(\"a\", move (new R())); d.Add(\"b\", move (new R())); "
    "    var v: R? := null; var hit := d.TryPopValue(\"a\", out v); "
    "    if hit then if v <> null then "
    "      do using var _ := v; end; end; end; "
    "  end; "
    "  return R.count; "                  // 2
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_831, owned_dict_try_pop_value_transfer_disposed_once) {
    expectExitAndLeakFree(kDictTryPopValueTransfer, 2);
}

// TryPopFirst on an EMPTY OwnedArray<R> returns false and disposes nothing
// extra; the one element added afterwards (never popped) is disposed at
// scope-end exactly once → 1 (the false path must not corrupt ownership).
const char* kArrTryPopEmptyThenScopeEnd =
    "module M; "
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; "
    "class C public static method Main(): int "
    "  do "
    "    using var a := new OwnedArray<R>(); "
    "    var got: R? := null; var ok := a.TryPopFirst(out got); "
    "    a.Add(move (new R())); "
    "  end; "
    "  return R.count; "                  // 1
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_831, owned_array_try_pop_empty_false_no_corruption) {
    expectExitAndLeakFree(kArrTryPopEmptyThenScopeEnd, 1);
}

// --------------------------------------------------------------------------
// [837] §4.10 / §8.3 — OwnedSet<T> deduplicates a class element by the user's
// Equals; the rejected duplicate is disposed exactly once and the retained
// elements are disposed at scope-end. Verifies the dedup-dispose path is
// ownership-correct (no leak, no double-free), not merely that the count is
// right.
// --------------------------------------------------------------------------

// Add the same logical Box(1) twice + a distinct Box(2): the duplicate is
// disposed on the second Add, the two retained are disposed at scope-end → 3.
const char* kSetAddDupDisposed =
    "module M; "
    "class Box public static count: int := 0; public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor Box.count := Box.count + 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; end Equals; "
    "  public override method GetHashCode(): int => X; end Box; "
    "class C public static method Main(): int "
    "  do "
    "    using var s := new OwnedSet<Box>(); "
    "    s.Add(move (new Box(1))); s.Add(move (new Box(2))); "
    "    s.Add(move (new Box(1))); "
    "  end; "
    "  return Box.count; "                // 3
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_837, owned_set_add_duplicate_disposed_once) {
    expectExitAndLeakFree(kSetAddDupDisposed, 3);
}

// Same invariant via TryAdd: the rejected duplicate is disposed once.
const char* kSetTryAddDupDisposed =
    "module M; "
    "class Box public static count: int := 0; public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor Box.count := Box.count + 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; end Equals; "
    "  public override method GetHashCode(): int => X; end Box; "
    "class C public static method Main(): int "
    "  do "
    "    using var s := new OwnedSet<Box>(); "
    "    s.Add(move (new Box(1))); s.Add(move (new Box(2))); "
    "    var r := s.TryAdd(move (new Box(1))); "
    "  end; "
    "  return Box.count; "                // 3
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_837, owned_set_tryadd_duplicate_disposed_once) {
    expectExitAndLeakFree(kSetTryAddDupDisposed, 3);
}

// NOTE [838]: an OwnedArray<Subscription>'s scope-end unsubscribe is verified by
// the value tests in owned_array_subscription_838_test.cpp (the handler stops
// firing). An LSan assertion is intentionally NOT added here: a capturing
// subscription handler's closure ENVIRONMENT is not freed when the subscription
// is disposed — a pre-existing leak that also affects a plain
// `using var sub := <event>.Subscribe(closure)` (no array), tracked separately
// (orthogonal to the #838 element-disposal fix, which only ensures the array
// disposes its elements at all). The array's own bz_array_t backing IS freed
// (covered by the LSan-clean OwnedArray<Cell> tests #819/#820).

// --------------------------------------------------------------------------
// [865] §28.3 — an owned local with an UNREACHABLE or CONDITIONAL Dispose() is
// still auto-disposed on the exception-unwind path (the UnwindIneligibleWalker
// was flow-insensitive: it saw `r.Dispose()` anywhere and suppressed the unwind
// disposal, leaking `r` on the throw path). Explicit Dispose nulls the slot and
// the unwind disposal is null-guarded, so leaving the local eligible is safe.
// --------------------------------------------------------------------------

// An UNREACHABLE `r.Dispose()` (after a throw) must not suppress the unwind
// disposal: r is live when E propagates → disposed once (_n == 1), no leak.
BEZEL_TEST(exception_disposal_865, unreachable_dispose_still_unwind_disposed) {
    expectExitAndLeakFree(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class E end E; "
        "class C public static method Main(): int "
        "  try var r := new R(); throw new E(); r.Dispose(); "
        "  catch E => var d := 0; end; "
        "  return R._n; "  // 1
        "end Main; end C;", 1);
}

// A Dispose() guarded by a runtime-FALSE condition did not run → r is live on
// the throw → unwind-disposed once (_n == 1).
BEZEL_TEST(exception_disposal_865, conditional_false_dispose_unwind_disposed) {
    expectExitAndLeakFree(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class E end E; "
        "class C public static method Main(): int "
        "  var cond := false; "
        "  try var r := new R(); if cond then r.Dispose(); end; throw new E(); "
        "  catch E => var d := 0; end; "
        "  return R._n; "  // 1
        "end Main; end C;", 1);
}

// Double-free control: a Dispose() that DID run before the throw (runtime-true
// condition) must NOT be re-disposed on unwind — the nulled slot + null-guard
// keep it exactly once (_n == 1, not 2). LSan also asserts no double-free.
BEZEL_TEST(exception_disposal_865, conditional_true_dispose_no_double_free) {
    expectExitAndLeakFree(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class E end E; "
        "class C public static method Main(): int "
        "  var cond := true; "
        "  try var r := new R(); if cond then r.Dispose(); end; throw new E(); "
        "  catch E => var d := 0; end; "
        "  return R._n; "  // 1 — disposed once by the explicit Dispose
        "end Main; end C;", 1);
}

// --------------------------------------------------------------------------
// [900] §28.3 — an `out owned` recipient (the local populated by
// TryPop/TryDequeue/TryPop*/TryPopValue) is auto-disposed on the exception path.
// The recipient solely owns the moved-out element and has no scope-end
// auto-dispose, so a throw before its explicit discharge leaked it. The walker
// wrongly treated the `out owned` argument as a consuming use (so the local was
// marked unwind-ineligible); now an `out`/`out var`/`out owned` argument is an
// OUTPUT (non-consuming), and the recipient is registered for unwind disposal.
// --------------------------------------------------------------------------

// A throw after a successful TryPop, before the explicit `item?.Dispose()`, must
// auto-dispose the moved-out element (Cell._n back to 0).
const char* kCellCtorDtor =
    "class Cell public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; end Cell; ";

BEZEL_TEST(exception_disposal_900, out_owned_recipient_unwind_disposed) {
    expectExitAndLeakFree(
        std::string("module M; ") + kCellCtorDtor +
        "class C public static method Main(): int "
        "  do using var s := new OwnedStack<Cell>(); s.Push(new Cell()); "
        "    try do var item: Cell? := null; var ok := s.TryPop(out owned item); "
        "           throw new Exception(\"x\"); item?.Dispose(); end; "
        "    catch Exception => return Cell._n; end; "  // 0
        "    return -1; end; "
        "end Main; end C;", 0);
}

// Double-free control: an `out owned` recipient explicitly disposed BEFORE the
// throw must not be re-disposed on unwind (the nulled slot + null-guard) — and
// LSan asserts no double-free.
BEZEL_TEST(exception_disposal_900, out_owned_dispose_then_throw_no_double_free) {
    expectExitAndLeakFree(
        std::string("module M; ") + kCellCtorDtor + "class E end E; "
        "class C public static method Main(): int "
        "  do using var s := new OwnedStack<Cell>(); s.Push(new Cell()); "
        "    try do var item: Cell? := null; var ok := s.TryPop(out owned item); "
        "           item?.Dispose(); throw new E(); end; "
        "    catch E => return Cell._n; end; "  // 0 — disposed once
        "    return -1; end; "
        "end Main; end C;", 0);
}

// Control: an `out owned` recipient disposed on a NORMAL exit (no throw) — single
// dispose, no double-free.
BEZEL_TEST(exception_disposal_900, out_owned_normal_exit_disposed_once) {
    expectExitAndLeakFree(
        std::string("module M; ") + kCellCtorDtor +
        "class C public static method Main(): int "
        "  do using var s := new OwnedStack<Cell>(); s.Push(new Cell()); "
        "    do var item: Cell? := null; var ok := s.TryPop(out owned item); "
        "       item?.Dispose(); end; "
        "    return Cell._n; end; "  // 0
        "end Main; end C;", 0);
}

// --------------------------------------------------------------------------
// [895] §28.3/§29.2 — the loop variable of a `for var x in <OwnedEnumerable<T>>`
// loop OWNS each yielded element; on the exception path the current element is
// auto-disposed. It was registered for neither normal nor unwind disposal (the
// enumerator's Dispose frees the iterator's remaining state, not the already-
// yielded current element), so a throw in the body leaked it.
// --------------------------------------------------------------------------

// A throw in the body auto-disposes the current owned element (_n == 1).
BEZEL_TEST(exception_disposal_895, owned_iteration_var_unwind_disposed) {
    expectExitAndLeakFree(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C "
        "  public static method Gen(): OwnedEnumerable<R> "
        "    yield return new R(); end Gen; "
        "  public static method Main(): int "
        "    var disposed := 0; "
        "    try for var x in Gen() do throw new Exception(\"test\"); end; "
        "    catch Exception => do disposed := R._n; end, end; "
        "    return disposed; "  // 1
        "  end Main; end C;", 1);
}

// Double-free control: the body disposes x BEFORE throwing — the nulled slot +
// null-guard keep it exactly once (_n == 1, not 2); LSan asserts no double-free.
BEZEL_TEST(exception_disposal_895, owned_iteration_var_dispose_then_throw_once) {
    expectExitAndLeakFree(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C "
        "  public static method Gen(): OwnedEnumerable<R> "
        "    yield return new R(); end Gen; "
        "  public static method Main(): int "
        "    var disposed := 0; "
        "    try for var x in Gen() do x.Dispose(); throw new Exception(\"t\"); end; "
        "    catch Exception => do disposed := R._n; end, end; "
        "    return disposed; "  // 1
        "  end Main; end C;", 1);
}

// Control: normal exhaustion with an explicit discharge per element disposes
// each exactly once (the unwindOnly disposal correctly skips the normal path —
// _n == 2 for two elements, not 4). Value-only (compileAndRun, not
// expectExitAndLeakFree): `x.Dispose()` on an OwnedEnumerable loop variable runs
// the destructor but does not RECLAIM the element block on the normal path — a
// pre-existing leak tracked as #963, independent of #895's exception-path fix.
BEZEL_TEST(exception_disposal_895, owned_iteration_var_normal_discharge_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end R; "
        "class C "
        "  public static method Gen(): OwnedEnumerable<R> "
        "    yield return new R(); yield return new R(); end Gen; "
        "  public static method Main(): int "
        "    for var x in Gen() do x.Dispose(); end; "
        "    return R._n; "  // 2
        "  end Main; end C;"), 2);
}

}  // namespace
