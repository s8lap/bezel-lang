#pragma once

// Test support for the parser milestone: lex+parse a source string and
// inspect the resulting AST and diagnostics. Mirrors the role of
// lex_support.hpp for the lexer.

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

namespace bezel::test {

// Single result of running the lex+parse pipeline on a source string. The
// diagnostic engine is held by value so the test can inspect it after the
// parser is gone.
struct ParseResult {
    std::unique_ptr<ast::SourceFile> sourceFile;
    DiagnosticEngine diagnostics;
};

inline ParseResult parseAll(std::string_view source) {
    ParseResult result;
    Lexer lexer(source, result.diagnostics);
    Parser parser(lexer, result.diagnostics);
    result.sourceFile = parser.parseCompilationUnit();
    return result;
}

// Flatten a QualifiedName to its dotted form (`Foo.Bar.Baz`). Returns an empty
// string for an empty name.
inline std::string qualifiedNameString(const ast::QualifiedName& name) {
    std::string out;
    for (std::size_t i = 0; i < name.parts.size(); ++i) {
        if (i > 0) out += '.';
        out += std::string(name.parts[i]);
    }
    return out;
}

// Convenience accessor: the source file's module name, or a sentinel if any
// link in the chain is missing. Lets tests assert on the dotted name without
// repeating null-checks.
inline std::string moduleName(const ParseResult& result) {
    if (!result.sourceFile || !result.sourceFile->module ||
        !result.sourceFile->module->name) {
        return "<no-module>";
    }
    return qualifiedNameString(*result.sourceFile->module->name);
}

// The source file's import names, in declaration order. Missing or malformed
// imports produce a "<invalid>" placeholder so tests can still assert on the
// shape of the import list.
inline std::vector<std::string> importNames(const ParseResult& result) {
    std::vector<std::string> out;
    if (!result.sourceFile) return out;
    for (const auto& imp : result.sourceFile->imports) {
        if (imp && imp->name) {
            out.push_back(qualifiedNameString(*imp->name));
        } else {
            out.push_back("<invalid>");
        }
    }
    return out;
}

// Render a Type AST back to its canonical text form (e.g. `List<int?>?`).
// Recursive over generic args.
inline std::string typeToString(const ast::Type& t) {
    std::string out;
    if (t.name) out += qualifiedNameString(*t.name);
    if (!t.genericArgs.empty()) {
        out += '<';
        for (std::size_t i = 0; i < t.genericArgs.size(); ++i) {
            if (i > 0) out += ", ";
            out += t.genericArgs[i] ? typeToString(*t.genericArgs[i])
                                    : std::string("<null>");
        }
        out += '>';
    }
    if (t.nullable) out += '?';
    return out;
}

// One-line human-readable summary of a top-level declaration, e.g.
// "public sealed class Foo", "enum E", or "type X = List<int>". Lets tests
// assert the visible shape of each decl in a single string comparison.
inline std::string topDeclSummary(const ast::TopDecl& decl) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&decl)) {
        std::string out;
        switch (td->visibility) {
            case ast::Visibility::Public:    out += "public ";    break;
            case ast::Visibility::Private:   out += "private ";   break;
            case ast::Visibility::Internal:  out += "internal ";  break;
            case ast::Visibility::Protected: out += "protected "; break;
            case ast::Visibility::None: break;
        }
        for (std::string_view m : td->modifiers) {
            out += std::string(m);
            out += ' ';
        }
        switch (td->kind) {
            case ast::TypeDeclKind::Class:     out += "class ";     break;
            case ast::TypeDeclKind::Interface: out += "interface "; break;
            case ast::TypeDeclKind::Record:    out += "record ";    break;
            case ast::TypeDeclKind::Enum:      out += "enum ";      break;
        }
        out += std::string(td->name);
        if (!td->genericParams.empty()) {
            out += '<';
            for (std::size_t i = 0; i < td->genericParams.size(); ++i) {
                if (i > 0) out += ", ";
                if (!td->genericParams[i].variance.empty()) {
                    out += std::string(td->genericParams[i].variance);
                    out += ' ';
                }
                out += std::string(td->genericParams[i].name);
            }
            out += '>';
        }
        if (!td->baseTypes.empty()) {
            out += ": ";
            for (std::size_t i = 0; i < td->baseTypes.size(); ++i) {
                if (i > 0) out += ", ";
                out += td->baseTypes[i] ? typeToString(*td->baseTypes[i])
                                        : std::string("<null>");
            }
        }
        return out;
    }
    if (auto* ta = dynamic_cast<const ast::TypeAlias*>(&decl)) {
        std::string out = "type ";
        out += std::string(ta->name);
        out += " = ";
        out += ta->aliasedType ? typeToString(*ta->aliasedType)
                               : std::string("<null>");
        return out;
    }
    return "<unknown>";
}

inline std::vector<std::string> topDeclSummaries(const ParseResult& result) {
    std::vector<std::string> out;
    if (!result.sourceFile) return out;
    for (const auto& d : result.sourceFile->topDecls) {
        out.push_back(d ? topDeclSummary(*d) : std::string("<invalid>"));
    }
    return out;
}

} // namespace bezel::test
