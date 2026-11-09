// Regression tests for #8009 -- an `owned` OWNING COLLECTION returned from a
// static method, then used as a chain root, LEAKED THE COLLECTION.
//
// UPDATED BY fix #8331 (stdlib §4.11, v0.355): the two rows that filled an
// `OwningStack<T>` / `OwningQueue<T>` now use the member those heads
// DECLARE (`Push` / `Enqueue`). They used `Add`, which stdlib §4.11 does
// NOT declare for them (`Stack<T>`/`Queue<T>` and their owning forms conform
// to `IReadOnlyCollection<T>` only — the push/enqueue spelling is theirs);
// the compiler admitted `Add` on them through the old SHARED name list, and
// #8331's per-head surface table refuses it. The rows' MEASUREMENT (the leak
// census over a 100-iteration chain) is unchanged.
//
// The #7906 fix (`ee76372b`) disposed a fresh owned built-in CHAIN ROOT at the
// §28.3 K8.1 boundary, but its covered set is the NAME-KEYED release table
// (`vtablelessBuiltinReleaseIsKnown` -> `classifyVtablelessBuiltinRelease`,
// 18 names) -- and the OWNING COLLECTION family is not in it. A collection's
// release is not name-keyed: it is the runtime free (`bz_array_free` /
// `bz_dict_free` / `bz_llist_free`) PLUS the §28.11 element/value disposer,
// and the disposer is a function of the ELEMENT type, which the head name does
// not carry (`OwningArray<Cell>` and `OwningArray<int>` are the same name and
// different releases). So the collection was allocated, used, and never freed.
//
// The issue's own repro (the reviewer's probe
// `r38-w22-7906/probes/p8_owning_array.bzl`), MEASURED on the pre-fix
// compiler under generated-code LeakSanitizer:
//
//   "SUMMARY: AddressSanitizer: 20800 byte(s) leaked in 200 allocation(s)."
//
// 8000 B of it is the `bz_array_t` header and 12800 B the backing buffer
// (both from `bz_array_alloc`), 100 iterations x 2 allocations.
//
// ── THE LEAK ORACLE (this is the point of the file) ────────────────────────
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// compiler and would prove nothing. Every row here measures a COUNT: the
// number of allocations LeakSanitizer reports as leaked, read out of LSan's own
// "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in <allocations>
// allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the same rows read 200 (20800 B) for the
// `OwningArray` row, 300 (41600 B) for `OwningDictionary`, 100 (6400 B) for
// `OwningLinkedList`, and 300 (22400 B) for the property-read row. The rows
// also run under `runAotChecked`, whose LSan leg turns a leak into a failed
// row, so the count and the verdict agree.
//
// Every row loops 100 times: a one-shot leak repro is a vacuous oracle.
//
// ── THE FAMILY, NOT ONE SHAPE ─────────────────────────────────────────────
// The six element-owning collection classes are `OwningArray` /
// `OwningDictionary` / `OwningSet` / `OwningStack` / `OwningQueue` /
// `OwningLinkedList` (stdlib §4.5 / §4.7 / §4.9 / §4.10 / §4.11; the ONE
// registry is `common::isOwningCollectionHead`). A row pins each of them, so
// the fix cannot be a one-name patch.
//
// ── THE SOUNDNESS ROW ─────────────────────────────────────────────────────
// A BORROW is never disposed (§28.1: an `owned` return transfers ownership; a
// plain return lends). `Holder.Get()` returns the receiver's own owning-
// collection FIELD -- a borrow -- so the receiver of `h.Get().Clear()` is NOT
// the caller's to dispose, and the fix must leave it alone: disposing it would
// free the collection the holder still owns, and cascade-dispose its elements
// -- a double free / heap-use-after-free, which is worse than a leak. The row
// asserts the run is CLEAN (zero leaked allocations, zero sanitizer reports).
//
// (The same shape for a USER class is the #7906 suite's soundness row; this is
// its owning-collection twin, and it is the row that keeps the fix from
// reaching for a vtable dispatch: a `bz_array_t` has no Bezel vtable at word
// 0.)

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
// when it finds leaks), and the summary's own number when there is one. The
// pre-fix compiler produced 200 here (20800 B, the `bz_array_t` header plus
// the backing buffer of 100 iterations); the fixed compiler produces 0.
//
// `leakedBytes` is the same measurement in bytes (20800 pre-fix).
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
    // oracle measured (the pre-fix compiler reads 200 allocations / 20800
    // bytes here, from `bz_array_alloc`).
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
}

}  // namespace

// THE ISSUE'S OWN REPRO, verbatim: `Mk(): owned OwningArray<Cell>` whose
// result is the receiver of a further call (`Mk().Clear()`). 100 iterations;
// the receiver of every `Clear` is an unbound owned temporary that §28.3
// K8.1 disposes at the full expression's end -- with the COLLECTION'S OWN
// release arm (`bz_array_free` plus the §28.11 element cascade), never a
// vtable dispatch.
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_array_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// THE WHOLE FAMILY, one row per head. The release is DIFFERENT per head --
// `bz_array_free` for the array-backed ones, `bz_dict_free` for the map, and
// `bz_llist_free` for the linked list -- so each row pins a different arm of
// the same rule. Pre-fix: 41600 B / 300 allocations (OwningDictionary),
// 20800 B / 200 (OwningSet / OwningStack / OwningQueue),
// 6400 B / 100 (OwningLinkedList).
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_dictionary_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningDictionary<string, Cell>
    var d := new OwningDictionary<string, Cell>();
    d.Add("k", new Cell());
    return d;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_set_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningSet<Cell>
    var s := new OwningSet<Cell>();
    s.Add(new Cell());
    return s;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_stack_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningStack<Cell>
    var s := new OwningStack<Cell>();
    s.Push(new Cell());
    return s;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_queue_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningQueue<Cell>
    var s := new OwningQueue<Cell>();
    s.Enqueue(new Cell());
    return s;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_linkedlist_call_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningLinkedList<Cell>
    var s := new OwningLinkedList<Cell>();
    s.Add(new Cell());
    return s;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// THE PROPERTY-READ FACE of the same rule. §28.3.11 lists "as a member or
// index access" among the BORROWING USES of an unbound owned temporary, so
// `Mk().Count` is disposed at the full expression's end too. MEASURED
// pre-fix: 22400 B / 300 allocations -- the array backing AND the element it
// held (the cascade never ran, so the element leaked as well).
//
// The BORROWING collection family's property face is ALREADY reclaimed inline
// (`freeUnboundOwnedBorrowingArrayTemp`), so this face is claimed for the
// OWNING family only -- claiming the borrowing family here would free it
// twice.
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_array_property_receiver_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
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
      t += Mk().Count;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// THE SOUNDNESS ROW (§28.1). `Holder.Get()` returns the receiver's own
// owning-collection FIELD -- a BORROW, not an `owned` transfer -- so the
// collection is the HOLDER's to dispose, not the caller's. The fix must leave
// it alone: freeing it would be a double free (and the §28.11 element cascade
// would dispose the elements the holder still owns). The row asserts the run
// is CLEAN -- zero leaked allocations, zero sanitizer reports -- so it is RED
// if the borrow is ever disposed.
//
// (The pre-fix compiler also passes this row: the borrow was never claimed.
// The row's job is to stay GREEN, and to be non-vacuous -- see the report.)
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           borrowed_owning_array_receiver_is_not_disposed_spec_28_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class Holder
  private owned _f: OwningArray<Cell> := new OwningArray<Cell>();
  public method Get(): OwningArray<Cell> => _f;
end Holder;
class C
  public static method Main(): int
    using var h := new Holder();
    for var i in 0..<100 do
      h.Get().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// TWO receivers in one function -- each must be released EXACTLY ONCE, at its
// own boundary. Two temporaries in ONE expression is the demanding shape (two
// slots, two K8 registrations, two releases); two statements is the simple
// shape. Both are pinned here. Pre-fix: 44800 B / 400 allocations for the
// one-expression shape.
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_array_two_receivers_in_one_expression_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
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
      t += Mk().Count + Mk().Count;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "200\n");
}

BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_array_two_receiver_statements_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// K8.1's EXCEPTION EDGE: "on the normal edge as on the exception edge". The
// receiver (`Mk()`) is produced FIRST (§22.5: the receiver precedes the
// argument list) and the ARGUMENT then throws, so the receiver is an
// already-produced, not-yet-consumed unbound owned temporary at the moment
// the unwinding starts -- §28.3's *Exception-path discharge* disposes it
// during unwinding. Pre-fix, all 100 receivers leaked on this edge too
// (22400 B / 300 allocations).
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           owning_array_exception_edge_loop_100_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Boom(): int
    throw new Exception("x");
  end Boom;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      try
        t += Mk().Count + Boom();
      catch Exception => t += 1;
      end try;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL"),
                 "100\n");
}

// A BOUND collection: its owner is the local, which disposes it at the
// scope's end (§28.3's auto-drop for an owned local). The fix must NOT also
// dispose it at the full expression's end -- that would free it TWICE, and
// ASan would report the double free. The row's oracle is the leak COUNT: zero,
// because the scope's own disposal reclaims the collection exactly once.
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           bound_owning_array_receiver_is_not_disposed_spec_28_3) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      var a := Mk();
      a.Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                 "0\n");
}

// A chain root whose RESULT borrows FROM it: `MkS().Peek()` hands back a
// borrow of an element the collection owns (§28.3's borrowing-use rule). The
// result is a CLASS, so the fix's result gate refuses the claim and the
// collection is NOT disposed at the boundary -- the pre-fix behaviour, and the
// sound direction (a leak, never a dangling result).
//
// THE ORACLE HERE IS "NO MEMORY ERROR", not "no leak": the collection's NOT
// being disposed IS a leak in this row, so LSan is off
// (`runAotCheckedLenient`) while the generated-code AddressSanitizer stays
// active. If the fix disposed the collection anyway, the element the result
// points at would be freed under it -- a heap-use-after-free, which ASan
// reports and which fails the row (the program would not run to completion).
// MEASURED on the fixed compiler: 22400 B / 300 allocations leak BY DESIGN
// here, and the program prints its sum with no sanitizer error.
//
// fix #8033 (K8.4, stdlib §4.11, v0.355): this row used to BIND the borrow
// (`var x: Cell := MkS().Peek();`), which K8.4 rejects ("rejected, never
// lifetime-extended … among them a binding"). The bound spelling now pins the
// rejection, and the subject is exercised within the full expression.
BEZEL_TEST(owned_owning_collection_chain_receiver_8009,
           peek_result_keeps_the_collection_alive_spec_28_3) {
    {
        const JitResult bound = runAotChecked(R"BZL(
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
)BZL");
        BEZEL_EXPECT_TRUE(
            bezel::test::failedAtCompileTimeWithRealLocation(bound));
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
            bound, "over an unbound owned temporary receiver is a borrow "
                   "whose source is that temporary"));
    }
    const JitResult r = bezel::test::runAotCheckedLenient(R"BZL(
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
      t += MkS().Peek().v;
    end for;
    Console.WriteLine(t); return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
    // A generated-code memory error would have shown up as an ASan report in
    // the program's diagnostics; name it explicitly so a failure says WHICH
    // memory error it was.
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
}
