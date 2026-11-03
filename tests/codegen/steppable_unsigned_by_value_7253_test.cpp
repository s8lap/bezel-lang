// Spec-derived regression test for issue #7253 — an UNSIGNED operand at
// `ISteppable<T>.Step`'s `by: long` parameter was SIGN-EXTENDED, so the
// VALUE was destroyed.
//
// THE SPEC.
//
//   spec §5.1 "Implicit conversions" (v0.350) lists the widening table:
//
//     | `uint` | `long`, `ulong`, `float`, `double`, `decimal` |
//
//   So an UNSIGNED operand IS admitted at a `long` parameter.  §5.1 is a
//   table of VALUES: an integer conversion that fits the target carries the
//   source's numeric value across.  The `uint` 4000000000 is a POSITIVE
//   number; a conversion that yields -294967296 has not converted the
//   value, it has converted the 32-bit BIT PATTERN.
//
//   stdlib §2.19 "INumber<T>, ISteppable<T>, IParsable<T>" (v0.350)
//   declares:
//
//     public interface ISteppable<T> where T: ISteppable<T>
//       static method Step(value: T, by: long): owned T;
//
//   `by` is declared `long` — a SIGNED type — so an unsigned argument is
//   CONVERTED at the call, and §5.1's conversion converts VALUES.
//
//   spec §11.8 "Static interface members" (v0.350) is the reachability
//   rule: `T.Step(...)` inside a generic body dispatches, at
//   instantiation, to the conforming type's static member; the primitive
//   numeric tower's conformance is compiler-provided.
//
// THE DEFECT.
//
// The `Step` lowering widened the `by` argument to the `long` width with an
// UNCONDITIONAL sign-extending widening, so the `uint` 4000000000 (bit
// pattern 0xEE6B2800) was read as the `int32` -294967296 and arrived as a
// NEGATIVE number:
//
//   T.Step(1.0, 4000000000u)   printed -294967295   (should be 4000000001)
//   T.Step(1.0, 4000000000L)   printed  4000000001   (the control)
//
// The two spellings of the same number disagreed.
//
// THE ORACLE IS POSITIVE: every PIN row asserts the VALUE the documents
// imply.  A program that printed a wrong value (or that was rejected)
// produces a different text, so the row fails.  The CONTROL rows assert the
// SIGNED behaviour that the widening must not disturb.
//
// The `value: T` parameter is GENERIC, so `T` may be a floating type, a
// `decimal` or an integer type (the issue's own note: "the fix must not
// assume `T` is a floating type ... Check all three").  The matrix below
// covers a `double`, a `decimal` and a `float` `T`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// The generic helper whose BODY performs the `ISteppable<T>.Step` call — the
// shape that reaches the compiler-provided primitive conformance (spec §11.8,
// stdlib §2.19).
//
//   params   — the helper's parameter list, e.g. "v: T" or "v: T, by: uint"
//   byExpr   — the `by` argument written at the `T.Step(v, ...)` call
//   mainBody — the statements of `Main`
std::string stepProg(const std::string& params, const std::string& byExpr,
                     const std::string& mainBody) {
    return "module M; static class F "
           "  public static method Nxt<T>(" + params + "): T "
           "      where T: ISteppable<T> "
           "    return T.Step(v, " + byExpr + "); "
           "  end Nxt; "
           "end F; "
           "class C public static method Main(): int " + mainBody +
           "  return 0; "
           "end Main; end C;";
}

// The accepting shape: the program RAN, it produced exactly this text, and
// it did not fail the compiler.
void expectValue(const std::string& program, const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    // The value channel: the program's stdout with ONE trailing line-ending
    // stripped (the same rule `runAotOut` applies), so the expectation is
    // platform-independent.
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// ===========================================================================
// THE PIN ROW — the `uint` spelling of the number, at a `double` `T`.
//
// stdlib §2.19's `Step(value: T, by: long)`: the `by` argument is converted
// to `long`, and §5.1's conversion converts the VALUE.  `1.0 + 4000000000`
// is 4000000001 — exactly what the `long` spelling prints.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_value_7253,
           step_double_uint_by_keeps_its_value_5_1) {
    expectValue(stepProg("v: T", "4000000000u",
                         "  Console.WriteLine(F.Nxt(1.0).ToString()); "),
                "4000000001");
}

// ===========================================================================
// THE ISSUE'S OWN CLAIM — THE TWO SPELLINGS OF THE SAME NUMBER MUST AGREE.
//
// Both helpers run in ONE program, so the row pins the disagreement itself
// and not merely one spelling's value.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_value_7253,
           the_uint_and_long_spellings_agree_5_1) {
    expectValue(
        "module M; static class F "
        "  public static method ByUint<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 4000000000u); "
        "  end ByUint; "
        "  public static method ByLong<T>(v: T): T where T: ISteppable<T> "
        "    return T.Step(v, 4000000000L); "
        "  end ByLong; "
        "end F; "
        "class C public static method Main(): int "
        "  Console.WriteLine(F.ByUint(1.0).ToString()); "
        "  Console.WriteLine(F.ByLong(1.0).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "4000000001\n4000000001");
}

// ===========================================================================
// A `decimal` `T` — stdlib §2.19's `Step` advances in `T`'s own space, and
// §6.4's decimal arithmetic is exact, so the value is 4000000001 exactly.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_value_7253,
           step_decimal_uint_by_keeps_its_value_5_1) {
    expectValue(stepProg("v: T", "4000000000u",
                         "  Console.WriteLine(F.Nxt(1.0m).ToString()); "),
                "4000000001");
}

// ===========================================================================
// A `float` `T` — the same value, at `float`'s own precision.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_value_7253,
           step_float_uint_by_keeps_its_value_5_1) {
    expectValue(stepProg("v: T", "4000000000u",
                         "  Console.WriteLine(F.Nxt(1.0f).ToString()); "),
                "4e+09");
}

// ===========================================================================
// THE `by` AS A NON-CONSTANT SOURCE — a `uint` VARIABLE, so the widening
// is not a constant fold.
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_value_7253,
           step_uint_variable_by_keeps_its_value_5_1) {
    expectValue(stepProg("v: T, by: uint", "by",
                         "  var u: uint := 4000000000u; "
                         "  Console.WriteLine(F.Nxt(1.0, u).ToString()); "),
                "4000000001");
}

// ===========================================================================
// THE CONTROL — a NEGATIVE signed `by` keeps its sign.  This is the row a
// widening that zero-extended INDISCRIMINATELY would break, so it pins
// that the change is signedness-aware and not "always zero-extend".
// ===========================================================================
BEZEL_TEST(steppable_unsigned_by_value_7253,
           step_negative_long_by_keeps_its_sign_5_1) {
    expectValue(stepProg("v: T", "-4000000000L",
                         "  Console.WriteLine(F.Nxt(1.0).ToString()); "),
                "-3999999999");
}
