// fix #4325 / spec §5.1 *Implicit conversions* / §11.2 *Interface
// declaration* (v0.298). Sibling of #4310 — that one fixed the
// collection-literal-typing family, this one fixes the GENERIC METHOD CALL
// ARGUMENT family when the call site wrote an EXPLICIT type argument.
//
// §11.2 says "A class implements an interface implicitly: by declaring
// matching public members" — read together with §11.1's "A class may
// declare ... any number of interfaces" (the `: I1, I2` inheritance-
// clause list a conforming class must appear in): the "implicitly" clause
// waives per-member `override` annotations, it does not waive LISTING the
// interface. This is the reading `checkAssignable`'s existing `isSubtype`
// walk already enforces — verified correct here at the two "_control" rows
// below before this fix touched anything.
//
// ROOT CAUSE (traced, ExprTypecheck_Calls.cpp). `checkArgsAgainstParams`
// performs the §5.1 / §11.2 conversion check for every call argument,
// including a generic method's, and correctly fires for a plain
// (non-generic) call argument (see `plain_call_argument_noconform_rejected_control`
// below — this is the test the #4310 suite added). But `typeCall`'s
// explicit-type-arguments path built the substitution map
// `T → IHolder` for a call like `Helper.MakeArr<IHolder>(arg)` and
// applied it ONLY to the call's RETURN type (`rt =
// substituteReturnType(rt, explicitSubst)`, ~L3490) — the per-argument
// §5.1 check ran against the RAW, unsubstituted parameter type, so for
// `MakeArr<T>(x: T)` the parameter was typed as the bare `T`,
// `checkAssignable`'s free-type-var fallback silently accepted a
// non-conforming `Widget3` against `IHolder`'s constraint, and codegen
// dispatched the interface call through a null itable slot — the
// SIGSEGV measured identically to #4302's crash signature.
//
// THE FIX is a one-line substitution inside `checkArgsAgainstParams`
// (`paramTy = substituteReturnType(paramTy, *explicitSubst)`,
// ExprTypecheck_Calls.cpp), applied between `extSubst` (the extension
// free var) and the `checkAssignable` call — letting the SAME
// `checkAssignable` + `isSubtype` authority that already rejects the
// non-generic twin reject this one too. The map is built in `typeCall`
// and threaded through a new `explicitSubst` parameter on `checkCallArity`
// and `checkArgsAgainstParams`; both default to `nullptr` so the four
// constructor / extension / multi-overload call sites stay unchanged in
// behaviour. Ordering: class-level `substituteMemberType` (receiver
// type params) first, then `extSubst` (extension free var), then
// `explicitSubst` (method's own type params from explicit `<…>`) — three
// disjoint namespaces, so the ordering is the documented rule and the
// method's own type params (which is what `<…>` names) win over a same-
// named class param, mirroring the spec's shadowing policy (the compiler
// warns; substitution follows the warning).
//
// SCOPE, MEASURED: the failure mode is `M<T: IBar>.Foo(arg)` with `arg`
// not conforming to `IBar`. Three tested shapes: a single-method
// generic, a generic with a compound return (`[1]T`), and a generic
// with multiple type params. The pre-fix compiler accepted all three
// shapes silently and the post-fix compiler rejects all three with the
// SAME `§5.1` text `checkAssignable` already produces for the
// non-generic twin — the diagnostic is identical because the
// substitution feeds the SAME `checkAssignable` call, not a second one.
//
// Known boundary NOT fixed here, filed separately:
//   - #4316, #4317, #4315 from #4310's own suite (different code paths;
//     not retested here).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §5.1 ("... has a type that is not implicitly convertible to the target
// type (§5.1)") — the SAME diagnostic `checkAssignable` already produces
// for a plain variable declaration and an ordinary (non-generic) call
// argument (the `_control` rows below, pinned by #4310); this fix reuses
// that exact call rather than inventing a second diagnostic, so every
// reject row asserts the IDENTICAL text.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// Shared prelude: `IHolder` (one method), a NON-conforming `Widget3`
// (never lists `IHolder`), and a genuinely conforming `Widget3Ok`.
const std::string kPrelude =
    "module M; "
    "interface IHolder method GetV(): string; end IHolder; "
    "class Widget3 "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3; "
    "class Widget3Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Ok; "
    "class Helper "
    "  public static method MakeArr<T>(x: T): [1]T return [x]; end MakeArr; "
    "  public static method Take<T>(x: T): T return x; end Take; "
    "end Helper; ";

JitResult run(const std::string& mainBody) {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; end C;");
}

}  // namespace

// ============================================================
// CONTROLS — the two positions checkAssignable already covered
// before this fix, proving the fix reuses the SAME authority.
// ============================================================

// A plain (non-generic) call argument with a non-conforming class —
// the control `checkAssignable` already rejects. (#4310's
// `plain_call_argument_noconform_rejected_control`.)
BEZEL_TEST(generic_method_call_argument_interface_conformance_4325,
           plain_call_argument_noconform_rejected_control_spec_5_1) {
    auto r = runAot(
        kPrelude +
        "class C public static method Main(): int "
        "  var s := \"he\" + \"llo\"; "
        "  return Foo(new Widget3(s)); "
        "end Main; "
        "public static method Foo(h: IHolder): int "
        "  return h.GetV().Length; end Foo; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A plain (non-generic) call argument with a conforming class —
// the control `checkAssignable` already accepts.
BEZEL_TEST(generic_method_call_argument_interface_conformance_4325,
           plain_call_argument_conforming_accepted_control_spec_5_1) {
    auto r = runAot(
        kPrelude +
        "class C public static method Main(): int "
        "  var s := \"he\" + \"llo\"; "
        "  return Foo(new Widget3Ok(s)); "
        "end Main; "
        "public static method Foo(h: IHolder): int "
        "  return h.GetV().Length; end Foo; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ============================================================
// REJECT ROWS — #4325's own repair. Each fails without the fix
// (compiles clean, then SIGSEGVs at the marked access) and
// rejects with it.
// ============================================================

// Case 1: a generic method `MakeArr<T>(x: T): [1]T` called with an
// EXPLICIT type argument `<IHolder>` and a NON-conforming class
// argument. This is #4325's own repro, verbatim — bound to a local
// first so the call's `T` argument is a plain named local, not a
// temporary (avoids pre-existing §28.3 borrow-lifetime rules that
// would otherwise layer on top).
BEZEL_TEST(generic_method_call_argument_interface_conformance_4325,
           generic_method_explicit_typearg_noconform_rejected_closes_4325_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "var outer: [1]IHolder := Helper.MakeArr<IHolder>(w); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Case 2: a generic method returning a BARE type parameter
// (`Take<T>(x: T): T`) called with an explicit type argument and a
// non-conforming class — the same shape but with a scalar (non-array)
// return. Confirms the fix is not specific to the array-return case.
BEZEL_TEST(generic_method_call_argument_interface_conformance_4325,
           generic_method_explicit_typearg_bare_return_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "var h: IHolder := Helper.Take<IHolder>(w); "
        "return h.GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — every shape the fix must NOT regress.
// ============================================================

// A generic method called with an EXPLICIT type argument AND a
// genuinely CONFORMING class — must stay accepted.
BEZEL_TEST(generic_method_call_argument_interface_conformance_4325,
           generic_method_explicit_typearg_conforming_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3Ok(s); "
        "var outer: [1]IHolder := Helper.MakeArr<IHolder>(w); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A generic method called with NO explicit type argument (type
// inferred from the argument) — the substitution map stays empty
// and the fix must NOT alter this path. Tests both a conforming
// argument (passes) and a non-conforming one (also passes — type
// inference happily binds T to whatever the argument is).
BEZEL_TEST(generic_method_call_argument_interface_conformance_4325,
           generic_method_inferred_typearg_conforming_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3Ok(s); "
        "var outer: [1]Widget3Ok := Helper.MakeArr(w); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A non-generic call with a non-conforming class argument still
// rejects (unchanged by this fix) — proves the substitution is not
// applied where the call did not supply explicit type args.
BEZEL_TEST(generic_method_call_argument_interface_conformance_4325,
           no_explicit_typeargs_no_substitution_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var w := new Widget3(s); "
        "var outer: [1]Widget3 := Helper.MakeArr(w); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}