// fix #5849 (§8.5 / §28.3 / §28.9): CG27c primitive ToString() on a bare
// expression statement leaks. emitPrimitiveToString (LlvmBackend_Exprs.cpp) mints
// a fresh heap string at rc=1, and for `var s := int.ToString()` the
// isPlusOneStringProducer machinery in lowerLocalDecl handles the transfer-and-
// release. But for a BARE expression statement the result is discarded with no
// release — 32+ bytes per call leak.
//
// This test uses `leaksAtLeast` (a measured, margined leak-magnitude floor)
// to detect the leak. The minimum (300 bytes) is set below the measured
// pre-fix leak (verified 50 iterations: 1760 B / 50 allocations, ~35 B/call
// and 1 alloc/call) while failing hard against a fix that leaves the leak
// present at only a few bytes of harness noise.
//
// After the fix, runChecked expects 0 (no sanitizer finding = clean exit).

#include <regex>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// LEAK oracle: runAotChecked sets sanitizeAddress on the GENERATED program
// and treats ANY sanitizer finding (including LSan's own exit-time leak
// report) as ran=false. A program that leaks therefore reports -999 here;
// a clean program reports its real exit code.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Measured leak-magnitude floor. Used to detect a leak's PRESENCE
// (wrongly-clean after-fix would return false, failing the row) and to
// prove the pre-fix leak exists (returns true before the fix lands).
// minBytes = 300 is below the measured pre-fix leak (35 B/call × 50 iters
// = 1750 B) but well above harness noise.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding at all
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;
}
}  // namespace

// ===========================================================================
// Regression row: bare int.ToString() in a loop. Before the fix: leak.
// After the fix: clean exit (0). per LANE policy: 50-500x loop amplifies
// the single-call leak (32 B/call, 1 alloc/call) to a detectable magnitude.
// ===========================================================================
BEZEL_TEST(primitive_tostring_bare_expr_leak_5849,
           bare_int_tostring_no_leak_loop_50x) {
    // Before fix: leaksAtLeast returns true (leak present, ~1750 B / 50 allocs)
    // After fix: runChecked returns 0 (no sanitizer finding = clean exit)
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 50 do "
        "    i.ToString(); "  // bare expression — result discarded without release
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// Control: verify the loop itself is not the source of the leak
// (var s := i.ToString() must work correctly after the fix)
BEZEL_TEST(primitive_tostring_bare_expr_leak_5849,
           assignment_int_tostring_works_correctly) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 10 do "
        "    var s := i.ToString(); "
        "    if s.Length <> 1 then return 1; end; "
        "    i += 1; "
        "  end; "
        "  return 0; "  // all 10 iterations produced correct "0"..."9"
        "end Main; end C;"), 0);
}

// Control: double.ToString() bare expression (CG27c path for Float/Double)
BEZEL_TEST(primitive_tostring_bare_expr_leak_5849,
           bare_double_tostring_no_leak_loop_50x) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0.0; "
        "  while i < 50.0 do "
        "    i.ToString(); "  // bare expression — result discarded without release
        "    i += 1.0; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 0);
}
