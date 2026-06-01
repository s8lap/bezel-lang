#include "parser/Parser.hpp"

#include <algorithm>
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

// §18.5 / §18.7: split an interpolated string token into its alternating
// literal and expression parts.  The lexer has already validated balanced
// braces, so we can walk the content in a single pass:
//  - literal chars accumulate into `current_literal`
//  - `{` opens an expression region; we find the matching `}` and create a
//    sub-parser to parse one expression from that slice
//  - `{{` / `}}` are literal-brace escapes; verbatim strings skip all `\`
//  - `\` in non-verbatim strings is consumed as an escape sequence
void Parser::splitInterpolation(ast::InterpolatedStringExpr& e) {
    std::string_view raw = e.text;   // full `$"..."` or `$@"..."` token text
    // Determine where the content starts and ends.
    size_t start = e.isVerbatim ? 3 : 2;  // skip `$@"` or `$"`
    if (raw.size() < start + 1) return;
    size_t end = raw.size() - 1;          // before the closing `"`

    std::string literal;
    size_t i = start;
    while (i < end) {
        char c = raw[i];
        // Literal-brace escapes `{{` → `{` and `}}` → `}`.
        if (c == '{' && i + 1 < end && raw[i + 1] == '{') {
            literal.push_back('{');
            i += 2;
            continue;
        }
        if (c == '}' && i + 1 < end && raw[i + 1] == '}') {
            literal.push_back('}');
            i += 2;
            continue;
        }
        // Expression region `{expr}`.
        if (c == '{') {
            ++i; // past `{`
            // Find matching `}` tracking brace depth (and nested strings).
            size_t exprStart = i;
            int depth = 1;
            while (i < end && depth > 0) {
                char ic = raw[i];
                if (ic == '"') {
                    // Nested string — skip to its closing `"`.
                    ++i;
                    while (i < end && raw[i] != '"') {
                        if (!e.isVerbatim && raw[i] == '\\') ++i; // escape
                        ++i;
                    }
                    if (i < end) ++i; // closing `"`
                } else if (ic == '{') { ++depth; ++i; }
                else if (ic == '}') { --depth; if (depth > 0) ++i; else break; }
                else ++i;
            }
            // `raw[exprStart..i)` is the expression text.
            std::string_view exprText = raw.substr(exprStart, i - exprStart);
            if (i < end) ++i; // consume `}`

            // Flush any accumulated literal.
            if (!literal.empty()) {
                ast::InterpPart lp;
                lp.literal = std::move(literal);
                e.parts.push_back(std::move(lp));
                literal.clear();
            }
            // Sub-parse the expression.
            if (!exprText.empty()) {
                Lexer subLex(exprText, diagnostics_);
                Parser subPar(subLex, diagnostics_);
                auto expr = subPar.parseExpression();
                if (expr) {
                    ast::InterpPart ep;
                    ep.expr = std::move(expr);
                    e.parts.push_back(std::move(ep));
                }
            }
            continue;
        }
        // Non-verbatim escape: skip the escape char, let processStringText
        // handle the actual byte at codegen time — just copy the raw chars.
        if (!e.isVerbatim && c == '\\' && i + 1 < end) {
            literal.push_back(c);
            literal.push_back(raw[i + 1]);
            i += 2;
            continue;
        }
        literal.push_back(c);
        ++i;
    }
    // Trailing literal.
    if (!literal.empty()) {
        ast::InterpPart lp;
        lp.literal = std::move(literal);
        e.parts.push_back(std::move(lp));
    }
}

void Parser::advance() {
    if (!peeked_.empty()) {
        current_ = std::move(peeked_.front());
        peeked_.pop_front();
    } else {
        current_ = lexer_.next();
    }
}

const Token& Parser::peekAt(std::size_t k) {
    // k=0 returns the current token; k>=1 reads from the buffered lookahead,
    // filling it lazily from the lexer. Cheap for the small k typically
    // needed; the discriminator for `(…) => …` may scan to the matching `)`
    // before deciding lambda-vs-paren.
    if (k == 0) return current_;
    while (peeked_.size() < k) {
        peeked_.push_back(lexer_.next());
    }
    return peeked_[k - 1];
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

    // ExtensionDecl (§A.3, §26): `Visibility? "extension" Type
    // GenericConstraints* ExtensionMember* "end" ";"`. Visibility is parsed
    // first if present, then `extension`. Since visibility is also used by
    // class/etc., we tentatively read it and dispatch.
    if (isKeyword("extension")) {
        return parseExtensionDecl(ast::Visibility::None);
    }
    if (isKeyword("extern")) {
        return parseExternBlock();
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
    // Now-after-visibility, check for `extension` (admits visibility prefix).
    if (isKeyword("extension")) {
        return parseExtensionDecl(visibility);
    }

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

    // §A.3 EnumInheritance:
    //   EnumInheritance ::= ":" IntegerType ("," InheritItem)*
    //                     | ":" InheritItem ("," InheritItem)*
    // Both forms parse through the regular Type machinery (IntegerType is
    // a primitive-type keyword admitted by `parseTypeName`); sema picks
    // the meaning by checking whether the first item is an IntegerType.
    if (decl->kind == ast::TypeDeclKind::Enum && consumePunctuator(":")) {
        decl->baseTypes.push_back(parseType());
        while (consumePunctuator(",")) {
            decl->baseTypes.push_back(parseType());
        }
    }
    // §A.3: GenericParams? and Inheritance? appear between the name and the
    // body for classes, interfaces, and records.
    if (decl->kind != ast::TypeDeclKind::Enum) {
        decl->genericParams = parseGenericParams();
        // §A.3 RecordDecl's PositionalFields and ClassDecl's
        // PrimaryCtorParams share the exact same syntactic shape
        // `"(" ParamList? ")"` at the same position (between generic params
        // and inheritance). Same AST representation (`positionalFields`).
        if ((decl->kind == ast::TypeDeclKind::Record ||
             decl->kind == ast::TypeDeclKind::Class) &&
            isPunctuator("(")) {
            advance();
            decl->hasPositionalFields = true;
            if (!isPunctuator(")")) {
                // Records: PositionalField list (SerializedClause-bearing,
                // no modifiers, no default). Classes: PrimaryCtorParam list
                // (ParamList — modifiers + default, no SerializedClause).
                decl->positionalFields =
                    (decl->kind == ast::TypeDeclKind::Record)
                        ? parsePositionalFieldList()
                        : parseParamList();
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close positional / primary-ctor list");
            }
        }
        // SerializedClassClause sits between PrimaryCtorParams and Inheritance
        // per A.3. The grammar only puts it on ClassDecl, but RecordDecl
        // shares the same syntactic shape at this position; we accept it on
        // both kinds and let sema enforce kind-specific restrictions.
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "serialized") {
            // Disambiguate from A.4 SerializedClause (on positional fields):
            // class-level shape keywords are tree/graph/cyclic, never skip
            // or optional or a string. peekAt(1) tells us which form.
            const Token& nxt = peekAt(1);
            if (nxt.kind == TokenKind::Identifier &&
                (nxt.text == "tree" || nxt.text == "graph" ||
                 nxt.text == "cyclic")) {
                decl->serializedClass = parseSerializedClassClause();
            }
        }
        decl->baseTypes = parseInheritance();
        decl->genericConstraints = parseGenericConstraints();
    }

    // §A.3 RecordDecl shorthand body: `record Foo(...) ;` (no `end`).
    if (decl->kind == ast::TypeDeclKind::Record &&
        consumePunctuator(";")) {
        decl->isRecordShorthand = true;
        return decl;
    }

    // §A.4 Members. Classes and records share the same Member set; interfaces
    // use the smaller signature-only InterfaceMember grammar; enums get the
    // EnumConstants list (below).
    if (decl->kind == ast::TypeDeclKind::Interface) {
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto m = parseInterfaceMember();
            if (m) {
                decl->members.push_back(std::move(m));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
    } else if (decl->kind == ast::TypeDeclKind::Class ||
               decl->kind == ast::TypeDeclKind::Record) {
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto member = parseMember();
            if (member) {
                decl->members.push_back(std::move(member));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
    } else if (decl->kind == ast::TypeDeclKind::Enum) {
        // A.3: `EnumConstants ::= EnumConstant ("," EnumConstant)* ";"`.
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected enum constant name (enum body requires at least "
                "one)");
        } else {
            ast::EnumConstant ec;
            ec.name = current_.text;
            advance();
            if (consumePunctuator(":=")) {
                ec.value = parseExpression();
            }
            if (isSerializedClauseStart()) {
                ec.serialized = parseSerializedClause();
            }
            decl->enumConstants.push_back(std::move(ec));
            while (consumePunctuator(",")) {
                if (current_.kind != TokenKind::Identifier) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected enum constant name after ','");
                    break;
                }
                ast::EnumConstant ec2;
                ec2.name = current_.text;
                advance();
                if (consumePunctuator(":=")) {
                    ec2.value = parseExpression();
                }
                if (isSerializedClauseStart()) {
                    ec2.serialized = parseSerializedClause();
                }
                decl->enumConstants.push_back(std::move(ec2));
            }
            if (!consumePunctuator(";")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ';' after enum constants");
            }
            // EnumMember* after the constants list. Sema restricts the
            // valid member kinds to MethodDecl, ExpressionPropertyDecl,
            // StaticReadonlyDecl, and StaticConstDecl (A.3 EnumMember).
            // We accept the full Member grammar at the parse level and let
            // sema filter — keeps the dispatch shared with class/record.
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto m = parseMember();
                if (m) {
                    decl->members.push_back(std::move(m));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
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

std::unique_ptr<ast::TopDecl> Parser::parseExtensionDecl(
    ast::Visibility visibility) {
    // A.3 `ExtensionDecl ::= Visibility? "extension" Type GenericConstraints*
    //                         ExtensionMember* "end" ";"`.
    auto ext = std::make_unique<ast::ExtensionDecl>();
    ext->visibility = visibility;
    advance(); // past `extension`
    ext->targetType = parseType();
    ext->genericConstraints = parseGenericConstraints();
    while (!isKeyword("end") &&
           current_.kind != TokenKind::EndOfFile) {
        auto m = parseMember();
        if (m) {
            ext->members.push_back(std::move(m));
        } else if (current_.kind != TokenKind::EndOfFile) {
            advance();
        }
    }
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end' to close extension block");
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end' of extension block");
    }
    return std::unique_ptr<ast::TopDecl>(ext.release());
}

std::unique_ptr<ast::TopDecl> Parser::parseExternBlock() {
    // A.3 `ExternBlock ::= "extern" StringLiteral
    //                        ("convention" StringLiteral)?
    //                        ExternDecl+ "end" ";"`.
    auto blk = std::make_unique<ast::ExternBlock>();
    advance(); // past `extern`
    if (current_.kind != TokenKind::StringLiteral) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected library-name string literal after "
                            "'extern'");
    } else {
        blk->library = current_.text;
        advance();
    }
    // `convention` is contextual (App. B.2) so the lexer emits Identifier.
    if (current_.kind == TokenKind::Identifier &&
        current_.text == "convention") {
        advance();
        if (current_.kind != TokenKind::StringLiteral) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected convention string literal");
        } else {
            blk->convention = current_.text;
            advance();
        }
    }
    while (!isKeyword("end") &&
           current_.kind != TokenKind::EndOfFile) {
        ast::ExternDecl decl;
        // Visibility?
        if (isKeyword("public"))         { decl.visibility = ast::Visibility::Public; advance(); }
        else if (isKeyword("private"))   { decl.visibility = ast::Visibility::Private; advance(); }
        else if (isKeyword("internal"))  { decl.visibility = ast::Visibility::Internal; advance(); }
        else if (isKeyword("protected")) { decl.visibility = ast::Visibility::Protected; advance(); }
        if (!consumeKeyword("method")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'method' in extern declaration");
            advance(); // make progress
            continue;
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected method name");
        } else {
            decl.name = current_.text;
            advance();
        }
        if (!consumePunctuator("(")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '(' to begin parameter list");
        }
        decl.params = parseParamList();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close parameter list");
        }
        if (consumePunctuator(":")) {
            if (isKeyword("owned")) {
                decl.returnOwned = true;
                advance();
            }
            decl.returnType = parseType();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after extern declaration");
        }
        blk->methods.push_back(std::move(decl));
    }
    if (blk->methods.empty()) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "extern block requires at least one ExternDecl (A.3)");
    }
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end' to close extern block");
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end' of extern block");
    }
    return std::unique_ptr<ast::TopDecl>(blk.release());
}

std::unique_ptr<ast::TypeAlias> Parser::parseTypeAlias() {
    // A.2: `TypeAliasDecl ::= "type" Identifier GenericParams? "=" Type
    //                          GenericConstraints* ";"`. Slice 27 completes
    // the optional GenericParams and trailing GenericConstraints.
    auto alias = std::make_unique<ast::TypeAlias>();
    advance(); // past `type`

    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected type alias name after 'type'");
    } else {
        alias->name = current_.text;
        advance();
    }

    alias->genericParams = parseGenericParams();

    if (!consumePunctuator("=")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '=' in type alias");
    }

    alias->aliasedType = parseType();
    alias->genericConstraints = parseGenericConstraints();

    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after type alias");
    }
    return alias;
}

std::unique_ptr<ast::Type> Parser::parseType() {
    // A.5 Type ::= TypeNoNullable "?"?
    // A.5 TypeNoNullable ::= QualifiedName GenericArgs?
    //                      | InlineArrayType
    //                      | FunctionType
    //                      | "(" ")"            /* unit */
    auto type = std::make_unique<ast::Type>();

    // InlineArrayType: `[ ConstIntExpr "owned"? ] Type`.
    if (consumePunctuator("[")) {
        type->kind = ast::TypeKind::InlineArray;
        type->arraySize = parseExpression();
        if (consumeKeyword("owned")) {
            type->arrayOwned = true;
        }
        if (!consumePunctuator("]")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ']' to close inline-array size");
        }
        type->arrayElement = parseType();
        if (isPunctuator("?")) {
            // Per the §A.5 note, nullable inline-array types are not
            // provided in this revision; admit syntactically and let
            // sema diagnose.
            type->nullable = true;
            advance();
        }
        return type;
    }

    // Unit type `()` or FunctionType `(...) -> ...`.
    if (consumePunctuator("(")) {
        if (consumePunctuator(")")) {
            // `()` followed by `->`: zero-param function type.
            if (consumePunctuator("->")) {
                type->kind = ast::TypeKind::Function;
                if (consumeKeyword("owned")) type->funcReturnOwned = true;
                type->funcReturn = parseType();
            } else {
                type->kind = ast::TypeKind::Unit;
            }
        } else {
            // Non-empty params; must be function type.
            type->kind = ast::TypeKind::Function;
            auto parseFuncParam = [&]() -> ast::FuncParamType {
                ast::FuncParamType fp;
                if (consumeKeyword("ref")) {
                    fp.modifier = "ref";
                } else if (consumeKeyword("out")) {
                    if (consumeKeyword("owned")) fp.modifier = "out owned";
                    else                          fp.modifier = "out";
                } else if (consumeKeyword("owned")) {
                    fp.modifier = "owned";
                }
                // Optional `Identifier ":"` prefix per A.5 FuncParamType.
                if (current_.kind == TokenKind::Identifier &&
                    peekAt(1).kind == TokenKind::Punctuator &&
                    peekAt(1).text == ":") {
                    fp.name = current_.text;
                    advance(); // past identifier
                    advance(); // past ':'
                }
                fp.type = parseType();
                return fp;
            };
            type->funcParams.push_back(parseFuncParam());
            while (consumePunctuator(",")) {
                type->funcParams.push_back(parseFuncParam());
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close function-type parameter list");
            }
            if (!consumePunctuator("->")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected '->' after function-type parameter list");
            }
            if (consumeKeyword("owned")) type->funcReturnOwned = true;
            type->funcReturn = parseType();
        }
        if (isPunctuator("?")) {
            type->nullable = true;
            advance();
        }
        return type;
    }

    // Default: NamedType.
    type->kind = ast::TypeKind::Named;
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
    // §13: most members declare their visibility explicitly. Destructors
    // (no visibility per A.4) and the static-constructor form are
    // exceptions, so we collect visibility optionally and validate per
    // member-kind below.
    ast::Visibility visibility = ast::Visibility::None;
    if (isKeyword("public")) {
        visibility = ast::Visibility::Public; advance();
    } else if (isKeyword("private")) {
        visibility = ast::Visibility::Private; advance();
    } else if (isKeyword("internal")) {
        visibility = ast::Visibility::Internal; advance();
    } else if (isKeyword("protected")) {
        visibility = ast::Visibility::Protected; advance();
    }

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

    // A.4 NestedTypeDecl as a Member: `class` / `interface` / `record` /
    // `enum` keyword after Visibility?+Modifier*. The modifier set for
    // top-level type-decls (`abstract`/`sealed`/`static`) overlaps with the
    // member-modifier set we already collected, so we hand them down.
    if (isKeyword("class") || isKeyword("interface") ||
        isKeyword("record") || isKeyword("enum")) {
        // Modifiers that are valid on nested types are the same as on
        // top-level types per A.3 — `abstract`/`sealed`/`static`. The
        // member-modifier set is a superset; semantic validation belongs
        // to sema (§11.7).
        auto td = parseTypeOrInterfaceDecl(visibility, std::move(modifiers));
        auto ntm = std::make_unique<ast::NestedTypeMember>();
        ntm->decl = std::move(td);
        return std::unique_ptr<ast::Member>(ntm.release());
    }

    // Member-kind keyword dispatch.
    if (isKeyword("const")) {
        if (visibility == ast::Visibility::None) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "const declarations require explicit visibility");
        }
        const bool isStatic =
            std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("static")) != modifiers.end();
        advance(); // past `const`
        auto cd = std::make_unique<ast::ConstDecl>();
        cd->visibility = visibility;
        cd->isStatic = isStatic;
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected const name");
        } else {
            cd->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            cd->type = parseType();
        }
        if (!consumePunctuator(":=")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ':=' in const declaration");
        }
        cd->value = parseExpression();
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after const declaration");
        }
        return std::unique_ptr<ast::Member>(cd.release());
    }
    if (isKeyword("event")) {
        if (visibility == ast::Visibility::None) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "event declarations require explicit visibility");
        }
        const bool isStatic =
            std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("static")) != modifiers.end();
        const bool isAsync =
            std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("async")) != modifiers.end();
        advance(); // past `event`
        auto ed = std::make_unique<ast::EventDecl>();
        ed->visibility = visibility;
        ed->isStatic = isStatic;
        ed->isAsync = isAsync;
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected event name");
        } else {
            ed->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            ed->type = parseType();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after event declaration");
        }
        return std::unique_ptr<ast::Member>(ed.release());
    }
    if (isKeyword("property")) {
        if (visibility == ast::Visibility::None) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "property declarations require explicit visibility");
        }
        advance(); // past `property`
        auto pd = std::make_unique<ast::PropertyDecl>();
        pd->visibility = visibility;
        pd->modifiers = std::move(modifiers);
        // A.4 IndexerDecl: `[ParamList]` in place of the Identifier name.
        if (consumePunctuator("[")) {
            pd->isIndexer = true;
            pd->indexerParams = parseParamList();
            if (!consumePunctuator("]")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ']' to close indexer parameter list");
            }
        } else if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected property name or '[' for indexer");
        } else {
            pd->name = current_.text;
            advance();
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ':' after property name");
        }
        pd->type = parseType();
        // Body shape dispatch per A.4 PropertyBody. Three forms:
        //   `=> Expression ;`                       Expression-body
        //   `(":=" Expression)? SerializedClause? ";"`   Auto-property
        //   `Accessor* "end" Identifier? ";"`            Accessor block
        if (consumePunctuator("=>")) {
            pd->bodyKind = ast::PropertyBodyKind::Expression;
            pd->exprBody = parseExpression();
            if (!consumePunctuator(";")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ';' after expression-body property");
            }
        } else {
            // Distinguish AutoAccessorVis-bearing auto-property from a
            // multi-arm accessor block. AutoAccessorVis is `Visibility
            // get|set` immediately followed by `:=` or `;` (the auto-form
            // continuation). Anything else after `Visibility get|set`
            // (=>, (, …) belongs to an accessor block.
            const bool autoAccVisCandidate =
                (isKeyword("public") || isKeyword("private") ||
                 isKeyword("internal") || isKeyword("protected")) &&
                peekAt(1).kind == TokenKind::Identifier &&
                (peekAt(1).text == "get" || peekAt(1).text == "set") &&
                peekAt(2).kind == TokenKind::Punctuator &&
                (peekAt(2).text == ":=" || peekAt(2).text == ";");
            if (autoAccVisCandidate) {
                pd->bodyKind = ast::PropertyBodyKind::Auto;
                pd->hasAutoAccessorVis = true;
                if (isKeyword("public")) pd->autoAccessorVis =
                    ast::Visibility::Public;
                else if (isKeyword("private")) pd->autoAccessorVis =
                    ast::Visibility::Private;
                else if (isKeyword("internal")) pd->autoAccessorVis =
                    ast::Visibility::Internal;
                else if (isKeyword("protected")) pd->autoAccessorVis =
                    ast::Visibility::Protected;
                advance(); // past visibility
                pd->autoAccessorIsSet = (current_.text == "set");
                advance(); // past get/set
                if (consumePunctuator(":=")) {
                    pd->autoInitializer = parseExpression();
                }
                if (isSerializedClauseStart()) {
                    pd->serialized = parseSerializedClause();
                }
                if (!consumePunctuator(";")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ';' after auto-property with "
                        "AutoAccessorVis");
                }
            } else if (isPropertyAccessorStart()) {
                pd->bodyKind = ast::PropertyBodyKind::Block;
                while (isPropertyAccessorStart()) {
                    pd->accessors.push_back(parsePropertyAccessor());
                }
                if (!consumeKeyword("end")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected 'end' to close property accessor block");
                }
                // §1.6 keyword-labelled construct. For a property, the
                // Identifier after `end` should match the property name.
                if (current_.kind == TokenKind::Identifier) {
                    pd->blockEndLabel = current_.text;
                    advance();
                }
                if (!consumePunctuator(";")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ';' after 'end' of property accessor block");
                }
            } else {
                pd->bodyKind = ast::PropertyBodyKind::Auto;
                if (consumePunctuator(":=")) {
                    pd->autoInitializer = parseExpression();
                }
                if (isSerializedClauseStart()) {
                    pd->serialized = parseSerializedClause();
                }
                if (!consumePunctuator(";")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ';' after auto-property");
                }
            }
        }
        return std::unique_ptr<ast::Member>(pd.release());
    }
    // `method` (slice 10), `method operator` (slice 27),
    // `constructor` / `destructor` (slice 19). `operator` is a contextual
    // keyword (App. B.2) so the lexer emits it as Identifier; we check by
    // identifier text rather than via isKeyword.
    if (consumeKeyword("method")) {
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "operator") {
            advance();
            return parseOperatorDeclBody(visibility, std::move(modifiers));
        }
        return std::unique_ptr<ast::Member>(
            parseMethodDeclBody(visibility, std::move(modifiers)).release());
    }
    if (isKeyword("constructor")) {
        // Static constructor form has no visibility per A.4; instance form
        // requires visibility. Both share the parsing path.
        const bool isStatic =
            std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("static")) != modifiers.end();
        if (!isStatic && visibility == ast::Visibility::None) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected member visibility before "
                                "'constructor' (or `static constructor`)");
        }
        auto ctor = std::make_unique<ast::ConstructorDecl>();
        ctor->visibility = visibility;
        ctor->isStatic = isStatic;
        advance(); // past `constructor`
        if (!consumePunctuator("(")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected '(' to begin constructor parameter list");
        }
        ctor->params = parseParamList();
        if (!consumePunctuator(")")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ')' to close constructor parameter list");
        }
        // CtorChain (`base(args);` / `this(args);`) is folded into the body's
        // first statement — sema recognises the pattern. Avoids parser
        // ambiguity with `this.field := ...`.
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                ctor->body.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close constructor body");
        }
        // Optional `constructor` label after `end` per §1.6 keyword-labelled
        // termination.
        if (isKeyword("constructor")) advance();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after 'end' of constructor");
        }
        return std::unique_ptr<ast::Member>(ctor.release());
    }
    if (isKeyword("destructor")) {
        if (visibility != ast::Visibility::None) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "destructor has no visibility per §A.4");
        }
        auto dtor = std::make_unique<ast::DestructorDecl>();
        advance(); // past `destructor`
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                dtor->body.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close destructor body");
        }
        if (isKeyword("destructor")) advance();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after 'end' of destructor");
        }
        return std::unique_ptr<ast::Member>(dtor.release());
    }

    // FieldDecl: visibility required.
    if (visibility == ast::Visibility::None) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected member visibility (public, private, internal, or "
            "protected) or member-kind keyword (method, constructor, "
            "destructor)");
        return nullptr;
    }
    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected field name or member kind keyword");
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

std::unique_ptr<ast::Member> Parser::parseInterfaceMember() {
    // §A.4 InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer |
    // IfaceEvent. No visibility (interface members are implicitly public per
    // §10.1), signatures only. The pre-keyword prefix is just `readonly?` (for
    // property) and `async?` (for event); both are scanned tentatively then
    // dispatched on the kind keyword.
    bool readonlyMod = false;
    bool asyncMod = false;
    while (isKeyword("readonly") || isKeyword("async")) {
        if (isKeyword("readonly")) {
            readonlyMod = true;
        } else {
            asyncMod = true;
        }
        advance();
    }

    // IfaceProperty / IfaceIndexer.
    if (consumeKeyword("property")) {
        auto pd = std::make_unique<ast::PropertyDecl>();
        pd->visibility = ast::Visibility::None; // implicit public in iface
        if (readonlyMod) pd->modifiers.push_back("readonly");
        if (consumePunctuator("[")) {
            pd->isIndexer = true;
            pd->indexerParams = parseParamList();
            if (!consumePunctuator("]")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ']' to close interface indexer parameters");
            }
        } else if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected property name or '[' for indexer");
        } else {
            pd->name = current_.text;
            advance();
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ':' after interface property name");
        }
        pd->type = parseType();
        pd->bodyKind = ast::PropertyBodyKind::Auto; // iface: signature only
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after interface property declaration");
        }
        return std::unique_ptr<ast::Member>(pd.release());
    }

    // IfaceEvent.
    if (consumeKeyword("event")) {
        auto ed = std::make_unique<ast::EventDecl>();
        ed->visibility = ast::Visibility::None;
        ed->isAsync = asyncMod;
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected event name");
        } else {
            ed->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            ed->type = parseType();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after interface event declaration");
        }
        return std::unique_ptr<ast::Member>(ed.release());
    }

    // IfaceMethod: shares the MethodDecl AST shape (signature + abstract body).
    if (consumeKeyword("method")) {
        auto m = std::make_unique<ast::MethodDecl>();
        m->visibility = ast::Visibility::None;
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
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after interface method signature");
        }
        m->bodyKind = ast::MethodBodyKind::Abstract;
        return std::unique_ptr<ast::Member>(m.release());
    }

    diagnostics_.report(
        Severity::Error, current_.location,
        "expected 'method', 'property', or 'event' in interface body");
    return nullptr;
}

bool Parser::isPropertyAccessorStart() {
    // §A.4 Accessor: `Visibility? ("get" | "set")` opens an accessor in a
    // PropertyBody block form. `get` and `set` are contextual keywords
    // (App. B.2) so the lexer emits them as Identifier — checked by text.
    auto isGetOrSet = [](const Token& t) {
        return t.kind == TokenKind::Identifier &&
               (t.text == "get" || t.text == "set");
    };
    if (isGetOrSet(current_)) return true;
    if (isKeyword("public") || isKeyword("private") ||
        isKeyword("internal") || isKeyword("protected")) {
        return isGetOrSet(peekAt(1));
    }
    return false;
}

ast::PropertyAccessor Parser::parsePropertyAccessor() {
    // §A.4 `Accessor ::= Visibility? "get" AccessorBody`
    //                  | Visibility? "set" SetterParamSpec? GenericConstraints?
    //                      AccessorBody`.
    ast::PropertyAccessor acc;
    if (consumeKeyword("public")) {
        acc.visibility = ast::Visibility::Public;
    } else if (consumeKeyword("private")) {
        acc.visibility = ast::Visibility::Private;
    } else if (consumeKeyword("internal")) {
        acc.visibility = ast::Visibility::Internal;
    } else if (consumeKeyword("protected")) {
        acc.visibility = ast::Visibility::Protected;
    }
    if (current_.kind != TokenKind::Identifier ||
        (current_.text != "get" && current_.text != "set")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'get' or 'set' in property accessor");
        return acc;
    }
    acc.isSet = (current_.text == "set");
    advance();
    if (acc.isSet && consumePunctuator("(")) {
        // SetterParamSpec ::= "(" ParamMod? Identifier? ")".
        acc.hasSetterParamSpec = true;
        if (consumeKeyword("ref")) {
            acc.setterParamMod = "ref";
        } else if (consumeKeyword("out")) {
            if (consumeKeyword("owned")) {
                acc.setterParamMod = "out owned";
            } else {
                acc.setterParamMod = "out";
            }
        } else if (consumeKeyword("owned")) {
            acc.setterParamMod = "owned";
        }
        if (current_.kind == TokenKind::Identifier) {
            acc.setterParamName = current_.text;
            advance();
        }
        if (!consumePunctuator(")")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ')' to close setter parameter spec");
        }
    }
    // GenericConstraints? on set is admitted by the grammar; rare in
    // practice — deferred. Sema will reject any leftover `where` that
    // appears here in slice 37 since we currently don't consume it.
    //
    // AccessorBody dispatch.
    if (consumePunctuator("=>")) {
        if (consumeKeyword("do")) {
            acc.bodyKind = ast::PropertyAccessorBodyKind::Block;
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto s = parseStatement();
                if (s) {
                    acc.stmts.push_back(std::move(s));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
            if (!consumeKeyword("end")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'end' to close accessor block body");
            }
        } else {
            acc.bodyKind = ast::PropertyAccessorBodyKind::Expression;
            acc.exprBody = parseExpression();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after accessor body");
        }
    } else if (consumePunctuator(";")) {
        acc.bodyKind = ast::PropertyAccessorBodyKind::Abstract;
    } else {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '=>' or ';' in accessor body");
    }
    return acc;
}

// Slice 41: SerializedClause.

bool Parser::isSerializedClauseStart() const {
    return current_.kind == TokenKind::Identifier &&
           current_.text == "serialized";
}

std::unique_ptr<ast::SerializedClause> Parser::parseSerializedClause() {
    advance(); // past `serialized`
    auto sc = std::make_unique<ast::SerializedClause>();
    // `serialized skip` form. `skip` is contextual.
    if (current_.kind == TokenKind::Identifier && current_.text == "skip") {
        sc->isSkip = true;
        advance();
        return sc;
    }
    // SerializedName: `StringLiteral? "optional"?` with at least one of the
    // two present.
    bool gotSomething = false;
    if (current_.kind == TokenKind::StringLiteral) {
        // Strip the surrounding quotes if present. Escape decoding is sema's
        // job — for our purposes the raw lexeme is the source-of-truth.
        std::string_view txt = current_.text;
        if (txt.size() >= 2 && txt.front() == '"' && txt.back() == '"') {
            sc->name = txt.substr(1, txt.size() - 2);
        } else {
            sc->name = txt;
        }
        advance();
        gotSomething = true;
    }
    if (current_.kind == TokenKind::Identifier &&
        current_.text == "optional") {
        sc->isOptional = true;
        advance();
        gotSomething = true;
    }
    if (!gotSomething) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "`serialized` requires `skip`, a name string, or `optional` "
            "(A.4 SerializedClause)");
    }
    return sc;
}

std::unique_ptr<ast::SerializedClassClause>
Parser::parseSerializedClassClause() {
    // A.3 `serialized ("tree"|"graph"|"cyclic") PolymorphicClause?`. All
    // four words are contextual. Caller checked the leading "serialized"
    // and the shape word.
    advance(); // past `serialized`
    auto scc = std::make_unique<ast::SerializedClassClause>();
    if (current_.kind == TokenKind::Identifier &&
        (current_.text == "tree" || current_.text == "graph" ||
         current_.text == "cyclic")) {
        scc->shape = current_.text;
        advance();
    } else {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected `tree`, `graph`, or `cyclic` after `serialized`");
    }
    // PolymorphicClause: `polymorphic ( "name" : Type ("," "name" : Type)* )`.
    if (current_.kind == TokenKind::Identifier &&
        current_.text == "polymorphic") {
        advance();
        if (!consumePunctuator("(")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected '(' to begin PolymorphicClause registry");
        }
        auto parseOne = [&]() {
            ast::PolyCase pc;
            if (current_.kind != TokenKind::StringLiteral) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected discriminator string in PolyCase");
            } else {
                std::string_view txt = current_.text;
                if (txt.size() >= 2 && txt.front() == '"' &&
                    txt.back() == '"') {
                    pc.discriminator = txt.substr(1, txt.size() - 2);
                } else {
                    pc.discriminator = txt;
                }
                advance();
            }
            if (!consumePunctuator(":")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ':' between PolyCase discriminator and type");
            }
            pc.type = parseType();
            return pc;
        };
        scc->polyCases.push_back(parseOne());
        while (consumePunctuator(",")) {
            scc->polyCases.push_back(parseOne());
        }
        if (!consumePunctuator(")")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ')' to close PolymorphicClause registry");
        }
    }
    return scc;
}

namespace {

// Higher precedence = tighter binding. The numeric scale is internal — we
// just need an ordering consistent with §17.1's precedence table. Returns 0
// for tokens that aren't binary operators recognised yet (deferred ops:
// `in`/`is`/`as` keyword relationals, `is not` two-token relational,
// conditional `expr if cond else expr`, assignments — assignments live in
// AssignmentStmt at statement level).
int binaryPrecedence(const Token& tok) {
    if (tok.kind == TokenKind::Punctuator) {
        const std::string_view t = tok.text;
        if (t == "*" || t == "/")  return 14; // §17.1 L3 multiplicative
        if (t == "+" || t == "-")  return 13; // §17.1 L4 additive
        if (t == "<<" || t == ">>") return 12; // §17.1 L5 shift
        if (t == ".." || t == "..<") return 11; // §17.1 L6 range
        if (t == "&")              return 10; // §17.1 L7 bitwise AND
        if (t == "^")              return 9;  // §17.1 L8 bitwise XOR
        if (t == "|")              return 8;  // §17.1 L9 bitwise OR
        if (t == "<" || t == ">" || t == "<=" || t == ">=") return 7;
        if (t == "=" || t == "<>") return 6;  // §17.1 L11 equality
        if (t == "??")             return 5;  // §17.1 L12 null-coalescing
    }
    if (tok.kind == TokenKind::Keyword) {
        const std::string_view t = tok.text;
        if (t == "mod") return 14; // multiplicative (keyword-spelled)
        // §17.1 L10 relational, keyword-spelled. `is not` is also relational
        // and handled below as a two-token sequence starting at `is`.
        if (t == "in" || t == "is" || t == "as") return 7;
        if (t == "and") return 4;  // §17.1 L13 logical AND
        if (t == "or")  return 3;  // §17.1 L14 logical OR
    }
    return 0;
}

// `??` (and only `??` at slice-14 scope) is right-associative per §17.1's
// note on level 12.
bool isRightAssociative(std::string_view op) {
    return op == "??";
}

} // namespace

// Issue #135: span-stamping wrappers. Each captures the location of the
// first token of the construct, delegates to the *Impl that builds the
// node, then stamps `span = [start, current token)` if the node didn't
// already carry a (tighter) span from a child parse. Spans flow to every
// later phase's diagnostics, which previously all reported 0:0.
namespace {
template <typename NodePtr>
NodePtr stampSpan(NodePtr node, const SourceLocation& start,
                  const SourceLocation& end) {
    if (node && node->span.begin.line == 0) {
        node->span.begin = start;
        node->span.end = end;
    }
    return node;
}
} // namespace

std::unique_ptr<ast::Expr> Parser::parseExpression() {
    SourceLocation start = current_.location;
    return stampSpan(parseExpressionImpl(), start, current_.location);
}

std::unique_ptr<ast::Expr> Parser::parseExpressionImpl() {
    // §A.7 ConditionalExpr ::= LogicalOr ("if" Expression "else" Expression)?
    //                         | ThrowExpr | CaseExpr (later slice)
    if (isKeyword("throw")) {
        advance();
        auto te = std::make_unique<ast::ThrowExpr>();
        const bool atTerminator =
            (current_.kind == TokenKind::EndOfFile) ||
            (current_.kind == TokenKind::Punctuator &&
             (current_.text == ";" || current_.text == ")" ||
              current_.text == "," || current_.text == "]")) ||
            (current_.kind == TokenKind::Keyword && current_.text == "end");
        if (!atTerminator) {
            te->operand = parseExpression();
        }
        return std::unique_ptr<ast::Expr>(te.release());
    }
    auto left = parseBinaryExpression(1);
    // Conditional suffix. Now that statement constructs that contain an
    // Expression-followed-by-Statement (`if cond then …`, `while cond do …`,
    // `for var x in expr do …`) have body-start keywords, naive `if`-suffix
    // parsing here can't gobble a statement's `if` — it'd just fail to find
    // `else` and report a parse error.
    if (consumeKeyword("if")) {
        auto cond = parseExpression();
        if (!consumeKeyword("else")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'else' after conditional 'if' condition");
        }
        auto right = parseExpression();
        auto c = std::make_unique<ast::ConditionalExpr>();
        c->thenValue = std::move(left);
        c->condition = std::move(cond);
        c->elseValue = std::move(right);
        return std::unique_ptr<ast::Expr>(c.release());
    }
    return left;
}

std::unique_ptr<ast::Expr> Parser::parseBinaryExpression(int minPrecedence) {
    // Precedence climbing. Right-associative operators (`??`) recurse with the
    // same minPrec; left-associative ones recurse with prec + 1 so same-prec
    // ops associate to the left. The keyword relationals `is`, `is not`, and
    // `as` have a Type on the right-hand side and produce TypeTestExpr; `in`
    // takes a regular expression on the right.
    auto left = parseUnaryExpression();
    for (;;) {
        const int prec = binaryPrecedence(current_);
        if (prec < minPrecedence) break;
        const Token opTok = current_;
        advance();

        // §17.4: `is not` is one logical operator with a two-token spelling.
        // The lexer emits `is` and `not` as separate Keyword tokens; we glue
        // them back together here when they appear in relational position.
        std::string_view opText = opTok.text;
        if (opTok.text == "is" && isKeyword("not")) {
            advance();
            opText = "is not";
        }

        // §17.4: `is` / `is not` / `as` take a Type on the right. `in` takes
        // an expression (it's a regular binary op).
        const bool isTypeTest =
            (opText == "is" || opText == "is not" || opText == "as");
        if (isTypeTest) {
            auto tt = std::make_unique<ast::TypeTestExpr>();
            tt->operand = std::move(left);
            tt->op = opText;
            tt->testType = parseType();
            left = std::unique_ptr<ast::Expr>(tt.release());
        } else if (opText == ".." || opText == "..<") {
            // §A.7 RangeExpr. The right (Shift) is optional — `a..` is the
            // low-only form. An optional `step EXPR` may follow (contextual
            // keyword per App. B.2).
            auto re = std::make_unique<ast::RangeExpr>();
            re->inclusive = (opText == "..");
            re->lo = std::move(left);
            auto isStopTok = [&]() {
                if (current_.kind == TokenKind::EndOfFile) return true;
                if (current_.kind == TokenKind::Punctuator) {
                    return current_.text == ";" || current_.text == ")" ||
                           current_.text == "]" || current_.text == "}" ||
                           current_.text == "," || current_.text == "=>";
                }
                if (current_.kind == TokenKind::Keyword) {
                    return current_.text == "do" || current_.text == "with" ||
                           current_.text == "when" ||
                           current_.text == "end" ||
                           current_.text == "else" ||
                           current_.text == "elif";
                }
                if (current_.kind == TokenKind::Identifier &&
                    current_.text == "step") return true;
                return false;
            };
            if (!isStopTok()) {
                re->hi = parseBinaryExpression(prec + 1);
            }
            if (current_.kind == TokenKind::Identifier &&
                current_.text == "step") {
                advance();
                re->step = parseBinaryExpression(prec + 1);
            }
            left = std::unique_ptr<ast::Expr>(re.release());
        } else {
            const int nextMin =
                isRightAssociative(opText) ? prec : prec + 1;
            // §17.12: a `throw` expression has the bottom type and is valid
            // in *any* expression position — including a binary right
            // operand, most idiomatically `x ?? throw new E(...)` (spec
            // §A.7 example). The precedence climber does not descend to the
            // ThrowExpr level (A.7's grammar is informal; §17.12 governs),
            // so parse a leading `throw` as a full expression here.
            auto right = isKeyword("throw") ? parseExpressionImpl()
                                            : parseBinaryExpression(nextMin);
            auto bin = std::make_unique<ast::BinaryExpr>();
            bin->op = opText;
            bin->left = std::move(left);
            bin->right = std::move(right);
            left = std::unique_ptr<ast::Expr>(bin.release());
        }
    }
    return left;
}

std::unique_ptr<ast::Expr> Parser::parseUnaryExpression() {
    // §17.1 level 2 prefix unary: `+ - ~ not await`. Right-associative.
    if (isPunctuator("+") || isPunctuator("-") || isPunctuator("~") ||
        isKeyword("not") || isKeyword("await")) {
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
    SourceLocation start = current_.location;
    return stampSpan(parsePrimaryExpressionImpl(), start, current_.location);
}

std::unique_ptr<ast::Expr> Parser::parsePrimaryExpressionImpl() {
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
    if (current_.kind == TokenKind::InterpolatedString ||
        current_.kind == TokenKind::InterpolatedVerbatimString) {
        auto e = std::make_unique<ast::InterpolatedStringExpr>();
        e->isVerbatim =
            (current_.kind == TokenKind::InterpolatedVerbatimString);
        e->text = current_.text;
        // §18.5 / §18.7: split the `$"..."` token into alternating literal
        // and expression parts. The lexer already validated balanced braces.
        splitInterpolation(*e);
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }
    // §A.7 high-only and fully-open RangeExpr forms: `..b`, `..<b`, `..`.
    // (Low-only `a..` and the with-bound forms `a..b` / `a..<b` arrive
    // through parseBinaryExpression's range path.)
    if (isPunctuator("..") || isPunctuator("..<")) {
        auto re = std::make_unique<ast::RangeExpr>();
        re->inclusive = (current_.text == "..");
        advance();
        auto isStopTok = [&]() {
            if (current_.kind == TokenKind::EndOfFile) return true;
            if (current_.kind == TokenKind::Punctuator) {
                return current_.text == ";" || current_.text == ")" ||
                       current_.text == "]" || current_.text == "}" ||
                       current_.text == "," || current_.text == "=>";
            }
            if (current_.kind == TokenKind::Keyword) {
                return current_.text == "do" || current_.text == "with" ||
                       current_.text == "when" || current_.text == "end" ||
                       current_.text == "else" || current_.text == "elif";
            }
            if (current_.kind == TokenKind::Identifier &&
                current_.text == "step") return true;
            return false;
        };
        if (!isStopTok()) {
            // Use binary precedence 11+1 (one above range itself) so the
            // hi-bound parses as a Shift expression per A.7.
            re->hi = parseBinaryExpression(12);
        }
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "step") {
            advance();
            re->step = parseBinaryExpression(12);
        }
        return std::unique_ptr<ast::Expr>(re.release());
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

    // §A.7 typeof / nameof primary expressions.
    if (isKeyword("typeof")) {
        advance();
        auto te = std::make_unique<ast::TypeOfExpr>();
        if (!consumePunctuator("(")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '(' after 'typeof'");
        }
        te->queriedType = parseType();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close 'typeof'");
        }
        return std::unique_ptr<ast::Expr>(te.release());
    }
    if (isKeyword("nameof")) {
        advance();
        auto ne = std::make_unique<ast::NameOfExpr>();
        if (!consumePunctuator("(")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '(' after 'nameof'");
        }
        ne->name = parseQualifiedName();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close 'nameof'");
        }
        return std::unique_ptr<ast::Expr>(ne.release());
    }
    // §A.7 `DefaultExpr ::= "default" "(" Type ")"`.
    if (isKeyword("default")) {
        advance();
        auto de = std::make_unique<ast::DefaultExpr>();
        if (!consumePunctuator("(")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '(' after 'default'");
        }
        de->type = parseType();
        if (!consumePunctuator(")")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ')' to close 'default'");
        }
        return std::unique_ptr<ast::Expr>(de.release());
    }
    // §A.7 CollectionLiteral — `[ ... ]`. Two forms (sequence / dict)
    // discriminated by what follows the first element: `:` means dict,
    // `,`/`]` means sequence. An empty `[]` is the empty sequence form
    // (the empty dict has no syntactic shape and must be expressed via
    // `default(IDictionary<K,V>)` or a target-type-explicit factory call).
    if (consumePunctuator("[")) {
        auto cl = std::make_unique<ast::CollectionLiteralExpr>();
        if (consumePunctuator("]")) {
            return std::unique_ptr<ast::Expr>(cl.release());
        }
        auto first = parseExpression();
        if (consumePunctuator(":")) {
            cl->isDict = true;
            cl->keys.push_back(std::move(first));
            cl->elements.push_back(parseExpression());
            while (consumePunctuator(",")) {
                if (isPunctuator("]")) break; // trailing `,` permitted
                cl->keys.push_back(parseExpression());
                if (!consumePunctuator(":")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ':' in dictionary literal entry");
                }
                cl->elements.push_back(parseExpression());
            }
        } else {
            cl->elements.push_back(std::move(first));
            while (consumePunctuator(",")) {
                if (isPunctuator("]")) break;
                cl->elements.push_back(parseExpression());
            }
        }
        if (!consumePunctuator("]")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ']' to close collection literal");
        }
        return std::unique_ptr<ast::Expr>(cl.release());
    }

    // A.7 `MoveExpr ::= "move" PrimaryExpr` — primary-level prefix per §28.6
    // for owned-field move-out.
    if (isKeyword("move")) {
        advance();
        auto m = std::make_unique<ast::UnaryExpr>();
        m->op = "move";
        m->operand = parsePostfix(parsePrimaryExpression());
        return std::unique_ptr<ast::Expr>(m.release());
    }

    // §22.3 `this` and `base` — reserved keywords (App. B.1) that name the
    // current instance and the base-class projection. The AST stores them as
    // IdentifierExpr with the keyword as the lexeme; sema interprets.
    if (isKeyword("this") || isKeyword("base")) {
        auto e = std::make_unique<ast::IdentifierExpr>();
        e->name = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }

    // §5.2 explicit conversion `int(x)`, `double(m)`, … — a primitive type
    // keyword in expression position names a conversion applied with
    // call syntax. Emit it as an IdentifierExpr so parsePostfix forms the
    // `(arg)` call; sema/codegen interpret the call as a conversion. (This
    // also ensures a stray primitive keyword is consumed rather than left
    // to spin the enclosing statement-list parser.)
    if (current_.kind == TokenKind::Keyword &&
        isPrimitiveTypeKeyword(current_.text)) {
        auto e = std::make_unique<ast::IdentifierExpr>();
        e->name = current_.text;
        advance();
        return std::unique_ptr<ast::Expr>(e.release());
    }

    // §A.7 CaseExpr — `"case" Expression? CaseArm ("," CaseArm)* ","? "end"`.
    // The bare form (no scrutinee) is admitted by the grammar; slice 38
    // requires the scrutinee form (the bare-form lookahead requires deciding
    // "is the next token a pattern or an expression" without backtracking,
    // which lands later).
    if (isKeyword("case")) {
        return parseCaseExpr();
    }

    // §A.5 NewExpr — `new Type "(" ArgList? ")"` or shorthand `new "(" … ")"`.
    if (isKeyword("new")) {
        advance(); // past `new`
        auto n = std::make_unique<ast::NewExpr>();
        if (!isPunctuator("(")) {
            n->type = parseType();
        }
        if (!consumePunctuator("(")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected '(' to begin constructor argument list");
        } else {
            n->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close constructor argument list");
            }
        }
        return std::unique_ptr<ast::Expr>(n.release());
    }

    // Lambda: `async? "(" LambdaParams? ")" "=>" LambdaBody` (A.7). The
    // discriminator is "what follows the matching `)`?": `=>` means lambda,
    // anything else means parenthesised expression. We need to scan past the
    // matching `)` before deciding — that is the parser's one use of
    // unbounded buffered lookahead (peekAt).
    const bool asyncLambdaCandidate =
        isKeyword("async") &&
        peekAt(1).kind == TokenKind::Punctuator && peekAt(1).text == "(" &&
        tokensLookLikeLambda(1);
    const bool plainLambdaCandidate =
        isPunctuator("(") && tokensLookLikeLambda(0);

    if (asyncLambdaCandidate || plainLambdaCandidate) {
        auto lam = std::make_unique<ast::LambdaExpr>();
        if (asyncLambdaCandidate) {
            lam->isAsync = true;
            advance(); // past `async`
        }
        consumePunctuator("("); // confirmed by lookahead
        if (!consumePunctuator(")")) {
            // Non-empty LambdaParams.
            lam->params.push_back(parseLambdaParam());
            while (consumePunctuator(",")) {
                lam->params.push_back(parseLambdaParam());
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close lambda parameter list");
            }
        }
        if (!consumePunctuator("=>")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '=>' in lambda expression");
        }
        if (consumeKeyword("do")) {
            lam->isBlockBody = true;
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto s = parseStatement();
                if (s) {
                    lam->stmts.push_back(std::move(s));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
            if (!consumeKeyword("end")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'end' to close lambda block body");
            }
        } else {
            lam->exprBody = parseExpression();
        }
        return std::unique_ptr<ast::Expr>(lam.release());
    }

    if (consumePunctuator("(")) {
        // §A.7 unit value `()` — empty parens at PrimaryAtom level.
        if (consumePunctuator(")")) {
            return std::unique_ptr<ast::Expr>(
                std::make_unique<ast::UnitExpr>().release());
        }
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

bool Parser::tokensLookLikeLambda(std::size_t startK) {
    // peekAt(startK) must be `(`. Scan forward through balanced parens to
    // find the matching `)`, then return true iff `=>` follows. Brackets
    // `[]` and angles `<>` need not be balanced separately: they nest
    // *inside* the outer `(...)` regardless, and we only care about which
    // `)` matches the leading `(`. Bails out on EOF.
    std::size_t depth = 1;
    std::size_t k = startK + 1;
    for (;;) {
        const Token& t = peekAt(k);
        if (t.kind == TokenKind::EndOfFile) return false;
        if (t.kind == TokenKind::Punctuator) {
            if (t.text == "(") ++depth;
            else if (t.text == ")") {
                if (--depth == 0) {
                    const Token& after = peekAt(k + 1);
                    return after.kind == TokenKind::Punctuator &&
                           after.text == "=>";
                }
            }
        }
        ++k;
    }
}

ast::Param Parser::parseLambdaParam() {
    // A.7 `LambdaParam ::= FuncParamMods? Identifier (":" Type)?`. Type is
    // optional (unlike A.4 method ParamList).
    ast::Param p;
    if (consumeKeyword("ref")) {
        p.modifier = "ref";
    } else if (consumeKeyword("out")) {
        if (consumeKeyword("owned")) {
            p.modifier = "out owned";
        } else {
            p.modifier = "out";
        }
    } else if (consumeKeyword("owned")) {
        p.modifier = "owned";
    }
    if (current_.kind != TokenKind::Identifier) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected lambda parameter name");
    } else {
        p.name = current_.text;
        advance();
    }
    if (consumePunctuator(":")) {
        p.type = parseType();
    }
    return p;
}

// ===== Slice 38: patterns and case expressions (§A.7) =====================

std::unique_ptr<ast::Pattern> Parser::parsePattern() {
    // `_` discard. `_` is an Identifier per the lexer.
    if (current_.kind == TokenKind::Identifier && current_.text == "_") {
        advance();
        return std::make_unique<ast::DiscardPattern>();
    }
    // `null` literal pattern.
    if (consumeKeyword("null")) {
        return std::make_unique<ast::NullPattern>();
    }
    // `var IDENT (: TYPE)?` binding pattern.
    if (consumeKeyword("var")) {
        auto bp = std::make_unique<ast::BindingPattern>();
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected identifier in `var` binding pattern");
        } else {
            bp->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) bp->type = parseType();
        return std::unique_ptr<ast::Pattern>(bp.release());
    }
    // Relational pattern: `<`, `>`, `<=`, `>=` followed by Expression.
    if (isPunctuator("<") || isPunctuator(">") ||
        isPunctuator("<=") || isPunctuator(">=")) {
        auto rop = std::make_unique<ast::RelOpPattern>();
        rop->op = current_.text;
        advance();
        rop->expr = parseExpression();
        return std::unique_ptr<ast::Pattern>(rop.release());
    }
    // Open-ended range patterns (`..`, `..<EXPR`, `..EXPR`) are not in this
    // slice. The with-lower-bound forms (`a..b`, `a..<b`) are detected in
    // the catch-all below.
    if (isPunctuator("..") || isPunctuator("..<")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "open-ended range patterns are not yet supported "
            "(only `a..b` / `a..<b` for now)");
        advance();
        return nullptr;
    }
    // Primitive-type-led TypePattern. Other types (named) fall through to
    // the Identifier branch as NamedPattern — sema disambiguates Constant
    // vs Type by name resolution per the §A.7 note.
    if (isKeyword("int")     || isKeyword("long")  ||
        isKeyword("short")   || isKeyword("byte")  ||
        isKeyword("sbyte")   || isKeyword("ushort")||
        isKeyword("uint")    || isKeyword("ulong") ||
        isKeyword("float")   || isKeyword("double")||
        isKeyword("decimal") || isKeyword("bool")  ||
        isKeyword("char")    || isKeyword("string")) {
        auto tp = std::make_unique<ast::TypePattern>();
        tp->type = parseType();
        return std::unique_ptr<ast::Pattern>(tp.release());
    }
    // Identifier → QualifiedName. `(` after means DestructuringPattern.
    // A `,` followed by another Constant means a multi-Constant alternatives
    // pattern (slice 44). Otherwise it's a NamedPattern (sema resolves
    // Constant-vs-Type-test).
    if (current_.kind == TokenKind::Identifier) {
        auto qn = parseQualifiedName();
        if (consumePunctuator("(")) {
            auto dp = std::make_unique<ast::DestructuringPattern>();
            dp->typeName = std::move(qn);
            if (!isPunctuator(")")) {
                dp->subPatterns.push_back(parsePositionalPattern());
                while (consumePunctuator(",")) {
                    dp->subPatterns.push_back(parsePositionalPattern());
                }
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close destructuring pattern");
            }
            return std::unique_ptr<ast::Pattern>(dp.release());
        }
        if (isPunctuator(",") && isConstantStart(peekAt(1))) {
            // Convert qn → IdentifierExpr/MemberAccess chain and continue
            // as ConstantPattern alternatives.
            auto cp = std::make_unique<ast::ConstantPattern>();
            cp->constants.push_back(qualifiedNameToExpr(std::move(qn)));
            while (consumePunctuator(",")) {
                cp->constants.push_back(parseConstantExpr());
                if (!(isPunctuator(",") && isConstantStart(peekAt(1)))) break;
            }
            return std::unique_ptr<ast::Pattern>(cp.release());
        }
        auto np = std::make_unique<ast::NamedPattern>();
        np->name = std::move(qn);
        return std::unique_ptr<ast::Pattern>(np.release());
    }
    // Catch-all: parse an Expression and classify. A RangeExpr at the root
    // is a RangePattern (slice 52 — replaces the earlier BinaryExpr-based
    // representation); otherwise wrap as a ConstantPattern with one or
    // more `,`-separated constants (slice 44).
    auto e = parseExpression();
    if (dynamic_cast<ast::RangeExpr*>(e.get())) {
        auto rp = std::make_unique<ast::RangePattern>();
        rp->rangeExpr = std::move(e);
        return std::unique_ptr<ast::Pattern>(rp.release());
    }
    auto cp = std::make_unique<ast::ConstantPattern>();
    cp->constants.push_back(std::move(e));
    while (isPunctuator(",") && isConstantStart(peekAt(1))) {
        advance(); // past `,`
        cp->constants.push_back(parseConstantExpr());
    }
    return std::unique_ptr<ast::Pattern>(cp.release());
}

ast::PositionalPattern Parser::parsePositionalPattern() {
    ast::PositionalPattern pp;
    // A.7 PositionalPattern: a Pattern OR a naked binding Identifier (valid
    // only inside destructuring position). Naked binding is detected as a
    // single Identifier (not `_`) followed immediately by `,` or `)`. Any
    // other shape (Identifier . ..., Identifier ( ..., literal, `var`, etc.)
    // takes the Pattern path.
    if (current_.kind == TokenKind::Identifier &&
        current_.text != "_") {
        const Token& nxt = peekAt(1);
        if (nxt.kind == TokenKind::Punctuator &&
            (nxt.text == "," || nxt.text == ")")) {
            pp.nakedBinding = current_.text;
            advance();
            return pp;
        }
    }
    pp.pattern = parsePattern();
    return pp;
}

ast::CaseArm Parser::parseCaseArm() {
    ast::CaseArm arm;
    arm.pattern = parsePattern();
    if (consumeKeyword("when")) {
        arm.guard = parseExpression();
    }
    if (!consumePunctuator("=>")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '=>' after case-arm pattern");
    }
    // Slice 38: only the Expression ArmBody form. Statement-form ArmBody is
    // valid only in statement-position case (also deferred); the `do … end`
    // ArmBody form lands with the statement-position case slice.
    arm.bodyKind = ast::CaseArmBodyKind::Expression;
    arm.exprBody = parseExpression();
    return arm;
}

std::unique_ptr<ast::Expr> Parser::parseCaseExpr() {
    advance(); // past `case`
    auto ce = std::make_unique<ast::CaseExpr>();
    // Slice 38 requires the scrutinee form. The bare-`case` form
    // (`case CaseArm* end`, no scrutinee) is deferred.
    ce->scrutinee = parseExpression();
    ce->arms.push_back(parseCaseArm());
    while (consumePunctuator(",")) {
        // Trailing `","?` per the grammar — `,` followed immediately by
        // `end` terminates the list.
        if (isKeyword("end")) break;
        ce->arms.push_back(parseCaseArm());
    }
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end' to close case expression");
    }
    return std::unique_ptr<ast::Expr>(ce.release());
}

// Slice 44: helpers for ConstantPattern alternatives.

bool Parser::isConstantStart(const Token& t) const {
    if (t.kind == TokenKind::IntLiteral ||
        t.kind == TokenKind::FloatLiteral ||
        t.kind == TokenKind::CharLiteral ||
        t.kind == TokenKind::StringLiteral ||
        t.kind == TokenKind::VerbatimStringLiteral) return true;
    if (t.kind == TokenKind::Keyword &&
        (t.text == "true" || t.text == "false" || t.text == "null")) {
        return true;
    }
    // Identifier — could begin a QualifiedName Constant — but `_` is the
    // DiscardPattern, not a Constant.
    return t.kind == TokenKind::Identifier && t.text != "_";
}

std::unique_ptr<ast::Expr> Parser::parseConstantExpr() {
    // A.8 Constant. Literals go through parsePrimaryExpression; QualifiedName
    // is rebuilt as the equivalent IdentifierExpr / MemberAccessExpr chain.
    // Note: `null` is a keyword the lexer emits as TokenKind::Keyword, and
    // parsePrimaryExpression handles it as NullLiteral.
    if (current_.kind == TokenKind::Identifier) {
        auto qn = parseQualifiedName();
        return qualifiedNameToExpr(std::move(qn));
    }
    return parsePrimaryExpression();
}

std::unique_ptr<ast::Expr> Parser::qualifiedNameToExpr(
    std::unique_ptr<ast::QualifiedName> qn) {
    if (!qn || qn->parts.empty()) return nullptr;
    auto first = std::make_unique<ast::IdentifierExpr>();
    first->name = qn->parts[0];
    std::unique_ptr<ast::Expr> cur(first.release());
    for (std::size_t i = 1; i < qn->parts.size(); ++i) {
        auto ma = std::make_unique<ast::MemberAccessExpr>();
        ma->target = std::move(cur);
        ma->member = qn->parts[i];
        cur = std::unique_ptr<ast::Expr>(ma.release());
    }
    return cur;
}

// Slice 39: statement-position `case`.

ast::CaseArm Parser::parseCaseStmtArm() {
    ast::CaseArm arm;
    arm.pattern = parsePattern();
    if (consumeKeyword("when")) {
        arm.guard = parseExpression();
    }
    if (!consumePunctuator("=>")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '=>' after case-arm pattern");
    }
    // ArmBody in statement position: `do … end` block OR a single Statement
    // (whose terminating `;` ends the arm body per the §A.7 ArmBody note).
    arm.bodyKind = ast::CaseArmBodyKind::Block;
    if (consumeKeyword("do")) {
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                arm.stmts.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'end' to close 'do' arm body");
        }
    } else {
        // Single Statement. The Statement consumes its own `;`. After
        // returning, the outer loop expects `,` or `end`.
        auto s = parseStatement();
        if (s) arm.stmts.push_back(std::move(s));
    }
    return arm;
}

std::unique_ptr<ast::Stmt> Parser::parseCaseStmt() {
    advance(); // past `case`
    auto cs = std::make_unique<ast::CaseStmt>();
    cs->scrutinee = parseExpression();
    cs->arms.push_back(parseCaseStmtArm());
    while (consumePunctuator(",")) {
        if (isKeyword("end")) break;
        cs->arms.push_back(parseCaseStmtArm());
    }
    if (!consumeKeyword("end")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected 'end' to close case statement");
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end' of case statement");
    }
    return std::unique_ptr<ast::Stmt>(cs.release());
}

std::unique_ptr<ast::Expr> Parser::parsePostfix(std::unique_ptr<ast::Expr> head) {
    // §A.5 / §17.1 level 1 postfix chain: `.member`, `(args)`, `?.member`,
    // `?(args)`, `[indices]`, `!` (postfix null-forgiving). All bind to the
    // expression on the left.
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
        } else if (consumePunctuator("?.")) {
            if (current_.kind != TokenKind::Identifier) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected member name after '?.'");
                break;
            }
            auto a = std::make_unique<ast::NullCondMemberExpr>();
            a->target = std::move(head);
            a->member = current_.text;
            advance();
            head = std::unique_ptr<ast::Expr>(a.release());
        } else if (consumePunctuator("(")) {
            auto call = std::make_unique<ast::CallExpr>();
            call->callee = std::move(head);
            call->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected ')' to close call argument list");
            }
            head = std::unique_ptr<ast::Expr>(call.release());
        } else if (consumePunctuator("?(")) {
            auto call = std::make_unique<ast::NullCondCallExpr>();
            call->callee = std::move(head);
            call->args = parseArgList();
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close '?(' argument list");
            }
            head = std::unique_ptr<ast::Expr>(call.release());
        } else if (consumePunctuator("[")) {
            auto idx = std::make_unique<ast::IndexExpr>();
            idx->target = std::move(head);
            // §A.7 PrimarySuffix `[ ArgList ]` — share the call-arg grammar
            // (slice 55). parseArgList stops on `)`; here we drive it until
            // `]` instead by inlining the empty-check.
            if (!isPunctuator("]")) {
                idx->indices = parseArgList();
            }
            if (!consumePunctuator("]")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected ']' to close index list");
            }
            head = std::unique_ptr<ast::Expr>(idx.release());
        } else if (consumePunctuator("!")) {
            auto bang = std::make_unique<ast::PostfixBangExpr>();
            bang->operand = std::move(head);
            head = std::unique_ptr<ast::Expr>(bang.release());
        } else if (!disableWithSuffix_ && consumeKeyword("with")) {
            // §A.7 `with` PrimarySuffix:
            //   "with" InitAssign ("," InitAssign)* ","? "end"
            //   InitAssign ::= Identifier ":=" Expression
            // Source-type determines the semantic (record copy-with-overrides
            // vs object initializer on `new C(...)`); sema disambiguates.
            auto w = std::make_unique<ast::WithExpr>();
            w->source = std::move(head);
            auto parseInitAssign = [&]() -> ast::WithExpr::InitAssign {
                ast::WithExpr::InitAssign ia;
                if (current_.kind != TokenKind::Identifier) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected identifier in `with` init-assignment");
                } else {
                    ia.name = current_.text;
                    advance();
                }
                if (!consumePunctuator(":=")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ':=' in `with` init-assignment");
                }
                ia.value = parseExpression();
                return ia;
            };
            w->assigns.push_back(parseInitAssign());
            while (consumePunctuator(",")) {
                if (isKeyword("end")) break; // trailing `,?`
                w->assigns.push_back(parseInitAssign());
            }
            if (!consumeKeyword("end")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'end' to close `with` initializer block");
            }
            head = std::unique_ptr<ast::Expr>(w.release());
        } else {
            break;
        }
    }
    return head;
}

std::unique_ptr<ast::MethodDecl> Parser::parseMethodDeclBody(
    ast::Visibility visibility, std::vector<std::string_view> modifiers) {
    // Caller has already consumed the `method` keyword. We pick up at the
    // method name.
    auto m = std::make_unique<ast::MethodDecl>();
    m->visibility = visibility;
    m->modifiers = std::move(modifiers);

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

    m->genericConstraints = parseGenericConstraints();

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
    SourceLocation start = current_.location;
    return stampSpan(parseStatementImpl(), start, current_.location);
}

std::unique_ptr<ast::Stmt> Parser::parseStatementImpl() {
    // §A.6 Statement dispatch. Slice 11 covers LocalDecl, ReturnStmt, and
    // the expression-led shapes (AssignmentStmt vs ExpressionStmt, decided
    // after parsing the leading expression).

    // A.6 LocalConstDecl (slice 53):
    //   "const" Identifier (":" Type)? ":=" ConstExpr ";"
    if (isKeyword("const")) {
        advance(); // past `const`
        auto lc = std::make_unique<ast::LocalConstDecl>();
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected name in local const declaration");
        } else {
            lc->name = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            lc->type = parseType();
        }
        if (!consumePunctuator(":=")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "local const requires an initializer ':=' (A.6 LocalConstDecl)");
        }
        lc->value = parseExpression();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after local const declaration");
        }
        return std::unique_ptr<ast::Stmt>(lc.release());
    }

    // LocalDecl (A.6): `var Identifier (":" Type)? (":=" Expression)? ";"`,
    // `using var ...`, or the async `await using var ...` form. The async
    // form is detected by one-token lookahead on the leading `await` —
    // without it, `await` would otherwise begin a unary-expression
    // ExpressionStmt.
    const bool awaitUsing =
        isKeyword("await") &&
        peekNext().kind == TokenKind::Keyword &&
        peekNext().text == "using";
    if (isKeyword("var") || isKeyword("using") || awaitUsing) {
        auto decl = std::make_unique<ast::LocalDecl>();
        if (awaitUsing) {
            decl->isUsing = true;
            decl->isAwaitUsing = true;
            advance(); // past `await`
            consumeKeyword("using"); // confirmed by lookahead
            if (!consumeKeyword("var")) {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected 'var' after 'await using'");
            }
        } else if (consumeKeyword("using")) {
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
                decl->isAwaitUsing
                    ? "`await using var` requires an initializer "
                      "(A.6 LocalDecl)"
                    : "`using var` requires an initializer (A.6 LocalDecl)");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after local declaration");
        }
        return std::unique_ptr<ast::Stmt>(decl.release());
    }

    // CaseStmt (A.6). Statement-position `case` admits Statement-form and
    // `do … end` ArmBodies, unlike the Expression-position CaseExpr. We
    // intercept before the expression fallthrough so the `;` terminator and
    // the richer body forms work correctly.
    if (isKeyword("case")) {
        return parseCaseStmt();
    }

    // IfStmt (A.6).
    if (isKeyword("if")) {
        advance(); // past `if`
        auto stmt = std::make_unique<ast::IfStmt>();
        stmt->ifBranch.condition = parseExpression();
        if (!consumeKeyword("then")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'then' after 'if' condition");
        }
        while (!isKeyword("elif") && !isKeyword("else") &&
               !isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->ifBranch.body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        while (consumeKeyword("elif")) {
            ast::IfStmt::Branch elif;
            elif.condition = parseExpression();
            if (!consumeKeyword("then")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'then' after 'elif' condition");
            }
            while (!isKeyword("elif") && !isKeyword("else") &&
                   !isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto inner = parseStatement();
                if (inner) {
                    elif.body.push_back(std::move(inner));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
            stmt->elifBranches.push_back(std::move(elif));
        }
        if (consumeKeyword("else")) {
            stmt->hasElse = true;
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto inner = parseStatement();
                if (inner) {
                    stmt->elseBody.push_back(std::move(inner));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'if' statement");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'if'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // WhileStmt (A.6): `"while" Expression "do" Statement* "end" ";"`.
    if (isKeyword("while")) {
        advance();
        auto stmt = std::make_unique<ast::WhileStmt>();
        stmt->condition = parseExpression();
        if (!consumeKeyword("do")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'do' to begin 'while' body");
        }
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'while' statement");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'while'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // TryStmt (A.6). Slice 22 covers the binding catch arm only —
    // `catch var Identifier : Type => Statement` — plus optional finally.
    if (isKeyword("try")) {
        advance(); // past `try`
        auto stmt = std::make_unique<ast::TryStmt>();
        while (!isKeyword("catch") && !isKeyword("finally") &&
               !isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                stmt->body.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        while (consumeKeyword("catch")) {
            // A.6 CatchPattern: `Type ("," Type)*` OR
            //   `"var" Identifier ":" Type ("," Type)*`.
            // Also accept `Identifier ":" Type` (no `var`) as an alias for
            // the binding form — a common C#/Java idiom — with a recovery
            // diagnostic. This prevents an infinite-loop parse hang if the
            // user forgets `var`.
            ast::CatchArm arm;
            if (consumeKeyword("var")) {
                arm.hasBinding = true;
                if (current_.kind != TokenKind::Identifier) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected exception-binding name");
                } else {
                    arm.varName = current_.text;
                    advance();
                }
                if (!consumePunctuator(":")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ':' after catch binding name");
                }
            } else if (current_.kind == TokenKind::Identifier &&
                       peekAt(1).kind == TokenKind::Punctuator &&
                       peekAt(1).text == ":") {
                // `catch name: Type` — silently treat as `catch var name: Type`
                arm.hasBinding = true;
                arm.varName = current_.text;
                advance(); // past name
                advance(); // past ":"
            }
            arm.types.push_back(parseType());
            while (consumePunctuator(",")) {
                // `,` here joins additional Types in the catch pattern. The
                // top-level CatchBlock arm-separator `,` only appears AFTER
                // the body — at this point we're still inside the pattern.
                arm.types.push_back(parseType());
            }
            if (consumeKeyword("when")) {
                arm.guard = parseExpression();
            }
            if (!consumePunctuator("=>")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected '=>' after catch pattern");
            }
            arm.body = parseStatement();
            stmt->catches.push_back(std::move(arm));
            // Optional trailing comma between arms; consumed here so the
            // next catch keyword (or `finally`/`end`) is current.
            (void)consumePunctuator(",");
        }
        if (consumeKeyword("finally")) {
            stmt->hasFinally = true;
            while (!isKeyword("end") &&
                   current_.kind != TokenKind::EndOfFile) {
                auto s = parseStatement();
                if (s) {
                    stmt->finallyBody.push_back(std::move(s));
                } else if (current_.kind != TokenKind::EndOfFile) {
                    advance();
                }
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'try' statement");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'try'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // ForStmt (A.6). Synchronous `for var ... in EXPR do ... end ;` and the
    // async `await for var ... in EXPR (with EXPR)? do ... end ;` form
    // (slice 35). The async form is detected by one-token lookahead on the
    // leading `await`.
    const bool awaitFor =
        isKeyword("await") &&
        peekNext().kind == TokenKind::Keyword &&
        peekNext().text == "for";
    if (isKeyword("for") || awaitFor) {
        auto stmt = std::make_unique<ast::ForStmt>();
        if (awaitFor) {
            stmt->isAwait = true;
            advance(); // past `await`
        }
        advance(); // past `for`
        if (!consumeKeyword("var")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'var' after 'for'");
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected loop variable name");
        } else {
            stmt->varName = current_.text;
            advance();
        }
        if (consumePunctuator(":")) {
            stmt->varType = parseType();
        }
        if (!consumeKeyword("in")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'in' in for statement");
        }
        // Disable the postfix `with` suffix here so the for-stmt sees its
        // own `with EXPR` cancellation-token clause rather than letting the
        // expression parser consume it as a WithExpr (slice 50).
        const bool prevDisable = disableWithSuffix_;
        disableWithSuffix_ = true;
        stmt->iterable = parseExpression();
        disableWithSuffix_ = prevDisable;
        // Optional `with EXPR` cancellation-token clause — async form only
        // (§30.7). Sema rejects it on the sync form.
        if (consumeKeyword("with")) {
            if (!stmt->isAwait) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "'with' clause is only valid on 'await for' (§30.7)");
            }
            stmt->withExpr = parseExpression();
        }
        if (!consumeKeyword("do")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'do' to begin 'for' body");
        }
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'for' statement");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'for'");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    // DoEndBlock (A.6) — plain statement-sequencing block, no value.
    if (isKeyword("do")) {
        advance();
        auto stmt = std::make_unique<ast::DoEndBlock>();
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto inner = parseStatement();
            if (inner) {
                stmt->body.push_back(std::move(inner));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close 'do' block");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of 'do' block");
        }
        return std::unique_ptr<ast::Stmt>(stmt.release());
    }

    if (consumeKeyword("break")) {
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'break'");
        }
        return std::unique_ptr<ast::Stmt>(
            std::make_unique<ast::BreakStmt>().release());
    }
    if (consumeKeyword("continue")) {
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'continue'");
        }
        return std::unique_ptr<ast::Stmt>(
            std::make_unique<ast::ContinueStmt>().release());
    }

    // YieldStmt (A.6): `yield return Expression ";"` or `yield break ";"`.
    if (isKeyword("yield")) {
        advance();
        auto y = std::make_unique<ast::YieldStmt>();
        if (consumeKeyword("break")) {
            y->isBreak = true;
        } else if (consumeKeyword("return")) {
            y->value = parseExpression();
        } else {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected 'return' or 'break' after 'yield'");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after yield statement");
        }
        return std::unique_ptr<ast::Stmt>(y.release());
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

    // ThrowStmt (A.6): `throw Expression? ";"`.
    if (isKeyword("throw")) {
        advance();
        auto th = std::make_unique<ast::ThrowStmt>();
        if (!isPunctuator(";")) {
            th->value = parseExpression();
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after throw statement");
        }
        return std::unique_ptr<ast::Stmt>(th.release());
    }

    // Otherwise parse an expression and decide between IncrementStmt (`++`/
    // `--`), DiscardStmt (LHS is identifier `_` and the op is `:=`),
    // AssignmentStmt (other assignment ops), and ExpressionStmt (fallback).
    auto lhs = parseExpression();

    if (current_.kind == TokenKind::Punctuator &&
        (current_.text == "++" || current_.text == "--")) {
        auto inc = std::make_unique<ast::IncrementStmt>();
        inc->target = std::move(lhs);
        inc->op = current_.text;
        advance();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after increment/decrement statement");
        }
        return std::unique_ptr<ast::Stmt>(inc.release());
    }

    if (current_.kind == TokenKind::Punctuator &&
        (current_.text == ":="  || current_.text == "+="  ||
         current_.text == "-="  || current_.text == "*="  ||
         current_.text == "/="  || current_.text == "&="  ||
         current_.text == "|="  || current_.text == "^="  ||
         current_.text == "<<=" || current_.text == ">>=" ||
         current_.text == "??=")) {
        // Discard form: LHS is the bare `_` identifier and the op is `:=`.
        if (current_.text == ":=") {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(lhs.get())) {
                if (id->name == "_") {
                    advance(); // past `:=`
                    auto d = std::make_unique<ast::DiscardStmt>();
                    d->value = parseExpression();
                    if (!consumePunctuator(";")) {
                        diagnostics_.report(
                            Severity::Error, current_.location,
                            "expected ';' after discard statement");
                    }
                    return std::unique_ptr<ast::Stmt>(d.release());
                }
            }
        }
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

std::unique_ptr<ast::Member> Parser::parseOperatorDeclBody(
    ast::Visibility visibility, std::vector<std::string_view> modifiers) {
    // A.4 `OperatorDecl ::= Visibility "static" "method" "operator" OpToken
    //                        "(" ParamList ")" ":" "owned"? Type
    //                        OperatorBody`. Caller has consumed
    // `method operator`.
    auto op = std::make_unique<ast::OperatorDecl>();
    op->visibility = visibility;
    (void)modifiers; // `static` is grammar-required for operators; sema checks.
    // OpToken: whatever punctuator (or keyword like `mod`) the operator is.
    op->opToken = current_.text;
    advance();
    if (!consumePunctuator("(")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected '(' to begin operator parameter list");
    }
    op->params = parseParamList();
    if (!consumePunctuator(")")) {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected ')' to close operator parameter list");
    }
    if (!consumePunctuator(":")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "operator overload requires a return type");
    }
    if (isKeyword("owned")) {
        op->returnOwned = true;
        advance();
    }
    op->returnType = parseType();
    // OperatorBody mirrors MethodBody (Expression / Block / Abstract).
    if (consumePunctuator("=>")) {
        op->bodyKind = ast::MethodBodyKind::Expression;
        op->exprBody = parseExpression();
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after expression-body operator");
        }
    } else if (consumePunctuator(";")) {
        op->bodyKind = ast::MethodBodyKind::Abstract;
    } else {
        op->bodyKind = ast::MethodBodyKind::Block;
        while (!isKeyword("end") &&
               current_.kind != TokenKind::EndOfFile) {
            auto s = parseStatement();
            if (s) {
                op->stmts.push_back(std::move(s));
            } else if (current_.kind != TokenKind::EndOfFile) {
                advance();
            }
        }
        if (!consumeKeyword("end")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected 'end' to close operator body");
        }
        // §1.6 keyword-labelled construct: `end operator OpToken?`.
        if (isKeyword("operator")) {
            advance();
            // The OpToken itself may also follow.
            if (current_.kind == TokenKind::Punctuator) {
                advance();
            }
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end' of operator");
        }
    }
    return std::unique_ptr<ast::Member>(op.release());
}

std::vector<ast::Param> Parser::parseParamList() {
    // A.4 `Param ::= ParamModifiers? Identifier ":" Type (":=" Expression)?`
    // ParamModifiers ∈ { ref | out owned? | owned }.
    std::vector<ast::Param> params;
    if (isPunctuator(")")) return params;

    auto parseOne = [&]() -> ast::Param {
        ast::Param p;
        if (consumeKeyword("ref")) {
            p.modifier = "ref";
        } else if (consumeKeyword("out")) {
            if (consumeKeyword("owned")) {
                p.modifier = "out owned";
            } else {
                p.modifier = "out";
            }
        } else if (consumeKeyword("owned")) {
            p.modifier = "owned";
        }
        if (current_.kind != TokenKind::Identifier) {
            // Recovery: if a keyword was used as a parameter name, accept
            // it (with a diagnostic) and advance. This prevents an infinite
            // loop in error recovery. Note: `base`, `this`, and other
            // keywords are not legal parameter names per Appendix B.1.
            if (current_.kind == TokenKind::Keyword) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    std::string("reserved keyword `") +
                    std::string(current_.text) +
                    "` cannot be used as a parameter name");
                p.name = current_.text;
                advance();
            } else {
                diagnostics_.report(Severity::Error, current_.location,
                                    "expected parameter name");
            }
        } else {
            p.name = current_.text;
            advance();
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ':' after parameter name");
        }
        p.type = parseType();
        if (consumePunctuator(":=")) {
            p.defaultValue = parseExpression();
        }
        return p;
    };

    params.push_back(parseOne());
    while (consumePunctuator(",")) {
        params.push_back(parseOne());
    }
    return params;
}

std::vector<ast::CallArgument> Parser::parseArgList() {
    // A.4 ArgList ::= Argument ("," Argument)*
    // Argument    ::= ArgModifiers? (Identifier ":=")? ArgExpression
    // ArgModifiers ∈ { ref | out var? | out using var | out owned | owned }
    // ArgExpression ::= Expression | "_"
    std::vector<ast::CallArgument> args;
    if (isPunctuator(")")) return args;
    auto parseOne = [&]() -> ast::CallArgument {
        ast::CallArgument a;
        if (consumeKeyword("ref")) {
            a.modifier = "ref";
        } else if (consumeKeyword("out")) {
            if (consumeKeyword("owned")) {
                a.modifier = "out owned";
            } else if (consumeKeyword("using")) {
                if (!consumeKeyword("var")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected 'var' after 'out using'");
                }
                a.modifier = "out using var";
            } else if (consumeKeyword("var")) {
                a.modifier = "out var";
            } else {
                a.modifier = "out";
            }
        } else if (consumeKeyword("owned")) {
            a.modifier = "owned";
        }
        // Named-arg prefix `Identifier :=`. peekAt(1) distinguishes from
        // a bare-Identifier expression argument.
        if (current_.kind == TokenKind::Identifier &&
            peekAt(1).kind == TokenKind::Punctuator &&
            peekAt(1).text == ":=") {
            a.name = current_.text;
            advance(); // identifier
            advance(); // :=
        }
        // ArgExpression — `_` discard or full Expression. `_` is detected
        // as a lone Identifier followed by `,` or `)`.
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "_" &&
            peekAt(1).kind == TokenKind::Punctuator &&
            (peekAt(1).text == "," || peekAt(1).text == ")")) {
            a.isDiscard = true;
            advance();
        } else {
            a.value = parseExpression();
        }
        return a;
    };
    args.push_back(parseOne());
    while (consumePunctuator(",")) {
        args.push_back(parseOne());
    }
    return args;
}

std::vector<ast::Param> Parser::parsePositionalFieldList() {
    // A.3 PositionalField ::= Identifier ":" Type SerializedClause?
    // No ParamModifiers and no default value (those belong to ParamList /
    // PrimaryCtorParams).
    std::vector<ast::Param> fields;
    if (isPunctuator(")")) return fields;
    auto parseOne = [&]() -> ast::Param {
        ast::Param p;
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected positional field name");
        } else {
            p.name = current_.text;
            advance();
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ':' after positional field name");
        }
        p.type = parseType();
        if (isSerializedClauseStart()) {
            p.serialized = parseSerializedClause();
        }
        return p;
    };
    fields.push_back(parseOne());
    while (consumePunctuator(",")) fields.push_back(parseOne());
    return fields;
}

std::vector<ast::GenericConstraint> Parser::parseGenericConstraints() {
    // A.3 GenericConstraints two forms:
    //   `where T : Constraint ("," Constraint)*`
    //   `where T is ModifierConstraint`    /* slice 54 — §14.4 */
    //   ModifierConstraint ::= "owned" | "not" "owned"
    std::vector<ast::GenericConstraint> all;
    while (isKeyword("where")) {
        advance(); // past `where`
        ast::GenericConstraint clause;
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected type-parameter name after 'where'");
        } else {
            clause.typeParam = current_.text;
            advance();
        }
        // Modifier-constraint form: `is owned` / `is not owned`.
        if (consumeKeyword("is")) {
            const bool isNot = consumeKeyword("not");
            if (consumeKeyword("owned")) {
                clause.modifierConstraint = isNot ? "not owned" : "owned";
            } else {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'owned' in ModifierConstraint "
                    "(only `is owned` / `is not owned` are supported)");
            }
            all.push_back(std::move(clause));
            continue;
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ':' or 'is' in generic constraint");
        }

        auto parseConstraintItem = [&]() -> ast::ConstraintItem {
            ast::ConstraintItem item;
            // A.3 Constraint ::= "class" | "value" | "notnull" | "new" "(" ")"
            //                  | Type.
            // `class` is reserved; `value` and `notnull` are contextual
            // (App. B.2) so the lexer produces Identifier tokens.
            if (consumeKeyword("class")) {
                item.kind = "class";
            } else if (current_.kind == TokenKind::Identifier &&
                       (current_.text == "value" ||
                        current_.text == "notnull")) {
                item.kind = current_.text;
                advance();
            } else if (consumeKeyword("new")) {
                item.kind = "new";
                if (!consumePunctuator("(") ||
                    !consumePunctuator(")")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected '()' after 'new' constraint");
                }
            } else {
                item.kind = "type";
                item.typeArg = parseType();
            }
            return item;
        };

        clause.items.push_back(parseConstraintItem());
        while (consumePunctuator(",")) {
            clause.items.push_back(parseConstraintItem());
        }
        all.push_back(std::move(clause));
    }
    return all;
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
