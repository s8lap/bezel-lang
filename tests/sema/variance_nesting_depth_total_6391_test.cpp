// fix #6391 (§12.9, §11.6, Appendix C.1, v0.338): the §12.9 position check
// is TOTAL over the module's own type declarations — there is no nesting depth
// at which it stops checking, and in particular no depth at which it stops
// checking SILENTLY.
//
// THE DEFECT. `checkVariancePositions` collected its `varianceSites` by
// descending into §11.6 nested types under a hard depth cap
// (`kMaxTypeNestingDepth = 64`), and the cap returned WITHOUT a diagnostic. §12.9
// and Appendix C.1 state the position rule with no depth qualifier —
// "Position is composed across nested type constructors", and "Variant generic
// parameters (`in`, `out`) appear only in the appropriate position" — while
// §11.6 puts nested types in a type body and states no limit on how deep that
// nesting goes. So the cap was a bound on this COMPILER, not on this language,
// and everything past it was accepted unverified.
//
// MEASURED on the base (`da1ec4d5`): #6383's own shape — `Box<in X>` behind a
// bare name under an `out T` return slot — rejects at depths 1, 3, 58 and 64 and
// COMPILES CLEAN at 65, 70 and 200. Depth 64 is the last rejected level and 65
// the first silently accepted one, matching the cap's `depth > 64` boundary
// exactly.
//
// THE FIX is not a larger constant: it is the removal of the bound, with a
// CYCLE guard (`onChain`) left in its place — a guard that terminates on a
// malformed symbol graph, and on any program the checker accepts never fires.
// That distinction is the point of this file. A REPLACED cap (a bigger literal)
// would leave the same silent-acceptance hole one constant higher; a cap that
// DIAGNOSED would reject a program §12.9 admits, for a reason the spec nowhere
// states — Appendix C enumerates the enforced invariants and no depth limit is
// among them.
//
// WHY THE DEPTHS CHOSEN. Every depth is written out by the generator, and the
// pair at EACH depth is a marker-flipped pair at the same spelling: `Box<in X>`
// reached through a bare name under `Holder<out T>`'s return slot composes to
// contravariant and is rejected, while `Box<out X>` composes to covariant and is
// accepted. A walk that simply stopped anywhere above the violation passes the
// reject row's twin vacuously, so the ACCEPT row is what pins that the walk
// reached the interface rather than merely not complaining — the
// vacuous-oracle shape. The depths span the old boundary: 64 (last rejected on
// the base) and 65 (first accepted), plus 70 and 200 (well past it) so the
// rows are not all satisfied by moving the constant to 65. Depth 1 and 3 are
// the same shape at a nesting the base already handled, and they are what say
// the total walk did not change the shallow verdicts.
//
// The generator builds the declaration nesting programmatically rather than
// writing 200 levels by hand, and the text it emits is asserted to be well
// formed by the ACCEPT rows: a program that failed to PARSE would be rejected
// for a syntax reason on the reject rows and never run on the accept rows, so
// `r.ran` is asserted on every accept row and a `§12.9`-bearing
// `failedAtCompileTimeWithRealLocation` on every reject row. A row that is green
// because it does not parse cannot pass here.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §12.9's diagnostic, quoted whole. Every reject row asserts the POLARITY
// sentence too, so a walk that answered with a constant, or read the polarity
// off the enclosing class (a class is always invariant, §12.9), is caught by
// the pair rather than by one row.
const char* kVariance129 = "(§12.9)";
const char* kComposedClause = "composed through a nested type constructor";
const char* kOutInInput =
    "is covariant (`out`) but appears in an input (parameter) position";

constexpr const char* kMain =
    "class Program "
    "  public static method Main() Console.WriteLine(\"x\"); end Main; "
    "end Program; ";

// #6383's shape at an ARBITRARY nesting depth: `depth` nested classes `L0 …
// L{depth-1}` enclosing a nested `Box<marker X>` and a nested
// `Holder<out T>` naming it by BARE NAME in a return slot.
//
// The nesting is what the removed cap bounded, so `depth` is the axis every
// row varies and nothing else does: the two decls under test are byte-identical
// at every depth, and the flip between a reject row and its accept row is the
// nested head's own `in`/`out` marker and nothing else.
std::string program(std::size_t depth, bool contravariantNestedHead) {
    std::string src = "module M; import System;\npublic class L0\n";
    for (std::size_t i = 1; i < depth; ++i)
        src += "public class L" + std::to_string(i) + "\n";
    src += "public interface Box<";
    src += contravariantNestedHead ? "in" : "out";
    src += " X> method ";
    src += contravariantNestedHead ? "Put(x: X);" : "Get(): X;";
    src += " end Box;\n";
    src += "public interface Holder<out T> method Get(): Box<T>; end Holder;\n";
    // Every nested `end` carries its semicolon — without it the generated
    // program is rejected by `expected ';' after 'end'` and the whole sweep is
    // a vacuous green (a reject row green because it does not parse).
    for (std::size_t i = depth; i-- > 0;) src += "end L" + std::to_string(i) + ";\n";
    return src + kMain;
}

// REJECT with §12.9, at the exact diagnostic this issue is about.
void expectVariance129(std::size_t depth) {
    auto r = runAot(program(depth, /*contravariantNestedHead=*/true));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutInInput));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
}

// ACCEPT: the sound composition at the same depth, so the walk demonstrably
// REACHED the interface rather than merely declining to object.
void expectAccepted(std::size_t depth) {
    auto r = runAot(program(depth, /*contravariantNestedHead=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// --- depths the base already checked: the walk is UNCHANGED there ------------
//
// The shallow rows are the control for every deep row. A fix that simply
// deleted the check (rather than making it total) would pass all four deep
// reject rows and fail both of these.

BEZEL_TEST(variance_nesting_depth_total_6391, depth_1_violation_rejected) {
    expectVariance129(1);
}

BEZEL_TEST(variance_nesting_depth_total_6391, depth_1_sound_composition_accepted) {
    expectAccepted(1);
}

BEZEL_TEST(variance_nesting_depth_total_6391, depth_3_violation_rejected) {
    expectVariance129(3);
}

BEZEL_TEST(variance_nesting_depth_total_6391, depth_3_sound_composition_accepted) {
    expectAccepted(3);
}

// --- 64: the LAST level the cap visited, and the boundary itself -------------

BEZEL_TEST(variance_nesting_depth_total_6391, depth_64_violation_rejected) {
    expectVariance129(64);
}

BEZEL_TEST(variance_nesting_depth_total_6391,
           depth_64_sound_composition_accepted) {
    expectAccepted(64);
}

// --- 65: THE FIRST LEVEL THE CAP SILENTLY ACCEPTED --------------------------
//
// MEASURED on the base: this exact program compiles clean, runs, and exits 0.
// It is the fail-without repro.

BEZEL_TEST(variance_nesting_depth_total_6391, depth_65_violation_rejected) {
    expectVariance129(65);
}

BEZEL_TEST(variance_nesting_depth_total_6391,
           depth_65_sound_composition_accepted) {
    expectAccepted(65);
}

// --- 70: past the boundary by a margin, so a raised constant cannot pass -----
//
// A mutant that RAISES `kMaxTypeNestingDepth` from 64 to 65 — the smallest
// change that closes the depth-65 row — survives rows 1/3/64/65 and dies here.

BEZEL_TEST(variance_nesting_depth_total_6391, depth_70_violation_rejected) {
    expectVariance129(70);
}

BEZEL_TEST(variance_nesting_depth_total_6391,
           depth_70_sound_composition_accepted) {
    expectAccepted(70);
}

// --- 200: an order of magnitude past the boundary ---------------------------
//
// MEASURED on the base: clean and rc=0. Also the row that says the total walk
// does not make a deeply nested LEGAL program expensive — the enclosing chain
// `heads.enclosingChain` is now 200 long, and every bare head resolves through
// it.

BEZEL_TEST(variance_nesting_depth_total_6391, depth_200_violation_rejected) {
    expectVariance129(200);
}

BEZEL_TEST(variance_nesting_depth_total_6391,
           depth_200_sound_composition_accepted) {
    expectAccepted(200);
}