#pragma once

// Module public-interface metadata BUILDER + EMIT HOOK (§2.5 / §2.6, issue
// #328 increment 2).
//
// Increment 1 (ModuleMetadata.{hpp,cpp}) defined the by-value public-interface
// schema and its LLVM-bitstream serialize/deserialize. This file is the
// INVERSE of Increment 1's hand-built fixtures: it walks the LIVE sema symbol
// table of the module being compiled (sema::SemaContext::module() and the
// resolved TypeRefs attached to each declaration's AST nodes) and produces a
// ModuleMetadata describing the module's exported, type-checkable surface, so a
// downstream compilation can resolve `import`s (§2.5) and type-check against it
// without the source.
//
// PUBLIC-SURFACE FILTERING (§13). A library's interface is the part a
// downstream importer can see and bind against:
//   - top-level types that are `public` (§13.2; the default for top-level types
//     is `internal`, which is module-private and therefore NOT exported);
//   - within an exported type, members that are `public` or `protected` — the
//     `protected` surface is part of a public type's *inheritance* contract
//     (§13.1: visible to derived classes, including derived classes in other
//     modules) so a downstream subclass needs it. `private` and `internal`
//     members are dropped: a downstream importer can never bind them (§13.1).
//   - interface members, which carry no visibility keyword and are implicitly
//     public (§13.3), are all exported.
//
// EMIT HOOK (§2.6 step 4). embedModuleMetadata() builds the metadata, serializes
// it (Increment 1), and embeds the bytes into the LLVM module as a private
// global placed in a dedicated object section (".bezel.meta"). The native
// object the backend then emits carries the section alongside the code; a
// downstream tool reads the section back and deserialize()s it. The section is
// purely additive — existing code is untouched and a consumer that does not
// look for the section is unaffected.

#include <cstddef>

#include "module/ModuleMetadata.hpp"

namespace llvm {
class Module;
}  // namespace llvm

namespace bezel::sema {
class SemaContext;
}  // namespace bezel::sema

namespace bezel::module {

// The object-file section the serialized metadata is embedded in (§2.6 step 4;
// issue #328 "embed in a dedicated object/.so section"). The leading dot keeps
// it out of the default symbol/linker namespace; the linker carries it through
// to the final artifact, and `objcopy --dump-section .bezel.meta=…` (or the
// LLVM object API) recovers the bytes.
inline constexpr const char* kMetadataSectionName = ".bezel.meta";

// The name of the private global holding the serialized blob. One per module.
inline constexpr const char* kMetadataGlobalName = "__bezel_module_metadata";

// Build the ModuleMetadata for the module currently held by `ctx` (the one just
// compiled). Walks ctx.module()'s children, including only the §13 public
// surface (see the file header). Returns an empty-but-named metadata when the
// module has no exported declarations. Pure: touches no LLVM state.
ModuleMetadata buildModuleMetadata(sema::SemaContext& ctx);

// Emit hook (§2.6 step 4): build the metadata from `ctx`, serialize it
// (Increment 1's LLVM bitstream), and embed the bytes into `module` as a
// constant global in the `.bezel.meta` object section. Called by the AOT driver
// after the LLVM module is built and before the object file is written. Returns
// the byte length of the embedded blob.
std::size_t embedModuleMetadata(llvm::Module& module, sema::SemaContext& ctx);

}  // namespace bezel::module
