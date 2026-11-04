// ── v0.346 (proposal 0207, #3047 lint A) ──────────────────────────────────
// The literal-only carve-out quoted below is GONE. §15.4 (v0.355): "a fresh
// unbound owned temporary of class type … in a single-element store position
// of a borrowing-family collection is a compile error at the store site" —
// so `arr.Add(new R(1))`, `s.Push(…)`, `q.Enqueue(…)` and the `owned`-call
// form no longer compile, and their rows below now pin the REJECTION. The
// mechanism they observed (K8.1's boundary disposal of a fresh temporary at a
// plain, non-owned argument) survives at the sink §15.4 keeps admitted — "A
// plain parameter of a user-declared member stays admitted" — and is re-pinned
// there (`plain_param_sink_*`). The named-borrow and owning-family controls
// are unchanged.
// ──────────────────────────────────────────────────────────────────────────
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

#include "support/aot_result_asserts.hpp"
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

// §15.4 (v0.355): the program is rejected at compile time, by the fresh-store
// rule, naming the store member.
void expectFreshStoreError(const std::string& src, const char* store) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "§15.4 fresh-store error"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, store));
}

// A user-declared plain-parameter sink (§15.4 v0.355 keeps it admitted).
const char* kSink =
    "class Sink "
    "  public static method Take(r: R): int return r.id; end Take; "
    "end Sink; ";

std::string prog(const std::string& classesAfterR, const std::string& mainBody) {
    return std::string("module M; ") + kR + classesAfterR +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE BUG (issue's own repro shape) — `arr.Add(new Res(1))` on a borrowing
// `Array<R>`. Pre-v0.346 the temporary was disposed at the end of the statement
// (R._n == 1, no leak); since v0.346 the program is §15.4's fresh-store error.
// ===========================================================================
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_fresh_temp_is_fresh_store_error_15_4) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  arr.Add(new R(1)); "
        "  return R._n; ");  // disposed at end of the Add statement -> 1
    expectFreshStoreError(src, "`.Add(...)` store");
}

// Two fresh temps added in separate statements — each disposed exactly once,
// in production order (pre-v0.346); now the §15.4 error.
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_two_fresh_temps_is_fresh_store_error_15_4) {
    const std::string src = prog(
        "",
        "  using var arr := new Array<R>(); "
        "  arr.Add(new R(1)); "
        "  arr.Add(new R(2)); "
        "  return R._n; ");  // 2
    expectFreshStoreError(src, "`.Add(...)` store");
}

// An `owned`-returning FACTORY call (not a bare `new`) used as the Add
// argument is the SAME kind of unbound owned temporary — the sound gate is
// isFreshOwnedClassProducer's `owned`-return discriminant, not a `new`-only
// whitelist. Since v0.346 it is §15.4's fresh-store error alike ("an
// `owned`-returning call result").
BEZEL_TEST(borrowing_collection_add_temp_1881, array_add_owned_returning_call_temp_is_fresh_store_error_15_4) {
    const std::string src = prog(
        "class Mk "
        "  public static method Make(x: int): owned R return new R(x); end Make; "
        "end Mk; ",
        "  using var arr := new Array<R>(); "
        "  arr.Add(Mk.Make(3)); "
        "  return R._n; ");  // 1
    expectFreshStoreError(src, "`.Add(...)` store");
}

// PRODUCER-FAMILY AXIS — Stack<R>.Push and Queue<R>.Enqueue share the SAME
// array-backed Add/Push/Enqueue lowering (LlvmBackend_Calls.cpp), so the fix
// covers all three uniformly. Since v0.346 each is §15.4's fresh-store error
// (§15.4's positions include `Push` and `Enqueue`).
BEZEL_TEST(borrowing_collection_add_temp_1881, stack_push_fresh_temp_is_fresh_store_error_15_4) {
    const std::string src = prog(
        "",
        "  using var s := new Stack<R>(); "
        "  s.Push(new R(1)); "
        "  return R._n; ");  // 1
    expectFreshStoreError(src, "`.Push(...)` store");
}

BEZEL_TEST(borrowing_collection_add_temp_1881, queue_enqueue_fresh_temp_is_fresh_store_error_15_4) {
    const std::string src = prog(
        "",
        "  using var q := new Queue<R>(); "
        "  q.Enqueue(new R(1)); "
        "  return R._n; ");  // 1
    expectFreshStoreError(src, "`.Enqueue(...)` store");
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
// free. Encoding: caught*10 + R._n. Since v0.346 the Add spelling is the §15.4
// error (first row below); the throw-after-boundary observation is re-pinned at
// a plain-parameter sink (`plain_param_sink_no_double_free_when_throw_follows`).
// ===========================================================================
BEZEL_TEST(borrowing_collection_add_temp_1881, throw_follows_add_program_is_fresh_store_error_15_4) {
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
    expectFreshStoreError(src, "`.Add(...)` store");
}

// ===========================================================================
// RE-CUT (v0.346) — the K8.1 boundary disposal of a fresh temporary at a
// plain, non-owned argument, observed at the sink §15.4 (v0.355) keeps
// admitted: "A plain parameter of a user-declared member stays admitted".
// §28.3 K8.1: the temporary is "disposed at the end of the full expression in
// which it is produced". Same encodings as the pre-v0.346 rows above, except
// that the count is read AFTER a helper's scope has exited: on this tree a
// fresh temporary at a plain user-member argument is released at its
// enclosing SCOPE's exit, not at the call's end (measured; a §28.3 K8.1
// timing question recorded in the #3047 lane report, out of this lane), so
// a same-scope read would observe 0. Exactly-once is still pinned, by the
// count and by the LSan/ASan oracle.
// // per §28.3 K8.1, §15.4
// ===========================================================================
BEZEL_TEST(borrowing_collection_add_temp_1881, plain_param_sink_fresh_temp_disposed_28_3) {
    const std::string src = prog(
        std::string(kSink) +
        "class G "
        "  public static method Go(): int "
        "    return Sink.Take(new R(4)); "
        "  end Go; "
        "end G; ",
        "  var v := G.Go(); "
        "  return R._n * 10 + v; ");  // disposed once by Go's exit -> 14
    BEZEL_EXPECT_EQ(run(src), 14);
}

BEZEL_TEST(borrowing_collection_add_temp_1881, plain_param_sink_two_fresh_temps_disposed_28_3) {
    const std::string src = prog(
        std::string(kSink) +
        "class G "
        "  public static method Go() "
        "    _ := Sink.Take(new R(1)); "
        "    _ := Sink.Take(new R(2)); "
        "  end Go; "
        "end G; ",
        "  G.Go(); "
        "  return R._n; ");  // 2
    BEZEL_EXPECT_EQ(run(src), 2);
}

BEZEL_TEST(borrowing_collection_add_temp_1881, plain_param_sink_owned_returning_call_temp_disposed_28_3) {
    const std::string src = prog(
        std::string(kSink) +
        "class Mk "
        "  public static method Make(x: int): owned R return new R(x); end Make; "
        "end Mk; "
        "class G "
        "  public static method Go() "
        "    _ := Sink.Take(Mk.Make(3)); "
        "  end Go; "
        "end G; ",
        "  G.Go(); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

BEZEL_TEST(borrowing_collection_add_temp_1881, plain_param_sink_no_double_free_when_throw_follows_28_2) {
    const std::string src = prog(
        std::string(kSink) +
        "class W "
        "  public static method Step(x: int) "
        "    if x > 0 then throw new InvalidOperationException(\"boom\"); end; "
        "  end Step; "
        "end W; "
        "class Run1 "
        "  public static method Go(): int "
        "    _ := Sink.Take(new R(1)); "  // disposes R._n -> 1 at the call's end
        "    W.Step(1); "                // throws AFTER the temp was disposed
        "    return 0; "
        "  end Go; "
        "end Run1; ",
        "  var caught := 0; "
        "  try "
        "    _ := Run1.Go(); "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return caught * 10 + R._n; ");  // 11, disposed exactly once
    BEZEL_EXPECT_EQ(run(src), 11);
}
