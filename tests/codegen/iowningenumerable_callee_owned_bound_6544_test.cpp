// §28.11 / §29.1 / §29.2 (v0.338) — #6544: an `IOwningEnumerable<T>` value that
// is BOUND but never iterated (or only partially iterated) reclaimed the
// collection's storage and stranded every element it still held.
//
// THE RULE. Two clauses meet here.
//   * §29.1 `IOwningEnumerator<T>.TakeCurrent()` "transfers ownership of the
//     most-recently-yielded value to the caller, leaving the enumerator's
//     internal slot empty", and §29.2's for-in takes exactly once per
//     successful MoveNext. So at an `IOwningEnumerable<T>` head, ownership
//     LEAVES the collection at each yield — the head DRAINS.
//   * §28.11's `Dispose()` row for an owning family: "disposes every live
//     element ... then releases storage".
// Together: what the frame owes at its own scope end is EVERY element that no
// yield took, plus the storage. For a callee-owned collection returned at that
// head the frame is the collection's SOLE owner (§28.3 K3 — the callee's local
// is gone), so nobody else can pay it.
//
// THE DEFECT. `wrapOwnedArrayAsIEnumerable` / `wrapOwnedLinkedListAsIEnumerable`
// installed the plain BORROWING MoveNext/Current at this head (no drain) and
// freed the backing with a null element disposer — so a never-read binding
// leaked every element, and a PARTIAL read leaked the remainder. MEASURED on
// HEAD before the fix: `OwningArray<Item>` 32 B / 2 allocations from
// `bz$M.Item$ctor`, and the LinkedList twin byte for byte.
//
// WHY A DISPOSAL FIX IS THE DOUBLE-FREE-RISK ONE. The tempting one-character
// repair — consult the array's recorded header disposer, as the BORROWING head
// does — was MEASURED to trade the leak for `heap-use-after-free` in
// `bz$elemDispose` on the FULLY-ITERATED twin: without a drain the collection
// still names pointers the loop already freed. Draining first is what makes the
// cascade correct in every state, which is why these rows pin all three.
//
// ORACLE. `runAotChecked` compiles and runs the program with GENERATED-CODE
// ASan/LSan in-process (aot_support.hpp's `opts.sanitizeAddress = true`): a
// leak OR a use-after-free / double-free turns `ran` false, here, in this
// process. The disposal COUNT is asserted through the program's stdout rather
// than `leaksAtLeast` (a leak oracle, not an ASan oracle) — a printed oracle is
// only meaningful if what it printed is also the right number, so every row
// asserts the exact count of §28.11's "every live element".
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
// owning collection at an `IOwningEnumerable<Item>` head — the §28.3 K3
// transfer that leaves the frame as sole owner. `family` selects the twin
// (OwningArray / OwningLinkedList); the two must agree.
//
// `body` is what `Inner` does with the bound value. `iters` how many elements
// the collection holds, so a partial read leaves a measurable remainder.
std::string program(const std::string& family, const std::string& body,
                    int iters) {
    std::string adds;
    for (int i = 0; i < iters; ++i) adds += "c.Add(move (new Item())); ";
    return
        "module M; import System.Console; "
        "class Item "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end Item; "
        "class C "
        "  public static method Build(): IOwningEnumerable<Item> "
        "    var c := new " + family + "<Item>(); " + adds +
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
// case, which is the shape #6544 reports.
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
        std::cout << "    [6544] program did not run cleanly (compile reject, "
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
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           bound_never_iterated_disposes_every_element_28_11) {
    std::string src = program("OwningArray", binding(""), 2);
    BEZEL_EXPECT_EQ(checkedStdout(src), "2\n");
}

// The LinkedList twin, byte for byte the same obligation. The two leaves are a
// family, not a coincidence, and this row is what makes that claim testable.
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           bound_never_iterated_list_twin_disposes_every_element_28_11) {
    std::string src = program("OwningLinkedList", binding(""), 2);
    BEZEL_EXPECT_EQ(checkedStdout(src), "2\n");
}

// ---------------------------------------------------------------------------
// §28.11 — PARTIAL iteration is the same rule over a smaller remainder. The
// loop discharges what it took (§29.2 requires the loop variable to be
// discharged); §28.11's row is what pays for the rest. MEASURED before the fix
// as a leak of exactly the 2 un-yielded elements.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           partially_iterated_disposes_the_unyielded_remainder_28_11) {
    std::string src = program(
        "OwningArray",
        binding("for var e in d do using var u := e; break; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           partially_iterated_list_twin_disposes_the_unyielded_remainder_28_11) {
    std::string src = program(
        "OwningLinkedList",
        binding("for var e in d do using var u := e; break; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — the FULLY-ITERATED read path, which was already correct and must
// STAY correct: the drain empties the collection, so the frame's cascade finds
// nothing and frees storage only. A count of 3 (not 6) is the double-dispose
// oracle for the family; ASan would also flag it, but a printed exact count
// says so directly.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           fully_iterated_disposes_each_element_once_28_11) {
    std::string src = program(
        "OwningArray",
        binding("for var e in d do using var u := e; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           fully_iterated_list_twin_disposes_each_element_once_28_11) {
    std::string src = program(
        "OwningLinkedList",
        binding("for var e in d do using var u := e; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — a SECOND enumeration of the same bound value. stdlib §4.13: "each
// GetOwningEnumerator() call on a live view yields a fresh enumerator over the
// collection's *current* remainder ... a re-enumeration continues draining what
// remains, never replaying what already left". So the second loop must yield
// NOTHING (the first drained it), and the frame's Dispose must still free the
// storage — a total of 3, not 6. This is the row that pins the §4.13 contract
// against a "reset to the front" reading of the drain.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           second_enumeration_drains_the_remainder_and_replays_nothing_4_13) {
    std::string src = program(
        "OwningArray",
        binding("for var e in d do using var u := e; end;"
                "for var e2 in d do using var u2 := e2; end;"), 3);
    BEZEL_EXPECT_EQ(checkedStdout(src), "3\n");
}

// ---------------------------------------------------------------------------
// §28.11 — amplified. A one-shot leak repro is a vacuous oracle (it can hide
// under the sanitizer's noise), so this row loops 200x x 3 elements = 600 and
// asserts the EXACT total. 600 truncates to 8 bits through the AOT exit path,
// so the count goes on stdout, never in a return code (lane contract §9).
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
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
        "    var c := new OwningArray<Item>(); "
        "    c.Add(move (new Item())); c.Add(move (new Item())); "
        "    c.Add(move (new Item())); "
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

// The same amplification on the LinkedList twin — the two leaves' numbers must
// agree, which is the family claim stated as a number.
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           never_iterated_list_twin_amplified_200x_no_leak_28_11) {
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
        "    var c := new OwningLinkedList<Item>(); "
        "    c.Add(move (new Item())); c.Add(move (new Item())); "
        "    c.Add(move (new Item())); "
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
// §29.2 — the drain must NOT change the VALUES the loop observes. Iteration
// order for an ordered owning collection is storage order (§28.11), so the
// yielded sequence is 1,2,3 and not 3,2,1. A drain implemented by popping from
// the wrong end would satisfy every leak row above and fail this one; that is
// what makes this row load-bearing rather than decorative.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           drain_yields_in_storage_order_29_2) {
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
        "    var c := new OwningArray<Item>(); "
        "    c.Add(move (new Item(1))); c.Add(move (new Item(2))); "
        "    c.Add(move (new Item(3))); "
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int "
        "    using var d := C.Build(); "
        "    for var e in d do "
        "      using var u := e; "
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
    BEZEL_EXPECT_EQ(checkedStdout(src), "1\n2\n3\n3\n");
}

// The LinkedList twin of that order row. §28.11's `Dispose()` order is reverse
// INSERTION order and the ITERATION order is insertion order, so a list drained
// head-to-tail yields 1,2,3 — the same answer as the array, from a different
// storage layout. If the two twins ever disagree here, the drain picked the
// wrong end on one of them.
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_callee_owned_bound_6544,
           drain_yields_in_storage_order_list_twin_29_2) {
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
        "    var c := new OwningLinkedList<Item>(); "
        "    c.Add(move (new Item(1))); c.Add(move (new Item(2))); "
        "    c.Add(move (new Item(3))); "
        "    return c; "
        "  end Build; "
        "  public static method Inner(): int "
        "    using var d := C.Build(); "
        "    for var e in d do "
        "      using var u := e; "
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
    BEZEL_EXPECT_EQ(checkedStdout(src), "1\n2\n3\n3\n");
}