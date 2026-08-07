// Codegen + sema — issue #2983. `base.<§31.4 member>` inside a class that
// derives DIRECTLY from a built-in exception resolves and reads the inherited
// slot, instead of being rejected with a claim that the class has no class
// base.
//
// Spec: §12.3 "`base.Member`" states the whole rule, verbatim —
//
//     Inside a derived member, `base.Name(...)` calls the base class's
//     implementation, bypassing virtual dispatch. `base` is legal only inside
//     instance members of a derived class.
//
// — with no carve-out. §31.4 "Standard exception types" declares the base:
//
//     public class Exception
//       public readonly property Message: string;
//       public readonly property StackTrace: string;             // formatted; "" when unavailable
//       public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
//     end Exception;
//     public class ArgumentException: Exception
//       public readonly property ParamName: string?;   // null when not supplied
//     end ArgumentException;
//
// so `class ExB: Exception` DOES have a class base and that base DOES declare
// these members. §12.1 makes them available on the derived class. v0.240.
//
// THE DEFECT, and why it is the third taxonomy variant. `resolveMemberAccess`'s
// `base` arm resolved `base` by scanning `TypeSymbol::baseTypes` for a class
// entry. A built-in exception has no `TypeSymbol` at all, so it is never in
// that collection — the predicate is CORRECT and the collection never held the
// population, which is the variant that is invisible to anyone who only reads
// the predicate. MEASURED on 0dac23f:
//
//     bm.bzl:5:12: `base.Message` does not name a base-class member: `ExB` has
//     no class base (its base is `Object`), and `Object` has no `Message` —
//     `base.M` calls a base class's implementation (§12.3); use `this.Message`
//     or just `Message`
//
// REACHABLE-BUT-INCONSISTENT, not uniformly broken: with a USER exception
// subclass as the class base (`class ExT2: ExT`, `class ExT: Exception`),
// `baseTypes` does hold it, and the same spelling compiles and returns the
// right text. So `base.Message` was legal one level down the chain and rejected
// at the top — the clearest evidence this was a lookup gap and not a rule.
//
// And the message ASSERTED A FALSEHOOD about the user's own declaration. Its
// suggested remedy, "use `this.Message`", pointed at the spelling that was
// itself SIGSEGV'ing until #2972 landed, so a user who followed the advice
// traded a false reject for a crash. The reject's diagnostic half — that a
// `base.X` reject which is still correct to fire no longer says something
// untrue, and never reports `0:0` — is pinned in
// tests/lit/base_builtin_exception_base_2983.bzl. This file pins the VALUES.
//
// THE FIX asks `sema::builtinExceptionBaseOf` (the ONE walk, #2950) in the
// `!targetType` fallback, and `sema::builtinExceptionSurfaceMemberOf` (the ONE
// statement of §31.4's surface, #2971) for what that base declares. Codegen
// needed nothing new: it already reads a `this`/`base` receiver uniformly for
// these members (#2972 for `Message`, #2971 for the other three), because the
// receiver object IS the same `this` pointer either way and the §31.4 header
// sits at the same byte offsets. That is asserted here rather than assumed —
// every case checks the program's output bytes, since a degraded lowering
// prints an empty string or a zero and exits 0.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

}  // namespace

// ===========================================================================
// The issue's own repro. `base.Message` on a class whose base is the BUILT-IN
// `Exception` — the shape that was rejected outright.
// ===========================================================================

BEZEL_TEST(codegen_base_exception_surface_2983,
           base_message_on_a_direct_builtin_subclass_spec_12_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2983a; "
        "class ExB2983: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "  public method ViaBase(): string return base.Message; end ViaBase; "
        "end ExB2983; "
        "class C public static method Main(): int "
        "  using var l := new ExB2983(\"bb\"); "
        "  Console.WriteLine(\"base=\" + l.ViaBase()); "
        "  return 0; "
        "end Main; end C;",
        "base=bb"));
}

// The rest of §31.4's `Exception` surface through `base.`, on a CHAINED
// instance so `CauseChain` has a non-empty, content-checkable value: a
// degraded read gives 0/"" and would otherwise be indistinguishable from a
// correct empty chain.
BEZEL_TEST(codegen_base_exception_surface_2983,
           base_stacktrace_and_causechain_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2983b; "
        "class ExB2983: Exception "
        "  public constructor(m: string, c: Exception) base(m, c); "
        "  end constructor; "
        "  public method St(): string return base.StackTrace; end St; "
        "  public method Cn(): int return base.CauseChain.Count; end Cn; "
        "  public method C0(): string return base.CauseChain[0].Message; "
        "  end C0; "
        "  public method Ct(): string return base.CauseChain[0].TypeName; "
        "  end Ct; "
        "end ExB2983; "
        "class C public static method Main(): int "
        "  using var root := new Exception(\"root\"); "
        "  using var l := new ExB2983(\"wrap\", root); "
        "  Console.WriteLine(\"st=[\" + l.St() + \"]\"); "
        "  Console.WriteLine(\"n=\" + l.Cn().ToString()); "
        "  Console.WriteLine(\"m=\" + l.C0()); "
        "  Console.WriteLine(\"t=\" + l.Ct()); "
        "  return 0; "
        "end Main; end C;",
        "st=[]\nn=1\nm=root\nt=Exception"));
}

// §31.4 declares `ParamName` on `ArgumentException`, so `base.ParamName`
// resolves through a class whose base IS one — and carries §31.4's declared
// nullability (`?? "<null>"` only type-checks on a nullable operand).
BEZEL_TEST(codegen_base_exception_surface_2983,
           base_paramname_on_an_argumentexception_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2983c; "
        "class ArgB2983: ArgumentException "
        "  public constructor(m: string, p: string) base(m, p); end constructor; "
        "  public constructor(m: string) base(m); end constructor; "
        "  public method Pn(): string return base.ParamName ?? \"<null>\"; "
        "  end Pn; "
        "end ArgB2983; "
        "class C public static method Main(): int "
        "  using var a := new ArgB2983(\"mm\", \"pp\"); "
        "  using var b := new ArgB2983(\"mm\"); "
        "  Console.WriteLine(\"a=\" + a.Pn()); "
        "  Console.WriteLine(\"b=\" + b.Pn()); "
        "  return 0; "
        "end Main; end C;",
        "a=pp\nb=<null>"));
}

// THE INCONSISTENCY THE ISSUE IDENTIFIED, both halves in one program: the same
// `base.Message` spelling one level DOWN the chain (through a user exception
// base, where `baseTypes` does hold the base) already worked, and at the TOP of
// the chain did not. Both must read the slot the object actually carries, and
// the two must agree on its value.
BEZEL_TEST(codegen_base_exception_surface_2983,
           base_message_agrees_at_both_chain_depths_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2983d; "
        "class ExT2983: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        // Top of the chain — the class base is the BUILT-IN. Rejected pre-fix.
        "  public method Top(): string return base.Message; end Top; "
        "end ExT2983; "
        "class ExT2b2983: ExT2983 "
        "  public constructor(m: string) base(m); end constructor; "
        // One level down — the class base is a USER class. Worked pre-fix.
        "  public method Down(): string return base.Message; end Down; "
        "end ExT2b2983; "
        "class C public static method Main(): int "
        "  using var d := new ExT2b2983(\"zz\"); "
        "  Console.WriteLine(\"top=\" + d.Top()); "
        "  Console.WriteLine(\"down=\" + d.Down()); "
        "  return 0; "
        "end Main; end C;",
        "top=zz\ndown=zz"));
}

// CONTROL — `base.ToString()` on such a class still routes through the §8.1
// universal-member path, unchanged. The fix inserts its arm only for members
// that are NOT universal, so a `base.ToString()` that already worked keeps
// working; without this the widened arm could have swallowed it and rejected a
// legal call. Passes with and without the fix, by design.
BEZEL_TEST(codegen_base_exception_surface_2983,
           base_tostring_on_an_exception_subclass_still_works_spec_8_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2983e; "
        "class ExB2983: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "  public method Ts(): bool return base.ToString().Length > 0; end Ts; "
        "end ExB2983; "
        "class C public static method Main(): int "
        "  using var l := new ExB2983(\"bb\"); "
        "  Console.WriteLine(\"ts=\" + l.Ts().ToString()); "
        "  return 0; "
        "end Main; end C;",
        "ts=True"));
}
