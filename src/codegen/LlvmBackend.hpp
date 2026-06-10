#pragma once

// LLVM code generation (decision #147). Lowers a checked compilation unit
// (the AST plus the SemaContext side tables produced by the front-end) to an
// `llvm::Module`. The driver emits an object file from the module and links a
// native executable; tests JIT-execute it (see tests/support/codegen_support).
//
// Slice CG0: emits a `main` that returns 0, ignoring the input — this exists
// to prove the LLVM toolchain wiring before real lowering begins.

#include <memory>
#include <string>

namespace llvm {
class Module;
class LLVMContext;
}  // namespace llvm

namespace bezel::ast {
struct SourceFile;
}
namespace bezel::sema {
class SemaContext;
}

namespace bezel::codegen {

// Per-build codegen options. Passed to emitModule; zero-initialised defaults
// match unchecked (release) behaviour so existing callers need no changes.
struct CodegenOptions {
    // §28.2 v0.134 / issue #601: enable checked disposal mode. When true,
    // every instance-method entry and property-accessor entry emits a disposed-
    // flag test; if the flag is set, DisposedAccessException is raised. Also
    // suppresses the heap-free in emitDisposeAndReclaim (quarantine: the block
    // remains readable so the flag check after disposal is well-defined).
    // Default false = unchecked (release) mode: zero extra cost.
    bool checkedDisposal = false;
};

// Build an LLVM module for `src` (already checked, using `ctx`'s side tables),
// constructed in `llvmCtx`. The caller owns `llvmCtx` and must keep it alive
// for the module's lifetime.
std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx,
                                         const CodegenOptions& opts = {});

// Emit `module` as a native object file at `objectPath` (host target).
// Returns true on success; on failure returns false and sets `error`.
// Initializes the native target on first use.
bool emitObjectFile(llvm::Module& module, const std::string& objectPath,
                    std::string& error);

}  // namespace bezel::codegen

