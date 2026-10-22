// Spec-derived regression test for issue #6952.
//
// Spec §19.3 (Patterns, v0.340): "Type patterns without binding: `string`, `int`,
// `Dog`. Subtypes match." Over a REFERENCE-typed scrutinee a reference type
// pattern (`string`, `Object`, a class, an interface) is the ordinary runtime
// test, so the bare pattern `Object` over `Object?` matches every non-null
// reference. §19.5 (v0.340): "A bare type pattern `T` matches only non-null
// values. A typed binding pattern `var n: T` also matches only non-null values."
// The `is` operator decides the same question (§12.7).
//
// THE DEFECT. `case o of Object` over `Object?` took the `_` arm for a non-null
// string, boxed value, array or user class instance, and `var x: Object` over a
// reference never bound: the bare name `Object` resolved to the builtin root's
// class descriptor, whose is-a walk does not reach that root for those values
// (§8.1 makes every type derive from `Object`). The fix tests for a non-null
// value, the answer `is Object` gives. Over a generic value substitution the
// answer is the per-instantiation `is Object` answer (§19.3, "Type parameters as pattern types").
//
// Rows that box a value (an int or a record handed to an `Object` parameter) use
// `runAot` (exact stdout only): a `case` over such a box leaks it, an independent
// defect (#6930). The other rows use `runAotChecked` (sanitizers armed).
//
// Negative rows pin the rejections the fix must not remove: a reference-type
// pattern over an unboxed value scrutinee is a compile error (§19.3), and so is
// `var x: Object` over a value substitution at the instantiation site (§19.3).

#include <string>

#include "support/aot_support.hpp"          // runAot, runAotChecked
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

// Plain oracle (no leak check) for the rows that box a value into an `Object`
// parameter: such a `case` leaks the box, an independent defect (#6930). The
// stdout is still compared exactly.
std::string plainOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

constexpr const char* kValueRejectNeedle =
    "value-type scrutinee can never match a reference-typed";

void expectValueScrutineeRejected(const std::string& body) {
    auto r = runAot(
        "module M; class C public static method Main(): int " + body +
        " end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kValueRejectNeedle));
}

}  // namespace

// The issue's literal repro (#6952): the expected output is `True`, `obj`, `null`.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           issue_repro_nullable_object_19_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module P; "
            "public class Program "
            "  public static method S2(o: Object?): string "
            "    return (case o of Object => \"obj\", _ => \"null\"); "
            "  end S2; "
            "  public static method Main() "
            "    var o: Object? := \"a\"; "
            "    Console.WriteLine(o is Object); "
            "    Console.WriteLine(S2(o)); "
            "    Console.WriteLine(S2(null)); "
            "  end Main; "
            "end Program;"),
        std::string("True\nobj\nnull\n"));
}

// §19.3 / §19.5: a `string` arm, then the bare `Object` arm, then `_`, over
// `Object?` holding a string, a boxed int, a record and null.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           mid_arm_after_string_arm_19_3) {
    BEZEL_EXPECT_EQ(
        plainOut(
            "module M; record Pt(X: int, Y: int); "
            "class C "
            " public static method S(o: Object?): string "
            "   return (case o of string => \"str\", Object => \"obj\", _ => \"null\"); "
            " end S; "
            " public static method Main(): int "
            "   Console.WriteLine(S(\"a\")); "
            "   Console.WriteLine(S(4)); "
            "   Console.WriteLine(S(new Pt(1, 2))); "
            "   Console.WriteLine(S(null)); "
            "   return 0; "
            " end Main; end C;"),
        std::string("str\nobj\nobj\nnull\n"));
}

// §19.3: the statement form of `case`, the bare `Object` arm over `Object?`.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           statement_form_19_3) {
    BEZEL_EXPECT_EQ(
        plainOut(
            "module M; class C "
            " public static method Q(o: Object?): int "
            "   var r: int := 0; "
            "   case o of string => r := 2, Object => r := 1, _ => r := 0 end case; "
            "   return r; "
            " end Q; "
            " public static method Main(): int "
            "   Console.WriteLine(Q(\"x\")); "
            "   Console.WriteLine(Q(7)); "
            "   Console.WriteLine(Q(null)); "
            "   return 0; "
            " end Main; end C;"),
        std::string("2\n1\n0\n"));
}

// §19.3 / §19.5: a `when` guard after the bare `Object` pattern. The guard
// decides; a null `Object?` matches neither the pattern nor `_`'s sibling arm.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           when_guard_19_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; class C "
            " public static method W(o: Object?, flag: bool): string "
            "   return (case o of Object when flag => \"obj\", _ => \"other\"); "
            " end W; "
            " public static method Main(): int "
            "   Console.WriteLine(W(\"a\", true)); "
            "   Console.WriteLine(W(\"a\", false)); "
            "   Console.WriteLine(W(null, true)); "
            "   return 0; "
            " end Main; end C;"),
        std::string("obj\nother\nother\n"));
}

// §19.5: the typed binding `var x: Object` over `Object?` binds the non-null
// value and does not bind null.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           typed_binding_nullable_object_19_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; class C "
            " public static method T(o: Object?): string "
            "   return (case o of var x: Object => \"obj\", _ => \"null\"); "
            " end T; "
            " public static method Main(): int "
            "   Console.WriteLine(T(\"a\")); "
            "   Console.WriteLine(T(null)); "
            "   return 0; "
            " end Main; end C;"),
        std::string("obj\nnull\n"));
}

// §19.3: over non-null reference scrutinees (a class instance, an `Array<int>`,
// a string) the bare `Object` pattern matches, and so does the `var x: Object`
// binding over a string. The class instance and the array are bound to locals
// first: a temporary passed directly to an `Object` parameter leaks with no
// `case` involved (#6975), and the checked
// oracle would report that leak rather than the pattern.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           non_null_reference_scrutinees_19_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; public class D end D; "
            "class C "
            " public static method S(o: Object): string "
            "   return (case o of Object => \"obj\", _ => \"null\"); "
            " end S; "
            " public static method B(o: string): string "
            "   return (case o of var x: Object => \"bind\", _ => \"null\"); "
            " end B; "
            " public static method Main(): int "
            "   var d: D := new D(); "
            "   Console.WriteLine(S(d)); "
            "   var a: Array<int> := new Array<int>(); "
            "   Console.WriteLine(S(a)); "
            "   Console.WriteLine(B(\"a\")); "
            "   return 0; "
            " end Main; end C;"),
        std::string("obj\nobj\nbind\n"));
}

// §19.3 ("Type parameters as pattern types"): `case v of Object` over `v: T`. A value
// substitution answers the `is Object` question: a non-null value matches, and a
// nullable substitution matches only its present value (`T=int?`, `T=string?`).
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           generic_value_substitution_19_3) {
    BEZEL_EXPECT_EQ(
        plainOut(
            "module M; record Pt(X: int, Y: int); "
            "class C "
            " public static method G<T>(v: T): string "
            "   return (case v of Object => \"obj\", _ => \"other\"); "
            " end G; "
            " public static method Main(): int "
            "   Console.WriteLine(G<int>(5)); "
            "   Console.WriteLine(G<int?>(null)); "
            "   Console.WriteLine(G<int?>(5)); "
            "   Console.WriteLine(G<string?>(null)); "
            "   Console.WriteLine(G<string?>(\"s\")); "
            "   Console.WriteLine(G<Pt>(new Pt(1, 2))); "
            "   return 0; "
            " end Main; end C;"),
        std::string("obj\nother\nobj\nother\nobj\nobj\n"));
}

// §19.5 (per instantiation): `var x: Object` over `v: T` at a reference
// substitution (`string`, `string?`) binds the non-null value only.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           generic_reference_binding_19_5) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; class C "
            " public static method G<T>(v: T): string "
            "   return (case v of var x: Object => \"bind\", _ => \"other\"); "
            " end G; "
            " public static method Main(): int "
            "   Console.WriteLine(G<string>(\"s\")); "
            "   Console.WriteLine(G<string?>(null)); "
            "   Console.WriteLine(G<string?>(\"s\")); "
            "   return 0; "
            " end Main; end C;"),
        std::string("bind\nother\nbind\n"));
}

// §19.3: arms are tried in order, so a bare `Object` arm placed before a `string`
// arm takes the non-null string.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           object_arm_first_wins_19_3) {
    BEZEL_EXPECT_EQ(
        checkedOut(
            "module M; class C "
            " public static method S(o: string): string "
            "   return (case o of Object => \"obj\", string => \"str\", _ => \"null\"); "
            " end S; "
            " public static method Main(): int "
            "   Console.WriteLine(S(\"a\")); "
            "   return 0; "
            " end Main; end C;"),
        std::string("obj\n"));
}

// §19.3 (negative): a bare `string` or `Object` pattern over an unboxed value
// scrutinee is a compile error, whatever the arms around it. The issue's
// expectation `"Object"` for `case 4 of string, Object` is not the spec's: the
// pattern is rejected, and that is what this row asserts.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           literal_int_object_arm_rejected_19_3) {
    expectValueScrutineeRejected(
        "return (case 4 of string => \"string\", Object => \"Object\", _ => \"other\"); ");
}

// §19.5 / §19.3 (negative): the nullable value scrutinee `int?` is not a reference,
// so the bare `Object` pattern over it is a compile error.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           nullable_int_object_pattern_rejected_19_3) {
    expectValueScrutineeRejected(
        "var n: int? := 5; "
        "return (case n of Object => 1, _ => 2); ");
}

// §19.3 / §19.5 (negative): `var x: Object` over an unboxed value is a compile
// error (a true answer would bind a reference over a value no box holds).
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           value_binding_object_rejected_19_5) {
    expectValueScrutineeRejected(
        "return (case 4 of var x: Object => 1, _ => 2); ");
}

// §19.3 (negative, per instantiation): `var x: Object` over `v: T` at T=int is
// rejected at the instantiation site: a true answer would bind a reference over a
// value no box holds.
BEZEL_TEST(codegen_object_bare_object_type_pattern_6952,
           generic_int_binding_rejected_at_instantiation_19_3) {
    auto r = runAot(
        "module M; class C "
        " public static method G<T>(v: T): string "
        "   return (case v of var x: Object => \"bind\", _ => \"other\"); "
        " end G; "
        " public static method Main(): int "
        "   Console.WriteLine(G<int>(5)); "
        "   return 0; "
        " end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "rejected at the instantiation site"));
}
