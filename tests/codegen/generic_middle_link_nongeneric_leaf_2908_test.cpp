// fix #2908 / §14.1 "Generic declarations" + §12 "Inheritance and Dispatch" +
// §22.8 "Field initializers" (steps 1 and 2) + §28.5 "Owned fields", v0.240.
//
// A GENERIC MIDDLE LINK under a NON-GENERIC LEAF (`class Leaf : Mid<int>` over
// `class Mid<U> : Base<string>`) ran the base's own field initializers at the
// ERASED `T -> i32` layout inside an object allocated at the composed one:
//
//     %"class.Leaf$spec0" = type { ptr, i8, ptr, ptr, i32, i32, i32, i32 }
//     %class.Base        = type { ptr, i8, i32, ptr, i32 }      <- initialized through
//
// so `Base`'s `owned cell` and `ba` stores landed 8 bytes early, on top of the
// inherited `bv: string` slot. Releasing that string then freed a pointer that
// was never allocated: `SEGV ... WRITE ... in __asan::Allocator::Deallocate`
// under `bz_string_release`, on legal single-module code with no diagnostic at
// any severity. Verified independent of the #2897 parameter-name collision —
// the parameters below are deliberately spelled differently at every link.
//
// ROOT CAUSE. The §22.8 construction sequence recurses down the base chain and
// handed each level its IMMEDIATE derived class as the receiver of
// `computeAncestorGenericSubst`. That walk resolves a chain POSITIONALLY from
// base clauses and needs a seed only for the RECEIVER's own parameters — so from
// `Mid` it needs `U` bound in the ACTIVE substitution, which a `new Leaf()` in a
// non-generic caller does not bind. (#2889 composes the whole chain into the
// active substitution at the `new` site for exactly this reason, but that
// composition is gated on the NEW'd class being GENERIC, so a non-generic leaf
// composes nothing — the population the mechanism was built for never contained
// this shape.) From `Leaf` the same walk needs NO seed at all: a non-generic
// receiver is the 0 == 0 case, and it resolves `U -> int` then `T -> string`.
//
// THE FIX threads the class the construction sequence STARTED at through the
// implicit-base recursion (`emitImplicitBase`'s new `root`), and hands THAT —
// not the immediate derived link — to `emitLocalBaseFieldInit` (§22.8 step 2),
// `emitSpecializedImplicitBaseCtor` (§22.8 step 1) and `emitImportedBaseFieldInit`.
// At every entry point `root` is the class the caller already passed, so nothing
// changes there; only the recursion stops narrowing the receiver.
//
// Every test here runs end to end under generated-code AddressSanitizer /
// LeakSanitizer.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

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
// T1 — THE ISSUE'S OWN REPRO. §22.8 step 2. `Base`'s field initializers run
// from a NO-CONSTRUCTOR chain. Pre-fix: heap corruption in the string release.
// ===========================================================================
BEZEL_TEST(generic_middle_link_2908, generic_middle_nongeneric_leaf_spec_22_8) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Mid<U> : Base<string> "
        "  public mv: U; "
        "  public ma: int := 6; "
        "end Mid; "
        "class Leaf : Mid<int> "
        "  public la: int := 2; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var c := new Leaf(); "
        "  c.bv := \"abcd\"; "
        "  c.mv := 9; "
        "  return c.ba + c.cell.tag + c.ma + c.bv.Length + c.mv + c.la; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 6 + 4 + 9 + 2
    BEZEL_EXPECT_EQ(r.exitCode, 65);
}

// ===========================================================================
// T2 — the same chain where the generic BASE has an explicit parameterless
// CONSTRUCTOR. That routes §22.8 step 1 through `emitSpecializedImplicitBaseCtor`
// instead, whose obligation 2 synthesizes a receiver out of the same argument —
// so it needed the same root, and had the same erased-layout failure.
// ===========================================================================
BEZEL_TEST(generic_middle_link_2908, generic_base_with_ctor_under_nongeneric_leaf_spec_22_8) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell; "
        "  public ba: int; "
        "  public constructor() cell := new Cell(); ba := 41; end constructor; "
        "end Base; "
        "class Mid<U> : Base<string> "
        "  public mv: U; "
        "  public ma: int := 6; "
        "end Mid; "
        "class Leaf : Mid<int> "
        "  public la: int := 2; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var c := new Leaf(); "
        "  c.bv := \"abcd\"; "
        "  c.mv := 9; "
        "  return c.ba + c.cell.tag + c.ma + c.bv.Length + c.mv + c.la; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 6 + 4 + 9 + 2
    BEZEL_EXPECT_EQ(r.exitCode, 65);
}

// ===========================================================================
// T3 — TWO generic middle links under a non-generic leaf, each renamed and each
// carrying its own owned `Cell` and its own `T`-typed field. Every level's
// initializers have to land at the composed offsets, not just the deepest one.
// The `long` round trip is checked separately so a truncating 8-into-4 store
// cannot hide inside the arithmetic. §22.8 / §14.1.
// ===========================================================================
BEZEL_TEST(generic_middle_link_2908, two_generic_middle_links_renamed_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class L3<A> "
        "  public av: A; "
        "  public owned c3: Cell := new Cell(); "
        "  public n3: int := 7; "
        "end L3; "
        "class L2<B> : L3<long> "
        "  public bv: B; "
        "  public owned c2: Cell := new Cell(); "
        "  public n2: int := 11; "
        "end L2; "
        "class L1<D> : L2<string> "
        "  public dv: D; "
        "  public owned c1: Cell := new Cell(); "
        "  public n1: int := 13; "
        "end L1; "
        "class Leaf : L1<int> "
        "  public n0: int := 2; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var x := new Leaf(); "
        "  x.av := 4294967296L; "
        "  x.bv := \"abcd\"; "
        "  x.dv := 9; "
        "  var wide: int := 0; "
        "  if x.av = 4294967296L then wide := 1; end if; "
        "  return x.n0 + x.n1 + x.n2 + x.n3 + x.c1.tag + x.c2.tag + x.c3.tag "
        "         + x.bv.Length + x.dv + wide; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 2 + 13 + 11 + 7 + 3 + 3 + 3 + 4 + 9 + 1
    BEZEL_EXPECT_EQ(r.exitCode, 56);
}

// ===========================================================================
// T4 — §28.4/§28.5 EXACTLY-ONCE DISPOSAL over 12 iterations. A base initializer
// that stores its `owned cell` at the wrong offset leaves the real slot null, so
// the disposer's null guard skips it and the object leaks with NO crash —
// "does not crash" would pass this without the counter.
// ===========================================================================
BEZEL_TEST(generic_middle_link_2908, exactly_once_disposal_loop_spec_28_4) {
    auto r = runAotChecked(
        std::string("module H; ") + kCountedCell +
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Mid<U> : Base<string> "
        "  public mv: U; "
        "  public ma: int := 6; "
        "end Mid; "
        "class Leaf : Mid<int> "
        "  public la: int := 2; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  for var i in 1..12 do "
        "    using var c := new Leaf(); "
        "    c.bv := \"abcd\"; "
        "    c.mv := 9; "
        "    acc := acc + c.ba + c.cell.tag + c.ma + c.bv.Length + c.mv + c.la; "
        "  end; "
        "  return (acc - 780) + Cell.live * 100 + (Cell.made - 12); "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 12 * 65 = 780; 12 cells made, 0 live at the end.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T5 — ACCEPT-CONTROL: the SAME chain with a GENERIC leaf. #2889 already
// composes the whole chain into the active substitution when the NEW'd class is
// generic, so this shape was correct before the fix and must stay correct after
// it. It passes with AND without the fix by construction. §14.1.
// ===========================================================================
BEZEL_TEST(generic_middle_link_2908, control_generic_leaf_over_generic_middle_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Mid<U> : Base<string> "
        "  public mv: U; "
        "  public ma: int := 6; "
        "end Mid; "
        "class Leaf<X> : Mid<int> "
        "  public lv: X; "
        "  public la: int := 2; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var c := new Leaf<string>(); "
        "  c.bv := \"abcd\"; "
        "  c.mv := 9; "
        "  c.lv := \"xyz\"; "
        "  return c.ba + c.cell.tag + c.ma + c.bv.Length + c.mv + c.la "
        "         + c.lv.Length; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 + 6 + 4 + 9 + 2 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 68);
}
