// fix #2755 / §14.1 "Generic declarations" (*Instantiation and its site*) +
// §2.8.3 rule 4 "Identity is one type at runtime, too", v0.306.
//
// Family I1 (the generic-substitution family, docs/proposals/
// defect-campaign-2026-09.md §2). The sema-side eager walkers were unified by
// #4212 into `sema::substituteGenericArgs`; this row pins the CODEGEN-side
// instance of the same mechanism.
//
// §14.1: "An **instantiation** of a generic declaration is the constructed type
// or constructed member obtained by substituting type arguments for its type
// parameters." A static-access call `Inner<U>.Head()` made from inside a LIVE
// specialization of an enclosing generic (`OuterU<long>`) names an instantiation
// whose type argument is the enclosing generic's own parameter — so the
// substitution that must be applied is the composition of the two.
//
// `Emitter::lowerCall`'s static-access arm built the callee's two substitution
// maps out of step. The LLVM half went through `lowerTypeRef`, which reads
// `genericSubst_` — still the OUTER map at that point — so it COMPOSED
// (`T -> i64`). The sema half stored `recvTy->genericArgs[gi]` RAW, so it kept
// the still-open `U`: a `Named` TypeRef with `declaration == nullptr`. Every
// consumer of the sema half then saw an unbound type parameter:
//
//   * the specialization NAME loop skips a `declaration`-less ref, so the callee
//     was emitted as `Inner$Head$spec` with no `$Long` suffix — indistinguishable
//     from the dead-template pseudo-specialization;
//   * the specialization CACHE KEY keyed on `typeRefIdentityKey(U)` rather than
//     on the concrete type, so distinct instantiations could collide — which is
//     exactly what §2.8.3 rule 4 forbids ("a constructed generic type ... is one
//     type *in the running process*");
//   * `lowerTypeRef`'s nullable bare-parameter branch fell back to the
//     `{i8, i32}` placeholder.
//
// Net effect: a live, concrete caller (`OuterU$Head$spec$long`, returning
// `{i8,i64}`) called a body built under a NON-concrete substitution, returning
// `{i8,i32}`. That is malformed IR on a LEGAL program, and `verifyModule` caught
// it:
//
//     Function return type does not match operand type of return inst!
//       ret { i8, i32 } %1
//      { i8, i64 }
//     bezelc: codegen error: IR verification failed
//
// THE FIX composes the sema argument through the still-active outer
// substitution before storing it — `substituteTypeRefLocal(recvTy->
// genericArgs[gi], genericSubstSemaTy_)` — exactly as the INSTANCE arm of the
// same function already does. `substituteTypeRefLocal` is the RECURSIVE superset
// of `resolveTypeParamRef`, which matters for a nested argument
// (`Inner<Wrap<U>>`) that is not a top-level bare parameter. An empty outer map
// makes it the identity, so the direct `Inner<long>.Head()` path is untouched —
// pinned by the accept controls at the bottom.
//
// FAIL-WITHOUT is a COMPILE failure (exit 1, "IR verification failed"), not a
// wrong value, so every row below asserts `r.ran` — a row that merely checked
// `exitCode` would still be meaningful, but `r.ran` is what actually flips.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

// ===========================================================================
// The issue's FIRST repro, verbatim: `where T: value`, a `T?` return, and the
// type argument is the enclosing generic's own parameter. `Head()` returns
// `null`, so 3 is the program's correct answer; what the fix changes is that
// the program COMPILES at all.
// ===========================================================================
BEZEL_TEST(static_access_generic_subst_2755, value_param_nullable_return_spec_14_1) {
    auto r = runAot(
        "module M; "
        "class Inner<T> where T: value "
        "  public static method Head(): T? "
        "    return null; "
        "  end Head; "
        "end Inner; "
        "class OuterU<U> where U: value "
        "  public method Head(): U? "
        "    return Inner<U>.Head(); "
        "  end Head; "
        "end OuterU; "
        "class C "
        "  public static method Main(): int "
        "    using var o := new OuterU<long>(); "
        "    var s := o.Head(); "
        "    if s = null then return 3; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// The issue's SECOND repro: a class-constrained parameter and an `owned T?`
// return. Pre-fix this failed the same way with a different shape
// (`ret { i8, i32 }` vs `ptr`), which is why both are pinned — one fix must
// cover the reference-typed lowering as well as the value-typed one.
// ===========================================================================
BEZEL_TEST(static_access_generic_subst_2755, class_param_owned_nullable_return_spec_14_1) {
    auto r = runAot(
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  public static method Head(): owned T? "
        "    return null; "
        "  end Head; "
        "end Inner; "
        "class OuterU<U> where U: class "
        "  public method Head(): owned U? "
        "    return Inner<U>.Head(); "
        "  end Head; "
        "end OuterU; "
        "class C "
        "  public static method Main(): int "
        "    using var o := new OuterU<Res>(); "
        "    using var s := o.Head(); "
        "    if s = null then return 3; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// The VALUE-CARRYING row, and the §2.8.3 rule 4 half. `Outer2755<long>` and
// `Outer2755<int>` both call `Inner2755<U>.Wrap(v)` in ONE program:
//
//   * `a = 5000000000` does not fit in 32 bits, so a body lowered at the i32
//     placeholder cannot produce it — this fails on a truncating specialization
//     even where the IR verifier would not have complained;
//   * `b = 7` in the same program pins that the two instantiations did not
//     COLLIDE in the specialization cache. Pre-fix both keyed on
//     `typeRefIdentityKey(U)`, the same unbound parameter, so one body could be
//     reused for both — "duplicate code copies ... permitted only where
//     observationally indistinguishable from one copy" (§2.8.3 rule 4).
//
// The observable is on stdout, not the exit code: 5000000000 is far outside the
// 8-bit `WEXITSTATUS` window (tests/README.md).
// ===========================================================================
BEZEL_TEST(static_access_generic_subst_2755, two_instantiations_do_not_collide_spec_2_8_3) {
    auto out = runAotOut(
        "module M; "
        "class Inner2755<T> where T: value "
        "  public static method Wrap(v: T): T? "
        "    return v; "
        "  end Wrap; "
        "end Inner2755; "
        "class Outer2755<U> where U: value "
        "  public method Wrap(v: U): U? "
        "    return Inner2755<U>.Wrap(v); "
        "  end Wrap; "
        "end Outer2755; "
        "class C "
        "  public static method Main(): int "
        "    using var ol := new Outer2755<long>(); "
        "    using var oi := new Outer2755<int>(); "
        "    var a := ol.Wrap(5000000000L); "
        "    var b := oi.Wrap(7); "
        "    if a = null then Console.WriteLine(\"null-a\"); return 91; end; "
        "    if b = null then Console.WriteLine(\"null-b\"); return 92; end; "
        "    Console.WriteLine($\"a={a!} b={b!}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, std::string("a=5000000000 b=7"));
}

// ===========================================================================
// The NESTED type argument — the reason the fix must use the RECURSIVE
// `substituteTypeRefLocal` and not the top-level-only `resolveTypeParamRef`.
// `Inner3<Wrap2755<U>>` is a constructed type whose OWN argument is the open
// `U`; a top-level-only substitution leaves `U` unbound one level down.
// ===========================================================================
BEZEL_TEST(static_access_generic_subst_2755, nested_type_argument_spec_14_1) {
    auto r = runAot(
        "module M; "
        "class Wrap2755<W> where W: value "
        "  public v: W; "
        "  public constructor(x: W) v := x; end constructor; "
        "end Wrap2755; "
        "class Inner3<T> where T: class "
        "  public static method Head(): owned T? "
        "    return null; "
        "  end Head; "
        "end Inner3; "
        "class Outer3<U> where U: value "
        "  public method Go(): int "
        "    using var h := Inner3<Wrap2755<U>>.Head(); "
        "    if h = null then return 7; end; "
        "    return 1; "
        "  end Go; "
        "end Outer3; "
        "class C "
        "  public static method Main(): int "
        "    using var o := new Outer3<long>(); "
        "    return o.Go(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// ACCEPT CONTROLS. These pass with the fix patched out too; they pin that
// composing through the outer map does not disturb the path where there IS no
// outer map (`genericSubstSemaTy_` empty → `substituteTypeRefLocal` is the
// identity).
// ===========================================================================

// A static-access call at a CONCRETE type argument, made from a non-generic
// body. No outer substitution is active, so the composition is the identity and
// the `long` specialization must still carry 64 bits.
BEZEL_TEST(static_access_generic_subst_2755, control_concrete_static_access_no_outer_subst) {
    auto out = runAotOut(
        "module M; "
        "class InnerC<T> where T: value "
        "  public static method Wrap(v: T): T? "
        "    return v; "
        "  end Wrap; "
        "end InnerC; "
        "class C "
        "  public static method Main(): int "
        "    var a := InnerC<long>.Wrap(5000000000L); "
        "    if a = null then Console.WriteLine(\"null\"); return 91; end; "
        "    Console.WriteLine($\"a={a!}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, std::string("a=5000000000"));
}

// The INSTANCE-receiver form of the same call, which has always composed
// correctly (it is the model the fix copies). Pinned so a future edit to the
// static arm that "unified" the two in the wrong direction would be caught.
BEZEL_TEST(static_access_generic_subst_2755, control_instance_receiver_still_composes) {
    auto out = runAotOut(
        "module M; "
        "class InnerI<T> where T: value "
        "  public v: T; "
        "  public constructor(x: T) v := x; end constructor; "
        "  public method Get(): T? return v; end Get; "
        "end InnerI; "
        "class OuterI<U> where U: value "
        "  public method Make(x: U): U? "
        "    using var i := new InnerI<U>(x); "
        "    return i.Get(); "
        "  end Make; "
        "end OuterI; "
        "class C "
        "  public static method Main(): int "
        "    using var o := new OuterI<long>(); "
        "    var a := o.Make(5000000000L); "
        "    if a = null then Console.WriteLine(\"null\"); return 91; end; "
        "    Console.WriteLine($\"a={a!}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(out, std::string("a=5000000000"));
}
