#include "sema/TypeResolution_impl.hpp"

#include "common/BuiltinRegistry.hpp"
#include "common/OwnershipFacts.hpp"  // classifyChoiceType — §35.7 hashable choice edge (#2087)

#include "sema/ConstFold.hpp"
#include "sema/EnumValues.hpp"  // enumHasZeroConstant — §17.11 default(E) (#3044)
#include "sema/FieldSymbolAccessors.hpp"  // fieldDeclTypeOf — §28.9/§15.1.1 (#3650)
#include "sema/GenericParams.hpp"  // detail::astGenericParams — §14.6 in-scope name set (#3271)
#include "sema/NestedTypeWalk.hpp"  // fix #3597: promoted so PureChecker.cpp can share it too
#include "sema/Sendability.hpp"  // isSendableType — §30.14 sendability computation
#include "sema/SubstArgs.hpp"  // fix #4212: shared generic-substitution walker (§14.1 / §27.8, v0.298)
#include "sema/TypeResolution.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <set>
#include <tuple>
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
        // fix #3650 (§28.9/§15.1.1, #3152 drain): the shared both-sources
        // field-type accessor — positional param first, then (LOCAL only)
        // the FieldDecl. `cur` can be an IMPORTED record/choice reached
        // through a value-typed field (see the header comment above), in
        // which case `fieldDeclTypeOf` returns null and this field's
        // component is simply not explored further. That is sound, not a
        // false negative, TODAY: Driver's `compileObject`/`compileFile`
        // dependency model is acyclic-only (src/driver/Driver.hpp,
        // `compileObject`'s doc: "`deps` lets a dependency itself import
        // further dependencies (acyclic)"), so no path leaving into an
        // imported type can currently lead back to `start`, and any
        // self-cycle wholly inside the dependency was already rejected when
        // THAT module was compiled (this same check, run there with every
        // symbol local). This is a DRIVER LIMITATION, not a language
        // guarantee — §2.8.1 rule 4 (v0.286) states plainly that "import
        // cycles between distinct modules remain legal and compile as an
        // SCC" (§2.5). `recordValueContainsSelf` MUST be revisited when SCC
        // import-cycle compilation lands: at that point a value-typed field
        // chain could leave `start`'s module, cross an SCC member back into
        // it, and this walk's silent stop at the first imported symbol
        // would need to resume across the cycle instead of assuming safety.
        const ast::Type* ftype = sema::fieldDeclTypeOf(fs);
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
// #2787: the single definition of the §14.2-rule-3 self-recurrence test. See
// TypeResolution.hpp for why this is exact rather than a proxy.
bool selfComparableIface(const TypeRef* t, const TypeSymbol* icomparable) {
    if (!t || t->nullable || !icomparable ||
        t->kind != TypeRef::Kind::Named || !t->declaration ||
        t->declaration->kind != ast::TypeDeclKind::Interface)
        return false;
    for (const TypeRef* bt : t->declaration->baseTypeRefs) {
        if (!bt || bt->nullable || bt->kind != TypeRef::Kind::Named ||
            bt->declaration != icomparable || bt->genericArgs.size() != 1)
            continue;
        if (typeRefEqual(bt->genericArgs[0], t)) return true;
    }
    return false;
}

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

// fix #3574 (§11.6 "Nested types", v0.286), fix #3597: the ONE recursive
// type-and-nested-types walk the module-level checks in this file share --
// `visitTypeAndNested` / `forEachTypeAndNested` (free functions) now live in
// sema/NestedTypeWalk.hpp, promoted out of this TU so PureChecker.cpp (a
// different translation unit with no `Resolver` instance) can share the
// identical walk rather than carrying a sixteenth hand-copy. See that
// header for the full rationale and the measured 907ce0f depth-0/1/2 ladder.
// `Resolver::forEachTypeAndNested` stays as a thin member wrapper so the
// thirteen `check*` passes below don't need to thread `ctx_.module()`
// themselves.
void Resolver::forEachTypeAndNested(
    const std::function<void(TypeSymbol*)>& fn) {
    bezel::sema::forEachTypeAndNested(ctx_.module(), fn);
}

// §13.3 (#956): every type appearing in a member's signature must be at least
// as visible as the member; "a private member is unconstrained". A private
// (nested) type is the least visible level, so a NON-private member whose
// signature names a private type is the unambiguous violation (the repro:
// `internal _x: NestedPrivate?`). We target that clear case — a broader rank
// comparison across internal/protected/public is a follow-up. Builtins and
// generic parameters (null declaration) are fully visible and never flagged.
void Resolver::checkMemberSignatureVisibility() {
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested(
        [&](TypeSymbol* t) { checkTypeMemberSignatureVisibility(t); });
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
            // SEAM-ACCESSOR: §13.3 signature-visibility walk: `fs` is a member of `t`, which
            // forEachTypeAndNested (#3574) draws only from this module's own
            // declaration tree (#3152) — never an imported symbol.
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
                checkSig(fd->type.get(), mem, "field");
        } else if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
            // SEAM-ACCESSOR: same walk as the field arm just above — `ms` is a member of the
            // locally-declared `t` (#3152).
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                checkSig(md->returnType.get(), mem, "return");
                for (const auto& p : md->params)
                    checkSig(p.type.get(), mem, "parameter");
            }
        }
    }
}

void Resolver::checkStaticConstructorParams() {
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypeStaticCtorParams(t); });
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
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypeConstructorChains(t); });
}

bool Resolver::isChainCall(const ast::Stmt* s, bool* isThis) {
    // fix #3624: forward to the ONE §12.8 chain-call recognizer (Node.hpp) --
    // this used to be its own six-line dynamic_cast walk.
    if (!s) return false;
    bool chainIsThis = false;
    const ast::CallExpr* chain = ast::chainCallOfStmt(*s, chainIsThis);
    if (!chain) return false;
    if (isThis) *isThis = chainIsThis;
    return true;
}

void Resolver::checkTypeConstructorChains(TypeSymbol* t) {
    // fix #3535 (§22.10 "Primary constructors" / §22.7 "Constructors",
    // v0.283): "A class with a primary constructor may declare additional
    // convenience constructors. Each must chain to another constructor via
    // `this(...)`; only the primary may chain to `base(...)`." §22.7 states
    // the same restriction from the constructor side. A class's primary
    // constructor is carried on the TypeDecl, NOT as a ConstructorSymbol
    // (see `checkNewConstraintSatisfied`), so in such a class EVERY declared
    // instance constructor is a convenience constructor and owes a leading
    // `this(...)`.
    //
    // This was not enforced anywhere, and the omission is memory-unsafe
    // rather than merely permissive: the `new` site stores the call's
    // arguments into the primary-constructor field slots by POSITION
    // (`Emitter::storePrimaryCtorPositionalArgs`, LlvmBackend_Exprs2.cpp),
    // pairing argument i with positional field i. When a convenience
    // constructor does not chain, nothing ever re-stores those slots with
    // the values the object should hold, so the blind pairing survives to
    // the first field read. Measured on e57a1eb with
    // `class B(n: int, s: string)` + a non-chaining `constructor(v: int,
    // w: int)`: `new B(5, 9)` stored the integer 9 into the `s: string`
    // slot and SIGSEGVed on `Show()`. `hasPositionalFields` is read off the
    // TypeSymbol (#501) rather than the TypeDecl for the same reason #3141
    // moved the codegen gate there.
    const bool classHasPrimaryCtor =
        t->kind == ast::TypeDeclKind::Class && t->hasPositionalFields;
    // fix #4999 (§9.1 / §9.3 / §12.8 / §22.10, v0.307): a record with
    // positional fields ALSO has a primary constructor — §9.1 "the compiler
    // generates: A primary constructor whose parameters are the positional
    // fields" — and §9.3 keeps "Chaining with `this(...)` to the primary
    // ... governed by §12.8 unchanged". Like a class's §22.10 primary, it
    // has no ConstructorSymbol, so the #3793 arity heuristic below, which
    // counts only ConstructorSymbols as chain candidates, would read a
    // body constructor's legal chain to the primary as a self-chain
    // whenever the arities coincide. The #3793 self-chain check therefore
    // exempts every type with a primary list, class or record; the §22.10
    // convenience-must-chain rule further down stays CLASS-only (§9.3: it
    // "is a class rule").
    const bool typeHasPrimaryCtor =
        t->hasPositionalFields && (t->kind == ast::TypeDeclKind::Class ||
                                   t->kind == ast::TypeDeclKind::Record);
    for (Symbol* m : t->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs) continue;
        // SEAM-ACCESSOR: `cs` is a member of `t`, drawn from forEachTypeAndNested's local-only
        // declaration tree (#3574 / #3152); a constructor-chain check has no
        // reason to run over an imported type's members.
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) continue;
        int chainCount = 0;
        bool misplacedChain = false;
        for (std::size_t i = 0; i < cd->body.size(); ++i) {
            if (!cd->body[i] || !isChainCall(cd->body[i].get())) continue;
            ++chainCount;
            if (i != 0) {
                misplacedChain = true;
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
        // fix #3793 (§22.10 / §12.8 / §22.8, v0.287): a `this(...)` chain
        // call must NOT resolve to the constructor that contains it.
        // §22.10 ("primary constructors" / "Constructors") states that a
        // `this(...)` chain "chains to another constructor in the same
        // class" — "another" excludes self; §12.8 gives the same rule from
        // the chain-call side. The codegen `ctorByArity` /
        // `ctorForArgs` already takes an `exclude` carve-out for `cs` so a
        // self-resolved chain call does not infinitely recurse
        // (LlvmBackend_impl.hpp:12259 and fix #3777's widening), but the
        // SEMANTIC omission was silent: the call compiled clean, codegen
        // emitted nothing for a non-primary-ctor class, and the object
        // stayed calloc-zero with no diagnostic at any severity — silently
        // producing a wrong value.
        //
        // Apply the rule ONLY to types without a primary constructor:
        // when a primary exists (`typeHasPrimaryCtor`, #4999), the §22.10
        // primary arm in `emitChainCall` (LlvmBackend_Setup.cpp:12189,
        // fix #1087) routes the call to the primary — which has no
        // ConstructorSymbol — rather than `cs`, so a self-arithmetic
        // match is never actually reached. In a non-primary class the
        // primary arm is absent and the chain would resolve to `cs` itself,
        // which is the defect.
        //
        // Detection: a leading `this(...)` chain call whose arity matches
        // `cs->params.size()` and matches NO OTHER non-static
        // ConstructorSymbol of the class. Static constructors are exempt
        // (no chain-call position). Constructors whose chain call is
        // already diagnosed as misplaced/duplicated are skipped — the §12.8
        // diagnostic above already names the defect, and a second one over
        // the same statement would only add noise.
        if (!typeHasPrimaryCtor && !cs->isStatic &&
            !misplacedChain && chainCount <= 1 &&
            !cd->body.empty() && cd->body[0]) {
            bool selfLeadingIsThis = false;
            const ast::CallExpr* selfChain =
                ast::chainCallOfStmt(*cd->body[0], selfLeadingIsThis);
            if (selfChain && selfLeadingIsThis) {
                const std::size_t n = selfChain->args.size();
                if (n == cs->params.size()) {
                    // Look for any OTHER non-static ConstructorSymbol of
                    // this class whose arity matches the chain call —
                    // if found, the chain resolves to that constructor,
                    // not to cs itself.
                    bool siblingMatch = false;
                    for (Symbol* m2 : t->members) {
                        auto* other = dynamic_cast<ConstructorSymbol*>(m2);
                        if (!other || other->isStatic || other == cs) continue;
                        if (other->params.size() == n) {
                            siblingMatch = true;
                            break;
                        }
                    }
                    // narrow #4791 (§12.8 / §22.5, v0.307): a named
                    // argument that names no parameter of `cs` means §22.5
                    // binding cannot bind the call to `cs` at all, so the
                    // arity match is not evidence of a self-chain. Defer
                    // exactly those calls — `deferredNamedThisChain` is the
                    // same predicate ExprTC reads — to
                    // `ExprTC::checkDeferredNamedThisChain`, which decides
                    // them with the argument types in hand and rejects every
                    // one it cannot prove binds a single other constructor.
                    if (!siblingMatch &&
                        !deferredNamedThisChain(*t, *cs, *cd)) {
                        ctx_.diagnostics().report(
                            Severity::Error, selfChain->span.begin,
                            std::string("`this(...)` constructor chain call in `") +
                            std::string(t->name) +
                            "` chains to the constructor itself; a "
                            "constructor cannot chain to its own "
                            "constructor (§22.10)");
                    }
                }
            }
        }
        // §22.10 / §22.7: the convenience-constructor chaining rule. A
        // static constructor has no chaining position (§22.7) and is
        // exempt. A constructor whose chain call is already diagnosed as
        // misplaced or duplicated is left alone — the §12.8 diagnostic
        // above already names the defect, and a second one over the same
        // statement would only add noise.
        if (!classHasPrimaryCtor || cs->isStatic) continue;
        if (misplacedChain || chainCount > 1) continue;
        bool leadingIsThis = false;
        const bool hasLeadingChain =
            !cd->body.empty() && cd->body[0] &&
            isChainCall(cd->body[0].get(), &leadingIsThis);
        if (hasLeadingChain && leadingIsThis) continue;  // conforming
        const SourceLocation where =
            hasLeadingChain ? cd->body[0]->span.begin : cd->span.begin;
        ctx_.diagnostics().report(
            Severity::Error, where,
            std::string("constructor of class `") + std::string(t->name) +
            "`, which has a primary constructor, must chain to another "
            "constructor via `this(...)`" +
            (hasLeadingChain
                 ? "; only the primary constructor may chain to `base(...)`"
                 : "") +
            " (§22.10)");
    }
    // fix #4839 (§11.1, v0.307): "The body may not redeclare a constructor
    // with the same parameter list as the primary constructor."
    //
    // fix-forward #5132 (§11.1 / §9.3 / §22.6, v0.308): the v0.307 check
    // gated the rejection on identical parameter names AND default
    // presence AND types AND §22.6 modifier. The v0.308 ruling (per
    // #4775's Q1/Q3/Q4 close) is that "same parameter list" is the §11.3
    // clash sense — i.e. §22.6's overload identity (arity, parameter
    // types, `ref`/`out` modifiers, and ordinary `owned` on a participating
    // position; parameter NAMES and DEFAULTS do not distinguish). So a
    // body constructor differing from the primary only in name (e.g.
    // `class Base(owned r: Res)` with `constructor(owned x: Res)`) or only
    // by default presence now also clashes. This check applies to classes
    // only — the record-side arm lives in `checkRecordExplicitCtorClash`
    // (src/sema/TypeResolution.cpp).
    if (!classHasPrimaryCtor) return;
    // imported-symbol seam (#3152, audit §4): the primary's parameters are
    // read off the §9.1/§22.10 positional FieldSymbols (`posParam`) and each
    // body constructor's off its ParameterSymbols — no TypeDecl or
    // ConstructorDecl downcast.
    const std::vector<const Symbol*> primary = recordPositionalFieldSymbols(t);
    if (primary.empty()) return;
    for (Symbol* m : t->members) {
        auto* cs = dynamic_cast<ConstructorSymbol*>(m);
        if (!cs || cs->isStatic) continue;
        if (cs->params.size() != primary.size()) continue;
        bool listsMatch = true;
        for (std::size_t i = 0; i < cs->params.size(); ++i) {
            auto* ps = dynamic_cast<ParameterSymbol*>(cs->params[i]);
            auto* pfs = dynamic_cast<const FieldSymbol*>(primary[i]);
            if (!ps || !pfs || !pfs->posParam) { listsMatch = false; break; }
            const ast::Param& pf = *pfs->posParam;
            // §22.6 overload identity: arity, parameter types, `ref`/`out`
            // modifiers, ordinary `owned`. Parameter NAMES and DEFAULTS do
            // not participate (§9.3 / §11.1 / §22.6, v0.308).
            TypeRef* paramType =
                ps->declType ? ctx_.resolvedType(*ps->declType) : nullptr;
            TypeRef* primaryType =
                pf.type ? ctx_.resolvedType(*pf.type) : nullptr;
            if (!typeRefEqual(paramType, primaryType)) {
                listsMatch = false;
                break;
            }
            // Ordinary vs owned is part of the §22.6 identity; scoped and
            // out are never distinguishable (the parameter list at the
            // primary's positional-field side carries no `ref`/`out` —
            // those would be `ref`/`out` parameters, which would be a
            // different overload kind, not a clash under this rule).
            if (overloadModifier(ps->modifier) !=
                overloadModifier(pf.modifier)) {
                listsMatch = false;
                break;
            }
        }
        if (listsMatch) {
            ctx_.diagnostics().report(
                Severity::Error, cs->declLoc,
                std::string("body constructor redeclares the primary ")
                    .append("constructor: it has the same parameter list ")
                    .append("as the primary constructor (§11.1, v0.308)"));
        }
    }
}

void Resolver::checkAsyncModifiers() {
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypeAsyncModifiers(t); });
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
            // SEAM-ACCESSOR: `ms` is a member of the locally-declared `t` (forEachTypeAndNested,
            // #3574) — this async-modifier check is a declaration-site check,
            // never reached for an imported member (#3152).
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
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypeStaticNotVirtual(t); });
}

// §11.5 (v0.307, #4719): slot assignment for the canonical modifier
// sequence
//   visibility static (async|pure) (abstract|virtual|override|sealed override)
//                (owned|scoped) readonly member-kind Name ...
// Visibility is slot 1 and is consumed before the modifier prefix
// (`Parser::parseMember` lines ~1138-1198) so it never appears in
// `MemberSymbol::modifiers`; `static` is slot 2, etc. Returns -1 for
// keywords outside §11.5 (e.g. `closed`, a class-level modifier that the
// parser admits into the shared member-modifier prefix but is checked
// separately by checkClassModifierCombinations). The walk skips those
// rather than treating them as slot 1; ordering them against §11.5
// modifiers is not the concern of this check.
namespace {
int memberModifierSlot(std::string_view mod) {
    if (mod == "static") return 2;
    if (mod == "async" || mod == "pure") return 3;
    if (mod == "abstract" || mod == "virtual" ||
        mod == "override" || mod == "sealed") return 4;
    if (mod == "owned" || mod == "scoped") return 5;
    if (mod == "readonly") return 6;
    return -1;
}
}  // namespace

void Resolver::checkMemberModifierOrder() {
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested(
        [&](TypeSymbol* t) { checkTypeMemberModifierOrder(t); });
}

void Resolver::checkTypeMemberModifierOrder(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* mem = dynamic_cast<MemberSymbol*>(m);
        if (!mem) continue;
        // Nested types are passed through `parseTypeOrInterfaceDecl`; their
        // own type-level modifiers are checked by
        // `checkTypeModifierCombinations` and the §11.5 member list is
        // not their concern.
        if (dynamic_cast<NestedTypeSymbol*>(m)) continue;
        // Visibility is parsed before the modifier prefix (slot 1), so
        // seed `prevSlot` at 1; the first member-modifier slot encountered
        // is always >= 2 and therefore non-decreasing.
        int prevSlot = 1;
        for (std::string_view mod : mem->modifiers) {
            const int slot = memberModifierSlot(mod);
            if (slot == -1) continue;
            if (slot < prevSlot) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("member `") + std::string(mem->name) +
                    "` declares modifier `" + std::string(mod) +
                    "` out of order; the canonical §11.5 modifier order "
                    "is `static (async|pure) (abstract|virtual|override|"
                    "sealed override) (owned|scoped) readonly`");
                break;
            }
            prevSlot = slot;
        }
    }
}

// §9.2 (v0.297): reject `virtual`, `abstract`, and `sealed override` on
// record members, and `override` on any record member other than the three
// Object-slot names `Equals` / `GetHashCode` / `ToString`. Records are
// implicitly sealed (§9.6) and have no virtual-dispatch surface, so the
// modifiers carry no meaning; `override` is admitted on the three Object-slot
// names because those names fill the `Object` slot (§10.5's admission for
// enums applies here verbatim, the spec's own footnote) and "imply no chain"
// — see §9.2 line 1376. The check is at the declaration per §9.2:
// record members are always dispatched statically, and a load-bearing-looking
// modifier that is inert would mislead every reader of the declaration.
void Resolver::checkRecordMemberModifiers() {
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind != ast::TypeDeclKind::Record) return;
        for (Symbol* m : t->members) {
            auto* mem = dynamic_cast<MemberSymbol*>(m);
            if (!mem) continue;
            // The rule covers method, property, and indexer members — §9.2
            // line 1376: "what its kind: method, property, or indexer".
            if (!dynamic_cast<MethodSymbol*>(m) &&
                !dynamic_cast<PropertySymbol*>(m)) {
                continue;
            }
            const bool hasVirtual = hasMod(mem, "virtual");
            const bool hasAbstract = hasMod(mem, "abstract");
            const bool hasOverride = hasMod(mem, "override");
            const bool hasSealed = hasMod(mem, "sealed");
            if (hasVirtual) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record member `") + std::string(mem->name) +
                    "` may not be declared `virtual` — record members are "
                    "always dispatched statically (§9.2)");
            }
            if (hasAbstract) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record member `") + std::string(mem->name) +
                    "` may not be declared `abstract` — record members are "
                    "always dispatched statically (§9.2)");
            }
            if (hasSealed && hasOverride) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record member `") + std::string(mem->name) +
                    "` may not be declared `sealed override` — record "
                    "members are always dispatched statically (§9.2)");
            }
            if (hasOverride && !hasSealed &&
                mem->name != "Equals" && mem->name != "GetHashCode" &&
                mem->name != "ToString") {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record member `") + std::string(mem->name) +
                    "` may not be declared `override` — `override` is "
                    "admitted on a record only on the Object-slot names "
                    "`Equals`, `GetHashCode`, or `ToString` (§9.2)");
            }
        }
    });
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

// §9.2 / fix #4643 (v0.298): reject `equality skip` on a static member.
// Spec §9.2 line 1419 — "On a computed (expression-bodied) property, a
// method, a static member, an enum constant, or a choice case (the case
// is the discriminant — skipping it is incoherent), `equality skip` is a
// compile error, mirroring §9.8's inert-annotation rule." The other
// inert-annotation arms (computed property / method / enum constant /
// choice case) are caught at parse time — the parser does not admit
// `equality` at those grammar positions, so the user gets the parser's
// own "expected ..." diagnostic. Only the static-member arm of the rule
// needs sema: a static FIELD is a fully valid declaration, the parser
// must accept it (its grammar admits the clause on FieldDecl), and
// dropping the clause silently would be the inert-looking-but-load-
// bearing failure mode spec §18 warns against.
//
// Mirroring §9.8's existing static-skip discipline (§9.8 line 1419
// itself cites §9.8), the diagnostic carries the clause's spelling so
// the user can grep for it: the substring `equality skip` is the load-
// bearing identifier the lane's regression test pins against.
void Resolver::checkEqualitySkipInert() {
    forEachTypeAndNested(
        [&](TypeSymbol* t) { checkTypeEqualitySkipInert(t); });
}

void Resolver::checkTypeEqualitySkipInert(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        // The grammar admits `equality skip` on the storage-bearing sites per
        // §9.2 line 1385; the static-member arm is the inert-annotation case
        // from §9.2 line 1419.
        //
        // Asked of the SYMBOL, not of `Symbol::decl` (umbrella #3152): the
        // clause now rides `MemberSymbol::equalitySkip`, which SymbolCollection
        // fills from the three AST hosts and ModuleLoader fills from
        // `MetaMember::equalitySkip`. The previous spelling cast `fs->decl` /
        // `ps->decl` to a FieldDecl/PropertyDecl, which also meant a POSITIONAL
        // field (whose `decl` is the owning TypeDecl) was never examined at
        // all. The walk itself is local-only by construction —
        // `forEachTypeAndNested` iterates `ctx_.module()->children`, which
        // holds only the types THIS compilation declares — so no diagnostic is
        // re-reported against a dependency's declarations.
        auto* mem = dynamic_cast<MemberSymbol*>(m);
        if (!mem || !memberEqualitySkip(mem)) continue;
        if (!hasMod(mem, "static")) continue;
        const char* kindWord =
            dynamic_cast<PropertySymbol*>(m) ? "property" : "field";
        ctx_.diagnostics().report(
            Severity::Error, m->declLoc,
            std::string("static ") + kindWord + " `" +
            std::string(m->name) +
            "` may not carry the `equality skip` clause — "
            "static members never participate in equality "
            "(§9.2 inert-annotation rule, mirroring §9.8; "
            "the clause is meaningless on a static and is "
            "rejected to avoid an inert-looking-but-load-"
            "bearing annotation, §18)");
    }
}

void Resolver::checkMethodBodies() {
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypeMethodBodies(t); });
}

void Resolver::checkTypeMethodBodies(TypeSymbol* t) {
    bool isInterface = t->kind == ast::TypeDeclKind::Interface;
    for (Symbol* m : t->members) {
        auto* ms = dynamic_cast<MethodSymbol*>(m);
        if (ms) {
            // SEAM-ACCESSOR: `ms` is a member of the locally-declared `t` (forEachTypeAndNested,
            // #3574); a method-body-kind check is declaration-site only (#3152).
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
            continue;
        }

        // fix #4101 (§12.5/§23.4, v0.295): the indexer analogue of the
        // method-body-presence rule just above. §23.4: "Indexers ...
        // participate in virtual/override/sealed/abstract like methods."
        // An indexer never gets automatic backing storage the way a plain
        // named auto-property does (§23.2's Auto form has a compiler-
        // synthesized field to read/write; an indexer's Auto form has no
        // per-instance storage an index expression could address), so its
        // Auto-bodied shape (no accessors, no expression body --
        // `public property [i: int]: int;`) is legal ONLY as the §12.5
        // abstract shorthand. `isAbstractPropertyShorthand`
        // (sema/Symbol.hpp) explicitly EXCLUDES indexers by design (its
        // own `p.isIndexer` guard, documented there as "a separate
        // mechanism ... handles the indexer abstract shorthand"), so this
        // rule cannot reuse it and instead reads the same two AST facts
        // (bodyKind, the `abstract` modifier) the method arm above reads.
        // Interface indexer signatures are the interface's WHOLE contract
        // (§23.4: "Indexers may be declared in interfaces with the
        // body-less form") and stay excluded exactly like the interface
        // method arm's `continue` above, but only on the ACCEPT side --
        // unlike §11.2 for methods, §23.4 does not forbid an interface
        // indexer from carrying a body, so mirroring that half of the
        // method rule here would be a new, unrelated rejection outside
        // this issue's scope (left to a separate issue if it is ever
        // measured to be live).
        //
        // Without this check, `public property [i: int]: int;` (no
        // accessors, no `abstract`) compiled clean and reached codegen,
        // where `ensureIndexerGetterFn` correctly declines to synthesize
        // a getter for it (it has no Auto branch at all) and the caller
        // silently degraded to a zero value through #2434's
        // degrade-disposition fallback -- an "internal:" stderr note at
        // runtime instead of a compile-time rejection naming the rule.
        auto* ps = dynamic_cast<PropertySymbol*>(m);
        if (!ps || !ps->isIndexer || isInterface) continue;
        // fix #4111 (§10.5/§35.1, v0.298): an indexer on an enum or choice
        // body is already unconditionally rejected regardless of body
        // presence — checkEnumRestrictions's and checkChoiceRestrictions's
        // own `PropertySymbol` arms both `bad("an indexer")`/continue on
        // `ps->isIndexer` before ever looking at body kind. MEASURED on
        // this HEAD (pre-fix): a BARE (bodyless, non-abstract) indexer on
        // either kind tripped BOTH that rejection AND this arm's
        // "non-abstract indexer must have a body" — two diagnostics, same
        // location, for one member (`enum Color ... public property [i:
        // int]: int; end Color;` reported both "enum `Color` may not
        // declare an indexer (§10.5)" and "non-abstract indexer must have a
        // body (§12.5/§23.4)"; the choice shape reported the analogous
        // pair). Both are individually correct — no false accept, no false
        // reject — but this codebase has explicit precedent for treating
        // that as a defect anyway: #3601's changelog entry records
        // retiring an earlier duplicate enum loop specifically "to avoid
        // double-reporting". Short-circuit here rather than weaken either
        // membership rejection — this function's OWN body-presence rule
        // has nothing further to add once the member kind itself is
        // rejected outright.
        if (t->kind == ast::TypeDeclKind::Enum ||
            t->kind == ast::TypeDeclKind::Choice)
            continue;
        // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t`
        // (forEachTypeAndNested, #3574); same declaration-site-only
        // reasoning as the method arm above (#3152).
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd) continue;
        bool noBody = pd->bodyKind == ast::PropertyBodyKind::Auto;
        bool isAbstract = hasMod(ps, "abstract");
        if (!isAbstract && noBody) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                "non-abstract indexer must have a body (§12.5/§23.4)");
        }
    }
}

void Resolver::checkAbstractPropertyInitializers() {
    // fix #4026 (§12.5, v0.295): shared recursive walk (§11.6, v0.286),
    // matching every other per-member modifier-combination check in this
    // file.
    forEachTypeAndNested(
        [&](TypeSymbol* t) { checkTypeAbstractPropertyInitializers(t); });
}

void Resolver::checkTypeAbstractPropertyInitializers(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* ps = dynamic_cast<PropertySymbol*>(m);
        if (!ps) continue;
        // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t`
        // (forEachTypeAndNested, #3574); an abstract-shorthand-initializer
        // check is declaration-site only (#3152), matching
        // checkTypeMethodBodies just above.
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd) continue;
        // §12.5: the shared predicate (sema/Symbol.hpp's
        // `isAbstractPropertyShorthand`) already excludes indexers by
        // design (a separate mechanism handles the indexer abstract
        // shorthand, #4037) and non-Auto bodies (an abstract property with
        // explicit `get;`/`set;` accessors is Block-bodied and carries no
        // `autoInitializer` slot at all — the grammar has nowhere to put
        // one there). Only a plain `abstract property Name: T := expr;`
        // reaches this branch.
        // fix #4100 (§12.5, v0.300): the named-property check below uses
        // `isAbstractPropertyShorthand`, which explicitly excludes indexers
        // (SymbolCollection.cpp: the `p.isIndexer` guard). Add the missing
        // indexer branch here: an abstract Auto-shorthand indexer with an
        // autoInitializer is the same defect as the named-property case
        // (#4026) and requires the same diagnostic.
        if (pd->isIndexer && hasMod(ps, "abstract") &&
            pd->bodyKind == ast::PropertyBodyKind::Auto && pd->autoInitializer) {
            ctx_.diagnostics().report(
                Severity::Error, pd->autoInitializer->span.begin,
                "abstract indexer may not declare an initializer — an abstract "
                "member has no backing storage for it to write into, so the "
                "initializer could never run (§12.5)");
            continue;
        }
        if (!isAbstractPropertyShorthand(*pd)) continue;
        if (!pd->autoInitializer) continue;
        ctx_.diagnostics().report(
            Severity::Error, pd->autoInitializer->span.begin,
            std::string("abstract property `") + std::string(ps->name) +
            "` may not declare an initializer — an abstract member has no "
            "backing storage for it to write into, so the initializer "
            "could never run (§12.5)");
    }
}

void Resolver::checkAbstractPropertyBodies() {
    // fix #4110 (§12.5/§23.4, v0.298): shared recursive walk (§11.6,
    // v0.286), matching every other per-member modifier-combination check
    // in this file.
    forEachTypeAndNested(
        [&](TypeSymbol* t) { checkTypeAbstractPropertyBodies(t); });
}

void Resolver::checkTypeAbstractPropertyBodies(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* ps = dynamic_cast<PropertySymbol*>(m);
        if (!ps) continue;
        // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t`
        // (forEachTypeAndNested, #3574); an abstract-member-body check is
        // declaration-site only (#3152), matching
        // checkTypeAbstractPropertyInitializers just above.
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd) continue;
        if (!hasMod(ps, "abstract")) continue;
        // §12.5 / §23.4: an `abstract` member has no backing storage and
        // no reachable call site of its own (dispatch through a base-typed
        // reference always resolves to the most-derived override, §12.1),
        // so a body it declares can never run. A `PropertyBodyKind::Auto`
        // body is out of scope here entirely — checkAbstractPropertyInitializers
        // (named properties, via `isAbstractPropertyShorthand`) and the
        // indexer body-presence branch of checkTypeMethodBodies (#4101)
        // already cover the Auto-body shapes; this check only ever fires
        // for `Expression` or `Block`.
        bool hasRealBody = false;
        if (pd->bodyKind == ast::PropertyBodyKind::Expression) {
            // `=> expr;` always carries real, unreachable code.
            hasRealBody = true;
        } else if (pd->bodyKind == ast::PropertyBodyKind::Block) {
            // The Block form is ALSO the legitimate way to spell an
            // abstract member needing more than the Auto shorthand's
            // implicit get-only accessor (e.g. both `get;` and `set;`
            // stubs, #4026's own accepted control
            // `abstract_block_form_accessors_accepted_control_spec_12_5`)
            // — every accessor there is itself
            // `PropertyAccessorBodyKind::Abstract` (the bare `;` form).
            // Only an accessor carrying REAL code (`Expression`/`Block`)
            // is the #4110 defect.
            for (const auto& acc : pd->accessors) {
                if (acc.bodyKind != ast::PropertyAccessorBodyKind::Abstract) {
                    hasRealBody = true;
                    break;
                }
            }
        }
        if (!hasRealBody) continue;
        if (pd->isIndexer) {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                "abstract indexer may not have a body — an abstract member "
                "has no backing storage and no reachable call site of its "
                "own, so the body could never run (\xC2\xA7"
                "12.5/\xC2\xA7""23.4)");
        } else {
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("abstract property `") + std::string(ps->name) +
                "` may not have a body — an abstract member has no backing "
                "storage and no reachable call site of its own, so the "
                "body could never run (\xC2\xA7""12.5)");
        }
    }
}

// §10.5 / §35.2 (v0.288, #3837): true iff `ms` is one of the three
// `Object`-virtual members whose replacement §10.5 admits spelling with
// `override` — `Equals` (arity 1: the co-declared `Equals(T)` §25.2
// requires; whether `override` reaches `Equals(Object?)` as a distinct
// declaration follows the record rule and is explicitly left open by
// #3837, so both arity-1 spellings are admitted alike here), `GetHashCode`
// (arity 0), and `ToString` (arity 0). Matched by name and arity, not
// exact parameter type — the same discipline checkEqualityCoDeclaration
// below already uses to recognize the co-declared `Equals`/`GetHashCode`
// pair (#1338/#3333). Not reused directly from there: that check's
// membership test omits `ToString` (outside the co-declaration
// obligation) and is inlined into a loop with a different shape, so
// sharing it would mean threading a "which members" parameter through an
// unrelated loop for one caller — a new, narrower predicate is the
// smaller change.
static bool isObjectVirtualReplacementMember(const MethodSymbol* ms) {
    if (!ms || ms->isOperator) return false;
    if (ms->name == "GetHashCode" || ms->name == "ToString")
        return ms->params.empty();
    if (ms->name == "Equals") return ms->params.size() == 1;
    return false;
}

void Resolver::checkEnumRestrictions() {
    if (!ctx_.module()) return;
    // fix #3601 (§10.4 / §10.5 / §11.6, v0.286): the two module-children-only
    // loops this replaced used to reach only MODULE-LEVEL enums (§10.5's
    // restriction set) or exactly one level of nesting (fix #3044's
    // `FromUnderlyingOrNull` carve-out) -- so a nested enum escaped §10.5
    // entirely (depth 1) and an enum nested two levels down escaped even the
    // carve-out (depth 2, issue 3601's second measured axis). Routing the
    // whole check through the shared recursive walker (fix #3574,
    // forEachTypeAndNested) closes both gaps together: every enum at every
    // nesting depth now gets the FULL §10.5 restriction set, including the
    // `FromUnderlyingOrNull` carve-out below -- so the old separate
    // module-children-only `FromUnderlyingOrNull` loop is retired outright
    // rather than kept alongside this one, which would double-report a
    // module-level enum's violation (this loop already covers depth 0).
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind != ast::TypeDeclKind::Enum) return;
        for (Symbol* m : t->members) {
            auto bad = [&](std::string_view what) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("enum `") + std::string(t->name) +
                    "` may not declare " + std::string(what) +
                    " (§10.5)");
            };
            // fix #3044 (§10.4, v0.242): `FromUnderlyingOrNull` is the
            // compiler-provided per-enum static member — "no declaration is
            // written, and no user declaration may override or overload it".
            // Rejecting the declaration (rather than letting it silently lose
            // to the compiler-provided member at every call site) is what makes
            // the member's contract — total, never raises — trustworthy.
            if (m->name == "FromUnderlyingOrNull") {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("enum `") + std::string(t->name) +
                    "` may not declare a member named `FromUnderlyingOrNull` — "
                    "it is the compiler-provided static member every enum "
                    "carries, and no user declaration may override or overload "
                    "it (§10.4)");
                continue;
            }
            // fix #3862 (§10.4, v0.293): `FromNameOrNull`/`NameOf` are the
            // other two members of the compiler-provided tier — "Every enum
            // `E` … carries three compiler-provided static members …
            // FromUnderlyingOrNull, FromNameOrNull, NameOf … no user
            // declaration, a member or a declared constant, may override or
            // overload any of them." The check runs on `m` directly, before
            // any dynamic_cast, so it reaches both a user MEMBER (this case,
            // a redeclared static method) and a user CONSTANT of either name
            // — the same reach `FromUnderlyingOrNull`'s sibling arm above
            // already has. MEASURED on this HEAD (pre-fix): `enum Priority
            // Low, Medium, High; public static method FromNameOrNull(name:
            // string): Priority? => Low; end Priority;` compiled at exit 0,
            // zero diagnostics — this loop had no `FromNameOrNull`/`NameOf`
            // arm at all (only `FromUnderlyingOrNull`'s, fix #3044).
            if (m->name == "FromNameOrNull" || m->name == "NameOf") {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("enum `") + std::string(t->name) +
                    "` may not declare a member named `" +
                    std::string(m->name) + "` — it is the compiler-provided "
                    "static member every enum carries, and no user "
                    "declaration may override or overload it (§10.4)");
                continue;
            }
            // fix #4121 (§10.4, §8.7, v0.298): §10.4's positive list of what
            // an enum "may declare exactly" — instance methods, expression-
            // bodied read-only instance properties, static methods,
            // `static readonly` fields, `const` members, operator overloads —
            // has no destructor entry, and §8.7 states plainly that "Static
            // classes, records, enums, choices, primitives, and inline
            // arrays do not implement `IDisposable`" — so an enum has no
            // Dispose() surface for a destructor to hook. MEASURED on this
            // HEAD (pre-fix): `enum Color: int Red, Green, Blue; destructor
            // end destructor; end Color;` compiled at exit 0, zero
            // diagnostics — this loop had no DestructorSymbol arm at all.
            // checkChoiceRestrictions already rejects the identical shape on
            // choices (fix #3869, "choice `Shape` may not declare a
            // destructor (§35.1 / §35.2)"); the spec's own Appendix A
            // preamble "Shared-production over-admissions" index (the
            // unnumbered table immediately before A.1, NOT a numbered
            // "Appendix A.9" — no such subsection exists, A stops at A.8)
            // already names this exact enum-side gap in its `Member` in an
            // enum-or-choice body row: "...destructors by §8.7" — for BOTH
            // kinds. #3869 explicitly declined to touch the enum side (see
            // that fix's own comment on checkChoiceRestrictions below), so
            // choice was the one made correct then; this fix makes enum
            // agree with choice, not the other way — choice's existing
            // rejection is the one the spec supports.
            if (dynamic_cast<DestructorSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("enum `") + std::string(t->name) +
                    "` may not declare a destructor — an enum never "
                    "implements `IDisposable` (§8.7) and a destructor is "
                    "not part of §10.4's exhaustive admitted-member list");
                continue;
            }
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
                if (ps->isIndexer) { bad("an indexer"); continue; }
                // fix #2499 (§10.5): "Block-form properties or properties
                // with backing storage" are forbidden on an enum. §10.4
                // permits only the expression-bodied READ-ONLY form
                // (`public property Name: string => …;` — grammar A.4's
                // `ExpressionPropertyDecl`; the "=>" production is
                // inherently read-only, no setter form exists for it, so
                // PropertyBodyKind::Expression is always the legal §10.4
                // shape and never needs a settability check here). Any
                // other body kind is rejected regardless of whether it
                // declares a setter: a GETTER-ONLY block-form property
                // (`get => 7; end P;`) is still "block-form" and equally
                // forbidden by §10.5's unqualified wording, not just a
                // settable one.
                // SEAM-ACCESSOR: `ps` is a member of the locally-declared enum `t`
                // (forEachTypeAndNested, #3574) — this §10.5 property-form check never
                // sees an imported member (#3152).
                if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
                    if (pd->bodyKind == ast::PropertyBodyKind::Block)
                        bad("a block-form property");
                    else if (pd->bodyKind == ast::PropertyBodyKind::Auto)
                        bad("a property with backing storage");
                }
                continue;
            }
            // per §24 (v0.180): an event's compiler-generated `owned
            // EventSource` backing field requires an owning, disposable
            // container, which an enum is not (§9.7, Appendix C.3).
            if (dynamic_cast<EventSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("enum `") + std::string(t->name) +
                    "` may not declare an `event` member — an event's "
                    "compiler-generated `owned EventSource` backing field "
                    "requires an owning, disposable container, which an enum "
                    "is not (§24, §9.7)");
                continue;
            }
            if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                // fix #3837 (§10.5, v0.288): `override` is admitted on
                // exactly the `Object`-virtual replacement trio — every
                // other use of `override`, and `virtual`/`abstract`
                // regardless of name, are still rejected below.
                //
                // narrow #3837 (§10.5 verbatim, v0.293): "Enum methods may
                // not be declared `virtual`, `abstract`, or `sealed
                // override`" — `sealed override` is named as its OWN
                // rejected compound, distinct from plain `override`, and is
                // never admitted here even on the trio (§11.5 Modifier order:
                //  `sealed override` seals a previously-virtual member
                // against further overriding — meaningless on a statically-
                // dispatched enum method, trio or not). The `hasMod(ms,
                // "sealed")` guard below disqualifies admission rather than
                // adding an unconditional top-level reject: `sealed` alone
                // (no `override`) is a different, already-rejected shape
                // (checkTypeStaticNotVirtual's "may not be `sealed` on its
                // own" check, ~line 583) and gating it here too would
                // double-report that case.
                bool overrideAdmitted = hasMod(ms, "override") &&
                    !hasMod(ms, "sealed") &&
                    isObjectVirtualReplacementMember(ms);
                if (hasMod(ms, "virtual") || hasMod(ms, "abstract") ||
                    (hasMod(ms, "override") && !overrideAdmitted)) {
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
        // SEAM-ACCESSOR: `t` is drawn from forEachTypeAndNested (#3574), which walks only this
        // module's own declaration tree — never an imported type (#3152).
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
            // fix #5402 (§10.2, v0.315): build the set of "later" constant names
            // (current + after) for self/forward-reference detection.
            std::unordered_set<std::string_view> laterConstants;
            for (const auto& ec : td->enumConstants)
                laterConstants.insert(ec.name);

            for (const auto& ec : td->enumConstants) {
                // Remove current from laterConstants so only truly LATER constants remain.
                laterConstants.erase(ec.name);

                // Helper: walks an expression tree to detect a self- or forward
                // reference to the enum's own constants (§10.2, v0.315). A bare
                // name or qualified `E.A` that refers to the constant being
                // declared (self) or to one declared later (forward) is a compile
                // error. Returns true if an error was reported.
                std::function<bool(const ast::Expr&)> checkRef = [&](
                    const ast::Expr& e) -> bool {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
                        // Bare self/forward reference?
                        if (laterConstants.count(id->name) > 0) {
                            ctx_.diagnostics().report(
                                Severity::Error, e.span.begin,
                                "enum member `" + std::string(ec.name) +
                                "` initializer contains a forward reference to `" +
                                std::string(id->name) +
                                "`, which is declared later in the same enum "
                                "(§10.2 — a reference to a later enum member is "
                                "a compile error)");
                            return true;
                        }
                        if (id->name == ec.name) {
                            ctx_.diagnostics().report(
                                Severity::Error, e.span.begin,
                                "enum member `" + std::string(ec.name) +
                                "` initializer contains a self-reference to its "
                                "own value (§10.2 — a self-reference in an enum "
                                "member value initializer is a compile error)");
                            return true;
                        }
                        return false;
                    }
                    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
                        // Qualified reference `E.A` — check if base is this enum.
                        if (auto* baseType = ctx_.exprType(*ma->target)) {
                            if (baseType->declaration &&
                                baseType->declaration->decl == td) {
                                // Base refers to this enum. Is the member self/forward?
                                if (laterConstants.count(ma->member) > 0) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, e.span.begin,
                                        "enum member `" + std::string(ec.name) +
                                        "` initializer contains a forward reference "
                                        "to `." + std::string(ma->member) +
                                        "`, which is declared later in the same "
                                        "enum (§10.2)");
                                    return true;
                                }
                                if (ma->member == ec.name) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, e.span.begin,
                                        "enum member `" + std::string(ec.name) +
                                        "` initializer contains a self-reference "
                                        "to its own value (§10.2)");
                                    return true;
                                }
                            }
                        }
                        // Not a reference to this enum's constants: fall through
                        // to recurse on children.
                    }
                    // Recurse into child expressions.
                    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
                        if (b->left && checkRef(*b->left)) return true;
                        if (b->right && checkRef(*b->right)) return true;
                        return false;
                    }
                    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
                        if (u->operand && checkRef(*u->operand)) return true;
                        return false;
                    }
                    if (auto* c = dynamic_cast<const ast::CallExpr*>(&e)) {
                        for (const auto& arg : c->args) {
                            if (arg.value && checkRef(*arg.value)) return true;
                        }
                        return false;
                    }
                    if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
                        if (checkRef(*cond->condition)) return true;
                        if (checkRef(*cond->thenValue)) return true;
                        if (checkRef(*cond->elseValue)) return true;
                        return false;
                    }
                    return false;
                };

                long long val = running;
                bool haveVal = true;
                if (ec.value) {
                    // First: check for self/forward references before folding.
                    bool errorSeen = checkRef(*ec.value);

                    auto resolve = [&](std::string_view name, int)
                        -> std::optional<long long> {
                        auto it = siblings.find(name);
                        if (it != siblings.end()) return it->second;
                        return std::nullopt;
                    };

                    // Callback to resolve qualified `E.A` references in enum value
                    // initializers (§10.2, v0.315).
                    auto resolveMemberAccess =
                        [&](const ast::Expr& base,
                            std::string_view member,
                            bool& cbError) -> std::optional<long long> {
                        cbError = false;
                        // Try exprType first (set when base has typeArgs or is
                        // a member-access chain). If null (bare type identifier in
                        // expression position, e.g. `E` in `E.A` where E is an enum),
                        // fall back to checking if the base is an IdentifierExpr
                        // naming the current enum type (`t`).
                        TypeSymbol* baseTs = nullptr;
                        if (auto* baseType = ctx_.exprType(base)) {
                            baseTs = baseType->declaration;
                        } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&base)) {
                            // Bare type identifier: if it names the current enum, use t.
                            if (id->name == t->name) baseTs = t;
                        }
                        if (!baseTs || baseTs->decl != td)
                            return std::nullopt;  // not a qualifier ref to this enum
                        // Is the member an earlier sibling (valid)?
                        auto it = siblings.find(member);
                        if (it != siblings.end()) return it->second;
                        // Is it the current constant (self-reference)?
                        if (member == ec.name) {
                            cbError = true;
                            return std::nullopt;
                        }
                        // Is it a later constant (forward reference)?
                        // Rebuild the "later" set: constants after ec in the enum.
                        // We need to check against the ORIGINAL laterConstants (which
                        // includes current). Since we removed ec.name from
                        // laterConstants at loop top, a name in laterConstants
                        // is definitely a forward reference.
                        if (laterConstants.count(member) > 0) {
                            cbError = true;
                            return std::nullopt;
                        }
                        return std::nullopt;  // not a const of this enum
                    };

                    bool foldErrorSeen = false;
                    if (auto folded = foldIntConst(
                            *ec.value, resolve, 0, resolveMemberAccess))
                        val = *folded;
                    else
                        // non-constant — a separate concern. §6.5/§17.14
                        // (v0.242, #3045): an enum member whose value is a
                        // constant shift with an out-of-domain count also
                        // lands here (foldIntConst declines a faulting
                        // expression), and it is NOT silently skipped: the
                        // value expression is visited by ExprTC, whose
                        // checkShiftCountDomain reports it at the shift with
                        // the operand's real type. Adding a probe here would
                        // only double the diagnostic.
                        haveVal = false;
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
    });
}

// §35.2 (v0.288, #3837): "Choice methods are statically dispatched; `virtual`
// and `abstract` are rejected as on enums (§10.5), `override` is admitted
// exactly where §10.5 admits it — the `Object`-virtual surface, spelled
// `override` or plain — and §10.5's `Equals`/`GetHashCode`/`ToString`
// replacement rule carries over for the boxable classifications." Before
// this fix, NOTHING rejected `virtual`/`abstract`/`override` on a choice
// method at all — MEASURED: a choice declaring `public virtual method
// Area(): double => …;` compiled with zero diagnostics on pre-fix HEAD,
// because checkEnumRestrictions (above) is gated to `TypeDeclKind::Enum`
// only and the general base-class override-match walk
// (checkHidingAndOverride) never populates `baseMembers` for a choice — a
// choice's `baseTypes` holds only `Interface`-kind entries, never `Class`,
// so that walk's frontier is empty and it early-outs. This is therefore a
// NEW check, not a widening of an existing one; it deliberately mirrors
// only checkEnumRestrictions's dispatch-modifier sub-check, not its other
// enum-only restrictions (instance fields / constructors / nested types /
// indexers) — those choice-side admissible-kind gaps are a separate,
// pre-existing defect, out of this fix's scope (issue #3869).
void Resolver::checkChoiceDispatchModifiers() {
    if (!ctx_.module()) return;
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind != ast::TypeDeclKind::Choice) return;
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms) continue;
            // narrow #3837 (§35.2, v0.293): §35.2 carries §10.5's rejection
            // set over "as on enums", and §10.5 names `sealed override` as
            // its OWN rejected compound, distinct from plain `override` —
            // never admitted here even on the trio. See the matching
            // `sealed` guard in checkEnumRestrictions above for the reason
            // this disqualifies admission rather than adding an
            // unconditional top-level reject (avoids double-reporting bare
            // `sealed`, already caught by checkTypeStaticNotVirtual).
            bool overrideAdmitted = hasMod(ms, "override") &&
                !hasMod(ms, "sealed") &&
                isObjectVirtualReplacementMember(ms);
            if (hasMod(ms, "virtual") || hasMod(ms, "abstract") ||
                (hasMod(ms, "override") && !overrideAdmitted)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("choice method `") + std::string(ms->name) +
                    "` may not be virtual, abstract, or override "
                    "— choices dispatch statically (§35.2)");
            }
        }
    });
}

// fix #3869 (§35.1 / §35.2, v0.295): §35.1 states plainly "Choices cannot
// have destructors, constructors, instance fields, block-form or
// storage-backed properties, indexers, or nested types — the same
// restriction set as enums (§10.5), for the same reasons, plus one: a
// choice's instance storage IS its cases' payloads, declared in the case
// list and reachable only through matching." §35.2 independently
// confirms the admitted-member list ("instance methods, expression-bodied
// read-only instance properties, static methods, `static readonly`
// fields..., `const` members, and operator overloads ... this list rejects
// the rest") and separately: "A choice has no user constructors, instance
// fields, or storage-backed properties by design."
//
// Before this fix NOTHING enforced any of this at sema — measured on
// origin/main `7561e19`: a choice body declaring an instance field, a
// constructor, a nested type, an indexer, a block-form or storage-backed
// (Auto) property, an `event` member, or a destructor compiled at exit 0
// with zero diagnostics, for every one of those eight forms (verified live
// with a probe per form before writing this check, not assumed from the
// spec text alone).
//
// Mirrors checkEnumRestrictions's non-dispatch-modifier member-kind checks
// (above) rather than inventing a second spelling of the same decision —
// same predicates (hasMod, PropertyBodyKind), same message shape, "choice"
// substituted for "enum". Two DELIBERATE differences from the enum mirror,
// both because §35.1 names a strictly larger reject set than §10.5's enum
// list:
//   - `destructor` is ALSO rejected here (§35.1 names it explicitly, cited
//     under §8.7 by the spec's own Appendix A "Shared-production
//     over-admissions" index — an unnumbered table in the Appendix A
//     preamble, before A.1 — in its `Member` in an enum-or-choice body
//     row); checkEnumRestrictions has NO destructor check at all (a
//     separate, pre-existing enum-side gap this fix does not touch —
//     filed alongside #3840's other enum/choice over-admission catalog).
//   - the `event` rejection is worded from §35.2's admitted-list boundary
//     ("not part of" the list), not enum's/record's §24/§9.7 "not an
//     owning disposable container" reasoning — an OWNING choice IS a real
//     disposal-obligated container (§35.4 rule 1), so that container-based
//     claim would be FALSE for it; the admitted-list boundary holds
//     regardless of classification.
//
// Deliberately NOT covered here (same scope line checkChoiceDispatchModifiers
// draws, issue #3840's own catalog): a `static` field/property/constructor
// MODIFIER-correctness gap identical to enum's (a mutable or non-`readonly`
// static field, a static constructor, and a `static` property of any body
// kind — including Expression — are still admitted here exactly as
// checkEnumRestrictions itself still admits them for enums; #3840 tracks
// fixing BOTH sides together, not this fix, which only closes the
// admissible-KIND gap #3869 describes). An `invariant` member is likewise
// NOT covered here — `t->members` never carries one (it is a distinct
// AST/side-table construct, `TypeSymbol::invariantContracts`, not a
// `Symbol` subclass at all), so neither this walk nor checkEnumRestrictions's
// can see it; that gap (named in the spec's own Appendix A preamble's
// "Shared-production over-admissions" index — the unnumbered table before
// A.1 — in its `Member` in an enum-or-choice body row's "invariant by
// §22.16" clause) was left uncovered for BOTH enum and choice bodies by
// this fix — closed separately by fix #4115's checkEnumChoiceInvariantMembers
// below, which reads the raw AST member list instead of `t->members`.
void Resolver::checkChoiceRestrictions() {
    if (!ctx_.module()) return;
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind != ast::TypeDeclKind::Choice) return;
        for (Symbol* m : t->members) {
            auto bad = [&](std::string_view what) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("choice `") + std::string(t->name) +
                    "` may not declare " + std::string(what) +
                    " (§35.1 / §35.2)");
            };
            if (dynamic_cast<DestructorSymbol*>(m)) {
                bad("a destructor");
                continue;
            }
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
                if (ps->isIndexer) { bad("an indexer"); continue; }
                // SEAM-ACCESSOR: `ps` is a member of the locally-declared
                // choice `t` (forEachTypeAndNested, #3574) — this §35.2
                // property-form check never sees an imported member
                // (#3152), mirroring checkEnumRestrictions's identical
                // comment on the same pattern.
                if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
                    if (pd->bodyKind == ast::PropertyBodyKind::Block)
                        bad("a block-form property");
                    else if (pd->bodyKind == ast::PropertyBodyKind::Auto)
                        bad("a property with backing storage");
                }
                continue;
            }
            if (dynamic_cast<EventSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("choice `") + std::string(t->name) +
                    "` may not declare an `event` member — §35.2's "
                    "admitted-member list has no `event` entry (instance "
                    "methods, expression-bodied read-only instance "
                    "properties, static methods, `static readonly` fields, "
                    "`const` members, and operator overloads only)");
                continue;
            }
        }
    });
}

// fix #4115 (§22.16, Appendix A preamble "Shared-production over-admissions"
// index, v0.298): an `invariant` member on an enum or choice body compiles
// with zero diagnostics — MEASURED on this HEAD (pre-fix): both
//   choice Shape Circle(radius: double), Square(side: double); invariant(true); end Shape;
// and
//   enum Color Red, Green, Blue; invariant(true); end Color;
// compile at exit 0, zero diagnostics. Neither checkEnumRestrictions nor
// checkChoiceRestrictions (above) can see this: `invariant` clauses are not
// modeled as a `Symbol` subclass in `TypeSymbol::members` at all —
// SymbolCollection never creates one for `ast::InvariantDecl` — they live
// only in `TypeSymbol::invariantContracts`, a side-table `ModuleLoader`
// populates for an IMPORTED class from the exporting module's already-
// validated metadata (ModuleLoader.cpp), never for a local declaration
// (Symbol.hpp's own comment on the field: "Local classes derive these from
// their AST; imported classes have no AST, so the loader publishes this
// side-table vector"). A LOCAL enum or choice's invariant member is
// therefore readable only from the raw AST: `t->decl`'s
// `ast::TypeDecl::members` (populated because the parser's EnumMember*/
// ChoiceMember* productions both route through the shared `parseMember()`
// used for class bodies — Parser_Decls.cpp's own comment there: "We accept
// the full Member grammar at the parse level and let sema filter" — so an
// `ast::InvariantDecl` parses into an enum/choice body exactly as it does
// into a class body; nothing at the grammar layer excludes it).
//
// Citation note: the issue that reported this (#4115) titled itself
// "Appendix A.9" for the admissions-index table; VERIFIED against
// docs/Bezel_Language_Spec.md that no such subsection exists — Appendix A's
// numbered subsections run A.1 through A.8 only (grep `^### A\.` — A.1
// Lexical .. A.8 Constants and literals). The table is the UNNUMBERED one
// in the Appendix A preamble, introduced "Shared-production over-admissions
// (index, not a rule)" just before A.1, whose `Member` in an enum-or-choice
// body row reads (verbatim): "invariant by [§22.16](#sec-contract-clauses),
// destructors by [§8.7](#sec-dispose-is-not-on-object)". §22.16 itself
// ("Contract clauses") states the scope directly: "a class body may declare
// invariant members" — an enum or choice is not a class, so this is the
// primary authority; the Appendix A table is the index confirming the
// rejection is deliberate policy, not an oversight.
//
// One check for both kinds (unlike the split checkEnumRestrictions /
// checkChoiceRestrictions pair): the AST-read, the message shape, and the
// citation are identical for both, and unlike those two functions this one
// has no other kind-specific rule to carry, so a single small function
// reading `t->kind` beats two near-duplicate ones.
void Resolver::checkEnumChoiceInvariantMembers() {
    if (!ctx_.module()) return;
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind != ast::TypeDeclKind::Enum &&
            t->kind != ast::TypeDeclKind::Choice) return;
        // `t` is drawn from forEachTypeAndNested (#3574), which walks only
        // this module's own local declaration tree — never an imported type
        // (#3152). An imported enum/choice's invariant member, if the
        // exporting module's own compile somehow let one through, is out of
        // this check's reach (it has no AST here) but also out of scope:
        // the exporting module's compile is where this same check would
        // have run and rejected it before the artifact interface (§2.8.2)
        // could ever publish it.
        // SEAM-ACCESSOR: `t->decl` is always a real `ast::TypeDecl*` here.
        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) return;
        const bool isEnum = t->kind == ast::TypeDeclKind::Enum;
        for (const auto& mp : td->members) {
            auto* inv = dynamic_cast<const ast::InvariantDecl*>(mp.get());
            if (!inv) continue;
            ctx_.diagnostics().report(
                Severity::Error, inv->loc,
                std::string(isEnum ? "enum `" : "choice `") +
                std::string(t->name) +
                "` may not declare an `invariant` member — §22.16 scopes "
                "invariant members to class bodies, and " +
                std::string(isEnum ? "an enum" : "a choice") +
                " is not a class (§22.16)");
        }
    });
}

// fix #4186 (§22.16 "Class invariants", Appendix A.4 `InvariantDecl`
// production comment, v0.298): an `invariant` member on a RECORD body or a
// `static class` body compiles with zero diagnostics — MEASURED on this
// HEAD (pre-fix), both
//   record Point4186 public X: int; public Y: int; invariant(true); end Point4186;
// and
//   static class Utils4186 invariant(true); end Utils4186;
// compile via `runAot` at exit 0, zero diagnostics. Same root cause as fix
// #4115's checkEnumChoiceInvariantMembers just above (and explicitly left
// out of that fix's scope — its own trailing "KNOWN GAP" test row cites this
// issue by number): `invariant` clauses are not modeled as a `Symbol`
// subclass in `TypeSymbol::members` at all (SymbolCollection never creates
// one for `ast::InvariantDecl`), so a `t->members`-based walk cannot see
// one; the raw AST (`t->decl`'s `ast::TypeDecl::members`) is the only place
// a LOCAL declaration's invariant member is visible.
//
// Authority: A.4's `InvariantDecl` grammar production comment states the
// scope directly — "class bodies only; rejected semantically in records and
// static classes (§22.16)" — and `ast::InvariantDecl`'s own doc comment
// (src/ast/Node.hpp) repeats it verbatim ("Class-body-only per A.4;
// semantically rejected in records, static classes, and outside class
// bodies"). §22.16's opening sentence independently backs the record half
// ("a class body may declare invariant members" — a record is not a
// class), and its "Class invariants" subsection's own "Out of scope in this
// revision" list backs the static-class half explicitly: "static-class and
// static-state invariants (an `invariant` in a static class is a compile
// error — no instance boundary exists)".
//
// A record's positional-field constructor and a static class's absent
// instance boundary are UNRELATED reasons (worth two distinct diagnostics,
// like the enum/choice split above), so this walks both kinds in one
// function — mirroring checkEnumChoiceInvariantMembers's own "one function,
// kind-specific message" shape — rather than duplicating the AST-read loop.
void Resolver::checkRecordAndStaticClassInvariantMembers() {
    if (!ctx_.module()) return;
    forEachTypeAndNested([&](TypeSymbol* t) {
        const bool isRecord = t->kind == ast::TypeDeclKind::Record;
        bool isStaticClass = false;
        if (t->kind == ast::TypeDeclKind::Class) {
            for (auto mod : t->modifiers)
                if (mod == "static") { isStaticClass = true; break; }
        }
        if (!isRecord && !isStaticClass) return;
        // `t` is drawn from forEachTypeAndNested (#3574): this module's own
        // local declaration tree only, never an imported type (#3152) —
        // same reach argument as checkEnumChoiceInvariantMembers above.
        // SEAM-ACCESSOR: `t->decl` is always a real `ast::TypeDecl*` here.
        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) return;
        for (const auto& mp : td->members) {
            auto* inv = dynamic_cast<const ast::InvariantDecl*>(mp.get());
            if (!inv) continue;
            ctx_.diagnostics().report(
                Severity::Error, inv->loc,
                isRecord
                    ? (std::string("record `") + std::string(t->name) +
                       "` may not declare an `invariant` member — §22.16 "
                       "scopes invariant members to class bodies, and a "
                       "record is not a class (§22.16)")
                    : (std::string("static class `") + std::string(t->name) +
                       "` may not declare an `invariant` member — §22.16 "
                       "states an `invariant` in a static class is a "
                       "compile error: no instance boundary exists "
                       "(§22.16)"));
        }
    });
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
            // fix #4689: a resolution scope for the new record/enum/choice
            // bound check below — module-level names plus this declaration's
            // own generic-param names as markers (mirrors the identical
            // construction in checkGenericConstraints/checkOneConstraint's
            // caller, so `where T: IComparable<T>` and similar self-
            // referencing bounds resolve here exactly as they do there).
            Scope boundModule;
            if (ctx_.module())
                for (Symbol* child : ctx_.module()->children)
                    if (!child->name.empty())
                        boundModule.names[child->name] = child;
            Scope boundScope;
            boundScope.parent = &boundModule;
            for (std::string_view p : paramOrder) boundScope.names[p] = nullptr;
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

            // fix #4689 (§14.2 / §19.6 / the #4372 ruling, v0.307): §14.2's
            // constraint table admits exactly two NAMED-TYPE bound forms —
            // `where T: SomeClass` ("Class types only") and `where T: IFoo`
            // (interface conformance) — plus the type-parameter bound
            // `where T: U` (handled by `boundParam` above, and excluded
            // below). Grammar A.3 `Constraint ::= ... | Type` admits ANY
            // type as the catch-all last alternative, so a bound that
            // resolves to a RECORD, ENUM, or CHOICE parses and reaches
            // instantiation-time checking, but §14.2 never describes such a
            // bound: every cross-reference to a concrete-type bound
            // elsewhere in the spec speaks only of "a class bound" or "an
            // interface bound" (e.g. the erasure-guard passage's "a class
            // bound (`where T: SomeClass`) qualifies — only class types
            // satisfy it"). Admitting it was therefore a grammar-vs-
            // semantics gap, not a deliberate third form — the same gap
            // `caseArmsExhaustive`'s `forEachConstraintBound` walk inherited
            // (#4674/#4685's sibling issues) by resolving such a bound as a
            // legitimate closed-value-space scrutinee kind rather than
            // rejecting it as ill-formed. Reject it HERE, at the
            // declaration, independent of instantiation: an un-instantiated
            // generic is never visited by the per-instantiation
            // `checkGenericConstraints`/`checkOneConstraint` (this
            // function's own opening comment states exactly this reach
            // gap), so this declaration-level pass is the only site that
            // reliably catches it — the identical reasoning that puts the
            // self-referential/cyclic checks above in this function rather
            // than in the per-instantiation one.
            for (const auto& gc : gcs) {
                if (!gc.modifierConstraint.empty()) continue;
                for (const auto& item : gc.items) {
                    if (item.kind != "type" || !item.typeArg) continue;
                    if (!boundParam(item).empty()) continue;  // type-param bound
                    TypeRef* bound = ctx_.resolvedType(*item.typeArg);
                    if (!bound) bound = resolveType(*item.typeArg, &boundScope);
                    if (!bound || bound->kind != TypeRef::Kind::Named ||
                        !bound->declaration)
                        continue;  // primitive/builtin/unresolved: defer
                    ast::TypeDeclKind bk = bound->declaration->kind;
                    if (bk == ast::TypeDeclKind::Record ||
                        bk == ast::TypeDeclKind::Enum ||
                        bk == ast::TypeDeclKind::Choice) {
                        const char* kindWord =
                            bk == ast::TypeDeclKind::Record
                                ? "record"
                                : (bk == ast::TypeDeclKind::Enum ? "enum"
                                                                  : "choice");
                        ctx_.diagnostics().report(
                            Severity::Error, loc,
                            "type parameter `" + std::string(gc.typeParam) +
                            "` is constrained to `" +
                            std::string(bound->declaration->name) + "`, a " +
                            kindWord +
                            " — a where-clause type bound may name only a "
                            "class or an interface (§14.2); write `where " +
                            std::string(gc.typeParam) +
                            ": value` to admit records, enums, and choices, "
                            "or constrain on an interface the type "
                            "implements");
                    }
                }
            }
        };
    for (Symbol* child : ctx_.module()->children) {
        auto* ts = dynamic_cast<TypeSymbol*>(child);
        if (!ts || !ts->decl) continue;
        // SEAM-ACCESSOR: already explicitly gated one line above (`if (!ts || !ts->decl)
        // continue;`) — this cast cannot see a null `decl` (#3152).
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
                // §14.2 (Conditionally-admitted members, rule 1, proposal 0116
                // D7, fix #3330): the *constrained identifier* of a method's
                // `where` clause must be one of the method's OWN type
                // parameters. The bound may name any type parameter in scope
                // (`where U: T` on `class Box<T>` is the ordinary type-
                // parameter-bound form, fully defined and unaffected), but
                // constraining the *enclosing* type's T on a member is a parse
                // that means nothing: SymbolCollection's collectGenericParams
                // keys constraints by parameter name and finds no match, so
                // the clause is silently dropped — the inert-looking-but-load-
                // bearing failure mode spec §9.2 names. Reject at the
                // declaration so the form is never written.
                std::unordered_set<std::string_view> methodOwnParams;
                for (const auto& gp : mdl->genericParams)
                    methodOwnParams.insert(gp.name);
                std::unordered_set<std::string> memberWhereReported;
                for (const auto& gc : mdl->genericConstraints) {
                    if (methodOwnParams.count(gc.typeParam)) continue;
                    if (!memberWhereReported.insert(std::string(gc.typeParam))
                             .second)
                        continue;  // one diagnostic per offending identifier
                    ctx_.diagnostics().report(
                        Severity::Error, mdl->span.begin,
                        std::string("method-level `where` clause names `") +
                        std::string(gc.typeParam) +
                        "`, which is a type parameter of the enclosing "
                        "declaration, not of this method (§14.2 "
                        "*Conditionally-admitted members*, rule 1 — a "
                        "`where` clause on a member may constrain only the "
                        "member's own type parameters; constraining an "
                        "enclosing type parameter on a member is meaningless "
                        "and not a user-extensible mechanism)");
                }
                checkList(mdl->genericConstraints, methodParams, mdl->span.begin);
            }
    }
}

void Resolver::checkConstTypes() {
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypeConsts(t); });
}

void Resolver::checkTypeConsts(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* cs = dynamic_cast<ConstMemberSymbol*>(m);
        if (!cs) continue;
        // SEAM-ACCESSOR: `cs` is a member of the locally-declared `t` (forEachTypeAndNested,
        // #3574); `t`'s OWN const members are always local (#3152).
        auto* cd = dynamic_cast<const ast::ConstDecl*>(cs->decl);
        // Build the `resolve` closure once — used by the (type-independent)
        // div-by-zero probe below AND by the integer-range check further down.
        //
        // fix #3635 (§6.1 / Appendix C.11, v0.286): walk `t`'s own members
        // AND its class base chain, so a bare name naming an INHERITED
        // const resolves here too. Before this fix the walk covered only
        // `t->members`; `fix #3619` deliberately DECLINED to fold this
        // closure into the shared `resolveConstIdentInScope` (which already
        // has this same walk) because the two differ in what happens on an
        // unfoldable match — `resolveConstIdentInScope` falls through to
        // let a same-named symbol further up shadow it, while this closure
        // returns immediately once ANY level of the chain has a matching
        // name, foldable or not. That contract is preserved: the widening
        // is *which levels get searched*, not *what happens on a match*.
        // Reachable: `Derived : Base` where `Derived`'s own const references
        // `Base`'s by bare name — measured silently accepting an
        // overflowing value (and, independently, a division by an inherited
        // zero const) before this fix, since both `foldIntConst` and
        // `containsConstDivByZero` share this one `resolve`.
        std::function<std::optional<long long>(std::string_view, int)>
            resolve = [&](std::string_view name,
                          int depth) -> std::optional<long long> {
            for (TypeSymbol* ts = t; ts;) {
                for (Symbol* sm : ts->members) {
                    auto* scs = dynamic_cast<ConstMemberSymbol*>(sm);
                    if (!scs || scs->name != name) continue;
                    if (auto* scd =
                            // SEAM-ACCESSOR: `ts` here walks `t`'s class BASE CHAIN (`ts->baseTypes` below), which
                            // DOES cross module boundaries, so `scs` can be an imported const. Fix
                            // #3230 (family M11 of #3152) already covers that: when `scd` (this AST
                            // cast) is null, the fallback three lines below reads the loader's
                            // typed `scs->intValue` instead of silently no-opping.
                            dynamic_cast<const ast::ConstDecl*>(scs->decl))
                        if (scd->value)
                            return foldIntConst(*scd->value, resolve,
                                                 depth + 1);
                    // fix #3230 (§17.14 / §15.1 / §22.12; family M11 of
                    // umbrella #3152): an imported sibling const has no AST
                    // TypeDecl (the symbol is reconstructed from
                    // metadata), so the `scd->value` arm above returns
                    // nullopt and the §6.1 / Appendix C.11 range check
                    // below silently no-ops. Fall back to the typed value
                    // the loader populated (parseImportedConstIntText in
                    // ModuleLoader.cpp).
                    if (scs->intValue) return *scs->intValue;
                    return std::nullopt;
                }
                TypeSymbol* nb = nullptr;
                for (TypeSymbol* base : ts->baseTypes)
                    if (base && base->kind == ast::TypeDeclKind::Class) {
                        nb = base;
                        break;
                    }
                ts = nb;
            }
            return std::nullopt;
        };

        // fix #4391 (§17.14, v0.298): the div-by-zero probe must run even for
        // inferred-type consts. Without this, `const C := 5 / Z` (where Z:=0)
        // was silently accepted because the `!cd->type` guard below skipped the
        // whole block. The probe is type-independent — it needs only the
        // initializer expression — so it runs before that guard.
        if (cd->value) {
            bool hasDivZero = containsConstDivByZero(*cd->value, resolve);
            if (hasDivZero) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("const `") + std::string(cs->name) +
                    "` initializer contains a division or modulo by "
                    "zero — a const initializer must fold to a "
                    "representable compile-time-constant value (§22.12)");
            }
        }

        // The remaining checks (type admissibility and integer range) require an
        // explicit type annotation — inferred types are deferred to ExprTypecheck.
        if (!cd->type) continue;
        TypeRef* rt = ctx_.resolvedType(*cd->type);
        if (!rt) continue;
        // fix #3879 (§22.12, v0.294): routed through the ONE shared predicate
        // (ConstFold.hpp) so this explicit-type-annotation admission point
        // and ExprTypecheck's inferred-type mirror (fix #3856) cannot drift —
        // this used to spell `kind == Primitive && primitive != Object`
        // in-line, which (like the mirror) omitted the `nullable` clause
        // rule 2's table states, silently accepting `const A: int? := 1;`.
        bool ok = isAdmissibleClassConstType(*rt);
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
            // §17.14 (v0.184, #1688): fitsConstIntRange is shared with the
            // local-`const` fault check (ExprTypecheck.cpp) so both const forms
            // agree on exactly one "fits" definition (ConstFold.hpp).
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
            }
            // §6.5 / §17.14 (v0.242, #3045): the OTHER evaluation fault
            // that leaves an otherwise-constant initializer unfoldable —
            // "`const F := 1 << 64;` [is] rejected where [it is] written"
            // (§17.14) — needs no probe here. Unlike a `/ 0`, the const's
            // initializer expression is visited by ExprTC, whose
            // checkShiftCountDomain reports the fault at the shift itself
            // and with the left operand's real type (which this walk can
            // only infer from the AST shape). A probe here would only
            // double the diagnostic.
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
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested(
        [&](TypeSymbol* t) { checkTypeAccessorVisibility(t); });
}

void Resolver::checkTypeAccessorVisibility(TypeSymbol* t) {
    for (Symbol* m : t->members) {
        auto* ps = dynamic_cast<PropertySymbol*>(m);
        if (!ps) continue;
        // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t` (forEachTypeAndNested,
        // #3574); accessor-visibility is a declaration-site check (#3152).
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

// #3152: every `hasAstAutoPropertyDecl` in this function is the AST-ONLY
// predicate on purpose. The walk is over `ctx_.module()->children` and each
// arm requires the member's own `ast::PropertyDecl` (it reads `pd->type` to
// classify the declared type), so no imported symbol reaches it and the two
// predicates coincide. Contrast `classHasOwnStorage` in
// `checkClassSerializationEligibilityLate`, which climbs `t->baseTypes` and
// therefore DOES leave the module — that one asks the storage question.
void Resolver::checkRecordRestrictions() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t || t->kind != ast::TypeDeclKind::Record) continue;
        // §9.6 / §12.5 (issue #2431): `abstract` is defined ONLY for a
        // class — §12.5 ("Abstract classes and members") states "`abstract`
        // on a class permits abstract members and forbids direct
        // instantiation," with every example and every other mention of
        // `abstract` in the spec written in terms of a class. §9.6
        // ("Records and inheritance") states plainly "Records are
        // implicitly sealed" and "A record cannot derive from another
        // record or from a class" — so a record can never have a subclass
        // through ANY route, which an `abstract` class awaits (possibly in
        // a different, later-compiled module). An `abstract record` would
        // therefore be permanently uninstantiable with no concrete type
        // ever able to provide bodies for its abstract members — a
        // meaningless combination the parser over-admits (it deliberately
        // defers all modifier-combination validity to sema, matching its
        // own §11.7-citing comment in Parser_Decls.cpp) and that nothing
        // previously rejected. `checkClassModifierCombinations`
        // (§11.7-driven `static`+`abstract` / `static`+`sealed` exclusion)
        // is filtered to `TypeDeclKind::Class` only and never inspects a
        // record's modifiers at all — this is a DECLINE-WITH-REASONS
        // interpretation (CLAUDE.md rule 4), not a spec ruling: the spec
        // does not explicitly forbid the token sequence `abstract record`,
        // but simply never defines a meaning for it, and per CLAUDE.md's
        // "when the spec is unclear ... implement the simplest
        // interpretation, document it" guidance, rejecting is the chosen,
        // recorded reading (see issue #2431 for the full analysis and the
        // alternative considered — declining to write code and instead
        // requesting a human ruling — before this was judged clear enough
        // to implement directly).
        bool recordIsAbstract = false;
        for (auto mod : t->modifiers)
            if (mod == "abstract") { recordIsAbstract = true; break; }
        if (recordIsAbstract) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string("record `") + std::string(t->name) +
                "` may not be `abstract` — records are implicitly sealed "
                "and can never be derived from (§9.6), so an abstract "
                "record could never have a concrete subclass to provide "
                "bodies for its abstract members; `abstract` is defined "
                "only for a class (§12.5)");
        }
        for (Symbol* m : t->members) {
            if (dynamic_cast<DestructorSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "` may not declare a destructor (§9.7)");
                continue;
            }
            // per §24 (v0.180): an event's compiler-generated `owned
            // EventSource` backing field requires an owning, disposable
            // container, which a record is not (§9.7, Appendix C.3).
            if (dynamic_cast<EventSymbol*>(m)) {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    std::string("record `") + std::string(t->name) +
                    "` may not declare an `event` member — an event's "
                    "compiler-generated `owned EventSource` backing field "
                    "requires an owning, disposable container, which a record "
                    "is not (§24, §9.7)");
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
                //
                // fix #4677 (§9.7 / §15.3 v0.307): this used to read ONLY the
                // body-record `ast::FieldDecl` source
                // (`dynamic_cast<const ast::FieldDecl*>(fs->decl)`), so a
                // POSITIONAL record's own [N owned]T-typed constructor
                // parameter (`record Wrapper(Items: [2 owned]Connection);`)
                // was never checked — `fs->decl` is null for a positional
                // field (ModuleLoader/SymbolCollection's positional-param
                // representation), so the cast always failed and the whole
                // branch was silently skipped. The sibling owning-choice
                // check 20 lines below already reads BOTH sources via the
                // shared #3650 accessor (`sema::fieldDeclTypeOf`, which
                // checks `fs->posParam` first, then `fs->decl`'s
                // `ast::FieldDecl`); this check just never got migrated to
                // it. Same accessor here closes the gap for both record
                // forms uniformly.
                if (const ast::Type* fty = sema::fieldDeclTypeOf(fs)) {
                    auto* rt = ctx_.resolvedType(*fty);
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
                // §9.7 / §35.4 (issue #2210, v0.226): an owning-classified
                // choice as a record field is rejected. Records declare no
                // disposal path (§9.7: no destructor, no `owned` fields, no
                // `IDisposable`), and §35.4 makes owning choices move-only
                // value types whose active case carries `owned` payloads
                // (§28.3); the buried owner would never be discharged. The
                // §35.4 classification is structural (§35.4) — a generic
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
                //
                // fix #3650 (#3152 drain): the shared both-sources accessor —
                // `fs` here is always a member of `t`, which this function's
                // caller (checkRecordRestrictions) draws only from
                // `ctx_.module()->children`, so `fs` is never imported and
                // `fieldDeclTypeOf` never takes the null-`decl` branch here.
                const ast::Type* ftypeAst = sema::fieldDeclTypeOf(fs);
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
                if (hasAstAutoPropertyDecl(ps)) {
                    // SEAM-ACCESSOR: `ps` is a member of `t`, which checkRecordRestrictions draws only
                    // from `ctx_.module()->children` — never an imported symbol (#3152).
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
        // SEAM-ACCESSOR: `t` is drawn from `ctx_.module()->children`
        // (checkRecordRestrictions's own loop) — never an imported type
        // (#3152).
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
    // #594 + #4421 (§28.2, stdlib §2.10, v0.300): "User code does not implement
    // `Dispose()` directly. ... Declaring a member named `Dispose` on a class
    // or a record is a compile error at that declaration, whatever its
    // parameter list; the diagnostic names the two customization points above
    // — the §9.3 posture for a body member colliding with a compiler-generated
    // one, keyed on the name as §12.2 is."
    //
    // A user-declared `Dispose` on a class or record is never invoked by the
    // compiler's ownership machinery (using var / owned field teardown /
    // explicit Dispose call all go through the compiler-generated Dispose) so
    // the user body silently never runs — a silent trap.  Reject it here.
    //
    // The v0.300 widening to "whatever its parameter list" matters at
    // optional-parameter declarations specifically (`Dispose(mode: int := 0)`,
    // §22.5): the zero-argument call `x.Dispose()` is then ambiguous against
    // the generated member, so the body silently never runs even though the
    // call looks right.
    //
    // Applies to both Class and Record even though records cannot normally
    // carry a destructor (§9.7): a user who accidentally writes a Dispose
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
            ctx_.diagnostics().report(
                Severity::Error, ms->declLoc,
                std::string(t->kind == ast::TypeDeclKind::Record ? "record" : "class") +
                " `" + std::string(t->name) +
                "` may not declare a `Dispose` method directly (§28.2): "
                "the compiler synthesises `Dispose()` from the `destructor`; "
                "user-declared `Dispose` bodies are never called by the "
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

// fix #3032 (§15.4.1, v0.262): a type declaring a static
// `FromCollectionLiteral` whose carrier is a recognized form
// (`ReadOnlyArraySpan<T>` or `owned OwningArray<T>`) must be `public` and must
// return `owned C<T>` at the enclosing type's own arity; declaring both
// carriers on the same type is a compile error. #2957 deliberately scoped
// the recognition predicate to the three mechanically-distinguishable
// conjuncts (exact name, `static`, exactly one recognized carrier) and
// declared these three conjuncts out of scope — folding them into the
// recognition predicate would reclassify such a type as "not a
// sequence-literal target at all", which is the wrong advice (the target
// type is fine; the factory's modifier or return type is at fault). The
// check therefore fires on the method's own declaration so the user sees a
// declaration-site diagnostic that points at the offending method, not at
// every literal site that targets it.
//
// Three conjuncts are checked (the three explicitly enumerated by §15.4.1
// v0.262, lines 2942-2954):
//   1. public — visibility must be `ast::Visibility::Public`.
//   2. owned C<T> return — must return `owned`, and the return type's head
//      must be the enclosing type with the same arity as the enclosing
//      type's generic-parameter list.
//   3. both-forms-allowed — at most one recognized carrier may be declared
//      on a given type; declaring both is a compile error. The error
//      surfaces on the SECOND declaration site so the user sees the value
//      that's the conflict (the FIRST declaration is the one they'll keep
//      when they delete the second).
//
// Out of scope (already enforced structurally elsewhere):
//   * `static` — the recognition predicate `isCollectionLiteralAdmissibleTarget`
//     (ExprTypecheck_Calls.cpp) only recognises a static method, so a non-
//     static `FromCollectionLiteral` is silently treated as a non-opt-in.
//   * declared directly on the collection type — `TypeSymbol::members` is
//     scoped to the current type so extension methods and inherited
//     members from a base class never appear here.
void Resolver::checkCollectionLiteralFactoryConjuncts() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        // Walk the type's members for every static `FromCollectionLiteral`
        // overload. The check fires on overloads whose carrier is one of
        // the two recognized forms; a non-recognized carrier leaves the
        // method NOT in the sequence-literal protocol and §15.4.1's
        // public / return-type / both-forms conjuncts do not apply.
        //
        // The both-forms check is per-type-across-overloads, so we
        // accumulate the form of each canonical-signature overload and
        // fire the diagnostic on the SECOND one found (deterministic —
        // `TypeSymbol::members` is in declaration order, the order the
        // user wrote them).
        const MethodSymbol* borrowForm = nullptr;
        const MethodSymbol* owningForm = nullptr;
        for (Symbol* m : t->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (!ms || ms->name != "FromCollectionLiteral") continue;
            if (ms->params.empty()) continue;
            // Static? Mirrors the predicate at ExprTypecheck_Calls.cpp:411-413
            // — `static` is on the modifiers vector, not a separate flag.
            bool isStatic = false;
            for (auto mod : ms->modifiers)
                if (mod == "static") { isStatic = true; break; }
            if (!isStatic) continue;
            // Identify the carrier form by reading the sole parameter's
            // resolved type. The carrier is checked directly off the
            // ParameterSymbol so the test is symmetric on local and
            // imported symbols (the AST-side arm is preferred when present;
            // for an imported method the resolved type lives on
            // `resolvedDeclType`).
            auto* param = dynamic_cast<ParameterSymbol*>(ms->params.front());
            if (!param) continue;
            const TypeRef* paramTy = param->declType
                                        ? ctx_.resolvedType(*param->declType)
                                        : param->resolvedDeclType;
            if (!paramTy || paramTy->kind != TypeRef::Kind::Named ||
                !paramTy->astNode || paramTy->astNode->name->parts.empty())
                continue;
            std::string_view carrierName =
                paramTy->astNode->name->parts.back();
            // The borrowing form's `ReadOnlyArraySpan<T>` is read by-value
            // into the factory; the owning form's `OwningArray<T>` is read
            // `owned` (the spec example writes `owned elements: OwningArray<T>`).
            // The carrier NAME is the discriminator — the modifier is
            // irrelevant to the §15.4.1 form split.
            // fix #3857 (§15.4.1, v0.272): proposal 0115 renamed the
            // borrowing carrier from `ReadOnlySpan<T>` (the OWNING family's
            // view, backed by `OwningBuffer<T>`) to `ReadOnlyArraySpan<T>`
            // (the borrowing family's view, backed by `Array<T>`) — the old
            // spelling was ownership-incoherent for a protocol whose
            // elements are borrows. The old spelling is no longer part of
            // the protocol at all: reject it here with a remedy naming the
            // current carrier, rather than leaving it silently unrecognized
            // (which left `isCollectionLiteralAdmissibleTarget` admitting
            // the literal on name alone and crashed codegen — #3857).
            if (carrierName == "ReadOnlySpan") {
                ctx_.diagnostics().report(
                    Severity::Error, ms->declLoc,
                    std::string("`FromCollectionLiteral` on type `") +
                    std::string(t->name) +
                    "` declares its borrowing carrier as `ReadOnlySpan<T>`, "
                    "which is no longer recognized; the borrowing carrier "
                    "is `ReadOnlyArraySpan<T>` as of spec v0.272 (§15.4.1)");
                continue;
            }
            const bool isBorrowForm = (carrierName == "ReadOnlyArraySpan");
            const bool isOwningForm = (carrierName == "OwningArray");
            if (!isBorrowForm && !isOwningForm) continue;
            // Conjunct 1: visibility must be `public`. §15.4.1: "The method
            // must be `public`". `ast::Visibility::None` is the parser's
            // default for a missing modifier (the same default that lets
            // §11.1's default class member visibility be `private`); for
            // a method it is always-default `private`.
            if (ms->visibility != ast::Visibility::Public) {
                ctx_.diagnostics().report(
                    Severity::Error, ms->declLoc,
                    std::string("`FromCollectionLiteral` on type `") +
                    std::string(t->name) + "` must be `public` (§15.4.1)");
            }
            // Conjunct 2: return type must be `owned C<T>` at the
            // enclosing type's own arity. §15.4.1: "must return `owned
            // C<T>` where `C<T>` is the enclosing type at its own arity".
            // The `owned` marker is checked first; the head-and-arity
            // match is checked second (returnType must resolve to a Named
            // type whose head is the enclosing type and whose arity matches
            // the enclosing type's generic-parameter list — a non-generic
            // enclosing type has arity 0, so the return type carries no
            // generic args; a generic enclosing type has arity 1, so the
            // return type carries exactly one generic arg).
            // SEAM-ACCESSOR: AST-arm of the both-sources pattern; the
            // importer-side arm is the producer's sema pass (the spec
            // shape check on an imported factory ran in the importer's
            // own compilation, so the AST-side guard here is local-only
            // by spec). `returnOwned` below uses `methodReturnsOwned`.
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            if (!md) continue;
            if (!md->returnType) {
                // Spec: the method must return `owned C<T>`. A missing
                // `: <type>` clause leaves the method returning `()`
                // (§22.1), which is not `owned C<T>` for any C — diagnostic.
                ctx_.diagnostics().report(
                    Severity::Error, ms->declLoc,
                    std::string("`FromCollectionLiteral` on type `") +
                    std::string(t->name) +
                    "` must return `owned " + std::string(t->name) +
                    "` (§15.4.1)");
            } else {
                if (!methodReturnsOwned(*ms, ctx_)) {
                    ctx_.diagnostics().report(
                        Severity::Error, ms->declLoc,
                        std::string("`FromCollectionLiteral` on type `") +
                        std::string(t->name) +
                        "` must return `owned " + std::string(t->name) +
                        "` (§15.4.1)");
                }
                const TypeRef* retTy = ctx_.resolvedType(*md->returnType);
                // The return type's element type must match the carrier's
                // element type — §15.4.1: "must return `owned C<T>` where
                // `C<T>` is the enclosing type at its own arity" and "the
                // element type is read directly from the parameter's
                // element type". The shape check is split into three parts:
                //   (a) head matches the enclosing type,
                //   (b) arity matches the enclosing type's arity,
                //   (c) the element type (the carrier's sole generic arg)
                //       equals the return type's sole generic arg.
                // The combined predicate mirrors the "owned C<T>" rule from
                // the spec's parameter signature template (§15.4.1 lines
                // 2945 and 2951).
                const TypeRef* elemTy =
                    (!paramTy->genericArgs.empty())
                        ? paramTy->genericArgs[0]
                        : nullptr;
                const TypeRef* retElemTy =
                    (retTy && !retTy->genericArgs.empty())
                        ? retTy->genericArgs[0]
                        : nullptr;
                const bool shapeOk =
                    retTy && retTy->kind == TypeRef::Kind::Named &&
                    retTy->declaration == t &&
                    retTy->genericArgs.size() == t->genericParams.size() &&
                    // The element-type match (§15.4.1 "must return `owned
                    // C<T>`" — T is the SOLE type argument of both the
                    // carrier AND the return type when the enclosing is
                    // generic) is only meaningful for generic enclosing
                    // types. For a NON-generic enclosing type the return
                    // is `owned C` — no element type exists to compare
                    // against the carrier's element type, and
                    // `retElemTy == nullptr` while `elemTy` is the
                    // carrier's (e.g. `int` from `ReadOnlyArraySpan<int>`).
                    // Without this carve the conjunct rejected every
                    // non-generic-owning form (with no T to bind, the
                    // carrier's T is whatever the user wrote).
                    (t->genericParams.size() == 0 ||
                     typeRefEqual(elemTy, retElemTy));
                if (!shapeOk) {
                    ctx_.diagnostics().report(
                        Severity::Error, ms->declLoc,
                        std::string("`FromCollectionLiteral` on type `") +
                        std::string(t->name) +
                        "` must return `owned " + std::string(t->name) +
                        "` (§15.4.1)");
                }
            }
            // Conjunct 3: at most one form declared on a given type.
            // Fire the diagnostic on the SECOND declaration; the first one
            // stays put so the user's "delete the second" fix is obvious.
            if (isBorrowForm) {
                if (owningForm) {
                    ctx_.diagnostics().report(
                        Severity::Error, ms->declLoc,
                        "type `" + std::string(t->name) +
                        "` declares both recognized `FromCollectionLiteral` "
                        "carriers (`ReadOnlyArraySpan<T>` and `OwningArray<T>" +
                        "`); exactly one may be declared on a given type "
                        "(§15.4.1)");
                }
                borrowForm = ms;
            } else /* isOwningForm */ {
                if (borrowForm) {
                    ctx_.diagnostics().report(
                        Severity::Error, ms->declLoc,
                        "type `" + std::string(t->name) +
                        "` declares both recognized `FromCollectionLiteral` "
                        "carriers (`ReadOnlyArraySpan<T>` and `OwningArray<T>" +
                        "`); exactly one may be declared on a given type "
                        "(§15.4.1)");
                }
                owningForm = ms;
            }
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
            // of the type identity (§2.8.3 rule 4 / §15.1). Comparing only element +
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
    // per §22.6 / §11.3 (v0.250, proposal 0091): ordinary-parameter `owned`
    // is a *candidate* for overload distinguishability under the declaration
    // guard below; until the guard check (signaturesIndistinguishable) runs,
    // `owned` keeps its own key so the two sides of the comparison can be
    // recognised. `scoped` never distinguishes (no call-site marker, §28.14)
    // and still collapses to the plain form. `out owned` collapses to `out`
    // (the v0.77 `out`/`out owned` carve-out is withdrawn). `ref` is its
    // own kind.
    if (mod == "owned") return "owned_value";
    if (mod == "scoped" || mod.empty()) return "value";
    if (mod == "out owned" || mod == "out") return "out";
    if (mod == "ref") return "ref";
    return mod;
}

// §22.6 (v0.250, proposal 0091) declaration guard (the erasure test):
// does the declared TypeRef at an ordinary-`owned` differing position
// qualify?  A type qualifies when (a) it is a class type, (b) it is an
// interface type whose conforming set is class-only (IDisposable or any
// IDisposable-extending interface — the closure §8.7 guarantees — and the
// inheritance §14.2 rule 3 carries), or (c) it is a type parameter whose
// own constraint set qualifies.  Any unqualifying position leaves the
// pair indistinguishable (the pair still clashes at the second declaration,
// reported under §11.3).
//
// All conservative branches err on the side of REJECTING (returning
// `false` — the pair still clashes), which is the safe direction per
// §22.6's "Never ship unsound code" posture: a TypeRef we cannot
// confidently classify never grants the lift. The class-type and
// IDisposable-closure arms are precise; everything else (primitives,
// records/enums/choices, ordinary user-declared interface bounds, the
// `class` special constraint, function types, inline arrays, value
// constraints) is conservative — over-rejection in those rare cases.
//
// `pa` is the parameter the TypeRef belongs to; it is the lookup root
// for the bare-type-parameter case (`T` resolves to a Named TypeRef
// whose `declaration` is null — see `bareTypeParamRef` in
// ExprTypecheck_Calls.cpp:17597 — and whose name is matched against the
// enclosing type's and method's own `genericParams`).
bool Resolver::positionTypePassesDeclarationGuard(
    const TypeRef* tr, const ParameterSymbol* pa, const MemberSymbol* enclosing) {
    if (!tr) return false;
    // Nullability is stripped per the guard (see long comment below).
    if (tr->nullable) return false;
    if (tr->kind == TypeRef::Kind::Named) {
        TypeSymbol* effectiveDecl = tr->declaration;
        if (!effectiveDecl) {
            // `IDisposable` (and the other §8.7 closure members — see comment
            // below) has NO TypeSymbol of its own (`nonSymbolBaseRefs` source,
            // SymbolCollection.cpp:955). `builtinInterfaceSym` only synthesizes
            // for the `isSynthesizedBuiltinInterfaceName` set (IComparable,
            // IEquatable, …) and returns null for `IDisposable`. The §8.7
            // closure establishes that `IDisposable` (and any IDisposable-
            // extending interface) has a class-only conforming set AS A MATTER
            // OF LANGUAGE RULE — there is no other path to an `IDisposable`
            // conforming class without it being a class. So at a constraint
            // bound position (where we have only a name, no resolved
            // TypeSymbol yet), `IDisposable` qualifies directly.
            std::string_view nm = tr->headName();
            if (nm == "IDisposable") return true;
            // For non-IDisposable names, fall back to `builtinInterfaceSym`
            // (the synthesized set, e.g. `IComparable`) — a user interface
            // named `IComparable` does NOT qualify (§2.8.3 identity rule, and
            // the synthesized TypeSymbol carries `isSynthesizedBuiltinInterface`
            // so `inheritsBuiltinInterface(..., "IDisposable")` rejects it).
            if (!nm.empty()) effectiveDecl = ctx_.builtinInterfaceSym(nm);
        }
        if (!effectiveDecl) {
            // Bare type-parameter reference (`T` inside `class Box<T>`).
            // The TypeRef's declaration is null (a §14.1 "bare" reference);
            // its identifying name lives in `headName()`. Look it up in
            // the enclosing method's (if any) and enclosing type's
            // genericParams; method-level generic params SHADOW type-level
            // ones with the same name (the same shadowing
            // ExprTypecheck_Calls.cpp:4865 resolves by walking the method
            // first). `pa` is the parameter; `enclosing` is the owning
            // member (method/ctor/indexer) — ParameterSymbol itself has no
            // back-pointer, so the caller passes the member in.
            std::string_view nm = tr->headName();
            if (nm.empty()) return false;
            const GenericParamSymbol* gp = nullptr;
            if (auto* ms = dynamic_cast<const MethodSymbol*>(enclosing)) {
                for (Symbol* g : ms->genericParams) {
                    if (g && g->name == nm) {
                        gp = dynamic_cast<GenericParamSymbol*>(g);
                        break;
                    }
                }
            }
            if (!gp && enclosing && enclosing->parent) {
                for (Symbol* g : enclosing->parent->genericParams) {
                    if (g && g->name == nm) {
                        gp = dynamic_cast<GenericParamSymbol*>(g);
                        break;
                    }
                }
            }
            (void)pa;  // reserved for future use (per-param closure tracking)
            if (!gp) return false;
            return typeParamConstraintSetQualifies(gp);
        }
        // A named TypeRef whose declaration is a GenericParamSymbol is a
        // type-parameter reference (rare — usually an explicit alias
        // through a method-generic-param symbol).
        if (auto* gp = dynamic_cast<GenericParamSymbol*>(effectiveDecl)) {
            return typeParamConstraintSetQualifies(gp);
        }
        if (auto* ts = dynamic_cast<TypeSymbol*>(effectiveDecl)) {
            if (ts->kind == ast::TypeDeclKind::Class) {
                // Every class type's marker survives §28.1's erasure
                // (only value/runtime-managed substitutions erase).
                return true;
            }
            if (ts->kind == ast::TypeDeclKind::Interface) {
                // Qualifies iff the interface's conforming set is
                // class-only as a matter of language rule. The §8.7
                // closure makes this true for IDisposable and for
                // every IDisposable-extending interface.
                return inheritsBuiltinInterface(ts, "IDisposable");
            }
            // Record, enum, choice — the kind does not survive §28.1's
            // erasure as a class.
            return false;
        }
        return false;
    }
    // Primitive, InlineArray, Function, Unit, Error: never class-only.
    return false;
}

// §22.6 declaration guard's constraint-set half. A constraint set qualifies
// iff every individual constraint qualifies individually:
//   - the special `class` constraint does NOT qualify (admits `string` and
//     function-type substitutions, §14.2);
//   - a class-typed bound (`where T: SomeClass`) qualifies — only class
//     types satisfy it;
//   - an interface-typed bound qualifies iff the interface's conforming
//     set is class-only (IDisposable or any IDisposable-extending
//     interface);
//   - `value`, `notnull`, `new`, `sendable`, `hashable` and similar special
//     constraints do NOT qualify on their own;
//   - an unconstrained type parameter has an empty constraint set and
//     therefore does NOT qualify (every substitution is admissible).
//
// We deliberately read the per-constraint qualifier in the strictest
// interpretation of the spec text: any non-qualifying constraint in the
// set rejects the whole set. This over-rejects on `where T: class, X`
// where X happens to be a class-only interface (the intersection is still
// class-only), but is the safe direction.
bool Resolver::typeParamConstraintSetQualifies(const GenericParamSymbol* gp) {
    if (!gp) return false;
    if (gp->constraints.empty()) return false;
    for (const GenericParamConstraint& c : gp->constraints) {
        // The special `class` form — does NOT qualify.
        if (c.kind == "class") return false;
        // Type-bound form — qualifies iff the bound is class-typed or an
        // IDisposable-closure interface.
        if (c.kind == "type") {
            TypeRef* bound =
                c.importedBound ? c.importedBound
                                : (c.astBound ? ctx_.resolvedType(*c.astBound)
                                              : nullptr);
            if (!positionTypePassesDeclarationGuard(bound, nullptr, nullptr)) {
                // Constraint resolution is a separate late pass (#3146): on
                // overload-signature checking the resolved TypeRef may be
                // null. Fall back to the AST node's bare name — for
                // `where T: IDisposable`, `astBound->name->parts.back()` is
                // `IDisposable`, which the §22.6 guard's name-side arm
                // (positionTypePassesDeclarationGuard) recognises. The bare
                // name alone admits ONLY the closed IDisposable-closure set
                // (and a few synthesized builtins via the existing
                // fallback), so this over-rejects by construction on
                // user-declared interface bounds whose resolution hasn't
                // landed yet — the safe direction per §22.6.
                if (!bound && c.astBound && c.astBound->name &&
                    !c.astBound->name->parts.empty()) {
                    std::string_view nm =
                        c.astBound->name->parts.back();
                    if (nm == "IDisposable") continue;
                }
                return false;
            }
            continue;
        }
        // value, notnull, new, sendable, hashable, ... — none qualify.
        return false;
    }
    return true;
}

void Resolver::checkOverloadSignatures() {
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypeOverloads(t); });
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
    // fix #4733 (§22.16, v0.307, corpus row s22_16_0026): a pair of
    // methods whose parameters match signaturesIndistinguishable but
    // whose ONLY distinguishing attribute is their contract-clause list
    // (§22.16 "Clauses never distinguish overloads and are not part of
    // §22.6 resolution; two members differing only in clauses are the
    // same signature and clash") surfaces here through the §22.6/§11.3
    // overload-resolution machinery, but the §22.6/§11.3 wording
    // (`indistinguishable … `owned`/`scoped`') is the WRONG diagnosis —
    // it names the parameter-modifier rule for a case where the modifier
    // lists are identical. The two reasons for the clash are distinct
    // (`owned`/`scoped` collapses in §11.3 vs clauses excluded in
    // §22.16) and the diagnostic must name each. Detect the clause-only
    // case at the same site and emit a §22.16-specific message naming
    // the rule's mechanism ("same signature") — the substring the
    // corpus row s22_16_0026's `//! diagnostic:` pin requires.
    auto clauseOnlyDifference = [this](const Symbol* aSym,
                                   const Symbol* bSym) -> bool {
        auto* msA = dynamic_cast<const MethodSymbol*>(aSym);
        auto* msB = dynamic_cast<const MethodSymbol*>(bSym);
        if (!msA || !msB) return false;
        if (msA->isOperator || msB->isOperator) return false;
        // An imported MethodSymbol carries no `decl`, so both casts below
        // yield null and this helper answers `false` for every imported
        // method (#3152 class-3). Severity is DIAGNOSTIC WORDING ONLY: the
        // clash is already decided elsewhere, and a false answer here merely
        // falls back to the §22.6/§11.3 "indistinguishable" text instead of
        // §22.16's "same signature". It never changes admission. The path is
        // probably unreachable with imported symbols at all — a clash is
        // diagnosed at the declaring site — but that is reasoning, not a
        // measurement, so it is tracked rather than asserted safe. Drain by
        // deciding from MethodSymbol state (clauses, resolved return type,
        // returnOwned, modifiers) with no `->decl` dereference.
        // SEAM-DEBT: #4753 AST-gated; answers false for every imported method
        auto* mdA = dynamic_cast<const ast::MethodDecl*>(msA->decl);
        // SEAM-DEBT: #4753 sibling of the read above; same analysis
        auto* mdB = dynamic_cast<const ast::MethodDecl*>(msB->decl);
        if (!mdA || !mdB) return false;
        // Return type must match — the parameter signature is already
        // covered by signaturesIndistinguishable; only the return-type
        // (resolved TypeRef) remains. Two methods declared in the same
        // source have DISTINCT AST pointers for `returnType` (each
        // declaration parses to its own Type AST), so AST-identity is
        // the wrong comparison here — we need resolved-TypeRef equality
        // via typeRefEqual (the §22.6 overload-resolution helper). A
        // null returnType on either side fails the gate (unit-typed
        // members are checked separately and not on this path).
        if ((mdA->returnType == nullptr) != (mdB->returnType == nullptr))
            return false;
        if (mdA->returnType && mdB->returnType) {
            const TypeRef* ra = ctx_.resolvedType(*mdA->returnType);
            const TypeRef* rb = ctx_.resolvedType(*mdB->returnType);
            if (!typeRefEqual(ra, rb)) return false;
        }
        if (mdA->returnOwned != mdB->returnOwned) return false;
        if (mdA->modifiers != mdB->modifiers) return false;
        // The clause lists must NOT be equal — a pair with no clauses
        // and a pair with identical clauses are NOT the clause-only
        // difference case (they are the genuine §22.6/§11.3 path,
        // modifier-only).
        if (mdA->contractClauses.size() != mdB->contractClauses.size())
            return mdA->contractClauses.size() > 0 ||
                   mdB->contractClauses.size() > 0;
        // Same number of clauses: textually compare via the clause's
        // source-spellable kind+expr identity. A simple structural
        // comparison is enough — two clauses are "the same" only when
        // they parsed the same source range; an expr identity mismatch
        // is the clause-only-difference signal.
        for (std::size_t k = 0; k < mdA->contractClauses.size(); ++k) {
            const auto& caA = mdA->contractClauses[k];
            const auto& caB = mdB->contractClauses[k];
            if (caA.kind != caB.kind) return true;
            if (caA.isStatic != caB.isStatic) return true;
            if (caA.expr.get() != caB.expr.get()) return true;
        }
        return false;
    };
    auto checkGroup = [&](std::vector<Overload>& overloads,
                          const std::string& label) {
        for (std::size_t i = 0; i < overloads.size(); ++i) {
            for (std::size_t j = i + 1; j < overloads.size(); ++j) {
                // §11.3 / §14.5 (v0.314, #5364/#5394): two methods
                // with different numbers of type parameters of their own
                // never clash.  `signaturesIndistinguishable` compares
                // parameter types but does not read `genericParams`, so a
                // pair like `M<T>(x: T)` / `M<T,U>(x: T)` or
                // `N(x: int)` / `N<T>(x: int)` was falsely rejected as
                // indistinguishable.  Check generic arity first; differing
                // arity is always a hard distinguish regardless of parameter
                // type equality.
                const auto* msI =
                    dynamic_cast<const MethodSymbol*>(overloads[i].second);
                const auto* msJ =
                    dynamic_cast<const MethodSymbol*>(overloads[j].second);
                if (msI && msJ &&
                    msI->genericParams.size() != msJ->genericParams.size()) {
                    continue;  // distinguishable by generic arity
                }
                if (signaturesIndistinguishable(
                        *overloads[i].first, *overloads[j].first,
                        dynamic_cast<const MemberSymbol*>(overloads[i].second),
                        msI ? msI->genericParams
                            : std::vector<Symbol*>{},
                        msJ ? msJ->genericParams
                            : std::vector<Symbol*>{})) {
                    if (clauseOnlyDifference(overloads[i].second,
                                             overloads[j].second)) {
                        ctx_.diagnostics().report(
                            Severity::Error, overloads[j].second->declLoc,
                            std::string("overload of ") + label +
                            " is the same signature as another overload "
                            "\xe2\x80\x94 contract clauses (`requires`, "
                            "`ensures`) do not distinguish overloads and "
                            "are not part of \xc2\xa7""22.6 resolution; "
                            "two members differing only in clauses "
                            "clash (\xc2\xa7""22.16, v0.250, #3038)");
                    } else {
                        ctx_.diagnostics().report(
                            Severity::Error, overloads[j].second->declLoc,
                            std::string("overload of ") + label +
                            " is indistinguishable from another overload "
                            "\xe2\x80\x94 overloads must differ in "
                            "parameter count, types, or `ref`/`out` "
                            "modifiers, or by ordinary `owned` under "
                            "the declaration guard; `out owned` and "
                            "`scoped` never distinguish. At a "
                            "position whose type fails the guard the "
                            "pair collapses to one signature "
                            "(\xc2\xa7""22.6/\xc2\xa7""11.3, v0.250)");
                    }
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
                                           const std::vector<Symbol*>& b,
                                           const MemberSymbol* enclosing,
                                           const std::vector<Symbol*>& aGenericParams,
                                           const std::vector<Symbol*>& bGenericParams) {
    if (a.size() != b.size()) return false;
    // §11.3 / §14.5 (v0.314, #5364/#5394): when both methods have the same
    // number of type parameters, compare bare type parameters by their
    // POSITION in the generic list, not by `typeRefEqual` (which equates all
    // declaration-less bare type parameters).  If at any parameter position the
    // bare type parameters have different positions in their respective generic
    // lists, the pair is distinguishable (no clash).
    const bool doPositionalCheck =
        !aGenericParams.empty() &&
        aGenericParams.size() == bGenericParams.size();
    // Helper: returns the position of a bare type parameter `paramType` in
    // `genericParams`, or -1 if `paramType` is not a bare type parameter.
    auto getTypeParamPosition =
        [&](const TypeRef* paramType,
            const std::vector<Symbol*>& genericParams) -> int {
        if (!paramType || paramType->kind != TypeRef::Kind::Named ||
            paramType->declaration != nullptr) {
            return -1;  // not a bare type parameter
        }
        if (!paramType->astNode || !paramType->astNode->name ||
            paramType->astNode->name->parts.empty()) {
            return -1;
        }
        std::string_view name = paramType->astNode->name->parts[0];
        for (std::size_t k = 0; k < genericParams.size(); ++k) {
            if (auto* gps =
                    dynamic_cast<GenericParamSymbol*>(genericParams[k])) {
                if (gps->name == name) return static_cast<int>(k);
            }
        }
        return -1;  // not found (should not happen for a valid bare T ref)
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto* pa = dynamic_cast<ParameterSymbol*>(a[i]);
        auto* pb = dynamic_cast<ParameterSymbol*>(b[i]);
        if (!pa || !pb) return false;
        // §11.3 / §14.5 (v0.314): positional correspondence check for
        // same-count generics.  If both parameter types are bare type
        // parameters (Kind::Named, declaration == nullptr), compare their
        // positions in their method's own generic parameter list.  Different
        // positions means the parameter types do NOT correspond positionally
        // and the pair is distinguishable.
        if (doPositionalCheck) {
            TypeRef* ta =
                pa->declType ? ctx_.resolvedType(*pa->declType) : nullptr;
            TypeRef* tb =
                pb->declType ? ctx_.resolvedType(*pb->declType) : nullptr;
            int posA = getTypeParamPosition(ta, aGenericParams);
            int posB = getTypeParamPosition(tb, bGenericParams);
            if (posA >= 0 && posB >= 0 && posA != posB) {
                return false;  // distinguishable: bare TPs at different positions
            }
        }
        std::string_view ma = overloadModifier(pa->modifier);
        std::string_view mb = overloadModifier(pb->modifier);
        if (ma != mb) {
            // §22.6 (v0.250, proposal 0091): the only modifier-pair that
            // stays *indistinguishable* on equal types but for which the
            // declaration guard may rescue distinguishability is the
            // ordinary-`owned` vs plain pair (one key "value", the other
            // "owned_value"). Every other modifier difference is a hard
            // distinguish — the kinds differ outright.
            bool ownedOnlyDiff =
                (ma == "value" && mb == "owned_value") ||
                (ma == "owned_value" && mb == "value");
            if (!ownedOnlyDiff) return false;
            // Ordinary-`owned` difference: check the §22.6 declaration
            // guard at this position. A TypeRef that passes the guard
            // (class type / class-only-interface / qualifying type
            // parameter) makes the pair DISTINGUISHABLE at this position
            // — return false. A failing position lets the pair collapse
            // here; fall through so the loop continues to the next
            // position (and ultimately to the per-position type check
            // below).
            TypeRef* ta = pa->declType ? ctx_.resolvedType(*pa->declType)
                                       : nullptr;
            if (positionTypePassesDeclarationGuard(ta, pa, enclosing)) {
                return false;
            }
            // Guard failed at this position — the pair still collides
            // here; re-equalise the keys so the rest of the loop runs
            // unchanged (every position must pass the guard for the
            // pair to be admitted).
            (void)0;
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
    // Nested types carry the same A.3 `Modifier*` production (§11.6 reuses the
    // type-declaration productions), so they are checked identically — at every
    // depth, per fix #3574's shared walk.
    forEachTypeAndNested(
        [&](TypeSymbol* t) { checkTypeModifierCombinations(t); });
}

void Resolver::checkTypeModifierCombinations(TypeSymbol* t) {
    // §11.7 / A.4: `closed` is a class-shape modifier, never a member one.
    // The parser collects it in the shared member-modifier prefix so a NESTED
    // type can carry it (Parser_Decls.cpp); on any other member kind it is
    // meaningless — the posture §11.5 states for `sealed` ("`sealed` alone is
    // a class-level modifier … it does not appear on individual members").
    // Runs BEFORE the type-kind gate below, so a member of a record/enum/
    // interface/choice is checked too.
    for (Symbol* m : t->members) {
        if (dynamic_cast<NestedTypeSymbol*>(m)) continue;
        auto* mem = dynamic_cast<MemberSymbol*>(m);
        if (!mem) continue;
        for (auto mod : mem->modifiers) {
            if (mod != "closed") continue;
            ctx_.diagnostics().report(
                Severity::Error, m->declLoc,
                std::string("member `") + std::string(mem->name) +
                "` may not be `closed` — `closed` is a class-level modifier "
                "(§11.7); it does not appear on individual members");
            break;
        }
    }
    // §11.7 / A.3 (v0.242, #3052): `closed` is defined for a CLASS. The
    // grammar's `Modifier*` is shared by every type-declaration production
    // (and, for a nested type, by the member-modifier prefix), so `closed
    // record R`, `closed enum E`, `closed interface I`, and `closed choice C`
    // all parse; none has a meaning. Recorded interpretation (CLAUDE.md rule
    // 4): reject, naming §11.7 — the alternative, letting the modifier ride
    // along inertly, would make `new R()` fail on a record with a diagnostic
    // about a class rule. This mirrors the same over-admit-and-reject split
    // the `abstract record` rejection below uses (#2431).
    if (t->kind != ast::TypeDeclKind::Class) {
        if (typeIsClosed(t)) {
            const char* kindWord =
                t->kind == ast::TypeDeclKind::Record    ? "record"
                : t->kind == ast::TypeDeclKind::Enum    ? "enum"
                : t->kind == ast::TypeDeclKind::Choice  ? "choice"
                                                        : "interface";
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string(kindWord) + " `" + std::string(t->name) +
                "` may not be `closed` — `closed` is a class modifier "
                "(§11.7); it declares a closed abstract CLASS hierarchy");
        }
        return;
    }
    bool isStatic = false, isAbstract = false, isSealed = false;
    for (auto mod : t->modifiers) {
        if (mod == "static") isStatic = true;
        else if (mod == "abstract") isAbstract = true;
        else if (mod == "sealed") isSealed = true;
    }
    const bool isClosed = typeIsClosed(t);
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
    // §11.7 (v0.242, #3052), verbatim: "(Through v0.241 the semantics were
    // spelled as the modifier pair `sealed abstract`; that spelling is
    // rejected, the diagnostic naming `closed`.)" Appendix C.1 carries the
    // same invariant. The pair gets its own fix-it-shaped diagnostic — it
    // prints the replacement declaration — because it is the MIGRATION case:
    // an author who writes it means exactly `closed`, unlike the three
    // `closed`+X combinations below, which are ordinary contradictions.
    if (isSealed && isAbstract && !isClosed) {
        ctx_.diagnostics().report(
            Severity::Error, t->declLoc,
            std::string("class `") + std::string(t->name) +
            "` may not be both `sealed` and `abstract` — the closed abstract "
            "class is spelled with the dedicated modifier: write `closed "
            "class " + std::string(t->name) + "` (§11.7)");
    }
    // §11.7 (v0.242, #3052), verbatim: "combining `closed` with `abstract`,
    // `sealed`, or `static` is a compile error."
    if (isClosed) {
        const char* other = isAbstract ? "abstract"
                          : isSealed   ? "sealed"
                          : isStatic   ? "static"
                                       : nullptr;
        if (other) {
            ctx_.diagnostics().report(
                Severity::Error, t->declLoc,
                std::string("class `") + std::string(t->name) +
                "` may not be both `closed` and `" + other +
                "` — `closed` already means \"abstract and inheritable only "
                "within the declaring module\" (§11.7)");
        }
    }
}

// fix #3100 (§19.6 / §11.7, v0.254): a `closed` class's cross-module
// exhaustiveness guarantee ("exhaustiveness holds across module
// boundaries", §19.6 last sentence) requires the COMPLETE leaf set to be
// visible to the importer — and `internal` classes are NOT exported
// (ModuleMetadataBuilder.cpp:2112, "§13.2 internal types not exported").
// A `closed` root with a non-public CONCRETE subclass therefore has an
// importer-invisible leaf: the importer's caseArmsExhaustive walk proves
// exhaustiveness against an incomplete leaf set and accepts non-coverage
// that the runtime can still dispatch into (the leaf's `new` is reachable
// via the declaring module's own static factory, e.g. `Factory.Make()`,
// and §2.8.3 rule 4 holds the runtime type identity across artifacts).
// At runtime the `case` raises MatchFailureException (§21.7) — a silent
// wrong behavior the static proof should have caught.
//
// The only sound posture: the cross-module guarantee holds for a `closed`
// root only when every concrete leaf of its hierarchy is public, because
// only public leaves ride the metadata (`isExportedType`, ModuleMetadata-
// Builder.cpp:41) into the importer's symbol table. Reject the shape at
// the declaration site so the dependent module does not produce an
// artifact whose cross-module guarantee is unsound — the importer never
// sees a closed root over a leaf set it cannot read, and the
// `caseArmsExhaustive` gate's `isClosedRoot` arm keeps its §19.6 promise.
//
// §13.2's "top-level types may be `public` or `internal`" framing means
// an `internal` `closed` root is fine (no other module can name it, so
// the cross-module guarantee is moot); the check is restricted to PUBLIC
// closed roots whose concrete leaves are not all public. An abstract or
// `closed` intermediate is skipped because it has no instances (§11.7,
// §12.5) and cannot itself be a runtime value of the closed type.
void Resolver::checkClosedHierarchyLeafVisibility() {
    if (!ctx_.module()) return;
    // Collect the public closed-class roots declared in this module —
    // an internal `closed` root cannot be named cross-module (§13.2) so
    // its leaf set is local-only and the cross-module guarantee does not
    // apply. (`isExportedType` requires Public; an internal root is not
    // exported, period.)
    //
    // fix #3637 (§19.6 / §11.7 / §11.6, v0.286): the root collection used
    // to walk only `ctx_.module()->children` (top-level only), so a `closed`
    // root nested inside a public class was silently skipped -- the WHOLE
    // §19.6 leaf-visibility check below ran against an empty root set and
    // produced no diagnostic, letting a concrete `internal` leaf escape.
    // §11.6 states "Nested types in records are permitted but uncommon" and
    // nested classes are equally legal (the canonical example: a public
    // outer class owning a public `closed` root that drives a cross-module
    // `case`); the root-collection half of #3599 was explicitly deferred
    // (cbb7be5's body says "the closed-ROOT collection ... stays restricted
    // to top-level public classes -- that is a separate, narrower question
    // from the leaf side this issue is about, and is flagged as issue
    // 3599's follow-up rather than folded in here"). Routing through the
    // shared recursive walker (fix #3574, `forEachTypeAndNested`) and
    // filtering to `kind == Class && typeIsClosed && visibility == Public`
    // inside the callback mirrors fix #3599's leaf-side shape exactly.
    std::vector<TypeSymbol*> closedRoots;
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind != ast::TypeDeclKind::Class) return;
        if (!typeIsClosed(t)) return;
        if (t->visibility != ast::Visibility::Public) return;
        closedRoots.push_back(t);
    });
    if (closedRoots.empty()) return;
    // Enumerate every class declared in this module (a closed root's leaf
    // set is local to its declaring module, §11.7; cross-module subclasses
    // are rejected at the base's inheritance site, TypeResolution_Names.
    // cpp:717). Nested classes are included — a nested class can extend a
    // top-level `closed` root, at ANY nesting depth and through nested types
    // of ANY kind: §11.6 states "Nested types in records are permitted but
    // uncommon", so a class may be declared inside a nested record (or
    // interface / enum / choice), and such a class is a leaf just like one
    // declared directly inside a class.
    // fix #3599 (§19.6 / §11.7 / §11.6, v0.286): this used to be a
    // hand-rolled `collectNested` that stopped DESCENDING as soon as it met a
    // non-class nested type, so a class nested inside a nested record was
    // silently excluded from `localClasses` -- the §19.6 cross-module
    // exhaustiveness guard never saw it, letting a concrete `internal` leaf
    // of a public `closed` root escape the guard entirely (issue 3599's
    // repro). Routing through the shared recursive walker (fix #3574,
    // forEachTypeAndNested) and filtering to `kind == Class` inside the
    // callback closes that hole: the walker DESCENDS through nested types of
    // every kind unconditionally and only the PUSH is filtered, matching the
    // shape `collectHidingCheckClasses` already used.
    std::vector<TypeSymbol*> localClasses;
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind == ast::TypeDeclKind::Class) localClasses.push_back(t);
    });
    // For each public closed root, walk every concrete (non-abstract,
    // non-`closed`) descendant in this module and check visibility.
    // Abstract and `closed` intermediates have no instances and need no
    // leaf — they are matched only through their concrete descendants
    // (§19.6 verbatim). The same shape `caseArmsExhaustive`'s
    // `tsInstanceless` check uses (ExprTypecheck.cpp:2213-2214) — local
    // re-derivation rather than a shared helper because the check runs
    // in a different pass with different reachability.
    for (TypeSymbol* root : closedRoots) {
        for (TypeSymbol* ts : localClasses) {
            if (ts == root) continue;
            // Is `root` an ancestor of `ts`?
            bool isDescendant = false;
            std::vector<TypeSymbol*> work = ts->baseTypes;
            std::unordered_set<TypeSymbol*> seen;
            while (!work.empty()) {
                TypeSymbol* b = work.back();
                work.pop_back();
                if (!b || !seen.insert(b).second) continue;
                if (b == root) { isDescendant = true; break; }
                for (TypeSymbol* bb : b->baseTypes) work.push_back(bb);
            }
            if (!isDescendant) continue;
            // Skip abstract and `closed` intermediates (no instances).
            const bool tsInstanceless =
                typeIsClosed(ts) ||
                std::find(ts->modifiers.begin(), ts->modifiers.end(),
                           std::string_view("abstract")) != ts->modifiers.end();
            if (tsInstanceless) continue;
            // Concrete leaf — visibility MUST be Public (§13.2, the only
            // visibility that survives `isExportedType`).
            if (ts->visibility != ast::Visibility::Public) {
                ctx_.diagnostics().report(
                    Severity::Error, ts->declLoc,
                    std::string("class `") + std::string(ts->name) +
                    "` is a concrete subclass of `closed` class `" +
                    std::string(root->name) +
                    "` but is not `public` — a `closed` hierarchy's "
                    "cross-module exhaustiveness guarantee (§19.6) "
                    "requires every concrete leaf to be exported (§13.2), "
                    "and `internal` classes are not; promote `" +
                    std::string(ts->name) + "` to `public` or remove "
                    "`closed` from `" + std::string(root->name) + "` "
                    "(§11.7)");
            }
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

// §11.7 (Static classes — inheritance and interface implementation):
// "A static class may not be inherited from and does not implement
// interfaces." The parser admits any `Modifier*` set and any `:`-list
// of base types per A.3 / A.4 (the grammar over-admits, sema
// narrows), so `public static class Utils: Base` and `public static
// class Utils: IMarker` both parse and would compile to an executable
// before this check. Reject each base type at the use site: a CLASS
// base reads as `inherited from another class`; an INTERFACE base reads
// as `implements an interface`. A static class is uninstantiable
// (§11.7's `new`-rejection arm at ExprTypecheck_Exprs.cpp:1208), so an
// inherited slot or interface contract has nothing to dispatch into.
//
// Walks this module's own local declaration tree only — same reach as
// every other §11.7-shaped check (`checkStaticClassMembers` and the
// #3574 recursive walker), so an imported type's shape is whatever its
// exporting module's compile produced.
//
// `t->baseTypes` is the resolved list (parallel to `t->baseTypeRefs`,
// one entry per InheritItem). When a base resolved (a built-in like
// `Object` does), its declaration may be null — but the use site
// location is the position the diagnostic wants anyway, and that comes
// from `baseTypeRefs[i]->astNode->span.begin` when present (set by the
// parser; reconstructable metadata would surface a synthetic location
// but `astNode` is only populated for locally-parsed bases).
void Resolver::checkStaticClassInheritance() {
    if (!ctx_.module()) return;
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind != ast::TypeDeclKind::Class) return;
        bool isStatic = false;
        for (auto mod : t->modifiers) {
            if (mod == "static") { isStatic = true; break; }
        }
        if (!isStatic) return;
        for (std::size_t bi = 0; bi < t->baseTypes.size(); ++bi) {
            TypeSymbol* base = t->baseTypes[bi];
            if (!base) continue;  // built-in / unresolved — already reported
            const SourceLocation useSite =
                (bi < t->baseTypeRefs.size() && t->baseTypeRefs[bi] &&
                 t->baseTypeRefs[bi]->astNode)
                    ? t->baseTypeRefs[bi]->astNode->span.begin
                    : t->declLoc;
            if (base->kind == ast::TypeDeclKind::Interface) {
                ctx_.diagnostics().report(
                    Severity::Error, useSite,
                    std::string("static class `") + std::string(t->name) +
                    "` may not implement interface `" +
                    std::string(base->name) +
                    "` — a static class may not be inherited from and does "
                    "not implement interfaces (§11.7)");
            } else {
                ctx_.diagnostics().report(
                    Severity::Error, useSite,
                    std::string("static class `") + std::string(t->name) +
                    "` may not be inherited from `" +
                    std::string(base->name) +
                    "` — a static class may not be inherited from and does "
                    "not implement interfaces (§11.7)");
            }
        }
    });
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

// fix #2758 (§23.5, v0.241): the DECLARATION-SIDE half of the same §23.5
// bullet the index-argument rule lives in — "Indexer parameters may not
// declare default values, so §22.5's optional-parameter and named-binding
// machinery is never reached from an index position".
//
// This is the premise the positional-only rule rests on: with no defaults an
// index list can never be short, so there is nothing for a name to bind
// out-of-order to. A.4's IndexerDecl comment records the over-admission in
// the same shape as the `static` one directly above: "Parameter default
// values (`:=` in the shared ParamList) are likewise over-admitted and
// rejected: indexer parameters may not declare defaults (§23.5)."
//
// `Parser::parseParamList` is shared with methods and primary constructors,
// so `public property [i: int := 0]: int` PARSES. Measured on 309356e it
// compiled clean and the default was simply unreachable — no call site can
// omit an index (`q[]` does not parse), and no consumer of `indexerParams`
// reads `defaultValue`. Rejecting it at the declaration is bug-revealing:
// the tree contains no such declaration, in `src/`, `tests/`, or `docs/`.
void Resolver::checkIndexerParamDefaults() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        for (Symbol* m : t->members) {
            auto* ps = dynamic_cast<PropertySymbol*>(m);
            if (!ps || !ps->isIndexer) continue;
            // The AST is the only place a default VALUE survives: the
            // reconstructed ParameterSymbols an IMPORTED indexer carries do
            // not record one, so an imported declaration is skipped here and
            // caught in its own module.
            // SEAM-ACCESSOR: `ps` is a member of `t`, which checkIndexerParamDefaults draws only
            // from `ctx_.module()->children`. An imported indexer's default-value
            // syntax is not reconstructed (see the comment above) and is instead
            // caught in ITS OWN module's compilation, where it was local (#3152).
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd) continue;
            for (const auto& p : pd->indexerParams) {
                if (!p.defaultValue) continue;
                ctx_.diagnostics().report(
                    Severity::Error, p.loc,
                    "indexer parameter `" + std::string(p.name) +
                    "` may not declare a default value — an index list can "
                    "never be short (there is no argument-less `[]` form) and "
                    "index arguments are positional-only, so §22.5's "
                    "optional-parameter and named-binding machinery is never "
                    "reached from an index position; remove the `:= …` "
                    "(§23.5)");
            }
        }
    }
}

// fix #4494 (§22.5 "Defaults and modifiers", v0.306). The paragraph's closing
// sentence, verbatim: "Every parameter after the first defaulted parameter must
// itself have a default."
//
// It was unenforced. MEASURED on 3975958e: `class B(n: int := 41, s: string)`
// with `new B(1, "ab")` compiled at exit 0 with no diagnostic, and the program
// ran and returned 3.
//
// WHY IT MATTERS BEYOND TIDINESS. A parameter that can never be omitted sitting
// BEHIND one that can makes the earlier one's default unreachable by any
// positional call — it is reachable only by NAMING, which is precisely the
// machinery §22.5 is about. The §22.5 binder already DEPENDS on the rule:
// `sema::FormalsView::ofNames` represents a built-in row's optionality as a
// single "how many leading parameters are required" count and says so in as
// many words ("the registry's rows put every defaulted parameter LAST … §22.5
// requires it"), a shape that cannot express an interleaved list at all. So the
// gap admitted declarations one of the binder's five formal sources
// structurally cannot represent.
//
// THE RULE IS READ THROUGH `sema::FormalsView`, deliberately. Its two facts are
// exactly `name(i)` and `hasDefault(i)`, which is the same view and the same
// pair of accessors `bindArgsToParams` runs on — so the declaration check and
// the call-site binder can never disagree about which parameters are optional,
// and the AST-vs-imported split stays inside `formalParam*`'s sanctioned
// accessors (SymbolCollection.cpp) instead of being re-spelled here as a
// `dynamic_cast<const ast::MethodDecl*>(ms->decl)` family, which would be four
// new #3152 imported-symbol seam reads for a fact the symbol already carries.
//
// WHAT THIS COVERS: every LOCAL formal list that can carry a `:= Expr` — a
// method and an operator (both are a `MethodSymbol`, and `FormalsView::of`
// resolves the `ast::MethodDecl` / `ast::OperatorDecl` split through
// `methodDeclParams`), a constructor, and a §22.10 primary-constructor
// parameter list (the positional-field view) — at every nesting depth
// (`visitTypeAndNested`, #3574/#3597) and inside extensions (`ExtensionSymbol`,
// which `forEachTypeAndNested` deliberately does not visit; see
// NestedTypeWalk.hpp, so the extension arm is walked explicitly here).
//
// WHAT IT DOES NOT, each with its reason:
//   * IMPORTED declarations. The rule is a property of the DECLARATION, so it
//     is caught when the declaring module is compiled. This walk starts from
//     `ctx_.module()->children`, which is the local module — the same scoping
//     `checkIndexerParamDefaults` uses, for the same reason.
//   * Indexer parameter lists. §23.5 forbids a default on ANY indexer
//     parameter and `checkIndexerParamDefaults` (a few hundred lines above)
//     already reports it, so the ordering rule can never be the first thing
//     wrong with one; adding it here would only double-report. An indexer is a
//     `PropertySymbol`, so the member walk below skips it structurally rather
//     than by a special case.
//   * Record positional fields. A.3 `PositionalField` admits no default syntax
//     at all (the parser routes records through `parsePositionalFieldList`), so
//     the rule is vacuous there — the walk still visits them and finds
//     nothing, rather than special-casing, which a control row pins.
//   * Lambda parameters (§27.4). `parseLambdaParam` parses no `:=` default, so
//     no lambda formal list can violate the rule.
//   * `extern` declarations (§32). They parse through the same
//     `parseParamList` and so CAN syntactically carry a default; whether §32
//     admits one at all is a separate question this fix does not answer, and
//     answering it here would be a second new rejection smuggled into a
//     one-sentence fix.
//
// ONE diagnostic per list, at the FIRST offending parameter: the rule names a
// single boundary ("the first defaulted parameter"), so a list with three
// offenders has one defect, not three.
// // per §22.5, fix #4494
void Resolver::checkParamListDefaultOrdering(
    sema::FormalsView params, const std::vector<SourceLocation>& locs,
    const std::string& what, SourceLocation fallback) {
    std::size_t firstDefaulted = params.size();
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params.hasDefault(i)) {
            if (firstDefaulted == params.size()) firstDefaulted = i;
            continue;
        }
        // `>= i` and not `> i`: the FIRST defaulted parameter is not "after"
        // itself, so slot `firstDefaulted` must never report against itself.
        if (firstDefaulted >= i) continue;
        SourceLocation at = (i < locs.size() && locs[i].line != 0) ? locs[i]
                                                                  : fallback;
        ctx_.diagnostics().report(
            Severity::Error, at,
            what + ": parameter `" + std::string(params.name(i)) +
                "` declares no default but follows defaulted parameter `" +
                std::string(params.name(firstDefaulted)) +
                "` — every parameter after the first defaulted parameter must "
                "itself have a default (§22.5); give it a default, or move it "
                "ahead of `" +
                std::string(params.name(firstDefaulted)) + "`");
        return;
    }
}

void Resolver::checkParamDefaultOrdering() {
    if (!ctx_.module()) return;
    // Per-parameter locations, symbol-side. `ParameterSymbol::declLoc` is
    // stamped from `ast::Param::loc` by `SymbolCollection::collectParams`, and
    // a §22.10 positional field's is on its `posParam` — both are members of
    // the SYMBOL, so a precise diagnostic costs no AST classification read.
    auto paramLocs = [](const std::vector<Symbol*>& ps) {
        std::vector<SourceLocation> out;
        out.reserve(ps.size());
        for (Symbol* p : ps) out.push_back(p ? p->declLoc : SourceLocation{});
        return out;
    };
    auto fieldLocs = [](const std::vector<const Symbol*>& fs) {
        std::vector<SourceLocation> out;
        out.reserve(fs.size());
        for (const Symbol* f : fs) {
            const auto* fld = dynamic_cast<const FieldSymbol*>(f);
            out.push_back(fld && fld->posParam ? fld->posParam->loc
                                               : SourceLocation{});
        }
        return out;
    };
    auto checkMembers = [&](const std::vector<Symbol*>& members,
                            const std::string& ownerDesc) {
        for (Symbol* m : members) {
            if (auto* ms = dynamic_cast<MethodSymbol*>(m)) {
                const std::string what =
                    (ms->isOperator
                         ? "operator `" + std::string(ms->operatorToken) + "`"
                         : "method `" + std::string(ms->name) + "`") +
                    ownerDesc;
                checkParamListDefaultOrdering(sema::FormalsView::of(*ms),
                                              paramLocs(ms->params), what,
                                              m->declLoc);
            } else if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
                checkParamListDefaultOrdering(sema::FormalsView::of(*cs),
                                              paramLocs(cs->params),
                                              "constructor" + ownerDesc,
                                              m->declLoc);
            }
        }
    };
    for (Symbol* child : ctx_.module()->children) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child)) {
            visitTypeAndNested(t, [&](TypeSymbol* ts) {
                if (!ts) return;
                const std::string owner = " of `" + std::string(ts->name) + "`";
                if (ts->hasPositionalFields) {
                    const std::vector<const Symbol*> fields =
                        recordPositionalFieldSymbols(ts);
                    if (!fields.empty())
                        checkParamListDefaultOrdering(
                            sema::FormalsView::of(fields), fieldLocs(fields),
                            "primary constructor" + owner, ts->declLoc);
                }
                checkMembers(ts->members, owner);
            });
        } else if (auto* ex = dynamic_cast<ExtensionSymbol*>(child)) {
            checkMembers(ex->members,
                         " of extension `" + std::string(ex->name) + "`");
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
            // §25.1 (#3107): the assignment and compound-assignment tokens
            // `:=`, `+=`, `-=`, `*=`, `/=`, `mod=`, `&=`, `|=`, `^=`, `<<=`,
            // `>>=`, `??=` are not user-overloadable — the compound forms
            // desugar mechanically to read-modify-write using the underlying
            // binary operator's overload (§25.1, §6.6). They were silently
            // accepted as overload declarations and the declared members were
            // inert, so the author got a confusing "you declared no
            // `operator +`" message while staring at the `operator +=` they
            // just wrote.
            if (tok == ":=" || tok == "+=" || tok == "-=" || tok == "*=" ||
                tok == "/=" || tok == "mod=" || tok == "&=" || tok == "|=" ||
                tok == "^=" || tok == "<<=" || tok == ">>=" || tok == "??=") {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "operator `" + std::string(tok) +
                    "` may not be overloaded (§25.1) — declare the underlying "
                    "binary operator; the compound form composes from it "
                    "automatically");
                continue;
            }
            // fix #3978 (§25.1 / §17.6, v0.295): `++`/`--` are not in Appendix
            // A.4's `OpToken` production at all (§25.1, verbatim: "`++` and
            // `--` are not an `OpToken`, so such a declaration is a syntax
            // error at the token"). The parser does not validate a declared
            // operator's token against the grammar's closed set
            // (`parseOperatorDeclBody` consumes whatever token follows
            // `operator` unconditionally), so `operator ++`/`operator --`
            // were silently accepted as ordinary overloads and dispatched
            // through at both the codegen `opMethod` path and the sema
            // `IncrementStmt` "does the type overload it" check below — the
            // retired v0.236 reading. §25.1's own not-overloadable list,
            // verbatim: "The increment and decrement statements `++` and
            // `--` — built-in statements over the operand types §6.6's
            // compound form admits (§17.6), not operators of the expression
            // grammar; a type spells advancement as `x := x + 1` under its
            // own `operator +`, or as a named method." Diagnosed here
            // (alongside `not`/`!`/`??`/... and the compound-assignment
            // tokens above) rather than as a parser syntax error: this is
            // already the one place that validates a declared token against
            // §25.1's exclusion list post-parse, which keeps the diagnostic
            // able to name the declaring type and match its neighbors'
            // remedy wording.
            if (tok == "++" || tok == "--") {
                ctx_.diagnostics().report(
                    Severity::Error, m->declLoc,
                    "operator `" + std::string(tok) +
                    "` may not be overloaded (§25.1) — `++`/`--` are "
                    "built-in statements over the operand types §6.6's "
                    "compound form admits (§17.6), not operators of the "
                    "expression grammar; a type spells advancement as "
                    "`x := x + 1` under its own `operator +`, or as a "
                    "named method");
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

// §25.2 (v0.158, #1338; enum spelling corrected v0.183, #1381/#1582; choices
// added v0.276, #3333): "A record, enum, or choice that overloads `=`/`<>`
// with both operands of the declaring type must also declare `Equals(T)` and
// — on a record, `override GetHashCode`; on an enum, a plain `GetHashCode(): int`
// (§10.5, v0.288, #3837: `override` is ALSO admitted on an enum's/choice's
// `GetHashCode` now, but the plain spelling remains fully sufficient here and
// is what this diagnostic recommends); on a choice, the enum spelling, §35.2
// carrying §10.5's plain-member replacement rule over — the same discipline,
// and the same diagnostic style, as §8.3's Equals→GetHashCode pairing." The
// three surfaces (`=`, `Equals`, hashing) can otherwise silently diverge
// exactly where it hurts: `a = b` true while a `Set<T>` treats them as
// distinct. Heterogeneous-operand overloads (`T = U`) carry no such obligation.
//
// Per v0.275/v0.276: there is no exemption for compiler-intrinsic types.
// `IntPtr`/`UIntPtr` (stdlib §10.0) declare the four members and satisfy the
// rule on their own merits; the date-scoped exemption they formerly carried
// was retired (the *predate* ground was refuted by #3322: rule at v0.158,
// `Ptr<T>` at v0.186, twenty-eight revisions later). The check therefore runs
// against every record/enum/choice and is keyed on the *declaration*, not on
// the type's name — a user type named `IntPtr` is subject to the same rule.
//
// v0.183 fix: the diagnostic text below previously said `` `override
// GetHashCode` `` unconditionally, including for enums — but §10.5 (pre-v0.288)
// REJECTED `override` on any enum method, so an enum author following that
// diagnostic literally hit a second compile error. The message now spells the
// requirement the way each `t->kind` actually accepts it. v0.276 widens the
// kind set to choices and uses the enum spelling on them (§35.2 carries
// §10.5's plain-member replacement rule over to choices). v0.288 (#3837) then
// admitted `override` on this same trio for enums/choices too (checkEnumRestrictions
// / checkChoiceDispatchModifiers) — that widened what the AUTHOR may write,
// not what this diagnostic RECOMMENDS: the plain spelling is still legal and
// still the simpler answer, so the message is unchanged.
//
// #3333 dependency note: §35.2 carries the rule over "for the boxable
// classifications"; an owning choice's ability to declare `Equals(T)`/
// `GetHashCode` is the open question tracked as #3335. Under the likely
// reading (§35.2's clause is about `Object`-virtual replacement, not about
// declaring plain members) the obligation is dischargeable on every
// classification and this check is correct as written — the diagnostic
// text is uniform. The fix takes that reading; #3335 settling the other way
// would add a classification guard here, not a different message.
void Resolver::checkEqualityCoDeclaration() {
    if (!ctx_.module()) return;
    for (Symbol* child : ctx_.module()->children) {
        auto* t = dynamic_cast<TypeSymbol*>(child);
        if (!t) continue;
        if (t->kind != ast::TypeDeclKind::Record &&
            t->kind != ast::TypeDeclKind::Enum &&
            t->kind != ast::TypeDeclKind::Choice)
            continue;

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
            // §10.5 + §35.2: a plain `GetHashCode(): int` is always an
            // accepted spelling on an enum/choice (v0.183 fix for enums,
            // #1381/#1582; v0.276 widening for choices, #3333) — v0.288
            // (#3837) additionally admits `override` on this same member,
            // but the plain form recommended below remains sufficient, so
            // the message is unchanged.
            bool isRecord = t->kind == ast::TypeDeclKind::Record;
            std::string kind =
                isRecord ? "a record" :
                (t->kind == ast::TypeDeclKind::Enum ? "an enum" : "a choice");
            std::string hashSpelling =
                isRecord ? "`override GetHashCode`" : "`GetHashCode`";
            ctx_.diagnostics().report(
                Severity::Error, homoOp->declLoc,
                kind + " that overloads `=` must also declare `Equals(T)` and "
                + hashSpelling + " (\xC2\xA7""25.2)");
        }
    }
}

// §3.6 / §8.3 (v0.158, #1338): a `Dictionary<K,V>` / `OwningDictionary<K,V>` key
// (K) and a `Set<T>` / `OwningSet<T>` element (T) must be **hashable**.
// The sites are recorded during name resolution (resolveNamed) and validated
// here — after every member type is resolved — because a record key's
// auto-hashable property depends on the record's own members being
// resolved, which may happen after the container annotation is visited (§3.6
// rule 2: per-instantiation, at the instantiation site).
//
// #1384: a second pass handles sites where the key was a generic type parameter
// (conservatively accepted in the generic body — no constraint surface exists to
// require hashable). When the enclosing generic is concretely
// instantiated with a non-hashable type, the per-instantiation check
// fires at the hash-container annotation site (§3.6 rule 2).
void Resolver::checkHashContainerKeys() {
    std::unordered_set<const ast::Type*> reported;

    // §3.6/§8.3: the two non-hashable shapes are (a) a record that
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
                // Value-like yet not hashable: the RECURSIVE §35.7
                // rule rejected a case payload (function/Object/etc.).
                return "it is a choice that is value-like but has a case "
                       "payload outside the \xC2\xA7""9.2 admissible-component "
                       "rule (\xC2\xA7""35.7), so it is not "
                       "auto-hashable. Restructure the payload, or "
                       "use a hashable key type.";
            const char* cat =
                (v && *v == common::ChoiceClassification::Owning)
                    ? "owning"
                    : "borrow-bearing";
            return std::string("it is a choice that is ") + cat +
                   " (\xC2\xA7""35.7), not value-like auto-hashable. "
                   "Restructure it to a value-like choice, or declare custom "
                   "equality.";
        }
        return "it is a record that is neither auto-hashable "
               "(\xC2\xA7""9.2) nor declares custom equality (`operator =`/`<>` "
               "with `Equals(T)` and `override GetHashCode`, \xC2\xA7""25.2). "
               "Give it an auto-hashable form, or declare custom "
               "equality.";
    };

    // Pass 1: direct (non-generic-param) key check (the original #1338 logic).
    for (const HashContainerSite& s : hashContainerSites_) {
        if (isEqualityBearing(s.keyRef, ctx_)) continue;
        if (s.site && !reported.insert(s.site).second) continue;  // dedup per site
        const TypeSymbol* rec = s.keyRef ? s.keyRef->declaration : nullptr;
        std::string keyName =
            rec ? ("`" + std::string(rec->name) + "`") : std::string("it");
        // fix #3796 (§3.6 / §8.3): prefer the offending TYPE-ARGUMENT's source
        // location over the enclosing annotation site's. The annotation may
        // sit inside a typed-local or a `new …()` whose Name* AST node has
        // spans populated by the lexer's per-token stamping (paths that flow
        // through parseExpression's stampSpan wrap); the type-argument's AST
        // node carries the more precise location the user actually needs to
        // jump to in an IDE. Falls back to `s.site` only if the type argument
        // has no AST node (an imported type whose TypeRef carries no AST).
        SourceLocation diagLoc;
        if (s.keyRef && s.keyRef->astNode &&
            s.keyRef->astNode->span.begin.line != 0)
            diagLoc = s.keyRef->astNode->span.begin;
        else if (s.site)
            diagLoc = s.site->span.begin;
        ctx_.diagnostics().report(
            Severity::Error, diagLoc,
            std::string("`") + std::string(s.container) +
            "` requires a hashable " + std::string(s.property) +
            " type, but the type argument " + keyName +
            " is not hashable (\xC2\xA7""3.6/\xC2\xA7""8.3): " +
            reasonClause(s.keyRef));
    }

    // Pass 2 (#1384): per-instantiation check for sites whose key is a generic
    // type parameter. isEqualityBearing returns true conservatively for unresolved
    // generic params (no concrete type is known at the declaration site), so those
    // sites pass pass 1. Here we walk concrete instantiations of the enclosing
    // type to check whether the bound type is hashable.
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
        // SEAM-ACCESSOR: already explicitly gated one line above
        // (`if (!enc || !enc->decl) return false;`); returning `false` here
        // means "no matching `hashable` constraint found", which does NOT
        // suppress the §3.6 rule-2 check below — it makes the check fire
        // unconditionally, the fail-SAFE direction (#3152).
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
            // Concrete key is not hashable — report at the hash-container
            // annotation site (§3.6 rule 2 per-instantiation discipline).
            if (s.site && !reported2.insert(s.site).second) continue;
            const TypeSymbol* rec = concreteKey->declaration;
            std::string keyName =
                rec ? ("`" + std::string(rec->name) + "`") : std::string("it");
            // fix #3796 (§3.6 / §8.3): prefer the offending concrete
            // type-argument's source location (`inst.argAsts[paramIdx]`) over
            // the interior annotation site — the §3.6 rule-2 per-instantiation
            // discipline wants the diagnostic at the instantiation site, which
            // for `Holder` in `new Registry2<Holder>()` is the inner type
            // argument's AST span, not the body annotation site's.
            SourceLocation diagLoc;
            const ast::Type* argAst = (paramIdx < inst.argAsts.size())
                                          ? inst.argAsts[paramIdx]
                                          : nullptr;
            if (argAst && argAst->span.begin.line != 0)
                diagLoc = argAst->span.begin;
            else if (s.site)
                diagLoc = s.site->span.begin;
            ctx_.diagnostics().report(
                Severity::Error, diagLoc,
                std::string("`") + std::string(s.container) +
                "` requires a hashable " + std::string(s.property) +
                " type, but the type argument " + keyName +
                " is not hashable (\xC2\xA7""3.6/\xC2\xA7""8.3): " +
                reasonClause(concreteKey));
        }
    }
}

// §28.11 / §15.3 (#3775): the element-slot-policy table's move-only row is
// "rejected at instantiation" for BOTH collection families — an owning
// inline array or an owning-classified choice (§15.3, common::
// isMoveOnlyValueType, shared with the §8.6 boxing rejection so the two
// checks cannot disagree) cannot be copied (the borrowing family's element
// operation for an ordinary, non-move-only value type) and has no disposal
// cascade wired through any of these collection kinds' codegen (issue
// #3775: an owning-classified `choice` `Dictionary<K,V>` value compiled,
// ran, and leaked the case's owned payload — nothing rejected it).
//
// Two-pass shape modelled on checkHashContainerKeys immediately above
// (concrete slots vs. a bare generic parameter of the enclosing type,
// re-checked per instantiation — #1338/#1384) rather than on
// checkOwningElementInstantiations's single predicate, because that
// predicate treats every value type (choice included, #2744) alike and
// needs no classification; ours must additionally ask WHICH choices are
// move-only, a question §15.3 says is not decidable until the choice's own
// case payloads are resolved, so it cannot run inline at
// moveOnlyElementSites_'s recording site.
void Resolver::checkElementCategoryInstantiations() {
    std::unordered_set<const ast::Type*> reported;

    // Pass 1: every site, direct (non-generic-param) AND bare-param alike —
    // modelled on checkHashContainerKeys' Pass 1 immediately above, which
    // calls its shared predicate (isEqualityBearing) unconditionally rather
    // than gating on `declaration == nullptr` first. That gate would be
    // WRONG here: an inline-array TypeRef (Kind::InlineArray) also carries a
    // null `declaration` — inline arrays have no TypeSymbol — so a
    // `declaration == nullptr` early-exit would route a genuinely CONCRETE
    // `[N owned]T`/`[N owned]T?` slot into Pass 2, whose bare-param walk
    // immediately drops it (`kind != TypeRef::Kind::Named`), silently
    // admitting the slot. common::isMoveOnlyValueType is itself null/kind-
    // safe and answers false for a bare generic parameter (Kind::Named with
    // no declaration, so its Choice arm never fires) with no gating needed,
    // so Pass 1 can simply ask it of every recorded site.
    for (const MoveOnlyElementSite& s : moveOnlyElementSites_) {
        const char* moveOnlyKind = nullptr;
        if (!common::isMoveOnlyValueType(s.elRef, ctx_, moveOnlyKind)) continue;
        if (s.site && !reported.insert(s.site).second) continue;  // dedup per site
        // fix #3796 (§28.11 / §15.3): prefer the offending TYPE-ARGUMENT's
        // source location (`s.elRef->astNode`) over the annotation site's,
        // matching the checkHashContainerKeys Pass-1 change just above — IDE
        // jumps land on the type argument, not the surrounding `Dictionary<…>`
        // name. Falls back to `s.site` if the elRef has no AST node.
        SourceLocation diagLoc;
        if (s.elRef && s.elRef->astNode &&
            s.elRef->astNode->span.begin.line != 0)
            diagLoc = s.elRef->astNode->span.begin;
        else if (s.site)
            diagLoc = s.site->span.begin;
        ctx_.diagnostics().report(
            Severity::Error, diagLoc,
            std::string("`") + std::string(s.container) + "` cannot use " +
            std::string(moveOnlyKind) + " as its " + std::string(s.slotName) +
            " type argument (\xC2\xA7""28.11 / \xC2\xA7""15.3) \xE2\x80\x94 "
            "the element-category table's move-only row is rejected at "
            "instantiation for both collection families: a move-only value "
            "cannot be copied, and this collection kind has no disposal "
            "cascade for it");
    }

    // Pass 2 (#3775, modelled on checkOwningElementInstantiations' #2713
    // pass and checkHashContainerKeys' Pass 2 above): a slot whose bound is
    // a bare generic parameter of the ENCLOSING type is re-checked against
    // every concrete instantiation of that type, once every instantiation
    // site has been recorded.
    std::unordered_set<const ast::Type*> reported2;
    for (const MoveOnlyElementSite& s : moveOnlyElementSites_) {
        if (!s.elRef || s.elRef->declaration) continue;  // concrete: pass 1
        if (!s.enclosingType) continue;
        if (s.elRef->kind != TypeRef::Kind::Named) continue;
        if (!s.elRef->astNode ||
            s.elRef->astNode->kind != ast::TypeKind::Named ||
            !s.elRef->astNode->name ||
            s.elRef->astNode->name->parts.size() != 1)
            continue;
        std::string_view paramName = s.elRef->headName();
        std::size_t paramIdx = s.enclosingType->genericParams.size();
        for (std::size_t i = 0; i < s.enclosingType->genericParams.size(); ++i) {
            if (s.enclosingType->genericParams[i] &&
                s.enclosingType->genericParams[i]->name == paramName) {
                paramIdx = i;
                break;
            }
        }
        if (paramIdx >= s.enclosingType->genericParams.size()) continue;
        for (const GenericInstantiation& inst : instantiations_) {
            if (inst.type != s.enclosingType) continue;
            if (paramIdx >= inst.argRefs.size()) continue;
            TypeRef* concrete = inst.argRefs[paramIdx];
            if (!concrete || concrete->kind == TypeRef::Kind::Error) continue;
            const char* moveOnlyKind = nullptr;
            if (!common::isMoveOnlyValueType(concrete, ctx_, moveOnlyKind))
                continue;
            if (s.site && !reported2.insert(s.site).second) continue;
            const TypeSymbol* rec = concrete->declaration;
            std::string argName =
                rec ? ("`" + std::string(rec->name) + "`") : std::string("it");
            // fix #3796 (§28.11 / §15.3): prefer the offending concrete
            // type-argument's source location over the interior annotation
            // site, like the hash-container Pass 2 just above — the §3.6
            // rule-2 per-instantiation discipline puts the diagnostic at the
            // instantiation site, where `concrete` is the bound's resolved
            // TypeRef and `inst.argAsts[paramIdx]` is its source-spelled
            // AST node.
            SourceLocation diagLoc;
            const ast::Type* argAst = (paramIdx < inst.argAsts.size())
                                          ? inst.argAsts[paramIdx]
                                          : nullptr;
            if (argAst && argAst->span.begin.line != 0)
                diagLoc = argAst->span.begin;
            else if (s.site)
                diagLoc = s.site->span.begin;
            ctx_.diagnostics().report(
                Severity::Error, diagLoc,
                std::string("`") + std::string(s.container) +
                "` cannot use " + argName + " as its " +
                std::string(s.slotName) +
                " type argument (\xC2\xA7""28.11 / \xC2\xA7""15.3) \xE2\x80\x94 "
                "it is " + std::string(moveOnlyKind) +
                ", and the element-category table's move-only row is "
                "rejected at instantiation for both collection families");
        }
    }
}

// stdlib §4.14 / spec §14.4 (#3794) — OwningBuffer<T>/Span<T>/ReadOnlySpan<T>
// reject move-only value types T (the §15.3 move-only kinds, owning inline
// arrays and owning-classified choices) per the §14.4 OwningBuffer/Span/
// ReadOnlySpan exception's own clause. Filed separately from #3775 (which
// covers the §28.11 element-slot-policy table's move-only row) because the
// admission rule differs — these three admit value-typed T in general — and
// the diagnostic cites §14.4 rather than §28.11.
//
// Spec §14.4 (v0.286): "`OwningBuffer<T>`, `Span<T>`, and `ReadOnlySpan<T>`
// (stdlib §4.14) are exceptions to this restriction. They admit T to be
// either class-typed (with ownership semantics as above) or value-typed
// (with plain value semantics — no per-slot ownership, no Dispose cascade,
// no `move` admission). They still reject the runtime-managed reference
// types (`string`, function values) and **move-only value types** — both
// kinds of §15.3: the owning inline arrays `[N owned]T` and `[N owned]T?`,
// and the owning-classified choice (§35.4)."
//
// Pass 1 — the concrete-bound case — is INLINE at the annotation site
// (TypeResolution_Names.cpp), so this function is just Pass 2: re-walk the
// bare-generic-parameter sites recorded in owningBufferSpanMoveOnlySites_
// against every concrete instantiation of the enclosing type, exactly like
// checkOwningElementInstantiations's #2713 shape and
// checkElementCategoryInstantiations's Pass 2 above. The `instantiations_`
// table is the same one those two walks use — every type instantiation the
// resolver has seen — so an H<Wrap> instantiation of a class H<T> holding
// `field: OwningBuffer<T>` lands here exactly once and gets one diagnostic
// at the type argument (fix #3796, §3.6 rule 2).
void Resolver::checkOwningBufferSpanMoveOnlyInstantiations() {
    std::unordered_set<const ast::Type*> reported;
    for (const OwningBufferSpanMoveOnlySite& s :
         owningBufferSpanMoveOnlySites_) {
        if (!s.enclosingType) continue;
        // Index of the bare generic parameter within the enclosing type's
        // list. The recorder has already filtered the method-shadow case but
        // re-derive the index here defensively in case the param list has
        // since grown or the bound name no longer resolves.
        std::size_t paramIdx = s.enclosingType->genericParams.size();
        for (std::size_t i = 0; i < s.enclosingType->genericParams.size(); ++i) {
            if (s.enclosingType->genericParams[i] &&
                s.enclosingType->genericParams[i]->name == s.paramName) {
                paramIdx = i;
                break;
            }
        }
        // Not a type parameter of the enclosing TYPE — silently drop. Same
        // tolerance as checkOwningElementInstantiations: a false reject here
        // is strictly worse than carrying the gap (#2736-class residue).
        if (paramIdx >= s.enclosingType->genericParams.size()) continue;
        for (const GenericInstantiation& inst : instantiations_) {
            if (inst.type != s.enclosingType) continue;
            if (paramIdx >= inst.argRefs.size()) continue;
            TypeRef* concrete = inst.argRefs[paramIdx];
            if (!concrete || concrete->kind == TypeRef::Kind::Error) continue;
            const char* moveOnlyKind = nullptr;
            if (!common::isMoveOnlyValueType(concrete, ctx_, moveOnlyKind))
                continue;
            if (s.site && !reported.insert(s.site).second) continue;
            const TypeSymbol* rec = concrete->declaration;
            std::string argName =
                rec ? ("`" + std::string(rec->name) + "`") : std::string("it");
            // fix #3796 (§3.6 rule 2 / §14.4): prefer the offending concrete
            // type-argument's source location over the interior annotation
            // site, like the hash-container and §28.11 Pass 2 above. The
            // §3.6 rule-2 per-instantiation discipline puts the diagnostic at
            // the instantiation site — for `H<Wrap>` with
            // `field: OwningBuffer<T>` that's the `Wrap` argument's location.
            SourceLocation diagLoc;
            const ast::Type* argAst = (paramIdx < inst.argAsts.size())
                                          ? inst.argAsts[paramIdx]
                                          : nullptr;
            if (argAst && argAst->span.begin.line != 0)
                diagLoc = argAst->span.begin;
            else if (s.site)
                diagLoc = s.site->span.begin;
            ctx_.diagnostics().report(
                Severity::Error, diagLoc,
                std::string("`") + std::string(s.container) +
                "` cannot use " + argName +
                " as its T type argument "
                "(\xC2\xA7""14.4 / \xC2\xA7""15.3) \xE2\x80\x94 "
                "the OwningBuffer/Span/ReadOnlySpan exception still rejects "
                "move-only value types, both kinds of \xC2\xA7""15.3: the "
                "owning inline arrays and the owning-classified choices "
                "(\xC2\xA7""35.4). It is " +
                std::string(moveOnlyKind) + ".");
        }
    }
}

// stdlib §4.2 / spec §14.4 (#1327/#1463/#2713) — THE predicate deciding whether
// a type may be an owning collection's owned element. Shared verbatim by the
// declaration-site check in resolveType() (TypeResolution_Names.cpp) and by the
// per-instantiation Pass 2 below, so the two can never give different answers
// for the same element type (the #1463 name-list-skew lesson, generalized).
//
// Spec §14.4 (heading "`owned` and generic type arguments"), bolded paragraph
// "Class-only restriction on owning generics" (v0.240): the owning
// types "restrict their element type T to **class and interface types**. Value
// types are rejected at the instantiation site. … the standard-library owning
// types are compiler-enforced to reject the runtime-managed reference types
// (`string`, function values)."
//
// Nullability is a FLAG on TypeRef, not a Kind, so `Foo?` reaches the Named arm
// with declaration = Foo and is admitted — matching §14.4's "Nullable class
// types and interfaces are admitted. The owning-type instantiation rule checks
// the *underlying* type, not the nullable-wrapper."
std::string Resolver::owningElementBadDesc(const TypeRef* el) {
    if (!el) return {};
    switch (el->kind) {
    case TypeRef::Kind::Primitive:
        // All primitives are rejected: value types, and `string` — a
        // runtime-managed reference (§28.9) with no disposal obligation.
        // fix #5250 (§3.4, v0.313): `Object` is the root class, not a
        // primitive value type — it has a destructor and implements
        // IDisposable, and satisfies `where T: class`.
        if (el->primitive == PrimitiveType::String)
            return "`string` (a runtime-managed reference type)";
        if (el->primitive == PrimitiveType::Object)
            return {};  // Object is a class — admitted
        return "a primitive value type";
    case TypeRef::Kind::Function:
        return "a function value";
    case TypeRef::Kind::Unit:
        return "the unit type `()`";
    case TypeRef::Kind::InlineArray:
        return "an inline array value type";
    case TypeRef::Kind::Named: {
        const TypeSymbol* ts = el->declaration;
        if (ts) {
            if (ts->isValueBuiltin) return "a value-type builtin";
            if (ts->kind == ast::TypeDeclKind::Record)
                return "a record (value type)";
            if (ts->kind == ast::TypeDeclKind::Enum)
                return "an enum (value type)";
            // fix #2744 (§28.11 / §35.4, v0.240): a choice is a value type
            // (§3.2) — and §28.11's element-slot-policy table rejects choices
            // in the owning family in BOTH rows, the "value type (non-move-
            // only)" row (copies in the borrowing column, rejected in the
            // owning column) and the "move-only value type" row (rejected in
            // both columns — owning-classified choices, §35.4). The pre-fix
            // fall-through admitted both, and a value-like choice carrying a
            // `string` payload reproduced #2713's elementPolicyFor Value-row
            // dangling-slot shape (collection takes no reference on insert
            // while `owned` argument transfer releases the caller's — a
            // heap-use-after-free on read-back, not a leak). The borrowing
            // family's "copies" cell is unaffected because the predicate
            // only runs against owning collections; an `OwningDictionary`'s
            // KEY slot is also unaffected because the borrowed element goes
            // through `checkHashContainerKeys`, not this predicate.
            if (ts->kind == ast::TypeDeclKind::Choice)
                return "a choice (value type; §35.4 classifies owning variants as move-only)";
            // Class or Interface → admitted; empty description.
            return {};
        }
        // No TypeSymbol: a builtin type or a generic parameter.
        // DateTime/TimeSpan/DateOnly/TimeOnly/Guid/CancellationToken keep
        // declaration=null intentionally (so lowerNew() fires for `new T(...)`),
        // so the shared isValueBuiltinByName() predicate (#1463) is what
        // recognises them. Class/interface BUILTINS (Array<T>, OwningArray<T>,
        // …) stay admitted, which is what keeps a nested owning collection such
        // as `OwningLinkedList<OwningArray<C>>` legal.
        //
        // An unresolved generic PARAMETER also lands here and is admitted: no
        // concrete type is known at a declaration site. That admission is
        // PROVISIONAL — checkOwningElementInstantiations() re-runs this same
        // predicate on the type each instantiation actually binds (#2713).
        if (el->astNode && el->astNode->name &&
            el->astNode->name->parts.size() == 1) {
            std::string_view nm = el->headName();
            // §28.9 / §28.11 (v0.300 ruling #4393; this fix v0.307): `Type`
            // is runtime-managed — the runtime holds exactly one instance
            // per type for the process's life and user code never
            // allocates one, so an OWNED element slot has nothing to own.
            // `isValueBuiltinByName` deliberately does NOT list `Type` (it
            // is not a value-type builtin — it is the named-but-
            // declaration=null member of §28.9's runtime-managed family,
            // the same family `string` and function values belong to,
            // both already rejected by this function's `Primitive` and
            // `Function` arms above), so this predicate never saw it and
            // `OwningArray<Type>` / `OwningDictionary<K, Type>` / etc. were
            // admitted at instantiation with no diagnostic — the exact gap
            // #4393's own ruling named: "the gate was caught missing `Type`
            // from its runtime-managed row" in §28.11's element-category
            // table. Mirrors the `nm == "Type"` test the owned-marker-
            // admission check already runs at TypeResolution_Names.cpp
            // (also #4393) — the owning-collection-element twin of that
            // check. // per §28.9, §28.11, v0.300/v0.307, #4393
            if (nm == "Type")
                return "the runtime `Type` token (a runtime-managed "
                       "reference type, §28.9)";
            if (isValueBuiltinByName(nm)) return "a value-type builtin";
        }
        return {};
    }
    default:  // Kind::Error — suppress cascade diagnostics
        return {};
    }
}

// True iff `bound` is a BARE generic-parameter binding rather than a concrete
// type: a single-part Named ref with no declaration naming neither a primitive
// nor a builtin. Hoisted from checkDeferredNestedConstraints' local lambda
// (#2111) so it and the #2713 owning-element closure share ONE definition —
// two copies of this predicate is exactly the skew #1463 was about. Excluding
// primitives and builtins by name is what keeps `Array<T>`, `Object`, and the
// value builtins out of the "chained" arm.
bool Resolver::isBareGenericParamBinding(const TypeRef* bound,
                                         const ast::Type* boundAst) {
    return bound && boundAst && !bound->declaration &&
           bound->kind == TypeRef::Kind::Named &&
           boundAst->kind == ast::TypeKind::Named && boundAst->name &&
           boundAst->name->parts.size() == 1 &&
           primitiveFromName(boundAst->name->parts.front()) ==
               PrimitiveType::None &&
           builtinTypeArity(boundAst->name->parts.front()) == kNotBuiltin;
}

// fix #2742 (§3.6 rule 2 "Per-instantiation evaluation for generics" / §14.1
// *Instantiation and its site*, v0.306). See the header comment for the
// contract and for why a non-shadowing method parameter is skipped too.
//
// This predicate had been hand-inlined at THREE recorders
// (`owningElementSites_`, `owningBufferSpanMoveOnlySites_`, `asTargetSites_`)
// and was missing from TWO (`hashContainerSites_` — #2742 itself — and
// `moveOnlyElementSites_`, the same defect unreported). That is the Family I1
// mechanism at the deferred-obligation recorder: the copies agreed when they
// were written and one was later taught something the others were not. One
// definition, five call sites, so a sixth recorder cannot silently omit it.
bool Resolver::nameBoundByCurrentMethod(std::string_view name) const {
    if (!currentMethodGenericParams_) return false;
    for (const ast::GenericParam& gp : *currentMethodGenericParams_)
        if (gp.name == name) return true;
    return false;
}

// stdlib §4.2 / spec §14.4 / §3.6 rule 2 (#2713) — Pass 2 for the owning-
// collection element check.
//
// Spec §3.6 rule 2 "Per-instantiation evaluation for generics" (v0.240): "For a
// generic type or member, the property is evaluated at each instantiation, after
// substituting the type arguments. A generic body type-checks under the weakest
// assumption its constraints establish; an instantiation that violates a
// property-dependent requirement is rejected *at the instantiation site*, with a
// diagnostic naming the property, the offending type argument, and the
// structural reason (§28.5's generic-owned-field check and **the
// owning-collection element restriction of §14.4** follow this pattern)."
//
// The declaration-site check admits `OwningLinkedList<T>` inside `class H<T>`
// because T is a placeholder there. Without this pass nothing ever re-checked
// the binding, so `H<string>` was admitted — and that is not merely cosmetic:
// codegen's elementPolicyFor() falls back to the Value row for a runtime-managed
// element in the owning family (LlvmBackend_ElementPolicy.cpp:238-250), so the
// collection stores the pointer WITHOUT taking a reference while the `owned`
// argument transfer still releases the caller's, leaving a dangling slot. Reading
// the element back out is a heap-use-after-free (measured; see the #2713 test).
//
// Modelled on checkHashContainerKeys()'s Pass 2 — the existing, working
// precedent for exactly this shape.
void Resolver::checkOwningElementInstantiations() {
    // One diagnostic per (instantiation site, container, parameter). The
    // interior site is deliberately NOT part of the key: a field spelled
    // `private owned _l: OwningLinkedList<T> := new OwningLinkedList<T>();`
    // records TWO interior sites (the annotation and the `new`) for one user
    // error, and §3.6 rule 2 asks for one diagnostic at the instantiation site.
    // Container and parameter ARE in the key, so a body that misuses two
    // different parameters still reports each one.
    // fix #4582: the site key is `GenericInstantiation::siteKey`, an opaque
    // identity (an ast::Type node for a type-position spelling, or the
    // IdentifierExpr of a static-member-access receiver, which has none).
    std::set<std::tuple<const void*, std::string_view, std::string_view>>
        reported;
    for (const OwningElementSite& s : owningElementSites_) {
        if (!s.enclosingType) continue;
        // Index of the recorded parameter within the enclosing type's list.
        std::size_t paramIdx = s.enclosingType->genericParams.size();
        for (std::size_t i = 0; i < s.enclosingType->genericParams.size(); ++i) {
            if (s.enclosingType->genericParams[i] &&
                s.enclosingType->genericParams[i]->name == s.paramName) {
                paramIdx = i;
                break;
            }
        }
        // Not a type parameter of the enclosing TYPE — e.g. a generic METHOD's
        // own parameter, whose bindings are not recorded in instantiations_.
        // Nothing to re-check, and (deliberately) nothing to reject: silently
        // dropping the obligation is preferable to a false reject here, and the
        // gap is recorded in the issue tracker rather than guessed at.
        if (paramIdx >= s.enclosingType->genericParams.size()) continue;

        // A binding may be another generic's parameter rather than a concrete
        // type: `class Outer<U>` holding `private owned g: Inner<U>`, where it
        // is INNER that holds the owning collection. The only instantiation of
        // Inner is then `Inner<U>` — argument unresolved — and no `Inner<string>`
        // node ever exists, so a single pass over instantiations_ of the
        // recording type sees nothing concrete and #2713's use-after-free
        // returns at one extra level of composition. Close it with a worklist
        // that RE-KEYS the obligation to (enclosingType, that parameter) and
        // iterates, exactly as checkDeferredNestedConstraints does for §14.2
        // constraints (the `isBareGenericParamBinding` arm below).
        struct Pending {
            TypeSymbol* type;
            std::string_view paramName;
            std::size_t idx;
        };
        std::vector<Pending> work{{s.enclosingType, s.paramName, paramIdx}};
        // (type, param) visited set: a recursive generic (`class A<T>` holding
        // an `A<T>`) or a diamond of instantiation sites must not re-enqueue.
        std::set<std::pair<TypeSymbol*, std::string_view>> seen;
        while (!work.empty()) {
            Pending p = work.back();
            work.pop_back();
            if (!seen.insert({p.type, p.paramName}).second) continue;
            for (const GenericInstantiation& inst : instantiations_) {
                if (inst.type != p.type) continue;
                if (p.idx >= inst.argRefs.size()) continue;
                const TypeRef* arg = inst.argRefs[p.idx];
                const ast::Type* argAst = (p.idx < inst.argAsts.size())
                                              ? inst.argAsts[p.idx] : nullptr;
                // Kind::Error → resolution already failed; suppress the cascade.
                if (!arg || arg->kind == TypeRef::Kind::Error) continue;

                if (isBareGenericParamBinding(arg, argAst)) {
                    // Chained: re-key to the body this site sits in.
                    if (!inst.enclosingType) continue;
                    std::string_view nextName = argAst->name->parts.front();
                    // Same misattribution guard as the recorder: if the METHOD
                    // enclosing this site binds that name, it is not
                    // enclosingType's parameter and its bindings are not
                    // tracked — `class Outer<U>` with `method M<U>()` holding
                    // an `Inner<U>` must NOT be validated against Outer's `U`.
                    bool boundByMethod = false;
                    if (inst.methodGenericParams) {
                        for (const ast::GenericParam& gp :
                             *inst.methodGenericParams) {
                            if (gp.name == nextName) {
                                boundByMethod = true;
                                break;
                            }
                        }
                    }
                    if (boundByMethod) continue;
                    std::size_t nextIdx = inst.enclosingType->genericParams.size();
                    for (std::size_t i = 0;
                         i < inst.enclosingType->genericParams.size(); ++i) {
                        if (inst.enclosingType->genericParams[i] &&
                            inst.enclosingType->genericParams[i]->name ==
                                nextName) {
                            nextIdx = i;
                            break;
                        }
                    }
                    // Not a parameter of that type either — nothing to track.
                    if (nextIdx >= inst.enclosingType->genericParams.size())
                        continue;
                    work.push_back({inst.enclosingType, nextName, nextIdx});
                    continue;
                }

                std::string badDesc = owningElementBadDesc(arg);
                if (badDesc.empty()) continue;
                if (!reported.insert({inst.siteKey, s.container, s.paramName})
                         .second)
                    continue;
                // The message names the type that DECLARES the owning
                // collection (s.enclosingType) and its parameter, not p.type —
                // through a chain those differ, and it is the declaring site
                // the reader must go fix. The location is the instantiation
                // that made the binding concrete, per §3.6 rule 2.
                ctx_.diagnostics().report(
                    Severity::Error, inst.loc,
                    std::string("`") + std::string(s.container) +
                    "` requires a class or interface element type "
                    "(stdlib \xC2\xA7""4.2 / spec \xC2\xA7""14.4, #1327) "
                    "\xE2\x80\x94 at this instantiation, `" +
                    std::string(s.enclosingType->name) +
                    "`'s type parameter `" + std::string(s.paramName) +
                    "`, used as its owning-collection element type, is bound "
                    "to " + badDesc +
                    ", which has no disposal obligation and cannot be owned in "
                    "an owning collection (\xC2\xA7""3.6 rule 2 \xE2\x80\x94 the "
                    "property is evaluated at each instantiation). Use `" +
                    std::string(s.container.substr(6)) +
                    "<\xE2\x80\xA6>` for value elements.");
            }
        }
    }
}

// fix #2911 + fix #2962 — §17.4 / §3.6 rule 2 (v0.241). Pass 2 for the
// degenerate-`as`-target rule at a TYPE-level generic parameter; the shape is
// checkOwningElementInstantiations above, minus its chained-binding worklist
// (see the residual note at the end).
//
// §17.4 (v0.241): "When the target is a generic type parameter, the check
// applies per instantiation (§3.6 rule 2): `x as T` at an inline-array or unit
// substitution is rejected at the instantiation site."
//
// MEASURED on 309356e: this is not a tidy-up. `class Box<T>` with `var r := o
// as T;` in a member, instantiated `new Box<[3]int>()`, SEGFAULTED the compiler
// (exit 139, core dumped) on the executable path; at `Box<()>` it emitted two
// #2434 "initializer expression failed to lower … degraded to a zero-initialized
// value" internal warnings. The rule turns both into one sourced diagnostic at
// the instantiation site.
void Resolver::checkAsTargetInstantiations() {
    if (asTargetSites_.empty() || instantiations_.empty()) return;
    // One diagnostic per (instantiation site, parameter): a body may write
    // `as T` several times over the same parameter for one user error, and
    // §3.6 rule 2 asks for one diagnostic at the instantiation site. The
    // parameter IS in the key, so a body misusing two parameters reports each.
    // fix #4582: the site key is `GenericInstantiation::siteKey`, an opaque
    // identity — a static-member-access receiver has no ast::Type node.
    std::set<std::pair<const void*, std::string_view>> reported;
    for (const AsTargetSite& s : asTargetSites_) {
        if (!s.enclosingType) continue;
        // Index of the recorded parameter within the enclosing type's list.
        std::size_t paramIdx = s.enclosingType->genericParams.size();
        for (std::size_t i = 0; i < s.enclosingType->genericParams.size(); ++i)
            if (s.enclosingType->genericParams[i] &&
                s.enclosingType->genericParams[i]->name == s.paramName) {
                paramIdx = i;
                break;
            }
        if (paramIdx >= s.enclosingType->genericParams.size()) continue;
        // Fix #3093: a binding that is itself another generic's parameter
        // (`class Outer<U>` holding `Inner<U>` whose body does `as T`) is
        // unreachable by a single pass over `s.enclosingType`'s instantiations
        // — the only `Inner<…>` node is `Inner<U>`, whose argument is the bare
        // `U`. The worklist re-keys the obligation to (enclosingType, that
        // parameter) and iterates, the same shape checkOwningElementInstantiations
        // (#2713) uses for the owning-element arm beside this one — sharing the
        // walk is cheaper and prevents the two predicates from drifting on
        // exactly the same shape (#1463 name-list-skew lesson).
        struct Pending {
            TypeSymbol* type;
            std::string_view paramName;
            std::size_t idx;
        };
        std::vector<Pending> work{{s.enclosingType, s.paramName, paramIdx}};
        // (type, param) visited set: a recursive generic (`class A<T>` holding
        // an `A<T>`) or a diamond of instantiation sites must not re-enqueue.
        std::set<std::pair<TypeSymbol*, std::string_view>> seen;
        while (!work.empty()) {
            Pending p = work.back();
            work.pop_back();
            if (!seen.insert({p.type, p.paramName}).second) continue;
            for (const GenericInstantiation& inst : instantiations_) {
                if (inst.type != p.type) continue;
                if (p.idx >= inst.argRefs.size()) continue;
                const TypeRef* arg = inst.argRefs[p.idx];
                const ast::Type* argAst =
                    (p.idx < inst.argAsts.size()) ? inst.argAsts[p.idx]
                                                  : nullptr;
                // Kind::Error → resolution already failed; suppress the cascade.
                if (!arg || arg->kind == TypeRef::Kind::Error) continue;
                if (isBareGenericParamBinding(arg, argAst)) {
                    // Chained: re-key to the body this site sits in.
                    if (!inst.enclosingType) continue;
                    std::string_view nextName = argAst->name->parts.front();
                    // Same misattribution guard as the recorder and #2713: a
                    // METHOD enclosing this site binds that name → its
                    // bindings are not tracked, and the chain must not
                    // attribute the argument to `enclosingType`.
                    bool boundByMethod = false;
                    if (inst.methodGenericParams) {
                        for (const ast::GenericParam& gp :
                             *inst.methodGenericParams) {
                            if (gp.name == nextName) {
                                boundByMethod = true;
                                break;
                            }
                        }
                    }
                    if (boundByMethod) continue;
                    std::size_t nextIdx =
                        inst.enclosingType->genericParams.size();
                    for (std::size_t i = 0;
                         i < inst.enclosingType->genericParams.size(); ++i) {
                        if (inst.enclosingType->genericParams[i] &&
                            inst.enclosingType->genericParams[i]->name ==
                                nextName) {
                            nextIdx = i;
                            break;
                        }
                    }
                    // Not a parameter of that type either — nothing to track.
                    if (nextIdx >= inst.enclosingType->genericParams.size())
                        continue;
                    work.push_back(
                        {inst.enclosingType, nextName, nextIdx});
                    continue;
                }
                const std::string degenerate = degenerateAsTargetReason(arg);
                if (degenerate.empty()) continue;
                if (!reported.insert({inst.siteKey, s.paramName}).second)
                    continue;
                // §3.6 rule 2 wants the diagnostic AT the instantiation site,
                // and that is where it goes — except that some `Named<…>`
                // nodes carry no span (measured: a `new Box<[3]int>()` type
                // node reports 0:0), and a 0:0 diagnostic is not actionable.
                // Fall back to the `as` expression, which is a real location
                // naming the body at fault; the message identifies both ends
                // either way.
                const SourceLocation at =
                    inst.loc.line != 0 ? inst.loc : s.loc;
                ctx_.diagnostics().report(
                    Severity::Error, at,
                    "type parameter `" + std::string(s.paramName) + "` of `" +
                    std::string(s.enclosingType->name) +
                    "` is used as an `as` target in the generic body (line " +
                    std::to_string(s.loc.line) +
                    "), so this instantiation is rejected at the instantiation "
                    "site (§3.6 rule 2): " + degenerate);
            }
        }
    }
}

// Forward declarations for the static-helper predicates that
// classifyStaticReadonlySubstitution and its callees need; their
// definitions live later in this TU (alongside the other §22.12
// statics-and-domains predicates around line 9289). Forward-declared
// here so the per-instantiation pass can sit beside checkAsTarget-
// Instantiations / checkOwningElementInstantiations without reordering
// the larger predicate block.
static bool isInSynchronizedSet(const TypeRef* rt);
static bool isInReadonlySuppressionSet(const TypeRef* rt);
static bool isRule4GovernedReference(const TypeRef* rt);
static std::string describeStaticValueType(const TypeRef* rt);
// Forward declarations for the §22.12 rule 4 residual (#3290) value-site
// helpers; their definitions live later in this TU alongside the other
// §22.12 predicates.
static bool isHoldsBorrowsChoice(const TypeRef* rt,
                                 sema::SemaContext& ctx);
static bool isPlainInlineArrayCarryingBorrows(
    const TypeRef* rt, sema::SemaContext& ctx);
static bool isBorrowBearingRecordForStaticReadonly(const TypeRef* rt,
                                                   SemaContext& ctx);
// §22.12 rule 4 residual + §9.7 strict reference-type quantifier
// (#3290): shallow check that recognises builtin reference types
// (Object, Mutex, Channel, …) as reference-typed, not just user-
// declared classes/interfaces. Forward-declared so the value-site pass
// can consult it; defined alongside the other §22.12 predicates.
static bool staticReadonlyFieldIsReferenceTyped(const TypeRef* rt,
                                                SemaContext& ctx);
static bool isStaticallyNullExpr(const ast::Expr* e);
static bool isStaticallyDefaultExpr(const ast::Expr* e);
// fix #3973 (§9.7, v0.295): returns true iff a diagnostic was reported (the
// walk is now recursive — see the definition below).
// fix #4164 (§9.7, v0.295): `incoming` is the ENCLOSING frame's own
// `common::RecordSubstEnv*` (null at the top of the recursion) — needed so
// `declTy` itself, when it is a COMPOUND generic type that mentions the
// enclosing record's own bare parameter (`InnerCG<T>` as a field of
// `OuterCG<T>`), resolves its own `genericArgs` through the outer binding
// before this frame seeds its own environment. See the definition for the
// full rationale.
static bool checkStaticReadonlyRecordInit(
    SemaContext& ctx, const ast::NewExpr& nx, const TypeRef* declTy,
    const common::RecordSubstEnv* incoming, SourceLocation loc,
    std::string_view what, std::string_view name);
// fix #3973 (§9.7, v0.295): handles a positional slot (record field or
// choice payload) that is not ITSELF directly reference-typed but may
// still carry a nested borrow one or more levels down — see the definition
// below, just above `checkStaticReadonlyRecordInit`.
// fix #4164 (§9.7, v0.295): `incoming` is `pft`'s OWN substitution
// environment (the `.env` half of the `RecordSubstEnv::Entry` the caller
// already resolved `pft` through) — threaded to the nested frame exactly
// as `common::isBorrowBearingRecordCommon`'s own walk already does, so a
// compound nested type's bare mentions of the ENCLOSING generic's own
// parameter compose instead of resolving to nothing.
static bool checkNestedBorrowBearingSlot(
    SemaContext& ctx, const TypeRef* pft, const common::RecordSubstEnv* incoming,
    const ast::Expr* argExpr, SourceLocation loc, std::string_view what,
    std::string_view name);
// fix #4134 (§9.7, v0.295): a choice VALUE is constructed via a case CALL
// (`Type.Case(args)`, resolved to a ChoiceCaseSymbol) — never a `new`
// expression (§35.1: "a choice's construction *is* case selection"; spec
// prose at the field-initializer admission list states this explicitly:
// "it is not a `new` expression"). `checkStaticReadonlyRecordInit`'s own
// Choice arm is keyed to a `new`-expression's argument list and can only
// GUESS which case is intended by matching arity across every declared
// case; here the callee already resolved to one EXACT case, so there is
// no ambiguity to guess through. See the definition below, just after
// `checkStaticReadonlyRecordInit`.
// fix #4133 (§9.7, v0.295): `choiceTy` is the CONCRETE instantiation TypeRef
// of the choice being constructed (carries `genericArgs` when the choice is
// generic) — needed to substitute a bare-type-parameter payload field
// through the actual instantiation, exactly as the record arm does. See the
// definition for the full rationale.
// fix #4164 (§9.7, v0.295): `incoming` is the enclosing frame's substitution
// environment, threaded down for the identical reason as
// `checkStaticReadonlyRecordInit`'s own `incoming` — null at the top of the
// recursion (this choice is the static readonly field's own declared type),
// `pft`'s own `.env` when reached via `checkNestedBorrowBearingSlot`.
static bool checkStaticReadonlyChoiceCaseInit(
    SemaContext& ctx, const ast::CallExpr& ce, const ChoiceCaseSymbol* cs,
    const TypeRef* choiceTy, const common::RecordSubstEnv* incoming,
    SourceLocation loc, std::string_view what, std::string_view name);

// §22.12 rule 3/4 (v0.262, #3272): classify a CONCRETE substitution at the
// instantiation site for a `static readonly` bare-parameter declaration,
// emitting the diagnostic directly per §3.6 rule 2's pattern (lands at
// the instantiation site; names the offending type argument and the
// structural reason; the §22.12 cited text per the rule).
//
// The classification is the dispatch the spec text states inline at v0.262:
//
//   - rule 3 (immutable content): value types of sendable-value category,
//     string, enums → unrestricted. No diagnostic.
//   - rule 4 (class type): the default rule 4 declaration-site-class
//     warning, silenced for the warning-suppression set
//     (FrozenArray<T>, CancellationTokenSource, Locked<T>, RwLocked<T>;
//     the form-(i) synchronized set is also reachable through this
//     predicate via `isInSynchronizedSet`).
//   - interface or function substitution: rule 4 warning explicitly, no
//     suppression (no interface or function type is in the suppression
//     set).
//   - owning-classified choice (§35.4): rejection on the disposal-site
//     ground — no rule enrols an owning-choice static readonly in §22.13's
//     program-exit disposal pass, so the value would be process-lived with
//     no disposal site.
//   - owning-inline-array substitution: default rule 4 warning, but §22.13's
//     program-exit pass disposes it (the type-level `owned` slot marker
//     conveys the obligation, no field-level `owned` modifier needed).
//   - borrow-bearing record / non-owning choice that holds borrows / plain
//     inline array carrying borrows: §9.7 null-source case, per instantiation.
//     For this first pass the implementation accepts and lets §9.7's own
//     rule reject at the value site when reference-typed slots are non-null
//     at write time (the spec text defers to §9.7 explicitly, so this is
//     faithful to the rule).
//
// The nullable form classifies as its underlying type per spec. rt->nullable
// is a flag on the same TypeRef, so the kind / declaration / headName tests
// already read the underlying type.
void Resolver::classifyStaticReadonlySubstitution(
    const TypeRef* rt, std::string_view paramName,
    std::string_view memberName, bool isField, SourceLocation declLoc,
    std::string_view enclosingTypeName) {
    const std::string what = isField ? "field" : "property";

    // The diagnostic text includes the offending type argument spelled out
    // (§3.6 rule 2's diagnostic pattern). Build it once per case so each
    // branch stays terse.
    auto spelled = [&]() {
        return describeStaticValueType(rt);
    };

    // RULE 4 (class-typed) — including interface and builtin reference
    // classes. isRule4GovernedReference covers user-declared classes/
    // interfaces AND the `Channel<T>` carve-out.
    if (isRule4GovernedReference(rt)) {
        if (isInReadonlySuppressionSet(rt) || isInSynchronizedSet(rt)) {
            return;  // suppressed — clean
        }
        // §22.12 rule 4 warning at the instantiation site, naming the
        // offending type argument. §3.6 rule 2's diagnostic pattern:
        // name the property, name the offending type argument, name the
        // structural reason, land at the instantiation site.
        ctx_.diagnostics().report(
            Severity::Warning, declLoc,
            std::string("static readonly ") + what + " `" +
            std::string(memberName) +
            "` of enclosing type `" +
            std::string(enclosingTypeName) +
            "`, declared at type parameter `" +
            std::string(paramName) +
            "`, is bound at this instantiation to `" + spelled() +
            "`, a class type reachable from every domain — confine "
            "its use to one domain or ensure its own internal "
            "synchronization (§22.12 rule 4, v0.262, #3272; §3.6 rule 2)");
        return;
    }

    // Function-typed substitution: rule 4 warning explicitly (no
    // suppression), per the residual paragraph's "function type" bullet.
    if (rt->kind == TypeRef::Kind::Function) {
        ctx_.diagnostics().report(
            Severity::Warning, declLoc,
            std::string("static readonly ") + what + " `" +
            std::string(memberName) +
            "` of enclosing type `" +
            std::string(enclosingTypeName) +
            "`, declared at type parameter `" +
            std::string(paramName) +
            "`, is bound at this instantiation to `" + spelled() +
            "`, a function type — captured variables are a heap-"
            "allocated mutable cell reachable from every domain through "
            "this field (§22.12 rule 4, v0.262, #3272; §3.6 rule 2)");
        return;
    }

    // Owning-classified choice substitution: rejection per the residual
    // paragraph's "owning-classified choice" bullet — no rule enrols an
    // owning-choice static readonly in §22.13's program-exit disposal
    // pass, so the value would be process-lived with no disposal site.
    // The §35.4 classification governs the predicate; for this first
    // pass we conservatively flag every choice substitution as
    // owning-classified and reject, deferring the narrower predicate to
    // a follow-up that factors choice classification into a shared
    // sema-readable helper. The existing
    // choice_classification_35_4_test pins the predicate's own
    // correctness; this lane ships the spec-text-faithful conservative
    // behaviour to land the rule 3/4 story whole.
    if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
        rt->declaration->kind == ast::TypeDeclKind::Choice) {
        // Conservative: every choice substitution is rejected at the
        // instantiation site with the spec's wording. A future
        // revision can narrow to "owning-classified choices" by
        // consulting §35.4's verdict on the substituted choice; the
        // diagnostic cites §35.4 so the conservative behaviour names
        // the upgrade hook for any narrowing lane.
        ctx_.diagnostics().report(
            Severity::Error, declLoc,
            std::string("static readonly ") + what + " `" +
            std::string(memberName) +
            "` of enclosing type `" +
            std::string(enclosingTypeName) +
            "`, declared at type parameter `" +
            std::string(paramName) +
            "`, is bound at this instantiation to `" + spelled() +
            "`, a choice type — no rule enrols an owning-classified "
            "choice static in §22.13's program-exit disposal, so the "
            "value would be process-lived with no disposal site "
            "(§22.12 rule 4 residual, v0.262, #3272; §35.4 future-"
            "narrowing path; §3.6 rule 2)");
        return;
    }

    // Owning-inline-array substitution: default rule 4 warning, but
    // §22.13's program-exit pass disposes it (the type-level `owned`
    // slot marker conveys the obligation, no field-level `owned`
    // modifier needed). Same shape as rule 4's owned-static clause.
    if (rt->kind == TypeRef::Kind::InlineArray && rt->arrayOwned) {
        ctx_.diagnostics().report(
            Severity::Warning, declLoc,
            std::string("static readonly ") + what + " `" +
            std::string(memberName) +
            "` of enclosing type `" +
            std::string(enclosingTypeName) +
            "`, declared at type parameter `" +
            std::string(paramName) +
            "`, is bound at this instantiation to `" + spelled() +
            "`, an owning-inline-array type reachable from every "
            "domain — confine every slot read to one domain "
            "(§22.12 rule 4 residual, v0.262, #3272; §3.6 rule 2)");
        return;
    }

    // Borrow-bearing record / non-owning choice that holds borrows /
    // plain inline array carrying borrows: §9.7 null-source case. The
    // spec defers to §9.7 explicitly; this implementation accepts the
    // substitution here and lets §9.7's own rule reject at the value
    // site when reference-typed slots are non-null at write time. A
    // narrower instantiation-site check (asserting the type carries no
    // reference-typed slot, or that the substitution is null-source-
    // eligible) is a possible follow-up.

    // RULE 3 (immutable content): value types of sendable-value
    // category, string, enums. Anything not covered above — primitives
    // (incl. string), enums, value-like records, value-like choices,
    // value builtins (DateTime, Guid, Range, etc.), FrozenArray<T>, etc.
    // — is unrestricted. No diagnostic.
}

// §22.12 rule 3/4 (v0.262, #3272): pass 2 for the `static readonly` bare-
// parameter classification. The declaration-site check in
// checkTypeStaticFieldDomains records each `static readonly` field/
// auto-property whose declared type is a bare parameter of the enclosing
// TYPE; this pass walks the recorded instantiations of each enclosing
// type and applies rule 3 (immutable content → clean), rule 4 (class type
// → declaration-site-class warning, suppressed for the warning-suppression
// set), or the residual paragraph's per-instantiation cases (interface
// type / function type → rule 4 warning explicitly; borrow-bearing record
// / non-owning choice that holds borrows → §9.7 null-source case per
// instantiation; owning-classified choice → rejection; owning-inline-array-
// typed → default warning). The diagnostic lands at the instantiation
// site, naming the offending type argument (§3.6 rule 2's diagnostic
// pattern, v0.240).
//
// Modelled on checkOwningElementInstantiations (#2713) and
// checkAsTargetInstantiations (#2911) — the existing per-instantiation
// precedents beside it. Sharing the worklist shape (chained-binding
// re-key through isBareGenericParamBinding) prevents the predicate set
// from drifting on exactly the same chained-generic shape, the #1463
// lesson.
//
// One diagnostic per (instantiation site, member, parameter). The
// parameter IS in the key so a body misusing two parameters reports each.
void Resolver::checkStaticReadonlyBareTInstantiations() {
    if (staticReadonlyBareTSites_.empty() || instantiations_.empty()) return;
    std::set<std::tuple<const ast::Type*, std::string_view, std::string_view>>
        reported;
    for (const StaticReadonlyBareTSite& s : staticReadonlyBareTSites_) {
        if (!s.enclosingType) continue;
        // Index of the recorded parameter within the enclosing type's list.
        std::size_t paramIdx = s.enclosingType->genericParams.size();
        for (std::size_t i = 0; i < s.enclosingType->genericParams.size(); ++i) {
            if (s.enclosingType->genericParams[i] &&
                s.enclosingType->genericParams[i]->name == s.paramName) {
                paramIdx = i;
                break;
            }
        }
        // Not a type parameter of the enclosing TYPE — e.g. a generic METHOD's
        // own parameter, whose bindings are not recorded in instantiations_.
        // Nothing to re-check, and (deliberately) nothing to reject: silently
        // dropping the obligation is preferable to a false reject here.
        if (paramIdx >= s.enclosingType->genericParams.size()) continue;

        // Chained binding worklist — `class Outer<U>` holding `Inner<U>`
        // with `Inner`'s `static readonly F: U;` records one obligation on
        // `Inner<U>`, whose `U` argument is itself a parameter of `Outer`.
        // Re-key to (Outer, U) and iterate, exactly as
        // checkOwningElementInstantiations does for the owning-element arm.
        struct Pending {
            TypeSymbol* type;
            std::string_view paramName;
            std::size_t idx;
        };
        std::vector<Pending> work{{s.enclosingType, s.paramName, paramIdx}};
        std::set<std::pair<TypeSymbol*, std::string_view>> seen;
        while (!work.empty()) {
            Pending p = work.back();
            work.pop_back();
            if (!seen.insert({p.type, p.paramName}).second) continue;
            for (const GenericInstantiation& inst : instantiations_) {
                if (inst.type != p.type) continue;
                if (p.idx >= inst.argRefs.size()) continue;
                const TypeRef* arg = inst.argRefs[p.idx];
                const ast::Type* argAst =
                    (p.idx < inst.argAsts.size()) ? inst.argAsts[p.idx]
                                                  : nullptr;
                if (!arg || arg->kind == TypeRef::Kind::Error) continue;

                if (isBareGenericParamBinding(arg, argAst)) {
                    // Chained: re-key through this binding.
                    if (!inst.enclosingType) continue;
                    std::string_view nextName =
                        argAst->name->parts.front();
                    // Same misattribution guard as #2713/#2911: a METHOD
                    // enclosing this site binds that name → its bindings
                    // are not tracked, and the chain must not attribute
                    // the argument to `enclosingType`.
                    bool boundByMethod = false;
                    if (inst.methodGenericParams) {
                        for (const ast::GenericParam& gp :
                             *inst.methodGenericParams) {
                            if (gp.name == nextName) {
                                boundByMethod = true;
                                break;
                            }
                        }
                    }
                    if (boundByMethod) continue;
                    std::size_t nextIdx =
                        inst.enclosingType->genericParams.size();
                    for (std::size_t i = 0;
                         i < inst.enclosingType->genericParams.size(); ++i) {
                        if (inst.enclosingType->genericParams[i] &&
                            inst.enclosingType->genericParams[i]->name ==
                                nextName) {
                            nextIdx = i;
                            break;
                        }
                    }
                    if (nextIdx >= inst.enclosingType->genericParams.size())
                        continue;
                    work.push_back(
                        {inst.enclosingType, nextName, nextIdx});
                    continue;
                }

                // The substitution is concrete. Classify per §22.12 rule 3
                // / rule 4 / the residual paragraph; the helper emits the
                // diagnostic at the instantiation site and is silent on
                // rule 3 immutable content. The location passed in is the
                // instantiation site's location, falling back to the
                // declaration site if the `Named<...>` AST node carries no
                // span (a measured case — see checkAsTargetInstantiations
                // for the same defensive fallback).
                classifyStaticReadonlySubstitution(
                    arg, s.paramName, s.memberName,
                    /*isField=*/s.isField,
                    inst.loc.line != 0 ? inst.loc : s.declLoc,
                    p.type->name);
            }
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
    // fix #4444: reservation of the three Object-virtual names against
    // choice-case names — §35.1 (v0.302). Runs alongside the duplicate
    // walk because it is the choice-specific counterpart to §11.3's
    // member-naming rule: cases ARE members (§35.1, "case names… may
    // not collide with any member name"), and the reservation sits one
    // rung further out — the name itself is off-limits regardless of
    // whether anything else holds it. Keying it on classification is
    // explicitly declined (§35.1 DECLINED.md) — the use-site compile
    // error the §35.7 auto-package removes exists on exactly the
    // classifications where the synthesis does NOT fire, so the
    // reservation must hold there precisely.
    checkChoiceCaseReservedNames(t);
    // Recurse into nested types.
    for (Symbol* m : t->members) {
        if (auto* nt = dynamic_cast<NestedTypeSymbol*>(m)) {
            if (nt->type) checkTypeDuplicates(nt->type);
        }
    }
}

// fix #4444 (§35.1, v0.302). The three names §35.7 calls "the member
// §35.7 lets the author declare" are reserved against choice case names
// on every classification, whether or not a member of that name is
// declared. Holding the reservation even on choices that declare the
// member would still be a compile error (the case and the member would
// be §11.3 duplicates by the walk above), but keying it on "a member
// is declared" would silently legalize the case on every choice that
// does NOT declare it — exactly the gap #4444 closes.
//
// The reservation is choice-specific (records, enums, and classes do
// not have cases), so the predicate is intentionally narrow: the three
// exact strings, no more, no less. Any widening is a separate ruling.
static bool isChoiceCaseReservedName(std::string_view n) {
    return n == "Equals" || n == "GetHashCode" || n == "ToString";
}

void Resolver::checkChoiceCaseReservedNames(const TypeSymbol* t) {
    if (!t) return;
    if (t->kind != ast::TypeDeclKind::Choice) return;
    for (const Symbol* m : t->members) {
        auto* ccs = dynamic_cast<const ChoiceCaseSymbol*>(m);
        if (!ccs) continue;
        if (!isChoiceCaseReservedName(ccs->name)) continue;
        // The diagnostic names the case, the choice, the reserved name,
        // the spec section that imposes the reservation, the §35.7
        // declared-member fix, and a concrete REMEDY (rename the case) —
        // exactly the kind of compile error §11.3 / §35.1 produces
        // elsewhere in this file.
        ctx_.diagnostics().report(
            Severity::Error, ccs->declLoc,
            std::string("choice `") + std::string(t->name) +
            "`: case `" + std::string(ccs->name) + "` is named `" +
            std::string(ccs->name) + "`, which is reserved against case "
            "names on every classification (§35.1, v0.302); rename the "
            "case, because the name is reserved for the member §35.7 "
            "lets the author declare");
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

// fix #4212 (Family I1, §14.1 / §27.8, v0.298): thin wrapper over the
// shared `sema::substituteGenericArgs` authority. The pre-fix body only
// substituted a TOP-LEVEL bare parameter reference; a buried parameter
// (`Array<T>`, `Dictionary<K, Array<V>>`) was silently missed — the fix
// recurses through Named-with-genericArgs / InlineArray element / Function
// param-and-return, with the same nullable preservation as before.
TypeRef* Resolver::applyGenericSubst(
        TypeRef* tr,
        const std::unordered_map<std::string_view, TypeRef*>& genericSubst) {
    return sema::substituteGenericArgs(
        tr, sema::SubstArgs{ctx_, genericSubst});
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

// fix #2391 (§14.2/§2.8.2, v0.236): the IMPORTED-base analog of
// `constraintSetsEquivalent` above. `d` is the locally-declared override's
// `where T: ...` clause; `b` is the imported base's reconstructed clause set
// (nullptr = base had no clause at all = empty set). The metadata builder
// folds the modifier form (`where T is owned`, §14.4) into the same
// `specialConstraints` list as the regular-kind items (`where T: class`,
// §14.2), so this comparison deliberately treats the derived side's items
// bag and modifier string as ONE combined multiset — semantically identical
// to `constraintSetsEquivalent`'s two-check (modifier equality + items
// multiset) for every valid input, since the grammar admits items XOR a
// modifier on a single clause (the two come from disjoint alphabets).
bool Resolver::derivedConstraintMatchesImported(
    const ast::GenericConstraint* d,
    const ImportedGenericConstraint* b) {
    // Derived-side flattened bag: each item's `kind` string for special kinds
    // (class/value/new/notnull/...), plus the modifier string when populated.
    // Type-constraint items (kind == "type") are matched by TypeRef below, so
    // excluded from the string bag.
    std::vector<std::string_view> dSpec;
    std::vector<TypeRef*> dTypes;
    if (d) {
        for (const auto& item : d->items) {
            if (item.kind == "type") {
                dTypes.push_back(item.typeArg
                                     ? ctx_.resolvedType(*item.typeArg)
                                     : nullptr);
            } else if (!item.kind.empty()) {
                dSpec.push_back(item.kind);
            }
        }
        if (!d->modifierConstraint.empty())
            dSpec.push_back(d->modifierConstraint);
    }
    const auto& bSpec = b ? b->specialConstraints
                          : std::vector<std::string_view>{};
    const auto& bTypes = b ? b->constraintTypes : std::vector<TypeRef*>{};
    if (dSpec.size() != bSpec.size()) return false;
    if (dTypes.size() != bTypes.size()) return false;
    // Multiset match on special-constraint strings.
    std::vector<bool> usedStr(bSpec.size(), false);
    for (std::string_view s : dSpec) {
        bool matched = false;
        for (std::size_t j = 0; j < bSpec.size(); ++j) {
            if (usedStr[j]) continue;
            if (s != bSpec[j]) continue;
            usedStr[j] = true;
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    // Multiset match on type-constraint TypeRefs (after alias resolution).
    std::vector<bool> usedTy(bTypes.size(), false);
    for (TypeRef* t : dTypes) {
        bool matched = false;
        for (std::size_t j = 0; j < bTypes.size(); ++j) {
            if (usedTy[j]) continue;
            if (!typeRefEqual(t, bTypes[j])) continue;
            usedTy[j] = true;
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    return true;
}

// fix #5091 (narrows #5039 and #5075; §14.2, v0.307): when the override
// already known to the calling check omitted its own `where` clause on
// type parameter `gi`, climb same-module ancestors one class at a time
// until we find one that actually writes an explicit `where` clause (or
// an imported ancestor's side table), and copy THAT clause's items onto
// the override's own `GenericParamSymbol::constraints`. Codegen, and
// every later constraint consumer, reads the OVERRIDE's symbol — not the
// base's — and a chained omission (`A.M <- B.M <- C.M` all omitting
// `where`, recommended spelling per §14.2) needs more than the IMMEDIATE
// base's own AST/side-table entry to populate it.
//
// #5091's narrower hazard: a base class that overloads the inherited
// method name with a SAME-arity sibling of a DIFFERENT shape (a
// `b: string` parameter rather than the real `b: T`) declared BEFORE the
// real overridden overload. The climb must identify the right overload by
// FULL SIGNATURE (the same `overrideParamsMatch` predicate
// `checkHidingAndOverride` uses to pick the overridden declaration at line
// 6536) — exactly the same signature check `checkOverrideSignatureMatch`
// already applies for that one `cur->parent` -> `baseCls` hop, including
// modifier-equality and the same `buildBaseGenericSubst` substitution —
// not just name + param arity + generic arity. A shape-only pick that
// matched the decoy's `(b: string)` by name+arity+gen-arity would silently
// copy the decoy's `where T: new()` onto the leaf override, the codegen
// warning "internal: codegen cannot dispatch member 'Equals' on receiver
// of type 'T'" would fire on the leaf body's `a.Equals(b)`, and the
// behavior #5039/#5075 exist to prevent would resurface through the fix.
//
// The climb deliberately reads the AST chain rather than already-
// materialized `GenericParamSymbol::constraints`: `checkHidingAndOverride`
// visits classes in declaration order (NestedTypeWalk.hpp), and a derived
// class may legally be declared textually BEFORE its own base, so an
// ancestor's own override check may not have run yet at this point in the
// SAME pass. Walking the AST directly is correct regardless of order.
//
// The generic-arity check is kept on top of `overrideParamsMatch`:
// `overrideParamsMatch` compares a method-level type parameter as a
// declaration-less Named ref, so `M<T, U>(a: T, b: U)` and `M<T>(a: T,
// b: T)` compare equal parameter-for-parameter; only the arity tells them
// apart, and `gi` is positional anyway.
//
// When no exact match exists at a level, the climb stops WITHOUT
// materializing — the pre-#5075 behaviour: the override keeps an empty
// set, as it did before the climb existed. That covers an IMPORTED
// candidate of the right shape: an imported method carries only
// `resolvedDeclType` (no `declType`, #2386), which `overrideParamsMatch`
// does not read, so its parameter types cannot be compared here.
static void materializeConstraintUpChain(
        SemaContext& ctx, GenericParamSymbol* dgp, MethodSymbol* member,
        std::size_t gi,
        const std::function<bool(MethodSymbol*, MethodSymbol*)>&
            overrideParamsMatch) {
    MethodSymbol* cur = member;
    while (cur) {
        // SEAM-DEBT: #4988 an imported `cur` has no `decl` (#2386, only
        // `resolvedDeclType`) and no way to read §14.2's per-level generic
        // constraint clauses at all — they are dropped at load (see the
        // audit's §3 "facts with no symbol-side channel" table). The loop
        // stops WITHOUT materializing rather than guessing, matching the
        // pre-#5075 behaviour; draining this needs a new wire carriage for
        // `MetaGenericParam::constraintTypes`, not a local read change.
        if (auto* curDecl = dynamic_cast<const ast::MethodDecl*>(cur->decl)) {
            if (gi >= curDecl->genericParams.size()) return;  // arity mismatch
            std::string_view curName = curDecl->genericParams[gi].name;
            const ast::GenericConstraint* cc = nullptr;
            for (const auto& gc : curDecl->genericConstraints)
                if (gc.typeParam == curName) { cc = &gc; break; }
            if (cc) {
                // §14.4: the modifier form (`where T is owned`) carries no
                // instantiation-time obligation and is never recorded in
                // `constraints` — mirrors `collectGenericParams`'s identical
                // filter. Found an explicit clause at this level either way:
                // stop climbing (an explicit clause, even a modifier-only
                // one, is this level's OWN clause, not a further omission).
                if (cc->modifierConstraint.empty()) {
                    for (const ast::ConstraintItem& ci : cc->items) {
                        if (ci.kind.empty()) continue;
                        GenericParamConstraint c;
                        c.kind = ci.kind;
                        c.astBound = ci.typeArg.get();
                        dgp->constraints.push_back(c);
                    }
                }
                return;
            }
            // Omitted at this level too: climb to `cur`'s own class's direct
            // base (`baseTypes[0]` is the class base by convention; see
            // `TypeSymbol::baseTypes`) and find the same-signature method
            // there, then repeat.
            TypeSymbol* cls = cur->parent;
            if (!cls || cls->baseTypes.empty()) return;
            TypeSymbol* baseCls = cls->baseTypes[0];
            if (!baseCls || baseCls->kind != ast::TypeDeclKind::Class) return;
            MethodSymbol* anc = nullptr;
            for (Symbol* mem : baseCls->members) {
                auto* ms = dynamic_cast<MethodSymbol*>(mem);
                if (!ms || ms->isOperator || ms->name != cur->name ||
                    ms->params.size() != cur->params.size() ||
                    ms->genericParams.size() != cur->genericParams.size())
                    continue;
                // An IMPORTED candidate has no `decl` (only `declType`), so
                // `overrideParamsMatch` cannot verify a full-signature match
                // here (the imported param's type lives on `resolvedDeclType`,
                // see ModuleLoader::convertParams, not on the `declType` that
                // `overrideParamsMatch` reads — and so this comparison would
                // false-reject for every imported member). The loop pre-filter
                // (name + param count + generic arity) is the strongest signal
                // we have for an imported candidate.
                //
                // fix #5075 T8 (§14.2, v0.307): when the imported ancestor's
                // constraint payload is available via
                // `ctx.importedGenericConstraints(ms)` (ModuleLoader,
                // #2391), materialize that set positionally onto `dgp` and
                // stop the climb. This closes the 3-module chain Leaf (local)
                // -> Mid (local, omitted) -> Base (IMPORTED from a different
                // dep): the climb walks Leaf -> Mid (both local, no AST
                // constraint at Mid) -> Base (in another dep, no AST
                // available here). Without consulting the imported side
                // table, Leaf's `T` is left unconstrained and codegen's
                // i32-placeholder fast path takes the wrong branch on
                // `a.Equals(b)` (exit 92 — the original #5075 repro).
                //
                // When the side table is absent (stale metadata, or the base
                // declared no `where` clauses at all for this parameter),
                // fall back to the previous safe behaviour: leave
                // `dgp->constraints` empty. CLAUDE.md rule 4 — the safe
                // direction is the one that preserves the long-standing
                // accept for unrecognised shapes, not the one that tightens.
                if (!dynamic_cast<const ast::MethodDecl*>(ms->decl) &&
                    !ms->params.empty()) {
                    const auto* iGCs = ctx.importedGenericConstraints(ms);
                    if (iGCs && gi < iGCs->size()) {
                        const ImportedGenericConstraint& bic = (*iGCs)[gi];
                        for (std::string_view kind :
                             bic.specialConstraints) {
                            GenericParamConstraint c;
                            c.kind = kind;
                            dgp->constraints.push_back(c);
                        }
                        for (TypeRef* bound : bic.constraintTypes) {
                            GenericParamConstraint c;
                            c.kind = "type";
                            c.importedBound = bound;
                            dgp->constraints.push_back(c);
                        }
                    }
                    return;  // imported ancestor is the climb boundary
                              // (#5091's narrow — no same-module AST past it)
                }
                if (overrideParamsMatch(cur, ms)) {
                    anc = ms;
                    break;
                }
            }
            if (!anc) return;  // chain doesn't continue; bail safely
            cur = anc;
            continue;
        }
        // IMPORTED member: no local AST to climb further with — consult its
        // side table once and stop either way. Mirrors the immediate-base
        // import branch above; the metadata WRITER reads the raw AST `where`
        // when exporting generic constraints, not the sema-materialized
        // symbols (#5076: scope of this fix), which bounds how far past an
        // imported ancestor the same-module AST chain can walk.
        const auto* iGCs = ctx.importedGenericConstraints(cur);
        if (iGCs && gi < iGCs->size()) {
            const ImportedGenericConstraint& bic = (*iGCs)[gi];
            for (std::string_view kind : bic.specialConstraints) {
                GenericParamConstraint c;
                c.kind = kind;
                dgp->constraints.push_back(c);
            }
            for (TypeRef* bound : bic.constraintTypes) {
                GenericParamConstraint c;
                c.kind = "type";
                c.importedBound = bound;
                dgp->constraints.push_back(c);
            }
        }
        return;
    }
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
    // SEAM-ACCESSOR: `derived` is always the LOCAL override (from `t`'s own module, per
    // the #2386 comment 15 lines below) — no import fallback needed for
    // this side (#3152).
    auto* dDecl = dynamic_cast<const ast::MethodDecl*>(derived->decl);
    // SEAM-ACCESSOR: fix #2386 (§12.1, family of #3152): `base` may be an IMPORTED
    // virtual/abstract ancestor member; when this cast is null the code
    // just below falls back to ctx_.importedReturnType/importedReturnOwned
    // instead of silently skipping the return-type/`owned` comparison.
    auto* bDecl = dynamic_cast<const ast::MethodDecl*>(base->decl);
    // §2.8.2/§12.1 (v0.235, #2386): `base` may be an IMPORTED virtual/
    // abstract ancestor member — the BFS at this check's `override`-handling
    // call site walks the full inheritance chain with no same-module
    // restriction, and ModuleLoader never attaches an ast::MethodDecl to a
    // body-less imported declaration (§2.8.2/§2.8.3: the artifact interface
    // carries no AST). The old `if (dDecl && bDecl)` gate made `bDecl`
    // unconditionally null for a cross-module base, silently skipping the
    // ENTIRE return-type/`owned`-return comparison — a false ACCEPT of any
    // override with a mismatched return type. Confirmed: overriding an
    // imported `public abstract method Get(): int;` with `override method
    // Get(): string => "garbage marker string"` compiled clean; dispatched
    // through the base-typed reference, `b.Get()` (statically `int`) read a
    // garbage int off the string body's actual return (observed v=4610944).
    // `derived` is always a LOCAL override (from `t`'s own module), so
    // `dDecl` needs no fallback. Mirrors #2373's fallback for the identical
    // read shape in `checkAbstractImplementations` (same file): fall back to
    // ctx_.importedReturnType/importedReturnOwned (ModuleLoader::
    // setImportedReturnType) when the base has no AST node.
    TypeRef* bRet = nullptr;
    bool bDeclaresReturn = false;
    bool bRetOwned = false;
    if (bDecl) {
        bRet = bDecl->returnType ? ctx_.resolvedType(*bDecl->returnType)
                                 : nullptr;
        bDeclaresReturn = (bDecl->returnType != nullptr);
        bRetOwned = bDecl->returnOwned;
    } else {
        bRetOwned = ctx_.importedReturnOwned(base);
        TypeRef* r = ctx_.importedReturnType(base);
        if (r && r->kind != TypeRef::Kind::Unit) bRet = r;
        bDeclaresReturn = (bRet != nullptr);
    }
    if (dDecl) {
        // Return type (null == unit) and owned-return modifier.
        TypeRef* dRet = dDecl->returnType
            ? ctx_.resolvedType(*dDecl->returnType) : nullptr;
        // Apply substitution to base return type (T → int, etc.).
        TypeRef* bRetSubst = applyGenericSubst(bRet, genericSubst);
        bool bothNullReturn = !dDecl->returnType && !bDeclaresReturn;
        if (!bothNullReturn && !typeRefEqual(dRet, bRetSubst)) {
            err("return type");
        }
        if (dDecl->returnOwned != bRetOwned) {
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
        // §2.8.2/§12.1 (v0.235, #2386): mirrors #2373's ParameterSymbol
        // fallback (checkAbstractImplementations, same file) — an IMPORTED
        // base member's parameter carries no syntactic `declType` at all;
        // ModuleLoader::convertParams stashes the already-resolved type on
        // `resolvedDeclType` instead (there is no ast::Type to attach).
        // Reading `declType` alone made `bt` null for every imported base
        // parameter, silently skipping this whole comparison via
        // `if (dt && btSubst && ...)` below — a further false ACCEPT,
        // independent of the return-type gate above (this loop was never
        // gated on `dDecl`/`bDecl`). `dp` is always a LOCAL override
        // parameter, so `dt` needs no fallback.
        TypeRef* bt = bp->declType ? ctx_.resolvedType(*bp->declType)
                                   : bp->resolvedDeclType;
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
    //
    // #2386/#2391 (v0.236): the base side is no longer required to carry an
    // ast::MethodDecl. When `base` is an IMPORTED generic method, `bDecl` is
    // null (§2.8.2/§2.8.3: the artifact interface carries no AST); the
    // fallback consults `ctx_.importedGenericConstraints(base)` — a per-
    // parameter payload reconstructed from MetaMember::genericParams by
    // ModuleLoader — via `derivedConstraintMatchesImported` (the imported
    // analog of `constraintSetsEquivalent`). Absent side table = stale/
    // pre-#1579 metadata OR a base with no clauses at all for this param —
    // the safe direction (skip the check, preserving long-standing accept)
    // per CLAUDE.md rule 4.
    if (!dDecl) return;
    const auto* importedGCs = (!bDecl)
        ? ctx_.importedGenericConstraints(base) : nullptr;
    if (!bDecl && !importedGCs) return;  // non-generic / stale metadata
    if (dDecl->genericParams.size() != (bDecl ? bDecl->genericParams.size()
                                              : base->genericParams.size()))
        return;  // arity mismatch already diagnosed elsewhere
    for (std::size_t gi = 0; gi < dDecl->genericParams.size(); ++gi) {
        std::string_view dName = dDecl->genericParams[gi].name;
        const ast::GenericConstraint* dc = nullptr;
        for (const auto& gc : dDecl->genericConstraints)
            if (gc.typeParam == dName) { dc = &gc; break; }
        // No `where` clause on the OVERRIDE for this type parameter at
        // all means "exactly the inherited set" — always legal, the
        // recommended spelling; nothing to *compare*. But fix #5039 (§14.2,
        // v0.307): "An override ... inherits the overridden ... declaration's
        // constraints" is not only a restatement-comparison rule — the
        // override's own GenericParamSymbol must actually CARRY that
        // inherited set, because every later reader (codegen's
        // `genericParamSoundAtI32ForPlaceholder`, and any other constraint
        // consumer) asks the OVERRIDE's own symbol, not the base's. Before
        // this fix, an omitted `where` clause left `derived`'s
        // GenericParamSymbol::constraints empty — indistinguishable from a
        // genuinely unconstrained `T` — so a cross-module virtual dispatch
        // into an override compiled from an unconstrained-`T` placeholder
        // body silently lost the `IEquatable<T>`-conformance information the
        // body's `a.Equals(b)` call depends on. Materialize the inherited
        // constraints positionally, from whichever side (`bDecl` /
        // `importedGCs`) `checkOverrideSignatureMatch` already established
        // just above.
        // No `where` clause on the OVERRIDE for this type parameter at
        // all means "exactly the inherited set" — always legal, the
        // recommended spelling; nothing to *compare*. But fix #5039 (§14.2,
        // v0.307): "An override ... inherits the overridden ... declaration's
        // constraints" is not only a restatement-comparison rule — the
        // override's own GenericParamSymbol must actually CARRY that
        // inherited set, because every later reader (codegen's
        // `genericParamSoundAtI32ForPlaceholder`, and any other constraint
        // consumer) asks the OVERRIDE's own symbol, not the base's. Before
        // #5039, an omitted `where` clause left `derived`'s
        // GenericParamSymbol::constraints empty — indistinguishable from a
        // genuinely unconstrained `T` — so a cross-module virtual dispatch
        // into an override compiled from an unconstrained-`T` placeholder
        // body silently lost the `IEquatable<T>`-conformance information the
        // body's `a.Equals(b)` call depends on.
        //
        // fix #5075 (§14.2, v0.307, NARROWED BY #5091): the immediate base
        // may ITSELF have omitted its own clause — a chained omission, as
        // legal per §14.2 as a single-level one. `materializeConstraintUpChain`
        // below walks same-module ancestors one class at a time until it
        // finds one that actually writes a clause (or an imported
        // ancestor's side table) and populates `dgp->constraints` from THAT
        // level, instead of stopping at the immediate base. The walk
        // identifies the ancestor by FULL SIGNATURE (`overrideParamsMatch`,
        // the same predicate `checkHidingAndOverride` uses at line 6536 to
        // pick the overridden overload); a same-arity sibling with
        // different parameter types declared before the real overridden
        // declaration would otherwise leak that sibling's `where T: …`
        // onto the leaf override, regressing #5039/#5075's own fix.
        if (!dc) {
            if (gi < derived->genericParams.size()) {
                auto* dgp = dynamic_cast<GenericParamSymbol*>(
                    derived->genericParams[gi]);
                if (dgp && dgp->constraints.empty()) {
                    materializeConstraintUpChain(
                        ctx_, dgp, base, gi,
                        [this](MethodSymbol* d, MethodSymbol* b) {
                            return overrideParamsMatch(d, b);
                        });
                }
            }
            continue;
        }
        bool mismatch = false;
        if (bDecl) {
            // Same-module base: AST-vs-AST comparison.
            std::string_view bName = bDecl->genericParams[gi].name;
            const ast::GenericConstraint* bc = nullptr;
            for (const auto& gc : bDecl->genericConstraints)
                if (gc.typeParam == bName) { bc = &gc; break; }
            mismatch = !constraintSetsEquivalent(dc, bc);
        } else {
            // #2391: IMPORTED base — consult the side table populated by
            // ModuleLoader from MetaMember::genericParams.
            mismatch = !derivedConstraintMatchesImported(
                dc, gi < importedGCs->size() ? &(*importedGCs)[gi]
                                             : nullptr);
        }
        if (mismatch) {
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
    // Auto body. NOTE (#4037, LANE_RULES §6): replaces the prior inline
    // modifier scan with a call to the shared `isAbstractPropertyShorthand`
    // (sema/Symbol.hpp). After the #4037 fix to that predicate (removing the
    // `p.isIndexer` guard), it is set-equal to the prior inline scan on the
    // NAMED-property domain and also correct on the INDEXER domain (abstract
    // auto-indexers are now correctly identified as shorthand/get-only). The
    // abstract-indexer rows in the #4037 and #4100 regression tests pin the
    // expected outcomes on both domains.
    if (isAbstractPropertyShorthand(*pd)) return false;  // §12.5 shorthand
    for (auto mod : pd->modifiers)
        if (mod == "readonly") return false;
    return true;
}

// #2549 / §11.2 / §23.3 / §2.8.2 clause 1 / §2.8.3 rule 4
// (v0.236): symmetric inverse of propertyDeclHasSetter — does the
// property/indexer declaration have a getter? An expression-bodied property's
// body IS the getter; an Auto-bodied property has the synthesized getter; a
// Block-bodied property has a getter iff it declares a `get` arm. (The §12.5
// abstract-shorthand refinement from propertyDeclHasSetter is irrelevant on
// the getter axis: an abstract Auto-shorthand property still DECLARES a
// getter, just no body to fill it — the iface-side getter-presence question
// is satisfied even for an abstract read-write obligation.)
static bool propertyDeclHasGetter(const ast::PropertyDecl* pd) {
    if (!pd) return false;
    if (pd->bodyKind == ast::PropertyBodyKind::Expression) return true;
    if (pd->bodyKind == ast::PropertyBodyKind::Auto) return true;
    // Block body: has a getter iff any accessor is a `get` arm.
    for (const auto& a : pd->accessors)
        if (!a.isSet) return true;
    return false;
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

    // SEAM-ACCESSOR: `derived` is always the LOCAL override — same reasoning as
    // checkOverrideSignatureMatch's `dDecl` (#2386, #3152).
    auto* dDecl = dynamic_cast<const ast::PropertyDecl*>(derived->decl);
    // SEAM-ACCESSOR: fix #2386 (§12.1/§23, family of #3152): `base` may be an imported
    // ancestor property; when this cast is null, `bTy` two lines below
    // falls back to ctx_.importedMemberType instead of silently skipping
    // the property-type comparison.
    auto* bDecl = dynamic_cast<const ast::PropertyDecl*>(base->decl);
    // §2.8.2/§12.1/§23 (v0.235, #2386): the OVERRIDE-MATCHING twin of #2380's
    // `checkAbstractImplementations` fix (§11.2 conformance, same file) for
    // the identical bDecl-null-for-imported-base gap. ModuleLoader never
    // attaches an ast::PropertyDecl to a body-less imported declaration; the
    // resolved property type instead rides the same importedMemberType side
    // table used for imported fields/consts (ModuleLoader.cpp ~437: "the
    // getter BODY is not reconstructed here"). Falling back there when
    // `bDecl` is null covers the property-TYPE comparison for an override of
    // an imported base member — the confirmed false-accept shape in #2386
    // (overriding an imported `public abstract property Val: int;` with
    // `override property Val: string => ...` compiled clean). `derived` is
    // always a LOCAL override, so `dDecl` needs no fallback.
    TypeRef* bTy = bDecl ? (bDecl->type ? ctx_.resolvedType(*bDecl->type)
                                        : nullptr)
                         : ctx_.importedMemberType(base);
    if (dDecl) {
        // The property type is the getter's return type (and the setter's
        // implicit `value` parameter type). Per §12.1, return types must
        // match exactly — no covariance in this version — so no T→T?
        // widening here (unlike a method parameter or an indexer index
        // parameter, which are inputs).
        TypeRef* dTy =
            dDecl->type ? ctx_.resolvedType(*dDecl->type) : nullptr;
        TypeRef* bTySubst = applyGenericSubst(bTy, genericSubst);
        if (!typeRefEqual(dTy, bTySubst)) {
            err("property type");
        }

        // Accessor presence must match (§12.1): a property that adds or
        // removes a setter relative to the base member changes the vtable
        // slot shape — codegen installs a separate `$set` slot only when a
        // property has a setter (LlvmBackend_Setup.cpp), so a mismatched
        // override leaves a slot unfilled or orphaned. propertyDeclHasSetter
        // is the same predicate codegen uses to decide slot allocation; it
        // reads the base's AST accessor shape directly. For an IMPORTED base
        // (bDecl null — ModuleLoader's Property case never reconstructs an
        // ast::PropertyDecl, §2.8.2/§2.8.3: the artifact interface carries no
        // AST) the bit rides the artifact interface instead: MetaMember::
        // hasSetter, reconstructed by the loader into the #2400 side table
        // ctx_.importedPropertyHasSetter — the accessor-presence analog of
        // the property-TYPE fallback to ctx_.importedMemberType above
        // (#2386). #2390: this fallback was deliberately deferred from
        // #2386 because the hasSetter schema bit had not yet been added;
        // #2400 added it (and codegen's propertyHasSetter already consumes
        // it), so this sema check now consumes it too — closing the
        // imported-base twin of the LOCAL-only gate #2386 had to leave in
        // place.
        //
        // The builder bit mirrors CODEGEN's predicate, which — per
        // propertyDeclHasSetter's own comment — SPURIOUSLY reports a setter
        // for an abstract Auto-shorthand member (`abstract property P: T;` /
        // `abstract property [i]: T;`, §12.5 = get-only): harmless for
        // codegen's vtable slot allocation (an abstract class is never
        // instantiated), but NOT for this semantic match, where it would
        // false-reject the canonical `abstract property`/`abstract indexer`
        // + get-only override idiom (the LOCAL twin of #1614's lit cases).
        // Apply the same §12.5 correction sema's local predicate does.
        // `importedPropertyIsAutoStorage` (#2478, extended by #2523 to cover
        // Auto-body INDEXERS in addition to Auto-body properties) identifies
        // the Auto-shorthand shape precisely — a block-form abstract
        // property OR indexer with a real `set` arm keeps the builder bit
        // (the local path accepts that exotic shape, and so does this one).
        // Before #2523 the metadata bit was property-only
        // (`hasAstAutoPropertyDecl` excluded indexers, #2478), so an abstract
        // indexer could only be detected via the coarse `base->isIndexer`
        // branch — which incorrectly fired for an exotic abstract BLOCK-
        // form indexer with an explicit `set` arm too, false-rejecting the
        // override (the #2390 residual #2523 fixes). Now the bit carries
        // the §12.5-correct shape and the §12.5 correction is exactly
        // `baseAbstract && isAutoShorthand`, with no indexer-specific
        // special case. A pre-#2400 blob defaults the bit to false (see
        // MetaMember::hasSetter's field comment); the project's
        // recompile-the-world model means a stale dependency is recompiled
        // rather than trusted with a stale accessor shape.
        bool bHasSetter;
        if (bDecl) {
            bHasSetter = propertyDeclHasSetter(bDecl);
        } else {
            bHasSetter = ctx_.importedPropertyHasSetter(base);
            if (bHasSetter) {
                bool baseAbstract = false;
                for (auto mod : base->modifiers)
                    if (mod == "abstract") { baseAbstract = true; break; }
                if (baseAbstract &&
                    ctx_.importedPropertyIsAutoStorage(base)) {
                    bHasSetter = false;  // §12.5 abstract shorthand = get-only
                }
            }
        }
        if (propertyDeclHasSetter(dDecl) != bHasSetter) {
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
            // §2.8.2/§12.1 (v0.235, #2386): same ParameterSymbol fallback as
            // the method-override parameter loop above — an imported base
            // indexer's index parameter carries no `declType`, only
            // `resolvedDeclType` (ModuleLoader::convertParams).
            TypeRef* bt =
                bp->declType ? ctx_.resolvedType(*bp->declType)
                             : bp->resolvedDeclType;
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

// §12.2 / #1749: the hiding/override check runs over `t` (if it's a class)
// plus every class reachable through its NestedTypeSymbol members,
// transitively — §11.6 states nested types "may themselves contain nested
// types" with no depth bound, so a class nested inside a nested class must be
// visited too. isNestedWithin (used above by memberBlocksHiding's `private`
// case) already assumes that transitive reach.
//
// fix #3574: the collector this used to call (`collectHidingCheckClasses`) was
// a hand-copy of the same recursion; it now routes through the shared
// `forEachTypeAndNested`. The pre-order visit sequence is identical — the old
// copy also descended through nested types of EVERY kind and filtered only
// what it PUSHED, which is what the lambda below does.
void Resolver::checkHidingAndOverride() {
    if (!ctx_.module()) return;
    std::vector<TypeSymbol*> classes;
    forEachTypeAndNested([&](TypeSymbol* t) {
        if (t->kind == ast::TypeDeclKind::Class) classes.push_back(t);
    });
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
        // fix #2984 (§12.2 / §31.4 / §12.1, v0.240): the frontier below is
        // seeded from `t->baseTypes`, and a BUILT-IN exception base is never in
        // that collection — it has no `TypeSymbol` at all. So the inherited-
        // member set this check walks was EMPTY for every class deriving from
        // one, and §12.2 — "Declaring a member in a derived class with the same
        // name as ANY inherited member visible at the derived class's
        // declaration site is a compile error" — was simply not enforced
        // against the §31.4 surface. That is the SILENT direction of the same
        // absence #2983 hit loudly: a positive check keyed on a collection that
        // never held the population fails by ACCEPTING.
        //
        // `sema::builtinExceptionBaseOf` is the ONE walk that answers whether
        // there is such a base (#2950), and `builtinExceptionSurfaceMemberOf`
        // the ONE statement of what it declares (#2971); the reject below asks
        // both, so it can never block a name the resolver would not have
        // resolved, nor miss one it would.
        std::string_view builtinExcBase = builtinExceptionBaseOf(t);
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
        // fix #2984: a class whose ONLY base is a built-in exception has both
        // maps empty, so this early-out used to skip its derived-member loop
        // entirely — the reject below would never have been reached.
        if (baseMembers.empty() && baseOperators.empty() &&
            builtinExcBase.empty())
            continue;
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
            if (it == baseMembers.end()) {
                // fix #2984 (§12.2 / §31.4, v0.240): no USER ancestor declares
                // this name — but a BUILT-IN exception ancestor may, and it
                // contributed nothing to `baseMembers` above. §31.4 declares
                // every one of these `public readonly property`, and §12.2's
                // blocked-name set says "`public` and `protected` base members
                // always block"; none is `virtual`, so no `override` escape
                // exists and the declaration has no legal form.
                //
                // MEASURED on 0dac23f, why this matters and why the accepted
                // program is worse than it looks: `class ExOwn: Exception` with
                // its own `public property Message: string => _m;` and
                // `base("header")` compiled with ZERO diagnostics, and the SAME
                // OBJECT then answered `Message` two different ways —
                //     this.Message  ->  shadow   (the declared property)
                //     p.Message     ->  header   (the inherited §31.4 slot)
                // — both through the subclass's own type. The object carries
                // the §31.4 header (`classStruct` inserts it for any class
                // `builtinExceptionBaseOf` accepts), so it really does have two
                // `Message`s, which is precisely the hazard §12.2 exists to
                // reject.
                //
                // Checked only after `baseMembers` misses, so a user ancestor
                // that (illegally) declares the same name reports once, on that
                // ancestor's own declaration, rather than twice down the chain.
                std::string_view excOwner;
                if (!builtinExcBase.empty() &&
                    builtinExceptionSurfaceMemberOf(builtinExcBase, ms->name,
                                                    &excOwner) !=
                        BuiltinExcMember::None) {
                    // The message names the built-in that DECLARES the member,
                    // not the class's own base — §31.4 declares `StackTrace` on
                    // `Exception` and `ParamName` on `ArgumentException`, so a
                    // class under `ArgumentOutOfRangeException` inherits them
                    // from different levels. Naming the base for both would
                    // state something false about the user's hierarchy, which
                    // is the #2983 defect in miniature; the walk that finds the
                    // member reports where it came from.
                    ctx_.diagnostics().report(
                        Severity::Error, ms->declLoc,
                        std::string("member `") + std::string(ms->name) +
                        "` hides the inherited `" + std::string(ms->name) +
                        "` declared by the built-in `" +
                        std::string(excOwner) +
                        "` (\xC2\xA7""31.4); hiding is forbidden — that member "
                        "is not `virtual`, so `override` is not available "
                        "either; choose a different name (\xC2\xA7""12.2)");
                }
                continue;
            }
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
                    // fix #3611 (§22.16 / §12.1, v0.286): "Clauses bind
                    // overrides and implementations ([§12.1], [§11.2]): they
                    // are inherited, never restated; an override or
                    // implementation may add `ensures` clauses and may not
                    // declare `requires` clauses." The two halves are wired
                    // here together: REJECT a `requires` the override
                    // declared (the "never restated" half), then COPY the
                    // base's `requires` clauses onto the override's symbol
                    // for codegen to lower in the prologue (the "inherited"
                    // half — without it, the override's prologue would be
                    // empty and a `d.Half(-4)` call would bypass the base's
                    // `requires(x > 0)` and divide a negative number).
                    // Ensures inheritance is intentionally NOT widened here
                    // (a separate spec question; this lane is contract-clause
                    // inheritance, not clause-merging). Local bases only —
                    // imported bases have no AST on `base->decl` and the
                    // cross-module direct call is already covered by the
                    // exporter's own prologue (brief probe 1).
                    // SEAM-DEBT: #3611 contract-clause inheritance — the
                    // `requires`-on-override rejection and the base's
                    // requires-copy both need the AST nodes' clause
                    // vectors; `MemberSymbol` does not carry the AST-shaped
                    // `contractClauses`, so the dynamic_cast is unavoidable
                    // until a clause-shaped field is added to MemberSymbol.
                    if (auto* dMD = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                        for (const auto& c : dMD->contractClauses) {
                            if (c.kind == ast::ContractClause::Kind::Requires) {
                                ctx_.diagnostics().report(
                                    Severity::Error, c.loc,
                                    std::string("override of `") +
                                    std::string(ms->name) +
                                    "` may not declare a `requires` clause — "
                                    "requires are inherited from the base "
                                    "member, never restated (\xC2\xA7""22.16)");
                            }
                        }
                    }
                    // SEAM-DEBT: #3611 contract-clause inheritance — see the
                    // marker 19 lines above; the base-side read is the
                    // second half of the same pair (override rejection +
                    // base-copy).
                    if (auto* bMD = dynamic_cast<const ast::MethodDecl*>(base->decl)) {
                        ms->inheritedRequiresClauses.reserve(
                            ms->inheritedRequiresClauses.size() +
                            bMD->contractClauses.size());
                        for (const auto& c : bMD->contractClauses) {
                            if (c.kind == ast::ContractClause::Kind::Requires) {
                                ms->inheritedRequiresClauses.push_back(&c);
                            }
                        }
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
    // SEAM-ACCESSOR: `ms->modifiers` (checked just above) already carries "abstract" for
    // an imported method — ModuleLoader.cpp:758 wires MethodSymbol
    // modifiers on both sides (#3152's audit §3). This AST fallback only
    // WIDENS the local, syntax-derived case (`abstract method Foo();` with
    // no explicit modifier token is not a real shape, but if it were, this
    // is where it would be caught); the cast is unreachable for an import
    // because the modifiers check above already returned true.
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
        // §2.8.2/§12.5 (#2373): the §11.2 instance-member twin of #2359 bug
        // 2d's `checkStaticInterfaceConformance::subst()` fix. `d` is a
        // bare-type-parameter reference read off the interface/abstract
        // member's own declared signature; when that member is IMPORTED
        // (an interface reached from another module), its declared type
        // carries no `astNode` at all — only `importedName`
        // (ModuleLoader::convert). The old `astNode`-only guard made this
        // substitution a silent no-op for an imported ancestor's bare `T`,
        // so the return/param type stayed unsubstituted "T" and the
        // subsequent `typesMatch` comparison against the implementation's
        // concrete type spuriously failed ("does not match ... on
        // parameter"/"does not match ... return type" — a false REJECT of a
        // correct generic implementation). Read the name via the shared
        // alias-transparent `TypeRef::headName()` (falls back to
        // `importedName`) instead, matching #2359's fix shape at its four
        // sibling sites.
        if (!d->declaration && !d->headName().empty() && d->genericArgs.empty()) {
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
        // #2416 / §11.2 / §9.6: records may implement interfaces exactly like
        // classes ("A record may implement any number of interfaces" — §9.6)
        // and are bound by the identical §11.2 conformance obligations, but
        // this function's outer loop previously filtered to Class only, so a
        // record got NONE of the checks below — not #489's "field doesn't
        // satisfy a property" check, not #2380's property-type-match check,
        // and not #2413's accessor-set check. That reproduced #2413's exact
        // null-itable-setter-slot SIGSEGV for a record's getter-only property
        // satisfying a read-write interface property obligation (confirmed by
        // probe before this fix). Widened to admit Record here; each check
        // below was individually audited for class-specific assumptions (see
        // the per-check comments) before being left enabled for records.
        const char* kindWord =
            (t->kind == ast::TypeDeclKind::Record) ? "record" : "class";
        if (t->kind != ast::TypeDeclKind::Class &&
            t->kind != ast::TypeDeclKind::Record)
            continue;
        // #2416: `isAbstract` — records cannot be inherited from and are
        // implicitly sealed (§9.6), and the grammar's shared Modifier*
        // production over-admits `abstract` on a RecordDecl the same way it
        // over-admits `ref`/`out`/`scoped` on primary-constructor parameters
        // (§22.10) — sema is the semantic gate, per the parser's own comment
        // at parseTopDecl. No existing check rejects `abstract record`
        // (verified: checkClassModifierCombinations, the only §11.7 modifier-
        // combination check, filters to Class only and never reports on a
        // record). Whether `abstract record` should itself be a compile error
        // is a separate, pre-existing question this fix does not decide either
        // way — out of scope for #2416 (filed as #2431); this loop simply
        // preserves the existing "abstract types need not implement
        // obligations" reading for whichever type kind carries the modifier.
        //
        // §12.5 (v0.242, #3052): "A class containing one or more abstract
        // members must itself be `abstract` (or `closed`, §11.7)" — a
        // `closed` class "may declare abstract members without carrying the
        // `abstract` modifier — `closed` subsumes it". This loop is where
        // that rule is ENFORCED (a non-abstract class declaring an abstract
        // member reaches the "does not implement inherited abstract member"
        // report below, since the BFS starts at `t` itself), so `closed` must
        // join the skip or every `closed` class with an abstract member would
        // be rejected.
        const bool isAbstract = typeIsAbstractOrClosed(t);
        if (isAbstract) continue;  // abstract / `closed` classes need not impl.

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
        // fix #4020 / §12.5 (sibling to #3641's ifaceIndexerObligations): a
        // class-declared `abstract property Name: T;` (the §12.5 abstract
        // shorthand, sema/Symbol.hpp's `isAbstractPropertyShorthand`) is an
        // OBLIGATION some class in the chain must satisfy with a real
        // (non-abstract) property — the NAMED-property twin of `abstractNames`
        // above. Before this fix the property loop below unconditionally
        // inserted every class PropertySymbol, abstract or not, into
        // `concretePropertyNames` (the SATISFYING set) — so a class's own
        // abstract property satisfied itself, and neither this obligation
        // check nor the separate "class containing an abstract member must
        // itself be abstract/closed" check (this function's `isAbstract`
        // early-continue above) ever saw an obligation to report. Unlike
        // indexers, a named property has a name to key on, so this is a flat
        // set (like `abstractNames`), not `ifaceIndexerObligations`'
        // anonymous-obligation vector.
        std::unordered_set<std::string_view> abstractPropertyNames;
        // R39 / §11.2: also remember the PropertySymbol (and, for interface
        // members, the declaring interface) so the implementing property's TYPE
        // can be checked against the interface's — return/property types must
        // match exactly (no covariance). Keyed by name; first writer wins, so
        // the most-derived class property (BFS starts at `t`) is kept.
        std::unordered_map<std::string_view,
                           std::pair<PropertySymbol*, TypeSymbol*>> ifaceProps;
        std::unordered_map<std::string_view, PropertySymbol*> concreteProps;
        // §9.1 / #2416: a RECORD's positional field auto-generates a public
        // read-only property of the same name (§9.1), but is represented
        // internally as a FieldSymbol with `posParam` set, not a
        // PropertySymbol (#815 — the optimization codegen's
        // classPositionalFieldProperty / the box-vtable Pass C fallback both
        // key off). concreteProps above is typed to PropertySymbol* and
        // cannot hold it, so it is tracked separately here — the accessor-set
        // (#2413) and type-match (#2380) checks below both consult this map
        // when concreteProps has no entry, so a positional-field satisfaction
        // is checked exactly like a declared property one (always getter-
        // only, since §9.1's generated property never has a setter).
        std::unordered_map<std::string_view, FieldSymbol*> concretePosFields;
        // #2417 / §11.2 / §23.4: interface INDEXER obligations, tracked
        // separately from ifaceProps just above (whose loop below skips
        // indexers outright) because indexers are ANONYMOUS — §23.4: "An
        // indexer is a property whose name is replaced by `[parameters]`" —
        // so there is no name to key a map on, and §23.4 permits overloading
        // by index-parameter type list ("Indexers may be overloaded by
        // parameter type list"). Kept as a flat vector, not deduped: two
        // DIFFERENT interfaces each declaring a read-write indexer are two
        // separate obligations. Checked after the BFS below (search
        // "ifaceIndexerObligations" for the check itself).
        struct IfaceIndexerObligation {
            PropertySymbol* ips;
            TypeSymbol* iface;
        };
        std::vector<IfaceIndexerObligation> ifaceIndexerObligations;
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
        // fix #4831 (§11.1 / §11.2, v0.307): each binding is keyed by the
        // FULL structural identity of the bound type — declaration, generic
        // arguments (recursively), nullability, and the inline-array /
        // function shape — not by `headName()` alone. The head-only key
        // spelled `T=Box<int>`, `T=Box<string>`, `T=Box<T>` and `T=Box?` all
        // as `T=Box;`, so `class D: IM<Box<int>>, IM<Box<string>>` visited
        // `IM` once, recorded ONE obligation, and never checked the second —
        // §11.1 requires §11.2's matching rule "applied once per obligation,
        // each under its own instantiation's substitution". The unchecked
        // obligation then dispatched through a type-confused itable slot.
        std::function<void(const TypeRef*, std::string&)> typeKey =
            [&](const TypeRef* v, std::string& out) {
            if (!v) { out += "~"; return; }
            switch (v->kind) {
            case TypeRef::Kind::Error: out += "E"; break;
            case TypeRef::Kind::Unit: out += "U"; break;
            case TypeRef::Kind::Primitive:
                out += "P";
                out += std::to_string(static_cast<int>(v->primitive));
                break;
            case TypeRef::Kind::Named:
                out += "N";
                if (v->declaration) {
                    out += "@";
                    out += std::to_string(
                        reinterpret_cast<std::uintptr_t>(v->declaration));
                } else {
                    out += "'";
                    out += std::string(v->headName());
                }
                out += "<";
                for (const TypeRef* ga : v->genericArgs) {
                    typeKey(ga, out);
                    out += ",";
                }
                out += ">";
                break;
            case TypeRef::Kind::InlineArray:
                out += "A";
                out += std::to_string(v->arraySizeValue);
                out += v->arrayOwned ? "o" : "";
                out += "[";
                typeKey(v->arrayElement, out);
                out += "]";
                break;
            case TypeRef::Kind::Function:
                out += "F(";
                for (const auto& fp : v->funcParams) {
                    out += std::string(fp.modifier);
                    out += ":";
                    typeKey(fp.type, out);
                    out += ",";
                }
                out += ")";
                out += v->funcReturnOwned ? "o" : "";
                typeKey(v->funcReturn, out);
                break;
            }
            if (v->nullable) out += "?";
        };
        auto substKey = [&](
            const std::unordered_map<std::string_view, const TypeRef*>& m)
            -> std::string {
            if (m.empty()) return std::string();
            std::vector<std::pair<std::string_view, std::string>> parts;
            parts.reserve(m.size());
            for (auto& [k, v] : m) {
                std::string vk;
                typeKey(v, vk);
                parts.emplace_back(k, std::move(vk));
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
        // fix #5048 (§14.2 clause 2 / §9.2, v0.307): a record that lists
        // `IEquatable<Self>` satisfies the constraint via the synthesized
        // `Equals(T)` witness. The BFS below populates `concreteNames` from
        // `TypeSymbol::members`, but `Equals(T)` / `GetHashCode()` are added
        // only at codegen time (LlvmBackend_Setup.cpp), not during sema — so
        // the record's own Equals is absent from `concreteNames` when the
        // BFS reaches the record, while `IEquatable<Self>`'s `Equals(T)` IS
        // in `abstractNames`, producing a false "does not implement inherited
        // abstract member" error below.
        //
        // fix #5095 (§9.1 / §12.5 / §2.3, v0.307): the synthesized witness
        // satisfies EXACTLY ONE obligation — the auto-imported (stdlib §2.9)
        // `IEquatable<T>.Equals(T)` at T = Self — and nothing else. Three
        // shapes the first cut admitted must keep the §12.5 reject:
        //   (a) a USER `interface IEquatable<T>` shadowing the name (§2.3
        //       permits it; #3139's precedent): identity, not name — the
        //       base must carry `isSynthesizedBuiltinInterface`;
        //   (b) `record Pair<T>(...): IEquatable<Pair<int>>`: the type
        //       argument must be Self in FULL — the record's own declaration
        //       applied to its own generic parameters, in order — not just
        //       the same head declaration;
        //   (c) `record Point(...): IEquatable<Point>, IEquatable<int>`: the
        //       `IEquatable<int>` entry is a second, separate obligation the
        //       synthesized `Equals(Point)` does not witness.
        // So the injection is decided AFTER the BFS: it happens only when
        // every abstract `Equals` obligation the BFS records is the builtin
        // IEquatable's at exactly Self, reached through `t`'s own inheritance
        // clause. Any other abstract `Equals` (a non-Self instantiation, a
        // shadow interface, a user interface declaring `Equals`, or builtin
        // IEquatable reached only through another interface) sets
        // `equalsNeedsRealWitness`, and the record then needs a written
        // `Equals` exactly as before #5048. Codegen's
        // `isSynthesizedIEquatableEquals` (#5068) tests builtin identity and
        // relies on this check for the Self-ness of the listing.
        TypeSymbol* builtinIEq =
            ctx_.builtinInterfaceSymIfCached("IEquatable");
        if (!builtinIEq) {
            for (TypeSymbol* b : t->baseTypes) {
                if (b && b->isSynthesizedBuiltinInterface &&
                    b->name == "IEquatable" && b->genericParams.size() == 1 &&
                    b->genericParams[0]) {
                    builtinIEq = b;
                    break;
                }
            }
        }
        // (b): is `ref` exactly Self — `t` at its own generic parameters?
        auto isExactlySelf = [&](const TypeRef* ref) -> bool {
            if (!ref || ref->kind != TypeRef::Kind::Named || ref->nullable ||
                ref->declaration != t)
                return false;
            if (ref->genericArgs.size() != t->genericParams.size())
                return false;
            for (std::size_t gi = 0; gi < t->genericParams.size(); ++gi) {
                const TypeRef* ga = ref->genericArgs[gi];
                const Symbol* gp = t->genericParams[gi];
                if (!ga || !gp || ga->kind != TypeRef::Kind::Named ||
                    ga->declaration || ga->nullable ||
                    !ga->genericArgs.empty() || ga->headName() != gp->name)
                    return false;
            }
            return true;
        };
        bool listsBuiltinIEqAtSelf = false;
        if (t->kind == ast::TypeDeclKind::Record && builtinIEq) {
            for (std::size_t bi = 0; bi < t->baseTypes.size(); ++bi) {
                if (t->baseTypes[bi] != builtinIEq ||
                    bi >= t->baseTypeRefs.size())
                    continue;
                TypeRef* bref = t->baseTypeRefs[bi];
                if (bref && bref->genericArgs.size() == 1 &&
                    isExactlySelf(bref->genericArgs[0])) {
                    listsBuiltinIEqAtSelf = true;
                    break;
                }
            }
        }
        // fix #5139 (§14.2 cl 2, v0.309): a record that lists an interface
        // extending `IEquatable<Self>` (`record Point: IMy` where
        // `IMy: IEquatable<Point>`) satisfies the constraint via the
        // synthesized `Equals(T)` witness the same way a direct listing does.
        // `listsBuiltinIEqAtSelf` above only sees the direct listing; this
        // companion flag tracks the indirect path through the BFS below. The
        // injection at the bottom still gates on
        // `!equalsNeedsRealWitness`, so any non-Self `IEquatable<U>` reached
        // through an extending interface (the new T4 negative row) keeps the
        // §12.5 reject.
        bool seesBuiltinIEqAtSelfViaInterface = false;
        bool equalsNeedsRealWitness = false;
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
                        // fix #5095: see `equalsNeedsRealWitness` above.
                        if (ms->name == "Equals") {
                            const TypeRef* selfArg = nullptr;
                            if (curIsIface && cur == builtinIEq &&
                                !builtinIEq->genericParams.empty()) {
                                auto sit = curSubst.find(
                                    builtinIEq->genericParams[0]->name);
                                if (sit != curSubst.end())
                                    selfArg = sit->second;
                            }
                            if (!isExactlySelf(selfArg))
                                equalsNeedsRealWitness = true;
                        }
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
                    if (ps->isIndexer) {
                        // #2417 / §23.5 "Static indexers are not supported" —
                        // the isStatic guard is defensive parity with the
                        // named-property branch below (and with codegen's
                        // own indexer helpers, which carry the identical
                        // guard despite the same restriction).
                        bool isStaticIdx = false;
                        for (auto mod : ps->modifiers)
                            if (mod == "static") { isStaticIdx = true; break; }
                        if (curIsIface && !isStaticIdx) {
                            ifaceIndexerObligations.push_back({ps, cur});
                        } else if (!curIsIface && !isStaticIdx &&
                                   hasMod(ps, "abstract")) {
                            // fix #3641 / §12.5: "`abstract` on a member ...
                            // every concrete subclass must override it" — the
                            // abstractNames set above only tracks MethodSymbol
                            // obligations (`if (auto* ms = dynamic_cast<
                            // MethodSymbol*>(m))`), so a CLASS-declared
                            // abstract indexer previously reached the
                            // unconditional `continue` below with NO
                            // obligation recorded anywhere — a concrete
                            // subclass that never overrides it compiled clean
                            // and SIGSEGV'd at the first indexer access
                            // (codegen's vtable slot for the indexer stays
                            // `ConstantPointerNull`; #3641's own measured
                            // repro). Reuse ifaceIndexerObligations/its
                            // matching loop below rather than inventing a
                            // parallel structure — the "find a wireable
                            // matching indexer on t's own members or its base-
                            // class chain" search (§11.2/§23.4's #2437/#2442/
                            // #2449 machinery) is exactly the same search a
                            // §12.5 obligation needs; only the reported
                            // diagnostic differs by `obl.iface->kind` below.
                            ifaceIndexerObligations.push_back({ps, cur});
                        }
                        continue;  // indexers have no name; concrete side is
                                   // matched directly off `t->members` below,
                                   // not tracked through this BFS (mirrors
                                   // codegen's classIndexerImplementing,
                                   // which never walks the base chain either
                                   // — see the check itself for why).
                    }
                    bool isStatic = false;
                    for (auto mod : ps->modifiers)
                        if (mod == "static") { isStatic = true; break; }
                    if (isStatic) continue;
                    if (curIsIface) {
                        // Record the interface's required property name.
                        ifacePropertyNames.insert(ps->name);
                        ifaceProps.emplace(ps->name, std::make_pair(ps, cur));
                    } else if (hasMod(ps, "abstract")) {
                        // fix #4020 / §12.5: mirrors the `methodIsAbstract`
                        // arm above and #3641's indexer arm just above this
                        // one — a class-declared abstract property is an
                        // OBLIGATION, not a satisfying implementation. Do NOT
                        // fall through to the `concretePropertyNames` arm
                        // below or this obligation self-satisfies (the exact
                        // #4020 SIGSEGV: `class Base { abstract property
                        // Name: string; }` compiled clean because `Base`'s
                        // own abstract property was accepted as proof `Base`
                        // implements `Base`).
                        abstractPropertyNames.insert(ps->name);
                    } else {
                        // A class PropertySymbol satisfies the contract.
                        concretePropertyNames.insert(ps->name);
                        concreteProps.emplace(ps->name, ps);
                    }
                }
                // §9.1 / #2416: a record's POSITIONAL field stands in for a
                // public read-only property (see concretePosFields'
                // declaration above) — record it as a satisfying candidate
                // the same way the PropertySymbol branch above does. Gated on
                // `cur->kind == Record` specifically, NOT merely `fs->
                // posParam != nullptr`: a CLASS's primary-constructor
                // parameter also produces a posParam-carrying FieldSymbol
                // (§22.10), but §22.10 is explicit that a class's primary-
                // ctor parameters are "not automatically promoted to public
                // properties" — unlike a record's positional fields (§9.1),
                // a class's positional field must NOT satisfy a property
                // obligation. `cur` here is never a record reached from a
                // class's ancestor chain (§9.6: records cannot be inherited
                // from, and cannot derive from a class), so this is
                // equivalent to (and simpler than) gating on `t->kind`.
                if (cur->kind == ast::TypeDeclKind::Record) {
                    if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                        if (fs->posParam) {
                            concretePropertyNames.insert(fs->name);
                            concretePosFields.emplace(fs->name, fs);
                        }
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
                // fix #5139 (§14.2 cl 2, v0.309): when builtin IEquatable is
                // reached through ANOTHER interface, the synthesized
                // `Equals(T)` still satisfies the obligation IF and ONLY IF
                // the type argument at this extension is exactly Self
                // (`record Point: IMy` where `IMy: IEquatable<Point>` is the
                // §14.2 cl 2 admit case; `IMy: IEquatable<int>` stays a
                // §12.5 reject — the synthesized `Equals(Point)` is not a
                // witness for `IEquatable<int>.Equals(int)`). Pre-#5139 this
                // was a blanket `equalsNeedsRealWitness = true`, which
                // rejected the indirect listing unconditionally. The
                // Self-ness check uses the same `isExactlySelf` predicate
                // that the direct-listing check above uses, applied to the
                // type argument in the EXTENDING interface's baseTypeRefs
                // (e.g. the `Point` in `IMy: IEquatable<Point>`) after
                // substitution through curSubst — for a non-generic
                // extending interface (the spec's case) curSubst is empty
                // and the literal TypeRef's declaration is compared against
                // `t` directly.
                if (cur != t && b == builtinIEq && builtinIEq) {
                    bool selfTyped = false;
                    if (bi < cur->baseTypeRefs.size()) {
                        TypeRef* bref = cur->baseTypeRefs[bi];
                        if (bref && bref->genericArgs.size() == 1) {
                            const TypeRef* selfArg = substTypeRef(
                                bref->genericArgs[0], curSubst);
                            selfTyped = isExactlySelf(selfArg);
                        }
                    }
                    if (selfTyped) {
                        seesBuiltinIEqAtSelfViaInterface = true;
                    } else {
                        equalsNeedsRealWitness = true;
                    }
                }
                std::string key = seenKey(b, bSubst);
                if (!seen.insert(key).second) continue;
                frontier.push_back(FrontierNode{b, std::move(bSubst)});
            }
        }
        // fix #5048 / #5095 (§9.1 / §14.2 clause 2 / §12.5, v0.307): inject
        // the synthesized `Equals(T)` for the one obligation it witnesses —
        // see the comment above `builtinIEq`. A record with a written
        // `Equals` already has it in `concreteNames` from the BFS.
        if ((listsBuiltinIEqAtSelf || seesBuiltinIEqAtSelfViaInterface) &&
            !equalsNeedsRealWitness &&
            (isPositionalRecordEqualityBearing(t, ctx_) ||
             isAutoEqualityBearingBodyRecord(t, ctx_))) {
            concreteNames.insert("Equals");
            concreteArities["Equals"].insert(1);  // Equals(T): 1 param
        }
        for (auto name : abstractNames) {
            if (!concreteNames.count(name)) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("concrete ") + kindWord + " `" +
                    std::string(t->name) +
                    "` does not implement inherited abstract member `" +
                    std::string(name) + "` (§12.5)");
            }
        }
        // fix #4020 / §12.5: the NAMED-property twin of the method check just
        // above (see `abstractPropertyNames`' declaration comment). Reports
        // the identical diagnostic shape the method arm uses — §12.5 draws no
        // distinction between a method and a property for this obligation
        // (§23.4 extends the same participation to indexers, #3641's already-
        // fixed sibling).
        for (auto name : abstractPropertyNames) {
            if (!concretePropertyNames.count(name)) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("concrete ") + kindWord + " `" +
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
                        std::string(kindWord) + " `" + std::string(t->name) +
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
                // fix #5097 (§11.2, v0.307, related #4836): a class may
                // legitimately declare TWO same-name, same-arity overloads
                // to witness TWO obligations from the same generic interface
                // at different instantiations (`Equals(other: Point)` for
                // `IEquatable<Point>` and `Equals(other: int)` for
                // `IEquatable<int>`). The pre-existing walk below took the
                // FIRST same-arity overload unconditionally, which is exactly
                // right when there is only one candidate but silently picks
                // the WRONG one of two when both exist — and after this fix
                // resolves the interface's bare-`T` parameter type (see the
                // `bareT`/`setResolvedType` fix in SemaContext.hpp), the
                // subsequent §11.2 parameter-type check below would then
                // reject the correct overload set as a false mismatch. Search
                // every same-arity, same-name candidate on `c` for one whose
                // parameter types actually match THIS obligation (under its
                // substitution) before falling back to the first-found
                // candidate, so the existing "first same-arity overload wins"
                // behavior is preserved only when no candidate's types match
                // — the shape #4836 is about — and a genuinely overloaded,
                // correctly-typed pair is no longer confused for one another.
                auto paramsMatchObligation = [&](MethodSymbol* candidate,
                                                  TypeSymbol* declClass) -> bool {
                    if (candidate->params.size() != im->params.size())
                        return false;
                    std::unordered_map<std::string_view, const TypeRef*>
                        candSubst;
                    auto it2 = classSubst.find(declClass);
                    if (it2 != classSubst.end()) candSubst = it2->second;
                    for (std::size_t pi = 0; pi < im->params.size(); ++pi) {
                        auto* ip = dynamic_cast<ParameterSymbol*>(im->params[pi]);
                        auto* cp = dynamic_cast<ParameterSymbol*>(
                            candidate->params[pi]);
                        if (!ip || !cp) return false;
                        TypeRef* it = ip->declType ? resolvedOf(ip->declType)
                                                    : ip->resolvedDeclType;
                        TypeRef* ct = cp->declType ? resolvedOf(cp->declType)
                                                    : cp->resolvedDeclType;
                        if (!it || !ct) return false;
                        if (it->kind == TypeRef::Kind::Error ||
                            ct->kind == TypeRef::Kind::Error) return false;
                        const TypeRef* itSub = substTypeRef(it, obl.subst);
                        const TypeRef* ctSub = substTypeRef(ct, candSubst);
                        bool match = typesMatch(itSub, ctSub);
                        if (!match && itSub && ctSub &&
                            !itSub->nullable && ctSub->nullable) {
                            TypeRef stripped = *ctSub;
                            stripped.nullable = false;
                            match = typesMatch(itSub, &stripped);
                        }
                        if (!match) return false;
                    }
                    return true;
                };
                TypeSymbol* c = t;
                while (c && !impl) {
                    MethodSymbol* firstArityMatch = nullptr;
                    TypeSymbol* firstArityMatchClass = nullptr;
                    for (Symbol* m : c->members) {
                        auto* ms = dynamic_cast<MethodSymbol*>(m);
                        if (!ms || ms->isOperator || hasMod(ms, "static") ||
                            ms->name != im->name || methodIsAbstract(ms))
                            continue;
                        if (ms->params.size() == im->params.size()) {
                            if (!firstArityMatch) {
                                firstArityMatch = ms;
                                firstArityMatchClass = c;
                            }
                            // fix #5097: prefer a candidate whose parameter
                            // types actually witness THIS obligation.
                            if (paramsMatchObligation(ms, c)) {
                                impl = ms;
                                implClass = c;
                                break;
                            }
                            continue;
                        }
                        if (!nameOnly) nameOnly = ms;
                    }
                    if (impl) break;
                    if (firstArityMatch) {
                        // No candidate's types matched — fall back to the
                        // first same-arity one, exactly as before this fix,
                        // so the existing §11.2 mismatch diagnostic still
                        // fires for a genuinely wrong-typed sole witness.
                        impl = firstArityMatch;
                        implClass = firstArityMatchClass;
                        break;
                    }
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
                // fix #3194: WHERE a §11.2 conformance diagnostic points. An
                // IMPORTED witness has no location in THIS compilation —
                // `Symbol::declLoc` defaults to 0:0 — and a diagnostic at 0:0
                // names nothing the user can act on (the shape #3178 records
                // for `checkImportedCallArity`). Point at the local
                // conformance declaration instead: the class whose
                // `: Base, IFace` clause is the only thing this module can
                // change. A LOCAL witness keeps `impl->declLoc` exactly, so no
                // existing diagnostic moves.
                const bool implSymIsImported = ctx_.isImported(impl);
                const SourceLocation implLoc =
                    implSymIsImported ? t->declLoc : impl->declLoc;
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
                        Severity::Error, implLoc,
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
                // SEAM-ACCESSOR: fix #3194 (family M13 of #3152): `im` may be an IMPORTED interface/
                // abstract member. When this cast is null, `imRetRef`/`imRetOwned` a
                // few lines below fall back to ctx_.importedReturnType/
                // importedReturnOwned instead of silently treating the member as
                // returning nothing.
                auto* imDecl = dynamic_cast<const ast::MethodDecl*>(im->decl);
                auto* implDecl =
                    // SEAM-ACCESSOR: fix #3194: the implementation-side twin of the `im->decl` fallback
                    // just above — gated on `implSymIsImported` (ctx_.isImported), which
                    // falls back to ctx_.importedReturnType/importedReturnOwned rather
                    // than defaulting to "unknown" for every decl-less witness.
                    dynamic_cast<const ast::MethodDecl*>(impl->decl);
                // §2.8.2/§12.5 (#2373): `im` may be an IMPORTED interface/
                // abstract member. ModuleLoader never attaches an
                // ast::MethodDecl to a body-less imported member (there is
                // no syntactic AST to attach), so `imDecl` is unconditionally
                // null for every cross-module obligation — which silently
                // skipped BOTH this `owned`-return check and the return-type
                // match below for any imported interface (a false ACCEPT of
                // any return-type/`owned`-return mismatch). Derive the
                // interface member's return the same way #2294 already
                // established for `checkStaticInterfaceConformance`'s twin
                // loop: from the AST when present, else from
                // ctx_.importedReturnType/importedReturnOwned (ModuleLoader::
                // setImportedReturnType).
                //
                // fix #3194 (§11.2 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.254;
                // family M13 of umbrella #3152): the comment that used to end
                // this paragraph — "`impl` is always a LOCAL class member
                // (obligation witnesses only ever come from `t`'s own module's
                // BFS above), so `implDecl` needs no fallback" — WAS FALSE, and
                // the same false claim is repeated at the head of this loop and
                // again below the parameter walk. The witness BFS 60 lines above
                // walks `c->baseTypes`, which CROSSES MODULES, so `impl` is an
                // IMPORTED MethodSymbol whenever a local class satisfies an
                // obligation through a method it inherits from an imported base.
                // The sibling comment in this same function (at the §11.8 static
                // loop) states the opposite and correct thing.
                //
                // With `implDecl` null, all three signature axes silently did
                // not run. MEASURED, `bezelc -c lib.bzl` then `--import lib.o`,
                // each against a character-identical single-module twin that
                // reports the §11.2 error and produces no executable:
                //   return type  `Get(): int` vs iface `Get(): string`
                //                -> compiles, then SIGSEGV (an int driven
                //                   through `bz.string`)
                //   param  type  `Take(x: int)` vs iface `Take(x: string)`
                //                -> compiles and returns a GARBAGE INT from a
                //                   truncated string pointer. ⚠ The exact value is
                //                   LAYOUT-DEPENDENT — this lane measured 225, the
                //                   review measured 241 stably on a byte-identical
                //                   repro. Do not quote it as a constant; what is
                //                   invariant is that it is garbage and the twin
                //                   rejects.
                //   `owned` ret   `Make(): owned T` vs iface `Make(): T`
                //                -> compiles, LSan: "Direct leak of 16 byte(s)
                //                   ... in bz$LibM13.BaseM13$Make"
                // The audit frames the `owned` bind as unwinnable — dropping
                // `owned` leaks, tightening it double-disposes. §11.2 gives the
                // third answer and this restores it: REJECT the mismatch, which
                // is what the single-module twin already does.
                const bool imHasAstDecl = (imDecl != nullptr);
                const ast::Type* imRetNode =
                    imDecl ? imDecl->returnType.get() : nullptr;
                bool imRetOwned = imDecl ? imDecl->returnOwned : false;
                TypeRef* imRetRef = nullptr;
                bool imDeclaresReturn;
                if (imHasAstDecl) {
                    imRetRef = resolvedOf(imRetNode);
                    imDeclaresReturn = (imRetNode != nullptr);
                } else {
                    imRetOwned = ctx_.importedReturnOwned(im);
                    TypeRef* r = ctx_.importedReturnType(im);
                    if (r && r->kind != TypeRef::Kind::Unit) imRetRef = r;
                    imDeclaresReturn = (imRetRef != nullptr);
                }
                // fix #3194: the IMPLEMENTATION side of the same both-sources
                // derivation the interface side got in #2373 — AST when there
                // is one, else the loader's `setImportedReturnType` record.
                //
                // ⚠ THE GATE IS `implRetKnown`, NOT "always". Widening the old
                // `implDecl &&` to nothing would newly admit every OTHER
                // decl-less witness — a local MethodSymbol carrying some non-
                // `ast::MethodDecl` node, for which `importedReturnOwned`
                // answers a DEFAULT `false` rather than a fact, and a default
                // read as a fact is exactly how a fix of this family turns a
                // false accept into a false reject. `ctx_.isImported(impl)`
                // (every symbol `SemaContext::makeImportedSymbol` mints is in
                // that set, including member symbols) is the discriminator
                // that says a record EXISTS to read; everything else keeps
                // today's skip, byte for byte.
                const bool implHasAstDecl = (implDecl != nullptr);
                const bool implIsImported =
                    !implHasAstDecl && implSymIsImported;
                const bool implRetKnown = implHasAstDecl || implIsImported;
                const bool implRetOwned =
                    implHasAstDecl ? implDecl->returnOwned
                                   : (implIsImported
                                          ? ctx_.importedReturnOwned(impl)
                                          : false);
                TypeRef* implRetRef = nullptr;
                bool implDeclaresReturn = false;
                if (implHasAstDecl) {
                    implRetRef = resolvedOf(implDecl->returnType.get());
                    implDeclaresReturn = (implDecl->returnType.get() != nullptr);
                } else if (implIsImported) {
                    TypeRef* r = ctx_.importedReturnType(impl);
                    // A Unit return is "declares no return type", exactly as a
                    // null `returnType` node is on the AST side — the same
                    // normalisation the `im` branch above performs.
                    if (r && r->kind != TypeRef::Kind::Unit) implRetRef = r;
                    implDeclaresReturn = (implRetRef != nullptr);
                }
                if (implRetKnown && imRetOwned != implRetOwned) {
                    ctx_.diagnostics().report(
                        Severity::Error, implLoc,
                        std::string("method `") + std::string(name) +
                        "` does not match interface member `" +
                        std::string(name) + "`'s `owned` return modifier "
                        "(§11.2): the interface declares the return type " +
                        (imRetOwned
                             ? std::string("`owned`")
                             : std::string("without `owned`")) +
                        "; the implementation declares it " +
                        (implRetOwned
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
                    // SEAM-ACCESSOR: `implPure` is already read from `hasMod(impl, "pure")` (symbol-side,
                    // correct for imports) just above; this OperatorDecl cast only WIDENS
                    // it further for a LOCAL operator whose purity rides the AST
                    // (`OperatorDecl::isPure` is not mirrored onto `modifiers`). Null for
                    // an import leaves `implPure` at its already-correct symbol-side
                    // value — no silent answer change (#3152).
                    if (auto* op = dynamic_cast<const ast::OperatorDecl*>(
                            impl->decl))
                        implPure = op->isPure;
                if (imPure && !implPure) {
                    ctx_.diagnostics().report(
                        Severity::Error, implLoc,
                        std::string("method `") + std::string(name) +
                        "` implements pure interface member `" +
                        std::string(name) +
                        "` and must be declared `pure` — purity is never "
                        "dropped across an interface implementation "
                        "(\xC2\xA7""22.15)");
                }
                // §14.2 / A.4 (v0.314, #5248): an implementing method's
                // `where` clause must match the interface method's constraint
                // set exactly (or be omitted to inherit it). A differing clause
                // is a compile error. Constraints are compared positionally by
                // the method's own generic parameter (not the interface's
                // substitution — method type parameters are fresh per
                // declaration and are not mapped by the interface substitution).
                // Only report a mismatch when the interface DECLARES a
                // constraint the implementation does not match — a constraint
                // the interface does not declare is inherited from the
                // implementation's own inheritance chain, and comparing it
                // against the interface's empty set would false-reject a legal
                // override that tightens or adds constraints (§14.2).
                {
                    auto findGC = [](const std::vector<ast::GenericConstraint>& v,
                                     std::string_view name)
                        -> const ast::GenericConstraint* {
                        for (const auto& gc : v)
                            if (gc.typeParam == name) return &gc;
                        return nullptr;
                    };
                    // imDecl: non-null when the interface is locally declared.
                    const bool imIsLocal = (imDecl != nullptr);
                    // implDecl: non-null when the implementation is locally declared.
                    const bool implIsLocal = (implDecl != nullptr);
                    // implIsImported: the impl came from an imported base class
                    // (ctx_.isImported, cached in implSymIsImported above).
                    const bool implIsImp = !implIsLocal && implSymIsImported;
                    // Number of method generic parameters on each side (for
                    // positional correspondence). Use the interface's param
                    // count as canonical; impl must match (arity check above).
                    std::size_t gpCount = imIsLocal
                        ? imDecl->genericParams.size()
                        : im->genericParams.size();
                    // Fetch the imported constraint side-tables if either side
                    // is imported.
                    const std::vector<ImportedGenericConstraint>*
                        imImpGCs = nullptr;
                    if (!imIsLocal) {
                        imImpGCs = ctx_.importedGenericConstraints(im);
                    }
                    const std::vector<ImportedGenericConstraint>*
                        implImpGCs = nullptr;
                    if (implIsImp) {
                        implImpGCs = ctx_.importedGenericConstraints(impl);
                    }
                    for (std::size_t gi = 0; gi < gpCount; ++gi) {
                        std::string_view imGpName;
                        if (imIsLocal) {
                            imGpName = imDecl->genericParams[gi].name;
                        } else if (imImpGCs && gi < imImpGCs->size()) {
                            imGpName = (*imImpGCs)[gi].typeParam;
                        } else {
                            continue;  // no constraint side-table entry
                        }
                        const ast::GenericConstraint* imGC =
                            imIsLocal
                                ? findGC(imDecl->genericConstraints, imGpName)
                                : nullptr;
                        // impl's constraint for this position
                        std::string_view implGpName;
                        if (implIsLocal) {
                            implGpName = implDecl->genericParams[gi].name;
                        } else if (implImpGCs && gi < implImpGCs->size()) {
                            implGpName = (*implImpGCs)[gi].typeParam;
                        } else {
                            continue;
                        }
                        const ast::GenericConstraint* implGC =
                            implIsLocal
                                ? findGC(implDecl->genericConstraints, implGpName)
                                : nullptr;
                        // If impl declares no constraint, it inherits — always
                        // legal (§14.2: "omit the clause to inherit it exactly").
                        if (!implGC) continue;
                        // The interface declares a constraint the impl does not
                        // match — report an error.
                        bool match = false;
                        if (imIsLocal && implIsLocal) {
                            match = constraintSetsEquivalent(imGC, implGC);
                        } else if (!imIsLocal && implIsLocal) {
                            // Interface imported, impl local: build a temporary
                            // AST constraint from the imported metadata.
                            if (imImpGCs && gi < imImpGCs->size()) {
                                const auto& igc = (*imImpGCs)[gi];
                                ast::GenericConstraint tmp;
                                tmp.typeParam = igc.typeParam;
                                for (std::string_view sv : igc.specialConstraints) {
                                    tmp.items.emplace_back(sv, nullptr);
                                }
                                // Type constraints require resolved TypeRefs, which
                                // are not reconstructable from metadata — skip
                                // the check when type constraints are present.
                                bool hasTypeConstraint = false;
                                for (const auto& ig : igc.constraintTypes) {
                                    (void)ig; hasTypeConstraint = true;
                                }
                                if (!hasTypeConstraint) {
                                    match = constraintSetsEquivalent(&tmp, implGC);
                                }
                            }
                        } else if (imIsLocal && !implIsLocal) {
                            // Interface local, impl imported: use the imported
                            // comparison helper directly.
                            if (implImpGCs && gi < implImpGCs->size()) {
                                match = derivedConstraintMatchesImported(
                                    implGC, &(*implImpGCs)[gi]);
                            }
                        } else {
                            // Both imported: compare imported-to-imported via
                            // temporary AST constraints (special-constraint bag
                            // only — type constraints need resolved TypeRefs and
                            // are not reconstructable from metadata).
                            if (imImpGCs && gi < imImpGCs->size() &&
                                implImpGCs && gi < implImpGCs->size()) {
                                const auto& igc = (*imImpGCs)[gi];
                                const auto& egc = (*implImpGCs)[gi];
                                ast::GenericConstraint tmpI;
                                tmpI.typeParam = igc.typeParam;
                                for (std::string_view sv : igc.specialConstraints) {
                                    tmpI.items.emplace_back(sv, nullptr);
                                }
                                ast::GenericConstraint tmpE;
                                tmpE.typeParam = egc.typeParam;
                                for (std::string_view sv : egc.specialConstraints) {
                                    tmpE.items.emplace_back(sv, nullptr);
                                }
                                match = constraintSetsEquivalent(&tmpI, &tmpE);
                            }
                        }
                        if (!match) {
                            ctx_.diagnostics().report(
                                Severity::Error, implLoc,
                                std::string("method `") +
                                std::string(name) +
                                "` implements interface member `" +
                                std::string(name) +
                                "` and must restate an equivalent "
                                "`where` clause — omit the clause to "
                                "inherit it exactly, or restate the "
                                "identical set (§14.2)");
                            break;  // one diagnostic per member
                        }
                    }
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
                    // §2.8.2/§12.5 (#2373): `im` (and so `ip`) may belong to
                    // an IMPORTED interface/abstract member — its parameters
                    // carry no syntactic `declType` at all;
                    // ModuleLoader::convertParams stashes the already-
                    // resolved type on `resolvedDeclType` instead (no
                    // ast::Type exists to attach). Reading `declType` alone
                    // made `it` null for every imported obligation, which
                    // silently skipped this whole parameter-type check (a
                    // false ACCEPT of any parameter-type mismatch) — the
                    // fallback mirrors #2294's established fix for
                    // `checkStaticInterfaceConformance`'s twin loop.
                    //
                    // fix #3194: the sentence that used to end this paragraph
                    // — "`cp` is always a LOCAL class member here (impl
                    // candidates only ever come from `t`'s own module's BFS
                    // above), so only the interface side needs the fallback"
                    // — is FALSE for the same reason as its twin above: the
                    // BFS walks `c->baseTypes` across modules. `cp->declType`
                    // was therefore null for an inherited-from-imported
                    // witness, `ct` was null, and this whole parameter-type
                    // check `continue`d — a false ACCEPT measured as a silent
                    // wrong value (an `int` formal fed a `string` argument
                    // through the itable, returning 225). The fallback is the
                    // SAME one the interface side has carried since #2373,
                    // written the same way one line up.
                    TypeRef* it = ip->declType ? resolvedOf(ip->declType)
                                                : ip->resolvedDeclType;
                    TypeRef* ct = cp->declType ? resolvedOf(cp->declType)
                                                : cp->resolvedDeclType;
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
                        Severity::Error, implLoc,
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
                //
                // §2.8.2/§12.5 (#2373): reuses `imRetRef`/`imDeclaresReturn`
                // derived above (AST for a local interface, ctx_.
                // importedReturnType for an imported one) rather than
                // re-deriving from `imDecl` here — `imDecl` is unconditionally
                // null for an imported member, so re-deriving from it (the
                // pre-fix shape) silently skipped this entire return-type
                // check for every cross-module obligation.
                //
                // fix #3194: and the same is now true of the IMPLEMENTATION
                // side. `implRetRef`/`implDeclaresReturn` are derived above
                // from the AST when there is one and from
                // ctx_.importedReturnType otherwise, so a witness inherited
                // from an IMPORTED base is compared instead of skipped. This
                // axis is the one whose false accept was memory-unsafe:
                // `Get(): int` satisfying `Get(): string` put an int-returning
                // function in the itable slot, and the caller drove the
                // returned 7 through `bz.string` (measured SIGSEGV).
                if (imDeclaresReturn && implDeclaresReturn) {
                    TypeRef* crt = implRetRef;
                    if (imRetRef && crt &&
                        imRetRef->kind != TypeRef::Kind::Error &&
                        crt->kind != TypeRef::Kind::Error) {
                        const TypeRef* irtSub = substTypeRef(imRetRef, obl.subst);
                        const TypeRef* crtSub = substTypeRef(crt, implSubst);
                        if (!typesMatch(irtSub, crtSub)) {
                            ctx_.diagnostics().report(
                                Severity::Error, implLoc,
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
                    std::string(kindWord) + " `" + std::string(t->name) +
                    "` does not implement interface property `" +
                    std::string(name) +
                    "` — a field does not satisfy a property contract;"
                    " declare `public property " + std::string(name) +
                    ": T` (§11.2 / §23)");
            }
        }
        // §11.2 / §23.3 (v0.235, #2413): the ACCESSOR-SET sibling of #489's
        // "field doesn't satisfy a property" check just above and #2380's
        // property TYPE-match check below. §11.2's "matches" rule requires an
        // implementation to provide the accessors the interface obligation
        // declares; §11.2's diamond-resolution note states the one-directional
        // relaxation explicitly — "A read-write member satisfies a read-only
        // interface member of the same name and type" — which by omission
        // means the converse does NOT hold: a read-only (getter-only) member
        // does not satisfy a read-write interface property obligation.
        // Before this fix, only the INTERFACE's own declared accessor set
        // gated whether codegen's declareVtables Pass D
        // (LlvmBackend_Setup.cpp) allocated an itable setter slot for that
        // interface property — the ACTUAL implementing property's accessor
        // set was never consulted here. A getter-only class property was
        // therefore accepted as satisfying a read-write interface property
        // obligation; ensurePropertyAccessorFn(ts, impl, isSet=true)
        // correctly returns nullptr for the getter-only impl (its
        // PropertyBodyKind::Expression branch), so Pass D silently installed
        // ConstantPointerNull in the unfillable setter slot with no
        // diagnostic anywhere — `iface.Prop := v` then called through that
        // null pointer (SIGSEGV, confirmed via gdb: RIP=0x0).
        //
        // Accessor PRESENCE is a shape fact, not a typed one — unlike the
        // type-match check below, substituting a generic interface's type
        // parameters does not change which accessors it declares, so this
        // check is intentionally NOT gated on t->genericParams.empty() /
        // iface->genericParams.empty() (it is sound, and strictly more
        // complete, to run it unconditionally).
        //
        // #2542 (§11.2 / §23.3 / §2.8.2 clause 1 / §2.8.3 rule 4,
        // v0.236): an imported interface property's PropertySymbol has no
        // ast::PropertyDecl (ModuleLoader reconstructs the symbol but cannot
        // reconstruct source AST), so the former `if (!ipd || !cpd) continue`
        // silently skipped this accessor-presence obligation. The setter bit
        // already crosses the module boundary as MetaMember::hasSetter and is
        // published through ctx_.importedPropertyHasSetter (#2400); read that
        // same bit when either side lacks a local AST, exactly as the sibling
        // iface-indexer check does (#2529). This also covers a witness inherited
        // from an imported base class and the positional-record branch below.
        //
        // #2549 / §11.2 / §23.3 / §2.8.2 clause 1 / §2.8.3 rule 4
        // (v0.236): the symmetric getter axis. The setter check below
        // rejects a getter-only impl of a read-write iface property (the
        // missing-setter side); its mirror — a setter-only impl of a
        // read-write iface property (the missing-getter side, the inverse
        // of #2542's NAMED-property twin) — was silently ACCEPTED: the
        // iface was read-write (the Auto-body kind has both arms by default),
        // the impl had a Block body with only a `set` arm (no `get` arm),
        // and `ifaceHasSetter() && !candidateHasSetter` never fired because
        // the impl DID declare a setter. The runtime then dispatched through
        // a null getter itable slot and SIGSEGV'd at pc=0x0. The added
        // check is the getter-axis mirror of the setter check below: when
        // the iface declares a getter (every non-write-only iface property
        // does — Auto bodyKind with no `readonly` modifier, as iface
        // members always are), the candidate must also declare one. Block
        // body with a `set` arm but no `get` arm is the exact setter-only
        // implementation this rejects.
        //
        // Both the setter (#2542) and getter (#2549) checks run in BOTH
        // the LOCAL and IMPORTED shapes via the same fallback lambdas —
        // the `importedPropertyHasSetter` and `importedPropertyHasGetter`
        // side tables (#2400's `MetaMember::hasSetter` and #2461's
        // `MetaMember::hasGetter` bits, already published to SemaContext)
        // carry the accessor presence across the module boundary, so a
        // getter-only or setter-only impl of an IMPORTED read-write iface
        // property can be rejected on either side. The getter-only pattern
        // in IMPORTED tests was cleaned up in the #2542 re-attempt
        // (#2552 follow-up: legacy #2401/#2387 fixtures now declare
        // `readonly` to match the corrected spec-correct behavior). The
        // setter-only pattern is rare in the corpus (swept manually
        // during the #2549 fix across tests/module/ and tests/sema/ —
        // every iface-property impl in those directories is either
        // expression-bodied, full block (get + set), or positional-record
        // — none pair a setter-only block with a read-write iface), so
        // the IMPORTED-side fallbacks ship alongside the local fix.
        //
        // The accessor-presence bit is shape-factual (#2413's note), not
        // type-dependent, so these checks are intentionally NOT gated on
        // t->genericParams.empty() / iface->genericParams.empty(). Mirror
        // the indexer-side `ifaceHasSetter`/`candidateHasSetter` lambdas in
        // this same file's ifaceIndexerObligations loop a few hundred lines
        // below (#2529/#2449).
        for (auto& [name, info] : ifaceProps) {
            PropertySymbol* ips = info.first;
            TypeSymbol* iface = info.second;
            // SEAM-ACCESSOR: fix #2400/#2414/#2529 (family of #3152): `ifaceHasSetter` already
            // falls back to ctx_.importedPropertyHasSetter(ips) when this AST cast
            // is null — see the lambda body two lines below.
            auto* ipd = dynamic_cast<const ast::PropertyDecl*>(ips->decl);
            auto ifaceHasSetter = [&]() -> bool {
                if (ipd) return propertyDeclHasSetter(ipd);
                return ctx_.importedPropertyHasSetter(ips);
            };
            auto cit = concreteProps.find(name);
            if (cit != concreteProps.end()) {
                auto* cpd =
                    // SEAM-ACCESSOR: fix #2449 (family of #3152): `candidateHasSetter`/`candidateHasGetter`
                    // just below already fall back to
                    // ctx_.importedPropertyHasSetter/HasGetter when `cpd` is null.
                    dynamic_cast<const ast::PropertyDecl*>(cit->second->decl);
                const bool candidateHasSetter =
                    cpd ? propertyDeclHasSetter(cpd)
                        : ctx_.importedPropertyHasSetter(cit->second);
                const bool candidateHasGetter =
                    cpd ? propertyDeclHasGetter(cpd)
                        : ctx_.importedPropertyHasGetter(cit->second);
                // SETTER check (gate fires for both LOCAL and IMPORTED via
                // the candidateHasSetter fallback above; the W9-D sibling
                // originally gated this on `ipd && cpd` because it
                // considered the IMPORTED-side setter check speculative,
                // but the W9-A re-attempt (with test fixture cleanup)
                // closed the #2552 follow-up so the IMPORTED-side check
                // is now sound).
                if (ifaceHasSetter() && !candidateHasSetter) {
                    // Reported at t->declLoc, not cit->second->declLoc: probed
                    // and confirmed (during this fix) that PropertySymbol::
                    // declLoc — like every other non-TypeSymbol member kind — is
                    // never populated by SymbolCollection.cpp's collectMember
                    // (only TypeSymbol::declLoc is set, a few hundred lines away
                    // in a different function), so it always reads as 0:0. That
                    // pre-existing, broadly-scoped gap is out of scope for #2413
                    // (filed separately); t->declLoc is populated and already
                    // the choice the sibling #489 "missing interface property"
                    // diagnostic three lines above uses for the same reason.
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("property `") + std::string(name) +
                        "` implements interface `" + std::string(iface->name) +
                        "` with a getter-only implementation, but `" +
                        std::string(iface->name) + "." + std::string(name) +
                        "` is declared read-write — an implementation of a "
                        "read-write interface property must provide a setter "
                        "(§11.2 / §23.3)");
                }
                // #2549 / §11.2 / §23.3 / §2.8.2 clause 1 / §2.8.3 rule 4
                // (v0.236): the symmetric getter-axis check. Mirrors the
                // setter check above: a read-write iface property obligation
                // requires the implementation to declare a getter.
                // Expression-bodied / Auto-bodied impls always have one
                // (§23.3's read-only-on-block-form rule); a Block impl has
                // a getter iff it declares a `get` arm. A Block with only a
                // `set` arm is the exact setter-only impl this rejects.
                //
                // Unlike the setter check above, this check runs in BOTH the
                // LOCAL and IMPORTED shapes — the
                // `importedPropertyHasGetter` side table (#2461's
                // `MetaMember::hasGetter` bit, published by ModuleLoader)
                // carries the accessor presence across the module boundary,
                // so a setter-only impl of an IMPORTED read-write iface
                // property (the inverse of #2542's NAMED-property twin,
                // #2549) can be rejected on either side. The setter-only
                // pattern is rare in the corpus (swept manually across
                // tests/module/ and tests/sema/ — every iface-property
                // impl in those directories is either expression-bodied,
                // full block (get + set), or positional-record — none pair
                // a setter-only block with a read-write iface), so the
                // IMPORTED-side fallback has no legacy fixtures to clean
                // up and ships alongside the local fix in this single
                // commit. The setter check above is LOCAL-only for the
                // separate reason #2552 documents: the legacy
                // #2401/#2387 tests paired getter-only impls with read-
                // write iface obligations and would false-reject without a
                // fixture cleanup pass (separate lane).
                //
                // The accessor-presence bit is shape-factual (#2413's
                // note), not type-dependent, so this check is
                // intentionally NOT gated on t->genericParams.empty() /
                // iface->genericParams.empty(). Mirrors the indexer-side
                // `ifaceHasSetter`/`candidateHasSetter` lambdas in this
                // same file's ifaceIndexerObligations loop a few hundred
                // lines below (#2529/#2449).
                auto ifaceHasGetter = [&]() -> bool {
                    return ipd ? propertyDeclHasGetter(ipd)
                               : ctx_.importedPropertyHasGetter(ips);
                };
                if (ifaceHasGetter() && !candidateHasGetter) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("property `") + std::string(name) +
                        "` implements interface `" + std::string(iface->name) +
                        "` with a setter-only implementation, but `" +
                        std::string(iface->name) + "." + std::string(name) +
                        "` is declared read-write — an implementation of a "
                        "read-write interface property must provide a getter "
                        "(§11.2 / §23.3)");
                }
                continue;
            }
            // §9.1 / #2416: satisfied by a RECORD's positional-field-backed
            // property (concretePosFields, see its declaration above) instead
            // of a declared PropertySymbol. §9.1's positional-field property
            // is always getter-only ("Properties auto-generated from
            // positional fields are read-only" — §9.4), so — exactly like a
            // getter-only PropertySymbol above — it cannot satisfy a
            // read-write interface property obligation; codegen has no setter
            // to install there either (the identical null-itable-setter-slot
            // SIGSEGV shape #2413 fixed, reproduced for this record shape and
            // confirmed by probe before this fix).
            auto pfit = concretePosFields.find(name);
            if (pfit == concretePosFields.end())
                continue;  // missing → reported above
            if (ifaceHasSetter()) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string("property `") + std::string(name) +
                    "` implements interface `" + std::string(iface->name) +
                    "` with a positional field (§9.1), which is always "
                    "getter-only, but `" + std::string(iface->name) + "." +
                    std::string(name) + "` is declared read-write — an "
                    "implementation of a read-write interface property must "
                    "provide a setter (§9.1 / §9.4 / §11.2 / §23.3)");
            }
        }
        // #2417 / §11.2 / §23.4: the INDEXER sibling of #2413's/#2416's
        // accessor-set check just above. `declareVtables` Pass D
        // (LlvmBackend_Setup.cpp, the `ip->isIndexer` branch) installs
        // `ConstantPointerNull` into an interface indexer's setter itable
        // slot whenever the implementing class's matched indexer has no
        // setter, with no diagnostic anywhere — `iface[i] := v` then calls
        // through that null pointer (SIGSEGV, confirmed by the issue's own
        // repro via gdb: RIP=0x0). Exactly #2413's bug, on the sibling
        // accessor-presence gap §23.4 shares with §23.3 ("An indexer is a
        // property whose name is replaced by `[parameters]`").
        //
        // MATCHING (the hard part): indexers are anonymous, and §23.4 admits
        // overloading by index-parameter type list ("Indexers may be
        // overloaded by parameter type list"), so — unlike a plain property,
        // matched by name — the obligation must be paired with a concrete
        // candidate by parameter shape. The candidate this check selects is
        // not a fresh, independent §11.2 full-parameter-list matcher; it
        // deliberately MIRRORS the exact selection codegen's own
        // `classIndexerImplementing`/`findIndexer` (LlvmBackend_Assign.cpp)
        // uses to fill the itable slot, so sema never accepts a candidate
        // codegen would not itself install (a false ACCEPT of the exact
        // SIGSEGV shape this check exists to close — see #2417's own commit
        // for why an independent, "more correct" matcher was considered and
        // rejected).
        //
        // #2437 / §11.2 / §23.4 (v0.235, landed in its own prior commit —
        // codegen's `findIndexer` carried the identical defect and was fixed
        // together with this mirror then, per the mirroring above; #2442
        // just below is a SEPARATE, later commit that builds on top of it):
        // matching is by the interface indexer's FULL parameter list —
        // arity AND every parameter type — not just the first. The prior
        // first-parameter-only match matched a `[i:int,j:int]` obligation
        // against a class's narrower `[i:int]` overload whenever the two
        // shared a first parameter type (confirmed: installed the wrong
        // getter, silently returning a value from the WRONG indexer — the
        // issue's own repro). There is also no more "fall back to the
        // first-declared indexer when nothing matches exactly": that
        // fallback was an UNBOUNDED version of the same arity-blind defect
        // (it would accept ANY indexer of ANY shape as "the implementation"
        // once no exact match existed). `candidate == nullptr` after a full
        // search is now the sole, correctly-scoped "no implementation"
        // condition — see the `!candidate` branch below.
        //
        // #2442 / §11.2 / §23.4 (v0.235): the search also now walks `t`'s
        // base CLASS chain (mirroring `classPropertyByName`'s identical walk
        // for named properties, a few hundred lines above), not `t->members`
        // alone: §11.2 says a class "implements an interface implicitly: by
        // declaring matching public members" and imposes no restriction that
        // the match be declared directly on the implementing type (ordinary
        // member inheritance applies, exactly as it does for methods via
        // `findMethodByName` and properties via `classPropertyByName`), so a
        // class satisfying the obligation only through an INHERITED indexer
        // (declared on a base class, never redeclared) is a valid
        // implementation.
        //
        // #2449 / §11.2 / §23.4 (v0.235): a candidate reached through the
        // base walk is accepted when it is either LOCAL (a genuine
        // `ast::PropertyDecl`, `candWireable` below) or a wireable IMPORTED
        // indexer — non-static, non-virtual, not itself an interface member.
        // `candWireable`/`candParamCount`/`candParamTy` below deliberately
        // mirror codegen's `indexerCandidateWireable`/`indexerParamCount`/
        // `indexerParamType` (LlvmBackend_Assign.cpp/-_impl.hpp) exactly:
        // #2449's codegen half teaches `ensureIndexerGetterFn`/`-SetterFn`'s
        // caller (`Emitter::indexerItableAccessor`, LlvmBackend_Setup.cpp
        // Pass D) to install #2414's already-declared cross-module ABI stub
        // (`bz$<Class>$idxget$<arity>`/`$idxset$<arity>`,
        // `declareImportedIndexerGetters`/`-Setters`) for a candidate that
        // has no local body to build one from — so an inherited indexer from
        // an IMPORTED base is now ALSO actually wireable, not just an
        // inherited LOCAL one. A VIRTUAL (or interface-declared) imported
        // candidate stays excluded: it dispatches through a vtable/itable
        // slot instead of this direct-call ABI, and
        // declareImportedIndexerGetters/-Setters themselves never declare a
        // stub for one — accepting it here would be a false ACCEPT of a
        // program codegen still cannot wire, the exact anti-pattern this
        // whole check exists to avoid. `t` itself is always LOCAL (this
        // function's outer loop walks `ctx_.module()->children` only), so
        // `cur` only becomes IMPORTED after the first `baseTypes` hop.
        //
        // Accessor PRESENCE is a shape fact, not a typed one (see the
        // sibling property check's identical note above) — substituting a
        // generic interface's type parameters does not change which
        // accessors an indexer declares, so this runs unconditionally, not
        // gated on genericParams.empty().
        //
        // Records get this check for free: this function's outer loop
        // already admits Record (#2416), and an indexer obligation is
        // resolved the same way regardless of `t->kind` (unlike a named
        // property, an indexer has no positional-field analog to special-
        // case — §9.1's auto-generated property applies to named fields
        // only). §23.4 ("positional-only records may not [declare read-
        // write indexers]") is not re-derived here: a positional-only
        // record's indexer, if it declares one at all, is necessarily
        // getter-only, so it already falls out of this same check with no
        // extra machinery.
        //
        // #2438 / §11.2 / §23.4: this loop ALSO reports when `t` (own
        // members plus its base-class chain) provides no matching indexer AT
        // ALL — the more severe sibling #2417 itself deferred:
        // `classIndexerImplementing` returns null in that shape, so
        // `declareVtables` Pass D installs `ConstantPointerNull` in the
        // GETTER slot too, not just the setter (confirmed by probe: a plain
        // READ SIGSEGVs, exit 139, even for a `readonly` obligation — the
        // getter-only-vs-read-write branch below never even runs in that
        // shape, since there is no accessor set to compare against). This is
        // NOT implemented by widening #489's own ifacePropertyNames/
        // concretePropertyNames name-keyed "does not implement interface
        // property" check a few hundred lines above (the naturally-looking
        // fix, and the literal reading of "extend the existing check"):
        // EVERY indexer's PropertySymbol::name is the same synthetic
        // ".this" (SymbolCollection.cpp), regardless of parameter list —
        // that collision is exactly why indexers were excluded from that
        // name-keyed check in the first place (pre-dating both #2417 and
        // #2438). Widening it would report `t` as satisfying ANY interface
        // indexer obligation as soon as `t` declares ANY indexer of ANY
        // shape — a false ACCEPT for a class whose only indexer has the
        // wrong parameter list. This loop's own candidate search is already
        // shape-aware (mirrors codegen's classIndexerImplementing/
        // findIndexer, per the comment above), so `candidate == nullptr`
        // here is the already-computed, sound condition.
        //
        // #2529 (§11.2 / §23.4 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.236):
        // for an IMPORTED interface indexer — `ips->decl` is null, because
        // ModuleLoader's Indexer case (#2414) reconstructs `indexerParams`
        // as ParameterSymbols but attaches no `ast::PropertyDecl` (there is
        // no syntactic AST to attach — "the getter BODY is not reconstructed
        // here") — the obligation was silently SKIPPED via `if (!ipd)
        // continue;` below. A getter-only impl of an imported read-write
        // interface indexer therefore compiled clean (verified: `b[1]` on a
        // `property [i: int]: int` interface with a getter-only local impl
        // returned `_v + i`, no diagnostic); the LOCAL twin — same source,
        // no import — was, and remains, correctly REJECTED with the
        // §11.2/§23.4 diagnostic (#2417). This is the indexer sibling of
        // the property-side #2406-shaped gap; the ifaceProps loop below
        // already has the analogous property-type fallback to
        // `ctx_.importedMemberType` for its imported side (#2380), so this
        // loop is extended the same way: `ifaceParamCount`/`ifaceParamTy`
        // and `ifaceHasSetter` read from the LOCAL `ast::PropertyDecl`
        // when present (the original shape) and fall back to the
        // RECONSTRUCTED `PropertySymbol::indexerParams` (ParameterSymbols
        // with `resolvedDeclType`) and to `ctx_.importedPropertyHasSetter`
        // (#2400's `propHasSetter_` side table, populated by ModuleLoader's
        // Indexer case from `MetaMember::hasSetter`) otherwise. No new
        // metadata bit is needed — every imported indexer obligation is
        // fully reconstructable from what #2414 + #2400 already publish.
        for (auto& obl : ifaceIndexerObligations) {
            PropertySymbol* ips = obl.ips;
            // SEAM-ACCESSOR: fix #2414/#2529 (family of #3152): `ifaceParamCount`/`ifaceParamTy`
            // (the comment above names them) already fall back to
            // `ips->indexerParams` (reconstructed ParameterSymbols) when `ipd` is
            // null.
            auto* ipd = dynamic_cast<const ast::PropertyDecl*>(ips->decl);

            // #2529: count/type of the interface indexer's parameter list,
            // local or imported — mirrors the candidate-side `candParamCount`
            // /`candParamTy` fallbacks already present in this loop (#2449).
            auto ifaceParamCount = [&]() -> std::size_t {
                if (ipd) return ipd->indexerParams.size();
                return ips->indexerParams.size();
            };
            const std::size_t nparams = ifaceParamCount();
            auto ifaceParamTy = [&](std::size_t k) -> TypeRef* {
                if (ipd) {
                    return (k < ipd->indexerParams.size() &&
                            ipd->indexerParams[k].type)
                               ? resolvedOf(ipd->indexerParams[k].type.get())
                               : nullptr;
                }
                if (k < ips->indexerParams.size())
                    if (auto* p = dynamic_cast<ParameterSymbol*>(
                            ips->indexerParams[k]))
                        return p->resolvedDeclType;
                return nullptr;
            };
            // #2529: whether the interface's own indexer declares a setter.
            // Mirrors the candidate-side `cpd ? propertyDeclHasSetter(cpd)
            // : ctx_.importedPropertyHasSetter(candidate)` fallback a few
            // lines below (#2449) — same MetaMember::hasSetter bit
            // (#2400), the same one codegen's `declareImportedIndexerSetters`
            // gate uses to reserve a setter itable slot.
            auto ifaceHasSetter = [&]() -> bool {
                if (ipd) return propertyDeclHasSetter(ipd);
                return ctx_.importedPropertyHasSetter(ips);
            };

            // #2449: a candidate's arity/k-th param type, read from its
            // local ast::PropertyDecl OR — for an imported candidate, no
            // ast::PropertyDecl to read — its reconstructed indexerParams
            // (ParameterSymbols with resolvedDeclType, ModuleLoader.cpp's
            // Indexer case). Mirrors codegen's identically-shaped
            // indexerParamType/indexerParamCount (LlvmBackend_Assign.cpp)
            // so sema and codegen agree on what a candidate's signature IS,
            // not just on which candidate wins.
            auto candParamCount = [](PropertySymbol* cand) -> std::size_t {
                // SEAM-ACCESSOR: fix #2449 (family of #3152): `candParamCount` already falls back to
                // `cand->indexerParams` (reconstructed ParameterSymbols) when this cast
                // is null — see the comment above and the `return cand->indexerParams
                // ...` line just below.
                if (auto* cpd = dynamic_cast<const ast::PropertyDecl*>(cand->decl))
                    return cpd->indexerParams.size();
                return cand->indexerParams.size();
            };
            auto candParamTy = [&](PropertySymbol* cand,
                                   std::size_t k) -> TypeRef* {
                if (auto* cpd =
                        // SEAM-ACCESSOR: same accessor as `candParamCount` just above — `candParamTy` falls
                        // back to `cand->indexerParams` when this cast is null (#2449, #3152).
                        dynamic_cast<const ast::PropertyDecl*>(cand->decl)) {
                    return (k < cpd->indexerParams.size() &&
                            cpd->indexerParams[k].type)
                               ? resolvedOf(cpd->indexerParams[k].type.get())
                               : nullptr;
                }
                if (k < cand->indexerParams.size())
                    if (auto* p =
                            dynamic_cast<ParameterSymbol*>(cand->indexerParams[k]))
                        return p->resolvedDeclType;
                return nullptr;
            };
            // #2449: mirrors codegen's indexerCandidateWireable exactly —
            // see the block comment above for why.
            // #3204 (§11.2 / §23.4 / §2.8.3 rule 4, v0.254): the
            // `virtual`/`override` exclusion is no longer complete. Fix
            // #3190 added a vtable-forward thunk
            // (`importedVirtualAccessorSlotFn`, LlvmBackend_Assign.cpp)
            // that emits a small forwarder for an imported virtual / override
            // indexer — the forwarder loads the `vt$<Owner>` slot and
            // tail-calls it, so the itable slot gets a real function
            // instead of `ConstantPointerNull`. Abstract is still rejected:
            // an abstract member has no body in the exporter, so its own
            // `vt$<Owner>` slot is legitimately null and the forwarder
            // would forward into the same null-call crash #2438 exists to
            // prevent at this sema gate (the forwarder guards the same
            // `abstract` exclusion; mirrors the codegen mirror at
            // `indexerCandidateWireable`, updated in this same commit).
            //
            // fix #3641 / §12.5: the `abstract` exclusion above was only
            // ever checked on the IMPORTED branch (`cand->decl == nullptr`)
            // — a LOCAL candidate (`dynamic_cast<const ast::PropertyDecl*>
            // (cand->decl)` succeeds) returned wireable unconditionally,
            // never inspecting `cand->modifiers` at all. That was invisible
            // before this fix because nothing pushed a CLASS-ancestor's own
            // abstract indexer into any obligation this loop's candidate
            // search could reach; now that the BFS above records one
            // (`ifaceIndexerObligations.push_back({ps, cur})` in the
            // `!curIsIface` arm), a same-module `class Derived: AB` that
            // never overrides `AB`'s LOCAL abstract indexer would otherwise
            // walk `Derived`'s base-class chain, land on `AB`'s own
            // abstract-indexer PropertySymbol (`cand->decl` is a real,
            // local `ast::PropertyDecl`), and accept it as a "candidate" —
            // finding the very declaration the obligation is about and
            // treating it as an implementation of itself. Checking
            // `abstract` on BOTH branches closes that false ACCEPT; it
            // cannot reject any previously-valid program, since a genuine
            // override is never itself declared `abstract` (§12.1 forbids
            // `abstract override`).
            auto candWireable = [](PropertySymbol* cand) -> bool {
                for (auto mod : cand->modifiers)
                    if (mod == "abstract")
                        return false;
                // SEAM-ACCESSOR: `candWireable` is a deliberate decl-PRESENCE test (documented in the
                // comment above): local when the cast succeeds, else a `cand->decl`
                // presence check (defensive) and finally an interface-kind fallback —
                // the null branch is handled explicitly, not silently (#2437/#2442/
                // #2449, family of #3152).
                if (dynamic_cast<const ast::PropertyDecl*>(cand->decl))
                    return true;  // local
                if (cand->decl) return false;  // defensive
                return !(cand->parent &&
                         cand->parent->kind == ast::TypeDeclKind::Interface);
            };

            // #2437/#2442/#2449: full parameter-list match over `t`'s own
            // members then its base-class chain, gated on wireability — see
            // the block comment above.
            PropertySymbol* candidate = nullptr;
            for (TypeSymbol* cur = t; cur && !candidate;) {
                for (Symbol* m : cur->members) {
                    auto* cps = dynamic_cast<PropertySymbol*>(m);
                    if (!cps || !cps->isIndexer) continue;
                    bool isStaticIdx = false;
                    for (auto mod : cps->modifiers)
                        if (mod == "static") { isStaticIdx = true; break; }
                    if (isStaticIdx) continue;
                    if (!candWireable(cps)) continue;
                    if (candParamCount(cps) != nparams) continue;
                    bool all = true;
                    for (std::size_t k = 0; k < nparams; ++k) {
                        TypeRef* pty = candParamTy(cps, k);
                        TypeRef* ity = ifaceParamTy(k);
                        if (!pty || !ity || !typeRefEqual(pty, ity)) {
                            all = false;
                            break;
                        }
                    }
                    if (all) { candidate = cps; break; }
                }
                TypeSymbol* base = nullptr;
                for (TypeSymbol* b : cur->baseTypes)
                    if (b && b->kind == ast::TypeDeclKind::Class) {
                        base = b;
                        break;
                    }
                cur = base;
            }

            // fix #3641 / §12.5: `obl.iface` is no longer always an
            // INTERFACE — the `!curIsIface` push arm above records a
            // CLASS-ancestor's abstract indexer here too, reusing this same
            // candidate search (comment on that push arm explains why). The
            // two obligation shapes report through different diagnostic
            // families (§11.2/§23.4 "implements interface" vs. §12.5's
            // "does not implement inherited abstract member", the same
            // family the method/property loop above uses), so branch on
            // which kind of type declared the obligation.
            const bool obligationIsInterface =
                (obl.iface->kind == ast::TypeDeclKind::Interface);

            if (!candidate) {
                if (obligationIsInterface) {
                    // #2438: no indexer, own or inherited, matches the
                    // obligation's full parameter list — mirrors #489's
                    // "does not implement interface property" message a few
                    // hundred lines above, adapted for indexers (see the
                    // block comment above this loop for why that check
                    // itself isn't reused directly).
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string(kindWord) + " `" + std::string(t->name) +
                        "` does not implement interface `" +
                        std::string(obl.iface->name) + "`'s indexer — "
                        "declare a matching `public property [params]: T` "
                        "on `" + std::string(t->name) + "` or an ancestor "
                        "class, with the same parameter count and types "
                        "(§11.2 / §23.4)");
                } else {
                    // fix #3641 / §12.5 ("`abstract` on a member declares it
                    // without a body; every concrete subclass must override
                    // it"): same diagnostic family as the abstractNames
                    // method/property report above — "indexer" stands in
                    // for the member name the way `checkPropertyOverrideSig-
                    // natureMatch`'s own `err` lambda already does for an
                    // override-signature mismatch (`derived->isIndexer ?
                    // "indexer" : derived->name`), since an indexer has no
                    // name to report (§23.4: "An indexer is a property
                    // whose name is replaced by `[parameters]`").
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("concrete ") + kindWord + " `" +
                        std::string(t->name) +
                        "` does not implement inherited abstract member "
                        "`indexer` (§12.5)");
                }
                continue;
            }

            // fix #3641 / §12.5: for the CLASS-ancestor obligation shape, a
            // non-null `candidate` already means SOME override was declared
            // — §12.5's own bar ("every concrete subclass must override
            // it"), the only obligation this walk exists to enforce for a
            // class ancestor. Whether that override's OWN accessor set
            // matches the abstract declaration's is a §12.1 override-
            // signature question, already checked in full (property TYPE,
            // visibility, AND accessor presence in both directions) by
            // `checkPropertyOverrideSignatureMatch`, which runs separately
            // wherever this override is paired with its base member — see
            // xmod_indexer_blockform_override_2523_test.cpp's T3/T4, which
            // pin that mismatch being caught there already. Re-deriving the
            // same getter/setter comparison here would just be a second,
            // differently-worded spelling of that same check for indexers
            // specifically (the §6 duplicated-predicate hazard) — so the
            // class-ancestor arm stops at "a candidate exists" and leaves
            // accessor-shape correctness to that existing authority. The
            // INTERFACE arm keeps the full accessor check below: an
            // interface member is never paired through the `override`
            // mechanism at all (structural conformance, not inheritance),
            // so nothing else checks it.
            if (!obligationIsInterface) continue;

            if (!ifaceHasSetter())
                continue;  // read-only obligation, candidate found: satisfied

            // #2449: an IMPORTED candidate has no ast::PropertyDecl to read
            // accessor presence off; use the reconstructed hasSetter bit
            // instead (#2400/#2414's propHasSetter_ side table, published
            // through ctx_.importedPropertyHasSetter — the SAME bit
            // declareImportedIndexerSetters, LlvmBackend_Setup.cpp, gates
            // stub declaration on, so sema and codegen read one source of
            // truth for accessor presence here too).
            // SEAM-ACCESSOR: fix #2400/#2414/#2449 (family of #3152): `candidateHasSetter` just
            // below already falls back to ctx_.importedPropertyHasSetter(candidate)
            // when `cpd` is null (the comment above names the source table).
            auto* cpd = dynamic_cast<const ast::PropertyDecl*>(candidate->decl);
            bool candidateHasSetter =
                cpd ? propertyDeclHasSetter(cpd)
                    : ctx_.importedPropertyHasSetter(candidate);

            if (!candidateHasSetter) {
                ctx_.diagnostics().report(
                    Severity::Error, t->declLoc,
                    std::string(kindWord) + " `" + std::string(t->name) +
                    "` implements interface `" + std::string(obl.iface->name) +
                    "`'s indexer with a getter-only implementation, but the "
                    "interface's indexer is declared read-write — an "
                    "implementation of a read-write interface indexer must "
                    "provide a setter (§11.2 / §23.4)");
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
            // §2.8.2/§11.2/§12.5 (#2380): the PROPERTY sibling of #2373's
            // METHOD fix in this same function. `ps` here is applied to BOTH
            // sides of the comparison below (`ips`, always from an interface
            // reached by the ancestor BFS above, and `cit->second`, from a
            // non-interface `cur` in that SAME BFS — which, per the BFS's own
            // reach, can be a base CLASS from another module just as readily
            // as `ips`'s interface can be) — so either side may be an
            // IMPORTED PropertySymbol: an interface property declared on an
            // imported interface, or a property inherited from an imported
            // base class that happens to satisfy a (possibly local)
            // interface's obligation. Confirmed reachable for both shapes by
            // probe before this fix (an imported interface's property and,
            // separately, a local interface satisfied by a property inherited
            // from an imported base class both silently accepted a type
            // mismatch).
            //
            // ModuleLoader::convertMember's Property case never attaches an
            // ast::PropertyDecl to a body-less imported declaration (there is
            // no syntactic AST to attach — "the getter BODY is not
            // reconstructed here"); the resolved property type instead rides
            // the same importedMemberType side table used for imported
            // fields/consts (ModuleLoader.cpp ~445, comment: "The resolved
            // property TYPE rides fieldTypes_ ... so ctx.importedMemberType()
            // can read it"). The old decl-only read made `propType` return
            // null for EVERY imported property, silently skipping the §11.2
            // exact-type-match check below via `if (!itype || !ctype)
            // continue;` — a false ACCEPT of any property-type mismatch
            // (verified: a `string`-typed local property satisfying an
            // imported `Name: int` interface property compiled clean and,
            // read through the interface reference, produced a garbage `int`
            // off the string's representation).
            auto propType = [&](PropertySymbol* ps) -> const TypeRef* {
                // SEAM-ACCESSOR: the `propType` lambda already falls back to
                // ctx_.importedMemberType(ps) when `pd` is null — see the comment 15
                // lines above and the `return ctx_.importedMemberType(ps);` line just
                // below (fix #3152's audit family for the §11.2 property-type check).
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (pd) {
                    if (!pd->type) return nullptr;
                    return ctx_.resolvedType(*pd->type);
                }
                return ctx_.importedMemberType(ps);
            };
            for (auto& [name, info] : ifaceProps) {
                PropertySymbol* ips = info.first;
                TypeSymbol* iface = info.second;
                if (!iface || !iface->genericParams.empty()) continue;
                const TypeRef* itype = propType(ips);
                auto cit = concreteProps.find(name);
                if (cit != concreteProps.end()) {
                    const TypeRef* ctype = propType(cit->second);
                    if (!itype || !ctype) continue;
                    if (itype->kind == TypeRef::Kind::Error ||
                        ctype->kind == TypeRef::Kind::Error) continue;
                    if (!typeRefEqual(itype, ctype)) {
                        ctx_.diagnostics().report(
                            Severity::Error, cit->second->declLoc,
                            std::string("property `") + std::string(name) +
                            "` implements interface `" +
                            std::string(iface->name) +
                            "` with a different type than the interface "
                            "declares — property types must match exactly "
                            "(no covariance) (§11.2)");
                    }
                    continue;
                }
                // §9.1 / #2416: the positional-field-backed satisfaction —
                // compare the interface property's type against the field's
                // own declared type (`posParam->type`) the same way.
                auto pfit = concretePosFields.find(name);
                if (pfit == concretePosFields.end())
                    continue;  // missing → reported above
                FieldSymbol* fs = pfit->second;
                const TypeRef* ctype =
                    (fs->posParam && fs->posParam->type)
                        ? ctx_.resolvedType(*fs->posParam->type)
                        : nullptr;
                if (!itype || !ctype) continue;
                if (itype->kind == TypeRef::Kind::Error ||
                    ctype->kind == TypeRef::Kind::Error) continue;
                if (!typeRefEqual(itype, ctype)) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        std::string("property `") + std::string(name) +
                        "` implements interface `" + std::string(iface->name) +
                        "` with a positional field of a different type than "
                        "the interface declares — property types must match "
                        "exactly (no covariance) (§9.1 / §11.2)");
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
        if (!argSatisfiesTypeBound(d.arg, d.bound, &ctx_)) {
            ctx_.diagnostics().report(
                Severity::Error, d.loc,
                std::string("type argument `") + d.argName +
                "` does not satisfy the constraint `where " + d.param +
                ": " + d.boundName + "` (§14.2)");
        }
    }
    deferredTypeBoundFailures_.clear();
}

void Resolver::flushDeferredTypeBoundFailures() {
    for (const auto& d : deferredTypeBoundFailures_) {
        ctx_.diagnostics().report(
            Severity::Error, d.loc,
            std::string("type argument `") + d.argName +
            "` does not satisfy the constraint `where " + d.param +
            ": " + d.boundName + "` (§14.2)");
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
        // §2.8.2/§11.8 (#2359 bug 2d): the SAME imported-TypeRef gap as bugs
        // 2a/2b/2c. `d` is a bare-type-parameter reference substituted here
        // for BOTH the direct-conformance case and (since 4349fdc) the
        // ancestor-interface-chain case; when the ancestor interface is
        // IMPORTED, its own declared return/param TypeRef (e.g. IBase3's
        // `owned T`, read via `ctx_.importedReturnType`) carries no `astNode`
        // at all — only `importedName`. The old `astNode`-only guard made
        // this substitution a silent no-op for an imported ancestor's bare
        // `T`, so the return type stayed unsubstituted "T" and the
        // subsequent `typesMatch` comparison against the conforming type's
        // concrete return spuriously failed ("different signature"). Read
        // the name via the shared alias-transparent `TypeRef::headName()`
        // (falls back to `importedName`) instead, matching the fix shape at
        // the other three sites.
        if (!d->declaration && !d->headName().empty() &&
            d->genericArgs.empty()) {
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
                // SEAM-ACCESSOR: fix #2294 (§11.8/§2.8.2, family of #3152): `im` may be an IMPORTED
                // static interface member. `imHasAstDecl` (set alongside this cast)
                // gates the fallback to ctx_.importedReturnType/importedReturnOwned a
                // few lines below — no silent "unit-returning" default.
                if (auto* imd = dynamic_cast<const ast::MethodDecl*>(im->decl)) {
                    imRetNode = imd->returnType.get();
                    imRetOwned = imd->returnOwned;
                    imHasAstDecl = true;
                } else if (auto* iod =
                               // SEAM-ACCESSOR: the OperatorDecl arm of the same #2294 fallback as the MethodDecl
                               // cast just above — `imHasAstDecl` still gates the
                               // ctx_.importedReturnType/importedReturnOwned fallback below.
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
                            // SEAM-ACCESSOR: `cms` is drawn ONLY from `t->members` (the "Candidates on the
                            // conforming type itself" loop ~150 lines above) — `t` comes from
                            // `ctx_.module()->children`, so `cms` is always LOCAL; this function
                            // does not walk a base-class chain to find a static witness (#3152).
                            dynamic_cast<const ast::MethodDecl*>(cms->decl)) {
                        cRetNode = cmd->returnType.get();
                        cRetOwned = cmd->returnOwned;
                    // SEAM-ACCESSOR: the OperatorDecl arm of the same local-only `cms` as the MethodDecl
                    // cast just above (#3152).
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

bool argSatisfiesTypeBound(const TypeRef* arg, const TypeRef* bound,
                           SemaContext* ctx) {
    if (!arg || !bound) return false;
    if (arg->kind != TypeRef::Kind::Named || !arg->declaration) return false;
    if (bound->kind != TypeRef::Kind::Named || !bound->declaration) {
        return false;
    }
    // BFS over the argument's transitive base/interface set, cycle-guarded.
    // §14.2 (fix #4863, v0.307): the constraint is an interface
    // INSTANTIATION — `where T: IEquatable<Other>` requires T to conform to
    // THAT instantiation, not merely to "some instantiation of IEquatable".
    // Pre-fix this matched by declaration identity only, so a candidate
    // conforming to `IEquatable<Self>` was wrongly accepted for
    // `IEquatable<Other>` and the body's `a.Equals(o)` was then called with
    // an `Other` argument against a `Self`-typed `Equals` (type confusion).
    // Walk `baseTypeRefs` in parallel with `baseTypes` so the candidate's
    // actual instantiation is visible at the comparison; require
    // `typeRefEqual` on the full TypeRef — not just declaration match —
    // whenever the bound carries type arguments.
    //
    // The bound's generic args may name the GENERIC OWNER's type parameters
    // — a bare-parameter marker (Named with declaration=nullptr). The §14.2
    // recurrence constraint `where T: INum<T>` resolves the bound as
    // `INum<T_param>`; substituting T → candidate gives `INum<candidate>`,
    // and only THAT instantiation is what the candidate must conform to
    // (per §14.2's type-parameter-bounds paragraph and the worked example
    // for `where T: U`). Treat a bare-parameter marker at the bound's arg
    // position as the candidate itself: this matches the substitution
    // single-entry substitution {constraint's param → candidate}, which is
    // correct for every current constraint shape (every `where T:
    // IFoo<T>` / `where T: IEquatable<T>` in the corpus is single-param).
    // Multi-param generics where the bound names a NON-constrained
    // parameter (`where T: IFoo<U>` on `class G<T, U>`) remain a documented
    // gap; the deferred-recheck path stores only the candidate for the
    // constraint's own param.
    TypeSymbol* target = bound->declaration;
    std::vector<TypeSymbol*> frontier{arg->declaration};
    std::vector<const TypeRef*> frontierRef{arg};  // parallel: instantiation
    std::unordered_set<TypeSymbol*> seen{arg->declaration};
    std::size_t i = 0;
    while (i < frontier.size()) {
        TypeSymbol* t = frontier[i];
        const TypeRef* tRef = frontierRef[i];
        ++i;
        if (t == target) {
            // Declaration matches. A non-generic bound (e.g. `IDisposable`)
            // admits any instantiation of its declaration — fall through to
            // accept (legacy behaviour, also matches the no-generic-args
            // specialization of the §14.2 constraint table). For a bound
            // carrying type arguments, demand structural equality of the
            // candidate's instantiation against the bound's; a missing
            // `tRef` for a generic bound is treated as "cannot confirm" and
            // rejected rather than silently admitted.
            if (bound->genericArgs.empty()) return true;
            if (!tRef) continue;
            if (bound->genericArgs.size() != tRef->genericArgs.size()) continue;
            bool allMatch = true;
            for (std::size_t j = 0; j < bound->genericArgs.size(); ++j) {
                const TypeRef* bArg = bound->genericArgs[j];
                // §14.2 substitution (single-entry): a bare-parameter marker
                // in the bound names the constraint's own type parameter;
                // under substitution it equals the candidate.
                if (bArg && bArg->kind == TypeRef::Kind::Named &&
                    !bArg->declaration) {
                    bArg = arg;
                    // fix #5086 (§14.2, v0.307): when bArg is replaced with
                    // the full candidate arg (e.g. Pair<string>), the nested
                    // generic in tRef->genericArgs[j] (e.g. Pair<T>) must also
                    // be substituted so the comparison is Pair<string> vs
                    // Pair<string>, not Pair<string> vs Pair<T>.
                    const TypeRef* tArg = tRef->genericArgs[j];
                    // fix-forward #5086 (§14.2, v0.307): a null ctx (some
                    // call sites historically passed none) must skip the
                    // substitution rather than dereference it — fall back to
                    // comparing the unsubstituted tArg, matching pre-fix
                    // behaviour rather than crashing.
                    if (ctx && tArg && arg->genericArgs.size() == arg->declaration->genericParams.size()) {
                        std::unordered_map<std::string_view, TypeRef*> innerSubst;
                        for (std::size_t ki = 0; ki < arg->declaration->genericParams.size(); ++ki) {
                            Symbol* gp = arg->declaration->genericParams[ki];
                            TypeRef* ga = arg->genericArgs[ki];
                            if (gp && ga) innerSubst[gp->name] = ga;
                        }
                        SubstArgs sa = {*ctx, innerSubst};
                        tArg = sema::substituteGenericArgs(const_cast<TypeRef*>(tArg), sa);
                    }
                    if (!typeRefEqual(bArg, tArg)) {
                        allMatch = false;
                        break;
                    }
                } else if (!typeRefEqual(bArg, tRef->genericArgs[j])) {
                    allMatch = false;
                    break;
                }
            }
            if (allMatch) return true;
        }
        for (std::size_t k = 0; k < t->baseTypes.size(); ++k) {
            TypeSymbol* b = t->baseTypes[k];
            if (!b || !seen.insert(b).second) continue;
            const TypeRef* bRef = (k < t->baseTypeRefs.size())
                                      ? t->baseTypeRefs[k]
                                      : nullptr;
            frontier.push_back(b);
            frontierRef.push_back(bRef);
        }
    }
    return false;
}

namespace {
// fix #4309 (§14.2/§17.11, v0.298; send-back on #4294) — the CLOSED set of
// builtin, null-declaration INTERFACE names consulted by BOTH
// `Resolver::argSatisfiesNew` and `typeHasNoDefaultWalk` below, replacing
// two independent by-name lists (§6: "two spellings of one set is how the
// next omission happens" — exactly how #4294 itself covered `IGrouping`
// without covering its 25 siblings).
//
// DERIVED, NOT RE-TYPED: every name here is (a) an `I*` head present in
// `Resolver::builtinTypeArity`'s table (TypeResolution_Names.cpp) — so it
// resolves as a builtin type at all — and (b) NOT one of the seven names
// `SemaContext::isSynthesizedBuiltinInterfaceName` mints a REAL synthesized
// `TypeSymbol` for (IComparable, IEquatable, IEqualityComparer,
// IReadOnlySpan, INumber, ISteppable, IParsable). Those seven already reach
// the TypeSymbol-backed `d->kind == ast::TypeDeclKind::Interface` arms
// directly (both below) and need no entry here.
//
// `IGrouping` is DELIBERATELY excluded even though it is a null-declaration
// interface in `builtinTypeArity`: fix #4294 gave it its own two-line
// branch (this file, both sites, search "fix #4294") on a considered
// judgement call — that it is a separate, self-contained addition, not a
// candidate for folding into a shared list this round. This set should not
// silently re-merge it; if `IGrouping` is ever folded in, that is its own
// decision to record, not a side effect of this one.
//
// `tests/sema/*4309*` asserts, for every name in this array,
// `isBuiltinTypeName(name)` is true (still present in `builtinTypeArity`)
// and `SemaContext::isSynthesizedBuiltinInterfaceName(name)` is false
// (still absent from the synthesized set) — so a future change to EITHER
// authoritative table that would silently invalidate this derivation fails
// that test instead of silently changing what §14.2/§17.11 reject.
//
// §14.2 makes every member unconditionally non-constructible ("Interface
// substitutions never satisfy `new()` — no constructor exists") and §17.11
// makes every member unconditionally non-defaultable ("Not defaultable:
// non-nullable reference types" — every interface is one). Both rules are
// categorical, with no per-type fact to get wrong (unlike DictionaryEntry/
// DrainedEntry, whose defaultability/constraint checking is instantiation-
// dependent) — so one closed, enumerable set safely answers BOTH questions.
//
// NOT `common::BuiltinRegistry.cpp`'s `kBuiltinInterfaceHeads` (22 names,
// ~line 424): that table is validated for a DIFFERENT question — "whose
// runtime value may be a USER class" (the exact complement of
// `isVtableLessBuiltinClassName`, a runtime-representation/disposal
// classification), and its own membership proves the mismatch: it
// deliberately EXCLUDES `IDisposable`/`IAsyncDisposable` (different
// runtime representation), lacks `IOrderedEnumerable`/`IFormattable`
// entirely, and INCLUDES `IGrouping` (excluded here, see above). Agreeing
// on 18 of 22 names is not the same question (§25) — reusing it here would
// silently drop four names from this fix's coverage and wrongly fold
// `IGrouping` back in.
//
// ANTI-DRIFT ONLY, NOT ANTI-OMISSION (#4327, #4299): this array is the
// authoritative closed set of non-constructible builtin interface names, and
// every entry in `builtinTypeArity` that is a null-declaration, non-synthesized
// interface IS mirrored here — so this set catches names that DRIFT out of
// sync.  It provides ZERO signal if a 26th such interface is added to
// `builtinTypeArity` without a corresponding entry here: no test would detect
// the gap.  The population-growth signal is the per-issue review of
// `builtinTypeArity`; this array requires manual upkeep and is validated only
// by human review (CLAUDE.md rule 4: decline-with-reasons, not silently).
constexpr std::array<std::string_view, 25>
    kNonConstructibleBuiltinInterfaceNames = {
        // arity-0 (stdlib §2.14/§2.15/§2.18; builtinTypeArity's first block)
        "IDisposable", "IAsyncDisposable", "ISerializable",
        "IDeserializable", "ISerializer", "IDeserializer", "IFormattable",
        // arity-1 (stdlib §2.8-§2.9/§4/§5/§6.5; builtinTypeArity's arity-1
        // block)
        "IEnumerable", "IEnumerator", "IOrderedEnumerable",
        "IAsyncEnumerable", "IAsyncEnumerator", "IOwningEnumerable",
        "IOwningEnumerator", "IOwningAsyncEnumerable",
        "IOwningAsyncEnumerator", "IReadOnlyCollection", "IReadOnlyList",
        "ICollection", "IList", "IOwningCollection", "IOwningList",
        "IEvent", "IAsyncEvent", "ISharedEvent",
};

bool isNonConstructibleBuiltinInterfaceName(std::string_view head) {
    return std::find(kNonConstructibleBuiltinInterfaceNames.begin(),
                      kNonConstructibleBuiltinInterfaceNames.end(),
                      head) != kNonConstructibleBuiltinInterfaceNames.end();
}
}  // namespace

// Free-function wrapper exposing `kNonConstructibleBuiltinInterfaceNames`
// (just above) for `tests/sema/*4309*`'s membership assertion — see that
// array's own comment, and the header declaration in TypeResolution.hpp,
// for why this is a `const&` onto the SAME array both call sites below
// consult, not a second copy.
const std::array<std::string_view, 25>& nonConstructibleBuiltinInterfaceNames() {
    return kNonConstructibleBuiltinInterfaceNames;
}

// fix #4472 (§14.2, v0.302; follow-up to fix #4446) — see the header
// declaration (TypeResolution.hpp) for the full rationale and the three
// call sites this unifies.
bool isAbstractOrClosedClass(const TypeSymbol* d) {
    if (!d) return false;
    for (auto mod : d->modifiers) {
        if (mod == "abstract" || mod == "closed") return true;
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
            // fix #3044 (§17.11 / §14.2, v0.242): `new T()` at a value
            // substitution IS `default(T)`, and §17.11 rejects `default(E)`
            // for "an enum type with no declared constant of underlying value
            // 0" — explicitly naming `new T()` at such a substitution. The
            // rejection lands at the INSTANTIATION SITE (the §3.6 rule 2
            // pattern), which is where checkOneConstraint reports. An enum
            // whose declared-value set cannot be decided leaves `decided`
            // false (defer, no diagnostic).
            std::optional<bool> hasZero = enumHasZeroConstant(*d);
            if (!hasZero) return false;  // decided stays false — defer
            decided = true;
            return *hasZero;
        }
        // §35.10 (v0.200, #2044): "Choices never satisfy the value-type arm:
        // `default(C)` is undefined for every choice type." So a choice
        // substitution fails `where T: new()`.
        if (d->kind == ast::TypeDeclKind::Choice) {
            decided = true;
            return false;
        }
        if (d->kind == ast::TypeDeclKind::Record) {
            // fix #4699 (§9.9, v0.307, proposal 0107): a constructor-only
            // record has no well-defined default value (§17.11's
            // "Not defaultable" list names it directly) — the §14.2
            // value-substitution-is-default rule is therefore unsatisfiable
            // for such a substitution, regardless of how defaultable every
            // individual field happens to be. Short-circuit BEFORE the
            // field walk below: a constructor-only record's storage can all
            // be defaultable value types (the cycle-23 s9_9_0003 case is
            // exactly that — `record Port { readonly property Value: int; … }`)
            // and the per-field walk would otherwise say "satisfied" and
            // silently admit the substitution. See the parallel
            // short-circuit at `typeHasNoDefault` (this file) for the
            // `default(R)` site — the two share the same authority.
            if (recordIsConstructorOnly(d)) {
                decided = true;
                return false;
            }
            // A record (positional, body-only, or combined-form — §9.3)
            // satisfies new() iff default(R) is well-defined — every
            // DECLARED field has a well-defined default (§14.2 / §17.11).
            // Verify every storage component's resolved type is not a
            // non-nullable reference type.
            //
            // fix #3141 (family F3 of umbrella #3152, §2.8.3 rule 4): the field
            // walk is symbol-side. Through `d->decl` it deferred on EVERY
            // imported record, so `new Holder<ImportedRefRec>()` compiled while
            // the byte-identical local record was rejected with the §14.2
            // diagnostic (measured both ways).
            //
            // fix #3988 (§17.11/§9.3, v0.294): this used to walk ONLY
            // `recordPositionalFieldSymbols` and deferred full body-field
            // coverage to "the default-operator slice" — i.e. to
            // `ExprTC::defaultIllFormed`, which (before that fix, in the
            // same commit) did not in fact cover body storage either, so
            // the deferral chain was broken at both ends: a body-only or
            // combined-form record's non-nullable reference-typed body
            // field/auto-property satisfied `where T: new()` unverified.
            // `recordStorageMembers` (sema/Symbol.hpp) is the ONE
            // symbol-side full-storage enumeration — positional fields,
            // body fields, and auto-properties together — so this now
            // reads it instead of leaving body storage unverified.
            //
            // fix #4002 (§17.11/§14.2/§9.7, v0.294): #3988 widened this
            // arm's field ENUMERATION to `recordStorageMembers`, but never
            // substituted the record's OWN generic parameter against the
            // concrete instantiation — the sibling gap of #3986 in
            // `ExprTC::defaultIllFormed` (ExprTypecheck_Exprs.cpp). For
            // `record Box<T>(V: T)`, `rsm.type` for `V` is the bare,
            // declaration-less `T`; `categorize(ft)` on an unresolved
            // bare parameter does not classify as `TypeCategory::Reference`,
            // so `nonDefaultable` never got set regardless of what `T` was
            // actually bound to. Seed a `common::RecordSubstEnv` from the
            // record's own generic parameters against `arg`'s
            // `genericArgs` (the SAME primitives `ExprTC::defaultIllFormed`
            // uses for the identically-shaped substitution; the
            // symbol-side `seedRecordSubstEnv` overload is correct for an
            // imported record too — `TypeSymbol::genericParams` is
            // populated by ModuleLoader, unlike `decl`) and apply
            // `common::resolveRecordFieldSubst` per member before the
            // `nullable`/`categorize` checks.
            //
            // fix #4166 (§14.2/§17.11, v0.295): #4002 substituted a BARE
            // field (`V: T`), but a field that is ITSELF a Record after
            // substitution (`In: Inner<T>` in a delegating
            // `record Outer<T>(In: Inner<T>)`) was still only run through
            // `categorize()`, which answers "Record ⇒ Value" for ANY
            // record — true of the record AS A TYPE, but silent on whether
            // ITS OWN fields are defaultable once ITS generic parameters
            // are bound.
            //
            // fix #4166 review follow-up (v0.298): the first version of
            // this arm called a LOCAL Resolver method
            // (`recordFieldTypeHasNoDefault`) that ported only the
            // Record-kind recursion from `ExprTC::defaultIllFormed`,
            // leaving its own leaf test at the narrower `categorize() ==
            // Reference` — which diverges from `defaultIllFormed`'s full
            // per-kind switch on Enum/Choice/InlineArray and
            // `isValueBuiltin`-aware Class/Interface (issues #4219,
            // #4223), and initially had no cycle guard for a
            // self-referential record (review-caught SIGSEGV). This now
            // calls the SHARED authority, `sema::typeHasNoDefault`
            // (TypeResolution.hpp), which `ExprTC::defaultIllFormed`
            // itself now also forwards to — one implementation, the same
            // cycle guard, for both the `default` operator's own
            // well-formedness check and this `where T: new()` constraint
            // check.
            decided = true;
            bool unresolved = false;
            bool nonDefaultable = false;
            common::RecordSubstEnv env;
            common::seedRecordSubstEnv(*d, *arg, /*incoming=*/nullptr, env);
            const common::RecordSubstEnv* fieldEnv =
                env.map.empty() ? nullptr : &env;
            recordStorageMembers(
                d, ctx_,
                [&](const RecordStorageMember& rsm) {
                    const TypeRef* ft = rsm.type;
                    if (!ft) { unresolved = true; return false; }  // can't verify
                    common::RecordSubstEnv::Entry sub =
                        common::resolveRecordFieldSubst(ft, fieldEnv);
                    if (!sub.ty) { unresolved = true; return false; }
                    if (typeHasNoDefault(sub.ty, ctx_, sub.env)) {
                        nonDefaultable = true;
                        return false;  // no default at this member
                    }
                    return true;
                },
                /*env=*/nullptr);
            if (unresolved) { decided = false; return false; }
            return !nonDefaultable;
        }
        if (d->kind == ast::TypeDeclKind::Class) {
            // A class satisfies new() iff it has an accessible parameterless
            // constructor, or declares no constructor at all (the implicit
            // default constructor, mirroring checkNewExprArgs). A class with
            // a primary constructor carries its params on the TypeDecl, not a
            // ConstructorSymbol; treat a non-empty primary param list as a
            // non-parameterless constructor.
            //
            // fix #4472 (§14.2, v0.302; follow-up to fix #4446 — this arm was
            // the un-fixed duplicate "Site A" the issue names): an `abstract`
            // or `closed` class does NOT satisfy `new()` even when the ctor
            // scan below would otherwise say yes (§22.7's implicit
            // parameterless constructor is not enough — §12.5 makes `new` on
            // such a class a compile error regardless). Shared with
            // `ExprTC::groundSatisfiesNew` (#4446) and
            // `checkMethodGenericConstraints`'s "new" item
            // (ExprTypecheck_Calls.cpp) through ONE function rather than a
            // third copy of the modifier test — see
            // `sema::isAbstractOrClosedClass`'s own comment (TypeResolution.hpp).
            if (isAbstractOrClosedClass(d)) {
                decided = true;
                return false;
            }
            //
            // fix #3141: read `hasPositionalFields` off the TypeSymbol (#501)
            // and the parameters themselves as FieldSymbols (#2516) — through
            // `d->decl` an imported primary-constructor class answered "no
            // primary parameters" and so SATISFIED `new()` (measured:
            // `new Holder<ImportedPBox>()` compiled; the local twin rejected).
            if (d->hasPositionalFields &&
                !recordPositionalFieldSymbols(d).empty()) {
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
            return false;  // interfaces are not constructible (§14.2: "Interface
                            // substitutions never satisfy `new()`")
        }
    }
    // fix #4235 (§14.2/§17.11/§31.4/stdlib §3.3, v0.298): a null-declaration
    // `Named` arg reaching here has no `TypeSymbol`, so none of the
    // `arg->declaration` arms above ever ran for it — this is the
    // TOP-LEVEL substitution case (`Holder<T>` instantiated directly at a
    // builtin name), distinct from a builtin appearing as a RECORD FIELD,
    // which the Record arm above already recurses into via
    // `typeHasNoDefault` (see the `recordStorageMembers` loop a few lines
    // up). Before this fix, no builtin name was ever consulted here at
    // all, so a substitution straight at one of them fell through to the
    // unconditional defer below and was silently ACCEPTED.
    //
    // MEASURED (issue #4235, parent `ea66776`): `HolderEINF<ExceptionInfo>
    // .Make()` under `where T: new()` compiled with no §14.2 diagnostic
    // and SIGSEGV'd at runtime.
    //
    // ⚠ FIRST VERSION OF THIS FIX (reverted — see the broad sweep this
    // commit's body cites) routed EVERY null-declaration `Named` arg
    // through `typeHasNoDefault`, reasoning that it would also close the
    // identically-shaped top-level gap for the collection family
    // (`Holder<Array<int>>.Make()` was ALSO measured silently accepted on
    // this same parent). That is a REAL, separate gap, but fixing it that
    // way was WRONG: §14.2 gives a substitution TWO independent routes to
    // satisfy `where T: new()` — "class types with a public parameterless
    // constructor" OR "defaultable value types" — and a stdlib
    // COLLECTION is documented as the FORMER (stdlib §4.4: `Array<T>` is a
    // `public class` with a `public constructor()`), never evaluated
    // against defaultability at the top level. `typeHasNoDefault` answers
    // the SECOND question only; routing every builtin name through it
    // MEASURABLY regressed two pinned codegen tests —
    // `codegen_generic_new_builtin_collection_2017`
    // (`generic_new_builtin_collection_2017_test.cpp`, whose own comment
    // quotes exactly this §14.2 class-arm reasoning for `Array`/
    // `Dictionary`) — which this fix's own broad, no-`--area` sweep
    // (LANE_RULES §2/§15) caught before landing. `ExceptionInfo` and
    // `ReadOnlyStringSpan`, by contrast, are `record`s — §14.2's
    // VALUE-type arm is the only one that could ever apply to them (a
    // record is never "a class type" for this purpose), so evaluating
    // exactly these two names against defaultability is correct; every
    // OTHER null-declaration builtin name is left alone here rather than
    // guessed at. (`CString`/`WString`/`RawBytes` were separately measured
    // to ALREADY reject correctly at this exact top-level site on this
    // corrected version, without this arm ever naming them — some other,
    // pre-existing path resolves them with a real `declaration` at a
    // generic-argument position and routes them through the ordinary
    // Class-kind arm above, unlike a plain type annotation. That
    // mechanism was not traced further — it predates this fix, this fix
    // does not disturb it, and there is no measured defect in it — so
    // it is left alone rather than described speculatively.)
    if (arg->kind == TypeRef::Kind::Named && !arg->declaration) {
        std::string_view head = arg->headName();
        // fix #4235 review send-back (#4245, §14.2/§17.11/stdlib
        // §4.14/§4.15, v0.298): `Span`/`ReadOnlySpan` (stdlib §4.14) and
        // `ArraySpan`/`ReadOnlyArraySpan` (stdlib §4.15) are the SAME
        // shape as `ExceptionInfo`/`ReadOnlyStringSpan` above — builtin
        // records whose declared field composition makes them
        // non-defaultable for EVERY `T`, not merely some. `Span<T>`/
        // `ReadOnlySpan<T>` always carry `Buffer: OwningBuffer<T>`
        // (stdlib §4.14: `public record Span<T>(Buffer: OwningBuffer<T>,
        // Start: int, Length: int): ...`) and `ArraySpan<T>`/
        // `ReadOnlyArraySpan<T>` always carry `Source: Array<T>` (stdlib
        // §4.15) — `OwningBuffer<T>` and `Array<T>` are both `public
        // class`es, so the field is class-typed (a non-nullable reference
        // type, §17.11) regardless of what `T` is substituted with. This
        // is four more names on the SAME by-name list, for the SAME
        // reason as the two above — not a re-generalization to a
        // structural predicate (§25): `Array<T>` itself stays OFF this
        // list and continues to satisfy `where T: new()` via §14.2's
        // class arm, never via defaultability (see row (7)/the
        // regression-pin test this file's suite already carries).
        if ((head == "ExceptionInfo" || head == "ReadOnlyStringSpan" ||
             head == "Span" || head == "ReadOnlySpan" ||
             head == "ArraySpan" || head == "ReadOnlyArraySpan") &&
            typeHasNoDefault(arg, ctx_, /*incoming=*/nullptr)) {
            decided = true;
            return false;
        }
        // fix #4294 (§14.2, v0.298): `IGrouping<TKey,T>` (stdlib §5: `public
        // interface IGrouping<TKey, T>: IEnumerable<T>`) is a null-
        // declaration builtin (TypeResolution_Names.cpp's arity-2 table,
        // alongside DictionaryEntry/DrainedEntry — no user `TypeSymbol`), so
        // it never reaches the TypeSymbol-backed `d->kind ==
        // ast::TypeDeclKind::Interface` arm above (~line 7939), which
        // already rejects a REAL interface unconditionally: "decided = true;
        // return false; // interfaces are not constructible (§14.2:
        // 'Interface substitutions never satisfy `new()`')".
        //
        // This is a DELIBERATE new branch, not a fold into the six-name
        // block immediately above (§25: ask what a check was validated to
        // answer, not what it happens to agree with). Those six are
        // RECORDS, which can fail `new()` only via §14.2's value-type/
        // defaultability route (gated on `typeHasNoDefault` — a record
        // could in principle satisfy `new()` if every declared field
        // defaulted). An interface has no such route at all: §14.2
        // excludes it categorically, the same unconditional rule the real
        // TypeSymbol-backed arm above applies, independent of
        // defaultability. Gating IGrouping through `typeHasNoDefault`
        // instead would reach the same answer only because this commit
        // ALSO teaches `typeHasNoDefaultWalk`'s null-declaration arm (below)
        // that IGrouping is non-defaultable — two independently-true facts
        // that happen to agree, not one fact reused for its validated
        // question.
        //
        // MEASURED (issue #4294, this file's own parent `f3d3a4d`, clean
        // tree, `bezelc` rebuilt directly): `HolderIG<IGrouping<string,
        // int>>.Make()` under `where T: new()` compiled with no §14.2
        // diagnostic (only the pre-existing #2477/#2491 dead-abstract-
        // template-body backstop notes) and ran (exit 0) — a silent,
        // degraded-to-zero accept of a non-constructible substitution.
        if (head == "IGrouping") {
            decided = true;
            return false;  // interfaces are not constructible (§14.2), same
                            // rule as the TypeSymbol-backed Interface arm
        }
        // fix #4309 (§14.2, v0.298; send-back on #4294): the 25-name closed
        // set of every OTHER null-declaration builtin interface — see
        // `kNonConstructibleBuiltinInterfaceNames`'s own comment above for
        // the derivation and why `IGrouping` (just above) and the
        // TypeSymbol-backed seven (IComparable/IEquatable/
        // IEqualityComparer/IReadOnlySpan/INumber/ISteppable/IParsable)
        // are not in it. Same rule as the TypeSymbol-backed Interface arm
        // and the IGrouping branch immediately above: §14.2, "Interface
        // substitutions never satisfy `new()` — no constructor exists".
        //
        // MEASURED (issue #4309, this file's own parent `e0182bc`, clean
        // tree, `bezelc` rebuilt directly): e.g. `HolderX<IEnumerable<int>>
        // .Make()`, `HolderY<IDisposable>.Make()`,
        // `HolderZ<IOrderedEnumerable<int>>.Make()` under `where T: new()`
        // all compiled with no §14.2 diagnostic (only the pre-existing
        // #2477/#2491 dead-abstract-template-body backstop notes) and ran
        // (exit 0) — the identical silent-accept symptom #4294 fixed for
        // `IGrouping` alone, reproduced on every other name in the set.
        if (isNonConstructibleBuiltinInterfaceName(head)) {
            decided = true;
            return false;  // interfaces are not constructible (§14.2), same
                            // rule as the TypeSymbol-backed Interface arm
        }
        // fix #4245 (§14.2/§17.11/stdlib §4.8, v0.298): `DictionaryEntry<K,V>`
        // is handled as its OWN branch, not folded into the six-name list
        // above, because — unlike those six, which are unconditionally
        // non-defaultable — `DictionaryEntry<K,V>`'s defaultability is
        // INSTANTIATION-DEPENDENT (see `typeHasNoDefaultWalk`'s own
        // null-declaration comment, this file, for the full reasoning and the
        // `t->genericArgs` recursion this shares). The six-name check above
        // only ever DECIDES the "does not satisfy" outcome (harmless there,
        // since none of those six is ever defaultable) — `DictionaryEntry`
        // needs both outcomes decided: `DictionaryEntry<int,int>` DOES
        // satisfy `where T: new()` (every field defaults), so `decided` must
        // be set unconditionally here, not only on the non-defaultable path.
        //
        // MEASURED (issue #4245, this file's own parent `8ca85ce`, clean
        // tree): `HolderDE<DictionaryEntry<string,int>>.Make()` under
        // `where T: new()` compiled with no §14.2 diagnostic (only the
        // pre-existing #2477/#2491 dead-abstract-template-body backstop
        // notes) and SIGSEGV'd at runtime (exit 139) reading `.Key.Length`.
        if (head == "DictionaryEntry") {
            decided = true;
            return !typeHasNoDefault(arg, ctx_, /*incoming=*/nullptr);
        }
    }
    // Built-ins, generic params, function/inline-array args: defer.
    return false;
}

// fix #4166 review follow-up (§14.2/§17.11/§35.10, v0.298): the shared
// authority declared in TypeResolution.hpp — see that declaration's
// comment for the full rationale (why this exists, replacing the first
// version's Resolver-local `recordFieldTypeHasNoDefault`/
// `recordFieldTypeHasNoDefaultWalk` pair, and what call sites it unifies).
// `typeHasNoDefaultWalk` below is a port of `ExprTC::defaultIllFormed`'s
// full per-kind switch (ExprTypecheck_Exprs.cpp), with THREE deliberate
// changes from that function's exact prior text:
//
//   1. The `seen` cycle guard around the Record-kind recursion (PATH
//      semantics — insert on entry, erase on exit — keyed by
//      `const TypeRef*` instantiation identity, mirroring
//      `common::isBorrowBearingRecordCommon`, OwnershipFacts.cpp, and that
//      file's fix #3933 for why the key is the instantiation and not the
//      bare `TypeSymbol*` declaration). `defaultIllFormed` itself had no
//      such guard; a self-referential record
//      (`record Self<T>(N: Self<int>, V: T)`) reached through an explicit
//      `default(...)` SIGSEGVs on it identically to the `where T: new()`
//      shape this fix's own review caught — this closes both at once,
//      since `ExprTC::defaultIllFormed` now forwards here too.
//   2. The Primitive arm now also flags a non-nullable `Object` as
//      non-defaultable (see that arm's own comment below for why:
//      `defaultIllFormed`'s prior text deliberately excluded it as a
//      documented, separate #951-era scope decision, but
//      `Resolver::argSatisfiesNew`'s pre-existing, tested behavior
//      correctly rejects it, and unifying in `defaultIllFormed`'s
//      direction would have silently reintroduced a false accept —
//      MEASURED regression, caught by this fix's own broad sweep against
//      `xmod_imported_positional_new_args_3141_test.cpp`).
//   3. The Named arm's `!t->declaration` branch now falls back to
//      `isBuiltinTypeName` before deferring (see that branch's own
//      comment below). `defaultIllFormed`'s prior text unconditionally
//      returned `false` ("defaultable") for EVERY null-declaration Named
//      type — both a genuine unresolved generic parameter AND every
//      always-available builtin (`Array`, `Dictionary`, `Set`, `OwningTask`,
//      …, TypeResolution_Names.cpp's ~50-name Slice-S24 population, which
//      is represented with a null declaration precisely because it has no
//      user `TypeSymbol`). `Resolver::categorize` already distinguished
//      these via an `astNode`/`builtinTypeArity` fallback;
//      `argSatisfiesNew`'s pre-hoist, categorize()-based leaf test
//      inherited that distinction and correctly rejected a builtin-
//      collection-typed record field under `where T: new()` — the naive
//      port of `defaultIllFormed`'s text (which never had that
//      distinction) silently dropped it, a REGRESSION relative to
//      `argSatisfiesNew`'s own prior behavior review caught with a live
//      A/B measurement (`record Box(A: Array<int>)`: REJECT on parent
//      `1e6d80f`, silent ACCEPT on the un-fixed hoist). This is the THIRD
//      predicate in this run caught misclassifying this same ~50-builtin
//      population — issue #4229 already tracks two others
//      (`refMentionsTypeVariable`, `refMentionsAnyOf`), and `fix #4199`
//      was sent back for a fourth; "no declaration" keeps getting
//      silently read as "generic parameter only", and it never has been.
//
// On a detected cycle this returns `false` ("has a default" — defer, not
// "non-defaultable"): a self-referential record is ALREADY rejected with
// its own clean §9 diagnostic by `recordValueContainsSelf`
// (checkRecordRestrictions, this file), independent of this function, so
// deferring on the cyclic field lets the walk continue to the record's
// OTHER fields (e.g. a `V: T` field bound to a non-nullable class still
// correctly triggers rejection) rather than manufacturing a crash.
//
// An unresolved field member (`rsm.type == nullptr`, or a substitution
// that fails to resolve) is likewise treated as defaultable (defer) —
// matching `defaultIllFormed`'s own established "unresolved — defer"
// precedent for the identical case, continuing the walk rather than
// stopping it.
namespace {

bool typeHasNoDefaultWalk(const TypeRef* t, const common::RecordSubstEnv* incoming,
                          SemaContext& ctx,
                          std::unordered_set<const TypeRef*>& seen) {
    if (!t || t->nullable) return false;  // nullable defaults to null — OK
    switch (t->kind) {
        case TypeRef::Kind::Primitive:
            // `string`/`Object` are runtime-managed/reference primitives
            // (§4.6) with no zero; every value primitive (int/bool/char/…)
            // has a zero.
            //
            // fix #4166 review follow-up: `ExprTC::defaultIllFormed`'s
            // pre-unification version deliberately excluded `Object` here
            // ("left out... to keep this consistent with the original #951
            // scope — a separate consideration"), but §17.11's own
            // "defaultable" definition is unambiguous — "Not defaultable:
            // non-nullable reference types" — and `Object` (non-nullable,
            // which is what reaches here; the nullable case already
            // returned above) is one. `Resolver::argSatisfiesNew` already
            // correctly rejected a non-nullable `Object`-typed record field
            // via `categorize()` before this unification (MEASURED:
            // `record RefRec3141(R: Object, N: int)` under
            // `where T: new()`, tests/module/
            // xmod_imported_positional_new_args_3141_test.cpp) — including
            // it here, rather than porting the narrower exclusion, is what
            // keeps that existing, correct, spec-cited rejection working
            // under the shared function.
            return t->primitive == PrimitiveType::String ||
                   t->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;  // a function value (§28.9) has no zero
        case TypeRef::Kind::InlineArray: {
            common::RecordSubstEnv::Entry sub =
                common::resolveRecordFieldSubst(t->arrayElement, incoming);
            return typeHasNoDefaultWalk(sub.ty, sub.env, ctx, seen);
        }
        case TypeRef::Kind::Named: {
            if (!t->declaration) {
                // fix #4166 review follow-up #3 (§2.3/§14.2, v0.298): a
                // null-declaration Named type is NOT only a generic param
                // or an unresolved name — it is also how EVERY
                // always-available builtin (`Array`, `OwningArray`,
                // `Dictionary`, `Set`, `SortedDictionary`, `OwningTask`,
                // and the rest of the ~50-name Slice-S24 population,
                // TypeResolution_Names.cpp) is represented: that file's own
                // comment says they "have no user TypeSymbol, so we record
                // a Named ref with a null declaration".
                //
                // MEASURED gap this closes: `record BoxArr(A: Array<int>)`
                // used at a `where T: new()` site — REJECT on parent
                // `1e6d80f`, silently ACCEPT on this fix before this
                // change (the unconditional `return false` two lines
                // below never consulted anything about the builtin name).
                //
                // review's literal suggestion — mirror `categorize()`'s
                // `astNode`/`builtinTypeArity` fallback, which flags EVERY
                // builtin as a non-defaultable reference type — was TRIED
                // and REVERTED: `categorize()` does not distinguish
                // VALUE builtins (`Guid`, `IntPtr`, `DateTime`, …) from
                // REFERENCE ones (`Array`, `Dictionary`, …) either — a
                // separate, pre-existing imprecision in `categorize()`
                // itself, out of scope here — and mirroring it broke
                // `default(Guid)` / `default(IntPtr)`
                // (tests/lit/builtin_closed_ctor_surface_accept_2994.bzl),
                // which `defaultIllFormed`'s ORIGINAL text had always
                // accepted (ITS null-declaration branch returned `false`
                // unconditionally, for every null-declaration builtin, not
                // narrowly — so this was passing by accident of a blanket
                // accept, not by any deliberate Guid/IntPtr handling).
                //
                // So this checks by NAME against the STDLIB COLLECTION
                // FAMILY specifically (docs/Bezel_For_Code_Generation.md's
                // collection-family table — the borrowing forms and their
                // `Owning*` counterparts) plus `Task`/`OwningTask` — the
                // exact population review's own repro named, and the
                // ONLY subset this fix's own regression sweep (including
                // the full 2311-test lit diagnostic track) verifies stays
                // correct on both the `where T: new()` and `default(...)`
                // paths. A complete, verified value-vs-reference table for
                // the FULL null-declaration builtin population (~50 names)
                // is future work, not attempted here — narrower-but-correct
                // over broader-but-regressing.
                //
                // A leaf that returns unconditionally on the FIRST line of
                // a branch, with no fallback for the builtin population at
                // all, is a recurring shape this run has hit before —
                // #4229 (`refMentionsTypeVariable`, `refMentionsAnyOf`) and
                // the returned `fix #4199` — so this walk joins that
                // family; the next predicate over a null-declaration Named
                // type should check that issue before assuming "no
                // declaration" means "generic parameter", AND should
                // verify against `default(...)`-context tests too, not
                // only `where T: new()` ones — this fix's own first
                // attempt did not, and it cost a lit-track regression.
                if (t->astNode && t->astNode->name &&
                    t->astNode->name->parts.size() == 1) {
                    std::string_view head = t->headName();
                    if (head == "Array" || head == "OwningArray" ||
                        head == "Dictionary" || head == "OwningDictionary" ||
                        head == "Set" || head == "OwningSet" ||
                        head == "SortedDictionary" || head == "SortedSet" ||
                        head == "Stack" || head == "OwningStack" ||
                        head == "Queue" || head == "OwningQueue" ||
                        head == "LinkedList" || head == "OwningLinkedList" ||
                        head == "Task" || head == "OwningTask") {
                        return true;  // heap collection/task: no default
                    }
                    // fix #4166 review follow-up #3b (stdlib §10.1, v0.298):
                    // `CString`/`WString`/`RawBytes` — the §10 FFI-boundary
                    // classes, resolved through this SAME null-declaration
                    // path (TypeResolution_Names.cpp:390, one combined
                    // check with these three names) — "none has a public
                    // constructor" (stdlib §10.1/§10.2/§10.3), so like the
                    // collection family above they are non-nullable
                    // reference types with no default. MEASURED: `record
                    // Box(C: CString, N: int)` under `where T: new()` /
                    // `default(...)` compiled clean (exit 0) and, when
                    // `.Length` was read on the zero-initialized field,
                    // SIGSEGV'd (exit 139) — identically reproduced on
                    // parent `1e6d80f` (this defect predates this whole
                    // fix chain; it was never introduced or worsened by
                    // it, only left unmeasured). `WString`/`RawBytes` did
                    // not crash on the SAME `.Length` probe (their layout
                    // apparently tolerates a null-initialized internal
                    // pointer for that one accessor), but the §17.11
                    // classification is identical for all three — a
                    // non-nullable reference type with no public
                    // constructor is not defaultable regardless of
                    // whether a given accessor happens to fault.
                    //
                    // NOT reused: `isFfiBoundaryNamedType` (this file,
                    // ~8869) groups these three with `IntPtr`/`UIntPtr` —
                    // correctly, for ITS question ("is this type
                    // FFI-signature-compatible", §32.5). `IntPtr`/`UIntPtr`
                    // ARE defaultable (value types; `default(IntPtr)` is
                    // pinned accepted by
                    // tests/lit/builtin_closed_ctor_surface_accept_2994.bzl),
                    // so answering MY question ("is this type
                    // defaultable") with that predicate would have
                    // wrongly flagged `IntPtr`/`UIntPtr` too — precisely
                    // the §25 trap ("what was this predicate validated to
                    // answer, not what it correlates with") this fix's
                    // own review already caught once this cycle for
                    // `categorize()`'s builtin fallback.
                    if (head == "CString" || head == "WString" ||
                        head == "RawBytes") {
                        return true;  // FFI-boundary class: no default
                    }
                    // fix #4235 (§17.11/§31.4/stdlib §3.3, v0.298): a
                    // DIFFERENT reason than the two blocks above — those are
                    // non-defaultable because they ARE reference types
                    // (collections) or declare no public constructor (FFI
                    // classes); `ExceptionInfo` and `ReadOnlyStringSpan` are
                    // ordinary VALUE-shaped records (§17.11's Record arm
                    // would apply to them exactly as it does to any local
                    // record, if they had a real `TypeSymbol` to recurse
                    // into) whose declared fields happen to include at
                    // least one non-nullable `string` — a Reference
                    // primitive (§17.11), hence non-defaultable by the
                    // SAME per-field rule the Record-kind arm below applies
                    // to a user record.
                    //
                    // `record ExceptionInfo(TypeName: string, Message:
                    // string, StackTrace: string)` (§31.4) — all three
                    // fields are non-nullable `string`.
                    // `record ReadOnlyStringSpan(Source: string, Start:
                    // int, Length: int): IReadOnlySpan<char>` (stdlib
                    // §3.3) — `Source` is a non-nullable `string`; `Start`/
                    // `Length` would default fine alone, but §17.11 needs
                    // only ONE non-defaultable declared field ("a record
                    // any of whose declared fields is not defaultable").
                    // Field names/arities are pinned by
                    // `BuiltinRegistry.cpp`'s `kBuiltinCtorParamNames`
                    // (`{"ExceptionInfo", ..., {"TypeName", "Message",
                    // "StackTrace", ""}}`, `{"ReadOnlyStringSpan", ...,
                    // {"Source", "Start", "Length", ""}}`) — the same
                    // fixed, closed shapes §17.7's own closed-constructor-
                    // surface rule already relies on.
                    //
                    // Both are, like the collection/FFI names above,
                    // resolved through this null-declaration path with no
                    // real `TypeSymbol` (TypeResolution_Names.cpp), so
                    // there is no `recordStorageMembers` walk to recurse
                    // into for them — the field composition is hardcoded
                    // here rather than walked, since it is fixed and
                    // spec-declared, not user-editable.
                    //
                    // MEASURED on parent `ea66776` (issue #4235): `default(
                    // ExceptionInfo)` compiled clean and SIGSEGV'd (exit
                    // 139) on `.TypeName.Length`; `default(
                    // ReadOnlyStringSpan)` compiled with only the #2491
                    // degraded-value backstop and silently read `.Length`
                    // as 0 instead of the expected value — the exact
                    // null-string-deref class already fixed here for
                    // `CString`/`WString`/`RawBytes`, just for two names
                    // outside that scoped list. See
                    // `Resolver::argSatisfiesNew`'s own comment (this
                    // file) for the THIRD symptom this issue reported —
                    // `where T: new()` accepting these two names at a
                    // TOP-LEVEL substitution never reaches this walk at
                    // all, and needed its own fix.
                    // fix #4235 review send-back (#4245, §17.11/stdlib
                    // §4.14/§4.15, v0.298): `Span`/`ReadOnlySpan` (stdlib
                    // §4.14) and `ArraySpan`/`ReadOnlyArraySpan` (stdlib
                    // §4.15) are the SAME shape as `ExceptionInfo`/
                    // `ReadOnlyStringSpan` immediately above, one level up
                    // in this same block: builtin records with an
                    // always-non-defaultable declared field. `Span<T>`/
                    // `ReadOnlySpan<T>` always carry a non-nullable
                    // class-typed `Buffer: OwningBuffer<T>` (stdlib §4.14);
                    // `ArraySpan<T>`/`ReadOnlyArraySpan<T>` always carry a
                    // non-nullable class-typed `Source: Array<T>` (stdlib
                    // §4.15) — both fields are class-typed regardless of
                    // `T`'s category, so all four are non-defaultable for
                    // EVERY instantiation, exactly like the fixed-shape
                    // records above; no per-`T` recursion is needed (this
                    // walker has no `TypeSymbol` to recurse into for a
                    // null-declaration builtin anyway).
                    //
                    // MEASURED (issue #4245; re-verified identically on
                    // this file's own parent `9ba93e3` before this edit):
                    // `default(Span<int>)`, `default(ReadOnlySpan<int>)`,
                    // `default(ArraySpan<int>)`, `default(
                    // ReadOnlyArraySpan<int>)` all compiled clean (exit 0)
                    // and ran clean (exit 0), `.Length` silently reading
                    // back 0 instead of being rejected — the same
                    // degrade-to-zero symptom already measured above for
                    // `ReadOnlyStringSpan`. The `where T: new()` top-level
                    // substitution axis was WORSE: `HolderSpan<Span<int>>
                    // .Make()` (and the sibling ReadOnlySpan/ArraySpan/
                    // ReadOnlyArraySpan substitutions) crashed the
                    // COMPILER ITSELF (SIGSEGV, `Emitter::lowerExprInner`,
                    // `LlvmBackend_Exprs.cpp:3830`, `CreateExtractValue`
                    // on a bad aggregate) rather than the generated
                    // program — see `Resolver::argSatisfiesNew`'s own
                    // comment (this file) for that axis's fix; rejecting
                    // here means `default(...)`/the record-field recursion
                    // never reaches codegen with an unmonomorphized
                    // builtin type parameter in the first place.
                    //
                    if (head == "ExceptionInfo" ||
                        head == "ReadOnlyStringSpan" ||
                        head == "Span" || head == "ReadOnlySpan" ||
                        head == "ArraySpan" || head == "ReadOnlyArraySpan") {
                        return true;  // non-nullable string/class field(s): no default
                    }
                    // fix #4294 (§17.11, v0.298): `IGrouping<TKey,T>` (stdlib
                    // §5: `public interface IGrouping<TKey, T>: IEnumerable
                    // <T>`) is, like the six names immediately above,
                    // unconditionally non-defaultable — but for a DIFFERENT
                    // reason, so it gets its own check rather than joining
                    // their list: those six are RECORDS whose declared
                    // FIELDS happen to include a non-nullable one; IGrouping
                    // is an INTERFACE, which §17.11 classifies as a non-
                    // nullable reference type outright, with no field
                    // composition to inspect at all. This is exactly the
                    // reasoning the TypeSymbol-backed arm below already
                    // applies to any REAL interface (`k == Class || k ==
                    // Interface -> return !t->declaration->isValueBuiltin` —
                    // true for every interface, since there is no such thing
                    // as a "value builtin interface"); IGrouping needs the
                    // identical conclusion because it has no `TypeSymbol` to
                    // reach that arm through (it is a null-declaration
                    // builtin, TypeResolution_Names.cpp's arity-2 table,
                    // alongside DictionaryEntry/DrainedEntry).
                    //
                    // MEASURED (issue #4294, this file's own parent
                    // `f3d3a4d`, clean tree, `bezelc` rebuilt directly):
                    // `default(IGrouping<string, int>)` compiled clean (exit
                    // 0) and ran clean (exit 0) — a silent, unrejected
                    // zero-initialized value where §17.11 ("Not defaultable:
                    // non-nullable reference types") requires a compile-time
                    // error.
                    if (head == "IGrouping") {
                        return true;  // interface: non-nullable reference
                                      // type (§17.11), unconditionally
                    }
                    // fix #4309 (§17.11, v0.298; send-back on #4294): the
                    // 25-name closed set of every OTHER null-declaration
                    // builtin interface — see
                    // `kNonConstructibleBuiltinInterfaceNames`'s own
                    // comment (this file, above `Resolver::
                    // argSatisfiesNew`) for the derivation and why
                    // `IGrouping` (just above) and the TypeSymbol-backed
                    // seven are not in it. Same rule as the TypeSymbol-
                    // backed arm below (`k == Class || k == Interface ->
                    // return !isValueBuiltin`, unconditionally true for
                    // every interface): §17.11, "Not defaultable:
                    // non-nullable reference types".
                    //
                    // MEASURED (issue #4309, this file's own parent
                    // `e0182bc`, clean tree, `bezelc` rebuilt directly):
                    // e.g. `default(IEnumerable<int>)`,
                    // `default(IDisposable)`,
                    // `default(IOrderedEnumerable<int>)` all compiled
                    // clean (exit 0) and ran clean (exit 0) — the identical
                    // silent zero-initialized-value symptom #4294 fixed for
                    // `IGrouping` alone, reproduced on every other name in
                    // the set.
                    if (isNonConstructibleBuiltinInterfaceName(head)) {
                        return true;  // interface: non-nullable reference
                                      // type (§17.11), unconditionally
                    }
                    // fix #4245 (§17.11/stdlib §4.8, v0.298): `DictionaryEntry<K,V>`
                    // is a DIFFERENT shape from the six names above — those are
                    // non-defaultable UNCONDITIONALLY (a fixed class/string field
                    // independent of the substitution). `DictionaryEntry<K,V>`'s
                    // declared fields ARE its own generic parameters (stdlib §4.8:
                    // "public record DictionaryEntry<K, V>(Key: K, Value: V);" —
                    // `Key: K`, `Value: V`, no wrapping), so its defaultability is
                    // INSTANTIATION-DEPENDENT: `DictionaryEntry<int,int>` IS
                    // defaultable (every field defaults); `DictionaryEntry<string,
                    // int>` is NOT (`Key: string` has no default, §17.11) — the
                    // exact same per-field rule the Record-kind arm below applies
                    // to a user record, just with no `TypeSymbol` to
                    // `recordStorageMembers`-walk.
                    //
                    // This null-declaration leaf still doesn't need a
                    // `seedRecordSubstEnv` call (that needs an `ast::TypeDecl`/
                    // `TypeSymbol`, which `DictionaryEntry` has neither) — because
                    // unlike an arbitrary generic record, whose fields may compose
                    // their generic parameters into some other shape entirely,
                    // `DictionaryEntry`'s two "virtual fields" ARE the identity of
                    // K/V. `t->genericArgs` is already exactly what a
                    // `recordStorageMembers` walk would have produced: the
                    // resolved, substituted-at-THIS-use-site K/V
                    // (`TypeResolution_Names.cpp`'s builtin-arity branch populates
                    // `ref.genericArgs` via `resolveType`, same as every other
                    // null-declaration builtin generic argument). Each is still run
                    // through `common::resolveRecordFieldSubst(ga, incoming)` —
                    // the SAME substitution primitive the Record-kind arm below
                    // uses, not a new mechanism — so a K/V that is itself a bare,
                    // not-yet-substituted OUTER generic parameter (DictionaryEntry
                    // appearing as a field of some other generic record, `incoming`
                    // carrying that record's own env) still resolves correctly.
                    //
                    // MEASURED (issue #4245, this file's own parent `8ca85ce`,
                    // clean tree): `default(DictionaryEntry<string, int>)` compiled
                    // clean (exit 0) and SIGSEGV'd at runtime (exit 139) reading
                    // `.Key.Length`.
                    if (head == "DictionaryEntry" && t->genericArgs.size() == 2) {
                        for (const TypeRef* ga : t->genericArgs) {
                            common::RecordSubstEnv::Entry sub =
                                common::resolveRecordFieldSubst(ga, incoming);
                            if (typeHasNoDefaultWalk(sub.ty, sub.env, ctx, seen))
                                return true;
                        }
                        return false;  // both Key/Value defaultable
                    }
                }
                return false;  // generic param / unresolved / other builtin — defer
            }
            auto k = t->declaration->kind;
            // fix #3108 (§16.3/§16.4): `default(Range<T>)` is a compile
            // error — the all-zero default would falsify Range's own "no
            // zero-step" invariant. Checked before the general Class/
            // Interface arm so the builtin isn't admitted through
            // `!isValueBuiltin` (Range IS a value builtin).
            if (t->declaration == ctx.builtinClassSym("Range")) return true;
            if (k == ast::TypeDeclKind::Class ||
                k == ast::TypeDeclKind::Interface)
                return !t->declaration->isValueBuiltin;
            // fix #3044 (§17.11): `default(E)` is a compile error for an
            // enum with no declared constant of underlying value 0. An
            // enum whose declared-value set cannot be decided is treated
            // as well-formed (conservative — matches enumHasZeroConstant's
            // own documented default).
            if (k == ast::TypeDeclKind::Enum) {
                std::optional<bool> hasZero =
                    enumHasZeroConstant(*t->declaration);
                return hasZero && !*hasZero;
            }
            // §35.10 (v0.200, #2044): every choice type is non-defaultable,
            // independent of substitution.
            if (k == ast::TypeDeclKind::Choice) return true;
            if (k == ast::TypeDeclKind::Record) {
                if (!seen.insert(t).second) return false;  // cycle: defer
                common::RecordSubstEnv env;
                common::seedRecordSubstEnv(*t->declaration, *t, incoming, env);
                const common::RecordSubstEnv* fieldEnv =
                    env.map.empty() ? nullptr : &env;
                bool illFormed = false;
                recordStorageMembers(
                    t->declaration, ctx,
                    [&](const RecordStorageMember& rsm) {
                        if (!rsm.type) return true;  // unresolved — defer
                        common::RecordSubstEnv::Entry sub =
                            common::resolveRecordFieldSubst(rsm.type,
                                                            fieldEnv);
                        if (typeHasNoDefaultWalk(sub.ty, sub.env, ctx,
                                                 seen)) {
                            illFormed = true;
                            return false;  // stop
                        }
                        return true;
                    },
                    /*env=*/nullptr);
                seen.erase(t);  // PATH semantics — see the header comment
                return illFormed;
            }
            return false;
        }
        default:
            return false;  // Unit / Error
    }
}

}  // namespace

bool typeHasNoDefault(const TypeRef* t, SemaContext& ctx,
                      const common::RecordSubstEnv* incoming) {
    // fix #4699 (§9.9, v0.307, proposal 0107): a constructor-only record has
    // no well-defined default (§17.11's "Not defaultable" list names it
    // directly), so the value-recursion below never has to find a
    // non-defaultable field — the record IS already non-defaultable at the
    // type level. Bail out before the walk; the per-field recursion
    // `typeHasNoDefaultWalk` runs only when the record itself is
    // defaultable, so this is a SHORT-CIRCUIT, not a duplicated check.
    //
    // Note the SUBSTITUTION interaction (§14.2's "value substitution is
    // equivalent to `default(T)`" rule — the cycle-23 s9_9_0003 row): the
    // property is carried on the record's DECLARATION, not on its
    // instantiation, so checking it off `t->declaration` (the bare
    // declaration-side TypeSymbol) is correct regardless of `genericArgs`.
    // The shared call sites (`defaultIllFormed` at §17.11, `argSatisfiesNew`
    // at §14.2, and §22.10's inline-array-of-T defaultability check) all
    // reach the same `default(R)` semantics, so they all close at this one
    // entry point.
    if (t && !t->nullable && t->kind == TypeRef::Kind::Named && t->declaration &&
        t->declaration->kind == ast::TypeDeclKind::Record &&
        recordIsConstructorOnly(t->declaration)) {
        return true;
    }
    std::unordered_set<const TypeRef*> seen;
    return typeHasNoDefaultWalk(t, incoming, ctx, seen);
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

// §14.2 (fix #3146): when `item` is a type bound whose bound is a BARE
// single-part name — the shape a `where T: U` type-PARAMETER bound takes — the
// name; otherwise "".
//
// Both sources, one predicate. A LOCAL bound is the AST node, and a bare name is
// a `QualifiedName` with exactly one part. An IMPORTED bound is a resolved
// TypeRef, and a type PARAMETER is precisely the case `Loader::lookupNamed`
// cannot resolve — a Named ref with a null declaration whose `importedName`
// holds the spelling. A qualified imported head (`Dep.Thing`) is rejected here
// for the same reason the local branch rejects `parts.size() != 1`: it is not a
// type-parameter spelling. The caller still checks that the name really is one
// of the declaring type's parameters, so a non-parameter bound falls through to
// the ordinary type-bound check exactly as before.
static std::string_view constraintBoundBareName(
    const GenericParamConstraint& item) {
    if (item.kind != "type") return {};
    if (item.astBound) {
        if (!item.astBound->name || item.astBound->name->parts.size() != 1)
            return {};
        return item.astBound->name->parts.back();
    }
    const TypeRef* b = item.importedBound;
    if (!b || b->kind != TypeRef::Kind::Named || b->declaration) return {};
    if (!b->genericArgs.empty()) return {};  // `IFoo<T>`, not a bare parameter
    std::string_view n = b->importedName;
    if (n.empty() || n.find('.') != std::string_view::npos) return {};
    return n;
}

void Resolver::checkOneConstraint(SourceLocation useSite,
                                  std::string_view paramName,
                                  const GenericParamConstraint& item,
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
        ctx_.diagnostics().report(Severity::Error, useSite, msg);
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
        //
        // fix #3235 (§30.14 / §14.2, v0.254): DEFER the `isSendableType`
        // call to `checkSendableConstraintsLate`, run at the tail of
        // `typecheckExpressions()`. `isSendableType` walks a class's §30.14
        // rule-2 storage members, and for a class that includes the
        // primary-ctor parameters IF `FieldSymbol::posParamBodyRef` is
        // set (§22.10 captured parameters, storage; uncaptured ones,
        // skipped). The bit is set by ExprTypecheck's expression walk —
        // `resolveTypes()` finishes BEFORE that walk starts, so reading
        // the bit here answers "uncaptured" for every class and a
        // captured primary-ctor parameter of non-sendable type passes
        // `where T: sendable`. The late pass replays this check after
        // the expression walk has run, when the bit is populated, and
        // sees the captured storage correctly. We still record the
        // diagnostic location, the constraint param name, the argument
        // spelling, and the argument TypeRef so the late replay can
        // reproduce the exact diagnostic message — same shape the
        // in-place `report` would have produced. The Error TypeRef
        // defer is unchanged: an upstream failure has already been
        // reported, no late replay needed. The pending list lives on
        // `SemaContext` (not the Resolver) because the late pass runs
        // through a fresh Resolver wrapper — see
        // `SemaContext::pendingSendableConstraints` for the rationale.
        if (arg->kind == TypeRef::Kind::Error) return;
        ctx_.addPendingSendableConstraint(
            SemaContext::PendingSendableConstraint{
                useSite, param, argName, arg});
        return;
    }
    if (item.kind == "hashable") {
        // §14.2 "Derived-property constraints" (v0.214, #2087): the constraint
        // names the §3.6 **hashable** derived-property (the keyword
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
        //
        // fix #3146: an IMPORTED bound arrives already resolved (the loader
        // built it from the artifact's MetaType); a LOCAL one is still the
        // unresolved AST node, resolved on demand below because an
        // instantiation site may be visited before the declaring type's own
        // constraint clause.
        TypeRef* bound = item.importedBound;
        if (!bound) {
            if (!item.astBound) return;
            bound = ctx_.resolvedType(*item.astBound);
            if (!bound) bound = resolveType(*item.astBound, boundScope);
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
        // §9.7 (v0.307): records and enums can never implement
        // IDisposable/IAsyncDisposable — which only classes implement. These
        // auto-imported builtin interfaces resolve to a Named TypeRef WITHOUT a
        // synthesized declaration, so without this they would fall through the
        // decl-null defer below. Mirrors the primitive check at lines 10903-10908
        // and the call-site check in ExprTypecheck_Calls.cpp.
        if (arg->kind == TypeRef::Kind::Named &&
            arg->declaration &&
            (arg->declaration->kind == ast::TypeDeclKind::Record ||
             arg->declaration->kind == ast::TypeDeclKind::Enum) &&
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
        if (!argSatisfiesTypeBound(arg, bound, &ctx_)) {
            deferredTypeBoundFailures_.push_back(DeferredTypeBoundFailure{
                useSite, param, boundName, argName, arg, bound});
        }
        return;
    }
    // Unknown constraint kind: ignore (parser would not produce it).
}

void Resolver::checkGenericConstraints(const ast::Type& t, TypeSymbol* tsym,
                                       const std::vector<TypeRef*>& argRefs,
                                       Scope* scope) {
    // fix #3146 (§14.2 / §2.8.3 rule 4, v0.252 — family F13 of umbrella #3152).
    // This used to open by casting `tsym->decl` to an `ast::TypeDecl` and
    // returning when it was null or carried no `where` clause. `tsym` is the
    // DECLARING type, so for `Dep.Registry<K>` it is imported and the cast is
    // always null — EVERY §14.2 constraint on EVERY imported generic type went
    // unchecked at every instantiation site, silently. MEASURED on 558c27c with
    // a real two-module build: `class BoxC<T> where T: class` exported from a
    // dependency admitted `BoxC<int>` in the importer with no diagnostic, while
    // the identical declarations in ONE module reported "type argument `int`
    // does not satisfy the constraint `where T: class` (§14.2)".
    //
    // The constraints are now read off `GenericParamSymbol::constraints`, which
    // SymbolCollection re-keys from the declaring node's `where` clauses and
    // ModuleLoader rebuilds from `MetaGenericParam` — one representation, both
    // sources. `td` survives for ONE residual use below (see the
    // recordDeferredNested call).
    //
    // fix #3160 (§14.2, v0.256): the receiver of a static-member access
    // (`Ferry<Bad>.Count()`) used to BYPASS this whole function — it is the
    // sole syntactic position that mints an instantiated TypeRef without ever
    // going through `Resolver::resolveType`. This wrapping-form overload is
    // now a thin adapter: it pulls `t.span.begin` (diagnostic location) and
    // the per-argument AST nodes out of `t.genericArgs` and forwards
    // everything else to `checkGenericConstraintsAtUseSite`, which the
    // static-member-access receiver path also calls. Every §14.2 constraint
    // form (`where T: sendable`, `where T: class`, a base-type bound, a
    // type-parameter bound) now fires uniformly at every instantiation site.
    // The deferred-nested path (where the arg is itself an in-scope generic
    // parameter of the enclosing type) is the one edge case the use-site
    // overload cannot reach — the Resolver's `deferredNested_` /
    // `deferredTypeBoundFailures_` lists are local to the resolveTypes-time
    // Resolver instance and already flushed by the time ExprTC calls the
    // overload. The wrapping-form `new` route preserves the full
    // deferred-nested behaviour; the use-site overload handles only the
    // static-member-access site, where the typeArg is a concrete class
    // (the issue's repro and every realistic instantiation site) and no
    // deferral is needed.
    if (!tsym) return;
    // SEAM-ACCESSOR: fix #3146 (family F13 of #3152): `td` survives only for the ONE
    // residual `recordDeferredNested` call ~25 lines below, explicitly
    // gated on `if (td)` and documented there as a known, tracked gap for
    // an imported `tsym`'s deferred-nested obligation — every other §14.2
    // constraint check in this function now reads `GenericParamSymbol::
    // constraints` instead.
    auto* td = dynamic_cast<const ast::TypeDecl*>(tsym->decl);
    bool anyConstraint = false;
    for (Symbol* gpSym : tsym->genericParams) {
        auto* gp = dynamic_cast<GenericParamSymbol*>(gpSym);
        if (gp && !gp->constraints.empty()) { anyConstraint = true; break; }
    }
    if (!anyConstraint) return;

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
            // ⚠ RESIDUAL (fix #3146): the deferred-nested machinery is still
            // AST-shaped — `recordDeferredNested` reads the declaring node's
            // `where` clauses to enqueue an obligation it re-checks after the
            // pass. When `tsym` is IMPORTED there is no such node, so a nested
            // obligation `C<W>` whose inner type `C` comes from a dependency is
            // still not recorded (unchanged from before this fix — the whole
            // function used to return here). Converting it needs the obligation
            // record and its visited-set key to become symbol-shaped too, which
            // is a separate change from this family's three reader sites; the
            // DIRECT check below now runs for an imported `tsym` either way.
            if (td) recordDeferredNested(*argAst, *gp, *td, t);
            continue;
        }
        if (arg->kind == TypeRef::Kind::Error) continue;

        // fix #3146: the parameter's own constraints, from the SYMBOL — the one
        // representation SymbolCollection and ModuleLoader both populate.
        for (const GenericParamConstraint& item : gp->constraints) {
            // §14.2 type-parameter bound (`where T: U`): the bound names
            // another generic parameter of `tsym`, so checkOneConstraint
            // defers on it (the bound's TypeRef has no declaration). Verify
            // it here where the sibling parameter's substitution is in hand:
            // T's substitution must be non-nullable and assignable to U's.
            std::string_view boundParam = constraintBoundBareName(item);
            if (!boundParam.empty()) {
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
                        if (!argSatisfiesTypeBound(arg, uSubst, &ctx_))
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
            checkOneConstraint(t.span.begin, gp->name, item, argAst, arg, &boundScope);
        }
    }
}

void Resolver::checkGenericConstraintsAtUseSite(
    SourceLocation useSite,
    const std::vector<const ast::Type*>& argAsts,
    const std::vector<TypeRef*>& argRefs, TypeSymbol* tsym) {
    // fix #3160 (§14.2, v0.256): §14.2 constraint check at an instantiation
    // site that does NOT have an `ast::Type` node — the receiver of a
    // `Type<...>.staticMember` access (`Ferry<Bad>.Count()`). The pre-#3160
    // IdentifierExpr handler built the instantiated TypeRef inline and never
    // invoked `Resolver::checkGenericConstraints`, so a failed `where T:
    // sendable` (or any §14.2 constraint) was silently skipped. This overload
    // takes the already-resolved argument TypeRefs, the AST nodes for the
    // arguments (for spelling in the diagnostic), and the use-site span
    // (IdentifierExpr's span — well-located, unlike the new-site path that
    // prints 0:0 from the wrapping `ast::Type` node's span), then runs the
    // SAME per-constraint logic as the wrapping form. The deferred-nested
    // and deferred-type-bound paths are intentionally skipped: they require
    // Resolver state (`deferredNested_`, `deferredTypeBoundFailures_`) local
    // to the resolveTypes-time Resolver, and ExprTC runs AFTER that
    // Resolver's late-flush passes. For the common case (concrete typeArgs —
    // every realistic instantiation site, including the issue's repro) no
    // deferral is needed; the deferred-nested edge case stays no-worse than
    // pre-#3160, since the static-member-access path never called this
    // check at all.
    if (!tsym) return;
    bool anyConstraint = false;
    for (Symbol* gpSym : tsym->genericParams) {
        auto* gp = dynamic_cast<GenericParamSymbol*>(gpSym);
        if (gp && !gp->constraints.empty()) { anyConstraint = true; break; }
    }
    if (!anyConstraint) return;

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
         pi < argAsts.size() && pi < argRefs.size(); ++pi) {
        auto* gp = dynamic_cast<GenericParamSymbol*>(tsym->genericParams[pi]);
        if (!gp) continue;
        const ast::Type* argAst = argAsts[pi];
        TypeRef* arg = argRefs[pi];
        if (!argAst || !arg) continue;
        // Skip the §14.2 deferred-nested path — see the function-level
        // comment. The wrapping form's `isGenericParamRef` check requires a
        // properly-marked scope, which this overload's caller does not
        // supply; in practice the typeArg is concrete (else the
        // IdentifierExpr wouldn't have typeArgs at all — generic params
        // are not valid typeArg spellings at a static-member-access site
        // when the param is the enclosing class's own).
        if (arg->kind == TypeRef::Kind::Error) continue;

        for (const GenericParamConstraint& item : gp->constraints) {
            std::string_view boundParam = constraintBoundBareName(item);
            if (!boundParam.empty()) {
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
                        ctx_.diagnostics().report(
                            Severity::Error, useSite,
                            std::string("type argument `") +
                            argSpelling(argAst) +
                            "` does not satisfy the constraint `where " +
                            std::string(gp->name) + ": " +
                            std::string(boundParam) + "` (§14.2)");
                    }
                    // Non-named substitutions → defer with no diagnostic
                    // (matches the wrapping form's conservatism).
                    continue;
                }
            }
            checkOneConstraint(useSite, gp->name, item, argAst, arg, &boundScope);
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
    // #2713: delegates to the shared predicate so this pass and the
    // owning-element closure cannot drift apart.
    auto isGenericParamBinding = [](TypeRef* bound,
                                    const ast::Type* boundAst) -> bool {
        return isBareGenericParamBinding(bound, boundAst);
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
            // fix #3146: the deferred-nested record is still AST-shaped (see
            // the ⚠ RESIDUAL note in checkGenericConstraints), so build the
            // symbol-shaped view at this one boundary rather than keeping a
            // second overload of the checker.
            GenericParamConstraint view;
            view.kind = p.item->kind;
            view.astBound = p.item->typeArg.get();
            checkOneConstraint(p.innerSite->span.begin, p.innerParamName, view,
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
            // §32.5 (v0.306): `Ptr<T>` is admitted per-instantiation, so a
            // diagnostic that prints the bare head `Ptr` names neither the
            // offending instantiation nor the rule. Render the arguments.
            if (!t.genericArgs.empty()) {
                out += "<";
                for (std::size_t i = 0; i < t.genericArgs.size(); ++i) {
                    if (i) out += ", ";
                    out += t.genericArgs[i] ? spellExternType(*t.genericArgs[i])
                                            : "?";
                }
                out += ">";
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

// per §32.13 clause 1 / §32.12 clause 1 (v0.306): the FFI scalar names that
// can be recognised without a declaration. The record/`Ptr<T>` arms need one,
// and go through the symbol side below.
static bool isFfiScalarPrimitiveName(std::string_view n) {
    PrimitiveType p = primitiveFromName(n);
    return p != PrimitiveType::None && isFfiPrimitive(p);
}

const TypeSymbol* Resolver::namedTypeSymbolInScope(const ast::Type& t,
                                                   Scope* scope) const {
    if (t.kind != ast::TypeKind::Named || !t.name ||
        t.name->parts.size() != 1)
        return nullptr;
    for (Scope* s = scope; s; s = s->parent) {
        auto it = s->names.find(t.name->parts.front());
        if (it == s->names.end()) continue;
        return dynamic_cast<const TypeSymbol*>(it->second);
    }
    return nullptr;
}

// per §32.5 (v0.306): "FFI-compatible scalar (integer types, `float`,
// `double`, `bool`, `IntPtr`, `UIntPtr`, `Ptr<T>`)" — §32.13 clause 1's field
// set. Reads the resolved TypeRef, never an `ast::TypeDecl`, so an IMPORTED
// record (whose TypeSymbol has no declaration at all) classifies identically
// to a local one — the #3152 seam.
bool Resolver::isFfiScalarTypeRef(const TypeRef* rt, int depth) {
    if (!rt || rt->nullable) return false;
    if (rt->kind == TypeRef::Kind::Primitive)
        return isFfiPrimitive(rt->primitive);
    if (rt->kind != TypeRef::Kind::Named) return false;
    std::string_view h = rt->headName();
    if (h == "IntPtr" || h == "UIntPtr") return true;
    if (h == "Ptr")
        return rt->genericArgs.size() == 1 &&
               isFfiPointeeTypeRef(rt->genericArgs[0], depth + 1);
    return false;
}

// per §32.12 clause 1 (v0.306): the FFI-POINTEE set — "integer types,
// `float`, `double`, `bool`, `IntPtr`, `UIntPtr`, and C-compatible records".
// One member short of the scalar set above, deliberately: `Ptr<U>` "is an
// FFI-compatible scalar … but is **not** a member here, which is exactly what
// makes `Ptr<Ptr<U>>` inadmissible".
bool Resolver::isFfiPointeeTypeRef(const TypeRef* rt, int depth) {
    if (!rt || rt->nullable) return false;
    if (rt->kind == TypeRef::Kind::Primitive)
        return isFfiPrimitive(rt->primitive);
    if (rt->kind != TypeRef::Kind::Named) return false;
    std::string_view h = rt->headName();
    if (h == "IntPtr" || h == "UIntPtr") return true;
    if (h == "Ptr") return false;  // the §32.12 v1 decline
    return isCCompatibleRecordSym(rt->declaration, depth + 1);
}

// per §32.13 clause 1: a field's type is "an FFI-compatible scalar …, an
// inline array `[N]T` of such a scalar (§32.5's element set …), or another
// C-compatible record (nesting)".
bool Resolver::isCCompatibleFieldTypeRef(const TypeRef* rt, int depth) {
    if (!rt || rt->nullable) return false;
    if (isFfiScalarTypeRef(rt, depth)) return true;
    if (rt->kind == TypeRef::Kind::InlineArray)
        return !rt->arrayOwned && isFfiScalarTypeRef(rt->arrayElement, depth);
    if (rt->kind == TypeRef::Kind::Named)
        return isCCompatibleRecordSym(rt->declaration, depth + 1);
    return false;
}

bool Resolver::isCCompatibleRecordSym(const TypeSymbol* ts, int depth) {
    // §32.13 clause 1 says recursion is impossible ("records are value
    // types"), which holds of a WELL-FORMED program; a malformed one must
    // still terminate, and §9's own cycle check owns that diagnostic.
    if (depth > 8 || !ts) return false;
    if (ts->kind != ast::TypeDeclKind::Record) return false;
    // §3.6's common rules make the property "evaluated per-instantiation for
    // generic records". That evaluation is not wired here, so a generic record
    // is conservatively NOT classified — the direction that rejects a boundary
    // declaration rather than mis-admitting one.
    if (!ts->genericParams.empty()) return false;
    bool any = false;
    for (const Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const FieldSymbol*>(m);
        if (!fs) continue;
        bool isStatic = false;
        for (auto mod : fs->modifiers)
            if (mod == "static") isStatic = true;
        if (isStatic) continue;  // §22.12: static storage is not a field slot
        any = true;
        // `storageComponentType` is the #3152 both-sources accessor: the
        // §9.1 positional `Param`, the body `FieldDecl`, or — for an imported
        // record with neither — `ctx.importedMemberType`.
        if (!isCCompatibleFieldTypeRef(sema::storageComponentType(m, ctx_),
                                       depth))
            return false;
    }
    // A record with no storage has no C struct to correspond to, so clause 2's
    // layout promise would have nothing to promise.
    return any;
}

bool Resolver::isCCompatibleRecord(const ast::Type& t, Scope* scope,
                                   int depth) {
    if (t.nullable) return false;
    return isCCompatibleRecordSym(namedTypeSymbolInScope(t, scope), depth);
}

bool Resolver::isFfiPointeeType(const ast::Type& t, Scope* scope, int depth) {
    // per §32.12 clause 1 (v0.306). A nullable pointee has no C
    // representation (§32.5's nullable-value-type rule).
    if (t.nullable) return false;
    if (t.kind != ast::TypeKind::Named || !t.name ||
        t.name->parts.size() != 1)
        return false;
    std::string_view n = t.name->parts.front();
    if (isFfiScalarPrimitiveName(n)) return true;
    if (n == "IntPtr" || n == "UIntPtr") return true;
    if (n == "Ptr") return false;  // the §32.12 v1 decline
    return isCCompatibleRecord(t, scope, depth);
}

bool Resolver::isSingleFieldHandleRecord(const ast::Type& t, Scope* scope) {
    const TypeSymbol* ts = namedTypeSymbolInScope(t, scope);
    if (t.nullable || !isCCompatibleRecordSym(ts, 0)) return false;
    const TypeRef* only = nullptr;
    int count = 0;
    for (const Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const FieldSymbol*>(m);
        if (!fs) continue;
        bool isStatic = false;
        for (auto mod : fs->modifiers)
            if (mod == "static") isStatic = true;
        if (isStatic) continue;
        ++count;
        only = sema::storageComponentType(m, ctx_);
    }
    if (count != 1 || !only || only->nullable) return false;
    // per §32.13 clause 4: "of *integer or pointer* scalar type (integer
    // types, `IntPtr`, `UIntPtr`, `Ptr<T>` — not `float`/`double`, not
    // `bool`)". The restriction is ABI-driven: `struct { double }` classes
    // SSE on SysV but rides an integer slot on Win64, so admitting it "would
    // mandate a wrong-register miscompile on a vouched target".
    if (only->kind == TypeRef::Kind::Named) {
        std::string_view h = only->headName();
        if (h == "IntPtr" || h == "UIntPtr") return true;
        return h == "Ptr" && only->genericArgs.size() == 1 &&
               isFfiPointeeTypeRef(only->genericArgs[0], 1);
    }
    if (only->kind != TypeRef::Kind::Primitive) return false;
    switch (only->primitive) {
        case PrimitiveType::Byte:
        case PrimitiveType::SByte:
        case PrimitiveType::Short:
        case PrimitiveType::UShort:
        case PrimitiveType::Int:
        case PrimitiveType::UInt:
        case PrimitiveType::Long:
        case PrimitiveType::ULong:
            return true;
        default:
            return false;
    }
}

bool Resolver::isFfiCompatibleType(const ast::Type& t, bool returnPosition,
                                   Scope* scope) {
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
            // per §32.5 (v0.306): "**`Ptr<T>`** — a typed foreign address
            // (§32.12); admitted as a parameter, as a return type, and inside
            // FFI function types, marshalling as the pointer it is. The null
            // pointer is `Ptr<T>.Zero`; `Ptr<T>?` is **not** admitted
            // (nullable value types below)." `T` carries §32.12's pointee
            // gate, so `Ptr<Ptr<U>>` and `Ptr<SomeClass>` stay out.
            if (n == "Ptr") {
                if (t.nullable) return false;
                return t.genericArgs.size() == 1 && t.genericArgs[0] &&
                       isFfiPointeeType(*t.genericArgs[0], scope);
            }
            // per §32.5 / §32.13 clause 4: the typed-handle exception to
            // "C-compatible records — by address only". A single-field record
            // over an integer or pointer scalar "is admitted **directly** —
            // parameter, return, `ref`/`out`, and callback positions —
            // marshalling exactly as its field." A C-compatible record that is
            // NOT of that form is admitted only at a `ref`/`out` parameter,
            // which is `checkExternType`'s business (the modifier is not
            // visible here).
            if (isSingleFieldHandleRecord(t, scope)) return true;
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
                    !isFfiCompatibleType(*fp.type, /*returnPosition=*/false,
                                         scope))
                    return false;
            }
            // The function's return slot is a return position (unit allowed).
            if (!t.funcReturn) return true;  // `-> ()` parsed as null return
            return isFfiCompatibleType(*t.funcReturn,
                                       /*returnPosition=*/true, scope);
        }
    }
    return false;
}

bool Resolver::checkExternType(const ast::Type& t, bool returnPosition,
                               const std::string& declSpelling, Scope* scope,
                               std::string_view paramModifier) {
    // §32.5 (fix #4660): an inline array `[N]T` as an extern RETURN type is
    // rejected — C functions do not return arrays. The diagnostic carries the
    // rule's term ("return type") and names the honest C contract (a `Ptr<U>`
    // return or a C-compatible record with an inline-array field). This check
    // runs BEFORE `isFfiCompatibleType` — the latter does not know that an
    // inline array's return-position rejection is the §32.5 carve-out, and
    // would otherwise admit `[4]int` and silently return true at every call.
    if (returnPosition && t.kind == ast::TypeKind::InlineArray) {
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            std::string("extern method ") + declSpelling +
                " has an inline array return type `" + spellExternType(t) +
                "`, which is rejected at the FFI boundary — a C function does "
                "not return arrays; declare `Ptr<U>`, or a C-compatible record "
                "with an inline-array field, per the actual C signature "
                "(\xC2\xA7""32.5)");
        return false;
    }
    // §32.5 (fix #4660) — "Not admitted, with reasons": `out owned` parameters
    // are not admitted at the FFI boundary. The diagnostic must carry the rule's
    // own spec term (`out owned`). Runs BEFORE `isFfiCompatibleType` because
    // the latter would otherwise admit the bare type (`CString`, etc.) and
    // silently return true.
    if (!returnPosition && paramModifier == "out owned") {
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            std::string("extern method ") + declSpelling +
                " declares an `out owned` parameter of type `" +
                spellExternType(t) +
                "`, which is not admitted at the FFI boundary — ownership "
                "transfer through an out-slot has no defined native contract "
                "(who allocated, who frees?); the `owned CString` return "
                "contract of \xC2\xA7""32.6 covers the owning case "
                "(\xC2\xA7""32.5)");
        return false;
    }
    // §32.5 (fix #4660) — "Not admitted, with reasons": `owned` parameters are
    // not admitted at the FFI boundary. The diagnostic must carry the rule's
    // own spec term (`owned`). Runs BEFORE `isFfiCompatibleType` because the
    // latter would otherwise admit the bare type and silently return true.
    if (!returnPosition && paramModifier == "owned") {
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            std::string("extern method ") + declSpelling +
                " declares an `owned` parameter of type `" +
                spellExternType(t) +
                "`, which is not admitted at the FFI boundary — there is no "
                "native contract for consuming a Bezel-owned argument at the "
                "FFI boundary (\xC2\xA7""32.5)");
        return false;
    }
    if (isFfiCompatibleType(t, returnPosition, scope)) return true;
    // per §32.13 clause 3 (v0.306): "Admission — by address (§32.5): as
    // `ref`/`out` extern parameters (C sees `R*`; §32.5's rules) and as
    // `Ptr<R>` pointees. **By value is not admitted**, except the single-field
    // form" — which `isFfiCompatibleType` already admitted above. The plain
    // `ref`/`out` spellings only: `out owned` is separately not admitted
    // (§32.5 "Not admitted, with reasons").
    bool byAddress = (paramModifier == "ref" || paramModifier == "out" ||
                      paramModifier == "out var");
    if (byAddress && !returnPosition && isCCompatibleRecord(t, scope))
        return true;
    // per §32.12 clause 1 (v0.306): `Ptr<T>`'s pointee gate is
    // "per-instantiation, §3.6-style diagnostics" and "any other instantiation
    // is rejected at the instantiation site with a diagnostic naming the set"
    // (stdlib §10.5). Say which set, and say why `Ptr<Ptr<U>>` is the one
    // deliberate absence from it.
    if (t.kind == ast::TypeKind::Named && t.name &&
        t.name->parts.size() == 1 && t.name->parts.front() == "Ptr" &&
        !t.nullable && t.genericArgs.size() == 1 && t.genericArgs[0]) {
        const ast::Type& arg = *t.genericArgs[0];
        bool nested = arg.kind == ast::TypeKind::Named && arg.name &&
                      arg.name->parts.size() == 1 &&
                      arg.name->parts.front() == "Ptr";
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            "extern method " + declSpelling + " has type `" +
                spellExternType(t) +
                "`, whose pointee is not in the FFI-pointee set — the integer "
                "types, `float`, `double`, `bool`, `IntPtr`, `UIntPtr`, and "
                "C-compatible records (\xC2\xA7""32.13)" +
                (nested ? std::string(
                              ". `Ptr<Ptr<U>>` is not admitted: use "
                              "`Ptr<IntPtr>` plus an explicit "
                              "`Ptr<U>.FromIntPtr` re-typing, one visible step "
                              "per indirection")
                        : std::string()) +
                " (\xC2\xA7""32.12)");
        return false;
    }
    // per §32.13: a record that reached here is either not C-compatible at
    // all, or is C-compatible but written by value. Naming WHICH is the
    // difference between an actionable diagnostic and "not permitted".
    if (t.kind == ast::TypeKind::Named && t.name &&
        t.name->parts.size() == 1 && !t.nullable) {
        if (const TypeSymbol* ts = namedTypeSymbolInScope(t, scope)) {
            if (ts->kind == ast::TypeDeclKind::Record) {
                std::string name(t.name->parts.front());
                if (!isCCompatibleRecord(t, scope)) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        "extern method " + declSpelling + " has record type `" +
                            name +
                            "`, which is not C-compatible: every field's type "
                            "must be an FFI-compatible scalar (an integer "
                            "type, `float`, `double`, `bool`, `IntPtr`, "
                            "`UIntPtr`, `Ptr<T>`), an inline array of such a "
                            "scalar, or another C-compatible record "
                            "(\xC2\xA7""32.13)");
                } else if (returnPosition) {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        "extern method " + declSpelling + " has record type `" +
                            name +
                            "`, which is C-compatible but admitted by address "
                            "only — a return slot has no address to pass. Only "
                            "the single-field form (exactly one field of "
                            "integer or pointer scalar type) is admitted "
                            "directly; by-value structs are deferred "
                            "(\xC2\xA7""32.13, \xC2\xA7""32.14)");
                } else {
                    ctx_.diagnostics().report(
                        Severity::Error, t.span.begin,
                        "extern method " + declSpelling + " has record type `" +
                            name +
                            "`, which is C-compatible but admitted by address "
                            "only — declare the parameter `ref` or `out`, or "
                            "take a `Ptr<" + name +
                            ">`. Only the single-field form (exactly one field "
                            "of integer or pointer scalar type — not `float`, "
                            "`double` or `bool`) is admitted directly by value "
                            "(\xC2\xA7""32.13)");
                }
                return false;
            }
        }
    }
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
    // §32.5 (fix #4659) — "Nullable value types are rejected": a nullable
    // primitive (`int?`, `bool?`), a nullable pointer-shaped scalar (`IntPtr?`,
    // `UIntPtr?`), `Ptr<T>?`, and a nullable inline array (`[N]T?`, OR an inline
    // array whose element type is itself nullable — `[N](int?)`, which the
    // parser stores with `t.nullable=false` and `t.arrayElement->nullable=true`
    // per the comment in Parser_Decls.cpp's parseType()) all have no C
    // representation. The diagnostic must carry the rule's own spec term
    // (`nullable`); it runs BEFORE the generic catch-all so a single condition
    // owns one message. The companion pointer-carrier rule (`CString?`,
    // `WString?`, `RawBytes?`) is nullable-by-design and is admitted above in
    // `isFfiCompatibleType`'s Named case.
    bool isNullableValueType = false;
    if (t.nullable) {
        if (t.kind == ast::TypeKind::Named && t.name &&
            t.name->parts.size() == 1) {
            const std::string_view nm = t.name->parts.front();
            isNullableValueType =
                primitiveFromName(nm) != PrimitiveType::None ||
                nm == "Ptr" || nm == "IntPtr" || nm == "UIntPtr";
        } else if (t.kind == ast::TypeKind::InlineArray) {
            isNullableValueType = true;
        }
    } else if (t.kind == ast::TypeKind::InlineArray && t.arrayElement &&
               t.arrayElement->nullable) {
        isNullableValueType = true;
    }
    if (isNullableValueType) {
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            std::string("extern method ") + declSpelling + " has type `" +
                spellExternType(t) +
                "`, which is a nullable value type — nullable value types are "
                "rejected at the FFI boundary (§32.5)");
        return false;
    }
    // §32.5 (fix #4659) — "Owning inline arrays and inline arrays of other
    // element types are not admitted": `[N owned]T` (class-typed) and `[N]T`
    // whose T is not an FFI-compatible primitive are not admitted. The
    // diagnostic carries the rule's own spec term (`not admitted`); it runs
    // BEFORE the generic catch-all. The element-shape rejection lives in
    // `isFfiCompatibleType`'s InlineArray case above (returns false for
    // non-primitive element types) — only the owning form needs its own
    // diagnostic here because the catch-all's `not FFI-compatible` wording
    // would name the rule's "Not allowed" set rather than the inline-array
    // "not admitted" sub-rule.
    if (t.kind == ast::TypeKind::InlineArray && t.arrayOwned) {
        ctx_.diagnostics().report(
            Severity::Error, t.span.begin,
            std::string("extern method ") + declSpelling + " has type `" +
                spellExternType(t) +
                "`, which is an owning inline array — owning inline arrays are "
                "not admitted at the FFI boundary (§32.5)");
        return false;
    }
    // per §32.5: catch-all for the §32.5 "Not allowed" set (Bezel `string`,
    // `char`, `decimal`, enums, choices, classes, interfaces, generic type
    // parameters, `Array<T>`/`OwningArray<T>` and other collections, records
    // other than the §32.13 forms, inline arrays of other element types).
    // Use the section's own term — the section title is "FFI-compatible
    // types" (line 9629), and §32.5 lists what is *not* admitted by stating
    // what *is*. The diagnostic carries the substring `FFI-compatible` for
    // any caller that pattern-matches the rule's term.
    std::string what = spellExternType(t);
    ctx_.diagnostics().report(
        Severity::Error, t.span.begin,
        std::string("extern method ") + declSpelling + " has type `" + what +
            "`, which is not FFI-compatible (§32.5)");
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
            // SEAM-ACCESSOR: `t` is drawn from `ctx_.module()->children`
            // (checkVariancePositions's own loop) — never an imported type
            // (#3152).
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
    // NOT `forEachTypeAndNested` (fix #3574): this recursion is INTERLEAVED
    // with the per-member work above, so each nested type is visited between
    // its siblings rather than after all of them. It already reaches every
    // depth; routing it through the shared pre-order walk would only reorder
    // the diagnostics.
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
//   - A positional record or auto-hashable body record that itself
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
// isSerializationEligibleType below (issue #1065, fixed by #2326): true
// when `tr` is a `string` (directly or through a nested inline array of
// strings) — i.e. a shape that USED to be rejected (the deserializer
// loop would leak the per-element strings, the #1747 release walk
// only saw a record's direct fields). The #1747 release walk was
// extended to recurse into inline arrays (#2326,
// LlvmBackend_impl.hpp), so the `[N]string` and `[N][N]string` shapes
// are NOW leak-clean — the InlineArray branch's reject gate is REMOVED
// for them. The helper now returns false for these leak-clean shapes, and
// the branch above simply does not call this helper anymore (the helper
// remains in place as a guard for FUTURE inline-array-of-Named-element
// shapes that re-introduce a leak — see the InlineArray branch's inline
// comment for the current "rejected as risky" set, which is the
// over-broad Named-record/choice exclusion that remains as a follow-up
// after the codegen release walk covers those transitively).
static bool typeRiskingUnreleasedStringInInlineArray(const TypeRef* tr) {
    if (!tr) return false;
    using K = TypeRef::Kind;
    if (tr->kind == K::Primitive)
        return tr->primitive == PrimitiveType::String;
    if (tr->kind == K::InlineArray)
        return tr->arrayElement &&
               typeRiskingUnreleasedStringInInlineArray(tr->arrayElement);
    return false;
}

static bool isSerializationEligibleType(
        const TypeRef* tr,
        std::unordered_set<const TypeSymbol*>& visited,
        SemaContext& ctx,
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
        // fix #3140 follow-up (§9.8 / §15.1 / §2.8.3 rule 4): §9.8 ADMITS "a
        // non-owning inline array [N]T/[N]T? of record-admissible element
        // type", but the emitter can only WRITE one whose size survived to
        // codegen. `ModuleMetadataBuilder` never writes `MetaType::arraySize`,
        // so an IMPORTED `[N]T` lowers to a bare `i32` placeholder and
        // `emitSerializeField`'s inline-array arm — a four-conjunct positive
        // guard with no `else` — silently fell through to writing NOTHING while
        // the wire header still counted the component. MEASURED: 22 bytes
        // imported against 50 local for the same all-zero
        // `record InlineArr(A: [3]int, N: int)`. Rejecting here puts the fault
        // at the conformance declaration, where the author can act on it,
        // instead of in the bytes. See `serializationComponentTypeIsEmittable`.
        if (!serializationComponentTypeIsEmittable(tr)) return false;
        // Codegen restriction (issue #1065 follow-up, filed): the synthesized
        // Deserialize's element-materialization loop (LlvmBackend_Serialization.cpp)
        // constructs the inline array via a raw GEP-store loop that bypasses the
        // `recordHasStringFields`/`emitRecordStringFieldReleases` retain/release
        // bookkeeping (LlvmBackend_impl.hpp, fix #1747) — that bookkeeping
        // only inspected a record's DIRECT storage members' kind
        // (Primitive/String), so it did not see a `string` nested inside an
        // inline-array component. Admitting such an element type here
        // would synthesize a Deserialize that leaked every array-element
        // string on the caller's scope exit (verified: a `[N]string`
        // field leaked 2 allocations per round-trip under ASan/LSan on
        // the pre-#2326 base). The #2326 fix extended the release walk
        // to recurse into inline arrays (recordHasStringFields /
        // emitRecordStringFieldReleases_impl / emitInlineArrayStringReleases
        // — LlvmBackend_impl.hpp / LlvmBackend_Disposal.cpp), so the
        // `[N]string` and `[N][N]string` shapes are now leak-clean and
        // ADMITTED via the recursive call below. The previous reject for
        // "string" and "Named record/choice" elements is REMOVED. A
        // non-string / non-record element is admitted by the same
        // recursive call into the standard Named branch. primitives-
        // other-than-string and enums carry no such obligation and
        // continue to be admitted. The pinned conservative behavior for
        // the RECORD-element case (covered by the dedicated
        // inline_array_record_element_still_rejected_1065_followup test)
        // is the same shape that #2326's codegen fix already handles
        // correctly — the test stays as a regression pin until the
        // broader transitive record/choice walk lands.
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
        // fix #3139 (§9.8 / §2.5, family F1 of #3152): the conformance comes
        // from the SYMBOL. The `if (!td) return false` this replaces made every
        // IMPORTED record component ineligible, so a LOCAL record whose
        // component type is an imported serializable record was rejected —
        // "field `P` has a type that is not serialization-eligible" (measured)
        // — for a program whose parts each compile.
        //
        // fix #3140 (family F2 of #3152): #3139's temporary
        // `serializationSurfaceIsEmittable` interlock is GONE from this arm.
        // It was here because codegen's component walk for THIS record still
        // needed the `ast::TypeDecl`, so admitting the component on conformance
        // alone made the enclosing record serialize it to NOTHING — measured on
        // a real two-module build as a local `Wrap(P: Pt, N: int)` over an
        // imported serializable `Pt` round-tripping to 5 for an expected 345,
        // with no diagnostic at any severity. `emitRecordSerialize` /
        // `emitRecordDeserialize` now walk `collectRecordStorage` and read each
        // component's clause through `sema::serializedComponentClause`, which
        // answers for an imported member, so the reason for the gate is gone;
        // the same probe now yields 345.
        bool declaredISerializable =
            declaresBuiltinInterface(ts, "ISerializable");
        bool declaredIDeserializable =
            declaresBuiltinInterface(ts, "IDeserializable");
        if (!declaredISerializable && !declaredIDeserializable) return false;
        // fix #3140 follow-up (§9.8): the "trust it, it is checked at its own
        // conformance site" posture below is UNBACKED for an imported record —
        // `Resolver::checkSerializationEligibility` iterates
        // `ctx_.module()->children`, local types only, so that site never ran
        // for it. Walk its components for an emittable shape here instead of
        // trusting a check that did not happen.
        if (!serializationSurfaceIsEmittable(ts, ctx)) return false;
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
        // fix #3139 / fix #3140 — the choice twin of the record arm above; see
        // it for the measured consequence of the `ast::TypeDecl` gate and for
        // why the emittability interlock that stood here is now gone.
        bool declSer   = declaresBuiltinInterface(ts, "ISerializable");
        bool declDeser = declaresBuiltinInterface(ts, "IDeserializable");
        if (!declSer && !declDeser) return false;
        // fix #3140 follow-up — the choice twin of the record arm's component
        // walk; see it for why "checked at its own site" is unbacked here.
        if (!serializationSurfaceIsEmittable(ts, ctx)) return false;
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
        // fix #3140 (family F2 of #3152): ask the symbol, not `ts->decl`. For a
        // LOCAL class the answer is identical — the synthesis pre-pass in
        // checkSerializationEligibility writes the derived shape onto the
        // symbol and no longer onto the AST clause. For an IMPORTED class it is
        // still "no surface", and see `serializedClassShape`'s comment in
        // Symbol.hpp for why that is a measured statement rather than a gap in
        // the carriage: cross-module class serialization SIGSEGVs at every
        // level today (#3179), so this reject is the only correct answer.
        return !serializedClassShape(ts).empty();
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
// fix #3044 (§9.8 / §17.11, v0.242): §9.8 — "A `serialized skip` component whose
// type has no well-defined default is a compile error at the conformance
// declaration (the synthesized `Deserialize` would have no value to construct
// with)." `skip` reconstructs the component with `default(T)`, and §17.11 makes
// `default(E)` an error for an enum with no declared constant of underlying
// value 0 — so such a component must be rejected rather than silently
// reconstructing at 0, a value equal to no declared constant (§5.4).
//
// Scoped to the ENUM row of §17.11's not-well-defined set, which is the row this
// fix owns. The other rows (a non-nullable reference type, a record carrying
// one, a choice) are checked at their own sites or remain the pre-existing gap
// #3072 records; this predicate answers only for enums so it can never widen a
// reject beyond what #3044 changed. Returns the reason, or "" when fine.
static std::string skipComponentEnumLacksZero(const TypeRef* ty) {
    if (!ty || ty->nullable) return {};  // `T?` reconstructs as null — fine
    if (ty->kind != TypeRef::Kind::Named || !ty->declaration) return {};
    if (ty->declaration->kind != ast::TypeDeclKind::Enum) return {};
    std::optional<bool> hasZero = enumHasZeroConstant(*ty->declaration);
    if (!hasZero || *hasZero) return {};
    return std::string("enum `") + std::string(ty->declaration->name) +
           "` declares no constant of underlying value 0, so `default(" +
           std::string(ty->declaration->name) + ")` — which `serialized skip` "
           "reconstructs it with — is a compile error (§9.8/§17.11); declare a "
           "zero-valued constant, make the component nullable, or drop `skip`";
}

static bool positionalFieldIsSkip(const ast::Param& pf) {
    return pf.serialized && pf.serialized->isSkip;
}

// fix #4688 (s9_8_0004) (§9.8 / §17.11): the same rule as
// `skipComponentEnumLacksZero` above, for `serialized optional` instead of
// `serialized skip`. §9.8: "A `serialized optional` component whose type is
// not defaultable (§17.11) is a compile error at the conformance declaration
// (a missing field would have no value to supply) ... exactly as for
// `serialized skip` below" — on read, a missing optional field is supplied
// `default(T)` exactly like a skip component's reconstruction, so the same
// §17.11 enum-lacks-a-zero-constant defect applies verbatim.
//
// Scoped identically to `skipComponentEnumLacksZero` (fix #3044): the ENUM
// row only. The other non-defaultable rows for `serialized optional` (a
// non-nullable reference type, a record carrying one, a choice) are the same
// pre-existing gap #3072 already records for `skip`; this predicate does not
// widen past what it answers for. Returns the reason, or "" when fine.
static std::string optionalComponentEnumLacksZero(const TypeRef* ty) {
    if (!ty || ty->nullable) return {};  // `T?` reconstructs as null — fine
    if (ty->kind != TypeRef::Kind::Named || !ty->declaration) return {};
    if (ty->declaration->kind != ast::TypeDeclKind::Enum) return {};
    std::optional<bool> hasZero = enumHasZeroConstant(*ty->declaration);
    if (!hasZero || *hasZero) return {};
    return std::string("enum `") + std::string(ty->declaration->name) +
           "` declares no constant of underlying value 0, so it is not "
           "defaultable (§17.11) and `default(" +
           std::string(ty->declaration->name) + ")` — which `serialized "
           "optional` supplies for a missing field — is a compile error "
           "(§9.8/§17.11); declare a zero-valued constant, make the "
           "component nullable, or drop `optional`";
}

// fix #4688 (s9_8_0007) (§9.8, v0.182): "[a serialized name] must be
// unique among the serialized names of the type's components ... a member's
// serialized name is its rename where one is declared and its declared
// identifier otherwise, so a rename equal to another member's identifier
// collides too." Computes that effective serialized name for a positional
// field or auto-property: the `serialized "X"` rename when one is declared
// and non-empty, else the declared identifier. Comparison is ordinal (the
// wire's own comparison, §9.8), so a plain `std::string` set suffices.
static std::string effectiveSerializedName(const ast::SerializedClause* sc,
                                            std::string_view declaredName) {
    if (sc && !sc->name.empty()) return std::string(sc->name);
    return std::string(declaredName);
}

// #3152: the `hasAstAutoPropertyDecl` calls in this function and in
// `checkClassSerializationEligibilityLate` below are the AST-ONLY predicate on
// purpose. Both walk `ctx_.module()->children` and both skip any `t` with no
// `ast::TypeDecl` (`if (!td) continue;`), so `t` is always a LOCAL declaration
// and every member of it has a decl; two of the four arms go straight on to
// read `pd->serialized` besides. The ONE walk in this family that does leave
// the module is `classHasOwnStorage` (it climbs `t->baseTypes`), and that one
// asks `isAutoPropertyStorageSymbol` — see its own comment.
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
        // SEAM-ACCESSOR: `t` is drawn from `ctx_.module()->children` (this pre-pass's own
        // loop) — never an imported type (#3152).
        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) continue;
        // fix #3140 (family F2): a shape already derived on the SYMBOL is left
        // alone (this pass is idempotent and the driver may re-enter it). A
        // clause with only polyCases — set by the `polymorphic` member clause,
        // which carries no shape — still needs its shape derived below.
        if (!serializedClassShape(t).empty()) continue;
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
        // (the AST clause is the parser's own registry — the pre-pass mirrors it
        //  onto the symbol at the end of this iteration, fix #3140)
        for (const Symbol* m : t->members) {
            auto* fs = dynamic_cast<const FieldSymbol*>(m);
            if (!fs || !fs->decl) continue;
            // SEAM-ACCESSOR: `fs` is a member of `t`, which this pre-pass draws only from
            // `ctx_.module()->children` — gated on `fs->decl` one line above too
            // (#3152).
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
        // fix #3140 (§9.8, family F2 of umbrella #3152): the derived shape now
        // lands on the SYMBOL, not on `ast::TypeDecl::serializedClass->shape`.
        //
        // Twelve readers used to recover it with
        // `dynamic_cast<const ast::TypeDecl*>(ts->decl)`, and that cast is null
        // for every IMPORTED type — so each of them independently decided "this
        // class has no §9.8 surface" off a dead pointer. Writing the symbol and
        // reading it back through `sema::serializedClassShape` makes it ONE
        // predicate with one source; the AST clause now carries only the
        // parser's `polymorphic(...)` registry, mirrored just below.
        //
        // Deliberately NOT written to both places: two copies of one fact held
        // in agreement by nothing is the exact shape the audit's §4 warns about
        // (and #3168 measured drifting in family F6).
        t->serializedShape = needsGraph ? kGraphShape : kTreeShape;
        // §9.8 tier 4: mirror the registry onto the symbol, resolved. The
        // parser puts the cases on the AST clause and TypeResolution_Names has
        // already resolved each case's type by the time this pass runs.
        t->serializedPolyCases.clear();
        if (td->serializedClass) {
            for (const auto& pc : td->serializedClass->polyCases) {
                SerializedPolyCase out;
                out.discriminator = pc.discriminator;
                if (pc.type) {
                    const TypeRef* tr = ctx_.resolvedType(*pc.type);
                    if (tr) out.type = tr->declaration;
                }
                t->serializedPolyCases.push_back(out);
            }
        }
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

        // SEAM-ACCESSOR: `t` is drawn from `ctx_.module()->children` (this function's second
        // loop) — never an imported type (#3152).
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
                        // fix #3044: the enum row of the same §9.8 rule.
                        std::string whyE = skipComponentEnumLacksZero(ptr);
                        if (!whyE.empty()) {
                            ctx_.diagnostics().report(
                                Severity::Error, t->declLoc,
                                "choice `" + std::string(t->name) +
                                "`: `serialized skip` on payload `" +
                                std::string(cc.name) + "." +
                                std::string(pf.name) + "` — " + whyE);
                            foundError = true;
                            break;
                        }
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

        // fix #4688 (s9_8_0007) (§9.8 v0.182): the serialized-name
        // uniqueness set for THIS record's components — one or the other
        // branch below populates it, never both (a record is either
        // positional or body-form). Skip-marked components are excluded:
        // §9.8 "the component is excluded from both directions entirely",
        // so it never occupies a wire name.
        std::unordered_set<std::string> seenSerializedNames;

        // Positional record: check positional fields.
        if (td->hasPositionalFields) {
            for (const auto& pf : td->positionalFields) {
                if (positionalFieldIsSkip(pf)) {
                    // fix #3044 (§9.8/§17.11): a skipped component still has to
                    // reconstruct with `default(T)`.
                    std::string whyE = pf.type
                        ? skipComponentEnumLacksZero(ctx_.resolvedType(*pf.type))
                        : std::string();
                    if (!whyE.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            std::string(isRecord ? "record" : "type") + " `" +
                            std::string(t->name) +
                            "`: `serialized skip` on field `" +
                            std::string(pf.name) + "` — " + whyE);
                        foundError = true;
                        break;
                    }
                    continue;  // skip → excluded
                }
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
                // fix #4688 (s9_8_0007) (§9.8 v0.182): the serialized
                // name — the rename where declared, the identifier otherwise —
                // must be unique among the type's components; a rename equal
                // to another member's plain identifier collides too.
                {
                    std::string serName =
                        effectiveSerializedName(pf.serialized.get(), pf.name);
                    if (!seenSerializedNames.insert(serName).second) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "record `" + std::string(t->name) +
                            "`: serialized name `" + serName +
                            "` for field `" + std::string(pf.name) +
                            "` is not unique among the type's components "
                            "(§9.8 v0.182) — a `serialized \"" + serName +
                            "\"` rename collides with another component's "
                            "serialized name or declared identifier; choose "
                            "a distinct name");
                        foundError = true;
                        break;
                    }
                }
                // §9.8 v0.182 (case s9_8_0004): `serialized optional` on a
                // component whose type is not defaultable is a compile error
                // — the same §17.11 reconstruction rule `serialized skip`
                // obeys above.
                if (pf.serialized && pf.serialized->isOptional && pf.type) {
                    std::string whyE =
                        optionalComponentEnumLacksZero(ctx_.resolvedType(*pf.type));
                    if (!whyE.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "record `" + std::string(t->name) +
                            "`: `serialized optional` on field `" +
                            std::string(pf.name) + "` — " + whyE);
                        foundError = true;
                        break;
                    }
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
                if (!hasAstAutoPropertyDecl(m)) continue;
                auto* ps = dynamic_cast<const PropertySymbol*>(m);
                if (!ps) continue;
                // SEAM-ACCESSOR: `ps` is a member of `t`, which checkSerializationEligibility draws
                // only from `ctx_.module()->children` — never an imported symbol
                // (#3152).
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (!pd) continue;
                // Check for serialized skip on the auto-property.
                if (pd->serialized && pd->serialized->isSkip) {
                    // fix #3044 (§9.8/§17.11): same rule, auto-property form.
                    std::string whyE = pd->type
                        ? skipComponentEnumLacksZero(ctx_.resolvedType(*pd->type))
                        : std::string();
                    if (!whyE.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            std::string(isRecord ? "record" : "type") + " `" +
                            std::string(t->name) +
                            "`: `serialized skip` on property `" +
                            std::string(m->name) + "` — " + whyE);
                        foundError = true;
                        break;
                    }
                    continue;
                }
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
                // fix #4688 (s9_8_0007) (§9.8 v0.182): same
                // serialized-name uniqueness rule, auto-property form.
                {
                    std::string serName =
                        effectiveSerializedName(pd->serialized.get(), m->name);
                    if (!seenSerializedNames.insert(serName).second) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "record `" + std::string(t->name) +
                            "`: serialized name `" + serName +
                            "` for property `" + std::string(m->name) +
                            "` is not unique among the type's components "
                            "(§9.8 v0.182) — a `serialized \"" + serName +
                            "\"` rename collides with another component's "
                            "serialized name or declared identifier; choose "
                            "a distinct name");
                        foundError = true;
                        break;
                    }
                }
                // §9.8 v0.182 (case s9_8_0004): same `serialized optional`
                // defaultability rule, auto-property form.
                if (pd->serialized && pd->serialized->isOptional && pd->type) {
                    std::string whyE =
                        optionalComponentEnumLacksZero(ctx_.resolvedType(*pd->type));
                    if (!whyE.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, t->declLoc,
                            "record `" + std::string(t->name) +
                            "`: `serialized optional` on property `" +
                            std::string(m->name) + "` — " + whyE);
                        foundError = true;
                        break;
                    }
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
                // §9.8/§9.2: a body record must also be auto-hashable
                // for synthesis to apply (same membership conditions as §9.2).
                // If it is not auto-hashable AND doesn't have hand-written
                // Serialize/Deserialize, the synthesis cannot proceed.
                // (We don't require auto-hashable for synthesis per the
                // spec wording; we require the §9.8 body-record conditions which
                // are the same as §9.2 "auto-hashable" conditions. The
                // spec says "under exactly the auto-equality membership conditions"
                // so treat them identically.)
                // Note: we only report this if there are actually auto-properties
                // to serialize — an empty record body is still eligible (trivially).
            }
        }

        // §9.8 v0.182 (#1572), updated for §9.8 step 2 (v0.309, #4775 Q2): the
        // record constructor-selection check, the record-side counterpart to
        // the class check above. Applies only when Deserialize synthesis is
        // actually needed and the record declares one or more EXPLICIT
        // constructors. The spec rule: "If the record declares one or more
        // explicit constructors, the synthesized Deserialize calls the
        // unique explicit constructor — or, when no explicit constructor
        // matches, the auto-generated primary of a record declaring
        // positional fields (§9.3) — whose parameter list binds exactly
        // the record's components — the serialized components together
        // with every `serialized skip` component, which binds its
        // default(T) — matched by name — compared case-insensitively ...
        // If explicit constructors exist but none matches and the primary
        // does not match either, more than one explicit constructor
        // matches, or the case-insensitive comparison leaves a parameter
        // ambiguous between two components, the conformance declaration
        // is a compile error naming the candidates." So: exactly-one-
        // matching explicit ctor → use it; more-than-one-matching explicit
        // ctor → compile error; zero-matching explicit ctor + record
        // declares positional fields → fall back to the synthesized
        // primary; zero-matching explicit ctor + body record (no positional
        // fields → no synthesized primary) → compile error. A record with
        // NO explicit constructor at all (a purely positional record, or a
        // body record with no author-declared constructor) is "constructed
        // directly from the read components at the storage level" — no
        // ambiguity is possible, so the check below is a no-op whenever
        // `allCtors` comes back empty.
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
            // For a combined-form record (§9.3) the components include BOTH
            // the positional fields AND the body's auto-properties: the
            // explicit ctor's parameter list may bind either kind.
            // (Pre-fix this loop was `if (td->hasPositionalFields) { ... }
            // else { ... }`, treating a record as either positional or body
            // form — never both — so a combined-form record's body ctor
            // binding an auto-property component could not match.)
            //
            // fix #5179 (§9.8 step 2, v0.310): detect case-folded collisions
            // between components. §9.8 step 2 says "the case-insensitive
            // comparison leaves a parameter ambiguous between two
            // components" is a compile error at the conformance declaration;
            // dc ruling 2026-09-29 made the rule concrete — reject two
            // components whose case-folded names collide, intrinsic to the
            // type (no explicit-ctor selection can resolve it). The set
            // alone (pre-fix) masked the collision: `Foo` and `foo` both
            // lowered to `foo`, so the set held one entry, and no ctor's
            // arity-N parameter list could biject against a size-1 set when
            // there were two components — the matching fell through to the
            // synthesized primary and the record was silently accepted. We
            // track the first component to register each lowered name so
            // the diagnostic names both colliding components.
            std::unordered_set<std::string> compNamesLower;
            std::vector<std::string> compNamesOrig;
            std::unordered_map<std::string, std::string> compLowerFirstOrig;
            auto addComponent = [&](std::string_view name) {
                std::string orig(name);
                std::string lowered = toLower(name);
                auto prior = compLowerFirstOrig.find(lowered);
                if (prior != compLowerFirstOrig.end()) {
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "record `" + std::string(t->name) + "` declares "
                        "`IDeserializable`: components `" + prior->second +
                        "` and `" + orig + "` are ambiguous between two "
                        "components under the case-insensitive comparison "
                        "§9.8 step 2 uses to match constructor parameters — "
                        "both lowercase to `" + lowered +
                        "` (§9.8 step 2, v0.310; Appendix C.8)");
                    foundError = true;
                    return;
                }
                compLowerFirstOrig.emplace(lowered, orig);
                compNamesLower.insert(lowered);
                compNamesOrig.push_back(orig);
            };
            if (td->hasPositionalFields) {
                for (const auto& pf : td->positionalFields) {
                    if (foundError) break;
                    addComponent(pf.name);
                }
            }
            if (!foundError) {
                for (const Symbol* m : t->members) {
                    if (!hasAstAutoPropertyDecl(m)) continue;
                    addComponent(m->name);
                    if (foundError) break;
                }
            }

            // Collect non-static EXPLICIT constructors and check which match.
            // The auto-generated primary of a record declaring positional
            // fields (§9.1) is NOT a ConstructorSymbol — it has no
            // ast::ConstructorDecl — so it is intentionally absent from this
            // loop. The post-loop check below consults it as the fallback
            // when no explicit constructor matches (§9.8 step 2, v0.309).
            std::vector<std::string> matchingCtors;
            std::vector<std::string> allCtors;
            for (const Symbol* m : t->members) {
                auto* cs = dynamic_cast<const sema::ConstructorSymbol*>(m);
                if (!cs || cs->isStatic) continue;
                // SEAM-ACCESSOR: `cs` is a member of `t`, which checkSerializationEligibility draws
                // only from `ctx_.module()->children` — never an imported symbol
                // (#3152).
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

            // §9.8 step 2 (v0.309, #4775 Q2): the explicit-constructors-vs-
            // synthesized-primary disambiguation. "More than one matches" is
            // read over the EXPLICIT constructors, with the primary consulted
            // ONLY when none of them matches. So:
            //   - matchingCtors.size() == 1 → use that explicit ctor. OK.
            //   - matchingCtors.size() > 1 → ambiguous across explicit ctors;
            //     compile error naming them. (The primary is not consulted
            //     because an explicit ctor already wins.)
            //   - matchingCtors.empty():
            //       - td->hasPositionalFields → fall back to the synthesized
            //         primary. OK.
            //       - else → no primary exists; compile error naming the
            //         non-matching explicit ctor(s).
            if (!allCtors.empty()) {
                std::string compSet;
                for (const auto& n : compNamesOrig) {
                    if (!compSet.empty()) compSet += ", ";
                    compSet += n;
                }
                if (matchingCtors.size() > 1) {
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
                        " (§9.8 step 2, v0.309: \"more than one matches\" "
                        "is read over the explicit constructors, with the "
                        "synthesized primary consulted only when none "
                        "matches)");
                    foundError = true;
                } else if (matchingCtors.empty() && !td->hasPositionalFields) {
                    std::string candidates;
                    for (const auto& c : allCtors) {
                        if (!candidates.empty()) candidates += ", ";
                        candidates += c;
                    }
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "record `" + std::string(t->name) + "` declares "
                        "`IDeserializable`: no explicit constructor has "
                        "parameter names matching the serialized component "
                        "set {" + compSet + "} — candidates: " + candidates +
                        " (§9.8 step 2, v0.309: constructor selection is by "
                        "name, case-insensitive, order-insensitive; the "
                        "synthesized primary is the fallback candidate when "
                        "the record declares positional fields)");
                    foundError = true;
                }
                // else: matchingCtors.size() == 1 → use this explicit ctor
                // else: matchingCtors.empty() && td->hasPositionalFields →
                //   use synthesized primary (the new acceptance path).
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

        // SEAM-ACCESSOR: `t` is drawn from `ctx_.module()->children`
        // (checkClassSerializationEligibilityLate's own loop) — never an
        // imported type; the base-CHAIN reads a few hundred lines below
        // (`t->baseTypes`) are the ones that cross modules, and those already
        // route through symbol-side predicates (`isInstanceFieldStorageSymbol`
        // / `isAutoPropertyStorageSymbol`, fix #3152/#3182/#3221) rather than
        // an AST cast (#3152).
        auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl);
        if (!td) continue;

        bool wantsIS = declaredConformance(*td, "ISerializable");
        bool wantsID = declaredConformance(*td, "IDeserializable");
        if (!wantsIS && !wantsID) continue;

            // fix #3140 (family F2 of #3152): the shape and the registry now
            // come from the SYMBOL — see `sema::serializedClassShape`. The AST
            // clause carries only the parser's `polymorphic(...)` cases, which
            // the pre-pass mirrors onto `t->serializedPolyCases`.
            const std::string_view sccShape = serializedClassShape(t);
            const auto& sccPolyCases = serializedPolyCasesOf(t);
            if (sccShape.empty()) {
                // §9.8 v0.141: after the synthesis pre-pass in
                // checkSerializationEligibility, a class with no shape here
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
            //
            // fix #3152 (§9.8/§2.8.2 clause 1): the auto-property arm asks the
            // STORAGE predicate. Unlike every other walk in this function, this
            // one leaves the module being compiled — it climbs `t->baseTypes`,
            // so `cls` is an IMPORTED TypeSymbol whenever the base lives in a
            // dependency, and the AST-only predicate reported "no own storage"
            // for every imported auto-property. MEASURED: a local
            // `class D: ImportedBase, ISerializable, IDeserializable` whose base
            // carries a single `property W: int := 41;` COMPILED AND RAN, while
            // its character-identical single-module twin was correctly rejected
            // by the guard below — a silent accept of exactly the #2364 shape
            // this guard exists to refuse.
            //
            // fix #3182 (§9.8 / §22.8 / §2.8.2 clause 1): the FIELD arm now
            // asks the STORAGE predicate too. It used to read
            // `dynamic_cast<const ast::FieldDecl*>(fs->decl)`, null for every
            // imported member, and fall through to "no own storage" for
            // anything but a captured positional parameter — so an imported
            // base's plain BODY FIELD slipped past this guard exactly as an
            // imported auto-property did before #3152. MEASURED: a local
            // `class D: ImportedBase, ISerializable, IDeserializable` whose
            // base carries a single `private _v: int := 41;` COMPILED AND RAN
            // (exit 9) while its character-identical single-module twin was
            // correctly rejected. The field really is there — #2456 carries
            // private instance fields across for layout — so this was a
            // classification error, not a missing channel.
            //
            // ⚠ THE WIDENING, EXACTLY. Locally NOTHING changes: the `static`
            // test moves from `ast::FieldDecl::modifiers` to
            // `FieldSymbol::modifiers`, which SymbolCollection sets to that same
            // vector, and every local FieldSymbol has either a `decl` or a
            // `posParam`, so the new "anything else is storage" arm is
            // unreachable locally. The single new admission is a decl-less,
            // non-static, non-positional field — the imported body field this
            // fixes.
            //
            // fix #3221 closes the residual the #3182 commit message named:
            // an imported class's CAPTURED primary-ctor parameter (the §22.10
            // capture enumeration: `posParamBodyRef == true` on the exporter)
            // now crosses the wire as `MetaMember::posParamBodyRef` (the 18th
            // trailing scalar), and the loader's `convertMember` FieldSymbol
            // arm sets it back on the imported symbol — so the
            // `if (fs->posParam) return fs->posParamBodyRef;` arm answers
            // correctly on both sides. Widening the positional arm to "posParam
            // ⟹ storage" was the wrong widening (it would have flipped the
            // answer for imported RECORDS too); the correct widening was the
            // one-bit wire carrier this fix lands.
            auto classHasOwnStorage = [this](const TypeSymbol* cls) -> bool {
                for (const Symbol* m : cls->members) {
                    if (dynamic_cast<const FieldSymbol*>(m)) {
                        if (isInstanceFieldStorageSymbol(m)) return true;
                        continue;
                    }
                    if (isAutoPropertyStorageSymbol(m, ctx_)) return true;
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

            for (const auto& pc : sccPolyCases) {
                const TypeSymbol* pcTs = pc.type;
                if (!pcTs) continue;
                // fix #3140 (family F2): ask the symbol for the registered
                // type's §9.8 surface. This cast was null for an IMPORTED
                // registered type, so the diagnostic below fired naming it and
                // telling the author to "add a graph-shape clause" to a type
                // whose source they may not have — the audit's "MISLEADING
                // diagnostic naming the imported type" row. The verdict is
                // unchanged (an imported class has no surface — see
                // `serializedClassShape`), but the advice is now accurate.
                if (serializedClassShape(pcTs).empty()) {
                    const bool imported = pcTs->decl == nullptr;
                    ctx_.diagnostics().report(
                        Severity::Error, t->declLoc,
                        "class `" + std::string(t->name) + "` polymorphic "
                        "registry: registered type `" + std::string(pcTs->name) +
                        (imported
                             ? "` is IMPORTED, and §9.8 class serialization does "
                               "not yet cross a module boundary (issue #3179) — "
                               "register a locally-declared type, or hand-write "
                               "`Serialize`/`Deserialize`"
                             : "` has no `serialized tree`/`graph`/`cyclic` "
                               "clause and is not serialization-eligible (§9.8); "
                               "add a graph-shape clause to the registered type"));
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
            if (!sccPolyCases.empty()) {
                std::unordered_set<std::string> seenDiscs;
                for (const auto& pc : sccPolyCases) {
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
                    if (sccShape == "tree") {
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
                        // fix #3140 (family F2): the back-edge target's §9.8
                        // surface, from the symbol. Same conversion, same
                        // verdict, same reason as the registry arm above.
                        if (serializedClassShape(beDecl).empty()) {
                            ctx_.diagnostics().report(
                                Severity::Error, t->declLoc,
                                "class `" + std::string(t->name) +
                                "` (`serialized " + std::string(sccShape) +
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
                    // SEAM-ACCESSOR: `fs` is a member of `t`, which checkClassSerializationEligibilityLate
                    // draws only from `ctx_.module()->children`. A null `fd` here is the
                    // ORDINARY local positional-field shape (the comment below), not an
                    // import gap — `fs->decl` of a positional field is the owning TypeDecl,
                    // never a FieldDecl (#3152).
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
                if (!hasAstAutoPropertyDecl(m)) continue;
                auto* ps = dynamic_cast<const PropertySymbol*>(m);
                if (!ps) continue;
                // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t` — never an imported
                // symbol (#3152).
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
            if (sccShape == "cyclic") {
                // Walk constructor bodies looking for `this.SomeMethod()` calls.
                for (const Symbol* m : t->members) {
                    auto* cs = dynamic_cast<const sema::ConstructorSymbol*>(m);
                    if (!cs || cs->isStatic) continue;
                    // SEAM-ACCESSOR: `cs` is a member of the locally-declared `t` — never an imported
                    // symbol (#3152).
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
            if (!sccPolyCases.empty()) { /* skip ctor check for poly base */ }
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
                        // SEAM-ACCESSOR: `fs` is a member of the locally-declared `t`; a null `fd` here is
                        // the ordinary local positional-field shape, handled by the `posParam`
                        // fallback just below, not an import gap (#3152).
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
                    if (!hasAstAutoPropertyDecl(m)) continue;
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
                    // SEAM-ACCESSOR: `cs` is a member of the locally-declared `t` — never an imported
                    // symbol (#3152).
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
                            std::string(sccShape) +
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
                            std::string(sccShape) +
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
           n == "ChannelSender" ||
           // fix #1832 / §22.12 rule 5 / stdlib §6.5 (v0.187): the §6.5
           // cross-domain publication-point pair (SharedEventSource<T>) is
           // the documented form-(i) example — the entire point of the pair
           // is that static reachability replaces transfer as the
           // distribution mechanism, and the synchronized fact is what makes
           // that safe. Adding it here lets the rule-4 warning be silenced
           // on the recommended `static readonly SharedEventSource<T>` shape
           // and lets §22.12 rule 2's form-(i) overwrite-never-disposes
           // store apply uniformly with the rest of the synchronized set.
           n == "SharedEventSource";
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

// §22.12 rule 3's "immutable content" (narrow-fix-forward for the rule 4
// residual's over-broad default; v0.265, #3273 / #3290): primitive value
// types, enums, `string`, and value types of sendable-value category (the
// compiler-known value builtins DateTime, Guid, TimeSpan, Range,
// CancellationToken, …). Rule 3 names this set as "unrestricted" — no
// declaration-site rule-4 warning. A `static readonly int`, `static readonly
// string`, or `static readonly Guid` therefore takes no rule 4 diagnostic;
// the residual paragraph's "default is warning" reading applies only to
// kinds rule 3 does NOT cover. Records and value-like choices are NOT in
// rule 3 (records are explicitly NOT in rule 3's "value types of sendable-
// value category" — that phrase denotes value-typed records' sendability,
// not records themselves; a record's process-global reachability is the
// residual paragraph's concern, and it is the (3d) warning's intended
// target).
static bool isRule3ImmutableContent(const TypeRef* rt) {
    if (!rt) return false;
    // `string` and `string?` reach TypeResolution as Kind::Primitive with
    // primitive == PrimitiveType::String (the convention the parser uses
    // for all the primitive-keyword spellings; named types use Kind::Named
    // even when they have no user decl, e.g. `Channel<T>`). Other
    // primitives (int/long/byte/char/bool/double/...) are also
    // Kind::Primitive, and rule 3 covers them as "value types of
    // sendable-value category" — except that rule 3 names the
    // non-nullable form; `int?` is a borrow-bearing record case
    // (§9.7) and is not in rule 3.
    if (rt->kind == TypeRef::Kind::Primitive) {
        if (rt->primitive == PrimitiveType::String) return true;
        if (!rt->nullable) return true;  // int/long/byte/char/bool/double/...
        return false;
    }
    // Enums reach TypeResolution as Kind::Named with declaration->kind == Enum.
    if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
        rt->declaration->kind == ast::TypeDeclKind::Enum) return true;
    if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
        rt->declaration->isValueBuiltin) return true;
    return false;
}

// §22.12 rule 4 (v0.262, #3272) warning-suppression set — every type
// declaring the **synchronized** or **immutable** fact (stdlib §1). The set
// is named verbatim in §22.12 rule 4's prose, and the four members outside
// rule 2's form (i) (Locked<T>, RwLocked<T>, FrozenArray<T>,
// CancellationTokenSource) are called out there too. Built-in reference
// types reach TypeResolution as Named+null-declaration, keyed on the head
// name. The form-(i) subset (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>,
// ChannelSender<T>, SharedEventSource<T>) is handled by isInSynchronizedSet
// at the rule 4 call site; this predicate adds the rule-4-only members
// (Locked<T>, RwLocked<T>, FrozenArray<T>, CancellationTokenSource).
//
// `Locked<T>` / `RwLocked<T>` carry the **displacement-unbounded** fact
// (their stdlib §6.11 line), and `FrozenArray<T>` /
// `CancellationTokenSource` carry the **immutable** fact (stdlib §4.19,
// §6.1). The four are outside rule 2's form (i) on that ground — rule 2's
// overwrite-never-disposes store semantics would strand a displaced value
// of unbounded payload until process exit, the payload-scaled cost each
// fact line names — but the rule 4 singleton-with-resources obligation is
// satisfied internally (locked, frozen, or shutdown) so the warning is
// not actionable for them. CancellationTokenSource reaches TypeResolution
// as a built-in reference (Named+null-declaration); Locked<T> / RwLocked<T>
// / FrozenArray<T> are user-declared classes in the stdlib conformance
// tests, so they reach this predicate as Named+declaration. The check is
// keyed on the head name in both cases, since the head carries the fact.
static bool isInReadonlySuppressionSet(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named) return false;
    // FrozenArray<T>, CancellationTokenSource, Locked<T>, RwLocked<T>. The
    // form-(i) members (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>,
    // ChannelSender<T>, SharedEventSource<T>) are handled by
    // isInSynchronizedSet at the rule 4 call site and intentionally not
    // duplicated here — sharing that head-name list with form (i)'s would
    // risk one side drifting from the spec text.
    return rt->headName() == "FrozenArray" ||
           rt->headName() == "CancellationTokenSource" ||
           rt->headName() == "Locked" ||
           rt->headName() == "RwLocked";
}

// True iff `rt` is the bare-parameter `T`/`T?` form of a parameter of
// the enclosing TYPE `t`. Mirrors the check checkTypeStaticFieldDomains
// uses inline; duplicated as a file-local helper for the per-instantiation
// pass beside it. A bare-T TypeRef reaches here with declaration=null
// (it's a placeholder) and the head name matching one of `t`'s generic
// parameter names — exactly the headName loop used inline below.
static bool isBareEnclosingTypeParam(const TypeSymbol* t,
                                     const TypeRef* rt) {
    if (!t || !rt || rt->kind != TypeRef::Kind::Named ||
        rt->declaration) return false;
    for (Symbol* gp : t->genericParams) {
        if (gp && gp->name == rt->headName()) return true;
    }
    return false;
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

// §22.12 rule 4 residual (v0.265, #3273 / #3290): a non-owning choice that
// HOLDS borrows (§35.4's holds-borrows property — every borrow-bearing
// choice and any owning choice with a borrow-carrying payload beside its
// owned ones; the spec text "an owning choice that also holds borrows
// takes the next bullet's rejection" lifts this case to the rejection, so
// here `holds-borrows` is read AS `borrow-bearing` — `BorrowBearing`
// classification under §35.4). The owning-classified-choice branch above
// already short-circuits before this one, so a borrow-bearing OWNING
// choice never reaches this predicate; what reaches here is a
// `BorrowBearing`-classified choice (§35.4).
static bool isHoldsBorrowsChoice(const TypeRef* rt,
                                 sema::SemaContext& ctx) {
    if (!isChoiceTypeRef(rt)) return false;
    auto verdict = common::classifyChoiceType(rt, ctx);
    return verdict && *verdict == common::ChoiceClassification::BorrowBearing;
}

// §22.12 rule 4 residual (v0.265, #3273 / #3290): a PLAIN inline array
// (no `owned` slot marker, §15.1.2) whose element type carries borrows.
// The §9.7 borrow-carrying-field predicate classifies an inline-array
// field as borrow-carrying when its element is a reference type; the
// narrower "carries borrows" wording here matches that. Owning-inline-
// arrays (arrayOwned) are handled in their own branch above (default
// warning with disposal-at-exit note) — they are NOT plain and NOT
// borrow-bearing under §9.7's element-resolved reading.
static bool isPlainInlineArrayCarryingBorrows(
    const TypeRef* rt, sema::SemaContext& ctx) {
    if (!rt || rt->kind != TypeRef::Kind::InlineArray) return false;
    if (rt->arrayOwned) return false;  // owning inline arrays handled above
    if (!rt->arrayElement) return false;
    const TypeRef* elem = rt->arrayElement;
    if (elem->kind == TypeRef::Kind::Primitive) return false;  // not reference
    if (elem->kind == TypeRef::Kind::Function) return false;   // runtime-managed
    if (elem->kind == TypeRef::Kind::Unit) return false;
    if (elem->kind == TypeRef::Kind::Error) return false;
    // Named reference-typed element → carries borrows. Catches user classes
    // and interfaces, builtin reference types (Array<T>, Channel<T>, etc.),
    // and (transitively) borrow-bearing records / borrow-bearing choices
    // (the §9.7 transitivity reading).
    if (elem->kind == TypeRef::Kind::Named) {
        if (elem->nullable) {
            // `T?` where T is reference-typed: still reference-typed (null
            // is the absence of a borrow, but the field's TYPE is
            // reference-typed — §9.7's stricter quantifier applies).
            return true;
        }
        if (!elem->declaration) {
            // Builtin reference type: Array<T>, Channel<T>, Mutex, etc.
            return common::isHeapPointerReferenceBuiltin(elem->headName()) ||
                   elem->headName() == "Channel";
        }
        const auto k = elem->declaration->kind;
        if (k == ast::TypeDeclKind::Class ||
            k == ast::TypeDeclKind::Interface) {
            return true;
        }
        if (k == ast::TypeDeclKind::Record) {
            return common::isBorrowBearingRecordOrSpan(elem, ctx);
        }
        if (k == ast::TypeDeclKind::Choice) {
            auto v = common::classifyChoiceType(elem, ctx);
            return v && *v == common::ChoiceClassification::BorrowBearing;
        }
        return false;
    }
    return false;
}

// §22.12 rule 4 residual + §9.7 static-field bullet (v0.265, #3273 /
// #3290): §9.7-deferred-kind test that handles BUILTIN reference types
// (Object, Mutex, Channel<T>, …) as well as user-declared classes. The
// shared `common::isBorrowBearingRecordOrSpan` predicate treats
// `ft->declaration == nullptr` as a generic-parameter case (returns
// false), which is correct for §30.1 / §9.7's strict instantiation-site
// reading but wrong for the §22.12 residual paragraph's reference-type
// quantifier: `record Tok(Src: Object?)` carries a borrow of an Object
// — Object is a class, just a builtin one — and the record is
// borrow-bearing for the §9.7 null-source bullet. This helper makes the
// §9.7 deferred-kind decision WITHOUT going through the shared predicate,
// enumerating the record's storage fields and checking each one against
// the static-readonly reference-typed predicate (which DOES handle
// builtin reference types via the same `isHeapPointerReferenceBuiltin`
// test the codegen side uses). The shared predicate is still consulted
// first in `reportRule4` — it covers span VIEWS that this helper does
// not — so both must be checked; a record whose ONLY reference-typed
// fields are `Object?`, `string`, or function-typed classifies correctly
// here.
static bool isBorrowBearingRecordForStaticReadonly(const TypeRef* rt,
                                                   SemaContext& ctx) {
    if (!rt || rt->kind != TypeRef::Kind::Named || !rt->declaration) {
        return false;
    }
    const TypeSymbol* ts = rt->declaration;
    if (ts->kind != ast::TypeDeclKind::Record) return false;
    for (Symbol* m : ts->members) {
        const auto* fs = dynamic_cast<const FieldSymbol*>(m);
        if (!fs) continue;
        bool isStatic = false;
        for (std::string_view mod : fs->modifiers)
            if (mod == "static") { isStatic = true; break; }
        if (isStatic) continue;
        // fix #3970 (§9.7/§22.12 rule 4, v0.294): read through
        // `storageComponentType` (sema/Symbol.hpp) — the ONE accessor that
        // consults the positional Param, then the body FieldDecl, then the
        // importer's `importedMemberType` side table — instead of the bare
        // `posParam->type` / `fs->decl`-as-`FieldDecl` pair. For an IMPORTED
        // record's positional field, fix #2516 documents `posParam->type` as
        // deliberately null, and `fs->decl` is never an `ast::FieldDecl` for
        // a positional field regardless of import status (#3152); the bare
        // pair above therefore left `ft` null for EVERY field of an imported
        // record, so this predicate answered "not borrow-bearing" for every
        // imported record — silently disabling the §9.7 enforcement gate
        // this predicate feeds (`checkStaticReadonlyNullSource` below).
        const TypeRef* ft = storageComponentType(m, ctx);
        if (!ft) continue;
        if (staticReadonlyFieldIsReferenceTyped(ft, ctx)) return true;
    }
    return false;
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

// fix #2911 + fix #2962 — §17.4 "Degenerate value-type targets" (v0.241).
// See the declaration comment in TypeResolution.hpp for why this is ONE
// function: the spec states both arms in one paragraph under one principle,
// so the compiler states them from one predicate.
//
// Spec §17.4 (v0.241, verbatim): "`as` requires its result type `T?` to exist
// and to be informative. An **inline-array target** (any form — `[N]T`,
// `[N]T?`, and the owning forms, which cannot even be boxed, §8.6) is a
// **compile error naming this rule**: §15.1 admits no nullable inline-array
// form, so the operator's contract is unsatisfiable. The remedy is the `is`
// test, whose §12.7 narrowing admits copying the value out: `if o is [3]int
// then var a: [3]int := o; end;`. The **unit type** is likewise a compile error
// as an `as` target: `()` has exactly one value, so the operation could never
// carry more information than `x is ()` (§12.7), which the diagnostic names as
// the remedy — `()?` remains a well-formed type (§4.7); the rejection is of the
// operator form, not of the type."
//
// NULLABILITY. `Kind::Unit` is rejected whether or not the ref carries `?`.
// §7.10 makes `?` idempotent, so `x as ()?` has the same result type `()?` as
// `x as ()` and carries exactly the same (zero) information — it is the same
// operator form at the same target, differently spelled, and admitting it would
// leave open through one extra character precisely the representation hole the
// ruling closes (#2962: the unit type lowers to LLVM `void`, so there is no
// `{i8, void}` for the result to be). What §17.4 protects is the TYPE `()?` —
// `var u: ()? := null;` and `var v: ()? := ();` compile and answer correctly,
// and nothing here touches them. (`()?` at a SIGNATURE position does not work
// today for reasons that predate and are independent of this rule: a `()?`
// PARAMETER crashes the compiler and a `()?` return type is rejected as "no
// return type" — filed as #3095, no `as` involved.) An inline array has no
// nullable form to distinguish at all (§15.1), so the same question does not
// arise on that arm.
std::string degenerateAsTargetReason(const TypeRef* target) {
    if (!target) return {};
    if (target->kind == TypeRef::Kind::InlineArray) {
        const std::string spelled = describeStaticValueType(target);
        std::string s =
            "`as` requires its result type `T?` to exist and to be informative "
            "(§17.4), and the inline-array target `" + spelled +
            "` has no such result: §15.1 admits no nullable inline-array form, "
            "so `(" + spelled + ")?` — the type §17.4 requires this expression "
            "to have — is not a type";
        if (target->arrayOwned) {
            // §8.6's parenthetical: an owning inline array "cannot even be
            // boxed", so no operand could ever match this target either.
            s += ", and an owning inline array cannot even be boxed (§8.6). "
                 "Use the `is` test (§12.7) if a type test is what is wanted"
                 " (§17.4/§15.1)";
        } else {
            s += ". Use the `is` test instead, whose §12.7 narrowing admits "
                 "copying the value out: `if o is " + spelled + " then var a: " +
                 spelled + " := o; end;` (§17.4/§15.1)";
        }
        return s;
    }
    if (target->kind == TypeRef::Kind::Unit) {
        return "`as` requires its result type `T?` to exist and to be "
               "informative (§17.4), and the unit target `()` has exactly one "
               "value — so `x as ()` could never carry more information than "
               "the type test `x is ()` (§12.7), which is the remedy. The type "
               "`()?` itself remains well-formed (§4.7); it is the operator "
               "form that is rejected (§17.4/§4.7)";
    }
    return {};
}

// ---------------------------------------------------------------------------
// Resolver::checkSendableConstraintsLate  §30.14 / §14.2 / #3235
//
// Replay each §14.2 `where T: sendable` site that was deferred in
// `checkOneConstraint` (the `item.kind == "sendable"` arm — see that
// function's comment for why). At replay time ExprTypecheck's expression
// walk has finished, so `FieldSymbol::posParamBodyRef` (the §22.10
// captured-vs-uncaptured bit for primary-ctor parameters) is set, and
// `isSendableType`'s §30.14 rule-2 storage walk sees a class's captured
// parameters of non-sendable type and reports them. The diagnostic
// shape matches the in-place `report` lambda's output: a §14.2 citation
// plus, when the walk confidently pins a single component, a §3.6
// rule-4 component-path fragment.
//
// The pending list lives on `SemaContext` (not on `Resolver`) so it
// survives across Resolver instances — this method is invoked through
// `bezel::sema::checkSendableConstraintsLate`, which builds a fresh
// Resolver wrapping the same ctx. Reading from
// `ctx_.pendingSendableConstraints()` here is therefore the same list
// the recording `checkOneConstraint` arm appended to.
//
// Idempotent w.r.t. diagnostics: an Error TypeRef is never enqueued
// (the in-place check skips it because an upstream failure is already
// reported), and the same is true here — the loop only re-runs
// `isSendableType` on resolved TypeRefs.
// ---------------------------------------------------------------------------
void Resolver::checkSendableConstraintsLate() {
    auto& pending = ctx_.pendingSendableConstraints();
    if (pending.empty()) return;
    for (const SemaContext::PendingSendableConstraint& p : pending) {
        if (!p.arg) continue;  // defensive — enqueue path always sets this
        if (p.arg->kind == TypeRef::Kind::Error) continue;
        if (!isSendableType(p.arg, ctx_)) {
            // Mirror the in-place `report` lambda's shape (checkOneConstraint):
            // §14.2 citation plus the §3.6 rule-4 component-path fragment
            // when the walk confidently pins a single component. The
            // fragment string is built lazily so its `isSendableType`
            // recursive walk runs against a TypeRef whose `posParamBodyRef`
            // bits are now populated — the same fix #3165 closed on the
            // call-site route.
            const std::string detail =
                sendableFailureComponentPath(p.arg, ctx_);
            std::string msg =
                std::string("type argument `") + p.argName +
                "` does not satisfy the constraint `where " + p.param +
                ": sendable` (§14.2)";
            if (!detail.empty()) msg += detail;
            ctx_.diagnostics().report(Severity::Error, p.loc, msg);
        }
    }
    // Drain: subsequent runs of this pass (theoretically; today there is
    // exactly one — at the tail of `typecheckExpressions()`) must not
    // re-report the same sites. `pending` lives on SemaContext, so the
    // drain is on the context, not the Resolver.
    pending.clear();
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
    // §22.12 rule 4 (v0.181, #1556 / v0.262, #3272 / v0.265, #3273): a static
    // readonly reference field/property draws a declaration-site warning unless
    // its type is in the warning-suppression set — the referenced instance is
    // process-global and reachable from every domain. The form-(i) synchronized
    // set (isInSynchronizedSet) silences the warning for Mutex / Semaphore /
    // Atomic<T> / AtomicInteger<T> / ChannelSender<T> / SharedEventSource<T>;
    // the broader rule-4-only suppression set (isInReadonlySuppressionSet)
    // adds Locked<T> / RwLocked<T> / FrozenArray<T> / CancellationTokenSource,
    // which form (i) does not admit but rule 4 explicitly carves out.
    //
    // v0.265 (#3273) widens the warning to function types and owning-inline-
    // array types (the residual paragraph names them explicitly); everything
    // else outside the suppression set ALSO draws the default warning per
    // the residual paragraph's "Rule 4's declaration-site warning is the
    // default" sentence. The dispatch below therefore:
    //
    //   1. Owning-classified choice (§35.4 rule 1) → compile ERROR at the
    //      declaration site (the rejection bullet of the residual
    //      paragraph). Nothing would dispose it: §22.13 enrols only
    //      `static owned` fields and owning-inline-array-typed statics, and
    //      §28.5 admits the `owned` modifier only on class/interface
    //      fields — a process-lived owned value with no disposal site.
    //
    //   2. Borrow-bearing record / non-owning choice that holds borrows /
    //      plain inline array carrying borrows → §9.7's null-source case
    //      (deferred). No declaration-site diagnostic: the bullet explicitly
    //      says "governed by §9.7's static-field bullet and NOT by this
    //      rule", and §9.7's own check rejects non-null-source values at
    //      the static-readonly initializer site (see
    //      checkStaticReadonlyNullSource below). CancellationToken's
    //      sentinel `None` is exempted by §9.7's `isValueBuiltin` arm of
    //      isBorrowBearingRecordOrSpan, so its declaration-site visit is
    //      silent.
    //
    //   3. Function type, owning-inline-array type, and every other value
    //      kind outside rule 3 (records — including records with a
    //      function-typed field, value-like choices, value builtins, …) →
    //      the default rule 4 warning. The owning-inline-array case notes
    //      disposal-at-exit per §22.13's *Disposal of static `owned`
    //      fields* clause (the type-level `owned` slot marker conveys the
    //      obligation, no field-level `owned` modifier needed).
    //
    // The synchronized-set + readonly-suppression-set short-circuits keep
    // the canonical pre-v0.265 suppression behaviour (Mutex / Semaphore /
    // FrozenArray<T> / CancellationTokenSource / etc.) without dropping
    // diagnostics on types that suppress did NOT cover.
    auto reportRule4 = [&](const TypeRef* rt, std::string_view name,
                           std::string_view what, SourceLocation loc,
                           const ast::Expr* initializer) {
        // (1) Owning-classified choice — compile ERROR. Diagnoses the
        // disposal-site ground the residual paragraph names. Done before
        // the suppression short-circuit so a suppressing choice (none
        // exist today, but the suppression set is a derived list that
        // could grow) does not silently swallow the rejection.
        if (isChoiceTypeRef(rt)) {
            auto verdict = common::classifyChoiceType(rt, ctx_);
            if (verdict && *verdict == common::ChoiceClassification::Owning) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    std::string("static readonly ") + std::string(what) + " `" +
                    std::string(name)
                    + "` has owning-classified choice type `"
                    + staticRefTypeName(rt)
                    + "`; \xC2\xA7""22.12 rule 4's residual paragraph "
                    "rejects owning-classified choices as process-lived owned "
                    "values would have no disposal site (\xC2\xA7""22.12 rule 4 "
                    "residual, v0.265, #3273 / #3290; \xC2\xA7""35.4; "
                    "\xC2\xA7""22.13; \xC2\xA7""28.5)");
                return;
            }
        }

        // (2) Borrow-bearing record / holds-borrows choice / plain
        // inline-array carrying borrows — defer to §9.7's null-source
        // check (handled at the initializer site in
        // checkStaticReadonlyNullSource below). NO declaration-site
        // diagnostic.
        //
        // TWO borrow-bearing predicates are checked, not one: the shared
        // `common::isBorrowBearingRecordOrSpan` (used by §30.1 / §9.7
        // across the sema surface) classifies span VIEWS (Span /
        // ReadOnlySpan / ArraySpan / ReadOnlyArraySpan) as borrow-
        // bearing via a head-name test, but treats
        // `ft->declaration == nullptr` fields as generic-parameter
        // cases and returns false — which is correct for §30.1's
        // instantiation-site reading but wrong for the §22.12
        // residual paragraph's reference-type quantifier on a builtin
        // reference field like `Object?` or a runtime-managed field
        // like `string` or function type. The §22.12-specific helper
        // `isBorrowBearingRecordForStaticReadonly` enumerates the
        // record's storage fields directly against the static-
        // readonly reference-typed predicate (which DOES handle
        // builtin reference types via `isHeapPointerReferenceBuiltin`),
        // so a record whose ONLY reference-typed fields are `Object?`,
        // `string`, or function-typed classifies correctly here. Both
        // predicates must be checked so span-field records defer to
        // §9.7 silently (no (3d) warning) AND a record with only
        // `Object?` / `string` / function-typed fields defers too
        // (instead of firing both (3d) and the §9.7 value-site
        // diagnostic).
        if (common::isBorrowBearingRecordOrSpan(rt, ctx_)) return;
        if (isBorrowBearingRecordForStaticReadonly(rt, ctx_)) return;
        if (isHoldsBorrowsChoice(rt, ctx_)) return;
        if (isPlainInlineArrayCarryingBorrows(rt, ctx_)) return;

        // (3a) Class / interface / builtin-reference rule 4 path — the
        // pre-v0.265 behaviour, with the suppression set still honoured.
        if (isRule4GovernedReference(rt)) {
            if (isInSynchronizedSet(rt) || isInReadonlySuppressionSet(rt))
                return;
            // per §22.12 rule 4 (v0.307); stdlib §6.9 / §6.11 require the
            // "warning" word so the conformance runner's WARNING_RE matches.
            ctx_.diagnostics().report(
                Severity::Warning, loc,
                std::string("static readonly ") + std::string(what) + " `" +
                std::string(name) + "` has class type `" + staticRefTypeName(rt) +
                "` which is reachable from every domain — a warning: "
                "confine its use to one domain or ensure its own internal "
                "synchronization (§22.12 rule 4, #1363)");
            return;
        }

        // (3b) Function type — default rule 4 warning explicitly. The
        // captures of a function value are a heap-allocated mutable cell
        // reachable and writable from every domain through the field
        // (§27.5); the warning does not discriminate capture-free
        // function values from capturing ones (a captured borrow cannot
        // survive into a static per §28.3/§28.9 anyway).
        if (rt->kind == TypeRef::Kind::Function) {
            ctx_.diagnostics().report(
                Severity::Warning, loc,
                std::string("static readonly ") + std::string(what) + " `" +
                std::string(name) + "` has function type `<function>`; a "
                "function value's captures are a heap-allocated mutable "
                "cell reachable from every domain through this field "
                "(§22.12 rule 4 residual, v0.265, #3273 / #3290; §27.5)");
            return;
        }

        // (3c) Owning-inline-array type — default rule 4 warning + note
        // disposal-at-exit (the type-level `owned` slot marker conveys
        // the obligation, so no field-level `owned` modifier is needed).
        if (rt->kind == TypeRef::Kind::InlineArray && rt->arrayOwned) {
            std::string elemType = describeStaticValueType(rt->arrayElement);
            ctx_.diagnostics().report(
                Severity::Warning, loc,
                std::string("static readonly ") + std::string(what) + " `" +
                std::string(name) + "` has owning-inline-array type `[" +
                (rt->arraySizeValue >= 0 ? std::to_string(rt->arraySizeValue)
                                         : std::string("N")) +
                " owned]" + elemType + "` — process-global and reachable "
                "from every domain; confine every slot read to one domain, "
                "or restructure behind a `Locked<T>` registry "
                "(§22.12 rule 4 residual, v0.265, #3273 / #3290; §22.13 "
                "disposal-of-static-`owned`-fields)");
            return;
        }

        // (3d) Every other declared type — default warning. Per
        // §22.12 rule 4's residual paragraph (v0.265, #3273): "Rule 4's
        // declaration-site warning is the default … with three kinds
        // excepted below." The excepted list (§22.12 rule 4 bullets
        // immediately following the residual paragraph) names interface
        // and function types (which still take the default warning per
        // that paragraph's first bullet — they are in (3a)/(3b) above),
        // borrow-bearing record / holds-borrows choice / plain inline
        // array carrying borrows (which are §9.7's static-field bullet,
        // not rule 4 — already covered by the (2) early-returns above),
        // and owning-classified choice (which is the (1) compile error
        // above). The default warning's *target* set, after those
        // excepted kinds are removed, is: every other declared type the
        // residual paragraph names — records (a plain record with no
        // reference-typed field is borrow-free and so NOT in the §9.7
        // excepted list, so it lands here), value-like choices (the
        // borrow-bearing reads from (2) catch the cases that would
        // otherwise misclassify), and the unit type.
        //
        // Rule 3's "immutable content" (primitives, enums, string, value
        // types of sendable-value category) is *not* listed in the
        // excepted kinds — it lives in rule 3 itself, and rule 3 says
        // "static readonly and const of immutable content … are
        // unrestricted". A `static readonly int` / `static readonly
        // string` / `static readonly Guid` therefore takes no rule 4
        // warning: rule 3's "unrestricted" is the load-bearing exemption
        // and overrides the residual paragraph's "default" reading. The
        // isRule3ImmutableContent test below defers to rule 3 before
        // falling into the (3d) default.
        if (isRule3ImmutableContent(rt)) return;

        // Rule 4's warning-suppression set (Mutex, Semaphore, Atomic<T>,
        // AtomicInteger<T>, ChannelSender<T>, SharedEventSource<T>,
        // Locked<T>, RwLocked<T>, FrozenArray<T>, CancellationTokenSource)
        // — the synchronized / immutable-fact types whose process-global
        // reachability is the canonical use of static statics. The set
        // is consulted at every static-readonly kind, not just the
        // (3a) class/interface/Channel arm; a `static readonly Mutex`
        // is silent because Mutex declares the **synchronized** fact,
        // regardless of which arm of the dispatch would otherwise fire.
        if (isInSynchronizedSet(rt) || isInReadonlySuppressionSet(rt))
            return;

        std::string_view kindDesc;
        if (isRecordTypeRef(rt)) kindDesc = "record";
        else if (isChoiceTypeRef(rt)) kindDesc = "choice";
        else if (rt->kind == TypeRef::Kind::Unit) kindDesc = "unit";
        else kindDesc = "value";
        // per §22.12 rule 4 residual (v0.307); stdlib §6.9 / §6.11 require
        // the "warning" word so the conformance runner's WARNING_RE matches.
        ctx_.diagnostics().report(
            Severity::Warning, loc,
            std::string("static readonly ") + std::string(what) + " `" +
            std::string(name) + "` has " + std::string(kindDesc) + " type `"
            + describeStaticValueType(rt) + "` which is reachable from every "
            "domain — a warning: confine its use to one domain or ensure its "
            "own internal synchronization (§22.12 rule 4 residual, v0.265, "
            "#3273 / #3290)");
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
        // parameters (`T`, `T?`) is intercepted by checkTypeStaticFieldDomains
        // BEFORE reaching here: rule 2's closing sentence (#2331, v0.230) for
        // the mutable arm emits the declaration-site error, and rule 3/4
        // (#3272, v0.262) for the readonly arm records a site for the
        // per-instantiation pass at the end of `run()`. Reaching this lambda
        // with a bare-T TypeRef would be a regression of that interception —
        // it cannot happen unless a future refactor drops the early branch.
        // The defensive return keeps the bug class loud (an assert) without
        // changing observable behaviour.
        if (isBareEnclosingTypeParam(t, rt)) return;

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
            // SEAM-ACCESSOR: `fs` is a member of `t`, which forEachTypeAndNested (#3574) draws
            // only from this module's own declaration tree — never an imported
            // symbol (#3152).
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) continue;

            const TypeRef* rt = ctx_.resolvedType(*fd->type);
            if (!rt) continue;

            // §22.12 (v0.262, #3272 / #2331): a static whose declared type
            // is a bare generic parameter of the enclosing TYPE (`T`, `T?`)
            // — classify per instantiation, not at the declaration.
            //
            //   - READONLY (#3272): record the site; the per-instantiation
            //     pass at the end of `run()` applies rule 3 (immutable
            //     content), rule 4 (class-typed warning, suppressed for
            //     the warning-suppression set), or the residual paragraph
            //     per instantiation, with the diagnostic landing at the
            //     instantiation site. No declaration-site verdict.
            //   - MUTABLE (#2331): per v0.230, rule 2's closing sentence
            //     rejects a mutable static of bare-parameter type at the
            //     declaration site, whatever the parameter's constraints
            //     or eventual substitutions. The two arms together make
            //     the bare-parameter statics story land whole.
            //
            // Both arms key on isBareEnclosingTypeParam below; the
            // diagnostic site, severity, and §-citation differ.
            if (isBareEnclosingTypeParam(t, rt)) {
                if (!hasMod(fs, "readonly")) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("mutable static field `") +
                        std::string(fs->name) +
                        "` has type `" + describeStaticValueType(rt) +
                        "`, a bare generic parameter of the enclosing "
                        "type — §22.12 rule 2's closing sentence "
                        "rejects mutable statics at this form, whatever "
                        "the parameter's constraints or eventual "
                        "substitutions (§22.12 rule 2, v0.230, #2331)");
                } else {
                    recordStaticReadonlyBareTSite(rt->headName(), fs->name,
                                                  /*isField=*/true,
                                                  m->declLoc, t);
                }
                continue;
            }

            if (!hasMod(fs, "readonly")) {
                if (isRule2GovernedReference(rt))
                    reportRule2(rt, fs->name, "field", m->declLoc);
                else
                    reportValueStatic(rt, fs->name, "field", m->declLoc,
                                      /*isField=*/true);
            } else {
                reportRule4(rt, fs->name, "field", m->declLoc, fd->initializer.get());
            }
            continue;
        }

        // Check static auto-properties (PropertySymbol with Auto body).
        if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
            if (ps->isIndexer || !hasMod(ps, "static")) continue;
            // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t` — same reasoning as the
            // field arm above (#3152).
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd || pd->isIndexer ||
                pd->bodyKind != ast::PropertyBodyKind::Auto ||
                !pd->type)
                continue;

            const TypeRef* rt = ctx_.resolvedType(*pd->type);
            if (!rt) continue;

            // §22.12 (v0.262, #3272 / #2331): same bare-parameter split as
            // the field branch above. See the field arm's comment for the
            // rule-3/4 vs rule-2-closing-sentence split.
            if (isBareEnclosingTypeParam(t, rt)) {
                if (!hasMod(ps, "readonly")) {
                    ctx_.diagnostics().report(
                        Severity::Error, m->declLoc,
                        std::string("mutable static auto-property `") +
                        std::string(ps->name) +
                        "` has type `" + describeStaticValueType(rt) +
                        "`, a bare generic parameter of the enclosing "
                        "type — §22.12 rule 2's closing sentence "
                        "rejects mutable statics at this form, whatever "
                        "the parameter's constraints or eventual "
                        "substitutions (§22.12 rule 2, v0.230, #2331)");
                } else {
                    recordStaticReadonlyBareTSite(rt->headName(), ps->name,
                                                  /*isField=*/false,
                                                  m->declLoc, t);
                }
                continue;
            }

            if (!hasMod(ps, "readonly")) {
                if (isRule2GovernedReference(rt))
                    reportRule2(rt, ps->name, "property", m->declLoc);
                else
                    reportValueStatic(rt, ps->name, "property", m->declLoc,
                                      /*isField=*/false);
            } else {
                reportRule4(rt, ps->name, "property", m->declLoc, pd->autoInitializer.get());
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

// §22.12 rule 3/4 (v0.262, #3272): record a `static readonly` field or
// auto-property whose declared type is a bare generic parameter of the
// enclosing TYPE (T or T?). The declaration-site check admits the
// declaration (no classification is decidable there — see the check in
// checkTypeStaticFieldDomains for why); the per-instantiation pass
// (checkStaticReadonlyBareTInstantiations below) walks `instantiations_`
// for each site and applies rule 3 / rule 4 / the residual paragraph per
// concrete substitution. The diagnostic lands at the instantiation site
// per §3.6 rule 2.
void Resolver::recordStaticReadonlyBareTSite(std::string_view paramName,
                                             std::string_view memberName,
                                             bool isField,
                                             SourceLocation declLoc,
                                             TypeSymbol* enclosingType) {
    staticReadonlyBareTSites_.push_back(
        {paramName, memberName, isField, declLoc, enclosingType});
}

// §22.12 rule 4 residual + §9.7 static-field bullet (v0.265, #3273 /
// #3290): the value-site pass. Walks every `static readonly` field /
// auto-property whose declared type is one of §9.7's three deferred kinds
// — borrow-bearing record, non-owning choice that holds borrows, plain
// inline array carrying borrows — and verifies the initializer's value
// is the §9.7 null-source form: every reference-typed field (and every
// reference-typed slot of every inline-array field) is `null`. The check
// is a static analysis on the initializer expression: it accepts `null`,
// `default(...)`, and `new Tok(null, …)` with all-null reference-typed
// arguments; a non-trivial initializer (a method call result, a captured
// value, a function reference, etc.) is left alone because the static
// analysis cannot statically disprove null-source for it.
//
// The diagnostic lands at the initializer site (the `Expr`'s source
// location) so a multi-line constructor points the user at the offending
// `null` reference or the offending call argument, not at the field
// declaration that the §22.12 declaration-site check already visited.
//
// §9.7's stricter quantifier (every REFERENCE-TYPED field must be null,
// not merely every borrow-carrying one) is what catches the v0.266
// #3291 ruling: `record Tok(Src: Object?, Name: string)` initialised
// `new Tok(null, "missing")` is REJECTED here because `Name` is
// reference-typed (`string`) and "missing" is not null — even though
// `Name` is NOT borrow-carrying (a `string` is runtime-managed) and a
// borrow-only reading would silently accept.
//
// CancellationToken's value-builtin flag (SemaContext.hpp's
// `isValueBuiltinByName`) makes `isBorrowBearingRecordOrSpan` return
// false for it, so the §30.8 sentinel `CancellationToken.None` and the
// every-default `token := CancellationToken.None` parameter form pass
// through this pass untouched.

// True iff `rt` is a reference-typed field per §9.7's stricter quantifier
// (every reference-typed field, not just every borrow-carrying one). This
// is a SHALLOW check at one field — class/interface (nullable or not),
// an inline array of reference-typed elements, an array of reference-typed
// elements, the runtime-managed-reference-type cases (`string` and
// function types are reference-typed but runtime-managed; §9.7 includes
// them in the stricter quantifier because a null-source value references
// nothing — runtime-managed or not).
bool staticReadonlyFieldIsReferenceTyped(const TypeRef* rt,
                                        sema::SemaContext& ctx) {
    if (!rt) return false;
    switch (rt->kind) {
        case TypeRef::Kind::Primitive:
            // §3.1 primitive types: only `string` and `Object` are
            // reference-typed (the rest are value-typed). Object is a
            // class, so it is reference-typed; §9.7's stricter quantifier
            // includes both.
            return rt->primitive == PrimitiveType::String ||
                   rt->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;  // runtime-managed, but reference-typed
        case TypeRef::Kind::InlineArray:
            // An inline-array field is reference-typed only when its
            // ELEMENT type is reference-typed (an inline array of value
            // types stores them inline, no heap reference).
            if (!rt->arrayElement) return false;
            return staticReadonlyFieldIsReferenceTyped(rt->arrayElement, ctx);
        case TypeRef::Kind::Named:
            if (!rt->declaration) {
                // Builtin reference type — class-shaped (the `string`
                // case is the PrimitiveType::String arm above). All
                // builtin reference types are reference-typed: Mutex,
                // Channel<T>, Array<T>, …
                return common::isHeapPointerReferenceBuiltin(rt->headName()) ||
                       rt->headName() == "Channel" ||
                       rt->headName() == "Task" ||
                       rt->headName() == "CancellationTokenSource";
            }
            // User-declared: class or interface is reference-typed;
            // record / choice / enum are value-typed for §9.7's purpose
            // (records and choices of value types are values, §3.1).
            {
                const auto k = rt->declaration->kind;
                if (k == ast::TypeDeclKind::Class) return true;
                if (k == ast::TypeDeclKind::Interface) return true;
                // Value-builtin records (CancellationToken) ARE
                // reference-typed at the field level per §9.7's stricter
                // reading (they reference nothing when null-source).
                return rt->declaration->isValueBuiltin &&
                       (rt->headName() == "CancellationToken");
            }
        case TypeRef::Kind::Unit:
            return false;
        case TypeRef::Kind::Error:
            // Unresolved: cannot decide; conservative reading would be
            // `true`, but every Error kind was already reported elsewhere
            // and the resolver will likely fail before this check
            // matters; reporting again would cascade. The §9.7 check
            // requires a concrete verdict — return false so this field
            // is treated as value-typed (the lenient read).
            return false;
    }
    return false;
}

// True iff the initializer expression `e` is statically the `null` literal.
// Auto-property default values (`new Tok(...)` of an all-null record) reach
// here as `NullLiteralExpr`; named-argument null sources too.
bool isStaticallyNullExpr(const ast::Expr* e) {
    return e && dynamic_cast<const ast::NullLiteralExpr*>(e) != nullptr;
}

// True iff the initializer expression is `default(...)` of the
// declared type — `default(T)` for reference-typed T is the null
// watermark, which is the §9.7 null-source form.
bool isStaticallyDefaultExpr(const ast::Expr* e) {
    if (!e) return false;
    return dynamic_cast<const ast::DefaultExpr*>(e) != nullptr;
}

// fix #3973 (§9.7, v0.295): a positional slot (record field / choice
// payload) whose OWN resolved type `pft` is not directly reference-typed
// (`staticReadonlyFieldIsReferenceTyped(pft, ctx)` is false) may still
// carry a nested borrow. Two shapes reach here:
//
//   (a) `pft` is a Named type with a REAL TypeSymbol declaration of kind
//       Record or Choice — an ordinary user-declared (or imported) nested
//       record/choice. Recurse into its own `new T(...)` initializer via
//       `checkStaticReadonlyRecordInit` (mutual recursion — forward-
//       declared above).
//
//   (b) `pft` is one of the four builtin span views (`Span<T>`,
//       `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>` — stdlib
//       §4.14/§4.15). MEASURED: despite being ordinary `record`
//       declarations in the stdlib's own prose, these are represented
//       internally as declaration-LESS Named TypeRefs (`BuiltinRegistry.cpp`'s
//       `kNonHeapReservedBuiltins` / `kBuiltinCtorParamNames` — a compiler-
//       intrinsic `{ptr, i32, i32}` shape with no `TypeSymbol`, the exact
//       same representation `common::isBorrowBearingRecordOrSpan` documents
//       and recognizes by NAME rather than by walking fields), so shape (a)'s
//       `recordPositionalFieldSymbols`/`choiceCasesOf` recursion cannot
//       descend into one — there is no TypeSymbol to enumerate fields from.
//       Handled by name instead: each of the four heads' own FIRST
//       constructor argument (`Buffer` for `Span`/`ReadOnlySpan`, `Source`
//       for `ArraySpan`/`ReadOnlyArraySpan` — `kBuiltinCtorParamNames`'s
//       rows for these four heads, all four "lowered by POSITION", so a
//       reorder via named arguments is independently rejected and index 0
//       is always the backing reference) is the live borrow of its backing
//       `Array`/`OwningBuffer`; the trailing `Start`/`Length` arguments are
//       plain ints, irrelevant to §9.7.
//
// Returns true iff a diagnostic was reported.
//
// fix #4164 (§9.7, v0.295): `incoming` is `pft`'s OWN substitution
// environment — the caller already resolved `pft` via
// `common::resolveRecordFieldSubst(rawPft, callerFieldEnv)`, whose returned
// `Entry::env` is precisely the environment `pft`'s own bare-parameter
// mentions resolve through (unchanged to `callerFieldEnv` itself when `pft`
// is a COMPOUND type such as `InnerCG<T>`, per that function's contract).
// Threading it into the nested frame's own `seedRecordSubstEnv` call below
// (instead of the previous hardcoded `nullptr`) is the "one composition
// hop" `common::RecordSubstEnv::Entry::env` exists for — mirrors
// `common::isBorrowBearingRecordCommon`'s own walk, the ORIGINAL consumer of
// `RecordSubstEnv`, which already threads its `env` parameter the same way
// when recursing into a Record-kind field (`recordFieldTypeIsBorrowCarryingCommon`,
// src/common/OwnershipFacts.cpp). Before this fix `incoming` did not exist
// here at all and every nested frame seeded from `nullptr`, so a compound
// field mentioning the ENCLOSING generic's own bare parameter (`I:
// InnerCG<T>` as a field of `OuterCG<T>`) recursed with `T` still
// unresolved, and the leaf field's own bare mention had nothing left to
// substitute it with (issue #4164's exact diagnosis).
bool checkNestedBorrowBearingSlot(
    SemaContext& ctx, const TypeRef* pft, const common::RecordSubstEnv* incoming,
    const ast::Expr* argExpr, SourceLocation loc, std::string_view what,
    std::string_view name) {
    if (!pft || pft->kind != TypeRef::Kind::Named) return false;
    if (pft->declaration) {
        if (pft->declaration->kind != ast::TypeDeclKind::Record &&
            pft->declaration->kind != ast::TypeDeclKind::Choice)
            return false;
        if (auto* nestedNew = dynamic_cast<const ast::NewExpr*>(argExpr))
            return checkStaticReadonlyRecordInit(ctx, *nestedNew, pft,
                                                 incoming, loc, what, name);
        // fix #4134 (§9.7, v0.295): a nested slot whose OWN type is a
        // Choice is never initialized with a `new` expression (choices
        // have no constructors, §35.1) — its real spelling is a case CALL
        // (`Type.Case(...)`). Recognize that spelling here too, exactly as
        // `checkStaticReadonlyNullSource`'s own top-level gate now does,
        // so a record/choice field of choice type is not silently
        // skipped just because it is one level down.
        if (pft->declaration->kind == ast::TypeDeclKind::Choice) {
            if (auto* nestedCall = dynamic_cast<const ast::CallExpr*>(argExpr)) {
                Symbol* calleeSym = ctx.exprSymbol(*nestedCall->callee);
                if (auto* ccs = dynamic_cast<ChoiceCaseSymbol*>(calleeSym))
                    return checkStaticReadonlyChoiceCaseInit(ctx, *nestedCall,
                                                             ccs, pft,
                                                             incoming, loc,
                                                             what, name);
            }
        }
        return false;
    }
    // Declaration-less: only the four builtin span views are handled; every
    // other declaration-less Named type (Array<T>, Channel<T>, a bare
    // still-uninstantiated generic parameter, …) is not itself a nested
    // record/choice and is left alone (unreachable via this predicate's
    // caller anyway, since `staticReadonlyFieldIsReferenceTyped` already
    // classifies every OTHER declaration-less builtin as reference-typed —
    // see its own `Kind::Named, !rt->declaration` arm above — so control
    // only reaches here when that check already answered false).
    const std::string_view h = pft->headName();
    std::string_view backingFieldName;
    if (h == "Span" || h == "ReadOnlySpan") backingFieldName = "Buffer";
    else if (h == "ArraySpan" || h == "ReadOnlyArraySpan") backingFieldName = "Source";
    else return false;
    auto* nestedNew = dynamic_cast<const ast::NewExpr*>(argExpr);
    if (!nestedNew || nestedNew->args.empty()) return false;
    if (isStaticallyNullExpr(nestedNew->args[0].value.get())) return false;
    ctx.diagnostics().report(
        Severity::Error, loc,
        std::string("static readonly ") + std::string(what) + " `" +
        std::string(name) + "` has record value with non-null "
        "reference-typed field `" + std::string(backingFieldName) +
        "`; §9.7 requires the null-source form (every reference-typed "
        "component must be null) (§22.12 rule 4 residual + \xC2\xA7""9.7, "
        "v0.265, #3273 / #3290)");
    return true;
}

// Walk the positional arguments of a `new Tok(...)` expression against the
// record/choice's field/payload shape and verify every reference-typed
// component is null. Named-argument or positional-only mismatches are
// reported at the call site. `loc` is the `NewExpr` site; the diagnostic
// uses it as a fallback when argument resolution is impossible. Returns
// true iff a diagnostic was reported — callers (including this function's
// own recursive calls) use this to stop after the first, preserving the
// "one diagnostic per site" contract.
//
// fix #3973 (§9.7, v0.295): this walk was originally SHALLOW / one level —
// a positional field/payload slot whose OWN resolved type is itself a
// record or choice was always treated as value-typed (records and choices
// of value types are values) and skipped outright, with no look at what
// that nested record/choice itself contains. So a slot that is ITSELF a
// borrow-bearing record — the canonical case being any of the span views
// (`Span<T>` / `ReadOnlySpan<T>` / `ArraySpan<T>` / `ReadOnlyArraySpan<T>`,
// ordinary user-level records per stdlib §4.14/§4.15) nested as a field,
// e.g. `SpanHolder(V: ReadOnlyArraySpan<int>)` — got zero §9.7 enforcement
// no matter what its own constructor argument held. Fixed by recursing:
// when a slot's type is itself Named with a Record or Choice declaration,
// and the slot's own initializer expression is statically a `new T(...)`,
// re-run this same per-slot walk against THAT nested `NewExpr`'s own
// arguments. The recursion is structurally bounded — no separate depth
// limit is needed — because neither a record nor a choice may transitively
// contain itself by value (§9 / #1272 for records; §35.1 / #2208 for
// choices; both enforced at declaration time by
// `Resolver::recordValueContainsSelf`, ~line 1716 above). A nested slot's
// initializer that is NOT statically a `new T(...)` (a variable, a call
// result, `null`, `default(...)`) is left alone, matching this function's
// own top-level caller's policy just above (the static analysis cannot
// disprove null-source for a non-trivial initializer).
bool checkStaticReadonlyRecordInit(
    SemaContext& ctx, const ast::NewExpr& nx, const TypeRef* declTy,
    const common::RecordSubstEnv* incoming, SourceLocation loc,
    std::string_view what, std::string_view name) {
    if (!declTy || declTy->kind != TypeRef::Kind::Named || !declTy->declaration)
        return false;
    const TypeSymbol* ts = declTy->declaration;
    // fix #3970 (§9.7/§22.12 rule 4, v0.294): this used to cast
    // `declTy->declaration->decl` to an `ast::TypeDecl` and walk ITS
    // `positionalFields`/`choiceCases` — `TypeSymbol::decl` is NEVER
    // assigned for an imported type (Symbol.hpp; #3145's OwnershipFacts.cpp
    // comment states the same fact for the field-walk sibling), so this
    // whole function silently "accepted conservatively" for every static
    // readonly field/property of an IMPORTED record or choice type, no
    // matter what its initializer held — a second, independent bypass of
    // this same §9.7 enforcement pass, layered UNDER the
    // `isBorrowBearingRecordForStaticReadonly` gate above (fixing that gate
    // alone still reached this early return and produced no diagnostic,
    // measured). Both branches below now read the SYMBOL side instead,
    // which is populated identically on both sides of an import boundary:
    // `recordPositionalFieldSymbols`/`storageComponentType` for a record
    // (the same accessors `isBorrowBearingRecordForStaticReadonly` now
    // uses), and `choiceCasesOf`/`ChoiceCaseSymbol::caseDecl` for a choice
    // — the latter's payload `ast::Param`s are, per fix #3144's
    // `ModuleLoader::convertChoiceCases`, reconstructed with a REAL
    // (non-null) `type` node unlike a record's synthetic `posParam`, so
    // `ctx.resolvedType(*pf.type)` already answers correctly for an
    // imported case and needed no change beyond removing the `td` gate
    // that stood in front of it.
    if (ts->kind == ast::TypeDeclKind::Record) {
        const std::vector<const Symbol*> fields =
            recordPositionalFieldSymbols(ts);
        std::size_t arity = fields.size();
        if (nx.args.size() != arity) return false;  // mismatch — typecheck reports
        // fix #4133 (§9.7, v0.295): `storageComponentType` reads a field's
        // type off the DECLARATION side, so a generic record's field
        // declared as a bare type parameter (`V: T` in `record
        // Box<T>(V: T)`) resolves to the unsubstituted `T` regardless of
        // what THIS instantiation (`declTy`) actually binds `T` to —
        // `staticReadonlyFieldIsReferenceTyped` can then never classify it
        // as reference-typed, no matter how reference-heavy the concrete
        // instantiation is. Seed a `common::RecordSubstEnv` from `ts`'s own
        // generic parameters against `declTy`'s concrete `genericArgs` —
        // the SAME machinery fix #4002 uses for the identically-shaped
        // `where T: new()` gap (this file's `where T: new()` arm, ~line
        // 7543) — and substitute each field's type through it before
        // classifying. `seedRecordSubstEnv`/`resolveRecordFieldSubst` are
        // no-ops (empty map / unchanged `ft`) for a non-generic record or
        // an unresolved/uninstantiated use, so this is exactly the prior
        // behaviour whenever there is nothing to substitute.
        // fix #4164 (§9.7, v0.295): seed THIS frame's env through `incoming`
        // (the enclosing frame's env, null at the top of the recursion)
        // rather than a hardcoded `nullptr` — `declTy`'s own `genericArgs`
        // may themselves be bare mentions of an ENCLOSING generic's
        // parameter (`declTy == InnerCG<T>` when `T` is `OuterCG`'s own
        // parameter), and `seedRecordSubstEnv` already resolves each
        // argument through its `incoming` param before storing it (see its
        // header contract's "one composition hop") — the same composition
        // `common::isBorrowBearingRecordCommon` already performs for the
        // §9.7 dangling-borrow check.
        common::RecordSubstEnv env;
        common::seedRecordSubstEnv(*ts, *declTy, incoming, env);
        const common::RecordSubstEnv* fieldEnv =
            env.map.empty() ? nullptr : &env;
        for (std::size_t i = 0; i < arity; ++i) {
            const TypeRef* rawPft = storageComponentType(fields[i], ctx);
            if (!rawPft) continue;
            const common::RecordSubstEnv::Entry sub =
                common::resolveRecordFieldSubst(rawPft, fieldEnv);
            const TypeRef* pft = sub.ty;
            if (!pft) continue;
            const ast::Expr* argExpr = nx.args[i].value.get();
            if (staticReadonlyFieldIsReferenceTyped(pft, ctx)) {
                if (isStaticallyNullExpr(argExpr)) continue;
                // Non-null reference-typed field: reject.
                std::string pname = std::string(fields[i]->name);
                ctx.diagnostics().report(
                    Severity::Error, loc,
                    std::string("static readonly ") + std::string(what) + " `"
                    + std::string(name) + "` has record value with non-null "
                    "reference-typed field `" + pname + "`; §9.7 requires the "
                    "null-source form (every reference-typed component must be "
                    "null) (§22.12 rule 4 residual + \xC2\xA7""9.7, v0.265, "
                    "#3273 / #3290)");
                return true;  // one diagnostic per site
            }
            // fix #3973: `pft` is not itself reference-typed, but it may be
            // a NESTED record/choice (or one of the four declaration-less
            // builtin span views) that carries a reference-typed component
            // further down. fix #4164: thread `sub.env` — `pft`'s own
            // substitution environment — down as the nested frame's
            // `incoming` (see `checkNestedBorrowBearingSlot`'s header).
            if (checkNestedBorrowBearingSlot(ctx, pft, sub.env, argExpr, loc,
                                             what, name))
                return true;
        }
        return false;
    }
    if (ts->kind == ast::TypeDeclKind::Choice) {
        // For a choice, only the case being constructed is null-source at
        // the value site; this analyser accepts `new MyChoice(...)` of any
        // case whose every payload-typed argument is null. Without case
        // resolution we can only conservatively accept (the constructor's
        // shape must be inspected case by case, which a value-site check
        // before typecheck could not do).
        // fix #4133 (§9.7, v0.295): same generic-substitution gap as the
        // record arm above, applied to a choice's payload fields — see that
        // arm's comment for the full rationale.
        // fix #4164 (§9.7, v0.295): thread `incoming` here too — see the
        // record arm's comment just above for the full rationale.
        common::RecordSubstEnv env;
        common::seedRecordSubstEnv(*ts, *declTy, incoming, env);
        const common::RecordSubstEnv* fieldEnv =
            env.map.empty() ? nullptr : &env;
        for (const ChoiceCaseSymbol* cs : choiceCasesOf(ts)) {
            if (!cs->caseDecl) continue;
            std::size_t arity = cs->caseDecl->payload.size();
            if (nx.args.size() != arity) continue;
            for (std::size_t i = 0; i < arity; ++i) {
                const ast::Param& pf = cs->caseDecl->payload[i];
                if (!pf.type) continue;
                const TypeRef* rawPft = ctx.resolvedType(*pf.type);
                if (!rawPft) continue;
                const common::RecordSubstEnv::Entry sub =
                    common::resolveRecordFieldSubst(rawPft, fieldEnv);
                const TypeRef* pft = sub.ty;
                if (!pft) continue;
                const ast::Expr* argExpr = nx.args[i].value.get();
                if (staticReadonlyFieldIsReferenceTyped(pft, ctx)) {
                    if (isStaticallyNullExpr(argExpr)) continue;
                    std::string pname = std::string(pf.name);
                    ctx.diagnostics().report(
                        Severity::Error, loc,
                        std::string("static readonly ") + std::string(what) + " `"
                        + std::string(name) + "` has choice value with non-null "
                        "reference-typed payload `" + pname + "`; \xC2\xA7""9.7 "
                        "requires the null-source form (every reference-typed "
                        "component must be null) (\xC2\xA7""22.12 rule 4 residual "
                        "+ \xC2\xA7""9.7, v0.265, #3273 / #3290)");
                    return true;  // one diagnostic per site
                }
                // fix #3973: same nested-slot handling as the record arm
                // above (nested record/choice, or a declaration-less
                // builtin span view). fix #4164: thread `sub.env` down.
                if (checkNestedBorrowBearingSlot(ctx, pft, sub.env, argExpr,
                                                 loc, what, name))
                    return true;
            }
            return false;  // first matching case wins
        }
        return false;
    }
    return false;
}

// fix #4134 (§9.7, v0.295): the choice-value counterpart of
// `checkStaticReadonlyRecordInit`, keyed to a SPECIFIC, already-resolved
// `ChoiceCaseSymbol` rather than a `new`-expression's bare argument list.
//
// `checkStaticReadonlyRecordInit`'s own Choice arm (just above) exists to
// serve a `new Tok(...)` NewExpr, which carries no case identity at all —
// it has to iterate every declared case and guess by matching arity. That
// guess is unsound in general (two cases of the same arity are
// indistinguishable to it) and, per the spec's own field-initializer
// admission list (#5130-area prose: "it is not a `new` expression … a
// choice's construction *is* case selection", §35.1), a `new`-expression
// is not how a choice value is ever legitimately constructed in the first
// place — the real spelling is a qualified case CALL, `Type.Case(args)`,
// which resolves through ordinary call typechecking to a `ChoiceCaseSymbol`
// (`ExprTypecheck_Calls.cpp`'s `ChoiceCaseSymbol` stamp). Every caller of
// this function already has that resolved symbol in hand, so there is no
// arity-guessing here: `cs` names the EXACT case, and `ce.args` are its
// EXACT arguments — the same per-payload null-source walk as the Choice
// arm above, applied without ambiguity. Returns true iff a diagnostic was
// reported.
bool checkStaticReadonlyChoiceCaseInit(
    SemaContext& ctx, const ast::CallExpr& ce, const ChoiceCaseSymbol* cs,
    const TypeRef* choiceTy, const common::RecordSubstEnv* incoming,
    SourceLocation loc, std::string_view what, std::string_view name) {
    if (!cs || !cs->caseDecl) return false;
    std::size_t arity = cs->caseDecl->payload.size();
    if (ce.args.size() != arity) return false;  // mismatch — typecheck reports
    // fix #4133 (§9.7, v0.295): identical generic-substitution gap as
    // checkStaticReadonlyRecordInit's record/choice arms — a case payload
    // declared as a bare type parameter never saw what a concrete
    // instantiation (`choiceTy`) actually binds it to. `cs->parent` is the
    // choice's own declaring TypeSymbol (its `genericParams`); `choiceTy`
    // carries the instantiation's concrete `genericArgs`.
    // fix #4164 (§9.7, v0.295): seed through `incoming` (the enclosing
    // frame's env, null at the top of the recursion) rather than a
    // hardcoded `nullptr` — same composition rationale as
    // checkStaticReadonlyRecordInit's record arm.
    common::RecordSubstEnv env;
    if (auto* choiceTs = dynamic_cast<TypeSymbol*>(cs->parent); choiceTs && choiceTy)
        common::seedRecordSubstEnv(*choiceTs, *choiceTy, incoming, env);
    const common::RecordSubstEnv* fieldEnv = env.map.empty() ? nullptr : &env;
    for (std::size_t i = 0; i < arity; ++i) {
        const ast::Param& pf = cs->caseDecl->payload[i];
        if (!pf.type) continue;
        const TypeRef* rawPft = ctx.resolvedType(*pf.type);
        if (!rawPft) continue;
        const common::RecordSubstEnv::Entry sub =
            common::resolveRecordFieldSubst(rawPft, fieldEnv);
        const TypeRef* pft = sub.ty;
        if (!pft) continue;
        const ast::Expr* argExpr = ce.args[i].value.get();
        if (!argExpr) continue;  // discard argument: no source to inspect
        if (staticReadonlyFieldIsReferenceTyped(pft, ctx)) {
            if (isStaticallyNullExpr(argExpr)) continue;
            std::string pname = std::string(pf.name);
            ctx.diagnostics().report(
                Severity::Error, loc,
                std::string("static readonly ") + std::string(what) + " `"
                + std::string(name) + "` has choice value with non-null "
                "reference-typed payload `" + pname + "`; \xC2\xA7""9.7 "
                "requires the null-source form (every reference-typed "
                "component must be null) (\xC2\xA7""22.12 rule 4 residual "
                "+ \xC2\xA7""9.7, v0.265, #3273 / #3290)");
            return true;  // one diagnostic per site
        }
        // fix #4134: same nested-slot handling as checkStaticReadonlyRecordInit's
        // record/choice arms (nested record/choice, or a declaration-less
        // builtin span view). fix #4164: thread `sub.env` down.
        if (checkNestedBorrowBearingSlot(ctx, pft, sub.env, argExpr, loc,
                                         what, name))
            return true;
    }
    return false;
}

void Resolver::checkStaticReadonlyNullSource() {
    // Walk every type in the program. For each type's static readonly
    // field / auto-property whose declared type is one of the three §9.7-
    // deferred kinds, examine the initializer and emit the null-source
    // diagnostic when a non-null reference-typed component is statically
    // observable.
    for (const auto& sym : ctx_.symbols()) {
        auto* t = dynamic_cast<TypeSymbol*>(sym.get());
        if (!t) continue;
        for (Symbol* m : t->members) {
            const TypeRef* rt = nullptr;
            std::string_view memberName;
            std::string_view what;
            SourceLocation loc;
            const ast::Expr* init = nullptr;
            if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                if (!hasMod(fs, "static") || !hasMod(fs, "readonly")) continue;
                // For body-record/class fields, `fs->decl` is a FieldDecl;
                // for POSITIONAL RECORD fields, `fs->decl` is the enclosing
                // TypeDecl and the storage type rides on `fs->posParam->type`.
                // Both shapes need the resolved type and the initializer; the
                // ast::FieldDecl path also provides the initializer (`fd-
                // >initializer`); the positional path needs the parent
                // record's initializer — which lives on the record decl's
                // primary-ctor args, not on the field symbol. Walk both
                // shapes; the §9.7 null-source check fires from the TYPE's
                // initializer context.
                // SEAM-ACCESSOR: AST arm of the both-sources pattern; needed
                // for the LOCAL body's fd->type + fd->initializer (the
                // importedMemberType() side-table carries the type but not
                // the initializer for an imported body field, and the
                // positional path covers only records' primary-ctor site).
                auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
                if (fd && fd->type) {
                    rt = ctx_.resolvedType(*fd->type);
                    init = fd->initializer.get();
                } else if (fs->posParam && fs->posParam->type) {
                    rt = ctx_.resolvedType(*fs->posParam->type);
                    init = nullptr;  // positional field: no field-local
                                       // initializer; the parent record's
                                       // primary-ctor args are the static
                                       // site — handled below for records.
                } else {
                    continue;
                }
                memberName = fs->name;
                what = "field";
                loc = fs->declLoc;
            } else if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
                if (ps->isIndexer || !hasMod(ps, "static") ||
                    !hasMod(ps, "readonly"))
                    continue;
                // SEAM-ACCESSOR: AST arm of the both-sources pattern; needed
                // for the LOCAL auto-property's pd->type + pd->autoInitializer
                // (the importedMemberType() side-table carries the type but
                // not the autoInitializer for an imported auto-property).
                auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                if (!pd || pd->isIndexer ||
                    pd->bodyKind != ast::PropertyBodyKind::Auto ||
                    !pd->type)
                    continue;
                rt = ctx_.resolvedType(*pd->type);
                memberName = ps->name;
                what = "property";
                init = pd->autoInitializer.get();
                loc = ps->declLoc;
            } else {
                continue;
            }
            if (!rt) continue;
            // fix #4679 / §9.7 / §4.14 / §4.15 (v0.307, the §9.7 static-
            // field bullet's third §4.14-family reject from the issue's
            // enumeration — `l4_14_0006` "Span<> stored in a static
            // field"): the four builtin span views (Span<T>,
            // ReadOnlySpan<T>, ArraySpan<T>, ReadOnlyArraySpan<T>) declare
            // a NON-NULLABLE Buffer/Source field — so the §9.7 null-source
            // escape clause (every reference-typed component is null) is
            // unreachable for any Span<T>-typed static readonly initialiser,
            // because there is no value of the type whose Buffer/Source is
            // null. The general-purpose checks below — which recognise
            // `new Tok(...)` and `Tok.Case(...)` forms — fall through to
            // "any other initializer (call result, captured variable,
            // method reference, etc.) … accept" for the common
            // `_buf.AsSpan()` spelling: the analysis cannot prove
            // non-null-source against a fresh borrow, but for a span view
            // the spell is FORBIDDEN by construction, not just
            // possibly-non-null. The Span family is named here (rather
            // than buried in the general path) because the diagnostic
            // word "static field" reads as the §9.7 static-field bullet
            // the rule enforces, and the ruling needs the four span-view
            // names to be unmistakable to the reader. No further pass on
            // `init` is attempted: every initializer of a Span<T>-typed
            // static readonly is rejected, including `default(...)`
            // (which a Span<T> lacks, by §17.11) and a literal-null
            // (which would fail span construction).
            //
            // Mirror OwnershipFacts.cpp's anonymous-namespace
            // `borrowSpanNameHead` inline (that helper is file-local, so
            // a header export would drag `common/` into `sema/`'s public
            // surface for one private three-fallback name read). Use the
            // project-wide `headName()` accessor (TypeRef.hpp:80) instead
            // of `declaration->name` directly: `rt->declaration` is null
            // for §3.4 synthetic value-type builtins (Span<T>,
            // ReadOnlySpan<T>, ArraySpan<T>, ReadOnlyArraySpan<T> are in
            // that set, #1463) so the direct read skips every builtin
            // span view and the rule silently accepts. `headName()` falls
            // back to `aliasResolvedHead` / use-site head / `importedName`
            // in that order and reliably returns "Span" / "ReadOnlySpan"
            // / "ArraySpan" / "ReadOnlyArraySpan" for the four builtin
            // span views.
            {
                std::string_view sn = rt->headName();
                if (sn == "Span" || sn == "ReadOnlySpan" ||
                    sn == "ArraySpan" || sn == "ReadOnlyArraySpan") {
                    const std::string specRef =
                        (sn == "ArraySpan" || sn == "ReadOnlyArraySpan")
                            ? "§4.15" : "§4.14";
                    ctx_.diagnostics().report(
                        Severity::Error, loc,
                        std::string("a `") + std::string(sn) +
                        "` value may not be stored in a static "
                        "field (" + specRef + " / §9.7) — a "
                        "span view's backing-storage field is "
                        "non-nullable, so no value of the type "
                        "satisfies the §9.7 null-source escape "
                        "clause; expose the backing storage (an "
                        "`owned OwningBuffer<T>` / `owned Array<T>` "
                        "field) and re-derive the span with "
                        "`AsSpan()` at the use site instead");
                    continue;
                }
            }
            // §9.7-deferred kinds only: borrow-bearing record,
            // non-owning choice that holds borrows, plain inline array
            // carrying borrows. The other §22.12 residual kinds (owning
            // choice, function, owning-inline-array) were rejected or
            // warned at the declaration site; records / choices / plain
            // arrays that are NOT borrow-bearing take the default warning
            // and are not §9.7-deferred.
            //
            // fix #3973 (§9.7, v0.295): unlike `reportRule4`'s
            // declaration-site defer test (above, ~line 11172), which ORs
            // `common::isBorrowBearingRecordOrSpan` with
            // `isBorrowBearingRecordForStaticReadonly`, this gate used to
            // consult only the narrower predicate. Adding the shared
            // predicate here was investigated for fix #3970 (§9.7/§22.12
            // rule 4, v0.294) and found OBSERVATIONALLY INERT at the time
            // against every shape reachable through
            // `checkStaticReadonlyRecordInit` below: that walk's own
            // per-field test (`staticReadonlyFieldIsReferenceTyped`) was a
            // SHALLOW, one-level check that classified every
            // record/choice-typed field as value-typed regardless of what
            // it recursively contained, so a record field that is ITSELF a
            // borrow-bearing record (a span-view field being the measured
            // case: `SpanHolder(V: ReadOnlySpan<int>)`) was never flagged
            // even with `deferred` forced true. `checkStaticReadonlyRecordInit`
            // now recurses into a nested record/choice field's own
            // constructor arguments (fix #3973), so the shared predicate is
            // added here too — WITHOUT it, a field like `SpanHolder` above
            // never reaches that recursive walk at all, because `deferred`
            // stays false and the member is skipped outright (`continue`
            // below) before `checkStaticReadonlyRecordInit` is ever called.
            bool deferred = isBorrowBearingRecordForStaticReadonly(rt, ctx_) ||
                            common::isBorrowBearingRecordOrSpan(rt, ctx_) ||
                            isHoldsBorrowsChoice(rt, ctx_) ||
                            isPlainInlineArrayCarryingBorrows(rt, ctx_);
            if (!deferred) continue;
            // Static readonly auto-property without an explicit initializer
            // (`public static readonly property None: CancellationToken;`) —
            // default value is the type's auto-default. For a record this
            // is the all-fields-default-initialised value, which IS the
            // null-source form for every field whose default is null (and
            // any non-reference-typed field's default is irrelevant). The
            // CancellationToken `None` sentinel hits this branch and is
            // accepted without further analysis — its `isValueBuiltin`
            // flag keeps it out of the borrow-bearing predicate above
            // (defensive: even if it slipped in, the no-initializer form
            // is the all-default value, not a programmer-supplied
            // non-null literal).
            if (!init) continue;
            // Statically null or `default(...)`: trivially null-source.
            if (isStaticallyNullExpr(init)) continue;
            if (isStaticallyDefaultExpr(init)) continue;
            // `new Tok(...)` constructor: walk the positional arguments.
            if (auto* nx = dynamic_cast<const ast::NewExpr*>(init)) {
                if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                    (rt->declaration->kind == ast::TypeDeclKind::Record ||
                     rt->declaration->kind == ast::TypeDeclKind::Choice)) {
                    checkStaticReadonlyRecordInit(ctx_, *nx, rt,
                                                  /*incoming=*/nullptr,
                                                  init->span.begin, what,
                                                  memberName);
                }
                // For an inline-array initializer of a plain array
                // carrying borrows (`[N]C := new C[N]()`), the per-slot
                // null-source check is structurally similar but the array
                // element type is the static known — every element must be
                // null. We do not analyse array-literal expressions here:
                // an inline-array literal `[null, null, …]` would have to
                // match the element count exactly to be statically
                // provable, and the spec's stricter quantifier is the
                // same. Defer to a follow-up; this lane covers the record
                // form the issue explicitly cites, and the negative test
                // in static_readonly_residual_3290_test.cpp's own record
                // case pins the spec's wording.
                continue;
            }
            // fix #4134 (§9.7, v0.295): a CHOICE value's real construction
            // spelling is a case CALL (`Type.Case(args)`), never a `new`
            // expression — the NewExpr-only branch above therefore never
            // reached `checkStaticReadonlyRecordInit`'s Choice arm for any
            // spec-compliant choice-typed static readonly initializer.
            // Recognize the real spelling: the callee resolves to a
            // `ChoiceCaseSymbol` exactly when the call constructs a case of
            // THIS declared choice type (§35.3), the same test
            // `ExprTypecheck_Calls.cpp`'s own call-typing arm uses.
            if (auto* ce = dynamic_cast<const ast::CallExpr*>(init)) {
                if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                    rt->declaration->kind == ast::TypeDeclKind::Choice) {
                    Symbol* calleeSym = ctx_.exprSymbol(*ce->callee);
                    if (auto* ccs = dynamic_cast<ChoiceCaseSymbol*>(calleeSym))
                        checkStaticReadonlyChoiceCaseInit(ctx_, *ce, ccs, rt,
                                                          /*incoming=*/nullptr,
                                                          init->span.begin,
                                                          what, memberName);
                }
                continue;
            }
            // Any other initializer (call result, captured variable,
            // method reference, etc.): the static analysis cannot prove
            // non-null-source. Accept — the §9.7 wording is the rejection
            // is "at that rule", and a non-trivial initializer may
            // resolve to null at runtime.
        }
    }
}

// §14.6 / #3271 (v0.262): the §14.6 per-edge predicates over a substitution
// and an in-scope parameter set. Two functions share one scope of work and
// are intentionally co-located: the closed-type walk (`typeContainsNoInScope`)
// and the bare-parameter check (`isBareInScopeParam`) partition the admitted
// shapes ("either a bare in-scope parameter or a closed type"). A substitution
// that is neither — typically a constructed type (`Pair<T, T>`, `Box<U>`,
// `T?` even when T is bare) containing one or more in-scope parameters
// nested inside it — is the expanding shape §14.6 rejects.
//
// Both walk the TypeRef tree. We do not consult `instantiations_` here; this
// predicate is reusable from any future per-edge check (e.g., an override-
// contributed edge audit) without re-binding to the resolution state.
namespace {

// Is `ref` a BARE reference to a parameter that is in scope at the source
// declaration? "Bare" means the TypeRef has no `declaration` (it resolves to
// no TypeSymbol — it is the syntactic parameter placeholder), is a Named
// shape, has a single-part name, and is not a primitive or builtin (the
// same predicate as #2713's `isBareGenericParamBinding` in
// TypeResolution_Checks.cpp). The name is then looked up in `inScope`.
//
// The nullable form `T?` is NOT a bare in-scope parameter — spec §14.6
// explicitly excludes it ("the nullable form `T?` included"). The call site
// checks `ref->nullable` first.
bool isBareInScopeParam(const TypeRef* ref, const ast::Type* refAst,
                        const std::set<std::string_view>& inScope) {
    if (!ref || !refAst) return false;
    if (ref->nullable) return false;
    if (ref->kind != TypeRef::Kind::Named) return false;
    if (ref->declaration != nullptr) return false;
    if (refAst->kind != ast::TypeKind::Named || !refAst->name ||
        refAst->name->parts.size() != 1)
        return false;
    return inScope.count(refAst->name->parts.front()) > 0;
}

// Does `ref` contain NO reference to a parameter in `inScope` at all? A
// closed type by definition contains no type parameter. The recursive walk
// rejects:
//   * the nullable form `T?` (spec text: "the nullable form `T?` included" —
//     this is the conservative reading: even T being in scope does not save
//     the substitution; the substitution itself must be the bare T, not T?),
//   * any Named type whose generic args recursively contain an in-scope
//     parameter,
//   * any Named type whose own declaration is a generic parameter (declaration
//     == null but the name is in scope — handled as the bare arm, NOT here),
//   * any InlineArray whose element contains an in-scope parameter,
//   * any Function whose parameter or return types contain an in-scope
//     parameter.
// Primitives and unit are closed.
bool typeContainsNoInScope(const TypeRef* ref,
                           const std::set<std::string_view>& inScope) {
    if (!ref) return true;
    if (ref->nullable) return false;
    switch (ref->kind) {
        case TypeRef::Kind::Error:
        case TypeRef::Kind::Unit:
            return true;
        case TypeRef::Kind::Primitive:
            return true;  // int, string, double, … — all closed.
        case TypeRef::Kind::Named:
            if (ref->declaration == nullptr) {
                // Bare parameter reference. The closed arm applies only when
                // the parameter is NOT in scope (so it is unresolvable and
                // would already be a separate error). For the §14.6 check,
                // a bare in-scope parameter is admitted by the OTHER arm
                // (isBareInScopeParam) — we are the closed-type arm and the
                // param is "not closed" by definition.
                return false;
            }
            for (const TypeRef* arg : ref->genericArgs) {
                if (!typeContainsNoInScope(arg, inScope)) return false;
            }
            return true;
        case TypeRef::Kind::InlineArray:
            return typeContainsNoInScope(ref->arrayElement, inScope);
        case TypeRef::Kind::Function:
            for (const auto& p : ref->funcParams) {
                if (!typeContainsNoInScope(p.type, inScope)) return false;
            }
            return typeContainsNoInScope(ref->funcReturn, inScope);
    }
    return true;
}

// The §14.6 per-substitution verdict: `true` iff this substitution at a
// single target-parameter position is EITHER a bare in-scope parameter of
// the source declaration OR a closed type. The shape the spec calls
// "expanding" (an argument containing an in-scope parameter other than as
// the bare argument itself, the nullable form `T?` included) returns false.
bool substitutionIsAdmitted(const TypeRef* sub, const ast::Type* subAst,
                            const std::set<std::string_view>& inScope) {
    // Skip unresolved args (Error kind); the resolution diagnostic was
    // already reported elsewhere, and re-reporting here would cascade.
    if (!sub || sub->kind == TypeRef::Kind::Error) return true;
    return isBareInScopeParam(sub, subAst, inScope) ||
           typeContainsNoInScope(sub, inScope);
}

// Compute the in-scope parameter NAME set at the source of an
// `instantiations_` edge. The source is the enclosing TYPE (when the
// instantiation site is at type-level, e.g., field types, base clauses,
// signature types AT the type level, OR inside a non-generic method's
// body) — the set is then the type's generic-parameter names. When the
// site sits inside a generic METHOD's body, the in-scope set is the
// union of the method's generic-parameter names and the enclosing
// type's. (The methodGenericParams field carries the method's name list;
// see #2713's mirror in checkOwningElementInstantiations.)
//
// Takes the fields directly (rather than the Resolver-private
// GenericInstantiation struct) so this helper can live at namespace
// scope in the anonymous namespace above.
std::set<std::string_view> inScopeParamNamesFor(
        TypeSymbol* enclosingType,
        const std::vector<ast::GenericParam>* methodGenericParams) {
    std::set<std::string_view> names;
    if (enclosingType) {
        for (const ast::GenericParam& gp :
             *bezel::sema::detail::astGenericParams(enclosingType)) {
            names.insert(gp.name);
        }
        if (names.empty()) {
            // Fallback to symbol-side for imported types (no AST). See
            // GenericParams.hpp for the precedence rule.
            for (Symbol* gp : enclosingType->genericParams) {
                if (gp) names.insert(gp->name);
            }
        }
    }
    if (methodGenericParams) {
        for (const ast::GenericParam& gp : *methodGenericParams) {
            names.insert(gp.name);
        }
    }
    return names;
}

// Render a TypeRef to a stable, user-readable spelling for the diagnostic.
// Reuses the AST node's syntactic form when available; otherwise falls back
// to "T<…>" using the imported-name or headName. The spelling is best-
// effort; the diagnostic's load-bearing part is the cycle (named by the
// target's symbol name) and the source's name, not the precise type print.
std::string substitutionSpelling(const TypeRef* sub, const ast::Type* subAst) {
    if (subAst) {
        // Best effort: walk the AST's name and generic-args.
        // We don't reconstruct the full grammar — just the head and any
        // immediate generic args — because the diagnostic's purpose is
        // identifying the cycle and the growing argument, not printing
        // arbitrary types.
        std::string out;
        if (subAst->name) {
            for (std::size_t i = 0; i < subAst->name->parts.size(); ++i) {
                if (i != 0) out += '.';
                out += std::string(subAst->name->parts[i]);
            }
        }
        if (!subAst->genericArgs.empty()) {
            out += '<';
            for (std::size_t i = 0; i < subAst->genericArgs.size(); ++i) {
                if (i != 0) out += ", ";
                out += substitutionSpelling(nullptr,
                                            subAst->genericArgs[i].get());
            }
            out += '>';
        }
        if (subAst->nullable) out += '?';
        return out;
    }
    if (!sub) return "<missing>";
    std::string out;
    if (!sub->headName().empty()) out += std::string(sub->headName());
    if (!sub->genericArgs.empty()) {
        out += '<';
        for (std::size_t i = 0; i < sub->genericArgs.size(); ++i) {
            if (i != 0) out += ", ";
            out += substitutionSpelling(sub->genericArgs[i], nullptr);
        }
        out += '>';
    }
    if (sub->nullable) out += '?';
    return out;
}

// §14.6 places the rejection "at the declaration holding the edge". Give it a
// real anchor: `Symbol::declLoc` when SymbolCollection stamped one, otherwise
// the declaration node's own span. Returns the degenerate location only when
// neither exists (an imported symbol, which carries no AST at all).
SourceLocation declarationAnchor(const Symbol* sym) {
    if (!sym) return SourceLocation{};
    if (sym->declLoc.line != 0) return sym->declLoc;
    if (sym->decl && sym->decl->span.begin.line != 0)
        return sym->decl->span.begin;
    return SourceLocation{};
}

}  // namespace

// §14.6 / #3271 (v0.262): expanding in-SCC instantiation edges are a
// compile error at the declaration holding the edge.
//
// Algorithm:
//   1. Build the instantiation-request graph from `instantiations_`. Nodes
//      are the generic TypeSymbols that appear either as a target (`type`)
//      or as a source (the `enclosingType` of any entry). Edges run from
//      source to target. The substitution is the recorded argRefs/argAsts
//      tuple (one edge per recorded instantiation, so a single program
//      position with two different targets yields two edges, both captured).
//   2. Compute strongly-connected components over the graph via Tarjan's
//      algorithm (recursive form, depth bound by the node count). The graph
//      is small (every generic declaration is at most one node) so the
//      recursion is bounded.
//   3. For each edge whose source and target are in the same SCC, apply
//      the §14.6 per-edge condition: every target parameter position must
//      be substituted with either (a) a bare type parameter in scope at
//      the source declaration or (b) a closed type. Any other substitution
//      — an argument containing an in-scope parameter other than as the
//      bare argument itself, the nullable form `T?` included — is a
//      compile error at the declaration holding the edge.
//
// Edge attribution (source side). The spec attributes each edge to a
// specific node ("each generic type is a node, and each generic member is
// its own node; a non-generic member's edges attach to its declaring
// type's node"). The instantiations_ record carries the enclosing TYPE
// (currentOuterType_) and an optional methodGenericParams pointer, so the
// natural decomposition is:
//
//   * enclosingType is null: the edge is at module scope. Such edges are
//     trivially not in any SCC and are skipped — there is no cycle to
//     bound.
//   * enclosingType is non-null, methodGenericParams is null: the edge
//     attaches to the enclosing type's node. The in-scope parameter set
//     is the enclosing type's generic-parameter names.
//   * enclosingType is non-null, methodGenericParams is non-null: the
//     edge sits inside a generic METHOD's body. Per spec, the edge
//     attaches to that generic member's node. The in-scope parameter set
//     is the union of the method's and the enclosing type's parameter
//     names.
//
// For the generic-member attribution path, this implementation v1 keeps
// the graph at TYPE granularity: it attributes the edge to the enclosing
// type's node regardless of methodGenericParams, and computes the in-
// scope set as the union. This is the conservative reading — the type's
// in-scope set is a SUBSET of the method's (it lacks the method's own
// parameters), so a substitution admitted at the method level may be
// over-rejected at the type level. The known over-rejection is recorded
// below; for the two reproducing shapes from #0104 §4.2 (call route via
// a NON-generic method, data route via a field), the substitution uses
// the type's own parameter — so the verdict is identical. The
// generic-method-body case is left as a recorded follow-up. This matches
// the spec's "per-edge over the component" posture: the rule fires
// independently of dynamic reachability, so over-rejecting an admitted
// shape is preferable to missing an expanding one.
//
// Diagnostic placement: at the declaration holding the edge — the AST
// node of the recorded instantiation site (`inst.siteKey`). The shape the
// spec names is "the declaration-site shape of §14.2's bound-cycle rule
// and §27.8's alias rule", so the message names the cycle (the SCC's
// representative target), the growing argument (the failing substitution
// at one target parameter position), and the spec section. One diagnostic
// per (edge, position) so the user sees every position that violates the
// rule; the SCC and the position are both in the diagnostic.
void Resolver::checkExpandingInstantiationEdges() {
    if (instantiations_.empty()) return;

    // ----- 1. Build the graph -------------------------------------------------
    // Nodes are TypeSymbol* that appear as either source or target. Edges
    // are tuples (source, target, instPtr, paramIdx, subPtr, subAstPtr)
    // — we keep the per-position data because the per-edge verdict is
    // position-specific (§14.6: "every edge … must substitute each type
    // parameter of its target with …").
    struct EdgeInfo {
        std::size_t instIdx;        // index into instantiations_
        std::size_t paramIdx;       // which target parameter position
    };
    std::unordered_map<TypeSymbol*, std::vector<TypeSymbol*>> adj;
    std::unordered_map<TypeSymbol*, std::vector<EdgeInfo>> edgesBySrc;
    std::unordered_set<TypeSymbol*> nodes;
    for (std::size_t i = 0; i < instantiations_.size(); ++i) {
        const GenericInstantiation& inst = instantiations_[i];
        if (!inst.enclosingType || !inst.type) continue;
        // Edge source = enclosingType (conservative type-level attribution
        // — see header comment for the known over-rejection in the generic-
        // method-body case). Target = type.
        TypeSymbol* src = inst.enclosingType;
        TypeSymbol* tgt = inst.type;
        nodes.insert(src);
        nodes.insert(tgt);
        adj[src].push_back(tgt);
        // One EdgeInfo per TARGET PARAMETER POSITION so the per-position
        // verdict can be reported individually (the diagnostic names the
        // growing argument at the failing position).
        const std::size_t n = inst.argRefs.size();
        for (std::size_t k = 0; k < n; ++k) {
            edgesBySrc[src].push_back({i, k});
        }
    }
    if (nodes.empty()) return;

    // ----- 2. Compute SCCs via Tarjan -----------------------------------------
    // Iterative Tarjan (the recursive form overflows on large graphs; this
    // codebase has at most a few hundred generic declarations, but using
    // the iterative form here keeps the depth-bounded invariant). The
    // index/counter gives the visitation order; lowlink tracks the
    // smallest index reachable from the subtree. A node joins the current
    // SCC when lowlink[index] == index. The result is the SCC list in
    // reverse topological order (post-order on the condensation graph).
    std::unordered_map<TypeSymbol*, int> indexOf;
    std::unordered_map<TypeSymbol*, int> lowlinkOf;
    std::vector<TypeSymbol*> stack;
    std::unordered_set<TypeSymbol*> onStack;
    std::vector<std::vector<TypeSymbol*>> sccs;
    int counter = 0;
    struct Frame {
        TypeSymbol* node;
        std::vector<TypeSymbol*>::iterator it;
    };
    for (TypeSymbol* root : nodes) {
        if (indexOf.count(root)) continue;
        std::vector<Frame> work;
        work.push_back({root, adj[root].begin()});
        indexOf[root] = counter;
        lowlinkOf[root] = counter;
        ++counter;
        stack.push_back(root);
        onStack.insert(root);
        while (!work.empty()) {
            Frame& f = work.back();
            TypeSymbol* u = f.node;
            auto& a = adj[u];
            if (f.it != a.end()) {
                TypeSymbol* w = *f.it;
                ++f.it;
                if (!indexOf.count(w)) {
                    indexOf[w] = counter;
                    lowlinkOf[w] = counter;
                    ++counter;
                    stack.push_back(w);
                    onStack.insert(w);
                    work.push_back({w, adj[w].begin()});
                } else if (onStack.count(w)) {
                    if (indexOf[w] < lowlinkOf[u]) lowlinkOf[u] = indexOf[w];
                }
            } else {
                // Children exhausted — finalize u.
                if (lowlinkOf[u] == indexOf[u]) {
                    std::vector<TypeSymbol*> scc;
                    while (true) {
                        TypeSymbol* x = stack.back();
                        stack.pop_back();
                        onStack.erase(x);
                        scc.push_back(x);
                        if (x == u) break;
                    }
                    sccs.push_back(std::move(scc));
                }
                work.pop_back();
                if (!work.empty()) {
                    TypeSymbol* parent = work.back().node;
                    if (lowlinkOf[u] < lowlinkOf[parent])
                        lowlinkOf[parent] = lowlinkOf[u];
                }
            }
        }
    }

    // ----- 3. For each edge, apply the per-edge verdict ------------------------
    // Map each node to its SCC representative (the node itself in a single-
    // node SCC, or any node in a multi-node SCC; we use the SCC's first
    // element as the canonical "this is the cycle"). A node is "in an SCC
    // with another" iff its SCC has size > 1 OR it is a self-loop (the
    // latter is a property of the EDGES, not the SCC: a single-node SCC
    // has a self-loop iff some edge runs from the node to itself).
    std::unordered_map<TypeSymbol*, int> sccId;  // canonical SCC id per node
    for (std::size_t i = 0; i < sccs.size(); ++i) {
        for (TypeSymbol* n : sccs[i]) sccId[n] = static_cast<int>(i);
    }

    // Per-source bookkeeping for the source's in-scope parameter set. The
    // set is the same for every edge sourced from a given enclosingType
    // (the in-scope names come from the type and the optional method
    // params of the recorded site); cache it.
    std::unordered_map<TypeSymbol*, std::set<std::string_view>> inScopeCache;
    // Dedup set: one diagnostic per (source, target, param-position) — a
    // single expanding edge may be recorded multiple times if the same
    // declaration names the same target with the same substitution at
    // multiple sites, and we want one error per offending position rather
    // than a flood.
    std::set<std::tuple<TypeSymbol*, TypeSymbol*, std::size_t>> reported;

    for (auto& entry : edgesBySrc) {
        TypeSymbol* src = entry.first;
        if (!sccId.count(src)) continue;  // not in any SCC? shouldn't happen
        int srcScc = sccId[src];
        for (const EdgeInfo& e : entry.second) {
            const GenericInstantiation& inst = instantiations_[e.instIdx];
            TypeSymbol* tgt = inst.type;
            if (!sccId.count(tgt)) continue;
            int tgtScc = sccId[tgt];
            // In-SCC test: same SCC AND (multi-node SCC OR self-loop).
            bool sameScc = (srcScc == tgtScc);
            bool selfLoop = (src == tgt);
            if (!sameScc && !selfLoop) continue;
            // Fetch / compute the in-scope parameter set.
            auto itScope = inScopeCache.find(src);
            if (itScope == inScopeCache.end()) {
                itScope = inScopeCache.emplace(src,
                    inScopeParamNamesFor(inst.enclosingType,
                                         inst.methodGenericParams)).first;
            }
            const std::set<std::string_view>& inScope = itScope->second;
            // Per-position verdict.
            if (e.paramIdx >= inst.argRefs.size()) continue;
            const TypeRef* sub = inst.argRefs[e.paramIdx];
            const ast::Type* subAst = (e.paramIdx < inst.argAsts.size())
                                          ? inst.argAsts[e.paramIdx]
                                          : nullptr;
            if (substitutionIsAdmitted(sub, subAst, inScope)) continue;
            // Emit a diagnostic. One per (src, tgt, position) — the user
            // sees every offending position on a single declaration.
            if (!reported.insert({src, tgt, e.paramIdx}).second) continue;
            const std::string targetName = std::string(tgt->name);
            // Locate the target's parameter name at this position so the
            // diagnostic names the SPECIFIC parameter being substituted
            // with the growing argument. Walk target's genericParams
            // positionally (matches the substitution at position e.paramIdx).
            std::string paramName = "?";
            const std::vector<ast::GenericParam>* tgtAst =
                bezel::sema::detail::astGenericParams(tgt);
            if (tgtAst && e.paramIdx < tgtAst->size())
                paramName = std::string((*tgtAst)[e.paramIdx].name);
            else if (e.paramIdx < tgt->genericParams.size() &&
                     tgt->genericParams[e.paramIdx])
                paramName = std::string(tgt->genericParams[e.paramIdx]->name);
            // §14.6 places the error "at the declaration holding the edge".
            // `inst.loc` is the `Node<Pair<T, T>>` AST node's span, which is
            // most precise anchor available — but `Parser::parseType` stamps
            // no span (the #1307/#2496 degenerate-span class, called out in
            // Parser_Stmts.cpp's catch-arm stamping comment), so every site
            // here reports the degenerate 0:0. Fall back to the SOURCE
            // declaration, which is the anchor the spec names anyway; the
            // site's span is still preferred so this needs no revisiting if
            // parseType is ever taught to stamp.
            SourceLocation loc =
                inst.loc.line != 0 ? inst.loc : declarationAnchor(src);
            std::string message =
                "the type parameter `" + paramName +
                "` of generic type `" + targetName +
                "` is substituted with a constructed type containing a "
                "type parameter of the enclosing declaration in a "
                "recursive generic instantiation: `" +
                targetName + "<" + substitutionSpelling(sub, subAst) +
                ">` (this declaration) requires `" + targetName +
                "` instantiated at a growing argument, whose instantiation "
                "closure has no bound (\xC2\xA7""14.6, #3271); a "
                "recursive generic may not nest a type parameter inside a "
                "constructed type argument";
            if (sub && sub->nullable)
                message += " \xE2\x80\x94 the nullable form `T?` is "
                           "explicitly excluded by the rule";
            ctx_.diagnostics().report(Severity::Error, loc, message);
        }
    }
}

// §14.6 / #3271 (v0.262), generic-MEMBER half. §14.6's graph makes "each
// generic type … a node, and each generic member … its own node", and the
// consequences paragraph names the call route explicitly: "a call
// `F<Pair<T, T>>` inside `F<T>`'s body". checkExpandingInstantiationEdges
// above answers that for generic TYPES, because `instantiations_` records a
// resolved `Named<…>` annotation and its node is always a TypeSymbol. A
// generic METHOD's self-request has no representation there at all — a
// `Depth<Pair<T, T>>(…)` call inside `Depth<T>` records only the *argument*
// `Pair<T, T>` (target `Pair`, an edge that closes no cycle), never the
// `Depth` → `Depth` edge — so the shape compiled silently.
//
// This function verdicts the self-edges recorded by
// `Resolver::recordGenericMemberSelfEdge`, against the SAME
// `substitutionIsAdmitted` predicate the type half uses: exactly one
// authority for "bare in-scope parameter or closed type", so the two halves
// cannot answer differently for the same substitution.
//
// SCOPE, and why it stops where it does. Only the DIRECT self-loop is
// decided here. §14.6's condition is per-edge *inside a strongly-connected
// component*, and deciding component membership for a member edge needs a
// resolved call graph — which does not exist at type-resolution time and
// which the typechecker builds only later. A self-loop is the one member
// edge class that needs no such graph: an edge from a node to itself is
// unconditionally inside its own component. Two residuals follow and are
// recorded as follow-up rather than guessed at:
//
//   * MUTUAL member cycles (`M<T>` calls `N<Pair<T, T>>`, `N<U>` calls
//     `M<U>`) — in-SCC per §14.6, not decidable without the call graph.
//   * INFERENCE-supplied type arguments (`Depth(p, n - 1)` where `p` has
//     type `Pair<T, T>`) — the same edge with no spelled substitution to
//     read; the substitution exists only after ExprTypecheck's inference.
//
// Both under-approximate — they miss expanding edges, they never reject an
// admitted one — which is the safe direction for a rule whose failure mode
// on the other side is rejecting legitimate generic code.
void Resolver::checkExpandingGenericMemberEdges() {
    if (genericMemberEdges_.empty()) return;

    // One diagnostic per (source member, target parameter position): a member
    // that spells the same expanding self-call at three sites is one defect.
    std::set<std::tuple<const ast::MethodDecl*, std::size_t>> reported;

    for (const GenericMemberEdge& edge : genericMemberEdges_) {
        const ast::MethodDecl* src = edge.source;
        if (!src || !edge.enclosingType) continue;

        // OVERLOAD GUARD — the whole reason this check can attribute an edge
        // without a resolved call graph. The recorder matched the callee by
        // NAME; if the declaring type has more than one member of that name,
        // the call may bind to a different overload, which §14.6 makes a
        // DIFFERENT node, and whose edge is in an SCC only if that overload
        // reaches back. Rejecting on a name match alone would be exactly the
        // over-rejection this rule must not commit, so an overloaded name is
        // left to the follow-up that models member nodes properly.
        std::size_t sameName = 0;
        for (const Symbol* mem : edge.enclosingType->members) {
            if (mem && mem->name == src->name) ++sameName;
        }
        if (sameName != 1) continue;

        // The source declaration's in-scope parameter names: the member's own
        // plus the enclosing type's, per §14.6 ("the source's own, or an
        // enclosing declaration's"). Same helper the type half uses.
        const std::set<std::string_view> inScope =
            inScopeParamNamesFor(edge.enclosingType, &src->genericParams);

        for (std::size_t i = 0; i < edge.argRefs.size(); ++i) {
            const TypeRef* sub = edge.argRefs[i];
            const ast::Type* subAst =
                i < edge.argAsts.size() ? edge.argAsts[i] : nullptr;
            if (substitutionIsAdmitted(sub, subAst, inScope)) continue;
            if (!reported.insert({src, i}).second) continue;

            const std::string memberName = std::string(src->name);
            const std::string paramName =
                i < src->genericParams.size()
                    ? std::string(src->genericParams[i].name)
                    : std::string("?");
            std::string message =
                "the type parameter `" + paramName +
                "` of generic method `" + memberName +
                "` is substituted with a constructed type containing a "
                "type parameter of the enclosing declaration in a "
                "recursive generic instantiation: `" +
                memberName + "<" + substitutionSpelling(sub, subAst) +
                ">` (this declaration) requires `" + memberName +
                "` instantiated at a growing argument, whose instantiation "
                "closure has no bound (\xC2\xA7""14.6, #3271); a "
                "recursive generic may not nest a type parameter inside a "
                "constructed type argument";
            if (sub && sub->nullable)
                message += " \xE2\x80\x94 the nullable form `T?` is "
                           "explicitly excluded by the rule";
            SourceLocation loc = edge.loc.line != 0
                                     ? edge.loc
                                     : declarationAnchor(edge.enclosingType);
            ctx_.diagnostics().report(Severity::Error, loc, message);
        }
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
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypePureModifiers(t); });
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
                // SEAM-ACCESSOR: `ms` is a member of `t`, which forEachTypeAndNested (#3574) draws
                // only from this module's own declaration tree — never an imported
                // symbol (#3152).
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
            // SEAM-ACCESSOR: same local-only `ms` as the OperatorDecl arm above (#3152).
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
            // SEAM-ACCESSOR: same local-only `ms` as the two purity-placement casts above (#3152).
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
                // SEAM-ACCESSOR: `cs` is a member of the locally-declared `t` — same reasoning as the
                // method arms above (#3152).
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

        // ---- PropertyDecl / IndexerDecl -----------------------------------
        if (auto* ps = dynamic_cast<PropertySymbol*>(m)) {
            if (!hasMod(ps, "pure")) continue;
            const auto* pd =
                // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t` — same reasoning as the
                // method/constructor arms above (#3152).
                dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd) continue;
            // §22.15: writing `pure` on the auto-property form is rejected
            // as redundant — the auto-getter is implicitly pure-classified.
            // An interface property's signature-only form parses as Auto but
            // is an abstract declaration, not an auto-property with backing
            // storage — pure is the contract, not redundant.
            //
            // fix #4018/#4020 follow-on (found auditing every
            // PropertyBodyKind::Auto reader for the same hazard, §12.5): a
            // CLASS-level `abstract pure property Name: T;` parses with the
            // exact same bodyKind == Auto as this check's ordinary
            // auto-property case (the member-modifier prefix at
            // Parser_Decls.cpp admits `pure` at the class-member PropertyDecl
            // grammar slot, not just IfaceProperty's), and has the identical
            // "signature-only, no auto-getter to be redundant with" shape the
            // `parentIsIface` exclusion above already carves out for
            // interfaces — but `parentIsIface` is false for a class, so
            // `public abstract pure property Name: string;` was false-
            // rejected as "redundant" (measured: probe with an abstract base
            // class declaring exactly this shape, overridden concretely by a
            // subclass, rejected at the base declaration before the override
            // is ever reached). Excluded via the shared
            // `isAbstractPropertyShorthand` (sema/Symbol.hpp) rather than a
            // third copy of the modifier scan.
            const bool parentIsIface =
                ps->parent && ps->parent->kind == ast::TypeDeclKind::Interface;
            if (pd->bodyKind == ast::PropertyBodyKind::Auto &&
                !parentIsIface && !isAbstractPropertyShorthand(*pd)) {
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
        // SEAM-ACCESSOR: `ps` is a member of the locally-declared `t` (this function's own
        // `for (Symbol* m : t->members)` loop just above) — same reasoning as
        // every other arm of checkTypePureModifiers (#3152).
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
    // fix #3574: routed through the ONE recursive nested-type walk
    // (§11.6, v0.286) -- this rule applies at every nesting depth.
    forEachTypeAndNested([&](TypeSymbol* t) { checkTypePureDispatch(t); });
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
            // fix #3574: the shared recursive walk (§11.6, v0.286). Driven from
            // inside this loop rather than via forEachTypeAndNested so the
            // extension arm below stays interleaved in declaration order.
            visitTypeAndNested(
                t, [&](TypeSymbol* ty) { checkTypeReadonlyPropertyForms(ty); });
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
    // SEAM-ACCESSOR: `ps` is always local: both call sites (checkTypeReadonlyPropertyForms
    // and the extension arm of checkReadonlyPropertyForms, ~20/~30 lines
    // above) draw it from `t->members`/`ext->members`, where `t`/`ext` are
    // themselves module()->children-only (#3152).
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
