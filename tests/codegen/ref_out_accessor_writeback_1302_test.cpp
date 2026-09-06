// Spec-derived tests for issue #1302 — spec §22.3 "`ref` and `out` argument
// locations" and its follow-on paragraph "**`ref` local and field targets
// alias; accessor targets bracket.**" (v0.228).
//
// §22.3 admits the location outright: "A `ref` or `out` argument must be an
// **assignable storage location** — the §17.6 lvalue categories: a mutable
// local variable, a writable field, **an indexer slot**, or **a property with
// an accessible setter**". It then fixes those last two categories' semantics:
//
//   "A `ref` argument naming a mutable local or writable field is a true
//    alias: callee writes are visible through the caller's location
//    immediately … An **indexer slot or property** target is instead
//    **copy-in / copy-out** under §17.6's evaluate-once discipline: the
//    receiver and every index expression are evaluated exactly once, in
//    §22.5's call-site textual order; for `ref`, the getter is called once to
//    produce the callee's slot value (for `out` no getter call occurs); on
//    **normal return** the setter is called once with the slot's final value.
//    Write-backs for multiple accessor-mediated arguments run in argument
//    textual order — `Swap(ref a[i], ref a[i])` is defined, the textually
//    later write-back winning — and on an exception exit no write-back
//    occurs … Accessor-mediated arguments are never aliases of each other or
//    of anything else during the call."
//
// So the spec ADMITS an indexer element as a `ref`/`out` argument, and the
// correct fix is a codegen one (a write-back), not a rejection.
//
// Codegen implemented no bracket at all. `lvalueAddr` resolves exactly the
// categories that have a real address to alias — locals, fields, statics, and
// §15.1 INLINE-array elements — and returned null for every accessor-mediated
// location. Two observable failures followed, in this order:
//   * as filed (#1302): the null address let the call through and the
//     callee's write went to a temporary — `C.Bump(ref arr[0])` on a heap
//     `Array<int>` returned 10 instead of 15, compiling clean with no
//     diagnostic. A silent WRONG_VALUE.
//   * after the #1682 §22.3 category check landed, the same null reached
//     buildOrderedArgs' fallback and the whole call failed with
//     "internal: lowerExpr returned null for call argument in
//     buildOrderedArgs — #1386" — an internal-error message, not a §22.3
//     rejection, and not the admitted behavior either.
// Every test below therefore fails without the fix: either on the VALUE (the
// dropped write-back) or on `.ran` (the #1386 compile failure). Each asserts
// the correct VALUE, per the issue's requirement.
//
// The defect was never Array-specific: it covered EVERY accessor-mediated
// location — the heap `Array<T>` and `Dictionary<K, V>` indexers, a
// user-declared indexer (§23.4), and a block-form property (§23.3) — while the
// inline-array element (the `lvalueAddr` alias path) was already correct. The
// control at the end pins that one so a future refactor cannot regress it into
// the bracket.

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// `Bump(ref a: int)` adds 5; `Set(out a: int)` writes 99. Shared by most cases.
const char* kBump =
    "  public static method Bump(ref a: int) a := a + 5; end Bump; ";
const char* kSet =
    "  public static method Set(out a: int) a := 99; end Set; ";
}  // namespace

// =============================================================================
// Heap-collection indexer slots — the shape #1302 was filed against.
// =============================================================================

// §22.3: `ref arr[0]` on a heap `Array<int>` — the getter runs once into the
// callee's slot, the setter runs once on normal return. Pre-fix: rc=10 (the
// write dropped), later a #1386 compile failure. Post-fix: 10 + 5 = 15.
BEZEL_TEST(ref_out_accessor_writeback_1302, ref_to_heap_array_slot_writes_back) {
    BEZEL_EXPECT_EQ(run(std::string("module M; class C ") + kBump +
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(10); "
        "    C.Bump(ref arr[0]); "
        "    return arr[0]; "
        "  end Main; end C;"), 15);
}

// §22.3: the `out` sibling of the case above — "for `out` no getter call
// occurs", then the setter runs on normal return. The issue records the
// pre-fix result explicitly: `out arr[0]` returned 1, the pre-existing value.
BEZEL_TEST(ref_out_accessor_writeback_1302, out_to_heap_array_slot_writes_back) {
    BEZEL_EXPECT_EQ(run(std::string("module M; class C ") + kSet +
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(1); "
        "    C.Set(out arr[0]); "
        "    return arr[0]; "
        "  end Main; end C;"), 99);
}

// §22.3 / stdlib §4.8: a `Dictionary<K, V>` indexer slot is an indexer slot
// like any other — the same bracket, driving the dictionary's own key lookup
// on both the getter and the setter.
BEZEL_TEST(ref_out_accessor_writeback_1302, ref_to_dictionary_slot_writes_back) {
    BEZEL_EXPECT_EQ(run(std::string("module M; class C ") + kBump +
        "  public static method Main(): int "
        "    var d := new Dictionary<string, int>(); d[\"k\"] := 10; "
        "    C.Bump(ref d[\"k\"]); "
        "    return d[\"k\"]; "
        "  end Main; end C;"), 15);
}

// §22.3: "for `out` no getter call occurs" is observable, not an optimization
// note — an `out` argument naming an ABSENT dictionary key must not raise
// KeyNotFoundException on the way in; only the setter runs, creating the entry.
// A bracket that read first (or that shared the `ref` path) would raise here.
BEZEL_TEST(ref_out_accessor_writeback_1302, out_to_absent_dict_key_calls_no_getter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Set(out a: int) a := 42; end Set; "
        "  public static method Main(): int "
        "    var d := new Dictionary<string, int>(); "
        "    C.Set(out d[\"k\"]); "
        "    return d[\"k\"]; "
        "  end Main; end C;"), 42);
}

// =============================================================================
// The other accessor-mediated categories §22.3 names in the same breath.
// =============================================================================

// §22.3 / §23.3: "a property with an accessible setter" is an admitted `ref`
// location, bracketed the same way — the block-form getter runs once in, the
// block-form setter runs once out.
BEZEL_TEST(ref_out_accessor_writeback_1302, ref_to_block_property_writes_back) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Box "
        "  private _v: int; "
        "  public constructor() this._v := 10; end constructor; "
        "  public property V: int get => this._v; set => this._v := value; end V; "
        "end Box; class C ") + kBump +
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    C.Bump(ref b.V); "
        "    return b.V; "
        "  end Main; end C;"), 15);
}

// §22.3 / §23.3: the `out` sibling for a property target.
BEZEL_TEST(ref_out_accessor_writeback_1302, out_to_block_property_writes_back) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Box "
        "  private _v: int; "
        "  public constructor() this._v := 1; end constructor; "
        "  public property V: int get => this._v; set => this._v := value; end V; "
        "end Box; class C ") + kSet +
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    C.Set(out b.V); "
        "    return b.V; "
        "  end Main; end C;"), 99);
}

// §22.3 / §23.3: the same property target reached WITHOUT a receiver
// expression — an implicit-`this` access from inside the declaring class.
// There is nothing to pin and no receiver to evaluate; the getter and setter
// run against the enclosing `this`.
BEZEL_TEST(ref_out_accessor_writeback_1302, ref_to_implicit_this_property_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  private _v: int; "
        "  public constructor() this._v := 10; end constructor; "
        "  public property V: int get => this._v; set => this._v := value; end V; "
        "  public method Go(): int C.Bump(ref V); return this._v; end Go; "
        "end Box; class C "
        "  public static method Bump(ref a: int) a := a + 5; end Bump; "
        "  public static method Main(): int "
        "    using var b := new Box(); return b.Go(); "
        "  end Main; end C;"), 15);
}

// §22.3 / §23.4: a USER-declared indexer is an indexer slot too — the bracket
// drives the declared accessors, not the built-in collection runtime.
BEZEL_TEST(ref_out_accessor_writeback_1302, ref_to_user_indexer_writes_back) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class Box "
        "  private _v: int; "
        "  public constructor() this._v := 10; end constructor; "
        "  public property [i: int]: int get => this._v; set => this._v := value; end; "
        "end Box; class C ") + kBump +
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    C.Bump(ref b[0]); "
        "    return b[0]; "
        "  end Main; end C;"), 15);
}

// =============================================================================
// The bracket's stated discipline: evaluate-once, exception edge, ordering,
// non-aliasing. Each of these is a sentence of §22.3, and each is a way a
// naive "just re-lower the target after the call" write-back gets it wrong.
// =============================================================================

// §22.3 / §17.6: "the receiver and every index expression are evaluated
// exactly once". A write-back that re-lowered the index expression would call
// `Idx()` twice — reading one slot and writing another (the #1299 hazard for
// `a[i] OP= v`). The return folds both facts: arr[1] must be 25 and Calls
// must be 1, so 26 — a double evaluation gives 27 (or 20 + 2 on a misdirected
// write), and no write-back at all gives 21.
BEZEL_TEST(ref_out_accessor_writeback_1302, index_expression_evaluated_exactly_once) {
    BEZEL_EXPECT_EQ(run(std::string(
        "module M; class C "
        "  public static Calls: int := 0; "
        "  public static method Idx(): int C.Calls := C.Calls + 1; return 1; end Idx; ") + kBump +
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(10); arr.Add(20); arr.Add(30); "
        "    C.Bump(ref arr[C.Idx()]); "
        "    return arr[1] + C.Calls; "
        "  end Main; end C;"), 26);
}

// §22.3: "on an exception exit no write-back occurs" (the same edge the `out`
// copy-out rule states: "on an exception exit the recipient is untouched").
// The callee assigns 77 through the `ref` and then throws; the caught path
// must observe the ORIGINAL 10.
BEZEL_TEST(ref_out_accessor_writeback_1302, no_write_back_on_exception_exit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Boom(ref a: int) "
        "    a := 77; throw new InvalidOperationException(\"x\"); "
        "  end Boom; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(10); "
        "    try C.Boom(ref arr[0]); "
        "    catch e: InvalidOperationException => return arr[0]; end; "
        "    return 1; "
        "  end Main; end C;"), 10);
}

// §22.3: "Write-backs for multiple accessor-mediated arguments run in argument
// textual order — `Swap(ref a[i], ref a[i])` is defined, the textually later
// write-back winning." The spec's own named example: `a` gets 1, `b` gets 2,
// both name arr[0], so the second write-back wins and the slot holds 2.
BEZEL_TEST(ref_out_accessor_writeback_1302, later_write_back_wins_spec_swap_example) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Swap(ref a: int, ref b: int) a := 1; b := 2; end Swap; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(0); "
        "    C.Swap(ref arr[0], ref arr[0]); "
        "    return arr[0]; "
        "  end Main; end C;"), 2);
}

// §22.3: "Accessor-mediated arguments are never aliases of each other or of
// anything else during the call." Both parameters name arr[0]; the callee
// writes 100 through `a` and then reads `b`, which must still hold the
// original 7 — copies, not aliases. (A ref-to-LOCAL pair would legitimately
// see 100; that is exactly the difference the paragraph draws.)
BEZEL_TEST(ref_out_accessor_writeback_1302, accessor_ref_args_do_not_alias_each_other) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static Seen: int := 0; "
        "  public static method Probe(ref a: int, ref b: int) "
        "    a := 100; C.Seen := b; "
        "  end Probe; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(7); "
        "    C.Probe(ref arr[0], ref arr[0]); "
        "    return C.Seen; "
        "  end Main; end C;"), 7);
}

// §22.3 / §17.6: a NESTED indexer — the index expression is itself an indexer
// read — evaluates its receiver and index once each, and the write-back
// addresses the slot the getter read. keys[0] is 1, so arr[1] becomes 25.
BEZEL_TEST(ref_out_accessor_writeback_1302, nested_indexer_index_expression) {
    BEZEL_EXPECT_EQ(run(std::string("module M; class C ") + kBump +
        "  public static method Main(): int "
        "    var keys := new Array<int>(); keys.Add(1); "
        "    var arr := new Array<int>(); arr.Add(10); arr.Add(20); "
        "    C.Bump(ref arr[keys[0]]); "
        "    return arr[1]; "
        "  end Main; end C;"), 25);
}

// =============================================================================
// Element/parameter shapes the bracket must size correctly.
// =============================================================================

// §22.3 / §14.1: the bracket's slot is sized from the LOCATION's type, so a
// GENERIC method whose `ref` parameter is the type parameter (`ref a: T`, T =
// long) writes a full 8-byte slot, and one whose `ref` parameter is
// CONCRETELY typed on a generic method (`ref a: int` — the #2266 shape, which
// neither generic guard used to check) writes exactly its 4. 40 + 7 = 47; a
// mis-sized slot corrupts the neighbouring value or the returned sum.
BEZEL_TEST(ref_out_accessor_writeback_1302, generic_and_concrete_ref_params_slot_sizing) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Bump<T>(ref a: T, v: T) a := v; end Bump; "
        "  public static method BumpConcrete<T>(ref a: int, t: T) a := a + 5; end BumpConcrete; "
        "  public static method Main(): int "
        "    var arr := new Array<long>(); arr.Add(10L); "
        "    C.Bump<long>(ref arr[0], 40L); "
        "    var xs := new Array<int>(); xs.Add(2); "
        "    C.BumpConcrete<string>(ref xs[0], \"s\"); "
        "    return int(arr[0]) + xs[0]; "
        "  end Main; end C;"), 47);
}

// §22.3 / §9: a value-typed RECORD element round-trips through the bracket by
// value — the getter's copy in, the callee's replacement out. 11 + 22 = 33.
BEZEL_TEST(ref_out_accessor_writeback_1302, ref_to_record_element_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point public X: int; public Y: int; end Point; "
        "class C "
        "  public static method Shift(ref p: Point) "
        "    p := new Point(p.X + 1, p.Y + 2); "
        "  end Shift; "
        "  public static method Main(): int "
        "    var arr := new Array<Point>(); arr.Add(new Point(10, 20)); "
        "    C.Shift(ref arr[0]); "
        "    return arr[0].X + arr[0].Y; "
        "  end Main; end C;"), 33);
}

// =============================================================================
// Controls — categories §22.3 treats DIFFERENTLY must keep their behavior.
// =============================================================================

// §22.3 / §15.1: an INLINE-array element is a slot inside a local, so it is a
// true ALIAS (`lvalueAddr` resolves it) — not bracketed. It was already
// correct before the fix (the issue records it as a passing control) and must
// stay correct: the bracket must not swallow this path.
BEZEL_TEST(ref_out_accessor_writeback_1302, inline_array_element_still_aliases) {
    BEZEL_EXPECT_EQ(run(std::string("module M; class C ") + kBump +
        "  public static method Main(): int "
        "    var arr: [3]int := [10, 20, 30]; "
        "    C.Bump(ref arr[0]); "
        "    return arr[0]; "
        "  end Main; end C;"), 15);
}

// §22.3: an OWNING-collection slot is owned state and stays REJECTED — "no
// location of **owned state** — … an owning-collection or owning-inline-array
// slot — because a `ref` write is an assignment through an alias, invisible to
// the drop-on-overwrite and consume-site-freeze machinery". The bracket must
// not turn that rejection into an accepted write-back.
BEZEL_TEST(ref_out_accessor_writeback_1302, owning_collection_slot_still_rejected) {
    auto r = runAot(
        "module M; class Cell public N: int; "
        "  public constructor(n: int) this.N := n; end constructor; end Cell; "
        "class C "
        "  public static method Swallow(ref c: Cell) c := new Cell(1); end Swallow; "
        "  public static method Main(): int "
        "    using var pool := new OwningArray<Cell>(); "
        "    pool.Add(new Cell(3)); "
        "    C.Swallow(ref pool[0]); "
        "    return 0; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§22.3") != std::string::npos &&
            d.message.find("owning-collection") != std::string::npos)
            cited = true;
    BEZEL_EXPECT_TRUE(cited);
}

// §22.3 closes the location list for the OWNED output variant: "for `out
// owned`, the accepted forms remain exactly the inline `out using var x` /
// `out owned var x` of §22.4". An indexer slot is therefore NOT an accepted
// `out owned` argument, and the bracket must not make it one — the callee's
// fresh owned value would be stored into a BORROWING `Array<Cell>` slot with
// nobody to discharge it (§28.6 leak prevention / §28.11 "collections do not
// own elements"). The compile must fail. (Today it fails through the generic
// #1386 fallback rather than a dedicated §22.3 diagnostic; this asserts the
// rejection, not the message, so the follow-up that adds the proper
// diagnostic does not have to touch this test.)
BEZEL_TEST(ref_out_accessor_writeback_1302, out_owned_to_indexer_slot_still_rejected) {
    auto r = runAot(
        "module M; class Cell public N: int; "
        "  public constructor(n: int) this.N := n; end constructor; end Cell; "
        "class C "
        "  public static method Make(out owned c: Cell) c := new Cell(5); end Make; "
        "  public static method Main(): int "
        "    var arr := new Array<Cell>(); "
        "    using var seed := new Cell(1); arr.Add(seed); "
        "    C.Make(out arr[0]); "
        "    return arr[0].N; "
        "  end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine §28.6/§28.11 rejection
    // from a runtime crash/signal, which a bare `hasErrors()` alone cannot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §22.3 / §22.12 rule 2: a STATIC property is declined by the bracket and
// stays rejected — in BOTH spellings. §22.3 subtracts "no **mutable static**
// admitted by §22.12 rule 2", whose subject is "a mutable … static field or
// auto-property"; a BLOCK-FORM static property is neither, so the pair does
// not decide the shape (ruling requested, #2322). What this test pins is not
// the verdict but the CONSISTENCY: the qualified `ref Box.S` and the bare
// `ref S` inside the declaring class must behave identically. Only the
// qualified spelling has a receiver expression, so without the explicit
// decline the bare form would have been silently admitted while the qualified
// one kept failing. When #2322 is ruled, both assertions move together.
BEZEL_TEST(ref_out_accessor_writeback_1302, static_property_declined_in_both_spellings) {
    const char* kBoxHdr =
        "module M; class Box "
        "  public static SV: int := 0; "
        "  public static property S: int get => Box.SV; set => Box.SV := value; end S; ";
    // Bare-name spelling, from inside the declaring class.
    auto bare = runAot(std::string(kBoxHdr) +
        "  public static method Go(): int "
        "    Box.SV := 3; C.Bump(ref S); return Box.SV; "
        "  end Go; "
        "end Box; class C "
        "  public static method Bump(ref a: int) a := a + 5; end Bump; "
        "  public static method Main(): int return Box.Go(); end Main; end C;");
    // Qualified spelling, from outside.
    auto qualified = runAot(std::string(kBoxHdr) +
        "end Box; class C "
        "  public static method Bump(ref a: int) a := a + 5; end Bump; "
        "  public static method Main(): int "
        "    Box.SV := 3; C.Bump(ref Box.S); return Box.SV; "
        "  end Main; end C;");
    // fix #3895 (issue #3892): distinguishes a genuine rejection from a
    // runtime crash/signal for EACH spelling, which a bare `hasErrors()`
    // cannot — the row's whole point is that both spellings decline the
    // same way, so both must be checked as real rejections, not just
    // "some Error diagnostic exists".
    bool bareRejected = failedAtCompileTimeWithRealLocation(bare);
    bool qualifiedRejected = failedAtCompileTimeWithRealLocation(qualified);
    BEZEL_EXPECT_TRUE(bareRejected);
    BEZEL_EXPECT_TRUE(qualifiedRejected);
    BEZEL_EXPECT_EQ(bareRejected, qualifiedRejected);
}

// §22.3 / §23.3: a read-only property is not "a property with an accessible
// setter", so it stays rejected — the bracket has no setter to call and must
// not invent one.
BEZEL_TEST(ref_out_accessor_writeback_1302, getter_only_property_still_rejected) {
    auto r = runAot(std::string(
        "module M; class Box "
        "  private _v: int; "
        "  public constructor() this._v := 10; end constructor; "
        "  public property V: int => this._v; "
        "end Box; class C ") + kBump +
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    C.Bump(ref b.V); "
        "    return b.V; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§22.3") != std::string::npos) cited = true;
    BEZEL_EXPECT_TRUE(cited);
}
