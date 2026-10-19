// fix #2439 (K7.4 "The join rule" / K1.4 "maybe-consumed", §28.3 "Local
// variables, ownership, and borrows", spec v0.235): an `owned` local
// declared outside a loop, moved unconditionally into an owned-sink call
// inside the loop body on every iteration, compiled clean with ZERO
// diagnostics on unmodified main and was a confirmed heap-use-after-free at
// runtime under AddressSanitizer:
//
//   ==ERROR: AddressSanitizer: heap-use-after-free ...
//   READ of size 8 ... in bz$M.Sink$Take
//   freed by thread T0 here:
//       #1 ... in bz$M.Sink$Take   (iteration 1's free)
//   previously allocated by thread T0 here:
//       #1 ... in main
//
// (iteration 2's `Sink.Take(move n)` reads/frees the SAME heap object
// iteration 1 already freed — `n` was never reassigned between calls, and
// the ownership checker never observed the second `move` as a re-consume of
// an already-moved-out value.)
//
// See tests/ownership/owned_local_loop_unconditional_move_uaf_2439_test.cpp
// for the full root-cause narrative (a fixpoint-convergence defect in
// MethodOwnership::markMovedByName, src/ownership/Ownership_Checks.cpp) and
// the diagnostic-text-level REJECT-direction coverage. This file proves the
// two things that matter at the codegen/runtime boundary:
//
//   1. Post-fix, the program that used to compile clean and crash NOW FAILS
//      TO COMPILE outright — the UAF binary can never be produced. Per the
//      lane protocol, the coarse "did it reject" check (`rejects()`,
//      `!r.ran`) is insufficient on its own — `runAot`'s WIFSIGNALED path
//      synthesizes an Error diagnostic for a runtime crash too, so a test
//      that only checked `!r.ran` could not tell a genuine compile-time
//      rejection from the pre-fix ASan crash it replaces. Diagnostic TEXT is
//      asserted here for exactly that reason.
//   2. The idiomatic ACCEPT-direction shape this fix must not break — a
//      fresh owned local minted and consumed once PER ITERATION (declared
//      INSIDE the loop body, not carried in from outside it) — still
//      compiles, links, and runs clean under ASan/LSan with EXACTLY-ONCE
//      disposal per iteration (a live-object counter, not merely "did not
//      crash").
//
// The "sweep the shape" step of this fix found the identical defect
// independently in checkRead's own maybeConsumed handling (a loop-carried
// maybe-consumed local that is only ever READ, never itself moved/disposed,
// inside the loop) — see the ownership test file for the full narrative and
// the `disposedOnSomePath` sibling. `reject_unconditional_read_of_maybe_
// consumed_never_produces_a_binary` below is the codegen-boundary proof for
// that sibling, mirroring this file's own move-based coverage.
//
// per K7.4, K1.4, §28.3, #2439, #2436

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

const char* kSink =
    "class Sink "
    "  public static method Take(owned x: Item) x.Dispose(); end Take; "
    "end Sink; ";

const char* kReader =
    "class Reader "
    "  public static method Peek(x: Item) end Peek; "
    "end Reader; ";

// Res — owned instance, destructor decrements a static counter: Res.alive ==
// 0 at the end proves exactly-once disposal of every constructor-minted Res
// across ALL loop iterations (not merely "no crash") on top of ASan/LSan's
// own double-free / leak / use-after-free detection.
const char* kRes =
    "class Res "
    "  public static alive: int := 0; "
    "  public constructor() Res.alive := Res.alive + 1; end constructor; "
    "  destructor Res.alive := Res.alive - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — `for` form. Pre-fix: compiled clean (r.ran==true),
// confirmed heap-use-after-free under ASan on iteration 2's `move n`.
// Post-fix: compilation itself fails (r.ran==false) with the maybe-consumed
// diagnostic — the UAF binary is never produced.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_unconditional_move_for_loop_never_produces_a_binary) {
    JitResult r = runAot(
        "module M; class Item end Item; " + std::string(kSink) +
        "class C "
        "  public static method Main(): int "
        "    var n := new Item(); "
        "    for var i in 0..3 do "
        "      Sink.Take(move n); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "cannot consume `n`: it may have already been consumed on "
           "another branch (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// THE ISSUE'S OWN REPRO — `while` form (the issue's second repro; the UAF
// was confirmed identically for both loop forms).
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_unconditional_move_while_loop_never_produces_a_binary) {
    JitResult r = runAot(
        "module M; class Item end Item; " + std::string(kSink) +
        "class C "
        "  public static method Main(): int "
        "    var n := new Item(); "
        "    var i: int := 0; "
        "    while i < 3 do "
        "      Sink.Take(move n); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "cannot consume `n`: it may have already been consumed on "
           "another branch (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// SWEEP FINDING — checkRead's own maybeConsumed branch had the identical
// defect independently of markMovedByName: a maybe-consumed local (joined
// ONCE, straight-line, in a plain if/else BEFORE the loop — no move anywhere
// inside the loop body) that a loop only ever READS on every iteration
// compiled clean pre-fix, even with the markMovedByName fix alone in place
// (confirmed by reverting just checkRead's own gate and re-probing: silent
// accept returns). See the ownership test file's matching case for the full
// mechanism. Codegen-boundary proof mirroring this file's move-based
// coverage above.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           reject_unconditional_read_of_maybe_consumed_never_produces_a_binary) {
    JitResult r = runAot(
        "module M; class Item end Item; " + std::string(kSink) +
        std::string(kReader) +
        "class C "
        "  public static method Main(): int "
        "    var n := new Item(); "
        "    var b: bool := true; "
        "    if b then "
        "      Sink.Take(move n); "
        "    end; "
        "    for var i in 0..3 do "
        "      Reader.Peek(n); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "local `n` may have already been consumed on another branch and "
           "may not be read here (maybe-consumed join, §28.3)"));
}

// ===========================================================================
// ACCEPT, RUNTIME-VERIFIED — the idiomatic fresh-per-iteration shape (`n`
// declared INSIDE the loop body, minted and consumed once per pass) must
// keep compiling, linking, and running clean, with EXACTLY-ONCE disposal
// per iteration under ASan/LSan. This is the regression check that the
// fix's stricter loop-back-edge tracking does not spill over into a false
// reject of ordinary, correct loop code — the case this fix's own
// mechanism (deferring the maybe-consumed poison until the unsuppressed
// pass) must leave untouched, since a freshly-redeclared-each-visit local
// has no pre-loop edge for the back edge to conflict with.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           accept_fresh_local_declared_inside_loop_disposes_exactly_once_each) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Sink "
        "  public static method Take(owned x: Res) x.Dispose(); end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..3 do "
        "      var n := new Res(); "
        "      Sink.Take(move n); "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    // 3 mints, 3 consumes each disposed exactly once — settles at 0. A
    // double-free / use-after-free would abort under ASan (r.ran == false),
    // and a leak would abort under LSan; reaching exitCode == 0 here proves
    // neither happened.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// ACCEPT, RUNTIME-VERIFIED — non-loop baseline: a single straight-line move
// of a pre-declared owned local, outside any loop, must be completely
// unaffected — the fix only changes behavior for a symbol whose entry state
// differs between fixpoint passes because of a loop back edge.
// ===========================================================================
BEZEL_TEST(owned_local_loop_unconditional_move_uaf_2439,
           accept_straightline_single_move_no_loop_runs_clean) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Sink "
        "  public static method Take(owned x: Res) x.Dispose(); end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var n := new Res(); "
        "    Sink.Take(move n); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
