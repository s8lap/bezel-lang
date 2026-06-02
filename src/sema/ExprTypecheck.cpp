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
    // §5 / issue #184: when a LocalDecl has an explicit wider integer type
    // (long, ulong, uint) and the initializer is an unsuffixed int literal
    // that overflows int, widen it to the declared type instead of erroring.
    PrimitiveType currentLiteralWideningHint_ = PrimitiveType::None;
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

    // §2.3: cache of synthetic TypeSymbol objects for always-available
    // built-in classes (Console, Math, Convert, Environment).
    std::unordered_map<std::string, TypeSymbol*> builtinClassCache_;

    // Return (or lazily create) a synthetic TypeSymbol for a built-in class.
    TypeSymbol* builtinClassSym(std::string_view name) {
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

    void visitStmt(const ast::Stmt& s) {
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
            } else if (id->name == "Console" || id->name == "Math" ||
                       id->name == "Convert" || id->name == "Environment" ||
                       id->name == "Random" ||
                       // §3 stdlib collection and helper classes:
                       id->name == "Array" || id->name == "OwnedArray" ||
                       id->name == "Dictionary" || id->name == "OwnedDictionary" ||
                       id->name == "Stack" || id->name == "OwnedStack" ||
                       id->name == "Queue" || id->name == "OwnedQueue" ||
                       id->name == "Set" || id->name == "OwnedSet" ||
                       id->name == "LinkedList" || id->name == "OwnedLinkedList" ||
                       id->name == "StringBuilder" || id->name == "Encoding" ||
                       id->name == "DateTime" || id->name == "TimeSpan" ||
                       // §7 System.IO:
                       id->name == "File" || id->name == "Directory" ||
                       id->name == "Path" ||
                       // §4 Linq:
                       id->name == "Linq") {
                // §2.3: these System classes are auto-imported and always
                // available without an explicit import. Give them a synthetic
                // TypeSymbol (null decl) so member access can proceed; the
                // codegen handles known methods (Write/WriteLine, etc.)
                // as intrinsics.
                auto* ts = builtinClassSym(id->name);
                ctx_.setExprSymbol(&e, ts);
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
            // not, it either requires a suffix OR the declared target type
            // (long/ulong/uint) allows implicit widening (issue #184).
            if (pt == PrimitiveType::Int) {
                unsigned long long mag = 0;
                if (intLiteralMagnitude(il->text, mag) &&
                    (errno == ERANGE || mag > 2147483647ull)) {
                    using P = PrimitiveType;
                    bool widened = false;
                    // Widen to the declared type when in a typed variable context.
                    if (currentLiteralWideningHint_ != P::None) {
                        bool fits = false;
                        switch (currentLiteralWideningHint_) {
                            case P::Long:
                                fits = (errno != ERANGE && mag <= 9223372036854775807ULL);
                                break;
                            case P::ULong:
                                fits = (errno != ERANGE);  // any positive value fits
                                break;
                            case P::UInt:
                                fits = (errno != ERANGE && mag <= 4294967295ULL);
                                break;
                            default: break;
                        }
                        if (fits) {
                            pt = currentLiteralWideningHint_;
                            widened = true;
                        }
                    }
                    if (!widened) {
                        ctx_.diagnostics().report(
                            Severity::Error, il->span.begin,
                            "integer literal does not fit in `int`; add a suffix "
                            "(`L` for long, `U`/`UL` for unsigned) to widen it "
                            "(§5)");
                    }
                }
            }
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
            return;
        }
        if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
            // §18.1: the `f`/`F` suffix gives type `float` (f32); `d`/`D`,
            // `m`/`M`, or no suffix gives `double` (f64) / decimal. Currently
            // only float and double are codegen-backed.
            PrimitiveType pt = PrimitiveType::Double;
            if (!fl->text.empty()) {
                char last = fl->text.back();
                if (last == 'f' || last == 'F') pt = PrimitiveType::Float;
                // `d`/`D` → double (already default); `m`/`M` → decimal
                // (not yet implemented, leave as double for now).
            }
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
            return;
        }
        if (dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Char));
            return;
        }
        if (dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
            dynamic_cast<const ast::VerbatimStringExpr*>(&e)) {
            ctx_.setExprType(
                &e, ctx_.primitiveTypeRef(PrimitiveType::String));
            return;
        }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
            // §18.5: visit embedded expression parts BEFORE returning, so
            // their identifiers resolve and their types are set for codegen.
            for (const auto& part : is->parts)
                if (part.expr) visitExpr(*part.expr);
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
        // §15.1: IndexExpr `a[i]` — the expression type is the array's
        // element type, so member access on array elements (`a[i].field`)
        // can resolve. The element type comes from the target's InlineArray
        // TypeRef, or from an indexer-property return type (deferred).
        // §4.6: string indexer `s[i]` → char (code point).
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (ix->target) {
                if (auto* tt = ctx_.exprType(*ix->target)) {
                    if (tt->kind == TypeRef::Kind::InlineArray && tt->arrayElement)
                        ctx_.setExprType(&e, tt->arrayElement);
                    else if (tt->kind == TypeRef::Kind::Primitive &&
                             tt->primitive == PrimitiveType::String)
                        ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Char));
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
        // call produces a value of that enum type. (Records are NOT
        // call-constructible: per §9.5 a record is built with `new Record(...)`
        // like a class, so a bare `Record(...)` call is not construction and is
        // left untyped here — it surfaces as an error at the use site.)
        if (auto* ts = dynamic_cast<TypeSymbol*>(calleeSym)) {
            if (ts->kind == ast::TypeDeclKind::Enum) {
                TypeRef* et = ctx_.makeTypeRef();
                et->kind = TypeRef::Kind::Named;
                et->declaration = ts;
                ctx_.setExprType(&ce, et);
                return;
            }
            // §9.5: records (like classes) are constructed with `new`, not by
            // calling the type name. A bare `Record(...)` is not a valid
            // expression — reject it with a fix-it rather than leaving the
            // call untyped (which would silently miscompile downstream).
            if (ts->kind == ast::TypeDeclKind::Record ||
                ts->kind == ast::TypeDeclKind::Class) {
                ctx_.diagnostics().report(
                    Severity::Error, ce.span.begin,
                    "`" + std::string(ts->name) +
                        "` is constructed with `new " + std::string(ts->name) +
                        "(...)`, not by calling the type name (§9.5)");
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
        // CG60: call through a function-typed local/parameter — propagate the
        // function's declared return type to the CallExpr so that downstream
        // expressions (e.g. `var x := f(n)`) are typed correctly.
        if (auto* loc = dynamic_cast<LocalSymbol*>(calleeSym)) {
            if (loc->declType && loc->declType->kind == TypeRef::Kind::Function &&
                loc->declType->funcReturn)
                ctx_.setExprType(&ce, loc->declType->funcReturn);
        } else if (auto* par = dynamic_cast<ParameterSymbol*>(calleeSym)) {
            TypeRef* prt = par->declType ? ctx_.resolvedType(*par->declType) : nullptr;
            if (prt && prt->kind == TypeRef::Kind::Function && prt->funcReturn)
                ctx_.setExprType(&ce, prt->funcReturn);
        }
        // CG27c / CG60: if the callee has no resolved symbol but has a type,
        // propagate the appropriate type to the CallExpr:
        // - If the callee has a Function TypeRef (e.g. indexed array element
        //   `handlers[i]`), use funcReturn as the call's return type.
        // - Otherwise (intrinsic member like `n.ToString()`), use the callee
        //   type directly.
        if (!calleeSym) {
            if (auto* rt = ctx_.exprType(*ce.callee)) {
                if (rt->kind == TypeRef::Kind::Function && rt->funcReturn)
                    ctx_.setExprType(&ce, rt->funcReturn);
                else if (rt->kind != TypeRef::Kind::Function)
                    ctx_.setExprType(&ce, rt);
                // If Function with null funcReturn: leave untyped (void/unknown)
            }
            // §6 / stdlib §5: Math.Abs and Math.Sign are polymorphic — the
            // return type matches the argument type, not always Int. If the
            // member access was typed as Int but the first argument is
            // float/double, override with Double so the string formatter and
            // type inference work correctly.
            if (auto* rt = ctx_.exprType(ce)) {
                if (rt->kind == TypeRef::Kind::Primitive &&
                    rt->primitive == PrimitiveType::Int &&
                    !ce.args.empty() && ce.args[0].value) {
                    const ast::Expr& argE = *ce.args[0].value;
                    if (auto* argTy = ctx_.exprType(argE)) {
                        if (argTy->kind == TypeRef::Kind::Primitive &&
                            (argTy->primitive == PrimitiveType::Double ||
                             argTy->primitive == PrimitiveType::Float ||
                             argTy->primitive == PrimitiveType::Long)) {
                            // Determine which method this is — only override
                            // for the numeric functions that return same-as-arg.
                            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                                    ce.callee.get())) {
                                if (ma->member == "Abs" || ma->member == "Sign" ||
                                    ma->member == "Min" || ma->member == "Max" ||
                                    ma->member == "Clamp") {
                                    ctx_.setExprType(&ce, argTy);
                                }
                            }
                        }
                    }
                }
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
        // §3.4: `int.MaxValue`, `double.NaN`, `char.MaxValue`, etc. — static
        // properties on primitive keyword aliases. The target is an identifier
        // that names a primitive type; there's no TypeSymbol in scope for it.
        if (!targetType && ma.target) {
            if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
                PrimitiveType pt = primitiveFromName(id2->name);
                if (pt != PrimitiveType::None) {
                    const auto& mbr = ma.member;
                    // Numeric properties: MaxValue/MinValue → same type.
                    if (mbr == "MaxValue" || mbr == "MinValue" ||
                        mbr == "Zero" || mbr == "One") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                        return;
                    }
                    // double/float properties.
                    if (pt == PrimitiveType::Double || pt == PrimitiveType::Float) {
                        if (mbr == "NaN" || mbr == "PositiveInfinity" ||
                            mbr == "NegativeInfinity" || mbr == "Epsilon") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                            return;
                        }
                        if (mbr == "IsNaN" || mbr == "IsInfinity") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
                    }
                    // int.Parse(s) → int.
                    if (mbr == "Parse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                        return;
                    }
                    // char static classification methods → bool.
                    if (pt == PrimitiveType::Char) {
                        if (mbr == "IsDigit" || mbr == "IsLetter" ||
                            mbr == "IsLetterOrDigit" || mbr == "IsWhiteSpace" ||
                            mbr == "IsUpper" || mbr == "IsLower" ||
                            mbr == "IsPunctuation" || mbr == "IsSymbol") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
                        if (mbr == "ToUpper" || mbr == "ToLower") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Char));
                            return;
                        }
                        // char.ToString(c) → string.
                        if (mbr == "ToString") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                            return;
                        }
                    }
                    // string static methods → bool or string or int.
                    if (pt == PrimitiveType::String) {
                        if (mbr == "IsNullOrEmpty" || mbr == "IsNullOrWhiteSpace") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
                        if (mbr == "Concat" || mbr == "Join" || mbr == "Empty" ||
                            mbr == "From" || mbr == "Format" || mbr == "Repeat") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                            return;
                        }
                        // string.Compare → int (ordinal comparison result).
                        if (mbr == "Compare") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                            return;
                        }
                    }
                    // int static methods → int or bool.
                    if (pt == PrimitiveType::Int || pt == PrimitiveType::Long ||
                        pt == PrimitiveType::Byte || pt == PrimitiveType::SByte ||
                        pt == PrimitiveType::Short || pt == PrimitiveType::UShort ||
                        pt == PrimitiveType::UInt || pt == PrimitiveType::ULong) {
                        if (mbr == "Parse") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                            return;
                        }
                        // int.TryParse(s, out result): bool — §3.4/stdlib §5.2
                        if (mbr == "TryParse") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
                        // int.MaxValue / int.MinValue: int
                        if (mbr == "MaxValue" || mbr == "MinValue") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                            return;
                        }
                    }
                    if (pt == PrimitiveType::Double || pt == PrimitiveType::Float) {
                        if (mbr == "Parse") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                            return;
                        }
                        if (mbr == "TryParse") {
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
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
        if (!targetType) {
            // CG27a (§4.6): handle known intrinsic string members directly —
            // string has no user TypeSymbol in the symbol table.
            if (ma.target) {
                auto* tt = ctx_.exprType(*ma.target);
                if (tt && tt->kind == TypeRef::Kind::Primitive) {
                    if (tt->primitive == PrimitiveType::String) {
                        if (ma.member == "Length") {
                            // stdlib §9.6: Length: int
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                            return;
                        }
                        if (ma.member == "IsAscii") {
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
                        if (ma.member == "ToString") {
                            // string.ToString() returns itself (§9.6)
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                            return;
                        }
                        // CG27f: stdlib §9.6 string methods — result types.
                        if (ma.member == "Contains" ||
                            ma.member == "StartsWith" ||
                            ma.member == "EndsWith" ||
                            ma.member == "Equals") {
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
                        if (ma.member == "CompareTo" ||
                            ma.member == "IndexOf" ||
                            ma.member == "LastIndexOf" ||
                            ma.member == "CountOf" ||
                            ma.member == "ToInt32" || ma.member == "ToUInt32") {
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                            return;
                        }
                        if (ma.member == "ToInt64") {
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::Long));
                            return;
                        }
                        if (ma.member == "ToDouble" || ma.member == "ToFloat") {
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::Double));
                            return;
                        }
                        if (ma.member == "ToBoolean") {
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                            return;
                        }
                        if (ma.member == "Substring" ||
                            ma.member == "Trim" || ma.member == "TrimStart" ||
                            ma.member == "TrimEnd" || ma.member == "ToLower" ||
                            ma.member == "ToUpper" || ma.member == "Replace" ||
                            ma.member == "PadLeft" || ma.member == "PadRight" ||
                            ma.member == "Repeat" || ma.member == "Remove" ||
                            ma.member == "Insert" ||
                            ma.member == "GetField") {
                            ctx_.setExprType(
                                &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                            return;
                        }
                        // Emit a diagnostic for unrecognized string members.
                        ctx_.diagnostics().report(
                            Severity::Error, ma.span.begin,
                            std::string("string member `.") +
                            std::string(ma.member) +
                            "` is not yet supported in codegen");
                    } else if (ma.member == "ToString") {
                        // CG27c: ToString() is an intrinsic on all primitive
                        // types (§5) — returns the string representation.
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                        return;
                    } else if (ma.member == "IsNaN" || ma.member == "IsInfinity" ||
                               ma.member == "IsFinite") {
                        // §6.3: FP predicates — bool result.
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    } else if (ma.member == "Abs") {
                        // §5: Abs on numeric primitive — same type.
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(tt->primitive));
                        return;
                    }
                }
            }
            return;
        }
        // Lookup member by name. Slice S5: walks own members first,
        // then transitively each base type — first match wins (no
        // ambiguity resolution between base interfaces yet).
        Symbol* member = lookupMember(targetType, ma.member);
        if (!member) {
            // §2.3: built-in class TypeSymbols (Console, Math, Convert, …)
            // have null `decl` and empty members. Assign return types for
            // their known methods so callers can infer the call result type.
            if (!targetType->decl) {
                std::string_view cls = targetType->name;
                const auto& m = ma.member;
                // Convert methods (§9 stdlib).
                if (cls == "Convert" || cls == "Math") {
                    auto setInt = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int)); };
                    auto setLong = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Long)); };
                    auto setDbl = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Double)); };
                    auto setBool = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool)); };
                    auto setStr = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String)); };
                    if (m=="ToInt32"||m=="Abs"||m=="Sign") setInt();
                    else if (m=="ToInt64") setLong();
                    else if (m=="ToDouble"||m=="Sqrt"||m=="Pow"||m=="Log"||m=="Log2"||m=="Log10"||
                             m=="Ceiling"||m=="Floor"||m=="Round"||m=="Truncate"||
                             m=="PI"||m=="E"||
                             m=="Fmod"||m=="IEEERemainder"||m=="Sin"||m=="Cos"||m=="Tan"||
                             m=="Asin"||m=="Acos"||m=="Atan"||m=="Atan2"||m=="Exp"||m=="Hypot"||
                             m=="Sinh"||m=="Cosh"||m=="Tanh") setDbl();
                    // Min/Max/Clamp are overloaded (int or double) — leave untyped.
                    else if (m=="ToBoolean") setBool();
                    else if (m=="ToString") setStr();
                }
                // Console input methods.
                if (cls == "Console") {
                    if (m == "ReadLine")
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::String,
                                                       /*nullable=*/true));
                    else if (m == "ReadKey")
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Char));
                    // Write/WriteLine → unit (leave untyped).
                }
                // Random class methods.
                if (cls == "Random") {
                    if (m == "Next" || m == "NextInt")
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    else if (m == "NextDouble" || m == "NextFloat")
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Double));
                    // Seed → unit (leave untyped)
                }
            }
            // Slice 3d intentionally does NOT diagnose unknown members.
            return;
        }
        // Slice S8: visibility check. A `private` member is accessible
        // only from inside the declaring type. Walk the member's
        // parent TypeSymbol; if it isn't currentType_, diagnose.
        // Other visibilities (internal/protected/public) are deferred.
        // Exception: if there are also public members with the same name
        // (overloads), suppress the error — the codegen picks by arity.
        if (auto* msym = dynamic_cast<MemberSymbol*>(member)) {
            if (msym->visibility == ast::Visibility::Private &&
                msym->parent && msym->parent != currentType_) {
                // Check if there is any public member with the same name
                // in the same type — if so, this is an overload set and
                // the codegen will select the accessible one.
                bool hasPublicOverload = false;
                for (Symbol* sib : msym->parent->members) {
                    if (sib && sib != member && sib->name == member->name) {
                        if (auto* sm = dynamic_cast<MemberSymbol*>(sib)) {
                            if (sm->visibility != ast::Visibility::Private)
                                hasPublicOverload = true;
                        }
                    }
                }
                if (!hasPublicOverload) {
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
            } else if (fs->posParam && fs->posParam->type) {
                // §9: positional record field — type comes from the Param.
                if (auto* rt = ctx_.resolvedType(*fs->posParam->type)) {
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
                // Suppress error if a non-private sibling with the same name
                // exists (overload set) — codegen will pick the accessible one.
                bool hasPublicOverload = false;
                for (Symbol* sib : msym->parent->members) {
                    if (sib && sib != member && sib->name == member->name) {
                        if (auto* sm = dynamic_cast<MemberSymbol*>(sib)) {
                            if (sm->visibility != ast::Visibility::Private)
                                hasPublicOverload = true;
                        }
                    }
                }
                if (!hasPublicOverload) {
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
    // §25: the user-defined `static operator <op>` method that applies to a
    // binary expression, if either operand's type (class or record)
    // declares one. `=`/`<>` on classes are excluded (they invoke Equals,
    // §25.2).
    MethodSymbol* findBinaryOperator(const ast::BinaryExpr& be) {
        auto onType = [&](const ast::Expr* operand) -> MethodSymbol* {
            if (!operand) return nullptr;
            TypeSymbol* ts = memberTargetType(operand);
            if (!ts) return nullptr;
            // §25: user-defined `operator =` and `<>` on class types take
            // precedence over built-in reference equality — only skip when
            // no user-defined operator exists (handled by falling through).
            for (Symbol* m : ts->members) {
                auto* ms = dynamic_cast<MethodSymbol*>(m);
                if (ms && ms->isOperator && ms->operatorToken == be.op &&
                    ms->params.size() == 2)
                    return ms;
            }
            return nullptr;
        };
        if (auto* m = onType(be.left.get())) return m;
        return onType(be.right.get());
    }

    void typeBinary(const ast::BinaryExpr& be) {
        const auto& op = be.op;
        // §25: a user-defined operator overload takes precedence over the
        // built-in result-type rules; the expression is that method's call.
        if (op != "and" && op != "or" && op != "in") {
            if (MethodSymbol* opm = findBinaryOperator(be)) {
                ctx_.setExprSymbol(&be, opm);
                if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl))
                    if (od->returnType)
                        if (auto* rt = ctx_.resolvedType(*od->returnType))
                            ctx_.setExprType(&be, rt);
                return;
            }
        }
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
            // When the LHS is a null literal (typed as nullable Object) or is
            // untyped, prefer the RHS type — `null ?? x` has x's type.
            bool lhsIsNullLiteral =
                be.left && dynamic_cast<const ast::NullLiteralExpr*>(be.left.get());
            if (lt && !lhsIsNullLiteral) {
                if (lt->nullable) {
                    TypeRef* nn = ctx_.makeTypeRef(*lt);
                    nn->nullable = false;
                    ctx_.setExprType(&be, nn);
                } else {
                    ctx_.setExprType(&be, lt);  // already non-nullable
                }
            } else if (be.right) {
                // LHS untyped or null literal — fall back to the RHS type.
                if (TypeRef* rt = ctx_.exprType(*be.right))
                    ctx_.setExprType(&be, rt);
            }
            return;
        }
        // Range ops, etc.: leave un-typed (later slices handle).
    }

    // §17.1 result-type table for unary operators (slice 3c).
    // §25: a user-defined unary `operator <op>` (one parameter) declared on
    // the operand's type. Overloadable prefix unary operators are `-`/`~`.
    MethodSymbol* findUnaryOperator(const ast::UnaryExpr& ue) {
        if (ue.op != "-" && ue.op != "~") return nullptr;
        TypeSymbol* ts = ue.operand ? memberTargetType(ue.operand.get())
                                    : nullptr;
        if (!ts) return nullptr;
        for (Symbol* m : ts->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (ms && ms->isOperator && ms->operatorToken == ue.op &&
                ms->params.size() == 1)
                return ms;
        }
        return nullptr;
    }

    void typeUnary(const ast::UnaryExpr& ue) {
        const auto& op = ue.op;
        // §25: a user-defined unary operator overload.
        if (MethodSymbol* opm = findUnaryOperator(ue)) {
            ctx_.setExprSymbol(&ue, opm);
            if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl))
                if (od->returnType)
                    if (auto* rt = ctx_.resolvedType(*od->returnType))
                        ctx_.setExprType(&ue, rt);
            return;
        }
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
            // Resolve callee's parameter list to type out-var locals.
            sema::Symbol* calleeSym2 = ce->callee ? ctx_.exprSymbol(*ce->callee) : nullptr;
            const ast::MethodDecl* calleeMd2 = nullptr;
            if (auto* ms2 = dynamic_cast<sema::MethodSymbol*>(calleeSym2))
                calleeMd2 = dynamic_cast<const ast::MethodDecl*>(ms2->decl);
            std::size_t pos2 = 0;
            for (const auto& a : ce->args) {
                // §22.4: `out var x` declares x in the current scope.
                // Register the symbol BEFORE visiting the IdentifierExpr so
                // lookup finds it. Infer type from the callee's parameter.
                if (a.modifier == "out var" && a.value && currentScope_) {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                       a.value.get())) {
                        auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                        lsym->name = id->name;
                        lsym->decl = id;
                        // Infer type from the callee's formal parameter.
                        if (calleeMd2 && pos2 < calleeMd2->params.size() &&
                            calleeMd2->params[pos2].type) {
                            lsym->declType = ctx_.resolvedType(
                                *calleeMd2->params[pos2].type);
                            // out params are nullable refs — strip nullable
                            // (the variable holds the non-nullable value).
                            if (lsym->declType && lsym->declType->nullable) {
                                TypeRef* nn = ctx_.makeTypeRef(*lsym->declType);
                                nn->nullable = false;
                                lsym->declType = nn;
                            }
                        }
                        currentScope_->names[id->name] = lsym;
                    }
                }
                if (a.value) visitExpr(*a.value);
                ++pos2;
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
            // §A.7 / §21.7: lambdas create their own scope with parameter
            // symbols. Each parameter becomes a ParameterSymbol whose
            // declType is inferred from the explicit type annotation (untyped
            // lambda params remain null — callers must supply context).
            Scope lamScope;
            lamScope.parent = currentScope_;
            std::vector<sema::Symbol*> lamParamSyms;
            for (const auto& p : lam->params) {
                auto* ps = ctx_.makeSymbol<ParameterSymbol>();
                ps->name = p.name;
                ps->modifier = p.modifier;
                if (p.type) ps->declType = p.type.get();
                lamParamSyms.push_back(ps);
                if (!p.name.empty()) lamScope.names[p.name] = ps;
            }
            ctx_.setLambdaParams(lam, std::move(lamParamSyms));
            // §21.7: suppress enclosing callable's return context;
            // §21.5: reset loop/catch depth across the lambda boundary.
            Scope* saveScope = currentScope_;
            ReturnCtx saveRet = returnCtx_;
            TypeRef* saveRetTy = currentReturnType_;
            int saveLoop = loopDepth_;
            int saveCatch = catchDepth_;
            currentScope_ = &lamScope;
            returnCtx_ = ReturnCtx::None;
            currentReturnType_ = nullptr;
            loopDepth_ = 0;
            catchDepth_ = 0;
            if (lam->exprBody) visitExpr(*lam->exprBody);
            for (const auto& s : lam->stmts) if (s) visitStmt(*s);
            currentScope_ = saveScope;
            returnCtx_ = saveRet;
            currentReturnType_ = saveRetTy;
            loopDepth_ = saveLoop;
            catchDepth_ = saveCatch;
            // §A.7: set the lambda's exprType to a Function TypeRef so
            // that `var f := lam; f(...)` calls type-check correctly and
            // lowerCall can detect it as a function-typed local.
            {
                auto* lamTy = ctx_.makeTypeRef();
                lamTy->kind = TypeRef::Kind::Function;
                for (const auto& p : lam->params) {
                    TypeRef::FuncParam fp;
                    fp.modifier = p.modifier;
                    fp.name = p.name;
                    if (p.type) fp.type = ctx_.resolvedType(*p.type);
                    lamTy->funcParams.push_back(std::move(fp));
                }
                // Infer the return type: from exprBody for expression lambdas,
                // or by scanning block-body stmts for the first `return expr`.
                TypeRef* inferredReturn = nullptr;
                if (lam->exprBody) {
                    inferredReturn = ctx_.exprType(*lam->exprBody);
                } else {
                    // Scan stmts (flat, not recursive) for a `return EXPR`.
                    for (const auto& s : lam->stmts) {
                        if (!s) continue;
                        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get())) {
                            if (rs->value) { inferredReturn = ctx_.exprType(*rs->value); break; }
                        }
                        // Check common wrappers: DoEndBlock, IfStmt body.
                        auto scanInner = [&](const std::vector<std::unique_ptr<ast::Stmt>>& inner) {
                            for (const auto& ss : inner) {
                                if (!ss) continue;
                                if (auto* rs2 = dynamic_cast<const ast::ReturnStmt*>(ss.get()))
                                    if (rs2->value && !inferredReturn)
                                        inferredReturn = ctx_.exprType(*rs2->value);
                            }
                        };
                        if (auto* bs = dynamic_cast<const ast::DoEndBlock*>(s.get()))
                            scanInner(bs->body);
                        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
                            scanInner(ifs->ifBranch.body);
                            for (auto& el : ifs->elifBranches) scanInner(el.body);
                            scanInner(ifs->elseBody);
                        }
                        if (inferredReturn) break;
                    }
                    // Fallback: use int (i32) for lambdas with no visible return.
                    if (!inferredReturn)
                        inferredReturn = ctx_.primitiveTypeRef(PrimitiveType::Int);
                }
                lamTy->funcReturn = inferredReturn;
                ctx_.setExprType(lam, lamTy);
            }
        } else if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
            if (ce->scrutinee) visitExpr(*ce->scrutinee);
            TypeRef* scrutTy =
                ce->scrutinee ? ctx_.exprType(*ce->scrutinee) : nullptr;
            for (const auto& arm : ce->arms) {
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
                    lsym->declType = bp->type ? ctx_.resolvedType(*bp->type)
                                              : scrutTy;
                    armScope.names[bp->name] = lsym;
                    Scope* saveScope = currentScope_;
                    currentScope_ = &armScope;
                    if (arm.guard) visitExpr(*arm.guard);
                    if (arm.exprBody) visitExpr(*arm.exprBody);
                    for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                    currentScope_ = saveScope;
                } else if (dp && currentScope_ && dp->typeName) {
                    // §19: DestructuringPattern — same logic as in CaseStmt.
                    TypeSymbol* recTy = nullptr;
                    std::string_view lastName = dp->typeName->parts.empty()
                        ? std::string_view{}
                        : dp->typeName->parts.back();
                    // Search ctx_.symbols() for the record TypeSymbol by name.
                    // This is more robust than searching only module children
                    // since symbols may be nested or added during synthesis.
                    for (const auto& sym : ctx_.symbols()) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(sym.get())) {
                            if (ts->name == lastName &&
                                ts->kind == ast::TypeDeclKind::Record) {
                                recTy = ts; break;
                            }
                        }
                    }
                    std::vector<FieldSymbol*> fields;
                    if (recTy) {
                        for (Symbol* m : recTy->members) {
                            if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                                bool isStatic = false;
                                for (auto& mod : fs->modifiers)
                                    if (mod == "static") { isStatic = true; break; }
                                if (!isStatic) fields.push_back(fs);
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
                        lsym->decl = reinterpret_cast<const ast::Node*>(&sp);
                        lsym->declType = fieldTy;
                        armScope.names[sp.nakedBinding] = lsym;
                    }
                    Scope* saveScope = currentScope_;
                    currentScope_ = &armScope;
                    if (arm.guard) visitExpr(*arm.guard);
                    if (arm.exprBody) visitExpr(*arm.exprBody);
                    for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                    currentScope_ = saveScope;
                } else {
                    if (arm.guard) visitExpr(*arm.guard);
                    if (arm.exprBody) visitExpr(*arm.exprBody);
                    for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                }
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
        } else if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
            // §16: visit lo, hi, and step so that identifiers inside range
            // bounds (e.g. `1..n`) get their symbols resolved.
            if (re->lo) visitExpr(*re->lo);
            if (re->hi) visitExpr(*re->hi);
            if (re->step) visitExpr(*re->step);
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
