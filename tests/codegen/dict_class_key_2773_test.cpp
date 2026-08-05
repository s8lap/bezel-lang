// Regression tests for #2773 rows A3 + A4 — a CLASS / INTERFACE / `Object`
// keyed `Dictionary` was built AND read in `string` mode, GEPing the key
// reference as a `%bz.string`.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §4.8 (v0.158): "`K` must be
// **equality-bearing** (spec §3.6/§8.3) — enforced at the instantiation site
// per §3.6 rule 2: a `Dictionary<K, V>` (or `OwningDictionary<K, V>`) whose
// `K` is a record that is neither auto-equality-bearing nor declares user
// equality is rejected".  Spec §3.6's equality-bearing row admits every type
// except an ineligible record or choice, and sema's own `isEqualityBearing`
// says so verbatim — "class (reference identity), interface, enum — all bear
// consistent Equals/GetHashCode" — so a class-keyed dictionary is legal and
// REACHABLE, not latent.
//
// Equality is then spec §8.3's table ("| Class | Reference equality |
// Identity-based |") refined by §8.4 ("For classes, `=` always invokes
// `Equals`. … classes customize equality by overriding `Equals`").
//
// Key ownership is settled too, which is why the generic backing can store the
// bare reference: stdlib §4.8 states "A class-typed key/element is held by
// **borrow** (the §28.11 borrowing column), so its owner may still mutate it
// while it is stored", and §4.9's `OwningDictionary` surface marks only the
// VALUE owned — `public method Add(key: K, owned value: V)`.
//
// THE DEFECT.  `genericBackingType` admitted only record / non-string
// primitive / enum / choice, so a class or interface key returned null and
// fell to the STRING fast path — at allocation (`bz_dict_alloc` instead of
// `bz_dict_alloc_generic`, row A4) and at every lookup (`bz_dict_*_str`, row
// A3).  MEASURED on the pre-fix compiler for
// `new Dictionary<Box,int>(); d.Add(b0, 10);`:
//
//     ==...==ERROR: AddressSanitizer: SEGV on unknown address 0x000000f0f000
//     #0 ... in fnv1a(char const*, long)
//     #1 ... in bz_dict_try_get_str
//     #2 ... in bz_dict_contains_str
//
// The in-code comment claiming `genericBackingType` is "null only for the
// string key" was simply false.
//
// BUILD MODE vs READ MODE.  The `GetEnumerator` sites
// (LlvmBackend_Assign.cpp, LlvmBackend_Calls2.cpp) classify with a REAL
// TypeRef string test, so they already read a class-keyed dictionary in
// GENERIC mode while everything else built it in STRING mode.  The fix
// converges the two on **generic**, which is the mode the read side already
// used; the `for var e in d` test below is the one that pins that agreement.
//
// THE GUARD.  `hasObjectHeaderEquality` names the property the fix needs — a
// value carrying the §8.3 object header, whose vtable slots kEqualsSlot /
// kGetHashCodeSlot hold its `Equals`/`GetHashCode` pair.  It is deliberately
// NOT "lowers to `ptr`": a closure, a `Shared<T>`/`Weak<T>` handle, an
// `IGrouping`, a `Subscription` and a nested builtin collection are all
// pointer-shaped with no vtable at offset 0.
//
// KNOWN RESIDUAL (measured, not inferred).  Across `--import` this shape is
// still broken, and NOT by anything in this fix: the importing module's
// `bz$vtable$table` does not list an imported public class's `vt$<Mod>.<Type>`
// global (`nm` on the importing object shows no reference to it), so
// `bz_obj_is_string`'s exclusion heuristic calls every instance of an imported
// class a string.  Pre-fix and post-fix the cross-module program faults; only
// the fault site moves.  Filed separately; the tests here are single-module.
//
// Spec: §3.6, §8.3, §8.4; stdlib §4.8, §4.9, §15.4.  Revision v0.240.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// A plain class key: §8.3 reference identity + identity hash.
const char* kBox =
    "module M; import System; "
    "class Box "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Box; ";

// A class key that overrides the §8.3 pair, keyed on X.
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
// A3 + A4 — the key surface
// ===========================================================================

// The issue's own shape, plus the rest of the §4.8 lookup surface.  Pre-fix
// this SEGV'd on the very first `.Add`.  Each check adds 1; all 6 must hold.
BEZEL_TEST(dict_class_key_2773,
           dictionary_class_key_reference_identity_spec_8_3) {
    BEZEL_EXPECT_EQ(run(main_(kBox,
        "  using var b0 := new Box(1); using var b1 := new Box(2); "
        "  using var b2 := new Box(3); "
        "  using var d := new Dictionary<Box, int>(); "
        "  d.Add(b0, 10); d.Add(b1, 20); "
        "  var n := 0; "
        "  if d.ContainsKey(b0) then n := n + 1; end; "
        "  if not d.ContainsKey(b2) then n := n + 1; end; "
        "  if d[b1] = 20 then n := n + 1; end; "
        "  d[b2] := 30; "
        "  if d.Count = 3 then n := n + 1; end; "
        "  if d.Remove(b0) then n := n + 1; end; "
        "  if d.Count = 2 then n := n + 1; end; "
        "  return n;")), 6);
}

// spec §8.4: with the §8.3 pair overridden, a DIFFERENT object carrying the
// same X is the SAME key — found, read, and overwritten in place.  This also
// pins that the hash trampoline honours `GetHashCode` (without it `k1b` lands
// in another bucket and the equality trampoline is never consulted).
BEZEL_TEST(dict_class_key_2773,
           dictionary_class_key_equals_override_spec_8_4) {
    BEZEL_EXPECT_EQ(run(main_(kKey,
        "  using var k1a := new Key(1); using var k1b := new Key(1); "
        "  using var k2 := new Key(2); "
        "  using var d := new Dictionary<Key, int>(); "
        "  d.Add(k1a, 10); "
        "  var n := 0; "
        "  if d.ContainsKey(k1b) then n := n + 1; end; "
        "  if d[k1b] = 10 then n := n + 1; end; "
        "  if not d.ContainsKey(k2) then n := n + 1; end; "
        "  d[k1b] := 99; "
        "  if d.Count = 1 then n := n + 1; end; "
        "  if d[k1a] = 99 then n := n + 1; end; "
        "  return n;")), 5);
}

// BUILD MODE vs READ MODE (the agreement the brief demands be stated): the
// dictionary is now BUILT generic-mode, which is the mode `GetEnumerator`
// already read it in.  Iterating a class-keyed dictionary must therefore
// see every entry.
BEZEL_TEST(dict_class_key_2773,
           for_in_class_keyed_dictionary_read_mode_agrees_spec_4_8) {
    BEZEL_EXPECT_EQ(run(main_(kBox,
        "  using var b0 := new Box(1); using var b1 := new Box(2); "
        "  using var d := new Dictionary<Box, int>(); "
        "  d.Add(b0, 10); d.Add(b1, 20); "
        "  var sum := 0; var cnt := 0; "
        "  for var e in d do sum := sum + e.Value; cnt := cnt + 1; end; "
        "  return sum + cnt;")), 32);   // 30 + 2
}

// spec §3.6 admits an interface as equality-bearing; stdlib §4.8 makes it a
// legal `K`.
BEZEL_TEST(dict_class_key_2773, dictionary_interface_key_spec_3_6) {
    BEZEL_EXPECT_EQ(run(main_(kShape,
        "  using var s0 := new Sq(2); using var s1 := new Sq(3); "
        "  using var d := new Dictionary<IShape, int>(); "
        "  d.Add(s0, 10); d.Add(s1, 20); "
        "  var n := 0; "
        "  if d.Count = 2 then n := n + 1; end; "
        "  if d.ContainsKey(s0) then n := n + 1; end; "
        "  if d[s1] = 20 then n := n + 1; end; "
        "  return n;")), 3);
}

// An `Object`-typed key — the third shape carrying the §8.3 object header.
BEZEL_TEST(dict_class_key_2773, dictionary_object_key_spec_8_3) {
    BEZEL_EXPECT_EQ(run(main_(kBox,
        "  using var b0 := new Box(1); using var b1 := new Box(2); "
        "  using var d := new Dictionary<Object, int>(); "
        "  d.Add(b0, 7); "
        "  var n := 0; "
        "  if d.ContainsKey(b0) then n := n + 1; end; "
        "  if not d.ContainsKey(b1) then n := n + 1; end; "
        "  return n;")), 2);
}

// spec §15.4: the collection-literal materializer allocates the backing table
// with the SAME discriminator, so a class-keyed dict literal must build a
// generic-mode table too (row A4's second site).
//
// ⚠ This one test uses `runAot` (ASan, no LeakSanitizer) rather than
// `runAotChecked`.  A dictionary LITERAL leaks its whole backing table
// TODAY, for every key type, and that is a SEPARATE pre-existing defect —
// MEASURED here on an int-keyed literal, which this change does not touch:
//
//     for var i in 1..2000 do
//       const d: Dictionary<int, int> := [1: 10, 2: 20]; … end;
//     SUMMARY: AddressSanitizer: 832000 byte(s) leaked in 10000 allocation(s)
//     (#1 bz_dict_set_gen)
//
// (The compiler also emits "`using var` d is vacuous … no disposal
// obligation" for the same literal, int key included — the same missing
// registration seen from the front end.)  Under LSan the leak is detected
// only ~half the time, because a leaked block whose address is still lying
// in a stack slot is reported as reachable, which made this test flaky before
// the cause was found.  Filed separately; asserting the VALUE here is the part
// row A4 is about.
BEZEL_TEST(dict_class_key_2773, dictionary_class_key_literal_spec_15_4) {
    auto r = runAot(main_(kBox,
        "  using var b0 := new Box(1); using var b1 := new Box(2); "
        "  const d: Dictionary<Box, int> := [b0: 5, b1: 6]; "
        "  return d.Count * 10 + d[b1];"));
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 26);
}

// stdlib §4.9: `OwningDictionary` owns its VALUES; the key is held by borrow
// (`Add(key: K, owned value: V)`).  The values here have a destructor, so a
// disposal mistake shows up as a leak under the ASan/LSan gate.
BEZEL_TEST(dict_class_key_2773,
           owning_dictionary_class_key_borrows_key_spec_4_9) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; "
        "class Box public V: int; "
        "  public constructor(v: int) V := v; end constructor; end Box; "
        "class Res public N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  destructor end destructor; end Res; ",
        "  using var b0 := new Box(1); using var b1 := new Box(2); "
        "  using var d := new OwningDictionary<Box, Res>(); "
        "  d.Add(b0, new Res(10)); d.Add(b1, new Res(20)); "
        "  var n := 0; "
        "  if d.Count = 2 then n := n + 1; end; "
        "  if d.ContainsKey(b0) then n := n + 1; end; "
        "  if d[b1].N = 20 then n := n + 1; end; "
        "  if d.Remove(b0) then n := n + 1; end; "
        "  if d.Count = 1 then n := n + 1; end; "
        "  return n;")), 5);
}

// spec §14.1 / §3.6 rule 2: the discriminator resolves a bare type parameter
// through the active monomorphization binding, so the SAME generic body must
// pick generic-mode for `K = Box` and the unchanged path for `K = int`.
BEZEL_TEST(dict_class_key_2773,
           generic_body_class_substituted_key_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System; "
        "class Box public V: int; "
        "  public constructor(v: int) V := v; end constructor; end Box; "
        "class Registry<K> "
        "  public static method CountOf(a: K, b: K): int "
        "    using var d := new Dictionary<K, int>(); "
        "    d.Add(a, 1); d.Add(b, 2); "
        "    return d.Count; "
        "  end CountOf; "
        "end Registry; "
        "class App public static method Main(): int "
        "  using var b0 := new Box(1); using var b1 := new Box(2); "
        "  return Registry<Box>.CountOf(b0, b1) * 10 + "
        "         Registry<int>.CountOf(7, 8); "
        "end Main; end App;"), 22);
}

// ===========================================================================
// Over-correction controls — the shapes that already worked must not move.
// The fix WIDENS a discriminator used at ~20 sites, so this is where a
// mis-widening would surface.
// ===========================================================================

// stdlib §4.8: a `string` key keeps its dedicated `bz_dict_*_str` fast path
// (content equality, §4.6) — the branch the fix must NOT capture.
BEZEL_TEST(dict_class_key_2773, string_key_keeps_fast_path_spec_4_8) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; ",
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 1); d.Add(\"b\", 2); "
        "  var n := 0; "
        "  if d.Count = 2 then n := n + 1; end; "
        "  if d.ContainsKey(\"a\") then n := n + 1; end; "
        "  if d[\"b\"] = 2 then n := n + 1; end; "
        "  if not d.ContainsKey(\"c\") then n := n + 1; end; "
        "  return n;")), 4);
}

// An int key (the existing generic path) and a record key (the #343 path).
BEZEL_TEST(dict_class_key_2773, int_and_record_keys_unchanged_spec_4_8) {
    BEZEL_EXPECT_EQ(run(main_(
        "module M; import System; record Pt(x: int); ",
        "  var n := 0; "
        "  do using var di := new Dictionary<int, int>(); "
        "    di.Add(1, 10); di.Add(2, 20); "
        "    if di.Count = 2 then n := n + 1; end; "
        "    if di[2] = 20 then n := n + 1; end; end; "
        "  do using var dr := new Dictionary<Pt, int>(); "
        "    dr.Add(new Pt(1), 10); dr.Add(new Pt(2), 20); "
        "    if dr.Count = 2 then n := n + 1; end; "
        "    if dr[new Pt(2)] = 20 then n := n + 1; end; end; "
        "  return n;")), 4);
}

// stdlib §4.10: a class-ELEMENT `Set<T>` keeps its ARRAY-backed linear-scan
// machinery (#1070) — the fix admits the object-header shape for the
// DICTIONARY key only (`isSet` is excluded), so this must be untouched.
BEZEL_TEST(dict_class_key_2773, class_element_set_stays_array_backed_spec_4_10) {
    BEZEL_EXPECT_EQ(run(main_(kBox,
        "  using var b0 := new Box(1); using var b1 := new Box(2); "
        "  using var s: Set<Box> := [b0, b1]; "
        "  return s.Count * 10 + int(s.Contains(b0));")), 21);
}
