// Codegen — issue #4639: `LinkedList<Object>.AddFirst`/`.AddLast` never
// §8.6-box a VALUE element, so a raw scalar goes into a pointer-wide node
// slot; any later membership op that treats the stored word as a §8.6 box
// dereferences it as a vtable pointer (§8.6, stdlib §4.6, v0.307).
//
// MECHANISM, confirmed on this lane's HEAD by probe before the fix (both
// sides measured under `--sanitize=address`, not inferred from the symptom):
//
//     store i32 10, ptr %6
//     call void @bz_llist_add_last(ptr %9, ptr %6, i64 8)  ; 4-byte alloca,
//                                                           ; 8-byte (ptr-wide
//                                                           ; Object slot) esz
//
// `esz` is `collectionElemSize` for the instantiation — 8 bytes, a pointer,
// for `LinkedList<Object>` — but `AddFirst`/`AddLast` lowered the argument
// with a bare `lowerExpr` and spilled it at ITS OWN type, so `ll.AddLast(10)`
// allocated a 4-byte `alloca i32`. `bz_llist_add_last` -> `llist_node_alloc`
// then `memcpy`s `esz` (8) bytes OUT of that 4-byte slot. MEASURED pre-fix:
// `AddressSanitizer: stack-buffer-overflow … READ of size 8 … in memcpy /
// llist_node_alloc / bz_llist_add_last` (and the identical shape through
// `bz_llist_add_first` under `bz_llist_add_before`/`_add_after`, which share
// the same node-alloc primitive). On the plain (non-ASan) build the same
// spill leaves the node holding a pointer-shaped word assembled from
// uninitialized stack bytes, and the issue's own repro — `AddLast(10)`,
// `AddLast(20)`, then `.Contains(20)` — SIGSEGVs when that word is
// dereferenced as a §8.6 box's `{ vtable*, i64 rc, payload }` header.
//
// THE FIX mirrors #819 (the Array/Stack/Queue `Add`/`Push`/`Enqueue` site,
// `LlvmBackend_Calls.cpp`): `boxForTarget` applied to the argument, once,
// before it is spilled to the node's alloca slot — `Emitter::boxForTarget`
// (LlvmBackend_Layout.cpp) is a no-op for a reference source (class/
// interface/string/`Object`-already) or a non-Object/interface element slot,
// so every already-correct shape (`LinkedList<int>`, a class element, a
// pre-boxed `Object` local) is unchanged. Taken at all FOUR insert sites in
// `tryLowerLinkedListInstanceCall` (`LlvmBackend_Calls3.cpp`) — `AddFirst`
// and `AddLast` per the issue, plus the sibling `AddBefore`/`AddAfter`, which
// share the identical `llElemSlot`-spill-into-`bz_llist_add_*` shape and were
// MEASURED to overflow identically before this fix (narrow #4639: leaving
// them unboxed while `AddFirst`/`AddLast` were fixed would still SIGSEGV).
//
// Once the container itself stores a real box, the membership funnel #4619
// already boxes its OWN query argument (`boxMembershipQuery`) — so
// `.Contains`/`.Find`/`.Remove`/`x in coll` against a `LinkedList<Object>`
// become reachable for the first time; #4619's own lane deliberately shipped
// no `LinkedList<Object>` rows for exactly this reason. Some are pinned below.
//
// ORACLES. `run()` returns `-999` when the program does not complete (crash /
// SIGSEGV / ASan abort — `AotResult::ran` is false), so a fix that leaves the
// overflow in place fails these rows rather than passing vacuously; every row
// asserts the actual boolean/count answer, never a bare "it compiled".
//
// §8.6 / stdlib §4.6, v0.307. Related: #819 (the mirrored fix), #4619 (the
// membership-query box this unblocks), #4579 (the neighboring nullable-stride
// overflow at the same four call sites, already fixed).

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

// ── AddLast — the issue's own repro ─────────────────────────────────────────

// The issue's exact program: `AddLast(10)`, `AddLast(20)`, `.Contains(20)`.
// Pre-fix this SIGSEGVs (`run()` returns -999); the corrected answer is `1`
// (present).
BEZEL_TEST(linkedlist_add_object_box_4639, addlast_bare_int_contains_present_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  _ := ll.AddLast(10); _ := ll.AddLast(20); "
        "  if ll.Contains(20) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The absent control, so a fix that merely stops the crash without storing
// the real payload (e.g. an uninitialized box) cannot pass by answering
// `true` unconditionally.
BEZEL_TEST(linkedlist_add_object_box_4639, addlast_bare_int_contains_absent_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  _ := ll.AddLast(10); _ := ll.AddLast(20); "
        "  if ll.Contains(99) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── AddFirst — the issue's other named entry point ──────────────────────────

BEZEL_TEST(linkedlist_add_object_box_4639, addfirst_bare_int_contains_present_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  _ := ll.AddFirst(10); _ := ll.AddFirst(20); "
        "  if ll.Contains(10) and ll.Contains(20) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── AddBefore / AddAfter — the sibling insert paths this fix also covers ───

BEZEL_TEST(linkedlist_add_object_box_4639, addbefore_bare_int_contains_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  var n := ll.AddLast(10); "
        "  _ := ll.AddBefore(n, 20); "
        "  if ll.Contains(10) and ll.Contains(20) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linkedlist_add_object_box_4639, addafter_bare_int_contains_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  var n := ll.AddLast(10); "
        "  _ := ll.AddAfter(n, 20); "
        "  if ll.Contains(10) and ll.Contains(20) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Find / Remove / `x in coll` — the #4619 funnel, reachable for the first
//    time now that the container itself stores a real box ──────────────────

BEZEL_TEST(linkedlist_add_object_box_4639, find_bare_int_returns_matching_node_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  _ := ll.AddLast(10); _ := ll.AddLast(20); "
        "  var node := ll.Find(20); "
        "  if node <> null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linkedlist_add_object_box_4639, remove_bare_int_effect_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  _ := ll.AddLast(10); _ := ll.AddLast(20); "
        "  var gone: bool := ll.Remove(10); "
        "  if gone and (ll.Count = 1) and (not ll.Contains(10)) "
        "     and ll.Contains(20) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(linkedlist_add_object_box_4639, x_in_linkedlist_bare_int_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  _ := ll.AddLast(10); _ := ll.AddLast(20); "
        "  if (20 in ll) and not (99 in ll) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Controls: shapes the fix must leave byte-identical ──────────────────────

// A pre-boxed `Object` local was already correct (the source is a reference,
// so `boxForTarget` is already a no-op there) — must stay correct, ruling out
// a fix that double-boxes an already-boxed source.
BEZEL_TEST(linkedlist_add_object_box_4639, addlast_preboxed_object_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<Object>(); "
        "  var o: Object := 10; "
        "  _ := ll.AddLast(o); "
        "  if ll.Contains(10) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `LinkedList<int>` (a non-Object element slot) is untouched by `boxForTarget`
// — this pins that the value-family (non-boxing) path still works.
BEZEL_TEST(linkedlist_add_object_box_4639, linkedlist_int_element_unaffected_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var ll := new LinkedList<int>(); "
        "  _ := ll.AddLast(10); _ := ll.AddLast(20); "
        "  if ll.Contains(10) and ll.Contains(20) and (not ll.Contains(30)) "
        "     then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
