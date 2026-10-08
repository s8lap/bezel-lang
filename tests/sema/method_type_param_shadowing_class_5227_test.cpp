// Regression test for issue #5227 (§14.1 / §5.1, v0.315).
//
// Bug: when a method's type parameter shadows the enclosing class's type parameter
// (e.g. `class G<T> { method F<T>(v: T) }`), calling `g.F<Impl>(i)` on `G<int>`
// produced a SPURIOUS §5.1 conversion error at the call site. The hypothesis: the
// call site substituted the method's `v: T` using the receiver's class-level `T = int`
// instead of the method's own `T = Impl`.
//
// THE BUG. `substituteMemberType` rewrites a parameter type by NAME, substituting
// ALL mentions of a class type-param name using the class's argument map — but it
// cannot distinguish a class's `T` from a method's `T` that shadows it. Both become
// `int` from `G<int>`. The explicit type-argument map (`T -> Impl`, applied after)
// found nothing left to substitute. The parameter type stayed `int` and `Impl` was
// false-rejected as not convertible to `int`.
//
// THE FIX. Add an `excludeNames` parameter to `substituteMemberType` (and the
// underlying `ancestorGenericSubst` walk). When a method's type parameter shadows
// a same-named class type parameter, the method's param name is excluded from the
// class-level substitution map. The explicit type-argument map (applied after)
// substitutes it correctly (`v: T` stays as `v: T` after class-subst, then
// becomes `v: Impl` after explicit-subst).
//
// CORRECT BEHAVIOR. Per §14.1, a generic method's type parameter may NOT reuse
// the name of a type parameter in scope at the declaration (including its
// enclosing type's). The declaration error is correct. The fix removes the
// spurious CALL-SITE error that accompanied it.
//
// CONTROLS (existing #4847 tests). The non-shadowing controls verify that a
// non-shadowing generic method (`Box<T>.Id<V>`) still works correctly. The
// shadowing controls verify that a shadowing method is still rejected (codegen
// limitation #4862: codegen's name-keyed substitution maps cannot bind both T's).

#include "support/aot_support.hpp"  // runAot
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

#include <string>

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

// ════════ The #5227 regression case ════════════════════════════════════════

// Before the fix, this produced TWO errors:
//   1. (correct) "type parameter `T` reuses the name of a type parameter
//      of enclosing type `G`" — at the declaration
//   2. (spurious) "the argument `v` to call to `F` has a type that is not
//      implicitly convertible to the target type (§5.1)" — at the call site
//
// After the fix, only error (1) is produced. The call site is type-correct
// with T=Impl (from the explicit type argument).
BEZEL_TEST(method_type_param_shadowing_5227,
           explicit_type_arg_no_spurious_conversion_error_spec_14_1) {
    auto r = runAot(R"BZ(
module M;
interface IS end IS;
class Impl : IS public constructor() end; end Impl;
class G<T>
  public method F<T>(v: T): bool
    var o := v as IS; return o <> null;
  end F;
end G;
public class Program
  public static method Main(): int
    var g := new G<int>(); var i := new Impl();
    if g.F<Impl>(i) then return 7; end;
    return 0;
  end Main;
end Program;
)BZ");
    // The program is rejected by the declaration error (§14.1) — the shadowing
    // is illegal. But it is rejected ONLY at the declaration, NOT at the call
    // site with a spurious §5.1 conversion error.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "reuses the name of a type parameter of"));
    // Assert the call-site §5.1 error is ABSENT — the spurious error this bug
    // added before the fix.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "the argument `v` to call to `F` has a type that is not implicitly convertible to the target type"));
}

// ════════ Control: non-shadowing with different names still works ════════════

BEZEL_TEST(method_type_param_shadowing_5227,
           non_shadowing_different_names_still_works_control_spec_14_5) {
    // Same pattern but with different names: G<T> with method F<U>. No shadowing.
    // The explicit type arg <Impl> binds U, and U has no relation to class T.
    auto r = runAot(R"BZ(
module M;
interface IS end IS;
class Impl : IS public constructor() end; end Impl;
class G<T>
  public method F<U>(v: U): bool
    var o := v as IS; return o <> null;
  end F;
end G;
public class Program
  public static method Main(): int
    var g := new G<int>(); var i := new Impl();
    if g.F<Impl>(i) then Console.WriteLine(7); end;
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// ════════ Control: non-shadowing with inherited method still works ══════════

BEZEL_TEST(method_type_param_shadowing_5227,
           inherited_non_shadowing_method_still_works_control_spec_14_1) {
    // A method that does NOT shadow the class's T: Box<T> has method Id<V>.
    // The inherited case: call on a derived class.
    auto r = runAot(R"BZ(
module M;
interface IS end IS;
class Impl : IS public constructor() end; end Impl;
class G<T>
  public method F<U>(v: U): bool
    var o := v as IS; return o <> null;
  end F;
end G;
class Derived : G<int> end;
public class Program
  public static method Main(): int
    var d := new Derived(); var i := new Impl();
    if d.F<Impl>(i) then Console.WriteLine(7); end;
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
