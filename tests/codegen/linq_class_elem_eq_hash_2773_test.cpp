// Regression tests for #2773 rows A1 + A2 — the LINQ set-like operators
// compared a CLASS/INTERFACE element by reading its object bytes as a
// `%bz.string`.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §5 ("Key and equality contracts",
// v0.195) states that "`Distinct`, `Union`, `Intersect`, `Except`, `Contains`,
// and `ToSet` require **hashable** `T` … enforced at the operator's
// instantiation site exactly as for `Set<T>`/`Dictionary<K, V>`", and spec
// §3.6's hashable row admits "every type except (a) a record that is
// neither auto-hashable nor declares user equality, and (b) a choice
// that is not value-like auto-hashable" — so a CLASS and an INTERFACE
// are admitted element types and these operators must work on them.
//
// What equality then MEANS is spec §8.3's table — for a class, "Reference
// equality" with an "Identity-based" `GetHashCode` — refined by §8.4: "For
// classes, `=` always invokes `Equals`. … classes customize equality by
// overriding `Equals`."  So: a user `Equals`/`GetHashCode` override when the
// class declares one, otherwise reference identity plus the §8.3 assigned
// identity hash.  That is exactly what `Set<C>`'s own membership scan already
// does (emitArrayBackedSetInsert → emitRemoveElementEquality), which is what
// stdlib §5's "exactly as for `Set<T>`" points at.
//
// THE DEFECT.  `buildLinqTrampoline` kind 3 (element equality) classified the
// element by its LOWERED type alone — `elemTy->isPointerTy()` ⇒ `emitStringEq`
// — consulting no `TypeRef` at all.  A class, an interface, `Object` and a
// boxed value all lower to `ptr`, so `loadStringLen`/`loadStringData` ran over
// the object's own bytes.  MEASURED on the pre-fix compiler:
//
//     ==...==ERROR: AddressSanitizer: heap-buffer-overflow ...
//     READ of size 1 at 0x... thread T0
//         #0 ... in bz$linq$tramp
//         #1 ... in bz_lazy_movenext(bz_lazy_t*)
//         #2 ... in bz_lazy_to_array
//     0x... is located 0 bytes after 16-byte region [...)
//
// `buildLinqHashTrampoline` was the other half: a class/interface element fell
// to a raw `ptrtoint` hash while the equality trampoline content-compared
// garbage — inconsistent (violating §8.3's `a.Equals(b)` ⇒ equal hashes) as
// well as unsafe.  The two are fixed as a pair: equality now dispatches
// through `lowerObjectEquals` (vtable[kEqualsSlot]) and the hash through
// `lowerObjectGetHashCode` (vtable[kGetHashCodeSlot]), the two halves of the
// same §8.3 pair.
//
// THE GUARD IS CATEGORY-BLIND, deliberately.  It does not enumerate the source
// categories that may arrive (the enumeration is what produced this family —
// one predicate was found missing Interface, then string, then builtin
// interfaces, then decimal).  It tests the one property the `%bz.string`
// layout path actually needs — "the sema TypeRef PROVES this is `string`" —
// and everything else goes to `lowerObjectEquals`, which decides at RUN time
// (null → reference identity, a genuine string → content equality, otherwise
// the vtable slot).  A null or unresolved `elemRef` therefore lands on a path
// that is correct for every element shape, string included.
//
// KNOWN RESIDUAL (measured, not inferred).  Across `--import` a class element
// is still broken, and NOT by anything in this fix: the importing module's
// `bz$vtable$table` does not list an imported public class's `vt$<Mod>.<Type>`
// global (`nm` on the importing object shows no reference to it at all), so
// `bz_obj_is_string`'s exclusion heuristic — "word 0 is not a known vtable,
// therefore this is a string" — calls every instance of an imported class a
// string.  Both before and after this fix the cross-module program faults with
// a heap-buffer-overflow; only the fault site moves.  Filed separately; the
// tests here are single-module.
//
// NOT ADDRESSED HERE (also measured): `ToSet()` over a class element leaks its
// backing table — `bz_lazy_to_set` always builds a dict-backed set while
// `Set<C>` is otherwise ARRAY-backed (emitArrayBackedSetInsert), so the two
// disagree.  That is a build-mode/read-mode split in the `Set` family, not in
// these trampolines, so no `ToSet`-over-a-class test appears below.  Filed
// separately.
//
// Spec: §8.3, §8.4, §3.6; stdlib §5.  Revision v0.240.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// A plain class: NO `Equals`/`GetHashCode` override, so §8.3's reference
// identity and identity hash govern.
const char* kBox =
    "module M; import System; "
    "class Box "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Box; ";

// A class that DOES override `Equals`/`GetHashCode`, keyed on X — §8.4's
// "classes customize equality by overriding `Equals`".
const char* kKey =
    "module M; import System; "
    "class Key "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Key then var k := other as Key; return k!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Key; ";

// An interface-typed element over a class that overrides neither.
const char* kShape =
    "module M; import System; "
    "interface IShape method Area(): int; end IShape; "
    "class Sq: IShape "
    "  public S: int; "
    "  public constructor(s: int) S := s; end constructor; "
    "  public method Area(): int => S * S; "
    "end Sq; ";

std::string main_(const char* fixture, const std::string& body) {
    return std::string(fixture) + "class App public static method Main(): int " +
           body + " end Main; end App;";
}
}  // namespace

// ===========================================================================
// A1 — element equality (`Distinct`)
// ===========================================================================

// spec §8.3 ("| Class | Reference equality | Identity-based |"): three
// SEPARATE `Box` objects that happen to wrap the same `1` are three distinct
// elements — the class declares no `Equals`, so nothing may look at contents.
//
// This is the load-bearing case: pre-fix the trampoline read each Box as a
// `%bz.string` (heap-buffer-overflow under ASan; a garbage verdict without).
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           distinct_class_no_override_is_reference_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(main_(kBox,
        "  using var b0 := new Box(1); using var b1 := new Box(1); "
        "  using var b2 := new Box(1); "
        "  using var a := new Array<Box>(); "
        "  a.Add(b0); a.Add(b1); a.Add(b2); "
        "  using var d: Array<Box> := a.Distinct().ToArray(); "
        "  return d.Count;")), 3);
}

// spec §8.4 ("For classes, `=` always invokes `Equals`. … classes customize
// equality by overriding `Equals`"): with the override in place, the two
// `Key(1)` objects ARE equal and collapse; `Key(2)` stays.
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           distinct_class_equals_override_is_honoured_spec_8_4) {
    BEZEL_EXPECT_EQ(run(main_(kKey,
        "  using var k0 := new Key(1); using var k1 := new Key(1); "
        "  using var k2 := new Key(2); "
        "  using var a := new Array<Key>(); "
        "  a.Add(k0); a.Add(k1); a.Add(k2); "
        "  using var d: Array<Key> := a.Distinct().ToArray(); "
        "  return d.Count;")), 2);
}

// spec §3.6 (hashable admits an interface) + §8.3: a statically
// INTERFACE-typed element is the shape the #2768 family keeps re-acquiring.
// `Sq` overrides nothing, so the three references stay distinct.
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           distinct_interface_element_is_reference_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(main_(kShape,
        "  using var s0 := new Sq(2); using var s1 := new Sq(2); "
        "  using var s2 := new Sq(3); "
        "  var a: Array<IShape> := [s0, s1, s2]; "
        "  using var d: Array<IShape> := a.Distinct().ToArray(); "
        "  return d.Count;")), 3);
}

// ===========================================================================
// A2 — the hash half (`Union` / `Intersect` / `Except` build a membership
// dict, so BOTH trampolines run and must agree)
// ===========================================================================

// stdlib §5 ("`Distinct` and `Union` yield first occurrences in encounter
// order … `Intersect` and `Except` yield the retained elements, made distinct,
// in first-input order") over class elements with a content `Equals`/
// `GetHashCode` pair.
//
// This test PINS A2 specifically.  The two `Key(2)` objects are DIFFERENT
// objects, so `Intersect` can only find them via the override's content hash:
// with the pre-fix `ptrtoint` hash they land in different buckets and the
// equality trampoline is never even consulted, so `Intersect` would be empty
// regardless of what A1 does.  §8.3's "if `a.Equals(b)` then
// `a.GetHashCode() = b.GetHashCode()`" is exactly what is being checked.
//
// Union {1,2,3} = 3, Intersect {2} = 1, Except {1} = 1 → 3*25 + 1*5 + 1 = 81.
// (Encoded well under 255: `runAot`'s exitCode is WEXITSTATUS-masked.)
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           union_intersect_except_class_hash_agrees_with_eq_spec_8_3) {
    BEZEL_EXPECT_EQ(run(main_(kKey,
        "  using var a0 := new Key(1); using var a1 := new Key(2); "
        "  using var b0 := new Key(2); using var b1 := new Key(3); "
        "  using var a := new Array<Key>(); a.Add(a0); a.Add(a1); "
        "  using var b := new Array<Key>(); b.Add(b0); b.Add(b1); "
        "  using var u: Array<Key> := a.Union(b).ToArray(); "
        "  using var i: Array<Key> := a.Intersect(b).ToArray(); "
        "  using var e: Array<Key> := a.Except(b).ToArray(); "
        "  return u.Count * 25 + i.Count * 5 + e.Count;")), 81);
}

// The same three operators over a class with NO override: §8.3 identity.
// `b1` is the SAME reference in both inputs, so the identity hash must agree
// with itself (Union {b0,b1,b2} = 3, Intersect {b1} = 1, Except {b0} = 1).
//
// HONEST NOTE: this one passes on the PRE-FIX compiler too, and it is kept as
// an over-correction control rather than as a pin.  It is not vacuous, but it
// is not load-bearing either: with only same-pointer pairs ever compared,
// `emitStringEq`'s `l == r` fast path returns true before dereferencing
// anything, so the old code got the right answer by never reaching the bad
// load.  The four tests above are the ones that fail without the fix.
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           union_intersect_except_class_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(main_(kBox,
        "  using var b0 := new Box(1); using var b1 := new Box(1); "
        "  using var b2 := new Box(1); "
        "  using var a := new Array<Box>(); a.Add(b0); a.Add(b1); "
        "  using var b := new Array<Box>(); b.Add(b1); b.Add(b2); "
        "  using var u: Array<Box> := a.Union(b).ToArray(); "
        "  using var i: Array<Box> := a.Intersect(b).ToArray(); "
        "  using var e: Array<Box> := a.Except(b).ToArray(); "
        "  return u.Count * 25 + i.Count * 5 + e.Count;")), 81);
}

// ===========================================================================
// Over-correction control — the `string` element must keep §4.6 CONTENT
// equality on every one of these operators.  The fix narrows the
// `%bz.string`-layout path, so this is where a too-wide narrowing would show.
// ===========================================================================

// Distinct 2, Union {apple,banana,cherry} = 3, Intersect {banana} = 1,
// Except {apple} = 1, ToSet 2 → 2*100 + 3*25 + 1*5 + 1*2 + 2 = 284 … too wide
// for the 8-bit exit code, so encode inside 255: 2*50 + 3*20 + 1*10 + 1*5 + 2
// = 177.
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           string_element_keeps_content_equality_spec_4_6) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; ",
        "  using var a := new Array<string>(); "
        "  a.Add(\"apple\"); a.Add(\"banana\"); a.Add(\"apple\"); "
        "  using var b := new Array<string>(); "
        "  b.Add(\"banana\"); b.Add(\"cherry\"); "
        "  using var d: Array<string> := a.Distinct().ToArray(); "
        "  using var u: Array<string> := a.Union(b).ToArray(); "
        "  using var i: Array<string> := a.Intersect(b).ToArray(); "
        "  using var e: Array<string> := a.Except(b).ToArray(); "
        "  using var s := a.ToSet(); "
        "  return d.Count * 50 + u.Count * 20 + i.Count * 10 + "
        "         e.Count * 5 + s.Count;")), 177);
}

// The record element (issue D6's path) is untouched by this fix and must stay
// structural: two equal `Pt(1)` collapse (spec §9.2 auto-hashable).
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           record_element_stays_structural_spec_9_2) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; record Pt(x: int); ",
        "  using var a := new Array<Pt>(); "
        "  a.Add(new Pt(1)); a.Add(new Pt(2)); a.Add(new Pt(1)); "
        "  using var d: Array<Pt> := a.Distinct().ToArray(); "
        "  return d.Count;")), 2);
}

// The `int` element must stay a plain value compare (the `else` arm).
BEZEL_TEST(linq_class_elem_eq_hash_2773,
           int_element_stays_value_equality_spec_8_3) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; ",
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(1); a.Add(3); "
        "  using var d: Array<int> := a.Distinct().ToArray(); "
        "  return d.Count;")), 3);
}
