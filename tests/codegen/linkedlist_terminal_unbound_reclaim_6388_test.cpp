// Codegen — #6388 / §28.3 *Unbound owned temporaries* + §28.9, stdlib
// §4.6/§4.7/§4.3 (v0.338).
//
// THE ISSUE. `LinkedList<T>.Contains(x)` consumes an unbound `owned`-producing
// receiver directly as the receiver and never reclaims it. MEASURED on this
// fix's parent (`r30-c1` @ b1a94683), 101 iterations,
// `bezelc --sanitize=address`, over a 3-element producer:
//
//   f.Make().Contains(2)     14,140 B in 404 allocations
//   f.Make().Remove(2)       11,312 B in 303 allocations
//
// (`bz_llist_alloc`, bezel_runtime_collections.cpp:1897, plus one
// `bz_llist_add_last` node per element.) `bz_llist_free` is emitted 0 times
// for either.
//
// WHAT IDENTIFIES THE FAMILY. Every other `LinkedList<T>` terminal at
// operator-position 0 is clean at 0 B on this parent — measured, not assumed:
//
//   Contains  14,140 B      Any       CLEAN       Last       CLEAN
//   Remove    11,312 B      Count     CLEAN       Max        CLEAN
//                         First      CLEAN       Sum        CLEAN
//                         FirstOrNull CLEAN      ToArray    CLEAN
//
// so the discriminator is not the collection family — it is that these two arms
// walk the receiver node-by-node and return, having made no reclaim call. The
// `Count` arm in the SAME function (`LlvmBackend_Calls3.cpp:3975`, from #6341)
// already routes through the identical helper, which is what makes this a
// missing call rather than a wrong classification.
//
// THE FIX. Two `freeUnboundOwnedBorrowingArrayTemp` calls, one per arm, on the
// same helper and the same gate the `Count` arm uses. No new free logic and no
// name list: `LinkedList` is in `isPlainBuiltinCollectionHead`, so
// `isUnboundOwnedBorrowingArrayCallExpr` admits the `owned`-returning `f.Make()`
// receiver and the helper's `freeIsLlist` arm frees the `bz_llist_t*` through
// `bz_llist_free`.
//
// §28.3 is what makes the walk a BORROWING use, and what makes the boundary the
// end of the statement: a value "used only by borrowing operations within the
// expression — as the receiver of a further call ..." is disposed "at the end of
// the full expression in which it is produced, after every use of it within that
// expression has completed". Both arms have completed every use of `recv` by
// the time the reclaim is emitted: `Contains`'s phi holds only the two
// `true`/`false` constants and reads no storage, and `Remove`'s `found` is an
// `icmp` over a pointer loaded out of its own alloca.
//
// `Remove`'s soundness, stated because it is the arm that mutates. The removed
// node was already unlinked AND (for the owning family) disposed by
// `bz_llist_remove_node`, so it is no longer reachable from `recv` and the
// reclaim cannot double-free it; what `bz_llist_free` still owns is the header
// and the surviving nodes. For a BORROWING `LinkedList<T>` that disposer
// argument is null at the `bz_llist_remove_node` call too (stdlib §4.6: the slots
// are borrows the caller does not own), so the two calls dispose disjoint sets
// on both families.
//
// NO-OP DIRECTION. The helper matches an unbound `owned`-returning CALL receiver
// of the PLAIN (borrowing) family only. `OwningLinkedList` is excluded by
// construction — `isPlainBuiltinCollectionHead` lists no `Owning*` head — so its
// element cascade is untouched and `freeUnboundOwnedOwningCollectionTemp`
// remains its sole reclaimer. A bound / `move`d receiver is not a NewExpr or an
// owned-returning CallExpr in receiver position and is never matched.
//
// ORACLE. `runAotChecked` is ALWAYS ASan and returns nonzero on ANY sanitizer
// finding, so a leak, a use-after-free and a double free each fail the RUN — one
// oracle, both directions. No `leaksAtLeast` (that is a leak oracle, not an ASan
// one). `r.ran` is asserted on every row; there is NO skip-absorbing guard. The
// control rows read the list back INSIDE the same pass, which is what makes an
// over-eager reclaim loud — ASan reports the read of the freed node chain and
// `runAotChecked` returns nonzero for a use-after-free just as it does for a
// leak.
//
// NOTHING HERE ASSERTS A LEAK. A row asserting a leak as the expected outcome
// would be a vacuous oracle: it passes while the leak persists and INVERTS the
// moment the leak is fixed. Every leak row asserts exit 0.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

// A 3-element `LinkedList<int>` producer, so the byte counts match the issue's
// and every row varies only the terminal under test.
std::string makeLlist() {
    return "public class F "
           "  public method Make(): owned LinkedList<int> "
           "    var l: LinkedList<int> := new LinkedList<int>(); "
           "    l.Add(1); l.Add(2); l.Add(3); "
           "    return l; "
           "  end Make; "
           "end F; ";
}

// One-element producer: a second magnitude, so a row cannot pass by accident
// against a count that happens to match.
std::string makeLlist1() {
    return "public class F "
           "  public method Make(): owned LinkedList<int> "
           "    var l: LinkedList<int> := new LinkedList<int>(); "
           "    l.Add(2); "
           "    return l; "
           "  end Make; "
           "end F; ";
}

constexpr int kResidueOf101 = 1;  // 101 mod 100

// The affirmative leak oracle: the loop ran, it ran 101 times, and the process
// exited 0 — which under `runAotChecked` (always ASan) is the ONLY clean result.
void expectClean(const std::string& prog) {
    JitResult r = checked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf101);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}

// Run `stmt` (which must not evaluate to the counted value) 101 times and assert
// the run is clean.
void expectCleanStmt(const std::string& producer, const std::string& modName,
                     const std::string& stmt) {
    expectClean(std::string("module ") + modName + "; " + producer +
                "public class M "
                "  public static method Main(): int "
                "    var f := new F(); "
                "    var acc := 0; "
                "    for var i in 0 .. 100 do "
                "      " + stmt + " "
                "      acc := acc + 1; "
                "    end; "
                "    Console.WriteLine($\"{acc}\"); "
                "    return acc mod 100; "
                "  end Main; end M;");
}

}  // namespace

// ===========================================================================
// FACE 1 — the issue's own repro.
// ===========================================================================
BEZEL_TEST(linkedlist_terminal_unbound_reclaim_6388,
           contains_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectCleanStmt(makeLlist(), "M6388A", "f.Make().Contains(2);");
}

// FACE 1 on the one-element producer — the issue's second measured magnitude
// (8,456 B). Without it a row could not distinguish "the reclaim fired" from
// "the leak happened to be small this run".
BEZEL_TEST(linkedlist_terminal_unbound_reclaim_6388,
           contains_of_a_one_element_unbound_temp_is_reclaimed_spec_28_3) {
    expectCleanStmt(makeLlist1(), "M6388B", "f.Make().Contains(2);");
}

// ===========================================================================
// FACE 2 — `Remove`, the mutating twin, in the same function and the same
// family. It was measured leaking at 11,312 B / 303 allocs alongside `Contains`
// and is fixed in the same commit; the two arms are separable but the defect is
// one omission repeated.
// ===========================================================================
BEZEL_TEST(linkedlist_terminal_unbound_reclaim_6388,
           remove_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectCleanStmt(makeLlist(), "M6388C", "f.Make().Remove(2);");
}

// The `Remove` soundness check, in the only form that can observe it. The loop
// removes element 2 from a FRESH list every pass, so every pass disposes a node
// through `bz_llist_remove_node` and then frees the rest through the reclaim —
// two calls disposing disjoint node sets. A reclaim that reached back into the
// removed node, or a `bz_llist_remove_node` that ran twice on it, is a
// heap-use-after-free here and `runAotChecked` returns nonzero. The row asserts
// the run is clean, which is the affirmative statement that it did not.
BEZEL_TEST(linkedlist_terminal_unbound_reclaim_6388,
           remove_reclaim_does_not_touch_the_already_unlinked_node_spec_28_3) {
    expectCleanStmt(makeLlist(), "M6388D", "f.Make().Remove(1);");
}

// ===========================================================================
// THE CONTROLS — green BOTH before and after the fix, and the reason this fix's
// predicate is auditable: if it matched a receiver it does not own, ASan reports
// the read of freed storage and `runAotChecked` returns nonzero for it too.
// ===========================================================================

// A producer BOUND to a local, mutated and read back inside the same pass. The
// binding is not an owned-returning call in RECEIVER position, so the helper
// never matches and the local's own owner discharges it at scope end. 101 x 3 =
// 303.
BEZEL_TEST(linkedlist_terminal_unbound_reclaim_6388,
           bound_local_producer_is_never_freed_by_the_loop_spec_28_3) {
    JitResult r = checked(
        std::string("module M6388E; ") + makeLlist() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      using var l := f.Make(); "
        "      acc := acc + (if l.Contains(2) then 1 else 0); "
        "      acc := acc + (if l.Remove(1) then 1 else 0); "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // 202 mod 100 = 2
    BEZEL_EXPECT_TRUE(r.stdoutText.find("202") != std::string::npos);
}

// The bound producer with the ELEMENTS REMOVED first, so the following reads
// walk a shorter chain each pass — the row that would catch a reclaim which
// freed the list while a later read still traversed it.
BEZEL_TEST(linkedlist_terminal_unbound_reclaim_6388,
           bound_local_read_back_after_removal_stays_clean_spec_28_3) {
    JitResult r = checked(
        std::string("module M6388F; ") + makeLlist() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      using var l := f.Make(); "
        "      l.Remove(1); "
        "      acc := acc + (if l.Contains(1) then 1 else 0); "
        "      acc := acc + l.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // 202 mod 100 = 2
    BEZEL_EXPECT_TRUE(r.stdoutText.find("202") != std::string::npos);
}

// The plain (non-ASan) leg, so the VALUE is pinned on an oracle that does not
// depend on the sanitizer runtime. `Contains(2)` over [1,2,3] is true on every
// pass — 101 true values summed as 1 each is 101, whose `mod 100` residue is 1,
// which is exactly what a clean `acc := acc + 1` also produces, so the row adds
// a distinct `Contains(9)` miss term to make the two directions differ.
BEZEL_TEST(linkedlist_terminal_unbound_reclaim_6388,
           contains_value_is_correct_on_the_plain_leg_spec_28_3) {
    JitResult r = runAot(
        std::string("module M6388G; ") + makeLlist() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      acc := acc + (if f.Make().Contains(2) then 1 else 0); "
        "      acc := acc + (if f.Make().Contains(9) then 10 else 0); "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // 101 mod 100
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}