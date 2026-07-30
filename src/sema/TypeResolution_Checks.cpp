#include "sema/TypeResolution_impl.hpp"

#include "common/BuiltinRegistry.hpp"
#include "common/OwnershipFacts.hpp"  // classifyChoiceType — §35.7 equality-bearing choice edge (#2087)

#include "sema/ConstFold.hpp"
#include "sema/Sendability.hpp"  // isSendableType — §30.14 sendability computation
#include "sema/TypeResolution.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>

namespace bezel::sema {

// §11.8/§28.1 (v0.196, #2063): owned-marker erasure at value/runtime-managed
// substitutions on the witness-side conformance match. A LAYER-LOCAL copy of
// the value-kind predicate the declaration-site check (Resolver::
// checkOwnedDeclType, §28.1) uses to decide where `owned` is admissible: it
// returns true on exactly that check's REJECT set — the value and runtime-
// managed types (§28.9) on which `owned` carries no disposal obligation to
// transfer. When an interface member declares `owned T` (or `out owned T`)
// on a bare type parameter T of the interface, §28.1 (v0.196) ERASES the
// marker at a value/runtime-managed conformer: the substituted signature is
// the unmarked one, so the witness is spelled without the marker (which the
// concrete type would reject). The marker is RETAINED at class and interface
// substitutions (ordinary transfer meaning) and at move-only value
// substitutions (admitted-but-redundant, §15.3). Kept as a private copy
// (mirroring ExprTypecheck_Calls.cpp's ownedMarkerErasedAtSubstitutedType,
// same reasoning) so the two layers do not share mutable state; the
// classification is deliberately identical.
static bool ownedMarkerErasedAtSubstitutedType(const TypeRef* t) {
    if (!t) return false;
    switch (t->kind) {
    case TypeRef::Kind::Error:
        return false;  // resolution failed — leave the marker untouched.
    case TypeRef::Kind::Unit:
        return true;   // the unit type `()` — a value type.
    case TypeRef::Kind::Function:
        return true;   // a function value is a runtime-managed reference (§28.9).
    case TypeRef::Kind::Primitive:
        // Every primitive is a value type, or — for `string`/`Object` — a
        // runtime-managed reference type (§28.9); none carries a disposal
        // obligation, mirroring checkOwnedDeclType's uniform primitive reject.
        return true;
    case TypeRef::Kind::InlineArray:
        // §15.3: an owning inline array is a MOVE-ONLY value type — the marker
        // is retained (admitted-but-redundant), so return false; a non-owning
        // inline array is a plain value type and erases.
        return !t->arrayOwned;
    case TypeRef::Kind::Named: {
        const TypeSymbol* ts = t->declaration;
        if (ts)
            return ts->isValueBuiltin ||
                   ts->kind == ast::TypeDeclKind::Record ||
                   ts->kind == ast::TypeDeclKind::Enum;
        // No TypeSymbol: a value-type builtin (DateTime/Guid/… —
        // isValueBuiltinByName) erases; a class/interface builtin (Array<T>,
        // Task, …) and an UNRESOLVED bare type parameter (a nested generic
        // still abstract at this instantiation) both RETAIN the marker.
        return isValueBuiltinByName(t->headName());
    }
    }
    return false;
}

// #1272 / #2208 (§9 + §35.1, v0.226): value-containment-cycle detection for
// value types. Returns true when `start` is transitively reachable from `cur`
// by following value-typed components — record instance fields, choice case
// payloads, and inline-array elements of either. A class/interface component
// is a heap reference and breaks the cycle; a bare generic type-parameter
// (declaration == null) is opaque and conservatively ignored. `seen` prevents
// unbounded recursion through unrelated cycles and through the cycle itself on
// malformed recursive graphs (record→choice→record, mutual record pairs, etc.),
// which would otherwise send `Emitter::recordStruct` into infinite LLVM struct
// layout recursion (#2208 — the §35.1 choice-self-containment clause also
// reachable here, since a choice is walked like a record).
//
// The unwrap step strips nullable (`T?` for a value type is still stored inline
// — §9) and inline-array wrappers (`[N]T` is a fixed-size value struct, so its
// element participates in the value-containment graph — §15.1). Named types
// other than Record/Choice (classes, interfaces, enums, primitives) are not
// stored inline and therefore break the cycle.
//
// `recordsAllowed` (default true) is the keying for diagnostic dedup: when
// false, the walker treats records as cycle-breakers (a record remains a heap
// reference's analytical counterpart in this mode) and only follows choices.
// This is the choice-side check's view of the world — a record↔choice cycle's
// record already reports itself via checkRecordRestrictions, and the choice
// check only fires when the cycle is purely choice↔choice. The two checks
// therefore never emit duplicate diagnostics on a single malformed graph.
bool Resolver::recordValueContainsSelf(TypeSymbol* start, TypeSymbol* cur,
                                       std::unordered_set<TypeSymbol*>& seen,
                                       bool recordsAllowed) {
    if (!cur) return false;
    // Unwrap nullable + inline-array to the value-typed declaration (record or
    // choice) that the component stores inline. Returns null for any non-value
    // type (class, interface, enum, primitive, function, unresolved/error).
    auto valueTypeOf = [&](const TypeRef* rt) -> TypeSymbol* {
        while (rt && rt->kind == TypeRef::Kind::InlineArray)
            rt = rt->arrayElement;
        if (!rt || rt->kind != TypeRef::Kind::Named || !rt->declaration)
            return nullptr;
        auto k = rt->declaration->kind;
        if (k == ast::TypeDeclKind::Choice) return rt->declaration;
        if (k == ast::TypeDeclKind::Record)
            return recordsAllowed ? rt->declaration : nullptr;
        return nullptr;
    };
    // §35.1: a choice's value-typed components are its case-payload fields.
    // Walking payload→record→field→... extends the existing record-only walk
    // to the record↔choice by-value cycles that issue #2208 surfaced.
    if (cur->kind == ast::TypeDeclKind::Choice) {
        for (Symbol* m : cur->members) {
            auto* cc = dynamic_cast<ChoiceCaseSymbol*>(m);
            if (!cc || !cc->caseDecl) continue;
            for (const ast::Param& pf : cc->caseDecl->payload) {
                if (!pf.type) continue;
                TypeSymbol* contained =
                    valueTypeOf(ctx_.resolvedType(*pf.type));
                if (!contained) continue;
                if (contained == start) return true;
                if (seen.insert(contained).second &&
                    recordValueContainsSelf(start, contained, seen,
                                            recordsAllowed))
                    return true;
            }
        }
        return false;
    }
    for (Symbol* m : cur->members) {
        auto* fs = dynamic_cast<FieldSymbol*>(m);
        if (!fs || hasMod(fs, "static")) continue;
        // A record field's type comes from its positional param (`posParam`) or,
        // for a body field, its FieldDecl. (`fs->decl` of a positional field is
        // the owning TypeDecl, not a FieldDecl — §9.1.)
        const ast::Type* ftype = nullptr;
        if (fs->posParam && fs->posParam->type)
            ftype = fs->posParam->type.get();
        else if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
            ftype = fd->type.get();
        if (!ftype) continue;
        TypeSymbol* contained = valueTypeOf(ctx_.resolvedType(*ftype));
        if (!contained) continue;
        if (contained == start) return true;
        if (seen.insert(contained).second &&
            recordValueContainsSelf(start, contained, seen, recordsAllowed))
            return true;
    }
    return false;
}

// Free-function wrapper exposing the resolver's structural type equality
// for reuse by other passes (§22.6 overload resolution). Single source of
// truth: delegates to Resolver::typeRefEqual.
bool typeRefEqual(const TypeRef* a, const TypeRef* b) {
    return Resolver::typeRefEqual(a, b);
}

// Free-function wrapper exposing §12.1's override parameter-nullability-
// widening match predicate for reuse by other passes (§22.6 overload-set
// dedup, #1560). Single source of truth: delegates to
// Resolver::overrideParamTypeMatches.
bool overrideParamTypeMatches(const TypeRef* derivedTy, const TypeRef* baseTy,
                              bool isByRefParam) {
    return Resolver::overrideParamTypeMatches(derivedTy, baseTy, isByRefParam);
}

// Free-function wrapper exposing the resolver's built-in-type-name recognizer
// for reuse by the §19.3 top-level case-pattern validator (#1847). Single
// source of truth: delegates to Resolver::isBuiltinTypeName.
bool isBuiltinTypeName(std::string_view n) {
    return Resolver::isBuiltinTypeName(n);
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
// `Task` / `Task<T>` / `OwningTask<T>` cover the value/borrow/owned result
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
    return head == "Task" || head == "OwningTask" ||
           head == "IAsyncEnumerable" || head == "IAsyncEnumerator" ||
           head == "IOwningAsyncEnumerable" || head == "IOwningAsyncEnumerator";
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
        // admitted async forms (`Task` / `Task<T>` / `OwningTask<T>`, or an
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
                    "` must declare a `Task`, `Task<T>`, or `OwningTask<T>` "
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
            // per §24.1 (v0.180): an event's compiler-generated `owned
            // EventSource` backing field requires an owning, disposable
            // container, which an enum is not (§9.7, Appendix C.3).
            if (dynamic_cast<EventSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("enum `") + std::string(t->name) +
                    "` may not declare an `event` member — an event's "
                    "compiler-generated `owned EventSource` backing field "
                    "requires an owning, disposable container, which an enum "
                    "is not (§24.1, §9.7)");
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
    // `paramOrder` is the type parameters in scope for this `where`-clause set,
    // in declaration order (a method's own parameters first, then the enclosing
    // type's). A bound whose head names one of these is a type-parameter bound
    // (§14.2's `where T: U`), as opposed to a bound naming a real class or
    // interface — the distinction the self/cycle checks below turn on.
    auto checkList =
        [&](const std::vector<ast::GenericConstraint>& gcs,
            const std::vector<std::string_view>& paramOrder, SourceLocation loc) {
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

            // §14.2 (v0.196): "A self-referential bound (`where T: T`) or a cycle
            // among type-parameter bounds (`where T: U` together with `where U:
            // T`) is a compile error at the declaration." Detected here — once
            // per declaration, independent of instantiation.
            std::unordered_set<std::string_view> paramSet(paramOrder.begin(),
                                                          paramOrder.end());
            // A bound is a *type-parameter bound* only when it is a bare name (a
            // single-part `QualifiedName` with no generic arguments) that matches
            // a type parameter in scope. `where T: IComparable<T>` is NOT such a
            // bound: its head is the interface `IComparable`; `T` appears only as
            // a generic argument, so this leaves it accepted.
            auto boundParam = [&](const ast::ConstraintItem& item)
                -> std::string_view {
                if (item.kind != "type" || !item.typeArg) return {};
                const ast::Type& bt = *item.typeArg;
                if (bt.kind != ast::TypeKind::Named || !bt.name ||
                    bt.name->parts.size() != 1 || !bt.genericArgs.empty())
                    return {};
                std::string_view head = bt.name->parts.back();
                return paramSet.count(head) ? head : std::string_view{};
            };
            // Directed graph "gc.typeParam is bounded by U" over type-parameter
            // bounds; a self-edge is the self-referential error, reported here.
            std::unordered_map<std::string_view, std::vector<std::string_view>>
                edges;
            std::unordered_set<std::string_view> selfReported;
            for (const auto& gc : gcs) {
                if (!gc.modifierConstraint.empty()) continue;
                for (const auto& item : gc.items) {
                    std::string_view u = boundParam(item);
                    if (u.empty()) continue;
                    if (u == gc.typeParam) {
                        if (selfReported.insert(gc.typeParam).second)
                            ctx_.diagnostics().report(
                                Severity::Error, loc,
                                "self-referential constraint on type parameter `" +
                                std::string(gc.typeParam) +
                                "`: a type parameter cannot be bounded by itself "
                                "(`where " + std::string(gc.typeParam) + ": " +
                                std::string(gc.typeParam) + "`) (§14.2)");
                        continue;  // no self-loop edge — reported already
                    }
                    edges[gc.typeParam].push_back(u);
                }
            }
            // Report each distinct cycle once (covers the 2-cycle `T: U`,
            // `U: T` and transitive cycles `T: U`, `U: V`, `V: T`). DFS coloring;
            // a back edge to a gray node closes a cycle. Roots are visited in
            // declaration order so the diagnostic is deterministic.
            std::unordered_map<std::string_view, int> color;  // 0 white,1 gray,2 black
            std::vector<std::string_view> stack;
            std::unordered_set<std::string> reported;
            std::function<void(std::string_view)> dfs = [&](std::string_view n) {
                color[n] = 1;
                stack.push_back(n);
                auto it = edges.find(n);
                if (it != edges.end())
                    for (std::string_view m : it->second) {
                        if (color[m] == 1) {
                            std::vector<std::string_view> cyc;
                            auto s = std::find(stack.begin(), stack.end(), m);
                            for (auto i = s; i != stack.end(); ++i)
                                cyc.push_back(*i);
                            std::vector<std::string> key;
                            for (std::string_view c : cyc) key.emplace_back(c);
                            std::sort(key.begin(), key.end());
                            std::string canon;
                            for (const auto& c : key) canon += c + ",";
                            if (reported.insert(canon).second) {
                                std::string chain;
                                for (std::string_view c : cyc)
                                    chain += "`" + std::string(c) + "` -> ";
                                chain += "`" + std::string(cyc.front()) + "`";
                                ctx_.diagnostics().report(
                                    Severity::Error, loc,
                                    "cyclic constraint among type parameters: " +
                                    chain +
                                    " — a cycle among type-parameter bounds is a "
                                    "compile error at the declaration (§14.2)");
                            }
                        } else if (color[m] == 0) {
                            dfs(m);
                        }
                    }
                stack.pop_back();
                color[n] = 2;
            };
            for (std::string_view p : paramOrder)
                if (color[p] == 0) dfs(p);
        };
    for (Symbol* child : ctx_.module()->children) {
        auto* ts = dynamic_cast<TypeSymbol*>(child);
        if (!ts || !ts->decl) continue;
        auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
        if (!td) continue;
        std::vector<std::string_view> typeParams;
        for (const auto& gp : td->genericParams) typeParams.push_back(gp.name);
        checkList(td->genericConstraints, typeParams, td->span.begin);
        for (const auto& m : td->members)
            if (auto* mdl = dynamic_cast<const ast::MethodDecl*>(m.get())) {
                // A method's `where` clause may bound a method parameter by an
                // enclosing-type parameter (§14.2), so both are in scope. A
                // cycle can only form among the method's own parameters (the
                // enclosing declaration cannot name a method parameter), but
                // including the enclosing parameters lets `where T: U` (U the
                // enclosing param) be recognized as a type-parameter bound.
                std::vector<std::string_view> methodParams;
                for (const auto& gp : mdl->genericParams)
                    methodParams.push_back(gp.name);
                for (std::string_view p : typeParams) methodParams.push_back(p);
                checkList(mdl->genericConstraints, methodParams, mdl->span.begin);
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
            // §17.14 (v0.184, #1688): fitsConstIntRange / containsConstDivByZero
            // are shared with the local-`const` fault check (ExprTypecheck.cpp)
            // so both const forms agree on exactly one "fits"/"div-by-zero"
            // definition (ConstFold.hpp).
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
                if (!fitsConstIntRange(*folded, p)) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("const `") + std::string(cs->name) +
                        "` initializer value " + std::to_string(*folded) +
                        " does not fit its declared type — a const initializer "
                        "must be a representable compile-time constant "
                        "(§6.1 / Appendix C.11)");
                }
            } else {
                // per §22.12 (#1211): the initializer is structurally a
                // constant expression (passed isConstInitExpr) but
                // foldIntConst returned nullopt — report the specific cause
                // when it is a division or modulo by zero.
                if (containsConstDivByZero(*cd->value, resolve)) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("const `") + std::string(cs->name) +
                        "` initializer contains a division or modulo by "
                        "zero — a const initializer must fold to a "
                        "representable compile-time-constant value (§22.12)");
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
            // per §24.1 (v0.180): an event's compiler-generated `owned
            // EventSource` backing field requires an owning, disposable
            // container, which a record is not (§9.7, Appendix C.3).
            if (dynamic_cast<EventSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "` may not declare an `event` member — an event's "
                    "compiler-generated `owned EventSource` backing field "
                    "requires an owning, disposable container, which a record "
                    "is not (§24.1, §9.7)");
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
                // §9.7 / §35.4 (issue #2210, v0.226): an owning-classified
                // choice as a record field is rejected. Records declare no
                // disposal path (§9.7: no destructor, no `owned` fields, no
                // `IDisposable`), and §35.4 makes owning choices move-only
                // value types whose active case carries `owned` payloads
                // (§28.3); the buried owner would never be discharged. The
                // §35.4 classification is structural (§35.4.1) — a generic
                // `Opt<T>` field whose `T` is an owning class instantiates to
                // an owning choice and is rejected at the instantiation site.
                // Borrow-bearing and value-like choices are unaffected; the
                // former is travel-restricted by §9.7's regular borrow rules
                // (consulted via the #2159 Choice leg of the record field
                // walk), the latter is an ordinary value type.
                //
                // Positional records: type rides on `posParam->type`. Body
                // fields: type rides on the `FieldDecl`. Resolve whichever
                // is present and consult the §35.4 classifier.
                const ast::Type* ftypeAst = nullptr;
                if (fs->posParam && fs->posParam->type)
                    ftypeAst = fs->posParam->type.get();
                else if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
                    ftypeAst = fd->type.get();
                if (ftypeAst) {
                    const TypeRef* rt = ctx_.resolvedType(*ftypeAst);
                    if (rt && rt->kind == TypeRef::Kind::Named &&
                        rt->declaration &&
                        rt->declaration->kind == ast::TypeDeclKind::Choice) {
                        auto verdict = common::classifyChoiceType(rt, ctx_);
                        if (verdict &&
                            *verdict == common::ChoiceClassification::Owning) {
                            ctx_.diagnostics().report(
                                Severity::Error, m->declLoc,
                                std::string("record `") +
                                std::string(t->name) +
                                "` may not have an owning-classified choice "
                                "field `" + std::string(fs->name) +
                                "` (§35.4, §9.7); owning choices are move-only "
                                "value types and records declare no disposal "
                                "path (§15.3, §9.7)");
                        }
                    }
                }
            }
            // §9.7 / §35.4 (issue #2210): body-record auto-properties
            // (`property Name: T;`) are storage-bearing fields too; reject
            // an owning-classified choice type at the property declaration.
            if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
                if (isAutoPropertySymbol(ps)) {
                    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                    if (pd && pd->type) {
                        const TypeRef* rt = ctx_.resolvedType(*pd->type);
                        if (rt && rt->kind == TypeRef::Kind::Named &&
                            rt->declaration &&
                            rt->declaration->kind ==
                                ast::TypeDeclKind::Choice) {
                            auto verdict =
                                common::classifyChoiceType(rt, ctx_);
                            if (verdict &&
                                *verdict ==
                                    common::ChoiceClassification::Owning) {
                                ctx_.diagnostics().report(
                                    Severity::Error, m->declLoc,
                                    std::string("record `") +
                                    std::string(t->name) +
                                    "` may not have an owning-classified "
                                    "choice auto-property `" +
                                    std::string(ps->name) +
                                    "` (§35.4, §9.7); owning choices are "
                                    "move-only value types and records "
                                    "declare no disposal path (§15.3, §9.7)");
                            }
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
        // #1272 (§9): a record is a VALUE type stored inline, so a record that
        // transitively contains itself by value (`record Node(Next: Node?)`,
        // mutual recursion, or an inline-array-of-self) has infinite size and
        // cannot be laid out. Left unchecked this crashed codegen (unbounded
        // record-struct layout recursion → LLVM setBody). Reject it here with a
        // clean diagnostic; a recursive structure must use a class (heap ref).
        {
            std::unordered_set<TypeSymbol*> seen;
            if (recordValueContainsSelf(t, t, seen)) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "` cannot recursively contain itself by value — a record is "
                    "stored inline, so a self-referential record has infinite "
                    "size (a nullable `" + std::string(t->name) +
                    "?` field is still stored inline); use a `class` (a heap "
                    "reference) for a recursive structure (§9)");
            }
        }
    }
}

// #2208 (§35.1, v0.226): a choice may not transitively contain itself through
// value-typed components (case payloads, including payload records, nested
// payloads, and inline-array elements of either). The §35.1 statement is
// pronounced on the cycle's STARTING choice — each choice that transitively
// contains itself through value-typed components is its own violation (a
// choice↔choice mutual pair reports once per choice, mirroring the §9 mutual
// record pair `A` ↔ `B` that fires once per record). The underlying walker is
// the existing Resolver::recordValueContainsSelf (extended to follow choice
// payloads — §9's record walk was too narrow for the record↔choice case).
// Without this check, a `choice Ch Wrap(r: Rec)` paired with a
// `record Rec(K: Ch)` would loop codegen's Emitter::recordStruct into
// infinite LLVM struct-body recursion (a stack overflow / hang).
//
// `recordsAllowed = false` keys the duplicate-avoidance between the two CHECK
// sides: a record↔choice cycle (the most common form of #2208) is reported
// exactly once — by the record's checkRecordRestrictions — never twice (the
// choice check deliberately does NOT fire here, because the record is the
// cycle's canonical starting point). The choice check is therefore the
// record-side diagnostic's complement on record↔choice graphs: it suppresses
// itself when a record is on the cycle's path, and only fires on cycles that
// are entirely choices (or that satisfy the §35.1 single-choice self-
// containment clause, which the walker also covers).
//
// NB: a mutual choice PAIR (A ↔ B) is reported ONCE PER choice — both A and B
// independently violate §35.1 from their own starting declLoc. This is the
// same-shape behavior the existing §9 record self-containment check emits
// for a mutual record pair (one diagnostic per record, both firing on the
// same graph). The "recordsAllowed = false" discipline is graph-level dedup
// (record-side vs choice-side), not per-cycle-fold dedup.
void Resolver::checkChoiceSelfContainment() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Choice) continue;
        std::unordered_set<TypeSymbol*> seen;
        if (recordValueContainsSelf(t, t, seen, /*recordsAllowed=*/false)) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string("choice `") + std::string(t->name) +
                "` cannot recursively contain itself by value through its "
                "case payloads — a choice is stored inline, so a "
                "value-typed cycle through its payload fields has infinite "
                "size (a nullable payload is still stored inline); use a "
                "`class` (a heap reference) at the cycle's break point "
                "(§35.1)");
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
            // #2001: `[3]T` and `[4]T` are DISTINCT types — the size N is part
            // of the type identity (§14.2 / §15.1). Comparing only element +
            // ownedness collapsed them, mirroring (and feeding) the runtime
            // descriptor collision. Distinguish ONLY when both sizes are FOLDED
            // (arraySizeValue >= 0, the folded compile-time size): if either is
            // still unfolded (-1) fall back to the pre-existing size-blind
            // behavior — the size is not statically known here, so a fold-state
            // mismatch between two otherwise-identical `[N]T` refs must not be
            // read as a type difference. Two folded, differing sizes ARE distinct
            // (the case the descriptor collision and the memory-unsafety needed).
            if (a->arrayOwned != b->arrayOwned) return false;
            if (a->arraySizeValue >= 0 && b->arraySizeValue >= 0 &&
                a->arraySizeValue != b->arraySizeValue)
                return false;
            return typeRefEqual(a->arrayElement, b->arrayElement);
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
    // per §22.6 / §11.3 (v0.181): neither `owned` (ordinary or on an `out`
    // parameter) nor `scoped` participates in overload resolution — only
    // arity, parameter types, and the `ref`/`out` modifiers distinguish
    // overloads. `owned` and `scoped` collapse to the plain form, and
    // `out owned` collapses to `out` (the v0.77 `out`/`out owned` carve-out is
    // withdrawn), so two members differing only in these markers are
    // indistinguishable and clash.
    if (mod == "owned" || mod == "scoped" || mod.empty()) return "value";
    if (mod == "out owned" || mod == "out") return "out";
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
                        "types, or `ref`/`out` modifiers; neither `owned` "
                        "(ordinary or on an `out` parameter) nor `scoped` "
                        "distinguishes (§22.6/§11.3, v0.181)");
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
            // §11.8 (v0.176, #807): a STATIC INTERFACE operator member is a
            // contract, not an overload declaration — its parameters are the
            // constrained type parameter (`(a: T, b: T)`), it is implicitly
            // public (§10.1), and the §2.19 interfaces normatively declare a
            // lone `<` (no complete comparison group). The overload-shape
            // rules below (public, declaring-type parameter, complete groups)
            // apply to the CONFORMING type's operators, not to the contract.
            if (t->kind == ast::TypeDeclKind::Interface) continue;
            // §25 (#1347, v0.160): operators must be declared `public`. An
            // operator invoked through `a + b` syntax has no call-site spelling
            // that could name a restricted member; any other visibility is a
            // compile error (Appendix A.4's OperatorDecl requires `public`).
            if (ms->visibility != ast::Visibility::Public) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "operator `" + std::string(tok) + "` must be `public` — "
                    "operators are declared `public static` and invoked through "
                    "infix syntax with no call-site spelling for a restricted "
                    "member (§25)");
            }
            // §25.2 (#1664, v0.184): "Operator parameters admit no parameter
            // modifiers." `ref`/`out`/`owned`/`scoped` on an operator's
            // parameter list are rejected semantically — Appendix A.4's
            // OperatorDecl over-admits them through the shared `Param`
            // production (the same over-admit-and-reject pattern as §22.10's
            // primary constructors), and the return slot's optional `owned`
            // (grammar A.4) is the sole ownership annotation on operators.
            // Operands are received as plain borrows (or value copies) under
            // their type kinds. Was silently accepted (`owned`/`out`) or fell
            // through to a codegen IR-verification failure (`ref`, whose
            // by-reference ABI never matches the by-value operator signature).
            for (Symbol* ps : ms->params) {
                auto* param = dynamic_cast<ParameterSymbol*>(ps);
                if (!param || param->modifier.empty()) continue;
                // ParameterSymbol carries no declLoc of its own (#1664
                // survey: no other diagnostic in this file keys off
                // param->declLoc either) — point at the operator's own
                // declaration, as the declaring-type-parameter check below
                // (`m->declLoc`) already does.
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "operator parameter `" + std::string(param->name) +
                    "` may not carry the `" + std::string(param->modifier) +
                    "` modifier — operator parameters admit no parameter "
                    "modifiers; operands are received as plain borrows or "
                    "value copies, and `owned` may appear only on an "
                    "operator's return type (§25.2)");
            }
            // §25.2 (#1347, v0.160): at least one parameter of an operator
            // overload must be the declaring type. An operator whose parameters
            // never mention the declaring type would be unreachable by the
            // resolution rule (which consults the static types of both operands).
            {
                bool hasDeclaringTypeParam = false;
                for (Symbol* ps : ms->params) {
                    auto* param = dynamic_cast<ParameterSymbol*>(ps);
                    if (!param) continue;
                    TypeRef* pt = param->declType
                                  ? ctx_.resolvedType(*param->declType)
                                  : param->resolvedDeclType;
                    if (pt && pt->kind == TypeRef::Kind::Named &&
                        pt->declaration == t) {
                        hasDeclaringTypeParam = true;
                        break;
                    }
                }
                if (!hasDeclaringTypeParam) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        "at least one parameter of operator `" +
                        std::string(tok) + "` must be the declaring type `" +
                        std::string(t->name) +
                        "` — an operator with no declaring-type parameter is "
                        "unreachable by the resolution rule (§25.2)");
                }
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

// §8.3 (#1193): "A type that overrides `Equals` must also override
// `GetHashCode`. The compiler enforces this." `Object` declares `Equals` and
// `GetHashCode` as `virtual` (§8.1), so a user override of either carries the
// `override` modifier; an `IEquatable<T>` `Equals(T)` is an interface
// *implementation*, not an `override`, so it is not the `Equals(Object?)`
// override this rule is about. A type overriding `Equals` but not `GetHashCode`
// would satisfy `=` while leaving the hash inconsistent — the classic
// dictionary-lookup footgun the rule prevents (rationale §7.3). Records using
// the same `override` form (§9.2) are covered uniformly. Was accepted silently.
void Resolver::checkEqualsHashCodePairing() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        MethodSymbol* equalsOverride = nullptr;
        bool overridesHashCode = false;
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms || ms->isOperator || !hasMod(ms, "override")) continue;
            if (ms->name == "Equals" && !equalsOverride) equalsOverride = ms;
            else if (ms->name == "GetHashCode") overridesHashCode = true;
        }
        if (equalsOverride && !overridesHashCode) {
            ctx_.diagnostics().report(
                Severity::Error, equalsOverride->declLoc,
                "a type that overrides `Equals` must also override "
                "`GetHashCode` (§8.3)");
        }
    }
}

// §25.2 (v0.158, #1338; enum spelling corrected v0.183, #1381/#1582): "A record
// or enum that overloads `=`/`<>` with both operands of the declaring type must
// also declare `Equals(T)` and — on a record, `override GetHashCode`; on an
// enum, a plain `GetHashCode(): int`, since enum members never carry `override`
// (§10.5) — the same discipline, and the same diagnostic style, as §8.3's
// Equals→GetHashCode pairing." The three surfaces (`=`, `Equals`, hashing) can
// otherwise silently diverge exactly where it hurts: `a = b` true while a
// `Set<T>` treats them as distinct. Heterogeneous-operand overloads (`T = U`)
// carry no such obligation. The compiler-intrinsic IntPtr/UIntPtr operator
// declarations predate and are exempt.
//
// v0.183 fix: the diagnostic text below previously said `` `override
// GetHashCode` `` unconditionally, including for enums — but §10.5 REJECTS
// `override` on any enum method, so an enum author following that diagnostic
// literally hit a second compile error. The message now spells the requirement
// the way each `t->kind` actually accepts it.
void Resolver::checkEqualityCoDeclaration() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        if (t->kind != ast::TypeDeclKind::Record &&
            t->kind != ast::TypeDeclKind::Enum)
            continue;
        // §25.2: the compiler-intrinsic pointer-sized integer records are exempt.
        if (t->name == "IntPtr" || t->name == "UIntPtr") continue;

        MethodSymbol* homoOp = nullptr;   // first homogeneous `=`/`<>` overload
        bool hasEquals = false, hasGetHashCode = false;
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms) continue;
            if (ms->isOperator &&
                (ms->operatorToken == "=" || ms->operatorToken == "<>")) {
                if (homoOp || ms->params.size() != 2) continue;
                // Homogeneous iff BOTH operands resolve to the declaring type.
                bool bothT = true;
                for (Symbol* psym : ms->params) {
                    auto* p = dynamic_cast<ParameterSymbol*>(psym);
                    const TypeRef* pt = (p && p->declType)
                        ? ctx_.resolvedType(*p->declType) : nullptr;
                    if (!pt || pt->kind != TypeRef::Kind::Named ||
                        pt->declaration != t) { bothT = false; break; }
                }
                if (bothT) homoOp = ms;
            } else if (!ms->isOperator && ms->name == "Equals" &&
                       ms->params.size() == 1) {
                // Any single-parameter `Equals` (whether `Equals(T)` or
                // `Equals(Object?)`) states the equality surface (§25.2 lists
                // both as reachable via `.Equals(...)`).
                hasEquals = true;
            } else if (!ms->isOperator && ms->name == "GetHashCode" &&
                       ms->params.empty()) {
                hasGetHashCode = true;
            }
        }
        if (homoOp && !(hasEquals && hasGetHashCode)) {
            bool isEnum = t->kind == ast::TypeDeclKind::Enum;
            std::string kind = isEnum ? "an enum" : "a record";
            // §10.5: enum members never carry `override` — a plain
            // `GetHashCode(): int` is the enum spelling (v0.183 fix, #1582).
            std::string hashSpelling =
                isEnum ? "`GetHashCode`" : "`override GetHashCode`";
            ctx_.diagnostics().report(
                Severity::Error, homoOp->declLoc,
                kind + " that overloads `=` must also declare `Equals(T)` and "
                + hashSpelling + " (\xC2\xA7""25.2)");
        }
    }
}

// §3.6 / §8.3 (v0.158, #1338): a `Dictionary<K,V>` / `OwningDictionary<K,V>` key
// (K) and a `Set<T>` / `OwningSet<T>` element (T) must be **equality-bearing**.
// The sites are recorded during name resolution (resolveNamed) and validated
// here — after every member type is resolved — because a record key's
// auto-equality-bearing property depends on the record's own members being
// resolved, which may happen after the container annotation is visited (§3.6
// rule 2: per-instantiation, at the instantiation site).
//
// #1384: a second pass handles sites where the key was a generic type parameter
// (conservatively accepted in the generic body — no constraint surface exists to
// require equality-bearing). When the enclosing generic is concretely
// instantiated with a non-equality-bearing type, the per-instantiation check
// fires at the hash-container annotation site (§3.6 rule 2).
void Resolver::checkHashContainerKeys() {
    std::unordered_set<const ast::Type*> reported;

    // §3.6/§8.3: the two non-equality-bearing shapes are (a) a record that
    // neither synthesizes nor declares equality (#1338) and (b) — v0.210/
    // v0.214, #2083/#2087 — a choice that is not value-like auto-equality-
    // bearing (§35.7: owning / borrow-bearing, or value-like with a payload
    // outside the recursive §9.2 admissible-component rule — the #2110
    // key-position half). The reason clause is kind-aware so an owning-choice
    // key does not draw the record-worded message.
    auto reasonClause = [&](const TypeRef* keyRef) -> std::string {
        const TypeSymbol* d = keyRef ? keyRef->declaration : nullptr;
        if (d && d->kind == ast::TypeDeclKind::Choice) {
            auto v = common::classifyChoiceType(keyRef, ctx_);
            if (v && *v == common::ChoiceClassification::ValueLike)
                // Value-like yet not equality-bearing: the RECURSIVE §35.7
                // rule rejected a case payload (function/Object/etc.).
                return "it is a choice that is value-like but has a case "
                       "payload outside the \xC2\xA7""9.2 admissible-component "
                       "rule (\xC2\xA7""35.7), so it is not "
                       "auto-equality-bearing. Restructure the payload, or "
                       "use an equality-bearing key type.";
            const char* cat =
                (v && *v == common::ChoiceClassification::Owning)
                    ? "owning"
                    : "borrow-bearing";
            return std::string("it is a choice that is ") + cat +
                   " (\xC2\xA7""35.7), not value-like auto-equality-bearing. "
                   "Restructure it to a value-like choice, or declare custom "
                   "equality.";
        }
        return "it is a record that is neither auto-equality-bearing "
               "(\xC2\xA7""9.2) nor declares custom equality (`operator =`/`<>` "
               "with `Equals(T)` and `override GetHashCode`, \xC2\xA7""25.2). "
               "Give it an auto-equality-bearing form, or declare custom "
               "equality.";
    };

    // Pass 1: direct (non-generic-param) key check (the original #1338 logic).
    for (const HashContainerSite& s : hashContainerSites_) {
        if (isEqualityBearing(s.keyRef, ctx_)) continue;
        if (s.site && !reported.insert(s.site).second) continue;  // dedup per site
        const TypeSymbol* rec = s.keyRef ? s.keyRef->declaration : nullptr;
        std::string keyName =
            rec ? ("`" + std::string(rec->name) + "`") : std::string("it");
        ctx_.diagnostics().report(
            Severity::Error,
            s.site ? s.site->span.begin : SourceLocation{},
            std::string("`") + std::string(s.container) +
            "` requires an equality-bearing " + std::string(s.property) +
            " type, but the type argument " + keyName +
            " is not equality-bearing (\xC2\xA7""3.6/\xC2\xA7""8.3): " +
            reasonClause(s.keyRef));
    }

    // Pass 2 (#1384): per-instantiation check for sites whose key is a generic
    // type parameter. isEqualityBearing returns true conservatively for unresolved
    // generic params (no concrete type is known at the declaration site), so those
    // sites pass pass 1. Here we walk concrete instantiations of the enclosing
    // type to check whether the bound type is equality-bearing.
    //
    // §14.2 "Derived-property constraints" (#2111): "the per-instantiation
    // re-check for those interior uses is subsumed by the constraint, and one
    // diagnostic (the constraint failure, at the instantiation site) replaces
    // the interior one." When the enclosing generic type declares a
    // `where <param>: hashable` constraint on THIS key parameter, the §14.2
    // constraint check at the enclosing type's instantiation site
    // (checkOneConstraint) owns the diagnostic, so the interior §3.6 rule-2
    // check here is skipped — a bad instantiation is reported exactly once.
    // Suppress ONLY when a matching `hashable` constraint is present on that
    // exact param: an unconstrained `Registry2<K>` whose body mentions
    // `Dictionary<K, int>` carries its own interior obligation and must still
    // fire here (silently dropping it would be a soundness regression).
    auto paramHasHashableConstraint = [](const TypeSymbol* enc,
                                         std::string_view pn) -> bool {
        if (!enc || !enc->decl) return false;  // imported type, no AST
        const auto* td = dynamic_cast<const ast::TypeDecl*>(enc->decl);
        if (!td) return false;
        for (const ast::GenericConstraint& gc : td->genericConstraints) {
            if (gc.typeParam != pn) continue;
            for (const ast::ConstraintItem& ci : gc.items)
                if (ci.kind == "hashable") return true;
        }
        return false;
    };
    std::unordered_set<const ast::Type*> reported2;
    for (const HashContainerSite& s : hashContainerSites_) {
        // Only unresolved-param keys (declaration == null); concrete keys were
        // handled in pass 1.
        if (!s.keyRef || s.keyRef->declaration) continue;
        if (s.keyRef->kind != TypeRef::Kind::Named) continue;
        if (!s.enclosingType) continue;
        // Recover the parameter name from the key's AST node.
        if (!s.keyRef->astNode ||
            s.keyRef->astNode->kind != ast::TypeKind::Named ||
            !s.keyRef->astNode->name ||
            s.keyRef->astNode->name->parts.size() != 1)
            continue;
        std::string_view paramName = s.keyRef->headName();
        // §14.2 (#2111): if the enclosing type declares `where <param>:
        // hashable`, the constraint check at the instantiation site owns the
        // diagnostic — skip the interior check (one diagnostic replaces the
        // interior one). See paramHasHashableConstraint above.
        if (paramHasHashableConstraint(s.enclosingType, paramName)) continue;
        // Find the index of this parameter in the enclosing type's param list.
        std::size_t paramIdx = s.enclosingType->genericParams.size();
        for (std::size_t i = 0; i < s.enclosingType->genericParams.size(); ++i) {
            if (s.enclosingType->genericParams[i] &&
                s.enclosingType->genericParams[i]->name == paramName) {
                paramIdx = i; break;
            }
        }
        if (paramIdx >= s.enclosingType->genericParams.size()) continue;
        // Walk all recorded instantiations of the enclosing type.
        for (const GenericInstantiation& inst : instantiations_) {
            if (inst.type != s.enclosingType) continue;
            if (paramIdx >= inst.argRefs.size()) continue;
            TypeRef* concreteKey = inst.argRefs[paramIdx];
            if (!concreteKey || concreteKey->kind == TypeRef::Kind::Error) continue;
            if (isEqualityBearing(concreteKey, ctx_)) continue;
            // Concrete key is not equality-bearing — report at the hash-container
            // annotation site (§3.6 rule 2 per-instantiation discipline).
            if (s.site && !reported2.insert(s.site).second) continue;
            const TypeSymbol* rec = concreteKey->declaration;
            std::string keyName =
                rec ? ("`" + std::string(rec->name) + "`") : std::string("it");
            ctx_.diagnostics().report(
                Severity::Error,
                s.site ? s.site->span.begin : SourceLocation{},
                std::string("`") + std::string(s.container) +
                "` requires an equality-bearing " + std::string(s.property) +
                " type, but the type argument " + keyName +
                " is not equality-bearing (\xC2\xA7""3.6/\xC2\xA7""8.3): " +
                reasonClause(concreteKey));
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
        }
        // §11.7 (v0.181, lifting an unrecorded leftover of the v0.55 ban):
        // a static class MAY declare `static owned readonly` fields (and
        // static owning-inline-array fields — those never carried the
        // field-level `owned` modifier this check inspects; `owned` there is
        // part of the inline-array TYPE, `[N owned]T`, so they were never
        // rejected here). Such fields participate in §22.13 program-exit
        // disposal exactly as on any other type: that mechanism is
        // runtime-driven and independent of instance `Dispose()`, so the
        // no-instantiation reasoning behind this function's other
        // rejections never reached it. No rejection belongs here anymore.
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
            auto it = genericSubst.find(tr->headName());
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

// #1592: is `mod` a parameter modifier that passes the argument BY
// REFERENCE — aliasing the caller's own storage — rather than by value?
// Mirrors codegen's Emitter::isByRefMod (LlvmBackend_impl.hpp); kept as an
// independent copy since sema and codegen are separate layers, but the
// modifier vocabulary (§22.3/§22.4) is shared.
static bool isByRefParamModifier(std::string_view mod) {
    return mod == "ref" || mod == "out" || mod == "out var" ||
           mod == "out owned" || mod == "out owned var" ||
           mod == "out using var";
}

// §12.1 (v0.181, #1560) implementation note / #1592: is `t` a widening
// candidate the codegen side can marshal safely, so a vtable call built from
// an ancestor's narrower static type can safely reach an override that
// widened this parameter?
//
// Reference types (class, interface, `string`, `Object`) always are: `T`
// and `T?` both lower to a plain (possibly-null) pointer, so no thunk is
// needed at the vtable slot regardless of which ancestor's declared type a
// given call site used to build its call.
//
// VALUE types (numeric/bool primitives, enums, records — codegen's
// isNullableValueTy) are NOT ABI-identical: `T?` lowers to a tagged
// `{i8 hasValue; T value}` struct, a different runtime representation than
// the bare `T` the original (un-widened) virtual declaration's ABI uses.
// #1592 closes this: `declareVtables` (LlvmBackend_Setup.cpp) now installs a
// per-slot ADAPTER THUNK whenever any override anywhere in the virtual
// slot's hierarchy widens a value-typed parameter — every contributing
// class's slot (including the un-overridden base's own, and any sibling
// that never widened) presents one uniform, family-wide ABI, and
// `emitMethodCall` (LlvmBackend_Calls2.cpp) builds every call to that slot
// against the SAME uniform ABI regardless of which ancestor's static type
// resolved the call, boxing a definite narrow value up when necessary. So a
// plain (by-value) VALUE-typed parameter is now ABI-safe to widen too.
//
// A `ref`/`out` parameter is the one case that stays unsafe even for a
// value type: it aliases the CALLER's own storage directly (no thunk-owned
// copy to marshal), and that storage is sized for the narrow, un-boxed `T` —
// there is no safe place to grow an extra hasValue byte. `isByRefParam`
// (threaded from the parameter's modifier, per the call sites below) keeps
// that case declined. Reference-typed by-ref parameters are unaffected: `T`
// and `T?` are both plain pointer-sized slots there regardless of the
// modifier, so no aliasing hazard exists for them.
static bool overrideWideningIsAbiSafe(const TypeRef* t, bool isByRefParam) {
    if (!t) return false;
    if (t->kind == TypeRef::Kind::Primitive) {
        if (t->primitive == PrimitiveType::Object ||
            t->primitive == PrimitiveType::String)
            return true;
        return !isByRefParam;  // #1592: numeric/bool value primitives.
    }
    if (t->kind == TypeRef::Kind::Named && t->declaration) {
        if (t->declaration->kind == ast::TypeDeclKind::Class ||
            t->declaration->kind == ast::TypeDeclKind::Interface)
            return true;
        if (t->declaration->kind == ast::TypeDeclKind::Enum ||
            t->declaration->kind == ast::TypeDeclKind::Record)
            return !isByRefParam;  // #1592: enum/record value types.
        return false;
    }
    // Unresolved / declaration-less Named type (a generic type parameter, or
    // a builtin whose concrete shape isn't visible here): the concrete
    // instantiation could be a value type behind a ref/out parameter, so
    // conservatively decline rather than risk the ABI corruption above.
    return false;
}

// §12.1 (v0.181, #1560): does a derived (override) parameter type MATCH a
// base parameter type — exactly, or via the one admitted widening (base `T`,
// derived `T?`)? Mirrors §11.2's interface-implementation-matching
// nullability allowance: "an implementation may declare a parameter as `T?`
// where the interface requires `T`" — an override may likewise accept more
// inputs than the virtual slot promises to pass. The reverse (base `T?`,
// derived `T`) is NOT a match — a narrowing override would let a `null`
// admitted through an intermediate static type reach a non-nullable
// parameter. Widening is monotone down the chain because callers always
// apply this predicate against the NEAREST overridden declaration: once a
// parameter is `T?`, the next override's base type IS `T?`, so this
// predicate demands the next override also declare `T?` (or narrower is
// rejected, matching exactly is fine, but there is no "T?→T" widening case).
// The widening is further restricted to ABI-safe types per
// overrideWideningIsAbiSafe above — reference types always qualify; a
// by-value VALUE type qualifies too as of #1592 (vtable adapter-thunk
// machinery); a by-ref (`ref`/`out`) VALUE-typed parameter still does not.
bool Resolver::overrideParamTypeMatches(const TypeRef* derivedTy,
                                        const TypeRef* baseTy,
                                        bool isByRefParam) {
    if (!derivedTy || !baseTy) return false;
    if (typeRefEqual(derivedTy, baseTy)) return true;
    // Only a base-non-nullable / derived-nullable pair can be a widening
    // match; the reverse, or any other kind of mismatch, is rejected.
    if (!derivedTy->nullable || baseTy->nullable) return false;
    if (!overrideWideningIsAbiSafe(baseTy, isByRefParam)) return false;
    // Compare structurally with the derived type's `?` stripped — a plain
    // stack copy is fine here, typeRefEqual only reads through the pointer
    // for the duration of this call (no arena allocation needed).
    TypeRef stripped = *derivedTy;
    stripped.nullable = false;
    return typeRefEqual(&stripped, baseTy);
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
        // #1592: dp->modifier == bp->modifier already (checked above), so
        // either symbol's modifier tells us whether this is a by-ref param.
        if (!overrideParamTypeMatches(dt, btSubst,
                                      isByRefParamModifier(dp->modifier)))
            return false;
    }
    return true;
}

// per §14.2 (v0.182, #1579): "the same constraint set per type parameter,
// compared order-insensitively and after type-alias resolution" — a null
// constraint pointer means the type parameter carries no `where` clause at
// all (the empty set, e.g. an omitted clause). Two same-`kind` "type" items
// are equal when their resolved TypeRefs are equal (typeRefEqual already
// looks through alias resolution, since ctx_.resolvedType resolves aliases
// transparently the first time the Type node is resolved — TypeResolution_
// Names.cpp's alias-copy-into-ref path).
bool Resolver::constraintSetsEquivalent(const ast::GenericConstraint* a,
                                        const ast::GenericConstraint* b) {
    static const std::vector<ast::ConstraintItem> kEmpty;
    const auto& ai = a ? a->items : kEmpty;
    const auto& bi = b ? b->items : kEmpty;
    std::string_view aMod = a ? a->modifierConstraint : std::string_view{};
    std::string_view bMod = b ? b->modifierConstraint : std::string_view{};
    if (aMod != bMod) return false;
    if (ai.size() != bi.size()) return false;
    std::vector<bool> used(bi.size(), false);
    for (const auto& ia : ai) {
        bool matched = false;
        for (std::size_t j = 0; j < bi.size(); ++j) {
            if (used[j]) continue;
            const auto& ib = bi[j];
            if (ia.kind != ib.kind) continue;
            if (ia.kind == "type") {
                TypeRef* ta = ia.typeArg ? ctx_.resolvedType(*ia.typeArg) : nullptr;
                TypeRef* tb = ib.typeArg ? ctx_.resolvedType(*ib.typeArg) : nullptr;
                if (!typeRefEqual(ta, tb)) continue;
            }
            used[j] = true;
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    return true;
}

void Resolver::checkOverrideSignatureMatch(MemberSymbol* derivedSym,
                                           MemberSymbol* baseSym) {
    auto* derived = dynamic_cast<MethodSymbol*>(derivedSym);
    auto* base = dynamic_cast<MethodSymbol*>(baseSym);
    if (!derived || !base) {
        // #1614 (§12.1): a property/indexer override must also match the
        // base member's signature. Previously this returned immediately for
        // any non-method pair, so a property/indexer override with a
        // mismatched property type, accessor presence, or indexer index-
        // parameter signature was silently accepted. Delegate to the
        // property/indexer check when BOTH members are PropertySymbols.
        auto* dp = dynamic_cast<PropertySymbol*>(derivedSym);
        auto* bp = dynamic_cast<PropertySymbol*>(baseSym);
        if (dp && bp) checkPropertyOverrideSignatureMatch(dp, bp);
        return;  // not a method pair — nothing else to check here.
    }
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
        // §22.4 (v0.183, #1585): the out-nullability postcondition is part of
        // the method's contract like the other parameter modifiers — an
        // override must repeat it (the §12 modifier-match rule).
        if (dp->outNullPostcond != bp->outNullPostcond) {
            err("`when` nullability postcondition");
            return;
        }
        TypeRef* dt = dp->declType ? ctx_.resolvedType(*dp->declType)
                                   : nullptr;
        TypeRef* bt = bp->declType ? ctx_.resolvedType(*bp->declType)
                                   : nullptr;
        // Apply substitution to base parameter type. §12.1 (v0.181, #1560):
        // a parameter may widen the base's `T` to `T?` (monotone down the
        // chain — see overrideParamTypeMatches); every other mismatch,
        // including the reverse `T?`→`T` narrowing, is rejected.
        TypeRef* btSubst = applyGenericSubst(bt, genericSubst);
        // #1592: dp->modifier == bp->modifier already (checked above).
        if (dt && btSubst &&
            !overrideParamTypeMatches(dt, btSubst,
                                      isByRefParamModifier(dp->modifier))) {
            err("parameter types");
            return;
        }
    }
    // per §14.2 (v0.182, #1579): "An `override` of a generic method ...
    // inherits the overridden ... declaration's constraints. Omitting the
    // `where` clauses is the recommended spelling and means exactly the
    // inherited set; the clauses may instead be restated in an equivalent
    // form ... A differing clause — added, removed, narrowed, or widened
    // relative to the inherited set — is a compile error." Matched
    // positionally by the METHOD's own generic-parameter list (the `T` in
    // `method Foo<T>` on both sides), not the enclosing class's generic
    // substitution — a method-level type parameter is fresh per declaration
    // regardless of name reuse.
    if (dDecl && bDecl &&
        dDecl->genericParams.size() == bDecl->genericParams.size()) {
        for (std::size_t gi = 0; gi < dDecl->genericParams.size(); ++gi) {
            std::string_view dName = dDecl->genericParams[gi].name;
            std::string_view bName = bDecl->genericParams[gi].name;
            const ast::GenericConstraint* dc = nullptr;
            for (const auto& gc : dDecl->genericConstraints)
                if (gc.typeParam == dName) { dc = &gc; break; }
            // No `where` clause on the OVERRIDE for this type parameter at
            // all means "exactly the inherited set" — always legal, the
            // recommended spelling; nothing to compare.
            if (!dc) continue;
            const ast::GenericConstraint* bc = nullptr;
            for (const auto& gc : bDecl->genericConstraints)
                if (gc.typeParam == bName) { bc = &gc; break; }
            if (!constraintSetsEquivalent(dc, bc)) {
                ctx_.diagnostics().report(
                    Severity::Error, derived->declLoc,
                    std::string("override of `") + std::string(derived->name) +
                    "` restates a `where " + std::string(dName) +
                    ": ...` clause that differs from the inherited "
                    "constraint set — omit the clause to inherit it exactly, "
                    "or restate the identical set (§14.2)");
                return;
            }
        }
    }
}

// #1614 / §12.1: does a property/indexer declaration have a setter? Mirrors
// codegen's Emitter::propertyHasSetter (LlvmBackend_impl.hpp) for the
// concrete case, with one semantic refinement for the override-match: an
// abstract Auto-shorthand property (`abstract property Name: T;`) declares a
// single abstract getter (get-only) — the conventional reading of the §12.5
// shorthand. (Codegen's predicate reports a spurious setter for such a
// declaration because the Auto body has no `readonly`; that allocates an
// empty setter vtable slot, which is harmless — an abstract class is never
// instantiated and an abstract get-only property is never assigned. The
// override check must use the semantic accessor shape, not the spurious slot,
// or it would reject the common `abstract property P: T;` / `override
// property P: T => e;` idiom.) An expression-bodied property is get-only; a
// concrete auto-property is read-write unless `readonly`; a block-body
// property has a setter iff it declares a `set` arm.
static bool propertyDeclHasSetter(const ast::PropertyDecl* pd) {
    if (!pd) return false;
    if (pd->bodyKind == ast::PropertyBodyKind::Expression) return false;
    if (pd->bodyKind == ast::PropertyBodyKind::Block) {
        for (const auto& a : pd->accessors)
            if (a.isSet) return true;
        return false;
    }
    // Auto body.
    bool isAbstract = false;
    for (auto mod : pd->modifiers)
        if (mod == "abstract") { isAbstract = true; break; }
    if (isAbstract) return false;  // §12.5 shorthand = get-only abstract getter
    for (auto mod : pd->modifiers)
        if (mod == "readonly") return false;
    return true;
}

void Resolver::checkPropertyOverrideSignatureMatch(PropertySymbol* derived,
                                                   PropertySymbol* base) {
    auto err = [&](const std::string& what) {
        ctx_.diagnostics().report(
            Severity::Error, derived->declLoc,
            std::string("override of `") +
            (derived->isIndexer ? std::string("indexer")
                                : std::string(derived->name)) +
            "` does not match the base member's " + what + " (§12.1)");
    };

    // Build the enclosing-class generic substitution map (base class T →
    // concrete arg the derived class binds it to), the property analog of
    // the method check's buildBaseGenericSubst. A property/indexer carries
    // no own generic parameters, so only the enclosing-class substitution
    // applies.
    std::unordered_map<std::string_view, TypeRef*> genericSubst;
    if (derived->parent && base->parent) {
        for (std::size_t bi = 0; bi < derived->parent->baseTypes.size();
             ++bi) {
            if (derived->parent->baseTypes[bi] != base->parent) continue;
            TypeRef* baseRef =
                (bi < derived->parent->baseTypeRefs.size())
                    ? derived->parent->baseTypeRefs[bi]
                    : nullptr;
            if (!baseRef) break;
            for (std::size_t gi = 0;
                 gi < base->parent->genericParams.size() &&
                 gi < baseRef->genericArgs.size();
                 ++gi) {
                if (auto* gp = dynamic_cast<GenericParamSymbol*>(
                        base->parent->genericParams[gi])) {
                    genericSubst[gp->name] = baseRef->genericArgs[gi];
                }
            }
            break;
        }
    }

    // Visibility must match exactly (per §12.1, same as methods).
    if (derived->visibility != base->visibility) {
        err("visibility");
    }

    auto* dDecl = dynamic_cast<const ast::PropertyDecl*>(derived->decl);
    auto* bDecl = dynamic_cast<const ast::PropertyDecl*>(base->decl);
    if (dDecl && bDecl) {
        // The property type is the getter's return type (and the setter's
        // implicit `value` parameter type). Per §12.1, return types must
        // match exactly — no covariance in this version — so no T→T?
        // widening here (unlike a method parameter or an indexer index
        // parameter, which are inputs).
        TypeRef* dTy =
            dDecl->type ? ctx_.resolvedType(*dDecl->type) : nullptr;
        TypeRef* bTy =
            bDecl->type ? ctx_.resolvedType(*bDecl->type) : nullptr;
        TypeRef* bTySubst = applyGenericSubst(bTy, genericSubst);
        if (!typeRefEqual(dTy, bTySubst)) {
            err("property type");
        }

        // Accessor presence must match (§12.1): a property that adds or
        // removes a setter relative to the base member changes the vtable
        // slot shape — codegen installs a separate `$set` slot only when a
        // property has a setter (LlvmBackend_Setup.cpp), so a mismatched
        // override leaves a slot unfilled or orphaned. propertyDeclHasSetter
        // is the same predicate codegen uses to decide slot allocation.
        if (propertyDeclHasSetter(dDecl) != propertyDeclHasSetter(bDecl)) {
            err("accessor set/get presence");
        }
    }

    // Indexer index-parameter signature: count, modifiers, and types (with
    // the §12.1 T→T? parameter widening a method override admits). The index
    // parameters are bona fide parameters, so the widening applies to them
    // just as it does to a method's.
    if (derived->isIndexer && base->isIndexer) {
        if (derived->indexerParams.size() != base->indexerParams.size()) {
            err("index parameter count");
            return;
        }
        for (std::size_t i = 0; i < derived->indexerParams.size(); ++i) {
            auto* dp =
                dynamic_cast<ParameterSymbol*>(derived->indexerParams[i]);
            auto* bp =
                dynamic_cast<ParameterSymbol*>(base->indexerParams[i]);
            if (!dp || !bp) continue;
            if (dp->modifier != bp->modifier) {
                err("index parameter modifiers");
                return;
            }
            // §22.4 (v0.183, #1585): the out-nullability postcondition is
            // part of the contract like the other parameter modifiers — an
            // override must repeat it.
            if (dp->outNullPostcond != bp->outNullPostcond) {
                err("`when` nullability postcondition");
                return;
            }
            TypeRef* dt =
                dp->declType ? ctx_.resolvedType(*dp->declType) : nullptr;
            TypeRef* bt =
                bp->declType ? ctx_.resolvedType(*bp->declType) : nullptr;
            TypeRef* btSubst = applyGenericSubst(bt, genericSubst);
            if (dt && btSubst &&
                !overrideParamTypeMatches(dt, btSubst,
                                          isByRefParamModifier(dp->modifier))) {
                err("index parameter types");
                return;
            }
        }
    }
}

// §12.2 (v0.184): is `inner` nested (directly or transitively) inside
// `outer`? Mirrors ExprTypecheck_Calls.cpp's isNestedWithin, which backs the
// same §11.6 mutual-access rule for ordinary member access — kept as a
// separate file-local (internal-linkage) copy here since the two checkers
// live in different translation units with no shared utility header for a
// helper this narrow.
static bool isNestedWithin(const TypeSymbol* inner, const TypeSymbol* outer) {
    if (!inner || !outer) return false;
    for (const Symbol* m : outer->members) {
        if (auto* nts = dynamic_cast<const NestedTypeSymbol*>(m)) {
            if (nts->type == inner) return true;
            if (nts->type && isNestedWithin(inner, nts->type)) return true;
        }
    }
    return false;
}

// §12.2 (v0.184 — "The blocked-name set"): does `baseMember`, declared on
// `declaringBase`, block `derived` from reusing its name? `public` and
// `protected` base members always block (a subclass reached `declaringBase`
// by inheriting from it, so a `protected` member of `declaringBase` is
// always visible to `derived`). An `internal` base member blocks exactly
// when it is visible at the derived declaration site — same module, i.e.
// not an imported symbol (§13.4, mirroring ExprTypecheck_Calls.cpp's
// checkMemberVisibility internal case). A `private` base member never
// blocks, EXCEPT that a derived class nested inside the base class (§11.6)
// sees the base's private members and IS blocked by them. When a base
// member does not block, it is invisible at the derived declaration site
// and the derived declaration is a distinct member: no override
// relationship and no shared overload family exist between the two, so
// such a member must not be collected as a hiding/override candidate at
// all (not just suppressed at diagnostic time).
static bool memberBlocksHiding(SemaContext& ctx, TypeSymbol* derived,
                                TypeSymbol* declaringBase,
                                MemberSymbol* baseMember) {
    switch (baseMember->visibility) {
        case ast::Visibility::Public:
        case ast::Visibility::None:
        case ast::Visibility::Protected:
            return true;
        case ast::Visibility::Internal:
            return !ctx.isImported(baseMember);
        case ast::Visibility::Private:
            return isNestedWithin(derived, declaringBase);
    }
    return true;
}

// §12.2 / #1749: collect `t` (if it's a class) plus every class reachable
// through its NestedTypeSymbol members, transitively — §11.6 states nested
// types "may themselves contain nested types" with no depth bound, so a
// class nested inside a nested class must be visited too, not just the
// module's direct children and their immediate nested members (the shallow,
// one-level pattern several sibling checks in this file use, e.g.
// checkConstTypes above). isNestedWithin (used below by
// memberBlocksHiding's `private` case) already assumes this transitive
// reach; the outer walk driving checkHidingAndOverride was the one piece
// that never visited nested classes at all.
static void collectHidingCheckClasses(TypeSymbol* t,
                                       std::vector<TypeSymbol*>& out) {
    if (!t) return;
    if (t->kind == ast::TypeDeclKind::Class) out.push_back(t);
    for (Symbol* m : t->members) {
        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
            if (nt->type) collectHidingCheckClasses(nt->type, out);
        }
    }
}

void Resolver::checkHidingAndOverride() {
    if (!ctx_.module()) return;
    std::vector<TypeSymbol*> classes;
    for (Symbol* child : ctx_.module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            collectHidingCheckClasses(t, classes);
        }
    }
    for (TypeSymbol* t : classes) {
        // Gather base CLASS members by name (transitive). A name can map to
        // SEVERAL members — overloaded virtual methods (#1090) — so collect all
        // of them (most-derived first) and pick the matching overload below.
        std::unordered_map<std::string_view, std::vector<MemberSymbol*>>
            baseMembers;
        // §12.2 (v0.181, #1562): operator declarations are covered by the
        // no-hiding rule PER OPERATOR TOKEN — a derived class may not declare an
        // `operator <op>` overload when any base class declares an overload of
        // the same token, whatever the parameter types. An operator symbol's
        // `name` is its token (SymbolCollection sets `name = opToken`), so a
        // derived operator hiding a base one is *already* caught by the generic
        // name-keyed path below — but with an operator-inappropriate message
        // ("choose a different name"). Collect base operators separately, keyed
        // by token, so the derived side can emit a per-token §12.2 diagnostic
        // instead, and so the rule is enforced explicitly rather than by the
        // incidental name==token coincidence. This is what keeps §25.2's
        // inherited-operator candidate set duplicate-free within one chain.
        std::unordered_map<std::string_view, MethodSymbol*> baseOperators;
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
                // §12.2 (v0.181, #1562): route base operators to the per-token
                // map and out of the name-keyed collection, so the derived side
                // handles them with the operator-specific diagnostic (below)
                // rather than the generic name-hiding message. emplace keeps the
                // first-seen (nearest, most-derived-first) declaration.
                if (auto* bop = dynamic_cast<MethodSymbol*>(m);
                    bop && bop->isOperator) {
                    baseOperators.emplace(bop->operatorToken, bop);
                    continue;
                }
                if (auto* ms = dynamic_cast<MemberSymbol*>(m)) {
                    // §12.2 (v0.184): only a base member VISIBLE at `t`'s
                    // declaration site can block its name — an invisible
                    // member (typically `private`, not reachable via
                    // nesting) is not collected at all, so it neither
                    // diagnoses a hiding conflict nor stands in as an
                    // override target.
                    if (!memberBlocksHiding(ctx_, t, cur, ms)) continue;
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
        if (baseMembers.empty() && baseOperators.empty()) continue;
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
            // §12.2 (v0.181, #1562): a derived operator redeclaring a token any
            // base class already declares is a compile error — operators are
            // static (§12.1: static members cannot be virtual), so no `override`
            // escape exists, and the redeclaration would inject a base-vs-derived
            // intra-chain ambiguity into §25.2's candidate set that the spec
            // declares undeclarable. The prohibition is per token, whatever the
            // parameter types.
            if (auto* dop = dynamic_cast<MethodSymbol*>(m);
                dop && dop->isOperator) {
                auto it = baseOperators.find(dop->operatorToken);
                if (it != baseOperators.end()) {
                    std::string baseCls =
                        (it->second->parent && !it->second->parent->name.empty())
                            ? std::string(it->second->parent->name)
                            : "a base class";
                    ctx_.diagnostics().report(
                        Severity::Error, dop->declLoc,
                        std::string("operator `") +
                        std::string(dop->operatorToken) +
                        "` is already declared in `" + baseCls +
                        "`; hiding is forbidden — an operator token declared in "
                        "a base class may not be redeclared in a derived class "
                        "(operators are static and cannot be overridden) (§12.2)");
                }
                continue;  // operators handled here; not name-keyed below
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
                // §12.1 (v0.181, #1560): pick the NEAREST ancestor class that
                // declares an overload of this arity (bases is most-derived
                // first), disambiguating same-arity candidates WITHIN that
                // one class by full signature (#1090's original concern —
                // several arities live across the chain). Once a nearer
                // class has contributed an arity match, do NOT fall through
                // to a farther ancestor even if it happens to signature-match
                // better: matching anything but the nearest overridden
                // declaration would let a derived override bypass an
                // intermediate override's widened (`T?`) parameter and
                // validate against the original (narrower) declaration
                // instead — silently admitting a narrowing override and
                // breaking the monotone-widening rule.
                TypeSymbol* nearestClass = nullptr;
                MemberSymbol* arityMatch = nullptr;
                for (MemberSymbol* cand : bases) {
                    auto* bm = dynamic_cast<MethodSymbol*>(cand);
                    if (!bm || bm->params.size() != dm->params.size()) continue;
                    if (nearestClass && cand->parent != nearestClass) break;
                    nearestClass = cand->parent;
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
                    // §22.15 (v0.202, #2052) dispatch rule 1: an override
                    // of a pure base member must itself be declared pure.
                    // Rule 2 (strengthening) is permissive — an override
                    // MAY declare pure where the base does not.
                    if (hasMod(base, "pure") && !hasMod(ms, "pure")) {
                        ctx_.diagnostics().report(
                            Severity::Error, ms->declLoc,
                            std::string("override of pure member `") +
                            std::string(ms->name) +
                            "` must be declared `pure` — purity is never "
                            "dropped down an override chain (\xC2\xA7""22.15)");
                    }
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

    // #2186 / §11.2 / §12.5: per-instantiation interface-obligation machinery.
    // The §11.2 matching rule compares parameter and return types UNDER THE
    // INTERFACE'S SUBSTITUTION, so an interface conformance declaration
    // (`D : K<string>, IM<int>`) cannot be silently satisfied by an inherited
    // member of the wrong instantiation (K<string>::Get(): string? for
    // IM<int>::Get(): int?). Before #2186 the check compared name+arity+
    // modifier only; this machinery mirrors `checkStaticInterfaceConformance`'s
    // local helpers — same recursive substitution and the same structural
    // equality that distinguishes declaration-less builtins/bare type params
    // by spelling (`typeRefEqual` would conflate them, treating two null-
    // declaration Named refs of the same shape as equal).
    std::function<TypeRef*(TypeRef*,
                           const std::unordered_map<std::string_view, const TypeRef*>&)>
        substTypeRef = [&](TypeRef* d,
                           const std::unordered_map<std::string_view, const TypeRef*>& map)
        -> TypeRef* {
        if (!d) return d;
        if (d->kind != TypeRef::Kind::Named) return d;
        if (!d->declaration && d->astNode && d->astNode->name &&
            d->astNode->name->parts.size() == 1 && d->genericArgs.empty()) {
            auto it = map.find(d->headName());
            if (it != map.end() && it->second) {
                const TypeRef* s = it->second;
                if (d->nullable && !s->nullable) {
                    TypeRef* nn = ctx_.makeTypeRef(*s);
                    nn->nullable = true;
                    return nn;
                }
                return const_cast<TypeRef*>(s);
            }
            return d;
        }
        if (!d->genericArgs.empty()) {
            bool changed = false;
            std::vector<TypeRef*> newArgs;
            newArgs.reserve(d->genericArgs.size());
            for (TypeRef* ga : d->genericArgs) {
                TypeRef* s = substTypeRef(ga, map);
                changed = changed || (s != ga);
                newArgs.push_back(s);
            }
            if (changed) {
                TypeRef* nn = ctx_.makeTypeRef(*d);
                nn->genericArgs = std::move(newArgs);
                return nn;
            }
        }
        return d;
    };
    std::function<bool(const TypeRef*, const TypeRef*)> typesMatch =
        [&](const TypeRef* a, const TypeRef* b) -> bool {
        if (!a || !b) return false;
        if (a->kind == TypeRef::Kind::Named && b->kind == TypeRef::Kind::Named &&
            !a->declaration && !b->declaration) {
            auto spell = [](const TypeRef* t) -> std::string_view {
                if (t->astNode && t->astNode->name &&
                    !t->astNode->name->parts.empty())
                    return t->astNode->name->parts.back();
                return t->importedName;
            };
            if (spell(a) != spell(b)) return false;
            if (a->nullable != b->nullable) return false;
            if (a->genericArgs.size() != b->genericArgs.size()) return false;
            for (std::size_t i = 0; i < a->genericArgs.size(); ++i)
                if (!typesMatch(a->genericArgs[i], b->genericArgs[i]))
                    return false;
            return true;
        }
        return typeRefEqual(a, b);
    };
    auto resolvedOf = [&](const ast::Type* node) -> TypeRef* {
        return node ? ctx_.resolvedType(*node) : nullptr;
    };

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
        // #2186 / §11.2 / §12.5: per-obligation record. The interface's own
        // method (declared with bare type-parameter types) plus the SUBSTITUTION
        // the conformance declaration induces for that interface instantiation
        // (`: IM<int>` → {T → int}). Applied to the interface member's signature
        // to compare against the (similarly substituted) implementation's — the
        // §11.2 matching rule is exact-under-substitution, not name+arity only.
        // Carrying the substitution per obligation is what lets a generic
        // interface implemented at TWO instantiations (D : K<string>, IM<int>)
        // record TWO separate obligations; the flat name-only set previously
        // used satisfied both with whichever same-named member the BFS hit first.
        struct IfaceObligation {
            MethodSymbol* method;
            TypeSymbol* iface;
            std::unordered_map<std::string_view, const TypeRef*> subst;
        };
        std::unordered_map<std::string_view, std::vector<IfaceObligation>>
            ifaceObligationsByName;
        // #2186: per-class substitution accumulated during the BFS, looked up
        // when verifying a conformance to substitute the impl-side signature
        // the same way. First-path-wins (a class declares one member set; the
        // subst only changes how those members' types are READ); the same class
        // reached via two paths with different substs is rare and not the
        // #2186 shape (which is about interfaces implemented twice).
        std::unordered_map<TypeSymbol*,
                           std::unordered_map<std::string_view, const TypeRef*>>
            classSubst;
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
        // #2186: BFS over the inheritance chain tracking the per-node
        // substitution the path induces. Dedup is by (TypeSymbol, subst-key)
        // — not TypeSymbol alone — so a generic interface reached through two
        // paths at different instantiations is visited once PER instantiation
        // (two separate obligation sets, the crux of #2186). Mirrors
        // instanceDescriptorFor's BFS in LlvmBackend_Setup.cpp (which built
        // the per-monomorphization ancestor array under the same constraint).
        struct FrontierNode {
            TypeSymbol* sym;
            std::unordered_map<std::string_view, const TypeRef*> subst;
        };
        auto substKey = [&](
            const std::unordered_map<std::string_view, const TypeRef*>& m)
            -> std::string {
            if (m.empty()) return std::string();
            std::vector<std::pair<std::string_view, std::string>> parts;
            parts.reserve(m.size());
            for (auto& [k, v] : m) {
                parts.emplace_back(k,
                    v ? std::string(v->headName()) : std::string());
            }
            std::sort(parts.begin(), parts.end());
            std::string out;
            for (auto& [k, v] : parts) {
                out += std::string(k);
                out += "=";
                out += v;
                out += ";";
            }
            return out;
        };
        auto seenKey = [&](TypeSymbol* s,
                           const std::unordered_map<std::string_view,
                                                    const TypeRef*>& m)
            -> std::string {
            return std::to_string(reinterpret_cast<std::uintptr_t>(s)) +
                   "#" + substKey(m);
        };
        std::vector<FrontierNode> frontier;
        frontier.push_back({t, {}});
        std::unordered_set<std::string> seen;
        seen.insert(seenKey(t, {}));
        std::size_t i = 0;
        while (i < frontier.size()) {
            FrontierNode node = frontier[i++];  // copy — frontier may realloc
            TypeSymbol* cur = node.sym;
            auto curSubst = node.subst;  // copy — base-loop below may realloc
            if (cur->kind != ast::TypeDeclKind::Interface) {
                // First-path-wins for the impl-side substitution lookup.
                classSubst.emplace(cur, curSubst);
            }
            bool curIsIface = (cur->kind == ast::TypeDeclKind::Interface);
            for (Symbol* m : cur->members) {
                if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                    // §11.8 (v0.176, #807): STATIC interface members (static
                    // methods and operators — every interface operator is
                    // static) are NOT instance obligations; their conformance
                    // is checked signature-exactly by
                    // checkStaticInterfaceConformance. Operators never
                    // participate in the instance abstract-member set either
                    // way (they dispatch by operand type, not by name).
                    if (ms->isOperator) continue;
                    if (curIsIface && hasMod(ms, "static")) continue;
                    if (methodIsAbstract(ms)) {
                        abstractNames.insert(ms->name);
                        abstractArities[ms->name].insert(ms->params.size());
                        // #1685: record the interface's own MethodSymbol so
                        // the `scoped` match below can inspect its per-
                        // parameter modifiers directly (curIsIface implies
                        // every non-static member here is interface-declared,
                        // since methodIsAbstract is true for ALL interface
                        // members — see methodIsAbstract above).
                        // #2186: carry this visit's substitution so the
                        // signature comparison below substitutes both sides.
                        if (curIsIface) {
                            ifaceObligationsByName[ms->name].push_back(
                                IfaceObligation{ms, cur, curSubst});
                        }
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
            for (std::size_t bi = 0; bi < cur->baseTypes.size(); ++bi) {
                TypeSymbol* b = cur->baseTypes[bi];
                if (!b) continue;
                // #2186: bind b's generic parameters to the (already-
                // substituted-through-curSubst) generic args of cur's base
                // ref. `: IM<int>` at the root binds IM's T → int; the same
                // binding through an inherited `: IM<T>` under {T → string}
                // gives IM's T → string — two separate obligations, the
                // crux of #2186.
                std::unordered_map<std::string_view, const TypeRef*> bSubst;
                if (bi < cur->baseTypeRefs.size()) {
                    TypeRef* bref = cur->baseTypeRefs[bi];
                    if (bref &&
                        b->genericParams.size() == bref->genericArgs.size()) {
                        for (std::size_t gi = 0; gi < b->genericParams.size();
                             ++gi) {
                            if (b->genericParams[gi] && bref->genericArgs[gi]) {
                                bSubst[b->genericParams[gi]->name] =
                                    substTypeRef(bref->genericArgs[gi], curSubst);
                            }
                        }
                    }
                }
                std::string key = seenKey(b, bSubst);
                if (!seen.insert(key).second) continue;
                frontier.push_back(FrontierNode{b, std::move(bSubst)});
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
        // #1685 / §11.2 (v0.184), generalized by #1750: "A member matches an
        // interface member when it ... has the same parameter modifiers
        // (`ref`, `out`, `owned`, `scoped` — v0.184) ... Parameter modifiers
        // must match exactly." #1685 only compared the `scoped` bit
        // (`ip->modifier == "scoped"` on each side); an `owned`/`ref`/`out`
        // mismatch that didn't ALSO involve `scoped` fell through both sides
        // of that comparison as `false == false` and was silently accepted —
        // e.g. interface `Make(owned x: T)` vs implementation `Make(x: T)`,
        // an undischarged-ownership hazard the rationale calls out by name
        // (§11.2 "`owned` in interface signatures": "Tightening from `T` to
        // `owned T` in a parameter would create undischarged ownership").
        // ParameterSymbol::modifier is a single mutually-exclusive spelling
        // per parameter ("", "ref", "out", "out owned", "owned", "scoped" —
        // see Parser_Decls.cpp param-modifier parsing), so a plain string
        // compare covers every modifier axis in one shot, exactly mirroring
        // checkOverrideSignatureMatch's `dp->modifier != bp->modifier`
        // (§12.1) and checkStaticInterfaceConformance's `ip->modifier !=
        // cp->modifier` (§11.8) for the instance/interface path. Compared
        // against the SAME concrete candidate codegen's `findInterfaceImpl`
        // (LlvmBackend_impl.hpp) wires into the itable slot: most-derived
        // class first, first same-arity overload in declaration order.
        for (auto& [name, obligations] : ifaceObligationsByName) {
            for (auto& obl : obligations) {
                MethodSymbol* im = obl.method;
                MethodSymbol* impl = nullptr;
                MethodSymbol* nameOnly = nullptr;
                // #2186: track the declaring class of the chosen impl so its
                // signature can be substituted through the SAME path-induced
                // substitution the BFS recorded for that class (the impl is
                // the unspecialized template's MethodSymbol; its bare type-
                // parameter types read correctly only under that subst).
                TypeSymbol* implClass = nullptr;
                TypeSymbol* c = t;
                while (c && !impl) {
                    for (Symbol* m : c->members) {
                        auto* ms = dynamic_cast<MethodSymbol*>(m);
                        if (!ms || ms->isOperator || hasMod(ms, "static") ||
                            ms->name != im->name || methodIsAbstract(ms))
                            continue;
                        if (ms->params.size() == im->params.size()) {
                            impl = ms;
                            implClass = c;
                            break;
                        }
                        if (!nameOnly) nameOnly = ms;
                    }
                    if (impl) break;
                    TypeSymbol* nextClass = nullptr;
                    for (TypeSymbol* b : c->baseTypes) {
                        if (b && b->kind == ast::TypeDeclKind::Class) {
                            nextClass = b;
                            break;
                        }
                    }
                    c = nextClass;
                }
                if (!impl) impl = nameOnly;
                if (!impl || impl->params.size() != im->params.size())
                    continue;  // missing/arity mismatch already reported above
                std::unordered_map<std::string_view, const TypeRef*> implSubst;
                auto csIt = classSubst.find(implClass);
                if (csIt != classSubst.end()) implSubst = csIt->second;
                auto describeModifier = [](std::string_view m) -> std::string {
                    return m.empty() ? std::string("no modifier")
                                      : ("`" + std::string(m) + "`");
                };
                for (std::size_t pi = 0; pi < im->params.size(); ++pi) {
                    auto* ip = dynamic_cast<ParameterSymbol*>(im->params[pi]);
                    auto* cp = dynamic_cast<ParameterSymbol*>(impl->params[pi]);
                    if (!ip || !cp) continue;
                    if (ip->modifier == cp->modifier) continue;
                    ctx_.diagnostics().report(
                        Severity::Error, impl->declLoc,
                        std::string("method `") + std::string(name) +
                        "` does not match interface member `" +
                        std::string(name) + "`'s parameter modifier on "
                        "parameter " + std::to_string(pi + 1) +
                        " — `ref`/`out`/`owned`/`scoped` must match exactly "
                        "(§11.2): the interface declares it " +
                        describeModifier(ip->modifier) +
                        "; the implementation declares it " +
                        describeModifier(cp->modifier) + " (§11.2)");
                    break;  // one diagnostic per member is enough
                }
                // §11.2 "owned in interface signatures": "an implementing
                // method's signature and the interface method's signature
                // must agree on ... `owned` on a return type." Loosening
                // `OwningTask<T>`/`owned` off a return would leak ownership
                // at the caller (§11.2 reason 1); tightening it on desyncs
                // the itable caller's disposal contract the other direction.
                // Compare the AST-level `returnOwned` bit the same way
                // checkOverrideSignatureMatch does for `override`
                // (`dDecl->returnOwned != bDecl->returnOwned`, §12.1).
                auto* imDecl = dynamic_cast<const ast::MethodDecl*>(im->decl);
                auto* implDecl =
                    dynamic_cast<const ast::MethodDecl*>(impl->decl);
                if (imDecl && implDecl &&
                    imDecl->returnOwned != implDecl->returnOwned) {
                    ctx_.diagnostics().report(
                        Severity::Error, impl->declLoc,
                        std::string("method `") + std::string(name) +
                        "` does not match interface member `" +
                        std::string(name) + "`'s `owned` return modifier "
                        "(§11.2): the interface declares the return type " +
                        (imDecl->returnOwned
                             ? std::string("`owned`")
                             : std::string("without `owned`")) +
                        "; the implementation declares it " +
                        (implDecl->returnOwned
                             ? std::string("`owned`")
                             : std::string("without `owned`")) +
                        " (§11.2)");
                }
                // §22.15 (v0.202, #2052) dispatch rule 1: a class member
                // implementing a pure interface member must be declared
                // pure. Rule 2 (strengthening) is permissive. Operators
                // implementing pure static interface operators are also
                // covered (the OperatorDecl AST carries isPure).
                const bool imPure = hasMod(im, "pure");
                bool implPure = hasMod(impl, "pure");
                if (!implPure)
                    if (auto* op = dynamic_cast<const ast::OperatorDecl*>(
                            impl->decl))
                        implPure = op->isPure;
                if (imPure && !implPure) {
                    ctx_.diagnostics().report(
                        Severity::Error, impl->declLoc,
                        std::string("method `") + std::string(name) +
                        "` implements pure interface member `" +
                        std::string(name) +
                        "` and must be declared `pure` — purity is never "
                        "dropped across an interface implementation "
                        "(\xC2\xA7""22.15)");
                }
                // #2186 / §11.2: parameter types must match exactly UNDER THE
                // INTERFACE'S SUBSTITUTION — and the impl's substitution. This
                // is the substituted-signature comparison the name+arity+modifier
                // check above was missing; without it, an inherited member of
                // the wrong instantiation silently satisfied a second generic-
                // interface conformance (D : K<string>, IM<int> with no
                // Get(): int?). Parameter nullability widening (interface T,
                // implementation T?) is admitted per §11.2's contravariant-
                // parameter allowance; everything else must match exactly.
                for (std::size_t pi = 0; pi < im->params.size(); ++pi) {
                    auto* ip = dynamic_cast<ParameterSymbol*>(im->params[pi]);
                    auto* cp = dynamic_cast<ParameterSymbol*>(impl->params[pi]);
                    if (!ip || !cp) continue;
                    if (ip->modifier != cp->modifier) continue;  // reported above
                    TypeRef* it = resolvedOf(ip->declType);
                    TypeRef* ct = resolvedOf(cp->declType);
                    if (!it || !ct) continue;
                    if (it->kind == TypeRef::Kind::Error ||
                        ct->kind == TypeRef::Kind::Error) continue;
                    const TypeRef* itSub = substTypeRef(it, obl.subst);
                    const TypeRef* ctSub = substTypeRef(ct, implSubst);
                    // §11.2 widening: impl may declare T? where interface
                    // requires T (the implementation accepts more inputs).
                    bool match = typesMatch(itSub, ctSub);
                    if (!match && itSub && ctSub &&
                        !itSub->nullable && ctSub->nullable) {
                        TypeRef stripped = *ctSub;
                        stripped.nullable = false;
                        match = typesMatch(itSub, &stripped);
                    }
                    if (match) continue;
                    ctx_.diagnostics().report(
                        Severity::Error, impl->declLoc,
                        std::string("method `") + std::string(name) +
                        "` does not match interface `" +
                        std::string(obl.iface->name) + "`'s member `" +
                        std::string(name) + "` on parameter " +
                        std::to_string(pi + 1) + " — parameter types must "
                        "match exactly under the interface's substitution, "
                        "with only the T -> T? nullability widening admitted "
                        "(\xC2\xA7""11.2)");
                    break;  // one diagnostic per member is enough
                }
                // #2186 / §11.2: return type must match exactly UNDER
                // SUBSTITUTION (no covariance, no widening — §11.2 is
                // strict on returns). A mismatch here is the surface of
                // wrong-typed interface dispatch the issue calls out (a
                // statically int?-typed call dispatching to a string?-
                // returning body).
                const ast::Type* imRetNode =
                    imDecl ? imDecl->returnType.get() : nullptr;
                const ast::Type* implRetNode =
                    implDecl ? implDecl->returnType.get() : nullptr;
                if (imRetNode && implRetNode) {
                    TypeRef* irt = resolvedOf(imRetNode);
                    TypeRef* crt = resolvedOf(implRetNode);
                    if (irt && crt &&
                        irt->kind != TypeRef::Kind::Error &&
                        crt->kind != TypeRef::Kind::Error) {
                        const TypeRef* irtSub = substTypeRef(irt, obl.subst);
                        const TypeRef* crtSub = substTypeRef(crt, implSubst);
                        if (!typesMatch(irtSub, crtSub)) {
                            ctx_.diagnostics().report(
                                Severity::Error, impl->declLoc,
                                std::string("method `") + std::string(name) +
                                "` does not match interface `" +
                                std::string(obl.iface->name) + "`'s return "
                                "type — return types must match exactly "
                                "(no covariance) under the interface's "
                                "substitution (\xC2\xA7""11.2)");
                        }
                    }
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

// §11.8 / §14.2 (v0.176, #807): re-run the type-bound BFS for every failure
// deferred during resolution (see checkOneConstraint). All baseTypes are
// populated by now, so a self-/mutually-referential conformance (the
// recurrence `class Meters: INumber<Meters>`) passes and only genuine
// violations report. Entries appended during checkDeferredNestedConstraints
// are flushed by the same loop (the driver calls this after it).
void Resolver::recheckDeferredTypeBounds() {
    for (const auto& d : deferredTypeBoundFailures_) {
        if (!argSatisfiesTypeBound(d.arg, d.bound)) {
            ctx_.diagnostics().report(
                Severity::Error, d.loc,
                std::string("type argument `") + d.argName +
                "` does not satisfy the constraint `where " + d.param +
                ": " + d.boundName + "` (§14.2)");
        }
    }
    deferredTypeBoundFailures_.clear();
}

// §11.8 (v0.176, #807): a class/record declaring `: I<...>` where I carries
// STATIC members (static methods / static operators) must itself supply
// matching statics — exact signatures per the §11.2 matching rule applied to
// statics (same name/arity/param types/param modifiers/return type including
// `owned`; operators match by token + signature). The interface member
// signatures are compared AFTER substituting I's type parameters with the
// conformance declaration's arguments (T → Meters for `: INumber<Meters>`).
// Static members create no itable slots and no runtime dispatch — this check
// is the whole conformance story for them; dispatch resolves statically at
// each generic instantiation (§11.8).
void Resolver::checkStaticInterfaceConformance() {
    if (!ctx_.module()) return;

    // Substitute the interface's own type-parameter names (a declaration-less
    // Named TypeRef with a single-part name) with the conformance arguments,
    // recursing through constructed generic args. Mirrors ExprTC's
    // substituteReturnType, resolver-side.
    std::function<TypeRef*(TypeRef*,
                           const std::unordered_map<std::string_view, TypeRef*>&)>
        subst = [&](TypeRef* d,
                    const std::unordered_map<std::string_view, TypeRef*>& map)
        -> TypeRef* {
        if (!d) return d;
        if (d->kind != TypeRef::Kind::Named) return d;
        if (!d->declaration && d->astNode && d->astNode->name &&
            d->astNode->name->parts.size() == 1 && d->genericArgs.empty()) {
            auto it = map.find(d->headName());
            if (it != map.end() && it->second) {
                TypeRef* s = it->second;
                if (d->nullable && !s->nullable) {
                    TypeRef* nn = ctx_.makeTypeRef(*s);
                    nn->nullable = true;
                    return nn;
                }
                return s;
            }
            return d;
        }
        if (!d->genericArgs.empty()) {
            bool changed = false;
            std::vector<TypeRef*> newArgs;
            newArgs.reserve(d->genericArgs.size());
            for (TypeRef* ga : d->genericArgs) {
                TypeRef* s = subst(ga, map);
                changed = changed || (s != ga);
                newArgs.push_back(s);
            }
            if (changed) {
                TypeRef* nn = ctx_.makeTypeRef(*d);
                nn->genericArgs = std::move(newArgs);
                return nn;
            }
        }
        return d;
    };

    // Structural equality that also distinguishes declaration-less builtins by
    // their spelling (typeRefEqual treats two null declarations as equal, which
    // would conflate e.g. Array<int> with Stack<int>).
    std::function<bool(const TypeRef*, const TypeRef*)> typesMatch =
        [&](const TypeRef* a, const TypeRef* b) -> bool {
        if (!a || !b) return false;
        if (a->kind == TypeRef::Kind::Named && b->kind == TypeRef::Kind::Named &&
            !a->declaration && !b->declaration) {
            auto spell = [](const TypeRef* t) -> std::string_view {
                if (t->astNode && t->astNode->name &&
                    !t->astNode->name->parts.empty())
                    return t->astNode->name->parts.back();
                return t->importedName;
            };
            if (spell(a) != spell(b)) return false;
            if (a->nullable != b->nullable) return false;
            if (a->genericArgs.size() != b->genericArgs.size()) return false;
            for (std::size_t i = 0; i < a->genericArgs.size(); ++i)
                if (!typesMatch(a->genericArgs[i], b->genericArgs[i]))
                    return false;
            return true;
        }
        return typeRefEqual(a, b);
    };

    // Resolve a member's declared param/return TypeRef through the resolved-
    // type map (both user interfaces and the synthesized §2.19 builtins have
    // their nodes resolved).
    auto resolvedOf = [&](const ast::Type* node) -> TypeRef* {
        return node ? ctx_.resolvedType(*node) : nullptr;
    };

    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        if (t->kind != ast::TypeDeclKind::Class &&
            t->kind != ast::TypeDeclKind::Record)
            continue;
        // §11.8 (#1425 sub-item 1): a class conforms via a DERIVED interface
        // (`class Meters: IRich<Meters>` where `IRich<T>: INumber<T>`) —
        // IRich declares no statics of its own, they live on the ANCESTOR
        // INumber. The pre-fix loop below scanned only `iface->members` for
        // each DIRECT base interface, so a class conforming via IRich was
        // never required to supply INumber's statics: a Meters with NONE of
        // Zero/One/+/-/*//< implemented compiled clean (a false-accept —
        // confirmed by probe before this fix). checkIfaceStatics is now a
        // recursive walk: run the existing per-interface check on `iface`,
        // then recurse into `iface`'s OWN base interfaces (`iface->baseTypes`
        // / `baseTypeRefs`, populated for an interface's `: Base<...>` list
        // exactly like a class's — TypeResolution_Names.cpp), composing the
        // substitution map at each hop (ancestor's own generic-param names →
        // `subst(ancestor-arg, the-CALLER's-map)`) so a multi-hop chain
        // resolves the recurrence parameter all the way down to `t`'s own
        // conformance argument. `visited` guards a diamond re-visit
        // (`IRich<T>: INumber<T>, IOrderable<T>` both reaching a common
        // ancestor) against a duplicate diagnostic; it can never be a true
        // cycle (interface-inheritance cycles are rejected elsewhere, the
        // §12.10 checkCycleFrom pass below runs over the same `baseTypes`).
        std::function<void(TypeSymbol*,
                           const std::unordered_map<std::string_view, TypeRef*>&,
                           std::unordered_set<TypeSymbol*>&)>
            checkIfaceStatics =
            [&](TypeSymbol* iface,
                const std::unordered_map<std::string_view, TypeRef*>& map,
                std::unordered_set<TypeSymbol*>& visited) {
            if (!iface || iface->kind != ast::TypeDeclKind::Interface) return;
            if (!visited.insert(iface).second) return;
            // Collect the interface's static members (static methods and
            // operators — every interface operator is static per §11.8).
            std::vector<MethodSymbol*> statics;
            for (Symbol* m : iface->members) {
                auto* ms = dynamic_cast<MethodSymbol*>(m);
                if (!ms) continue;
                if (ms->isOperator || hasMod(ms, "static"))
                    statics.push_back(ms);
            }
            for (MethodSymbol* im : statics) {
                // The interface member's declared signature pieces.
                //
                // §11.8 / §2.8.2 (#2294): the interface may be IMPORTED, in
                // which case `im->decl` is not a local ast::MethodDecl — an
                // imported member carries no syntactic AST at all. Its resolved
                // return type rides SemaContext's imported side table
                // (ModuleLoader::setImportedReturnType) and its parameters carry
                // `resolvedDeclType` instead of a syntactic `declType`. Reading
                // only the AST conflated "imported" with "unit-returning" and
                // left every imported parameter type null, so a CORRECT witness
                // for an imported static-interface member was rejected with the
                // "different signature" diagnostic below (a false rejection:
                // §11.8 conformance was unusable across a module boundary).
                const ast::Type* imRetNode = nullptr;
                bool imRetOwned = false;
                bool imHasAstDecl = false;
                if (auto* imd = dynamic_cast<const ast::MethodDecl*>(im->decl)) {
                    imRetNode = imd->returnType.get();
                    imRetOwned = imd->returnOwned;
                    imHasAstDecl = true;
                } else if (auto* iod =
                               dynamic_cast<const ast::OperatorDecl*>(im->decl)) {
                    imRetNode = iod->returnType.get();
                    imRetOwned = iod->returnOwned;
                    imHasAstDecl = true;
                }
                // The interface member's return, resolved from whichever source
                // it has. `imRetRef` is null exactly when the member returns
                // unit — for a local member that is a missing `: Type` node, for
                // an imported one a `Unit`-kind (or absent) recorded type.
                TypeRef* imRetRef = nullptr;
                if (imHasAstDecl) {
                    imRetRef = resolvedOf(imRetNode);
                } else {
                    imRetOwned = ctx_.importedReturnOwned(im);
                    TypeRef* r = ctx_.importedReturnType(im);
                    if (r && r->kind != TypeRef::Kind::Unit) imRetRef = r;
                }
                // "Declares a return" — the unit-vs-typed test below. For a
                // local member with a `: Type` node whose resolution failed,
                // keep the old behaviour (typed, and the match then fails on the
                // null TypeRef) rather than silently reclassifying it as unit.
                const bool imDeclaresReturn =
                    imHasAstDecl ? (imRetNode != nullptr) : (imRetRef != nullptr);
                const std::string memberSpelling =
                    im->isOperator
                        ? ("operator " + std::string(im->operatorToken))
                        : std::string(im->name);
                // Candidates on the conforming type itself: statics only.
                bool nameFound = false;
                bool matched = false;
                for (Symbol* cm : t->members) {
                    auto* cms = dynamic_cast<MethodSymbol*>(cm);
                    if (!cms) continue;
                    if (im->isOperator) {
                        if (!cms->isOperator ||
                            cms->operatorToken != im->operatorToken)
                            continue;
                    } else {
                        if (cms->isOperator || cms->name != im->name ||
                            !hasMod(cms, "static"))
                            continue;
                    }
                    nameFound = true;
                    if (cms->params.size() != im->params.size()) continue;
                    // Param-by-param: modifier + substituted type.
                    bool ok = true;
                    for (std::size_t pi = 0; pi < im->params.size(); ++pi) {
                        auto* ip = dynamic_cast<ParameterSymbol*>(im->params[pi]);
                        auto* cp = dynamic_cast<ParameterSymbol*>(cms->params[pi]);
                        if (!ip || !cp) { ok = false; break; }
                        // §11.8 / §2.8.2 (#2294): an IMPORTED interface
                        // parameter has no syntactic `declType`; the loader
                        // records its already-resolved type in
                        // `resolvedDeclType` instead (ModuleLoader::
                        // convertParams). `cp` is always local here (the
                        // conforming type is a member of this module), so only
                        // the interface side needs the fallback.
                        TypeRef* it2 = ip->declType ? resolvedOf(ip->declType)
                                                    : ip->resolvedDeclType;
                        TypeRef* ct = resolvedOf(cp->declType);
                        if (!it2 || !ct) { ok = false; break; }
                        // §11.8/§28.1 (v0.196, #2063): an `owned`/`out owned`
                        // slot on a bare type parameter of the interface is
                        // erased by the substitution at a value/runtime-managed
                        // conformer — the witness is spelled without the
                        // marker (which the concrete type would reject). Only
                        // applied when substitution actually rewrote the slot
                        // (`sub != it2`), so a still-abstract bare parameter
                        // retains the marker and a concretely-spelled `owned`
                        // interface parameter (already validated at the
                        // interface's own declaration) is unaffected.
                        TypeRef* subIt2 = subst(it2, map);
                        std::string_view imMod = ip->modifier;
                        if (subIt2 != it2 &&
                            (imMod == "owned" || imMod == "out owned") &&
                            ownedMarkerErasedAtSubstitutedType(subIt2)) {
                            imMod = (imMod == "out owned") ? "out" : "";
                        }
                        if (imMod != cp->modifier) { ok = false; break; }
                        if (!typesMatch(subIt2, ct)) { ok = false; break; }
                    }
                    if (!ok) continue;
                    // Return type (including `owned`, §11.2 exact-match rule).
                    const ast::Type* cRetNode = nullptr;
                    bool cRetOwned = false;
                    if (auto* cmd =
                            dynamic_cast<const ast::MethodDecl*>(cms->decl)) {
                        cRetNode = cmd->returnType.get();
                        cRetOwned = cmd->returnOwned;
                    } else if (auto* cod = dynamic_cast<const ast::OperatorDecl*>(
                                   cms->decl)) {
                        cRetNode = cod->returnType.get();
                        cRetOwned = cod->returnOwned;
                    }
                    // §11.8/§28.1 (v0.196, #2063): the same erasure on the
                    // return slot — `owned T` at a value/runtime-managed
                    // conformer substitutes to plain `T`, so the witness is
                    // spelled without the marker. Gated on substitution having
                    // rewritten the return (`sub != irt`); a concrete `owned
                    // C` interface return and an unresolved bare-T both retain.
                    bool effImRetOwned = imRetOwned;
                    TypeRef* subIrt = nullptr;
                    if (imRetRef) {
                        subIrt = subst(imRetRef, map);
                        if (subIrt != imRetRef && effImRetOwned &&
                            ownedMarkerErasedAtSubstitutedType(subIrt))
                            effImRetOwned = false;
                    }
                    if (effImRetOwned != cRetOwned) continue;
                    // unit vs typed (#2294: `imDeclaresReturn`, not the presence
                    // of an AST node — an imported member never has one).
                    if (imDeclaresReturn != (cRetNode != nullptr)) continue;
                    if (imDeclaresReturn) {
                        TypeRef* crt = resolvedOf(cRetNode);
                        if (!subIrt || !crt) continue;
                        if (!typesMatch(subIrt, crt)) continue;
                    }
                    matched = true;
                    break;
                }
                if (matched) continue;
                if (!nameFound) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        (t->kind == ast::TypeDeclKind::Record
                             ? std::string("record `")
                             : std::string("class `")) +
                        std::string(t->name) +
                        "` does not implement static interface member `" +
                        memberSpelling + "` of `" + std::string(iface->name) +
                        "` — a type declaring conformance to an interface with "
                        "static members must supply each of them as a `public "
                        "static` member (§11.8)");
                } else {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        (t->kind == ast::TypeDeclKind::Record
                             ? std::string("record `")
                             : std::string("class `")) +
                        std::string(t->name) + "` implements static interface "
                        "member `" + memberSpelling + "` of `" +
                        std::string(iface->name) +
                        "` with a different signature — the implementing static "
                        "must match the interface member exactly: same "
                        "parameter count, parameter types and modifiers, and "
                        "return type including `owned` (§11.2 / §11.8)");
                }
            }
            // §11.8 (#1425 sub-item 1): recurse into THIS interface's own
            // base interfaces, composing the substitution — `anc`'s own
            // generic-param names map to `subst(anc's-arg-as-written-on-
            // iface, map)`, i.e. resolved one hop further toward `t`'s
            // concrete conformance argument. E.g. `IRich<T>: INumber<T>`
            // conformed via `Meters: IRich<Meters>`: iface=IRich,
            // map={T(IRich's)->Meters}; anc=INumber, ancRef=INumber<T> (T is
            // IRich's own param), ancMap={T(INumber's)->subst(T(IRich's),
            // map)=Meters}.
            for (std::size_t bi2 = 0; bi2 < iface->baseTypeRefs.size(); ++bi2) {
                TypeSymbol* anc = bi2 < iface->baseTypes.size()
                                       ? iface->baseTypes[bi2] : nullptr;
                if (!anc || anc->kind != ast::TypeDeclKind::Interface) continue;
                TypeRef* ancRef = iface->baseTypeRefs[bi2];
                std::unordered_map<std::string_view, TypeRef*> ancMap;
                if (!anc->genericParams.empty()) {
                    if (!ancRef || ancRef->genericArgs.size() !=
                                       anc->genericParams.size())
                        continue;  // arity error reported elsewhere
                    for (std::size_t gi = 0; gi < anc->genericParams.size();
                         ++gi)
                        if (anc->genericParams[gi] && ancRef->genericArgs[gi])
                            ancMap[anc->genericParams[gi]->name] =
                                subst(ancRef->genericArgs[gi], map);
                }
                checkIfaceStatics(anc, ancMap, visited);
            }
        };

        for (std::size_t bi = 0; bi < t->baseTypes.size(); ++bi) {
            TypeSymbol* iface = t->baseTypes[bi];
            if (!iface || iface->kind != ast::TypeDeclKind::Interface) continue;
            // Bind the interface's type-parameter names to the conformance
            // declaration's arguments (`: INumber<Meters>` → T → Meters).
            std::unordered_map<std::string_view, TypeRef*> map;
            TypeRef* baseRef = bi < t->baseTypeRefs.size()
                                   ? t->baseTypeRefs[bi] : nullptr;
            if (!iface->genericParams.empty()) {
                if (!baseRef || baseRef->genericArgs.size() !=
                                    iface->genericParams.size())
                    continue;  // arity error reported elsewhere
                for (std::size_t gi = 0; gi < iface->genericParams.size(); ++gi)
                    if (iface->genericParams[gi] && baseRef->genericArgs[gi])
                        map[iface->genericParams[gi]->name] =
                            baseRef->genericArgs[gi];
            }
            std::unordered_set<TypeSymbol*> visited;
            checkIfaceStatics(iface, map, visited);
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
                // Classes and interfaces are reference types; records,
                // enums, and choices are value types (§9 / §10 / §14.2 /
                // §35, v0.200 #2044).
                switch (a->declaration->kind) {
                    case ast::TypeDeclKind::Class:
                    case ast::TypeDeclKind::Interface:
                        return TypeCategory::Reference;
                    case ast::TypeDeclKind::Record:
                    case ast::TypeDeclKind::Enum:
                    case ast::TypeDeclKind::Choice:
                        return TypeCategory::Value;
                }
            }
            // Built-in named types (Array, OwningArray, IEnumerable, …) carry
            // no user declaration. Every built-in named type in the language
            // is a class or interface — a reference type (§14.2). A generic
            // parameter also lands here, but the caller filters those out via
            // isGenericParamRef before calling categorize.
            if (a->astNode && a->astNode->name &&
                a->astNode->name->parts.size() == 1 &&
                builtinTypeArity(a->headName()) !=
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
        // §35.10 (v0.200, #2044): "Choices never satisfy the value-type arm:
        // `default(C)` is undefined for every choice type." So a choice
        // substitution fails `where T: new()`.
        if (d->kind == ast::TypeDeclKind::Choice) {
            decided = true;
            return false;
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
    auto report = [&](const std::string& reason,
                      const std::string& detail = {}) {
        // §3.6 rule 4 (sharpened v0.201, #2059): a derived-property
        // constraint failure may carry a component-path fragment naming
        // the offending field/element + the failing sub-rule. The fragment
        // is APPENDED after the §14.2 citation (not inside the `where T: …`
        // backtick enclosure — the fragment carries its own backticks).
        std::string msg =
            std::string("type argument `") + argName +
            "` does not satisfy the constraint `where " + param + ": " +
            reason + "` (§14.2)";
        if (!detail.empty()) msg += detail;
        ctx_.diagnostics().report(Severity::Error, t.span.begin, msg);
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
    if (item.kind == "sendable") {
        // §14.2 "Derived-property constraints" (v0.201, #2050): the
        // constraint names the §30.14 sendable derived-property. Satisfaction
        // is decided per instantiation, after substitution, by the property's
        // defining computation — the same evaluation §3.6 rule 2 already
        // performs; the constraint adds no second definition of the property.
        // The diagnostic at the instantiation site replaces the interior one
        // the caller would otherwise hit (§14.2). Defer on an Error TypeRef
        // (an upstream failure has already been reported).
        //
        // §3.6 rule 4 (sharpened v0.201, #2059): the diagnostic NAMES the
        // structural reason "as a component path" — the offending field /
        // element chain + the failing §30.14 rule. The flat "sendable"
        // spelling remains; the component-path fragment is APPENDED only
        // when the structural walk confidently pins the failure to a
        // single component (conservative — falls back to the flat message
        // rather than over-promising detail on ambiguous verdicts).
        if (arg->kind == TypeRef::Kind::Error) return;
        if (!isSendableType(arg, ctx_))
            report("sendable", sendableFailureComponentPath(arg, ctx_));
        return;
    }
    if (item.kind == "hashable") {
        // §14.2 "Derived-property constraints" (v0.214, #2087): the constraint
        // names the §3.6 **equality-bearing** derived-property (the keyword
        // names the capability site — hash-container keys/elements — while the
        // property keeps its §3.6 name). Satisfaction is decided per
        // instantiation, after substitution, by the property's defining
        // computation — isEqualityBearing, the SAME evaluation the §3.6 rule-2
        // hash-container-key check (checkHashContainerKeys) and the use-site
        // `=`/`<>` equality error perform; the constraint adds no second
        // definition of the property. The diagnostic at the instantiation site
        // replaces the interior one the caller would otherwise hit (§14.2).
        // Defer on an Error TypeRef (an upstream failure is already reported).
        //
        // §3.6 rule 4: the diagnostic NAMES the structural reason as a
        // component path — the offending record/choice + the failing §9.2/§35.7
        // reason (appended only when the type is that record/choice failure
        // shape; falls back to the flat message otherwise).
        if (arg->kind == TypeRef::Kind::Error) return;
        if (!isEqualityBearing(arg, ctx_))
            report("hashable", hashableFailureComponentPath(arg, ctx_));
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
        // #1867 / §14.2 clause 5 + §28.2/§9.7: a PRIMITIVE argument against
        // `IDisposable`/`IAsyncDisposable` — which only classes implement — can
        // never conform. These auto-imported builtin interfaces resolve to a
        // Named TypeRef WITHOUT a synthesized declaration, so without this they
        // would fall through the decl-null defer below. Mirrors the method-call-
        // site check in ExprTypecheck_Calls.cpp.
        if (arg->kind == TypeRef::Kind::Primitive &&
            bound->kind == TypeRef::Kind::Named && !bound->declaration &&
            primitiveNeverConformsToBuiltinIface(bound->headName())) {
            report(std::string(bound->headName()));
            return;
        }
        // We can only verify a type-bound when both sides are user-declared
        // named types (BFS over baseTypes). Built-in bounds/args carry no
        // base-type chain here; defer to avoid false positives.
        if (bound->kind != TypeRef::Kind::Named || !bound->declaration) {
            return;
        }
        // stdlib §2.19 / §11.8 (v0.176, #807): a PRIMITIVE argument against one
        // of the static-abstraction bounds (INumber/ISteppable/IParsable) is
        // decided by the compiler-provided conformance table — the numeric
        // tower conforms to all three, `char` to ISteppable/IParsable only,
        // everything else (string, bool, Object) to none.
        if (arg->kind == TypeRef::Kind::Primitive) {
            std::string_view bn = bound->declaration->name;
            if ((bn == "INumber" || bn == "ISteppable" || bn == "IParsable") &&
                bound->declaration == ctx_.builtinInterfaceSym(bn)) {
                if (arg->nullable ||
                    !primitiveConformsToStaticIface(arg->primitive, bn))
                    report(std::string(bn));
                return;
            }
            // per §14.2 clause 5 v0.196: a primitive conforms ONLY to the
            // interfaces its stdlib declaration lists plus the §11.8
            // compiler-provided abstractions (handled above) — all of which are
            // compiler-known builtin interfaces (IComparable/IEquatable/
            // IEqualityComparer/IReadOnlySpan carry a synthesized decl via
            // builtinInterfaceSym). A bound resolving to any OTHER declaration —
            // a USER-declared interface or class, or a builtin class — is one a
            // primitive can never conform to (primitives cannot declare
            // conformances); reject rather than defer. Mirrors the method-call-
            // site check in ExprTypecheck_Calls.cpp.
            if (bound->declaration != ctx_.builtinInterfaceSym(bn)) {
                report(std::string(bn));
                return;
            }
            // #1867: the bound IS a synthesized builtin interface. Blanket-
            // accepting by decl/name was too lenient — it admitted a WRONG
            // generic argument (`int` vs `IComparable<long>`) and interfaces no
            // primitive implements (`int` vs `IEqualityComparer<int>` /
            // `IReadOnlySpan<int>`). Verify the exact §14.2-clause-5 conformance
            // (correct head + generic arg); Defer keeps the historical
            // conservative accept when the arg is not a decided concrete
            // primitive. Mirrors the method-call-site check.
            if (primitiveConformsToBuiltinInstanceIface(
                    arg->primitive, bn, bound->genericArgs) ==
                PrimBuiltinIfaceConformance::Rejects)
                report(std::string(bn));
            return;
        }
        if (arg->kind != TypeRef::Kind::Named || !arg->declaration) {
            return;  // built-in / generic-param arg: defer
        }
        const std::string boundName(bound->declaration->name);
        if (arg->nullable) { report(boundName); return; }
        // §11.8 (v0.176, #807): a failed BFS here may be an ORDERING artifact —
        // the recurrence `class Meters: INumber<Meters>` checks this bound while
        // Meters' base list is mid-resolution (baseTypes not yet pushed). Defer
        // the report; recheckDeferredTypeBounds re-runs the BFS after every
        // type's bases are populated and reports persistent failures.
        if (!argSatisfiesTypeBound(arg, bound)) {
            deferredTypeBoundFailures_.push_back(DeferredTypeBoundFailure{
                t.span.begin, param, boundName, argName, arg, bound});
        }
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
                // §14.2 type-parameter bound (`where T: U`): the bound names
                // another generic parameter of `tsym`, so checkOneConstraint
                // defers on it (the bound's TypeRef has no declaration). Verify
                // it here where the sibling parameter's substitution is in hand:
                // T's substitution must be non-nullable and assignable to U's.
                if (item.kind == "type" && item.typeArg && item.typeArg->name &&
                    item.typeArg->name->parts.size() == 1) {
                    std::string_view boundParam =
                        item.typeArg->name->parts.back();
                    std::size_t ui = tsym->genericParams.size();
                    for (std::size_t k = 0; k < tsym->genericParams.size(); ++k)
                        if (tsym->genericParams[k] &&
                            tsym->genericParams[k]->name == boundParam) {
                            ui = k;
                            break;
                        }
                    if (ui < argRefs.size() && argRefs[ui]) {
                        TypeRef* uSubst = argRefs[ui];
                        if (arg->nullable) {
                            // §14.2: `where T: U` implies non-null T.
                            ctx_.diagnostics().report(
                                Severity::Error, t.span.begin,
                                std::string("type argument `") +
                                argSpelling(argAst) +
                                "` does not satisfy the constraint `where " +
                                std::string(gp->name) + ": " +
                                std::string(boundParam) + "` (§14.2)");
                        } else if (uSubst->kind == TypeRef::Kind::Named &&
                                   uSubst->declaration &&
                                   arg->kind == TypeRef::Kind::Named &&
                                   arg->declaration) {
                            // Both substitutions are user types: route through
                            // the ordering-safe recheck (a base set may still be
                            // mid-population at this instantiation site).
                            if (!argSatisfiesTypeBound(arg, uSubst))
                                deferredTypeBoundFailures_.push_back(
                                    DeferredTypeBoundFailure{
                                        t.span.begin, std::string(gp->name),
                                        std::string(uSubst->declaration->name),
                                        argSpelling(argAst), arg, uSubst});
                        }
                        // else: a substitution that is not a user declaration
                        // (primitive/builtin/param) → defer (no diagnostic),
                        // matching checkOneConstraint's conservatism.
                        continue;
                    }
                }
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
            if (prim != PrimitiveType::None) {
                // §32.5 (v0.186): "Nullable value types are rejected" —
                // `int?`, `bool?`, etc. have no C representation. Only the
                // pointer-carrying boundary types below admit a `?`.
                if (t.nullable) return false;
                return isFfiPrimitive(prim);
            }
            // §32.5 / stdlib §10: IntPtr, UIntPtr, CString, WString, RawBytes.
            if (isFfiBoundaryNamedType(n)) {
                // §32.5 (v0.186): a `RawBytes` extern RETURN is rejected — a C
                // function returns one value and the two-slot (pointer, length)
                // marshalling shape has no return convention. RawBytes stays
                // admitted in parameter positions (of extern methods and of FFI
                // function types), where it occupies two consecutive C slots.
                // The rejection precedes the nullable branch so `RawBytes?`
                // returns are rejected too (#1720).
                if (n == "RawBytes" && returnPosition) return false;
                // §32.5 (v0.186) "Nullable boundary forms": only `CString?`,
                // `WString?`, and `RawBytes?` are admitted nullable — the
                // nullability rides the same pointer representation (Bezel
                // `null` ⇄ C `NULL`). `IntPtr?`/`UIntPtr?` are nullable value
                // types with no C representation and are rejected, like `int?`.
                if (t.nullable)
                    return n == "CString" || n == "WString" || n == "RawBytes";
                return true;
            }
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
            // §32.5 (v0.196, #1827): a nullable inline array `[N]T?` has no
            // C representation. Per §32.5 "Nullable value types are
            // rejected" — only the pointer-carrying boundary forms
            // (`CString?`/`WString?`/`RawBytes?`) admit `?`. Inline arrays
            // fall in the same class as `int?`/`bool?` and are rejected.
            // The InlineArray mirror of the Named case's `if (t.nullable)
            // return false;` was missing — the diagnostic at the call site
            // spells the rejected `[N]T?` and cites §32.5.
            if (t.nullable) return false;
            // The element must be an FFI-compatible *primitive* — §32.5 does
            // not admit inline arrays of class/record/inline-array elements.
            const ast::Type& elem = *t.arrayElement;
            // §32.5 (v0.196, #1827): an inline array whose element is a
            // nullable value type `[N](int?)` is the InlineArray mirror of
            // the Named `int?` reject above — same root cause, same hole.
            // Without this check, primitiveFromName ignores nullability and
            // the element passes through as if it were the bare primitive.
            if (elem.nullable) return false;
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
    // §32.5 (v0.186, #1720): a `RawBytes` (or `RawBytes?`) extern RETURN is
    // specifically rejected — the two-slot (pointer, length) marshalling shape
    // has no C return convention. Give an actionable diagnostic naming the
    // honest C contract rather than the generic "not permitted" message, since
    // RawBytes *is* permitted in parameter positions.
    if (returnPosition && t.kind == ast::TypeKind::Named && t.name &&
        t.name->parts.size() == 1 && t.name->parts.front() == "RawBytes") {
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            "an extern method's `RawBytes` return type is rejected — a C "
            "function returns one value and RawBytes' two-slot (pointer, "
            "length) shape has no return convention; declare the pointer return "
            "plus a length out-parameter and wrap with RawBytes.FromForeign "
            "(§32.5)");
        return false;
    }
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

// ---------------------------------------------------------------------------
// §12.9 / §11.8 (v0.181, issue #1559) — variance markers are interface-only.
//
// The shared `GenericParam` grammar (Appendix A.4) admits `in`/`out` on
// every generic-parameter list — classes, records, enums, methods, type
// aliases, extension methods — but only an interface's OWN type parameters
// may legally carry one: variance is defined by instance-reference
// assignability, and classes/records/concrete generic types are always
// invariant (§12.9). A method's type parameters are never variant, even on
// an interface's own methods — the position rules govern the *type's*
// parameters, not a method's.
//
// Separately (§11.8), an interface that declares a static member (a
// `static` method, or an operator — implicitly static, §25) may not mark
// its own type parameters variant either: variance is defined over
// instance references and static signatures have no recorded reading.
// ---------------------------------------------------------------------------

// True iff `t` declares at least one static member: an explicit `static`
// method, or an operator overload (operators are implicitly static — A.4
// `OperatorDecl ::= Visibility "static" "method" "operator" ...`). A member
// function (not a free function) because it uses the private `hasMod` helper.
bool Resolver::declaresStaticMember(TypeSymbol& t) {
    for (Symbol* m : t.members) {
        auto* ms = dynamic_cast<MethodSymbol*>(m);
        if (!ms) continue;
        if (ms->isOperator || hasMod(ms, "static")) return true;
    }
    return false;
}

// Reject a variance marker on `member`'s own generic parameters, if any —
// a method's type parameters are never variant (§12.9), regardless of the
// enclosing declaration's kind. Diagnostics anchor on the method's
// declaration span (GenericParam itself carries no span, Appendix A.3).
void Resolver::checkMethodGenericParamsNoVariance(Symbol* member) {
    auto* ms = dynamic_cast<MethodSymbol*>(member);
    if (!ms) return;
    const SourceLocation at =
        ms->decl ? ms->decl->span.begin : SourceLocation{};
    for (Symbol* gp : ms->genericParams) {
        auto* gps = dynamic_cast<GenericParamSymbol*>(gp);
        if (!gps || gps->variance.empty()) continue;
        ctx_.diagnostics().report(
            Severity::Error, at,
            std::string("type parameter `") + std::string(gps->name) +
            "` may not be marked `" + std::string(gps->variance) +
            "` — variance markers are valid only on an interface's own "
            "type parameters (§12.9), not on a method type parameter");
    }
}

void Resolver::checkVarianceMarkerScopeForType(TypeSymbol& t) {
    const SourceLocation at = t.decl ? t.decl->span.begin : SourceLocation{};
    if (t.kind != ast::TypeDeclKind::Interface) {
        std::string_view declKind =
            t.kind == ast::TypeDeclKind::Record ? "a record type parameter" :
            t.kind == ast::TypeDeclKind::Enum   ? "an enum type parameter" :
                                                   "a class type parameter";
        for (Symbol* gp : t.genericParams) {
            auto* gps = dynamic_cast<GenericParamSymbol*>(gp);
            if (!gps || gps->variance.empty()) continue;
            ctx_.diagnostics().report(
                Severity::Error, at,
                std::string("type parameter `") + std::string(gps->name) +
                "` may not be marked `" + std::string(gps->variance) +
                "` — variance markers are valid only on an interface's own "
                "type parameters (§12.9), not on " + std::string(declKind));
        }
    } else if (declaresStaticMember(t)) {
        for (Symbol* gp : t.genericParams) {
            auto* gps = dynamic_cast<GenericParamSymbol*>(gp);
            if (!gps || gps->variance.empty()) continue;
            ctx_.diagnostics().report(
                Severity::Error, at,
                std::string("interface `") + std::string(t.name) +
                "` declares a static member, so its type parameter `" +
                std::string(gps->name) + "` may not be marked `" +
                std::string(gps->variance) + "` (§11.8)");
        }
    }
    for (Symbol* m : t.members) {
        checkMethodGenericParamsNoVariance(m);
        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
            if (nt->type) checkVarianceMarkerScopeForType(*nt->type);
        }
    }
}

void Resolver::checkVarianceMarkerScope() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            checkVarianceMarkerScopeForType(*t);
            continue;
        }
        if (auto* alias = dynamic_cast<TypeAliasSymbol*>(child)) {
            const SourceLocation at =
                alias->decl ? alias->decl->span.begin : SourceLocation{};
            for (Symbol* gp : alias->genericParams) {
                auto* gps = dynamic_cast<GenericParamSymbol*>(gp);
                if (!gps || gps->variance.empty()) continue;
                ctx_.diagnostics().report(
                    Severity::Error, at,
                    std::string("type parameter `") + std::string(gps->name) +
                    "` may not be marked `" + std::string(gps->variance) +
                    "` — variance markers are valid only on an interface's "
                    "own type parameters (§12.9), not on a type alias "
                    "parameter");
            }
            continue;
        }
        if (auto* ext = dynamic_cast<ExtensionSymbol*>(child)) {
            for (Symbol* m : ext->members) {
                checkMethodGenericParamsNoVariance(m);
            }
            continue;
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
//   - A non-owning inline array [N]T whose element type is eligible (§9.8
//     v0.228, issue #1065): encoded like any other standard sequence
//     collection (BeginArray(N)/per-element/EndArray)
// NOT eligible:
//   - A class without a `serialized tree`/`graph`/`cyclic` clause
//   - A record that does not itself declare ISerializable/IDeserializable
//   - Any other Named type (interface, etc.)
//   - An OWNING inline array ([N owned]T) — no spec text admits it (§9.8
//     v0.228, issue #1065)
//   - Function, unit types
//
// A field marked `serialized skip` is excluded from the component set and does
// not disqualify the type (§9.8).

// Check whether a TypeRef is a serialization-eligible COMPONENT type (§9.8).
// `visited` prevents infinite recursion on mutually-referencing eligible records.
// `forRecordComponent` is true when this component is being checked as a field
// of a RECORD (positional field or body-record auto-property) rather than a
// class. §9.8 v0.182: "A record cannot hold a class-typed component in its
// serialized set at all ... a record's reference-typed fields are always
// borrows, and a borrow has no position in an owning spanning tree ... The
// same holds for `Shared<T>`, `Weak<T>`, and interface-typed components: a
// record declares no owning spanning tree against which an identity
// participant or a back-edge could resolve." Class-typed and `Shared<T>`/
// `Weak<T>`-typed components are therefore eligible only when the containing
// type is a class, never a record (interface-typed components are already
// unconditionally ineligible below, satisfying the same rule for records).
// Codegen restriction helper for the InlineArray branch of
// isSerializationEligibleType below (issue #1065 follow-up, filed): true when
// `tr` is a `string`, OR a Named record/choice/class (which may itself carry
// a `string` component, transitively) — i.e. a shape that MAY carry a
// runtime-managed reference the record-scope-end release walk
// (LlvmBackend_impl.hpp recordHasStringFields / emitRecordStringFieldReleases,
// fix #1747) would need to reach through an inline array to release, but
// currently does not (that walk inspects only a record's own DIRECT storage
// members' kind). Deliberately conservative — it excludes every Named
// element type rather than attempting the transitive "does this nested
// record contain a string" walk, which is future work once the release walk
// itself is extended to recurse into inline arrays (tracked in the filed
// follow-up). Primitives-other-than-string, enums, and nested non-owning
// inline arrays of the same carry no such obligation and are unaffected.
static bool typeRiskingUnreleasedStringInInlineArray(const TypeRef* tr) {
    if (!tr) return false;
    using K = TypeRef::Kind;
    if (tr->kind == K::Primitive)
        return tr->primitive == PrimitiveType::String;
    if (tr->kind == K::InlineArray)
        return typeRiskingUnreleasedStringInInlineArray(tr->arrayElement);
    if (tr->kind == K::Named) {
        // A resolved Named type is a record, enum, choice, or class; only an
        // enum is guaranteed string-free (its representation is an integer,
        // §10.3). Everything else (record/choice/class, and an unresolved
        // Named with no declaration, e.g. Shared<T>/Weak<T>) is excluded
        // conservatively.
        return !(tr->declaration &&
                 tr->declaration->kind == ast::TypeDeclKind::Enum);
    }
    return false;
}

static bool isSerializationEligibleType(
        const TypeRef* tr,
        std::unordered_set<const TypeSymbol*>& visited,
        const SemaContext& ctx,
        bool forRecordComponent = false) {
    if (!tr) return false;
    using K = TypeRef::Kind;
    // Nullable T?: eligible when T is eligible (§9.8).
    if (tr->nullable) {
        // Peel the nullable wrapper and check the inner type.
        TypeRef inner = *tr;
        inner.nullable = false;
        return isSerializationEligibleType(&inner, visited, ctx, forRecordComponent);
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
    // §9.8 (v0.228, issue #1065): a non-owning inline array `[N]T` is a
    // serialization-eligible standard sequence collection when its element
    // type is eligible — "or a standard sequence collection (Array<T>,
    // OwningArray<T>, LinkedList<T>, etc., and inline arrays [N]T) whose
    // element type is serialization-eligible (encoded with
    // BeginArray/EndArray)". Record-admissibility (the record clause, same
    // section) further requires the array be NON-OWNING: "a non-owning
    // inline array [N]T/[N]T? of record-admissible element type" — an
    // owning inline array (`[N owned]T`) is a move-only value type with a
    // per-element disposal obligation that BeginArray/EndArray's plain
    // element-encoding does not model, and no spec text admits it as a
    // component in either the record or the class list, so it is rejected
    // here for both (simplest reading of an otherwise-silent case; recorded
    // per CLAUDE.md rule 4 — see issue #1065 for the record of this choice).
    if (tr->kind == K::InlineArray) {
        if (tr->arrayOwned) return false;
        if (!tr->arrayElement) return false;
        // Codegen restriction (issue #1065 follow-up, filed): the synthesized
        // Deserialize's element-materialization loop (LlvmBackend_Serialization.cpp)
        // constructs the inline array via a raw GEP-store loop that bypasses the
        // `recordHasStringFields`/`emitRecordStringFieldReleases` retain/release
        // bookkeeping (LlvmBackend_impl.hpp, fix #1747) — that bookkeeping only
        // inspects a record's DIRECT storage members' kind (Primitive/String), so
        // it does not see a `string` (or a string-bearing record/choice) nested
        // inside an inline-array component. Admitting such an element type here
        // would synthesize a Deserialize that leaks every array-element string on
        // the caller's scope exit (verified: a `[N]string` field leaks 2
        // allocations per round-trip under ASan/LSan). Excluded here, conservatively,
        // until that retain/release walk is extended to recurse into inline
        // arrays; primitives-other-than-string, enums, and nested non-owning
        // inline arrays thereof carry no such obligation and are unaffected.
        if (typeRiskingUnreleasedStringInInlineArray(tr->arrayElement))
            return false;
        return isSerializationEligibleType(tr->arrayElement, visited, ctx,
                                            forRecordComponent);
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
            bname = tr->headName();
        } else if (!tr->importedName.empty()) {
            bname = tr->importedName;
        }
        if ((bname == "Shared" || bname == "Weak") &&
            !tr->genericArgs.empty() && tr->genericArgs[0]) {
            // §9.8 v0.182: a record declares no owning spanning tree, so it
            // cannot hold a `Shared<T>` identity participant or a `Weak<T>`
            // back-edge — only a class can.
            if (forRecordComponent) return false;
            return isSerializationEligibleType(tr->genericArgs[0], visited, ctx,
                                                forRecordComponent);
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

    // Choice: §9.8 choice serialization (v0.200, #2046). A value-like choice is
    // value-shaped storage throughout (its payloads are value/string/runtime-
    // managed — no owning tree, no class-typed owner), so it is admissible as a
    // choice payload and as a record component alike; the same "trust the nested
    // declaration, checked at its own conformance site" posture as records. An
    // owning or borrow-bearing choice is never eligible (the Deserialize-has-no-
    // owner argument — §9.8/§35.4), matching the classifyChoiceType verdict.
    // (Whether a value-like choice is formally in §9.8's record-admissible list
    // is a documentation seam noted as a filed issue; the value-shaped reading is
    // taken here, consistent with §9.2's admission of value-like choices as
    // record equality components.)
    if (ts->kind == ast::TypeDeclKind::Choice) {
        if (visited.count(ts)) return true;  // cycle → assume eligible
        auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
        if (!td) return false;
        bool declSer = false, declDeser = false;
        for (const auto& bt : td->baseTypes) {
            if (!bt || !bt->name || bt->name->parts.empty()) continue;
            auto n = bt->name->parts.back();
            if (n == "ISerializable")   declSer   = true;
            if (n == "IDeserializable") declDeser = true;
        }
        if (!declSer && !declDeser) return false;
        // Only value-like choices are eligible (§9.8 choice subsection).
        TypeRef rt;
        rt.kind = TypeRef::Kind::Named;
        rt.declaration = const_cast<TypeSymbol*>(ts);
        auto verdict = common::classifyChoiceType(&rt, ctx);
        if (verdict && *verdict != common::ChoiceClassification::ValueLike)
            return false;
        visited.insert(ts);
        return true;
    }

    // Class: never eligible as a RECORD component (§9.8 v0.182 — a record
    // declares no owning spanning tree for a class-typed owning edge or
    // back-edge to resolve against; the synthesized `Deserialize` could not
    // construct the record without binding a freshly-produced component to a
    // borrowing field, which §9.7's no-temporary-source rule for
    // borrow-bearing records rejects). As a CLASS component it is eligible
    // when it has a serialized-class clause ("serialization-eligible
    // synthesized class" per §9.8).
    if (ts->kind == ast::TypeDeclKind::Class) {
        if (forRecordComponent) return false;
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
                auto n = inner->headName();
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
        bool isChoice = t->kind == ast::TypeDeclKind::Choice;
        if (!isRecord && !isEnum && !isClass && !isChoice) continue;

        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) continue;

        bool wantsIS  = declaredConformance(*td, "ISerializable");
        bool wantsID  = declaredConformance(*td, "IDeserializable");
        if (!wantsIS && !wantsID) continue;

        // Enums are always serialization-eligible (§9.8).
        if (isEnum) continue;  // no further check needed

        // §9.8 choice serialization (v0.200, #2046): a choice is serialization-
        // eligible when it is value-like (§35.4) and every payload field's type
        // is serialization-eligible and record-admissible.  An ineligible
        // conformance declaration is a compile error at the conformance
        // declaration, naming the first offending payload — the same discipline
        // as the record rule below.
        if (isChoice) {
            bool hwS = typeHasHandWrittenMethod(t, "Serialize");
            bool hwD = typeHasHandWrittenMethod(t, "Deserialize");
            bool needSer   = wantsIS && !hwS;
            bool needDeser = wantsID && !hwD;
            if (!needSer && !needDeser) continue;  // fully hand-written

            bool foundError = false;

            // Case-level and payload-level `serialized` clause validation (§9.8):
            //  - the `serialized` clause on a CASE admits the rename form only
            //    (a case is not omittable — the enum-constant rule): `skip` and
            //    `optional` on a case are compile errors;
            //  - a case rename, or a payload rename, beginning with `$` is
            //    reserved for the JSON $-set (stdlib §2.15, including `$case`).
            for (const auto& cc : td->choiceCases) {
                if (cc.serialized) {
                    if (cc.serialized->isSkip || cc.serialized->isOptional) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "choice `" + std::string(t->name) + "`: the "
                            "`serialized` clause on case `" + std::string(cc.name) +
                            "` admits the rename form only — a case is not "
                            "omittable (§9.8; the enum-constant rule). Remove "
                            "`skip`/`optional`.");
                        foundError = true;
                        break;
                    }
                    if (!cc.serialized->name.empty() &&
                        cc.serialized->name.front() == '$') {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "choice `" + std::string(t->name) + "`: `serialized` "
                            "rename `" + std::string(cc.serialized->name) +
                            "` for case `" + std::string(cc.name) +
                            "` begins with `$`, which is reserved for the JSON "
                            "$-set (`$case`/`$type`/`$id`/`$ref`, stdlib §2.15); "
                            "choose a name that does not start with `$`");
                        foundError = true;
                        break;
                    }
                }
                for (const auto& pf : cc.payload) {
                    if (pf.serialized && !pf.serialized->name.empty() &&
                        pf.serialized->name.front() == '$') {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "choice `" + std::string(t->name) + "`: `serialized` "
                            "rename `" + std::string(pf.serialized->name) +
                            "` for payload `" + std::string(cc.name) + "." +
                            std::string(pf.name) + "` begins with `$`, which is "
                            "reserved for the JSON $-set (stdlib §2.15); choose a "
                            "name that does not start with `$`");
                        foundError = true;
                        break;
                    }
                    // §9.8/§35.10: `serialized skip` supplies default(T); a
                    // non-nullable choice-typed payload has no default (§17.11),
                    // so `skip` on it requires the nullable form.
                    if (positionalFieldIsSkip(pf) && pf.type) {
                        const TypeRef* ptr = ctx_.resolvedType(*pf.type);
                        if (ptr && !ptr->nullable && ptr->kind == TypeRef::Kind::Named &&
                            ptr->declaration &&
                            ptr->declaration->kind == ast::TypeDeclKind::Choice) {
                            ctx_.diagnostics().report(
                                Severity::Error, t->declLoc,
                                "choice `" + std::string(t->name) + "`: "
                                "`serialized skip` on payload `" +
                                std::string(cc.name) + "." + std::string(pf.name) +
                                "` reconstructs with `default(T)`, but a choice "
                                "type has no default (§17.11/§35.10) — make the "
                                "payload nullable (`C?`) or remove `skip`");
                            foundError = true;
                            break;
                        }
                    }
                }
                if (foundError) break;
            }
            if (foundError) continue;

            // §9.8/§35.4: only value-like choices are eligible — an owning or
            // borrow-bearing choice is never eligible (the Deserialize-has-no-
            // owner argument, same as class-typed record fields).
            TypeRef rt;
            rt.kind = TypeRef::Kind::Named;
            rt.declaration = t;
            auto verdict = common::classifyChoiceType(&rt, ctx_);
            if (verdict &&
                *verdict != common::ChoiceClassification::ValueLike) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    "choice `" + std::string(t->name) + "` declares "
                    "`ISerializable`/`IDeserializable` but is not serialization-"
                    "eligible (§9.8): the choice is " +
                    (*verdict == common::ChoiceClassification::Owning
                         ? std::string("owning")
                         : std::string("borrow-bearing")) +
                    " (§35.4), and only a value-like choice is eligible — a "
                    "class-, interface-, or `owned`-payload choice has no owner "
                    "for the synthesized `Deserialize` to construct against. "
                    "Remove the conformance declaration, restructure to a "
                    "value-like choice, or hand-write `Serialize`/`Deserialize`.");
                continue;
            }

            // Every non-skip payload field must itself be serialization-eligible
            // and record-admissible (value-shaped storage throughout, §9.8).
            std::unordered_set<const TypeSymbol*> visited;
            visited.insert(t);
            for (const auto& cc : td->choiceCases) {
                bool broke = false;
                for (const auto& pf : cc.payload) {
                    if (positionalFieldIsSkip(pf)) continue;  // excluded
                    if (!pf.type) continue;
                    const TypeRef* tr = ctx_.resolvedType(*pf.type);
                    if (!tr) continue;
                    if (!isSerializationEligibleType(tr, visited, ctx_,
                                                     /*forRecordComponent=*/true)) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "choice `" + std::string(t->name) + "` declares "
                            "`ISerializable`/`IDeserializable` but is not "
                            "serialization-eligible (§9.8): payload `" +
                            std::string(cc.name) + "." + std::string(pf.name) +
                            "` has a type that is not serialization-eligible and "
                            "record-admissible; remove the conformance "
                            "declaration, mark the payload `serialized skip` (if "
                            "defaultable), make the payload type serializable, or "
                            "hand-write `Serialize`/`Deserialize`");
                        broke = true;
                        break;
                    }
                }
                if (broke) break;
            }
            continue;
        }

        // Classes: §9.8 tier-2 eligibility checking — MOVED to the late pass
        // checkClassSerializationEligibilityLate() (see the declaration
        // comment in TypeResolution_impl.hpp / #2325): the check needs
        // FieldSymbol::posParamBodyRef, which ExprTypecheck computes AFTER
        // resolveTypes() (this function) has already run. The PRE-PASS above
        // (synthesizing td->serializedClass's tree/graph shape from field
        // TYPES alone, which does not need posParam capture classification)
        // still runs here, unchanged, because ExprTypecheck's own
        // `.Serialize()`/`.Deserialize()` call resolution depends on that
        // shape already being set.
        if (isClass) continue;

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
                // §9.8 v0.182: checked as a RECORD component — class-,
                // interface-, `Shared<T>`-, and `Weak<T>`-typed fields are
                // never eligible here, only within a class.
                if (!isSerializationEligibleType(tr, visited, ctx_,
                                                  /*forRecordComponent=*/true)) {
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
                // §9.8 v0.182: checked as a RECORD component (see the
                // positional-field case above).
                if (!isSerializationEligibleType(tr, visited, ctx_,
                                                  /*forRecordComponent=*/true)) {
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

        // §9.8 v0.182 (#1572): record constructor-selection check — the
        // record-side counterpart to the class check above (same rule,
        // written out at the top of this function's `else if (isClass)`
        // branch). Applies only when Deserialize synthesis is actually
        // needed and the record body declares one or more EXPLICIT
        // constructors: "If the record declares one or more explicit
        // constructors, the synthesized Deserialize calls the unique
        // explicit constructor whose parameter list binds exactly the
        // serialized components, matched by name — compared
        // case-insensitively... If explicit constructors exist but none
        // matches, more than one matches, ... the conformance declaration
        // is a compile error naming the candidates." A record with NO
        // explicit constructor (the common case — a purely positional
        // record, or a body record with no author-declared constructor) is
        // "constructed directly from the read components at the storage
        // level" — no ambiguity is possible, so the check below is a no-op
        // whenever `allCtors` comes back empty.
        if (!foundError && needsDeserializeCheck) {
            auto toLower = [](std::string_view sv) {
                std::string r(sv);
                for (char& c : r)
                    c = (char)std::tolower((unsigned char)c);
                return r;
            };

            // Collect the lowercased declared identifiers of ALL serialized
            // components — including `serialized skip` ones, which still
            // bind a constructor parameter (their value is default(T)).
            std::unordered_set<std::string> compNamesLower;
            std::vector<std::string> compNamesOrig;
            if (td->hasPositionalFields) {
                for (const auto& pf : td->positionalFields) {
                    compNamesLower.insert(toLower(pf.name));
                    compNamesOrig.push_back(std::string(pf.name));
                }
            } else {
                for (const Symbol* m : t->members) {
                    if (!isAutoPropertySymbol(m)) continue;
                    compNamesLower.insert(toLower(m->name));
                    compNamesOrig.push_back(std::string(m->name));
                }
            }

            // Collect non-static explicit constructors and check which match.
            std::vector<std::string> matchingCtors;
            std::vector<std::string> allCtors;
            for (const Symbol* m : t->members) {
                auto* cs = dynamic_cast<const sema::ConstructorSymbol*>(m);
                if (!cs || cs->isStatic) continue;
                auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                if (!cd) continue;

                std::string ctorDesc = "constructor(";
                for (unsigned pi = 0; pi < cd->params.size(); ++pi) {
                    if (pi) ctorDesc += ", ";
                    ctorDesc += std::string(cd->params[pi].name);
                }
                ctorDesc += ")";
                allCtors.push_back(ctorDesc);

                // Match: param count == component count AND every param name
                // (lowered) appears among the component names (bijection).
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

            // No explicit constructor at all: storage-level construction
            // applies (§9.8 v0.182) — nothing to disambiguate, skip.
            if (!allCtors.empty() && matchingCtors.size() != 1) {
                std::string candidates;
                for (const auto& c : allCtors) {
                    if (!candidates.empty()) candidates += ", ";
                    candidates += c;
                }
                std::string compSet;
                for (const auto& n : compNamesOrig) {
                    if (!compSet.empty()) compSet += ", ";
                    compSet += n;
                }
                if (matchingCtors.empty()) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "record `" + std::string(t->name) + "` declares "
                        "`IDeserializable`: no explicit constructor has "
                        "parameter names matching the serialized component "
                        "set {" + compSet + "} — candidates: " + candidates +
                        " (§9.8 v0.182: constructor selection is by name, "
                        "case-insensitive, order-insensitive)");
                } else {
                    std::string matchList;
                    for (const auto& c : matchingCtors) {
                        if (!matchList.empty()) matchList += ", ";
                        matchList += c;
                    }
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "record `" + std::string(t->name) + "` declares "
                        "`IDeserializable`: more than one explicit "
                        "constructor matches the serialized component set — "
                        "ambiguous: " + matchList +
                        " (§9.8 v0.182: the unique matching constructor is "
                        "required)");
                }
                foundError = true;
            }
        }

        (void)foundError;  // error was already reported if true
    }
}

// ---------------------------------------------------------------------------
// Resolver::checkClassSerializationEligibilityLate  §9.8 / #2325
//
// The CLASS half of checkSerializationEligibility's component-eligibility and
// constructor-selection checks (see that function's declaration comment,
// TypeResolution_impl.hpp, for why this is a separate LATE pass called by the
// driver after typecheckExpressions() rather than as part of resolveTypes()).
// Logic is otherwise unchanged from the original (pre-#2325) class branch,
// plus the #2325 posParam-capture handling: a primary-constructor parameter
// captured into a private field (§11.1 — referenced in a body position, e.g.
// `property Id: int => id;`) is a genuine serialized component (its type via
// `posParam->type`, ownership via `posParam->modifier`), and the class's own
// implicit PRIMARY constructor (`td->positionalFields`) is itself a
// constructor-selection match candidate alongside any explicit
// `constructor(...)` members (there is no ConstructorSymbol for it —
// SymbolCollection.cpp creates one only for an explicit ast::ConstructorDecl).
// ---------------------------------------------------------------------------
void Resolver::checkClassSerializationEligibilityLate() {
    if (!ctx_.module()) return;

    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Class) continue;

        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) continue;

        bool wantsIS = declaredConformance(*td, "ISerializable");
        bool wantsID = declaredConformance(*td, "IDeserializable");
        if (!wantsIS && !wantsID) continue;

            if (!td->serializedClass) {
                // §9.8 v0.141: after the synthesis pre-pass in
                // checkSerializationEligibility, a class with no clause here
                // is fully hand-written (Serialize + Deserialize) — it
                // satisfies the conformance without synthesis, so there is
                // nothing to check.
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

            // §9.8 does not specify the inheritance × synthesis interaction
            // (a documented spec silence — serialization_tier2_classes_test.cpp
            // SILENCE-1). The synthesized Deserialize (emitClassDeserialize,
            // LlvmBackend_Serialization.cpp) does not construct a base-class
            // subobject at all on EITHER the primary-ctor or explicit-ctor
            // construction path (no emitImplicitBase / base-ctor-chain call
            // exists there) — so a derived class's INHERITED fields are
            // silently left at their calloc-zeroed defaults on Deserialize.
            // Verified reachable (not merely theoretical): `class Derived():
            // Base(999), ISerializable, IDeserializable ... end;` compiles
            // and RUNS, round-tripping Base's field as 0 instead of 999, with
            // no diagnostic — instance-corrupting, not merely incomplete.
            // Per the spec's governing principle for a shape an
            // implementation cannot carry correctly (§2.8.2: "An
            // implementation unable to carry some body shape ... must reject
            // the export, or the import, with a diagnostic naming the shape;
            // it must not alter the instantiation's semantics" — §2.8.2
            // itself governs cross-module artifact interfaces, not
            // serialization, but the same reasoning applies: silently
            // altering semantics is strictly worse than a clean rejection),
            // reject synthesis of Deserialize specifically for a class whose
            // base chain actually carries storage, rather than emit a
            // deserializer that silently corrupts the base portion of the
            // instance. Serialize alone is UNAFFECTED and not rejected:
            // writing an already-fully-constructed instance's inherited
            // fields is sound today (collectInstanceStorage /
            // emitClassSerialize already walk the base chain correctly —
            // verified) — only CONSTRUCTION (Deserialize) has no
            // base-subobject path. This turns the live corruption bug into a
            // clean compile-time rejection until inheritance is supported
            // (tracked: issue #2364).
            //
            // NOT a blanket "any base class" rejection: the §9.8 tier-4
            // `polymorphic` pattern (serialization_tier2_classes_test.cpp)
            // has an abstract base (`Shape`) that carries NO storage of its
            // own (only the `polymorphic` registry — a member clause, not a
            // field — and an abstract method) — a base with zero own
            // components has nothing for Deserialize to lose, so it is not
            // rejected here. `baseHasOwnStorage` walks the FULL base chain
            // (mirroring collectInstanceStorage's own recursion) checking
            // each level's OWN fields/auto-properties/captured posParams —
            // any single level with storage is disqualifying.
            auto classHasOwnStorage = [](const TypeSymbol* cls) -> bool {
                for (const Symbol* m : cls->members) {
                    auto* fs = dynamic_cast<const FieldSymbol*>(m);
                    if (fs) {
                        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
                        if (fd) {
                            bool isStat = false;
                            for (auto mod : fd->modifiers)
                                if (mod == "static") { isStat = true; break; }
                            if (!isStat) return true;
                        } else if (fs->posParam && fs->posParamBodyRef) {
                            return true;
                        }
                        continue;
                    }
                    if (isAutoPropertySymbol(m)) return true;
                }
                return false;
            };
            bool needsDeserHere = wantsID && !hwD2;
            bool hasBaseClass = !t->baseTypes.empty() &&
                                t->baseTypes[0]->kind == ast::TypeDeclKind::Class;
            bool baseHasOwnStorage = false;
            if (hasBaseClass) {
                for (const TypeSymbol* cur = t->baseTypes[0]; cur; ) {
                    if (classHasOwnStorage(cur)) { baseHasOwnStorage = true; break; }
                    cur = (!cur->baseTypes.empty() &&
                           cur->baseTypes[0]->kind == ast::TypeDeclKind::Class)
                              ? cur->baseTypes[0] : nullptr;
                }
            }
            if (needsDeserHere && hasBaseClass && baseHasOwnStorage) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    "class `" + std::string(t->name) + "` declares "
                    "`IDeserializable` and inherits from `" +
                    std::string(t->baseTypes[0]->name) + "`, which carries "
                    "its own serialized storage — synthesized class "
                    "deserialization does not yet support inheritance over a "
                    "base with storage (§9.8 does not specify base-class "
                    "field construction, and the synthesized Deserialize "
                    "does not construct the base subobject, which would "
                    "silently leave inherited fields at their default value "
                    "instead of the deserialized data — issue #2364); "
                    "remove the `IDeserializable` conformance declaration, "
                    "remove the base class, or hand-write a static "
                    "`Deserialize` yourself");
                continue;
            }

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
            //
            // §9.8 / #2325: a class's serialized components are its `owned`
            // and value-typed fields/auto-properties — which, for a class with
            // PRIMARY-CONSTRUCTOR parameters (§11.1), includes any parameter
            // CAPTURED into a private field (referenced in a body position —
            // a method/property/constructor/destructor body — represented as
            // a FieldSymbol with `posParam` set and `posParamBodyRef == true`,
            // since fs->decl for such a symbol is the owning TypeDecl, not a
            // FieldDecl — see the FieldSymbol comment in Symbol.hpp). A
            // parameter referenced ONLY in an initializer position generates
            // no field of its own (§11.1) — its value flows into whichever
            // explicit field consumes it, which is already counted there — so
            // it is excluded here to avoid double-counting. This resolves
            // against the stdlib §2.14 `Order` worked example, which
            // serializes `Id` via a captured `id` (a primary-ctor param read
            // only from `property Id: int => id;`), while `customer`/`lines`
            // — consumed only by the explicit `Customer`/`Lines` field
            // initializers — are not themselves separately counted.
            auto checkComponentType = [&](std::string_view fieldName,
                                          const TypeRef* tr,
                                          bool isOwned) -> int {
                // 0 = eligible; 1 = back-edge OK (not disqualifying);
                // 2 = error (already reported; caller sets foundBackEdge).
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
                            "tree`): field `" + std::string(fieldName) +
                            "` is a non-owned (borrowing) class-typed field "
                            "(a back-edge); `serialized tree` forbids aliasing — "
                            "mark the field `owned`, declare `serialized skip`, "
                            "or change the clause to `serialized graph`");
                        return 2;
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
                                "`): back-edge field `" + std::string(fieldName) +
                                "` has type `" + std::string(beDecl->name) +
                                "` which has no `serialized tree`/`graph`/`cyclic` "
                                "clause; the back-edge target must itself be "
                                "serialization-eligible (§9.8)");
                            return 2;
                        }
                    }
                    // Valid back-edge under graph/cyclic.
                    return 1;
                }
                if (!isSerializationEligibleType(tr, visited, ctx_)) {
                    reportIneligible(fieldName,
                        "has a type that is not serialization-eligible");
                    return 2;
                }
                return 0;
            };

            bool foundBackEdge = false;
            for (const Symbol* m : t->members) {
                // Fields (FieldDecl).
                auto* fs = dynamic_cast<const FieldSymbol*>(m);
                if (fs) {
                    auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
                    if (!fd) {
                        // §9.8 / #2325: a primary-constructor-parameter-
                        // derived FieldSymbol (see the comment above) — only
                        // a CAPTURED one is a real serialized component.
                        if (!fs->posParam || !fs->posParamBodyRef) continue;
                        if (!fs->posParam->type) continue;
                        const TypeRef* tr = ctx_.resolvedType(*fs->posParam->type);
                        if (!tr) continue;
                        bool isOwned = fs->posParam->modifier == "owned";
                        int r = checkComponentType(fs->name, tr, isOwned);
                        if (r == 2) { foundBackEdge = true; break; }
                        continue;
                    }
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
                    int r = checkComponentType(fs->name, tr, isOwned);
                    if (r == 2) { foundBackEdge = true; break; }
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
                if (!isSerializationEligibleType(tr, visited, ctx_)) {
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
                        if (!fd) {
                            // §9.8 / #2325: a captured primary-ctor-param
                            // field (see the eligibility-loop comment above)
                            // is a genuine serialized component too.
                            if (fs->posParam && fs->posParamBodyRef) {
                                compNamesLower.insert(toLower(fs->name));
                                compNamesOrig.push_back(std::string(fs->name));
                            }
                            continue;
                        }
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

                // §9.8 / #2325: a class's PRIMARY constructor (§11.1) — its
                // parameter list is `td->positionalFields` — is itself a
                // match candidate alongside any explicit `constructor(...)`
                // declarations. There is no separate ConstructorSymbol for
                // it (SymbolCollection.cpp creates one only for an explicit
                // ast::ConstructorDecl), so it is checked here directly from
                // the AST, using the exact same bijection rule as below.
                if (td->hasPositionalFields) {
                    std::string ctorDesc = "constructor(";
                    for (unsigned pi = 0; pi < td->positionalFields.size(); ++pi) {
                        if (pi) ctorDesc += ", ";
                        ctorDesc += std::string(td->positionalFields[pi].name);
                    }
                    ctorDesc += ")";
                    allCtors.push_back(ctorDesc);
                    if (td->positionalFields.size() == compNamesLower.size()) {
                        bool allMatch = true;
                        for (const auto& pf : td->positionalFields) {
                            if (compNamesLower.find(toLower(pf.name)) ==
                                    compNamesLower.end()) {
                                allMatch = false;
                                break;
                            }
                        }
                        if (allMatch) matchingCtors.push_back(ctorDesc);
                    }
                }

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
    }
}

// ---------------------------------------------------------------------------
// §22.12 rules 2 and 4 (v0.163, #1363; two-form admission v0.181, #1556):
// statics-and-domains checks.
// ---------------------------------------------------------------------------

// §22.12 rule 2 form (i) (v0.181): the compiler-known internally-synchronized
// handle set — a class-typed mutable static is admitted when its type is one of
//   Mutex, Semaphore, Atomic<T>, AtomicInteger<T>, ChannelSender<T>
// `Channel<T>` is EXCLUDED (v0.181, #1556): the channel object is the *receiving*
// end (§30.15), single-consumer by ownership, and a process-global receiving end
// would readmit the deferred multi-consumer receive (stdlib §6.9) — publish a
// `ChannelSender<T>` static instead. These builtins have a null `declaration`
// (their names resolve to a Named TypeRef with no user TypeSymbol), so the set is
// keyed on the syntactic head name. The rule 4 warning is suppressed for exactly
// this set (a `static readonly Channel<T>` therefore still warns).
static bool isInSynchronizedSet(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named || rt->declaration)
        return false;
    std::string_view n = rt->headName();
    if (n.empty()) return false;
    return n == "Mutex" || n == "Semaphore" ||
           n == "Atomic" || n == "AtomicInteger" ||
           n == "ChannelSender";
}

// §22.12 rule 2 form (ii) (v0.181, #1556): the lazy task cache. A mutable static
// whose DECLARED type is `Task?` or `Task<T>?` is admitted — the intended
// null-initialized once-set cache of an asynchronous computation (§22.13 pattern
// (b), Pattern A multi-await §30.5). The admission is keyed to the declared type
// alone (nullable + head `Task`), so it stays decidable at the declaration.
// `OwningTask<T>?` is NOT admitted: it is single-await (§30.5), useless as a
// shared cache, and a displaced owned task would strand its result — its head is
// `OwningTask`, not `Task`, so it is correctly excluded here. A non-nullable
// `Task`/`Task<T>` is likewise not admitted (the cache is null-initialized).
static bool isAdmittedTaskCache(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->nullable) return false;
    return rt->headName() == "Task";
}

// True when `rt` is a user-declared class or interface type (possibly nullable).
static bool isUserClassOrInterface(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named || !rt->declaration)
        return false;
    auto k = rt->declaration->kind;
    return k == ast::TypeDeclKind::Class ||
           k == ast::TypeDeclKind::Interface;
}

// True when `rt` is a compiler-known builtin *reference* (heap-pointer) type —
// `Task`, `OwningTask`, `Channel`, `ChannelSender`, the collections, the sync
// handles, etc. (the set codegen lowers to `ptr`). These builtins resolve to a
// Named TypeRef with a null `declaration`, so they are keyed on the head name via
// the shared registry. A bare generic parameter (`static x: T`) is also
// Named+null-declaration but its head is not a registered builtin, so it is
// correctly excluded. Value builtins (DateTime, Guid, Range, spans, …) and
// `string`/primitives/records/enums are NOT reference builtins and are left to
// §22.12 rule 3 / the #1393 primitive-string-record residual.
static bool isBuiltinReferenceType(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named || rt->declaration)
        return false;
    return common::isHeapPointerReferenceBuiltin(rt->headName());
}

// §22.12 rules 2 and 4 govern reference (class/interface) types: user-declared
// classes/interfaces AND compiler-known builtin reference types. The v0.163 check
// narrowed to user types only, which silently admitted every builtin class type
// as a mutable static (the #1556 bug: `Channel<T>`, `OwningTask<T>?`). string,
// primitives, records, enums, value builtins, and function types are NOT governed
// here (rule 3 / the #1393 residual).
static bool isRule2GovernedReference(const TypeRef* rt) {
    return isUserClassOrInterface(rt) || isBuiltinReferenceType(rt);
}

// §22.12 rule 4 (v0.181, #1556): the readonly-static declaration-site warning is
// emitted for user-declared class/interface types (as in v0.163) AND for the
// builtin `Channel<T>` — the case the v0.181 rule 4 text explicitly calls out (a
// `static readonly Channel<T>` still warns: the reminder that the channel is the
// single-consumer *receiving* end and must stay confined to one consuming
// domain, §30.15). The broader builtin-collection readonly warnings (a
// `static readonly Array<T>` / `List<T>`, a widespread and idiomatic form) remain
// deferred with the #1393 residual to avoid warning-spamming that idiom; only the
// Channel case is added here per the issue.
static bool isRule4GovernedReference(const TypeRef* rt) {
    if (isUserClassOrInterface(rt)) return true;
    return rt && rt->kind == TypeRef::Kind::Named && !rt->declaration &&
           rt->headName() == "Channel";
}

// Display spelling of a static field/property's reference type for a diagnostic:
// the user TypeSymbol's name when declared, else the builtin syntactic head.
static std::string staticRefTypeName(const TypeRef* rt) {
    if (rt && rt->declaration) return std::string(rt->declaration->name);
    return rt ? std::string(rt->headName()) : std::string();
}

// ---------------------------------------------------------------------------
// §22.12 rule 2 forms (iii)/(iv) and the residual error categories (#1393
// warned-admission ruling, spec v0.221 -> v0.228 text unchanged): the
// primitive/enum/plain-inline-array warned admission, the owning-inline-array
// static FIELD warned admission, and the compile-error residual (`string`,
// records, choices, value builtins, function types, generic-parameter types,
// disqualified inline arrays, and inline-array-typed auto-properties). This
// governs every mutable static NOT already handled by the class/interface
// path above (isRule2GovernedReference) — i.e. primitives, enums, records,
// choices, value builtins, `string`, function types, generic-parameter
// types, the unit type, and inline arrays of any of these.
// ---------------------------------------------------------------------------

// §22.12 rule 2 form (iii) primitive/enum leg: a primitive value type
// (§3.1 — bool, the integer types, float, double, decimal, char; `string`
// and `Object` excluded) or a user-declared enum type. `nullable` is a flag
// on the SAME TypeRef (not a distinct Kind), so the nullable form of either
// is covered with no extra check.
static bool isForm3PrimitiveOrEnum(const TypeRef* rt) {
    if (!rt) return false;
    if (rt->kind == TypeRef::Kind::Primitive)
        return rt->primitive != PrimitiveType::String &&
               rt->primitive != PrimitiveType::Object &&
               rt->primitive != PrimitiveType::None;
    if (rt->kind == TypeRef::Kind::Named && rt->declaration)
        return rt->declaration->kind == ast::TypeDeclKind::Enum;
    return false;
}

// §22.12 rule 2 form (iii) inline-array leg (fields only, gated by the
// caller): a plain inline array (`[N]T`, no `owned` slot marker, §15.1)
// whose element type is itself, recursively, form-(iii)-eligible. §15.1
// currently forbids an inline array being the element of another inline
// array (`[N][M]T` is a compile error), so the recursive arm is unreachable
// today; it is kept because the spec text states the admission recursively
// and a future multi-dimensional-array revision should not have to
// rediscover this.
static bool isForm3EligiblePlainInlineArray(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::InlineArray) return false;
    if (rt->arrayOwned) return false;
    const TypeRef* elem = rt->arrayElement;
    if (!elem) return false;
    return isForm3PrimitiveOrEnum(elem) || isForm3EligiblePlainInlineArray(elem);
}

// §22.12 rule 2 residual: `string`/`string?`.
static bool isStringTypeRef(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Primitive &&
           rt->primitive == PrimitiveType::String;
}

// §22.12 rule 2 residual: a user-declared record type.
static bool isRecordTypeRef(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Named && rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Record;
}

// §22.12 rule 2 residual: a user-declared choice type (§35).
static bool isChoiceTypeRef(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Named && rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Choice;
}

// §22.12 rule 2 residual: a compiler-known value-builtin type (DateTime,
// Guid, TimeSpan, CancellationToken, Range, …). Mirrors the two shapes these
// reach TypeResolution as (SemaContext.hpp's isValueBuiltinByName doc
// comment): a synthesized TypeSymbol with isValueBuiltin=true (reached via
// ExprTypecheck's `new T()` inference), or — the shape a declared static
// FIELD/property's type actually resolves to — a Named TypeRef with
// declaration=null whose syntactic head is one of the registered names.
static bool isValueBuiltinTypeRef(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named) return false;
    if (rt->declaration) return rt->declaration->isValueBuiltin;
    return isValueBuiltinByName(rt->headName());
}

// Primitive-type keyword spelling for diagnostics (mirrors
// ExprTypecheck_Calls.cpp's file-local spellTypeRefForDiag switch, which is
// not visible from this translation unit).
static const char* primitiveKeyword(PrimitiveType p) {
    switch (p) {
        case PrimitiveType::Bool:    return "bool";
        case PrimitiveType::Char:    return "char";
        case PrimitiveType::String:  return "string";
        case PrimitiveType::Byte:    return "byte";
        case PrimitiveType::SByte:   return "sbyte";
        case PrimitiveType::Short:   return "short";
        case PrimitiveType::UShort:  return "ushort";
        case PrimitiveType::Int:     return "int";
        case PrimitiveType::UInt:    return "uint";
        case PrimitiveType::Long:    return "long";
        case PrimitiveType::ULong:   return "ulong";
        case PrimitiveType::Float:   return "float";
        case PrimitiveType::Double:  return "double";
        case PrimitiveType::Decimal: return "decimal";
        case PrimitiveType::Object:  return "Object";
        default:                     return "?";
    }
}

// Display spelling of a static field/property's VALUE-kind type for a
// diagnostic (primitives, enums/records/choices/value-builtins, inline
// arrays, function types, the unit type) — the counterpart to
// staticRefTypeName above, which only handles the reference-type path.
static std::string describeStaticValueType(const TypeRef* rt) {
    if (!rt) return "?";
    switch (rt->kind) {
        case TypeRef::Kind::Primitive: {
            std::string s(primitiveKeyword(rt->primitive));
            if (rt->nullable) s += '?';
            return s;
        }
        case TypeRef::Kind::Named: {
            std::string s = rt->declaration ? std::string(rt->declaration->name)
                                            : std::string(rt->headName());
            if (s.empty()) s = "?";
            if (rt->nullable) s += '?';
            return s;
        }
        case TypeRef::Kind::InlineArray: {
            std::string s = "[";
            s += rt->arraySizeValue >= 0 ? std::to_string(rt->arraySizeValue)
                                          : "N";
            if (rt->arrayOwned) s += " owned";
            s += "]";
            s += describeStaticValueType(rt->arrayElement);
            return s;
        }
        case TypeRef::Kind::Function:
            return "<function type>";
        case TypeRef::Kind::Unit:
            return "()";
        case TypeRef::Kind::Error:
            return "?";
    }
    return "?";
}

void Resolver::checkTypeStaticFieldDomains(TypeSymbol* t) {
    // §22.12 rule 2 (v0.181, #1556): a mutable static reference field/property is
    // a compile error unless its type is a form-(i) synchronized handle or a
    // form-(ii) `Task?`/`Task<T>?` lazy cache. `what` is the member wording.
    auto reportRule2 = [&](const TypeRef* rt, std::string_view name,
                           std::string_view what, SourceLocation loc) {
        if (!isRule2GovernedReference(rt) || isInSynchronizedSet(rt) ||
            isAdmittedTaskCache(rt))
            return;
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("mutable static ") + std::string(what) + " `" +
            std::string(name) + "` has type `" + staticRefTypeName(rt) +
            "`, which §22.12 rule 2 does not admit as a mutable static; a "
            "mutable static of class or interface type must be an internally-"
            "synchronized handle (Mutex, Semaphore, Atomic<T>, "
            "AtomicInteger<T>, ChannelSender<T> — form (i); Channel<T> is "
            "excluded, publish a ChannelSender<T> instead) or a `Task?` / "
            "`Task<T>?` lazy cache (form (ii); OwningTask<T>? is not admitted) "
            "(§22.12 rule 2, v0.181, #1556)");
    };
    // §22.12 rule 4 (v0.181, #1556): a static readonly reference field/property
    // draws a declaration-site warning unless its type is in the form-(i)
    // synchronized set — the referenced instance is process-global and reachable
    // from every domain.
    auto reportRule4 = [&](const TypeRef* rt, std::string_view name,
                           std::string_view what, SourceLocation loc) {
        if (!isRule4GovernedReference(rt) || isInSynchronizedSet(rt)) return;
        ctx_.diagnostics().report(
            Severity::Warning, loc,
            std::string("static readonly ") + std::string(what) + " `" +
            std::string(name) + "` has class type `" + staticRefTypeName(rt) +
            "` which is reachable from every domain; confine its use to one "
            "domain or ensure its own internal synchronization "
            "(§22.12 rule 4, #1363)");
    };
    // §22.12 rule 2 forms (iii)/(iv) + residual (#1393 warned-admission
    // ruling): a mutable static NOT governed by the class/interface path
    // above (isRule2GovernedReference) is a primitive/enum/plain-inline-array
    // (form iii, WARNING), an owning-inline-array FIELD (form iv, WARNING),
    // or one of the residual categories the ruling declined to admit
    // (`string`, records, choices, value builtins, function types,
    // generic-parameter types, disqualified inline arrays, and any
    // inline-array-typed auto-property) — a compile ERROR naming rule 2.
    // `isField` distinguishes the FIELDS-only admission of (iii)'s
    // inline-array leg and all of (iv) from the auto-property case, where
    // every inline-array shape is a residual error regardless of element
    // type or `owned`-ness.
    auto reportValueStatic = [&](const TypeRef* rt, std::string_view name,
                                 std::string_view what, SourceLocation loc,
                                 bool isField) {
        if (!rt || rt->kind == TypeRef::Kind::Error) return;

        // A static whose declared type is one of the ENCLOSING TYPE's generic
        // parameters (`T`, `T?`) is deliberately left at status quo — no
        // warning, no error. §22.12 rule 2 enumerates its admitted forms by
        // TYPE, and a bare type parameter has no single one: the same
        // declaration is a primitive static in `Holder<int>` (form (iii)) and
        // a class-typed one in `Holder<Cell>` (rule 2's residual error), so
        // the category is only decided per monomorphization. Rule 2's prose
        // names no form for it, and diagnosing at the DECLARATION — the only
        // site this check runs at — would have to assume one instantiation
        // for all. Erroring here regressed the pre-existing spec-cited
        // `generic_static_state_536` round-trip cases (§22.12/§28.5), which
        // exercise exactly `public static owned shared: T?;`.
        // Per CLAUDE.md rule 1 this is left to a ruling rather than settled
        // in the implementation: see #2331.
        if (rt->kind == TypeRef::Kind::Named && !rt->declaration) {
            for (Symbol* gp : t->genericParams) {
                if (gp && gp->name == rt->headName()) return;
            }
        }

        // Form (iii) primitive/enum leg — admitted for fields AND
        // auto-properties alike; the "fields only" qualifier in the spec
        // text scopes only the inline-array alternative below.
        if (isForm3PrimitiveOrEnum(rt)) {
            ctx_.diagnostics().report(
                Severity::Warning, loc,
                std::string("mutable static ") + std::string(what) + " `" +
                std::string(name) + "` has type `" +
                describeStaticValueType(rt) + "` — a primitive/enum value "
                "static is admitted only with a declaration-site warning "
                "(§22.12 rule 2 form (iii)): the storage is process-global "
                "and reachable from every domain; confine every read and "
                "write to one domain, or use a synchronized spelling "
                "(`AtomicInteger<T>`/`Atomic<T>` for the cell-sized "
                "primitives, a `Mutex`/`Locked<T>` cell for `decimal` and "
                "enums) (§22.12 rule 2, v0.221, #1393)");
            return;
        }

        // Inline-array-typed statics: (iii)'s array leg and all of (iv) are
        // FIELDS only — an inline-array-typed auto-property is a residual
        // error regardless of element type or `owned`-ness.
        if (rt->kind == TypeRef::Kind::InlineArray) {
            if (!isField) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("mutable static ") + std::string(what) +
                    " `" + std::string(name) + "` has inline-array type `" +
                    describeStaticValueType(rt) + "`; §22.12 rule 2 admits "
                    "inline-array-typed mutable statics for FIELDS only "
                    "(forms (iii)/(iv)) — an inline-array-typed auto-"
                    "property is not admitted (§22.12 rule 2, v0.221, "
                    "#1393)");
                return;
            }
            if (rt->arrayOwned) {
                ctx_.diagnostics().report(
                    Severity::Warning, loc,
                    std::string("mutable static ") + std::string(what) +
                    " `" + std::string(name) +
                    "` has owning-inline-array type `" +
                    describeStaticValueType(rt) + "` — admitted with a "
                    "declaration-site warning (§22.12 rule 2 form (iv)): "
                    "the array's owned instances are reachable from every "
                    "domain and its slots are mutable; confine every slot "
                    "read/write, `move arr[j]`, and whole-field "
                    "reassignment to one domain, or restructure behind a "
                    "`Locked<T>` registry (§22.12 rule 2, v0.221, #1393)");
                return;
            }
            if (isForm3EligiblePlainInlineArray(rt)) {
                ctx_.diagnostics().report(
                    Severity::Warning, loc,
                    std::string("mutable static ") + std::string(what) +
                    " `" + std::string(name) + "` has type `" +
                    describeStaticValueType(rt) + "` — a plain inline array "
                    "of primitive/enum element type is admitted only with "
                    "a declaration-site warning (§22.12 rule 2 form (iii)): "
                    "confine every read and write to one domain, or use a "
                    "`Mutex`/`Locked<T>` cell (§22.12 rule 2, v0.221, "
                    "#1393)");
                return;
            }
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("mutable static ") + std::string(what) + " `" +
                std::string(name) + "` has type `" +
                describeStaticValueType(rt) + "`, which §22.12 rule 2 does "
                "not admit as a mutable static — a plain inline array's "
                "element type is outside the primitive/enum recursion (a "
                "`string`- or record-element array imports its element "
                "category's dilemma; a class- or interface-element array "
                "carries borrows, whose cross-domain dangling is memory-"
                "unsafe) (§22.12 rule 2, v0.221, #1393)");
            return;
        }

        // Residual: `string`, records, choices, value builtins, function
        // types, generic-parameter types, the unit type — none admitted.
        std::string_view reason;
        if (isStringTypeRef(rt))
            reason = "a `string`(-bearing) static's overwrite must either "
                     "release the displaced referent (a cross-domain "
                     "use-after-free) or never release it (an unbounded "
                     "leak); use a `Mutex`/`Locked<T>` cell instead";
        else if (isRecordTypeRef(rt))
            reason = "a multi-word record static's torn cross-domain read "
                     "can fabricate a value violating the record's own "
                     "invariants; use a `Mutex`/`Locked<T>` cell instead";
        else if (isChoiceTypeRef(rt))
            reason = "a multi-word choice static's torn cross-domain read "
                     "can fabricate an invalid value; use a "
                     "`Mutex`/`Locked<T>` cell instead";
        else if (isValueBuiltinTypeRef(rt))
            reason = "compiler-known value-builtin types (DateTime, Guid, "
                     "\xE2\x80\xA6) are not in the admitted category set; "
                     "use a `Mutex`/`Locked<T>` cell instead";
        else if (rt->kind == TypeRef::Kind::Function)
            reason = "function-typed statics are not in the admitted "
                     "category set";
        else
            reason = "the admitted category set (primitive/enum value "
                     "statics, owning-inline-array fields, or the "
                     "form-(i)/(ii) synchronized-handle/task-cache "
                     "reference forms) does not include this type";
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("mutable static ") + std::string(what) + " `" +
            std::string(name) + "` has type `" + describeStaticValueType(rt) +
            "`, which §22.12 rule 2 does not admit as a mutable static — " +
            std::string(reason) + " (§22.12 rule 2, v0.221, #1393)");
    };

    for (Symbol* m : t->members) {
        // Check FieldSymbol (body fields, not positional record fields).
        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
            if (!hasMod(fs, "static")) continue;
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) continue;

            const TypeRef* rt = ctx_.resolvedType(*fd->type);
            if (!rt) continue;

            if (!hasMod(fs, "readonly")) {
                if (isRule2GovernedReference(rt))
                    reportRule2(rt, fs->name, "field", m->declLoc);
                else
                    reportValueStatic(rt, fs->name, "field", m->declLoc,
                                      /*isField=*/true);
            } else {
                reportRule4(rt, fs->name, "field", m->declLoc);
            }
            continue;
        }

        // Check static auto-properties (PropertySymbol with Auto body).
        if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
            if (ps->isIndexer || !hasMod(ps, "static")) continue;
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd || pd->isIndexer ||
                pd->bodyKind != ast::PropertyBodyKind::Auto ||
                !pd->type)
                continue;

            const TypeRef* rt = ctx_.resolvedType(*pd->type);
            if (!rt) continue;

            if (!hasMod(ps, "readonly")) {
                if (isRule2GovernedReference(rt))
                    reportRule2(rt, ps->name, "property", m->declLoc);
                else
                    reportValueStatic(rt, ps->name, "property", m->declLoc,
                                      /*isField=*/false);
            } else {
                reportRule4(rt, ps->name, "property", m->declLoc);
            }
        }
    }
}

void Resolver::checkStaticFieldDomains() {
    for (const auto& sym : ctx_.symbols()) {
        auto* t = dynamic_cast<TypeSymbol*>(sym.get());
        if (!t) continue;
        checkTypeStaticFieldDomains(t);
    }
}

// §22.15 (v0.202, #2052): true iff `rt` is a user-declared CLASS TypeRef
// (the case the `owned`-return restriction targets — class-typed returns
// carry disposal). Built-in and non-class types return false.
namespace {
bool isUserClassTypeRef(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Named && rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Class;
}
}  // namespace

void Resolver::checkPureModifiers() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypePureModifiers(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypePureModifiers(nt->type);
            }
        }
    }
}

void Resolver::checkTypePureModifiers(TypeSymbol* t) {
    // §22.15 "Where `pure` may appear" / "Parameters and result" — placement
    // and signature restrictions only; the pure-body rule set (closed body,
    // throw carve-out, pure constructors) is a separate slice. Per-member:
    for (Symbol* m : t->members) {
        // Skip nested-type wrappers; their inner type was visited above.
        if (dynamic_cast<NestedTypeSymbol*>(m)) continue;

        // ---- MethodDecl / OperatorDecl -------------------------------------
        if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
            // MethodDecl carries `pure` in its modifiers; OperatorDecl
            // carries it on the AST (`OperatorDecl::isPure`) since the
            // operator parser doesn't propagate modifiers into the symbol.
            const auto* opDecl =
                dynamic_cast<const ast::OperatorDecl*>(ms->decl);
            const bool isPure = hasMod(ms, "pure") ||
                                (opDecl && opDecl->isPure);
            if (!isPure) continue;
            // §22.15 "Where pure is rejected": `pure` and `async` are
            // mutually exclusive (the two share async's modifier slot, and
            // an `async` body interleaves with the domain's continuation
            // queue). Operators are always `public static`, so the
            // combination cannot arise there.
            if (hasMod(ms, "async")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("`pure` combines with neither `async` nor "
                                "`yield`; method `") +
                    std::string(ms->name) +
                    "` is declared `async` (\xC2\xA7""22.15)");
                continue;
            }
            // §22.15 "Where pure is rejected": iterator methods (a body
            // containing `yield`) may not be `pure`. Operators and abstract
            // methods have no body to check.
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                bool bodyHasYield = false;
                for (const auto& s : md->stmts) {
                    if (s && dynamic_cast<const ast::YieldStmt*>(s.get())) {
                        bodyHasYield = true;
                        break;
                    }
                }
                if (bodyHasYield) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("`pure` combines with neither `async` nor "
                                    "`yield`; method `") +
                        std::string(ms->name) +
                        "` is an iterator method (its body contains "
                        "`yield`) (\xC2\xA7""22.15)");
                    continue;
                }
            }
            // §22.15 "Parameters and result": no ref/out/out-owned/owned/
            // scoped parameters.
            for (Symbol* p : ms->params) {
                auto* ps = dynamic_cast<ParameterSymbol*>(p);
                if (!ps) continue;
                if (!ps->modifier.empty()) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("a `pure` member declares no "
                                    "`ref`/`out`/`out owned`/`owned`/`scoped` "
                                    "parameter; method `") +
                        std::string(ms->name) + "` parameter `" +
                        std::string(ps->name) + "` carries modifier `" +
                        std::string(ps->modifier) + "` (\xC2\xA7""22.15)");
                }
            }
            // §22.15 "Parameters and result": no `owned` class-typed return.
            bool retOwned = opDecl ? opDecl->returnOwned : false;
            const ast::Type* retTy = opDecl ? opDecl->returnType.get()
                                            : nullptr;
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                retOwned = md->returnOwned;
                retTy = md->returnType.get();
            }
            if (retOwned && retTy) {
                const TypeRef* rt = ctx_.resolvedType(*retTy);
                if (isUserClassTypeRef(rt)) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("a `pure` member declares no `owned` "
                                    "class-typed return; method `") +
                        std::string(ms->name) +
                        "` returns an owned class instance "
                        "(\xC2\xA7""22.15)");
                }
            }
            continue;
        }

        // ---- ConstructorDecl ----------------------------------------------
        if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
            const auto* cd =
                dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (!cd || !cd->isPure) continue;
            // §22.15 "Where pure is rejected": static constructors admit no
            // `pure` (no call site exists to consume the fact).
            if (cd->isStatic) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "`pure` is not admitted on a static constructor "
                    "(\xC2\xA7""22.15)");
                continue;
            }
            // §22.15 "Parameters and result": same parameter-modifier rule
            // as for methods.
            for (Symbol* p : cs->params) {
                auto* ps = dynamic_cast<ParameterSymbol*>(p);
                if (!ps) continue;
                if (!ps->modifier.empty()) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("a `pure` member declares no "
                                    "`ref`/`out`/`out owned`/`owned`/`scoped` "
                                    "parameter; constructor parameter `") +
                        std::string(ps->name) + "` carries modifier `" +
                        std::string(ps->modifier) + "` (\xC2\xA7""22.15)");
                }
            }
            continue;
        }

        // ---- DestructorDecl ------------------------------------------------
        if (dynamic_cast<DestructorSymbol*>(m)) {
            // A destructor carries no modifiers vector in the AST, so `pure`
            // on a destructor cannot reach sema via the modifier-collection
            // loop. Still, defend against the case: if a future change stores
            // modifiers on DestructorDecl, the rule still applies.
            if (hasMod(dynamic_cast<MemberSymbol*>(m), "pure")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "`pure` is not admitted on a destructor — disposal is the "
                    "effect context purity fences off (\xC2\xA7""22.15)");
            }
            continue;
        }

        // ---- PropertyDecl / IndexerDecl -----------------------------------
        if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
            if (!hasMod(ps, "pure")) continue;
            const auto* pd =
                dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd) continue;
            // §22.15: writing `pure` on the auto-property form is rejected
            // as redundant — the auto-getter is implicitly pure-classified.
            // An interface property's signature-only form parses as Auto but
            // is an abstract declaration, not an auto-property with backing
            // storage — pure is the contract, not redundant.
            const bool parentIsIface =
                ps->parent && ps->parent->kind == ast::TypeDeclKind::Interface;
            if (pd->bodyKind == ast::PropertyBodyKind::Auto &&
                !parentIsIface) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("`pure` on an auto-property is redundant — the "
                                "auto-getter is implicitly pure-classified; "
                                "property `") +
                    std::string(ps->name) + "` (\xC2\xA7""22.15)");
                continue;
            }
            // §22.15 "Purity attaches to the getter": a read-write block-form
            // property takes per-accessor `pure get` instead of property-level
            // `pure`. Property-level `pure` is for read-only / expression-bodied
            // shapes only.
            if (pd->bodyKind == ast::PropertyBodyKind::Block) {
                bool hasSet = false;
                for (const auto& a : pd->accessors)
                    if (a.isSet) { hasSet = true; break; }
                if (hasSet) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("property-level `pure` marks the single "
                                    "getter; a read-write block-form property "
                                    "uses per-accessor `pure get` instead; "
                                    "property `") +
                        std::string(ps->name) + "` (\xC2\xA7""22.15)");
                    continue;
                }
            }
            continue;
        }

        // ---- FieldDecl -----------------------------------------------------
        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
            if (hasMod(fs, "pure")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("`pure` is not admitted on a field; field `") +
                    std::string(fs->name) + "` (\xC2\xA7""22.15)");
            }
            continue;
        }

        // ---- EventDecl -----------------------------------------------------
        if (dynamic_cast<EventSymbol*>(m)) {
            if (hasMod(dynamic_cast<MemberSymbol*>(m), "pure")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "`pure` is not admitted on an event — events are the "
                    "synchronization surface purity fences off "
                    "(\xC2\xA7""22.15)");
            }
            continue;
        }

        // ---- ConstDecl / EnumConstantSymbol --------------------------------
        if (dynamic_cast<ConstMemberSymbol*>(m) ||
            dynamic_cast<EnumConstantSymbol*>(m)) {
            if (hasMod(dynamic_cast<MemberSymbol*>(m), "pure")) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "`pure` is not admitted on a const or enum constant "
                    "(\xC2\xA7""22.15)");
            }
            continue;
        }
    }

    // §22.15 "Purity attaches to the getter; `pure` on `set` is always
    // rejected." A second pass over property accessors so a `pure set` inside
    // any block-form property surfaces regardless of the property's own
    // modifiers.
    for (Symbol* m : t->members) {
        auto* ps = dynamic_cast<PropertySymbol*>(m);
        if (!ps) continue;
        const auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd || pd->bodyKind != ast::PropertyBodyKind::Block) continue;
        for (const auto& a : pd->accessors) {
            if (a.isPure && a.isSet) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("`pure` on a `set` accessor is always "
                                "rejected — purity attaches to the getter "
                                "only (\xC2\xA7""22.15)"));
            }
        }
    }
}

// §22.15 (v0.202, #2052) "Checking and dispatch": the load-bearing rule-1
// checks (override-of-pure-must-be-pure and impl-of-pure-iface-must-be-
// pure) are wired into checkHidingAndOverride and checkAbstractImpl-
// ementations respectively. The standalone walker below is reserved for
// future rule-3 chain-wide work (monotone-down-the-chain audits) — for
// now it iterates members so per-type entry points are symmetric with the
// other pure checks, and any future chain-wide invariant can grow here
// without re-threading the override walker.
void Resolver::checkPureDispatch() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        checkTypePureDispatch(t);
        for (Symbol* m : t->members) {
            if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                if (nt->type) checkTypePureDispatch(nt->type);
            }
        }
    }
}

void Resolver::checkTypePureDispatch(TypeSymbol* t) {
    (void)t;
    // Rule 1 (override-of-pure and impl-of-pure-iface) is enforced inline
    // in checkHidingAndOverride and checkAbstractImplementations — see the
    // §22.15 markers there. Rule 2 (strengthening) is permissive. Rule 3
    // (monotone down the chain) is enforced transitively by rule 1's
    // nearest-base match.
}

// §23.2 (v0.214, issue #2088, proposal 0058): `readonly` is written exactly
// where the SHAPE cannot express read-onlyness — auto-properties (§23.1) and
// interface properties, whose abstract accessor sets are declared rather than
// implemented (so the absencd-of-setter cannot express it). On every BODY-
// BEARING form — an expression-bodied property or indexer (`=>`), a get-only
// block-form accessor list, a block form declaring a setter, and the
// expression-bodied properties of choices / enums / extensions — read-onlyness
// is already expressed by the shape (or contradicted by it). The grammar's
// `readonly?` slot on the body-bearing productions is the shared-production
// over-admission; this pass performs the §23.2 semantic rejection.
//
// Accept (read-only is meaningful):  bodyKind == Auto on class / record /
//                                    choice / enum / extension, and on
//                                    interface (signature form).
// Reject (redundant or contradictory): bodyKind == Expression any depth;
//                                      bodyKind == Block regardless of
//                                      accessors, AND indexer with body.
void Resolver::checkReadonlyPropertyForms() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            checkTypeReadonlyPropertyForms(t);
            for (Symbol* m : t->members) {
                if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
                    if (nt->type) checkTypeReadonlyPropertyForms(nt->type);
                }
            }
            continue;
        }
        // §23.2: extensions (`ExtensionSymbol`) live at module scope, not
        // under a TypeSymbol, and their members are tagged with `parent =
        // nullptr` during symbol collection (§26, #433). The expression-
        // bodied property of an extension is one of the three body-bearing
        // grammar slots named in issue #2088; visit them explicitly.
        if (auto* ext = dynamic_cast<ExtensionSymbol*>(child)) {
            for (Symbol* m : ext->members) {
                auto* ps = dynamic_cast<PropertySymbol*>(m);
                if (!ps) continue;
                checkTypeReadonlyPropertyFormsMember(ps);
            }
        }
    }
}

void Resolver::checkTypeReadonlyPropertyForms(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* ps = dynamic_cast<PropertySymbol*>(m);
        if (!ps) continue;
        checkTypeReadonlyPropertyFormsMember(ps);
    }
}

// §23.2 per-member check — extracted so extensions (`ExtensionSymbol`,
// which lives at module scope) can share it.
void Resolver::checkTypeReadonlyPropertyFormsMember(PropertySymbol* ps) {
    // §23.2: a `readonly` property is meaningful only on the Auto form.
    // Anything body-bearing — expression body, indexer arrow, block body
    // (get-only or setter-bearing) — is rejected. Interface properties
    // may carry `readonly` (their abstract accessor set is declared, not
    // implemented) and they parse to PropertyDecl with bodyKind=Auto
    // when in signature form, so the Auto branch below accepts them.
    if (!hasMod(ps, "readonly")) return;
    const auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return;

    const bool parentIsIface =
        ps->parent && ps->parent->kind == ast::TypeDeclKind::Interface;

    // Auto body: the SHAPE cannot express read-onlyness — accept.
    // (This covers class/record auto-properties AND interface
    // signature-form properties; both reject nothing here.)
    if (pd->bodyKind == ast::PropertyBodyKind::Auto) {
        // An interface property declared with an abstract block body
        // (`{ get; end }`) is body-bearing and SHOULD be rejected; the
        // check below handles Block. For Auto-body interface props this
        // branch accepts and we are done.
        return;
    }

    // From here on, the property has a body — `readonly` is redundant
    // or contradictory. The diagnostic message differs by shape so the
    // user sees the §23.2 reason in their tool window.
    std::string whatName;
    if (pd->isIndexer) {
        whatName = "indexer";
    } else if (parentIsIface) {
        whatName = "interface property";
    } else {
        whatName = "property";
    }

    const Symbol* m = ps;
    if (pd->bodyKind == ast::PropertyBodyKind::Expression) {
        // Expression-bodied form — read-onlyness is expressed by the
        // shape, so `readonly` is redundant. Applies to class/record
        // expression-bodied properties (PropertyDecl arrow), and to the
        // expression-bodied properties of choices / enums / extensions
        // (ExpressionPropertyDecl — same AST node, same bodyKind), and
        // to expression-bodied indexers (IndexerDecl arrow).
        ctx_.diagnostics().report(
            Severity::Error, m->declLoc,
            std::string("`readonly` is redundant on an expression-bodied ") +
            whatName + " — the `=>` form is read-only by shape; remove the "
            "`readonly` keyword (§23.2, v0.214)");
        return;
    }

    // Block body — accessors are declared. Two reject cases:
    //   * a `set` accessor contradicts `readonly` (semantic conflict);
    //   * a get-only block ALSO expresses read-onlyness by shape, so
    //     `readonly` is redundant rather than contradictory.
    bool hasSet = false;
    for (const auto& a : pd->accessors)
        if (a.isSet) { hasSet = true; break; }

    if (hasSet) {
        ctx_.diagnostics().report(
            Severity::Error, m->declLoc,
            std::string("`readonly` is contradictory on a block-form ") +
            whatName + " that declares a `set` accessor — remove the "
            "`readonly` keyword or remove the `set` arm (§23.2, v0.214)");
    } else {
        ctx_.diagnostics().report(
            Severity::Error, m->declLoc,
            std::string("`readonly` is redundant on a get-only block-form ") +
            whatName + " — omitting `set` already expresses read-onlyness; "
            "remove the `readonly` keyword (§23.2, v0.214)");
    }
}

}  // namespace bezel::sema
