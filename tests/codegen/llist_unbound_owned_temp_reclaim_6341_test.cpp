// Codegen — #6341 (a) / §28.3 *unbound owned temporaries* / §28.9 / §29.2 /
// stdlib §4.6 / §4.7 (v0.338): an `owned LinkedList<T>`-producing call
// consumed UNBOUND as the receiver of `.Count` / `.Count()` / `for-in` leaked
// the whole list, while the `Array<T>` and `Dictionary<K,V>` twins of the
// identical loop were clean.
//
//     public method Make(): owned LinkedList<string>
//       return new LinkedList<string>();
//     end Make;
//     // Make().Count / Make().Count() / for var s in Make() do … end
//
// ROOT CAUSE.  `LinkedList<T>` is `bz_llist_t`-backed and is in
// `isPlainBuiltinCollectionHead`, so #6352 taught
// `freeUnboundOwnedBorrowingArrayTemp` to release it via `bz_llist_free`.  But
// three LinkedList faces never CALL that helper, so the arm #6352 added was
// live and unreachable from them:
//
//   * the `.Count` PROPERTY arm (LlvmBackend_Exprs.cpp) returned straight out
//     of `bz_llist_count` and was UPSTREAM of the reclaim the array-backed
//     `.Length`/`.Count` arm makes;
//   * the no-arg `.Count()` CALL arm (LlvmBackend_Calls3.cpp) returned straight
//     out of `bz_llist_count` with no `freeFreshInputTemp()` — the lambda the
//     `Array` `.Length`/`.Count` arm routes its reclaim through;
//   * `lowerLinkedListFor` (LlvmBackend_Stmts.cpp) reached no disposal at all,
//     unlike `lowerArrayFor`, which reclaims at its own `endBB` (#1055).
//
// MEASURED on this fix's parent (r31-c1 @ d0754095), 100 iterations, ASan
// `detect_leaks=1`: 5,600 B in 100 allocations for EACH of the three faces,
// against 0 for the `Array<T>` twin and 0 for the `Dictionary<K,V>` twin of
// the byte-identical loop.  Post-fix: 0 on all three.
//
// ORACLE.  `runAotChecked` is the LSan-clean oracle (ASan-instrumented
// generated code, `detect_leaks=1:exitcode=99:abort_on_error=0`); a real
// sanitizer finding surfaces as `r.ran == false`.  Every leak row below is
// LOOP-AMPLIFIED (200 iterations) — a one-shot LSan row can pass on both sides.
// `Main` returns `acc mod 100`, a value computed from the loop, so the body
// cannot be constant-folded away.
//
// ⚠ THIS IS NOT #6341's SECOND HALF.  The instance-`Array<T>`-FIELD backing
// store (#6341(b), §28.9) is a separate DISPOSAL-layer defect, filed and
// tracked separately; nothing here touches a field slot.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// Plain (non-ASan) oracle for the value-correctness control.
static int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -998;
}

namespace {

// LSan-clean oracle: a real sanitizer failure (leak / UAF / OOB) surfaces as
// `r.ran == false` and yields -999; a clean program returns its exit code.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// The `owned LinkedList<T>` producer under test.  `int` elements keep the rows
// free of the `Array<string>` co-owned-+1 element-disposer question, which is a
// SEPARATE axis (the reclaim's string handling is #6352's, unchanged here);
// the `.Count` PROPERTY face is additionally pinned with `string` elements
// below so both element categories are covered on that face.
const char* kFactoryInt =
    "module M; "
    "public class Factory "
    "  public method Make(): owned LinkedList<int> "
    "    return new LinkedList<int>(); "
    "  end Make; "
    "end Factory; ";

const char* kFactoryStr =
    "module M; "
    "public class Factory "
    "  public method Make(): owned LinkedList<string> "
    "    return new LinkedList<string>(); "
    "  end Make; "
    "end Factory; ";

// The `Array<T>` / `Dictionary<K,V>` twins — the CONTROL that establishes
// LinkedList as the outlier rather than the whole borrowing family leaking.
// These rows are green on this fix's PARENT too; they exist so a future change
// that widens the regression to the twins is caught here rather than blamed on
// #6341.
const char* kFactoryArray =
    "module M; "
    "public class Factory "
    "  public method Make(): owned Array<int> "
    "    return new Array<int>(); "
    "  end Make; "
    "end Factory; ";

const char* kFactoryDict =
    "module M; "
    "public class Factory "
    "  public method Make(): owned Dictionary<int, int> "
    "    return new Dictionary<int, int>(); "
    "  end Make; "
    "end Factory; ";

// Wrap a factory preamble + a Main body into a whole program.
std::string prog(const char* preamble, const std::string& body) {
    return std::string(preamble) +
           "class C  public static method Main(): int "
           "  var f := new Factory(); "
           "  var i := 0; var acc := 0; "
           "  while i < 200 do " + body +
           "    i += 1; "
           "  end; "
           "  return acc mod 100; "
           "end Main; end C; ";
}

}  // namespace

// ===========================================================================
// The three leaking faces.  Each is loop-amplified 200x and reads a value
// derived from the loop, so a pass cannot come from a constant-folded body.
// ===========================================================================

// Face 1 — `.Count` PROPERTY.  The arm that returned upstream of the reclaim.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_unbound_count_property_no_leak_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    acc += f.Make().Count; ")), 0);
}

// Face 1, string elements: the reclaim's element-disposer axis is #6352's and
// unchanged, but a `LinkedList<string>` free must still not leak.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_unbound_count_property_string_no_leak_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryStr,
        "    acc += f.Make().Count; ")), 0);
}

// Face 2 — `.Count()` no-arg CALL.  The arm with no `freeFreshInputTemp()`.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_unbound_count_method_no_leak_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    acc += f.Make().Count(); ")), 0);
}

// Face 3 — `for-in` over an unbound owned list.  `lowerLinkedListFor` reached
// no disposal at all; spec §29.2 (the enumeration protocol this lowering
// implements) plus §28.3's unbound-temporary rule put the obligation at the
// end of the loop.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_unbound_forin_no_leak_spec_29_2) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    for var s in f.Make() do acc += s; end; ")), 0);
}

// All three faces in ONE program, interleaved with a bound `using var` —
// this is the shape that would catch a reclaim that fires on the wrong face or
// twice on one.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_all_three_faces_mixed_with_using_var_no_leak_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    acc += f.Make().Count; "
        "    acc += f.Make().Count(); "
        "    for var s in f.Make() do acc += s; end; "
        "    using var l := f.Make(); "
        "    acc += l.Count + l.Count(); "
        "    for var t in l do acc += t; end; ")), 0);
}

// ===========================================================================
// OVER-RECLAIM GUARDS.  A reclaim that fires too eagerly is a double-free /
// UAF, and ASan reports those as a NON-leak finding — so `runChecked` (which
// yields -999 on ANY sanitizer finding) is the right oracle here, and every
// one of these rows must be GREEN BOTH BEFORE AND AFTER this fix.  Each binds
// the result so the receiver is an IdentifierExpr, which
// `isUnboundOwnedBorrowingArrayCallExpr` must decline.
// ===========================================================================

// A bound `using var` local is disposed by ITS OWN using-disposal; the three
// reclaims must not touch it (double-free).
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_bound_using_var_still_disposed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    using var l := f.Make(); "
        "    acc += l.Count; "
        "    acc += l.Count(); "
        "    for var s in l do acc += s; end; ")), 0);
}

// A plain `var` binding carries its own §28.3 owned-local discharge.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_bound_alias_local_still_disposed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    var l := f.Make(); "
        "    acc += l.Count; "
        "    acc += l.Count(); "
        "    for var s in l do acc += s; end; ")), 0);
}

// An explicitly `.Dispose()`d local: the explicit dispose is the sole free, so
// a reclaim here would be a second free of the same backing.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_explicit_dispose_then_reads_still_disposed_once_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    var l := f.Make(); "
        "    acc += l.Count; "
        "    acc += l.Count(); "
        "    for var s in l do acc += s; end; "
        "    l.Dispose(); ")), 0);
}

// A local bound from the SAME producer, then read through an ALIAS (`move`) —
// the alias must not make the reclaim fire on an already-owned binding.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_moved_alias_receiver_not_reclaimed_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryInt,
        "    var l := f.Make(); "
        "    var m := move l; "
        "    acc += m.Count; "
        "    acc += m.Count(); "
        "    for var s in m do acc += s; end; ")), 0);
}

// A value read back through a BOUND list after the three faces ran on OTHER
// unbound lists — proves the frees did not corrupt the heap around a live list.
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_bound_list_reads_correct_after_unbound_frees_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "public class Factory "
        "  public method Make(): owned LinkedList<int> "
        "    return new LinkedList<int>(); "
        "  end Make; "
        "  public method MakeFull(): owned LinkedList<int> "
        "    var l := new LinkedList<int>(); "
        "    l.Add(7); l.Add(8); l.Add(9); "
        "    return l; "
        "  end MakeFull; "
        "end Factory; "
        "class C  public static method Main(): int "
        "  var f := new Factory(); "
        "  var acc := 0; var i := 0; "
        "  while i < 200 do "
        "    acc += f.Make().Count; "
        "    acc += f.Make().Count(); "
        "    for var s in f.Make() do acc += s; end; "
        "    i += 1; "
        "  end; "
        "  using var live := f.MakeFull(); "
        "  if live.Count <> 3 then return 1; end; "
        "  var t := 0; "
        "  for var s in live do t += s; end; "
        "  if t <> 24 then return 2; end; "
        "  return acc mod 100; "
        "end Main; end C; "), 0);
}

// ===========================================================================
// VALUE-CORRECTNESS CONTROL (plain oracle, non-loop): the fix must not change
// what value the three faces actually read.  `.Count` reads 3 off a 3-element
// list; `for-in` sums 24 (7+8+9).
// ===========================================================================
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           llist_unbound_faces_read_correct_values_plain_oracle) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Factory "
        "  public method Make(): owned LinkedList<int> "
        "    var l := new LinkedList<int>(); "
        "    l.Add(7); l.Add(8); l.Add(9); "
        "    return l; "
        "  end Make; "
        "end Factory; "
        "class C  public static method Main(): int "
        "  var f := new Factory(); "
        "  if f.Make().Count <> 3 then return 1; end; "
        "  if f.Make().Count() <> 3 then return 2; end; "
        "  var t := 0; "
        "  for var s in f.Make() do t += s; end; "
        "  if t <> 24 then return 3; end; "
        "  return 0; "
        "end Main; end C; "), 0);
}

// ===========================================================================
// TWIN CONTROLS — green on this fix's PARENT as well.  They pin that this
// issue was the LinkedList OUTLIER and that the fix did not silently change
// the twins' behavior.
// ===========================================================================
BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           array_twin_unbound_faces_unchanged_no_leak_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryArray,
        "    acc += f.Make().Count; "
        "    acc += f.Make().Count(); "
        "    for var s in f.Make() do acc += s; end; ")), 0);
}

BEZEL_TEST(llist_unbound_owned_temp_reclaim_6341,
           dictionary_twin_unbound_faces_unchanged_no_leak_spec_28_3) {
    // NOTE this row covers the two faces #6341 mirrors on the dict-backed
    // twin, and deliberately does NOT cover `f.Make().Keys`. That shape leaks
    // 70,400 B / 400 allocations over the same 200-iteration loop — MEASURED
    // IDENTICALLY ON THIS FIX'S PARENT (`bz_dict_alloc_generic`,
    // bezel_runtime_collections.cpp:1079/1083), i.e. it is PRE-EXISTING and
    // untouched by this fix, which reaches only `isLinkedListTypeName` arms.
    // Filed separately rather than folded in: it is the `SnapshotKeys`
    // materializer-temp reclaim of the #1013 family, a different site.
    BEZEL_EXPECT_EQ(runChecked(prog(kFactoryDict,
        "    acc += f.Make().Count; "
        "    acc += f.Make().Count(); ")), 0);
}