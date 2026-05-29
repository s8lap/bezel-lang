#include "codegen/LlvmBackend.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

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
    llvm::Function* currentFn_ = nullptr;
    // CG3: each local's storage, keyed by its declaration node (a
    // LocalDecl/LocalConstDecl). An IdentifierExpr resolves to a
    // LocalSymbol whose `decl` is that node.
    std::unordered_map<const ast::Node*, llvm::AllocaInst*> locals_;

    llvm::Type* i32() { return llvm::Type::getInt32Ty(llvm_); }
    llvm::Type* i1() { return llvm::Type::getInt1Ty(llvm_); }

    // Create an alloca in the function's entry block (the conventional
    // home for locals, so they dominate all uses once control flow lands).
    llvm::AllocaInst* entryAlloca(llvm::Type* ty) {
        llvm::BasicBlock& entry = currentFn_->getEntryBlock();
        llvm::IRBuilder<> tmp(&entry, entry.begin());
        return tmp.CreateAlloca(ty);
    }

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
        currentFn_ = mainFn;
        builder_.SetInsertPoint(
            llvm::BasicBlock::Create(llvm_, "entry", mainFn));

        const ast::MethodDecl* main = findMain();
        bool terminated = false;
        if (main && main->bodyKind == ast::MethodBodyKind::Expression &&
            main->exprBody) {
            // CG1/CG2: `Main(): int => <expr>`.
            llvm::Value* v = lowerExpr(*main->exprBody);
            builder_.CreateRet(v ? v : llvm::ConstantInt::get(i32(), 0));
            terminated = true;
        } else if (main && main->bodyKind == ast::MethodBodyKind::Block) {
            // CG3: a block body — lower statements until one terminates.
            terminated = lowerBody(main->stmts);
        }
        // Fall-through (void Main or unterminated path): exit 0.
        if (!terminated) builder_.CreateRet(llvm::ConstantInt::get(i32(), 0));
    }

    // Lower a statement list into the current block; returns true if the
    // list terminated the block (so the caller stops adding to it).
    bool lowerBody(const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        for (const auto& s : body) {
            if (s && lowerStmt(*s)) return true;
        }
        return false;
    }

    // Lower one statement into the current block. Returns true if the
    // statement terminated the block (a return), so the caller stops.
    // CG3: LocalDecl/LocalConstDecl, AssignmentStmt, ReturnStmt,
    // ExpressionStmt, DiscardStmt.
    bool lowerStmt(const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            llvm::AllocaInst* slot = entryAlloca(i32());
            locals_[ld] = slot;
            if (ld->initializer) {
                if (llvm::Value* v = lowerExpr(*ld->initializer)) {
                    builder_.CreateStore(v, slot);
                }
            }
            return false;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            llvm::AllocaInst* slot = entryAlloca(i32());
            locals_[lc] = slot;
            if (lc->value) {
                if (llvm::Value* v = lowerExpr(*lc->value)) {
                    builder_.CreateStore(v, slot);
                }
            }
            return false;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            if (as->op == ":=" && as->target && as->value) {
                if (llvm::AllocaInst* slot = localSlot(*as->target)) {
                    if (llvm::Value* v = lowerExpr(*as->value)) {
                        builder_.CreateStore(v, slot);
                    }
                }
            }
            return false;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            llvm::Value* v = rs->value ? lowerExpr(*rs->value) : nullptr;
            builder_.CreateRet(v ? v : llvm::ConstantInt::get(i32(), 0));
            return true;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            if (es->expr) lowerExpr(*es->expr);  // side effects only (CG3: none)
            return false;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            if (ds->value) lowerExpr(*ds->value);
            return false;
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            return lowerIf(*ifs);
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            return lowerWhile(*ws);
        }
        return false;  // unhandled statement kinds land in later slices
    }

    // CG5: if / elif* / else. Each condition is tested in its own block;
    // a branch that doesn't terminate falls through to a lazily-created
    // merge block. Returns true only when every path terminates (no
    // merge needed) — so we never leave a danging unterminated block.
    bool lowerIf(const ast::IfStmt& ifs) {
        llvm::BasicBlock* mergeBB = nullptr;
        auto merge = [&]() -> llvm::BasicBlock* {
            if (!mergeBB) mergeBB = llvm::BasicBlock::Create(
                llvm_, "if.end", currentFn_);
            return mergeBB;
        };

        // The if-branch and each elif form a (condition, body) test chain.
        std::vector<const ast::IfStmt::Branch*> chain;
        chain.push_back(&ifs.ifBranch);
        for (const auto& el : ifs.elifBranches) chain.push_back(&el);

        bool allPathsTerminate = true;
        for (const ast::IfStmt::Branch* br : chain) {
            llvm::Value* cond =
                br->condition ? lowerExpr(*br->condition) : nullptr;
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "if.body", currentFn_);
            auto* nextBB = llvm::BasicBlock::Create(llvm_, "if.next", currentFn_);
            if (cond) builder_.CreateCondBr(cond, bodyBB, nextBB);
            else builder_.CreateBr(nextBB);  // defensive

            builder_.SetInsertPoint(bodyBB);
            if (!lowerBody(br->body)) {
                allPathsTerminate = false;
                builder_.CreateBr(merge());
            }
            builder_.SetInsertPoint(nextBB);  // next test / else goes here
        }

        // The final fall-through block is the else (or an empty pass-through).
        if (ifs.hasElse) {
            if (!lowerBody(ifs.elseBody)) {
                allPathsTerminate = false;
                builder_.CreateBr(merge());
            }
        } else {
            allPathsTerminate = false;
            builder_.CreateBr(merge());
        }

        if (mergeBB) {
            builder_.SetInsertPoint(mergeBB);
            return false;
        }
        return allPathsTerminate;
    }

    // CG5: while loop. cond block tests, body block loops back. break /
    // continue are not yet lowered (deferred; no test uses them).
    bool lowerWhile(const ast::WhileStmt& ws) {
        auto* condBB = llvm::BasicBlock::Create(llvm_, "while.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "while.body", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "while.end", currentFn_);

        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* cond = ws.condition ? lowerExpr(*ws.condition) : nullptr;
        if (cond) builder_.CreateCondBr(cond, bodyBB, endBB);
        else builder_.CreateBr(endBB);

        builder_.SetInsertPoint(bodyBB);
        if (!lowerBody(ws.body)) builder_.CreateBr(condBB);  // back-edge

        builder_.SetInsertPoint(endBB);
        return false;  // control may always exit the loop
    }

    // If `e` names a local variable, return its storage slot; else null.
    llvm::AllocaInst* localSlot(const ast::Expr& e) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e);
        if (!id) return nullptr;
        sema::Symbol* sym = ctx_.exprSymbol(e);
        auto* loc = dynamic_cast<sema::LocalSymbol*>(sym);
        if (!loc || !loc->decl) return nullptr;
        auto it = locals_.find(loc->decl);
        return it == locals_.end() ? nullptr : it->second;
    }

    // Lower an expression to an LLVM value.
    //   CG1: integer literals.
    //   CG2: integer arithmetic / bitwise / shift binary ops, unary -/~/+.
    llvm::Value* lowerExpr(const ast::Expr& e) {
        if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
            return llvm::ConstantInt::get(
                i32(),
                static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
                /*isSigned=*/true);
        }
        if (auto* boolLit = dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
            return llvm::ConstantInt::get(i1(), boolLit->value ? 1 : 0);
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            return lowerBinary(*bin);
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            return lowerUnary(*un);
        }
        if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            return lowerConditional(*cond);
        }
        // CG3: a reference to a local variable loads from its slot.
        if (dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            if (llvm::AllocaInst* slot = localSlot(e)) {
                return builder_.CreateLoad(slot->getAllocatedType(), slot);
            }
        }
        return nullptr;
    }

    // Slice CG2: integer binary operators. NOTE (§6.1): `+ - * / mod`
    // are *overflow-checked* in Bezel (raise OverflowException) and
    // `/`÷0 raises DivideByZeroException — those checks need the
    // exception runtime (§31), which is not built yet, so this slice
    // emits the value computation only (correct for the non-overflowing,
    // non-zero-divisor case). The checks land with exceptions codegen.
    // Bitwise/shift ops are not overflow-checked, so they are exact now.
    llvm::Value* lowerBinary(const ast::BinaryExpr& bin) {
        if (!bin.left || !bin.right) return nullptr;
        llvm::Value* l = lowerExpr(*bin.left);
        llvm::Value* r = lowerExpr(*bin.right);
        if (!l || !r) return nullptr;
        const auto& op = bin.op;
        if (op == "+") return builder_.CreateAdd(l, r);
        if (op == "-") return builder_.CreateSub(l, r);
        if (op == "*") return builder_.CreateMul(l, r);
        if (op == "/") return builder_.CreateSDiv(l, r);
        if (op == "mod") return builder_.CreateSRem(l, r);
        if (op == "&") return builder_.CreateAnd(l, r);
        if (op == "|") return builder_.CreateOr(l, r);
        if (op == "^") return builder_.CreateXor(l, r);
        if (op == "<<") return builder_.CreateShl(l, r);
        if (op == ">>") return builder_.CreateAShr(l, r);  // signed types
        // CG4: integer comparisons → i1 (Bezel uses `=` / `<>` for
        // equality, signed relational ops).
        if (op == "=") return builder_.CreateICmpEQ(l, r);
        if (op == "<>") return builder_.CreateICmpNE(l, r);
        if (op == "<") return builder_.CreateICmpSLT(l, r);
        if (op == ">") return builder_.CreateICmpSGT(l, r);
        if (op == "<=") return builder_.CreateICmpSLE(l, r);
        if (op == ">=") return builder_.CreateICmpSGE(l, r);
        // CG4: logical and/or on i1. NOTE: §17 specifies short-circuit
        // evaluation; this eager form is observationally identical while
        // operands are side-effect-free (no calls lowered yet) — true
        // short-circuiting lands once call lowering exists.
        if (op == "and") return builder_.CreateAnd(l, r);
        if (op == "or") return builder_.CreateOr(l, r);
        return nullptr;
    }

    llvm::Value* lowerUnary(const ast::UnaryExpr& un) {
        if (!un.operand) return nullptr;
        llvm::Value* v = lowerExpr(*un.operand);
        if (!v) return nullptr;
        const auto& op = un.op;
        if (op == "-") return builder_.CreateNeg(v);   // §6.1 checked; see above
        if (op == "~") return builder_.CreateNot(v);   // bitwise complement
        if (op == "+") return v;                       // identity
        if (op == "not") return builder_.CreateNot(v); // i1 logical negation
        return nullptr;
    }

    // CG4: the conditional expression `thenValue if condition else
    // elseValue` (§20). Lowers to a branch + phi so only the taken
    // branch's value expression is evaluated (lazy, per spec).
    llvm::Value* lowerConditional(const ast::ConditionalExpr& c) {
        if (!c.condition || !c.thenValue || !c.elseValue) return nullptr;
        llvm::Value* cond = lowerExpr(*c.condition);
        if (!cond) return nullptr;

        auto* thenBB = llvm::BasicBlock::Create(llvm_, "cond.then", currentFn_);
        auto* elseBB = llvm::BasicBlock::Create(llvm_, "cond.else", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "cond.end", currentFn_);
        builder_.CreateCondBr(cond, thenBB, elseBB);

        builder_.SetInsertPoint(thenBB);
        llvm::Value* tv = lowerExpr(*c.thenValue);
        builder_.CreateBr(endBB);
        llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();

        builder_.SetInsertPoint(elseBB);
        llvm::Value* ev = lowerExpr(*c.elseValue);
        builder_.CreateBr(endBB);
        llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();

        builder_.SetInsertPoint(endBB);
        if (!tv || !ev) return nullptr;
        llvm::PHINode* phi = builder_.CreatePHI(tv->getType(), 2);
        phi->addIncoming(tv, thenEnd);
        phi->addIncoming(ev, elseEnd);
        return phi;
    }
};

}  // namespace

std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx) {
    return Emitter(src, ctx, llvmCtx).run();
}

}  // namespace bezel::codegen
