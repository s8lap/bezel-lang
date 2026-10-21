// Spec-derived regression tests for issue #6709 -- an argument whose class
// DERIVES from a user generic (`class DogBox: Box<Dog>`, `class Box<T>:
// IBox<T>`) was reported "ambiguous call to `F`: no overload is unambiguously
// better (§22.6)" at `F(IBox<Dog>)` beside `F(IBox<string>)`, though only one
// candidate is applicable.
//
// Spec (docs/Bezel_Language_Spec.md, v0.339):
//   §22.6: a candidate is applicable when each argument converts to its
//     parameter; "two candidates that both require an implicit conversion at
//     the same position are **equally ranked** there".
//   §12.9: "Type parameters without `out` or `in` are invariant."; "under
//     `out`, `I<S>` converts to `I<T>` when `S` so converts to `T`".
//   §5.1 (Reference conversions): "a type to an interface it implements or
//     conforms to" -- `DogBox` implements `IBox<Dog>`, not `IBox<string>`.
//
// ROOT CAUSE (MEASURED): `DogBox` is a non-generic declared type, so the call
// takes `resolveOverload`'s judgeable grid. There `classifyArg` ->
// `implicitlyConvertible` grades a declared argument against a declared
// generic parameter by `isSubtype(DogBox, IBox)` -- the HEADS -- so both
// candidates graded Convert and tied.
//
// FAIL-WITHOUT (MEASURED): every run row below is rejected on the pre-fix
// compiler (ambiguous, or the generic row's §5.1 reject), and so is the
// neither-applies row (ambiguous instead of unreachable). The KEPT row passes
// both ways.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Compile, run under the checked (ASan) oracle, and expect `out` on stdout.
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
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

// `variance` is "" or "out ". `Pup` derives one level further.
std::string types(const char* variance) {
    return std::string(
               "class Animal public constructor() end; end Animal; "
               "class Dog: Animal public constructor() end; end Dog; "
               "interface IBox<") + variance +
           "T> method Get(): int; end IBox; "
           "class Box<T>: IBox<T> public constructor() end; "
           "  public method Get(): int => 0; end Box; "
           "class DogBox: Box<Dog> public constructor() end; end DogBox; "
           "class Pup: DogBox public constructor() end; end Pup; ";
}

std::string prog(const char* variance, const std::string& decls,
                 const std::string& body) {
    return "module M; " + types(variance) + decls +
           "class P public static method Main(): int " + body +
           "  return 0; end Main; end P;";
}

}  // namespace

// -- The issue's two probes: covariant `IBox<out T>` at `IBox<Animal>` /
// `IBox<string>` (obox_derived), invariant `IBox<T>` at `IBox<Dog>` /
// `IBox<string>` (d2_ibox_derived_inv).
BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           issue_repro_covariant_spec_22_6_12_9) {
    expectRuns(prog("out ",
                    "static class S "
                    "  public static method F(x: IBox<Animal>): int => 1; "
                    "  public static method F(x: IBox<string>): int => 2; "
                    "end S; ",
                    "  using var a := new DogBox(); "
                    "  using var b := new Box<string>(); "
                    "  Console.WriteLine(S.F(a)); "
                    "  Console.WriteLine(S.F(b)); "),
               "1\n2\n");
}

BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           issue_repro_invariant_both_orders_spec_22_6_12_9) {
    const char* body =
        "  using var a := new DogBox(); "
        "  Console.WriteLine(S.F(a)); ";
    expectRuns(prog("",
                    "static class S "
                    "  public static method F(x: IBox<Dog>): int => 1; "
                    "  public static method F(x: IBox<string>): int => 2; "
                    "end S; ",
                    body),
               "1\n");
    expectRuns(prog("",
                    "static class S "
                    "  public static method F(x: IBox<string>): int => 2; "
                    "  public static method F(x: IBox<Dog>): int => 1; "
                    "end S; ",
                    body),
               "1\n");
}

// -- Deeper derivation (`Pup: DogBox`), covariant and invariant, on a static
// and an instance member.
BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           deeper_derivation_spec_22_6_12_9) {
    expectRuns(prog("out ",
                    "static class S "
                    "  public static method F(x: IBox<Animal>): int => 1; "
                    "  public static method F(x: IBox<string>): int => 2; "
                    "end S; ",
                    "  using var a := new Pup(); "
                    "  Console.WriteLine(S.F(a)); "),
               "1\n");
    expectRuns(prog("",
                    "class C "
                    "  public constructor() end; "
                    "  public method F(x: IBox<string>): int => 2; "
                    "  public method F(x: IBox<Dog>): int => 1; "
                    "end C; ",
                    "  using var a := new Pup(); "
                    "  using var c := new C(); "
                    "  Console.WriteLine(c.F(a)); "),
               "1\n");
}

// -- A class implementing the interface directly, an extension pair, and a
// two-argument pair where each candidate fails at a different position.
BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           direct_impl_extension_two_args_spec_22_6_12_9) {
    expectRuns(
        "module M; "
        "class Dog public constructor() end; end Dog; "
        "interface IBox<T> method Get(): int; end IBox; "
        "class DogBox: IBox<Dog> public constructor() end; "
        "  public method Get(): int => 0; end DogBox; "
        "static class S "
        "  public static method F(x: IBox<string>): int => 2; "
        "  public static method F(x: IBox<Dog>): int => 1; "
        "end S; "
        "class P public static method Main(): int "
        "  using var a := new DogBox(); "
        "  Console.WriteLine(S.F(a)); "
        "  return 0; end Main; end P;",
        "1\n");
    expectRuns(prog("",
                    "class C public constructor() end; end C; "
                    "extension C "
                    "  public method F(x: IBox<string>): int => 2; "
                    "  public method F(x: IBox<Dog>): int => 1; "
                    "end; ",
                    "  using var a := new DogBox(); "
                    "  using var c := new C(); "
                    "  Console.WriteLine(c.F(a)); "),
               "1\n");
    expectRuns(prog("",
                    "class StrBox: Box<string> public constructor() end; "
                    "end StrBox; "
                    "static class S "
                    "  public static method F(x: IBox<Dog>, y: IBox<string>): "
                    "int => 1; "
                    "  public static method F(x: IBox<string>, y: IBox<Dog>): "
                    "int => 2; "
                    "end S; ",
                    "  using var a := new DogBox(); "
                    "  using var b := new StrBox(); "
                    "  Console.WriteLine(S.F(a, b)); "
                    "  Console.WriteLine(S.F(b, a)); "),
               "1\n2\n");
}

// -- Constructors resolve through the same grid; both declaration orders
// confirm the selected constructor is the one that runs.
BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           constructor_pair_both_orders_spec_22_6_12_9) {
    const char* body =
        "  using var a := new Pup(); "
        "  using var k := new K(a); "
        "  Console.WriteLine(k.V); ";
    expectRuns(prog("",
                    "class K "
                    "  public readonly property V: int; "
                    "  public constructor(x: IBox<string>) V := 2; end; "
                    "  public constructor(x: IBox<Dog>) V := 1; end; "
                    "end K; ",
                    body),
               "1\n");
    expectRuns(prog("",
                    "class K "
                    "  public readonly property V: int; "
                    "  public constructor(x: IBox<Dog>) V := 1; end; "
                    "  public constructor(x: IBox<string>) V := 2; end; "
                    "end K; ",
                    body),
               "1\n");
}

// -- A generic candidate binds `T := Dog`; the non-generic `IBox<string>`
// candidate is inapplicable (§14.5, §22.6). It was rejected as "not
// implicitly convertible" before the fix (the inapplicable one was picked).
BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           generic_candidate_beside_inapplicable_spec_22_6_14_5) {
    expectRuns(prog("",
                    "static class S "
                    "  public static method F<T>(x: IBox<T>): int => 3; "
                    "  public static method F(x: IBox<string>): int => 2; "
                    "end S; ",
                    "  using var a := new DogBox(); "
                    "  Console.WriteLine(S.F(a)); "),
               "3\n");
}

// -- KEPT (pass before and after the fix): a single-candidate call (no
// overload) runs or is rejected per §5.1, and both candidates converting is
// still §22.6-ambiguous.
BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           kept_single_candidate_and_rejects_spec_22_6_12_9) {
    expectRuns(prog("",
                    "static class S "
                    "  public static method F(x: IBox<Dog>): int => 5; "
                    "end S; ",
                    "  using var a := new DogBox(); "
                    "  Console.WriteLine(S.F(a)); "),
               "5\n");
    expectRejects(prog("",
                       "static class S "
                       "  public static method F(x: IBox<string>): int => 5; "
                       "end S; ",
                       "  using var a := new DogBox(); "
                       "  Console.WriteLine(S.F(a)); "),
                  "reaches `IBox` only at other type arguments");
    expectRejects(prog("out ",
                       "static class S "
                       "  public static method F(x: IBox<Animal>): int => 1; "
                       "  public static method F(x: IBox<Dog>): int => 2; "
                       "end S; ",
                       "  using var a := new DogBox(); "
                       "  Console.WriteLine(S.F(a)); "),
                  "ambiguous call to `F`");
}

// -- Neither candidate applies (invariant `IBox<T>` at `IBox<Animal>` /
// `IBox<string>`): §22.6's "no applicable candidate" error, where the pre-fix
// compiler said "ambiguous" (MEASURED).
BEZEL_TEST(sema_overload_derived_user_generic_iface_6709,
           neither_candidate_applies_spec_22_6_12_9) {
    expectRejects(prog("",
                       "static class S "
                       "  public static method F(x: IBox<Animal>): int => 1; "
                       "  public static method F(x: IBox<string>): int => 2; "
                       "end S; ",
                       "  using var a := new DogBox(); "
                       "  Console.WriteLine(S.F(a)); "),
                  "no overload of `F` is applicable");
}
