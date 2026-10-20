// fix #2598 (§23.4/§28.9/§4.1/§4.6, v0.236): the IList<string> indexer-set AND
// compound-assignment leak — `lst[i] := "abc" + i.ToString()` and
// `lst[i] += "abc" + i.ToString()` on a borrowing IList<string> leak the
// displaced old slot value AND the producer's orphan +1 per call.
//
// This is the IList twin of #2587 (Array/Stack/Queue<string> compound) and
// shares its root cause: `lowerCollectionIndexerSet` (LlvmBackend_Assign.cpp
// ~957-987) and `lowerCollectionIndexerCompound` (ibid ~2890-2930) gate the
// disposing-setter + rhs-release path on the `isStrElemArr` predicate, which
// matches collection families by HEAD NAME (`Array`/`Stack`/`Queue`). It does
// NOT match `IList<T>` — so an `IList<string>` indexer falls through to the
// generic write-back (`elementPolicyFor` → `disposerThunk=nullptr` for a
// borrowing string element → `bz_array_set` plain memcpy, no refcount
// management). That leaks the displaced old value and orphans the producer.
//
// Per issue #401 (stdlib §4.1 / §4.4 / spec §23.4), an IList<T> receiver is
// ARRAY-BACKED — it carries the same bz_array_t* as Array<T> and lowers through
// the identical bz_array_set ABI (lowerCollectionIndexerSet line ~896-905:
// `isArr = isCollectionTypeName(recvTy) || isListInterfaceName(recvTy)`).
// IReadOnlyList<T> declares no setter (never reaches this path); IOwningList<T>
// is owning and handled by the separate elemDisp path (which runs the
// owning-family disposer against a class element — never a string). So the
// `bz_array_set_disposing` + `stringElementReleaseThunk()` path applies to
// IList<string> in BOTH the plain-set and compound paths, exactly as it does
// for Array<string>.
//
// FIX: widen the `isStrElemArr` head-name allowlist in BOTH spots to also
// admit `IList`. No other change — the same disposing-setter transfer model
// (release displaced old, transfer newVal's +1 into the slot) and rhs-release
// gate (only fire on a +1 producer; a borrow rhs's +1 is its own) apply.
//
// OWNERSHIP GATE — same as #2587: a collection indexer does NOT retain — it
// TRANSFERs the +1 into the slot (the disposing setter releases the OLD value
// and memcpy's the new pointer in, consuming exactly one +1). So newVal's +1
// is the slot's own reference; releasing it would UAF the slot. Only rhs's
// orphan +1 (compound path, when the AST is a producer) needs a caller-side
// release — a borrow rhs's +1 belongs to its own slot and must NOT be released.
//
// OBSERVABILITY: the leak is a DIRECT leak of bz_string headers + their byte
// buffers, so runAotChecked's LSan flags it. Pre-fix `r.ran` is false ("Direct
// leak of N byte(s)"), post-fix the run is clean and the exit code independently
// confirms the program computed correctly.
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL plain-set — IList<string> := producer-RHS string concat.
//     Pre-fix LSan: 3752 byte(s) / 198 allocations (99 displaced headers +
//     99 byte buffers, plus the 99 producer +1's). Post-fix: LSan-clean,
//     exit 234 (sink 490 mod 256).
//   * (B) CARDINAL compound — IList<string> += producer-RHS. Pre-fix LSan:
//     30961 byte(s) / 398 allocations (same shape as #2587's Array case).
//     Post-fix: LSan-clean, exit 75 (sink 24395 mod 256 — same as #2587).
//   * (C) BORROW RHS — IList<string> += named local. The isPlusOneStringProducer
//     gate must NOT match an IdentifierExpr; releasing a borrow rhs would UAF
//     the local's only ref. Pre/post-fix clean.
//   * (D) INT control — IList<int> := int / += int. The widened isStrElemArr
//     predicate must NOT admit IList<int> (the string-primitive gate excludes
//     int); the generic bz_array_set path handles value elements. Guards
//     against an over-release misfiring on a non-string element (would crash
//     calling stringElementReleaseThunk on int data).
//   * (E) #2587 Array<string> compound regression guard — the predicate
//     widening must not regress the existing Array<string> path.
//   * (F) #2567 USER-INDEXER regression guard — separate code path
//     (lowerIndexerCompound, class receiver); must stay clean.
//
// SPEC CITATION: §23.4 (indexers — IList<T> declares `property [i: int]: T`),
// §28.9 (runtime-managed string refcounting on producer transfer), §4.1/§4.4
// (IList<T> is array-backed, same bz_array_t* as Array<T> per issue #401),
// §4.6 (string RC). §28.11 (collection element ownership — borrowing family).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ----------------------------------------------------------------------
// Probe A — CARDINAL plain-set: IList<string> := producer-RHS string concat.
// Each iteration overwrites slot 0 with a fresh +1 producer ("abc" +
// i.ToString()); the displaced old slot value must be released by the
// disposing setter, and the producer's +1 must transfer into the slot.
// Pre-fix LSan: 3752 byte(s) across 198 allocations. Post-fix: LSan-clean,
// exit 234 (sink = sum of (3 + digits(i)) for i=0..99 = 490, mod 256 = 234).
// ----------------------------------------------------------------------
const char* kIListSetProducer =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: IList<string> := new Array<string>(); "
    "    arr.Add(\"x\"); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      arr[0] := \"abc\" + i.ToString(); "
    "      sink := sink + arr[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 490 mod 256 = 234
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — CARDINAL compound: IList<string> += producer-RHS string concat.
// emitStringConcat mints newVal (+1, transferred to slot via the disposing
// setter) and leaves rhs as an orphan +1 (the #2587 leak, now also fixed for
// IList). Pre-fix LSan: 30961 byte(s) across 398 allocations (identical shape
// to #2587's Array<string> case). Post-fix: LSan-clean, exit 75 (sink 24395
// mod 256 — same value as #2587's Array case since the element-policy math is
// identical).
// ----------------------------------------------------------------------
const char* kIListCompoundProducer =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: IList<string> := new Array<string>(); "
    "    arr.Add(\"x\"); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      arr[0] += \"abc\" + i.ToString(); "
    "      sink := sink + arr[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 24395 mod 256 = 75
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — BORROW RHS control: a named local string rhs. The producer
// predicate (isPlusOneStringProducer) must NOT match a borrow (an
// IdentifierExpr), so the rhs release does not fire; if it did, the local's
// scope-end release would UAF its own +1. The SLOT side still leaks pre-fix
// (emitStringConcat mints a +1 newVal regardless of rhs kind, and bz_array_set
// has no disposer), so this case also goes red pre-fix; post-fix it is clean,
// proving both that the slot leak is closed AND that the borrow rhs is not
// released (no UAF).
// ----------------------------------------------------------------------
const char* kIListCompoundBorrow =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: IList<string> := new Array<string>(); "
    "    arr.Add(\"x\"); "
    "    var local: string := \"abc\"; "
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
// Probe D — INT (non-string) control: IList<int> := int and += int. The
// widened isStrElemArr predicate must NOT admit IList<int> (the
// string-primitive gate excludes int); the generic bz_array_set path handles
// value elements. Pre/post-fix computes correctly (99+1=100) and LSan-clean.
// Guards against the widened predicate misfiring on a non-string element.
// ----------------------------------------------------------------------
const char* kIListIntSetCompound =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: IList<int> := new Array<int>(); "
    "    arr.Add(0); "
    "    var i := 0; "
    "    while i < 100 do "
    "      arr[0] := i; "
    "      arr[0] += 1; "
    "      i := i + 1; "
    "    end; "
    "    return arr[0] mod 256; "  // 100
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe E — #2587 Array<string> compound regression guard. The predicate
// widening must not regress the existing Array<string> path. Must stay clean.
// ----------------------------------------------------------------------
const char* kArrayCompoundProducer =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var arr := new Array<string>(); "
    "    arr.Add(\"x\"); "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      arr[0] += \"abc\" + i.ToString(); "
    "      sink := sink + arr[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 24395 mod 256 = 75
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe F — #2567 USER-INDEXER regression guard. A class with a user-defined
// indexer, compound += producer rhs. The #2567 fix (lowerIndexerCompound, a
// SEPARATE function) must remain LSan-clean — this fix must not regress it.
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
    "      g[0] += \"ab\" + i.ToString(); "
    "      sink := sink + g[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2598 — CARDINAL PLAIN-SET: IList<string> := producer. Pre-fix LSan flags
// 3752 byte(s) / 198 allocations. Post-fix: the disposing setter releases
// the displaced old value and transfers the producer's +1 into the slot.
// LSan-clean + value 234.
// =====================================================================
BEZEL_TEST(codegen_ilist_string_indexer_set_leak_2598,
           ilist_string_plain_set_producer_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kIListSetProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);           // LSan-clean (the #2598 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 234);   // 490 mod 256 — independent value check
}

// =====================================================================
// #2598 — CARDINAL COMPOUND: IList<string> += producer. Pre-fix LSan flags
// 30961 byte(s) / 398 allocations (same shape as #2587's Array case). Post-
// fix: the disposing setter releases the displaced old value and transfers
// newVal's +1 into the slot, and the rhs release drops the orphan +1.
// LSan-clean + value 75.
// =====================================================================
BEZEL_TEST(codegen_ilist_string_indexer_set_leak_2598,
           ilist_string_compound_producer_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kIListCompoundProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);           // LSan-clean (the #2598 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 75);    // 24395 mod 256 — independent value check
}

// =====================================================================
// #2598 — BORROW RHS control: a named-local string rhs. The producer
// predicate must NOT match a borrow, so the rhs release does not fire;
// the slot-side leak is still closed by the disposing setter. Pre-fix
// red (slot leak), post-fix clean.
// =====================================================================
BEZEL_TEST(codegen_ilist_string_indexer_set_leak_2598,
           ilist_string_compound_borrow_remains_clean_spec_23_4_28_9) {
    JitResult r = runAotChecked(kIListCompoundBorrow);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2598 — INT (non-string) control: the widened isStrElemArr gate excludes
// int elements, so the generic bz_array_set path handles this. The rhs
// release is pointer-gated, so it must NOT fire on an int. Computes 100,
// LSan-clean.
// =====================================================================
BEZEL_TEST(codegen_ilist_string_indexer_set_leak_2598,
           ilist_int_set_compound_unaffected_spec_23_4_28_9) {
    JitResult r = runAotChecked(kIListIntSetCompound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);   // 99 + 1
}

// =====================================================================
// #2598 — #2587 Array<string> compound regression guard: the predicate
// widening must not regress the existing Array<string> path. Must stay clean.
// =====================================================================
BEZEL_TEST(codegen_ilist_string_indexer_set_leak_2598,
           array_string_compound_remains_clean_2587_spec_23_4_28_9) {
    JitResult r = runAotChecked(kArrayCompoundProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 75);    // 24395 mod 256
}

// =====================================================================
// #2598 — #2567 USER-INDEXER regression guard: a class user-defined indexer
// compound += producer rhs. The #2567 fix (lowerIndexerCompound, a SEPARATE
// function from lowerCollectionIndexerCompound) must remain LSan-clean —
// this fix must not regress it.
// =====================================================================
BEZEL_TEST(codegen_ilist_string_indexer_set_leak_2598,
           user_indexer_compound_remains_clean_2567_spec_23_4_28_9) {
    JitResult r = runAotChecked(kUserIndexerCompound);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}
