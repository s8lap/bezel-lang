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
// fix #6965 review (§11.6, v0.340): the nested type is reached through the
// static factory `C<int>.Make()` — "Elsewhere a chain head reaches its static
// members, and a local declared without a type annotation holds a value of
// it". The row's first spelling, `new C.N()`, is Appendix C.1's arity error
// ("A bare enclosing segment fixes arity 0"); the reject rows below pin it.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_nested_method_reuses_enclosing_class_T) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm<T>(x: T): T return x; end Mm; end N; "
        "public static method Make(): owned N return new N(); end Make; end C; "
        "class D public static method Main(): int "
        "  using var n := C<int>.Make(); "
        "  Console.WriteLine(n.Mm(7)); "
        "  Console.WriteLine(n.Mm(\"s\")); "
        "  return 0; end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\ns\n");
}
// The same nested type named by its bare name inside C's own body (§2.7's
// enclosing-type lookup reaches it there, §11.6).
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_nested_method_reuses_enclosing_class_T_bare_name_inside_C) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm<T>(x: T): T return x; end Mm; end N; "
        "public static method Run(): int "
        "  using var n := new N(); "
        "  Console.WriteLine(n.Mm(7)); "
        "  Console.WriteLine(n.Mm(\"s\")); "
        "  return 0; end Run; end C; "
        "class D public static method Main(): int return C<int>.Run(); end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\ns\n");
}
// X03: a type nested two deep in a generic class; its method reuses the
// outer class's T. N is reached through C's factory, M2 by its bare name in
// N's body.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_doubly_nested_method_reuses_enclosing_class_T) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public class N "
        "  public class M2 public method Mm<T>(x: T): T return x; end Mm; end M2; "
        "  public method Run(): int "
        "    using var m := new M2(); "
        "    Console.WriteLine(m.Mm(7)); "
        "    Console.WriteLine(m.Mm(\"s\")); "
        "    return 0; end Run; end N; "
        "public static method Make(): owned N return new N(); end Make; end C; "
        "class D public static method Main(): int "
        "  using var n := C<int>.Make(); return n.Run(); end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\ns\n");
}
// fix #6965 review (§11.6, Appendix C.1, v0.340): "A bare enclosing segment
// fixes arity 0 (§2.2, §14.1), so `Tree.Node<int>` at a generic-only `Tree`
// is Appendix C.1's arity error ... In a type position, therefore, no name
// passes through a generic enclosing type". Before af59ce4c the rows with a
// generic `Mm<T>` were rejected only by accident (§14.1's name-reuse check)
// and the others ran; af59ce4c ran all of them.
namespace {
void expectGenericEnclosingArityError(const bezel::test::JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "generic-argument count does not match type's generic-parameter count"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "supplies none, so no type name passes through it"));
}
}  // namespace
// C2's first spelling: `new C.N()` with a `using var`.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_new_bare_generic_enclosing_segment_rejected) {
    expectGenericEnclosingArityError(runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm<T>(x: T): T return x; end Mm; end N; end C; "
        "class D public static method Main(): int "
        "  using var n := new C.N(); "
        "  Console.WriteLine(n.Mm(7)); "
        "  Console.WriteLine(n.Mm(\"s\")); "
        "  return 0; end Main; end D; "));
}
// P1: the annotated-local spelling `var n: C.N := new C.N();`.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_annotated_local_bare_generic_enclosing_segment_rejected) {
    expectGenericEnclosingArityError(runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm<T>(x: T): T return x; end Mm; end N; end C; "
        "class D public static method Main(): int "
        "  var n: C.N := new C.N(); "
        "  Console.WriteLine(n.Mm(7)); "
        "  Console.WriteLine(n.Mm(\"s\")); "
        "  return 0; end Main; end D; "));
}
// X03: `var n := new C.N();` reaching a doubly-nested type's host.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_doubly_nested_host_bare_generic_enclosing_segment_rejected) {
    expectGenericEnclosingArityError(runAotChecked(
        "module M; class C<T> "
        "public class N "
        "  public class M2 public method Mm<T>(x: T): T return x; end Mm; end M2; "
        "  public method Run(): int "
        "    var m := new M2(); "
        "    Console.WriteLine(m.Mm(7)); "
        "    Console.WriteLine(m.Mm(\"s\")); "
        "    return 0; end Run; end N; end C; "
        "class D public static method Main(): int "
        "  var n := new C.N(); return n.Run(); end Main; end D; "));
}
// q1: no generic method anywhere; base 1fc0fc18 already ran this (the arity
// check never existed).
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_new_bare_generic_enclosing_nongeneric_method_rejected) {
    expectGenericEnclosingArityError(runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm(x: int): int return x; end Mm; end N; end C; "
        "class D public static method Main(): int "
        "  using var n := new C.N(); "
        "  Console.WriteLine(n.Mm(3)); "
        "  return 0; end Main; end D; "));
}
// q2: the annotated-local form of q1.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_annotated_local_bare_generic_enclosing_nongeneric_method_rejected) {
    expectGenericEnclosingArityError(runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm(x: int): int return x; end Mm; end N; end C; "
        "class D public static method Main(): int "
        "  var n: C.N := new C.N(); "
        "  Console.WriteLine(n.Mm(3)); "
        "  return 0; end Main; end D; "));
}
// §14.1 "a nested type of a generic type is named in that type's bodies only
// by its bare name (§11.6)": `C.N` inside C's own body is the same error.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_14_1_qualified_C_N_inside_generic_C_rejected) {
    expectGenericEnclosingArityError(runAotChecked(
        "module M; class C<T> "
        "public class N public method Mm(x: int): int return x; end Mm; end N; "
        "public static method Run(): int "
        "  using var n := new C.N(); "
        "  Console.WriteLine(n.Mm(3)); "
        "  return 0; end Run; end C; "
        "class D public static method Main(): int return C<int>.Run(); end Main; end D; "));
}
// Controls: a NON-generic enclosing type is passed through freely.
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_nongeneric_enclosing_segment_still_runs) {
    auto r = runAotChecked(
        "module M; class E "
        "public class N public method Mm<T>(x: T): T return x; end Mm; end N; end E; "
        "class D public static method Main(): int "
        "  using var n: E.N := new E.N(); "
        "  Console.WriteLine(n.Mm(7)); "
        "  return 0; end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}
// ...and a non-generic type nested below the generic one is named from
// inside it (§11.6: "or reaches a non-generic type enclosing it below the
// generic one ... `Inner.Leaf` there").
BEZEL_TEST(nested_type_method_typeparam_name_6965, spec_11_6_nongeneric_inner_below_generic_named_inside_still_runs) {
    auto r = runAotChecked(
        "module M; class C<T> "
        "public class Inner public class Leaf public method Get(): int return 7; end Get; end Leaf; end Inner; "
        "public static method Run(): int "
        "  using var l: Inner.Leaf := new Inner.Leaf(); "
        "  Console.WriteLine(l.Get()); "
        "  return 0; end Run; end C; "
        "class D public static method Main(): int return C<int>.Run(); end Main; end D; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
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
