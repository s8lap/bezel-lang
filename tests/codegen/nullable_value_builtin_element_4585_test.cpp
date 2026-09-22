// Codegen — issue #4585: `Array<Guid?>` stored a corrupted payload; the
// `hasValue` tag survived, the sixteen `Guid` bytes did not (§7.2/§8.4,
// stdlib §2.5/§2.6/§4.4/§4.6, v0.307).
//
// STATUS. **Already fixed when this file was written** — by #4593, whose root
// cause was wider than either issue described: `llvm::Module` is born with the
// EMPTY DataLayout, in which `i64` has a 4-byte ABI alignment against x86-64's
// 8, and `setDataLayout(tm->createDataLayout())` ran only in `emitObjectFile`,
// AFTER lowering. Every `getTypeAllocSize` the emitter performed therefore
// answered a different question from the one the backend answers for
// `alloca T`, and those counts ARE the memcpy lengths and element strides
// handed to the runtime — `Guid?` (`{i8,{i64,i64}}`) measuring 20 where the
// host says 24. §7.2 puts the tag at offset 0, which is exactly why the tag and
// the `null` slot always survived and only the present payload was truncated:
// this issue's reported symptom, in full.
//
// WHY THIS FILE EXISTS ANYWAY. #4593's own regression test
// (`elem_stride_4593_test.cpp`) pins the STRIDE — `Array<Guid?>` /
// `Array<DateTime?>` / `Array<Range<int>?>` round-trip, the `.Add` path, one
// `LinkedList<Guid?>` membership row, and the tag/`null` control. It does not
// pin #4585's own three measured lines (`a[0]!.ToString()` against the source's
// rendering, `a[0] = q` for a nullable local `q`, and `a.Contains(q)` on the
// ARRAY-backed family), and it covers none of the sibling grid this issue asked
// for. A fixed-but-unpinned defect is one revert away from returning, and this
// family has now produced three issues in three weeks (#4579 `LinkedList<T?>`,
// #4585 `Array<Guid?>`, #4593 the shared cause), so the grid is worth owning.
//
// THE GRID, and why these five element types. The discriminator is `i64`, not
// nullability and not `Guid`: x86-64 overrides the LLVM defaults only for
// `i64`, `i128` and `f80`. So `Guid?` (two `i64`), `DateTime?` (`Ticks: long`)
// and `TimeSpan?` (`Ticks: long`) were all short-strided, while `DateOnly?` and
// `TimeOnly?` — whose components are `int`s (stdlib §2.5) — measure the same
// under both layouts and were always correct. The last two are therefore
// CONTROLS: they must pass on both sides of any fix in this family, and a
// "repair" that widened every nullable stride unconditionally would break them.
// Each element type is exercised through BOTH container families, because the
// stride is computed per backing (`bz_array_alloc` vs `bz_llist_alloc`) and
// #4579 was precisely one of those two being wrong on its own.
//
// ORACLES. Every row asserts a VALUE and returns a DISTINCT non-1 exit code per
// failed clause, so a failure names the clause rather than the row. The read-back
// assertions compare against a re-derived value, never against the stored local,
// so a slot that merely aliased its source would not pass.
//
// FAIL-WITHOUT: proved mechanically by reverse-applying #4593's `src/` hunks
// (`git show a99fb6b9 -- src/ | git apply -R`) and rebuilding the test binary —
// see the commit body for the per-row results.
//
// spec §7.2 / §8.4, stdlib §2.5 / §2.6 / §4.4 / §4.6, v0.307.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's own repro, line for line ───────────────────────────────────

// #4585's measured program: `a[0] = null` answered False (correct — the tag
// survived), while `a[0]!.ToString() = g1.ToString()`, `a[0] = q` and
// `a.Contains(q)` all answered False (wrong — the payload did not). All four
// are asserted here, in that order, on the `.Add` insertion path the issue used.
BEZEL_TEST(nullable_value_builtin_element_4585, array_guid_add_readback_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\"); "
        "  var a: Array<Guid?> := []; "
        "  a.Add(g1); a.Add(null); "
        "  if a[0] = null then return 2; end; "
        "  if a[0]!.ToString() <> g1.ToString() then return 3; end; "
        "  var q: Guid? := g1; "
        "  if a[0] <> q then return 4; end; "
        "  if not a.Contains(q) then return 5; end; "
        "  if a[1] <> null then return 6; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// The same four assertions reached through the §15.4 collection literal rather
// than `.Add` — the issue states it measured both, and the two insertion paths
// compute their stride independently.
BEZEL_TEST(nullable_value_builtin_element_4585, array_guid_literal_readback_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.Parse(\"0123abcd-4567-89ef-fedc-ba9876543210\"); "
        "  var a: Array<Guid?> := [g1, null]; "
        "  if a[0] = null then return 2; end; "
        "  if a[0]!.ToString() <> g1.ToString() then return 3; end; "
        "  var q: Guid? := g1; "
        "  if a[0] <> q then return 4; end; "
        "  if not a.Contains(q) then return 5; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// The ABSENT control for the membership half: a DIFFERENT Guid must not be
// found. Without it, a stride repair that made every nullable comparison
// constant-true would pass every row above.
BEZEL_TEST(nullable_value_builtin_element_4585, array_guid_contains_absent_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\"); "
        "  var g2: Guid := Guid.Parse(\"0123abcd-4567-89ef-fedc-ba9876543210\"); "
        "  var a: Array<Guid?> := [g1]; "
        "  var q2: Guid? := g2; "
        "  if a.Contains(q2) then return 2; end; "
        "  var q1: Guid? := g1; "
        "  if not a.Contains(q1) then return 3; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── The sibling grid: the i64-bearing value builtins ───────────────────────

// stdlib §2.5: `TimeSpan.Ticks` is a `long`, so `TimeSpan?` is `{i8,i64}` — the
// same shape as `DateTime?` and short-strided for the same reason. The tick
// count chosen does not fit in 32 bits, so a dropped high word is observable.
BEZEL_TEST(nullable_value_builtin_element_4585, array_timespan_nullable_readback_spec_2_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: TimeSpan := TimeSpan.FromTicks(864000000000L); "
        "  var a: Array<TimeSpan?> := [t, null]; "
        "  if a[0] = null then return 2; end; "
        "  if a[0]!.Ticks <> 864000000000L then return 3; end; "
        "  var q: TimeSpan? := t; "
        "  if not a.Contains(q) then return 4; end; "
        "  if a[1] <> null then return 5; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// stdlib §2.5: `DateTime?` through the ARRAY membership path — #4593 pinned its
// round-trip but not `.Contains`, and this issue's complaint was membership.
BEZEL_TEST(nullable_value_builtin_element_4585, array_datetime_nullable_contains_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: DateTime := DateTime.FromTicks(637000000000000000L); "
        "  var e: DateTime := DateTime.FromTicks(500000000000000000L); "
        "  var a: Array<DateTime?> := [d, null]; "
        "  var qd: DateTime? := d; "
        "  var qe: DateTime? := e; "
        "  if not a.Contains(qd) then return 2; end; "
        "  if a.Contains(qe) then return 3; end; "
        "  if a[0]!.Ticks <> 637000000000000000L then return 4; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── The same grid through `LinkedList<T?>` (the #4579 sibling backing) ──────

// stdlib §4.6: a node's payload width comes from `bz_llist_alloc`, a call site
// distinct from `bz_array_alloc` — #4579 was exactly one of the two being wrong
// on its own. Read back through `FirstNode!.Value`, so the oracle is the stored
// BYTES and not only a membership verdict, and paired with an absent control.
//
// ⚠ `LinkedList<T?>.First` (the `T?` property, which for `T = Guid?` collapses
// to `Guid?`) cannot be used here: it emits malformed IR on any nullable value
// element — `insertvalue { i8, %bz.guid } { i8 1, %bz.guid undef },
// { i8, %bz.guid } %ll.val, 1`, i.e. it wraps an already-tagged value a second
// time — and the compile dies in IR verification. Found while writing this file
// and reported separately; `FirstNode!.Value` is the read-back that works.
BEZEL_TEST(nullable_value_builtin_element_4585, linkedlist_guid_nullable_readback_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g: Guid := Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\"); "
        "  var g2: Guid := Guid.Parse(\"0123abcd-4567-89ef-fedc-ba9876543210\"); "
        "  var ll: LinkedList<Guid?> := []; "
        "  _ := ll.AddLast(g); "
        "  var n := ll.FirstNode; "
        "  if n = null then return 2; end; "
        "  var v: Guid? := n!.Value; "
        "  if v = null then return 3; end; "
        "  if v!.ToString() <> g.ToString() then return 4; end; "
        "  var q: Guid? := g; "
        "  var q2: Guid? := g2; "
        "  if not ll.Contains(q) then return 5; end; "
        "  if ll.Contains(q2) then return 6; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nullable_value_builtin_element_4585, linkedlist_timespan_nullable_readback_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: TimeSpan := TimeSpan.FromTicks(864000000000L); "
        "  var ll: LinkedList<TimeSpan?> := []; "
        "  _ := ll.AddLast(t); "
        "  var n := ll.FirstNode; "
        "  if n = null then return 2; end; "
        "  var v: TimeSpan? := n!.Value; "
        "  if v = null then return 3; end; "
        "  if v!.Ticks <> 864000000000L then return 4; end; "
        "  var q: TimeSpan? := t; "
        "  var q2: TimeSpan? := TimeSpan.FromTicks(5L); "
        "  if not ll.Contains(q) then return 5; end; "
        "  if ll.Contains(q2) then return 6; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nullable_value_builtin_element_4585, linkedlist_datetime_nullable_readback_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: DateTime := DateTime.FromTicks(637000000000000000L); "
        "  var ll: LinkedList<DateTime?> := []; "
        "  _ := ll.AddLast(d); "
        "  var n := ll.FirstNode; "
        "  if n = null then return 2; end; "
        "  var v: DateTime? := n!.Value; "
        "  if v = null then return 3; end; "
        "  if v!.Ticks <> 637000000000000000L then return 4; end; "
        "  var q: DateTime? := d; "
        "  var q2: DateTime? := DateTime.FromTicks(5L); "
        "  if not ll.Contains(q) then return 5; end; "
        "  if ll.Contains(q2) then return 6; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── Controls: the i32-component siblings, correct BEFORE and AFTER ─────────

// stdlib §2.5: `DateOnly`'s components are `int`s, so `DateOnly?` measures the
// same under the empty and the host layout and was never truncated. It pins
// that a fix in this family does not disturb the half that always worked.
BEZEL_TEST(nullable_value_builtin_element_4585, array_dateonly_nullable_control_spec_2_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: DateOnly := new DateOnly(2024, 3, 15); "
        "  var a: Array<DateOnly?> := [d, null]; "
        "  if a[0] = null then return 2; end; "
        "  if a[0]!.Year <> 2024 then return 3; end; "
        "  if a[0]!.Day <> 15 then return 4; end; "
        "  var q: DateOnly? := d; "
        "  if not a.Contains(q) then return 5; end; "
        "  if a[1] <> null then return 6; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(nullable_value_builtin_element_4585, linkedlist_timeonly_nullable_control_spec_2_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t: TimeOnly := new TimeOnly(13, 45, 30); "
        "  var ll: LinkedList<TimeOnly?> := []; "
        "  _ := ll.AddLast(t); "
        "  var n := ll.FirstNode; "
        "  if n = null then return 2; end; "
        "  var v: TimeOnly? := n!.Value; "
        "  if v = null then return 3; end; "
        "  if v!.Hour <> 13 then return 4; end; "
        "  if v!.Second <> 30 then return 5; end; "
        "  var q: TimeOnly? := t; "
        "  var q2: TimeOnly? := new TimeOnly(1, 2, 3); "
        "  if not ll.Contains(q) then return 6; end; "
        "  if ll.Contains(q2) then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}
