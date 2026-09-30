// Codegen — issue #2873 (§28.4 The `destructor` step 2 / §28.5 Owned fields
// clause 1 / §28.9 Runtime-managed types / §14.1 Generic declarations /
// §22.8 Field initializers step 2 / §12 Inheritance and Dispatch, v0.240).
//
// A generic class whose FIRST field is type-parameter-typed (`v: T`) had its
// PLACEHOLDER (T -> i32) layout leak into two per-instantiation code paths.
// Both are SINGLE-MODULE — no `import`, no module boundary.
//
// Governing spec sentences.
//
//   §28.4, the generated `Dispose()`, step 2: "Disposes all owned-bearing
//   fields in reverse declaration order."
//
//   §28.5, the `owned` modifier, clause 1: "**Auto-disposes** the field when
//   the containing object is disposed (after the destructor body runs, in
//   reverse declaration order; a nullable field is null-checked before
//   disposal, and a null field disposes nothing)."
//
//   §22.8, the full construction sequence, step 2: "Field initializers, in
//   declaration order".
//
// HALF (a) — the DISPOSER (LlvmBackend_Setup.cpp, `specializedVtableFor`).
// Vtable slot 2 was replaced by a per-instantiation disposer only when the
// class had an owned type-PARAMETER field (#816/#1240) or a user-written
// destructor (#1247). Neither holds for
//
//     class G<T>  v: T;  i: int := 41;  s: string := "hi";  end G;
//
// so `G<string>`'s slot 2 kept the SHARED `bz$G$DisposeCore`, emitted once
// against the template struct `{ptr, i8, i32, i32, ptr}` (`s` at byte 24)
// while the object is `{ptr, i8, ptr, i32, ptr}` (`s` at byte 32). The field
// INDEX is unchanged — only the widths ahead of it move — so the GEP is
// well-typed and silently wrong: `bz_string_release` was handed the `int` 41
// and SEGV'd at `using var` scope exit (measured under ASan on the base:
// "SEGV ... in bz_string_release <- bz$A.G4$DisposeCore").
//
// The identical off-by-one-slot read with an `owned` field in that position is
// SILENT: it lands on the (null) `v: T` slot, the null guard skips it, and the
// owned field is NEVER DISPOSED — a §28.4/§28.5 leak with no diagnostic at any
// severity. That is why every case below asserts a live-object COUNT (and runs
// under runAotChecked's LeakSanitizer), not merely "does not crash"; the count
// also catches the opposite failure, a disposer that now runs twice.
//
// The fix adds `|| layoutDiffers` — the same lowered-layout-identity question
// #1240 already asks for the ITABLE two lines below it, and #2019 asks for the
// BASE core in emitBaseDisposeChain, asked for the class's OWN disposer where
// nobody was asking it.
//
// HALF (b) — CONSTRUCTION (LlvmBackend_Setup.cpp, `emitImplicitBase`). Issue
// #2873's third repro (`class DS : GS<string>`) is NOT the disposer: it SEGVs
// during construction, before any output. A non-generic class deriving from a
// LOCAL no-explicit-ctor generic base ran that base's §22.8 step-2 field
// initializers through `emitOwnFieldInit(b, obj, classStruct(b))` with NO
// active substitution — the placeholder layout again — while `new DS()`
// allocated the instantiation's layout (correct since #617/#1101-B/#2840). The
// `owned cell` initializer therefore stored 8 bytes early, over `v`, and the
// read of `d.cell` (correct since #585/#2840) loaded the still-zero slot.
// This is the LOCAL twin of the imported branch #2860 fixed immediately above
// it, and it reuses that branch's `computeAncestorGenericSubst` — which returns
// false rather than installing a substitution it cannot resolve. Sweeping the
// SHAPE rather than the line turned up a second call site with the identical
// defect — the explicit-`base()` fall-through in `emitChainCall` — so both are
// routed through one new `emitLocalBaseFieldInit` helper, exactly the pair of
// sites #2452 needed for the imported side.
//
// MEASURED 4-cell matrix over the two halves (this file, run four times):
//
//   neither  : t1 t2 t3 t4 t8 t9 t12 t13 t14 red
//   (a) only : t8 t9 t14 red           (the construction half is untouched)
//   (b) only : t1 t2 t3 t4 t12 t13 red (the disposer half is untouched)
//   both     : all 14 green
//
// So — unlike #2840, where half (b) alone was strictly WORSE than the bug —
// the two halves here are independent: each closes its own repro and neither
// perturbs the other's. They are one commit because they are one issue.
//
// Per-case fail-without / pass-with:
//   t1  string field after `v: T`            SEGV in bz_string_release  -> 0
//   t2  owned field after `v: T`, x20        Cell.n == 20, no crash     -> 0
//   t3  owned + string + T, x20              red                        -> 0
//   t4  three instantiations in one program  red                        -> 0
//   t12 the shape #2860's control had to omit  SEGV                     -> 0
//   t13 disposed via an owner's field cascade  red                      -> 0
//   t8  `class DS : GS<string>`              SEGV in construction       -> 0
//   t9  `Leaf : Mid : GS<string>`            SEGV in construction       -> 0
//   t14 explicit `base()` into a no-ctor generic base  SEGV             -> 0
//   t5  T = int control                      0 BOTH ways (no perturbation)
//   t6  T-field LAST control                 0 BOTH ways
//   t7  non-generic accept-control           0 BOTH ways
//   t10 generic derived over generic base    0 BOTH ways (D<T> : B<T> keeps
//                                            today's behaviour — the receiver
//                                            arity check declines)
//   t11 user destructor, exactly-once        0 BOTH ways — an honest
//                                            NON-hinge: `findDestructor(ts)`
//                                            already forced a specialized
//                                            disposer (#1247), so this pins
//                                            that the new clause does not
//                                            double-dispose where the old one
//                                            already fired.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + generated-code ASan
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;  // a SEGV/crash makes r.ran false -> -999
}

// Live-object counter: ctor ++, dtor --. `Cell.n == 0` after the scope is
// exactly-once disposal; a positive residue is a MISSED disposal (the silent
// half of #2873), a negative one a double-dispose.
const char* kCell =
    "class Cell public static n: int := 0; public tag: int := 3; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// ===========================================================================
// HALF (a) — the disposer. Hinges first.
// ===========================================================================

// t1 — #2873 repro A, the shortest one: a `string` field whose offset the
// intervening `int` makes diverge between the placeholder and the
// instantiation. §28.9 makes the string field runtime-managed, so the
// generated disposer releases it — at the WRONG offset without the fix, where
// it finds the `int` 41 and SEGVs. Loop-amplified so a single lucky read
// cannot pass. §28.4 / §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t1_string_field_after_typeparam_releases_at_instantiation_offset_28_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class G<T> public v: T; public i: int := 41; public s: string := \"hi\"; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var g := new G<string>(); "
        "    if g.i <> 41 then bad := bad + 1; end if; "
        "    if g.s <> \"hi\" then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t2 — the SILENT half: an `owned` field after `v: T`. Without the fix the
// shared disposer reads the (null) `v` slot instead, the null guard skips it,
// and every one of the 20 Cells leaks — no crash, no diagnostic. The count is
// the only oracle that sees this. §28.4 step 2 / §28.5 clause 1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t2_owned_field_after_typeparam_disposed_exactly_once_28_5) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var g := new G<string>(); "
        "    if g.a <> 41 then bad := bad + 1; end if; "
        "    if g.cell.tag <> 3 then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 4; end if; "   // 4 == leaked
        "  return bad; "
        "end Main; end C;"), 0);
}

// t3 — both managed kinds behind the same `v: T`: an `owned` class field AND a
// §28.9 runtime-managed `string`. Exercises the two arms of the generated
// disposer at once. §28.4 step 2 / §28.9.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t3_owned_and_string_after_typeparam_both_reclaimed_28_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; public s: string := \"payload-text\"; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var g := new G<string>(); "
        "    if g.a <> 41 then bad := bad + 1; end if; "
        "    if g.s <> \"payload-text\" then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 4; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t4 — SEVERAL instantiations of the same generic in ONE program. The
// specialized disposer is cached per (class, spec-field-type vector), so this
// pins that `G<string>`, `G<long>` and `G<int>` each get their own — and that
// the `int` one (whose layout IS the placeholder) is not perturbed. §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t4_several_instantiations_each_dispose_at_own_layout_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; public s: string := \"x\"; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 10 do "
        "    using var g1 := new G<string>(); "
        "    using var g2 := new G<long>(); "
        "    using var g3 := new G<int>(); "
        "    if g1.a <> 41 then bad := bad + 1; end if; "
        "    if g2.a <> 41 then bad := bad + 2; end if; "
        "    if g3.a <> 41 then bad := bad + 4; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 8; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t11 — a user-written `destructor` under the same shifted layout: it must run
// EXACTLY ONCE per instance (20 constructions -> 20 destructor runs), and the
// owned field behind `v: T` must still be reclaimed. `findDestructor(ts)`
// already forced a specialized disposer here before the fix (#1247), so this
// passes BOTH ways — it is a control, not a hinge, and its job is to prove the
// new clause does not make a disposer that ALREADY fired fire twice. §28.4.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t11_user_destructor_runs_exactly_once_under_shifted_layout_28_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> public static d: int := 0; public v: T; "
        "  public owned cell: Cell := new Cell(); public a: int := 41; "
        "  public s: string := \"z\"; "
        "  destructor G<T>.d := G<T>.d + 1; end destructor; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var g := new G<string>(); "
        "    if g.a <> 41 then bad := bad + 1; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 2; end if; "
        "  if G<string>.d <> 20 then bad := bad + 4; end if; "  // exactly once
        "  return bad; "
        "end Main; end C;"), 0);
}

// t12 — the shape #2860's single-module parity control (its T6) deliberately
// could NOT carry: `{ v: T; i: int; n: int?; s: string }`. That lane dropped
// the `string` field from an otherwise-identical control because this defect
// made it SEGV, and filed the defect as #2873. Pinned here, string field
// restored, so the omission is closed rather than remembered. §28.9 / §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t12_the_shape_2860s_parity_control_had_to_omit_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class G<T> public v: T; public i: int := 41; public n: int? := 7; "
        "  public s: string := \"hi\"; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var g := new G<string>(); "
        "    if g.i <> 41 then bad := bad + 1; end if; "
        "    if g.n = null then bad := bad + 2; "
        "    else if g.n <> 7 then bad := bad + 4; end if; end if; "
        "    if g.s <> \"hi\" then bad := bad + 8; end if; "
        "    k := k + 1; "
        "  end while; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t13 — disposal reached through an OWNER's cascade rather than `using var`:
// the generic instance is an `owned` field of an outer class, so §28.4 step 2
// disposes it through vtable slot 2 from the outer object's generated
// `Dispose()`. Same slot, different call site — the fix must cover both.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t13_disposed_through_owning_field_cascade_28_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; public s: string := \"held\"; end G; "
        "class Outer public owned g: G<string> := new G<string>(); "
        "  public m: int := 5; end Outer; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var o := new Outer(); "
        "    if o.m <> 5 then bad := bad + 1; end if; "
        "    if o.g.a <> 41 then bad := bad + 2; end if; "
        "    if o.g.s <> \"held\" then bad := bad + 4; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 8; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// HALF (a) — controls that must NOT change (a false reject / a behaviour
// change here would be worse than the bug).
// ===========================================================================

// t5 — T = int: the instantiation's layout IS the placeholder, so the shared
// disposer was already correct and the fix's layout comparison leaves it in
// place. Passes BOTH ways — an honest non-hinge. §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t5_control_value_typeparam_matching_placeholder_unchanged_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var g := new G<int>(); "
        "    if g.a <> 41 then bad := bad + 1; end if; "
        "    if g.cell.tag <> 3 then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 4; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t6 — the type-parameter field declared LAST, so no managed field's offset
// depends on it. The ordering control for t2. Passes BOTH ways. §28.5.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t6_control_typeparam_field_last_unchanged_28_5) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> public owned cell: Cell := new Cell(); public a: int := 41; "
        "  public v: T; end G; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var g := new G<string>(); "
        "    if g.a <> 41 then bad := bad + 1; end if; "
        "    if g.cell.tag <> 3 then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 4; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t7 — a NON-generic class with the same field shape: outside the change
// entirely (no substitution is ever active for it). Passes BOTH ways. §28.4.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t7_control_nongeneric_class_unaffected_28_4) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class N public owned cell: Cell := new Cell(); public a: int := 41; "
        "  public s: string := \"hi\"; end N; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var n := new N(); "
        "    if n.a <> 41 then bad := bad + 1; end if; "
        "    if n.s <> \"hi\" then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 4; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// HALF (b) — construction through a LOCAL no-explicit-ctor generic base.
// ===========================================================================

// t8 — #2873 repro C: `class DS : GS<string>`. Without the fix the base's
// §22.8 step-2 initializers write at the placeholder offsets and the read
// SEGVs before any output. With it, every field reads back and every Cell is
// disposed exactly once. §22.8 step 2 / §12 / §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t8_nongeneric_derived_runs_base_initializers_at_instantiation_22_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class GS<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; end GS; "
        "class DS : GS<string> end DS; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var d := new DS(); "
        "    if d.a <> 41 then bad := bad + 1; end if; "
        "    if d.cell.tag <> 3 then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 4; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t9 — the same through an intermediate NON-generic link, `Leaf : Mid :
// GS<string>`, plus a field of Leaf's own after the inherited ones. This is
// the shape computeAncestorGenericSubst's chain walk exists for. §12 / §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t9_two_level_chain_through_nongeneric_link_12) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class GS<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; end GS; "
        "class Mid : GS<string> end Mid; "
        "class Leaf : Mid public own: int := 9; end Leaf; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var d := new Leaf(); "
        "    if d.a <> 41 then bad := bad + 1; end if; "
        "    if d.own <> 9 then bad := bad + 2; end if; "
        "    if d.cell.tag <> 3 then bad := bad + 4; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 8; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t14 — half (b)'s SECOND call site, found by sweeping the shape rather than
// the line: a derived class that HAS its own constructor and writes an
// explicit `base()`. That reaches the no-matching-ConstructorSymbol
// fall-through in emitChainCall, which ran the identical
// `emitOwnFieldInit(base, obj, classStruct(base))` at the placeholder layout.
// (#2452 needed both of these sites for the IMPORTED side for the same
// reason.) Without the fix: SEGV before any output. §22.8 step 1-2 / §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t14_explicit_base_call_into_no_ctor_generic_base_22_8) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class GS<T> public v: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; end GS; "
        "class DExpl : GS<string> public own: int; "
        "  public constructor() base(); own := 9; end constructor; end DExpl; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var d := new DExpl(); "
        "    if d.a <> 41 then bad := bad + 1; end if; "
        "    if d.own <> 9 then bad := bad + 2; end if; "
        "    if d.cell.tag <> 3 then bad := bad + 4; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 8; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}

// t10 — control for half (b): a GENERIC derived class over a generic base
// (`D<T> : B<T>`). The synthesized receiver carries no generic arguments while
// `D` declares one, so computeAncestorGenericSubst declines and the active
// substitution is left exactly as it was — today's behaviour, preserved.
// Passes BOTH ways. §14.1.
BEZEL_TEST(generic_typeparam_field_disposer_layout_2873,
           t10_control_generic_derived_over_generic_base_unchanged_14_1) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class B<T> public bv: T; public owned cell: Cell := new Cell(); "
        "  public a: int := 41; end B; "
        "class D<T> : B<T> public own: int := 9; end D; "
        "class C public static method Main(): int "
        "  var bad := 0; var k := 0; "
        "  while k < 20 do "
        "    using var d := new D<int>(); "
        "    if d.a <> 41 then bad := bad + 1; end if; "
        "    if d.own <> 9 then bad := bad + 2; end if; "
        "    k := k + 1; "
        "  end while; "
        "  if Cell.n <> 0 then bad := bad + 4; end if; "
        "  return bad; "
        "end Main; end C;"), 0);
}
