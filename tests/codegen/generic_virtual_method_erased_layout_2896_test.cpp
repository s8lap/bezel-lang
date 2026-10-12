// fix #2896 / §14.1 "Generic declarations" + §12 "Inheritance and Dispatch" +
// §12.1 "Members are non-virtual by default" + §12.7 "Type identity" +
// §22.8 "Field initializers", v0.240.
//
// A method marked `virtual` (or `override`) on a GENERIC class was dispatched to
// the erased `T -> i32` TEMPLATE body, which was compiled against the
// placeholder layout. The object is allocated at the instantiation's layout
// (correct since #617/#1101-B/#2840, and for a generic derived class since
// #2889), so every field access inside that shared body reads the wrong offset:
// `SEGV on unknown address 0xc ... in bz$H.GBase$Show` — the frame naming the
// TEMPLATE, not a `$spec` body — on ordinary single-module code with no
// diagnostic at any severity. NO INHERITANCE IS REQUIRED; the narrowest repro is
// a plain generic class with one `virtual` method.
//
// WHY IT SLIPPED. The virtual dispatch branch decided whether to monomorphize by
// comparing SIGNATURES: `Show(): int` lowers to `(ptr) -> i32` under the erased
// AND the concrete substitution alike, so the test saw "nothing to specialize"
// and called the template. The two bodies differ in LAYOUT, not in signature —
// which the NON-virtual twin fifty lines below already says in as many words
// ("we must specialize the call — even for methods with non-generic
// param/return types (like `Size(): int`), because the struct layout changes
// when generic field types ... are substituted").
//
// THE FIX asks #2019's layout-identity question instead — the property the call
// actually needs — and routes a firing to `emitVirtualGenericDispatch` (#580),
// NOT to the non-virtual twin's `emitInlineGenericMethod`: `canDispatch` is true
// here, so inlining the statically-resolved body would DEVIRTUALIZE and run the
// wrong override. `dynamic_dispatch_through_generic_base` below is the case that
// pins that distinction.
//
// Reaching a candidate through a generic base also required naming the
// candidate's PER-INSTANTIATION descriptor (§12.9 / #1843): a `DGen<string>`
// object's vtable slot 0 holds `td$DGen_string`, so the pre-existing comparison
// against the shared `td$H.DGen` was never equal and the override silently lost
// to the default branch (measured: 44 where 149 is right — a wrong VALUE, which
// is worse than the crash it replaced).
//
// Every test here runs end to end under generated-code AddressSanitizer /
// LeakSanitizer.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A `Cell` whose live count is the exactly-once disposal oracle.
const char* kCountedCell =
    "class Cell "
    "  public tag: int := 3; "
    "  public static live: int := 0; "
    "  public static made: int := 0; "
    "  public constructor() "
    "    Cell.live := Cell.live + 1; Cell.made := Cell.made + 1; "
    "  end constructor; "
    "  destructor Cell.live := Cell.live - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// The issue's repro, verbatim: ONE generic class, ONE `virtual` method, no
// inheritance. Was `SEGV on unknown address 0xc` in `bz$H.GBase$Show`; must now
// read `a` (41) and `cell.tag` (3).
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, virtual_on_plain_generic_class_spec_12_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class C public static method Main(): int "
        "  using var s := new GBase<string>(); "
        "  return s.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// The `override` half of the same defect: a generic derived class over a generic
// base whose parameter is RENAMED. The override's body reads BOTH its own field
// and two inherited ones, so a wrong offset anywhere is observable.
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, override_on_generic_derived_spec_12) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class DGen<U> : GBase<U> "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return a + own + cell.tag; "
        "  end Show; "
        "end DGen; "
        "class C public static method Main(): int "
        "  using var d := new DGen<string>(); "
        "  return d.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 49);
}

// ===========================================================================
// THE DEVIRTUALIZATION DISCRIMINATOR (§12): the receiver's STATIC type is the
// generic base while its DYNAMIC type is the generic override. A fix that
// inlined the statically-resolved body would return 44 here; a fix that
// compared against the shared (non-instantiation) descriptor would ALSO return
// 44, because the branch could never be taken. Only a dispatch that names the
// candidate's per-instantiation descriptor (§12.9) returns 149.
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, dynamic_dispatch_through_generic_base_spec_12) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class DGen<U> : GBase<U> "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end DGen; "
        "class C public static method Main(): int "
        "  using var d := new DGen<string>(); "
        "  var g: GBase<string> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}

// ===========================================================================
// A THREE-LEVEL chain with the type parameter RENAMED at every link, the
// override at the leaf, and an owned `Cell` at every level (so a wrong offset
// anywhere is a wrong `tag` or a crash).
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, three_level_renamed_params_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class L3<A> "
        "  public av: A; "
        "  public owned c3: Cell := new Cell(); "
        "  public n3: int := 7; "
        "  public virtual method Show(): int return n3 + c3.tag; end Show; "
        "end L3; "
        "class L2<B> : L3<B> "
        "  public bv: B; "
        "  public owned c2: Cell := new Cell(); "
        "  public n2: int := 11; "
        "end L2; "
        "class L1<D> : L2<D> "
        "  public dv: D; "
        "  public owned c1: Cell := new Cell(); "
        "  public n1: int := 13; "
        "  public override method Show(): int "
        "    return n1 + n2 + n3 + c1.tag + c2.tag + c3.tag; "
        "  end Show; "
        "end L1; "
        "class C public static method Main(): int "
        "  using var x := new L1<string>(); "
        "  return x.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 13 + 11 + 7 + 3 + 3 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 40);
}

// ===========================================================================
// A `virtual` method that WRITES through the wrong offset is the silent-
// corruption face of the same defect (the read face merely crashes). Bump the
// inherited `a` and the owned cell's `tag`, then read both back.
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, virtual_writes_inherited_field_spec_22_8) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBox<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Bump(n: int) "
        "    a := a + n; cell.tag := cell.tag + 1; "
        "  end Bump; "
        "  public method Read(): int return a + cell.tag; end Read; "
        "end GBox; "
        "class C public static method Main(): int "
        "  using var b := new GBox<string>(); "
        "  b.Bump(5); "
        "  return b.Read(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 46 + 4
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}

// ===========================================================================
// A two-parameter base reached with the arguments PERMUTED
// (`class D2<X, Y> : B2<Y, X>`), dispatched through the base's static type.
// Inverting the base clause has to solve BOTH parameters and in the right
// order, or the candidate's descriptor names a different instantiation.
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, permuted_parameters_via_base_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class B2<P, Q> "
        "  public p: P; public q: Q; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end B2; "
        "class D2<X, Y> : B2<Y, X> "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int, string>(); "
        "  var g: B2<string, int> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}

// ===========================================================================
// Several instantiations of ONE generic class in ONE program, each calling the
// same `virtual` method. Each must get its own body at its own layout — a
// shared specialization would give one of them the other's offsets.
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, several_instantiations_spec_14_2) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class C public static method Main(): int "
        "  using var s := new GBase<string>(); "
        "  using var l := new GBase<long>(); "
        "  using var i := new GBase<int>(); "
        "  using var d := new GBase<double>(); "
        "  return s.Show() + l.Show() + i.Show() + d.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 4 * 44
    BEZEL_EXPECT_EQ(r.exitCode, 176);
}

// ===========================================================================
// §28.4/§28.5 EXACTLY-ONCE DISPOSAL under the new dispatch. A wrong-offset write
// through the virtual body can silently blank an owned-field slot so that its
// disposal does nothing, which "does not crash" would pass. `Cell.live` at the
// end is the oracle; `Cell.made` pins that both owned cells were built.
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, exactly_once_disposal_loop_spec_28_4) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class DGen<U> : GBase<U> "
        "  public owned own: Cell := new Cell(); "
        "  public override method Show(): int "
        "    return a + own.tag + cell.tag; "
        "  end Show; "
        "end DGen; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..12 do "
        "    using var d := new DGen<string>(); "
        "    var g: GBase<string> := d; "
        "    acc := acc + g.Show(); "
        "  end; "
        "  return (acc - 564) + Cell.live * 100 + (Cell.made - 24); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // per iteration 41 + 3 + 3 = 47; 12 iterations = 564; 24 cells, 0 live.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// A NON-generic subclass of the generic base, dispatched through the base's
// static type: the candidate keeps the shared descriptor (it has no
// instantiation of its own) and must still win the dispatch.
//
// MEASURED VACUOUS as a fix oracle — it passes on the parent commit too, because
// a non-generic derived class's own struct is already composed from its base
// clause (#617/#1101-B) and its `Show` is compiled at that composed layout. It
// is kept as the OVER-DISPATCH control for the new descriptor comparison: a fix
// that switched every candidate to a per-instantiation descriptor would stop
// matching this one and silently fall through to the base's body (44).
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, non_generic_subclass_via_generic_base_spec_12) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class DS : GBase<string> "
        "  public own: int := 5; "
        "  public override method Show(): int "
        "    return 100 + a + own + cell.tag; "
        "  end Show; "
        "end DS; "
        "class C public static method Main(): int "
        "  using var d := new DS(); "
        "  var g: GBase<string> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}

// ===========================================================================
// A generic subclass that does NOT override: the base's own body must run, at
// the DERIVED object's layout. This is the over-dispatch control — a fix that
// took the subclass branch anyway would run the wrong body.
// ===========================================================================
BEZEL_TEST(generic_virtual_erased_layout_2896, generic_subclass_without_override_spec_12) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class DGen<U> : GBase<U> public own: int := 5; end DGen; "
        "class C public static method Main(): int "
        "  using var d := new DGen<string>(); "
        "  var g: GBase<string> := d; "
        "  return g.Show(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// ACCEPT-CONTROLS. These pass with the fix patched out as well; they are here to
// prove the fix does not narrow or disturb what already worked.
// ===========================================================================

// `T = int` — the instantiation whose layout IS the erased placeholder's, so the
// layout-identity predicate is FALSE and the call stays on today's vtable path —
// plus a `virtual` method on a NON-generic class, which the predicate never
// looks at.
BEZEL_TEST(generic_virtual_erased_layout_2896, control_int_arg_and_non_generic_parent) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual method Show(): int return a + cell.tag; end Show; "
        "end GBase; "
        "class Plain "
        "  public owned cell2: Cell := new Cell(); "
        "  public p: int := 4; "
        "  public virtual method Show(): int return p + cell2.tag; end Show; "
        "end Plain; "
        "class PlainD : Plain "
        "  public override method Show(): int return 100 + p + cell2.tag; "
        "  end Show; "
        "end PlainD; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  do "
        "    using var i := new GBase<int>(); "
        "    using var pd := new PlainD(); "
        "    var pb: Plain := pd; "
        "    n := i.Show() + pb.Show(); "
        "  end; "
        "  return n - 151; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 44 + 107
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The #580 shape this fix widens: a virtual method with its OWN `<T>`
// parameters, whose gate already fired. Untouched.
BEZEL_TEST(generic_virtual_erased_layout_2896, control_method_level_generic_virtual_580) {
    auto r = runAotChecked(
        "module H; "
        "class Base "
        "  public virtual method Echo<T>(x: T): string return x.ToString(); "
        "  end Echo; "
        "end Base; "
        "class Der : Base "
        "  public override method Echo<U>(x: U): string "
        "    return \"d\" + x.ToString(); "
        "  end Echo; "
        "end Der; "
        "class C public static method Main(): int "
        "  using var d := new Der(); "
        "  var b: Base := d; "
        "  return b.Echo(7).Length + b.Echo(\"ab\").Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // "d7".Length == 2, "dab".Length == 3
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
