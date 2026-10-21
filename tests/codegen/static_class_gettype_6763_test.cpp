// #6763 (§11.7 Static classes, §12.2 Hiding is not allowed, §8.2 GetType, v0.340).
// §11.7: "Having no instances, a static class carries none of `Object`'s instance
// surface, ... its static members may take any name of that surface, `Equals`,
// `GetHashCode`, `ToString` and `GetType` included". A call `V.GetType()` on a
// static class that declares its own static `GetType` therefore calls THAT
// member, and §8.2's universal `GetType()` (the codegen refusal of #487) is not
// reached. Base refused every such call: codegen's lowerCallDispatch refused any
// zero-argument `X.GetType()` by name, before the callee was looked up. Sema had
// already bound the callee to the declared static (it types the call as the
// declared return type), so the fix is that the refusal applies only when the
// callee has no declared MethodSymbol.
//
// Rows: the item-19a matrix (static and instance callers, qualified and bare,
// inside and outside the class, a second overload, a string return), exact
// stdout. The non-static controls pin that `GetType` stays universal on a class
// with instances (§8.2): the reflection rejection is unchanged. The Equals /
// GetHashCode / ToString rows are controls that pass on base as well: the
// static-class call routing of those names was never the defect.
//
// Cross-module rows: tests/module/static_class_gettype_cross_module_6763_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §11.7 / §8.2 (#6763): the issue's repro, a static caller of a qualified
// `V.GetType()` on a static class with a declared static `GetType`.
BEZEL_TEST(static_class_gettype_6763, static_caller_qualified_calls_declared_spec_11_7) {
    BEZEL_EXPECT_EQ(runAotOut(R"BZL(
module M;
public static class V
  public static method GetType(): int => 1;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.GetType());
    return 0;
  end Main;
end Program;
)BZL"),
        std::string("1"));
}

// §11.7 (#6763): an instance caller reaches the same declared member.
BEZEL_TEST(static_class_gettype_6763, instance_caller_qualified_calls_declared_spec_11_7) {
    BEZEL_EXPECT_EQ(runAot(R"BZL(
module M;
public static class V
  public static method GetType(): int => 2;
end V;
public class Program
  public method Run(): int
    Console.WriteLine(V.GetType());
    return 0;
  end Run;
  public static method Main(): int
    var p := new Program();
    return p.Run();
  end Main;
end Program;
)BZL").stdoutText,
        std::string("2\n"));
}

// §11.7 (#6763) control, passing on base: a BARE `GetType()` inside the static
// class already binds to the declared member through the enclosing-type lookup.
BEZEL_TEST(static_class_gettype_6763, bare_call_inside_static_class_calls_declared_spec_11_7) {
    BEZEL_EXPECT_EQ(runAotOut(R"BZL(
module M;
public static class V
  public static method GetType(): int => 3;
  public static method Probe(): int => GetType();
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Probe());
    return 0;
  end Main;
end Program;
)BZL"),
        std::string("3"));
}

// §11.7 (#6763): a QUALIFIED `V.GetType()` written inside the static class's
// own body calls the declared member too.
BEZEL_TEST(static_class_gettype_6763, qualified_call_inside_static_class_calls_declared_spec_11_7) {
    BEZEL_EXPECT_EQ(runAotOut(R"BZL(
module M;
public static class V
  public static method GetType(): int => 4;
  public static method Probe(): int => V.GetType();
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Probe());
    return 0;
  end Main;
end Program;
)BZL"),
        std::string("4"));
}

// §11.7 / §8.2 (#6763): the declared return type is the call's type. A string
// return must reach the caller as a string.
BEZEL_TEST(static_class_gettype_6763, declared_string_return_reaches_caller_spec_11_7) {
    BEZEL_EXPECT_EQ(runAotOut(R"BZL(
module M;
public static class V
  public static method GetType(): string => "s7";
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.GetType());
    return 0;
  end Main;
end Program;
)BZL"),
        std::string("s7"));
}

// §11.7 (#6763): the declared member's value is used in an arithmetic
// expression, so the call carries the declared `int` type into the expression.
BEZEL_TEST(static_class_gettype_6763, declared_value_used_in_expression_spec_11_7) {
    BEZEL_EXPECT_EQ(runAotOut(R"BZL(
module M;
public static class V
  public static method GetType(): int => 1;
end V;
public class Program
  public static method Main(): int
    var n := V.GetType() + 10;
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
)BZL"),
        std::string("11"));
}

// §11.7 (#6763): an overload pair. The zero-argument call is the declared
// member; the one-argument call is an ordinary overload and was never refused.
BEZEL_TEST(static_class_gettype_6763, zero_and_one_argument_overloads_both_call_declared_spec_11_7) {
    BEZEL_EXPECT_EQ(runAot(R"BZL(
module M;
public static class V
  public static method GetType(): int => 1;
  public static method GetType(x: int): int => x + 1;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.GetType());
    Console.WriteLine(V.GetType(5));
    return 0;
  end Main;
end Program;
)BZL").stdoutText,
        std::string("1\n6\n"));
}

// §11.7 (#6763): a call used as an expression statement (its result discarded)
// still runs the declared member; the program then prints its marker.
BEZEL_TEST(static_class_gettype_6763, discarded_call_runs_declared_member_spec_11_7) {
    BEZEL_EXPECT_EQ(runAot(R"BZL(
module M;
public static class V
  public static method GetType(): int
    Console.WriteLine("body9");
    return 9;
  end GetType;
end V;
public class Program
  public static method Main(): int
    V.GetType();
    Console.WriteLine("done9");
    return 0;
  end Main;
end Program;
)BZL").stdoutText,
        std::string("body9\ndone9\n"));
}

// §11.7 (#6763) control, passing on base as well: `Equals` declared static at a
// two-parameter list on a static class is an ordinary overload, and its call
// routing was never the defect.
BEZEL_TEST(static_class_gettype_6763, control_static_equals_pair_calls_declared_spec_11_7) {
    BEZEL_EXPECT_EQ(runAot(R"BZL(
module M;
public static class V
  public static method Equals(a: int, b: int): bool => a = b;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Equals(1, 1));
    Console.WriteLine(V.Equals(1, 2));
    return 0;
  end Main;
end Program;
)BZL").stdoutText,
        std::string("True\nFalse\n"));
}

// §11.7 (#6763) control, passing on base as well: a static `GetHashCode` and a
// static `ToString` at a parameter list other than Object's own are admitted and
// routed to the declared member. (The own-signature spelling is blocked at
// declaration by §12.2 and is tracked separately; it has no row here.)
BEZEL_TEST(static_class_gettype_6763, control_static_gethashcode_and_tostring_at_other_lists_spec_11_7) {
    BEZEL_EXPECT_EQ(runAot(R"BZL(
module M;
public static class V
  public static method GetHashCode(x: int): int => x + 14;
  public static method ToString(v: int): string => "t12";
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.GetHashCode(3));
    Console.WriteLine(V.ToString(5));
    return 0;
  end Main;
end Program;
)BZL").stdoutText,
        std::string("17\nt12\n"));
}

// §8.2 (#6763) control: `GetType` stays universal on a class with instances. A
// `this.GetType()` there is still typed by sema as the universal `Type`, and the
// reflection refusal is unchanged; the program is rejected and does not run.
BEZEL_TEST(static_class_gettype_6763, control_nonstatic_this_gettype_stays_universal_spec_8_2) {
    auto r = runAot(R"BZL(
module M;
public class C
  public method Go(): int
    var t := this.GetType();
    return 0;
  end Go;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "reflection is not yet implemented: 'GetType()' (§8.2, #487)"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.2 (#6763) control: an instance `GetType()` on a value of a class with
// instances stays universal, rejected by sema, when called from a static method.
BEZEL_TEST(static_class_gettype_6763, control_nonstatic_qualified_gettype_stays_universal_spec_8_2) {
    auto r = runAot(R"BZL(
module M;
public class C
  public method Go(): int => 0;
end C;
public class Program
  public static method Main(): int
    var c := new C();
    var t := c.GetType();
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "reflection is not yet implemented: 'GetType()' (§8.2, #487)"));
    BEZEL_EXPECT_FALSE(r.ran);
}
