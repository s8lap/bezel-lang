// fix #2204 — §28.3 (unbound owned temporaries) + §28.11 (borrowing
// collections / the store-a-borrow rule), spec v0.226. Codegen-observable,
// LeakSanitizer-checked coverage for: an owned TEMPORARY (`new R(1)`)
// passed to a BORROWING collection's `Insert(index, value)` or indexer-set
// `arr[i] := value` / `d[k] := value` (a non-owned argument/RHS position)
// was silently leaked — no disposal, no reject, no diagnostic. Sibling to
// the #1881 Add/Push/Enqueue fix; code inspection found the IDENTICAL gap
// in two sibling codegen sites for the same collection family, which
// #1881's fix deliberately did not touch (kept out of scope; filed as the
// follow-up #2204).
//
// Spec resolution (this is a DISPOSE, not a reject, per §28.3 v0.226 line
// ~2771): "Add, Insert, and the indexer-set on a borrowing collection
// remain governed by §28.3's unbound-temporary admission and §28.11's
// residual-trust store rule — an opaque callee's signature does not
// announce retention — so coll.Add(new Connection("a")) stays admissible
// and stays the borrow-lifetime hazard that checked disposal mode
// backstops." Only the COLLECTION-LITERAL form (`Array<T> := [new X()]`) is
// rejected at the literal site — a call like `Insert` or an indexer-set
// is never rejected. §28.3's own "Unbound owned temporaries" enumeration
// lists "a non-owned argument" as one of the borrowing uses an unbound
// owned temporary may serve before being "disposed at the end of the full
// expression in which it is produced." `Insert` returns unit and the
// indexer-set is a statement-shaped store, so nothing in the statement
// can chain off the result — the call's own end IS that full-expression
// boundary for this argument.
//
// The fix:
//   - Insert on a borrowing array (LlvmBackend_Calls.cpp, the bz_array_insert_at
//     branch) — disposes a fresh-producer argument (isFreshOwnedClassProducer)
//     immediately after the runtime call, when the receiving collection is
//     BORROWING (`!arrIsOwning`; an OWNING collection instead transfers
//     ownership via the pre-existing #1955 nulling path — mutually exclusive,
//     no double dispatch).
//   - Indexer-set on a borrowing array (LlvmBackend_Assign.cpp, the no-disposer
//     bz_array_set branch) — same gate, with the same `!arrIsOwning2` check.
//   - Indexer-set on a borrowing dictionary (LlvmBackend_Assign.cpp, the
//     bz_dict_set_gen / bz_dict_set_str branches) — same gate, with
//     `!dictIsOwning2`. The fresh-producer gate IS the class-type check:
//     isFreshOwnedClassProducer only returns true for class-typed fresh
//     producers, so a record (the dict's value is heap-boxed — `vPtr` is a
//     separately-owned box, not the source `val`) or a string (the dict
//     retains a +1, transferring the borrow into a co-owned +1) is NEVER
//     matched — no spurious double-free of the boxed record or the
//     transferred string +1.
//
// All three intentionally create the "borrow-lifetime hazard that checked
// disposal mode backstops" the spec names: the copy just stored in the
// collection dangles from this point on — reading it back is undefined
// behavior in the DEFAULT (unchecked) disposal-checking mode (§28.2; the
// "checked disposal mode" referenced here is a separate, opt-in per-build
// --checked-disposal flag, not the memory-sanitizer sense `runAotChecked`
// tests) and would deterministically raise DisposedAccessException in
// checked mode. None of the tests below read a stored element back — every
// observable is the destructor-count exit code, which is unaffected by
// whether the stored slot is later read.
//
// Every observable rides on a single `runAotChecked` run: the exit code
// encodes a destructor COUNT (a static counter bumped on dispose), and the
// same AOT+ASan/LSan run flips `ran` to false on any leak / double-free /
// use-after-free. FAIL-WITHOUT/PASS-WITH: before the fix the temporary's
// destructor never ran (R._n stayed 0) and the block leaked (LSan flag);
// after the fix it is disposed exactly once with no leak.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A resource class R whose destructor bumps a static disposal counter R._n.
const char* kR =
    "class R "
    "  public static _n: int := 0; "
    "  public id: int; "
    "  public constructor(x: int) id := x; end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; ";

std::string prog(const std::string& classesAfterR, const std::string& mainBody) {
    return std::string("module M; ") + kR + classesAfterR +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE BUG (issue's own repro shape) — `arr.Insert(0, new R(1))` on a
// borrowing `Array<R>`: the temporary is disposed at the end of the
// statement (its destructor observably runs), with no leak.
// FAIL-WITHOUT: R._n == 0 (+ leak).
// ===========================================================================
BEZEL_TEST(borrowing_collection_insert_temp_2204,
           array_insert_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  arr.Insert(0, new R(1)); "
        "  return R._n; ");  // disposed at end of the Insert statement -> 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// Two fresh temps inserted in separate statements — each disposed exactly
// once, in production order.
BEZEL_TEST(borrowing_collection_insert_temp_2204,
           array_insert_two_fresh_temps_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  arr.Insert(0, new R(1)); "
        "  arr.Insert(1, new R(2)); "
        "  return R._n; ");  // 2
    BEZEL_EXPECT_EQ(run(src), 2);
}

// An `owned`-returning FACTORY call (not a bare `new`) used as the Insert
// argument is the SAME kind of unbound owned temporary — the sound gate is
// isFreshOwnedClassProducer's `owned`-return discriminant, not a `new`-only
// whitelist.
BEZEL_TEST(borrowing_collection_insert_temp_2204,
           array_insert_owned_returning_call_temp_disposed_28_3) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(x: int): owned R return new R(x); end Make; "
        "end Mk; ",
        "  using var arr := new Array<R>(); "
        "  arr.Insert(0, Mk.Make(3)); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// THE BUG (indexer-set repro shape) — `arr[0] := new R(1)` on a borrowing
// `Array<R>`: the temporary RHS is disposed at the end of the statement
// (its destructor observably runs), with no leak. FAIL-WITHOUT: R._n == 0
// (+ leak). Uses the size+fill constructor to pre-allocate a valid slot 0;
// the filled `first` value is OWNED so it survives at scope end (the
// array's slot just borrows it for the lifetime of the indexer-set
// expression) and is disposed AFTER the return.
// ===========================================================================
BEZEL_TEST(borrowing_collection_indexer_set_temp_2204,
           array_indexer_set_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        "",
        "  var first := new R(0); "
        "  using var arr := new Array<R>(1, first); "
        "  arr[0] := new R(1); "
        "  return R._n; ");  // set RHS disposed at end of indexer-set stmt -> 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// Same shape but with a successor indexer-set statement — both RHS temps
// disposed exactly once, in production order.
BEZEL_TEST(borrowing_collection_indexer_set_temp_2204,
           array_indexer_set_two_fresh_temps_disposed_28_3) {
    const std::string src = prog(
        "",
        "  var first := new R(0); "
        "  using var arr := new Array<R>(2, first); "
        "  arr[0] := new R(1); "
        "  arr[1] := new R(2); "
        "  return R._n; ");  // 2 set RHSs disposed at end of their stmts -> 2
    BEZEL_EXPECT_EQ(run(src), 2);
}

// An `owned`-returning FACTORY call (not a bare `new`) used as the indexer-
// set RHS is the SAME kind of unbound owned temporary — the sound gate is
// isFreshOwnedClassProducer's `owned`-return discriminant, not a `new`-only
// whitelist.
BEZEL_TEST(borrowing_collection_indexer_set_temp_2204,
           array_indexer_set_owned_returning_call_temp_disposed_28_3) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(x: int): owned R return new R(x); end Make; "
        "end Mk; ",
        "  var first := new R(0); "
        "  using var arr := new Array<R>(1, first); "
        "  arr[0] := Mk.Make(3); "
        "  return R._n; ");  // 1 (set RHS disposed at end of indexer-set stmt)
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// THE BUG (dictionary indexer-set repro shape) — `d["k"] := new R(1)` on a
// borrowing `Dictionary<string, R>`: the temporary RHS is disposed at the
// end of the statement (its destructor observably runs), with no leak.
// FAIL-WITHOUT: R._n == 0 (+ leak). Dictionary's "Insert" is the indexer-
// setter per stdlib §4.8 (it has no separate `Insert`/`Add` with a non-
// owning argument shape on a borrowing dict — `Add` rejects duplicate keys
// and the indexer-set is the insert-or-replace path).
// ===========================================================================
BEZEL_TEST(borrowing_collection_indexer_set_temp_2204,
           dict_indexer_set_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var d := new Dictionary<string, R>(); "
        "  d[\"k\"] := new R(1); "
        "  return R._n; ");  // disposed at end of the indexer-set stmt -> 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// Borrowing Dictionary<INT-GENERIC-KEY, R> indexer-set — the gen-key path
// (bz_dict_set_gen) is the second branch of the borrowing dict indexer-set
// fix. Confirms the fix is hardware-shared between string- and gen-key
// borrowing dicts.
BEZEL_TEST(borrowing_collection_indexer_set_temp_2204,
           dict_indexer_set_int_key_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var d := new Dictionary<int, R>(); "
        "  d[0] := new R(1); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// CONTROLS — a BORROW argument (a named binding, a field read) must NOT be
// disposed: the collection stores it as a transit borrow; some OTHER holder
// still owns it and disposes it later, exactly once, on its own path.
// ===========================================================================

// A named-local BORROW argument to Insert: the array borrows it; `r` is
// disposed once by its own `using var` scope end, never by the Insert call.
BEZEL_TEST(borrowing_collection_insert_temp_2204,
           array_insert_named_borrow_not_disposed_28_11) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  using var r := new R(1); "
        "  arr.Insert(0, r); "       // borrow argument — not a fresh producer
        "  return R._n; ");          // r still owned by its own using var here -> 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// A named-local BORROW RHS to the indexer-set: the array borrows it; `r`
// is disposed once by its own `using var` scope end (after the return).
BEZEL_TEST(borrowing_collection_indexer_set_temp_2204,
           array_indexer_set_named_borrow_not_disposed_28_11) {
    const std::string src = prog(
        "",
        "  var first := new R(0); "
        "  using var arr := new Array<R>(1, first); "
        "  using var r := new R(1); "
        "  arr[0] := r; "          // borrow RHS — not a fresh producer
        "  return R._n; ");        // r still owned by its own using var here
                                  // and the fill is owned by `first` — both
                                  // disposed at scope end AFTER the return -> 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// A named-local BORROW RHS to the dict indexer-set: the dict borrows it;
// `r` is disposed once by its own `using var` scope end.
BEZEL_TEST(borrowing_collection_indexer_set_temp_2204,
           dict_indexer_set_named_borrow_not_disposed_28_11) {
    const std::string src = prog(
        "",
        "  using var d := new Dictionary<string, R>(); "
        "  using var r := new R(1); "
        "  d[\"k\"] := r; "        // borrow RHS — not a fresh producer
        "  return R._n; ");        // r still owned by its own using var here -> 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// CONTROL — the OWNING-collection sibling (OwningArray<T>.Insert and
// OwningArray<T>[i] := owned v) is UNCHANGED: it takes ownership via the
// pre-existing #1955 transfer path, never the new dispose-after-Insert/
// dispose-after-IndexerSet path (mutually exclusive on `arrIsOwning`).
// Disposal happens later, at the collection's own scope end / dispose.
// ===========================================================================
BEZEL_TEST(borrowing_collection_insert_temp_2204,
           owning_array_insert_unaffected_transfers_ownership_1955) {
    const std::string src = prog(
        "",
        "  using var arr := new OwningArray<R>(); "
        "  arr.Insert(0, new R(1)); "
        "  var midCount := R._n; "  // NOT yet disposed here (owning collection
                                    // holds it) — would be 1 if wrongly disposed
        "  return midCount; ");    // 0: still owned by arr, not by this statement
    BEZEL_EXPECT_EQ(run(src), 0);
}

BEZEL_TEST(borrowing_collection_insert_temp_2204,
           owning_array_insert_disposed_once_at_scope_end_28_11) {
    const std::string src = prog(
        "",
        "  do "
        "    using var arr := new OwningArray<R>(); "
        "    arr.Insert(0, new R(1)); "
        "  end; "
        "  return R._n; ");  // disposed once via the OWNING collection cascade -> 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// EXCEPTION-PATH AXIS — a throw between the Insert call and a LATER
// statement is unaffected: the temp was already disposed (synchronously,
// right after the call) before the later throw, so the unwind finds
// nothing left to double-free. Encoding: caught*10 + R._n.
// ===========================================================================
BEZEL_TEST(borrowing_collection_insert_temp_2204,
           no_double_free_when_throw_follows_insert_28_2) {
    const std::string src = prog(
        "class W "
        "  public static method Step(x: int) "
        "    if x > 0 then throw new InvalidOperationException(\"boom\"); end; "
        "  end Step; "
        "end W; "
        "class Run1 "
        "  public static method Go(): int "
        "    using var arr := new Array<R>(); "
        "    arr.Insert(0, new R(1)); "  // disposes R._n -> 1 immediately
        "    W.Step(1); "                // throws AFTER the temp was already disposed
        "    return 0; "
        "  end Go; "
        "end Run1; ",
        "  var caught := 0; "
        "  try "
        "    _ := Run1.Go(); "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return caught * 10 + R._n; ");  // 1*10 + 1 = 11, disposed exactly once
    BEZEL_EXPECT_EQ(run(src), 11);
}
