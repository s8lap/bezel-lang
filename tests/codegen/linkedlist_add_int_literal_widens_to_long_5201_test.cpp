// Codegen — issue #5201: §5.1 implicit int→long widening at a CALL ARGUMENT
// was not performed at LinkedList<T>.AddFirst / .AddLast / .AddBefore /
// .AddAfter (and at Array<T>.Add / Stack<T>.Push / Queue<T>.Enqueue). The
// runtime `bz_llist_add_*` and `bz_array_add` accept a `void*` plus a byte
// count `esz = collectionElemSize(recvTy)` (= 8 for long); the call site
// produced an `alloca` of the LOWERED ARGUMENT'S TYPE (i32 for an `int`
// literal, the literal type §17.14 fix) and then `memcpy`'d `esz` (8) bytes
// out of it. The 4-byte slot overflowed by 4 bytes every insert.
//
// MEASURED on r23-c28 @49aa9dc0 (ASan probe):
//
//     store i32 7, ptr %6               ; 4-byte alloca (literal type int → i32)
//     call void @bz_llist_add_last(    ; esz=8 (long element)
//         ptr %9, ptr %6, i64 8)
//
// Reported as "AddressSanitizer: stack-buffer-overflow … READ of size 8 …
// in memcpy / llist_node_alloc / bz_llist_add_last" with the overflowed
// 4-byte slot named in the frame report. The plain (non-ASan) build silently
// copied 4 garbage bytes after the slot — the high half of the stored long
// — so any later `.Contains(intLiteral)` membership query that compares the
// stored long against the literal's i32 (low 32 bits equal) failed
// spuriously when the high half of the literal was zero. The long-local
// control worked: `var a: long := 7; inner.AddLast(a);` lowered `a` as i64,
// the slot was 8 bytes, no overflow.
//
// THE FIX (src/codegen/LlvmBackend_Calls3.cpp, llElemSlot; and the sibling
// Add/Push/Enqueue path in LlvmBackend_Calls.cpp) widens `v` to the
// element's LLVM type at the call site when the element is a non-nullable
// primitive integer AND the lowered argument is narrower. Signedness of the
// source is consulted so a `uint` source zero-extends (`uint` → `long` =
// ZExt) and an `int` source sign-extends (`int` → `long` = SExt), per
// §5.1 / issue #410. `coerceNumericToType` itself returns `v` unchanged
// when the types already match — so every already-correct shape
// (`LinkedList<int>.AddLast(7)`, an `Object` element, a long source, a
// long literal `7L`) is byte-identical.
//
// Pinning rows: the literal-int repro (the issue's headline case) for each
// LinkedList insert method and for the array-backed Add/Push/Enqueue
// triplet, and the `uint` → `long` sibling (the sign-vs-zero-extension
// question, exactly the dimension #410's documentation calls out).
//
// §5.1 "Implicit conversions" (int → long, uint → long), §5.1
// "Constant narrowing" (the literal still arrives at the slot at the
// target width even when not in a position the §5.1 widening hint
// recognizes — `AddLast(7)` on a `LinkedList<long>` is exactly the "Set(3)
// called on Set(level: byte)" case §5.1 names as one of its constant-
// narrowing examples), v0.313. Related: #410 (ZExt vs SExt), #4579 (the
// same site, nullable-value stride, already fixed), #4639 (the same site,
// §8.6 box, already fixed), #4745 (AddLast/Add dispatch union).

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

// ── The issue's exact repro ─────────────────────────────────────────────────

// `LinkedList<long>.AddLast(7)` — an `int` literal at a `long`-typed
// LinkedList insert. Pre-fix this triggers the ASan stack-buffer-overflow
// (or, on the plain build, leaves the stored node's high half as garbage
// and the membership query answers FALSE on a present element).
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addlast_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  inner.AddLast(7); "
        "  if inner.Count = 1 then return 1; else return 0; end if; "
        "end Main; end C;"), 1);
}

// The membership query pin: pre-fix this answers FALSE on the plain build
// because the stored node's high half is the 4 uninitialized stack bytes
// past the slot — `7L == 7L` fails when the right side's high 32 bits are
// not zero. Post-fix this answers TRUE.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addlast_int_literal_contains_value_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  inner.AddLast(7); "
        "  if inner.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The long-local twin (the control from the issue body) — must stay
// green on every build; rules out a fix that merely stops the ASan abort
// without storing the real value.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addlast_long_local_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  var a: long := 7; inner.AddLast(a); "
        "  if inner.Count = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Sibling inserts at the same LinkedList<T> site ─────────────────────────

// AddFirst — the issue explicitly names it as a likely sibling ("check
// AddFirst, the Add* family and Stack/Queue").
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addfirst_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  inner.AddFirst(7); "
        "  if inner.Count = 1 and inner.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// AddBefore(node, item): the element is arg 1 (arg 0 is the anchor node),
// the widening must apply to arg 1.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addbefore_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  var n := inner.AddLast(0L); "
        "  inner.AddBefore(n, 7); "
        "  if inner.Count = 2 and inner.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// AddAfter(node, item): the element is arg 1.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addafter_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  var n := inner.AddLast(0L); "
        "  inner.AddAfter(n, 7); "
        "  if inner.Count = 2 and inner.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// LinkedList<T>.Add(item) — #4745's dispatch-union with AddLast; the
// issue's `Add* family` includes it.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, add_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  inner.Add(7); "
        "  if inner.Count = 1 and inner.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Non-literal int EXPRESSION (`3 + 4`, typed int) at a long element slot. The
// row was first written against OwningLinkedList<long>, which §4.2/§14.4
// (#1327) rejects (a primitive element cannot be owned); respelled to the
// non-literal widening shape, which exercises the same llElemSlot path.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, owninglinkedlist_addlast_int_literal_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  inner.AddLast(3 + 4); "
        "  if inner.Count = 1 and inner.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Stack / Queue / Array — the array-backed `Add* family` ─────────────────

// Array<long>.Add(7) — the same widening must apply to the array-backed
// arm. Pre-fix this hits the identical stack-buffer-overflow in
// `bz_array_add` (Stack.Push / Queue.Enqueue share the runtime).
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, array_add_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<long>(); "
        "  a.Add(7); "
        "  if a.Count = 1 and a.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Stack<long>.Push(7).
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, stack_push_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<long>(); "
        "  s.Push(7); "
        "  if s.Count = 1 and s.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Queue<long>.Enqueue(7).
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, queue_enqueue_int_literal_present_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q := new Queue<long>(); "
        "  q.Enqueue(7); "
        "  if q.Count = 1 and q.Contains(7L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── The `uint` → `long` sibling — sign vs zero extension (#410) ────────────

// A `uint` value at a `long` slot MUST zero-extend (§5.1, issue #410). A
// pre-fix sign-extension would store the high 32 bits as 0xFFFFFFFF for
// `4000000000u` and a `Contains(4000000000L)` query would falsely answer
// FALSE. Post-fix the value at the slot is exactly `4000000000L`.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addlast_uint_to_long_zero_extends_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  var u: uint := 4000000000U; "
        "  inner.AddLast(u); "
        "  if inner.Count = 1 and inner.Contains(4000000000L) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Same width (`int` → `long`, value < 2^31) — the high 32 bits after a
// sign-extension are all zero, so this distinguishes sign-extension from
// zero-extension only on negative values; pinning the `int.MinValue` case
// pins the §5.1 / #410 SExt decision on a literal.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, addlast_int_literal_value_preserved_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<long>(); "
        "  inner.AddLast(-1); "
        "  if inner.Contains(-1L) and inner.Count = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Control: shapes the fix must leave byte-identical ──────────────────────

// `LinkedList<int>.AddLast(7)` — the source is already at the element's
// width, so the widening is a no-op. This rules out a fix that always
// re-emits a conversion.
BEZEL_TEST(linkedlist_add_int_literal_widens_to_long_5201, linkedlist_int_element_unaffected_spec_5_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var inner := new LinkedList<int>(); "
        "  inner.AddLast(7); "
        "  if inner.Count = 1 and inner.Contains(7) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}