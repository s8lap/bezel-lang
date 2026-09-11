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
