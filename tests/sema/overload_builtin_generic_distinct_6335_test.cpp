// Spec-derived regression tests for issue #6335 -- `signaturesIndistinguishable`
// (§22.6 / §11.3 clash predicate) compared parameter types with the
// DECLARATION-BLIND `typeRefEqual`, so two overloads differing only in which
// stdlib generic instantiation they name were reported as indistinguishable.
//
// Spec (docs/Bezel_Language_Spec.md, v0.338):
//   §22.6 (line 4503): "Overload resolution considers number, types, the
//     `ref`/`out` modifiers, and the number of type parameters a method
//     declares of its own -- overloads differing in any of these are
//     distinguishable."
//   §11.3 (line 1891): two methods clash only when they "do not differ in
//     number of parameters, number of type parameters of their own, parameter
//     types, or the `ref`/`out` modifier".
//   §12.9: the standard library's collections and their interfaces are
//     DISTINCT types -- `public class Set<T>: ICollection<T>` (stdlib
//     §4/§6.x) is a class over an interface, not the interface itself.
//   §2.3 / §31.4: the always-available built-in classes and interfaces have no
//     user `TypeSymbol`, so their TypeRef carries `declaration == nullptr`.
//
// ROOT CAUSE (MEASURED): `typeRefEqualImpl`'s Named arm decides a pair by
// `a->declaration != b->declaration`, which equates ANY two declaration-less
// Named refs. `Array<V>`, `IEnumerable<V>`, `IList<V>`, `ICollection<V>`,
// `Set<V>`, `Object`, `Exception`, `Task` and every CLASS type parameter all
// carry a null declaration, so the blind compare called them one type and the
// pair clashed. `typeRefEqualSameScope` fixes the same blindness on the
// OVERRIDE path (#6315); `overloadSigTypeRefEqual` is the declaration-side
// half, the twin of #6292's call-side half.
//
// TWO GUARDS keep the name compare from over-reaching, and each has a REJECT
// row below:
//   * §11.3 "Generic methods": "Two methods declaring the same number of type
//     parameters of their own compare their parameter types with those type
//     parameters corresponding by position, not by name ... So `M<T>(x: T)`
//     and `M<U>(x: U)` clash ... Neither type-parameter names nor `where`
//     constraints distinguish members."
//   * §27.8: a type alias is transparent for identity, so `Id<V>` and a bare
//     `V` are ONE type and must still clash.
//
// FAIL-WITHOUT: every ACCEPT row below is rejected as "indistinguishable" on
// the pre-fix compiler, and every REJECT row passes on it too (they pin the
// behaviour the fix must NOT change).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// A class declaring the two overloads; `body` is Main's text. The class is
// generic so the class-type-parameter rows have a scope to read.
std::string genericClassProg(const std::string& a, const std::string& b,
                             const std::string& body) {
    return "module M; "
           "type Id<T> := T; "
           "type Id2<T> := Id<T>; "
           "class C<V, X, Y, W> "
           "  private static method Take(x: " + a + "): int => 1; "
           "  private static method Take(x: " + b + "): int => 2; "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

// Compile under the checked (ASan) oracle and expect no diagnostic.
void expectAccepted(const std::string& prog) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Compile, run under the checked oracle, and expect `out` on stdout.
void expectRuns(const std::string& prog, const char* out) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

// REJECT: the pair must still be reported as an indistinguishable overload.
void expectIndistinguishable(const std::string& prog) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is indistinguishable from another overload"));
}

}  // namespace

// -- The issue's first named pair, RESOLVED as well as accepted: the
// `Array<int>` argument picks the `Array<int>` overload and the run prints
// the value the overload returns. (Reads the data the fix handles, so a fix
// that merely dropped the diagnostic without teaching resolution the two
// instantiations apart would still fail this row.)
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           array_vs_ienumerable_overloads_resolve_spec_22_6) {
    expectRuns(
        "module M; static class C "
        "  private static method Take(x: Array<int>): int => 1; "
        "  private static method Take(x: IEnumerable<int>): int => 2; "
        "  public static method Main(): int "
        "    var a: Array<int> := [7, 8]; "
        "    Console.WriteLine(Take(a)); "
        "    Console.WriteLine(Take([9, 10])); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "1\n1\n");
}

// -- The issue's second named pair.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           array_vs_ilist_overloads_accepted_spec_22_6) {
    expectAccepted(
        "module M; static class C "
        "  private static method Take(x: Array<int>): int => 1; "
        "  private static method Take(x: IList<int>): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
}

// -- The issue's third named pair: `Set<T>` is a CLASS over `ICollection<T>`
// (§12.9 / stdlib §4), so the two are distinct types.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           set_vs_icollection_overloads_accepted_spec_22_6) {
    expectAccepted(
        "module M; static class C "
        "  private static method Take(x: Set<int>): int => 1; "
        "  private static method Take(x: ICollection<int>): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
}

// -- A collection against each of the interfaces it is separately related to.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           array_vs_each_stdlib_interface_accepted_spec_12_9) {
    expectAccepted(genericClassProg("Array<V>", "IEnumerable<V>", "return 0;"));
    expectAccepted(genericClassProg("Array<V>", "ICollection<V>", "return 0;"));
    expectAccepted(genericClassProg("Array<V>", "IReadOnlyList<V>", "return 0;"));
    expectAccepted(genericClassProg("IList<V>", "ICollection<V>", "return 0;"));
    expectAccepted(genericClassProg("IEnumerable<V>", "IList<V>", "return 0;"));
    expectAccepted(
        genericClassProg("IEnumerable<V>", "IReadOnlyList<V>", "return 0;"));
}

// -- Distinct at the TYPE ARGUMENT too, not just at the head: the case
// #5862's test header names as uncovered ("a built-in exception nested in a
// generic argument (`Array<Exception>` beside `Array<ArgumentException>`)").
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           generic_argument_pair_distinct_spec_12_9) {
    expectAccepted(
        "module M; static class C "
        "  private static method Take(x: Array<Exception>): int => 1; "
        "  private static method Take(x: Array<ArgumentException>): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
}

// -- Nested construction: `Array<IEnumerable<V>>` against `IEnumerable<Array<V>>`
// differs at BOTH the head and the argument.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           nested_generic_argument_pair_distinct_spec_12_9) {
    expectAccepted(genericClassProg("Array<IEnumerable<V>>",
                                    "IEnumerable<Array<V>>", "return 0;"));
}

// -- A CLASS type parameter pair. §11.3's positional clause speaks of "type
// parameters of their OWN", so it does not cover the class's `V` against `X`
// -- those are two different types and the overloads are legal.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           class_type_parameter_pair_distinct_spec_11_3) {
    expectAccepted(genericClassProg("V", "X", "return 0;"));
    expectAccepted(genericClassProg("Array<V>", "Array<X>", "return 0;"));
}

// -- A class type parameter against a method's OWN type parameter: different
// declaration lists, so different types (the two positional spaces are not
// merged).
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           class_type_parameter_vs_method_type_parameter_distinct_spec_11_3) {
    expectAccepted(
        "module M; class C<V> "
        "  private static method Take<T>(x: V): int => 1; "
        "  private static method Take<T>(x: T): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
}

// -- A function-type parameter: the head compare recurses into the signature.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           function_type_parameter_pair_distinct_spec_11_3) {
    expectAccepted(genericClassProg("(V) -> ()", "(W) -> ()", "return 0;"));
}

// -- REJECT (§11.3): two identical generic parameter types are ONE signature.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           identical_generic_overload_still_clashes_spec_11_3) {
    expectIndistinguishable(genericClassProg("Array<V>", "Array<V>",
                                             "return 0;"));
    expectIndistinguishable(genericClassProg("Set<int>", "Set<int>",
                                             "return 0;"));
}

// -- REJECT (§11.3 "Generic methods"): method type parameters correspond BY
// POSITION, not by name, so the heads differing at the ARGUMENT does not make
// the pair distinguishable.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           method_type_parameters_still_clash_positionally_spec_11_3) {
    expectIndistinguishable(
        "module M; class C<V> "
        "  private static method Take<T>(x: Array<T>): int => 1; "
        "  private static method Take<U>(x: Array<U>): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    expectIndistinguishable(
        "module M; class C<V> "
        "  private static method Take<T>(x: (T) -> ()): int => 1; "
        "  private static method Take<U>(x: (U) -> ()): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
}

// -- REJECT (§11.3 "Generic methods", the rule's own worked example): a bare
// type parameter pair clashes.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           bare_method_type_parameters_still_clash_spec_11_3) {
    expectIndistinguishable(
        "module M; class C<V> "
        "  private static method Take<T>(x: T): int => 1; "
        "  private static method Take<U>(x: U): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
}

// -- REJECT (§27.8): a type alias is transparent for identity, so the aliased
// spelling and the bare class type parameter are ONE type and still clash.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           alias_spelling_still_clashes_spec_27_8) {
    expectIndistinguishable(genericClassProg("Id<V>", "V", "return 0;"));
    expectIndistinguishable(genericClassProg("Id<V>", "Id2<V>", "return 0;"));
}

// -- REJECT (§11.3): the non-generic pair the fix must leave alone.
BEZEL_TEST(sema_overload_builtin_generic_distinct_6335,
           identical_primitive_overload_still_clashes_spec_11_3) {
    expectIndistinguishable(
        "module M; static class C "
        "  private static method Take(x: int): int => 1; "
        "  private static method Take(x: int): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
}