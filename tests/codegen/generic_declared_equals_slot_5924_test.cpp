// Regression tests for #5924 (residual, conformance row s9_2_0026): a call of
// `Equals` on a `T`-typed operand, instantiated at a VALUE record whose own-typed
// `Equals` is user-declared, with an `Object?` argument (the "second kind" of
// §9.2 "Through a type parameter"). Base answered `False` for a boxed equal
// value, because the redirect in `tryLowerValueSemanticsCall` declined: a
// record with a user `Equals` is not auto-bearing, so `lowerValueSemMethodOn`
// returned without routing the argument, and the call degraded to a zero with
// an `internal:` warning.
//
// Spec: §9.2 (v0.338), "Through a type parameter": "the second kind reaches the
// same member through the slot: an argument that is a boxed S compares by it
// on the box's payload, and any other argument, `null` included, compares
// unequal".
//
// Only stdout is asserted: `runAot` fills diagnostics only on a reject, so the
// absence of the `internal:` warning is not observable from these rows.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The conformance row s9_2_0026 program, verbatim.
const char* kMoneyProg =
    "module Conf;\n"
    "public record Money\n"
    "  public readonly property Amount: int;\n"
    "  public readonly property Note: string;\n"
    "  public constructor(amount: int, note: string)\n"
    "    Amount := amount;\n"
    "    Note := note;\n"
    "  end constructor;\n"
    "  public method Equals(other: Money): bool => Amount = other.Amount;\n"
    "  public method GetHashCode(): int => Amount;\n"
    "end Money;\n"
    "public class Program\n"
    "  public static method Same<T>(x: T, o: Object?): bool where T: notnull => x.Equals(o);\n"
    "  public static method Main()\n"
    "    var a := new Money(1, \"x\");\n"
    "    var boxed: Object? := new Money(1, \"y\");\n"
    "    Console.WriteLine(Same<Money>(a, boxed));\n"
    "    Console.WriteLine(Same<Money>(a, \"x\"));\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// #5924 (s9_2_0026) / §9.2 (v0.338): a boxed equal Money reaches the declared
// Equals through the slot (True); a string argument compares unequal (False).
BEZEL_TEST(generic_declared_equals_slot_9_2_5924,
           value_record_declared_equals_second_kind_spec_9_2_s9_2_0026) {
    JitResult r = runAotChecked(kMoneyProg);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

// Same shape, looped over 100 iterations with the three argument kinds
// `null`, a boxed value of another record, and a boxed equal Money; the
// counters pin the answers. runAot, not runAotChecked: a boxed `Object?`
// argument handed to a GENERIC callee leaks under the sanitizer whatever the
// callee's body does (measured with `o = null` as the body), the #5951 defect
// the generic_inline_array_universal_members_5806 test records the same way.
// Convert to runAotChecked when #5951 is fixed.
BEZEL_TEST(generic_declared_equals_slot_9_2_5924,
           value_record_declared_equals_slot_loop_spec_9_2_5924) {
    const std::string prog =
        "module Conf;\n"
        "public record Money\n"
        "  public readonly property Amount: int;\n"
        "  public constructor(amount: int)\n"
        "    Amount := amount;\n"
        "  end constructor;\n"
        "  public method Equals(other: Money): bool => (Amount mod 10) = (other.Amount mod 10);\n"
        "  public method GetHashCode(): int => Amount mod 10;\n"
        "end Money;\n"
        "public record Other(Q: int);\n"
        "public class Program\n"
        "  public static method Same<T>(x: T, o: Object?): bool where T: notnull => x.Equals(o);\n"
        "  public static method Main()\n"
        "    var a := new Money(1);\n"
        "    var nSame := 0;\n"
        "    var nNull := 0;\n"
        "    var nOther := 0;\n"
        "    for var j in 0..<100 do\n"
        "      var sameBox: Object? := new Money(11);\n"
        "      var otherBox: Object? := new Other(1);\n"
        "      nSame := nSame + (if Same<Money>(a, sameBox) then 1 else 0);\n"
        "      nNull := nNull + (if Same<Money>(a, null) then 1 else 0);\n"
        "      nOther := nOther + (if Same<Money>(a, otherBox) then 1 else 0);\n"
        "    end for;\n"
        "    Console.WriteLine(nSame.ToString() + \" \" + nNull.ToString() + \" \" + nOther.ToString());\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0 0\n"));
}
