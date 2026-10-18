// narrow #6429 (§6.5, §17.14, v0.338): `findConstShiftDomainFault` used to
// stop at depth > 64 and return `none` — indistinguishable from "I looked and
// found no fault" — so a shift-domain violation nested 65 or more expression-
// level hops deep was never reported: the program compiled, ran and exited 0.
// §6.5 states the shift-count domain rule with no depth qualifier ("A count
// outside the domain is a fault: a compile error when the count is a
// compile-time constant") and §17.14 quotes the same rule. The cap was a
// bound on this COMPILER rather than on this language.
//
// THE DEFECT. `findConstShiftDomainFault` walked an expression tree with a
// `depth > 64 → return none` cap. Unlike `containsConstDivByZero` (whose
// `depth > 64 → return false` is CONSERVATIVE — "cannot prove non-zero,
// report div-by-zero anyway"), returning `none` here means "no fault found":
// silence is read as an answer, not as "I gave up". A shift-domain violation
// at depth 65+ compiled silently.
//
// THE FIX removes the cap entirely, replacing it with a CYCLE guard
// (`onChain` set) — the same approach as the sibling fix for the identical
// defect in `checkVariancePositions` (#6391, commit 020f8291). The AST is
// strictly tree-shaped (unique_ptr children, no shared nodes), so the guard
// fires only on a malformed self-referential AST and on no well-formed program
// ever fires. The guard is per enclosing path: a node reached by two different
// paths is visited on each, exactly as the original depth-limited walk was.
//
// WHY NO DEEP-REJECT PIN. `findConstShiftDomainFault` is only called when
// `foldIntConst` returns nullopt (shift count unresolvable as a local
// constant). In a single module, any literal integer (like `40`) is resolvable
// at every depth, so `foldIntConst` itself detects the domain fault before
// the probe is ever called.
//
// A two-module repro was attempted: a library exports a const whose value is
// a 65-level deeply nested shift, and the importer uses that const as an
// inline-array size. However, the shift's count (the innermost literal, e.g.
// 40) is itself a literal in the library's source — it resolves to 40 when
// the library is compiled, and `cms->intValue = 40` is stored in metadata.
// When the importer resolves `Lib.COUNT`, `cms->intValue = 40` is returned
// directly (no AST path, no depth cap involved), and `foldIntConst` gets the
// already-resolved value 40 and detects the domain fault immediately. The
// depth cap in `findConstShiftDomainFault` is never reached because the count
// never requires the walk to find it — it is returned as a precomputed value
// from the metadata. The two-module harness (`runAotModules`) can express the
// program but cannot express the shape that would make the cap fire.
//
// THE ROWS.  The reject rows at 1/3/64 levels assert that the total walk
// does not change shallow verdicts (unchanged controls — the cap never fires
// there). The accept rows at all six depths (1/3/64/65/70/200) assert that
// a sound in-domain program compiles cleanly at every depth — real oracles
// for the compilation path.  The deep accept rows are vacuous with respect to
// this fix specifically (they pass with or without the cap removal), but they
// are not vacuous as general compilation oracles.  No existing test
// expectation is changed.  §6.5, §17.14, v0.338.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §6.5 / §17.14 diagnostic content.
const char* kShiftDomain = "is outside the domain";
const char* kShiftDiag  = "(\xC2\xA7""6.5/\xC2\xA7""17.14)";

// Generates a left-nested shift chain:  1 << (1 << (1 << … (1 << count) …))
// with `depth` total shift operators.  The innermost count is always the value
// that distinguishes a REJECT row (out-of-domain, e.g. 40) from an ACCEPT row
// (in-domain, e.g. 0).
std::string nestedShiftProgram(std::size_t depth, int count) {
    std::string inner = std::to_string(count);
    for (std::size_t i = 0; i < depth; ++i)
        inner = "1 << " + inner;
    return "module M; "
           "class C "
           "  public static method Main(): int "
           "    const X: int := " + inner + "; "
           "    return X; "
           "  end Main; "
           "end C;";
}

// REJECT: out-of-domain shift count (e.g. 40 for int, whose domain is 0..31).
void expectShiftFault(std::size_t depth) {
    auto r = runAot(nestedShiftProgram(depth, 40));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kShiftDomain));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kShiftDiag));
}

// ACCEPT: in-domain shift count (0), so the composition compiles cleanly.
// Asserts `r.ran` to rule out the vacuous-oracle shape: a parse failure
// would make the reject row green for the wrong reason.
void expectAccepted(std::size_t depth) {
    auto r = runAot(nestedShiftProgram(depth, 0));
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// --- Shallow depths: unchanged verdicts — the total walk does not break them ---

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_1_fault_rejected) {
    expectShiftFault(1);
}

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_1_in_domain_accepted) {
    expectAccepted(1);
}

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_3_fault_rejected) {
    expectShiftFault(3);
}

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_3_in_domain_accepted) {
    expectAccepted(3);
}

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_64_fault_rejected) {
    expectShiftFault(64);
}

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_64_in_domain_accepted) {
    expectAccepted(64);
}

// --- Deep in-domain accepts: real oracles that compile cleanly at every depth ---

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_65_in_domain_accepted) {
    expectAccepted(65);
}

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_70_in_domain_accepted) {
    expectAccepted(70);
}

BEZEL_TEST(const_shift_domain_depth_total_6429,
           depth_200_in_domain_accepted) {
    expectAccepted(200);
}
