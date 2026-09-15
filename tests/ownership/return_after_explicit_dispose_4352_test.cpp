// Spec-derived regression test for issue #4352.
//
// THE BUG. After an explicit `b.Dispose();`, the `b` symbol transitions to
// the Disposed state (the slot is freed and nulled). A subsequent
// `return b;` then reads the null/dangling slot and returns a freed pointer —
// a use-after-free. ASan confirmed: SEGV at offset 12 (null pointer + field
// offset) on the unfixed build.
//
// The unfixed compiler silently accepted `b.Dispose(); return b;` with no
// diagnostic. `handleReturn`'s bare-identifier fast-path called
// `markDisposedByName(b, ...)` which, on a Disposed state, emits the
// idempotent §28.2 Warning ("the second `.Dispose()` is a no-op") — but
// `return b;` is NOT a second dispose, it is a USE of the already-disposed
// variable, and the warning is wrong on two counts: (a) it diagnoses the
// wrong site shape, and (b) it leaves the use-after-free unflagged.
//
// EXISTING COVERAGE. `markDisposedByName`'s Disposed leg is correct for the
// genuinely idempotent case `b.Dispose(); b.Dispose();` (warn, don't error,
// preserve the §28.2 idempotency guarantee). The bug is solely the
// bare-identifier return fast-path's interaction with a prior dispose.
//
// FIX. `handleReturn`'s bare-identifier fast-path checks the symbol's state
// BEFORE falling into `markDisposedByName`. If the state is Disposed, emit
// a hard Error ("local `b` is already disposed — returning it after
// `.Dispose()` is a use-after-free (§28.2 / §28.3)") and Poison the state
// so subsequent uses are also diagnosed.
//
// ACCEPTANCE (from the issue):
//   b.Dispose(); b.Dispose(); → Warning only (idempotent §28.2 preserved)
//   b.Dispose(); return b;     → Error (use-after-free, the bug)
//
// per §28.2, §28.3, v0.298, #4352

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::Severity;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// `Box` is an ordinary owned class with a destructor — a genuine non-trivial
// disposal whose use-after-dispose is a real heap use-after-free.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class Box "
        "  public N: int := 0; "
        "  public constructor(nn: int) this.N := nn; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C ") + body + " end C;";
}

// Returns true if any Error-severity diagnostic mentions the use-after-free
// shape introduced by fix #4352.
bool hasUseAfterDisposeReturnError(const bezel::test::OwnershipResult& r,
                                   const char* name) {
    const std::string needle =
        std::string("local `") + name +
        "` is already disposed — returning it after `.Dispose()` "
        "is a use-after-free (§28.2 / §28.3)";
    return hasErrorMessage(r, needle);
}

// Returns true if the idempotent double-dispose Warning (§28.2) fires at
// the second `b.Dispose();` site. The Warning's text is emitted by
// `markDisposedByName` in the Disposed-state leg.
bool hasIdempotentDoubleDisposeWarning(const bezel::test::OwnershipResult& r,
                                       const char* name) {
    const std::string needle =
        std::string("local `") + name +
        "` is disposed again here — the second `.Dispose()` is a no-op "
        "(disposal is idempotent, §28.2)";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// THE BUG: `return b;` after `b.Dispose();` must Error (use-after-free).
// This is the new diagnostic introduced by fix #4352.
BEZEL_TEST(return_after_explicit_dispose_4352,
           dispose_then_return_errors_28_2_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): owned Box "
        "  var b := new Box(42); "
        "  b.Dispose(); "
        "  return b; "
        "end Run;"));
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeReturnError(r, "b"));
}

// CONTROL: `b.Dispose(); b.Dispose();` must still be Warning-only (no Error).
// The idempotent §28.2 guarantee is preserved — the second `.Dispose()` is a
// no-op, not an error. This pins the boundary so a future fix does not regress
// §28.2 idempotency in pursuit of rejecting the use-after-free shape.
BEZEL_TEST(return_after_explicit_dispose_4352,
           double_dispose_warning_only_28_2_idempotent) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  var b := new Box(1); "
        "  b.Dispose(); "
        "  b.Dispose(); "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_TRUE(hasIdempotentDoubleDisposeWarning(r, "b"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: dispose + RETURN a different identifier → no use-after-free error.
// Only the bare-identifier path for the disposed symbol is the bug shape;
// returning an unrelated local after the dispose is fine.
BEZEL_TEST(return_after_explicit_dispose_4352,
           dispose_then_return_unrelated_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): int "
        "  var b := new Box(1); "
        "  var k := new Box(2); "
        "  b.Dispose(); "
        "  k.Dispose(); "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeReturnError(r, "b"));
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeReturnError(r, "k"));
}

// CONTROL: bare return without prior dispose is unchanged — no diagnostic.
// Confirms the fix only fires on the Disposed-state branch.
BEZEL_TEST(return_after_explicit_dispose_4352,
           bare_return_without_dispose_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(): owned Box "
        "  var b := new Box(42); "
        "  return b; "
        "end Run;"));
    BEZEL_EXPECT_FALSE(hasUseAfterDisposeReturnError(r, "b"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}