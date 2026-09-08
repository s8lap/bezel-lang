// Spec-derived regression tests for issue #4071 — §5.1 / §17.14 / §27.8
// (v0.294): the qualified-const narrowing fold (`foldIntConstExprTC`,
// src/sema/ExprTypecheck.cpp) did not see through a `type` alias of the
// class declaring the const, so `var b: byte := AliasK4052.V;` was REJECTED
// (the §5.1 narrowing diagnostic fired with "not implicitly convertible")
// rather than folding `AliasK4052.V` to 5 and accepting the narrowing —
// the same shape every other sibling in the
// #4038/#4048/#4050/#4051/#4070/#4076 alias family closed.
//
// The direct `K4052.V` spelling folds to 5 and is accepted; the alias
// spelling now does the same.
//
// `ExprTC::foldIntConstExprTC`'s qualified-name arm resolves the head via
// `if (Symbol* head = lookupName(id->name)) t = dynamic_cast<TypeSymbol*>(head);`
// with no `TypeAliasSymbol` arm — the identical missing-arm shape
// #4038/#4048/#4050/#4051/#4070 all fixed in their own call chains, with
// no TypeAliasSymbol arm here. The function's own comment claims the
// decline path is safe ("return nullopt and fall back to the historical
// narrowing diagnostic so no false accept is lifted"), but the MEASURED
// behavior contradicts that — the narrowing produces a wrong diagnostic
// when the alias form is used.
//
// Fix: resolve the alias through the shared
// `sema::resolveAliasToTypeSymbol` helper (TypeResolution.hpp,
// #4050/#4051) so an alias-qualified qualified-const reference folds
// exactly like a direct-name one. Pure sema-layer fix (codegen does not
// re-resolve qualified-const heads; #4050's DestructuringPattern finding
// showed codegen needs its own fix only when codegen independently
// re-reads the name).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// #4071 issue repro, verbatim (§5.1 / §17.14): `var b: byte := AliasK4052.V;`
// where `AliasK4052 := K4052` and `K4052.V : int := 5`. The fold now
// resolves AliasK4052 through to K4052, sees V = 5, and the §5.1 narrowing
// admits the byte assignment.
// ---------------------------------------------------------------------------

// The issue repro: the alias form must fold and narrow just like the
// direct form — return 5 to confirm the byte held 5 (not 0).
BEZEL_TEST(type_alias_qualified_const_fold_4071,
           alias_qualified_const_folds_and_narrows_to_byte) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasK4071 := K4071; "
        "class K4071 "
        "  public const V: int := 5; "
        "end K4071; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := AliasK4071.V; "
        "    return int(b); "
        "  end Main; "
        "end C;"), 5);
}

// Anti-regression control — the DIRECT (non-alias) form must still fold
// and narrow identically. Pins the fix's new resolution arm did not
// disturb the existing TypeSymbol arm's behavior.
BEZEL_TEST(type_alias_qualified_const_fold_4071,
           direct_qualified_const_still_folds_and_narrows) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class K4071direct "
        "  public const V: int := 5; "
        "end K4071direct; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := K4071direct.V; "
        "    return int(b); "
        "  end Main; "
        "end C;"), 5);
}

// `byte` → `byte` is a vacuous narrowing (the source type IS byte), but
// the fold's qualified-name arm still exercises on it — pin that the
// alias path runs the same code path as the direct path on this shape.
BEZEL_TEST(type_alias_qualified_const_fold_4071,
           alias_byte_const_vacuous_narrow_still_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasK4071b := K4071b; "
        "class K4071b "
        "  public const V: byte := 7; "
        "end K4071b; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := AliasK4071b.V; "
        "    return int(b); "
        "  end Main; "
        "end C;"), 7);
}

// Negative-fold: `var b: byte := AliasK4071big.V` where V = 1000 cannot
// narrow — must still be REJECTED identically through the alias form.
// Pins the fix does NOT lift acceptance past §5.1's range guard: the
// alias path produces the same diagnostic as the direct path.
BEZEL_TEST(type_alias_qualified_const_fold_4071,
           alias_out_of_range_qualified_const_still_rejected) {
    auto r = runAot(
        "module M; "
        "type AliasK4071big := K4071big; "
        "class K4071big "
        "  public const V: int := 1000; "
        "end K4071big; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := AliasK4071big.V; "
        "    return int(b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── KNOWN-OPEN CONTROL for #4047 ─────────────────────────────────────────
//
// A CHAINED alias (outer declared textually BEFORE inner) inherits
// #4047's resolution-order gap: `Resolver::resolveTopDecl`'s single
// forward pass has not yet resolved the inner alias's `aliasedType`
// when the outer alias's own `aliasedType` (a reference to the inner
// alias) is resolved, so `sema::resolveAliasToTypeSymbol` returns null
// and #4071's fix has nothing to dispatch on — the narrowing fires its
// §5.1 diagnostic for both spelling forms of the chained alias. A
// DIRECT (non-chained) alias declared before its target class is
// UNAFFECTED (the target's TypeSymbol exists from symbol collection
// regardless of declaration order; only a TypeAliasSymbol needs its OWN
// `aliasedType` resolved first) — confirmed separately, not itself a
// #4047 case, and not pinned here.
BEZEL_TEST(type_alias_qualified_const_fold_4071,
           chained_alias_REVERSE_ORDER_const_fold_known_open_4047) {
    auto r = runAot(
        "module M; "
        "type AliasOuter4071 := AliasInner4071; "
        "type AliasInner4071 := K4071rev; "
        "class K4071rev "
        "  public const V: int := 5; "
        "end K4071rev; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := AliasOuter4071.V; "
        "    return int(b); "
        "  end Main; "
        "end C;");
    // KNOWN-OPEN (#4047): correct behavior is the alias-form fold to 5
    // (matching the inner-before-outer order this fix admits); measured
    // actual is the §5.1 narrowing diagnostic — the inherited
    // pre-fix symptom from the upstream ordering gap, not
    // reintroduced by this fix.
    BEZEL_EXPECT_FALSE(r.ran);
}