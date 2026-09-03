// Spec-derived regression tests for issue #3783 — the §22.3 missing-marker
// diagnostic must name the v0.286-mandated spelling `M(move x)`, not the
// legacy `M(owned x)` the compiler was emitting.
//
// §22.3 (v0.286): "the caller writes the transfer marker at the call site:
// `M(move x)` (an identifier-operand `move` expression, §28.8.1 — the marker
// is part of the argument expression, not an argument modifier). ... Omitting
// the marker on a named owned binding is a compile error naming the fix
// ('this call consumes `x`; write `M(move x)`')."
//
// The compiler flip is a literal substitution in `MethodOwnership::
// reportMissingOwnedMarker` (src/ownership/Ownership_Checks.cpp:8013). The
// legacy spelling `M(owned x)` reads as an argument modifier (A.7
// `ArgModifiers`); the v0.286 spelling is an identifier-operand `move`
// expression (§28.8.1). A user following the diagnostic with the OLD text was
// pointed at a spelling the rest of the §22.3 wording now contradicts.
//
// These tests pin both halves: the diagnostic TEXT, and the fact that the
// v0.286 spelling `M(move x)` compiles, codegens, runs, and disposes exactly
// once. The OLD spelling is asserted as the diagnostic's NOT — see also the
// lit row `tests/lit/ownership/missing_marker_remedy_text_3783__names_move_x.bzl`.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// Mirrors `owned_marker_transfer_test.cpp`'s `mod()` so the cells below name
// exactly the missing-marker path, not anything else.
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class T end T; "
        "class Sink "
        "  public method Add(owned item: T) "
        "    using var hold := move item; "
        "  end Add; "
        "end Sink; ") + body;
}

// True iff some error diagnostic carries the §22.3 missing-marker message
// naming the v0.286 `M(move X)` spelling.
bool saysMarkerIsMoveX(const OwnershipResult& r, const std::string& name) {
    const std::string consume = "this call consumes `" + name + "`";
    const std::string fix = "write `M(move " + name + ")`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(consume) != std::string::npos &&
            d.message.find(fix) != std::string::npos)
            return true;
    return false;
}

// True iff some error diagnostic still names the LEGACY `M(owned X)` spelling.
// This must be FALSE everywhere after fix #3783 lands — the OLD spelling is
// the defect this fix removes from the diagnostic.
bool saysMarkerIsOwnedX(const OwnershipResult& r, const std::string& name) {
    const std::string consume = "this call consumes `" + name + "`";
    const std::string fix = "write `M(owned " + name + ")`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(consume) != std::string::npos &&
            d.message.find(fix) != std::string::npos)
            return true;
    return false;
}

// `T` carries a live-instance counter. `Main` returns `T.live`; zero means
// exactly-once construction + exactly-once disposal.
const char* kT =
    "class T "
    "  public static live: int := 0; "
    "  public constructor() T.live := T.live + 1; end constructor; "
    "  destructor T.live := T.live - 1; end destructor; "
    "end T; ";

} // namespace

// ---------------------------------------------------------------------------
// TEXT REGRESSION (§22.3, v0.286): the diagnostic names `M(move x)`, not
// `M(owned x)`. Mirrored by the lit row of the same name; kept here in gtest
// so the assertion lives next to every other missing-marker behavior test.
// ---------------------------------------------------------------------------

// ACCEPT half (negative): the OLD spelling must not appear in the missing-
// marker diagnostic. Pre-fix this was the very defect; post-fix it is the
// negative pin that catches a regression introducing the spelling back.
BEZEL_TEST(missing_marker_remedy_text_3783,
           missing_marker_diagnostic_does_not_name_legacy_owned_spelling) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(x); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(saysMarkerIsOwnedX(r, "x"));
}

// ACCEPT half (positive): the NEW spelling must appear. Pre-fix this was
// false; post-fix it is the positive pin the lit row's CHECK line guards.
BEZEL_TEST(missing_marker_remedy_text_3783,
           missing_marker_diagnostic_names_v0_286_move_spelling) {
    auto r = parseAndCheckOwnership(mod(
        "class C public method M(s: Sink) "
        "  var x := new T(); s.Add(x); "
        "end M; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(saysMarkerIsMoveX(r, "x"));
}

// ---------------------------------------------------------------------------
// RUNTIME REGRESSION (§22.3 / §28.8.1, v0.286): the v0.286 spelling
// `M(move x)` compiles, codegens, runs, and disposes exactly once. Without
// this row a regression could leave the diagnostic text right while the
// sema path the text points at silently rejects the new spelling — the
// cell the diagnostic IS the user-facing contract for.
// ---------------------------------------------------------------------------

BEZEL_TEST(missing_marker_remedy_text_3783,
           v0_286_move_spelling_compiles_runs_disposes_once) {
    const std::string src =
        std::string("module M; ") + kT +
        "class Sink "
        "  public method Add(owned item: T) "
        "    using var hold := move item; "
        "  end Add; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var s: Sink := new Sink(); "
        "    var x := new T(); "
        "    s.Add(move x); "                       // §22.3 / §28.8.1
        "    return T.live; "                       // 0 — disposed exactly once
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}