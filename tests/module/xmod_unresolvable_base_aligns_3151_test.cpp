// fix #3151 (§2.8.3 rule 4, v0.252): `TypeSymbol::baseTypes` and
// `baseTypeRefs` are documented as parallel vectors (the comment on
// `baseTypes` in sema/Symbol.hpp), but the IMPORTER side at
// ModuleLoader.cpp ~549 pushes to `baseTypeRefs` UNCONDITIONALLY and to
// `baseTypes` only when the named base resolves to a declaration. A base
// the importer cannot resolve (a transitive-dep type not loaded into the
// importer's context) leaves baseTypeRefs with the entry and baseTypes
// without it, so any consumer that walks them in lockstep — every indexed
// read of the form `baseRef = baseTypeRefs[bi]` where `bi` indexes
// `baseTypes` (TypeResolution_Checks.cpp L2612, L3123, L4029, L5335;
// LlvmBackend_Disposal.cpp L3674; LlvmBackend_Layout.cpp L1748; …) — pairs
// the resolved base at baseTypes[bi] with the unresolved base's TypeRef
// at baseTypeRefs[bi] (one position shifted because of the missing baseTypes
// entry). The misalignment is silent at the load site (baseTypeRefs is just
// longer than baseTypes) and only surfaces when the consumer actually walks
// both — the layout/disposal paths that bind base-class generic arguments.
//
// This test reconstructs that state directly: build a ModuleMetadata with
// ONE resolvable base (a normal user-exported class in the same module) and
// ONE unresolvable base (a name that maps to no loaded symbol), then load
// the metadata into a fresh SemaContext and inspect the reconstructed
// TypeSymbol's two parallel vectors.
//
// WHAT #3151 WOULD DO: the predicate asserts they are parallel (equal
// sizes, indices aligned). The reproducer below observes the asymmetry:
// baseTypeRefs.size() == 2 and baseTypes.size() == 1, with baseTypes[0]
// pointing at the SECOND (resolvable) base but baseTypeRefs[0] pointing at
// the FIRST (unresolvable) base — exactly the shape the indexed consumers
// above silently mis-bind.
//
// The fix chosen here (#3151 records the design decision in its body) is
// option (1) from the issue: make the importer symmetric — push to BOTH
// vectors under one guard, like the LOCAL side at TypeResolution_Names.cpp
// L604, OR push to NEITHER when the base is unresolvable. The choice is
// documented in the commit body.

#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "support/Diagnostic.hpp"

#include "test_harness.hpp"

#include <memory>
#include <string>


namespace bezel::module {
namespace {

// Build a ModuleMetadata with one public class that has TWO named bases:
// one is a real exported class in the same module (so `lookupNamed` succeeds
// — name resolution finds it), the other is a name pointing outside the
// loaded set (so `lookupNamed` returns null). The metadata does NOT
// depend on a driver; we construct it directly so the importer's behaviour
// is exercised in isolation.
ModuleMetadata makeMetadataWithUnresolvableBase() {
    ModuleMetadata md;
    md.moduleName = "SynthLib3151";

    // First class: the resolved base. Export it so `lookupNamed` finds it
    // when the importer runs.
    MetaTypeDecl resolvedBase;
    resolvedBase.name = "ResolvedBase";
    resolvedBase.kind = MetaTypeDeclKind::Class;
    resolvedBase.visibility = MetaVisibility::Public;
    md.types.push_back(std::move(resolvedBase));

    // Second class: the user-visible derived. It has TWO bases:
    //   [0] "UnresolvableBase" — a name the importer cannot resolve (no
    //        module loads anything under that name).
    //   [1] "ResolvedBase"     — the resolved one above.
    // The asymmetry: baseTypeRefs gets BOTH entries; baseTypes gets only
    // the resolved one. baseTypes[0] is the WRONG base (ResolvedBase),
    // baseTypeRefs[0] is the unresolvable ref, so every indexed consumer
    // binds ResolvedBase's layout to the UnresolvableBase TypeRef.
    MetaTypeDecl derived;
    derived.name = "Derived";
    derived.kind = MetaTypeDeclKind::Class;
    derived.visibility = MetaVisibility::Public;

    MetaType unresolvedBase;
    unresolvedBase.kind = TypeKind::Named;
    unresolvedBase.namedName = "UnresolvableBase";
    derived.baseTypes.push_back(std::move(unresolvedBase));

    MetaType resolvedBaseRef;
    resolvedBaseRef.kind = TypeKind::Named;
    resolvedBaseRef.namedName = "ResolvedBase";
    derived.baseTypes.push_back(std::move(resolvedBaseRef));

    md.types.push_back(std::move(derived));
    return md;
}

}  // namespace

// ============================================================================
// THE TEST: after fix #3151 (§2.8.3 rule 4, v0.252), the importer on a
// synthesized metadata whose `Derived` class names one resolvable + one
// unresolvable base populates BOTH baseTypeRefs and baseTypes with only the
// resolved one. The two vectors stay index-aligned: same size, both indices
// point at ResolvedBase. The unresolvable base is dropped from BOTH lists —
// the design choice documented in the fix's commit body (CLAUDE.md §4,
// option 1 in the issue text: matches the LOCAL side at
// TypeResolution_Names.cpp L604 under one guard `kind == Named &&
// declaration`).
//
// Fail-without demonstrated: with src/module/ModuleLoader.cpp reverted,
// baseTypeRefs.size() == 2 (the unconditional push) and baseTypes.size() ==
// 1 (the guarded push), and the assertions on size equality fail.
// ============================================================================
BEZEL_TEST(xmod_unresolvable_base_aligns_3151,
           importer_drops_unresolvable_base_symmetrically) {
    ModuleMetadata md = makeMetadataWithUnresolvableBase();

    DiagnosticEngine diags;
    sema::SemaContext ctx(diags);
    bool ok = loadMetadata(md, ctx);
    if (!ok) {
        // The loader bails out before reconstructing — note the alternative
        // shape here so a fix that moves the check earlier shows up as a
        // different test outcome.
        BEZEL_EXPECT_TRUE(false);
        return;
    }

    sema::TypeSymbol* derived = ctx.lookupImportedTypeByName("Derived");
    BEZEL_EXPECT_TRUE(derived != nullptr);
    if (!derived) return;
    sema::TypeSymbol* resolvedBase =
        ctx.lookupImportedTypeByName("ResolvedBase");
    BEZEL_EXPECT_TRUE(resolvedBase != nullptr);

    // baseTypeRefs and baseTypes are now PARALLEL: both have exactly one
    // entry — the resolved base. The unresolvable base is dropped from
    // BOTH lists, matching the local-side invariant.
    BEZEL_EXPECT_EQ(derived->baseTypeRefs.size(), 1u);
    BEZEL_EXPECT_EQ(derived->baseTypes.size(), 1u);
    if (derived->baseTypeRefs.empty() || derived->baseTypes.empty()) return;
    // baseTypes[0] and baseTypeRefs[0] agree — same base, same index.
    BEZEL_EXPECT_TRUE(derived->baseTypes[0] == resolvedBase);
    BEZEL_EXPECT_TRUE(derived->baseTypeRefs[0] != nullptr);
    BEZEL_EXPECT_TRUE(derived->baseTypeRefs[0]->declaration == resolvedBase);
}

}  // namespace bezel::module