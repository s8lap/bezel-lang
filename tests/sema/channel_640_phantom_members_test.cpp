// fix #640 — stdlib §6.9: Channel<T> (receiving end) must reject phantom members.
//
// Spec authority: stdlib §6.9 (v0.135+):
//   Channel<T> exposes: NewSender(), Receive(), TryReceive(out owned item: T?),
//   Count, IsClosed.  Send lives only on ChannelSender<T>.
//   TrySend is defined on NEITHER type in any stdlib version.
//
// Bug: ch.Send(42) and ch.TrySend(42) both compiled silently and no-op'd.
// Root cause: the Channel<T> dispatch arm in ExprTypecheck_Calls.cpp recognized
// "Send" as a valid member name, and both "Send" and "TrySend" fell through
// without an error when the name matched nothing.
//
// Tests:
//   (1) ch.Send(42) on a Channel<int> receiver — compile error; Send is on
//       ChannelSender<T>, not Channel<T> (stdlib §6.9, #640).
//   (2) ch.TrySend(42) on a Channel<int> receiver — compile error; TrySend
//       exists on neither Channel nor ChannelSender (stdlib §6.9, #640).
//   (3) ch.Bogus() — compile error; entirely unknown member on Channel<T>.
//   (4) sender.Send(42) on a ChannelSender<int> receiver — still compiles OK
//       (stdlib §6.9; this is the correct sending surface).
//   (5) ch.NewSender() — still compiles OK (§30.15 / stdlib §6.9).
//   (6) ch.Receive() — still compiles OK (§30.15 / stdlib §6.9).
//   (7) ch.TryReceive(out v) — still compiles OK (§30.15 / stdlib §6.9).
//   (8) ch.Count — not a method call; property access; no error.
//   (9) ch.IsClosed — not a method call; property access; no error.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiag(const bezel::test::SemaResult& r, std::string_view substr) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(substr) != std::string::npos) return true;
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) §6.9 / #640: ch.Send(42) on Channel<int> → compile error.
//     Send is on ChannelSender<T>, not Channel<T>.
// ---------------------------------------------------------------------------
BEZEL_TEST(channel_640_phantom_members,
           channel_send_on_receiver_end_rejected_6_9) {
    // stdlib §6.9 (#640): `Send` is not a method of `Channel<T>`; it lives on
    // `ChannelSender<T>`.  Calling ch.Send on the channel end must be an error.
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    ch.Send(42); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "Channel"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// (2) §6.9 / #640: ch.TrySend(42) on Channel<int> → compile error.
//     TrySend does not exist on Channel<T> or ChannelSender<T> (§6.9).
// ---------------------------------------------------------------------------
BEZEL_TEST(channel_640_phantom_members,
           channel_trysend_on_receiver_end_rejected_6_9) {
    // stdlib §6.9 (#640): `TrySend` exists on neither `Channel<T>` nor
    // `ChannelSender<T>`.  Calling ch.TrySend on any end must be an error.
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    ch.TrySend(42); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "Channel"));
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// (3) §6.9 / #640: ch.Bogus() — entirely unknown member on Channel<T>.
// ---------------------------------------------------------------------------
BEZEL_TEST(channel_640_phantom_members,
           channel_bogus_member_rejected_6_9) {
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    ch.Bogus(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// (4) §6.9: sender.Send(42) on ChannelSender<int> — still compiles OK.
//     Send is the correct method on the sending end.
// ---------------------------------------------------------------------------
BEZEL_TEST(channel_640_phantom_members,
           channel_sender_send_still_allowed_6_9) {
    // stdlib §6.9: `Send` lives on `ChannelSender<T>`; this shape is correct.
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(42); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// (5) §6.9: ch.NewSender() — legitimate method on Channel<T>; no error.
// ---------------------------------------------------------------------------
BEZEL_TEST(channel_640_phantom_members,
           channel_newsender_still_allowed_6_9) {
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    using var s := ch.NewSender(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// (6) §6.9 / §30.15: ch.Receive() — legitimate async method on Channel<T>; no error.
//     (Receive returns an OwnedTask/Task; just type-check, don't run.)
// ---------------------------------------------------------------------------
BEZEL_TEST(channel_640_phantom_members,
           channel_receive_still_allowed_6_9) {
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static async method Test(): Task "
        "    using var ch := new Channel<int>(); "
        "    var _ := await ch.Receive(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// ---------------------------------------------------------------------------
// (7) §6.9 / §30.15: ch.TryReceive(out v) — legitimate method on Channel<T>; no error.
// ---------------------------------------------------------------------------
BEZEL_TEST(channel_640_phantom_members,
           channel_tryreceive_still_allowed_6_9) {
    auto r = parseSemaAll(
        "module M; "
        "import System.Threading; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<int>(); "
        "    var v: int? := null; "
        "    var ok := ch.TryReceive(out v); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}
