// Spec-derived sema tests for fix #746 (R41 cluster C) — comparison/equality
// operator-overload group completeness.
//
// Authority: §25.2 — "Comparison operators must be overloaded as a complete
// group: `<`, `>`, `<=`, `>=` together; `=` and `<>` together." A partial group
// was silently accepted, and then using a missing member (`v1 <= v2` with only
// `<`/`>` declared) fell through to a struct-level `icmp` → malformed IR. The
// fix rejects the incomplete group at the declaration.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasGroupError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("25.2") != std::string::npos &&
            (d.message.find("complete group") != std::string::npos ||
             d.message.find("overloaded together") != std::string::npos))
            return true;
    return false;
}

// §25.1: a forbidden-operator overload (e.g. `not`) diagnostic.
bool hasForbiddenError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("may not be overloaded") != std::string::npos &&
            d.message.find("25.1") != std::string::npos)
            return true;
    return false;
}

// §25.2: `=`/`<>` overloaded on a class diagnostic.
bool hasClassEqError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("may not be overloaded on a class") !=
                std::string::npos &&
            d.message.find("25.2") != std::string::npos)
            return true;
    return false;
}

// §25.2 (#1664, v0.184): a parameter-modifier-on-operator-parameter diagnostic.
bool hasParamModifierError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("may not carry the") != std::string::npos &&
            d.message.find("25.2") != std::string::npos)
            return true;
    return false;
}

// §8.3 (#1193): a type overrides `Equals` but not `GetHashCode` diagnostic.
bool hasEqHashPairingError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("must also override `GetHashCode`") !=
                std::string::npos &&
            d.message.find("8.3") != std::string::npos)
            return true;
    return false;
}

const char* kRec = "module M; record V(n: int) ";

}  // namespace

// §25.2: a partial comparison group (`<`, `>` without `<=`, `>=`) is rejected.
BEZEL_TEST(operator_groups_746, partial_comparison_rejected) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator <(a: V, b: V): bool => a.n < b.n; "
        "  public static method operator >(a: V, b: V): bool => a.n > b.n; "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasGroupError(r));
}

// §25.2: a partial equality group (`=` without `<>`) is rejected.
BEZEL_TEST(operator_groups_746, partial_equality_rejected) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator =(a: V, b: V): bool => a.n = b.n; "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasGroupError(r));
}

// §25.2: a complete comparison group is accepted.
BEZEL_TEST(operator_groups_746, complete_comparison_ok) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator <(a: V, b: V): bool => a.n < b.n; "
        "  public static method operator >(a: V, b: V): bool => a.n > b.n; "
        "  public static method operator <=(a: V, b: V): bool => a.n <= b.n; "
        "  public static method operator >=(a: V, b: V): bool => a.n >= b.n; "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasGroupError(r));
}

// §25.2: a complete equality group is accepted.
BEZEL_TEST(operator_groups_746, complete_equality_ok) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator =(a: V, b: V): bool => a.n = b.n; "
        "  public static method operator <>(a: V, b: V): bool => a.n <> b.n; "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasGroupError(r));
}

// §25.2: a non-grouped operator (`+`) alone is fine.
BEZEL_TEST(operator_groups_746, additive_operator_alone_ok) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(a: V, b: V): V => new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasGroupError(r));
}

// ---------------------------------------------------------------------------
// §25.2 (#1664, v0.184): "Operator parameters admit no parameter modifiers.
// `ref`, `out`, `owned`, and `scoped` on an operator's parameter list are
// rejected semantically — the grammar admits them through the shared `Param`
// production, the same over-admit-and-reject pattern as §22.10's primary
// constructors. Operands are received as plain borrows (or value copies)
// under their type kinds; the return slot's optional `owned` (grammar A.4)
// is the sole ownership annotation on operators." Verified pre-fix: `ref`
// fell through to a codegen IR-verification crash (by-ref ABI vs. by-value
// operator signature); `owned`/`out` were silently accepted with undefined
// semantics.
// ---------------------------------------------------------------------------

// §25.2: `ref` on an operator parameter is rejected.
BEZEL_TEST(operator_groups_746, ref_modifier_on_operator_param_rejected_1664) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(ref a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasParamModifierError(r));
}

// §25.2: `out` on an operator parameter is rejected.
BEZEL_TEST(operator_groups_746, out_modifier_on_operator_param_rejected_1664) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(out a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasParamModifierError(r));
}

// §25.2: `owned` on an operator parameter is rejected — `owned` is admitted
// only on an operator's return type, not its parameters.
BEZEL_TEST(operator_groups_746, owned_modifier_on_operator_param_rejected_1664) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(owned a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasParamModifierError(r));
}

// §25.2: `scoped` on an operator parameter is rejected.
BEZEL_TEST(operator_groups_746, scoped_modifier_on_operator_param_rejected_1664) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(scoped a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasParamModifierError(r));
}

// §25.2: a modifier-free operator parameter list is accepted — no false
// positive from the new check.
BEZEL_TEST(operator_groups_746, unmodified_operator_params_accepted_1664) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasParamModifierError(r));
}

// §25.2: `owned` on an operator's RETURN type (not a parameter) is unaffected
// — the return slot is the sole permitted ownership annotation on operators.
// The return type here is a CLASS: §28.1's type requirement (v0.193, #1797)
// admits `owned` only on a class/interface/bare-type-parameter return, so this
// intent ("owned on an operator return is permitted") must be exercised with a
// class-typed operator — the value-typed `record V` this test formerly used is
// now itself a §28.1 compile error, which would silently mask the §25.2
// property under test. The assertion still targets the §25.2 param-modifier
// diagnostic, which must be absent.
BEZEL_TEST(operator_groups_746, owned_return_type_unaffected_1664) {
    auto r = parseSemaAll(
        "module M; class V "
        "  public n: int; "
        "  public constructor(x: int) n := x; end constructor; "
        "  public static method operator +(a: V, b: V): owned V => "
        "    new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasParamModifierError(r));
}

// §25.1: `operator not` may not be overloaded — rejected.
BEZEL_TEST(operator_groups_746, operator_not_forbidden) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator not(a: V): bool => a.n = 0; "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasForbiddenError(r));
}

// §25.1: `operator ~` (bitwise complement) IS overloadable — no error.
BEZEL_TEST(operator_groups_746, operator_complement_allowed) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator ~(a: V): V => new V(0 - a.n); "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasForbiddenError(r));
}

// §25.2: a CLASS may not overload `=`/`<>` (it customizes via Equals).
BEZEL_TEST(operator_groups_746, class_equality_overload_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public n: int := 0; "
        "  public static method operator =(a: C, b: C): bool => a.n = b.n; "
        "  public static method operator <>(a: C, b: C): bool => a.n <> b.n; "
        "end C; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasClassEqError(r));
}

// §25.2: a RECORD may overload `=`/`<>` — no class restriction.
BEZEL_TEST(operator_groups_746, record_equality_overload_ok) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator =(a: V, b: V): bool => a.n = b.n; "
        "  public static method operator <>(a: V, b: V): bool => a.n <> b.n; "
        "end V; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasClassEqError(r));
}

// ---------------------------------------------------------------------------
// §8.3 (#1193): "A type that overrides `Equals` must also override
// `GetHashCode`. The compiler enforces this." The rule is one-directional, as
// the spec states it: overriding `Equals` requires `GetHashCode`; the converse
// (overriding only `GetHashCode`) is permitted. `Equals(T)` for `IEquatable<T>`
// is an interface implementation, not an `override`, and does not trigger.
// ---------------------------------------------------------------------------

// §8.3: a class overriding `Equals` but not `GetHashCode` is rejected.
BEZEL_TEST(operator_groups_746, equals_without_gethashcode_diagnosed_1193) {
    auto r = parseSemaAll(
        "module M; class K "
        "  public constructor() end constructor; "
        "  public override method Equals(o: Object?): bool => false; "
        "end K; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasEqHashPairingError(r));
}

// §8.3: a class overriding both `Equals` and `GetHashCode` is accepted.
BEZEL_TEST(operator_groups_746, equals_and_gethashcode_both_ok_1193) {
    auto r = parseSemaAll(
        "module M; class K "
        "  public constructor() end constructor; "
        "  public override method Equals(o: Object?): bool => false; "
        "  public override method GetHashCode(): int => 0; "
        "end K; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasEqHashPairingError(r));
}

// §8.3: overriding only `GetHashCode` is permitted (the rule is one-directional
// — overriding `Equals` is what requires the pair).
BEZEL_TEST(operator_groups_746, gethashcode_only_ok_1193) {
    auto r = parseSemaAll(
        "module M; class K "
        "  public constructor() end constructor; "
        "  public override method GetHashCode(): int => 0; "
        "end K; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasEqHashPairingError(r));
}

// §8.3: overriding an unrelated `Object` method (`ToString`) does not trigger.
BEZEL_TEST(operator_groups_746, tostring_override_only_ok_1193) {
    auto r = parseSemaAll(
        "module M; class K "
        "  public constructor() end constructor; "
        "  public override method ToString(): string => \"k\"; "
        "end K; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasEqHashPairingError(r));
}

// §8.3: the rule applies to records too — they override via the same `override`
// form (§9.2). `Equals` without `GetHashCode` is rejected.
BEZEL_TEST(operator_groups_746, record_equals_without_gethashcode_diagnosed_1193) {
    auto r = parseSemaAll(
        "module M; record R "
        "  public X: int; "
        "  public override method Equals(other: R): bool => false; "
        "end R; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_TRUE(hasEqHashPairingError(r));
}

// §8.3: an `IEquatable<T>` `Equals(T)` implementation (no `override`) paired with
// a `GetHashCode` override is not an `Equals(Object?)` override — no error.
BEZEL_TEST(operator_groups_746, iequatable_equals_t_not_an_override_1193) {
    auto r = parseSemaAll(
        "module M; class K : IEquatable<K> "
        "  public constructor() end constructor; "
        "  public method Equals(other: K?): bool => false; "
        "  public override method GetHashCode(): int => 0; "
        "end K; class F public static method Main(): int return 0; end Main; "
        "end F;");
    BEZEL_EXPECT_FALSE(hasEqHashPairingError(r));
}
