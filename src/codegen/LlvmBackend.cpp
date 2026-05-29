#include "codegen/LlvmBackend.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace bezel::codegen {

std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx) {
    (void)src;
    (void)ctx;
    auto module = std::make_unique<llvm::Module>("bezel", llvmCtx);

    // Slice CG0: a `main` that returns 0. Real entry-point lowering (§33,
    // mapping Bezel's `Main` to this function) lands in CG1+.
    llvm::Type* i32 = llvm::Type::getInt32Ty(llvmCtx);
    llvm::FunctionType* fnType = llvm::FunctionType::get(i32, /*isVarArg=*/false);
    llvm::Function* mainFn = llvm::Function::Create(
        fnType, llvm::Function::ExternalLinkage, "main", module.get());

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(llvmCtx, "entry", mainFn);
    llvm::IRBuilder<> builder(entry);
    builder.CreateRet(llvm::ConstantInt::get(i32, 0));

    return module;
}

}  // namespace bezel::codegen
