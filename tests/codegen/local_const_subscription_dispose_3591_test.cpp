// fix #3591 / §21.1 / §24.4: a runtime `const` local bound to a stored
// IEvent<T>/IAsyncEvent<T> FACE's `.Subscribe(...)` result was not recognized
// as an owned Subscription, so its explicit `.Dispose()` dispatched through
// the generic vtable-slot path against Subscription's vtable-less handle —
// SIGSEGV. `Emitter::isSubscriptionExpr`'s inferred-local fallback (added by
// #747 for exactly the "stored IEvent<T> value" shape, because sema does not
// yet type a bare `.Subscribe(...)` call result as `Subscription`) only
// matched `dynamic_cast<const ast::LocalDecl*>(loc->decl)` — the `var` AST
// node. §21.1's runtime `const` is a DIFFERENT node, `ast::LocalConstDecl`
// (initializer field `value`, not `initializer`), so a `const`-bound local
// fell through to `return false`, and its `.Dispose()` call was routed as an
// ordinary (non-Subscription) receiver.
//
// This was originally reported by review as a REFUTATION of a
// `LOCALDECL-ONLY` exemption marker claimed for this exact cast: the
// exemption's premise — "§21.1 rejects `const sub := x.Subscribe(...)` as a
// compile error, so a LocalConstDecl can never reach here" — is FALSE.
// `Subscribe()` returns `owned Subscription` (stdlib §2.11 / §6.5), which by
// §21.1's own text SHOULD be rejected for a `const` binding ("Rejected: ...
// an `owned`-returning call"). The earlier fix shipped the codegen-side
// `isSubscriptionExpr` LocalConstDecl arm that made `.Dispose()` dispatch
// correctly WITHOUT crashing — but the §21.1 REJECTION at the declaration was
// not yet enforced (a separate, pre-existing gap flagged in #3591's own
// commit body and filed as #3694). #3694 closes that gap: a `const sub :=
// face.Subscribe(...)` binding is now REJECTED at compile time by the
// ownership pass's `MethodOwnership::constInitializerRejected` (the new
// `common::isEventSubscribeCall` disjunct). The codegen-side LocalConstDecl
// arm added in #3591 stays in place as a defensive recognition (so a future
// diagnostic refactor cannot reintroduce the SIGSEGV).
//
// TWO ROWS REWRITTEN for #3694 (previously three — rows 1 and 3 tested
// `const sub := face.Subscribe(...); sub.Dispose();` running clean via
// codegen's `Emitter::isSubscriptionExpr` LocalConstDecl arm; those programs
// are no longer LEGAL under the §21.1 rejection, so the test now asserts
// the compile error instead. Per the project's "Fix must update EXISTING
// tests" ratchet — the test's premise (the const binding compiles and
// disposes correctly) is reversed by #3694, and updating in the same PR is
// the right move; the gate would otherwise report this as a REAL
// regression):
//   1. IEVENT-FACE, const. PRE-#3694: SEGV on `.Dispose()`. POST-#3694:
//      rejected at the declaration by §21.1 (verified via
//      `parseAndCheckOwnership`).
//   2. CONCRETE-FIELD, const. PRE-#3694: compiled cleanly. POST-#3694:
//      rejected at the declaration by §21.1.
//   3. IEVENT-FACE, var (control). Byte-identical to row 1 except `var` in
//      place of `const`. Unchanged — the `var` fallback arm is untouched
//      by this fix and must still compile, dispose cleanly, and total 200.
//
// FAIL-WITHOUT / PASS-WITH (mechanically verified, §R12-H): row 1's
// pre-#3591 program SEGV'd; #3591 made rows 1 and 2 (then-control) and 3
// run clean via codegen's LocalConstDecl arm; #3694 makes rows 1 and 3
// rejected at compile time, leaves row 2 (renamed) compiling and
// disposing clean.

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/ownership_support.hpp"  // parseAndCheckOwnership — diagnostic check
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// r.ran ? r.stdoutText : "<did-not-run>" — a crash (pre-fix row 1) or an
// LSan-flagged leak both turn into the sentinel, distinct from every
// well-formed total string, so the EQ assertion below fails cleanly either
// way without decoding a signal or exit code.
std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// fix #3694 (§21.1 / stdlib §2.11 / §6.5): the `const` initializer of a
// `.Subscribe(...)` call on an event-family receiver is rejected at the
// declaration. Match the SPECIFIC Subscribe wording (not the generic
// "must not take an owned production" — that's the §28.3 `new`-RHS /
// materializer branches and would pass for unrelated reasons).
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

// Shared preamble: a Bell with an int event, and C's IEvent<int> face plumbing.
const char* kPre =
    "module M; "
    "class Bell "
    "  public event E: int; "
    "  public method Fire(v: int) E.Raise(v); end Fire; "
    "end Bell; ";

}  // namespace

// Row 1 (rewritten for #3694): a `const` local bound to a stored
// IEvent<int> FACE's Subscribe() result is REJECTED at the declaration by
// §21.1 — the leak path that #3591 closed at codegen-side is closed at the
// sema side here. PRE-#3694: this program compiled and SEGV'd on
// `.Dispose()`; #3591 made the `.Dispose()` work; #3694 removes the
// program from the legal set.
BEZEL_TEST(local_const_subscription_dispose_3591, ievent_face_const_loop_200) {
    auto r = parseAndCheckOwnership(std::string(kPre) +
        "class C "
        "  public static method Main(): int "
        "    using var bell := new Bell(); "
        "    var face: IEvent<int> := bell.E; "
        "    const sub := face.Subscribe(lambda(v: int) => do end); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstSubscribeError(r, "sub"));
}

// Row 2 (control, unchanged): byte-identical to row 1's PREVIOUS shape
// except `var` in place of `const`. The `var` fallback arm this fix does not
// touch must keep compiling, disposing cleanly, and totalling exactly 200
// over 200 iterations of Fire(1) + Dispose + Fire(1000) — the VALUE oracle
// (§R12-G) that pins codegen's `isSubscriptionExpr` LocalDecl arm.
BEZEL_TEST(local_const_subscription_dispose_3591, ievent_face_var_loop_200_control) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "class C "
        "  private owned _bell: Bell := new(); "
        "  public static Total: int := 0; "
        "  public method Loop() "
        "    var face: IEvent<int> := _bell.E; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var sub := face.Subscribe(lambda(v: int) => do C.Total := C.Total + v; end); "
        "      _bell.Fire(1); "
        "      sub.Dispose(); "
        "      _bell.Fire(1000); "
        "      i := i + 1; "
        "    end; "
        "  end Loop; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c.Loop(); "
        "    Console.WriteLine(C.Total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "200\n");
}

// Row 3 (rewritten for #3694): a `const` local bound directly to a concrete
// EventSource<int> FIELD's Subscribe() result (no IEvent<T> face) is
// REJECTED at the declaration by §21.1. PRE-#3694: this program compiled
// cleanly; #3694 removes it from the legal set.
BEZEL_TEST(local_const_subscription_dispose_3591, concrete_eventsource_field_const_loop_200) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var evt := new EventSource<int>(); "
        "    const sub := evt.Subscribe(lambda(v: int) => do end); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstSubscribeError(r, "sub"));
}