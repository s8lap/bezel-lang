// Codegen — `LinkedList<T>.Add` / `OwningLinkedList<T>.Add` (#4745).
//
// stdlib §4.6 (v0.307) closes the `LinkedList<T>` declaration with:
//
//   // Plus the standard ICollection<T> operations: Add (appends at the tail,
//   // = AddLast), Clear, Contains, Remove, IsReadOnly.
//
// and §4.7 closes `OwningLinkedList<T>` with:
//
//   // Plus the remaining IOwningCollection<T> operations, elided per §1's
//   //   marker convention: Add(owned item) — appends at the tail, = AddLast —
//   //   and IsReadOnly.
//
// Pre-fix `tryLowerLinkedListInstanceCall` (LlvmBackend_Calls3.cpp) dispatched
// `AddFirst`/`AddLast`/`AddBefore`/`AddAfter` but NOT `Add`, so `l.Add(30)`
// reached the builtin-call backstop and was rejected outright:
//
//   `Add` on built-in type `LinkedList` produced no value, and no dispatch arm
//   emitted any code for this call. … either `Add` is not on that surface (a
//   misspelling), or it is documented but not implemented by this compiler
//   (#2457)
//
// — a documented member that did not compile. Every other layer already
// agreed that it exists: sema's per-family argument check names `Add` on both
// `LinkedList` and `OwningLinkedList` (ExprTypecheck_Calls.cpp), and the
// ownership pass's owned-argument table maps `OwningLinkedList.Add` to
// argument 0 exactly as it maps `AddLast` (Ownership_Checks.cpp), while
// `calleeIsOwningCollectionTransfer` (LlvmBackend_Setup.cpp) already listed
// `Add` on `OwningLinkedList` as a slot-nulling transfer. Only the codegen
// dispatch was absent.
//
// Row 1 derives from rule-corpus case l4_6_0004 ("Add appends at the tail,
// equivalent to AddLast"). The owning rows additionally check that the
// transfer accounting the ownership pass already assumed is the one the new
// arm performs: each element is disposed EXACTLY once, at the list's own
// disposal, and not a second time through the caller's slot.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// stdlib §4.6: Add appends AT THE TAIL. Asserting `Last` (not `Count`) is what
// distinguishes the documented `= AddLast` semantics from a hypothetical
// head-insert; `Count` alone would pass for either.
BEZEL_TEST(linkedlist_add_alias_4745, borrowing_add_appends_at_the_tail) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var l: LinkedList<int> := [10, 20]; "
        "  l.Add(30); "
        "  Console.WriteLine(l.Last!); "
        "end Main; end C;"), "30");
}

// The head is untouched and the count moves — the rest of the `= AddLast`
// claim. Pre-fix this program did not compile at all.
BEZEL_TEST(linkedlist_add_alias_4745, borrowing_add_keeps_head_and_raises_count) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var l: LinkedList<int> := [10, 20]; "
        "  l.Add(30); "
        "  Console.WriteLine(l.First!.ToString() + \"/\" + l.Count.ToString()); "
        "end Main; end C;"), "10/3");
}

// stdlib §4.6: `Add` composes with `AddFirst`, which must still reach the head
// — guards the shared arm against having swallowed the other spelling.
BEZEL_TEST(linkedlist_add_alias_4745, add_and_addfirst_land_at_opposite_ends) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var l := new LinkedList<int>(); "
        "  l.Add(1); "
        "  var n := l.AddFirst(0); "
        "  l.Add(2); "
        "  Console.WriteLine(l.First!.ToString() + \",\" + l.Last!.ToString()); "
        "end Main; end C;"), "0,2");
}

// stdlib §4.7 / spec §28.11: `Add(owned item)` takes ownership. The list's
// disposal cascade must dispose each element EXACTLY once — the destructor
// prints per disposal, so a double-free shows up as a repeated line and a
// dropped transfer shows up as a missing one.
BEZEL_TEST(linkedlist_add_alias_4745, owning_add_transfers_and_disposes_once_each) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class W "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  destructor Console.WriteLine(\"d\" + X.ToString()); end destructor; "
        "end W; "
        "class C public static method Main() "
        "  do "
        "    using var l := new OwningLinkedList<W>(); "
        "    l.Add(new W(1)); "
        "    l.Add(new W(2)); "
        "    Console.WriteLine(l.Count.ToString() + \"/\" + l.Last!.X.ToString()); "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "end Main; end C;"), "2/2\nd2\nd1\nend");
}

// stdlib §4.7 with a MOVED owned local at the `owned item` position — the
// shape `calleeIsOwningCollectionTransfer` already claimed nulls the caller's
// slot. Before the fix nothing lowered, so nothing nulled; the assertion is
// that the element is disposed once (by the list), not twice.
BEZEL_TEST(linkedlist_add_alias_4745, owning_add_of_moved_local_disposes_once) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class W "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  destructor Console.WriteLine(\"d\" + X.ToString()); end destructor; "
        "end W; "
        "class C public static method Main() "
        "  do "
        "    using var l := new OwningLinkedList<W>(); "
        "    var w := new W(7); "
        "    l.Add(move w); "
        "    Console.WriteLine(l.Count); "
        "  end; "
        "  Console.WriteLine(\"end\"); "
        "end Main; end C;"), "1\nd7\nend");
}
