// Codegen — §28.3 / §14.1 / §28.8.1: `using var x := move ownedParam` inside
// a generic method body must dispose `x` at scope end when the type-parameter
// is substituted with a class type.
//
// BUG #572: classOfLocal() and usingVarIsMovedClassObject() both failed to
// resolve through genericSubstSemaTy_ for a MoveExpr whose source operand has
// a generic-type-parameter-typed type (declaration == nullptr).
//
//   usingVarIsMovedClassObject() calls isClassOrInterfaceRef(ctx_.exprType(...))
//   which requires declaration != nullptr.  When `item` has type T (a generic
//   type param), the exprType's declaration is null → isClassOrInterfaceRef
//   returns false → usingVarIsMovedClassObject returns false.
//
//   classOfLocal() only inspects NewExpr and WithExpr initializers; a MoveExpr
//   initializer was never examined, so classOfLocal also returns null.
//
//   No disposal entry was registered → x was never disposed → the object held
//   by item after `move item` leaked; its destructor never ran.
//
// FIX: in usingVarIsMovedClassObject, when the MoveExpr's resolved type has
// declaration == nullptr AND is a single-part name present in
// genericSubstSemaTy_, look up the concrete substituted TypeRef and test
// isClassOrInterfaceRef on it instead.  Move already transferred ownership to
// the using-var slot (item is consumed), so we register exactly one disposal
// entry and avoid double-dispose of item.
//
// Test programs (§28.3 / §14.1 / §28.8.1):
//   (a) Repro from #572: Take<T>(owned item: T) where T: class, using
//       var x := move item; destructor count must be 1 after Take returns.
//   (b) Move a locally-created T: using var x := move local where local is
//       type-param typed — equivalent path, same fix needed.
//   (c) Non-generic control: Take(owned item: R) without type parameters —
//       ensures the existing non-generic move path is not regressed.
//   (d) Double-dispose check: item must NOT be disposed after move; only x
//       is disposed. We verify destructor count == 1 (not 2).
//
// Test kinds:
//   * JIT destructor-count tests (always run): exit code encodes the count.
//   * LSan native tests (run only when the ASan toolchain is present): verify
//     no leak (the bug) and no double-free / use-after-free.

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
static const std::filesystem::path& wtDirGenUvm() {
    return wtScratchDir("wt-gen-uvm");
}

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification. Returns 0 = clean, 1 = sanitizer problem,
// -1 = toolchain unavailable / compile failure.
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDirGenUvm();
    std::string stem = unique_stem("bezel_gen_uvm_lsan");
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return -1;
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

// Assert destructor count (always) and, when the toolchain is present,
// that LSan reports no leak/corruption.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert no double-free: LSan with leak detection disabled (only UAF/DF).
void expectNoDoubleFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// -----------------------------------------------------------------------
// (a) Repro from #572: `using var x := move item` where item is an
//     `owned` parameter of generic type T (T: class).
//
// class R: destructor increments static _n (count starts 0).
// class Util static Take<T>(owned item: T): int where T: class
//   using var x := move item;   // item ownership moves to x
//   return 0;                   // x must be disposed here → _n becomes 1
// end Take;
// Main: var r := new R(); Util.Take<R>(move r); return R._n
// Expected: 1 (x disposed at Take return → destructor ran once).
// Before fix: 0 (_n never incremented — no disposal entry for x).
// -----------------------------------------------------------------------
const char* kReproMoveParam =
    "module M; "
    "class R "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class Util "
    "  public static method Take<T>(owned item: T): int where T: class "
    "    using var x := move item; "
    "    return 0; "
    "  end Take; "
    "end Util; "
    "class C public static method Main(): int "
    "  var r := new R(); "
    "  Util.Take<R>(move r); "
    "  return R._n; "
    "end Main; end C;";
// Expected: 1

// -----------------------------------------------------------------------
// (b) `using var x := move local` where local itself was created inside the
//     generic method as a T (type-param typed local), then moved into x.
//     Two objects created → two disposals expected → _n == 2.
//     local1 is moved into x (disposed at inner scope end), local2 is moved
//     into y (disposed at outer scope end). Main verifies both disposed.
//     Expected: 2.
// -----------------------------------------------------------------------
const char* kMoveLocalTypeParam =
    "module M; "
    "class R "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class Maker<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method RunTwo(): int "
    "    var local1 := new T(); "
    "    using var x := move local1; "
    "    var local2 := new T(); "
    "    using var y := move local2; "
    "    return 0; "
    "  end RunTwo; "
    "end Maker; "
    "class C public static method Main(): int "
    "  do using var m := new Maker<R>(); m.RunTwo(); end; "
    "  return R._n; "
    "end Main; end C;";
// Expected: 2

// -----------------------------------------------------------------------
// (c) Control: non-generic `Take(owned item: R)` — existing concrete
//     move path must still dispose x exactly once. §28.3 / §28.8.1.
//     No type parameters involved → existing usingVarIsMovedClassObject
//     should already handle this (regression guard).
// -----------------------------------------------------------------------
const char* kControlNonGeneric =
    "module M; "
    "class R "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class Util "
    "  public static method Take(owned item: R): int "
    "    using var x := move item; "
    "    return 0; "
    "  end Take; "
    "end Util; "
    "class C public static method Main(): int "
    "  var r := new R(); "
    "  Util.Take(move r); "
    "  return R._n; "
    "end Main; end C;";
// Expected: 1

// -----------------------------------------------------------------------
// (d) Double-dispose safety: after `using var x := move item`, item is
//     consumed and must NOT be separately disposed. Only x is disposed,
//     so the destructor count must be exactly 1 (not 2).
//     §28.8.1: move transfers ownership; the source slot is dead.
// -----------------------------------------------------------------------
const char* kNoDoubleFree =
    "module M; "
    "class R "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class Util "
    "  public static method Take<T>(owned item: T): int where T: class "
    "    using var x := move item; "
    "    return 0; "
    "  end Take; "
    "end Util; "
    "class C public static method Main(): int "
    "  var r := new R(); "
    "  Util.Take<R>(move r); "
    "  return R._n; "
    "end Main; end C;";
// Expected: 1  (exactly one disposal — x; item was consumed by move)

}  // namespace

// ---- (a) repro: using var x := move item disposes x at scope end ---------

// §28.3 / §14.1 / §28.8.1: owned generic-type-param parameter moved into
// a using-var must be disposed exactly once at scope end.
// Before fix: R._n == 0 (no disposal entry pushed — classOfLocal and
// usingVarIsMovedClassObject both failed to resolve through
// genericSubstSemaTy_ for a MoveExpr source whose exprType has no
// declaration).
BEZEL_TEST(codegen_generic_usingvar_move_disposal_572_28_3_14_1,
           repro_move_owned_param_disposes_at_scope_end_28_3) {
    expectDisposedAndLeakFree(kReproMoveParam, 1);
}

// ---- (b) move of local type-param variable into using-var ---------------

// §28.3 / §14.1: both moves (local1→x and local2→y) must each register a
// disposal entry; the destructor runs once per using-var → count == 2.
// Before fix: count == 0 (neither x nor y had a disposal entry).
BEZEL_TEST(codegen_generic_usingvar_move_disposal_572_28_3_14_1,
           move_local_type_param_to_usingvar_disposes_both_28_3) {
    expectDisposedAndLeakFree(kMoveLocalTypeParam, 2);
}

// ---- (c) control: non-generic move path unaffected ----------------------

// §28.3 / §28.8.1: the concrete-class (non-generic) `using var x := move
// item` path must still dispose x exactly once. Regression guard.
BEZEL_TEST(codegen_generic_usingvar_move_disposal_572_28_3_14_1,
           control_non_generic_move_still_disposes_28_3) {
    expectDisposedAndLeakFree(kControlNonGeneric, 1);
}

// ---- (d) double-dispose safety: item not disposed after move ------------

// §28.8.1: move transfers ownership from item to x. After the move, item
// is dead and must not be disposed a second time. Destructor count must be
// exactly 1 (x disposed at scope end), not 2.  ASan/LSan verify no
// double-free / use-after-free.
BEZEL_TEST(codegen_generic_usingvar_move_disposal_572_28_3_14_1,
           move_owned_param_no_double_free_28_8_1) {
    expectNoDoubleFree(kNoDoubleFree, 1);
}
