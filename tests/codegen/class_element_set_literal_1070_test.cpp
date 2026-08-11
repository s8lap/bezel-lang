// Codegen — issue #1070 (stdlib §4.10 `Set<T>`/`OwningSet<T>`, spec §15.4
// collection literals, §17.5 `in`, §29 iteration): a CLASS-element
// `Set<C>`/`OwningSet<C>` built from a sequence literal was a SILENT
// MISCOMPILE — every read returned empty.
//
// Verified root cause (probes on main before the fix):
//   `using var s: OwningSet<Item> := [new Item(5), new Item(6)]; s.Count` -> 0
//   `s.Contains(equalItem)`                                              -> false
//   `for var x in s do … end`  -> COMPILE ERROR "not yet implemented:
//                                 codegen for this for statement"  (issue #1069)
// while the SAME set built with `new OwningSet<Item>()` + `.Add(…)` was fully
// correct (Count 2, Contains true, for-in iterating) — so the defect was in the
// LITERAL path only, not in the reads.
//
// `materializeHeapSetLiteral` required `genericBackingType(setTy, 0, isSet=true)`
// to be non-null, i.e. an element the generic-key HASH table supports (record,
// primitive, enum, string). It returns null for every CLASS — and a class-element
// set is not hash-backed at all: `new Set<C>()` / `new OwningSet<C>()` fall
// through `lowerNew` to `bz_array_alloc`, and `.Add` dedup-appends into that
// `bz_array_t` with a linear §8.3-equality scan (#837). The materializer returned
// null for that shape, and `lowerLocalDecl`'s #559 guard turned the null into
// `return;  // materialization failed; safe no-op` — so the local's SLOT WAS
// NEVER CREATED. Every later read of `s` therefore saw a null/absent backing:
// `bz_array_len(null)` = 0, `Contains` false, and for-in could not lower at all.
// The #559 guard's comment assumed "a sema error already diagnosed the type",
// which is untrue for a class element — that program is perfectly legal.
//
// Fix: `materializeArrayBackedSetLiteral` — the class/interface half of
// `materializeHeapSetLiteral`. It allocates the SAME `bz_array_t` `new Set<C>()`
// allocates and inserts each element through the SAME dedup helper `.Add` uses
// (`emitArrayBackedSetInsert`, extracted from the #837/#1474 lambda), so a
// literal-built class-element set is bit-for-bit what `new` + `.Add` builds and
// every existing read (`Count` -> bz_array_len, `Contains`/`in` -> the linear
// scan, for-in -> lowerArrayFor, scope-end disposal -> bz_array_free with the
// element disposer) already agrees with it.
//
// fail-without / pass-with: every test below FAILS on the pre-fix compiler —
// the Count/Contains/`in` cases return 0/false (the wrong VALUE, which is why
// they assert the value, not just that the program runs), and the for-in cases
// do not compile at all (mapped to the -999 sentinel). All pass with the fix.
// // per stdlib §4.10, spec §15.4, §17.5, §29 (issue #1070)

#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// -999 on any sanitizer fire (leak / UAF / double-free), else the real exit
// code — one EXPECT_EQ pins both memory-cleanliness and the computed value.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// A plain class element: §8.3 reference identity (no Equals override).
const char* kItem =
    "module M; class Item "
    "  public V: int := 0; "
    "  public constructor(v: int) this.V := v; end constructor; "
    "  destructor end destructor; "
    "end Item; class C ";

// A class element with a user Equals/GetHashCode keyed on X (§8.3/§8.4), plus a
// destructor counter so disposal can be observed.
const char* kBox =
    "module M; class Box "
    "  public static _n: int := 0; "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor _n := _n + 1; end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Box then var b := other as Box; return b!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Box; class C ";
}  // namespace

// ===========================================================================
// stdlib §4.10 + spec §15.4 — `Count` over a class-element set LITERAL
// ===========================================================================

// The issue's own repro. Pre-fix: 0 (the declaration lowered to nothing).
BEZEL_TEST(class_element_set_literal_1070, owning_set_literal_count_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kItem) +
        "public static method Main(): int "
        "  do using var s: OwningSet<Item> := [new Item(5), new Item(6)]; "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// The borrowing sibling: `Set<C>` holds borrows of two `using var` locals.
// Pre-fix: 0.
BEZEL_TEST(class_element_set_literal_1070, borrowing_set_literal_count_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kItem) +
        "public static method Main(): int "
        "  do using var a := new Item(5); using var b := new Item(6); "
        "    using var s: Set<Item> := [a, b]; "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// ===========================================================================
// stdlib §4.10 + spec §29 — `for`-in over a class-element set LITERAL (#1069's
// "for-in over a Set is unimplemented" half shares this root cause)
// ===========================================================================

// Pre-fix: "not yet implemented: codegen for this for statement" — lowerArrayFor
// declined because `lowerExpr(s)` found no slot for the never-materialized local.
BEZEL_TEST(class_element_set_literal_1070, owning_set_literal_forin_spec_29) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kItem) +
        "public static method Main(): int "
        "  var n: int := 0; "
        "  do using var s: OwningSet<Item> := [new Item(5), new Item(6)]; "
        "    for var x in s do n := n + x.V; end; end; "
        "  return n; end Main; end C;"), 11);
}

// Same for the borrowing form. Pre-fix: the same compile error.
BEZEL_TEST(class_element_set_literal_1070, borrowing_set_literal_forin_spec_29) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kItem) +
        "public static method Main(): int "
        "  var n: int := 0; "
        "  do using var a := new Item(5); using var b := new Item(6); "
        "    using var s: Set<Item> := [a, b]; "
        "    for var x in s do n := n + x.V; end; end; "
        "  return n; end Main; end C;"), 11);
}

// ===========================================================================
// stdlib §4.10 + §8.3 — membership over a class-element set LITERAL
// ===========================================================================

// `Contains` dispatches the element's user `Equals` (§8.4): a fresh
// content-equal query is found. Pre-fix: false (empty backing) -> 0.
BEZEL_TEST(class_element_set_literal_1070, owning_set_literal_contains_spec_4_10) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s: OwningSet<Box> := [new Box(7)]; "
        "    using var q := new Box(7); "
        "    if s.Contains(q) then return 1; end; return 0; end; "
        "  return 0; end Main; end C;"), 1);
}

// spec §17.5: `x in set` over a class-element set literal is the same scan.
// Pre-fix: false -> 0.
BEZEL_TEST(class_element_set_literal_1070, owning_set_literal_in_operator_spec_17_5) {
    BEZEL_EXPECT_EQ(run(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s: OwningSet<Box> := [new Box(7)]; "
        "    using var q := new Box(7); "
        "    if q in s then return 1; end; return 0; end; "
        "  return 0; end Main; end C;"), 1);
}

// ===========================================================================
// stdlib §4.10 — the literal's dedup contract: "duplicates (per T's equality
// contract) are silently collapsed, the first textual occurrence retained"
// ===========================================================================

// `[Box(1), Box(1), Box(2)]` collapses to {1, 2}. Pre-fix: 0.
BEZEL_TEST(class_element_set_literal_1070, owning_set_literal_dedup_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s: OwningSet<Box> := "
        "       [new Box(1), new Box(1), new Box(2)]; "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// stdlib §4.10: "For `OwningSet<T>`, each later duplicate's owned element is
// disposed AT CONSTRUCTION". Observed as `Box._n == 1` while the set is still
// live, then 3 after scope end (1 duplicate + 2 retained) — i.e. every element
// is disposed EXACTLY once. The two dispose sites for an element slot are (a)
// the literal's duplicate branch (this element is never appended, so the
// backing never holds it) and (b) the scope-end bz_array_free element disposer
// (only appended elements); they are disjoint, so no slot is freed twice —
// which runAotChecked's ASan/LSan net independently confirms.
// Pre-fix: 0 (nothing was constructed into a backing at all).
BEZEL_TEST(class_element_set_literal_1070, owning_set_literal_dup_disposed_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kBox) +
        "public static method Main(): int "
        "  var c: int := 0; "
        "  do using var s: OwningSet<Box> := "
        "       [new Box(1), new Box(1), new Box(2)]; "
        "    c := s.Count; "
        "    if Box._n <> 1 then return 90 + Box._n; end; end; "
        "  return c * 10 + Box._n; end Main; end C;"), 23);
}

// The BORROWING `Set<C>` disposes nothing — its elements are borrows whose
// `using var` owners dispose them (stdlib §4.10 / spec §28.11 borrowing
// column). Count still dedups to 1 by the user `Equals`; `Box._n` is 0 while
// the set is live. Pre-fix: 0 * 10 + 0 = 0.
BEZEL_TEST(class_element_set_literal_1070, borrowing_set_literal_no_dispose_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kBox) +
        "public static method Main(): int "
        "  var c: int := 0; var d: int := 0; "
        "  do using var a := new Box(1); using var b := new Box(1); "
        "    using var s: Set<Box> := [a, b]; "
        "    c := s.Count; d := Box._n; end; "
        "  return c * 10 + d; end Main; end C;"), 10);
}

// ===========================================================================
// spec §15.4 L2170 / §28.3 — a throwing element expression
// ===========================================================================

// "as for an `OwningArray<U>` literal": when an element expression throws
// partway, the elements already inserted AND the partially-filled backing are
// disposed on the unwind edge. Box(1) is in the backing when Boom() throws, so
// exactly 1 destructor call — and runAotChecked proves the partial backing did
// not leak. Pre-fix: the literal built nothing, so `Box._n` was 0.
BEZEL_TEST(class_element_set_literal_1070, owning_set_literal_throw_partial_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class AppError: Exception end AppError; "
        "class Box "
        "  public static _n: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end Box; class C "
        "public static method Boom(): owned Box throw new AppError(); end Boom; "
        "public static method Main(): int "
        "  try do using var s: OwningSet<Box> := [new Box(1), C.Boom()]; "
        "    return 0; end; "
        "  catch AppError => return Box._n; end; "
        "  return -1; end Main; end C;"), 1);
}

// ===========================================================================
// Regressions the fix must NOT disturb
// ===========================================================================

// A VALUE element keeps the hash (dict) backing and its §4.10 dedup — the #559
// path is untouched by the class-element branch. `[1, 2, 2, 3]` -> {1, 2, 3}.
BEZEL_TEST(class_element_set_literal_1070, value_element_set_literal_unchanged_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C public static method Main(): int "
        "  do var s: Set<int> := [1, 2, 2, 3]; return s.Count; end; "
        "  return 0; end Main; end C;"), 3);
}

// `new` + `.Add` on a class-element OwningSet still behaves exactly as #837
// established — the shared insert helper was extracted, not changed.
BEZEL_TEST(class_element_set_literal_1070, new_plus_add_still_dedups_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kBox) +
        "public static method Main(): int "
        "  do using var s := new OwningSet<Box>(); "
        "    s.Add(move (new Box(1))); s.Add(move (new Box(1))); "
        "    s.Add(move (new Box(2))); "
        "    return s.Count; end; "
        "  return 0; end Main; end C;"), 2);
}

// An `OwningArray<C>` literal must still APPEND (no set dedup) — the new
// materializer branch is gated on the Set/OwningSet name head.
BEZEL_TEST(class_element_set_literal_1070, owning_array_literal_still_appends_spec_4_5) {
    BEZEL_EXPECT_EQ(runChecked(std::string(kBox) +
        "public static method Main(): int "
        "  do using var a: OwningArray<Box> := [new Box(1), new Box(1)]; "
        "    return a.Count; end; "
        "  return 0; end Main; end C;"), 2);
}
