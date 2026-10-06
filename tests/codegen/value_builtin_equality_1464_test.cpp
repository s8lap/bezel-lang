// Codegen — value-builtin (`Guid`) equality (#1464, spec §9.6, stdlib §2.6).
//
// `Guid` is a 128-bit value record (stdlib §2.6) that lowers to the inline
// struct `{ i64 lo, i64 hi }`. Equality (§9.6 records / §8.3) must compare it
// STRUCTURALLY (its two words), never via a raw `icmp` on the whole struct
// operand — `ICmpInst` accepts only int/ptr operands, so a struct reaching
// `CreateICmpEQ` tripped the LLVM `ICmpInst::AssertOK` assertion and crashed
// bezelc (#1464).
//
// Two crash sites shared the value-builtin-equality class:
//   * the `=` / `<>` operator (already lowered structurally — regression guard);
//   * the per-key-type equality trampoline the JIT builds for a `Dictionary<K,V>`
//     or `Set<K>` whose key is a value-builtin struct (the actual #1464 crash):
//     `buildValueEqTrampoline` fell through to `CreateICmpEQ` on the loaded
//     `{i64,i64}` struct. The companion hash trampoline hashed a `Guid` to the
//     constant 0 (every key in one bucket) — the `distinct_*` cases below fail
//     if the hash is degenerate or the equality is wrong.
//
// Observed via the int exit code (JIT).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body of `C.Main(): int`, wrapped in a module + class.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// The `=` / `<>` operator on `Guid` operands (§9.6 / §8.3). Regression guard:
// direct value-builtin equality already lowers structurally; keep it covered.
// ---------------------------------------------------------------------------

BEZEL_TEST(value_builtin_eq_1464, guid_empty_equals_empty) {
    // Two `Guid.Empty` values are equal (both all-zero, §2.6).
    BEZEL_EXPECT_EQ(run(
        "var a := Guid.Empty; var b := Guid.Empty; "
        "if a = b then return 1; end; return 0;"), 1);
}

BEZEL_TEST(value_builtin_eq_1464, guid_distinct_not_equal) {
    // Two freshly generated Guids are (overwhelmingly) distinct → `=` is false.
    BEZEL_EXPECT_EQ(run(
        "var a := Guid.NewGuid(); var b := Guid.NewGuid(); "
        "if a = b then return 1; end; return 0;"), 0);
}

BEZEL_TEST(value_builtin_eq_1464, guid_not_equal_operator) {
    // `<>` is the negation of `=`: distinct Guids satisfy `<>`.
    BEZEL_EXPECT_EQ(run(
        "var a := Guid.NewGuid(); var b := Guid.NewGuid(); "
        "if a <> b then return 1; end; return 0;"), 1);
}

BEZEL_TEST(value_builtin_eq_1464, guid_local_vs_empty) {
    // A parsed all-zero Guid equals `Guid.Empty` (structural, not identity).
    BEZEL_EXPECT_EQ(run(
        "var a := Guid.Parse(\"00000000-0000-0000-0000-000000000000\"); "
        "if a = Guid.Empty then return 1; end; return 0;"), 1);
}

// ---------------------------------------------------------------------------
// `Guid` as a `Dictionary<K,V>` key — the value-key equality/hash trampolines
// (#1464: buildValueEqTrampoline crashed on the `{i64,i64}` struct).
// ---------------------------------------------------------------------------

BEZEL_TEST(value_builtin_eq_1464, dict_guid_key_lookup) {
    // Store under Guid.Empty, read it back by an equal key.
    BEZEL_EXPECT_EQ(run(
        "using var d := new Dictionary<Guid, int>(); "
        "d.Add(Guid.Empty, 42); return d[Guid.Empty];"), 42);
}

BEZEL_TEST(value_builtin_eq_1464, dict_guid_distinct_keys) {
    // Distinct keys map to distinct slots (fails if the hash is degenerate OR
    // the key equality is wrong): 7 + 9 = 16.
    BEZEL_EXPECT_EQ(run(
        "using var d := new Dictionary<Guid, int>(); "
        "var g := Guid.NewGuid(); "
        "d.Add(Guid.Empty, 7); d.Add(g, 9); "
        "return d[Guid.Empty] + d[g];"), 16);
}

BEZEL_TEST(value_builtin_eq_1464, dict_guid_contains_key) {
    // ContainsKey: hit on an equal key, miss on a fresh one → 5.
    // v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `(when …)`
    // expression is retired — replaced with the equivalent `(if … then … else …)`.
    BEZEL_EXPECT_EQ(run(
        "using var d := new Dictionary<Guid, int>(); "
        "d.Add(Guid.Empty, 1); "
        "var hit  := (if d.ContainsKey(Guid.Empty)   then 5   else 0); "
        "var miss := (if d.ContainsKey(Guid.NewGuid()) then 100 else 0); "
        "return hit + miss;"), 5);
}

// ---------------------------------------------------------------------------
// `Guid` as a `Set<K>` element — same value-key trampoline path.
// ---------------------------------------------------------------------------

BEZEL_TEST(value_builtin_eq_1464, set_guid_membership) {
    // Add Empty and one fresh Guid; Contains is true for both, false for a
    // third fresh one → 1 + 2 = 3.
    BEZEL_EXPECT_EQ(run(
        "using var s := new Set<Guid>(); "
        "var g := Guid.NewGuid(); "
        "s.Add(Guid.Empty); s.Add(g); "
        "var r := 0; "
        "if s.Contains(Guid.Empty) then r := r + 1; end; "
        "if s.Contains(g) then r := r + 2; end; "
        "if s.Contains(Guid.NewGuid()) then r := r + 100; end; "
        "return r;"), 3);
}

// ---------------------------------------------------------------------------
// `Guid` is usable as a declared type (stdlib §2.6) — param / field / return /
// annotation. Before #1464 these errored "unresolved type name `Guid`", so the
// value-builtin equality above could only be reached through inference.
// ---------------------------------------------------------------------------

BEZEL_TEST(value_builtin_eq_1464, guid_param_annotation_equality) {
    auto r = runAot(
        "module M; class C "
        "  public static method Eq(a: Guid, b: Guid): int "
        "    if a = b then return 1; end; return 0; "
        "  end Eq; "
        "  public static method Main(): int "
        "    var g: Guid := Guid.Empty; "
        "    return Eq(g, Guid.Empty); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
