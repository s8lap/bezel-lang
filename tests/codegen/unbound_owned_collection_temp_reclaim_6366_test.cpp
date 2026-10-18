// Codegen — #6366 (narrow) / §28.3 K8 + §28.9 + §17.13, stdlib §4.6/§4.7/§4.8
// (v0.338).
//
// THE ISSUE. §28.3 K8 discharges an unbound owned temporary "at the end of the
// full expression in which it is produced", and for an expression statement /
// a member call on an unbound receiver that boundary is the end of the
// statement. Three faces did not reach a reclaim: an unbound `owned`-producing
// call consumed as `.Sum()`'s receiver, and the same call as a BARE statement
// (its result discarded entirely).
//
// THE ASYMMETRY THAT IDENTIFIES IT. The byte-identical `_ := f.Make();`
// binding is CLEAN on every family — the reclaim fires when the temporary is
// CONSUMED (the DiscardStmt arm, #1015, already routed it through
// `freeUnboundOwnedBorrowingArrayTemp`) and did not fire when it was DISCARDED.
// Everything here is measured on that contrast, not on an absolute.
//
// MEASURED on this fix's parent (r30-c1 @ 7201a5bc + #6372), 101 iterations,
// `bezelc --sanitize=address`:
//
//   shape                        LinkedList<int>    Array<int>
//   f.Make().Sum()                 28,280 B          12,120 B
//   f.Make();  (bare statement)     14,140 B          12,120 B
//   _ := f.Make();                    0 B                0 B
//
// and 0 on every face after the fix, except the `lazyTerminal` family below,
// which this commit does NOT fix.
//
// THE FIX, two sites, both routing through the EXISTING helper — no new free
// logic and no new name list:
//
//   * `LlvmBackend_Calls.cpp`, the `Sum` arm's return. The `.Length`/`.Count`
//     arm already ends with `freeFreshInputTemp()`; `Sum` read its accumulator
//     out of `accSlot` and returned without it.
//
//   * `LlvmBackend_Stmts.cpp`, the bare `ExpressionStmt` fall-through. #2655's
//     arm above it fires only for §21.9's `ownedProduced` set, which requires
//     `kind == Class` — a builtin collection is not a Class, so the arm had
//     already returned and nothing reclaimed the backing. The new arm reuses the
//     same helper the `_ :=` DiscardStmt arm uses (#1015).
//
// NARROW — THE `lazyTerminal` FAMILY IS DELIBERATELY LEFT, ON EVERY COLLECTION
// FAMILY. (Corrected 2026-10-17 after review: this header originally named only
// `LinkedList<T>.Sum()`, which understated the residual — it is NOT a
// LinkedList-specific leak, and `Array<T>` has two leaking faces too. Anything
// scoping the follow-up to `LinkedList` would miss them.)
//
// The discriminator is NOT the collection family; it is whether the receiver
// takes the `lazyTerminal` MATERIALIZATION path. MEASURED at this commit's own
// tip, 101 iterations, `bezelc --sanitize=address`:
//
//   shape                          receiver        result
//   f.Make().Sum()                 Array<int>       CLEAN  (fixed here)
//   f.Make();                      Array<int>       CLEAN  (fixed here)
//   f.Make();                      LinkedList<int>  CLEAN  (fixed here)
//   f.Make().Sum()                 LinkedList<int>   28,280 B
//   f.Make().ToArray()             Array<int>       12,120 B
//   f.Make().Where(...).Sum()      Array<int>       12,120 B
//
// `LinkedList<T>` is never array-backed, so EVERY terminal on it materializes
// and every one leaks; an `Array<T>` receiver materializes only when the
// terminal is a LINQ one (`ToArray`) or when a lazy operator precedes it
// (`Where(...).Sum()`), and those leak identically. So the residual is: any
// unbound `owned` producer consumed as a receiver that takes the `lazyTerminal`
// path, whatever its family.
//
// WHY. On that path `lowerToLazyEnumerator(*ma->target, …)` lowers the receiver
// expression ITSELF, so by the time `recv` is rebound to the `bz_lazy_to_array`
// snapshot the source value exists only inside that callee;
// `freeFreshInputTemp` correctly early-returns (`lazyTempArr` is set — that IS
// `recv`), leaving the source unreclaimed. Closing it needs an out-param for the
// wrapped source on `lowerToLazyEnumerator`, i.e. an interface change reaching
// every LINQ terminal, which is its own change.
//
// It is NOT pinned here, and deliberately so: a row asserting a leak as the
// EXPECTED outcome is a VACUOUS oracle — it passes while the leak persists and
// INVERTS (turns red) when the leak is fixed, which is the wrong way round for
// a regression suite. #6380 carries this scoping.
//
// NO DICTIONARY ROW. `Dictionary<K,V>.Sum()` is a DEGENERATE no-dispatch case
// (`warning: codegen cannot dispatch member 'Sum' on receiver of type
// 'Dictionary'`), so its byte count is not this defect and pinning it would
// measure the warning, not the leak.
//
// ORACLE. `runAotChecked` is always ASan and returns nonzero on ANY sanitizer
// finding, so each leak row fails the RUN rather than merely missing a finding.
// `leaksAtLeast` is deliberately NOT used: it is a leak oracle, not an ASan
// oracle, and these rows are genuinely fixed.
//
// SPEC BASIS. §28.3 K8 (unbound owned temporaries; end of full expression /
// end of statement), §28.9 (the compiler frees the undischarged value at its
// owner's release point), §17.13 (the discard spelling), §21.9 (the expression
// statement boundary), stdlib §4.6/§4.7 (Array/LinkedList), §4.8.
// Expected values: `Make()` builds [1, 2, 3]; `acc` counts one pass, so 101
// iterations give 101, whose `mod 100` residue is 1.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Two producers over the same element type, so every row is a family SWAP with
// nothing else changing. `LinkedList<T>` is `bz_llist_t`-backed and
// `Array<T>` is `bz_array_t`-backed, so the two exercise the helper's
// `freeIsLlist` arm and its default arm respectively (#6352).
std::string makeLlist() {
    return "public class F "
           "  public method Make(): owned LinkedList<int> "
           "    var l: LinkedList<int> := new LinkedList<int>(); "
           "    l.Add(1); l.Add(2); l.Add(3); "
           "    return l; "
           "  end Make; "
           "end F; ";
}

std::string makeArray() {
    return "public class F "
           "  public method Make(): owned Array<int> "
           "    return [1, 2, 3]; "
           "  end Make; "
           "end F; ";
}

// `acc` counts one pass, so 101 iterations print 101 and return `101 mod 100`
// = 1. Both are well under 256, so the exit code is asserted without relying on
// 8-bit truncation, and stdout carries the untruncated count.
std::string driver(const std::string& decl, const char* moduleName,
                   const char* stmt) {
    return std::string("module ") + moduleName + "; " + decl +
           "public class M "
           "  public static method Main(): int "
           "    var f := new F(); "
           "    var acc := 0; "
           "    for var i in 0 .. 100 do "
           "      " + stmt +
           "      acc := acc + 1; "
           "    end; "
           "    Console.WriteLine($\"{acc}\"); "
           "    return acc mod 100; "
           "  end Main; end M;";
}

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

constexpr int kResidueOf101 = 1;  // 101 mod 100

// Run one face and assert both oracles: the run completed (no sanitizer
// finding, which for `runAotChecked` means exit 0 is the ONLY clean result) and
// the loop really ran 101 times.
void expectClean(const std::string& prog) {
    JitResult r = checked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf101);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}

}  // namespace

// ===========================================================================
// FACE 1 — `.Sum()` on an unbound owned Array temporary. 12,120 B / 101 at the
// fix's parent; 0 after.
// ===========================================================================
BEZEL_TEST(unbound_owned_collection_temp_reclaim_6366,
           array_sum_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeArray(), "M6366A", "f.Make().Sum();"));
}

// ===========================================================================
// FACE 2 — the LinkedList twin of FACE 1's receiver. The `.Sum()` face for
// LinkedList is NOT covered (see the file header); this row is the LINKEDLIST
// family's own coverage of the BARE-STATEMENT face below, so it is stated here
// only once, against the family whose free the helper dispatches through its
// `freeIsLlist` arm.
// ===========================================================================
BEZEL_TEST(unbound_owned_collection_temp_reclaim_6366,
           linkedlist_bare_statement_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeLlist(), "M6366B", "f.Make();"));
}

// FACE 2's Array twin.
BEZEL_TEST(unbound_owned_collection_temp_reclaim_6366,
           array_bare_statement_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeArray(), "M6366C", "f.Make();"));
}

// ===========================================================================
// THE CONTROLS — the asymmetry this issue is defined by. These are green
// BOTH before and after the fix (measured 0 B on the parent). They are here so
// a future over-eager reclaim cannot pass by freeing these too: an
// over-eager free of a still-owned value shows up in ASan as a non-leak
// finding — a use-after-free or double-free — and `runAotChecked` returns
// nonzero for those too, so the same oracle covers both directions.
// ===========================================================================
BEZEL_TEST(unbound_owned_collection_temp_reclaim_6366,
           bound_discard_binding_stays_clean_on_both_families_spec_28_3) {
    expectClean(driver(makeArray(), "M6366D", "_ := f.Make();"));
    expectClean(driver(makeLlist(), "M6366E", "_ := f.Make();"));
}

// A producer BOUND to a local the loop must NOT free. `isUnboundOwnedBorrowing-
// ArrayCallExpr` matches only a CallExpr used directly as the receiver, so this
// binding is never matched and is discharged by its own owner at the end of the
// enclosing scope (§28.3's must-use auto-drop, which the compiler warns about
// here). Reading `.Count` back INSIDE the pass is what makes an over-eager
// free loud — ASan reports the read of the freed node chain, and
// `runAotChecked` returns nonzero for a use-after-free just as it does for a
// leak, so this row covers both directions of the same oracle.
// 101 passes x 3 elements = 303, whose `mod 100` residue is 3.
BEZEL_TEST(unbound_owned_collection_temp_reclaim_6366,
           bound_local_producer_is_never_freed_by_the_loop_spec_28_3) {
    JitResult r = checked(
        std::string("module M6366F; ") + makeLlist() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var l := f.Make(); "
        "      acc := acc + l.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("303") != std::string::npos);
}

// The plain (non-ASan) leg of FACE 1, so the value is pinned on an oracle
// that does not depend on the sanitizer runtime at all. `f.Make().Sum()` is
// 1+2+3 = 6 per pass; the accumulator is what makes the loop non-foldable.
BEZEL_TEST(unbound_owned_collection_temp_reclaim_6366,
           array_sum_value_is_correct_on_the_plain_leg_spec_28_3) {
    JitResult r = runAot(
        std::string("module M6366G; ") + makeArray() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      acc := acc + f.Make().Sum(); "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // 606 mod 100 = 6
    BEZEL_EXPECT_TRUE(r.stdoutText.find("606") != std::string::npos);
}