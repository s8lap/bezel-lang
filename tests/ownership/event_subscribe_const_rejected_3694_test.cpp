// Ownership / diagnostics — §21.1's runtime-`const` owned-production rejection
// was MISSING for the §24.1 / §6.5 `.Subscribe(...)` call family. A `const
// local := face.Subscribe(...)` accepted silently (rc 0, no diagnostic) and
// the returned `owned Subscription` handle (stdlib §2.11 / §6.5) leaked
// because nothing disposed it. The shape recurrence of the landed #2709 for
// the six builtin collection materializers: `producerState`'s
// `md->returnOwned` arm (the only CallExpr-side Owned authority) keys on a
// user `MethodDecl`, but `Subscribe` is a COMPILER-KNOWN member on
// `IEvent<T>` / `IAsyncEvent<T>` / `ISharedEvent<T>` / `EventSource<T>` /
// `AsyncEventSource<T>` / `SharedEventSource<T>` with no user `MethodDecl`,
// so producerState cannot see it.
//
// Spec, v0.286:
//   §21.1 "Runtime `const` locals", the **Ownership** paragraph, verbatim:
//     "The initializer of a runtime `const` must not be an **owned production**
//      (§28.3): the binding never takes a disposal obligation and never holds an
//      affine (consumable) value. […] Rejected, with a diagnostic naming this
//      rule and the `using var` remedy: a fresh `new`, an `owned`-returning
//      call, a `Task`-family call, a deferred-activation producer (a lazy query,
//      `Drain()`, `ReceiveAll()` — affine values, §29.11), a `move`, a marked
//      transfer (`const y := owned x;` is not a form), and a move-only value
//      (§15.3)."
//   stdlib §2.11 "Subscription", verbatim: "`Subscription` is returned from
//     `IEvent<T>.Subscribe` (§6.5), from `IAsyncEvent<T>.Subscribe` (§6.5),
//     from `CancellationToken.Register` (§6.1), and from analogous register-
//     and-unregister APIs. Methods returning a `Subscription` annotate the
//     return type as `owned Subscription` so that the caller's leak-prevention
//     obligation is explicit."
//   stdlib §6.5 "Events: IEvent<T>, EventSource<T>", verbatim: `Subscribe`
//     `returns an owned Subscription` on `IEvent<T>` / `IAsyncEvent<T>` /
//     `EventSource<T>` / `AsyncEventSource<T>` / `SharedEventSource<T>`.
//
// So every `.Subscribe(...)` call on one of those receivers IS "an
// `owned`-returning call", and §21.1 REQUIRES its rejection as a `const`
// initializer.
//
// THE BUG, measured on 2a990de (c4-d4 lane review, 2026-09-03):
//
//   using var src := new EventSource<int>();
//   var face: IEvent<int> := src;
//   const sub := face.Subscribe(lambda(v: int) => do end);
//
// → ACCEPTED, rc 0, no diagnostic. Then 500 iterations of the same program
// in a loop leak 16000 B (32 B per Subscription handle — measured by the #3591
// lane review that originally surfaced this on the `Subscribe()` member).
// Same shape as #2709: producerState's `md->returnOwned` arm never fires for
// compiler-known members, no user `MethodDecl` exists, so the §21.1 rejection
// falls through.
//
// THE FIX, and why it is NOT a reclassification. `producerState`'s `Borrowed`
// verdict here is load-bearing for the bindable spellings — `var sub` and
// `using var sub` are already reclaimed by codegen (`Emitter::isSubscriptionExpr`
// for plain `var`, `registerUsingDisposal` for `using var`). Flipping it to
// `Owned` would register a SECOND free on top — the double-free direction
// (#2709's exact decline). Instead §21.1's const rejection routes through the
// SAME predicate as the materializer / drain / range-slice cases,
// `MethodOwnership::constInitializerRejected`, whose fifth disjunct is the new
// `common::isEventSubscribeCall`. No disposal, retain, or dataflow state
// changes; the generated code for every program that still compiles is what
// it was. The leak goes away because §21.1 removes the program.
//
// The same routing subsumes codegen's `Emitter::isSubscribeCall` structural
// test (LlvmBackend_Calls2.cpp:3847) on the recognition clauses (member name,
// arity, receiver type / EventSymbol) and WIDENS it with the producer-side
// sealed classes (`EventSource<T>` / `AsyncEventSource<T>` /
// `SharedEventSource<T>`) plus the `ISharedEvent<T>` face (#1832) — all six
// heads are the same `owned Subscription` return (stdlib §6.5), so the wider
// recognition is required for completeness. The declaration-less gate is
// what excludes a user class that happens to share one of those names: such
// a type carries a declaration and reaches the user-method authority
// instead.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// §21.1's NEW Subscribe-rejection wording. Matching the SPECIFIC text (not
// just "must not take an owned production", which the pre-existing `new`-RHS
// and materializer branches also emit) is what pins this fix rather than
// #2709's or #2749's.
bool hasConstSubscribeError(const OwnershipResult& r,
                            const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take an owned production") ==
            std::string::npos)
            continue;
        if (d.message.find("`.Subscribe(...)` returns a fresh `owned` "
                           "Subscription") == std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// `const`-reject fragment matches the §21.1 wording — the substring is a
// STABLE rule identifier (§-reference + a key term from the message) so
// diagnostic-wording tweaks that rephrase the sentence do not silently break
// the test (cf. `hasErrorMessage`'s Issue-2988 rationale).
const char* kRejectFragment = "§21.1 / §28.3";

// =====================================================================
// ROW 1 — THE BUG. A `const` local bound to an `IEvent<int>`-typed FACE's
// `.Subscribe(...)` result. PRE-FIX: accepted silently. POST-FIX: rejected
// at the declaration with §21.1's specific Subscribe wording.
// FAILS WITHOUT THE FIX (the const branch is silently accepted; no error).
// =====================================================================
BEZEL_TEST(event_subscribe_const_rejected_3694,
           ievent_face_const_rejected_spec_21_1_stdlib_2_11) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new EventSource<int>(); "
        "    var face: IEvent<int> := src; "
        "    const sub := face.Subscribe(lambda(v: int) => do end); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstSubscribeError(r, "sub"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kRejectFragment));
}

// =====================================================================
// ROW 2 — CONTROL. Byte-identical to row 1 except `var` in place of
// `const`. The `var` form is reclaimed by codegen's plain-`var` Subscription
// path (Emitter::isSubscriptionExpr, the LocalDecl arm) — pre- and post-fix
// identical, must keep compiling CLEAN. FAILS WITHOUT THE FIX only if the
// fix over-fires; the pre-fix tree compiles cleanly too.
// =====================================================================
BEZEL_TEST(event_subscribe_const_rejected_3694,
           ievent_face_var_compiles_clean_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new EventSource<int>(); "
        "    var face: IEvent<int> := src; "
        "    var sub := face.Subscribe(lambda(v: int) => do end); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// ROW 3 — CONTROL. Same shape with `using var` — the canonical disposal
// spelling for an `owned Subscription`. Pre- and post-fix identical, must
// compile clean and LSan-clean on runAotChecked (verified via 500-iteration
// amplification, the §R12-H discipline the #3591 fixture used). The
// VALUE oracle prints "500" — the loop counter. FAILS WITHOUT THE FIX
// only if the fix over-fires; this row is the regression guard on the
// bindable spellings, not the bug pin.
// =====================================================================
BEZEL_TEST(event_subscribe_const_rejected_3694,
           ievent_face_using_var_runsaotclean_500) {
    auto src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new EventSource<int>(); "
        "    var face: IEvent<int> := src; "
        "    var i := 0; "
        "    while i < 500 do "
        "      using var sub := face.Subscribe(lambda(v: int) => do end); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(\"500\"); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "500\n");
}

// =====================================================================
// ROW 4 — DIRECT EVENT-MEMBER path. `bell.E.Subscribe(handler)` where
// `bell.E` is a §24.1 event access carrying an `EventSymbol`. The fix's
// first recognition clause (EventSymbol-bearing target). FAILS WITHOUT THE
// FIX the same way row 1 does.
// =====================================================================
BEZEL_TEST(event_subscribe_const_rejected_3694,
           direct_event_member_const_rejected_spec_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Bell "
        "  public event E: int; "
        "end Bell; "
        "class C "
        "  public static method Main(): int "
        "    using var bell := new Bell(); "
        "    const sub := bell.E.Subscribe(lambda(v: int) => do end); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstSubscribeError(r, "sub"));
}

// =====================================================================
// ROW 5 — IAsyncEvent<T> face. The async sibling of row 1. Same shape,
// same wording. FAILS WITHOUT THE FIX.
// =====================================================================
BEZEL_TEST(event_subscribe_const_rejected_3694,
           iasyncevent_face_const_rejected_spec_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new AsyncEventSource<int>(); "
        "    var face: IAsyncEvent<int> := src; "
        "    const sub := face.Subscribe(lambda(v: int) => do end); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstSubscribeError(r, "sub"));
}

// =====================================================================
// ROW 6 — SharedEventSource<T> producer-side sealed class (stdlib §6.5,
// #1832). The shared-event pair does not extend IEvent<T> by design — the
// fix widens the receiver-type gate to include this head, and the ISharedEvent<T>
// face as well, so the §21.1 half matches codegen's reclamation coverage.
// FAILS WITHOUT THE FIX.
// =====================================================================
BEZEL_TEST(event_subscribe_const_rejected_3694,
           shared_source_const_rejected_spec_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    const sub := src.Subscribe(lambda(v: int) => do end); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstSubscribeError(r, "sub"));
}

// =====================================================================
// ROW 7 — FALSE-REJECT GUARD. A USER class with a `Subscribe` member
// (NOT an event-family receiver, NOT a `Subscription` return) must keep
// compiling clean under §21.1's "Admitted: borrows" rule. A fix keyed on
// the member NAME alone passes rows 1, 4, 5, 6 and fails here. The fix is
// gated on the receiver type being one of the six event-family heads OR an
// `EventSymbol`-bearing target — neither of which a plain user class reaches.
// =====================================================================
BEZEL_TEST(event_subscribe_const_rejected_3694,
           user_class_with_subscribe_member_admissible_spec_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public method Subscribe(handler: (int) -> ()): int return 7; "
        "    end Subscribe; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    const v := b.Subscribe(lambda(n: int) => do end); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstSubscribeError(r, "v"));
}

}  // namespace