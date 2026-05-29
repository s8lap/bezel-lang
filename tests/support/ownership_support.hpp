#pragma once

// Test support for the ownership milestone. Drives the full pipeline
// (lex + parse + sema + ownership) and exposes the result for tests.

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::test {

struct OwnershipResult {
    std::unique_ptr<ast::SourceFile> sourceFile;
    DiagnosticEngine diagnostics;
    std::unique_ptr<sema::SemaContext> ctx;
};

inline OwnershipResult parseAndCheckOwnership(std::string_view source) {
    OwnershipResult r;
    Lexer lexer(source, r.diagnostics);
    Parser parser(lexer, r.diagnostics);
    r.sourceFile = parser.parseCompilationUnit();
    r.ctx = std::make_unique<sema::SemaContext>(r.diagnostics);
    if (r.sourceFile) {
        sema::collectSymbols(*r.sourceFile, *r.ctx);
        sema::resolveTypes(*r.sourceFile, *r.ctx);
        sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        ownership::checkOwnership(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace bezel::test
