// fix #2882 / §22.8 "Field initializers" step 1 ("The base or chained
// constructor call") + §14.1 "Generic declarations" + §12 "Inheritance and
// Dispatch" + §28.5 "Owned fields", v0.240.
//
// An IMPLICIT base chain into a GENERIC base's explicit parameterless
// constructor called `ctorFns_[target]` — the shared, type-erased
// `bz$<Base>$ctor`, emitted once against the base template's `T -> i32`
// placeholder layout. The object is allocated with the INSTANTIATION's layout
// (#617/#1101-B/#2840), so every store in that shared body landed at the
// placeholder's offsets: on `GCtor<T> { v: T; owned cell: Cell; a: int }` at
// T = string the base ctor's `cell := new Cell()` wrote 8 bytes early, over
// `v`, and the read of `d.cell` SEGV'd on the still-zero slot with no output
// and no diagnostic at any severity.
//
// §22.8 step 1 is the base constructor call; §14.1 makes each instantiation its
// own type. The fix asks the #2019 layout-identity question and, when the
// instantiation's layout differs from the erased template's, INLINES the base
// ctor's body under the base instantiation's substitution — the same mechanism
// an EXPLICIT `base()` has always used (emitChainCall's #585/#617 inline),
// which is why the explicit spelling was already correct.
//
// Every test here runs the program end to end under generated-code
// AddressSanitizer/LeakSanitizer, because a constructor writing at the wrong
// offsets can corrupt an owned-field slot so that its disposal silently does
// nothing — a live-object count, not merely "does not crash", is the oracle.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 8 rows returned WITHOUT asserting and the suite reported a clean PASS
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

// The shared generic base of most cases below: a type-parameter field AHEAD of
// an owned class field, so a non-placeholder-compatible T shifts `cell`.
const char* kGCtor =
    "class Cell public tag: int := 3; end Cell; "
    "class GCtor<T> "
    "  public v: T; "
    "  public owned cell: Cell; "
    "  public a: int; "
    "  public constructor() cell := new Cell(); a := 41; end constructor; "
    "end GCtor; ";

}  // namespace

// ===========================================================================
// The issue's exact repro: no derived constructor at all, so §22.8 step 1 runs
// as an IMPLICIT base chain. Was a SEGV on `d.cell`; must now read 41 and 3.
// ===========================================================================
BEZEL_TEST(implicit_base_generic_ctor_2882, implicit_chain_reference_type_arg_spec_22_8) {
    auto r = runAotChecked(
        std::string("module H; ") + kGCtor +
        "class DCtor : GCtor<string> end DCtor; "
        "class C public static method Main(): int "
        "  using var d := new DCtor(); "
        "  return d.a + d.cell.tag; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// TWO LEVELS of derivation through a NON-GENERIC middle link, plus a second
// instantiation of the same base in the same module (`GCtor<long>`), so the
// per-instantiation resolution is exercised more than once.
// ===========================================================================
BEZEL_TEST(implicit_base_generic_ctor_2882, two_levels_and_two_instantiations_spec_14_1) {
    auto r = runAotChecked(
        std::string("module H; ") + kGCtor +
        "class Mid : GCtor<string> end Mid; "
        "class Leaf : Mid public own: int := 7; end Leaf; "
        "class DLong : GCtor<long> end DLong; "
        "class C public static method Main(): int "
        "  using var l := new Leaf(); "
        "  using var g := new DLong(); "
        "  return l.a + l.cell.tag + l.own + g.a + g.cell.tag; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 95);   // 41+3+7 + 41+3
}

// ===========================================================================
// Axis sweep: the type-parameter field BEFORE and AFTER a managed (string)
// field, over value / reference / managed / int type arguments. `GPost` puts
// `v: T` last, so its earlier fields keep the template's offsets while the
// STRUCT SIZE still changes — the shape a naive "only the first field moves"
// reading would miss.
// ===========================================================================
BEZEL_TEST(implicit_base_generic_ctor_2882, field_order_and_type_arg_axes_spec_22_8) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GPre<T> "
        "  public v: T; public owned cell: Cell; public name: string; "
        "  public a: int; "
        "  public constructor() "
        "    cell := new Cell(); name := \"pre\"; a := 41; "
        "  end constructor; "
        "end GPre; "
        "class GPost<T> "
        "  public name: string; public owned cell: Cell; public a: int; "
        "  public v: T; "
        "  public constructor() "
        "    cell := new Cell(); name := \"post\"; a := 17; "
        "  end constructor; "
        "end GPost; "
        "class DPreStr : GPre<string> end DPreStr; "
        "class DPreLong : GPre<long> end DPreLong; "
        "class DPreCell : GPre<Cell> end DPreCell; "
        "class DPreInt : GPre<int> end DPreInt; "
        "class DPostStr : GPost<string> end DPostStr; "
        "class DPostLong : GPost<long> end DPostLong; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  do "
        "    using var p1 := new DPreStr(); "
        "    using var p2 := new DPreLong(); "
        "    using var p3 := new DPreCell(); "
        "    using var p4 := new DPreInt(); "
        "    using var q1 := new DPostStr(); "
        "    using var q2 := new DPostLong(); "
        "    n := p1.a + p1.cell.tag + p1.name.Length "
        "       + p2.a + p2.cell.tag + p2.name.Length "
        "       + p3.a + p3.cell.tag + p3.name.Length "
        "       + p4.a + p4.cell.tag + p4.name.Length "
        "       + q1.a + q1.cell.tag + q1.name.Length "
        "       + q2.a + q2.cell.tag + q2.name.Length; "
        "  end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 236);  // 4*(41+3+3) + 2*(17+3+4)
}

// ===========================================================================
// The paths the fix could BREAK, all under a live-Cell counter:
//   * a derived class WITH its own constructor and NO explicit `base()` — the
//     implicit chain reached from inside a compiled derived ctor;
//   * a base ctor that THROWS. §22.9 bullet 1: a chain-call throw "leaves no
//     derived state to clean up" and is the base ctor's own responsibility. The
//     owned `Cell` it had already built must be disposed exactly once and the
//     object freed — no leak, no double free. Inlining routes this through
//     inlineCtorBody's own cleanup block, which is the behaviour an explicit
//     `base()` already had;
//   * a base ctor that delegates with `this(...)`.
// `Cell.live == 0` at the end is the exactly-once assertion.
// ===========================================================================
BEZEL_TEST(implicit_base_generic_ctor_2882, own_ctor_throwing_base_and_this_delegation_spec_22_9) {
    auto r = runAotChecked(
        "module H; "
        "class Cell "
        "  public tag: int := 3; "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class GCtor<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor() cell := new Cell(); a := 41; end constructor; "
        "end GCtor; "
        "class DOwnCtor : GCtor<string> "
        "  public own: int; "
        "  public constructor() own := 9; end constructor; "
        "end DOwnCtor; "
        "class GThrow<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor() "
        "    cell := new Cell(); a := 41; "
        "    throw new InvalidOperationException(\"base boom\"); "
        "  end constructor; "
        "end GThrow; "
        "class DThrow : GThrow<string> end DThrow; "
        "class GDelegate<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor(k: int) cell := new Cell(); a := k; end constructor; "
        "  public constructor() this(23); end constructor; "
        "end GDelegate; "
        "class DDelegate : GDelegate<string> end DDelegate; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  do using var d := new DOwnCtor(); "
        "     n := n + d.a + d.cell.tag + d.own; end; "
        "  do using var g := new DDelegate(); "
        "     n := n + g.a + g.cell.tag; end; "
        "  try "
        "    using var t := new DThrow(); "
        "    n := n + 1000; "
        "  catch e: InvalidOperationException => "
        "    n := n + 100; "
        "  end; "
        "  return n + Cell.live; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 179);  // (41+3+9) + (23+3) + 100 + 0 live
}

// ===========================================================================
// Loop-amplified live-object accounting. A single `Main` is not a reliable
// oracle: a wrong-offset store can leave an owned-field slot whose disposal
// silently no-ops, which shows up as an imbalance across many iterations rather
// than as a crash on the first. 20 iterations, two instantiations each.
// ===========================================================================
BEZEL_TEST(implicit_base_generic_ctor_2882, loop_amplified_owned_base_field_disposed_once) {
    auto r = runAotChecked(
        "module H; "
        "class Cell "
        "  public tag: int := 3; "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  public constructor() "
        "    Cell.live := Cell.live + 1; Cell.made := Cell.made + 1; "
        "  end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class GCtor<T> "
        "  public v: T; public owned cell: Cell; public a: int; "
        "  public constructor(seed: T) "
        "    cell := new Cell(); a := 41; v := seed; "
        "  end constructor; "
        "  public constructor() cell := new Cell(); a := 41; end constructor; "
        "end GCtor; "
        "class DStr : GCtor<string> end DStr; "
        "class DCell : GCtor<Cell> end DCell; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..20 do "
        "    using var d := new DStr(); "
        "    using var e := new DCell(); "
        "    acc := acc + d.cell.tag + e.cell.tag; "
        "  end; "
        "  return acc + Cell.live * 1000 + (Cell.made - 40); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 120);  // 20*(3+3), 0 live, 40 made
}

// ===========================================================================
// ACCEPT-CONTROLS. These pass with the fix patched out as well; they are here
// to prove the fix does not narrow what already worked.
// ===========================================================================

// `T = int`: the instantiation's layout IS the erased placeholder's, so the
// layout-identity gate must decline and leave the shared ctor call in place.
BEZEL_TEST(implicit_base_generic_ctor_2882, control_int_type_arg_layout_equals_placeholder) {
    auto r = runAotChecked(
        std::string("module H; ") + kGCtor +
        "class DInt : GCtor<int> end DInt; "
        "class C public static method Main(): int "
        "  using var d := new DInt(); "
        "  return d.a + d.cell.tag; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// The #2873-fixed shape (generic base with NO explicit constructor, field
// initializers only — it goes through emitLocalBaseFieldInit, not this path)
// and a DIRECT `new GCtor<string>()` of the generic class itself.
BEZEL_TEST(implicit_base_generic_ctor_2882, control_no_ctor_base_and_direct_new_unchanged) {
    auto r = runAotChecked(
        std::string("module H; ") + kGCtor +
        "class GNoCtor<T> "
        "  public v: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "end GNoCtor; "
        "class DNoCtor : GNoCtor<string> end DNoCtor; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  do "
        "    using var d := new DNoCtor(); "
        "    using var g := new GCtor<string>(); "
        "    n := d.a + d.cell.tag + g.a + g.cell.tag; "
        "  end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}

// The known-good control the issue itself identified: an EXPLICIT `base()` from
// a derived ctor, here at the middle link of a two-level chain. emitChainCall
// already inlined this; it must keep working byte-for-byte.
BEZEL_TEST(implicit_base_generic_ctor_2882, control_explicit_base_call_unchanged) {
    auto r = runAotChecked(
        std::string("module H; ") + kGCtor +
        "class MidChain : GCtor<string> "
        "  public constructor() base(); end constructor; "
        "end MidChain; "
        "class LeafChain : MidChain public own: int := 7; end LeafChain; "
        "class C public static method Main(): int "
        "  using var l := new LeafChain(); "
        "  return l.a + l.cell.tag + l.own; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 51);
}
