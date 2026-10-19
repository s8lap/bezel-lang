// Spec-derived regression test for issue #3308 — the SECOND MethodSymbol
// construction path in `src/sema/SemaContext.hpp` (the IComparable /
// IEquatable / IEqualityComparer / IReadOnlySpan synthesized builtin-
// interface family, ~line 1385) built ParameterSymbols with `name` and
// `declType` only, SKIPPING `ps->modifier = …;`. For every method produced
// by that path, `ParameterSymbol::modifier` was a default-constructed
// (empty) `string_view` rather than the explicit value the parallel first
// path (~line 1211) writes. #2641's `registerOutVarStringRelease` codegen
// fix had to install an `ast::MethodDecl`-side AST FALLBACK for this exact
// reason; closing the gap on the sema side restores the seam ratchet.
//
// SPEC GROUNDS — §22.3 "Parameter modifiers" (v0.268) lists `owned`,
// `using`, `ref`, `out`, `scoped`. §22.4 (v0.268) says "each parameter is
// one of {plain, owned, using, ref, out, out owned, scoped}". Both
// MethodSymbol-construction paths MUST populate `ParameterSymbol::modifier`
// from the source so downstream codegen reading the SYMBOL (rather than
// the AST) sees the modifier consistently. The synthesized builtin-
// interface methods today carry no modifier in the spec, so the value is
// empty — but the ASSIGNMENT makes the contract explicit and prepares any
// future synthesized interface with `out`/`ref`/`owned` parameters to
// flow through correctly.
//
// THE FIX centralizes the per-parameter construction in a free helper
// `buildBuiltinInterfaceParamSymbol(SemaContext&, const ast::Param&)` that
// path #2 calls. The helper is the test seam: drive it directly with a
// constructed `ast::Param` carrying a non-empty `modifier = "out"` and
// assert the symbol carries it through. Pre-fix, the helper would leave
// `ps->modifier` default-empty (FAIL). Post-fix, the helper propagates
// `param.modifier` (PASS).
//
// Companion tests cover the four synthesized interfaces end-to-end
// (calling `builtinInterfaceSym` and inspecting the synthesized
// `MethodSymbol::params`), plus a path-1 control that pins the still-
// correct `out` modifier on `IParsable.TryParse`. The synthesized-method
// tests are not FAIL-WITHOUT on their own (the four interfaces' params
// carry no spec-side modifier, so empty == empty), but they pin the
// post-fix contract so any future regression in the helper is caught
// immediately rather than only showing up via codegen.

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

// Force the second construction path by minting the synthesized
// `IComparable<T>` TypeSymbol. The cached entry is reused across calls
// inside the same SemaContext, so the construction happens at most once
// per test invocation.
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

const PropertySymbol* findPropertyByName(const TypeSymbol* iface,
                                         std::string_view name) {
    if (!iface) return nullptr;
    for (Symbol* m : iface->members) {
        if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
            if (ps->name == name) return ps;
        }
    }
    return nullptr;
}

// Build an `ast::Param` for a primitive-named type. The synthesized
// `ast::Param` that path #2 iterates via `rm.md->params[p]` has the same
// shape — a default-constructed object whose `modifier` is left empty by
// the synthesized-loop body. The test seam drives the helper with a
// non-empty `modifier` so the symbol-side assignment is observable.
std::unique_ptr<ast::Param> makeAstParam(std::string_view name,
                                         const char* typeName,
                                         std::string_view modifier) {
    auto p = std::make_unique<ast::Param>();
    p->name = name;
    p->modifier = modifier;
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

// ─── T1 — FAIL-WITHOUT PROOF ─────────────────────────────────────────────
// The synthesized ast::Param that survives path #2's loop body carries
// no modifier (the four builtin interfaces' shapes have no spec-side
// modifier). Pre-fix the helper had no `ps->modifier = param.modifier;`
// assignment, so the field was default-init empty — and THIS test would
// observe `ps->modifier == ""` instead of the constructed `"out"`. Post-
// fix the helper writes the modifier through, and the test passes.
//
// This is the only test in this file that genuinely trips FAIL-WITHOUT.
// The synthesized-interface tests below (T2–T5) are contract-pin regression
// nets — they pass both pre- and post-fix (empty == empty today) and
// catch any future refactor that breaks the helper for the empty case.
BEZEL_TEST(method_symbol_param_modifier_3308,
           ast_param_with_out_modifier_propagates_to_symbol_via_helper) {
    auto r = parseAndCollect(
        "module M3308T1; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    auto param = makeAstParam("n", "string", "out");
    auto* ps = buildBuiltinInterfaceParamSymbol(*r.ctx, *param);
    BEZEL_EXPECT_TRUE(ps != nullptr);
    // Pre-fix: ps->modifier is default-empty (FAIL).
    // Post-fix: ps->modifier == "out" (PASS).
    BEZEL_EXPECT_EQ(std::string(ps->modifier), std::string("out"));
    BEZEL_EXPECT_EQ(std::string(ps->name),     std::string("n"));
    BEZEL_EXPECT_TRUE(ps->declType != nullptr);
}

// ─── T2 — `ref` modifier is propagated too ──────────────────────────────
BEZEL_TEST(method_symbol_param_modifier_3308,
           ast_param_with_ref_modifier_propagates_to_symbol_via_helper) {
    auto r = parseAndCollect(
        "module M3308T2; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    auto param = makeAstParam("x", "int", "ref");
    auto* ps = buildBuiltinInterfaceParamSymbol(*r.ctx, *param);
    BEZEL_EXPECT_TRUE(ps != nullptr);
    BEZEL_EXPECT_EQ(std::string(ps->modifier), std::string("ref"));
}

// ─── T3 — empty modifier is propagated (whatever the source has) ────────
BEZEL_TEST(method_symbol_param_modifier_3308,
           ast_param_with_default_modifier_remains_empty_via_helper) {
    auto r = parseAndCollect(
        "module M3308T3; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    auto param = makeAstParam("v", "int", "");
    auto* ps = buildBuiltinInterfaceParamSymbol(*r.ctx, *param);
    BEZEL_EXPECT_TRUE(ps != nullptr);
    BEZEL_EXPECT_EQ(std::string(ps->modifier), std::string(""));
}

// ─── T4 — IComparable<T>.CompareTo(T other) end-to-end ─────────────────
// The `other` ParameterSymbol must carry an explicit (empty) modifier —
// the spec has no modifier for this parameter, so the empty value is
// correct, but the ASSIGNMENT (per §22.3/§22.4, v0.268) is what the
// second construction path skipped pre-#3308. Pinning the field is
// REACHABLE and EXPLICITLY empty pins the post-fix contract.
BEZEL_TEST(method_symbol_param_modifier_3308,
           builtin_comparable_compare_to_param_modifier_is_explicit_empty) {
    auto r = parseAndCollect(
        "module M3308T4; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IComparable");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "CompareTo");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 1u);
    auto* other = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    BEZEL_EXPECT_TRUE(other != nullptr);
    BEZEL_EXPECT_EQ(std::string(other->modifier), std::string(""));
    BEZEL_EXPECT_EQ(std::string(other->name), std::string("other"));
    BEZEL_EXPECT_TRUE(other->declType != nullptr);
}

// ─── T5 — IEquatable<T>.Equals(T other) end-to-end ──────────────────────
BEZEL_TEST(method_symbol_param_modifier_3308,
           builtin_equatable_equals_param_modifier_is_explicit_empty) {
    auto r = parseAndCollect(
        "module M3308T5; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IEquatable");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "Equals");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 1u);
    auto* other = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    BEZEL_EXPECT_TRUE(other != nullptr);
    BEZEL_EXPECT_EQ(std::string(other->modifier), std::string(""));
}

// ─── T6 — IEqualityComparer<T>.Equals(T a, T b) two-parameter shape ─────
BEZEL_TEST(method_symbol_param_modifier_3308,
           builtin_comparer_equals_two_params_each_modifier_is_explicit_empty) {
    auto r = parseAndCollect(
        "module M3308T6; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IEqualityComparer");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "Equals");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 2u);
    auto* a = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    auto* b = dynamic_cast<const ParameterSymbol*>(ms->params[1]);
    BEZEL_EXPECT_TRUE(a != nullptr);
    BEZEL_EXPECT_TRUE(b != nullptr);
    BEZEL_EXPECT_EQ(std::string(a->modifier), std::string(""));
    BEZEL_EXPECT_EQ(std::string(b->modifier), std::string(""));
}

// ─── T7 — IReadOnlySpan<T>.SequenceEqual(T other) end-to-end ───────────
BEZEL_TEST(method_symbol_param_modifier_3308,
           builtin_readonly_span_sequence_equal_param_modifier_is_explicit_empty) {
    auto r = parseAndCollect(
        "module M3308T7; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IReadOnlySpan");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "SequenceEqual");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 1u);
    auto* other = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    BEZEL_EXPECT_TRUE(other != nullptr);
    BEZEL_EXPECT_EQ(std::string(other->modifier), std::string(""));
}

// ─── T8 — IReadOnlySpan indexer's `i: int` parameter ───────────────────
BEZEL_TEST(method_symbol_param_modifier_3308,
           builtin_readonly_span_indexer_param_modifier_is_explicit_empty) {
    auto r = parseAndCollect(
        "module M3308T8; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IReadOnlySpan");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const PropertySymbol* ps = findPropertyByName(iface, std::string_view());
    BEZEL_EXPECT_TRUE(ps != nullptr);
    BEZEL_EXPECT_TRUE(ps->isIndexer);
    BEZEL_EXPECT_EQ(ps->indexerParams.size(), 1u);
    auto* ip = dynamic_cast<const ParameterSymbol*>(ps->indexerParams[0]);
    BEZEL_EXPECT_TRUE(ip != nullptr);
    BEZEL_EXPECT_EQ(std::string(ip->modifier), std::string(""));
}

// ─── T9 — CONTROL: IParsable<T>.TryParse uses path #1 (already correct) ─
// Path #1 already sets `ps->modifier = sm.params[p].mod;` — pinning that
// path is still correct post-#3308 guards against any unintended
// widening of the fix that would have altered path #1.
BEZEL_TEST(method_symbol_param_modifier_3308,
           builtin_parsable_try_parse_out_param_modifier_is_out) {
    auto r = parseAndCollect(
        "module M3308T9; class C3308 "
        "  public static method Main(): int return 0; "
        "end C3308;");
    const TypeSymbol* iface = r.ctx->builtinInterfaceSym("IParsable");
    BEZEL_EXPECT_TRUE(iface != nullptr);
    const MethodSymbol* ms = findMethodByName(iface, "TryParse");
    BEZEL_EXPECT_TRUE(ms != nullptr);
    BEZEL_EXPECT_EQ(ms->params.size(), 2u);
    auto* s    = dynamic_cast<const ParameterSymbol*>(ms->params[0]);
    auto* val  = dynamic_cast<const ParameterSymbol*>(ms->params[1]);
    BEZEL_EXPECT_TRUE(s != nullptr);
    BEZEL_EXPECT_TRUE(val != nullptr);
    BEZEL_EXPECT_EQ(std::string(s->modifier),   std::string(""));
    BEZEL_EXPECT_EQ(std::string(val->modifier), std::string("out"));
}

}  // namespace bezel::sema
