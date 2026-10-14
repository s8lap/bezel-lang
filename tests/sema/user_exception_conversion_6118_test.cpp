// fix #6118 (§5.1 "Reference conversions", §31.4, §12.7, v0.338): a user
// class and the built-in exception hierarchy.
//
// §5.1 converts a class implicitly only "to base class, transitively, up to
// `Object`". The issue's repro
//
//     public class MyEx: IOException … end MyEx;
//     var my := new MyEx("m"); var a: ArgumentException := my;
//     Console.WriteLine($"{a.Message.Length} {a is ArgumentException}");
//
// compiled and printed `1 False`: `ArgumentException` is not a base of `MyEx`.
// #6023 rejected the built-in → built-in sibling pair; the USER-class source
// was not covered. The reject rows take the conversion at each position and
// for each kind of non-base target; the accept row runs the upcasts.
//
// The issue's two false rejects are the #6083 shapes #6023 keeps for a
// built-in exception target — inside a type-pattern arm and after an exit
// test — with a user-declared exception as the tested type. They are admitted
// on the same record and the same terms, and the rows after them bound it.
// Every accept row runs on runAot and asserts its output.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

constexpr const char* kPrelude = R"bzl(module M6118;
public class Plain
  public constructor() end constructor;
  public Message: string := "p";
end Plain;
public class MyEx: IOException
  public constructor(m: string) base(m); end constructor;
  public code: int := 4;
end MyEx;
public class Deep: MyEx
  public constructor(m: string) base(m); end constructor;
  public extra: int := 6;
end Deep;
public class P
  public static method TakeArg(a: ArgumentException): int => a.Message.Length;
  public static method T(my: MyEx, e0: Exception, other: Exception): int
    var ex: Exception := e0;
    )bzl";

// `extra` is a declaration placed before `class P`.
JitResult runBody(const char* body, const char* second,
                  const char* extra = "") {
    std::string pre = kPrelude;
    pre.insert(pre.find("public class P"), extra);
    return runAot(pre + body + R"bzl(
  end T;
  public static method Main(): int
    var my := new MyEx("m"); var e1 := new )bzl" + second + R"bzl(("m");
    var e2 := new IOException("i"); var oth := new Exception("o");
    Console.WriteLine($"{T(my, e1, oth)} {T(my, e2, oth)}");
    return 0;
  end Main;
end P;
)bzl");
}

// A user class whose bases do not include the built-in exception target.
void expectNotABase(const JitResult& r, const char* target) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type (§5.1)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the built-in exception type `") + target +
            "` is not a base of `").c_str()));
}

// The pre-existing built-in exception → user class rejection.
void expectUserDowncastRejected(const JitResult& r, const char* target) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the built-in exception type `Exception` is not a `") +
            target + "`").c_str()));
}

}  // namespace

// THE ISSUE'S REPRO: `MyEx: IOException` initialising an `ArgumentException`.
BEZEL_TEST(sema_user_exception_conversion_6118,
           issue_repro_user_exception_into_unrelated_builtin_exception_local_spec_5_1) {
    auto r = runBody(R"bzl(var a: ArgumentException := my; return a.Message.Length;)bzl", "MyEx");
    expectNotABase(r, "ArgumentException");
}

BEZEL_TEST(sema_user_exception_conversion_6118,
           user_exception_assigned_to_unrelated_builtin_exception_spec_5_1) {
    auto r = runBody(R"bzl(var a: ArgumentException := new ArgumentException("z"); a := my; return a.Message.Length;)bzl", "MyEx");
    expectNotABase(r, "ArgumentException");
}

BEZEL_TEST(sema_user_exception_conversion_6118,
           user_exception_argument_to_unrelated_builtin_exception_parameter_spec_5_1) {
    auto r = runBody(R"bzl(return TakeArg(my);)bzl", "MyEx");
    expectNotABase(r, "ArgumentException");
}

BEZEL_TEST(sema_user_exception_conversion_6118,
           user_exception_returned_as_unrelated_builtin_exception_spec_5_1) {
    auto r = runBody(R"bzl(return Q.Ret().Message.Length;)bzl", "MyEx", R"bzl(public class Q
  public static method Ret(): ArgumentException
    return new MyEx("r");
  end Ret;
end Q;
)bzl");
    expectNotABase(r, "ArgumentException");
}

// A built-in exception DERIVED from the user class's base is not a base of it.
BEZEL_TEST(sema_user_exception_conversion_6118,
           user_exception_into_builtin_exception_derived_from_its_base_spec_5_1) {
    auto r = runBody(R"bzl(var a: FileNotFoundException := my; return a.Message.Length;)bzl", "MyEx");
    expectNotABase(r, "FileNotFoundException");
}

BEZEL_TEST(sema_user_exception_conversion_6118,
           user_exception_into_nullable_unrelated_builtin_exception_spec_5_1) {
    auto r = runBody(R"bzl(var a: ArgumentException? := my; return 0;)bzl", "MyEx");
    expectNotABase(r, "ArgumentException");
}

// Transitively: `Deep: MyEx: IOException`.
BEZEL_TEST(sema_user_exception_conversion_6118,
           class_derived_from_user_exception_into_unrelated_builtin_exception_spec_5_1) {
    auto r = runBody(R"bzl(var a: ArgumentException := new Deep("d"); return a.Message.Length;)bzl", "MyEx");
    expectNotABase(r, "ArgumentException");
}

// A class with no exception base at all.
BEZEL_TEST(sema_user_exception_conversion_6118,
           non_exception_class_into_builtin_exception_spec_5_1) {
    auto r = runBody(R"bzl(var a: Exception := new Plain(); return a.Message.Length;)bzl", "MyEx");
    expectNotABase(r, "Exception");
}

// ACCEPT. The upcasts §5.1 lists: to the direct base and transitively.
BEZEL_TEST(sema_user_exception_conversion_6118,
           user_exception_into_its_builtin_bases_accepted_spec_5_1) {
    auto r = runBody(R"bzl(var io: IOException := my; var e: Exception := my; var d: Exception := new Deep("d"); var o: Object := my; return io.Message.Length * e.Message.Length * d.Message.Length;)bzl", "MyEx");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 1\n"));
}

// ACCEPT (the issue's first false reject). Inside the type-pattern arm the
// carrier was tested against `MyEx`, as it is for a built-in target.
BEZEL_TEST(sema_user_exception_conversion_6118,
           type_pattern_arm_admits_the_tested_user_exception_spec_12_7) {
    auto r = runBody(R"bzl(case ex of MyEx => do var back: MyEx := ex; return back.code; end, _ => do return -1; end end case; return -7;)bzl", "MyEx");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4 -1\n"));
}

// ACCEPT (the second). After an exit test.
BEZEL_TEST(sema_user_exception_conversion_6118,
           exit_test_admits_the_tested_user_exception_spec_12_7) {
    auto r = runBody(R"bzl(if ex is not MyEx then return -1; end if; var back: MyEx := ex; return back.code;)bzl", "MyEx");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4 -1\n"));
}

// ACCEPT. A test against `Deep` admits `MyEx`, its base.
BEZEL_TEST(sema_user_exception_conversion_6118,
           exit_test_against_a_derived_user_exception_admits_its_base_spec_12_7) {
    auto r = runBody(R"bzl(if ex is not Deep then return -1; end if; var back: MyEx := ex; return back.code;)bzl", "Deep");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4 -1\n"));
}

// REJECT. Without the test the conversion is the downcast it always was.
BEZEL_TEST(sema_user_exception_conversion_6118,
           user_exception_downcast_without_a_test_still_rejected_spec_5_1) {
    auto r = runBody(R"bzl(var back: MyEx := ex; return back.code;)bzl", "MyEx");
    expectUserDowncastRejected(r, "MyEx");
}

// REJECT. A test against `MyEx` does not admit `Deep`.
BEZEL_TEST(sema_user_exception_conversion_6118,
           exit_test_against_the_base_does_not_admit_the_derived_user_exception_spec_5_1) {
    auto r = runBody(R"bzl(if ex is not MyEx then return -1; end if; var back: Deep := ex; return back.code;)bzl", "MyEx");
    expectUserDowncastRejected(r, "Deep");
}

// REJECT. The record ends with a reassignment, as for a built-in target.
BEZEL_TEST(sema_user_exception_conversion_6118,
           exit_test_then_reassignment_ends_the_record_spec_7_4) {
    auto r = runBody(R"bzl(if ex is not MyEx then return -1; end if; ex := other; var back: MyEx := ex; return back.code;)bzl", "MyEx");
    expectUserDowncastRejected(r, "MyEx");
}
