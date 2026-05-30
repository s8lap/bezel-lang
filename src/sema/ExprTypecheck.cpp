#include "sema/ExprTypecheck.hpp"

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

namespace {

// §5: parse an integer literal's magnitude (ignoring base prefix,
// underscores, and any type suffix). Returns true and the value via `out`
// when it parses; saturates large hex/decimal in the rare overflow case
// (which still exceeds int and is reported). Mirrors the codegen parser.
bool intLiteralMagnitude(std::string_view text, unsigned long long& out) {
    int base = 10;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char b = text[1];
        if (b == 'x' || b == 'X') { base = 16; i = 2; }
        else if (b == 'b' || b == 'B') { base = 2; i = 2; }
        else if (b == 'o' || b == 'O') { base = 8; i = 2; }
    }
    std::string digits;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        bool isDigit = (c >= '0' && c <= '9');
        bool isHex =
            isDigit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (base == 16 ? !isHex : !isDigit) break;  // stop at suffix letters
        digits.push_back(c);
    }
    if (digits.empty()) return false;
    errno = 0;
    out = std::strtoull(digits.c_str(), nullptr, base);
    return true;
}

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
    // Slice S29: true while walking a field/auto-property initializer,
    // for the §22.8 rule that initializers may not observe instance
    // state (`this`, `base`, instance members).
    bool inFieldInitializer_ = false;

    // Slice S29: report when a bare identifier inside a field
    // initializer resolves to a non-static member of the current type
    // (an implicit-`this` reference, forbidden by §22.8). Static
    // members, class constants, enum constants, locals, parameters,
    // types, and free names are all permitted.
    void checkNotInstanceMember(const ast::IdentifierExpr& id, Symbol* sym) {
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
    bool bodyAlwaysExits(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        for (const auto& s : body) {
            if (s && stmtAlwaysExits(*s)) return true;
        }
        return false;
    }

    bool stmtAlwaysExits(const ast::Stmt& s) {
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

    bool armBodyAlwaysExits(const ast::CaseArm& arm) {
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
        // An unqualified member name resolves as an implicit-`this`
        // reference — search the enclosing type AND its base types
        // (§12 inheritance), mirroring lookupMember's transitive walk so
        // an inherited field/method is reachable without `this.`/`base.`.
        if (currentType_) {
            if (Symbol* m = lookupMember(currentType_, name)) return m;
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
            if (stmts && returnCtx_ == ReturnCtx::ValueMethod &&
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
            if (pd->exprBody)
                runMethodBody(none, nullptr, pd->exprBody.get(), nullptr);
            if (pd->autoInitializer) {
                // §22.8 applies to auto-property initializers too.
                bool save = inFieldInitializer_;
                inFieldInitializer_ = true;
                visitExpr(*pd->autoInitializer);
                inFieldInitializer_ = save;
            }
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
                // Slice S29 (§22.8): a field initializer may not
                // reference `this` / `base`.
                if (inFieldInitializer_) {
                    ctx_.diagnostics().report(
                        Severity::Error, id->span.begin,
                        std::string("a field initializer may not "
                                    "reference `") +
                        std::string(id->name) + "` (§22.8)");
                }
                return;
            }
            Symbol* sym = lookupName(id->name);
            if (sym) {
                ctx_.setExprSymbol(&e, sym);
                // Slice S29 (§22.8): a field initializer may not observe
                // an instance member (implicit-`this` reference).
                if (inFieldInitializer_) checkNotInstanceMember(*id, sym);
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
            } else if (primitiveFromName(id->name) != PrimitiveType::None) {
                // §5.2: a primitive type name used in expression position is
                // a conversion callee (`int(x)`). It has no value symbol;
                // the enclosing call resolves the conversion (typeCall).
            } else if (currentScope_) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("unresolved identifier `") +
                    std::string(id->name) + "`");
            }
            return;
        }
        // Slice 3 main contract: every literal gets a type.
        if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
            // §5: an integer literal's type comes from its suffix when
            // present (`L` → long, `u` → uint, `uL`/`Lu` → ulong), else
            // int. Context (target) typing of unsuffixed literals is a
            // separate, broader change (tracked in the literal-typing
            // issue); this covers the explicit-suffix forms.
            bool hasL = false, hasU = false;
            for (char c : il->text) {
                if (c == 'l' || c == 'L') hasL = true;
                if (c == 'u' || c == 'U') hasU = true;
            }
            PrimitiveType pt = PrimitiveType::Int;
            if (hasL && hasU) pt = PrimitiveType::ULong;
            else if (hasL) pt = PrimitiveType::Long;
            else if (hasU) pt = PrimitiveType::UInt;
            // §5: an unsuffixed integer literal must fit in `int`; if it does
            // not, the literal requires a suffix (it is not silently widened).
            if (pt == PrimitiveType::Int) {
                unsigned long long mag = 0;
                if (intLiteralMagnitude(il->text, mag) &&
                    (errno == ERANGE || mag > 2147483647ull)) {
                    ctx_.diagnostics().report(
                        Severity::Error, il->span.begin,
                        "integer literal does not fit in `int`; add a suffix "
                        "(`L` for long, `U`/`UL` for unsigned) to widen it "
                        "(§5)");
                }
            }
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
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
        // §17.12 / §2553: the postfix `!` (null-forgiving) operator yields
        // the non-nullable form of its operand's type. Recording it lets
        // member access through `x!` (and any other consumer) resolve —
        // without it, `x!.member` reads/writes silently fail (#206).
        if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
            if (bg->operand) {
                if (TypeRef* ot = ctx_.exprType(*bg->operand)) {
                    if (ot->nullable) {
                        TypeRef* nn = ctx_.makeTypeRef(*ot);
                        nn->nullable = false;
                        ctx_.setExprType(&e, nn);
                    } else {
                        ctx_.setExprType(&e, ot);  // already non-nullable
                    }
                }
            }
        }
        // §20: the `if` expression `x if c else y` has the common type of
        // its two branches. Take the then-branch's type, falling back to
        // the else-branch (e.g. when the then-branch is a bottom-typed
        // `throw`). Without this the expression is untyped, so member
        // access on its result (`(a if c else b).M`) fails to resolve.
        if (auto* cnd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            TypeRef* tt =
                cnd->thenValue ? ctx_.exprType(*cnd->thenValue) : nullptr;
            TypeRef* et =
                cnd->elseValue ? ctx_.exprType(*cnd->elseValue) : nullptr;
            if (tt) ctx_.setExprType(&e, tt);
            else if (et) ctx_.setExprType(&e, et);
        }
        // §17.4: `is` / `is not` yield bool; `as` yields the nullable form
        // of the target type (a failed cast is null). Recording it lets
        // `(x as T)` — and member access on its result — resolve.
        if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            if (te->op == "is" || te->op == "is not") {
                ctx_.setExprType(&e,
                                 ctx_.primitiveTypeRef(PrimitiveType::Bool));
            } else if (te->op == "as" && te->testType) {
                if (TypeRef* tt = ctx_.resolvedType(*te->testType)) {
                    if (tt->nullable) {
                        ctx_.setExprType(&e, tt);
                    } else {
                        TypeRef* nn = ctx_.makeTypeRef(*tt);
                        nn->nullable = true;
                        ctx_.setExprType(&e, nn);
                    }
                }
            }
        }
        // §19: a `case` expression's type is the common type of its arm
        // bodies — take the first expression-bodied arm's type. Without it,
        // member access on a case result (`(case … end).M`) fails to
        // resolve.
        if (auto* cse = dynamic_cast<const ast::CaseExpr*>(&e)) {
            for (const auto& arm : cse->arms) {
                if (!arm.exprBody) continue;
                if (TypeRef* at = ctx_.exprType(*arm.exprBody)) {
                    ctx_.setExprType(&e, at);
                    break;
                }
            }
        }
        // Slice 3d: MemberAccessExpr resolution. Runs after the target
        // is visited (so its TypeRef is available), looks the member up
        // on the target's TypeSymbol, and annotates the access with the
        // resolved member symbol and field/property type.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            resolveMemberAccess(*ma);
        }
        // §17.5 null-conditional member access `x?.M`.
        if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
            resolveNullCondMember(*nm);
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
                        } else if (t->kind == ast::TypeDeclKind::Class) {
                            // Slice T7: constructor arity / arg types.
                            checkNewExprArgs(*ne, t);
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
        // §5.2: an explicit conversion `int(x)` — the callee is a primitive
        // type name with no symbol. The call's type is that primitive type.
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ce.callee.get())) {
            if (!ctx_.exprSymbol(*ce.callee)) {
                PrimitiveType pt = primitiveFromName(id->name);
                if (pt != PrimitiveType::None) {
                    ctx_.setExprType(&ce, ctx_.primitiveTypeRef(pt));
                    return;
                }
            }
        }
        Symbol* calleeSym = ctx_.exprSymbol(*ce.callee);
        // §5.4: an enum cast `E(i)` — the callee is an enum type name; the
        // call's type is that enum.
        if (auto* ts = dynamic_cast<TypeSymbol*>(calleeSym)) {
            if (ts->kind == ast::TypeDeclKind::Enum) {
                TypeRef* et = ctx_.makeTypeRef();
                et->kind = TypeRef::Kind::Named;
                et->declaration = ts;
                ctx_.setExprType(&ce, et);
                return;
            }
        }
        // §22.6: arity-based overload resolution. The callee resolved to one
        // method of its name; when that one's parameter count doesn't match
        // this call's argument count, re-select the same-named overload
        // whose parameter count does. (Type-based tie-breaking between
        // equal-arity overloads is a later refinement.)
        if (auto* ms0 = dynamic_cast<MethodSymbol*>(calleeSym)) {
            if (ms0->parent && ms0->params.size() != ce.args.size()) {
                for (Symbol* m : ms0->parent->members) {
                    auto* cand = dynamic_cast<MethodSymbol*>(m);
                    if (cand && cand->name == ms0->name &&
                        cand->params.size() == ce.args.size()) {
                        ctx_.setExprSymbol(ce.callee.get(), cand);
                        calleeSym = cand;
                        break;
                    }
                }
            }
        }
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
        } else if (auto* xd = dynamic_cast<ExternDeclSymbol*>(calleeSym)) {
            // §32: a call to an extern method takes its declared return type.
            if (xd->externDecl && xd->externDecl->returnType) {
                if (auto* rt = ctx_.resolvedType(*xd->externDecl->returnType))
                    ctx_.setExprType(&ce, rt);
            } else {
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
        checkArgsAgainstParams(md->params, ce.args, ce.span.begin,
                               std::string("call to `") +
                               std::string(ms->name) + "`");
    }

    // Slices T5/T6/T7: shared check of an all-positional argument list
    // against a parameter list — argument count within [required,
    // total] (optional params lower the minimum), then per-argument
    // assignability. Callers must have already ruled out named args
    // and overload ambiguity.
    void checkArgsAgainstParams(const std::vector<ast::Param>& params,
                                const std::vector<ast::CallArgument>& args,
                                SourceLocation loc,
                                const std::string& calleeDesc) {
        for (const auto& a : args) {
            if (!a.name.empty()) return;  // named-arg call — defer
        }
        std::size_t total = params.size();
        std::size_t required = 0;
        for (const auto& p : params) {
            if (!p.defaultValue) ++required;
        }
        std::size_t got = args.size();
        if (got < required || got > total) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                calleeDesc + " has " + std::to_string(got) +
                " argument(s) but expects " +
                (required == total
                     ? std::to_string(total)
                     : std::to_string(required) + "–" +
                           std::to_string(total)) +
                " (§22.5)");
            return;
        }
        for (std::size_t i = 0; i < got && i < total; ++i) {
            const ast::CallArgument& a = args[i];
            if (!a.value || !a.modifier.empty() || a.isDiscard) continue;
            const ast::Param& p = params[i];
            if (!p.type) continue;
            checkAssignable(ctx_.exprType(*a.value),
                            ctx_.resolvedType(*p.type),
                            a.value->span.begin,
                            std::string("argument ") +
                            std::to_string(i + 1) + " to " + calleeDesc);
        }
    }

    // Slice T7: check `new T(args)` against T's sole constructor (if
    // any). Zero declared constructors → the implicit default ctor
    // takes no arguments. Multiple constructors → overload resolution
    // is deferred, so skip.
    void checkNewExprArgs(const ast::NewExpr& ne, TypeSymbol* t) {
        // A class with a primary constructor (parenthesized params after
        // the name) carries those params on the TypeDecl rather than as
        // a ConstructorSymbol, and may combine with chained body
        // constructors. That interaction is deferred — skip such types.
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl)) {
            if (td->hasPositionalFields) return;
        }
        std::vector<ConstructorSymbol*> ctors;
        for (Symbol* m : t->members) {
            if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
                if (!cs->isStatic) ctors.push_back(cs);
            }
        }
        if (ctors.size() > 1) return;  // overloaded — defer
        if (ctors.empty()) {
            // Implicit default constructor: zero parameters. (A primary
            // constructor would surface as a ConstructorSymbol, so the
            // empty case really is the no-arg default.)
            for (const auto& a : ne.args) {
                if (!a.name.empty()) return;
            }
            if (!ne.args.empty()) {
                ctx_.diagnostics().report(
                    Severity::Error, ne.span.begin,
                    std::string("`new ") + std::string(t->name) +
                    "` has " + std::to_string(ne.args.size()) +
                    " argument(s) but the implicit default constructor "
                    "takes 0 (§22.7)");
            }
            return;
        }
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(ctors[0]->decl);
        if (!cd) return;
        checkArgsAgainstParams(cd->params, ne.args, ne.span.begin,
                               std::string("`new ") + std::string(t->name) +
                               "`");
    }

    // §17.1: postfix `.` member access. The target's type determines
    // the lookup scope. For now we handle `this.member` (lookup in the
    // current type) and `<expr>.member` where the expression has a
    // resolved Named or Primitive-Object type. Generics and inherited
    // members from base types are deferred to a later slice.
    // The TypeSymbol a member-access target denotes (for `.` and `?.`):
    // `this`/`base` keywords, then the target's exprType, then a fallback
    // through its exprSymbol (local/parameter declared type, or a type name
    // for static access). Returns null when it can't be determined.
    TypeSymbol* memberTargetType(const ast::Expr* target) {
        TypeSymbol* targetType = nullptr;
        // `this.X` — currentType_ takes precedence over name lookup so
        // we don't have to teach `this` to be an expression with type
        // currentType_.
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
            if (id->name == "this") {
                return currentType_;
            } else if (id->name == "base" && currentType_) {
                // §12.3: `base.X` resolves X against the base class, so an
                // override in the current type is bypassed and the base
                // member is found.
                for (TypeSymbol* b : currentType_->baseTypes) {
                    if (b && b->kind == ast::TypeDeclKind::Class) return b;
                }
            }
        }
        if (target) {
            auto* tt = ctx_.exprType(*target);
            if (tt && tt->kind == TypeRef::Kind::Named) {
                targetType = tt->declaration;
            } else if (!tt) {
                auto* sym = ctx_.exprSymbol(*target);
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
                } else if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                    // Static member access `Type.member`: the target is a
                    // type name, so look the member up on that type.
                    targetType = ts;
                }
            }
        }
        return targetType;
    }

    void resolveMemberAccess(const ast::MemberAccessExpr& ma) {
        TypeSymbol* targetType = nullptr;
        // `this.X` — currentType_ takes precedence over name lookup so
        // we don't have to teach `this` to be an expression with type
        // currentType_.
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                ma.target.get())) {
            if (id->name == "this") {
                targetType = currentType_;
            } else if (id->name == "base" && currentType_) {
                // §12.3: `base.X` resolves X against the base class, so an
                // override in the current type is bypassed and the base
                // member is found.
                for (TypeSymbol* b : currentType_->baseTypes) {
                    if (b && b->kind == ast::TypeDeclKind::Class) {
                        targetType = b;
                        break;
                    }
                }
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
                } else if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                    // Static member access `Type.member`: the target is a
                    // type name, so look the member up on that type. (The
                    // static-vs-instance correctness of the member is
                    // enforced by the static-context checks, not here.)
                    targetType = ts;
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
        } else if (dynamic_cast<EnumConstantSymbol*>(member) && targetType) {
            // §10: an enum-constant access `E.Green` has the enum type, so a
            // local inferred from it (`var e := E.Green`) and a `case`
            // scrutinee both carry the enum type.
            TypeRef* et = ctx_.makeTypeRef();
            et->kind = TypeRef::Kind::Named;
            et->declaration = targetType;
            ctx_.setExprType(&ma, et);
        }
    }

    // §7.5 null-conditional member access `x?.M`. Resolves the member on
    // the target's (non-nullable) type exactly like `.`, but the result
    // type is ALWAYS nullable (the access yields null when the target is
    // null). The member's own type is looked up; its nullable form is the
    // expression's type, so member access chains (`a?.b?.c`) resolve.
    void resolveNullCondMember(const ast::NullCondMemberExpr& nm) {
        if (!nm.target) return;
        TypeSymbol* targetType = memberTargetType(nm.target.get());
        if (!targetType) return;
        Symbol* member = lookupMember(targetType, nm.member);
        if (!member) return;
        if (auto* msym = dynamic_cast<MemberSymbol*>(member)) {
            if (msym->visibility == ast::Visibility::Private &&
                msym->parent && msym->parent != currentType_) {
                ctx_.diagnostics().report(
                    Severity::Error, nm.span.begin,
                    std::string("`") + std::string(nm.member) +
                    "` is private to type `" +
                    std::string(msym->parent->name) +
                    "` and not accessible from `" +
                    std::string(currentType_ ? currentType_->name
                                             : std::string_view("(global)")) +
                    "` (§13.1 — `private` is visible to the declaring "
                    "class only)");
            }
        }
        ctx_.setExprSymbol(&nm, member);
        TypeRef* memberTy = nullptr;
        if (auto* fs = dynamic_cast<FieldSymbol*>(member)) {
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
                if (fd->type) memberTy = ctx_.resolvedType(*fd->type);
        } else if (auto* ps = dynamic_cast<PropertySymbol*>(member)) {
            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl))
                if (pd->type) memberTy = ctx_.resolvedType(*pd->type);
        }
        if (!memberTy) return;
        if (memberTy->nullable) {
            ctx_.setExprType(&nm, memberTy);  // already nullable
        } else {
            TypeRef* nn = ctx_.makeTypeRef(*memberTy);
            nn->nullable = true;
            ctx_.setExprType(&nm, nn);
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
        // `??` null-coalescing (§17.1 L12): the result is the non-nullable
        // form of the LHS — `a ?? b` is `a` when non-null, else `b`, and the
        // spec requires the RHS to be compatible with the non-nullable form
        // of the left (§ null-coalescing). Deriving the result from the LHS
        // (rather than the RHS) keeps the whole `??` typed even when the RHS
        // is a call or postfix-`!` expression whose own type is not recorded.
        if (op == "??") {
            TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
            if (lt) {
                if (lt->nullable) {
                    TypeRef* nn = ctx_.makeTypeRef(*lt);
                    nn->nullable = false;
                    ctx_.setExprType(&be, nn);
                } else {
                    ctx_.setExprType(&be, lt);  // already non-nullable
                }
            } else if (be.right) {
                // LHS untyped — fall back to the RHS type when available.
                if (TypeRef* rt = ctx_.exprType(*be.right))
                    ctx_.setExprType(&be, rt);
            }
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
        // `move x` (§28.3) keeps the operand's type — the result is the
        // same value with ownership transferred. Without the type, member
        // access on a moved value (`var s := move r; s.M`) fails to resolve.
        if (op == "+" || op == "-" || op == "~" || op == "move") {
            TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
            if (ot) ctx_.setExprType(&ue, ot);
            return;
        }
        // `await` — result-type rule is deeper (Task<T> -> T); not typed
        // here.
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
            // §A.7: a `source with …` expression has the same type as its
            // source (an object initializer / record copy preserves type).
            if (we->source) {
                if (auto* st = ctx_.exprType(*we->source))
                    ctx_.setExprType(&e, st);
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
