#pragma once

// fix #3650 (§28.9 / §15.1.1, v0.286): the ONE both-sources accessor for a
// field's DECLARED AST TYPE.
//
// §15.1.1's owning-inline-array disqualifier (`[N owned]T?` / `[N owned]T`)
// lives only on the SYNTACTIC `ast::Type` (the `arrayOwned` flag) — not on
// the resolved `sema::TypeRef` `storageComponentType` (Symbol.hpp) returns —
// so a walk that needs to test it must read the field's `ast::Type` node
// directly. Three copies of that exact read used to live in
// `src/common/OwnershipFacts.cpp`: the body-FieldDecl arm of the legacy
// `isNonTrivialDisposeClass(TypeSymbol*, ...)` overload (line 120), a
// character-identical arm a few lines below it, and a THIRD, net-new copy
// `ed9746e` (`narrow #3632`) added inside the new
// `isNonTrivialDisposeClass(TypeRef*, SemaContext&, ...)` overload (line
// 289) — flagged by #3651 as the seam-ratchet regression it was, and
// patched there with an interim `// SEAM-ACCESSOR` marker rather than a
// real drain. This header is the drain #3650 asked for: one function, one
// AST cast, every call site collapsed onto it.
//
// Consults BOTH sources, in the ModuleLoader-safe order (#3149): a
// primary-ctor parameter's positional type (`fs->posParam->type`) first;
// then, ONLY when `fs->decl` is non-null, the body `FieldDecl`'s declared
// type. An IMPORTED FieldSymbol has `decl == nullptr` per ModuleLoader's
// policy (#3149) — there is no `ast::FieldDecl` to reconstruct for an
// import — so the AST cast below is structurally unreachable on that path;
// wire-side fidelity for the imported case comes from the carried
// `TypeSymbol::importedNonTrivialDispose` verdict instead (#3191), consulted
// by the caller BEFORE this accessor is ever reached. Returns nullptr when
// neither source resolves.

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"

namespace bezel::sema {

inline const ast::Type* fieldDeclTypeOf(const FieldSymbol* fs) {
    if (!fs) return nullptr;
    if (fs->posParam) return fs->posParam->type.get();
    if (fs->decl) {
        // SEAM-ACCESSOR: this IS the both-sources accessor — symbol-side
        // gate immediately above (`fs->decl != nullptr`), AST-side cast
        // here, the imported path already dead by construction (see the
        // header comment). Same shape as Layout.cpp:1509 / Disposal.cpp:956
        // (#3149 / #3152's audit template).
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
            return fd->type.get();
    }
    return nullptr;
}

}  // namespace bezel::sema
