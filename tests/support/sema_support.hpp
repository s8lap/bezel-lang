#pragma once

// Test support for the sema milestone. Drives the lex+parse+sema pipeline
// on a source string and exposes the resulting SemaContext for inspection.

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::test {

struct SemaResult {
    std::unique_ptr<ast::SourceFile> sourceFile;
    DiagnosticEngine diagnostics;
    std::unique_ptr<sema::SemaContext> ctx;
};

// Lex + parse + run symbol-collection. Returns the assembled SemaResult.
// Callers inspect ctx->module() and ctx->symbols() to assert on the
// produced symbol table.
inline SemaResult parseAndCollect(std::string_view source) {
    SemaResult r;
    Lexer lexer(source, r.diagnostics);
    Parser parser(lexer, r.diagnostics);
    r.sourceFile = parser.parseCompilationUnit();
    r.ctx = std::make_unique<sema::SemaContext>(r.diagnostics);
    if (r.sourceFile) {
        sema::collectSymbols(*r.sourceFile, *r.ctx);
    }
    return r;
}

// fix #3985: `parseAndCollect` followed by an UNGATED `resolveTypes`/
// `typecheckExpressions` call was a 137-file, hand-duplicated pattern across
// tests/sema/*.cpp (each file spelling its own 3-line "parseSemaAll"-shaped
// helper), diverging from the real driver's own gate (src/driver/Driver.cpp
// ~473, ~672: `if (unit && !diagnostics.hasErrors()) { ... }`). A fixture
// that failed to PARSE still had sema run over the parser's error-recovery-
// truncated AST, so a genuinely grammar-invalid row could "pass" by
// accident instead of exercising the sema rule its name and comment claim
// (first found and fixed for one file by fix #3958; this pair centralizes
// that gate so the class cannot recur file-by-file). Two shapes cover the
// helpers found in the wild: resolve-only (classification/type-resolution
// tests that never reach expression typechecking) and resolve+typecheck
// (the common case). Both gate on `!r.diagnostics.hasErrors()`, exactly as
// the driver does.
inline SemaResult parseAndResolveGated(std::string_view source) {
    auto r = parseAndCollect(source);
    if (r.sourceFile && r.ctx && !r.diagnostics.hasErrors()) {
        sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

inline SemaResult parseAndTypecheckGated(std::string_view source) {
    auto r = parseAndCollect(source);
    if (r.sourceFile && r.ctx && !r.diagnostics.hasErrors()) {
        sema::resolveTypes(*r.sourceFile, *r.ctx);
        sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// A third recurring shape (resolve + typecheck + the §22.15 pure-body
// checker), used by the §22.15/§22.16 purity test files.
inline SemaResult parseAndCheckPureGated(std::string_view source) {
    auto r = parseAndCollect(source);
    if (r.sourceFile && r.ctx && !r.diagnostics.hasErrors()) {
        sema::resolveTypes(*r.sourceFile, *r.ctx);
        sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Render a symbol to a one-line shape so tests can assert on the
// collected table without dredging the dynamic_cast chain themselves.
// Format examples:
//   "module Foo"
//   "class Foo"
//   "method Foo.M"
//   "field Foo.x"
//   "param Foo.M.x"
// Visibility is included when explicit ("public class Foo").
inline std::string symbolSummary(const sema::Symbol& s);

namespace detail {

inline std::string vis(ast::Visibility v) {
    switch (v) {
        case ast::Visibility::Public:    return "public ";
        case ast::Visibility::Private:   return "private ";
        case ast::Visibility::Internal:  return "internal ";
        case ast::Visibility::Protected: return "protected ";
        case ast::Visibility::None:      return "";
    }
    return "";
}

inline const char* typeKindWord(ast::TypeDeclKind k) {
    switch (k) {
        case ast::TypeDeclKind::Class:     return "class";
        case ast::TypeDeclKind::Interface: return "interface";
        case ast::TypeDeclKind::Record:    return "record";
        case ast::TypeDeclKind::Enum:      return "enum";
        case ast::TypeDeclKind::Choice:    return "choice";  // §35 (v0.200, #2044)
    }
    return "?";
}

inline std::string parentPrefix(const sema::MemberSymbol& m) {
    if (m.parent && !m.parent->name.empty()) {
        return std::string(m.parent->name) + ".";
    }
    return "";
}

} // namespace detail

inline std::string symbolSummary(const sema::Symbol& s) {
    using namespace detail;
    if (auto* m = dynamic_cast<const sema::ModuleSymbol*>(&s)) {
        return "module " + std::string(m->name);
    }
    if (auto* t = dynamic_cast<const sema::TypeSymbol*>(&s)) {
        return vis(t->visibility) + typeKindWord(t->kind) + " " +
               std::string(t->name);
    }
    if (auto* a = dynamic_cast<const sema::TypeAliasSymbol*>(&s)) {
        return "type " + std::string(a->name);
    }
    if (auto* nt = dynamic_cast<const sema::NestedTypeSymbol*>(&s)) {
        std::string out = "nested ";
        if (nt->type) {
            out += vis(nt->type->visibility);
            out += typeKindWord(nt->type->kind);
            out += " ";
            out += std::string(nt->name);
        } else {
            out += std::string(nt->name);
        }
        return out;
    }
    if (auto* md = dynamic_cast<const sema::MethodSymbol*>(&s)) {
        std::string out = vis(md->visibility);
        out += md->isOperator ? "operator " : "method ";
        out += parentPrefix(*md);
        out += std::string(md->name);
        return out;
    }
    if (auto* fd = dynamic_cast<const sema::FieldSymbol*>(&s)) {
        return vis(fd->visibility) + "field " + parentPrefix(*fd) +
               std::string(fd->name);
    }
    if (auto* pd = dynamic_cast<const sema::PropertySymbol*>(&s)) {
        return vis(pd->visibility) + (pd->isIndexer ? "indexer " : "property ") +
               parentPrefix(*pd) + std::string(pd->name);
    }
    if (auto* ed = dynamic_cast<const sema::EventSymbol*>(&s)) {
        return vis(ed->visibility) + "event " + parentPrefix(*ed) +
               std::string(ed->name);
    }
    if (auto* cd = dynamic_cast<const sema::ConstMemberSymbol*>(&s)) {
        return vis(cd->visibility) + (cd->isStatic ? "static " : "") +
               "const " + parentPrefix(*cd) + std::string(cd->name);
    }
    if (auto* cs = dynamic_cast<const sema::ConstructorSymbol*>(&s)) {
        return vis(cs->visibility) + (cs->isStatic ? "static " : "") +
               "constructor " + parentPrefix(*cs);
    }
    if (auto* dd = dynamic_cast<const sema::DestructorSymbol*>(&s)) {
        return "destructor " + parentPrefix(*dd);
    }
    if (auto* ec = dynamic_cast<const sema::EnumConstantSymbol*>(&s)) {
        return "enum-constant " + parentPrefix(*ec) + std::string(ec->name);
    }
    // §35 (v0.200, #2044): choice cases render with their arity so a test
    // can distinguish payload-free from payload-carrying cases.
    if (auto* cc = dynamic_cast<const sema::ChoiceCaseSymbol*>(&s)) {
        std::string out = "choice-case " + parentPrefix(*cc) +
                          std::string(cc->name);
        if (cc->caseDecl) {
            out += " (";
            out += std::to_string(cc->caseDecl->payload.size());
            out += "-payload)";
        }
        return out;
    }
    if (auto* g = dynamic_cast<const sema::GenericParamSymbol*>(&s)) {
        return "generic-param " + std::string(g->name);
    }
    if (auto* p = dynamic_cast<const sema::ParameterSymbol*>(&s)) {
        return "param " + std::string(p->name);
    }
    if (auto* l = dynamic_cast<const sema::LocalSymbol*>(&s)) {
        return "local " + std::string(l->name);
    }
    if (dynamic_cast<const sema::ExtensionSymbol*>(&s)) {
        return "extension";
    }
    if (auto* xb = dynamic_cast<const sema::ExternBlockSymbol*>(&s)) {
        return "extern-block " + std::string(xb->library);
    }
    if (auto* x = dynamic_cast<const sema::ExternDeclSymbol*>(&s)) {
        return "extern-decl " + std::string(x->name);
    }
    return "<unknown symbol>";
}

} // namespace bezel::test
