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

std::string runtimeSourcePath() {
    fs::path here(__FILE__);                       // tests/codegen/<this>
    fs::path root = here.parent_path()             // tests/codegen
                        .parent_path()             // tests
                        .parent_path();            // repo root
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// LeakSanitizer verification for one program. Lowers to an object file, links
// against an ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP.
// Identical mechanism to free_on_dispose_test.cpp.
int runUnderLeakSanitizer(const std::string& source) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_excdisp_lsan_" + std::to_string(counter++);
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
// [03] §28.2 / §28.4 / §28.5 — destructor explicitly disposes an owned field,
// then §28.4-step-2 auto-disposes the same field: the field's destructor runs
// exactly once; the field's block is freed exactly once.
// --------------------------------------------------------------------------

// MyError's destructor calls `_p.Dispose()` (disposes Payload — Payload._n == 1)
// and the generated MyError Dispose then auto-disposes `_p` (§28.4 step 2). Per
// §28.2 the second disposal is a no-op. Pre-fix: re-ran Payload's destructor on
// the freed Payload (double-free, SIGSEGV). The counter is read after `e` is
// disposed at scope end.
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
    "  destructor _p.Dispose(); end destructor; "
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

// The original finding [03] minimal repro: `using var e := new MyError(new
// Payload()); return 0;` — no counter accessor, just exit 0 with no crash.
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
    "  destructor _p.Dispose(); end destructor; "
    "end MyError; "
    "class R "
    "  public static method Main(): int "
    "    using var e := new MyError(new Payload()); "
    "    return 0; "
    "  end Main; "
    "end R;";

BEZEL_TEST(codegen_excdisp_03, dtor_field_dispose_plus_autodispose_no_double_free) {
    // Exit 0 AND ASan/LSan-clean: Payload disposed exactly once, both blocks
    // (MyError and Payload) reclaimed exactly once, no double-free / UAF.
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
    "    new Holder(); "
    "  end Use; "
    "  public static method Main(): int "
    "    try "
    "      Use(); "
    "    catch Boom => Console.WriteLine(\"caught\"); end; "
    "    return Res.count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(codegen_excdisp_04, ctor_throw_result_dropped_still_cleaned) {
    // The dropped `new Holder()` whose ctor throws still disposes its assigned
    // owned field (count == 1) on the unwind.
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

}  // namespace
