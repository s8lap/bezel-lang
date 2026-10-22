// Regression tests for #6934 (spec v0.340): §14.1 "Type-parameter names" says
// a generic method's type parameter may not reuse the name of one in scope at
// its declaration, "but not, at a method of a local type, an enclosing
// method's or type's, which is not in scope in a local type's declaration
// (§21.10's no-reference restriction), as an enclosing type's is not in a
// nested type's (§11.6)". A generic method on a LOCAL record may therefore
// reuse an enclosing generic method's (or class's) `T`. The controls keep the
// collision where the spec requires it.
//
// ORACLE: runAotChecked (real AOT path, generated-code ASan).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_local_method_reuses_enclosing_method_T) {
    auto r = runAotChecked(
        "module M; class C "
        "public static method Go<T>(t: T): int "
        "  record P(A: int) "
        "    public method Pick<T>(x: T, y: T): T return (if A > 0 then x else y); end Pick; "
        "  end P; "
        "  Console.WriteLine(new P(3).Pick(1, 2)); "
        "  Console.WriteLine(new P(0).Pick(\"a\", \"b\")); "
        "  return 0; end Go; "
        "public static method Main(): int return Go<int>(1); end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\nb\n");
}
BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_local_method_reuses_enclosing_class_T) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public method Go(t: T): int "
        "  record P(A: int) "
        "    public method Pick<T>(x: T, y: T): T return (if A > 0 then x else y); end Pick; "
        "  end P; "
        "  Console.WriteLine(new P(3).Pick(1, 2)); "
        "  return 0; end Go; end C; "
        "class D public static method Main(): int return new C<string>().Go(\"s\"); end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}
// Controls: the collision stays where the spec requires it.
BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_ordinary_method_reusing_class_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C<T> public method M<T>(x: T): int return 0; end M; end C; "
        "class D public static method Main(): int return 0; end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "reuses the name of a type parameter of enclosing type"));
}
BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_local_method_duplicate_own_list_still_rejected) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record P(A: int) public method Pick<U, U>(x: U): U return x; end Pick; end P; "
        "  return 0; end Go; "
        "public static method Main(): int return Go<int>(1); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "duplicate type parameter name"));
}
// A local-in-local generic method: neither enclosing `Run<T>` (a method of the
// outer local type) nor `Go<T>` is in scope in `Inner`'s declaration.
BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_local_in_local_method_reuses_enclosing_T) {
    auto r = runAotChecked(
        "module M; class C "
        "public static method Go<T>(t: T): int "
        "  record Outer(A: int) "
        "    public method Run<T>(x: T): T "
        "      record Inner(B: int) "
        "        public method Pick<T>(y: T): T return y; end Pick; "
        "      end Inner; "
        "      return new Inner(1).Pick(x); "
        "    end Run; "
        "  end Outer; "
        "  Console.WriteLine(new Outer(2).Run(5)); "
        "  Console.WriteLine(new Outer(2).Run(\"q\")); "
        "  return 0; end Go; "
        "public static method Main(): int return Go<string>(\"s\"); end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\nq\n");
}
// The carve-out is for a method whose IMMEDIATE enclosing type is the local
// type. A generic type `N<T>` nested in a local record keeps §14.1's rule for
// its own methods: `Mm<T>` reuses `N`'s own `T`, which IS in scope at `Mm`.
BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_nested_generic_in_local_method_reuses_own_T_rejected) {
    auto r = runAotChecked(
        "module M; class C "
        "public static method Go<T>(t: T): int "
        "  record R(A: int) "
        "    public class N<T> "
        "      public method Mm<T>(x: T): int return 0; end Mm; "
        "    end N; "
        "  end R; "
        "  return 0; end Go; "
        "public static method Main(): int return Go<string>(\"s\"); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "reuses the name of a type parameter"));
}
BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_nested_generic_in_local_in_generic_class_reuses_own_T_rejected) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public method Go(t: T): int "
        "  record R(A: int) "
        "    public class N<T> "
        "      public method Mm<T>(x: T): int return 0; end Mm; "
        "    end N; "
        "  end R; "
        "  return 0; end Go; end C; "
        "class D public static method Main(): int return 0; end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "reuses the name of a type parameter"));
}
// No enclosing generic at all: only `N`'s own `T` can collide (was accepted).
BEZEL_TEST(local_type_method_typeparam_name_6934, spec_14_1_nested_generic_in_local_no_enclosing_T_reuses_own_T_rejected) {
    auto r = runAotChecked(
        "module M; class C "
        "public static method Go(): int "
        "  record R(A: int) "
        "    public class N<T> "
        "      public method Mm<T>(x: T): int return 0; end Mm; "
        "    end N; "
        "  end R; "
        "  return 0; end Go; "
        "public static method Main(): int return Go(); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "reuses the name of a type parameter"));
}
