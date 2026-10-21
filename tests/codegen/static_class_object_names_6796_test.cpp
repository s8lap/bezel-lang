// #6796 (§11.7 Static classes, §12.2 Hiding is not allowed, v0.340; ruling #6305 A).
// §11.7: "Having no instances, a static class carries none of `Object`'s instance
// surface, so §12.2's rule for `Object`'s three names does not reach it: its static
// members may take any name of that surface, `Equals`, `GetHashCode`, `ToString`
// and `GetType` included" (§11.7 continues with a stdlib cross-reference to
// `Convert.ToString`). Base rejected a static class's own static
// `ToString(): string`, `GetHashCode(): int` and `Equals(other: Object?): bool` at
// the declaration, as hiding `Object` (the hiding check in
// TypeResolution_Checks.cpp, which had no static-class exemption at Object's own
// signature). The declarations are now admitted and every call reaches the declared
// member, as the exact stdout below shows.
//
// Rows: the three names at Object's own signature, bare and qualified calls, the
// other parameter lists beside them, a nested static class, a generic static class,
// a static class called from another static class and from a string argument.
// Reject controls (§12.2 still blocks a non-static class's statics and an instance
// member of a static class) are in tests/sema/static_class_object_names_6796_test.cpp.
// Cross-module rows: tests/module/static_class_object_names_cross_module_6796_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program's stdout when it ran, or a sentinel that no expectation matches.
std::string stdoutOrSentinel(const JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// §11.7 (#6796): the issue's repro. A qualified call of a static class's own static
// `ToString(): string` prints the declared value.
BEZEL_TEST(static_class_object_names_6796,
           static_tostring_own_signature_runs_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method ToString(): string => "s01";
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("s01\n"));
}

// §11.7 (#6796): a static `GetHashCode(): int` on a static class is the declared member.
BEZEL_TEST(static_class_object_names_6796,
           static_gethashcode_own_signature_runs_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method GetHashCode(): int => 2;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.GetHashCode());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("2\n"));
}

// §11.7 (#6796): a static `Equals(other: Object?): bool` on a static class is the
// declared member; the argument is boxed to the `Object?` parameter.
BEZEL_TEST(static_class_object_names_6796,
           static_equals_object_nullable_own_signature_runs_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method Equals(other: Object?): bool => true;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Equals(1));
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("True\n"));
}

// §11.7 (#6796): the non-null `Equals(other: Object)` spelling of the slot's
// parameter type is Object's own signature too, so a static of it is admitted.
BEZEL_TEST(static_class_object_names_6796,
           static_equals_object_nonnull_own_signature_runs_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method Equals(other: Object): bool => false;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Equals(1));
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("False\n"));
}

// §11.7 (#6796): an unqualified call inside the static class resolves to its own
// static `ToString()`, not to any inherited surface.
BEZEL_TEST(static_class_object_names_6796,
           static_tostring_called_bare_inside_static_class_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method ToString(): string => "s05";
  public static method Show(): string => ToString();
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Show());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("s05\n"));
}

// §11.7 (#6796): the same for a bare `GetHashCode()` inside the static class.
BEZEL_TEST(static_class_object_names_6796,
           static_gethashcode_called_bare_inside_static_class_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method GetHashCode(): int => 6;
  public static method Show(): int => GetHashCode();
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Show());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("6\n"));
}

// §11.7 (#6796): the own-signature `ToString()` beside the `ToString(v: int)`
// overload; each call reaches its own member.
BEZEL_TEST(static_class_object_names_6796,
           static_tostring_beside_int_overload_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method ToString(): string => "a";
  public static method ToString(v: int): string => "b";
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.ToString());
    Console.WriteLine(V.ToString(5));
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("a\nb\n"));
}

// §11.7 (#6796): the own-signature `Equals(other: Object?)` beside the two-parameter
// `Equals(a: int, b: int)` overload.
BEZEL_TEST(static_class_object_names_6796,
           static_equals_beside_equals_pair_overload_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method Equals(other: Object?): bool => true;
  public static method Equals(a: int, b: int): bool => a = b;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.Equals(1));
    Console.WriteLine(V.Equals(1, 2));
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("True\nFalse\n"));
}

// §11.7 (#6796): a static class nested in an ordinary class is a static class too;
// its own static `ToString()` is admitted.
BEZEL_TEST(static_class_object_names_6796,
           static_tostring_in_nested_static_class_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public class Outer
  public static class V
    public static method ToString(): string => "s09";
  end V;
end Outer;
public class Program
  public static method Main(): int
    Console.WriteLine(Outer.V.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("s09\n"));
}

// §11.7 (#6796): a generic static class's own static `ToString()`.
BEZEL_TEST(static_class_object_names_6796,
           static_tostring_in_generic_static_class_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class G<T>
  public static method ToString(): string => "s17";
end G;
public class Program
  public static method Main(): int
    Console.WriteLine(G<int>.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("s17\n"));
}

// §11.7 (#6796): a static `GetHashCode()` and a static `Equals(Object?)` on the same
// static class; both are declared members and both run.
BEZEL_TEST(static_class_object_names_6796,
           static_gethashcode_and_equals_both_own_signature_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method GetHashCode(): int => 10;
  public static method Equals(other: Object?): bool => true;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.GetHashCode());
    Console.WriteLine(V.Equals(1));
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("10\nTrue\n"));
}

// §11.7 (#6796): the declared static `ToString()` is the value of a call passed as an
// argument of type string.
BEZEL_TEST(static_class_object_names_6796,
           static_tostring_passed_as_string_argument_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method ToString(): string => "s21";
end V;
public class Program
  public static method Show(s: string): int
    Console.WriteLine(s);
    return 0;
  end Show;
  public static method Main(): int
    return Show(V.ToString());
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("s21\n"));
}

// §11.7 (#6796): the declared static `GetHashCode()` used in an arithmetic expression.
BEZEL_TEST(static_class_object_names_6796,
           static_gethashcode_result_used_in_expression_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method GetHashCode(): int => 6;
end V;
public class Program
  public static method Main(): int
    var h := V.GetHashCode();
    Console.WriteLine(h + 1);
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("7\n"));
}

// §11.7 (#6796): a static class calls its sibling static class's own static
// `ToString()`; the call is through the qualified name, from another static member.
BEZEL_TEST(static_class_object_names_6796,
           static_tostring_called_from_another_static_class_spec_11_7) {
    auto r = runAot(R"BZL(
module M;
public static class V
  public static method ToString(): string => "s22";
end V;
public static class W
  public static method Run(): string => V.ToString();
end W;
public class Program
  public static method Main(): int
    Console.WriteLine(W.Run());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_EQ(stdoutOrSentinel(r), std::string("s22\n"));
}
