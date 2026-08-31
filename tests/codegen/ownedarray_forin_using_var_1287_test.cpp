// Codegen/ownership — #1287: direct `for var item in <OwningArray<T>>` combined
// with `using var u := item` (adopting each element) double-freed.
//
// Spec §28.11 / §29.2 (v0.183): default iteration over an owning collection
// (`for var item in arr` where arr: OwningArray<T>) uses the collection's
// IEnumerable<T> conformance — the BORROWING path — and produces BORROWS of
// each element; the collection still owns its elements after iteration. To
// consume, iterate over `arr.Drain()` (which returns IOwningEnumerable<T> and
// yields owned values). Spec §28.10 ("using var initialized from a borrow"):
// a `using var u := item` whose right-hand side is a borrow does NOT assume a
// disposal obligation — it is a borrow-pin; at scope end no Dispose() is
// called, because the value is not owned by this scope.
//
// The bug: codegen's registerUsingDisposal → classOfLocal #639 path registered
// a static disposal for `using var u := item` keyed on the element's class type
// alone, not on whether ownership actually transferred. The loop variable of a
// borrowing for-loop is a borrow, so the using var is a borrow-pin — but codegen
// disposed it at each iteration's body end AND the array's own element cascade
// disposed it again at scope end → SIGSEGV in bz$elemDispose (double-free).
//
// The fix lives in the ownership pass (Ownership.cpp handleLocalDecl): a
// borrowing for-loop variable is never seeded into the ownership state map
// (seedOwnedIterVar only seeds IOwningEnumerable/IOwningAsyncEnumerable loops),
// so the usingVarSourceIsBorrow flag was not set for it. The fix detects a
// bare-identifier using-var source whose symbol is a borrowing for-loop variable
// (decl is a ForStmt that is not an owning-enumerable loop) and sets the flag,
// so codegen skips the disposal — matching §28.10's borrow-pin semantics.
//
// IMPORTANT: these tests MUST run the full pipeline including
// ownership::checkOwnership (the shared runJit skips it; see the runner below).
// Without checkOwnership the ownership pass never runs, usingVarSourceIsBorrow
// is never set, and the bug would be masked.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

namespace {

namespace fs = std::filesystem;

// Full-pipeline runner: lex → parse → sema → checkDefiniteAssignment →
// checkOwnership → emitModule → JIT. The ownership pass MUST run (it sets
// usingVarSourceIsBorrow, the flag the fix keys on); the shared runJit skips
// checkOwnership and would mask the bug. Mirrors owned_drain_639_test.cpp's
// runFull / iterator_owned_param_945_test.cpp's run.
int runFull(const std::string& src) {
    bezel::test::ensureNativeTargetInitialized();
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -999;
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx);
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
        llvm::consumeError(std::move(err)); return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    int exitCode = sym->toPtr<int(*)()>()();
    bz_flush_static_owned();
    return exitCode;
}

// Subprocess-isolated ASan runner: full pipeline (with checkOwnership) → object
// → link with the ASan runtime → run. Returns 0 (clean), 1 (sanitizer fired —
// the double-free), -1 (toolchain missing → SKIP). Mirrors
// owned_drain_639_test.cpp's runUnderASan. This is the robust pin: without the
// fix the array cascade re-disposes each already-freed element → heap-use-after-
// free detected by ASan in a child process (no shard crash); with the fix each
// element is disposed exactly once (by the array) and the using var is a no-op
// borrow-pin.
int runUnderASan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1287_asan_" + std::to_string(counter++);
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
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Shared destructor-counter class. R._n increments on each disposal; the body
// is wrapped in a nested `do ... end` so the OwningArray's scope-end element
// cascade runs BEFORE `return R._n` reads the counter (the using var arr is
// disposed at its enclosing block's end, which precedes the return).
std::string makeProgram(const std::string& blockBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "do "
        + blockBody +
        " end; "
        "return R._n; "
        "end Main; end C;";
}

}  // namespace

// ===========================================================================
// #1287 / §28.10 / §28.11 / §29.2: the bug — direct for-in over an
// OwningArray<T> + `using var u := item` must NOT double-free.
// ===========================================================================

// §28.10 / §29.2: `for var item in arr` (default iteration over OwningArray<T>)
// yields BORROWS — the array retains ownership. `using var u := item` is a
// borrow-pin (§28.10): no disposal obligation. Each element is disposed exactly
// once, by the array's own element cascade at the block's end. R._n == 2.
//
// Without the fix: `using var u := item` registered a second disposal per
// iteration (classOfLocal #639 keyed on the class type, not the borrow), so each
// element was disposed at body-end AND again by the array cascade → double-free
// (SIGSEGV in bz$elemDispose).
BEZEL_TEST(ownedarray_forin_using_var_1287,
           direct_forin_using_var_disposes_once_spec_28_11) {
    BEZEL_EXPECT_EQ(runFull(makeProgram(
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  arr.Add(new R()); "
        "  for var item in arr do "
        "    using var u := item; "
        "  end; "
    )), 2);
}

// Robust double-free pin (subprocess-isolated ASan). Without the fix the array
// cascade re-disposes each already-freed element → ASan heap-use-after-free
// (returns 1); with the fix each element is disposed once (returns 0).
//
// runUnderASan returns 0 (clean), 1 (sanitizer fired), -1 (toolchain missing →
// SKIP). The sibling pattern in owned_drain_639_test.cpp /
// owning_enumerator_1349_test.cpp handles -1 with an early `return` — early-
// returning from a BEZEL_TEST body keeps `currentTestFailed()` false, so the
// harness reports a clean PASS. The bare `BEZEL_EXPECT_EQ(..., 0)` form used
// here would instead report a FAIL on -1, mis-diagnosing the unavailable
// toolchain as a real double-free (#3613). Mirror the sibling pattern.
BEZEL_TEST(ownedarray_forin_using_var_1287,
           direct_forin_using_var_no_double_free_asan_spec_28_11) {
    int rc = runUnderASan(makeProgram(
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  arr.Add(new R()); "
        "  for var item in arr do "
        "    using var u := item; "
        "  end; "
    ));
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no leak, no double-free)
}

// ===========================================================================
// No-regression controls.
// ===========================================================================

// §28.11 / §29.2: direct for-in over an OwningArray<T> WITHOUT adoption still
// finalizes every element via the array's own cascade (the loop variable is a
// borrow; nothing is consumed). R._n == 2. Unaffected by the fix (no using-var
// adoption of a loop variable) — a no-regression guard on the borrowing path.
BEZEL_TEST(ownedarray_forin_using_var_1287,
           direct_forin_without_adoption_finalizes_spec_28_11) {
    BEZEL_EXPECT_EQ(runFull(makeProgram(
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  arr.Add(new R()); "
        "  for var item in arr do "
        "    var n: int := R._n; "   // read the borrow; no adoption
        "  end; "
    )), 2);
}

// §4.5 / §29.2 (#639): the ownership-transferring Drain path is UNAFFECTED by
// the fix. `for var item in arr.Drain()` yields OWNED values (the loop variable
// is seeded Owned by the ownership pass), so `using var u := item` legitimately
// adopts (item is marked moved; u discharges it). R._n == 2. This confirms the
// fix's else-branch never fires for the owning path.
BEZEL_TEST(ownedarray_forin_using_var_1287,
           drain_forin_using_var_unaffected_spec_4_5) {
    BEZEL_EXPECT_EQ(runFull(makeProgram(
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  arr.Add(new R()); "
        "  for var item in arr.Drain() do "
        "    using var u := item; "
        "  end; "
    )), 2);
}
