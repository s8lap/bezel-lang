#include "codegen/LlvmBackend.hpp"

#include <cstdint>
#include <string>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

namespace bezel::codegen {

namespace {

// Parse a Bezel integer literal lexeme to its value. Slice CG1 handles
// decimal and the `0x` / `0b` / `0o` bases, with `_` digit separators
// stripped and any trailing type-suffix letters ignored. Returns 0 on a
// shape it doesn't recognize (the lexer already validated the literal).
std::int64_t parseIntLiteral(std::string_view text) {
    std::string digits;
    int base = 10;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char b = text[1];
        if (b == 'x' || b == 'X') { base = 16; i = 2; }
        else if (b == 'b' || b == 'B') { base = 2; i = 2; }
        else if (b == 'o' || b == 'O') { base = 8; i = 2; }
    }
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        // Stop at a type-suffix letter (e.g. `u`, `l`) for decimal; for
        // hex, hex-digits are consumed and only non-hex letters stop us.
        bool isDigit = (c >= '0' && c <= '9');
        bool isHex = isDigit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (base == 16 ? !isHex : !isDigit) break;
        digits.push_back(c);
    }
    if (digits.empty()) return 0;
    return static_cast<std::int64_t>(std::strtoll(digits.c_str(), nullptr, base));
}

// Per-module emitter. Holds the LLVM context/module/builder and lowers the
// checked AST. Grows one expression/statement kind per codegen slice.
class Emitter {
public:
    Emitter(const ast::SourceFile& src, sema::SemaContext& ctx,
            llvm::LLVMContext& llvmCtx)
        : src_(src), ctx_(ctx), llvm_(llvmCtx),
          module_(std::make_unique<llvm::Module>("bezel", llvmCtx)),
          builder_(llvmCtx) {}

    std::unique_ptr<llvm::Module> run() {
        emitMain();
        return std::move(module_);
    }

private:
    const ast::SourceFile& src_;
    sema::SemaContext& ctx_;
    llvm::LLVMContext& llvm_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;

    llvm::Type* i32() { return llvm::Type::getInt32Ty(llvm_); }

    // §33 entry point: the first `public static method Main` found in any
    // top-level class. Multi-/missing-Main diagnostics are a later concern.
    const ast::MethodDecl* findMain() {
        for (const auto& d : src_.topDecls) {
            auto* td = dynamic_cast<const ast::TypeDecl*>(d.get());
            if (!td) continue;
            for (const auto& m : td->members) {
                auto* md = dynamic_cast<const ast::MethodDecl*>(m.get());
                if (!md || md->name != "Main") continue;
                bool isStatic = false;
                for (auto mod : md->modifiers) {
                    if (mod == "static") { isStatic = true; break; }
                }
                if (isStatic) return md;
            }
        }
        return nullptr;
    }

    void emitMain() {
        llvm::FunctionType* fnType =
            llvm::FunctionType::get(i32(), /*isVarArg=*/false);
        llvm::Function* mainFn = llvm::Function::Create(
            fnType, llvm::Function::ExternalLinkage, "main", module_.get());
        builder_.SetInsertPoint(
            llvm::BasicBlock::Create(llvm_, "entry", mainFn));

        const ast::MethodDecl* main = findMain();
        llvm::Value* ret = nullptr;
        // Slice CG1: an expression-bodied `Main(): int` returns its
        // (integer) expression. Other shapes fall back to 0 until later
        // slices lower them.
        if (main && main->bodyKind == ast::MethodBodyKind::Expression &&
            main->exprBody) {
            ret = lowerExpr(*main->exprBody);
        }
        if (!ret) ret = llvm::ConstantInt::get(i32(), 0);
        builder_.CreateRet(ret);
    }

    // Lower an expression to an LLVM value. Slice CG1: integer literals.
    llvm::Value* lowerExpr(const ast::Expr& e) {
        if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
            return llvm::ConstantInt::get(
                i32(),
                static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
                /*isSigned=*/true);
        }
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx) {
    return Emitter(src, ctx, llvmCtx).run();
}

}  // namespace bezel::codegen
