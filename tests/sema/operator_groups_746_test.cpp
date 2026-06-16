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
