// fix #2928 / §14.1 "Generic declarations" + §12 "Inheritance and Dispatch" +
// §12.1 "Members are non-virtual by default" + §13 "Interfaces" + §23
// "Properties and Indexers" + §22.8 "Field initializers" + §2.8.3 rule 4 (one
// runtime type identity per monomorphization), v0.240.
//
// #2906 put the per-instantiation body in the per-instantiation vtable slot for
// a virtual METHOD. This is the SAME DEFECT, ONE MEMBER KIND OVER: a virtual
// PROPERTY or INDEXER accessor slot carries no `sema::MethodSymbol`, so #2906's
// loop skipped it and the slot kept the accessor `declareVtables` emitted
// against the erased `T -> i32` TEMPLATE layout. Measured on the issue's repro
// (`class D2<U> : GBase<string>` read through a `GBase<string>`-typed variable):
//
//     AddressSanitizer: SEGV on unknown address 0x0c ... in bz$D2$Score$propget
//
// — `bz$D2$Score$propget` being the template accessor, on legal single-module
// code with no diagnostic at any severity.
//
// THE SETTER FACE IS WORSE THAN THE FAULT and is pinned here separately (T2):
// a wrong-offset WRITE lands on the neighbouring slot. Measured pre-fix, the
// write clobbered the inherited `owned cell` POINTER, and the program then
// SEGV'd inside `bz$GBase$DisposeCore$baseSpec0` — nowhere near the accessor,
// with the accessor itself apparently "working".
//
// THE INTERFACE-TYPED FACE (T4/T5) is a second slot table: `specializedVtableFor`
// rebuilds the itable for a layout-moving instantiation but its loop rewrote
// only METHOD indices, so the slot-by-slot copy that precedes it carried the
// template ACCESSOR forward verbatim — `SEGV ... in bz$GBase$Score$propget`
// through an interface reference.
//
// THE FIX. The four accessor memo caches (`propAccessorFns_`,
// `indexerGetterFns_`, `indexerSetterFns_`, `propBlockGetterFns_`) were keyed by
// the accessor's DECLARATION alone, so the first emission won and every later
// asker got that body — including a per-instantiation slot whose objects do not
// have that layout. The missing key dimension is the SUBSTITUTION the body was
// emitted under; #2928 adds it as `accessorSpecTag_` (0 = the shared/erased
// template, so every pre-existing caller's key space is unchanged), and
// `specializedVtableFor` installs a tag-specific accessor in the vtable slot
// (condition (d)) and in the itable accessor slots. The gate is the SAME
// PROPERTY #2906 uses — `instanceLayoutDiffersFromErasedTemplate(impl->parent)`,
// asked about the class that DECLARES the accessor, since that is the class
// whose `classStruct` the accessor body GEPs — not an enumeration of accessor
// shapes.
//
// DELIBERATELY REFUSED, and measured rather than assumed: a slot whose
// specialized accessor SIGNATURE moves under the substitution. The slot's ABI is
// fixed by the receiver's static type at every access site, so a mistyped
// indirect call would be worse than the status quo; such a slot keeps today's
// content. (`virtual property Item: T` overridden as `Item: string` does not
// reach that guard at all — it is a PRE-EXISTING hard IR-verification failure on
// this commit's parent as well as with the fix; filed separately, not claimed
// here.)
//
// NOT CLOSED BY THIS FIX, measured: #2894 (a virtual/interface property on an
// IMPORTED GENERIC class) still exits 139 with this fix applied. It is a
// different cause — `buildImportedGenericVtable` builds its itmap as a literal
// one-element `[null]` and leaves accessor slots null, so the failure is a call
// through an EMPTY slot, not a wrong body in a filled one.
//
// Every case runs end to end under generated-code AddressSanitizer /
// LeakSanitizer and asserts the VALUE. A fix that only makes the SEGV go away is
// not evidence the value is right — #2896 measured 44 where 149 is correct.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. Each row in this file used to guard on a
// local `wasSkipped` predicate whose whole body read the literal "SKIP" out
// of the result's diagnostics and returned from the row on it. That is an
// ABSORBING site by scripts/check_toolchain_skip_surface.py's own
// definition, and it is a silent vacuous pass: on a box whose clang cannot
// link the ASan binary, all 11 rows returned WITHOUT asserting and the suite
// reported a clean PASS having compiled and run nothing. MEASURED on the
// pre-change tree with the harness's own knob
// (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): 11 passed, 0 failed, and
// `CHECKED TOOLCHAIN: 11 skip(s)`.
//
// Each guard became the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r))`
// (tests/support/aot_result_asserts.hpp): the skip is now LOUD — the row
// FAILS — instead of being absorbed by a guard that reports a clean pass. That is strictly stronger than the guard it replaced — the row can no
// longer report green on a machine that never compiled the program — and
// every substantive assertion in every row (r.ran and the value pins) stays
// exactly where it was.

// ⚠ MAINTENANCE NOTE — THE PREMISE OF EVERY CASE BELOW, AND HOW IT EXPIRES.
//
// These programs are only about anything while the instantiation's instance
// layout genuinely MOVES relative to the erased `T -> i32` template. That is why
// `GBase<T>` declares TWO type-parameter-typed fields before the concrete ones:
// with a SINGLE `v: T`, the i32 placeholder and the `ptr` occupy the same
// 8-byte-aligned slot, so `cell` and `a` land at IDENTICAL offsets under both
// layouts and every case here silently becomes vacuous — it would pass with and
// without the fix. #2906 hit exactly that trap with #2907's filed repro
// (measured: the right answer, 44, on the unfixed parent).
//
// So: do NOT narrow `GBase` back to one `T` field, and do not reorder `v`/`v2`
// after `cell`. If a future change makes the two layouts agree again, these
// tests must FAIL LOUDLY rather than quietly stop testing anything — which is
// what T0 is for: it asserts the layouts differ by asserting a value that is
// only reachable through the composed (moved) offsets, and it is the first case
// to break if the premise is lost.
const char* kGBase =
    "class Cell public tag: int := 3; end Cell; "
    "class GBase<T> "
    "  public v: T; "
    "  public v2: T; "
    "  public owned cell: Cell := new Cell(); "
    "  public a: int := 41; "
    "  public virtual property Score: int "
    "    get => a + cell.tag; "
    "  end; "
    "end GBase; ";

}  // namespace

// ===========================================================================
// T0 — THE PREMISE CANARY. The inherited `a` and `cell` must be readable at the
// COMPOSED offsets through an ordinary (non-virtual, non-dispatched) read on the
// concrete type. If this ever returns something other than 44, the layouts have
// stopped moving (or field access itself regressed) and every case below has
// become vacuous — see the maintenance note above. §22.8 / §14.1.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, layout_premise_canary_spec_22_8) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  return g.a + g.cell.tag; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3 — through the composed layout, where `v`/`v2` are ptr-wide.
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO. §14.1 / §23 / §12.1. `class D2<U> : GBase<string>`
// dispatched through the base type: `U` is not pinned by the base clause, so
// #2896's call-site inversion refuses and control reaches the vtable slot.
// Pre-fix: SEGV in `bz$D2$Score$propget`. Post-fix: 149.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, virtual_property_getter_via_base_spec_23) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override property Score: int "
        "    get => 100 + a + own + cell.tag; "
        "  end; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<string> := d; "
        "  return g.Score; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}

// ===========================================================================
// T2 — THE SETTER FACE: a wrong-offset WRITE, which is the silent-corruption
// half of this defect. Every field the setter writes is read back, AND so is the
// inherited `owned cell`'s tag — the neighbouring slot the pre-fix store landed
// on (pre-fix the program SEGV'd inside the DISPOSER, not the accessor).
// §23.3 / §22.8 / §28.5.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, virtual_property_setter_writes_spec_23_3) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public v2: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual property Score: int "
        "    get => a; "
        "    set => do a := value; end; "
        "  end; "
        "end GBase; "
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override property Score: int "
        "    get => a + own; "
        "    set => do a := value + 1; own := value + 2; end; "
        "  end; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<string> := d; "
        "  g.Score := 10; "
        "  return g.Score + d.a + d.own + d.cell.tag; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // a := 11, own := 12 -> get = 23; 23 + 11 + 12 + 3 (cell UNCLOBBERED)
    BEZEL_EXPECT_EQ(r.exitCode, 49);
}

// ===========================================================================
// T3 — THE INDEXER FACE (§23.4). An indexer accessor is a different builder
// (`ensureIndexer{Getter,Setter}Fn`, and for a block body `ensureIndexerBlock-
// Getter`, which memoizes in a THIRD cache) — so a fix that keyed only the
// property cache would leave this crashing. Pre-fix: SEGV in `bz$indexer$get`.
//
// The sibling `D3` is LOAD-BEARING for the same reason it is in #2906's T3: it
// makes the dispatch reach the vtable instead of being devirtualized.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, virtual_indexer_getter_via_base_spec_23_4) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> "
        "  public v: T; "
        "  public v2: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public virtual property [i: int]: int "
        "    get => a + i; "
        "  end; "
        "end GBase; "
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override property [i: int]: int "
        "    get => 100 + a + own + cell.tag + i; "
        "  end; "
        "end D2; "
        "class D3<U> : GBase<string> "
        "  public dv3: U; "
        "  public override property [i: int]: int "
        "    get => 200 + a + i; "
        "  end; "
        "end D3; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<string> := d; "
        "  return g[2]; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3 + 2
    BEZEL_EXPECT_EQ(r.exitCode, 151);
}

// ===========================================================================
// T4 — THE INTERFACE-TYPED GETTER (§13 / §11.2). A different slot TABLE: the
// itable rebuilt by `specializedVtableFor` for a layout-moving instantiation.
// Its method loop rewrote only method indices, so the copied accessor slot still
// held the template accessor. Pre-fix: SEGV in `bz$GBase$Score$propget`.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, interface_property_getter_spec_13) {
    auto r = runAotChecked(
        "module H; "
        "interface IScore property Score: int get; end; end IScore; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> : IScore "
        "  public v: T; "
        "  public v2: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public property Score: int "
        "    get => a + cell.tag; "
        "  end; "
        "end GBase; "
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  var i: IScore := g; "
        "  return i.Score; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T5 — THE INTERFACE-TYPED SETTER, with the two NEIGHBOURING managed fields read
// back: the inherited `string note` (a refcounted §4.6 field the wrong-offset
// store would have overwritten with an int) and the `owned cell`'s tag. This is
// the case that proves the fix put the write at the right offset rather than
// merely stopping a fault. §13 / §23.3 / §28.9.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, interface_property_setter_spec_23_3) {
    auto r = runAotChecked(
        "module H; "
        "interface IScore property Score: int get; set; end; end IScore; "
        "class Cell public tag: int := 3; end Cell; "
        "class GBase<T> : IScore "
        "  public v: T; "
        "  public v2: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public a: int := 41; "
        "  public note: string := \"keep\"; "
        "  public property Score: int "
        "    get => a + cell.tag; "
        "    set => do a := value + 1; end; "
        "  end; "
        "end GBase; "
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  var i: IScore := g; "
        "  i.Score := 10; "
        "  Console.WriteLine(\"note=\" + g.note); "
        "  Console.WriteLine(\"tag=\" + g.cell.tag.ToString()); "
        "  return i.Score; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // a := 11, + cell.tag 3
    BEZEL_EXPECT_EQ(r.exitCode, 14);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("note=keep") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("tag=3") != std::string::npos);
}

// ===========================================================================
// T6 — THE INHERITED, NOT-OVERRIDDEN SLOT. The slot holds the BASE's accessor,
// whose body GEPs `classStruct(GBase)` — which still moves under `D2<int>`'s
// composition. So the gate must ask the layout question about the class that
// DECLARES the accessor, not about the receiver's own class. §12 / §22.8.
//
// ⚠ THE SIBLING `D3` IS LOAD-BEARING, not decoration — this is #2906's own
// measured trap, and it applies verbatim to an accessor. With a SINGLE
// non-overriding subclass the access is devirtualized to a direct call and the
// vtable is never consulted, so the case passes with AND without the fix. The
// sibling override makes the dispatch reach the slot. Do not delete it.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, inherited_not_overridden_accessor_spec_12) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "end D2; "
        "class D3<U> : GBase<string> "
        "  public dv3: U; "
        "  public override property Score: int get => 900; end; "
        "end D3; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<string> := d; "
        "  return g.Score; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T7 — TWO INSTANTIATIONS of one generic subclass, both reached through the SAME
// generic-base static type. The discriminator against a conflating cache key: a
// tag shared by `D2<int>` and `D2<double>` would run one instantiation's
// accessor on the other's object. §2.8.3 rule 4. (The sum is printed rather than
// returned: 298 does not survive an 8-bit process exit status.)
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, two_instantiations_via_one_base_spec_2_8_3) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override property Score: int "
        "    get => 100 + a + own + cell.tag; "
        "  end; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var di := new D2<int>(); "
        "  using var dd := new D2<double>(); "
        "  var g1: GBase<string> := di; "
        "  var g2: GBase<string> := dd; "
        "  Console.WriteLine(\"sum=\" + (g1.Score + g2.Score).ToString()); "
        "  return g1.Score - g2.Score + 7; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("sum=298") != std::string::npos);
}

// ===========================================================================
// T8 — RE-ENTRANCY. The accessor body constructs ANOTHER instance of its own
// instantiation, so `specializedVtableFor` is re-entered for the SAME
// instantiation while this accessor is mid-emission. #2906 made the specialized
// vtable global be created and REGISTERED before any body is emitted for exactly
// this reason; #2928's accessor emission is one more such body, so the property
// is re-pinned here for the accessor path. §14.1 / §2.8.3 rule 4 / §28.5.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, accessor_constructs_own_instantiation_spec_2_8_3) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public depth: int := 0; "
        "  public override property Score: int "
        "    get => do "
        "      if depth > 0 then return a + own; end if; "
        "      using var inner := new D2<int>(); "
        "      inner.depth := 1; "
        "      return 100 + a + own + cell.tag + inner.Score; "
        "    end; "
        "  end; "
        "end D2; "
        "class D3<U> : GBase<string> "
        "  public dv3: U; "
        "  public override property Score: int get => 900; end; "
        "end D3; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<string> := d; "
        "  return g.Score; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3 + (41 + 5)
    BEZEL_EXPECT_EQ(r.exitCode, 195);
}

// ===========================================================================
// T9 — ACCEPT CONTROL: `T = int`, where the instantiation layout is IDENTICAL to
// the erased template, so the gate is false and the slot keeps the shared
// accessor. Passes with AND without the fix, deliberately — it is the evidence
// that the gate is a layout PROPERTY and not "specialize every generic
// accessor". §14.1.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, int_instantiation_keeps_shared_accessor_spec_14_1) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<int> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override property Score: int "
        "    get => 100 + a + own + cell.tag; "
        "  end; "
        "end D2; "
        "class D3<U> : GBase<int> "
        "  public dv3: U; "
        "  public override property Score: int get => 900; end; "
        "end D3; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  var g: GBase<int> := d; "
        "  return g.Score; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 + 41 + 5 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 149);
}

// ===========================================================================
// T10 — a NON-GENERIC class carrying a virtual property, inherited by a generic
// subclass that does not move it. The declaring class has no type parameters at
// all, so `instanceLayoutDiffersFromErasedTemplate` is false by construction and
// the shared accessor is kept. Second accept control: it pins that a non-generic
// declarer is not swept into the specialization path. §12.1 / §14.1.
// ===========================================================================
BEZEL_TEST(generic_virtual_accessor_slot_2928, nongeneric_declarer_keeps_shared_accessor_spec_12_1) {
    auto r = runAotChecked(
        "module H; "
        "class Plain "
        "  public a: int := 41; "
        "  public virtual property Score: int get => a; end; "
        "end Plain; "
        "class GSub<U> : Plain "
        "  public gv: U; "
        "  public override property Score: int get => a + 3; end; "
        "end GSub; "
        "class C public static method Main(): int "
        "  using var d := new GSub<string>(); "
        "  var p: Plain := d; "
        "  return p.Score; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 41 + 3
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}
