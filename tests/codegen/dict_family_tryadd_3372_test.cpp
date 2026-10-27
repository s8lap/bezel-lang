// Codegen + sema — fix #3372 / stdlib §4.8, §4.9, §4.17 (v0.282):
// the dictionary family's `TryAdd` — the §1 Try shape (d) "bool-only attempt"
// sibling of `Add`:
//
//   Dictionary<K, V>.TryAdd(key: K, value: V): bool          (stdlib §4.8)
//   OwningDictionary<K, V>.TryAdd(key: K, owned value: V): bool  (stdlib §4.9)
//   SortedDictionary<K, V>.TryAdd(key: K, value: V): bool    (stdlib §4.17)
//
// v0.282 graduated all three (proposal 0119). The compiler implemented none of
// them; measured on the pre-fix build:
//
//   * `Dictionary`/`OwningDictionary` — a hard #2457 reject ("`TryAdd` on
//     built-in type `Dictionary` produced no value, and no dispatch arm emitted
//     any code for this call").
//   * `SortedDictionary` — WORSE than a reject: sema's catch-all V-return arm
//     typed the call as the VALUE type, and codegen's `internal:` degradation
//     note is not an error, so `var ok := sd.TryAdd(k, v)` COMPILED and silently
//     produced a zero-initialized result. With a string-valued map the same
//     mistyping produced malformed IR (a GEP through an `i1`).
//
// The fix is two-layered, and the tests below pin BOTH layers:
//   1. codegen (LlvmBackend_Calls.cpp) — a `TryAdd` arm beside the `Add` arm in
//      the dict-backed handler: probe the key, insert-and-true or leave-and-
//      false, with the owning form disposing the rejected value on the false
//      path (stdlib §4.9 v0.282 cites the `OwningSet<T>.TryAdd` contract of
//      §4.10 verbatim) and the borrowing string-value form taking its §28.9 +1
//      only on the insert edge.
//   2. sema (ExprTypecheck_Calls.cpp) — the result type is `bool` (§1 shape (d)
//      carries no out parameter and no nullability postcondition), the key at
//      pos 0 is non-nullable, and the borrowing forms' value at pos 1 is
//      non-nullable (§7). Codegen alone is NOT enough: without the typing arm
//      the call is left untyped, so `var b: int := d.TryAdd(k, v)` sails past
//      §5.1 and `if sd.TryAdd(k, v)` is rejected as a non-bool condition.
//
// Every case below FAILS on the pre-fix build (compile reject, wrong value, or
// missing diagnostic) and PASSES with the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Plain program wrapper — no resource class.
std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// Destructor-counting resource, for the OwningDictionary disposal oracle.
std::string progR(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.8 (v0.282) — Dictionary<K, V>.TryAdd(key, value): bool
// ---------------------------------------------------------------------------

// stdlib §4.8: a fresh key inserts and returns true; a present key returns
// false and leaves the table UNMUTATED (Count and the stored value both
// unchanged — the second TryAdd must not overwrite, unlike the indexer set).
// Generic-key (int) path.
// Encoded: a*8 + (not b)*4 + (Count = 1)*2 + (d[1] = 10) -> 15.
BEZEL_TEST(dict_family_tryadd_3372, dictionary_generic_key_true_then_false_no_mutation_4_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Dictionary<int, int>(); "
        "var a := d.TryAdd(1, 10); "
        "var b := d.TryAdd(1, 20); "
        "return int(a) * 8 + int(not b) * 4 + int(d.Count = 1) * 2 + "
        "       int(d[1] = 10); ")), 15);
}

// stdlib §4.8: the same contract on the STRING-key fast path
// (bz_dict_contains_str / bz_dict_set_str), which is a separate codegen edge
// from the generic-key one above.
// Encoded: a*8 + (not b)*4 + (Count = 1)*2 + (d["k"] = 10) -> 15.
BEZEL_TEST(dict_family_tryadd_3372, dictionary_string_key_true_then_false_no_mutation_4_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Dictionary<string, int>(); "
        "var a := d.TryAdd(\"k\", 10); "
        "var b := d.TryAdd(\"k\", 20); "
        "return int(a) * 8 + int(not b) * 4 + int(d.Count = 1) * 2 + "
        "       int(d[\"k\"] = 10); ")), 15);
}

// stdlib §4.8 / §1 shape (d): the result is `bool` — usable directly as a
// condition. Pre-fix this was a #2457 compile reject.
BEZEL_TEST(dict_family_tryadd_3372, dictionary_result_is_bool_usable_as_condition_4_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Dictionary<int, int>(); "
        "var n := 0; "
        "if d.TryAdd(1, 10) then n := n + 1; end; "
        "if not d.TryAdd(1, 20) then n := n + 2; end; "
        "return n; ")), 3);
}

// §5.1 + stdlib §4.8 / §1 shape (d): the result is `bool`, so binding it to an
// `int` target is a §5.1 reject. This is the case codegen ALONE does not fix —
// with the lowering arm but no sema typing arm the call is left untyped and
// this program COMPILES (measured), a silent §5.1 hole.
BEZEL_TEST(dict_family_tryadd_3372, dictionary_result_bool_not_assignable_to_int_5_1) {
    auto r = runAot(prog(
        "var d := new Dictionary<int, int>(); "
        "var b: int := d.TryAdd(1, 10); "
        "return b; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("not implicitly convertible") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §7 + stdlib §4.8: `TryAdd(key: K, ...)` declares a NON-nullable key, exactly
// as its `Add` twin does — an un-narrowed `K?` at pos 0 is a §7 reject.
BEZEL_TEST(dict_family_tryadd_3372, dictionary_nullable_key_rejected_spec_7) {
    auto r = runAot(prog(
        "var d := new Dictionary<string, int>(); "
        "var k: string? := null; "
        "var a := d.TryAdd(k, 1); "
        "return int(a); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("TryAdd") != std::string::npos &&
            d.message.find("nullable but the target type is non-nullable") !=
                std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §7 + stdlib §4.8: the borrowing form's VALUE at pos 1 is non-nullable too
// (the owning form's is `owned`, so §22.3 governs it instead).
BEZEL_TEST(dict_family_tryadd_3372, dictionary_nullable_value_rejected_spec_7) {
    auto r = runAot(prog(
        "var d := new Dictionary<int, string>(); "
        "var v: string? := null; "
        "var a := d.TryAdd(1, v); "
        "return int(a); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("nullable but the target type is non-nullable") !=
            std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// #429 / §28.9 + stdlib §4.8: a borrowing Dictionary<K, string> co-owns a +1
// per STORED value. TryAdd takes that +1 only on the INSERT edge — the
// duplicate edge stores nothing, so it must take none (a retain there would
// leak). LSan is the oracle: r.ran flips false on either a leak or a
// double-release.
// Encoded: a*4 + (not b)*2 + (d[1] = "alpha") -> 7.
BEZEL_TEST(dict_family_tryadd_3372, dictionary_string_value_plusone_only_on_insert_28_9) {
    JitResult r = runAotChecked(prog(
        "var d := new Dictionary<int, string>(); "
        "var a := d.TryAdd(1, \"alpha\"); "
        "var b := d.TryAdd(1, \"beta\"); "
        "return int(a) * 4 + int(not b) * 2 + int(d[1] = \"alpha\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------------
// stdlib §4.9 (v0.282) — OwningDictionary<K, V>.TryAdd(key, owned value): bool
// ---------------------------------------------------------------------------

// stdlib §4.9 (v0.282): "false on duplicate, and the passed value is disposed
// on that path — the OwningSet<T>.TryAdd contract (§4.10) verbatim". The
// disposal-count oracle pins that the rejected value is disposed AT THE CALL
// (exactly once, not deferred), and the stored value is not.
// Encoded: a*16 + (not b)*8 + (dispAfterInsert = 0)*4 + (dispAfterDup = 1)*2 +
//          (Count = 1) -> 31.
BEZEL_TEST(dict_family_tryadd_3372, owning_dictionary_false_path_disposes_rejected_value_4_9) {
    BEZEL_EXPECT_EQ(run(progR(
        "using var d := new OwningDictionary<int, R>(); "
        "var a := d.TryAdd(1, new R(10)); "
        "var dispAfterInsert := R._n; "
        "var b := d.TryAdd(1, new R(20)); "
        "var dispAfterDup := R._n; "
        "return int(a) * 16 + int(not b) * 8 + int(dispAfterInsert = 0) * 4 + "
        "       int(dispAfterDup = 1) * 2 + int(d.Count = 1); ")), 31);
}

// stdlib §4.9: the false path must not displace the value already stored under
// that key — the survivor is the FIRST value, and it is disposed exactly once,
// at the dictionary's scope end (not at the rejected TryAdd).
// Encoded: (survivorTag = 10)*4 + dispAtScopeEnd (2 total: rejected + stored).
BEZEL_TEST(dict_family_tryadd_3372, owning_dictionary_false_path_keeps_stored_value_4_9) {
    BEZEL_EXPECT_EQ(runAotOut(progR(
        "var survivorTag := -1; "
        "var mid := -1; "
        "do "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.TryAdd(1, new R(10)); "
        "  d.TryAdd(1, new R(20)); "
        "  survivorTag := d[1].Tag; "
        "  mid := R._n; "
        "end; "
        "Console.WriteLine(survivorTag * 100 + mid * 10 + R._n); "
        "return 0; ")), "1012");
}

// stdlib §4.9 + §28.3/§28.9: neither edge leaks and neither double-frees —
// LSan/ASan is the oracle. The true path transfers the value into the table
// (freed by the dictionary's cascade at scope end); the false path discharges
// the transferred value at the call.
BEZEL_TEST(dict_family_tryadd_3372, owning_dictionary_tryadd_both_edges_leak_free_28_3) {
    JitResult r = runAotChecked(progR(
        "using var d := new OwningDictionary<int, R>(); "
        "var a := d.TryAdd(1, new R(10)); "
        "var b := d.TryAdd(1, new R(20)); "
        "var c := d.TryAdd(2, new R(30)); "
        "return int(a) * 8 + int(not b) * 4 + int(c) * 2 + int(d.Count = 2); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
}

// ---------------------------------------------------------------------------
// stdlib §4.17 (v0.282) — SortedDictionary<K, V>.TryAdd(key, value): bool
// ---------------------------------------------------------------------------

// stdlib §4.17: the same insert-or-decline contract, and the ascending-key
// iteration order §4.17 guarantees is preserved across TryAdd inserts.
// Encoded: a*16 + b*8 + (not c)*4 + (Count = 2)*2 + (sd[2] = 20) -> 31.
BEZEL_TEST(dict_family_tryadd_3372, sorted_dictionary_true_then_false_no_mutation_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "var a := sd.TryAdd(2, 20); "
        "var b := sd.TryAdd(1, 10); "
        "var c := sd.TryAdd(2, 99); "
        "return int(a) * 16 + int(b) * 8 + int(not c) * 4 + "
        "       int(sd.Count = 2) * 2 + int(sd[2] = 20); ")), 31);
}

// stdlib §4.17: iteration still visits entries in ascending key order after a
// TryAdd-built map, and the declined key contributes nothing.
BEZEL_TEST(dict_family_tryadd_3372, sorted_dictionary_tryadd_preserves_ascending_order_4_17) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.TryAdd(3, 30); "
        "sd.TryAdd(1, 10); "
        "sd.TryAdd(2, 20); "
        "sd.TryAdd(1, 99); "
        "var acc := \"\"; "
        "for var e in sd do acc := acc + $\"{e.Key}={e.Value};\"; end; "
        "Console.WriteLine(acc); "
        "return 0; ")), "1=10;2=20;3=30;");
}

// stdlib §4.17 / §1 shape (d) + spec §21.2: the result is `bool`, NOT the value
// type. This is the arm that pins the sema mistyping: pre-fix the catch-all
// V-return arm typed the call as V, so this program was rejected with "the if
// condition must be `bool` (§21.2)".
BEZEL_TEST(dict_family_tryadd_3372, sorted_dictionary_result_is_bool_not_value_type_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "var n := 0; "
        "if sd.TryAdd(1, 10) then n := n + 1; end; "
        "if not sd.TryAdd(1, 20) then n := n + 2; end; "
        "return n; ")), 3);
}

// stdlib §4.17: the same V-mistyping produced MALFORMED IR for a string-valued
// map (a GEP through the `i1` result, LLVM-rejected at run time) — this case
// pins that the result is a plain bool regardless of V.
// Encoded: a*4 + (not b)*2 + (sd[1] = "x") -> 7.
BEZEL_TEST(dict_family_tryadd_3372, sorted_dictionary_string_value_result_is_bool_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sd := new SortedDictionary<int, string>(); "
        "var a := sd.TryAdd(1, \"x\"); "
        "var b := sd.TryAdd(1, \"y\"); "
        "return int(a) * 4 + int(not b) * 2 + int(sd[1] = \"x\"); ")), 7);
}

// §7 + stdlib §4.17: non-nullable key at pos 0, same as the §4.8 twin.
BEZEL_TEST(dict_family_tryadd_3372, sorted_dictionary_nullable_key_rejected_spec_7) {
    auto r = runAot(prog(
        "var sd := new SortedDictionary<string, int>(); "
        "var k: string? := null; "
        "var a := sd.TryAdd(k, 1); "
        "return int(a); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("TryAdd") != std::string::npos &&
            d.message.find("nullable but the target type is non-nullable") !=
                std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// stdlib §4.17 (v0.282) — SortedDictionary<K, V>.Add: the duplicate-is-a-bug
// tier that #3372 also named. NOT a fix in this commit — measured already
// present and correct on the pre-fix build; the cases stand as the pin that
// keeps `Add` and its new `TryAdd` sibling distinguishable (throw vs. decline).
// ---------------------------------------------------------------------------

// stdlib §4.17 (v0.282): "ArgumentException if the key is present".
BEZEL_TEST(dict_family_tryadd_3372, sorted_dictionary_add_duplicate_raises_argument_exception_4_17) {
    BEZEL_EXPECT_EQ(run(prog(
        "var sd := new SortedDictionary<int, int>(); "
        "sd.Add(1, 10); "
        "var caught := 0; "
        "try "
        "  sd.Add(1, 20); "
        "catch "
        "  var e: ArgumentException => do caught := 1; end "
        "end try; "
        "return caught * 4 + int(sd.Count = 1) * 2 + int(sd[1] = 10); ")), 7);
}

// ---------------------------------------------------------------------------
// stdlib §4.9 + spec §22.3 — the `move` transfer shape at the value argument.
// ---------------------------------------------------------------------------

// §22.3: a NAMED owned binding feeding the `owned value: V` parameter must be
// written `move r`. Both edges consume it: the true edge transfers it into the
// table, the false edge disposes it at the call (§4.9 v0.282). Exactly-once on
// both, with no double-free of the moved-from binding at scope end.
// Encoded: a*16 + (not b)*8 + (dispAfterTrue = 0)*4 + (dispAfterFalse = 1)*2 +
//          (Count = 1) -> 31.
BEZEL_TEST(dict_family_tryadd_3372, owning_dictionary_move_argument_both_edges_22_3) {
    BEZEL_EXPECT_EQ(run(progR(
        "using var d := new OwningDictionary<int, R>(); "
        "var r1 := new R(10); "
        "var r2 := new R(20); "
        "var a := d.TryAdd(1, move r1); "
        "var dispAfterTrue := R._n; "
        "var b := d.TryAdd(1, move r2); "
        "var dispAfterFalse := R._n; "
        "return int(a) * 16 + int(not b) * 8 + int(dispAfterTrue = 0) * 4 + "
        "       int(dispAfterFalse = 1) * 2 + int(d.Count = 1); ")), 31);
}
