// Spec-derived regression test for issue #7227 — `Math.Sign`'s type-parameter
// CONSTRAINT was not enforced.
//
// spec §14.2 "Constraints" (v0.343);
// stdlib §2.2 `Math`; stdlib §2.19 `INumber<T>`.
//
// WHAT THE DEFECT WAS.
//
// stdlib §2.2 declares
//
//   public static method Sign<T>(x: T): int where T: INumber<T>;
//
// and stdlib §2.19's conformance table says the primitive NUMERIC TOWER
// conforms to `INumber<T>`, while `char` conforms to `ISteppable<char>` and
// `IParsable<char>` ONLY ("`char` is not an `INumber<T>` conformer",
// #6956) and `bool` and `string` conform to NONE of the three.
//
// The SIBLING constraints are enforced: `Math.Min`, `Math.Max` and
// `Math.Clamp` all reject an argument that does not satisfy their `where`
// clause. `Math.Sign` did not, so it was the odd one out:
//
//   Math.Sign("x")   -> bezelc: codegen error: IR verification failed
//                       (Both operands to ICmp instruction are not of the
//                       same type!)   — a COMPILER ABORT
//   Math.Sign(true)  -> compiles clean, prints -1   — a SILENT WRONG ANSWER
//
// THE RULE.
//
// §14.2 (v0.343): a type argument that does not satisfy the declared
// constraint is rejected AT THE CALL. §5.1 does not reach the argument
// here — its rule applies "where the target type is known", and with a bare
// type parameter the target type is NOT known — so the rule that governs is
// §14.2's constraint, and a constraint check must REJECT, never coerce.
//
// THE ORACLE.
//
// The refusals are pinned by their DIAGNOSTIC and their LOCATION, and by
// the ABSENCE of the two pre-fix symptoms (the LLVM-verifier abort and the
// silent `-1`). The legal spellings are pinned by their VALUE.
//
// A BEHAVIOUR FLIP this file records: `Math.Sign('c')` COMPILED and printed
// `1` before the fix. stdlib §2.2's `where T: INumber<T>` and stdlib
// §2.19's conformance table ("`char` is not an `INumber<T>` conformer")
// make that call a compile error, so it is refused now. No test pinned the
// old behaviour.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

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

// The §14.2 constraint refusal: the program did not run (so the silent `-1`
// is gone), the driver rejected it at a REAL source location (never a crash
// / signal / timeout / ASan report), and the diagnostic carries the
// constraint wording.
void expectConstraintRefused(const JitResult& r, const char* typeName) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("type argument `") + typeName +
            "` does not satisfy the constraint `where T: INumber<T>`")
                .c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "Math.Sign"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§14.2"));
    // The pre-fix symptoms: the compiler aborted inside the LLVM verifier,
    // and a `bool` argument compiled and ran and printed -1.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
}

}  // namespace

// ===========================================================================
// The REFUSALS.
// ===========================================================================

// The issue's own repro (a): a `string` argument. Before the fix this
// ABORTED the compiler inside the LLVM verifier.
BEZEL_TEST(math_sign_constraint_7227, sign_string_argument_is_refused_14_2) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sign(\"x\"));"));
    expectConstraintRefused(r, "string");
}

// The issue's own repro (b): a `bool` argument. Before the fix this
// COMPILED and printed -1 — a SILENT WRONG ANSWER, because `bool` conforms
// to none of stdlib §2.19's three static abstractions.
BEZEL_TEST(math_sign_constraint_7227, sign_bool_argument_is_refused_14_2) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sign(true));"));
    expectConstraintRefused(r, "bool");
}

// stdlib §2.19: "`char` to `ISteppable<char>` and `IParsable<char>` only …
// `char` is not an `INumber<T>` conformer". This row is the BEHAVIOUR FLIP
// the file's header records: it compiled and printed `1` before the fix.
BEZEL_TEST(math_sign_constraint_7227, sign_char_argument_is_refused_14_2) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sign('c'));"));
    expectConstraintRefused(r, "char");
}

// The `null` LITERAL is §7's question (a non-nullable parameter), not the
// constraint's: §7.3 refuses it, and the §7 gate owns that shape.
// `Math.Sign`'s parameter is the type parameter `T` constrained
// `where T: INumber<T>` (stdlib §2.2) — a VALUE type, so the parameter is
// non-nullable and the `null` literal reaches it by no route.
BEZEL_TEST(math_sign_constraint_7227, sign_null_literal_is_refused_7) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sign(null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Math.Sign` is nullable but the target type is "
           "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// ===========================================================================
// The §7.4-NARROWED argument — §22.6 reads the argument's type AT THE CALL.
// ===========================================================================

// §22.6 (v0.343): "The argument's static type is its type at the call: a
// read of a binding §7.4 has narrowed non-null is of its non-nullable type."
//
// So a §7.4-NARROWED `string?` is a `string` AT THE CALL — and a `string`
// conforms to no `INumber<T>` (stdlib §2.19), so the CONSTRAINT owns this
// call. Before the fix the narrowed read was judged by its DECLARED type
// (`string?`), the constraint did not fire, and the call reached codegen
// and ABORTED the compiler inside the LLVM verifier:
//   "Both operands to ICmp instruction are not of the same type!
//    %sgn.neg = icmp slt ptr %2, i0 0"
BEZEL_TEST(math_sign_constraint_7227,
           sign_narrowed_nullable_string_argument_is_refused_14_2) {
    auto r = runAot(prog(
        "    var s: string? := \"x\"; "
        "    Console.WriteLine(Math.Sign(s));"));
    expectConstraintRefused(r, "string");
}

// The same shape with a `bool`: §7.4's declaration-assignment narrowing
// discharges the nullability, and `bool` conforms to none of stdlib §2.19's
// three static abstractions.
BEZEL_TEST(math_sign_constraint_7227,
           sign_narrowed_nullable_bool_argument_is_refused_14_2) {
    auto r = runAot(prog(
        "    var b: bool? := true; "
        "    Console.WriteLine(Math.Sign(b));"));
    expectConstraintRefused(r, "bool");
}

// And with a `char`: stdlib §2.19 says "`char` is not an `INumber<T>`
// conformer" (#6956).
BEZEL_TEST(math_sign_constraint_7227,
           sign_narrowed_nullable_char_argument_is_refused_14_2) {
    auto r = runAot(prog(
        "    var c: char? := 'c'; "
        "    Console.WriteLine(Math.Sign(c));"));
    expectConstraintRefused(r, "char");
}

// The NULL-TEST spelling of the same narrowing (§7.4's first form): the read
// inside the controlled block is a `string` at the call, so the constraint
// owns it there too.
BEZEL_TEST(math_sign_constraint_7227,
           sign_null_test_narrowed_string_argument_is_refused_14_2) {
    auto r = runAot(prog(
        "    var s: string? := \"x\"; "
        "    if s <> null then Console.WriteLine(Math.Sign(s)); end if;"));
    expectConstraintRefused(r, "string");
}

// ===========================================================================
// The CONTROLS — the numeric tower CONFORMS to `INumber<T>` (stdlib §2.19),
// so every conforming spelling still works and still prints its sign.
// ===========================================================================

BEZEL_TEST(math_sign_constraint_7227, sign_negative_int_is_minus_one_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(-3));")),
                    "-1");
}

BEZEL_TEST(math_sign_constraint_7227, sign_zero_is_zero_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(0));")),
                    "0");
}

BEZEL_TEST(math_sign_constraint_7227, sign_positive_int_is_plus_one_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(3));")),
                    "1");
}

BEZEL_TEST(math_sign_constraint_7227, sign_double_is_plus_one_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(2.5));")),
                    "1");
}

BEZEL_TEST(math_sign_constraint_7227, sign_decimal_is_plus_one_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(2.5m));")),
                    "1");
}

// The unsigned widths are in the numeric tower too (stdlib §2.19).
BEZEL_TEST(math_sign_constraint_7227, sign_unsigned_is_plus_one_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(200));")),
                    "1");
}

// `Math.Sign` always returns `int` (stdlib §2.2), whatever the argument's
// own type.
BEZEL_TEST(math_sign_constraint_7227, sign_long_is_plus_one_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sign(3L));")),
                    "1");
}
