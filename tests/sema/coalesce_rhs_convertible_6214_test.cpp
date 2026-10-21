// #6214 (spec §7.7, §5.1, v0.339): "for a left operand of type `T?`, the right
// operand must be implicitly convertible (§5.1) to `T` or to `T?`".
//
// MEASURED on the pre-fix base (f9c549e0): `fnNullable ?? "s"` and
// `Acc? ?? "str"` were accepted by sema (and ran), and `string? ?? 5` passed
// sema and died in the backend with "PHI node operands are not the same type
// as the result" (IR verification failed). The `??` arm of sema's binary-
// operator typing derived the result type from the operands but never checked
// the right operand against the left's type.
//
// Category A rejects each mismatched right operand at the §5.1 conversion
// site. Category B are accepting controls on the same shapes: subtype right
// operand, widening right operand, nullable right operand, `null`, and the
// `throw` form (§17.12), all of which must keep compiling and running.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kNeedle =
    "right operand of `??` has a type that is not implicitly convertible to "
    "the target type (\xC2\xA7""5.1)";

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "public class Animal\n"
           "  public constructor() end constructor;\n"
           "end Animal;\n"
           "public class Dog: Animal\n"
           "  public constructor() end constructor;\n"
           "end Dog;\n"
           "public class Program\n"
           "  public static method Pick(some: bool): ((int) -> int)?\n"
           "    if some then return lambda(x: int) => x * 2; end;\n"
           "    return null;\n"
           "  end Pick;\n"
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// Category A

// Issue repro (3): reached the backend as malformed IR.
BEZEL_TEST(coalesce_rhs_convertible_6214, string_nullable_coalesce_int_rejected_spec_7_7) {
    auto r = runAot(prog("    var s: string? := null;\n"
                         "    var r := s ?? 5;\n"
                         "    Console.WriteLine(r);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "IR verification"));
}

// Issue repro (2).
BEZEL_TEST(coalesce_rhs_convertible_6214, class_nullable_coalesce_string_rejected_spec_7_7) {
    auto r = runAot(prog("    var a: Animal? := null;\n"
                         "    var q := a ?? \"str\";\n"
                         "    Console.WriteLine(q is string);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
}

// Issue repro (1): function-typed nullable left.
BEZEL_TEST(coalesce_rhs_convertible_6214, function_nullable_coalesce_string_rejected_spec_7_7) {
    auto r = runAot(prog("    var nn := Program.Pick(true);\n"
                         "    var o: Object := nn ?? \"s\";\n"
                         "    Console.WriteLine(o is string);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
}

// A supertype is not convertible down to the left's type.
BEZEL_TEST(coalesce_rhs_convertible_6214, subclass_nullable_coalesce_base_rejected_spec_7_7) {
    auto r = runAot(prog("    var d: Dog? := null;\n"
                         "    var q := d ?? new Animal();\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
}

// Category B

BEZEL_TEST(coalesce_rhs_convertible_6214, matching_and_widening_operands_still_run_spec_7_7) {
    auto r = runAot(prog("    var s: string? := null;\n"
                         "    var l: long? := null;\n"
                         "    var i: int? := 4;\n"
                         "    Console.WriteLine(s ?? \"x\");\n"
                         "    Console.WriteLine(l ?? 5);\n"
                         "    Console.WriteLine(i ?? 9);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x\n5\n4\n"));
}

BEZEL_TEST(coalesce_rhs_convertible_6214, subtype_nullable_and_null_operands_still_run_spec_7_7) {
    auto r = runAot(prog("    var a: Object? := null;\n"
                         "    var b: Object := a ?? \"sub\";\n"
                         "    var c: Object? := a ?? null;\n"
                         "    var s: string? := null;\n"
                         "    var t: string? := null;\n"
                         "    var u := s ?? t ?? \"end\";\n"
                         "    Console.WriteLine(b is string);\n"
                         "    Console.WriteLine(c is null);\n"
                         "    Console.WriteLine(u);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nend\n"));
}

BEZEL_TEST(coalesce_rhs_convertible_6214, throw_right_operand_still_runs_spec_7_7) {
    auto r = runAot(prog("    var s: string? := \"v\";\n"
                         "    var r: string := s ?? throw new InvalidOperationException(\"no\");\n"
                         "    Console.WriteLine(r);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v\n"));
}
