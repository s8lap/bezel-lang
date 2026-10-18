// §11.6 "Nested types" / §12.10 "Inheritance cycles" (issue #6445, v0.338).
//
// §12.10: "An inheritance cycle is a compile error. A type may not, directly
// or transitively, list itself among its base types... The compiler detects
// the cycle and reports an error naming the participating types; the cycle is
// never silently broken or otherwise tolerated." §11.6 admits a class nested
// inside another class, and nothing in §12.10 restricts the rule to top-level
// types, so a cycle whose every member is nested is the same violation.
//
// MEASURED PRE-FIX. `Resolver::checkInheritanceCycles` rooted its DFS at
// `ctx_.module()->children` alone, so a nested type was never a ROOT and a
// cycle among nested types was never entered, never reported. §12.10's
// diagnostic did not simply go missing: with it missing the cycle survived
// into codegen, where `Emitter::classStruct`'s base-chain walk
// (`for (const sema::TypeSymbol* cur = t; cur;) { cur = baseClassOf(cur); }`,
// src/codegen/LlvmBackend_Layout.cpp) has no cycle guard at all and spins
// forever. Observed on r30-c1 at a59b3e5d:
//
//   eu-stack -p <pid of ./build/bezelc cyc.bzl>
//   #0 bezel::codegen::Emitter::classStruct(bezel::sema::TypeSymbol const*)
//   #1 bezel::codegen::Emitter::declareStructs()
//   #2 bezel::codegen::Emitter::run()
//   #3 bezel::codegen::emitModule(...)
//   #4 bezel::Driver::compileExecutableObject(...)
//   #5 bezel::Driver::compileFile(...)
//
// and, on the command line, rc=124 at a 40 s timeout with ZERO output — not
// even a warning. The sibling TOP-LEVEL cycle (`class Q1 : Q2` /
// `class Q2 : Q1`) reported §12.10's diagnostic in 7 ms on the same binary,
// which is what makes this a nesting gap rather than a general one: the
// top-level rows are pinned by
// tests/lit/sema/sema_cycle__{two,three}_class_cycle_diagnoses.bzl and by
// `sema_base_type_path_depth_total_6416.inheritance_cycle_reports_12_10_not_override_mismatch`.
//
// THE FIX is that the nested types become ROOTS (`visitTypeAndNested`, §11.6,
// the one shared recursive walk from sema/NestedTypeWalk.hpp that thirteen
// other passes in this file already use). No walk is bounded and no depth cap
// is added anywhere: a cap would trade the hang for a silent acceptance, which
// §12.10 forbids outright ("never silently broken or otherwise tolerated").
// The base-chain walk in codegen is left alone because §12.10 now rejects the
// program before codegen is reached; see the handback for why that is not
// left as defence-in-depth.
//
// WHY THESE ROWS ARE NOT ALL PINS. Only `nested_two_class_cycle_rejected` and
// `nested_self_cycle_rejected` fail on the pre-fix tree. `top_level_cycle_`
// still reports BOTH participants, which is the regression guard for the one
// subtle thing this fix could have broken: `checkCycleFrom`'s `done` set must
// stay PER ROOT, or the first root's DFS marks a participant "fully
// explored" and the second root returns without reporting it.
//
// THE ANTI-INVERSION CONTROL (`nested_chain_without_cycle_accepted` and its
// siblings) is what makes this test worth having. Adding roots to a cycle
// check is exactly the shape that can reject too much: the fix widens the set
// of DFS roots over a graph walk, and a walk with more roots emits more
// diagnostics by construction. `checkCycleFrom` reports only on a genuine
// re-entry of a symbol already on the CURRENT DFS path, so a well-formed
// hierarchy can emit nothing — but that is the claim, and these rows MEASURE
// it at one level of nesting, at two levels of nesting, and with a nested
// type deriving from a TOP-LEVEL base (the shape §11.6's own conformance row
// tests/conformance/rules/cases/s13_7_0002.bzl uses).

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §12.10's own diagnostic, verbatim from `Resolver::checkCycleFrom`
// (src/sema/TypeResolution_Checks.cpp). Every rejection row below asserts this
// is present AND that the runAot compile-timeout text is absent, so a
// regression to "hangs" cannot be read as a pass.
const char* kCycle = "circular inheritance detected";

// The runAot compile-timeout diagnostic
// (tests/support/aot_support.hpp: "runAot: compile did not finish within
// <N>s; killed (compile hang / runaway front-end or link)"). This is the
// PRE-FIX outcome of every rejection row, so its ABSENCE is half the pin —
// and `failedAtCompileTimeWithRealLocation` already returns false when it is
// present, so the two assertions are not redundant.
const char* kTimedOut = "did not finish within";

const std::string kProg =
    "public class Prog "
    "  public static method Main(): int => 0; "
    "end Prog;";

// A cycle whose every member is a direct member of one top-level class — the
// issue's shape. `private`, so the row is about nesting and visibility is
// orthogonal; the cycle is legal to write and must be diagnosed.
std::string nestedTwoClassCycle() {
    return "module M; public class Outer "
           "  private class P1 : P2 end P1; "
           "  private class P2 : P1 end P2; "
           "end Outer; " + kProg;
}

// §12.10's "simplest case is a type naming itself (`class C: C`)". Nested.
std::string nestedSelfCycle() {
    return "module M; public class Outer "
           "  private class S : S end S; "
           "end Outer; " + kProg;
}

// The same rule as the lit rows pin at top level, and the regression guard for
// `done` staying per root: BOTH participants must still be named.
std::string topLevelCycle() {
    return "module M; public class Q1 : Q2 end Q1; public class Q2 : Q1 end Q2; " +
           kProg;
}

// ANTI-INVERSION. Legal nested inheritance: a nested type deriving from a
// TOP-LEVEL class, another nested type deriving from that, two levels of
// nesting under one outer type. No cycle anywhere.
std::string nestedChainWithoutCycle() {
    return "module M; "
           "public class Base "
           "  public constructor() end constructor; "
           "end Base; "
           "public class Outer "
           "  private class Mid : Base end Mid; "
           "  private class Leaf : Mid end Leaf; "
           "end Outer; " + kProg;
}

// ANTI-INVERSION at DEPTH TWO, which exercises `visitTypeAndNested`'s
// recursion rather than just its first level: `D` names `B.C`, reached
// through two levels of nesting.
std::string twoLevelNestingWithoutCycle() {
    return "module M; "
           "public class A "
           "  public class B "
           "    public class C "
           "    end C; "
           "  end B; "
           "  public class D : B.C end D; "
           "end A; " + kProg;
}

// ANTI-INVERSION, narrowest legal nested case: a nested type deriving from a
// TOP-LEVEL class, the shape §11.6's own conformance row exercises. This is
// the row a fix that "just made nested bases an error" would fail.
std::string nestedDerivesTopLevel() {
    return "module M; "
           "public class Top "
           "  public constructor() end constructor; "
           "end Top; "
           "public class Outer "
           "  private class N : Top end N; "
           "end Outer; " + kProg;
}

// Every rejection row asserts §12.10's diagnostic, a real source location,
// and the ABSENCE of the compile-timeout text.
void expectRejectedWith12_10(const bezel::test::JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCycle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kTimedOut));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
}

}  // namespace

// ===========================================================================
// THE REGRESSION PINS — a cycle among NESTED types is REJECTED with §12.10's
// diagnostic instead of hanging the compiler.
// ===========================================================================

BEZEL_TEST(sema_nested_inheritance_cycle_6445, nested_two_class_cycle_rejected) {
    // MEASURED pre-fix: compile hang, runAot killed at its compile deadline,
    // `kCycle` absent. MEASURED post-fix: rc=1 in 8 ms, both P1 and P2 named.
    expectRejectedWith12_10(runAot(nestedTwoClassCycle()));
}

BEZEL_TEST(sema_nested_inheritance_cycle_6445, nested_self_cycle_rejected) {
    // §12.10's "simplest case", nested. Same pre-fix hang.
    expectRejectedWith12_10(runAot(nestedSelfCycle()));
}

// ===========================================================================
// NOT A PIN — the top-level rule must still diagnose, and must still name
// BOTH participants (`done` stays per root).
// ===========================================================================

BEZEL_TEST(sema_nested_inheritance_cycle_6445, top_level_cycle_still_diagnosed) {
    auto r = runAot(topLevelCycle());
    expectRejectedWith12_10(r);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`Q1`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`Q2`"));
}

// ===========================================================================
// THE ANTI-INVERSION CONTROLS — legal nested hierarchies still ACCEPT. A fix
// that rejects too much is invisible without these.
// ===========================================================================

BEZEL_TEST(sema_nested_inheritance_cycle_6445,
           nested_chain_without_cycle_accepted) {
    auto r = runAot(nestedChainWithoutCycle());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kCycle));
}

BEZEL_TEST(sema_nested_inheritance_cycle_6445,
           two_level_nesting_without_cycle_accepted) {
    auto r = runAot(twoLevelNestingWithoutCycle());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kCycle));
}

BEZEL_TEST(sema_nested_inheritance_cycle_6445,
           nested_derives_top_level_accepted) {
    auto r = runAot(nestedDerivesTopLevel());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kCycle));
}
