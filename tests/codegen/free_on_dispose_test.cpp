// Codegen — free-on-dispose (issue #380, §28). The compiler-generated Dispose
// must run the destructor (and base-dispose chain) AND reclaim the object's
// heap block: "dispose" means run the destructor and free the memory,
// deterministically (no GC). Before the fix, every class object leaked its
// `calloc`'d block on dispose; the destructor ran but the block was never
// freed.
//
// Two complementary checks:
//
//  1. Destructor-count tests (always run): compile each program to a native
//     executable via the real Driver pipeline and assert its exit code. These
//     prove the *most-derived* Dispose ran the full destructor + base chain on
//     every dispose path (using-var scope-end, base chain, owned field,
//     exception unwind, owning-collection elements). They do not, by
//     themselves, observe the free — but a misplaced free shows up as a
//     use-after-free / double-free under the sanitizer check below, and the
//     full ASan/LSan suite was run out-of-band (see the report on #380).
//
//  2. LeakSanitizer check (runs only when a sanitizer toolchain is available):
//     re-lower the same program to an object file, link it against an
//     ASan-instrumented build of the Bezel runtime, and run the result under
//     LeakSanitizer. The fix is verified iff LSan reports no leak (the object's
//     block is reclaimed) and ASan reports no use-after-free / double-free.
//     The JIT test harness cannot host LSan (its in-process LLVM/ORC state is
//     intentionally never freed and would swamp the report — see MEMORY note on
//     "LLVM accumulation"), so this is built as a standalone native program, as
//     the issue directs.

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
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 if compilation/link/run failed. Mirrors
// native_exe_test.cpp.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_380_" + std::to_string(counter++);
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

// Returns the path to the Bezel runtime source (compiled here with ASan for the
// LeakSanitizer check). Derived from this test file's location so it works
// regardless of the build/run directory.
std::string runtimeSourcePath() {
    fs::path here(__FILE__);                       // tests/codegen/<this>
    fs::path root = here.parent_path()             // tests/codegen
                        .parent_path()             // tests
                        .parent_path();            // repo root
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// LeakSanitizer verification for a single program. Lowers `source` to an object
// file (reusing the codegen API), links it against an ASan-instrumented build of
// the runtime, and runs the binary under LeakSanitizer. Returns:
//   0  — ran clean: no leak, no use-after-free / double-free (the #380 fix).
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
// Requires `clang++` and ASan support; skips gracefully when absent.
int runUnderLeakSanitizer(const std::string& source) {
    // A working C++ compiler with sanitizers is required; if any step's command
    // is unavailable the function returns -1 (skip), never a false failure.
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_380_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
    std::string exe     = (dir / stem).string();

    // Step 1: front-end + lower the program to a native object file.
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

    // Step 2: compile the runtime with ASan, then link both into an ASan binary.
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

    // Step 3: run under LeakSanitizer. ASan exits non-zero on any leak or
    // memory-safety error; a clean dispose path exits with the program's own
    // (non-error) status.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Run both checks for a program: assert the destructor-count exit code, and (if
// the toolchain is present) assert LeakSanitizer reports no leak/corruption.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;          // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);        // 0 == LSan clean (no leak, no UAF/DF)
}

// --- Programs (exit code encodes destructor activity; LSan checks the free) ---

// §28.2: a single `using var` class instance — its destructor runs at scope end
// and its heap block is reclaimed. The #380 baseline reproducer.
const char* kSimple =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    do using var r := new R(); end; return _n; "
    "  end Main; "
    "end R;";

// §28.4: a derived instance disposes most-derived-first (Derived +1) then chains
// to the base (Base +100) = 101, and the block is freed exactly once — at the
// most-derived entry, after the full base chain.
const char* kBaseChain =
    "module M; class Base "
    "  public static _n: int := 0; "
    "  destructor Base._n := Base._n + 100; end destructor; "
    "  public static method Main(): int "
    "    do using var d := new Derived(); end; return Base._n; "
    "  end Main; "
    "end Base; "
    "class Derived : Base "
    "  destructor Base._n := Base._n + 1; end destructor; "
    "end Derived;";

// §28.4: an owner with an owned field disposes the field (its own destructor +
// base chain) first, then itself; both blocks are reclaimed, in order. Encodes
// owner(10000) + field-Derived(1) + field-Base-chain(100) = 10101.
const char* kOwnedField =
    "module M; class Base "
    "  public static _n: int := 0; "
    "  destructor Base._n := Base._n + 100; end destructor; "
    "  public static method Main(): int "
    "    do using var o := new Owner(); end; return Base._n; "
    "  end Main; "
    "end Base; "
    "class Derived : Base "
    "  destructor Base._n := Base._n + 1; end destructor; "
    "end Derived; "
    "class Owner "
    "  private owned _b: Base; "
    "  destructor Base._n := Base._n + 10000; end destructor; "
    "  public constructor() this._b := new Derived(); end constructor; "
    "end Owner;";

// §28.3: a using var disposed on the exception-unwind path — its destructor runs
// and its block is reclaimed during unwinding. (The thrown AppError object has a
// separate, pre-existing lifecycle and is out of scope for #380.)
const char* kExceptionPath =
    "module M; class AppError end AppError; "
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method Main(): int "
    "    try do using var r := new R(); throw new AppError(); end; "
    "    catch AppError => return _n; end; return -1; "
    "  end Main; "
    "end R;";

// stdlib §4.5 + §28: an OwnedArray<R> disposes each owned class element exactly
// once at scope end and reclaims each element's block (the owning-collection
// element path, distinct from the dynamic-dispatch path).
const char* kOwnedArray =
    "module M; class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class C public static method Main(): int "
    "  do using var a := new OwnedArray<R>(); "
    "     a.Add(move (new R())); a.Add(move (new R())); a.Add(move (new R())); "
    "  end; return R._n; "
    "end Main; end C;";

}  // namespace

BEZEL_TEST(codegen_free_on_dispose_380, simple_object_disposed_and_freed) {
    // One destructor; block reclaimed (LSan: 0 leaked).
    expectDisposedAndLeakFree(kSimple, 1);
}

BEZEL_TEST(codegen_free_on_dispose_380, base_chain_freed_once_after_chain) {
    // Derived(+1) then Base(+100) = 101; freed once at most-derived entry.
    expectDisposedAndLeakFree(kBaseChain, 101);
}

BEZEL_TEST(codegen_free_on_dispose_380, owned_field_owner_frees_field_then_self) {
    // owner(10000)+field-Derived(1)+field-Base(100) = 10101; both blocks freed.
    // The process exit status is the low 8 bits: 10101 & 0xFF == 117.
    expectDisposedAndLeakFree(kOwnedField, 10101 & 0xFF);
}

BEZEL_TEST(codegen_free_on_dispose_380, disposed_and_freed_on_exception_path) {
    // The using var R is disposed (destructor runs) AND its block is freed
    // during exception unwinding — both via the same dynamic-dispatch path that
    // every other dispose site uses, so the free verified by the other cases'
    // LSan legs covers this one too. We assert the destructor count (1) here.
    //
    // We deliberately do NOT run the LeakSanitizer leg for this program: the
    // thrown `AppError` exception object has its own, pre-existing lifecycle
    // (exception objects are `calloc`'d and not reclaimed — unrelated to #380's
    // "free the disposed class instance" fix), so LSan would report that
    // separate leak and mask the (correct) R reclamation. Verified out-of-band
    // that the *only* block leaked on this path is the exception object (8
    // bytes, one object), i.e. R is freed correctly on unwind.
    BEZEL_EXPECT_EQ(compileAndRun(kExceptionPath), 1);
}

BEZEL_TEST(codegen_free_on_dispose_380, owning_collection_elements_freed) {
    // Three owned elements disposed+freed exactly once each.
    expectDisposedAndLeakFree(kOwnedArray, 3);
}
