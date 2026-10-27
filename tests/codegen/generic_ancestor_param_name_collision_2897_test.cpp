// fix #2897 / §14.1 "Generic declarations" + §12 "Inheritance and Dispatch" +
// §22.8 "Field initializers" + §28.5 "Owned fields", v0.240.
//
// Codegen represents a type-parameter substitution as ONE FLAT MAP KEYED BY
// PARAMETER NAME (`genericSubst_` / `genericSubstSemaTy_`). When a derived class
// and one of its ancestors both declare a parameter with the SAME SPELLING but
// the derived class binds the ancestor's to a DIFFERENT argument
// (`class Coll<T> : Base<string>`, where `Base`'s parameter is also `T`), the
// two bindings cannot coexist: `storageType` looks the name up once and gets
// whichever binding won.
//
// Every composing walk in LlvmBackend_Layout.cpp had to pick a winner and said
// so in its comment — `classStruct`'s non-generic-derived walk ("keep the
// lowest/most-derived binding on a param-name collision") and #2889's
// `ancestorGenericSubstAdditions` (most-derived wins). So the ALLOCATION laid
// `bv` out as the i32 placeholder (the active substitution binds `T -> i32` from
// `Coll<int>`), while the field-access path — `fieldAddr`'s inherited-field
// branch calling `computeAncestorGenericSubst`, correct since #2840 — resolved
// `Base`'s `T` to `string` and GEP'd an 8-byte slot. Store and load disagreed,
// and the 8-byte store past a 4-byte slot corrupted the field after it:
// `AddressSanitizer: SEGV ... in __asan::Allocator::Deallocate` / `SEGV ... in
// main`, on legal single-module code with no diagnostic at any severity.
//
// THE FIX resolves each instance-storage slot under the substitution of the
// class that DECLARES it (`instanceStorageTypes`), instead of under one flat
// map. The ancestor's own binding is recoverable exactly, because
// `computeAncestorGenericSubst` re-derives every level POSITIONALLY from the
// base clause — a shared parameter spelling never reaches it. This is provably
// inert without a collision: an ancestor's own map and the flat map agree on
// that ancestor's parameter names whenever nothing overwrote them, and an
// ancestor whose substitution cannot be resolved keeps the flat map.
//
// Every test here runs end to end under generated-code AddressSanitizer /
// LeakSanitizer, and the loop case carries a live-object counter: a wrong-size
// slot can blank an owned-field pointer so its disposal silently does nothing,
// which "does not crash" would pass.

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
// The issue's repro, verbatim. `Base`'s `T` is `string` and `Coll`'s `T` is
// `int`; the inherited `bv: T` must be an 8-byte slot and `dv: T` a 4-byte one,
// in one object.
// ===========================================================================
BEZEL_TEST(generic_ancestor_param_collision_2897, base_reuses_derived_param_name_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Coll<T> : Base<string> "
        "  public dv: T; "
        "  public da: int := 6; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  c.bv := \"abcd\"; "
        "  c.dv := 9; "
        "  return c.ba + c.cell.tag + c.da + c.bv.Length + c.dv; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 6 + 4 + 9
    BEZEL_EXPECT_EQ(r.exitCode, 63);
}

// ===========================================================================
// The same collision with EXPLICIT constructors and an explicit `base()`, so
// the §22.8 step-1 chain (emitChainCall's inline path, and #2882's
// layout-identity gate on it) is exercised rather than the field-initializer
// path. The base's `cell := new Cell()` must land at the slot the object has.
// ===========================================================================
BEZEL_TEST(generic_ancestor_param_collision_2897, explicit_base_ctor_over_collision_spec_22_8) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; public owned cell: Cell; public ba: int; "
        "  public constructor() cell := new Cell(); ba := 41; end constructor; "
        "end Base; "
        "class Coll<T> : Base<string> "
        "  public dv: T; public da: int; "
        "  public constructor() base(); da := 6; end constructor; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  c.bv := \"abcd\"; "
        "  c.dv := 9; "
        "  return c.ba + c.cell.tag + c.da + c.bv.Length + c.dv; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 6 + 4 + 9
    BEZEL_EXPECT_EQ(r.exitCode, 63);
}

// ===========================================================================
// THREE levels all spelling the parameter `T` and all binding it differently
// (`long`, `string`, `int`), with an owned `Cell` on two of them. A per-level
// resolution has to keep three simultaneous meanings of one name; a flat map
// keeps exactly one. The `long` round-trip is checked separately so a truncating
// 8-into-4 store cannot hide inside the arithmetic.
// ===========================================================================
BEZEL_TEST(generic_ancestor_param_collision_2897, three_levels_one_name_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Root<T> "
        "  public rv: T; "
        "  public owned rc: Cell := new Cell(); "
        "  public ra: int := 41; "
        "end Root; "
        "class Mid<T> : Root<long> "
        "  public mv: T; "
        "  public owned mc: Cell := new Cell(); "
        "  public ma: int := 6; "
        "end Mid; "
        "class Leaf<T> : Mid<string> "
        "  public lv: T; "
        "  public la: int := 2; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var c := new Leaf<int>(); "
        "  c.rv := 100L; "
        "  c.mv := \"abcd\"; "
        "  c.lv := 9; "
        "  if c.rv <> 100L then return 91; end; "
        "  return c.ra + c.rc.tag + c.ma + c.mc.tag + c.la + c.mv.Length + c.lv; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 6 + 3 + 2 + 4 + 9
    BEZEL_EXPECT_EQ(r.exitCode, 68);
}

// ===========================================================================
// §28.4/§28.5 EXACTLY-ONCE DISPOSAL over the colliding shape, repeated so a
// leak or a double free shows up. `Cell.live` at the end is the oracle;
// `Cell.made` pins that both owned cells were actually built each round.
// ===========================================================================
BEZEL_TEST(generic_ancestor_param_collision_2897, exactly_once_disposal_loop_spec_28_5) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Coll<T> : Base<string> "
        "  public dv: T; "
        "  public owned own: Cell := new Cell(); "
        "  public da: int := 6; "
        "end Coll; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..12 do "
        "    using var c := new Coll<int>(); "
        "    c.bv := \"abcd\"; "
        "    c.dv := 9; "
        "    acc := acc + c.ba + c.cell.tag + c.own.tag + c.da "
        "         + c.bv.Length + c.dv; "
        "  end; "
        "  return (acc - 792) + Cell.live * 100 + (Cell.made - 24); "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // per iteration 41 + 3 + 3 + 6 + 4 + 9 = 66; 12 iterations = 792;
    // 24 cells made, 0 live.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT-CONTROLS. These pass with the fix patched out as well; they are here to
// prove the fix does not narrow or disturb what already worked.
// ===========================================================================

// The issue's own documented workaround — RENAME the derived parameter — plus
// `class D<T> : B<T>` (same spelling, SAME argument, no collision). Both are
// shapes #2889 made correct and this must leave alone.
BEZEL_TEST(generic_ancestor_param_collision_2897, control_renamed_and_same_argument) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Renamed<U> : Base<string> "
        "  public dv: U; public da: int := 6; "
        "end Renamed; "
        "class Same<T> : Base<T> public own: int := 2; end Same; "
        "class C public static method Main(): int "
        "  var n := 0; "
        "  do "
        "    using var r2 := new Renamed<int>(); "
        "    r2.bv := \"abcd\"; r2.dv := 9; "
        "    using var s := new Same<string>(); "
        "    s.bv := \"xy\"; "
        "    n := r2.ba + r2.cell.tag + r2.da + r2.bv.Length + r2.dv "
        "       + s.ba + s.cell.tag + s.own + s.bv.Length; "
        "  end; "
        "  return n - 111; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 41+3+6+4+9 = 63 and 41+3+2+2 = 48 => 111
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
