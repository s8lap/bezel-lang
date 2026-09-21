// Codegen — issue #4571: `Array<decimal>.Contains`/`.IndexOf`/`.Remove` (and
// `LinkedList<decimal>.Contains`, and the array-backed `Set<T>` dedup insert
// for any element the generic-dict backing declines) crash the compiler with
// an IR-verification failure instead of comparing decimal values correctly
// (§4.4/§8.3, v0.306).
//
// MECHANISM. `Array<T>.Contains`/`.IndexOf`/`.Remove`, `LinkedList<T>.Contains`/
// `.Remove`/`.Find`, and the array-backed `Set<T>`/`OwningSet<T>` dedup insert
// all route per-element comparison through `Emitter::emitRemoveElementEquality`
// (src/codegen/LlvmBackend_Calls.cpp). That function had dedicated arms for a
// record element (`emitStructEquality`), a class element (user `Equals`
// override or reference identity), and — as of narrow #4553 — a nullable-value
// element (`emitValueEquality`) — but NO arm for a non-nullable `decimal`
// element. `decimal` lowers to the 16-byte `%bz.decimal` struct
// (`decimalStructType()`, `{i32×4}`), which matched none of those shapes, so it
// fell to the final scalar fallback:
//
//   if (elN->getType()->isFloatingPointTy())
//       return builder_.CreateFCmpOEQ(elN, tN);
//   return builder_.CreateICmpEQ(elN, tN);
//
// `%bz.decimal` is neither a floating-point type nor a valid `ICmpInst`
// operand (int/ptr/vector-of-int-or-ptr only), so `CreateICmpEQ` built
// malformed IR that only surfaced as a diagnostic at the verifier —
// "Invalid operand types for ICmp instruction … icmp eq %bz.decimal %el, %tgt"
// — rather than failing closed at construction time. Measured on this
// worktree's pre-fix HEAD (2b8e465f) with the issue's own repro.
//
// THE FIX adds a decimal-struct arm that routes to `emitValueEquality` — the
// SAME per-value helper narrow #4553 routed the nullable-value arm through
// immediately above it ("route to it rather than spelling one rule twice"),
// and the SAME helper fix #4562 routed `decimal.Equals`/`.GetHashCode`
// (`lowerValueSemMethodOn`, a DIFFERENT dispatch site reached from a receiver
// method call rather than a collection's internal element scan) through for
// the identical "decimal is a struct, not a scalar" reason.
// `emitValueEquality`'s own non-nullable-decimal arm (LlvmBackend_Layout.cpp,
// fix #663) already calls `bz_decimal_cmp`, which is scale-invariant per
// §4.4: `5.5m` and `5.50m` compare equal despite differing scale bits — the
// oracle below asserts that value, not merely that the program compiles (a
// fix that made every comparison answer `true` unconditionally would still
// need to fail the "absent value" and "unequal pair" rows below).
//
// SCOPE. `Dictionary<decimal, V>` keys and `decimal.Equals(...)` were ALREADY
// fixed by #4562, through the unrelated `lowerValueSemMethodOn` dispatch site
// (LlvmBackend_Calls2.cpp) — confirmed still-passing here as a control, not
// re-fixed. `Set<decimal>`/`OwningSet<decimal>` are NOT reached through
// `emitRemoveElementEquality`'s array-backed scan at all: `decimal` is
// GENERIC-BACKED-ELIGIBLE (`genericBackingType`, fix #685 — "decimal is a
// 16-byte struct but has well-defined value equality (bz_decimal_cmp) and
// hash (bz_decimal_hash)"), so `Set<decimal>` uses the hash-table hash/equals
// trampolines (already decimal-aware), not the linear array scan this fix
// touches; measured below as a control, unaffected by (and not exercising)
// this fix. `OwningSet<T>`/`OwningLinkedList<T>` both declare `where T: class`
// (stdlib §4.7/§4.10), so `decimal` cannot instantiate them — out of the grid
// by construction, not measured.
//
// FAIL-WITHOUT proved mechanically: with LlvmBackend_Calls.cpp restored to
// this worktree's pre-fix HEAD (2b8e465f) and the test binary rebuilt, every
// `array_*`/`linkedlist_*` row below reports `ran == false` (a codegen IR-
// verification crash, so `run()` yields the -999 sentinel against an expected
// 0/1) — the issue's own repro and its Remove/IndexOf/LinkedList siblings; with
// the fix, all rows pass. The `dictionary_*`/`set_*` control rows are
// unaffected on both sides (already correct via #4562/#685 respectively).
//
// §4.4 / §8.3, v0.306.

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

// ── Array<decimal>.Contains — the issue's own repro ─────────────────────────

// §4.4: `5.50m` (present as `5.5m`, differing scale bits) must be found.
BEZEL_TEST(array_decimal_element_equality_4571, array_contains_true_same_value_different_scale_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [5.5m, 9.25m]; "
        "  if a.Contains(5.50m) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The absent-value control: a fix that made Contains always answer `true`
// would pass the row above but fail this one.
BEZEL_TEST(array_decimal_element_equality_4571, array_contains_false_absent_value_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [5.5m, 9.25m]; "
        "  if a.Contains(1.0m) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── Array<decimal>.IndexOf ──────────────────────────────────────────────────

BEZEL_TEST(array_decimal_element_equality_4571, array_indexof_finds_scale_invariant_match_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [9.25m, 5.5m]; "
        "  if a.IndexOf(5.50m) = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// -1 control: an absent value must not be reported as found at any index.
BEZEL_TEST(array_decimal_element_equality_4571, array_indexof_absent_returns_negative_one_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [9.25m, 5.5m]; "
        "  if a.IndexOf(1.0m) = -1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Array<decimal>.Remove ───────────────────────────────────────────────────

// Removes the scale-invariant match, leaves the other element intact — a
// stronger oracle than "compiles": wrong-target removal or a no-op both fail.
BEZEL_TEST(array_decimal_element_equality_4571, array_remove_true_leaves_other_element_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [5.5m, 9.25m]; "
        "  var removed: bool := a.Remove(5.50m); "
        "  if removed and (a.Count = 1) and (a[0] = 9.25m) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Removing an absent value: no mutation, false result.
BEZEL_TEST(array_decimal_element_equality_4571, array_remove_false_absent_value_no_mutation_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [5.5m, 9.25m]; "
        "  var removed: bool := a.Remove(1.0m); "
        "  if (not removed) and (a.Count = 2) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── `in` over Array<decimal> (shares the same emitRemoveElementEquality path)

BEZEL_TEST(array_decimal_element_equality_4571, decimal_in_array_true_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<decimal> := [5.5m, 9.25m]; "
        "  var q: decimal := 5.50m; "
        "  if q in a then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── LinkedList<decimal>.Contains (same emitRemoveElementEquality call site,
// reached through the node-walk in LlvmBackend_Calls3.cpp rather than an
// array scan) ────────────────────────────────────────────────────────────

BEZEL_TEST(array_decimal_element_equality_4571, linkedlist_contains_true_scale_invariant_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var l: LinkedList<decimal> := [5.5m, 9.25m]; "
        "  if l.Contains(5.50m) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

BEZEL_TEST(array_decimal_element_equality_4571, linkedlist_contains_false_absent_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var l: LinkedList<decimal> := [5.5m, 9.25m]; "
        "  if l.Contains(1.0m) then return 0; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ── controls: NOT reached by this fix, must remain correct on both sides ──

// `Set<decimal>` is generic-dict-backed (fix #685), not array-scanned —
// dedup collapses the scale-invariant duplicate via the hash trampolines,
// independent of emitRemoveElementEquality.
BEZEL_TEST(array_decimal_element_equality_4571, set_decimal_dedup_control_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<decimal> := [5.5m, 5.50m, 9.25m]; "
        "  if s.Count = 2 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// `Dictionary<decimal, V>.ContainsKey` — already fixed by #4562's
// `lowerValueSemMethodOn` arm, a different dispatch site; control only.
BEZEL_TEST(array_decimal_element_equality_4571, dictionary_decimal_key_control_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var m: Dictionary<decimal, int> := [5.5m: 1, 9.25m: 2]; "
        "  if m.ContainsKey(5.50m) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
