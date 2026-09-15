// Spec-derived regression test for issue #4355.
//
// THE BUG. When a closure is stored in a local and that closure is invoked
// after the captured local has been disposed, no diagnostic fires:
//
//     using var x := new Foo();
//     var c := lambda(): int => x.Get();  // captures while Owned
//     x.Dispose();                          // disposed AFTER creation
//     _ := c();                           // reads disposed x → no diagnostic
//
// The compiler accepted this with zero diagnostics and ASan confirmed the
// use-after-free: a SEGV reading a null pointer inside the closure's
// `Foo$Get` call.
//
// ROOT CAUSE. The existing captured-owned-checks fire only at the closure
// literal's CREATION point — `reportMovedFromCapturesIn` consults the
// statement-entry state. But `x` is Owned at the literal, so the check passes.
// The subsequent explicit `x.Dispose()` sets `x` to Disposed; the `c()`
// invocation reads `x` inside the closure body, but the ownership pass has no
// check at the INVOKE point of a closure stored in a local.
//
// EXISTING COVERAGE. `reportMovedFromCapturesIn`'s Disposed leg fires when
// the local is already Disposed at the CLOSURE LITERAL (e.g. `x.Dispose();
// var c := lambda(): int => x.Get()` — "closure captures `x` after a
// dominating explicit Dispose() call"). This is NOT the same case: here the
// dispose happens AFTER the closure is created.
//
// FIX. `scanCapturedOwnedConsumeExpr` now checks, when a function-typed
// local is invoked at the method top level (not inside another lambda), whether
// any of its captured identifiers are in the Disposed state. The check uses
// the same `scopedLambdaLocalInit_` map already maintained for the
// §28.14 scoped-call analysis.
//
// Per §28.2 (use-after-dispose): "use-after-dispose is undefined behaviour in
// unchecked mode and raises DisposedAccessException in checked mode". Severity is
// Warning (matching checkRead's own disposed-identifier arm) because the
// violation is statically decidable but the runtime either handles it
// gracefully (checked) or crashes (unchecked).
//
// per §28.2, §28.3, v0.298, #4355

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

// Check for the new diagnostic: closure captures identifier which was disposed
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

// POSITIVE: dispose AFTER closure creation, BEFORE invoke → should warn.
// The NEW gap this test pins.
BEZEL_TEST(closure_invoke_reads_disposed_capture,
           disposed_after_creation_before_invoke_rejected_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  using var x := new R(); "
        "  var c := lambda(): int => x.Get(); "
        "  x.Dispose(); "
        "  _ := c(); "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_TRUE(rejectsDisposedBeforeInvoke(r, "x"));
}

// CONTROL: no dispose between creation and invoke → should be clean.
// The closure captures `x` while Owned and is invoked while `x` is still
// alive; no warning.
BEZEL_TEST(closure_invoke_reads_disposed_capture,
           no_dispose_between_creation_and_invoke_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  using var x := new R(); "
        "  var c := lambda(): int => x.Get(); "
        "  _ := c(); "
        "  _ := c(); "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: dispose BEFORE creation → existing check fires at creation point.
// This is the already-covered case from #3771 / §28.2; pinned so a future
// change that affects the creation-point check doesn't silently regress this.
BEZEL_TEST(closure_invoke_reads_disposed_capture,
           dispose_before_creation_still_rejected_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  using var x := new R(); "
        "  x.Dispose(); "
        "  var c := lambda(): int => x.Get(); "
        "  _ := c(); "
        "  return 0; "
        "end Run;"));
    // The existing creation-point check fires a Warning about the capture
    // at the literal, not at the invoke.
    bool hasCreationWarning = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == Severity::Warning &&
            d.message.find("closure captures `x` after a dominating explicit "
                           "Dispose() call") != std::string::npos) {
            hasCreationWarning = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(hasCreationWarning);
}
