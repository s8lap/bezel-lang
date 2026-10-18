// fix #6408 (§12.9, §27.8, §11.6, Appendix C.1, v0.338): the §12.9 composed
// position walk follows a type-alias chain to its END, at every chain length,
// and gives up only on a CYCLE — which §27.8 rejects in its own right.
//
// THE DEFECT. `namedHeadArgVariance`'s local-alias arm followed
// `type A<T> := B<T>; type B<T> := C<T>; …` under a hard hop bound
// (`kMaxAliasSeeThroughDepth = 32`) and, past it, returned `Unknown` WITHOUT a
// diagnostic. `composedVarianceViolation` treats `Unknown` as "no violation
// found" (it skips rather than guesses, so that an unresolvable head cannot
// manufacture a false rejection), so a real §12.9 violation past the 32nd hop
// was silently ACCEPTED.
//
// The constant's own comment justified it as a cycle guard — "§27.8 makes a
// cycle a compile error … so on a program the checker accepts there is nothing
// to hit this" — and that justification covers CYCLES ONLY. A long NON-CYCLIC
// alias chain is perfectly legal: §27.8 puts no limit on how many aliases one
// may chain through, and §11.6 states no limit on type nesting. So the bound
// fired on ordinary programs, silently, on a rule §12.9 states unconditionally.
//
// MEASURED on the base (`04d402b3`): this file's own generator, with
// `type A0<T> := A1<T>; … type A{n}<T> := Array<T>;` under
// `Holder<out T>.Get(): A0<T>`, rejects at n = 5 and n = 31 with "(§12.9)"
// and COMPILES CLEAN (rc = 0, not a skipped run) at n = 32, 33 and 40. The
// `n = 32` row is the fail-without repro.
//
// 32 IS the bound, not one past it. The guard read
// `if (depth >= kMaxAliasSeeThroughDepth) return Unknown;`, tested BEFORE the
// hop with the walk entering `A0` at `depth == 0`, so it stopped entering at
// `depth == 32`: it declined `A32` and composed `A0..A31`. The sibling cap in
// `TypeResolution_Checks.cpp` (#6391's `kMaxTypeNestingDepth = 64`) is the
// same shape with a STRICT `>` rather than a `>=`, so it admits 64 and first
// declines at 65 — one past ITS bound. The two differ by one comparison
// operator, and that is why their boundaries land one value apart; neither is
// an off-by-one.
//
// WHY THE BOUND WAS REMOVED RATHER THAN RAISED. §12.9 states the position
// rule with no depth qualifier — "Position is composed across nested type
// constructors", one rule for every interface — and Appendix C.1:11596 lists
// the invariant unconditionally ("Variant generic parameters (`in`, `out`)
// appear only in the appropriate position"). "The compiler cannot decide this
// deep" is therefore not a spec situation; it is an implementation shortcut
// creating one, and a larger literal only moves the hole one constant higher.
// A cap that DIAGNOSED was rejected for the mirror reason: it would make a
// long-but-LEGAL chain un-compilable for a reason the spec nowhere states,
// which is a spec ruling rather than an implementation fix.
//
// THE CYCLE GUARD, AND THE ORDERING QUESTION. What replaced the bound is a
// per-path set: an alias is followed only when it is not already on the chain
// being composed. A cycle guard that fired BEFORE §27.8's own diagnostic would
// convert a REJECTED program into a silently accepted one — the same defect
// class, relocated — so the ordering is pinned rather than assumed:
//
//   * alias_cycle_rejected_27_8 — a 3-alias cycle still reports §27.8 and
//     still exits non-zero. This row is the one that would go red if the
//     guard could preempt the diagnostic.
//
// The ordering is structural, not lucky: §27.8's "type alias `X` is recursive"
// report is emitted from `Resolver::resolveNamed` during NAME RESOLUTION,
// which runs inside `TypeResolution::run` long before `checkVariancePositions()`
// is reached at `TypeResolution.cpp:747`. And what the guard returns on a cycle
// is `Unknown` — the same conservative non-answer an unresolvable head yields —
// so even if the order were reversed the cycle is still REJECTED, by §27.8,
// and this walk merely declines to add a second opinion.
//
// WHY EVERY DEPTH IS WRITTEN BY A GENERATOR. The chain is the axis the removed
// bound bounded, so `chainLength` is the only thing that varies down the
// reject rows, and — this is the vacuous-oracle control — the ONLY difference
// between a reject row and its accept twin is the terminal element's own
// variance. `Array<T>` is a class and so always invariant (§12.9): an `out T`
// reaching it is a violation at EVERY chain length. `IReadOnlyList<T>` is
// covariant, so the identical chain composes soundly at every length. A walk
// that stopped anywhere above the violation would therefore pass the reject rows
// vacuously while the accept twins stayed green; the pairs are what make the
// reject rows mean "the walk reached the end", not "the walk gave up".
//
// The depths span the old boundary: 31 (last rejected on the base) and 32 (first
// accepted), plus 33, 40 and 64 so the rows are not all satisfied by moving the
// constant to 33. Depth 5 is the shape the base already handled and is the
// control against a fix that simply DELETED the alias arm.
//
// The cross product the axis is swept across: terminal element {array, set,
// dict, queue, contravariant head, function} x chain shape {linear, two hops
// per level, diamond} x variance {covariant, contravariant}. Each terminal has
// a violating twin and a sound twin at EVERY depth, so a walk that mis-resolves
// any single terminal is caught at the depth where it is decisive.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §12.9's diagnostic, quoted whole. Every reject row asserts the POLARITY
// sentence too, so a walk that answered with a constant — or read the polarity
// off the wrong terminal — is caught by the pair, not by one row.
const char* kVariance129 = "(§12.9)";
const char* kComposedClause = "composed through a nested type constructor";
const char* kOutInInput =
    "is covariant (`out`) but appears in an input (parameter) position";

// §27.8's cycle diagnostic. Asserted on the cycle rows so a guard that
// SUPPRESSED it (the relocated-defect shape) could not pass.
const char* kAliasCycle = "is recursive (\xC2\xA7""27.8)";

constexpr const char* kMain =
    "class Program "
    "  public static method Main() Console.WriteLine(\"x\"); end Main; "
    "end Program; ";

// What a chain of `chainLength` alias hops terminates in. `text` is the
// alias BODY (written with the chain's own parameter `T`), and `sound` is the
// same body with the head's marker flipped — the pair that makes each terminal
// a vacuous-oracle control rather than a single verdict.
struct Terminal {
    const char* name;
    const char* violatingBody;
    const char* soundBody;
};

// §12.9 names the built-ins' variance, so each terminal has a real answer at
// the end of the chain and none of them is `Unknown`. Every `violatingBody`
// below was MEASURED to compose to a contravariant position through a long
// chain and to be rejected for exactly this reason — the pairs are not
// constructed from the spec's prose alone, because a terminal that resolved
// to `Unknown` would make its reject row vacuous:
//
//  * Array<T> / Set<T> / Dictionary<K, V> / Queue<T> are CLASSES — always
//    invariant, so no marker survives them.
//  * IEvent<out T> is the spec's own worked example, so `T` inside it lands
//    in a contravariant slot (a method parameter).
//  * A function type `(T) -> ()` is contravariant in its parameter (§12.9).
//  * A user-declared generic INTERFACE head, whose own marker the walk reads
//    through the chain exactly as it reads a built-in's.
//
// Every `soundBody` is one of the NINE covariant built-in interfaces (the
// `kCovariant` table at `builtinHeadIsCovariantAtZero`), so the sound twin of
// each terminal composes to a covariant position at EVERY chain length. The
// sound twin is not a formality: it is what makes the reject row mean "the
// walk reached the end of the chain" rather than "the walk gave up".
const Terminal kTerminals[] = {
    {"array", "Array<T>", "IReadOnlyList<T>"},
    {"set", "Set<T>", "IEnumerable<T>"},
    {"dict", "Dictionary<T, T>", "IReadOnlyCollection<T>"},
    {"queue", "Queue<T>", "IEnumerator<T>"},
    {"contravariant_head", "IEat<T>", "IEvent<T>"},
    {"function", "(T) -> ()", "IAsyncEnumerable<T>"},
};

// `IEat<T>` declares T contravariantly, so an alias chain ending in it puts
// `out T` in an input position — a violation at every length. `IEat` is
// declared alongside every program so the `contravariant_head` terminal has a
// head to name.
constexpr const char* kSupportDecls =
    "public interface IEat<in T> method Put(x: T); end IEat; ";

std::string program(std::size_t chainLength, const char* terminalBody) {
    std::string src = "module M; import System;\n";
    src += kSupportDecls;
    for (std::size_t i = 0; i < chainLength; ++i)
        src += "type A" + std::to_string(i) + "<T> := A" +
               std::to_string(i + 1) + "<T>;\n";
    src += std::string("type A") + std::to_string(chainLength) +
           "<T> := " + terminalBody + ";\n";
    src += "public interface Holder<out T> method Get(): A0<T>; end Holder;\n";
    return src + kMain;
}

// Two alias hops per LEVEL, so `chainLength` LEVELS is `2 * chainLength` hops.
// This is still a LINEAR run — it is not a branching shape, and it is not
// labelled one. What it buys is a different unit: a mutant that kept a bound
// of 32 and counted LEVELS instead of HOPS would pass every other row here and
// die on these, because 32 levels is 64 hops. The genuine DAG shape is
// `diamondProgram` below.
std::string twoHopsPerLevelProgram(std::size_t chainLength,
                                   const char* terminalBody) {
    std::string src = "module M; import System;\n";
    src += kSupportDecls;
    std::size_t hops = chainLength * 2;
    for (std::size_t i = 0; i < hops; ++i)
        src += "type A" + std::to_string(i) + "<T> := A" +
               std::to_string(i + 1) + "<T>;\n";
    src += std::string("type A") + std::to_string(hops) + "<T> := " +
           terminalBody + ";\n";
    src += "public interface Holder<out T> method Get(): A0<T>; end Holder;\n";
    return src + kMain;
}

// A genuine DAG, and the only branching shape in this file: ONE root interface
// reaches the terminal alias `Deep` through TWO distinct aliases. Two spines
// of `chainLength` hops each — `S0 -> … -> S{n} -> Arm1` and
// `T0 -> … -> T{n} -> Arm2` — and `Arm1` and `Arm2` both resolve to `Deep`,
// which alone carries the terminal body. So `Deep` is composed TWICE, once
// down each arm, within one program.
//
// BOTH spines are long on purpose. A first draft gave `Get2()` the short arm
// (`Arm2` entered at depth 0, `Deep` two hops down), and MEASURED on the base
// those rows went GREEN — the short route reached `Deep` without ever touching
// the hop cap, so the program was rejected on the base for a reason that had
// nothing to do with the fix. A diamond that does not report the hole is a
// diamond that pins nothing. With both spines at `chainLength >= 32`, neither
// route reaches `Deep` past the base's cap of 32, and the rows go red.
//
// What this pins: that a DAG is walked to its end, and that a node shared by
// two routes is composed on both — `Deep` is pushed and popped per path, not
// latched.
//
// What it deliberately does NOT claim: it cannot tell PER-PATH dedup from a
// global `seen`. An alias has exactly ONE body, so composing it by two routes
// yields the same position either way and no verdict here distinguishes the two
// policies. Stating that is the point — a row that read as if it discriminated
// them would be the same over-claim as the mislabelled section it replaced.
std::string diamondProgram(std::size_t chainLength, const char* terminalBody) {
    std::string src = "module M; import System;\n";
    src += kSupportDecls;
    for (std::size_t i = 0; i < chainLength; ++i) {
        std::string n = std::to_string(i);
        src += "type S" + n + "<T> := S" + std::to_string(i + 1) + "<T>;\n";
        src += "type T" + n + "<T> := T" + std::to_string(i + 1) + "<T>;\n";
    }
    std::string n = std::to_string(chainLength);
    src += "type S" + n + "<T> := Arm1<T>;\n";  // spine 1 ends on Arm1
    src += "type T" + n + "<T> := Arm2<T>;\n";  // spine 2 ends on Arm2
    src += "type Arm1<T> := Deep<T>;\n";
    src += "type Arm2<T> := Deep<T>;\n";
    src += "type Deep<T> := " + std::string(terminalBody) + ";\n";
    src += "public interface Holder<out T> method Get(): S0<T>; "
           "method Get2(): T0<T>; end Holder;\n";
    return src + kMain;
}

// REJECT with §12.9, at the exact diagnostic this issue is about.
void expectVariance129(std::size_t chainLength, const Terminal& t) {
    auto r = runAot(program(chainLength, t.violatingBody));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutInInput));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
}

void expectVariance129TwoHops(std::size_t chainLength, const Terminal& t) {
    auto r = runAot(twoHopsPerLevelProgram(chainLength, t.violatingBody));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutInInput));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
}

// ACCEPT: the sound composition at the same chain length, so the walk
// demonstrably REACHED the end of the chain rather than merely declining to
// object. `r.ran` is asserted so a row cannot be green because the program
// failed to PARSE.
void expectAccepted(std::size_t chainLength, const Terminal& t) {
    auto r = runAot(program(chainLength, t.soundBody));
    BEZEL_EXPECT_TRUE(r.ran);
}

void expectAcceptedTwoHops(std::size_t chainLength, const Terminal& t) {
    auto r = runAot(twoHopsPerLevelProgram(chainLength, t.soundBody));
    BEZEL_EXPECT_TRUE(r.ran);
}

void expectVariance129Diamond(std::size_t chainLength, const Terminal& t) {
    auto r = runAot(diamondProgram(chainLength, t.violatingBody));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kOutInInput));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
}

void expectAcceptedDiamond(std::size_t chainLength, const Terminal& t) {
    auto r = runAot(diamondProgram(chainLength, t.soundBody));
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// --- the CYCLES: the ordering this fix had to be checked for ---------------

// §27.8's own diagnostic, unchanged by this commit. If the cycle guard could
// fire before it, this row goes green-to-RED, which is the point.
BEZEL_TEST(variance_alias_chain_total_6408, alias_cycle_rejected_27_8) {
    auto r = runAot(std::string("module M; import System;\n"
                                "type A0<T> := A1<T>;\n"
                                "type A1<T> := A2<T>;\n"
                                "type A2<T> := A0<T>;\n") +
                    kSupportDecls +
                    "public interface Holder<out T> method Get(): A0<T>; "
                    "end Holder;\n" +
                    kMain);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAliasCycle));
}

// A SELF-referential alias (the shortest cycle §27.8 names) carrying a real
// §12.9 violation at its use site. Two things at once: the cycle is still
// rejected, and the walk TERMINATES rather than recursing until the stack is
// exhausted — the hazard the deleted bound existed to prevent, and the reason
// a cycle guard is the right replacement.
BEZEL_TEST(variance_alias_chain_total_6408, self_referential_alias_rejected_27_8) {
    auto r = runAot(std::string("module M; import System;\n"
                                "type A0<T> := Array<A0<T>>;\n") +
                    kSupportDecls +
                    "public interface Holder<out T> method Get(): A0<T>; "
                    "end Holder;\n" +
                    kMain);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAliasCycle));
}

// A cyclic chain where the CYCLE IS NOT YET SPELLED AT ANY USE SITE: the
// diagnostic must come from §27.8 at the declarations, not from this walk, and
// the program must still be rejected. This is the shape where a guard that
// fired first would be least visible.
BEZEL_TEST(variance_alias_chain_total_6408, unused_cyclic_alias_still_rejected_27_8) {
    auto r = runAot(std::string("module M; import System;\n"
                                "type A0<T> := A1<T>;\n"
                                "type A1<T> := A0<T>;\n"
                                "public interface I<out T> method Get(): "
                                "IReadOnlyList<T>; end I;\n") +
                    kMain);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kAliasCycle));
}

// --- depths the base already checked: the walk is UNCHANGED there ----------

BEZEL_TEST(variance_alias_chain_total_6408, depth_5_violation_rejected) {
    expectVariance129(5, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_5_sound_composition_accepted) {
    expectAccepted(5, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_31_violation_rejected) {
    expectVariance129(31, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_31_sound_composition_accepted) {
    expectAccepted(31, kTerminals[0]);
}

// --- 32: THE FIRST LENGTH THE BOUND SILENTLY ACCEPTED ---------------------
//
// MEASURED on the base: this exact program compiles clean, runs, and exits 0.
// It is the fail-without repro.

BEZEL_TEST(variance_alias_chain_total_6408, depth_32_violation_rejected) {
    expectVariance129(32, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_32_sound_composition_accepted) {
    expectAccepted(32, kTerminals[0]);
}

// --- past the boundary by a margin, so a raised constant cannot pass --------

BEZEL_TEST(variance_alias_chain_total_6408, depth_33_violation_rejected) {
    expectVariance129(33, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_33_sound_composition_accepted) {
    expectAccepted(33, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_40_violation_rejected) {
    expectVariance129(40, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_40_sound_composition_accepted) {
    expectAccepted(40, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_64_violation_rejected) {
    expectVariance129(64, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_64_sound_composition_accepted) {
    expectAccepted(64, kTerminals[0]);
}

// --- the CROSS PRODUCT: every terminal at every decisive depth -------------
//
// Each terminal is swept at 32 (the fail-without depth), at 33 (so a mutant
// that special-cases exactly 32 dies), and at 64 (an order of magnitude of
// boundary past 32's own). The terminal's SOUND twin is swept at 32 and 64 as
// well, so no terminal can pass by being skipped outright.

BEZEL_TEST(variance_alias_chain_total_6408, terminal_set_violation_rejected) {
    expectVariance129(32, kTerminals[1]);
    expectVariance129(33, kTerminals[1]);
    expectVariance129(64, kTerminals[1]);
}

BEZEL_TEST(variance_alias_chain_total_6408, terminal_set_sound_accepted) {
    expectAccepted(32, kTerminals[1]);
    expectAccepted(64, kTerminals[1]);
}

BEZEL_TEST(variance_alias_chain_total_6408, terminal_dict_violation_rejected) {
    expectVariance129(32, kTerminals[2]);
    expectVariance129(64, kTerminals[2]);
}

BEZEL_TEST(variance_alias_chain_total_6408, terminal_dict_sound_accepted) {
    expectAccepted(32, kTerminals[2]);
    expectAccepted(64, kTerminals[2]);
}

// A chain ending in a CLASS head is a violation at every length, and the
// identical chain ending in a covariant built-in is sound at every length.
// `queue` is one such pair (`Queue<T>` against `IEnumerator<T>`), swept with
// two different covariant sound twins elsewhere so no single covariant head
// can be the only thing the sound rows exercise.
BEZEL_TEST(variance_alias_chain_total_6408, terminal_queue_violation_rejected) {
    expectVariance129(32, kTerminals[3]);
    expectVariance129(64, kTerminals[3]);
}

BEZEL_TEST(variance_alias_chain_total_6408, terminal_queue_sound_accepted) {
    expectAccepted(32, kTerminals[3]);
    expectAccepted(64, kTerminals[3]);
}

// A chain ending in a USER-DECLARED CONTRAVARIANT interface head: the walk
// must read the head's own `in` marker through the whole chain, at every
// length. Its sound twin ends at the covariant built-in.
BEZEL_TEST(variance_alias_chain_total_6408, terminal_contravariant_head_violation_rejected) {
    expectVariance129(32, kTerminals[4]);
    expectVariance129(64, kTerminals[4]);
}

BEZEL_TEST(variance_alias_chain_total_6408,
           terminal_contravariant_head_sound_accepted) {
    expectAccepted(32, kTerminals[4]);
    expectAccepted(64, kTerminals[4]);
}

// A chain ending in a FUNCTION TYPE: §12.9's "`(T) -> ()` is contravariant in
// its parameters" composed through 32 and 64 alias hops. This is the terminal
// with the deepest non-alias nesting, so it is the one that would expose a
// bound that counted the wrong thing.
BEZEL_TEST(variance_alias_chain_total_6408, terminal_function_violation_rejected) {
    expectVariance129(32, kTerminals[5]);
    expectVariance129(64, kTerminals[5]);
}

BEZEL_TEST(variance_alias_chain_total_6408, terminal_function_sound_accepted) {
    expectAccepted(32, kTerminals[5]);
    expectAccepted(64, kTerminals[5]);
}

// --- two alias hops per LEVEL (still linear — see the header at the helper) --
//
// 32 LEVELS here is 64 HOPS, so a mutant that kept the bound at 32 and merely
// counted levels instead of hops would pass every other linear row and die
// here. 40 levels is 80 hops.

BEZEL_TEST(variance_alias_chain_total_6408,
           two_hops_per_level_depth_32_violation_rejected) {
    expectVariance129TwoHops(32, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408,
           two_hops_per_level_depth_32_sound_composition_accepted) {
    expectAcceptedTwoHops(32, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408,
           two_hops_per_level_depth_40_violation_rejected) {
    expectVariance129TwoHops(40, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408,
           two_hops_per_level_depth_40_sound_composition_accepted) {
    expectAcceptedTwoHops(40, kTerminals[0]);
}

// --- the DIAMOND shape: one root, two routes, one terminal alias -----------
//
// The only genuinely branching program in this file. `Deep` — the alias that
// carries the terminal body — is reached down TWO distinct arms, each behind
// its own full-length spine, so it is composed twice in one program. Both
// spines clear the base's cap of 32 before `Deep` is reached at all, which is
// what makes these four rows go RED on the base (MEASURED — the first draft
// gave `Get2()` a short arm and these rows went GREEN there; see the helper).

BEZEL_TEST(variance_alias_chain_total_6408, diamond_depth_32_violation_rejected) {
    expectVariance129Diamond(32, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408,
           diamond_depth_32_sound_composition_accepted) {
    expectAcceptedDiamond(32, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, diamond_depth_40_violation_rejected) {
    expectVariance129Diamond(40, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408,
           diamond_depth_40_sound_composition_accepted) {
    expectAcceptedDiamond(40, kTerminals[0]);
}

// --- a long chain whose composition is SOUND must still compile ------------
//
// 200 hops of a covariant chain, run end to end. This is the row that says
// removing the bound does not make a long LEGAL program expensive or
// un-compilable — the mirror of the "a cap that DIAGNOSED would be a spec
// ruling" argument in the fix. It is the ACCEPT twin of the 200-hop reject.
BEZEL_TEST(variance_alias_chain_total_6408, depth_200_violation_rejected) {
    expectVariance129(200, kTerminals[0]);
}

BEZEL_TEST(variance_alias_chain_total_6408, depth_200_sound_composition_accepted) {
    expectAccepted(200, kTerminals[0]);
}

// --- the SHALLOW rows #6354 already pins, re-pinned through this guard ----
//
// #6354's `out_t_under_chained_alias_rejected` and its accepted twin are the
// 2-hop and 3-hop cases. They are unchanged by this commit; they are restated
// here only in the ONE shape this commit could have altered — a chain that
// ends at a USER-DECLARED head, which the 2-hop path reaches through the same
// arm the guard now sits in.

BEZEL_TEST(variance_alias_chain_total_6408, two_hop_chain_to_user_head_rejected) {
    expectVariance129(2, kTerminals[4]);
}

BEZEL_TEST(variance_alias_chain_total_6408, two_hop_chain_to_user_head_accepted) {
    expectAccepted(2, kTerminals[4]);
}