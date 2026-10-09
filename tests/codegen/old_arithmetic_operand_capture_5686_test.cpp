// Spec-derived runtime tests for §22.16 `old(E)` operand capture with an
// arithmetic operand E (issue #5686, v0.330).
//
// THE DEFECT (#5686). `captureOldOperands` evaluated each `old(E)` operand
// with `SetInsertPoint(entryBB)` and then RESTORED the caller's insert
// block. That is unsound the moment the operand needs CODE rather than a
// bare load: §22.16 admits an ARBITRARY pure expression as E, so `old(a / b)`
// routes through `lowerBinary`'s CHECKED path (§6.1), whose `emitGuardThrow`
// creates `chk.err`/`chk.cont`, branches from the entry block, and leaves the
// insert point in an UNTERMINATED `chk.cont`. The capture store then landed in
// `chk.cont` with no terminator and the restore returned to an already-
// terminated entry block:
//
//   Basic Block in function 'bz$M.C$Div' does not have terminator!
//   label %chk.cont2
//   bezelc: codegen error: IR verification failed
//
// Two earlier attempts were rejected for the same defect. One enumerated only
// `+ - *`; the next covered all eight operators but emitted UNCHECKED IR
// (`CreateSDiv`, `CreateSRem`, `CreateShl`, `CreateAShr`, `CreateNeg`) on the
// argument that "overflow is impossible at method entry since operands are
// just-past-entry parameters". That argument is false — its own test case is
// `old(a / b)` with `a, b` PARAMETERS, and a parameter `b` can be 0. This
// suite therefore runs every shape to completion.
//
// WHAT THE SPEC REQUIRES.
//   * §22.16: "`old(E)` … the compiler introduces a hidden entry binding
//     equivalent to `var __oldN := E;` … evaluated exactly once per
//     invocation". E is an ordinary pure expression, so it carries the
//     ordinary arithmetic semantics — nothing about being at entry changes
//     them.
//   * §6.1: "An operation whose mathematical result is not representable in
//     the result type raises OverflowException"; "Division and `mod` raise
//     DivideByZeroException when the right operand is zero."
//   * §6.5: an out-of-domain shift count raises OverflowException.
//   * §22.16 "Faults during clause evaluation": "A clause expression may
//     fault … The fault propagates from the clause's evaluation point AS
//     ITSELF — never converted to ContractViolationException."
//   * Appendix C.15: integer arithmetic overflow and division by zero are
//     compiler-enforced invariants that survive into codegen.
//
// WHY A RUNNING TEST. The rejected attempts' only case was compile-only
// (`%bezelc -c`), so it could not observe a fault that only occurs when the
// program RUNS with `b = 0` or `a = maxInt`. Every test here drives the AOT
// path and asserts on the recovered exception value, which is what actually
// distinguishes checked arithmetic from an unchecked wrap.
//
// A no-op case is not enough either: with `b = 0` the `ensures` clause would
// be FALSE and raise ContractViolationException regardless of how `a / b`
// was evaluated. Each fault test therefore catches the SPECIFIC exception
// type — DivideByZeroException or OverflowException — so a build that
// silently wrapped and then reported a ContractViolationException would fail
// the assertion rather than pass it.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

int runFull(const std::string& program) {
    auto r = bezel::test::runAot(program);
    return r.ran ? r.exitCode : -999;
}

// `ensures(old(a <op> b) = 1)` with the operand evaluated at entry, and a
// Main that calls `c.M(x, y)` inside a try that maps the SPECIFIED fault to
// `sink`, and any other outcome to a distinct sentinel. Returns the sink so
// the caller can assert which exception actually fired.
//
//   sink = 40  DivideByZeroException  (§6.1 zero divisor)
//   sink = 50  OverflowException      (§6.1 overflow / §6.5 shift domain)
//   sink = 70  ContractViolationException (the clause evaluated false — the
//              operand did NOT fault, so the arithmetic was unchecked/wrapped)
//   sink = 80  anything else (including an uncaught fault → nonzero exit)
std::string divZeroProgram() {
    return
        "module M; class C "
        "  public method M(a: int, b: int): int "
        "    ensures(old(a / b) = 7) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(42, 0); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

std::string modZeroProgram() {
    return
        "module M; class C "
        "  public method M(a: int, b: int): int "
        "    ensures(old(a mod b) = 7) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(42, 0); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

// int.MaxValue * 2 — §6.1's representability rule. The operand is a pair of
// PARAMETERS, which is precisely the "impossible at entry" argument the
// rejected attempt rested on: `a` arrives as maxInt from the caller.
std::string mulOverflowProgram() {
    return
        "module M; class C "
        "  public method M(a: int, b: int): int "
        "    ensures(old(a * b) = 7) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(2147483647, 2); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

// int.MinValue unary negation — §6.1's unary case. `-int.MinValue` is not
// representable, so this is the one shape that faults on a UNARY operator.
std::string negOverflowProgram() {
    return
        "module M; class C "
        "  public method M(a: int): int "
        "    ensures(old(-a) = 7) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(-2147483647 - 1); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

// A shift count of 32 over an `int` (width 32) is out of the §6.5 domain
// `0 .. 31`, so it raises OverflowException rather than being reduced modulo
// the width. The count arrives as a PARAMETER.
std::string shiftDomainProgram() {
    return
        "module M; class C "
        "  public method M(a: int, b: int): int "
        "    ensures(old(a << b) = 7) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(1, 32); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

// int.MinValue / -1 — §6.1's signed-division overflow. Distinct from the
// zero-divisor case: the divisor is non-zero, so only the representability
// rule can produce the fault.
std::string divOverflowProgram() {
    return
        "module M; class C "
        "  public method M(a: int, b: int): int "
        "    ensures(old(a / b) = 7) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(-2147483647 - 1, -1); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

// §22.16 admits an arbitrary pure expression as the operand — NOT only
// parameters. This one's E reads a FIELD (`this.k`) and a STATIC, then
// divides, so the "operands are just-past-entry parameters" argument does not
// reach it. The field is a copyable value-typed projection, the form §22.16
// names as its recommended idiom.
std::string nonParameterOperandProgram() {
    return
        "module M; class C "
        "  public k: int := 100; "
        "  public method M(d: int): int "
        "    ensures(old(this.k / d) = 7) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(0); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

// A NESTED capture: two `old(...)` operands in one clause, the second an
// arithmetic expression over the first's shape. Exercises the store-into-
// `chk.cont` path twice in one walk, which is where a per-operand restore
// would resume in a block the previous operand already terminated.
std::string nestedOperandProgram() {
    return
        "module M; class C "
        "  public method M(a: int, b: int): int "
        "    ensures(old(a + b) = 1 and old(a / b) = 2) "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    try r := c.M(10, 0); "
        "    catch DivideByZeroException => r := 40; "
        "    catch OverflowException => r := 50; "
        "    catch ContractViolationException => r := 70; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
}

}  // namespace

// =========================================================================
// #5686 — the faulting shapes, RUNNING. Each asserts the SPECIFIED exception
// type (40 = DivideByZeroException, 50 = OverflowException), never merely
// "something was raised": 70 is reserved for the silent-wrap outcome, where
// the unchecked IR produced a value, the clause evaluated false, and the
// failure was misreported as a postcondition violation.
// =========================================================================

// §6.1: division by zero raises DivideByZeroException.
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_div_by_zero_raises_divide_by_zero_spec_6_1) {
    BEZEL_EXPECT_EQ(runFull(divZeroProgram()), 40);
}

// §6.1: `mod` by zero raises DivideByZeroException too.
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_mod_by_zero_raises_divide_by_zero_spec_6_1) {
    BEZEL_EXPECT_EQ(runFull(modZeroProgram()), 40);
}

// §6.1: int.MaxValue * 2 is not representable → OverflowException.
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_mul_overflow_raises_overflow_spec_6_1) {
    BEZEL_EXPECT_EQ(runFull(mulOverflowProgram()), 50);
}

// §6.1: `-int.MinValue` is not representable → OverflowException on the
// unary operator.
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_unary_negate_min_raises_overflow_spec_6_1) {
    BEZEL_EXPECT_EQ(runFull(negOverflowProgram()), 50);
}

// §6.5: a shift count of 32 over a 32-bit value is out of domain →
// OverflowException (not a silent modulo reduction).
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_shift_count_out_of_domain_raises_overflow_spec_6_5) {
    BEZEL_EXPECT_EQ(runFull(shiftDomainProgram()), 50);
}

// §6.1: int.MinValue / -1 overflows signed division → OverflowException.
// LLVM `sdiv` on that pair is undefined behaviour and x86 IDIV raises
// SIGFPE, so an unchecked build crashes rather than faulting cleanly.
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_div_min_by_neg_one_raises_overflow_spec_6_1) {
    BEZEL_EXPECT_EQ(runFull(divOverflowProgram()), 50);
}

// =========================================================================
// §22.16: E is an arbitrary pure expression, not only parameters. This one
// reads a field and a static before dividing — outside the reach of the
// "impossible at method entry" argument the rejected fix rested on.
// =========================================================================
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_non_parameter_operand_zero_divisor_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(nonParameterOperandProgram()), 40);
}

// =========================================================================
// Two captures in one clause: the second operand's guard splits the block the
// first operand's store already landed in.
// =========================================================================
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_two_arithmetic_captures_in_one_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(nestedOperandProgram()), 40);
}

// =========================================================================
// The NON-faulting arithmetic shapes must still be ACCEPTED and compile to
// well-formed IR — the malformed-IR defect #5686 reports was a codegen
// rejection, so a compile-clean assertion is the direct regression pin for
// it. `old(a + b)` is the exact shape the rejected compile-only case used,
// which could not observe anything at run time; here it is both compiled and
// run, and the satisfied clause must leave the return value intact.
// =========================================================================
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_add_satisfied_clause_returns_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method M(a: int, b: int): int "
        "    ensures(old(a + b) = 42) "
        "    return 5; "
        "  end M; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; r := c.M(20, 22); return r; "
        "  end Main; "
        "end C;"), 5);
}

// The same for the subtraction and multiplication shapes, so a regression
// that broke only one operator cannot hide behind the two above.
BEZEL_TEST(old_arithmetic_operand_capture_5686,
           old_sub_and_mul_satisfied_clauses_return_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public method S(a: int, b: int): int "
        "    ensures(old(a - b) = 10) return 6; end S; "
        "  public method P(a: int, b: int): int "
        "    ensures(old(a * b) = 42) return 7; end P; "
        "  public method D(a: int, b: int): int "
        "    ensures(old(a / b) = 10) return 8; end D; "
        "  public method Q(a: int, b: int): int "
        "    ensures(old(a mod b) = 1) return 9; end Q; "
        "  public method L(a: int, b: int): int "
        "    ensures(old(a << b) = 40) return 3; end L; "
        "  public method R(a: int, b: int): int "
        "    ensures(old(a >> b) = 5) return 4; end R; "
        "  public method N(a: int): int "
        "    ensures(old(-a) = -5) return 2; end N; "
        "  public static method Main(): int "
        "    var c := new C(); var r: int := 0; "
        "    r := c.S(30, 20); r := r + c.P(6, 7); r := r + c.D(30, 3); "
        "    r := r + c.Q(31, 10); r := r + c.L(5, 3); r := r + c.R(40, 3); "
        "    r := r + c.N(5); "
        "    return r; "
        "  end Main; "
        "end C;"), 6 + 7 + 8 + 9 + 3 + 4 + 2);
}