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

private:
    DiagnosticEngine& diagnostics_;
    std::vector<std::unique_ptr<Symbol>> symbols_;
    ModuleSymbol* module_ = nullptr;
    // Scope IDs are assigned in DFS order. `kModuleScope` (=1) is the
    // initial module scope; everything nested gets a fresh ID > 1.
    ScopeId nextScope_ = kModuleScope + 1;
};

} // namespace bezel::sema
