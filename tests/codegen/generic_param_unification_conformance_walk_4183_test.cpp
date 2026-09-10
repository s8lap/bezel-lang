// fix #4183 (§14.5 phase 1, v0.296/v0.298 — the FULL sentence): review
// finding on the #4089/#4090 lane (commits 7ec92f7, 62d88ce). §14.5 reads:
//
//   "a constructed parameter type G<P1, ..., Pn> unifies component-wise and
//    invariantly against an argument of the same generic type, OR AGAINST
//    THE UNIQUE INSTANTIATION OF G THAT THE ARGUMENT'S TYPE IMPLEMENTS OR
//    EXTENDS (an `Array<int>` argument against `xs: IEnumerable<T>` binds
//    `T` to `int`); an argument whose type reaches no instantiation of G
//    makes the candidate inapplicable, and so does one whose type reaches
//    MORE THAN ONE ... a non-unique instantiation is never resolved by
//    choice."
//
// #7ec92f7's fix (for the unrelated #4089/#4090 Task<R>-vs-OwningTask<Widget>
// unsound-bind defect) implemented only the FIRST HALF of this sentence — an
// exact head-name-equality check — dropping the "implements or extends"
// half entirely. That silently regressed every constructed-generic-type
// parameter bound through an interface, a base class, or a parameter-side
// generic type alias — including the spec's OWN worked example — none of
// which involve a function-typed parameter at all, yet all were rejected by
// the (mis-firing, now-corrected) "still unbound" diagnostic. See #4183 for
// the full review finding and measured base-vs-fix numbers.
//
// THE FIX (src/sema/ExprTypecheck_Calls.cpp, ExprTC::unifyArgSubst's
// constructed-generic-type branch): resolve the parameter's declared type
// (`pt`) to find G's declaration — this also resolves a generic alias on
// the parameter side transparently (#1462: alias resolution copies the
// aliased type's `declaration`, per §27.8's "type aliases ... see through
// ... for ... overload resolution") — then:
//   - for a USER-declared G (a real TypeSymbol): walk the argument's
//     ancestor/conformance graph via `collectAncestorInstantiationsOf`
//     (ExprTypecheck.cpp; #1920 — the SAME BFS §19.3's generic-
//     instantiation-impossible checks already use for "which instantiation
//     of G does this concrete type implement or extend");
//   - for a BUILTIN G (no TypeSymbol — `IReadOnlyList`, `IEnumerable`, ...):
//     walk the builtin interface-hierarchy table via the new
//     `collectBuiltinAncestorInstantiationsOf` (ExprTypecheck.cpp), which
//     exposes `builtinInterfaceAncestorsOf` — the SAME table
//     `checkAssignable`'s `variantGenericAssignable` already consults for
//     ordinary `Array<int>` → `IReadOnlyList<int>` assignability
//     (v0.180/#1180) — deduped by structural identity so a diamond in that
//     table (`IList` reaches `IReadOnlyCollection` via both `IReadOnlyList`
//     and `ICollection`) is not mistaken for the spec's "reaches more than
//     one [DISTINCT instantiation]" case.
// Both walkers return every DISTINCT instantiation reached; a result of
// exactly one binds component-wise (the spec's positive case), zero or more
// than one leaves the parameter unbound (the spec's "inapplicable"/
// "never resolved by choice" cases) — reported by the existing "cannot
// infer" check (#4089) rather than proceeding with an incomplete
// substitution.
//
// FAIL-WITHOUT / PASS-WITH: 4 of the 6 rows below fail without this fix (on
// `62d88ce`, #7ec92f7's head-name-only guard) and pass with it (on this
// commit) — measured via a src/ revert + rebuild + re-run cycle, both
// directions against the SAME rebuilt bezel_tests_codegen binary:
// `builtin_interface_param_one_hop`, `builtin_interface_param_transitive`,
// `user_base_class_param`, `param_side_generic_alias`. The other two are
// NOT fail-without proofs, each for a different, stated reason:
//   - `arg_side_alias_static_type_known` is a REGRESSION CONTROL — it
//     passes on BOTH sides (see its own comment).
//   - `genuinely_ambiguous_two_impls` ALSO passes on both sides, but for
//     TWO DIFFERENT REASONS, not one: on `62d88ce`, `Dual` (a NON-generic
//     class) fails the pre-fix code's `argTy->genericArgs.size() ==
//     pt->genericArgs.size()` gate outright (0 != 1, ITag<T> has one
//     generic arg), falling through with no binding attempted at all —
//     coincidentally producing the SAME "cannot infer" outcome the fix's
//     genuine "reaches more than one instantiation" detection produces for
//     a real reason. The row is kept — and its comment says so — because
//     it is still the cleanest end-to-end exercise of the spec's "more
//     than one" clause and the diamond-dedup logic on the builtin side;
//     an earlier version of an unrelated row in this file (the transitive
//     one, before it returned `T`) made exactly this "passes both, but not
//     for the same reason" mistake look like a genuine fail-without proof,
//     which is why every row here was independently re-measured, not
//     assumed from the shape of its change.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// #4183 repro 1 — the spec's OWN worked example: a concrete BUILTIN
// collection argument against a BUILTIN generic-interface-typed parameter.
// Neither side has a TypeSymbol; resolved via collectBuiltinAncestorInstantiationsOf.
BEZEL_TEST(generic_param_unification_conformance_walk_4183,
           builtin_interface_param_one_hop) {
    auto r = runAot(
        "module M; "
        "class Util "
        "  public static method Peek<T>(b: IReadOnlyList<T>): T "
        "    return b[0]; "
        "  end Peek; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(1, fill := 9); "
        "    return Util.Peek(arr); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// Additional coverage per review request #5: a builtin argument implementing
// the target interface TRANSITIVELY, two hops (`Array<T>: IList<T>`,
// `IList<T>: ICollection<T>`, `ICollection<T>: IReadOnlyCollection<T>` —
// three hops from Array, exercising the builtin walker's own recursive
// closure and its diamond-dedup: `IReadOnlyCollection` is reachable from
// `IList` via BOTH `IReadOnlyList` and `ICollection`).
BEZEL_TEST(generic_param_unification_conformance_walk_4183,
           builtin_interface_param_transitive) {
    // T must appear in the RETURN type too (not just the parameter) — the
    // "still unbound" check (#4089) only fires then, so only this shape
    // actually distinguishes pre-fix from post-fix. An earlier version of
    // this row returned `int` (not `T`) and passed BOTH with and without
    // this fix — codegen re-derives a generic call's monomorphization
    // independently at the lowering site (see the sibling #4089 test
    // file's header) when sema's own substitution is never consulted for
    // anything sema-visible, so a T that binds nothing OBSERVABLE at the
    // sema level is not a real fail-without row.
    auto r = runAot(
        "module M; "
        "class Util "
        "  public static method First<T>(b: IReadOnlyCollection<T>): T "
        "    for var x in b do return x; end; "
        "    throw new IndexOutOfRangeException(); "
        "  end First; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var arr := new Array<int>(3, fill := 5); "
        "    return Util.First(arr); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// #4183 repro 2 — a USER-declared subclass argument against a base-class-
// typed parameter.
BEZEL_TEST(generic_param_unification_conformance_walk_4183,
           user_base_class_param) {
    auto r = runAot(
        "module M; "
        "class Base<T> "
        "  public property V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "end Base; "
        "class Derived<T>: Base<T> "
        "  public constructor(v: T) base(v); end constructor; "
        "end Derived; "
        "class Util "
        "  public static method Peek<T>(b: Base<T>): T "
        "    return b.V; "
        "  end Peek; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var d := new Derived<int>(7); "
        "    return Util.Peek(d); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #4183 repro 3 — a generic type ALIAS on the PARAMETER side (§27.8: aliases
// see through for overload resolution). `pt` is resolved so `ptResolved->
// declaration` is already `Box`'s real TypeSymbol, never a synthetic
// `Wrapper` symbol.
BEZEL_TEST(generic_param_unification_conformance_walk_4183,
           param_side_generic_alias) {
    auto r = runAot(
        "module M; "
        "type Wrapper<T> := Box<T>; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method Unwrap<T>(b: Wrapper<T>): T "
        "    return b.Value; "
        "  end Unwrap; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box<int>(42); "
        "    return Util.Unwrap(b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Additional coverage per review request #5 — a generic type alias on the
// ARGUMENT side. SEMA-level check only: asserting full execution here would
// collide with issue #4188, a SEPARATE, PRE-EXISTING codegen defect
// (measured identically on base `2c0ac3e`, unaffected by this fix) where a
// local variable's OWN declared-alias type derives a different LLVM
// monomorphization key than the callee's parameter type, an IR-verifier
// crash unrelated to type-argument INFERENCE. What this row actually pins:
// sema itself accepts the call and does NOT emit this fix's "cannot infer"
// diagnostic for an argument-side alias — i.e. unification correctly reads
// through the argument's alias-transparent `declaration` (already the real
// `Box` TypeSymbol, set by the standard alias-resolution convention,
// independent of anything this fix added) rather than rejecting.
BEZEL_TEST(generic_param_unification_conformance_walk_4183,
           arg_side_alias_static_type_known) {
    auto r = runAot(
        "module M; "
        "type Alias<T> := Box<T>; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method Unwrap<T>(b: Box<T>): T "
        "    return b.Value; "
        "  end Unwrap; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var a: Alias<int> := new Box<int>(55); "
        "    return Util.Unwrap(a); "
        "  end Main; "
        "end C;");
    // #4188: this specific program crashes CODEGEN (IR verification
    // failure), pre-existing and unaffected by this fix — so `r.ran` is
    // NOT asserted true here. What IS asserted: whatever diagnostics ARE
    // present, none of them is this fix's own "cannot infer" rejection —
    // sema's type-argument binding for the argument-side alias is sound.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "cannot infer the type argument"));
}

// Additional coverage per review request #5 — "a class implementing the
// same interface at two different instantiations, if the language permits
// it." It does, for an interface whose member signature does not depend on
// T (`ITag<T>.Ping(): int` — declaring a T-DEPENDENT member (e.g.
// `Get(): T`) twice at different T is NOT constructible in Bezel: measured
// separately, `class Dual: IHolder<int>, IHolder<string>` with two `Get()`
// overloads is rejected at the DECLARATION site, "method `Get` does not
// match interface ... parameter types must match exactly" (§11.2) — there
// is no per-instantiation dispatch for interface conformance). This is the
// spec's own "reaches more than one" clause exercised end-to-end: `Dual`
// implements BOTH `ITag<int>` and `ITag<string>`, so `Util.Peek<T>`'s `x:
// ITag<T>` parameter reaches two DISTINCT instantiations — no binding,
// silently, and T also appears in the return type, so the "still unbound"
// check reports cleanly rather than picking one arbitrarily.
BEZEL_TEST(generic_param_unification_conformance_walk_4183,
           genuinely_ambiguous_two_impls) {
    auto r = runAot(
        "module M; "
        "interface ITag<T> method Ping(): int; end ITag; "
        "class Dual: ITag<int>, ITag<string> "
        "  method Ping(): int return 1; end Ping; "
        "end Dual; "
        "class Util "
        "  public static method Peek<T>(x: ITag<T>): T "
        "    throw new IndexOutOfRangeException(); "
        "  end Peek; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var d := new Dual(); "
        "    var v := Util.Peek(d); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot infer the type argument"));
}
