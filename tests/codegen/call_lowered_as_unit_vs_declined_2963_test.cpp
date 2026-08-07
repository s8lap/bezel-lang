// Regression tests for issue #2963 (codegen; language spec §21.7 "The return
// statement" / §4.7 the unit type; stdlib §6.9 `ChannelSender<T>.Send`, v0.240):
// `Emitter::lowerCall` returned `nullptr` for TWO categorically different
// outcomes, and nothing downstream could tell them apart —
//
//   1. "no arm lowered this call"  — nothing was emitted; a zero is substituted.
//   2. "an arm lowered it and the result is unit" — real code WAS emitted.
//
// The `try*Call` family is dispatched as `if (auto* v = tryLowerX(call)) return
// v;`, so an arm that emits code and returns nullptr because its result is unit
// falls through the REST of the chain exactly as a decline does.
// `tryLowerChannelAndDomainCall`'s `Send` arm is that shape: it emits a real
// `bz_channel_sender_send`, calls `emitPendingCheck()`, and returns nullptr.
//
// THE OBSERVABLE DEFECT (the filed repro, reproduced on `5feaae2`): a correct
// program printed an `internal:` note claiming a degrade that never happened —
//
//   public static method Push(s: ChannelSender<int>) => s.Send(42);
//   arrowunit.bzl:4:55: internal: return-value expression failed to lower to a
//                       value (degraded to a zero/null return value) — #2491
//   exit 0 ; program returns 1  (the item WAS buffered — the send worked)
//
// ⚠ THE DIAGNOSTIC HALF OF THIS FIX IS NOT PINNED HERE, and cannot be.  The AOT
// gtest harness populates `JitResult::diagnostics` only on a compile REJECT (or
// crash/timeout) — a successful compile's WARNINGS never reach it — so an
// "assert no `internal:` note" written in this file passes on every compiler,
// fixed or not.  That was written first here and caught by running it against a
// deliberately unfixed build; it lives in the lit track instead:
//
//   tests/lit/call_lowered_as_unit_no_degrade_note_2963.bzl            (accept)
//   tests/lit/call_lowered_as_unit_declared_return_still_warns_2963.bzl (control)
//
// WHAT THIS FILE PINS is the other half, and the half a diagnostic test cannot
// see: that each of these calls really did LOWER.  A "fix" that silenced the
// note by breaking `Send` would pass the lit test and fail every test here.
//
// THE FIX IS NOT A LIST OF THE ARMS THAT EMIT-AND-RETURN-NULL.  That list is
// precisely the hand-maintained mirror this codebase has already paid for twice
// (#2868, #2888): the dispatch chain's arms include several hand-written
// `return nullptr;  // unit result` exits (`tryLowerConsoleCall`, the §9.8
// serialization void calls, #2967's built-in `Dispose` chain), and a nineteenth
// added tomorrow would silently acquire a FALSE REJECT — a positive list missing
// a name.  It is instead a fact the compiler ALREADY COMPUTES: the IR itself.
// `lowerCall` is now a thin wrapper that snapshots the builder position
// (`IrFootprint` — insert block plus the instruction before the insert point, an
// O(1) pointer pair), runs the dispatch, and classifies the nullptr by whether
// anything was appended.  One assignment, in one place; every arm participates
// by existing.
//
// ⚠ ITS FAILURE DIRECTION IS ONE-WAY BY CONSTRUCTION.  An arm that lowers the
// RECEIVER before discovering it does not know the member
// (`tryLowerChannelAndDomainCall`'s own `isSender` arm does exactly this) leaves
// a footprint and reads as "handled".  That costs a MISSED detection — never a
// false reject, which is the only error a codegen gate may not make.

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── The filed repro: the call must really have lowered ─────────────────────

BEZEL_TEST(call_lowered_as_unit_vs_declined_2963,
           unit_arrow_body_over_channel_send_really_sends_stdlib_6_9) {
    // The oracle is the channel's buffered `Count`, not exit 0: it is the only
    // observable that distinguishes "the send lowered" from "the whole arrow
    // body was degraded away, quietly or loudly".
    const std::string program =
        "module M; import System.Threading; "
        "class C "
        "  public static method Push(s: ChannelSender<int>) => s.Send(42); "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    C.Push(sender); "
        "    C.Push(sender); "
        "    return ch.Count; "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(2, r.exitCode);
}

// ── A SECOND arm, so the fix is not "the Send arm was special-cased" ────────

BEZEL_TEST(call_lowered_as_unit_vs_declined_2963,
           unit_arrow_body_over_console_writeline_really_prints_spec_4_7) {
    // `Console.WriteLine` reaches the dispatch through a DIFFERENT hand-written
    // exit — `if (tryLowerConsoleCall(call)) return nullptr;  // unit result`, a
    // bool-returning helper several thousand lines from the `Send` arm and
    // sharing no code with it. Oracle: the text really reaches stdout.
    const std::string program =
        "module M; "
        "class C "
        "  public static method Say(s: string) => Console.WriteLine(s); "
        "  public static method Main(): int "
        "    C.Say(\"hello\"); "
        "    return 7; "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("hello") != std::string::npos);
}

BEZEL_TEST(call_lowered_as_unit_vs_declined_2963,
           unit_arrow_body_over_a_user_method_really_runs_spec_21_7) {
    // A THIRD route with nothing built-in about it: the arrow body forwards to a
    // user method that declares no return type, so §21.7's "this callable has no
    // return type" holds on both sides and the ordinary method-symbol dispatch
    // (not any `try*Call` arm) is what emits and yields nothing. Oracle: the
    // side effect happened twice.
    const std::string program =
        "module M; "
        "class Counter "
        "  public _n: int := 0; "
        "  public method Bump() _n := _n + 1; end Bump; "
        "  public method Twice() => Bump(); "
        "end Counter; "
        "class C public static method Main(): int "
        "  using var c := new Counter(); "
        "  c.Twice(); "
        "  c.Twice(); "
        "  return c._n; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(2, r.exitCode);
}

// ── ANTI-OVER-REJECT control ────────────────────────────────────────────────

BEZEL_TEST(call_lowered_as_unit_vs_declined_2963,
           value_returning_arrow_body_after_unit_ones_is_untouched_spec_21_7) {
    // The wrapper assigns the flag on EVERY return, including the ordinary
    // value-producing one, so a value-returning call must CLEAR it rather than
    // inherit a stale true from an earlier statement. The two unit calls run
    // first precisely to set it; if the flag leaked, the `=> ch.Count` body
    // would be treated as valueless and this program would return 0.
    const std::string program =
        "module M; import System.Threading; "
        "class C "
        "  public static method Push(s: ChannelSender<int>) => s.Send(42); "
        "  public static method Count(ch: Channel<int>): int => ch.Count; "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    C.Push(sender); "
        "    C.Push(sender); "
        "    return C.Count(ch); "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(2, r.exitCode);
}
