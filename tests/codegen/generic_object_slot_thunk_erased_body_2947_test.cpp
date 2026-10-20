// fix #2947 / §8.5 "`ToString`" + §8.3 "`Equals` and `GetHashCode`" + §12
// "Inheritance and Dispatch" + §14.1 "Generic declarations" + §18.5
// "Interpolated strings" + §22.8 "Field initializers" + §2.8.3 rule 4 (one
// runtime type identity per monomorphization), v0.240.
//
// THE DEFECT. The FIXED vtable slots 3/4/5 — `ToString` / `Equals` /
// `GetHashCode` — were the last members of the placeholder-layout family still
// copied through verbatim by `specializedVtableFor`. #2906 closed the virtual
// METHOD slots; #2928 closed the virtual PROPERTY / INDEXER accessor slots and
// the itable's accessor slots. Each of these three slots holds a THUNK that
// calls `methodFns_[user]` — the shared, ERASED `T -> i32` template body —
// non-virtually, so a per-instantiation vtable whose objects carry the COMPOSED
// layout still GEP'd the placeholder offsets. All three arms were MEASURED on
// this fix's parent, none inferred:
//
//     SEGV on unknown address 0x0c ... in bz$H.GBase$ToString   <- bz$tostr$GBase
//     SEGV on unknown address 0x0c ... in bz$H3.GBase$Equals    <- bz$eq$GBase
//     SEGV on unknown address 0x0c ... in bz$H2.GBase$GetHashCode <- bz$hash$GBase
//
// on legal single-module code with no diagnostic at any severity. (The issue
// listed `Equals` as "not probed ... it shares the builder"; it is probed here,
// and it does fault.)
//
// WHY THIS PREFIX REACHES FURTHER than the rest of the family. §8.5: "`ToString`
// is invoked implicitly inside string interpolation (§18.5) via virtual
// dispatch", so an interpolation hole is a dispatch site with no explicit call
// in the source at all; §8.3's `Equals`/`GetHashCode` are what a hashed
// collection dispatches to on a key.
//
// ⚠ ONE CLAIM IN THE ISSUE IS REFUTED HERE, by measurement rather than
// argument: it says slot 3 is reached by "every string interpolation of a
// class-typed value". It is not — a CONCRETE-typed hole resolves the override
// statically and emits a direct call, so it never reads the vtable and was
// already correct on this fix's parent. T4b pins that as a control and T4 uses
// an `Object`-typed hole, which does dispatch. Written the issue's way, T4 would
// have passed with AND without the fix.
//
// THE FIX. The thunk is a layout-agnostic forwarder — what had to change is what
// it forwards TO. `specializedVtableFor` gains condition (e): for a slot that
// still holds the shared template thunk AND whose user override is declared by a
// class whose instance layout MOVES under this substitution
// (`instanceLayoutDiffersFromErasedTemplate(user->parent)` — #2019's predicate,
// the very one (c) and (d) use, asked about the class whose `classStruct` the
// forwarded-to body GEPs), a fresh `…$specN` thunk is emitted around
// `buildSpecializedMethodFn`'s re-lowered body. The class-chain walk and the
// thunk body emitter are each factored out and shared with the template path, so
// the question asked here cannot drift from the content actually in the slot and
// the Equals/GetHashCode result coercions cannot diverge between the two.
//
// A class with NO user override never reaches condition (e) at all: its thunk is
// the compiler-generated default — §8.5's "the type's qualified name" for
// ToString, and §8.3's table row for a Class, "Reference equality" /
// "Identity-based" — which reads no field and so has no layout to be wrong
// about. A slot whose specialized target's SIGNATURE moves under the
// substitution is REFUSED, keeping today's content rather than a mistyped call.
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
// (tests/support/aot_result_asserts.hpp): the skip is still recognised as an
// environment condition, but it now MARKS THE ROW FAILED instead of hiding
// it. That is strictly stronger than the guard it replaced — the row can no
// longer report green on a machine that never compiled the program — and
// every substantive assertion in every row (r.ran and the value pins) stays
// exactly where it was.

// ⚠ MAINTENANCE NOTE — THE PREMISE OF EVERY CASE BELOW, AND HOW IT EXPIRES.
//
// These programs are only about anything while the instantiation's instance
// layout genuinely MOVES relative to the erased `T -> i32` template. That is why
// every generic class here declares TWO type-parameter-typed fields before the
// concrete ones: with a SINGLE `v: T`, the i32 placeholder and the `ptr` occupy
// the same 8-byte-aligned slot, so `cell` and `a` land at IDENTICAL offsets
// under both layouts and every case silently becomes vacuous — it would pass
// with and without the fix. #2906 hit exactly that trap with #2907's filed
// repro (measured: the right answer, 44, on the unfixed parent), and #2928
// carries the same note for the same reason.
//
// So: do NOT narrow these classes back to one `T` field, and do not reorder
// `v`/`v2` after `cell`. If a future change makes the two layouts agree again,
// these tests must FAIL LOUDLY rather than quietly stop testing anything —
// which is what T0 is for: it asserts a value only reachable through the
// composed (moved) offsets, and is the first case to break if the premise is
// lost.
//
// A SECOND PREMISE, equally load-bearing and equally quiet if lost: the
// receiver at the dispatch site must be `Object`-typed (or an interpolation
// hole), because that is what forces the read of the fixed vtable slot. A
// concrete-typed receiver is a direct call and never consults the vtable — the
// devirtualization vacuity #2906's lane measured on a different shape. Do not
// "simplify" `var o: Object := g; o.ToString()` to `g.ToString()`.
const char* kGBaseToString =
    "class Cell public tag: int := 3; end Cell; "
    "class GBase<T> "
    "  public v: T; "
    "  public v2: T; "
    "  public owned cell: Cell := new Cell(); "
    "  public a: int := 41; "
    "  public override method ToString(): string "
    "    return \"a=\" + a.ToString() + \",tag=\" + cell.tag.ToString(); "
    "  end ToString; "
    "end GBase; ";

const char* kGBaseHash =
    "class Cell public tag: int := 3; end Cell; "
    "class GBase<T> "
    "  public v: T; "
    "  public v2: T; "
    "  public owned cell: Cell := new Cell(); "
    "  public a: int := 41; "
    "  public override method GetHashCode(): int "
    "    return a + cell.tag; "
    "  end GetHashCode; "
    "end GBase; ";

// §8.3: "A type that overrides `Equals` must also override `GetHashCode`. The
// compiler enforces this." — so the Equals shape necessarily carries both, and
// T3 therefore exercises slots 4 AND 5 of one vtable at once.
const char* kGBaseEquals =
    "class Cell public tag: int := 3; end Cell; "
    "class GBase<T> "
    "  public v: T; "
    "  public v2: T; "
    "  public owned cell: Cell := new Cell(); "
    "  public a: int := 41; "
    "  public override method Equals(other: Object?): bool "
    "    return a + cell.tag = 44; "
    "  end Equals; "
    "  public override method GetHashCode(): int "
    "    return a + cell.tag; "
    "  end GetHashCode; "
    "end GBase; ";

}  // namespace

// ===========================================================================
// T0 — THE PREMISE CANARY. The `a` and `cell` fields must be readable at the
// COMPOSED offsets through an ordinary (non-dispatched) read on the concrete
// type. If this ever returns something other than 44, the layouts have stopped
// moving (or field access itself regressed) and every case below has become
// vacuous — see the maintenance note above. §22.8 / §14.1.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, layout_premise_canary_spec_22_8) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBaseToString +
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
// T1 — THE ISSUE'S REPRO A: `ToString`, vtable slot 3, through an `Object`-typed
// receiver. Pre-fix: `SEGV @0x0c in bz$H.GBase$ToString` (the erased template)
// under `bz$tostr$GBase` (the slot-3 thunk).
//
// The assertion is on the STRING VALUE, not on "it did not crash": the body
// reads an `int` field AND dereferences an `owned` class-typed field, both at
// composed offsets, so a wrong-offset read that happened to land in mapped
// memory would produce a different string rather than a fault. §8.5 / §12.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, tostring_slot_object_receiver_spec_8_5) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBaseToString +
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  var o: Object := g; "
        "  var s: string := o.ToString(); "
        "  if s <> \"a=41,tag=3\" then return 91; end; "
        "  return 44; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T2 — THE ISSUE'S REPRO B: `GetHashCode`, vtable slot 5. Pre-fix:
// `SEGV @0x0c in bz$H2.GBase$GetHashCode` under `bz$hash$GBase`.
//
// §8.3's identity-hash default is NOT what is being tested — this class
// overrides `GetHashCode`, and the override must reach the composed layout.
// 44 = 41 + 3, the value only the composed offsets produce.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, gethashcode_slot_object_receiver_spec_8_3) {
    auto r = runAotChecked(
        std::string("module H2; ") + kGBaseHash +
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  var o: Object := g; "
        "  return o.GetHashCode(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T3 — `Equals`, vtable slot 4. THE ISSUE EXPLICITLY LABELLED THIS ARM AS
// INFERRED, NOT MEASURED ("it is listed here because it shares the builder, not
// because it was measured"). It is measured here, and it does fault pre-fix:
//
//     AddressSanitizer: SEGV on unknown address 0x0c ... in bz$H3.GBase$Equals
//     ... in bz$eq$GBase
//
// The `Equals` body returns a BOOL derived from the composed-offset fields, so
// the assertion distinguishes "reached the right body" from "returned false".
// §8.3 / §8.4.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, equals_slot_object_receiver_spec_8_3) {
    auto r = runAotChecked(
        std::string("module H3; ") + kGBaseEquals +
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  var o: Object := g; "
        "  if o.Equals(o) then return 44; end; "
        "  return 91; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T4 — THE INTERPOLATION CONSUMER. §8.5: "`ToString` is invoked implicitly
// inside string interpolation (§18.5) via virtual dispatch" — so an
// interpolation hole is a dispatch site in its own right, with no explicit
// `.ToString()` call anywhere in the program.
//
// ⚠ THE HOLE IS `Object`-TYPED, AND THAT IS LOAD-BEARING — MEASURED, NOT
// ARGUED. The issue asserts that slot 3 is reached by "every string
// interpolation of a class-typed value". It is not: with a CONCRETE-typed hole
// (`$"v={g}"` where `g: GBase<string>`) the interpolation lowering resolves the
// override statically and emits a DIRECT call, so the vtable is never consulted
// and the program was already correct on this fix's parent — the devirtualization
// vacuity #2906's lane hit on a different shape, reproduced here and caught by
// running this file against the reverted tree. Written that way, this case would
// pass with AND without the fix while appearing to pin the interpolation path.
// Do not "simplify" `var o: Object := g; $"v={o}"` back to `$"v={g}"`.
// §8.5 / §18.5 / §12.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, tostring_slot_interpolation_spec_18_5) {
    auto r = runAotChecked(
        std::string("module H4; ") + kGBaseToString +
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  var o: Object := g; "
        "  var s: string := $\"v={o}\"; "
        "  if s <> \"v=a=41,tag=3\" then return 91; end; "
        "  return 44; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T4b — CONTROL, and the record of T4's measurement. A CONCRETE-typed
// interpolation hole is a static (direct) call to the override, not a slot-3
// dispatch, so it was correct on this fix's parent and must stay correct. It is
// declared a control here rather than deleted, because "interpolation of a
// class-typed value reaches slot 3" is what the issue states and what a future
// reader would otherwise re-derive; this case is the standing counter-evidence.
// Passes with and without the fix. §18.5 / §8.5.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, concrete_typed_interpolation_hole_is_direct_spec_18_5) {
    auto r = runAotChecked(
        std::string("module H4b; ") + kGBaseToString +
        "class C public static method Main(): int "
        "  using var g := new GBase<string>(); "
        "  var s: string := $\"v={g}\"; "
        "  if s <> \"v=a=41,tag=3\" then return 91; end; "
        "  return 44; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T5 — TWO INSTANTIATIONS LIVE AT ONCE through one class, both dispatched
// through slot 3. §2.8.3 rule 4 gives each monomorphization its own runtime
// identity, so each must carry its OWN vtable and its own slot-3 thunk: a
// specialization cached across instantiations would hand one instantiation's
// thunk to the other.
//
// `GBase<int>` is layout-IDENTICAL to the erased template and `GBase<string>`
// is not, so this also pins that the two coexist — the fix specializes exactly
// one of them and must leave the other on the shared thunk.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, two_instantiations_each_get_a_thunk_spec_2_8_3) {
    auto r = runAotChecked(
        std::string("module H5; ") + kGBaseToString +
        "class C public static method Main(): int "
        "  using var gs := new GBase<string>(); "
        "  using var gi := new GBase<int>(); "
        "  var os: Object := gs; "
        "  var oi: Object := gi; "
        "  if os.ToString() <> \"a=41,tag=3\" then return 91; end; "
        "  if oi.ToString() <> \"a=41,tag=3\" then return 92; end; "
        "  return 44; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T6 — AN INHERITED, NOT-OVERRIDDEN slot. `D<U> : GBase<string>` declares no
// `ToString` of its own, so `bz$tostr$D` forwards to the body GBase declared —
// and the layout question must be asked about GBase (the DECLARING class, whose
// `classStruct` that body GEPs), not about D. This is #2906's own shape one
// member kind over, and the case that a "does this class override?" gate would
// silently miss. §12 / §12.1 / §14.1.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, inherited_not_overridden_slot_spec_12_1) {
    auto r = runAotChecked(
        std::string("module H6; ") + kGBaseToString +
        "class D<U>: GBase<string> "
        "  public d: int := 5; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D<int>(); "
        "  var o: Object := d; "
        "  var s: string := o.ToString(); "
        "  if s <> \"a=41,tag=3\" then return 91; end; "
        "  return 44; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T7 — CONTROL. `T = int` keeps the erased template layout, so the predicate is
// false and the instantiation must keep the SHARED thunk. Passes with and
// without the fix; it exists to prove the gate is a LAYOUT property and not
// "every generic class gets a new thunk". §14.1.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, layout_identical_instantiation_unchanged_spec_14_1) {
    auto r = runAotChecked(
        std::string("module H7; ") + kGBaseHash +
        "class C public static method Main(): int "
        "  using var g := new GBase<int>(); "
        "  var o: Object := g; "
        "  return o.GetHashCode(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T8 — CONTROL. A NON-GENERIC class with all three overrides. There is no
// substitution, so `specializedVtableFor` is never consulted and the three
// fixed slots keep the thunks `buildVtable` installed. Passes with and without
// the fix; it exists so that a regression in the REFACTOR (the class-chain
// walks and thunk-body emitters that (e) now shares with the template path)
// fails here rather than silently in the specialized path. §8.5 / §8.3.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, nongeneric_class_thunks_unchanged_spec_8_5) {
    auto r = runAotChecked(
        "module H8; "
        "class Plain "
        "  public a: int := 41; "
        "  public override method ToString(): string return \"plain\"; end ToString; "
        "  public override method Equals(other: Object?): bool return a = 41; end Equals; "
        "  public override method GetHashCode(): int return a + 3; end GetHashCode; "
        "end Plain; "
        "class C public static method Main(): int "
        "  using var p := new Plain(); "
        "  var o: Object := p; "
        "  if o.ToString() <> \"plain\" then return 91; end; "
        "  if not o.Equals(o) then return 92; end; "
        "  return o.GetHashCode(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

// ===========================================================================
// T9 — CONTROL, and the second half of the refactor's safety net: a generic
// class whose layout MOVES but which declares NO override of any of the three.
// §8.5's compiler-generated default for a class is "the type's qualified name",
// which reads no field, so condition (e) must not fire and the default must
// still be produced. Passes with and without the fix. §8.5.
// ===========================================================================
BEZEL_TEST(generic_object_slot_thunk_2947, no_override_keeps_default_thunk_spec_8_5) {
    auto r = runAotChecked(
        "module H9; "
        "class GPlain<T> "
        "  public v: T; "
        "  public v2: T; "
        "  public a: int := 41; "
        "end GPlain; "
        "class C public static method Main(): int "
        "  using var g := new GPlain<string>(); "
        "  var o: Object := g; "
        "  if o.ToString().Length = 0 then return 91; end; "
        "  if not o.Equals(o) then return 92; end; "
        "  return g.a + 3; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}
