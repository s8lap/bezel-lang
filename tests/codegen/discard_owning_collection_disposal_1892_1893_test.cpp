// fix #1892 / #1893 — §17.13 (discard of a named owned binding) + §28.3
// (exception-path discharge) + §28.5 (owning-collection disposal cascade),
// v0.194. Codegen-observable, LeakSanitizer-checked coverage for two related
// disposal-codegen gaps around the `_ := x;` DISCARD form:
//
//   #1892  `_ := coll;` where `coll` is a named owning-COLLECTION local
//          (OwningArray / OwningLinkedList / OwningDictionary / …) disposed
//          NOTHING — the container and every owned element leaked. The discard
//          reached the class-only reclaim path (explicitDisposeOwnedReceiver-
//          Slot → unwindOwnedLocalClass, which excludes collections) and
//          emitted no disposal at all, while markDisposedByName suppressed the
//          scope-end auto-drop that would otherwise have disposed it. Per §17.13
//          the discard is an "announced consume" — the value is disposed at the
//          END OF THE DISCARD STATEMENT — and per §28.5 disposing an owning
//          collection cascades into each owned element. The fix disposes the
//          container (bz_array_free / bz_dict_free / bz_llist_free with the
//          element-disposer cascade) at the discard point and nulls the slot.
//
//   #1893  a discharged owned local (`_ := x;`) LEAKED on the EXCEPTION path
//          when an operation between acquisition and the discard threw. The
//          UnwindIneligibleWalker treated `_ := x` as a consuming use and marked
//          `x` unwind-INELIGIBLE (flow-insensitively — even though the discard
//          textually FOLLOWS the throwing call), dropping the implicit §28.3
//          exception-path disposal for the edge that precedes the discard. The
//          fix treats a bare-identifier discard as a BORROW for ineligibility
//          (exactly as `x.Dispose()` already was), so `x` stays unwind-eligible
//          and is disposed on the pre-discard-throw edge. For an owning
//          collection the same edge is covered by routing a discard-discharged
//          collection local through the full (both-edges, null-safe) collection
//          disposal at its LocalDecl (dischargedByDiscard).
//
// Every observable rides on a single `runAotChecked` run: the exit code encodes
// a destructor COUNT (class R bumps a static `R._n` on dispose), and the same
// AOT+ASan/LSan run flips `ran` to false on any leak / double-free /
// use-after-free. So each assertion proves the destructor ran the right number
// of times AND that memory is exactly-once-disposed. FAIL-WITHOUT/PASS-WITH:
// before the fix the leak legs return the wrong count AND flag an LSan leak;
// the double-free control legs would return an inflated count or crash.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Disposed-exactly-once oracle: a leak / double-free / use-after-free fails the
// checked run (ran=false -> -999), so the sanitizer verdict rides on the same
// assertion as the destructor-count exit code.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A resource class R whose destructor bumps a static disposal counter R._n. No
// instance-level refcounted field, so emitDisposeAndReclaim is exact for R.
const char* kR =
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; ";

// A Worker whose Step throws InvalidOperationException when x > 0 — the mid-
// scope operation that throws BEFORE the discard is reached (#1893 / #1892 exc).
const char* kWorker =
    "class W "
    "  public static method Step(x: int) "
    "    if x > 0 then throw new InvalidOperationException(\"boom\"); end; "
    "  end Step; "
    "end W; ";

std::string prog(const std::string& classesAfterR, const std::string& mainBody) {
    return std::string("module M; ") + kR + classesAfterR +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ===========================================================================
// #1892 — normal-path discard of an owning collection disposes the container
// AND its owned elements, at the DISCARD STATEMENT (§17.13 timing), exactly
// once (§28.5 cascade). FAIL-WITHOUT: the discard disposed nothing → R._n was
// 0 at the return AND the container + both elements leaked (LSan flag).
// ===========================================================================
BEZEL_TEST(discard_owning_collection_1892, owning_array_discard_disposes_container_and_elements_17_13) {
    // Two elements added; `_ := arr;` disposes both at the discard point, so
    // R._n == 2 is observable at the very next statement (discard-point timing,
    // §17.13 — NOT deferred to scope end). Before the fix: 0 (+ 200-byte leak).
    const std::string src = prog(
        "",
        "  var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  arr.Add(new R()); "
        "  _ := arr; "
        "  return R._n; ");  // disposed at the discard → 2
    BEZEL_EXPECT_EQ(run(src), 2);
}

BEZEL_TEST(discard_owning_collection_1892, owning_array_discard_disposed_exactly_once_across_call_28_5) {
    // The discard is inside a helper; Main reads the counter after the helper's
    // scope has fully torn down. Disposed exactly once (no double-free between
    // the discard-point disposal and the LocalDecl both-edges registration whose
    // reload finds the nulled slot) → R._n == 2. Before the fix: 0 (+ leak).
    const std::string src = prog(
        "class H "
        "  public static method Consume() "
        "    var arr := new OwningArray<R>(); "
        "    arr.Add(new R()); "
        "    arr.Add(new R()); "
        "    _ := arr; "
        "  end Consume; "
        "end H; ",
        "  H.Consume(); "
        "  return R._n; ");  // 2, disposed exactly once
    BEZEL_EXPECT_EQ(run(src), 2);
}

BEZEL_TEST(discard_owning_collection_1892, owning_linked_list_discard_disposes_elements_28_5) {
    // §4.7 generalization: an OwningLinkedList<R> discard cascades disposal into
    // its node elements the same way (bz_llist_free with the element disposer).
    // One element → R._n == 1 at the discard point. Before the fix: 0 (+ leak).
    const std::string src = prog(
        "",
        "  var ll := new OwningLinkedList<R>(); "
        "  ll.AddLast(new R()); "
        "  _ := ll; "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

BEZEL_TEST(discard_owning_collection_1892, owning_dictionary_discard_disposes_values_28_5) {
    // §4.9 generalization: an OwningDictionary<int,R> discard cascades disposal
    // into its owned values (bz_dict_free with the value disposer). One value →
    // R._n == 1 at the discard point. Before the fix: 0 (+ leak).
    const std::string src = prog(
        "",
        "  var d := new OwningDictionary<int, R>(); "
        "  d.Add(1, new R()); "
        "  _ := d; "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// A `using var` owning-collection discard (§17.13; v0.184 lists `using var`
// among the eligible discard receivers). Before the fix this SEGV'd: the
// discard's class reclaim path (explicitDisposeOwnedReceiverSlot returns the
// using-var's slot) fed the collection struct into emitDisposeAndReclaim, which
// read its first word as a vtable pointer → crash. The fix routes any
// owning-collection discard (plain OR using var) through the collection free.
// The discard disposes the container at the discard point (R._n == 1) and nulls
// the slot; the using var's own scope-end disposal then reloads null and no-ops.
// Disposed exactly once, no crash, no double-free.
// ===========================================================================
BEZEL_TEST(discard_owning_collection_1892, using_var_owning_collection_discard_no_crash_17_13) {
    const std::string src = prog(
        "",
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  _ := arr; "
        "  return R._n; ");  // disposed at the discard, not crashed → 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// #1893 — a discharged owned CLASS local is disposed on the EXCEPTION path
// when an operation between acquisition and the discard throws. W.Step(1)
// throws BEFORE `_ := r;` is reached, so at the throw point r is still owned
// and must be auto-disposed by the §28.3 implicit try/finally on the exception
// edge. FAIL-WITHOUT: the discard's markDisposedByName made the ineligibility
// analysis treat r as already-disposed, dropping the finally → r leaked
// (R._n == 0). Encoding: caught*10 + R._n.
// ===========================================================================
BEZEL_TEST(discard_owned_local_exception_1893, class_local_disposed_on_pre_discard_throw_28_3) {
    const std::string src = prog(
        std::string(kWorker) +
        "class Run1 "
        "  public static method Go(): int "
        "    var r := new R(); "
        "    W.Step(1); "     // throws — r still owned/undischarged here
        "    _ := r; "        // never reached
        "    return 0; "
        "  end Go; "
        "end Run1; ",
        "  var caught := 0; "
        "  try "
        "    _ := Run1.Go(); "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return caught * 10 + R._n; ");  // 1*10 + 1 = 11 (r disposed on unwind)
    BEZEL_EXPECT_EQ(run(src), 11);
}

// ===========================================================================
// #1892 + #1893 combined — a discharged owning-COLLECTION local disposed on the
// pre-discard-throw exception edge. registerUnwindOwnedLocal does not cover
// collections, so before the fix the container (and its owned element) leaked
// on the throw edge; the dischargedByDiscard both-edges registration covers it.
// Encoding: caught*10 + R._n → 1*10 + 1 = 11 (element disposed on unwind).
// FAIL-WITHOUT: 10 (+ 184-byte leak).
// ===========================================================================
BEZEL_TEST(discard_owned_local_exception_1893, owning_array_disposed_on_pre_discard_throw_28_3_28_5) {
    const std::string src = prog(
        std::string(kWorker) +
        "class Run2 "
        "  public static method Go(): int "
        "    var arr := new OwningArray<R>(); "
        "    arr.Add(new R()); "
        "    W.Step(1); "     // throws — arr still owned/undischarged here
        "    _ := arr; "      // never reached
        "    return 0; "
        "  end Go; "
        "end Run2; ",
        "  var caught := 0; "
        "  try "
        "    _ := Run2.Go(); "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return caught * 10 + R._n; ");  // 11
    BEZEL_EXPECT_EQ(run(src), 11);
}

// ===========================================================================
// CONTROL — no double-free when the throw is AFTER the discard. The discard
// disposes the collection and NULLS the slot; the both-edges LocalDecl
// registration then reloads null on the unwind edge and no-ops. Disposed
// EXACTLY ONCE. Encoding: caught*10 + R._n → 1*10 + 1 = 11 (a double-free would
// crash the checked run, and a leak would return 10).
// ===========================================================================
BEZEL_TEST(discard_owning_collection_1892, no_double_free_when_throw_follows_discard_28_2) {
    const std::string src = prog(
        std::string(kWorker) +
        "class Run3 "
        "  public static method Go(): int "
        "    var arr := new OwningArray<R>(); "
        "    arr.Add(new R()); "
        "    _ := arr; "      // disposes + nulls the slot (R._n == 1)
        "    W.Step(1); "     // throws AFTER the discard — unwind reload is null
        "    return 0; "
        "  end Go; "
        "end Run3; ",
        "  var caught := 0; "
        "  try "
        "    _ := Run3.Go(); "
        "  catch InvalidOperationException => "
        "    caught := 1; "
        "  end; "
        "  return caught * 10 + R._n; ");  // 11, disposed exactly once
    BEZEL_EXPECT_EQ(run(src), 11);
}

// ===========================================================================
// CONTROL — a `move`d-out owning-collection source is NOT double-freed by the
// discard machinery. `arr` is moved into `arr2`; only `arr2` is discarded. The
// dischargedByDiscard flag is set on `arr2` (the discard target), never on the
// moved-from `arr`, so the container is disposed exactly once via arr2's
// discard. R._n == 1. A regression that registered a disposal for the moved-from
// source would double-free (crash) or over-count.
// ===========================================================================
BEZEL_TEST(discard_owning_collection_1892, moved_out_source_not_double_freed_28_8_1) {
    const std::string src = prog(
        "",
        "  var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  var arr2 := move arr; "
        "  _ := arr2; "
        "  return R._n; ");  // 1, disposed exactly once via arr2
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// CONTROL — an explicit `arr.Dispose()` on a collection is unaffected by the
// dischargedByDiscard path (that flag is set ONLY for the `_ := x` discard
// form). Dispose remains the sole disposer; no both-edges registration is added
// for it, so there is no double-free. R._n == 1.
// ===========================================================================
BEZEL_TEST(discard_owning_collection_1892, explicit_dispose_unaffected_no_double_free_28_2) {
    const std::string src = prog(
        "",
        "  var arr := new OwningArray<R>(); "
        "  arr.Add(new R()); "
        "  arr.Dispose(); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// CONTROL — an owning-collection local NEVER discharged (plain scope-end
// auto-drop, no discard) is still disposed exactly once. This pins that the
// dischargedByDiscard branch did not disturb the pre-existing auto-drop path.
// R._n == 1.
// ===========================================================================
BEZEL_TEST(discard_owning_collection_1892, undischarged_autodrop_still_disposes_once_28_3) {
    const std::string src = prog(
        "class H2 "
        "  public static method Hold() "
        "    var arr := new OwningArray<R>(); "
        "    arr.Add(new R()); "
        "  end Hold; "     // scope-end auto-drop disposes it (no discard)
        "end H2; ",
        "  H2.Hold(); "
        "  return R._n; ");  // 1
    BEZEL_EXPECT_EQ(run(src), 1);
}
