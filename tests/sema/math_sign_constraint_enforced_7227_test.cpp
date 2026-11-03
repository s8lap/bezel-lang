// Spec-derived regression test for issue #7227 — `Math.Sign`'s type-
// parameter CONSTRAINT was not enforced: `Math.Sign("x")` aborted the
// compiler inside LLVM, and `Math.Sign(true)` compiled and SILENTLY
// returned -1.
//
// THE SPEC.
//
//   stdlib §2.2 `Math` (v0.350) declares:
//
//     public static method Sign<T>(x: T): int where T: INumber<T>;
//
//   spec §14.2 "Generic constraints" (v0.350): a type argument that does
//   not satisfy the declared constraint is REJECTED at the call.
//
//   §5.1 does NOT reach the argument here, and that is the point of this
//   file: `Sign`'s declared parameter type is the bare type parameter `T`,
//   so the target type is not known and §5.1's conversion table does not
//   apply.  The rule that DOES reach it is §14.2's constraint — the same
//   rule that already rejects a non-conforming argument at
//   `Math.Min` / `Math.Max` / `Math.Clamp`, whose constraints are
//   `where T: IComparable<T>`.
//
// THE DEFECT.
//
// `Math.Sign` was simply MISSING from the set of members whose constraint
// is enforced, so it was the odd one out:
//
//   Math.Sign("x")   -> the compiler aborted inside LLVM
//                       ("Both operands to ICmp instruction are not of the
//                        same type!")
//   Math.Sign(true)  -> compiled clean and printed -1
//
// `Sign` of `true` is not a number at all, so the second case is a SILENT
// WRONG ANSWER, not a crash.
//
// A constraint check must REJECT, not coerce: `Math.Sign(true)` must be
// REFUSED.
//
// THE ORACLE IS POSITIVE: each refusal row asserts the TEXT of the
// §14.2 constraint diagnostic and that the rejection is located at a real
// source location; the control rows assert the SIGN each legal spelling
// produces.  No row asserts that a diagnostic is absent.
//
// §7 owns the NULLABILITY of an argument, so the `null` literal is refused
// by §7's own rule — a different diagnostic from the constraint's, and the
// row below pins that spelling too.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §14.2 constraint refusal: the program did NOT run, the rejection is a
// located diagnostic at a REAL source location, and the diagnostic carries
// the constraint's wording and its §14.2 citation.
void expectConstraintRefused(const JitResult& r, const char* typeName) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "does not satisfy the constraint"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "where T: INumber<T>"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, typeName));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§14.2"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

}  // namespace

// ===========================================================================
// THE SILENT WRONG ANSWER — `bool` does not conform to `INumber<T>`.
//
// stdlib §2.19 lists the primitive numeric tower's `INumber<T>`
// conformers; `bool` is not among them, so `Math.Sign(true)` must be
// REFUSED with the constraint diagnostic, not answered with -1.
// ===========================================================================
BEZEL_TEST(math_sign_constraint_enforced_7227,
           math_sign_bool_argument_is_refused_by_the_inumber_constraint) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sign(true));"));
    expectConstraintRefused(r, "`bool`");
}

// ===========================================================================
// THE ISSUE'S OTHER HALF — a `string` argument.
//
// `string` conforms to no numeric interface, so `Math.Sign("x")` must be
// REFUSED too (pre-fix it aborted the compiler inside LLVM).
// ===========================================================================
BEZEL_TEST(math_sign_constraint_enforced_7227,
           math_sign_string_argument_is_refused_by_the_inumber_constraint) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sign(\"x\"));"));
    expectConstraintRefused(r, "`string`");
}

// ===========================================================================
// THE `null` LITERAL — §7's nullability rule reaches the argument before
// the constraint's, and the refusal is located too.
// ===========================================================================
BEZEL_TEST(math_sign_constraint_enforced_7227,
           math_sign_null_argument_is_refused_at_its_parameter) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sign(null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "the argument to `Math.Sign` is nullable"));
}

// ===========================================================================
// THE CONTROLS — the legal spellings keep their signs.
//
// stdlib §2.2: "Sign<T>(x: T): int ... -1, 0, or 1".
// ===========================================================================
BEZEL_TEST(math_sign_constraint_enforced_7227,
           math_sign_of_a_negative_number_is_minus_one) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(-3).ToString());")),
                    std::string("-1"));
}

BEZEL_TEST(math_sign_constraint_enforced_7227,
           math_sign_of_zero_is_zero) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(0).ToString());")),
                    std::string("0"));
}

BEZEL_TEST(math_sign_constraint_enforced_7227,
           math_sign_of_a_positive_fraction_is_one) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(2.5).ToString());")),
                    std::string("1"));
}
