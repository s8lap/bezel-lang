// Spec-derived sema tests for fix #1575 — ordering operators on nullable
// `string?` operands.
//
// Authority: §4.6 (v0.182) — "Ordering requires non-nullable operands ...
// an operand of static type `string?` is a compile error — narrow first
// (§7.4) — the same use-requires-narrowing discipline as every other
// operation on a nullable reference; equality stays null-safe per §8.4, and
// §7.11's relational lifting is a nullable-*value*-type rule that does not
// extend here."
//
// Before the fix, `string? < string?` (and `<=`/`>`/`>=`) type-checked
// successfully and then dereferenced a null operand in the built-in ordinal-
// compare codegen, segfaulting at runtime. These tests verify:
//   - unnarrowed `string? < string?` (and the other three ordering ops) is
//     rejected at compile time with a §4.6-citing diagnostic;
//   - non-nullable `string < string` ordering keeps compiling (§4.6 base
//     case, unaffected);
//   - `string? = string?` / `string? <> string?` stay legal — equality is
//     null-safe per §8.4 and this fix must not touch it;
//   - a `string?` operand narrowed first (`<> null` guard) is accepted, so
//     the fix does not regress §7.4's narrow-then-use idiom.

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

// True iff some diagnostic is the §4.6 nullable-string-ordering rejection.
bool hasNullableStringOrderingError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("requires non-nullable `string` operands") !=
                std::string::npos &&
            d.message.find("4.6") != std::string::npos)
            return true;
    return false;
}

constexpr const char* kMainPrefix =
    "module M; class C public static method Main(): int ";
constexpr const char* kMainSuffix = " end Main; end C;";

}  // namespace

// §4.6 (#1575): `<` on unnarrowed `string?` operands is a compile error.
BEZEL_TEST(string_ordering_nullable_1575, lt_rejected_unnarrowed) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := null; var b: string? := \"x\"; "
        "if a < b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableStringOrderingError(r));
}

// §4.6 (#1575): `<=`, `>`, `>=` on unnarrowed `string?` operands are likewise
// rejected — the rule covers the whole ordering group, not just `<`.
BEZEL_TEST(string_ordering_nullable_1575, le_rejected_unnarrowed) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := null; var b: string? := \"x\"; "
        "if a <= b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableStringOrderingError(r));
}

BEZEL_TEST(string_ordering_nullable_1575, gt_rejected_unnarrowed) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := null; var b: string? := \"x\"; "
        "if a > b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableStringOrderingError(r));
}

BEZEL_TEST(string_ordering_nullable_1575, ge_rejected_unnarrowed) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := null; var b: string? := \"x\"; "
        "if a >= b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableStringOrderingError(r));
}

// §4.6 (#1575): a mixed nullable/non-nullable pair (`string?` vs `string`) is
// rejected too — only ONE operand needs to be nullable to require narrowing.
BEZEL_TEST(string_ordering_nullable_1575, mixed_nullable_operand_rejected) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := null; var b: string := \"x\"; "
        "if a < b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableStringOrderingError(r));
}

// §4.6 base case: non-nullable `string < string` keeps compiling — the fix
// only gates nullable operands.
BEZEL_TEST(string_ordering_nullable_1575, non_nullable_ordering_accepted) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string := \"a\"; var b: string := \"b\"; "
        "if a < b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.4 (#1575): `=` on `string?` stays legal and null-safe — this fix must
// not touch equality, only the four ordering operators.
BEZEL_TEST(string_ordering_nullable_1575, nullable_equality_accepted) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := null; var b: string? := \"x\"; "
        "if a = b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.4 (#1575): `<>` on `string?` stays legal too.
BEZEL_TEST(string_ordering_nullable_1575, nullable_inequality_accepted) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := null; var b: string? := \"x\"; "
        "if a <> b then return 1; end; return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1575): a `string?` operand narrowed first (`<> null` guard) is
// accepted — the fix must not regress the narrow-then-use idiom that §4.6
// explicitly points callers toward.
BEZEL_TEST(string_ordering_nullable_1575, narrowed_operand_accepted) {
    auto r = parseSemaAll(
        std::string(kMainPrefix) +
        "var a: string? := \"a\"; var b: string? := \"b\"; "
        "if a <> null and b <> null then "
        "  if a < b then return 1; end; "
        "end; "
        "return 0;" +
        kMainSuffix);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
