// Fix #534 — dictionary literal `["k":v, ...]` causes heap corruption / SIGSEGV.
//
// Spec: stdlib §4.8 (Dictionary<K,V>) — a dictionary literal `["a":1, "b":2]`
// must produce a correctly-initialized Dictionary<K,V> identical in observable
// behaviour to the equivalent `new Dictionary<K,V>()` followed by .Add calls.
//
// Root cause (pre-fix): `lowerLocalDecl` in LlvmBackend_Setup.cpp had two early
// guards `!cl->isDict` for the heap-collection and inline-array paths, so a dict
// literal fell through to `lowerExpr`, which had no CollectionLiteralExpr case
// and returned nullptr.  The slot was then left uninitialized (an alloca'd ptr
// with whatever was on the stack). Reading `Count` from that garbage pointer
// yielded ~1.2×10⁹ and any subsequent access crashed.
//
// Fix (LlvmBackend_Setup.cpp): add a new helper
// `materializeHeapDictLiteral(cl, dictTy)` that:
//   1. allocates via the same bz_dict_alloc / bz_dict_alloc_generic the
//      `new Dictionary<K,V>()` lowering uses;
//   2. for each (key, value) pair in the literal, calls the same
//      bz_dict_set_str / bz_dict_set_gen the `.Add(k, v)` lowering uses.
// `lowerLocalDecl` now calls this helper when `cl->isDict && declType` resolves
// to a Dictionary/OwningDictionary TypeRef.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
// #1297: a program that must be rejected by the front-end (typecheck) — it does
// not run and reports at least one diagnostic.
bool rejectedAtCompile(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}
}  // namespace

// ---------------------------------------------------------------------------
// Core regression: string-keyed dictionary literal — Count, lookup, containment
// These were the crash scenario in #534.
// ---------------------------------------------------------------------------

// #534 / stdlib §4.8: `["a":1, "b":2]` — Count==2, values correct, no crash.
BEZEL_TEST(dict_literal_534, string_key_count_and_lookup) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "  var va: int? := null; var vb: int? := null; "  // v0.171 (#1336): out is V?
        "  d.TryGetValue(\"a\", out va); "
        "  d.TryGetValue(\"b\", out vb); "
        "  return int(d.Count = 2 and va = 1 and vb = 2); "
        "end Main; end C;"), 1);
}

// #534 / stdlib §4.8: ContainsKey works correctly on literal-initialized dict.
BEZEL_TEST(dict_literal_534, string_key_contains) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := [\"x\": 10, \"y\": 20]; "
        "  return int(d.ContainsKey(\"x\") and d.ContainsKey(\"y\") and not d.ContainsKey(\"z\")); "
        "end Main; end C;"), 1);
}

// #534 / stdlib §4.8: three-entry literal — correct Count and all values.
BEZEL_TEST(dict_literal_534, string_key_three_entries) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := [\"one\": 1, \"two\": 2, \"three\": 3]; "
        "  var v1: int? := null; var v2: int? := null; var v3: int? := null; "  // v0.171 (#1336)
        "  d.TryGetValue(\"one\",   out v1); "
        "  d.TryGetValue(\"two\",   out v2); "
        "  d.TryGetValue(\"three\", out v3); "
        "  return int(d.Count = 3 and v1 = 1 and v2 = 2 and v3 = 3); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// Integer-key dictionary literal (generic-key runtime path)
// ---------------------------------------------------------------------------

// #534 / stdlib §4.8: `[1:10, 2:20]` with int keys — generic-key path.
BEZEL_TEST(dict_literal_534, int_key_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<int, int> := [1: 10, 2: 20]; "
        "  var v1: int? := null; var v2: int? := null; "  // v0.171 (#1336)
        "  d.TryGetValue(1, out v1); "
        "  d.TryGetValue(2, out v2); "
        "  return int(d.Count = 2 and v1 = 10 and v2 = 20); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// Larger literal — forces a resize of the backing hash table
// ---------------------------------------------------------------------------

// #534 / stdlib §4.8: 20-entry literal exceeds default table capacity (16),
// so the runtime must resize.  All entries must survive correctly.
BEZEL_TEST(dict_literal_534, large_literal_forces_resize) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := "
        "    [\"k01\": 1, \"k02\": 2, \"k03\": 3, \"k04\": 4, \"k05\": 5, "
        "     \"k06\": 6, \"k07\": 7, \"k08\": 8, \"k09\": 9, \"k10\": 10, "
        "     \"k11\": 11, \"k12\": 12, \"k13\": 13, \"k14\": 14, \"k15\": 15, "
        "     \"k16\": 16, \"k17\": 17, \"k18\": 18, \"k19\": 19, \"k20\": 20]; "
        "  var v1: int? := null; var v10: int? := null; var v20: int? := null; "  // v0.171 (#1336)
        "  d.TryGetValue(\"k01\",  out v1); "
        "  d.TryGetValue(\"k10\", out v10); "
        "  d.TryGetValue(\"k20\", out v20); "
        "  return int(d.Count = 20 and v1 = 1 and v10 = 10 and v20 = 20); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// Control: new()+Add still works (regression guard)
// ---------------------------------------------------------------------------

// Ensure the baseline new()+Add path is undisturbed by the fix.
BEZEL_TEST(dict_literal_534, control_new_plus_add) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 1); d.Add(\"b\", 2); "
        "  var va: int? := null; var vb: int? := null; "  // v0.171 (#1336)
        "  d.TryGetValue(\"a\", out va); "
        "  d.TryGetValue(\"b\", out vb); "
        "  return int(d.Count = 2 and va = 1 and vb = 2); "
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// Collateral: Array literal still works (not broken by the dict fix)
// ---------------------------------------------------------------------------

// Array literal unaffected by the dict-literal fix.
BEZEL_TEST(dict_literal_534, array_literal_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a: Array<int> := [10, 20, 30]; "
        "  return int(a.Count = 3 and a.Get(0) = 10 and a.Get(2) = 30); "
        "end Main; end C;"), 1);
}

// ── §15.4 (R39): empty `[]` with a Dictionary target → empty dictionary ──────
// The empty literal parses as a NON-dict CollectionLiteralExpr; with a
// Dictionary target it must construct an empty dict, not leave the slot
// uninitialized. Pre-fix `using var d: Dictionary<…> := []` free()d a garbage
// pointer at scope end → SIGABRT.

BEZEL_TEST(dict_literal_534, empty_dict_literal_local_count_zero_r39) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := []; "
        "  return d.Count; "  // 0, and no crash at scope end
        "end Main; end C;"), 0);
}

BEZEL_TEST(dict_literal_534, empty_dict_literal_then_add_r39) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := []; "
        "  d.Add(\"a\", 5); d.Add(\"b\", 7); "
        "  return d[\"a\"] + d[\"b\"] + d.Count; "  // 5+7+2 = 14
        "end Main; end C;"), 14);
}

BEZEL_TEST(dict_literal_534, empty_dict_literal_int_key_r39) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<int, int> := []; "
        "  d.Add(1, 100); return d[1] + d.Count; "  // 100 + 1
        "end Main; end C;"), 101);
}

// Empty `[]` returned where a Dictionary is expected (return path delegates
// through the same helper).
BEZEL_TEST(dict_literal_534, empty_dict_literal_return_r39) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(): owned Dictionary<string, int> return []; end Make; "
        "  public static method Main(): int "
        "    using var d := Make(); return d.Count; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// Empty `[]` as a Dictionary FIELD initializer.
BEZEL_TEST(dict_literal_534, empty_dict_literal_field_r39) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box public m: Dictionary<string, int> := []; end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); b.m.Add(\"x\", 3); return b.m[\"x\"]; "  // 3
        "end Main; end C;"), 3);
}

// Regression: an empty `[]` with an Array target is still an empty array.
BEZEL_TEST(dict_literal_534, empty_array_literal_still_array_r39) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a: Array<int> := []; a.Add(9); "
        "  return a.Get(0) + a.Count; "  // 9 + 1
        "end Main; end C;"), 10);
}

// ---------------------------------------------------------------------------
// §15.4 (#1297): duplicate compile-time-constant keys in a dictionary literal
// are a compile error (not silent runtime last-wins). Only non-statically-
// detectable duplicates fall back to last-wins.
// ---------------------------------------------------------------------------

// §15.4: duplicate int-literal keys are rejected at compile time.
BEZEL_TEST(dict_literal_534, duplicate_int_const_key_rejected_1297) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<int, int> := [1: 10, 2: 20, 1: 99]; "
        "  return d.Count; "
        "end Main; end C;"));
}

// §15.4: duplicate string-literal keys are rejected.
BEZEL_TEST(dict_literal_534, duplicate_string_const_key_rejected_1297) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2, \"a\": 3]; "
        "  return d.Count; "
        "end Main; end C;"));
}

// §15.4: the numeric value is compared, so `0x1` duplicates `1`.
BEZEL_TEST(dict_literal_534, duplicate_int_key_hex_vs_dec_rejected_1297) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<int, int> := [1: 10, 0x1: 99]; "
        "  return d.Count; "
        "end Main; end C;"));
}

// §15.4: distinct constant keys are NOT rejected (control) — Count == 2.
BEZEL_TEST(dict_literal_534, distinct_const_keys_accepted_1297) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d: Dictionary<int, int> := [1: 10, 2: 99]; "
        "  return d.Count; "  // 2
        "end Main; end C;"), 2);
}

// §15.4: NON-constant duplicate keys are NOT statically detectable, so the
// literal is accepted and runtime last-wins applies (control) — Count == 1.
BEZEL_TEST(dict_literal_534, nonconst_duplicate_keys_last_wins_1297) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var k1 := 1; var k2 := 1; "
        "  using var d: Dictionary<int, int> := [k1: 10, k2: 99]; "
        "  return d.Count; "  // 1 (runtime last-wins)
        "end Main; end C;"), 1);
}
