// Spec-derived regression tests for fix #3740 — `SortedSet<T>` for a
// non-hashable, `IComparable<T>`-conforming body RECORD element.
//
// AUTHORITY. stdlib §4.18 (v0.291), "4.18 SortedSet<T>":
//
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//
// declares the SOLE constraint as `T: IComparable<T>` — no hashability
// requirement — and "for `SortedSet<T>` that identity is `CompareTo`-equality,
// and adding an equal element leaves the set unmodified". Language spec §3.6
// ("Derived type properties") defines **hashable** as "every type except (a)
// a record that is neither auto-hashable nor declares user equality...", and
// §9.2 excludes a body record from auto-hashable when it declares an explicit
// FIELD (not an auto-property) that is not `equality skip`ped — so a record
// can satisfy §4.18's sole `IComparable<T>` constraint while failing §3.6's
// hashability test, which is exactly the shape this issue names.
//
// THE TWO DEFECTS (measured on f19de07, pre-fix).
//
//   (A) `new SortedSet<Bad>()` failed with lowerLocalDecl's #2974 degrade —
//       "produced no value ... matches no documented constructor" — because
//       `genericBackingType` admitted a record element ONLY via
//       `recordTypeOfRef`, which requires AUTO-HASHABLE (§9.2) — a strictly
//       narrower test than §4.18's actual `IComparable<T>` requirement.
//
//   (B) `s.Contains(s.First())` over a BORROWED `SortedSet<Bad>` parameter (no
//       construction anywhere in the program) aborted the compiler during LLVM
//       IR verification — "Invalid operand types for ICmp instruction,
//       %N = icmp eq %record.Bad %M, %fst.r" — because with `genericBackingType`
//       returning null, the dict-backed Set arm in `tryLowerCollectionInstanceCall`
//       never claimed the call, and control fell through to the ARRAY-BACKED
//       `Contains`/`First` arms, which read a `bz_dict_t*` receiver as if it
//       were array-backed and compared two `%record.Bad` STRUCT values with a
//       raw `icmp eq` (LLVM rejects ICmp on an aggregate operand).
//
// THE FIX. Three additions, all named/gated to admit EXACTLY this shape and
// nothing wider (see each site's own comment for the full reasoning):
//   * `genericBackingType` (LlvmBackend_Calls3.cpp): a new `else if` branch,
//     gated on `isSet && collTy->headName() == "SortedSet"`, admits a record
//     with a dispatchable `CompareTo` that `recordTypeOfRef` excluded.
//   * `buildRecordCompareToThunk` (new): the record analogue of
//     `buildObjectCompareToThunk` — a direct (non-vtable) `CompareTo` dispatch
//     thunk, since records are not polymorphic (CG29).
//   * `buildSortedObjectKeyIdentity` and `buildSortedKeyComparator` each try
//     `buildRecordCompareToThunk` as a fallback, gated on
//     `!recordTypeOfRef(keyTy)` so an AUTO-HASHABLE record's existing
//     structural §9.1/§9.2 identity is UNCHANGED (a strict widening, not a
//     behavior change for any shape that worked before).
//
// This makes membership, dedup, `for`-in / `.First` / `.Last` ordering, and
// `.Contains` over a borrowed receiver all honor §4.18's `CompareTo`-equality
// for the newly-admitted shape, using the SAME (constant-hash,
// `CompareTo == 0`) identity fix #2773 (B1) gave a reference element.
//
// REVIEW FIX on this file's own first commit (b07588d): `genericBackingType`
// admitted a GENERIC non-auto-hashable record with `CompareTo` at
// ELIGIBILITY while `buildRecordCompareToThunk` declined one at BUILD time —
// the two predicates disagreeing meant the dict table was built with the
// STRUCTURAL hash/eq fallback instead of `CompareTo`-equality, a SILENT
// wrong answer strictly worse than the pre-#3740 crash. Fixed by excluding a
// generic record from `recordHasDispatchableCompareTo` too, so the two
// predicates agree and a generic record declines exactly as before #3740.
// See the "Review fix" section below for the pinning tests.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

std::string out(const std::string& src) { return runAotOut(src); }

// `R` is a class with a destructor (so `Bad` is genuinely NOT auto-hashable
// per §9.2 — `Item` is a plain FIELD declaration, not an auto-property, and
// an explicit field disqualifies auto-hashability unless `equality skip`ped,
// which `Bad` does not do), and `Bad: IComparable<Bad>` orders by `Item.V` —
// a MEANINGFUL comparator (not the issue's literal `return 0;`, which was
// sufficient to reproduce the crash but cannot pin ordering behavior).
const char* PRELUDE =
    "module M; import System; "
    "class R "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor end destructor; "
    "end R; "
    "record Bad: IComparable<Bad> "
    "  public Item: R; "
    "  public constructor(i: R) Item := i; end constructor; "
    "  public method CompareTo(other: Bad): int "
    "    return Item.V - other.Item.V; "
    "  end CompareTo; "
    "end Bad; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ── Finding A: construction ──────────────────────────────────────────────

// per stdlib §4.18 (v0.291), fix #3740: the issue's Finding A repro,
// verbatim in shape — `new SortedSet<Bad>()` now constructs.
BEZEL_TEST(sorted_set_record_element_3740, finding_a_construction_succeeds) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  return 0; ")),
        0);
}

// per stdlib §4.18 (v0.291), fix #3740: construction plus a real `Add` and
// `Count` read — not just "did not crash".
BEZEL_TEST(sorted_set_record_element_3740, construction_then_add_and_count) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  using var r := new R(5); "
        "  s.Add(new Bad(r)); "
        "  return s.Count; ")),
        1);
}

// ── Finding B: `.Contains`/`.First` over a borrowed receiver ─────────────

// per stdlib §4.18 (v0.291), fix #3740: the issue's Finding B repro, called
// with a REAL (non-empty) set — pre-fix this aborted the compiler during IR
// verification before the program could ever run; post-fix it both compiles
// AND returns the spec-correct answer (`First()` is always a member of the
// set it came from).
BEZEL_TEST(sorted_set_record_element_3740, finding_b_contains_over_borrowed_receiver) {
    BEZEL_EXPECT_EQ(run(std::string(PRELUDE) +
        "class C "
        "  public static method CheckSorted(s: SortedSet<Bad>): bool "
        "    return s.Contains(s.First()); "
        "  end CheckSorted; "
        "  public static method Main(): int "
        "    using var s := new SortedSet<Bad>(); "
        "    using var a := new R(3); using var b := new R(1); using var c := new R(2); "
        "    s.Add(new Bad(a)); s.Add(new Bad(b)); s.Add(new Bad(c)); "
        "    return (if C.CheckSorted(s) then 1 else 0); "
        "  end Main; "
        "end C;"),
        1);
}

// ── Ordering (stdlib §4.18's "iteration in ascending order") ────────────

// per stdlib §4.18 (v0.291), fix #3740: `for`-in over a non-hashable RECORD
// element — added out of order (3, 1, 2), the desugared iteration must read
// 1, 2, 3 (`CompareTo`-ordered, not insertion- or hash-bucket-ordered).
BEZEL_TEST(sorted_set_record_element_3740, for_in_orders_ascending) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  using var a := new R(3); using var b := new R(1); using var c := new R(2); "
        "  s.Add(new Bad(a)); s.Add(new Bad(b)); s.Add(new Bad(c)); "
        "  var acc: int := 0; "
        "  for var x in s do acc := acc * 10 + x.Item.V; end; "
        "  Console.WriteLine(acc); "
        "  return 0; ")),
        std::string("123"));
}

// per stdlib §4.18 (v0.291), fix #3740: `.First` (least) / `.Last` (greatest)
// via `buildSortedKeyComparator`'s new record arm.
BEZEL_TEST(sorted_set_record_element_3740, first_and_last) {
    BEZEL_EXPECT_EQ(out(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  using var a := new R(3); using var b := new R(1); using var c := new R(2); "
        "  s.Add(new Bad(a)); s.Add(new Bad(b)); s.Add(new Bad(c)); "
        "  Console.WriteLine(s.First.Item.V); "
        "  Console.WriteLine(s.Last.Item.V); "
        "  return 0; ")),
        std::string("1\n3"));
}

// ── Membership: `CompareTo`-equality, not structural §9.1/§9.2 equality ──
// (stdlib §4.18: "adding an equal element leaves the set unmodified" — and
// `Bad`'s `CompareTo` only compares `Item.V`, so two DIFFERENT `R` instances
// with the same `V` must dedup, exactly as fix #2773 (B1) pinned for a
// reference element's reference-distinct-but-CompareTo-equal instances.)

// per stdlib §4.18 (v0.291), fix #3740: `Add` of a CompareTo-equal (same
// `Item.V`, distinct `R` instance) record leaves the set unmodified.
BEZEL_TEST(sorted_set_record_element_3740, add_compareto_equal_leaves_set_unmodified) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  using var a := new R(5); using var b := new R(5); "
        "  s.Add(new Bad(a)); "
        "  s.Add(new Bad(b)); "  // CompareTo-equal to the first (same V=5)
        "  return s.Count; ")),
        1);
}

// per stdlib §4.18 (v0.291), fix #3740: `TryAdd` reports the duplicate.
BEZEL_TEST(sorted_set_record_element_3740, tryadd_reports_compareto_duplicate) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  using var a := new R(5); using var b := new R(5); "
        "  s.Add(new Bad(a)); "
        "  var added := s.TryAdd(new Bad(b)); "
        "  return (if added then 1 else 0); ")),
        0);
}

// per stdlib §4.18 (v0.291), fix #3740: `Contains` and `Remove` both key on
// `CompareTo`-equality over a LOCAL (non-parameter) receiver too.
BEZEL_TEST(sorted_set_record_element_3740, contains_and_remove_use_compareto_equality) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  using var a := new R(5); "
        "  s.Add(new Bad(a)); "
        "  using var probe := new R(5); "
        "  var found: int := (if s.Contains(new Bad(probe)) then 10 else 0); "
        "  var removed: int := (if s.Remove(new Bad(probe)) then 1 else 0); "
        "  return found + removed * 100 + s.Count; ")),
        /* Contains true (10) + Remove true (100) + Count 0 afterward */ 110);
}

// per stdlib §4.18 (v0.291): DISTINCT (non-CompareTo-equal) records all land.
BEZEL_TEST(sorted_set_record_element_3740, distinct_elements_all_land) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new SortedSet<Bad>(); "
        "  using var a := new R(1); using var b := new R(2); using var c := new R(3); "
        "  s.Add(new Bad(a)); s.Add(new Bad(b)); s.Add(new Bad(c)); "
        "  return s.Count; ")),
        3);
}

// ── Review fix: a GENERIC non-auto-hashable record with CompareTo ───────────
//
// A per-fix-review pass on this file's own first commit (b07588d) found a
// SILENT-WRONG-IDENTITY gap this section pins: `recordHasDispatchableCompareTo`
// (the `genericBackingType` ELIGIBILITY predicate) did not exclude a GENERIC
// record, while `buildRecordCompareToThunk` (the actual comparator BUILDER)
// declined one (monomorphizing a generic record's `CompareTo` is out of
// scope for this fix — see that function's own comment). The two disagreeing
// meant: `genericBackingType` admitted a generic record with `CompareTo`,
// `lowerNew` built the dict table via `buildValueHashTrampoline`/
// `buildValueEqTrampoline` (the STRUCTURAL fallback — `recordTypeOfRef` also
// declines a non-auto-hashable record), and `buildSortedObjectKeyIdentity`
// then found `buildRecordCompareToThunk` null and left that STRUCTURAL pair
// UNCHANGED. Net effect: `SortedSet<BadG<T>>` silently ordered/deduped by
// STRUCTURAL equality instead of `CompareTo`-equality — MEASURED before this
// fix: two `BadG<int>` elements with equal `Item.V` (so `CompareTo` returns
// 0 — §4.18 says "adding an equal element leaves the set unmodified") but
// different `Payload` landed as TWO entries (`Count` == 2, not 1). That is a
// loud-crash-to-quiet-wrong-answer regression relative to pre-#3740 (a
// compiler abort) — strictly worse than what #3740 started with, for this
// shape.
//
// THE FIX: `recordHasDispatchableCompareTo` now excludes a generic record
// too, so the two predicates agree and `genericBackingType` declines a
// generic record exactly as it did before #3740 — construction fails LOUDLY
// with the pre-existing #2974 degrade instead of silently constructing with
// the wrong identity. Monomorphizing a generic record's `CompareTo` (the
// alternative fix) is a self-contained follow-up, not attempted here.
const char* GENERIC_PRELUDE =
    "module M; import System; "
    "class R "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor end destructor; "
    "end R; "
    "record BadG<T>: IComparable<BadG<T>> "
    "  public Item: R; "
    "  public Payload: T; "
    "  public constructor(i: R, p: T) Item := i; Payload := p; end constructor; "
    "  public method CompareTo(other: BadG<T>): int "
    "    return Item.V - other.Item.V; "
    "  end CompareTo; "
    "end BadG; ";

// True when some diagnostic in the compile carries `needle` as a substring.
bool hasDiagnostic(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// per stdlib §4.18 (v0.291), review fix (#3740): `new SortedSet<BadG<int>>()`
// — a GENERIC, non-auto-hashable record with a dispatchable `CompareTo` —
// now DECLINES (does not compile) instead of silently constructing.
BEZEL_TEST(sorted_set_record_element_3740, generic_record_construction_declines_loudly) {
    auto r = runAot(std::string(GENERIC_PRELUDE) +
        "class C public static method Main(): int "
        "  using var s := new SortedSet<BadG<int>>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
}

// per stdlib §4.18 (v0.291), review fix (#3740): the EXACT silent-wrong-
// identity repro this fix closes — two `BadG<int>` elements that are
// `CompareTo`-equal (same `Item.V`) but structurally distinct (different
// `Payload`) must NOT silently land as two entries. Asserted as a compile
// DECLINE (the pre-existing #2974 message), not merely "Count != 2": an
// oracle that cannot observe the effect cannot certify the path, and `r.ran`
// alone would not distinguish "declined" from "some other silent change".
BEZEL_TEST(sorted_set_record_element_3740,
           generic_record_compareto_equal_elements_never_silently_double_count) {
    BEZEL_EXPECT_TRUE(hasDiagnostic(std::string(GENERIC_PRELUDE) +
        "class C public static method Main(): int "
        "  using var s := new SortedSet<BadG<int>>(); "
        "  using var r1 := new R(5); using var r2 := new R(5); "
        "  s.Add(new BadG<int>(r1, 1)); "
        "  s.Add(new BadG<int>(r2, 2)); "
        "  return s.Count; "
        "end Main; end C;",
        "produced no value"));
}
