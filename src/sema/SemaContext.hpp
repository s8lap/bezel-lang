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

// fix #2391 (§14.2/§2.8.2, v0.236): constraints on an IMPORTED generic base
// method's type parameters — reconstructed from MetaMember::genericParams's
// per-param `constraintTypes`/`specialConstraints` payload (which the metadata
// builder has carried since #1579/v0.182). The loader stashes one entry per
// generic parameter, positionally aligned with `MethodSymbol::genericParams`,
// so `Resolver::checkOverrideSignatureMatch`'s §14.2 override-restatement
// check can compare against an imported generic base method's clause set the
// way it already does against a same-module base's `ast::GenericConstraint`
// set. The metadata builder folds the modifier form (`where T is owned`) into
// the same `specialConstraints` list as the regular-kind items
// (`where T: class`), so this struct deliberately does NOT split them — the
// override check compares a combined multiset (see
// `derivedConstraintMatchesImported`), which is equivalent to the AST-vs-AST
// `constraintSetsEquivalent` for every valid input (the grammar admits items
// XOR a modifier on a single clause).
struct ImportedGenericConstraint {
    std::string_view typeParam;  // generic parameter name
    std::vector<std::string_view> specialConstraints;  // "class"/"value"/"new"/...
    std::vector<TypeRef*> constraintTypes;  // resolved type-constraint TypeRefs
};

class SemaContext {
public:
    explicit SemaContext(DiagnosticEngine& diagnostics)
        : diagnostics_(diagnostics) {}

    SemaContext(const SemaContext&) = delete;
    SemaContext& operator=(const SemaContext&) = delete;

    // §22.16 (v0.250, #3038): a stable-address LocalSymbol standing in for
    // the contextual keyword `result` inside an `ensures` clause. Owned by
    // the SemaContext (lifetime = whole compilation) so the pointer stays
    // valid for codegen to read after the ExprTC that created the binding
    // is destroyed. The symbol's `declType` is rebound (not preserved) to
    // the current method's return type at every IdentifierExpr visit; a
    // single instance is reused across every `result` reference in every
    // clause of every method.
    LocalSymbol* syntheticResultLocal() {
        if (!syntheticResultLocal_) {
            syntheticResultLocal_ = makeSymbol<LocalSymbol>();
            syntheticResultLocal_->name = "result";
        }
        return syntheticResultLocal_;
    }

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

    // fix #3235 (§30.14 / §14.2): the §14.2 `where T: sendable` sites
    // `Resolver::checkOneConstraint` defers during `resolveTypes()` so
    // the late pass at the tail of `typecheckExpressions()` can replay
    // them when `FieldSymbol::posParamBodyRef` is populated (see
    // `bezel::sema::checkSendableConstraintsLate` / `Resolver::
    // checkSendableConstraintsLate`). The list lives on `SemaContext`
    // rather than on `Resolver` because the late pass is invoked from
    // `typecheckExpressions()` (ExprTypecheck.cpp) through a fresh
    // `Resolver` wrapper — the `Resolver` instance that recorded the
    // sites is no longer in scope, but the sites themselves (location,
    // param name, argument spelling, resolved TypeRef) are still valid
    // for the late replay. Each entry stores the same fields the
    // in-place `report` lambda in `checkOneConstraint` would have read.
    struct PendingSendableConstraint {
        SourceLocation loc;
        std::string param;     // constraint's type-parameter name
        std::string argName;   // argument spelling (diagnostic)
        TypeRef* arg = nullptr;
    };
    void addPendingSendableConstraint(PendingSendableConstraint p) {
        pendingSendableConstraints_.push_back(std::move(p));
    }
    std::vector<PendingSendableConstraint>& pendingSendableConstraints() {
        return pendingSendableConstraints_;
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
    // §14/#662c: iterate all expr→symbol entries (used by ModuleLoader to
    // remap fresh-context symbols to imported symbols after re-parsing a ctor body).
    const std::unordered_map<const ast::Expr*, Symbol*>& allExprSymbols() const {
        return exprSymbols_;
    }

    // §19.3 (v0.226, #1853): per-Pattern resolved symbol. A bare-name
    // NamedPattern that resolves to a LOCAL `const` carries no Expr node for
    // exprSymbol() to key on (a Pattern is not an Expr), so codegen's const
    // pattern resolver cannot recover the scope-correct LocalSymbol from the
    // name alone (locals are not in `symbols()` and a flat name search would
    // mis-handle shadowing). Sema records the resolved symbol here during
    // checkTopLevelNamedPattern; codegen's constValueForPatternName reads it
    // to fold the local const's compile-time value. Currently set only for a
    // compile-time-classified local const pattern (§17.14).
    void setPatternSymbol(const ast::Pattern* p, Symbol* s) {
        patternSymbols_[p] = s;
    }
    Symbol* patternSymbol(const ast::Pattern& p) const {
        auto it = patternSymbols_.find(&p);
        return it == patternSymbols_.end() ? nullptr : it->second;
    }

    // fix #3196 (§22.5 "Optional parameters and named arguments" / §22.10
    // "primary constructors", v0.254): a per-NewExpr side table that records
    // the §22.5 named-vs-positional BINDING of the call's arguments to the
    // target class's positional primary-ctor field slots. Each entry is a
    // vector parallel to `recordPositionalFieldSymbols(t)` — index j holds the
    // (0-based) argument index from `ne.args` that supplies field slot j, or
    // `-1` when the call OMITS that slot (left to default-fill or calloc-zero).
    //
    // The binding has exactly one body — `checkPositionalNewArgsFromSymbols`
    // in sema, which already computed it as a local `fieldToArg` vector
    // (§22.5 positional/named resolution over the symbol-side
    // `recordPositionalFieldSymbols` list) — and is published here so codegen
    // reads the SAME mapping sema accepted. Without this, codegen's
    // `storePrimaryCtorPositionalArgs` could only do the trivial "argument i
    // stores into field i" pairing, which silently zeroes every slot when the
    // call uses a named argument (`new PBox(W := 4, V := 3)` — §22.5 admits
    // the spelling, sema binds `W→1, V→0`, but codegen pairs `ne.args[0]`
    // with field 0 and `ne.args[1]` with field 1 and stores `4` into V, `3`
    // into W).
    //
    // Published by `checkPositionalNewArgsFromSymbols` for any `new T(...)`
    // whose target T has positional primary-ctor fields (LOCAL class primary
    // ctor, IMPORTED class primary ctor, IMPORTED record — all use the same
    // binding rule §22.5). Read by codegen's `storePrimaryCtorPositionalArgs`
    // for classes; records do not currently call that helper.
    //
    // Keys are the NewExpr nodes of the importer's / single-module AST (kept
    // alive by the SourceFile); values point into the map's own storage.
    // Unlike the four remap-required side tables (#2839/#2818/#2857/#2857),
    // this one lives ONLY on the call site — a `new` for an imported type
    // occurs in the IMPORTER, never in the dependency's re-parsed body — so
    // `ModuleLoader`'s body-reconstruction remap does not need to copy it.
    void setNewExprPositionalBinding(const ast::NewExpr* ne,
                                     std::vector<int> binding) {
        newExprPositionalBindings_[ne] = std::move(binding);
    }
    const std::vector<int>* newExprPositionalBinding(
        const ast::NewExpr& ne) const {
        auto it = newExprPositionalBindings_.find(&ne);
        return it == newExprPositionalBindings_.end() ? nullptr
                                                      : &it->second;
    }
    // §14.1/#783: iterate all AST-type→TypeRef and expr→TypeRef entries. Used by
    // ModuleLoader to COPY a re-parsed generic method body's resolved/expr types
    // from the fresh context into the main context, so codegen's per-instantiation
    // monomorphization (which queries resolvedType/exprType on the body's nodes)
    // sees them. The TypeRef* values stay valid because the fresh context is kept
    // alive alongside the re-parsed AST (ownImportedSemaContext).
    const std::unordered_map<const ast::Type*, TypeRef*>& allResolvedTypes()
        const {
        return resolvedTypes_;
    }
    const std::unordered_map<const ast::Expr*, TypeRef*>& allExprTypes() const {
        return exprTypes_;
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
    // fix #2839 (§14.1/#783, §14/#662c, v0.240): iterate all lambda→param-list
    // entries. Used by ModuleLoader to COPY a re-parsed imported generic body's
    // lambda parameter lists from the fresh context into the main one — the
    // fourth AST-keyed side table the body-reconstruction remap loops must
    // carry, alongside allExprSymbols/allResolvedTypes/allExprTypes. Without it
    // `lambdaParams(&lam)` is null in the importer, codegen's lowerLambda
    // declares the closure with NO user parameters, and the lambda's value is
    // lost. The keys are the re-parsed ast::LambdaExpr nodes (kept alive by
    // ownImportedAst) and the values point into the fresh context's symbol
    // arena (kept alive by ownImportedAuxiliary).
    const std::unordered_map<const ast::LambdaExpr*, std::vector<Symbol*>>&
    allLambdaParams() const {
        return lambdaParams_;
    }

    // fix #2857 (§19.3/#1853 / §2.8.2 clause 2, v0.240): patternSymbols_ is the
    // FIFTH AST-keyed side table the body-reconstruction remap loops must carry
    // — the last one nothing copied. Without it `patternSymbol(p)` is null in
    // the importer and codegen's `constValueForPatternName` cannot fold the
    // local const's compile-time value, so a `case` arm whose pattern is a
    // bare-name local `const` is rejected outright. Unlike its two silent
    // siblings (#2818, #2839) this one fails LOUDLY — but it is still the same
    // §2.8.2 clause 2 divergence: a body that compiles as ONE module fails to
    // compile through the artifact boundary.
    //
    // Keys are the re-parsed ast::Pattern nodes (kept alive by ownImportedAst);
    // values point into the fresh context's symbol arena (kept alive by
    // ownImportedAuxiliary). Copied verbatim for the same reason as #2818's
    // LocalSymbols: the symbol is declared INSIDE the re-parsed body and has no
    // importer-side twin to remap to.
    const std::unordered_map<const ast::Pattern*, Symbol*>&
    allPatternSymbols() const {
        return patternSymbols_;
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

    // fix #2516 (§9.2/§9.3/§2.8.3 rule 4): allocate a synthetic ast::Param
    // for an imported positional FieldSymbol — mirrors the LOCAL path's
    // `posParam = &pf` at SymbolCollection.cpp:309, but the importer has no
    // AST to borrow from so we synthesize one. Kept alive by `importedParams_`.
    ast::Param* makeImportedParam() {
        auto owned = std::make_unique<ast::Param>();
        ast::Param* raw = owned.get();
        importedParams_.push_back(std::move(owned));
        return raw;
    }

    // fix #3144 (§35.1 / §35.3 / §35.4, family F9 of umbrella #3152): allocate
    // a synthetic ast::ChoiceCase for an imported ChoiceCaseSymbol — the exact
    // shape of #2516's `makeImportedParam` above, for the exact same reason.
    // `ChoiceCaseSymbol::caseDecl` is where BOTH sides keep a case's payload
    // list (91 reads across sema, ownership and codegen), so reconstructing the
    // node the imported symbol points at is what makes the imported and local
    // answers come from ONE predicate rather than two that must be kept in
    // step. The case's own payload `ast::Param`s (and their `ast::Type` nodes)
    // live inside it by value; the resolved type of each payload node is
    // published through `setResolvedType`, so `ctx.resolvedType(*pf.type)` —
    // what every payload consumer already calls — answers for an imported case
    // exactly as it does for a local one. Kept alive by `importedChoiceCases_`,
    // which outlives every symbol that references it.
    ast::ChoiceCase* makeImportedChoiceCase() {
        auto owned = std::make_unique<ast::ChoiceCase>();
        ast::ChoiceCase* raw = owned.get();
        importedChoiceCases_.push_back(std::move(owned));
        return raw;
    }

    // Register an imported, exported top-level TypeSymbol under its module's
    // §2.1 dotted name. One module may export many types.
    void registerImportedType(const std::string& moduleName, TypeSymbol* t) {
        importedByModule_[moduleName].push_back(t);
    }

    // §26 / §2.5 (#735): register an imported `public extension T` block under
    // its defining module's §2.1 dotted name. Kept in its own registry (NOT in
    // ModuleSymbol::children) for two reasons: the importing file's own module
    // symbol must keep holding only ITS OWN declarations (the §23.2 / §26.3
    // per-block checks in TypeResolution walk `children` and must not re-check
    // a dependency's already-checked block), and §26.5 step 2 visibility is
    // `import`-gated exactly like an imported type's.
    void registerImportedExtension(const std::string& moduleName,
                                   ExtensionSymbol* e) {
        importedExtensionsByModule_[moduleName].push_back(e);
    }

    // The imported extension blocks of a loaded module, or null if the module
    // was never loaded (§26.5 step 2 — consulted only for `import`ed modules).
    const std::vector<ExtensionSymbol*>* importedExtensionsOf(
        const std::string& moduleName) const {
        auto it = importedExtensionsByModule_.find(moduleName);
        return it == importedExtensionsByModule_.end() ? nullptr : &it->second;
    }

    // §14 / #662c: take ownership of a re-parsed SourceFile that holds
    // ctor/dtor AST nodes for imported generic classes.  The SourceFile's
    // lifetime is tied to this SemaContext so that symbol decl pointers
    // remain valid through codegen.
    void ownImportedAst(std::unique_ptr<ast::SourceFile> sf) {
        importedCtorAst_.push_back(std::move(sf));
    }

    // §14 / #662c: keep the synthetic source string alive so that all
    // std::string_view fields in the re-parsed AST (IdentifierExpr::name,
    // AssignmentStmt::op, etc.) remain valid for the lifetime of this context.
    void ownImportedSource(std::string src) {
        importedSources_.push_back(std::move(src));
    }

    // §14.1 / #783: keep an arbitrary heap object (the fresh SemaContext +
    // DiagnosticEngine used to re-parse and type-resolve an imported generic
    // method body) alive for the lifetime of this context. The body's
    // resolved/expr TypeRefs (copied into resolvedTypes_/exprTypes_ via
    // setResolvedType/setExprType) point into the fresh context's TypeRef arena,
    // so the fresh context must outlive codegen. Type-erased via shared_ptr<void>
    // so SemaContext need not be a complete type at the member declaration.
    void ownImportedAuxiliary(std::shared_ptr<void> obj) {
        importedAuxiliary_.push_back(std::move(obj));
    }

    // The exported types of a loaded module, or null if the module was never
    // loaded. Used by name resolution to expose `import X;` targets.
    const std::vector<TypeSymbol*>* importedTypesOf(
        const std::string& moduleName) const {
        auto it = importedByModule_.find(moduleName);
        return it == importedByModule_.end() ? nullptr : &it->second;
    }

    // §2.5 / §12 / §11.3 (issue #1223): resolve a short type name across ALL
    // already-loaded modules. Used by the module loader when a MetaType::Named
    // refers to a type from a different module (e.g. a base class exported by
    // module A, referenced in module B's `Extended: Base` metadata). The local
    // `localTypes` map only covers the current module being loaded; cross-module
    // base references must fall back to this search. Returns the first match; in
    // a well-formed program each public type name is unique across the import
    // closure (the spec forbids ambiguous top-level names within a module, and
    // inter-module conflicts are a separate error), so first-match is correct.
    TypeSymbol* lookupImportedTypeByName(std::string_view name) const {
        for (const auto& [modName, types] : importedByModule_) {
            for (TypeSymbol* t : types) {
                if (t && t->name == name) return t;
            }
        }
        return nullptr;
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

    // fix #3358 (§2.8.3 rule 4, §8.6, v0.240): the per-module registry of
    // imported TypeSymbols, exposed to codegen so the consumer-side block in
    // `declareVtables` can iterate module names (the cross-module span box
    // vtable fix needs the dependency's `bvt$spanbox$<module>.<head>`
    // declared in its own `bz$vtable$table` regardless of whether the
    // consumer's source mentions Span — which `importedSymbols()` does NOT
    // surface when the consumer only sees the dependency as a typed result
    // reaching through `Object`/`IReadOnlySpan`). Read-only view; the map
    // itself stays private (writes go through `registerImportedType`).
    const std::unordered_map<std::string, std::vector<TypeSymbol*>>&
    importedByModule() const { return importedByModule_; }

    // §26.5 step 2 (#2573): true once at least one imported `public extension`
    // block has been registered. An extension-only dependency (one whose public
    // surface carries extension blocks but no exported types) never populates
    // importedByModule_, so hasImports() alone must not gate extension
    // visibility — the typecheck pass consults this to enter its
    // extension-population loop even when no types were imported.
    bool hasImportedExtensions() const {
        return !importedExtensionsByModule_.empty();
    }

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
    // fix #2391 (§14.2/§2.8.2, v0.236): per-generic-parameter constraint
    // payload for an IMPORTED generic base method, populated by ModuleLoader
    // from MetaMember::genericParams. Positionally aligned with the method's
    // `MethodSymbol::genericParams`. Absent (nullptr) for any non-generic
    // import or a method imported from a pre-#1579 blob — in either case the
    // §14.2 override-restatement check has nothing to compare against and
    // (preserving the long-standing accept behavior for unrecognized shapes)
    // skips the imported-base path; the same-module path is unaffected.
    void setImportedGenericConstraints(const Symbol* m,
                                       std::vector<ImportedGenericConstraint> v) {
        importedGenericConstraints_[m] = std::move(v);
    }
    const std::vector<ImportedGenericConstraint>* importedGenericConstraints(
        const Symbol* m) const {
        auto it = importedGenericConstraints_.find(m);
        return it == importedGenericConstraints_.end() ? nullptr : &it->second;
    }

    // fix #2400 (§23/§2.8.2/§2.8.3 rule 4): whether an imported Property (or
    // Indexer) member declares a setter — MetaMember::hasSetter (see its field
    // comment in ModuleMetadata.hpp for why this cannot be recovered from
    // `modifiers` alone, unlike a field's "readonly"). Absent (never set) for
    // any non-property import, and for a property imported from a pre-#2400
    // blob (no 11th MEMBER_BLOCK scalar) — both correctly default to `false`
    // ("no cross-module setter"), the safe direction.
    void setImportedPropertyHasSetter(const Symbol* m, bool hasSetter) {
        importedPropertyHasSetter_[m] = hasSetter;
    }
    bool importedPropertyHasSetter(const Symbol* m) const {
        auto it = importedPropertyHasSetter_.find(m);
        return it != importedPropertyHasSetter_.end() && it->second;
    }

    // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4): an imported
    // Property (or Indexer) member's setter's EFFECTIVE visibility —
    // MetaMember::setterVisibility (see its field comment in
    // ModuleMetadata.hpp). `hasSetter` above only recovers whether a setter
    // EXISTS; this recovers WHO may call it — without it, a `private set` /
    // `protected set` / `internal set` on an imported property was
    // unenforceable at the cross-module `:=` assignment site (the identical
    // same-module shape was, correctly, already rejected). Absent (never
    // set, e.g. any non-property import) defaults to `ast::Visibility::
    // Private` — the reject-conservative direction, so an unrecognized
    // shape is refused rather than silently permitted (§2.8.2 clause 1).
    void setImportedPropertySetterVisibility(const Symbol* m,
                                             ast::Visibility v) {
        importedPropertySetterVisibility_[m] = v;
    }
    ast::Visibility importedPropertySetterVisibility(const Symbol* m) const {
        auto it = importedPropertySetterVisibility_.find(m);
        return it != importedPropertySetterVisibility_.end()
            ? it->second
            : ast::Visibility::Private;
    }

    // fix #2478 (§9.2/§22.8): whether an imported Property member is declared
    // in AUTO form (a real instance-storage member, §9.2) — MetaMember::
    // isAutoProperty (see its field comment in ModuleMetadata.hpp for the
    // full rationale and the confirmed live memory-safety hazard this
    // closes). `sema::hasAstAutoPropertyDecl` is decl-dependent and always
    // returns `false` for an imported PropertySymbol (no ast::PropertyDecl
    // crosses the module boundary); THIS side table is the wire-carried
    // fallback that the one storage predicate
    // `sema::isAutoPropertyStorageSymbol` (sema/Symbol.hpp — called by
    // `collectInstanceStorage`/`collectRecordStorage` and by sema alike)
    // consults for a decl-null Property. Absent
    // (never set — any non-property import, or a property imported from a
    // pre-#2478 blob with no 13th MEMBER_BLOCK scalar) defaults to `false`
    // — the SAME "not storage" behavior already exhibited today, not a new
    // regression for a stale dependency.
    void setImportedPropertyIsAutoStorage(const Symbol* m, bool isAuto) {
        importedPropertyIsAutoStorage_[m] = isAuto;
    }
    bool importedPropertyIsAutoStorage(const Symbol* m) const {
        auto it = importedPropertyIsAutoStorage_.find(m);
        return it != importedPropertyIsAutoStorage_.end() && it->second;
    }

    // fix #2461 (§22.3/§23): whether an imported Property (or Indexer) member
    // declares a getter — MetaMember::hasGetter (the getter-presence twin of
    // #2400's hasSetter above). §22.3: "for `ref`, the getter is called once
    // to produce the callee's slot value (for `out` no getter call occurs)",
    // so a write-only property (setter but no getter) is a legal `out` target
    // but NOT a legal `ref` target. checkRefOutPropertyOperand consults this
    // to reject a write-only imported property used as `ref` with a clean
    // §22.3/§23 diagnostic; without it, the operand fell through sema to a
    // raw linker error (undefined `...$propget`). Absent (never set — any
    // non-property import, or a property imported from a pre-#2461 blob with
    // no 14th MEMBER_BLOCK scalar) defaults to `true` — NOT `false` like
    // hasSetter: a false default would false-reject a legitimate `ref` to a
    // read-write property from a stale dependency, while `true` preserves the
    // pre-#2461 behavior for a stale dependency (see the hasGetter field
    // comment in ModuleMetadata.hpp for the full rationale).
    void setImportedPropertyHasGetter(const Symbol* m, bool hasGetter) {
        importedPropertyHasGetter_[m] = hasGetter;
    }
    bool importedPropertyHasGetter(const Symbol* m) const {
        auto it = importedPropertyHasGetter_.find(m);
        return it == importedPropertyHasGetter_.end() || it->second;
    }

    // fix #2421 (§24/§2.8.2): an imported Event member's static/async
    // classification — MetaMember::isEventStatic/isEventAsync (see their
    // field comments in ModuleMetadata.hpp). ast::EventDecl is the SOLE
    // carrier in the local path (SymbolCollection.cpp never copies these onto
    // EventSymbol::modifiers), and an imported EventSymbol has no decl at all
    // (ModuleLoader reconstructs it without one). The codegen event-dispatch
    // path's `eventIsStatic`/`eventIsAsync` helpers (LlvmBackend_impl.hpp)
    // consult THIS side table as the decl-null fallback for an imported event,
    // so collectInstanceStorage/emitOwnFieldInit/ctor-cleanup (isStatic) and
    // the ifaceEventIndex_ mirror + itab$C$I build (isAsync) classify an
    // imported event IDENTICALLY to the exporter's real layout — the only way
    // to avoid an itable/struct-layout mismatch. Absent (never set — any
    // non-event import, or an event imported from a pre-#2421 blob with no
    // 15th/16th MEMBER_BLOCK scalar) defaults to `false` for both — the
    // classification a plain instance sync event carries (the common case),
    // so a stale not-yet-recompiled dependency keeps its already-broken pre-
    // fix behavior rather than guessing and risking the mismatch.
    void setImportedEventIsStatic(const Symbol* m, bool isStatic) {
        importedEventIsStatic_[m] = isStatic;
    }
    bool importedEventIsStatic(const Symbol* m) const {
        auto it = importedEventIsStatic_.find(m);
        return it != importedEventIsStatic_.end() && it->second;
    }
    void setImportedEventIsAsync(const Symbol* m, bool isAsync) {
        importedEventIsAsync_[m] = isAsync;
    }
    bool importedEventIsAsync(const Symbol* m) const {
        auto it = importedEventIsAsync_.find(m);
        return it != importedEventIsAsync_.end() && it->second;
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

    // fix #2860 (§2.8.2 "No restrictions beyond visibility" / §22.8 step 2,
    // v0.240): the source text of an IMPORTED GENERIC class's INSTANCE field
    // initializer (MetaMember::fieldInitText, recorded by ModuleLoader). A
    // generic class exports no `bz$<Type>$init`, so the consumer monomorphizes
    // construction and replays each initializer itself — but it can only replay
    // one that crossed the boundary as a foldable constant (importedConstValue
    // above). This table says "this field HAS an initializer", so a construction
    // site whose replay produced no store can name the shape in a diagnostic
    // (§2.8.2) instead of silently observing the §7.9 default. Absent → the
    // field genuinely has no initializer and its §7.9 default is correct.
    void setImportedInstanceFieldInitText(const Symbol* m,
                                          std::string_view text) {
        importedInstanceFieldInitTexts_[m] = std::string(text);
    }
    const std::string* importedInstanceFieldInitText(const Symbol* m) const {
        auto it = importedInstanceFieldInitTexts_.find(m);
        return it == importedInstanceFieldInitTexts_.end() ? nullptr
                                                           : &it->second;
    }

    // fix #2874 (§2.8.2 / §22.8 step 2, v0.240): a load-time-reconstructed
    // ast::Expr* for an IMPORTED GENERIC class's INSTANCE field initializer —
    // set by ModuleLoader when the field's `fieldInitText` (#2860) was
    // successfully re-parsed and sema-typed in a fresh context, and remapped
    // into mainCtx. Lives in the loader's imported-AST arena (ownImportedAst
    // keeps the SourceFile alive; the Expr* is a node in that SourceFile).
    // The codegen path `emitOwnFieldInit` reads this BEFORE consulting
    // `importedInstanceFieldInitText` so a successful reconstruction runs the
    // initializer instead of firing the construction-site diagnostic. A parse
    // or sema failure leaves this null and the diagnostic still fires —
    // matching the "failure path must not become a load-time reject" rule
    // (a mere import of the module must still build, #2351 / §2.8.2).
    void setImportedInstanceFieldInitExpr(const Symbol* m,
                                          const ast::Expr* e) {
        importedInstanceFieldInitExprs_[m] = e;
    }
    const ast::Expr* importedInstanceFieldInitExpr(const Symbol* m) const {
        auto it = importedInstanceFieldInitExprs_.find(m);
        return it == importedInstanceFieldInitExprs_.end() ? nullptr
                                                           : it->second;
    }

    // §9.8 / §2.3 / issue #922: allocate-and-cache a compiler-intrinsic
    // TypeSymbol for a builtin class name (e.g. "Range", "DateTime",
    // "TimeSpan").  The SAME pointer is returned on every call with the same
    // name so `declaration == ctx_.builtinClassSym("Range")` pointer-equality
    // checks (e.g. in ExprTypecheck_Exprs.cpp and isRangeType) work correctly
    // across both the TypeResolution pass (annotation path) and the
    // ExprTypecheck pass (inferred-form path).  isValueBuiltin is set for the
    // same names that ExprTC::builtinClassSym historically marked (DateTime,
    // TimeSpan, Guid, CancellationToken, Range — all are stack-allocated value
    // types with no disposal obligation per §28).
    TypeSymbol* builtinClassSym(std::string_view name);

    // #1115 / §2.8–§2.9 / #975 / stdlib §5: a compiler-synthesized REAL Interface
    // TypeSymbol for the auto-imported builtin generic interfaces IComparable<T>
    // / IEquatable<T> / IEqualityComparer<T>, so they lower as ordinary heap
    // interface references through the existing user-generic-interface itable path
    // (every codegen layer keys off declaration->kind==Interface). The SAME
    // pointer is cached/returned per name (pointer-equality guards). Each carries
    // one generic param `T` and one or more member MethodSymbols backed by real
    // abstract ast::MethodDecls so methodFnType resolves the itable signature:
    //   IComparable:       CompareTo(other: T): int
    //   IEquatable:        Equals(other: T): bool
    //   IEqualityComparer: Equals(a: T, b: T): bool; GetHashCode(value: T): int
    // Returns nullptr for any non-builtin-interface name.
    TypeSymbol* builtinInterfaceSym(std::string_view name);
    // fix #3023 (§11.2 / §14.1 / §2.8.3 rule 5, v0.240): a non-creating
    // lookup for the synthesized builtin interface symbol. Returns the cached
    // TypeSymbol or `nullptr` if none has been minted yet — never mints one,
    // which `builtinInterfaceSym` does as a side effect and which mutates
    // `symbols_` (a `push_back` can reallocate). The only call site that needs
    // this is `declareVtables` Pass B (LlvmBackend_Setup.cpp:3874), which is
    // iterating `ctx_.symbols()` and would otherwise invalidate its iterators
    // by triggering a reallocation from inside the loop body. Every other
    // caller of `builtinInterfaceSym` is past sema and outside any
    // `symbols_` iteration, so the creating form is correct for them.
    TypeSymbol* builtinInterfaceSymIfCached(std::string_view name) const {
        auto it = builtinInterfaceCache_.find(std::string(name));
        return it != builtinInterfaceCache_.end() ? it->second : nullptr;
    }

    // #2776 / #2787: the CLOSED set of builtin interfaces that get a REAL
    // synthesized Interface TypeSymbol (see TypeResolution_Names.cpp's
    // name-resolution site for why each is on the list). Factored into one
    // predicate rather than copied, because #1867's convention is that these
    // are matched by IDENTITY against `builtinInterfaceSym` — so every place
    // that must produce or recognise that identity has to agree on the same
    // set, and a second hand-written copy is how they drift apart (#2787).
    static bool isSynthesizedBuiltinInterfaceName(std::string_view n) {
        return n == "IComparable" || n == "IEquatable" ||
               n == "IEqualityComparer" || n == "IReadOnlySpan" ||
               n == "INumber" || n == "ISteppable" || n == "IParsable";
    }

    // §26.5 (#714): store/retrieve the free-type-variable binding that was
    // inferred when a generic extension method matched via ancestor lookup.
    // `e` is the MemberAccessExpr node of the member access (the `.MyCount`
    // in `xs.MyCount()`).  Each entry is a list of (paramName, concreteTypeRef)
    // pairs, e.g. [("T", TypeRef*{int})].  The list is consumed by typeCall
    // (return-type substitution) and by codegen (genericSubst_ setup).
    void setExprExtBinding(
        const ast::Expr* e,
        std::vector<std::pair<std::string, TypeRef*>> binding) {
        exprExtBindings_[e] = std::move(binding);
    }
    const std::vector<std::pair<std::string, TypeRef*>>*
    exprExtBinding(const ast::Expr& e) const {
        auto it = exprExtBindings_.find(&e);
        return it == exprExtBindings_.end() ? nullptr : &it->second;
    }

private:
    DiagnosticEngine& diagnostics_;
    std::vector<std::unique_ptr<Symbol>> symbols_;
    std::vector<std::unique_ptr<TypeRef>> typeRefs_;
    // fix #3235 (§30.14 / §14.2): pending sendable-constraint sites for
    // the late pass at the tail of `typecheckExpressions()`. See the
    // `addPendingSendableConstraint` / `pendingSendableConstraints`
    // accessors above for the full rationale. The list survives across
    // Resolver instances because the late pass is invoked through a
    // fresh Resolver wrapper (see `bezel::sema::checkSendableConstraintsLate`).
    std::vector<PendingSendableConstraint> pendingSendableConstraints_;
    std::unordered_map<const ast::Type*, TypeRef*> resolvedTypes_;
    std::unordered_map<const ast::Expr*, TypeRef*> exprTypes_;
    std::unordered_map<const ast::Expr*, Symbol*> exprSymbols_;
    // §22.16 (v0.250, #3038): see `syntheticResultLocal()` accessor. Lazy so
    // the symbol is only created when an `ensures` clause actually uses
    // `result` (the common program has no clauses at all).
    LocalSymbol* syntheticResultLocal_ = nullptr;
    // §19.3 (#1853): see setPatternSymbol — a bare-name local-const NamedPattern.
    std::unordered_map<const ast::Pattern*, Symbol*> patternSymbols_;
    std::unordered_map<const ast::LambdaExpr*, std::vector<Symbol*>> lambdaParams_;
    // fix #3196 (§22.5 / §22.10 / v0.254): per-NewExpr §22.5 binding of call
    // arguments to positional primary-ctor field slots. Populated by
    // `checkPositionalNewArgsFromSymbols`, read by codegen's
    // `storePrimaryCtorPositionalArgs`. See the setter/getter comments above
    // for the rationale and lifetime story. Local-only — never copied by
    // ModuleLoader's body-reconstruction remap (the `new` site lives in the
    // importer, not in a dependency's re-parsed body).
    std::unordered_map<const ast::NewExpr*, std::vector<int>>
        newExprPositionalBindings_;
    // Cache for the common primitive TypeRefs. Key is `(p << 1) | nullable`.
    std::unordered_map<std::uint32_t, TypeRef*> primitiveCache_;
    // §2.5 imports (issue #328 inc 3): owned imported symbols + their TypeRefs,
    // kept separate from `symbols_`/`typeRefs_` (see the import block above),
    // plus the module-name → exported-type registry and the imported-symbol set.
    std::vector<std::unique_ptr<Symbol>> importedSymbols_;
    std::vector<std::unique_ptr<TypeRef>> importedTypeRefs_;
    // fix #2516 (§9.2/§9.3/§2.8.3 rule 4): synthetic ast::Param instances
    // attached to imported positional FieldSymbols so `fs->posParam != nullptr`
    // distinguishes positional from body fields cross-module (mirrors the
    // LOCAL SymbolCollection.cpp:295-313 path, which points posParam at a real
    // td.positionalFields[i]). The Params live at least as long as the
    // FieldSymbols that reference them.
    std::vector<std::unique_ptr<ast::Param>> importedParams_;
    // fix #3144 (§35.1, family F9 of #3152): synthetic ast::ChoiceCase nodes
    // attached to imported ChoiceCaseSymbols (see makeImportedChoiceCase). They
    // own their payload Params and those Params' ast::Type nodes, so this one
    // vector keeps the whole reconstructed case alive.
    std::vector<std::unique_ptr<ast::ChoiceCase>> importedChoiceCases_;
    std::unordered_map<std::string, std::vector<TypeSymbol*>> importedByModule_;
    // §26 / §2.5 (#735): imported extension blocks, keyed by defining module.
    std::unordered_map<std::string, std::vector<ExtensionSymbol*>>
        importedExtensionsByModule_;
    std::unordered_set<Symbol*> imported_;
    std::vector<std::unique_ptr<std::string>> internedStrings_;
    std::unordered_map<const Symbol*, TypeRef*> importedReturnTypes_;
    std::unordered_set<const Symbol*> importedReturnOwned_;
    std::unordered_map<const Symbol*, TypeRef*> importedMemberTypes_;
    // fix #2391 (§14.2/§2.8.2, v0.236): imported generic base method's
    // per-parameter constraint payload, from MetaMember::genericParams.
    std::unordered_map<const Symbol*, std::vector<ImportedGenericConstraint>>
        importedGenericConstraints_;
    // fix #2400 (§23/§2.8.2/§2.8.3 rule 4): imported property/indexer setter
    // presence, from MetaMember::hasSetter.
    std::unordered_map<const Symbol*, bool> importedPropertyHasSetter_;
    // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4): imported
    // property/indexer setter EFFECTIVE visibility, from
    // MetaMember::setterVisibility.
    std::unordered_map<const Symbol*, ast::Visibility>
        importedPropertySetterVisibility_;
    // fix #2478 (§9.2/§22.8): imported property AUTO-storage classification,
    // from MetaMember::isAutoProperty.
    std::unordered_map<const Symbol*, bool> importedPropertyIsAutoStorage_;
    // fix #2461 (§22.3/§23): imported property/indexer getter presence, from
    // MetaMember::hasGetter.
    std::unordered_map<const Symbol*, bool> importedPropertyHasGetter_;
    // fix #2421 (§24/§2.8.2): imported event static/async classification,
    // from MetaMember::isEventStatic/isEventAsync.
    std::unordered_map<const Symbol*, bool> importedEventIsStatic_;
    std::unordered_map<const Symbol*, bool> importedEventIsAsync_;
    // §22.12 imported const values (issue #328 FINDING [3] BUG A), textual.
    std::unordered_map<const Symbol*, std::string> importedConstValues_;
    // fix #2860 (§2.8.2): imported GENERIC class instance-field initializer
    // source text, keyed by the reconstructed FieldSymbol. See the accessors.
    std::unordered_map<const Symbol*, std::string>
        importedInstanceFieldInitTexts_;
    // fix #2874 (§2.8.2 / §22.8 step 2, v0.240): a load-time-reconstructed
    // ast::Expr* for an imported GENERIC class's instance field initializer,
    // set when parseAndAttachFieldInit succeeded for a non-static member
    // (the same parse path #2351 wrote for static members, without the final
    // `importedFs->decl = parsedFd` attach that would re-route the imported
    // member's LAYOUT through a TypeRef resolved in the throwaway context —
    // see #2860's own decline comment). The Expr* rides in
    // importedCtorAst_'s SourceFile (the fresh context's parsed SourceFile,
    // kept alive via ownImportedAuxiliary), so the side-table pointer is
    // valid for the lifetime of this SemaContext.
    std::unordered_map<const Symbol*, const ast::Expr*>
        importedInstanceFieldInitExprs_;
    // §14 / #662c: imported SourceFile objects that own re-parsed ctor/dtor
    // AST nodes for imported generic classes.  The re-parsed AST carries
    // ConstructorDecl / DestructorDecl nodes whose decl pointers are attached
    // to the reconstructed ConstructorSymbol / DestructorSymbol; the SourceFile
    // must stay alive for the lifetime of this SemaContext so those pointers
    // remain valid.  Completely independent of the importer's own SourceFile.
    std::vector<std::unique_ptr<ast::SourceFile>> importedCtorAst_;
    // Backing storage for synthetic source strings whose string_view fields
    // are referenced by re-parsed AST nodes in importedCtorAst_.
    std::vector<std::string> importedSources_;
    // §14.1 / #783: type-erased heap objects kept alive for this context's
    // lifetime (the fresh SemaContext + DiagnosticEngine that own a re-parsed
    // generic method body's resolved/expr TypeRefs).
    std::vector<std::shared_ptr<void>> importedAuxiliary_;
    // §26.5 (#714): per MemberAccessExpr, the free-type-variable → concrete-TypeRef
    // binding inferred when a generic extension method matched a builtin receiver
    // through the ancestor chain (e.g. `extension IEnumerable<T>` on `Array<int>`
    // binds T → int).  Set by ExprTypecheck (lookupExtensionMember/resolveExtension)
    // and consumed by typeCall (return-type substitution) and codegen
    // (genericSubst_ setup via exprExtBinding).
    std::unordered_map<const ast::Expr*,
                       std::vector<std::pair<std::string, TypeRef*>>>
        exprExtBindings_;
    // §9.8 / issue #922: shared cache for compiler-intrinsic TypeSymbol stubs
    // allocated by builtinClassSym().  Shared so the annotation-resolution pass
    // (TypeResolution) and the typecheck pass (ExprTypecheck) return the SAME
    // pointer for the same builtin name, enabling pointer-equality guards.
    std::unordered_map<std::string, TypeSymbol*> builtinClassCache_;
    // #1115: cache + owning AST storage for builtinInterfaceSym. The synthesized
    // ast::TypeDecl transitively owns its MethodDecl + Type + QualifiedName
    // nodes; ms->decl and ParameterSymbol::declType point into it, so it must
    // outlive this SemaContext (which it does — same lifetime).
    std::unordered_map<std::string, TypeSymbol*> builtinInterfaceCache_;
    std::vector<std::unique_ptr<ast::TypeDecl>> builtinInterfaceAst_;
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

// §9.8 / issue #922: return (and cache) a compiler-intrinsic TypeSymbol for
// a builtin class name.  isValueBuiltin is set for the scalar/struct value
// types that carry no disposal obligation (§28): DateTime, TimeSpan, Guid,
// CancellationToken, and Range (§9.8 — a pair of integer bounds, stack-
// allocated).  All other names default to isValueBuiltin=false.
inline TypeSymbol* SemaContext::builtinClassSym(std::string_view name) {
    auto key = std::string(name);
    auto it = builtinClassCache_.find(key);
    if (it != builtinClassCache_.end()) return it->second;
    auto* ts = makeSymbol<TypeSymbol>();
    ts->kind = ast::TypeDeclKind::Class;
    ts->name = name;
    ts->decl = nullptr;
    if (name == "DateTime" || name == "TimeSpan" ||
        name == "Guid"     || name == "CancellationToken" ||
        name == "Range"    || name == "DateOnly" || name == "TimeOnly") {
        ts->isValueBuiltin = true;
    }
    builtinClassCache_[key] = ts;
    return ts;
}

// #1463 / §4.2 / §14.4: returns true when `name` is a value-builtin whose
// Named TypeRef intentionally carries `declaration=null` after TypeResolution
// (DateTime/TimeSpan/DateOnly/TimeOnly/Guid/CancellationToken keep
// declaration=null so that lowerNew()'s `!rt->declaration` gate fires for
// `new T(...)`; see TypeResolution_Names.cpp ~line 1040).  The function
// consolidates the authoritative value-builtin-name set so that callers such
// as the owning-collection element-type check do not maintain separate hand-
// lists that diverge as new value-builtins are added.  Range/Buffer/IntPtr/
// UIntPtr/ReadOnlyStringSpan/Boolean/Span/ReadOnlyArraySpan/ArraySpan are
// also value-or-record types that must be excluded from owning collections.
inline bool isValueBuiltinByName(std::string_view n) {
    // Primary: synthetic value-type builtins (isValueBuiltin=true in
    // builtinClassSym) that reach TypeResolution checks with declaration=null.
    if (n == "DateTime" || n == "TimeSpan" ||
        n == "DateOnly" || n == "TimeOnly" ||
        n == "Guid"     || n == "CancellationToken")
        return true;
    // Secondary: other non-class/non-interface builtins that have no
    // disposal obligation (value records, FFI value types, span views).
    if (n == "Range"    || n == "Buffer"   ||
        n == "IntPtr"   || n == "UIntPtr"  ||
        n == "ReadOnlyStringSpan" || n == "Boolean" ||
        n == "Span"     || n == "ReadOnlySpan" ||
        n == "ArraySpan" || n == "ReadOnlyArraySpan")
        return true;
    return false;
}

// #1115 / §2.8–§2.9 / #975 (stdlib §5): synthesize (and cache) a real Interface
// TypeSymbol for the builtin generic interfaces IComparable<T> / IEquatable<T> /
// IEqualityComparer<T>, mirroring the shape SymbolCollection produces for a user
// `interface I<T> method M(...): R`, so they join the existing user-interface
// itable codegen path with no new codegen. The contract members are:
//   IComparable:       CompareTo(other: T): int
//   IEquatable:        Equals(other: T): bool
//   IEqualityComparer: Equals(a: T, b: T): bool; GetHashCode(value: T): int
// each backed by a real abstract ast::MethodDecl (methodFnType needs the decl to
// build the itable signature; every return-type node is pre-resolved so it lowers
// to int/bool, not the i32 fallback). Each generic-`T` parameter gets its own
// Type node so per-use substitution (T → the instantiation argument) is keyed
// independently. The synthesized interface dispatches identically to a user
// `interface` with the same shape — IEqualityComparer is a user-IMPLEMENTED
// agent (stdlib §5); the Dictionary/Set comparer-constructor overloads that
// would *consume* one are a separate, still-deferred feature.

// fix #3308 / §22.3 / §22.4 (v0.268): the SECOND MethodSymbol-construction
// path in builtinInterfaceSym (the IComparable / IEquatable /
// IEqualityComparer / IReadOnlySpan synthesized-family arm, ~line 1345) used
// to inline the ParameterSymbol construction without setting `ps->modifier`.
// #2641's codegen fix had to install an `ast::MethodDecl`-side AST fall-
// back for this exact reason. Centralizing the per-parameter construction in
// this helper so BOTH the path #2 loop AND the test seam drive the same
// `ps->modifier = param.modifier;` assignment is the fix.
//
// The helper is intentionally free (not a SemaContext method) so the test
// can drive it directly with a constructed `ast::Param` carrying a non-
// empty `modifier` (the four synthesized interfaces' parameters all carry
// no modifier in the spec, so reading them through the helper would always
// see "" — which is correct, but observationally equivalent to a missing
// assignment). The test constructs an `ast::Param` with `modifier = "out"`
// and asserts the symbol carries it through: pre-fix the helper would
// leave `ps->modifier` default-empty (FAIL), post-fix it propagates the
// modifier (PASS).
inline ParameterSymbol* buildBuiltinInterfaceParamSymbol(
    SemaContext& ctx,
    const ast::Param& param) {
    auto* ps = ctx.makeSymbol<ParameterSymbol>();
    ps->name = param.name;
    // per §22.3/§22.4 (v0.268, #3308) — populate the formal modifier from
    // the synthesized ast::Param so downstream codegen (#2641's
    // registerOutVarStringRelease) reads the modifier from the SYMBOL
    // rather than falling back to the AST. Today the four synthesized
    // builtin interfaces' parameters carry no spec-side modifier, so the
    // value is empty; the assignment closes the seam gap.
    ps->modifier = param.modifier;
    // per §22.4/§1585 (v0.268, #3313) — populate the `when true`/`when
    // false` out-nullability postcondition from the synthesized
    // ast::Param so the caller-side narrowing (narrowFromOutPostcond)
    // sees the same postcondition path #1 (~line 1216) writes. Today
    // the four synthesized interfaces carry no spec-side postcondition,
    // so the value defaults to None; the assignment closes the parallel
    // seam gap mirrored by #3308.
    ps->outNullPostcond = param.outNullPostcond;
    ps->declType = param.type.get();
    return ps;
}

inline TypeSymbol* SemaContext::builtinInterfaceSym(std::string_view name) {
    // stdlib §2.19 / spec §11.8 (v0.176, #807): the static-abstraction
    // interfaces INumber<T> / ISteppable<T> / IParsable<T>. All members are
    // STATIC (abstract-only static methods + static operators, §11.8), so —
    // unlike the instance-member builtin interfaces below — they contribute
    // NO itable slots (declareVtables Pass 0 excludes statics/operators).
    // They exist as real Interface TypeSymbols so that:
    //   • `class Meters: INumber<Meters>` resolves and lands in baseTypes,
    //   • `where T: INumber<T>` resolves as a constraint bound,
    //   • the §11.8 conformance check and T.Member dispatch read the member
    //     signatures (each backed by a real abstract ast node).
    // The member sets are the §2.19 declarations verbatim.
    if (name == "INumber" || name == "ISteppable" || name == "IParsable") {
        auto key = std::string(name);
        auto it = builtinInterfaceCache_.find(key);
        if (it != builtinInterfaceCache_.end()) return it->second;

        // One member spec: a static method (isOp=false, name) or a static
        // operator (isOp=true, token). Type spellings admitted: "T", "T?",
        // "bool", "long", "string" — the §2.19 surface.
        struct SParam {
            const char* pname;
            const char* type;
            const char* mod;
            // §22.4/§1585 (stdlib §1): a shape-(a) Try out-parameter's
            // `when true` nullability postcondition, or None when absent.
            ast::OutNullWhen postcond = ast::OutNullWhen::None;
        };
        struct SMember {
            bool isOp;
            const char* nameOrToken;
            std::vector<SParam> params;
            const char* ret;
            // §28.1 / §11.8 (v0.196, #2048): the producer members of these
            // static-abstraction interfaces declare `owned T` per stdlib §2.19
            // — at a class/interface conformer the marker is the explicit
            // ownership transfer; at a value/runtime-managed conformer the
            // substitution ERASES the marker (the per-instantiation erasure of
            // §28.1). The matching logic accounts for both.
            bool retOwned = false;
        };
        std::vector<SMember> smembers;
        if (name == "INumber") {
            smembers = {
                {false, "Zero", {}, "T", true},
                {false, "One",  {}, "T", true},
                {true,  "+", {{"a", "T", ""}, {"b", "T", ""}}, "T", true},
                {true,  "-", {{"a", "T", ""}, {"b", "T", ""}}, "T", true},
                {true,  "*", {{"a", "T", ""}, {"b", "T", ""}}, "T", true},
                {true,  "/", {{"a", "T", ""}, {"b", "T", ""}}, "T", true},
                {true,  "<", {{"a", "T", ""}, {"b", "T", ""}}, "bool", false},
            };
        } else if (name == "ISteppable") {
            smembers = {
                {false, "Step", {{"value", "T", ""}, {"by", "long", ""}}, "T", true},
                {true,  "<", {{"a", "T", ""}, {"b", "T", ""}}, "bool", false},
            };
        } else {  // IParsable
            // stdlib §1 / §2.19: `TryParse` is a shape-(a) Try — its `out`
            // parameter is `T?` and carries the `when true` nullability
            // postcondition (elided from the stdlib listing for brevity but
            // normative for "the §11.8 `TryParse` companions"). Without it the
            // §22.4 caller narrowing never fires for the generic `T.TryParse`
            // bridge, so `if T.TryParse(s, out var v) then … return v; end`
            // wrongly sees `v` as still-nullable `T?` (regression from #1597,
            // which correctly stopped stripping the `?` off the recipient).
            smembers = {
                {false, "Parse", {{"s", "string", ""}}, "T", true},
                {false, "TryParse",
                 {{"s", "string", ""},
                  {"value", "T?", "out", ast::OutNullWhen::WhenTrue}},
                 "bool", false},
            };
        }

        // Build one ast::Type node for a spelling, returning the raw node.
        auto typeDecl = std::make_unique<ast::TypeDecl>();
        typeDecl->kind = ast::TypeDeclKind::Interface;
        typeDecl->visibility = ast::Visibility::Public;
        typeDecl->name = name == "INumber" ? "INumber"
                        : name == "ISteppable" ? "ISteppable" : "IParsable";
        typeDecl->genericParams.push_back(ast::GenericParam{"", "T"});
        auto makeTypeNode = [&](const char* spelling) -> std::unique_ptr<ast::Type> {
            auto t = std::make_unique<ast::Type>();
            t->kind = ast::TypeKind::Named;
            t->name = std::make_unique<ast::QualifiedName>();
            std::string_view sp(spelling);
            if (sp == "T?") {
                t->name->parts.push_back("T");
                t->nullable = true;
            } else {
                t->name->parts.push_back(sp);
            }
            return t;
        };
        // Pre-resolve a node: primitives → the cached primitive TypeRef;
        // `T`/`T?` → a declaration-less Named TypeRef carrying the node (so
        // the §11.8 substitution machinery keys off astNode->name).
        auto preResolve = [&](ast::Type* node) {
            std::string_view n = node->name->parts[0];
            PrimitiveType p = PrimitiveType::None;
            if (n == "bool") p = PrimitiveType::Bool;
            else if (n == "long") p = PrimitiveType::Long;
            else if (n == "string") p = PrimitiveType::String;
            if (p != PrimitiveType::None) {
                setResolvedType(node, primitiveTypeRef(p, false));
            } else {
                TypeRef* tr = makeTypeRef();
                tr->kind = TypeRef::Kind::Named;
                tr->astNode = node;
                tr->nullable = node->nullable;
                setResolvedType(node, tr);
            }
        };
        // Raw AST pointers, parallel to smembers, to back the symbols.
        struct RawM {
            ast::Member* m;
            std::vector<ast::Type*> paramTypes;
        };
        std::vector<RawM> raws;
        for (const auto& sm : smembers) {
            RawM rm;
            if (sm.isOp) {
                auto od = std::make_unique<ast::OperatorDecl>();
                od->visibility = ast::Visibility::None;  // implicit public
                od->opToken = sm.nameOrToken;
                od->bodyKind = ast::MethodBodyKind::Abstract;
                for (const auto& sp : sm.params) {
                    ast::Param p;
                    p.name = sp.pname;
                    p.modifier = sp.mod;
                    p.type = makeTypeNode(sp.type);
                    preResolve(p.type.get());
                    rm.paramTypes.push_back(p.type.get());
                    od->params.push_back(std::move(p));
                }
                od->returnType = makeTypeNode(sm.ret);
                od->returnOwned = sm.retOwned;  // §28.1/§11.8 v0.196 (#2048)
                preResolve(od->returnType.get());
                rm.m = od.get();
                typeDecl->members.push_back(std::move(od));
            } else {
                auto md = std::make_unique<ast::MethodDecl>();
                md->visibility = ast::Visibility::None;  // implicit public
                md->modifiers.push_back("static");       // per §11.8
                md->name = sm.nameOrToken;
                md->bodyKind = ast::MethodBodyKind::Abstract;
                for (const auto& sp : sm.params) {
                    ast::Param p;
                    p.name = sp.pname;
                    p.modifier = sp.mod;
                    p.outNullPostcond = sp.postcond;  // §22.4/§1585
                    p.type = makeTypeNode(sp.type);
                    preResolve(p.type.get());
                    rm.paramTypes.push_back(p.type.get());
                    md->params.push_back(std::move(p));
                }
                md->returnType = makeTypeNode(sm.ret);
                md->returnOwned = sm.retOwned;  // §28.1/§11.8 v0.196 (#2048)
                preResolve(md->returnType.get());
                rm.m = md.get();
                typeDecl->members.push_back(std::move(md));
            }
            raws.push_back(std::move(rm));
        }
        ast::TypeDecl* tdRaw = typeDecl.get();
        builtinInterfaceAst_.push_back(std::move(typeDecl));

        auto* ts = makeSymbol<TypeSymbol>();
        ts->kind = ast::TypeDeclKind::Interface;
        ts->name = tdRaw->name;
        ts->visibility = ast::Visibility::Public;
        ts->decl = tdRaw;
        auto* tparam = makeSymbol<GenericParamSymbol>();
        tparam->name = "T";
        ts->genericParams.push_back(tparam);
        for (std::size_t i = 0; i < smembers.size(); ++i) {
            const SMember& sm = smembers[i];
            const RawM& rm = raws[i];
            auto* ms = makeSymbol<MethodSymbol>();
            ms->name = sm.nameOrToken;
            ms->visibility = ast::Visibility::Public;
            ms->parent = ts;
            ms->decl = rm.m;
            if (sm.isOp) {
                ms->isOperator = true;
                ms->operatorToken = sm.nameOrToken;
            } else {
                ms->modifiers.push_back("static");
            }
            for (std::size_t p = 0; p < sm.params.size(); ++p) {
                auto* ps = makeSymbol<ParameterSymbol>();
                ps->name = sm.params[p].pname;
                ps->modifier = sm.params[p].mod;
                // §22.4/§1585 (stdlib §1): carry the out-nullability
                // postcondition onto the symbol the caller-side narrowing
                // (narrowFromOutPostcond) reads for the `T.TryParse` bridge.
                ps->outNullPostcond = sm.params[p].postcond;
                ps->declType = rm.paramTypes[p];
                ms->params.push_back(ps);
            }
            ts->members.push_back(ms);
        }
        // #3139: mark the synthesized identity, so `declaresBuiltinInterface`
        // can tell a base that names one of these from a USER type of the same
        // name without a second hand-listed copy of the name set.
        ts->isSynthesizedBuiltinInterface = true;
        builtinInterfaceCache_[key] = ts;
        return ts;
    }

    // Per-method contract spec. Every parameter is the generic `T` (the only
    // shape the three builtin interfaces need); paramNames give the spec-faithful
    // names. retName/retPrim must agree so the pre-resolved return node matches
    // methodFnType's lowering.
    struct MethodSpec {
        const char* methodName;
        const char* retName;
        PrimitiveType retPrim;
        std::vector<const char*> paramNames;  // each parameter has type `T`
    };
    // PropSpec: a named property or read-only indexer on a builtin interface.
    // retPrim == PrimitiveType::None means generic T return (indexer); the
    // pre-resolution step is skipped and lowerIndexerGet recovers the concrete
    // element type from the receiver's genericArgs at use time.
    // propName == nullptr signals the indexer form; indexParamName is the index
    // parameter name (type fixed to `int`).
    struct PropSpec {
        const char* propName;       // null for the indexer form
        const char* retName;        // "int", "bool", or "T" for indexer
        PrimitiveType retPrim;      // PrimitiveType::None for generic T
        bool isIndexer;
        const char* indexParamName; // "i" for the indexer; nullptr otherwise
    };
    std::vector<MethodSpec> methods;
    std::vector<PropSpec>   props;
    const char* ifaceName = nullptr;
    if (name == "IComparable") {
        ifaceName = "IComparable";
        methods = {{"CompareTo", "int", PrimitiveType::Int, {"other"}}};
    } else if (name == "IEquatable") {
        ifaceName = "IEquatable";
        methods = {{"Equals", "bool", PrimitiveType::Bool, {"other"}}};
    } else if (name == "IEqualityComparer") {
        ifaceName = "IEqualityComparer";
        methods = {{"Equals", "bool", PrimitiveType::Bool, {"a", "b"}},
                   {"GetHashCode", "int", PrimitiveType::Int, {"value"}}};
    } else if (name == "IReadOnlySpan") {
        // §4.16 (#970): read-only span interface. Methods use generic T params
        // (erased to i32 in the itable ABI); the span box thunk ABI and the
        // needsConcrete fix in lowerCallOnInterface reconcile the i32↔ptr
        // mismatch for non-int element types at dispatch time. SequenceEqual
        // uses T as well — the caller always passes a boxed IReadOnlySpan<T>
        // (ptr), which needsConcrete adjusts from the declared i32 width.
        ifaceName = "IReadOnlySpan";
        methods = {{"IndexOf",       "int",  PrimitiveType::Int,  {"item"}},
                   {"Contains",      "bool", PrimitiveType::Bool, {"item"}},
                   {"SequenceEqual", "bool", PrimitiveType::Bool, {"other"}}};
        props   = {{"Length",  "int",  PrimitiveType::Int,  /*isIndexer=*/false, nullptr},
                   {"IsEmpty", "bool", PrimitiveType::Bool, /*isIndexer=*/false, nullptr},
                   {nullptr,   "T",    PrimitiveType::None, /*isIndexer=*/true,  "i"}};
    } else {
        return nullptr;
    }

    auto key = std::string(name);
    auto it = builtinInterfaceCache_.find(key);
    if (it != builtinInterfaceCache_.end()) return it->second;

    // ── AST nodes (owned, transitively, by builtinInterfaceAst_) ───────────────
    auto typeDecl = std::make_unique<ast::TypeDecl>();
    typeDecl->kind = ast::TypeDeclKind::Interface;
    typeDecl->visibility = ast::Visibility::Public;
    typeDecl->name = ifaceName;  // string-literal string_view (stable)
    typeDecl->genericParams.push_back(ast::GenericParam{/*variance=*/"", "T"});

    // Build one abstract MethodDecl per contract member, remembering the raw
    // return-type node (to pre-resolve) and each raw param-type node (to back the
    // ParameterSymbol::declType) before the decls are moved into typeDecl.
    struct RawMethod {
        ast::MethodDecl* md;
        ast::Type* ret;
        std::vector<ast::Type*> paramTypes;
    };
    std::vector<RawMethod> raws;
    for (const auto& spec : methods) {
        auto methodDecl = std::make_unique<ast::MethodDecl>();
        methodDecl->visibility = ast::Visibility::Public;
        methodDecl->name = spec.methodName;
        methodDecl->bodyKind = ast::MethodBodyKind::Abstract;
        auto retType = std::make_unique<ast::Type>();
        retType->kind = ast::TypeKind::Named;
        retType->name = std::make_unique<ast::QualifiedName>();
        retType->name->parts.push_back(spec.retName);
        methodDecl->returnType = std::move(retType);
        RawMethod rm;
        rm.ret = methodDecl->returnType.get();
        for (const char* pname : spec.paramNames) {
            ast::Param param;
            param.name = pname;
            auto paramType = std::make_unique<ast::Type>();
            paramType->kind = ast::TypeKind::Named;
            paramType->name = std::make_unique<ast::QualifiedName>();
            paramType->name->parts.push_back("T");  // generic param → substituted per use
            param.type = std::move(paramType);
            rm.paramTypes.push_back(param.type.get());
            methodDecl->params.push_back(std::move(param));
        }
        rm.md = methodDecl.get();
        raws.push_back(std::move(rm));
        typeDecl->members.push_back(std::move(methodDecl));
    }
    // Build one PropertyDecl per PropSpec (following the MethodDecls so the
    // member order matches the itable ordering: methods first, properties after).
    struct RawProp {
        ast::PropertyDecl* pd;
        ast::Type* ret;        // return-type node to pre-resolve (null = generic T)
        ast::Type* indexParam; // non-null for indexer: the `i: int` param type
    };
    std::vector<RawProp> rawProps;
    for (const auto& pspec : props) {
        auto propDecl = std::make_unique<ast::PropertyDecl>();
        propDecl->visibility = ast::Visibility::Public;
        if (pspec.propName) propDecl->name = pspec.propName;
        propDecl->bodyKind = ast::PropertyBodyKind::Auto; // placeholder; interface has no body
        propDecl->modifiers.push_back("readonly"); // per §4.16: all IReadOnlySpan props are read-only
        auto retType = std::make_unique<ast::Type>();
        retType->kind = ast::TypeKind::Named;
        retType->name = std::make_unique<ast::QualifiedName>();
        retType->name->parts.push_back(pspec.retName);  // "int"/"bool"/"T"
        propDecl->type = std::move(retType);
        propDecl->isIndexer = pspec.isIndexer;
        RawProp rp;
        rp.ret = propDecl->type.get();
        rp.indexParam = nullptr;
        if (pspec.isIndexer && pspec.indexParamName) {
            ast::Param idxPar;
            idxPar.name = pspec.indexParamName;
            auto idxTy = std::make_unique<ast::Type>();
            idxTy->kind = ast::TypeKind::Named;
            idxTy->name = std::make_unique<ast::QualifiedName>();
            idxTy->name->parts.push_back("int");
            rp.indexParam = idxTy.get();
            idxPar.type = std::move(idxTy);
            propDecl->indexerParams.push_back(std::move(idxPar));
        }
        rp.pd = propDecl.get();
        rawProps.push_back(rp);
        typeDecl->members.push_back(std::move(propDecl));
    }
    ast::TypeDecl* tdRaw = typeDecl.get();
    builtinInterfaceAst_.push_back(std::move(typeDecl));

    // ── Symbols (via makeSymbol → land in symbols_, so codegen picks them up) ──
    auto* ts = makeSymbol<TypeSymbol>();
    ts->kind = ast::TypeDeclKind::Interface;
    ts->name = ifaceName;
    ts->visibility = ast::Visibility::Public;
    ts->decl = tdRaw;
    auto* tparam = makeSymbol<GenericParamSymbol>();
    tparam->name = "T";
    ts->genericParams.push_back(tparam);
    for (std::size_t i = 0; i < methods.size(); ++i) {
        const MethodSpec& spec = methods[i];
        const RawMethod& rm = raws[i];
        auto* ms = makeSymbol<MethodSymbol>();
        ms->name = spec.methodName;
        ms->visibility = ast::Visibility::Public;
        ms->parent = ts;
        ms->decl = rm.md;
        for (std::size_t p = 0; p < spec.paramNames.size(); ++p) {
            ms->params.push_back(
                buildBuiltinInterfaceParamSymbol(*this, rm.md->params[p]));
        }
        ts->members.push_back(ms);

        // The return-type node must resolve to int/bool so methodFnType's
        // lowerType(returnType) yields i32/i1, matching the implementing class's
        // method signature (else the itable fnTy disagrees — risk #3).
        setResolvedType(rm.ret, primitiveTypeRef(spec.retPrim, false));
    }
    // Build one PropertySymbol per PropSpec. These land in ts->members after the
    // MethodSymbols so the itable ordering (methods, then properties) matches
    // declareVtables Pass 0 which walks members in the same order.
    for (std::size_t j = 0; j < props.size(); ++j) {
        const PropSpec& pspec = props[j];
        const RawProp&  rp    = rawProps[j];
        auto* ps = makeSymbol<PropertySymbol>();
        ps->name = pspec.propName ? std::string_view(pspec.propName)
                                  : std::string_view();
        ps->visibility = ast::Visibility::Public;
        ps->parent = ts;
        ps->decl   = rp.pd;
        ps->isIndexer = pspec.isIndexer;
        if (pspec.isIndexer && pspec.indexParamName && rp.indexParam) {
            auto* ip = makeSymbol<ParameterSymbol>();
            ip->name = pspec.indexParamName;
            ip->declType = rp.indexParam;
            ps->indexerParams.push_back(ip);
        }
        ts->members.push_back(ps);
        // Pre-resolve non-generic property return types to their concrete LLVM
        // type. PrimitiveType::None signals generic T (indexer); that stays
        // unresolved so lowerIndexerGet can recover the concrete element type
        // from the receiver's genericArgs[0] at each use site.
        if (pspec.retPrim != PrimitiveType::None)
            setResolvedType(rp.ret, primitiveTypeRef(pspec.retPrim, false));
    }

    // #3139: see the sibling assignment in the §11.8 branch above.
    ts->isSynthesizedBuiltinInterface = true;
    builtinInterfaceCache_[key] = ts;
    return ts;
}

} // namespace bezel::sema
