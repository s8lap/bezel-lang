// Codegen — §28.3 / §14.2: `using var a := new T()` inside a generic method
// body must dispose `a` at scope exit when T is a class type parameter.
//
// THE BUG: classOfLocal() resolved `new T()` to a TypeRef with
// `declaration == nullptr` (T is a type param, not a concrete class), so it
// returned null. None of the fallback paths in registerUsingDisposal matched
// a generic-new initializer, so no disposal entry was pushed. The object
// constructed (T=Widget) was never disposed — destructor count stayed wrong
// and the 16-byte block leaked.
//
// THE FIX: in classOfLocal, when the NewExpr's resolved type has
// `declaration == nullptr` AND its type name is a single-part identifier that
// appears in `genericSubstSemaTy_`, resolve through that map to the concrete
// class TypeRef and return its `declaration`.  The rest of
// registerUsingDisposal is unchanged: the concrete class is in disposerFns_
// so it follows the existing class-instance disposal path.
//
// Two complementary check kinds (matching undisposed_leak_test.cpp):
//   * destructor-count JIT tests (always run): the destructor bumps a static
//     counter observable via the int exit code — proves Dispose now runs the
//     right number of times.
//   * LeakSanitizer native tests (run only when ASan toolchain is present):
//     lower the program to an object file, link against an ASan-instrumented
//     runtime, run under LSan — proves the block is reclaimed with no leak /
//     no double-free / no use-after-free.

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirGenUsingvar() {
    return wtScratchDir("wt-gen-usingvar");
}

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification — same harness as undisposed_leak_test.cpp.
// Returns 0 = clean, 1 = sanitizer reported a problem, -1 = toolchain unavailable.
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDirGenUsingvar();
    std::string stem = unique_stem("bezel_gen_usingvar_lsan");
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

// Assert no double-free: LSan run with leak detection disabled (only UAF/DF).
void expectNoDoubleFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// -----------------------------------------------------------------------
// (a) The exact repro from the bug report (§28.3 / §14.2).
//
// class Maker<T> where T: class, new()
//   public method MakeOne(): int
//     using var a := new T();      // a is type-param-typed
//     return Widget._n;            // 1 inside
//   end MakeOne;                   // a's destructor SHOULD run here
// end Maker;
//
// Widget._n starts 0, MakeOne() constructs one Widget → _n becomes 1, then
// `a` must be disposed before MakeOne returns → _n goes back to 0.
// Main reads: r=1 (value BEFORE disposal), _n=0 (AFTER disposal).
// Expected: r * 10 + _n = 10.
// Bug (before fix): _n stays 1 → result = 11.
// -----------------------------------------------------------------------
const char* kReproExact =
    "module M; "
    "class Widget "
    "  public static _n: int := 0; "
    "  public constructor() _n := _n + 1; end constructor; "
    "  destructor _n := _n - 1; end destructor; "
    "end Widget; "
    "class Maker<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method MakeOne(): int "
    "    using var a := new T(); "
    "    return Widget._n; "
    "  end MakeOne; "
    "end Maker; "
    "class C public static method Main(): int "
    "  using var m := new Maker<Widget>(); "
    "  var r := m.MakeOne(); "
    "  return r * 10 + Widget._n; "
    "end Main; end C;";
// Expected: 10  (r=1 inside MakeOne before disposal; _n=0 after disposal)

// -----------------------------------------------------------------------
// (b) `using var a := new T()` in a loop — destructor count == iterations.
//     Each iteration constructs one Widget (counter +1) and disposes it
//     (counter -1), so after 5 iterations the counter is back to 0.
//     We track total constructions separately via a second static.
//     Expected exit code: 5 (total constructions) — or equivalently, _n==0
//     and ctor_count==5. We encode: ctor_count * 10 + _n → 50.
// -----------------------------------------------------------------------
const char* kLoopDispose =
    "module M; "
    "class Widget "
    "  public static _n: int := 0; "
    "  public static _ctor: int := 0; "
    "  public constructor() _n := _n + 1; _ctor := _ctor + 1; end constructor; "
    "  destructor _n := _n - 1; end destructor; "
    "end Widget; "
    "class Maker<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Run() "
    "    var i := 0; "
    "    while i < 5 do "
    "      using var a := new T(); "
    "      i := i + 1; "
    "    end; "
    "  end Run; "
    "end Maker; "
    "class C public static method Main(): int "
    "  do "
    "    using var m := new Maker<Widget>(); "
    "    m.Run(); "
    "  end; "
    "  return Widget._ctor * 10 + Widget._n; "
    "end Main; end C;";
// Expected: 50 (5 constructions, 5 disposals → _n back to 0)

// -----------------------------------------------------------------------
// (c) CONTROL: non-generic `using var a := new Widget()` in a plain method
//     still disposes once (ensure no regression).  §28.3.
//     Uses a `do...end` block so disposal runs before `return _n`.
// -----------------------------------------------------------------------
const char* kControlPlainNew =
    "module M; "
    "class Widget "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do using var a := new Widget(); end; "
    "    return _n; "
    "  end Main; "
    "end Widget;";
// Expected: 1

// -----------------------------------------------------------------------
// (d) Generic method with early return — `using var a := new T()` must still
//     be disposed on every exit path (§28.3 "on every normal-exit path").
//     MakeOne() returns Widget._n while a is live (=1), then a is disposed
//     (via scope exit, even on the `return` path) → _n goes back to 0.
//     Expected: r=1 (value before disposal), _n=0 (after). Encoded: r*10+_n = 10.
// -----------------------------------------------------------------------
const char* kEarlyReturnDispose =
    "module M; "
    "class Widget "
    "  public static _n: int := 0; "
    "  public constructor() _n := _n + 1; end constructor; "
    "  destructor _n := _n - 1; end destructor; "
    "end Widget; "
    "class Factory<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method MakeOne(): int "
    "    using var a := new T(); "
    "    return Widget._n; "
    "  end MakeOne; "
    "end Factory; "
    "class C public static method Main(): int "
    "  using var f := new Factory<Widget>(); "
    "  var r := f.MakeOne(); "
    "  return r * 10 + Widget._n; "
    "end Main; end C;";
// Expected: 10

// §28.10 (#1285): run the FULL front-end pipeline INCLUDING checkOwnership,
// then AOT-compile, link with ASan, and run — returning the program's real exit
// code (99 on a sanitizer abort / SEGV from a double-free, -1 when the
// toolchain is absent). runJit omits checkOwnership, so the ownership-pass flag
// that suppresses a borrow-local using-var disposal (usingVarSourceIsBorrow) is
// set only on this AOT path; a runJit run would double-free. An exit code equal
// to the expected value simultaneously proves disposed-exactly-once AND
// ASan-clean (no double-free).
int runAotExit(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1285_" + std::to_string(counter++);
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
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd = "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe +
                         " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (!WIFEXITED(status)) return 99;  // crashed (e.g. SEGV from a double-free)
    return WEXITSTATUS(status);
}

}  // namespace

// ---- (a) exact repro: destructor of `using var a := new T()` runs at scope end ----

// §28.3 / §14.2: `using var a := new T()` in a generic instance method body
// must dispose `a` exactly once at scope end (T=Widget, destructor increments
// then decrements the static counter). Expected exit 10: r=1 (Widget count
// inside MakeOne), _n=0 (Widget disposed before MakeOne returns).
// Before the fix: exit 11 (_n stayed 1 — no disposal entry was pushed).
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           repro_type_param_new_disposes_at_scope_end_28_3) {
    expectDisposedAndLeakFree(kReproExact, 10);
}

// ---- (b) loop: destructor count == iterations, no leak ----------------------

// §28.3 / §14.2: each of 5 loop iterations disposes exactly one Widget —
// ctor_count==5, _n==0. ASan/LSan verify no double-free and no leak.
// Before the fix: _n stayed 5 (no disposal per iteration) → result 55.
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           type_param_new_loop_exact_count_no_leak_28_3) {
    expectDisposedAndLeakFree(kLoopDispose, 50);
}

// ---- (c) CONTROL: plain non-generic new still disposes once -----------------

// §28.3: ensure the existing concrete-class `using var a := new Widget()`
// path is unaffected by the fix. Destructor runs exactly once → exit 1.
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           control_plain_new_unaffected_28_3) {
    expectDisposedAndLeakFree(kControlPlainNew, 1);
}

// ---- (d) early return path: disposal runs on return from generic method -----

// §28.3: `using var a := new T()` in a method that returns (not
// fall-through). The return path must also trigger disposal.
// Expected: 10 (r=1 inside, _n=0 after disposal).
// Before the fix: 11 (disposal never ran on return path either).
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           type_param_new_early_return_disposes_28_3) {
    expectNoDoubleFree(kEarlyReturnDispose, 10);
}

// ---- (e) §28.10 (#1285): borrow-LOCAL using-var must not double-free --------

// §28.10 (#1285): `using var c := b` where `b` is a local that ALIASES a borrow
// parameter holds no ownership obligation — the value's real owner (the
// caller's `using var w`) disposes it. Before the fix codegen registered a
// second scope-end disposal → a double-free (SIGSEGV). Now the ownership pass
// flags the borrow-local source and codegen skips the disposal, so the object
// is disposed exactly once (Widget.D == 1). The borrow-PARAMETER case was #1274;
// this is the borrow-LOCAL sibling. Verified on the AOT path because runJit
// omits checkOwnership (the suppression flag would otherwise not be set).
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           using_var_of_borrow_local_no_double_free_1285) {
    int rc = runAotExit(
        "module M; "
        "class Widget public static D: int := 0; "
        "  destructor Widget.D := Widget.D + 1; end destructor; end Widget; "
        "public static class Prog "
        "  public static method Take(x: Widget) "
        "    var b := x; using var c := b; end Take; "
        "  public static method Main(): int "
        "    do using var w := new Widget(); Prog.Take(w); end; "
        "    return Widget.D; end Main; "
        "end Prog;");
    if (rc == -1) return;         // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(rc, 1);       // disposed exactly once, ASan-clean
}

// §28.10 (#1285) control: an OWNED-local transfer to a using var STILL disposes
// exactly once — the fix only suppresses a positively-Borrowed source, so an
// owned local (Owned in the flow) keeps its scope-end disposal (Widget.D == 1).
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           using_var_of_owned_local_still_disposes_1285) {
    int rc = runAotExit(
        "module M; "
        "class Widget public static D: int := 0; "
        "  destructor Widget.D := Widget.D + 1; end destructor; end Widget; "
        "public static class Prog "
        "  public static method Main(): int "
        "    do var o := new Widget(); using var c := o; end; "
        "    return Widget.D; end Main; "
        "end Prog;");
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

// ---- (f) §27.1 (#1290): owned result of a function-typed-value call -----------

// §27.1 / §28.3 (#1290): `mk: () -> owned Res; using var made := mk()` — a call
// through a FUNCTION-TYPED VALUE whose return slot carries `owned` produces a
// fresh owned value; binding it to a `using var` disposes it exactly once. The
// callee is an ordinary value (not a MethodSymbol), so the owned-return was
// previously undetected and the object leaked (Res.D stayed 0). Now the
// producerState/dynamic-dispose paths key on the callee function type's
// funcReturnOwned flag → disposed once (Res.D == 1). Verified on the AOT path
// (ASan-clean; the plain-var auto-drop leg needs checkOwnership, which runJit
// omits).
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           using_var_of_fn_value_owned_result_disposed_1290) {
    int rc = runAotExit(
        "module M; "
        "class Res public static D: int := 0; "
        "  destructor Res.D := Res.D + 1; end destructor; end Res; "
        "class C public static method Main(): int "
        "  var mk: () -> owned Res := lambda() => new Res(); "
        "  do using var made := mk(); end; "
        "  return Res.D; end Main; "
        "end C;");
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 1);
}

// §27.1 / §28.3 (#1290): the plain-`var` auto-drop path also disposes the owned
// result of a function-typed-value call exactly once (Res.D == 1).
BEZEL_TEST(codegen_generic_usingvar_disposal_28_3_14_2,
           plain_var_of_fn_value_owned_result_autodropped_1290) {
    int rc = runAotExit(
        "module M; "
        "class Res public static D: int := 0; "
        "  destructor Res.D := Res.D + 1; end destructor; end Res; "
        "class C public static method Main(): int "
        "  var mk: () -> owned Res := lambda() => new Res(); "
        "  do var made := mk(); end; "
        "  return Res.D; end Main; "
        "end C;");
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 1);
}
