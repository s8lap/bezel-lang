// Regression test for issue #9270 — an OWNED chain-call result
// (`Make(): () -> owned W`, §28.3.5's announcement table row 1) in an operand
// that runs on only SOME paths of its full expression emitted MALFORMED IR: the
// chain-call arm's `ccres.slot` alloca was created inside the branch block
// (`sc.rhs`, `cond.then`, `cond.else`) while the scope-end disposal it
// registers is emitted at the end of the ENCLOSING scope, which the alloca does
// not dominate.
//
//   Instruction does not dominate all uses!
//     %ccres.slot = alloca ptr, align 8
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// Two positions (the issue's own two programs):
//
//   1. the RIGHT OPERAND of a short-circuit `and` / `or` (§17.2: "`and` and
//      `or` short-circuit: the right operand is not evaluated if the result is
//      determined by the left");
//   2. an ARM of a CONDITIONAL EXPRESSION (§20: only the selected arm is
//      evaluated).
//
// THE SPEC. §28.3.11 *Unbound owned temporaries* (v0.359): an owned temporary
// that is not bound is disposed at the end of the full expression in which it
// is produced; "a temporary that is instead bound (to a `var`/`using var`),
// `move`d, returned, or transferred into an owning location is governed by the
// corresponding transfer or discharge rule and is not disposed under this rule
// (it would otherwise be disposed twice)".
//
//   Position 1's value is an UNBOUND temporary (the `.V` read only borrows it):
//   it is disposed exactly once on each path that EVALUATED the operand, and
//   never on a path that skipped it.
//
//   Position 2's value is TRANSFERRED (into the binding / the `owned` return
//   slot): the destination disposes it exactly once, and the chain-call arm
//   must register no disposal of its own (a hoisted slot alone would leave a
//   DOUBLE dispose, which the dominance ICE had been hiding).
//
// THE ORACLE. `runAotChecked`: AddressSanitizer over the GENERATED module and
// LeakSanitizer at exit. Every program COUNTS its destructor runs, so "exactly
// once" is checked both ways (the leak leg catches an undisposed value, the
// count catches a double dispose). The rows LOOP (200 iterations).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

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
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// The class the chain call mints; it COUNTS its destructor runs.
const char* kCountingClass =
    "public class W "
    "  public static Drops: int := 0; "
    "  public V: int := 3; "
    "  destructor "
    "    W.Drops := W.Drops + 1; "
    "  end destructor; "
    "end W; ";

// A FUNCTION VALUE whose declared return carries `owned`: §28.3.5's table
// (row 1) makes the invocation's result OWNED.
const char* kProducer =
    "public static method Make(): () -> owned W "
    "  return lambda(): W => new W(); "
    "end Make; ";

// `Run` holds the construct so every owner has been disposed by the time
// `Main` reads the destructor count.
std::string program(const std::string& members, const std::string& runBody) {
    return std::string("module M; ") + kCountingClass +
        "public class C "
        "  " + kProducer + members +
        "  public static method Run(): int "
        "    var t := 0; " + runBody +
        "    return t; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := C.Run(); "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(W.Drops); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

}  // namespace

// ROW 1 — the issue's POSITION 1 repro (`and`), looped 200 times. The right
// operand runs on the 100 even iterations: 100 values, each disposed exactly
// once; the 100 odd iterations skip it and dispose nothing.
//
// RED on the base: the ICE. RED under a wrong fix that hoists the slot but does
// not null-initialise it / reset it: a skipped iteration would dispose the
// previous iteration's already-freed value (ASan heap-use-after-free, count 200).
BEZEL_TEST(owned_chain_call_branch_operand_9270, and_right_operand_disposed_once_per_evaluation) {
    JitResult r = runAotChecked(program("",
        "for var j in 0..<200 do "
        "  var b := j mod 2 = 0 and C.Make()().V = 3; "
        "  t := t + 3; "
        "end; "));
    expectClean(r, 0, "600\n100\n");
}

// ROW 2 — the `or` twin: the right operand runs when the left is false, on the
// 100 odd iterations.
BEZEL_TEST(owned_chain_call_branch_operand_9270, or_right_operand_disposed_once_per_evaluation) {
    JitResult r = runAotChecked(program("",
        "for var j in 0..<200 do "
        "  var b := j mod 2 = 0 or C.Make()().V = 3; "
        "  t := t + 3; "
        "end; "));
    expectClean(r, 0, "600\n100\n");
}

// ROW 3 — a NESTED short-circuit: the chain call sits in the right operand of an
// `or` that is itself the right operand of an `and`. It runs only when the outer
// left (`j < 100`) holds and the inner left (`j mod 2 = 0`) is false: the odd j
// below 100, 50 values.
BEZEL_TEST(owned_chain_call_branch_operand_9270, nested_short_circuit_operand_disposed_once_per_evaluation) {
    JitResult r = runAotChecked(program("",
        "for var j in 0..<200 do "
        "  var b := j < 100 and (j mod 2 = 0 or C.Make()().V = 3); "
        "  t := t + 3; "
        "end; "));
    expectClean(r, 0, "600\n50\n");
}

// ROW 4 — the issue's POSITION 2 repro: a conditional-expression arm RETURNED.
// The THEN arm is selected (`Get(true)`) on all 200 calls: the chain call's value
// is transferred into the `owned` return slot and into the caller's `var`, and is
// disposed once by that binding.
//
// RED on the base: the ICE. RED under a wrong fix that hoists the slot without
// handing the value off: each value is disposed twice (count 400).
BEZEL_TEST(owned_chain_call_branch_operand_9270, then_arm_returned_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Get(b: bool): owned W "
        "  return (if b then C.Make()() else new W()); "
        "end Get; ",
        "for var j in 0..<200 do "
        "  var s := C.Get(true); "
        "  t := t + s.V; "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 5 — the ELSE arm of the same conditional: the chain call is in the arm
// that is NOT selected on the `then` calls, so it runs on exactly 100 of the 200
// calls.
BEZEL_TEST(owned_chain_call_branch_operand_9270, else_arm_returned_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Get(b: bool): owned W "
        "  return (if b then new W() else C.Make()()); "
        "end Get; ",
        "for var j in 0..<200 do "
        "  var s := C.Get(j mod 2 = 0); "
        "  t := t + s.V; "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 6 — conditional-expression arms that are chain calls, BOUND by a `var`
// initializer. The conditional's arm keeps the chain-call arm's §28.3.11
// registration (the ownership pass does not drop a local bound to a
// conditional initializer), so each value is disposed exactly once, by the
// entry-slot disposal. (A MIXED `C.Make()()` / `new W()` conditional bound to a
// local leaks the `new W()` arm: an ownership-pass defect, filed separately.)
BEZEL_TEST(owned_chain_call_branch_operand_9270, arm_bound_to_local_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program("",
        "for var j in 0..<200 do "
        "  var s := (if j mod 2 = 0 then C.Make()() else C.Make()()); "
        "  t := t + s.V; "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 7 — a conditional-expression arm that is an UNBOUND temporary (only its
// `.V` is read): disposed at the end of the full expression on each path that
// evaluated it. Both arms are chain calls, so every iteration evaluates exactly
// one: 200 values, each disposed once.
BEZEL_TEST(owned_chain_call_branch_operand_9270, unbound_arm_temporary_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program("",
        "for var j in 0..<200 do "
        "  var n := (if j mod 2 = 0 then C.Make()() else C.Make()()).V; "
        "  t := t + n; "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}
