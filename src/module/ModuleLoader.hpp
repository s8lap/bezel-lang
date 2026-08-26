#pragma once

// Cross-module import LOADER (§2.2 / §2.5, issue #328 increment 3).
//
// The inverse of Increment 2's builder/emit hook. Given a dependency's compiled
// artifact (an object or executable that carries a `.bezel.meta` section, §2.6
// step 4), the loader:
//
//   1. reads the `.bezel.meta` section bytes out of the artifact (LLVM Object
//      API — the compiler already links LLVM, no new dependency);
//   2. deserialize()s them (Increment 1) into a by-value ModuleMetadata;
//   3. reconstructs the module's exported public interface as live sema
//      TypeSymbol*/MemberSymbol* + resolved TypeRef* in the IMPORTER's
//      SemaContext, so the importer's name resolution (§2.2) can bind `import`s
//      and cross-module type references, and type-check against the imported
//      signatures (§2.6 step 3).
//
// Named-type references in the metadata are carried BY NAME (Inc1 decision);
// the loader resolves them within the set of modules loaded into the same
// context (so a method whose parameter is another exported type of the same
// module binds correctly). A name that resolves to neither a primitive nor a
// loaded type is left as an unresolved Named ref (declaration == nullptr) —
// the importer can still bind the surrounding member; only a use of that exact
// type fails, exactly as for a missing local type.
//
// The reconstructed symbols are owned by the SemaContext's IMPORTED list (kept
// separate from `symbols_`), and registered under the module's §2.1 dotted name
// for `import`-gated exposure (SemaContext::importedTypesOf).
//
// SCOPE (issue #328 inc 3): loads the acyclic A→B case. The §2.5
// circular-types-OK / circular-static-init-forbidden dependency loader, and
// staleness rejection via the header's semantic hash / schema version, are
// deferred (see the .cpp and the PR notes).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "module/ModuleMetadata.hpp"

namespace bezel::sema {
class SemaContext;
}  // namespace bezel::sema

namespace bezel::module {

// Read the raw bytes of the named section (`.bezel.meta`) out of an object or
// executable file at `artifactPath`, using the LLVM Object API. Returns
// std::nullopt if the file cannot be opened/parsed or the section is absent.
std::optional<std::vector<std::uint8_t>> readMetadataSection(
    const std::string& artifactPath);

// Load a dependency artifact's public interface into `ctx`. Reads the
// `.bezel.meta` section, deserializes it, and reconstructs the exported types
// as imported TypeSymbols (registered under the module's §2.1 dotted name).
// Returns the deserialized ModuleMetadata on success (so the driver can report
// the module name) or std::nullopt on any failure (no section / bad bytes /
// unreadable artifact).
//
// §2.8.4 rule 4 (issue #1733, v0.188): a dependency artifact must be
// library-kind. If the artifact is executable-kind (ModuleMetadata::isExecutable
// — it carries a native entry symbol), the load FAILS: std::nullopt is returned
// and, when `diag` is non-null, `*diag` is set to a human-readable build error
// naming the artifact and its kind for the caller to print. `diag` is left
// untouched for every OTHER failure mode, so the caller distinguishes the
// §2.8.4 rejection from an unreadable/corrupt artifact.
std::optional<ModuleMetadata> loadDependency(const std::string& artifactPath,
                                             sema::SemaContext& ctx,
                                             std::string* diag = nullptr);

// §2.5 / issue #1487: load a SET of dependency artifacts so that Named type
// references resolve INDEPENDENTLY of the order the artifacts are listed. Reads
// and validates every artifact, then reconstructs them in two global passes —
// all exported-type shells registered (pass 1), then all members filled (pass 2)
// — so a reference in one dependency to a user type declared in another binds by
// name whichever loads first. Loading each dependency separately (a loop over
// loadDependency) instead resolves each artifact's references against only the
// artifacts loaded before it, silently degrading a forward reference to a
// declaration=nullptr TypeRef (garbage runtime value). Returns true on success;
// on failure returns false and, for an executable-artifact rejection (§2.8.4
// rule 4), sets `*diag`; `*failedPath` (when non-null) names the offending
// artifact for a generic "cannot load metadata" message.
//
// §2.8.1 rules 1/3/5 (#1729, v0.188):
//   - When `rootModuleName` is non-empty, any dependency whose moduleName
//     equals it is rejected as the §2.8.1 rule-3 build error (a dependency
//     artifact declaring a module name the root compilation also declares).
//   - Any dependency whose moduleName is impl-provided (see
//     isImplProvidedModuleName) is rejected as the rule-5 build error
//     (rule 3 applied through rule 5).
//   - Two distinct dependencies declaring the same moduleName with
//     disagreeing descriptions are rejected as the rule-3 build error
//     (stale/disagreeing).
//   - Two distinct dependencies declaring the same moduleName with
//     agreeing descriptions are deduplicated, not diagnosed (rule 3's
//     idempotence carve-out, "the same artifact … bit-identical content").
//
// `survivingPaths` (when non-null) receives the subset of `artifactPaths` that
// were kept (one per moduleName — the canonical survivor wins), so the caller
// can pass exactly those to the native link step (otherwise a duplicate
// metadata-deduped dependency still contributes its object file twice to the
// link, producing §2.8.1 rule-3's "multiple definition of …" error at link
// time, #1729's repro).
bool loadDependencies(const std::vector<std::string>& artifactPaths,
                      sema::SemaContext& ctx, std::string* diag = nullptr,
                      std::string* failedPath = nullptr,
                      std::string_view rootModuleName = {},
                      std::vector<std::string>* survivingPaths = nullptr);

// Load an already-deserialized ModuleMetadata into `ctx` (the artifact bytes
// were obtained elsewhere — e.g. a sema-level test). Reconstructs and registers
// the exported types as above. Returns true on success.
bool loadMetadata(const ModuleMetadata& md, sema::SemaContext& ctx);

// §2.8.1 rule 5 (#1729, v0.188): the standard-library module names the
// compiler provides without any presented artifact. A root compilation or
// dependency artifact declaring one of these names is a build error (rule 3,
// applied through rule 5). The list is the standard-library document's module
// catalog at this revision.
bool isImplProvidedModuleName(std::string_view name);

}  // namespace bezel::module
