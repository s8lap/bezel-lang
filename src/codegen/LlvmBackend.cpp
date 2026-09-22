#include "codegen/LlvmBackend.hpp"
#include "codegen/LlvmBackend_impl.hpp"

#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Instrumentation/AddressSanitizer.h"

namespace bezel::codegen {

// The Emitter class is declared in LlvmBackend_impl.hpp; its method
// definitions live in the sibling LlvmBackend_*.cpp translation units. This
// file holds the public entry points.

// fix #4593 (spec §7.2, v0.306): the ONE host TargetMachine both the emitter's
// layout queries and the object emitter derive from. Created once, on first
// use; null when the host target is unavailable (in which case the module keeps
// the empty DataLayout and emitObjectFile reports the lookup failure, exactly
// as before this fix).
static llvm::TargetMachine* hostTargetMachine() {
    static std::unique_ptr<llvm::TargetMachine> tm = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
        std::string err;
        const llvm::Target* target =
            llvm::TargetRegistry::lookupTarget(triple.str(), err);
        if (!target) return std::unique_ptr<llvm::TargetMachine>{};
        llvm::TargetOptions opts;
        return std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(triple, "generic", "", opts,
                                        llvm::Reloc::PIC_));
    }();
    return tm.get();
}

void setHostTargetLayout(llvm::Module& module) {
    llvm::TargetMachine* tm = hostTargetMachine();
    if (!tm) return;
    module.setTargetTriple(tm->getTargetTriple());
    module.setDataLayout(tm->createDataLayout());
}

std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx,
                                         const CodegenOptions& opts) {
    // Codegen works off the SemaContext symbol table (each symbol points at
    // its AST decl), so the SourceFile root isn't needed directly; the
    // parameter is kept for API stability / future use.
    (void)src;
    auto m = Emitter(ctx, llvmCtx, opts).run();
    if (m && std::getenv("BEZEL_DUMP_IR")) m->print(llvm::errs(), nullptr);
    return m;
}

bool emitObjectFile(llvm::Module& module, const std::string& objectPath,
                    std::string& error, bool sanitizeAddress) {
    // #506: verify the module before any file I/O or codegen pass runs. A
    // malformed module must not produce a (broken) object file; the driver
    // interprets a false return as a codegen error and stops the pipeline
    // (no link, non-zero exit). This is the AOT-path gate — the JIT path
    // (codegen_support.hpp::runJit) calls emitModule directly and does not
    // go through here, so it is not affected.
    if (llvm::verifyModule(module, &llvm::errs())) {
        error = "IR verification failed (malformed IR emitted; "
                "see report above)";
        return false;
    }

    // fix #4593 (spec §7.2, v0.306): one shared host TargetMachine, so the layout
    // the emitter measured its byte counts against IS the layout this pass
    // lowers `alloca`/GEP with. Re-stamping is idempotent for an Emitter-built
    // module (which stamps itself at construction) and is still required for a
    // module that came from anywhere else.
    llvm::TargetMachine* tm = hostTargetMachine();
    if (!tm) {
        error = "could not create a target machine for the host triple '" +
                llvm::sys::getDefaultTargetTriple() + "'";
        return false;
    }
    setHostTargetLayout(module);

    // JIT→AOT test-harness migration §2 (memory net): when requested, run LLVM's
    // AddressSanitizerPass over the generated module so the emitted machine code
    // carries shadow checks on its loads/stores. This closes the WS2 gap: an
    // ASan-instrumented *runtime* + AOT link already catches allocator events
    // (leaks via LSan, double-frees via the free() interceptor), but a
    // use-after-free / out-of-bounds *read* performed by a *generated* load
    // slips because no shadow check is inserted at bezelc's own loads. Guarded so
    // the default AOT path is byte-identical: with sanitizeAddress==false this
    // whole block is skipped and emission proceeds exactly as before.
    //
    // The linked executable must be built with `-fsanitize=address` so the ASan
    // runtime (defining `__asan_report_*` / `asan.module_ctor`) is present; the
    // driver's executable path adds that flag when this option is set.
    if (sanitizeAddress) {
        // (1) The AddressSanitizerPass only instruments functions carrying the
        // `sanitize_address` fn-attr (Clang sets it under -fsanitize=address);
        // tag every defined function bezelc emitted.
        for (llvm::Function& fn : module)
            if (!fn.isDeclaration())
                fn.addFnAttr(llvm::Attribute::SanitizeAddress);

        // (2) Run the module-level AddressSanitizerPass via the new PassManager.
        // In LLVM's new PM (14+) a single module pass, AddressSanitizerPass,
        // subsumes the old split of ModuleAddressSanitizer + the per-function
        // AddressSanitizer legacy passes: it inserts `asan.module_ctor`/globals
        // instrumentation at module scope and the per-function load/store shadow
        // checks in one run over the module.
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb(tm);
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        llvm::ModulePassManager mpm;
        mpm.addPass(llvm::AddressSanitizerPass(llvm::AddressSanitizerOptions{}));
        mpm.run(module, mam);
    }

    std::error_code ec;
    llvm::raw_fd_ostream os(objectPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        error = "could not open '" + objectPath + "': " + ec.message();
        return false;
    }

    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, os, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
        error = "target cannot emit an object file";
        return false;
    }
    pm.run(module);
    os.flush();
    return true;
}

}  // namespace bezel::codegen
