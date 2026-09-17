// Codegen — disposal of `using var` bound to a class instance whose fields
// are then set by ordinary assignment (§28): `using var h := new C(); h.f
// := v;` must dispose the resulting object exactly once at scope end.
//
// Before v0.188 (proposal 0037 graduation, #1735) this was originally a
// class-source `with`-expression test: `using var h := new C() with f := v
// end` (former §22.11's object-initializer sugar for exactly the
// two-statement form below). §9.5 (v0.188) retired that sugar — a
// class-typed `with` source is now a compile error — so these tests are
// migrated to the two-statement form the sugar desugared to. The disposal
// fix this file originally pinned (classOfLocal() recognizing a WithExpr
// initializer wrapping a NewExpr, or the object leaked) is retired along
// with the construct that needed it: a bare `new C()` initializer was
// always recognized, so the two-statement form was never at risk of that
// leak. See with_init_test.cpp for the disposal-on-a-throwing-assignment
// coverage (§28.3/§28.10) the retired construct's #747 R43 tests migrated
// to.
//
// Two complementary check kinds (matching the pattern in undisposed_leak_test.cpp):
//   * destructor-count JIT tests (always run): prove Dispose runs the right
//     number of times (destructor counter == expected).
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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
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
static const std::filesystem::path& wtDirObjinitUsingvar() {
    return wtScratchDir("wt-objinit-usingvar");
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
    if (rtObj.empty()) return -1;

    auto dir = wtDirObjinitUsingvar();
    std::string stem = unique_stem("bezel_objinit_usingvar_lsan");
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Assert the destructor-count (always) and, if the toolchain is present, that
// LeakSanitizer reports no leak/corruption.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert no double-free: LSan run with leak detection disabled (only UAF/DF).
void expectNoDoubleFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// -----------------------------------------------------------------------
// Shared class definitions used by the test programs.
//
// class C: owns a destructor that increments static _n, has one int field _f.
// class D: owns a destructor that increments static _n, has two int fields.
// -----------------------------------------------------------------------

// (a) `using var h := new C(); h._f := i;` in a loop → destructor runs
// each iteration, no leak. Migrated from the retired class-source `with`
// form (§9.5, v0.188, #1735); §28.
const char* kWithOneField_Loop =
    "module M; class C "
    "  public static _n: int := 0; "
    "  public _f: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 5 do "
    "      using var h := new C(); h._f := i; "
    "      i := i + 1; "
    "    end; "
    "    return _n; "
    "  end Main; "
    "end C;";

// (b) Multiple field assignments → no leak. Migrated from the retired
// class-source `with` form (§9.5, v0.188, #1735); §28.
// Uses a `do ... end` block so disposal runs before `return _n`.
const char* kWithMultipleFields =
    "module M; class D "
    "  public static _n: int := 0; "
    "  public _x: int := 0; public _y: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do using var h := new D(); h._x := 10; h._y := 32; end; "
    "    return _n; "
    "  end Main; "
    "end D;";

// (c) No double-free (ASan): destructor count == iterations.  §28.
// Each loop iteration disposes exactly one object (destructor +1 per iter).
// Migrated from the retired class-source `with` form (§9.5, v0.188, #1735).
const char* kWithLoop_CountExact =
    "module M; class C "
    "  public static _n: int := 0; "
    "  public _f: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 3 do "
    "      using var h := new C(); h._f := i; "
    "      i := i + 1; "
    "    end; "
    "    return _n; "
    "  end Main; "
    "end C;";
// Note: the while-loop `do...end` body disposes `h` at each iteration end,
// so `_n == 3` is read AFTER all iterations. No `do...end` wrapper needed.

// (d) CONTROL: plain `using var h := new C()` still disposes once.  §28.
// Ensures the fix did not regress the existing path.
// Uses a `do ... end` block so disposal runs before `return _n`.
const char* kPlainNew_Control =
    "module M; class C "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do using var h := new C(); end; "
    "    return _n; "
    "  end Main; "
    "end C;";

// (e) CONTROL: a record with-expression (value type, no class disposal needed).
// A record has no destructor; the with-copy is a value — nothing to dispose.
// The program just reads back the updated field to confirm correctness (no
// crash, no spurious disposal attempt).
const char* kRecordWith_Control =
    "module M; record Point(x: int, y: int); "
    "class Runner public static method Main(): int "
    "  var a := new Point(1, 2); "
    "  var b := a with(x := 9); "
    "  return b.x * 10 + b.y; "
    "end Main; end Runner;";

}  // namespace

// ---- (a) using var with one field, loop: destructor runs each iteration ----

// §28 (migrated from the retired §22.11 class-source `with` form, v0.188,
// #1735): `using var h := new C(); h._f := i;` in a loop → destructor runs
// each of the 5 iterations.
BEZEL_TEST(codegen_objinit_usingvar_migrated_9_5_28, with_one_field_loop_disposes) {
    expectDisposedAndLeakFree(kWithOneField_Loop, 5);
}

// ---- (b) multiple fields: destructor runs once at scope end ----------------

// §28 (migrated, #1735): two field assignments after construction →
// disposed once.
BEZEL_TEST(codegen_objinit_usingvar_migrated_9_5_28, with_multiple_fields_disposes_once) {
    expectDisposedAndLeakFree(kWithMultipleFields, 1);
}

// ---- (c) no double-free (ASan): destructor count == iterations -------------

// §28 (migrated, #1735): exactly 3 destructor calls for 3 loop iterations —
// no double-free, no skip. ASan confirms no UAF/DF even when disposal runs
// inside a loop.
BEZEL_TEST(codegen_objinit_usingvar_migrated_9_5_28, with_loop_exact_count_no_doublefree) {
    expectNoDoubleFree(kWithLoop_CountExact, 3);
}

// ---- (d) CONTROL: plain new still disposes once ----------------------------

// §28: ensure the existing plain `using var h := new C()` path is unaffected.
BEZEL_TEST(codegen_objinit_usingvar_migrated_9_5_28, plain_new_using_var_control_28) {
    expectDisposedAndLeakFree(kPlainNew_Control, 1);
}

// ---- (e) CONTROL: record with-expression is unaffected ---------------------

// §9.5: a record with-expression produces a value type — no class disposal
// is triggered. The field update is correct and there is no crash.
BEZEL_TEST(codegen_objinit_usingvar_migrated_9_5_28, record_with_unaffected_9_5) {
    BEZEL_EXPECT_EQ(run(kRecordWith_Control), 92);
}
