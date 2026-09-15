// Spec-derived regression test for issue #4175 — the `return mk()` indirect
// escape check silently drops a captured binding disposed before the call.
//
// The issue is a narrow sibling of #4156 (which fixed the bare-identifier
// `return f;` shape). #4156's fix added a Disposed-state arm to
// `checkIndirectEscapeOnReturn`, keyed on `isDisposedTrackedLocal`, which
// re-checks the returned holder's captured source's live state at the return.
// But `checkIndirectEscapeOnReturn`'s guard only fires on a bare identifier
// (`peelBindAtomToBody` must resolve a `sym`); for `return mk()` the callee
// is a CallExpr, so no `sym` is ever resolved and the new arm never runs.
//
// The third escape path — `checkClosureEscapesOnReturn`'s direct-lambda-literal
// arm — was investigated during #4156's fix and found to already be covered
// by `reportMovedFromCapturesIn`'s #3771 Disposed leg (a per-statement check
// at the literal's creation point, which for a directly-embedded literal
// coincides with the return). But the `return mk()` CallExpr indirection was
// NOT covered by that mechanism either.
//
// Root cause (per issue #4175's own analysis): the `return mk()` CallExpr
// arm in `checkClosureEscapesOnReturn`'s `findLambdas` walks into `mk`'s
// initializer lambda to find the inner literal (`lambda() => entry.Key.Length`).
// When `checkOneLambdaBodyEscapes` processes that inner lambda, it re-seeds
// `states_` with ONLY the lambda body's own owned obligations. `entry` — a
// for-loop variable of the OUTER method, not the lambda's body — is not in
// that re-seeded map. The main loop's per-captured-identifier check then
// looks up `entry` in the WRONG `states_` (the lambda-body map, not the
// method map), finds nothing, and silently skips the diagnostic.
//
// The fix resolves the captured identifier's state from the METHOD's
// `states_` (saved before the lambda-body walk begins) when the
// `CallExpr`-indirection path is in play, rather than from the
// lambda-body-re-seeded `states_` that `checkOneLambdaBodyEscapes` leaves
// behind.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Widget mirrors the stdlib §4.9 `OwningDictionary<K,V>` / `DrainedEntry<K,V>`
// fixture that issue #4136/#4156/#4197 all share.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "public class Widget "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Widget; "
        "class C ") + method + " end C;";
}

}  // namespace

// POSITIVE (§28.2 / §28.3, #4175): the core fix. `mk` is a function-typed
// local whose initializer is `lambda(): () -> int => lambda() => entry.Key.Length`
// — the outer lambda wraps the inner one. `entry` is disposed BEFORE `mk()`
// is called, but the inner lambda still captures `entry` and escapes via the
// outer `return mk()`. Before the fix: zero diagnostics. After the fix: Error
// at the `return mk();` line.
//
// NOTE: `entry` is a `using var` local (not a for-loop variable) because
// `OwningDictionary.Drain()` is NOT an owning enumerable — `iterableIsOwningEnumerable`
// returns false for it — so the loop variable would NOT be seeded into `states_`
// by `seedOwnedIterVar` and the check would never run. A `using var` entry IS
// tracked, making it a valid fixture for this test.
BEZEL_TEST(return_mk_indirection_captured_disposed_escape,
           mk_call_captures_disposed_entry_rejected_4175) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): () -> int "
        "  using var entry := new Widget(9); "
        "  var mk := lambda(): () -> int => lambda() => entry.N; "
        "  entry.Dispose(); "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `entry`, which was explicitly disposed "
           "before this return"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "(§28.2 dominating-Dispose rule, extended to a capture-escaping "
           "binding per §28.3"));
}

// CONTROL (§28.9, pre-existing, unaffected by this fix): the SAME shape but
// `entry` is NEVER disposed — it falls into the pre-existing "still Owned"
// arm of `checkClosureEscapesOnReturn`'s §28.9 leg, which fires regardless
// of the CallExpr indirection. Pinned so the fix cannot silently merge the
// Disposed and Owned arms.
BEZEL_TEST(return_mk_indirection_captured_disposed_escape,
           mk_call_captures_never_disposed_still_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): () -> int "
        "  using var entry := new Widget(9); "
        "  var mk := lambda(): () -> int => lambda() => entry.N; "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "whose source is disposed at scope end"));
}

// NEGATIVE (§28.9, accept-side sweep): `entry` IS disposed before `mk()`,
// but the inner lambda does NOT capture `entry` — it captures an unrelated
// parameter. Must stay accepted. The Disposed check must not fire for a
// captured identifier that is NOT the disposed one.
BEZEL_TEST(return_mk_indirection_captured_disposed_escape,
           mk_call_captures_unrelated_param_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(other: int): () -> int "
        "  using var entry := new Widget(9); "
        "  var mk := lambda(): () -> int => lambda() => other + 1; "
        "  entry.Dispose(); "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§28.9, accept-side sweep): `mk()` is called but its RESULT is
// DISCARDED (not returned). The inner lambda is consumed by the call and
// never escapes. Must stay accepted.
BEZEL_TEST(return_mk_indirection_captured_disposed_escape,
           mk_call_result_discarded_no_escape_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): int "
        "  using var entry := new Widget(9); "
        "  var mk := lambda(): () -> int => lambda() => entry.N; "
        "  entry.Dispose(); "
        "  var _unused := mk(); "
        "  return 0; "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (parity with #4156 accept-side): the inner lambda is invoked
// INSIDE the loop body (synchronously, before Mk returns). The result of
// THAT call — a plain int — is what gets returned, not the closure itself.
// The inner lambda does not escape and `entry.Dispose()` is safe.
BEZEL_TEST(return_mk_indirection_captured_disposed_escape,
           mk_call_inner_invoked_before_dispose_result_returned_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): int "
        "  using var entry := new Widget(9); "
        "  var mk := lambda(): () -> int => lambda() => entry.N; "
        "  var v := mk()(); "
        "  entry.Dispose(); "
        "  return v; "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
