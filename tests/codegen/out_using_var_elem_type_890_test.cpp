// fix #890 / §28.11 / stdlib §4.5 / §4.7 / §4.9 / §6.9:
// `out using var item` from a non-Weak<T> Try*(out owned ...) call got no
// declared type in sema, so a member read on `item` yielded a wrong value
// (rc=255 from untyped local, not the actual field value).
//
// The sema `out using var` branch now derives the element type from the
// receiver's generic argument for all owning-collection / channel Try* methods:
//   OwningStack<T>.TryPop          → T  (arg 0)
//   OwningQueue<T>.TryDequeue      → T  (arg 0)
//   OwningLinkedList<T>.TryPopFirst → T  (arg 0)
//   OwningLinkedList<T>.TryPopLast  → T  (arg 0)
//   OwningDictionary<K,V>.TryPopValue → V (arg 1)
//   Weak<T>.TryUpgrade            → Shared<T>  (unchanged — regression check)
//
// Value tests — runJit returns the raw int (kept in 0..255).
// Each test verifies a member read or method call on the `item` local.
// The disposal path (dynamicDispose, already wired in #889) is also exercised
// via a destructor counter: item disposed exactly once, no leak/double-free.
//
// §28.11, #890

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A class with a public field and a static disposal counter.
// constructor increments Cell.n; destructor decrements Cell.n.
// Correct program: Cell.n == 0 at end (disposed exactly once per created cell).
const char* kCellHdr =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public _v: int := 0; "
    "  public constructor(v: int) _v := v; Cell.n := Cell.n + 1; end constructor; "
    "  public method GetV(): int return _v; end GetV; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ──────────────────────────────────────────────────────────────────────────────
// OwningStack<T>.TryPop — the primary repro from #890.
// §28.11: item is Cell; item._v == 42 (was rc=255 before the fix).
// ──────────────────────────────────────────────────────────────────────────────

// §28.11, #890: OwningStack<Cell>.TryPop — field read through out using var.
BEZEL_TEST(out_using_var_elem_type_890, owned_stack_trypop_field_read) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Cell>(); "
        "  s.Push(move (new Cell(42))); "
        "  var got := -1; "
        "  if s.TryPop(out using var item) then "
        "    got := item!._v; "  // v0.171 (#1336): out is T?, narrow with `!`
        "  end; "
        "  return got; "
        "end Main; end C;"), 42);
}

// §28.11, #890: OwningStack<Cell>.TryPop — method call on item.
// item.GetV() resolves to Cell.GetV() only if item has the correct type.
BEZEL_TEST(out_using_var_elem_type_890, owned_stack_trypop_method_call) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Cell>(); "
        "  s.Push(move (new Cell(99))); "
        "  var got := -1; "
        "  if s.TryPop(out using var item) then "
        "    got := item!.GetV(); "  // v0.171 (#1336): out is T?, narrow with `!`
        "  end; "
        "  return got; "
        "end Main; end C;"), 99);
}

// §28.11, #890: OwningStack<Cell>.TryPop — item disposed exactly once.
// Cell.n starts at 0, constructor +1, destructor -1; must end at 0 (no leak).
BEZEL_TEST(out_using_var_elem_type_890, owned_stack_trypop_disposal_once) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do "
        "    using var s := new OwningStack<Cell>(); "
        "    s.Push(move (new Cell(1))); "
        "    if s.TryPop(out using var item) then "
        "    end; "
        "  end; "
        "  return Cell.n; "
        "end Main; end C;"), 0);
}

// ──────────────────────────────────────────────────────────────────────────────
// OwningQueue<T>.TryDequeue
// §28.11, #890: element type derived from genericArgs[0].
// ──────────────────────────────────────────────────────────────────────────────

// §28.11, #890: OwningQueue<Cell>.TryDequeue — field read.
BEZEL_TEST(out_using_var_elem_type_890, owned_queue_trydequeue_field_read) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<Cell>(); "
        "  q.Enqueue(move (new Cell(77))); "
        "  var got := -1; "
        "  if q.TryDequeue(out using var item) then "
        "    got := item!._v; "  // v0.171 (#1336): out is T?, narrow with `!`
        "  end; "
        "  return got; "
        "end Main; end C;"), 77);
}

// ──────────────────────────────────────────────────────────────────────────────
// OwningLinkedList<T>.TryPopFirst / TryPopLast
// §28.11, #890: element type derived from genericArgs[0].
// ──────────────────────────────────────────────────────────────────────────────

// §28.11, #890: OwningLinkedList<Cell>.TryPopFirst — field read.
BEZEL_TEST(out_using_var_elem_type_890, owned_linkedlist_trypopfirst_field_read) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var ll := new OwningLinkedList<Cell>(); "
        "  ll.AddLast(move (new Cell(11))); "
        "  ll.AddLast(move (new Cell(22))); "
        "  var got := -1; "
        "  if ll.TryPopFirst(out using var first) then "
        "    got := first!._v; "  // v0.171 (#1336): out is T?, narrow with `!`
        "  end; "
        "  return got; "
        "end Main; end C;"), 11);
}

// §28.11, #890: OwningLinkedList<Cell>.TryPopLast — field read.
BEZEL_TEST(out_using_var_elem_type_890, owned_linkedlist_trypoplast_field_read) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var ll := new OwningLinkedList<Cell>(); "
        "  ll.AddLast(move (new Cell(11))); "
        "  ll.AddLast(move (new Cell(22))); "
        "  var got := -1; "
        "  if ll.TryPopLast(out using var last) then "
        "    got := last!._v; "  // v0.171 (#1336): out is T?, narrow with `!`
        "  end; "
        "  return got; "
        "end Main; end C;"), 22);
}

// ──────────────────────────────────────────────────────────────────────────────
// OwningDictionary<K,V>.TryPopValue
// §28.11, #890: value type = genericArgs[1] (V).
// ──────────────────────────────────────────────────────────────────────────────

// §28.11, #890: OwningDictionary<int,Cell>.TryPopValue — field read on V element.
BEZEL_TEST(out_using_var_elem_type_890, owned_dict_trypopvalue_field_read) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, Cell>(); "
        "  d.Add(1, move (new Cell(55))); "
        "  var got := -1; "
        "  if d.TryPopValue(1, out using var v) then "
        "    got := v!._v; "  // v0.171 (#1336): out is V?, narrow with `!`
        "  end; "
        "  return got; "
        "end Main; end C;"), 55);
}

// ──────────────────────────────────────────────────────────────────────────────
// Regression: Weak<T>.TryUpgrade still derives Shared<T>.
// §28.13, #603: s is Shared<Cell>; s.Get()._v == 66.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13, #603 (regression): Weak<Cell>.TryUpgrade — out using var s is Shared<Cell>.
// s.Get() returns a borrow of Cell; field read on the borrow works.
BEZEL_TEST(out_using_var_elem_type_890, weak_tryupgrade_shared_regression) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  var obj := new Cell(66); "
        "  using var sh := new Shared<Cell>(move obj); "
        "  using var w := sh.Downgrade(); "
        "  var got := -1; "
        "  if w.TryUpgrade(out using var s) then "
        "    if s.Get()._v = 66 then got := 66; end; "
        "  end; "
        "  return got; "
        "end Main; end C;"), 66);
}
