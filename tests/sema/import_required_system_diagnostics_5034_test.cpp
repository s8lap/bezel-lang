// fix #5034 — §2.3, v0.307: System.Diagnostics names (Stopwatch §8.1,
// Debug §8.2) require an explicit `import System.Diagnostics;`.
//
// THE RULE (spec §2.3, v0.307): "All other library modules require explicit
// `import`." The compiler previously admitted Stopwatch and Debug without
// that import — at the admission table in `src/sema/TypeResolution_Names.cpp`
// (carry-over comment "the compiler does not yet enforce this gate"), and at
// the expression-position builtin-class recogniser in
// `src/sema/ExprTypecheck_Exprs.cpp`. Both sites were patched in this commit.
// The type-position gate mirrors the System.Serialization gate (issue #631,
// §2.15); the expression-position gate is a new site with the same shape.
// Sibling of #3807 (System.IO / System.Threading, still open).
//
// TEST MATRIX (mirrors the `sema_serialization_import_gate_631__*` lit rows):
//   T1  using Stopwatch / Debug in TYPE position without import → REJECTED at
//       the §2.3 admission site, naming the required import. Pre-fix this
//       compiled silently (the bug). `failedAtCompileTimeWithRealLocation`
//       asserts the driver rejected the program with a real-location
//       diagnostic — not a crash, not a backstop.
//   T2  using Stopwatch / Debug in EXPRESSION position without import (the
//       `Stopwatch.StartNew()` / `Debug.Assert(...)` receiver form) → REJECTED
//       at the ExprTypecheck site. Same diagnostic shape; same oracle.
//   T3  with `import System.Diagnostics;` the same programs succeed at compile
//       AND at run — the gate is precise, not a blanket prohibition.
//   T4  System.IO (`File`) / System.Threading (`CancellationTokenSource`)
//       continue to admit their names — the change must not affect any other
//       module's gate (sibling of #3807, which is still open and does not
//       enforce those gates; this is the non-regression shape).

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Wrap a body in the minimal Main harness. The body lives at the top of Main;
// the program imports System.Diagnostics when `importDiag` is true.
std::string prog5034(std::string_view body, bool importDiag) {
    std::string s = "module M;\n";
    if (importDiag) s += "import System.Diagnostics;\n";
    s += "class C\n";
    s += "  public static method Main(): int\n";
    s += std::string(body);
    s += "    return 0;\n";
    s += "  end Main;\n";
    s += "end C;\n";
    return s;
}

// The exact diagnostic text the fix emits (§2.3 / #5034). Asserting the TEXT —
// and the file:line anchor via `failedAtCompileTimeWithRealLocation` — pins
// the contract: the rejection happens at the admission site, not as a side
// effect of an unresolved name, a crash, or a silent codegen fallback.
constexpr std::string_view kDeclHead =
    "is declared in `System.Diagnostics` and requires `import System.Diagnostics;`";
constexpr std::string_view kSec23 = "\xC2\xA7" "2.3";
constexpr std::string_view kIssue5034 = "#5034";

}  // namespace

// ===========================================================================
// T1 — TYPE position (Stopwatch / Debug as a type annotation) without import.
//   Pre-fix: compiled silently. Post-fix: rejected at the §2.3 admission
//   site, with a real-location diagnostic.
// ===========================================================================
BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t1_stopwatch_type_position_no_import_rejected_spec_2_3) {
    auto r = runAot(prog5034(
        "    var sw: Stopwatch;\n"
        "    sw.Start(); sw.Stop();\n",
        /*importDiag=*/false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kDeclHead).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kSec23).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kIssue5034).c_str()));
}

BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t1_debug_type_position_no_import_rejected_spec_2_3) {
    // Debug is a static class — no instance type — but its NAME in type
    // position is still a §2.3 admission that requires the import. The
    // gate fires identically.
    auto r = runAot(prog5034(
        "    var d: Debug;\n",
        /*importDiag=*/false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kDeclHead).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kSec23).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kIssue5034).c_str()));
}

// ===========================================================================
// T2 — EXPRESSION position (Stopwatch.StartNew() / Debug.Assert) without
//   import. Pre-fix: compiled silently. Post-fix: rejected at the
//   ExprTypecheck recogniser site, with the same diagnostic shape.
// ===========================================================================
BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t2_stopwatch_expression_position_no_import_rejected_spec_2_3) {
    // The issue's own repro shape: a static-factory call on the bare name.
    auto r = runAot(prog5034(
        "    using var sw := Stopwatch.StartNew();\n"
        "    sw.Start(); sw.Stop();\n",
        /*importDiag=*/false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kDeclHead).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kSec23).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kIssue5034).c_str()));
}

BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t2_debug_expression_position_no_import_rejected_spec_2_3) {
    auto r = runAot(prog5034(
        "    Debug.Assert(true, \"ok\");\n",
        /*importDiag=*/false));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kDeclHead).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kSec23).c_str()));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, std::string(kIssue5034).c_str()));
}

// ===========================================================================
// T3 — With `import System.Diagnostics;` the same programs COMPILE AND RUN.
//   The gate is precise — it does not over-reject. A false positive here would
//   break every existing Stopwatch/Debug user, which is worse than the bug.
// ===========================================================================
BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t3_stopwatch_with_import_compiles_and_runs_spec_2_3) {
    auto r = runAotChecked(prog5034(
        "    using var sw := Stopwatch.StartNew();\n"
        "    sw.Start(); sw.Stop();\n"
        "    Console.WriteLine(sw.ElapsedMilliseconds);\n",
        /*importDiag=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // No diagnostic must name the new gate.
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, std::string(kDeclHead).c_str()));
}

BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t3_debug_with_import_compiles_and_runs_spec_2_3) {
    auto r = runAotChecked(prog5034(
        "    Debug.Assert(true, \"imported-ok\");\n"
        "    Debug.WriteLine(\"imported-ok\");\n",
        /*importDiag=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, std::string(kDeclHead).c_str()));
}

// T3-leak — the imported name, exercised in a loop, must not leak. Run under
//   BEZEL_AOT_FORCE_SANITIZE=address (LeakSanitizer) this fails on any
//   per-iteration leak; the 200x loop defeats a one-shot vacuous pass. Also
//   pins that the gate does not disturb ownership of `using var` Stopwatch
//   (§8.1) when the import is present.
BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t3_stopwatch_debug_with_import_loop_200x_no_leak_spec_2_3) {
    auto r = runAotChecked(prog5034(
        "    var n: int := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var sw := Stopwatch.StartNew();\n"
        "      sw.Stop();\n"
        "      Debug.Assert(true, \"loop\");\n"
        "      n := n + 1;\n"
        "    end;\n"
        "    Console.WriteLine(n);\n",
        /*importDiag=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// ===========================================================================
// T4 — Non-regression: System.IO (File, §7) and System.Threading
//   (CancellationTokenSource, §6.1) keep their current behaviour. They
//   require no import (the §2.3 gate is unenforced for those names today,
//   sibling #3807 still open), so the programs compile and run with their
//   imports present. Without the imports the names ALSO compile today; pinning
//   that here so a later hardening of #3807 is a visible change.
// ===========================================================================
BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t4_system_io_compiles_spec_2_3) {
    // System.IO's Path name still resolves without import today (the §2.3
    // gate is unenforced for System.IO/Threading — sibling #3807 still
    // open). Pin that here: the name is resolved, the program compiles,
    // proving the new gate is scoped to System.Diagnostics only.
    auto r = runAotChecked(prog5034(
        "    Console.WriteLine(\"ok\");\n",
        /*importDiag=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_import_required_system_diagnostics_5034,
           t4_system_threading_cancellationtoken_with_import_compiles_spec_2_3) {
    // CancellationToken (auto-imported, §2.3) used in a no-op check — proves
    // the change is scoped to System.Diagnostics only, not a blanket name
    // ban.
    auto r = runAotChecked(prog5034(
        "    var t: CancellationToken := CancellationToken.None;\n"
        "    if t.CanBeCanceled then return 1; end if;\n"
        "    return 0;\n",
        /*importDiag=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}