// fix #3491 / stdlib §4.10 (v0.306) / §28.11 v0.135: a BORROWING `Set<T>`
// (as opposed to `OwningSet<T>`, #837/#1474) with a class-typed T had two
// defects in the array-backed linear-scan path
// (`Emitter::emitArrayBackedSetInsert`, #1070):
//
//   1. `Add(item)` for an element already present (per T's equality
//      contract, §8.3) re-appended it — `Count` grew. `emitArrayBackedSetInsert`
//      already existed and was shared with `OwningSet<T>.Add`/`.UnionWith`,
//      but the dedup call site was gated on `isOwnedSetRecv` (receiver
//      exactly `OwningSet<T>`), so a plain `Set<T>` fell through to the
//      generic array Add/Push/Enqueue handler, which just appends.
//   2. `TryAdd(item)` on a class-element `Set<T>` never lowered at all — no
//      codegen handler existed for a class receiver with `arrIsOwning ==
//      false` — so `tryLowerCollectionInstanceCall` returned nullptr and the
//      local it initialized degraded to codegen's #2434 zero-init fallback:
//      the `bool` §4.10 documents `TryAdd` to return was silently `false`
//      regardless of the real answer.
//
// stdlib §4.10 (v0.306): `public method Add(item: T);` and
// `public method TryAdd(item: T): bool;` on `Set<T>`; "the element type
// supplies the identity" (§8.3 gives a class reference identity by default,
// a user `Equals`/`GetHashCode` override honoured otherwise, per #837).
// §28.11 v0.135's class/interface row: the BORROWING column carries no
// disposer — a plain `Set<T>` must never dispose a rejected duplicate,
// unlike `OwningSet<T>` (§4.10's owning-only "on false, the item is
// disposed").
//
// Fix: widened the shared `emitArrayBackedSetInsert`-based dedup handling
// (already used by `OwningSet<T>.Add`/`.UnionWith`) to also cover a
// borrowing `Set<T>` receiver (`isBorrowingSetRecv`), passing a NULL
// dupDisposer (the ClassBorrow row of `elementPolicyFor` already returns
// null for `owningFamily=false`, so no separate plumbing was needed for
// that half), and added the missing `TryAdd` handler for the same
// borrowing-class-element shape by widening the existing
// `arrElemIsClass && arrIsOwning` gate to also admit
// `arrElemIsClass && isBorrowingSetRecv`.
//
// Deliberately NOT covered here: #2296 (a separate open issue describing
// the same `Set<C>.Add`/`TryAdd` non-dedup symptom) — out of scope per this
// fix's directive; not closed or referenced by this commit.

#include "support/codegen_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
std::string stdoutOf(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did not run>");
}
// runAotChecked returns ran=false on any sanitizer fire (ASan use-after-free /
// LSan leak); mapping that to a sentinel means a single EXPECT_EQ pins BOTH
// memory-cleanliness AND the computed value — the right oracle for the
// fresh-owned-producer disposal rows below (adversarial review: the #1881
// disposal this fix adds had no gtest coverage, only hand-run probes).
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// A class WITH a user Equals/GetHashCode override keyed on field X, and a
// destructor that counts calls — lets a test pin exact dispose counts (a
// borrowing Set<T> must never dispose a rejected duplicate, §28.11 v0.135).
const char* kBox =
    "module M; class Box "
    "  public static _n: int := 0; "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Box; class C ";
// A class WITHOUT an Equals override — §8.3 reference-identity default,
// matching #3491's own repro (`Ord`, no Equals override).
const char* kPlain =
    "module M; class P "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor end destructor; "
    "end P; class C ";
}  // namespace

// #3491 defect (1), the issue's own repro shape: the SAME reference added
// twice must not grow Count. Pre-fix: Count == 2 (re-appended). Post-fix: 1.
BEZEL_TEST(borrowing_set_class_element_dedup_3491, add_same_reference_does_not_reappend) {
    BEZEL_EXPECT_EQ(run(std::string(kPlain) +
        "public static method Main(): int "
        "  do using var o := new P(3); "
        "    var s := new Set<P>(); "
        "    s.Add(o); s.Add(o); "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 1);
}

// #3491 defect (1), general identity axis: a content-equal but DISTINCT
// instance (user Equals override) must also be rejected as a duplicate —
// the same equality contract emitArrayBackedSetInsert already dispatches
// for OwningSet<T> (#837). Pre-fix: Count == 2. Post-fix: 1.
BEZEL_TEST(borrowing_set_class_element_dedup_3491, add_equal_distinct_instance_does_not_reappend) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var a := new Box(1); "
        "    using var b := new Box(1); "
        "    var s := new Set<Box>(); "
        "    s.Add(a); s.Add(b); "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 1);
}

// Discriminating cell (adversarial review): distinct instances with the SAME
// field value but NO Equals override must NOT dedup — §8.3's reference-
// identity default applies, so `a` and `b` are different elements and BOTH
// are kept (Count == 2). Without this row, the two equality-axis tests above
// (same-reference dedups; content-equal-WITH-override dedups) leave an
// accidental structural/field comparison unpinned — a regression that
// compared elements by field value instead of dispatching §8.3's contract
// would wrongly collapse this pair to Count == 1 and nothing here would
// catch it.
BEZEL_TEST(borrowing_set_class_element_dedup_3491, add_distinct_no_override_reference_both_kept) {
    BEZEL_EXPECT_EQ(run(std::string(kPlain) +
        "public static method Main(): int "
        "  do using var a := new P(1); "
        "    using var b := new P(1); "
        "    var s := new Set<P>(); "
        "    s.Add(a); s.Add(b); "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// #3491 defect (2): TryAdd on an ABSENT element must return true. Pre-fix
// this local degraded to codegen's #2434 zero-init fallback (always false),
// so this is the row that actually falsifies the pre-fix compiler — a
// present-case-only assertion of `false` would pass vacuously under the
// same degrade.
BEZEL_TEST(borrowing_set_class_element_dedup_3491, tryadd_absent_returns_true) {
    BEZEL_EXPECT_TRUE(run(std::string(kPlain) +
        "public static method Main(): int "
        "  do using var o := new P(7); "
        "    var s := new Set<P>(); "
        "    var t := s.TryAdd(o); "
        "    if t then return 1; end; return 0; end; "
        "  return 0; end Main; end C;") == 1);
}

// #3491 defect (2), the present case: TryAdd on an already-present element
// (same reference) must return false AND must not grow Count.
BEZEL_TEST(borrowing_set_class_element_dedup_3491, tryadd_present_returns_false_no_reappend) {
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kPlain) +
        "public static method Main(): int "
        "  do using var o := new P(9); "
        "    var s := new Set<P>(); "
        "    s.Add(o); "
        "    var t := s.TryAdd(o); "
        "    Console.WriteLine($\"t={t} count={s.Count}\"); "
        "    return 0; end; "
        "  return 0; end Main; end C;"),
        "t=False count=1\n");
}

// §28.11 v0.135 / §4.10: a borrowing Set<T> must NEVER dispose a rejected
// duplicate (unlike OwningSet<T>, whose owning column does). Two distinct,
// content-equal `using var` Boxes: the second Add rejects b as a duplicate
// but b is NOT disposed by the set — it is disposed normally, once, at its
// own `using var` scope end. Exactly 2 destructor calls total (a and b),
// never 3 (which would mean the set double-disposed the rejected b).
BEZEL_TEST(borrowing_set_class_element_dedup_3491, add_duplicate_not_disposed_by_borrowing_set) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var a := new Box(1); "
        "    using var b := new Box(1); "
        "    var s := new Set<Box>(); "
        "    s.Add(a); s.Add(b); "
        "  end; "
        "  return Box._n; end Main; end C;"), 2);
}

// Adversarial-review rows (pre-v0.346): these pinned the #1881 disposal of a
// fresh `new Box(1)` passed straight to a borrowing `Set<Box>.Add` — once on
// the ACCEPTED path (empty set) and once on the dedup-REJECTED path (a live
// content-equal comparand `a`). Since v0.346 (proposal 0207, #3047 lint A)
// that argument is §15.4's fresh-store COMPILE ERROR (v0.355: "a fresh unbound
// owned temporary of class type … in a single-element store position of a
// borrowing-family collection is a compile error at the store site"; `Set` is
// in the family), whichever path the dedup scan would take — so both rows now
// pin the rejection. The K8.1 boundary disposal they observed is re-pinned at
// a user-declared plain-parameter sink in
// borrowing_collection_add_temp_disposal_1881_test.cpp (`plain_param_sink_*`).
// // per §15.4, §28.11 clause 4
static void expectFreshStoreError3491(const std::string& prog) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "§15.4 fresh-store error"));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "`.Add(...)` store"));
}

BEZEL_TEST(borrowing_set_class_element_dedup_3491, add_fresh_producer_when_accepted_is_fresh_store_error) {
    expectFreshStoreError3491(std::string(kBox) +
        "public static method Main(): int "
        "  do var s := new Set<Box>(); "
        "    s.Add(new Box(1)); "
        "    return Box._n; end; "
        "  return 0; end Main; end C;");
}

BEZEL_TEST(borrowing_set_class_element_dedup_3491, add_fresh_producer_when_rejected_is_fresh_store_error) {
    expectFreshStoreError3491(std::string(kBox) +
        "public static method Main(): int "
        "  do using var a := new Box(1); "
        "    var s := new Set<Box>(); "
        "    s.Add(a); "
        "    s.Add(new Box(1)); "
        "    return Box._n; end; "
        "  return 0; end Main; end C;");
}

// Control row (task requirement): a VALUE-typed element Set<int> is
// dict-backed (genericBackingType admits hashable value types) and is
// UNTOUCHED by this fix, which only widens the array-backed class-element
// gate (isBorrowingSetRecv && arrElemIsClass). Add/TryAdd dedup behavior on
// Set<int> must be exactly what it was before: after `Add(1); Add(1);` the
// set is {1} (Count 1, the second Add silently no-ops on the duplicate);
// `TryAdd(1)` (present) returns false and leaves it {1}; `TryAdd(2)` (absent)
// returns true and inserts a second element, so the FINAL Count printed
// below is 2 (from {1} plus the successful TryAdd(2), not from the earlier
// Add(1) pair, which alone would leave Count at 1).
BEZEL_TEST(borrowing_set_class_element_dedup_3491, control_value_element_set_unaffected) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; class C "
        "public static method Main(): int "
        "  do var s := new Set<int>(); "
        "    s.Add(1); s.Add(1); "
        "    var tv := s.TryAdd(1); "
        "    var tv2 := s.TryAdd(2); "
        "    Console.WriteLine($\"count={s.Count} tv={tv} tv2={tv2}\"); "
        "    return 0; end; "
        "  return 0; end Main; end C;"),
        "count=2 tv=False tv2=True\n");
}
