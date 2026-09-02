// Codegen — GENERIC owning-collection .Drain() (#990).
//
// stdlib §4.5/§4.7/§4.11 / §14.2: `for var item in coll.Drain()` over an owning
// sequence collection (OwningArray/Stack/Queue/LinkedList) inside a GENERIC class
// `Wrapper<T> where T: class`, where the element type is the unresolved type
// parameter `T`.
//
// Before the fix the generic case (T an unresolved class type parameter) was a
// two-fold defect:
//   1. lowerOwnedSeqDrainFor lowered the bare `T` to the i32 placeholder, so the
//      drain loop's element-slot store bitcast the popped `ptr` to i32 — malformed
//      IR (IR verification failed, compile rc=1).
//   2. A naive "force ptr" band-aid made it compile but never disposed the drained
//      elements (classOfLocal cannot resolve T to a concrete class in the generic
//      TEMPLATE body, so the static-disposal path missed it) — a silent leak.
//
// The fix resolves the element type through the per-instantiation substitution
// (falling back to a class pointer for the owning-collection invariant `T: class`)
// AND routes `using var _ := item` for a drained owning-collection element to the
// dynamic (vtable-slot-2) dispose, so each drained element disposes EXACTLY ONCE.
//
// These tests instantiate Wrapper<R> with a destructor-counter class R and assert
// the post-drain count is exactly the number of drained elements (every element
// disposed once, none leaked, none double-freed), plus ASan leak/double-free
// checks for all four owning collections.

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

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
    return bezel::test::runUnderAsan(source, "bezel_990_asan");
}

// Build a program with a destructor-counter class R and a generic Wrapper<T>
// holding an owning collection `coll` of type `collType<T>`.  `addCall` is the
// method used to insert (Add / AddLast / Push / Enqueue).  DrainAll() drains via
// `using var _ := item`.  Main instantiates Wrapper<R>, adds two elements, drains,
// disposes the wrapper, and returns R._n (== 0 when both drained elements were
// disposed exactly once).
std::string makeGenericProgram(const std::string& collType,
                               const std::string& addCall) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public constructor() _n := _n + 1; end constructor; "
        "  destructor _n := _n - 1; end destructor; "
        "end R; "
        "class Wrapper<T> where T: class "
        "  public owned _items: " + collType + "<T>; "
        "  public constructor() _items := new " + collType + "<T>(); end constructor; "
        "  destructor end destructor; "
        "  public method Add(owned c: T) _items." + addCall + "(owned c); end Add; "
        "  public method DrainAll() "
        "    for var item in _items.Drain() do using var _ := item; end; "
        "  end DrainAll; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  var w := new Wrapper<R>(); "
        "  w.Add(new R()); "
        "  w.Add(new R()); "
        "  w.DrainAll(); "
        "  using var _ := w; "
        "  return R._n; "  // 0 when both drained elements disposed exactly once
        "end Main; end C;";
}

}  // namespace

// ===========================================================================
// §4.5 / §14.2 / #990: generic OwningArray<T>.Drain() — compile + dispose-once
// ===========================================================================

// The element type T is an unresolved class type parameter; the drain compiles
// (no IR_FAIL) and disposes each drained element exactly once (R._n == 0).
BEZEL_TEST(generic_owned_drain_990, ownedarray_generic_drain_disposes_once_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeGenericProgram("OwningArray", "Add")), 0);
}

// §4.11 / §14.2: generic OwningStack<T>.Drain().
BEZEL_TEST(generic_owned_drain_990, ownedstack_generic_drain_disposes_once_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeGenericProgram("OwningStack", "Push")), 0);
}

// §4.11 / §14.2: generic OwningQueue<T>.Drain().
BEZEL_TEST(generic_owned_drain_990, ownedqueue_generic_drain_disposes_once_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeGenericProgram("OwningQueue", "Enqueue")), 0);
}

// §4.7 / §14.2: generic OwningLinkedList<T>.Drain() (AddLast inserts).
BEZEL_TEST(generic_owned_drain_990, ownedlinkedlist_generic_drain_disposes_once_spec_4_7) {
    BEZEL_EXPECT_EQ(run(makeGenericProgram("OwningLinkedList", "AddLast")), 0);
}

// ===========================================================================
// §4.5 / §14.2 / #990: the drained element's value is usable in the loop body
// (the popped pointer is a real class object, not an i32-bitcast garbage value).
// ===========================================================================

// Sum the drained elements' stored values inside the generic drain body — proves
// the loop variable holds a live class object (front-to-back drain of [10,20,30]).
BEZEL_TEST(generic_owned_drain_990, generic_drain_element_value_usable_spec_4_5) {
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class Bag<T> where T: class "
        "  public owned _items: OwningArray<T>; "
        "  public constructor() _items := new OwningArray<T>(); end constructor; "
        "  destructor end destructor; "
        "  public method Add(owned c: T) _items.Add(owned c); end Add; "
        "end Bag; "
        "class C public static method Main(): int "
        "  using var b := new Bag<Val>(); "
        "  b.Add(new Val(10)); "
        "  b.Add(new Val(20)); "
        "  b.Add(new Val(30)); "
        "  var sum := 0; "
        "  for var item in b._items.Drain() do "
        "    using var it := item; "
        "    sum := sum + it.N; "
        "  end; "
        "  return sum; "  // 60
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// ===========================================================================
// §4.5 / §14.2 / #990: a NON-generic instantiation of the same Wrapper still
// disposes once (no regression of the concrete path).
// ===========================================================================

BEZEL_TEST(generic_owned_drain_990, nongeneric_ownedarray_drain_disposes_once_spec_4_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public constructor() _n := _n + 1; end constructor; "
        "  destructor _n := _n - 1; end destructor; "
        "end R; "
        "class Wrapper "
        "  public owned _items: OwningArray<R>; "
        "  public constructor() _items := new OwningArray<R>(); end constructor; "
        "  destructor end destructor; "
        "  public method Add(owned c: R) _items.Add(owned c); end Add; "
        "  public method DrainAll() "
        "    for var item in _items.Drain() do using var _ := item; end; "
        "  end DrainAll; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  var w := new Wrapper(); "
        "  w.Add(new R()); "
        "  w.Add(new R()); "
        "  w.DrainAll(); "
        "  using var _ := w; "
        "  return R._n; "  // 0
        "end Main; end C;"), 0);
}

// ===========================================================================
// §4.5 / §14.2 / #990: ASan — generic drain has NO leak / double-free / UAF.
// ===========================================================================

BEZEL_TEST(generic_owned_drain_990, asan_ownedarray_generic_drain_no_leak) {
    int asan = runUnderASan(makeGenericProgram("OwningArray", "Add"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

BEZEL_TEST(generic_owned_drain_990, asan_ownedstack_generic_drain_no_leak) {
    int asan = runUnderASan(makeGenericProgram("OwningStack", "Push"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

BEZEL_TEST(generic_owned_drain_990, asan_ownedqueue_generic_drain_no_leak) {
    int asan = runUnderASan(makeGenericProgram("OwningQueue", "Enqueue"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}

BEZEL_TEST(generic_owned_drain_990, asan_ownedlinkedlist_generic_drain_no_leak) {
    int asan = runUnderASan(makeGenericProgram("OwningLinkedList", "AddLast"));
    if (asan == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(asan >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(asan, 0);
}
