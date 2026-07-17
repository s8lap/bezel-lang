// Codegen / runtime — §30.15 / §28.13 / §28.3 (#1957): auto-drop of an `owned`
// ChannelSender<T> / Channel<T> / Shared<T> PARAMETER must run the builtin's
// TYPE-SPECIFIC runtime release, not a generic vtable dispose (nor nothing).
//
// §30.15 (stdlib §6.9 "Closing"): "Disposing the last sender closes the
// channel." An `owned tx: ChannelSender<T>` parameter that is never discharged
// relies on §28.3 affine auto-drop to be disposed at method exit; as the last
// sender that disposal must CLOSE the channel.
//
// THE BUG (#1957). registerUnwindOwnedParam (LlvmBackend_Setup.cpp) computed the
// param type `rt` and gated on isClassOrInterfaceRef(rt) before registering the
// auto-drop disposal. A builtin ChannelSender<T>/Channel<T>/Shared<T> has NO user
// TypeSymbol (rt->declaration == null), so that gate is false and the disposal
// registration was SKIPPED ENTIRELY: the auto-dropped owned sender was never
// released. Its channel stayed OPEN (IsClosed == false), the whole graph LEAKED
// (LSan: undelivered items + the channel core + the sender), and the canonical
// cross-domain producer idiom DEADLOCKED — a consumer draining until close blocks
// forever on empty-and-open. (The compiler even prints the "§28.3 auto-drop"
// note, but nothing ran.)
//
// THE FIX. registerUnwindOwnedParam now detects the compiler-known builtin
// endpoints by head name — exactly the using-var path — BEFORE the class/interface
// gate and registers the type-specific disposal (bz_channel_sender_release /
// bz_channel_dispose / bz_shared_release). A never-discharged owner (auto-drop)
// releases on BOTH exits (the sole release); a discharged owned param
// (moved/returned/disposed on the normal path) releases only on the unwind exit
// (releaseOnUnwindOnly), so the normal-path discharge site is not double-released.
//
// OBSERVABILITY. The functional test reads Channel.IsClosed after the auto-drop —
// false without the fix, true with it. The leak tests run under runAotChecked
// (generated-code ASan + LSan): the #1957 leak flips r.ran false without the fix;
// with it the run is clean AND the type-specific release must not double-free
// (dispose-exactly-once rides on r.ran). The cross-domain test drains until the
// channel closes and returns the item sum: it HANGS (r.ran false, timeout)
// without the fix and returns the sum with it.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// A sendable class item (sealed, value fields) so Channel<R> is well-formed
// (§30.14). Its destructor makes disposal observable if needed.
const char* kR =
    "sealed class R "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end R; ";

// The exact #1957 repro shape: an `owned tx: ChannelSender<R>` parameter that
// sends an item and returns WITHOUT discharging tx — §28.3 auto-drop must
// release it, closing the channel (§30.15, last sender). Main mints the ONLY
// sender into Sink, then reads IsClosed.
const char* kSinkAutodropClosesChannel =
    "module M; import System.Threading; ";  // + kR + body (assembled below)

}  // namespace

// =====================================================================
// #1957 — functional: auto-dropped owned ChannelSender param closes the
// channel (§30.15 last-sender-closes via §28.3 auto-drop). IsClosed == true.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           sender_autodrop_closes_channel_spec_30_15_28_3) {
    JitResult r = runAot(
        std::string(kSinkAutodropClosesChannel) + kR +
        "class C "
        "  public static method Sink(owned tx: ChannelSender<R>) "
        "    tx.Send(new R(1)); "         // no discharge — §28.3 auto-drop closes
        "  end Sink; "
        "  public static async method Main(): Task<int> "
        "    using var inbox := new Channel<R>(); "
        "    Sink(inbox.NewSender()); "   // mint the ONLY sender, move into Sink
        "    if inbox.IsClosed then "
        "      return 1; "                // WITH fix
        "    else "
        "      return 0; "                // WITHOUT fix: sender never released
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);       // IsClosed == true (0 without the fix)
}

// =====================================================================
// #1957 — leak: the SAME program under ASan/LSan. WITHOUT the fix the sender is
// never released; the channel core + its undelivered item + the sender leak
// (LSan flips r.ran false). WITH the fix the release runs exactly once (a
// double-release would be an ASan hit), so the run is clean.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           sender_autodrop_no_leak_spec_30_15_28_3) {
    JitResult r = runAotChecked(
        std::string(kSinkAutodropClosesChannel) + kR +
        "class C "
        "  public static method Sink(owned tx: ChannelSender<R>) "
        "    tx.Send(new R(1)); "
        "  end Sink; "
        "  public static async method Main(): Task<int> "
        "    using var inbox := new Channel<R>(); "
        "    Sink(inbox.NewSender()); "
        "    if inbox.IsClosed then return 1; else return 0; end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;            // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);             // no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// =====================================================================
// #1957 — the canonical CROSS-DOMAIN producer idiom. A producer domain is
// spawned with the only sender; it sends items and returns, and §28.3 auto-drop
// of the owned sender must close the channel (§30.15) so the consumer's drain
// terminates. WITHOUT the fix the sender is never released: the consumer's
// post-drain Receive blocks forever → the program hangs (r.ran false, timeout).
// WITH the fix the drain ends on ChannelClosedException and returns the sum.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           sender_autodrop_crossdomain_no_deadlock_spec_30_15_28_3) {
    JitResult r = runAot(
        std::string("module M; import System.Threading; ") + kR +
        "class C "
        "  public static method Produce(owned tx: ChannelSender<R>) "
        "    tx.Send(new R(10)); "
        "    tx.Send(new R(20)); "
        "    tx.Send(new R(12)); "        // returns -> auto-drop closes channel
        "  end Produce; "
        "  public static async method Main(): Task<int> "
        "    using var inbox := new Channel<R>(); "
        "    using var w := Domain.Spawn(Produce, inbox.NewSender()); "
        "    var sum := 0; "
        "    var draining := true; "
        "    while draining do "
        "      try "
        "        using var item := await inbox.Receive(); "
        "        sum := sum + item.V; "
        "      catch ChannelClosedException => do "
        "        draining := false; "     // drain terminates ONLY if channel closed
        "      end "
        "      end try; "
        "    end while; "
        "    await w; "
        "    return sum; "                // 10+20+12 = 42
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);             // no deadlock (would time out without fix)
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// #1957 regression guard: an EXPLICIT `tx.Dispose()` (the normal-path discharge)
// must still close the channel and must NOT be double-released by the fix. The
// param is discharged, so the auto-drop registration does not fire on the normal
// path; the explicit dispose is the sole release. Clean under ASan/LSan.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           explicit_dispose_still_closes_no_double_release_spec_30_15) {
    JitResult r = runAotChecked(
        std::string("module M; import System.Threading; ") + kR +
        "class C "
        "  public static method Sink(owned tx: ChannelSender<R>) "
        "    tx.Send(new R(1)); "
        "    tx.Dispose(); "             // explicit discharge — sole release
        "  end Sink; "
        "  public static async method Main(): Task<int> "
        "    using var inbox := new Channel<R>(); "
        "    Sink(inbox.NewSender()); "
        "    if inbox.IsClosed then return 1; else return 0; end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);            // no double-release, no leak
    BEZEL_EXPECT_EQ(r.exitCode, 1);      // still closes
}

// =====================================================================
// #1957 generalization: an `owned ch: Channel<T>` PARAMETER auto-dropped must
// run bz_channel_dispose (not a generic vtable dispose), disposing the channel
// and its undelivered class-typed item. Same isClassOrInterfaceRef early-return
// root cause as the sender case. Clean under ASan/LSan.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           channel_owned_param_autodrop_no_leak_spec_30_15_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; import System.Threading; ") + kR +
        "class C "
        "  public static method Consume(owned ch: Channel<R>) "
        "  end Consume; "                // drop the channel -> auto-drop disposes
        "  public static async method Main(): Task<int> "
        "    var inbox := new Channel<R>(); "
        "    using var tx := inbox.NewSender(); "
        "    tx.Send(new R(7)); "        // one undelivered item to dispose on close
        "    Consume(move inbox); "      // move the channel into Consume
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #1957 generalization: an `owned sm: Shared<T>` PARAMETER auto-dropped must run
// bz_shared_release (decrement the strong count; dispose the inner value at 0),
// not a generic vtable dispose. Same root cause. Clean under ASan/LSan.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           shared_owned_param_autodrop_no_leak_spec_28_13_28_3) {
    JitResult r = runAotChecked(
        "module M; import System.Threading; "
        "class C "
        "  public static method Take(owned sm: Shared<Mutex>) "
        "  end Take; "                   // drop the handle -> auto-drop releases
        "  public static method Main(): int "
        "    var sm := new Shared<Mutex>(new Mutex()); "
        "    Take(move sm); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
