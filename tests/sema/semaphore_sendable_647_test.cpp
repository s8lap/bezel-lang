// #647 / stdlib §6.3 — Semaphore is sendable; SemaphoreToken is NOT sendable.
//
// Spec derivation:
//   stdlib §6.3 (v0.140): Semaphore is internally synchronized — like Mutex
//     (§6.2 v0.138) and Channel<T> (§6.9), it may be reached by multiple
//     domains simultaneously; that is its purpose.  Sendable per §30.14.
//   stdlib §6.3: SemaphoreToken is a held permit guard, domain-local — NOT
//     sendable (same reasoning as Lock in §6.2 v0.138).
//   §30.14: argument types at Domain.Spawn must be sendable.
//   §30.15 / §3.6: Channel<T> element type must be sendable — Channel<Semaphore>
//     must be accepted; Channel<SemaphoreToken> must be rejected.
//
// Tests cover:
//   (a) Semaphore as Domain.Spawn arg — accepted (positive / §30.14).
//   (b) SemaphoreToken as Domain.Spawn arg — rejected (§30.14).
//   (c) Channel<Semaphore> instantiation — accepted (§30.15 / §3.6).
//   (d) Channel<SemaphoreToken> instantiation — rejected (§30.15 / §3.6).
//   (e) Semaphore method surface accepted (no "no member" diagnostic).
//   (f) new SemaphoreToken() is a compile error (no public ctor; §6.3).

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
// (a) Semaphore as Domain.Spawn argument — accepted per stdlib §6.3 / §30.14.
// Semaphore is sendable (internally synchronized).
// ---------------------------------------------------------------------------

// §30.14 / stdlib §6.3: Semaphore passed as owned spawn argument → accepted.
BEZEL_TEST(semaphore_sendable_6_3,
           semaphore_as_spawn_arg_accepted_6_3_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned s: Semaphore) "
        "    s.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(Worker.Run, new Semaphore(1)); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Semaphore is sendable per §6.3 — no sendability error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (b) SemaphoreToken as Domain.Spawn argument — rejected per §6.3 / §30.14.
// A held permit guard is domain-local (like Lock in §6.2).
// ---------------------------------------------------------------------------

// §30.14 / stdlib §6.3: SemaphoreToken passed as spawn argument → rejected.
BEZEL_TEST(semaphore_sendable_6_3,
           semaphore_token_as_spawn_arg_rejected_6_3_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned tok: SemaphoreToken) "
        "    tok.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var sem := new Semaphore(1); "
        // §30.12 (v0.179 / #1522): blocking Acquire() is rejected lexically
        // inside async methods — AcquireAsync() keeps this test isolated to
        // the sendability diagnostic under test.
        "    var tok := await sem.AcquireAsync(); "
        "    using var t := Domain.Spawn(Worker.Run, move tok); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // SemaphoreToken is NOT sendable — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (c) Channel<Semaphore> instantiation — accepted per §6.3 / §30.15 / §3.6.
// Semaphore is sendable so it satisfies the element-sendable requirement.
// ---------------------------------------------------------------------------

// §30.15 / §3.6 / stdlib §6.3: Channel<Semaphore> accepted.
BEZEL_TEST(semaphore_sendable_6_3,
           channel_semaphore_accepted_6_3_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Semaphore>(); "
        "  end Test; "
        "end C;");
    // Semaphore is sendable → Channel<Semaphore> is valid per §3.6/§30.15.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (d) Channel<SemaphoreToken> instantiation — rejected per §6.3 / §30.15.
// SemaphoreToken is NOT sendable.
// ---------------------------------------------------------------------------

// §30.15 / §3.6 / stdlib §6.3: Channel<SemaphoreToken> rejected.
BEZEL_TEST(semaphore_sendable_6_3,
           channel_semaphore_token_rejected_6_3_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<SemaphoreToken>(); "
        "  end Test; "
        "end C;");
    // SemaphoreToken is NOT sendable per §6.3 — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (e) Semaphore method surface: Acquire, TryAcquire, AcquireAsync,
//     TryAcquireAsync are all accepted; unknown methods produce a diagnostic.
// ---------------------------------------------------------------------------

// §6.3: Acquire() is a valid method on Semaphore — no error.
BEZEL_TEST(semaphore_sendable_6_3, acquire_method_accepted_6_3) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var sem := new Semaphore(1); "
        "    using var tok := sem.Acquire(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.3: TryAcquire(timeout) is a valid method on Semaphore — no error.
BEZEL_TEST(semaphore_sendable_6_3, try_acquire_method_accepted_6_3) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var sem := new Semaphore(2); "
        "    using var tok := sem.TryAcquire(TimeSpan.FromSeconds(1)); "
        "    if tok <> null then "
        "    end; "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.3: AcquireAsync() is valid on Semaphore in an async method — no error.
BEZEL_TEST(semaphore_sendable_6_3, acquire_async_method_accepted_6_3) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(): Task "
        "    using var sem := new Semaphore(1); "
        "    using var tok := await sem.AcquireAsync(); "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.3: TryAcquireAsync() is valid on Semaphore in an async method — no error.
BEZEL_TEST(semaphore_sendable_6_3, try_acquire_async_method_accepted_6_3) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Go(): Task "
        "    using var sem := new Semaphore(2); "
        "    using var tok := await sem.TryAcquireAsync(TimeSpan.FromSeconds(1)); "
        "    if tok <> null then "
        "    end; "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.3 / #674: an unknown method on Semaphore produces a diagnostic.
BEZEL_TEST(semaphore_sendable_6_3, unknown_method_rejected_6_3) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var sem := new Semaphore(1); "
        "    sem.Release(99); "
        "  end Test; "
        "end C;");
    // 'Release' is not on Semaphore's stdlib §6.3 surface.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (f) new SemaphoreToken() is a compile error (no public constructor; §6.3).
// SemaphoreToken instances are obtained only from Semaphore.Acquire() /
// TryAcquire().
// ---------------------------------------------------------------------------

// §6.3: `new SemaphoreToken()` is a compile error.
BEZEL_TEST(semaphore_sendable_6_3, new_semaphore_token_rejected_6_3) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var tok := new SemaphoreToken(); "
        "  end Test; "
        "end C;");
    // SemaphoreToken has no public constructor.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
