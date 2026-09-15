// Spec-derived tests for fix #4188 — §27.8 (v0.298): a local variable
// declared with a GENERIC type alias on its own declared type, then passed
// as an argument to a method whose parameter is declared with the ALIASED
// (underlying) type directly, crashed codegen with an LLVM IR verification
// failure.
//
// §27.8 states: "Each instantiation of the alias is interchangeable with
// the corresponding instantiation of the aliased type. `Lookup<string,
// int>` and `Dictionary<string, Array<int>>` are the same type." — and,
// more generally: "Type aliases — generic or not — see through to the
// aliased type for identity, equality, method dispatch, and overload
// resolution."
//
// ROOT CAUSE (measured, not the issue's original hypothesis): the alias-
// transparency arm in `Resolver::resolveNamed` (src/sema/
// TypeResolution_Names.cpp) resolves a generic alias reference by copying
// `aliasDecl->aliasedType`'s CACHED, already-resolved TypeRef verbatim
// (`ref = *underlying;`). That cached TypeRef was resolved exactly ONCE, at
// the alias's OWN declaration, against a throwaway scope that binds the
// alias's generic parameters to nullptr markers (`type Alias<T> := Box<T>;`
// resolves its `Box<T>` body with `T` UNBOUND) — and the copy never
// substituted the REFERENCE SITE's concrete type arguments
// (`Alias<int>`'s `int`) into that cached tree. So `declaration` correctly
// became `Box`'s real TypeSymbol (per #1462/§27.8, confirmed unaffected by
// this fix — see #4183's `arg_side_alias_static_type_known` row), but
// `genericArgs` stayed `[T-unbound]` instead of `[int]`.
//
// That under-substituted type then reached
// `Emitter::applyMethodGenericSubst`'s structural unification
// (`unifyMethodReturnTypeArg`, called from `emitMethodCall`,
// LlvmBackend_Calls2.cpp), which unifies the callee's declared parameter
// type (`Box<T>`) against the ARGUMENT's sema type. An unbound `T` on the
// argument side unifies with nothing, so the call's `genericSubst_` stayed
// EMPTY — which made `emitMethodCall` skip every "must specialize" branch
// and fall through to calling the method's UNSPECIALIZED i32-erased
// template function directly, with the argument's actual (correctly
// specialized) `record.Box$spec0`-typed value: a call-parameter-type
// mismatch the IR verifier rejects. This is NOT a codegen monomorphization-
// key mismatch (the issue's own hypothesis) — it is a SEMA under-
// substitution bug whose only symptom happens to surface in codegen.
//
// THE FIX (src/sema/TypeResolution_Names.cpp / TypeResolution_impl.hpp):
// `Resolver::substituteAliasGenericArgs` recursively substitutes the
// alias's own generic-parameter names for the reference site's resolved
// concrete arguments throughout the cached underlying TypeRef tree — at ANY
// nesting depth (`type Pair<A> := Box<Box<A>>` buries `A` two levels down,
// exercised below), never mutating the shared cached TypeRef in place
// (returns a fresh one only where something beneath it changed).

#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// §27.8 / #4188 issue repro verbatim: a generic alias on a CALL ARGUMENT's
// declared local type must specialize the callee exactly as the direct
// (unaliased) spelling does. Pre-fix: `bezelc` exits non-zero with an "IR
// verification failed" diagnostic at SourceLocation{} (a genuine compile
// REJECT via the driver's normal codegen-error exit, never a crash/signal —
// `failedAtCompileTimeNotRuntime` is the exact helper this pins per
// LANE_RULES §18/§14). Post-fix: runs and returns 55, IDENTICALLY to the
// direct `Box<int>` spelling (the `box_int_no_alias_control` row below).
BEZEL_TEST(type_alias_generic_arg_monomorphization_4188,
           alias_typed_local_as_call_argument_runs_and_returns_value) {
    const std::string prog =
        "module M; "
        "type Alias4188<T> := Box4188<T>; "
        "record Box4188<T>(Value: T); "
        "class Util4188 "
        "  public static method Unwrap<T>(b: Box4188<T>): T "
        "    return b.Value; "
        "  end Unwrap; "
        "end Util4188; "
        "class C "
        "  public static method Main(): int "
        "    var a: Alias4188<int> := new Box4188<int>(55); "
        "    return Util4188.Unwrap(a); "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    // Post-fix: the call verifies and runs. (Pre-fix this row instead
    // satisfies `failedAtCompileTimeNotRuntime(r)` — a genuine compile-time
    // IR-verification reject via the driver's normal codegen-error exit,
    // never a runtime crash/signal — which is how the FAIL-WITHOUT half of
    // this fix's proof was measured; see the file header.)
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// Control: the IDENTICAL program with the alias spelled out directly
// (`Box4188<int>`, no alias at all) — this always worked, on both sides of
// the fix. Pins that the fix's shared alias-resolution code path change did
// not need to touch (and did not break) the non-alias case.
BEZEL_TEST(type_alias_generic_arg_monomorphization_4188,
           box_int_no_alias_control) {
    const std::string prog =
        "module M; "
        "record Box4188b(Value: int); "
        "class Util4188b "
        "  public static method Unwrap(b: Box4188b): int "
        "    return b.Value; "
        "  end Unwrap; "
        "end Util4188b; "
        "class C "
        "  public static method Main(): int "
        "    var a: Box4188b := new Box4188b(55); "
        "    return Util4188b.Unwrap(a); "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// §27.8's own generic-alias shape (`Lookup<K, V> := Dictionary<K,
// Array<V>>`) buries a parameter INSIDE the underlying type's own
// generic-argument tree, not at the top level. `substituteAliasGenericArgs`
// must recurse rather than only substitute a top-level bare parameter
// (which is all `applyGenericSubst`, the pre-existing override-signature
// helper, does — the reason this fix needed a NEW recursive walk rather
// than reusing that one, per LANE_RULES §6/§25). Exercised here with two
// levels of a user generic record standing in for Dictionary/Array: `type
// Pair4188<A> := Box4188c<Box4188c<A>>;` buries `A` two `genericArgs` deep.
// Field access proves the SPECIALIZED layout is correct at both levels
// (wrong-depth substitution would either fail to compile or read the wrong
// field width).
BEZEL_TEST(type_alias_generic_arg_monomorphization_4188,
           nested_generic_alias_substitutes_at_every_depth) {
    const std::string prog =
        "module M; "
        "type Pair4188<A> := Box4188c<Box4188c<A>>; "
        "record Box4188c<T>(Value: T); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pair4188<int> := new Box4188c<Box4188c<int>>(new "
        "Box4188c<int>(19)); "
        "    return p.Value.Value; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 19);
}

// Control: a NON-generic alias (`aliasDecl->genericParams` empty) must take
// the unaffected fast path — `substituteAliasGenericArgs`'s substitution
// map is empty, so it returns its input unchanged with no allocation. Pins
// that the fix is scoped to GENERIC aliases and does not disturb the
// ordinary `type UserId := int;` form (§27.8's own first example).
BEZEL_TEST(type_alias_generic_arg_monomorphization_4188,
           non_generic_alias_unaffected) {
    const std::string prog =
        "module M; "
        "type UserId4188 := int; "
        "class C "
        "  public static method Main(): int "
        "    var u: UserId4188 := 7; "
        "    return u; "
        "  end Main; "
        "end C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 7);
}
