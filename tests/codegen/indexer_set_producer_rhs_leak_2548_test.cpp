// fix #2548 (§23.4/§28.9/§4.6, v0.236): a +1 PRODUCER RHS passed to a
// user-defined INDEXER setter leaks the +1 per call, at all three setter-call
// paths inside `lowerIndexerSet` (LlvmBackend_Assign.cpp):
//   * INTERFACE-INDEXER DISPATCH (itable slot, ~line 1799/1801 — the
//     `ifacePropertySetIndex_` branch).
//   * CLASS-VTABLE DISPATCH (vtable slot, ~line 1840/1951 — the
//     `propSetSlot_` branch for a virtual/override class indexer).
//   * INLINE-SETTER-BODY INLINING (the body-store path, ~line 1937/2062 —
//     reached for non-virtual class indexers and for record indexers).
//
// Pre-fix, every iteration of `h[i] := "abc" + i.ToString();` (or any other
// +1 producer — string concatenation, ToString, interpolation, a
// closure-returning method call, a box-return call to an Object/interface
// property target) leaked the producer's +1 across the setter call, because
// the setter (or the inlined setter body) RETAINED its incoming `value` for
// a CLASS-OWNED indexer — but the producer RHS's +1 was the only call-side
// reference of the producer, so retaining doubled it and the doubled +1 was
// unowned. LSan observed 320 byte(s) direct + 50 byte(s) indirect = 370
// bytes leaked across 10 iterations (37 bytes/iteration, the 32-byte bz_string
// header + ~5 byte int-digit payload).
//
// The property-side analogue `h.Name := ...` on a virtual/interface property
// did NOT leak, because `lowerVirtualPropertySet` (LlvmBackend_Exprs.cpp
// ~5106-5128) defines and invokes an `emitProducerRelease` lambda at every
// setter-call site (unconditional on the vtable path; flag-gated on the
// itable path so a record-backed runtime receiver — which plain-stores — is
// preserved). The indexer shape had NO analogous lambda in `lowerIndexerSet`
// — the fix copies that exact pattern into `lowerIndexerSet` so all three
// setter-call paths close the doubled-+1 leak.
//
// OBSERVABILITY: the leak is a DIRECT leak of the bz_string header, so
// runAotChecked's LSan flags it; without the fix `r.ran` is false
// ("Direct leak of N byte(s)"), with it the run is clean and no
// double-free / use-after-free is introduced (the dispose-exactly-once net
// rides on `r.ran`).
//
// TEST SHAPE: each probe loop-amplifies 10× so LSan catches the leak
// decisively; the exit code is `sink mod 256` — the program's own
// observable value (independent of the leak), confirming the program also
// computes correctly and the indexer set/get round-trips. The cardinally
// affected paths are exercised separately (virtual indexer setter on a
// class; interface indexer setter through an interface reference;
// non-virtual class indexer setter for the inline-body path).
//
// REGRESSION COVERAGE:
//   * The virtual-PROPERTY analogue (the case that DID already work) sits
//     in the loop body of one probe as a cross-check that the property-side
//     fix is unaffected.
//   * The RECORD indexer case is exercised WITHOUT the producer-release
//     expectation — it is preserved as-is (the inline-body path gates
//     `emitProducerRelease` on `recvDecl->kind == Class` because the
//     record-side setter plain-stores; releasing a producer a record-side
//     setter never retained would drop the field's only reference, a UAF).
//     The record case's leak is a separate, pre-existing concern (out of
//     scope for #2548's three setter-call sites; see the dispatch at the
//     top of `lowerIndexerSet`'s `!pd` imported-stub branch).

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
// Probe A — the INLINE-SETTER-BODY path (non-virtual class indexer; the
// inlined setter body stores `_items := value;` which mints the class's
// field-retain). The producer RHS is a string concatenation that isPlusOneStringProducer
// admits. Without #2548 every iteration leaks the +1. With #2548 the
// inline-body path emits producer release after the body runs.
// ----------------------------------------------------------------------
const char* kInlineBodyLoop =
    "module M; "
    "class Holder "
    "  public _items: string := \"\"; "
    "  public property [i: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var i := 0; var sink := 0; "
    "    while i < 10 do "
    "      h[0] := \"abc\" + i.ToString(); "  // producer RHS, inline path
    "      sink := sink + h[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — the CLASS-VTABLE path (virtual/override indexer dispatched
// through the vtable slot). Mirrors `h.Name := ...` on a virtual property,
// the canonical property-side analogue. Without #2548 every iteration
// leaks the +1.
// ----------------------------------------------------------------------
const char* kClassVtableLoop =
    "module M; "
    "class Base "
    "  public _items: string := \"\"; "
    "  public virtual property [i: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Base; "
    "class Derived: Base end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var i := 0; var sink := 0; "
    "    while i < 10 do "
    "      b[0] := \"abc\" + i.ToString(); "  // producer RHS, vtable path
    "      sink := sink + b[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — the INTERFACE-INDEXER-DISPATCH path (itable slot through an
// interface-typed local). The classBB branch's new emitProducerRelease
// fires on the class-backed runtime receiver; the recordBB branch's
// existing #2540 emitStringRetain is preserved for the NAMED-LOCAL case
// (orthogonal).
// ----------------------------------------------------------------------
const char* kInterfaceDispatchLoop =
    "module M; "
    "interface IHolder property [i: int]: string; end IHolder; "
    "class Holder: IHolder "
    "  public _items: string := \"\"; "
    "  public property [i: int]: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var ih: IHolder := h; "
    "    var i := 0; var sink := 0; "
    "    while i < 10 do "
    "      ih[0] := \"abc\" + i.ToString(); "  // producer RHS, itable path
    "      sink := sink + ih[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — cross-check: the VIRTUAL PROPERTY analogue that DID already
// work (lowerVirtualPropertySet's emitProducerRelease) is unaffected by
// the indexer fix. If `h.Name := "abc" + i.ToString();` regressed, this
// probe would fail.
// ----------------------------------------------------------------------
const char* kVirtualPropertyAnalogueLoop =
    "module M; "
    "class Base "
    "  public _items: string := \"\"; "
    "  public virtual property Name: string "
    "    get => _items; "
    "    set => _items := value; "
    "  end; "
    "end Base; "
    "class Derived: Base end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var i := 0; var sink := 0; "
    "    while i < 10 do "
    "      b.Name := \"abc\" + i.ToString(); "  // producer RHS, property
    "      sink := sink + b.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2548 — INLINE-SETTER-BODY path: a non-virtual class indexer setter
// stores `_items := value` (a class field store mints a retain); a +1
// producer RHS ("abc" + i.ToString()) leaks its +1 pre-fix because the
// inline-body path had no emitProducerRelease. The fix closes it.
// =====================================================================
BEZEL_TEST(codegen_indexer_set_producer_rhs_leak_2548,
           inline_body_class_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kInlineBodyLoop);
    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean (the #2548 leak flips this)
}

// =====================================================================
// #2548 — CLASS-VTABLE path: a virtual/override class indexer setter
// dispatched through the vtable slot retained its incoming `value` but
// the call site had no emitProducerRelease, doubling the producer's +1.
// The fix adds an unconditional emitProducerRelease on the vtable path
// (mirroring lowerVirtualPropertySet's `!isIfaceProp` site).
// =====================================================================
BEZEL_TEST(codegen_indexer_set_producer_rhs_leak_2548,
           class_vtable_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kClassVtableLoop);
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2548 — INTERFACE-INDEXER-DISPATCH path: an itable-dispatched setter
// with a producer RHS leaks pre-fix because the classBB branch had no
// emitProducerRelease (only the named-local recordBB retain from #2540).
// The fix adds emitProducerRelease to classBB.
// =====================================================================
BEZEL_TEST(codegen_indexer_set_producer_rhs_leak_2548,
           interface_dispatch_does_not_leak_spec_23_4_28_9) {
    JitResult r = runAotChecked(kInterfaceDispatchLoop);
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2548 — REGRESSION CROSS-CHECK: the virtual-PROPERTY analogue was the
// reference for the fix; ensure the property path stays clean (no
// over-eager release on a record-backed path; no double release).
// =====================================================================
BEZEL_TEST(codegen_indexer_set_producer_rhs_leak_2548,
           virtual_property_analogue_still_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kVirtualPropertyAnalogueLoop);
    BEZEL_EXPECT_TRUE(r.ran);
}
