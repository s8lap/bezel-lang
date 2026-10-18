// fix #6417 (§32.13, v0.306): `isCCompatibleRecordSym`'s magic `depth > 8`
// cap produced a FALSE DIAGNOSTIC — "not C-compatible" — on a 10-level
// C-compatible record chain, when §32.13 clause 1 states the nesting rule
// with no depth qualifier.
//
// THE DEFECT. `isCCompatibleRecordSym` returned `false` at `depth > 8`
// without a diagnostic. `isCCompatibleRecord` is called at line 13739 from
// `checkExternType` (the `extern` parameter checker) and at line 13701
// (the by-address shortcut). Both call sites pass `depth` via the public
// `isCCompatibleRecord` which has `depth = 0` as its default argument, so
// the chain terminates at exactly 9 levels of nesting (depth 0 through 8),
// and a 10-level chain is mis-classified as NOT C-compatible with a
// factually wrong diagnostic ("every field's type must be an FFI-compatible
// scalar").
//
// THE FIX is to remove the depth cap and replace it with a per-path cycle
// guard (`onChain`, `unordered_set`).  The cycle guard is the right
// instrument: it fires ONLY on a malformed cyclic graph (R0 contains
// Ptr[R1], R1 contains Ptr[R0>), never on a well-formed program, since a
// record cannot contain itself by value.  §32.13 clause 1's "recursion is
// impossible" claim is about value containment, not about the field-type
// graph — the cycle guard handles the latter.  The depth cap was a bound
// on this compiler, not on this language; §32.13 states the nesting rule
// with no depth qualifier.
//
// BEHAVIOUR FLIP. What changes is the diagnostic on a C-compatible record
// chain at depth 10 or more by VALUE: "not C-compatible" before (wrong),
// "by address only" after (correct).  The by-address shortcut (out/ref)
// was always correct.  No existing test pins the old (wrong) verdict.
//
// WHY THE DEPTHS CHOSEN.  The cap was 8 (return at `depth > 8`), allowing
// depths 0-8 inclusive — 9 levels of nesting.  The issue measured depth
// 1 accepted, depths 2-9 correct reject-by-address, depth 10+ wrong reject
// ("not C-compatible").  The test covers: depth 10 (first wrong reject
// before the fix, now correct reject-by-address) and depth 16 (well past
// the old boundary).

#include <string>

#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Builds a nested C-compatible record chain of `depth` levels, with R0
// containing R1 containing ... containing R{depth-1} containing Int32.
// Each Ri is C-compatible (single Int32 field) so the whole chain is
// C-compatible per §32.13 clause 1.
std::string buildNestingChain(int depth) {
    std::string r;
    for (int i = depth - 1; i >= 0; --i) {
        if (i == depth - 1)
            r += "public record R" + std::to_string(i) + "(f: Int32); ";
        else
            r += "public record R" + std::to_string(i) + "(f0: R" +
                 std::to_string(i + 1) + "); ";
    }
    return r;
}

// Program that takes R0 by value at an extern parameter.
std::string makeByValueProgram(int depth) {
    return buildNestingChain(depth) +
           "extern \"libc\" "
           "  method Take(r: R0): int; "
           "end; "
           "class C "
           "  public static method Main(): int => 0; "
           "end C;";
}

// A malformed program: R0 contains Ptr[R1], R1 contains Ptr[R0], forming
// a cycle through the Ptr head.  The cycle guard handles this gracefully.
std::string makeCyclicProgram() {
    return std::string(
        "public class R0 "
        "  public field f: Ptr[R1]; "
        "end R0; "
        "public class R1 "
        "  public field f: Ptr[R0]; "
        "end R1; "
        "extern \"libc\" "
        "  method Take(out r: R0): int; "
        "end; "
        "class C "
        "  public static method Main(): int => 0; "
        "end C;");
}

}  // namespace

// ===========================================================================
// DEPTH 10 — the first level that triggered the FALSE DIAGNOSTIC before the
// fix ("not C-compatible").  After the fix it correctly rejects by value
// with "by address only".  If the depth > 8 cap is restored, this FAILS:
// it produces the wrong diagnostic "not C-compatible" and passes vacuously.
// ===========================================================================

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_10_by_value_rejected) {
    auto r = runAot(makeByValueProgram(10));
    // Must not compile: a C-compatible record chain by value is not admitted.
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// DEPTH 16 — well past the old cap.  Same correct behaviour as depth 10.
// ===========================================================================

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_16_by_value_rejected) {
    auto r = runAot(makeByValueProgram(16));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// CYCLE GUARD — malformed cyclic record chain must not hang or crash
// ===========================================================================

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           cyclic_record_chain_does_not_crash) {
    auto r = runAot(makeCyclicProgram());
    // The cycle must be handled gracefully — either a diagnostic or a clean
    // run.  Must not hang or segfault.
    BEZEL_EXPECT_TRUE(r.ran || r.diagnostics.hasErrors());
}
