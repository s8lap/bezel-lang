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

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

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
// `opts` allows per-call codegen options (default = unchecked release mode).
inline JitResult runJitWithOptions(std::string_view source,
                                   const codegen::CodegenOptions& opts) {
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
    auto module = codegen::emitModule(*unit, ctx, *llvmCtx, opts);
    if (!module) return r;

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        llvm::consumeError(jitOrErr.takeError());
        return r;
    }
    auto jit = std::move(*jitOrErr);

    // Resolve external symbols (calloc, and FFI calls like putchar/abs)
    // against the host process so extern declarations can be invoked.
    if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
    } else {
        llvm::consumeError(gen.takeError());
    }

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

// Convenience: run in unchecked (default release) mode.
inline JitResult runJit(std::string_view source) {
    return runJitWithOptions(source, codegen::CodegenOptions{});
}

// §28.2 v0.134 / issue #601: run in checked disposal mode. Equivalent to
// runJit but with checkedDisposal=true: instance-member entry guards are
// emitted, and DisposedAccessException is raised on use-after-dispose.
inline JitResult runJitChecked(std::string_view source) {
    codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    return runJitWithOptions(source, opts);
}

struct EmitResult {
    DiagnosticEngine diagnostics;
    bool lowered = false;       // true once a module was produced (no FE errors)
    bool verifies = false;      // true if llvm::verifyModule found NO errors
};

// Lex + parse + sema + lower, then run llvm::verifyModule on the emitted module
// WITHOUT requiring a `main` (so a declaration-only library fragment — e.g. a
// top-level `extension` block — can be checked). `verifies` is true only when
// the module is structurally valid IR. Used to assert that a function body
// emits well-typed IR (e.g. a generic-extension template stub returning the
// correct LLVM type, not `ret i32` into a `ptr` function — issue #699 / §26.4).
inline EmitResult emitAndVerify(std::string_view source) {
    ensureNativeTargetInitialized();
    EmitResult r;

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
    auto module = codegen::emitModule(*unit, ctx, *llvmCtx,
                                      codegen::CodegenOptions{});
    if (!module) return r;
    r.lowered = true;

    // llvm::verifyModule returns TRUE when the module is BROKEN; invert it.
    r.verifies = !llvm::verifyModule(*module, &llvm::errs());
    return r;
}

}  // namespace bezel::test
