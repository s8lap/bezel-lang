// Spec-derived regression tests for issue #6330 -- an override was rejected
// against a base member whose type was spelled through a GENERIC type alias
// applied to the base's type parameter (`R(): Id<X>` on `B<X>`, overridden as
// `int` on `E: B<int>`).
//
// Spec (docs/Bezel_Language_Spec.md, v0.338):
//   §27.8, *Type aliases*: "Type aliases -- generic or not -- see through to
//     the aliased type for identity, equality, method dispatch, and overload
//     resolution", and "Each instantiation of the alias is interchangeable with
//     the corresponding instantiation of the aliased type." So `Id<int>` IS
//     `int`; the base member's return type, once the derived class closes `X`
//     with `int`, IS `int`.
//   §12.1, *Members are non-virtual by default*: "To override a virtual member
//     in a derived class, mark the matching member `override`. Visibility,
//     return type, parameter types, and parameter modifiers must match the base
//     member exactly" -- after the base class's type parameters are substituted
//     by the derived class's arguments, which is what `Id<X>` with `X := int`
//     has to be read at.
//   §31.4 / §2.3: the always-available built-in classes are real types, so an
//     alias over one does NOT see through to an arbitrary substitution.
//
// ROOT CAUSE (MEASURED by instrumenting the §12.1 return-type comparison with
// a one-off trace): the base's `Id<X>` has its UNDERLYING payload copied in at
// alias resolution, and the use-site argument `X` substituted for the alias's
// own parameter -- so the TypeRef's *structure* is right. What is left is the
// NAME the substitution walker keys on. `SubstArgs::paramHead` reads the
// SYNTACTIC head of the node's `astNode`, which for an alias is restored to
// the use-site alias NAME (`Id`), while the name that actually identifies the
// type parameter sits in `aliasResolvedHead` (`X`). `Resolver::applyGenericSubst`
// -- the one walker every §12.1/§11.2 type position uses -- therefore missed
// the leaf and left the base's return type as `X` unsubstituted, which then
// failed `typeRefEqual(int, X)`.
//
// Trace on the pre-fix compiler (base b28c2119):
//   [ovr] dRet kind=Primitive head='int'
//         | bRet kind=Named decl=nil genericArgs=0 head='X' arh='X'
//         | bRetSubst kind=Named head='X' arh='X' | equal=0 spelling=0
// CONTROL, no alias in the base (base `R(): X`): bRetSubst kind=Primitive
// head='int' | equal=1 -- the same walker substitutes correctly once the node
// is not an alias, which is what isolates the cause to the lookup key.
//
// FAIL-WITHOUT: every ACCEPT row below is rejected as "override of `R` does not
// match the base member's return type (§12.1)" (or "...parameter types") on the
// pre-fix compiler; every REJECT row passes on it too.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// `B<X>` declaring `R` with the given return type, `E: B<int>` overriding it
// with `int`, read through a base-typed binding so the dispatch is exercised.
std::string retProg(const std::string& baseRet, const std::string& aliases) {
    return "module M; " + aliases +
           "public class B<X> "
           "  public virtual method R(): " + baseRet + " => default(X); "
           "end B; "
           "public class E: B<int> "
           "  public override method R(): int => 42; "
           "end E; "
           "public class P "
           "  public static method Main(): int "
           "    var e := new E(); "
           "    var b: B<int> := e; "
           "    Console.WriteLine(b.R()); "
           "    return 0; "
           "  end Main; "
           "end P;";
}

void expectAccepted(const std::string& prog) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

void expectRuns(const std::string& prog, const char* out) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

void expectRejected(const std::string& prog, const char* what) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, what));
}

}  // namespace

// -- The issue's repro: a base return type spelled `Id<X>` on `B<X>`, closed
// with `int` by the derived class. §27.8 makes `Id<int>` `int`, so the
// override matches -- and DISPATCHES through the base-typed binding.
BEZEL_TEST(sema_override_alias_transparent_6330,
           alias_of_class_type_param_return_matches_spec_27_8) {
    expectRuns(retProg("Id<X>", "type Id<T> := T; "), "42\n");
}

// -- The chained spelling `Id2<X>` (the issue's `c0.bzl` repro): two alias
// layers see through the same way.
BEZEL_TEST(sema_override_alias_transparent_6330,
           chained_alias_of_class_type_param_return_matches_spec_27_8) {
    expectRuns(retProg("Id2<X>",
                       "type Id<T> := T; type Id2<T> := Id<T>; "),
               "42\n");
}

// -- The PARAMETER position of the same rule: `overrideParamTypeMatches` reads
// the same substituted base type, so the fix covers every §12.1 type position
// and not only the return type.
BEZEL_TEST(sema_override_alias_transparent_6330,
           alias_of_class_type_param_parameter_matches_spec_12_1) {
    expectRuns(
        "module M; type Id<T> := T; "
        "public class B<X> "
        "  public virtual method Take(x: Id<X>): int => 1; "
        "end B; "
        "public class E: B<int> "
        "  public override method Take(x: int): int => 42; "
        "end E; "
        "public class P "
        "  public static method Main(): int "
        "    var e := new E(); "
        "    var b: B<int> := e; "
        "    Console.WriteLine(b.Take(7)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "42\n");
}

// -- A GENERIC derived class keeps the base parameter open, and the alias sees
// through to it unchanged. (This shape is admitted on the pre-fix compiler
// too -- it is the CONTROL that isolates the defect to the CONCRETE close.)
BEZEL_TEST(sema_override_alias_transparent_6330,
           alias_against_open_class_type_param_matches_spec_12_1) {
    expectAccepted(
        "module M; type Id<T> := T; "
        "public class B<X> "
        "  public virtual method R(): Id<X> => default(X); "
        "end B; "
        "public class E<Q>: B<Q> "
        "  public override method R(): Q => default(Q); "
        "end E; "
        "public class P "
        "  public static method Main(): int "
        "    var e := new E<int>(); "
        "    var b: B<int> := e; "
        "    Console.WriteLine(b.R()); "
        "    return 0; "
        "  end Main; "
        "end P;");
}

// -- REJECT (§12.1): an alias over a BUILT-IN does NOT see through to the
// derived class's substitution. `Exception` is a real type (§31.4), so a base
// returning `type Ex := Exception` still returns `Exception`, and an override
// returning `int` does not match it.
BEZEL_TEST(sema_override_alias_transparent_6330,
           alias_of_builtin_return_still_rejected_spec_12_1) {
    expectRejected(retProg("Ex", "type Ex := Exception; "),
                   "does not match the base member's return type");
}

// -- REJECT (§12.1): the plain mismatched-return-type rejection the fix must
// not disturb.
BEZEL_TEST(sema_override_alias_transparent_6330,
           mismatched_return_type_still_rejected_spec_12_1) {
    expectRejected(
        "module M; "
        "public class B<X> "
        "  public virtual method R(): string => \"base\"; "
        "end B; "
        "public class E: B<int> "
        "  public override method R(): int => 42; "
        "end E; "
        "public class P "
        "  public static method Main(): int "
        "    var e := new E(); "
        "    var b: B<int> := e; "
        "    Console.WriteLine(b.R()); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "does not match the base member's return type");
}

// -- REJECT (§11.3 / §22.6): two ALIASES of the SAME underlying are ONE
// signature, so `M(x: Id2<int>)` beside `M(x: Id<int>)` still clashes. This is
// the declaration-side twin of the #6335 rows and must not be weakened by the
// alias-transparent lookup.
BEZEL_TEST(sema_override_alias_transparent_6330,
           two_aliases_of_one_type_still_clash_spec_11_3) {
    expectRejected(
        "module M; type Id<T> := T; type Id2<T> := Id<T>; "
        "static class C "
        "  private static method Take(x: Id2<int>): int => 1; "
        "  private static method Take(x: Id<int>): int => 2; "
        "  public static method Main(): int return 0; end Main; "
        "end C;",
        "is indistinguishable from another overload");
}