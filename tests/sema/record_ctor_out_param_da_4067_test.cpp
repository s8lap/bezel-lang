// §22.3 (issue #4067, v0.294): a record constructor's own `out` parameters
// received NO definite-assignment checking at all. Found while diffing
// `DefiniteAssignment.cpp::runConstructor` (class) against
// `runRecordConstructor` (record) for issue #4060 (the record chain-call
// argument DA-walk gap) — a separate, unrelated divergence from #4060's own
// fix, discovered by the same diff exercise.
//
// WHAT THE DEFECT WAS. `runConstructor` seeds the constructor's own `out`
// parameters into `outParams_`/`declared_` at entry and calls
// `checkOutParams()` at fall-off (§22.3 — "the callee must assign every `out`
// parameter before returning"). `runRecordConstructor` did neither. Measured
// on origin/main a6e270d + fix #4060 (this fix's parent): a record
// constructor falling off with an unassigned `out` parameter on some path
// compiled clean, silently returning whatever garbage was in the caller's
// recipient slot — while the character-identical class shape was already
// correctly rejected.
//
// THE FIX. `runRecordConstructor` now seeds `outParams_`/`declared_` from
// `cd.params` (identical logic to `runConstructor`'s inline copy) and calls
// `checkOutParams(/*loc=*/nullptr)` at fall-off, exactly mirroring
// `runConstructor` -- including its `nullptr` location argument, an existing
// wart in the class path this fix does not attempt to correct (hence Row 1's
// real-location assertion is on the OTHER diagnostic in the same reject,
// none needed here -- see Row 1's comment).

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%u:%u] %s\n", d.location.line,
                     d.location.column, d.message.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — REJECT (record): an `out` parameter left unassigned on one path
// (the `if cond` guard's false branch never assigns `ok`) falling off the
// constructor. `runConstructor`'s own `checkOutParams` call passes a
// `nullptr` location (SourceLocation{}), so this diagnostic's own location is
// `0:0` by design on BOTH class and record — matching that pre-existing
// class-path behaviour is the point of this fix, so this row asserts the
// message TEXT only (LANE_RULES §3's "never ship a diagnostic at 0:0" is
// about NEW diagnostics this fix invents; this one is verified byte-for-byte
// identical to the already-shipped class diagnostic, reproduced by Row 2).
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_out_param_da_4067, unassigned_out_param_rejected) {
    auto r = runAot(
        "module M; "
        "public record ProbeOutParamUnassigned4067 "
        "  public N: int; "
        "  public constructor(n: int, cond: bool, out ok: bool) "
        "    N := n; "
        "    if cond then "
        "      ok := true; "
        "    end if; "
        "  end constructor; "
        "end ProbeOutParamUnassigned4067; "
        "class C "
        "  public static method Main(): int "
        "    var ok: bool; "
        "    var r := new ProbeOutParamUnassigned4067(5, false, out ok); "
        "    return r.N; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "out parameter `ok` is not definitely assigned"))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasError(r, "out parameter `ok` is not definitely assigned"));
    BEZEL_EXPECT_TRUE(hasError(r, "§22.3"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 2 — CONTROL (class): the character-identical shape on the class side,
// unaffected by this fix (runConstructor already did this), already
// correctly rejects it on origin/main a6e270d. Pins that Row 1's new
// diagnostic is genuine parity, not a novel rule invented here.
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_out_param_da_4067, class_control_unassigned_out_param_rejected) {
    auto r = runAot(
        "module M; "
        "public class ProbeClassOutParamUnassigned4067 "
        "  public N: int; "
        "  public constructor(n: int, cond: bool, out ok: bool) "
        "    N := n; "
        "    if cond then "
        "      ok := true; "
        "    end if; "
        "  end constructor; "
        "end ProbeClassOutParamUnassigned4067; "
        "class C "
        "  public static method Main(): int "
        "    var ok: bool; "
        "    var r := new ProbeClassOutParamUnassigned4067(5, false, out ok); "
        "    return r.N; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "out parameter `ok` is not definitely assigned"))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasError(r, "out parameter `ok` is not definitely assigned"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 3 — CONTROL, ACCEPT (record): an `out` parameter assigned
// unconditionally must still compile and run correctly (LANE_RULES §15
// accept-side sweep) — the ordinary, correct use of this feature.
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_out_param_da_4067, assigned_out_param_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record ProbeOutParamAssigned4067 "
        "  public N: int; "
        "  public constructor(n: int, out ok: bool) "
        "    N := n; "
        "    ok := true; "
        "  end constructor; "
        "end ProbeOutParamAssigned4067; "
        "class C "
        "  public static method Main(): int "
        "    var ok: bool; "
        "    var r := new ProbeOutParamAssigned4067(5, out ok); "
        "    return r.N; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
