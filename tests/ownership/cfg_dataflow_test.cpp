// Regression tests for Ownership slice O3a — the analysis now runs as a
// forward dataflow over `bezel::flow::CFG` instead of the old syntactic
// walker. O3a is behavior-preserving: these cases pin the diagnostics the
// CFG driver must reproduce for control-flow shapes that the per-slice
// test files (O1, O2, O4, …) did not already cover directly — loops
// (walked single-pass with zero-trip semantics, §28.3 / matching the old
// walker), do/end blocks, and nesting of those with if/else joins.
//
// O3a changes NO observable behavior; every expectation here is what the
// pre-O3a walker produced for the same input.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// §28.9 (v0.237, #2654): `Stream` declares a DESTRUCTOR so it is NON-trivial-
// dispose. This file's subject is the §28 CFG/dataflow discharge analysis. A
// trivial-dispose class is exempt from the must-use auto-drop warning ("A
// `new` expression of a trivial-dispose class type … does *not* produce the
// standard ownership-discharge warning"), so a bare `class Stream` with no
// destructor would make the auto-drop assertions below test that exemption
// rather than the dataflow analysis they were written for.
std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class Stream "
        "  destructor end destructor; "
        "  public Inner: Stream? := null; "
        "  public Other: Stream? := null; "
        "end Stream; "
        "class C(source: Stream) "
        "  public method M(cond: bool, items: Stream) "
        ) + body + " end M; end C;";
}

} // namespace

// A `do/end` block is pure sequencing; an owned local declared inside it
// and never discharged is still dropped at method exit (the CFG inlines
// the do/end body into the linear thread, as the builder documents).
BEZEL_TEST(own_cfg, do_end_owned_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "do var x := new Stream(); end;"));
    // §28.3 v0.150 (#1008 Part B): `x` auto-drops at the do-block scope end.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}

BEZEL_TEST(own_cfg, do_end_owned_discharged_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "do var x := new Stream(); x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An owned local acquired and disposed within a `while` body is clean on
// the single pass the analysis makes over the loop body.
BEZEL_TEST(own_cfg, while_body_acquire_and_dispose_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do var x := new Stream(); x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A read of a moved-from local later in the same loop body is diagnosed
// (intra-block sequencing is preserved).
BEZEL_TEST(own_cfg, while_body_read_after_move_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "while cond do "
        "  var x := new Stream(); _ := move x; _ := x; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An if/else inside a do/end block still produces the §28.3 owned-vs-
// borrowed branch-join clash at the merge.
BEZEL_TEST(own_cfg, if_else_clash_inside_do_end) {
    auto r = parseAndCheckOwnership(wrap(
        "do "
        "  var x: Stream?; "
        "  if cond then x := new Stream(); "
        "  else x := source.Inner; "
        "  end; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Owned on both branches, discharged after the merge — clean. Exercises
// the CFG merge meet feeding a subsequent linear block.
BEZEL_TEST(own_cfg, if_else_owned_both_then_dispose_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); "
        "else x := new Stream(); "
        "end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A for-loop body acquiring an owned local without disposal: the field/
// local is dropped on the body path the analysis walks → diagnosed.
// (§21.4: the source is `0..10`, a `Range<int>`. An earlier form iterated the
// `items: Stream` parameter, but a plain non-`IEnumerable` class is not an
// iterable source — now a §21.4 error, #1775 — and that is orthogonal to the
// ownership behaviour this test pins.)
BEZEL_TEST(own_cfg, for_body_owned_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "for var it in 0..10 do var x := new Stream(); end;"));
    // §28.3 v0.150: a fresh owned local in a for-body auto-drops each iteration.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "x"));
}
