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

    // §22.16 / issue #4386: enable checked-contracts mode. When true,
    // `ensures` clauses evaluate at every normal-completion path (after
    // result + out-slot binding, before scope-end disposals) and `invariant`
    // clauses evaluate at every construction-exit / public-method-entry /
    // public-method-exit point; a false clause raises
    // ContractViolationException (§31.4). When false (the §22.16 release
    // default), `ensures`/`invariant` evaluations are NOT emitted at codegen
    // — clauses are pure, so non-evaluation is unobservable for any program
    // whose checked clauses never fault. `requires` is always-on regardless
    // of this bit (gated only on the clause kind, not on this flag). The two
    // modes are independent per-build choices — checkedDisposal and
    // checkedContracts are orthogonal.
    bool checkedContracts = false;

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

    // JIT→AOT test-harness migration §2 (memory net): when true, bezelc runs
    // LLVM's AddressSanitizerPass over the generated module in emitObjectFile,
    // so the *generated Bezel code* — not just the C++ runtime — is ASan-
    // instrumented. This closes the WS2 gap where use-after-free / out-of-bounds
    // *reads* performed by generated loads slip (allocator events — leaks,
    // double-frees — are already caught by an ASan-instrumented runtime + the
    // AOT link). Default false = byte-identical AOT path: no ASan pass, no
    // instrumentation. The executable this option's build produces must be
    // linked with `-fsanitize=address` (the driver's executable path does so
    // when set) so the ASan runtime that defines `__asan_report_*` /
    // `asan.module_ctor` is present.
    bool sanitizeAddress = false;

    // Proposal 0082 §3 — the *scoped await-bind*, stage-1 PROTOTYPE (#3048),
    // OFF BY DEFAULT. CLI: `--experimental-scoped-await-bind`.
    //
    // Despite living in CodegenOptions — the one options struct the driver
    // already plumbs from `main` through `Driver::compileFile` (the
    // `--checked-disposal` precedent) — this bit is a FRONT-END knob and is
    // never read by codegen. `Driver::compileExecutableObject` consults it to
    // decide whether to run the proposal §3 AST desugaring
    // (`src/driver/ScopedAwaitBind.{hpp,cpp}`) between the parse and the rest
    // of the pipeline; every pass after that point, this backend included, is
    // unaware the prototype exists. It nonetheless CHANGES THE EMITTED OBJECT
    // (a program that did not compile now does), so the AOT test harness's
    // content-addressed cache keys on it exactly as on the three bits above.
    bool scopedAwaitBind = false;

    // §28.3 *Diagnostics* — "The trust-admission report" (v0.307, #4605),
    // OFF BY DEFAULT. CLI: `--report-trust-admissions`.
    //
    // The same front-end-knob-in-a-codegen-named-struct precedent as
    // `scopedAwaitBind` above, and for a stronger reason: the spec states the
    // report "is informative: it changes no verdict and no generated code",
    // so it does NOT belong in the AOT harness's codegen-mode cache key
    // (that key's set is "every CodegenOptions bit that alters the EMITTED
    // OBJECT"). A cache hit skips the compile entirely and therefore cannot
    // report a diagnostic, so a *test* driving this mode must go through a
    // front-end oracle (`compileObject`) rather than a cached `runAot`.
    bool reportTrustAdmissions = false;
};

// Build an LLVM module for `src` (already checked, using `ctx`'s side tables),
// constructed in `llvmCtx`. The caller owns `llvmCtx` and must keep it alive
// for the module's lifetime.
std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx,
                                         const CodegenOptions& opts = {});

// fix #4593 (spec §7.2, stdlib §2.6 / §4.4 / §9.8, v0.306): stamp `module` with the HOST
// target triple and DataLayout.
//
// Why this is not cosmetic. An `llvm::Module` starts life with the EMPTY
// DataLayout, whose default integer alignments are not the host's — most
// importantly `i64` gets an ABI alignment of 4 bytes ("i64:32:64"), where
// x86-64 gives it 8. Every `module_->getDataLayout().getTypeAllocSize(T)` the
// emitter performs therefore answered a DIFFERENT question from the one the
// object emitter answers when it lays `alloca T` out, for every aggregate whose
// padding depends on an i64 member. The emitter uses those byte counts as
// memcpy/stride arguments to the runtime (`bz_array_add`, `bz_array_get`,
// `bz_llist_add_last`, the dictionary entry stride, the async/iterator frame
// size, …) against storage the BACKEND sized — so the two disagreed and the
// tail of every such value was dropped on the way in and read back
// uninitialized. Stamping the module at construction makes the emitter and the
// backend answer with one layout; `emitObjectFile` re-stamps idempotently for
// any module it did not build.
void setHostTargetLayout(llvm::Module& module);

// Emit `module` as a native object file at `objectPath` (host target).
// Returns true on success; on failure returns false and sets `error`.
// Initializes the native target on first use.
//
// When `sanitizeAddress` is true, LLVM's AddressSanitizerPass is run over the
// module (and every defined function tagged `sanitize_address`) before object
// emission, so the emitted machine code carries shadow checks on its
// loads/stores (JIT→AOT migration §2 memory net). Default false leaves the AOT
// path byte-identical — no pass, no instrumentation.
bool emitObjectFile(llvm::Module& module, const std::string& objectPath,
                    std::string& error, bool sanitizeAddress = false);

}  // namespace bezel::codegen

