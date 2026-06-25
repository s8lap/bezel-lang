// Spec-derived tests for Ownership slice O10 — owned parameter discharge
// (§22.3 + §28.3 + §28.8).
//
// An `owned` parameter is in the Owned state on method entry. It must
// reach a discharge point on every normal-exit path (same rule as a
// plain `var x := producer-of-owned;`). `out owned` follows the same
// rule on the callee side.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& method) {
    return std::string("module M; "
        "class T end T; class Resource end Resource; "
        "class C ") + method + " end C;";
}

} // namespace

// §28.3 v0.152 (#1008 Part B, extended to parameters): an undischarged `owned`
// parameter is no longer an error — it auto-drops at method end (the same
// implicit disposal the exception edge already performed) with the must-use
// warning.
BEZEL_TEST(own_param, owned_param_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "r"));
}

BEZEL_TEST(own_param, owned_param_explicitly_disposed_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) r.Dispose(); end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_param, owned_param_returned_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Pass(owned r: Resource): Resource "
        "  return r; "
        "end Pass;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_param, borrowed_param_no_obligation) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Inspect(r: Resource) end Inspect;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.2: a double Dispose() on an owned param is a WARNING (idempotent no-op),
// not an error — the second call is a guaranteed runtime no-op.
BEZEL_TEST(own_param, owned_param_double_disposed_warned_not_errored) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) "
        "  r.Dispose(); r.Dispose(); "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool warned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("disposed again") != std::string::npos)
            warned = true;
    BEZEL_EXPECT_TRUE(warned);
}

BEZEL_TEST(own_param, owned_param_moved_then_read_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r: Resource) "
        "  _ := move r; "
        "  _ := r; "
        "end Take;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.3 v0.152: `r1` is explicitly disposed; the undischarged `r2` auto-drops at
// method end with the must-use warning — no error.
BEZEL_TEST(own_param, two_owned_params_one_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Take(owned r1: Resource, owned r2: Resource) "
        "  r1.Dispose(); "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasAutoDropWarning(r, "r2"));
}

// (The dispose-exactly-once / no-double-free behaviour of the parameter
// auto-drop — on both the normal and exception edges, with a caller-`move`d
// source not double-freed — is verified in the codegen battery
// move_result_owned_724: owned_param_auto_drop_disposes_once.)
