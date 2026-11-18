// fix #6696 (§27.3 *Admission at a target-typed non-call position*, §5.1,
// §7.4, v0.359):
//
//   §27.3: "the body is typed, and its inferred return type must equal the
//    target's return slot or implicitly convert to it (§5.1); otherwise the
//    lambda is a compile error at the position".
//
//   §5.1: "`S?` implicitly converts to `T?`" — there is NO `S?` -> `T`
//    conversion, so a nullable body cannot inhabit a non-nullable return
//    slot.
//
//   §7.4 *Narrowing inside a lambda body*: "a test on a `var` local or a
//    parameter does not reach a lambda formed under it" — the body of a
//    lambda over a `var` local is read at the local's DECLARED type, so a
//    `var k: int?` body is `int?` at a `() -> int` slot.
//
// The target return slot was threaded to a lambda at a `var` local's
// initializer and at a call argument, but NOT at a `const` local's: the
// `LocalConstDecl` arm visited the initializer with no target thread at all,
// so the §27.3 body-vs-target check never ran for the `const` spelling.
//
// MEASURED on the base (b279e8daf): the `int?` program compiles and prints
// `0` (the null payload); the `string?` program compiles and SEGFAULTs on
// `f().Length`.
//
// Every row below is a REJECTION row: each one is RED on the base (the
// program compiles there) and GREEN with the fix. The base-green controls —
// the `var` spelling of the same program (already rejected), the `const k:
// int? := 5` spelling (whose narrowing fact DOES enter the body, §7.4) and a
// value body at the same slot — are in the lane's corpus-candidates
// directory, not here.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// The §5.1 / §7 verdict a nullable body gets at a non-nullable return slot.
#define NULLABLE_BODY_MSG                                                  \
    "the lambda body's returned value is nullable but the target type is " \
    "non-nullable"

// REJECT: the issue's exact repro — a `const` local holding a function-typed
// value, whose lambda body reads a nullable `var` local.
BEZEL_TEST(const_local_lambda_body_target_6696,
           const_local_lambda_over_nullable_var_rejected_spec_5_1) {
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Mk(): int? => null;\n"
        "  public static method Main(): int\n"
        "    var k: int? := P.Mk();\n"
        "    const f: () -> int := lambda() => k;\n"
        "    Console.WriteLine(f());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":6:39: " NULLABLE_BODY_MSG));
}

// REJECT: the same program with a `string?` body — on the base it compiles
// and SEGFAULTs on `f().Length`, because nothing ever checked the body's
// type against the target's return slot.
BEZEL_TEST(const_local_lambda_body_target_6696,
           const_local_lambda_over_nullable_string_rejected_spec_5_1) {
    auto r = runAot(
        "module M;\n"
        "public class P\n"
        "  public static method Mk(): string? => null;\n"
        "  public static method Main(): int\n"
        "    var k: string? := P.Mk();\n"
        "    const f: () -> string := lambda() => k;\n"
        "    Console.WriteLine(f().Length);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":6:42: " NULLABLE_BODY_MSG));
}
