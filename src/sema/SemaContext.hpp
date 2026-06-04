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
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

    // Slice 3b: per-Expression resolved symbol (IdentifierExpr →
    // local/parameter/type/member). Null when the expression doesn't
    // carry a symbol reference or lookup failed.
    void setExprSymbol(const ast::Expr* e, Symbol* s) {
        exprSymbols_[e] = s;
    }
    Symbol* exprSymbol(const ast::Expr& e) const {
        auto it = exprSymbols_.find(&e);
        return it == exprSymbols_.end() ? nullptr : it->second;
    }

    // A small primitive-type cache: looking up the same primitive shape
    // (Int, Bool, String, …) should return the same TypeRef so equality
    // comparisons are cheap. The cache is populated lazily on first
    // request.
    TypeRef* primitiveTypeRef(PrimitiveType p, bool nullable = false);

    // §A.7 Lambda expressions — per-lambda parameter symbol list. Sema
    // populates this during expression type-checking; codegen uses it to
    // bind each lambda parameter to its LLVM argument.
    void setLambdaParams(const ast::LambdaExpr* lam,
                         std::vector<Symbol*> syms) {
        lambdaParams_[lam] = std::move(syms);
    }
    const std::vector<Symbol*>* lambdaParams(
        const ast::LambdaExpr* lam) const {
        auto it = lambdaParams_.find(lam);
        return it == lambdaParams_.end() ? nullptr : &it->second;
    }

    // --- Cross-module imports (§2.2 / §2.5, issue #328 increment 3) ---------
    //
    // A dependency's `.bezel.meta` is loaded (ModuleLoader) into THIS context,
    // reconstructing its exported public interface as TypeSymbol*/MemberSymbol*
    // and resolved TypeRef*. Those imported symbols are owned here in a list
    // SEPARATE from `symbols_`, so codegen — which iterates `symbols()` to
    // declare and EMIT bodies for the local module's declarations — never
    // tries to emit a body for an imported (externally-defined) symbol. Codegen
    // declares imported callables as `external` on demand (see importedTypes()).
    //
    // The registry is keyed by §2.1 dotted module name so a file's `import X;`
    // (§2.2) can expose exactly the named module's exported types. An imported
    // TypeSymbol carries its normal `kind`/members so name resolution and
    // type-checking treat it like any other type; the `imported_` set records
    // that it is externally defined.

    // Allocate-and-own an imported symbol (kept out of `symbols_`).
    template <typename S, typename... Args>
    S* makeImportedSymbol(Args&&... args) {
        auto owned = std::make_unique<S>(std::forward<Args>(args)...);
        S* raw = owned.get();
        importedSymbols_.push_back(std::move(owned));
        imported_.insert(raw);
        return raw;
    }

    // Allocate-and-own a TypeRef used by an imported signature (kept out of the
    // local `typeRefs_` lifetime concerns; same owner semantics as makeTypeRef).
    template <typename... Args>
    TypeRef* makeImportedTypeRef(Args&&... args) {
        auto owned = std::make_unique<TypeRef>(std::forward<Args>(args)...);
        TypeRef* raw = owned.get();
        importedTypeRefs_.push_back(std::move(owned));
        return raw;
    }

    // Register an imported, exported top-level TypeSymbol under its module's
    // §2.1 dotted name. One module may export many types.
    void registerImportedType(const std::string& moduleName, TypeSymbol* t) {
        importedByModule_[moduleName].push_back(t);
    }

    // The exported types of a loaded module, or null if the module was never
    // loaded. Used by name resolution to expose `import X;` targets.
    const std::vector<TypeSymbol*>* importedTypesOf(
        const std::string& moduleName) const {
        auto it = importedByModule_.find(moduleName);
        return it == importedByModule_.end() ? nullptr : &it->second;
    }

    // Every imported symbol (across all loaded modules), in load order. Codegen
    // walks this to emit external declarations for imported callables.
    std::vector<Symbol*> importedSymbols() const {
        std::vector<Symbol*> out;
        out.reserve(importedSymbols_.size());
        for (const auto& s : importedSymbols_) out.push_back(s.get());
        return out;
    }

    // True once at least one dependency module has been loaded — lets the local
    // (non-import) path stay byte-for-byte unchanged when there are no imports.
    bool hasImports() const { return !importedByModule_.empty(); }

    // Is `s` an imported (externally-defined) symbol? Codegen consults this to
    // emit an external declaration rather than a definition.
    bool isImported(const Symbol* s) const {
        return imported_.count(const_cast<Symbol*>(s)) != 0;
    }

    // Intern a string so a Symbol's string_view `name` (which does not own its
    // bytes) can point at storage that lives for the whole compilation. Used by
    // the loader, whose source ModuleMetadata is a transient by-value blob.
    std::string_view internString(const std::string& s) {
        internedStrings_.push_back(std::make_unique<std::string>(s));
        return *internedStrings_.back();
    }

    // An imported method/field's resolved type cannot ride the ast::Type-keyed
    // `resolvedTypes_` side table (an imported symbol has no ast::Type node), so
    // the loader records it keyed by the symbol. Codegen / overload resolution
    // read it via the accessors. (`returnOwned` mirrors §28 `owned T` returns.)
    void setImportedReturnType(const Symbol* m, TypeRef* t, bool returnOwned) {
        importedReturnTypes_[m] = t;
        if (returnOwned) importedReturnOwned_.insert(m);
    }
    TypeRef* importedReturnType(const Symbol* m) const {
        auto it = importedReturnTypes_.find(m);
        return it == importedReturnTypes_.end() ? nullptr : it->second;
    }
    bool importedReturnOwned(const Symbol* m) const {
        return importedReturnOwned_.count(m) != 0;
    }
    void setImportedMemberType(const Symbol* m, TypeRef* t) {
        importedMemberTypes_[m] = t;
    }
    TypeRef* importedMemberType(const Symbol* m) const {
        auto it = importedMemberTypes_.find(m);
        return it == importedMemberTypes_.end() ? nullptr : it->second;
    }

    // §22.12 (issue #328 FINDING [3] BUG A): an imported class-level `const` has
    // no storage symbol in the library — a const initializer is a compile-time
    // constant expression, so the library emits no global. The loader records
    // the const's textual value here, keyed by the imported ConstMemberSymbol;
    // codegen folds it at the cross-module read site. Absent → no folded value.
    void setImportedConstValue(const Symbol* m, std::string_view text) {
        importedConstValues_[m] = std::string(text);
    }
    const std::string* importedConstValue(const Symbol* m) const {
        auto it = importedConstValues_.find(m);
        return it == importedConstValues_.end() ? nullptr : &it->second;
    }

private:
    DiagnosticEngine& diagnostics_;
    std::vector<std::unique_ptr<Symbol>> symbols_;
    std::vector<std::unique_ptr<TypeRef>> typeRefs_;
    std::unordered_map<const ast::Type*, TypeRef*> resolvedTypes_;
    std::unordered_map<const ast::Expr*, TypeRef*> exprTypes_;
    std::unordered_map<const ast::Expr*, Symbol*> exprSymbols_;
    std::unordered_map<const ast::LambdaExpr*, std::vector<Symbol*>> lambdaParams_;
    // Cache for the common primitive TypeRefs. Key is `(p << 1) | nullable`.
    std::unordered_map<std::uint32_t, TypeRef*> primitiveCache_;
    // §2.5 imports (issue #328 inc 3): owned imported symbols + their TypeRefs,
    // kept separate from `symbols_`/`typeRefs_` (see the import block above),
    // plus the module-name → exported-type registry and the imported-symbol set.
    std::vector<std::unique_ptr<Symbol>> importedSymbols_;
    std::vector<std::unique_ptr<TypeRef>> importedTypeRefs_;
    std::unordered_map<std::string, std::vector<TypeSymbol*>> importedByModule_;
    std::unordered_set<Symbol*> imported_;
    std::vector<std::unique_ptr<std::string>> internedStrings_;
    std::unordered_map<const Symbol*, TypeRef*> importedReturnTypes_;
    std::unordered_set<const Symbol*> importedReturnOwned_;
    std::unordered_map<const Symbol*, TypeRef*> importedMemberTypes_;
    // §22.12 imported const values (issue #328 FINDING [3] BUG A), textual.
    std::unordered_map<const Symbol*, std::string> importedConstValues_;
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
