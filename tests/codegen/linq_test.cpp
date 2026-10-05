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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// --- Any / All (eager quantifiers) ---

BEZEL_TEST(linq, any_all_int) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return int(a.Any(lambda(x) => x > 2) and a.All(lambda(x) => x > 0) and "
        "             not a.Any(lambda(x) => x > 10)); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, any_all_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"hello\"); a.Add(\"hi\"); "
        "  return int(a.Any(lambda(s) => s.Length > 4) and a.All(lambda(s) => s.Length > 0)); "
        "end Main; end C;"), 1);
}

// --- Count (eager) / Where (lazy) ---

BEZEL_TEST(linq, count_where) {
    // Count(pred) is eager (int); Where is lazy — materialize with ToArray().
    // fix #3342 (§26.5 rule 1 / stdlib §4.4, v0.277): `Array<T>` declares
    // `Count` as a 0-arg property, so an `arr.Count(pred)` call is now a
    // compile error — the §5 LINQ `Count(pred)` extension is unreachable on
    // a receiver that declares `Count` by name. The composed spelling
    // `arr.Where(pred).Count()` (Where is shadowed nowhere) reaches the
    // operator pipeline.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var evens: Array<int> := a.Where(lambda(x) => x mod 2 = 0).ToArray(); "
        "  return int(a.Where(lambda(x) => x mod 2 = 0).Count() = 2 and evens.Count = 2 "
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
        "  for var x in a.Where(lambda(x) => x mod 2 = 0) do s := s + x; end; "
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
        "  var ss := a.Sum(lambda(x: int) => x * x); "
        "  return int(s = 14 and mn = 1 and mx = 5 and ss = 52); "
        "end Main; end C;"), 1);
}

// stdlib §5.1 / §8.4 (#1194): no-selector Min()/Max() over an Array<T> whose
// element T is a PTR-typed IComparable<T> — a user class implementing
// IComparable<T> (§14.2). Pre-fix the lowering truncated the element pointer to
// i32 (`trunc ptr … to i32` → malformed IR) and sema mistyped the result as int
// (so `.amount` folded to 0). The comparison must dispatch through T.CompareTo,
// and the result is typed as T so member access on it resolves.
BEZEL_TEST(linq, max_class_icomparable_1194) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class Money: IComparable<Money> "
        "  public amount: int := 0; "
        "  public constructor(a: int) amount := a; end constructor; "
        "  public method CompareTo(other: Money): int => amount - other.amount; "
        "end Money; "
        // §15.4/§28.11: a borrowing collection holds a borrow of a live local,
        // not a fresh temp. The three Money are bound to `using var` locals
        // declared before the borrowing Array<Money> and added as borrows, so
        // Max()/Min() (which dispatch through Money.CompareTo and return a borrow
        // of the receiver's element, §28.3) read live Money. The original added
        // fresh `new Money(..)` temporaries, disposed post-#1881 at each Add's end
        // — CompareTo then dereferenced freed Money (heap-use-after-free). The
        // dispatch intent and the `mx.amount = 9 and mn.amount = 3` are preserved.
        "class C public static method Main(): int "
        "  using var m0 := new Money(3); using var m1 := new Money(9); "
        "  using var m2 := new Money(5); "
        "  using var a := new Array<Money>(); "
        "  a.Add(m0); a.Add(m1); a.Add(m2); "   // borrows of live locals
        "  var mx := a.Max(); var mn := a.Min(); "
        "  return int(mx.amount = 9 and mn.amount = 3); "
        "end Main; end C;"), 1);
}

// stdlib §5.1 / §9.6 (#1194): no-selector Min()/Max() over an Array<string>
// dispatches through ordinal string comparison (strcmp on the data pointers,
// mirroring Sort()) and returns the lexicographic extreme — not the truncated
// pointer the i32 path produced. Max of {pear, apple, banana} = "pear";
// Min = "apple".
BEZEL_TEST(linq, max_string_1194) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"pear\"); a.Add(\"apple\"); a.Add(\"banana\"); "
        "  var mx := a.Max(); var mn := a.Min(); "
        "  return int(mx = \"pear\" and mn = \"apple\"); "
        "end Main; end C;"), 1);
}

// stdlib §5.1 / §14.2 (#1198): no-selector Min()/Max() over a class element
// that does NOT implement IComparable<T> must be rejected at sema with a
// clear diagnostic — not silently mistyped and then IR-failed in codegen.
// Anti-over-reject: a class that DOES implement IComparable<T> is still
// accepted (covered by max_class_icomparable_1194 above).
BEZEL_TEST(linq, min_max_non_icomparable_class_rejected_1198) {
    // NotComparable has no IComparable<T> base — both Min() and Max()
    // must fail with errors and the program must not run.
    auto r_max = runAot(
        "module M; import System.Collections.Generic; "
        "class NotComparable "
        "  public val: int := 0; "
        "  public constructor(v: int) val := v; end constructor; "
        "end NotComparable; "
        "class C public static method Main(): int "
        "  using var a := new Array<NotComparable>(); "
        "  a.Add(new NotComparable(1)); "
        "  var mx := a.Max(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r_max.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r_max.ran);
    // Confirm the diagnostic cites IComparable.
    bool found_max = false;
    for (const auto& d : r_max.diagnostics.diagnostics())
        if (d.message.find("IComparable") != std::string::npos)
            found_max = true;
    BEZEL_EXPECT_TRUE(found_max);

    auto r_min = runAot(
        "module M; import System.Collections.Generic; "
        "class NotComparable "
        "  public val: int := 0; "
        "  public constructor(v: int) val := v; end constructor; "
        "end NotComparable; "
        "class C public static method Main(): int "
        "  using var a := new Array<NotComparable>(); "
        "  a.Add(new NotComparable(1)); "
        "  var mn := a.Min(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r_min.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r_min.ran);
    bool found_min = false;
    for (const auto& d : r_min.diagnostics.diagnostics())
        if (d.message.find("IComparable") != std::string::npos)
            found_min = true;
    BEZEL_EXPECT_TRUE(found_min);
}

// stdlib §5.1 / §14.2 (#1198): anti-over-reject — a class element with a
// selector lambda is accepted regardless of IComparable (the selector returns
// a concrete numeric T, satisfying the `where U: IComparable<U>` form).
// §15.4/§28.11: a borrowing collection holds a borrow of a live local, not a
// fresh temp. The three Box are bound to `using var` locals declared before the
// borrowing Array<Box> and added as borrows, so the selector `b.val` reads live
// Box during Max()/Min(). The original added fresh `new Box(..)` temporaries,
// disposed post-#1881 at each Add's end — the selector then dereferenced freed
// Box (heap-use-after-free). The anti-over-reject intent and the `mx = 9 and
// mn = 3` are preserved.
BEZEL_TEST(linq, min_max_selector_non_icomparable_accepted_1198) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class Box "
        "  public val: int := 0; "
        "  public constructor(v: int) val := v; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b0 := new Box(3); using var b1 := new Box(9); "
        "  using var b2 := new Box(5); "
        "  using var a := new Array<Box>(); "
        "  a.Add(b0); a.Add(b1); a.Add(b2); "   // borrows of live locals
        "  var mx := a.Max(lambda(b) => b.val); "
        "  var mn := a.Min(lambda(b) => b.val); "
        "  return int(mx = 9 and mn = 3); "
        "end Main; end C;"), 1);
}

// fix #1901 / stdlib §5: the selector overload
// `Min<U>(selector: (T) -> U): U` / `Max<U>(sel): U` types its RESULT as `U`
// — the selector's inferred return type — NOT the element type or `int`. Here
// the element is `int` but the selector projects to `double` (`x + 0.5`), so
// U = double and the maximum must be the *double* 9.5, not the truncated int 9.
// Before the fix the result typed as `int`, so `var mx: double := a.Max(...)`
// silently truncated 9.5 -> 9.0 (9 not in (9.4, 9.6)) and this returned 0.
BEZEL_TEST(linq, min_max_selector_double_result_type_1901) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(9); a.Add(5); "
        "  var mx: double := a.Max(lambda(x) => x + 0.5); "   // U = double -> 9.5
        "  var mn: double := a.Min(lambda(x) => x + 0.5); "   // U = double -> 1.5
        "  return int(mx > 9.4 and mx < 9.6 and mn > 1.4 and mn < 1.6); "
        "end Main; end C;"), 1);
}

// fix #1901 / stdlib §5 / §28.3 (v0.194): the selector overload projecting to a
// CLASS `U` — `Max<U>((o) => o): U` where U is the (owned) element type. The
// result types as the class (not int), so member access on it resolves AND the
// §28.3 receiver-sourced-borrow carve tracks it as a borrow of the receiver
// (the caller takes no disposal obligation; the OwningArray disposes each Box
// exactly once). Before the fix the result typed as `int`, so `.v` below could
// not resolve / the value was mis-dispatched.
// Per spec §22.10 (v0.307, line 1861) post-#4868 5458a4e1: the class's
// primary-ctor parameter `v` is NOT auto-promoted to a public property. The
// canonical §22.10 idiom (docs/Bezel_Language_Spec.md:2042-2057) requires an
// explicit `public readonly property V: int := v;` so `top.v` reads out-of-class
// are legal (now as `top.V`). The migration preserves the #1901 `Max()` selector
// intent — selector returns the class element (`U = Box`), member access on
// the result resolves, and the observable value 9 (the max element's `V`) is
// unchanged.
BEZEL_TEST(linq, min_max_selector_class_result_type_1901) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System; "
        "class Box(v: int): IComparable<Box> "
        "  public readonly property V: int := v; "
        "  public method CompareTo(other: Box): int => "
        "    (if v > other.v then 1 else (if v < other.v then -1 else 0)); "
        "end Box; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Box>(); "
        "  a.Add(new Box(3)); a.Add(new Box(9)); a.Add(new Box(5)); "
        "  var top := a.Max(lambda(o) => o); "   // U = Box (borrow of receiver element)
        "  return top.V; "                 // resolves only if result types as Box
        "end Main; end C;"), 9);
}

// --- First (eager element access) ---

BEZEL_TEST(linq, first) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(5); a.Add(3); a.Add(7); "
        "  return a.First(lambda(x) => x > 4); "
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
        "  return int(t.Count = 3 and t.Get(2) = 3 and "
        "             s.Count = 3 and s.Get(0) = 3 and "
        "             m.Count = 3 and m.Get(0) = 2); "
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
        "  using var asc: Array<int> := a.OrderBy(lambda(x: int) => x).ToArray(); "
        "  using var desc: Array<int> := a.OrderByDescending(lambda(x: int) => x).ToArray(); "
        "  return int(asc.Get(0) = 1 and asc.Get(3) = 8 and "
        "             desc.Get(0) = 8 and desc.Get(3) = 1); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, orderby_string_by_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"cherry\"); a.Add(\"a\"); a.Add(\"bb\"); "
        "  using var sorted: Array<string> := a.OrderBy(lambda(s) => s.Length).ToArray(); "
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
        "  return d.Count; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(linq, distinct_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"apple\"); a.Add(\"banana\"); a.Add(\"apple\"); "
        "  using var d: Array<string> := a.Distinct().ToArray(); "
        "  return d.Count; "
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
        "    a.Where(lambda(x) => x mod 2 = 0).OrderBy(lambda(x: int) => x).Skip(1).Take(3).ToArray(); "
        "  return int(r.Count = 3 and r.Get(0) = 4 and r.Get(2) = 8); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linq, chaining_deferred_foreach) {
    // The same chain consumed by a for-loop without materializing.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  for var i in 1..10 do a.Add(i); end; "
        "  var s := 0; var n := 0; "
        "  for var x in a.Where(lambda(x) => x mod 2 = 0).OrderBy(lambda(x: int) => x).Skip(1).Take(3) do "
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
        "    using var doubled: Array<int> := a.Select(lambda(x: int) => x * 2).ToArray(); "
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
// fix #3342 (§26.5 rule 1 / stdlib §4.4, v0.277): `Array<T>` declares `Count`
// as a 0-arg property, so the direct `arr.Count(pred)` shape is a compile
// error — the §5 LINQ operator is unreachable by name. The composed spelling
// `arr.Where(pred).Count()` (Where is shadowed nowhere in this document)
// reaches the operator pipeline.
BEZEL_TEST(linq, count_predicate_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 0)); a.Add(new Point(3, 9)); a.Add(new Point(5, 7)); "
        "  return a.Where(lambda(p) => p.x = 5).Count(); "
        "end Main; end C;"), 2);
}

// §5 Any / All over records: quantifiers read record fields.
BEZEL_TEST(linq, any_all_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(5, 0)); a.Add(new Point(7, 0)); "
        "  return int(a.Any(lambda(p) => p.x = 5) and a.All(lambda(p) => p.y = 0) and "
        "             not a.Any(lambda(p) => p.x = 99)); "
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
        "  var p: Point := a.First(lambda(q) => q.x = 2); "
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
        "  using var w: Array<Point> := a.Where(lambda(p) => p.x = 5).ToArray(); "
        "  return int(w.Count = 2 and w.Get(0).y = 1 and w.Get(1).y = 3); "
        "end Main; end C;"), 1);
}

// §5 Select (lazy) over records: project a record to one of its fields.
BEZEL_TEST(linq, select_record_field_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(4, 0)); a.Add(new Point(6, 0)); "
        "  using var xs: Array<int> := a.Select(lambda(p) => p.x).ToArray(); "
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
        "  using var s: Array<Point> := a.OrderBy(lambda(p) => p.x).ToArray(); "
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
        "  return d.Count; "
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
        "  return d.Count; "
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
        "  for var p in a.Where(lambda(q) => q.x mod 2 = 0) do "
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
        "  var p := a.First(lambda(q) => q.x = 2); "
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
        "  return a.First(lambda(q) => q.x = 2).y; "
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

// ===========================================================================
// FINDING [5] / stdlib §5: an EAGER terminal LINQ operator chained DIRECTLY
// onto a LAZY LINQ result (Select/Where/Concat/TakeWhile/SkipWhile/Zip/...)
// must enumerate the lazy sequence — not treat it as empty. Before the fix
// these returned 0/false/garbage because the terminal lowering indexed the
// lazy enumerator object as if it were a concrete bz_array_t. The fix
// materializes the lazy pipeline (bz_lazy_to_array) before the array fold.
// ===========================================================================

// .Select(...).Count() — Count over a lazy projection.
BEZEL_TEST(linq, lazy_select_then_count_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.Select(lambda(x: int) => x * 2).Count(); "  // 3 elements
        "end Main; end C;"), 3);
}

// .Where(...).Count() — Count over a lazy filter.
BEZEL_TEST(linq, lazy_where_then_count_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.Where(lambda(x: int) => x > 1).Count(); "  // {2,3} -> 2
        "end Main; end C;"), 2);
}

// .Where(...).Any() — Any over a lazy filter is true when a match exists.
BEZEL_TEST(linq, lazy_where_then_any_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return int(a.Where(lambda(x: int) => x > 1).Any()); "  // true
        "end Main; end C;"), 1);
}

// .Where(...).Any() — false when nothing matches the lazy filter.
BEZEL_TEST(linq, lazy_where_then_any_empty_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return int(a.Where(lambda(x: int) => x > 99).Any()); "  // false
        "end Main; end C;"), 0);
}

// .Where(...).First() — First over a lazy filter returns the first match.
BEZEL_TEST(linq, lazy_where_then_first_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.Where(lambda(x: int) => x > 1).First(); "  // first match = 2
        "end Main; end C;"), 2);
}

// .Select(...).Aggregate(seed, fold) — left fold over a lazy projection.
BEZEL_TEST(linq, lazy_select_then_aggregate_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.Select(lambda(x: int) => x * 2).Aggregate(0, lambda(acc: int, x: int) => acc + x); "
        "end Main; end C;"), 12);  // 2+4+6
}

// .Select(...).Sum() — Sum over a lazy projection (no selector overload).
BEZEL_TEST(linq, lazy_select_then_sum_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.Select(lambda(x: int) => x * 2).Sum(); "  // 2+4+6 = 12
        "end Main; end C;"), 12);
}

// .TakeWhile(...).Aggregate(...) — terminal over a lazy slice.
BEZEL_TEST(linq, lazy_takewhile_then_aggregate_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(5); a.Add(3); "
        "  return a.TakeWhile(lambda(x: int) => x < 5).Aggregate(0, lambda(acc: int, x: int) => acc + x); "
        "end Main; end C;"), 3);  // {1,2} -> 1+2 = 3 (stops at 5)
}

// a.Zip(b, *).Aggregate(...) — Zip is lazy; Aggregate folds the combined seq.
BEZEL_TEST(linq, lazy_zip_then_aggregate_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var b := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  b.Add(10); b.Add(20); b.Add(30); "
        "  return a.Zip(b, lambda(x: int, y: int) => x * y)"
        "          .Aggregate(0, lambda(acc: int, p: int) => acc + p); "
        "end Main; end C;"), 140);  // 10 + 40 + 90 = 140
}

// Chained lazy then terminal: Where().Select().Count().
BEZEL_TEST(linq, lazy_where_select_then_count_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return a.Where(lambda(x: int) => x mod 2 = 0).Select(lambda(x: int) => x * 10).Count(); "
        "end Main; end C;"), 2);  // {2,4} -> {20,40} -> 2
}

// .Concat(...).Sum() — terminal over a lazy concatenation.
BEZEL_TEST(linq, lazy_concat_then_sum_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var b := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  b.Add(3); b.Add(4); "
        "  return a.Concat(b).Sum(); "  // 1+2+3+4 = 10
        "end Main; end C;"), 10);
}

// .Where(...).All(pred) — All over a lazy filter (every survivor satisfies it).
BEZEL_TEST(linq, lazy_where_then_all_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return int(a.Where(lambda(x: int) => x > 1).All(lambda(x: int) => x > 1)); "  // true
        "end Main; end C;"), 1);
}

// .Where(...).Count(pred) — Count(pred) over a lazy filter.
BEZEL_TEST(linq, lazy_where_then_count_pred_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  return a.Where(lambda(x: int) => x > 1).Count(lambda(x: int) => x mod 2 = 0); "  // {2,3,4} even -> {2,4} -> 2
        "end Main; end C;"), 2);
}

// CONTROL: terminal on a concrete array is unchanged (Aggregate over Array).
BEZEL_TEST(linq, control_concrete_aggregate_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  return a.Aggregate(0, lambda(acc: int, x: int) => acc + x); "  // 6
        "end Main; end C;"), 6);
}

// CONTROL: lazy materialized to array first (ToArray) is unchanged.
BEZEL_TEST(linq, control_lazy_toarray_length_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var m: Array<int> := a.Select(lambda(x: int) => x * 2).ToArray(); "
        "  return m.Count; "  // 3
        "end Main; end C;"), 3);
}

// CONTROL: for-loop over a lazy pipeline is unchanged.
BEZEL_TEST(linq, control_for_over_lazy_spec_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var s := 0; "
        "  for var x in a.Select(lambda(x: int) => x * 2) do s := s + x; end; "
        "  return s; "  // 2+4+6 = 12
        "end Main; end C;"), 12);
}

// --- #1182: string.Join(sep, items: IEnumerable<string>) over a LAZY query ---
// stdlib §9.6: Join's second argument is an IEnumerable<string>, not just a
// concrete Array<string>. A lazy LINQ query (.Where/.Select) returns a lazy
// IEnumerable; the Join lowering used to read it as a bz_array_t* backing and
// dereferenced garbage → SIGSEGV. It now materializes any IEnumerable<string>
// uniformly (the shared lazy-enumerator path) before joining.
BEZEL_TEST(linq, string_join_lazy_query_arg_1182) {
    // Where keeps both -> "ab,cd" (length 5). Was a SIGSEGV before the fix.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs.Where(lambda(x: string) => x.Length > 0)); "
        "  return s.Length; "  // "ab,cd" -> 5
        "end Main; end C;"), 5);
}

// Exact content (not just length): "ab,cd" — index 2 is the ',' separator (44),
// index 0 is 'a' (97), index 4 is 'd' (100). 44 + (97-97) + (100-100) = 44.
BEZEL_TEST(linq, string_join_lazy_query_content_1182) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs.Where(lambda(x: string) => x.Length > 0)); "
        "  return int(s[2]) + (int(s[0]) - 97) + (int(s[4]) - 100); "
        "end Main; end C;"), 44);
}

// .Select(...) (a different lazy operator) joins the same way.
BEZEL_TEST(linq, string_join_lazy_select_arg_1182) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs.Select(lambda(x: string) => x)); "
        "  return s.Length; "  // "ab,cd" -> 5
        "end Main; end C;"), 5);
}

// CONTROL: a concrete Array<string> second argument is unchanged.
BEZEL_TEST(linq, string_join_concrete_array_control_1182) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs); "
        "  return s.Length; "  // "ab,cd" -> 5
        "end Main; end C;"), 5);
}

// An empty lazy query joins to the empty string (no separator emitted).
BEZEL_TEST(linq, string_join_lazy_empty_1182) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs.Where(lambda(x: string) => x.Length > 9)); "
        "  return s.Length; "  // "" -> 0
        "end Main; end C;"), 0);
}

// A single-element lazy query joins with NO trailing separator.
BEZEL_TEST(linq, string_join_lazy_single_1182) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs.Where(lambda(x: string) => x = \"ab\")); "
        "  return s.Length; "  // "ab" -> 2 (no trailing ',')
        "end Main; end C;"), 2);
}
