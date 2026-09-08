// Spec-derived regression tests for fix #4080 — §27.6 / §27.8 (v0.294):
// an UNBOUND extension-method reference (type-name qualifier) through a
// `type` alias of a PRIMITIVE (`type MyInt := int;`) was misclassified as
// the BOUND (instance-qualifier, captured-receiver) form.
//
// The shared `resolveAliasToTypeSymbol` helper (TypeResolution_Names.cpp,
// #4050/#4051 / #4070) is Named-only — its signature returns TypeSymbol*
// and its guard (`underlying->kind != TypeRef::Kind::Named`) excludes every
// Primitive-kind underlying by construction. So for `type MyInt4080 := int;`
// the helper returns null and `ExprTC::resolveExtensionMember`'s
// `unboundExtRef` flag stays false: the Function TypeRef built for the
// reference silently DROPS the leading receiver parameter §27.6 requires,
// mirroring the class/record/interface/enum alias gap #4070 closed for Named
// underlyings on the exact same classification site.
//
// The receiver-type-driven extension lookup at
// `ExprTC::resolveMemberAccess` (~line 19625, the §26.5 primitive-receiver
// arm) consults `ctx_.exprType(*ma.target)` directly. Without an
// accompanying fix in the alias arm of `resolveMemberAccess`, the alias
// identifier's exprType stays null and the lookup silently misses the
// extension — the Function TypeRef is built by `resolveExtensionMember`
// with the receiver prepended (the `unboundExtRef` arm below), but the
// lookup never reaches that code path at all.
//
// Pre-fix (verified on `461578b`/`511e576`, no `setExprType` arm and no
// `unboundExtRef` primitive-alias arm): the bare-reference form SIGSEGVs
// at runtime (the missing receiver parameter §27.6 requires causes the
// runtime adapter to dereference unmapped memory when the body reads
// `this`), and the call form silently returns 0 (the #2434/#2491 backstops
// degrade the call to a zero/null return value rather than crashing). The
// alias arm in `resolveMemberAccess` is what routes the alias qualifier
// into the primitive-receiver lookup at all; the `unboundExtRef` arm in
// `resolveExtensionMember` is what makes the lookup's result prepend the
// receiver. Both halves are required to land the fix.
//
// Each pin below derives from §27.6 (method references — "the reference
// uses the extended type's name as qualifier; the resulting function has
// the receiver as its first parameter") and §27.8 (type aliases — "Type
// aliases see through to the aliased type for identity, equality, method
// dispatch, and overload resolution").

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #4080 issue repro, verbatim (§27.6 / §27.8): an UNBOUND extension-method
// reference through a primitive alias, assigned to an explicitly-typed
// local. Pre-fix: SIGSEGV at runtime (the reference's computed type is
// missing the receiver parameter; codegen's runtime adapter dereferences
// unmapped memory when the extension's body reads `this`). Post-fix: 42.
BEZEL_TEST(type_alias_extension_method_ref_4080,
           prim_alias_unbound_ref_declared_type) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type MyIntAlias4080 := int; "
        "extension int "
        "  public method Doubled4080(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := MyIntAlias4080.Doubled4080; "
        "    return f(21); "
        "  end Main; "
        "end C;"), 42);
}

// Same shape, INFERRED local type — the reference's own computed function
// type drives the call site's arity check. Pre-fix: SIGSEGV (same root
// cause as the declared-type cell above — the missing receiver parameter
// mis-shapes the reference's runtime type regardless of whether the local's
// declared target is explicit or inferred). Post-fix: 42.
BEZEL_TEST(type_alias_extension_method_ref_4080,
           prim_alias_unbound_ref_inferred_type) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type MyIntAlias4080b := int; "
        "extension int "
        "  public method Doubled4080b(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f := MyIntAlias4080b.Doubled4080b; "
        "    return f(21); "
        "  end Main; "
        "end C;"), 42);
}

// Anti-regression: the DIRECT (non-alias) primitive-keyword spelling
// `int.Doubled4080c` must still work identically — pins that the new
// primitive-alias arm did not disturb the existing primitive-keyword arm
// it sits beside (ExprTC::resolveExtensionMember's `else if` for an
// IdentifierExpr target with a `primitiveFromName(tid->name) != None`
// check), nor the alias arm in `resolveMemberAccess` (the
// `ctx_.setExprType` call must not have changed the receiver-type for
// the bare-keyword case).
BEZEL_TEST(type_alias_extension_method_ref_4080,
           direct_prim_keyword_unbound_ref_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extension int "
        "  public method Doubled4080c(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := int.Doubled4080c; "
        "    return f(21); "
        "  end Main; "
        "end C;"), 42);
}

// Anti-regression: a BOUND (instance-qualifier) extension reference
// through an instance whose DECLARED type happens to be the alias must
// still capture the receiver, not misfire the new primitive-alias arm.
// `t`'s exprSymbol at `t.Doubled4080d` is `t`'s own LocalSymbol (a value),
// never the alias itself, so `dynamic_cast<TypeAliasSymbol*>(tgtSym)`
// correctly fails regardless of this fix — verified end-to-end, not just
// by code inspection.
BEZEL_TEST(type_alias_extension_method_ref_4080,
           bound_ref_via_prim_alias_typed_local_regression) {
    // A primitive is value-shaped, so `using var t := 21` plus
    // `t.Doubled4080d()` binds the receiver by copy and returns 21*2 = 42.
    // The LocalSymbol's declType is `MyIntAlias4080d`, an alias of `int`,
    // so the bound-ref path's existing `TypeSymbol? -> bool` test returns
    // false (it's a LocalSymbol, not a TypeSymbol or TypeAliasSymbol).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type MyIntAlias4080d := int; "
        "extension int "
        "  public method Doubled4080d(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var t: MyIntAlias4080d := 21; "
        "    return t.Doubled4080d(); "
        "  end Main; "
        "end C;"), 42);
}
