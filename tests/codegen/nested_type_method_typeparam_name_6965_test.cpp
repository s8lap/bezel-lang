// Regression tests for #6965 (spec v0.340): §11.6 "Nested types of a generic
// type" says the enclosing type's type parameters are not in scope in a nested
// type's declaration, "Because the name is not in scope, a nested type, or a
// method of one, may declare a type parameter spelled like an enclosing type's
// (§14.1)". §14.1 "Type-parameter names" extends it to a type nested in a
// local type (§21.10): an enclosing method's or type's parameter is not in
// scope there either. The controls keep the collisions the spec still requires.
//
// ORACLE: runAotChecked (real AOT path, generated-code ASan).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

// C2: a method of a type nested in a generic class reuses the class's T.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_nested_method_reuses_enclosing_class_T) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm<T>(x: T): T return x; end Mm; end N; end C; "
        "class D public static method Main(): int "
        "  using var n := new C.N(); "
        "  Console.WriteLine(n.Mm(7)); "
        "  Console.WriteLine(n.Mm(\"s\")); "
        "  return 0; end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\ns\n");
}
// C5d: a method of a non-generic type nested in a local type reuses the
// enclosing generic method's T.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_14_1_method_of_type_nested_in_local_reuses_enclosing_method_T) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record R(A: int) "
        "    public class N public method Mm<T>(x: T): T return x; end Mm; end N; "
        "    public method Make(): owned N return new N(); end Make; "
        "  end R; "
        "  using var n := new R(1).Make(); "
        "  Console.WriteLine(n.Mm(5)); "
        "  Console.WriteLine(n.Mm(\"q\")); "
        "  return 0; end Go; "
        "public static method Main(): int return Go<string>(\"s\"); end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\nq\n");
}
// Same shape under a generic CLASS declaring the local type.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_14_1_method_of_type_nested_in_local_reuses_enclosing_class_T) {
    auto r = runAotChecked(
        "module M; class C<T> public method Go(t: T): int "
        "  record R(A: int) "
        "    public class N public method Mm<T>(x: T): T return x; end Mm; end N; "
        "    public method Make(): owned N return new N(); end Make; "
        "  end R; "
        "  using var n := new R(1).Make(); "
        "  Console.WriteLine(n.Mm(8)); "
        "  return 0; end Go; end C; "
        "class D public static method Main(): int return new C<string>().Go(\"s\"); end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "8\n");
}
// Controls.
// A nested type's method may not reuse the nested type's OWN parameter.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_14_1_nested_method_reusing_own_type_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C "
        "public class N<T> public method Mm<T>(x: T): T return x; end Mm; end N; end C; "
        "class D public static method Main(): int return 0; end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "reuses the name of a type parameter"));
}
// ...nor, in a type nested in a local type, the nested type's own parameter.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_14_1_method_of_generic_type_nested_in_local_reusing_own_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record R(A: int) "
        "    public class N<V> public method Mm<V>(x: V): V return x; end Mm; end N; "
        "  end R; "
        "  return 0; end Go; "
        "public static method Main(): int return Go<int>(1); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "reuses the name of a type parameter"));
}
// §11.6: a REFERENCE to the enclosing T inside a nested type is still rejected
// (the method's own `Mm<T>` shadows it only inside Mm).
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_nested_body_reference_to_enclosing_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm<T>(x: T): T return x; end Mm; "
        "  public method Bad(): int var z: T := default(T); return 1; end Bad; end N; end C; "
        "class D public static method Main(): int return 0; end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "may not use the enclosing type's type parameters"));
}
// §21.10: a reference to the enclosing method's T from a type nested in a local
// type is still rejected.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_21_10_local_nested_body_reference_to_enclosing_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record R(A: int) "
        "    public class N public method Mm<T>(x: T): T return x; end Mm; "
        "      public method Bad(): int var z: T := default(T); return 1; end Bad; end N; "
        "  end R; "
        "  return 0; end Go; "
        "public static method Main(): int return Go<int>(1); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "local type references the enclosing generic parameter"));
}
