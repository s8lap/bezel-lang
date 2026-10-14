// Codegen — issue #5882 / spec §3.6, §8.3, §15.1.2 (v0.338): a non-owning
// inline array `[N]T` / `[N]T?` is a legal hash-container key and set element.
//
// THE DEFECT. `genericBackingType` — the one predicate every `Dictionary` /
// `Set` lowering site asks "is this key generic-backed, or a string?" — had no
// arm for an inline array, so it answered "string". A dictionary then lowered
// every key operation through the string fast path and GEP'd the `[3 x i32]`
// aggregate as a `%bz.string`:
//
//   GEP base pointer is not a vector or a vector of pointers
//     %8 = getelementptr inbounds nuw %bz.string, [3 x i32] %7, i32 0, i32 0
//
// (IR verifier, every spelling of every key operation). `Set<[3]int>` took the
// array-backed linear-scan path instead and COMPILED — with no deduplication
// (`Add` of an equal array grew the set) and `Set.ExceptWith` / `UnionWith` /
// `IsSupersetOf` refused as "produced no value".
//
// §3.6's hashable row excludes only "an inline array of any §15.1 form whose
// element type is `T` or `T?`, where `T` is itself not hashable"; §8.3's
// table gives the contract: "Inline array `[N]T`, `[N]T?` | Element-by-element
// value equality (§15.1.2) | Hash combining element hashes in slot order";
// §15.1.2: "Two inline-array values of the same type are equal iff their
// elements are pairwise equal under the element type's equality."
//
// The hash and equality the table asks for already existed
// (`emitValueHashCode`'s inline-array fold, `emitValueEquality`); the fix is
// the missing eligibility arm that routes the key to them. The arm is limited
// to an element with no string anywhere in it (#6238 — the table does not
// retain a key's strings) and a literal written directly in a key position is
// refused (#6237 — it is not lowered); Rows 15 and 16 pin both.
//
// Every run row is on `runAotChecked` (generated-code ASan) and asserts
// stdout; the multi-operation rows loop 50x so a per-operation leak or a
// stale key byte cannot hide in a one-shot.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* const kHead =
    "module M; "
    "enum E A, B, C; end E; "
    "record P(X: int, Y: int); "
    "class Res public N: int := 0; end Res; "
    "class Holder "
    "  private owned _d: Dictionary<[2]int, int> := new Dictionary<[2]int, int>(); "
    "  public method Put(k: [2]int, v: int) _d[k] := v; end Put; "
    "  public method Get(k: [2]int): int => _d[k]; "
    "  public method Has(k: [2]int): bool => _d.ContainsKey(k); "
    "  public method N(): int => _d.Count; "
    "end Holder; "
    "class Box<K> "
    "  private owned _d: Dictionary<K, int> := new Dictionary<K, int>(); "
    "  public method Put(k: K, v: int) _d[k] := v; end Put; "
    "  public method Get(k: K): int => _d[k]; "
    "  public method Has(k: K): bool => _d.ContainsKey(k); "
    "  public method N(): int => _d.Count; "
    "end Box; "
    "class C public static method Main(): int ";
const char* const kTail = "  return 0; end Main; end C;";

std::string prog(const std::string& body) {
    return std::string(kHead) + body + kTail;
}

// `body` runs 50 times; `once` is what one iteration prints.
std::string looped(const std::string& body) {
    return prog("for var i in 0..49 do " + body + " end; ");
}
std::string times50(const std::string& once) {
    std::string s;
    for (int i = 0; i < 50; ++i) s += once;
    return s;
}

bool hasDiagnostic(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// Row 1 — THE ISSUE'S OWN SHAPE: `Dictionary<[3]int, string>`, one `Add`,
// and the plain `d[k]` indexer read the issue names ("no `out var` at all").
// Pre-fix: the `%bz.string` GEP verifier failure.
BEZEL_TEST(dictionary_inline_array_key_5882,
           issue_repro_add_then_plain_indexer_read_spec_8_3) {
    auto r = runAotChecked(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<[3]int, string>(); "
        "  var k: [3]int := [1, 2, 3]; "
        "  d.Add(k, \"x\"); "
        "  Console.WriteLine(d[k]); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x\n"));
}

// Row 2 — hash + equality are by ELEMENTS, in slot order (§8.3 / §15.1.2): an
// equal array in a different local finds and overwrites the entry; one
// differing in the last slot, and a permutation, are distinct keys.
BEZEL_TEST(dictionary_inline_array_key_5882,
           equal_arrays_are_one_key_permutation_is_another_spec_15_1_2) {
    auto r = runAotChecked(looped(
        "using var d := new Dictionary<[3]int, string>(); "
        "var a: [3]int := [1, 2, 3]; var a2: [3]int := [1, 2, 3]; "
        "var b: [3]int := [1, 2, 4]; var p: [3]int := [3, 2, 1]; "
        "d.Add(a, \"x\"); d[b] := \"y\"; d[a2] := \"z\"; "
        "Console.WriteLine(d.Count); Console.WriteLine(d[a]); "
        "Console.WriteLine(d[b]); Console.WriteLine(d.ContainsKey(a2)); "
        "Console.WriteLine(d.ContainsKey(p)); "
        "Console.WriteLine(d.Remove(a2)); Console.WriteLine(d.Count); "
        "Console.WriteLine(d.Remove(a)); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    times50("2\nz\ny\nTrue\nFalse\nTrue\n1\nFalse\n"));
}

// Row 3 — the lookup family the issue says #5641 was masking:
// `TryGetValue(k, out var v)`, `GetOrNull`, `TryAdd`, `GetOrAdd`, `Clear`.
BEZEL_TEST(dictionary_inline_array_key_5882,
           try_get_value_get_or_null_try_add_get_or_add_clear_spec_8_3) {
    auto r = runAotChecked(looped(
        "using var d := new Dictionary<[3]int, int>(); "
        "var a: [3]int := [1, 2, 3]; var a2: [3]int := [1, 2, 3]; "
        "var z: [3]int := [0, 0, 0]; "
        "d.Add(a, 4); "
        "var g1 := d.TryGetValue(a2, out var v1); "
        "var g2 := d.TryGetValue(z, out var v2); "
        "Console.WriteLine(g1); Console.WriteLine(v1 ?? -1); "
        "Console.WriteLine(g2); Console.WriteLine(v2 ?? -1); "
        "Console.WriteLine(d.GetOrNull(a2) ?? -1); "
        "Console.WriteLine(d.GetOrNull(z) ?? -1); "
        "Console.WriteLine(d.TryAdd(a2, 9)); Console.WriteLine(d.TryAdd(z, 9)); "
        "Console.WriteLine(d.GetOrAdd(a2, lambda(k) => k[0] + 70)); "
        "var n: [3]int := [5, 6, 7]; "
        "Console.WriteLine(d.GetOrAdd(n, lambda(k) => k[0] + k[1] + k[2])); "
        "Console.WriteLine(d.Count); d.Clear(); Console.WriteLine(d.Count); "
        "Console.WriteLine(d.ContainsKey(a)); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, times50(
        "True\n4\nFalse\n-1\n4\n-1\nFalse\nTrue\n4\n18\n3\n0\nFalse\n"));
}

// Row 4 — the key is STORED BY VALUE: iteration hands back the stored
// elements (entries, `Keys`, `Values`), and mutating the local the key was
// added from does not move the entry.
BEZEL_TEST(dictionary_inline_array_key_5882,
           stored_key_is_a_copy_and_iterates_back_spec_15_1_2) {
    auto r = runAotChecked(looped(
        "using var d := new Dictionary<[3]int, int>(); "
        "var a: [3]int := [1, 2, 3]; var a2: [3]int := [1, 2, 3]; "
        "var b: [3]int := [4, 5, 6]; "
        "d.Add(a, 10); d.Add(b, 20); "
        "a[0] := 99; "
        "Console.WriteLine(d.ContainsKey(a)); Console.WriteLine(d[a2]); "
        "var ks := 0; var vs := 0; "
        "for var e in d do const k: [3]int := e.Key; "
        "  ks := ks + k[0] * 100 + k[1] * 10 + k[2]; vs := vs + e.Value; end; "
        "Console.WriteLine(ks); Console.WriteLine(vs); "
        "var k2s := 0; for var k2 in d.Keys do k2s := k2s + k2[2] + d[k2]; end; "
        "Console.WriteLine(k2s); "
        "var sv := 0; for var v in d.Values do sv := sv + v; end; "
        "Console.WriteLine(sv); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 123 + 456 = 579; 10 + 20 = 30; (3 + 10) + (6 + 20) = 39.
    BEZEL_EXPECT_EQ(r.stdoutText, times50("False\n10\n579\n30\n39\n30\n"));
}

// Row 5 — the SET twin. Pre-fix this one COMPILED and was wrong: the
// array-backed scan did not deduplicate (Count 3 after adding `a`, an equal
// `a2`, and `b`), and `Remove` of an equal array failed.
BEZEL_TEST(dictionary_inline_array_key_5882,
           set_of_inline_arrays_deduplicates_and_removes_spec_8_3) {
    auto r = runAotChecked(looped(
        "using var s := new Set<[3]int>(); "
        "var a: [3]int := [1, 2, 3]; var a2: [3]int := [1, 2, 3]; "
        "var b: [3]int := [3, 2, 1]; var c: [3]int := [9, 9, 9]; "
        "s.Add(a); s.Add(a2); s.Add(b); "
        "Console.WriteLine(s.Count); Console.WriteLine(s.Contains(a2)); "
        "Console.WriteLine(s.Contains(c)); "
        "var sum := 0; for var e in s do sum := sum + e[0] * 10 + e[2]; end; "
        "Console.WriteLine(sum); "
        "Console.WriteLine(s.Remove(a2)); Console.WriteLine(s.Remove(a)); "
        "Console.WriteLine(s.Count); Console.WriteLine(s.Contains(b)); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // (1*10 + 3) + (3*10 + 1) = 44.
    BEZEL_EXPECT_EQ(r.stdoutText,
                    times50("2\nTrue\nFalse\n44\nTrue\nFalse\n1\nTrue\n"));
}

// Row 6 — set algebra over inline-array elements. Pre-fix: a compile-time
// "`ExceptWith` on built-in type `Set` produced no value" refusal.
BEZEL_TEST(dictionary_inline_array_key_5882,
           set_algebra_over_inline_array_elements_spec_8_3) {
    auto r = runAotChecked(looped(
        "using var s := new Set<[2]int>(); using var t := new Set<[2]int>(); "
        "var a: [2]int := [1, 2]; var a2: [2]int := [1, 2]; "
        "var b: [2]int := [3, 4]; var c: [2]int := [5, 6]; "
        "s.Add(a); s.Add(b); t.Add(b); t.Add(c); "
        "Console.WriteLine(s.IsSupersetOf(t)); "
        "s.UnionWith(t); Console.WriteLine(s.Count); "
        "Console.WriteLine(s.IsSupersetOf(t)); "
        "s.ExceptWith(t); Console.WriteLine(s.Count); "
        "Console.WriteLine(s.Contains(a2)); Console.WriteLine(s.Contains(b)); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    times50("False\n3\nTrue\n1\nTrue\nFalse\n"));
}

// Row 7 — element kinds, each under its OWN element equality (§15.1.2
// "pairwise equal under the element type's equality"): a record
// structurally, an enum, `long`/`bool`/`char`, a nullable slot (`[2]int?`,
// null = null), and a class by identity (the same two objects in the other
// order is another key). A `string`-bearing element is Row 16 (#6238).
BEZEL_TEST(dictionary_inline_array_key_5882,
           element_kinds_use_their_own_equality_spec_15_1_2) {
    auto r = runAotChecked(looped(
        "using var dr := new Dictionary<[2]P, int>(); "
        "var r1: [2]P := [new P(1, 2), new P(3, 4)]; "
        "var r2: [2]P := [new P(1, 2), new P(3, 4)]; "
        "var r3: [2]P := [new P(3, 4), new P(1, 2)]; "
        "dr.Add(r1, 1); dr[r2] := 2; dr.Add(r3, 3); "
        "Console.WriteLine(dr.Count * 100 + dr[r1] * 10 + dr[r3]); "
        "using var de := new Dictionary<[2]E, int>(); "
        "var e1: [2]E := [E.A, E.B]; var e2: [2]E := [E.A, E.B]; "
        "var e3: [2]E := [E.B, E.A]; "
        "de.Add(e1, 1); de[e2] := 2; de.Add(e3, 3); "
        "Console.WriteLine(de.Count * 100 + de[e1] * 10 + de[e3]); "
        "using var dl := new Dictionary<[2]long, int>(); "
        "var l1: [2]long := [1L, 2L]; var l2: [2]long := [1L, 2L]; "
        "var l3: [2]long := [2L, 1L]; "
        "dl.Add(l1, 1); dl[l2] := 2; dl.Add(l3, 3); "
        "Console.WriteLine(dl.Count * 100 + dl[l1] * 10 + dl[l3]); "
        "using var db := new Dictionary<[2]bool, int>(); "
        "var b1: [2]bool := [true, false]; var b2: [2]bool := [true, false]; "
        "var b3: [2]bool := [false, true]; "
        "db.Add(b1, 1); db[b2] := 2; db.Add(b3, 3); "
        "Console.WriteLine(db.Count * 100 + db[b1] * 10 + db[b3]); "
        "using var dc := new Dictionary<[2]char, int>(); "
        "var c1: [2]char := ['a', 'b']; var c2: [2]char := ['a', 'b']; "
        "var c3: [2]char := ['b', 'a']; "
        "dc.Add(c1, 1); dc[c2] := 2; dc.Add(c3, 3); "
        "Console.WriteLine(dc.Count * 100 + dc[c1] * 10 + dc[c3]); "
        "using var dn := new Dictionary<[2]int?, int>(); "
        "var n1: [2]int? := [1, null]; var n2: [2]int? := [1, null]; "
        "var n3: [2]int? := [null, 1]; "
        "dn.Add(n1, 1); dn[n2] := 2; dn.Add(n3, 3); "
        "Console.WriteLine(dn.Count * 100 + dn[n1] * 10 + dn[n3]); "
        "using var x1 := new Res(); using var x2 := new Res(); "
        "using var dx := new Dictionary<[2]Res, int>(); "
        "var o1: [2]Res := [x1, x2]; var o2: [2]Res := [x1, x2]; "
        "var o3: [2]Res := [x2, x1]; "
        "dx.Add(o1, 1); dx[o2] := 2; dx.Add(o3, 3); "
        "Console.WriteLine(dx.Count * 100 + dx[o1] * 10 + dx[o3]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    times50("223\n223\n223\n223\n223\n223\n223\n"));
}

// Row 8 — `double` slots follow §4.3.1's reflexive `=` inside the key: a NaN
// slot equals a NaN slot (one entry, overwritten), and `0.0` / `-0.0` are the
// same key.
BEZEL_TEST(dictionary_inline_array_key_5882,
           double_slots_nan_and_signed_zero_spec_4_3_1) {
    auto r = runAotChecked(prog(
        "using var d := new Dictionary<[2]double, int>(); "
        "var z := 0.0; "
        "var a: [2]double := [z / z, 1.0]; var a2: [2]double := [z / z, 1.0]; "
        "var p: [2]double := [0.0, 1.0]; var m: [2]double := [-0.0, 1.0]; "
        "d.Add(a, 1); d[a2] := 2; d.Add(p, 5); d[m] := 6; "
        "Console.WriteLine(d.Count); Console.WriteLine(d[a]); "
        "Console.WriteLine(d[p]); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n2\n6\n"));
}

// Row 9 — the key type reached through other positions: a dictionary FIELD
// with the key as a method parameter, and a generic class `Box<K>` whose `K`
// is substituted with `[2]int` (genericBackingType resolves the parameter to
// the concrete inline array inside the monomorphized body).
BEZEL_TEST(dictionary_inline_array_key_5882,
           field_held_and_generic_substituted_key_spec_8_3) {
    auto r = runAotChecked(looped(
        "var a: [2]int := [1, 2]; var a2: [2]int := [1, 2]; "
        "var b: [2]int := [2, 1]; var c: [2]int := [7, 7]; "
        "using var h := new Holder(); "
        "h.Put(a, 3); h.Put(b, 4); h.Put(a2, 10); "
        "Console.WriteLine(h.Get(a)); Console.WriteLine(h.Get(b)); "
        "Console.WriteLine(h.Has(c)); Console.WriteLine(h.N()); "
        "using var g := new Box<[2]int>(); "
        "g.Put(a, 5); g.Put(b, 6); g.Put(a2, 11); "
        "Console.WriteLine(g.Get(a)); Console.WriteLine(g.Get(b)); "
        "Console.WriteLine(g.Has(c)); Console.WriteLine(g.N()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    times50("10\n4\nFalse\n2\n11\n6\nFalse\n2\n"));
}

// Row 10 — 1000 distinct keys through growth/rehash, then read every one
// back: sum 0..999 = 499500 (stdout, not the exit code — 8-bit truncation).
BEZEL_TEST(dictionary_inline_array_key_5882,
           thousand_keys_survive_rehash_spec_8_3) {
    auto r = runAotChecked(prog(
        "using var d := new Dictionary<[2]int, int>(); "
        "for var i in 0..999 do var k: [2]int := [i mod 37, i / 37]; "
        "  d[k] := i; end; "
        "var sum := 0L; "
        "for var i in 0..999 do var k: [2]int := [i mod 37, i / 37]; "
        "  sum := sum + d[k]; end; "
        "Console.WriteLine(d.Count); Console.WriteLine(sum); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n499500\n"));
}

// Row 11 — §15.4 literals whose key/element is an inline array. The set
// literal COMPILED pre-fix and printed nothing at all.
BEZEL_TEST(dictionary_inline_array_key_5882,
           dictionary_and_set_literals_spec_15_4) {
    auto r = runAotChecked(looped(
        "var a: [2]int := [1, 2]; var b: [2]int := [3, 4]; "
        "var a2: [2]int := [1, 2]; var c: [2]int := [5, 6]; "
        "using var d: Dictionary<[2]int, string> := [a: \"x\", b: \"y\"]; "
        "Console.WriteLine(d.Count); Console.WriteLine(d[a2]); "
        "Console.WriteLine(d.ContainsKey(c)); "
        "using var s: Set<[2]int> := [a, b, a2]; "
        "Console.WriteLine(s.Count); Console.WriteLine(s.Contains(b)); "
        "Console.WriteLine(s.Contains(c)); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    times50("2\nx\nFalse\n2\nTrue\nFalse\n"));
}

// Row 12 — CONTROL, a rejection the fix must NOT lose: the sorted containers
// identify a key by `CompareTo` (stdlib §4.17), which an inline array does not
// have, so `SortedDictionary<[3]int, V>` keeps its `IComparable<K>` refusal
// rather than acquiring a hash identity it was never specified to use.
BEZEL_TEST(dictionary_inline_array_key_5882,
           control_sorted_dictionary_inline_array_key_still_rejected_stdlib_4_17) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new SortedDictionary<[3]int, string>(); "
        "  var a: [3]int := [1, 2, 3]; "
        "  d.Add(a, \"x\"); Console.WriteLine(d.Count); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        src, "requires the key type to implement `IComparable<K>`"));
}

// Row 13 — the admissibility gate the newly generic-backed key relies on.
// §3.6 clause (c): "an inline array of any §15.1 form whose element type is
// `T` or `T?`, where `T` is itself not hashable" is NOT hashable. Pre-fix
// `isEqualityBearing` answered "hashable" for every inline array, so these
// containers were admitted (and, with the key now generic-backed, would have
// run a function value's identity as a hash key). Three spellings — a
// non-hashable record element, a function-typed element, a `T?` element under
// `Set` — plus the hashable controls that must stay admitted.
BEZEL_TEST(dictionary_inline_array_key_5882,
           non_hashable_element_makes_the_key_non_hashable_spec_3_6_c) {
    const std::string nh =
        "module M; "
        "record NH public F: (int) -> int := lambda(x) => x; end NH; "
        "class C public static method Main(): int ";
    const std::string tail = " return 0; end Main; end C;";
    const char* const kNeedle =
        "it is an inline array whose element type is not hashable";
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        nh + "using var d := new Dictionary<[2]NH, int>(); "
             "Console.WriteLine(d.Count);" + tail, kNeedle));
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        nh + "using var d := new Dictionary<[2](int) -> int, int>(); "
             "Console.WriteLine(d.Count);" + tail, kNeedle));
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        nh + "using var s := new Set<[2]NH?>(); "
             "Console.WriteLine(s.Count);" + tail, kNeedle));
    auto ok = runAotChecked(prog(
        "using var d := new Dictionary<[2]P, int>(); "
        "using var e := new Dictionary<[2]int?, int>(); "
        "using var s := new Set<[2]E>(); "
        "Console.WriteLine(d.Count + e.Count + s.Count); "));
    BEZEL_EXPECT_TRUE(ok.ran);
    BEZEL_EXPECT_EQ(ok.stdoutText, std::string("0\n"));
}

// Row 14 — the `Contains` equality-search gate after clause (c) made the inline
// array's hashability recurse. A function element is not hashable (§3.6 clause
// (e)) but IS equality-bearing (§27.7.1: a lambda equals only itself), so
// `Array<[2](int) -> int>.Contains` / `Stack<…>.Contains` / `x in xs` stay
// ADMITTED exactly as the bare `Array<(int) -> int>.Contains` is (the
// reviewer's c5 / w08 shapes, which the first #5882 tip newly refused). A
// non-hashable RECORD element is not equality-bearing either (§9.2), so
// `Array<[2]NH>.Contains` is refused — it compiled pre-fix and answered
// `False`. Only admission is asserted for the function rows: their answer is
// the separate >8-byte inline-array `Contains` defect, #6273.
BEZEL_TEST(dictionary_inline_array_key_5882,
           function_element_contains_admitted_nh_element_refused_spec_3_6_c) {
    const std::string head =
        "module M; "
        "record NH public F: (int) -> int := lambda(x) => x; end NH; "
        "class C public static method Main(): int ";
    const std::string tail = " return 0; end Main; end C;";
    auto fn = runAot(head +
        "var f: (int) -> int := lambda(x) => x; "
        "var a: [2](int) -> int := [f, f]; "
        "var xs: Array<[2](int) -> int> := [a]; "
        "using var st := new Stack<[2](int) -> int>(); st.Push(a); "
        "var r1 := xs.Contains(a); var r2 := st.Contains(a); var r3 := a in xs; "
        "Console.WriteLine(\"ok\");" + tail);
    BEZEL_EXPECT_TRUE(fn.ran);
    BEZEL_EXPECT_EQ(fn.exitCode, 0);
    BEZEL_EXPECT_EQ(fn.stdoutText, std::string("ok\n"));
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        head + "var n := new NH(); var a: [2]NH := [n, n]; "
               "var xs: Array<[2]NH> := [a]; "
               "Console.WriteLine(xs.Contains(a));" + tail,
        "operator `Contains` requires `T: hashable`"));
}

// Row 15 — #6237's limitation, made a compile-time refusal instead of a silent
// loss. A collection literal written DIRECTLY as an inline-array key / element
// (`d[[1, 2]]`, `d.ContainsKey([1, 2])`, `s.Contains([1, 2])`, `[1, 2] in s`)
// is admitted by §15.4 but no key-position lowering materializes it, so with
// the key generic-backed each such operation dropped the store or answered
// `0` / `False` (the reviewer's z6 / l2 and this lane's n01–n17 probes); pre-
// fix every one died in IR verification. The bound-local spelling is Row 11's
// and keeps working.
BEZEL_TEST(dictionary_inline_array_key_5882,
           literal_key_position_refused_not_dropped_pending_6237_spec_15_4) {
    const char* const kNeedle =
        "is not lowered by this compiler (#6237)";
    const std::string pre =
        "using var d := new Dictionary<[2]int, int>(); "
        "using var s := new Set<[2]int>(); "
        "var k: [2]int := [1, 2]; d[k] := 7; s.Add(k); ";
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        prog(pre + "Console.WriteLine(d[[1, 2]]); "), kNeedle));
    BEZEL_EXPECT_TRUE(hasDiagnostic(prog(pre + "d[[3, 4]] := 1; "), kNeedle));
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        prog(pre + "Console.WriteLine(d.ContainsKey([1, 2])); "), kNeedle));
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        prog(pre + "Console.WriteLine(s.Contains([1, 2])); "), kNeedle));
    BEZEL_EXPECT_TRUE(hasDiagnostic(
        prog(pre + "Console.WriteLine([1, 2] in s); "), kNeedle));
}

// Row 16 — #6238 UNCHANGED, pending: a key whose inline-array element carries
// a string (`[2]string`, `[2]string?`, a record with a string field) is NOT
// admitted to the generic-backed path, because the table copies key bytes
// without retaining their strings (7160 B leaked per 50 iterations when it
// was). It keeps its pre-#5882 lowering, which refuses at IR verification;
// this row pins that refusal so admitting the key requires fixing the leak.
BEZEL_TEST(dictionary_inline_array_key_5882,
           string_bearing_element_key_unchanged_pending_6238) {
    auto r = runAot(prog(
        "for var i in 0..49 do "
        "  using var d := new Dictionary<[2]string, int>(); "
        "  var a: [2]string := [$\"a{i}\", \"b\"]; d[a] := 1; "
        "  Console.WriteLine(d.Count); "
        "end; "));
    BEZEL_EXPECT_TRUE(!r.ran);
}
