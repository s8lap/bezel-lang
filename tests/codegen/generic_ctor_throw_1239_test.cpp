// Codegen — generic class constructor containing `throw` emits well-formed IR
// (issue #1239, §14/§31).
//
// ROOT CAUSE: when a generic class constructor body is inlined into the caller
// (lowerNew/inlineCtorBody) and the body ends with an unconditional `throw`,
// lowerScopedBlock terminates the current basic block via the throw's `br`
// instruction.  inlineCtorBody returned without checking the terminated flag,
// so the caller continued emitting instructions into the already-terminated
// block — producing a second terminator and failing LLVM IR verification
// ("Terminator found in the middle of a basic block").
//
// FIX: inlineCtorBody now captures lowerScopedBlock's return value and, when
// the body terminated the block, moves the builder to a fresh dead block so
// the caller's post-ctor continuation code is IR-valid (unreachable but
// well-formed; LLVM's DCE prunes it).
//
// Non-generic constructors with throw compiled fine before the fix (they are
// emitted as standalone functions; emitConstructorBody already checked
// `terminated` and omitted the trailing RetVoid).  The generic inline path
// was the only place lacking this guard.
//
// Tests:
//   T1 — generic ctor with unconditional throw: catches the exception, rc=0.
//         (was IR_FAIL before the fix)
//   T2 — generic ctor with field-store-then-throw: destructor count proves
//         the assigned owned field is disposed on the exception path (§28.4
//         ctor cleanup), rc=0 when count==1.
//   T3 — generic ctor with conditional throw: both paths (throw / no-throw)
//         produce correct results.
//   T4 — non-generic ctor with throw still works (regression guard).
//   T5 — generic ctor WITHOUT throw still works (regression guard).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

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
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1239() {
    return wtScratchDir("wt-1239");
}

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// ASan / LSan check: returns 0 = clean, 1 = sanitizer error, -1 = skip.
int runUnderAsanLsan(const std::string& source) {
    namespace fs = std::filesystem;
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir1239();
    std::string stem = unique_stem("bezel_1239_asan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer hit
    return 0;
}

} // namespace

// ── T1: §14/§31 — generic ctor with unconditional throw, caught ──────────────
//
// Issue #1239 repro: before the fix this emitted IR_FAIL ("Terminator found
// in the middle of a basic block").  After the fix the throw is caught and
// rc=0.
BEZEL_TEST(generic_ctor_throw_1239, ctor_unconditional_throw_caught) {
    // per §14 (generic class instantiation) / §31.1 (throw)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class G<T> where T: class "
        "  public constructor() throw new Exception(\"g\"); end constructor; "
        "  destructor end destructor; "
        "end G; "
        "class Cell end Cell; "
        "class C public static method Main(): int "
        "  try using var g := new G<Cell>(); catch Exception => var _x := 0; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── T2: §14/§28.4/§31 — field-store-then-throw: owned field disposed on ──────
//         the exception path (ctor cleanup, §28.4 / §22.9 step-3).
//
// The generic ctor stores an owned `Inner` into `_v`, then throws.  The ctor
// cleanup block (built by inlineCtorBody) must dispose `_v` before the
// exception leaves — the destructor bumps Inner._n.  rc=0 iff count==1.
BEZEL_TEST(generic_ctor_throw_1239, field_store_then_throw_owned_cleanup) {
    // per §14 (generic ctor inline) / §28.4 (ctor cleanup disposes assigned fields)
    // / §22.9 step-3 (exception path: dispose assigned owned fields + base).
    const std::string source =
        "module M; "
        "class Inner "
        "  public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Inner._n := Inner._n + 1; end destructor; "
        "end Inner; "
        "class G<T> where T: class "
        "  public owned _v: Inner; "
        "  public constructor(owned v: Inner) _v := v; throw new Exception(\"fail\"); end constructor; "
        "  destructor end destructor; "
        "end G; "
        "class Cell end Cell; "
        "class C public static method Main(): int "
        "  var inner := new Inner(); "
        "  try using var g := new G<Cell>(owned inner); "
        "  catch Exception => do "
        "    if Inner._n <> 1 then return 1; end; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;";

    // Destructor-count check: count must be exactly 1 (ctor cleanup ran).
    BEZEL_EXPECT_EQ(run(source), 0);

    // ASan/LSan: no double-free or use-after-free from the ctor cleanup path.
    int asan = runUnderAsanLsan(source);
    if (asan != -1) BEZEL_EXPECT_EQ(asan, 0);  // 0 == ASan-clean
}

// ── T3: §14/§31 — conditional throw: both paths produce correct results ───────
//
// With cond=true: the exception is caught and `a` is set to 2.
// With cond=false: the ctor succeeds and the using-var is properly disposed.
// rc=0 iff both branches produce the right result.
BEZEL_TEST(generic_ctor_throw_1239, conditional_throw_both_paths_correct) {
    // per §14 (generic ctor, conditional body) / §31.1 (throw)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class G3<T> where T: class "
        "  public owned _f: T; "
        "  public constructor(owned f: T, cond: bool) "
        "    _f := f; "
        "    if cond then throw new Exception(\"cond\"); end; "
        "  end constructor; "
        "  destructor end destructor; "
        "end G3; "
        "class Cell3 end Cell3; "
        "class C public static method Main(): int "
        "  var a := 0; "
        "  try using var g := new G3<Cell3>(new Cell3(), true); "
        "  catch Exception => a := 2; end; "
        "  if a <> 2 then return 1; end; "
        "  using var g2 := new G3<Cell3>(new Cell3(), false); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── T4: regression — non-generic ctor with throw still works ─────────────────
//
// Non-generic ctors were unaffected by #1239 (compiled as standalone functions).
// Guard against regression.
BEZEL_TEST(generic_ctor_throw_1239, non_generic_ctor_with_throw_regression) {
    // per §22.8 (constructor) / §31.1 (throw) — non-generic, must still work
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Plain "
        "  public constructor() throw new Exception(\"plain\"); end constructor; "
        "  destructor end destructor; "
        "end Plain; "
        "class C public static method Main(): int "
        "  var a := 0; "
        "  try using var p := new Plain(); "
        "  catch Exception => a := 1; end; "
        "  if a <> 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── T5: regression — generic ctor WITHOUT throw still works ──────────────────
//
// The fix only fires when bodyTerminated is true.  A normal (non-throwing)
// generic ctor must be unaffected.
BEZEL_TEST(generic_ctor_throw_1239, generic_ctor_without_throw_regression) {
    // per §14 (generic ctor) — normal path must still work after #1239 fix
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class NoThrow<T> where T: class "
        "  public owned _v: T; "
        "  public constructor(owned v: T) _v := v; end constructor; "
        "  destructor end destructor; "
        "end NoThrow; "
        "class Cell5 "
        "  public static _n: int := 0; "
        "  public constructor() Cell5._n := Cell5._n + 1; end constructor; "
        "  destructor end destructor; "
        "end Cell5; "
        "class C public static method Main(): int "
        "  using var g := new NoThrow<Cell5>(new Cell5()); "
        "  if Cell5._n <> 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}
