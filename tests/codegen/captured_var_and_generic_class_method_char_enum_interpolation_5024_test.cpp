// Regression tests for #5024 — sibling of #4896: a char/enum value reached
// through a generic `T` still interpolates as its underlying integer in TWO
// shapes #4896's own fix does not cover — a CAPTURED variable (not a
// declared lambda parameter) and a generic-CLASS instance method invoked
// through an enclosing generic method's own type parameter.
//
// ISSUE REPROS (verbatim from #5024's probe comment; each `.bzl` compiled
// as its own program except `boxcls` which is one program with 3 lines):
//
//   Cap_char.bzl / Cap_color.bzl:
//     module Conf;
//     public enum Color Red, Green; end Color;
//     public class Util
//       public static method Map<W>(x: W, f: (W) -> string): string
//         return f(x);
//       end Map;
//       public static method Go<T>(v: T): string
//         return Map(v, lambda(y) => $"<{y}>");
//       end Go;
//       public static method Inner<T>(v: T): string
//         return Map(v, lambda(z) => $"[{z}]");
//       end Inner;
//       public static method Outer<T>(v: T): string
//         return Inner<T>(v);
//       end Outer;
//       public static method Expl<T>(v: T): string
//         return Map<T>(v, lambda(y) => $"({y})");
//       end Expl;
//       public static method Cap<T>(v: T): string
//         var f := lambda(q: int) => $"{v}/{q}";
//         return f(7);
//       end Cap;
//     end Util;
//     public class Program
//       public static method Main()
//         Console.WriteLine(Util.Cap<char>('x'));      // (Cap_color.bzl: Cap<Color>(Color.Green))
//       end Main;
//     end Program;
//
//   boxcls.bzl:
//     module Conf;
//     public enum Color Red, Green; end Color;
//     public class Box<T>
//       private _v: T;
//       public constructor(v: T) _v := v; end constructor;
//       public method Apply(f: (T) -> string): string
//         return f(_v);
//       end Apply;
//     end Box;
//     public class Util
//       public static method Go<U>(v: U): string
//         var b := new Box<U>(v);
//         return b.Apply(lambda(y) => $"<{y}>");
//       end Go;
//       public static method Two<T>(v: T, n: int): string
//         var b := new Box<int>(n);
//         return b.Apply(lambda(y) => $"<{y}>") + Util.Go<T>(v);
//       end Two;
//     end Util;
//     public class Program
//       public static method Main()
//         var b := new Box<char>('k');
//         Console.WriteLine(b.Apply(lambda(y) => $"<{y}>"));
//         Console.WriteLine(Util.Go<char>('m'));
//         Console.WriteLine(Util.Two<Color>(Color.Green, 65));
//       end Main;
//     end Program;
//
// Observed pre-fix (MEASURED on the #4896-applied, pre-#5024 tree):
//   Cap_char   -> "120/7\n"     (want "x/7\n")
//   Cap_color  -> "1/7\n"       (want "Conf.Color/7\n")
//   boxcls     -> "<k>\n<109>\n<65><1>\n"
//                 (want         "<k>\n<m>\n<65><Conf.Color>\n")
// per stdlib §8.5 (char.ToString() is the character itself) and stdlib
// §2.13 + §8.5 (enum ToString() renders the qualified TYPE name — #4927's
// correction).
//
// ROOT CAUSE — two distinct siblings of #4896's i32/char/enum ambiguity,
// neither reached by #4896's own fix (which only injects a type hint for a
// declared, INFERRED lambda PARAMETER):
//
// (a) CAPTURED variable (Cap_char / Cap_color): `v: T` is captured from
//     `Cap<T>`'s own frame into `lambda(q: int) => $"{v}/{q}"` — `v` is not
//     a lambda parameter at all, so #4896's `lambdaParamTypeHint_` never
//     applies. The interpolation lowering (LlvmBackend_Exprs.cpp) read `v`'s
//     unsubstituted sema type (bare `T`) and, on a LLVM i32 value, fell
//     through to the raw-integer render arm. Fixed by a new
//     `resolveBareTParamConcreteHoleTy` helper (LlvmBackend_Exprs.cpp) that
//     re-resolves a bare-`T` hole through the ACTIVE `genericSubstSemaTy_`
//     substitution, gated on the resolved candidate's OWN LLVM shape
//     actually matching the lowered value (so an unrelated, same-named
//     parameter from a different scope can't mis-route the hole).
//
// (b) Generic-CLASS instance method via an enclosing generic method
//     (boxcls, `Util.Go<char>('m')` and, nested, `Util.Two<Color>`'s call to
//     `Util.Go<T>(v)`): `Box<U>.Apply(lambda)`'s pre-pass in
//     `lowerCallDispatch` (LlvmBackend_Calls2.cpp) only iterates the
//     CALLEE's OWN `genericParams` (`Apply` has none — its `T` belongs to
//     the DECLARING CLASS `Box<T>`), so the whole pre-pass was skipped and
//     `T` was never bound before the lambda argument was lowered. Fixed by a
//     new, purely-additive block in the same function that binds the
//     callee's OWNER (declaring class) type-parameter names from the
//     receiver's own class instantiation (`Box<U>` -> resolve `U` through
//     the caller's active substitution) before `buildOrderedArgs` runs.
//     Additive only: a name already bound (by the method-generic-param
//     pre-pass, or by an outer scope of the SAME name) is left untouched,
//     and a still-bare resolution is skipped.
//
// Also fixed: a stale comment in LlvmBackend_Exprs2.cpp's hint-injection
// site, which attributed the non-i32 pre-pass substitution to "pre-#4896"
// when it has been in place since #4813 (bb41c32f) — flagged verbatim in
// #5024's body.
//
// BONUS (same fix, #4953's own body note): #4953 ("$"{_v}" of a T field at
// T := int? renders an empty string") ALSO flagged, as an aside, "a related
// rendering oddity: at T = enum and T = char, $"{_v}" renders the
// underlying integer... on direct calls too" — a DIRECT field access with
// NO lambda involved at all, inside a generic class's own method body.
// #4953's own landed fix (commit 7e152849) addressed only the nullable-
// carrier (`T := int?`) case the issue's title names; the char/enum aside
// was left open. `resolveBareTParamConcreteHoleTy` (part (a) above) is a
// general interpolation-lowering fix, not lambda-specific, so it ALSO
// resolves this direct-field-access shape as a side effect — covered below
// by `direct_field_access_char_enum_4953_aside_no_lambda`.
//
// SCOPE NOTE: #5024's probe explicitly separates this from #5007 (the
// same-lambda-AST-monomorphized-twice cross-specialization leak #4896's own
// test file documents avoiding) — this file's `boxcls` case DOES compile
// `Go<U>` at two specializations (char, then Color via the nested
// `Two<Color>` -> `Go<T>` call) in one program, but #5007's corruption is
// specific to specializations whose LLVM representations DIFFER in kind
// (e.g. `int` vs a boxed `string` pointer); char and every enum share the
// SAME i32 representation, so this combination is not the shape #5007
// tracks and is safe to assert exact stdout for.
//
// spec: Bezel_Language_Spec.md §14.1 (monomorphization), §14.5 phase 2
// (lambda parameter types come from the bindings established so far),
// §18.5 (interpolation dispatches ToString() via the receiver's static
// type); Bezel_Standard_Library.md §8.5 (primitive ToString), §2.13 (enum
// ToString renders the qualified name, corrected per #4927). v0.307.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

// #5024 part (a) / §14.1, §14.5 phase 2, §18.5, stdlib §8.5 (v0.307): a
// captured `T`-typed variable, specialized at `char`, interpolated from
// inside a lambda that itself has an ordinary (non-generic) declared
// parameter. Pre-fix stdout: "120/7\n". Post-fix: "x/7\n".
BEZEL_TEST(captured_var_and_generic_class_method_char_enum_interpolation_5024,
           captured_var_char_specialization_prints_char) {
    auto r = runAot(
        "module Conf; "
        "public enum Color Red, Green; end Color; "
        "public class Util "
        "  public static method Map<W>(x: W, f: (W) -> string): string "
        "    return f(x); "
        "  end Map; "
        "  public static method Go<T>(v: T): string "
        "    return Map(v, lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "  public static method Inner<T>(v: T): string "
        "    return Map(v, lambda(z) => $\"[{z}]\"); "
        "  end Inner; "
        "  public static method Outer<T>(v: T): string "
        "    return Inner<T>(v); "
        "  end Outer; "
        "  public static method Expl<T>(v: T): string "
        "    return Map<T>(v, lambda(y) => $\"({y})\"); "
        "  end Expl; "
        "  public static method Cap<T>(v: T): string "
        "    var f := lambda(q: int) => $\"{v}/{q}\"; "
        "    return f(7); "
        "  end Cap; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util.Cap<char>('x')); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "x/7\n");
}

// #5024 part (a), same shape specialized at an enum. Enum ToString() renders
// the qualified TYPE name (stdlib §2.13 + §8.5, corrected per #4927).
// Pre-fix stdout: "1/7\n" (the ordinal). Post-fix: "Conf.Color/7\n".
BEZEL_TEST(captured_var_and_generic_class_method_char_enum_interpolation_5024,
           captured_var_enum_specialization_prints_qualified_name) {
    auto r = runAot(
        "module Conf; "
        "public enum Color Red, Green; end Color; "
        "public class Util "
        "  public static method Map<W>(x: W, f: (W) -> string): string "
        "    return f(x); "
        "  end Map; "
        "  public static method Go<T>(v: T): string "
        "    return Map(v, lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "  public static method Inner<T>(v: T): string "
        "    return Map(v, lambda(z) => $\"[{z}]\"); "
        "  end Inner; "
        "  public static method Outer<T>(v: T): string "
        "    return Inner<T>(v); "
        "  end Outer; "
        "  public static method Expl<T>(v: T): string "
        "    return Map<T>(v, lambda(y) => $\"({y})\"); "
        "  end Expl; "
        "  public static method Cap<T>(v: T): string "
        "    var f := lambda(q: int) => $\"{v}/{q}\"; "
        "    return f(7); "
        "  end Cap; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util.Cap<Color>(Color.Green)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "Conf.Color/7\n");
}

// #5024 regression control: the SAME captured-variable shape specialized at
// `int` must stay unaffected (int also lowers to i32, the ambiguous
// sentinel). Unchanged on both sides of the fix: "5/7\n".
BEZEL_TEST(captured_var_and_generic_class_method_char_enum_interpolation_5024,
           captured_var_int_specialization_unaffected) {
    std::string out = runAotOut(
        "module Conf; "
        "public class Util "
        "  public static method Cap<T>(v: T): string "
        "    var f := lambda(q: int) => $\"{v}/{q}\"; "
        "    return f(7); "
        "  end Cap; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Cap<int>(5)); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(out, "5/7");
}

// #5024 part (b) / §14.1, §14.5 phase 2, §18.5, stdlib §2.13 + §8.5
// (v0.307): a generic-CLASS instance method (`Box<T>.Apply`) invoked
// through an enclosing generic method's own type parameter, at char and at
// an enum — the issue's exact 3-line `boxcls.bzl` repro (line 1: a direct,
// concrete `Box<char>` receiver, already correct pre-fix, kept as an
// in-program control; line 2: `Util.Go<char>`; line 3: `Util.Two<Color>`,
// which nests a SECOND `Go<T>` specialization at Color through a plain
// method call). Pre-fix stdout: "<k>\n<109>\n<65><1>\n". Post-fix:
// "<k>\n<m>\n<65><Conf.Color>\n".
BEZEL_TEST(captured_var_and_generic_class_method_char_enum_interpolation_5024,
           generic_class_instance_method_via_lambda_char_and_enum) {
    auto r = runAot(
        "module Conf; "
        "public enum Color Red, Green; end Color; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Apply(f: (T) -> string): string "
        "    return f(_v); "
        "  end Apply; "
        "end Box; "
        "public class Util "
        "  public static method Go<U>(v: U): string "
        "    var b := new Box<U>(v); "
        "    return b.Apply(lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "  public static method Two<T>(v: T, n: int): string "
        "    var b := new Box<int>(n); "
        "    return b.Apply(lambda(y) => $\"<{y}>\") + Util.Go<T>(v); "
        "  end Two; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    var b := new Box<char>('k'); "
        "    Console.WriteLine(b.Apply(lambda(y) => $\"<{y}>\")); "
        "    Console.WriteLine(Util.Go<char>('m')); "
        "    Console.WriteLine(Util.Two<Color>(Color.Green, 65)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<k>\n<m>\n<65><Conf.Color>\n");
}

// #4953's own body note (BONUS, same fix as part (a) above) / §14.1, §18.5,
// stdlib §2.13 + §8.5 (v0.307): a DIRECT field access — no lambda, no
// capture — inside a generic class's own method, at T = char and T = enum.
// #4953's landed fix (7e152849) covers only the T := int? nullable-carrier
// case its title names; this is the aside its body separately flagged as
// still broken ("on direct calls too"): a BARE `T` hole's sema type has a
// null `declaration`, so it matches neither the Primitive-char dispatch arm
// nor the Named+declaration+Enum arm before substitution — both fall
// through to the same raw-integer fallback the pre-fix issue text
// describes ("2", "113", the enum ordinal and the code point). Post-fix,
// once `resolveBareTParamConcreteHoleTy` substitutes the hole to its
// concrete type, both arms dispatch correctly: "x\nM.Color\n" (this test's
// module is `M`, not `Conf` like the other cases above — the qualified enum
// name is the DECLARING module's name, per stdlib §2.13).
BEZEL_TEST(captured_var_and_generic_class_method_char_enum_interpolation_5024,
           direct_field_access_char_enum_4953_aside_no_lambda) {
    auto r = runAot(
        "module M; "
        "public enum Color Red, Green; end Color; "
        "public class B<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Show(): string => $\"{_v}\"; "
        "end B; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new B<char>('x'); "
        "    Console.WriteLine(a.Show()); "
        "    using var b := new B<Color>(Color.Green); "
        "    Console.WriteLine(b.Show()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "x\nM.Color\n");
}
