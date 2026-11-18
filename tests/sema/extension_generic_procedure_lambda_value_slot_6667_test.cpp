// fix #6667 (§26.5 rule 2, §27.3, v0.359):
//
//   §27.3: "A target type, when present, is checked against this
//    classification — a procedure-bodied lambda does not convert to a
//    value-returning function type, nor the reverse."
//
//   §26.5 step 2: an extension block's type parameter "is inferred from the
//    receiver's type".
//
// A GENERIC extension block's member has no `parent` TypeSymbol
// (Symbol.hpp:998), so `ExprTC::formalTypeAtReceiver` returned the formal
// UNSUBSTITUTED: the block's free type variable `T` reached the caller as a
// bare, declaration-less Named type-parameter ref. Every §27.3
// body-vs-target predicate excludes such a slot ("a bare type-parameter slot
// may still be an unsubstituted callee type parameter here"), so the
// procedure-lambda check never ran and a procedure-bodied lambda was
// admitted at the block's `() -> T` value slot — the callee then read a
// return value the body never produced.
//
// MEASURED on the base (b279e8daf): the issue's repro compiles and prints a
// garbage integer that changes from run to run.
//
// Every row below is a REJECTION row: each one is RED on the base (the
// program compiles there) and GREEN with the fix. The base-green controls —
// the class-level generic twin, the same block with a UNIT member beside the
// value member (which selects the unit member and is already correct), and a
// VALUE lambda at the same slot — are in the lane's corpus-candidates
// directory, not here.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// The §27.3 verdict a procedure-bodied lambda gets at a value slot.
#define PROC_BODY_MSG                                                        \
    "a procedure-bodied lambda (no path executes a value-carrying `return`) " \
    "does not convert to a function type whose return type is a value"

// REJECT: the issue's exact repro — a generic extension block whose member
// takes the block's free `T` at a `() -> T` slot, called on an `Array<int>`
// receiver.
BEZEL_TEST(extension_generic_procedure_lambda_value_slot_6667,
           generic_extension_procedure_lambda_at_value_slot_rejected_spec_26_5) {
    auto r = runAot(
        "module T;\n"
        "extension Array<T>\n"
        "  public method E(f: () -> T): T\n"
        "    return f();\n"
        "  end E;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var xs: Array<int> := [1];\n"
        "    Console.WriteLine(xs.E(lambda() => do Console.Write(\"p\"); end));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":10:28: " PROC_BODY_MSG));
}

// REJECT: the same block, the member spelled with an expression body, and a
// STRING receiver — the substitution is per receiver instantiation, so the
// slot is `() -> string` here.
BEZEL_TEST(extension_generic_procedure_lambda_value_slot_6667,
           generic_extension_procedure_lambda_at_string_slot_rejected_spec_26_5) {
    auto r = runAot(
        "module T;\n"
        "extension Array<T>\n"
        "  public method E(f: () -> T): T => f();\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var xs: Array<string> := [\"s\"];\n"
        "    Console.WriteLine(xs.E(lambda() => do Console.Write(\"p\"); end));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":8:28: " PROC_BODY_MSG));
}
