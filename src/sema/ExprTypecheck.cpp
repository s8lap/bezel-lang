#include "sema/ExprTypecheck.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

namespace {

// Per-walker scope for identifier lookups inside method bodies (slice 3b).
struct Scope {
    std::unordered_map<std::string_view, Symbol*> names;
    Scope* parent = nullptr;
};

class ExprTC {
public:
    ExprTC(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src) {
        moduleScope_ = std::make_unique<Scope>();
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (!child->name.empty()) {
                    moduleScope_->names[child->name] = child;
                }
            }
        }
        for (const auto& decl : src.topDecls) {
            if (!decl) continue;
            visitTopDecl(*decl);
        }
    }

private:
    SemaContext& ctx_;
    std::unique_ptr<Scope> moduleScope_;
    Scope* currentScope_ = nullptr;
    TypeSymbol* currentType_ = nullptr;
    // Slice S6: in-progress method static-ness, so visitExpr's
    // IdentifierExpr handling can diagnose `this`/`base` in static
    // member bodies.
    bool inStaticMember_ = false;
    // Slice S7: tracks whether we're inside a constructor (or static
    // constructor) body — the only places where `this.<readonly>` may
    // be assigned. Field initializers are handled separately (slice
    // S7 doesn't flag those — they're part of the initialization flow).
    bool inConstructor_ = false;
    // Slice S21: the return-form context of the callable currently
    // being walked, for the §21.7 form rules.
    enum class ReturnCtx { None, ValueMethod, VoidMethod, CtorDtor };
    ReturnCtx returnCtx_ = ReturnCtx::None;
    // Slice T3: the resolved return type of the value-returning
    // callable currently being walked (null otherwise), for primitive
    // return-value assignability (§5.1).
    TypeRef* currentReturnType_ = nullptr;
    // Slice S26: enclosing-loop nesting depth, for the §21.5 rule that
    // `break`/`continue` are legal only inside a loop.
    int loopDepth_ = 0;
    // Slice S27: enclosing catch-arm-body depth, for the §31.5 rule
    // that a bare `throw;` / `throw` re-raise is valid only in a catch
    // arm body.
    int catchDepth_ = 0;

    // Slice S21: does a callable with this return-type ascription
    // produce a value? No ascription → no (unit). An ascription that
    // resolves to the unit type → no. Anything else → yes.
    bool returnsValue(const ast::Type* rt) {
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
    void checkReturnForm(const ast::ReturnStmt& rs) {
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
    static bool primitiveWidens(PrimitiveType from, PrimitiveType to) {
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
    static bool isSubtype(TypeSymbol* from, TypeSymbol* to) {
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
    void checkAssignable(TypeRef* from, TypeRef* to,
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
    void checkConditionIsBool(const ast::Expr& cond, std::string_view ctx) {
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
    void checkReadonlyAssignment(const ast::AssignmentStmt& as) {
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
    Symbol* lookupMember(TypeSymbol* type, std::string_view name) {
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

    Symbol* lookupName(std::string_view name) {
        for (Scope* s = currentScope_; s; s = s->parent) {
            auto it = s->names.find(name);
            if (it != s->names.end()) return it->second;
        }
        if (currentType_) {
            for (Symbol* m : currentType_->members) {
                if (m && m->name == name) return m;
            }
        }
        return nullptr;
    }

    void visitTopDecl(const ast::TopDecl& d) {
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
    const std::vector<Symbol*>* paramSymbolsOf(const ast::Member& m) const {
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
    void runMethodBody(
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

    void visitMember(const ast::Member& m) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
            if (fd->initializer) visitExpr(*fd->initializer);
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
            if (pd->exprBody)
                runMethodBody(none, nullptr, pd->exprBody.get(), nullptr);
            if (pd->autoInitializer)
                visitExpr(*pd->autoInitializer);
            for (const auto& a : pd->accessors) {
                if (a.exprBody)
                    runMethodBody(none, nullptr, a.exprBody.get(), nullptr);
                if (!a.stmts.empty())
                    runMethodBody(none, &a.stmts, nullptr, nullptr);
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

    void visitStmt(const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            // Initializer evaluates BEFORE the local comes into scope.
            if (ld->initializer) visitExpr(*ld->initializer);
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
                    if (ld->initializer) {
                        checkAssignable(
                            ctx_.exprType(*ld->initializer),
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
            if (as->value) visitExpr(*as->value);
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
            ++loopDepth_;
            for (const auto& bs : fs->body) if (bs) visitStmt(*bs);
            --loopDepth_;
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
                if (arm.guard) visitExpr(*arm.guard);
                // Slice S27: a bare `throw;` is valid inside a catch
                // arm body (but not in the try body or finally).
                ++catchDepth_;
                if (arm.body) visitStmt(*arm.body);
                --catchDepth_;
            }
            for (const auto& bs : trys->finallyBody) if (bs) visitStmt(*bs);
        } else if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
            if (ys->value) visitExpr(*ys->value);
        } else if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
            if (is->target) visitExpr(*is->target);
        } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
            if (cs->scrutinee) visitExpr(*cs->scrutinee);
            for (const auto& arm : cs->arms) {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
            }
        }
        // Break, Continue have no expressions.
    }

    void visitExpr(const ast::Expr& e) {
        // Slice 3b: IdentifierExpr name lookup. `this` / `base` are
        // implicit-meaning identifiers — leave them unresolved
        // (typecheck handles their semantics in a future slice).
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            if (id->name == "this" || id->name == "base") {
                if (inStaticMember_) {
                    ctx_.diagnostics().report(
                        Severity::Error, id->span.begin,
                        std::string("`") + std::string(id->name) +
                        "` is not available in a static method or "
                        "static constructor — it requires an instance "
                        "receiver (§12.3)");
                }
                return;
            }
            Symbol* sym = lookupName(id->name);
            if (sym) {
                ctx_.setExprSymbol(&e, sym);
                // Slice 3d: also attach the symbol's resolved type to
                // the identifier expr so MemberAccessExpr can pick it
                // up when this identifier is the target.
                if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
                    // Slice 3g: prefer the LocalSymbol's resolved type
                    // (which already accounts for inference) over re-
                    // querying the LocalDecl's ascription.
                    if (loc->declType) {
                        ctx_.setExprType(&e, loc->declType);
                    }
                } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
                    // Slice 3e: ParameterSymbol::declType points at the
                    // parameter's Type ast (set by symbol collection).
                    // Look up its resolved TypeRef and attach.
                    if (par->declType) {
                        if (auto* rt = ctx_.resolvedType(*par->declType)) {
                            ctx_.setExprType(&e, rt);
                        }
                    }
                } else if (auto* fs = dynamic_cast<FieldSymbol*>(sym)) {
                    // Implicit `this.<field>` form: bare identifier
                    // resolving to a class field. Same field-type rule
                    // as MemberAccessExpr below.
                    if (auto* fd =
                            dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                        if (fd->type) {
                            if (auto* rt = ctx_.resolvedType(*fd->type)) {
                                ctx_.setExprType(&e, rt);
                            }
                        }
                    }
                }
            } else if (currentScope_) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("unresolved identifier `") +
                    std::string(id->name) + "`");
            }
            return;
        }
        // Slice 3 main contract: every literal gets a type.
        if (dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Int));
            return;
        }
        if (dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
            ctx_.setExprType(
                &e, ctx_.primitiveTypeRef(PrimitiveType::Double));
            return;
        }
        if (dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Char));
            return;
        }
        if (dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
            dynamic_cast<const ast::VerbatimStringExpr*>(&e) ||
            dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
            ctx_.setExprType(
                &e, ctx_.primitiveTypeRef(PrimitiveType::String));
            return;
        }
        if (dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
            // null is context-typed; absent the context we annotate with
            // nullable Object. Later passes refine when there's a target
            // type to constrain against.
            ctx_.setExprType(
                &e, ctx_.primitiveTypeRef(PrimitiveType::Object,
                                          /*nullable=*/true));
            return;
        }
        if (dynamic_cast<const ast::UnitExpr*>(&e)) {
            // Build a unit TypeRef on first use (no primitive entry).
            auto* unit = ctx_.makeTypeRef();
            unit->kind = TypeRef::Kind::Unit;
            ctx_.setExprType(&e, unit);
            return;
        }
        // For non-literal expressions, recurse into children so their
        // own literals get typed. The expression itself may also get a
        // type from the operator's result rules (slice 3c).
        visitChildren(e);
        // Slice 3c: binary / unary operator result typing.
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            typeBinary(*be);
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            typeUnary(*ue);
        }
        // Slice 3d: MemberAccessExpr resolution. Runs after the target
        // is visited (so its TypeRef is available), looks the member up
        // on the target's TypeSymbol, and annotates the access with the
        // resolved member symbol and field/property type.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            resolveMemberAccess(*ma);
        }
        // Slice 3f: NewExpr is typed as its constructed type. `default`
        // and `typeof` follow the same pattern (default(T) → T,
        // typeof(T) → Type per §17.1).
        if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
            if (ne->type) {
                if (auto* rt = ctx_.resolvedType(*ne->type)) {
                    ctx_.setExprType(&e, rt);
                    // Slice S11: forbid `new` on abstract or static
                    // class types. Interfaces and (purely declarative)
                    // static-only types are also non-instantiable.
                    if (rt->kind == TypeRef::Kind::Named && rt->declaration) {
                        TypeSymbol* t = rt->declaration;
                        bool isAbstract = false;
                        bool isStatic = false;
                        for (auto mod : t->modifiers) {
                            if (mod == "abstract") isAbstract = true;
                            if (mod == "static") isStatic = true;
                        }
                        if (isAbstract) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne->span.begin,
                                std::string("cannot instantiate abstract "
                                            "class `") +
                                std::string(t->name) + "` with `new` "
                                "(§12.5)");
                        } else if (isStatic) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne->span.begin,
                                std::string("cannot instantiate static "
                                            "class `") +
                                std::string(t->name) + "` with `new` "
                                "(§11.7)");
                        } else if (t->kind ==
                                   ast::TypeDeclKind::Interface) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne->span.begin,
                                std::string("cannot instantiate interface `") +
                                std::string(t->name) +
                                "` with `new` (§11.2)");
                        }
                    }
                }
            }
        }
        if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
            if (de->type) {
                if (auto* rt = ctx_.resolvedType(*de->type)) {
                    ctx_.setExprType(&e, rt);
                }
            }
        }
        // Slice 3h: CallExpr return-type typing. When the callee
        // resolves to a MethodSymbol (via slice 3d's MemberAccessExpr
        // resolver or slice 3b's identifier resolver), the call's
        // exprType is the method's resolved return type. Overload
        // picking is not yet implemented — the first matching member
        // wins, which can give the wrong result when overloads have
        // distinct return types. Tightened with the overload-resolution
        // slice.
        if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
            typeCall(*ce);
        }
    }

    // Slice 3h: resolve a CallExpr's return type from the callee's
    // resolved MethodSymbol.
    void typeCall(const ast::CallExpr& ce) {
        if (!ce.callee) return;
        Symbol* calleeSym = ctx_.exprSymbol(*ce.callee);
        if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym)) {
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            if (md && md->returnType) {
                if (auto* rt = ctx_.resolvedType(*md->returnType)) {
                    ctx_.setExprType(&ce, rt);
                }
            } else if (md && !md->returnType) {
                // No return type ascription → unit.
                auto* unit = ctx_.makeTypeRef();
                unit->kind = TypeRef::Kind::Unit;
                ctx_.setExprType(&ce, unit);
            }
        }
        // CallExpr where the callee is a constructor reference (e.g.
        // `T(args)` form — rare in Bezel since `new T(args)` is the
        // canonical form), or the callee is a type symbol, are not yet
        // handled. Likewise calls on lambda values await the
        // function-type slice.

        // Slice T5: argument-count check (§22.5). Only when the callee
        // resolves to a method that is the SOLE overload of its name in
        // its type, and the call uses all-positional arguments. Named
        // arguments and overload sets are skipped (overload resolution
        // is a later slice). Optional parameters (those with a default)
        // set the minimum; the total is the maximum (no variadics in
        // Bezel — §22.5).
        if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym)) {
            checkCallArity(ce, ms);
        }
    }

    void checkCallArity(const ast::CallExpr& ce, MethodSymbol* ms) {
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        if (!md || !ms->parent) return;
        // Bail if any argument is named (named-arg matching is deferred).
        for (const auto& a : ce.args) {
            if (!a.name.empty()) return;
        }
        // Bail if the name has more than one method overload in the type.
        int sameName = 0;
        for (Symbol* m : ms->parent->members) {
            if (dynamic_cast<MethodSymbol*>(m) && m->name == ms->name) {
                ++sameName;
            }
        }
        if (sameName != 1) return;
        std::size_t total = md->params.size();
        std::size_t required = 0;
        for (const auto& p : md->params) {
            if (!p.defaultValue) ++required;
        }
        std::size_t got = ce.args.size();
        if (got < required || got > total) {
            ctx_.diagnostics().report(
                Severity::Error, ce.span.begin,
                std::string("call to `") + std::string(ms->name) +
                "` has " + std::to_string(got) +
                " argument(s) but the method takes " +
                (required == total
                     ? std::to_string(total)
                     : std::to_string(required) + "–" +
                           std::to_string(total)) +
                " (§22.5)");
        }
    }

    // §17.1: postfix `.` member access. The target's type determines
    // the lookup scope. For now we handle `this.member` (lookup in the
    // current type) and `<expr>.member` where the expression has a
    // resolved Named or Primitive-Object type. Generics and inherited
    // members from base types are deferred to a later slice.
    void resolveMemberAccess(const ast::MemberAccessExpr& ma) {
        TypeSymbol* targetType = nullptr;
        // `this.X` — currentType_ takes precedence over name lookup so
        // we don't have to teach `this` to be an expression with type
        // currentType_.
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                ma.target.get())) {
            if (id->name == "this" || id->name == "base") {
                targetType = currentType_;
            }
        }
        if (!targetType && ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            if (tt && tt->kind == TypeRef::Kind::Named) {
                targetType = tt->declaration;
            } else if (!tt) {
                // Target had no resolvable type — fall back to the
                // exprSymbol path (e.g. local declared with `var d: D`
                // when locals don't carry exprType yet but DO carry
                // exprSymbol whose decl gives the resolved type).
                auto* sym = ctx_.exprSymbol(*ma.target);
                if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
                    if (loc->declType &&
                        loc->declType->kind == TypeRef::Kind::Named) {
                        targetType = loc->declType->declaration;
                    }
                } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
                    if (par->declType) {
                        if (auto* rt = ctx_.resolvedType(*par->declType)) {
                            if (rt->kind == TypeRef::Kind::Named) {
                                targetType = rt->declaration;
                            }
                        }
                    }
                }
            }
        }
        if (!targetType) return;
        // Lookup member by name. Slice S5: walks own members first,
        // then transitively each base type — first match wins (no
        // ambiguity resolution between base interfaces yet).
        Symbol* member = lookupMember(targetType, ma.member);
        if (!member) {
            // Slice 3d intentionally does NOT diagnose unknown members
            // — base-type/inherited-member lookup and extension-method
            // dispatch are later slices. A user-typed `obj.Dispose()`
            // against `Stream` would otherwise mis-flag a method that
            // really lives on the IDisposable interface. The lookup
            // failure is non-fatal: the expression simply lacks an
            // exprSymbol/exprType, and later passes can still proceed.
            return;
        }
        // Slice S8: visibility check. A `private` member is accessible
        // only from inside the declaring type. Walk the member's
        // parent TypeSymbol; if it isn't currentType_, diagnose.
        // Other visibilities (internal/protected/public) are deferred.
        if (auto* msym = dynamic_cast<MemberSymbol*>(member)) {
            if (msym->visibility == ast::Visibility::Private &&
                msym->parent && msym->parent != currentType_) {
                ctx_.diagnostics().report(
                    Severity::Error, ma.span.begin,
                    std::string("`") + std::string(ma.member) +
                    "` is private to type `" +
                    std::string(msym->parent->name) +
                    "` and not accessible from `" +
                    std::string(currentType_ ? currentType_->name
                                             : std::string_view("(global)")) +
                    "` (§13.1 — `private` is visible to the declaring "
                    "class only)");
            }
        }
        ctx_.setExprSymbol(&ma, member);
        // Attach the member's type for field / property access. Methods
        // and constructors are deferred to the call-resolution slice.
        if (auto* fs = dynamic_cast<FieldSymbol*>(member)) {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (fd && fd->type) {
                if (auto* rt = ctx_.resolvedType(*fd->type)) {
                    ctx_.setExprType(&ma, rt);
                }
            }
        } else if (auto* ps = dynamic_cast<PropertySymbol*>(member)) {
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (pd && pd->type) {
                if (auto* rt = ctx_.resolvedType(*pd->type)) {
                    ctx_.setExprType(&ma, rt);
                }
            }
        }
    }

    // §17.1 result-type table for binary operators. Slice 3c covers the
    // common cases with no operator-overload resolution: equality and
    // relational ops produce bool; arithmetic / shift / bitwise produce
    // the left operand's type (when the operands type-match); logical
    // ops `and` / `or` produce bool; `??` produces the non-nullable
    // result type of the LHS; range ops produce a Range value (slice 3c
    // leaves Range as Error).
    void typeBinary(const ast::BinaryExpr& be) {
        const auto& op = be.op;
        if (op == "=" || op == "<>" || op == "<" || op == ">" ||
            op == "<=" || op == ">=" || op == "and" || op == "or") {
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        // `in` is a membership test → bool.
        if (op == "in") {
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        // Arithmetic / shift / bitwise: result type follows the left
        // operand's type when both sides are present and known.
        if (op == "+" || op == "-" || op == "*" || op == "/" ||
            op == "mod" || op == "&" || op == "|" || op == "^" ||
            op == "<<" || op == ">>") {
            TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
            if (lt) ctx_.setExprType(&be, lt);
            return;
        }
        // `??` null-coalescing: result type is the non-nullable form of
        // the LHS (sema-3c approximation: take RHS type if known, else
        // LHS).
        if (op == "??") {
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            if (rt) ctx_.setExprType(&be, rt);
            return;
        }
        // Range ops, etc.: leave un-typed (later slices handle).
    }

    // §17.1 result-type table for unary operators (slice 3c).
    void typeUnary(const ast::UnaryExpr& ue) {
        const auto& op = ue.op;
        if (op == "not") {
            ctx_.setExprType(&ue, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        if (op == "+" || op == "-" || op == "~") {
            TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
            if (ot) ctx_.setExprType(&ue, ot);
            return;
        }
        // `await`, `move` — result-type rules deeper (Task<T> -> T,
        // move keeps the operand's type). Slice 3c doesn't type them.
    }

    void visitChildren(const ast::Expr& e) {
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (be->left) visitExpr(*be->left);
            if (be->right) visitExpr(*be->right);
        } else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (ue->operand) visitExpr(*ue->operand);
        } else if (auto* ma =
                       dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) visitExpr(*ma->target);
        } else if (auto* nm =
                       dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
            if (nm->target) visitExpr(*nm->target);
        } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
            if (ce->callee) visitExpr(*ce->callee);
            for (const auto& a : ce->args) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
            if (nc->callee) visitExpr(*nc->callee);
            for (const auto& a : nc->args) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (ix->target) visitExpr(*ix->target);
            for (const auto& a : ix->indices) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
            if (bg->operand) visitExpr(*bg->operand);
        } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
            for (const auto& a : ne->args) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            if (ce->condition) {
                visitExpr(*ce->condition);
                // §20: the `if` expression's condition must be bool.
                checkConditionIsBool(*ce->condition, "if-expression");
            }
            if (ce->thenValue) visitExpr(*ce->thenValue);
            if (ce->elseValue) visitExpr(*ce->elseValue);
        } else if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            if (te->operand) visitExpr(*te->operand);
        } else if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
            if (th->operand) {
                visitExpr(*th->operand);
            } else if (catchDepth_ == 0) {
                // Slice S27: bare `throw` expression re-raise is valid
                // only inside a catch arm body (§31.5 / §17.12).
                ctx_.diagnostics().report(
                    Severity::Error, e.span.begin,
                    "bare `throw` (re-raise) is only valid inside a "
                    "catch arm body (§31.5)");
            }
        } else if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
            // §21.7: a `return` inside a lambda exits the lambda and
            // follows the lambda's own return type — which slice S21
            // does not yet infer. Suppress the enclosing callable's
            // return-form context while walking the lambda body.
            // §21.5: a loop in the enclosing scope does not make
            // `break`/`continue` legal inside the lambda, so reset the
            // loop depth across the lambda boundary too.
            ReturnCtx saveRet = returnCtx_;
            TypeRef* saveRetTy = currentReturnType_;
            int saveLoop = loopDepth_;
            int saveCatch = catchDepth_;
            returnCtx_ = ReturnCtx::None;
            currentReturnType_ = nullptr;
            loopDepth_ = 0;
            catchDepth_ = 0;
            if (lam->exprBody) visitExpr(*lam->exprBody);
            for (const auto& s : lam->stmts) if (s) visitStmt(*s);
            returnCtx_ = saveRet;
            currentReturnType_ = saveRetTy;
            loopDepth_ = saveLoop;
            catchDepth_ = saveCatch;
        } else if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
            if (ce->scrutinee) visitExpr(*ce->scrutinee);
            for (const auto& arm : ce->arms) {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
            }
        } else if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
            if (we->source) visitExpr(*we->source);
            for (const auto& ia : we->assigns) {
                if (ia.value) visitExpr(*ia.value);
            }
        } else if (auto* cl =
                       dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
            for (const auto& k : cl->keys) if (k) visitExpr(*k);
            for (const auto& v : cl->elements) if (v) visitExpr(*v);
        }
        // Identifier / This / Base / TypeOf / NameOf / Default —
        // no expression children to recurse into.
    }
};

} // namespace

bool typecheckExpressions(const ast::SourceFile& src, SemaContext& ctx) {
    ExprTC tc(ctx);
    tc.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
