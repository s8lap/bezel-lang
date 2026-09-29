// Codegen — issue #4593: a collection element slot whose LLVM layout depends on
// an `i64` member was measured against the WRONG DataLayout, so the tail of
// every such value was dropped going in and read back uninitialized.
// (spec §7.2 / §9.1, stdlib §2.5 / §2.6 / §4.4 / §4.6 / §4.8 / §4.10 / §9.8,
// v0.306.)
//
// AUTHORITY. §7.2 v0.306, verbatim: "A value type wrapped in `?` is an
// optional: it either holds a value or `null`. `int?`, `Priority?`, and
// `DateTime?` are optional value types." stdlib §4.4 declares `Array<T>`'s
// indexer as `public property [i: int]: T` — reading slot `i` yields the value
// stored at `i`, for every admitted `T`. Between them: `var a: Array<T?> := [v];
// a[0]` is `v`, and a slot written `null` reads back `null`. Nothing in either
// document makes the round-trip conditional on T's internal shape.
//
// THE BUG — one DataLayout, measured twice. A fresh `llvm::Module` carries the
// EMPTY DataLayout, whose default integer alignments are not the host's: `i64`
// gets a 4-byte ABI alignment ("i64:32:64") where x86-64 gives it 8. The
// emitter's every `module_->getDataLayout().getTypeAllocSize(T)` — the byte
// count it hands the runtime as a memcpy length or an element stride
// (`collectionElemSize`, `lazyElemSize`, the dictionary/set entry stride, the
// record-box malloc, the iterator frame calloc) — therefore answered a DIFFERENT
// question from the one the object emitter answers when it lays `alloca T` out,
// because `Emitter` ran to completion and only then did `emitObjectFile` stamp
// the host layout. For any aggregate whose padding depends on an i64 member the
// two disagreed:
//
//     Guid?          {i8, {i64,i64}}     empty-DL 20   host 24
//     DateTime?      {i8, i64}           empty-DL 12   host 16
//     Range<int>?    {i8, {i64,i64,i64,i8}}  empty-DL 32   host 40
//     record R(A: int, B: long)  {i32,i64}   empty-DL 12   host 16
//
// while the families the issue measured as CORRECT — `int?` `{i8,i32}`,
// `decimal?` `{i8,{i32,i32,i32,i32}}`, `Color?`, `record P(X: int)` — contain no
// i64 and so are laid out identically by both. That asymmetry is the whole
// shape of the defect: it is not a property of the value-BUILTIN family, it is
// a property of `i64`.
//
// §7.2's tag is at offset 0, so it always survived the short copy; the payload
// is what was truncated. Measured on cafdfc52, `Array<DateTime?>` storing
// 637000000000000000 (0x08d7142001ec8000) read back -4051333369136644096
// (0xc7c6c5c401ec8000): the low 32 bits round-trip exactly and the high 32 bits
// are leftover stack bytes — `bz_array_add` copied 12 of the 16 bytes in, and
// `bz_array_get` copied 12 back into a 16-byte destination, leaving the top word
// whatever the frame already held.
//
// The fix stamps the host triple + DataLayout on the module at Emitter
// construction, so the emitter measures with the layout the backend lowers with.
// It can only ever RAISE a size: the default DataLayout's alignments are <= the
// host's for every type (x86-64 overrides only i64/i128/f80 upward), so no site
// can move from over- to under-allocating.
//
// FAIL-WITHOUT, proved mechanically on cafdfc52 (baseline `origin/main`), by
// copying the three edited src files to scratch, `git checkout HEAD -- <file>`,
// rebuilding the TEST binary and re-running this filter on both oracles:
//   * plain          — 3 passed, 8 failed. The 3 are the two `_control_` rows
//                      and `dict_record_long_field_box_spec_4_8`, whose defect
//                      the plain build tolerates.
//   * FORCE_SANITIZE — 2 passed, 9 failed, `NATIVE SANITIZER: 0 check(s)
//                      skipped`. Only the two controls survive; the dict row
//                      joins the failures with a heap-buffer-overflow.
// With the fix: 11/11 on both, `0 check(s) skipped`.
// The controls pass on BOTH sides by design — they are the guard against a
// "fix" that repairs the i64 family by breaking the i32 one, or that makes the
// tagged comparison constant-true.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's three named types, `Array<T?>` store-then-index ─────────────

// stdlib §2.6 / §4.4, spec §7.2: two distinct `Guid` values and a `null` share
// one `Array<Guid?>`; each slot reads back what was written. Pre-fix: exit 5 —
// the tag and the `null` slot were right (rows 2..4 pass) and only the present
// payload's top word was garbage, which is exactly what the issue measured.
// The parsed literals are re-parsed for the comparison rather than compared to
// the stored local, so a slot that merely aliased the source would not pass.
BEZEL_TEST(elem_stride_4593, array_guid_element_roundtrip_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g1: Guid := Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\"); "
        "  var g2: Guid := Guid.Parse(\"0123abcd-4567-89ef-fedc-ba9876543210\"); "
        "  var a: Array<Guid?> := [g1, null, g2]; "
        "  var e0: Guid? := a[0]; "
        "  var e1: Guid? := a[1]; "
        "  var e2: Guid? := a[2]; "
        "  if e0 = null then return 2; end; "
        "  if e1 <> null then return 3; end; "
        "  if e2 = null then return 4; end; "
        "  if e0 <> Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\") then return 5; end; "
        "  if e2 <> Guid.Parse(\"0123abcd-4567-89ef-fedc-ba9876543210\") then return 6; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// stdlib §2.5 / §4.4, spec §7.2: the `DateTime?` repro, asserted on `Ticks`
// (a `long`) rather than on struct equality, so the failure names the truncated
// word directly. 637000000000000000 does not fit in 32 bits, so the dropped high
// word is observable. Pre-fix: exit 3.
BEZEL_TEST(elem_stride_4593, array_datetime_ticks_roundtrip_spec_2_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<DateTime?> := [DateTime.FromTicks(637000000000000000L), null]; "
        "  var e: DateTime? := a[0]; "
        "  if e = null then return 2; end; "
        "  if e!.Ticks <> 637000000000000000L then return 3; end; "
        "  if a[1] <> null then return 4; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// stdlib §9.8 / §4.4, spec §7.2: `Range<int>` is the widest of the three —
// `{i64,i64,i64,i8}` padded to 32 on the host against 28 under the empty
// layout, so `Range<int>?` was copied 32 bytes of 40. Pre-fix: exit 4 — the
// tag and the `null` slot were right, the payload was not.
BEZEL_TEST(elem_stride_4593, array_range_element_roundtrip_spec_9_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 1 .. 5; "
        "  var a: Array<Range<int>?> := [r, null]; "
        "  var e: Range<int>? := a[0]; "
        "  if e = null then return 2; end; "
        "  if a[1] <> null then return 3; end; "
        "  if e <> r then return 4; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// stdlib §4.4 `Add`: the issue's repro 4 — the `.Add` insertion path, not the
// §15.4 literal materializer, so a fix that only repaired the literal would
// still fail here. Pre-fix: exit 4.
BEZEL_TEST(elem_stride_4593, array_add_then_index_guid_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g: Guid := Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\"); "
        "  var a: Array<Guid?> := []; "
        "  a.Add(g); "
        "  a.Add(null); "
        "  if a.Count <> 2 then return 2; end; "
        "  var e: Guid? := a[0]; "
        "  if e = null then return 3; end; "
        "  if e <> Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\") then return 4; end; "
        "  var q: Guid? := g; "
        "  if not a.Contains(q) then return 5; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── The same root cause outside the nullable value-builtin family ───────────
// These are why the fix is at the DataLayout and not at `collectionElemSize`:
// each is a DIFFERENT lowering (array of a bare record, a hash-backed set, a
// LINQ lazy pipeline, the node-backed list, a dictionary's record box) reaching
// the same wrong measurement through its own `getTypeAllocSize` call site.

// spec §9.1 / stdlib §4.4: `record R(A: int, B: long)` needs no `?` at all —
// `{i32,i64}` is 12 under the empty layout and 16 on the host, so a plain
// `Array<R>` truncated `B`. Pre-fix: exit 3.
BEZEL_TEST(elem_stride_4593, array_record_long_field_roundtrip_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(A: int, B: long); class C public static method Main(): int "
        "  var a: Array<R> := []; "
        "  a.Add(new R(1, 9000000000L)); "
        "  a.Add(new R(2, 8000000000L)); "
        "  var x: R := a[1]; "
        "  if x.A <> 2 then return 2; end; "
        "  if x.B <> 8000000000L then return 3; end; "
        "  var y: R := a[0]; "
        "  if y.B <> 9000000000L then return 4; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// stdlib §4.10 / §4.11: the hash-backed `Set<T>` entry stride
// (`bz_dict_alloc_generic`) and the `Stack<T>` array stride are separate call
// sites from `Array<T>`'s. Pre-fix: exit 2 — the member stored was not found.
// This row is also #2633's asymmetry in miniature: under generated-code ASan
// the pre-fix binary instead exits 1 and reports a heap-buffer-overflow, so it
// fails on BOTH oracles but for different reasons. The plain oracle is the one
// that shows the wrong ANSWER.
BEZEL_TEST(elem_stride_4593, set_and_stack_record_long_field_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(A: int, B: long); class C public static method Main(): int "
        "  var s: Set<R> := []; "
        "  s.Add(new R(1, 9000000000L)); "
        "  if not s.Contains(new R(1, 9000000000L)) then return 2; end; "
        "  var q: Stack<R> := []; "
        "  q.Push(new R(2, 8000000000L)); "
        "  var t: R := q.Pop(); "
        "  if t.B <> 8000000000L then return 3; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// stdlib §4.6: the node-backed `LinkedList<T>` family reaches the stride through
// `bz_llist_alloc`/`bz_llist_add_last` — the same pair fix #4579 corrected for a
// different reason. Its element here is a nullable value-builtin, so the node
// payload was 20 bytes wide for a 24-byte value. Pre-fix: exit 0 plain, and a
// heap-buffer-overflow under generated-code ASan.
BEZEL_TEST(elem_stride_4593, linkedlist_nullable_guid_membership_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll: LinkedList<Guid?> := []; "
        "  var g: Guid := Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\"); "
        "  ll.AddLast(g); "
        "  var q: Guid? := g; "
        "  if ll.Contains(q) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// stdlib §4.8: a `Dictionary<K,V>` boxes a record value on the heap with
// `malloc(getTypeAllocSize(V))` and then stores the WHOLE struct into it. The
// plain build tolerated the 4-byte overrun (the value read back correctly here,
// so this row passes on both sides UNSANITIZED); under generated-code ASan the
// pre-fix binary aborts with "heap-buffer-overflow … WRITE of size 16 at … 3
// bytes after 12-byte region". Measured on cafdfc52. This is the row that makes
// the `BEZEL_AOT_FORCE_SANITIZE=address` leg load-bearing for this fix.
BEZEL_TEST(elem_stride_4593, dict_record_long_field_box_spec_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(A: int, B: long); class C public static method Main(): int "
        "  var d: Dictionary<string, R> := []; "
        "  d[\"k\"] := new R(5, 9000000000L); "
        "  var p: R := d[\"k\"]; "
        "  if p.B <> 9000000000L then return 2; end; "
        "  if p.A <> 5 then return 3; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// stdlib §4.4 / §4.12: the LINQ lazy pipeline carries its own element stride
// (`bz_lazy_from_array` / `bz_lazy_to_array`), so the truncated `B` changed
// which elements a predicate selected — a wrong COUNT, not merely a wrong field.
// Pre-fix: exit 2 plain, and a heap-buffer-overflow under generated-code ASan.
BEZEL_TEST(elem_stride_4593, linq_over_record_long_field_spec_4_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(A: int, B: long); class C public static method Main(): int "
        "  var a: Array<R> := [new R(1, 9000000000L), new R(2, 8000000000L)]; "
        "  using var r := a.Where(lambda(x) => x.B > 8500000000L).ToArray(); "
        "  if r.Count <> 1 then return 2; end; "
        "  if r[0].B <> 9000000000L then return 3; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── Controls: correct BEFORE and AFTER ─────────────────────────────────────

// spec §7.2 / stdlib §4.4: `int?` is `{i8,i32}` — no i64, so both layouts agree
// and this round-trip was already correct. It is the asymmetry the issue named,
// and it pins that the fix does not disturb the family that worked.
BEZEL_TEST(elem_stride_4593, array_int_element_roundtrip_control_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int?> := [77, null]; "
        "  var e: int? := a[0]; "
        "  if e = null then return 2; end; "
        "  if e <> 77 then return 3; end; "
        "  if a[1] <> null then return 4; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// spec §8.4 / §7.2: a `null` slot is `null` and a present slot is NOT — the tag
// half of the comparison, which survived the short copy and must keep doing so.
// A "fix" that widened the stride but wrote the tag at the wrong offset, or one
// that made the nullable comparison constant-true, fails here.
BEZEL_TEST(elem_stride_4593, array_guid_tag_discrimination_control_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var g: Guid := Guid.Parse(\"11223344-5566-7788-99aa-bbccddeeff01\"); "
        "  var a: Array<Guid?> := [null, g]; "
        "  if a[0] <> null then return 2; end; "
        "  if a[1] = null then return 3; end; "
        "  var n: Guid? := null; "
        "  if not a.Contains(n) then return 4; end; "
        "  var b: Array<Guid?> := [g]; "
        "  if b.Contains(n) then return 5; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}
