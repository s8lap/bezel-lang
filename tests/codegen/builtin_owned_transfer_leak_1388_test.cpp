// Regression + control tests for fix #1388 (§28.3 affine auto-drop of a
// never-discharged builtin owned-TRANSFER result).
//
// The sema ownership pass keyed producer recognition on a user MethodSymbol's
// `returnOwned` (plus NewExpr / operator / `??`). The owning-collection drain
// methods (OwningStack.Pop, OwningQueue.Dequeue, OwningArray.PopLast/PopFirst/
// PopAt, OwningDictionary.PopValue) and IOwningEnumerator.TakeCurrent have NO
// MethodSymbol, so `var x := s.Pop();` was NEVER classified Owned: no §28.3
// affine must-use warning, no `autoDropAtScopeEnd`, no scope-end disposal — a
// SILENT leak. Codegen already reclaimed these via the shared predicate
// `common::isBuiltinOwnedTransferCall` (LlvmBackend_Setup emitOwnedLocal case
// (b3); unwind; §28.7 drop-on-overwrite), so only the never-discharged
// sema-driven scope-end path leaked. The fix makes `producerState` consult the
// SAME shared predicate.
//
// Coverage:
//   (a) SEMA: the §28.3 auto-drop warning now FIRES for a never-discharged
//       plain-`var` bound to each builtin owned-transfer method.
//   (b) AOT + ASan/LSan: the never-discharged case is now LEAK-CLEAN (the
//       auto-drop disposes it at scope end).
//   CONTROLS (prove the drift closed without over-broadening):
//   (c) a DISCHARGED transfer (using var / explicit Dispose / move-into-
//       collection) must NOT warn and must be leak-clean.
//   (d) a BORROW-returning builtin call (OwningStack.Peek, a borrowing
//       Stack.Pop) must NOT be treated as an owned producer — no warning, no
//       double-free.
//
// Spec citations: §28.3 (affine auto-drop), §28.11 (owning-collection element
// disposal), §29.1 v0.161 (TakeCurrent), stdlib §4.5/§4.7/§4.9/§4.11.

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
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1388() {
    return wtScratchDir("wt-1388");
}

// Full pipeline (DA + ownership) for value-observable disposal checks.
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

// AOT compile → object → link with ASan → run.
// Returns 0 (clean), 1 (sanitizer fired), -1 (toolchain missing / build error).
int runUnderASan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir1388();
    std::string stem = unique_stem("bezel_1388_asan");
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

// Counting class: destructor bumps a static so disposal is observable.
const std::string kR =
    "class R "
    "  public static _n: int := 0; "
    "  public v: int := 0; "
    "  public constructor(x: int) v := x; end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

// Sema-only pipeline: returns true iff the §28.3 auto-drop warning fired for the
// named local (and there were no hard errors).
bool warnsAutoDrop(const std::string& body, const std::string& name) {
    auto r = parseAndCheckOwnership("module M; " + kR + body);
    return !r.diagnostics.hasErrors() && hasAutoDropWarning(r, name);
}

}  // namespace

// ===========================================================================
// (a) SEMA — the §28.3 auto-drop warning now fires for a never-discharged
// plain `var` bound to each builtin owned-transfer method.
// ===========================================================================

// §4.11 OwningStack.Pop(): owned T.
BEZEL_TEST(builtin_owned_transfer_1388, pop_owning_stack_warns_spec_28_3) {
    BEZEL_EXPECT_TRUE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  var x := s.Pop(); "  // never discharged → warns
        "  return 0; "
        "end Main; end C;", "x"));
}

// §4.11 OwningQueue.Dequeue(): owned T.
BEZEL_TEST(builtin_owned_transfer_1388, dequeue_owning_queue_warns_spec_28_3) {
    BEZEL_EXPECT_TRUE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<R>(); "
        "  q.Enqueue(move (new R(1))); "
        "  var x := q.Dequeue(); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// §4.5 OwningArray.PopLast(): owned T.
BEZEL_TEST(builtin_owned_transfer_1388, poplast_owning_array_warns_spec_28_3) {
    BEZEL_EXPECT_TRUE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(new R(1)); "
        "  var x := a.PopLast(); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// §4.5 OwningArray.PopFirst(): owned T.
BEZEL_TEST(builtin_owned_transfer_1388, popfirst_owning_array_warns_spec_28_3) {
    BEZEL_EXPECT_TRUE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(new R(1)); "
        "  var x := a.PopFirst(); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// §4.5 OwningArray.PopAt(index): owned T.
BEZEL_TEST(builtin_owned_transfer_1388, popat_owning_array_warns_spec_28_3) {
    BEZEL_EXPECT_TRUE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(new R(1)); "
        "  var x := a.PopAt(0); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// §4.9 OwningDictionary.PopValue(key): owned V.
BEZEL_TEST(builtin_owned_transfer_1388, popvalue_owning_dict_warns_spec_28_3) {
    BEZEL_EXPECT_TRUE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.Add(1, new R(1)); "
        "  var x := d.PopValue(1); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// §29.1 v0.161 IOwningEnumerator.TakeCurrent(): owned T.
BEZEL_TEST(builtin_owned_transfer_1388, takecurrent_owning_enum_warns_spec_29_1) {
    BEZEL_EXPECT_TRUE(warnsAutoDrop(
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    if en.MoveNext() then "
        "      var x := en.TakeCurrent(); "  // never discharged → warns
        "    end; "
        "    return 0; "
        "  end Main; end C;", "x"));
}

// ===========================================================================
// (c) CONTROL (SEMA) — a DISCHARGED transfer must NOT warn.
// ===========================================================================

// `using var` discharges — no auto-drop warning.
BEZEL_TEST(builtin_owned_transfer_1388, pop_using_var_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  using var x := s.Pop(); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// Explicit `.Dispose()` discharges (everDischarged > 0) — no warning.
BEZEL_TEST(builtin_owned_transfer_1388, pop_explicit_dispose_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  var x := s.Pop(); "
        "  x.Dispose(); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// `move x` into another owning collection discharges — no warning.
BEZEL_TEST(builtin_owned_transfer_1388, pop_move_into_collection_no_warn_spec_28_11) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  using var s2 := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  var x := s.Pop(); "
        "  s2.Push(move x); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// ===========================================================================
// (d) CONTROL (SEMA) — a BORROW-returning builtin call is NOT an owned
// producer: no auto-drop warning (disposing it would double-free).
// ===========================================================================

// OwningStack.Peek(): T is a BORROW of the top element — no warning.
BEZEL_TEST(builtin_owned_transfer_1388, owning_stack_peek_borrow_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  var t := s.Peek(); "  // borrow — not owned
        "  return 0; "
        "end Main; end C;", "t"));
}

// A BORROWING Stack.Pop() returns a borrow (the element is owned elsewhere) —
// the receiver is `Stack`, not `OwningStack`, so isBuiltinOwnedTransferCall's
// receiver guard excludes it: no warning.
BEZEL_TEST(builtin_owned_transfer_1388, borrowing_stack_pop_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var owner := new R(1); "
        "  using var s := new Stack<R>(); "
        "  s.Push(owner); "
        "  var top := s.Pop(); "  // borrow — owned by `owner`
        "  return 0; "
        "end Main; end C;", "top"));
}

// #1388 soundness-review regression: the FOUR positional/keyed transfer names
// (PopLast/PopFirst/PopAt/PopValue) on a BORROWING collection return borrows —
// the receiver is a plain `Array`/`Dictionary`, not `Owning*`. Before the
// isOwningCollectionHead gate they were classified Owned unconditionally, so
// the §28.3 auto-drop disposed a borrow → double-free / SEGV. Each must NOT
// warn (and the AOT control below proves each does NOT double-free).
BEZEL_TEST(builtin_owned_transfer_1388, borrowing_array_poplast_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var owner := new R(1); "
        "  using var arr := new Array<R>(); "
        "  arr.Add(owner); "
        "  var x := arr.PopLast(); "  // borrow — owned by `owner`
        "  return 0; "
        "end Main; end C;", "x"));
}

BEZEL_TEST(builtin_owned_transfer_1388, borrowing_array_popfirst_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var owner := new R(1); "
        "  using var arr := new Array<R>(); "
        "  arr.Add(owner); "
        "  var x := arr.PopFirst(); "
        "  return 0; "
        "end Main; end C;", "x"));
}

BEZEL_TEST(builtin_owned_transfer_1388, borrowing_array_popat_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var owner := new R(1); "
        "  using var arr := new Array<R>(); "
        "  arr.Add(owner); "
        "  var x := arr.PopAt(0); "
        "  return 0; "
        "end Main; end C;", "x"));
}

BEZEL_TEST(builtin_owned_transfer_1388, borrowing_dict_popvalue_no_warn_spec_28_3) {
    BEZEL_EXPECT_FALSE(warnsAutoDrop(
        "class C public static method Main(): int "
        "  using var owner := new R(1); "
        "  using var d := new Dictionary<int, R>(); "
        "  d.Add(1, owner); "
        "  var x := d.PopValue(1); "
        "  return 0; "
        "end Main; end C;", "x"));
}

// ===========================================================================
// (b) AOT + ASan/LSan — the never-discharged case is now LEAK-CLEAN, and the
// auto-drop disposes exactly once (value-observable via a `do`-block).
// ===========================================================================

// Value: the popped element is disposed at the `do`-block scope end → R._n == 1.
BEZEL_TEST(builtin_owned_transfer_1388, pop_autodrop_disposes_at_scope_end_28_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; " + kR +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  do "
        "    var x := s.Pop(); "  // auto-dropped at end of this block
        "  end; "
        "  return R._n; "  // 1 = disposed exactly once
        "end Main; end C;"), 1);
}

// AOT + ASan: the exact #1388 repro — never-discharged Pop → leak GONE.
BEZEL_TEST(builtin_owned_transfer_1388, asan_pop_never_discharged_no_leak_28_3) {
    int rc = runUnderASan(
        "module M; " + kR +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  var x := s.Pop(); "  // never discharged — was a silent leak
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no leak)
}

// AOT + ASan: never-discharged Dequeue → leak GONE.
BEZEL_TEST(builtin_owned_transfer_1388, asan_dequeue_never_discharged_no_leak_28_3) {
    int rc = runUnderASan(
        "module M; " + kR +
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<R>(); "
        "  q.Enqueue(move (new R(1))); "
        "  var x := q.Dequeue(); "
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// AOT + ASan: never-discharged TakeCurrent (drain a 2-element owning source) →
// leak GONE for every taken element.
BEZEL_TEST(builtin_owned_transfer_1388, asan_takecurrent_never_discharged_no_leak_29_1) {
    int rc = runUnderASan(
        "module M; " + kR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    while en.MoveNext() do "
        "      var x := en.TakeCurrent(); "  // never discharged — auto-dropped
        "    end; "
        "    return 0; "
        "  end Main; end C;");
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// AOT + ASan CONTROL: OwningStack.Peek() borrow into a plain var, never
// disposed — must stay leak-clean and double-free-clean (the element is
// disposed once by the stack, NOT by the borrow).
BEZEL_TEST(builtin_owned_transfer_1388, asan_owning_stack_peek_borrow_clean_28_3) {
    int rc = runUnderASan(
        "module M; " + kR +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R(1))); "
        "  var t := s.Peek(); "  // borrow — stack still owns the element
        "  return t.v; "
        "end Main; end C;");
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// AOT + ASan REGRESSION (the soundness-review refutation): a borrowing
// Array<R>.PopLast() bound to a plain var and never discharged. Before the
// isOwningCollectionHead gate this classified the borrow Owned and the §28.3
// auto-drop double-freed it (SEGV under ASan). Now: the borrow is not owned,
// not disposed; `owner` is disposed exactly once by its own using var. Must be
// double-free-clean AND leak-clean. Covers PopFirst/PopAt/PopValue by the same
// codegen path.
BEZEL_TEST(builtin_owned_transfer_1388, asan_borrowing_array_poplast_no_double_free_28_3) {
    int rc = runUnderASan(
        "module M; " + kR +
        "class C public static method Main(): int "
        "  using var owner := new R(1); "
        "  using var arr := new Array<R>(); "
        "  arr.Add(owner); "
        "  var x := arr.PopLast(); "  // borrow — was double-freed pre-fix
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = no double-free, no leak (was SEGV)
}

// AOT + ASan CONTROL: a borrowing Dictionary<int,R>.PopValue(k) — same guard,
// same no-double-free requirement.
BEZEL_TEST(builtin_owned_transfer_1388, asan_borrowing_dict_popvalue_no_double_free_28_3) {
    int rc = runUnderASan(
        "module M; " + kR +
        "class C public static method Main(): int "
        "  using var owner := new R(1); "
        "  using var d := new Dictionary<int, R>(); "
        "  d.Add(1, owner); "
        "  var x := d.PopValue(1); "
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 0);
}
