// Regression tests for #5094 — a narrower gate in #5024's own fix (b) misses
// an INHERITED (not directly-declared) generic-class method called through a
// SUBCLASS receiver.
//
// ISSUE REPRO (verbatim from #5094's body, MEASURED on r22-5024 / edabc0b0,
// non-author review lane rv-5024):
//
//   module P8;
//   public class Box<T>
//     protected _v: T;
//     public constructor(v: T) _v := v; end constructor;
//     public method Apply(f: (T) -> string): string
//       return f(_v);
//     end Apply;
//   end Box;
//   public class SubBox<T>: Box<T>
//     public constructor(v: T) base(v); end constructor;
//   end SubBox;
//   public class Util
//     public static method Go<U>(v: U): string
//       var b := new SubBox<U>(v);
//       return b.Apply(lambda(y) => $"<{y}>");
//     end Go;
//   end Util;
//   public class Program
//     public static method Main()
//       Console.WriteLine(Util.Go<char>('m'));
//     end Main;
//   end Program;
//
// Prints "<109>\n" pre-fix; expected "<m>\n" per stdlib §8.5 (char.ToString()
// is the character itself) — identical to #5024's own `boxcls` repro, just
// through an INHERITED (not directly-declared) `Apply`.
//
// ROOT CAUSE (#5094's own analysis, confirmed by reading #5024's diff):
// `LlvmBackend_Calls2.cpp`'s #5024 pre-pass block binds the callee's OWNER
// (declaring class) type-parameter names from the receiver's own class
// instantiation, but gates on an EXACT match between the receiver's static
// type and the callee's declaring class (`recvTy->declaration ==
// ms->parent`). For a `SubBox<U>` receiver calling the inherited `Apply`,
// `ms->parent` is `Box` but `recvTy->declaration` is `SubBox`, so the gate
// misses and the whole block is skipped — same as pre-#5024 behavior for
// this shape (a MISSED CASE, not a regression: #5024's own test file uses a
// direct, non-subclass receiver in every row).
//
// FIX: the block now branches on the receiver/declaring-class relationship.
// An exact-match receiver keeps #5024's original binding unchanged. A
// SUBCLASS receiver instead asks the shared #585/#2840
// `computeAncestorGenericSubst` authority (LlvmBackend_Calls2.cpp, reused
// by a dozen other call sites — `emitMethodCall`'s field/property/CompareTo
// paths, `classStruct`'s layout walk, etc. — rather than re-derived here) to
// walk `recvTy`'s base-class chain up to `ms->parent`, composing each
// level's own `baseTypeRefs[i]->genericArgs` against the level below. That
// walk already handles a pass-through subclass (`SubBox<U>: Box<U>`), a
// fixed base argument (`IntBox: Box<char>`), a reordered base-clause
// parameter (`Pair2<A,B>: Base<B>`), and a multi-level chain — every shape
// this file exercises below.
//
// spec: Bezel_Language_Spec.md §14.1 (monomorphization), §14.5 phase 2
// (lambda parameter types come from the bindings established so far),
// §12 / §12.7 / §12.9 (inheritance and dispatch), §18.5 (interpolation
// dispatches ToString() via the receiver's static type); Bezel_Standard_
// Library.md §8.5 (char.ToString() is the character itself). v0.307.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// #5094 (§14.1, §14.5 phase 2, §12.7, §18.5, stdlib §8.5, v0.307): the
// issue's own repro — a generic subclass (`SubBox<T>: Box<T>`) that simply
// passes its own type parameter through to the base, called from inside an
// enclosing generic method (`Go<U>`) so the concrete type (`char`) is only
// known via the active outer substitution, not written literally anywhere in
// `SubBox`'s own declaration. Pre-fix stdout: "<109>\n". Post-fix: "<m>\n".
BEZEL_TEST(subclass_inherited_generic_method_char_enum_interpolation_5094,
           subclass_generic_pass_through_prints_char) {
    auto r = runAot(
        "module P8; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Apply(f: (T) -> string): string "
        "    return f(_v); "
        "  end Apply; "
        "end Box; "
        "public class SubBox<T>: Box<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "end SubBox; "
        "public class Util "
        "  public static method Go<U>(v: U): string "
        "    var b := new SubBox<U>(v); "
        "    return b.Apply(lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util.Go<char>('m')); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<m>\n");
}

// #5094 control (§14.1, v0.307): the SAME subclass-pass-through shape,
// specialized at `int`, must stay unaffected — `int` also lowers to i32 (the
// ambiguous sentinel), but its ordinal IS its printed value, so this row is
// unchanged on both sides of the fix. Proves the fix is additive: it does
// not disturb a shape that already happened to print correctly.
BEZEL_TEST(subclass_inherited_generic_method_char_enum_interpolation_5094,
           subclass_generic_pass_through_int_control_unaffected) {
    auto r = runAot(
        "module P8; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Apply(f: (T) -> string): string "
        "    return f(_v); "
        "  end Apply; "
        "end Box; "
        "public class SubBox<T>: Box<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "end SubBox; "
        "public class Util "
        "  public static method Go<U>(v: U): string "
        "    var b := new SubBox<U>(v); "
        "    return b.Apply(lambda(y) => $\"<{y}>\"); "
        "  end Go; "
        "end Util; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util.Go<int>(99)); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<99>\n");
}

// #5094 (§14.1, §12.7, §18.5, stdlib §8.5, v0.307): a FIXED base argument —
// a non-generic subclass that instantiates the generic base at a concrete
// `char` written directly in its own base clause (`CharBox: Box<char>`,
// mirroring the issue body's own `IntBox: Box<char>` example). No enclosing
// generic method is involved at all: `ms->parent` (`Box`) is reachable from
// `CharBox` only via the base-chain walk, never by the exact-match gate,
// regardless of any active substitution. Pre-fix stdout: "<122>\n" (the
// ordinal of 'z'). Post-fix: "<z>\n".
BEZEL_TEST(subclass_inherited_generic_method_char_enum_interpolation_5094,
           fixed_arg_subclass_prints_char) {
    auto r = runAot(
        "module P8; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Apply(f: (T) -> string): string "
        "    return f(_v); "
        "  end Apply; "
        "end Box; "
        "public class CharBox: Box<char> "
        "  public constructor(v: char) base(v); end constructor; "
        "end CharBox; "
        "public class Program "
        "  public static method Main() "
        "    var b := new CharBox('z'); "
        "    Console.WriteLine(b.Apply(lambda(y) => $\"<{y}>\")); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<z>\n");
}

// #5094 (§14.1, §12.7, §18.5, stdlib §8.5, v0.307): a REORDERED base-clause
// parameter — the derived class declares its OWN two type parameters (`A`,
// `B`) and instantiates the base with the SECOND one (`Wrap<A,B>: Box<B>`),
// mirroring the issue body's own `Pair2<A,B>: Base<B>` example. A walk that
// naively used positional index 0 of the derived receiver's own generic
// args (binding `A`, i.e. `int`) instead of resolving through the written
// base clause (`Box<B>`, i.e. `char`) would print the WRONG value's ordinal
// ("<1>") rather than declining or printing "<q>". Both of `Go3`'s type
// parameters are inferred from the call's ordinary arguments (§14.5
// inference), not written explicitly, so this also confirms the fix does
// not depend on an explicit type-argument call spelling. Pre-fix stdout:
// "<113>\n" (the ordinal of 'q'). Post-fix: "<q>\n".
BEZEL_TEST(subclass_inherited_generic_method_char_enum_interpolation_5094,
           reordered_param_subclass_prints_char) {
    auto r = runAot(
        "module P8; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Apply(f: (T) -> string): string "
        "    return f(_v); "
        "  end Apply; "
        "end Box; "
        "public class Wrap<A, B>: Box<B> "
        "  public constructor(a: A, b: B) base(b); end constructor; "
        "end Wrap; "
        "public class Util3 "
        "  public static method Go3<X, Y>(a: X, b: Y): string "
        "    var w := new Wrap<X, Y>(a, b); "
        "    return w.Apply(lambda(y) => $\"<{y}>\"); "
        "  end Go3; "
        "end Util3; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util3.Go3(1, 'q')); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<q>\n");
}

// #5094 (§14.1, §12.7, §12.9, §18.5, stdlib §8.5, v0.307): a TWO-LEVEL
// inheritance chain (`DeepBox<T>: MidBox<T>: Box<T>`) — `ms->parent` (`Box`)
// is neither `DeepBox` (the receiver's static type) nor its immediate base
// (`MidBox`), so the fix's base-chain walk must cross two `baseTypeRefs`
// links, composing `MidBox`'s own generic-argument binding through
// `DeepBox`'s before reaching `Box`'s. Pre-fix stdout: "<119>\n" (the
// ordinal of 'w'). Post-fix: "<w>\n".
BEZEL_TEST(subclass_inherited_generic_method_char_enum_interpolation_5094,
           two_level_inheritance_prints_char) {
    auto r = runAot(
        "module P8; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Apply(f: (T) -> string): string "
        "    return f(_v); "
        "  end Apply; "
        "end Box; "
        "public class MidBox<T>: Box<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "end MidBox; "
        "public class DeepBox<T>: MidBox<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "end DeepBox; "
        "public class Util4 "
        "  public static method Go4<V>(v: V): string "
        "    var d := new DeepBox<V>(v); "
        "    return d.Apply(lambda(y) => $\"<{y}>\"); "
        "  end Go4; "
        "end Util4; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine(Util4.Go4<char>('w')); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "<w>\n");
}
