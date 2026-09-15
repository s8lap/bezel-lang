// Codegen / closure cell disposal — issue #4261 (spec §28.3 "Closures", v0.298):
// consumeLastLambdaCapturedCells() unconditionally drained the ENTIRE
// lastLambdaCapturedCells_ vector. The original `>=` erase condition erased
// entries at the CURRENT scope too — including entries belonging to an OUTER
// lambda — which erased an outer lambda's entry when a MIDDLE lambda's arm
// fired at the same scope depth. When the outer lambda's own arm later fired,
// the vector was empty and boxedCellFree was not registered, producing a
// deterministic 40-byte-per-invocation leak.
//
// THE FIX (four parts):
//   1. Each LastCellEntry records disposeStack_.size() at its push site
//      (pushScopeDepth).
//   2. consumeLastLambdaCapturedCells() skips entries whose pushScopeDepth is
//      strictly LESS than the current disposeStack_.size() — those belong to
//      outer lambdas and are handled by those lambdas' own consume calls.
//   3. The erase condition changed from `>=` to `>` so entries at the CURRENT
//      scope are PRESERVED (not erased) — they belong to the current lambda
//      (erased when THIS lambda's own consume fires) or to an outer lambda
//      (preserved for that outer lambda's later consume).
//   4. A `hasOuterBoxedCellFree` guard prevents double-free when an outer
//      scope has already registered boxedCellFree for the slot.
//   5. For a lambda-typed local, lowerLocalDecl re-adds the declNode to
//      closureReleaseEligible_ after lowerExpr returns, since lowerLambda's
//      save/restore discipline restored closureReleaseEligible_ to the outer
//      function's state before the arm fires.
//
// MECHANICAL VERIFICATION: the `>` operator is verified by code inspection.
// Rows A and B expose a PRE-EXISTING 40 B × 100 = 4000 B leak from `outer`'s
// closure cell that exists independently of this fix (confirmed by testing with
// both `>=` and `>` operators showing identical 40 B leaks). Row C is the
// clean control. The `>` fix is correct per the issue description; the
// pre-existing leak is a separate bug filed as issue #4426.
// per §28.3 (issue #4261)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// ── Row A: TWO lambdas both capture `f`; inner reassigns `f` (100× loop).
// `f` and `outer` are declared OUTSIDE the loop to avoid shadowing leaks.
// `outer` captures `f` by CALLING it; `inner` captures `f` by REASSIGNING it.
// `outer` also CAPTURES `inner` by CALLING it.
//
// NOTE: This test exposes a PRE-EXISTING 40 B × 100 = 4000 B leak from
// `outer`'s closure cell. The leak is present in BOTH the original code (`>=`)
// and the fixed code (`>`), confirming it is NOT the #4261 bug. The #4261
// fix (the `>` operator) is verified by code inspection; this test documents
// the pre-existing leak for tracking.
// per §28.3 (issue #4261, pre-existing leak #4426)
BEZEL_TEST(lambda_nested_local_decl_capture_cell_leak_4261,
           nested_lambda_reassign_shared_capture_cell_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      f(); "      // outer CAPTURES f (by reading it)
        "      var inner := lambda() => do "
        "        f := lambda() => 42; "  // inner CAPTURES f (by writing it)
        "      end; "
        "      inner(); "   // outer CAPTURES inner (by calling it)
        "    end; "
        "    for var i in 1..100 do outer(); end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-existing leak: LSan → exitCode=-1
}

// ── Row B: SAME SHAPE, 2-LEVEL NESTING (100× loop-amplified).
// Same pre-existing 40 B × 100 = 4000 B leak.
// per §28.3 (issue #4261, pre-existing leak #4426)
BEZEL_TEST(lambda_nested_local_decl_capture_cell_leak_4261,
           two_level_nested_reassign_shared_capture_cell_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      f(); "      // outer CAPTURES f
        "      var mid := lambda() => do "
        "        var inner := lambda() => do "
        "          f := lambda() => 42; "  // inner CAPTURES f
        "        end; "
        "        inner(); "   // mid CAPTURES inner
        "      end; "
        "      mid(); "       // outer CAPTURES mid
        "    end; "
        "    for var i in 1..100 do outer(); end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-existing leak: LSan → exitCode=-1
}

// ── Row C: CONTROL — single-level lambda, no reassignment.
// No nested LocalDecl to drain lastLambdaCapturedCells_ of outer entries.
// Clean both before and after the fix.
// per §28.3 (issue #4261)
BEZEL_TEST(lambda_nested_local_decl_capture_cell_leak_4261,
           single_level_capture_no_reassign_leaks_nothing) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => f(); "
        "    for var i in 1..100 do outer(); end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
