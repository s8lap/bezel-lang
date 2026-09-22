// Codegen + runtime — fix #4499 (spec §4.6 / §8.3 / §4.5, stdlib §4.8 / §4.9 /
// §4.17, v0.306): the string-key hash table treated a key's CODE-POINT count as
// a BYTE count and dropped the key's storage width entirely, so distinct
// non-Latin-1 keys collided into one entry (silent data loss), and its
// `strndup` key copy heap-overflowed on a key holding a genuine U+0000.
//
// THE CONTRACT, and it is settled — nothing here is a chosen interpretation.
// Spec §4.6 (v0.306): "`s = t` and `s <> t` compare by code-point sequence",
// and of the per-string 1/2/4-byte storage width: "The choice of width is
// invisible at the language level; programs observe only the `char` sequence
// and its length." Spec §8.3 (v0.306): "`GetHashCode` must satisfy: if
// `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`." Stdlib §4.8
// (v0.306) makes `Dictionary<K, V>` a hash map whose `K` must be *hashable*
// (spec §3.6/§8.3) — so a string key's IDENTITY in the table is exactly its
// code-point sequence, and neither equality nor the hash may depend on the
// storage width. Spec §4.5 makes U+0000 an ordinary `char` value, so a key may
// legally contain one.
//
// DEFECT A — truncation and collision. Every codegen call site passed the
// runtime `(loadStringData(k), loadStringLen(k))`, i.e. the raw buffer plus the
// CODE-POINT count, and `bz_dict_set_str`/`dict_find`/`fnv1a` used that count
// as a BYTE count. At width 1 the two numbers coincide, which is why an
// ASCII-only corpus never caught it. At width 2 the stored key is the first
// `len` of `len*2` bytes — i.e. its first `len/2` code points — so keys sharing
// that truncated prefix hash and compare EQUAL. Measured on HEAD 6c05b4cf
// (`scripts/agent_check.sh --probe --asan`), the issue's own repro:
//
//     d["日本"] := 1;  d["日x"] := 2;  d["日a"] := 3;
//     -> count=1, and every TryGetValue returned 3
//
// All three keys are width 2 and 2 code points; each was truncated to the two
// bytes of 日 (U+65E5) alone, so the first two writes were silently clobbered.
//
// DEFECT B — heap-buffer-overflow. `bz_dict_set_str` copied the key with
// `strndup(k, k_len)`. `k` is a bz_string data buffer (not NUL-terminated) and
// `k_len` is a code-point count; for a key containing U+0000 the internal
// `strnlen` stops at the embedded NUL and allocates LESS than `k_len`, while
// the entry still records `key_len = k_len`. The next length-bounded read of
// that entry runs off the allocation. Measured on the same HEAD:
//
//     d["a\u0000b"] := 1;  d["a\u0000b"] := 2;
//     -> AddressSanitizer: heap-buffer-overflow, "0 bytes after 2-byte region",
//        read in MemcmpInterceptorCommon, allocation by strndup in
//        bz_dict_set_str (bezel_runtime_collections.cpp:674)
//
// THE FIX. The ABI now carries the key as (data, CODE-POINT count, WIDTH) —
// one extra `int8_t` at every string-key entry point — `bz_entry_t` stores the
// width beside the bytes, and hashing/equality are code-point-level
// (`fnv1a_cp`/`bz_key_eq`, decoding each side at its own width, the same
// policy `bz_str_compare_cp` (#4487) applies to ordering). `strndup` is
// replaced by `bz_key_dup` — malloc + memcpy of exactly `len * width` bytes
// plus a width-sized zero terminator — so `key_len` always matches the
// allocation whatever the key contains. The SNAPSHOT side (`bz_dict_keys_str`,
// `bz_dict_min_key_str`/`max_key_str`, `bz_dict_drain_next_str`) hands the
// width back as a third parallel spine, because a key's byte size is
// `len * width` and the width is NOT recoverable from the bytes.
//
// WHY THE WIDTH TAG, AND NOT JUST A BYTE LENGTH. Threading the true byte length
// alone is unsound: the width-2 key "日" and the width-1 key "åe" hold the SAME
// two bytes (E5 65), so a (bytes, byte-length) key would make two different
// strings one entry. `..._width_tag_distinguishes_same_bytes_...` below pins
// that; it is one of the rows that answers identically on both sides of THIS
// fix, and it is here to keep the next one honest.
//
// ORDERING IS NOT THIS ISSUE. `bz_kv_idx_cmp_str` still orders the sorted
// snapshot by `strcmp` over the raw bytes; that is #4493, and it is what the
// width spine added here makes writable. No row below asserts a non-ASCII
// SortedDictionary ORDER — only that its keys stay distinct and round-trip.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotOut / runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// The ASan/LSan oracle. A sanitizer report flips `ran` to false, so the -999
// sentinel is what Defect B's rows report pre-fix.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

std::string wrap(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Defect A — Dictionary<string, V>. The issue's own repro and its neighbours.
// Each key below is width 2 or width 4 and shares its LEADING code point(s)
// with its siblings, which is exactly the shape the byte-count truncation
// collapsed.
// ---------------------------------------------------------------------------

// The issue's repro, count half. Three distinct 2-code-point width-2 keys.
// Pre-fix: 1.
BEZEL_TEST(dict_string_key_width_4499, width2_three_keys_count_spec_4_8) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"日本\"] := 1; d[\"日x\"] := 2; d[\"日a\"] := 3; "
        "  return d.Count; ")), 3);
}

// The issue's repro, value half: each key must read back its OWN value.
// Encoded 100*v1 + 10*v2 + v3. Pre-fix: 333 (the last write won for all three).
BEZEL_TEST(dict_string_key_width_4499, width2_three_keys_values_spec_8_3) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"日本\"] := 1; d[\"日x\"] := 2; d[\"日a\"] := 3; "
        "  var a: int? := null; var b: int? := null; var c: int? := null; "
        "  d.TryGetValue(\"日本\", out a); "
        "  d.TryGetValue(\"日x\", out b); "
        "  d.TryGetValue(\"日a\", out c); "
        "  return a! * 100 + b! * 10 + c!; ")), 123);
}

// ContainsKey must not answer `true` for a key that merely shares the
// truncated prefix. Encoded: present*10 + absent. Pre-fix: 11 (日y collided
// with 日本 — both truncate to the two bytes of 日).
BEZEL_TEST(dict_string_key_width_4499, width2_containskey_discriminates_spec_4_8) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"日本\"] := 1; "
        "  var hit  := (if d.ContainsKey(\"日本\") then 1 else 0); "
        "  var miss := (if d.ContainsKey(\"日y\") then 1 else 0); "
        "  return hit * 10 + miss; ")), 10);
}

// Remove must unlink exactly the named key. Encoded: Count*10 + survivor.
// Pre-fix: the single colliding entry was removed, leaving 0 and no survivor.
BEZEL_TEST(dict_string_key_width_4499, width2_remove_is_targeted_spec_4_8) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"日本\"] := 7; d[\"日x\"] := 9; "
        "  var gone := (if d.Remove(\"日x\") then 1 else 0); "
        "  var left: int? := null; d.TryGetValue(\"日本\", out left); "
        "  return gone * 100 + d.Count * 10 + left!; ")), 117);
}

// §4.8 TryAdd: two distinct width-2 keys are not duplicates of one another.
// Encoded: first*100 + second*10 + Count. Pre-fix: 102 (the second TryAdd saw
// a duplicate and declined, leaving one entry).
BEZEL_TEST(dict_string_key_width_4499, width2_tryadd_distinct_keys_spec_4_8) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  var f := (if d.TryAdd(\"日本\", 1) then 1 else 0); "
        "  var s := (if d.TryAdd(\"日x\", 2) then 1 else 0); "
        "  return f * 100 + s * 10 + d.Count; ")), 112);
}

// Width 4. U+1F600 is an astral code point, so these keys store 4 bytes per
// code point; the byte-count walk read 2 of the 8 bytes. Pre-fix: 1 and the
// clobbered value. Encoded Count*10 + value-of-the-first-key.
BEZEL_TEST(dict_string_key_width_4499, width4_astral_keys_spec_4_6) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"\\U0001F600a\"] := 4; d[\"\\U0001F600b\"] := 6; "
        "  var v: int? := null; d.TryGetValue(\"\\U0001F600a\", out v); "
        "  return d.Count * 10 + v!; ")), 24);
}

// THE WIDTH-TAG GUARD. "日" is one code point at width 2; "\u00E5e" is two code
// points at width 1. Both hold the SAME two bytes, E5 65. A fix that threaded
// only a true BYTE length would make these one entry; the per-entry width tag
// is what keeps them two. This row answers 2 on BOTH sides of this fix (pre-fix
// they differed in code-point count, which the old key_len happened to carry) —
// it is a guard against the next fix, not a repro of this one, and is labelled
// as such rather than counted as evidence.
BEZEL_TEST(dict_string_key_width_4499, width_tag_distinguishes_same_bytes_spec_4_6) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"日\"] := 1; d[\"\\u00E5e\"] := 2; "
        "  var a: int? := null; var b: int? := null; "
        "  d.TryGetValue(\"日\", out a); "
        "  d.TryGetValue(\"\\u00E5e\", out b); "
        "  return d.Count * 100 + a! * 10 + b!; ")), 212);
}

// ---------------------------------------------------------------------------
// Defect A — the other two string-keyed dictionaries. stdlib §4.9
// (OwningDictionary) and §4.17 (SortedDictionary) share `bz_dict_set_str` /
// `dict_find` with §4.8's Dictionary, so the defect and the fix are the same
// for all three. Each is exercised on its own surface.
// ---------------------------------------------------------------------------

// stdlib §4.17. Pre-fix: Count 1, and every TryGetValue answered 3 —
// "n=1 a=3 b=3 c=3". The observable goes to stdout: a 4-digit encoding would
// not survive `runAot`'s WEXITSTATUS mask (3123 reads back as 51).
BEZEL_TEST(dict_string_key_width_4499, sorted_dictionary_width2_keys_spec_4_17) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "  using var d := new SortedDictionary<string,int>(); "
            "  d[\"日本\"] := 1; d[\"日x\"] := 2; d[\"日a\"] := 3; "
            "  var a: int? := null; var b: int? := null; var c: int? := null; "
            "  d.TryGetValue(\"日本\", out a); "
            "  d.TryGetValue(\"日x\", out b); "
            "  d.TryGetValue(\"日a\", out c); "
            "  Console.WriteLine($\"n={d.Count} a={a!} b={b!} c={c!}\"); "
            "  return 0; ")),
        "n=3 a=1 b=2 c=3");
}

// stdlib §4.9. V: class, so the collision ALSO disposed the displaced owner
// through the indexer's `set(owned)` — a lost object, not just a lost value.
// Pre-fix: "n=1 a=3 b=3 c=3". On stdout for the same 8-bit reason as the row
// above.
BEZEL_TEST(dict_string_key_width_4499, owning_dictionary_width2_keys_spec_4_9) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Box public v: int; public constructor(v: int) this.v := v; "
        "end constructor; end Box; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, Box>(); "
        "  d[\"日本\"] := new Box(1); "
        "  d[\"日x\"]  := new Box(2); "
        "  d[\"日a\"]  := new Box(3); "
        "  var a: Box? := null; var b: Box? := null; var c: Box? := null; "
        "  d.TryGetValue(\"日本\", out a); "
        "  d.TryGetValue(\"日x\", out b); "
        "  d.TryGetValue(\"日a\", out c); "
        "  Console.WriteLine($\"n={d.Count} a={a!.v} b={b!.v} c={c!.v}\"); "
        "  return 0; "
        "end Main; end C;"),
        "n=3 a=1 b=2 c=3");
}

// ---------------------------------------------------------------------------
// The SNAPSHOT side — a key must come back out at its true width. The pre-fix
// decode rebuilt every key as a width-1 string of `lens[i]` BYTES, which is the
// same code-point-count-as-byte-count confusion on the read side.
// ---------------------------------------------------------------------------

// `for var k in d.Keys` (stdlib §4.8). Each key is 2 code points, so the total
// is 4. Pre-fix: 2 — one surviving (collided) key of length 2.
BEZEL_TEST(dict_string_key_width_4499, keys_view_lengths_round_trip_spec_4_8) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"日本\"] := 1; d[\"日x\"] := 2; "
        "  var total := 0; "
        "  for var k in d.Keys do total := total + k.Length; end; "
        "  return total; ")), 4);
}

// The enumerated key's CODE POINTS, not its bytes. U+672C = 26412. Pre-fix the
// key came back as a width-1 string over the two bytes E5 65, so `k[1]` was
// 0x65 = 101. Out of exit-code range, so the value goes to stdout.
BEZEL_TEST(dict_string_key_width_4499, keys_view_code_points_round_trip_spec_4_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "  using var d := new Dictionary<string,int>(); "
            "  d[\"日本\"] := 1; "
            "  var cp := 0; "
            "  for var k in d.Keys do cp := int(k[1]); end; "
            "  Console.WriteLine($\"cp={cp}\"); "
            "  return 0; ")),
        "cp=26412");
}

// Entry iteration (`for var e in d`, stdlib §4.8) shares the same snapshot and
// the same decode. U+65E5 = 26085. Pre-fix: 229 (0xE5, the low byte).
BEZEL_TEST(dict_string_key_width_4499, entry_for_in_code_points_round_trip_spec_4_8) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "  using var d := new Dictionary<string,int>(); "
            "  d[\"日本\"] := 5; "
            "  var cp := 0; var v := 0; "
            "  for var e in d do cp := int(e.Key[0]); v := e.Value; end; "
            "  Console.WriteLine($\"cp={cp} v={v}\"); "
            "  return 0; ")),
        "cp=26085 v=5");
}

// stdlib §4.17 FirstKey/LastKey run through `bz_dict_min_key_str` /
// `bz_dict_max_key_str`, which copy the stored key and hand back its width.
// With a single entry the two are the same key, so the row asserts the
// round-trip and not an ORDER (that is #4493). Pre-fix: length 2 over the two
// bytes of 日, so `Length` read 2 but `[1]` was 0x65 = 101.
BEZEL_TEST(dict_string_key_width_4499, sorted_first_last_key_round_trip_spec_4_17) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "  using var d := new SortedDictionary<string,int>(); "
            "  d[\"日本\"] := 1; "
            "  var f := d.FirstKey; var l := d.LastKey; "
            "  Console.WriteLine($\"f={int(f[1])} l={int(l[1])} n={f.Length}\"); "
            "  return 0; ")),
        "f=26412 l=26412 n=2");
}

// ---------------------------------------------------------------------------
// Defect B — a key containing U+0000 (spec §4.5: an ordinary `char` value, so
// this is reachable from well-typed source). These rows run under the ASan/LSan
// oracle, because the pre-fix behaviour is a heap-buffer-overflow READ whose
// VALUE consequence is whatever byte happened to follow the short allocation —
// the report is the deterministic signal, the wrong value is not.
// ---------------------------------------------------------------------------

// Re-inserting the SAME embedded-NUL key is what reaches `dict_find`'s
// length-bounded compare against the short `strndup` allocation. Pre-fix:
// heap-buffer-overflow, "0 bytes after 2-byte region".
BEZEL_TEST(dict_string_key_width_4499, embedded_nul_reinsert_is_clean_spec_4_5) {
    BEZEL_EXPECT_EQ(runChecked(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"a\\u0000b\"] := 1; "
        "  d[\"a\\u0000b\"] := 2; "
        "  var v: int? := null; d.TryGetValue(\"a\\u0000b\", out v); "
        "  return d.Count * 10 + v!; ")), 12);
}

// Two embedded-NUL keys differing only AFTER the NUL. Both stored the single
// byte 'a' pre-fix, and the ContainsKey probes read past those allocations.
BEZEL_TEST(dict_string_key_width_4499, embedded_nul_keys_are_distinct_spec_4_5) {
    BEZEL_EXPECT_EQ(runChecked(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"a\\u0000b\"] := 1; "
        "  d[\"a\\u0000c\"] := 2; "
        "  var b := (if d.ContainsKey(\"a\\u0000b\") then 1 else 0); "
        "  var c := (if d.ContainsKey(\"a\\u0000c\") then 1 else 0); "
        "  return d.Count * 100 + b * 10 + c; ")), 211);
}

// An embedded-NUL key is not its own pre-NUL prefix: "a\u0000b" (3 code points)
// and "a" (1) are different keys. Pre-fix BOTH stored the byte 'a'; they were
// held apart only by the code-point count that happened to ride in `key_len`,
// over allocations of the wrong size.
BEZEL_TEST(dict_string_key_width_4499, embedded_nul_key_vs_prefix_spec_4_5) {
    BEZEL_EXPECT_EQ(runChecked(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"a\\u0000b\"] := 1; "
        "  d[\"a\"] := 2; "
        "  var x: int? := null; var y: int? := null; "
        "  d.TryGetValue(\"a\\u0000b\", out x); "
        "  d.TryGetValue(\"a\", out y); "
        "  return d.Count * 100 + x! * 10 + y!; ")), 212);
}

// A NUL in the middle of a WIDTH-2 key — both defects at once: the code point
// after the NUL is above U+00FF, so the key is 3 code points over 6 bytes and
// the pre-fix copy was 3 bytes stopped at the first zero byte.
BEZEL_TEST(dict_string_key_width_4499, embedded_nul_in_width2_key_spec_4_5) {
    BEZEL_EXPECT_EQ(runChecked(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"a\\u0000日\"] := 1; "
        "  d[\"a\\u0000本\"] := 2; "
        "  var x: int? := null; d.TryGetValue(\"a\\u0000日\", out x); "
        "  return d.Count * 10 + x!; ")), 21);
}

// ---------------------------------------------------------------------------
// Anti-regression: the width-1 (ASCII and Latin-1) path is where byte count and
// code-point count coincide, so it was already correct and must be UNMOVED.
// These rows answer identically on both sides of the fix by design.
// ---------------------------------------------------------------------------

BEZEL_TEST(dict_string_key_width_4499, ascii_keys_unchanged_spec_4_8) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"alpha\"] := 1; d[\"beta\"] := 2; d[\"gamma\"] := 3; "
        "  var v: int? := null; d.TryGetValue(\"beta\", out v); "
        "  var gone := (if d.Remove(\"gamma\") then 1 else 0); "
        "  return d.Count * 100 + v! * 10 + gone; ")), 221);
}

BEZEL_TEST(dict_string_key_width_4499, ascii_keys_view_unchanged_spec_4_8) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "  using var d := new Dictionary<string,int>(); "
            "  d[\"a\"] := 1; d[\"bb\"] := 2; d[\"ccc\"] := 3; "
            "  var acc := \"\"; "
            "  for var k in d.Keys do acc := acc + k + \",\"; end; "
            "  Console.WriteLine($\"ks={acc}\"); "
            "  return 0; ")),
        "ks=a,bb,ccc,");
}

// Latin-1 above ASCII is still width 1 — one byte per code point — so it too is
// unmoved. This is the boundary row: U+00E5 is the largest class of key the
// pre-fix code handled correctly.
BEZEL_TEST(dict_string_key_width_4499, latin1_width1_keys_unchanged_spec_4_6) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  d[\"\\u00E5x\"] := 1; d[\"\\u00E5y\"] := 2; "
        "  var v: int? := null; d.TryGetValue(\"\\u00E5y\", out v); "
        "  return d.Count * 10 + v!; ")), 22);
}

BEZEL_TEST(dict_string_key_width_4499, empty_dictionary_unchanged_spec_4_8) {
    BEZEL_EXPECT_EQ(run(wrap(
        "  using var d := new Dictionary<string,int>(); "
        "  var n := 0; "
        "  for var k in d.Keys do n := n + 1; end; "
        "  return d.Count * 10 + n; ")), 0);
}
