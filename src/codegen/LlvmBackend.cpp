#include "codegen/LlvmBackend.hpp"
#include "codegen/LlvmBackend_impl.hpp"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

namespace bezel::codegen {

// The Emitter class is declared in LlvmBackend_impl.hpp; its method
// definitions live in the sibling LlvmBackend_*.cpp translation units. This
// file holds the public entry points.

std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx) {
    // Codegen works off the SemaContext symbol table (each symbol points at
    // its AST decl), so the SourceFile root isn't needed directly; the
    // parameter is kept for API stability / future use.
    (void)src;
    auto m = Emitter(ctx, llvmCtx).run();
    if (m && std::getenv("BEZEL_DUMP_IR")) m->print(llvm::errs(), nullptr);
    return m;
}

bool emitObjectFile(llvm::Module& module, const std::string& objectPath,
                    std::string& error) {
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

    static const bool inited = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        return true;
    }();
    (void)inited;

    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple.str(), error);
    if (!target) return false;

    llvm::TargetOptions opts;
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, "generic", "", opts, llvm::Reloc::PIC_));
    if (!tm) {
        error = "could not create target machine";
        return false;
    }

    module.setTargetTriple(triple);
    module.setDataLayout(tm->createDataLayout());

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
