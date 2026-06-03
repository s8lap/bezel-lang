#include "sema/ExprTypecheck.hpp"
#include "sema/ExprTypecheck_impl.hpp"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// The ExprTC class is declared in ExprTypecheck_impl.hpp; its method
// definitions live across this and the sibling ExprTypecheck_*.cpp
// translation units. This file holds the public entry point, the worker's
// driver (run / visitTopDecl / visitMember / visitStmt), and the small
// shared helpers.

void ExprTC::run(const ast::SourceFile& src) {
    moduleScope_ = std::make_unique<Scope>();
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (!child->name.empty()) {
                moduleScope_->names[child->name] = child;
            }
            // §32: an extern block's methods are module-scoped names so
            // that calls to them resolve like ordinary functions.
            if (auto* xb = dynamic_cast<ExternBlockSymbol*>(child)) {
                for (Symbol* d : xb->decls) {
                    if (d && !d->name.empty())
                        moduleScope_->names[d->name] = d;
                }
            }
        }
    }
    for (const auto& decl : src.topDecls) {
        if (!decl) continue;
        visitTopDecl(*decl);
    }
}

// Return (or lazily create) a synthetic TypeSymbol for a built-in class.
TypeSymbol* ExprTC::builtinClassSym(std::string_view name) {
    auto key = std::string(name);
    auto it = builtinClassCache_.find(key);
    if (it != builtinClassCache_.end()) return it->second;
    auto* ts = ctx_.makeSymbol<TypeSymbol>();
    ts->kind = ast::TypeDeclKind::Class;
    ts->name = name;
    ts->decl = nullptr;
    builtinClassCache_[key] = ts;
    return ts;
}

// Slice S29: report when a bare identifier inside a field
// initializer resolves to a non-static member of the current type
// (an implicit-`this` reference, forbidden by §22.8). Static
// members, class constants, enum constants, locals, parameters,
// types, and free names are all permitted.
void ExprTC::checkNotInstanceMember(const ast::IdentifierExpr& id, Symbol* sym) {
    auto* mem = dynamic_cast<MemberSymbol*>(sym);
    if (!mem) return;
    // ConstMemberSymbol is implicitly static (a class const).
    if (dynamic_cast<ConstMemberSymbol*>(sym)) return;
    bool isStatic = false;
    for (auto mod : mem->modifiers) {
        if (mod == "static") { isStatic = true; break; }
    }
    if (isStatic) return;
    // Only flag members of the type we're initializing.
    if (currentType_ && mem->parent != currentType_) return;
    ctx_.diagnostics().report(
        Severity::Error, id.span.begin,
        std::string("a field initializer may not reference instance "
                    "member `") +
        std::string(id.name) + "` (§22.8)");
}

// Slice DR1 (§21.7): does control definitely leave this statement
// list abruptly (return / throw / non-terminating loop) before
// reaching its end? Conservatively returns true only when certain,
// so a value-method check that flags `!bodyAlwaysExits` never
// false-positives on a body that does always exit (modulo the
// noreturn-call case, which mainstream compilers also flag).
bool ExprTC::bodyAlwaysExits(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (s && stmtAlwaysExits(*s)) return true;
    }
    return false;
}

// §29.3: check if a statement list contains any yield return/break
// (making it an iterator method body that is exempt from DR1).
bool ExprTC::stmtsContainYield(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (!s) continue;
        if (dynamic_cast<const ast::YieldStmt*>(s.get())) return true;
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            if (stmtsContainYield(fs->body)) return true;
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get()))
            if (stmtsContainYield(ws->body)) return true;
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (stmtsContainYield(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (stmtsContainYield(el.body)) return true;
            if (ifs->hasElse && stmtsContainYield(ifs->elseBody)) return true;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get()))
            if (stmtsContainYield(db->body)) return true;
    }
    return false;
}

bool ExprTC::stmtAlwaysExits(const ast::Stmt& s) {
    if (dynamic_cast<const ast::ReturnStmt*>(&s)) return true;
    if (dynamic_cast<const ast::ThrowStmt*>(&s)) return true;
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (!ifs->hasElse) return false;  // false edge falls through
        if (!bodyAlwaysExits(ifs->ifBranch.body)) return false;
        for (const auto& el : ifs->elifBranches) {
            if (!bodyAlwaysExits(el.body)) return false;
        }
        return bodyAlwaysExits(ifs->elseBody);
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        // `while true do … end` without a reachable break never
        // completes normally. We conservatively treat a literal-true
        // condition as non-terminating (a break would let it fall
        // through, which would only make us UNDER-report — never a
        // false positive).
        auto* lit =
            dynamic_cast<const ast::BoolLiteralExpr*>(ws->condition.get());
        return lit && lit->value;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        return bodyAlwaysExits(dob->body);
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        // §19.6: a non-exhaustive case STATEMENT falls through on no
        // match. We can only be sure it exits when there is an
        // unguarded `_` arm (full cover) AND every arm body exits.
        bool hasWildcard = false;
        for (const auto& arm : cs->arms) {
            if (!armBodyAlwaysExits(arm)) return false;
            if (!arm.guard &&
                dynamic_cast<const ast::DiscardPattern*>(
                    arm.pattern.get())) {
                hasWildcard = true;
            }
        }
        return hasWildcard;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        // A finally that always exits dominates. Otherwise the try
        // body and every catch arm must always exit.
        if (ts->hasFinally && bodyAlwaysExits(ts->finallyBody)) {
            return true;
        }
        if (!bodyAlwaysExits(ts->body)) return false;
        for (const auto& arm : ts->catches) {
            if (!arm.body || !stmtAlwaysExits(*arm.body)) return false;
        }
        return true;
    }
    return false;  // loops that may not run, plain stmts, etc.
}

bool ExprTC::armBodyAlwaysExits(const ast::CaseArm& arm) {
    if (arm.bodyKind == ast::CaseArmBodyKind::Block) {
        return bodyAlwaysExits(arm.stmts);
    }
    // Expression-bodied arm in statement position discards its value
    // and falls through; treat as non-exiting.
    return false;
}

// Slice S21: does a callable with this return-type ascription
// produce a value? No ascription → no (unit). An ascription that
// resolves to the unit type → no. Anything else → yes.
bool ExprTC::returnsValue(const ast::Type* rt) {
    if (!rt) return false;
    TypeRef* r = ctx_.resolvedType(*rt);
    if (r && r->kind == TypeRef::Kind::Unit) return false;
    return true;
}

// Slice S21 (§21.7) return-statement form rules:
//  - constructor / destructor: `return expression;` is an error.
//  - value-returning method: bare `return;` is an error.
//  - unit method: `return expression;` is an error.
// The "every exit path returns" coverage rule needs flow analysis
// and is left to a definite-return slice.
void ExprTC::checkReturnForm(const ast::ReturnStmt& rs) {
    switch (returnCtx_) {
        case ReturnCtx::None:
            return;
        case ReturnCtx::CtorDtor:
            if (rs.value) {
                ctx_.diagnostics().report(
                    Severity::Error, rs.span.begin,
                    "a constructor or destructor may not return a "
                    "value; use `return;` (§21.7)");
            }
            return;
        case ReturnCtx::ValueMethod:
            if (!rs.value) {
                ctx_.diagnostics().report(
                    Severity::Error, rs.span.begin,
                    "this callable has a return type — `return;` "
                    "must provide a value (§21.7)");
            }
            return;
        case ReturnCtx::VoidMethod:
            if (rs.value) {
                ctx_.diagnostics().report(
                    Severity::Error, rs.span.begin,
                    "this callable has no return type — `return` "
                    "may not carry a value (§21.7)");
            }
            return;
    }
}

// Slice T1: §5.1 numeric/char implicit-widening table. Returns
// true iff `from` implicitly widens to `to` (both primitives,
// distinct). Identity is handled by the caller.
bool ExprTC::primitiveWidens(PrimitiveType from, PrimitiveType to) {
    using P = PrimitiveType;
    auto isAny = [&](std::initializer_list<P> set) {
        for (P p : set) if (p == to) return true;
        return false;
    };
    switch (from) {
        case P::SByte:
            return isAny({P::Short, P::Int, P::Long, P::Float,
                          P::Double, P::Decimal});
        case P::Byte:
            return isAny({P::Short, P::UShort, P::Int, P::UInt,
                          P::Long, P::ULong, P::Float, P::Double,
                          P::Decimal});
        case P::Short:
            return isAny({P::Int, P::Long, P::Float, P::Double,
                          P::Decimal});
        case P::UShort:
            return isAny({P::Int, P::UInt, P::Long, P::ULong,
                          P::Float, P::Double, P::Decimal});
        case P::Int:
            return isAny({P::Long, P::Float, P::Double, P::Decimal});
        case P::UInt:
            return isAny({P::Long, P::ULong, P::Float, P::Double,
                          P::Decimal});
        case P::Long:
            return isAny({P::Float, P::Double, P::Decimal});
        case P::ULong:
            return isAny({P::Float, P::Double, P::Decimal});
        case P::Float:
            return isAny({P::Double});
        case P::Char:
            return isAny({P::Int, P::UInt, P::Long, P::ULong,
                          P::Float, P::Double, P::Decimal});
        default:
            return false;
    }
}

// Slice T4: is `from` a subtype of `to` — i.e. does `to` appear in
// `from`'s transitive base-type chain (base class or implemented
// interface)? BFS, cycle-guarded.
bool ExprTC::isSubtype(TypeSymbol* from, TypeSymbol* to) {
    std::vector<TypeSymbol*> frontier{from};
    std::unordered_set<TypeSymbol*> seen{from};
    std::size_t i = 0;
    while (i < frontier.size()) {
        TypeSymbol* t = frontier[i++];
        if (t == to) return true;
        for (TypeSymbol* b : t->baseTypes) {
            if (b && seen.insert(b).second) frontier.push_back(b);
        }
    }
    return false;
}

// Slices T1/T4: assignability check (§5.1, §7, §12 subtyping).
// Handles two cases and conservatively skips everything else
// (mixed primitive/named, generics, null, Object target) so it
// never false-positives while the type lattice is incomplete:
//   - primitive → primitive: identity or §5.1 widening.
//   - named → named (non-generic): identity or subclass/interface.
// A non-nullable target accepting a nullable source is rejected.
void ExprTC::checkAssignable(TypeRef* from, TypeRef* to,
                             SourceLocation loc, const std::string& what) {
    if (!from || !to) return;
    // `Object` (the root) accepts anything — never flag.
    if (to->kind == TypeRef::Kind::Primitive &&
        to->primitive == PrimitiveType::Object) {
        return;
    }
    auto reportNullable = [&] {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("the ") + what +
            " is nullable but the target type is non-nullable (§7)");
    };
    auto reportNotConvertible = [&] {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("the ") + what + " has a type that is not "
            "implicitly convertible to the target type (§5.1)");
    };
    if (from->kind == TypeRef::Kind::Primitive &&
        to->kind == TypeRef::Kind::Primitive) {
        if (from->primitive == PrimitiveType::Object) return;
        if (from->nullable && !to->nullable) { reportNullable(); return; }
        if (from->primitive == to->primitive) return;
        if (primitiveWidens(from->primitive, to->primitive)) return;
        reportNotConvertible();
        return;
    }
    if (from->kind == TypeRef::Kind::Named &&
        to->kind == TypeRef::Kind::Named &&
        from->declaration && to->declaration) {
        // Defer generic types to the variance slice.
        if (!from->genericArgs.empty() || !to->genericArgs.empty()) {
            return;
        }
        if (from->nullable && !to->nullable) { reportNullable(); return; }
        if (from->declaration == to->declaration) return;
        if (isSubtype(from->declaration, to->declaration)) return;
        reportNotConvertible();
        return;
    }
    // Mixed / generic-parameter / null-literal cases: skip.
}

// Slice S18: a condition must be `bool` (§21.2 if, §21.3 while,
// §20 if-expression). Only flags when the condition has a resolved
// primitive type that is not bool — conditions whose type isn't yet
// inferred (unresolved identifiers, un-typed calls) are left alone
// so this pass never produces false positives on incomplete typing.
void ExprTC::checkConditionIsBool(const ast::Expr& cond, std::string_view ctx) {
    TypeRef* t = ctx_.exprType(cond);
    if (!t) return;
    if (t->kind != TypeRef::Kind::Primitive) return;
    if (t->primitive == PrimitiveType::Bool) return;
    ctx_.diagnostics().report(
        Severity::Error, cond.span.begin,
        std::string("the ") + std::string(ctx) +
        " condition must be `bool` (§21.2)");
}

// Slice S7: diagnose assignment to a readonly field outside the
// declaring class's constructor. Tracks only the `this.<field>`
// target shape; `other.f := X` would need cross-object readonly
// tracking that we don't have yet (and rarely matters in practice).
void ExprTC::checkReadonlyAssignment(const ast::AssignmentStmt& as) {
    if (inConstructor_) return;
    if (!as.target) return;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(as.target.get());
    if (!ma) return;
    auto* tid = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    if (!tid || tid->name != "this") return;
    if (!currentType_) return;
    Symbol* m = lookupMember(currentType_, ma->member);
    auto* fs = dynamic_cast<FieldSymbol*>(m);
    if (!fs) return;
    // Check the readonly modifier on the field's MemberSymbol.
    for (auto mod : fs->modifiers) {
        if (mod == "readonly") {
            ctx_.diagnostics().report(
                Severity::Error, as.span.begin,
                std::string("cannot assign to readonly field `") +
                std::string(fs->name) +
                "` outside the declaring class's constructor "
                "(§22.8)");
            return;
        }
    }
}

// Slice S5: member lookup that walks the base-type chain.
// BFS over the inheritance graph (no cycle detection — the spec
// forbids cycles; sema's class-graph validation slice will catch
// them). First match wins; ambiguity resolution between sibling
// interfaces is a separate slice.
Symbol* ExprTC::lookupMember(TypeSymbol* type, std::string_view name) {
    std::vector<TypeSymbol*> frontier{type};
    std::size_t i = 0;
    while (i < frontier.size()) {
        TypeSymbol* t = frontier[i++];
        for (Symbol* m : t->members) {
            if (m && m->name == name) return m;
        }
        for (TypeSymbol* b : t->baseTypes) {
            if (b) frontier.push_back(b);
        }
    }
    return nullptr;
}

Symbol* ExprTC::lookupName(std::string_view name) {
    for (Scope* s = currentScope_; s; s = s->parent) {
        auto it = s->names.find(name);
        if (it != s->names.end()) return it->second;
    }
    // An unqualified member name resolves as an implicit-`this`
    // reference — search the enclosing type AND its base types
    // (§12 inheritance), mirroring lookupMember's transitive walk so
    // an inherited field/method is reachable without `this.`/`base.`.
    if (currentType_) {
        if (Symbol* m = lookupMember(currentType_, name)) return m;
    }
    return nullptr;
}

void ExprTC::visitTopDecl(const ast::TopDecl& d) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
        TypeSymbol* tsym = nullptr;
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                    if (ts->decl == td) { tsym = ts; break; }
                }
            }
        }
        TypeSymbol* save = currentType_;
        currentType_ = tsym;
        for (const auto& ec : td->enumConstants) {
            if (ec.value) visitExpr(*ec.value);
        }
        for (const auto& m : td->members) {
            if (m) visitMember(*m);
        }
        currentType_ = save;
    } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
        for (const auto& m : ext->members) if (m) visitMember(*m);
    }
}

// Param-symbol lookup for the in-progress method/ctor on currentType_.
const std::vector<Symbol*>* ExprTC::paramSymbolsOf(const ast::Member& m) const {
    if (!currentType_) return nullptr;
    for (Symbol* child : currentType_->members) {
        if (!child->decl) continue;
        if (child->decl == &m) {
            if (auto* ms = dynamic_cast<MethodSymbol*>(child))
                return &ms->params;
            if (auto* cs = dynamic_cast<ConstructorSymbol*>(child))
                return &cs->params;
        }
    }
    return nullptr;
}

// Build a method scope (params on top of module scope), then visit
// body. Locals are registered into this scope as visitStmt walks
// LocalDecls.
void ExprTC::runMethodBody(
    const std::vector<ast::Param>& params,
    const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
    const ast::Expr* exprBody,
    const std::vector<Symbol*>* paramSyms) {
    Scope methodScope;
    methodScope.parent = moduleScope_.get();
    if (paramSyms) {
        for (std::size_t i = 0;
             i < params.size() && i < paramSyms->size(); ++i) {
            if ((*paramSyms)[i] && !params[i].name.empty()) {
                methodScope.names[params[i].name] = (*paramSyms)[i];
            }
        }
    }
    Scope* save = currentScope_;
    currentScope_ = &methodScope;
    for (const auto& p : params) {
        if (p.defaultValue) visitExpr(*p.defaultValue);
    }
    if (exprBody) visitExpr(*exprBody);
    if (stmts) {
        for (const auto& s : *stmts) {
            if (s) visitStmt(*s);
        }
    }
    currentScope_ = save;
}

void ExprTC::visitMember(const ast::Member& m) {
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
        if (fd->initializer) {
            // Slice S29 (§22.8): a field initializer may not observe
            // instance state.
            bool save = inFieldInitializer_;
            inFieldInitializer_ = true;
            visitExpr(*fd->initializer);
            inFieldInitializer_ = save;
        }
    } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts =
            (md->bodyKind == ast::MethodBodyKind::Block) ? &md->stmts
                                                         : nullptr;
        const ast::Expr* expr =
            (md->bodyKind == ast::MethodBodyKind::Expression)
                ? md->exprBody.get() : nullptr;
        // Slice S6: enter static context if this method is static.
        bool isStatic = false;
        for (auto mod : md->modifiers) {
            if (mod == "static") { isStatic = true; break; }
        }
        bool save = inStaticMember_;
        inStaticMember_ = isStatic;
        ReturnCtx saveRet = returnCtx_;
        TypeRef* saveRetTy = currentReturnType_;
        returnCtx_ = returnsValue(md->returnType.get())
            ? ReturnCtx::ValueMethod : ReturnCtx::VoidMethod;
        currentReturnType_ = md->returnType
            ? ctx_.resolvedType(*md->returnType) : nullptr;
        runMethodBody(md->params, stmts, expr, paramSymbolsOf(m));
        // Slice DR1 (§21.7): a value-returning method with a block
        // body must return (or otherwise abruptly exit) on every
        // path. Abstract / expression bodies are exempt.
        // §29.3: iterator methods (containing yield return/break) are
        // exempt — the compiler synthesizes the return mechanism.
        bool isIteratorMethod = stmts && stmtsContainYield(*stmts);
        if (stmts && returnCtx_ == ReturnCtx::ValueMethod &&
            !isIteratorMethod &&
            !bodyAlwaysExits(*stmts)) {
            ctx_.diagnostics().report(
                Severity::Error, md->span.begin,
                std::string("method `") + std::string(md->name) +
                "` has a return type but does not return a value on "
                "every path (§21.7)");
        }
        currentReturnType_ = saveRetTy;
        returnCtx_ = saveRet;
        inStaticMember_ = save;
    } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts =
            (od->bodyKind == ast::MethodBodyKind::Block) ? &od->stmts
                                                         : nullptr;
        const ast::Expr* expr =
            (od->bodyKind == ast::MethodBodyKind::Expression)
                ? od->exprBody.get() : nullptr;
        ReturnCtx saveRet = returnCtx_;
        TypeRef* saveRetTy = currentReturnType_;
        returnCtx_ = returnsValue(od->returnType.get())
            ? ReturnCtx::ValueMethod : ReturnCtx::VoidMethod;
        currentReturnType_ = od->returnType
            ? ctx_.resolvedType(*od->returnType) : nullptr;
        runMethodBody(od->params, stmts, expr, paramSymbolsOf(m));
        currentReturnType_ = saveRetTy;
        returnCtx_ = saveRet;
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        bool saveStatic = inStaticMember_;
        bool saveCtor = inConstructor_;
        ReturnCtx saveRet = returnCtx_;
        // Slice S6: static constructor body has no `this`.
        inStaticMember_ = cd->isStatic;
        // Slice S7: readonly fields are assignable inside ctors.
        inConstructor_ = true;
        returnCtx_ = ReturnCtx::CtorDtor;
        runMethodBody(cd->params, &cd->body, nullptr,
                      paramSymbolsOf(m));
        returnCtx_ = saveRet;
        inStaticMember_ = saveStatic;
        inConstructor_ = saveCtor;
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        std::vector<ast::Param> none;
        ReturnCtx saveRet = returnCtx_;
        returnCtx_ = ReturnCtx::CtorDtor;
        runMethodBody(none, &dd->body, nullptr, nullptr);
        returnCtx_ = saveRet;
    } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(&m)) {
        std::vector<ast::Param> none;
        // §22.10: for an indexer property, find the PropertySymbol's
        // indexerParams so they're visible inside accessor bodies.
        const std::vector<Symbol*>* idxSymsPtr = nullptr;
        std::vector<Symbol*> idxSyms;
        if (pd->isIndexer && currentType_) {
            for (Symbol* mem : currentType_->members) {
                if (auto* ps = dynamic_cast<PropertySymbol*>(mem)) {
                    if (ps->isIndexer && ps->decl == pd) {
                        idxSyms = ps->indexerParams;
                        idxSymsPtr = &idxSyms;
                        break;
                    }
                }
            }
        }
        // Use pd->indexerParams as the param reference (move-only but
        // we only need its .name field in runMethodBody).
        const auto& pdParams = pd->isIndexer ? pd->indexerParams : none;
        if (pd->exprBody)
            runMethodBody(pdParams, nullptr, pd->exprBody.get(),
                          idxSymsPtr);
        if (pd->autoInitializer) {
            // §22.8 applies to auto-property initializers too.
            bool save = inFieldInitializer_;
            inFieldInitializer_ = true;
            visitExpr(*pd->autoInitializer);
            inFieldInitializer_ = false;
            inFieldInitializer_ = save;
        }
        for (const auto& a : pd->accessors) {
            // §22.9: a setter accessor has an implicit `value` parameter
            // (renamable via a SetterParamSpec) of the property's type.
            // Its decl anchors on the PropertyDecl so codegen can map it
            // to the assigned value. For indexers, include indexer params.
            if (a.isSet) {
                std::string_view vname = a.setterParamName.empty()
                    ? std::string_view("value")
                    : a.setterParamName;
                auto* vsym = ctx_.makeSymbol<LocalSymbol>();
                vsym->name = vname;
                vsym->decl = pd;
                if (pd->type) vsym->declType = ctx_.resolvedType(*pd->type);
                // Build extended symbol list: indexer params + value.
                std::vector<Symbol*> ssyms;
                for (auto* s : idxSyms) ssyms.push_back(s);
                ssyms.push_back(vsym);
                // Build synthetic param list for the combined set.
                // runMethodBody just reads param[i].name to populate scope.
                std::vector<ast::Param> sp;
                for (const auto& p : pd->indexerParams) {
                    ast::Param dummy; dummy.name = p.name;
                    sp.push_back(std::move(dummy));
                }
                ast::Param vp; vp.name = vname;
                sp.push_back(std::move(vp));
                if (a.exprBody)
                    runMethodBody(sp, nullptr, a.exprBody.get(), &ssyms);
                if (!a.stmts.empty())
                    runMethodBody(sp, &a.stmts, nullptr, &ssyms);
                continue;
            }
            // Getter: include indexer params.
            if (a.exprBody)
                runMethodBody(pdParams, nullptr, a.exprBody.get(),
                              idxSymsPtr);
            if (!a.stmts.empty())
                runMethodBody(pdParams, &a.stmts, nullptr, idxSymsPtr);
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstDecl*>(&m)) {
        if (cd->value) visitExpr(*cd->value);
    } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) {
            TypeSymbol* nested = nullptr;
            if (currentType_) {
                for (Symbol* mem : currentType_->members) {
                    if (auto* nts =
                            dynamic_cast<NestedTypeSymbol*>(mem)) {
                        if (nts->type &&
                            nts->type->decl == nt->decl.get()) {
                            nested = nts->type; break;
                        }
                    }
                }
            }
            TypeSymbol* save = currentType_;
            currentType_ = nested;
            for (const auto& innerM : nt->decl->members) {
                if (innerM) visitMember(*innerM);
            }
            currentType_ = save;
        }
    }
}

void ExprTC::visitStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        // Initializer evaluates BEFORE the local comes into scope.
        // §5 / issue #184: if the variable has an explicit wider integer
        // type, pass it as a hint so unsuffixed int literals can be
        // widened instead of rejected.
        PrimitiveType savedHint = currentLiteralWideningHint_;
        if (ld->type && ld->initializer) {
            if (auto* rt = ctx_.resolvedType(*ld->type)) {
                if (rt->kind == TypeRef::Kind::Primitive) {
                    using P = PrimitiveType;
                    switch (rt->primitive) {
                        case P::Long: case P::ULong:
                        case P::UInt:
                            currentLiteralWideningHint_ = rt->primitive;
                            break;
                        default: break;
                    }
                }
            }
        }
        if (ld->initializer) visitExpr(*ld->initializer);
        currentLiteralWideningHint_ = savedHint;
        if (currentScope_ && !ld->name.empty()) {
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = ld->name;
            lsym->decl = ld;
            lsym->isUsing = ld->isUsing;
            lsym->isAwaitUsing = ld->isAwaitUsing;
            // Slice 3g: type inference. Explicit `: Type` wins;
            // otherwise infer from the initializer's exprType.
            // Either may be null (plain `var x;` with no init).
            if (ld->type) {
                lsym->declType = ctx_.resolvedType(*ld->type);
                // Slice T1: when an explicit ascription and an
                // initializer are both present and both primitive,
                // check primitive assignability (§5.1).
                // §5.1 special: an integer literal assigns to any
                // integer type that can represent its value without
                // loss — re-type the literal as the target type so
                // the assignability check passes.
                if (ld->initializer) {
                    TypeRef* fromTy = ctx_.exprType(*ld->initializer);
                    if (fromTy && lsym->declType &&
                        fromTy->kind == TypeRef::Kind::Primitive &&
                        lsym->declType->kind == TypeRef::Kind::Primitive &&
                        fromTy->primitive == PrimitiveType::Int) {
                        PrimitiveType tgt = lsym->declType->primitive;
                        if (tgt == PrimitiveType::Byte ||
                            tgt == PrimitiveType::SByte ||
                            tgt == PrimitiveType::Short ||
                            tgt == PrimitiveType::UShort ||
                            tgt == PrimitiveType::UInt ||
                            tgt == PrimitiveType::ULong) {
                            // Retype the literal as the narrower type.
                            ctx_.setExprType(ld->initializer.get(),
                                             lsym->declType);
                            fromTy = lsym->declType;
                        }
                    }
                    checkAssignable(
                        fromTy,
                        lsym->declType, ld->initializer->span.begin,
                        std::string("initializer for `") +
                        std::string(ld->name) + "`");
                }
            } else if (ld->initializer) {
                lsym->declType = ctx_.exprType(*ld->initializer);
            }
            currentScope_->names[ld->name] = lsym;
        }
    } else if (auto* lc =
                   dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->value) visitExpr(*lc->value);
        // Slice S31 (§21.1 / §22.12): a local const's explicit type
        // must be a primitive value type or string.
        if (lc->type) {
            if (auto* rt = ctx_.resolvedType(*lc->type)) {
                bool ok = rt->kind == TypeRef::Kind::Primitive &&
                          rt->primitive != PrimitiveType::Object;
                if (!ok) {
                    ctx_.diagnostics().report(
                        Severity::Error, lc->span.begin,
                        std::string("const `") + std::string(lc->name) +
                        "` must have a primitive value type or "
                        "`string` (§22.12)");
                }
            }
        }
        if (currentScope_ && !lc->name.empty()) {
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = lc->name;
            lsym->decl = lc;
            lsym->isConst = true;
            currentScope_->names[lc->name] = lsym;
        }
    } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) visitExpr(*rs->value);
        checkReturnForm(*rs);
        // Slice T3: a returned value must be primitive-assignable to
        // the callable's return type.
        if (rs->value && returnCtx_ == ReturnCtx::ValueMethod) {
            checkAssignable(
                ctx_.exprType(*rs->value), currentReturnType_,
                rs->value->span.begin, "returned value");
        }
    } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) visitExpr(*es->expr);
    } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target) visitExpr(*as->target);
        // per §24.4: `event += handler` / `event -= handler` — type the
        // handler lambda's param as the event payload `T` (these forms desugar
        // to Add/Remove; they are not real compound assignments).
        TypeRef* eventPayload = nullptr;
        if ((as->op == "+=" || as->op == "-=") && as->target) {
            if (auto* es = dynamic_cast<EventSymbol*>(
                    ctx_.exprSymbol(*as->target))) {
                // per §24.3: the `+=` / `-=` forms desugar to Add / Remove,
                // which live on the EventSource<T> face — available only
                // inside the declaring class (and inside a derived class for a
                // `protected` event). Outside, the event is IEvent<T>, which
                // has no Add / Remove.
                checkEventVisibilityFence(es, as->op, as->span.begin);
                if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl))
                    if (ed->type) eventPayload = ctx_.resolvedType(*ed->type);
            }
        }
        if (as->value) {
            TypeRef* savedPending = pendingLambdaParamType_;
            if (eventPayload &&
                dynamic_cast<const ast::LambdaExpr*>(as->value.get()))
                pendingLambdaParamType_ = eventPayload;
            visitExpr(*as->value);
            pendingLambdaParamType_ = savedPending;
        }
        // per §24.4: an event += / -= is not a real compound assignment, so
        // the readonly / assignability checks below do not apply to it.
        if (!eventPayload) {
            // Slice S7: readonly field assignment outside a ctor.
            checkReadonlyAssignment(*as);
            // Slice T2: plain `:=` assignment must be primitive-
            // assignable (compound ops like `+=` have their own
            // operator semantics and are left to a later slice).
            if (as->op == ":=" && as->target && as->value) {
                checkAssignable(
                    ctx_.exprType(*as->value),
                    ctx_.exprType(*as->target),
                    as->value->span.begin, "assigned value");
            }
        }
    } else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) visitExpr(*ds->value);
    } else if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (ts->value) {
            visitExpr(*ts->value);
        } else if (catchDepth_ == 0) {
            // Slice S27 (§21.8 / §31.5): bare `throw;` re-raise is
            // valid only inside a catch arm body.
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "bare `throw;` (re-raise) is only valid inside a "
                "catch arm body (§31.5)");
        }
    } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition) {
            visitExpr(*ifs->ifBranch.condition);
            checkConditionIsBool(*ifs->ifBranch.condition, "if");
        }
        for (const auto& bs : ifs->ifBranch.body) {
            if (bs) visitStmt(*bs);
        }
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) {
                visitExpr(*el.condition);
                checkConditionIsBool(*el.condition, "elif");
            }
            for (const auto& bs : el.body) if (bs) visitStmt(*bs);
        }
        for (const auto& bs : ifs->elseBody) if (bs) visitStmt(*bs);
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) {
            visitExpr(*ws->condition);
            checkConditionIsBool(*ws->condition, "while");
        }
        ++loopDepth_;
        for (const auto& bs : ws->body) if (bs) visitStmt(*bs);
        --loopDepth_;
    } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable) visitExpr(*fs->iterable);
        if (fs->withExpr) visitExpr(*fs->withExpr);
        // §16.5: symbolize the loop variable so the body can refer to it.
        // The variable's type is the explicit annotation if given; for a
        // range literal iterable (`1..3`) it is the element type inferred
        // from the lo/hi expression type. `lsym->decl = fs` anchors the
        // symbol on the ForStmt — codegen maps `locals_[fs]` to the
        // counter alloca so identifier `i` resolves to the slot.
        Scope forScope;
        forScope.parent = currentScope_;
        if (currentScope_ && !fs->varName.empty()) {
            auto* lsym = ctx_.makeSymbol<LocalSymbol>();
            lsym->name = fs->varName;
            lsym->decl = fs;
            if (fs->varType) {
                lsym->declType = ctx_.resolvedType(*fs->varType);
            } else if (auto* range =
                           dynamic_cast<const ast::RangeExpr*>(
                               fs->iterable.get())) {
                if (range->lo)
                    lsym->declType = ctx_.exprType(*range->lo);
                else if (range->hi)
                    lsym->declType = ctx_.exprType(*range->hi);
            } else if (fs->iterable) {
                // §15.1: `for var x in a` over an inline array — the
                if (auto* tt = ctx_.exprType(*fs->iterable)) {
                    // §15.1: `for var x in a` over an inline array.
                    if (tt->kind == TypeRef::Kind::InlineArray &&
                        tt->arrayElement) {
                        lsym->declType = tt->arrayElement;
                    // §4.6: `for var c in s` over a string → char.
                    } else if (tt->kind == TypeRef::Kind::Primitive &&
                               tt->primitive == PrimitiveType::String) {
                        lsym->declType =
                            ctx_.primitiveTypeRef(PrimitiveType::Char);
                    // §3 / §29.2: runtime Array<T> or other IEnumerable<T>
                    // with a Named TypeRef — element type from first generic arg.
                    } else if (tt->kind == TypeRef::Kind::Named &&
                               !tt->genericArgs.empty() &&
                               tt->genericArgs[0]) {
                        // Covers Array<T>, Stack<T>, Queue<T>, user-defined
                        // IEnumerable<T> types that expose their T directly.
                        lsym->declType = tt->genericArgs[0];
                    } else if (tt->kind == TypeRef::Kind::Named &&
                               tt->declaration) {
                        // §29.2: user-defined IEnumerable<T> — find GetEnumerator
                        // and read its return type's first generic arg (T).
                        auto* ts = tt->declaration;
                        // Walk ts and its base types (simple BFS).
                        std::vector<const TypeSymbol*> queue = {ts};
                        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
                            const TypeSymbol* t = queue[qi];
                            for (auto* m : t->members) {
                                auto* ms = dynamic_cast<MethodSymbol*>(m);
                                if (ms && ms->name == "GetEnumerator" && ms->decl) {
                                    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
                                    if (md && md->returnType) {
                                        auto* rt = ctx_.resolvedType(*md->returnType);
                                        if (rt && !rt->genericArgs.empty() && rt->genericArgs[0])
                                            lsym->declType = rt->genericArgs[0];
                                    }
                                    break;
                                }
                            }
                            if (lsym->declType) break;
                            for (auto* bt : t->baseTypes) if (bt) queue.push_back(bt);
                        }
                    }
                } else if (auto* cl =
                               dynamic_cast<const ast::CollectionLiteralExpr*>(
                                   fs->iterable.get())) {
                    // §16: `for var x in [e1, e2, ...]` — infer element
                    // type from first element.
                    if (!cl->isDict && !cl->elements.empty() &&
                        cl->elements[0]) {
                        lsym->declType = ctx_.exprType(*cl->elements[0]);
                    }
                }
            }
            forScope.names[fs->varName] = lsym;
        }
        Scope* saveScope = currentScope_;
        currentScope_ = &forScope;
        ++loopDepth_;
        for (const auto& bs : fs->body) if (bs) visitStmt(*bs);
        --loopDepth_;
        currentScope_ = saveScope;
    } else if (dynamic_cast<const ast::BreakStmt*>(&s)) {
        // Slice S26 (§21.5): `break` is legal only inside a loop.
        if (loopDepth_ == 0) {
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`break` is not allowed outside a loop (§21.5)");
        }
    } else if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
        if (loopDepth_ == 0) {
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "`continue` is not allowed outside a loop (§21.5)");
        }
    } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body) if (bs) visitStmt(*bs);
    } else if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : trys->body) if (bs) visitStmt(*bs);
        for (const auto& arm : trys->catches) {
            // §31.2: a `catch var e: T` arm binds the caught exception
            // to `e` (a borrow of the caught type) in the guard and
            // body scope. The binding's decl anchors on the arm body so
            // codegen can map it to the exception value.
            Scope armScope;
            armScope.parent = currentScope_;
            if (arm.hasBinding && !arm.varName.empty() &&
                !arm.types.empty() && arm.types[0] && arm.body) {
                auto* bnd = ctx_.makeSymbol<LocalSymbol>();
                bnd->name = arm.varName;
                bnd->decl = arm.body.get();
                bnd->declType = ctx_.resolvedType(*arm.types[0]);
                armScope.names[arm.varName] = bnd;
            }
            Scope* saveScope = currentScope_;
            currentScope_ = &armScope;
            if (arm.guard) visitExpr(*arm.guard);
            // Slice S27: a bare `throw;` is valid inside a catch
            // arm body (but not in the try body or finally).
            ++catchDepth_;
            if (arm.body) visitStmt(*arm.body);
            --catchDepth_;
            currentScope_ = saveScope;
        }
        for (const auto& bs : trys->finallyBody) if (bs) visitStmt(*bs);
    } else if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) visitExpr(*ys->value);
    } else if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) visitExpr(*is->target);
    } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) visitExpr(*cs->scrutinee);
        TypeRef* scrutTy =
            cs->scrutinee ? ctx_.exprType(*cs->scrutinee) : nullptr;
        for (const auto& arm : cs->arms) {
            // §19: a BindingPattern arm introduces a new scope with the
            // bound name visible inside the guard and body (per §19.2).
            auto* bp =
                dynamic_cast<const ast::BindingPattern*>(arm.pattern.get());
            auto* dp =
                dynamic_cast<const ast::DestructuringPattern*>(arm.pattern.get());
            if (bp && currentScope_) {
                Scope armScope;
                armScope.parent = currentScope_;
                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                lsym->name = bp->name;
                lsym->decl = bp;
                // Explicit type annotation wins; otherwise inherit scrutinee
                // type (the binding is a borrow of the matched value).
                lsym->declType = bp->type ? ctx_.resolvedType(*bp->type)
                                          : scrutTy;
                armScope.names[bp->name] = lsym;
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
                currentScope_ = saveScope;
            } else if (dp && currentScope_ && dp->typeName) {
                // §19: a DestructuringPattern arm introduces a scope with
                // one LocalSymbol per naked binding, typed as the
                // corresponding positional field of the record (§9.4).
                // Look up the record TypeSymbol by the pattern's type name.
                TypeSymbol* recTy = nullptr;
                std::string_view lastName = dp->typeName->parts.empty()
                    ? std::string_view{}
                    : dp->typeName->parts.back();
                if (ctx_.module()) {
                    for (Symbol* child : ctx_.module()->children) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                            if (ts->name == lastName &&
                                ts->kind == ast::TypeDeclKind::Record) {
                                recTy = ts; break;
                            }
                        }
                    }
                }
                // Collect the record's positional (non-static) fields.
                std::vector<FieldSymbol*> fields;
                if (recTy) {
                    for (Symbol* m : recTy->members) {
                        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                            if (!fs->modifiers.empty()) {
                                // skip static fields
                                bool isStatic = false;
                                for (auto& mod : fs->modifiers)
                                    if (mod == "static") { isStatic = true; break; }
                                if (isStatic) continue;
                            }
                            fields.push_back(fs);
                        }
                    }
                }
                Scope armScope;
                armScope.parent = currentScope_;
                for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
                    const auto& sp = dp->subPatterns[i];
                    if (sp.nakedBinding.empty()) continue;
                    TypeRef* fieldTy = nullptr;
                    if (i < fields.size() && fields[i]->posParam &&
                        fields[i]->posParam->type) {
                        fieldTy = ctx_.resolvedType(*fields[i]->posParam->type);
                    }
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = sp.nakedBinding;
                    // Use the PositionalPattern's address (stable since
                    // AST is immutable) as the unique decl anchor so
                    // codegen can key locals_[] on the same pointer.
                    lsym->decl = reinterpret_cast<const ast::Node*>(&sp);
                    lsym->declType = fieldTy;
                    armScope.names[sp.nakedBinding] = lsym;
                }
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
                currentScope_ = saveScope;
            } else {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
            }
        }
    }
    // Break, Continue have no expressions.
}

bool typecheckExpressions(const ast::SourceFile& src, SemaContext& ctx) {
    ExprTC tc(ctx);
    tc.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
