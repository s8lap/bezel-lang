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

// Render a visibility keyword (or empty for None). Trailing space included
// when non-empty so it slots into a member summary cleanly.
inline std::string visibilityPrefix(ast::Visibility v) {
    switch (v) {
        case ast::Visibility::Public:    return "public ";
        case ast::Visibility::Private:   return "private ";
        case ast::Visibility::Internal:  return "internal ";
        case ast::Visibility::Protected: return "protected ";
        case ast::Visibility::None:      return "";
    }
    return "";
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

// Render an expression AST back to a canonical string form. Recursive on
// composite expressions (member access, call, paren).
inline std::string exprToString(const ast::Expr& e) {
    if (auto* i = dynamic_cast<const ast::IntLiteralExpr*>(&e))
        return std::string(i->text);
    if (auto* f = dynamic_cast<const ast::FloatLiteralExpr*>(&e))
        return std::string(f->text);
    if (auto* c = dynamic_cast<const ast::CharLiteralExpr*>(&e))
        return std::string(c->text);
    if (auto* s = dynamic_cast<const ast::StringLiteralExpr*>(&e))
        return std::string(s->text);
    if (auto* v = dynamic_cast<const ast::VerbatimStringExpr*>(&e))
        return std::string(v->text);
    if (auto* b = dynamic_cast<const ast::BoolLiteralExpr*>(&e))
        return std::string(b->value ? "true" : "false");
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e))
        return std::string("null");
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e))
        return std::string(id->name);
    if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        return (m->target ? exprToString(*m->target) : std::string("<null>")) +
               "." + std::string(m->member);
    }
    if (auto* c = dynamic_cast<const ast::CallExpr*>(&e)) {
        std::string out = c->callee ? exprToString(*c->callee)
                                    : std::string("<null>");
        out += '(';
        for (std::size_t i = 0; i < c->args.size(); ++i) {
            if (i > 0) out += ", ";
            out += c->args[i] ? exprToString(*c->args[i])
                              : std::string("<null>");
        }
        out += ')';
        return out;
    }
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        // Always wrap in explicit parens so precedence and associativity are
        // observable in test assertions without ambiguity.
        std::string out = "(";
        out += b->left ? exprToString(*b->left) : std::string("<null>");
        out += ' ';
        out += std::string(b->op);
        out += ' ';
        out += b->right ? exprToString(*b->right) : std::string("<null>");
        out += ')';
        return out;
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        // Keyword-spelled unary operators (`not`) need a space; the
        // punctuator-spelled ones (`+ - ~`) don't.
        const bool needsSpace = (u->op == "not");
        std::string out = "(";
        out += std::string(u->op);
        if (needsSpace) out += ' ';
        out += u->operand ? exprToString(*u->operand) : std::string("<null>");
        out += ')';
        return out;
    }
    return "<unknown expr>";
}

// Render one statement to a single source-like string. Slice 11 covers
// LocalDecl, ReturnStmt, AssignmentStmt, ExpressionStmt.
inline std::string stmtSummary(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        std::string out = ld->isUsing ? "using var " : "var ";
        out += std::string(ld->name);
        if (ld->type) {
            out += ": ";
            out += typeToString(*ld->type);
        }
        if (ld->initializer) {
            out += " := ";
            out += exprToString(*ld->initializer);
        }
        out += ';';
        return out;
    }
    if (auto* r = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        std::string out = "return";
        if (r->value) {
            out += ' ';
            out += exprToString(*r->value);
        }
        out += ';';
        return out;
    }
    if (auto* a = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        std::string out = a->target ? exprToString(*a->target)
                                    : std::string("<null>");
        out += ' ';
        out += std::string(a->op);
        out += ' ';
        out += a->value ? exprToString(*a->value) : std::string("<null>");
        out += ';';
        return out;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        std::string out = es->expr ? exprToString(*es->expr)
                                   : std::string("<null>");
        out += ';';
        return out;
    }
    auto bodyToString =
        [](const std::vector<std::unique_ptr<ast::Stmt>>& body) {
            std::string b = "{ ";
            for (std::size_t i = 0; i < body.size(); ++i) {
                if (i > 0) b += ' ';
                b += body[i] ? stmtSummary(*body[i]) : std::string("<null>");
            }
            b += " }";
            return b;
        };
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        std::string out = "if ";
        out += ifs->ifBranch.condition
                   ? exprToString(*ifs->ifBranch.condition)
                   : std::string("<null>");
        out += " then ";
        out += bodyToString(ifs->ifBranch.body);
        for (const auto& elif : ifs->elifBranches) {
            out += " elif ";
            out += elif.condition ? exprToString(*elif.condition)
                                  : std::string("<null>");
            out += " then ";
            out += bodyToString(elif.body);
        }
        if (ifs->hasElse) {
            out += " else ";
            out += bodyToString(ifs->elseBody);
        }
        out += " end;";
        return out;
    }
    if (auto* w = dynamic_cast<const ast::WhileStmt*>(&s)) {
        std::string out = "while ";
        out += w->condition ? exprToString(*w->condition)
                            : std::string("<null>");
        out += ' ';
        out += bodyToString(w->body);
        out += " end;";
        return out;
    }
    if (auto* d = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        std::string out = "do ";
        out += bodyToString(d->body);
        out += " end;";
        return out;
    }
    if (dynamic_cast<const ast::BreakStmt*>(&s))    return "break;";
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) return "continue;";
    return "<unknown stmt>";
}

// Render one class member to a single string. Slice 10 covers FieldDecl and
// MethodDecl; the other Member subclasses land with their slices.
inline std::string memberSummary(const ast::Member& m) {
    if (auto* f = dynamic_cast<const ast::FieldDecl*>(&m)) {
        std::string out = visibilityPrefix(f->visibility);
        for (std::string_view mod : f->modifiers) {
            out += std::string(mod);
            out += ' ';
        }
        out += std::string(f->name);
        out += ": ";
        out += f->type ? typeToString(*f->type) : std::string("<null>");
        if (f->initializer) {
            out += " := ";
            out += exprToString(*f->initializer);
        }
        return out;
    }
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        std::string out = visibilityPrefix(md->visibility);
        for (std::string_view mod : md->modifiers) {
            out += std::string(mod);
            out += ' ';
        }
        out += "method ";
        out += std::string(md->name);
        if (!md->genericParams.empty()) {
            out += '<';
            for (std::size_t i = 0; i < md->genericParams.size(); ++i) {
                if (i > 0) out += ", ";
                if (!md->genericParams[i].variance.empty()) {
                    out += std::string(md->genericParams[i].variance);
                    out += ' ';
                }
                out += std::string(md->genericParams[i].name);
            }
            out += '>';
        }
        out += '(';
        for (std::size_t i = 0; i < md->params.size(); ++i) {
            if (i > 0) out += ", ";
            out += std::string(md->params[i].name);
            out += ": ";
            out += md->params[i].type ? typeToString(*md->params[i].type)
                                      : std::string("<null>");
        }
        out += ')';
        if (md->returnType) {
            out += ": ";
            if (md->returnOwned) out += "owned ";
            out += typeToString(*md->returnType);
        }
        switch (md->bodyKind) {
            case ast::MethodBodyKind::Expression:
                out += " => ";
                out += md->exprBody ? exprToString(*md->exprBody)
                                    : std::string("<null>");
                break;
            case ast::MethodBodyKind::Block: {
                if (md->stmts.empty()) {
                    out += " { }";
                } else {
                    out += " { ";
                    for (std::size_t i = 0; i < md->stmts.size(); ++i) {
                        if (i > 0) out += ' ';
                        out += md->stmts[i] ? stmtSummary(*md->stmts[i])
                                            : std::string("<null>");
                    }
                    out += " }";
                }
                break;
            }
            case ast::MethodBodyKind::Abstract:
                out += " ; ";
                break;
        }
        return out;
    }
    return "<unknown member>";
}

// The class members of the i-th top-level declaration, rendered. Returns
// an empty vector if the index is out of range or the decl isn't a TypeDecl
// with members.
inline std::vector<std::string> memberSummaries(const ParseResult& result,
                                                std::size_t topDeclIndex = 0) {
    std::vector<std::string> out;
    if (!result.sourceFile) return out;
    if (topDeclIndex >= result.sourceFile->topDecls.size()) return out;
    const auto* td = dynamic_cast<const ast::TypeDecl*>(
        result.sourceFile->topDecls[topDeclIndex].get());
    if (!td) return out;
    for (const auto& m : td->members) {
        out.push_back(m ? memberSummary(*m) : std::string("<null>"));
    }
    return out;
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
