// Spec-derived regression test for issue #3313 — the SECOND MethodSymbol
// construction path in `src/sema/SemaContext.hpp` (the IComparable /
// IEquatable / IEqualityComparer / IReadOnlySpan synthesized builtin-
// interface family, ~line 1385) builds ParameterSymbols via the free
// helper `buildBuiltinInterfaceParamSymbol(SemaContext&, const ast::Param&)`,
// which (pre-#3313) wrote `name`, `modifier`, and `declType` but did NOT
// carry `ast::Param::outNullPostcond` through to `ps->outNullPostcond`.
// The parallel first path (~line 1216) DOES write it: `ps->outNullPostcond
// = sm.params[p].postcond;`. The seam-gap is the mirror-miss of #3308's
// modifier gap.
//
// SPEC GROUNDS — §22.4 "Inline `out` declarations" (v0.268): an `out`
// parameter may carry a `when true`/`when false` nullability
// postcondition (the §22.4 "OutNullPostcond" surface, A.5). The
// postcondition is consumed by the caller-side narrowing
// (`narrowFromOutPostcond`, in `src/sema/ExprTypecheck.cpp`). Both
// MethodSymbol-construction paths MUST populate
// `ParameterSymbol::outNullPostcond` from the source so the symbol
// surface is consistent. The four synthesized interfaces today carry no
// `when` clause on any parameter, so the value is `OutNullWhen::None`
// by default and the missing assignment has no observable effect today
// — but a future synthesized interface with a `when true`/`when false`
// clause would silently lose the postcondition through path #2.
//
// THE FIX adds `ps->outNullPostcond = param.outNullPostcond;` inside
// the helper, mirroring the parallel `ps->modifier = param.modifier;`
// added by #3308. The test seam is the helper itself — drive it
// directly with a constructed `ast::Param` carrying a non-None
// `outNullPostcond` (WhenTrue / WhenFalse) and assert the symbol
// carries it through. Pre-fix the helper would leave `ps->outNullPostcond`
// default-None (FAIL), post-fix it propagates the value (PASS).
//
// Companion tests cover:
//   T1 (FAIL-WITHOUT proof): helper with `outNullPostcond = WhenTrue`.
//   T2 (FAIL-WITHOUT proof): helper with `outNullPostcond = WhenFalse`.
//   T3: helper with `outNullPostcond = None` (the synthesized-interface
//       default; pins the consistent-for-empty case so any future
//       regression that flips the helper's default behavior is caught).
//   T4-T7: end-to-end on the four synthesized interfaces — pin the
//       post-fix contract that every synthesized-interface parameter
//       carries an explicit `OutNullWhen::None`. The synthesized
//       interfaces carry no spec-side postcondition, so the value
//       is None — but the ASSIGNMENT (per §22.4, v0.268) is what path
//       #2 skipped pre-#3313. Pinning the field is REACHABLE and
//       EXPLICITLY None pins the post-fix contract.
//   T8: control — IParsable.TryParse uses path #1 (already correct);
//       pins that the fix does NOT widen to path #1 (it doesn't touch
//       the path-1 loop body at all).

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace bezel::sema {
namespace {

// Find a synthesized MethodSymbol by name on a builtin-interface
// TypeSymbol. The cached entry is reused across calls inside the
// same SemaContext, so the construction happens at most once per
// test invocation.
const MethodSymbol* findMethodByName(const TypeSymbol* iface,
                                     std::string_view name) {
    if (!iface) return nullptr;
    for (Symbol* m : iface->members) {
        if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
            if (ms->name == name) return ms;
        }
    }
    return nullptr;
}

// Build an `ast::Param` for a primitive-named type with a specified
// out-nullability postcondition. The synthesized `ast::Param` that
// path #2 iterates via `rm.md->params[p]` has the same shape — a
// default-constructed object whose `outNullPostcond` is left at
// `OutNullWhen::None` by the synthesized-loop body. The test seam
// drives the helper with a non-None postcondition so the symbol-side
// assignment is observable.
std::unique_ptr<ast::Param> makeAstParam(std::string_view name,
                                         const char* typeName,
                                         ast::OutNullWhen postcond) {
    auto p = std::make_unique<ast::Param>();
    p->name = name;
    p->outNullPostcond = postcond;
    auto ty = std::make_unique<ast::Type>();
    ty->kind = ast::TypeKind::Named;
    ty->name = std::make_unique<ast::QualifiedName>();
    // fix #6469: `QualifiedName::parts` is a `vector<string_view>`
    // (src/ast/Node.hpp:42), so this used to construct a temporary std::string
    // and push a view into it — storage that died at the end of that
    // statement. It is the same defect the #6460 fix shipped and had to
    // rewrite (commit 978588d0): the view reads back as the EMPTY head name
    // through `TypeRef::headName()`
    // (src/sema/TypeRef.hpp:80) rather than crashing, so `exitCode == 0`
    // never sees it. The parameter is `const char*`, not `string_view`, so a
    // caller cannot hand in a temporary's bytes by accident: every call site
    // below passes a string literal, whose storage is static.
    ty->name->parts.push_back(typeName);
    p->type = std::move(ty);
    return p;
}

}  // namespace

// ─── T1 — FAIL-WITHOUT PROOF: WhenTrue postcondition propagates ─────
// The synthesized ast::Param that survives path #2's loop body carries
// `outNullPostcond = OutNullWhen::None` (the four builtin interfaces'
// shapes have no spec-side postcondition). Pre-fix the helper had no
// `ps->outNullPostcond = param.outNullPostcond;` assignment, so the
// field was default-init None — and THIS test would observe
// `ps->outNullPostcond == OutNullWhen::None` instead of the
// constructed `OutNullWhen::WhenTrue`. Post-fix the helper writes the
// postcondition through, and the test passes.
//
// This is the test that genuinely trips FAIL-WITHOUT.
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           ast_param_with_when_true_postcond_propagates_to_symbol_via_helper) {
    auto r = parseAndCollect(
        "module M3313T1; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    auto param = makeAstParam("n", "string", ast::OutNullWhen::WhenTrue);
    auto* ps = buildBuiltinInterfaceParamSymbol(*r.ctx, *param);
    BEZEL_EXPECT_TRUE(ps != nullptr);
    // Pre-fix: ps->outNullPostcond is default-None (FAIL).
    // Post-fix: ps->outNullPostcond == OutNullWhen::WhenTrue (PASS).
    BEZEL_EXPECT_EQ(static_cast<int>(ps->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::WhenTrue));
    BEZEL_EXPECT_EQ(std::string(ps->name), std::string("n"));
    BEZEL_EXPECT_TRUE(ps->declType != nullptr);
}

// ─── T2 — FAIL-WITHOUT PROOF: WhenFalse postcondition propagates ────
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           ast_param_with_when_false_postcond_propagates_to_symbol_via_helper) {
    auto r = parseAndCollect(
        "module M3313T2; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    auto param = makeAstParam("x", "string", ast::OutNullWhen::WhenFalse);
    auto* ps = buildBuiltinInterfaceParamSymbol(*r.ctx, *param);
    BEZEL_EXPECT_TRUE(ps != nullptr);
    BEZEL_EXPECT_EQ(static_cast<int>(ps->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::WhenFalse));
}

// ─── T3 — None postcondition is propagated (default case) ──────────
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           ast_param_with_none_postcond_remains_none_via_helper) {
    auto r = parseAndCollect(
        "module M3313T3; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    auto param = makeAstParam("v", "int", ast::OutNullWhen::None);
    auto* ps = buildBuiltinInterfaceParamSymbol(*r.ctx, *param);
    BEZEL_EXPECT_TRUE(ps != nullptr);
    BEZEL_EXPECT_EQ(static_cast<int>(ps->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::None));
}

// ─── T4 — IComparable<T>.CompareTo(T other) end-to-end ──────────────
// The `other` ParameterSymbol must carry an explicit `OutNullWhen::None`
// postcondition — the spec has no `when` postcondition for this
// parameter, so None is correct, but the ASSIGNMENT (per §22.4,
// v0.268) is what the second construction path skipped pre-#3313.
// Pinning the field is REACHABLE and EXPLICITLY None pins the
// post-fix contract.
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           builtin_comparable_compare_to_param_postcond_is_explicit_none) {
    auto r = parseAndCollect(
        "module M3313T4; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IComparable");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "CompareTo");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 1u);
    auto* other = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    BEZEL_EXPECT_TRUE(other != nullptr);
    BEZEL_EXPECT_EQ(static_cast<int>(other->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::None));
    BEZEL_EXPECT_EQ(std::string(other->name), std::string("other"));
    BEZEL_EXPECT_TRUE(other->declType != nullptr);
}

// ─── T5 — IEquatable<T>.Equals(T other) end-to-end ──────────────────
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           builtin_equatable_equals_param_postcond_is_explicit_none) {
    auto r = parseAndCollect(
        "module M3313T5; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IEquatable");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "Equals");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 1u);
    auto* other = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    BEZEL_EXPECT_TRUE(other != nullptr);
    BEZEL_EXPECT_EQ(static_cast<int>(other->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::None));
}

// ─── T6 — IEqualityComparer<T>.Equals(T a, T b) two-parameter shape ─
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           builtin_comparer_equals_two_params_each_postcond_is_explicit_none) {
    auto r = parseAndCollect(
        "module M3313T6; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IEqualityComparer");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "Equals");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 2u);
    auto* a = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    auto* b = dynamic_cast<const ParameterSymbol*>(ms->params[1]);
    BEZEL_EXPECT_TRUE(a != nullptr);
    BEZEL_EXPECT_TRUE(b != nullptr);
    BEZEL_EXPECT_EQ(static_cast<int>(a->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::None));
    BEZEL_EXPECT_EQ(static_cast<int>(b->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::None));
}

// ─── T7 — IReadOnlySpan<T>.SequenceEqual(T other) end-to-end ───────
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           builtin_readonly_span_sequence_equal_param_postcond_is_explicit_none) {
    auto r = parseAndCollect(
        "module M3313T7; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IReadOnlySpan");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "SequenceEqual");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 1u);
    auto* other = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    BEZEL_EXPECT_TRUE(other != nullptr);
    BEZEL_EXPECT_EQ(static_cast<int>(other->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::None));
}

// ─── T8 — CONTROL: IParsable<T>.TryParse uses path #1 (already correct) ─
// Path #1 already sets `ps->outNullPostcond = sm.params[p].postcond;` —
// pinning that path is still correct post-#3313 guards against any
// unintended widening of the fix that would have altered path #1.
//
// `IParsable<T>.TryParse(s: string, out value: T?): bool` is the §22.4
// shape-(a) Try — its `out value` parameter carries a `when true`
// nullability postcondition (stdlib §1 / #1585). Without it the §22.4
// caller narrowing never fires for the generic `T.TryParse` bridge.
BEZEL_TEST(method_symbol_param_out_null_postcond_3313,
           builtin_parsable_try_parse_out_param_postcond_is_when_true) {
    auto r = parseAndCollect(
        "module M3313T8; class C3313 "
        "  public static method Main(): int return 0; "
        "end C3313;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IParsable");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "TryParse");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 2u);
    auto* s    = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    auto* val  = dynamic_cast<const ParameterSymbol*>(ms->params[1]);
    BEZEL_EXPECT_TRUE(s != nullptr);
    BEZEL_EXPECT_TRUE(val != nullptr);
    // The `s: string` parameter carries no `when` postcondition.
    BEZEL_EXPECT_EQ(static_cast<int>(s->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::None));
    // The `out value: T?` parameter carries the shape-(a) `when true`
    // postcondition — this is the spec-faithful value path #1 has
    // always written. Pinning it here guards against any unintended
    // widening of the #3313 fix that would have altered path #1.
    BEZEL_EXPECT_EQ(static_cast<int>(val->outNullPostcond),
                    static_cast<int>(ast::OutNullWhen::WhenTrue));
}

}  // namespace bezel::sema