// fix #8261 + #8338 (stdlib §5.1, spec §11.8 / §14.2 / §22.6, v0.355) — the
// `Sum` arm of `Emitter::tryLowerCollectionInstanceCall`
// (`src/codegen/LlvmBackend_Calls.cpp`).
//
// TWO defects, ONE arm.
//
// #8261 — THE OPERATOR IS SELECTED BY DECLARATION ORDER. stdlib §5.1 declares
//   `extension IEnumerable<T> where T: INumber<T> { public method Sum(): owned
//   T; }` and spec §11.8 models it as `var total := T.Zero(); total := total +
//   el` — so the fold calls the CONFORMER's own `operator +`, and BOTH
//   operands are the element type. The codegen scan took the LAST declared
//   `operator +` of arity two, whatever its parameter types: a record
//   declaring `operator +(a: Money, b: Money)` and then
//   `operator +(a: Money, b: int)` folded through the HETEROGENEOUS one and
//   passed a `%record.Money` where an `i32` was expected
//   (`@llvm.sadd.with.overflow.i32(i32, %record.Money)`) — "Call parameter
//   type does not match function signature!" -> "IR verification failed".
//   The answer therefore DEPENDED ON THE DECLARATION ORDER, which spec §22.6
//   forbids: operator resolution is "exact beats convert", INDEPENDENT of
//   declaration order. The fix selects the operator BY OPERAND TYPE, grading
//   EACH POSITION the way sema's own `ExprTC::findBinaryOperator` does.
//
//   THE ROWS THAT MATTER MOST are the IFACE-FORMAL ones (the first three
//   below): a record that declares BOTH `operator +(a: Money, b: Money)` and
//   `operator +(a: Money, b: IBase)` (IBase an interface the record
//   implements). BOTH overloads are APPLICABLE to the fold's `Money + Money`
//   — the second one by an ordinary §5.1 reference conversion — so the
//   selection must be decided by §22.6's "exact beats convert", not by the
//   declaration order. The exact overload must win IN BOTH DECLARATION
//   ORDERS. A selection that merely moved the tie-break from "last" to
//   "first" would print 777 for the first order — a silently wrong number
//   where the base compiler printed the right one.
//
// #8338 — THE SELECTOR'S RETURN TYPE IS READ UNRESOLVED. Inside a generic
//   body the selector's return type is still the PLACEHOLDER type parameter
//   (a `Kind::Named` type parameter, never a `Kind::Primitive`), so the
//   accumulator's `Kind::Primitive` test never matched, the accumulator fell
//   back to `int`, and the emitted IR mixed an i32 accumulator with the
//   INSTANTIATED use site (`%sum.r = load i32` fed to
//   `bz_double_to_string_r(double)`) — the same IR-verification abort. The
//   fix reads the selector's return type through the generic substitution
//   (`resolveTypeParamRef`), exactly as the no-selector branch reads the
//   ELEMENT type.
//
//   The long/ulong selector rows are the SILENT half of #8338: the same
//   unresolved type parameter made the accumulator an i32, so a `long`/`ulong`
//   selector's total was truncated to 32 bits (9_000_000_000 printed as
//   410_065_408) instead of aborting. The one-line resolution closes both
//   faces of the same defect.
//
// CONTROLS (must not move): the record whose `operator +` is `Money + Money`
// only; the heterogeneous record with the heterogeneous overload declared
// FIRST (it compiled before the fix, and must keep compiling); the CONCRETE
// receiver's selector `Sum` (outside any generic body); the primitive
// no-selector folds; and the SEMANTIC GROUND TRUTH (`x + y` written out in
// the program, resolved by sema) — the fold MUST agree with it.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The conformer's OTHER `INumber<T>` members (stdlib §2.19). They are the
// same in every row, so only the `+` overloads vary.
const char* kRest =
    "  public static method One(): Money => new Money(1, \"o\");\n"
    "  public static method operator -(a: Money, b: Money): Money =>"
    " new Money(a.Cents - b.Cents, a.Label);\n"
    "  public static method operator *(a: Money, b: Money): Money =>"
    " new Money(a.Cents * b.Cents, a.Label);\n"
    "  public static method operator /(a: Money, b: Money): Money =>"
    " new Money(a.Cents / b.Cents, a.Label);\n"
    "  public static method operator <(a: Money, b: Money): bool =>"
    " a.Cents < b.Cents;\n"
    "  public static method operator >(a: Money, b: Money): bool =>"
    " a.Cents > b.Cents;\n"
    "  public static method operator <=(a: Money, b: Money): bool =>"
    " a.Cents <= b.Cents;\n"
    "  public static method operator >=(a: Money, b: Money): bool =>"
    " a.Cents >= b.Cents;\n";

// `plus` is the `operator +` OVERLOAD DECLARATIONS, in DECLARATION ORDER.
// The record's `+` keeps the LEFT operand's Label (it does NOT concatenate):
// the seeded fold therefore keeps the seed's "z", and the CENTS field (5 + 6
// = 11) is what pins that the fold really ran through the conformer's `+`.
// The non-allocating `+` is deliberate: the CONCATENATING form
// (`a.Label + b.Label`) allocates a fresh string on every step and the
// displaced accumulator's Label then leaks — MEASURED at BASE on the control
// probe p13_heterogeneous_plus_before.bzl (the same 71-byte LeakSanitizer
// report, with the base compiler), a PRE-EXISTING defect of the fold, not
// this lane's.
std::string moneyRecord(const std::string& plus) {
    return std::string(
               "public record Money(Cents: int, Label: string):"
               " INumber<Money>, IBase\n"
               "  public static method Zero(): Money => new Money(0, \"z\");\n") +
           plus + kRest +
           "end Money;\n";
}

// The `IBase` interface the `Money` record implements. An overload whose
// SECOND formal is `IBase` is APPLICABLE to the fold's `Money + Money`
// operands by an ordinary §5.1 reference conversion — the "convert" tier of
// §22.6 — so it competes with the exact overload.
const char* kIBase =
    "public interface IBase\n"
    "end IBase;\n";

// The homogeneous `+` — the one the fold must select for a `Money + Money`
// accumulation.
const char* kPlusMoneyMoney =
    "  public static method operator +(a: Money, b: Money): Money =>"
    " new Money(a.Cents + b.Cents, a.Label);\n";

// The heterogeneous `+` — `Money + int`. It does NOT accept the fold's
// second operand (a `Money`), so the fold must not select it.
const char* kPlusMoneyInt =
    "  public static method operator +(a: Money, b: int): Money =>"
    " new Money(a.Cents + b + 1000, a.Label);\n";

// The other heterogeneous `+` — `int + Money`. It does NOT accept the fold's
// FIRST operand (a `Money`).
const char* kPlusIntMoney =
    "  public static method operator +(a: int, b: Money): Money =>"
    " new Money(a + b.Cents + 2000, b.Label);\n";

// The CONVERT-tier overload — `Money + IBase`. It IS applicable to the
// fold's operands (IBase is an interface `Money` implements), but it needs a
// reference conversion at the second position, so §22.6 ranks it BELOW the
// exact overload. Its body is `777` so that picking it is visible in the
// output.
const char* kPlusMoneyIBase =
    "  public static method operator +(a: Money, b: IBase): Money =>"
    " new Money(777, a.Label);\n";

// The MIRROR of the convert-tier overload — `IBase + Money`: applicable, but
// it converts at the FIRST position. Its body is `888`.
const char* kPlusIBaseMoney =
    "  public static method operator +(a: IBase, b: Money): Money =>"
    " new Money(888, b.Label);\n";

// A generic body whose `Sum()` is the stdlib §5.1 accumulation over the
// conformer's `Zero()` and `+` (spec §11.8's model).
std::string recordProgram(const std::string& plus) {
    return std::string("module M;\n") + kIBase + moneyRecord(plus) +
           "public class Program\n"
           "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
           "  public static method Main()\n"
           "    var a := new Array<Money>();\n"
           "    a.Add(new Money(5, \"a\")); a.Add(new Money(6, \"b\"));\n"
           "    var s := H(a);\n"
           "    Console.WriteLine(s.Cents);\n"
           "    Console.WriteLine(s.Label);\n"
           "  end Main;\n"
           "end Program;\n";
}

// The SEMANTIC GROUND TRUTH — the same record, but the addition is written
// out as an ordinary `x + y` expression, which SEMA resolves (spec §22.6).
// The fold MUST agree with this answer in every declaration order.
std::string groundTruthProgram(const std::string& plus) {
    return std::string("module M;\n") + kIBase + moneyRecord(plus) +
           "public class Program\n"
           "  public static method Main()\n"
           "    var x := new Money(5, \"a\");\n"
           "    var y := new Money(6, \"b\");\n"
           "    var z := x + y;\n"
           "    Console.WriteLine(z.Cents);\n"
           "    Console.WriteLine(z.Label);\n"
           "  end Main;\n"
           "end Program;\n";
}

// A generic body whose `Sum` carries a SELECTOR, and the concrete primitive
// rows of the same family.
std::string selectorProgram(const char* elemType, const char* adds) {
    return std::string("module M;\n"
                       "public class Program\n"
                       "  static method H<T>(a: Array<T>): T where T: INumber<T> =>"
                       " a.Sum(lambda(x) => x);\n"
                       "  public static method Main()\n"
                       "    var a := new Array<") +
           elemType + ">();\n" + adds +
           "    Console.WriteLine(H(a));\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// ===========================================================================
// #8261 — THE DECIDING ROW: the CONVERT-tier overload (`Money + IBase`)
// declared FIRST, the EXACT overload (`Money + Money`) declared SECOND.
//
// Both are APPLICABLE to the fold's `Money + Money` operands. §22.6 ranks the
// exact one higher, so the fold MUST call it — in BOTH declaration orders.
// The base compiler took the LAST declared overload: it happened to be right
// here, and a fix that merely moved the tie-break to "first" would print 777.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           exact_overload_wins_with_convert_formal_declared_first) {
    JitResult r =
        runAotChecked(recordProgram(std::string(kPlusMoneyIBase) +
                                    kPlusMoneyMoney));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8261 — THE MIRROR: the EXACT overload declared FIRST, the CONVERT-tier
// overload (`Money + IBase`) declared LAST.
//
// This is the order the base compiler got WRONG (it took the last declared
// overload and folded through the converting one). §22.6's "exact beats
// convert" must make the answer order-independent.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           exact_overload_wins_with_convert_formal_declared_last) {
    JitResult r =
        runAotChecked(recordProgram(std::string(kPlusMoneyMoney) +
                                    kPlusMoneyIBase));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8261 — THE MIRROR SHAPE: the convert-tier overload converts at the FIRST
// position (`IBase + Money`) and is declared FIRST.
//
// The exact overload must still win: it is better at BOTH positions, and §22.6
// compares the PAIR, not the minimum of the two positions.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           exact_overload_wins_with_mirror_convert_formal) {
    JitResult r =
        runAotChecked(recordProgram(std::string(kPlusIBaseMoney) +
                                    kPlusMoneyMoney));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8261 — THE CONVERT-TIER OVERLOAD IS STILL APPLICABLE, BUT NEVER SELECTED:
// all three overloads at once (`Money + Money`, `Money + IBase`, `IBase +
// Money`). The exact one wins and the others are left alone.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           exact_overload_wins_among_convert_tier_overloads) {
    JitResult r = runAotChecked(recordProgram(std::string(kPlusMoneyIBase) +
                                               kPlusMoneyMoney +
                                               kPlusIBaseMoney));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8261 — THE SEMANTIC GROUND TRUTH: the same records with the addition
// written out as `x + y`, which SEMA resolves by §22.6. Both declaration
// orders must print the same answer, and the fold rows above must agree
// with it (the exact overload is the one sema picks).
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           sema_ground_truth_is_order_independent) {
    JitResult first =
        runAotChecked(groundTruthProgram(std::string(kPlusMoneyIBase) +
                                         kPlusMoneyMoney));
    BEZEL_EXPECT_TRUE(first.ran);
    BEZEL_EXPECT_EQ(first.exitCode, 0);
    BEZEL_EXPECT_EQ(first.stdoutText, "11\na\n");

    JitResult last = runAotChecked(groundTruthProgram(
        std::string(kPlusMoneyMoney) + kPlusMoneyIBase));
    BEZEL_EXPECT_TRUE(last.ran);
    BEZEL_EXPECT_EQ(last.exitCode, 0);
    BEZEL_EXPECT_EQ(last.stdoutText, "11\na\n");
}

// ===========================================================================
// #8261 — THE §22.6 AMBIGUOUS CASE IS NOT SILENTLY PICKED.
//
// A record whose only two `operator +` overloads CONVERT at opposite
// positions (`Money + IBase` and `IBase + Money`) ranks them EQUAL — no
// applicable overload is unambiguously better than the other — so the fold
// has no answer to pick. Such a record cannot conform to `INumber<Money>`
// at all (§11.2: the implementing static must match the interface member
// `operator +(a: T, b: T)` exactly), and the fold's receiver must conform
// (§2.19), so SEMA rejects the program BEFORE codegen — on every compiler.
// The row pins that: the program is REJECTED, and it never prints a number
// (no silently picked 777 / 888, no IR-verification crash).
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           ambiguous_operator_set_is_not_silently_picked) {
    JitResult r =
        runAotChecked(recordProgram(std::string(kPlusMoneyIBase) +
                                    kPlusIBaseMoney));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.empty());
    // The rejection is a clean diagnostic, not the codegen crash the
    // declaration-order bug used to produce.
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("operator +") != std::string::npos ||
            d.message.find("ambiguous") != std::string::npos)
            cited = true;
    BEZEL_EXPECT_TRUE(cited);
}

// ===========================================================================
// #8261 — the issue's exact program: the heterogeneous overload declared
// AFTER the homogeneous one. It ABORTED at compile time ("IR verification
// failed") because the scan took the LAST `operator +`.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           heterogeneous_plus_after_selects_by_operand_type) {
    JitResult r = runAotChecked(recordProgram(
        std::string(kPlusMoneyMoney) + kPlusMoneyInt));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8261 — CONTROL: the heterogeneous overload declared FIRST. This row
// COMPILED and printed "11" / "z" before the fix (the scan happened to pick
// the homogeneous one) and must keep doing so.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           heterogeneous_plus_before_keeps_the_same_answer) {
    JitResult r = runAotChecked(recordProgram(
        std::string(kPlusMoneyInt) + kPlusMoneyMoney));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8261 — THREE `operator +` overloads, the exact one declared FIRST (so the
// pre-fix scan, taking the LAST, picked `int + Money` and emitted
// `add %record.Money, i32` — "Both operands to a binary operator are not of
// the same type!"). The selection must be by operand type, not by position.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           three_plus_overloads_select_the_exact_one) {
    JitResult r = runAotChecked(recordProgram(std::string(kPlusMoneyMoney) +
                                               kPlusMoneyInt + kPlusIntMoney));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8261 — CONTROL: a record whose `operator +` is `Money + Money` ONLY. The
// operand-type filter must still select it (one candidate, one match).
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           money_plus_money_only_control) {
    JitResult r = runAotChecked(recordProgram(kPlusMoneyMoney));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\nz\n");
}

// ===========================================================================
// #8338 — the issue's exact program: the SELECTOR `Sum()` in a generic body
// at T = double. It ABORTED at compile time ("%sum.r = load i32" fed to
// `bz_double_to_string_r(double)`); the expected output is 3.75.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           selector_sum_generic_body_double) {
    JitResult r =
        runAotChecked(selectorProgram("double", "    a.Add(1.5); a.Add(2.25);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3.75\n");
}

// ===========================================================================
// #8338 — the same shape at T = float (also an IR-verification abort before
// the fix).
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           selector_sum_generic_body_float) {
    JitResult r =
        runAotChecked(selectorProgram("float", "    a.Add(1.5f); a.Add(2.25f);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3.75\n");
}

// ===========================================================================
// #8338 — the SILENT half of the same defect: at T = long the unresolved type
// parameter made the accumulator an i32, so the total was truncated to 32
// bits and printed as 410_065_408 instead of 9_000_000_000.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           selector_sum_generic_body_long_is_not_truncated) {
    JitResult r = runAotChecked(selectorProgram(
        "long", "    a.Add(3000000000L); a.Add(6000000000L);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9000000000\n");
}

// ===========================================================================
// #8338 — the same at T = ulong (10_000_000_000, truncated to 1_410_065_408
// before the fix).
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           selector_sum_generic_body_ulong_is_not_truncated) {
    JitResult r = runAotChecked(selectorProgram(
        "ulong", "    a.Add(5000000000UL); a.Add(5000000000UL);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "10000000000\n");
}

// ===========================================================================
// #8338 — CONTROL: the CONCRETE receiver's selector `Sum` (outside any
// generic body) keeps its answer — 3.75.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           concrete_selector_sum_control) {
    JitResult r = runAotChecked(
        "module M;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<double>();\n"
        "    a.Add(1.5); a.Add(2.25);\n"
        "    Console.WriteLine(a.Sum(lambda(x) => x));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3.75\n");
}

// ===========================================================================
// CONTROLS — the primitive no-selector folds (stdlib §5.1 over the eleven
// primitive conformers) are untouched by both fixes.
// ===========================================================================
BEZEL_TEST(codegen_linq_sum_operator_exact_overload_8261_8338,
           primitive_sums_are_unchanged) {
    JitResult r = runAotChecked(
        "module M;\n"
        "public class Program\n"
        "  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();\n"
        "  public static method Main()\n"
        "    var i := new Array<int>(); i.Add(5); i.Add(6);\n"
        "    var d := new Array<double>(); d.Add(1.5); d.Add(2.25);\n"
        "    var l := new Array<long>(); l.Add(3000000000L); l.Add(6000000000L);\n"
        "    Console.WriteLine(i.Sum());\n"
        "    Console.WriteLine(d.Sum());\n"
        "    Console.WriteLine(l.Sum());\n"
        "    Console.WriteLine(H(i));\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n3.75\n9000000000\n11\n");
}
