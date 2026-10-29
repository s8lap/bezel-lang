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
//
// The AST's identifier/literal fields are `std::string_view`s into the lexed
// source buffer (see ast::Node.hpp), so that buffer must outlive `sourceFile`.
// Callers commonly pass a temporary `std::string` (e.g. concatenated test
// fixtures) to `parseAll`, whose lifetime ends at the end of the call
// expression — leaving every view dangling. To avoid that heap-use-after-free
// (issue #419), `ParseResult` owns the source: `parseAll` copies the caller's
// source into `source` and lexes against that owned copy. The copy lives in a
// `unique_ptr<std::string>` so its heap address is stable across the
// return-by-value (and any later move) of `ParseResult`, keeping every view
// valid for as long as the AST it points into.
struct ParseResult {
    std::unique_ptr<std::string> source;
    std::unique_ptr<ast::SourceFile> sourceFile;
    DiagnosticEngine diagnostics;
};

inline ParseResult parseAll(std::string_view source) {
    ParseResult result;
    result.source = std::make_unique<std::string>(source);
    Lexer lexer(*result.source, result.diagnostics);
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

// Render a Type AST back to its canonical text form.
// Recursive over generic args, inline-array elements, and function-type
// component types.
inline std::string exprToString(const ast::Expr& e); // forward
inline std::string typeToString(const ast::Type& t) {
    std::string out;
    switch (t.kind) {
        case ast::TypeKind::Named: {
            if (t.name) out += qualifiedNameString(*t.name);
            if (!t.genericArgs.empty()) {
                out += '<';
                for (std::size_t i = 0; i < t.genericArgs.size(); ++i) {
                    if (i > 0) out += ", ";
                    out += t.genericArgs[i]
                               ? typeToString(*t.genericArgs[i])
                               : std::string("<null>");
                }
                out += '>';
            }
            break;
        }
        case ast::TypeKind::Unit:
            out += "()";
            break;
        case ast::TypeKind::InlineArray:
            out += '[';
            out += t.arraySize ? exprToString(*t.arraySize)
                               : std::string("<null>");
            if (t.arrayOwned) out += " owned";
            out += ']';
            out += t.arrayElement ? typeToString(*t.arrayElement)
                                  : std::string("<null>");
            break;
        case ast::TypeKind::Function:
            // §27.2: a nullable function type is rendered with outer parens —
            // `((int) -> bool)?` — to distinguish it from `(int) -> bool?`
            // (non-null function returning nullable bool). The extra parens
            // only appear when the whole function type is nullable.
            if (t.nullable) out += '(';
            out += '(';
            for (std::size_t i = 0; i < t.funcParams.size(); ++i) {
                if (i > 0) out += ", ";
                const auto& p = t.funcParams[i];
                if (!p.modifier.empty()) {
                    out += std::string(p.modifier);
                    out += ' ';
                }
                if (!p.name.empty()) {
                    out += std::string(p.name);
                    out += ": ";
                }
                out += p.type ? typeToString(*p.type)
                              : std::string("<null>");
            }
            out += ") -> ";
            if (t.funcReturnOwned) out += "owned ";
            out += t.funcReturn ? typeToString(*t.funcReturn)
                                : std::string("<null>");
            if (t.nullable) out += ')';
            break;
    }
    if (t.nullable) out += '?';
    return out;
}

// Forward declarations — exprToString and stmtSummary are mutually recursive
// (lambdas hold statements; statements hold expressions) and both are also
// called from earlier-defined topDeclSummary / memberSummary. patternToString
// is its own peer (used inside CaseExpr).
inline std::string exprToString(const ast::Expr& e);
inline std::string stmtSummary(const ast::Stmt& s);
inline std::string patternToString(const ast::Pattern& p);

// Render an A.4 SerializedClause. Leading space included so call sites can
// just append unconditionally — they can guard on the null pointer.
inline std::string serializedClauseString(const ast::SerializedClause& sc) {
    std::string out = " serialized";
    if (sc.isSkip) {
        out += " skip";
        return out;
    }
    if (!sc.name.empty()) {
        out += " \"";
        out += std::string(sc.name);
        out += '"';
    }
    if (sc.isOptional) out += " optional";
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
            case ast::TypeDeclKind::Choice:    out += "choice ";    break;
        }
        out += std::string(td->name);
        if (td->hasPositionalFields) {
            out += '(';
            for (std::size_t i = 0; i < td->positionalFields.size(); ++i) {
                if (i > 0) out += ", ";
                out += std::string(td->positionalFields[i].name);
                out += ": ";
                out += td->positionalFields[i].type
                           ? typeToString(*td->positionalFields[i].type)
                           : std::string("<null>");
                if (td->positionalFields[i].serialized) {
                    out += serializedClauseString(
                        *td->positionalFields[i].serialized);
                }
            }
            out += ')';
        }
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
        // §9.8 v0.141: the `serialized tree/graph/cyclic` header clause was
        // retired. At parse time, serializedClass is set only by a class-body
        // `polymorphic(...)` member clause (its polyCases; the wire shape is
        // derived later by sema). Render that registry.
        if (td->serializedClass && !td->serializedClass->polyCases.empty()) {
            out += " polymorphic(";
            for (std::size_t i = 0;
                 i < td->serializedClass->polyCases.size(); ++i) {
                if (i > 0) out += ", ";
                const auto& pc = td->serializedClass->polyCases[i];
                out += '"';
                out += std::string(pc.discriminator);
                out += "\": ";
                out += pc.type ? typeToString(*pc.type)
                               : std::string("<null>");
            }
            out += ')';
        }
        if (!td->baseTypes.empty()) {
            out += ": ";
            for (std::size_t i = 0; i < td->baseTypes.size(); ++i) {
                if (i > 0) out += ", ";
                out += td->baseTypes[i] ? typeToString(*td->baseTypes[i])
                                        : std::string("<null>");
            }
        }
        if (!td->enumConstants.empty()) {
            out += " { ";
            for (std::size_t i = 0; i < td->enumConstants.size(); ++i) {
                if (i > 0) out += ", ";
                out += std::string(td->enumConstants[i].name);
                if (td->enumConstants[i].value) {
                    out += " := ";
                    out += exprToString(*td->enumConstants[i].value);
                }
                if (td->enumConstants[i].serialized) {
                    out += serializedClauseString(
                        *td->enumConstants[i].serialized);
                }
            }
            out += " }";
        }
        // §35 (v0.200, #2044): render choice cases. Payload-free cases print
        // bare; payload cases print as `Name(type name: Type, ...)`. An
        // `owned` payload marker prints as a leading `owned ` modifier.
        if (!td->choiceCases.empty()) {
            out += " { ";
            for (std::size_t i = 0; i < td->choiceCases.size(); ++i) {
                if (i > 0) out += ", ";
                out += std::string(td->choiceCases[i].name);
                if (!td->choiceCases[i].payload.empty()) {
                    out += '(';
                    for (std::size_t j = 0;
                         j < td->choiceCases[i].payload.size(); ++j) {
                        if (j > 0) out += ", ";
                        const auto& pf = td->choiceCases[i].payload[j];
                        if (!pf.modifier.empty()) {
                            out += std::string(pf.modifier);
                            out += ' ';
                        }
                        out += std::string(pf.name);
                        out += ": ";
                        out += pf.type ? typeToString(*pf.type)
                                       : std::string("<null>");
                        if (pf.serialized) {
                            out += serializedClauseString(*pf.serialized);
                        }
                    }
                    out += ')';
                }
                if (td->choiceCases[i].serialized) {
                    out += serializedClauseString(
                        *td->choiceCases[i].serialized);
                }
            }
            out += " }";
        }
        return out;
    }
    if (auto* ta = dynamic_cast<const ast::TypeAlias*>(&decl)) {
        std::string out = "type ";
        out += std::string(ta->name);
        out += " := ";  // v0.160: type aliases bind with := (§27.8)
        out += ta->aliasedType ? typeToString(*ta->aliasedType)
                               : std::string("<null>");
        return out;
    }
    if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&decl)) {
        std::string out = visibilityPrefix(ext->visibility);
        out += "extension ";
        out += ext->targetType ? typeToString(*ext->targetType)
                               : std::string("<null>");
        return out;
    }
    if (auto* x = dynamic_cast<const ast::ExternBlock*>(&decl)) {
        std::string out = "extern ";
        out += std::string(x->library);
        if (!x->convention.empty()) {
            out += " convention ";
            out += std::string(x->convention);
        }
        out += " { ";
        for (std::size_t i = 0; i < x->methods.size(); ++i) {
            if (i > 0) out += ' ';
            out += "method ";
            out += std::string(x->methods[i].name);
            out += "()";
            if (x->methods[i].returnType) {
                out += ": ";
                out += typeToString(*x->methods[i].returnType);
            }
            out += ';';
        }
        out += " }";
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
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e))
        return std::string(is->text);
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
    auto renderArg = [&](const ast::CallArgument& a) -> std::string {
        std::string s;
        if (!a.modifier.empty()) {
            s += std::string(a.modifier);
            s += ' ';
        }
        if (!a.name.empty()) {
            s += std::string(a.name);
            s += " := ";
        }
        if (a.isDiscard) s += "_";
        else s += a.value ? exprToString(*a.value) : std::string("<null>");
        return s;
    };
    if (auto* c = dynamic_cast<const ast::CallExpr*>(&e)) {
        std::string out = c->callee ? exprToString(*c->callee)
                                    : std::string("<null>");
        out += '(';
        for (std::size_t i = 0; i < c->args.size(); ++i) {
            if (i > 0) out += ", ";
            out += renderArg(c->args[i]);
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
    if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        // §20 / A.7 IfExpr (v0.208): rendered condition-first as the atom;
        // an elif chain prints structure-true as the right-nested form.
        std::string out = "(if ";
        out += c->condition ? exprToString(*c->condition)
                            : std::string("<null>");
        out += " then ";
        out += c->thenValue ? exprToString(*c->thenValue)
                            : std::string("<null>");
        out += " else ";
        out += c->elseValue ? exprToString(*c->elseValue)
                            : std::string("<null>");
        out += ')';
        return out;
    }
    if (auto* t = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (!t->operand) return std::string("(throw)");
        return "(throw " + exprToString(*t->operand) + ")";
    }
    if (auto* mt = dynamic_cast<const ast::MarkedTransferExpr*>(&e)) {
        // A.7 MarkedTransfer (v0.209, #2080): rendered as written.
        return "(owned " + std::string(mt->name) + ")";
    }
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(&e)) {
        // §17.15 / A.7 BindExpr (v0.216, #2098): rendered as written —
        // `(var x: T := owned E; body)`.
        std::string out = "(";
        for (const auto& cl : bx->clauses) {
            out += "var ";
            out += cl ? std::string(cl->name) : std::string("<null>");
            if (cl && cl->type) {
                out += ": ";
                out += typeToString(*cl->type);
            }
            out += " := ";
            if (cl && cl->initializerHasOwnedMarker) out += "owned ";
            out += (cl && cl->initializer) ? exprToString(*cl->initializer)
                                           : std::string("<null>");
            out += "; ";
        }
        out += bx->body ? exprToString(*bx->body) : std::string("<null>");
        out += ')';
        return out;
    }
    if (auto* tof = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        return "typeof(" + (tof->queriedType
                                ? typeToString(*tof->queriedType)
                                : std::string("<null>")) + ")";
    }
    if (auto* nof = dynamic_cast<const ast::NameOfExpr*>(&e)) {
        return "nameof(" + (nof->name ? qualifiedNameString(*nof->name)
                                      : std::string("<null>")) + ")";
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // §27.3 / A.7 (v0.217): keyword-led form, rendered as written —
        // `lambda(params): T => body`, `async lambda(...) => ...`.
        std::string out = "(";
        if (lam->isAsync) out += "async ";
        out += "lambda(";
        for (std::size_t i = 0; i < lam->params.size(); ++i) {
            if (i > 0) out += ", ";
            const auto& p = lam->params[i];
            // §27.4 (v0.183, #1408): LambdaDestructure — render like the
            // §19.3 case-pattern DestructuringPattern (patternToString),
            // since this IS one (Param::destructure), just reused in
            // parameter position.
            if (p.destructure) {
                out += patternToString(*p.destructure);
                continue;
            }
            if (!p.modifier.empty()) {
                out += std::string(p.modifier);
                out += ' ';
            }
            out += std::string(p.name);
            if (p.type) {
                out += ": ";
                out += typeToString(*p.type);
            }
        }
        out += ')';
        // §27.3 (v0.217): the optional return-type ascription.
        if (lam->retType) {
            out += ": ";
            out += typeToString(*lam->retType);
        }
        out += " => ";
        if (lam->isBlockBody) {
            out += "do { ";
            for (std::size_t i = 0; i < lam->stmts.size(); ++i) {
                if (i > 0) out += ' ';
                out += lam->stmts[i] ? stmtSummary(*lam->stmts[i])
                                     : std::string("<null>");
            }
            out += " } end";
        } else {
            out += lam->exprBody ? exprToString(*lam->exprBody)
                                 : std::string("<null>");
        }
        out += ')';
        return out;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        std::string out = "(";
        out += tt->operand ? exprToString(*tt->operand)
                           : std::string("<null>");
        out += ' ';
        out += std::string(tt->op);
        out += ' ';
        out += tt->testType ? typeToString(*tt->testType)
                            : std::string("<null>");
        out += ')';
        return out;
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        // Keyword-spelled unary operators need a space; punctuator-spelled
        // ones don't.
        const bool needsSpace =
            (u->op == "not" || u->op == "await" || u->op == "move");
        std::string out = "(";
        out += std::string(u->op);
        if (needsSpace) out += ' ';
        out += u->operand ? exprToString(*u->operand) : std::string("<null>");
        if (u->replacement) {
            // A.7 MoveExpr's replacing form (§28.8.1): `(move loc with e)`.
            out += " with ";
            out += exprToString(*u->replacement);
        }
        out += ')';
        return out;
    }
    if (auto* a = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        return (a->target ? exprToString(*a->target) : std::string("<null>")) +
               "?." + std::string(a->member);
    }
    if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        std::string out = c->callee ? exprToString(*c->callee)
                                    : std::string("<null>");
        out += "?(";
        for (std::size_t i = 0; i < c->args.size(); ++i) {
            if (i > 0) out += ", ";
            out += renderArg(c->args[i]);
        }
        out += ')';
        return out;
    }
    if (auto* idx = dynamic_cast<const ast::IndexExpr*>(&e)) {
        std::string out = idx->target ? exprToString(*idx->target)
                                      : std::string("<null>");
        out += '[';
        for (std::size_t i = 0; i < idx->indices.size(); ++i) {
            if (i > 0) out += ", ";
            out += renderArg(idx->indices[i]);
        }
        out += ']';
        return out;
    }
    if (auto* b = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        return (b->operand ? exprToString(*b->operand)
                           : std::string("<null>")) + "!";
    }
    if (auto* n = dynamic_cast<const ast::NewExpr*>(&e)) {
        std::string out = "new ";
        if (n->type) out += typeToString(*n->type);
        out += '(';
        for (std::size_t i = 0; i < n->args.size(); ++i) {
            if (i > 0) out += ", ";
            out += renderArg(n->args[i]);
        }
        out += ')';
        return out;
    }
    if (dynamic_cast<const ast::UnitExpr*>(&e)) return "()";
    if (auto* r = dynamic_cast<const ast::RangeExpr*>(&e)) {
        // Render shape `(LO .. HI step S)` / `(LO ..< HI step S)`. Missing
        // operands render as `<>` to keep the form visibly distinguished
        // from the with-bounds version. Wrapped in parens for the same
        // unambiguous-precedence reason as BinaryExpr.
        std::string out = "(";
        out += r->lo ? exprToString(*r->lo) : std::string("<>");
        out += r->inclusive ? " .. " : " ..< ";
        out += r->hi ? exprToString(*r->hi) : std::string("<>");
        if (r->step) {
            out += " step ";
            out += exprToString(*r->step);
        }
        out += ')';
        return out;
    }
    if (auto* w = dynamic_cast<const ast::WithExpr*>(&e)) {
        // §9.5 (v0.208): the keyword-attached-parentheses spelling.
        std::string out = w->source ? exprToString(*w->source)
                                    : std::string("<null>");
        out += " with(";
        for (std::size_t i = 0; i < w->assigns.size(); ++i) {
            if (i > 0) out += ", ";
            out += std::string(w->assigns[i].name);
            out += " := ";
            out += w->assigns[i].value
                       ? exprToString(*w->assigns[i].value)
                       : std::string("<null>");
        }
        out += ')';
        return out;
    }
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        std::string out = "default(";
        out += de->type ? typeToString(*de->type) : std::string("<null>");
        out += ')';
        return out;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        std::string out = "[";
        if (cl->isDict) {
            for (std::size_t i = 0; i < cl->elements.size(); ++i) {
                if (i > 0) out += ", ";
                out += i < cl->keys.size() && cl->keys[i]
                           ? exprToString(*cl->keys[i])
                           : std::string("<null>");
                out += ": ";
                out += cl->elements[i] ? exprToString(*cl->elements[i])
                                       : std::string("<null>");
            }
        } else {
            for (std::size_t i = 0; i < cl->elements.size(); ++i) {
                if (i > 0) out += ", ";
                out += cl->elements[i] ? exprToString(*cl->elements[i])
                                       : std::string("<null>");
            }
        }
        out += ']';
        return out;
    }
    if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        std::string out = "case ";
        out += ce->scrutinee ? exprToString(*ce->scrutinee)
                             : std::string("<bare>");
        out += " of {";
        for (std::size_t i = 0; i < ce->arms.size(); ++i) {
            if (i > 0) out += " | ";
            else       out += ' ';
            const auto& arm = ce->arms[i];
            out += arm.pattern ? patternToString(*arm.pattern)
                               : std::string("<null>");
            if (arm.guard) {
                out += " when ";
                out += exprToString(*arm.guard);
            }
            out += " => ";
            out += arm.exprBody ? exprToString(*arm.exprBody)
                                : std::string("<null>");
        }
        out += " } end";
        return out;
    }
    return "<unknown expr>";
}

inline std::string patternToString(const ast::Pattern& p) {
    if (dynamic_cast<const ast::DiscardPattern*>(&p)) return "_";
    if (dynamic_cast<const ast::NullPattern*>(&p))    return "null";
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(&p)) {
        std::string out = "var ";
        out += std::string(bp->name);
        if (bp->type) {
            out += ": ";
            out += typeToString(*bp->type);
        }
        return out;
    }
    if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(&p)) {
        std::string out;
        for (std::size_t i = 0; i < cp->constants.size(); ++i) {
            if (i > 0) out += ", ";
            out += cp->constants[i] ? exprToString(*cp->constants[i])
                                    : std::string("<null>");
        }
        return out;
    }
    if (auto* np = dynamic_cast<const ast::NamedPattern*>(&p)) {
        return np->name ? qualifiedNameString(*np->name)
                        : std::string("<null>");
    }
    if (auto* rp = dynamic_cast<const ast::RangePattern*>(&p)) {
        return rp->rangeExpr ? exprToString(*rp->rangeExpr)
                             : std::string("<null>");
    }
    if (auto* rop = dynamic_cast<const ast::RelOpPattern*>(&p)) {
        std::string out = std::string(rop->op);
        out += ' ';
        out += rop->expr ? exprToString(*rop->expr) : std::string("<null>");
        return out;
    }
    if (auto* tp = dynamic_cast<const ast::TypePattern*>(&p)) {
        return tp->type ? typeToString(*tp->type) : std::string("<null>");
    }
    if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(&p)) {
        std::string out = dp->typeName
                              ? qualifiedNameString(*dp->typeName)
                              : std::string("<null>");
        out += '(';
        for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
            if (i > 0) out += ", ";
            const auto& sp = dp->subPatterns[i];
            if (!sp.nakedBinding.empty()) {
                out += std::string(sp.nakedBinding);
            } else {
                out += sp.pattern ? patternToString(*sp.pattern)
                                  : std::string("<null>");
            }
        }
        out += ')';
        return out;
    }
    return "<unknown pattern>";
}

// Render one statement to a single source-like string. Slice 11 covers
// LocalDecl, ReturnStmt, AssignmentStmt, ExpressionStmt.
inline std::string stmtSummary(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        std::string out;
        if (ld->isAwaitUsing) out = "await using var ";
        else if (ld->isUsing) out = "using var ";
        else                  out = "var ";
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
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        std::string out = "const ";
        out += std::string(lc->name);
        if (lc->type) {
            out += ": ";
            out += typeToString(*lc->type);
        }
        out += " := ";
        out += lc->value ? exprToString(*lc->value)
                         : std::string("<null>");
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
        out += " do ";
        out += bodyToString(w->body);
        out += " end;";
        return out;
    }
    if (auto* f = dynamic_cast<const ast::ForStmt*>(&s)) {
        std::string out = f->isAwait ? "await for var " : "for var ";
        out += std::string(f->varName);
        if (f->varType) {
            out += ": ";
            out += typeToString(*f->varType);
        }
        out += " in ";
        out += f->iterable ? exprToString(*f->iterable)
                           : std::string("<null>");
        if (f->withExpr) {
            out += " with ";
            out += exprToString(*f->withExpr);
        }
        out += " do ";
        out += bodyToString(f->body);
        out += " end;";
        return out;
    }
    if (auto* d = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        std::string out = "do ";
        out += bodyToString(d->body);
        out += " end;";
        return out;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        std::string out = "case ";
        out += cs->scrutinee ? exprToString(*cs->scrutinee)
                             : std::string("<bare>");
        out += " of {";
        for (std::size_t i = 0; i < cs->arms.size(); ++i) {
            if (i > 0) out += " | ";
            else       out += ' ';
            const auto& arm = cs->arms[i];
            out += arm.pattern ? patternToString(*arm.pattern)
                               : std::string("<null>");
            if (arm.guard) {
                out += " when ";
                out += exprToString(*arm.guard);
            }
            out += " => ";
            // Stmt-position arms are always Block kind.
            out += "{ ";
            for (std::size_t j = 0; j < arm.stmts.size(); ++j) {
                if (j > 0) out += ' ';
                out += arm.stmts[j] ? stmtSummary(*arm.stmts[j])
                                    : std::string("<null>");
            }
            out += " }";
        }
        out += " } end;";
        return out;
    }
    if (auto* t = dynamic_cast<const ast::TryStmt*>(&s)) {
        std::string out = "try ";
        out += bodyToString(t->body);
        for (const auto& arm : t->catches) {
            out += " catch ";
            if (arm.hasBinding) {
                out += "var ";
                out += std::string(arm.varName);
                out += ": ";
            }
            for (std::size_t i = 0; i < arm.types.size(); ++i) {
                // §A.6 CatchPattern (v0.167, #1371): multi-type alternatives are
                // `|`-separated, matching the case-pattern alternatives spelling
                // and the source form the parser consumes.
                if (i > 0) out += " | ";
                out += arm.types[i] ? typeToString(*arm.types[i])
                                    : std::string("<null>");
            }
            if (arm.guard) {
                out += " when ";
                out += exprToString(*arm.guard);
            }
            out += " => ";
            out += arm.body ? stmtSummary(*arm.body)
                            : std::string("<null>");
        }
        if (t->hasFinally) {
            out += " finally ";
            out += bodyToString(t->finallyBody);
        }
        out += " end;";
        return out;
    }
    if (dynamic_cast<const ast::BreakStmt*>(&s))    return "break;";
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) return "continue;";
    if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        return (inc->target ? exprToString(*inc->target)
                            : std::string("<null>")) +
               std::string(inc->op) + ";";
    }
    if (auto* d = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        return "_ := " + (d->value ? exprToString(*d->value)
                                   : std::string("<null>")) + ";";
    }
    if (auto* y = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (y->isBreak) return "yield break;";
        return "yield return " +
               (y->value ? exprToString(*y->value) : std::string("<null>")) +
               ";";
    }
    if (auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        std::string out = "throw";
        if (th->value) {
            out += ' ';
            out += exprToString(*th->value);
        }
        out += ';';
        return out;
    }
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
    if (auto* c = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        std::string out;
        if (c->isStatic) out += "static ";
        else             out += visibilityPrefix(c->visibility);
        out += "constructor(";
        for (std::size_t i = 0; i < c->params.size(); ++i) {
            if (i > 0) out += ", ";
            out += std::string(c->params[i].name);
            out += ": ";
            out += c->params[i].type ? typeToString(*c->params[i].type)
                                     : std::string("<null>");
        }
        out += ')';
        out += " { ";
        for (std::size_t i = 0; i < c->body.size(); ++i) {
            if (i > 0) out += ' ';
            out += c->body[i] ? stmtSummary(*c->body[i])
                              : std::string("<null>");
        }
        out += " }";
        return out;
    }
    if (auto* d = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        std::string out = "destructor { ";
        for (std::size_t i = 0; i < d->body.size(); ++i) {
            if (i > 0) out += ' ';
            out += d->body[i] ? stmtSummary(*d->body[i])
                              : std::string("<null>");
        }
        out += " }";
        return out;
    }
    if (auto* ed = dynamic_cast<const ast::EventDecl*>(&m)) {
        std::string out = visibilityPrefix(ed->visibility);
        if (ed->isStatic) out += "static ";
        if (ed->isAsync)  out += "async ";
        out += "event ";
        out += std::string(ed->name);
        if (ed->type) {
            out += ": ";
            out += typeToString(*ed->type);
        }
        return out;
    }
    if (auto* cd = dynamic_cast<const ast::ConstDecl*>(&m)) {
        std::string out = visibilityPrefix(cd->visibility);
        if (cd->isStatic) out += "static ";
        out += "const ";
        out += std::string(cd->name);
        if (cd->type) {
            out += ": ";
            out += typeToString(*cd->type);
        }
        out += " := ";
        out += cd->value ? exprToString(*cd->value)
                         : std::string("<null>");
        return out;
    }
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(&m)) {
        std::string out = visibilityPrefix(pd->visibility);
        for (std::string_view mod : pd->modifiers) {
            out += std::string(mod);
            out += ' ';
        }
        out += "property ";
        if (pd->isIndexer) {
            out += '[';
            for (std::size_t i = 0; i < pd->indexerParams.size(); ++i) {
                if (i > 0) out += ", ";
                out += std::string(pd->indexerParams[i].name);
                out += ": ";
                out += pd->indexerParams[i].type
                           ? typeToString(*pd->indexerParams[i].type)
                           : std::string("<null>");
            }
            out += ']';
        } else {
            out += std::string(pd->name);
        }
        out += ": ";
        out += pd->type ? typeToString(*pd->type) : std::string("<null>");
        switch (pd->bodyKind) {
            case ast::PropertyBodyKind::Expression:
                out += " => ";
                out += pd->exprBody ? exprToString(*pd->exprBody)
                                    : std::string("<null>");
                break;
            case ast::PropertyBodyKind::Auto:
                if (pd->hasAutoAccessorVis) {
                    out += ' ';
                    out += visibilityPrefix(pd->autoAccessorVis);
                    // visibilityPrefix() already adds a trailing space.
                    out += pd->autoAccessorIsSet ? "set" : "get";
                }
                if (pd->autoInitializer) {
                    out += " := ";
                    out += exprToString(*pd->autoInitializer);
                }
                if (pd->serialized) {
                    out += serializedClauseString(*pd->serialized);
                }
                break;
            case ast::PropertyBodyKind::Block: {
                // Accessor-block body: render each accessor compactly so
                // tests can assert on the shape.
                out += " { ";
                for (std::size_t i = 0; i < pd->accessors.size(); ++i) {
                    if (i > 0) out += ' ';
                    const auto& a = pd->accessors[i];
                    out += visibilityPrefix(a.visibility);
                    out += (a.isSet ? "set" : "get");
                    if (a.isSet && a.hasSetterParamSpec) {
                        out += '(';
                        if (!a.setterParamMod.empty()) {
                            out += std::string(a.setterParamMod);
                            if (!a.setterParamName.empty()) out += ' ';
                        }
                        if (!a.setterParamName.empty()) {
                            out += std::string(a.setterParamName);
                        }
                        out += ')';
                    }
                    switch (a.bodyKind) {
                        case ast::PropertyAccessorBodyKind::Abstract:
                            out += ';';
                            break;
                        case ast::PropertyAccessorBodyKind::Expression:
                            out += " => ";
                            out += a.exprBody
                                       ? exprToString(*a.exprBody)
                                       : std::string("<null>");
                            out += ';';
                            break;
                        case ast::PropertyAccessorBodyKind::Block:
                            out += " => do { ";
                            for (std::size_t s = 0; s < a.stmts.size(); ++s) {
                                if (s > 0) out += ' ';
                                out += a.stmts[s] ? stmtSummary(*a.stmts[s])
                                                  : std::string("<null>");
                            }
                            out += " } end;";
                            break;
                    }
                }
                out += " }";
                break;
            }
        }
        return out;
    }
    if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        std::string out = "nested ";
        out += nt->decl ? topDeclSummary(*nt->decl) : std::string("<null>");
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
