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

// §28.3 v0.150 (#1008 Part B): true when the ownership pass emitted the affine
// auto-drop must-use WARNING for an undischarged plain-`var` owned local (the
// value is implicitly disposed at scope end rather than rejected). Optionally
// scoped to a named local.
inline bool hasAutoDropWarning(const OwnershipResult& r,
                               const std::string& name = "") {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Warning) continue;
        // Matches both the local ("end of its scope") and parameter ("end of
        // the method") auto-drop warnings (§28.3 v0.150/v0.152).
        if (d.message.find("is implicitly disposed at the end of") ==
            std::string::npos)
            continue;
        if (name.empty() ||
            d.message.find("`" + name + "`") != std::string::npos)
            return true;
    }
    return false;
}

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
