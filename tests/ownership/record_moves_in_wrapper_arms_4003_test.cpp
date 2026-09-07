// Ownership — `MethodOwnership::recordMovesIn` had no arm for several
// structural wrapper expression kinds (`MemberAccessExpr`, `IndexExpr`,
// `PostfixBangExpr`, `NullCondMemberExpr`, `NullCondCallExpr`), so a `move`
// nested one level down — inside a `new T(...)` / call the wrapper's
// target/callee immediately reaches — was never validated or marked: the
// dynamic_cast chain fell off the end doing nothing, and the wrapped
// owned-arg-consuming construct one level down was never visited. Issue
// #4003 (`MemberAccessExpr`, the reported case); the same audit this fix
// performed (per CLAUDE.md's "survey sibling arms" discipline) found the
// identical gap, independently measured, in `IndexExpr`, `PostfixBangExpr`,
// `NullCondMemberExpr`, and `NullCondCallExpr` too — one root cause, five
// confirmed SEGV repros.
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
//     - IndexExpr:          (new Container(move x))[0]
//     - PostfixBangExpr:    (new Holder(move x))!.v
//     - NullCondMemberExpr: (Factory.Make(move x))?.v
//     - NullCondCallExpr:   f?(move x)   (f: a nullable owned-param callable)
//   each SEGV'd identically pre-fix and is rejected with the precise §28.3
//   diagnostic post-fix.
//
// ROOT CAUSE: `MethodOwnership::recordMovesIn` (Ownership_Checks.cpp) is the
// ONE authority that walks a LocalDecl-initializer / assignment-RHS
// expression looking for a `move` to validate (via `markMovedByName`) and
// mark MovedFrom. Its dynamic_cast chain had arms only for `UnaryExpr`,
// `BinaryExpr`, `CallExpr`, and `NewExpr` — not for the five wrapper kinds
// above, each of which simply carries the real owned-arg-consuming
// construct one level down (`ma->target`, `ix->target`, `pb->operand`,
// `nm->target`, `nc->callee`/`nc->args`). With no arm, the chain silently
// falls through and NEVER recurses into that substructure, so the `move`
// buried inside is invisible to this pass — exactly the family
// `checkRead` (the read-side twin) already guards against via its own
// arms for these same four node kinds (the fifth, NullCondCallExpr, has no
// checkRead arm either — a distinct, unaudited gap on the read side, filed
// separately).
//
// THE FIX: `recordMovesIn` gained five arms, each simply recursing into the
// substructure — mirroring `checkRead`'s existing `MemberAccessExpr` /
// `PostfixBangExpr` / `IndexExpr` arms for the first three, and the
// existing `CallExpr` arm's `recordMovesInArgs` dispatch for
// `NullCondCallExpr`.
//
// NOT part of this fix (independently confirmed, filed separately):
//   - `ConditionalExpr`/`CaseExpr`/`ThrowExpr` arms — in flight on a
//     sibling issue (#3989/#3990) as of this writing; not duplicated here.
//   - `CollectionLiteralExpr` (`var arr: OwningArray<Res> := [move x];`) —
//     same defect family, but with substantial pre-existing adjacent
//     machinery this fix did not audit; filed as #4024.
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
// per §28.2, §28.3, v0.294, #4003

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

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
