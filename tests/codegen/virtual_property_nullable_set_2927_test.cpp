// Spec-derived regression test for issue #2927.
//
// §7.2 (Nullable value types) / §5.1 (Implicit conversions) / §12.1 (Members
// are non-virtual by default — virtual dispatch) / §23.3 (Block-form
// properties) / §22.3 (Parameter modifiers), v0.240.
//
// THE DEFECT. `iface.P := v` where `P` is a NULLABLE VALUE property — or any
// property routed through `lowerVirtualPropertySet` — passed the BARE payload
// to a setter whose parameter is §7.2's tagged `{i8 hasValue; T}` struct, and
// the compile ABORTED. Measured on 10bd6d3 (the parent commit) for
// `i.IP := 4` with `property IP: int?`:
//
//     Call parameter type does not match function signature!
//     i32 4
//      { i8, i32 }  call void %pset(ptr %15, i32 4)
//     bezelc: codegen error: IR verification failed
//
// An ICE on a legal program, with no diagnostic naming any source construct.
// Every non-interface spelling of the same statement was correct: `b.IP := 4`
// on the concrete type, `b._v := 4` on the field, and — since #2905 —
// `i.IP ??= 4` through the same interface, because that path coerces to the
// target's slot before calling the SAME helper. That asymmetry is how the gap
// became visible.
//
// THE CAUSE. `lowerVirtualPropertySet` applied `coerceNumericToType` alone:
// §5.1's numeric widening, which has no case for a nullable value slot (the
// slot is a struct, the value an integer, so it returned the value untouched
// and the malformed call was emitted).
//
// THE FIX applies the SAME pair every other store into a nullable slot uses —
// #2903's `coerceInitializerToSlot` (one definition of an initializer's slot
// width, taking the nullable slot's PAYLOAD as the width to match, in both of
// §5.1's directions) followed by §7.2's `coerceToNullableSlot` present-wrap,
// which also normalizes a `null` literal to the all-zero form. It is applied
// inside `lowerVirtualPropertySet` rather than in `lowerStmt`'s `:=` arm so
// that every caller gets it: the plain assignment, the §22.3 `ref`/`out`
// write-back, and the compound-assign path all reach that one setter.
//
// THE ISSUE'S INFERRED SIBLING DOES NOT REPRODUCE. The filing predicted that
// "presumably the same applies to a §5.1 WIDENING through a virtual/interface
// property (`i.LongP := 5` where `LongP: long`) — NOT probed, inferred from the
// same missing `coerceNumericToType`". MEASURED on 10bd6d3: `i.LongP := 5`
// gives 5 and `i.DblP := 7` gives 7 — both already correct, because
// `coerceNumericToType` was precisely the call that WAS there. T3 pins them so
// the refutation is recorded executably rather than in prose.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — #2927's own repro plus its `null` twin, with a neighbouring field
// asserted so a wrong-slot store would show up too. `_v` is declared FIRST here
// on purpose: T1 is about the VALUE SHAPE (§7.2's tag), not about the slot, and
// tests/codegen/interface_property_lvalue_addr_2931_test.cpp owns the slot.
// Pre-fix: the program does not compile at all (IR verification abort).
// Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_virtual_property_nullable_set_2927, iface_nullable_value_set_7_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module V2927a; "
        "public interface IHasQ "
        "  property IP: int?; "
        "end IHasQ; "
        "public class Box: IHasQ "
        "  public _v: int? := null; "
        "  public _w: int := 0; "
        "  public property IP: int? "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var i: IHasQ := b; "
        "    i.IP := 4; "
        "    if (b._v ?? -1) = 4 then acc := acc + 1; end; "
        "    if b._w = 0 then acc := acc + 2; end; "
        "    i.IP := null; "
        "    if b._v = null then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}

// ===========================================================================
// T2 — the same shape on a CLASS `virtual`/`override` pair (the vtable branch
// of `lowerVirtualPropertySet`, `propSetSlot_`) rather than an interface
// itable, and with an ALREADY-TAGGED `int?` right-hand side beside the bare
// payload — the value that must NOT be wrapped a second time. The write goes
// through a base-typed reference so the override's setter is the one that runs.
// Pre-fix: does not compile.  Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_virtual_property_nullable_set_2927, virtual_class_nullable_set_12_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module V2927b; "
        "public class B "
        "  public _s: int? := null; "
        "  public virtual property NP: int? "
        "    get => _s; "
        "    set => do _s := value; end; "
        "  end; "
        "end B; "
        "public class D: B "
        "  public override property NP: int? "
        "    get => _s; "
        "    set => do _s := value; end; "
        "  end; "
        "end D; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var d1 := new D(); "
        "    var b1: B := d1; "
        "    b1.NP := 4; "
        "    if (d1._s ?? -1) = 4 then acc := acc + 1; end; "
        "    using var d2 := new D(); "
        "    var b2: B := d2; "
        "    var src: int? := 6; "
        "    b2.NP := src; "
        "    if (d2._s ?? -1) = 6 then acc := acc + 2; end; "
        "    using var d3 := new D(); "
        "    var b3: B := d3; "
        "    b3.NP := 9; "
        "    b3.NP := null; "
        "    if d3._s = null then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}

// ===========================================================================
// T3 — THE REFUTED INFERENCE, pinned. §5.1's widening through a virtual or
// interface property was already correct before the fix, and must stay correct
// after it: `coerceInitializerToSlot` subsumes `coerceNumericToType` for a
// non-nullable slot, and adds §5.1's constant-narrowing direction on top.
// A NON-NULLABLE `string` property is included as the reference-typed control
// (no numeric coercion, no §7.2 wrap — the value must pass through untouched).
// Pre-fix: 15.  Post-fix: 15. A deliberate always-green control.
// ===========================================================================
BEZEL_TEST(codegen_virtual_property_nullable_set_2927, widening_through_iface_unchanged_5_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module V2927c; "
        "public interface IHasL "
        "  property LongP: long; "
        "  property DblP: double; "
        "  property SP: string; "
        "  property NSP: string?; "
        "end IHasL; "
        "public class Box: IHasL "
        "  public _z: int := 0; "
        "  public _l: long := 0; "
        "  public _d: double := 0.0; "
        "  public _s: string := \"\"; "
        "  public _ns: string? := null; "
        "  public property LongP: long get => _l; set => do _l := value; end; end; "
        "  public property DblP: double get => _d; set => do _d := value; end; end; "
        "  public property SP: string get => _s; set => do _s := value; end; end; "
        "  public property NSP: string? get => _ns; set => do _ns := value; end; end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var i: IHasL := b; "
        "    i.LongP := 5; "
        "    if b._l = 5 then acc := acc + 1; end; "
        "    i.DblP := 7; "
        "    if b._d = 7.0 then acc := acc + 2; end; "
        "    i.SP := \"hi\"; "
        "    if b._s = \"hi\" then acc := acc + 4; end; "
        "    i.NSP := \"yo\"; "
        "    if (b._ns ?? \"\") = \"yo\" and b._z = 0 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}

// ===========================================================================
// T4 — the OTHER callers of the fixed helper, which is why the coercion was
// placed inside it rather than in `lowerStmt`'s `:=` arm:
//   bit 1  a nullable RECORD property through an interface — the payload is an
//          AGGREGATE, which `coerceInitializerToSlot` must recognize as the
//          nullable slot's payload and pass through untouched rather than
//          treat as a numeric target;
//   bit 2  the §22.3 `ref` write-back on a nullable interface property, which
//          loads the LOCATION's already-tagged value and calls the same setter
//          (reachable only since #2931 stopped `lvalueAddr` claiming an
//          address for this shape);
//   bit 4  `i.IP += 3` on a nullable interface property — §7.11's lifting arm
//          produces an already-tagged value, which must not be re-wrapped.
// Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_virtual_property_nullable_set_2927, other_callers_of_the_setter_22_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module V2927d; "
        "public record Pt "
        "  public x: int; "
        "end Pt; "
        "public interface IHasQ "
        "  property PP: Pt?; "
        "  property IP: int?; "
        "end IHasQ; "
        "public class Box: IHasQ "
        "  public _z: int := 0; "
        "  public _p: Pt? := null; "
        "  public _v: int? := null; "
        "  public property PP: Pt? get => _p; set => do _p := value; end; end; "
        "  public property IP: int? get => _v; set => do _v := value; end; end; "
        "end Box; "
        "public class M "
        "  public static method BumpQ(ref n: int?): int "
        "    n := (n ?? 0) + 5; "
        "    return n ?? -1; "
        "  end BumpQ; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b1 := new Box(); "
        "    var i1: IHasQ := b1; "
        "    i1.PP := new Pt(x := 6); "
        "    var q := b1._p!; "
        "    if q.x = 6 and b1._z = 0 then acc := acc + 1; end; "
        "    using var b2 := new Box(); "
        "    var i2: IHasQ := b2; "
        "    M.BumpQ(ref i2.IP); "
        "    if (b2._v ?? -1) = 5 and b2._z = 0 then acc := acc + 2; end; "
        // §6.6 / §7.11 (v0.286, #3298): the third leg used to be
        // `i3.IP += 3;` — a read-modify-write compound over a nullable
        // interface property, which is a compile error now (a property is
        // never narrowable under §7.4, so no guard rescues it). #2927's
        // subject is that OTHER CALLERS of the itable setter route through it
        // without disturbing `_z`, so the leg is respelled as the plain
        // assignment that survives; the rejection itself is pinned by
        // `codegen_nullable_property_compound_2888`.
        "    using var b3 := new Box(); "
        "    b3._v := 1; "
        "    var i3: IHasQ := b3; "
        "    i3.IP := (b3._v ?? 0) + 3; "
        "    if (b3._v ?? -1) = 4 and b3._z = 0 then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}
