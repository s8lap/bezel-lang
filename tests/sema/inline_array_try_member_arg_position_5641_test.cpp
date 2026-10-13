// fix-forward #5641 — spec §15.1 (v0.334), the conditionally-admitted-member
// register for a stdlib member that forms `T?` at a receiver type parameter.
//
// THE RULE, quoted from docs/Bezel_Language_Spec.md §15.1 (line 2784, v0.334):
//
//   "a standard-library member whose **signature** forms `T?` at a type
//    parameter of its receiver type — the element type, or a dictionary's value
//    type — as a result, `out` slot or property, the `*OrNull` tier
//    (`FirstOrNull`) and the `Try*(out item: T?)` family (`TryPop`,
//    `TryReceive`) among them, is a conditionally-admitted member where that
//    type argument is an inline array (§14.2's register)"
//
// The register is scoped BY POSITION. `Dictionary<K,V>.TryGetValue(key: K,
// out value: V?)` forms its `T?` at V. The KEY type participates in no `T?`
// the signature forms — `key: K` is K, not `K?` — so `Dictionary<[3]int,
// string>` is NOT over the register and the call is admitted.
//
// WHAT WAS WRONG: 05e2661b (the rejected tip of r29-151-inline-array) scanned
// EVERY generic argument of the receiver for Kind::InlineArray and rejected on
// any hit. MEASURED over-rejection:
//
//   Dictionary<[3]int, string>.TryGetValue(k, out var v)
//     -> "`TryGetValue()` with an inline-array type argument is not admitted"
//        (rc=1, SEMANTIC rejection at the call)
//
// 2e9b91e8 narrows the scan to the one argument the member forms `T?` at,
// via `builtinNullableOutTypeArgIndex` (ExprTypecheck_Calls.cpp). The rows
// below pin that narrowing.
//
// ─────────────────────────────────────────────────────────────────────────
// READ THIS BEFORE READING A GREEN ROW AS "inline-array dictionary keys
// WORK NOW" — THEY DO NOT. A green SEMA row is not a green program.
//
// 05e2661b's over-rejection was MASKING a pre-existing codegen hole (#5882).
// Narrowing the predicate UNMASKS it: the admitted program below still fails,
// but LATER and for an unrelated pre-existing reason — IR verification:
//
//   GEP base pointer is not a vector or a vector of pointers
//     %10 = getelementptr inbounds nuw %bz.string, [3 x i32] %9, i32 0, i32 0
//   Load operand must be a pointer.
//     %str.data = load ptr, [3 x i32] %10, align 8
//   bezelc: codegen error: IR verification failed
//
// MEASURED, that failure is INDEPENDENT of this fix: a plain `d[k]` READ
// with no TryGetValue anywhere in the program fails identically. So R1 below
// asserts the ABSENCE OF A SPECIFIC DIAGNOSTIC, never "it compiled". A row
// that only asserted `r.ran` would go green again the moment #5882 lands and
// would have pinned nothing in the meantime.
//
// Rows:
//   R1  Dictionary<[3]int, string>.TryGetValue — the fix. The §15.1
//       Try*-family rejection must be ABSENT (it is present on 05e2661b).
//   R2  Dictionary<[3]int, [3]int>.TryGetValue — ADMIT CONTROL. Key AND value
//       both inline arrays, so the value type at index 1 IS over the register:
//       the rejection must be PRESENT. Proves the scan was narrowed, not
//       disabled, and that the predicate reads index 1 for a dictionary.
//   R3  Dictionary<string, [3]int>.TryGetValue — value type inline, key not.
//       Rejection PRESENT: it is the value argument that the rule keys on,
//       in the mirror-image receiver.
//   R4  Stack<[3]int>.TryPop(out var) — ELEMENT type (index 0). PRESENT.
//   R5  Channel<[3]int>.TryReceive(out var) — ELEMENT type (index 0). PRESENT.
//   R6  Array<[3]int>.FirstOrNull() — the `*OrNull` tier, a different path
//       (return type, not an `out` slot). PRESENT, unaffected by this fix.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// The §15.1 Try*-family rejection text. Deliberately matched on the phrase
// that names the formed type, not on a whole-message literal, so a cosmetic
// reword does not silently unpin the row.
constexpr const char* kTryMemberRejected =
    "with an inline-array type argument";

// ── R1 — Dictionary<[3]int, string>: the KEY is an inline array but the
// member forms `T?` at V, so §15.1 does not register this call. The
// Try-family rejection must be ABSENT.
//
// Deliberately does NOT assert `r.ran`: #5882 makes this program fail at IR
// verification (see the header). Asserting success would pin a codegen bug
// this fix does not own, and would be the shape that re-greens silently.
BEZEL_TEST(sema_inline_array_try_member_arg_position_5641,
           dict_inline_array_key_string_value_not_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<[3]int, string>(); "
        "  var k: [3]int := [1, 2, 3]; "
        "  d.Add(k, \"three\"); "
        "  if d.TryGetValue(k, out var v) then "
        "    Console.WriteLine(v); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kTryMemberRejected));
}

// ── R2 — ADMIT CONTROL. Both type arguments are inline arrays, so the VALUE
// type the member forms `T?` at (index 1) IS over the register. The rejection
// must be PRESENT. This is what distinguishes "narrowed to the right
// argument" from "stopped checking dictionaries".
BEZEL_TEST(sema_inline_array_try_member_arg_position_5641,
           dict_inline_array_key_and_value_still_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<[3]int, [3]int>(); "
        "  var k: [3]int := [1, 2, 3]; "
        "  d.Add(k, k); "
        "  if d.TryGetValue(k, out var v) then "
        "    Console.WriteLine(v[0]); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTryMemberRejected));
}

// ── R3 — Mirror image of R2: the KEY is fine, the VALUE is the inline array.
// The rule keys on the value argument, so the rejection must be PRESENT.
BEZEL_TEST(sema_inline_array_try_member_arg_position_5641,
           dict_string_key_inline_array_value_still_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, [3]int>(); "
        "  d.Add(\"a\", [1, 2, 3]); "
        "  if d.TryGetValue(\"a\", out var v) then "
        "    Console.WriteLine(v[0]); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTryMemberRejected));
}

// ── R4 — ELEMENT type (index 0) over the register. PRESENT.
BEZEL_TEST(sema_inline_array_try_member_arg_position_5641,
           stack_inline_array_element_try_pop_still_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var s := new Stack<[3]int>(); "
        "  var a: [3]int := [1, 2, 3]; "
        "  s.Push(a); "
        "  if s.TryPop(out var top) then "
        "    Console.WriteLine(top[0]); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTryMemberRejected));
}

// ── R5 — ELEMENT type (index 0) over the register. PRESENT.
BEZEL_TEST(sema_inline_array_try_member_arg_position_5641,
           channel_inline_array_element_try_receive_still_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var ch := new Channel<[3]int>(1); "
        "  if ch.TryReceive(out var got) then "
        "    Console.WriteLine(got[0]); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kTryMemberRejected));
}

// ── R6 — The `*OrNull` tier: a RETURN type, not an `out` slot, so it is a
// different code path and must be unaffected by this fix. PRESENT.
BEZEL_TEST(sema_inline_array_try_member_arg_position_5641,
           array_inline_array_element_first_or_null_still_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  using var xs := new Array<[3]int>(); "
        "  var a: [3]int := [1, 2, 3]; "
        "  xs.Add(a); "
        "  var first := xs.FirstOrNull(); "
        "  return xs.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "is not admitted on an `Array`"));
}