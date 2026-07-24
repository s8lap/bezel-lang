// fix #1881 — §28.3 (unbound owned temporaries) + §28.11 (borrowing
// collections / the store-a-borrow rule), spec v0.226. Codegen-observable,
// LeakSanitizer-checked coverage for: an owned TEMPORARY (`new Res(1)`)
// passed to a BORROWING collection's `Add` (a non-owned argument position) was
// silently leaked — no disposal, no reject, no diagnostic.
//
// Spec resolution (this is a DISPOSE, not a reject, per §28.3 v0.226 line
// ~2771): "The rejection is deliberately literal-only (recorded): Add,
// Insert, and the indexer-set on a borrowing collection remain governed by
// §28.3's unbound-temporary admission and §28.11's residual-trust store rule
// — an opaque callee's signature does not announce retention — so
// coll.Add(new Connection("a")) stays admissible and stays the borrow-
// lifetime hazard that checked disposal mode backstops." Only the COLLECTION-
// LITERAL form (`Array<T> := [new X()]`) is rejected at the literal site — a
// call like `Add` is never rejected. §28.3's own "Unbound owned temporaries"
// enumeration lists "a non-owned argument" as one of the borrowing uses an
// unbound owned temporary may serve before being "disposed at the end of the
// full expression in which it is produced." `Add`/`Push`/`Enqueue` return
// unit, so nothing in the statement can chain off the call's result — the
// call's own end IS that full-expression boundary for this argument.
//
// The fix (LlvmBackend_Calls.cpp, the array-backed Add/Push/Enqueue lowering)
// disposes a fresh-producer argument (isFreshOwnedClassProducer — the same
// discriminant the #1039/#1053 receiver-temp fix uses) immediately after the
// `bz_array_add` call, when the receiving collection is BORROWING
// (`!arrIsOwning`; an OWNING collection instead transfers ownership via the
// pre-existing #1955 nulling path — mutually exclusive, no double dispatch).
// This intentionally creates the exact "borrow-lifetime hazard that checked
// disposal mode backstops" the spec names: the array's stored copy of the
// pointer dangles from this point on — reading it back is undefined behavior
// in the DEFAULT (unchecked) disposal-checking mode (§28.2; "checked disposal
// mode" is a separate, opt-in per-build --checked-disposal flag, not the
// memory-sanitizer sense `runAotChecked` tests) and would deterministically
// raise DisposedAccessException in checked mode. None of the tests below read
// a stored element back — every observable is the destructor-count exit code,
// which is unaffected by whether the stored slot is later read.
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
// THE BUG (issue's own repro shape) — `arr.Add(new Res(1))` on a borrowing
// `Array<R>`: the temporary is disposed at the end of the statement (its
// destructor observably runs), with no leak. FAIL-WITHOUT: R._n == 0 (+ leak).
// ===========================================================================
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  arr.Add(new R(1)); "
        "  return R._n; ");  // disposed at end of the Add statement -> 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// Two fresh temps added in separate statements — each disposed exactly once,
// in production order.
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_two_fresh_temps_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  arr.Add(new R(1)); "
        "  arr.Add(new R(2)); "
        "  return R._n; ");  // 2
    BEZEL_EXPECT_EQ(run(src), 2);
}

// An `owned`-returning FACTORY call (not a bare `new`) used as the Add
// argument is the SAME kind of unbound owned temporary — the sound gate is
// isFreshOwnedClassProducer's `owned`-return discriminant, not a `new`-only
// whitelist.
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_owned_returning_call_temp_disposed_28_3) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(x: int): owned R return new R(x); end Make; "
        "end Mk; ",
        "  using var arr := new Array<R>(); "
        "  arr.Add(Mk.Make(3)); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// PRODUCER-FAMILY AXIS — Stack<R>.Push and Queue<R>.Enqueue share the SAME
// array-backed Add/Push/Enqueue lowering (LlvmBackend_Calls.cpp), so the fix
// covers all three uniformly.
BEZEL_TEST(borrowing_collection_add_temp_1881, stack_push_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var s := new Stack<R>(); "
        "  s.Push(new R(1)); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

BEZEL_TEST(borrowing_collection_add_temp_1881, queue_enqueue_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        "",
        "  using var q := new Queue<R>(); "
        "  q.Enqueue(new R(1)); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// CONTROLS — a BORROW argument (a named binding, a field read) must NOT be
// disposed: the collection stores it as a transit borrow; some OTHER holder
// still owns it and disposes it later, exactly once, on its own path.
// ===========================================================================

// A named-local BORROW argument: the collection borrows it; `r` is disposed
// once by its own `using var` scope end, never by the Add call.
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_named_borrow_not_disposed_28_11) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  using var r := new R(1); "
        "  arr.Add(r); "          // borrow argument — not a fresh producer
        "  return R._n; ");       // r still owned by its own using var here -> 0
    BEZEL_EXPECT_EQ(run(src), 0);
}

// The SAME named-local `r`, added by borrow then disposed once by its own
// `using var` scope end — proves no leak AND no double-free across the whole
// statement sequence (the control's full lifecycle, not just the snapshot
// read above).
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_named_borrow_disposed_once_at_scope_end_28_11) {
    const std::string src = prog(
        "",
        "  do "
        "    using var arr := new Array<R>(); "
        "    using var r := new R(1); "
        "    arr.Add(r); "
        "  end; "
        "  return R._n; ");  // r disposed exactly once at its own scope end -> 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// CONTROL — the OWNING-collection sibling (OwningArray<R>.Add) is UNCHANGED:
// it takes ownership via the pre-existing #1955 transfer path, never the new
// dispose-after-Add path (mutually exclusive on `arrIsOwning`). Disposal
// happens later, at the collection's own scope end / dispose, not immediately
// after Add.
// ===========================================================================
BEZEL_TEST(borrowing_collection_add_temp_1881, owning_array_add_unaffected_transfers_ownership_1955) {
    const std::string src = prog(
        "",
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R(1)); "
        "  var midCount := R._n; "  // NOT yet disposed here (owning collection
                                    // holds it) — would be 1 if wrongly disposed
        "  return midCount; ");    // 0: still owned by arr, not by this statement
    BEZEL_EXPECT_EQ(run(src), 0);
}

BEZEL_TEST(borrowing_collection_add_temp_1881, owning_array_add_disposed_once_at_scope_end_28_11) {
    const std::string src = prog(
        "",
        "  do "
        "    using var arr := new OwningArray<R>(); "
        "    arr.Add(new R(1)); "
        "  end; "
        "  return R._n; ");  // disposed once via the OWNING collection cascade -> 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// EXCEPTION-PATH AXIS — a throw between the Add call and a LATER statement is
// unaffected: the temp was already disposed (synchronously, right after the
// call) before the later throw, so the unwind finds nothing left to double-
// free. Encoding: caught*10 + R._n.
// ===========================================================================
BEZEL_TEST(borrowing_collection_add_temp_1881, no_double_free_when_throw_follows_add_28_2) {
    const std::string src = prog(
        "class W "
        "  public static method Step(x: int) "
        "    if x > 0 then throw new InvalidOperationException(\"boom\"); end; "
        "  end Step; "
        "end W; "
        "class Run1 "
        "  public static method Go(): int "
        "    using var arr := new Array<R>(); "
        "    arr.Add(new R(1)); "  // disposes R._n -> 1 immediately
        "    W.Step(1); "          // throws AFTER the temp was already disposed
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
