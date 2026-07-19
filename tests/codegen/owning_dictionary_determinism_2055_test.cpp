// Pinning tests for #2055 item 2 — OwningDictionary iteration determinism.
//
// Spec: Bezel_Language_Spec.md v0.204
//   §30.13 (sec-determinism):
//     "A domain's execution is a deterministic function of what its edges
//      deliver ... each domain runs the same continuation steps computing the
//      same values, issues the same edge requests with the same request
//      payloads in the same order, produces the same output bytes at each
//      output edge, and the program terminates identically ... Memory
//      addresses need no exclusion ... and identity hash codes are assigned
//      per §8.3, never derived from addresses. ... any within-domain
//      nondeterminism given fixed edges — an uninitialized read, an
//      unpinned iteration order, an address leak — is an implementation
//      defect, not latitude."
//   §8.3 (sec-identity-hash-assignment): identity hashes are assigned on
//     first request from a per-domain deterministic sequence, so two runs of
//     the same single-domain program with the same operation history produce
//     identical hash sequences.
//   stdlib §4.9 (OwningDictionary iteration order, v0.204):
//     "The order, while unspecified, is deterministic: it is a pure function
//      of the sequence of mutating operations performed and the key hash
//      values involved — two instances built by identical operation sequences
//      over keys with identical hash values iterate identically."
//
// COVERAGE GOAL. The basic cross-run determinism case for a 3-entry string-
// keyed dict is already pinned by the (e)/(e-cont) tests in
// assigned_identity_hash_2055_test.cpp. This file makes the invariant LOAD-
// BEARING for the harder paths the spec's wording covers but the basic test
// does not exercise:
//
//   (a) REHASH (stdlib §4.9): inserting enough entries to force at least one
//       capacity-doubling rehash. dict_rehash rebuilds the bucket array AND
//       rewrites the order[] tracking array via slot_map; both must be
//       address-independent so two runs of the same program produce identical
//       iteration output.
//   (b) REMOVES + rehash compaction (stdlib §4.9): a remove writes a
//       tombstone into order[]; the next rehash compacts the tombstones and
//       translates surviving slots via slot_map. The post-rehash iteration
//       order must be a pure function of the op history.
//   (c) GENERIC-KEY path (stdlib §4.9): a record-typed key routes through
//       the bz_dict_*_gen runtime path with the codegen-emitted
//       buildRecordHashTrampoline. Determinism on this path depends on the
//       synthesized record GetHashCode being a pure function of the key's
//       bytes (it is — §9.1/§9.2 structural hash) AND on the runtime's
//       insertion-order tracking not depending on key addresses.
//   (d) IN-PROCESS two-instance agreement (stdlib §4.9): two OwningDictionary
//       instances built by identical op sequences in ONE program iterate
//       identically — the strong form (different addresses, different key-
//       instance pointers, same observable order).
//   (e) DRAIN() order (stdlib §4.9 Drain): bz_dict_drain_next_* walks order[]
//       from the front, yielding entries in insertion order. The drain
//       sequence must be reproducible across runs.
//   (f) RE-ITERATION stability (§30.13): iterating the same dict twice in one
//       run yields identical order — the §30.13 single-domain determinism
//       property at the iteration level.
//
// WHY THESE ARE #2055 ITEM 2 PINNING TESTS, NOT GENERAL DICT TESTS. Before
// the §8.3 fix (commit 79d7dec), a class-typed identity hash was the LOW 32
// BITS of the heap address — ASLR/heap-layout entropy that the runtime
// surfaced through GetHashCode. The OwningDictionary runtime stores per-key
// hash values in bucket entries (e.hash) and uses them for bucketing AND for
// equality fast-paths; an address-derived hash would have made the bucket
// distribution vary across runs and, with it, the order[] slot indices that
// rehash compaction leaves behind. None of these tests' key TYPES reach the
// identity-hash path (string keys hash by content via FNV-1a; record keys by
// synthesized structural hash), so they pass even pre-fix; the value of
// pinning them here is that any FUTURE change to the runtime that re-introduces
// address dependence into the iteration path (e.g. replacing the order[]
// tracking with a pure hash-bucket walk, or making the synthesized record
// hash inadvertently include a boxed-field address) flips these tests red.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotOut;

// Returns the captured stdout of an AOT run, or "<did-not-run>" on a
// reject/crash/timeout — the standard runAotOut sentinel. Tests compare two
// such results (two runs of the SAME source) for byte-equality to pin cross-
// run determinism, and assert a non-sentinel first to surface a compiler/run
// failure rather than a false "both runs were equally broken" green.
static std::string outOf(const char* src) { return runAotOut(src); }

// True iff `s` is a real program output (not the runAotOut sentinel).
static bool ranOk(const std::string& s) {
    return !s.empty() && s != std::string("<did-not-run>");
}

// ===========================================================================
// (a) REHASH — many entries force capacity doublings (8 -> 16 -> 32 -> 64).
//     stdlib §4.9: the iteration order is a pure function of the op sequence
//     and key hashes. dict_rehash rebuilds buckets + rewrites order[] via
//     slot_map; the rewrite must be address-independent. The program inserts
//     50 string-keyed entries (forcing at least two rehashes) and prints the
//     iteration order; two independent runs must produce byte-identical
//     output.
// ===========================================================================
BEZEL_TEST(owning_dictionary_determinism_2055, rehash_cross_run_deterministic) {
    // 50 entries ≫ initial capacity 8 → at least two doublings (8 → 16 → 32
    // → 64). Keys are content-distinct strings so bucket placement is a pure
    // function of FNV-1a(key) & mask, identical across runs.
    const char* src =
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "  end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, Box>(); "
        "  var i := 0; "
        "  while i < 50 do "
        "    d[$\"k{i}\"] := new Box(i); "
        "    i := i + 1; "
        "  end; "
        "  var line := \"\"; "
        "  for var k in d.Keys do "
        "    if line <> \"\" then line := line + \",\"; end; "
        "    line := line + k; "
        "  end; "
        "  Console.WriteLine(line); "
        "  return 0; "
        "end Main; end C;";
    std::string first  = outOf(src);
    std::string second = outOf(src);
    BEZEL_EXPECT_TRUE(ranOk(first));   // compile + run succeeded
    BEZEL_EXPECT_EQ(first, second);    // §30.13 cross-run determinism
}

// ===========================================================================
// (b) REMOVES + rehash compaction. stdlib §4.9: the iteration order is a pure
//     function of the MUTATING-OPERATION sequence, which includes removes.
//     A remove writes a -1 tombstone into order[]; a subsequent insert that
//     triggers rehash compacts tombstones and translates surviving slots via
//     slot_map. Two runs of the same insert/remove/insert program must agree.
// ===========================================================================
BEZEL_TEST(owning_dictionary_determinism_2055,
           removes_then_rehash_cross_run_deterministic) {
    // Insert 12 entries (capacity 8 → 16 after entry ~6), then remove the
    // first 6 (writing tombstones), then insert 8 more (forcing another
    // rehash that compacts tombstones via slot_map). The final iteration
    // order is a function of the full op history.
    const char* src =
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "  end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, Box>(); "
        "  var i := 0; "
        "  while i < 12 do "           // fill past initial capacity 8
        "    d[$\"k{i}\"] := new Box(i); "
        "    i := i + 1; "
        "  end; "
        "  i := 0; "
        "  while i < 6 do "            // remove first 6 (tombstones)
        "    d.Remove($\"k{i}\"); "
        "    i := i + 1; "
        "  end; "
        "  i := 100; "
        "  while i < 108 do "          // 8 more inserts, forces rehash
        "    d[$\"k{i}\"] := new Box(i); "
        "    i := i + 1; "
        "  end; "
        "  var line := \"\"; "
        "  for var k in d.Keys do "
        "    if line <> \"\" then line := line + \",\"; end; "
        "    line := line + k; "
        "  end; "
        "  Console.WriteLine(line); "
        "  return 0; "
        "end Main; end C;";
    std::string first  = outOf(src);
    std::string second = outOf(src);
    BEZEL_EXPECT_TRUE(ranOk(first));
    BEZEL_EXPECT_EQ(first, second);    // §30.13 — tombstone+rehash path
}

// ===========================================================================
// (c) GENERIC-KEY path. stdlib §4.9 / §4.8: a record key routes through the
//     bz_dict_*_gen runtime path with codegen-emitted trampolines over the
//     record's synthesized GetHashCode (§9.1/§9.2 structural hash). The
//     iteration order is determined by (record-key hash, op sequence); both
//     are identical across runs → output identical.
// ===========================================================================
BEZEL_TEST(owning_dictionary_determinism_2055,
           record_keys_cross_run_deterministic) {
    // OwningDictionary<Coord, Box> — Coord is a positional record (auto-
    // equality-bearing per §9.1), so it routes through the generic-key path.
    // OwningDictionary requires V: class, so Box plays the value. The key
    // set is built so consecutive Coords land in distinct hash buckets.
    const char* src =
        "module M; "
        "record Coord(x: int, y: int); "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "  end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<Coord, Box>(); "
        "  d[new Coord(1, 1)] := new Box(11); "
        "  d[new Coord(2, 1)] := new Box(21); "
        "  d[new Coord(1, 2)] := new Box(12); "
        "  d[new Coord(3, 5)] := new Box(35); "
        "  d[new Coord(7, 4)] := new Box(74); "
        "  d[new Coord(9, 9)] := new Box(99); "
        "  var line := \"\"; "
        "  for var k in d.Keys do "
        "    if line <> \"\" then line := line + \",\"; end; "
        "    line := line + $\"{k.x}x{k.y}\"; "
        "  end; "
        "  Console.WriteLine(line); "
        "  return 0; "
        "end Main; end C;";
    std::string first  = outOf(src);
    std::string second = outOf(src);
    BEZEL_EXPECT_TRUE(ranOk(first));
    BEZEL_EXPECT_EQ(first, second);    // §30.13 — generic-key path
}

// ===========================================================================
// (d) IN-PROCESS two-instance agreement with rehash. stdlib §4.9: "two
//     instances built by identical operation sequences over keys with
//     identical hash values iterate identically." This is the STRONG form
//     of the determinism invariant: even inside one run, where the two
//     dicts live at different addresses with different key-string heap
//     pointers, the iteration orders must agree. We force a rehash (50
//     entries each) and compare the two iteration lines.
//
//     Encoded as a 1-line stdout: "agree" if the two iteration orders match
//     character-for-character, "disagree" otherwise. (The explicit disagreement
//     signal keeps the test from passing silently on a sentinel like
//     "<did-not-run>".)
// ===========================================================================
BEZEL_TEST(owning_dictionary_determinism_2055,
           two_instances_agree_after_rehash) {
    const char* src =
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "  end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d1 := new OwningDictionary<string, Box>(); "
        "  var i := 0; "
        "  while i < 50 do "
        "    d1[$\"k{i}\"] := new Box(i); "
        "    i := i + 1; "
        "  end; "
        "  using var d2 := new OwningDictionary<string, Box>(); "
        "  i := 0; "
        "  while i < 50 do "
        "    d2[$\"k{i}\"] := new Box(i); "
        "    i := i + 1; "
        "  end; "
        "  var l1 := \"\"; var l2 := \"\"; "
        "  for var k in d1.Keys do "
        "    if l1 <> \"\" then l1 := l1 + \",\"; end; "
        "    l1 := l1 + k; "
        "  end; "
        "  for var k in d2.Keys do "
        "    if l2 <> \"\" then l2 := l2 + \",\"; end; "
        "    l2 := l2 + k; "
        "  end; "
        "  if l1 = l2 then Console.WriteLine(\"agree\"); "
        "             else Console.WriteLine(\"disagree\"); end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(outOf(src), "agree");
}

// ===========================================================================
// (d) cont.: two-instance agreement WITH removes. Same as above but the op
//     sequence includes removes (writing tombstones) before the comparison.
//     Pins that the tombstone path is deterministic across two distinct dict
//     instances inside one run.
// ===========================================================================
BEZEL_TEST(owning_dictionary_determinism_2055,
           two_instances_agree_after_removes) {
    const char* src =
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "  end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d1 := new OwningDictionary<string, Box>(); "
        "  d1[\"a\"] := new Box(1); d1[\"b\"] := new Box(2); d1[\"c\"] := new Box(3); "
        "  d1[\"d\"] := new Box(4); d1[\"e\"] := new Box(5); d1[\"f\"] := new Box(6); "
        "  d1.Remove(\"b\"); d1.Remove(\"d\"); d1.Remove(\"f\"); "
        "  d1[\"g\"] := new Box(7); d1[\"h\"] := new Box(8); "
        "  using var d2 := new OwningDictionary<string, Box>(); "
        "  d2[\"a\"] := new Box(1); d2[\"b\"] := new Box(2); d2[\"c\"] := new Box(3); "
        "  d2[\"d\"] := new Box(4); d2[\"e\"] := new Box(5); d2[\"f\"] := new Box(6); "
        "  d2.Remove(\"b\"); d2.Remove(\"d\"); d2.Remove(\"f\"); "
        "  d2[\"g\"] := new Box(7); d2[\"h\"] := new Box(8); "
        "  var l1 := \"\"; var l2 := \"\"; "
        "  for var k in d1.Keys do "
        "    if l1 <> \"\" then l1 := l1 + \",\"; end; "
        "    l1 := l1 + k; "
        "  end; "
        "  for var k in d2.Keys do "
        "    if l2 <> \"\" then l2 := l2 + \",\"; end; "
        "    l2 := l2 + k; "
        "  end; "
        "  if l1 = l2 then Console.WriteLine(\"agree\"); "
        "             else Console.WriteLine(\"disagree\"); end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(outOf(src), "agree");
}

// ===========================================================================
// (e) DRAIN() order. stdlib §4.9: Drain() yields DrainedEntry<K, V> in
//     insertion order (bz_dict_drain_next_* walks order[] from the front,
//     advancing a cursor). Two runs of the same program must observe the
//     same drain order. This pins that the drain cursor's order[] scan is
//     a pure function of the op history.
// ===========================================================================
BEZEL_TEST(owning_dictionary_determinism_2055,
           drain_order_cross_run_deterministic) {
    // Drain() is ownership-transferring; the loop variable must be `owned`
    // and discharged (stdlib §4.9 example). Each yielded entry's Key is the
    // original insertion key; we collect them into one line for comparison.
    const char* src =
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "  end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, Box>(); "
        "  d[\"alpha\"] := new Box(1); "
        "  d[\"beta\"]  := new Box(2); "
        "  d[\"gamma\"] := new Box(3); "
        "  d[\"delta\"] := new Box(4); "
        "  d[\"epsilon\"] := new Box(5); "
        "  d[\"zeta\"]  := new Box(6); "
        "  var line := \"\"; "
        "  for var e in d.Drain() do "
        "    using var owned_e := owned e; "      // v0.180 transfer marker
        "    if line <> \"\" then line := line + \",\"; end; "
        "    line := line + owned_e.Key; "
        "  end; "
        "  Console.WriteLine(line); "
        "  return 0; "
        "end Main; end C;";
    std::string first  = outOf(src);
    std::string second = outOf(src);
    BEZEL_EXPECT_TRUE(ranOk(first));
    BEZEL_EXPECT_EQ(first, second);    // §30.13 — Drain() order
}

// ===========================================================================
// (f) RE-ITERATION stability. §30.13: within a single domain, fixed edges
//     produce identical execution. Iterating the same dict twice in one run
//     is the simplest within-run determinism check — the two passes must
//     observe identical order. A violation here would mean the snapshot
//     iterator itself introduced non-determinism (it doesn't: bz_dict_keys_str
//     walks order[] deterministically).
// ===========================================================================
BEZEL_TEST(owning_dictionary_determinism_2055,
           reiteration_within_run_stable) {
    const char* src =
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "  end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, Box>(); "
        "  var i := 0; "
        "  while i < 30 do "
        "    d[$\"k{i}\"] := new Box(i); "
        "    i := i + 1; "
        "  end; "
        "  var pass1 := \"\"; "
        "  for var k in d.Keys do "
        "    if pass1 <> \"\" then pass1 := pass1 + \",\"; end; "
        "    pass1 := pass1 + k; "
        "  end; "
        "  var pass2 := \"\"; "
        "  for var k in d.Keys do "
        "    if pass2 <> \"\" then pass2 := pass2 + \",\"; end; "
        "    pass2 := pass2 + k; "
        "  end; "
        "  if pass1 = pass2 then Console.WriteLine(\"stable\"); "
        "                    else Console.WriteLine(\"unstable\"); end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(outOf(src), "stable");
}
