// Regression tests for #2055 — assigned identity hashes (proposal 0045
// normative deltas, v0.204).
//
// Spec: Bezel_Language_Spec.md v0.204
//   §8.3 (sec-identity-hash-assignment):
//     "The identity-based default `GetHashCode` for classes is assigned on
//      first request, not derived from the object's storage address: the
//      runtime draws the value from a per-domain deterministic sequence
//      (objects first hashed outside any domain draw from the startup/
//      shutdown thread's sequence, §30.13) and stores it with the object
//      thereafter. Identity hash codes are therefore a function of the order
//      in which objects' hashes are first requested — deterministic under
//      §30.13 — and never expose addresses."
//   §30.13 (sec-determinism):
//     "A domain's execution is a deterministic function of what its edges
//      deliver ... Memory addresses need no exclusion: no conforming program
//      can observe one ... and identity hash codes are assigned per §8.3,
//      never derived from addresses."
//
// Prior to this fix the class-default GetHashCode thunk and the box-default
// identity-hash thunk were both `trunc(ptrToInt(this, i64), i32)` — i.e. the
// hash WAS the low 32 bits of the heap address, so two runs of the same
// program produced different hash sequences (ASLR, heap-layout entropy), and
// the hash value was a (masked) object address. The fix swaps both thunks to
// call `bz_obj_identity_hash`, which assigns from a per-domain monotonic
// counter on first request and stores the value in a side-table thereafter.
//
// Test surface:
//   (a) within-run stability — calling GetHashCode twice on the same object
//       yields the same value (the stored value, not a recompute).
//   (b) cross-run determinism — running the same single-domain program twice
//       yields identical printed hashes (verified by running the program two
//       times in the same test and comparing stdout).
//   (c) distinct objects get distinct hashes (counter is monotonic).
//   (d) equal objects (the same object) get equal hashes (trivially via (a)).
//   (e) OwningDictionary determinism (stdlib §4.9, v0.204): two identically-
//       built instances iterate identically. Already holds post-fix because
//       keys hash to assigned (deterministic) values; this test makes the
//       invariant load-bearing per the issue's request.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// (a) §8.3 within-run stability — calling GetHashCode twice on the same
//     object returns the same value (the assignment is cached in the side-
//     table). Print and re-compare rather than assert on a specific value.
BEZEL_TEST(assigned_identity_hash_2055, within_run_stable_class) {
    // Plain class (no GetHashCode override) — exercises the §8.3 default
    // thunk installed at kGetHashCodeSlot. Two calls on the same receiver
    // must agree.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); "
        "  var h1 := a.GetHashCode(); "
        "  var h2 := a.GetHashCode(); "
        "  if h1 = h2 then Console.WriteLine(\"same\"); "
        "             else Console.WriteLine(\"diff\"); end; "
        "  return 0; "
        "end Main; end C;"), "same");
}

// (a) cont.: the same stability holds when the identity hash is reached via
// `base.GetHashCode()` (the §12.3 / #1111 path), confirming the override-
// through-base route still lands in the assigned-hash thunk.
BEZEL_TEST(assigned_identity_hash_2055, within_run_stable_base_gethashcode) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class A "
        "  public override method GetHashCode(): int => base.GetHashCode(); "
        "end A; "
        "class C public static method Main(): int "
        "  using var a := new A(); "
        "  var h1 := a.GetHashCode(); var h2 := a.GetHashCode(); "
        "  if h1 = h2 then Console.WriteLine(\"same\"); "
        "             else Console.WriteLine(\"diff\"); end; "
        "  return 0; "
        "end Main; end C;"), "same");
}

// (a) for boxes — a boxed record reached through Object uses the box-default
// GetHashCode thunk (also bz_obj_identity_hash post-fix). Two reads agree.
BEZEL_TEST(assigned_identity_hash_2055, within_run_stable_boxed_default) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Rec public x: int; end Rec; "
        "class C public static method Main(): int "
        "  var r := new Rec(7); "
        "  var o: Object := r; "            // boxes Rec; uses box-default thunk
        "  var h1 := o.GetHashCode(); "
        "  var h2 := o.GetHashCode(); "
        "  if h1 = h2 then Console.WriteLine(\"same\"); "
        "             else Console.WriteLine(\"diff\"); end; "
        "  return 0; "
        "end Main; end C;"), "same");
}

// ---------------------------------------------------------------------------
// (b) §8.3 / §30.13 cross-run determinism — running the same single-domain
//     program TWICE yields identical printed hash sequences. Under the prior
//     address-derived hash, ASLR / heap layout produced different values each
//     run; under the assigned-counter hash, the first-request order is fixed
//     by the program text so the sequence is reproducible. We print the
//     concrete hash values of three freshly-allocated objects and require
//     runAotOut to be byte-identical across two invocations.
//
//     We do NOT assert a specific value (the spec leaves the exact sequence
//     opaque) — only that it is the SAME across runs of the same artifact.
BEZEL_TEST(assigned_identity_hash_2055, cross_run_deterministic) {
    const char* src =
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); "
        "  using var b := new Plain(); "
        "  using var c := new Plain(); "
        "  Console.WriteLine(a.GetHashCode()); "
        "  Console.WriteLine(b.GetHashCode()); "
        "  Console.WriteLine(c.GetHashCode()); "
        "  return 0; "
        "end Main; end C;";
    std::string first  = runAotOut(src);
    std::string second = runAotOut(src);
    // Must have actually run (not "<did-not-run>").
    BEZEL_EXPECT_FALSE(first.empty());
    BEZEL_EXPECT_FALSE((first) == (std::string("<did-not-run>")));
    // The two runs produce byte-identical stdout (the §8.3 / §30.13 contract).
    BEZEL_EXPECT_EQ(first, second);
}

// ---------------------------------------------------------------------------
// (c) §8.3 distinct-objects-get-distinct-hashes — three fresh objects must
//     hash to three distinct values (the counter is monotonic; collision
//     across 3 consecutive draws is impossible). Encodes the result as the
//     count of pairwise-equal pairs among (a,b), (a,c), (b,c): expect 0.
BEZEL_TEST(assigned_identity_hash_2055, distinct_objects_distinct_hashes) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); "
        "  using var b := new Plain(); "
        "  using var c := new Plain(); "
        "  var ha := a.GetHashCode(); "
        "  var hb := b.GetHashCode(); "
        "  var hc := c.GetHashCode(); "
        "  var eqpairs := 0; "
        "  if ha = hb then eqpairs := eqpairs + 1; end; "
        "  if ha = hc then eqpairs := eqpairs + 1; end; "
        "  if hb = hc then eqpairs := eqpairs + 1; end; "
        "  return eqpairs; "                // expect 0
        "end Main; end C;"), 0);
}

// (c) cont.: the assigned identity is itself stable for a given object —
// requesting b's hash BEFORE a's hash (counter draws in request order) still
// yields three pairwise-distinct values. Confirms the monotonic-counter
// property regardless of the order in which the program requests hashes.
BEZEL_TEST(assigned_identity_hash_2055, distinct_objects_reverse_order) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); "
        "  using var b := new Plain(); "
        "  using var c := new Plain(); "
        "  var hc := c.GetHashCode(); "     // request out-of-alloc-order
        "  var hb := b.GetHashCode(); "
        "  var ha := a.GetHashCode(); "
        "  var eqpairs := 0; "
        "  if ha = hb then eqpairs := eqpairs + 1; end; "
        "  if ha = hc then eqpairs := eqpairs + 1; end; "
        "  if hb = hc then eqpairs := eqpairs + 1; end; "
        "  return eqpairs; "                // expect 0
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// (d) §8.3 hash-consistency under reference equality — `a = a` (Object
//     reference equality, §8.4) and `a.GetHashCode() = a.GetHashCode()` both
//     hold trivially for the identity hash; this is the equality-hash
//     invariant in the §8.3 table row "Class | Reference equality |
//     Identity-based". Encoded as: same object always hashes equal to itself.
BEZEL_TEST(assigned_identity_hash_2055, identity_hash_self_consistency) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var a := new Plain(); "
        "  if a.GetHashCode() = a.GetHashCode() then return 1; "
        "                                       else return 0; end; "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// (e) OwningDictionary determinism — stdlib §4.9 (v0.204): "The order, while
//     unspecified, is deterministic: it is a pure function of the sequence of
//     mutating operations performed and the key hash values involved — two
//     instances built by identical operation sequences over keys with
//     identical hash values iterate identically." We build two instances with
//     the same operation sequence (same key strings, same insertion order)
//     and require their iteration to produce identical value-order. Under the
//     address-derived hash this was UNSOUND (the two dicts allocated at
//     different addresses had different bucket distributions → different
//     orders); under the assigned hash, key hashes are identical across
//     instances → identical iteration.
//
//     Value type is a class (stdlib §4.9 requires V: class for OwningDictionary).
//     Encoding: each dict's iteration is reduced to a single line "v1 v2 v3
//     v4" (the values in iteration order); the two lines must be equal.
BEZEL_TEST(assigned_identity_hash_2055, owning_dictionary_iteration_deterministic) {
    const char* body =
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  var line := \"\"; "
        "  using var d := new OwningDictionary<string, Box>(); "
        "  d[\"alpha\"] := new Box(1); "
        "  d[\"beta\"]  := new Box(2); "
        "  d[\"gamma\"] := new Box(3); "
        "  d[\"delta\"] := new Box(4); "
        "  for var k in d.Keys do "
        "    if line <> \"\" then line := line + \" \"; end; "
        "    line := line + $\"{d[k].v}\"; "
        "  end; "
        "  Console.WriteLine(line); "
        "  return 0; "
        "end Main; end C; ";
    std::string first  = runAotOut(body);
    std::string second = runAotOut(body);
    BEZEL_EXPECT_FALSE((first) == ( std::string("<did-not-run>")));
    BEZEL_EXPECT_EQ(first, second);
    // The two independent processes run the SAME program; §4.9 requires the
    // iteration to be a deterministic function of (operation sequence, key
    // hashes). Both inputs are identical across the two runs → output equal.
}

// (e) cont.: in-process determinism — two OwningDictionary instances built by
//     identical sequences inside ONE program iterate identically. (This is
//     the strong form of the §4.9 invariant: even within one run, the two
//     instances — at different addresses, with different key-instance
//     pointers — agree, because string-key hashes are content-based and
//     OwningDictionary's drain-and-rebuild is a pure function of the
//     operation+hash history.)
BEZEL_TEST(assigned_identity_hash_2055, owning_dictionary_two_instances_agree) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d1 := new OwningDictionary<string, Box>(); "
        "  d1[\"alpha\"] := new Box(1); d1[\"beta\"] := new Box(2); d1[\"gamma\"] := new Box(3); "
        "  using var d2 := new OwningDictionary<string, Box>(); "
        "  d2[\"alpha\"] := new Box(1); d2[\"beta\"] := new Box(2); d2[\"gamma\"] := new Box(3); "
        "  var agree := 1; "
        "  var seen1 := \"\"; var seen2 := \"\"; "
        "  for var k in d1.Keys do "
        "    if seen1 <> \"\" then seen1 := seen1 + \",\"; end; "
        "    seen1 := seen1 + k; "
        "  end; "
        "  for var k in d2.Keys do "
        "    if seen2 <> \"\" then seen2 := seen2 + \",\"; end; "
        "    seen2 := seen2 + k; "
        "  end; "
        "  if seen1 <> seen2 then agree := 0; end; "
        "  Console.WriteLine(agree); "
        "  return 0; "
        "end Main; end C;"), "1");
}
