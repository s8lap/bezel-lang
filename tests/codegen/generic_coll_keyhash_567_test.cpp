// Codegen — a generic method that constructs / uses a hash-backed stdlib
// collection (Dictionary<K,V> / Set<T>) keyed by a TYPE PARAMETER, then called
// with a concrete key/element type (issue #567).
//
// §14.1 (generic monomorphization): a generic method body is monomorphized at
// each call site; the type parameter is substituted by the concrete argument.
// stdlib §4.8 (Dictionary) / §4.10 (Set): a non-string key/element is backed by
// the generic-key hash table whose hash + equality follow the key/element type's
// contract (§8.3); a string key keeps the dedicated string fast path.
//
// Root cause (before fix): the collection created INSIDE the generic body was
// initialized from the type parameter's PLACEHOLDER (an unresolved single-part
// Named TypeRef with no declaration). genericBackingType read that placeholder
// directly, so:
//   * In the UNSPECIALIZED template body emitted by emitFunctionBody (no active
//     substitution) the placeholder lowered to i32 but routed through the STRING
//     fast path — which GEP'd the i32 key as a %bz.string pointer → IR
//     verification failure ("GEP base pointer is not a vector or a vector of
//     pointers" / "Load operand must be a pointer") — Symptom A.
//   * After monomorphization the collection was allocated array-backed (the
//     placeholder was not eligible for the generic-key path), while the caller's
//     concrete-type Contains/indexer dispatch used the generic hash entry points
//     → reading array memory as a dict → SIGSEGV — Symptom B (Count, a field
//     read, worked; the hash ops crashed).
//
// After fix (codegen): genericBackingType resolves a type-parameter genericArg
// through the active monomorphization binding (genericSubstSemaTy_) to the
// concrete key/element type, so eligibility and the hash/eq trampolines follow
// the CONCRETE type; an unresolved type parameter (the dead template body) routes
// through the i32-keyed generic path so the template still passes IR
// verification. Each real call site sees the concrete substituted key/element.
//
// Each test ran (IR-verify failure / SIGSEGV) before the fix and produces the
// spec-stated value after.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::JitResult jit(const std::string& prog) { return runAot(prog); }

}  // namespace

// ── §14.1 / §4.8 / Symptom A: Dictionary<K,int> built in a generic method ─────

// Util.MakeDict<int> then read back via the indexer. Before fix: IR verification
// failure (i32 key routed through the %bz.string GEP path). After: d[1] == 42.
BEZEL_TEST(codegen_generic_coll_keyhash_567, make_dict_int_key_indexer_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method MakeDict<K>(k: K, v: int): owned Dictionary<K, int> "
        "      where K: notnull "
        "    using var d := new Dictionary<K, int>(); "
        "    d[k] := v; "
        "    return d; "
        "  end MakeDict; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var d: Dictionary<int, int> := Util.MakeDict<int>(1, 42); "
        "  return d[1]; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §4.8: ContainsKey on the dict built by the generic method — the membership
// query dispatches through the concrete int key's hash/eq, returning true.
BEZEL_TEST(codegen_generic_coll_keyhash_567, make_dict_int_key_containskey_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method MakeDict<K>(k: K, v: int): owned Dictionary<K, int> "
        "      where K: notnull "
        "    using var d := new Dictionary<K, int>(); "
        "    d[k] := v; "
        "    return d; "
        "  end MakeDict; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var d: Dictionary<int, int> := Util.MakeDict<int>(7, 1); "
        "  if d.ContainsKey(7) then return 5; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ── §14.1 / §4.8 / string key keeps the dedicated fast path ───────────────────

// Util.MakeDict<string> — the concrete substitution is `string`, so the dict
// must use the string fast path (NOT the generic placeholder). d["hi"] == 99.
BEZEL_TEST(codegen_generic_coll_keyhash_567, make_dict_string_key_indexer_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method MakeDict<K>(k: K, v: int): owned Dictionary<K, int> "
        "      where K: notnull "
        "    using var d := new Dictionary<K, int>(); "
        "    d[k] := v; "
        "    return d; "
        "  end MakeDict; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := Util.MakeDict<string>(\"hi\", 99); "
        "  return d[\"hi\"]; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ── §14.1 / §4.8 / record key ─────────────────────────────────────────────────

// Util.MakeDict<Pt> with a record key — the generic-key hash/eq trampolines must
// fold the record's field hashes (§9.1) so a structurally-equal lookup key hits.
BEZEL_TEST(codegen_generic_coll_keyhash_567, make_dict_record_key_indexer_14_1) {
    auto r = jit(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Util "
        "  public static method MakeDict<K>(k: K, v: int): owned Dictionary<K, int> "
        "      where K: notnull "
        "    using var d := new Dictionary<K, int>(); "
        "    d[k] := v; "
        "    return d; "
        "  end MakeDict; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var d: Dictionary<Pt, int> := Util.MakeDict<Pt>(new Pt(1, 2), 55); "
        "  return d[new Pt(1, 2)]; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// ── §14.1 / §4.10 / Symptom B: Set<T> built/returned by a generic method ──────

// Util.Wrap<int> then Contains(3). Before fix: SIGSEGV (the set was array-backed
// inside the body, but the caller's Contains used the generic hash entry point).
// After: Contains(3) is true → 7.
BEZEL_TEST(codegen_generic_coll_keyhash_567, wrap_set_int_contains_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(a: T): owned Set<T> where T: notnull "
        "    var s := new Set<T>(); "
        "    s.Add(a); "
        "    return s; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var s: Set<int> := Util.Wrap<int>(3); "
        "  if s.Contains(3) then return 7; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §4.10: Count works (a field read) AND a NON-member query returns false. The
// element absent from the set must not match — dedup/equality is by the concrete
// int's contract, so Contains(9) is false; Contains(3) is true.
BEZEL_TEST(codegen_generic_coll_keyhash_567, wrap_set_int_count_and_absent_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(a: T): owned Set<T> where T: notnull "
        "    var s := new Set<T>(); "
        "    s.Add(a); "
        "    return s; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var s: Set<int> := Util.Wrap<int>(3); "
        "  if s.Count = 1 and s.Contains(3) and not s.Contains(9) then return 8; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// ── §14.1 / §4.10 / string element ────────────────────────────────────────────

// Util.Wrap<string> — set element is a string; Contains by CONTENT (§4.6/§8.3).
BEZEL_TEST(codegen_generic_coll_keyhash_567, wrap_set_string_contains_14_1) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(a: T): owned Set<T> where T: notnull "
        "    var s := new Set<T>(); "
        "    s.Add(a); "
        "    return s; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var s: Set<string> := Util.Wrap<string>(\"yo\"); "
        "  if s.Contains(\"yo\") then return 11; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ── §14.1 / §4.10 / record element ────────────────────────────────────────────

// Util.Wrap<Pt> — set element is a record; Contains by structural equality (§9.1).
BEZEL_TEST(codegen_generic_coll_keyhash_567, wrap_set_record_contains_14_1) {
    auto r = jit(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Util "
        "  public static method Wrap<T>(a: T): owned Set<T> where T: notnull "
        "    var s := new Set<T>(); "
        "    s.Add(a); "
        "    return s; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var s: Set<Pt> := Util.Wrap<Pt>(new Pt(3, 4)); "
        "  if s.Contains(new Pt(3, 4)) then return 13; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ── §14.1: two distinct instantiations from the same generic method ───────────

// Both Set<int> and Set<string> from the SAME Wrap<T> must each dispatch through
// their own concrete element's hash/eq (distinct monomorphizations, §14.2).
BEZEL_TEST(codegen_generic_coll_keyhash_567, wrap_set_two_instantiations_14_2) {
    auto r = jit(
        "module M; "
        "class Util "
        "  public static method Wrap<T>(a: T): owned Set<T> where T: notnull "
        "    var s := new Set<T>(); "
        "    s.Add(a); "
        "    return s; "
        "  end Wrap; "
        "end Util; "
        "class C public static method Main(): int "
        "  using var si: Set<int> := Util.Wrap<int>(4); "
        "  using var ss: Set<string> := Util.Wrap<string>(\"k\"); "
        "  if si.Contains(4) and ss.Contains(\"k\") then return 9; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}
