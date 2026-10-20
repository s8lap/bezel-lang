// fix #2586 (§23.3/§28.9/§4.6, v0.236): the VIRTUAL/INTERFACE compound-assign
// twin of #2576's non-virtual fix. The VIRTUAL compound-assign path lives in
// LlvmBackend_Stmts.cpp (the `else` branch of the AssignmentStmt lowering —
// `obj.P OP= v` where P is a virtual/interface property), NOT in
// `lowerPropertyCompound` (which handles the non-virtual path and was fixed by
// #2576). The virtual path desugars `b.Name OP= rhs` to a dynamic read-modify-
// write:
//   * `cur = lowerExpr(*as->target)` — the dynamic getter dispatches through
//     the vtable/itable (lowerVirtualPropertyGet). The getter accessor body
//     is `return lowerExpr(*getterExpr);` where getterExpr is the field
//     reference `_name` — a plain LOAD, no retain. So `cur` is a BORROW of
//     the field, NOT a +1 producer. (The #2576 test comment speculated that
//     "the virtual getter returns +1"; this is INCORRECT — the getter body is
//     a field load, and `cur`'s +1 is never minted. The actual leak is `res`
//     and `rhs`, exactly the same shape as #2576's non-virtual fix.)
//   * `rhs = lowerExpr(*as->value)` — a +1 string when the AST is a producer
//     (BinaryExpr `+`, InterpolatedStringExpr, etc.); a borrow when the AST
//     is a named local; an immortal sentinel (no +1) when the AST is a bare
//     string literal.
//   * `res = emitCompoundOp(op, cur, rhs, ...)` — for a string target this
//     dispatches to emitStringConcat, which ALWAYS mints a fresh rc=1 string
//     (per #422), memcpy'ing both operands' bytes into a distinct header.
//   * `lowerVirtualPropertySet(*as->target, res)` — dispatches the setter
//     dynamically. The setter accessor body (for a CLASS property) stores
//     `res` into the backing field via lowerStringFieldStore's ARC:
//     retain(res) (rc 1→2), store, release(old). The retain doubles `res`'s
//     +1 — exactly the #2576 shape.
//
// THE LEAK: `lowerVirtualPropertySet` was called with NO valueExpr (res is a
// computed value, not a source AST node), so its internal `emitProducerRelease`
// — which would have released `res` to balance the setter's retain — DID NOT
// FIRE. So `res`'s +1 was orphaned every iteration. Additionally, a producer
// `rhs`'s +1 was orphaned (emitStringConcat memcpy's rhs's bytes and leaves
// rhs's own +1 untouched — the same #2576 rhs leak).
//
// LSan-confirmed on origin/main: 100 iterations of `b.Name += "x"` (literal
// rhs — rhs is an immortal sentinel, only `res` leaks) leak 3168 byte(s)
// direct + 5050 byte(s) indirect = 8218 byte(s) in 198 allocation(s) (99
// bz_string headers + 99 accumulated-string payloads). The producer-rhs
// shape `b.Name += "abc" + i.ToString()` leaks 31385 byte(s) in 400
// allocation(s) (the 200 extra allocations are the rhs producer headers +
// payloads).
//
// THE FIX mirrors #2576's structure:
//   * `res`'s +1 — released INSIDE `lowerVirtualPropertySet` via a new
//     `valueIsPlusOne` parameter (the compound caller passes true). The
//     function's EXISTING owner-kind flag branching (class-backed vs
//     record-backed, #2506) gates the actual emission: a CLASS vtable or
//     class-backed iface setter retains `res`, so the release fires; a
//     boxed RECORD's plain-store setter does NOT retain, so the release is
//     skipped (releasing would UAF the record field's only ref — §9/§9.7).
//   * `rhs`'s +1 — released at the CALL SITE in LlvmBackend_Stmts.cpp, gated
//     on `isPlusOneStringProducer(as->value)`. Safe for ALL backings: rhs
//     is a DISTINCT allocation from res (emitStringConcat memcpy's into a
//     fresh header), and rhs's +1 is never stored anywhere. A borrow rhs
//     (named local) is not +1 — the producer predicate gates it.
//
// REGRESSION COVERAGE:
//   * Virtual CLASS compound with LITERAL rhs (`b.Name += "x"`) — the
//     cardinal case: only `res` leaks pre-fix (literal rhs is an immortal
//     sentinel, no +1). Post-fix: LSan-clean.
//   * Virtual CLASS compound with PRODUCER rhs (`b.Name += "abc" + ...]`) —
//     both `res` AND `rhs` leak pre-fix. Post-fix: LSan-clean.
//   * Virtual CLASS compound with BORROW rhs (`b.Name += local`) — control:
//     res is released (valueIsPlusOne=true); rhs (a borrow) is NOT released
//     (producer predicate fails). Post-fix clean; proves no UAF on the
//     borrow's only ref.
//   * INTERFACE (class-backed) compound (`iref.Name += "x"`) — the itable-
//     dispatch twin; same leak mechanism. Post-fix: LSan-clean.
//   * NON-VIRTUAL control (`s.Name += "x"` on a non-virtual class property) —
//     handled by #2576's `lowerPropertyCompound` path, NOT this fix. Must
//     stay clean pre- and post-fix; proves the fix doesn't over-release the
//     non-virtual (borrow-getter) path.
//   * INT (non-string) virtual compound (`b.Count += i`) — pointer-gate
//     skips the release. Post-fix: computes correctly (sum 0..99 mod 256),
//     LSan-clean.
//   * VALUE assertion — the compound assign produces the correct accumulated
//     string length, not just "doesn't crash/leak".
//
// OUT OF SCOPE (pre-existing, NOT covered):
//   * The boxed-RECORD interface compound leak — a record's setter plain-
//     stores (no retain, no release of old field value), so each iteration
//     leaks the displaced old field value. This is the §9/§9.7 record auto-
//     property gap tracked by #2577 (needs record-side retain/release infra,
//     not a caller-side release here). My fix does NOT touch this path: the
//     record-backed iface branch in lowerVirtualPropertySet skips the res
//     release (no UAF), and the rhs release at the call site is safe (rhs
//     is orphaned regardless of backing). Verified: the boxed-record leak
//     count is identical pre- and post-fix.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ----------------------------------------------------------------------
// Probe A — CARDINAL: virtual CLASS compound assign with a LITERAL rhs.
// emitStringConcat mints `res` (+1); the virtual setter retains it (rc 1→2);
// pre-fix the doubled +1 is never released → 99 headers + 99 payloads leak
// over 100 iters. Post-fix: valueIsPlusOne=true releases `res` on the
// class-backed path.
// ----------------------------------------------------------------------
const char* kVirtualCompoundLiteral =
    "module M; "
    "class Base "
    "  private _name: string := \"\"; "
    "  public virtual property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Base; "
    "class Derived: Base "
    "  public override property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      b.Name += \"x\"; "  // literal rhs — only res leaks pre-fix
    "      sink := sink + b.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — PRODUCER RHS: virtual CLASS compound assign with a BinaryExpr
// concat rhs. BOTH `res` (emitStringConcat +1) AND `rhs` (producer orphan
// +1) leak pre-fix. Post-fix: both released.
// ----------------------------------------------------------------------
const char* kVirtualCompoundProducer =
    "module M; "
    "class Base "
    "  private _name: string := \"\"; "
    "  public virtual property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Base; "
    "class Derived: Base "
    "  public override property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      b.Name += \"abc\" + i.ToString(); "  // producer rhs — res + rhs leak
    "      sink := sink + b.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — BORROW RHS control: a named-local string rhs. The producer
// predicate must NOT match (an IdentifierExpr is a borrow, not a producer),
// so the rhs release does not fire; if it did, the local's scope-end
// release would UAF its own +1. `res` is still released (valueIsPlusOne=true
// — emitStringConcat always mints +1 regardless of rhs kind). Pre- and
// post-fix clean.
// ----------------------------------------------------------------------
const char* kVirtualCompoundBorrow =
    "module M; "
    "class Base "
    "  private _name: string := \"\"; "
    "  public virtual property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Base; "
    "class Derived: Base "
    "  public override property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var local: string := \"y\"; "  // mint one +1 local (borrow rhs)
    "    var i := 0; var sink := 0; "
    "    while i < 50 do "
    "      b.Name += local; "  // borrow rhs — local's +1 unchanged
    "      sink := sink + b.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — INTERFACE (class-backed) compound: the itable-dispatch twin of
// Probe A. `iref.Name += "x"` dispatches through the itable to the class's
// retaining setter — same leak mechanism. Post-fix: the owner-kind flag
// (class-backed) gates the res release correctly.
// ----------------------------------------------------------------------
const char* kIfaceCompoundLiteral =
    "module M; "
    "interface IHasName "
    "  property Name: string "
    "    get; "
    "    set; "
    "  end; "
    "end IHasName; "
    "class Impl: IHasName "
    "  private _name: string := \"\"; "
    "  public property Name: string "
    "    get => _name; "
    "    set => _name := value; "
    "  end; "
    "end Impl; "
    "class C "
    "  public static method Main(): int "
    "    using var im := new Impl(); "
    "    var b: IHasName := im; "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      b.Name += \"x\"; "
    "      sink := sink + b.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe E — NON-VIRTUAL control: a non-virtual class computed property.
// Handled by #2576's `lowerPropertyCompound` path, NOT this fix. Must stay
// clean pre- and post-fix; proves the fix doesn't over-release the
// non-virtual borrow-getter path.
// ----------------------------------------------------------------------
const char* kNonVirtualControl =
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
    "      s.Name += \"x\"; "
    "      sink := sink + s.Name.Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe F — INT (non-string) control: a virtual int computed property
// compound-assigned an int. rhs is an i32 (not a pointer, not a producer),
// so the fix's pointer-gate + producer-predicate must NOT fire. Pre- and
// post-fix this computes correctly (sum 0..99 = 4950, mod 256 = 86) and is
// LSan-clean. Guards against the release misfiring on a non-pointer rhs.
// ----------------------------------------------------------------------
const char* kVirtualCompoundInt =
    "module M; "
    "class Base "
    "  private _count: int := 0; "
    "  public virtual property Count: int "
    "    get => _count; "
    "    set => _count := value; "
    "  end; "
    "end Base; "
    "class Derived: Base "
    "  public override property Count: int "
    "    get => _count; "
    "    set => _count := value; "
    "  end; "
    "end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var i := 0; "
    "    while i < 100 do "
    "      b.Count += i; "
    "      i := i + 1; "
    "    end; "
    "    return b.Count mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2586 — CARDINAL CASE: virtual class compound assign with a LITERAL rhs.
// Pre-fix: LSan reports 8218 byte(s) leaked in 198 allocation(s) (99
// bz_string headers + 99 accumulated-string payloads). Post-fix: clean.
// =====================================================================
BEZEL_TEST(codegen_property_compound_virtual_leak_2586,
           virtual_compound_literal_rhs_does_not_leak_spec_23_3_28_9) {
    JitResult r = runAotChecked(kVirtualCompoundLiteral);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);  // LSan-clean (the #2586 res leak flips this)
}

// =====================================================================
// #2586 — PRODUCER RHS: virtual class compound assign with a BinaryExpr
// concat rhs. Both res and rhs leak pre-fix (31385 byte(s) in 400
// allocation(s)). Post-fix: clean.
// =====================================================================
BEZEL_TEST(codegen_property_compound_virtual_leak_2586,
           virtual_compound_producer_rhs_does_not_leak_spec_23_3_28_9) {
    JitResult r = runAotChecked(kVirtualCompoundProducer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2586 — BORROW RHS control: a named-local string rhs. The producer
// predicate (isPlusOneStringProducer) must NOT match a borrow, so the rhs
// release does not fire; res IS released (valueIsPlusOne=true). Pre- and
// post-fix both clean; proves no UAF on the borrow's only ref.
// =====================================================================
BEZEL_TEST(codegen_property_compound_virtual_leak_2586,
           virtual_compound_borrow_remains_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kVirtualCompoundBorrow);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2586 — INTERFACE (class-backed) compound: the itable-dispatch twin.
// Pre-fix: same 8218-byte leak as the vtable case. Post-fix: the owner-kind
// flag (class-backed) gates the res release correctly.
// =====================================================================
BEZEL_TEST(codegen_property_compound_virtual_leak_2586,
           iface_compound_literal_rhs_does_not_leak_spec_23_3_28_9) {
    JitResult r = runAotChecked(kIfaceCompoundLiteral);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2586 — NON-VIRTUAL control: handled by #2576's lowerPropertyCompound,
// NOT this fix. Must stay clean pre- and post-fix; proves the fix doesn't
// over-release the non-virtual borrow-getter path.
// =====================================================================
BEZEL_TEST(codegen_property_compound_virtual_leak_2586,
           nonvirtual_compound_remains_clean_spec_23_3_28_9) {
    JitResult r = runAotChecked(kNonVirtualControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// #2586 — INT (non-string) control: the res release is pointer-gated and
// the rhs release is producer-predicate-gated, so neither fires on an int.
// Pre- and post-fix this computes correctly (sum 0..99 = 4950, mod 256 = 86)
// and is LSan-clean.
// =====================================================================
BEZEL_TEST(codegen_property_compound_virtual_leak_2586,
           virtual_compound_int_rhs_unaffected_spec_23_3_28_9) {
    JitResult r = runAotChecked(kVirtualCompoundInt);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // VALUE assertion: sum 0..99 = 4950, mod 256 = 86.
    BEZEL_EXPECT_EQ(r.exitCode, 86);
}

// =====================================================================
// #2586 — VALUE assertion for the cardinal case: the compound assign
// produces the correct accumulated string length, not just "no leak".
// 100 iterations of `+= "x"` accumulate Name to length 100; sink sums
// 1+2+...+100 = 5050; 5050 mod 256 = 186.
// =====================================================================
BEZEL_TEST(codegen_property_compound_virtual_leak_2586,
           virtual_compound_value_correct_spec_23_3_28_9) {
    JitResult r = runAot(kVirtualCompoundLiteral);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 186);  // 5050 mod 256
}
