// Spec-derived regression tests for issue #4225 — `checkConsumeFreeze` /
// `borrowHolderUsedAfter` treated a REBIND of a borrow holder as a live "use",
// producing a false consume-site-freeze reject (§28.3/§28.7/§30.14 v0.180).
//
// `MethodOwnership::borrowHolderUsedAfter` answers "is there a later use of
// `holder`?" by scanning the whole-body pre-scan `borrowUseOffsets_[holder]`
// for ANY offset after the consume site — with no notion of when `holder`
// was later REBOUND away from the consumed source. `collectBorrowUsesStmt`'s
// AssignmentStmt arm already excludes a bare-identifier `:=` TARGET from
// counting as a use of ITSELF ("a pure WRITE (rebind), not a read"), but that
// exclusion does not help a LATER, real touch of the holder's NEW value —
// `y := new Res(1); y.Dispose();` after `x := new Res(9);` was rejected as
// though `y` were still aliasing the just-reassigned `x`, even though `y` had
// already been rebound away from it on the very next line.
//
// Fix: `aliasBorrowSourceWindows_` (Ownership_impl.hpp) tracks, for a PLAIN
// identifier alias holder, the source-offset WINDOW over which its tracked
// source equals a given symbol — the same window discipline
// `recordBorrowLocalSource_` / `RecordBorrowSource` already established for
// the record/span case (#1741), applied here to the plain-alias case
// `checkConsumeFreeze`'s states_-based holder loop reads directly.
// `resolveAliasChainRoot` follows a chain of alias holders to its ultimate
// root (state-independent, since the pre-scan runs before `states_` is
// live) so a rebind THROUGH an intermediate alias (`y := w;` where `w`
// itself borrows the consumed symbol) still correctly keeps the window open
// — closing it there would be an UNSOUND missed rejection, not just an
// over-rejection.
//
// Confirmed PRE-EXISTING and SYMMETRIC between method scope and lambda-body
// scope on this branch's parent (measured against `6f46a73`, the commit
// under review when #4225 was filed): the identical shape is reached via the
// pre-existing `move`-RHS consuming path inside a lambda body too, so both
// scopes are covered below.
//
// A NOTE ON EXECUTION ROWS: the issue's own literal repro (`y := new
// Res(1); y.Dispose();`) reproduces a SEPARATE, pre-existing, unrelated leak
// (filed as issue 4246 — a borrow-declared local reassigned to an owned
// value and explicitly `.Dispose()`d does not free it, independent of any
// consume/freeze mechanism; confirmed to reproduce with ZERO reassignment of
// the original consumed local at all). Running that exact shape through the
// AOT pipeline under the ASan gate (`--full`) would spuriously fail THIS
// fix's own regression test on an unrelated defect. The real-execution rows
// below therefore rebind the holder to another EXISTING borrow (`y := w;`
// where `w` is a separately-owned local) instead of a fresh owned value —
// the holder never itself takes a disposal obligation, so these rows are
// leak-free while still exercising exactly the same window-closing
// mechanism the fix adds. The diagnostic-only rows use the issue's literal
// shape directly (no execution, no ASan exposure).
//
// per §28.3, §28.7, §30.14, v0.180, #4225

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

const char* kFreezeWording =
    "cannot consume `x` here: the borrow held by `y` is still live — its "
    "later use would read freed memory (consume-site freeze, "
    "§28.3/§28.7/§30.14 v0.180)";

const char* kRes =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int "
    "    var t := r.tag; r.Dispose(); return t; "
    "  end Take; "
    "end Sink; ";

}  // namespace

// ===========================================================================
// Method scope — the issue's own repro.
// ===========================================================================

// Row 1 (diagnostic-only, issue's literal shape): `y` is rebound to a fresh
// owned value on the line immediately after `x`'s reassignment, before `y`
// is ever touched again — must NOT false-reject. FAIL-WITHOUT this fix
// (measured on 6f46a73): rejected with kFreezeWording at the `x := new
// Res(9);` line. PASS-WITH: no errors.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_rebind_before_later_use_accepted_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y := new Res(1); "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 2 (real execution, leak-safe rebind-to-existing-borrow shape — see the
// file header for why): `y` borrows `x`, `x` is reassigned, `y` is rebound
// to `w` (a separately-owned local) BEFORE `y` is read again. FAIL-WITHOUT
// this fix (measured on 6f46a73): rejected with kFreezeWording at the `x :=
// new Res(9);` line. PASS-WITH: compiles, runs, ASan-clean (measured under
// `agent_check.sh --probe --asan`), returns `w.tag` (7).
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_rebind_to_other_borrow_before_later_use_accepted_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var w := new Res(7); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y := w; "
        "    var t := y.tag; "
        "    w.Dispose(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// Row 3 (negative control, real execution): the SAME wording still fires
// with the exact §28.3/§28.7/§30.14 text and a real diagnostic location when
// `y` is never rebound at all — the baseline true-positive this fix must not
// weaken. Pairs failedAtCompileTimeWithRealLocation with message text (a
// bare compile-time-failure check is not enough for a shape whose pre-fix
// history in this family included crash-routed diagnostics, lane rule §18).
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_no_rebind_still_rejected_control_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 4 (negative control): a live READ of `y` BETWEEN the consume and the
// later rebind must still reject — the window only closes at the rebind
// offset, not retroactively before it.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_read_before_rebind_still_rejected_control_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    var u := y.tag; "
        "    y := new Res(1); "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return u; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, std::string(kFreezeWording)));
}

// Row 5 (negative control — SOUNDNESS, the risk the fix must not introduce):
// `y` is rebound to `w`, which is ITSELF a borrow of `x` (`var w := x;`), so
// `y` still (transitively) aliases `x` after the rebind — must still reject.
// This is the row `resolveAliasChainRoot` exists for: without transitive
// resolution, the pre-scan would record `y`'s new window against `w`
// (the immediate RHS identifier) instead of `x` (the real, live
// `states_[y].source` after the rebind, per `borrowSourceForExpr`'s own
// multi-hop chain resolution), and the freeze would wrongly miss this case.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_rebind_to_another_alias_of_same_source_still_rejected_control_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    var w := x; "
        "    y := w; "
        "    x := new Res(9); "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, std::string(kFreezeWording)));
}

// ===========================================================================
// Lambda-body scope — the review's own symmetry finding.
// ===========================================================================

// Row 6 (diagnostic-only, issue's literal lambda shape, reached via the
// pre-existing `move`-RHS consuming path — NOT the #4216/#4217/#4218 port):
// must NOT false-reject. FAIL-WITHOUT this fix (measured on 6f46a73):
// rejected with kFreezeWording. PASS-WITH: no errors.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           lambda_rebind_before_later_use_accepted_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      var t := Sink.Take(move x); "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 7 (real execution, lambda scope, leak-safe rebind-to-existing-borrow
// shape). FAIL-WITHOUT this fix (measured on 6f46a73): rejected with
// kFreezeWording. PASS-WITH: compiles, runs, ASan-clean (measured), returns
// `t + u` = Res(5).tag + Res(7).tag = 12.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           lambda_rebind_to_other_borrow_before_later_use_accepted_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var w := new Res(7); "
        "      var y := x; "
        "      var t := Sink.Take(move x); "
        "      y := w; "
        "      var u := y.tag; "
        "      w.Dispose(); "
        "      return t + u; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// Row 8 (negative control, lambda scope): the symmetric baseline
// true-positive — no rebind at all — must still reject inside a lambda body
// too, pairing the crash-safe location check with message text.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           lambda_no_rebind_still_rejected_control_4225) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      var t := Sink.Take(move x); "
        "      y.Dispose(); "
        "      return t; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// ===========================================================================
// Send-back #4250 — the original #4225 fix's `recollectAliasBorrowSourceOnReassign`
// closed a holder's tracked-alias window UNCONDITIONALLY before examining the
// new RHS, and only conditionally reopened it for a bare-identifier RHS. Two
// RHS shapes fell through that gap:
//
//  (a) SELF-ASSIGNMENT (`y := y;`): closing unconditionally retired a
//      genuinely still-open window; the reopen check
//      (`resolveAliasChainRoot(lsym)`) then found the entry it had JUST
//      closed, returned `lsym` itself, and the reopen guard (`src != lsym`)
//      failed — the window stayed closed forever. Measured (#4250) as a REAL
//      heap-use-after-free, method AND lambda scope.
//
//  (b) Any RHS this pre-scan does not structurally recognize as a bare
//      identifier (`this.w`, `x.Field`, `w[0]`) was ALSO closed
//      unconditionally, on the (false) reasoning that the real forward walk
//      resolves those shapes to no source either. `borrowSourceForExpr`'s own
//      Shape 1 (Ownership_Checks.cpp, ~line 5556) DOES recognize
//      MemberAccessExpr / NullCondMemberExpr / IndexExpr as alias-producing
//      when their root is a still-owned local — closing anyway silently
//      retired a window real dataflow still considered open. Measured
//      (#4250) as "compiles rc=0, zero diagnostics" for `y := x.other;`.
//
// Fix: `recollectAliasBorrowSourceOnReassign` now FAILS CLOSED BY DEFAULT.
// Only self-assignment (a no-op: touch nothing) and a bare identifier naming
// a DIFFERENT symbol (close-then-reopen, unchanged) or a `new` expression
// (close only) are trusted to retire a window; every other RHS shape leaves
// every open window untouched, so the old unbounded query stays in force —
// sound by construction rather than by enumeration.
//
// Rows below are ASan-run (`runAotChecked`, the full AOT pipeline), not
// compile-only: the member-access/index shapes' FAIL-WITHOUT symptom is
// "rc=0, zero diagnostics", which only `runAotChecked` exercises through the
// same native pipeline the `--full` ASan gate runs these through (a bare
// `parseAndCheckOwnership` call never reaches codegen or a sanitizer at all).
//
// per §28.3, §28.7, §30.14, v0.180, #4225, #4250
// ===========================================================================

namespace {

// A Res whose `other` PROPERTY (not field) returns `this` — a genuine alias,
// not merely a structurally-recognized shape with an uninitialized payload.
// `x.other` therefore really does read through `x`'s own memory, so a
// FAIL-WITHOUT run (the unfixed `4c65122` mechanism) is a measured
// heap-use-after-free, not just a static inconsistency.
const char* kResWithOtherProperty =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  public property other: Res => this; "
    "  destructor end destructor; "
    "end Res; ";

// Same trick for the indexer shape: `x[0]` genuinely aliases `x`.
const char* kResWithIndexer =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  public property [i: int]: Res => this; "
    "  destructor end destructor; "
    "end Res; ";

}  // namespace

// Row 9 (real execution, method scope, issue #4250's own repro 1):
// self-assignment (`y := y;`) between the declaration that opens `y`'s
// window and the later consume must NOT retire it. FAIL-WITHOUT this fix
// (measured on `4c65122`): accepts rc=0, zero diagnostics, and crashes under
// ASan with a heap-use-after-free reading `y.tag` after `x`'s reassignment
// frees the original `Res(5)`. PASS-WITH: rejects with the exact freeze
// wording, matching the no-rebind-at-all control (row 3) — self-assignment
// is a no-op for the tracked alias, so it must behave identically.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_self_assignment_before_consume_rejected_4250) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    y := y; "
        "    x := new Res(9); "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 10 (real execution, lambda scope, issue #4250's own repro 2): the same
// self-assignment shape inside a lambda body, reached via the pre-existing
// `move`-RHS consuming path. FAIL-WITHOUT: accepts, ASan heap-use-after-free
// reading `y.tag` after `Sink.Take(move x)` frees the original value.
// PASS-WITH: rejects with the exact freeze wording.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           lambda_self_assignment_before_consume_rejected_4250) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      y := y; "
        "      var t := Sink.Take(move x); "
        "      var u := y.tag; "
        "      return t + u; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 11 (real execution, method scope): `y := x.other;` where `other` is a
// PROPERTY (not a field) that returns `this` — a genuine self-alias, so a
// FAIL-WITHOUT run measures an actual heap-use-after-free, not merely a
// static inconsistency (the send-back's own construction hit a null deref
// instead, leaving the UAF unconfirmed for this shape; this variant closes
// that gap). `borrowSourceForExpr`'s Shape 1 recognizes a MemberAccessExpr
// RHS rooted at a still-owned local as alias-producing regardless of what
// member is actually read, so `y` genuinely still aliases `x` afterward.
// PASS-WITH: rejects with the exact freeze wording.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_member_access_reassign_rejected_4250) {
    std::string src =
        std::string(kResWithOtherProperty) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    y := x.other; "
        "    x := new Res(9); "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 12 (real execution, method scope): `y := x[0];` — the send-back's own
// "not probed" IndexExpr shape, "same by code reading" as the member-access
// row. The indexer property returns `this`, so this is a genuine self-alias
// and a FAIL-WITHOUT run measures a real heap-use-after-free. PASS-WITH:
// rejects with the exact freeze wording.
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_index_reassign_rejected_4250) {
    std::string src =
        std::string(kResWithIndexer) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    y := x[0]; "
        "    x := new Res(9); "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 13 (real execution, method scope, CONTROL — not a hole): the
// send-back's other "not probed" shape, `y := w!;` (a `PostfixBangExpr` RHS)
// reassigning a holder that previously tracked `x`. Measured: this accepts
// identically on `6f46a73` (baseline), `4c65122` (the unfixed #4225 commit),
// and this fix — `borrowSourceForExpr`'s Shape 1 dynamic_casts only match
// IdentifierExpr / MemberAccessExpr / NullCondMemberExpr / IndexExpr as the
// RHS's OUTER node; a `PostfixBangExpr` wrapper never enters that arm at
// all (checked: no `PostfixBangExpr` handling anywhere in Shape 1's body,
// Ownership_Checks.cpp:5459-5986), so `states_[y].source` is unconditionally
// CLEARED by the real forward walk the moment `y := w!;` executes — gate 1's
// own live `states_[holder].source == consumed` check (unmodified by any of
// this) excludes `y` from the holder loop entirely, before this fix's window
// (open or closed) is ever consulted. This fix's fail-closed default leaves
// the window open here too (a `PostfixBangExpr` is not a recognized
// close-safe shape either), but that is inert: gate 1 already retired the
// holder. Kept as a control asserting the accept is unaffected, not folded
// into the "must reject" rows above. per §28.3, #4250
BEZEL_TEST(rebind_bounded_consume_freeze_4225,
           method_bang_reassign_unrelated_symbol_control_4250) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var w: Res? := new Res(7); "
        "    var y := x; "
        "    y := w!; "
        "    x := new Res(9); "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    w!.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
