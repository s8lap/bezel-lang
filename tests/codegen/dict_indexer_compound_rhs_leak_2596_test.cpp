// fix #2596 (§23.4/§28.9/§4.6, v0.236): the DICTIONARY-INDEXER compound-
// assignment leak — `d[k] += "abc" + i.ToString()` on a borrowing
// Dictionary<K,string> leaks the rhs's orphan +1 AND a spurious-retain'd
// newVal +1 per iteration. This is the dict twin of #2587 (Array<string>
// compound) / #2567 (user-indexer compound) / #2576 (property compound).
//
// `lowerCollectionIndexerCompound` (LlvmBackend_Assign.cpp ~2606) desugars
// `d[k] OP= rhs` to read-modify-write via lowerCollectionIndexerGet /
// emitCompoundOp / lowerCollectionIndexerSet. For a string value the compound
// desugar mints TWO +1 string producers:
//   * `rhs = lowerExpr(*as.value)` — a +1 string for the BinaryExpr concat
//     `"abc" + i.ToString()`.
//   * `newVal = emitCompoundOp(...)` — a SECOND +1 (emitStringConcat allocates
//     rc=1 per #422, memcpy'ing rhs's bytes into a fresh header; ALWAYS +1
//     regardless of whether rhs is itself a producer or a borrow).
// The write-back delegated to lowerCollectionIndexerSet with valueExpr=nullptr.
// The plain-set path's #429 borrowing-Dictionary<K,string> retain fires
// whenever valueExpr is null OR not a +1 producer, so with valueExpr=nullptr it
// fired UNCONDITIONALLY — bumping newVal from +1 to +2. The disposing setter
// (bz_dict_set_gen_disposing / bz_dict_set_str_disposing with
// stringElementReleaseThunk) then transferred only ONE +1 into the slot
// (releasing the displaced OLD value), orphaning the retain's +1 each iter.
// Plus rhs's own +1 leaked (emitStringConcat does NOT consume rhs). LSan on
// origin/main: 100 iterations of `d[0] += "abc" + i.ToString()` leaked 31485
// byte(s) across 400 allocations (two direct leak sites of 3200B each = 100 ×
// 32B bz_string headers: the spurious-retain'd newVal +1 and the rhs orphan
// +1; plus their byte buffers as indirect leaks).
//
// FIX: thread a `valIsFreshPlusOneString=true` flag from the dict compound path
// into lowerCollectionIndexerSet so the #429 retain is SKIPPED (newVal is
// already +1 from emitStringConcat) and the +1 transfers into the slot via the
// disposing setter (which also releases the displaced OLD value). Then release
// rhs when the AST is a +1 producer (isPlusOneStringProducer gate — a borrow
// rhs must NOT be released or it UAFs the local's only ref). Mirrors #2587's
// rhs-release + the plain-set path's conditional-retain exactly.
//
// OWNERSHIP GATE — why newVal is NOT released here (same as #2587): a
// COLLECTION indexer does NOT retain — it TRANSFERs the +1 into the slot (the
// disposing setter releases the OLD value and stores the new pointer, consuming
// exactly one +1). Releasing newVal would UAF the slot. Only rhs's orphan +1
// (when the AST is a producer) needs a caller-side release. This is the same
// ownership model as #2587's isStrElemArr branch and #429's dict-string fix.
//
// OBSERVABILITY: the leak is a DIRECT leak of bz_string headers + their byte
// buffers, so runAotChecked's LSan flags it. Without the fix `r.ran` is false
// ("Direct leak of N byte(s)"), with it the run is clean (r.ran=true) and the
// exit code independently confirms the program computed correctly.
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL — Dictionary<int,string> compound += producer-RHS string
//     concat. LSan-clean post-fix; exit-code value assertion (sink mod 256).
//   * (B) BORROW RHS — Dictionary<int,string> compound += named local. The
//     isPlusOneStringProducer gate must NOT match an IdentifierExpr (releasing
//     a borrow rhs would UAF the local's only ref). Guards against an over-
//     eager rhs release.
//   * (C) INT VALUE — Dictionary<int,int> compound += int. Unaffected: the
//     fix's isStrValDict gate excludes int values; the generic dict store path
//     still handles value types. Guards against the release misfiring on a
//     non-string value.
//   * (D) #2587 ARRAY control — Array<string> compound += producer. Must stay
//     LSan-clean (the #2587 fix remains in place; this fix must not regress it).
//   * (E) #2567 USER-INDEXER control — class indexer compound += producer.
//     Must stay LSan-clean (the #2567 fix remains in place).
//
// SPEC CITATION: §23.4 (indexers), §28.9 (runtime-managed types refcounting on
// producer transfer), §4.6 (string RC), stdlib §4.8 (Dictionary<K,V> indexer
// get/set). §4.8: a borrowing Dictionary<K,V> just overwrites; the indexer
// setter transfers the +1 (it does NOT retain, unlike a class property setter).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ----------------------------------------------------------------------
// Probe A — CARDINAL: Dictionary<int,string> compound += producer-RHS string
// concat. emitStringConcat mints newVal (+1, transferred to slot via the
// disposing setter) and leaves rhs as an orphan +1; the plain-set path's
// spurious retain bumped newVal to +2 (the #2596 leak). Pre-fix LSan flags
// 31485 byte(s) across 400 allocations over 100 iterations. Post-fix the
// retain is skipped and the rhs release fires. LSan-clean + exit value.
// ----------------------------------------------------------------------
const char* kDictCompoundProducer =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Dictionary<int, string>(); "
    "    d.Add(0, \"x\"); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      d[0] += \"abc\" + i.ToString(); "  // producer rhs, dict compound path
    "      sink := sink + d[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — BORROW RHS control: a named local string compound-concatenated
// into the same dict slot. emitStringConcat STILL mints a +1 newVal (always
// rc=1 per #422), so pre-fix the spurious retain leaked here too. The local's
// +1 belongs to its own slot, so a caller-side release would UAF the local's
// only ref at its scope end. The fix's producer-predicate gate
// (isPlusOneStringProducer must match a BinaryExpr/string-interp/call, NOT a
// named IdentifierExpr) protects this — post-fix this is LSan-clean with no
// UAF. If the predicate were widened to fire on borrows too, this would fail
// with UAF on the local's release.
// ----------------------------------------------------------------------
const char* kDictCompoundBorrow =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Dictionary<int, string>(); "
    "    d.Add(0, \"x\"); "
    "    var local: string := \"abc\"; "  // mint one +1 local (borrow rhs)
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      d[0] += local; "  // borrow rhs — local's +1 unchanged
    "      sink := sink + d[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — INT (non-string) VALUE control: an int value compound-assigned an
// int. The fix's isStrValDict gate excludes int values (RuntimeManaged is
// string/function only), so the generic dict store path handles this. The rhs
// release is pointer-gated + producer-predicate-gated, so it must NOT fire on
// an int. Pre- and post-fix this computes correctly (sum 0..99 = 4950,
// mod 256 = 86) and is LSan-clean.
// ----------------------------------------------------------------------
const char* kDictCompoundInt =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Dictionary<int, int>(); "
    "    d.Add(0, 0); "
    "    var i := 0; "
    "    while i < 100 do "
    "      d[0] += i; "  // int rhs, dict compound path — generic store
    "      i := i + 1; "
    "    end; "
    "    return d[0] mod 256; "  // 4950 mod 256 = 86
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — #2587 ARRAY control: an Array<string> compound += producer rhs.
// The #2587 fix (the isStrElemArr branch in lowerCollectionIndexerCompound)
// must remain LSan-clean — this fix (the dict path) must not regress it.
// ----------------------------------------------------------------------
const char* kArrayCompoundProducer =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var arr := new Array<string>(); "
    "    arr.Add(\"x\"); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      arr[0] += \"abc\" + i.ToString(); "  // #2587 array compound path
    "      sink := sink + arr[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe E — #2567 USER-INDEXER control: a class with a user-defined indexer,
// compound += producer rhs. The #2567 fix (lowerIndexerCompound) releases
// newVal + rhs after the setter body. This fix (lowerCollectionIndexerCompound)
// must not regress that path — they are separate functions. Must stay clean.
// ----------------------------------------------------------------------
const char* kUserIndexerCompound =
    "module M; "
    "class Grid "
    "  private _cell: string := \"\"; "
    "  public property this[r: int]: string "
    "    get => _cell; "
    "    set => _cell := value; "
    "  end; "
    "end Grid; "
    "class C "
    "  public static method Main(): int "
    "    using var g := new Grid(); "
    "    var i := 0; var sink := 0; "
    "    while i < 50 do "
    "      g[0] += \"ab\" + i.ToString(); "  // user-indexer compound, #2567 path
    "      sink := sink + g[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2596 — CARDINAL CASE: Dictionary<int,string> compound += producer-RHS
// string concat. Pre-fix leaks 31485 byte(s) across 400 allocations (two
// direct leak sites of 3200B: the spurious-retain'd newVal +1 and the rhs
// orphan +1; plus byte-buffer indirect leaks). Post-fix: the retain is
// skipped (valIsFreshPlusOneString) and the rhs release drops the orphan +1.
// LSan-clean + exit value 75.
// =====================================================================
BEZEL_TEST(dict_indexer_compound_rhs_leak_2596,
           dict_string_compound_producer_rhs_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kDictCompoundProducer);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);           // LSan-clean (the #2596 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 75);    // sink mod 256 — independent value check
}

// =====================================================================
// #2596 — BORROW RHS control: a named-local string rhs. emitStringConcat
// always mints +1 newVal, so pre-fix the spurious retain leaked here too.
// Post-fix the retain is skipped. The producer predicate
// (isPlusOneStringProducer) must NOT match a borrow, so the rhs release does
// not fire; if it did, the local's scope-end release would UAF its own +1.
// LSan-clean + no UAF.
// =====================================================================
BEZEL_TEST(dict_indexer_compound_rhs_leak_2596,
           dict_string_compound_borrow_no_uaf_spec_23_4_28_9) {
    JitResult r = runAotChecked(kDictCompoundBorrow);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2596 — INT (non-string) VALUE control: the isStrValDict gate excludes int
// values, so the generic dict store path handles this. The rhs release is
// pointer-gated, so it must NOT fire on an int. Computes 4950 mod 256 = 86,
// LSan-clean.
// =====================================================================
BEZEL_TEST(dict_indexer_compound_rhs_leak_2596,
           dict_int_compound_unaffected_spec_23_4_28_9) {
    JitResult r = runAotChecked(kDictCompoundInt);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 86);    // 4950 mod 256
}

// =====================================================================
// #2596 — #2587 ARRAY regression guard: an Array<string> compound += producer
// rhs. The #2587 fix (the isStrElemArr branch, a SEPARATE code path from the
// dict branch this fix touches) must remain LSan-clean — this fix must not
// regress it.
// =====================================================================
BEZEL_TEST(dict_indexer_compound_rhs_leak_2596,
           array_string_compound_remains_clean_2587_spec_23_4_28_9) {
    JitResult r = runAotChecked(kArrayCompoundProducer);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2596 — #2567 USER-INDEXER regression guard: a class user-defined indexer
// compound += producer rhs. The #2567 fix (lowerIndexerCompound, a SEPARATE
// function from lowerCollectionIndexerCompound) must remain LSan-clean — this
// fix must not regress it.
// =====================================================================
BEZEL_TEST(dict_indexer_compound_rhs_leak_2596,
           user_indexer_compound_remains_clean_2567_spec_23_4_28_9) {
    JitResult r = runAotChecked(kUserIndexerCompound);
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
}
