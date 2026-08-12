// §30.14 / §3.6 (#600, #2050): the sendability derived-type property.
//
// Implements the §30.14 sendable-type table for use by both:
//   - the call-site checks (Domain.Spawn args/result, `new Channel<T>()`,
//     `new ChannelSender<T>()`) in ExprTypecheck_Calls.cpp /
//     ExprTypecheck_Exprs.cpp, and
//   - the §14.2 derived-property constraint `where T: sendable` satisfaction
//     check at the instantiation site (Resolver::checkOneConstraint), v0.201.
//
// Both routes feed the SAME computation (§14.2: "the constraint adds no second
// definition of the property"), so this file is the single source of truth for
// §30.14. The body was lifted verbatim from the previous
// `ExprTC::isSendable` member and threaded-`SemaContext&` instead of
// `ctx_`; `ExprTC::isSendable` is now a thin delegating wrapper.

#include "sema/Sendability.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "ast/Node.hpp"
#include "common/OwnershipFacts.hpp"  // §35.12 (#2045): classifyChoiceType
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

namespace {

// Helper: extract the builtin-name of a TypeRef whose declaration is null
// (i.e., a builtin Named TypeRef with an astNode carrying the type name).
std::string_view builtinTypeName(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named || rt->declaration)
        return {};
    if (rt->astNode && rt->astNode->name &&
        !rt->astNode->name->parts.empty())
        return rt->headName();
    return {};
}

// Helper: does the TypeRef represent a user-declared class (not builtin)?
bool isUserClass(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Named &&
           rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Class;
}

// Helper: does the TypeRef represent a user-declared record?
bool isUserRecord(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Named &&
           rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Record;
}

}  // namespace

bool isSendableType(const TypeRef* rt, SemaContext& ctx,
                    std::unordered_set<const TypeSymbol*>& inProgress) {
    if (!rt) return false;
    if (rt->kind == TypeRef::Kind::Error) return false;

    // §30.14: function-typed values are NEVER sendable.
    if (rt->kind == TypeRef::Kind::Function) return false;

    // §30.14 / §3.6: Unit type is sendable (no state).
    if (rt->kind == TypeRef::Kind::Unit) return true;

    // §30.14 value-type rule:
    //   "Value types are sendable iff they are not borrow-bearing (§9.7)
    //    and not move-only (§15.3 [N owned]T)."
    //
    // Inline array `[N]T` (not owned) is a value type but its element
    // determines borrow-bearing; `[N owned]T` is move-only → not sendable.
    if (rt->kind == TypeRef::Kind::InlineArray) {
        // [N owned]T is move-only per §15.3 → not sendable.
        if (rt->arrayOwned) return false;
        // [N]T is a value array; sendable iff element is sendable-value or string.
        // (An inline array of class elements makes the array borrow-bearing.)
        if (!rt->arrayElement) return false;
        const TypeRef* elem = rt->arrayElement;
        // Element must be a primitive (non-class) or string to be sendable.
        if (elem->kind == TypeRef::Kind::Primitive) return true;
        if (isUserRecord(elem)) {
            // A record in an inline array: sendable iff not borrow-bearing.
            // We reuse the full isSendable check on the record.
            return isSendableType(elem, ctx, inProgress);
        }
        return false;
    }

    // §30.14: string is sendable.
    if (rt->kind == TypeRef::Kind::Primitive) {
        if (rt->primitive == PrimitiveType::String) return true;
        // Object is NOT sendable (runtime type behind it is unverifiable).
        if (rt->primitive == PrimitiveType::Object) return false;
        // All other primitives (int, bool, char, byte, etc.) are sendable
        // value types.
        return true;
    }

    // Named types:
    if (rt->kind == TypeRef::Kind::Named) {
        // --- Stdlib-specific overrides (§30.14 stdlib table) ---
        std::string_view bname = builtinTypeName(rt);
        if (!bname.empty()) {
            // §30.14 stdlib table: NOT sendable (domain-local or borrow-carrying).
            //   Task<T>, OwningTask<T>, Task (bare): domain-local.
            //   Weak<T>: v0.140 lifted Shared<T> handles but NOT Weak<T> — a
            //     Weak<T> across domains leaves TryUpgrade racing with the last
            //     strong drop on another domain; the CAS in TryUpgrade is safe
            //     but the spec text was conservative: only handles (Shared<T>)
            //     were lifted, not weak handles.  Weak stays domain-local.
            //   EventSource<T>, AsyncEventSource<T>: domain-local.
            //   Semaphore: sendable per the §6.2-Mutex precedent (internally
            //     synchronized; SemaphoreToken is the non-sendable guard like Lock).
            //     §6.3 is silent — docs sentence queued in the 0.141 batch (#691).
            //   Lock: held guard is domain-local; must be released in the acquiring
            //     domain (stdlib §6.2 v0.138 / #647).
            //   SemaphoreToken: held permit guard is domain-local (same reasoning
            //     as Lock; §6.3 v0.140 / #647).
            //   CancellationTokenSource, CancellationToken: borrow-bearing record.
            // NOTE: Mutex is NOT in this list — sendable per stdlib §6.2 v0.138
            //   / #647: internally synchronized, like Channel<T>.
            // NOTE: Semaphore is NOT in this list — sendable per stdlib §6.3
            //   v0.140 / #647: internally synchronized (same reasoning as Mutex).
            if (bname == "Task" || bname == "OwningTask" ||
                bname == "Weak" ||
                bname == "EventSource" || bname == "AsyncEventSource" ||
                bname == "SemaphoreToken" ||
                bname == "Lock" ||
                bname == "CancellationTokenSource")
                return false;
            // CancellationToken is a borrow-bearing record per §30.14
            // ("CancellationToken is a borrow-bearing record and therefore
            //  not sendable by the value-type rule").
            if (bname == "CancellationToken") return false;

            // §28.13 v0.140 / #689: Shared<T> is sendable iff T is sendable AND
            // internally synchronized (compiler-known set: Mutex, Atomic<T>,
            // AtomicInteger<T>, Semaphore — per spec §28.13 sendability rule).
            // Shared<string>/Shared<UserClass>/etc. remain domain-local.
            // Prerequisite: the control block counts are now atomic (§689 stage 1).
            if (bname == "Shared") {
                if (rt->genericArgs.empty() || !rt->genericArgs[0])
                    return false;
                const TypeRef* T = rt->genericArgs[0];
                // T must be in the compiler-known internally-synchronized set.
                std::string_view tname = builtinTypeName(T);
                if (tname == "Mutex")           return true;
                if (tname == "Atomic")          return true;
                if (tname == "AtomicInteger")   return true;
                if (tname == "Semaphore")       return true;
                // All other T: domain-local, not sendable.
                return false;
            }

            // §30.14 stdlib table: sendable iff element sendable.
            //   OwningArray<T>, OwningStack<T>, OwningQueue<T>, OwningSet<T>,
            //   OwningLinkedList<T>, OwningDictionary<K,V>: owning collections.
            if (bname == "OwningArray" || bname == "OwningStack" ||
                bname == "OwningQueue" || bname == "OwningSet" ||
                bname == "OwningLinkedList") {
                // Sendable iff element type is sendable.
                if (rt->genericArgs.empty() || !rt->genericArgs[0])
                    return false;
                return isSendableType(rt->genericArgs[0], ctx, inProgress);
            }
            if (bname == "OwningDictionary") {
                // Both K and V must be sendable.
                if (rt->genericArgs.size() < 2) return false;
                if (!rt->genericArgs[0] || !rt->genericArgs[1]) return false;
                return isSendableType(rt->genericArgs[0], ctx, inProgress) &&
                       isSendableType(rt->genericArgs[1], ctx, inProgress);
            }

            // §30.14 stdlib table: borrowing collections (Array<T>, Stack<T>, etc.)
            //   sendable iff element is a value-sendable type (primitive or string)
            //   or a sendable class. Per spec: "A borrowing collection of sendable
            //   value/string elements is sendable; a borrowing collection of class
            //   elements (borrow slots) is NOT sendable."
            if (bname == "Array" || bname == "Stack" || bname == "Queue" ||
                bname == "Set" || bname == "LinkedList") {
                if (rt->genericArgs.empty() || !rt->genericArgs[0])
                    return false;
                const TypeRef* elem = rt->genericArgs[0];
                // String element: sendable.
                if (elem->kind == TypeRef::Kind::Primitive &&
                    elem->primitive == PrimitiveType::String)
                    return true;
                // Primitive (non-Object) element: sendable.
                if (elem->kind == TypeRef::Kind::Primitive &&
                    elem->primitive != PrimitiveType::Object)
                    return true;
                // Class element: NOT sendable (array carries borrow slots).
                if (isUserClass(elem)) return false;
                // Record element: sendable iff not borrow-bearing.
                if (isUserRecord(elem)) return isSendableType(elem, ctx, inProgress);
                return false;
            }

            // §30.14 / stdlib §6.9: Channel<T> and ChannelSender<T> are
            // themselves sendable (they carry ownership of the underlying
            // channel object — a move-transfer).
            if (bname == "Channel" || bname == "ChannelSender")
                return true;

            // §30.14 / stdlib §6.4 / #647: Atomic<T> and AtomicInteger<T> are
            // sendable — internally synchronized (all operations seq_cst per §6.4
            // v0.140); may be reached by multiple domains simultaneously. Ownership
            // transfers on move. This entry is the compiler-known companion to the
            // Shared<Atomic<T>> sendability check in the "Shared" branch above.
            if (bname == "Atomic" || bname == "AtomicInteger")
                return true;
            // §30.14 / stdlib §6.2 v0.138 / #647: Mutex is sendable —
            // internally synchronized; may be reached by multiple domains
            // simultaneously (like Channel<T>). Ownership transfers on move.
            if (bname == "Mutex")
                return true;

            // §30.14 / stdlib §6.3 v0.140 / #647: Semaphore is sendable —
            // internally synchronized (counting semaphore); same reasoning as
            // Mutex above. Ownership transfers on move.
            if (bname == "Semaphore")
                return true;

            // Other builtin types with no user TypeDecl: conservatively
            // not sendable unless on the explicit list above.
            return false;
        }

        // --- User-declared types ---
        TypeSymbol* decl = rt->declaration;
        if (!decl) return false;

        // §2.8.2 clause 1 / §30.14 (#1732, #1824): a cross-module-IMPORTED
        // record OR class has no ast::TypeDecl (decl->decl is null) — the
        // ModuleLoader reconstructs its members as sema::FieldSymbol /
        // PropertySymbol on a side table (§2.5) — so an AST walk cannot see its
        // fields. Both the record and the class branch below must therefore
        // NOT vacuously default such a type to sendable (that silently lets a
        // borrow-bearing imported value cross a domain boundary: a data race).
        // This shared helper walks the reconstructed storage members instead:
        // a field / auto-property's resolved type rides ctx.importedMemberType()
        // and its `owned` modifier rides MemberSymbol::modifiers. It applies the
        // caller-supplied per-field predicate `check(fieldType, isOwned)` — the
        // SAME check the branch's AST path uses — and returns false on the first
        // rejection. Positional record fields, body fields, and auto-properties
        // all reconstruct here (Field/Property members with a side-table type),
        // so all three are covered.
        //
        // Metadata does not distinguish an auto-property (storage) from a
        // computed one (no storage) — both reconstruct as PropertySymbol — so a
        // computed property whose type is a reference type is treated
        // conservatively here as storage: a SOUND over-approximation (may
        // over-reject, never under-reject). Carrying the §3.6 auto/computed
        // structural fact in the artifact interface is tracked by #1732.
        auto forEachReconstructedStorageMember =
            [&](const TypeSymbol* ts,
                const std::function<bool(const TypeRef*, bool)>& check) -> bool {
            for (Symbol* ms : ts->members) {
                const MemberSymbol* mem = nullptr;
                if (auto* fs = dynamic_cast<const FieldSymbol*>(ms)) {
                    mem = fs;
                } else if (auto* ps =
                               dynamic_cast<const PropertySymbol*>(ms)) {
                    if (ps->isIndexer) continue; // indexer: not storage
                    mem = ps;
                } else {
                    continue; // methods/ctors/consts/nested: not storage
                }
                const TypeRef* ft = ctx.importedMemberType(ms);
                bool isOwned = false;
                for (auto mod : mem->modifiers)
                    if (mod == "owned") { isOwned = true; break; }
                if (!check(ft, isOwned)) return false;
            }
            return true;
        };

        // Enum: value type → sendable.
        if (decl->kind == ast::TypeDeclKind::Enum) return true;

        // §35.12 (v0.200, #2045): a choice is sendable iff it is VALUE-LIKE
        // AND every payload field's type is sendable; a sendable choice
        // crosses as a copy. Borrow-bearing choices are never sendable
        // (they carry origin-domain borrows); owning choices are not
        // directly sendable (the §30.14 move-only exclusion, unchanged).
        // The §35.4 classification already supplies the per-field
        // transitivity (a value-like choice has no class/interface-typed
        // payload and no move-only payload), so a value-like verdict
        // leaves only the per-field sendable recursion over the (value-
        // typed) payloads.
        if (decl->kind == ast::TypeDeclKind::Choice) {
            auto verdict = common::classifyChoiceType(rt, ctx);
            if (!verdict) return false;  // defensive — unresolved
            if (*verdict != common::ChoiceClassification::ValueLike)
                return false;  // borrow-bearing or owning → not sendable
            const auto* td =
                dynamic_cast<const ast::TypeDecl*>(decl->decl);
            if (!td) return true;  // no AST → trust the value-like verdict

            // §35.11 / §35.12 (v0.226, #2211): #2183 made the §35.4
            // classifier instantiation-aware, but this payload recursion still
            // read the declaration template's bare `T`. Seed the same #2145
            // substitution environment and recurse on the instantiated payload
            // (`Opt<int>` -> `int`), not the bare parameter. Only bare payloads
            // are substituted here, so structured recursive payload traversal is
            // unchanged; the finite nested form `Opt<Opt<int>>` still reaches its
            // concrete leaf without a declaration-keyed recursion shortcut.
            common::RecordSubstEnv env;
            common::seedRecordSubstEnv(*td, *rt, /*incoming=*/nullptr, env);
            const common::RecordSubstEnv* fieldEnv =
                env.map.empty() ? nullptr : &env;
            for (const auto& cc : td->choiceCases) {
                for (const auto& pf : cc.payload) {
                    if (!pf.type) continue;
                    const TypeRef* ft = ctx.resolvedType(*pf.type);
                    ft = common::resolveRecordFieldSubst(ft, fieldEnv).ty;
                    if (!isSendableType(ft, ctx, inProgress)) return false;
                }
            }
            return true;
        }

        // Interface: NOT sendable ("no interface-typed fields" rule
        // means interface types themselves cannot be verified).
        if (decl->kind == ast::TypeDeclKind::Interface) return false;

        // Record: sendable iff not borrow-bearing per §9.7 / §30.14.
        // A record is borrow-bearing if any field is a class/interface
        // reference (non-owned) or a function type. Already covered
        // by value-type rule.
        if (decl->kind == ast::TypeDeclKind::Record) {
            const auto* td =
                dynamic_cast<const ast::TypeDecl*>(decl->decl);

            // Collect all fields and check each.
            auto fieldSendable = [&](const TypeRef* ft) -> bool {
                if (!ft) return true;
                // Function-typed fields: never sendable per §30.14.
                if (ft->kind == TypeRef::Kind::Function) return false;
                // Class-typed field that is NOT owned: borrow → not sendable.
                // (Record fields of class type are borrows by default; only
                // an `owned` field modifier makes it an ownership transfer,
                // but records cannot have owned fields per §9.7 / §15.3.)
                // NOTE: per §9.7, record fields of class type are always
                // borrows — so a class field makes the record borrow-bearing.
                if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                    ft->declaration->kind == ast::TypeDeclKind::Class)
                    return false;
                // Interface-typed field: not sendable.
                if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                    ft->declaration->kind == ast::TypeDeclKind::Interface)
                    return false;
                // Object-typed primitive: not sendable.
                if (ft->kind == TypeRef::Kind::Primitive &&
                    ft->primitive == PrimitiveType::Object)
                    return false;
                // Builtin non-sendable: defer to recursive check.
                if (ft->kind == TypeRef::Kind::Named && !ft->declaration)
                    return isSendableType(ft, ctx, inProgress);
                // Nested record: recurse.
                if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                    ft->declaration->kind == ast::TypeDeclKind::Record)
                    return isSendableType(ft, ctx, inProgress);
                // Primitive (non-Object) / string: sendable.
                if (ft->kind == TypeRef::Kind::Primitive)
                    return true;
                // InlineArray: defer.
                if (ft->kind == TypeRef::Kind::InlineArray)
                    return isSendableType(ft, ctx, inProgress);
                return true; // conservative: accept unknown
            };

            // §2.8.2 clause 1 / §30.14 (#1824): an IMPORTED record has no
            // ast::TypeDecl, so the AST field walk below cannot see its fields.
            // Do NOT vacuously default to sendable — a borrow-bearing imported
            // record (e.g. one with a non-owned class-typed or interface-typed
            // field) would then silently satisfy the §30.15 Channel<T> element
            // rule and cross a domain boundary: a data race (the exact sibling
            // of the #1732 class-branch hole). Walk the reconstructed members
            // instead, applying the SAME per-field `fieldSendable` check the AST
            // path uses. A record cannot carry `owned` fields (§9.7), so the
            // helper's isOwned bit is ignored here (fieldSendable rejects any
            // class-typed field regardless — a sound over-approximation).
            if (!td) {
                return forEachReconstructedStorageMember(
                    decl,
                    [&](const TypeRef* ft, bool /*isOwned*/) {
                        return fieldSendable(ft);
                    });
            }

            // Check positional fields.
            for (const ast::Param& f : td->positionalFields) {
                if (f.type) {
                    const TypeRef* ft = ctx.resolvedType(*f.type);
                    if (!fieldSendable(ft)) return false;
                }
            }
            // Check body fields and auto-properties.
            // In Bezel records, properties declared as `public Name: T;` are
            // auto-properties (PropertyDecl with bodyKind=Auto) — these are
            // semantically fields and must be checked for sendability (§30.14).
            for (const auto& m : td->members) {
                if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                    if (fd->type) {
                        const TypeRef* ft = ctx.resolvedType(*fd->type);
                        if (!fieldSendable(ft)) return false;
                    }
                } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                    // Auto-property: check its type for sendability.
                    if (pd->bodyKind == ast::PropertyBodyKind::Auto && pd->type) {
                        const TypeRef* ft = ctx.resolvedType(*pd->type);
                        if (!fieldSendable(ft)) return false;
                    }
                }
            }
            return true;
        }

        // Class: sendable iff "transfer-closed" per §30.14.
        if (decl->kind == ast::TypeDeclKind::Class) {
            // Coinductive: if already in progress, assume sendable
            // (break mutual recursion).
            if (inProgress.count(decl)) return true;
            inProgress.insert(decl);

            // §30.14 rule 1 (v0.242, #3052), verbatim: "The class is `sealed`
            // (§12.4); or it is `closed` and every class in its module-closed
            // hierarchy is itself transfer-closed (then the whole closed
            // hierarchy is sendable)." Through v0.241 the second arm was
            // spelled `sealed abstract`; both spellings are accepted here so a
            // program still carrying the retired pair keeps its verdict.
            bool isSealed = false;
            bool isAbstract = false;
            for (auto mod : decl->modifiers) {
                if (mod == "sealed") isSealed = true;
                if (mod == "abstract") isAbstract = true;
            }
            const bool isClosed = typeIsClosed(decl);
            if (!isSealed && !isClosed) { inProgress.erase(decl); return false; }

            if (isClosed || isAbstract) {
                // `closed`: sendable iff every concrete subclass in the
                // module is also sendable (coinductive over the closed hierarchy).
                // Collect subclasses: scan all TypeSymbols in the module whose
                // base-type chain includes this class.
                // Implementation: walk all symbols in context and find those that
                // have `decl` in their baseTypes chain.
                bool allSendable = true;
                for (const auto& sym : ctx.symbols()) {
                    auto* sub = dynamic_cast<TypeSymbol*>(sym.get());
                    if (!sub || sub == decl) continue;
                    if (sub->kind != ast::TypeDeclKind::Class) continue;
                    // Check if sub is a direct or indirect subclass of decl.
                    bool isSubclass = false;
                    // Simple: check baseTypes directly (direct subclasses only
                    // for the `closed`-root case, since in Bezel a `closed`
                    // hierarchy is always closed and shallow per §12.5).
                    for (TypeSymbol* base : sub->baseTypes) {
                        if (base == decl) { isSubclass = true; break; }
                    }
                    if (!isSubclass) continue;
                    // This is a direct subclass: check sendability.
                    auto* subTy = ctx.makeTypeRef();
                    subTy->kind = TypeRef::Kind::Named;
                    subTy->declaration = sub;
                    if (!isSendableType(subTy, ctx, inProgress)) {
                        allSendable = false;
                        break;
                    }
                }
                inProgress.erase(decl);
                return allSendable;
            }

            // Sealed (non-abstract) class: apply Rule 2 + Rule 3.
            // Walk fields directly from the TypeDecl to check owned modifiers.
            {
                const auto* td = dynamic_cast<const ast::TypeDecl*>(decl->decl);

                // Helper: check one TypeRef + owned modifier pair.
                // Rule 2: class-typed fields must be `owned` AND the class sendable.
                // Rule 3: no function, Object, or interface-typed fields.
                auto checkField = [&](const TypeRef* ft, bool isOwned) -> bool {
                    if (!ft) return true;
                    if (ft->kind == TypeRef::Kind::Function) return false;
                    if (ft->kind == TypeRef::Kind::Primitive &&
                        ft->primitive == PrimitiveType::Object) return false;
                    if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                        ft->declaration->kind == ast::TypeDeclKind::Interface)
                        return false;
                    if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                        ft->declaration->kind == ast::TypeDeclKind::Class) {
                        // Class field: must be owned AND the class must be sendable.
                        if (!isOwned) return false;
                        return isSendableType(ft, ctx, inProgress);
                    }
                    return isSendableType(ft, ctx, inProgress);
                };

                // Walk own fields and auto-properties.
                // (Auto-properties in classes are stored as PropertyDecl with
                // bodyKind=Auto; they are semantically fields for §30.14.)
                auto walkMembersForSend = [&](const ast::TypeDecl* atd) -> bool {
                    for (const auto& m : atd->members) {
                        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                            if (!fd->type) continue;
                            const TypeRef* ft = ctx.resolvedType(*fd->type);
                            bool isOwned = false;
                            for (auto mod : fd->modifiers)
                                if (mod == "owned") { isOwned = true; break; }
                            if (!checkField(ft, isOwned)) return false;
                        } else if (auto* pd =
                                       dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                            // Auto-properties (field-like) must also satisfy §30.14.
                            if (pd->bodyKind != ast::PropertyBodyKind::Auto ||
                                !pd->type) continue;
                            const TypeRef* ft = ctx.resolvedType(*pd->type);
                            bool isOwned = false;
                            for (auto mod : pd->modifiers)
                                if (mod == "owned") { isOwned = true; break; }
                            if (!checkField(ft, isOwned)) return false;
                        }
                    }
                    return true;
                };

                // §2.8.2 clause 1 / §30.14 (#1732): an IMPORTED class has no
                // ast::TypeDecl (decl->decl is null), so the AST walk above
                // cannot see its fields. Do NOT vacuously default to sendable —
                // that silently lets a non-sendable imported class (e.g. one
                // with a non-owned class-typed / interface-typed field) cross a
                // domain boundary, a data race. Instead walk the reconstructed
                // sema member list via forEachReconstructedStorageMember (shared
                // with the record branch, #1824), applying the SAME per-field
                // Rule 2/3 `checkField` the AST path uses.
                if (td ? !walkMembersForSend(td)
                       : !forEachReconstructedStorageMember(decl, checkField)) {
                    inProgress.erase(decl); return false;
                }
                // Walk inherited fields from base classes. An imported base has
                // no ast::TypeDecl, so use the reconstructed-member walk for it
                // too (#1732); a local base uses the AST walk.
                for (TypeSymbol* base : decl->baseTypes) {
                    if (!base || base->kind != ast::TypeDeclKind::Class) continue;
                    const auto* btd = dynamic_cast<const ast::TypeDecl*>(base->decl);
                    if (btd ? !walkMembersForSend(btd)
                            : !forEachReconstructedStorageMember(base, checkField)) {
                        inProgress.erase(decl); return false;
                    }
                }
            }
            inProgress.erase(decl);
            return true;
        }
    }
    return false; // unknown kind
}

// ── §14.2 (v0.201 sharpened §3.6 rule 4, #2059) component-path walker ──────
//
// Given a type that has just FAILED `isSendableType`, walk its structure to
// find the first field/element that violates a §30.14 sub-rule and return a
// fragment (shaped to APPEND to the existing flat diagnostic) naming the
// offending component and the rule it violates. Returns empty when the
// verdict cannot be confidently pinned to a single component (the spec
// sharpening disallows over-promising detail — fall back to the flat
// message rather than guess). Depth is bounded by `kMaxDepth` to keep the
// walk terminating and the message readable.

namespace {

constexpr int kMaxDepth = 3;  // component-path chain length cap.

// Display name for a TypeRef suitable for a diagnostic message: prefers the
// declaration's name; falls back to the syntactic head / stdlib imported
// name; finally a few kind-based spellings. Returns "<type>" on no verdict.
std::string typeDisplayName(const TypeRef* rt) {
    if (!rt) return "<type>";
    if (rt->kind == TypeRef::Kind::Named) {
        if (rt->declaration) return std::string(rt->declaration->name);
        if (!rt->headName().empty()) return std::string(rt->headName());
        return "<type>";
    }
    if (rt->kind == TypeRef::Kind::Primitive) {
        using P = PrimitiveType;
        switch (rt->primitive) {
            case P::Bool:    return "bool";
            case P::Char:    return "char";
            case P::String:  return "string";
            case P::Int:     return "int";
            case P::Object:  return "Object";
            case P::Byte:    return "byte";
            case P::SByte:   return "sbyte";
            case P::Short:   return "short";
            case P::UShort:  return "ushort";
            case P::UInt:    return "uint";
            case P::Long:    return "long";
            case P::ULong:   return "ulong";
            case P::Float:   return "float";
            case P::Double:  return "double";
            case P::Decimal: return "decimal";
            default:         return "primitive";
        }
    }
    if (rt->kind == TypeRef::Kind::Function) return "function type";
    if (rt->kind == TypeRef::Kind::InlineArray)
        return rt->arrayOwned ? "[N owned]T" : "[N]T";
    if (rt->kind == TypeRef::Kind::Unit) return "()";
    return "<type>";
}

// True iff `rt` is a builtin stdlib type whose sendability is decided by a
// §30.14 stdlib-table override (rather than by walking its fields). The
// component-path walker bails on these — the stdlib rule, not a field, is
// the structural reason, and the flat diagnostic already names §30.14.
bool isStdlibSendabilityOverride(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named) return false;
    if (rt->declaration) return false;  // user-declared → walk its fields
    return !rt->headName().empty();     // builtin-name sentinel
}

// One-level walk for a user-declared record: returns the diagnostic fragment
// naming the first field whose own type is non-sendable per the §30.14 /
// §9.7 record rule, or empty when no single field can be pinned.
std::string recordFailurePath(const TypeRef* rt, SemaContext& ctx, int depth) {
    const TypeSymbol* decl = rt->declaration;
    if (!decl) return {};
    const auto* td = dynamic_cast<const ast::TypeDecl*>(decl->decl);

    // Per-field predicate matching Sendability.cpp's `fieldSendable` (record
    // branch): a field fails when its type is function-typed, Object-typed,
    // an interface, a class (records hold only borrows; §9.7 forbids owned
    // record fields), or a non-sendable builtin / nested record.
    auto checkOne = [&](const TypeRef* ft) -> std::string {
        if (!ft) return {};
        if (ft->kind == TypeRef::Kind::Function)
            return "has function type (§30.14 value-type rule)";
        if (ft->kind == TypeRef::Kind::Primitive &&
            ft->primitive == PrimitiveType::Object)
            return "has `Object` type (§30.14 value-type rule)";
        if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
            ft->declaration->kind == ast::TypeDeclKind::Interface)
            return std::string("has interface type `") +
                   typeDisplayName(ft) + "` (§30.14 value-type rule)";
        if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
            ft->declaration->kind == ast::TypeDeclKind::Class)
            return std::string("has class type `") +
                   typeDisplayName(ft) +
                   "` (§9.7 borrow-bearing record)";
        if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
            ft->declaration->kind == ast::TypeDeclKind::Record) {
            if (isSendableType(ft, ctx)) return {};
            std::string sub;
            if (depth + 1 < kMaxDepth) sub = recordFailurePath(ft, ctx, depth + 1);
            return std::string("has record type `") +
                   typeDisplayName(ft) + "` that is not sendable" + sub;
        }
        if (ft->kind == TypeRef::Kind::Named && !ft->declaration &&
            !ft->headName().empty() &&
            !isSendableType(ft, ctx))
            return std::string("has type `") + typeDisplayName(ft) +
                   "` that is not sendable (§30.14 value-type rule)";
        if (ft->kind == TypeRef::Kind::InlineArray &&
            !isSendableType(ft, ctx))
            return std::string("has inline-array type `") +
                   typeDisplayName(ft) + "` that is not sendable";
        return {};
    };

    // AST path: positional fields, body fields, auto-properties.
    if (td) {
        for (const ast::Param& f : td->positionalFields) {
            if (!f.type || f.name.empty()) continue;
            const TypeRef* ft = ctx.resolvedType(*f.type);
            if (!ft) continue;
            std::string verdict = checkOne(ft);
            if (!verdict.empty())
                return std::string("field `") + std::string(f.name) + "` " +
                       verdict;
        }
        for (const auto& m : td->members) {
            std::string_view fName;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                if (!fd->type) continue;
                fName = fd->name;
                const TypeRef* ft = ctx.resolvedType(*fd->type);
                if (!ft) continue;
                std::string verdict = checkOne(ft);
                if (!verdict.empty())
                    return std::string("field `") + std::string(fName) + "` " +
                           verdict;
            } else if (auto* pd =
                           dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                if (pd->bodyKind != ast::PropertyBodyKind::Auto || !pd->type)
                    continue;
                fName = pd->name;
                const TypeRef* ft = ctx.resolvedType(*pd->type);
                if (!ft) continue;
                std::string verdict = checkOne(ft);
                if (!verdict.empty())
                    return std::string("field `") + std::string(fName) + "` " +
                           verdict;
            }
        }
        return {};
    }

    // #1824 imported-record path: walk reconstructed storage members via
    // the side table (no ast::TypeDecl available). Positional / body fields
    // and auto-properties all reconstruct as Field/Property members here.
    for (Symbol* ms : decl->members) {
        std::string_view fName;
        if (auto* fs = dynamic_cast<const FieldSymbol*>(ms)) {
            if (fs->posParam && !fs->posParam->name.empty())
                fName = fs->posParam->name;
            else
                fName = fs->name;
        } else if (auto* ps = dynamic_cast<const PropertySymbol*>(ms)) {
            if (ps->isIndexer) continue;
            fName = ps->name;
        } else {
            continue;
        }
        if (fName.empty()) continue;
        const TypeRef* ft = ctx.importedMemberType(ms);
        if (!ft) continue;
        std::string verdict = checkOne(ft);
        if (!verdict.empty())
            return std::string("field `") + std::string(fName) + "` " + verdict;
    }
    return {};
}

// One-level walk for a user-declared sealed class: returns the diagnostic
// fragment naming the first field that violates §30.14 rule 2 or rule 3
// (after rule 1 — sealing — is already verified by the caller), or empty
// when no single field can be pinned. Mirrors Sendability.cpp's class
// branch `checkField` exactly so the verdict never disagrees with the
// property computation.
std::string sealedClassFailurePath(const TypeRef* rt, SemaContext& ctx,
                                   int depth) {
    const TypeSymbol* decl = rt->declaration;
    if (!decl) return {};

    // Per-field predicate mirroring Sendability.cpp's class branch
    // `checkField`: function / Object / interface / class-not-owned /
    // class-owned-non-sendable / nested-non-sendable.
    auto checkOne = [&](const TypeRef* ft, bool isOwned) -> std::string {
        if (!ft) return {};
        if (ft->kind == TypeRef::Kind::Function)
            return "has function type (§30.14 rule 3)";
        if (ft->kind == TypeRef::Kind::Primitive &&
            ft->primitive == PrimitiveType::Object)
            return "has `Object` type (§30.14 rule 3)";
        if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
            ft->declaration->kind == ast::TypeDeclKind::Interface)
            return std::string("has interface type `") +
                   typeDisplayName(ft) + "` (§30.14 rule 3)";
        if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
            ft->declaration->kind == ast::TypeDeclKind::Class) {
            if (!isOwned)
                return std::string("has class type `") +
                       typeDisplayName(ft) +
                       "` that is not `owned` (§30.14 rule 3)";
            if (isSendableType(ft, ctx)) return {};
            // Owned-but-non-sendable class field: recurse one level for the
            // inner reason (typically "not sealed" or another offending
            // field) so the diagnostic chain stays informative.
            std::string sub;
            if (depth + 1 < kMaxDepth) {
                bool innerSealed = false;
                for (auto mod : ft->declaration->modifiers)
                    if (mod == "sealed") { innerSealed = true; break; }
                // §30.14 rule 1's second arm (v0.242, #3052): a `closed`
                // root satisfies the sealing requirement, so it must not be
                // reported as "not sealed".
                innerSealed = innerSealed || typeIsClosed(ft->declaration);
                sub = innerSealed ? sealedClassFailurePath(ft, ctx, depth + 1)
                                  : std::string(" class `") +
                                        typeDisplayName(ft) +
                                        "` is neither `sealed` nor `closed` "
                                        "(§30.14 rule 1)";
            }
            return std::string("has `owned` class type `") +
                   typeDisplayName(ft) + "` that is not sendable" + sub;
        }
        if (!isSendableType(ft, ctx))
            return std::string("has type `") + typeDisplayName(ft) +
                   "` that is not sendable (§30.14 rule 2)";
        return {};
    };

    // AST path: walk own members then inherited members.
    auto walkAtd = [&](const ast::TypeDecl* atd) -> std::string {
        for (const auto& m : atd->members) {
            std::string_view fName;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                if (!fd->type) continue;
                fName = fd->name;
                const TypeRef* ft = ctx.resolvedType(*fd->type);
                if (!ft) continue;
                bool isOwned = false;
                for (auto mod : fd->modifiers)
                    if (mod == "owned") { isOwned = true; break; }
                std::string verdict = checkOne(ft, isOwned);
                if (!verdict.empty())
                    return std::string("field `") + std::string(fName) + "` " +
                           verdict;
            } else if (auto* pd =
                           dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                if (pd->bodyKind != ast::PropertyBodyKind::Auto || !pd->type)
                    continue;
                fName = pd->name;
                const TypeRef* ft = ctx.resolvedType(*pd->type);
                if (!ft) continue;
                bool isOwned = false;
                for (auto mod : pd->modifiers)
                    if (mod == "owned") { isOwned = true; break; }
                std::string verdict = checkOne(ft, isOwned);
                if (!verdict.empty())
                    return std::string("field `") + std::string(fName) + "` " +
                           verdict;
            }
        }
        return {};
    };

    // Imported-class path: reconstructed storage members via the side table
    // (#1732). Same predicate; the `owned` modifier rides MemberSymbol.
    auto walkReconstructed = [&](const TypeSymbol* ts) -> std::string {
        for (Symbol* ms : ts->members) {
            const MemberSymbol* mem = nullptr;
            std::string_view fName;
            if (auto* fs = dynamic_cast<const FieldSymbol*>(ms)) {
                mem = fs;
                if (fs->posParam && !fs->posParam->name.empty())
                    fName = fs->posParam->name;
                else
                    fName = fs->name;
            } else if (auto* ps = dynamic_cast<const PropertySymbol*>(ms)) {
                if (ps->isIndexer) continue;
                mem = ps;
                fName = ps->name;
            } else {
                continue;
            }
            if (fName.empty()) continue;
            const TypeRef* ft = ctx.importedMemberType(ms);
            if (!ft) continue;
            bool isOwned = false;
            for (auto mod : mem->modifiers)
                if (mod == "owned") { isOwned = true; break; }
            std::string verdict = checkOne(ft, isOwned);
            if (!verdict.empty())
                return std::string("field `") + std::string(fName) + "` " +
                       verdict;
        }
        return {};
    };

    if (const auto* td = dynamic_cast<const ast::TypeDecl*>(decl->decl)) {
        if (std::string r = walkAtd(td); !r.empty()) return r;
    } else {
        if (std::string r = walkReconstructed(decl); !r.empty()) return r;
    }
    for (const TypeSymbol* base : decl->baseTypes) {
        if (!base || base->kind != ast::TypeDeclKind::Class) continue;
        const auto* btd = dynamic_cast<const ast::TypeDecl*>(base->decl);
        if (btd) {
            if (std::string r = walkAtd(btd); !r.empty()) return r;
        } else {
            if (std::string r = walkReconstructed(base); !r.empty()) return r;
        }
    }
    return {};
}

}  // namespace

std::string sendableFailureComponentPath(const TypeRef* rt, SemaContext& ctx) {
    if (!rt || rt->kind == TypeRef::Kind::Error) return {};

    // §30.14: function-typed values are never sendable — flat reason at the
    // root, no component chain to walk.
    if (rt->kind == TypeRef::Kind::Function) return {};

    // Fragment prefix — every successful return below starts with this so
    // the message reads naturally after the parent diagnostic's §14.2
    // citation: "...does not satisfy the constraint `where T: sendable`
    // (§14.2): field `f` has class type `X` ...".
    constexpr char kPrefix[] = ": ";

    // InlineArray. [N owned]T is move-only per §15.3 (flat reason); [N]T is
    // a value array whose element determines sendability.
    if (rt->kind == TypeRef::Kind::InlineArray) {
        if (rt->arrayOwned) return {};  // move-only — flat reason
        if (!rt->arrayElement) return {};
        const TypeRef* elem = rt->arrayElement;
        if (isSendableType(elem, ctx)) return {};  // unexpected — bail
        if (elem->kind == TypeRef::Kind::Primitive &&
            elem->primitive == PrimitiveType::Object)
            return std::string(kPrefix) +
                   "element type `Object` is not sendable "
                   "(§30.14 value-type rule)";
        if (isUserClass(elem))
            return std::string(kPrefix) + "element class type `" +
                   typeDisplayName(elem) +
                   "` is a borrow (§30.14 value-type rule)";
        if (isUserRecord(elem)) {
            // Recurse one level into the element record for a deeper chain.
            std::string sub = recordFailurePath(elem, ctx, 1);
            return std::string(kPrefix) + "element record type `" +
                   typeDisplayName(elem) + "` is not sendable" +
                   (sub.empty() ? std::string() : std::string(": ") + sub);
        }
        return std::string(kPrefix) + "element type `" +
               typeDisplayName(elem) +
               "` is not sendable (§30.14 value-type rule)";
    }

    // Primitives: only `Object` fails among the primitives — flat reason.
    if (rt->kind == TypeRef::Kind::Primitive) {
        if (rt->primitive == PrimitiveType::Object)
            return std::string(kPrefix) +
                   "`Object` is not sendable (§30.14 value-type rule)";
        return {};  // other primitives are sendable — unexpected, bail.
    }

    if (rt->kind != TypeRef::Kind::Named) return {};

    // Builtin stdlib type decided by a §30.14 stdlib-table override (Task<T>,
    // Weak<T>, Channel/atomics, etc.) — the §30.14 stdlib rule, not a field,
    // is the structural reason. The flat diagnostic already names §30.14.
    if (isStdlibSendabilityOverride(rt)) return {};

    const TypeSymbol* decl = rt->declaration;
    if (!decl) return {};

    // Interface at root: flat reason ("interface types are never sendable"
    // per §30.14 rule 3's reasoning) — no component to walk.
    if (decl->kind == ast::TypeDeclKind::Interface) return {};

    // Enum at root: sendable by §30.14 — would not reach here.
    if (decl->kind == ast::TypeDeclKind::Enum) return {};

    // Choice: borrow-bearing / owning choices are non-sendable for a
    // category-level §35.4 / §35.12 reason (flat). Value-like choices with
    // a non-sendable payload field name that payload.
    if (decl->kind == ast::TypeDeclKind::Choice) {
        auto verdict = common::classifyChoiceType(rt, ctx);
        if (!verdict) return {};
        if (*verdict != common::ChoiceClassification::ValueLike)
            return {};  // category-level — flat reason
        const auto* td = dynamic_cast<const ast::TypeDecl*>(decl->decl);
        if (!td) return {};

        // §35.11 / §35.12 (#2211): mirror the property walk above so a failed
        // generic payload diagnostic names the instantiated type (`Object`),
        // not the declaration template's bare parameter (`T`).
        common::RecordSubstEnv env;
        common::seedRecordSubstEnv(*td, *rt, /*incoming=*/nullptr, env);
        const common::RecordSubstEnv* fieldEnv =
            env.map.empty() ? nullptr : &env;
        for (const auto& cc : td->choiceCases) {
            for (const auto& pf : cc.payload) {
                if (!pf.type) continue;
                const TypeRef* ft = ctx.resolvedType(*pf.type);
                ft = common::resolveRecordFieldSubst(ft, fieldEnv).ty;
                if (!ft) continue;
                if (!isSendableType(ft, ctx)) {
                    std::string_view pName =
                        !pf.name.empty() ? pf.name : cc.name;
                    return std::string(kPrefix) + "payload `" +
                           std::string(pName) + "` of type `" +
                           typeDisplayName(ft) +
                           "` is not sendable (§35.12)";
                }
            }
        }
        return {};
    }

    // Record: walk fields for the first non-sendable component per §9.7 /
    // §30.14 (borrow-bearing record).
    if (decl->kind == ast::TypeDeclKind::Record) {
        std::string inner = recordFailurePath(rt, ctx, 0);
        return inner.empty() ? std::string() : std::string(kPrefix) + inner;
    }

    // Class: §30.14 transfer-closed rules.
    if (decl->kind == ast::TypeDeclKind::Class) {
        // Rule 1: sealing. A class that is neither `sealed` nor `closed`
        // (v0.242, #3052 — rule 1's two arms) fails at the root for a
        // structural reason the field walk cannot pin — name the rule.
        bool isSealed = false;
        for (auto mod : decl->modifiers)
            if (mod == "sealed") { isSealed = true; break; }
        if (!isSealed && !typeIsClosed(decl))
            return std::string(kPrefix) +
                   "class is neither `sealed` nor `closed` (§30.14 rule 1)";
        // Sealed: walk fields per rule 2/3.
        std::string inner = sealedClassFailurePath(rt, ctx, 0);
        return inner.empty() ? std::string() : std::string(kPrefix) + inner;
    }

    return {};
}

}  // namespace bezel::sema
