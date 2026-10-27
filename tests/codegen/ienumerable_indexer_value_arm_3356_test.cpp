// Codegen / SIGSEGV — #3356 / §29.2 / §23.4: an EXPRESSION-bodied
// (`=> expr;`) INDEXER (a property with an `[i]` parameter list, §23.4)
// whose declared return type is `IEnumerable<T>` and whose body yields a
// NON-ARRAY-backed collection (LinkedList<T>, a user class implementing
// IEnumerable<T>, or any family NOT in the wrapArrayAsIEnumerable
// fast-path's Array/Stack/Queue/OwningArray/OwningBuffer/OwningStack/
// OwningQueue head-name set) lowered the raw collection pointer instead
// of a vtable-backed enumerator. Every consumer of the indexer read —
// a local-decl-init (`var it := obj[i];`), a direct for-in
// (`for var x in obj[i] do …`), an argument (`Sink(obj[i])`) — then drove
// §29.2's vtable dispatch on the raw collection descriptor and jumped
// through vtable slot 4 (GetEnumerator, NULL) → SIGSEGV (`pc 0x0`,
// exit 139, no leak — a HARD CRASH, not the silent-wrong-value family
// #1840 closed).
//
// #3355 (Lane E) fixed the BLOCK-BODIED property GETTER (the §23.3 shape
// without index parameters) by widening `wrapAsIEnumerableForTarget` and
// `wrapPropertyArrayAsIEnumerable` to also accept `IOwningEnumerable` as
// the target head. #2777 (Wave 12) fixed the EXPRESSION-BODIED property
// GETTER (§23.2 shape) by calling `wrapAsIEnumerableForTarget` from the
// EXPRESSION-BODIED emit site at LlvmBackend_Exprs.cpp ~5596 (mirroring
// the BLOCK form's RETURN-edge call). NEITHER covers the INDEXER
// emit site (`lowerIndexerGet`, LlvmBackend_Assign.cpp ~2148-2264,
// EXPRESSION-bodied inline path). The indexer shape is structurally
// distinct from the property GETTER shape — the indexer takes an `[i]`
// parameter list and inlines its body against `this`+indexer-params,
// whereas the property getter inline path is parameterized only by
// `this` — and the wrap call was never mirrored. A
// `LinkedList<int>`-yielding indexer therefore kept the pre-#2815 /
// pre-#2777 defect and crashed identically to #2815's
// `property P: IEnumerable<int> => _items;` over an Array-backed source.
//
// THE FIX (this commit). Two coordinated changes:
//
// 1. LlvmBackend_Assign.cpp `lowerIndexerGet`, EXPRESSION-bodied inline
//    path (~line 2233): add a `wrapPropertyArrayAsIEnumerable` call
//    AFTER the §8.6 box (line ~2216) and §7.2 nullable wrap (line ~2233)
//    have run, mirroring the property GETTER inline path's call verbatim:
//       result = wrapPropertyArrayAsIEnumerable(
//           result, ctx_.exprType(ix), getterExpr);
//    `wrapPropertyArrayAsIEnumerable` is the EXISTING helper the property
//    GETTER inline path already calls (LlvmBackend_Exprs.cpp ~5596); it
//    keys on `declTy->headName() == "IEnumerable"` and dispatches
//    IEnumerable sources through `wrapAsIEnumerableForTarget`, which
//    handles Array/Set/LinkedList/user-class-IEnumerable uniformly. The
//    wrap is composed AFTER `boxForTarget` and `wrapNullableGetterResult`
//    so a value-typed body yielding `IEnumerable<int>` is wrapped into a
//    vtable-backed enumerator after its nullable/box transformation has
//    run; the helper returns `v` unchanged when the declared type is not
//    a pure IEnumerable-family interface or the getter's type is not in
//    the wrap family, so the non-Enumerable indexers (#3352's
//    `property [i: int]: Object => i;`, the standard library's
//    `[i: int]: T`, etc.) are unaffected.
//
// 2. LlvmBackend_Stmts.cpp `lowerIEnumerableBuiltinFor`: extend the
//    `isArrayBacked` heuristic (which decides whether the for-in uses
//    the array-backed protocol — `bz_array_len` / `bz_array_ptr_at` — or
//    the vtable-backed protocol — slot-2 MoveNext / slot-3 Current /
//    slot-4 GetEnumerator) to recognize an `IndexExpr` iterable whose
//    static type is `IEnumerable<T>` / `IOwningEnumerable<T>` as needing
//    vtable dispatch. Pre-fix the heuristic only flipped `isArrayBacked`
//    to `false` for `CallExpr` / `NewExpr` / `IdentifierExpr` /
//    `MemberAccessExpr` sources (the property/field read arm at line
//    ~7334 keys on the symbol being a FieldSymbol or PropertySymbol and
//    the headName being an enumerable family); an `IndexExpr` fell
//    through to the array-backed protocol which read the wrapper's
//    vtable pointer as a `bz_array_t.data` and dereferenced past the
//    wrapper — measured as a global-buffer-overflow on
//    `bz$llenum$i32$vtable` (the wrapper's slot-0 = vtable, mis-read as
//    `data`, then `bz_array_ptr_at(vtable, 0, 8)` reads slot 5 of the
//    5-slot vtable).
//
// TEST PLAN. The HEADLINE test is the issue's repro — a class with an
// EXPRESSION-bodied INDEXER of declared type `IEnumerable<int>` whose
// body yields a `LinkedList<int>` field, iterated by a direct for-in
// over `obj[i]`. Pre-fix: pc 0x0 SIGSEGV (the indexer read returned the
// raw `bz_llist_t*`, the for-in read slot 4 of the llist descriptor —
// the count field, NOT a vtable pointer — and called it → SEGV).
// Post-fix: exit code 58 (the llist's only element × the loop's
// sum-to-exit-code mapping), ASan/LSan-clean.
//
// SIBLING DEFECTS LEFT OPEN (filed as follow-ups, NOT fixed here):
//   * BLOCK-BODIED INDEXER `get => do return coll; end;` over the SAME
//     LinkedList<int> source — the block form's RETURN-edge wrap
//     (`wrapAsIEnumerableForTarget`) fires but the wrapped enumerator
//     drives the for-in into a global-buffer-overflow on
//     `bz$llenum$i32$vtable`'s slot-5. The block form's wrap is
//     structurally different from the EXPRESSION form's
//     `wrapPropertyArrayAsIEnumerable` — it lands in a separate code
//     path. Filed as a sibling defect, out of #3356's scope.
//   * LOCAL-INIT `var it := obj[i];` followed by `for var x in it` —
//     `registerIterableSourceDisposal`'s `ownsSource` claim
//     (LlvmBackend_Setup.cpp ~13922) keys on the lowerLocalDecl wrap
//     arms (Array/Set/LinkedList/user-class) and the MemberAccessExpr
//     property-getter arm (#2815/#2777/#2779), but not on IndexExpr
//     sources. The wrapper-frame ownership is unclaimed and the
//     wrapper leaks at scope end. Filed as a sibling defect.
//   * ARGUMENT PASS `Sink(obj[i])` — same root cause as the local-init
//     case (the argument's classification doesn't recognize the
//     wrapped IndexExpr as needing vtable-frame disposal). Filed as a
//     sibling defect.
//
// runAotChecked links + runs under ASan/LSan; the SEGV on the pre-fix
// path is caught as a non-zero exit code (or ASan's "SEGV on unknown
// address 0x000000000000 (pc 0x0)"). The exit code pins the correct
// enumeration result, so a UAF in the wrapper would corrupt the yield
// values as well. // per §29.2, §23.4, §28.9 (issue #3356)

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

}  // namespace

// ---------------------------------------------------------------------------
// // per §29.2, §23.4 (issue #3356)
// ---------------------------------------------------------------------------

// HEADLINE: an EXPRESSION-bodied INDEXER of declared type `IEnumerable<int>`
// whose body yields a `LinkedList<int>` field (`_ll`). The llist source is
// the LinkedList-backed family the property GETTER inline path already wraps
// on its EXPRESSION form (#2777) via `wrapAsIEnumerableForTarget`'s
// LinkedList arm; the indexer inline path (LlvmBackend_Assign.cpp
// `lowerIndexerGet`, EXPRESSION-bodied branch ~2148-2264) had no equivalent
// wrap, so the slot was the raw `bz_llist_t*`. The for-in's vtable slot-4
// read the count field as a function pointer and called it → SIGSEGV.
// Pre-fix: pc 0x0 SIGSEGV. Post-fix: exit code 58, ASan/LSan-clean.
BEZEL_TEST(ienumerable_indexer_value_arm_3356,
           linked_list_element_indexer_for_in_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Holder "
        "  private owned _ll: LinkedList<int>; "
        "  public constructor() "
        "    _ll := new LinkedList<int>(); "
        "    _ll.AddLast(3); "
        "  end constructor; "
        "  public property [i: int]: IEnumerable<int> "
        "    => _ll; "
        "end Holder; "
        "class P public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var x in h[0] do total := total + x; end; "
        "  if total = 3 then return 58; end; "
        "  return 1; "
        "end Main; end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 58);
}
