// Regression tests for issue #2486 — a THIRD, distinct codegen path still
// iterated a `Stack<T>`/`OwningStack<T>` in physical (bottom-to-top /
// insertion) order instead of stdlib §4.11 "Iteration order" (v0.195)'s
// required top-to-bottom (pop) order: the IMPLICIT `Stack<T>` ->
// `IEnumerable<T>` coercion at a call ARGUMENT, a LOCAL-DECL initializer, or
// a method RETURN position.
//
// This was the third of three independently-wrong paths for the same
// collection:
//   - #2472 fixed `for x in stack do ... end` (LlvmBackend_Stmts.cpp's
//     `lowerArrayFor`, the direct-indexed-loop elision).
//   - #2471 fixed the explicit `.GetEnumerator()` call
//     (LlvmBackend_Calls2.cpp), adding the reverse machinery
//     (`ensureStackEnumMachinery` / `wrapStackAsIEnumerator`,
//     LlvmBackend_Assign.cpp) this fix reuses verbatim.
//   - THIS fix closes `Emitter::wrapAsIEnumerableForTarget`
//     (LlvmBackend_Assign.cpp ~3419) — used for the ARGUMENT and RETURN
//     coercion positions — and a SEPARATE, independent duplicate of the same
//     `actualIsArray` dispatch logic inside `lowerLocalDecl`
//     (LlvmBackend_Setup.cpp ~8598-8617) that handles the LOCAL-DECL
//     position. These are NOT one shared call site for all three positions,
//     contrary to what a first reading of #2486 (and #2471's own filing
//     comment) suggested — confirmed by direct probe: patching only
//     `wrapAsIEnumerableForTarget` left a local-decl coercion
//     (`var src: IEnumerable<int> := s;`) still forward-ordered. Both sites
//     are fixed here, identically, by routing Stack/OwningStack through
//     #2471's `wrapStackAsIEnumerator` instead of the forward
//     `wrapArrayAsIEnumerable` — no new machinery, per the coordinator's
//     "reuse #2471's own reverse machinery... do not write a third
//     implementation" direction. `wrapAsIEnumerableForTarget` DOES serve all
//     four RETURN call sites (LlvmBackend_Calls2.cpp x2, LlvmBackend_Stmts.cpp,
//     LlvmBackend_Setup.cpp's arrow-body path) plus the one ARGUMENT call
//     site (`buildOrderedArgs`) through the single shared function, so those
//     four positions are fixed by the one dispatch-point edit.
//
// Ownership: both edits are a plain dispatch swap onto the EXISTING
// #2471 machinery (`wrapStackAsIEnumerator`), which mirrors
// `wrapArrayAsIEnumerable`'s struct shape/Dispose contract field-for-field —
// a pure BORROW of the backing collection (never disposes it). No
// `outOwningWrap`/ownership behavior changes: the argument, local-decl, and
// return positions all already used (and still use) the borrowing wrap for
// Stack, exactly as they already did — and still do — for Array/Queue at
// this same branch. §29.1's "caller owns a returned enumerator" obligation
// (#2443) does not newly attach here: this coercion is a TYPE WIDENING, not
// a `.GetEnumerator()` call, and the source collection remains the sole
// owner throughout, same as Array's already-shipped identical code path.
//
// A PRE-EXISTING, unrelated leak (the small vtable-enumerator STRUCT this
// wrap `calloc`s is never freed on the borrowing argument/local-decl paths —
// tracked by #2298, confirmed identically for a plain `Array<int>` control
// both before and after this fix) is NOT this issue's concern (§29.1
// ownership of the borrowed BACKING COLLECTION is unaffected either way) and
// is not asserted against here, matching the established convention in
// tests/codegen/set_as_ienumerable_1069_test.cpp (`run()`, not a
// leak-checking variant, for the identical borrowing-argument shape).
//
// Two further defects, confirmed live and reachable but NOT fixed here (a
// materially larger surface — `ensureIEnumViewFactory`'s nested-view
// composition is not yet Stack-aware in either direction), are filed as
// #2487 and guarded below (asserting the CURRENT, still-wrong value, so a
// future accidental regression — or an accidental undocumented fix — is
// caught either way).
//
// fail-without / pass-with: reverting this fix's diff turns every ORDER-
// sensitive case below red (wrong digit sequence); the Queue/Array
// non-regression guards and the #2487 nested guards stay green either way.
// // per stdlib §4.11 "Iteration order" (v0.195), language spec §29.1/§29.2
// (v0.235), issue #2486

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── Argument-position coercion: Stack<int> -> IEnumerable<int> ──────────

// Push(1);Push(2);Push(3) -> physical [1,2,3]. §4.11 pop order visits 3,2,1.
// Base-5 encoding (n := n*5+x) so forward (38) and pop (86) are
// unambiguous and safely under the WEXITSTATUS 8-bit mask (tests/README.md).
BEZEL_TEST(stack_ienumerable_coercion_order_2486, argument_coercion_pop_order_sequence) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<int>): int "
        "  var n := 0; "
        "  for var x in src do n := n * 5 + x; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  return C.Sum(s); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// The source Stack is untouched by the (borrowing) coercion — still usable,
// and still Count 3, after the call. (Note: runAot's exitCode is
// WEXITSTATUS-masked to 8 bits — tests/README.md — so this stays <= 255
// rather than combining `n` and `Count` into one out-of-range value.)
BEZEL_TEST(stack_ienumerable_coercion_order_2486, argument_coercion_source_intact_after_call) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<int>): int "
        "  var n := 0; "
        "  for var x in src do n := n * 5 + x; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var n := C.Sum(s); "
        "  if s.Count <> 3 then return 1; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// ── Local-decl-position coercion: `var src: IEnumerable<int> := s;` ─────

// A SEPARATE dispatch point from the argument/return sites (see file header)
// — confirmed to need its OWN fix, not just wrapAsIEnumerableForTarget's.
BEZEL_TEST(stack_ienumerable_coercion_order_2486, localdecl_coercion_pop_order_sequence) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var src: IEnumerable<int> := s; "
        "  var n := 0; "
        "  for var x in src do n := n * 5 + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// ── Return-position coercion: `method(...): IEnumerable<int> return s;` ─

BEZEL_TEST(stack_ienumerable_coercion_order_2486, return_coercion_pop_order_sequence) {
    const std::string program =
        "module M; "
        "class C public static method AsEnum(s: Stack<int>): IEnumerable<int> "
        "  return s; "
        "end AsEnum; "
        "public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  var n := 0; "
        "  for var x in C.AsEnum(s) do n := n * 5 + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// ── Direct cross-check: ALL FIVE paths agree on the SAME element sequence ─
//
// The coordinator's explicit "prove agreement across all three paths — a
// test per path that each passes independently does NOT prove they agree"
// requirement. Collects the traversal sequence from the for-loop (#2472),
// explicit .GetEnumerator() (#2471), the argument coercion, the local-decl
// coercion, and the return coercion — all over the SAME live Stack<int> —
// into five arrays, then asserts every one of them equal to the OTHER FOUR
// element-for-element (not just each separately equal to a hand-computed
// constant).
BEZEL_TEST(stack_ienumerable_coercion_order_2486, all_five_coercion_paths_agree_directly) {
    const std::string program =
        "module M; "
        "class C "
        "  public static method ViaArg(src: IEnumerable<int>): owned Array<int> "
        "    var acc := new Array<int>(0); "
        "    for var x in src do acc.Add(x); end; "
        "    return acc; "
        "  end ViaArg; "
        "  public static method ViaReturn(s: Stack<int>): IEnumerable<int> "
        "    return s; "
        "  end ViaReturn; "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(10); s.Push(20); s.Push(30); s.Push(40); "
        "    using var viaFor := new Array<int>(0); "
        "    for var x in s do viaFor.Add(x); end; "
        "    using var viaGetEnum := new Array<int>(0); "
        "    using var e := s.GetEnumerator(); "
        "    while e.MoveNext() do viaGetEnum.Add(e.Current); end; "
        "    using var viaArgCoercion := C.ViaArg(s); "
        "    var src2: IEnumerable<int> := s; "
        "    using var viaLocalDecl := new Array<int>(0); "
        "    for var x in src2 do viaLocalDecl.Add(x); end; "
        "    using var viaReturnCoercion := new Array<int>(0); "
        "    for var x in C.ViaReturn(s) do viaReturnCoercion.Add(x); end; "
        "    var n := viaFor.Length; "
        "    if viaGetEnum.Length <> n then return 1; end; "
        "    if viaArgCoercion.Length <> n then return 2; end; "
        "    if viaLocalDecl.Length <> n then return 3; end; "
        "    if viaReturnCoercion.Length <> n then return 4; end; "
        "    var i := 0; "
        "    while i < n do "
        "      if viaGetEnum[i] <> viaFor[i] then return 10; end; "
        "      if viaArgCoercion[i] <> viaFor[i] then return 11; end; "
        "      if viaLocalDecl[i] <> viaFor[i] then return 12; end; "
        "      if viaReturnCoercion[i] <> viaFor[i] then return 13; end; "
        "      i := i + 1; "
        "    end; "
        "    return 42; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── OwningStack<T>, class element, argument-position coercion ───────────

BEZEL_TEST(stack_ienumerable_coercion_order_2486, owningstack_argument_coercion_pop_order) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method Sum(src: IEnumerable<Res>): int "
        "  var n := 0; "
        "  for var x in src do n := n * 5 + x.Tag; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var s := new OwningStack<Res>(); "
        "  s.Push(new Res(1)); s.Push(new Res(2)); s.Push(new Res(3)); "
        "  return C.Sum(s); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);
}

// ── Empty stack — argument-position coercion visits zero elements ───────

BEZEL_TEST(stack_ienumerable_coercion_order_2486, empty_stack_argument_coercion_zero_iterations) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<int>): int "
        "  var n := 0; "
        "  for var x in src do n := n + 1000; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var s := new Stack<int>(); "
        "  return C.Sum(s) + 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Queue<T> non-regression control — dequeue order coincides with ──────
// ── forward/physical order, so it must be UNCHANGED by this fix ─────────

BEZEL_TEST(stack_ienumerable_coercion_order_2486, queue_argument_coercion_unchanged_forward_order) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<int>): int "
        "  var n := 0; "
        "  for var x in src do n := n * 5 + x; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  return C.Sum(q); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 38);  // forward = correct for Queue
}

BEZEL_TEST(stack_ienumerable_coercion_order_2486, queue_localdecl_coercion_unchanged_forward_order) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  var src: IEnumerable<int> := q; "
        "  var n := 0; "
        "  for var x in src do n := n * 5 + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 38);
}

BEZEL_TEST(stack_ienumerable_coercion_order_2486, queue_return_coercion_unchanged_forward_order) {
    const std::string program =
        "module M; "
        "class C public static method AsEnum(q: Queue<int>): IEnumerable<int> "
        "  return q; "
        "end AsEnum; "
        "public static method Main(): int "
        "  using var q := new Queue<int>(); "
        "  q.Enqueue(1); q.Enqueue(2); q.Enqueue(3); "
        "  var n := 0; "
        "  for var x in C.AsEnum(q) do n := n * 5 + x; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 38);
}

// ── Array<T>/OwningArray<T> non-regression control — forward order is ───
// ── the CORRECT order for Array (§29.2) and must not change ─────────────

BEZEL_TEST(stack_ienumerable_coercion_order_2486, array_argument_coercion_unchanged_forward_order) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<int>): int "
        "  var n := 0; "
        "  for var x in src do n := n * 5 + x; end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return C.Sum(a); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 38);
}

BEZEL_TEST(stack_ienumerable_coercion_order_2486, owningarray_return_coercion_unchanged_forward_order) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "end Res; "
        "class C public static method AsEnum(a: OwningArray<Res>): IEnumerable<Res> "
        "  return a; "
        "end AsEnum; "
        "public static method Main(): int "
        "  using var a := new OwningArray<Res>(); "
        "  a.Add(new Res(1)); a.Add(new Res(2)); a.Add(new Res(3)); "
        "  var n := 0; "
        "  for var x in C.AsEnum(a) do n := n * 5 + x.Tag; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 38);
}

// ── #2487 (nested composition): these two were flip-on-fix guards in the ─
// ── original #2486 commit (asserting the then-current WRONG value so an ──
// ── accidental fix or extra breakage was caught either way). #2487's fix ─
// ── (ensureIEnumViewFactory now Stack-aware in both directions) flips ────
// ── them to the §4.11-correct pop-order values; richer cases live in ────
// ── tests/codegen/nested_ienumerable_stack_order_2487_test.cpp. ──────────

// Outer Array<Stack<int>>, elements are Stack<int>: outer order (forward) is
// already correct for Array; each Stack ELEMENT's own leaf wrap
// (ensureIEnumViewFactory's leaf branch) now iterates §4.11 pop order.
// s = Push(1);Push(2);Push(3) -> 3,2,1 -> n = 3*25+2*5+1 = 86.
BEZEL_TEST(stack_ienumerable_coercion_order_2486,
           nested_array_of_stack_element_pop_order_2487) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n * 5 + x; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  using var outer := new Array<Stack<int>>(); "
        "  var s := new Stack<int>(); "
        "  s.Push(1); s.Push(2); s.Push(3); "
        "  outer.Add(s); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 86);  // #2487 fix: was 38 (forward) before
}

// Outer Stack<Array<int>>: the OUTER traversal itself (over the two inner
// Array<int> elements) now iterates §4.11 pop order. Pop order visits
// a2=[2] then a1=[1] -> n = 2*5+1 = 11.
BEZEL_TEST(stack_ienumerable_coercion_order_2486,
           nested_stack_of_array_outer_pop_order_2487) {
    const std::string program =
        "module M; "
        "class C public static method Sum(src: IEnumerable<IEnumerable<int>>): int "
        "  var n := 0; "
        "  for var inner in src do "
        "    for var x in inner do n := n * 5 + x; end; "
        "  end; "
        "  return n; "
        "end Sum; "
        "public static method Main(): int "
        "  var outer := new Stack<Array<int>>(); "
        "  using var a1 := new Array<int>(); a1.Add(1); "
        "  using var a2 := new Array<int>(); a2.Add(2); "
        "  outer.Push(a1); outer.Push(a2); "
        "  return C.Sum(outer); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 11);  // #2487 fix: was 7 (forward) before
}
