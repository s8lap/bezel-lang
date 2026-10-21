// #6684 (spec §5.1 "Reference conversions", §17.5, v0.339): `x in c` IS the call
// `c.Contains(x)`, admitted exactly when `x` is admissible as that call's
// argument, and §5.1's reference conversions are a closed list (identity,
// class to base class, any type to `Object`, a type to an interface it
// implements, ...). `Exception` has no conversion to `StringBuilder`, nor the
// reverse, nor does any built-in class to another unrelated built-in class.
// Before the fix `checkAssignable`'s catch-all accepted the pair (#6073 item 4),
// so `a.Contains(b)` / `b in a` over `Array<StringBuilder>` with `b: Exception`
// compiled and ran, and so did `F(b)` against `F(s: StringBuilder)`.
//
// Control rows pin the conversions that must stay admitted: a built-in
// exception to its base, anything to `Object`, a class to `IDisposable`
// (§28.2), a user class to a user base class, and the identity.

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
const char* kWhy = "the built-in type `Exception` is not a `StringBuilder`";

std::string prog(const std::string& body) {
    return "module Conf;\n"
           "public class Dog end Dog;\n"
           "public class Cat: Dog end Cat;\n"
           "public class Program\n"
           "  public static method F(s: StringBuilder) end F;\n"
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n";
}
}  // namespace

BEZEL_TEST(builtin_class_unrelated_6684,
           r1_issue_repro_contains_exception_in_array_stringbuilder) {
    auto r = runAot(prog(
        "    var a: Array<StringBuilder> := [new StringBuilder()];\n"
        "    using var b := new Exception(\"x\");\n"
        "    var found := a.Contains(b);\n"
        "    Console.WriteLine(found);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "`Array<T>.Contains`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kWhy));
}

BEZEL_TEST(builtin_class_unrelated_6684,
           r2_issue_repro_in_exception_in_array_stringbuilder) {
    auto r = runAot(prog(
        "    var a: Array<StringBuilder> := [new StringBuilder()];\n"
        "    using var b := new Exception(\"x\");\n"
        "    var found := b in a;\n"
        "    Console.WriteLine(found);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "left operand of `in` over `Array`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kWhy));
}

BEZEL_TEST(builtin_class_unrelated_6684,
           r3_stringbuilder_in_array_exception_reverse_direction) {
    auto r = runAot(prog(
        "    var a: Array<Exception> := [new Exception(\"x\")];\n"
        "    var s := new StringBuilder();\n"
        "    Console.WriteLine(s in a);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "the built-in type `StringBuilder` is not a `Exception`"));
}

BEZEL_TEST(builtin_class_unrelated_6684,
           r4_random_argument_for_stringbuilder_parameter) {
    auto r = runAot(prog("    F(new Random());\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "the built-in type `Random` is not a `StringBuilder`"));
}

BEZEL_TEST(builtin_class_unrelated_6684,
           c1_builtin_exception_to_base_and_object_and_disposable_admitted) {
    auto r = runAot(prog(
        "    using var e := new IOException(\"x\");\n"
        "    var exs: Array<Exception> := [e];\n"
        "    var objs: Array<Object> := [new Object()];\n"
        "    var disp: Array<IDisposable> := [new StringBuilder()];\n"
        "    var sb := new StringBuilder();\n"
        "    Console.WriteLine(e in exs);\n"
        "    Console.WriteLine(exs.Contains(e));\n"
        "    Console.WriteLine(sb in objs);\n"
        "    Console.WriteLine(sb in disp);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nFalse\nFalse\n");
}

BEZEL_TEST(builtin_class_unrelated_6684,
           c2_identity_and_user_derived_class_admitted) {
    auto r = runAot(prog(
        "    var sb := new StringBuilder();\n"
        "    var sbs: Array<StringBuilder> := [sb];\n"
        "    var c := new Cat();\n"
        "    var dogs: Array<Dog> := [c];\n"
        "    F(sb);\n"
        "    Console.WriteLine(sb in sbs);\n"
        "    Console.WriteLine(c in dogs);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n");
}
