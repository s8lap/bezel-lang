// Spec-derived regression test for issue #4399.
//
// THE BUG. When an outer lambda stores an inner closure in a local variable
// and the outer lambda invokes that inner closure after the captured local has
// been disposed, no diagnostic fires:
//
//     var inner := lambda(): int => r.Get();  // captures while Owned
//     var outer := lambda(): int => inner();  // outer invokes inner
//     r.Dispose();
//     _ := outer();                           // outer() -> inner() -> reads disposed r
//
// The compiler accepted this with zero diagnostics and ASan confirmed the
// use-after-free: a SIGSEGV inside `inner`'s `r.Get()` call.
//
// ROOT CAUSE. The #4355 fix (`scanCapturedOwnedConsumeExpr`) added a check
// that fires when a function-typed local holding a closure is invoked and
// any of its captured identifiers are in the Disposed state. However, the
// check is guarded by `!insideLambda`, so it only fires at the method top
// level, not inside a lambda body. When `outer()` is called at the top level,
// `insideLambda=false` so the check is reached, but `outer` itself captures
// nothing (only `inner` does). The `inner()` invocation happens INSIDE
// `outer`'s lambda body, where `insideLambda=true`, so the check is skipped.
// The result: no diagnostic fires even though `inner()` reads a disposed
// capture.
//
// SIBLING OF #4355. The #4355 fix covered the one-level case (closure stored
// in local, invoked at top level). This is the two-level case (outer lambda
// invokes inner closure stored in local, inner captures disposed variable).
//
// FIX. Remove the `!insideLambda` guard from the #4355 check. The check
// examines the IMMEDIATE callee's captures, not the transitive closure, so it
// is safe to fire inside a lambda body: we check whether `inner`'s captures
// are disposed when `inner()` is called inside `outer`'s body, which is the
// correct diagnostic location.
//
// Per §28.2 (use-after-dispose): "use-after-dispose is undefined behaviour in
// unchecked mode and raises DisposedAccessException in checked mode". Severity is
// Warning, matching checkRead's own disposed-identifier arm for a direct read.
//
// per §28.2, §28.3, v0.298, #4399

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::Severity;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// `R` is an ordinary owned class with a destructor — a genuine non-trivial
// disposal whose use-after-dispose is a real heap use-after-free.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  public method Get(): int return V; end Get; "
        "  destructor end destructor; "
        "end R; "
        "class C ") + body + " end C;";
}

// Check for the diagnostic: closure captures identifier which was disposed
// BEFORE THIS INVOCATION.
bool rejectsDisposedBeforeInvoke(const bezel::test::OwnershipResult& r,
                                 const char* name) {
    const std::string needle =
        std::string("closure captures `") + name +
        "` which was disposed before this invocation";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// POSITIVE: nested lambda invocation reads disposed capture.
// outer() -> inner() -> reads disposed r. The NEW gap this test pins.
BEZEL_TEST(nested_lambda_invoke_disposed_capture,
           outer_invokes_inner_inner_captures_disposed_rejected_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  using var r := new R(); "
        "  var inner := lambda(): int => r.Get(); "
        "  var outer := lambda(): int => inner(); "
        "  r.Dispose(); "
        "  _ := outer(); "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_TRUE(rejectsDisposedBeforeInvoke(r, "r"));
}

// CONTROL: dispose BEFORE both lambdas — existing creation-point check fires.
BEZEL_TEST(nested_lambda_invoke_disposed_capture,
           dispose_before_both_lambdas_still_rejected_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  using var r := new R(); "
        "  r.Dispose(); "
        "  var inner := lambda(): int => r.Get(); "
        "  var outer := lambda(): int => inner(); "
        "  _ := outer(); "
        "  return 0; "
        "end Run;"));
    // The existing creation-point check fires a Warning about the capture
    // at the inner lambda literal.
    bool hasCreationWarning = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == Severity::Warning &&
            d.message.find("closure captures `r` after a dominating explicit "
                           "Dispose() call") != std::string::npos) {
            hasCreationWarning = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(hasCreationWarning);
}

// CONTROL: no dispose between creation and invoke — should be clean.
BEZEL_TEST(nested_lambda_invoke_disposed_capture,
           no_dispose_between_creation_and_invoke_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  using var r := new R(); "
        "  var inner := lambda(): int => r.Get(); "
        "  var outer := lambda(): int => inner(); "
        "  _ := outer(); "
        "  _ := outer(); "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: outer captures something else, inner captures r — dispose r,
// invoke outer. The inner closure's capture should still be checked.
BEZEL_TEST(nested_lambda_invoke_disposed_capture,
           inner_captures_disposed_outer_passes_unrelated_borrow_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  using var r := new R(); "
        "  var inner := lambda(): int => r.Get(); "
        "  var other: int := 0; "
        "  var outer := lambda(): int => other + inner(); "
        "  r.Dispose(); "
        "  _ := outer(); "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_TRUE(rejectsDisposedBeforeInvoke(r, "r"));
}
