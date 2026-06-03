#include "sema/TypeResolution_impl.hpp"

namespace bezel::sema {

int Resolver::builtinTypeArity(std::string_view n) {
    if (n == "Type" || n == "IDisposable" || n == "IAsyncDisposable" ||
        n == "Subscription" || n == "CancellationToken") {
        return 0;
    }
    // Slice S25 (§31.4): language-built-in exception types, all
    // nullary, available without import.
    if (n == "Exception" || n == "ArgumentException" ||
        n == "ArgumentNullException" || n == "ArithmeticException" ||
        n == "OverflowException" || n == "DivideByZeroException" ||
        n == "InvalidOperationException" ||
        n == "NotImplementedException" ||
        n == "NullReferenceException" ||
        n == "IndexOutOfRangeException" ||
        n == "MatchFailureException" ||
        n == "OperationCanceledException" ||
        n == "TypeInitializationException" ||
        n == "FormatException" || n == "StackOverflowException") {
        return 0;
    }
    if (n == "Task") return kBuiltinTaskLike;
    if (n == "Array" || n == "OwnedArray" || n == "OwnedTask" ||
        n == "IEnumerable" || n == "IEnumerator" ||
        n == "IAsyncEnumerable" || n == "IAsyncEnumerator" ||
        n == "OwnedEnumerable" || n == "OwnedEnumerator" ||
        n == "OwnedAsyncEnumerable" || n == "OwnedAsyncEnumerator" ||
        n == "IReadOnlyCollection" || n == "IReadOnlyList" ||
        n == "ICollection" || n == "IList" ||
        n == "IOwnedCollection" || n == "IOwnedList" ||
        n == "IEvent" || n == "IAsyncEvent" ||
        // §3 stdlib collection types (arity 1):
        n == "Stack" || n == "OwnedStack" ||
        n == "Queue" || n == "OwnedQueue" ||
        n == "Set" || n == "OwnedSet" ||
        n == "LinkedList" || n == "OwnedLinkedList") {
        return 1;
    }
    // §3 stdlib two-parameter collection types (arity 2):
    if (n == "Dictionary" || n == "OwnedDictionary") return 2;
    // §3 nullary stdlib classes:
    if (n == "StringBuilder" || n == "Random" ||
        n == "DateTime" || n == "TimeSpan" ||
        n == "File" || n == "Directory" || n == "Path" ||
        n == "Encoding" || n == "Linq") return 0;
    // §32.5 / stdlib §10: FFI-boundary types. IntPtr/UIntPtr are
    // compiler-intrinsic value types (pointer-sized integers); CString,
    // WString, RawBytes are FFI-boundary classes. They have no user
    // TypeDecl, so — like the §3 built-ins above — they resolve to a
    // Named TypeRef with a null declaration. All are nullary. Making them
    // resolvable lets §32 extern signatures using them compile (§32.5);
    // before this they errored "unresolved type name".
    if (n == "IntPtr" || n == "UIntPtr" ||
        n == "CString" || n == "WString" || n == "RawBytes") return 0;
    return kNotBuiltin;
}

void Resolver::resolveTopDecl(const ast::TopDecl& decl, Scope* parent) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&decl)) {
        resolveTypeDecl(*td, parent);
    } else if (auto* alias = dynamic_cast<const ast::TypeAlias*>(&decl)) {
        Scope aliasScope;
        aliasScope.parent = parent;
        for (const auto& g : alias->genericParams) {
            // Register a temporary symbol for lookup; we don't need to
            // create a TypeSymbol stand-in because the alias body only
            // ever references the name.
            aliasScope.names[g.name] = nullptr; // marker for "is generic"
        }
        if (alias->aliasedType) {
            resolveType(*alias->aliasedType, &aliasScope);
        }
        for (const auto& gc : alias->genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &aliasScope);
            }
        }
    } else if (auto* ext =
                   dynamic_cast<const ast::ExtensionDecl*>(&decl)) {
        Scope extScope;
        extScope.parent = parent;
        if (ext->targetType) resolveType(*ext->targetType, &extScope);
        for (const auto& gc : ext->genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &extScope);
            }
        }
        for (const auto& m : ext->members) {
            if (m) resolveMember(*m, &extScope);
        }
    } else if (auto* xb =
                   dynamic_cast<const ast::ExternBlock*>(&decl)) {
        for (const auto& d : xb->methods) {
            for (const auto& p : d.params) {
                if (p.type) {
                    resolveType(*p.type, parent);
                    // per §32.5: only FFI-compatible types may appear in an
                    // extern method signature. A parameter is never a return
                    // position, so the unit type `()` is not admitted here.
                    checkExternType(*p.type, /*returnPosition=*/false,
                                    std::string("parameter `") +
                                        std::string(p.name) + "`");
                }
            }
            if (d.returnType) {
                resolveType(*d.returnType, parent);
                // per §32.5: the return slot may additionally be the unit
                // type `()`. (An omitted `: T` clause leaves returnType null
                // and is already a unit-returning method — §32.5.)
                checkExternType(*d.returnType, /*returnPosition=*/true,
                                "return type");
            }
        }
    }
}

void Resolver::resolveTypeDecl(const ast::TypeDecl& td, Scope* parent) {
    Scope typeScope;
    typeScope.parent = parent;
    // Register the type's generic params + any nested types so members
    // can reference them.
    for (const auto& g : td.genericParams) {
        typeScope.names[g.name] = nullptr;  // marker for generic-param
    }
    for (const auto& m : td.members) {
        if (auto* nt =
                dynamic_cast<const ast::NestedTypeMember*>(m.get())) {
            if (nt->decl) {
                // Look up the corresponding TypeSymbol by name in the
                // outer type's members — slice 1 stored them already.
                // For now we just register the name as visible.
                typeScope.names[nt->decl->name] = nullptr;
            }
        }
    }
    // Base types. Slice S5: cache the resolved TypeSymbols on the
    // declaring TypeSymbol so member lookup can walk them.
    TypeSymbol* tsym = nullptr;
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                if (ts->decl == &td) { tsym = ts; break; }
            }
        }
    }
    // Slice S10: a class has at most one class-kind base; the
    // rest must be interfaces. Records and enums don't take
    // base classes (we don't enforce here — that's a separate
    // syntactic/structural check).
    std::size_t classBaseCount = 0;
    for (const auto& bt : td.baseTypes) {
        if (!bt) continue;
        TypeRef* ref = resolveType(*bt, &typeScope);
        if (tsym && ref && ref->kind == TypeRef::Kind::Named &&
            ref->declaration) {
            tsym->baseTypes.push_back(ref->declaration);
            tsym->baseTypeRefs.push_back(ref);  // preserve generic args
            TypeSymbol* base = ref->declaration;
            // Slice S9: a sealed class cannot be inherited from.
            if (base->kind == ast::TypeDeclKind::Class) {
                ++classBaseCount;
                if (td.kind == ast::TypeDeclKind::Class &&
                    classBaseCount > 1) {
                    ctx_.diagnostics().report(
                        Severity::Error, bt->span.begin,
                        std::string("class `") + std::string(td.name) +
                        "` already has a base class; only the "
                        "first base may be a class (single-"
                        "inheritance — §11.1)");
                }
                bool sealed = false;
                for (auto mod : base->modifiers) {
                    if (mod == "sealed") { sealed = true; break; }
                }
                if (sealed) {
                    ctx_.diagnostics().report(
                        Severity::Error, bt->span.begin,
                        std::string("cannot inherit from sealed class `") +
                        std::string(base->name) + "` (§12.4)");
                }
            }
        }
    }
    // Generic constraints.
    for (const auto& gc : td.genericConstraints) {
        for (const auto& item : gc.items) {
            if (item.typeArg) resolveType(*item.typeArg, &typeScope);
        }
    }
    // Positional fields (records).
    for (const auto& pf : td.positionalFields) {
        if (pf.type) resolveType(*pf.type, &typeScope);
    }
    // SerializedClassClause polymorphic case types.
    if (td.serializedClass) {
        for (const auto& pc : td.serializedClass->polyCases) {
            if (pc.type) resolveType(*pc.type, &typeScope);
        }
    }
    // Members.
    for (const auto& m : td.members) {
        if (m) resolveMember(*m, &typeScope);
    }
    // Enum constants don't carry types syntactically (their `:= value`
    // is a Const expression; types come from the underlying-int).
}

void Resolver::resolveMember(const ast::Member& m, Scope* parent) {
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
        if (fd->type) resolveType(*fd->type, parent);
        // Issue #163: resolve types embedded in the initializer
        // expression (e.g. `new T()`), so a class-typed field
        // initializer is fully typed — mirrors what ctor/method
        // bodies get via resolveTypesInBody below.
        if (fd->initializer) resolveTypesInExpr(*fd->initializer, parent);
    } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        Scope methodScope;
        methodScope.parent = parent;
        for (const auto& g : md->genericParams) {
            methodScope.names[g.name] = nullptr;
        }
        for (const auto& p : md->params) {
            if (p.type) resolveType(*p.type, &methodScope);
        }
        if (md->returnType) resolveType(*md->returnType, &methodScope);
        for (const auto& gc : md->genericConstraints) {
            for (const auto& item : gc.items) {
                if (item.typeArg) resolveType(*item.typeArg, &methodScope);
            }
        }
        // Slice 3d: also resolve type ascriptions inside the method
        // body (LocalDecl `: Type`, foreach iteration type, …) so
        // ExprTypecheck can pick them up to type identifier uses.
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            resolveTypesInBody(md->stmts, &methodScope);
        } else if (md->bodyKind == ast::MethodBodyKind::Expression &&
                   md->exprBody) {
            // Resolve types embedded in an expression body too (e.g.
            // `=> new T()`), mirroring block bodies — otherwise a
            // factory method's `new T()` has no resolved type.
            resolveTypesInExpr(*md->exprBody, &methodScope);
        }
    } else if (auto* od =
                   dynamic_cast<const ast::OperatorDecl*>(&m)) {
        Scope methodScope;
        methodScope.parent = parent;
        for (const auto& p : od->params) {
            if (p.type) resolveType(*p.type, &methodScope);
        }
        if (od->returnType) resolveType(*od->returnType, &methodScope);
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            resolveTypesInBody(od->stmts, &methodScope);
        } else if (od->bodyKind == ast::MethodBodyKind::Expression &&
                   od->exprBody) {
            resolveTypesInExpr(*od->exprBody, &methodScope);
        }
    } else if (auto* cd =
                   dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        Scope ctorScope;
        ctorScope.parent = parent;
        for (const auto& p : cd->params) {
            if (p.type) resolveType(*p.type, &ctorScope);
        }
        resolveTypesInBody(cd->body, &ctorScope);
    } else if (auto* dd =
                   dynamic_cast<const ast::DestructorDecl*>(&m)) {
        resolveTypesInBody(dd->body, parent);
    } else if (auto* pd =
                   dynamic_cast<const ast::PropertyDecl*>(&m)) {
        if (pd->type) resolveType(*pd->type, parent);
        for (const auto& p : pd->indexerParams) {
            if (p.type) resolveType(*p.type, parent);
        }
    } else if (auto* ed = dynamic_cast<const ast::EventDecl*>(&m)) {
        if (ed->type) resolveType(*ed->type, parent);
    } else if (auto* cd2 = dynamic_cast<const ast::ConstDecl*>(&m)) {
        if (cd2->type) resolveType(*cd2->type, parent);
    } else if (auto* nt =
                   dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) resolveTypeDecl(*nt->decl, parent);
    }
}

Symbol* Resolver::lookupName(std::string_view name, Scope* scope) {
    for (Scope* s = scope; s; s = s->parent) {
        auto it = s->names.find(name);
        if (it != s->names.end()) return it->second;
    }
    return nullptr;
}

TypeRef* Resolver::resolveType(const ast::Type& t, Scope* scope) {
    auto* ref = ctx_.makeTypeRef();
    ref->astNode = &t;
    ref->nullable = t.nullable;
    switch (t.kind) {
        case ast::TypeKind::Unit:
            ref->kind = TypeRef::Kind::Unit;
            break;
        case ast::TypeKind::InlineArray:
            ref->kind = TypeRef::Kind::InlineArray;
            ref->arraySize = t.arraySize.get();
            ref->arrayOwned = t.arrayOwned;
            if (t.arrayElement) {
                ref->arrayElement = resolveType(*t.arrayElement, scope);
            }
            break;
        case ast::TypeKind::Function:
            ref->kind = TypeRef::Kind::Function;
            ref->funcReturnOwned = t.funcReturnOwned;
            for (const auto& p : t.funcParams) {
                TypeRef::FuncParam fp;
                fp.modifier = p.modifier;
                fp.name = p.name;
                if (p.type) fp.type = resolveType(*p.type, scope);
                ref->funcParams.push_back(std::move(fp));
            }
            if (t.funcReturn) {
                ref->funcReturn = resolveType(*t.funcReturn, scope);
            }
            break;
        case ast::TypeKind::Named:
            resolveNamed(t, scope, *ref);
            break;
    }
    ctx_.setResolvedType(&t, ref);
    return ref;
}

void Resolver::resolveNamed(const ast::Type& t, Scope* scope, TypeRef& ref) {
    if (!t.name || t.name->parts.empty()) {
        ref.kind = TypeRef::Kind::Error;
        return;
    }
    std::string_view first = t.name->parts.front();
    // Primitive check first.
    if (t.name->parts.size() == 1) {
        PrimitiveType prim = primitiveFromName(first);
        if (prim != PrimitiveType::None) {
            ref.kind = TypeRef::Kind::Primitive;
            ref.primitive = prim;
            // Primitives shouldn't have generic args (sema enforces);
            // we still resolve them so the AST is fully visited.
            for (const auto& ga : t.genericArgs) {
                if (ga) resolveType(*ga, scope);
            }
            if (!t.genericArgs.empty()) {
                ctx_.diagnostics().report(
                    Severity::Error, t.span.begin,
                    "primitive type cannot take generic arguments");
            }
            return;
        }
    }
    // Look up the first part.
    Symbol* sym = lookupName(first, scope);
    if (!sym) {
        // Unknown — but a null lookup may also mean "name is in scope as
        // a generic-param marker" (we used nullptr to indicate that).
        // Distinguish by checking if the key exists.
        bool isGeneric = false;
        for (Scope* s = scope; s; s = s->parent) {
            if (s->names.count(first)) { isGeneric = true; break; }
        }
        if (isGeneric) {
            // Generic parameter — record as a Named with no declaration.
            // Sema slice 3+ will treat generic-param TypeRefs specially.
            ref.kind = TypeRef::Kind::Named;
            ref.declaration = nullptr;
            return;
        }
        // Slice S24 (§2.3): always-available built-in type names
        // resolve without import. They have no user TypeSymbol, so
        // we record a Named ref with a null declaration and verify
        // generic arity against the spec's declared form.
        if (t.name->parts.size() == 1) {
            int arity = builtinTypeArity(first);
            if (arity != kNotBuiltin) {
                ref.kind = TypeRef::Kind::Named;
                ref.declaration = nullptr;
                int n = static_cast<int>(t.genericArgs.size());
                bool ok = (arity == kBuiltinTaskLike) ? (n == 0 || n == 1)
                                                      : (n == arity);
                if (!ok) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        std::string("generic-argument count does not "
                                    "match built-in type `") +
                        std::string(first) + "`");
                }
                for (const auto& ga : t.genericArgs) {
                    if (!ga) continue;
                    ref.genericArgs.push_back(resolveType(*ga, scope));
                }
                return;
            }
        }
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            std::string("unresolved type name `") +
            std::string(first) + "`");
        ref.kind = TypeRef::Kind::Error;
        return;
    }
    // For multi-part QualifiedNames (`Color.Red` style), we'd descend
    // into the looked-up symbol's members. Slice 2 supports only the
    // single-identifier case and a one-deep dotted name (e.g.
    // `System.Object` resolving to Object). For now we accept the
    // first part and ignore the rest if it's a TypeSymbol (the only
    // dotted-name case sema slice 2 needs is `Module.Name`-style).
    auto* tsym = dynamic_cast<TypeSymbol*>(sym);
    if (!tsym) {
        // Could be a TypeAliasSymbol or other. For slice 2, treat as
        // a Named pointing to no declaration (sema slice 3+ resolves
        // aliases).
        ref.kind = TypeRef::Kind::Named;
        ref.declaration = nullptr;
    } else {
        ref.kind = TypeRef::Kind::Named;
        ref.declaration = tsym;
        // Generic-arity check.
        if (!t.genericArgs.empty() &&
            t.genericArgs.size() != tsym->genericParams.size()) {
            ctx_.diagnostics().report(
                Severity::Error, t.span.begin,
                "generic-argument count does not match type's "
                "generic-parameter count");
        }
    }
    for (const auto& ga : t.genericArgs) {
        if (!ga) continue;
        TypeRef* arg = resolveType(*ga, scope);
        ref.genericArgs.push_back(arg);
    }
    // §14.2 / Appendix C.4: validate that each supplied type argument
    // satisfies the constraints declared on its type parameter. Only for a
    // resolved user TypeSymbol whose arity matches (a mismatch is already
    // diagnosed above; checking a misaligned arg list would be noise).
    if (tsym && !t.genericArgs.empty() &&
        t.genericArgs.size() == tsym->genericParams.size()) {
        checkGenericConstraints(t, tsym, ref.genericArgs, scope);
    }
}

void Resolver::resolveTypesInBody(
    const std::vector<std::unique_ptr<ast::Stmt>>& body, Scope* scope) {
    for (const auto& s : body) {
        if (s) resolveTypesInStmt(*s, scope);
    }
}

void Resolver::resolveTypesInStmt(const ast::Stmt& s, Scope* scope) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->type) resolveType(*ld->type, scope);
        if (ld->initializer) resolveTypesInExpr(*ld->initializer, scope);
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->type) resolveType(*lc->type, scope);
        if (lc->value) resolveTypesInExpr(*lc->value, scope);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) resolveTypesInExpr(*rs->value, scope);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) resolveTypesInExpr(*es->expr, scope);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target) resolveTypesInExpr(*as->target, scope);
        if (as->value) resolveTypesInExpr(*as->value, scope);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) resolveTypesInExpr(*ds->value, scope);
        return;
    }
    if (auto* tr = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (tr->value) resolveTypesInExpr(*tr->value, scope);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            resolveTypesInExpr(*ifs->ifBranch.condition, scope);
        resolveTypesInBody(ifs->ifBranch.body, scope);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) resolveTypesInExpr(*el.condition, scope);
            resolveTypesInBody(el.body, scope);
        }
        resolveTypesInBody(ifs->elseBody, scope);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) resolveTypesInExpr(*ws->condition, scope);
        resolveTypesInBody(ws->body, scope);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->varType) resolveType(*fs->varType, scope);
        if (fs->iterable) resolveTypesInExpr(*fs->iterable, scope);
        if (fs->withExpr) resolveTypesInExpr(*fs->withExpr, scope);
        resolveTypesInBody(fs->body, scope);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        resolveTypesInBody(dob->body, scope);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        resolveTypesInBody(ts->body, scope);
        for (const auto& arm : ts->catches) {
            // Resolve the catch pattern's type(s) and guard, not just the
            // body — else `catch T => …` leaves T with no resolved
            // TypeRef and codegen cannot match the arm.
            for (const auto& ty : arm.types) {
                if (ty) resolveType(*ty, scope);
            }
            if (arm.guard) resolveTypesInExpr(*arm.guard, scope);
            if (arm.body) resolveTypesInStmt(*arm.body, scope);
        }
        resolveTypesInBody(ts->finallyBody, scope);
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) resolveTypesInExpr(*is->target, scope);
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) resolveTypesInExpr(*ys->value, scope);
        return;
    }
}

void Resolver::resolveTypesInExpr(const ast::Expr& e, Scope* scope) {
    // First handle expressions that own a Type node directly.
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        if (ne->type) resolveType(*ne->type, scope);
        for (const auto& a : ne->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (te->testType) resolveType(*te->testType, scope);
        if (te->operand) resolveTypesInExpr(*te->operand, scope);
        return;
    }
    if (auto* to = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        if (to->queriedType) resolveType(*to->queriedType, scope);
        return;
    }
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        if (de->type) resolveType(*de->type, scope);
        return;
    }
    // Recurse into children for everything else.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) resolveTypesInExpr(*be->left, scope);
        if (be->right) resolveTypesInExpr(*be->right, scope);
        return;
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->operand) resolveTypesInExpr(*ue->operand, scope);
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) resolveTypesInExpr(*ma->target, scope);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nc->target) resolveTypesInExpr(*nc->target, scope);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) resolveTypesInExpr(*ce->callee, scope);
        for (const auto& a : ce->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) resolveTypesInExpr(*nc->callee, scope);
        for (const auto& a : nc->args) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) resolveTypesInExpr(*ix->target, scope);
        for (const auto& a : ix->indices) {
            if (a.value) resolveTypesInExpr(*a.value, scope);
        }
        return;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) resolveTypesInExpr(*bg->operand, scope);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (ce->condition) resolveTypesInExpr(*ce->condition, scope);
        if (ce->thenValue) resolveTypesInExpr(*ce->thenValue, scope);
        if (ce->elseValue) resolveTypesInExpr(*ce->elseValue, scope);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) resolveTypesInExpr(*th->operand, scope);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // Lambda parameter types are themselves Types when present.
        for (const auto& p : lam->params) {
            if (p.type) resolveType(*p.type, scope);
        }
        if (lam->exprBody) resolveTypesInExpr(*lam->exprBody, scope);
        for (const auto& s : lam->stmts) {
            if (s) resolveTypesInStmt(*s, scope);
        }
        return;
    }
    if (auto* cae = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (cae->scrutinee) resolveTypesInExpr(*cae->scrutinee, scope);
        for (const auto& arm : cae->arms) {
            if (arm.guard) resolveTypesInExpr(*arm.guard, scope);
            if (arm.exprBody) resolveTypesInExpr(*arm.exprBody, scope);
            for (const auto& s : arm.stmts) {
                if (s) resolveTypesInStmt(*s, scope);
            }
        }
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) resolveTypesInExpr(*we->source, scope);
        for (const auto& ia : we->assigns) {
            if (ia.value) resolveTypesInExpr(*ia.value, scope);
        }
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys) {
            if (k) resolveTypesInExpr(*k, scope);
        }
        for (const auto& v : cl->elements) {
            if (v) resolveTypesInExpr(*v, scope);
        }
        return;
    }
    // §18.5: InterpolatedStringExpr — each expression part may contain type
    // nodes (e.g. `{x is IAnimal}` has a testType; `{new T(...)}`has a new-type).
    // Resolve them so that lowerTypeTest / lowerNew find the resolved TypeRef.
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& part : is->parts) {
            if (part.expr) resolveTypesInExpr(*part.expr, scope);
        }
        return;
    }
    // Literals + Identifier + ThisBaseTypeOf — no nested Types/Exprs.
}

}  // namespace bezel::sema
