#include "common/OwnershipFacts.hpp"
#include "common/BuiltinRegistry.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/Node.hpp"
#include "sema/FieldSymbolAccessors.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SubstArgs.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

// #1414 Stage 4 Phase A — the shared ownership-facts core (see the header).
//
// The bodies below are the pre-refactor codegen predicates
// (Emitter::isFreshOwnedClassProducer / Emitter::isBuiltinOwnedTransferCall,
// LlvmBackend_Setup.cpp) relocated VERBATIM so both the ownership pass and the
// Emitter reach one copy.  Not one line of classification logic changed: this
// is a behaviour-preserving extraction, gated by the full suite.  The known
// drift (#1388 / #1415) is Phase B and is left untouched here.

namespace bezel::common {

// fix #3634 (§22.10 v0.180 / #3205, v0.286) — see the header declaration for
// the fact/mechanism argument. Relocated VERBATIM from codegen's
// `Emitter::isCapturedOwnedPrimaryParam` (LlvmBackend_impl.hpp), which is now
// a one-line delegating wrapper around this — the #2654 pattern. Not one
// token of the condition changed; this is a behaviour-preserving extraction.
bool isCapturedOwnedPrimaryParam(const sema::FieldSymbol* fs) {
    return fs->posParam && fs->parent &&
           fs->parent->kind == ast::TypeDeclKind::Class &&
           fs->posParam->modifier == "owned" && !fs->posParamInitRef;
}

// §28.9 "Trivial-dispose classes" (v0.286) — see the header. This is the
// pre-#3632 (TypeSymbol-only) entry point. It cannot read a field's RESOLVED
// TypeRef without a `SemaContext`, so its `owned`-field arm is the
// pre-recursive-clause reading: any non-static `owned` field disqualifies.
// That is the SAFE direction (a wrong "non-trivial" frees a no-op Dispose;
// a wrong "trivial" leaks, §6c), and it preserves byte-identical behaviour
// for every caller that does not opt into the new TypeRef+SemaContext
// overload below. The widening (issue 3632) lives in the TypeRef overload;
// callers that thread the active `SemaContext` see it; every existing
// caller keeps the conservative reading.
bool isNonTrivialDisposeClass(const sema::TypeSymbol* decl,
                              ImportedDisposeVerdict importedVerdict) {
    bool reject = false;
    std::unordered_set<const sema::TypeSymbol*> visitedBase;
    const std::size_t kMaxWalk = 4096;  // defensive; base chains are shallow
    std::size_t walked = 0;
    for (const sema::TypeSymbol* cur = decl; cur && !reject;) {
        if (++walked > kMaxWalk) break;
        if (!visitedBase.insert(cur).second) break;  // cycle guard (#1624)
        // fix #3191 (§3.6 / §28.9 / §21.9 / §2.8.2, family M2 of umbrella
        // #3152): an IMPORTED class's carried §3.6 verdict, consulted BEFORE the
        // member scan below — because for an imported class that scan cannot
        // succeed. A class's destructor is deliberately not part of the exported
        // member list (`convertMember`: "§13.3: not part of the public surface —
        // Dispose() is", and there is no `MetaMemberKind::Destructor`), and
        // `ModuleLoader` builds a `DestructorSymbol` on exactly one path — the
        // generic body re-parse. So EVERY non-generic imported class scanned as
        // "no destructor" and this function answered trivial-dispose for all of
        // them, whatever they declared.
        //
        // §2.8.2 requires the artifact interface to carry the §3.6 derived type
        // properties as "computed verdicts for non-generic public types",
        // naming trivial-dispose among them and stating the consequence of
        // omitting them: the consumer must "either reject imported types at
        // those rules or guess". The verdict is now carried
        // (MetaTypeDecl::nonTrivialDispose) and replayed by ModuleLoader, so the
        // question this function asks has an answer on the imported side rather
        // than a default.
        //
        // Checked PER LEVEL, not once at `decl`, which is what makes the
        // reachable shape work: a LOCAL `class Derived: Res` over an imported
        // `Res` has no destructor and no `owned` field of its own, so the
        // verdict must be found when the walk REACHES `Res`. The carried verdict
        // is itself the full recursive answer for the imported type, so the walk
        // is finished the moment it fires.
        //
        // Note the asymmetry this deliberately does NOT introduce: a `false`
        // verdict never short-circuits. Absent (a pre-#3191 artifact) and
        // "genuinely trivial" are the same read, and both must fall through to
        // the member scan, which still decides correctly for a local type and
        // degrades to exactly the pre-fix answer for a stale imported one.
        //
        // fix #3588 (§28.9 / §21.9, v0.286): `importedVerdict` gates ONLY this
        // arm — the structural member scan below runs either way, so a LOCAL
        // non-trivial-dispose class is classified identically for every
        // caller. See the header for which caller asks for `Ignore` and why.
        if (importedVerdict == ImportedDisposeVerdict::Consult &&
            cur->importedNonTrivialDispose) {
            reject = true;
            break;
        }
        for (sema::Symbol* m : cur->members) {
            if (dynamic_cast<sema::DestructorSymbol*>(m)) {
                reject = true;
                break;
            }
            auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
            if (!fs) continue;
            // fix #2666 (§28.9 v0.237 / §15.1.1): an owning-inline-array
            // field — `[N owned]T?` or `[N owned]T`, an `ast::TypeKind::
            // InlineArray` whose `arrayOwned` flag is set — disqualifies
            // trivial-dispose per the spec's third arm. The ownedness lives
            // in the TYPE, not in `fs->modifiers`, so the `owned`-modifier
            // arm below does not see it. Read directly from the AST: a
            // primary-ctor parameter's TYPE lives at `fs->posParam->type`,
            // a body FieldDecl's TYPE at `fd->type`. The IMPORTED-class
            // short-circuit above (`importedNonTrivialDispose`) already
            // covers wire-side fidelity: the exporter runs this same
            // predicate, and the carried verdict is replayed at import.
            // fix #3650: the ONE both-sources field-declared-type accessor
            // (src/sema/FieldSymbolAccessors.hpp) replaces this file's own
            // copy of the AST cast — see that header for the full rationale.
            const ast::Type* fieldAstTy = sema::fieldDeclTypeOf(fs);
            if (fieldAstTy &&
                fieldAstTy->kind == ast::TypeKind::InlineArray &&
                fieldAstTy->arrayOwned) {
                reject = true;
                break;
            }
            if (fs->posParam) {
                // fix #3205 (§22.10 v0.254) / fix #3634: `isCapturedOwnedPrimaryParam`
                // (declared above) is the widened `!posParamInitRef`-only gate,
                // so a NEVER-referenced `owned` primary-ctor parameter is also
                // counted as non-trivial-dispose storage here. See that
                // predicate's declaration comment for the fact/mechanism
                // argument and #3205's four-case truth table. Walked AFTER the
                // inline-array arm above so a primary-ctor parameter whose type
                // is `[N owned]T?` is also caught (#2666); record positional
                // fields skip the inline-array leg via `fieldAstTy == null`
                // for the `record` parent (their type resolves to the
                // param's type, but the inline-array arm above rejects first
                // for owning records — rejected separately).
                //
                // Before #3634 this repeated the condition inline
                // (character-identical to codegen's own copy); it now calls
                // the ONE shared predicate both consumers share, so a future
                // widening of one edits both from a single site.
                if (isCapturedOwnedPrimaryParam(fs)) {
                    reject = true;
                    break;
                }
                continue;
            }
            // A `static owned` field (§22.12 rule 4) is disposed at
            // process-exit order (§22.13), not by this instance's Dispose()
            // — exclude it, or a class whose only `owned` member is static
            // would be wrongly classified non-trivial-dispose here.
            bool isStatic = false, isOwned = false;
            for (auto mod : fs->modifiers) {
                if (mod == "static") isStatic = true;
                if (mod == "owned") isOwned = true;
            }
            if (isOwned && !isStatic) {
                reject = true;
                break;
            }
        }
        sema::TypeSymbol* nextBase = nullptr;
        for (sema::TypeSymbol* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) {
                nextBase = b;
                break;
            }
        cur = nextBase;
    }
    return reject;
}

namespace {

// fix #3632 (§28.9 v0.286, §3.6 rule 2): the §28.9 "no `owned` fields of
// NON-TRIVIAL-DISPOSE TYPE" recursive clause, walked coinductively.
// `inProgress` is the PATH set (§6 / #1624 / #2159 — re-entry answers
// trivially trivial-dispose, then erases on exit so a sibling field's
// re-classification on the same TypeRef gets the full fresh verdict).
// Path semantics are essential: a `Holder<X>` and `Holder<Y>` both walk
// through the same `Inner` TypeRef on different paths, and an outer
// enclosing generic must NOT eat a sibling-frame re-classification.
bool isNonTrivialDisposeClassImpl(
    const sema::TypeRef* rt,
    const sema::SemaContext& ctx,
    common::ImportedDisposeVerdict importedVerdict,
    std::unordered_set<const sema::TypeSymbol*>& inProgress);

// §3.6 rule 2 / §9.7 / #2145: substitute a bare type-parameter field
// reference (`owned field: T`) through `decl`'s `genericParams` ×
// `rt->genericArgs` — the §28.9 example `OwningBox<T>` reads the field
// type as `T`, resolves the parameter via the holder's genericArgs, and
// classifies the substituted type (`OwningBox<Connection>` →
// non-trivial-dispose iff `Connection` is). Returns rt unchanged when
// the substitution does not apply (a concrete, non-parameter type —
// the immediate-fix shape `class K { owned inner: Inner? }`, where
// `Inner?` has a declaration and is not a bare parameter).
const sema::TypeRef* substitutedTrivialDisposeFieldType(
    const sema::TypeSymbol* decl,
    const sema::TypeRef* rt,
    const sema::TypeRef* fieldTy) {
    if (!decl || !fieldTy) return fieldTy;
    // Only a bare type-parameter reference (Named, declaration-null,
    // argument-less, head naming one of `decl`'s genericParams) needs
    // substitution; a concrete type is a no-op. The shape matches
    // `substitutedChoicePayloadType` (§35.4) and the §9.7 record walk's
    // bare-parameter recognizer (#2145).
    if (fieldTy->kind != sema::TypeRef::Kind::Named || fieldTy->declaration)
        return fieldTy;
    if (!fieldTy->astNode || !fieldTy->astNode->name ||
        fieldTy->astNode->name->parts.empty())
        return fieldTy;
    if (decl->genericParams.empty() || rt->genericArgs.size() != decl->genericParams.size())
        return fieldTy;
    const std::string_view head = fieldTy->headName();
    for (std::size_t i = 0; i < decl->genericParams.size(); ++i) {
        const sema::Symbol* gp = decl->genericParams[i];
        if (!gp || gp->name != head) continue;
        const sema::TypeRef* arg = rt->genericArgs[i];
        if (arg && arg != fieldTy) return arg;
        return fieldTy;  // uninstantiated — keep the conservative reading.
    }
    return fieldTy;
}

bool isNonTrivialDisposeClassImpl(
    const sema::TypeRef* rt,
    const sema::SemaContext& ctx,
    common::ImportedDisposeVerdict importedVerdict,
    std::unordered_set<const sema::TypeSymbol*>& inProgress) {
    if (!rt) return false;
    if (rt->kind != sema::TypeRef::Kind::Named) return false;
    sema::TypeSymbol* decl = rt->declaration;
    if (!decl) return false;  // bare generic parameter — uninstantiated
    if (decl->kind != ast::TypeDeclKind::Class) return false;
    // §28.9 coinductive walk: re-entry of any TypeSymbol already on the
    // current classification path answers trivially trivial-dispose. The
    // path set is shared across the BASE chain and the FIELD-type
    // recursion, so a `Holder<X>` and `Holder<Y>` walking through the same
    // `Inner` TypeRef on different paths each get the full fresh verdict;
    // only a genuinely cyclic cluster (e.g. `A { owned b: B? } class B {
    // owned a: A? }`) hits the path. PATH semantics — insert on entry,
    // erase on exit — match the §9.7 record-walk and §35.4 choice-walk
    // discipline (#1611, #2159). The previous `visitedBase`-only cycle
    // guard (#1624) on the BASE chain alone happened to coincide because
    // `break` with `reject == false` is a return-false; this set makes the
    // answer load-bearing — and extends it across the FIELD-type
    // recursion the original guard did not see.
    if (!inProgress.insert(decl).second) return false;
    bool reject = false;
    const std::size_t kMaxWalk = 4096;
    std::size_t walked = 0;
    sema::TypeSymbol* cur = decl;
    while (cur && !reject) {
        // PATH set entry — already-checked above for `decl`; here for any
        // base-class step. The early `decl` insert handles the recursion
        // guard for the `decl → field's type class → back to decl` shape;
        // the loop's insert here handles a `decl → base → ...` chain.
        if (cur != decl && !inProgress.insert(cur).second) break;
        if (++walked > kMaxWalk) break;
        // fix #3191: imported classes consult the carried §3.6 verdict FIRST.
        if (importedVerdict == common::ImportedDisposeVerdict::Consult &&
            cur->importedNonTrivialDispose) {
            reject = true;
            break;
        }
        for (sema::Symbol* m : cur->members) {
            if (dynamic_cast<sema::DestructorSymbol*>(m)) { reject = true; break; }
            auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
            if (!fs) continue;
            // fix #2666 / §15.1.1: an owning-inline-array field disqualifies
            // — the AST `arrayOwned` flag is read directly (no ctx needed).
            // Gate the AST cast on `fs->decl != nullptr` (an IMPORTED field
            // symbol has decl == nullptr per ModuleLoader's policy, #3149).
            // Imports are already short-circuited above via the Consult arm
            // of `importedNonTrivialDispose` (the exporter runs the same
            // predicate and carries the verdict across the wire, #3191).
            // fix #3650 (closes issue 3651's interim SEAM-ACCESSOR marker):
            // the ONE both-sources field-declared-type accessor, same as the
            // legacy overload's arm above.
            const ast::Type* fieldAstTy = sema::fieldDeclTypeOf(fs);
            if (fieldAstTy && fieldAstTy->kind == ast::TypeKind::InlineArray &&
                fieldAstTy->arrayOwned) { reject = true; break; }
            if (fs->posParam) {
                // fix #3205 / §22.10: a captured `owned` primary-ctor parameter
                // disqualifies regardless of type — its generated capture field
                // walks in the generated Dispose (the #3634 fact/mechanism).
                if (fs->parent && fs->parent->kind == ast::TypeDeclKind::Class &&
                    fs->posParam->modifier == "owned" && !fs->posParamInitRef) {
                    reject = true;
                    break;
                }
                continue;
            }
            // A `static owned` field (§22.12 rule 4) is disposed at process
            // exit, not by this instance's Dispose — exclude (§22.13).
            bool isStatic = false, isOwned = false;
            for (auto mod : fs->modifiers) {
                if (mod == "static") isStatic = true;
                if (mod == "owned") isOwned = true;
            }
            if (!isOwned || isStatic) continue;
            // fix #3632 (§28.9 / §3.6 rule 2, v0.286): the recursive clause.
            // An `owned` field of TRIVIAL-dispose type does NOT disqualify —
            // the disposal cascade over it runs no user code, exactly what
            // §28.9 asserts. Resolve the field's TypeRef through ctx, apply
            // per-instantiation substitution when the field's declared type
            // is a bare `T` (a `OwningBox<T>` over `Connection` reads the
            // substituted `Connection`, not the uninstantiated `T`), and
            // recurse. The recursive call uses the field's resolved TypeRef
            // (it carries the substituted genericArgs for any nested generic
            // field type, the §9.7 / #2145 record-walk discipline).
            const sema::TypeRef* fieldTy =
                sema::storageComponentType(fs, ctx);
            if (!fieldTy) continue;  // unknown → conservative-skip (no false reject)
            fieldTy = substitutedTrivialDisposeFieldType(cur, rt, fieldTy);
            // Only class/interface TypeRefs count toward the §28.9. A record,
            // enum, primitive, function, unit, inline-array is OUT by kind;
            // a Nullable<Inner> reads as Inner (the nullable flag is on the
            // TypeRef) — the recursion into Inner decides its verdict; a
            // null fieldTy (unresolved) is conservative-skip.
            if (fieldTy->kind != sema::TypeRef::Kind::Named) continue;
            if (!fieldTy->declaration) {
                // fix #3647 (§28.9 / §3.6 rule 2, v0.286): `declaration ==
                // nullptr` here has TWO causes, not one. (a) a still-bare
                // parameter that did not resolve (the holder itself is
                // uninstantiated, or the substitution didn't apply) — truly
                // unknown, conservative-reject is right. (b) a substitution
                // that DID apply, onto a real, resolved stdlib builtin with
                // NO user TypeSymbol at all — `Random`, `StringBuilder`, the
                // borrowing collections (`TypeResolution_Names.cpp`'s
                // "nullary stdlib classes: no user TypeDecl" list) — which
                // is not unresolved, just symbol-less. Measured: `OwningBox
                // <Random>` (§28.9's own worked "trivial-dispose for generic
                // class instantiations" example, substituting the spec's
                // OwningBox<T> at the stdlib's own worked trivial-dispose
                // type) fell into the (a) branch and rejected, which is
                // wrong per §3.6 rule 2 — `Random` unambiguously classifies
                // trivial-dispose (§28.9's own "Standard library classes
                // that are trivial-dispose include Random" sentence).
                //
                // Case (b) is exactly the question
                // `MethodOwnership::localIsTrivialDisposeNewBinding`
                // (Ownership.cpp) already answers for a no-TypeSymbol
                // builtin at the §28.3 auto-drop gate — reuse its authority,
                // `common::isChainClaimedBuiltinHeadName`, rather than a
                // second table. That table names exactly the builtins with
                // NO user TypeDecl that are ALSO trivial-dispose (the eight
                // borrowing collections, §28.9/§28.11's "they do not own
                // their elements"; the five vtable-less builtins Random /
                // Atomic / AtomicInteger / Stopwatch / Buffer; StringBuilder,
                // fix #2269's own citation of this same §28.9 sentence) — the
                // Owning* variants are deliberately absent (they DO own
                // storage, §28.9's non-trivial worked example), so they
                // still fall through to the conservative reject below,
                // unchanged. A head name NOT on the table (including one
                // this compiler has no builtin-registry entry for at all)
                // stays the (a) conservative reject — this widens ONLY the
                // classes the existing authority already calls
                // trivial-dispose, never a class it does not name.
                if (!fieldTy->headName().empty() &&
                    common::isChainClaimedBuiltinHeadName(
                        fieldTy->headName())) {
                    continue;  // resolved, and trivial-dispose — not a reject
                }
                reject = true;
                break;
            }
            sema::TypeSymbol* fieldDecl = fieldTy->declaration;
            if (fieldDecl->kind != ast::TypeDeclKind::Class &&
                fieldDecl->kind != ast::TypeDeclKind::Interface) continue;
            if (isNonTrivialDisposeClassImpl(fieldTy, ctx, importedVerdict,
                                              inProgress)) {
                reject = true;
                break;
            }
        }
        sema::TypeSymbol* nextBase = nullptr;
        for (sema::TypeSymbol* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) {
                nextBase = b;
                break;
            }
        // PATH semantics — erase the CURRENT frame's `cur` from the set on
        // exit so a sibling frame's re-classification (or a base-class step)
        // sees it fresh. The field-type recursion above has its OWN
        // push/pop (#2159's model).
        const sema::TypeSymbol* leaving = cur;
        cur = nextBase;
        inProgress.erase(leaving);
    }
    // FALL-THROUGH cleanup — `cur == nullptr`, the loop terminated normally.
    // (The rejecting path already broke out without erasing; the
    // classification result is `reject == false` so any leftover entry is
    // harmless at call site, but erase for cleanness' sake.)
    if (cur) inProgress.erase(cur);
    inProgress.erase(decl);
    return reject;
}

}  // namespace

// §28.9 — the TypeRef+SemaContext entry point. Used by callers that can
// resolve a field's RESOLVED TypeRef (and so do per-instantiation
// substitution for generics, §3.6 rule 2). Seeds a fresh in-progress PATH
// set. See the header for the rationale and the §3.6 / §28.9 / §21.9 /
// §22.15 / §28.3 call-site migration that must follow.
bool isNonTrivialDisposeClass(
    const sema::TypeRef* rt,
    const sema::SemaContext& ctx,
    ImportedDisposeVerdict importedVerdict) {
    std::unordered_set<const sema::TypeSymbol*> inProgress;
    return isNonTrivialDisposeClassImpl(rt, ctx, importedVerdict, inProgress);
}

const ast::MethodDecl* calleeMethodDecl(const ast::CallExpr& call,
                                        const sema::SemaContext& ctx) {
    // The callee → MethodSymbol → user MethodDecl navigation.  A null callee, a
    // callee with no MethodSymbol (a builtin, or a call through a value /
    // function), or a MethodSymbol with no AST MethodDecl (an imported method —
    // §28.10 / #707) all yield null.
    //
    // fix #3189 (family M2 of umbrella #3152): this NO LONGER doubles as the
    // route to `returnOwned` — see the header. Its remaining callers either
    // dereference the node (a body scan, a modifier list, a generic-parameter
    // list) or use its null as "no user method here, run the builtin rule",
    // and those two readings cannot both be widened.
    if (!call.callee) return nullptr;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx.exprSymbol(*call.callee));
    if (!ms) return nullptr;
    return dynamic_cast<const ast::MethodDecl*>(ms->decl);
}

bool calleeReturnsOwned(const ast::CallExpr& call,
                        const sema::SemaContext& ctx) {
    // fix #3189 (§28.1 / §28.3 / §2.5, v0.254 — family M2 of umbrella #3152):
    // the both-sources route to the `owned`-return marker. See the header.
    if (!call.callee) return false;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx.exprSymbol(*call.callee));
    if (!ms) return false;
    return sema::methodReturnsOwned(*ms, ctx);
}

bool isFreshOwnedClassProducer(const ast::Expr* e, const sema::SemaContext& ctx,
                               const TypeParamResolver& resolveParam) {
    // #1039 / #1053: a literal `new T(...)` or a call to a method declared to
    // return `owned T` — both transfer a fresh +1 the caller solely owns. A
    // call WITHOUT an `owned` return transfers no ownership: its result is a
    // BORROW (e.g. `return this.field`), and reclaiming it would free a value
    // owned elsewhere (use-after-free). `returnOwned` is the sound
    // discriminator. A variable / field / `this` / property receiver is
    // neither a NewExpr nor a call, so it is never a fresh producer.
    //
    // fix #3189 (§28.1/§28.3, v0.254, family M2 of umbrella #3152): the marker
    // is read through `calleeReturnsOwned`, which consults the imported
    // side table as well as the AST decl. The old
    // `calleeMethodDecl(*call, ctx)->returnOwned` answered "not a producer"
    // for EVERY method of a non-generic imported class, so the dozen codegen
    // disposal sites this predicate gates emitted no reclaim: measured,
    // `Lib.Make(i).v` in a 200-iteration loop leaked 3200 bytes in 200
    // allocations under LSan while its character-identical single-module twin
    // was clean. Nothing else disposes those temporaries — the leak IS the
    // proof there is no second owner to double-free.
    //
    // `resolveParam` is the documented shared resolver hook (see header); the
    // weakest-copy shape does not resolve the receiver type-parameter, so the
    // result is byte-identical to the pre-refactor codegen predicate.
    (void)resolveParam;
    if (!e) return false;
    bool producer = false;
    if (dynamic_cast<const ast::NewExpr*>(e)) {
        producer = true;
    } else if (auto* call = dynamic_cast<const ast::CallExpr*>(e)) {
        producer = calleeReturnsOwned(*call, ctx);
    }
    if (!producer) return false;
    // The receiver type must be a heap CLASS: records/structs are value types
    // with no heap block or vtable disposer; interfaces/enums are not `new`-of-
    // class temporaries.
    //
    // fix #3142 (§2.8.3 rule 4, v0.252): read the kind off the TypeSymbol, not
    // through `->decl`. `ModuleLoader` never assigns `Symbol::decl` (there is no
    // AST for a dependency's types), so the old cast was null for EVERY
    // imported type and the "conservatively excluded — leak, but sound"
    // reasoning below it silently applied to every `new ImportedClass()` in
    // temporary position: no §28.3 disposal obligation was registered and the
    // object LEAKED. `TypeSymbol::kind` carries the same fact on both sides —
    // `SymbolCollection::collectTypeDecl` locally, `fromMetaTypeDeclKind` at
    // ModuleLoader.cpp:327 and :2435 for an import.
    const sema::TypeRef* rt = ctx.exprType(*e);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    return rt->declaration->kind == ast::TypeDeclKind::Class;
}

// §28.3 K3.4 (v0.209, #2080; extended #4006/#4016) — see the header for the
// full rationale. A bare `move <identifier>` arm value: a UnaryExpr("move")
// whose operand is a plain identifier, nothing more.
bool isBareMoveArmValue(const ast::Expr& e) {
    const auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e);
    return ue && ue->op == "move" &&
           dynamic_cast<const ast::IdentifierExpr*>(ue->operand.get());
}

namespace {

// One arm of an if/case expression is an OWNED arm iff it is a fresh owned
// producer or a nested owned-meet if/case. // §28.3 arm-meet (v0.184, #1661)
bool armIsOwnedProducer(const ast::Expr* arm, const sema::SemaContext& ctx,
                        const TypeParamResolver& resolveParam) {
    if (!arm) return false;
    // §28.3 K3.4 (v0.209, #2080): a MARKED arm — `owned x`
    // (MarkedTransferExpr) or the spec's current canonical bare `move x`
    // spelling (isBareMoveArmValue, #4006/#4016) — is an OWNED arm: the
    // transfer hands over the binding's obligation on the selected path, so
    // the meet treats it exactly as a fresh producer (the arm-meet row's
    // marked-arm entry; the UNMARKED read stays a borrow arm, K3.13).
    if (dynamic_cast<const ast::MarkedTransferExpr*>(arm)) return true;
    if (isBareMoveArmValue(*arm)) return true;
    if (dynamic_cast<const ast::ConditionalExpr*>(arm) ||
        dynamic_cast<const ast::CaseExpr*>(arm))
        return conditionalCaseArmMeetIsOwned(*arm, ctx, resolveParam);
    return isFreshOwnedClassProducer(arm, ctx, resolveParam);
}

// The shared arm-meet decision (see the header). Every non-absorbing arm must
// be an owned producer; at least one must exist.
bool armsMeetOwned(const std::vector<const ast::Expr*>& arms,
                   const sema::SemaContext& ctx,
                   const TypeParamResolver& resolveParam) {
    bool sawOwned = false;
    for (const ast::Expr* arm : arms) {
        if (!arm) return false;
        // A `throw` arm is bottom-typed and a `null`-literal arm has no value
        // to own — both absorb to the other arms' state (§28.3, §17.12).
        if (dynamic_cast<const ast::ThrowExpr*>(arm)) continue;
        if (dynamic_cast<const ast::NullLiteralExpr*>(arm)) continue;
        if (!armIsOwnedProducer(arm, ctx, resolveParam)) return false;
        sawOwned = true;
    }
    return sawOwned;
}

}  // namespace

bool conditionalCaseArmMeetIsOwned(const ast::Expr& e,
                                   const sema::SemaContext& ctx,
                                   const TypeParamResolver& resolveParam) {
    if (const auto* c = dynamic_cast<const ast::ConditionalExpr*>(&e))
        return armsMeetOwned({c->thenValue.get(), c->elseValue.get()}, ctx,
                             resolveParam);
    if (const auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        std::vector<const ast::Expr*> arms;
        for (const ast::CaseArm& arm : ce->arms) {
            // A block-body arm's value expression is not structurally
            // inspectable here — bail conservatively to a borrow meet.
            if (arm.bodyKind != ast::CaseArmBodyKind::Expression) return false;
            arms.push_back(arm.exprBody.get());
        }
        if (arms.empty()) return false;
        return armsMeetOwned(arms, ctx, resolveParam);
    }
    return false;
}

bool isBuiltinOwnedTransferCall(const ast::CallExpr& call,
                                const sema::SemaContext& ctx) {
    // §4.5/§4.7/§4.9/§4.11/§2.14/§29.1: builtin methods that TRANSFER an owned
    // class element to the caller. These have no MethodSymbol (sema sets no
    // exprSymbol on the builtin callee), so recognition keys on the callee name
    // plus — where a borrowing counterpart with the same name exists — an
    // OWNING receiver type (a borrowing Stack.Pop returns a BORROW; disposing
    // it would double-free). The receiver-name guards are per-method operand
    // pairings, not a reusable name table, so they stay inline here.
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma) return false;
    const auto& nm = ma->member;
    std::string_view rcn;
    if (ma->target) {
        const sema::TypeRef* rt = ctx.exprType(*ma->target);
        // #1462 / §27.8: see through a transparent type alias
        // (`type OA := OwningArray<R>`) — the alias-resolved TypeRef keeps the
        // use-site astNode (`OA`) for diagnostics, so a bare astNode read would
        // miss the owning-collection head and the popped element would never be
        // tracked Owned (no §28.3 auto-drop → leak).
        if (rt && !rt->aliasResolvedHead.empty())
            rcn = rt->aliasResolvedHead;
        else if (rt && rt->astNode && rt->astNode->name &&
                 !rt->astNode->name->parts.empty())
            rcn = rt->headName();
        else if (rt && !rt->importedName.empty())
            rcn = rt->importedName;
    }
    // §4.7/§4.9/§4.11 (#1388 review): the positional/keyed removals transfer an
    // owned element ONLY on an element-owning COLLECTION receiver. Sema leniently
    // accepts these transfer-named calls on BORROWING collections too (Array /
    // Dictionary hold borrows), so without the owning-receiver gate the caller's
    // §28.3 auto-drop would dispose a value it does not own — a double-free.
    // Gate on the registry's owning-collection set (not a hand-list — the miss-
    // a-name bug class this refactor exists to kill). // per §5.11
    if ((nm == "PopLast" || nm == "PopFirst" || nm == "PopAt" ||
         nm == "PopValue") &&
        isOwningCollectionHead(rcn))
        return true;
    // §2.14: `T.Deserialize(d)` is a fresh owned producer for a class target
    // (the receiver is the TYPE, not a collection); a value-type target is
    // filtered out by the caller's class/interface-result guard.
    if (nm == "Deserialize")
        return true;
    if ((nm == "Pop" || nm == "Dequeue") &&
        (rcn == "OwningStack" || rcn == "OwningQueue"))
        return true;
    // #1349 / §29.1 v0.161: TakeCurrent on the owning enumerators (vtable
    // slot 3) transfers the yielded element.
    if (nm == "TakeCurrent" &&
        (rcn == "IOwningEnumerator" || rcn == "IOwningAsyncEnumerator"))
        return true;
    // fix #5361 / stdlib §4.10 (v0.259): OwningSet<T>.Pop — same as Take
    // (renamed per stdlib §4.10). Kept alongside Take until #5362's sema
    // rejection of Take lands in this branch.
    if (nm == "Pop" && rcn == "OwningSet")
        return true;
    return false;
}

// fix #2749 item 3 / #2767: a builtin INSTANCE-call callee is EITHER a
// `MemberAccessExpr` (`recv.M(..)`) OR a `NullCondMemberExpr` (`recv?.M(..)`).
// The two are SIBLING `Expr` kinds — not one a subclass of the other — that
// share identical `target`/`member` fields, so a `dynamic_cast<
// MemberAccessExpr*>`-only gate silently misses every `?.` spelling.
//
// This is sema's own precedent, added for #2581 for exactly this reason
// (`ExprTC::builtinInstanceCallee`, sema/ExprTypecheck_Calls.cpp) — duplicated
// rather than shared because that one is a file-local static inside the sema
// translation unit. Returns the (target, member) pair, or (nullptr, {}) when
// the callee is neither kind. Shared between `isOwningCollectionDrainAliasCall`
// (#2767) and `isBuiltinOwnedMaterializerCall` (#2749) so the two predicates
// cannot drift on the callee shape — a `?.`-widening in one but not the other
// would silently route `recv?.Drain()` through the disposal path while leaving
// `recv?.Split(",")` as a diagnostic gap (or vice versa). Defined here, ahead
// of both predicates, so they can use it without a forward declaration.
// // per §7.5, §21.1, §28.10, §28.11, §29.11, #2581, #2749, #2767
static std::pair<const ast::Expr*, std::string_view> builtinInstanceCallee(
    const ast::Expr* callee) {
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee))
        return {ma->target.get(), ma->member};
    if (auto* ncm = dynamic_cast<const ast::NullCondMemberExpr*>(callee))
        return {ncm->target.get(), ncm->member};
    return {nullptr, {}};
}

bool isOwningCollectionDrainAliasCall(const ast::CallExpr& call,
                                      const sema::SemaContext& ctx) {
    // See the header contract. // per §28.10, §28.11, stdlib §4.5 v0.307,
    // #2146, #2767
    if (!call.args.empty()) return false;
    // fix #2767: the callee is EITHER `recv.M(..)` (MemberAccessExpr) OR
    // `recv?.M(..)` (NullCondMemberExpr) — the two are SIBLING Expr kinds, so
    // a `dynamic_cast<MemberAccessExpr*>`-only gate silently missed every
    // `?.` spelling and the §21.1 / §29.11 rejection of `const q :=
    // a?.Drain();` never fired. The shape is the same #2749 item 3 closed
    // for the builtin-materializer predicate (which is diagnostic-only) —
    // here the predicate ALSO feeds codegen's registerUsingDisposal /
    // registerIterableSourceDisposal (#2146's bz_array double-dispose SEGV)
    // and ownership::MethodOwnership's producerState Borrowed carve /
    // usingVarSourceIsBorrow, so widening has disposal consequences, not
    // diagnostic-only ones, and the narrowness premise (the `?.` form
    // attaches a MethodSymbol for user methods, none for builtins — same
    // `ExprTC::resolveNullCondMember` reasoning #2749 item 3 verified for
    // the materializer predicate) must apply unchanged. The shared
    // builtinInstanceCallee helper below is exactly that extraction;
    // re-using it keeps the two predicates from drifting on the callee
    // shape. // per §21.1 v0.201, §28.10, §28.11, §29.11, #2581, #2749, #2767
    auto [maTarget, mm] = builtinInstanceCallee(call.callee.get());
    if (!maTarget || mm != "Drain") return false;
    // The BUILTIN Drain only: a USER method named `Drain` resolves a
    // MethodSymbol on the callee and is classified by the ordinary user-call
    // rules (returnOwned / borrow-body scan), never by the alias fact. (The
    // `MethodSymbol` cast — not a bare non-null test — mirrors producerState's
    // builtin-transfer guard: sema may attach non-method symbols to a callee.
    // On the `?.` path, `ExprTC::resolveNullCondMember` attaches a
    // MethodSymbol for user methods and FieldSymbol for function-typed
    // fields, both excluded here on the same line that excludes their `.`
    // spellings — the narrowness #2749 measured and pinned for the
    // materializer predicate transfers unchanged to this predicate.)
    if (call.callee &&
        dynamic_cast<sema::MethodSymbol*>(ctx.exprSymbol(*call.callee)))
        return false;
    // Receiver type head must be an owning-collection (alias-resolved, the
    // same extraction as isBuiltinOwnedTransferCall above — #1462).
    std::string_view rcn;
    if (const sema::TypeRef* rt = ctx.exprType(*maTarget)) {
        if (!rt->aliasResolvedHead.empty())
            rcn = rt->aliasResolvedHead;
        else if (rt->astNode && rt->astNode->name &&
                 !rt->astNode->name->parts.empty())
            rcn = rt->headName();
        else if (!rt->importedName.empty())
            rcn = rt->importedName;
    }
    if (!isOwningCollectionHead(rcn)) return false;
    // The receiver must be a STABLE NAMED place — the alias's single owner is
    // then that place's owner. A DIRECT producer/temporary receiver keeps its
    // own (separate) obligation story (#2169) and is not recognized; the gate
    // is on the receiver form only — an index slot or field member-access is
    // recognized even over a temporary chain ROOT (see the header).
    const ast::Expr* recv = maTarget;
    if (dynamic_cast<const ast::IndexExpr*>(recv)) return true;
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(recv)) {
        sema::Symbol* s = ctx.exprSymbol(*id);
        return dynamic_cast<sema::LocalSymbol*>(s) != nullptr ||
               dynamic_cast<sema::ParameterSymbol*>(s) != nullptr ||
               dynamic_cast<sema::FieldSymbol*>(s) != nullptr;
    }
    if (auto* rma = dynamic_cast<const ast::MemberAccessExpr*>(recv))
        return dynamic_cast<sema::FieldSymbol*>(ctx.exprSymbol(*rma)) !=
               nullptr;
    return false;
}

bool isBuiltinOwnedMaterializerCall(const ast::CallExpr& call,
                                    const sema::SemaContext& ctx) {
    // See the header contract. // per §21.1 v0.201, stdlib §5 / §4.4 / §9.6,
    // #2709, #2749
    auto [maTarget, mm] = builtinInstanceCallee(call.callee.get());
    if (!maTarget) return false;
    // The stdlib's `owned`-returning materializer members, each verified to
    // leak under `const` before being added (100-of-100 under LSan):
    //   stdlib §5   ToArray / ToSet / ToDictionary  (LINQ terminals)
    //   stdlib §4.4 Sorted / Reversed / Slice       (Array copy-forms)
    //   stdlib §9.6 Split                           (string)
    //   stdlib §7   GetBytes                        (Encoding; the
    //               `(s, destination: Span<byte>): int` overload returns a
    //               value type and is excluded by the result-head gate below)
    // `ToList` is NOT here: stdlib §5 states "There is no `ToList()` because
    // Bezel does not have a dynamic-array type named `List<T>`".
    //
    // NOT EXHAUSTIVE, and knowingly so — see #2749. Grepping the stdlib for
    // `owned Array` returns also finds ToCharArray, ToByteArray, ToBytes,
    // CopyToArray, ReadAll / ReadAllBytes / ReadAllLines, GetFiles and
    // GetDirectories, which are the same shape but were not individually
    // measured here. The list is deliberately measurement-gated rather than
    // grep-gated — but note the rationale precisely, because an earlier draft of
    // this comment stated it wrongly. §21.1 rejects "an `owned`-returning call",
    // and the STDLIB DECLARATION is the authority for that, NOT whether today's
    // codegen happens to leak: a `const` binding of `ToCharArray()` is illegal
    // per spec whether or not it leaks. So the measurement is evidence of
    // SEVERITY, not of APPLICABILITY, and this list UNDER-APPROXIMATES the rule
    // rather than tracking it. Under-approximating is the right call for a
    // diagnostic-only change — the file-I/O names are hard to probe.
    //
    // fix #2826 (§28.9 / §28.10, v0.240, stdlib §10.1): `ToBytes` enters the
    // allowlist. Per stdlib §10.1, `CString.ToBytes()` returns `owned
    // Array<byte>` — a fresh `bz_array_t*` (the runtime `bz_cstring_to_bytes`
    // allocates a new buffer and copies the CString's bytes into it, so the
    // returned array's `data` does NOT alias the source; see
    // bezel_runtime_strings.cpp:1050-1061 and the round-trip comment).
    // codegen's collection-family arm reclaims the result regardless of how
    // the receiver was constructed (parameter annotation vs. local-from-factory
    // — the result's TypeRef is the same `Array<byte>` Named/declaration-null
    // shape in both), so the lint and §21.1 must both classify it as an
    // owned production. Adding it here closes BOTH halves at once — the
    // spurious §28.10 vacuity warning on `using var b := c.ToBytes();` (#2826)
    // and the would-be-leak the lint steered authors toward
    // (`const b := c.ToBytes();`, accepted by the pre-fix compiler but a leak
    // under LSan). The result-head gate below (`head == "Array"`) still
    // excludes BinarySerializer.ToBytes / JsonSerializer.ToBytes (those return
    // `OwningBuffer<byte>`, head `OwningBuffer` — already covered by
    // `isDisposableBuiltinHeadName` and the chain's `OwningBuffer` disposal
    // arm, #2992). The remaining #2749 items (ToCharArray / ToByteArray /
    // CopyToArray / ReadAll* / GetFiles / GetDirectories) keep their
    // measurement-gated philosophy and are filed separately.
    if (mm != "ToArray" && mm != "ToSet" && mm != "ToDictionary" &&
        mm != "Sorted" && mm != "Reversed" && mm != "Slice" &&
        mm != "Split" && mm != "GetBytes" && mm != "ToBytes")
        return false;
    // BUILTIN only, and the test is "NO symbol at all" — not "no MethodSymbol".
    // The builtin member-call typing path in sema lives entirely inside
    // `if (!calleeSym)` (ExprTypecheck_Calls.cpp), so every one of the eight
    // above reaches its declaration-less collection return type with NOTHING
    // attached to the callee. Anything sema DID resolve is therefore, by
    // construction, not one of these builtins.
    //
    // A `MethodSymbol`-only cast (which is what isOwningCollectionDrainAliasCall
    // uses, and what this predicate first copied) is too weak: it lets a
    // FUNCTION-TYPED FIELD through. `public Split: (string) -> Array<string>;`
    // attaches a `FieldSymbol` — not a MethodSymbol — and the call is typed
    // from the field's `funcReturn`, whose plain `Array<string>` slot is
    // declaration-less, so the result-head gate below passes too. §28.3's
    // producers table classifies a function-value invocation with a
    // non-`owned` concrete return as BORROWED, and §21.1 admits borrows
    // verbatim ("Admitted: […] and **borrows**"), so `const parts :=
    // cfg.Split(line);` over such a field is LEGAL and must keep compiling.
    // An `owned`-marked function-typed slot is a different matter and is
    // already Owned via producerState's `funcReturnOwned` arm — §21.1's FIRST
    // branch, not this one. // per §28.3, §21.1, #2709
    //
    // ⚠ THE SAME NARROWNESS ARGUMENT HOLDS ON THE `?.` PATH, and that is what
    // makes #2749 item 3's widening safe rather than a false-reject risk. The
    // null-conditional form is typed by a SEPARATE function with its own
    // resolution mirror (`ExprTC::resolveNullCondMember`,
    // sema/ExprTypecheck_Calls.cpp), so the premise had to be re-established
    // there rather than assumed: it calls `setExprSymbol(&nm, member)`
    // UNCONDITIONALLY once `lookupMember` (or `lookupExtensionMember`) resolved
    // one, and returns EARLY without attaching anything when neither did — which
    // is exactly the builtin case. So `obj?.Sorted()` over a USER method gets a
    // MethodSymbol, `cfg?.Split(line)` over a function-typed FIELD gets a
    // FieldSymbol, and both are excluded here by the same one line that excludes
    // their `.` spellings; only the compiler-known builtins arrive symbol-less.
    // // per §7.5, §28.3, §21.1, #2581, #2749
    if (call.callee && ctx.exprSymbol(*call.callee) != nullptr)
        return false;
    // The RESULT must be a declaration-less BUILTIN collection head. This is
    // the second half of the narrowness: it excludes any user type reached
    // through a symbol-less callee, and the owning families (`OwningArray`,
    // …), which the stdlib gives no value copy-form at all ("OwningArray<T>
    // deliberately has **no** value copy-forms — neither `Reversed`/`Sorted`
    // nor `ToArray`", stdlib §4.4).
    const sema::TypeRef* rt = ctx.exprType(call);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    std::string_view head;
    if (!rt->aliasResolvedHead.empty())
        head = rt->aliasResolvedHead;
    else if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
        head = rt->headName();
    else if (!rt->importedName.empty())
        head = rt->importedName;
    return head == "Array" || head == "Set" || head == "Dictionary";
}

// §28.8 / §28.3 *Producers* / stdlib §6.1, §7.3, §7.4, §8.1, §4.19 (fix #5284).
// See the header for the full recognition argument. // per §28.8, §28.3, §2.3,
// §2.7, stdlib §4.19/§6.1/§7.3/§7.4/§8.1
bool isBuiltinOwnedStaticFactoryCall(const ast::CallExpr& call,
                                     const sema::SemaContext& ctx) {
    // (a) The STATIC shape `Col.Member(...)`. There is no `?.` form: a static
    // factory is reached through a TYPE NAME, which has no value to
    // null-conditionally access, and `builtinInstanceCallee`'s NullCond arm
    // exists for the INSTANCE materializer shape only. Extracting through the
    // same helper anyway would be harmless (a NullCondMemberExpr target is a
    // value, which (b) below rejects), but reading the MemberAccessExpr
    // directly states the shape this predicate is about.
    const auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
        call.callee.get());
    if (!ma || !ma->target) return false;

    // The RECEIVER's class-name head, in all three spellings the surface
    // admits. `D.Stopwatch.StartNew()` (import alias) and
    // `System.Diagnostics.Stopwatch.StartNew()` (module-qualified) put the
    // class name in the LAST component of a dotted receiver rather than in a
    // bare IdentifierExpr, and MEASURED both were rejected identically
    // pre-fix, so a bare-identifier-only read would fix one spelling of three
    // — the #4698 alias trap. Read the last dotted component, which is the
    // class name in all three.
    std::string_view colName;
    for (const ast::Expr* t = ma->target.get();;) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(t)) {
            colName = id->name;
            break;
        }
        if (auto* inner =
                dynamic_cast<const ast::MemberAccessExpr*>(t)) {
            t = inner->target.get();
            continue;
        }
        return false;  // a non-name receiver: not a static-class shape
    }
    if (colName.empty()) return false;

    // The stdlib table, transcribed from the `owned`-returning static
    // declarations beside each class's own section. Each entry is
    // (member name, the class head the stdlib declares the result as) — the
    // second element is the (c) cross-check, not a re-derivation:
    //
    //   stdlib §8.1  Stopwatch.StartNew(): owned Stopwatch
    //   stdlib §6.1  CancellationTokenSource.WithCancelAfter(delay): owned CTS
    //   stdlib §6.1  CancellationTokenSource.CreateLinked(a, b): owned CTS
    //   stdlib §6.1  CancellationTokenSource.CreateLinkedAll(parents): owned CTS
    //   stdlib §7.3  File.ReadAllBytes(path): owned Array<byte>
    //   stdlib §7.3  File.ReadAllLines(path): owned Array<string>
    //   stdlib §7.4  Directory.GetFiles(path): owned Array<string>
    //   stdlib §7.4  Directory.GetDirectories(path): owned Array<string>
    //   stdlib §4.19 FrozenArray<T>.From(...): owned FrozenArray<T>
    //
    // MEASURED pre-fix on r23-c28 (4e996839): every one of these nine was
    // rejected by the §28.8 "a borrow cannot be passed where an `owned` value
    // is required" diagnostic at the `Take(move x)` call site.
    //
    // BUT THE LAST TWO ROWS ARE CURRENTLY INERT, and deliberately so. The
    // (c) cross-check reads `ctx.exprType(call)`, and MEASURED (with
    // temporary instrumentation: the factory table matched, the result
    // TypeRef did not exist) sema types NO result for `Directory.GetFiles` /
    // `Directory.GetDirectories` — the `colName == "Directory"` arm in
    // ExprTypecheck_Calls.cpp types only `Exists`, and `src/` contains no
    // other `GetFiles` arm. `ctx.exprType` is therefore null, (c) fails
    // open, and the row returns false. The rows are kept because the table
    // documents the stdlib's `owned` surface, and because the day sema gains
    // the missing typing these two become correct with no further change.
    // Claiming ownership of a value the front end never typed would classify
    // a notional `owned` and dispose a zero — the wrong polarity to fail in.
    // The two stay rejected for now; that residual is a SEPARATE
    // missing-sema-typing defect, not a classification one.
    //
    // NOT listed, with the reason in each case (all MEASURED to already accept
    // `Take(move x)`, i.e. already classified Owned by an earlier arm):
    // `Task.FromOwnedResult` / `WhenAllOwned` / `WhenEachOwned` /
    // `Domain.Spawn` / the whole Task family (producerState's SB01-A
    // `isTaskFamilyHead` arm), `CString.From`/`FromBytes`, `WString.From`,
    // `RawBytes.From` (producerState's FFI-buffer arm), and
    // `JsonSerializer.ParseOwned` / `BinarySerializer.Deserialize` (the
    // serializer arms). `Environment.Variables(): owned Dictionary<string,
    // string>` (stdlib §3) is a TENTH `owned` static whose local is ALSO
    // rejected pre-fix, but is DELIBERATELY excluded here: MEASURED, sema
    // types no result for that call at all (the call is not in sema's
    // `colName` surface table, and `src/` contains no `Variables` arm), so
    // there is no TypeRef for the (c) cross-check to read and no codegen
    // lowering — the callee degrades to a zero value (#2434). Admitting a
    // shape whose value the compiler cannot produce would classify a
    // notional `owned` and dispose a zero. That API gap is a separate defect,
    // filed alongside this fix.
    struct OwnedStaticFactory {
        std::string_view colName;
        std::string_view member;
        std::string_view resultHead;
    };
    static const OwnedStaticFactory kOwnedStaticFactories[] = {
        {"Stopwatch", "StartNew", "Stopwatch"},
        {"CancellationTokenSource", "WithCancelAfter", "CancellationTokenSource"},
        {"CancellationTokenSource", "CreateLinked", "CancellationTokenSource"},
        {"CancellationTokenSource", "CreateLinkedAll", "CancellationTokenSource"},
        {"File", "ReadAllBytes", "Array"},
        {"File", "ReadAllLines", "Array"},
        {"Directory", "GetFiles", "Array"},
        {"Directory", "GetDirectories", "Array"},
        {"FrozenArray", "From", "FrozenArray"},
    };
    const OwnedStaticFactory* hit = nullptr;
    for (const OwnedStaticFactory& f : kOwnedStaticFactories)
        if (f.colName == colName && f.member == ma->member) { hit = &f; break; }
    if (!hit) return false;

    // (b) The RECEIVER must be the BUILTIN class, not a user declaration that
    // took the spelling. §2.3 (v0.313) puts every one of these names outside
    // the §3.4 reserved row, so a user `class Stopwatch { static method
    // StartNew(): owned Stopwatch }` is legal and MEASURED compiles today —
    // through the ordinary user-MethodDecl path (`md->returnOwned`), which this
    // predicate must not shadow. `isUserDeclaredTypeSym` is the §2.3/#5272
    // discriminator: a builtinClassSym is `decl == nullptr &&
    // moduleName.empty()`; a user's TypeSymbol is the exact complement. A
    // receiver with NO symbol is NOT a user type, and the pre-existing builtin
    // behaviour stands.
    //
    // Tested on EVERY component of a dotted receiver, not just the class-name
    // one: a user `class Stopwatch` reached through a user alias
    // (`using NS.Stopwatch.StartNew()`) puts the user TypeSymbol on the
    // OUTER expression. Checking only `ma->target` would miss exactly that and
    // claim a user type's factory as a builtin one.
    for (const ast::Expr* t = ma->target.get(); t;) {
        if (sema::isUserDeclaredTypeSym(ctx.exprSymbol(*t))) return false;
        auto* inner = dynamic_cast<const ast::MemberAccessExpr*>(t);
        t = inner ? inner->target.get() : nullptr;
    }

    // (d) A USER / imported static method carries a MethodSymbol on the callee
    // and is classified by the user-call rules, never here — the same
    // "no symbol at all" test and the same premise
    // `isBuiltinOwnedMaterializerCall` uses (the builtin member-call typing
    // path lives entirely inside `if (!calleeSym)`, so a symbol-less callee is
    // what a builtin static factory looks like).
    if (dynamic_cast<sema::MethodSymbol*>(ctx.exprSymbol(*call.callee)))
        return false;

    // (c) The RESULT must be the class/head the stdlib declares. This is the
    // fail-open cross-check: a shape this cannot read is NOT claimed (the
    // caller then keeps the conservative Borrowed verdict, which over-rejects
    // rather than over-accepts) — the same polarity
    // `isBuiltinOwnedMaterializerCall`'s result-head gate documents.
    //
    // Two result shapes occur, matching the sema arms that type them: a
    // `builtinClassSym` DECLARATION (Stopwatch / CancellationTokenSource /
    // FrozenArray<T> — `rt->declaration` is the synthetic TypeSymbol, so
    // `headName()` is empty and the head is read off the symbol), and a
    // declaration-LESS `astNode`-carrying Named TypeRef (Array<T>). A
    // value-builtin declaration is excluded: a value type carries no disposal
    // obligation at all (§28.3), so claiming one would be claiming ownership
    // of nothing.
    const sema::TypeRef* rt = ctx.exprType(call);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    if (rt->declaration) {
        if (sema::isUserDeclaredTypeSym(rt->declaration)) return false;
        if (rt->declaration->isValueBuiltin) return false;
        if (rt->declaration->name != hit->resultHead) return false;
        return true;
    }
    // Declaration-less: read the syntactic head (alias-aware, #1462), which is
    // what `makeConcreteCollectionResult` stamps for the `Array<T>` results.
    if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
        return rt->headName() == hit->resultHead;
    return !rt->importedName.empty() && rt->importedName == hit->resultHead;
}

bool isBuiltinArrayRangeSliceIndex(const ast::IndexExpr& ix,
                                   const sema::SemaContext& ctx) {
    // See the header contract. // per §28.3 producers "Range slice" row, §15.6,
    // §21.1, v0.240, #2749 item 2
    if (!ix.target) return false;
    const sema::TypeRef* rt = ctx.exprType(*ix.target);
    // (a) The RECEIVER must be a declaration-less builtin `Array` head with a
    // generic argument — the exact shape sema's own range-slice arm requires
    // (`ExprTypecheck_Exprs.cpp`: `tt->kind == Named && !tt->declaration &&
    // tt->astNode && !tt->astNode->name->parts.empty()`, then
    // `cn == "Array"`). `declaration == nullptr` is what makes this BUILTIN-only:
    // a user type — including a user class that shadows the name `Array`, and
    // any user type with a range-taking indexer — carries a declaration and is
    // excluded here. `OwningArray<T>` is excluded by the head (§15.6 rejects
    // slicing on it outright, and that rejection stands on its own); so are the
    // span families, whose stdlib `Slice` overloads return span heads rather
    // than owned arrays and whose range index sema types as the ELEMENT.
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    if (rt->headName() != "Array") return false;
    if (rt->genericArgs.empty() || !rt->genericArgs[0]) return false;
    // (b) The index must be a RANGE, in either of the two forms sema accepts: a
    // syntactic range literal `arr[0..2]`, or a `Range`-typed value
    // `var r := 0..2; arr[r]` (#1079). A single-element read `arr[i]` is
    // §28.3's *indexer* row — a Borrow, which §21.1 admits verbatim
    // ("Admitted: […] and **borrows**") — and must keep compiling as a `const`
    // initializer.
    if (ix.indices.empty() || !ix.indices[0].value) return false;
    const ast::Expr* idx = ix.indices[0].value.get();
    if (!dynamic_cast<const ast::RangeExpr*>(idx)) {
        const sema::TypeRef* it = ctx.exprType(*idx);
        if (!it || it->kind != sema::TypeRef::Kind::Named || !it->declaration ||
            it->declaration->name != "Range")
            return false;
    }
    // (c) And sema must in fact have typed the whole index expression as the
    // RECEIVER'S OWN TypeRef object. WITHIN the receiver family (a) selects
    // — Named, declaration-less, head `Array` — that is the range-slice arm's
    // signature: it is the only arm there writing `tt` itself rather than a
    // derived TypeRef (`tt->genericArgs[0]` / `[1]`), which cannot alias `tt`.
    // ⚠ NOT unique across ALL IndexExpr arms: primitiveTypeRef is INTERNED, so
    // the string range-slice arm collides too — harmless only because (a)
    // rejects a Kind::Primitive receiver first. See the header.
    //
    // ★ The real work (c) does is NOT excluding a `Span<Array<int>>` element
    // read — (a) already gates on the RECEIVER's head, so a span receiver never
    // reaches (c). It is that sema tests the Range index by SYMBOL IDENTITY
    // (`it->declaration == builtinClassSym("Range")`, ExprTypecheck_Exprs.cpp
    // :1574) while (b) below tests by NAME STRING — so (b) is strictly WIDER
    // than sema's arm, and a USER type named `Range` passes (b) but not sema.
    // (c) catches exactly that, failing open. Load-bearing; the earlier stated
    // reason was wrong. It is a cross-check on (a)+(b), not a substitute: if sema ever
    // stops setting this arm's type by identity the predicate goes SILENT
    // (accepting), never over-wide.
    return ctx.exprType(ix) == rt;
}

bool isEventSubscribeCall(const ast::CallExpr& call,
                          const sema::SemaContext& ctx) {
    // See the header contract. // per §21.1 v0.286, stdlib §2.11 / §6.5, #3694
    auto [maTarget, mm] = builtinInstanceCallee(call.callee.get());
    if (!maTarget) return false;
    if (mm != "Subscribe") return false;
    // §6.5's signature is `Subscribe(handler)` — exactly one argument. The
    // codegen-side structural test (LlvmBackend_Calls2.cpp:3847) has the same
    // gate; an arity-0 or arity-2 spelling reaches a different stdlib surface
    // and is excluded here on the same basis. The `AsyncSubscribe` member
    // referenced in #3694's reproduction does not exist — the
    // async-event-handler path is the same `Subscribe` member with a
    // `(T) -> Task` handler type (stdlib §6.5, `IAsyncEvent<T>.Subscribe`'s
    // signature) — so the single `Subscribe` name covers both.
    if (call.args.size() != 1) return false;
    // The direct-event path: `evt.Subscribe(handler)` / `this.Clicked.Subscribe(...)`
    // — the EventSymbol-bearing shape (§24, the §17.2 desugaring of the
    // `+=` operator stays in `Emitter::isSubscriptionExpr`'s recognition,
    // which we mirror here for the §21.1 half). An event-member access on its
    // own is a `MemberAccessExpr` / `IdentifierExpr` carrying an `EventSymbol`
    // attached by sema to that expr.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
        if (ma->target &&
            dynamic_cast<sema::EventSymbol*>(ctx.exprSymbol(*ma->target)))
            return true;
    }
    // The stored-face / sealed-source path: `face.Subscribe(handler)` /
    // `src.Subscribe(handler)` where the receiver types as one of the six
    // compiler-known event-family heads. Same shape predicate codegen's
    // `Emitter::isIEventTypeName` / `isEventSourceTypeName` /
    // `isISharedEventTypeName` / `isSharedEventSourceTypeName` (the latter two
    // #1832 added for `SharedEventSource`; the same `SharedEventSource<T>`
    // pair produces `owned Subscription` and is therefore in scope here) use:
    // Named TypeRef, declaration-null, astNode name, head is in the set below.
    // The declaration-null gate is what excludes a user type that happens to
    // share the name `EventSource` / `IEvent` / etc.: such a type carries a
    // declaration and reaches the user-method authority instead. Per stdlib
    // §6.5 the `.Subscribe` member returns `owned Subscription` on every one
    // of these receivers.
    const sema::TypeRef* rt = ctx.exprType(*maTarget);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    const auto head = rt->headName();
    return head == "IEvent" || head == "IAsyncEvent" ||
           head == "ISharedEvent" || head == "EventSource" ||
           head == "AsyncEventSource" || head == "SharedEventSource";
}

// Recursively scan every `return EXPR;` reachable in statement `s` (the same
// statement forms that can carry a nested return as
// MethodOwnership::scanReturnsForWrapperReceiverBorrow, Ownership_Checks.cpp —
// a nested closure carries its body as an Expr, never spliced into the Stmt
// list, so it is structurally never descended into here). `sawReturn` records
// whether any return was seen at all; `allFresh` is cleared the moment a
// return whose value is not a fresh owned producer is found. Once `allFresh`
// is false the walk short-circuits. // fix #1499 (mirrors §28.3 callee scan)
void scanReturnsAllFreshOwned(const ast::Stmt* s, const sema::SemaContext& ctx,
                              const TypeParamResolver& resolveParam,
                              bool& sawReturn, bool& allFresh) {
    if (!s || !allFresh) return;
    if (const auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        sawReturn = true;
        // A borrow return (`return _shared;`, `return p;`, `return this;`,
        // `return this.f;`, `return alias;`) is NOT a fresh owned producer, so
        // it clears allFresh — the whole method is then not a fresh-owned
        // factory and the call stays a borrow (§28.10; the #1499 regression).
        if (!rs->value ||
            !isFreshOwnedClassProducer(rs->value.get(), ctx, resolveParam))
            allFresh = false;
        return;
    }
    if (const auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
        for (auto& b : is->ifBranch.body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        for (auto& br : is->elifBranches)
            for (auto& b : br.body)
                scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                         allFresh);
        for (auto& b : is->elseBody)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        for (auto& b : ws->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
        for (auto& b : fs->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
        for (auto& b : d->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
        for (auto& b : ts->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        for (auto& c : ts->catches)
            scanReturnsAllFreshOwned(c.body.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        for (auto& b : ts->finallyBody)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
        for (auto& arm : cs->arms)
            for (auto& a : arm.stmts)
                scanReturnsAllFreshOwned(a.get(), ctx, resolveParam, sawReturn,
                                         allFresh);
        return;
    }
    // ExpressionStmt / AssignmentStmt / DiscardStmt / YieldStmt (iterator
    // methods are handled by the for-loop desugaring, not this rule) /
    // IncrementStmt / BreakStmt / ContinueStmt / ThrowStmt / LocalDecl: none
    // can carry a nested `return` of the enclosing method.
}

bool staticFactoryReturnsFreshOwned(const ast::MethodDecl& md,
                                    const sema::TypeRef* resultTy,
                                    const sema::SemaContext& ctx,
                                    const TypeParamResolver& resolveParam) {
    // (1) STATIC only. §28.3: an INSTANCE method's non-`owned` class return has
    // a structural source (the receiver), so it is a borrow — never covered by
    // the sourceless-return rule.
    bool isStatic = false;
    for (std::string_view mod : md.modifiers)
        if (mod == "static") { isStatic = true; break; }
    if (!isStatic) return false;
    // (2) The resolved result must be a plain class/interface. A value-builtin
    // (DateTime/Guid/TimeSpan/…) carries kind==Class but has no disposal
    // obligation (mirrors the sibling isClassOrInterfaceResult guards).
    if (!resultTy || resultTy->kind != sema::TypeRef::Kind::Named ||
        !resultTy->declaration)
        return false;
    if (resultTy->declaration->isValueBuiltin) return false;
    if (resultTy->declaration->kind != ast::TypeDeclKind::Class &&
        resultTy->declaration->kind != ast::TypeDeclKind::Interface)
        return false;
    // §28.6 / §5.11: an owning-collection / owning-wrapper name-head result
    // carries its OWN, more specific documented-source borrow assumption (the
    // `Owning*` name denotes ELEMENT ownership, orthogonal to the container
    // `owned` qualifier). Excluded here so the two rules never fight over the
    // same call; a fresh owning value must be returned through an `owned`
    // return type (§28.3 borrow-lifetime rules), enforced at the definition.
    if (isOwningTypeHead(resultTy->headName())) return false;
    // (3) The DISTINGUISHING check (the #1499 soundness core): classify owned
    // ONLY when EVERY return path produces a fresh owned value. A borrow on any
    // path (a field read like `_shared`, a parameter, `this`, a plain alias)
    // leaves the call a borrow — never disposed by the caller (§28.10). An
    // abstract/iterator/no-return body (sawReturn stays false) is likewise not
    // a fresh-owned factory.
    bool sawReturn = false;
    bool allFresh = true;
    if (md.bodyKind == ast::MethodBodyKind::Expression) {
        sawReturn = md.exprBody != nullptr;
        allFresh = md.exprBody &&
                   isFreshOwnedClassProducer(md.exprBody.get(), ctx,
                                             resolveParam);
    } else {
        for (auto& s : md.stmts)
            scanReturnsAllFreshOwned(s.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
    }
    return sawReturn && allFresh;
}

// fix #4360 (§28.3, v0.298): true if `e` is `self` or a chain from `self`
// (e.g., `self`, `self.field`, `self.field.nested`). Used to exclude
// instance methods whose return has a structural borrow relationship to the
// receiver — such a return is a borrow, never an instance factory.
// Only bare `this` is self-derived. `this.field` reads (even of owned fields)
// are NOT self-derived: reading an owned field is a move of the owned value,
// transferring ownership to the caller — the method IS a factory.
namespace {
bool isSelfDerived(const ast::Expr* e) {
    if (!e) return false;
    // `this` (bare IdentifierExpr) is self-derived — returning the receiver
    // itself transfers no new allocation; the caller already has the object.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
        return id->name == "this";
    // `this.field` / `this.field.nested` reads are NOT self-derived:
    // reading an owned field moves the owned value out (ownership transfer),
    // reading a borrow field copies the borrow reference — in either case the
    // method can be a factory (caller receives owned or borrowed from the
    // read, not from self directly).
    return false;
}

// Scans all return statements in `s` and returns true if any return is
// self-derived. Mirrors scanReturnsAllFreshOwned's statement-type coverage.
bool anyReturnIsSelfDerived(const ast::Stmt* s) {
    if (!s) return false;
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        return isSelfDerived(rs->value.get());
    }
    // Descend using the same statement types scanReturnsAllFreshOwned covers.
    if (const auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
        for (auto& b : is->ifBranch.body)
            if (anyReturnIsSelfDerived(b.get())) return true;
        for (auto& br : is->elifBranches)
            for (auto& b : br.body)
                if (anyReturnIsSelfDerived(b.get())) return true;
        for (auto& b : is->elseBody)
            if (anyReturnIsSelfDerived(b.get())) return true;
        return false;
    }
    if (const auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        for (auto& b : ws->body)
            if (anyReturnIsSelfDerived(b.get())) return true;
        return false;
    }
    if (const auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
        for (auto& b : fs->body)
            if (anyReturnIsSelfDerived(b.get())) return true;
        return false;
    }
    if (const auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
        for (auto& b : d->body)
            if (anyReturnIsSelfDerived(b.get())) return true;
        return false;
    }
    if (const auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
        for (auto& b : ts->body)
            if (anyReturnIsSelfDerived(b.get())) return true;
        for (auto& c : ts->catches)
            if (anyReturnIsSelfDerived(c.body.get())) return true;
        for (auto& b : ts->finallyBody)
            if (anyReturnIsSelfDerived(b.get())) return true;
        return false;
    }
    if (const auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
        for (auto& arm : cs->arms)
            for (auto& a : arm.stmts)
                if (anyReturnIsSelfDerived(a.get())) return true;
        return false;
    }
    return false;
}
}  // namespace

bool instanceFactoryReturnsFreshOwned(const ast::MethodDecl& md,
                                    const sema::TypeRef* resultTy,
                                    const sema::SemaContext& ctx,
                                    const TypeParamResolver& resolveParam) {
    // (1) INSTANCE only (non-static). Static methods are handled by
    // staticFactoryReturnsFreshOwned.
    bool isStatic = false;
    for (std::string_view mod : md.modifiers)
        if (mod == "static") { isStatic = true; break; }
    if (isStatic) return false;
    // (2) The resolved result must be a plain class/interface. A value-builtin
    // carries no disposal obligation; an owning-wrapper/owning-collection head
    // is excluded separately below.
    if (!resultTy || resultTy->kind != sema::TypeRef::Kind::Named ||
        !resultTy->declaration)
        return false;
    if (resultTy->declaration->isValueBuiltin) return false;
    if (resultTy->declaration->kind != ast::TypeDeclKind::Class &&
        resultTy->declaration->kind != ast::TypeDeclKind::Interface)
        return false;
    if (isOwningTypeHead(resultTy->headName())) return false;
    // (3) Every return path must produce a fresh owned value, AND none of
    // the returns may be self-derived. A self-derived return
    // (`return self;`, `return self.field;`) has the receiver as a
    // structural source and is a borrow — the caller already has a borrow of
    // the same object, so treating the call as owned would be a double-free.
    bool sawReturn = false;
    bool allFresh = true;
    bool anySelfDerived = false;
    if (md.bodyKind == ast::MethodBodyKind::Expression) {
        sawReturn = md.exprBody != nullptr;
        allFresh = md.exprBody &&
                   isFreshOwnedClassProducer(md.exprBody.get(), ctx,
                                             resolveParam);
        anySelfDerived = isSelfDerived(md.exprBody.get());
    } else {
        for (auto& s : md.stmts) {
            scanReturnsAllFreshOwned(s.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
            if (anyReturnIsSelfDerived(s.get())) anySelfDerived = true;
        }
    }
    return sawReturn && allFresh && !anySelfDerived;
}

namespace {

// The three TypeRef name fallbacks shared by the builtin-type matchers
// (mirrors MethodOwnership's own `builtinTypeRefNameHead`,
// Ownership_Checks.cpp — that copy is a private `namespace {}`-local helper
// with no free-function form to share, so this scoped duplicate exists only
// so `isBorrowBearingRecordOrSpan` below can name a Named TypeRef's builtin
// head without depending on the ownership pass, which is a layer ABOVE
// common/): builtin TypeSymbol declaration name, annotation AST name,
// imported name.
std::string_view borrowSpanNameHead(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return {};
    if (rt->declaration) return rt->declaration->name;
    if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
        return rt->headName();
    return rt->importedName;
}

// Mirrors MethodOwnership::isBorrowableType (Ownership_Checks.cpp), used here
// only for the InlineArray element-type leg of the record-field
// classification (`recordFieldTypeIsBorrowCarryingCommon`, below) — true when
// `rt` is a class/interface reference (the only element kind an inline-array
// field can hold that itself carries a borrow).
bool isReferenceTypeForRecordField(const sema::TypeRef* rt) {
    if (!rt) return true;  // unknown → conservative
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
        case sema::TypeRef::Kind::Function:
            return false;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // generic param
            if (rt->declaration->isValueBuiltin) return false;
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        case sema::TypeRef::Kind::Error:
            return true;  // unresolved → conservative
        default:
            return false;  // Unit, InlineArray, enum, record
    }
}

// §9.7: true when `rt` is itself a borrow-bearing record (recursion target of
// the field walk below) — mirrors MethodOwnership::isBorrowBearingRecordImpl
// (Ownership_Checks.cpp) exactly, so the declaration-site and instantiation-
// site checks classify identically (#1595). `seen` is the #1611 cycle guard —
// the set of TypeRefs on the CURRENT DFS path (inserted on entry, erased on
// exit), keyed on the visited TypeRef (not the TypeDecl: two distinct
// INSTANTIATIONS of one generic record on a chain —
// `Wrap<Wrap<ArraySpan<int>>>`, #2145 — are distinct classification subjects,
// while a genuinely cyclic chain revisits the SAME cached field TypeRef,
// `SemaContext::resolvedType` being per-node, so termination is preserved;
// path — not visited-set — semantics so one field TypeRef shared by SIBLING
// frames re-classifies under each frame's own env, the #2145 diamond). `env`
// is the #2145 instantiation-site substitution environment for the record's
// bare-parameter fields; null reproduces the declaration-site behaviour.
// `choiceSeen` (#2159) is the shared cycle guard for the record↔choice mutual
// recursion §9.7 transitivity opens (a record field of choice type consults the
// choice classifier, whose payloads may recurse back into a record): the set of
// choice TypeRef INSTANTIATIONS on the CURRENT classification path. Null at a
// pure-record entry point (no choice frame open yet).
//
// fix #3144 (family F9 of #3152): the guard's key must be an identity both an
// imported and a local choice have. An IMPORTED choice has no `ast::TypeDecl`
// at all, so a TypeDecl-keyed path set could not even represent it — every
// imported frame would have hashed to the same null key and the guard would
// have fired on the FIRST nested imported choice, silently reporting it
// value-like.
//
// fix #3933 (§35.11/§35.4, v0.295): the key was `sema::TypeSymbol*` — the bare
// generic DECLARATION, shared by every instantiation of one generic choice
// (`Pair<int>`, `Pair<Box>`, `Pair<Pair<Box>>` all point at the same `Pair`
// TypeSymbol). Classifying `Pair<Pair<Box>>` inserts `Pair`'s decl, then
// recurses into the INNER `Pair<Box>` instantiation while still on that
// stack frame — same decl, so the guard fired as a false "self-referential
// cycle" for a legitimate, non-recursive nesting at a DIFFERENT type
// argument. Re-keyed on the `sema::TypeRef*` instantiation itself (the `rt`
// each recursive call already carries, exactly the identity
// `isBorrowBearingRecordCommon`'s own `seen` guard below uses for the same
// record↔record problem, #2145) — two different instantiations of the same
// generic choice no longer collide, while a genuine same-instantiation cycle
// (independently rejected by §35.1/§9's no-self-containment rule; this guard
// only defends against it) still terminates.
bool isBorrowBearingRecordCommon(
    const sema::TypeRef* rt, sema::SemaContext& ctx,
    std::unordered_set<const sema::TypeRef*>& seen, const RecordSubstEnv* env,
    std::unordered_set<const sema::TypeRef*>* choiceSeen = nullptr);

// The §35.4 choice-classification core (defined below near classifyChoiceType),
// forward-declared here so the record field walk can consult it for a Choice-
// kind field (§9.7 transitivity, #2159). Shares the `choiceSeen` path set so the
// record↔choice mutual recursion terminates.
//
// `memo` (fix #4140+#4125+#4145+#4112, family J §2.6, v0.298): an OPTIONAL
// per-`classifyChoiceType`-call classification cache keyed on the
// INSTANTIATED `TypeRef*` (not the bare `TypeSymbol*`). Lookup happens BEFORE
// the cycle guard — a fully-classified `rt` from earlier in the same call
// returns its cached verdict without re-walking its cases. The cache
// collapses the residual ~N^2 of #4145's deep linear nesting to O(N)
// (every nested choice is classified at most once per call) and is the
// structural fix for #4112's masked bug (the unmarked-payload walk now
// reaches the §28.1 erasure oracle via the delegation below, and the
// cache prevents the exponential re-walk the broken-by-design delegation
// would otherwise produce). The cycle guard (insert on entry, erase on
// exit, return `ValueLike` on cycle) is the SEPARATE mechanism that bounds
// the recursion on self-/mutually-referential types — it catches the
// in-progress case the cache cannot, because the cache only stores FINAL
// verdicts, never the cycle guard's `ValueLike`.  // per §35.1, §35.4, §28.1
std::optional<ChoiceClassification> classifyChoiceCore(
    const sema::TypeRef* rt, sema::SemaContext& ctx,
    const RecordSubstEnv* env,
    std::unordered_set<const sema::TypeRef*>& choiceSeen,
    std::unordered_map<const sema::TypeRef*, ChoiceClassification>* memo =
        nullptr);

// §28.1 (v0.226, #2130): the file-local `owned`-marker erasure oracle impl,
// forward-declared here so classifyChoiceCore's owning leg can thread its
// in-flight `choiceSeen` AND its per-call `memo` (family J §2.6, v0.298)
// through the choice↔choice recursion an `owned`-typed payload opens.
// Defined alongside classifyChoiceCore below. The public
// `ownedMarkerActiveOnInstantiatedType` entry seeds a fresh `choiceSeen`
// and passes a null `memo` (its queries are isolated, not nested inside
// a `classifyChoiceType` call).
bool ownedMarkerActiveOnInstantiatedTypeImpl(
    const sema::TypeRef* ft, sema::SemaContext& ctx,
    std::unordered_set<const sema::TypeRef*>& choiceSeen,
    std::unordered_map<const sema::TypeRef*, ChoiceClassification>* memo =
        nullptr);

// §9.7's borrow-carrying-field kinds, applied to one resolved field type in
// the substitution environment `env` (which governs `ft`'s OWN bare-parameter
// mentions). Split out of the walker (from the former lambda) because a
// substituted field type must be re-classified in the environment carried by
// its substitution entry, not the visiting record's. // per §9.7, #2145
bool recordFieldTypeIsBorrowCarryingCommon(
    const sema::TypeRef* ft, sema::SemaContext& ctx,
    std::unordered_set<const sema::TypeRef*>& seen, const RecordSubstEnv* env,
    std::unordered_set<const sema::TypeRef*>* choiceSeen = nullptr) {
    if (!ft) return false;
    // §9.7 transitivity / §4.14 / §4.15 (#2127): a builtin span VIEW field
    // (Span / ReadOnlySpan / ArraySpan / ReadOnlyArraySpan) carries a borrow
    // of its backing Array / OwningBuffer but has no user Record TypeDecl, so
    // neither the isValueBuiltin/null-declaration bail nor the user-Record
    // recurse below fires for it — recognize it by name-head (mirrors
    // MethodOwnership::isBuiltinSpanTypeRef, Ownership_Checks.cpp, so the
    // declaration-site and instantiation-site checks classify identically,
    // #1595). // per §9.7
    {
        const std::string_view sn = borrowSpanNameHead(ft);
        if (sn == "Span" || sn == "ReadOnlySpan" ||
            sn == "ArraySpan" || sn == "ReadOnlyArraySpan")
            return true;
    }
    switch (ft->kind) {
        case sema::TypeRef::Kind::Named: {
            if (!ft->declaration) {
                // §9.7 / §30.1 (#2145): a bare type-parameter field of a
                // generic record — resolve through the instantiation-site
                // environment and classify the SUBSTITUTED type in its own
                // environment. Unresolvable (no env / uninstantiated / a
                // null-declaration builtin head) stays not-borrow-carrying,
                // the declaration-site conservative reading (§30.1 defers
                // those to the instantiation-site re-check).
                const RecordSubstEnv::Entry sub =
                    resolveRecordFieldSubst(ft, env);
                if (sub.ty && sub.ty != ft)
                    return recordFieldTypeIsBorrowCarryingCommon(
                        sub.ty, ctx, seen, sub.env, choiceSeen);
                return false;  // generic param (uninstantiated)
            }
            if (ft->declaration->isValueBuiltin) return false;
            if (ft->declaration->kind == ast::TypeDeclKind::Class ||
                ft->declaration->kind == ast::TypeDeclKind::Interface)
                return true;
            if (ft->declaration->kind == ast::TypeDeclKind::Record)
                return isBorrowBearingRecordCommon(ft, ctx, seen, env,
                                                   choiceSeen);
            // §9.7 transitivity (v0.226, #2159): "Its type is another borrow-
            // bearing record, or a borrow-bearing choice (§35.4) (transitivity)."
            // A CHOICE-kind field makes the record borrow-bearing iff that choice
            // is borrow-bearing AT THIS instantiation — consult the §35.4
            // classifier, threading the record's env (so a generic choice field
            // whose args are the record's parameters — `Holder<T>(K: Opt<T>)` —
            // resolves through the record's substitution) and the shared
            // `choiceSeen` path set (so the record↔choice mutual recursion this
            // leg opens terminates). An OWNING choice payload cannot appear in a
            // record (§9.7 records have no owned fields; §15.3/§35.4 owning
            // choices are move-only and may not be a record field — rejected
            // separately), so only the borrow-bearing verdict matters here.
            if (ft->declaration->kind == ast::TypeDeclKind::Choice) {
                std::unordered_set<const sema::TypeRef*> localChoiceSeen;
                auto& cs = choiceSeen ? *choiceSeen : localChoiceSeen;
                auto cls = classifyChoiceCore(ft, ctx, env, cs);
                return cls && *cls == ChoiceClassification::BorrowBearing;
            }
            return false;  // enum
        }
        case sema::TypeRef::Kind::InlineArray: {
            if (!ft->arrayElement || ft->arrayOwned) return false;
            // §9.7: "an inline array whose element type is a reference type" —
            // resolve a bare-parameter ELEMENT through the environment first
            // (`[N]T` at `T := C` is an inline array of references). // #2145
            const sema::TypeRef* el =
                resolveRecordFieldSubst(ft->arrayElement, env).ty;
            return isReferenceTypeForRecordField(el ? el : ft->arrayElement);
        }
        default:
            return false;  // Primitive, Unit, Function, Error
    }
}

bool isBorrowBearingRecordCommon(
    const sema::TypeRef* rt, sema::SemaContext& ctx,
    std::unordered_set<const sema::TypeRef*>& seen, const RecordSubstEnv* env,
    std::unordered_set<const sema::TypeRef*>* choiceSeen) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    // fix #3145 (§2.8.3 rule 4, v0.252): the record test and the field walk both
    // come off the TypeSymbol. This used to cast `rt->declaration->decl` to an
    // `ast::TypeDecl` and walk THAT — null for every imported type, so the walk
    // ran over no fields at all and the function returned its empty-walk default,
    // "not borrow-bearing". §9.7's dangling-borrow REJECT is built on this
    // predicate, so it silently did not fire across a module boundary: an
    // accepted program that the spec requires be rejected. Measured — the same
    // `new Holder(new Res(1), 7)` is rejected when `Holder` is local and compiles
    // clean when it is imported.
    const sema::TypeSymbol* ts = rt->declaration;
    if (ts->kind != ast::TypeDeclKind::Record) return false;
    if (!seen.insert(rt).second) return false;

    // §9.7 / §30.1 (#2145): this instantiation's substitution environment for
    // the field walk below (empty → declaration-site behaviour). The symbol
    // overload reads `TypeSymbol::genericParams`, which carries the parameter
    // names on both sides.
    RecordSubstEnv myEnv;
    seedRecordSubstEnv(*ts, *rt, env, myEnv);
    const RecordSubstEnv* fieldEnv = myEnv.map.empty() ? nullptr : &myEnv;

    // §9.1 positional fields and §9.2 body fields in one walk — they are all
    // FieldSymbols on both sides, and `static` is excluded inside the helper
    // (positional fields are never static, so this is exactly the old pair of
    // loops). The field's declared type comes from `storageComponentType`, the
    // one accessor that consults the positional Param, then the body FieldDecl,
    // then the importer's side table.
    const bool carrying = sema::anyRecordStorageField(
        ts, ctx, [&](const sema::Symbol*, const sema::TypeRef* ft) {
            return ft && recordFieldTypeIsBorrowCarryingCommon(
                             ft, ctx, seen, fieldEnv, choiceSeen);
        });
    // PATH semantics, not visited-set memoization: un-mark on exit so a
    // SIBLING field can re-classify the same field TypeRef under its OWN
    // substitution env — one shared `NRec<X>` node reached through both
    // `PairR<int>` and `PairR<Res>` frames must answer per-frame, or the
    // benign-first walk vacuously classifies the borrow-carrying sibling
    // (#2145 diamond). Termination is unaffected: a genuinely cyclic chain
    // still repeats a TypeRef on the CURRENT path and is cut above.
    // // per §9.7, #1611, #2145
    seen.erase(rt);
    return carrying;
}

// §28.3 (v0.194, #1801) helpers for the bare-type-parameter source set. Kept in
// this shared core so the ownership pass reaches ONE copy.

// Mirrors MethodOwnership::isBorrowableType (Ownership_Checks.cpp): true when a
// resolved argument type is a borrow-bearing class/interface reference. A
// value type, a value-builtin, a function value, `string`/`Object`
// (runtime-managed primitives), and a bare generic-param arg type (null
// declaration) are NOT borrowable — such an argument carries no lifetime
// constraint into the result (§28.3: "any argument … whose substituted type is
// a value or runtime-managed type[] contributes no constraint"). Unknown /
// unresolved → conservative (borrowable, so the source is kept).
bool bareArgIsBorrowable(const sema::TypeRef* rt) {
    if (!rt) return true;  // unknown → conservative
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
        case sema::TypeRef::Kind::Function:
            return false;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // bare generic param
            if (rt->declaration->isValueBuiltin) return false;
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        case sema::TypeRef::Kind::Error:
            return true;  // unresolved → conservative
        default:
            return false;  // Unit, InlineArray, enum, record
    }
}

// The identifier root of an access chain (`buf.f[i].g` → `buf`, a bare `a` →
// `a`) resolved to its symbol — mirrors MethodOwnership::identifierRoot +
// ctx.exprSymbol, replicated here (that peel is an ownership-pass member, a
// layer above common/). Null for a non-identifier-rooted expression.
sema::Symbol* bareRootSymbolOf(const ast::Expr& e, const sema::SemaContext& ctx) {
    const ast::Expr* cur = &e;
    while (cur) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(cur))
            return ctx.exprSymbol(*id);
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(cur))
            cur = ma->target.get();
        else if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(cur))
            cur = nm->target.get();
        else if (auto* c = dynamic_cast<const ast::CallExpr*>(cur))
            cur = c->callee.get();
        else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(cur))
            cur = ix->target.get();
        else
            return nullptr;
    }
    return nullptr;
}

// §28.3: does the declared parameter type `t` MENTION the returned type
// parameter `name`? A bare `T`, and any nesting that reaches it (`List<T>`,
// `[N]T`, `(T) -> ()`), qualifies — so an argument that carries `T` as an
// element/component is a source of the returned `T`.
bool bareTypeMentionsParam(const ast::Type& t, std::string_view name) {
    if (t.kind == ast::TypeKind::Named && t.name &&
        t.name->parts.size() == 1 && t.name->parts[0] == name)
        return true;
    for (const auto& ga : t.genericArgs)
        if (ga && bareTypeMentionsParam(*ga, name)) return true;
    if (t.arrayElement && bareTypeMentionsParam(*t.arrayElement, name))
        return true;
    if (t.funcReturn && bareTypeMentionsParam(*t.funcReturn, name)) return true;
    for (const auto& fp : t.funcParams)
        if (fp.type && bareTypeMentionsParam(*fp.type, name)) return true;
    return false;
}

// The single-part head name of a method's declared return type, or empty.
std::string_view bareReturnHead(const ast::MethodDecl& md) {
    if (md.returnType && md.returnType->name &&
        md.returnType->name->parts.size() == 1)
        return md.returnType->name->parts[0];
    return {};
}

}  // namespace

// fix #3145 (§9.7 / §30.1 / §2.8.3 rule 4, v0.252): the SYMBOL-side seeding.
// `TypeSymbol::genericParams` carries the parameter names on both sides (they
// are `GenericParamSymbol`s, whose `name` is the Symbol's own), so this needs no
// AST and no new metadata — the audit's family F6 records the same for the other
// 22 generic-parameter readers. Semantics are otherwise identical to the AST
// overload below, including the arity-mismatch behaviour.
void seedRecordSubstEnv(const sema::TypeSymbol& ts, const sema::TypeRef& rt,
                        const RecordSubstEnv* incoming, RecordSubstEnv& out) {
    if (ts.genericParams.empty() || rt.genericArgs.empty()) return;
    const std::size_t n =
        std::min(ts.genericParams.size(), rt.genericArgs.size());
    for (std::size_t i = 0; i < n; ++i) {
        const sema::TypeRef* arg = rt.genericArgs[i];
        const sema::Symbol* gp = ts.genericParams[i];
        if (!arg || !gp) continue;
        out.map.emplace(gp->name, resolveRecordFieldSubst(arg, incoming));
    }
}

void seedRecordSubstEnv(const ast::TypeDecl& decl, const sema::TypeRef& rt,
                        const RecordSubstEnv* incoming, RecordSubstEnv& out) {
    // per §9.7 / §30.1 (#2145) — see the header contract. Arity mismatch (an
    // ill-formed instantiation under error recovery) seeds only the pairable
    // prefix; the unseeded params classify as uninstantiated (conservative).
    if (decl.genericParams.empty() || rt.genericArgs.empty()) return;
    const std::size_t n =
        std::min(decl.genericParams.size(), rt.genericArgs.size());
    for (std::size_t i = 0; i < n; ++i) {
        const sema::TypeRef* arg = rt.genericArgs[i];
        if (!arg) continue;
        // An argument that is itself a bare parameter of the ENCLOSING frame
        // (`Inner<T>` inside `Outer<T>`'s body) resolves through `incoming`
        // now, so the entry always carries the outermost known substitution.
        out.map.emplace(decl.genericParams[i].name,
                        resolveRecordFieldSubst(arg, incoming));
    }
}

RecordSubstEnv::Entry resolveRecordFieldSubst(const sema::TypeRef* ft,
                                              const RecordSubstEnv* env) {
    // per §9.7 / §30.1 (#2145) — see the header contract. A bare parameter is
    // a declaration-less, argument-less Named TypeRef (mirrors
    // ExprTypecheck::substituteReturnType's bare-param recognizer); whether
    // its head is a PARAM (vs a null-declaration builtin such as `Array`) is
    // decided by the env lookup itself — only in-scope param names are keys.
    if (ft && env && ft->kind == sema::TypeRef::Kind::Named &&
        !ft->declaration && ft->genericArgs.empty()) {
        auto it = env->map.find(borrowSpanNameHead(ft));
        if (it != env->map.end() && it->second.ty) return it->second;
    }
    return {ft, env};
}

bool isBorrowBearingRecordOrSpan(const sema::TypeRef* rt,
                                 sema::SemaContext& ctx) {
    if (!rt) return false;
    // Builtin span VIEWS carry a borrow of their backing Array/OwningBuffer
    // but have no user Record TypeDecl, so the record walk below misses them.
    // CancellationToken (§30.8) is a value builtin with no Record decl and is
    // not in this list — correctly not flagged; likewise the string-backed
    // spans (runtime-managed `string`, §9.7).
    const std::string_view n = borrowSpanNameHead(rt);
    if (n == "Span" || n == "ReadOnlySpan" ||
        n == "ArraySpan" || n == "ReadOnlyArraySpan")
        return true;
    std::unordered_set<const sema::TypeRef*> seen;
    return isBorrowBearingRecordCommon(rt, ctx, seen, /*env=*/nullptr);
}

// §4.16 + §9.7 (v0.307, issue #4751): classify a TypeRef as the
// `IReadOnlySpan<TElement>` interface. The interface is an alias-target
// for the borrowing span family (Span / ReadOnlySpan / ArraySpan /
// ReadOnlyArraySpan); an alias declaration (`type Al := IReadOnlySpan<int>`)
// must read the same as the bare interface, mirroring `isIReadOnlySpanType`
// in `LlvmBackend_impl.hpp` (#4583 alias-blind read). The branch checks
// `declaration` first (the §27.8 alias-resolution result) and falls through
// to `astNode->name->parts[0]` only when `declaration` is null (the
// not-yet-resolved shape). Both legs read identically for a bare
// `IReadOnlySpan<int>` (the alias-tail is read off `headName()` after
// resolution), so a non-aliased program reaches the `declaration` leg;
// aliased programs reach the `astNode` leg until resolution populates
// `declaration`. The spelling "IReadOnlySpan" is reserved for the stdlib's
// read-only span interface (§4.16) — no user code can name its own type
// with that head.
bool isIReadOnlySpanType(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    if (rt->declaration && rt->declaration->name == "IReadOnlySpan")
        return true;
    if (rt->astNode && rt->astNode->name &&
        !rt->astNode->name->parts.empty() &&
        rt->astNode->name->parts[0] == "IReadOnlySpan")
        return true;
    return false;
}

// §35.4 (v0.200, #2045): the choice classification — see the header comment
// on `classifyChoiceType` for the full contract. The file-local `classifyChoice
// Core` recursion helper takes the cycle-guard `choiceSeen` set the public
// entrypoint seeds — a `const ast::TypeDecl*` PATH set over the CHOICE decls on
// the current classification chain (insert on entry, erase on exit). Since §9.7
// transitivity makes the record field walk consult the choice classifier for a
// Choice-kind field (#2159), that same `choiceSeen` set is threaded THROUGH the
// record walk (as its `choiceSeen` parameter) so the record↔choice mutual
// recursion terminates on a value cycle (already rejected by §35.1/§9, defended
// here). The record walk keeps its own TypeRef-keyed guard for record-record
// cycles.
namespace {

// §35.4 / §15.3: a payload field's resolved type is a MOVE-ONLY VALUE TYPE
// — an owning inline array (`[N owned]T`) or another owning choice (reached
// transitively). Owns cases is what makes a choice owning; value types that
// are NOT move-only (records, value-like choices, enums, primitives) do not.
//
// fix #4140+#4125+#4112 (family J §2.6, v0.298): the choice arm DELEGATES
// to `classifyChoiceCore` rather than re-walking the nested choice's cases
// by hand. The two reasons are separate:
//
//   (i) §28.1 erasure oracle application (#4112, the still-reproducing
//       member). The hand-rolled walk below used to check
//       `pf.modifier == "owned"` unconditionally and return `true` — but
//       §28.1 erases the marker at value-type instantiations, so a
//       `D<int>` reached through an unmarked payload of an enclosing
//       choice wrongly classified as Owning when reached DIRECTLY it
//       classified as ValueLike (same choice, same instantiation, two
//       different answers). Delegating to `classifyChoiceCore` routes
//       the decision through `ownedMarkerActiveOnInstantiatedTypeImpl` /
//       `substitutedChoicePayloadType`, the SAME machinery the direct
//       classify path uses, so the two cannot disagree.
//
//   (ii) `choiceSeen` threading (#4140). The pre-fix code seeded a fresh
//        `nestedChoiceSeen` for every recursive classifyChoiceCore call,
//        severing the path-guard chain the surrounding classifyChoiceCore
//        call was maintaining. So a choice reached through this unmarked-
//        payload leg could not detect it was already being classified on
//        the current path, and a mutually-recursive UNMARKED choice pair
//        recursed without limit (78,510 frames in the #4140 stack). The
//        fix threads the AMBIENT `choiceSeen` — mirroring what
//        `payloadFieldTypeIsBorrowCarrying`'s own Choice-kind arm
//        (two paragraphs below) already does — so a re-entry on the
//        current path terminates via the cycle guard with `ValueLike`.
//
// `seen` keys on the INSTANTIATION (`const sema::TypeRef*`, fix #4125,
// mirroring #3933's same re-keying for `classifyChoiceCore`'s own
// `choiceSeen` guard). The bare-declaration keying used to collide `D<int>`
// and `D<Widget>` in a SINGLE sibling-loop guard, so the first sibling
// erased the second's verdict regardless of order (§3.6 rule 1 forbids).
//
// `memo` is the optional per-call classification cache on `classifyChoiceCore`.
// Threaded through so the delegated classify benefits from the same memo
// (and the cache records the verdict of THIS classify through the same
// path). Without it the delegation would re-classify the same nested
// choice at every distinct enclosing site — the structural source of the
// pre-#4125 exponential #4145. // per §35.1, §35.4, §28.1, v0.298
bool payloadFieldTypeIsMoveOnly(
    const sema::TypeRef* ft,
    sema::SemaContext& ctx,
    std::unordered_set<const sema::TypeRef*>& seen,
    std::unordered_set<const sema::TypeRef*>& choiceSeen,
    std::unordered_map<const sema::TypeRef*, ChoiceClassification>* memo =
        nullptr) {
    if (!ft) return false;
    // §15.3: an owning inline array is a move-only value type. We do NOT
    // require `arraySizeValue >= 0` here (unlike MethodOwnership::
    // isMoveOnlyInlineArrayType) — the size-fold check is the ownership
    // pass's slot-allocation concern, not the type-property classification.
    // A payload declared `[N owned]T` is owning regardless of whether N
    // folds yet; if N never folds the ownership pass rejects the program
    // separately.
    if (ft->kind == sema::TypeRef::Kind::InlineArray && ft->arrayOwned)
        return true;
    if (ft->kind != sema::TypeRef::Kind::Named || !ft->declaration) return false;
    if (ft->declaration->kind != ast::TypeDeclKind::Choice) return false;
    // fix #4140: delegate to the shared, per-instantiation-aware core
    // (which already threads `choiceSeen` and consults the §28.1 erasure
    // oracle through `substitutedChoicePayloadType` /
    // `ownedMarkerActiveOnInstantiatedTypeImpl`). Passing the AMBIENT
    // `choiceSeen` (NOT a fresh `nestedChoiceSeen`) is what closes the
    // pre-#4140 stack-overflow — the re-entry on the current path
    // terminates in the cycle guard at `classifyChoiceCore`'s head with
    // `ValueLike`, exactly as the existing `payloadFieldTypeIsBorrowCarrying`
    // Choice arm already does for the borrow leg.
    //
    // fix #3144 (family F9 of #3152): the case walk in
    // `classifyChoiceCore` is SYMBOL-side, so a nested IMPORTED owning-choice
    // payload correctly classifies via the exported payload list — the old
    // hand-rolled `dynamic_cast<const ast::TypeDecl*>` walk here answered
    // "not move-only" for every imported choice and broke disposal.
    if (!seen.insert(ft).second) return false;
    auto verdict = classifyChoiceCore(ft, ctx, /*env=*/nullptr, choiceSeen, memo);
    return verdict && *verdict == ChoiceClassification::Owning;
}

// §35.4 / §9.7: a payload field's resolved type is BORROW-CARRYING under
// §9.7's definition (verbatim with "record" read as "record or choice" per
// §35.4): a plain (unmarked) class/interface reference field (nullable
// or not); an inline array of reference-typed elements; a borrow-bearing
// record; or transitively a borrow-bearing choice. The caller has already
// checked the field is NOT `owned`-marked (owning is decided first).
//
// `memo` (fix #4140+#4125+#4145+#4112, family J §2.6, v0.298): threaded
// through the Choice arm's delegated `classifyChoiceCore` so the borrow
// leg benefits from the SAME per-call classification cache the owning
// leg now consults. Together they collapse the deep-linear-chain case
// (#4145) from ~N^2 classifyChoiceCore invocations to O(N), and stop
// the unmarked-payload walk from re-classifying the same nested choice
// at every distinct enclosing site.
bool payloadFieldTypeIsBorrowCarrying(
    const sema::TypeRef* ft,
    sema::SemaContext& ctx,
    std::unordered_set<const sema::TypeRef*>& choiceSeen,
    const RecordSubstEnv* env,
    std::unordered_map<const sema::TypeRef*, ChoiceClassification>* memo =
        nullptr) {
    if (!ft) return false;
    switch (ft->kind) {
        case sema::TypeRef::Kind::Named: {
            if (!ft->declaration) {
                // §35.11 / §9.7 / §30.1 (v0.226, #2183): a bare (unmarked)
                // type-parameter payload — resolve through the instantiation
                // env and classify the SUBSTITUTED type in its own env, exactly
                // as the §9.7 record field walk does (#2145). `Opt<int>` → int
                // (a value builtin, not borrow), `Opt<ArraySpan<int>>` → span
                // (borrow), `Opt<Res>` → class (borrow). Unresolvable — no env,
                // or a still-generic use whose substituted type is itself a bare
                // parameter — keeps the conservative reading: it MIGHT carry a
                // borrow (the documented uninstantiated-template posture, which
                // per-instantiation reclassification can only RELAX).
                const RecordSubstEnv::Entry sub =
                    resolveRecordFieldSubst(ft, env);
                if (sub.ty && sub.ty != ft)
                    return payloadFieldTypeIsBorrowCarrying(sub.ty, ctx,
                                                            choiceSeen, sub.env,
                                                            memo);
                return true;  // uninstantiated / still-generic bare param
            }
            if (ft->declaration->isValueBuiltin) return false;
            if (ft->declaration->kind == ast::TypeDeclKind::Class ||
                ft->declaration->kind == ast::TypeDeclKind::Interface)
                return true;
            if (ft->declaration->kind == ast::TypeDeclKind::Record) {
                // §9.7 transitivity: a borrow-bearing record payload. Thread the
                // env (its own bare-parameter fields resolve through it, #2145)
                // and the choice PATH set as the record walk's `choiceSeen`
                // guard, so a record field of choice type (#2159) that reaches
                // back into a choice on the current path terminates.
                std::unordered_set<const sema::TypeRef*> recSeen;
                return isBorrowBearingRecordCommon(ft, ctx, recSeen, env,
                                                   &choiceSeen);
            }
            if (ft->declaration->kind == ast::TypeDeclKind::Choice) {
                // §35.4 transitivity: a payload that is itself a borrow-
                // bearing choice. Classify at THIS instantiation (env-threaded,
                // #2183) via the shared core, which continues the `choiceSeen`
                // path guard and consults the per-call memo.
                auto cls = classifyChoiceCore(ft, ctx, env, choiceSeen, memo);
                return cls && *cls == ChoiceClassification::BorrowBearing;
            }
            return false;  // enum
        }
        case sema::TypeRef::Kind::InlineArray: {
            // §9.7: an inline-array field with reference-typed elements is
            // borrow-carrying; `arrayOwned` would have made it owning (decided
            // before this leg). Resolve a bare-parameter ELEMENT through the env
            // first (`[N]T` at `T := C` is an array of references). // #2183
            if (!ft->arrayElement || ft->arrayOwned) return false;
            const sema::TypeRef* el =
                resolveRecordFieldSubst(ft->arrayElement, env).ty;
            return isReferenceTypeForRecordField(el ? el : ft->arrayElement);
        }
        default:
            return false;  // Primitive, Unit, Function, Error
    }
}

// §35.4 (v0.200, #2045; env-threaded v0.226, #2183): the classification core.
// `choiceSeen` is the shared CHOICE PATH guard (also threaded through the
// record walk for the §9.7 record↔choice mutual recursion, #2159). `env` is the
// optional incoming substitution env for `rt`'s own bare-parameter arguments.
//
// fix #3144 (family F9 of umbrella #3152): the whole walk is SYMBOL-side. It
// used to open with
//
//     const auto* decl = dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
//     if (!decl) return std::nullopt;
//
// which is null for EVERY imported type, so this answered `nullopt` — "not a
// choice at all" — for every imported choice. That is the audit's third
// taxonomy shape: not a wrong predicate, but a correct predicate fed an EMPTY
// SOURCE. MEASURED consequence (two-module build, LSan, 200 iterations): 199 of
// 200 `owned` payloads of an imported owning choice were never disposed, while
// the identical program in one module leaked nothing.
std::optional<ChoiceClassification> classifyChoiceCore(
    const sema::TypeRef* rt, sema::SemaContext& ctx,
    const RecordSubstEnv* env,
    std::unordered_set<const sema::TypeRef*>& choiceSeen,
    std::unordered_map<const sema::TypeRef*, ChoiceClassification>* memo) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return std::nullopt;
    if (rt->declaration->kind != ast::TypeDeclKind::Choice)
        return std::nullopt;
    // fix #4140+#4125+#4145+#4112 (family J §2.6, v0.298): per-call
    // classification cache lookup (BEFORE the cycle guard). A
    // fully-classified `rt` from earlier in this same `classifyChoiceType`
    // call returns its cached verdict without re-walking its cases —
    // collapsing the deep linear nesting (#4145) from ~N^2 to O(N), and
    // letting `payloadFieldTypeIsMoveOnly`'s Choice arm reach the §28.1
    // erasure oracle (#4112) without paying a 2^N re-walk (#4145
    // exponential). The cache stores only FINAL verdicts from completed
    // classifications — the cycle guard's `ValueLike` (below) is never
    // cached, so a re-entry from a NEW path that hits the cache gets the
    // RIGHT answer, not the cycle-edge's conservative stand-in. // per
    // §35.1, §35.4, §28.1, v0.298
    if (memo) {
        auto it = memo->find(rt);
        if (it != memo->end()) return it->second;
    }
    const sema::TypeSymbol* decl = rt->declaration;
    const std::vector<const sema::ChoiceCaseSymbol*> cases =
        sema::choiceCasesOf(decl);
    // §35.1: "A choice declares at least one case." A Choice-kinded symbol with
    // ZERO cases is therefore not a well-formed choice — it is a choice this
    // compilation cannot see the cases of. Two shapes reach here: a STALE
    // dependency artifact compiled before #3144 put the case list on the wire,
    // and a local declaration that failed to parse (already diagnosed).
    //
    // Answer `nullopt` — "no verdict" — rather than letting the empty walk fall
    // through to ValueLike. This is the exact conservative degradation the
    // pre-#3144 code produced for every imported choice, so a stale artifact
    // keeps its old (safe) behaviour rather than acquiring a NEW wrong answer:
    // `Sendability.cpp`'s §35.12 gate reads a ValueLike verdict as "now walk the
    // payloads", and with no payloads to walk that would silently answer
    // SENDABLE for an owning choice whose payloads were never examined. A reject
    // is recoverable; a data race is not.
    if (cases.empty()) return std::nullopt;

    // #2159 cycle guard (PATH semantics): a choice INSTANTIATION already being
    // classified on the current path — reached only through a record↔choice
    // value cycle, itself rejected by §35.1/§9's no-self-containment rule,
    // defended here — contributes no new borrow information; treat it as
    // value-like FOR THIS EDGE and stop. Erased on exit so a SIBLING component
    // re-classifies it under its own env (the #2145 diamond, applied to
    // choices).
    //
    // fix #3933 (§35.11/§35.4, v0.295): keyed on `rt` (the instantiation),
    // not `decl` (the bare declaration) — see the header comment on
    // `isBorrowBearingRecordCommon` above for the false-cycle this used to
    // produce for a generic choice nested in itself at a DIFFERENT type
    // argument (`Pair<Pair<Box>>`, `Pair<Pair<int>>`).
    //
    // fix #4140: the cycle guard is also what closes the §35.1 self-
    // containment diagnostic's "report the existing diagnostic and stop"
    // requirement — a re-entry on the current path terminates here with
    // `ValueLike`, exactly as #4140's body requested, without any further
    // classification attempt. DO NOT cache this `ValueLike`: a sibling or
    // follow-up classification from a DIFFERENT path must get the real
    // verdict, which the memo would otherwise mask. // per §35.1, v0.298
    if (!choiceSeen.insert(rt).second)
        return ChoiceClassification::ValueLike;

    // §9.7 / §35.4 / §30.1 (#2183): this instantiation's substitution env for
    // the borrow leg's bare-parameter payloads, seeded from the choice's generic
    // params × rt->genericArgs and composed through the incoming `env` (so a
    // generic choice field of a generic record — `Holder<T>(K: Opt<T>)` at
    // `Holder<int>` — resolves `Opt<T>`'s `T` to `int`). Empty → the
    // conservative declaration-site reading.
    RecordSubstEnv myEnv;
    seedRecordSubstEnv(*decl, *rt, env, myEnv);
    const RecordSubstEnv* fieldEnv = myEnv.map.empty() ? nullptr : &myEnv;

    const ChoiceClassification verdict = [&]() -> ChoiceClassification {
        // §35.4: OWNING — any case has an ACTIVE `owned` marker (per the
        // §28.1 per-instantiation erasure oracle) or a move-only value-typed
        // payload. The `owned`-marker leg threads the in-flight `choiceSeen`
        // path guard through the file-local oracle impl so the choice↔choice
        // cycle an `owned`-typed payload opens (an `owned` slot whose
        // instantiated type is itself a choice — #2130) terminates; PATH
        // semantics (insert on entry, erase on exit) mean any entry pushed by
        // the recursive descent is gone before the borrow leg runs. The
        // move-only leg keeps its OWN accumulator (`owningSeen` never recurses
        // into a record and shares no path with the borrow leg).
        //
        // fix #4125 (v0.295): `owningSeen` keyed on `const sema::TypeRef*`
        // (instantiation), not `TypeSymbol*` declaration — distinct
        // instantiations of a shared generic declaration (`D<int>` and
        // `D<Widget>` both point at the same `D` symbol) must not collide in
        // a guard whose job is bounding SIBLING deduplication within one
        // caller's cases loop, not memoizing across unrelated instantiations.
        //
        // fix #4140+#4112: `payloadFieldTypeIsMoveOnly`'s Choice arm now
        // threads the ambient `choiceSeen` (cycle guard continuity) and the
        // per-call `memo` (cache continuity), and delegates to
        // `classifyChoiceCore` for the §28.1 erasure-aware decision. The
        // sibling `seen` parameter is the second cycle guard scoped to THIS
        // caller's cases loop only, mirroring the pre-existing
        // `payloadFieldTypeIsBorrowCarrying` Choice-arm threading.
        std::unordered_set<const sema::TypeRef*> owningSeen;
        owningSeen.insert(rt);
        for (const sema::ChoiceCaseSymbol* ccs : cases) {
            if (!ccs->caseDecl) continue;
            for (const auto& pf : ccs->caseDecl->payload) {
                if (!pf.type) continue;
                if (pf.modifier == "owned") {
                    if (ownedMarkerActiveOnInstantiatedTypeImpl(
                            substitutedChoicePayloadType(pf, decl, rt, ctx),
                            ctx, choiceSeen, memo))
                        return ChoiceClassification::Owning;
                    continue;  // erased → a value / runtime-managed slot.
                }
                if (payloadFieldTypeIsMoveOnly(ctx.resolvedType(*pf.type), ctx,
                                               owningSeen, choiceSeen, memo))
                    return ChoiceClassification::Owning;
            }
        }
        // §35.4: BORROW-BEARING — any case has a plain (unmarked) borrow-
        // carrying payload field under §9.7's definition, classified AT THIS
        // instantiation via `fieldEnv` (#2183).
        for (const sema::ChoiceCaseSymbol* ccs : cases) {
            if (!ccs->caseDecl) continue;
            for (const auto& pf : ccs->caseDecl->payload) {
                if (!pf.type) continue;
                if (pf.modifier == "owned") continue;
                if (payloadFieldTypeIsBorrowCarrying(ctx.resolvedType(*pf.type),
                                                     ctx, choiceSeen, fieldEnv,
                                                     memo))
                    return ChoiceClassification::BorrowBearing;
            }
        }
        // §35.4: VALUE-LIKE.
        return ChoiceClassification::ValueLike;
    }();
    choiceSeen.erase(rt);
    // fix #4140+#4125+#4145+#4112 (family J §2.6, v0.298): cache the FINAL
    // verdict so subsequent classifications of the SAME instantiation in
    // THIS call return the cached answer instead of re-walking cases. The
    // cycle guard's `ValueLike` is intentionally NOT cached (handled above,
    // before this cache write). // per §35.1, §35.4, §28.1, v0.298
    if (memo) (*memo)[rt] = verdict;
    return verdict;
}

// §28.1 (v0.226, #2130): file-local impl of the per-instantiation `owned`
// marker erasure oracle that THREADS the choice-decl cycle guard, so the
// choice↔choice recursion this oracle opens (an `owned` payload slot whose
// instantiated type is itself a choice — `choice L Cons(owned t: L)` —
// recurses ownedMarkerActiveOnInstantiatedTypeImpl -> classifyChoiceCore ->
// (owning leg) -> ownedMarkerActiveOnInstantiatedTypeImpl) terminates on a
// self- or mutually-referential choice. The public entrypoint below seeds a
// fresh guard for an isolated query; the choice classifier passes its OWN
// in-flight `choiceSeen` so the guard carries across the pair (the bug, per
// the issue body: the public entrypoint created a fresh set per invocation,
// so the guard never fired across ownedMarkerActiveOnInstantiatedType <->
// classifyChoiceType and the stack overflowed). PATH semantics — insert on
// entry, erase on exit — match classifyChoiceCore, so sibling payload
// frames re-classify under their own env (the #2145 diamond).
bool ownedMarkerActiveOnInstantiatedTypeImpl(
    const sema::TypeRef* ft, sema::SemaContext& ctx,
    std::unordered_set<const sema::TypeRef*>& choiceSeen,
    std::unordered_map<const sema::TypeRef*, ChoiceClassification>* memo) {
    if (!ft) return true;  // unknown → conservative (owning)
    switch (ft->kind) {
        case sema::TypeRef::Kind::InlineArray:
            // §15.3: an owning inline array (`[N owned]T`) is a move-only
            // owning value type; a non-owning array carrying the marker is
            // rejected at the declaration (§28.1), so it never reaches here.
            return ft->arrayOwned;
        case sema::TypeRef::Kind::Named: {
            if (!ft->declaration) {
                // A bare generic parameter still uninstantiated, OR a builtin
                // whose Named TypeRef carries declaration=null. Value-type
                // builtins (DateTime/Guid/…) erase the marker; the class /
                // interface builtins (Array<T>, Task, …) and a still-generic
                // bare parameter are conservatively owning (§28.1's erasure is
                // a per-INSTANTIATION property — an unresolved parameter keeps
                // the marker's owning intent).
                return !sema::isValueBuiltinByName(ft->headName());
            }
            if (ft->declaration->isValueBuiltin) return false;
            switch (ft->declaration->kind) {
                case ast::TypeDeclKind::Class:
                case ast::TypeDeclKind::Interface:
                    return true;  // owning heap reference (vtable disposal)
                case ast::TypeDeclKind::Choice: {
                    // A nested choice: the marker is active only when that
                    // choice is itself owning at this instantiation. Thread
                    // the in-flight `choiceSeen` so the choice↔choice cycle
                    // this opens terminates (#2130). Thread `memo` (null
                    // from the public isolated query, populated from
                    // `classifyChoiceCore`'s per-call cache) so a nested
                    // choice reached through the owned-marker walk shares
                    // the SAME cache as the surrounding classifyChoiceType
                    // call — otherwise the same TypeRef would classify
                    // twice on the same path (#4145's exponential source).
                    // // per §28.1, #2130, #4140+#4125 (family J §2.6,
                    // v0.298)
                    auto v = classifyChoiceCore(ft, ctx, /*env=*/nullptr,
                                                choiceSeen, memo);
                    return v && *v == ChoiceClassification::Owning;
                }
                default:  // Record, Enum
                    return false;
            }
        }
        default:
            // Primitive (incl. `string`/`Object`, runtime-managed), Unit,
            // Function, Error — the marker erases to a value / runtime-managed
            // slot.
            return false;
    }
}

}  // namespace

bool ownedMarkerActiveOnInstantiatedType(const sema::TypeRef* ft,
                                         sema::SemaContext& ctx) {
    // Public entry: an isolated per-instantiation query with no in-flight
    // classification path, so seed a fresh cycle guard. The choice-side
    // recursion (an `owned` payload whose type is a choice) consults the
    // file-local impl directly with the classifier's threaded `choiceSeen`,
    // which is where the #2130 cycle guard fires. The `memo` parameter is
    // NOT threaded here — this is a stand-alone isolated query, and
    // sharing a cache with an unrelated classifyChoiceType call would
    // conflate scopes.
    //
    // fix #4140+#4125+#4145+#4112 (family J §2.6, v0.298): the public
    // entry's isolated `choiceSeen` means each public call gets its own
    // cycle guard, which is the right semantics for an isolated query.
    // // per §28.1, #2130, v0.298
    std::unordered_set<const sema::TypeRef*> choiceSeen;
    return ownedMarkerActiveOnInstantiatedTypeImpl(ft, ctx, choiceSeen);
}

const sema::TypeRef* substitutedChoicePayloadType(
    const ast::Param& pf, const sema::TypeSymbol* decl, const sema::TypeRef* rt,
    sema::SemaContext& ctx) {
    if (!pf.type) return nullptr;
    sema::TypeRef* ftTemplate = ctx.resolvedType(*pf.type);
    if (!ftTemplate) return nullptr;
    // fix #3144 (family F9 of #3152): `decl` is the choice's `TypeSymbol`, not
    // its `ast::TypeDecl`. Every caller reached the TypeDecl by
    // `dynamic_cast<...>(choiceTy->decl)`, null for every imported choice, so
    // an imported generic choice's bare-parameter payloads were never
    // substituted — `Result<Connection, string>.Ok` classified on the raw `T`.
    // `TypeSymbol::genericParams` carries the same names on both sides (#3145's
    // `seedRecordSubstEnv` overload rests on the same fact), so the substitution
    // is now decidable cross-module.
    if (!decl || !rt || decl->genericParams.empty() ||
        rt->genericArgs.size() != decl->genericParams.size())
        return ftTemplate;  // uninstantiated template / arity mismatch —
                            // the conservative declaration-site reading.

    // The `name -> concrete` map the shared walker takes. Built per call from
    // (genericParams, genericArgs) rather than threaded, because this entry
    // point's callers hold the INSTANTIATION, never a substitution map.
    std::unordered_map<std::string_view, sema::TypeRef*> subst;
    subst.reserve(decl->genericParams.size());
    for (std::size_t i = 0; i < decl->genericParams.size(); ++i)
        if (decl->genericParams[i])
            subst.emplace(decl->genericParams[i]->name, rt->genericArgs[i]);

    // TOP-LEVEL arm, kept local ON PURPOSE — the one place this walker must
    // NOT delegate (fix #4568). It reads `headName()`, which is
    // ALIAS-TRANSPARENT (#1462: `aliasResolvedHead` carries the underlying
    // head after `type Id<T> := T;` restores `astNode` to the use site),
    // whereas `sema::substituteGenericArgs`'s `paramHead` deliberately reads
    // only the use-site single-part head. The difference is load-bearing and
    // measured: a payload declared `owned v: Id<T>` resolves to the bare
    // parameter with `astNode` pointing at `Id`, so the shared walker's leaf
    // test misses it and `Flat<int>` would regress from value-like to owning.
    // Nullable is preserved here exactly as the shared walker preserves it
    // (`T?` -> `int?`); the pre-#4568 body dropped the `?`.
    if (ftTemplate->kind == sema::TypeRef::Kind::Named &&
        !ftTemplate->declaration) {
        auto it = subst.find(ftTemplate->headName());
        if (it != subst.end()) {
            sema::TypeRef* substituted = it->second;
            if (ftTemplate->nullable && substituted && !substituted->nullable) {
                sema::TypeRef* nn = ctx.makeTypeRef(*substituted);
                nn->nullable = true;
                return nn;
            }
            return substituted;
        }
    }

    // Everything BELOW the top level. Pre-#4568 this returned `ftTemplate`
    // unchanged, so `owned inner: Inner<T>` at `Outer<int>` handed the §28.1
    // erasure oracle a still-generic `Inner<T>`, which classifies
    // conservatively OWNING — `Outer<int>` came out move-only instead of
    // value-like and a second read of the local was rejected as a use after
    // move. The shared walker recurses into `genericArgs`, an InlineArray
    // element and Function parameter/return slots, and returns `ftTemplate`
    // itself (no allocation) when nothing beneath needed substituting, so a
    // concrete payload costs exactly the map build. // per §14.1, §35.4, v0.306
    return sema::substituteGenericArgs(ftTemplate, sema::SubstArgs{ctx, subst});
}

std::optional<ChoiceClassification> classifyChoiceType(
    const sema::TypeRef* rt, sema::SemaContext& ctx,
    const RecordSubstEnv* env) {
    // Public entry point: seed a fresh CHOICE PATH guard AND a fresh
    // per-call classification memo, then delegate to the shared core.
    //
    // `env` (default null) supplies substitutions for `rt`'s own bare-
    // parameter arguments when this choice is a component of an enclosing
    // generic type (#2183). Concrete `rt->genericArgs` are threaded regardless.
    //
    // fix #4140+#4125+#4145+#4112 (family J §2.6, v0.298): the memo is
    // PER CALL — two back-to-back `classifyChoiceType` calls on the same
    // TypeRef each get their own scope (no cross-call leakage, no
    // stale-cache risk if intervening sema mutations re-classify the
    // type). The memo lives in stack storage; no static/global state.
    // // per §35.1, §35.4, §28.1, v0.298
    std::unordered_set<const sema::TypeRef*> choiceSeen;
    std::unordered_map<const sema::TypeRef*, ChoiceClassification> memo;
    return classifyChoiceCore(rt, ctx, env, choiceSeen, &memo);
}

// per §15.3 (#3775): see the header comment for the contract. Promoted
// verbatim from the previously TU-local `static bool isMoveOnlyValueType`
// in sema/ExprTypecheck.cpp (#2912) — same two structural facts, same
// `kindOut` spellings — so the §8.6 boxing check and the new §28.11
// collection-element check share one decision instead of two drifting
// copies (CLAUDE.md §6 / LANE_RULES §6).
bool isMoveOnlyValueType(const sema::TypeRef* t, sema::SemaContext& ctx,
                         const char*& kindOut) {
    if (!t) return false;
    if (t->kind == sema::TypeRef::Kind::InlineArray && t->arrayOwned) {
        kindOut = "an owning inline array";
        return true;
    }
    if (t->kind == sema::TypeRef::Kind::Named && t->declaration &&
        t->declaration->kind == ast::TypeDeclKind::Choice) {
        auto verdict = classifyChoiceType(t, ctx);
        if (verdict && *verdict == ChoiceClassification::Owning) {
            kindOut = "an owning-classified choice";
            return true;
        }
    }
    return false;
}

bool declaredReturnIsBareTypeParam(const ast::MethodDecl& md,
                                   const sema::SemaContext& ctx) {
    // `owned T` is the ordinary ownership-transferring return (§28.8), not a
    // bare parameter — it is classified Owned, outside this rule.
    if (md.returnOwned) return false;
    if (!md.returnType) return false;
    // A bare type parameter resolves to a Named TypeRef with a NULL declaration
    // (TypeResolution_Names.cpp's isGeneric branch). Nullability is stripped —
    // the `nullable` flag lives on the TypeRef, not the head — so `T?` qualifies.
    const sema::TypeRef* rt = ctx.resolvedType(*md.returnType);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named ||
        rt->declaration != nullptr)
        return false;
    // Distinguish a bare type parameter from a 0-arity builtin (`Task`, `Object`
    // via its class form, …), which ALSO resolves to a null-declaration Named
    // ref: require the head to be one of `md`'s own (method-level) generic
    // parameters. The enclosing-type-parameter case is matched at the call site
    // (the caller has the callee symbol's parent type). // per §28.3, v0.194
    const std::string_view head = bareReturnHead(md);
    if (head.empty()) return false;
    for (const ast::GenericParam& g : md.genericParams)
        if (g.name == head) return true;
    return false;
}

std::vector<sema::Symbol*> bareTypeParamCallSourceRoots(
    const ast::CallExpr& ce, const ast::MethodDecl& md,
    const sema::SemaContext& ctx) {
    std::vector<sema::Symbol*> roots;
    const std::string_view retParam = bareReturnHead(md);
    if (retParam.empty()) return roots;

    bool isStatic = false;
    for (std::string_view m : md.modifiers)
        if (m == "static") { isStatic = true; break; }

    // Receiver source: for an instance / extension callee the returned borrow
    // may alias the receiver (§28.3 "the call's receiver … together with …").
    // A STATIC call has no value receiver — its callee target is a type name,
    // never a consumable binding — so it contributes none.
    if (!isStatic && ce.callee)
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get()))
            if (ma->target)
                if (sema::Symbol* s = bareRootSymbolOf(*ma->target, ctx))
                    roots.push_back(s);

    // Positional plain (non-`owned`) parameters whose declared type mentions the
    // returned parameter; keep only borrowable arguments. `owned` and `out`
    // parameters are never sources (the former is consumed into the callee and
    // dies there; the latter is an output). Named/discard/out-of-range args are
    // skipped (positional keying only, §28.3).
    for (std::size_t i = 0; i < md.params.size(); ++i) {
        const ast::Param& p = md.params[i];
        if (p.modifier.find("owned") != std::string_view::npos) continue;
        if (p.modifier.find("out") != std::string_view::npos) continue;
        if (!p.type || !bareTypeMentionsParam(*p.type, retParam)) continue;
        if (i >= ce.args.size()) continue;
        const ast::CallArgument& arg = ce.args[i];
        if (arg.isDiscard || !arg.value || !arg.name.empty()) continue;
        if (!bareArgIsBorrowable(ctx.exprType(*arg.value))) continue;
        if (sema::Symbol* s = bareRootSymbolOf(*arg.value, ctx))
            roots.push_back(s);
    }
    return roots;
}

// §17.15 (v0.216, #2098): peel a binding atom to its body, transitively —
// the atom's value/type/ownership classification is the body's (§28.3
// producers row; the atom is not a join). See the header.
const ast::Expr* peelBindAtomToBody(const ast::Expr* e) {
    while (const auto* bx = dynamic_cast<const ast::BindExpr*>(e)) {
        if (!bx->body) break;
        e = bx->body.get();
    }
    return e;
}

bool bindClauseIsOwnedProduction(const ast::LocalDecl& clause,
                                 const sema::SemaContext& ctx,
                                 const TypeParamResolver& resolveParam) {
    // K3.4: the marked binding transfer `var y := owned x;` — the binding
    // adopts the moved source's obligation (§28.3 v0.180 #1962, admitted
    // inside the atom per §17.15).
    if (clause.initializerHasOwnedMarker) return true;
    const ast::Expr* init = peelBindAtomToBody(clause.initializer.get());
    if (!init) return false;
    // A `new T(...)` of a non-value kind. Mirrors producerState's NewExpr arm
    // (Ownership.cpp): a record/enum/choice construction is a plain value
    // with no obligation; the value builtins (DateTime/TimeSpan/Guid/
    // CancellationToken/DateOnly/TimeOnly) likewise; every other `new` —
    // user classes AND the builtin heap types registerUsingDisposal knows
    // (collections, Channel, Stopwatch, CTS, …) — is a fresh owned
    // production. // per §28.3
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(init)) {
        const sema::TypeRef* rt =
            ne->type ? ctx.resolvedType(*ne->type) : ctx.exprType(*init);
        if (rt && rt->kind == sema::TypeRef::Kind::Named) {
            if (rt->declaration &&
                rt->declaration->kind != ast::TypeDeclKind::Class &&
                rt->declaration->kind != ast::TypeDeclKind::Interface)
                return false;  // value kind: no obligation
            if (rt->declaration && rt->declaration->isValueBuiltin)
                return false;
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                rt->astNode->name->parts.size() == 1) {
                std::string_view n = rt->headName();
                if (n == "DateTime" || n == "TimeSpan" || n == "Guid" ||
                    n == "CancellationToken" || n == "DateOnly" ||
                    n == "TimeOnly")
                    return false;  // value builtin (§2.5 / #434 / #1357)
            }
        }
        return true;
    }
    if (auto* call = dynamic_cast<const ast::CallExpr*>(init)) {
        // An `owned`-returning user call whose result is a heap class
        // (#1817's value-erasure gate holds: isFreshOwnedClassProducer
        // requires a class-declared result), or a builtin owned-transfer
        // call (Pop/Dequeue/TakeCurrent/… on an owning receiver).
        if (isFreshOwnedClassProducer(init, ctx, resolveParam)) return true;
        if (isBuiltinOwnedTransferCall(*call, ctx)) return true;
        return false;
    }
    // An owned-meet if/case expression (§28.3 arm-meet, #1661).
    if (dynamic_cast<const ast::ConditionalExpr*>(init) ||
        dynamic_cast<const ast::CaseExpr*>(init))
        return conditionalCaseArmMeetIsOwned(*init, ctx, resolveParam);
    return false;
}

namespace {

// §17.15 (#2098): does `e` contain a K3 consume of the visible binding
// `name`? See bindClauseConsumedInAtom's header comment for the closed
// shape set and the shadowing/lambda rules. With `unconditionalOnly`
// (#2080 F2 — the expression-body staged checks), only consumes on the
// STRAIGHT-LINE evaluation count: if/case ARM positions are skipped
// (conditions, scrutinees, and guards still evaluate unconditionally and
// are scanned), so a conditional consume is distinguishable structurally.
bool exprConsumesBinding(const ast::Expr* e, std::string_view name,
                         bool unconditionalOnly = false);

// True when `e` is (possibly a `!`-wrapped) identifier naming `name`.
bool isBindingIdent(const ast::Expr* e, std::string_view name) {
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e))
        e = pb->operand.get();
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
    return id && id->name == name;
}

bool argsConsumeBinding(const std::vector<ast::CallArgument>& args,
                        std::string_view name, bool unconditionalOnly) {
    for (const auto& a : args) {
        if (!a.value) continue;
        // The call-site transfer marker `M(owned x)` (§22.3 / K3.3).
        if (a.modifier == "owned" && isBindingIdent(a.value.get(), name))
            return true;
        if (exprConsumesBinding(a.value.get(), name, unconditionalOnly))
            return true;
    }
    return false;
}

bool exprConsumesBinding(const ast::Expr* e, std::string_view name,
                         bool unconditionalOnly) {
    if (!e) return false;
    // §28.3 K3.4 (v0.209, #2080): the marked ARM transfer `owned x` — a K3
    // consuming position wherever an if/case arm admits it.
    if (auto* mt = dynamic_cast<const ast::MarkedTransferExpr*>(e))
        return mt->name == name;
    if (auto* call = dynamic_cast<const ast::CallExpr*>(e)) {
        // Explicit `x.Dispose()` (K3.10; the `x!.Dispose()` spelling too).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call->callee.get())) {
            if (ma->member == "Dispose" && isBindingIdent(ma->target.get(), name))
                return true;
        }
        if (call->callee &&
            exprConsumesBinding(call->callee.get(), name, unconditionalOnly))
            return true;
        return argsConsumeBinding(call->args, name, unconditionalOnly);
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        // `x?.Dispose()` — callee is a NullCondMemberExpr.
        if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(
                nc->callee.get())) {
            if (nm->member == "Dispose" && isBindingIdent(nm->target.get(), name))
                return true;
        }
        if (nc->callee &&
            exprConsumesBinding(nc->callee.get(), name, unconditionalOnly))
            return true;
        return argsConsumeBinding(nc->args, name, unconditionalOnly);
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(e))
        return argsConsumeBinding(ne->args, name, unconditionalOnly);
    if (auto* te = dynamic_cast<const ast::ThrowExpr*>(e)) {
        // `throw x` in expression position (K3.8 / K4.2's announced kill).
        if (isBindingIdent(te->operand.get(), name)) return true;
        return exprConsumesBinding(te->operand.get(), name, unconditionalOnly);
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e)) {
        // `move x` — the owning-collection whole transfer (§28.6/#1107).
        if (ue->op == "move" && isBindingIdent(ue->operand.get(), name))
            return true;
        return exprConsumesBinding(ue->operand.get(), name, unconditionalOnly);
    }
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(e)) {
        // A nested atom: a clause's marked transfer `var y := owned x;`
        // consumes the outer binding; a nested clause REBINDING the name
        // shadows it for the remainder of that nested atom.
        for (const auto& cl : bx->clauses) {
            if (!cl) continue;
            if (cl->initializerHasOwnedMarker &&
                isBindingIdent(cl->initializer.get(), name))
                return true;
            if (exprConsumesBinding(cl->initializer.get(), name,
                                    unconditionalOnly))
                return true;
            if (cl->name == name) return false;  // shadowed from here on
        }
        return exprConsumesBinding(bx->body.get(), name, unconditionalOnly);
    }
    if (dynamic_cast<const ast::LambdaExpr*>(e)) {
        // A closure consuming a captured owned binding is rejected by the
        // §28.3 v0.184 (#1958) machinery (wired for atom clauses in
        // processBindAtom), so it cannot occur in an accepted program — and
        // a lambda parameter of the same name shadows its body anyway.
        return false;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e))
        return exprConsumesBinding(be->left.get(), name, unconditionalOnly) ||
               exprConsumesBinding(be->right.get(), name, unconditionalOnly);
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        if (exprConsumesBinding(cd->condition.get(), name, unconditionalOnly))
            return true;
        if (unconditionalOnly) return false;  // arm positions are conditional
        return exprConsumesBinding(cd->thenValue.get(), name, false) ||
               exprConsumesBinding(cd->elseValue.get(), name, false);
    }
    if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
        if (exprConsumesBinding(ce->scrutinee.get(), name, unconditionalOnly))
            return true;
        for (const auto& arm : ce->arms) {
            if (arm.guard &&
                exprConsumesBinding(arm.guard.get(), name, unconditionalOnly))
                return true;
            if (!unconditionalOnly &&
                arm.bodyKind == ast::CaseArmBodyKind::Expression &&
                exprConsumesBinding(arm.exprBody.get(), name, false))
                return true;
        }
        return false;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return exprConsumesBinding(ma->target.get(), name, unconditionalOnly);
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(e))
        return exprConsumesBinding(nm->target.get(), name, unconditionalOnly);
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        if (exprConsumesBinding(ix->target.get(), name, unconditionalOnly))
            return true;
        for (const auto& a : ix->indices)
            if (a.value &&
                exprConsumesBinding(a.value.get(), name, unconditionalOnly))
                return true;
        return false;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e))
        return exprConsumesBinding(pb->operand.get(), name, unconditionalOnly);
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(e))
        return exprConsumesBinding(tt->operand.get(), name, unconditionalOnly);
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& p : is->parts)
            if (p.expr &&
                exprConsumesBinding(p.expr.get(), name, unconditionalOnly))
                return true;
        return false;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        if (exprConsumesBinding(we->source.get(), name, unconditionalOnly))
            return true;
        for (const auto& ia : we->assigns)
            if (ia.value &&
                exprConsumesBinding(ia.value.get(), name, unconditionalOnly))
                return true;
        return false;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& k : cl->keys)
            if (k && exprConsumesBinding(k.get(), name, unconditionalOnly))
                return true;
        for (const auto& v : cl->elements)
            if (v && exprConsumesBinding(v.get(), name, unconditionalOnly))
                return true;
        return false;
    }
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(e))
        return exprConsumesBinding(re->lo.get(), name, unconditionalOnly) ||
               exprConsumesBinding(re->hi.get(), name, unconditionalOnly) ||
               exprConsumesBinding(re->step.get(), name, unconditionalOnly);
    // Literals / identifiers / typeof / nameof / default — no consume.
    return false;
}

}  // namespace

namespace {
bool bindClauseConsumedInAtomImpl(const ast::BindExpr& bx,
                                  std::size_t clauseIdx,
                                  bool unconditionalOnly) {
    if (clauseIdx >= bx.clauses.size() || !bx.clauses[clauseIdx]) return false;
    std::string_view name = bx.clauses[clauseIdx]->name;
    if (name.empty()) return false;
    // The binding scopes over the LATER clauses' initializers and the body
    // (§17.15); a later clause rebinding the name shadows it from the end of
    // that clause on.
    for (std::size_t i = clauseIdx + 1; i < bx.clauses.size(); ++i) {
        const auto& cl = bx.clauses[i];
        if (!cl) continue;
        if (cl->initializerHasOwnedMarker &&
            isBindingIdent(cl->initializer.get(), name))
            return true;
        if (exprConsumesBinding(cl->initializer.get(), name,
                                unconditionalOnly))
            return true;
        if (cl->name == name) return false;  // shadowed from here on
    }
    return exprConsumesBinding(bx.body.get(), name, unconditionalOnly);
}
}  // namespace

bool bindClauseConsumedInAtom(const ast::BindExpr& bx, std::size_t clauseIdx) {
    return bindClauseConsumedInAtomImpl(bx, clauseIdx,
                                        /*unconditionalOnly=*/false);
}

// §17.15 / K7.4 (#2080 F2): the STRAIGHT-LINE variant — true only when a K3
// consume of the clause is spelled on the atom's unconditional evaluation
// (if/case ARM positions excluded; conditions/scrutinees/guards included).
// consumedInAtom && !consumedUnconditionally structurally identifies a
// CONDITIONAL consume — the shape the first landing rejects — without the
// dataflow, so the expression-body path enforces the same bound.
bool bindClauseConsumedUnconditionally(const ast::BindExpr& bx,
                                       std::size_t clauseIdx) {
    return bindClauseConsumedInAtomImpl(bx, clauseIdx,
                                        /*unconditionalOnly=*/true);
}

bool bindClauseNeedsAtomDisposal(const ast::BindExpr& bx,
                                 std::size_t clauseIdx,
                                 const sema::SemaContext& ctx,
                                 const TypeParamResolver& resolveParam) {
    if (clauseIdx >= bx.clauses.size() || !bx.clauses[clauseIdx]) return false;
    return bindClauseIsOwnedProduction(*bx.clauses[clauseIdx], ctx,
                                       resolveParam) &&
           !bindClauseConsumedInAtom(bx, clauseIdx);
}

namespace {

// §29.8 / §30.1 (#973a): the scan behind stateMachineOwnedParamIsFrameNullable.
//
// DENY-BY-DEFAULT, unlike every other walker in the tree (CaptureWalker &c.,
// which silently ignore an unrecognised node). Here an unrecognised node could
// HIDE a discharge of the parameter, and a hidden discharge is a double-free —
// so `ok` is cleared on any node kind the scan does not enumerate, and the
// caller then keeps the §28.3 reject. Every `return` below is reached only
// after the node's children have been visited or the node has been proved
// child-free.
struct FrameNullableParamScan {
    const sema::SemaContext& ctx;
    const sema::Symbol* param;
    bool ok = true;

    // Is `e` a bare identifier naming the tracked parameter?
    bool isParamRef(const ast::Expr* e) const {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return false;
        return ctx.exprSymbol(*id) == param;
    }

    // finally-body walker: deny ANY occurrence of the parameter (borrow or
    // discharge). The outer stmt()/expr() allow a bare identifier only in
    // specific safe positions (the marked-adoption RHS, `p.F` borrow read);
    // from the §29.8 / §30.1 viewpoint a finally-body occurrence is at best a
    // false-positive (a borrow read that the frame doesn't double-free) and
    // at worst a duplicate-discharge (a marked adoption the iterator's
    // re-driven finally chains would lower twice). Either way, conservatively
    // reject. // per §29.8 / §30.1
    void finallyStmt(const ast::Stmt* s) {
        if (!ok || !s) return;
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(es->expr.get())) {
                if (ctx.exprSymbol(*id) == param) ok = false;
                return;
            }
            // Any expression-level occurrence of the parameter (member access,
            // call argument, etc.) inside finally: deny.
            finallyExpr(es->expr.get());
            return;
        }
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (ld->initializer) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ld->initializer.get())) {
                    if (ctx.exprSymbol(*id) == param) ok = false;
                    return;
                }
                finallyExpr(ld->initializer.get());
            }
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) finallyStmt(b.get());
            for (auto& ca : ts->catches) {
                if (ca.body) finallyStmt(ca.body.get());
            }
            for (auto& b : ts->finallyBody) finallyStmt(b.get());
            return;
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            for (auto& b : is->ifBranch.body) finallyStmt(b.get());
            for (auto& br : is->elifBranches)
                for (auto& b : br.body) finallyStmt(b.get());
            for (auto& b : is->elseBody) finallyStmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            for (auto& b : ws->body) finallyStmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            for (auto& b : fs->body) finallyStmt(b.get());
            return;
        }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) finallyStmt(b.get());
            return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            finallyExpr(r->value.get());
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            finallyExpr(as->target.get());
            finallyExpr(as->value.get());
            return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            finallyExpr(ds->value.get());
            return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            finallyExpr(ys->value.get());
            return;
        }
        if (auto* es = dynamic_cast<const ast::ThrowStmt*>(s)) {
            finallyExpr(es->value.get());
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            finallyExpr(inc->target.get());
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            finallyExpr(lc->value.get());
            return;
        }
        // Any other shape — deny conservatively.
        ok = false;
    }

    void finallyExpr(const ast::Expr* e) {
        if (!ok || !e) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (ctx.exprSymbol(*id) == param) ok = false;
            return;
        }
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            finallyExpr(m->target.get()); return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            finallyExpr(m->target.get()); return;
        }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            finallyExpr(c->callee.get());
            for (auto& a : c->args) finallyExpr(a.value.get());
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            finallyExpr(c->callee.get());
            for (auto& a : c->args) finallyExpr(a.value.get());
            return;
        }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            finallyExpr(b->left.get()); finallyExpr(b->right.get()); return;
        }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            finallyExpr(u->operand.get()); return;
        }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            finallyExpr(p->operand.get()); return;
        }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            finallyExpr(t->operand.get()); return;
        }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            finallyExpr(c->condition.get()); finallyExpr(c->thenValue.get());
            finallyExpr(c->elseValue.get()); return;
        }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            finallyExpr(t->operand.get()); return;
        }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            finallyExpr(r->lo.get()); finallyExpr(r->hi.get());
            finallyExpr(r->step.get()); return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) finallyExpr(a.value.get()); return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            finallyExpr(ix->target.get());
            for (auto& a : ix->indices) finallyExpr(a.value.get());
            return;
        }
        if (auto* ise = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : ise->parts) finallyExpr(part.expr.get());
            return;
        }
        // Any other shape — deny conservatively.
        ok = false;
    }

    void expr(const ast::Expr* e) {
        if (!ok || !e) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            // A bare read reached here through a position the allowlist below
            // did not clear — treat it as a possible discharge.
            if (ctx.exprSymbol(*id) == param) ok = false;
            return;
        }
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            // (b) `p.F` / `p.M(...)` — a borrow read of the parameter's
            // storage. `p.Dispose()` IS a discharge (and now also nulls the
            // frame capture slot, #2310 / §29.8 / §30.1), so it IS admitted.
            if (isParamRef(m->target.get())) {
                if (m->member == "Dispose") return;  // form 1 — admitted
                return;  // borrow read of a field/property/method
            }
            expr(m->target.get());
            return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            // (c) `p?.Dispose()` — the §7.5 null-conditional Dispose form.
            // Same lowering family as form 1; admitted. // per §29.8 / §30.1
            if (isParamRef(m->target.get())) {
                if (m->member == "Dispose") return;  // form 1 — admitted
                return;
            }
            expr(m->target.get());
            return;
        }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get());
            // An ARGUMENT position is a possible transfer (`M(owned p)`, or a
            // callee `owned` parameter). #2310: the call-site lowering
            // (`buildOrderedArgs` / the owning-collection transfer paths) now
            // nulls the frame capture slot BESIDE the alloca, so an
            // `M(owned p)` or owning-collection `Add(owned p)` transfer IS
            // admitted. Borrow-position arguments still fall through to
            // expr() below, which rejects a bare `p` there (a borrow is fine
            // but isn't a discharge — so it doesn't move ownership and the
            // scan has nothing to admit).
            for (auto& a : c->args) {
                if (a.modifier == "owned" && isParamRef(a.value.get())) {
                    // form 3 / form 5 — explicit `owned` argument modifier
                    // (user-method call, owning-collection Add/Insert/Set).
                    continue;
                }
                if (!a.modifier.empty()) {
                    expr(a.value.get());  // other modifiers (`out`, `ref`,
                                          // `scoped`, …) — generic walk
                    continue;
                }
                // Bare positional argument — check whether the callee parameter
                // is itself declared `owned` (the implicit-transfer case for a
                // bare `p` passed to `M(p: owned T)`). Reaching into the call
                // site's parameter list would require plumbing through the
                // enclosing call, which the scan does not have at expr() level;
                // the lower-level helper handles the frame-null, but a bare `p`
                // here would otherwise slip through as a borrow read. Deny.
                expr(a.value.get());
            }
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get());
            // Same argument-position rule as CallExpr above (#2310: explicit
            // `owned` arg is admitted).
            for (auto& a : c->args) {
                if (a.modifier == "owned" && isParamRef(a.value.get())) continue;
                if (!a.modifier.empty()) { expr(a.value.get()); continue; }
                expr(a.value.get());
            }
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get());
            for (auto& a : ix->indices) expr(a.value.get());
            return;
        }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get()); expr(b->right.get()); return;
        }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get()); return;
        }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get()); return;
        }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get()); return;
        }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get()); expr(c->thenValue.get());
            expr(c->elseValue.get()); return;
        }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(t->operand.get()); return;
        }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get()); expr(r->hi.get()); expr(r->step.get()); return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            // #2310 / §29.8 / §30.1 (form 6): `new Ctor(owned p)` — same
            // argument-position rule as CallExpr above.
            for (auto& a : n->args) {
                if (a.modifier == "owned" && isParamRef(a.value.get())) continue;
                if (!a.modifier.empty()) { expr(a.value.get()); continue; }
                expr(a.value.get());
            }
            return;
        }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get());
            return;
        }
        if (dynamic_cast<const ast::IntLiteralExpr*>(e) ||
            dynamic_cast<const ast::FloatLiteralExpr*>(e) ||
            dynamic_cast<const ast::StringLiteralExpr*>(e) ||
            dynamic_cast<const ast::CharLiteralExpr*>(e) ||
            dynamic_cast<const ast::BoolLiteralExpr*>(e) ||
            dynamic_cast<const ast::NullLiteralExpr*>(e))
            return;  // child-free leaves
        // Anything else (lambdas, `case`/`when` expressions, binding atoms,
        // marked transfers, collection literals, `with`, …) may hide a
        // discharge in a position this scan does not model.
        ok = false;
    }

    void stmt(const ast::Stmt* s) {
        if (!ok || !s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            // (a) the MARKED adoption `var y := owned p;` / `using var y :=
            // owned p;` — the admitted discharge form. Its lowering nulls the
            // frame capture slot beside the caller's alloca.
            if (ld->initializerHasOwnedMarker &&
                isParamRef(ld->initializer.get()))
                return;
            expr(ld->initializer.get());
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get()); return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get()); return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get()); return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get()); return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            // #2310 / §29.8 / §30.1 (form 4): `y := owned p` — the
            // assignment-position `owned` marker. The assignment-statement
            // lowering (LlvmBackend_Stmts.cpp ~L744) already nulls the alloca,
            // and `nullTransferredOwnedLocalSlot` (called from there) now also
            // nulls the frame capture slot. Admitted.
            if (as->valueHasOwnedMarker && isParamRef(as->value.get())) {
                // Target still walked: a `y := owned p` whose target is the
                // parameter itself (a self-assignment `p := owned p`) is
                // rejected by sema upstream; this admission only fires for a
                // target distinct from the parameter.
                return;
            }
            expr(as->target.get()); expr(as->value.get()); return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            expr(inc->target.get()); return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            // #2310 / §29.8 / §30.1 (form 2): `_ := p` — the §17.13 consuming
            // discard. Lowering now nulls the frame capture slot beside the
            // alloca (#973a residual). Admitted.
            return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get()); return;   // `yield return p` → reject
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get());
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get());
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get());
            for (auto& b : ws->body) stmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get()); expr(fs->withExpr.get());
            for (auto& b : fs->body) stmt(b.get());
            return;
        }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return;
        }
        if (dynamic_cast<const ast::BreakStmt*>(s) ||
            dynamic_cast<const ast::ContinueStmt*>(s))
            return;  // child-free
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            // #2310 / §29.8 / §30.1 (#973a residual): descend into the try body
            // and each catch arm normally — a discharge in either is admitted (the
            // matching lowering sites null the frame slot beside the alloca).
            // Deny only an occurrence inside the finally body: a marked adoption
            // spelled in a `finally` is lowered TWICE — once on the body's own path
            // and once more when the iterator's Dispose re-runs the pending
            // finally chains for a suspended state — and reasoning about which of
            // those two runs owns the value is a separate piece of work. The
            // catch arm `when` guard is descended into as an expr; the arm body
            // is descended into as a stmt (exactly the same walk the IfStmt branch
            // above uses for its condition / body).
            for (auto& b : ts->body) stmt(b.get());
            for (auto& ca : ts->catches) {
                expr(ca.guard.get());
                if (ca.body) stmt(ca.body.get());
            }
            if (!ts->hasFinally) return;
            // finally body: only an occurrence of the parameter as a discharge is
            // a problem; a borrow read is fine. Match the same sentinel the
            // existing deny rules use (treat any occurrence of the parameter as a
            // possible discharge, so the rest of the scan continues to flag
            // it).
            for (auto& b : ts->finallyBody) {
                finallyStmt(b.get());
                if (!ok) return;
            }
            return;
        }
        // A `case` / anything else. `case` arms and binding atoms are denied
        // for the deny-what-is-not-modelled reason (each is a false reject of a
        // possibly valid program, never a miscompile). // per §29.8 / §30.1
        ok = false;
    }
};

}  // namespace

bool stateMachineOwnedParamIsFrameNullable(
    const sema::SemaContext& ctx, const sema::Symbol* param,
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    if (!param) return false;
    FrameNullableParamScan scan{ctx, param};
    for (const auto& s : body) {
        scan.stmt(s.get());
        if (!scan.ok) return false;
    }
    return scan.ok;
}

}  // namespace bezel::common
