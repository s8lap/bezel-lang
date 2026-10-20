// fix #2889 / §14.1 "Generic declarations" + §12 "Inheritance and Dispatch" +
// §22.8 "Field initializers" + §28.4 "The `destructor`" + §28.5 "Owned fields",
// v0.240.
//
// A GENERIC derived class over a GENERIC base was ALLOCATED at the base
// template's erased `T -> i32` layout.
//
// The layout machinery keys type-parameter bindings by PARAMETER NAME in one
// flat map, and every site that installs a substitution for a generic class
// binds that class's OWN parameter names — `lowerNew` from the `new` site's type
// arguments, method emission from the receiver's. Storage INHERITED from a
// generic ancestor is declared in the ANCESTOR's names, so for
// `class D<U> : B<U>` (B's field is `v: T`) nothing named `T` and `storageType`
// produced the i32 placeholder for the inherited slot. `lowerNew` sizes its
// calloc from exactly that struct, so `new D<string>()` allocated a 32-byte
// object while every reader — correct since #2840 — GEP'd the 40-byte
// instantiation offsets: `heap-buffer-overflow ... 0 bytes after 32-byte region`
// on ordinary single-module code, with no diagnostic at any severity.
//
// This is the ALLOCATION face of the placeholder-layout family whose other faces
// are #2840 (field access), #2860 (imported field init), #2873 (disposer + local
// field init) and #2882 (implicit base constructor). Each of those is correct and
// still read out of bounds, because they all read a buffer that was too small.
//
// The fix composes the missing half of the substitution: for each generic
// ancestor on the chain, `computeAncestorGenericSubst` (the shared #2840 helper)
// is asked for that ancestor's own bindings and what it PROVES is merged in,
// most-derived-wins. Its seed is widened to read a synthesized bare receiver's
// arguments out of the ACTIVE substitution — the information the synthesized-
// receiver callers always had and never passed — which is what made
// `class D<U> : B<U>` resolvable at all.
//
// Every test here runs end to end under generated-code AddressSanitizer /
// LeakSanitizer, and the multi-shape ones carry a live-object counter: a
// wrong-size allocation can corrupt an owned-field slot so that its disposal
// silently does nothing, which "does not crash" would pass.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 9 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 9 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

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
// The issue's repro 1: generic derived, generic base, NO constructor anywhere
// (field initializers only). Was `heap-buffer-overflow ... READ of size 4 ...
// 0 bytes after 32-byte region`; must now read 41 and 3.
// ===========================================================================
BEZEL_TEST(generic_derived_generic_base_2889, no_ctor_reference_type_arg_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GNoCtor<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "end GNoCtor; "
        "class DGenNo<U> : GNoCtor<U> end DGenNo; "
        "class C public static method Main(): int "
        "  using var d := new DGenNo<string>(); "
        "  return d.a + d.cell.tag; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// The issue's repro 2: an explicit base constructor AND an explicit `base()`.
// This is the discriminator that puts the defect OUTSIDE #2882 — emitChainCall's
// inline path is the one #2882 relies on being correct, and it still failed,
// because it assumed the base's parameter shares the derived's spelling.
// ===========================================================================
BEZEL_TEST(generic_derived_generic_base_2889, explicit_base_call_spec_22_8_step_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GCtor<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor() cell := new Cell(); a := 41; end constructor; "
        "end GCtor; "
        "class DGenChain<U> : GCtor<U> "
        "  public own: int; "
        "  public constructor() base(); own := 9; end constructor; "
        "end DGenChain; "
        "class C public static method Main(): int "
        "  using var d := new DGenChain<string>(); "
        "  return d.a + d.cell.tag + d.own; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 53);
}

// ===========================================================================
// The issue's repro 3: a GENERIC derived class over a CONCRETE base
// instantiation. The base's type argument is fully concrete in the source, yet
// the derived class being generic was enough to erase the layout.
// ===========================================================================
BEZEL_TEST(generic_derived_generic_base_2889, concrete_base_instantiation_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GCtor<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor() cell := new Cell(); a := 41; end constructor; "
        "end GCtor; "
        "class DConc<U> : GCtor<string> public extra: U; end DConc; "
        "class C public static method Main(): int "
        "  using var d := new DConc<int>(); "
        "  return d.a + d.cell.tag; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// TWO LEVELS of GENERIC derivation with a RENAMED parameter at every link
// (`L3<X> : M3<W> : B3<T>`), read through a qualified access AND through an
// implicit `this` inside the derived class's own method body.
//
// The middle link is what the construction sequence gets wrong on its own: it
// recurses level by level and hands each level's IMMEDIATE derived class to the
// substitution helper, so at `M3` the receiver's parameter `W` is not named by
// the `new` site's substitution. Measured before the fix: SEGV on `l.cell.tag`;
// with the allocation fixed but the chain still unresolved: the base
// initializers stored 8 bytes early and the owned `Cell` leaked 16 bytes.
// ===========================================================================
BEZEL_TEST(generic_derived_generic_base_2889, three_level_renamed_params_spec_12) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class B3<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "end B3; "
        "class M3<W> : B3<W> public mid: int := 7; end M3; "
        "class L3<X> : M3<X> "
        "  public leaf: int := 11; "
        "  public method Total(): int return a + mid + leaf + cell.tag; end Total; "
        "end L3; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..15 do "
        "    using var s := new L3<string>(); "
        "    using var n := new L3<int>(); "
        "    acc := acc + s.a + s.mid + s.leaf + s.cell.tag; "
        "    acc := acc + s.Total() + n.Total(); "
        "  end; "
        "  return (acc - 2790) + Cell.live * 100 + (Cell.made - 30); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // per iteration: 62 (qualified) + 62 (L3<string>.Total) + 62 (L3<int>.Total)
    //              = 186; 15 iterations = 2790. 30 Cells made, 0 left live.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// Axis sweep under a live-object counter, loop-amplified. The type-parameter
// field sits BEFORE a managed field in `GPre` and AFTER one in `GPost` — the
// latter keeps every earlier field at the template's offset while the STRUCT
// SIZE still changes, the shape a "only the first field moves" reading misses.
// Type arguments span reference (string, class), wide value (long), and the
// int control whose layout equals the placeholder. Inherited fields are both
// READ and WRITTEN, with the written value asserted.
// ===========================================================================
BEZEL_TEST(generic_derived_generic_base_2889, field_order_and_type_arg_axes_spec_22_8) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class GPre<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public name: string := \"pre\"; "
        "  public a: int := 41; "
        "end GPre; "
        "class GPost<T> "
        "  public name: string := \"post\"; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 17; "
        "  public v: T; "
        "end GPost; "
        "class DPre<U> : GPre<U> public own: int := 5; end DPre; "
        "class DPost<U> : GPost<U> public own: int := 5; end DPost; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..20 do "
        "    using var p1 := new DPre<string>(); "
        "    using var p2 := new DPre<long>(); "
        "    using var p3 := new DPre<Cell>(); "
        "    using var p4 := new DPre<int>(); "
        "    using var q1 := new DPost<string>(); "
        "    using var q2 := new DPost<double>(); "
        "    p1.a := p1.a + 1; "
        "    q1.a := q1.a + 1; "
        "    acc := acc + p1.a + p1.cell.tag + p1.name.Length + p1.own; "
        "    acc := acc + p2.a + p2.cell.tag + p2.name.Length + p2.own; "
        "    acc := acc + p3.a + p3.cell.tag + p3.name.Length + p3.own; "
        "    acc := acc + p4.a + p4.cell.tag + p4.name.Length + p4.own; "
        "    acc := acc + q1.a + q1.cell.tag + q1.name.Length + q1.own; "
        "    acc := acc + q2.a + q2.cell.tag + q2.name.Length + q2.own; "
        "  end; "
        "  return (acc - 5360) + Cell.live * 100 + (Cell.made - 120); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // per iteration: p1 42+3+3+5 = 53; p2/p3/p4 41+3+3+5 = 52 each; q1 18+3+4+5
    // = 30; q2 17+3+4+5 = 29  => 268; 20 iterations = 5360. 120 Cells, 0 live.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// A TWO-PARAMETER generic base whose arguments the derived class PERMUTES
// (`Swap<X, Y> : Pair<Y, X>`), plus a parameterized base constructor reached
// through an explicit `base(k)` and a base that delegates with `this(...)`.
// The permutation is the case a positional "same index ⇒ same parameter"
// shortcut would get wrong.
// ===========================================================================
BEZEL_TEST(generic_derived_generic_base_2889, permuted_two_param_base_spec_14_1) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class Pair<A, B> "
        "  public first: A; "
        "  public owned cell: Cell := new Cell(); "
        "  public second: B; "
        "  public a: int := 41; "
        "end Pair; "
        "class Swap<X, Y> : Pair<Y, X> public own: int := 6; end Swap; "
        "class GArgs<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor(k: int) cell := new Cell(); a := k; end constructor; "
        "  public constructor() this(41); end constructor; "
        "end GArgs; "
        "class DArgs<U> : GArgs<U> "
        "  public own: int; "
        "  public constructor(k: int) base(k); own := 9; end constructor; "
        "end DArgs; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..12 do "
        "    using var s := new Swap<int, string>(); "
        "    s.first := \"abcd\"; "
        "    s.second := 7; "
        "    acc := acc + s.a + s.cell.tag + s.own + s.first.Length + s.second; "
        "    using var d := new DArgs<string>(5); "
        "    acc := acc + d.a + d.cell.tag + d.own; "
        "  end; "
        "  return (acc - 936) + Cell.live * 100 + (Cell.made - 24); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // per iteration: 41+3+6+4+7 = 61 and 5+3+9 = 17 => 78; 12 iterations = 936.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// §22.9 exception safety: a generic base constructor that THROWS, reached from
// a GENERIC derived class. The owned `Cell` it had already built must be
// disposed exactly once and the object freed — no leak, no double free — which
// only holds if the object was allocated at, and the base ctor wrote at, the
// same offsets. `Cell.live == 0` at the end is the assertion.
//
// The throw path ALONE is not an oracle for #2889: with the allocation and the
// stores both erased they agree with each other, so the pre-fix compiler
// unwound cleanly and this half passed either way (measured). It is paired here
// with a SUCCESSFUL construction of the same shape whose inherited fields are
// read, so the case observes the defect and still pins the §22.9 obligation.
// ===========================================================================
BEZEL_TEST(generic_derived_generic_base_2889, throwing_generic_base_ctor_spec_22_9) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class GThrow<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor() "
        "    cell := new Cell(); a := 41; "
        "    throw new InvalidOperationException(\"base boom\"); "
        "  end constructor; "
        "end GThrow; "
        "class DThrow<U> : GThrow<U> end DThrow; "
        "class GOk<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor() cell := new Cell(); a := 41; end constructor; "
        "end GOk; "
        "class DOk<U> : GOk<U> public own: int := 4; end DOk; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  for var i in 1..10 do "
        "    try "
        "      using var t := new DThrow<string>(); "
        "      n := n + 1000; "
        "    catch e: InvalidOperationException => "
        "      n := n + 1; "
        "    end; "
        "    using var k := new DOk<string>(); "
        "    n := n + k.a + k.cell.tag + k.own; "
        "  end; "
        "  return (n - 490) + Cell.live * 100 + (Cell.made - 20); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // per iteration: 1 (caught) + 41 + 3 + 4 = 49; 10 iterations = 490.
    // 20 Cells made (one per throwing ctor, one per successful ctor), 0 live.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT-CONTROLS. These pass with the fix patched out as well; they are here
// to prove the fix does not narrow or disturb what already worked.
// ===========================================================================

// The #2840/#2873/#2882 shapes: a NON-generic derived class over a generic base,
// through a direct link and through a non-generic middle link; plus a direct
// `new GNoCtor<string>()` of the generic class itself; plus the `T = int`
// instantiation whose layout IS the erased placeholder's.
BEZEL_TEST(generic_derived_generic_base_2889, control_non_generic_derived_unchanged_spec_12) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GNoCtor<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "end GNoCtor; "
        "class DNoCtor : GNoCtor<string> end DNoCtor; "
        "class MidC : GNoCtor<string> end MidC; "
        "class LeafC : MidC public own: int := 7; end LeafC; "
        "class DInt : GNoCtor<int> end DInt; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  do "
        "    using var d := new DNoCtor(); "
        "    using var l := new LeafC(); "
        "    using var g := new GNoCtor<string>(); "
        "    using var i := new DInt(); "
        "    n := d.a + d.cell.tag + l.a + l.cell.tag + l.own "
        "       + g.a + g.cell.tag + i.a + i.cell.tag; "
        "  end; "
        "  return n - 183; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 44 + 51 + 44 + 44 = 183
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A GENERIC derived class over a NON-generic base, and a generic derived class
// whose base spells the SAME parameter name (`D<T> : B<T>`) — the one generic
// shape that already worked, because the flat name-keyed substitution happened
// to name the base's parameter too. Both must be untouched.
BEZEL_TEST(generic_derived_generic_base_2889, control_non_generic_base_and_same_param_name) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Plain "
        "  public owned cell2: Cell := new Cell(); "
        "  public p: int := 4; "
        "end Plain; "
        "class DPlain<U> : Plain public u: U; public q: int := 5; end DPlain; "
        "class GSame<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "end GSame; "
        "class DSame<T> : GSame<T> public own: int := 2; end DSame; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  do "
        "    using var d := new DPlain<string>(); "
        "    d.u := \"xy\"; "
        "    using var s := new DSame<Cell>(); "
        "    n := d.p + d.q + d.cell2.tag + d.u.Length "
        "       + s.a + s.cell.tag + s.own; "
        "  end; "
        "  return n - 60; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 4+5+3+2 = 14 and 41+3+2 = 46 => 60
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
