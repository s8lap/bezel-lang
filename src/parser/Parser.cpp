#include "parser/Parser.hpp"

#include <string_view>

namespace bezel {

namespace {

// §3.4: the lowercase primitive-type aliases. These are unconditionally
// reserved keywords (Appendix B.1) but they ARE valid type-name first
// components — `int`, `string`, etc. The PascalCase forms (`Object`, `String`,
// `Array`) are ordinary identifiers and need no special handling here.
bool isPrimitiveTypeKeyword(std::string_view text) {
    return text == "bool"   || text == "byte"  || text == "sbyte" ||
           text == "short"  || text == "ushort" || text == "int"  ||
           text == "uint"   || text == "long"  || text == "ulong" ||
           text == "float"  || text == "double" || text == "decimal" ||
           text == "char"   || text == "string";
}

} // namespace

Parser::Parser(Lexer& lexer, DiagnosticEngine& diagnostics)
    : lexer_(lexer), diagnostics_(diagnostics) {
    advance(); // prime current_ with the first token
}

void Parser::advance() {
    current_ = lexer_.next();
}

bool Parser::isKeyword(std::string_view text) const {
    return current_.kind == TokenKind::Keyword && current_.text == text;
}

bool Parser::isPunctuator(std::string_view text) const {
    return current_.kind == TokenKind::Punctuator && current_.text == text;
}

bool Parser::consumeKeyword(std::string_view text) {
    if (!isKeyword(text)) return false;
    advance();
    return true;
}

bool Parser::consumePunctuator(std::string_view text) {
    if (!isPunctuator(text)) return false;
    advance();
    return true;
}

std::unique_ptr<ast::SourceFile> Parser::parseCompilationUnit() {
    auto sourceFile = std::make_unique<ast::SourceFile>();

    // §2.1: "Every source file declares its module on its first non-comment,
    // non-import line." The lexer has already skipped comments, so the very
    // first token must be the `module` keyword.
    if (isKeyword("module")) {
        sourceFile->module = parseModuleDecl();
    } else {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'module' declaration");
    }

    // §2.2: zero or more import declarations follow the module declaration.
    while (isKeyword("import")) {
        sourceFile->imports.push_back(parseImport());
    }

    // §2.4: top-level declarations. Slice 3 covers empty class/interface/
    // record/enum heads only; extension declarations (§26), type aliases,
    // and extern blocks land in later slices.
    while (current_.kind != TokenKind::EndOfFile) {
        auto decl = parseTopDecl();
        if (decl) {
            sourceFile->topDecls.push_back(std::move(decl));
        } else if (current_.kind != TokenKind::EndOfFile) {
            // parseTopDecl already reported; consume one token to make
            // progress and try again. Better recovery comes when we have a
            // notion of "synchronization tokens" (semicolons / `end`).
            advance();
        }
    }

    return sourceFile;
}

std::unique_ptr<ast::Import> Parser::parseImport() {
    auto decl = std::make_unique<ast::Import>();
    // Caller has verified `current_` is the `import` keyword.
    advance(); // past `import`
    decl->name = parseQualifiedName();
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after import declaration");
    }
    return decl;
}

std::unique_ptr<ast::ModuleDecl> Parser::parseModuleDecl() {
    auto decl = std::make_unique<ast::ModuleDecl>();
    // Caller has verified `current_` is the `module` keyword.
    advance(); // past `module`
    decl->name = parseQualifiedName();
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after module declaration");
    }
    return decl;
}

std::unique_ptr<ast::TopDecl> Parser::parseTopDecl() {
    // Grammar A.2 TopDecl alternatives. Type aliases have no Visibility prefix
    // per A.2 `TypeAliasDecl ::= "type" Identifier ...`, so we check for them
    // first; the class-like decls below all admit Visibility? Modifier*.
    if (isKeyword("type")) {
        auto alias = parseTypeAlias();
        return std::unique_ptr<ast::TopDecl>(alias.release());
    }

    // §13 visibility keyword (optional, must precede modifiers).
    ast::Visibility visibility = ast::Visibility::None;
    if (isKeyword("public")) {
        visibility = ast::Visibility::Public;
        advance();
    } else if (isKeyword("private")) {
        visibility = ast::Visibility::Private;
        advance();
    } else if (isKeyword("internal")) {
        visibility = ast::Visibility::Internal;
        advance();
    } else if (isKeyword("protected")) {
        visibility = ast::Visibility::Protected;
        advance();
    }

    // Grammar `Modifier* = ("abstract" | "sealed" | "static")*`. The set of
    // semantically valid combinations is constrained by §11.7 (sema's
    // problem); the parser admits any combination per the grammar.
    std::vector<std::string_view> modifiers;
    while (isKeyword("abstract") || isKeyword("sealed") || isKeyword("static")) {
        modifiers.push_back(current_.text);
        advance();
    }

    // unique_ptr<TypeDecl> -> unique_ptr<TopDecl>: the implicit converting
    // constructor is blocked here (Node deletes its move constructor, which
    // taints the derived classes). Reseat the raw pointer explicitly.
    auto typeDecl = parseTypeOrInterfaceDecl(visibility, std::move(modifiers));
    return std::unique_ptr<ast::TopDecl>(typeDecl.release());
}

std::unique_ptr<ast::TypeDecl> Parser::parseTypeOrInterfaceDecl(
    ast::Visibility visibility, std::vector<std::string_view> modifiers) {
    // Grammar A.3 (ClassDecl/InterfaceDecl/RecordDecl/EnumDecl):
    //   Visibility? Modifier* keyword Identifier ... ClassBody
    // Slice 3 limits the body to the empty form `end Identifier? ;`.

    // The keyword that selects which kind of top-level declaration this is.
    ast::TypeDeclKind kind;
    if (isKeyword("class"))          kind = ast::TypeDeclKind::Class;
    else if (isKeyword("interface")) kind = ast::TypeDeclKind::Interface;
    else if (isKeyword("record"))    kind = ast::TypeDeclKind::Record;
    else if (isKeyword("enum"))      kind = ast::TypeDeclKind::Enum;
    else {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected top-level declaration (class, interface, record, enum, type)");
        return nullptr;
    }
    advance(); // past the kind keyword

    auto decl = std::make_unique<ast::TypeDecl>();
    decl->kind = kind;
    decl->visibility = visibility;
    decl->modifiers = std::move(modifiers);

    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected type name");
    } else {
        decl->name = current_.text;
        advance();
    }

    // §A.3: GenericParams? and Inheritance? appear between the name and the
    // body for classes, interfaces, and records. Enums use EnumInheritance
    // (different grammar — `: IntegerType ...`) and do not admit generic
    // parameters; the enum-specific shape is a later slice.
    if (decl->kind != ast::TypeDeclKind::Enum) {
        decl->genericParams = parseGenericParams();
        decl->baseTypes = parseInheritance();
    }

    // §A.4 Members: slice 6 covers class fields without initializers. Other
    // type kinds and other member forms come in later slices, so their
    // bodies stay empty here.
    if (decl->kind == ast::TypeDeclKind::Class) {
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto member = parseMember();
            if (member) {
                decl->members.push_back(std::move(member));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance(); // crude recovery; sync on `end` or `;` later.
            }
        }
    }

    // §1.6 named-construct termination: `end Identifier? ;`. The optional
    // identifier must match the declared name exactly when supplied.
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end'");
        return decl;
    }
    if (current_.kind == TokenKind::Identifier) {
        decl->endLabel = current_.text;
        if (!decl->name.empty() && decl->endLabel != decl->name) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "end label does not match the declared type name");
        }
        advance();
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end'");
    }

    return decl;
}

std::unique_ptr<ast::TypeAlias> Parser::parseTypeAlias() {
    // A.2: `TypeAliasDecl ::= "type" Identifier GenericParams? "=" Type
    //                          GenericConstraints* ";"`.
    // Slice 4: degenerate form — no generic params, no constraints.
    auto alias = std::make_unique<ast::TypeAlias>();
    advance(); // past `type`

    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected type alias name after 'type'");
    } else {
        alias->name = current_.text;
        advance();
    }

    if (!consumePunctuator("=")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '=' in type alias");
    }

    alias->aliasedType = parseType();

    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after type alias");
    }
    return alias;
}

std::unique_ptr<ast::Type> Parser::parseType() {
    // A.5 (slice-4 subset): `Type ::= QualifiedName GenericArgs? "?"?`. The
    // qualified-name root may be a primitive-type keyword (§3.4), so we use
    // parseTypeName rather than parseQualifiedName.
    auto type = std::make_unique<ast::Type>();
    type->name = parseTypeName();

    if (isPunctuator("<")) {
        advance(); // past '<'
        type->genericArgs.push_back(parseType());
        while (isPunctuator(",")) {
            advance(); // past ','
            type->genericArgs.push_back(parseType());
        }
        if (!consumeClosingAngle()) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '>' to close generic arguments");
        }
    }

    if (isPunctuator("?")) {
        type->nullable = true;
        advance();
    }
    return type;
}

std::unique_ptr<ast::Member> Parser::parseMember() {
    // §13: every member declares its visibility explicitly. Slice 6 only
    // recognizes the FieldDecl shape after that.
    ast::Visibility visibility;
    if (isKeyword("public"))         visibility = ast::Visibility::Public;
    else if (isKeyword("private"))   visibility = ast::Visibility::Private;
    else if (isKeyword("internal"))  visibility = ast::Visibility::Internal;
    else if (isKeyword("protected")) visibility = ast::Visibility::Protected;
    else {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected member visibility (public, private, "
                            "internal, or protected)");
        return nullptr;
    }
    advance();

    // Member-modifier prefix. The set the member-kind-switch below recognises
    // includes A.4 FieldDecl modifiers (`static`, `owned`, `readonly`) and the
    // method-only ones (`async`, `abstract`, `virtual`, `sealed`, `override`).
    // The semantically valid combinations per member kind are sema's job.
    std::vector<std::string_view> modifiers;
    while (isKeyword("static")   || isKeyword("owned")    ||
           isKeyword("readonly") || isKeyword("async")    ||
           isKeyword("abstract") || isKeyword("virtual")  ||
           isKeyword("sealed")   || isKeyword("override")) {
        modifiers.push_back(current_.text);
        advance();
    }

    // Member-kind keyword (`method` for slice 10; `property`, `event`, …
    // arrive with their slices). Without one of those, this is a FieldDecl
    // and the next token must be its name.
    if (isKeyword("method")) {
        return std::unique_ptr<ast::Member>(
            parseMethodDecl(visibility, std::move(modifiers)).release());
    }

    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected field name or member kind keyword "
            "(other member forms are not yet supported)");
        return nullptr;
    }

    auto field = std::make_unique<ast::FieldDecl>();
    field->visibility = visibility;
    field->modifiers = std::move(modifiers);
    field->name = current_.text;
    advance();

    if (!consumePunctuator(":")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ':' after field name");
    }
    field->type = parseType();
    if (consumePunctuator(":=")) {
        field->initializer = parseExpression();
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after field declaration");
    }

    // Reseat the raw pointer explicitly (Node's deleted move blocks the
    // implicit unique_ptr<Derived> -> unique_ptr<Base> conversion).
    return std::unique_ptr<ast::Member>(field.release());
}

namespace {

// Higher precedence = tighter binding. The numeric scale is internal — we
// just need an ordering consistent with §17.1's precedence table. Returns 0
// for tokens that aren't binary operators handled in this slice.
int binaryPrecedence(const Token& tok) {
    if (tok.kind == TokenKind::Punctuator) {
        const std::string_view t = tok.text;
        // §17.1 level 3 multiplicative.
        if (t == "*" || t == "/") return 14;
        // §17.1 level 4 additive.
        if (t == "+" || t == "-") return 13;
        // §17.1 level 10 relational (subset — `in`/`is`/`as` are keywords).
        if (t == "<" || t == ">" || t == "<=" || t == ">=") return 7;
        // §17.1 level 11 equality.
        if (t == "=" || t == "<>") return 6;
    }
    if (tok.kind == TokenKind::Keyword) {
        const std::string_view t = tok.text;
        if (t == "mod") return 14; // multiplicative
        if (t == "and") return 4;  // §17.1 level 13 logical AND
        if (t == "or")  return 3;  // §17.1 level 14 logical OR
    }
    return 0;
}

} // namespace

std::unique_ptr<ast::Expr> Parser::parseExpression() {
    return parseBinaryExpression(1);
}

std::unique_ptr<ast::Expr> Parser::parseBinaryExpression(int minPrecedence) {
    // Precedence climbing. All §17.1 operators handled in this slice are
    // left-associative; we recurse on `prec + 1` for the right operand so
    // same-precedence ops associate to the left.
    auto left = parseUnaryExpression();
    for (;;) {
        const int prec = binaryPrecedence(current_);
        if (prec < minPrecedence) break;
        const Token opTok = current_;
        advance();
        auto right = parseBinaryExpression(prec + 1);
        auto bin = std::make_unique<ast::BinaryExpr>();
        bin->op = opTok.text;
        bin->left = std::move(left);
        bin->right = std::move(right);
        left = std::unique_ptr<ast::Expr>(bin.release());
    }
    return left;
}

std::unique_ptr<ast::Expr> Parser::parseUnaryExpression() {
    // §17.1 level 2 prefix unary. Right-associative — `--x` parses as
    // `- (-x)`. `await` (also level 2) lands with the async slice.
    if (isPunctuator("+") || isPunctuator("-") || isPunctuator("~") ||
        isKeyword("not")) {
        const Token opTok = current_;
        advance();
        auto u = std::make_unique<ast::UnaryExpr>();
        u->op = opTok.text;
        u->operand = parseUnaryExpression();
        return std::unique_ptr<ast::Expr>(u.release());
    }
    return parsePostfix(parsePrimaryExpression());
}

std::unique_ptr<ast::Expr> Parser::parsePrimaryExpression() {
    // Literals from the lexer (§18 forms covered in lexer slices 3 and 4).
    if (current_.kind == TokenKind::IntLiteral) {
        auto e = std::make_unique<ast::IntLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::FloatLiteral) {
        auto e = std::make_unique<ast::FloatLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::CharLiteral) {
        auto e = std::make_unique<ast::CharLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::StringLiteral) {
        auto e = std::make_unique<ast::StringLiteralExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (current_.kind == TokenKind::VerbatimStringLiteral) {
        auto e = std::make_unique<ast::VerbatimStringExpr>();
        e->text = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }

    // §18.2 boolean and null keyword literals.
    if (isKeyword("true") || isKeyword("false")) {
        auto e = std::make_unique<ast::BoolLiteralExpr>();
        e->value = (current_.text == "true");
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    if (isKeyword("null")) {
        advance();
        return std::unique_ptr<ast::Expr>(
            std::make_unique<ast::NullLiteralExpr>().release());
    }

    if (current_.kind == TokenKind::Identifier) {
        auto e = std::make_unique<ast::IdentifierExpr>();
        e->name = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }

    if (consumePunctuator("(")) {
        // No ParenExpr node: source-level parens only group the parse; the
        // resulting tree shape encodes the grouping directly. See Node.hpp.
        auto inner = parseExpression();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close parenthesized expression");
        }
        return inner;
    }

    diagnostics_.report(Severity::Error, current_.location,
                        "expected expression");
    return nullptr;
}

std::unique_ptr<ast::Expr> Parser::parsePostfix(std::unique_ptr<ast::Expr> head) {
    // §A.5 primary-level postfix chain. Only `.member` and `(args)` here;
    // `[i]`, `?.`, `?(`, `!` (postfix) land with the postfix-extras slice.
    for (;;) {
        if (consumePunctuator(".")) {
            if (current_.kind != TokenKind::Identifier) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected member name after '.'");
                break;
            }
            auto access = std::make_unique<ast::MemberAccessExpr>();
            access->target = std::move(head);
            access->member = current_.text;
            advance();
            head = std::unique_ptr<ast::Expr>(access.release());
        } else if (consumePunctuator("(")) {
            auto call = std::make_unique<ast::CallExpr>();
            call->callee = std::move(head);
            if (!isPunctuator(")")) {
                call->args.push_back(parseExpression());
                while (consumePunctuator(",")) {
                    call->args.push_back(parseExpression());
                }
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected ')' to close call argument list");
            }
            head = std::unique_ptr<ast::Expr>(call.release());
        } else {
            break;
        }
    }
    return head;
}

std::unique_ptr<ast::MethodDecl> Parser::parseMethodDecl(
    ast::Visibility visibility, std::vector<std::string_view> modifiers) {
    // Caller has verified `current_` is the `method` keyword.
    auto m = std::make_unique<ast::MethodDecl>();
    m->visibility = visibility;
    m->modifiers = std::move(modifiers);
    advance(); // past `method`

    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected method name");
    } else {
        m->name = current_.text;
        advance();
    }

    m->genericParams = parseGenericParams();

    if (!consumePunctuator("(")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '(' to begin parameter list");
    }
    m->params = parseParamList();
    if (!consumePunctuator(")")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ')' to close parameter list");
    }

    if (consumePunctuator(":")) {
        if (isKeyword("owned")) {
            m->returnOwned = true;
            advance();
        }
        m->returnType = parseType();
    }

    // A.4 MethodBody: `=> Expression ";"` | `Statement* "end" Identifier? ";"`
    // | `";"`. (GenericConstraints* between signature and body land later.)
    if (consumePunctuator("=>")) {
        m->bodyKind = ast::MethodBodyKind::Expression;
        m->exprBody = parseExpression();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after expression-body method");
        }
    } else if (consumePunctuator(";")) {
        m->bodyKind = ast::MethodBodyKind::Abstract;
    } else {
        // Block body (A.4 MethodBody form 2): `Statement* "end" Identifier? ";"`.
        m->bodyKind = ast::MethodBodyKind::Block;
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                m->stmts.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance(); // crude recovery
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close method body");
            return m;
        }
        if (current_.kind == TokenKind::Identifier) {
            m->endLabel = current_.text;
            if (!m->name.empty() && m->endLabel != m->name) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "end label does not match the method name");
            }
            advance();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end'");
        }
    }
    return m;
}

std::unique_ptr<ast::Stmt> Parser::parseStatement() {
    // §A.6 Statement dispatch. Slice 11 covers LocalDecl, ReturnStmt, and
    // the expression-led shapes (AssignmentStmt vs ExpressionStmt, decided
    // after parsing the leading expression).

    // LocalDecl: `var Identifier (":" Type)? (":=" Expression)? ";"` or
    // `using var ...`. The async `await using var` form lands later.
    if (isKeyword("var") || isKeyword("using")) {
        auto decl = std::make_unique<ast::LocalDecl>();
        if (consumeKeyword("using")) {
            decl->isUsing = true;
            if (!consumeKeyword("var")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected 'var' after 'using'");
            }
        } else {
            advance(); // past `var`
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected local-variable name");
        } else {
            decl->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            decl->type = parseType();
        }
        if (consumePunctuator(":=")) {
            decl->initializer = parseExpression();
        } else if (decl->isUsing) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`using var` requires an initializer (A.6 LocalDecl)");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after local declaration");
        }
        return std::unique_ptr<ast::Stmt>(decl.release());
    }

    // ReturnStmt: `return Expression? ";"`.
    if (isKeyword("return")) {
        advance();
        auto ret = std::make_unique<ast::ReturnStmt>();
        if (!isPunctuator(";")) {
            ret->value = parseExpression();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after return statement");
        }
        return std::unique_ptr<ast::Stmt>(ret.release());
    }

    // Otherwise parse an expression and decide: AssignmentStmt if it's
    // followed by an assignment operator, ExpressionStmt otherwise.
    auto lhs = parseExpression();
    if (current_.kind == TokenKind::Punctuator &&
        (current_.text == ":="  || current_.text == "+="  ||
         current_.text == "-="  || current_.text == "*="  ||
         current_.text == "/="  || current_.text == "&="  ||
         current_.text == "|="  || current_.text == "^="  ||
         current_.text == "<<=" || current_.text == ">>=" ||
         current_.text == "??=")) {
        auto a = std::make_unique<ast::AssignmentStmt>();
        a->target = std::move(lhs);
        a->op = current_.text;
        advance();
        a->value = parseExpression();
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after assignment");
        }
        return std::unique_ptr<ast::Stmt>(a.release());
    }

    auto es = std::make_unique<ast::ExpressionStmt>();
    es->expr = std::move(lhs);
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after expression statement");
    }
    return std::unique_ptr<ast::Stmt>(es.release());
}

std::vector<ast::Param> Parser::parseParamList() {
    // Slice 10: each param is `Identifier ":" Type`. ParamModifiers (ref,
    // out, out owned, owned) and default values arrive with later slices.
    std::vector<ast::Param> params;
    if (isPunctuator(")")) return params;

    auto parseOne = [&]() -> ast::Param {
        ast::Param p;
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected parameter name");
        } else {
            p.name = current_.text;
            advance();
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ':' after parameter name");
        }
        p.type = parseType();
        return p;
    };

    params.push_back(parseOne());
    while (consumePunctuator(",")) {
        params.push_back(parseOne());
    }
    return params;
}

std::vector<ast::GenericParam> Parser::parseGenericParams() {
    // A.3: `GenericParams ::= "<" GenericParam ("," GenericParam)* ">"`.
    // `GenericParam ::= Variance? Identifier`, Variance ∈ {`in`, `out`}.
    std::vector<ast::GenericParam> params;
    if (!isPunctuator("<")) return params;
    advance(); // past '<'

    auto parseOne = [&]() -> ast::GenericParam {
        ast::GenericParam p;
        if (isKeyword("in")) {
            p.variance = current_.text;
            advance();
        } else if (isKeyword("out")) {
            p.variance = current_.text;
            advance();
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected type parameter name");
            return p;
        }
        p.name = current_.text;
        advance();
        return p;
    };

    params.push_back(parseOne());
    while (isPunctuator(",")) {
        advance();
        params.push_back(parseOne());
    }
    if (!consumeClosingAngle()) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '>' to close generic parameter list");
    }
    return params;
}

std::vector<std::unique_ptr<ast::Type>> Parser::parseInheritance() {
    // A.3: `Inheritance ::= ":" InheritItem ("," InheritItem)*`.
    // `InheritItem ::= Type ("(" ArgList? ")")?` — the optional argument
    // list is tokenized away here; ArgList contains expressions which
    // arrive in the expression slice.
    std::vector<std::unique_ptr<ast::Type>> items;
    if (!isPunctuator(":")) return items;
    advance(); // past ':'

    auto parseOne = [&]() -> std::unique_ptr<ast::Type> {
        auto t = parseType();
        if (isPunctuator("(")) {
            // Skip a balanced parenthesised group without parsing its contents.
            int depth = 1;
            advance();
            while (depth > 0 && current_.kind != TokenKind::EndOfFile) {
                if (isPunctuator("("))      ++depth;
                else if (isPunctuator(")")) --depth;
                advance();
            }
        }
        return t;
    };

    items.push_back(parseOne());
    while (isPunctuator(",")) {
        advance();
        items.push_back(parseOne());
    }
    return items;
}

bool Parser::consumeClosingAngle() {
    // The lexer's maximal-munch on `>` produces single tokens for `>`, `>=`,
    // `>>`, and `>>=`. Closing a generic-arg list needs to consume one `>`,
    // splitting `>=` / `>>` / `>>=` in place by mutating current_ so the
    // remaining chars stay for the outer scope to consume. The mutated text
    // is still a substring of `source_` (held by the lexer), so the
    // string_view stays valid.
    if (current_.kind != TokenKind::Punctuator) return false;
    if (current_.text == ">") {
        advance();
        return true;
    }
    if (current_.text == ">>" || current_.text == ">>=" ||
        current_.text == ">=") {
        Token leftover = current_;
        leftover.text = leftover.text.substr(1);
        leftover.location.column += 1;
        leftover.location.offset += 1;
        current_ = leftover;
        return true;
    }
    return false;
}

std::unique_ptr<ast::QualifiedName> Parser::parseTypeName() {
    // Like parseQualifiedName but accepts a primitive-type keyword as the
    // first component (§3.4). Subsequent components after `.` must be plain
    // identifiers — the primitive keywords are unconditionally reserved per
    // §B.1 and cannot appear inside a qualified namespace path.
    auto name = std::make_unique<ast::QualifiedName>();
    const bool primitiveStart = current_.kind == TokenKind::Keyword &&
                                isPrimitiveTypeKeyword(current_.text);
    if (current_.kind != TokenKind::Identifier && !primitiveStart) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected type name");
        return name;
    }
    name->parts.push_back(current_.text);
    advance();
    while (isPunctuator(".")) {
        advance();
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected identifier after '.'");
            break;
        }
        name->parts.push_back(current_.text);
        advance();
    }
    return name;
}

std::unique_ptr<ast::QualifiedName> Parser::parseQualifiedName() {
    // A.2: `QualifiedName ::= Identifier ("." Identifier)*`
    auto name = std::make_unique<ast::QualifiedName>();
    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected identifier");
        return name;
    }
    name->parts.push_back(current_.text);
    advance();
    while (isPunctuator(".")) {
        advance(); // past '.'
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected identifier after '.'");
            break;
        }
        name->parts.push_back(current_.text);
        advance();
    }
    return name;
}

} // namespace bezel
