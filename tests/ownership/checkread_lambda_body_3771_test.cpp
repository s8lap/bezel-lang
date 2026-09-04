// Spec-derived tests for issue #3771 — §28.3 "Captured owned bindings" /
// §27.5 "Closures" / §28.2 "Dispose" (v0.287).
//
// Normative text this file pins, quoted verbatim:
//
//   §28.3 (*Captured owned bindings*): "Inside a closure, a captured owned
//   binding — an owned local or `owned` parameter of the enclosing body — is
//   uniformly in **borrow state**: reads borrow (the producers rows above),
//   and assignment to it, a marked transfer from it, an explicit `Dispose()`
//   or discard of it, and every other owned-consuming use of it are compile
//   errors".
//
//   §28.3 (Owned ⊓ MovedFrom join, #1626): "A join of owned with moved-from
//   ... yields maybe-consumed ... the variable may not be read, borrowed from,
//   or consumed after the join".
//
//   §28.3 (Owned ⊓ Disposed join, #1883): "set on the Owned record resulting
//   from an Owned ⊓ Disposed join ... a READ after the join would operate on
//   a disposed object on the disposed-branch path (use-after-dispose, §28.2)".
//
//   §28.2 (use-after-dispose): "use-after-dispose is undefined behaviour in
//   unchecked mode and raises DisposedAccessException in checked mode".
//
// fix #3755 (commit e36521c) wired ONE leg of `checkRead`'s IdentifierExpr
// arm — MovedFrom — into the lambda-body descent, so `var q := move p;
// this.h := lambda(): int => p.V();` is now rejected. The OTHER legs of the
// same arm were left silent in the captured form:
//
//   A. maybe-consumed (Owned ⊓ MovedFrom join, error) — SILENT captured.
//   B. explicit Dispose (same-frame use-after-dispose, warning) — SILENT
//      captured.
//   C. disposed-on-some-path (Owned ⊓ Disposed join, error) — SILENT
//      captured.
//
// This file pins the fix: each captured-form row FAILS without the fix and
// PASSES with it. The non-lambda control (Row 4) pins that the direct form
// the existing checker already rejects is still rejected — no regression.

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

using bezel::Severity;
using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// `R` is an ordinary owned class with a destructor, so its disposal is a real
// event and a read after the move or dispose is a genuine read of freed
// storage. `Sink` is the standard move sink.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; import System; "
        "class R "
        "  private v: int := 5; "
        "  public method V(): int return this.v; end V; "
        "  destructor end destructor; "
        "end R; "
        "class Sink "
        "  public static method Eat(r: owned R): int return r.V(); end Eat; "
        "end Sink; "
        "class Box "
        "  public h: (() -> int)?; "
        ) + body +
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// Row 1 (maybe-consumed capture, cross-branch join). Distinct wording from
// the MovedFrom capture row so a row cannot pass on the wrong one.
bool rejectsMaybeConsumedCapture(const OwnershipResult& r, const char* name) {
    return hasErrorMessage(r, std::string("closure captures `") + name +
                                  "` whose owned value may have already been "
                                  "consumed on another branch");
}

// Row 2 (explicit Dispose capture, same-frame). §28.2 is a WARNING severity
// (use-after-dispose is statically decidable here, the runtime checked mode
// handles the error path) so `hasErrorMessage` (which is Error-only) cannot
// match it. Inline a Warning-aware check.
bool rejectsDisposeCapture(const OwnershipResult& r, const char* name) {
    const std::string needle =
        std::string("closure captures `") + name +
        "` after a dominating explicit Dispose() call";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasWarningMessage(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// --- Row 1: maybe-consumed capture (cross-branch Owned ⊓ MovedFrom) -------

// The issue's repro A: a maybe-consumed local (`p` is moved into `Sink.Eat`
// on one branch, left Owned on the other) captured by a closure literal. The
// DIRECT form (`_ := p.V();` after the if) is rejected with the §28.3
// maybe-consumed diagnostic; the CAPTURED form (lambda literal holding
// `p.V()`) was silently accepted on the parent (r12-cycle-1 = eb0cf1b) —
// the only remaining message was an unrelated auto-drop note. Post-fix, the
// captured form is rejected with the rule-specific capture wording.
BEZEL_TEST(checkread_lambda_body_3771, maybe_consumed_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run(flag: bool) "
        "  var p := new R(); "
        "  if flag then _ := Sink.Eat(move p); end; "
        "  var f := lambda(): int => p.V(); "
        "  _ := f(); "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsMaybeConsumedCapture(r, "p"));
}

// --- Row 2: explicit Dispose capture (same-frame use-after-dispose) --------

// The issue's repro B: a local disposed with `.Dispose()` then captured by a
// closure literal. The DIRECT form (`_ := p.V();` after the dispose) is
// rejected with the §28.2 warning; the CAPTURED form (`var f := lambda():
// int => p.V(); _ := f();`) compiled rc=0 on the parent with zero
// diagnostics. Post-fix, the captured form is rejected with the §28.2
// warning wording AND a Note pointing at the dispose site (mirroring
// checkRead's own warning).
BEZEL_TEST(checkread_lambda_body_3771, dispose_capture_rejected_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  p.Dispose(); "
        "  var f := lambda(): int => p.V(); "
        "  _ := f(); "
        "end Run; "));
    BEZEL_EXPECT_TRUE(rejectsDisposeCapture(r, "p"));
    // The Note pinning the dispose site (mirroring checkRead's own warning
    // Note) must accompany the warning.
    bool hasNote = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == Severity::Note &&
            d.message.find("explicit Dispose() of `p` was here") !=
                std::string::npos) {
            hasNote = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(hasNote);
}

// --- Row 3: MovedFrom capture (control — pre-existing wiring) --------------

// The MovedFrom leg was wired by #3755. This row pins that the fix #3771
// did not regress it: the diagnostic still fires for the same-frame
// MovedFrom capture. Distinct wording from Row 1 — the Row 3 wording names
// "already moved out at this point", Row 1 names "may have already been
// consumed on another branch".
BEZEL_TEST(checkread_lambda_body_3771, moved_from_capture_still_emits_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(p: owned R) "
        "  var q := move p; "
        "  this.h := lambda(): int => p.V(); "
        "  _ := move q; "
        "end Take; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("closure captures `p` whose owned value was already "
                       "moved out at this point")));
}

// --- Row 3b: disposed-on-some-path capture (cross-branch Owned ⊓ Disposed)

// The "third" leg the brief mentions — distinct from the maybe-consumed
// (MovedFrom-split) row above. `p` is explicitly Dispose()d on one branch
// (without a prior move — the disposal is the OWNED ⊓ DISPOSED join, NOT a
// transfer-out), left Owned on the other. The DIRECT form (`_ := p.V();`)
// is rejected by checkRead's existing §28.3 #1883 leg with the
// "conditional-discharge join" wording; the CAPTURED form was silently
// accepted on the parent. Post-fix, the captured form is rejected with the
// rule-specific capture wording.
BEZEL_TEST(checkread_lambda_body_3771,
           disposed_on_some_path_capture_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run(flag: bool) "
        "  var p := new R(); "
        "  if flag then p.Dispose(); end; "
        "  var f := lambda(): int => p.V(); "
        "  _ := f(); "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("closure captures `p` whose owned value may have "
                       "already been disposed on another branch")));
}

// --- Row 4: direct (non-lambda) read — regression control -----------------

// The DIRECT form of Row 1 — a maybe-consumed local read directly (not via a
// lambda capture). This is the diagnostic checkRead's IdentifierExpr arm
// already reports; the fix must not change that. Without the regression
// guard a future change that "fixes" the captured form by deleting the
// direct form's diagnostic would pass this file.
BEZEL_TEST(checkread_lambda_body_3771, direct_maybe_consumed_still_emits_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run(flag: bool) "
        "  var p := new R(); "
        "  if flag then _ := Sink.Eat(move p); end; "
        "  _ := p.V(); "
        "  p.Dispose(); "
        "end Run; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, std::string("local `p` may have already been consumed on another "
                       "branch")));
}

// The DIRECT form of Row 2 — a disposed local read directly. Same regression
// discipline as the maybe-consumed row above.
BEZEL_TEST(checkread_lambda_body_3771, direct_dispose_still_emits_28_2) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Run() "
        "  var p := new R(); "
        "  p.Dispose(); "
        "  _ := p.V(); "
        "end Run; "));
    BEZEL_EXPECT_TRUE(hasWarningMessage(
        r, std::string("use of `p` after a dominating explicit Dispose() "
                       "call")));
}
