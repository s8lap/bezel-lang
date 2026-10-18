// Codegen — #6380 / §28.3 *unbound owned temporaries* + §28.9, stdlib §4.4–§4.11
// (v0.338).
//
// THE ISSUE. A LINQ source reaches codegen through ONE function —
// `lowerToLazyEnumerator`, which lowers the receiver EXPRESSION ITSELF and
// returns a `bz_lazy_t*` wrapping it. Every LINQ consumer then drains that
// enumerator (`bz_lazy_to_array`, `bz_lazy_to_dict`, the for-loop) and disposes
// it. When the receiver is an UNBOUND fresh `owned` collection temporary — an
// `owned`-returning producer call, or a fresh materializer call, used DIRECTLY
// as the receiver — nothing else holds that value: §28.3 K8 discharges it "at
// the end of the full expression in which it is produced", and on this path
// that boundary is the consuming enumeration. Before this fix the lazy wrap was
// a pure BORROW (`bz_lazy_from_array` / `bz_lazy_from_llist` / `bz_lazy_from_set`
// / the Dictionary materializer), so `bz_lazy_dispose` left the source alive and
// it leaked once per evaluation.
//
// This is the residual #6366 filed. #6366 fixed the two faces whose CONSUMER can
// name the source (`Sum` and the `.Length`/`.Count` arm, through
// `freeFreshInputTemp`); it deliberately declined this path and left a precise
// scoping note, because closing it needs the source surfaced out of
// `lowerToLazyEnumerator`. That is what this fix does — and it does it at the
// ONE site every consumer funnels through, so the whole family is covered from
// a single change rather than once per terminal.
//
// THE DISCRIMINATOR IS NOT THE FAMILY. (Corrected on this issue after review of
// #6366, whose first framing said `LinkedList<T>` — that understated it, and an
// `Array<T>` receiver has two leaking faces of its own.) `LinkedList<T>` is never
// array-backed, so EVERY terminal on it materializes and every one leaked. An
// `Array<T>` receiver materializes only when the terminal is a LINQ one
// (`ToArray`) or when a lazy operator precedes the terminal (`Where(...).Sum()`),
// and both leaked identically. The rows below therefore cover BOTH families and
// BOTH reasons an `Array<T>` receiver materializes; a suite scoped to
// `LinkedList` would miss the two `Array` faces, which is the mistake this
// scoping note exists to prevent.
//
// MEASURED on this fix's parent (r30-c1 @ e97bc2bb), 101 iterations,
// `bezelc --sanitize=address` — one `bz_*_alloc` per iteration in each case:
//
//   shape                                 receiver        result
//   f.Make().ToArray()                    Array<int>        12,120 B
//   f.Make().Where(..).Sum()              Array<int>        12,120 B
//   f.Make().Where(..).Count()            Array<int>        12,120 B
//   f.Make().Where(..).ToArray()          Array<int>        12,120 B
//   f.Make().Sum()                        LinkedList<int>   28,280 B
//   f.Make().ToArray()                    LinkedList<int>   14,140 B
//   f.Make().First()                      LinkedList<int>   28,280 B
//   f.Make().Where(..).Sum()              LinkedList<int>   14,140 B
//   f.Make().Sum()                        Set<int>          42,824 B
//   f.Make().Sum()                        Dictionary<int,int> 42,824 B
//   f.Make().Sum()                        Array<int>             0 B  (#6366)
//   f.Make();                             Array/LinkedList       0 B  (#6366)
//   _ := f.Make();                        Array/LinkedList       0 B
//
// THE FIX. `lowerToLazyEnumerator` gains an ownership transfer: when the
// receiver is one of the unowned shapes — the SAME population
// `freeUnboundOwnedBorrowingArrayTemp` discharges at end of statement
// (`isFreshOwnerlessArrayMaterializerExpr` /
// `isUnboundOwnedBorrowingArrayCallExpr`) — the wrap it builds is handed
// `bz_lazy_own_source(lazy, src, kind)` and `bz_lazy_dispose` reclaims `src`
// with the runtime free its own backing needs. `kind` is the SAME three-way
// classification `freeUnboundOwnedBorrowingArrayTemp` dispatches on (1
// `bz_array_t*`, 2 `bz_llist_t*`, 3 `bz_dict_t*`), so one temporary is freed by
// exactly one runtime free whichever consumer drains it.
//
// WHY THIS IS THE SAFE DIRECTION. The dispose free passes a NULL element
// disposer. Every family named here is a BORROWING collection (stdlib §3/§5):
// the producer owns its elements, not the receiver, and any result that aliased
// a slot (the #2263 `Array<string>`-valued `ToDictionary` shape) would
// use-after-free if this released them. Measured on the two `Array<string>`
// LINQ-source faces below — 18,584 B on this fix's parent — the source's array
// AND its strings are both reclaimed at 0, so the null disposer costs nothing
// here; the rows are pinned so a future change to that disposer is caught.
//
// NOT PINNED, DELIBERATELY. (1) A row asserting a leak as the EXPECTED outcome
// is a vacuous oracle: it passes while the leak persists and INVERTS the moment
// the leak is fixed. (2) The eager `Array<T>` terminal arms that never call
// `freeFreshInputTemp` at all (`.Any()`, `.First()`, `.Max()` on a plain
// `Array<int>`) leak by a DIFFERENT mechanism — no lazy enumerator is built
// there at all, so no ownership transfer reaches them — and are out of this
// fix's scope: measured at 12,120 B each on this fix's parent and unchanged by
// it. (3) `Dictionary<K,V>.Sum()` is a degenerate no-dispatch case (a
// `warning:` about dispatch, no lazy enumerator built) whose byte count is that
// warning, not this defect.
//
// ORACLE. `runAotChecked` is always ASan and returns nonzero on ANY sanitizer
// finding, so each row fails the RUN rather than merely missing a finding. No
// `leaksAtLeast`: these rows are genuinely fixed, and `leaksAtLeast` is a leak
// oracle, not an ASan one. `BEZEL_EXPECT_TRUE(r.ran)` is asserted on every row —
// that is the assertion that turns a leak, a use-after-free and a double free
// into a red row, all three.
//
// SPEC BASIS. §28.3 K8 (unbound owned temporaries; end of full expression),
// §28.9 (the compiler frees the undischarged value at its owner's release point),
// stdlib §4.4–§4.11 (Array/Stack/Queue/Set/Dictionary/LinkedList borrowing
// family) and §4.17–§4.18 (SortedSet/SortedDictionary), §5 (LINQ materializes
// through the lazy enumerator, which owns its source for its lifetime).
// Expected values: `Make()` builds [1, 2, 3]; `acc` counts one pass, so 101
// iterations give 101, whose `mod 100` residue is 1.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Four producers over the same element type where the element type allows it,
// so every row is a family SWAP with nothing else changing. `Array<T>` is
// `bz_array_t`-backed, `LinkedList<T>` is `bz_llist_t`-backed, and the
// generic-keyed `Set<T>` is `bz_dict_t`-backed (the same `genericBackingType`
// predicate the Set member arms read) — so between them the rows exercise all
// three `kind` values `bz_lazy_own_source` takes.
std::string makeArray() {
    return "public class F "
           "  public method Make(): owned Array<int> "
           "    return [1, 2, 3]; "
           "  end Make; "
           "end F; ";
}

std::string makeLlist() {
    return "public class F "
           "  public method Make(): owned LinkedList<int> "
           "    var l: LinkedList<int> := new LinkedList<int>(); "
           "    l.Add(1); l.Add(2); l.Add(3); "
           "    return l; "
           "  end Make; "
           "end F; ";
}

std::string makeSet() {
    return "public class F "
           "  public method Make(): owned Set<int> "
           "    var s: Set<int> := new Set<int>(); "
           "    s.Add(1); s.Add(2); s.Add(3); "
           "    return s; "
           "  end Make; "
           "end F; ";
}

std::string makeDict() {
    return "public class F "
           "  public method Make(): owned Dictionary<int, int> "
           "    var d: Dictionary<int, int> := new Dictionary<int, int>(); "
           "    d.Add(1, 1); d.Add(2, 2); d.Add(3, 3); "
           "    return d; "
           "  end Make; "
           "end F; ";
}

// `Array<string>` differs from the value-element families in exactly one way
// that reaches this fix: its elements are `bz_string*` slots with a co-owned
// +1, so a reclaim that RELEASED them would use-after-free any result aliasing
// a slot (#2263). The ownership transfer passes a null disposer instead, so this
// producer pins that the source array is reclaimed while its strings survive.
std::string makeStringArray() {
    return "public class F "
           "  public method Make(): owned Array<string> "
           "    return [\"a\", \"b\", \"c\"]; "
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

constexpr int kResidueOf101 = 1;  // 101 mod 100

// Run one face and assert BOTH directions of the same oracle: the run completed
// (`runAotChecked` returns nonzero for a leak, a use-after-free AND a double
// free alike, so `r.ran` is false for each) and the loop really ran 101 times.
void expectClean(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf101);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}

}  // namespace

// ===========================================================================
// ARRAY (bz_array_t*, kind 1) — REASON ONE: the terminal ITSELF is a LINQ one,
// so the receiver materializes. 12,120 B / 101 on this fix's parent; 0 after.
// ===========================================================================
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           array_linq_terminal_toarray_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeArray(), "M6380A", "f.Make().ToArray();"));
}

// ===========================================================================
// ARRAY — REASON TWO: a LAZY OPERATOR precedes the terminal, so the receiver
// materializes one step further down (inside the operator's own source wrap).
// These are the two faces the corrected scoping names explicitly; a suite
// covering only `LinkedList` would have missed both.
// ===========================================================================
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           array_lazy_operator_then_scalar_terminal_is_reclaimed_spec_28_3) {
    expectClean(driver(makeArray(), "M6380B",
                       "f.Make().Where(lambda(x) => x > 0).Sum();"));
}

BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           array_lazy_operator_then_count_terminal_is_reclaimed_spec_28_3) {
    expectClean(driver(makeArray(), "M6380C",
                       "f.Make().Where(lambda(x) => x > 0).Count();"));
}

// The composition of both reasons at once: a LINQ terminal reached THROUGH a
// lazy operator, whose source is the unbound temporary.
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           array_lazy_operator_then_linq_terminal_is_reclaimed_spec_28_3) {
    expectClean(driver(makeArray(), "M6380D",
                       "f.Make().Where(lambda(x) => x > 0).ToArray();"));
}

// ===========================================================================
// LINKEDLIST (bz_llist_t*, kind 2) — never array-backed, so EVERY terminal
// materializes; the two reasons collapse into one and the family is covered
// across terminal kinds.
// ===========================================================================
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           linkedlist_scalar_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeLlist(), "M6380E", "f.Make().Sum();"));
}

BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           linkedlist_linq_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeLlist(), "M6380F", "f.Make().ToArray();"));
}

BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           linkedlist_lazy_operator_then_terminal_is_reclaimed_spec_28_3) {
    expectClean(driver(makeLlist(), "M6380G",
                       "f.Make().Where(lambda(x) => x > 0).Sum();"));
}

// ===========================================================================
// SET / DICTIONARY (bz_dict_t*, kind 3) — the same rule on the third backing.
// `.Sum()` over a Dictionary/Set receiver materializes its entries, so the
// container itself is the unbound temporary the lazy source now owns.
// ===========================================================================
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           set_scalar_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeSet(), "M6380H", "f.Make().Sum();"));
}

BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           dictionary_linq_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeDict(), "M6380I", "f.Make().ToArray();"));
}

// ===========================================================================
// ELEMENT-RETURNING AND STRING-ELEMENT FACES. `First()` on a `LinkedList`
// returns an element read out of the materialized snapshot, so the reclaim must
// run AFTER that read and must not reach into the snapshot; the two `Array<string>`
// rows pin the null-element-disposer decision above.
// ===========================================================================
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           linkedlist_element_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeLlist(), "M6380R", "f.Make().First();"));
}

BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           array_string_linq_terminal_of_an_unbound_owned_temp_is_reclaimed_spec_28_3) {
    expectClean(driver(makeStringArray(), "M6380S", "f.Make().ToArray();"));
}

BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           array_string_lazy_operator_then_terminal_is_reclaimed_spec_28_3) {
    expectClean(driver(makeStringArray(), "M6380T",
                       "f.Make().Where(lambda(x) => x > \"a\").Count();"));
}

// ===========================================================================
// THE CONTROLS — the over-free direction. `isUnboundOwnedBorrowingArrayCallExpr`
// matches only a CALL used DIRECTLY as the receiver, so a receiver bound to a
// local is never matched, never marked owned, and is discharged by its own owner
// at scope end. Reading that receiver back AFTER the terminal has drained and
// disposed the lazy is what makes an over-eager reclaim LOUD: ASan reports the
// read of the freed backing and `runAotChecked` returns nonzero for that exactly
// as it does for a leak, so one oracle covers both directions.
//
// 101 passes x 3 elements = 303, whose `mod 100` residue is 3.
// ===========================================================================
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           bound_local_source_is_never_freed_by_the_lazy_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M6380J; ") + makeLlist() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var l := f.Make(); "
        "      acc := acc + l.Sum(); "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // 606 mod 100 = 6
    BEZEL_EXPECT_TRUE(r.stdoutText.find("606") != std::string::npos);
}

// The same shape on the `bz_array_t*` backing, so both `kind` values are
// covered in the over-free direction and not just the `bz_llist_t*` one. It
// goes through `ToArray` rather than `Sum` on purpose: an `Array<T>` receiver's
// `Sum`/no-arg `Count` are DIRECT array folds that build no lazy enumerator at
// all, so a reclaim that reached them would not exercise the ownership transfer
// this fix adds. `ToArray` does build one, and the terminal's snapshot is kept
// in `t` so the row also pins that the reclaim stops at the source — reading
// `t` afterwards is a use-after-free if it reached past the snapshot.
// 101 passes x 3 elements = 303, whose `mod 100` residue is 3.
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           bound_local_array_source_is_never_freed_by_the_lazy_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M6380K; ") + makeArray() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var a := f.Make(); "
        "      var t := a.ToArray(); "
        "      acc := acc + t.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // 303 mod 100 = 3
    BEZEL_EXPECT_TRUE(r.stdoutText.find("303") != std::string::npos);
}

// A PIPELINE bound to a local: the unbound temporary is owned by the lazy (the
// binding owns the LAZY, not the collection), and disposing the lazy at the
// terminal frees the source. The value read back here is the terminal's RESULT,
// which outlives the dispose — if the reclaim reached past the snapshot into the
// result, this row is the one that would catch it.
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           bound_pipeline_read_back_after_the_dispose_is_reclaimed_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M6380L; ") + makeArray() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var q := f.Make().Where(lambda(x) => x > 0); "
        "      acc := acc + q.ToArray().Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // 303 mod 100 = 3
    BEZEL_EXPECT_TRUE(r.stdoutText.find("303") != std::string::npos);
}

// THE OPT-OUT. `ToDictionary` is the one consumer that reclaims the source
// ITSELF (through `lowerToLazyEnumerator`'s `outRecvArr`, added by #2151), so it
// passes `ownSourceTemp=false` and keeps the borrowing wrap. If the ownership
// transfer fired here as well, `bz_lazy_dispose` and the call site's own reclaim
// would free one source twice — a double free, which `runAotChecked` reports
// and this row pins.
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           todictionary_of_an_unbound_owned_temp_is_reclaimed_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M6380M; ") + makeArray() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      var d := f.Make().ToDictionary(lambda(x) => x); "
        "      acc := acc + d.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // 303 mod 100 = 3
    BEZEL_EXPECT_TRUE(r.stdoutText.find("303") != std::string::npos);
}

// The `_ := f.Make();` binding #6366 already made clean. Green BEFORE and AFTER
// this fix (measured 0 B on the parent) — it is here so a future widening of
// the ownership transfer cannot pass by taking a value whose own owner disposes
// it.
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           bound_discard_binding_stays_clean_on_both_families_spec_28_3) {
    expectClean(driver(makeArray(), "M6380N", "_ := f.Make();"));
    expectClean(driver(makeLlist(), "M6380O", "_ := f.Make();"));
}

// VALUES, on the plain (non-ASan) leg so the pinned quantity does not depend on
// the sanitizer runtime at all. `f.Make().Sum()` is 1+2+3 = 6 per pass; the
// accumulator is what makes the loop non-foldable.
BEZEL_TEST(lazy_terminal_unbound_owned_source_reclaim_6380,
           materialized_values_are_correct_after_the_reclaim_spec_28_3) {
    JitResult r = runAot(
        std::string("module M6380P; ") + makeArray() +
        "public class M "
        "  public static method Main(): int "
        "    var f := new F(); "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      acc := acc + f.Make().Where(lambda(x) => x > 0).Sum(); "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // 606 mod 100 = 6
    BEZEL_EXPECT_TRUE(r.stdoutText.find("606") != std::string::npos);
}