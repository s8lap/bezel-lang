#include "sema/TypeResolution_impl.hpp"

#include "sema/TypeResolution.hpp"

namespace bezel::sema {

// Free-function wrapper exposing the resolver's structural type equality
// for reuse by other passes (§22.6 overload resolution). Single source of
// truth: delegates to Resolver::typeRefEqual.
bool typeRefEqual(const TypeRef* a, const TypeRef* b) {
    return Resolver::typeRefEqual(a, b);
}

int Resolver::effVisRank(ast::Visibility v) {
    int r = visRank(v);
    return r == 0 ? 2 : r;  // None → internal
}

void Resolver::checkBaseVisibility() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        int classRank = effVisRank(t->visibility);
        for (TypeSymbol* b : t->baseTypes) {
            if (!b) continue;  // built-in / unresolved — skip
            if (effVisRank(b->visibility) < classRank) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("base type `") + std::string(b->name) +
                    "` is less visible than `" + std::string(t->name) +
                    "` — a base class or implemented interface must be "
                    "at least as visible as the type (§13.7)");
            }
        }
    }
}

void Resolver::checkStaticConstructorParams() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeStaticCtorParams(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeStaticCtorParams(nt->type);
            }
        }
    }
}

void Resolver::checkTypeStaticCtorParams(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs || !cs->isStatic) continue;
        if (!cs->params.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("static constructor of `") +
                std::string(t->name) +
                "` may not declare parameters (§22.7)");
        }
    }
}

void Resolver::checkConstructorChains() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeConstructorChains(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeConstructorChains(nt->type);
            }
        }
    }
}

bool Resolver::isChainCall(const ast::Stmt* s) {
    auto* es = dynamic_cast<const ast::ExpressionStmt*>(s);
    if (!es || !es->expr) return false;
    auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
    if (!call || !call->callee) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(
        call->callee.get());
    return id && (id->name == "this" || id->name == "base");
}

void Resolver::checkTypeConstructorChains(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs) continue;
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) continue;
        int chainCount = 0;
        for (std::size_t i = 0; i < cd->body.size(); ++i) {
            if (!cd->body[i] || !isChainCall(cd->body[i].get())) continue;
            ++chainCount;
            if (i != 0) {
                ctx_.diagnostics().report(
                    Severity::Error, cd->body[i]->span.begin,
                    "a `base(...)` / `this(...)` constructor chain "
                    "call may appear only as the first statement "
                    "(§12.8)");
            }
        }
        if (chainCount > 1) {
            ctx_.diagnostics().report(
                Severity::Error, cd->span.begin,
                "a constructor may contain at most one `base(...)` "
                "or `this(...)` chain call (§12.8)");
        }
    }
}

void Resolver::checkAsyncModifiers() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeAsyncModifiers(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeAsyncModifiers(nt->type);
            }
        }
    }
}

void Resolver::checkTypeAsyncModifiers(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* ms = dynamic_cast<MethodSymbol*>(m);
        if (!ms) continue;
        if (hasMod(ms, "async") && hasMod(ms, "abstract")) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("method `") + std::string(ms->name) +
                "` may not be both `async` and `abstract`; declare a "
                "`Task` / `Task<T>` return type without `async` "
                "(§12.6)");
        }
    }
}

void Resolver::checkStaticNotVirtual() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeStaticNotVirtual(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeStaticNotVirtual(nt->type);
            }
        }
    }
}

void Resolver::checkTypeStaticNotVirtual(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* mem = dynamic_cast<MemberSymbol*>(m);
        if (!mem) continue;
        if (!dynamic_cast<MethodSymbol*>(m) &&
            !dynamic_cast<PropertySymbol*>(m)) {
            continue;
        }
        if (!hasMod(mem, "static")) continue;
        if (hasMod(mem, "virtual") || hasMod(mem, "abstract") ||
            hasMod(mem, "override")) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("static member `") + std::string(mem->name) +
                "` may not be virtual, abstract, or override (§12.1)");
        }
    }
}

void Resolver::checkMethodBodies() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeMethodBodies(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeMethodBodies(nt->type);
            }
        }
    }
}

void Resolver::checkTypeMethodBodies(TypeSymbol* t) {
    bool isInterface = t->kind == ast::TypeDeclKind::Interface;
    for (Symbol* m : t->members) {
        auto* ms = dynamic_cast<MethodSymbol*>(m);
        if (!ms) continue;
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        if (!md) continue;
        bool noBody = md->bodyKind == ast::MethodBodyKind::Abstract;
        bool isAbstract = hasMod(ms, "abstract");
        if (isInterface) {
            if (!noBody) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("interface method `") +
                    std::string(ms->name) +
                    "` may not have a body (§11.2)");
            }
            continue;
        }
        if (isAbstract && !noBody) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("abstract method `") +
                std::string(ms->name) +
                "` may not have a body (§12.5)");
        } else if (!isAbstract && noBody) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("non-abstract method `") +
                std::string(ms->name) +
                "` must have a body (§12.5)");
        }
    }
}

void Resolver::checkEnumRestrictions() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Enum) continue;
        for (Symbol* m : t->members) {
            auto bad = [&](std::string_view what) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("enum `") + std::string(t->name) +
                    "` may not declare " + std::string(what) +
                    " (§10.5)");
            };
            if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                if (!hasMod(fs, "static")) bad("an instance field");
                continue;
            }
            if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
                if (!cs->isStatic) bad("a constructor");
                continue;
            }
            if (dynamic_cast<NestedTypeSymbol*>(m)) {
                bad("a nested type");
                continue;
            }
            if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
                if (ps->isIndexer) bad("an indexer");
                continue;
            }
            if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                if (hasMod(ms, "virtual") || hasMod(ms, "abstract") ||
                    hasMod(ms, "override")) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("enum method `") +
                        std::string(ms->name) +
                        "` may not be virtual, abstract, or override "
                        "— enums dispatch statically (§10.5)");
                }
            }
        }
    }
}

void Resolver::checkConstTypes() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeConsts(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeConsts(nt->type);
            }
        }
    }
}

void Resolver::checkTypeConsts(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* cs = dynamic_cast<ConstMemberSymbol*>(m);
        if (!cs) continue;
        auto* cd = dynamic_cast<const ast::ConstDecl*>(cs->decl);
        if (!cd || !cd->type) continue;  // inferred — deferred
        TypeRef* rt = ctx_.resolvedType(*cd->type);
        if (!rt) continue;
        bool ok = rt->kind == TypeRef::Kind::Primitive &&
                  rt->primitive != PrimitiveType::Object;
        if (!ok) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("const `") + std::string(cs->name) +
                "` must have a primitive value type or `string` "
                "(§22.12)");
        }
    }
}

int Resolver::visRank(ast::Visibility v) {
    switch (v) {
        case ast::Visibility::Public: return 3;
        case ast::Visibility::Internal: return 2;
        case ast::Visibility::Protected: return 2;
        case ast::Visibility::Private: return 1;
        default: return 0;  // None — inherits; not checked
    }
}

void Resolver::checkAccessorVisibility() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeAccessorVisibility(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeAccessorVisibility(nt->type);
            }
        }
    }
}

void Resolver::checkTypeAccessorVisibility(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* ps = dynamic_cast<PropertySymbol*>(m);
        if (!ps) continue;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd) continue;
        int propRank = visRank(pd->visibility);
        if (propRank == 0) continue;  // interface / no explicit vis
        auto checkOne = [&](ast::Visibility av, SourceLocation loc) {
            if (av == ast::Visibility::None) return;
            if (visRank(av) > propRank) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("accessor of property `") +
                    std::string(ps->name) +
                    "` is less restrictive than the property's own "
                    "visibility (§13.6)");
            }
        };
        for (const auto& a : pd->accessors) {
            checkOne(a.visibility, pd->span.begin);
        }
        if (pd->hasAutoAccessorVis) {
            checkOne(pd->autoAccessorVis, pd->span.begin);
        }
    }
}

void Resolver::checkRecordRestrictions() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Record) continue;
        for (Symbol* m : t->members) {
            if (dynamic_cast<DestructorSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "` may not declare a destructor (§9.7)");
                continue;
            }
            if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                if (hasMod(fs, "owned")) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("record `") + std::string(t->name) +
                        "` may not have an `owned` field `" +
                        std::string(fs->name) + "` (§9.7)");
                    continue;
                }
                // Inline-array field with owned slots.
                if (auto* fd =
                        dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                    if (fd->type) {
                        auto* rt = ctx_.resolvedType(*fd->type);
                        if (rt &&
                            rt->kind == TypeRef::Kind::InlineArray &&
                            rt->arrayOwned) {
                            ctx_.diagnostics().report(
                                Severity::Error, m->declLoc,
                                std::string("record `") +
                                std::string(t->name) +
                                "` may not have an owning inline-array "
                                "field `" + std::string(fs->name) +
                                "` (§9.7)");
                        }
                    }
                }
            }
        }
        // IDisposable implementation: name-based check on the AST
        // base-type list (the built-in IDisposable has no user
        // TypeSymbol to resolve against).
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl)) {
            for (const auto& bt : td->baseTypes) {
                if (bt && bt->name && !bt->name->parts.empty() &&
                    bt->name->parts.back() == "IDisposable") {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("record `") + std::string(t->name) +
                        "` may not implement `IDisposable` (§9.7)");
                }
            }
        }
    }
}

void Resolver::checkTopLevelVisibility() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        if (t->visibility == ast::Visibility::Private ||
            t->visibility == ast::Visibility::Protected) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string("top-level type `") + std::string(t->name) +
                "` may only be `public` or `internal` (§13.2)");
        }
    }
}

bool Resolver::typeRefEqual(const TypeRef* a, const TypeRef* b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->nullable != b->nullable) return false;
    switch (a->kind) {
        case TypeRef::Kind::Error:
            return false;
        case TypeRef::Kind::Unit:
            return true;
        case TypeRef::Kind::Primitive:
            return a->primitive == b->primitive;
        case TypeRef::Kind::Named: {
            if (a->declaration != b->declaration) return false;
            if (a->genericArgs.size() != b->genericArgs.size())
                return false;
            for (std::size_t i = 0; i < a->genericArgs.size(); ++i) {
                if (!typeRefEqual(a->genericArgs[i], b->genericArgs[i]))
                    return false;
            }
            return true;
        }
        case TypeRef::Kind::InlineArray:
            return a->arrayOwned == b->arrayOwned &&
                   typeRefEqual(a->arrayElement, b->arrayElement);
        case TypeRef::Kind::Function: {
            if (a->funcParams.size() != b->funcParams.size())
                return false;
            for (std::size_t i = 0; i < a->funcParams.size(); ++i) {
                if (a->funcParams[i].modifier != b->funcParams[i].modifier)
                    return false;
                if (!typeRefEqual(a->funcParams[i].type,
                                  b->funcParams[i].type))
                    return false;
            }
            return a->funcReturnOwned == b->funcReturnOwned &&
                   typeRefEqual(a->funcReturn, b->funcReturn);
        }
    }
    return false;
}

std::string_view Resolver::overloadModifier(std::string_view mod) {
    if (mod == "owned" || mod.empty()) return "value";
    if (mod == "out owned") return "out owned";
    if (mod == "out") return "out";
    if (mod == "ref") return "ref";
    return mod;
}

void Resolver::checkOverloadSignatures() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeOverloads(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypeOverloads(nt->type);
            }
        }
    }
}

void Resolver::checkTypeOverloads(TypeSymbol* t) {
    using Overload = std::pair<const std::vector<Symbol*>*, Symbol*>;
    // Methods grouped by name; constructors grouped separately
    // (they overload as a single name-less family).
    std::unordered_map<std::string_view, std::vector<Overload>> methods;
    std::vector<Overload> ctors;
    for (Symbol* m : t->members) {
        if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
            methods[ms->name].push_back({&ms->params, m});
        } else if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
            if (!cs->isStatic) ctors.push_back({&cs->params, m});
        }
    }
    auto checkGroup = [&](std::vector<Overload>& overloads,
                          const std::string& label) {
        for (std::size_t i = 0; i < overloads.size(); ++i) {
            for (std::size_t j = i + 1; j < overloads.size(); ++j) {
                if (signaturesIndistinguishable(*overloads[i].first,
                                                *overloads[j].first)) {
                    ctx_.diagnostics().report(
                        Severity::Error, overloads[j].second->declLoc,
                        std::string("overload of ") + label +
                        " is indistinguishable from another overload "
                        "— overloads must differ in parameter count, "
                        "types, or `ref`/`out` modifiers; ordinary "
                        "`owned` does not distinguish (§22.6)");
                }
            }
        }
    };
    for (auto& [name, overloads] : methods) {
        checkGroup(overloads, std::string("`") + std::string(name) + "`");
    }
    checkGroup(ctors, "constructor");
}

bool Resolver::signaturesIndistinguishable(const std::vector<Symbol*>& a,
                                           const std::vector<Symbol*>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto* pa = dynamic_cast<ParameterSymbol*>(a[i]);
        auto* pb = dynamic_cast<ParameterSymbol*>(b[i]);
        if (!pa || !pb) return false;
        if (overloadModifier(pa->modifier) !=
            overloadModifier(pb->modifier)) {
            return false;
        }
        TypeRef* ta = pa->declType ? ctx_.resolvedType(*pa->declType)
                                   : nullptr;
        TypeRef* tb = pb->declType ? ctx_.resolvedType(*pb->declType)
                                   : nullptr;
        if (!typeRefEqual(ta, tb)) return false;
    }
    return true;
}

void Resolver::checkClassModifierCombinations() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Class) continue;
        bool isStatic = false, isAbstract = false, isSealed = false;
        for (auto mod : t->modifiers) {
            if (mod == "static") isStatic = true;
            else if (mod == "abstract") isAbstract = true;
            else if (mod == "sealed") isSealed = true;
        }
        if (isStatic && isAbstract) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string("class `") + std::string(t->name) +
                "` may not be both `abstract` and `static` "
                "(mutually exclusive — §11.7)");
        }
        if (isStatic && isSealed) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string("class `") + std::string(t->name) +
                "` may not be both `sealed` and `static` "
                "(mutually exclusive — §11.7)");
        }
    }
}

void Resolver::checkStaticClassMembers() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Class) continue;
        bool isStatic = false;
        for (auto mod : t->modifiers) {
            if (mod == "static") { isStatic = true; break; }
        }
        if (!isStatic) continue;
        for (Symbol* m : t->members) {
            checkStaticClassMember(t, m);
        }
    }
}

void Resolver::checkStaticClassMember(TypeSymbol* t, Symbol* m) {
    auto reportInstance = [&](std::string_view kind,
                              std::string_view name) {
        ctx_.diagnostics().report(
            Severity::Error,
            m->declLoc,
            std::string("static class `") + std::string(t->name) +
            "` may not declare instance " + std::string(kind) +
            " `" + std::string(name) + "` (§11.7 — a static class "
            "contains only static members)");
    };
    if (auto* dd = dynamic_cast<DestructorSymbol*>(m)) {
        (void)dd;
        ctx_.diagnostics().report(
            Severity::Error, m->declLoc,
            std::string("static class `") + std::string(t->name) +
            "` may not declare a destructor (§11.7)");
        return;
    }
    if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
        if (!cs->isStatic) reportInstance("constructor", t->name);
        return;
    }
    if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
        if (!hasMod(fs, "static")) {
            reportInstance("field", fs->name);
        } else if (hasMod(fs, "owned")) {
            // §11.7: a static class may not have owned fields at all.
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("static class `") + std::string(t->name) +
                "` may not declare an owned field `" +
                std::string(fs->name) + "` (§11.7)");
        }
        return;
    }
    if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
        if (!hasMod(ms, "static")) reportInstance("method", ms->name);
        return;
    }
    if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
        if (!hasMod(ps, "static")) {
            reportInstance(ps->isIndexer ? "indexer" : "property",
                           ps->name);
        }
        return;
    }
    if (auto* es = dynamic_cast<EventSymbol*>(m)) {
        if (!hasMod(es, "static")) reportInstance("event", es->name);
        return;
    }
    // ConstMemberSymbol (implicitly static), NestedTypeSymbol,
    // EnumConstantSymbol — permitted.
}

void Resolver::checkDuplicateMembers() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeDuplicates(t);
    }
}

bool Resolver::isOverloadable(Symbol* m) {
    return dynamic_cast<MethodSymbol*>(m) != nullptr ||
           dynamic_cast<ConstructorSymbol*>(m) != nullptr;
}

void Resolver::checkTypeDuplicates(TypeSymbol* t) {
    // Map name → the first member seen with that name. When a later
    // member collides and at least one side is non-overloadable,
    // diagnose at the later member.
    std::unordered_map<std::string_view, Symbol*> seen;
    for (Symbol* m : t->members) {
        if (!m || m->name.empty()) continue;
        // Constructors share the empty/synthetic name in some
        // designs, but here they carry the type name or are
        // overloadable anyway — skip diagnosing constructor-vs
        // collisions.
        auto it = seen.find(m->name);
        if (it == seen.end()) {
            seen[m->name] = m;
            continue;
        }
        Symbol* prior = it->second;
        // If both are overloadable, it's a legal overload pair.
        if (isOverloadable(m) && isOverloadable(prior)) {
            continue;
        }
        ctx_.diagnostics().report(
            Severity::Error, m->declLoc,
            std::string("duplicate member `") + std::string(m->name) +
            "` in type `" + std::string(t->name) +
            "` (a non-overloadable member cannot share a name with "
            "another member of the same type; §11.3 lists the member "
            "kinds — uniqueness is a well-formedness requirement)");
    }
    // Recurse into nested types.
    for (Symbol* m : t->members) {
        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
            if (nt->type) checkTypeDuplicates(nt->type);
        }
    }
}

bool Resolver::checkInheritanceCycles() {
    if (!ctx_.module()) return false;
    std::unordered_set<TypeSymbol*> reported;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        std::unordered_set<TypeSymbol*> onStack;
        std::unordered_set<TypeSymbol*> done;
        std::vector<TypeSymbol*> path;
        checkCycleFrom(t, onStack, done, path, reported);
    }
    return !reported.empty();
}

bool Resolver::hasMod(MemberSymbol* m, std::string_view want) {
    for (auto mod : m->modifiers) {
        if (mod == want) return true;
    }
    return false;
}

bool Resolver::baseMemberOverridable(MemberSymbol* m) {
    if (hasMod(m, "virtual") || hasMod(m, "abstract")) return true;
    if (hasMod(m, "override")) return !hasMod(m, "sealed");
    return false;
}

TypeRef* Resolver::applyGenericSubst(
        TypeRef* tr,
        const std::unordered_map<std::string_view, TypeRef*>& genericSubst) {
    if (!tr) return tr;
    if (tr->kind == TypeRef::Kind::Named && !tr->declaration) {
        // Unresolved named type — might be a generic param.
        if (tr->astNode && tr->astNode->name &&
            tr->astNode->name->parts.size() == 1) {
            auto it = genericSubst.find(tr->astNode->name->parts[0]);
            if (it != genericSubst.end()) {
                TypeRef* substituted = it->second;
                // Preserve the nullable modifier from the generic type
                // reference: T? → int? when T→int.
                if (tr->nullable && substituted && !substituted->nullable) {
                    TypeRef* nn = ctx_.makeTypeRef(*substituted);
                    nn->nullable = true;
                    return nn;
                }
                return substituted;
            }
        }
    }
    return tr;
}

void Resolver::checkOverrideSignatureMatch(MemberSymbol* derivedSym,
                                           MemberSymbol* baseSym) {
    auto* derived = dynamic_cast<MethodSymbol*>(derivedSym);
    auto* base = dynamic_cast<MethodSymbol*>(baseSym);
    if (!derived || !base) return;  // not a method pair — skip.
    auto err = [&](const std::string& what) {
        ctx_.diagnostics().report(
            Severity::Error, derived->declLoc,
            std::string("override of `") + std::string(derived->name) +
            "` does not match the base member's " + what + " (§12.1)");
    };

    // Build generic substitution map: base class's T → concrete arg.
    // Example: Container<T> base, IntBox : Container<int> derived →
    // {T → int}. Look up the base type's TypeRef from the derived
    // class's baseTypeRefs (which store the resolved generic args).
    std::unordered_map<std::string_view, TypeRef*> genericSubst;
    TypeSymbol* derivedClass = derived->parent;
    TypeSymbol* baseClass = base->parent;
    if (derivedClass && baseClass) {
        for (std::size_t bi = 0; bi < derivedClass->baseTypes.size(); ++bi) {
            if (derivedClass->baseTypes[bi] != baseClass) continue;
            TypeRef* baseRef = (bi < derivedClass->baseTypeRefs.size())
                               ? derivedClass->baseTypeRefs[bi] : nullptr;
            if (!baseRef) break;
            // Match base class generic params to the concrete args.
            for (std::size_t gi = 0; gi < baseClass->genericParams.size() &&
                 gi < baseRef->genericArgs.size(); ++gi) {
                if (auto* gp = dynamic_cast<GenericParamSymbol*>(
                                  baseClass->genericParams[gi])) {
                    genericSubst[gp->name] = baseRef->genericArgs[gi];
                }
            }
            break;
        }
    }

    // Visibility must match exactly.
    if (derived->visibility != base->visibility) {
        err("visibility");
    }
    auto* dDecl = dynamic_cast<const ast::MethodDecl*>(derived->decl);
    auto* bDecl = dynamic_cast<const ast::MethodDecl*>(base->decl);
    if (dDecl && bDecl) {
        // Return type (null == unit) and owned-return modifier.
        TypeRef* dRet = dDecl->returnType
            ? ctx_.resolvedType(*dDecl->returnType) : nullptr;
        TypeRef* bRet = bDecl->returnType
            ? ctx_.resolvedType(*bDecl->returnType) : nullptr;
        // Apply substitution to base return type (T → int, etc.).
        TypeRef* bRetSubst = applyGenericSubst(bRet, genericSubst);
        bool bothNullReturn = !dDecl->returnType && !bDecl->returnType;
        if (!bothNullReturn && !typeRefEqual(dRet, bRetSubst)) {
            err("return type");
        }
        if (dDecl->returnOwned != bDecl->returnOwned) {
            err("`owned` return modifier");
        }
    }
    // Parameters: count, exact modifier, exact type.
    if (derived->params.size() != base->params.size()) {
        err("parameter count");
        return;
    }
    for (std::size_t i = 0; i < derived->params.size(); ++i) {
        auto* dp = dynamic_cast<ParameterSymbol*>(derived->params[i]);
        auto* bp = dynamic_cast<ParameterSymbol*>(base->params[i]);
        if (!dp || !bp) continue;
        if (dp->modifier != bp->modifier) {
            err("parameter modifiers");
            return;
        }
        TypeRef* dt = dp->declType ? ctx_.resolvedType(*dp->declType)
                                   : nullptr;
        TypeRef* bt = bp->declType ? ctx_.resolvedType(*bp->declType)
                                   : nullptr;
        // Apply substitution to base parameter type.
        TypeRef* btSubst = applyGenericSubst(bt, genericSubst);
        if (dt && btSubst && !typeRefEqual(dt, btSubst)) {
            err("parameter types");
            return;
        }
    }
}

void Resolver::checkHidingAndOverride() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Class) continue;
        // Gather base CLASS members by name (transitive).
        std::unordered_map<std::string_view, MemberSymbol*> baseMembers;
        std::vector<TypeSymbol*> frontier;
        for (TypeSymbol* b : t->baseTypes) {
            if (b && b->kind == ast::TypeDeclKind::Class) {
                frontier.push_back(b);
            }
        }
        std::unordered_set<TypeSymbol*> seen;
        std::size_t i = 0;
        while (i < frontier.size()) {
            TypeSymbol* cur = frontier[i++];
            if (!seen.insert(cur).second) continue;
            for (Symbol* m : cur->members) {
                // Constructors and destructors are not inherited, so
                // they never participate in hiding/override.
                if (dynamic_cast<ConstructorSymbol*>(m) ||
                    dynamic_cast<DestructorSymbol*>(m)) {
                    continue;
                }
                if (auto* ms = dynamic_cast<MemberSymbol*>(m)) {
                    // First (most-derived) base wins on collision.
                    baseMembers.emplace(ms->name, ms);
                }
            }
            for (TypeSymbol* b : cur->baseTypes) {
                if (b && b->kind == ast::TypeDeclKind::Class) {
                    frontier.push_back(b);
                }
            }
        }
        if (baseMembers.empty()) continue;
        for (Symbol* m : t->members) {
            if (dynamic_cast<ConstructorSymbol*>(m) ||
                dynamic_cast<DestructorSymbol*>(m)) {
                continue;
            }
            auto* ms = dynamic_cast<MemberSymbol*>(m);
            if (!ms || ms->name.empty()) continue;
            auto it = baseMembers.find(ms->name);
            if (it == baseMembers.end()) continue;
            MemberSymbol* base = it->second;
            bool isOverride = hasMod(ms, "override");
            if (isOverride) {
                if (!baseMemberOverridable(base)) {
                    ctx_.diagnostics().report(
                        Severity::Error, ms->declLoc,
                        std::string("`") + std::string(ms->name) +
                        "` is declared `override` but the base "
                        "member it would override is not `virtual`, "
                        "`abstract`, or `override` (§12.1)");
                } else {
                    // Slice S20: a legal override must match the
                    // base member's signature exactly.
                    checkOverrideSignatureMatch(ms, base);
                }
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, ms->declLoc,
                    std::string("member `") + std::string(ms->name) +
                    "` hides an inherited member of the same name; "
                    "hiding is forbidden — use `override` for a "
                    "virtual base member, or choose a different "
                    "name (§12.2)");
            }
        }
    }
}

bool Resolver::methodIsAbstract(MethodSymbol* ms) {
    if (ms->parent &&
        ms->parent->kind == ast::TypeDeclKind::Interface) {
        return true;
    }
    for (auto mod : ms->modifiers) {
        if (mod == "abstract") return true;
    }
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
        if (md->bodyKind == ast::MethodBodyKind::Abstract) return true;
    }
    return false;
}

void Resolver::checkAbstractImplementations() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        if (t->kind != ast::TypeDeclKind::Class) continue;
        bool isAbstract = false;
        for (auto mod : t->modifiers) {
            if (mod == "abstract") { isAbstract = true; break; }
        }
        if (isAbstract) continue;  // abstract classes need not impl.

        std::unordered_set<std::string_view> abstractNames;
        std::unordered_set<std::string_view> concreteNames;
        // BFS the chain.
        std::vector<TypeSymbol*> frontier{t};
        std::unordered_set<TypeSymbol*> seen{t};
        std::size_t i = 0;
        while (i < frontier.size()) {
            TypeSymbol* cur = frontier[i++];
            for (Symbol* m : cur->members) {
                if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                    if (methodIsAbstract(ms))
                        abstractNames.insert(ms->name);
                    else
                        concreteNames.insert(ms->name);
                }
            }
            for (TypeSymbol* b : cur->baseTypes) {
                if (b && !seen.count(b)) {
                    seen.insert(b);
                    frontier.push_back(b);
                }
            }
        }
        for (auto name : abstractNames) {
            if (!concreteNames.count(name)) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("concrete class `") +
                    std::string(t->name) +
                    "` does not implement inherited abstract member `" +
                    std::string(name) + "` (§12.5)");
            }
        }
    }
}

bool Resolver::checkCycleFrom(
    TypeSymbol* t,
    std::unordered_set<TypeSymbol*>& onStack,
    std::unordered_set<TypeSymbol*>& done,
    std::vector<TypeSymbol*>& path,
    std::unordered_set<TypeSymbol*>& reported) {
    if (done.count(t)) return false;  // already fully explored, no cycle
    if (onStack.count(t)) {
        // Back-edge → actual cycle.
        if (!reported.count(t)) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string("circular inheritance detected at `") +
                std::string(t->name) +
                "` — base-type chain re-enters this type "
                "(circular inheritance is ill-formed: the dispatch "
                "model of §12 presupposes a well-founded base chain)");
            reported.insert(t);
        }
        return true;
    }
    onStack.insert(t);
    path.push_back(t);
    for (TypeSymbol* b : t->baseTypes) {
        if (b) checkCycleFrom(b, onStack, done, path, reported);
    }
    path.pop_back();
    onStack.erase(t);
    done.insert(t);
    return false;
}

// ---------------------------------------------------------------------------
// §14.2 / Appendix C.4 — generic constraint validation at instantiation sites.
// ---------------------------------------------------------------------------

bool Resolver::isGenericParamRef(const ast::Type& t, Scope* scope) {
    // A single-identifier Named type that is NOT a primitive and resolves to
    // an in-scope generic-param marker (a key present in the scope chain with
    // a null Symbol*). resolveNamed records these as Named with no
    // declaration; we must distinguish them from built-ins, which are not in
    // scope at all.
    if (t.kind != ast::TypeKind::Named || !t.name ||
        t.name->parts.size() != 1) {
        return false;
    }
    std::string_view n = t.name->parts.front();
    if (primitiveFromName(n) != PrimitiveType::None) return false;
    for (Scope* s = scope; s; s = s->parent) {
        auto it = s->names.find(n);
        if (it != s->names.end()) {
            // Present in scope. A null entry is the generic-param marker;
            // a non-null TypeSymbol entry is a concrete (e.g. nested) type.
            return it->second == nullptr;
        }
    }
    return false;
}

Resolver::TypeCategory Resolver::categorize(const TypeRef* a) {
    if (!a) return TypeCategory::Unknown;
    switch (a->kind) {
        case TypeRef::Kind::Error:
            return TypeCategory::Unknown;
        case TypeRef::Kind::Unit:
            // The unit type is a value type (§14.2 value table row).
            return TypeCategory::Value;
        case TypeRef::Kind::InlineArray:
            // Inline arrays are value types (§14.2 / §15.1).
            return TypeCategory::Value;
        case TypeRef::Kind::Function:
            // Function values are reference (runtime-managed) types (§14.2).
            return TypeCategory::Reference;
        case TypeRef::Kind::Primitive:
            // `string` and `Object` are reference types; every other
            // primitive (bool/char/numerics) is a value type (§14.2 / §3).
            if (a->primitive == PrimitiveType::String ||
                a->primitive == PrimitiveType::Object) {
                return TypeCategory::Reference;
            }
            return TypeCategory::Value;
        case TypeRef::Kind::Named:
            if (a->declaration) {
                // Classes and interfaces are reference types; records and
                // enums are value types (§9 / §10 / §14.2).
                switch (a->declaration->kind) {
                    case ast::TypeDeclKind::Class:
                    case ast::TypeDeclKind::Interface:
                        return TypeCategory::Reference;
                    case ast::TypeDeclKind::Record:
                    case ast::TypeDeclKind::Enum:
                        return TypeCategory::Value;
                }
            }
            // Built-in named types (Array, OwnedArray, IEnumerable, …) carry
            // no user declaration. Every built-in named type in the language
            // is a class or interface — a reference type (§14.2). A generic
            // parameter also lands here, but the caller filters those out via
            // isGenericParamRef before calling categorize.
            if (a->astNode && a->astNode->name &&
                a->astNode->name->parts.size() == 1 &&
                builtinTypeArity(a->astNode->name->parts.front()) !=
                    kNotBuiltin) {
                return TypeCategory::Reference;
            }
            return TypeCategory::Unknown;
    }
    return TypeCategory::Unknown;
}

bool Resolver::argSatisfiesTypeBound(const TypeRef* arg, const TypeRef* bound) {
    if (!arg || !bound) return false;
    if (arg->kind != TypeRef::Kind::Named || !arg->declaration) return false;
    if (bound->kind != TypeRef::Kind::Named || !bound->declaration) {
        return false;
    }
    // BFS over the argument's transitive base/interface set, cycle-guarded.
    // The argument conforms when `bound`'s declaration is the argument
    // itself or any of its (transitive) bases. (Generic-argument matching of
    // the bound — e.g. distinguishing IComparable<int> from IComparable<long>
    // — is left to the variance/substitution slice; this check is by
    // declaration identity, which is sound for the common monomorphic and
    // single-instantiation cases and never over-rejects a true subtype.)
    TypeSymbol* target = bound->declaration;
    std::vector<TypeSymbol*> frontier{arg->declaration};
    std::unordered_set<TypeSymbol*> seen{arg->declaration};
    std::size_t i = 0;
    while (i < frontier.size()) {
        TypeSymbol* t = frontier[i++];
        if (t == target) return true;
        for (TypeSymbol* b : t->baseTypes) {
            if (b && seen.insert(b).second) frontier.push_back(b);
        }
    }
    return false;
}

bool Resolver::argSatisfiesNew(const TypeRef* arg, bool& decided) {
    decided = false;
    if (!arg) return false;
    // Value types: `default(T)` stands in for `new T()`. We can only be sure
    // the default is well-defined for primitives, enums, and the unit type;
    // a record's default depends on its field types (a non-nullable
    // reference field makes the default ill-formed, §17.11). To stay
    // conservative we decide only the cases we can verify and defer the rest.
    if (arg->kind == TypeRef::Kind::Primitive) {
        // string / Object are reference types and handled below; the value
        // primitives all have a well-defined zero (§17.11).
        if (arg->primitive == PrimitiveType::String ||
            arg->primitive == PrimitiveType::Object) {
            decided = false;  // reference; fall through to class handling
        } else {
            decided = true;
            return true;
        }
    }
    if (arg->kind == TypeRef::Kind::Named && arg->declaration) {
        TypeSymbol* d = arg->declaration;
        if (d->kind == ast::TypeDeclKind::Enum) {
            decided = true;  // enum default is the underlying-0 value (§17.11)
            return true;
        }
        if (d->kind == ast::TypeDeclKind::Record) {
            // A positional record satisfies new() iff default(R) is
            // well-defined — every field has a well-defined default
            // (§14.2 / §17.11). Verify each positional field's resolved type
            // is not a non-nullable reference type.
            auto* td = dynamic_cast<const ast::TypeDecl*>(d->decl);
            if (!td) return false;  // defer (decided stays false)
            decided = true;
            for (const auto& pf : td->positionalFields) {
                if (!pf.type) continue;
                TypeRef* ft = ctx_.resolvedType(*pf.type);
                if (!ft) { decided = false; return false; }  // can't verify
                if (ft->nullable) continue;  // nullable field → defaults null
                if (categorize(ft) == TypeCategory::Reference) {
                    return false;  // non-nullable reference field: no default
                }
            }
            // Body fields of a record can also carry initializers; a record
            // with body fields lacking defaults would be ill-formed under
            // §17.11, but verifying that fully is the default-operator slice.
            // The positional-field check covers the spec's worked example.
            return true;
        }
        if (d->kind == ast::TypeDeclKind::Class) {
            // A class satisfies new() iff it has an accessible parameterless
            // constructor, or declares no constructor at all (the implicit
            // default constructor, mirroring checkNewExprArgs). A class with
            // a primary constructor carries its params on the TypeDecl, not a
            // ConstructorSymbol; treat a non-empty primary param list as a
            // non-parameterless constructor.
            auto* td = dynamic_cast<const ast::TypeDecl*>(d->decl);
            if (td && td->hasPositionalFields &&
                !td->positionalFields.empty()) {
                decided = true;
                return false;  // primary ctor has parameters
            }
            std::vector<const ConstructorSymbol*> ctors;
            for (Symbol* m : d->members) {
                if (auto* cs = dynamic_cast<const ConstructorSymbol*>(m)) {
                    if (!cs->isStatic) ctors.push_back(cs);
                }
            }
            decided = true;
            if (ctors.empty()) return true;  // implicit default ctor
            for (const ConstructorSymbol* cs : ctors) {
                if (cs->params.empty() &&
                    cs->visibility != ast::Visibility::Private) {
                    return true;  // accessible parameterless ctor
                }
            }
            return false;
        }
        if (d->kind == ast::TypeDeclKind::Interface) {
            decided = true;
            return false;  // interfaces are not constructible (§11.2)
        }
    }
    // Built-ins, generic params, function/inline-array args: defer.
    return false;
}

namespace {

// Render an argument type's spelling for diagnostics from its AST node.
// Falls back to "<type>" when the node has no name (function/inline-array
// args, which constraint checks defer on anyway).
std::string argSpelling(const ast::Type* argAst) {
    if (!argAst) return "<type>";
    std::string out;
    if (argAst->kind == ast::TypeKind::Named && argAst->name) {
        for (std::size_t i = 0; i < argAst->name->parts.size(); ++i) {
            if (i) out += '.';
            out += std::string(argAst->name->parts[i]);
        }
    } else {
        out = "<type>";
    }
    if (argAst->nullable) out += '?';
    return out.empty() ? "<type>" : out;
}

}  // namespace

void Resolver::checkOneConstraint(const ast::Type& t,
                                  std::string_view paramName,
                                  const ast::ConstraintItem& item,
                                  const ast::Type* argAst, TypeRef* arg,
                                  Scope* boundScope) {
    const std::string param(paramName);
    const std::string argName = argSpelling(argAst);
    auto report = [&](const std::string& reason) {
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            std::string("type argument `") + argName +
            "` does not satisfy the constraint `where " + param + ": " +
            reason + "` (§14.2)");
    };

    if (item.kind == "notnull") {
        // §14.2: non-nullable types only (value or reference).
        if (arg->nullable) report("notnull");
        return;
    }
    if (item.kind == "class") {
        // §14.2: non-nullable reference types only.
        if (arg->nullable) { report("class"); return; }
        TypeCategory c = categorize(arg);
        if (c == TypeCategory::Value) report("class");
        // Unknown → defer (no diagnostic).
        return;
    }
    if (item.kind == "value") {
        // §14.2: value types only. A reference-typed argument is the clear
        // violation; nullability of a value type is left to a later slice to
        // avoid a false positive here.
        TypeCategory c = categorize(arg);
        if (c == TypeCategory::Reference) report("value");
        return;
    }
    if (item.kind == "new") {
        bool decided = false;
        bool ok = argSatisfiesNew(arg, decided);
        if (decided && !ok) report("new()");
        return;
    }
    if (item.kind == "type") {
        // §14.2: a type-bound (class or interface). Implies non-null T —
        // a nullable argument never satisfies it.
        if (!item.typeArg) return;
        TypeRef* bound = ctx_.resolvedType(*item.typeArg);
        if (!bound) {
            // Not yet resolved (forward-declared generic type instantiated
            // before its own constraint clause was visited). Resolve on
            // demand in the declaring type's scope.
            bound = resolveType(*item.typeArg, boundScope);
        }
        if (!bound || bound->kind == TypeRef::Kind::Error) return;  // defer
        // We can only verify a type-bound when both sides are user-declared
        // named types (BFS over baseTypes). Built-in bounds/args carry no
        // base-type chain here; defer to avoid false positives.
        if (bound->kind != TypeRef::Kind::Named || !bound->declaration) {
            return;
        }
        if (arg->kind != TypeRef::Kind::Named || !arg->declaration) {
            return;  // built-in / generic-param arg: defer
        }
        const std::string boundName(bound->declaration->name);
        if (arg->nullable) { report(boundName); return; }
        if (!argSatisfiesTypeBound(arg, bound)) report(boundName);
        return;
    }
    // Unknown constraint kind: ignore (parser would not produce it).
}

void Resolver::checkGenericConstraints(const ast::Type& t, TypeSymbol* tsym,
                                       const std::vector<TypeRef*>& argRefs,
                                       Scope* scope) {
    // Constraints live on the declaring TypeDecl's `where` clauses, keyed by
    // type-parameter name. Build name → constraint-items map.
    auto* td = dynamic_cast<const ast::TypeDecl*>(tsym->decl);
    if (!td || td->genericConstraints.empty()) return;

    // Reconstruct a lookup scope for resolving constraint type-bounds on
    // demand: module-scope names plus the declaring type's own generic-param
    // names (so `where T: IComparable<T>` resolves T). Bound resolution is
    // done lazily because instantiation sites are visited in source order and
    // a forward-declared generic type's constraint clause may not yet be
    // resolved.
    Scope boundModule;
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (!child->name.empty()) boundModule.names[child->name] = child;
        }
    }
    Scope boundScope;
    boundScope.parent = &boundModule;
    for (Symbol* gp : tsym->genericParams) {
        if (gp) boundScope.names[gp->name] = nullptr;  // generic-param marker
    }

    for (std::size_t pi = 0; pi < tsym->genericParams.size() &&
         pi < t.genericArgs.size() && pi < argRefs.size(); ++pi) {
        auto* gp = dynamic_cast<GenericParamSymbol*>(tsym->genericParams[pi]);
        if (!gp) continue;
        const ast::Type* argAst = t.genericArgs[pi].get();
        TypeRef* arg = argRefs[pi];
        if (!argAst || !arg) continue;
        // Skip arguments that are themselves generic params — their
        // classification is only known at the outer instantiation, where the
        // outer constraints are checked (§14.2).
        if (isGenericParamRef(*argAst, scope)) continue;
        if (arg->kind == TypeRef::Kind::Error) continue;

        // Find the constraint clause(s) for this parameter name.
        for (const auto& gc : td->genericConstraints) {
            if (gc.typeParam != gp->name) continue;
            // The `is owned` / `is not owned` modifier-constraint form is
            // historical (removed in v0.97, §14.4) and carries no
            // instantiation-time obligation here; skip it.
            if (!gc.modifierConstraint.empty()) continue;
            for (const auto& item : gc.items) {
                checkOneConstraint(t, gp->name, item, argAst, arg,
                                   &boundScope);
            }
        }
    }
}

}  // namespace bezel::sema
