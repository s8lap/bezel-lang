// Codegen tests for LINQ operations (stdlib §5).
//
// LINQ operators are LAZY (deferred): Where/Select/Skip/Take/Distinct/OrderBy/
// OrderByDescending return an `IEnumerable<T>` that does no work until it is
// enumerated. Only ToArray()/ToSet()/ToDictionary() materialize (eager).
// These tests therefore consume each pipeline either by iterating it in a
// `for` loop or by an explicit `.ToArray()` — never by binding the operator
// result straight into an `Array<T>` (which would assume eager materialization).
// The observable results (sums, counts, element checks) are unchanged from the
// eager version; only the consumption pattern proves the lazy contract.
//
// Eager LINQ members exercised here: Any/All/Count(pred)/Sum/Min/Max/First/
// IndexOf/Contains.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- Any / All (eager quantifiers) ---

BEZEL_TEST(linq, any_all_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return int(a.Any((x) => x > 2) and a.All((x) => x > 0) and "
        "             not a.Any((x) => x > 10)); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, any_all_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"hi\"); "
        "  return int(a.Any((s) => s.Length > 4) and a.All((s) => s.Length > 0)); "
        "end Main; end C;"), 1);
}

// --- Count (eager) / Where (lazy) ---

BEZEL_TEST(linq, count_where) {
    // Count(pred) is eager (int); Where is lazy — materialize with ToArray().
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var evens: Array<int> := a.Where((x) => x mod 2 = 0).ToArray(); "
        "  return int(a.Count((x) => x mod 2 = 0) = 2 and evens.Length = 2 "
        "    and evens.Get(0) = 2 and evens.Get(1) = 4); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, where_deferred_foreach) {
    // A lazy Where pipeline iterated directly in a for-loop (no materialization).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  var s := 0; "
        "  for var x in a.Where((x) => x mod 2 = 0) do s := s + x; end; "
        "  return s; "  // 2 + 4 = 6
        "end Main; end C;"), 6);
}

// --- Sum / Min / Max (eager aggregations) ---

BEZEL_TEST(linq, sum_min_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(4); a.Add(1); a.Add(5); "
        "  var s := a.Sum(); var mn := a.Min(); var mx := a.Max(); "
        "  var ss := a.Sum((x: int) => x * x); "
        "  return int(s = 14 and mn = 1 and mx = 5 and ss = 52); "
        "end Main; end C;"), 1);
}

// --- First (eager element access) ---

BEZEL_TEST(linq, first) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(5); a.Add(3); a.Add(7); "
        "  return a.First((x) => x > 4); "
        "end Main; end C;"), 5);
}

// --- Skip / Take (lazy) ---

BEZEL_TEST(linq, skip_take) {
    // Skip/Take are lazy; materialize each pipeline with ToArray() to observe.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var t: Array<int> := a.Take(3).ToArray(); "
        "  using var s: Array<int> := a.Skip(2).ToArray(); "
        "  using var m: Array<int> := a.Take(4).Skip(1).ToArray(); "
        "  return int(t.Length = 3 and t.Get(2) = 3 and "
        "             s.Length = 3 and s.Get(0) = 3 and "
        "             m.Length = 3 and m.Get(0) = 2); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, take_deferred_foreach) {
    // A lazy Take pipeline iterated directly (deferred end-to-end).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var s := 0; "
        "  for var x in a.Take(2) do s := s + x; end; "
        "  return s; "  // 10 + 20 = 30
        "end Main; end C;"), 30);
}

// --- OrderBy / OrderByDescending (lazy) ---

BEZEL_TEST(linq, orderby_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(2); a.Add(8); a.Add(1); "
        "  using var asc: Array<int> := a.OrderBy((x: int) => x).ToArray(); "
        "  using var desc: Array<int> := a.OrderByDescending((x: int) => x).ToArray(); "
        "  return int(asc.Get(0) = 1 and asc.Get(3) = 8 and "
        "             desc.Get(0) = 8 and desc.Get(3) = 1); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, orderby_string_by_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"cherry\"); a.Add(\"a\"); a.Add(\"bb\"); "
        "  using var sorted: Array<string> := a.OrderBy((s) => s.Length).ToArray(); "
        "  return int(sorted.Get(0) = \"a\" and sorted.Get(2) = \"cherry\"); "
        "end Main; end C;"), 1);
}

// --- Distinct (lazy) ---

BEZEL_TEST(linq, distinct_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(1); a.Add(3); a.Add(2); "
        "  using var d: Array<int> := a.Distinct().ToArray(); "
        "  return d.Length; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(linq, distinct_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"apple\"); a.Add(\"banana\"); a.Add(\"apple\"); "
        "  using var d: Array<string> := a.Distinct().ToArray(); "
        "  return d.Length; "
        "end Main; end C;"), 2);
}

// --- IndexOf / Contains (eager) ---

BEZEL_TEST(linq, indexof_contains) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var i := a.IndexOf(20); var j := a.IndexOf(99); "
        "  return int(i = 1 and j = -1 and a.Contains(10) and not a.Contains(99)); "
        "end Main; end C;"), 1);
}

// --- Chaining (all lazy until ToArray) ---

BEZEL_TEST(linq, chaining) {
    // Where → OrderBy → Skip → Take composes lazily; only ToArray materializes.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  for var i in 1..10 do a.Add(i); end; "
        "  using var r: Array<int> := "
        "    a.Where((x) => x mod 2 = 0).OrderBy((x: int) => x).Skip(1).Take(3).ToArray(); "
        "  return int(r.Length = 3 and r.Get(0) = 4 and r.Get(2) = 8); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, chaining_deferred_foreach) {
    // The same chain consumed by a for-loop without materializing.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  for var i in 1..10 do a.Add(i); end; "
        "  var s := 0; var n := 0; "
        "  for var x in a.Where((x) => x mod 2 = 0).OrderBy((x: int) => x).Skip(1).Take(3) do "
        "    s := s + x; n := n + 1; "
        "  end; "
        "  return int(n = 3 and s = 18); "  // 4 + 6 + 8 = 18
        "end Main; end C;"), 1);
}

// --- Select (lazy projection) ---

BEZEL_TEST(linq, select_projection) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        // Select is lazy; materialize with ToArray() to observe the projection.
        "    using var doubled: Array<int> := a.Select((x: int) => x * 2).ToArray(); "
        "    return int(doubled.Get(0) = 2 and doubled.Get(2) = 6); "
        "  end Main; end C;"), 1);
}

// --- LINQ over a RECORD element sequence (stdlib §5 over §9 records) ----------
// A LINQ lambda's untyped parameter over Array<record> is typed as the element
// record (§5 `(T) -> …`), so its body reads the record's fields (§9.1). The
// trampoline / eager loop must pass the ACTUAL record element to the closure —
// a predicate reading a field must see the field's real value, not zero.

// §5 Count(predicate): the predicate reads a record field. Point(5,0) satisfies
// `p.x = 5`, so Count = 1. (Load-bearing: before the fix the record param was
// untyped, `p.x` did not resolve, and the predicate body collapsed to a no-op.)
BEZEL_TEST(linq, count_predicate_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 0)); a.Add(new Point(3, 9)); a.Add(new Point(5, 7)); "
        "  return a.Count((p) => p.x = 5); "
        "end Main; end C;"), 2);
}

// §5 Any / All over records: quantifiers read record fields.
BEZEL_TEST(linq, any_all_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 0)); a.Add(new Point(7, 0)); "
        "  return int(a.Any((p) => p.x = 5) and a.All((p) => p.y = 0) and "
        "             not a.Any((p) => p.x = 99)); "
        "end Main; end C;"), 1);
}

// §5 First(predicate) over records: the predicate reads a record field and the
// first matching record is returned. The result is bound to an explicitly-typed
// local (the §5 element type Point) so `p.y` resolves; the predicate sees the
// real field values (matches Point(2,20), so p.y = 20 — not zero).
BEZEL_TEST(linq, first_predicate_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 10)); a.Add(new Point(2, 20)); a.Add(new Point(3, 30)); "
        "  var p: Point := a.First((q) => q.x = 2); "
        "  return p.y; "
        "end Main; end C;"), 20);
}

// §5 Where (lazy) over records: filter by a field, materialize, read fields back.
BEZEL_TEST(linq, where_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 1)); a.Add(new Point(2, 2)); a.Add(new Point(5, 3)); "
        "  using var w: Array<Point> := a.Where((p) => p.x = 5).ToArray(); "
        "  return int(w.Length = 2 and w.Get(0).y = 1 and w.Get(1).y = 3); "
        "end Main; end C;"), 1);
}

// §5 Select (lazy) over records: project a record to one of its fields.
BEZEL_TEST(linq, select_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(4, 0)); a.Add(new Point(6, 0)); "
        "  using var xs: Array<int> := a.Select((p) => p.x).ToArray(); "
        "  return int(xs.Get(0) = 4 and xs.Get(1) = 6); "
        "end Main; end C;"), 1);
}

// §5 OrderBy a record sequence by a field key (`(T) -> TKey`, §9.1 field read).
BEZEL_TEST(linq, orderby_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 1)); a.Add(new Point(2, 2)); a.Add(new Point(8, 3)); "
        "  using var s: Array<Point> := a.OrderBy((p) => p.x).ToArray(); "
        "  return int(s.Get(0).x = 2 and s.Get(1).x = 5 and s.Get(2).x = 8 and "
        "             s.Get(0).y = 2); "  // payload travels with the sort
        "end Main; end C;"), 1);
}

// §5 Distinct over records dedups by structural equality (§9.1/§9.2), mirroring
// Set<record>. Two equal Pt records collapse to one. (Load-bearing for D6:
// before the fix, Distinct emitted an ICmp on the record aggregate and aborted
// the compiler.)
BEZEL_TEST(linq, distinct_record_structural_equality_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1)); a.Add(new Pt(2)); a.Add(new Pt(1)); a.Add(new Pt(2)); "
        "  using var d: Array<Pt> := a.Distinct().ToArray(); "
        "  return d.Length; "
        "end Main; end C;"), 2);
}

// §5 Distinct over a multi-field record: equality is field-by-field (§9.1),
// so Point(1,2) and Point(1,3) are distinct but the two Point(1,2) collapse.
BEZEL_TEST(linq, distinct_record_multifield_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 2)); a.Add(new Point(1, 3)); a.Add(new Point(1, 2)); "
        "  using var d: Array<Point> := a.Distinct().ToArray(); "
        "  return d.Length; "
        "end Main; end C;"), 2);
}

// §5 a lazy pipeline over records consumed by a for-loop (no materialization):
// Where filters by a field; the loop reads fields of each yielded record.
BEZEL_TEST(linq, lazy_record_pipeline_foreach_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 10)); a.Add(new Point(2, 20)); "
        "  a.Add(new Point(3, 30)); a.Add(new Point(4, 40)); "
        "  var s := 0; var n := 0; "
        "  for var p in a.Where((q) => q.x mod 2 = 0) do "
        "    s := s + p.y; n := n + 1; "
        "  end; "
        "  return int(n = 2 and s = 60); "  // Point(2,20).y + Point(4,40).y
        "end Main; end C;"), 1);
}

// --- #366: element-returning LINQ ops type their result as the element T -----
// stdlib §5: First/Last/Single/ElementAt each return T (the source element
// type). Previously these were typed only over a lazy IEnumerable receiver, not
// over an eager Array<T> receiver, so `var p := a.First(pred)` left `p` untyped
// and a later `p.field` was dropped (workaround: an explicit `var p: T`). These
// tests drop the explicit annotation; they are load-bearing — without the §5
// result typing the inferred local is untyped and the field read collapses to 0.

// §5 First(predicate) over records, RESULT TYPE INFERRED (no `: Point`): the
// inferred local p is the element type Point, so `p.y` resolves. Point(2,20)
// matches `q.x = 2`, so p.y = 20.
BEZEL_TEST(linq, first_predicate_record_inferred_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 10)); a.Add(new Point(2, 20)); a.Add(new Point(3, 30)); "
        "  var p := a.First((q) => q.x = 2); "
        "  return p.y; "
        "end Main; end C;"), 20);
}

// §5 member access directly on a record-returning call result: `a.First(pred).y`.
// The call yields a by-value record struct; reading `.y` spills it to a slot and
// field-accesses. Point(2,20) matches, so the result is 20.
BEZEL_TEST(linq, first_predicate_record_member_on_call_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 10)); a.Add(new Point(2, 20)); a.Add(new Point(3, 30)); "
        "  return a.First((q) => q.x = 2).y; "
        "end Main; end C;"), 20);
}

// §5 First() (no predicate) over records, result type inferred → first element.
BEZEL_TEST(linq, first_noarg_record_inferred_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(7, 11)); a.Add(new Point(2, 22)); "
        "  var p := a.First(); "
        "  return p.y; "  // first element Point(7,11).y
        "end Main; end C;"), 11);
}

// §5 Last() over records, result type inferred → last element.
BEZEL_TEST(linq, last_record_inferred_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 10)); a.Add(new Point(2, 20)); a.Add(new Point(3, 30)); "
        "  var p := a.Last(); "
        "  return p.y; "  // last element Point(3,30).y
        "end Main; end C;"), 30);
}

// §5 Single() over a one-element record array, result type inferred.
BEZEL_TEST(linq, single_record_inferred_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(9, 99)); "
        "  var p := a.Single(); "
        "  return p.y; "  // the only element Point(9,99).y
        "end Main; end C;"), 99);
}

// §5 ElementAt(i) over records, result type inferred; member access on the call.
BEZEL_TEST(linq, element_at_record_member_on_call_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 10)); a.Add(new Point(2, 20)); a.Add(new Point(3, 30)); "
        "  return a.ElementAt(1).y; "  // Point(2,20).y
        "end Main; end C;"), 20);
}

// §5 int-element First() control: result inferred as int, returned directly.
// (No member access — proves the element-type inference is correct for a
// primitive element, not only for records.)
BEZEL_TEST(linq, first_noarg_int_inferred_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(42); a.Add(7); a.Add(3); "
        "  var n := a.First(); "
        "  return n; "
        "end Main; end C;"), 42);
}
