#pragma once

// Test support for the Flow milestone — drives parse + cfg-build on a
// method body, returning the constructed CFG so tests can assert on
// block counts and edges.

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "flow/CFG.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::test {

struct CFGResult {
    std::unique_ptr<ast::SourceFile> sourceFile;
    DiagnosticEngine diagnostics;
    std::unique_ptr<flow::CFG> cfg;
};

// Lex + parse a source string, find the first method's body, and build
// a CFG for it. Returns null cfg if the source has no method.
inline CFGResult parseFirstMethodCFG(std::string_view source) {
    CFGResult r;
    Lexer lexer(source, r.diagnostics);
    Parser parser(lexer, r.diagnostics);
    r.sourceFile = parser.parseCompilationUnit();
    if (!r.sourceFile) return r;
    for (const auto& td : r.sourceFile->topDecls) {
        const auto* type = dynamic_cast<const ast::TypeDecl*>(td.get());
        if (!type) continue;
        for (const auto& m : type->members) {
            const auto* method = dynamic_cast<const ast::MethodDecl*>(m.get());
            if (method && method->bodyKind == ast::MethodBodyKind::Block) {
                r.cfg = flow::buildCFG(method->stmts);
                return r;
            }
        }
    }
    return r;
}

} // namespace bezel::test
