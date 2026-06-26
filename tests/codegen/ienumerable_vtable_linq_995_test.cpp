// Codegen — issue #995 / stdlib §5 / §29.11: LINQ terminals and operators, and
// for-in, over a VTABLE-BACKED `IEnumerable<T>` receiver — an interface-typed
// local (`var e: IEnumerable<int> := P.Seq()`) or a yield-method result
// (`P.Seq()`). These receivers carry the iterator-vtable object, not the lazy
// sentinel and not a bz_array_t*.
//
// Root cause (pre-fix): isLazyEnumerableType recognised only the lazy sentinel
// (declaration-less, NO astNode, NO importedName) and excluded the vtable-backed
// IEnumerable (which carries the interface name in an astNode / importedName). So
// a terminal (Count/Sum/Any/First/ToArray) on such a receiver silently fell
// through to a wrong dispatch path and returned GARBAGE (a non-deterministic
// wrong value, not a clean error — the #995 soundness defect), and a LINQ
// operator (Where/Select/…) was left untyped so the for-in over it reported
// "not yet implemented".
//
// Fix: isVtableBackedIEnumerableType recognises the receiver, and
// lowerToLazyEnumerator (via materializeVtableIEnumerableToArray) drives the
// iterator vtable protocol once (GetEnumerator slot 4 → MoveNext slot 2 → Current
// slot 3 → Dispose slot 1) to materialize the elements into a fresh OWNED Array<T>
// (bz_lazy_from_owned_array: the lazy source frees the array on dispose, leak-
// free). The terminal/operator/for-in machinery then composes uniformly. Sema
// types the deferred LINQ operators on this receiver as the lazy sentinel so the
// for-in / chained operators are typed.
//
// Disposal is exactly-once: the per-consumption enumerator (the GetEnumerator
// clone) is disposed by the bridge; the source IEnumERABLE is disposed at its own
// scope end (named local/param) or by the bridge (a fresh temporary). This is
// verified under ASan with a Cell static counter in the lane's manual
// verification (net Cell.n == 0, no double-free/UAF).

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
// Full-pipeline runner (runs the ownership pass before codegen so scope-end /
// loop disposals are emitted), then JIT-executes `main` and returns its value.
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

// A yield iterator method `Seq(): IEnumerable<int>` producing 1, 2, 3 — the
// canonical vtable-backed IEnumerable<T> producer.
const char* kSeq =
    "module M; import System.Collections.Generic; class P "
    "  public static method Seq(): IEnumerable<int> "
    "    yield return 1; yield return 2; yield return 3; "
    "  end Seq; ";
}  // namespace

// ── B (soundness): terminals on a vtable-backed IEnumerable return the CORRECT
//    value (was: garbage). The interface-typed-local form and the direct
//    yield-method-result form. stdlib §5. ────────────────────────────────────

// var e: IEnumerable<int> := P.Seq(); e.Count() → 3 (was 0).
BEZEL_TEST(ienumerable_vtable_linq_995, count_on_interface_typed_local) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var e: IEnumerable<int> := P.Seq(); "
        "    return e.Count(); "
        "  end Main; end P;"), 3);
}

// P.Seq().Count() → 3 directly on the yield-method result (a temporary; its
// source is disposed by the bridge).
BEZEL_TEST(ienumerable_vtable_linq_995, count_on_yield_method_result) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return P.Seq().Count(); "
        "  end Main; end P;"), 3);
}

// The exact #995 garbage repro: var c := e.Count(); return c*100+7. With the
// correct Count() == 3 this is 307 (was a non-deterministic wrong value).
BEZEL_TEST(ienumerable_vtable_linq_995, count_composed_arithmetic) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var e: IEnumerable<int> := P.Seq(); "
        "    var c := e.Count(); "
        "    return c * 100 + 7; "
        "  end Main; end P;"), 307);
}

// Sum on an interface-typed local → 6 (1+2+3).
BEZEL_TEST(ienumerable_vtable_linq_995, sum_on_interface_typed_local) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var e: IEnumerable<int> := P.Seq(); "
        "    return e.Sum((x) => x); "
        "  end Main; end P;"), 6);
}

// Any on a yield-method result → true (1) (was: false — wrong).
BEZEL_TEST(ienumerable_vtable_linq_995, any_on_yield_method_result) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return 1 if P.Seq().Any() else 0; "
        "  end Main; end P;"), 1);
}

// First → 1 (the first yielded element).
BEZEL_TEST(ienumerable_vtable_linq_995, first_on_yield_method_result) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return P.Seq().First(); "
        "  end Main; end P;"), 1);
}

// ToArray materializes to a concrete Array<int> whose Length is 3.
BEZEL_TEST(ienumerable_vtable_linq_995, toarray_length) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var a := P.Seq().ToArray(); "
        "    return a.Length; "
        "  end Main; end P;"), 3);
}

// ── A (feature): LINQ operators (Where/Select) and for-in over a vtable-backed
//    IEnumerable. Was: "not yet implemented: codegen for this for statement". ──

// for-in over Where on a yield-method result: sum of {2,3} = 5.
BEZEL_TEST(ienumerable_vtable_linq_995, for_in_over_where) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in P.Seq().Where((n) => n > 1) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end P;"), 5);
}

// for-in over Select on an interface-typed local: sum of {10,20,30} = 60.
BEZEL_TEST(ienumerable_vtable_linq_995, for_in_over_select) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var e: IEnumerable<int> := P.Seq(); "
        "    var sum := 0; "
        "    for var x in e.Select((n) => n * 10) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end P;"), 60);
}

// Chained operator + terminal: Where then Count → 2 (the {2,3} that pass).
BEZEL_TEST(ienumerable_vtable_linq_995, where_then_count) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return P.Seq().Where((n) => n > 1).Count(); "
        "  end Main; end P;"), 2);
}

// Select then Sum → 60 (10+20+30).
BEZEL_TEST(ienumerable_vtable_linq_995, select_then_sum) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return P.Seq().Select((n) => n * 10).Sum((x) => x); "
        "  end Main; end P;"), 60);
}

// ── Soundness: re-enumerating the same interface-typed local replays (it is the
//    IEnumERABLE, not an exhausted enumerator) — #943 invariant preserved through
//    the materialize bridge. Two Count() calls each see all three elements. ────
BEZEL_TEST(ienumerable_vtable_linq_995, reenumeration_replays) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var e: IEnumerable<int> := P.Seq(); "
        "    var a := e.Count(); "
        "    var b := e.Count(); "
        "    return a * 10 + b; "  // 3,3 → 33
        "  end Main; end P;"), 33);
}

// #1067: an OwnedArray<T> passed as an IEnumerable<T> PARAMETER and iterated
// through the interface (`for var a in xs`) must work. Pre-fix it SIGSEGV'd: the
// call-site argument coercion (LlvmBackend_Assign.cpp) wrapped only `Array<T>`,
// so the OwnedArray's bz_array_t layout was passed raw and the callee's vtable
// dispatch called a null GetEnumerator slot. OwnedArray shares the bz_array_t
// layout, so wrapArrayAsIEnumerable applies the same way; `Array<T>` (already
// covered) and direct OwnedArray iteration are unaffected.
BEZEL_TEST(ienumerable_vtable_linq_995, owned_array_as_ienumerable_param_1067) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Item>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var items: OwnedArray<Item> := [new Item(3), new Item(4)]; "
        "    return Sum(items); "
        "  end Main; end C;"), 7);
}

// #1067 disposal: the IEnumerable wrapper SHARES the array (does not own it), so
// after iterating-as-IEnumerable (twice) and disposing the OwnedArray, its owned
// elements are disposed EXACTLY once (Cell.n net 0 — no leak, no double-free).
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_array_as_ienumerable_param_disposed_once_1067) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cell public static n: int := 0; public V: int := 0; "
        "  public constructor(v: int) this.V := v; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Cell>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    do using var items: OwnedArray<Cell> := [new Cell(3), new Cell(4)]; "
        "       var t: int := Sum(items); var u: int := Sum(items); end; "
        "    return Cell.n; "
        "  end Main; end C;"), 0);
}
