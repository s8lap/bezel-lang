// Spec-derived tests for §21.5/§31.2 (v0.182, issue #1578): a `break`/
// `continue` inside a `finally` block whose target loop (the immediately
// enclosing loop, §21.5) lies OUTSIDE the `finally` is a compile error — the
// same rule as `return` inside a `finally` (§21.7/§31.2,
// return_in_finally_test.cpp). When the `finally` runs during exception
// propagation, such a jump would silently discard the in-flight exception. A
// loop declared wholly INSIDE the `finally` is unaffected — its `break`/
// `continue` behaves normally (§21.5: "Loops wholly inside the finally use
// break/continue normally.").
//
// Bug #1578: the compiler currently accepts this and emits no diagnostic.
// These tests verify the fix.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Convenience: wrap a body inside a static int method with a loop-condition
// parameter, mirroring tests/sema/break_continue_test.cpp's `wrap` helper.
std::string wrap(const std::string& body) {
    return std::string("module M; class C public method M(flag: bool, n: int) ")
        + body + " end M; end C;";
}

} // namespace

// §21.5/§31.2: `break` inside a `finally` targeting the loop that ENCLOSES
// the `try`/`finally` is a compile error — the loop was opened before the
// `finally`, so the jump would exit past it.
BEZEL_TEST(break_continue_in_finally_1578, break_targeting_enclosing_loop_rejected_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "while flag do "
        "  try n := n + 1; "
        "  finally break; end; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Same rule for `continue`.
BEZEL_TEST(break_continue_in_finally_1578, continue_targeting_enclosing_loop_rejected_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "while flag do "
        "  try n := n + 1; "
        "  finally continue; end; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The enclosing-loop target is still rejected when the break/continue is
// nested inside an `if` within the finally (lexical nesting through
// non-loop constructs does not change the target loop).
BEZEL_TEST(break_continue_in_finally_1578, break_in_nested_if_inside_finally_rejected_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "while flag do "
        "  try n := n + 1; "
        "  finally if flag then break; end; end; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Two levels of enclosing try/finally: the target loop encloses BOTH, and
// the break sits in the innermost finally — still rejected.
BEZEL_TEST(break_continue_in_finally_1578, break_in_nested_finally_still_rejected_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "while flag do "
        "  try n := n + 1; "
        "  finally "
        "    try n := n + 2; "
        "    finally break; end; "
        "  end; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Positive: a loop declared WHOLLY INSIDE the `finally` block uses
// `break`/`continue` normally — §21.5: "Loops wholly inside the finally use
// break/continue normally."
BEZEL_TEST(break_continue_in_finally_1578, break_in_loop_wholly_inside_finally_ok_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "try n := n + 1; "
        "finally "
        "  while flag do break; end; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same positive case for `continue`.
BEZEL_TEST(break_continue_in_finally_1578, continue_in_loop_wholly_inside_finally_ok_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "try n := n + 1; "
        "finally "
        "  while flag do continue; end; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive: `break`/`continue` in a `try` body (not `finally`) targeting a
// loop that encloses the whole `try` statement is unaffected by this rule —
// only jumps physically inside the `finally` are restricted.
BEZEL_TEST(break_continue_in_finally_1578, break_in_try_body_targeting_enclosing_loop_ok_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "while flag do "
        "  try break; "
        "  finally n := n + 1; end; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive: ordinary `break`/`continue` with no try/finally involved at all
// still compiles (baseline, mirrors break_continue_test.cpp).
BEZEL_TEST(break_continue_in_finally_1578, break_outside_finally_ok_spec_21_5) {
    auto r = parseSemaAll(wrap("while flag do break; end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive: a `try`/`finally` with no `break`/`continue` in the finally at
// all is unaffected.
BEZEL_TEST(break_continue_in_finally_1578, no_break_in_finally_ok_spec_21_5) {
    auto r = parseSemaAll(wrap(
        "while flag do "
        "  try n := n + 1; "
        "  finally n := n + 2; end; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
