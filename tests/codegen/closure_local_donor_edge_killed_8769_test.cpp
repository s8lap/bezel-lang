// Codegen — issue #8769: a closure local assigned into another closure local
// (`b := g;`) that is reassigned again before its value reaches a store
// that keeps the pointer (`b := H; fs.Add(b);`) was treated as the
// stored value's donor, so `g`'s closure was never released: one leaked
// closure (and its capture cells) per call.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): a function value's
// lifetime is managed by the runtime without user involvement; §27.5
// (v0.359): a closure's captures live as long as the closure, and no longer
// is required, but a value nothing can reach any more that is never freed
// is a leak the checked-AOT oracle reports.
//
// ROOT CAUSE. The #8723 donor pass in `collectClosureReleaseEligible`
// (3dd878b93) is flow-insensitive: the edge `g -> b` made `g` a donor
// whenever `b` was kept anywhere, even when the definition `b := g;` is
// overwritten before every kept site; and a target that leaves the frame
// moved `g` out of closureReleaseEligible_, so `g`'s capture cells were not
// freed either. INTRODUCED by 3dd878b93: origin/main @ b279e8daf has no
// donor pass and is leak-free.
//
// THE FIX. The edge is per definition: `b := g;` donates `g` only if that
// definition of `b` reaches a kept site (the reaching-definition analysis
// of the #8753 fix), and a definition no read of `b` can see carries
// nothing out of the frame. `b := b;` passes the reaching definitions on.
//
// The issue's repro takes the other value from `hs[0]`, an array Main
// fills with `hs.Add(h)`, whose own #5866 leak (64 B / 1) would trip the
// strict oracle; these rows read a `static readonly` field instead, which
// stays reachable. Every program loops 200 times under generated-code
// AddressSanitizer and LeakSanitizer (`runAotChecked`, strict: any leak
// fails the row); the defect leaks 3 allocations per call.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1; the
// issue's literal `hs[0]` repro: 15264 B / 601 on r30-c1, 64 B / 1 on
// b279e8daf and with the fix):
//   row                                          r30-c1        b279e8daf   fix
//   issue_repro_assign_then_kill_then_add        15200 B/600   clean       clean
//   control_self_assign_keeps_assigned_value     40200, leak   UAF         40200, leak
//   control_kill_on_one_branch_only              20800, leak   UAF         20800, leak
// (the other rows: same as the repro row; see the commit body.)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// `fillBody` follows the declarations of `g` (`x - y + k`, capturing `k`)
// and `b`; it must leave `H` (`x * y`) in `fs[0]`.
std::string fillProgram(const std::string& fillBody) {
    return R"BZL(
module M;
type Fn := (int, int) -> int;
public class C
  private static readonly H: Fn := lambda(x: int, y: int) => x * y;
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var b: Fn := lambda(x: int, y: int) => 0;
)BZL" + fillBody + R"BZL(
  end Fill;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      s := s + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(s);
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
}

// The strict oracle: the program ran, printed `out`, and neither
// AddressSanitizer nor LeakSanitizer reported anything.
void expectNoLeak(const std::string& fillBody, const std::string& out) {
    auto r = runAotChecked(fillProgram(fillBody));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    if (bezel::test::aot_detail::parseLeakSummary(text, leakedBytes,
                                                  leakedAllocs))
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    BEZEL_EXPECT_EQ(leakedAllocs, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// Lenient (LeakSanitizer off): stdout when no memory error fired.
std::string runNoMemoryErrorOut(const std::string& fillBody) {
    auto r = runAotCheckedLenient(fillProgram(fillBody));
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// Per call: H(1, 2) + H(4, 1) = 2 + 4.
const std::string kOut1200 = "1200\nDONE\n";

}  // namespace

// #8769's repro, with `H` for `hs[0]` (see the header).
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           issue_repro_assign_then_kill_then_add_spec_28_9) {
    expectNoLeak(
        "    b := g;\n"
        "    b := H;\n"
        "    fs.Add(b);\n",
        kOut1200);
}

// Branch: the definition `b := g;` is killed on the path that makes it, the
// other path never makes it.
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           kill_on_both_branches_then_add_spec_28_9) {
    expectNoLeak(
        "    if k mod 2 = 0 then\n"
        "      b := g;\n"
        "      b := H;\n"
        "    else\n"
        "      b := H;\n"
        "    end;\n"
        "    fs.Add(b);\n",
        kOut1200);
}

// Loop-carried: the definition reaches the next iteration's header but is
// killed before the loop exits.
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           loop_assign_then_kill_then_add_spec_28_9) {
    expectNoLeak(
        "    for var j in 0..<2 do\n"
        "      b := g;\n"
        "      b := H;\n"
        "    end for;\n"
        "    fs.Add(b);\n",
        kOut1200);
}

// Reassign-to-self passes the definition on; the next assignment kills it.
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           assign_then_self_assign_then_kill_then_add_spec_28_9) {
    expectNoLeak(
        "    b := g;\n"
        "    b := b;\n"
        "    b := H;\n"
        "    fs.Add(b);\n",
        kOut1200);
}

// The kept site precedes the definition: `fs` keeps `H`, and `b := g;`
// is never read.
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           add_then_assign_from_local_spec_28_9) {
    expectNoLeak(
        "    b := H;\n"
        "    fs.Add(b);\n"
        "    b := g;\n",
        kOut1200);
}

// CONTROL (a corpus case cannot run AddressSanitizer): `b := b;` must pass
// `b := g;`'s definition on to the store, so `g` still donates. RED (heap-
// use-after-free) under the plausible wrong fix "every reassignment kills
// the reaching definitions". Per call (g at k = i): 2 + 2i, summed 40200.
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           control_self_assign_keeps_assigned_value_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(
        "    b := H;\n"
        "    b := g;\n"
        "    b := b;\n"
        "    fs.Add(b);\n"),
        std::string("40200\nDONE\n"));
}

// CONTROL: a kill on ONE path leaves `b := g;` reaching the store on the
// other, so `g` still donates. RED (heap-use-after-free) under the
// plausible wrong fix "a definition killed on some path is dead".
// Even i: 6; odd i: 2 + 2i. Sum: 600 + 20200 = 20800.
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           control_kill_on_one_branch_only_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(
        "    b := g;\n"
        "    if k mod 2 = 0 then\n"
        "      b := H;\n"
        "    end;\n"
        "    fs.Add(b);\n"),
        std::string("20800\nDONE\n"));
}

// FIX-FORWARD (review r42-rev-s48, probe h03): a catch `when` guard is
// walked under its TryStmt, but it runs at the catch entry, after the
// protected body. Kept at the try's ENTRY, the guard missed the definition
// `b := g;` made inside the body, so the edge g -> b was dropped and `g` was
// released at scope end while `d` (and then `fs`) still held it: a heap-use-
// after-free under AddressSanitizer. Per call (g at k = i): 2 + 2i, summed
// 40200.
BEZEL_TEST(closure_local_donor_edge_killed_8769,
           catch_guard_keeps_definition_made_in_try_body_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(
        "    var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();\n"
        "    b := H;\n"
        "    try\n"
        "      b := g;\n"
        "      if k >= 0 then\n"
        "        throw new InvalidOperationException(\"x\");\n"
        "      end;\n"
        "    catch\n"
        "      var e: InvalidOperationException when d.TryAdd(1, b) => do var z := 0; end\n"
        "    end try;\n"
        "    b := H;\n"
        "    fs.Add(d[1]);\n"),
        std::string("40200\nDONE\n"));
}
