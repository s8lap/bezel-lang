// Spec-derived tests for fix #4070 — §27.6 / §27.8 (v0.294): an UNBOUND
// extension-method reference (type-name qualifier, e.g. `Vec.Scaled`) through
// a `type` ALIAS of the extension's target class/record was misclassified as
// the BOUND (instance-qualifier, captured-receiver) form.
//
// `ExprTC::resolveExtensionMember` (src/sema/ExprTypecheck_Calls.cpp)
// distinguishes the two per §27.6 ("For an extension method, the reference
// uses the extended type's name as qualifier; the resulting function has the
// receiver as its first parameter") by dynamic_cast<TypeSymbol*> on the
// qualifier's resolved symbol — a type-NAME qualifier resolves to a
// TypeSymbol, an instance to a value symbol. A `type` alias of the target
// resolves the qualifier to a `TypeAliasSymbol`, not a `TypeSymbol` — the
// identical missing-arm shape #4038/#4048/#4050/#4051 all fixed in their own
// call chains, with no `TypeAliasSymbol` arm here. Left unhandled,
// `unboundExtRef` stays false for `AliasOfExt.Val`, so the Function TypeRef
// built for the reference silently DROPS the leading receiver parameter
// §27.6 requires.
//
// #4070 filed this as a SIGSEGV (the reference's declared/inferred function
// type disagreeing with codegen's own thunk shape). On this tree, codegen's
// `extRef` (LlvmBackend_Layout.cpp, makeMethodThunk) is derived OPERATIONALLY
// from whether a receiver value was lowered (`!receiverVal`), not from the
// qualifier's symbol kind — `lowerExpr` on a bare TypeAliasSymbol-bound
// identifier already falls through to null exactly like a bare TypeSymbol-
// bound one, so codegen's thunk shape was already correct. The observable
// symptom had morphed (as #1306's own test file documents happening for a
// sibling bug) from the filed SIGSEGV into a false compile-time rejection:
// a §5.1 "not implicitly convertible" mismatch against an explicit target
// type, or a §5.1 call-arity mismatch ("expects 0" against the 1 argument
// the call site supplies) with an inferred type — both measured pre-fix
// below. Fix: resolve the alias through the shared
// `ExprTC::resolveAliasToTypeSymbol` helper (#4050/#4051; wraps the free
// function in TypeResolution.hpp/.cpp) so `AliasOfExt.Val` is recognized as
// the UNBOUND type-name-qualifier form exactly like the direct `Ext.Val`
// spelling — a single-layer (sema-only) fix; no codegen change needed.

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

// #4070 issue repro, verbatim (§27.6 / §27.8): an UNBOUND extension-method
// reference through an alias of the CLASS target, assigned to an explicitly-
// typed local. Pre-fix: rejected — "the initializer for `f` has a type that
// is not implicitly convertible to the target type (§5.1)" (the reference's
// computed type, missing the receiver parameter, does not unify with the
// explicit `(ExtTarget4052) -> int`). Post-fix: 42.
BEZEL_TEST(type_alias_extension_method_ref_4070, class_alias_unbound_ref_declared_type) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class ExtTarget4070 "
        "  destructor end destructor; "
        "end ExtTarget4070; "
        "type AliasExtTarget4070 := ExtTarget4070; "
        "extension ExtTarget4070 "
        "  public method Val(): int => 42; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (ExtTarget4070) -> int := AliasExtTarget4070.Val; "
        "    using var t := new ExtTarget4070(); "
        "    return f(t); "
        "  end Main; "
        "end C;"), 42);
}

// Same shape, INFERRED local type — the reference's own computed function
// type (not an explicit target) drives the call site's arity check. Pre-fix:
// rejected — "call supplies 1 argument(s) but the function value expects 0
// (§5.1)" (the misclassified BOUND type is `() -> int`). Post-fix: 42.
BEZEL_TEST(type_alias_extension_method_ref_4070, class_alias_unbound_ref_inferred_type) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class ExtTarget4070b "
        "  destructor end destructor; "
        "end ExtTarget4070b; "
        "type AliasExtTarget4070b := ExtTarget4070b; "
        "extension ExtTarget4070b "
        "  public method Val(): int => 42; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f := AliasExtTarget4070b.Val; "
        "    using var t := new ExtTarget4070b(); "
        "    return f(t); "
        "  end Main; "
        "end C;"), 42);
}

// §27.6 / §9.2: the RECORD-receiver grid cell (parity with fix #1306's own
// class/record split) — a value-typed extension receiver reached through an
// alias of the record.
BEZEL_TEST(type_alias_extension_method_ref_4070, record_alias_unbound_ref) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Vec4070(x: int) end; "
        "type AliasVec4070 := Vec4070; "
        "extension Vec4070 "
        "  public method Scaled(): int => this.x * 3; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var v := new Vec4070(14); "
        "    var f: (Vec4070) -> int := AliasVec4070.Scaled; "
        "    return f(v); "  // 14 * 3 = 42
        "  end Main; "
        "end C;"), 42);
}

// Anti-regression: the DIRECT (non-alias) unbound reference must still work
// identically — pins that the new TypeAliasSymbol arm did not disturb the
// existing TypeSymbol arm it sits beside.
BEZEL_TEST(type_alias_extension_method_ref_4070, direct_class_unbound_ref_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class ExtTargetDirect4070 "
        "  destructor end destructor; "
        "end ExtTargetDirect4070; "
        "extension ExtTargetDirect4070 "
        "  public method Val(): int => 42; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f: (ExtTargetDirect4070) -> int := ExtTargetDirect4070.Val; "
        "    using var t := new ExtTargetDirect4070(); "
        "    return f(t); "
        "  end Main; "
        "end C;"), 42);
}

// Anti-regression: a BOUND (instance-qualifier) extension reference through
// an instance whose DECLARED type happens to be the alias must still capture
// the receiver, not misfire the new TypeAliasSymbol arm. `t`'s exprSymbol at
// `t.Val` is `t`'s own LocalSymbol (a value), never the alias itself, so
// `dynamic_cast<TypeAliasSymbol*>(tgtSym)` correctly fails regardless of this
// fix — verified end-to-end, not just by code inspection.
BEZEL_TEST(type_alias_extension_method_ref_4070, bound_ref_via_alias_typed_local_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class ExtTargetBound4070 "
        "  destructor end destructor; "
        "end ExtTargetBound4070; "
        "type AliasExtTargetBound4070 := ExtTargetBound4070; "
        "extension ExtTargetBound4070 "
        "  public method Val(): int => 42; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var t: AliasExtTargetBound4070 := new ExtTargetBound4070(); "
        "    var f: () -> int := t.Val; "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// ── KNOWN-OPEN CONTROL for #4047 ─────────────────────────────────────────
//
// A CHAINED alias (outer declared textually BEFORE inner, inner naming the
// extension's target class) inherits #4047's resolution-order gap, exactly
// as fix #4051's own known-open control documents for its call chain:
// `Resolver::resolveTopDecl`'s single forward pass has not yet resolved the
// inner alias's `aliasedType` when the outer alias's own `aliasedType` (a
// reference to the inner alias) is resolved, so `resolveAliasToTypeSymbol`
// returns null for the OUTER alias and this fix has nothing to dispatch on.
// Measured actual (matching #4051's control exactly): the member lookup
// this depends on fails upstream, and the reference degrades through the
// #2434/#2491 backstops to a compiles-clean, runs-clean WRONG VALUE (0, not
// 42) — with two internal backstop diagnostics on stderr, not the SIGSEGV
// #4070 itself was filed against.
BEZEL_TEST(type_alias_extension_method_ref_4070,
           chained_alias_REVERSE_ORDER_ref_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasExtOuterRev4070 := AliasExtInnerRev4070; "
        "type AliasExtInnerRev4070 := ExtTargetRev4070; "
        "public class ExtTargetRev4070 "
        "  destructor end destructor; "
        "end ExtTargetRev4070; "
        "extension ExtTargetRev4070 "
        "  public method Val(): int => 42; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var f := AliasExtOuterRev4070.Val; "
        "    using var t := new ExtTargetRev4070(); "
        "    return f(t); "
        "  end Main; "
        "end C;");
    // KNOWN-OPEN (#4047): correct behavior is either the reference resolving
    // correctly (42) or a clean rejection; measured actual is the degraded 0,
    // inherited from the upstream ordering gap, not reintroduced by this fix.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
