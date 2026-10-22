// fix #2587 (§23.3/§28.9/§4.6, v0.236): the COLLECTION-INDEXER compound-
// assignment leak — `arr[i] += "abc" + i.ToString()` on a borrowing
// Array<string> leaks the rhs's +1 and the displaced old slot value per call.
// This is the built-in-heap-collection twin of the #2567 (user-indexer) and
// #2576 (computed-property) compound leaks.
//
// `lowerCollectionIndexerCompound` (LlvmBackend_Assign.cpp ~2680) desugars
// `arr[i] OP= rhs` to read-modify-write via bz_array_get / emitCompoundOp /
// bz_array_set. For a string element the compound desugar mints TWO +1
// producers:
//   * `rhs = lowerExpr(*as.value)` — a +1 string for the BinaryExpr concat
//     `"abc" + i.ToString()`.
//   * `newVal = emitCompoundOp(...)` — a SECOND +1 (emitStringConcat allocates
//     rc=1 per #422, memcpy'ing rhs's bytes into a fresh header).
// The write-back derived its disposer from elementPolicyFor, which returns
// disposerThunk=nullptr for a BORROWING string element (RuntimeManaged row) —
// so it fell to `bz_array_set` (a plain memcpy, no refcount management).
// That leaked THREE ways: (1) rhs's orphan +1 (emitStringConcat does not
// consume rhs), (2) the displaced OLD slot value (bz_array_set has no
// disposer), and (3) newVal was byte-copied into the slot with no +1
// bookkeeping. LSan on origin/main: 100 iterations of `arr[0] += "abc" +
// i.ToString()` leaked 30961 byte(s) across 398 allocations (100 rhs headers
// + 99 displaced old-value headers + their byte buffers).
//
// FIX: the COMPOUND twin of the plain-set path's `isStrElemArr` branch
// (lowerCollectionIndexerSet line ~957-987). For a borrowing Array<string>
// (and Stack<string> / Queue<string>), emit `bz_array_set_disposing` with
// stringElementReleaseThunk() — this releases the displaced OLD slot value
// and transfers newVal's +1 into the slot. Then release rhs when the AST is
// a +1 producer (isPlusOneStringProducer gate — a borrow rhs must NOT be
// released or it UAFs the local's only ref).
//
// OWNERSHIP GATE — why newVal is NOT released (UNLIKE #2567): a CLASS
// property setter RETAINs its incoming value (§23.3 / §9), doubling a +1
// producer's refcount, so #2567 releases the OP-result's "extra" +1 after
// the setter body. A COLLECTION indexer does NOT retain — it TRANSFERs the
// +1 into the slot (the disposing setter releases the OLD value and memcpy's
// the new pointer in, consuming exactly one +1). Releasing newVal here would
// UAF the slot. Only rhs's orphan +1 needs a caller-side release. This is
// the same ownership model as the plain-set path's isStrElemArr branch and
// #429's dict-string fix.
//
// OBSERVABILITY: the leak is a DIRECT leak of bz_string headers + their byte
// buffers, so runAotChecked's LSan flags it. Without the fix `r.ran` is
// false ("Direct leak of N byte(s)"), with it the run is clean (r.ran=true)
// and the exit code independently confirms the program computed correctly.
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL — Array<string> compound += producer-RHS string concat.
//     LSan-clean post-fix; exit-code value assertion (24395 mod 256 = 75).
//   * (B) BORROW RHS — Array<string> compound += named local. Pre/post-fix
//     clean: the isPlusOneStringProducer gate must NOT match an IdentifierExpr
//     (releasing a borrow rhs would UAF the local's only ref). Guards against
//     an over-eager rhs release.
//   * (C) INT RHS — Array<int> compound += int. Unaffected: the fix's
//     isStrElemArr gate excludes int elements; the generic bz_array_set path
//     still handles value types. Guards against the release misfiring on a
//     non-string element.
//   * (D) #2567 USER-INDEXER control — class indexer compound += producer.
//     Must stay LSan-clean (the #2567 fix remains in place; this fix must not
//     regress it).
//   * (E) #2576 PROPERTY control — class computed-property compound += producer.
//     Must stay LSan-clean (the #2576 fix remains in place).
//
// SPEC CITATION: §6.6 (compound assignments), §28.9 (runtime-managed types
// refcounting on producer transfer), §4.6 (string RC), stdlib §4.4 (Array<T>
// indexer get/set). §23.3 is cited for contrast (class property setter
// retains, unlike the collection indexer's transfer).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ----------------------------------------------------------------------
// Probe A — CARDINAL: Array<string> compound += producer-RHS string concat.
// emitStringConcat mints newVal (+1, transferred to slot via the disposing
// setter) and leaves rhs as an orphan +1 (the #2587 leak). Pre-fix LSan
// flags 30961 byte(s) across 398 allocations over 100 iterations. Post-fix
// the rhs release fires and the disposing setter releases the displaced old
// value. Exit code = 24395 mod 256 = 75 (independent value assertion).
// ----------------------------------------------------------------------
const char* kArrayCompoundProducer =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var arr := new Array<string>(); "
    "    arr.Add(\"x\"); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      arr[0] += \"abc\" + i.ToString(); "  // producer rhs, compound path
    "      sink := sink + arr[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 24395 mod 256 = 75
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — BORROW RHS control: a named local string compound-concatenated
// into the same array slot. The local's +1 belongs to its own slot, so a
// caller-side release would UAF the local's only ref at its scope end. The
// fix's producer-predicate gate (isPlusOneStringProducer must match a
// BinaryExpr/string-interp/call, NOT a named IdentifierExpr) protects this —
// pre- and post-fix this is LSan-clean. If the predicate were widened to fire
// on borrows too, this would fail with UAF on the local's release.
// ----------------------------------------------------------------------
const char* kArrayCompoundBorrow =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var arr := new Array<string>(); "
    "    arr.Add(\"x\"); "
    "    var local: string := \"abc\"; "  // mint one +1 local (borrow rhs)
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      arr[0] += local; "  // borrow rhs — local's +1 unchanged
    "      sink := sink + arr[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — INT (non-string) control: an int element compound-assigned an
// int. The fix's isStrElemArr gate excludes int elements (RuntimeManaged is
// string/function only), so the generic bz_array_set path still handles this.
// The rhs release is pointer-gated + producer-predicate-gated, so it must NOT
// fire on an int. Pre- and post-fix this computes correctly (sum 0..99 =
// 4950, mod 256 = 86) and is LSan-clean.
// ----------------------------------------------------------------------
const char* kArrayCompoundInt =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var arr := new Array<int>(); "
    "    arr.Add(0); "
    "    var i := 0; "
    "    while i < 100 do "
    "      arr[0] += i; "  // int rhs, compound path — generic store
    "      i := i + 1; "
    "    end; "
    "    return arr[0] mod 256; "  // 4950 mod 256 = 186
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — #2567 USER-INDEXER control: a class with a user-defined indexer,
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

// ----------------------------------------------------------------------
// Probe E — #2576 PROPERTY control: a class computed property, compound +=
// producer rhs. The #2576 fix (lowerPropertyCompound) releases res + rhs
// after the setter body. This fix must not regress that path. Must stay clean.
// ----------------------------------------------------------------------
const char* kPropertyCompound =
    "module M; "
    "class Sink "
    "  private _name: string := \"\"; "
    "  public property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Sink; "
    "class C "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var sink := 0; "
    "    while i < 50 do "
    "      s.Name += \"ab\" + i.ToString(); "  // property compound, #2576 path
    "      sink := sink + s.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2587 — CARDINAL CASE: Array<string> compound += producer-RHS string
// concat. Pre-fix leaks 30961 byte(s) across 398 allocations (100 rhs
// headers + 99 displaced old-value headers + byte buffers). Post-fix: the
// disposing setter releases the displaced old value and transfers newVal's
// +1 into the slot, and the rhs release drops the orphan +1. LSan-clean +
// value 75.
// =====================================================================
BEZEL_TEST(codegen_collection_indexer_compound_rhs_leak_2587,
           array_string_compound_producer_rhs_does_not_leak_spec_23_3_28_9) {
    JitResult r = runAotChecked(kArrayCompoundProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);           // LSan-clean (the #2587 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 75);    // 24395 mod 256 — independent value check
}

// =====================================================================
// #2587 — BORROW RHS control: a named-local string rhs. The producer
// predicate (isPlusOneStringProducer) must NOT match a borrow, so the rhs
// release does not fire; if it did, the local's scope-end release would
// UAF its own +1. Pre-fix and post-fix both clean (the disposing setter
// fix applies regardless of rhs kind — emitStringConcat always mints +1).
// =====================================================================
BEZEL_TEST(codegen_collection_indexer_compound_rhs_leak_2587,
           array_string_compound_borrow_remains_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kArrayCompoundBorrow);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2587 — INT (non-string) control: the isStrElemArr gate excludes int
// elements, so the generic bz_array_set path handles this. The rhs release
// is pointer-gated, so it must NOT fire on an int. Computes 4950 mod 256 =
// 186, LSan-clean.
// =====================================================================
BEZEL_TEST(codegen_collection_indexer_compound_rhs_leak_2587,
           array_int_compound_unaffected_spec_23_3_28_9) {
    JitResult r = runAotChecked(kArrayCompoundInt);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 86);    // 4950 mod 256
}

// =====================================================================
// #2587 — #2567 USER-INDEXER regression guard: a class user-defined
// indexer compound += producer rhs. The #2567 fix (lowerIndexerCompound,
// a SEPARATE function from lowerCollectionIndexerCompound) must remain
// LSan-clean — this fix must not regress it.
// =====================================================================
BEZEL_TEST(codegen_collection_indexer_compound_rhs_leak_2587,
           user_indexer_compound_remains_clean_2567_spec_23_3_28_9) {
    JitResult r = runAotChecked(kUserIndexerCompound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2587 — #2576 PROPERTY regression guard: a class computed-property
// compound += producer rhs. The #2576 fix (lowerPropertyCompound) must
// remain LSan-clean — this fix must not regress it.
// =====================================================================
BEZEL_TEST(codegen_collection_indexer_compound_rhs_leak_2587,
           property_compound_remains_clean_2576_spec_23_3_28_9) {
    JitResult r = runAotChecked(kPropertyCompound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}
