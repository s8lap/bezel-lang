#pragma once

// fix #6767 (§3.6 rule 2, §9.4, v0.359): the generic-TYPE instantiation sites
// type resolution recorded, handed on to the ownership pass.
//
// §3.6 rule 2: "A use of a generic **type's** parameter, in any member body
// ... is therefore a requirement of the constructed type, checked at every
// site that names it whether or not the program reaches that member". Some
// such requirements are only known once the ownership pass has read the
// member bodies: §9.4's "For a generic value kind both counts are read at
// each instantiation" makes a member that returns or stores a closure over
// `this` a requirement of each constructed type whose receiver value holds
// borrows. The Resolver records every site that names a generic type
// (`Resolver::instantiations_`) and joins its own obligations against them at
// the end of `resolveTypes`; this side table keeps that list for the ownership
// pass, which runs after it on the same SemaContext.
//
// It lives beside, not inside, SemaContext so that the hand-off does not
// touch the header every TU includes (`docs/ops/lane-contract.md` item 20).
// `resolveTypes` replaces the context's entry on every run; the ownership pass
// takes it (removing it). Every pipeline runs `resolveTypes` before
// `checkOwnership` on the same context, and `resolveTypes` clears the entry as
// it starts, so a reader never sees another context's list. A context that
// never reaches `checkOwnership` (ModuleLoader's per-dependency contexts)
// leaves its entry behind until a context at the same address replaces it;
// nothing reads it.

#include <vector>

#include "ast/Node.hpp"
#include "support/SourceLocation.hpp"

namespace bezel::sema {

class SemaContext;
struct TypeRef;
struct TypeSymbol;

struct RecordedTypeInstantiation {
    TypeSymbol* type = nullptr;           // the generic type named (`Bx`)
    TypeSymbol* enclosingType = nullptr;  // the type body the site sits in
    std::vector<TypeRef*> argRefs;        // its resolved type arguments
    std::vector<const ast::Type*> argAsts;  // their spellings (may be null)
    const void* siteKey = nullptr;        // one-diagnostic-per-site identity
    SourceLocation loc;                   // the site
    // The generic parameters the METHOD enclosing the site binds (null
    // outside a generic method): a bare argument naming one of them is that
    // method's parameter, not `enclosingType`'s.
    const std::vector<ast::GenericParam>* methodGenericParams = nullptr;
};

// Replace `ctx`'s recorded list (called once, at the end of resolveTypes).
void publishTypeInstantiations(const SemaContext& ctx,
                               std::vector<RecordedTypeInstantiation> sites);

// Move `ctx`'s recorded list out (empty when none was published).
std::vector<RecordedTypeInstantiation> takeTypeInstantiations(
    const SemaContext& ctx);

}  // namespace bezel::sema
