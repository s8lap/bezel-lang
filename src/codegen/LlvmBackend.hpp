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

    // per §33.1/§2.4/§33.6: is this compilation unit the executable's **main
    // module** — the module named as the build's executable target? Only the
    // main module's qualifying `Main` (§33.2) is the process entry point: it
    // alone is lowered to the native `main` symbol (§33.2 signature, i32
    // return, argc/argv ABI, the propagateBB uncaught-exception boundary,
    // §22.7 static-ctor bootstrapping). Everywhere else — a library artifact
    // (§33.6: "a library module has no entry point") and, by construction, any
    // dependency module (compiled as its own library artifact, §2.5) — a
    // method named `Main` is an ordinary method and must NOT claim the native
    // `main` symbol; doing so previously caused a link-time "multiple
    // definition of `main`" when a dependency happened to also declare a
    // qualifying `Main` (#1563). Default true: matches every single-module
    // caller (the executable driver path and the JIT test harnesses, which
    // always JIT-execute their snippet as if it were the main module). The
    // driver's library (`-c`) path explicitly sets this false.
    bool isMainModule = true;
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

