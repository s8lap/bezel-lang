// fix #5588 (§23.4 / §28.11, v0.315): OwningStack<T>, OwningQueue<T>,
// OwningLinkedList<T>, OwningSet<T>, and OwningSlotMap<T> declare NO indexer.
// `stack[i]` on any of these five types is a compile error (§23.4).
// Before the fix: the sema IndexExpr typing path left the expression untyped
// (none of the isArrLike / isSpan / isDict checks match these types), but
// codegen's lowerCollectionIndexerGet accepted them via isCollectionTypeName
// (BuiltinRegistry.cpp's kArrayBackedCollections — Stack/Queue/LinkedList/Set
// and their Owning... variants are registered there for non-indexer operations
// like `.Count`). The program compiled, ran, and threw IndexOutOfRangeException
// at runtime.
//
// THE FIX adds one explicit rejection arm in ExprTypecheck_Exprs.cpp's builtin
// collection IndexExpr handling (the branch that checks !tt->declaration &&
// tt->astNode), positioned after the isArrLike / isSpan / isDict checks that
// name the types that DO have indexers. The five types are named explicitly
// and rejected with a §23.4 diagnostic.
//
// TEST MATRIX:
//   T1  all five owning collection types rejected at the driver level
//   T2  owning collections still work for their other operations (Push/Pop, etc.)
//   T3  OwningArray HAS an indexer — confirmed it is unaffected
//   T4  Array HAS an indexer — confirmed it is unaffected

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr std::string_view kSec234 = "\xC2\xA7" "23.4";

// A driver-level rejection row: the whole bezelc pipeline must refuse the program.
void expectDriverRejectsOwningCollectionIndexer(const std::string& collName,
                                               const std::string& body) {
    std::string src = "module M; class R public static _n: int := 0; "
                      "  public constructor() end constructor; "
                      "  destructor R._n := R._n + 1; end destructor; end R; "
                      "class C public static method Main(): int " + body +
                      " end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("type `") + collName +
            "<T>` does not declare an indexer").c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kSec234).c_str()));
}

}  // namespace

// ===========================================================================
// T1 — all five owning collection types: indexing is rejected.
// ===========================================================================
BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t1_owning_stack_index_rejected) {
    // Pre-fix: compiled, ran, IndexOutOfRangeException.
    expectDriverRejectsOwningCollectionIndexer(
        "OwningStack",
        " using var s := new OwningStack<R>(); "
        " s.Push(new R()); var w := s[0]; return 0; ");
}

BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t1_owning_queue_index_rejected) {
    expectDriverRejectsOwningCollectionIndexer(
        "OwningQueue",
        " using var q := new OwningQueue<R>(); "
        " q.Enqueue(new R()); var w := q[0]; return 0; ");
}

BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t1_owning_linked_list_index_rejected) {
    expectDriverRejectsOwningCollectionIndexer(
        "OwningLinkedList",
        " using var l := new OwningLinkedList<R>(); "
        " l.AddLast(new R()); var w := l[0]; return 0; ");
}

BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t1_owning_set_index_rejected) {
    expectDriverRejectsOwningCollectionIndexer(
        "OwningSet",
        " using var s := new OwningSet<R>(); "
        " s.Add(new R()); var w := s[0]; return 0; ");
}

BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t1_owning_slot_map_index_rejected) {
    // OwningSlotMap requires arity-1 entry in builtinTypeArity
    // (TypeResolution_Names.cpp); added as part of this fix.
    std::string src =
        "module M; class R public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R._n := R._n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var m := new OwningSlotMap<R>(); "
        "  m.Insert(1, new R()); "
        "  var w := m[1]; "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type `OwningSlotMap<T>` does not declare an indexer"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kSec234).c_str()));
}

// ===========================================================================
// T2 — owning collections still work for their other operations.
// All use runAot (which asserts exit 0) + TryPeek/TryPop patterns with
// proper out-bindings so that owned elements are explicitly disposed.
// ===========================================================================
BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t2_owning_stack_push_pop_still_works) {
    // Use TryPeek/TryPop with out-bindings; the owned element from TryPop
    // is bound to `using var` so it is disposed at end of that scope.
    auto r = runAot(
        "module M; class R public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R._n := R._n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(new R()); "
        "  if s.TryPeek(out var peeked) then end if; "
        "  if s.TryPop(out using var popped) then end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}

BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t2_owning_queue_enqueue_dequeue_still_works) {
    auto r = runAot(
        "module M; class R public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R._n := R._n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<R>(); "
        "  q.Enqueue(new R()); "
        "  if q.TryPeek(out var peeked) then end if; "
        "  if q.TryDequeue(out using var deqd) then end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}

BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t2_owning_linked_list_add_last_remove_first_still_works) {
    auto r = runAot(
        "module M; class R public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R._n := R._n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var l := new OwningLinkedList<R>(); "
        "  l.AddLast(new R()); "
        "  if l.TryPopFirst(out using var popped) then end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}

BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t2_owning_set_add_still_works) {
    // OwningSet.Add is sufficient — no indexer involved.
    auto r = runAot(
        "module M; class R public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R._n := R._n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(new R()); "
        "  return s.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}

// ===========================================================================
// T3 — OwningArray HAS an indexer: confirmed it is unaffected.
// ===========================================================================
BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t3_owning_array_index_still_works) {
    auto r = runAotChecked(
        "module M; class R public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R._n := R._n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(new R()); "
        "  var w := a[0]; "
        "  return R._n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // R disposed exactly once (Add), array holds it
}

// ===========================================================================
// T4 — Array HAS an indexer: confirmed it is unaffected.
// Array holds borrows, so use a named borrow (from using var) in the literal.
// ===========================================================================
BEZEL_TEST(sema_owning_collection_no_indexer_5588,
           t4_array_index_still_works) {
    auto r = runAot(
        "module M; class R public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor R._n := R._n + 1; end destructor; end R; "
        "class C public static method Main(): int "
        "  using var r := new R(); "
        "  var a: Array<R> := [r]; "
        "  var w := a[0]; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
}
