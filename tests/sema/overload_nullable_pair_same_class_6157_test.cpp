// Spec-derived regression tests for issue #6157 -- with `G(x: string?)`
// declared BEFORE `G(x: string)` in one class, `G(s)` at `s: string` ran the
// `string?` body (and `H(1)` beside `H(int?)` / `H(int)` ran `H(int?)`).
//
// Spec lines (docs/Bezel_Language_Spec.md, v0.338):
//   §22.6 *Implicit conversions and tie-breaking*: "one candidate is **better**
//     than another when the better candidate requires *no* implicit conversion
//     (the argument's static type matches the parameter type exactly) and the
//     other requires an implicit conversion"; "two candidates that both
//     require an implicit conversion at the same position are **equally
//     ranked** there".
//   §5.1 nullable conversions: "`T` implicitly converts to `T?`" -- so `T?`
//     is the CONVERTING candidate for a `T` argument and `T` the exact one.
//   §12.1: the parameter-nullability widening (`T` -> `T?`) is what an
//     `override` may do to its BASE member's parameter.
//
// CAUSE: `collectHierarchyOverloads` drops a base member that a more-derived
// one overrides, matching parameters up to §12.1's widening. The same test ran
// between two members of ONE class, so the later-declared `G(string)` was
// dropped as "overridden" by `G(string?)` and never ranked. Declaration order
// decided the overload; every run row below prints which body ran, in the
// order that used to be wrong and in the other one.
//
// Also here: a bare `null` argument. With the pair ranked properly, `G(null)`
// would have no applicable candidate (the literal's sentinel type relates to
// no nullable parameter but `Object?`), where the accident above made it run
// `G(string?)`; `null` now reaches every nullable parameter as a conversion
// when ordinary grading finds no candidate.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kTypes =
    "class Animal end Animal; class Dog: Animal end Dog; ";

// A static class `S` with two overloads of `F` in the given order, and a
// `Main` that runs `body`. `F(first)` returns 1, `F(second)` returns 2.
std::string pairProg(const std::string& first, const std::string& second,
                     const std::string& body) {
    return "module M; " + std::string(kTypes) +
           "static class S "
           "  public static method F(x: " + first + "): int => 1; "
           "  public static method F(x: " + second + "): int => 2; "
           "  public static method MkS(): string? => \"s\"; "
           "  public static method MkI(): int? => 3; "
           "end S; "
           "class P public static method Main(): int " + body +
           "  return 0; end Main; end P;";
}

void expectRuns(const std::string& prog, const char* out) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

void expectRejects(const std::string& prog, const char* needle) {
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// -- The issue's probe (m1.bzl), verbatim. Pre-fix the right-hand (class
//    receiver) half of the G and H lines read `66` and `88`.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           issue_probe_m1_exact_overload_runs_spec_22_6) {
    expectRuns(
        "module M;\n"
        "interface IP\n"
        " method F(x: int): int;\n"
        " method F(x: long): int;\n"
        " method F(x: string): int;\n"
        " method G(x: string): int;\n"
        " method G(x: string?): int;\n"
        "end IP;\n"
        "class C: IP\n"
        " public method F(x: string): int => 3;\n"
        " public method F(x: long): int => 2;\n"
        " public method F(x: int): int => 1;\n"
        " public method G(x: string?): int => 6;\n"
        " public method G(x: string): int => 5;\n"
        "end C;\n"
        "interface IW method H(x: int): int; method H(x: int?): int; end IW;\n"
        "class W: IW\n"
        " public method H(x: int?): int => 8;\n"
        " public method H(x: int): int => 7;\n"
        "end W;\n"
        "interface IA method M(a: int, b: Animal): int; method M(a: int, b: Dog): int; end IA;\n"
        "class Animal end Animal; class Dog: Animal end Dog;\n"
        "class A: IA\n"
        " public method M(a: int, b: Dog): int => 12;\n"
        " public method M(a: int, b: Animal): int => 11;\n"
        "end A;\n"
        "class B: A end B;\n"
        "interface IJ: IA method N(x: Dog): int; method N(x: Animal): int; end IJ;\n"
        "class K: IJ\n"
        " public method M(a: int, b: Dog): int => 22;\n"
        " public method M(a: int, b: Animal): int => 21;\n"
        " public method N(x: Animal): int => 31;\n"
        " public method N(x: Dog): int => 32;\n"
        "end K;\n"
        "class P public static method Main(): int\n"
        " using var c := new C(); var p: IP := c; var l: long := 5; var s: string := \"a\"; var sn: string? := \"b\";\n"
        " Console.WriteLine(\"F \" + p.F(1).ToString() + p.F(l).ToString() + p.F(s).ToString() + \" | \" + c.F(1).ToString() + c.F(l).ToString() + c.F(s).ToString());\n"
        " Console.WriteLine(\"G \" + p.G(s).ToString() + p.G(sn).ToString() + \" | \" + c.G(s).ToString() + c.G(sn).ToString());\n"
        " using var w := new W(); var iw: IW := w; var on: int? := 3;\n"
        " Console.WriteLine(\"H \" + iw.H(1).ToString() + iw.H(on).ToString() + \" | \" + w.H(1).ToString() + w.H(on).ToString());\n"
        " using var d := new Dog(); using var an := new Animal();\n"
        " using var b := new B(); var ia: IA := b;\n"
        " Console.WriteLine(\"IA \" + ia.M(1, d).ToString() + ia.M(1, an).ToString() + \" | \" + b.M(1, d).ToString() + b.M(1, an).ToString());\n"
        " using var k := new K(); var ij: IJ := k; var ia2: IA := k;\n"
        " Console.WriteLine(\"IJ \" + ij.N(d).ToString() + ij.N(an).ToString() + ij.M(1, d).ToString() + ia2.M(1, an).ToString() + ia2.M(1, d).ToString() + \" | \" + k.N(d).ToString() + k.N(an).ToString()+ k.M(1, d).ToString() + k.M(1, an).ToString()+ k.M(1, d).ToString());\n"
        " return 0; end Main; end P;\n",
        "F 123 | 123\nG 56 | 56\nH 78 | 78\nIA 1211 | 1211\n"
        "IJ 3231222122 | 3231222122\n");
}

// -- `string?` declared first: a `string` argument is the `string`
//    overload's exact match (§22.6). Pre-fix: 1 (the `string?` body).
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           nullable_first_string_arg_runs_non_nullable_spec_22_6) {
    expectRuns(pairProg("string?", "string",
                        "var s: string := \"x\"; "
                        "Console.WriteLine(S.F(s)); "
                        "Console.WriteLine(S.F(\"lit\")); "),
               "2\n2\n");
}

// -- Same pair, a `string?` argument: only `string?` is applicable (§7).
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           nullable_first_nullable_arg_runs_nullable_spec_22_6) {
    expectRuns(pairProg("string?", "string",
                        "var s := S.MkS(); Console.WriteLine(S.F(s)); "),
               "1\n");
}

// -- The other declaration order was already right; pinned so the two orders
//    cannot drift apart again.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           non_nullable_first_both_args_spec_22_6) {
    expectRuns(pairProg("string", "string?",
                        "var s: string := \"x\"; var n := S.MkS(); "
                        "Console.WriteLine(S.F(s)); "
                        "Console.WriteLine(S.F(n)); "),
               "1\n2\n");
}

// -- Value type: `int?` first, an `int` variable and an `int` literal run
//    `F(int)` (§22.6 Example 1: "exact match"). Pre-fix: 1 and 1.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           nullable_first_int_arg_runs_non_nullable_spec_22_6) {
    expectRuns(pairProg("int?", "int",
                        "var i: int := 4; var n := S.MkI(); "
                        "Console.WriteLine(S.F(i)); "
                        "Console.WriteLine(S.F(1)); "
                        "Console.WriteLine(S.F(n)); "),
               "2\n2\n1\n");
}

// -- User class, instance receiver: `Dog?` first, a `Dog` argument.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           nullable_first_class_arg_instance_receiver_spec_22_6) {
    expectRuns(
        "module M; " + std::string(kTypes) +
        "class C "
        "  public method F(x: Dog?): int => 1; "
        "  public method F(x: Dog): int => 2; "
        "end C; "
        "class P public static method Main(): int "
        "  using var c := new C(); using var d := new Dog(); "
        "  Console.WriteLine(c.F(d)); "
        "  return 0; end Main; end P;",
        "2\n");
}

// -- A `Dog` argument at `F(Animal?)` / `F(Animal)` converts to both, so the
//    call is ambiguous in EITHER order (§22.6: "equally ranked"). Pre-fix the
//    nullable-first order compiled and ran `F(Animal?)`.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           nullable_first_derived_arg_is_ambiguous_spec_22_6) {
    const char* body = "using var d := new Dog(); Console.WriteLine(S.F(d)); ";
    expectRejects(pairProg("Animal?", "Animal", body), "ambiguous call to `F`");
    expectRejects(pairProg("Animal", "Animal?", body), "ambiguous call to `F`");
}

// -- An argument neither overload admits is the no-candidate error in either
//    order. Pre-fix the nullable-first order reported the single-candidate
//    §5.1 message against `F(string?)`.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           nullable_first_inapplicable_arg_is_no_overload_spec_22_6) {
    const char* body = "var i: int := 4; Console.WriteLine(S.F(i)); ";
    expectRejects(pairProg("string?", "string", body),
                  "no overload of `F` is applicable");
    expectRejects(pairProg("string", "string?", body),
                  "no overload of `F` is applicable");
}

// -- `null` reaches the nullable overload in both orders. The nullable-first
//    order compiled pre-fix (by the dropped-candidate accident) and must keep
//    compiling; the other order was "no overload of `F` is applicable".
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           null_literal_runs_nullable_overload_both_orders_spec_22_6) {
    expectRuns(pairProg("string?", "string", "Console.WriteLine(S.F(null)); "),
               "1\n");
    expectRuns(pairProg("string", "string?", "Console.WriteLine(S.F(null)); "),
               "2\n");
    expectRuns(pairProg("int?", "int", "Console.WriteLine(S.F(null)); "),
               "1\n");
    expectRuns(pairProg("Animal", "Animal?", "Console.WriteLine(S.F(null)); "),
               "2\n");
}

// -- `null` beside one nullable and one non-nullable parameter of unrelated
//    types: only the nullable one admits it. Pre-fix: no overload applicable.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           null_literal_single_nullable_candidate_spec_22_6) {
    expectRuns(pairProg("int?", "long", "Console.WriteLine(S.F(null)); "),
               "1\n");
    expectRuns(pairProg("long", "int?", "Console.WriteLine(S.F(null)); "),
               "2\n");
}

// -- `null` beside two NON-nullable parameters stays the no-candidate error.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           null_literal_no_nullable_candidate_still_rejected_spec_22_6) {
    expectRejects(pairProg("Animal", "string", "Console.WriteLine(S.F(null)); "),
                  "no overload of `F` is applicable");
}

// -- Constructors rank the pair too; `new Box(null)` was the no-candidate
//    error in both orders.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           constructor_pair_null_and_exact_spec_22_6) {
    expectRuns(
        "module M; " + std::string(kTypes) +
        "class Box "
        "  public Tag: int; "
        "  public constructor(x: Dog?) Tag := 1; end constructor; "
        "  public constructor(x: Dog) Tag := 2; end constructor; "
        "end Box; "
        "class P public static method Main(): int "
        "  using var d := new Dog(); "
        "  using var a := new Box(d); using var b := new Box(null); "
        "  Console.WriteLine(a.Tag); Console.WriteLine(b.Tag); "
        "  return 0; end Main; end P;",
        "2\n1\n");
}

// -- CONTROL: across the inheritance boundary the widening still means
//    OVERRIDE (§12.1), so the base declaration is not a second candidate:
//    both calls reach the override, `null` included.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           widened_override_still_shadows_base_spec_12_1) {
    expectRuns(
        "module M; " + std::string(kTypes) +
        "class B public virtual method F(x: Animal): int => 1; end B; "
        "class D: B public override method F(x: Animal?): int => 2; end D; "
        "class P public static method Main(): int "
        "  using var d := new D(); using var a := new Animal(); "
        "  Console.WriteLine(d.F(a)); Console.WriteLine(d.F(null)); "
        "  return 0; end Main; end P;",
        "2\n2\n");
}

// -- REJECT: §14.5 (v0.338): "a candidate whose inference fails is
//    inapplicable and drops out". A bare `null` binds no type parameter, so
//    `M<T>(x: T?)` is not a nullable candidate the `null` regrade may admit;
//    `M(x: string)` does not admit `null`. Must be a diagnostic, not the
//    malformed-IR failure an unbound `T` produced.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           null_arg_generic_candidate_with_unbound_t_rejected_spec_14_5) {
    expectRejects(
        "module M; "
        "static class S public static method M<T>(x: T?): int => 1; "
        "public static method M(x: string): int => 2; end S; "
        "class P public static method Main(): int "
        "Console.WriteLine(S.M(null)); return 0; end Main; end P;",
        "no overload of `M` is applicable");
}

// -- REJECT: the same through a constructed parameter, `Array<T>?`.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           null_arg_generic_array_candidate_with_unbound_t_rejected_spec_14_5) {
    expectRejects(
        "module M; "
        "static class S public static method M<T>(x: Array<T>?): int => 1; "
        "public static method M(x: int): int => 2; end S; "
        "class P public static method Main(): int "
        "Console.WriteLine(S.M(null)); return 0; end Main; end P;",
        "no overload of `M` is applicable");
}

// -- CONTROL: with `T` bound by another argument (`T = int`), `x: T?` is
//    `int?` and the `null` regrade admits it.
BEZEL_TEST(sema_overload_nullable_pair_same_class_6157,
           null_arg_generic_candidate_with_bound_t_runs_spec_14_5) {
    expectRuns(
        "module M; "
        "static class S public static method M<T>(x: T?, y: T): int => 1; "
        "public static method M(x: string, y: int): int => 2; end S; "
        "class P public static method Main(): int "
        "Console.WriteLine(S.M(null, 5)); return 0; end Main; end P;",
        "1\n");
}
