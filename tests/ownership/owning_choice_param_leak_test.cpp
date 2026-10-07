// Regression test for owning choice parameter disposal leak — §15.3 (v0.315).
//
// DEFECT — owning choice passed as a by-value-move parameter is never disposed.
// §15.3 "Parameter passing": a parameter declared `(x: V)` for a move-only V
// is by value-move (transfer ownership). The callee's scope-end pass must
// dispose it. Before the fix, `registerUnwindOwnedParam` handled owned params
// for class/interface/collection/FFI-buffer/vtableless-builtin types but had NO
// branch for owning choice parameters — the slot fell through all checks and
// was silently skipped, causing a direct leak.
//
// The defect is a LEAK (sound direction — the compiler fails to emit disposal
// that the spec requires). The repro uses a disposable class W to make the
// owning choice payload visible to LeakSanitizer.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"    // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// -------------------------------------------------------------------
// Defect repro — owning choice parameter leak
// -------------------------------------------------------------------
// An owning choice (`Outer`) carrying a disposable payload (`W`) is passed
// as an `owned` by-value-move parameter to `Take`. The callee's scope-end
// pass does NOT cover it without the fix — the active case's owned payload
// leaks. Amplified 100× per spec rule: one-shot leak rows are vacuous
// oracles.
//
// §15.3 (v0.315): parameter passing for a move-only type is by value-move
// (transfer ownership). The callee's auto-dispose must cover the incoming
// value on every exit path.
//
// Expected without fix: LSan reports ~2400 byte leak (100 × 24 byte W).
// Expected with fix:    0 bytes leaked; "W-DISPOSED-a" appears exactly 100×.

const char* kOwningChoiceParamLeak =
    "module M; "
    "public class W : IDisposable "
    "  public Name: string; "
    "  public constructor(name: string) Name := name; end constructor; "
    "  destructor Console.WriteLine(\"W-DISPOSED-\" + Name); end destructor; "
    "end W; "
    "public choice Outer K(owned w: W), NoK; end Outer; "
    "public class Program "
    "  public static method Take(o: owned Outer): int "
    "    Console.WriteLine(\"nomatch\"); "   // ensures the choice is fully consumed
    "    return 0; "
    "  end Take; "
    "  public static method Main(): int "
    "    var result := 0; "
    "    var i := 0; "
    "    while i < 100 do "
    "      result := Program.Take(Outer.K(new W(\"a\"))); "
    "      i := i + 1; "
    "    end while; "
    "    return result; "
    "  end Main; "
    "end Program;";

}  // namespace

// =====================================================================
// §15.3 (v0.315): parameter passing for a move-only type is by value-move;
// the callee's scope-end auto-dispose must cover the incoming value.
// =====================================================================
BEZEL_TEST(ownership_owning_choice_param_leak,
           owning_choice_param_disposed_at_scope_end_spec_15_3) {
    JitResult r = runAotChecked(kOwningChoiceParamLeak);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip, not a failure

    // Debug: print diagnostics if ran is false
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            fprintf(stderr, "DEBUG diag: %s\n", d.message.c_str());
        }
        fprintf(stderr, "DEBUG ran=%d exitCode=%d stdout_len=%zu\n",
                (int)r.ran, r.exitCode, r.stdoutText.size());
    }

    // The program compiles and runs cleanly.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // LeakSanitizer must report zero leaks with the fix in place.
    // Without the fix: ~2400 bytes leaked (100 × 24 byte W("a") allocations).
    const bool hasLeak = r.stdoutText.find("detected memory leaks") != std::string::npos;
    BEZEL_EXPECT_FALSE(hasLeak);

    // The payload's destructor fires exactly once per construction (100×).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "W-DISPOSED-a"), 100);
}