// Regression tests for #8028 — the OWNING family's TRANSFER members still
// LEAKED the receiver, and `OwningBuffer<T>` was not covered at all.
//
// THE TWO RESIDUALS #8009 LEFT (both PRE-EXISTING, measured on the #8009 tip
// AND on its parent by the #8009 review):
//
//   * `Mk().Pop()` with `Mk(): owned OwningStack<Cell>` — 20 800 B / 200
//     allocations leaked (the issue's own magnitude);
//   * `Mk().Length` with `Mk(): owned OwningBuffer<Cell>` — 8 800 B / 200
//     allocations leaked.
//
// THE CLASSIFICATION, CORRECTED. The #8009 report grouped `Pop*` with
// `Peek` / `Drain` / `GetEnumerator` / `GetOrNull` as "reference-returning, so
// the result may borrow from the collection". That is TRUE of the borrow
// members and FALSE of the TRANSFER members: stdlib §4.5 / §4.7 / §4.9 / §4.10
// / §4.11 declare `Pop` / `PopAt` / `PopFirst` / `PopLast` / `PopValue` /
// `Dequeue` with an `owned` return ("transfers out"), and their lowering
// REMOVES the element from the collection BEFORE returning it
// (`bz_array_get` + `bz_array_remove_at`, `bz_array_pop_at`,
// `bz_dict_pop_value_*`). A transfer's result is therefore a FRESH owned value
// that does NOT alias the receiver — and the collection is the CALLER's to
// dispose at the §28.3 K8.1 boundary. Disposing it cannot dangle the
// transferred value.
//
// ⇒ THE TRANSFERRED VALUE IS A DIFFERENT VALUE FROM THE RECEIVER. The
// receiver's remaining elements are the receiver's (the §28.11 cascade
// disposes them); the popped element is the CALLER's.
//
// `OwningBuffer<T>` (stdlib §4.14) is the family's runtime-sized storage
// primitive — NOT one of §28.11's seven element-owning collections — and it
// was not covered by #8009's head test, so its chain root leaked too. Its
// shape: a class with fixed-at-construction storage of N slots; for class-typed
// T the slots hold `T?` and "the buffer owns each non-null slot's referent",
// for value-typed T the slots hold `T` directly and "the buffer's destructor
// releases its backing storage but performs no per-slot disposal"; it is
// `IDisposable` in both cases. Its runtime storage is a `bz_array_t*`, and
// `bz_array_free`'s element loop skips a NULL slot — so the §28.11 cascade
// disposes exactly the non-null referents §4.14 names.
//
// ── THE LEAK ORACLE (this is the point of the file) ────────────────────────
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// compiler and would prove nothing. Every DEFECT row here measures a COUNT:
// the number of allocations LeakSanitizer reports as leaked, read out of
// LSan's own "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in
// <allocations> allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO.
//
// Every row loops 100 times: a one-shot leak repro is a vacuous oracle.
//
// ── THE CONTROLS, WHICH CUT BOTH WAYS ──────────────────────────────────────
//   * a row that pins the TRANSFERRED VALUE (a `Pop` returns the element, and
//     the RIGHT one) — it must be GREEN on BOTH arms, so its oracle is the
//     program's OUTPUT with LeakSanitizer OFF (`runAotCheckedLenient`): the
//     collection's not being disposed IS a leak in that row, so the row
//     measures the VALUE, and a generated-code AddressSanitizer error (which
//     is what a wrongly-disposed collection would produce) fails it;
//   * the BORROW rows — `Peek`, `GetOrNull` — really do borrow, so
//     disposing the collection for them WOULD DANGLE the result. They must NOT
//     be disposed, and their rows must stay GREEN. If a fix disposed them it
//     would be wrong; (`Drain` and `GetEnumerator` borrow too, and are left
//     untouched — but they have no row here, so they are NOT measured);
//   * a §28.1 BORROW receiver (a holder's own field) and a BOUND receiver (a
//     local the scope disposes) must NOT be disposed either — that would be a
//     double free.
//
// The sibling suite `owned_owning_collection_chain_receiver_8009` (the #8009
// rows, including ITS borrow control) is NOT touched by this file and stays
// green.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated -- the sanitizer report
// the AOT harness embeds in the diagnostic message when LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// THE ORACLE. `leakedAllocations` is the COUNT of allocations LeakSanitizer
// reports as leaked: 0 when there is no SUMMARY line (LSan prints one only
// when it finds leaks), and the summary's own number when there is one.
//
// `leakedBytes` is the same measurement in bytes.
void expectNoLeak(const JitResult& r, const char* out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured.
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
}

// THE VALUE CONTROL'S ORACLE. LeakSanitizer is OFF here (the collection's NOT
// being disposed IS the leak in that row), the generated-code AddressSanitizer
// stays ON, and the row asserts the program's OUTPUT — the transferred
// element's value — plus the ABSENCE of any AddressSanitizer report. A
// wrongly-disposed collection would show up as a heap-use-after-free here.
void expectValue(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}

}  // namespace

// ── THE TRANSFER MEMBERS ───────────────────────────────────────────────────
// Every row below has the SAME shape: a static factory returns an `owned`
// owning collection, the collection is used ONLY as the receiver of a
// TRANSFER member, and the transferred element is discharged by the caller
// (`using var`), so the ONLY thing that can leak is the RECEIVER.
//
// `OwningStack<T>.Pop(): owned T` — "raises if empty; transfers out"
// (stdlib §4.11). MEASURED pre-fix: 20 800 B / 200 allocations — the issue's
// own magnitude.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_stack_pop_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningStack<Cell>
    var s := new OwningStack<Cell>();
    s.Push(new Cell());
    return s;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().Pop();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// `OwningQueue<T>.Dequeue(): owned T` (stdlib §4.11).
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_queue_dequeue_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningQueue<Cell>
    var q := new OwningQueue<Cell>();
    q.Enqueue(new Cell());
    return q;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().Dequeue();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// `OwningArray<T>.PopAt(index: int): owned T` — "removes and returns owned"
// (stdlib §4.5). MEASURED pre-fix: 20 800 B / 200 allocations.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_array_popat_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().PopAt(0);
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// `OwningArray<T>.PopFirst(): owned T` (stdlib §4.5).
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_array_popfirst_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().PopFirst();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// `OwningArray<T>.PopLast(): owned T` (stdlib §4.5).
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_array_poplast_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().PopLast();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// `OwningLinkedList<T>.PopFirst(): owned T` (stdlib §4.7).
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_linkedlist_popfirst_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningLinkedList<Cell>
    var l := new OwningLinkedList<Cell>();
    l.AddLast(new Cell());
    return l;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().PopFirst();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// `OwningDictionary<K, V>.PopValue(key: K): owned V` (stdlib §4.9).
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_dictionary_popvalue_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningDictionary<string, Cell>
    var d := new OwningDictionary<string, Cell>();
    d.Add("k", new Cell());
    return d;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().PopValue("k");
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// `OwningSet<T>.Pop(query: T): owned T` — "borrow-taking query" (stdlib
// §4.10). The query is a BORROW of an equal element; the RESULT is an owned
// transfer.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_set_pop_transfer_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 5;
  public override method Equals(other: Cell): bool => this.v = other.v;
  public override method GetHashCode(): int => this.v;
end Cell;
class C
  public static method MkS(): owned OwningSet<Cell>
    var s := new OwningSet<Cell>();
    _ := s.TryAdd(new Cell());
    return s;
  end MkS;
  public static method Main(): int
    var t := 0;
    using var probe := new Cell();
    for var i in 0..<100 do
      using var x := MkS().Pop(probe);
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "500\n");
}

// The TRANSFERRED ELEMENT AND THE RECEIVER'S REMAINING ELEMENTS ARE
// DISPOSED EXACTLY ONCE EACH — the §28.11 cascade must dispose the element the
// collection STILL holds, and must NOT touch the one the call removed and
// transferred to the caller (a double free).
//
// ⇒ The stack holds TWO cells: `Pop()` removes the top and transfers it (the
// caller discharges it with `using var`), and the collection's cascade
// disposes the one it still holds. If the cascade disposed BOTH, the
// transferred cell would be freed twice — a double free, which ASan reports
// and which fails this row.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_stack_pop_disposes_the_remaining_element_once_spec_28_11) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningStack<Cell>
    var s := new OwningStack<Cell>();
    s.Push(new Cell());
    s.Push(new Cell());
    return s;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().Pop();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// K8.1's EXCEPTION EDGE: "on the normal edge as on the exception edge". The
// receiver (`Mk()`) is produced FIRST (§22.5: the receiver precedes the
// argument list) and the member call then RAISES — `PopAt(99)` is out of
// range — so the receiver is an already-produced, not-yet-consumed unbound
// owned temporary at the moment the unwinding starts, and §28.3's
// exception-path discharge disposes it during unwinding. The row is the
// transfer face's twin of the #8009 exception-edge row.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_array_popat_exception_edge_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      try
        using var x := Mk().PopAt(99);
        t += x.v;
      catch Exception => t += 1;
      end try;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// ── `OwningBuffer<T>` (stdlib §4.14) — the runtime-sized storage primitive ──
// The PROPERTY-READ face: `Mk().Length`. §28.3.11 lists "as a member or index
// access" among the BORROWING USES of an unbound owned temporary, so the
// temporary is disposed at the full expression's end.
//
// MEASURED pre-fix: 8 800 B / 200 allocations — the `bz_array_t` header and
// the 1-slot backing of 100 iterations. (The byte count is the buffer's own
// size: a 2-slot buffer reads 9 600 B and a 3-slot one 10 400 B, with the
// SAME 200 allocations — the allocation COUNT is the defect's invariant.)
//
// The class-typed tier is used here on purpose: it is the tier whose slots are
// NULLABLE (`T?`) and whose cascade must dispose "every non-null slot".
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_buffer_property_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningBuffer<Cell>
    return new OwningBuffer<Cell>(1);
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Mk().Length;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// The SAME face with a NON-NULL slot: §4.14 says "for class-typed T … the
// buffer owns each non-null slot's referent … the buffer's destructor disposes
// every non-null slot". So the referent the buffer holds must be disposed
// EXACTLY ONCE by the §28.11 cascade — not zero times (a leak) and not twice
// (a double free).
//
// MEASURED pre-fix: 300 allocations / 10 400 B — the buffer's header and
// backing (2 allocations) AND the referent its slot holds (1) per iteration.
// On the tip the whole triple is released exactly once (no SUMMARY line at all).
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           owning_buffer_slot_referent_is_disposed_once_spec_28_11) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 9;
end Cell;
class C
  public static method Mk(): owned OwningBuffer<Cell>
    return new OwningBuffer<Cell>(1, lambda(i: int) => new Cell());
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      t += Mk().Length;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// ── CONTROL 1: THE TRANSFERRED VALUE (green on BOTH arms) ─────────────────
// `Pop()` on an `OwningStack` takes the TOP element, so the value must be the
// LAST one pushed (3), not the first (1) — the row pins WHICH element
// transferred. The oracle is the program's OUTPUT with LeakSanitizer OFF: the
// collection's NOT being disposed is a leak in this row, so this row measures
// the VALUE, and it must be GREEN on the pre-fix arm as well as on the tip.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           pop_result_is_the_transferred_element_spec_28_3) {
    expectValue(bezel::test::runAotCheckedLenient(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningStack<Cell>
    var s := new OwningStack<Cell>();
    s.Push(new Cell());
    var a := new Cell(); a.v := 1; s.Push(move a);
    var b := new Cell(); b.v := 2; s.Push(move b);
    var c := new Cell(); c.v := 3; s.Push(move c);
    return s;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var x := Mk().Pop();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "300\n");
}

// ── CONTROL 2: THE BORROW ROWS (green on BOTH arms) ───────────────────────
// `Peek()` declares `public pure method Peek(): T;  // borrow of top element`
// (stdlib §4.11) — the result is a BORROW of an element the collection owns,
// so the collection must SURVIVE its result. A fix that disposed it would free
// the element the result points at: a heap-use-after-free, which the
// generated-code AddressSanitizer reports and which fails this row.
//
// ⇒ BY DESIGN the collection is NOT disposed here — its result borrows from it,
// so the collection leaks on BOTH arms (the sound direction, and the reason this
// row's oracle is the program's OUTPUT and not a leak count). The program
// prints its sum with no sanitizer error.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           peek_borrow_result_keeps_the_collection_alive_spec_28_3) {
    expectValue(bezel::test::runAotCheckedLenient(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 1;
end Cell;
class C
  public static method MkS(): owned OwningStack<Cell>
    var s := new OwningStack<Cell>();
    s.Push(new Cell());
    return s;
  end MkS;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      var x: Cell := MkS().Peek();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// `GetOrNull(key: K): V?` (stdlib §4.9) — a BORROW of the stored value, and
// the same rule: the collection must survive its result.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           getornull_borrow_result_keeps_the_collection_alive_spec_28_3) {
    expectValue(bezel::test::runAotCheckedLenient(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 4;
end Cell;
class C
  public static method MkD(): owned OwningDictionary<string, Cell>
    var d := new OwningDictionary<string, Cell>();
    d.Add("k", new Cell());
    return d;
  end MkD;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      var x: Cell? := MkD().GetOrNull("k");
      if x <> null then t += x.v; end if;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "400\n");
}

// ── CONTROL 3: A BORROWED RECEIVER (§28.1) ────────────────────────────────
// `Holder.Get()` returns the receiver's OWN owning-collection FIELD — a
// BORROW, not an `owned` transfer — so the collection is the HOLDER's to
// dispose, not the caller's. The fix must leave it alone: freeing it would
// free the collection the holder still owns (a double free) and cascade-
// dispose its elements. The row's oracle is the leak COUNT (zero) plus the
// absence of any AddressSanitizer report, so it is RED if the borrow is ever
// disposed.
//
// The holder is REFILLED each iteration because `Pop` empties the stack.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           borrowed_transfer_receiver_is_not_disposed_spec_28_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class Holder
  private owned _f: OwningStack<Cell> := new OwningStack<Cell>();
  public method Refill(): ()
    _f.Push(new Cell());
  end Refill;
  public method Get(): OwningStack<Cell> => _f;
end Holder;
class C
  public static method Main(): int
    var t := 0;
    using var h := new Holder();
    for var i in 0..<100 do
      h.Refill();
      using var x := h.Get().Pop();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}

// ── CONTROL 4: A BOUND RECEIVER (§28.3) ───────────────────────────────────
// The collection is a LOCAL: its owner is the local, which disposes it at the
// scope's end (§28.3's auto-drop for an owned local). The fix must NOT also
// dispose it at the full expression's end — that would free it TWICE, and
// ASan would report the double free. The row's oracle is the leak COUNT
// (zero), so it is RED if the bound collection is disposed twice.
BEZEL_TEST(owned_transfer_chain_receiver_8028,
           bound_transfer_receiver_is_not_disposed_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 7;
end Cell;
class C
  public static method Mk(): owned OwningStack<Cell>
    var s := new OwningStack<Cell>();
    s.Push(new Cell());
    return s;
  end Mk;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      var s := Mk();
      using var x := s.Pop();
      t += x.v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "700\n");
}
