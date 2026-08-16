// Spec-derived sema tests for fix #3107 — §25.1's not-overloadable list.
//
// Authority: §25.1, verbatim, in the not-overloadable list:
//
//   "Assignment and compound-assignment operators (`:=`, `+=`, `-=`, `*=`,
//   `/=`, `mod=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=`) — Bezel has no
//   compound-operator overload mechanism; the compound forms desugar
//   mechanically to read-modify-write using the underlying binary operator's
//   overload, with the evaluation counts and result typing of §6.6."
//
// Appendix C.10 carries it as a compiler-enforced invariant.
//
// The pre-fix implementation only listed the unary/null-aware/type-test/range
// tokens in `src/sema/TypeResolution_Checks.cpp`'s exclusion list, so
// `operator +=` / `operator mod=` / etc. declarations were silently accepted
// and the declared members were inert. The author then got a confusing
// "`V` declares no applicable `operator +` overload (§25.2)" message while
// staring at the `operator +=` they just wrote — the actual cost of the bug.

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

// §25.1 (#3107): a compound-assignment-overload diagnostic — the new
// diagnostic includes the "compound form composes from it automatically"
// remedy phrasing, which distinguishes it from the pre-existing
// `not`/`!`/`??`/`is`/`as`/`..`/`..<` exclusion message.
bool hasCompoundAssignError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("may not be overloaded") != std::string::npos &&
            d.message.find("25.1") != std::string::npos &&
            d.message.find("compound form composes from it automatically") !=
                std::string::npos)
            return true;
    return false;
}

const char* kRec = "module M; record V(n: int) ";

}  // namespace

// §25.1 (#3107): `operator +=` is rejected — the literal example from the
// issue body. Pre-fix this was silently accepted and the declared member
// was inert.
BEZEL_TEST(operator_compound_assign_3107, plus_equal_rejected) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +=(a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCompoundAssignError(r));
}

// §25.1 (#3107): `operator mod=` is rejected — the surfacing example from
// #3052's per-fix review that found the gap.
BEZEL_TEST(operator_compound_assign_3107, mod_equal_rejected) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator mod=(a: V, b: V): V => "
        "new V(a.n mod b.n); "
        "end V; class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCompoundAssignError(r));
}

// §25.1 (#3107): the full compound-assignment set — every token from the
// spec list is rejected. The twelve tokens are: `:=`, `+=`, `-=`, `*=`,
// `/=`, `mod=`, `&=`, `|=`, `^=`, `<<=`, `>>=`, `??=`. Pre-fix none of
// these reached the diagnostic.
BEZEL_TEST(operator_compound_assign_3107, all_compound_assign_tokens_rejected) {
    const char* tokens[] = {
        ":=", "+=", "-=", "*=", "/=", "mod=",
        "&=", "|=", "^=", "<<=", ">>=", "??=",
    };
    int accepted = 0;
    const char* firstAccepted = nullptr;
    for (const char* tok : tokens) {
        std::string src = std::string(kRec) +
            "  public static method operator " + tok + "(a: V, b: V): V => "
            "new V(a.n); "
            "end V; class F public static method Main(): int return 0; "
            "end Main; end F;";
        auto r = parseSemaAll(src);
        if (!hasCompoundAssignError(r)) {
            if (!firstAccepted) firstAccepted = tok;
            ++accepted;
        }
    }
    BEZEL_EXPECT_EQ(accepted, 0);
    if (accepted > 0) {
        std::cout << "    `operator " << firstAccepted
                  << "` was not rejected as not-overloadable (§25.1, #3107)"
                  << " (and " << (accepted - 1) << " other(s))\n";
    }
}

// §25.1 (#3107): the pointer-style shift compound `>>=` is also rejected —
// `>>=` is the same compound-assignment token set as the spec lists, and
// `>>` alone IS overloadable per §25.1's binary list. The pre-fix parser
// would accept `operator >>=` and the field would be inert; the author
// would then get a confusing "no `operator >>`" on a `v >>= n` use.
BEZEL_TEST(operator_compound_assign_3107, shift_right_assign_rejected) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator >>=(a: V, b: int): V => "
        "new V(a.n); "
        "end V; class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_TRUE(hasCompoundAssignError(r));
}

// §25.1 (#3107): the underlying binary operator (`+`) IS overloadable —
// this is the remedy the new diagnostic names. The compound form
// mechanically desugars to `v = v + rhs` once `operator +` is declared.
BEZEL_TEST(operator_compound_assign_3107, underlying_binary_operator_allowed) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCompoundAssignError(r));
}

// §25.1 (#3107): the underlying binary `mod` is overloadable — the
// compound `mod=` desugars to read-modify-write using `operator mod`.
BEZEL_TEST(operator_compound_assign_3107, underlying_mod_binary_allowed) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator mod(a: V, b: V): V => "
        "new V(a.n mod b.n); "
        "end V; class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCompoundAssignError(r));
}

// §25.1 (#3107): pre-fix, declaring `operator +=` was silently accepted
// and the inert member created noise without diagnostic. This test pins
// the silent-accept regression by asserting the post-fix counterexample:
// a clean `+` declaration (no `+=`) compiles with no §25.1 compound
// error.
BEZEL_TEST(operator_compound_assign_3107, no_compound_decl_no_error) {
    auto r = parseSemaAll(
        std::string(kRec) +
        "  public static method operator +(a: V, b: V): V => "
        "new V(a.n + b.n); "
        "end V; class F public static method Main(): int return 0; "
        "end Main; end F;");
    BEZEL_EXPECT_FALSE(hasCompoundAssignError(r));
}
