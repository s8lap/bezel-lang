// Codegen + sema — issue #2971. A user exception subclass exposes the WHOLE
// §31.4 surface through its own static type, not just `Message`.
//
// Spec: §31.4 "Standard exception types" declares that surface verbatim —
//
//     public class Exception
//       public pure constructor();
//       public pure constructor(message: string);
//       public constructor(message: string, cause: Exception);   // snapshot chaining
//       public readonly property Message: string;
//       public readonly property StackTrace: string;             // formatted; "" when unavailable
//       public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
//     end Exception;
//
//     public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string);
//
//     public class ArgumentException: Exception
//       public pure constructor(message: string);
//       public pure constructor(message: string, paramName: string);
//       public readonly property ParamName: string?;   // null when not supplied
//     end ArgumentException;
//
// — §12.1 makes a base class's members available on a derived class, and §11.3
// governs member bodies. Nothing in §31.4 or §12 carves out three of the four.
// v0.240.
//
// THE DEFECT, and why it is a MEMBER-NAME defect and not a receiver defect
// (that was #2972's, a different bug in a different layer). Sema's inherited-
// member arm for a built-in exception base read
//
//     if (mm == "Message" && knownCompleteUserType && …)
//
// so `Message` was the ONLY §31.4 member a subclass exposed. MEASURED on
// 0dac23f, three spellings, three DIFFERENT diagnostics from three sites, one
// missing surface:
//
//   receiver-typed     `s.StackTrace`   -> surface.bzl:8:32: no member
//   (`var s: Sub1`)    `s.CauseChain`      `StackTrace` on type `Sub1`
//                      `l.ParamName`       (§11.3, §12.1)
//   `this.`-qualified  `this.StackTrace` -> the same reject (the `this` arm
//                                          sets targetType = currentType_ and
//                                          lands in the same lookup)
//   unqualified        `StackTrace`      -> bare.bzl:5:38: unresolved
//                      `CauseChain`         identifier `StackTrace`
//                      `ParamName`          (a THIRD site, ExprTypecheck_Exprs)
//
// while the VALUES were all present and correct: the same object read through a
// receiver retyped at the built-in base gave `pn=pp`, `cc=1`, `cc0=root`,
// `cc0t=Exception` on 0dac23f. So this was purely a lookup gap — the object
// layout and the stores were already right (`classStruct` mirrors the full
// 40-byte header into any class `sema::builtinExceptionBaseOf` accepts:
// Message@8, InnerException@16, CauseChain@24, ParamName@32, disposed flag at
// slot 5).
//
// THE FIX MOVES SEMA AND CODEGEN TOGETHER, deliberately. #2972's lane left an
// explicit warning on this issue: the codegen readers for StackTrace /
// CauseChain / ParamName were gated on `isBuiltinExceptionTypeRef(rtt)` — a
// receiver statically typed as a BUILT-IN — so admitting the three members in
// sema alone would have reproduced #2972's shape, where sema accepts and
// codegen degrades the access to a null/zero (a SIGSEGV at the first use, not a
// rejection). Every case below therefore asserts the program's OUTPUT BYTES;
// "it compiled" and "exit 0" are not evidence here.
//
// THE GATE IS THE PROPERTY, NOT A NAME LIST. §31.4's surface is stated once,
// in `sema::builtinExceptionSurfaceMemberOf`, as "which built-in DECLARES this
// name", and answered by walking `common::builtinExceptionParentOf` from the
// class's own built-in base to the root. That is why `ParamName` is available
// on a subclass of ArgumentOutOfRangeException (pinned below) without
// ArgumentOutOfRangeException being named anywhere, and why it is NOT available
// on a subclass of plain `Exception` (also pinned) — the walk decides, not an
// enumeration of receiver type names. The pre-existing hand-listed receiver set
// for `ParamName` ("ArgumentException" || "ArgumentNullException" ||
// "ArgumentOutOfRangeException") is exactly the shape this replaces.
//
// Mechanical fail-without: see the commit message. With `src/` reverted to
// 0dac23f and this file kept, every positive case below fails to compile
// (`no member …` / `unresolved identifier …`) and the two negative controls
// pass — they are controls precisely because they do not depend on the fix.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

// `class Ex2971: Exception` with a (message, cause) chaining constructor, so
// its §31.4 CauseChain slot is POPULATED — an all-zero read is distinguishable
// from a correct one, which an empty chain would not be.
std::string chainProg(const std::string& members, const std::string& main) {
    return "module M2971; "
           "class Ex2971: Exception "
           "  public constructor(m: string, c: Exception) base(m, c); "
           "  end constructor; " +
           members +
           "end Ex2971; "
           "class C public static method Main(): int "
           "  using var root := new Exception(\"root\"); "
           "  using var s := new Ex2971(\"wrap\", root); " +
           main +
           "  return 0; "
           "end Main; end C;";
}

bool hasErrorContaining(const bezel::test::OwnershipResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// A. The issue's own repro, receiver typed at the SUBCLASS.
//
// `StackTrace` is §31.4's `""`-when-unavailable stub, so it is asserted inside
// brackets: an empty read and a degraded/absent read print identically without
// them.
// ===========================================================================

BEZEL_TEST(codegen_exception_surface_2971,
           stacktrace_on_subclass_own_static_type_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        chainProg("", "  Console.WriteLine(\"st=[\" + s.StackTrace + \"]\"); "),
        "st=[]"));
}

// The whole point of chaining the constructor: a NON-EMPTY CauseChain read
// through the subclass's own static type. `cc=1` distinguishes the real slot
// from a null/zero, and the element's fields prove it is the real snapshot and
// not some other array.
BEZEL_TEST(codegen_exception_surface_2971,
           causechain_on_subclass_own_static_type_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        chainProg("",
                  "  Console.WriteLine(\"n=\" + s.CauseChain.Count.ToString()); "
                  "  Console.WriteLine(\"m=\" + s.CauseChain[0].Message); "
                  "  Console.WriteLine(\"t=\" + s.CauseChain[0].TypeName); "),
        "n=1\nm=root\nt=Exception"));
}

// §31.4 "empty when no cause" — the same access on a subclass built through the
// one-argument base constructor. The slot is calloc-zeroed and must read as the
// EMPTY chain, not fault.
BEZEL_TEST(codegen_exception_surface_2971,
           causechain_is_empty_without_a_cause_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2971b; "
        "class Plain2971: Exception "
        "  public constructor() base(\"mm\"); end constructor; "
        "end Plain2971; "
        "class C public static method Main(): int "
        "  using var s := new Plain2971(); "
        "  Console.WriteLine(\"n=\" + s.CauseChain.Count.ToString()); "
        "  return 0; "
        "end Main; end C;",
        "n=0"));
}

// §31.4 `ArgumentException … public readonly property ParamName: string?`.
BEZEL_TEST(codegen_exception_surface_2971,
           paramname_on_argumentexception_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2971c; "
        "class ArgSub2971: ArgumentException "
        "  public constructor() base(\"mm\", \"pp\"); end constructor; "
        "end ArgSub2971; "
        "class C public static method Main(): int "
        "  using var l := new ArgSub2971(); "
        "  Console.WriteLine(\"pn=\" + (l.ParamName ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "pn=pp"));
}

// §31.4 "null when not supplied" — the NULLABILITY, which is the one place a
// wrong type here would be unsound rather than merely inconvenient. A subclass
// that chained the ONE-argument `base(message)` leaves the slot calloc-zeroed;
// typing the access `string` instead of `string?` would hand that null to §7's
// non-nullable contract. `?? "<null>"` only type-checks on a nullable operand,
// so this case pins the type AND the value.
BEZEL_TEST(codegen_exception_surface_2971,
           paramname_is_null_when_not_supplied_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2971d; "
        "class ArgNoP2971: ArgumentException "
        "  public constructor() base(\"only-message\"); end constructor; "
        "end ArgNoP2971; "
        "class C public static method Main(): int "
        "  using var l := new ArgNoP2971(); "
        "  Console.WriteLine(\"pn=\" + (l.ParamName ?? \"<null>\")); "
        "  Console.WriteLine(\"m=\" + l.Message); "
        "  return 0; "
        "end Main; end C;",
        "pn=<null>\nm=only-message"));
}

// The HIERARCHY WALK, not a name list: `ArgumentOutOfRangeException` derives
// from `ArgumentException` (§31.4 / stdlib §2.5), so a subclass of it inherits
// `ParamName` — and no code names `ArgumentOutOfRangeException` to say so.
BEZEL_TEST(codegen_exception_surface_2971,
           paramname_through_a_two_step_builtin_chain_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2971e; "
        "class OorSub2971: ArgumentOutOfRangeException "
        "  public constructor() base(\"oor\", \"idx\"); end constructor; "
        "end OorSub2971; "
        "class C public static method Main(): int "
        "  using var l := new OorSub2971(); "
        "  Console.WriteLine(\"pn=\" + (l.ParamName ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "pn=idx"));
}

// A USER-class step in the chain (`Deep2971: Ex2971: Exception`): the walk
// crosses it, the header is inherited through it, and the values are the ones
// the object actually carries.
BEZEL_TEST(codegen_exception_surface_2971,
           surface_through_a_user_class_ancestor_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2971f; "
        "class Ex2971: Exception "
        "  public constructor(m: string, c: Exception) base(m, c); "
        "  end constructor; "
        "end Ex2971; "
        "class Deep2971: Ex2971 "
        "  public constructor(m: string, c: Exception) base(m, c); "
        "  end constructor; "
        "end Deep2971; "
        "class C public static method Main(): int "
        "  using var root := new Exception(\"root\"); "
        "  using var d := new Deep2971(\"deep\", root); "
        "  Console.WriteLine(\"m=\" + d.Message); "
        "  Console.WriteLine(\"n=\" + d.CauseChain.Count.ToString()); "
        "  Console.WriteLine(\"c0=\" + d.CauseChain[0].Message); "
        "  Console.WriteLine(\"st=[\" + d.StackTrace + \"]\"); "
        "  return 0; "
        "end Main; end C;",
        "m=deep\nn=1\nc0=root\nst=[]"));
}

// ===========================================================================
// B. The other two spellings, from INSIDE the subclass's own body. §12.1 makes
// the member available; §11.3 puts no restriction on how it is spelled.
//
// These are the spellings whose codegen the #2972 lane warned about: sema
// admitting them without widening the `isBuiltinExceptionTypeRef`-gated readers
// would give an accepted program that reads a null. They read the SAME
// populated chain the receiver-typed cases above do, so a degraded lowering
// shows up as `n=0` / an empty message, not as a compile error.
// ===========================================================================

BEZEL_TEST(codegen_exception_surface_2971,
           this_qualified_surface_inside_the_subclass_spec_11_3) {
    BEZEL_EXPECT_TRUE(prints(
        chainProg("  public method St(): string return this.StackTrace; end St; "
                  "  public method Cn(): int return this.CauseChain.Count; end Cn; "
                  "  public method C0(): string return this.CauseChain[0].Message; "
                  "  end C0; ",
                  "  Console.WriteLine(\"st=[\" + s.St() + \"]\"); "
                  "  Console.WriteLine(\"n=\" + s.Cn().ToString()); "
                  "  Console.WriteLine(\"c0=\" + s.C0()); "),
        "st=[]\nn=1\nc0=root"));
}

BEZEL_TEST(codegen_exception_surface_2971,
           unqualified_surface_inside_the_subclass_spec_11_3) {
    BEZEL_EXPECT_TRUE(prints(
        chainProg("  public method St(): string return StackTrace; end St; "
                  "  public method Cn(): int return CauseChain.Count; end Cn; "
                  "  public method C0(): string return CauseChain[0].Message; "
                  "  end C0; ",
                  "  Console.WriteLine(\"st=[\" + s.St() + \"]\"); "
                  "  Console.WriteLine(\"n=\" + s.Cn().ToString()); "
                  "  Console.WriteLine(\"c0=\" + s.C0()); "),
        "st=[]\nn=1\nc0=root"));
}

// The unqualified `ParamName` — nullable, and read from inside the body. Both
// arms of the nullability are exercised so a fix that typed it `string` shows
// up here as a compile error rather than silently.
BEZEL_TEST(codegen_exception_surface_2971,
           unqualified_paramname_inside_the_subclass_spec_11_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2971g; "
        "class ArgSub2971: ArgumentException "
        "  public constructor(m: string, p: string) base(m, p); end constructor; "
        "  public constructor(m: string) base(m); end constructor; "
        "  public method Pn(): string return ParamName ?? \"<null>\"; end Pn; "
        "  public method PnQ(): string return this.ParamName ?? \"<null>\"; "
        "  end PnQ; "
        "end ArgSub2971; "
        "class C public static method Main(): int "
        "  using var a := new ArgSub2971(\"mm\", \"pp\"); "
        "  using var b := new ArgSub2971(\"mm\"); "
        "  Console.WriteLine(\"a=\" + a.Pn() + \"/\" + a.PnQ()); "
        "  Console.WriteLine(\"b=\" + b.Pn() + \"/\" + b.PnQ()); "
        "  return 0; "
        "end Main; end C;",
        "a=pp/pp\nb=<null>/<null>"));
}

// A PROPERTY ACCESSOR body — the frame whose enclosing type is unrecoverable in
// codegen (an inlined accessor re-points `currentThis_` while `currentFn_` still
// belongs to the enclosing method), which is why the gate reads sema's
// resolution rather than re-deriving the receiver. #2972 measured that an
// enclosing-type-recovery draft left exactly this frame broken.
BEZEL_TEST(codegen_exception_surface_2971,
           surface_in_a_property_accessor_body_spec_11_3) {
    BEZEL_EXPECT_TRUE(prints(
        chainProg("  public property Depth: int => this.CauseChain.Count; "
                  "  public property Trace: string => StackTrace; ",
                  "  Console.WriteLine(\"d=\" + s.Depth.ToString()); "
                  "  Console.WriteLine(\"t=[\" + s.Trace + \"]\"); "),
        "d=1\nt=[]"));
}

// ===========================================================================
// C. NEGATIVE CONTROLS — the gate is the §31.4 declaration walk, so it must
// still say NO where §31.4 does. These two pass WITHOUT the fix as well as with
// it; that is what makes them controls rather than pins.
// ===========================================================================

// §31.4 declares `ParamName` on `ArgumentException`, NOT on `Exception`. A
// subclass of plain `Exception` must still be rejected — a fix that admitted
// the name on every header-bearing class would read the calloc-zeroed slot at
// offset 32 and hand back a null typed `string?` for a member that does not
// exist.
BEZEL_TEST(codegen_exception_surface_2971,
           paramname_still_rejected_on_a_plain_exception_subclass_spec_31_4) {
    auto r = parseAndCheckOwnership(
        "module M2971h; "
        "class Plain2971: Exception "
        "  public constructor() base(\"mm\"); end constructor; "
        "end Plain2971; "
        "class C public static method Main(): int "
        "  using var s := new Plain2971(); "
        "  Console.WriteLine(s.ParamName ?? \"x\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "no member `ParamName`"));
}

// A class with NO built-in exception ancestor keeps its own member of the same
// name. This is the guard that stops the widened codegen readers from
// intercepting an ordinary property: the access carries a member symbol and the
// receiver's declaration has no exception base, so both new arms decline it.
BEZEL_TEST(codegen_exception_surface_2971,
           own_surface_named_member_on_a_non_exception_class_spec_11_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2971i; "
        "class NotEx2971 "
        "  public property StackTrace: string => \"own-trace\"; "
        "  public property ParamName: string => \"own-param\"; "
        "  public method Read(): string return this.StackTrace; end Read; "
        "end NotEx2971; "
        "class C public static method Main(): int "
        "  using var n := new NotEx2971(); "
        "  Console.WriteLine(\"a=\" + n.StackTrace); "
        "  Console.WriteLine(\"b=\" + n.ParamName); "
        "  Console.WriteLine(\"c=\" + n.Read()); "
        "  return 0; "
        "end Main; end C;",
        "a=own-trace\nb=own-param\nc=own-trace"));
}
