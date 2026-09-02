// Codegen — BOUND OwningArray<T>/OwningLinkedList<T>/OwningStack<T>/
// OwningQueue<T>/OwningSet<T>.Drain() view iterated by `for x in d`
// (#2167).
//
// stdlib §4.5/§4.7/§4.11 v0.135: Drain() returns IOwningEnumerable<T>.
// stdlib §4.5 v0.195 / §29.2: the Drain() view IS the receiver collection at
// IOwningEnumerable<T> static type ("no separate enumerable object"). The
// canonical drain form `for var x in coll.Drain() do ... end` is recognised by
// lowerOwnedSeqDrainFor (LlvmBackend_Stmts.cpp). The BOUND form
//   `var d := coll.Drain(); for var x in d do ... end`
// is semantically identical (§28.10, §28.11 — `d` aliases the receiver; the
// receiver's owner keeps the one disposal obligation, #2146).
//
// Pre-#2167 the bound form fell through to lowerIEnumerableBuiltinFor, which
// iterated the IOwningEnumerable<T>-typed local as a bz_array_t* via
// bz_array_len / bz_array_ptr_at. The OwningArray IS-A bz_array_t*, so the
// "array" path sometimes ran, but the layout offsets mismatched: the class
// vtable occupied offset 0 (which bz_array_len read as the count), the first
// class field occupied offset 8 (which bz_array_ptr_at read as the data
// pointer). With ASan-instrumented heap the first 8 bytes of a class instance
// are a vtable pointer that ASan occasionally poisons with redzone patterns,
// and the for-in's own scope-end iterEnumDispose (slot 1 = Dispose) called a
// random field offset as a function pointer. Either way: SEGV inside
// bz_array_len or the iterator Dispose trampoline.
//
// #2167 extends lowerOwnedSeqDrainFor to recognise the bound form: when the
// for-in iterable is an IdentifierExpr whose resolved symbol is a LocalSymbol
// whose LocalDecl::initializer is the builtin `recv.Drain()` call, the
// function recovers the original receiver expression and lowers the for-in
// through the same drain path the inline form uses — identical IR shape, no
// bz_array protocol, no vtable-disposal dispatch on a class instance.
//
// spec-cited tests below exercise each owning-collection type in the bound
// form (positive control: inline form continues to work; bound form now works;
// both share the same R._n disposal contract).
//
// ASan tests in this file run the same programs under
// BEZEL_AOT_FORCE_SANITIZE=address so a regression in the bind/unbind
// distinction surfaces immediately as a SEGV / heap-use-after-free / leak
// rather than a silent miscompile.

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
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

namespace fs = std::filesystem;

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LSan/ASan helper — compile → object → link with ASan runtime → run.
// Returns 0 (clean), 1 (sanitizer fired), -1 (toolchain missing).
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_2167_asan");
}

// Shared destructor-counter class template for all tests.
// R._n increments on each disposal; tests encode expected values in return.
std::string makeProgram(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        + mainBody + " end Main; end C;";
}

}  // namespace

// ===========================================================================
// §4.5 / #2167: OwningArray<T>.Drain() bound view — front-to-back order
// ===========================================================================

// §4.5: bound Drain view disposes every element exactly once.
// Three elements added; the bound view drains all of them.
// (Mirrors the inline `for var item in arr.Drain()` control test in
// owned_drain_639_test.cpp; the bound form must produce the same R._n.)
BEZEL_TEST(owned_drain_bound_2167, ownedarray_full_drain_disposes_all_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "var d := arr.Drain(); "            // bound Drain view
        "for var item in d do "
        "  using var u := item; "
        "end; "
        "return R._n;"  // 3
    )), 3);
}

// §4.5: bound Drain view empties the source collection. After full drain,
// arr.Count == 0 (the drain view IS the receiver per §4.5 v0.195).
BEZEL_TEST(owned_drain_bound_2167, ownedarray_full_drain_empties_array_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "var d := arr.Drain(); "
        "for var item in d do "
        "  using var u := item; "
        "end; "
        "return arr.Count;"  // 0 — alias per §28.10
    )), 0);
}

// §4.5 / §28.11: bound Drain order is front-to-back (matches the inline form).
// Elements added with N values [1,2,3]. First drained should be 1, last 3.
// Encoded: first * 100 + last = 1*100 + 3 = 103.
BEZEL_TEST(owned_drain_bound_2167, ownedarray_drain_order_front_to_back_spec_4_5) {
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<Val>(); "
        "  arr.Add(new Val(1)); "
        "  arr.Add(new Val(2)); "
        "  arr.Add(new Val(3)); "
        "  var d := arr.Drain(); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in d do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  return first * 100 + last; "  // 1*100 + 3 = 103
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 103);
}

// §4.5: bound Drain partial — break after first iteration.
// Drained 1 element; remaining 2 stay in array, disposed by arr.Dispose at
// scope end. Use a `do`-block to force the arr.Dispose to fire before the
// return (the same pattern as owned_drain_639_test.cpp §4.5 control test).
// Encoded: 1*10 + 3 = 13.
BEZEL_TEST(owned_drain_bound_2167, ownedarray_partial_drain_break_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var afterBreak := -1; "
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  var d := arr.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "  afterBreak := R._n; "  // 1 disposed (drained element)
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §4.5: empty source — bound Drain view loop body never executes.
BEZEL_TEST(owned_drain_bound_2167, ownedarray_empty_drain_no_iterations_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "var d := arr.Drain(); "
        "var n := 0; "
        "for var item in d do "
        "  using var u := item; "
        "  n := n + 1; "
        "end; "
        "return n;"  // 0
    )), 0);
}

// §4.5: bound Drain view with explicit IOwningEnumerable<T> annotation on
// the local — the recognition in lowerOwnedSeqDrainFor must NOT depend on
// the local being unannotated (the spec admits the annotated form).
// 3 elements drained; encoded 33.
BEZEL_TEST(owned_drain_bound_2167, ownedarray_bound_drain_annotated_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "var d: IOwningEnumerable<R> := arr.Drain(); "  // annotated
        "var n := 0; "
        "for var item in d do "
        "  using var u := item; "
        "  n := n + 1; "
        "end; "
        "return R._n * 10 + n;"  // 3*10 + 3 = 33
    )), 33);
}

// ===========================================================================
// §4.7 / #2167: OwningStack<T> — LIFO via bound Drain view
// ===========================================================================

BEZEL_TEST(owned_drain_bound_2167, owningstack_full_drain_lifo_spec_4_7) {
    // §4.7: Drain order is LIFO (top-first). Elements pushed in order 1,2,3
    // → drain yields 3 first, 1 last. Encode: first*100 + last = 301.
    // Packed value 301 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var stk := new OwningStack<Val>(); "
        "  stk.Push(new Val(1)); "
        "  stk.Push(new Val(2)); "
        "  stk.Push(new Val(3)); "
        "  var d := stk.Drain(); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in d do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  Console.WriteLine(first * 100 + last); return 0; "  // 3*100 + 1 = 301
        "end Main; end C;"), "301");
}

// ===========================================================================
// §4.11 / #2167: OwningQueue<T> — FIFO via bound Drain view
// ===========================================================================

BEZEL_TEST(owned_drain_bound_2167, owningqueue_full_drain_fifo_spec_4_11) {
    // §4.11: Drain order is FIFO (front-to-back). Elements enqueued 1,2,3
    // → drain yields 1 first, 3 last. Encode: first*100 + last = 103.
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<Val>(); "
        "  q.Enqueue(new Val(1)); "
        "  q.Enqueue(new Val(2)); "
        "  q.Enqueue(new Val(3)); "
        "  var d := q.Drain(); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in d do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  return first * 100 + last; "  // 1*100 + 3 = 103
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 103);
}

// ===========================================================================
// §4.6 / #2167: OwningLinkedList<T> — head-to-tail via bound Drain view
// ===========================================================================

BEZEL_TEST(owned_drain_bound_2167, owninglinkedlist_full_drain_spec_4_6) {
    // §4.6: Drain walks head-to-tail. AddLast 1,2,3 → drain yields 1,2,3.
    // Encode: 1*100 + 3 = 103 (same as OwningArray FIFO).
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var ll := new OwningLinkedList<Val>(); "
        "  ll.AddLast(new Val(1)); "
        "  ll.AddLast(new Val(2)); "
        "  ll.AddLast(new Val(3)); "
        "  var d := ll.Drain(); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in d do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  return first * 100 + last; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 103);
}

// ===========================================================================
// §4.10 / #2167: OwningSet<T> — array-backed (fix #1475), bound Drain view
// ===========================================================================

// §4.10: OwningSet<T> drains front-to-back (no ordering contract). All
// elements disposed exactly once.
BEZEL_TEST(owned_drain_bound_2167, owningset_full_drain_disposes_all_spec_4_10) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningSet<R>(); "
        "s.Add(move (new R())); "
        "s.Add(move (new R())); "
        "s.Add(move (new R())); "
        "var d := s.Drain(); "
        "for var item in d do "
        "  using var u := item; "
        "end; "
        "return R._n;"  // 3
    )), 3);
}

// ===========================================================================
// §28.10 / §28.11 / #2146 / #2167: the bound view ALIASES the receiver.
// ===========================================================================

// §28.10 / §28.11 / stdlib §4.5 v0.195: a `var d := recv.Drain()` over a named
// owning collection is a borrow — the receiver's owner keeps the one
// disposal. Encoding: 3 elements drained + scope-end arr.Dispose = 0 leak,
// but ALSO no double-dispose (R._n must be exactly 3, not 6).
BEZEL_TEST(owned_drain_bound_2167, aliasing_no_double_dispose_spec_28_10_2146) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var afterDrain := -1; "
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  var d := arr.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "  end; "
        "  afterDrain := R._n; "  // 3 disposed by drain
        "end; "
        "return afterDrain * 10 + R._n;"  // 3*10 + 3 = 33
    )), 33);
}

// ===========================================================================
// ASan / #2167 — the pre-fix code SEGV'd in bz_array_len with the bound view.
// These tests run the SAME programs under ASan so any regression in the
// bind/unbind distinction surfaces as a heap-use-after-free / SEGV rather
// than a silent miscompile. They are guard tests for the runtime shape.
// ===========================================================================

// §4.5 ASan: bound Drain view full iteration, no leak, no double-free.
// Pre-#2167 this crashed in bz_array_len at the first iteration.
BEZEL_TEST(owned_drain_bound_2167, asan_ownedarray_full_drain_no_crash_spec_4_5) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  var d := arr.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "  end; "
        "end; "
        "return 0;"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

// §4.7 ASan: bound Drain view on OwningStack, no crash.
BEZEL_TEST(owned_drain_bound_2167, asan_owningstack_full_drain_no_crash_spec_4_7) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var stk := new OwningStack<R>(); "
        "  stk.Push(move (new R())); "
        "  stk.Push(move (new R())); "
        "  stk.Push(move (new R())); "
        "  var d := stk.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "  end; "
        "end; "
        "return 0;"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

// §4.11 ASan: bound Drain view on OwningQueue, no crash.
BEZEL_TEST(owned_drain_bound_2167, asan_owningqueue_full_drain_no_crash_spec_4_11) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var q := new OwningQueue<R>(); "
        "  q.Enqueue(move (new R())); "
        "  q.Enqueue(move (new R())); "
        "  var d := q.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "  end; "
        "end; "
        "return 0;"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

// §4.6 ASan: bound Drain view on OwningLinkedList, no crash.
BEZEL_TEST(owned_drain_bound_2167, asan_owninglinkedlist_full_drain_no_crash_spec_4_6) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var ll := new OwningLinkedList<R>(); "
        "  ll.AddLast(move (new R())); "
        "  ll.AddLast(move (new R())); "
        "  ll.AddLast(move (new R())); "
        "  var d := ll.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "  end; "
        "end; "
        "return 0;"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

// §4.10 ASan: bound Drain view on OwningSet, no crash.
BEZEL_TEST(owned_drain_bound_2167, asan_owningset_full_drain_no_crash_spec_4_10) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(move (new R())); "
        "  s.Add(move (new R())); "
        "  var d := s.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "  end; "
        "end; "
        "return 0;"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

// §4.5 ASan: bound Drain view partial drain + break, no crash / no leak.
// Pre-#2167 SEGV in bz_array_len.
BEZEL_TEST(owned_drain_bound_2167, asan_ownedarray_partial_drain_break_no_crash_spec_4_5) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  var d := arr.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "end; "
        "return 0;"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}
