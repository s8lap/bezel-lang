// Spec-derived regression test for issue #6970.
//
// Spec §19.5 (v0.340): "A bare type pattern `T` matches only non-null values."
// §19.3 (v0.340), type parameters as pattern types: "all type-pattern rules
// then apply to the substituted type, including the null rule". §3.6 rule 2: a
// generic body decides per instantiation, so `v is Object` at T = string? must
// give the answer the non-generic `var n: string? := null; n is Object` gives.
//
// A NULLABLE target (`v is string?`, `v is Object?`) is out of scope: the spec
// does not decide `null is T?` (spec-question #7032). The guard rows at the end
// pin those answers as UNCHANGED from origin/main, generic and non-generic.
//
// THE DEFECT. lowerTypeTest's string-operand arm read the `?` from the raw
// operand type only. At `v: T` with T = string? the `?` sits on the substitution
// (resolveTypeParamRef drops it from the operand), so the null test was skipped
// and a null string answered True for `is Object` and `is string`.

#include <string>

#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string out(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

// A generic carrier testing `v` against `tgt`, then `Main` printing the calls.
std::string prog(const std::string& tgt, const std::string& calls) {
    return "module M; class Foo public x: int; end Foo; "
           "class C "
           " public static method G<T>(v: T): bool return v is " + tgt +
           "; end G; "
           " public static method N<T>(v: T): bool return v is not " + tgt +
           "; end N; "
           " public static method Main(): int " + calls +
           " return 0; end Main; end C;";
}

}  // namespace

// The issue's literal repro: expected `False`.
BEZEL_TEST(codegen_is_object_generic_nullable_6970, issue_repro_19_5) {
    BEZEL_EXPECT_EQ(
        out("module M; class C "
            " public static method G<T>(v: T): bool return v is Object; end G; "
            " public static method Main(): int "
            "   Console.WriteLine(G<string?>(null)); return 0; "
            " end Main; end C;"),
        std::string("False\n"));
}

BEZEL_TEST(codegen_is_object_generic_nullable_6970, is_not_object_null_string) {
    BEZEL_EXPECT_EQ(
        out(prog("Object", "Console.WriteLine(N<string?>(null));")),
        std::string("True\n"));
}

BEZEL_TEST(codegen_is_object_generic_nullable_6970, is_string_null_string) {
    BEZEL_EXPECT_EQ(
        out(prog("string",
                 "Console.WriteLine(G<string?>(null)); "
                 "Console.WriteLine(N<string?>(null));")),
        std::string("False\nTrue\n"));
}

// Controls: non-null string substitution still answers True.
BEZEL_TEST(codegen_is_object_generic_nullable_6970, control_nonnull_string) {
    BEZEL_EXPECT_EQ(
        out(prog("Object",
                 "Console.WriteLine(G<string?>(\"hi\")); "
                 "Console.WriteLine(N<string?>(\"hi\"));")),
        std::string("True\nFalse\n"));
    BEZEL_EXPECT_EQ(
        out(prog("string", "Console.WriteLine(G<string?>(\"hi\"));")),
        std::string("True\n"));
}

// Controls that never flipped: class and Object? substitutions, null and non-null.
BEZEL_TEST(codegen_is_object_generic_nullable_6970, control_class_substitution) {
    BEZEL_EXPECT_EQ(
        out(prog("Object",
                 "Console.WriteLine(G<Foo?>(null)); "
                 "Console.WriteLine(G<Foo?>(new Foo())); "
                 "Console.WriteLine(G<Object?>(null)); "
                 "Console.WriteLine(G<Object?>(\"hi\"));")),
        std::string("False\nTrue\nFalse\nTrue\n"));
    BEZEL_EXPECT_EQ(
        out(prog("Foo",
                 "Console.WriteLine(G<Foo?>(null)); "
                 "Console.WriteLine(G<Foo?>(new Foo())); "
                 "Console.WriteLine(G<string?>(null));")),
        std::string("False\nTrue\nFalse\n"));
}

BEZEL_TEST(codegen_is_object_generic_nullable_6970, control_int_substitution) {
    BEZEL_EXPECT_EQ(
        out(prog("Object",
                 "Console.WriteLine(G<int?>(null)); "
                 "Console.WriteLine(G<int?>(5));")),
        std::string("False\nTrue\n"));
}

// Non-generic nullable operands agree with the generic answer.
BEZEL_TEST(codegen_is_object_generic_nullable_6970, nongeneric_agrees) {
    BEZEL_EXPECT_EQ(
        out("module M; class C public static method Main(): int "
            " var n: string? := null; var s: string? := \"hi\"; "
            " Console.WriteLine(n is Object); Console.WriteLine(n is string); "
            " Console.WriteLine(s is Object); Console.WriteLine(s is string); "
            " return 0; end Main; end C;"),
        std::string("False\nFalse\nTrue\nTrue\n"));
}

// GUARD rows (#7032, spec-question): a nullable target keeps the answer it had
// before #6970, generic and non-generic. These rows cannot fail without the
// fix by construction (they pin the unchanged pre-fix behaviour); they fail if
// the #6970 predicate widens to a nullable target before #7032 is ruled. The
// generic and non-generic null answers disagree today; #7032 asks which is
// right, and the ruling will flip one pair.
BEZEL_TEST(codegen_is_object_generic_nullable_6970, guard_nullable_target_generic_7032) {
    BEZEL_EXPECT_EQ(
        out(prog("string?",
                 "Console.WriteLine(G<string?>(null)); "
                 "Console.WriteLine(G<string?>(\"hi\")); "
                 "Console.WriteLine(N<string?>(null)); "
                 "Console.WriteLine(N<string?>(\"hi\"));")),
        std::string("True\nTrue\nFalse\nFalse\n"));
    BEZEL_EXPECT_EQ(
        out(prog("Object?",
                 "Console.WriteLine(G<string?>(null)); "
                 "Console.WriteLine(G<string?>(\"hi\")); "
                 "Console.WriteLine(N<string?>(null)); "
                 "Console.WriteLine(N<string?>(\"hi\"));")),
        std::string("True\nTrue\nFalse\nFalse\n"));
}

BEZEL_TEST(codegen_is_object_generic_nullable_6970, guard_nullable_target_nongeneric_7032) {
    BEZEL_EXPECT_EQ(
        out("module M; class C public static method Main(): int "
            " var n: string? := null; var s: string? := \"hi\"; "
            " Console.WriteLine(n is string?); Console.WriteLine(n is Object?); "
            " Console.WriteLine(s is string?); Console.WriteLine(s is Object?); "
            " Console.WriteLine(n is not string?); Console.WriteLine(n is not Object?); "
            " Console.WriteLine(s is not string?); Console.WriteLine(s is not Object?); "
            " return 0; end Main; end C;"),
        std::string("False\nFalse\nTrue\nTrue\nTrue\nTrue\nFalse\nFalse\n"));
}
