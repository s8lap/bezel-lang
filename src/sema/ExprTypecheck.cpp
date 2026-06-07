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
    // §2.2 imports (issue #328 inc 3): expose each imported module's exported
    // top-level types by simple name, so a bare identifier naming an imported
    // type (e.g. `Calc` in `Calc.Add(...)`) resolves to the imported TypeSymbol
    // and the static-member-access path can find its method. Mirrors the
    // type-resolution pass's import gating (a local declaration wins). Engaged
    // only when dependencies were loaded — the single-module path is unchanged.
    if (ctx_.hasImports()) {
        for (const auto& imp : src.imports) {
            if (!imp || !imp->name) continue;
            std::string dotted;
            for (std::size_t i = 0; i < imp->name->parts.size(); ++i) {
                if (i != 0) dotted += '.';
                dotted += std::string(imp->name->parts[i]);
            }
            if (const std::vector<TypeSymbol*>* exported =
                    ctx_.importedTypesOf(dotted)) {
                for (TypeSymbol* t : *exported)
                    if (!t->name.empty())
                        moduleScope_->names.emplace(t->name, t);
            }
        }
    }
    for (const auto& decl : src.topDecls) {
        if (!decl) continue;
        visitTopDecl(*decl);
    }
}

// Return (or lazily create) a synthetic TypeSymbol for a built-in class.
// §2.5 / §2.6 / §6.1 / §28 (issue #434): value-type builtins (DateTime,
// TimeSpan, Guid, CancellationToken) carry isValueBuiltin=true so the
// ownership checker treats them as non-owned (no §28.3 disposal obligation).
// All other builtins (CancellationTokenSource, Encoding, …) keep the default
// isValueBuiltin=false and remain owned/reference-typed.
TypeSymbol* ExprTC::builtinClassSym(std::string_view name) {
    auto key = std::string(name);
    auto it = builtinClassCache_.find(key);
    if (it != builtinClassCache_.end()) return it->second;
    auto* ts = ctx_.makeSymbol<TypeSymbol>();
    ts->kind = ast::TypeDeclKind::Class;
    ts->name = name;
    ts->decl = nullptr;
    // per §2.5 / §2.6 / §6.1 / §28 (issue #434): scalar/struct value types
    // have no heap allocation and no disposal obligation.
    if (name == "DateTime" || name == "TimeSpan" ||
        name == "Guid"     || name == "CancellationToken") {
        ts->isValueBuiltin = true;
    }
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
    // per §22.8 / §22.10: a primary-constructor parameter (a FieldSymbol
    // whose `posParam` is non-null AND whose parent type is a CLASS, not a
    // record) is explicitly permitted in field/property initializers. The
    // parameter's value is guaranteed available before field initializers
    // run (§22.8 step 0). A record's positional fields are genuine instance
    // members and remain forbidden. Bug #478.
    if (auto* fs = dynamic_cast<FieldSymbol*>(sym)) {
        if (fs->posParam && mem->parent &&
            mem->parent->kind == ast::TypeDeclKind::Class)
            return;
    }
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

namespace {

// §19.3: an unguarded `_` (DiscardPattern) or unconstrained `var x`
// (BindingPattern with no type annotation) matches any value — a
// catch-all that makes the `case` trivially exhaustive (§19.6).
bool isUnguardedCatchAll(const ast::CaseArm& arm) {
    if (arm.guard) return false;  // a guard can fail at runtime
    const ast::Pattern* p = arm.pattern.get();
    if (dynamic_cast<const ast::DiscardPattern*>(p)) return true;
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(p))
        return bp->type == nullptr;  // `var x` (untyped) is total; `var x: T` is a type test
    return false;
}

// §19.6: collect the last name part of every enum-constant pattern an
// unguarded arm covers. Enum patterns appear as a NamedPattern (`E.Red`
// or bare `Red`) or as a ConstantPattern holding one or more constant
// expressions (each a MemberAccessExpr `E.Red`, an IdentifierExpr `Red`,
// or a QualifiedName-bearing form).
void collectEnumPatternNames(const ast::CaseArm& arm,
                             std::unordered_set<std::string_view>& covered) {
    if (arm.guard) return;  // a guarded arm cannot be relied on for coverage
    const ast::Pattern* p = arm.pattern.get();
    if (auto* np = dynamic_cast<const ast::NamedPattern*>(p)) {
        if (np->name && !np->name->parts.empty())
            covered.insert(np->name->parts.back());
    } else if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p)) {
        for (const auto& c : cp->constants) {
            if (!c) continue;
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(c.get()))
                covered.insert(ma->member);
            else if (auto* id =
                         dynamic_cast<const ast::IdentifierExpr*>(c.get()))
                covered.insert(id->name);
        }
    }
}

// §19.6: does any unguarded arm explicitly match `null` (NullPattern)?
bool armsCoverNull(const ast::CaseExpr& ce) {
    for (const auto& arm : ce.arms) {
        if (arm.guard) continue;
        if (dynamic_cast<const ast::NullPattern*>(arm.pattern.get()))
            return true;
    }
    return false;
}

// §11.7 / §19.6: is `cls` a closed abstract base — one whose complete set
// of leaf types is statically knowable? Two forms qualify:
//   * `sealed abstract`: no subclass outside the declaring module can
//     exist, so the closed set holds across module boundaries (§11.7).
//   * a same-module (non-imported) `abstract` class whose every direct
//     subclass is itself `sealed`/closed (recursively forms a closed set,
//     §19.6). An imported plain-abstract base may have unseen subclasses,
//     so it is treated as open.
bool hasModifier(const TypeSymbol* t, std::string_view m) {
    for (auto mod : t->modifiers) if (mod == m) return true;
    return false;
}

} // namespace

// §19.6: emit a non-exhaustiveness WARNING for an expression-position
// `case` whose arms the compiler cannot prove cover every possible value.
// The spec lists the scrutinee shapes for which exhaustiveness IS provable
// — enum (all constants), bool (both literals), a closed sealed-abstract
// class hierarchy (all leaves), and the nullable variant of those (the
// above plus an explicit `null` arm). An unguarded `_` or unconstrained
// `var x` arm is a catch-all and is always exhaustive. Every other shape
// (an open class hierarchy, int/string/range scrutinees, the scrutinee-
// less boolean-condition form, partial coverage) cannot be proven
// exhaustive, so the warning fires unless a catch-all is present (§19.6).
//
// This is a WARNING, not an error: §19.6 states an unmatched value raises
// MatchFailureException at runtime and "the compiler warns when it cannot
// prove exhaustiveness statically." The diagnostic does not fail
// compilation (DiagnosticEngine::hasErrors counts only errors).
void ExprTC::checkCaseExhaustiveness(const ast::CaseExpr& ce, TypeRef* scrutTy) {
    // An unguarded catch-all (`_` / untyped `var x`) covers everything.
    for (const auto& arm : ce.arms) {
        if (isUnguardedCatchAll(arm)) return;
    }

    auto warn = [&](const std::string& detail) {
        ctx_.diagnostics().report(
            Severity::Warning, ce.span.begin,
            "non-exhaustive `case`: " + detail +
            " (add a `_` arm or cover the remaining value(s)) (§19.6)");
    };

    // A nullable scrutinee additionally requires an explicit `null` arm
    // for exhaustiveness (§19.6: "the rules above plus an explicit `null`
    // pattern"). If the value can be null but no arm matches null, the
    // case is non-exhaustive regardless of the non-null coverage below.
    bool nullableUncovered =
        scrutTy && scrutTy->nullable && !armsCoverNull(ce);

    // --- Bool scrutinee (§19.6): both `true` and `false` must be covered.
    if (scrutTy && !scrutTy->nullable &&
        scrutTy->kind == TypeRef::Kind::Primitive &&
        scrutTy->primitive == PrimitiveType::Bool) {
        bool t = false, f = false;
        for (const auto& arm : ce.arms) {
            if (arm.guard) continue;
            auto scan = [&](const ast::Expr* e) {
                if (auto* b = dynamic_cast<const ast::BoolLiteralExpr*>(e)) {
                    if (b->value) t = true; else f = true;
                }
            };
            if (auto* cp =
                    dynamic_cast<const ast::ConstantPattern*>(arm.pattern.get()))
                for (const auto& c : cp->constants) if (c) scan(c.get());
        }
        if (!(t && f))
            warn("bool scrutinee does not cover both `true` and `false`");
        return;
    }

    // --- Enum / closed-hierarchy scrutinee both resolve to a Named type.
    TypeSymbol* sub = (scrutTy && scrutTy->kind == TypeRef::Kind::Named)
                          ? scrutTy->declaration
                          : nullptr;

    if (sub && sub->kind == ast::TypeDeclKind::Enum) {
        // §19.6: every declared enum constant must have a matching pattern.
        std::unordered_set<std::string_view> covered;
        for (const auto& arm : ce.arms) collectEnumPatternNames(arm, covered);
        std::vector<std::string_view> missing;
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(sub->decl)) {
            for (const auto& ec : td->enumConstants)
                if (!covered.count(ec.name)) missing.push_back(ec.name);
        } else {
            // No AST decl (e.g. imported enum): fall back to symbol members.
            for (const Symbol* m : sub->members)
                if (dynamic_cast<const EnumConstantSymbol*>(m) &&
                    !covered.count(m->name))
                    missing.push_back(m->name);
        }
        if (!missing.empty() || nullableUncovered) {
            std::string detail = "enum `" + std::string(sub->name) + "`";
            if (!missing.empty()) {
                detail += " is missing constant(s):";
                for (auto n : missing) detail += " " + std::string(n);
            }
            if (nullableUncovered)
                detail += (missing.empty() ? " nullable value not covered"
                                           : " (and a `null` arm)");
            warn(detail);
        }
        return;
    }

    if (sub && sub->kind == ast::TypeDeclKind::Class) {
        // §11.7 / §19.6: the scrutinee's hierarchy is closed only when the
        // base is `sealed abstract`, or a same-module abstract whose every
        // subclass is recursively sealed/closed. Otherwise the set of leaf
        // types is open and exhaustiveness cannot be proven.
        bool isAbstract = hasModifier(sub, "abstract");
        bool isSealed = hasModifier(sub, "sealed");

        // Gather every transitive subclass of `sub` visible in this
        // compilation. A leaf to cover is a concrete (non-abstract)
        // subclass; abstract intermediates are matched only through their
        // concrete descendants. `descendants` keeps the full set (including
        // abstract intermediates) so a pattern naming an intermediate by
        // name still resolves (subtype matching, §19.3).
        std::vector<TypeSymbol*> leaves;
        std::vector<TypeSymbol*> descendants;
        bool sawOpenLeaf = false;  // a non-sealed subclass leaves the set open
        for (const auto& s : ctx_.symbols()) {
            auto* ts = dynamic_cast<TypeSymbol*>(s.get());
            if (!ts || ts == sub || ts->kind != ast::TypeDeclKind::Class)
                continue;
            // Is `sub` an ancestor of `ts`?
            bool isDescendant = false;
            std::vector<TypeSymbol*> work = ts->baseTypes;
            std::unordered_set<TypeSymbol*> seen;
            while (!work.empty()) {
                TypeSymbol* b = work.back();
                work.pop_back();
                if (!b || !seen.insert(b).second) continue;
                if (b == sub) { isDescendant = true; break; }
                for (TypeSymbol* bb : b->baseTypes) work.push_back(bb);
            }
            if (!isDescendant) continue;
            descendants.push_back(ts);
            if (!hasModifier(ts, "abstract"))
                leaves.push_back(ts);  // concrete leaf to be covered
            if (!hasModifier(ts, "sealed") && !hasModifier(ts, "abstract"))
                sawOpenLeaf = true;    // a non-sealed concrete subclass
        }

        // The hierarchy is provably closed when the base is `sealed
        // abstract` (cross-module guarantee, §11.7), OR a same-module
        // abstract base with no non-sealed concrete leaf (recursively
        // closed, §19.6). A non-imported plain-abstract base whose leaves
        // are all sealed still forms a closed set within the module.
        bool closed =
            (isAbstract && isSealed) ||
            (isAbstract && !ctx_.isImported(sub) && !sawOpenLeaf);
        if (!closed) {
            warn("class hierarchy rooted at `" + std::string(sub->name) +
                 "` is open");
            return;
        }

        // Collect the leaf types covered by an unguarded type-test arm.
        std::unordered_set<const TypeSymbol*> coveredLeaves;
        for (const auto& arm : ce.arms) {
            if (arm.guard) continue;
            const ast::Pattern* p = arm.pattern.get();
            TypeSymbol* matchTy = nullptr;
            // A type-test arm names the matched class either as a bare
            // NamedPattern (`Circle => …`) or as a typed binding
            // (`var c: Circle => …`). Resolve the named class by its last
            // name part against the closed hierarchy's leaf set — the
            // BindingPattern's `bp->type` annotation is NOT resolved by
            // TypeResolution inside case arms (no `resolvedType` entry), so
            // we match by name, consistent with the NamedPattern path.
            std::string_view typeName;
            if (auto* np = dynamic_cast<const ast::NamedPattern*>(p)) {
                if (np->name && !np->name->parts.empty())
                    typeName = np->name->parts.back();
            } else if (auto* bp =
                           dynamic_cast<const ast::BindingPattern*>(p)) {
                if (bp->type && bp->type->name &&
                    !bp->type->name->parts.empty())
                    typeName = bp->type->name->parts.back();
            }
            // Resolve against the full descendant set (a leaf, or an
            // abstract intermediate that subtype-covers its own leaves), or
            // against `sub` itself (`var x: Shape` covers everything).
            if (!typeName.empty()) {
                if (typeName == sub->name) matchTy = sub;
                for (auto* d : descendants)
                    if (d->name == typeName) matchTy = d;
            }
            if (matchTy) {
                // A pattern naming an ancestor also covers all its leaf
                // descendants (subtype matching, §19.3).
                for (auto* lf : leaves) {
                    if (lf == matchTy) { coveredLeaves.insert(lf); continue; }
                    std::vector<TypeSymbol*> work = lf->baseTypes;
                    std::unordered_set<TypeSymbol*> seen;
                    while (!work.empty()) {
                        TypeSymbol* b = work.back();
                        work.pop_back();
                        if (!b || !seen.insert(b).second) continue;
                        if (b == matchTy) { coveredLeaves.insert(lf); break; }
                        for (TypeSymbol* bb : b->baseTypes)
                            work.push_back(bb);
                    }
                }
            }
        }
        std::vector<std::string_view> missing;
        for (auto* lf : leaves)
            if (!coveredLeaves.count(lf)) missing.push_back(lf->name);
        if (!missing.empty() || nullableUncovered) {
            std::string detail =
                "closed hierarchy `" + std::string(sub->name) + "`";
            if (!missing.empty()) {
                detail += " is missing leaf type(s):";
                for (auto n : missing) detail += " " + std::string(n);
            }
            if (nullableUncovered)
                detail += (missing.empty() ? " nullable value not covered"
                                           : " (and a `null` arm)");
            warn(detail);
        }
        return;
    }

    // --- The scrutinee-less boolean-condition form (§19.1) has no value
    // set to enumerate and no `_` (we already returned on a catch-all), so
    // it cannot be proven exhaustive (§19.6).
    if (!ce.scrutinee) {
        warn("scrutinee-less `case` has no catch-all `_` arm");
        return;
    }

    // A scrutinee whose type failed to resolve (Error sentinel) or was not
    // inferred is suppressed here: a non-exhaustiveness warning layered on
    // top of an upstream type error would be a confusing cascade (the Error
    // kind exists precisely to suppress such follow-on diagnostics —
    // TypeRef.hpp). The real diagnostic is reported by the resolution pass.
    if (!scrutTy || scrutTy->kind == TypeRef::Kind::Error) return;

    // --- Every other scrutinee shape (int / string / range / float /
    // open class) cannot be proven exhaustive (§19.6): there is no finite,
    // statically-knowable value set to enumerate. Without a catch-all arm
    // (handled above) the case is non-exhaustive.
    warn("scrutinee type cannot be proven exhaustive");
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

// async-readiness 3b (§30.1): map an `async` method's declared return type to
// the return-FORM context and the result-assignability type used inside its
// body. The task wrapper is peeled: `Task` is a void-method context (no
// result), `Task<T>` / `OwnedTask<T>` are value-method contexts whose result
// type is the element T. Anything else (no return type, a non-async type, or
// an async-iterator interface) is left as a void context — the dedicated async
// return-type diagnostic (§30.1, in TypeResolution) already names a non-async
// return type, and an async-iterator method is exempt from the value-return
// check via the `isIteratorMethod` path.
void ExprTC::asyncReturnCtxAndType(const ast::Type* rt, ReturnCtx& outCtx,
                                   TypeRef*& outType) {
    outCtx = ReturnCtx::VoidMethod;
    outType = nullptr;
    if (!rt) return;
    TypeRef* r = ctx_.resolvedType(*rt);
    if (!r || r->kind != TypeRef::Kind::Named || !r->astNode ||
        !r->astNode->name || r->astNode->name->parts.size() != 1) {
        return;
    }
    std::string_view head = r->astNode->name->parts.front();
    if (head == "Task" && r->genericArgs.empty()) {
        // `Task` — no result value; void-method form rules.
        return;
    }
    if ((head == "Task" || head == "OwnedTask") && !r->genericArgs.empty()) {
        // `Task<T>` / `OwnedTask<T>` — value-method form rules; the body
        // returns the element type T.
        outCtx = ReturnCtx::ValueMethod;
        outType = r->genericArgs.front();
    }
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
    // §27.6 / §5.1 (issue #436): a bare method reference has a Function type.
    // A Function value is not implicitly convertible to a non-Function type
    // (Primitive or Named). Report a type mismatch so the user sees a clean
    // §5.1 error instead of producing malformed IR.  Function-to-Function
    // compatibility (e.g. `var f: () -> int := obj.Method`) is left to a
    // future function-type signature-matching slice; for now it passes silently
    // (the existing codegen path handles it correctly).
    if (from->kind == TypeRef::Kind::Function &&
        to->kind != TypeRef::Kind::Function) {
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

// §20: type the `if` expression with the COMMON type of its branches.
// The common type is the one branch's type that the other widens (§5.1)
// or subtypes (§12) into; a bottom-typed (`throw`/diverging) branch — its
// type left null — imposes no constraint and the result is the other
// branch's type. When both branches are typed but neither converts to the
// other, no common type exists and a clean §20 error is reported. The
// reporting is conservative (mirroring checkAssignable) so the pass never
// false-positives while the type lattice is incomplete.
//
// This is also what guards the `throw E if c else x` shape — which parses
// as `throw (E if c else x)` per the grammar (ThrowExpr's operand is a
// full Expression; §17.12, Appendix A) — where the conditional's arms are
// an exception value (Named) and a plain value (e.g. a primitive). They
// have no common type, so codegen never reaches the ill-typed PHI that
// crashed it; sema rejects the program first.
// §20: the pairwise common type of two typed branches/arms — the one the
// other widens (§5.1) or subtypes (§12) into. Returns null when neither
// converts to the other (no common type exists). Both inputs must be
// non-null (callers handle the bottom-typed `null` case before getting here:
// a bottom-typed arm imposes no constraint). An Error-kind input yields the
// other operand so a prior diagnostic doesn't cascade. `Object` is the
// universal common supertype. Conservative (mirrors checkAssignable /
// convertsInto) so it never false-positives while the type lattice is
// incomplete.
TypeRef* ExprTC::commonTypeOf(TypeRef* a, TypeRef* b) {
    if (!a) return b;
    if (!b) return a;
    if (a->kind == TypeRef::Kind::Error) return b;
    if (b->kind == TypeRef::Kind::Error) return a;
    auto isObject = [](TypeRef* t) {
        return t->kind == TypeRef::Kind::Primitive &&
               t->primitive == PrimitiveType::Object;
    };
    if (isObject(a)) return a;
    if (isObject(b)) return b;
    // `from` widens/subtypes into `to` — so `to` is a candidate common type.
    auto convertsInto = [&](TypeRef* from, TypeRef* to) -> bool {
        if (from->kind != to->kind) return false;
        if (from->kind == TypeRef::Kind::Primitive) {
            if (from->primitive == to->primitive) return true;
            return primitiveWidens(from->primitive, to->primitive);
        }
        if (from->kind == TypeRef::Kind::Named &&
            from->declaration && to->declaration) {
            // Defer generics to the variance slice (conservative skip).
            if (!from->genericArgs.empty() || !to->genericArgs.empty())
                return true;
            if (from->declaration == to->declaration) return true;
            return isSubtype(from->declaration, to->declaration);
        }
        // Other kinds (InlineArray/Function/Unit): conservatively treat
        // same-kind as compatible to avoid false positives.
        return true;
    };
    // The common type is whichever operand the other converts into. Prefer
    // `a` on a tie (equal types / mutual conversion).
    if (convertsInto(b, a)) return a;
    if (convertsInto(a, b)) return b;
    return nullptr;  // no common type
}

void ExprTC::typeConditional(const ast::ConditionalExpr& cnd,
                             const ast::Expr& e) {
    TypeRef* tt = cnd.thenValue ? ctx_.exprType(*cnd.thenValue) : nullptr;
    TypeRef* et = cnd.elseValue ? ctx_.exprType(*cnd.elseValue) : nullptr;
    // A bottom-typed branch leaves its type null: the result is the other
    // branch's type, with no compatibility constraint. This is the valid
    // `x if c else throw E` idiom (and its mirror).
    if (!tt || !et) {
        if (tt) ctx_.setExprType(&e, tt);
        else if (et) ctx_.setExprType(&e, et);
        return;
    }
    // Both branches typed: take the §20 common type (the one the other
    // widens/subtypes into). commonTypeOf handles Error-kind and Object.
    if (TypeRef* common = commonTypeOf(tt, et)) {
        ctx_.setExprType(&e, common);
        return;
    }
    // No common type: report and adopt the then-branch type so member
    // access on the (already-erroneous) result still resolves downstream.
    ctx_.setExprType(&e, tt);
    ctx_.diagnostics().report(
        Severity::Error, cnd.span.begin,
        "the two branches of an `if` expression have no common type: "
        "neither branch's type is implicitly convertible to the other "
        "(§20)");
}

// §19 + §20: type a `case` EXPRESSION with the COMMON type of its
// (non-diverging) arm bodies. A diverging arm — a `throw` (§17.12) or
// other bottom-typed body — leaves its body's exprType null and imposes
// no type constraint, exactly like the bottom-typed branch of an `if`
// expression. The result is the running common type of the value-producing
// arms (each folded through commonTypeOf, identical to typeConditional's
// pairwise rule). When two value arms have no common type, a clean §20
// error is reported here rather than letting codegen build an ill-typed
// result PHI and crash. When EVERY arm diverges, the case yields no value
// and no type is recorded.
void ExprTC::typeCase(const ast::CaseExpr& cse, const ast::Expr& e) {
    TypeRef* common = nullptr;     // running common type of value arms
    bool haveValueArm = false;     // at least one non-diverging arm seen
    bool reported = false;         // emit the §20 error at most once
    for (const auto& arm : cse.arms) {
        // Only expression-bodied arms produce a value (statement / `do…end`
        // arms are statement-position only, §19.2). A bottom-typed arm body
        // (e.g. `throw E`) has a null exprType and imposes no constraint.
        if (!arm.exprBody) continue;
        TypeRef* at = ctx_.exprType(*arm.exprBody);
        if (!at) continue;  // diverging (bottom) arm — no constraint
        if (!haveValueArm) {
            common = at;
            haveValueArm = true;
            continue;
        }
        TypeRef* merged = commonTypeOf(common, at);
        if (!merged) {
            // No common type between this arm and the arms so far. Report
            // once and keep the running type so later arms still merge.
            if (!reported) {
                ctx_.diagnostics().report(
                    Severity::Error, cse.span.begin,
                    "the arms of a `case` expression have no common type: "
                    "an arm's type is not implicitly convertible to that of "
                    "the other arms (§20)");
                reported = true;
            }
            continue;  // leave `common` unchanged
        }
        common = merged;
    }
    // Record the common type (when there was a value-producing arm) so
    // member access on the case result (`(case … end).M`) resolves. When
    // every arm diverges, no type is recorded — the case produces no value.
    if (haveValueArm && common) ctx_.setExprType(&e, common);
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

// §26.5: does an extension declared on `target` apply to a receiver of type
// `recv`? This slice matches EXACTLY (the receiver's own compile-time type),
// ignoring nullability on both sides (§26.2 forbids `extension int?`, so an
// extension on `int` applies to an `int`/`int?` receiver alike). Base-class,
// interface, and generic-extension matching (§26.5 step 2/3) are deferred
// (#433): a non-empty genericArgs list on either side disqualifies the match
// here so an `extension Box<int>` is never mis-applied to a `Box<string>`.
static bool extensionTargetMatchesReceiver(const TypeRef* target,
                                           const TypeRef* recv) {
    if (!target || !recv) return false;
    if (target->kind != recv->kind) return false;
    switch (target->kind) {
        case TypeRef::Kind::Primitive:
            return target->primitive == recv->primitive;
        case TypeRef::Kind::Named:
            // Exact user type: same declaration, no generic args on either
            // side (generic extensions deferred).
            if (target->declaration == nullptr ||
                recv->declaration == nullptr)
                return false;
            if (target->declaration != recv->declaration) return false;
            if (!target->genericArgs.empty() || !recv->genericArgs.empty())
                return false;
            return true;
        default:
            return false;  // function/array/unit types are not extensible
    }
}

Symbol* ExprTC::lookupExtensionMember(const TypeRef* receiver,
                                      std::string_view name) {
    if (!receiver || !ctx_.module()) return nullptr;
    for (Symbol* child : ctx_.module()->children) {
        auto* ext = dynamic_cast<ExtensionSymbol*>(child);
        if (!ext) continue;
        auto* decl = dynamic_cast<const ast::ExtensionDecl*>(ext->decl);
        if (!decl || !decl->targetType) continue;
        const TypeRef* target = ctx_.resolvedType(*decl->targetType);
        if (!extensionTargetMatchesReceiver(target, receiver)) continue;
        for (Symbol* m : ext->members) {
            if (m && m->name == name) return m;
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

// §12.7: a single `<simple var> is T` / `is not T` type-test contributes a
// narrowing to one branch. `x is T` narrows the true branch; `x is not T`
// narrows the else branch (the spec: in the true branch of `is not T` the
// value could be many non-T types, so no narrowing). Returns true iff an entry
// was written into narrowings_.
bool ExprTC::narrowFromTypeTest(const ast::Expr* cond, bool forTrueBranch) {
    auto* te = dynamic_cast<const ast::TypeTestExpr*>(cond);
    if (!te) return false;
    // Only the type-class tests `is` / `is not` narrow here. (The
    // nullability tests `is null` / `is not null` change nullability, not the
    // type-class declaration; they are deferred — see collectNarrowings.)
    bool narrowsThis;
    if (te->op == "is") narrowsThis = forTrueBranch;       // true branch
    else if (te->op == "is not") narrowsThis = !forTrueBranch;  // else branch
    else return false;                                     // `as` etc.
    if (!narrowsThis) return false;
    // The tested expression must be a SIMPLE VARIABLE (§12.7). We narrow
    // locals and parameters — the unambiguous "simple variable" case. (A bare
    // field is a `this.f` access whose narrowing would need cross-object
    // invalidation tracking we don't have; deferred — see collectNarrowings.)
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(te->operand.get());
    if (!id || id->name == "this" || id->name == "base") return false;
    Symbol* sym = lookupName(id->name);
    if (!sym) return false;
    if (!dynamic_cast<LocalSymbol*>(sym) &&
        !dynamic_cast<ParameterSymbol*>(sym))
        return false;
    // The narrowed type is the test type. It must resolve to a Named type with
    // a declaration so member lookup can run against it. The narrowed form is
    // non-nullable (a successful `is T` means the value is a non-null T).
    if (!te->testType) return false;
    TypeRef* tt = ctx_.resolvedType(*te->testType);
    if (!tt || tt->kind != TypeRef::Kind::Named || !tt->declaration)
        return false;
    TypeRef* narrowed = tt;
    if (tt->nullable) {
        narrowed = ctx_.makeTypeRef(*tt);
        narrowed->nullable = false;
    }
    narrowings_[sym] = narrowed;
    return true;
}

// The resolved declared TypeRef of a local or parameter, or null for any other
// symbol kind (issue #418). Locals carry an already-resolved declType; params
// carry an ast::Type that must be looked up (or a context-supplied
// resolvedDeclType for inferred lambda params).
TypeRef* ExprTC::declaredTypeOf(Symbol* sym) {
    if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
        return loc->declType;
    }
    if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
        if (par->declType) return ctx_.resolvedType(*par->declType);
        return par->resolvedDeclType;
    }
    return nullptr;
}

// §7.4 (issue #418): a simple variable is known non-null when null-narrowing
// has proved it so (`nullNarrowed_`), when a §12.7 type-narrowing is in effect
// (those always narrow to a non-nullable form), or when its declared type is
// itself non-nullable.
bool ExprTC::isKnownNonNull(Symbol* sym) {
    if (!sym) return false;
    if (nullNarrowed_.count(sym)) return true;
    if (auto it = narrowings_.find(sym); it != narrowings_.end())
        return it->second && !it->second->nullable;
    TypeRef* dt = declaredTypeOf(sym);
    return dt && !dt->nullable;
}

// §7.4 (issue #418): refresh `nullNarrowed_` for `sym := value`. A non-null
// value proves the variable non-null from here; a nullable value (including a
// bare `null` literal, typed nullable) clears any prior knowledge.
void ExprTC::narrowOnAssignment(Symbol* sym, const TypeRef* valueType) {
    if (!sym) return;
    if (!dynamic_cast<LocalSymbol*>(sym) &&
        !dynamic_cast<ParameterSymbol*>(sym))
        return;
    if (valueType && !valueType->nullable)
        nullNarrowed_.insert(sym);
    else
        nullNarrowed_.erase(sym);
}

// §7.4 / §8.4 (issue #418): a `<simple var> <> null` / `= null` test proves a
// nullable simple variable non-null in one branch by recording it in
// `nullNarrowed_`. The parser desugars `x is not null` to `x <> null` and
// `x is null` to `x = null` (Parser_Exprs §17.4), so handling the two binary
// operators covers all four spec spellings. `<> null` narrows the TRUE branch;
// `= null` narrows the ELSE branch (where the value is known non-null). Returns
// true iff a narrowing was recorded. Unlike §12.7 type-narrowing this does NOT
// rewrite the variable's exprType (codegen relies on the declared nullable type
// to locate the tagged-optional payload).
bool ExprTC::narrowFromNullTest(const ast::Expr* cond, bool forTrueBranch) {
    auto* be = dynamic_cast<const ast::BinaryExpr*>(cond);
    if (!be) return false;
    bool narrowsThis;
    if (be->op == "<>") narrowsThis = forTrueBranch;       // true branch
    else if (be->op == "=") narrowsThis = !forTrueBranch;  // else branch
    else return false;
    if (!narrowsThis) return false;
    // One operand must be the null literal and the other a simple variable
    // (§7.4: narrowing applies to local variables only, never fields). Accept
    // `x <> null` and `null <> x`.
    const ast::Expr* varSide = nullptr;
    if (dynamic_cast<const ast::NullLiteralExpr*>(be->right.get()))
        varSide = be->left.get();
    else if (dynamic_cast<const ast::NullLiteralExpr*>(be->left.get()))
        varSide = be->right.get();
    if (!varSide) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(varSide);
    if (!id || id->name == "this" || id->name == "base") return false;
    Symbol* sym = lookupName(id->name);
    if (!sym) return false;
    if (!dynamic_cast<LocalSymbol*>(sym) &&
        !dynamic_cast<ParameterSymbol*>(sym))
        return false;
    nullNarrowed_.insert(sym);
    return true;
}

// §12.7: collect every flow-narrowing an `if`/`elif` condition establishes for
// one branch into narrowings_. A single type test narrows per
// narrowFromTypeTest; a single null test narrows per narrowFromNullTest (§7.4 /
// §8.4); `and`-joined tests (`x is T and y is S`, `a <> null and b <> null`)
// contribute all their narrowings to the combined true branch. The caller has
// already visited the condition (so operand symbols and test types are
// resolved) and is responsible for save/restore of narrowings_ around the
// branch body.
void ExprTC::collectNarrowings(const ast::Expr* cond, bool forTrueBranch) {
    if (!cond) return;
    if (narrowFromTypeTest(cond, forTrueBranch)) return;
    if (narrowFromNullTest(cond, forTrueBranch)) return;
    // `and`-joined tests: both sides narrow throughout the combined true
    // branch (§12.7). This does not extend to `or` (where neither side is
    // guaranteed) nor to the else branch of an `and` (De Morgan would make it
    // an `or` of negations — no single narrowing holds); those are deferred.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(cond)) {
        if (be->op == "and" && forTrueBranch) {
            collectNarrowings(be->left.get(), /*forTrueBranch=*/true);
            collectNarrowings(be->right.get(), /*forTrueBranch=*/true);
        }
    }
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
        // §26.1: inside an extension block, `this` is the receiver. For an
        // extension on a user class/record/enum, point currentType_ at the
        // target's TypeSymbol so the body's `this.member` and bare-member
        // references resolve against the extended type (exactly as a real
        // method body does). For a primitive/builtin extension the target has
        // no user TypeSymbol; currentType_ stays null (a primitive has no
        // member set — `this` is used as a value, e.g. `this * 2`).
        TypeSymbol* targetSym = nullptr;
        if (ext->targetType) {
            if (auto* rt = ctx_.resolvedType(*ext->targetType))
                if (rt->kind == TypeRef::Kind::Named)
                    targetSym = rt->declaration;
        }
        // Find the ExtensionSymbol for this decl so paramSymbolsOf can locate
        // the extension methods' parameters (they live on the ExtensionSymbol,
        // not on the extended type).
        ExtensionSymbol* extSym = nullptr;
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (auto* es = dynamic_cast<ExtensionSymbol*>(child)) {
                    if (es->decl == ext) { extSym = es; break; }
                }
            }
        }
        TypeSymbol* save = currentType_;
        ExtensionSymbol* saveExt = currentExtension_;
        currentType_ = targetSym;
        currentExtension_ = extSym;
        for (const auto& m : ext->members) if (m) visitMember(*m);
        currentType_ = save;
        currentExtension_ = saveExt;
    }
}

// Param-symbol lookup for the in-progress method/ctor on currentType_.
const std::vector<Symbol*>* ExprTC::paramSymbolsOf(const ast::Member& m) const {
    // §26.1: inside an extension block, the method's params live on the
    // ExtensionSymbol, not on currentType_ (the extended type). Search there
    // first when type-checking an extension body.
    if (currentExtension_) {
        for (Symbol* child : currentExtension_->members) {
            if (!child || child->decl != &m) continue;
            if (auto* ms = dynamic_cast<MethodSymbol*>(child))
                return &ms->params;
        }
    }
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
    // §12.7: flow narrowings are method-local — start each body with none.
    // (The if-statement walk save/restores the map per controlled block, so
    // this is normally already empty; clearing makes the boundary explicit and
    // robust to symbol reuse across methods.)
    narrowings_.clear();
    // §7.4 (issue #418): nullability narrowing is likewise method-local.
    nullNarrowed_.clear();
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
        // issue #484 / §14.2: expose the in-progress MethodDecl so that
        // resolveMemberAccess can walk its generic constraints when the
        // receiver is a type parameter (e.g. `x: T` where `where T: ISome`).
        const ast::MethodDecl* saveMD = currentMethodDecl_;
        currentMethodDecl_ = md;
        ReturnCtx saveRet = returnCtx_;
        TypeRef* saveRetTy = currentReturnType_;
        // async-readiness 3b (§30.2): an `async` method body is an async
        // context, where `await` is valid.
        bool saveAsync = inAsyncContext_;
        inAsyncContext_ = false;
        for (auto mod : md->modifiers) {
            if (mod == "async") { inAsyncContext_ = true; break; }
        }
        // async-readiness 3b (§30.1): an async method's return-FORM rules and
        // the type a `return EXPR;` is checked against are driven by the
        // task's *result* type, not the task wrapper itself:
        //   async ...: Task          -> behaves like a void method — bare
        //                               `return;` is allowed, and falling off
        //                               the end is `return;`; `return EXPR;`
        //                               is rejected (the task has no result).
        //   async ...: Task<T> /
        //              OwnedTask<T>  -> behaves like a value method — must
        //                               `return EXPR;` on every path, with
        //                               EXPR assignable to the element type T.
        //   async ...: IAsyncEnumerable<T> / ... (async iterator) -> an
        //                               iterator method (it uses `yield`);
        //                               exempt from the value-return check
        //                               via `isIteratorMethod` below, exactly
        //                               like a synchronous iterator (§29.3).
        if (inAsyncContext_) {
            asyncReturnCtxAndType(md->returnType.get(), returnCtx_,
                                  currentReturnType_);
        } else {
            returnCtx_ = returnsValue(md->returnType.get())
                ? ReturnCtx::ValueMethod : ReturnCtx::VoidMethod;
            currentReturnType_ = md->returnType
                ? ctx_.resolvedType(*md->returnType) : nullptr;
        }
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
        inAsyncContext_ = saveAsync;
        inStaticMember_ = save;
        currentMethodDecl_ = saveMD;  // issue #484 / §14.2
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
                // per §14.1/§14.2 + §A.7 (issue #391): return-target generic
                // type-argument inference. When the initializer is a call to a
                // generic method whose type parameter appears only in the
                // return type (`Create<T>(): T`), the typed local declaration
                // supplies the target that solves T. This re-types the call to
                // the substituted return type before the assignability check.
                if (auto* initCall = dynamic_cast<const ast::CallExpr*>(
                        ld->initializer.get())) {
                    if (lsym->declType)
                        inferReturnTargetTypeArgs(*initCall, lsym->declType,
                                                  /*outNeedsInfer=*/nullptr);
                }
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
                // per §15.4: a collection literal has no standalone type; a
                // `var x := [...]` with no type annotation gives no target type
                // and is a compile error. Must be checked BEFORE visitExpr so
                // we diagnose on the LocalDecl's position (and return early to
                // avoid cascaded inference errors).
                if (dynamic_cast<const ast::CollectionLiteralExpr*>(
                        ld->initializer.get())) {
                    ctx_.diagnostics().report(
                        Severity::Error, ld->initializer->span.begin,
                        "collection literal has no determinable target type — "
                        "add a type annotation (`var " + std::string(ld->name) +
                        ": <CollectionType> := [...]`) (§15.4)");
                    // Still visit sub-expressions so inner errors are found.
                    visitExpr(*ld->initializer);
                    currentScope_->names[ld->name] = lsym;
                    return;  // early-return: lsym->declType intentionally null
                }
                lsym->declType = ctx_.exprType(*ld->initializer);
                // per §14.1 + §A.7 (issue #391): a `var x := Generic.Create();`
                // without a type annotation gives the call no target, so a
                // return-only type parameter cannot be solved. Per §15.4 /
                // §17.7 the spec withholds target inference from `var` without
                // annotation; diagnose rather than silently leaving T unsolved.
                if (auto* initCall = dynamic_cast<const ast::CallExpr*>(
                        ld->initializer.get())) {
                    reportNoTargetInferenceIfNeeded(*initCall);
                }
            }
            currentScope_->names[ld->name] = lsym;
            // §7.4 (issue #418): a nullable-declared local initialized with a
            // non-null value is known non-null until reassigned (parallel to
            // the assignment-narrowing in AssignmentStmt above).
            if (ld->initializer && lsym->declType && lsym->declType->nullable) {
                if (TypeRef* initTy = ctx_.exprType(*ld->initializer))
                    if (!initTy->nullable) nullNarrowed_.insert(lsym);
            }
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
        // per §31.2: `return` inside a `finally` block is a compile error.
        // Appendix A: "`return` does not appear in `finally` blocks."
        if (finallyDepth_ > 0) {
            ctx_.diagnostics().report(
                Severity::Error, rs->span.begin,
                "`return` inside a `finally` block is a compile error (§31.2)");
            return;
        }
        checkReturnForm(*rs);
        // per §14.1/§14.2 + §A.7 (issue #391): return-target generic
        // type-argument inference. `return Generic.Create();` in a method with
        // a declared return type supplies that return type as the target that
        // solves a return-only type parameter (§15.4 / §17.7 recognize the
        // enclosing return type as a target when the method has one).
        if (rs->value && returnCtx_ == ReturnCtx::ValueMethod &&
            currentReturnType_) {
            if (auto* retCall = dynamic_cast<const ast::CallExpr*>(
                    rs->value.get())) {
                inferReturnTargetTypeArgs(*retCall, currentReturnType_,
                                          /*outNeedsInfer=*/nullptr);
            }
        }
        // Slice T3: a returned value must be primitive-assignable to
        // the callable's return type.
        if (rs->value && returnCtx_ == ReturnCtx::ValueMethod) {
            checkAssignable(
                ctx_.exprType(*rs->value), currentReturnType_,
                rs->value->span.begin, "returned value");
        }
    } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) visitExpr(*es->expr);
        // per §14.1 + §A.7 (issue #391): a bare expression statement discards
        // the result, so a return-only type parameter has no target. Diagnose.
        if (es->expr) {
            if (auto* exCall =
                    dynamic_cast<const ast::CallExpr*>(es->expr.get()))
                reportNoTargetInferenceIfNeeded(*exCall);
        }
    } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        // §12.7: "Reassignment within the controlled block invalidates the
        // narrowing." If the assignment target is the narrowed variable, drop
        // its narrowing from this point so the rest of the block re-widens it to
        // its declared type (this also makes the assignability check below use
        // the declared target type, not the narrowed one). The map is restored
        // when the block ends. Only a bare-identifier target can match a
        // narrowed simple variable.
        if (as->target) {
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (tid->name != "this" && tid->name != "base") {
                    if (Symbol* tsym = lookupName(tid->name))
                        narrowings_.erase(tsym);
                }
            }
        }
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
        // §7.4 (issue #418): a plain `:=` of a non-null value to a simple
        // variable proves it non-null from here; assigning a (possibly-)null
        // value clears that knowledge. (Compound ops keep the existing value's
        // nullability, so they don't change the analysis.)
        if (as->op == ":=" && as->target && as->value) {
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (tid->name != "this" && tid->name != "base") {
                    if (Symbol* tsym = lookupName(tid->name))
                        narrowOnAssignment(tsym, ctx_.exprType(*as->value));
                }
            }
        }
    } else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        // per §15.4: `_ := [...]` has no target type — a compile error.
        // The discard position never provides a target type for a collection
        // literal, since there is no type annotation or parameter context.
        if (ds->value) {
            if (dynamic_cast<const ast::CollectionLiteralExpr*>(
                    ds->value.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, ds->value->span.begin,
                    "collection literal has no determinable target type in "
                    "discard position — add a type annotation (§15.4)");
                visitExpr(*ds->value);
                return;
            }
        }
        if (ds->value) visitExpr(*ds->value);
        // per §14.1 + §A.7 (issue #391): `_ := Generic.Create();` discards the
        // result, so a return-only type parameter has no target. Diagnose.
        if (ds->value) {
            if (auto* dCall =
                    dynamic_cast<const ast::CallExpr*>(ds->value.get()))
                reportNoTargetInferenceIfNeeded(*dCall);
        }
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
        // §12.7 flow narrowing (type) and §7.4 / §8.4 nullability narrowing.
        // Each branch body is walked with the narrowings its guards establish,
        // restored afterward so they never leak past the controlled block. The
        // `if`/`elif` bodies see their own condition's true-branch narrowings;
        // the `else` body sees the else-branch narrowings of every preceding
        // condition (each one was false). Both narrowing maps are saved/restored
        // around each body for scoping (and a reassignment inside a body updates
        // the affected entry — see AssignmentStmt above).
        //
        // §7.4: nullability narrowing additionally PERSISTS past the if when it
        // holds on every path that reaches the following statement: a branch
        // that always exits (early `return`/`throw`) contributes no fall-through
        // state, so e.g. `if x = null then return; end;` leaves x non-null
        // afterward, and an if whose branches all establish non-null on their
        // fall-through paths joins to non-null. The after-if set is the
        // intersection of the surviving paths' exit sets (§7.4 "Narrowing also
        // applies after early return").
        auto savedNarrowings = narrowings_;
        auto entryNullNarrowed = nullNarrowed_;
        // Collected exit sets of each fall-through path; null pointer means "no
        // path here" (always-exits). We intersect the non-null ones.
        std::vector<std::unordered_set<Symbol*>> fallThroughExits;
        if (ifs->ifBranch.condition) {
            visitExpr(*ifs->ifBranch.condition);
            checkConditionIsBool(*ifs->ifBranch.condition, "if");
        }
        nullNarrowed_ = entryNullNarrowed;
        collectNarrowings(ifs->ifBranch.condition.get(), /*forTrueBranch=*/true);
        for (const auto& bs : ifs->ifBranch.body) {
            if (bs) visitStmt(*bs);
        }
        if (!bodyAlwaysExits(ifs->ifBranch.body))
            fallThroughExits.push_back(nullNarrowed_);
        narrowings_ = savedNarrowings;
        for (std::size_t i = 0; i < ifs->elifBranches.size(); ++i) {
            const auto& el = ifs->elifBranches[i];
            if (el.condition) {
                visitExpr(*el.condition);
                checkConditionIsBool(*el.condition, "elif");
            }
            // This elif is reached only when every preceding condition was
            // false (their else-narrowings hold) and this condition is true.
            nullNarrowed_ = entryNullNarrowed;
            collectNarrowings(ifs->ifBranch.condition.get(),
                              /*forTrueBranch=*/false);
            for (std::size_t j = 0; j < i; ++j)
                collectNarrowings(ifs->elifBranches[j].condition.get(),
                                  /*forTrueBranch=*/false);
            collectNarrowings(el.condition.get(), /*forTrueBranch=*/true);
            for (const auto& bs : el.body) if (bs) visitStmt(*bs);
            if (!bodyAlwaysExits(el.body))
                fallThroughExits.push_back(nullNarrowed_);
            narrowings_ = savedNarrowings;
        }
        // The else body (explicit or implicit fall-through): all preceding
        // guards were false, so each contributes its else-branch narrowing.
        nullNarrowed_ = entryNullNarrowed;
        collectNarrowings(ifs->ifBranch.condition.get(), /*forTrueBranch=*/false);
        for (const auto& el : ifs->elifBranches)
            collectNarrowings(el.condition.get(), /*forTrueBranch=*/false);
        for (const auto& bs : ifs->elseBody) if (bs) visitStmt(*bs);
        // The else path falls through unless an explicit else body always exits.
        if (!ifs->hasElse || !bodyAlwaysExits(ifs->elseBody))
            fallThroughExits.push_back(nullNarrowed_);
        narrowings_ = savedNarrowings;
        // After-if nullability = intersection of all fall-through exit sets.
        if (fallThroughExits.empty()) {
            // Every path exits the method; the after-if point is unreachable.
            // Keep the entry set (harmless — nothing follows).
            nullNarrowed_ = entryNullNarrowed;
        } else {
            std::unordered_set<Symbol*> joined = fallThroughExits.front();
            for (std::size_t i = 1; i < fallThroughExits.size(); ++i) {
                std::unordered_set<Symbol*> next;
                for (Symbol* sym : joined)
                    if (fallThroughExits[i].count(sym)) next.insert(sym);
                joined = std::move(next);
            }
            nullNarrowed_ = std::move(joined);
        }
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
        // per §31.2: `return` inside a `finally` block is a compile error.
        // Track finally-body nesting so the ReturnStmt handler can reject it.
        ++finallyDepth_;
        for (const auto& bs : trys->finallyBody) if (bs) visitStmt(*bs);
        --finallyDepth_;
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
                // §19.3: "Destructuring applies to records only — classes do
                // not have a positional-destructuring surface … use a
                // type-test pattern (`Dog`, `var d: Dog`) instead."
                // Emit a compile error when the named type is a class.
                if (ctx_.module()) {
                    for (Symbol* child : ctx_.module()->children) {
                        if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                            if (ts->name == lastName &&
                                ts->kind == ast::TypeDeclKind::Class) {
                                ctx_.diagnostics().report(
                                    Severity::Error, dp->span.begin,
                                    "positional destructuring pattern `" +
                                    std::string(lastName) +
                                    "(...)` is not valid on a class type — "
                                    "classes do not have a positional-"
                                    "destructuring surface (§19.3); use a "
                                    "type-test pattern (`" +
                                    std::string(lastName) +
                                    "` or `var x: " +
                                    std::string(lastName) +
                                    "`) instead");
                                break;
                            }
                        }
                    }
                }
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
