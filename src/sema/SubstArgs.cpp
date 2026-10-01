// Single authority for eager generic substitution over a TypeRef. See
// SubstArgs.hpp for the walker contract; per §14.1, §27.8, v0.298.
//
// Closes #4212 (Family I1 anchor). Replaces:
//   - Resolver::applyGenericSubst              (src/sema/TypeResolution_Checks.cpp, pre-fix #4188)
//   - Resolver::substituteAliasGenericArgs     (src/sema/TypeResolution_Names.cpp)
//   - ExprTC::substituteGenericFieldType       (src/sema/ExprTypecheck.cpp)

#include "sema/SubstArgs.hpp"

#include "ast/Node.hpp"
#include "support/Diagnostic.hpp"

#include <string_view>

namespace bezel::sema {

namespace {

// §15.1 (#953): "an inline array may not be the element of another inline
// array (`[N][M]T`)". Shared text for the direct-syntax check in
// resolveType's InlineArray case and the alias-substitution re-validation
// here — one spelling, not a drifting second copy (LANE_RULES §6).
void reportNestedInlineArrayError(SemaContext& ctx, SourceLocation loc) {
    ctx.diagnostics().report(
        Severity::Error, loc,
        "an inline array may not be the element of another "
        "inline array (`[N][M]T`) — multi-dimensional inline "
        "arrays are not admitted (§15.1)");
}

// Resolve the single-part syntactic name for a bare-parameter TypeRef.
// Locally-resolved refs carry `astNode` (with `name->parts[0]`); cross-module
// refs (ModuleLoader built the TypeRef, no AST exists) carry `importedName`.
// The qualified-name exclusion is preserved on both legs: only a single-part
// head can name a type parameter.
std::string_view paramHead(const TypeRef* sub) {
    if (!sub) return {};
    // fix #4698 (§2.2, v0.313): an alias-qualified built-in (`D.Stopwatch`,
    // alias segment dropped) is never a type-parameter reference, even when a
    // type parameter shares its bare name.
    if (sub->astNode && sub->astNode->aliasQualifiedBuiltin) return {};
    if (sub->astNode && sub->astNode->name && sub->astNode->name->parts.size() == 1) {
        return sub->astNode->name->parts.front();
    }
    if (!sub->astNode && !sub->importedName.empty() &&
        sub->importedName.find('.') == std::string_view::npos) {
        return sub->importedName;
    }
    return {};
}

// Apply `args.subst[head] -> concrete` at a bare-parameter leaf, preserving
// the nullable modifier (T? → int? when T→int). Returns `nullptr` when the
// leaf is not a bare-parameter lookup so the caller can continue with the
// recursive kinds.
TypeRef* applyBareParamSubst(const TypeRef* sub,
                              const std::unordered_map<std::string_view, TypeRef*>& subst,
                              SemaContext& ctx) {
    if (!sub || sub->kind != TypeRef::Kind::Named || sub->declaration) return nullptr;
    std::string_view head = paramHead(sub);
    if (head.empty()) return nullptr;
    auto it = subst.find(head);
    if (it == subst.end()) return nullptr;
    TypeRef* substituted = it->second;
    if (sub->nullable && substituted && !substituted->nullable) {
        TypeRef* nn = ctx.makeTypeRef(*substituted);
        nn->nullable = true;
        return nn;
    }
    return substituted;
}

}  // namespace

TypeRef* substituteGenericArgs(TypeRef* tr, const SubstArgs& args) {
    if (!tr) return tr;
    if (args.subst.empty() && !args.validateNestedInlineArrays) return tr;

    switch (tr->kind) {
        case TypeRef::Kind::Named: {
            // Bare-parameter leaf: substitute directly. If the lookup misses
            // (param name not in subst), fall through to the recursive walk —
            // the parameter may be a non-Named shape (rare) or a NAME not in
            // this particular subst (e.g. a different generic's parameter).
            if (TypeRef* substituted = applyBareParamSubst(tr, args.subst, args.ctx)) {
                return substituted;
            }
            // Not a bare-parameter reference — recurse into genericArgs so
            // any bare parameter BURIED inside (Dictionary<K, Array<V>>'s K
            // and V) is substituted. Returns `tr` unchanged if nothing beneath
            // needed substitution (no allocation).
            if (tr->genericArgs.empty()) return tr;
            std::vector<TypeRef*> newArgs;
            newArgs.reserve(tr->genericArgs.size());
            bool changed = false;
            for (TypeRef* arg : tr->genericArgs) {
                TypeRef* na = substituteGenericArgs(arg, args);
                newArgs.push_back(na);
                if (na != arg) changed = true;
            }
            if (!changed) return tr;
            TypeRef* out = args.ctx.makeTypeRef(*tr);
            out->genericArgs = std::move(newArgs);
            return out;
        }
        case TypeRef::Kind::InlineArray: {
            if (!tr->arrayElement) return tr;
            TypeRef* ne = substituteGenericArgs(tr->arrayElement, args);
            if (ne == tr->arrayElement) return tr;
            // §15.1 (send-back for fix #4280): resolveType's direct-syntax
            // check only sees the alias's own literal body at the ALIAS's
            // declaration site (`type Nested<T> := [1]T`, where T is still
            // unbound) — it cannot see that substitution at THIS reference
            // site just replaced T with an inline array itself
            // (`Nested<[1]string>`), which builds a genuine `[N][1]string`
            // with no re-validation anywhere else. Re-run the identical
            // check at the point substitution actually creates the nested
            // shape, so the rejection fires "wherever a nested inline array
            // is formed" rather than only through direct syntax.
            if (args.validateNestedInlineArrays &&
                ne->kind == TypeRef::Kind::InlineArray) {
                reportNestedInlineArrayError(args.ctx, args.useLoc);
            }
            TypeRef* out = args.ctx.makeTypeRef(*tr);
            out->arrayElement = ne;
            return out;
        }
        case TypeRef::Kind::Function: {
            bool changed = false;
            std::vector<TypeRef::FuncParam> newParams = tr->funcParams;
            for (auto& p : newParams) {
                TypeRef* np = substituteGenericArgs(p.type, args);
                if (np != p.type) { p.type = np; changed = true; }
            }
            TypeRef* newRet = substituteGenericArgs(tr->funcReturn, args);
            if (newRet != tr->funcReturn) changed = true;
            if (!changed) return tr;
            TypeRef* out = args.ctx.makeTypeRef(*tr);
            out->funcParams = std::move(newParams);
            out->funcReturn = newRet;
            return out;
        }
        default:
            // Primitive / Unit / Error — no nested positions to walk.
            return tr;
    }
}

}  // namespace bezel::sema