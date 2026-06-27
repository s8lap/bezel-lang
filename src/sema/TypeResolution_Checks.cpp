#include "sema/TypeResolution_impl.hpp"

#include "sema/ConstFold.hpp"

#include "sema/TypeResolution.hpp"

#include <cctype>

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

// §13.3 (#956): every type appearing in a member's signature must be at least
// as visible as the member; "a private member is unconstrained". A private
// (nested) type is the least visible level, so a NON-private member whose
// signature names a private type is the unambiguous violation (the repro:
// `internal _x: NestedPrivate?`). We target that clear case — a broader rank
// comparison across internal/protected/public is a follow-up. Builtins and
// generic parameters (null declaration) are fully visible and never flagged.
void Resolver::checkMemberSignatureVisibility() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypeMemberSignatureVisibility(t);
        for (Symbol* m : t->members)
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m))
                if (nt->type) checkTypeMemberSignatureVisibility(nt->type);
    }
}

void Resolver::checkTypeMemberSignatureVisibility(TypeSymbol* t) {
    auto checkSig = [&](const ast::Type* astTy, const MemberSymbol* mem,
                        std::string_view what) {
        if (!astTy) return;
        const TypeRef* rt = ctx_.resolvedType(*astTy);
        if (!rt || !rt->declaration) return;  // builtin / generic param
        if (rt->declaration->visibility == ast::Visibility::Private) {
            ctx_.diagnostics().report(
                Severity::Error, mem->declLoc,
                std::string("the ") + std::string(what) + " type `" +
                std::string(rt->declaration->name) +
                "` is private and less visible than `" +
                std::string(mem->name) +
                "` — a type in a member's signature must be at least as "
                "visible as the member (§13.3)");
        }
    };
    for (Symbol* m : t->members) {
        auto* mem = dynamic_cast<MemberSymbol*>(m);
        if (!mem) continue;
        // §13.3: a private member is unconstrained.
        if (mem->visibility == ast::Visibility::Private) continue;
        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
                checkSig(fd->type.get(), mem, "field");
        } else if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                checkSig(md->returnType.get(), mem, "return");
                for (const auto& p : md->params)
                    checkSig(p.type.get(), mem, "parameter");
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

namespace {

// §30.1 / §30.6 / §30.7: the admitted return types for an `async` callable.
// `Task` / `Task<T>` / `OwnedTask<T>` cover the value/borrow/owned result
// patterns (§30.5); the six async-iterator interfaces (§30.7) cover async
// `yield`. A bare single-identifier Named type (no nullable, no namespace
// qualifier) whose head is one of these names is admitted; the generic arity
// is the resolver's separate concern. Anything else (a value type, a sync
// `IEnumerable<T>`, a plain class, or no declared return type) is rejected.
bool isAdmittedAsyncReturnType(const ast::Type* rt) {
    if (!rt) return false;                              // no return type → unit
    if (rt->kind != ast::TypeKind::Named || rt->nullable) return false;
    if (!rt->name || rt->name->parts.size() != 1) return false;
    std::string_view head = rt->name->parts.front();
    return head == "Task" || head == "OwnedTask" ||
           head == "IAsyncEnumerable" || head == "IAsyncEnumerator" ||
           head == "OwnedAsyncEnumerable" || head == "OwnedAsyncEnumerator";
}

}  // namespace

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
        // §30.1: an `async` method's declared return type must be one of the
        // admitted async forms (`Task` / `Task<T>` / `OwnedTask<T>`, or an
        // async-iterator interface, §30.7). A non-async return type — including
        // a missing one (the implicit unit return) — is a compile error. The
        // `async abstract` combination is already diagnosed above; we still run
        // the return-type check on it so the more specific cause is also named.
        if (hasMod(ms, "async")) {
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            if (md && !isAdmittedAsyncReturnType(md->returnType.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("`async` method `") + std::string(ms->name) +
                    "` must declare a `Task`, `Task<T>`, or `OwnedTask<T>` "
                    "return type (or an async-iterator interface), not a "
                    "non-async return type (§30.1)");
            }
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
        // §12.1: `abstract`, `virtual`, `override`, and `sealed override` are
        // mutually exclusive — at most one inheritance modifier may appear on a
        // member. Without this check `virtual abstract` / `virtual override` /
        // `abstract override` were silently accepted, producing ABI-corrupt or
        // contradictory dispatch (R39). `sealed` is a class-level modifier and
        // appears on a member only as part of `sealed override`.
        {
            int n = (hasMod(mem, "abstract") ? 1 : 0) +
                    (hasMod(mem, "virtual") ? 1 : 0) +
                    (hasMod(mem, "override") ? 1 : 0);
            if (n > 1) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("member `") + std::string(mem->name) +
                    "` may have at most one of `abstract`, `virtual`, "
                    "`override`, or `sealed override` — they are mutually "
                    "exclusive (§12.1)");
            }
            if (hasMod(mem, "sealed") && !hasMod(mem, "override")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("member `") + std::string(mem->name) +
                    "` may not be `sealed` on its own — on a member `sealed` "
                    "appears only as `sealed override` (§12.1)");
            }
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
        // §10.3 (#740): every declared enum member value must fit the
        // underlying type's range. The underlying type is explicit
        // (enumUnderlyingType) or the default `int`. A value that fits `int`
        // but exceeds a smaller declared underlying type (`enum E: byte A :=
        // 256;`) previously wrapped silently at codegen.
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl)) {
            using P = PrimitiveType;
            P up = P::Int;
            if (t->enumUnderlyingType &&
                t->enumUnderlyingType->kind == TypeRef::Kind::Primitive)
                up = t->enumUnderlyingType->primitive;
            auto fits = [](long long v, P p) -> bool {
                switch (p) {
                    case P::Byte:   return v >= 0 && v <= 255;
                    case P::SByte:  return v >= -128 && v <= 127;
                    case P::Short:  return v >= -32768 && v <= 32767;
                    case P::UShort: return v >= 0 && v <= 65535;
                    case P::Int:    return v >= -2147483648LL && v <= 2147483647LL;
                    case P::UInt:   return v >= 0 && v <= 4294967295LL;
                    // long / ulong: any 64-bit pattern is in range (the literal
                    // type-check already rejects values that exceed 64 bits).
                    default:        return true;
                }
            };
            auto spell = [](P p) -> const char* {
                switch (p) {
                    case P::Byte: return "byte"; case P::SByte: return "sbyte";
                    case P::Short: return "short"; case P::UShort: return "ushort";
                    case P::Int: return "int"; case P::UInt: return "uint";
                    case P::Long: return "long"; case P::ULong: return "ulong";
                    default: return "the underlying type";
                }
            };
            std::unordered_map<std::string_view, long long> siblings;
            long long running = 0;
            for (const auto& ec : td->enumConstants) {
                long long val = running;
                bool haveVal = true;
                if (ec.value) {
                    auto resolve = [&](std::string_view name, int)
                        -> std::optional<long long> {
                        auto it = siblings.find(name);
                        if (it != siblings.end()) return it->second;
                        return std::nullopt;
                    };
                    if (auto folded = foldIntConst(*ec.value, resolve))
                        val = *folded;
                    else
                        haveVal = false;  // non-constant — a separate concern
                }
                if (haveVal) {
                    siblings[ec.name] = val;
                    if (!fits(val, up))
                        ctx_.diagnostics().report(
                            Severity::Error,
                            ec.value ? ec.value->span.begin : t->declLoc,
                            std::string("enum member `") + std::string(ec.name) +
                            "` value " + std::to_string(val) +
                            " does not fit the underlying type `" + spell(up) +
                            "` (§10.3 — all declared values must fit the "
                            "underlying type's range)");
                }
                running = val + 1;
            }
        }
    }
}

void Resolver::checkGenericConstraintConsistency() {
    if (!ctx_.module()) return;
    // §14.2: "Contradictory constraint combinations (e.g. class, value) are
    // compile errors." Check each generic declaration's `where` clauses once,
    // independent of instantiation (an un-instantiated generic was never
    // checked by the per-instantiation checkGenericConstraints).
    auto checkList =
        [&](const std::vector<ast::GenericConstraint>& gcs, SourceLocation loc) {
            // Per type-parameter: does it require both `class` and `value`?
            std::unordered_map<std::string_view, std::pair<bool, bool>> kinds;
            for (const auto& gc : gcs)
                for (const auto& item : gc.items) {
                    if (item.kind == "class") kinds[gc.typeParam].first = true;
                    else if (item.kind == "value") kinds[gc.typeParam].second = true;
                }
            for (const auto& kv : kinds)
                if (kv.second.first && kv.second.second)
                    ctx_.diagnostics().report(
                        Severity::Error, loc,
                        "contradictory constraints on type parameter `" +
                        std::string(kv.first) +
                        "`: `class` and `value` cannot both be required (§14.2)");
        };
    for (Symbol* child : ctx_.module()->children) {
        auto* ts = dynamic_cast<TypeSymbol*>(child);
        if (!ts || !ts->decl) continue;
        auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
        if (!td) continue;
        checkList(td->genericConstraints, td->span.begin);
        for (const auto& m : td->members)
            if (auto* mdl = dynamic_cast<const ast::MethodDecl*>(m.get()))
                checkList(mdl->genericConstraints, mdl->span.begin);
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
            continue;
        }
        // §6.1 / Appendix C.11 (#958): a const initializer is a compile-time
        // constant; an integer const whose folded initializer does not fit its
        // declared integer type is a compile error (the value cannot be
        // represented). Without this it compiled and raised OverflowException at
        // runtime — const behaving like static-readonly. Fold the initializer
        // (resolving sibling const references, with the shared depth guard) and
        // range-check against the declared type.
        using P = PrimitiveType;
        const P p = rt->primitive;
        const bool isIntConst =
            p == P::Byte || p == P::SByte || p == P::Short || p == P::UShort ||
            p == P::Int || p == P::UInt || p == P::Long || p == P::ULong;
        if (isIntConst && cd->value) {
            auto fits = [](long long v, P pt) -> bool {
                switch (pt) {
                    case P::Byte:   return v >= 0 && v <= 255;
                    case P::SByte:  return v >= -128 && v <= 127;
                    case P::Short:  return v >= -32768 && v <= 32767;
                    case P::UShort: return v >= 0 && v <= 65535;
                    case P::Int:    return v >= -2147483648LL &&
                                           v <= 2147483647LL;
                    case P::UInt:   return v >= 0 && v <= 4294967295LL;
                    default:        return true;  // long / ulong: 64-bit
                }
            };
            std::function<std::optional<long long>(std::string_view, int)>
                resolve = [&](std::string_view name,
                              int depth) -> std::optional<long long> {
                for (Symbol* sm : t->members) {
                    auto* scs = dynamic_cast<ConstMemberSymbol*>(sm);
                    if (!scs || scs->name != name) continue;
                    if (auto* scd =
                            dynamic_cast<const ast::ConstDecl*>(scs->decl))
                        if (scd->value)
                            return foldIntConst(*scd->value, resolve, depth + 1);
                    return std::nullopt;
                }
                return std::nullopt;
            };
            if (auto folded = foldIntConst(*cd->value, resolve)) {
                if (!fits(*folded, p)) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("const `") + std::string(cs->name) +
                        "` initializer value " + std::to_string(*folded) +
                        " does not fit its declared type — a const initializer "
                        "must be a representable compile-time constant "
                        "(§6.1 / Appendix C.11)");
                }
            }
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
        // §9.6: a record may not derive from a class or from another record.
        // It may only implement interfaces. Walk the resolved baseTypes list
        // (populated by TypeResolution pass S5) and reject any base whose
        // kind is Class or Record. Interfaces are allowed; unresolved (null)
        // bases are skipped here — unresolved-name errors were already emitted
        // during the base-type resolution pass.
        for (TypeSymbol* base : t->baseTypes) {
            if (!base) continue;
            if (base->kind == ast::TypeDeclKind::Class) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "` cannot derive from class `" +
                    std::string(base->name) +
                    "` (§9.6); records may only implement interfaces");
            } else if (base->kind == ast::TypeDeclKind::Record) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "` cannot derive from record `" +
                    std::string(base->name) +
                    "` (§9.6); records may only implement interfaces");
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

void Resolver::checkUserDeclaredDispose() {
    // #594 (§28.2): "User code does not implement Dispose() directly.  The
    // customization points are: a destructor for cleanup logic; owned fields
    // for fields that should be disposed with the containing object."
    //
    // A user-declared `Dispose()` on a class or record is never invoked by
    // the compiler's ownership machinery (using var / owned field teardown /
    // explicit Dispose call all go through the compiler-generated Dispose) so
    // the user body silently never runs — a silent trap.  Reject it here.
    //
    // Applies to both Class and Record even though records cannot normally
    // carry a destructor (§9.7): a user who accidentally writes a Dispose()
    // method on a record also gets a clear diagnostic rather than a silent
    // no-op (the destructor restriction is a separate, earlier check).
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        if (t->kind != ast::TypeDeclKind::Class &&
            t->kind != ast::TypeDeclKind::Record)
            continue;
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms || ms->name != "Dispose") continue;
            // Only reject a zero-parameter Dispose(): that's the exact
            // signature the compiler generates per §28.2.  A user method named
            // Dispose with parameters is unusual but technically unambiguous
            // and not what §28.2 restricts.
            if (!ms->params.empty()) continue;
            ctx_.diagnostics().report(
                Severity::Error, ms->declLoc,
                std::string(t->kind == ast::TypeDeclKind::Record ? "record" : "class") +
                " `" + std::string(t->name) +
                "` may not declare a `Dispose()` method directly (§28.2): "
                "the compiler synthesises `Dispose()` from the `destructor`; "
                "user-declared `Dispose()` bodies are never called by the "
                "ownership machinery.  Use a `destructor` for cleanup logic "
                "and `owned` fields for resource fields instead.");
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
    // §23.4 (#774): indexers overload as a single name-less family (like
    // constructors), distinguished by their parameter type lists.
    std::vector<Overload> indexers;
    for (Symbol* m : t->members) {
        if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
            methods[ms->name].push_back({&ms->params, m});
        } else if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
            if (!cs->isStatic) ctors.push_back({&cs->params, m});
        } else if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
            if (ps->isIndexer) indexers.push_back({&ps->indexerParams, m});
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
    checkGroup(indexers, "indexer");  // §23.4 (#774)
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

// §23.5 (#747 R42): static indexers are not supported — in ANY class or record,
// not only static classes. A `static` indexer was silently accepted and then
// miscompiled (the access lowered to garbage). Reject it at the declaration.
void Resolver::checkStaticIndexers() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        for (Symbol* m : t->members) {
            auto* ps = dynamic_cast<PropertySymbol*>(m);
            if (ps && ps->isIndexer && hasMod(ps, "static")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "static indexers are not supported (§23.5)");
            }
        }
    }
}

// §25.2 (#746 R41): comparison operators must be overloaded as a complete group
// — `<`, `>`, `<=`, `>=` together; `=` and `<>` together. A partial group was
// silently accepted, and then using a missing member (`v1 <= v2` with only
// `<`/`>` declared) fell through to a struct-level `icmp` → malformed IR. Reject
// the incomplete group at the declaration so the downstream IR_FAIL can't arise.
void Resolver::checkOperatorGroups() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        bool lt = false, gt = false, le = false, ge = false;
        bool eq = false, ne = false;
        Symbol* cmpFirst = nullptr;
        Symbol* eqFirst = nullptr;
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms || !ms->isOperator) continue;
            std::string_view tok = ms->operatorToken;
            // §25.1 (#746 R41): operators excluded from the overload set. `not`
            // is boolean negation (use a named `.Negate()` or overload `~`); the
            // null-aware / type-test / range operators have language-fixed
            // semantics. These were silently accepted as overload declarations.
            if (tok == "not" || tok == "!" || tok == "??" || tok == "is" ||
                tok == "as" || tok == ".." || tok == "..<") {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "operator `" + std::string(tok) +
                    "` may not be overloaded (§25.1)");
                continue;
            }
            if (tok == "<" || tok == ">" || tok == "<=" || tok == ">=") {
                if (!cmpFirst) cmpFirst = m;
                if (tok == "<") lt = true;
                else if (tok == ">") gt = true;
                else if (tok == "<=") le = true;
                else ge = true;
            } else if (tok == "=" || tok == "<>") {
                // §25.2: `=` and `<>` may not be overloaded on a CLASS — classes
                // customize equality by overriding `Equals`. Only records and
                // enums may overload them. (Was silently accepted.)
                if (t->kind == ast::TypeDeclKind::Class) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        "`" + std::string(tok) +
                        "` may not be overloaded on a class — classes customize "
                        "equality by overriding `Equals` (§25.2)");
                    continue;
                }
                if (!eqFirst) eqFirst = m;
                if (tok == "=") eq = true;
                else ne = true;
            }
        }
        if (cmpFirst && !(lt && gt && le && ge)) {
            ctx_.diagnostics().report(
                Severity::Error, cmpFirst->declLoc,
                "comparison operators must be overloaded as a complete group: "
                "`<`, `>`, `<=`, `>=` together (§25.2)");
        }
        if (eqFirst && !(eq && ne)) {
            ctx_.diagnostics().report(
                Severity::Error, eqFirst->declLoc,
                "the equality operators `=` and `<>` must be overloaded together "
                "(§25.2)");
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
    if (dynamic_cast<MethodSymbol*>(m) != nullptr ||
        dynamic_cast<ConstructorSymbol*>(m) != nullptr)
        return true;
    // §23.4 (#774): "Indexers may be overloaded by parameter type list." An
    // indexer is a PropertySymbol with isIndexer set; two indexers sharing the
    // synthetic `.this` name are a legal overload pair (distinguished by their
    // parameter types in checkTypeOverloads), not a duplicate member.
    if (auto* ps = dynamic_cast<PropertySymbol*>(m)) return ps->isIndexer;
    return false;
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

void Resolver::checkDuplicateTopLevelTypes() {
    if (!ctx_.module()) return;
    // §11.3 / §13.2 (#747 R42): a module may not declare two top-level types
    // with the same name. §11.3 states the clash rule for members and NESTED
    // types; the module is the top-level namespace and the same well-formedness
    // requirement applies — two same-named top-level types are unresolvable, and
    // silently letting the second declaration shadow the first is a soundness
    // hazard (e.g. `Foo.Val` reads the wrong type's static). The spec states the
    // member/nested case explicitly; this top-level extension is the obvious
    // reading (a candidate for a future spec clarification). Local decls only —
    // imported types live in a separate symbol list.
    std::unordered_map<std::string_view, Symbol*> seen;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->name.empty()) continue;
        auto it = seen.find(t->name);
        if (it == seen.end()) { seen[t->name] = t; continue; }
        SourceLocation loc = t->declLoc;
        if (t->decl) loc = t->decl->span.begin;  // point at the 2nd declaration
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("a top-level type named `") + std::string(t->name) +
            "` is already declared in this module — top-level type names must be "
            "unique (§11.3 well-formedness)");
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

void Resolver::buildBaseGenericSubst(
        MethodSymbol* derived, MethodSymbol* base,
        std::unordered_map<std::string_view, TypeRef*>& out) {
    // Example: Container<T> base, IntBox : Container<int> derived → {T → int}.
    // Look up the base type's TypeRef from the derived class's baseTypeRefs
    // (which store the resolved generic args).
    TypeSymbol* derivedClass = derived ? derived->parent : nullptr;
    TypeSymbol* baseClass = base ? base->parent : nullptr;
    if (!derivedClass || !baseClass) return;
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
                out[gp->name] = baseRef->genericArgs[gi];
            }
        }
        break;
    }
}

bool Resolver::overrideParamsMatch(MethodSymbol* derived, MethodSymbol* base) {
    if (!derived || !base) return false;
    if (derived->params.size() != base->params.size()) return false;
    std::unordered_map<std::string_view, TypeRef*> genericSubst;
    buildBaseGenericSubst(derived, base, genericSubst);
    for (std::size_t i = 0; i < derived->params.size(); ++i) {
        auto* dp = dynamic_cast<ParameterSymbol*>(derived->params[i]);
        auto* bp = dynamic_cast<ParameterSymbol*>(base->params[i]);
        if (!dp || !bp) return false;
        // A `ref`/`out`/`owned` modifier is part of the signature (§22.6) — an
        // override must match it, so a modifier difference means this is a
        // DIFFERENT overload, not the one being overridden.
        if (dp->modifier != bp->modifier) return false;
        TypeRef* dt = dp->declType ? ctx_.resolvedType(*dp->declType) : nullptr;
        TypeRef* bt = bp->declType ? ctx_.resolvedType(*bp->declType) : nullptr;
        TypeRef* btSubst = applyGenericSubst(bt, genericSubst);
        if (!typeRefEqual(dt, btSubst)) return false;
    }
    return true;
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
    std::unordered_map<std::string_view, TypeRef*> genericSubst;
    buildBaseGenericSubst(derived, base, genericSubst);

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
        // Gather base CLASS members by name (transitive). A name can map to
        // SEVERAL members — overloaded virtual methods (#1090) — so collect all
        // of them (most-derived first) and pick the matching overload below.
        std::unordered_map<std::string_view, std::vector<MemberSymbol*>>
            baseMembers;
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
                // per §11.1 / §22.10: a class primary-constructor parameter
                // is stored as a FieldSymbol with posParam != null and a Class
                // parent kind. Such a parameter is NOT a member of the class's
                // public API — it is an initialization-time value that may be
                // captured into a private field, but it is not a declared
                // member. It cannot be hidden or overridden (§12.2 applies to
                // MEMBERS; fix #680).
                if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                    if (fs->posParam && cur->kind == ast::TypeDeclKind::Class)
                        continue;
                }
                if (auto* ms = dynamic_cast<MemberSymbol*>(m)) {
                    // Collect every same-name base member (most-derived first);
                    // an overloaded method contributes one entry per overload.
                    baseMembers[ms->name].push_back(ms);
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
            // per §11.1 / §22.10 / fix #680: a class primary-ctor parameter
            // (FieldSymbol with posParam != null, Class parent) is not a
            // member for hiding purposes — skip it.
            if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                if (fs->posParam && t->kind == ast::TypeDeclKind::Class)
                    continue;
            }
            auto* ms = dynamic_cast<MemberSymbol*>(m);
            if (!ms || ms->name.empty()) continue;
            auto it = baseMembers.find(ms->name);
            if (it == baseMembers.end()) continue;
            const std::vector<MemberSymbol*>& bases = it->second;
            // Pick the base overload this member matches. For an overloaded
            // method, match by SIGNATURE so a derived override of one overload
            // is validated against the right base overload (not whichever was
            // first in declaration order) (#1090). Non-method members and a
            // single candidate use the most-derived (front) entry unchanged.
            MemberSymbol* base = bases.front();
            if (auto* dm = dynamic_cast<MethodSymbol*>(ms); dm && bases.size() > 1) {
                MemberSymbol* arityMatch = nullptr;
                for (MemberSymbol* cand : bases) {
                    auto* bm = dynamic_cast<MethodSymbol*>(cand);
                    if (!bm || bm->params.size() != dm->params.size()) continue;
                    if (!arityMatch) arityMatch = cand;
                    if (overrideParamsMatch(dm, bm)) { arityMatch = cand; break; }
                }
                if (arityMatch) base = arityMatch;
            }
            bool isOverride = hasMod(ms, "override");
            if (isOverride) {
                if (!baseMemberOverridable(base)) {
                    // Per §12.4: if the base member is a `sealed override`,
                    // the override IS in the virtual category but is locked
                    // against further overriding. Emit a sealed-specific
                    // diagnostic instead of the generic §12.1 message so the
                    // programmer knows WHY the override is rejected.
                    if (hasMod(base, "override") && hasMod(base, "sealed")) {
                        std::string baseCls = (base->parent && !base->parent->name.empty())
                            ? std::string(base->parent->name) : "base class";
                        ctx_.diagnostics().report(
                            Severity::Error, ms->declLoc,
                            std::string("cannot override `") +
                            std::string(ms->name) +
                            "`: it is declared `sealed` in `" +
                            baseCls + "` (§12.4)");
                    } else {
                        ctx_.diagnostics().report(
                            Severity::Error, ms->declLoc,
                            std::string("`") + std::string(ms->name) +
                            "` is declared `override` but the base "
                            "member it would override is not `virtual`, "
                            "`abstract`, or `override` (§12.1)");
                    }
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
        // R39 / §11.2 + §12.1: also track each abstract/interface method's
        // parameter COUNT and the set of parameter counts the class provides
        // for that name, so an implementation with the right name but the wrong
        // arity is rejected (it otherwise installs a wrong-signature function in
        // the v/itable → ABI-corrupt dispatch). Param count is invariant under
        // generic instantiation and default values, so this is sound without
        // any generic gating; full param-type/return matching is a follow-up.
        std::unordered_map<std::string_view, std::unordered_set<std::size_t>>
            abstractArities;
        std::unordered_map<std::string_view, std::unordered_set<std::size_t>>
            concreteArities;
        // §11.2 / §23: interface property names that require a property
        // implementation (a field does NOT satisfy a property contract — per
        // §23, a property is declared with the `property` keyword and
        // generates accessor functions; a field is a FieldDecl with no
        // getter/setter surface, so it cannot satisfy itable dispatch).
        std::unordered_set<std::string_view> ifacePropertyNames;
        // Property names (PropertySymbol) provided by class or base classes.
        std::unordered_set<std::string_view> concretePropertyNames;
        // R39 / §11.2: also remember the PropertySymbol (and, for interface
        // members, the declaring interface) so the implementing property's TYPE
        // can be checked against the interface's — return/property types must
        // match exactly (no covariance). Keyed by name; first writer wins, so
        // the most-derived class property (BFS starts at `t`) is kept.
        std::unordered_map<std::string_view,
                           std::pair<PropertySymbol*, TypeSymbol*>> ifaceProps;
        std::unordered_map<std::string_view, PropertySymbol*> concreteProps;
        // BFS the chain.
        std::vector<TypeSymbol*> frontier{t};
        std::unordered_set<TypeSymbol*> seen{t};
        std::size_t i = 0;
        while (i < frontier.size()) {
            TypeSymbol* cur = frontier[i++];
            bool curIsIface = (cur->kind == ast::TypeDeclKind::Interface);
            for (Symbol* m : cur->members) {
                if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                    if (methodIsAbstract(ms)) {
                        abstractNames.insert(ms->name);
                        abstractArities[ms->name].insert(ms->params.size());
                    } else {
                        concreteNames.insert(ms->name);
                        concreteArities[ms->name].insert(ms->params.size());
                    }
                }
                // §11.2 / §23 (#489): interface properties require a matching
                // PropertySymbol, not merely a same-named FieldSymbol.
                if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
                    if (ps->isIndexer) continue;  // indexers are separate
                    bool isStatic = false;
                    for (auto mod : ps->modifiers)
                        if (mod == "static") { isStatic = true; break; }
                    if (isStatic) continue;
                    if (curIsIface) {
                        // Record the interface's required property name.
                        ifacePropertyNames.insert(ps->name);
                        ifaceProps.emplace(ps->name, std::make_pair(ps, cur));
                    } else {
                        // A class PropertySymbol satisfies the contract.
                        concretePropertyNames.insert(ps->name);
                        concreteProps.emplace(ps->name, ps);
                    }
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
        // R39 / §11.2 + §12.1: the implementing method must match the abstract/
        // interface member's signature — at minimum its parameter COUNT. If the
        // class provides a method of the right name but no overload with the
        // required arity, the v/itable slot is filled by a wrong-signature
        // function and dispatch reads/writes the wrong stack/register layout
        // (ABI corruption, garbage results). Report it as a conformance error.
        for (auto& [name, arities] : abstractArities) {
            if (!concreteNames.count(name)) continue;  // missing name reported above
            auto cit = concreteArities.find(name);
            for (std::size_t arity : arities) {
                if (cit == concreteArities.end() || !cit->second.count(arity)) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("class `") + std::string(t->name) +
                        "` does not implement inherited member `" +
                        std::string(name) + "` with the required " +
                        std::to_string(arity) + "-parameter signature — the "
                        "implementing method's parameter count must match the "
                        "abstract/interface member exactly (§11.2 / §12.1)");
                }
            }
        }
        // §11.2 / §23 (#489): for each interface property, the implementing
        // class must provide a property (auto-property or block-form), NOT a
        // field. A FieldSymbol has no getter/setter function and cannot be
        // installed in the itable; calling through the interface would invoke a
        // null function pointer (SIGSEGV). Reject with a clear conformance
        // error instead.
        for (auto name : ifacePropertyNames) {
            if (!concretePropertyNames.count(name)) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("class `") + std::string(t->name) +
                    "` does not implement interface property `" +
                    std::string(name) +
                    "` — a field does not satisfy a property contract;"
                    " declare `public property " + std::string(name) +
                    ": T` (§11.2 / §23)");
            }
        }
        // R39 / §11.2 ("Return types must match exactly — no covariance"): a
        // provided property must have the SAME type the interface declares.
        // A mismatch installs a getter of the wrong return type into the itable
        // (e.g. an i32-returning `Name` for an interface `Name: string`); the
        // caller reads the i32 as a string pointer and dereferences it →
        // SIGSEGV. Checked only for fully NON-GENERIC conformance: a generic
        // interface member's type needs type-argument substitution before it
        // can be compared, which is deferred (so generic conformance is never
        // false-rejected here).
        if (t->genericParams.empty()) {
            auto propType = [&](PropertySymbol* ps) -> const TypeRef* {
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (!pd || !pd->type) return nullptr;
                return ctx_.resolvedType(*pd->type);
            };
            for (auto& [name, info] : ifaceProps) {
                PropertySymbol* ips = info.first;
                TypeSymbol* iface = info.second;
                if (!iface || !iface->genericParams.empty()) continue;
                auto cit = concreteProps.find(name);
                if (cit == concreteProps.end()) continue;  // missing → reported above
                const TypeRef* itype = propType(ips);
                const TypeRef* ctype = propType(cit->second);
                if (!itype || !ctype) continue;
                if (itype->kind == TypeRef::Kind::Error ||
                    ctype->kind == TypeRef::Kind::Error) continue;
                if (!typeRefEqual(itype, ctype)) {
                    ctx_.diagnostics().report(
                        Severity::Error, cit->second->declLoc,
                        std::string("property `") + std::string(name) +
                        "` implements interface `" + std::string(iface->name) +
                        "` with a different type than the interface declares — "
                        "property types must match exactly (no covariance) "
                        "(§11.2)");
                }
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
        // §14.2: non-nullable types only — `where T: notnull` excludes
        // EVERY nullable type, value or reference.
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
        // §14.2: non-nullable value types only. A nullable value type
        // (`int?`, `Priority?`, nullable record / enum / inline-array) is a
        // distinct type from its non-nullable form (§7.1) and does NOT
        // satisfy `value`; reject it regardless of underlying category. A
        // reference-typed argument is the other clear violation.
        if (arg->nullable) { report("value"); return; }
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
        // §14.2: an argument that is itself a generic parameter of the type
        // whose body we are resolving (e.g. `C<W>` inside `class Box<W>`)
        // cannot be classified here — W's identity is only known at Box's
        // concrete instantiation sites. Record the inner constraint as a
        // deferred obligation, attributed to the outer parameter (W) and its
        // owning type (Box), and re-check it after the whole pass once every
        // instantiation site is known (checkDeferredNestedConstraints).
        if (isGenericParamRef(*argAst, scope)) {
            recordDeferredNested(*argAst, *gp, *td, t);
            continue;
        }
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

void Resolver::recordDeferredNested(const ast::Type& argAst,
                                    const GenericParamSymbol& innerParam,
                                    const ast::TypeDecl& innerDecl,
                                    const ast::Type& innerSite) {
    // §14.2: only obligations parameterised by a generic param of the type
    // whose body we are resolving can be discharged by re-checking that
    // type's concrete instantiation sites. A *method*-level generic param
    // (or a stray ref with no owning type context) binds at the call site, a
    // separate mechanism — leave it deferred as before to avoid false joins.
    if (!currentOuterType_) return;
    std::string_view outerParam = argAst.name->parts.front();
    bool ownedByOuter = false;
    for (Symbol* gp : currentOuterType_->genericParams) {
        if (gp && gp->name == outerParam) { ownedByOuter = true; break; }
    }
    if (!ownedByOuter) return;

    // The inner constraints for `innerParam` are the obligations. Skip the
    // historical `is owned` / `is not owned` modifier form (§14.4) — it
    // carries no instantiation-time obligation, mirroring checkGenericConstraints.
    for (const auto& gc : innerDecl.genericConstraints) {
        if (gc.typeParam != innerParam.name) continue;
        if (!gc.modifierConstraint.empty()) continue;
        for (const auto& item : gc.items) {
            DeferredNestedObligation ob;
            ob.outerType = currentOuterType_;
            ob.outerParamName = outerParam;
            ob.innerParamName = innerParam.name;
            ob.item = &item;
            ob.innerSite = &innerSite;
            deferredNested_.push_back(ob);
        }
    }
}

void Resolver::checkDeferredNestedConstraints() {
    // §14.2: discharge each deferred inner obligation against the concrete
    // bindings the program supplies for it. Example: `class Box<W> ... C<W>
    // ...` with `class C<T> where T: new()` yields an obligation (outer=Box,
    // outerParam=W, item=new()). The instantiation `Box<IFoo>` binds W=IFoo,
    // so we check that IFoo satisfies `where T: new()` — rejecting the
    // indirect form exactly as the direct form `C<IFoo>` is rejected.
    //
    // Bindings may chain through several nesting generics: `Outer<X>` binds
    // `Box<X>`, and `Outer<IFoo>` binds X=IFoo, so the obligation on Box's W
    // re-keys to Outer's X and resolves at `Outer<IFoo>`. We handle this with
    // a worklist: a binding that is itself a generic parameter of the
    // enclosing type re-targets the obligation to that (type, param), and a
    // visited set keeps the closure finite.
    Scope boundModule;
    if (ctx_.module()) {
        for (Symbol* child : ctx_.module()->children) {
            if (!child->name.empty()) boundModule.names[child->name] = child;
        }
    }

    // The propagation key for an obligation: which (type, param) currently
    // carries it. The constraint item and inner-site span travel unchanged.
    struct Pending {
        TypeSymbol* outerType;
        std::string_view outerParamName;
        std::string_view innerParamName;
        const ast::ConstraintItem* item;
        const ast::Type* innerSite;
    };

    // A binding that is itself an in-scope generic parameter (not a primitive,
    // not a built-in, no resolved user declaration) — i.e. a chained
    // deferral that must re-key to the enclosing type's parameter.
    auto isGenericParamBinding = [&](TypeRef* bound,
                                     const ast::Type* boundAst) -> bool {
        return bound && boundAst && !bound->declaration &&
               bound->kind == TypeRef::Kind::Named &&
               boundAst->kind == ast::TypeKind::Named && boundAst->name &&
               boundAst->name->parts.size() == 1 &&
               primitiveFromName(boundAst->name->parts.front()) ==
                   PrimitiveType::None &&
               builtinTypeArity(boundAst->name->parts.front()) == kNotBuiltin;
    };

    std::vector<Pending> work;
    for (const DeferredNestedObligation& ob : deferredNested_) {
        if (ob.item && ob.outerType) {
            work.push_back({ob.outerType, ob.outerParamName,
                            ob.innerParamName, ob.item, ob.innerSite});
        }
    }

    // Visited set over (item, type, param) so a binding cycle (or a diamond
    // of instantiation sites) does not re-enqueue the same obligation.
    std::set<std::tuple<const ast::ConstraintItem*, TypeSymbol*,
                        std::string_view>>
        visited;

    while (!work.empty()) {
        Pending p = work.back();
        work.pop_back();
        if (!visited.insert({p.item, p.outerType, p.outerParamName}).second) {
            continue;
        }

        // Index of the carrying parameter (W) in its owning type (Box).
        std::size_t outerIdx = p.outerType->genericParams.size();
        for (std::size_t i = 0; i < p.outerType->genericParams.size(); ++i) {
            Symbol* gp = p.outerType->genericParams[i];
            if (gp && gp->name == p.outerParamName) { outerIdx = i; break; }
        }
        if (outerIdx >= p.outerType->genericParams.size()) continue;

        for (const GenericInstantiation& inst : instantiations_) {
            if (inst.type != p.outerType) continue;
            if (outerIdx >= inst.argRefs.size()) continue;
            TypeRef* bound = inst.argRefs[outerIdx];
            const ast::Type* boundAst = (outerIdx < inst.argAsts.size())
                                            ? inst.argAsts[outerIdx] : nullptr;
            if (!bound || !boundAst) continue;
            if (bound->kind == TypeRef::Kind::Error) continue;

            if (isGenericParamBinding(bound, boundAst)) {
                // Chained: the binding is a generic param of the body this
                // site sits in. Re-key the obligation to (enclosingType,
                // that param) and continue the closure. Without an enclosing
                // type the param cannot be a type-generic param we track, so
                // there is nothing further to discharge here.
                if (!inst.enclosingType) continue;
                work.push_back({inst.enclosingType,
                                boundAst->name->parts.front(),
                                p.innerParamName, p.item, p.innerSite});
                continue;
            }

            // Concrete binding: re-check the inner constraint against it. The
            // diagnostic points at the inner site (`C<W>`), naming the
            // concrete bound's spelling, so the message identifies which
            // nested instantiation is unsatisfiable. boundScope only needs
            // module names for type-bound resolution here.
            Scope boundScope;
            boundScope.parent = &boundModule;
            checkOneConstraint(*p.innerSite, p.innerParamName, *p.item,
                               boundAst, bound, &boundScope);
        }
    }
}

// per §32.5: the set of primitive types admissible at the FFI boundary —
// all integer types plus `float` and `double` and `bool`. `string`, `char`,
// and `decimal` are explicitly excluded (§32.5 prose: "Not allowed: Bezel
// `string` ... `char` ...").
static bool isFfiPrimitive(PrimitiveType p) {
    switch (p) {
        case PrimitiveType::Bool:
        case PrimitiveType::Byte:
        case PrimitiveType::SByte:
        case PrimitiveType::Short:
        case PrimitiveType::UShort:
        case PrimitiveType::Int:
        case PrimitiveType::UInt:
        case PrimitiveType::Long:
        case PrimitiveType::ULong:
        case PrimitiveType::Float:
        case PrimitiveType::Double:
            return true;
        // §32.5: excluded.
        case PrimitiveType::Char:
        case PrimitiveType::String:
        case PrimitiveType::Decimal:
        case PrimitiveType::Object:
        case PrimitiveType::None:
            return false;
    }
    return false;
}

// per §32.5: the FFI-boundary class/value types named in §32.5 and stdlib
// §10 (CString, WString, RawBytes, IntPtr, UIntPtr). These have no user
// TypeSymbol in the current front-end, so they are recognized by name on
// the AST. Inline-array element validation reuses the primitive check above.
static bool isFfiBoundaryNamedType(std::string_view n) {
    return n == "CString" || n == "WString" || n == "RawBytes" ||
           n == "IntPtr" || n == "UIntPtr";
}

// A best-effort source-like spelling of an AST type, for FFI diagnostics.
// Covers the shapes that can appear in an extern signature (§32.5); falls
// back to a generic label for anything else.
static std::string spellExternType(const ast::Type& t) {
    std::string out;
    switch (t.kind) {
        case ast::TypeKind::Unit:
            out = "()";
            break;
        case ast::TypeKind::Named:
            if (t.name && !t.name->parts.empty()) {
                for (std::size_t i = 0; i < t.name->parts.size(); ++i) {
                    if (i) out += ".";
                    out += std::string(t.name->parts[i]);
                }
            } else {
                out = "<type>";
            }
            break;
        case ast::TypeKind::InlineArray:
            out = t.arrayOwned ? "[N owned]" : "[N]";
            out += t.arrayElement ? spellExternType(*t.arrayElement) : "?";
            break;
        case ast::TypeKind::Function:
            out = "(...) -> ";
            out += t.funcReturn ? spellExternType(*t.funcReturn) : "()";
            break;
    }
    if (t.nullable) out += "?";
    return out;
}

bool Resolver::isFfiCompatibleType(const ast::Type& t, bool returnPosition) {
    switch (t.kind) {
        case ast::TypeKind::Unit:
            // §32.5: the unit type `()` is valid as a return type only.
            return returnPosition;
        case ast::TypeKind::Named: {
            if (!t.name || t.name->parts.size() != 1) return false;
            std::string_view n = t.name->parts.front();
            // §32.5: integer types, float, double, bool.
            PrimitiveType prim = primitiveFromName(n);
            if (prim != PrimitiveType::None) return isFfiPrimitive(prim);
            // §32.5 / stdlib §10: IntPtr, UIntPtr, CString, WString, RawBytes.
            // A nullable FFI-boundary type (`CString?`, e.g. `strdup`'s return
            // in the §32.1 example) is admitted — the nullability rides the
            // same pointer representation.
            if (isFfiBoundaryNamedType(n)) return true;
            // Anything else with a name — user classes/records/interfaces,
            // `Array<T>`, generic params, etc. — is not FFI-compatible (§32.5
            // "Not allowed: ... classes, records, interfaces, generic type
            // parameters, Array<T>, ...").
            return false;
        }
        case ast::TypeKind::InlineArray: {
            // §32.5: inline arrays `[N]T` where T is an FFI-compatible
            // primitive. Owning inline arrays (`[N owned]T`) are class-typed
            // and not admitted; their element type is never a bare primitive,
            // so the element check below already rejects them, but reject
            // explicitly for clarity.
            if (t.arrayOwned) return false;
            if (!t.arrayElement) return false;
            // The element must be an FFI-compatible *primitive* — §32.5 does
            // not admit inline arrays of class/record/inline-array elements.
            const ast::Type& elem = *t.arrayElement;
            if (elem.kind != ast::TypeKind::Named || !elem.name ||
                elem.name->parts.size() != 1)
                return false;
            PrimitiveType ep = primitiveFromName(elem.name->parts.front());
            return ep != PrimitiveType::None && isFfiPrimitive(ep);
        }
        case ast::TypeKind::Function: {
            // §32.5: function types whose parameters and return type are
            // themselves FFI-compatible.
            for (const auto& fp : t.funcParams) {
                if (!fp.type ||
                    !isFfiCompatibleType(*fp.type, /*returnPosition=*/false))
                    return false;
            }
            // The function's return slot is a return position (unit allowed).
            if (!t.funcReturn) return true;  // `-> ()` parsed as null return
            return isFfiCompatibleType(*t.funcReturn,
                                       /*returnPosition=*/true);
        }
    }
    return false;
}

bool Resolver::checkExternType(const ast::Type& t, bool returnPosition,
                               const std::string& declSpelling) {
    if (isFfiCompatibleType(t, returnPosition)) return true;
    // per §32.5: report the offending type and signature site. Use the
    // AST type's source span so the diagnostic points at the declaration.
    std::string what = spellExternType(t);
    ctx_.diagnostics().report(
        Severity::Error, t.span.begin,
        std::string("extern method ") + declSpelling + " has type `" + what +
            "`, which is not permitted at the FFI boundary (§32.5)");
    return false;
}

// ---------------------------------------------------------------------------
// §12.9 — Variance position enforcement (issue #431).
//
// For each interface's `out`-annotated (covariant) or `in`-annotated
// (contravariant) generic type parameters, verify that every direct
// occurrence of that parameter in the interface's member signatures is in
// the appropriate position:
//   out T (covariant)     → allowed only in OUTPUT positions:
//                              method return types, readonly-property types,
//                              `out`-parameter positions.
//   in T (contravariant)  → allowed only in INPUT positions:
//                              ordinary method parameters (not `out`-params).
//
// Nested positions (T inside a function-type parameter, T inside another
// generic's type arguments) are NOT checked here — position composition is
// deferred to a later slice. This keeps the check sound (no false positives)
// while eliminating the most common unsound declarations (direct param misuse).
//
// The check is purely over the raw AST (`ast::Type` nodes), because TypeRefs
// for generic param references carry no name (declaration == nullptr), and
// the AST-level `QualifiedName` is the only way to identify which parameter
// is referenced at each occurrence.
// ---------------------------------------------------------------------------

namespace {

// Return true iff the ast::Type `t` is a bare reference to the generic
// parameter named `paramName` — a single-part QualifiedName with no generic
// args, not nullable (nullable doesn't affect the position check, but the
// bare-name test is sufficient to isolate direct occurrences).
//
// This deliberately does NOT recurse into function-type params, return
// types, or generic type arguments — those compose positions in ways this
// check defers (see comment above).
bool isDirectParamRef(const ast::Type& t, std::string_view paramName) {
    if (t.kind != ast::TypeKind::Named) return false;
    if (!t.name || t.name->parts.size() != 1) return false;
    if (!t.genericArgs.empty()) return false;  // e.g. T<U> — not a bare param ref
    return t.name->parts.front() == paramName;
}

// Check whether `t` contains a direct reference to `paramName` AND that
// reference is in an invalid position given the declared variance.
//
// `paramVariance`  — "out" (covariant) or "in" (contravariant).
// `isOutputPos`    — true when `t` occurs in an output position (method
//                    return, readonly property, `out` parameter).
//
// Returns true iff a violation was detected at this direct level.
// (Nested positions inside function types or generic args are skipped.)
bool directVarianceViolation(const ast::Type& t,
                              std::string_view paramVariance,
                              bool isOutputPos,
                              std::string_view paramName) {
    if (!isDirectParamRef(t, paramName)) return false;
    if (paramVariance == "out" && !isOutputPos) return true;   // out T in input
    if (paramVariance == "in"  &&  isOutputPos) return true;   // in T in output
    return false;
}

}  // anonymous namespace

void Resolver::checkVariancePositions() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Interface) continue;
        // Collect variant generic params (those with "in" or "out").
        // For each, perform the member-by-member position check.
        for (Symbol* gp : t->genericParams) {
            auto* gps = dynamic_cast<GenericParamSymbol*>(gp);
            if (!gps || gps->variance.empty()) continue;  // invariant — skip
            const std::string_view paramName = gps->name;
            const std::string_view variance  = gps->variance;  // "in" or "out"

            // Walk each member of the interface.
            auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
            if (!td) continue;

            for (const auto& mem : td->members) {
                if (!mem) continue;

                // --- MethodDecl ---
                if (auto* md = dynamic_cast<const ast::MethodDecl*>(mem.get())) {
                    // Return type is an output position.
                    if (md->returnType &&
                        directVarianceViolation(*md->returnType, variance,
                                                /*isOutputPos=*/true, paramName)) {
                        ctx_.diagnostics().report(
                            Severity::Error, md->returnType->span.begin,
                            std::string("type parameter `") +
                            std::string(paramName) + "` is " +
                            (variance == "out" ? "covariant (`out`)" : "contravariant (`in`)") +
                            " but appears in an " +
                            (variance == "out" ? "input (parameter)" : "output (return)") +
                            " position in method `" + std::string(md->name) +
                            "` (§12.9)");
                    }
                    // Each parameter: check modifier for out-param vs. input.
                    for (const auto& p : md->params) {
                        if (!p.type) continue;
                        // `out` and `out owned` parameters are output positions.
                        bool paramIsOutput = (p.modifier == "out" ||
                                              p.modifier == "out owned");
                        if (directVarianceViolation(*p.type, variance,
                                                    paramIsOutput, paramName)) {
                            ctx_.diagnostics().report(
                                Severity::Error, p.type->span.begin,
                                std::string("type parameter `") +
                                std::string(paramName) + "` is " +
                                (variance == "out" ? "covariant (`out`)" : "contravariant (`in`)") +
                                " but appears in an " +
                                (paramIsOutput ? "output (out-param)" : "input (parameter)") +
                                " position in parameter `" + std::string(p.name) +
                                "` of method `" + std::string(md->name) +
                                "` (§12.9)");
                        }
                    }
                    continue;
                }

                // --- PropertyDecl ---
                if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(mem.get())) {
                    if (!pd->type) continue;
                    // A readonly property is a pure output position.
                    // A read-write property is both input and output — only
                    // `in T` is safe in the setter's implied input; `out T`
                    // is unsafe because the setter accepts T as input.
                    // We detect "readonly" by checking the `readonly` modifier.
                    bool isReadonly = false;
                    for (auto mod : pd->modifiers) {
                        if (mod == "readonly") { isReadonly = true; break; }
                    }
                    // A readonly property is an output position.
                    // A read-write property is an input+output position —
                    // treat it as an input position check only (the output
                    // direction is always safe for `out T`; the input
                    // direction blocks `out T`).
                    if (!isReadonly) {
                        // Read-write: setter is an input position — `out T` is unsafe.
                        if (variance == "out" &&
                            directVarianceViolation(*pd->type, variance,
                                                    /*isOutputPos=*/false, paramName)) {
                            ctx_.diagnostics().report(
                                Severity::Error, pd->type->span.begin,
                                std::string("type parameter `") +
                                std::string(paramName) +
                                "` is covariant (`out`) but appears in a "
                                "read-write property `" + std::string(pd->name) +
                                "`, which has a setter (input position) (§12.9)");
                        }
                        // `in T` in read-write property: the getter is an
                        // output position — flag as violation.
                        if (variance == "in" &&
                            directVarianceViolation(*pd->type, variance,
                                                    /*isOutputPos=*/true, paramName)) {
                            ctx_.diagnostics().report(
                                Severity::Error, pd->type->span.begin,
                                std::string("type parameter `") +
                                std::string(paramName) +
                                "` is contravariant (`in`) but appears in a "
                                "read-write property `" + std::string(pd->name) +
                                "`, which has a getter (output position) (§12.9)");
                        }
                    } else {
                        // Readonly property: pure output.
                        if (directVarianceViolation(*pd->type, variance,
                                                    /*isOutputPos=*/true, paramName)) {
                            ctx_.diagnostics().report(
                                Severity::Error, pd->type->span.begin,
                                std::string("type parameter `") +
                                std::string(paramName) + "` is " +
                                (variance == "out" ? "covariant (`out`)" : "contravariant (`in`)") +
                                " but appears in a readonly property `" +
                                std::string(pd->name) +
                                "` — readonly properties are output positions (§12.9)");
                        }
                    }
                    continue;
                }

                // --- FieldDecl: fields are input+output (invariant requirement).
                // An interface field would be unusual but is not expressly banned
                // by §11.2 (interface methods have no body; fields are not methods).
                // For safety, treat interface fields as invariant positions — flag
                // any directly-referenced variant param.
                if (auto* fd = dynamic_cast<const ast::FieldDecl*>(mem.get())) {
                    if (!fd->type) continue;
                    // A field is both readable and writable → both input and output.
                    // `out T` in an input context is a violation.
                    if (variance == "out" &&
                        directVarianceViolation(*fd->type, variance,
                                                /*isOutputPos=*/false, paramName)) {
                        ctx_.diagnostics().report(
                            Severity::Error, fd->type->span.begin,
                            std::string("type parameter `") +
                            std::string(paramName) +
                            "` is covariant (`out`) but appears in field `" +
                            std::string(fd->name) +
                            "` (invariant position — §12.9)");
                    }
                    // `in T` in an output context is a violation.
                    if (variance == "in" &&
                        directVarianceViolation(*fd->type, variance,
                                                /*isOutputPos=*/true, paramName)) {
                        ctx_.diagnostics().report(
                            Severity::Error, fd->type->span.begin,
                            std::string("type parameter `") +
                            std::string(paramName) +
                            "` is contravariant (`in`) but appears in field `" +
                            std::string(fd->name) +
                            "` (invariant position — §12.9)");
                    }
                    continue;
                }
                // Other member kinds (event, constructor, nested type, etc.)
                // don't carry T in positions we need to check for interfaces.
            }
        }
    }
}

// §9.8 (issue #550): serialization-eligibility check.
//
// A type that declares `: ISerializable` or `: IDeserializable` and is NOT
// serialization-eligible (by §9.8 rules) is a compile error at the conformance
// declaration.  A type that has a hand-written `Serialize` (resp. `Deserialize`)
// method suppresses synthesis on that direction — it is always valid regardless
// of eligibility, because the hand-written body is the contract (§9.8).
//
// Eligible component types (§9.8 — tier 2):
//   - Primitive types (bool, char, the integer types, float, double, decimal)
//   - string
//   - An enum type
//   - A positional record or auto-equality-bearing body record that itself
//     declares ISerializable/IDeserializable (recursive eligibility)
//   - A class with a `serialized tree`/`graph`/`cyclic` clause (§9.8 v0.136)
//   - A nullable T? whose T is an eligible component type
//   - A Shared<T> or Weak<T> whose element T is an eligible component type
//     (§9.8 v0.141): Shared<T> serializes as an identity participant
//     ($id/$ref), Weak<T> as a back-reference ($ref)
// NOT eligible:
//   - A class without a `serialized tree`/`graph`/`cyclic` clause
//   - A record that does not itself declare ISerializable/IDeserializable
//   - Any other Named type (interface, etc.)
//   - Function, inline-array, unit types
//
// A field marked `serialized skip` is excluded from the component set and does
// not disqualify the type (§9.8).

// Check whether a TypeRef is a serialization-eligible COMPONENT type (§9.8).
// `visited` prevents infinite recursion on mutually-referencing eligible records.
static bool isSerializationEligibleType(
        const TypeRef* tr,
        std::unordered_set<const TypeSymbol*>& visited) {
    if (!tr) return false;
    using K = TypeRef::Kind;
    // Nullable T?: eligible when T is eligible (§9.8).
    if (tr->nullable) {
        // Peel the nullable wrapper and check the inner type.
        TypeRef inner = *tr;
        inner.nullable = false;
        return isSerializationEligibleType(&inner, visited);
    }
    if (tr->kind == K::Primitive) {
        // All primitives except Object are eligible.
        using P = PrimitiveType;
        switch (tr->primitive) {
            case P::Bool:
            case P::Char:
            case P::String:
            case P::Byte: case P::SByte:
            case P::Short: case P::UShort:
            case P::Int: case P::UInt:
            case P::Long: case P::ULong:
            case P::Float: case P::Double:
            case P::Decimal:
                return true;
            default:
                return false;
        }
    }
    if (tr->kind != K::Named) return false;

    // §9.8 (v0.141): Shared<T> and Weak<T> are serialization-eligible component
    // types when their element type T is eligible.  Shared<T> serializes as an
    // identity participant ($id on first encounter, $ref on aliases, keyed on
    // the underlying value); Weak<T> serializes as a back-reference ($ref).
    // Both are builtin Named types: `declaration` is null and the type name is
    // carried in `astNode->name` (locally) or `importedName` (cross-module
    // metadata).  Recurse on the element type argument (genericArgs[0]).
    if (!tr->declaration) {
        std::string_view bname;
        if (tr->astNode && tr->astNode->name &&
            !tr->astNode->name->parts.empty()) {
            bname = tr->astNode->name->parts[0];
        } else if (!tr->importedName.empty()) {
            bname = tr->importedName;
        }
        if ((bname == "Shared" || bname == "Weak") &&
            !tr->genericArgs.empty() && tr->genericArgs[0]) {
            return isSerializationEligibleType(tr->genericArgs[0], visited);
        }
    }

    const TypeSymbol* ts = tr->declaration;
    if (!ts) return false;

    // Enum: always eligible (§9.8).
    if (ts->kind == ast::TypeDeclKind::Enum) return true;

    // Record: eligible when it itself declares ISerializable/IDeserializable
    // (the same eligibility contract it must satisfy — the recursive component
    // check). Guard against cycles.
    if (ts->kind == ast::TypeDeclKind::Record) {
        if (visited.count(ts)) return true;  // assume eligible (cycle = eligible set)
        auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
        if (!td) return false;
        bool declaredISerializable = false;
        bool declaredIDeserializable = false;
        for (const auto& bt : td->baseTypes) {
            if (!bt || !bt->name || bt->name->parts.empty()) continue;
            auto n = bt->name->parts.back();
            if (n == "ISerializable")   declaredISerializable = true;
            if (n == "IDeserializable") declaredIDeserializable = true;
        }
        if (!declaredISerializable && !declaredIDeserializable) return false;
        visited.insert(ts);
        // Verify the component types of the nested record are themselves eligible.
        // (This is the recursive eligibility: a nested record must be FULLY
        // eligible, not just declared conformant.)
        // For tier 1, trust the declaration — if it declared ISerializable then
        // the compiler will check IT at its own conformance declaration.
        // This avoids deep recursion and mirrors how auto-equality treats nested
        // records (it trusts them, checks each at its own declaration site).
        return true;
    }

    // Class: NOT eligible in tier 1 unless it has a serialized-class clause.
    // A class without such a clause is not a "serialization-eligible synthesized
    // class" per §9.8. (Tier 1 scope: tree shape is supported; graph/cyclic not.)
    if (ts->kind == ast::TypeDeclKind::Class) {
        auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
        if (td && td->serializedClass) return true;  // has serialized-class clause
        return false;
    }

    return false;
}

// §9.8: return true if the TypeSymbol `t` has a user-written method of the
// given name.  This is the suppression check (§9.8): "Hand-writing `Serialize`
// (respectively a recognized static `Deserialize`) suppresses synthesis."
static bool typeHasHandWrittenMethod(const TypeSymbol* t, std::string_view name) {
    for (const Symbol* m : t->members) {
        const auto* ms = dynamic_cast<const MethodSymbol*>(m);
        if (ms && ms->name == name) return true;
    }
    return false;
}

// §9.8: does the TypeDecl's base-type list mention the given interface name?
// ISerializable/IDeserializable are auto-imported built-ins with no TypeSymbol;
// we match by the last name part, identical to the IDisposable check above.
static bool declaredConformance(const ast::TypeDecl& td,
                                std::string_view ifaceName) {
    for (const auto& bt : td.baseTypes) {
        if (!bt || !bt->name || bt->name->parts.empty()) continue;
        if (bt->name->parts.back() == ifaceName) return true;
    }
    return false;
}

// §9.8 / `serialized cyclic`: check whether an expression tree contains a call
// to an instance method on `this` (i.e. `this.SomeMethod(args)` pattern).
// Returns true iff such a call is found.
static bool exprContainsThisMethodCall(const ast::Expr& e);
static bool stmtsContainThisMethodCall(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts);

static bool exprContainsThisMethodCall(const ast::Expr& e) {
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) {
            auto* mae = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get());
            if (mae && mae->target) {
                auto* ide = dynamic_cast<const ast::IdentifierExpr*>(mae->target.get());
                if (ide && ide->name == "this") return true;
            }
        }
        for (const auto& arg : ce->args)
            if (arg.value && exprContainsThisMethodCall(*arg.value)) return true;
    }
    if (auto* mae = dynamic_cast<const ast::MemberAccessExpr*>(&e))
        if (mae->target && exprContainsThisMethodCall(*mae->target)) return true;
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left  && exprContainsThisMethodCall(*be->left))  return true;
        if (be->right && exprContainsThisMethodCall(*be->right)) return true;
    }
    return false;
}

static bool stmtsContainThisMethodCall(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s.get()))
            if (es->expr && exprContainsThisMethodCall(*es->expr)) return true;
        if (auto* ls = dynamic_cast<const ast::LocalDecl*>(s.get()))
            if (ls->initializer && exprContainsThisMethodCall(*ls->initializer)) return true;
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s.get()))
            if (as->value && exprContainsThisMethodCall(*as->value)) return true;
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get()))
            if (rs->value && exprContainsThisMethodCall(*rs->value)) return true;
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (ifs->ifBranch.condition &&
                exprContainsThisMethodCall(*ifs->ifBranch.condition)) return true;
            if (stmtsContainThisMethodCall(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches) {
                if (el.condition && exprContainsThisMethodCall(*el.condition)) return true;
                if (stmtsContainThisMethodCall(el.body)) return true;
            }
            if (stmtsContainThisMethodCall(ifs->elseBody)) return true;
        }
    }
    return false;
}

// §9.8: the serialized name for a positional field — the `serialized "X"` clause
// if present, else the declared identifier.  Returns the clause name if set and
// non-empty, else the field name.  Also returns whether this field is skip.
static bool positionalFieldIsSkip(const ast::Param& pf) {
    return pf.serialized && pf.serialized->isSkip;
}

void Resolver::checkSerializationEligibility() {
    if (!ctx_.module()) return;

    // §9.8 v0.141 (#703): PRE-PASS — synthesize a graph-shape
    // SerializedClassClause for every class that declares
    // `: ISerializable`/`: IDeserializable` without a keyword clause (and without
    // hand-written Serialize/Deserialize, which suppresses synthesis on that
    // side). The keyword clause was retired; a declaration drives synthesis, and
    // field types drive the wire shape. This MUST run before the eligibility
    // checks below: a class-typed component (owned, or a borrow back-edge) whose
    // target is ANOTHER keyword-less serializable class needs that target's
    // clause already set, regardless of declaration order (forward references).
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Class) continue;
        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) continue;
        // A clause with a shape already set (a real keyword clause during the
        // additive transition) is left alone. A clause with only polyCases (set
        // by the `polymorphic` member clause, no shape) still needs its shape
        // derived below.
        if (td->serializedClass && !td->serializedClass->shape.empty()) continue;
        bool wantsIS = declaredConformance(*td, "ISerializable");
        bool wantsID = declaredConformance(*td, "IDeserializable");
        if (!wantsIS && !wantsID) continue;
        bool hwS = typeHasHandWrittenMethod(t, "Serialize");
        bool hwD = typeHasHandWrittenMethod(t, "Deserialize");
        bool needsSer   = wantsIS && !hwS;
        bool needsDeser = wantsID && !hwD;
        if (!needsSer && !needsDeser) continue;  // fully hand-written

        // §9.8 v0.141: the wire shape is driven by field types. "graph" when the
        // class has an identity participant (Shared<T>) or a back-reference
        // (Weak<T>, or a non-owned class-typed borrow field) — those need the
        // identity-tag machinery. "tree" otherwise (all owned/value components
        // serialize inline, which is null-safe for nullable owned children;
        // graph-mode owned-tagging is not). This matches the legacy keyword
        // behavior per component kind without a user-written clause.
        // A polymorphic registry (P6 member clause) also implies the graph shape.
        bool needsGraph = td->serializedClass &&
                          !td->serializedClass->polyCases.empty();
        for (const Symbol* m : t->members) {
            auto* fs = dynamic_cast<const FieldSymbol*>(m);
            if (!fs || !fs->decl) continue;
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) continue;
            bool isStatic = false, isOwned = false;
            for (auto mod : fd->modifiers) {
                if (mod == "static") isStatic = true;
                if (mod == "owned")  isOwned  = true;
            }
            if (isStatic) continue;
            const TypeRef* tr = ctx_.resolvedType(*fd->type);
            if (!tr) continue;
            const TypeRef* inner = tr;
            TypeRef peeled;
            if (inner->nullable) { peeled = *inner; peeled.nullable = false; inner = &peeled; }
            if (inner->kind != TypeRef::Kind::Named) continue;
            // Shared<T>/Weak<T>: builtin Named (declaration null), name in astNode.
            if (!inner->declaration && inner->astNode && inner->astNode->name &&
                !inner->astNode->name->parts.empty()) {
                auto n = inner->astNode->name->parts[0];
                if (n == "Shared" || n == "Weak") { needsGraph = true; break; }
            }
            // Non-owned class-typed field = borrow back-edge.
            if (inner->declaration &&
                inner->declaration->kind == ast::TypeDeclKind::Class && !isOwned) {
                needsGraph = true; break;
            }
        }
        static const std::string kGraphShape = "graph";
        static const std::string kTreeShape  = "tree";
        auto* mutTd = const_cast<ast::TypeDecl*>(td);
        if (!mutTd->serializedClass) {
            // No clause yet (no `polymorphic` member): synthesize one.
            mutTd->serializedClass = std::make_unique<ast::SerializedClassClause>();
        }
        // Set the derived shape (the member clause leaves it empty).
        mutTd->serializedClass->shape = needsGraph ? kGraphShape : kTreeShape;
    }

    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;

        // §9.8 applies to records and enums that declare ISerializable/
        // IDeserializable.  Classes are checked only when they carry an
        // explicit serialized-class clause — tier 1 does not synthesize class
        // serialization but we still emit the ineligibility error if declared.
        bool isRecord = t->kind == ast::TypeDeclKind::Record;
        bool isEnum   = t->kind == ast::TypeDeclKind::Enum;
        bool isClass  = t->kind == ast::TypeDeclKind::Class;
        if (!isRecord && !isEnum && !isClass) continue;

        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) continue;

        bool wantsIS  = declaredConformance(*td, "ISerializable");
        bool wantsID  = declaredConformance(*td, "IDeserializable");
        if (!wantsIS && !wantsID) continue;

        // Enums are always serialization-eligible (§9.8).
        if (isEnum) continue;  // no further check needed

        // Classes: §9.8 tier-2 eligibility checking.
        if (isClass) {
            if (!td->serializedClass) {
                // §9.8 v0.141: after the synthesis pre-pass at the top of this
                // function, a class with no clause here is fully hand-written
                // (Serialize + Deserialize) — it satisfies the conformance
                // without synthesis, so there is nothing to check.
                continue;
            }

            // Class has a serialized-class clause. §9.8: eligibility requires
            // every serialized component to itself be serialization-eligible.
            // The serialized components of a class are its owned and value-typed
            // fields and auto-properties in declaration order (§9.8).
            // Per §9.8 v0.136: a borrowing (non-owned) class-typed field is a
            // back-edge — eligible only under serialized graph/cyclic (tier 3+).
            // For tier 2 (serialized tree), only owned and value-typed components
            // are admitted.
            //
            // Hand-written suppression: if the class has hand-written Serialize
            // AND hand-written Deserialize, no synthesis occurs — skip check.
            bool hwS2 = typeHasHandWrittenMethod(t, "Serialize");
            bool hwD2 = typeHasHandWrittenMethod(t, "Deserialize");
            if (hwS2 && hwD2) continue;  // fully hand-written: always valid

            // Check: ineligible polymorphic registered types (§9.8).
            // A registered type that is not serialization-eligible is a compile
            // error at the registry.  Also: each registered type must be
            // assignable to (a subtype of) the declaring base class (§9.8:
            // "each named type must be assignable to the declaring class").
            const auto& scc = *td->serializedClass;
            bool polyError = false;

            // BFS subtype predicate: is `sub` a subtype of `base`?
            // Used to enforce §9.8 assignability.
            auto isSubtypeOf = [](const TypeSymbol* sub,
                                  const TypeSymbol* base) -> bool {
                if (!sub || !base) return false;
                std::vector<const TypeSymbol*> frontier;
                frontier.push_back(sub);
                std::unordered_set<const TypeSymbol*> seen;
                std::size_t i = 0;
                while (i < frontier.size()) {
                    const TypeSymbol* cur = frontier[i++];
                    if (!seen.insert(cur).second) continue;
                    if (cur == base) return true;
                    for (TypeSymbol* b : cur->baseTypes) {
                        if (b) frontier.push_back(b);
                    }
                }
                return false;
            };

            for (const auto& pc : scc.polyCases) {
                if (!pc.type) continue;
                const TypeRef* pcTr = ctx_.resolvedType(*pc.type);
                if (!pcTr || !pcTr->declaration) continue;
                const TypeSymbol* pcTs = pcTr->declaration;
                auto* pcTd = dynamic_cast<const ast::TypeDecl*>(pcTs->decl);
                if (!pcTd || !pcTd->serializedClass) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "class `" + std::string(t->name) + "` polymorphic "
                        "registry: registered type `" + std::string(pcTs->name) +
                        "` has no `serialized tree`/`graph`/`cyclic` clause and "
                        "is not serialization-eligible (§9.8); add a graph-shape "
                        "clause to the registered type");
                    polyError = true;
                    continue;
                }
                // §9.8: "each named type must be assignable to the declaring class"
                if (!isSubtypeOf(pcTs, t)) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "class `" + std::string(t->name) + "` polymorphic "
                        "registry: registered type `" + std::string(pcTs->name) +
                        "` is not a subtype of `" + std::string(t->name) +
                        "` (§9.8 — each registered type must be assignable to "
                        "the declaring class)");
                    polyError = true;
                }
            }

            // §9.8: each discriminator must be a unique non-empty string literal
            // within the clause.
            if (!scc.polyCases.empty()) {
                std::unordered_set<std::string> seenDiscs;
                for (const auto& pc : scc.polyCases) {
                    if (pc.discriminator.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "class `" + std::string(t->name) + "` polymorphic "
                            "registry: discriminator must be a non-empty string "
                            "literal (§9.8)");
                        polyError = true;
                        continue;
                    }
                    std::string disc(pc.discriminator);
                    if (!seenDiscs.insert(disc).second) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "class `" + std::string(t->name) + "` polymorphic "
                            "registry: discriminator \"" + disc + "\" is not "
                            "unique within the clause (§9.8); each discriminator "
                            "must appear exactly once");
                        polyError = true;
                    }
                }
            }

            if (polyError) continue;   // skip component checks on registry errors

            // Check component eligibility for owned/value fields and auto-props.
            std::unordered_set<const TypeSymbol*> visited;
            visited.insert(t);

            auto reportIneligible = [&](std::string_view memberName,
                                        std::string_view reason) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    "class `" + std::string(t->name) + "` declares `ISerializable`/"
                    "`IDeserializable` but is not serialization-eligible (§9.8): "
                    "field `" + std::string(memberName) + "` " +
                    std::string(reason) + "; remove the conformance declaration, "
                    "mark the field `serialized skip`, make the field type "
                    "serializable, or hand-write `Serialize`/`Deserialize`");
            };

            // Check owned class-typed fields and auto-properties whose types
            // are class-typed.  A non-owned (borrowing) class-typed field is a
            // back-edge; back-edges require tier-3 (serialized graph/cyclic back-
            // edge serialization) and are not yet implemented.
            bool foundBackEdge = false;
            for (const Symbol* m : t->members) {
                // Fields (FieldDecl).
                auto* fs = dynamic_cast<const FieldSymbol*>(m);
                if (fs) {
                    auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
                    if (!fd) continue;
                    // Check static: FieldDecl.modifiers contains "static".
                    bool isStat = false;
                    bool isOwned = false;
                    for (auto mod : fd->modifiers) {
                        if (mod == "static") isStat = true;
                        if (mod == "owned")  isOwned = true;
                    }
                    if (isStat) continue;
                    const TypeRef* tr = ctx_.resolvedType(*fd->type);
                    if (!tr) continue;
                    // §9.8 tier-boundary: a non-owned class-typed field is a
                    // back-edge; back-edge serialization is tier 3 (not yet
                    // implemented — issue #550 tier 3).
                    bool fieldIsClass = (tr->kind == TypeRef::Kind::Named &&
                                        tr->declaration &&
                                        tr->declaration->kind ==
                                            ast::TypeDeclKind::Class);
                    // Nullable T?: peel and check inner.
                    bool fieldIsNullableClass = false;
                    if (!fieldIsClass && tr->nullable) {
                        TypeRef inner = *tr; inner.nullable = false;
                        fieldIsNullableClass =
                            inner.kind == TypeRef::Kind::Named &&
                            inner.declaration &&
                            inner.declaration->kind == ast::TypeDeclKind::Class;
                    }
                    if ((fieldIsClass || fieldIsNullableClass) && !isOwned) {
                        // §9.8 / tier 3: back-edge fields (non-owned class-typed
                        // fields) are serialization-eligible under `serialized graph`
                        // and `serialized cyclic` — they are written as tag references
                        // and resolved from the tag→object table on deserialize.
                        // Under `serialized tree`, aliasing is forbidden (back-edges
                        // violate the tree assertion), so they remain an error there.
                        if (scc.shape == "tree") {
                            ctx_.diagnostics().report(
                                Severity::Error, t->declLoc,
                                "class `" + std::string(t->name) + "` (`serialized "
                                "tree`): field `" + std::string(fs->name) +
                                "` is a non-owned (borrowing) class-typed field "
                                "(a back-edge); `serialized tree` forbids aliasing — "
                                "mark the field `owned`, declare `serialized skip`, "
                                "or change the clause to `serialized graph`");
                            foundBackEdge = true;
                            break;
                        }
                        // Under `serialized graph` / `serialized cyclic`:
                        // back-edge is eligible (§9.8 tier 3 implemented).
                        // Verify the target type has a graph-shape clause.
                        const TypeSymbol* beDecl = tr->declaration;
                        if (!beDecl && tr->nullable) beDecl = tr->declaration;
                        if (beDecl) {
                            auto* beTd = dynamic_cast<const ast::TypeDecl*>(beDecl->decl);
                            if (!beTd || !beTd->serializedClass) {
                                ctx_.diagnostics().report(
                                    Severity::Error, t->declLoc,
                                    "class `" + std::string(t->name) +
                                    "` (`serialized " + std::string(scc.shape) +
                                    "`): back-edge field `" + std::string(fs->name) +
                                    "` has type `" + std::string(beDecl->name) +
                                    "` which has no `serialized tree`/`graph`/`cyclic` "
                                    "clause; the back-edge target must itself be "
                                    "serialization-eligible (§9.8)");
                                foundBackEdge = true;
                                break;
                            }
                        }
                        // Valid back-edge under graph/cyclic — continue.
                        continue;
                    }
                    if (!isSerializationEligibleType(tr, visited)) {
                        reportIneligible(fs->name,
                            "has a type that is not serialization-eligible");
                        break;
                    }
                    continue;
                }
                // Auto-properties (PropertySymbol with auto backing).
                if (!isAutoPropertySymbol(m)) continue;
                auto* ps = dynamic_cast<const PropertySymbol*>(m);
                if (!ps) continue;
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (!pd) continue;
                if (pd->serialized && pd->serialized->isSkip) continue;
                // §2.15 v0.139: `serialized "xxx"` rename beginning with `$` reserved.
                if (pd->serialized && !pd->serialized->name.empty() &&
                    pd->serialized->name.front() == '$') {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "class `" + std::string(t->name) + "`: `serialized` "
                        "rename `" + std::string(pd->serialized->name) +
                        "` for property `" + std::string(m->name) +
                        "` begins with `$`, which is reserved for JSON "
                        "graph-identity tokens (§2.15 v0.139); choose a name "
                        "that does not start with `$`");
                    break;
                }
                if (!pd->type) continue;
                const TypeRef* tr = ctx_.resolvedType(*pd->type);
                if (!tr) continue;
                if (!isSerializationEligibleType(tr, visited)) {
                    reportIneligible(m->name,
                        "has a type that is not serialization-eligible");
                    break;
                }
            }
            if (foundBackEdge) continue;

            // §9.8 / `serialized cyclic`: the constructor must be self-contained.
            // Self-contained: assigns own fields from constructor params/value fields
            // only.  MUST NOT: (1) read a back-edge field; (2) call any instance
            // method on `this`; (3) let `this` escape; (4) call virtual/abstract.
            // We check rule (2) — calling an instance method on `this` — which is
            // detectable via a simple AST walk.
            if (scc.shape == "cyclic") {
                // Walk constructor bodies looking for `this.SomeMethod()` calls.
                for (const Symbol* m : t->members) {
                    auto* cs = dynamic_cast<const sema::ConstructorSymbol*>(m);
                    if (!cs || cs->isStatic) continue;
                    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                    if (!cd) continue;

                    if (stmtsContainThisMethodCall(cd->body)) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "class `" + std::string(t->name) + "` (`serialized "
                            "cyclic`) constructor is not self-contained (§9.8): "
                            "it calls an instance method on `this`, which is "
                            "forbidden; the constructor may only assign own "
                            "fields from constructor parameters and own value "
                            "fields (§9.8, rule 2)");
                        break;
                    }
                }
            }

            // §9.8 v0.136 constructor-selection check (applies when synthesis of
            // Deserialize is needed — i.e. !hwD2).
            // Rule: there must be exactly one non-static constructor whose
            // parameter names match exactly the set of serialized component
            // declared identifiers (bijection, order-insensitive, case-
            // insensitive).  `serialized "<name>"` renames do NOT affect the
            // match — which uses declared identifiers only (§9.8 v0.136).
            // Case-insensitivity: Bezel convention is PascalCase fields and
            // camelCase parameters; `id` and `Id` are treated as the same
            // component name in the synthesized-Deserialize match.
            //
            // §9.8 tier-4 exception: a class with a non-empty `polymorphic`
            // registry is a polymorphic base.  Its synthesized Deserialize is
            // a pure dispatcher — it reads $type and delegates to the registered
            // concrete type's Deserialize; it does NOT invoke any constructor on
            // the base class itself.  Therefore the constructor-matching check
            // does not apply to the declaring base.  Skip it.
            if (!scc.polyCases.empty()) { /* skip ctor check for poly base */ }
            else if (!hwD2) {
                // Helper: fold to lowercase (ASCII).
                auto toLower = [](std::string_view sv) {
                    std::string r(sv);
                    for (char& c : r)
                        c = (char)std::tolower((unsigned char)c);
                    return r;
                };

                // Collect the lowercased declared identifiers of ALL serialized
                // components — including `serialized skip` ones.  Skip
                // components are still passed to the constructor as default(T),
                // so they count toward the parameter-name match.  What `skip`
                // means is only that the value is not read/written on the wire;
                // the param name must still appear in the ctor (§9.8 v0.136).
                // For field members: FieldDecl.name (non-static).
                // For auto-property members: the member name (all, skip or not).
                std::unordered_set<std::string> compNamesLower;
                std::vector<std::string> compNamesOrig;  // for error message
                for (const Symbol* m : t->members) {
                    auto* fs = dynamic_cast<const FieldSymbol*>(m);
                    if (fs) {
                        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
                        if (!fd) continue;
                        bool isStat = false;
                        for (auto mod : fd->modifiers)
                            if (mod == "static") { isStat = true; break; }
                        if (isStat) continue;
                        compNamesLower.insert(toLower(fd->name));
                        compNamesOrig.push_back(std::string(fd->name));
                        continue;
                    }
                    if (!isAutoPropertySymbol(m)) continue;
                    auto* ps = dynamic_cast<const PropertySymbol*>(m);
                    if (!ps) continue;
                    // Include skip-marked auto-properties: skip means "don't
                    // serialize" but the component still appears in the ctor.
                    compNamesLower.insert(toLower(m->name));
                    compNamesOrig.push_back(std::string(m->name));
                }

                // Collect non-static constructors and check which ones match.
                std::vector<std::string> matchingCtors;  // for error message
                std::vector<std::string> allCtors;
                for (const Symbol* m : t->members) {
                    auto* cs = dynamic_cast<const sema::ConstructorSymbol*>(m);
                    if (!cs || cs->isStatic) continue;
                    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                    if (!cd) continue;

                    // Build a string representation for the error message.
                    std::string ctorDesc = "constructor(";
                    for (unsigned pi = 0; pi < cd->params.size(); ++pi) {
                        if (pi) ctorDesc += ", ";
                        ctorDesc += std::string(cd->params[pi].name);
                    }
                    ctorDesc += ")";
                    allCtors.push_back(ctorDesc);

                    // Match: param count == component count AND every param
                    // name (lowercased) appears in compNamesLower (bijection).
                    if (cd->params.size() != compNamesLower.size()) continue;
                    bool allMatch = true;
                    for (const auto& p : cd->params) {
                        if (compNamesLower.find(toLower(p.name)) ==
                                compNamesLower.end()) {
                            allMatch = false;
                            break;
                        }
                    }
                    if (allMatch) matchingCtors.push_back(ctorDesc);
                }

                if (matchingCtors.size() != 1) {
                    // Build candidate list for the error message.
                    std::string candidates;
                    const auto& list = allCtors.empty() ? matchingCtors : allCtors;
                    for (const auto& c : list) {
                        if (!candidates.empty()) candidates += ", ";
                        candidates += c;
                    }
                    if (candidates.empty()) candidates = "(none)";
                    // Build component-set string for the error message.
                    std::string compSet;
                    for (const auto& n : compNamesOrig) {
                        if (!compSet.empty()) compSet += ", ";
                        compSet += n;
                    }
                    if (matchingCtors.size() == 0) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "class `" + std::string(t->name) + "` declares "
                            "`IDeserializable` (`serialized " +
                            std::string(scc.shape) +
                            "`): no constructor has parameter names matching "
                            "the serialized component set {" + compSet +
                            "} — candidates: " + candidates +
                            " (§9.8 v0.136: constructor selection is by name, "
                            "case-insensitive, order-insensitive)");
                    } else {
                        // >1 match
                        std::string matchList;
                        for (const auto& c : matchingCtors) {
                            if (!matchList.empty()) matchList += ", ";
                            matchList += c;
                        }
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "class `" + std::string(t->name) + "` declares "
                            "`IDeserializable` (`serialized " +
                            std::string(scc.shape) +
                            "`): more than one constructor matches the serialized "
                            "component set — ambiguous: " + matchList +
                            " (§9.8 v0.136: the unique matching constructor is "
                            "required)");
                    }
                }
            }
            continue;
        }

        // Record: check hand-written suppression and then eligibility.
        // §9.8: "Hand-writing `Serialize` suppresses synthesis of that direction."
        bool hasHandWrittenSerialize   = typeHasHandWrittenMethod(t, "Serialize");
        bool hasHandWrittenDeserialize = typeHasHandWrittenMethod(t, "Deserialize");

        // For ISerializable: need to check eligibility unless hand-written.
        bool needsSerializeCheck   = wantsIS  && !hasHandWrittenSerialize;
        bool needsDeserializeCheck = wantsID  && !hasHandWrittenDeserialize;
        if (!needsSerializeCheck && !needsDeserializeCheck) continue;

        // Collect the serialized components of this record (§9.8).
        // For a positional record: the positional fields (minus skip-marked ones).
        // For a body record: the auto-properties (minus skip-marked ones).
        // A `serialized skip` field is excluded from the component set (§9.8).
        std::unordered_set<const TypeSymbol*> visited;
        visited.insert(t);  // guard self-reference

        auto reportIneligible = [&](std::string_view memberName,
                                    std::string_view reason) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string(isRecord ? "record" : "type") +
                " `" + std::string(t->name) + "` declares `ISerializable`/"
                "`IDeserializable` but is not serialization-eligible (§9.8): "
                "field `" + std::string(memberName) + "` " +
                std::string(reason) + "; remove the conformance declaration, "
                "mark the field `serialized skip`, make the field type "
                "serializable, or hand-write `Serialize`/`Deserialize`");
        };

        bool foundError = false;

        // Positional record: check positional fields.
        if (td->hasPositionalFields) {
            for (const auto& pf : td->positionalFields) {
                if (positionalFieldIsSkip(pf)) continue;  // skip → excluded
                // §2.15 v0.139: `serialized "xxx"` rename whose name begins with
                // `$` is reserved for JSON graph-identity tokens — compile error.
                if (pf.serialized && !pf.serialized->name.empty() &&
                    pf.serialized->name.front() == '$') {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "record `" + std::string(t->name) + "`: `serialized` "
                        "rename `" + std::string(pf.serialized->name) +
                        "` for field `" + std::string(pf.name) +
                        "` begins with `$`, which is reserved for JSON "
                        "graph-identity tokens (§2.15 v0.139); choose a name "
                        "that does not start with `$`");
                    foundError = true;
                    break;
                }
                if (!pf.type) continue;
                const TypeRef* tr = ctx_.resolvedType(*pf.type);
                if (!tr) continue;
                if (!isSerializationEligibleType(tr, visited)) {
                    reportIneligible(pf.name,
                        "has a type that is not serialization-eligible");
                    foundError = true;
                    break;  // report first offending member only (§9.8)
                }
            }
        } else {
            // Body record: check auto-properties.
            for (const Symbol* m : t->members) {
                if (!isAutoPropertySymbol(m)) continue;
                auto* ps = dynamic_cast<const PropertySymbol*>(m);
                if (!ps) continue;
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (!pd) continue;
                // Check for serialized skip on the auto-property.
                if (pd->serialized && pd->serialized->isSkip) continue;
                // §2.15 v0.139: `serialized "xxx"` rename beginning with `$` reserved.
                if (pd->serialized && !pd->serialized->name.empty() &&
                    pd->serialized->name.front() == '$') {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "record `" + std::string(t->name) + "`: `serialized` "
                        "rename `" + std::string(pd->serialized->name) +
                        "` for property `" + std::string(m->name) +
                        "` begins with `$`, which is reserved for JSON "
                        "graph-identity tokens (§2.15 v0.139); choose a name "
                        "that does not start with `$`");
                    foundError = true;
                    break;
                }
                if (!pd->type) continue;
                const TypeRef* tr = ctx_.resolvedType(*pd->type);
                if (!tr) continue;
                if (!isSerializationEligibleType(tr, visited)) {
                    reportIneligible(m->name,
                        "has a type that is not serialization-eligible");
                    foundError = true;
                    break;
                }
            }
            if (!foundError) {
                // §9.8/§9.2: a body record must also be auto-equality-bearing
                // for synthesis to apply (same membership conditions as §9.2).
                // If it is not auto-equality-bearing AND doesn't have hand-written
                // Serialize/Deserialize, the synthesis cannot proceed.
                // (We don't require auto-equality-bearing for synthesis per the
                // spec wording; we require the §9.8 body-record conditions which
                // are the same as §9.2 "auto-equality-bearing" conditions. The
                // spec says "under exactly the auto-equality membership conditions"
                // so treat them identically.)
                // Note: we only report this if there are actually auto-properties
                // to serialize — an empty record body is still eligible (trivially).
            }
        }

        (void)foundError;  // error was already reported if true
    }
}

}  // namespace bezel::sema
