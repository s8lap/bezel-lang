// Regression test for issue #9340 — an OWNED chain-call result
// (`Make(): () -> owned W`, §28.3.5's announcement table row 1) in the RIGHT
// OPERAND of `??` or in an arm / guard of an EXPRESSION-position `case` emitted
// MALFORMED IR: the chain-call arm's `ccres.slot` alloca was created inside the
// branch block (`coalesce.rhs`, `case.arm`) while the scope-end disposal it
// registers is emitted at the end of the ENCLOSING scope, which the alloca does
// not dominate ("Instruction does not dominate all uses!", IR verification
// failed). The same mechanism as #9177 / #9270 in two more positions.
//
// THE SPEC. §7.7: the right operand of `??` is "evaluated only when the left is
// null"; §19.2: only the selected arm of a `case` expression is evaluated;
// §28.3.11 *Unbound owned temporaries* (v0.359): an owned temporary that is not
// bound is disposed at the end of the full expression in which it is produced,
// and a transferred one (returned, bound, stored into an owning location) "is
// governed by the corresponding transfer or discharge rule and is not disposed
// under this rule (it would otherwise be disposed twice)"; §28.3's `a ?? b` and
// expression-`case` producer rows: "exactly one of the owned values is produced
// at runtime and is discharged once".
//
//   * an UNBOUND operand/arm takes an entry-block null-initialised slot
//     (issue #9270's discipline): disposed once where it ran, never where it did
//     not;
//   * a TRANSFERRED operand/arm (returned, stored into an `owned` field) is
//     handed off, so the chain-call arm registers no disposal of its own.
//
// THE ORACLE. `runAotChecked`: AddressSanitizer over the GENERATED module and
// LeakSanitizer at exit; every program COUNTS its destructor runs, so "exactly
// once" is checked both ways. Rows LOOP (200 iterations).

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
std::string program(const std::string& members, const std::string& runBody,
                    const std::string& extraClasses = "") {
    return std::string("module M; ") + kCountingClass + extraClasses +
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

// The `W?`-returning helpers the `??` rows use: `MaybeNull(true)` is null,
// `MaybeNull(false)` is a fresh `W`. `H` holds an `owned` field.
const char* kCoalesceMembers =
    "public static method MaybeNull(b: bool): owned W? "
    "  if b then return null; end; "
    "  return new W(); "
    "end MaybeNull; "
    "public static method Peek(w: W): int "
    "  return w.V; "
    "end Peek; ";

const char* kHolder =
    "public class H "
    "  public owned f: W? := null; "
    "  public constructor() end constructor; "
    "end H; ";

}  // namespace

// ROW 1 — the issue's `??` repro: the right operand is an UNBOUND temporary
// (`.V` only borrows it). The left is null on all 200 iterations, so the right
// operand runs 200 times: 200 values, 200 runs. RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, coalesce_right_operand_unbound_is_disposed_once_per_evaluation) {
    JitResult r = runAotChecked(program(kCoalesceMembers,
        "for var j in 0..<200 do "
        "  t := t + (C.MaybeNull(true) ?? C.Make()()).V; "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 2 — the right operand is NEVER evaluated (the left is non-null on all 200
// iterations) and the `??` is RETURNED, so the left's value is the one the
// caller owns: 200 values, 200 runs, none from `Make`. RED on the base: the ICE
// (the module does not compile even if the operand never runs). RED under a
// wrong fix that leaves the entry slot uninitialised: the disposal would read
// garbage on a path that skipped the operand.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, coalesce_right_operand_never_evaluated_disposes_nothing_extra) {
    JitResult r = runAotChecked(program(
        std::string(kCoalesceMembers) +
        "public static method Get(b: bool): owned W "
        "  return C.MaybeNull(b) ?? C.Make()(); "
        "end Get; "
        "public static method Go(b: bool): int "
        "  using var s := C.Get(b); "
        "  return s.V; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(false); "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 3 — the `??` RETURNED (a hand-off position): the right operand's value is
// transferred into the `owned` return slot and the caller's `using var`, so the
// chain-call arm registers no disposal of its own. Even iterations take the
// chain call (100), odd iterations take the left's own value (100): 200 values,
// 200 runs. RED on the base: the ICE. RED under a wrong fix that hoists the slot
// without handing the operand off: the chain-call values are disposed twice.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, coalesce_right_operand_returned_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        std::string(kCoalesceMembers) +
        "public static method Get(b: bool): owned W "
        "  return C.MaybeNull(b) ?? C.Make()(); "
        "end Get; "
        "public static method Go(b: bool): int "
        "  using var s := C.Get(b); "
        "  return s.V; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(j mod 2 = 0); "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 4 — the `??` right operand stored into an `owned` FIELD: the field owns
// the value (drop-on-overwrite, disposed with its holder), so the chain-call arm
// registers nothing of its own. 200 values, 200 runs.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, coalesce_right_operand_stored_into_owned_field_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        std::string(kCoalesceMembers) +
        "public static method Go(b: bool): int "
        "  var h := new H(); "
        "  h.f := C.MaybeNull(b) ?? C.Make()(); "
        "  return h.f!.V; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(j mod 2 = 0); "
        "end; ",
        kHolder));
    expectClean(r, 0, "600\n200\n");
}

// ROW 5 — a `??` over a nullable PRIMITIVE left (the `coal.rhs` arm of
// `lowerCoalesce`): the right operand `C.Peek(C.Make()())` runs when the left is
// null (even iterations, 100): t = 100*3 (Peek) + 100*4 (the left's value);
// 100 values, 100 runs. RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, coalesce_nullable_primitive_left_right_operand_is_disposed_once_per_evaluation) {
    JitResult r = runAotChecked(program(
        std::string(kCoalesceMembers) +
        "public static method NI(b: bool): int? "
        "  if b then return null; end; "
        "  return 4; "
        "end NI; "
        "public static method Go(b: bool): int "
        "  return C.NI(b) ?? C.Peek(C.Make()()); "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(j mod 2 = 0); "
        "end; "));
    expectClean(r, 0, "700\n100\n");
}

// ROW 6 — the issue's expression-`case` repro: an arm that is an UNBOUND
// temporary. The chain-call arm is selected when `j mod 3 = 0` (67 of 200
// iterations): 67 values, 67 runs; t = 67*3 + 133*1. RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, case_expression_arm_unbound_is_disposed_once_per_evaluation) {
    JitResult r = runAotChecked(program("",
        "for var j in 0..<200 do "
        "  var n := (case j mod 3 of 0 => C.Make()().V, _ => 1); "
        "  t := t + n; "
        "end; "));
    expectClean(r, 0, "334\n67\n");
}

// ROW 7 — the issue's RETURNED `case` arm (a hand-off position): the chain-call
// arm (k = 0, 100 iterations) and the `new W()` arm (k = 1, 100 iterations)
// each produce a value transferred to the caller's `using var`: 200 values, 200
// runs. RED on the base: the ICE. RED under a wrong fix that hoists the slot
// without handing the arm off: the chain-call values are disposed twice.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, case_expression_arm_returned_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Get(k: int): owned W "
        "  return (case k of 0 => C.Make()(), _ => new W()); "
        "end Get; "
        "public static method Go(k: int): int "
        "  using var s := C.Get(k); "
        "  return s.V; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(j mod 2); "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 8 — `case` arms that are chain calls, BOUND by a `var` initializer. As the
// conditional-expression twin (#9270), the arms keep their own §28.3.11
// registration (the `var` site does not hand arms off), so each value is
// disposed exactly once. 200 values, 200 runs.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, case_expression_arms_bound_to_local_are_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Go(k: int): int "
        "  var s := (case k of 0 => C.Make()(), _ => C.Make()()); "
        "  return s.V; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(j mod 2); "
        "end; "));
    expectClean(r, 0, "600\n200\n");
}

// ROW 9 — `case` arms stored into an `owned` field: the field owns the value;
// the arm registers nothing of its own. 200 values, 200 runs.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, case_expression_arm_stored_into_owned_field_is_disposed_exactly_once) {
    JitResult r = runAotChecked(program(
        "public static method Go(k: int): int "
        "  var h := new H(); "
        "  h.f := (case k of 0 => C.Make()(), _ => C.Make()()); "
        "  return h.f!.V; "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(j mod 2); "
        "end; ",
        kHolder));
    expectClean(r, 0, "600\n200\n");
}

// ROW 10 — a chain call in a `case` arm GUARD: the guard runs only when the
// arm's pattern matched (k = 0, 100 iterations): 100 values, 100 runs;
// t = 100*10 + 100*1. RED on the base: the ICE.
BEZEL_TEST(owned_chain_call_coalesce_case_9340, case_expression_arm_guard_is_disposed_once_per_evaluation) {
    JitResult r = runAotChecked(program(
        "public static method Go(k: int): int "
        "  return (case k of 0 when C.Make()().V = 3 => 10, _ => 1); "
        "end Go; ",
        "for var j in 0..<200 do "
        "  t := t + C.Go(j mod 2); "
        "end; "));
    expectClean(r, 0, "1100\n100\n");
}
