// Codegen — #6757 (§3.6 rule 2, §14.2; the #5449 ruling, v0.340): a bare type
// parameter's member surface is what its class bounds establish, with their
// inherited members. A built-in exception bound (`where T: Exception`), a
// `where T: U` chain ending in one, and a user class deriving from one all
// carry §31.4's surface: `Message`, `StackTrace` and `CauseChain` on `Exception`
// (inherited by every descendant), `ParamName` on `ArgumentException` and its
// descendants. A member outside that surface stays rejected at the body.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

// §14.2 / §31.4: the built-in bound's own `Message` reaches the body.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_exception_bound_message_spec_14_2_31_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string where T: Exception "
        "    return x.Message; "
        "  end M; "
        "  public static method Main() "
        "    using var io := new IOException(\"m\"); "
        "    Console.WriteLine(M(io)); "
        "  end Main; "
        "end Program;"), "m");
}

// Ownership: the accepted generic read of a `T: Exception` value disposes
// cleanly (`runAotChecked` is the leak / use-after-free oracle).
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_exception_bound_message_owned_checked_spec_14_2_31_4) {
    auto r = runAotChecked(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string where T: Exception "
        "    return x.Message; "
        "  end M; "
        "  public static method Main() "
        "    using var io := new IOException(\"m\"); "
        "    Console.WriteLine(M(io)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("m") != std::string::npos);
}

// §31.4 `StackTrace: string` — "" when unavailable; the bare-bound read is typed
// as `string`, so the comparison against "" type-checks and holds.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_exception_bound_stacktrace_spec_31_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string where T: Exception "
        "    return x.StackTrace; "
        "  end M; "
        "  public static method Main() "
        "    using var io := new IOException(\"m\"); "
        "    if M(io) = \"\" then Console.WriteLine(\"empty\"); end if; "
        "  end Main; "
        "end Program;"), "empty");
}

// §31.4 `ArgumentException`: `ParamName: string?` — a nullable string, read
// through a `where T: ArgumentException` bound.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_argument_exception_bound_paramname_spec_31_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string? where T: ArgumentException "
        "    return x.ParamName; "
        "  end M; "
        "  public static method Main() "
        "    using var a := new ArgumentException(\"m\", \"p\"); "
        "    Console.WriteLine(M(a) ?? \"none\"); "
        "  end Main; "
        "end Program;"), "p");
}

// §31.4: `ArgumentException: Exception` — its bound inherits `Message`.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_argument_exception_bound_inherits_message_spec_31_4) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string where T: ArgumentException "
        "    return x.Message; "
        "  end M; "
        "  public static method Main() "
        "    using var a := new ArgumentException(\"m\", \"p\"); "
        "    Console.WriteLine(M(a)); "
        "  end Main; "
        "end Program;"), "m");
}

// §14.2 "class bound … with their inherited members": a user class deriving
// from the built-in `Exception` supplies the inherited `Message`.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_user_exception_class_bound_inherits_message_spec_14_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module Mod; "
        "public class AppError: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end AppError; "
        "public class Program "
        "  public static method M<T>(x: T): string where T: AppError "
        "    return x.Message; "
        "  end M; "
        "  public static method Main() "
        "    using var e := new AppError(\"m\"); "
        "    Console.WriteLine(M(e)); "
        "  end Main; "
        "end Program;"), "m");
}

// §14.2 `where T: U` chain: `T`'s surface is `U`'s, and `U: Exception` carries
// `Message`.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_type_param_chain_to_exception_bound_spec_14_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module Mod; "
        "public class Program "
        "  public static method M<T, U>(x: T): string where T: U where U: Exception "
        "    return x.Message; "
        "  end M; "
        "  public static method Main() "
        "    using var io := new IOException(\"m\"); "
        "    Console.WriteLine(M<IOException, Exception>(io)); "
        "  end Main; "
        "end Program;"), "m");
}

// Reject (stays rejected): `ParamName` is `ArgumentException`'s own, not
// `Exception`'s (§31.4), so a `where T: Exception` bound does not supply it.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_exception_bound_paramname_still_rejected_spec_31_4) {
    auto r = runAot(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string? where T: Exception "
        "    return x.ParamName; "
        "  end M; "
        "  public static method Main() "
        "    using var io := new IOException(\"m\"); "
        "    Console.WriteLine(M(io) ?? \"none\"); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `ParamName` on bare type parameter"));
}

// Reject (stays rejected): `InnerException` is `TypeInitializationException`'s
// (§31.4), not a member of `Exception`.
BEZEL_TEST(bare_typeparam_exception_surface_6757, where_exception_bound_innerexception_still_rejected_spec_31_4) {
    auto r = runAot(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string where T: Exception "
        "    return x.InnerException; "
        "  end M; "
        "  public static method Main() "
        "    using var io := new IOException(\"m\"); "
        "    Console.WriteLine(M(io)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `InnerException` on bare type parameter"));
}

// Reject (stays rejected): an unconstrained `T` has no class bound to supply
// `Message` (§3.6 rule 2), and nothing about the bound changed.
BEZEL_TEST(bare_typeparam_exception_surface_6757, unconstrained_type_param_message_still_rejected_spec_3_6) {
    auto r = runAot(
        "module Mod; "
        "public class Program "
        "  public static method M<T>(x: T): string "
        "    return x.Message; "
        "  end M; "
        "  public static method Main() "
        "    using var io := new IOException(\"m\"); "
        "    Console.WriteLine(M(io)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Message` on bare type parameter"));
}

// Control (unchanged): a user class bound's own declared member, read through
// the bare parameter, still resolves as before (§14.2).
BEZEL_TEST(bare_typeparam_exception_surface_6757, user_class_bound_declared_member_control_spec_14_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module Mod; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public method Name(): string => \"b\"; "
        "  destructor end destructor; "
        "end Base; "
        "public class Program "
        "  public static method M<T>(x: T): string where T: Base "
        "    return x.Name(); "
        "  end M; "
        "  public static method Main() "
        "    using var b := new Base(); "
        "    Console.WriteLine(M(b)); "
        "  end Main; "
        "end Program;"), "b");
}
