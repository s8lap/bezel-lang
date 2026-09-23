// Codegen — issue #4642: `Set<Object>` did not lower AT ALL. The declaration
// `var s: Set<Object> := [];` silently produced no storage, every later read of
// `s` degraded to the #2434 zero-init fallback, and the program still linked
// and RAN, printing `added=False contains1=False count=0` (§8.6 / §8.4 /
// stdlib §4.10, v0.307).
//
// MECHANISM, re-derived on this lane's HEAD (556e2ee5) by probe rather than
// taken from the issue text — which gets the headline right and the boundary
// wrong. `new Set<Object>()` DOES lower: `lowerNew`'s array arm
// (LlvmBackend_Exprs2.cpp) lists `Set`/`OwningSet`, so it allocates a
// `bz_array_t` exactly as it does for a class element, and
// `registerCollectionDisposal` already classifies such a set array-backed (its
// `setDictBacked` test is the same `genericBackingType` call). What did not
// work was every predicate that has to answer "is this element slot a
// reference?" for `Object`:
//
//   1. `collectionElemIsClass` (LlvmBackend_Disposal.cpp) required
//      `Kind::Named` with a `TypeSymbol` declaration. `Object` is
//      `Kind::Primitive`, so `materializeArrayBackedSetLiteral`'s opening
//      `if (!collectionElemIsClass(setTy, 0)) return nullptr;` bailed, BOTH set
//      materialisers returned null, and `lowerLocalDecl`'s #559 "materialisation
//      failed; sema already diagnosed the type mismatch" guard bare-`return`ed:
//      no alloca, no `locals_` entry, NO DIAGNOSTIC. That guard's premise is
//      false here — sema does not reject `Set<Object>`; it is a documented,
//      legal type (stdlib §4.10 with §3.6's hashable rule).
//   2. `arrElemIsClass` (LlvmBackend_Calls.cpp), derived from
//      `elementPolicyFor`, answered false for the same reason, so every
//      array-backed `Set` handler stayed gated off. MEASURED with the `new`
//      spelling, which isolates this half from (1): `s.TryAdd(1)` did not lower
//      at all and its own local degraded to #2434.
//
// WHY THE OBVIOUS TWO-PREDICATE FIX IS A TRAP, and what the dedup rows below
// exist to pin. `emitRemoveElementEquality`'s step 1 (the class/interface arm)
// resolves `cls = (ty->kind == Named) ? ty->declaration : nullptr`, which is
// ALWAYS null for `Object`; `classEqualsOverride(null)` answers "no override",
// and step 1 RETURNS on every path once entered — at the class default,
// `CreateICmpEQ(el, target)`, RAW BOX-POINTER IDENTITY. Step 4's `Object` arm
// (#4573/#4619) is never reached. Two §8.6 boxes of the same `int` are distinct
// allocations, so with only the gate widenings landed `s.TryAdd(1)` twice would
// insert twice and `s.Contains(1)` would answer false against a freshly boxed
// query — #4619's `Array<Object>.Contains` defect reproduced one level up. The
// fix adds the `Object` arm to step 1, routing to the same `lowerObjectEquals`
// step 4 uses, so the two arms cannot disagree.
//
// AND THE INSERT SIDE NEEDS §8.6 BOXING — not mentioned in the issue at all.
// The array-backed set's slot is a POINTER (`collectionElemSize` gives 8, the
// scan loads `ptrTy()`), so `s.TryAdd(1)` handing the raw `i32` to
// `bz_array_add` with an 8-byte stride would read 4 bytes past a 4-byte alloca
// and then store a garbage pointer that the next `lowerObjectEquals`
// dereferences. `boxForTarget` (§8.6: "when a value type is used where `Object`
// … is expected, the value is boxed") is applied at the three set-insert arms;
// `emitArrayBackedSetInsert` additionally refuses a non-pointer item outright,
// so a caller added later without the boxing step cannot emit that IR.
//
// ⚠ MEASURED RESIDUAL, deliberately not discharged — the same trade #4619,
// #4639 and #4640 recorded for this family. Each §8.6 box minted here (one per
// inserted element, one per membership query) is 24 B and nothing discharges
// it: the stored element's box has no owner (a BORROWING `Set<T>` holds borrows
// and §28.11 v0.135 gives its slot a null disposer), and the query box is
// passed as the `other` argument of a vtable dispatch on an element loaded out
// of a heterogeneous `Object` slot, where #3480 measured an unconditional
// release as a heap-use-after-free. MEASURED, both sides, 300 iterations of
// `TryAdd` + `Contains` on a fresh `Set<Object>`, compiled `--sanitize=address`:
//
//   pre-fix    0 B / 0 allocations leaked, and the program computed 0 of 600
//              (nothing lowered — no box is minted on that path at all).
//   post-fix   14,400 B / 600 allocations leaked, 600 of 600 correct.
//
// i.e. exactly 24 B per box, two boxes per iteration. The leak-floor row below
// pins the residual's PRESENCE with a margined floor, never an exact count.
//
// KNOWN GAP, filed separately, NOT pinned here as a passing oracle: a NON-EMPTY
// `Set<Object>` literal (`var s: Set<Object> := [1, 2];`) is now REJECTED at
// compile time instead of silently producing an empty set — the literal
// materialiser lives in LlvmBackend_Setup.cpp, outside this lane's file grant,
// and still lowers its elements unboxed, so `emitArrayBackedSetInsert`'s
// pointer-slot refusal fires. Loud beats silently-wrong (§6c / the #3428
// precedent), and the one-line completion is
// `ev = boxForTarget(ev, elemRef, elemPtr.get());` before that call.
//
// §8.6 / §8.4 / §8.3 / §17.5 / §28.11 / stdlib §4.10 / §4.6, v0.307.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// A class WITH a user `Equals`/`GetHashCode` override keyed on X — the control
// shape `Set<T>`'s class-element dedup (#3491/#837) already handled and must
// keep handling byte-identically.
const char* kBox =
    "module M; class Box "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Box; class C ";

// leaksAtLeast — a margined leak-MAGNITUDE floor (never an exact byte count).
// The pre-fix program does not lower the set at all, mints no box and leaks
// nothing, so this returns false on that side — which is what FAIL-WITHOUT
// requires of the residual row.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding at all — wrongly clean
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;  // fired, but not with a leak SUMMARY (a crash/UAF) — fails
}
}  // namespace

// ── The issue's own repro ───────────────────────────────────────────────────

// Issue #4642 verbatim: the empty `Set<Object>` literal, one `TryAdd`, one
// `Contains`, one `Count`. MEASURED pre-fix: three `internal: … #2434` notes
// and `added=False contains1=False count=0`. `Count` being 0 after a successful
// `TryAdd` is what the issue cites to separate this from the #4619 membership
// family — nothing about the container worked.
BEZEL_TEST(set_object_element_4642, empty_literal_tryadd_contains_count_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := []; "
        "  var added: bool := s.TryAdd(1); "
        "  var c1: bool := s.Contains(1); "
        "  var n: int := s.Count; "
        "  if added and c1 and n = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The `new Set<Object>()` spelling — the isolator that separates the literal
// half (defect 1, `collectionElemIsClass`) from the handler half (defect 2,
// `arrElemIsClass`). MEASURED pre-fix: the set itself lowered fine, but the
// `TryAdd` local carried its own `#2434` note and read `False`.
BEZEL_TEST(set_object_element_4642, new_constructor_form_tryadd_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := new Set<Object>(); "
        "  var added: bool := s.TryAdd(1); "
        "  var c1: bool := s.Contains(1); "
        "  if added and c1 and s.Count = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Dedup: the row that pins `emitRemoveElementEquality`'s Object arm ───────

// stdlib §4.10: `TryAdd` "returns whether added (false if duplicate)". With the
// two gate widenings but WITHOUT step 1's `Object` arm, the scan compares two
// distinct §8.6 box pointers with `icmp eq` and the second `TryAdd` inserts:
// this row reads `again == true` and `Count == 2`. Both halves are asserted so
// neither a wrong bool nor a wrong count can slip through.
BEZEL_TEST(set_object_element_4642, tryadd_duplicate_rejected_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := []; "
        "  var first: bool := s.TryAdd(1); "
        "  var again: bool := s.TryAdd(1); "
        "  if first and (not again) and s.Count = 1 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.10 through the inherited `ICollection<T>.Add`, plus §17.5's `x in coll`
// over an array-backed set (which reaches the linear scan through
// `collectionElemIsClass`, the OTHER widened predicate, in a different file).
BEZEL_TEST(set_object_element_4642, add_dedups_and_in_operator_spec_17_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := []; "
        "  s.Add(1); s.Add(1); s.Add(2); "
        "  if (1 in s) and (2 in s) and (not (5 in s)) and s.Count = 2 "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The ABSENT control, so a fix that answered `true` unconditionally — or that
// made `Contains` compare nothing at all — cannot pass the present rows.
BEZEL_TEST(set_object_element_4642, contains_absent_is_false_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := []; "
        "  var _a: bool := s.TryAdd(1); "
        "  if s.Contains(5) or s.Contains(0) then return 0; end; "
        "  if s.Contains(1) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.10 removal: `Remove` shares `emitRemoveElementEquality` with the dedup
// scan, so it exercises the same Object arm from a third caller — and the
// surviving element is asserted, not just the count.
BEZEL_TEST(set_object_element_4642, remove_matching_element_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := []; "
        "  var _a: bool := s.TryAdd(1); "
        "  var _b: bool := s.TryAdd(2); "
        "  var gone: bool := s.Remove(1); "
        "  if gone and (not s.Contains(1)) and s.Contains(2) and s.Count = 1 "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §4.6 content equality reached dynamically: a `string` stored in an `Object`
// slot is a bare `bz_string*`, not a box, and `lowerObjectEquals` answers
// content equality for it. Two equal strings must therefore collapse to ONE
// element — the row that proves the dispatch is `lowerObjectEquals` and not a
// pointer compare wearing its name.
BEZEL_TEST(set_object_element_4642, string_element_content_equality_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := []; "
        "  var a: bool := s.TryAdd(\"hi\"); "
        "  var b: bool := s.TryAdd(\"hi\"); "
        "  if a and (not b) and s.Contains(\"hi\") and s.Count = 1 "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The PRE-BOXED control: a query/element already typed `Object` needs no box,
// and a fix that double-boxed an already-reference source would break exactly
// this row (§8.6's rule applies to a VALUE source only).
BEZEL_TEST(set_object_element_4642, preboxed_object_source_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: Set<Object> := []; "
        "  var o: Object := 1; "
        "  var p: Object := 5; "
        "  var a: bool := s.TryAdd(o); "
        "  if a and s.Contains(o) and (not s.Contains(p)) and s.Count = 1 "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Controls: the four array/list families at <Object> must be unaffected ────
//
// `arrElemIsClass` and `collectionElemIsClass` are SHARED with every
// array-backed and node-backed collection, so widening them re-routes
// `Array`/`Stack`/`Queue`/`LinkedList` at `<Object>` from
// `emitRemoveElementEquality`'s step 4 to its step 1. The two arms call the
// same `lowerObjectEquals`, and these rows are what proves it: each was correct
// before this fix (#4619 / #4639) and must still be.

// Array<Object>: `.Contains` present + absent, `.IndexOf`, `x in a`, `.Count`.
BEZEL_TEST(set_object_element_4642, array_object_membership_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  if a.Contains(1) and a.Contains(2) and (not a.Contains(5)) "
        "     and a.IndexOf(2) = 1 and a.IndexOf(5) = -1 "
        "     and (1 in a) and (not (5 in a)) and a.Count = 2 "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Array<Object>.Remove — the third `emitRemoveElementEquality` caller, checked
// by its effect on the surviving contents rather than only on Count.
BEZEL_TEST(set_object_element_4642, array_object_remove_control_spec_25_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Object> := [1, 2]; "
        "  var gone: bool := a.Remove(1); "
        "  if gone and a.Count = 1 and a.Contains(2) and (not a.Contains(1)) "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Stack<Object> and Queue<Object>: the same shared `arrElemIsClass` dispatch on
// two more array-backed heads.
BEZEL_TEST(set_object_element_4642, stack_queue_object_membership_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var st: Stack<Object> := new Stack<Object>(); "
        "  st.Push(1); st.Push(2); "
        "  var q: Queue<Object> := new Queue<Object>(); "
        "  q.Enqueue(7); "
        "  if st.Contains(2) and (not st.Contains(9)) and st.Count = 2 "
        "     and q.Contains(7) and (not q.Contains(8)) and q.Count = 1 "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// LinkedList<Object>: node-backed, and the ONE control that goes through
// `collectionElemIsClass` (the `x in coll` arm in LlvmBackend_Exprs.cpp) rather
// than through `arrElemIsClass` — #4639's family, re-pinned here.
BEZEL_TEST(set_object_element_4642, linkedlist_object_membership_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ll: LinkedList<Object> := new LinkedList<Object>(); "
        "  ll.AddLast(3); ll.AddLast(4); "
        "  if ll.Contains(3) and (not ll.Contains(9)) and (3 in ll) "
        "     and (not (9 in ll)) and ll.Count = 2 "
        "  then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Controls: the element families whose classification did NOT move ────────

// A CLASS element with a user `Equals` override — #3491/#837's contract. This
// is the shape `arrElemIsClass` already admitted, so it must be byte-identical:
// two distinct but content-equal instances collapse to one.
BEZEL_TEST(set_object_element_4642, set_class_element_dedup_control_spec_8_3) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var a := new Box(1); "
        "    using var b := new Box(1); "
        "    var s := new Set<Box>(); "
        "    var f: bool := s.TryAdd(a); "
        "    var g: bool := s.TryAdd(b); "
        "    if f and (not g) and s.Contains(a) and s.Count = 1 "
        "    then return 1; end; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// A hash-backed `Set<int>` — `genericBackingType` admits `int`, so this set is
// a `bz_dict_t` and reaches NONE of the array-backed code this fix touches.
// Here to prove the widening did not leak into the dict-backed family.
BEZEL_TEST(set_object_element_4642, set_int_hash_backed_control_spec_4_10) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var si: Set<int> := [1, 2, 2]; "
        "  var a: bool := si.TryAdd(3); "
        "  var b: bool := si.TryAdd(1); "
        "  if a and (not b) and si.Count = 3 and si.Contains(2) "
        "     and (not si.Contains(9)) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── The §8.6 residual, loop-amplified ──────────────────────────────────────

// Per this family's discipline: loop-amplify (a one-shot repro can leave the
// allocation reachable from a live stack slot and pass on BOTH sides) and
// assert a FLOOR, never an exact count. MEASURED on this lane, 300 iterations,
// `--sanitize=address`: 14,400 B in 600 allocations — 24 B per box, two boxes
// per iteration (the inserted element's, and the `Contains` query's). The
// 12,000 B floor sits comfortably below. Pre-fix the loop lowers nothing, mints
// no box and reports NO leak summary, so `leaksAtLeast` is false on that side.
BEZEL_TEST(set_object_element_4642, tryadd_contains_box_residual_floor_spec_8_6) {
    BEZEL_EXPECT_TRUE(leaksAtLeast(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 300 do "
        "    var s: Set<Object> := []; "
        "    var a: bool := s.TryAdd(i); "
        "    if a then acc := acc + 1; end; "
        "    if s.Contains(i) then acc := acc + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  return acc mod 256; "
        "end Main; end C;", 12000));
}
