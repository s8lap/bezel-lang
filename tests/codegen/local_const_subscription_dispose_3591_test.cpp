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
// an `owned`-returning call"), but sema does not yet enforce that rejection
// for `Subscribe()` specifically (a separate, pre-existing gap, NOT fixed
// here — filed as #3591's own residue). Because the rejection does not fire,
// codegen is handed exactly the shape the exemption claimed was unreachable.
//
// THREE ROWS, MEASURED (probes in /var/tmp/bezel-scratch/r12/c3-d5/):
//   1. IEVENT-FACE, const (the crash). `var face: IEvent<int> := bell.E;
//      const sub := face.Subscribe(...); sub.Dispose();` in a 200-iteration
//      loop. PRE-FIX: AddressSanitizer SEGV on address 0x4 (a small-integer
//      value misread as a vtable-slot pointer) on the FIRST iteration's
//      `.Dispose()` — matches the reviewer's repro exactly.
//   2. IEVENT-FACE, var (control). Byte-identical except `var` in place of
//      `const`. Clean before AND after this fix — the `var` fallback arm is
//      unchanged; this row pins that the fix does not alter it.
//   3. CONCRETE-FIELD, const (`_evt.Subscribe(...)` where `_evt:
//      EventSource<int>`, no IEvent<T> face). Measured clean on BOTH sides
//      (sema's primary isSubscriptionType check already recognizes this
//      shape without the LocalDecl/LocalConstDecl fallback, because
//      `isSubscribeCall`'s structural test only matches a direct `event`
//      member or an IEvent<T>/IAsyncEvent<T>-typed target — an
//      EventSource<T>-typed target is neither). Included per the review
//      request as a regression guard on the untouched sibling shape, not
//      because it crashes.
//
// Every row is a VALUE oracle, not a crash-signature oracle (§R12-G): each
// Fire(1) immediately before `.Dispose()` must land (handler still
// subscribed) and each Fire(1000) immediately after must NOT (handler
// unsubscribed) — over 200 iterations the running total must be exactly 200,
// never 200200 (disposal silently a no-op) nor any other value. A crash
// (pre-fix row 1) makes `runAotChecked` return `ran=false`, which the `run()`
// helper below turns into the sentinel "<did-not-run>" — distinct from every
// passing value, so the assertion fails either way without inspecting a
// signal or exit code. Expected values are "200\n" — `stdoutText` keeps its
// trailing newline (unlike `runAotOut`, which strips one; #runet-vs-exit).
//
// FAIL-WITHOUT / PASS-WITH (mechanically verified, §R12-H): built bezelc
// against the pre-fix `isSubscriptionExpr` (the committed `LOCALDECL-ONLY`
// exemption, `git stash` isolated to this one file, `ab779f8`'s tree) with
// `--sanitize=address`; row 1's program SEGVs (`AddressSanitizer: SEGV on
// unknown address 0x000000000004`), rows 2 and 3 stay clean. Restored the
// fix, rebuilt, relinked; all three rows clean with row 1's total == 200.

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

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

// Shared preamble: a Bell with an int event, and C's IEvent<int> face plumbing.
const char* kPre =
    "module M; "
    "class Bell "
    "  public event E: int; "
    "  public method Fire(v: int) E.Raise(v); end Fire; "
    "end Bell; ";

}  // namespace

// Row 1 (the crash): a `const` local bound to a stored IEvent<int> FACE's
// Subscribe() result, explicitly disposed, 200x. Fire(1) before Dispose must
// land; Fire(1000) after must not. Expected total: 200.
BEZEL_TEST(local_const_subscription_dispose_3591, ievent_face_const_loop_200) {
    BEZEL_EXPECT_EQ(run(std::string(kPre) +
        "class C "
        "  private owned _bell: Bell := new(); "
        "  public static Total: int := 0; "
        "  public method Loop() "
        "    var face: IEvent<int> := _bell.E; "
        "    var i := 0; "
        "    while i < 200 do "
        "      const sub := face.Subscribe(lambda(v: int) => do C.Total := C.Total + v; end); "
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

// Row 2 (control): byte-identical to row 1 except `var` in place of `const`.
// Must stay at total 200 — the `var` fallback arm this fix does not touch.
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

// Row 3 (regression guard, measured clean on both sides): a `const` local
// bound directly to a concrete EventSource<int> FIELD's Subscribe() result
// (no IEvent<T> face) — a sibling shape `isSubscribeCall`'s structural test
// does not recognize either way (it only matches a direct `event` member or
// an IEvent<T>/IAsyncEvent<T>-typed target), so sema's primary
// isSubscriptionType check is what disposes it correctly. Total: 200.
BEZEL_TEST(local_const_subscription_dispose_3591, concrete_eventsource_field_const_loop_200) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  private owned _evt: EventSource<int> := new(); "
        "  public static Total: int := 0; "
        "  public method Loop() "
        "    var i := 0; "
        "    while i < 200 do "
        "      const sub := _evt.Subscribe(lambda(v: int) => do C.Total := C.Total + v; end); "
        "      _evt.Raise(1); "
        "      sub.Dispose(); "
        "      _evt.Raise(1000); "
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

