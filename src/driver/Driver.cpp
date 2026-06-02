#include "driver/Driver.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

#include <cstdlib>

#include "codegen/CodeGen.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "ownership/OwnershipChecker.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeChecker.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace bezel {

namespace {

// Reads an entire file into `out`. Returns false if the file cannot be opened.
bool readFile(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

// Link a native object into an executable using clang (the project's
// toolchain). Returns true on success.
bool linkExecutable(const std::string& objectPath,
                    const std::string& exePath) {
    // Link with libc, libm, libpthread, and the Bezel runtime library.
    std::string cmd = "clang " + objectPath + " -o " + exePath + " -lm -lpthread";
#ifdef BEZEL_RUNTIME_LIB_PATH
    // Static-link the Bezel runtime (path injected at compile time by CMake).
    cmd += " ";
    cmd += BEZEL_RUNTIME_LIB_PATH;
#endif
    cmd += " -lstdc++";  // runtime uses C++ std (std::mutex, std::thread, etc.)
    return std::system(cmd.c_str()) == 0;
}

} // namespace

CompileResult Driver::compileFile(const std::string& path,
                                  const std::string& outputPath) {
    std::string source;
    if (!readFile(path, source)) {
        std::cerr << "bezelc: error: cannot open '" << path << "'\n";
        return CompileResult{false, 1};
    }

    DiagnosticEngine diagnostics;

    // Phase pipeline (§2, §33). Each phase is a stub today; this establishes
    // the order in which the milestones will fill them in.
    Lexer lexer(source, diagnostics);
    Parser parser(lexer, diagnostics);
    auto unit = parser.parseCompilationUnit();

    if (unit) {
        // Sema milestone — runs in pass order: symbol collection,
        // type resolution, expression typecheck. Each pass annotates
        // the AST via side-tables on SemaContext.
        sema::SemaContext semaCtx(diagnostics);
        sema::collectSymbols(*unit, semaCtx);
        sema::resolveTypes(*unit, semaCtx);
        sema::typecheckExpressions(*unit, semaCtx);
        // Definite-assignment runs after expression typecheck so it
        // can see the same scope structure ExprTypecheck built.
        sema::checkDefiniteAssignment(*unit, semaCtx);

        // Ownership milestone — runs after sema since it needs
        // resolved local symbols + producer info.
        ownership::checkOwnership(*unit, semaCtx);

        // Stub passes kept around so the milestone scaffolding doesn't
        // bit-rot — they no-op until their own sub-milestones land.
        TypeChecker typeChecker(diagnostics);
        typeChecker.check(*unit);

        OwnershipChecker ownershipChecker(diagnostics);
        ownershipChecker.check(*unit);

        // Codegen: when an output path is requested and the front-end is
        // clean, lower to LLVM, emit a native object, and link an
        // executable with clang.
        if (!outputPath.empty() && !diagnostics.hasErrors()) {
            llvm::LLVMContext llvmCtx;
            auto module = codegen::emitModule(*unit, semaCtx, llvmCtx);
            std::string objectPath = outputPath + ".o";
            std::string err;
            if (!module || !codegen::emitObjectFile(*module, objectPath, err)) {
                std::cerr << "bezelc: codegen error: " << err << "\n";
                return CompileResult{false, diagnostics.errorCount() + 1};
            }
            if (!linkExecutable(objectPath, outputPath)) {
                std::cerr << "bezelc: link error\n";
                std::remove(objectPath.c_str());
                return CompileResult{false, diagnostics.errorCount() + 1};
            }
            std::remove(objectPath.c_str());
        }
    }

    for (const Diagnostic& diag : diagnostics.diagnostics()) {
        std::cerr << path << ":" << diag.location.line << ":"
                  << diag.location.column << ": " << diag.message << "\n";
    }

    CompileResult result;
    result.errorCount = diagnostics.errorCount();
    result.success = !diagnostics.hasErrors();
    return result;
}

} // namespace bezel
