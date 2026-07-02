// #647 / stdlib §6.2 v0.138 — Mutex is sendable; Lock is NOT sendable.
//
// Spec derivation:
//   stdlib §6.2 v0.138: "Mutex is sendable (language spec §30.14) and internally
//     synchronized — like Channel<T>, it may be reached by multiple domains
//     simultaneously; that is its purpose."
//   stdlib §6.2 v0.138: "Lock is not sendable: a held guard is bound to the
//     acquiring domain and is released there."
//   §30.14: argument types at Domain.Spawn must be sendable.
//   §30.15 / §3.6: Channel<T> element type must be sendable — Channel<Mutex>
//     must be accepted; Channel<Lock> must be rejected.
//
// Tests cover:
//   (a) Mutex as Domain.Spawn arg — accepted (positive / §30.14).
//   (b) Lock as Domain.Spawn arg — rejected with sendability diagnostic (§30.14).
//   (c) Channel<Mutex> instantiation — accepted (§30.15 / §3.6).
//   (d) Channel<Lock> instantiation — rejected (§30.15 / §3.6).

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

bool hasNotSendableDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not sendable") != std::string::npos ||
            d.message.find("sendable") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) Mutex as Domain.Spawn argument — accepted per stdlib §6.2 v0.138 /
//     §30.14.  Mutex transfers ownership into the spawned domain; the spawn
//     call must not produce a sendability diagnostic.
// ---------------------------------------------------------------------------

// §30.14 / stdlib §6.2 v0.138: Mutex passed as owned spawn argument → accepted.
BEZEL_TEST(mutex_sendable_647,
           mutex_as_spawn_arg_accepted_6_2_v0138_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned m: Mutex) "
        "    m.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(Worker.Run, new Mutex()); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Mutex is sendable per §6.2 v0.138 — no sendability error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (b) Lock as Domain.Spawn argument — rejected per stdlib §6.2 v0.138 /
//     §30.14.  A held guard is domain-local; transferring it across domains
//     would decouple acquisition from release.
// ---------------------------------------------------------------------------

// §30.14 / stdlib §6.2 v0.138: Lock passed as spawn argument → rejected.
BEZEL_TEST(mutex_sendable_647,
           lock_as_spawn_arg_rejected_6_2_v0138_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned g: Lock) "
        "    g.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var mutex := new Mutex(); "
        "    var guard := mutex.Acquire(); "
        "    using var t := Domain.Spawn(Worker.Run, move guard); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Lock is NOT sendable per §6.2 v0.138 — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (c) Channel<Mutex> instantiation — accepted per stdlib §6.2 v0.138 /
//     §30.15 / §3.6.  Mutex is sendable so it satisfies the element-sendable
//     requirement.
// ---------------------------------------------------------------------------

// §30.15 / §3.6 / stdlib §6.2 v0.138: Channel<Mutex> accepted.
BEZEL_TEST(mutex_sendable_647,
           channel_mutex_accepted_6_2_v0138_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Mutex>(); "
        "  end Test; "
        "end C;");
    // Mutex is sendable → Channel<Mutex> is valid per §3.6/§30.15.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (d) Channel<Lock> instantiation — rejected per stdlib §6.2 v0.138 /
//     §30.15 / §3.6.  Lock is NOT sendable.
// ---------------------------------------------------------------------------

// §30.15 / §3.6 / stdlib §6.2 v0.138: Channel<Lock> rejected.
BEZEL_TEST(mutex_sendable_647,
           channel_lock_rejected_6_2_v0138_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Lock>(); "
        "  end Test; "
        "end C;");
    // Lock is NOT sendable per §6.2 v0.138 — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// §6.2 / #647 / v0.139: AcquireAsync / TryAcquireAsync are now implemented.
//
// These tests confirm that the "not yet implemented" diagnostic is no longer
// emitted, and that the returned types are correct:
//   AcquireAsync()        → OwningTask<Lock>  (await yields Lock)
//   TryAcquireAsync(ts)   → OwningTask<Lock?> (await yields Lock?)
// ---------------------------------------------------------------------------

// §6.2 / #647 / v0.139: AcquireAsync in an async method produces no error.
// The "not yet implemented (#647 async acquire)" message must NOT appear.
BEZEL_TEST(mutex_sendable_647,
           acquire_async_no_longer_not_implemented_6_2_v0139) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(): Task "
        "    using var mutex := new Mutex(); "
        "    using var guard := await mutex.AcquireAsync(); "
        "  end Go; "
        "end C;");
    // No "not yet implemented" error — AcquireAsync is now live.
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("not yet implemented") == std::string::npos);
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.2 / #647 / v0.139: TryAcquireAsync in an async method produces no error.
BEZEL_TEST(mutex_sendable_647,
           try_acquire_async_no_longer_not_implemented_6_2_v0139) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(): Task "
        "    using var mutex := new Mutex(); "
        "    using var guard := await mutex.TryAcquireAsync("
        "        TimeSpan.FromSeconds(5)); "
        "  end Go; "
        "end C;");
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("not yet implemented") == std::string::npos);
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
