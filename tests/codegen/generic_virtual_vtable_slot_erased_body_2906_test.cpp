// fix #2906 / §14.1 "Generic declarations" + §12 "Inheritance and Dispatch" +
// §12.1 "Members are non-virtual by default" + §12.7 "Type tests: `is`, `is
// not`, `as`" + §2.8.3 rule 4 (one runtime type identity per monomorphization) +
// §22.8 "Field initializers", v0.240.
//
// A PER-INSTANTIATION vtable (`vt$D2$spec0`) minted its own runtime descriptor
// (slot 0, #1843), its own Dispose (slot 2, #1240/#2873) and its own itable
// (slot 1, #1143) — but its VIRTUAL METHOD slots still held the shared, erased
// `T -> i32` TEMPLATE body:
//
//     @"vt$D2$spec0" = ... ptr @"bz$H.D2$Show"      <- the TEMPLATE
//
// so ordinary vtable dispatch on a `D2<int>` object GEP'd the placeholder
// offsets while the object was allocated at the composed ones (correct since
// #617/#1101-B/#2840/#2889/#2897): `SEGV on unknown address 0xc ... in
// bz$H.D2$Show` on legal single-module code with no diagnostic at any severity.
//
// WHY THE CALL SITE CANNOT CLOSE IT. #2896 repaired the shapes whose candidate
// type arguments are DERIVABLE from the receiver's static type, by comparing the
// receiver's runtime descriptor against each candidate's. `class D2<U> :
// GBase<string>` is exactly the shape it refuses: a `GBase<string>` variable can
// hold a `D2<int>` and a `D2<double>` at once, so `deriveDescendantGenericSubst`
// returns false and `emitVirtualGenericDispatch` abandons the dispatch rather
// than emit a comparison it cannot make correct. The set of instantiations
// reachable at such a call site is not statically enumerable, so no call-site
// device can be complete — the body has to be right in the SLOT.
//
// THE FIX adds condition (c) to `specializedVtableFor`: a virtual-method slot is
// re-emitted (`buildSpecializedMethodFn`, the same vehicle the itable slots use)
// when the class DECLARING that slot's implementation has an instance-storage
// layout that MOVES under this substitution — #2019's predicate, exposed by
// #2896 as `instanceLayoutDiffersFromErasedTemplate`, asked per declaring class
// because the declaring class is the one whose `classStruct` the body GEPs. It
// is false for a non-generic declarer, for `T = int`, and for every generic
// class whose storage does not move, so those keep the shared slot untouched.
//
// The fix also creates and REGISTERS the specialized vtable global before
// emitting any specialized body, because a virtual body can construct its own
// instantiation (`t10` below) and re-enter — which previously built a second
// vtable for one instantiation.
//
// SCOPE, stated rather than implied. A virtual PROPERTY / INDEXER accessor slot
// carries no MethodSymbol and is NOT patched — it still runs the template
// accessor, filed separately. A slot whose specialized SIGNATURE moves under the
// substitution is deliberately left alone too: the slot's ABI is fixed by the
// receiver's static method type at every call site.
//
// Every test here runs end to end under generated-code AddressSanitizer /
// LeakSanitizer, and the value is asserted in every case — a fix that only makes
// the SEGV go away is not evidence the value is right (#2896 measured 44 where
// 149 is correct).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 10 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 10 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

// The shared generic base: `v: T` PRECEDES an owned concrete field and an int,
// so `T = string` widens slot 0 of the storage and moves both of the ones after
// it. That is what makes the template body's GEPs land off the object.
const char* kGBase =
    "class Cell public tag: int := 3; end Cell; "
    "class GBase<T> "
    "  public v: T; "
    "  public owned cell: Cell := new Cell(); "
    "  public a: int := 41; "
    "  public virtual method Show(): int return a + cell.tag; end Show; "
    "end GBase; ";

// A `Cell` whose live count is the exactly-once (§28.4/§28.5) disposal oracle.
const char* kCountedCell =
    "class Cell "
    "  public static live: int := 0; "
    "  public static made: int := 0; "
    "  public tag: int := 3; "
    "  public constructor() "
    "    tag := 3; Cell.live := Cell.live + 1; Cell.made := Cell.made + 1; "
    "  end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO. §14.1 / §12.7. `class D2<U> : GBase<string>`: `U`
// is not pinned by the base clause, so no call-site device can name the
// candidate's instantiation. Pre-fix: SEGV (exit 139) in `bz$H.D2$Show`.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, undetermined_own_param_via_base_spec_14_1) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<string> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}

// ===========================================================================
// T2 — TWO INSTANTIATIONS of the same generic subclass in one program, both
// reached through the SAME generic-base static type. This is the discriminator
// for "one patched body per instantiation, not one shared patched body": a
// cache key that conflated them would run one instantiation's body on the
// other's object. §2.8.3 rule 4 (one runtime type identity; duplicate code
// copies only where observationally indistinguishable) / §12.7.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, two_instantiations_via_one_base_spec_2_8_3) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var di := new D2<int>(); "
        "  using var dl := new D2<double>(); "
        "  var g1: GBase<string> := di; "
        "  var g2: GBase<string> := dl; "
        "  return g1.Show() + g2.Show() - 200; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 149 + 149 - 200
    BEZEL_EXPECT_EQ(r.exitCode, 98);
}

// ===========================================================================
// T3 — the receiver's subclass does NOT override, so the slot holds the BASE's
// virtual — whose body GEPs `classStruct(GBase)`, which still MOVES under
// `DPlainSub<int>`'s composition. The gate therefore has to ask the layout
// question about the class that DECLARES the implementation, not about the
// receiver's own class. §12 / §22.8.
//
// WHY THE SIBLING `DOver` IS LOAD-BEARING, and not decoration. Without it, this
// case is VACUOUS — measured, not reasoned: with a single non-overriding
// subclass, `emitVirtualGenericDispatch` finds no branch that differs from the
// default and emits a DIRECT call to the base's own specialization, so the
// vtable is never consulted and the case passes with or without this fix. The
// sibling override makes the branch list non-empty, `deriveDescendantGenericSubst`
// then refuses to invert `<U> : GBase<string>`, the whole dispatch is abandoned,
// and control reaches the vtable slot — which is what this test is for.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, inherited_not_overridden_slot_spec_12) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class DOver<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end DOver; "
        "class DPlainSub<U> : GBase<string> "
        "  public pv: U; "
        "  public pown: int := 6; "
        "end DPlainSub; "
        "class C public static method Main(): int "
        "  using var d := new DPlainSub<int>(); "
        "  var g: GBase<string> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T4 — a virtual method that WRITES through the moved offsets. A wrong-offset
// STORE is the SILENT-CORRUPTION face of this defect (the read face merely
// crashes): it lands on the neighbouring slot, which here is the inherited
// `v: string` pointer. Every written field AND the string are read back.
// §22.8 / §28.5.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, virtual_writes_inherited_field_spec_22_8) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Bump(): int a := a + 1; return a; end Bump; "
        "end GBase; "
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override method Bump(): int "
        "    a := a + 100; own := own + 2; cell.tag := cell.tag + 1; "
        "    return a + own; "
        "  end Bump; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  d.v := \"abcd\"; "
        "  var g: GBase<string> := d; "
        "  var r := g.Bump(); "
        "  return (r - 148) + (d.v.Length - 4) + (d.a - 141) + (d.own - 7) "
        "         + (d.cell.tag - 4); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // Every term is written as a difference from its expected value, so any one
    // of them being wrong shows up as a non-zero exit.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T5 — a THREE-LEVEL chain, generic at every link, dispatched from the topmost
// generic base. The override is declared at the LEAF and reads a field from
// each level. §12 / §14.1.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, three_level_chain_via_root_base_spec_12) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class Mid<W> : GBase<string> "
        "  public mv: W; "
        "  public m: int := 7; "
        "end Mid; "
        "class Leaf<Z> : Mid<int> "
        "  public lv: Z; "
        "  public l: int := 9; "
        "  public override method Show(): int "
        "    return a + cell.tag + m + l; "
        "  end Show; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var x := new Leaf<string>(); "
        "  var g: GBase<string> := x; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 7 + 9
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// ===========================================================================
// T6 — RE-ENTRANCY: the virtual body being specialized CONSTRUCTS its own
// instantiation, so emitting it re-enters `specializedVtableFor` for the same
// (class, type argument). The specialized vtable global must already be
// registered, or that re-entry builds a SECOND vtable and objects minted inside
// the body carry a different global from objects minted outside it.
// §12.7 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, virtual_body_constructs_own_instantiation_spec_2_8_3) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    using var inner := new D2<int>(); "
        "    return 100 + a + own + cell.tag + inner.own; "
        "  end Show; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<string> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3 + 5
    BEZEL_EXPECT_EQ(r.exitCode, 154);
}

// ===========================================================================
// T7 — §28.4/§28.5 EXACTLY-ONCE DISPOSAL under the patched slot. A wrong-offset
// write through a virtual body can blank an owned-field pointer so its disposal
// silently does nothing, which "does not crash" would pass. `Cell.live` is the
// oracle; `Cell.made` pins that all twelve cells were actually built.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, exactly_once_disposal_loop_spec_28_4) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end D2; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..12 do "
        "    using var d := new D2<int>(); "
        "    var g: GBase<string> := d; "
        "    acc := acc + g.Show(); "
        "  end; "
        "  return (acc - 1788) + Cell.live * 100 + (Cell.made - 12); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 12 * 149 = 1788; 12 cells made, 0 live at the end.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T8 — the #2907 SHAPE, made NON-VACUOUS: a `virtual` on a NON-generic base
// overridden by a GENERIC subclass, called through the NON-generic static type.
// No substitution is active at that call site at all, so #2896's whole
// call-site block is skipped and the fixed vtable slot is the ONLY mechanism.
//
// MAINTENANCE NOTE (a pin whose premise can expire): #2907's own repro uses ONE
// `v: T` field, and at `T = string` that field's i32 placeholder and its ptr
// both occupy the same 8-byte aligned slot — so `cell` and `a` land at
// IDENTICAL offsets under both layouts and the repro passes with or without any
// fix. It is VACUOUS. The two `T` fields below are what make the offsets move
// for real; if this case is ever narrowed to one, it stops testing anything.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, nongeneric_base_generic_override_spec_12_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Plain "
        "  public p: int := 4; "
        "  public virtual method Show(): int return p; end Show; "
        "end Plain; "
        "class G<T> : Plain "
        "  public v1: T; "
        "  public v2: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public override method Show(): int "
        "    return a + cell.tag + p; "
        "  end Show; "
        "end G; "
        "class C public static method Main(): int "
        "  using var g := new G<string>(); "
        "  var p: Plain := g; "
        "  return p.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 4
    BEZEL_EXPECT_EQ(r.exitCode, 48);
}

// ===========================================================================
// T9 — ACCEPT-CONTROL. `T = int` leaves the instantiation's storage sequence
// bit-identical to the erased template's, so the layout question is FALSE and
// the shared slot is already right. This case passes with AND without the fix
// by construction — it is here to pin that the gate does not fire where it must
// not, not as an oracle for the fix. §14.1.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, control_t_is_int_layout_identical_spec_14_1) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<int> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<int> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}

// ===========================================================================
// T10 — ACCEPT-CONTROL for the NON-generic subclass of a generic base: the
// object's vtable is the base instantiation's, and this dispatch was already
// correct. Kept so a future widening of the patch that mis-selected a slot for
// a non-generic candidate fails here rather than silently. §12.
// ===========================================================================
BEZEL_TEST(generic_virtual_vtable_slot_2906, control_nongeneric_subclass_of_generic_base_spec_12) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class DPlain : GBase<string> "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end DPlain; "
        "class C public static method Main(): int "
        "  using var d := new DPlain(); "
        "  var g: GBase<string> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}
