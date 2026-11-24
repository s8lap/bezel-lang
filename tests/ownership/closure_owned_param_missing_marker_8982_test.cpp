// Spec-derived regression test for issue #8982 — a call THROUGH A CLOSURE
// whose parameter is `owned`, passing a NAMED owned local WITHOUT the §22.3
// transfer marker, was ACCEPTED (and the program double-freed under ASan) in
// three POSITIONS the #8960 fix did not reach: a BINARY OPERAND, an `if`
// CONDITION, and a `?(` CALLEE.
//
// §22.3 (v0.359) *Parameter modifiers*, the `owned` bullet: "**Call-site
// marker:** when the argument is a **named owned binding** — an owned local or
// an `owned` parameter, which the call leaves moved-from — the caller writes
// the transfer marker at the call site: `M(move x)` ... Omitting the marker on
// a named owned binding is a compile error naming the fix (\"this call consumes
// `x`; write `M(move x)`\")". The rule is a property of the ARGUMENT POSITION
// — an `owned` parameter — and NOT of the spelling or of the ENCLOSING
// EXPRESSION of the call.
//
// §28.3.7 (v0.359) *Move semantics for owned locals*: "A callback that must
// genuinely consume a value takes an `owned` parameter and is invoked with the
// value transferred from outside (§28.9)."
//
// §28.9 (v0.359) *Runtime-managed types*: "a closure that must consume slot
// contents takes an owned-parameter argument and is invoked with `move`d slots
// from outside."
//
// THE DEFECT (the #8960 fix reached only the BINDING and the BARE-STATEMENT
// / `return`-operand positions). The `owned` call-site marker rule has
// exactly two homes in the ownership pass: `recordMovesInArgs` (reached from
// `recordMovesIn`, whose `BinaryExpr` arm recurses into both operands — so
// the LocalDecl-initializer / assignment-RHS positions were covered) and
// `handleDischarge`'s `CallExpr` arm (reached only when the call IS the
// position's top-level expression). A binary expression had NO `handleDischarge`
// arm, so a call nested in one was visited by NEITHER classifier when the
// position routed through `handleDischarge`: the marker was not demanded and
// the transfer was not recorded, so the caller kept its disposal obligation
// while the closure disposed the value too.
//
// The three rows below are the issue's three positions, each RED on the base
// (accepted, rc=0, no diagnostic) and GREEN with the fix. The METHOD twin row
// pins the POSITIONAL half of the same rule: the gap was never
// function-value-specific — `1 + Take(a)` over `method Take(owned ys: T)` was
// accepted at a binary operand for the same reason. The controls that must
// keep their verdicts — the marked spelling, the fresh-producer spelling, a
// plain (borrow) formal, the method twin — are in the lane's probe directory
// under `corpus-candidates/`, not here: they are green on both arms, so a
// gtest row would pin nothing.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The closure whose `owned` parameter is the sink, plus the named owned local
// that must be transferred. `tail` is the statement sequence that makes the
// call — it is the POSITION under test, so it is spelled per row.
//
// The closure stores the value it is given, so an unmarked transfer really is
// a double disposal (the caller disposes `a` and the closure disposes the
// same array) — the issue's own repro shape.
std::string closureProgram(const std::string& tail) {
    return
        "module M; "
        "import System; "
        "public class C "
        "  public owned keep: Array<int>?; "
        "  public method Go(): int "
        "    var f := lambda(owned ys: Array<int>): int => do "
        "      this.keep := ys; "
        "      return 1; "
        "    end; "
        "    var a := new Array<int>(); " + tail +
        "  end Go; "
        "end C; ";
}

// The same call, but through a NULLABLE function-typed field, so the `?(`
// spelling is exercised. The closure DISPOSES the value it is given rather
// than storing it in a field: a closure that captures `this` may not be
// stored in a field (§28.3's sink rule), and that unrelated diagnostic would
// mask the one this row is about.
std::string nullConditionalProgram(const std::string& tail) {
    return
        "module M; "
        "import System; "
        "public class C "
        "  public cb: ((owned Array<int>) -> int)? := null; "
        "  public method Go(): int "
        "    this.cb := lambda(owned ys: Array<int>): int => do "
        "      ys.Dispose(); "
        "      return 1; "
        "    end; "
        "    var a := new Array<int>(); " + tail +
        "  end Go; "
        "end C; ";
}

// The METHOD twin of the closure program: the same `owned` formal, reached
// through a method rather than through a function value.
std::string methodTwinProgram(const std::string& tail) {
    return
        "module M; "
        "import System; "
        "public class C "
        "  public owned keep: Array<int>?; "
        "  public method Take(owned ys: Array<int>): int "
        "    this.keep := ys; "
        "    return 1; "
        "  end Take; "
        "  public method Go(): int "
        "    var a := new Array<int>(); " + tail +
        "  end Go; "
        "end C; ";
}

// True iff some ERROR diagnostic is §22.3's missing-marker diagnostic for the
// binding `name` — the exact text the METHOD twin produces, so the two
// spellings of one rule cannot drift apart.
bool saysCallConsumes(const OwnershipResult& r, const std::string& name) {
    return hasErrorMessage(r, "this call consumes `" + name + "`");
}

// True iff some ERROR diagnostic is §22.3's missing-marker diagnostic naming
// the `M(move X)` remedy for the binding `name`.
bool saysWriteMoveX(const OwnershipResult& r, const std::string& name) {
    return hasErrorMessage(r, "write `M(move " + name + ")`");
}

} // namespace

// ---------------------------------------------------------------------------
// §22.3 (v0.359) *Parameter modifiers* — position 1 of #8982: the call is a
// BINARY OPERAND of a `return`. RED on the base (accepted, and the program
// double-freed under ASan); GREEN with the fix.
// ---------------------------------------------------------------------------

BEZEL_TEST(closure_owned_param_missing_marker_8982,
           closure_call_at_a_binary_operand_is_rejected) {
    auto r = parseAndCheckOwnership(closureProgram("return 1 + f(a);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The SPECIFIC rule: §22.3's missing-marker diagnostic, not an unrelated
    // error that happens to fire on the same program.
    BEZEL_EXPECT_TRUE(saysCallConsumes(r, "a"));
    BEZEL_EXPECT_TRUE(saysWriteMoveX(r, "a"));
}

// ---------------------------------------------------------------------------
// Position 2 of #8982: the call is the `if` CONDITION (the comparison's
// left operand). RED on the base; GREEN with the fix.
// ---------------------------------------------------------------------------

BEZEL_TEST(closure_owned_param_missing_marker_8982,
           closure_call_at_an_if_condition_is_rejected) {
    auto r = parseAndCheckOwnership(
        closureProgram("var r := 0; if f(a) > 0 then r := 5; end; return r;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(saysCallConsumes(r, "a"));
    BEZEL_EXPECT_TRUE(saysWriteMoveX(r, "a"));
}

// ---------------------------------------------------------------------------
// Position 3 of #8982: the call is the `?(` CALLEE — the left operand of the
// `??` meet, which is a binary operator, so the same position gap reaches it.
// RED on the base; GREEN with the fix.
// ---------------------------------------------------------------------------

BEZEL_TEST(closure_owned_param_missing_marker_8982,
           null_conditional_call_as_a_coalesce_operand_is_rejected) {
    auto r = parseAndCheckOwnership(
        nullConditionalProgram("return this.cb?(a) ?? 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(saysCallConsumes(r, "a"));
    BEZEL_EXPECT_TRUE(saysWriteMoveX(r, "a"));
}

// ---------------------------------------------------------------------------
// The POSITIONAL twin: the same `owned` formal reached through a METHOD. The
// gap was never function-value-specific — `1 + Take(a)` was accepted at a
// binary operand for the same reason, while the byte-identical bare-statement
// spelling `Take(a);` was already rejected. RED on the base; GREEN with the
// fix.
// ---------------------------------------------------------------------------

BEZEL_TEST(closure_owned_param_missing_marker_8982,
           method_twin_call_at_a_binary_operand_is_rejected) {
    auto r =
        parseAndCheckOwnership(methodTwinProgram("return 1 + this.Take(a);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(saysCallConsumes(r, "a"));
    BEZEL_EXPECT_TRUE(saysWriteMoveX(r, "a"));
}
