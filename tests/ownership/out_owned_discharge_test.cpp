// Spec-derived tests for `out owned` parameter discharge (issue #390).
//
// §22.3: "`out owned` — output parameter that produces an owned value.
// The callee assigns an owned value through the parameter; the caller
// receives ownership and must discharge it." §28.8: assigning an owned
// value to an `out owned` parameter transfers ownership OUT to the caller
// through the out-location, which DISCHARGES the callee's obligation.
//
// Regression: the ownership analysis seeds an `out owned` parameter as
// Owned and, on reassignment, kept it Owned — so a method that produced
// an owned value through the `out owned` parameter (`o := new R();`) was
// spuriously flagged as an undischarged owned obligation (over-rejection).
// The POSITIVE cases below must now be ACCEPTED; the NEGATIVE cases must
// still be REJECTED (no under-rejection).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap a method body in a class with helper types declared so type/identifier
// resolution does not emit unrelated diagnostics. `Resource` is the disposable
// class used as the produced/owned type.
std::string wrap(const std::string& method) {
    return std::string("module M; "
        "class R end R; class Resource end Resource; "
        "class C ") + method + " end C;";
}

} // namespace

// ---- POSITIVE cases (must now be accepted) --------------------------------

// §22.3 / §28.8: `o := new R();` produces a fresh owned value and transfers
// it out through the `out owned` parameter. This is the minimal repro from
// issue #390 — it was spuriously rejected before the fix.
BEZEL_TEST(out_owned, assign_new_discharges) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(out owned o: R) "
        "  o := new R(); "
        "end Make;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / §28.3: moving an owned local into the `out owned` parameter
// transfers ownership out; the local is left moved-from (discharged) and the
// parameter carries the obligation to the caller.
BEZEL_TEST(out_owned, assign_move_local_discharges) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(out owned o: R) "
        "  var x := new R(); "
        "  o := move x; "
        "end Make;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 branch join: assigning an owned value to the `out owned` parameter on
// every branch discharges on every path — accepted.
BEZEL_TEST(out_owned, assign_on_both_branches_discharges) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(flag: bool, out owned o: R) "
        "  if flag then o := new R(); else o := new R(); end; "
        "end Make;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 / §28.8: producing the owned value through a chain — build a fresh
// owned local, then move it out through the `out owned` parameter.
BEZEL_TEST(out_owned, produce_through_chain_discharges) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(out owned o: R) "
        "  var tmp := new R(); "
        "  o := move tmp; "
        "end Make;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A plain `out` (non-owned) parameter carries no ownership obligation; writing
// a value type through it is clean. (Guards against the fix mis-classifying
// ordinary `out`.)
BEZEL_TEST(out_owned, plain_out_value_param_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(out o: int) "
        "  o := 1; "
        "end Make;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- NEGATIVE cases (must still be rejected) ------------------------------

// §28.3: an owned local created and never moved into the `out owned`
// parameter (nor disposed) is still an undischarged obligation, even when the
// parameter itself is correctly produced. The fix must not silence the local
// leak.
BEZEL_TEST(out_owned, unmoved_owned_local_still_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(out owned o: R) "
        "  var x := new R(); "
        "  o := new R(); "
        "end Make;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 double-ownership: moving an owned local into the `out owned`
// parameter leaves the local moved-from; continuing to use it (here, a later
// `.Dispose()`) is a use-after-move — still rejected.
BEZEL_TEST(out_owned, use_after_move_into_out_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(out owned o: R) "
        "  var x := new R(); "
        "  o := move x; "
        "  x.Dispose(); "
        "end Make;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3: assigning a bare owned local (without `move`) to the `out owned`
// parameter does NOT transfer the local's ownership; the local stays Owned and
// is flagged as undischarged. (The required form is `o := move x;`.)
BEZEL_TEST(out_owned, bare_local_assign_leaves_local_undischarged) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Make(out owned o: R) "
        "  var x := new R(); "
        "  o := x; "
        "end Make;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
