// Spec-derived tests for issue #8935 — the `move` marker at an owned-field
// store is a spelling error, and a LAMBDA body must get the same verdict as a
// METHOD body.
//
// §28.3.7 (v0.359), "Three spelling errors": the marker at a marker-free
// position — "... an owned-field or `set(owned)` store spelled `sink._f := move
// x;` ... — one spelling per site: the announcement is the spelling there, and
// the diagnostic names it." The consume table (K3.1) gives the owned-field
// store's Spelling column as "none — the declared slot announces (marker-free)".
//
// In a method body `this.keep := move ys;` is rejected with the redundant-
// marker diagnostic. Before the #8935 fix the same statement inside a lambda
// body was admitted: the redundant-marker check lived only in the method-body
// statement walk, and the lambda-body pre-pass never ran it. The fix moves that
// check into one helper that both walks call.
//
// Both rows below are red on the base (the lambda store is admitted, so no
// error is reported) and green with the fix. The admitted control (the same
// store without `move`, which must stay admitted and run clean) is a base-green
// row, so it lives in the conformance corpus candidates, not here (owner,
// 2026-11-12).

#include <string>
#include <string_view>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some error diagnostic's message contains `needle`.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// §28.3.7 (v0.359) spelling error at an owned-field store, LAMBDA body, the
// issue's repro: `this.keep := move ys;`. Rejected with the redundant-marker
// diagnostic, exactly as the method-body twin is. // per §28.3.7, v0.359
BEZEL_TEST(redundant_marker_owned_field_lambda_8935,
           lambda_body_this_field_store_with_move_rejected_redundant_marker_spec_28_3_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  private owned keep: Array<int>?; "
        "  public method Go(): int "
        "    var f := lambda(): int => do "
        "      var ys := new Array<int>(); "
        "      this.keep := move ys; "
        "      return 1; "
        "    end; "
        "    return f(); "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "the `move` marker is redundant"));
}

// §28.3.7 (v0.359), the same spelling error through the BARE field name
// (implicit `this`): `keep := move ys;` inside a lambda body. The method-body
// check resolves the target through `exprIsOwnedFieldRead`, which accepts a
// bare identifier naming an owned field, so the lambda path must accept it too.
// // per §28.3.7, v0.359
BEZEL_TEST(redundant_marker_owned_field_lambda_8935,
           lambda_body_bare_field_store_with_move_rejected_redundant_marker_spec_28_3_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  private owned keep: Array<int>?; "
        "  public method Go(): int "
        "    var f := lambda(): int => do "
        "      var ys := new Array<int>(); "
        "      keep := move ys; "
        "      return 1; "
        "    end; "
        "    return f(); "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "the `move` marker is redundant"));
}
