#pragma once

// SemaContext — global state passed through the semantic-analysis passes.
// Owns every Symbol allocated during compilation; later passes attach
// annotations (resolved TypeRefs, resolved-symbol pointers, ownership
// states) to the AST via side tables keyed on `const ast::Node*`.
//
// The context is deliberately the long-lived owner: symbols outlive the
// declaring AST node (e.g. so a future serialization pass can persist
// symbols without dragging the AST along). This is the architectural
// decision called out in the implementation plan as "AST mutation vs side
// tables" — we pick side tables.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::sema {

class SemaContext {
public:
    explicit SemaContext(DiagnosticEngine& diagnostics)
        : diagnostics_(diagnostics) {}

    SemaContext(const SemaContext&) = delete;
    SemaContext& operator=(const SemaContext&) = delete;

    DiagnosticEngine& diagnostics() { return diagnostics_; }

    // Allocate-and-own a symbol of the requested kind. Returns a raw pointer
    // that's stable for the lifetime of the context. The new symbol is
    // appended to the global symbol list (queryable via symbols()).
    template <typename S, typename... Args>
    S* makeSymbol(Args&&... args) {
        auto owned = std::make_unique<S>(std::forward<Args>(args)...);
        S* raw = owned.get();
        symbols_.push_back(std::move(owned));
        return raw;
    }

    // Read-only view of every allocated symbol. Ordering is allocation
    // order — useful for assertions in tests; iteration order is not part
    // of the symbol-collection contract.
    const std::vector<std::unique_ptr<Symbol>>& symbols() const {
        return symbols_;
    }

    // The module symbol introduced by the SourceFile's ModuleDecl. Null
    // until symbol collection runs; one ModuleSymbol per SourceFile.
    ModuleSymbol* module() { return module_; }
    void setModule(ModuleSymbol* m) { module_ = m; }

    // Reserve and return a fresh ScopeId. Caller is responsible for
    // attaching the ID to the symbols it creates inside that scope.
    ScopeId newScope() { return nextScope_++; }

    // Allocate-and-own a resolved TypeRef. Returned pointer is stable
    // for the lifetime of the context. Populated by type-resolution and
    // queried by later passes via `resolvedType(...)`.
    template <typename... Args>
    TypeRef* makeTypeRef(Args&&... args) {
        auto owned = std::make_unique<TypeRef>(std::forward<Args>(args)...);
        TypeRef* raw = owned.get();
        typeRefs_.push_back(std::move(owned));
        return raw;
    }

    // Record that `ast` resolves to `ref`. Replaces any prior binding.
    void setResolvedType(const ast::Type* ast, TypeRef* ref) {
        resolvedTypes_[ast] = ref;
    }

    // Look up the resolved TypeRef for an AST type node. Null when
    // type-resolution hasn't run or the node wasn't visitable.
    TypeRef* resolvedType(const ast::Type& ast) const {
        auto it = resolvedTypes_.find(&ast);
        return it == resolvedTypes_.end() ? nullptr : it->second;
    }

    // Record the typecheck-pass-inferred type of an expression. Used by
    // the expression-typecheck pass and queried by later passes
    // (ownership, definite-assignment).
    void setExprType(const ast::Expr* e, TypeRef* t) {
        exprTypes_[e] = t;
    }
    TypeRef* exprType(const ast::Expr& e) const {
        auto it = exprTypes_.find(&e);
        return it == exprTypes_.end() ? nullptr : it->second;
    }

    // A small primitive-type cache: looking up the same primitive shape
    // (Int, Bool, String, …) should return the same TypeRef so equality
    // comparisons are cheap. The cache is populated lazily on first
    // request.
    TypeRef* primitiveTypeRef(PrimitiveType p, bool nullable = false);

private:
    DiagnosticEngine& diagnostics_;
    std::vector<std::unique_ptr<Symbol>> symbols_;
    std::vector<std::unique_ptr<TypeRef>> typeRefs_;
    std::unordered_map<const ast::Type*, TypeRef*> resolvedTypes_;
    std::unordered_map<const ast::Expr*, TypeRef*> exprTypes_;
    // Cache for the common primitive TypeRefs. Key is `(p << 1) | nullable`.
    std::unordered_map<std::uint32_t, TypeRef*> primitiveCache_;
    ModuleSymbol* module_ = nullptr;
    // Scope IDs are assigned in DFS order. `kModuleScope` (=1) is the
    // initial module scope; everything nested gets a fresh ID > 1.
    ScopeId nextScope_ = kModuleScope + 1;
};

inline TypeRef* SemaContext::primitiveTypeRef(PrimitiveType p, bool nullable) {
    const std::uint32_t key =
        (static_cast<std::uint32_t>(p) << 1) | (nullable ? 1u : 0u);
    auto it = primitiveCache_.find(key);
    if (it != primitiveCache_.end()) return it->second;
    auto* ref = makeTypeRef();
    ref->kind = TypeRef::Kind::Primitive;
    ref->primitive = p;
    ref->nullable = nullable;
    primitiveCache_.emplace(key, ref);
    return ref;
}

} // namespace bezel::sema
