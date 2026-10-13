// fix #6109 (§7.1 "`string` and `string?` are distinct types"; §12.9 "Classes,
// records, and concrete generic types are always invariant", v0.338):
// nullability inside a generic type argument is part of the argument's
// identity. `typeArgIdentical` compared only the base type, so
// `Array<string?>` <-> `Array<string>`, `Array<Array<int>?>` <-> `Array<Array<int>>`,
// `Dictionary<string, string?>`, `Box<string?>`, `Stack<..?>`, `Queue<..?>` and
// `Array<(int) -> int?>` <-> `Array<(int) -> int>` were ACCEPTED in every
// direction, a null-into-non-null hole. The #6102 fix covered only the
// Function/Unit arms of typeArgSameBase.
//
// Rows R*: must be rejected with the §5.1 located diagnostic, both directions,
// across the argument, local-initializer, collection-literal and constructor
// argument contexts. Rows A*: accept controls that RUN with stdout, pinning
// what must NOT change: identical nullable arguments, and the covariant
// (`out T`) interface views `IEnumerable<T>` / `IReadOnlyList<T>`, whose
// top-level nullability handling is deliberately left as it was (§12.9 does not
// pin `IEnumerable<string>` -> `IEnumerable<string?>`; HELD OUT of #6109).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
// Shared declarations the rows use (one string so each row stays a one-liner).
const char* kDecls =
    "public class Box<T> public constructor() end constructor; end Box; "
    "public class Cls public constructor() end constructor; end Cls; ";
}  // namespace

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_string_nullable_elem_to_nonnull_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R0; ") + kDecls +
                    "public class K public static method P(b: Array<string>): int return 0; end P; public static method Q(a: Array<string?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_string_nonnull_elem_to_nullable_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R1; ") + kDecls +
                    "public class K public static method P(b: Array<string?>): int return 0; end P; public static method Q(a: Array<string>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_array_int_nullable_elem_to_nonnull_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R2; ") + kDecls +
                    "public class K public static method P(b: Array<Array<int>>): int return 0; end P; public static method Q(a: Array<Array<int>?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_array_int_nonnull_elem_to_nullable_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R3; ") + kDecls +
                    "public class K public static method P(b: Array<Array<int>?>): int return 0; end P; public static method Q(a: Array<Array<int>>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_cls_nullable_elem_to_nonnull_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R4; ") + kDecls +
                    "public class K public static method P(b: Array<Cls>): int return 0; end P; public static method Q(a: Array<Cls?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_cls_nonnull_elem_to_nullable_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R5; ") + kDecls +
                    "public class K public static method P(b: Array<Cls?>): int return 0; end P; public static method Q(a: Array<Cls>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_nested_array_nullable_elem_to_nonnull_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R6; ") + kDecls +
                    "public class K public static method P(b: Array<Array<Array<int>>>): int return 0; end P; public static method Q(a: Array<Array<Array<int>>?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_array_nested_array_nonnull_elem_to_nullable_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R7; ") + kDecls +
                    "public class K public static method P(b: Array<Array<Array<int>>?>): int return 0; end P; public static method Q(a: Array<Array<Array<int>>>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_assign_array_string_nullable_to_nonnull_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R8; ") + kDecls +
                    "public class K public static method Q(a: Array<string?>): int var x: Array<string> := a; return 0; end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the initializer for `x` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_assign_array_string_nonnull_to_nullable_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R9; ") + kDecls +
                    "public class K public static method Q(a: Array<string>): int var x: Array<string?> := a; return 0; end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the initializer for `x` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_literal_array_of_array_nullable_elem_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R10; ") + kDecls +
                    "public class K public static method Q(a: Array<string?>): int var xs: Array<Array<string>> := [a]; return 0; end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the collection-literal element has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_ctor_array_string_nullable_to_nonnull_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R11; ") + kDecls +
                    "public class Holder private _f: Array<string>; public constructor(f: Array<string>) _f := f; end constructor; end Holder; public class K public static method Q(a: Array<string?>): int var h := new Holder(a); return 0; end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `f` to `new Holder` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_dictionary_value_nullable_to_nonnull_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R12; ") + kDecls +
                    "public class K public static method P(b: Dictionary<string, string>): int return 0; end P; public static method Q(a: Dictionary<string, string?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_dictionary_value_nonnull_to_nullable_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R13; ") + kDecls +
                    "public class K public static method P(b: Dictionary<string, string?>): int return 0; end P; public static method Q(a: Dictionary<string, string>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_box_string_nullable_to_nonnull_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R14; ") + kDecls +
                    "public class K public static method P(b: Box<string>): int return 0; end P; public static method Q(a: Box<string?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_box_string_nonnull_to_nullable_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R15; ") + kDecls +
                    "public class K public static method P(b: Box<string?>): int return 0; end P; public static method Q(a: Box<string>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_stack_string_nullable_to_nonnull_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R16; ") + kDecls +
                    "public class K public static method P(b: Stack<string>): int return 0; end P; public static method Q(a: Stack<string?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_queue_cls_nonnull_to_nullable_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R17; ") + kDecls +
                    "public class K public static method P(b: Queue<Cls?>): int return 0; end P; public static method Q(a: Queue<Cls>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_nested_array_of_array_of_nullable_string_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R18; ") + kDecls +
                    "public class K public static method P(b: Array<Array<string>>): int return 0; end P; public static method Q(a: Array<Array<string?>>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_fn_primitive_nullable_return_to_nonnull_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R19; ") + kDecls +
                    "public class K public static method P(b: Array<(int) -> int>): int return 0; end P; public static method Q(a: Array<(int) -> int?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_fn_primitive_nullable_return_nonnull_to_nullable_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R20; ") + kDecls +
                    "public class K public static method P(b: Array<(int) -> int?>): int return 0; end P; public static method Q(a: Array<(int) -> int>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_fn_nullable_param_vs_nonnull_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R21; ") + kDecls +
                    "public class K public static method P(b: Array<(string) -> int>): int return 0; end P; public static method Q(a: Array<(string?) -> int>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           r_fn_nullable_string_return_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109R22; ") + kDecls +
                    "public class K public static method P(b: Array<() -> string>): int return 0; end P; public static method Q(a: Array<() -> string?>): int return K.P(a); end Q; end K; "
                    "public class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument `b` to call to `P` has a type that is not implicitly convertible to the target type (§5.1)"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_array_nullable_string_to_identical_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A1; ") + kDecls +
                    "public class K public static method P(a: Array<string?>): int return a.Count; end P; end K; "
                    "public class C public static method Main(): int var xs: Array<string?> := [\"a\", null]; Console.WriteLine(K.P(xs)); Console.WriteLine(xs[1] = null); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\nTrue\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_array_nonnull_string_to_identical_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A2; ") + kDecls +
                    "public class K public static method P(a: Array<string>): int return a.Count; end P; end K; "
                    "public class C public static method Main(): int var xs: Array<string> := [\"a\", \"b\", \"c\"]; Console.WriteLine(K.P(xs)); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_nested_array_nullable_to_identical_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A3; ") + kDecls +
                    "public class K public static method P(a: Array<Array<int>?>): int return a.Count; end P; end K; "
                    "public class C public static method Main(): int var xs: Array<Array<int>?> := [null]; Console.WriteLine(K.P(xs)); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_dictionary_nullable_value_to_identical_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A4; ") + kDecls +
                    "public class K public static method P(d: Dictionary<string, string?>): int return d.Count; end P; end K; "
                    "public class C public static method Main(): int var d := new Dictionary<string, string?>(); d.Add(\"k\", null); Console.WriteLine(K.P(d)); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_user_generic_nullable_to_identical_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A5; ") + kDecls +
                    "public class K public static method P(b: Box<string?>): int return 7; end P; end K; "
                    "public class C public static method Main(): int var b := new Box<string?>(); Console.WriteLine(K.P(b)); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_fn_nullable_return_array_to_identical_param_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A6; ") + kDecls +
                    "public class K public static method P(a: Array<(int) -> int?>): int return a.Count; end P; end K; "
                    "public class C public static method Main(): int var fs: Array<(int) -> int?> := []; Console.WriteLine(K.P(fs)); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_covariant_ienumerable_view_nonnull_to_nullable_unchanged_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A7; ") + kDecls +
                    ""
                    "public class C public static method Main(): int var xs: Array<string> := [\"a\", \"b\"]; var v: IEnumerable<string?> := xs; var n := 0; for var s in v do n := n + 1; end for; Console.WriteLine(n); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_covariant_ireadonlylist_view_nonnull_to_nullable_unchanged_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A8; ") + kDecls +
                    ""
                    "public class C public static method Main(): int var xs: Array<string> := [\"a\", \"b\"]; var v: IReadOnlyList<string?> := xs; Console.WriteLine(v.Count); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(sema_nullable_generic_argument_identity_6109,
           a_covariant_ienumerable_view_identical_nullable_spec_7_1_12_9_v0_338) {
    auto r = runAot(std::string("module M6109A9; ") + kDecls +
                    ""
                    "public class C public static method Main(): int var xs: Array<string?> := [\"a\", null]; var v: IEnumerable<string?> := xs; var n := 0; for var s in v do n := n + 1; end for; Console.WriteLine(n); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

