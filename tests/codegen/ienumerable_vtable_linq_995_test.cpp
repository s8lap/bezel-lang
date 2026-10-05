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

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

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
        "    return e.Sum(lambda(x) => x); "
        "  end Main; end P;"), 6);
}

// Any on a yield-method result → true (1) (was: false — wrong).
BEZEL_TEST(ienumerable_vtable_linq_995, any_on_yield_method_result) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return (if P.Seq().Any() then 1 else 0); "
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
        "    return a.Count; "
        "  end Main; end P;"), 3);
}

// ── A (feature): LINQ operators (Where/Select) and for-in over a vtable-backed
//    IEnumerable. Was: "not yet implemented: codegen for this for statement". ──

// for-in over Where on a yield-method result: sum of {2,3} = 5.
BEZEL_TEST(ienumerable_vtable_linq_995, for_in_over_where) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in P.Seq().Where(lambda(n) => n > 1) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end P;"), 5);
}

// for-in over Select on an interface-typed local: sum of {10,20,30} = 60.
BEZEL_TEST(ienumerable_vtable_linq_995, for_in_over_select) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    var e: IEnumerable<int> := P.Seq(); "
        "    var sum := 0; "
        "    for var x in e.Select(lambda(n) => n * 10) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end P;"), 60);
}

// Chained operator + terminal: Where then Count → 2 (the {2,3} that pass).
BEZEL_TEST(ienumerable_vtable_linq_995, where_then_count) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return P.Seq().Where(lambda(n) => n > 1).Count(); "
        "  end Main; end P;"), 2);
}

// Select then Sum → 60 (10+20+30).
BEZEL_TEST(ienumerable_vtable_linq_995, select_then_sum) {
    BEZEL_EXPECT_EQ(run(std::string(kSeq) +
        "  public static method Main(): int "
        "    return P.Seq().Select(lambda(n) => n * 10).Sum(lambda(x) => x); "
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

// #1067: an OwningArray<T> passed as an IEnumerable<T> PARAMETER and iterated
// through the interface (`for var a in xs`) must work. Pre-fix it SIGSEGV'd: the
// call-site argument coercion (LlvmBackend_Assign.cpp) wrapped only `Array<T>`,
// so the OwningArray's bz_array_t layout was passed raw and the callee's vtable
// dispatch called a null GetEnumerator slot. OwningArray shares the bz_array_t
// layout, so wrapArrayAsIEnumerable applies the same way; `Array<T>` (already
// covered) and direct OwningArray iteration are unaffected.
BEZEL_TEST(ienumerable_vtable_linq_995, owned_array_as_ienumerable_param_1067) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Item>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var items: OwningArray<Item> := [new Item(3), new Item(4)]; "
        "    return Sum(items); "
        "  end Main; end C;"), 7);
}

// #1067 disposal: the IEnumerable wrapper SHARES the array (does not own it), so
// after iterating-as-IEnumerable (twice) and disposing the OwningArray, its owned
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
        "    do using var items: OwningArray<Cell> := [new Cell(3), new Cell(4)]; "
        "       var t: int := Sum(items); var u: int := Sum(items); end; "
        "    return Cell.n; "
        "  end Main; end C;"), 0);
}

// #1068: the #1067 fix extends to every DENSE bz_array_t-backed collection —
// Stack/OwningStack, Queue/OwningQueue, OwningBuffer (the set lowerArrayFor iterates
// directly). Each, passed as an IEnumerable<T> parameter and iterated, must work
// (each previously SIGSEGV'd). Set/OwningSet and LinkedList are NOT array-dense
// and stay out (#1069).
namespace {
std::string ifaceParamSum(const std::string& collDecl) {
    return
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Item>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var c: " + collDecl + " := [new Item(3), new Item(4)]; "
        "    return Sum(c); "
        "  end Main; end C;";
}
}  // namespace

BEZEL_TEST(ienumerable_vtable_linq_995, owned_stack_as_ienumerable_param_1068) {
    BEZEL_EXPECT_EQ(run(ifaceParamSum("OwningStack<Item>")), 7);
}
BEZEL_TEST(ienumerable_vtable_linq_995, owned_queue_as_ienumerable_param_1068) {
    BEZEL_EXPECT_EQ(run(ifaceParamSum("OwningQueue<Item>")), 7);
}
// #1068, RE-POINTED by #2914 (§15.4, v0.240). This case used
// `ifaceParamSum("OwningBuffer<Item>")`, i.e. it built the buffer from a
// COLLECTION LITERAL — `using var c: OwningBuffer<Item> := [new Item(3), …];`.
// §15.4's list of "target types that admit collection literals" is explicit and
// closed, and `OwningBuffer<T>` is NOT on it: inline arrays, `Array`/`OwningArray`,
// `Set`/`OwningSet`, `LinkedList`/`OwningLinkedList`, `Stack`/`OwningStack`,
// `Queue`/`OwningQueue`, `Dictionary`/`OwningDictionary`, and user types
// declaring `FromCollectionLiteral`. The old gate was a NEGATIVE list
// (`isCollectionLiteralInterfaceTarget`) that admitted this target by omission;
// #2914 replaced it with §15.4's positive list, which correctly rejects it.
//
// So this case was pinning a spelling the language does not admit. What it is
// FOR — that an `OwningBuffer` reaches `IEnumerable<T>` dispatch and iterates —
// is unchanged and still worth pinning, so the construction is re-pointed at
// stdlib §4.14's real two-argument constructor rather than deleted. Verified
// equivalent: both spellings produced 7 before #2914.
//
// ⚠ MAINTENANCE: if §15.4 ever adds `OwningBuffer<T>` to its admitted targets,
// this case may go back to the literal spelling — but do NOT change it merely
// because a future gate accepts the literal again; check §15.4 first, since an
// accepting gate is exactly what this re-point was needed to correct.
BEZEL_TEST(ienumerable_vtable_linq_995, owned_buffer_as_ienumerable_param_1068) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Item>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var c := new OwningBuffer<Item>(2, "
        "        lambda(i: int) => new Item(i + 3)); "
        "    return Sum(c); "
        "  end Main; end C;"), 7);
}
BEZEL_TEST(ienumerable_vtable_linq_995, borrowing_stack_as_ienumerable_param_1068) {
    BEZEL_EXPECT_EQ(run(ifaceParamSum("Stack<Item>")), 7);
}

// #1068 disposal: an OwningStack iterated through IEnumerable (twice) still
// disposes its owned elements exactly once (the wrapper shares the collection).
BEZEL_TEST(ienumerable_vtable_linq_995, owned_stack_as_ienumerable_disposed_once_1068) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cell public static n: int := 0; public V: int := 0; "
        "  public constructor(v: int) this.V := v; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Cell>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    do using var c: OwningStack<Cell> := [new Cell(3), new Cell(4)]; "
        "       var t: int := Sum(c); var u: int := Sum(c); end; "
        "    return Cell.n; "
        "  end Main; end C;"), 0);
}

// #1069: a node-based (Owned)LinkedList<T> passed as an IEnumerable<T> param and
// iterated must work — via the node-walking wrapper (wrapLinkedListAsIEnumerable),
// since a LinkedList is bz_llist-backed, not array-backed (it was SIGSEGV before).
BEZEL_TEST(ienumerable_vtable_linq_995, owned_linkedlist_as_ienumerable_param_1069) {
    BEZEL_EXPECT_EQ(run(ifaceParamSum("OwningLinkedList<Item>")), 7);
}
BEZEL_TEST(ienumerable_vtable_linq_995, borrowing_linkedlist_as_ienumerable_param_1069) {
    BEZEL_EXPECT_EQ(run(ifaceParamSum("LinkedList<Item>")), 7);
}

// #1069: re-enumerating a LinkedList through IEnumerable replays from the head
// (GetEnumerator resets the cursor) — two Sum() calls each see all elements.
BEZEL_TEST(ienumerable_vtable_linq_995, linkedlist_as_ienumerable_reenumerates_1069) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Item>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var c: OwningLinkedList<Item> := [new Item(3), new Item(4)]; "
        "    var a: int := Sum(c); var b: int := Sum(c); return a * 10 + b; "
        "  end Main; end C;"), 77);
}

// #1069 disposal: an OwningLinkedList iterated through IEnumerable (twice) disposes
// its owned elements exactly once (the wrapper shares the list — no double-free).
BEZEL_TEST(ienumerable_vtable_linq_995, owned_linkedlist_as_ienumerable_disposed_once_1069) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cell public static n: int := 0; public V: int := 0; "
        "  public constructor(v: int) this.V := v; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Cell>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    do using var c: OwningLinkedList<Cell> := [new Cell(3), new Cell(4)]; "
        "       var t: int := Sum(c); var u: int := Sum(c); end; "
        "    return Cell.n; "
        "  end Main; end C;"), 0);
}

// #1069: an EMPTY LinkedList through IEnumerable yields no elements and does not
// crash (exhaustion = next_node == null, handled on the first MoveNext).
BEZEL_TEST(ienumerable_vtable_linq_995, empty_linkedlist_as_ienumerable_param_1069) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Sum(xs: IEnumerable<Item>): int "
        "    var s: int := 0; for var a in xs do s := s + a.V; end; return s + 99; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var c: OwningLinkedList<Item> := []; return Sum(c); "
        "  end Main; end C;"), 99);
}

// #1177 / §4.5 / §12.1: a LOCAL variable declared as IEnumerable<T> and bound
// to an owning collection (OwningArray, OwningStack, OwningQueue, OwningLinkedList)
// must iterate correctly. Pre-fix: the local initializer wrapped only Array<T>,
// so OwningArray etc. stored the raw bz_array_t* or bz_llist* in the
// IEnumerable<T> slot; lowerIEnumerableBuiltinFor then read vtable slot 4 off
// the raw pointer (non-existent for owning collections) → SIGSEGV even on an
// empty collection. Fix in LlvmBackend_Setup.cpp: extend the local-initializer
// wrapping to all bz_array_t-backed collections (array wrapper) and to
// (Owned)LinkedList (node-walking wrapper), mirroring buildOrderedArgs.
namespace {
std::string ifaceLocalSum(const std::string& collDecl) {
    return
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Main(): int "
        "    using var c: " + collDecl + " := [new Item(3), new Item(4)]; "
        "    var e: IEnumerable<Item> := c; "
        "    var s: int := 0; for var a in e do s := s + a.V; end; return s; "
        "  end Main; end C;";
}
}  // anonymous namespace

// §4.5 / §12.1 / #1177: empty OwningArray via IEnumerable local — no crash (was
// SIGSEGV on HEAD before fix; the vtable-slot-4 read fired even on 0 elements).
BEZEL_TEST(ienumerable_vtable_linq_995,
           empty_owned_array_as_ienumerable_local_no_crash_1177) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Item public V: int := 0; "
        "  public constructor(v: int) this.V := v; end constructor; end Item; "
        "class C "
        "  public static method Main(): int "
        "    using var c := new OwningArray<Item>(); "
        "    var e: IEnumerable<Item> := c; "
        "    var s: int := 0; for var a in e do s := s + a.V; end; return s + 99; "
        "  end Main; end C;"), 99);
}

// §4.5 / §12.1 / #1177: OwningArray<Item> bound to IEnumerable<Item> local —
// for-in sees all elements and sums correctly (was SIGSEGV).
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_array_as_ienumerable_local_sum_1177) {
    BEZEL_EXPECT_EQ(run(ifaceLocalSum("OwningArray<Item>")), 7);
}

// §4.5 / §12.1 / #1177: other bz_array_t-backed owning collections via
// IEnumerable<T> local — same fix, same wrapper (wrapArrayAsIEnumerable).
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_stack_as_ienumerable_local_sum_1177) {
    BEZEL_EXPECT_EQ(run(ifaceLocalSum("OwningStack<Item>")), 7);
}
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_queue_as_ienumerable_local_sum_1177) {
    BEZEL_EXPECT_EQ(run(ifaceLocalSum("OwningQueue<Item>")), 7);
}

// §4.5 / §12.1 / #1177: (Owned)LinkedList<T> via IEnumerable<T> local —
// uses wrapLinkedListAsIEnumerable (node-walking), mirroring buildOrderedArgs.
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_linkedlist_as_ienumerable_local_sum_1177) {
    BEZEL_EXPECT_EQ(run(ifaceLocalSum("OwningLinkedList<Item>")), 7);
}

// §4.5 / §12.1 / #1177 disposal: the IEnumerable wrapper SHARES the collection
// (does not own it), so after iterating via the local the OwningArray still
// disposes its elements exactly once at scope close (Cell.n net 0 — no leak,
// no double-free). Covers the ownership-safety invariant of the #1177 fix.
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_array_as_ienumerable_local_disposed_once_1177) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Cell public static n: int := 0; public V: int := 0; "
        "  public constructor(v: int) this.V := v; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C "
        "  public static method Main(): int "
        "    do using var items: OwningArray<Cell> := [new Cell(3), new Cell(4)]; "
        "       var e: IEnumerable<Cell> := items; "
        "       var s: int := 0; for var a in e do s := s + a.V; end; end; "
        "    return Cell.n; "
        "  end Main; end C;"), 0);
}

// §29.2 / §4.3 / #1310: a method RETURNING an owned OwningArray<T> field coerced
// to its `IEnumerable<T>` view was previously admitted under v0.221 (the
// wrapper that constructs the IEnumerable<T> from the field read was emitted
// at every return site; the receiver's destructor invalidated the storage on
// Holder's scope-end). §28.6 (v0.258, #3295) closed the stored-view case for
// the four non-owning iterable heads — the shape is REJECTED at the def site
// and the vtable-coercion runtime shape is no longer reachable. Converted
// from a runtime test (which pinned the v0.221 acceptance + the runtime
// fix for the SIGSEGV from the un-wrapped raw bz_array_t*) to a compile-
// failure test that pins the v0.258 rejection.
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_field_returned_as_ienumerable_bind_only_1310) {
    auto r = parseAndCheckOwnership(
        "module M; class Dog end Dog; "
        "class Holder "
        "  private owned arr: OwningArray<Dog>; "
        "  public constructor() arr := new OwningArray<Dog>(); end constructor; "
        "  public method Items(): IEnumerable<Dog> => arr; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

// §29.2 / §28.6 (v0.258, #3295): an owned field re-exposed through an
// unmarked mint-announcing method return is REJECTED — the receiver's
// destructor frees the field's storage, and the wrapper's caller iterates
// through the same storage past the receiver's lifetime (a use-after-free).
// The shape was admitted under v0.221; v0.258's stored-view exclusion closed
// it. Converted from a runtime test (which pinned the v0.221 acceptance) to a
// compile-failure test that pins the v0.258 rejection. The for-in vtable
// protocol is still exercised by the parameter-passthrough siblings below;
// the underlying `OwningArray<Cell> → IEnumerable<Cell>` wrapper remains
// exercised by `owned_field_returned_as_iowningenumerable_disposed_once_1310`
// in `owning_iterable_head_borrow_return_2369_test.cpp` (the owning-four
// sibling) — but for the non-owning four this exact shape is no longer
// legal under v0.258. // Migrated from OwningArray<int> → OwningArray<Cell>
// per stdlib §4.2/#1327.
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_field_returned_as_ienumerable_iterated_1310) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; end Cell; "
        "class Holder "
        "  private owned arr: OwningArray<Cell>; "
        "  public constructor() "
        "    arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell(3)); arr.Add(new Cell(4)); "
        "  end constructor; "
        "  public method Items(): IEnumerable<Cell> => arr; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

// §28.6 (v0.258, #3295): the explicit-ReturnStmt path (`return arr;` body)
// hits the same stored-view exclusion as the `=> arr` body above.
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_field_returned_as_ienumerable_return_stmt_1310) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; end Cell; "
        "class Holder "
        "  private owned arr: OwningArray<Cell>; "
        "  public constructor() "
        "    arr := new OwningArray<Cell>(); arr.Add(new Cell(5)); "
        "  end constructor; "
        "  public method Items(): IEnumerable<Cell> "
        "    return arr; "
        "  end Items; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

// §28.6 (v0.258, #3295): the disposal-tracking variant of the same shape —
// the receiver's destructor frees the field; the wrapper's caller iterates
// through the same storage past the receiver's lifetime. Pin the rejection.
BEZEL_TEST(ienumerable_vtable_linq_995,
           owned_field_returned_as_ienumerable_disposed_once_1310) {
    auto r = parseAndCheckOwnership(
        "module M; class Dog public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; end Dog; "
        "class Holder "
        "  private owned arr: OwningArray<Dog>; "
        "  public constructor() "
        "    arr := new OwningArray<Dog>(); arr.Add(new Dog()); arr.Add(new Dog()); "
        "  end constructor; "
        "  public method Items(): IEnumerable<Dog> => arr; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a stored view is re-exposed through the mint-announcing"));
}

// §12.9 / §29.2 / stdlib §4.4 / #1316: a METHOD-LEVEL-GENERIC method returning
// `Array<T>` coerced to its `IEnumerable<T>` view must wrap the array into the
// vtable-backed enumerator — the generic sibling of the #1310 return-coercion.
// Root cause: a method-level generic (`AsSeq<T>`) is specialized via
// emitInlineGenericMethod, whose EXPRESSION-body path inlined `=> a` RAW (it
// never reaches buildSpecializedMethodFn — only block bodies do), so the raw
// bz_array_t* pointer was returned; the caller's for-in then dispatched through
// the array's first word as a vtable (null-fn-pointer call) and SIGSEGV'd. Fix
// in LlvmBackend_Calls2.cpp: emitInlineGenericMethod's expression-body path now
// applies the same boxForTarget + wrapAsIEnumerableForTarget return-coercion as
// buildSpecializedMethodFn. Array<T> implements IEnumerable<T> via IList<T>
// (stdlib §4.4); the for desugaring (§29.2) drives the wrapper's vtable
// protocol; T is substituted under the active genericSubst_ (§12.9) so the
// enumerator's element type/stride matches the instantiation.
BEZEL_TEST(ienumerable_vtable_linq_995,
           generic_method_array_as_ienumerable_iterated_1316) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Util "
        "  public static method AsSeq<T>(a: Array<T>): IEnumerable<T> => a; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 4]; "
        "    var s := 0; "
        "    for var x in Util.AsSeq<int>(xs) do s := s + x; end; "
        "    return s; "
        "  end Main; end C;"), 7);
}

// §29.11 / §29.2 / #1316: re-enumerating the generic-returned IEnumerable<T>
// replays from the start (GetEnumerator slot 4 clones the cursor to idx=-1),
// so two for-in loops over the same view each see all three elements
// (7*10+7 = 77). Pins that the wrapper (not the raw array) is what reaches the
// caller: a raw bz_array_t* has no GetEnumerator slot and would SIGSEGV on the
// second enumeration just as on the first.
BEZEL_TEST(ienumerable_vtable_linq_995,
           generic_method_array_as_ienumerable_reenumerates_1316) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Util "
        "  public static method AsSeq<T>(a: Array<T>): IEnumerable<T> => a; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 4]; "
        "    var e := Util.AsSeq<int>(xs); "
        "    var s1 := 0; for var x in e do s1 := s1 + x; end; "
        "    var s2 := 0; for var y in e do s2 := s2 + y; end; "
        "    return s1 * 10 + s2; "
        "  end Main; end C;"), 77);
}

// §12.9 / §29.2 / #1316: a reference element type (string = ptr, stride 8)
// substituted for T exercises the genericSubst_-driven element-type/stride
// selection in wrapArrayAsIEnumerable. Sum of "ab"(2)+"cde"(3)+"f"(1) = 6.
BEZEL_TEST(ienumerable_vtable_linq_995,
           generic_method_string_array_as_ienumerable_1316) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Util "
        "  public static method AsSeq<T>(a: Array<T>): IEnumerable<T> => a; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var xs: Array<string> := [\"ab\", \"cde\", \"f\"]; "
        "    var n := 0; "
        "    for var x in Util.AsSeq<string>(xs) do n := n + x.Length; end; "
        "    return n; "
        "  end Main; end C;"), 6);
}
