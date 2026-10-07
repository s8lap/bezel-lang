// Codegen — fix-forward of the #5449 / #5466 bare-type-parameter member-access
// guard (spec §2.7 rule 1, v0.315; §3.6 rule 2 / §14.2 for the rejection).
//
// The guard rejects member access on a receiver that is a bare type
// parameter: such a parameter's member surface is what its constraints
// establish, never the concrete substitution's (§3.6 rule 2, §14.2). #5466
// added `isInScopeTypeParam(headName)` to it so builtinClassSym-minted
// collection types (OwningArray, OwningSet, …) — whose Named TypeRef also
// carries `declaration == nullptr` — would not be mistaken for parameters.
//
// That name-in-scope test is still too weak when a type parameter's NAME
// shadows a built-in: `class Holder<Stack>` declares a parameter spelled
// `Stack`, so the name test is satisfied, while the receiver `G.Stack<int>`
// (alias-qualified, §2.7 rule 1's alias step) is the BUILT-IN. §2.2 and
// §2.7 rule 1 make an alias "an additional *qualifier*, not a rename", and
// #4698's `resolveNamed` already drops the `G.` segment and stamps the
// surviving node with `ast::Type::aliasQualifiedBuiltin` precisely so a
// bare-name source cannot capture it. These rows pin that the #5449 guard
// honours the same stamp.
//
// The PAIR is the point. `spec_2_7_alias_qualified_builtin_not_a_type_param`
// accepts and `spec_2_7_bare_receiver_that_is_the_param_rejects` rejects,
// and they differ ONLY in whether the receiver spelling was alias-qualified.
// Without both, a fix that simply deleted the guard would pass.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
// ACCEPT: the receiver is alias-qualified, so it reaches the built-in even
// though a type parameter of the same name is in scope (§2.7 rule 1 — the
// alias step is reached only after the bare-name sources miss, and #4698's
// stamp removes this name from bare-name capture).
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

const char* kNeedle = "no member `Push` on bare type parameter `Stack`";
}  // namespace

// ACCEPT (R1): `class Holder<Stack>` + alias-qualified `G.Stack<int>`.
BEZEL_TEST(alias_param_name_shadows_builtin_5449,
           spec_2_7_alias_qualified_builtin_not_a_type_param) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic as G; "
        "class Holder<Stack> "
        "  public method Make(): int "
        "    var s := new G.Stack<int>(); "
        "    s.Push(3); s.Push(4); "
        "    return s.Count; "
        "  end Make; "
        "end Holder; "
        "class C public static method Main() "
        "  var h := new Holder<string>(); "
        "  Console.WriteLine(h.Make()); "
        "end Main; end C;"), "2");
}

// REJECT: the twin of the row above — same class, same parameter name, same
// member, but the receiver is spelled BARE, so it really IS the parameter.
// §3.6 rule 2 / §14.2: nothing constrains `Stack`, so `Push` is not in its
// surface.
BEZEL_TEST(alias_param_name_shadows_builtin_5449,
           spec_2_7_bare_receiver_that_is_the_param_rejects) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; import System.Collections.Generic as G; "
        "class Holder<Stack> "
        "  public method Make(x: Stack): int "
        "    return x.Push(3); "
        "  end Make; "
        "end Holder; "
        "class C public static method Main() "
        "  var h := new Holder<string>(); "
        "  Console.WriteLine(h.Make(\"z\")); "
        "end Main; end C;",
        kNeedle));
}

// ACCEPT (method-level parameter, non-generic built-in): a generic METHOD's
// parameter named `Stopwatch` must not capture the alias-qualified
// `D.Stopwatch` either. Covers the non-generic built-in case, where a
// structural discriminator on generic arity would not separate the spellings.
BEZEL_TEST(alias_param_name_shadows_builtin_5449,
           spec_2_7_alias_qualified_non_generic_builtin_accepts) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Diagnostics as D; "
        "class C "
        "  public static method Go<Stopwatch>(x: Stopwatch) "
        "    using var s := D.Stopwatch.StartNew(); "
        "    Console.WriteLine(s.IsRunning); "
        "  end Go; "
        "  public static method Main() Go<int>(1); end Main; "
        "end C;"), "True");
}

// REJECT: the same method-level parameter, bare receiver — the parameter, so
// `Push` is off-surface. The accept/reject twin for the row above.
BEZEL_TEST(alias_param_name_shadows_builtin_5449,
           spec_2_7_method_level_bare_receiver_that_is_the_param_rejects) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; import System.Collections.Generic as G; "
        "class C "
        "  public static method Go<Stack>(x: Stack): int "
        "    return x.Push(3); "
        "  end Go; "
        "  public static method Main() Console.WriteLine(Go<int>(1)); end Main; "
        "end C;",
        kNeedle));
}

// REJECT (constraint present, member off it): a `where Stack: IStack`
// constraint makes `Peek` reachable, but `Count` is not on it, so the
// rejection still fires — the constraint walk (which the #5449 guard runs
// first) finds nothing and the guard must not be short-circuited by the
// alias-qualified accept path. The type argument is a real `IStack`
// implementor so the §14.2 constraint check at the instantiation site is
// satisfied and cannot be what rejects this.
BEZEL_TEST(alias_param_name_shadows_builtin_5449,
           spec_2_7_constrained_param_member_off_constraint_rejects) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; import System.Collections.Generic as G; "
        "interface IStack method Peek(): int; end IStack; "
        "class CImpl : IStack public method Peek(): int return 0; end Peek; end CImpl; "
        "class Holder<Stack> where Stack: IStack "
        "  public method Make(x: Stack): int "
        "    return x.Count; "
        "  end Make; "
        "end Holder; "
        "class C public static method Main() "
        "  var h := new Holder<CImpl>(); "
        "  Console.WriteLine(h.Make(new CImpl())); "
        "end Main; end C;",
        "no member `Count` on bare type parameter `Stack`"));
}

// NOT PINNED HERE — the accept-side twin of the row above (a constrained
// parameter's member that IS on the constraint, with the parameter's name
// shadowing a built-in) does not run: the program COMPILES but SEGFAULTS at
// run time, byte-identically on the pre-fix compiler (69e86601) and on this
// one, and only when the parameter is spelled like the built-in — the same
// program with the parameter named `T` returns 9. That is a codegen
// monomorphization defect downstream of this fix, in neither guard's scope,
// and it is reported rather than papered over: pinning an expected crash
// here would freeze a defect instead of surfacing it. The accept direction
// of the constraint walk is already covered by
// generic_bare_typeparam_tostring_record_2469_test.cpp's
// with_interface_constraint_member_access_works.
