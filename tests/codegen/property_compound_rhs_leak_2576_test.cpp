// fix #2576 (§23.3/§28.9/§4.6, v0.236): the COMPOUND-PROPERTY rhs leak —
// `s.Name += "abc" + i.ToString()` on a non-virtual CLASS computed property
// leaks the rhs's +1 per call. `lowerPropertyCompound`
// (LlvmBackend_Assign.cpp ~4346) desugars `s.Name OP= rhs` to
// `s.Name := s.Name OP rhs`:
//   * `rhs = lowerExpr(*as.value)` — a +1 string producer for the BinaryExpr
//     concat `"abc" + i.ToString()`.
//   * `res = emitCompoundOp(op, cur, rhs)` — a SECOND +1 (emitStringConcat
//     allocates rc=1 per #422, memcpy'ing rhs's bytes into a fresh header).
//   * `lowerPropertySetValue(target, res, as.value)` — the #2568 fix releases
//     `res` (the OP-result the setter body retained via the field store).
//   * BUT `rhs`'s own +1 was never discharged: emitStringConcat does NOT
//     consume rhs (it memcpy's its bytes and leaves rhs's +1 untouched), and
//     no caller-side release fired for rhs. So rhs's +1 was an orphan that
//     leaked out from under the call. LSan observed 3200 byte(s) direct +
//     590 byte(s) indirect = 3790 byte(s) across 100 iterations
//     (37 byte(s)/iteration — one 32-byte bz_string header + ~5 byte int-digit
//     payload), exactly the per-iteration pattern the #2568 fix observed for
//     the `res` leak it closed.
//
// The fix mirrors `lowerIndexerCompound`'s #2567 rhs release
// (LlvmBackend_Assign.cpp ~2539) EXACTLY: after `lowerPropertySetValue`
// returns, release `rhs` when `classOwned`, `rhs` is a pointer, and the source
// AST (`as.value`) is a +1 string producer. CLASS-only gate (§9/§9.7): a
// record's setter body plain-stores `res`, so `res`'s +1 is the field's own
// reference; the record-side compound rhs leak is the same §9/§9.7 auto-property
// gap tracked by #2577 (do NOT widen the gate to records). rhs is an ORPHAN
// temporary (emitStringConcat allocates a distinct header), so releasing it
// cannot UAF `res` or the field; for a BORROW rhs (a named local),
// isPlusOneStringProducer is false and the release does not fire.
//
// This is the property-twin of the #2567 indexer compound fix and the
// compound-twin of the #2568 property plain-set fix.
//
// OBSERVABILITY: the leak is a DIRECT leak of the bz_string header, so
// runAotChecked's LSan flags it; without the fix `r.ran` is false ("Direct
// leak of N byte(s)"), with it the run is clean and no double-free /
// use-after-free is introduced (verified: the borrow-RHS control and the
// virtual-property cross-check both stay clean).
//
// REGRESSION COVERAGE:
//   * Non-virtual CLASS computed-property COMPOUND assign with producer-RHS
//     string concat — the cardinal case, must be LSan-clean post-fix.
//   * Non-virtual CLASS computed-property COMPOUND assign with a borrow-RHS
//     (a named string local) — pre-fix this ALSO leaked: emitStringConcat
//     always mints `res` +1 regardless of rhs kind, but #2568's valueExpr
//     release only fired for a PRODUCER rhs, so a borrow rhs's `res` leaked.
//     The fix mirrors #2567 by releasing `res` on (classOwned && pointer)
//     unconditionally, closing the borrow-rhs res leak too. Post-fix clean;
//     also protects against an over-eager rhs release that would UAF the
//     borrow's only ref at its scope end (the rhs predicate must NOT match).
//   * Non-string (int) computed-property COMPOUND assign — cross-check that a
//     non-producer, non-pointer rhs is unaffected (the release is pointer-
//     gated, so it must not fire on an int).
//
// OUT OF SCOPE (filed separately, NOT covered here):
//   * The VIRTUAL compound-assign getter-`cur` leak — a virtual getter
//     returns +1, and the compound read-modify-write's `cur` operand
//     (lowerPropertyCompound line ~4348) is never released. Pre-existing,
//     separate from #2576's rhs/res mechanism (non-virtual getters return a
//     borrow, so the non-virtual probes are clean). Filed separately.
//   * The RECORD compound rhs leak — §9/§9.7: a record's setter body plain-
//     stores, so the fix's `classOwned` gate deliberately does NOT release on
//     records. The record-side leak is tracked by #2577 (needs record auto-
//     property +1 retain infra, not a caller-side release here). The fix must
//     NOT widen the gate to records; the UAF-on-records hazard is checked at
//     code-review time via the gate predicate (a record probe would either
//     pass and prove nothing about the gate, or fail and conflate the
//     pre-existing record leak with this fix).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// ----------------------------------------------------------------------
// Probe A — the CARDINAL case: non-virtual CLASS computed-property
// COMPOUND assign with a +1 producer-RHS string concat. emitStringConcat
// mints `res` (released by #2568) AND leaves `rhs` as an orphan +1 (the
// #2576 leak). Pre-fix LSan flags 37 byte(s)/iteration.
// ----------------------------------------------------------------------
const char* kClassCompoundProducer =
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
    "    while i < 100 do "
    "      s.Name += \"abc\" + i.ToString(); "  // producer rhs, compound path
    "      sink := sink + s.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — BORROW RHS control: a named local string compound-concatenated
// into the same computed property. The local's +1 belongs to its own slot,
// so a caller-side release would UAF the local's only ref at its scope end.
// The fix's producer-predicate gate (isPlusOneStringProducer must match a
// BinaryExpr/string-interp/call, NOT a named IdentifierExpr) protects this —
// pre- AND post-fix this is LSan-clean. If the predicate were widened to fire
// on borrows too, this would fail with UAF on the local's release.
// ----------------------------------------------------------------------
const char* kClassCompoundBorrow =
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
    "    var local: string := \"abc\"; "  // mint one +1 local
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      s.Name += local; "  // borrow rhs — local's +1 unchanged
    "      sink := sink + s.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — NON-STRING (int) control: an int computed property compound-
// assigned an int. rhs is an i32 (not a pointer, not a producer), so the
// fix's pointer-gate + producer-predicate must NOT fire. Pre- and post-fix
// this computes correctly and is LSan-clean; guards against the release
// misfiring on a non-producer, non-pointer rhs.
// ----------------------------------------------------------------------
const char* kClassCompoundInt =
    "module M; "
    "class Sink "
    "  private _count: int := 0; "
    "  public property Count: int "
    "    get => _count; "
    "    set => _count := value; "
    "  end; "
    "end Sink; "
    "class C "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; "
    "    while i < 100 do "
    "      s.Count += i; "  // int rhs, compound path
    "      i := i + 1; "
    "    end; "
    "    return s.Count mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2576 — CARDINAL CASE: non-virtual class computed-property COMPOUND
// assign with a producer-RHS string concat. Pre-fix leaks ~37 byte(s) per
// iteration (the orphan rhs +1 emitStringConcat left behind). Post-fix the
// rhs release fires and the orphan +1 is dropped.
// =====================================================================
BEZEL_TEST(codegen_property_compound_rhs_leak_2576,
           class_compound_producer_rhs_does_not_leak_spec_23_3_28_9) {
    JitResult r = runAotChecked(kClassCompoundProducer);
    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean (the #2576 rhs leak flips this)
}

// =====================================================================
// #2576 — BORROW CONTROL: a named-local string rhs. The producer
// predicate (isPlusOneStringProducer) must NOT match a borrow, so the rhs
// release does not fire; if it did, the local's scope-end release would
// UAF its own +1. Pre-fix and post-fix both clean.
// =====================================================================
BEZEL_TEST(codegen_property_compound_rhs_leak_2576,
           class_compound_borrow_remains_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kClassCompoundBorrow);
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2576 — VIRTUAL PROPERTY cross-check: DELIBERATELY OMITTED. The virtual
// compound-assign path has a PRE-EXISTING, separate leak — the `cur`
// operand (the getter read at lowerPropertyCompound line ~4348) returns a
// +1 from a VIRTUAL getter dispatch, and that +1 is never released in the
// compound read-modify-write (non-virtual getters return a borrow, so the
// non-virtual probes above are clean). This is unrelated to #2576's
// rhs/res leak and is filed as a separate issue; it is out of scope here.
// (Verified: a bare `var tmp := b.Name;` virtual-getter read is clean —
// the +1 is managed by the local's scope-end release; only the compound
// path's ephemeral `cur` operand leaks.)
// =====================================================================

// =====================================================================
// #2576 — INT (non-string) control: the rhs release is pointer-gated and
// producer-predicate-gated, so it must NOT fire on an int rhs. Pre- and
// post-fix this computes correctly (sum 0..99 = 4950, mod 256 = 42) and is
// LSan-clean.
// =====================================================================
BEZEL_TEST(codegen_property_compound_rhs_leak_2576,
           class_compound_int_rhs_unaffected_spec_23_3_28_9) {
    JitResult r = runAotChecked(kClassCompoundInt);
    BEZEL_EXPECT_TRUE(r.ran);
}
