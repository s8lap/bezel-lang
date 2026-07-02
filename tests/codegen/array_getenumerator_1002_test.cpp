// Codegen — #1002 / §29.3 / §29.11 / stdlib §4.4/§4.12: the manual enumerator
// protocol on an `Array<T>.GetEnumerator()` (and `OwningArray<T>.GetEnumerator()`)
// result.
//
// Root cause (pre-fix): `Array<T>.GetEnumerator()` returns `owned
// IEnumerator<T>` per the stdlib, but sema did not type its result as the
// builtin IEnumerator<T> and codegen had no dedicated lowering — so the #494
// manual-enumerator machinery did not fire:
//   A: an inferred `var ok := arr.GetEnumerator().MoveNext()` defaulted the slot
//      to i32, and a subsequent `if ok` / conditional emitted `br i32`, which the
//      LLVM IR verifier rejects ("Branch condition is not 'i1' type") — IR_FAIL.
//   B: an explicit `var ok: bool := e.MoveNext()` verified but MoveNext returned
//      false on a NON-empty array (the #494 slot-2 vtable dispatch did not fire),
//      so the program silently returned the wrong value — WRONG_VALUE.
//
// Fix:
//   - sema (ExprTypecheck_Calls): type Array<T>/OwningArray<T> GetEnumerator() as
//     the builtin IEnumerator<T> sentinel, so the #494 MoveNext-bool / Current-T
//     typing fires.
//   - codegen (LlvmBackend_Calls): lower the call by reusing the #943
//     array-wrapper machinery — wrap the array as the iterator-vtable IEnumerable
//     object ([0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current,[4]=GetEnumerator) then
//     call slot 4 to hand back a fresh independent cursor enumerator (and free the
//     throwaway wrapper). MoveNext (slot 2) / Current (slot 3) then dispatch via
//     the #494 path; Dispose (slot 1) frees only the wrapper struct, BORROWING the
//     array, so a separate `using var arr` is disposed exactly once (§28.2).
//   - codegen (LlvmBackend_Setup, registerUsingDisposal): recognise an
//     IEnumerator<T>/IOwningEnumerator<T>-typed `using var` and dispose it via the
//     iterator slot-1 path (§29.7/§29.11), so the enumerator struct does not leak.
//
// These tests run the FULL pipeline (including the ownership pass) so the
// using-var scope-end disposals are emitted and exercised; a leak or double-free
// of an owned element surfaces as a wrong Cell.n counter (the owned-element test
// returns net Cell.n, which must be 0). Independently ASan-verified in the lane.

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {
// Full-pipeline runner (mirrors iterator_reenum_943_test): runs the ownership
// pass before codegen so scope-end / using-var disposals are emitted and
// exercised, then JIT-executes `main` and returns its value.
int run(const std::string& src) {
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
    auto fn = sym->toPtr<int (*)()>();
    return fn();
}

const char* kImport = "module M; import System.Collections.Generic; ";
const char* kCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";
}  // namespace

// ---- A: Repro A — inferred `var ok := e.MoveNext()` (was IR_FAIL br i32) -------
// §29.3: GetEnumerator() now types as IEnumerator<int>, so MoveNext() is bool and
// the inferred slot is i1; `1 if ok else 0` produces valid IR. MoveNext is true on
// a non-empty array → 1.
BEZEL_TEST(array_getenumerator_1002, repro_a_inferred_movenext_spec_29_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) +
        "class P public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(5); arr.Add(6); "
        "  using var e := arr.GetEnumerator(); "
        "  var ok := e.MoveNext(); "
        "  return 1 if ok else 0; "
        "end Main; end P;"), 1);
}

// ---- B: Repro B — explicit `var ok: bool := e.MoveNext()` (was WRONG_VALUE) ----
// §29.3: the #494 slot-2 vtable dispatch now fires, so MoveNext returns TRUE on a
// non-empty array → 1 (was 0 before the fix).
BEZEL_TEST(array_getenumerator_1002, repro_b_explicit_bool_movenext_spec_29_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) +
        "class P public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(5); arr.Add(6); "
        "  using var e := arr.GetEnumerator(); "
        "  var ok: bool := e.MoveNext(); "
        "  return 1 if ok else 0; "
        "end Main; end P;"), 1);
}

// ---- C: full manual drain loop ------------------------------------------------
// §29.3 / §29.11: drive the enumerator with MoveNext (slot 2) + Current (slot 3),
// summing every element. 5 + 6 + 7 = 18.
BEZEL_TEST(array_getenumerator_1002, full_manual_drain_loop_spec_29_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) +
        "class P public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(5); arr.Add(6); arr.Add(7); "
        "  using var e := arr.GetEnumerator(); "
        "  var sum := 0; "
        "  while e.MoveNext() do sum := sum + e.Current; end; "
        "  return sum; "
        "end Main; end P;"), 18);
}

// ---- D: exhaustion — MoveNext past the end returns false ----------------------
// §29.3: one true MoveNext on a single-element array, then every subsequent
// MoveNext is false. c = 1 (only the first branch fires).
BEZEL_TEST(array_getenumerator_1002, movenext_past_end_is_false_spec_29_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) +
        "class P public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(5); "
        "  using var e := arr.GetEnumerator(); "
        "  var c := 0; "
        "  if e.MoveNext() then c := c + 1; end; "
        "  if e.MoveNext() then c := c + 100; end; "
        "  if e.MoveNext() then c := c + 100; end; "
        "  return c; "
        "end Main; end P;"), 1);
}

// ---- E: for-in over the same array still works (no regression) -----------------
// §29.2: the direct-indexed for-in path is unaffected; for-in then a manual drain
// over the SAME array both produce 60 → 120 (no double-disposal between them).
BEZEL_TEST(array_getenumerator_1002, for_in_then_manual_no_regression_spec_29_2) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) +
        "class P public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(10); arr.Add(20); arr.Add(30); "
        "  var f := 0; for var x in arr do f := f + x; end; "
        "  using var e := arr.GetEnumerator(); "
        "  var m := 0; while e.MoveNext() do m := m + e.Current; end; "
        "  return f + m; "
        "end Main; end P;"), 120);
}

// ---- F: owned-element dispose-once (OwningArray<Cell>) --------------------------
// §28.2 / §4.3: the enumerator BORROWS the OwningArray — its slot-1 Dispose frees
// only the wrapper struct, NOT the cells. The OwningArray disposes each owned Cell
// exactly once at its own scope end. Net Cell.n == 0 (no double-free, no leak).
// The drain reads each Cell.V (= 7) through the borrowing IEnumerable<T> view
// (Current is non-destructive here, §4.3), so the sum is 21 while alive; the test
// returns net Cell.n after both using-vars exit, which must be 0.
BEZEL_TEST(array_getenumerator_1002, owned_array_enum_dispose_once_spec_28_2) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) + kCell +
        "class P public static method Main(): int "
        "  do "
        "    using var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell()); arr.Add(new Cell()); arr.Add(new Cell()); "
        "    using var e := arr.GetEnumerator(); "
        "    var sum := 0; "
        "    while e.MoveNext() do sum := sum + e.Current.V; end; "
        "  end; "
        "  return Cell.n; "
        "end Main; end P;"), 0);
}

// ---- G: owned-element drain value while alive ---------------------------------
// §4.3: borrowing iteration over OwningArray<Cell> yields borrows of each Cell;
// reading Current.V three times sums 21 (3 × 7) before disposal.
BEZEL_TEST(array_getenumerator_1002, owned_array_drain_value_spec_4_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) + kCell +
        "class P public static method Main(): int "
        "  using var arr := new OwningArray<Cell>(); "
        "  arr.Add(new Cell()); arr.Add(new Cell()); arr.Add(new Cell()); "
        "  using var e := arr.GetEnumerator(); "
        "  var sum := 0; "
        "  while e.MoveNext() do sum := sum + e.Current.V; end; "
        "  return sum; "
        "end Main; end P;"), 21);
}

// ---- H: non-using `var e` with explicit `e.Dispose()` -------------------------
// §29.3 / §29.7: an explicit `e.Dispose()` on the builtin IEnumerator<T> sentinel
// dispatches the iterator slot-1 Dispose (frees the wrapper-clone struct; the
// borrowed array is untouched), so the enumerator does not leak. Value still 11.
BEZEL_TEST(array_getenumerator_1002, non_using_explicit_dispose_spec_29_7) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) +
        "class P public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(5); arr.Add(6); "
        "  var e := arr.GetEnumerator(); "
        "  var s := 0; "
        "  while e.MoveNext() do s := s + e.Current; end; "
        "  e.Dispose(); "
        "  return s; "
        "end Main; end P;"), 11);
}

// ---- I: using-var + explicit Dispose — exactly-once (no double-free) -----------
// §28.2: an explicit `e.Dispose()` nulls the receiver slot; the using-var
// scope-exit reloads null and skips, so the OwningArray's cells are disposed
// exactly once. Net Cell.n == 0 (a double-free would have crashed/under-counted).
BEZEL_TEST(array_getenumerator_1002, using_var_plus_explicit_dispose_once_spec_28_2) {
    BEZEL_EXPECT_EQ(run(
        std::string(kImport) + kCell +
        "class P public static method Main(): int "
        "  do "
        "    using var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell()); arr.Add(new Cell()); "
        "    using var e := arr.GetEnumerator(); "
        "    var s := 0; "
        "    while e.MoveNext() do s := s + e.Current.V; end; "
        "    e.Dispose(); "
        "  end; "
        "  return Cell.n; "
        "end Main; end P;"), 0);
}
