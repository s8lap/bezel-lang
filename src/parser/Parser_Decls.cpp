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
        // §14: `end TypeName<T>;` — skip generic params in the end label so
        // that generic class declarations can label their end consistently.
        if (isPunctuator("<")) {
            advance();  // skip `<`
            int depth = 1;
            while (depth > 0 && current_.kind != TokenKind::EndOfFile) {
                if (isPunctuator("<")) depth++;
                else if (isPunctuator(">")) { depth--; if (depth == 0) break; }
                advance();
            }
            if (isPunctuator(">")) advance();  // consume final `>`
        }
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
        // A.4 IndexerDecl: `[ParamList]` or `this [ParamList]` in place of
        // the Identifier name. The `this` form is the spec's canonical
        // indexer syntax (§22.10): `property this[params]: T`.
        if (isKeyword("this") && peekAt(1).kind == TokenKind::Punctuator &&
            peekAt(1).text == "[") {
            advance();  // past `this`
        }
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
        // `operator` is a contextual keyword (App. B.2) — lexed as Identifier.
        if ((isKeyword("operator") ||
             (current_.kind == TokenKind::Identifier &&
              current_.text == "operator"))) {
            advance();
            // The OpToken itself may also follow.
            if (current_.kind == TokenKind::Punctuator) {
                advance();
            } else if (current_.kind == TokenKind::Identifier) {
                advance();  // keyword-spelled operator (e.g. `mod`)
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
