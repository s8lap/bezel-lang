// Spec-derived regression test for issue #3399.
//
// AUTHORITY. spec §7 (nullable -> non-nullable) and stdlib §4.17
// `SortedDictionary<K, V>` (v0.283), which declares, verbatim at
// Bezel_Standard_Library.md:3322-3326:
//
//     public method Add(key: K, value: V);          // v0.282: ArgumentException if the key is present …
//     public method TryAdd(key: K, value: V): bool; // v0.282 (§1 shape (d)): false and no mutation if present
//
// Both slots of `Add` are non-nullable, exactly as on the `Dictionary<K, V>`
// twin (§4.8 line 1962).
//
// THE DEFECT. `checkBuiltinCollectionCallArgs` carries two positive name lists —
// the pos-0 key gate inside `isItemPosition` and the pos-1 `isDictValuePosition`
// lambda — and `SortedDictionary` was absent from the `Add` entry of BOTH. So on
// 0c5ecd1:
//
//     var sd := new SortedDictionary<string, int>();
//     var k: string? := null;
//     sd.Add(k, 1);            // accepted
//
//     var sd2 := new SortedDictionary<int, string>();
//     var v: string? := null;
//     sd2.Add(1, v);           // accepted
//
// while the `Dictionary` spellings of both are rejected, and — the sharper
// inconsistency — `SortedDictionary`'s OWN indexer-set path already rejects
// both ("the assigned value is nullable but the target type is non-nullable
// (§7)" and "the index argument to `SortedDictionary[key]` is nullable …").
//
// WHY THE GUARD FIRING IS RIGHT FOR THIS TYPE, not merely a name to add. A
// missing name in a positive list can be load-bearing: #3444 records the case
// where adding one would SUPPRESS a diagnostic and let an unlowered path
// through, converting a loud error into a silent miscompile. The direction here
// is the opposite: this list selects a `checkAssignable` call, i.e. it ADDS a
// rejection, so its dominant risk is over-rejection — which the second half of
// this file measures at zero.
//
// ⚠ ONE QUALIFIER, found by per-fix review and recorded because an earlier
// revision of this comment stated the stronger claim flatly. `checkAssignable`
// is NOT side-effect-free: `ExprTypecheck.cpp:4095` calls `ctx_.setExprType(...)`
// on the §27.6 property-getter-reference path (gated `to->kind == Function`).
// So this list CAN reach a typing side effect. Measured, that direction is
// strictly toward correctness — `SortedDictionary<int, () -> int>.Add(1, p.Q)`
// with an `int` property `Q` went from rc 139 (a SEGV, storing an i32 into a
// function slot) to the correct value, converging on the `Dictionary` twin
// which was already right. No shape exists where "before" was correct and
// "after" is not, because that branch requires `from->kind != Function`.
// That arm is currently UNTESTED; it is a newly-reached path.
//
// And the accepts this list was leaking are not benign — but they are not
// symmetric either. MEASURED on 0c5ecd1: the KEY program SEGVs at the `Add`
// itself (`AddressSanitizer: SEGV on unknown address 0x0 … READ`, rc 139); the
// VALUE program does NOT — it compiles, stores the null, returns `Count == 1`,
// and is ASan-clean, faulting only on the first read-back. Saying both "SEGV at
// run time" sends the next reader hunting a fault one of them never produces.
//
// WHY IT WAS MISSED. #2582's own comment in this file asserted, of the pos-1
// gate, that "`SortedDictionary<K, V>` (§4.17 line 2999-3017) declares NO
// `Add(key, value)` at all — its write surface is the indexer-set". That was
// TRUE when written and became false at v0.282, when proposal 0119 restored the
// throwing `Add` to the sorted map. #3372 then added the sibling `TryAdd` to
// both lambdas without re-reading the bullet beside them. The fix corrects the
// comment as well as the lists; a stale comment that reads as a justification is
// what made this a two-revision defect rather than a one-line omission.
//
// Every rejection row asserts on diagnostic TEXT. The second half is the
// false-reject guard: each of those rows compiles AND produces its value on the
// PRE-fix compiler too, which is what makes it a control rather than a
// restatement of the fix.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

std::string sema(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class R public X: int := 0; end R; "
           "class C "
           "  private static method NI(): int? => null; "
           "  private static method NS(): string? => null; "
           "  public static method T(): int " + body + " end T; "
           "end C;";
}

std::string aot(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class R public X: int := 0; end R; "
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ── The two unguarded slots now reject (§7 / stdlib §4.17, v0.283) ───────────

// per §7 / stdlib §4.17 (v0.283), #3399: the issue's first repro — an
// un-narrowed `string?` at `Add`'s pos-0 `key: K`.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorted_dictionary_add_key_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<string, int>(); "
        "    sd.Add(NS(), 1); "
        "    return sd.Count; "));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "SortedDictionary<T>.Add"));
    BEZEL_EXPECT_TRUE(hasError(r, "(\xC2\xA7""7)"));
}

// per §7 / stdlib §4.17 (v0.283), #3399: the issue's second repro — an
// un-narrowed `string?` at `Add`'s pos-1 `value: V`. The key here is a plain
// non-nullable `int`, so the pos-1 gate is the ONLY thing that can produce the
// diagnostic; without it this row cannot go green.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorted_dictionary_add_value_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, string>(); "
        "    sd.Add(1, NS()); "
        "    return sd.Count; "));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "SortedDictionary<T>.Add"));
}

// per §7 / stdlib §4.17 (v0.283), #3399: a CLASS-typed `V`. §4.17 says values
// are "borrowed for class types" and declares a plain non-nullable `V` for
// them — the borrowing family does not wrap a class value in `V?` — so an
// un-narrowed `R?` is rejected exactly as a `string?` is.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorted_dictionary_add_class_value_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, R>(); "
        "    var q: R? := null; "
        "    sd.Add(1, q); "
        "    return sd.Count; "));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "SortedDictionary<T>.Add"));
}

// per §7 / stdlib §4.17 (v0.283), #3399: both slots at once still names `Add`
// and reports the §7 rule — the two gates are independent, and neither
// swallows the other.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorted_dictionary_add_both_slots_reject) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<string, string>(); "
        "    sd.Add(NS(), NS()); "
        "    return sd.Count; "));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "SortedDictionary<T>.Add"));
}

// ── FALSE-REJECT GUARD ───────────────────────────────────────────────────────
//
// Every row below compiles and returns the same value on the PRE-fix compiler.

// per §7 / stdlib §4.17 (v0.283), #3399: the ordinary non-nullable call. If the
// new gate over-fired on the common shape nothing else would matter.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorted_dictionary_add_nonnullable_still_compiles) {
    BEZEL_EXPECT_EQ(run(aot(
        "  using var sd := new SortedDictionary<string, int>(); "
        "  sd.Add(\"b\", 2); sd.Add(\"a\", 1); "
        "  return sd.Count * 10 + sd[\"a\"]; ")),
        21);
}

// per §7.4 / §12.7 (#914) / stdlib §4.17 (v0.283), #3399: a NARROWED nullable is
// admissible — the narrowing exception lives inside `checkAssignable`, and a
// gate that ignored it would reject correct code. Both slots are narrowed here.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorted_dictionary_add_narrowed_nullable_still_compiles) {
    BEZEL_EXPECT_EQ(run(aot(
        "  using var sd := new SortedDictionary<string, string>(); "
        "  var k: string? := \"k\"; "
        "  var v: string? := \"vv\"; "
        "  if k <> null and v <> null then sd.Add(k, v); end; "
        "  return sd.Count * 10 + sd[\"k\"].Length; ")),
        12);
}

// per §7.10 (v0.159) / stdlib §4.17 (v0.283), #3399: the check is against the
// INSTANTIATED `K`/`V`, so a `SortedDictionary<string?, …>` — whose declared key
// IS `string?` — keeps accepting an un-narrowed `string?`. This is the row a
// naive "reject any nullable argument at pos 0" gate breaks.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           nullable_key_instantiation_still_accepts_nullable) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<string?, int>(); "
        "    sd.Add(NS(), 1); "
        "    return sd.Count; "));
    BEZEL_EXPECT_FALSE(hasError(r, kNullableDiag));
}

// per §7.10 (v0.159) / stdlib §4.17 (v0.283), #3399: the same for a nullable
// VALUE instantiation at pos 1.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           nullable_value_instantiation_still_accepts_nullable) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, int?>(); "
        "    sd.Add(1, NI()); "
        "    return sd.Count; "));
    BEZEL_EXPECT_FALSE(hasError(r, kNullableDiag));
}

// per §7 / stdlib §4.8 (v0.283), #3399: the `Dictionary` twin the fix shares
// these two lambdas with must keep rejecting — the widening must not have
// disturbed the name it already carried.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           dictionary_add_still_rejects) {
    auto r = parseSemaAll(sema(
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(NS(), 1); "
        "    return d.Count; "));
    BEZEL_EXPECT_TRUE(hasError(r, "Dictionary<T>.Add"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
}

// per §7 / stdlib §4.17 (v0.283), #3372 / #3399: `SortedDictionary.TryAdd` was
// already on both lists (#3372) and must stay there — a widening that replaced
// rather than extended the entry would silently drop it.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorted_dictionary_tryadd_still_rejects_both_slots) {
    auto rk = parseSemaAll(sema(
        "    var sd := new SortedDictionary<string, int>(); "
        "    var b := sd.TryAdd(NS(), 1); "
        "    return sd.Count; "));
    BEZEL_EXPECT_TRUE(hasError(rk, "SortedDictionary<T>.TryAdd"));
    auto rv = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, string>(); "
        "    var b := sd.TryAdd(1, NS()); "
        "    return sd.Count; "));
    BEZEL_EXPECT_TRUE(hasError(rv, "SortedDictionary<T>.TryAdd"));
}

// per §22.3 / stdlib §4.9 (v0.283), #3399: `OwningDictionary<K, V>.Add(key: K,
// owned value: V)` marks the value `owned`, so §22.3 governs that slot and the
// modifier guard skips it. The widening is scoped to the two BORROWING maps and
// must not have reached the owning one's value. (No `owned` marker is written
// at the call site here: §22.3 v0.169 rejects the marker on a fresh producer —
// "no live binding is invalidated, so there is nothing to announce".)
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           owning_dictionary_owned_value_slot_untouched) {
    BEZEL_EXPECT_EQ(run(aot(
        "  using var od := new OwningDictionary<string, R>(); "
        "  od.Add(\"a\", new R()); "
        "  return od.Count; ")),
        1);
}

// #4318 (stdlib §4.17 v0.315): SortedDictionary<K,V> null-declaration builtin
// skips its own where K: IComparable<K> constraint. Before fix, no constraint
// check fired for null-declaration builtins. Fix adds kNullDeclSelfRefConstraints
// to TypeResolution_Names.cpp so the self-referential IComparable bound is checked.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorteddictionary_class_key_rejected_icomparable_constraint_s4318_001) {
    auto r = parseSemaAll(
        "module M; import System.Collections.Generic; "
        "class NC end NC; "
        "class P "
        "  public static method Main(): int "
        "    var sd := new SortedDictionary<NC, int>(); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(hasError(r, "does not satisfy the constraint `where K: IComparable"));
    BEZEL_EXPECT_TRUE(hasError(r, "(\xC2\xA7""14.2)"));
}

// Control: SortedDictionary<string,int> — string satisfies IComparable<string>.
BEZEL_TEST(sema_sorted_dictionary_add_nullable_3399,
           sorteddictionary_string_key_accepted_icomparable_constraint_s4318_002) {
    auto r = parseSemaAll(
        "module M; import System.Collections.Generic; "
        "class P "
        "  public static method Main(): int "
        "    var sd := new SortedDictionary<string, int>(); "
        "    sd.Add(\"a\", 1); "
        "    return sd.Count; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(hasError(r, "does not satisfy the constraint"));
}
