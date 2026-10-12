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
// #5852 / §30.15 v0.334 — the SECOND HALF of the #1957 channel row, which the
// plain no-leak row above cannot see. #5852: the vtableless-builtin release arm
// that an auto-dropped `owned Channel<T>` PARAMETER takes declared
// bz_channel_dispose with ONE parameter and passed ONE argument, while the
// runtime takes (ch, cce_vtable). Two consequences, both measured:
//   (a) when a 2-param declaration already existed in the module the call failed
//       IR verification and the whole program was REJECTED — that is the
//       channel_owned_param_autodrop_no_leak row above (r.ran false);
//   (b) when that arm declared the function FIRST, declareC (which reuses the
//       first declaration of a name) installed the 1-param type and the call
//       site set up NO %rsi at all — the callee read whatever stale non-null
//       pointer happened to be in the second argument register.
// (b) is the live hazard: that second parameter is the vtable the runtime uses
// to fault PENDING RECEIVERS, and channel_close_and_drain_pending
// (bezel_runtime_concurrency.cpp) does `if (cce_vtable) memcpy(excObj, &cce_vtable,
// sizeof(void*))` — it tolerates NULL but faithfully copies a garbage pointer
// into the exception object a parked receiver is completed with.
//
// So this row parks a receiver (`inbox.Receive()` on an empty, still-open
// channel) and THEN lets the `owned ch` parameter auto-drop. A correctly formed
// bz_channel_dispose faults the parked task with ChannelClosedException and the
// catch fires → got == 1. A garbage vtable produces a fault through a bogus
// vtable instead, so this row is an end-to-end oracle on the SECOND argument,
// not merely on the call arity. It also fails without the fix, because that
// same program does not compile at all (IR verification).
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           channel_owned_param_autodrop_faults_pending_receiver_spec_30_15) {
    JitResult r = runAotChecked(
        std::string("module M; import System.Threading; ") + kR +
        "class C "
        "  public static method Consume(owned ch: Channel<R>) "
        "  end Consume; "              // auto-drop of `ch` disposes the channel
        "  public static async method Main(): Task<int> "
        "    var inbox := new Channel<R>(); "
        "    using var tx := inbox.NewSender(); "
        "    using var t := inbox.Receive(); "   // parks: empty + still open
        "    Consume(move inbox); "     // dispose faults the parked receiver
        "    var got := 0; "
        "    try "
        "      _ := await t; "
        "    catch ChannelClosedException => do "
        "      got := 1; "              // ONLY a real CCE vtable gets here
        "    end "
        "    end try; "
        "    return got; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
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

// =====================================================================
// #1957 BLOCKER regression — the DOUBLE-FREE the fix must NOT introduce. An
// `owned Shared<T>` param is explicitly Dispose()'d, then a throw unwinds. The
// §28.3 unwind disposal (releaseOnUnwindOnly) reloads the receiver slot — so
// Shared<T>.Dispose() MUST null that slot (as ChannelSender/Channel do), or the
// null-guarded unwind reload re-runs bz_shared_release on an already-freed
// control block (free(cb) is NOT idempotent → genuine double-free). A surviving
// Clone() strong handle makes it observable: a premature free would leave
// `a.Get()` reading freed memory. WITHOUT the slot-null fix this SEGVs under
// ASan (r.ran false); WITH it the release runs exactly once and V == 7.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           shared_owned_param_dispose_then_throw_single_release_spec_28_13_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; import System.Threading; ") + kR +
        "class E: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end E; "
        "class C "
        "  public static method Take(owned sm: Shared<R>) "
        "    sm.Dispose(); "             // release; MUST null the slot
        "    throw new E(); "            // unwind must NOT re-release
        "  end Take; "
        "  public static method Main(): int "
        "    using var a := new Shared<R>(new R(7)); "
        "    var b := a.Clone(); "       // surviving strong handle keeps R alive
        "    try "
        "      Take(move b); "
        "    catch E => do end "
        "    end try; "
        "    return a.Get().V; "         // 7 (a double-free would SEGV here)
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);            // single release — no double-free / SEGV
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// =====================================================================
// #1957 BLOCKER — the PRE-EXISTING double-free the same slot-null fix resolves
// (present on base, independent of the #1957 registration). A `using var` Shared
// clone is explicitly Dispose()'d, then its scope-end disposal ALSO releases →
// two bz_shared_release on one handle → free(cb) twice. With the slot-null fix
// the scope-end reload sees null and skips → single release. `a` still holds a
// strong handle, so the value is alive; V == 5. (SEGV without the fix.)
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           shared_usingvar_clone_dispose_scope_end_single_release_spec_28_13) {
    JitResult r = runAotChecked(
        std::string("module M; import System.Threading; ") + kR +
        "class C "
        "  public static method Main(): int "
        "    using var a := new Shared<R>(new R(5)); "
        "    do "
        "      using var s := a.Clone(); "
        "      s.Dispose(); "            // release the clone...
        "    end; "                       // ...scope-end must see null and skip
        "    return a.Get().V; "         // 5
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);            // single release — no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// =====================================================================
// #1957 guard: an `owned Shared<T>` param explicitly Dispose()'d then RETURNed
// (normal-path discharge, no throw). autoDropAtScopeEnd is false, so the param
// carries releaseOnUnwindOnly — which fires ONLY on the unwind exit; the normal
// return path leaves the release to the explicit Dispose alone. Single release,
// clean; the surviving Clone() handle keeps the value alive (V == 9).
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           shared_owned_param_dispose_then_return_single_release_spec_28_13) {
    JitResult r = runAotChecked(
        std::string("module M; import System.Threading; ") + kR +
        "class C "
        "  public static method Take(owned sm: Shared<R>) "
        "    sm.Dispose(); "             // sole release on the normal path
        "  end Take; "
        "  public static method Main(): int "
        "    using var a := new Shared<R>(new R(9)); "
        "    Take(a.Clone()); "          // pass an owned clone; Take disposes it
        "    return a.Get().V; "         // 9 (a still holds its own handle)
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);            // single release, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// =====================================================================
// #1957 sibling guard: the ChannelSender invariant the fix now also relies on.
// An `owned ChannelSender<T>` param explicitly Dispose()'d then a throw unwinds.
// ChannelSender<T>.Dispose() already nulls its slot, so the releaseOnUnwindOnly
// unwind reload skips → the sender is released exactly once (closing the channel,
// §30.15) with no double-release / UAF of the sender. The channel is observed
// closed after the caught throw (IsClosed == true → 1); ASan-clean.
// =====================================================================
BEZEL_TEST(channel_sender_owned_param_autodrop_1957,
           sender_owned_param_dispose_then_throw_single_release_spec_30_15) {
    JitResult r = runAotChecked(
        std::string("module M; import System.Threading; ") + kR +
        "class E: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end E; "
        "class C "
        "  public static method Emit(owned tx: ChannelSender<R>) "
        "    tx.Dispose(); "             // release (last sender -> closes)
        "    throw new E(); "            // unwind must NOT re-release
        "  end Emit; "
        "  public static async method Main(): Task<int> "
        "    using var inbox := new Channel<R>(); "
        "    try "
        "      Emit(inbox.NewSender()); "
        "    catch E => do end "
        "    end try; "
        "    if inbox.IsClosed then return 1; else return 0; end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);            // single release, no double-free / UAF
    BEZEL_EXPECT_EQ(r.exitCode, 1);      // dispose closed the channel
}
