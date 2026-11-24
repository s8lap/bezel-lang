// Regression test for issue #9016 — a LAMBDA-BODY owned value LEAKED where the
// byte-identical METHOD twin did not.
//
// THE DEFECT. Inside a lambda body:
//
//   var x := new Res(1); if flag then _ := x; return 1; end; x.Dispose(); return 2;
//
// the value owned by `x` was never reclaimed on the path that discarded it
// (`_ := x;`). MEASURED on the base r30-c1 @ b694c015e with
// `scripts/agent_check.sh --probe --asan`:
//
//   Direct leak of 160 byte(s) in 10 object(s) allocated from:
//       #1 ... in lam$0 bezel
//   SUMMARY: AddressSanitizer: 160 byte(s) leaked in 10 allocation(s).
//
// (10 objects over the 20 iterations of the issue's own probe: the ten
// iterations on which `flag` is true, i.e. the DISCARD path.) The METHOD twin
// of the same statements does not leak.
//
// THE ROOT CAUSE (two halves, both in the ownership pass's LAMBDA-body
// handling — `src/ownership/Ownership.cpp`):
//
//   1. The scope-end auto-drop was decided PATH-BLIND. A METHOD body's owned
//      local is auto-dropped at its scope end when the per-path dataflow's
//      `endState` says `Owned` && !`maybeConsumed`; an `Owned ⊓ Disposed`
//      join keeps `Owned` (an explicit `Dispose()` NULLS the slot, so the
//      null-guarded scope-end drop disposes only the still-owned branch). A
//      LAMBDA body has no `endState` — the ownership pass does not run the
//      dataflow over lambda bodies — so its locals fell to the method-level
//      SYNTACTIC sweep, whose gate is the MONOTONE `everDischarged_` set:
//      "discharged on SOME path" read as "discharged on EVERY path". A
//      lambda-body local disposed on one branch therefore got NO scope-end
//      auto-drop, and the branch that still owned it leaked.
//
//   2. The discard itself was not the K3.9 kill it is. §28.3's consume table
//      (K3.9) and §17.13 make `_ := x;` of a named owned binding an
//      ANNOUNCED CONSUME — the value is disposed at the end of the discard
//      statement and the binding is left explicitly-disposed. The METHOD
//      body's `visitStmt` DiscardStmt arm records that (`markExplicitlyDisposed`
//      + `ast::DiscardStmt::discardOutOwnedVar`, which is the flag codegen
//      reads to emit the disposal); the LAMBDA body's arm recorded NOTHING,
//      so codegen emitted no disposal at the discard.
//
//   The two halves are one fix: with (2) the discard discharges the binding
//   on its path and the exit-state join is `Disposed` (no scope-end drop
//   needed — the METHOD twin's verdict), and with (1) a binding that is
//   discharged on only SOME path still gets the scope-end drop on the paths
//   that still own it (again the METHOD twin's verdict).
//
// THE SPEC. §28.3.10 *Owned local discharge requirement* (v0.359): "An owned
// local is disposed exactly once on every control-flow path … When an owned
// local — or an `owned` parameter — reaches a *normal-exit* scope end without
// having reached a discharge point, and is not in moved-from state, it is
// disposed implicitly at that scope end"; "The rule is uniform across body
// kinds". §28.3's consume table K3.9 (v0.359) and §17.13 (v0.359) make the
// discard of a named owned binding an explicit disposal. §28.9 (v0.359) — the
// closure's capture cells.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own stdout AND exit code AND that LeakSanitizer counted
// nothing, so a silent zero-fill cannot pass. Every row LOOPS 200 times
// inside the program, so the leak cannot hide under the sanitizer's noise
// floor.
//
// Every row is RED on the base (r30-c1): the two leak rows report
// "1600 byte(s) leaked in 100 allocation(s)" — 16 B per leaked `Res`, one
// per iteration on the un-discharged path. The control row (the METHOD twin)
// is GREEN on the base and must stay green: the issue names it as already
// correct, and this fix must not change it.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// THE REGRESSION ROW — the issue's own repro, amplified 200x (the issue
// measured 160 B over 20 iterations; 200 iterations make it 1600 B over 100
// allocations, far above the sanitizer's noise floor).
//
// The lambda discards `_ := x;` on the `flag` branch and calls `x.Dispose()`
// on the other. RED on the base: "1600 byte(s) leaked in 100 allocation(s)".
// GREEN with the fix.
BEZEL_TEST(lambda_body_discard_dispose_branch_leak_9016,
           issue_repro_discard_one_branch_dispose_other_200_iterations) {
    const std::string src =
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method F(flag: bool): int "
        "    var f := lambda(flag: bool): int => do "
        "      var x := new Res(1); "
        "      if flag then _ := x; return 1; end; "
        "      x.Dispose(); return 2; "
        "    end; "
        "    return f(flag); "
        "  end F; "
        "  public static method Main(): int "
        "    var i := 0; var s := 0; "
        "    while i < 200 do s := s + C.F(i mod 2 = 0); i := i + 1; end; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;";
    // 100 iterations take the discard branch (returns 1), 100 the Dispose
    // branch (returns 2): 100 * 1 + 100 * 2 = 300.
    expectClean(runAotChecked(src), 0, "300\n");
}

// THE SIBLING ROW — the same defect WITHOUT the discard: a lambda-body local
// disposed on ONE branch only. This is the path-blind `everDischarged_` half
// of the root cause on its own; the METHOD twin of it does not leak.
//
// RED on the base: "1600 byte(s) leaked in 100 allocation(s)". GREEN with the
// fix — and, like the METHOD twin, it now draws the §28.3 must-use warning
// (the value IS implicitly disposed at the end of its scope, on the branch
// that does not dispose it by hand).
BEZEL_TEST(lambda_body_discard_dispose_branch_leak_9016,
           dispose_on_one_branch_only_200_iterations) {
    const std::string src =
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method F(flag: bool): int "
        "    var f := lambda(flag: bool): int => do "
        "      var x := new Res(1); "
        "      if flag then return 1; end; "
        "      x.Dispose(); return 2; "
        "    end; "
        "    return f(flag); "
        "  end F; "
        "  public static method Main(): int "
        "    var i := 0; var s := 0; "
        "    while i < 200 do s := s + C.F(i mod 2 = 0); i := i + 1; end; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;";
    expectClean(runAotChecked(src), 0, "300\n");
}

// THE CONTROL ROW — the METHOD twin of the issue's own repro, byte-identical
// statements in a method body. GREEN on the base and GREEN with the fix: the
// issue names it as already correct, and this fix must not change it. It is
// the one boundary control of this suite: a corpus case cannot express
// "LeakSanitizer counted nothing over 200 iterations".
BEZEL_TEST(lambda_body_discard_dispose_branch_leak_9016,
           method_twin_stays_leak_free_200_iterations) {
    const std::string src =
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method F(flag: bool): int "
        "    var x := new Res(1); "
        "    if flag then _ := x; return 1; end; "
        "    x.Dispose(); return 2; "
        "  end F; "
        "  public static method Main(): int "
        "    var i := 0; var s := 0; "
        "    while i < 200 do s := s + C.F(i mod 2 = 0); i := i + 1; end; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;";
    expectClean(runAotChecked(src), 0, "300\n");
}

}  // namespace
