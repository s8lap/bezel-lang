// Regression test for issue #6344 -- the §27.8 alias carve-out in
// `overloadSigTypeRefEqual` was ONE-SIDED, so an overload pair's verdict
// depended on the ORDER the two overloads were declared in.
// Spec: docs/Bezel_Language_Spec.md, v0.338.
//
// THE GOVERNING RULE (§27.8, §22.6, §11.3).
//   §27.8 closing sentence: "Type aliases — generic or not — see through to
//   the aliased type for identity, equality, method dispatch, and overload
//   resolution. They are a naming convenience, not a separate type kind."
//   The see-through is SYMMETRIC — it is a property of the TYPE, not of a
//   position in a comparison.
//   §22.6: "overloads differing in any of these [parameter types] are
//   distinguishable."
//   §11.3: two members clash only when they "do not differ in number of
//   parameters, number of type parameters of their own, parameter types, or
//   the `ref`/`out` modifiers in a participating position".
// So `Take(x: Id<V>)` beside `Take(x: W)` (with `type Id<T> := T`, so
// `Id<V>` IS `V`, and `W` is a DIFFERENT class type parameter) is a legal
// pair of overloads — and it must be legal in BOTH declaration orders.
//
// ROOT CAUSE (MEASURED). `overloadSigTypeRefEqual`'s Named arm carried:
//     if (!a->declaration && !a->aliasResolvedHead.empty()) {
//         // §27.8: arrived through an alias — skip the head compare
//     } else if (!a->declaration) { /* positional + head compare */ }
// `a->aliasResolvedHead` is consulted on ONE side only. When `a` is the
// alias-spelled ref the head compare is skipped and the pair falls through
// to the blind structural compare (→ indistinguishable → clash); when `a`
// is the plain ref the head compare runs and correctly separates them.
// Pre-fix, on this base, BOTH orders rejected for some pairs and only one
// order rejected for others — the asymmetry is the defect.
//
// The guard's own comment justified it by claiming `headName()` "would
// compare `Id<V>`'s alias spelling `Id` against a bare `V`". That is
// FALSE: `headName()` returns `aliasResolvedHead` when set, so `Id<V>`'s
// head reads as `V`. The compare was already alias-transparent on BOTH
// sides; the guard only suppressed it on one. Dropping the guard therefore
// changes a verdict ONLY where `aliasResolvedHead` is non-empty — exactly
// where `headName()` was already answering with the underlying head.
//
// NOT a regression: the base tree rejects BOTH orders of the issue's pair.
// #6335 (2c0e9795/dc9151af) fixed the alias-spelled-on-`a` side and left
// the mirror side rejecting, which is how the asymmetry surfaced.
//
// FAIL-WITHOUT. Every ACCEPT row is rejected on the pre-fix compiler; every
// REJECT row (the same-type pairs §27.8 requires to clash) passes on it
// too. The matrix below is stated in BOTH orders on purpose: a one-sided
// predicate paired with an order-insensitive test is precisely how this
// shipped unnoticed.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// `Id<T> := T`, `Id2<T> := Id<T>` and `Wrp<T> := Id<T>` exercise a direct
// alias, a 2-deep chain, and a 3-deep chain. The class supplies four
// DISTINCT class type parameters (V, X, Y, Z) so a row that wants two
// different bare types has them; none of these names collides with an alias.
std::string prog(const std::string& a, const std::string& b) {
    return "module M; "
           "type Id<T> := T; "
           "type Id2<T> := Id<T>; "
           "type Wrp<T> := Id<T>; "
           "class C<V, X, Y, Z> "
           "  private static method Take(x: " + a + "): int => 1; "
           "  private static method Take(x: " + b + "): int => 2; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// The §11.3 shape: each overload declares ONE type parameter OF ITS OWN,
// which is what the positional clause speaks of.
std::string methodProg(const std::string& a, const std::string& b) {
    return "module M; "
           "type Id<T> := T; "
           "type Id2<T> := Id<T>; "
           "class C<V, X, Y, Z> "
           "  private static method Take<P>(x: " + a + "): int => 1; "
           "  private static method Take<P>(x: " + b + "): int => 2; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// Aliases of DIFFERENT built-in instantiations: §12.9 separates a collection
// from the interface it conforms to (`public class Set<T>: ICollection<T>`).
std::string builtinProg(const std::string& a, const std::string& b) {
    return "module M; "
           "type Arr<T> := Array<T>; "
           "type St<T> := Set<T>; "
           "type En<T> := IEnumerable<T>; "
           "class C<V, X, Y, Z> "
           "  private static method Take(x: " + a + "): int => 1; "
           "  private static method Take(x: " + b + "): int => 2; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// The same shape but each overload names its OWN type parameter differently
// (P and Q) -- §11.3's verbatim example, `M<T>(x: T)` beside `M<U>(x: U)`.
// `a` is written in `Take<P>`'s scope and `b` in `Take<Q>`'s, so a caller
// asking for the (P, Q) order passes (P-spelling, Q-spelling) and the
// mirror asks for the (Q, P) order -- hence the two helpers.
std::string methodProgPQ(const std::string& p, const std::string& q) {
    return "module M; "
           "type Id<T> := T; "
           "type Id2<T> := Id<T>; "
           "class C<V, X, Y, Z> "
           "  private static method Take<P>(x: " + p + "): int => 1; "
           "  private static method Take<Q>(x: " + q + "): int => 2; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// The mirror declaration order: `Take<Q>` declared first.
std::string methodProgQP(const std::string& q, const std::string& p) {
    return "module M; "
           "type Id<T> := T; "
           "type Id2<T> := Id<T>; "
           "class C<V, X, Y, Z> "
           "  private static method Take<Q>(x: " + q + "): int => 1; "
           "  private static method Take<P>(x: " + p + "): int => 2; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

void expectAccepted(const std::string& p) {
    auto r = runAotChecked(p);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

void expectIndistinguishable(const std::string& p) {
    auto r = runAotChecked(p);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "is indistinguishable from another overload"));
}

}  // namespace

// -- THE ISSUE'S PAIR, IN BOTH ORDERS (§27.8, §22.6, §11.3).
//
// `Id<V>` is `V`; `Z` is a different class type parameter. The overloads are
// distinguishable, so this must be legal however the two are ordered. On the
// pre-fix compiler `Id<V>`-first CLASHED while `Z`-first was ACCEPTED.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           issue_repro_both_declaration_orders_spec_27_8) {
    expectAccepted(prog("Id<V>", "Z"));
    expectAccepted(prog("Z", "Id<V>"));
}

// -- The same asymmetry one and two alias hops down (§27.8 "Aliases of
// aliases are permitted"). Each hop must behave identically in both orders.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           chained_alias_both_orders_spec_27_8) {
    expectAccepted(prog("Id2<V>", "Z"));
    expectAccepted(prog("Z", "Id2<V>"));
    expectAccepted(prog("Wrp<V>", "Z"));
    expectAccepted(prog("Z", "Wrp<V>"));
}

// -- Two DIFFERENT types, each reached through an alias. Pre-fix this
// clashed in one order and was accepted in the other; §22.6 separates them
// by parameter type in both.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           two_distinct_alias_spellings_both_orders_spec_22_6) {
    expectAccepted(prog("Id<V>", "Id<X>"));
    expectAccepted(prog("Id<X>", "Id<V>"));
}

// -- Aliases of genuinely different built-in instantiations (§12.9). These
// are NOT type-parameter rows: two different collection types, each named
// through an alias.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           builtin_alias_pair_both_orders_spec_12_9) {
    expectAccepted(builtinProg("Arr<V>", "St<V>"));
    expectAccepted(builtinProg("St<V>", "Arr<V>"));
    expectAccepted(builtinProg("Arr<V>", "En<V>"));
    expectAccepted(builtinProg("En<V>", "Arr<V>"));
}

// -- The §11.3 method-generic shape: `Id<P>` is `P`, so against a bare `X`
// (a class type parameter, outside §11.3's "type parameters of their own")
// the pair is distinguishable — in both orders.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           method_generic_alias_both_orders_spec_11_3) {
    expectAccepted(methodProg("Id<P>", "X"));
    expectAccepted(methodProg("X", "Id<P>"));
}

// -- REJECT: the SAME type written two ways still clashes, in BOTH orders.
// This is §27.8's see-through doing its job, and it is the boundary the fix
// must not cross in either direction.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           same_type_two_spellings_clash_both_orders_spec_27_8) {
    expectIndistinguishable(prog("Id<V>", "V"));
    expectIndistinguishable(prog("V", "Id<V>"));
    expectIndistinguishable(prog("Id<V>", "Id2<V>"));
    expectIndistinguishable(prog("Id2<V>", "Id<V>"));
    expectIndistinguishable(prog("Id<V>", "Id<V>"));
    expectIndistinguishable(prog("Id<V>", "Wrp<V>"));
    expectIndistinguishable(prog("Wrp<V>", "Id<V>"));
}

// -- REJECT: §11.3's positional clause — a method's OWN type parameter is
// matched by POSITION, never by spelling, so `Take<P>(x: Id<P>)` and
// `Take<P>(x: P)` are one signature and still clash, in both orders.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           method_own_type_param_positional_still_clashes_spec_11_3) {
    expectIndistinguishable(methodProg("Id<P>", "P"));
    expectIndistinguishable(methodProg("P", "Id<P>"));
    expectIndistinguishable(methodProg("Array<Id<P>>", "Array<P>"));
    expectIndistinguishable(methodProg("Array<P>", "Array<Id<P>>"));
}

// -- REJECT: two methods each declaring one type parameter of its own, at
// the SAME position, clash even under different spellings (§11.3's
// verbatim example, `M<T>(x: T)` beside `M<U>(x: U)`).
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           positional_correspondence_still_clashes_spec_11_3) {
    expectIndistinguishable(methodProg("P", "P"));
    expectIndistinguishable(methodProg("Id<P>", "Id<P>"));
    // `M<T>(x: T)` beside `M<U>(x: U)` -- each method's own parameter at
    // POSITION 0, spelled differently (and, through an alias, the same
    // spelling seen from each side). Both declaration orders.
    expectIndistinguishable(methodProgPQ("P", "Q"));
    expectIndistinguishable(methodProgQP("Q", "P"));
    expectIndistinguishable(methodProgPQ("Id<P>", "Id<Q>"));
    expectIndistinguishable(methodProgQP("Id<Q>", "Id<P>"));
    expectIndistinguishable(methodProgPQ("Array<P>", "Array<Q>"));
    expectIndistinguishable(methodProgQP("Array<Q>", "Array<P>"));
}

// -- §11.3's positional clause has a boundary the fix must not cross: the
// clause speaks of "type parameters of their OWN", so a method's own
// parameter `P` (via `Id<P>`) against the CLASS's `X` is two different
// types and the pair is legal -- in both declaration orders.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           positional_clause_does_not_cover_class_type_params_spec_11_3) {
    expectAccepted(methodProgPQ("Id<P>", "X"));
    expectAccepted(methodProgQP("X", "Id<P>"));
}

// -- REJECT: two IDENTICAL built-in instantiations behind two different
// alias spellings are one type and must still clash — the §27.8 rows the
// carve-out was originally introduced to protect.
BEZEL_TEST(sema_overload_alias_see_through_symmetric_6344,
           builtin_alias_same_instantiation_still_clashes_spec_27_8) {
    expectIndistinguishable(builtinProg("Arr<V>", "Arr<V>"));
}