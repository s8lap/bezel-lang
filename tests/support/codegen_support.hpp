#pragma once

// Test support for the codegen milestone. Runs the full front-end on a source
// string, lowers it to an LLVM module, and JIT-executes `main` via LLVM ORC,
// returning the integer it produced (the program's exit code). JIT keeps the
// tests hermetic and fast — no external assembler/linker step.

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/TargetSelect.h"

namespace bezel::test {

struct JitResult {
    DiagnosticEngine diagnostics;
    bool ran = false;     // true once `main` executed
    int exitCode = 0;     // value returned by `main`
};

// Initialize the native target exactly once per process (required before the
// ORC JIT can emit machine code for the host).
inline void ensureNativeTargetInitialized() {
    static const bool once = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        return true;
    }();
    (void)once;
}

// Lex + parse + sema, then lower and JIT-run. On any front-end error (or JIT
// failure) `ran` stays false; callers assert on `diagnostics`/`ran`/`exitCode`.
inline JitResult runJit(std::string_view source) {
    ensureNativeTargetInitialized();
    JitResult r;

    Lexer lexer(source, r.diagnostics);
    Parser parser(lexer, r.diagnostics);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return r;

    sema::SemaContext ctx(r.diagnostics);
    sema::collectSymbols(*unit, ctx);
    sema::resolveTypes(*unit, ctx);
    sema::typecheckExpressions(*unit, ctx);
    if (r.diagnostics.hasErrors()) return r;

    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = codegen::emitModule(*unit, ctx, *llvmCtx);
    if (!module) return r;

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        llvm::consumeError(jitOrErr.takeError());
        return r;
    }
    auto jit = std::move(*jitOrErr);

    // ThreadSafeModule takes ownership of both the module and its context.
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(llvmCtx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        return r;
    }

    auto sym = jit->lookup("main");
    if (!sym) {
        llvm::consumeError(sym.takeError());
        return r;
    }
    auto fn = sym->toPtr<int (*)()>();
    r.exitCode = fn();
    r.ran = true;
    return r;
}

}  // namespace bezel::test
