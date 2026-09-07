// Ownership — `MethodOwnership::recordMovesIn` had no arm for several
// structural wrapper expression kinds (`MemberAccessExpr`, `IndexExpr`,
// `PostfixBangExpr`, `NullCondMemberExpr`, `NullCondCallExpr`, and — as a
// review of the first landing found, `TypeTestExpr`/`InterpolatedStringExpr`
// — see #4028 below), so a `move` nested one level down — inside a
// `new T(...)` / call the wrapper's target/callee/operand immediately
// reaches — was never validated or marked: the dynamic_cast chain fell off
// the end doing nothing, and the wrapped owned-arg-consuming construct one
// level down was never visited. Issue #4003 (`MemberAccessExpr`, the
// reported case); the same audit performed at that first landing (per
// CLAUDE.md's "survey sibling arms" discipline) found the identical gap,
// independently measured, in `IndexExpr`, `PostfixBangExpr`,
// `NullCondMemberExpr`, and `NullCondCallExpr` too. Issue #4028 (filed by
// review of that first landing) found the SAME gap, independently measured,
// in `TypeTestExpr` and `InterpolatedStringExpr` — seven confirmed SEGV
// repros, one root cause, one fix pattern.
//
// Spec, v0.294:
//   §28.3 "Local variables, ownership, and borrows" — the move/disposal
//     state machine `markMovedByName` enforces ("a `Disposed` local may not
//     be moved from"; K1's "moved-from ... unreadable until reassigned").
//   §28.2 "The `IDisposable` contract" — explicit-disposal tracking that
//     feeds the same state machine.
//
// THE BUG (measured pre-fix on HEAD 8cb70ed, origin/main):
//
//   using var h := new Holder(move x);   // x already .Dispose()d
//     -> REJECTED: "cannot move from disposed local `x` (§28.3)".
//   var y := (new Holder(move x)).v;     // byte-identical move, wrapped in
//                                        // a MemberAccessExpr
//     -> ACCEPTED, ZERO diagnostics. Compiled and run under
//        `--sanitize=address`: SEGV (null READ) inside `bz$M.Holder$ctor`
//        reading the disposed `x`'s freed storage (`r.tag` in the
//        constructor body).
//
//   The identical shape reproduces, independently measured, through:
//     - IndexExpr:              (new Container(move x))[0]
//     - PostfixBangExpr:        (new Holder(move x))!.v
//     - NullCondMemberExpr:     (Factory.Make(move x))?.v
//     - NullCondCallExpr:       f?(move x)  (f: a nullable owned-param
//                                            callable)
//     - TypeTestExpr (#4028):   (new Holder(move x)) is Object
//     - InterpolatedStringExpr
//       (#4028):                $"result={new Holder(move x)}"
//   each SEGV'd identically pre-fix and is rejected with the precise §28.3
//   diagnostic post-fix.
//
// ROOT CAUSE: `MethodOwnership::recordMovesIn` (Ownership_Checks.cpp) is the
// ONE authority that walks a LocalDecl-initializer / assignment-RHS
// expression looking for a `move` to validate (via `markMovedByName`) and
// mark MovedFrom. Its dynamic_cast chain had arms only for `UnaryExpr`,
// `BinaryExpr`, `CallExpr`, and `NewExpr` — not for the seven wrapper kinds
// above, each of which simply carries the real owned-arg-consuming
// construct one level down (`ma->target`, `ix->target`, `pb->operand`,
// `nm->target`, `nc->callee`/`nc->args`, `tt->operand`,
// `isx->parts[i].expr`). With no arm, the chain silently falls through and
// NEVER recurses into that substructure, so the `move` buried inside is
// invisible to this pass — exactly the family `checkRead` (the read-side
// twin) already guards against, via its own arms for THREE of these seven
// node kinds (`MemberAccessExpr`, `PostfixBangExpr`, `IndexExpr`).
// `checkRead` has no arm for `NullCondMemberExpr`, `NullCondCallExpr`,
// `TypeTestExpr`, or `InterpolatedStringExpr` either — a distinct,
// unaudited gap on the READ side, not fixed here (see the exhaustive
// per-node-kind audit below).
//
// THE FIX: `recordMovesIn` gained seven arms total (five at #4003's first
// landing, two more for #4028), each simply recursing into the
// substructure — mirroring `checkRead`'s existing `MemberAccessExpr` /
// `PostfixBangExpr` / `IndexExpr` arms for those three, and the existing
// `CallExpr` arm's `recordMovesInArgs` dispatch for `NullCondCallExpr`;
// `TypeTestExpr` and `InterpolatedStringExpr` have no checkRead analogue to
// mirror (see above), so those two arms are original to this fix.
//
// EXHAUSTIVE AUDIT (re-derived per review request, against the FULL
// `ast::Expr` subtype list in src/ast/Node.hpp — 32 struct definitions,
// enumerated by `grep -n "^struct .*Expr\s*:\s*Expr" src/ast/Node.hpp`):
//
//   HANDLED by a pre-existing arm (unchanged by this fix):
//     UnaryExpr, BinaryExpr, CallExpr, NewExpr.
//   HANDLED by an arm THIS fix (across both landings) added:
//     MemberAccessExpr, IndexExpr, PostfixBangExpr, NullCondMemberExpr,
//     NullCondCallExpr, TypeTestExpr, InterpolatedStringExpr.
//   LEAF nodes — no `Expr` sub-child to recurse into, so no arm is needed
//   and the fall-through no-op is correct:
//     IntLiteralExpr, FloatLiteralExpr, CharLiteralExpr, StringLiteralExpr,
//     VerbatimStringExpr, BoolLiteralExpr, NullLiteralExpr, UnitExpr.
//     TypeOfExpr, NameOfExpr, DefaultExpr also have no `Expr` sub-child —
//     each carries only a `Type`/`QualifiedName`, never an `Expr` — so these
//     are LEAF-equivalent for this walk despite not being literal atoms.
//   IdentifierExpr — deliberately a no-op: a bare identifier is never
//     ITSELF a move (only a `move`-prefixed `UnaryExpr` is), and this is
//     explicitly documented at the existing NewExpr owned-sink loop's
//     comment ("recordMovesIn has no IdentifierExpr arm" — an out-owned
//     recipient identifier correctly falls through as a no-op there).
//   RangeExpr — STRUCTURALLY EXCLUDED: `lo`/`hi`/`step` are `Expr`
//     children, but §16.3 "The `Range<T>` type" (v0.294) restricts `T` to
//     the nine integer types and `char` — never an owned class type — so a
//     `move`-consuming construct can never legally appear as a range bound;
//     sema rejects the type mismatch before `recordMovesIn` ever runs.
//   WithExpr — STRUCTURALLY EXCLUDED: §9.5 "Non-destructive update: `with`"
//     (v0.294) requires a record-typed source ("A `with` expression whose
//     source is class-typed is a compile error"), and §9.7 "Records and
//     disposal" (v0.294) states plainly "Records cannot have ... `owned`
//     fields" — so no `with`-expression source or value slot can ever be an
//     owned-consuming sink for a `move` to reach.
//   LambdaExpr — STRUCTURALLY EXCLUDED BY DESIGN, not merely untested: the
//     body is a SEPARATE scope, analyzed by its own capture/ownership
//     handling when the lambda is invoked-context-checked, not by the
//     enclosing scope's `recordMovesIn` walk at lambda-CREATION time.
//     Recursing into it here would be actively WRONG — it would mark a
//     captured local MovedFrom immediately, before the lambda ever runs (or
//     on every re-invocation), which is unsound regardless of whether the
//     lambda is ever called.
//   ConditionalExpr, CaseExpr, ThrowExpr — KNOWN GAP, in flight on sibling
//     issue #3989/#3990 as of this writing; not duplicated here.
//   CollectionLiteralExpr — KNOWN GAP, filed as #4024 (same defect family,
//     but with ~18 call sites of pre-existing, unaudited adjacent
//     `CollectionLiteralExpr` machinery this fix declined to touch without
//     first confirming no double-report/conflict).
//   MarkedTransferExpr — STRUCTURALLY EXCLUDED: per its own declaration
//     comment (Node.hpp), this node is admitted ONLY inside a marked
//     if/case arm result — "the binding/assignment RHS forms remain
//     site-parsed flags ... so this node never appears there" — so it is
//     never itself a `recordMovesIn`-reachable top-level target; it is
//     reachable only through the ConditionalExpr/CaseExpr marked-arm
//     machinery above (in flight on #3989/#3990, not this fix).
//   BindExpr — VERIFIED ALREADY CORRECT, no arm needed: measured
//     `var y := (var unused := 0; new Holder(move x));` with `x` already
//     `Dispose()`d and it is correctly REJECTED with the precise §28.3
//     diagnostic even with no `BindExpr` arm in `recordMovesIn`. Mechanism:
//     `checkRead`'s FIRST arm (this file's twin function) dispatches every
//     `BindExpr` to `processBindAtom`, which runs `handleDischarge(*bx.body)`
//     on the atom's body (Ownership_Checks.cpp, `processBindAtom`) — a
//     DIFFERENT, already-correct authority, reached before `recordMovesIn`
//     ever sees the top-level expression, not through this function's
//     dynamic_cast chain at all.
//
// NOT part of this fix (independently confirmed, filed separately):
//   - `ConditionalExpr`/`CaseExpr`/`ThrowExpr` arms — see audit above.
//   - `CollectionLiteralExpr` — see audit above; filed as #4024.
//   - A SEPARATE, pre-existing bug (`checkRead`'s `CallExpr` argument loop
//     poisoning a Disposed move-source's state before `recordMovesIn`'s
//     `markMovedByName` can report on it) masks the diagnostic for a
//     `move` argument reached via `checkRead`'s existing (correct)
//     recursion into `CallExpr` args — independent of the arms this fix
//     adds; filed as #4023. `Factory.Make(move x)` with `x` already
//     Disposed reproduces it with NO wrapper expression at all, so it is
//     not exercised by the tests below (which use a `NewExpr` — `checkRead`
//     has no `NewExpr` arm, so it never reaches the `move` and cannot
//     collide — or the callee-less `NullCondCallExpr`/`Factory.Make`-via-
//     `NullCondMemberExpr` shapes below, which also route around a
//     `checkRead`-reachable bare `CallExpr` argument list of a `move`).
//
// per §9.5, §9.7, §16.3, §28.2, §28.3, v0.294, #4003, #4028

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the strong AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

constexpr char kDisposedMoveError[] =
    "cannot move from disposed local `x` (§28.3)";

// Shared `Res`/`Holder` shapes: `Res` is a plain disposable class; `Holder`'s
// constructor consumes an `owned r: Res` and disposes it, mirroring the
// issue's own repro exactly.
constexpr char kResAndHolder[] =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Holder "
    "  public v: int; "
    "  public constructor(owned r: Res) this.v := r.tag; r.Dispose(); "
    "end constructor; "
    "  destructor end destructor; "
    "end Holder; ";

std::string program(const std::string& classesAndMain) {
    return kResAndHolder + classesAndMain;
}

}  // namespace

// =============================================================================
// CONTROL — the pre-existing direct spelling was already correctly rejected
// before this fix and must stay rejected with the IDENTICAL diagnostic text
// (proves the new arms route `move` validation through the SAME authority,
// not a second drifting copy).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           direct_new_ctor_move_of_disposed_local_rejected_28_3_control) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    using var h := new Holder(move x); "
        "    return h.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// =============================================================================
// NEGATIVE (issue #4003's own repro) — `MemberAccessExpr` wrapping a `new`
// whose owned argument is `move`d from an already-disposed local.
// FAILS WITHOUT THE FIX: compiles clean (hasErrors() is false), then SEGVs
// under ASan (measured; see file header).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           member_access_wrapping_new_ctor_move_of_disposed_local_rejected) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := (new Holder(move x)).v; "
        "    return y; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// The same case through the STRONG AOT oracle (real `bezelc` + link + exec):
// proves not merely that a diagnostic fired, but that NO EXECUTABLE was ever
// produced — the sibling fix #3989/#3990 held its own new arms to this same
// bar (`aot_..._rejected_no_program_28_3` rows).
BEZEL_TEST(
    record_moves_in_wrapper_arms_4003,
    aot_member_access_wrapping_new_ctor_move_of_disposed_local_rejected_no_program_28_3) {
    JitResult r = runAot(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := (new Holder(move x)).v; "
        "    return y; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// =============================================================================
// NEGATIVE — `IndexExpr` wrapping a `new` (via a user-declared indexer)
// whose owned argument is `move`d from an already-disposed local.
// FAILS WITHOUT THE FIX: compiles clean, then SEGVs under ASan (measured).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           index_expr_wrapping_new_ctor_move_of_disposed_local_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Container "
        "  private v: int; "
        "  public constructor(owned r: Res) this.v := r.tag; r.Dispose(); "
        "end constructor; "
        "  public property [i: int]: int => v; "
        "  destructor end destructor; "
        "end Container; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := (new Container(move x))[0]; "
        "    return y; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// =============================================================================
// NEGATIVE — `PostfixBangExpr` (`!`) wrapping a `new` whose owned argument is
// `move`d from an already-disposed local. Deliberately a `NewExpr` operand
// (not a `CallExpr`) so this test does NOT also exercise the separate,
// pre-existing `checkRead`/`CallExpr`-argument masking bug filed as #4023 —
// `checkRead` has no `NewExpr` arm, so it never reaches this `move` at all.
// FAILS WITHOUT THE FIX: compiles clean, then SEGVs under ASan (measured).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           postfix_bang_wrapping_new_ctor_move_of_disposed_local_rejected) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := (new Holder(move x))!.v; "
        "    return y; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// =============================================================================
// NEGATIVE — `NullCondMemberExpr` (`?.`) wrapping a static-method call whose
// owned argument is `move`d from an already-disposed local.
// FAILS WITHOUT THE FIX: compiles clean, then SEGVs under ASan (measured).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           null_cond_member_wrapping_factory_move_of_disposed_local_rejected) {
    auto r = parseAndCheckOwnership(program(
        "class Factory "
        "  public static method Make(owned r: Res): owned Holder? "
        "    return new Holder(move r); "
        "  end Make; "
        "end Factory; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := (Factory.Make(move x))?.v; "
        "    return y ?? -1; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// =============================================================================
// NEGATIVE — `NullCondCallExpr` (`f?(...)`, a null-conditional invocation of
// a nullable owned-parameter callable) whose argument is `move`d from an
// already-disposed local.
// FAILS WITHOUT THE FIX: compiles clean, then SEGVs under ASan (measured).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           null_cond_call_wrapping_lambda_move_of_disposed_local_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var f: ((owned Res) -> int)? := lambda(owned r) => "
        "        do var t := r.tag; r.Dispose(); return t; end; "
        "    var y := f?(move x); "
        "    return y ?? -1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// =============================================================================
// NEGATIVE (#4028) — `TypeTestExpr` (`is`) wrapping a `new` whose owned
// argument is `move`d from an already-disposed local.
// FAILS WITHOUT THE FIX: compiles clean, then SEGVs under ASan (measured).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           type_test_wrapping_new_ctor_move_of_disposed_local_rejected_4028) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := (new Holder(move x)) is Object; "
        "    return (if y then 1 else 0); "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

BEZEL_TEST(
    record_moves_in_wrapper_arms_4003,
    aot_type_test_wrapping_new_ctor_move_of_disposed_local_rejected_no_program_4028) {
    JitResult r = runAot(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := (new Holder(move x)) is Object; "
        "    return (if y then 1 else 0); "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// =============================================================================
// NEGATIVE (#4028) — `InterpolatedStringExpr` (`$"...{expr}..."`) wrapping a
// `new` whose owned argument is `move`d from an already-disposed local.
// FAILS WITHOUT THE FIX: compiles clean, then SEGVs under ASan (measured).
// =============================================================================
BEZEL_TEST(
    record_moves_in_wrapper_arms_4003,
    interpolated_string_wrapping_new_ctor_move_of_disposed_local_rejected_4028) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var s := $\"result={new Holder(move x)}\"; "
        "    return s.Length; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

BEZEL_TEST(
    record_moves_in_wrapper_arms_4003,
    aot_interpolated_string_wrapping_new_ctor_move_of_disposed_local_rejected_no_program_4028) {
    JitResult r = runAot(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var s := $\"result={new Holder(move x)}\"; "
        "    return s.Length; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// =============================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15) — the two new #4028 arms add
// REJECTIONS (a `move` operand inside `is`/an interpolation hole is now
// validated where it silently wasn't), so the FAIL-WITHOUT/PASS-WITH cycle
// above proves only that the negative rows move; it says nothing about
// programs that must KEEP COMPILING. These rows pin the adjacent legitimate
// shapes: a FRESH-PRODUCER operand (no move at all — must always be legal)
// and a LEGAL move (the source is Owned, not Disposed — must be accepted,
// not just any move-shaped operand rejected wholesale).
// =============================================================================
BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           accept_type_test_of_fresh_producer_compiles_clean_4028) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var y := (new Holder(new Res(1))) is Object; "
        "    return (if y then 1 else 0); "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           accept_type_test_of_legal_move_compiles_clean_4028) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); "
        "    var y := (new Holder(move x)) is Object; "
        "    return (if y then 1 else 0); "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           accept_interpolated_string_of_fresh_producer_compiles_clean_4028) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var s := $\"result={new Holder(new Res(3))}\"; "
        "    return s.Length; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(record_moves_in_wrapper_arms_4003,
           accept_interpolated_string_of_legal_move_compiles_clean_4028) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(4); "
        "    var s := $\"result={new Holder(move x)}\"; "
        "    return s.Length; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
