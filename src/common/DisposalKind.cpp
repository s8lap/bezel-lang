#include "common/DisposalKind.hpp"

#include <algorithm>
#include <vector>

#include "ast/Node.hpp"
#include "common/BuiltinRegistry.hpp"
#include "common/OwnershipFacts.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::common {

namespace {

// §28.9: the `Type` TOKEN is runtime-managed — "the instance is not
// compiler-freed at any holder's release point ... the trivial-dispose free
// below would set the disposed flag on a token every other holder still
// reads".
//
// The token's type is the BUILTIN `Type` class, i.e. the `builtinClassSym`
// shape: a declaration with no `ast::TypeDecl` and no originating module.
// That is the same discrimination `Emitter::builtinTypeName` makes (an
// IMPORTED user class has exactly the same "no ast::TypeDecl" shape, and the
// module loader is what tells them apart), so a user class named `Type` —
// legal under §2.3's shadowing rule — is NOT the token.
bool isTypeTokenRef(const sema::TypeRef* t) {
    if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
    const sema::TypeSymbol* d = t->declaration;
    if (!d) return false;
    return !d->decl && d->moduleName.empty() && d->name == "Type";
}

// The class chain of `cls`, base-first, without repeats.
void collectClassChain(const sema::TypeSymbol* cls,
                       std::vector<const sema::TypeSymbol*>& out) {
    std::vector<const sema::TypeSymbol*> work;
    if (cls) work.push_back(cls);
    while (!work.empty()) {
        const sema::TypeSymbol* t = work.back();
        work.pop_back();
        if (!t) continue;
        if (std::find(out.begin(), out.end(), t) != out.end()) continue;
        out.push_back(t);
        for (sema::TypeSymbol* b : t->baseTypes) work.push_back(b);
    }
}

// A `static` member is not an instance member: `DisposeAsync` is an instance
// method (§28.12 awaits it on the object), so a `static` one named
// `DisposeAsync` is not the §28.12 member.
bool isStaticMember(const sema::MethodSymbol* ms) {
    for (std::string_view mod : ms->modifiers)
        if (mod == "static") return true;
    return false;
}

// §28.4 / §28.5: does the class carry a DESTRUCTOR or an `owned` FIELD? This
// is the §28.10 vacuity lint's own rule (a destructor or an `owned` field),
// NOT `common::isNonTrivialDisposeClass` — fix #3588 kept the two apart on
// purpose, and the reason is recorded on that predicate: the lint also
// matches INTERFACES, does not walk base chains, and reads a positional
// field's modifier from a different place.
bool classCarriesDisposal(const sema::TypeSymbol* d) {
    for (sema::Symbol* m : d->members) {
        if (dynamic_cast<sema::DestructorSymbol*>(m)) return true;
        auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
        if (!fs) continue;
        for (std::string_view mod : fs->modifiers)
            if (mod == "owned") return true;
    }
    return false;
}

}  // namespace

bool isRuntimeManagedValue(const sema::TypeRef* t) {
    if (!t) return false;
    switch (t->kind) {
    case sema::TypeRef::Kind::Primitive:
        // §28.9: a `string` value is runtime-managed.
        return t->primitive == sema::PrimitiveType::String;
    case sema::TypeRef::Kind::Function:
        // §28.9: a function value is runtime-managed — "a `using var` local of
        // function type is nonetheless permitted (disposal at scope end is a
        // no-op)".
        return true;
    case sema::TypeRef::Kind::Named:
        // §28.9: the `Type` token is runtime-managed.
        return isTypeTokenRef(t);
    default:
        // A BOXED VALUE TYPE is deliberately NOT here: §28.12 permits the
        // synchronous disposal for it, because a box's word 0 IS a vtable.
        return false;
    }
}

const sema::TypeRef* representationOf(const sema::TypeRef* rhsTy,
                                       const GenericTypeSubst* subst) {
    if (!rhsTy) return nullptr;
    if (subst && !subst->empty() &&
        rhsTy->kind == sema::TypeRef::Kind::Named && !rhsTy->declaration) {
        // A still-unbound type parameter: resolve it through the live
        // instantiation's substitution, so a MONOMORPHIZED head can be named
        // at all (`new T()` at `T = Stopwatch` — #3482). This is the same
        // lookup `Emitter::resolveTypeParamRef` performs; the anchor keeps
        // its own copy so the sema and codegen sites read ONE authority.
        auto it = subst->find(rhsTy->headName());
        if (it != subst->end() && it->second) return it->second;
    }
    return rhsTy;
}

bool classDeclaresDisposeAsync(const sema::TypeSymbol* cls) {
    std::vector<const sema::TypeSymbol*> chain;
    collectClassChain(cls, chain);
    for (const sema::TypeSymbol* t : chain) {
        for (sema::Symbol* m : t->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
            if (!ms) continue;
            if (ms->name != "DisposeAsync") continue;
            if (isStaticMember(ms)) continue;
            return true;
        }
    }
    return false;
}

DisposalVerdict classifyDisposal(const sema::TypeRef* t, sema::SemaContext& ctx) {
    DisposalVerdict v;
    if (!t) return v;

    switch (t->kind) {
    case sema::TypeRef::Kind::Primitive:
        // §4: `Object` is the ONE primitive that may hold a class instance.
        if (t->primitive == sema::PrimitiveType::Object) v.mayHoldClass = true;
        // Every other primitive is a value type or a runtime-managed value
        // (§28.9): no class, no disposal obligation.
        return v;
    case sema::TypeRef::Kind::Function:
        // §28.9: a function value is runtime-managed.
        return v;
    case sema::TypeRef::Kind::InlineArray:
        // §15.3: an OWNING inline array is move-only — its owned slots are
        // disposed in place.
        if (t->arrayOwned) v.kind = DisposalKind::MoveOnly;
        // The unit type, an inline array and an error type are NOT
        // classifiable here, and the pre-existing path kept them ("may hold a
        // class" — the old `typeMayHoldClass` answered "unclassifiable" and its
        // caller kept the previous path). Preserved exactly.
        v.mayHoldClass = true;
        return v;
    case sema::TypeRef::Kind::Named:
        break;
    default:
        // The unit type, an error type: not classifiable here — keep the
        // pre-existing path (may hold a class).
        v.mayHoldClass = true;
        return v;
    }

    // §28.9: the `Type` token is runtime-managed — it has no class to
    // dispatch on, and the class disposal path would read its word 0 as a
    // vtable (#7338).
    if (isTypeTokenRef(t)) return v;

    const sema::TypeSymbol* d = t->declaration;
    if (!d) {
        // A DECLARATION-LESS head: a type parameter, a builtin collection
        // head, or one of the built-in INTERFACE heads (the disposal family),
        // which have no TypeSymbol at all.
        const std::string_view head = t->headName();
        if (isBuiltinDisposalFamilyHead(head)) {
            // §28.12: the disposal selects the run-time object's chain —
            // "through any interface — `IAsyncDisposable` or one deriving
            // from it" is the awaited path; every other family member
            // ("another such as `IDisposable`, which every class implements")
            // is the synchronous one. The async-vs-sync split is DERIVED from
            // the family chain table (fix #7497), the one list the stdlib
            // ratchet checks, not re-listed here by name.
            v.kind = builtinDisposalFamilyIsAsync(head)
                         ? DisposalKind::ClassAsync
                         : DisposalKind::ClassSync;
            v.mayHoldClass = true;
            v.carriesObligation = true;
            return v;
        }
        if (isDisposableBuiltinHeadName(head)) {
            // §28.3 / §28.9: the built-in heads that carry a disposal
            // obligation of their own (the shared table both sides read).
            v.kind = DisposalKind::ClassSync;
            v.mayHoldClass = true;
            v.carriesObligation = true;
            return v;
        }
        // An unknown head (a type parameter, an unrelated interface): it MAY
        // be a class instance at run time — the conservative answer the
        // codegen registration needs. It carries no obligation of its own, so
        // the §28.10 vacuity lint still answers "no obligation" for it.
        v.mayHoldClass = true;
        return v;
    }

    if (d->kind == ast::TypeDeclKind::Class ||
        d->kind == ast::TypeDeclKind::Interface) {
        v.cls = d;
        v.mayHoldClass = true;
        // §28.12: "the RHS's runtime class implements `IAsyncDisposable`" —
        // the class that carries the awaited `DisposeAsync`.
        v.kind = classDeclaresDisposeAsync(d) ? DisposalKind::ClassAsync
                                              : DisposalKind::ClassSync;
        // §28.10: the vacuity lint's own rule — a destructor or an `owned`
        // field (fix #3588 kept this separate from the shared
        // `isNonTrivialDisposeClass` on purpose).
        v.carriesObligation = classCarriesDisposal(d);
        return v;
    }

    // A record / enum / choice — a VALUE TYPE (§35.4).
    if (auto cv = classifyChoiceType(t, ctx)) {
        if (*cv == ChoiceClassification::Owning) {
            // §35.4: an owning choice is move-only — its active case's owned
            // payload is disposed in place.
            v.kind = DisposalKind::MoveOnly;
            return v;
        }
        if (*cv == ChoiceClassification::ValueLike) {
            // §35.3 / §35.4: a value-like choice's active case may hold a
            // runtime-managed payload with a +1 reference; scope exit
            // RELEASES that reference.
            v.kind = DisposalKind::ValueLikePayload;
            return v;
        }
    }
    {
        const char* kindName = nullptr;
        if (isMoveOnlyValueType(t, ctx, kindName)) {
            // §15.3: an owning inline array or an owning choice.
            v.kind = DisposalKind::MoveOnly;
        }
    }
    return v;
}

}  // namespace bezel::common
