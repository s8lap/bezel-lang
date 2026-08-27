// Codegen / leak — fix #3419 (§28.6 *Leak prevention* / §29.2 *Desugaring of
// `for`* / §23.2 *Properties* / §28.3 *Local variables, ownership, and
// borrows*, v0.283; stdlib §4.7 `OwningLinkedList<T>`): a wrapper frame minted
// at a PROPERTY-READ face and never claimed, because the ownership-claim site
// re-derived the mint from a hand-written name list instead of asking the
// wrap-creator what it did.
//
// THE DEFECT — the umbrella #3419 pattern, at the one claim site that still
// carried a name list. Reading `public property Items: IEnumerable<Item> =>
// _s;` over an `owned _s: OwningLinkedList<Item>` cannot hand back the raw
// `bz_llist_t*`: §29.2's for-in desugaring drives an iterator-format vtable
// ("[0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current"), so
// `wrapPropertyArrayAsIEnumerable` `calloc`s a fresh wrapper frame over the
// source. `Emitter::registerIterableSourceDisposal` is the ownership CLAIM for
// that frame — nothing else ever frees one, because the field's own holder
// disposes the BACKING, not the wrap (§28.6).
//
// The claim re-spelled the admissible (declared head x getter family) grid by
// hand, and the spelling had drifted in BOTH directions at once:
//
//   * TOO NARROW. The list carried `LinkedList` but not `OwningLinkedList`, so
//     the owning twin's frame was minted and never claimed. It also could not
//     see the lazy-pipeline drain the authority mints for a getter body such
//     as `=> _a.Select(f)`.
//   * TOO WIDE. `getterTy->declaration != nullptr` claimed a frame for EVERY
//     declaration-carrying getter type, including the five paths on which
//     `wrapUserClassAsIEnumerable` declines and leaves the raw class instance
//     in the slot — where a slot-1 dispatch calls the interface-map GLOBAL as
//     a function (#2975's measured wild jump).
//
// The list was stale for a third reason: fix #3405 DELETED the property face's
// own array-family arm and routed every family through
// `wrapAsIEnumerableForTarget`, so the claim's comment describing itself as
// matching "the property-read wrap's positive list verbatim" named a list that
// no longer existed.
//
// MEASURED on 0c5ecd1 (this commit's parent), under generated-code ASan+LSan,
// 60 executions of `var e := c.Items;`:
//   `owned _s: OwningLinkedList<Item>`  -> 1920 B leaked in 60 allocations
//                                          (32 B per borrowing frame), exit 1
//   `owned _s: OwningStack<Item>`       -> CLEAN, exit 207 — a name the stale
//                                          list DID carry, which is what
//                                          identifies the defect as a
//                                          positive-list GAP rather than a
//                                          missing mechanism.
// Post-fix both are clean at exit 207. The two programs differ by exactly the
// collection head and its insert method; every other byte is identical.
//
// THE FIX. `wrapPropertyArrayAsIEnumerable` now self-reports every frame it
// mints for a read into `propReadIterWrappedExprs_`, keyed by the READ
// expression node, written at exactly one site under its own `result != v`
// observation, and CLEARED when it declines. The claim reads that report and
// spells no name list at all — CLAIM = MINT by construction, the shape #3384
// established for the decl-site coercion. The clear is not a passenger: a read
// lowered once per generic instantiation would otherwise inherit an earlier
// instantiation's WRAPPED verdict, and a claim over a frame that was never
// minted is a double-free, strictly worse than the leak this closes.
//
// ORACLE. `runAotChecked` compiles the generated program with AddressSanitizer
// and LeakSanitizer enabled, so `r.ran` is simultaneously a no-leak and a
// no-double-free assertion; the exit code carries the ownership observable
// (`Count * 100 + sum`), which pins that the frames were freed and the SOURCE
// was not.
//
// (Never put a C++ `//` comment inside these string literals: it is lexed as
// Bezel source and comments out the rest of the program, so the test would
// pass for the wrong reason. Bezel's own comment forms are `//`, `/* ... */`
// and `///` -- spec §1.5, which is exhaustive.)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The two programs differ ONLY in the owning collection head and the method
// that inserts into it. Everything else -- the stored-view property, the 60
// unclaimed reads, the iterated 61st, and the observable -- is byte-identical,
// which is what makes the pair a discriminating control rather than two
// unrelated programs.
std::string program(const char* head, const char* insert) {
    return std::string(
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  private owned _s: ") + head + "<Item>; "
        "  public constructor() "
        "    _s := new " + head + "<Item>(); "
        "    _s." + insert + "(new Item(3)); "
        "    _s." + insert + "(new Item(4)); "
        "  end constructor; "
        "  public property Items: IEnumerable<Item> => _s; "
        "  public property N: int => _s.Count; "
        "end C; "
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var i := 0; "
        "    while i < 60 do "
        "      var e := c.Items; "
        "      i := i + 1; "
        "    end while; "
        "    var total := 0; "
        "    var v := c.Items; "
        "    for var x in v do "
        "      total := total + x.V; "
        "    end for; "
        "    return c.N * 100 + total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
}

}  // namespace

// ---------------------------------------------------------------------------
// (A) THE PIN. `OwningLinkedList<Item>` behind a §28.6 stored-view property
//     read into a plain `var`. 60 wrapper frames minted and dropped, plus a
//     61st that is iterated.
//     per §28.6 (leak prevention), §29.2 (the vtable protocol the frame
//     implements), §23.2 (the expression-bodied property face), stdlib §4.7
// ---------------------------------------------------------------------------
BEZEL_TEST(property_face_wrapper_claim_3419,
           owninglinkedlist_stored_view_read_claims_wrapper_frame_28_6) {
    JitResult r = runAotChecked(program("OwningLinkedList", "AddLast"));
    if (wasSkipped(r)) return;
    // Pre-fix on 0c5ecd1: LeakSanitizer reports 1920 bytes in 60 allocations
    // and the process exits 1.
    BEZEL_EXPECT_TRUE(r.ran);
    // N == 2 and total == 7. This is the claim-SUBSET-OF-mint half, which
    // matters more than the leak: an `IEnumerable<T>`-typed source desugars via
    // the non-destructive `IEnumerator<T>` (§29.2), so both elements must still
    // be in the collection and still readable after 61 frames have been minted
    // AND dropped. A claim wider than the mint would have freed the backing the
    // field owns and this read would fault or come back short.
    BEZEL_EXPECT_EQ(r.exitCode, 207);
}

// ---------------------------------------------------------------------------
// (B) THE CONTROL. `OwningStack<Item>` -- a head the stale list DID carry --
//     through the byte-identical program. Clean on BOTH sides of the fix, so a
//     green here does not depend on the fix; it is what shows (A)'s failure was
//     a gap in a positive list rather than an absent mechanism, and it pins
//     that the rewrite did not drop a claim the old list made.
//     per §28.6, stdlib §4.11
// ---------------------------------------------------------------------------
BEZEL_TEST(property_face_wrapper_claim_3419,
           owningstack_stored_view_read_control_stays_claimed_28_6) {
    JitResult r = runAotChecked(program("OwningStack", "Push"));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // stdlib §4.11 "Iteration order" is pop order, so the sum is order-
    // independent by construction: 3 + 4 == 7 either way, and the assertion is
    // about the CLAIM, not the order (which #3405's test pins).
    BEZEL_EXPECT_EQ(r.exitCode, 207);
}

// ---------------------------------------------------------------------------
// (C) THE TOO-WIDE HALF -- the direction that is a WILD JUMP, not a leak.
//
//     The stale claim list also gated on `getterTy->declaration != nullptr`,
//     which claimed a wrapper frame on every one of
//     `wrapUserClassAsIEnumerable`'s DECLINE paths. This program takes one of
//     them: `Src`'s `GetEnumerator` returns an iterator-METHOD result, so no
//     `TypeSymbol` is structurally an enumerator, `enumTs` stays null, and the
//     wrap declines -- while the old predicate claimed a frame anyway and the
//     scope-end disposal then called the interface-map GLOBAL as a function.
//
//     Measured on 0c5ecd1 (`bezelc` built from that tree, generated-code ASan):
//
//       AddressSanitizer: SEGV on unknown address 0x000000556820
//       Hint: PC is at a non-executable region. Maybe a wild jump?
//         #0 ... in itmap$Src
//       -> exit 1
//
//     and clean at exit 7 with the fix. `itmap$Src` is #2975's shape exactly.
//
//     ⚠ This row exists because the too-wide half is the DANGEROUS half and
//     rows (A) and (B) pin only the too-narrow one. Without it a future change
//     could re-widen the claim -- reintroducing a wild jump -- with the whole
//     suite green. A leak regression is recoverable; this one is not.
//     per §28.6, §29.2, §2975
// ---------------------------------------------------------------------------
BEZEL_TEST(property_face_wrapper_claim_3419,
           declined_user_class_wrap_claims_no_frame_28_6) {
    JitResult r = runAotChecked(
        "module M; "
        "class Src: IEnumerable<int> "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return Gen(); "
        "  end GetEnumerator; "
        "  private method Gen(): owned IEnumerator<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "end Src; "
        "class C "
        "  private owned _s: Src; "
        "  public constructor() _s := new Src(); end constructor; "
        "  public property Items: IEnumerable<int> => _s; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var e := c.Items; "
        "      i := i + 1; "
        "    end while; "
        "    return 7; "
        "  end Main; "
        "end Main;");
    if (wasSkipped(r)) return;
    // `r.ran` is the load-bearing assertion: pre-fix the process died on a wild
    // jump, so it is false there. The exit code then pins that the five reads
    // completed rather than the program merely surviving.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
