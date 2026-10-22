// Regression tests for #6968 (spec v0.340): §21.10 Restrictions says "a local
// type may not reference an enclosing generic parameter", and §11.6 says a
// local type's nested member types "follow their own rules". A generic type
// nested in a local type may therefore use its OWN type parameter; a
// reference to an ENCLOSING parameter from it stays rejected.
//
// ORACLE: runAotChecked (real AOT path, generated-code ASan).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

// H10: nothing generic encloses the local record.
BEZEL_TEST(local_nested_generic_own_typeparam_6968, spec_21_10_nested_generic_in_local_uses_own_T) {
    auto r = runAotChecked(
        "module M; class C public static method Go(): int "
        "  record Inner(B: int) "
        "    public class N<T> public method Get(x: T): T return x; end Get; end N; "
        "    public method Make(): owned N<string> return new N<string>(); end Make; "
        "    public method Pick<T>(y: T): T return y; end Pick; "
        "  end Inner; "
        "  var i := new Inner(1); "
        "  using var g := i.Make(); "
        "  Console.WriteLine(g.Get(\"n\")); "
        "  Console.WriteLine(i.Pick(4)); "
        "  return 0; end Go; "
        "public static method Main(): int return Go(); end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n\n4\n");
}
// C5g: the local record sits in a generic method whose T is spelled the same.
BEZEL_TEST(local_nested_generic_own_typeparam_6968, spec_21_10_nested_generic_in_local_uses_own_T_beside_enclosing_method_T) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record R(A: int) "
        "    public class N<T> public method Get(x: T): T return x; end Get; end N; "
        "    public method Make(): owned N<int> return new N<int>(); end Make; "
        "  end R; "
        "  using var g := new R(1).Make(); "
        "  Console.WriteLine(g.Get(9)); "
        "  return 0; end Go; "
        "public static method Main(): int return Go<string>(\"s\"); end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}
// Controls: an enclosing parameter is still rejected.
BEZEL_TEST(local_nested_generic_own_typeparam_6968, spec_21_10_local_field_enclosing_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C public static method Go<T>(t: T): int "
        "  record R(A: T) end R; "
        "  return 0; end Go; "
        "public static method Main(): int return Go<int>(1); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "local type references the enclosing generic parameter"));
}
// A type nested in a generic nested type of a local type does not inherit the
// middle type's parameter.
BEZEL_TEST(local_nested_generic_own_typeparam_6968, spec_21_10_doubly_nested_reference_to_middle_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C public static method Go(): int "
        "  record R(A: int) "
        "    public class N<T> public class P public method Get(): int var z: T := default(T); return 1; end Get; end P; end N; "
        "  end R; "
        "  return 0; end Go; "
        "public static method Main(): int return Go(); end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
// A class-level T reference inside a local type of a generic class is rejected.
BEZEL_TEST(local_nested_generic_own_typeparam_6968, spec_21_10_local_in_generic_class_enclosing_class_T_still_rejected) {
    auto r = runAotChecked(
        "module M; class C<T> public method Go(): int "
        "  record R(A: int) public class N<U> public method Get(x: T): U return default(U); end Get; end N; end R; "
        "  return 0; end Go; end C; "
        "class D public static method Main(): int return 0; end Main; end D; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "local type references the enclosing generic parameter"));
}
