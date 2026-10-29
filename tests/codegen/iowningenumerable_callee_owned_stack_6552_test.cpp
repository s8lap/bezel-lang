// §28.11 / §29.1 / §29.2 (v0.338) — #6552: `wrapOwnedStackAsIEnumerator` was
// the THIRD owning sibling of the leak #6544 closed for the array and
// LinkedList twins, and it iterated in the REVERSE order, so its drain had to
// be settled by measurement rather than by copying #6544's work.
//
// THE RULE. Two clauses meet at an `IOwningEnumerable<T>` head.
//   * §29.1 `IOwningEnumerator<T>.TakeCurrent()` "transfers ownership of the
//     most-recently-yielded value to the caller, leaving the enumerator's
//     internal slot empty", and §29.2's for-in takes exactly once per
//     successful MoveNext. So ownership LEAVES the collection at each yield —
//     the head DRAINS.
//   * §28.11's `Dispose()` row for an owning family: "disposes every live
//     element ... then releases storage".
// Together: what the frame owes at its own scope end is EVERY element that no
// yield took, plus the storage. A callee-owned collection returned at that
// head makes the frame the collection's SOLE owner (§28.3 K3 — the callee's
// local is gone), so nobody else can pay it.
//
// THE DEFECT. `wrapOwnedStackAsIEnumerator` installed the plain BORROWING
// MoveNext/Current at this head (no drain) and, via #6439's `consultHeader = 0`,
// freed the backing with a null element disposer — so a never-read binding
// stranded every element. MEASURED on `c31298da`: a callee-owned
// `OwningStack<Item>` bound at an `IOwningEnumerable<Item>` head and never
// iterated leaks 32 B in 2 allocations from `bz$M.C$Build`, byte for byte the
// array and LinkedList twins.
//
// THE ORDER — the one thing that does NOT transfer from #6544. stdlib §4.11
// "Iteration order": "All four types iterate (via `IReadOnlyCollection<T>`) in
// take order — a stack top-to-bottom (pop order), a queue front-to-back
// (dequeue order)". The default drain, `bz_array_drain_next`, pops index 0 of
// the backing array — for a stack that is the BOTTOM, the wrong end. So the
// fix asks for `lifo`, selecting `bz_array_drain_next_lifo` (stdlib §4.11's own
// `OwningStack<T>.Drain()`). MEASURED on `c31298da`, pushing 1,2,3 and reading
// at this head prints 3,2,1 — TOP-FIRST — so the pre-fix READ path was already
// §4.11-correct and `lifo = true` reproduces it rather than changing it.
//
// ORACLE. `runAotChecked` compiles and runs with GENERATED-CODE ASan/LSan
// in-process (a leak OR a use-after-free / double-free turns `ran` false) plus
// the EXACT destructor count on stdout — not `leaksAtLeast`, which is a leak
// oracle and not an ASan oracle. A printed oracle is only meaningful if what
// it printed is also the right number, so every row asserts the count, and a
// `did-not-run` sentinel keeps a leak distinguishable from a wrong count.
//
// Each row runs the binding inside `Inner` and prints `Item._n` from `Main`
// AFTER `Inner` returns, so the count observed is the count at the frame's own
// scope end.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT harness
#include "test_harness.hpp"
#include "native_test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A class with a counting destructor, and a factory that returns a callee-OWNED
// `OwningStack<Item>` at an `IOwningEnumerable<Item>` head — the §28.3 K3
// transfer that leaves the frame as sole owner. `body` is what `Inner` does
// with the bound value; `iters` how many elements the stack holds, so a
// partial read leaves a measurable remainder.
//
// `Push` is the stack family's own insert (§4.11's take order is pop order, so
// the LAST pushed is the FIRST read) — it is the `Add` spelling of the twins
// renamed, not a different collection.
std::string program(const std::string& body, int iters) {
    std::string adds;
    for (int i = 0; i < iters; ++i) adds += "c.Push(move (new Item())); ";
    return
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Build(): IOwningEnumerable<Item> "
        "    var c := new OwningStack<Item>(); " + adds +
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int " + body +
        "  end Inner; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    Console.WriteLine(Item._n); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// `using var d := C.Build();` is the §28.3 scope-bound discharge. The body is
// what the loop does with each yield; an empty body is the never-iterated
// case, which is the shape #6552 reports.
std::string binding(const std::string& body) {
    return "using var d := C.Build(); " + body + " return 0;";
}

// Run under generated-code ASan/LSan. Returns the program's stdout, or a
// sentinel that no program prints — so a leak, a UAF, a double-free, and a
// compile reject are all distinguishable from a WRONG COUNT, which is the
// vacuous-oracle trap a printed-output assertion walks into if it only checks
// "it ran".
const char* kDidNotRun = "<did-not-run>";

std::string checkedStdout(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::cout << "    [6552] program did not run cleanly (compile reject, "
                     "leak, or sanitizer abort)\n";
        return kDidNotRun;
    }
    return r.stdoutText;
}

}  // namespace

// ---------------------------------------------------------------------------
// §28.11 / §29.2 — the reported shape. The value is bound at an
// `IOwningEnumerable<Item>` head and never iterated: every element is still
// LIVE at the frame's Dispose, so §28.11's row disposes all of them. Before the
// fix this leaked every element (32 B / 2 allocations, MEASURED).
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_stack_6552,
           bound_never_iterated_disposes_every_element_28_11) {
    std::string src = program(binding(""), 2);
    BEZEL_EXPECT_EQ(checkedStdout(src), "2\n");
}

// ---------------------------------------------------------------------------
// §28.11 — PARTIAL iteration is the same rule over a smaller remainder. The
// loop discharges what it took (§29.2 requires the loop variable to be
// discharged); §28.11's row is what pays for the rest. On a stack the un-yielded
// remainder is the BOTTOM `iters - 1` — the top was popped first — which is the
// order-sensitivity this leaf has that the array twin does not.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_stack_6552,
           partially_iterated_disposes_the_unyielded_remainder_28_11) {
    std::string src = program(
        binding("for var e in d do using var u := move e; break; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — the FULLY-ITERATED read path, which was already correct and must
// STAY correct: the drain empties the collection, so the frame's cascade finds
// nothing and frees storage only. A count of 3 (not 6) is the double-dispose
// oracle for the family; ASan would also flag it, but a printed exact count
// says so directly.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_stack_6552,
           fully_iterated_disposes_each_element_once_28_11) {
    std::string src = program(
        binding("for var e in d do using var u := move e; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — a SECOND enumeration of the same bound value. stdlib §4.13: "each
// GetOwningEnumerator() call on a live view yields a fresh enumerator over the
// collection's *current* remainder ... a re-enumeration continues draining what
// remains, never replaying what already left". So the second loop must yield
// NOTHING (the first drained it), and the frame's Dispose must still free the
// storage — a total of 3, not 6. This is the row that pins the §4.13 contract
// against a "reset to the top" reading of the drain, which on a stack is the
// reading that would replay.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_stack_6552,
           second_enumeration_drains_the_remainder_and_replays_nothing_4_13) {
    std::string src = program(
        binding("for var e in d do using var u := move e; end;"
                "for var e2 in d do using var u2 := move e2; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — amplified. A one-shot leak repro is a vacuous oracle (it can hide
// under the sanitizer's noise), so this row loops 200x x 3 elements = 600 and
// asserts the EXACT total. 600 truncates to 8 bits through the AOT exit path
// (600 mod 256 = 88), so the count goes on stdout, never in a return code (lane
// contract §9). The exact total is also the exactly-once oracle: a drain that
// disposed any element twice would print > 600.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_stack_6552,
           never_iterated_amplified_200x_no_leak_28_11) {
    std::string src =
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Inner(): int "
        "    var k := 0; "
        "    while k < 200 do "
        "      using var d := C.Build(); "
        "      k := k + 1; "
        "    end while; "
        "    return 0; "
        "  end Inner; "
        "  public static method Build(): IOwningEnumerable<Item> "
        "    var c := new OwningStack<Item>(); "
        "    c.Push(move (new Item())); c.Push(move (new Item())); "
        "    c.Push(move (new Item())); "
        "    return c; "
        "  end Build; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    Console.WriteLine(Item._n); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(checkedStdout(src), "600\n");
}

// ---------------------------------------------------------------------------
// §29.2 / stdlib §4.11 — THE LOAD-BEARING ROW. The drain must pop the TOP, not
// the bottom: pushes 1,2,3 must read 3,2,1. A drain built by copying #6544's
// array work — which pops index 0 — would satisfy every leak row above (all six
// elements are still disposed exactly once either way) and fail ONLY this one.
// That is what makes this row load-bearing rather than decorative, and it is
// why the fix's `lifo = true` is a measurement (the pre-fix read path on this
// same base already printed 3,2,1) rather than an inference from the spec text.
//
// The trailing count is the same 3 the array twin's order row asserts, from a
// different storage layout: §28.11's `Dispose()` order is reverse insertion
// order, the iteration order is pop order, and both are paid exactly once.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_stack_6552,
           drain_yields_top_first_storage_order_4_11) {
    std::string src =
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Build(): IOwningEnumerable<Item> "
        "    var c := new OwningStack<Item>(); "
        "    c.Push(move (new Item(1))); c.Push(move (new Item(2))); "
        "    c.Push(move (new Item(3))); "
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int "
        "    using var d := C.Build(); "
        "    for var e in d do "
        "      using var u := move e; "
        "      Console.WriteLine(u.V); "
        "    end; "
        "    Console.WriteLine(Item._n); "
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n2\n1\n3\n");
}

// ---------------------------------------------------------------------------
// §4.11 — the same order on the BORROWING `IEnumerable<T>` head, which this fix
// must NOT change: `wrapOwnedStackAsIEnumerator`'s body below the divert is the
// borrowing head only, and the borrow keeps every element at Dispose. This row
// is the guard against the fix having moved the order at the OTHER head (a
// `lifo` on the wrong call site would print 1,2,3 here).
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_stack_6552,
           borrowing_head_still_yields_top_first_4_11) {
    std::string src =
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Build(): IEnumerable<Item> "
        "    var c := new OwningStack<Item>(); "
        "    c.Push(move (new Item(1))); c.Push(move (new Item(2))); "
        "    c.Push(move (new Item(3))); "
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int "
        "    using var d := C.Build(); "
        "    for var e in d do "
        "      Console.WriteLine(e.V); "
        "    end; "
        "    return 0; "
        "  end Inner; "
        "  public static method Main(): int "
        "    C.Inner(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n2\n1\n");
}