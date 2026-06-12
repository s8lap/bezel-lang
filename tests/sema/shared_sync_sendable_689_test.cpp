// #689 / §28.13 v0.140 — Shared<T> is sendable iff T is internally synchronized.
//
// Spec derivation:
//   §28.13 v0.140: "Shared<T> is sendable iff T is sendable and internally
//     synchronized — the compiler-known synchronized set: Mutex, Atomic<T>,
//     AtomicInteger<T>, and Semaphore."
//   §28.13 v0.140: "For every other T, Shared<T> remains domain-local and not
//     sendable — the count synchronization alone would not make an unsynchronized T
//     safe to reach from two domains."
//   §30.14: argument types at Domain.Spawn must be sendable.
//   §30.15 / §3.6: Channel<T> element type must be sendable.
//
// Tests cover:
//   (a) Shared<Mutex>    as Domain.Spawn arg — accepted (positive / §28.13 v0.140).
//   (b) Shared<string>   as Domain.Spawn arg — rejected (not synchronized).
//   (c) Shared<UserClass>as Domain.Spawn arg — rejected (not synchronized).
//   (d) Channel<Shared<Mutex>> — accepted (§30.15 / §3.6 / §28.13 v0.140).
//   (e) Channel<Shared<string>> — rejected.
//   (f) Weak<Mutex>      as Domain.Spawn arg — rejected (Weak stays domain-local,
//         v0.140 only lifted Shared handles not Weak handles).
//   (g) Shared<Atomic<int>>   — isSendable returns true for name "Atomic"
//         but Atomic<T> is not yet implemented (unresolved type name); this
//         test is deferred until the Atomic<T> implementation lands.
//   (h) Shared<AtomicInteger<int>> — same deferral as (g).
//   (i) Shared<Semaphore> — same deferral; Semaphore type is not yet resolved.
//
// Deferred tests (g)/(h)/(i) are excluded because `new Atomic<int>()` and
// `new Semaphore()` produce "unresolved type name" errors before the sendability
// check is reached.  The isSendable code IS correct (names "Atomic", "AtomicInteger",
// "Semaphore" return sendable in the sync-set path), but the tests cannot run
// until those stdlib types are implemented in the sema/codegen layers.

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
// (a) Shared<Mutex> as Domain.Spawn argument — accepted per §28.13 v0.140.
//     Mutex is in the internally-synchronized set; the handle count is atomic.
// ---------------------------------------------------------------------------

// §28.13 v0.140 / §30.14: Shared<Mutex> passed as owned spawn argument → accepted.
BEZEL_TEST(shared_sync_sendable_689,
           shared_mutex_as_spawn_arg_accepted_28_13_v0140) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned h: Shared<Mutex>) "
        "    h.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        // new Mutex() inline — no using-var binding, so ownership transfers
        // directly into Shared<Mutex>; no move-out-of-using-var error.
        "    using var h := new Shared<Mutex>(new Mutex()); "
        "    using var t := Domain.Spawn(Worker.Run, h.Clone()); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Shared<Mutex> is sendable per §28.13 v0.140 — no sendability error.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (b) Shared<string> as Domain.Spawn argument — rejected per §28.13 v0.140.
//     string is NOT in the internally-synchronized set.
// ---------------------------------------------------------------------------

// §28.13 v0.140 / §30.14: Shared<string> is NOT sendable → rejected.
BEZEL_TEST(shared_sync_sendable_689,
           shared_string_as_spawn_arg_rejected_28_13_v0140) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned h: Shared<string>) "
        "    h.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var h := new Shared<string>(\"hi\"); "
        "    using var t := Domain.Spawn(Worker.Run, h.Clone()); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Shared<string> is NOT sendable — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (c) Shared<UserClass> as Domain.Spawn argument — rejected per §28.13 v0.140.
//     A user class is not in the internally-synchronized set.
// ---------------------------------------------------------------------------

// §28.13 v0.140 / §30.14: Shared<UserClass> is NOT sendable → rejected.
BEZEL_TEST(shared_sync_sendable_689,
           shared_userclass_as_spawn_arg_rejected_28_13_v0140) {
    auto r = parseSemaAll(
        "module M; "
        "class Counter "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Counter; "
        "class Worker "
        "  public static method Run(owned h: Shared<Counter>) "
        "    h.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var h := new Shared<Counter>(new Counter()); "
        "    using var t := Domain.Spawn(Worker.Run, h.Clone()); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Shared<Counter> is NOT sendable — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (d) Channel<Shared<Mutex>> — accepted per §28.13 v0.140 / §30.15 / §3.6.
//     Shared<Mutex> is sendable, so it satisfies the channel element constraint.
// ---------------------------------------------------------------------------

// §30.15 / §3.6 / §28.13 v0.140: Channel<Shared<Mutex>> accepted.
BEZEL_TEST(shared_sync_sendable_689,
           channel_shared_mutex_accepted_28_13_v0140_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Shared<Mutex>>(); "
        "  end Test; "
        "end C;");
    // Shared<Mutex> is sendable → Channel<Shared<Mutex>> is valid.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (e) Channel<Shared<string>> — rejected per §28.13 v0.140 / §30.15 / §3.6.
//     Shared<string> is NOT sendable.
// ---------------------------------------------------------------------------

// §30.15 / §3.6 / §28.13 v0.140: Channel<Shared<string>> rejected.
BEZEL_TEST(shared_sync_sendable_689,
           channel_shared_string_rejected_28_13_v0140_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Shared<string>>(); "
        "  end Test; "
        "end C;");
    // Shared<string> is NOT sendable — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// (f) Weak<Mutex> as Domain.Spawn argument — rejected per §28.13 v0.140.
//     v0.140 lifted only Shared (strong handles), NOT Weak.  Weak stays
//     domain-local regardless of T.
// ---------------------------------------------------------------------------

// §28.13 v0.140: Weak<Mutex> is NOT sendable → rejected.
BEZEL_TEST(shared_sync_sendable_689,
           weak_mutex_as_spawn_arg_rejected_28_13_v0140) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(owned w: Weak<Mutex>) "
        "    w.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var h := new Shared<Mutex>(new Mutex()); "
        "    using var w := h.Downgrade(); "
        "    using var t := Domain.Spawn(Worker.Run, move w); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Weak<Mutex> is NOT sendable per §28.13 v0.140 — sendability diagnostic expected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// Tests (g)/(h)/(i) for Shared<Atomic<T>>, Shared<AtomicInteger<T>>,
// Shared<Semaphore> are deferred until those stdlib types are implemented.
// The isSendable code at ExprTypecheck_Calls.cpp correctly returns true for
// bname == "Atomic", "AtomicInteger", "Semaphore" in the Shared<T> branch,
// but `new Atomic<int>(0)` / `new Semaphore(1)` currently produce
// "unresolved type name" errors before the sendability path is reached.
// When those types land, enable tests here following the same pattern as (a).
