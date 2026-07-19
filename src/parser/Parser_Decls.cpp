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
    // Issue #135 span-stamping (see the wrapper above parseCompilationUnit):
    // capture the `import` keyword's location so a later pass (§2.8.1 rule 4
    // self-import rejection, issue #1730) can report a real file:line:col
    // instead of the 0:0 an unstamped node's default span would print.
    const SourceLocation start = current_.location;
    auto decl = std::make_unique<ast::Import>();
    // Caller has verified `current_` is the `import` keyword.
    advance(); // past `import`
    decl->name = parseQualifiedName();
    // Grammar A.1 (§2.2 v0.164 / #1365): optional `as Identifier` alias clause.
    // `import BankingApp.Transactions as Tx;` binds `Tx` as a file-scoped
    // qualifier for that module.
    if (isKeyword("as")) {
        advance(); // past `as`
        if (current_.kind == TokenKind::Identifier) {
            decl->alias = std::string(current_.text);
            advance(); // past alias identifier
        } else {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected identifier after 'as' in import alias "
                                "(Grammar A.1 / §2.2)");
        }
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after import declaration");
    }
    return stampSpan(std::move(decl), start, current_.location);
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
    else if (isKeyword("choice"))    kind = ast::TypeDeclKind::Choice;
    else {
        diagnostics_.report(
            Severity::Error, current_.location,
            "expected top-level declaration (class, interface, record, enum, choice, type)");
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
            const SourceLocation primaryCtorParamListLoc = current_.location;
            if (!isPunctuator(")")) {
                // Records: PositionalField list (SerializedClause-bearing,
                // no modifiers, no default). Classes: PrimaryCtorParam list
                // (ParamList — modifiers + default, no SerializedClause).
                decl->positionalFields =
                    (decl->kind == ast::TypeDeclKind::Record)
                        ? parsePositionalFieldList()
                        : parseParamList();
            }
            // §22.10 (v0.160 `ref`/`out`; v0.180 `scoped`) / #1594: a primary-
            // constructor parameter list admits plain and `owned` parameters
            // only — `ref`/`out` are rejected because a constructor parameter
            // initializes object state and a by-reference alias or caller-out
            // slot has no meaning in that role; `scoped` is rejected because
            // its storage contract (§28.14) needs an explicitly declared
            // field. The shared Param production over-admits all of these
            // syntactically (Appendix A.4's over-admit-and-reject-
            // semantically pattern, used identically by the extern-
            // declaration `scoped` check above); reject them here. Records
            // parse through parsePositionalFieldList (no modifiers at all),
            // so this loop is a no-op for records.
            if (decl->kind == ast::TypeDeclKind::Class) {
                for (const auto& p : decl->positionalFields) {
                    if (p.modifier == "ref" || p.modifier == "out" ||
                        p.modifier == "out owned") {
                        diagnostics_.report(
                            Severity::Error, primaryCtorParamListLoc,
                            std::string("`") + std::string(p.modifier) +
                            "` is not permitted on a primary-constructor "
                            "parameter (§22.10) — a constructor parameter "
                            "initializes object state, and a by-reference "
                            "alias or caller-out slot has no meaning in that "
                            "role; parameter `" + std::string(p.name) + "`");
                    } else if (p.modifier == "scoped") {
                        diagnostics_.report(
                            Severity::Error, primaryCtorParamListLoc,
                            std::string("`scoped` is not permitted on a "
                            "primary-constructor parameter (§22.10) — the "
                            "scoped-storage contract (§28.14) needs an "
                            "explicitly declared field; use a regular "
                            "constructor instead; parameter `") +
                            std::string(p.name) + "`");
                    }
                }
            }
            if (!consumePunctuator(")")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ')' to close positional / primary-ctor list");
            }
        }
        // §9.8 v0.141 (#703): the `serialized tree/graph/cyclic` class-header
        // clause was retired. A class participates in serialization synthesis by
        // DECLARING `: ISerializable`/`: IDeserializable`; field types drive the
        // wire shape (sema synthesizes the shape). A polymorphic registry is now
        // a class-BODY `polymorphic(...);` member clause (parseMember path). The
        // field-level `serialized skip`/`serialized "name"`/`serialized optional`
        // clauses are unaffected (parseSerializedClause, still parsed below).
        // §11.1 / #873: for a class with primary-constructor parameters, the
        // first base item may carry a base-ctor arg list `Base(args...)`. Parse
        // it and store in decl->primaryBaseCtorArgs so the codegen can pre-
        // populate the base's positional-param fields before field initializers
        // run (§22.8 step 0 / §11.1 "chain to base before field initializers").
        // For non-primary-ctor classes and records the existing tokenize-away
        // behaviour is preserved (the arg list is only valid on the first base
        // class of a primary-ctor class; sema enforces that separately).
        if (decl->kind == ast::TypeDeclKind::Class && decl->hasPositionalFields) {
            // Custom inheritance parse that captures the first base's arg list.
            if (isPunctuator(":")) {
                advance();  // past ':'
                // Parse the first base type.
                auto firstBase = parseType();
                decl->baseTypes.push_back(std::move(firstBase));
                // Capture base-ctor args if present.
                if (isPunctuator("(")) {
                    advance();  // past '('
                    if (!isPunctuator(")"))
                        decl->primaryBaseCtorArgs = parseArgList();
                    if (!consumePunctuator(")"))
                        diagnostics_.report(Severity::Error, current_.location,
                            "expected ')' to close base constructor argument list");
                }
                // Remaining base types (interfaces) — only one class base is
                // valid for a primary-ctor class (sema enforces); parse further
                // items for completeness, discarding their '(...)' groups.
                while (isPunctuator(",")) {
                    advance();
                    auto t = parseType();
                    decl->baseTypes.push_back(std::move(t));
                    if (isPunctuator("(")) {
                        int depth = 1;
                        advance();
                        while (depth > 0 && current_.kind != TokenKind::EndOfFile) {
                            if (isPunctuator("("))      ++depth;
                            else if (isPunctuator(")")) --depth;
                            advance();
                        }
                    }
                }
            }
        } else {
            decl->baseTypes = parseInheritance();
        }
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
            // §9.8 v0.141 (P6): class-body `polymorphic("disc": Type, ...);`
            // member clause declares the serialization polymorphic registry.
            // `polymorphic` is contextual: only a member clause when followed by
            // `(` (otherwise it is an ordinary identifier, e.g. a field name).
            if (current_.kind == TokenKind::Identifier &&
                current_.text == "polymorphic" &&
                peekAt(1).text == "(") {
                parsePolymorphicMember(*decl);
                continue;
            }
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
    } else if (decl->kind == ast::TypeDeclKind::Choice) {
        // §A.3 / §35.1 (v0.200, #2044):
        //   ChoiceCases ::= ChoiceCase ("," ChoiceCase)* ";"
        //   ChoiceCase  ::= Identifier CasePayload? SerializedClause?
        //   CasePayload ::= "(" CaseField ("," CaseField)* ")"
        // A choice requires at least one case (§35.1).
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected choice case name (choice body requires at least "
                "one)");
        } else {
            auto parseOneCase = [&]() -> ast::ChoiceCase {
                ast::ChoiceCase c;
                if (current_.kind != TokenKind::Identifier) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected choice case name");
                } else {
                    c.name = current_.text;
                    advance();
                }
                // §A.3 CasePayload? — payload-free case (`NotModified`) when
                // no `(` follows. The payload list reuses Param (its
                // `modifier` slot carries the optional `owned` marker).
                if (isPunctuator("(")) {
                    advance();  // past '('
                    if (!isPunctuator(")")) {
                        c.payload = parseCasePayloadList();
                    }
                    if (!consumePunctuator(")")) {
                        diagnostics_.report(
                            Severity::Error, current_.location,
                            "expected ')' to close choice case payload list");
                    }
                }
                if (isSerializedClauseStart()) {
                    c.serialized = parseSerializedClause();
                }
                return c;
            };
            decl->choiceCases.push_back(parseOneCase());
            while (consumePunctuator(",")) {
                decl->choiceCases.push_back(parseOneCase());
            }
            if (!consumePunctuator(";")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ';' after choice cases");
            }
            // §A.3 ChoiceMember* — identical grammar to EnumMember; parse
            // through the shared Member grammar and let sema filter the
            // admissible kinds (MethodDecl, ExpressionPropertyDecl,
            // StaticReadonlyDecl, StaticConstDecl, OperatorDecl).
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
    consumeOptionalEndKeyword("extension"); // §1.6 / v0.170 (#1374)
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
        const SourceLocation paramListLoc = current_.location;
        decl.params = parseParamList();
        // per §32 / §28.14 (v0.175): `scoped` is illegal on extern
        // declarations — neither the callee storage obligations nor the
        // quiescence guarantee can be verified across the FFI boundary.
        for (const auto& p : decl.params) {
            if (p.modifier == "scoped") {
                diagnostics_.report(
                    Severity::Error, paramListLoc,
                    std::string("`scoped` is illegal on an extern "
                                "declaration — the callee's storage "
                                "obligations and quiescence cannot be "
                                "verified across the FFI boundary (§32, "
                                "§28.14); parameter `") +
                    std::string(p.name) + "`");
            }
        }
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
    consumeOptionalEndKeyword("extern"); // §1.6 / v0.170 (#1374)
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end' of extern block");
    }
    return std::unique_ptr<ast::TopDecl>(blk.release());
}

std::unique_ptr<ast::TypeAlias> Parser::parseTypeAlias() {
    // A.2: `TypeAliasDecl ::= "type" Identifier GenericParams? ":=" Type
    //                          GenericConstraints* ";"`. (v0.160: ":=" replaces
    // the old "=" — all bindings in Bezel use ":="; §27.8.) Slice 27 completes
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

    // per §27.8 / v0.160: type aliases bind with ":=" (not bare "=").
    // Detect plain "=" and emit a fix-it diagnostic for migration.
    if (isPunctuator("=")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "type aliases bind with ':=' since v0.160 "
                            "(replace '=' with ':='; §27.8)");
        advance(); // consume "=" for error recovery
    } else if (!consumePunctuator(":=")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ':=' in type alias (§27.8)");
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
                } else if (current_.kind == TokenKind::Identifier &&
                           current_.text == "scoped" &&
                           (peekAt(1).kind == TokenKind::Identifier ||
                            (peekAt(1).kind == TokenKind::Punctuator &&
                             (peekAt(1).text == "(" ||
                              peekAt(1).text == "[")))) {
                    // per §28.14 / A.6 FuncParamMods (v0.175): contextual
                    // `scoped` in a function type, e.g. `(scoped () -> Task)
                    // -> ()`. It is the modifier exactly when the next token
                    // can begin a FuncParamType (an Identifier — a named
                    // param or a named type — or `(` / `[`); a lone param of
                    // NAMED TYPE `scoped` is followed by `,` / `)` / `?` and
                    // a param NAMED `scoped` by `:`, so neither matches.
                    fp.modifier = "scoped";
                    advance();
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
                // §27.2: `((FunctionType))?` — the outer parens are a
                // grouping operator required before `?` (without them
                // the `?` would attach to the return type instead of the
                // whole function type). Detect: exactly one param, no
                // modifier, no name, whose type is itself a Function;
                // and `?` follows (the nullable suffix on the grouped
                // type). Per spec: `((int) -> bool)?` is a nullable
                // function type, distinct from `(int) -> bool?`.
                if (type->funcParams.size() == 1 &&
                    type->funcParams[0].modifier.empty() &&
                    type->funcParams[0].name.empty() &&
                    type->funcParams[0].type &&
                    type->funcParams[0].type->kind ==
                        ast::TypeKind::Function &&
                    isPunctuator("?")) {
                    auto inner = std::move(type->funcParams[0].type);
                    inner->nullable = true;
                    advance(); // consume `?`
                    return inner;
                }
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
    // #662c: record the start offset of the member's first token (visibility
    // keyword or modifier or `constructor`/`destructor`) so that the builder
    // can extract source text for cross-module ctor/dtor body export.  The
    // offset is stamped onto ConstructorDecl/DestructorDecl nodes before
    // returning; all other member kinds leave their span at 0 (existing behaviour).
    const SourceLocation memberStart = current_.location;

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
    for (;;) {
        if (isKeyword("static")   || isKeyword("owned")    ||
            isKeyword("readonly") || isKeyword("async")    ||
            isKeyword("abstract") || isKeyword("virtual")  ||
            isKeyword("sealed")   || isKeyword("override")) {
            modifiers.push_back(current_.text);
            advance();
            continue;
        }
        // per §28.14 (v0.175): `scoped` is a CONTEXTUAL field modifier (lexed
        // as Identifier). A `scoped` FIELD holds a callback stored under a
        // `scoped` parameter: invocable / overwritable / clearable, never
        // readable-out. It is the modifier exactly when the next token is the
        // field's name (an Identifier); a field NAMED `scoped` is followed by
        // `:` and is not matched. Function-typed-ness is checked in sema.
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "scoped" &&
            peekAt(1).kind == TokenKind::Identifier) {
            modifiers.push_back(current_.text);
            advance();
            continue;
        }
        break;
    }

    // A.4 NestedTypeDecl as a Member: `class` / `interface` / `record` /
    // `enum` / `choice` keyword after Visibility?+Modifier*. The modifier set
    // for top-level type-decls (`abstract`/`sealed`/`static`) overlaps with the
    // member-modifier set we already collected, so we hand them down.
    if (isKeyword("class") || isKeyword("interface") ||
        isKeyword("record") || isKeyword("enum") ||
        isKeyword("choice")) {
        // A nested type is a class member (§11.3) and is not one of §13.3's
        // three no-visibility exceptions (static constructor, `polymorphic`
        // registry, destructor); §11.6 states explicitly that its visibility
        // keyword is required with no exception for nested types — omitting
        // it is a compile error, not a default to §13.2's top-level
        // default-internal.
        if (visibility == ast::Visibility::None) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "nested type declarations require explicit visibility "
                "(§13.3, §11.6)");
        }
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
            parseMethodDeclBody(visibility, std::move(modifiers),
                                memberStart).release());
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
        // #662c: capture end offset (the `;` + 1 byte) before consuming it so
        // the builder can slice the full declaration text for cross-module export.
        SourceLocation memberEnd = current_.location;
        memberEnd.offset += static_cast<std::uint32_t>(current_.text.size());
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after 'end' of constructor");
        }
        ctor->span.begin = memberStart;
        ctor->span.end   = memberEnd;
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
        // #662c: stamp span for cross-module dtor-body export (same pattern as
        // ctor above).
        SourceLocation dtorEnd = current_.location;
        dtorEnd.offset += static_cast<std::uint32_t>(current_.text.size());
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after 'end' of destructor");
        }
        dtor->span.begin = memberStart;
        dtor->span.end   = dtorEnd;
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
    // §10.1), signatures only. The pre-keyword prefix is `readonly?` (for
    // property), `async?` (for event), and — per §11.8 (v0.176, #807) —
    // `static` for the abstract-only static method / static operator forms.
    // All are scanned tentatively then dispatched on the kind keyword.
    bool readonlyMod = false;
    bool asyncMod = false;
    bool staticMod = false;
    while (isKeyword("readonly") || isKeyword("async") ||
           isKeyword("static")) {
        if (isKeyword("readonly")) {
            readonlyMod = true;
        } else if (isKeyword("static")) {
            staticMod = true;  // per §11.8
        } else {
            asyncMod = true;
        }
        advance();
    }

    // IfaceProperty / IfaceIndexer.
    if (consumeKeyword("property")) {
        // per §11.8 (v0.176): static interface constants and properties are
        // DEFERRED — only static methods and static operators are admitted.
        // Spell a constant as a nullary method (`Zero()`).
        if (staticMod) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`static` interface members may be methods or operators only; "
                "static interface properties are deferred — spell a constant "
                "as a nullary method (§11.8)");
        }
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
        // §23.3 / §23.4: interface accessor bodies may use the block form
        // `Accessor* "end" ";"` where each Accessor has the abstract body
        // (`;`-terminated), optionally carrying a SetterParamSpec (e.g.
        // `set(owned);`).  This is required for owned-setter declarations on
        // interface indexers (`IOwningList<T>`) per §23.3.  The grammar
        // appendix A shows only the simple `";"` form for IfaceIndexer /
        // IfaceProperty; the prose of §23.3 extends it.  per §23.3.
        if (isPropertyAccessorStart()) {
            pd->bodyKind = ast::PropertyBodyKind::Block;
            while (isPropertyAccessorStart()) {
                pd->accessors.push_back(parsePropertyAccessor());
            }
            if (!consumeKeyword("end")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected 'end' to close interface property accessor block");
            }
            // Interface properties have no name to label `end` with; plain
            // `end;` is the only valid form (enforced semantically for named
            // properties; indexers have no name per §23.4).
            if (current_.kind == TokenKind::Identifier &&
                !pd->isIndexer && !pd->name.empty()) {
                pd->blockEndLabel = current_.text;
                advance();
            }
            if (!consumePunctuator(";")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ';' after 'end' of interface property accessor block");
            }
        } else {
            pd->bodyKind = ast::PropertyBodyKind::Auto; // simple signature form
            if (!consumePunctuator(";")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ';' after interface property declaration");
            }
        }
        return std::unique_ptr<ast::Member>(pd.release());
    }

    // IfaceEvent.
    if (consumeKeyword("event")) {
        // per §11.8 (v0.176): `static` admits only method/operator members.
        if (staticMod) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`static` interface members may be methods or operators only "
                "(§11.8)");
        }
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
        // per §11.8 (v0.176, #807): a static interface method. Same signature
        // grammar; abstract-only (the `;` requirement below enforces no body).
        if (staticMod) m->modifiers.push_back("static");
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
        // per §11.8: static interface members are ABSTRACT-ONLY — a body (the
        // `=>` expression form is the recognisable shape) is a compile error.
        if (staticMod && isPunctuator("=>")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "a static interface member is abstract-only and may not have "
                "a body (§11.8)");
            advance();               // past `=>`
            (void)parseExpression(); // skim the body expression
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after interface method signature");
        }
        m->bodyKind = ast::MethodBodyKind::Abstract;
        return std::unique_ptr<ast::Member>(m.release());
    }

    // per §11.8 (v0.176, #807): IfaceOperator — `"static" "operator" OpToken
    // "(" ParamList ")" ":" "owned"? Type ";"`. Abstract-only: no body form is
    // admitted. `operator` is a contextual keyword (App. B.2) — lexed as an
    // Identifier — so it is matched by text.
    if (current_.kind == TokenKind::Identifier && current_.text == "operator") {
        if (!staticMod) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "an interface operator member requires `static` "
                "(`static operator +(a: T, b: T): T;`) (§11.8)");
        }
        advance();  // past `operator`
        auto op = std::make_unique<ast::OperatorDecl>();
        op->visibility = ast::Visibility::None;  // implicit public (§10.1)
        op->opToken = current_.text;
        advance();  // past the OpToken
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
        // per §11.8: abstract-only — reject the expression-body form cleanly.
        if (isPunctuator("=>")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "a static interface member is abstract-only and may not have "
                "a body (§11.8)");
            advance();               // past `=>`
            (void)parseExpression(); // skim the body expression
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after static interface operator signature");
        }
        op->bodyKind = ast::MethodBodyKind::Abstract;
        return std::unique_ptr<ast::Member>(op.release());
    }

    diagnostics_.report(
        Severity::Error, current_.location,
        "expected 'method', 'property', 'event', or 'static "
        "method'/'static operator' (§11.8) in interface body");
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
        // SetterParamSpec ::= "(" ParamMod? Identifier? ")"; ParamMod ::= "owned".
        // The setter's value parameter is always an input, so the `ref`/`out`
        // spellings never had semantics here and no longer parse (§A.4
        // ParamMod, v0.180 — Design Rationale v0.180 migration notes: "ref/out
        // in a setter parameter spec become compile errors").
        acc.hasSetterParamSpec = true;
        if (isKeyword("ref") || isKeyword("out")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                std::string("'") + std::string(current_.text) +
                "' is not a valid setter parameter modifier; only 'owned' "
                "is permitted (the setter's value parameter is always an "
                "input)");
            advance();
            if (consumeKeyword("owned")) {
                acc.setterParamMod = "owned";
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
            // §13.6 / §23.3 / §A.4: the `=> ...` body admits both an
            // Expression (followed by `;`) and a single Statement whose own
            // terminating `;` also closes the AccessorBody.  The latter is
            // required by the §13.6 example `set => _name := value;` because
            // §A.6 / §17 prose establish that assignments are statements, not
            // expressions.  We detect the statement cases (assignment and
            // increment) after parsing the leading expression/LHS, mirroring
            // the logic in parseStatement()'s expression-led path.  per §13.6
            auto lhs = parseExpression();
            const bool isAssignOp =
                current_.kind == TokenKind::Punctuator &&
                (current_.text == ":="  || current_.text == "+="  ||
                 current_.text == "-="  || current_.text == "*="  ||
                 current_.text == "/="  || current_.text == "&="  ||
                 current_.text == "|="  || current_.text == "^="  ||
                 current_.text == "<<=" || current_.text == ">>=" ||
                 current_.text == "??=");
            const bool isIncrOp =
                current_.kind == TokenKind::Punctuator &&
                (current_.text == "++" || current_.text == "--");
            if (isAssignOp) {
                // Single assignment statement body: store as Block so the
                // §A.4 invariant (accessor block == stmts list) is preserved.
                acc.bodyKind = ast::PropertyAccessorBodyKind::Block;
                auto a = std::make_unique<ast::AssignmentStmt>();
                a->target = std::move(lhs);
                a->op = current_.text;
                advance(); // past assignment operator
                a->value = parseExpression();
                if (!consumePunctuator(";")) {
                    diagnostics_.report(Severity::Error, current_.location,
                                        "expected ';' after accessor body");
                }
                acc.stmts.push_back(
                    std::unique_ptr<ast::Stmt>(a.release()));
                return acc; // `;` already consumed above
            } else if (isIncrOp) {
                // Single increment statement body.
                acc.bodyKind = ast::PropertyAccessorBodyKind::Block;
                auto inc = std::make_unique<ast::IncrementStmt>();
                inc->target = std::move(lhs);
                inc->op = current_.text;
                advance();
                if (!consumePunctuator(";")) {
                    diagnostics_.report(Severity::Error, current_.location,
                                        "expected ';' after accessor body");
                }
                acc.stmts.push_back(
                    std::unique_ptr<ast::Stmt>(inc.release()));
                return acc; // `;` already consumed above
            } else {
                acc.bodyKind = ast::PropertyAccessorBodyKind::Expression;
                acc.exprBody = std::move(lhs);
            }
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

// §9.8 v0.141 (P6): class-body member clause
//   `polymorphic ( "disc" : Type ("," "disc" : Type)* ) ;`
// Declares the serialization polymorphic registry on the class. Populates
// decl.serializedClass->polyCases (creating the clause if absent); the wire
// shape is derived later by the sema synthesis pre-pass (a registry implies the
// graph shape). Sema/codegen read polyCases identically to the retired header
// `polymorphic(...)` clause, so no sema/codegen change is needed.
void Parser::parsePolymorphicMember(ast::TypeDecl& decl) {
    advance();  // past `polymorphic`
    if (!consumePunctuator("(")) {
        diagnostics_.report(Severity::Error, current_.location,
            "expected '(' to begin `polymorphic` member registry");
    }
    if (!decl.serializedClass) {
        decl.serializedClass = std::make_unique<ast::SerializedClassClause>();
    }
    auto parseOne = [&]() {
        ast::PolyCase pc;
        if (current_.kind != TokenKind::StringLiteral) {
            diagnostics_.report(Severity::Error, current_.location,
                "expected discriminator string in `polymorphic` case");
        } else {
            std::string_view txt = current_.text;
            if (txt.size() >= 2 && txt.front() == '"' && txt.back() == '"') {
                pc.discriminator = txt.substr(1, txt.size() - 2);
            } else {
                pc.discriminator = txt;
            }
            advance();
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(Severity::Error, current_.location,
                "expected ':' between `polymorphic` discriminator and type");
        }
        pc.type = parseType();
        return pc;
    };
    decl.serializedClass->polyCases.push_back(parseOne());
    while (consumePunctuator(",")) {
        decl.serializedClass->polyCases.push_back(parseOne());
    }
    if (!consumePunctuator(")")) {
        diagnostics_.report(Severity::Error, current_.location,
            "expected ')' to close `polymorphic` member registry");
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
            "expected ';' after the `polymorphic` member clause");
    }
}


std::unique_ptr<ast::MethodDecl> Parser::parseMethodDeclBody(
    ast::Visibility visibility, std::vector<std::string_view> modifiers,
    SourceLocation memberStart) {
    // Caller has already consumed the `method` keyword. We pick up at the
    // method name.
    auto m = std::make_unique<ast::MethodDecl>();
    m->visibility = visibility;
    m->modifiers = std::move(modifiers);
    // #783 / §14.1: stamp the full-declaration span (first member token through
    // the terminating `;`) so the builder can slice the method source text for
    // cross-module generic-method body export, the same way the ctor/dtor paths
    // do (#662c). `methodEnd` is captured just before the terminating `;` is
    // consumed in each body form; it is one past that `;`. Left at default
    // (line 0) on a hard parse error, so the builder skips export (degenerate).
    SourceLocation methodEnd{};
    auto captureEnd = [&]() {
        methodEnd = current_.location;
        methodEnd.offset +=
            static_cast<std::uint32_t>(current_.text.size());
    };

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

    // A.4 MethodBody: `=> (Expression | AssignmentStmt) ";"` | Block | ";"
    // (v0.160: AssignmentStmt alternative in the `=>` body; valid only for
    // unit methods, i.e. no declared return type — enforced semantically, §22.2.)
    if (consumePunctuator("=>")) {
        // Parse the leading expression first (LHS of potential assignment).
        auto lhs = parseExpression();
        const bool isAssignOp =
            current_.kind == TokenKind::Punctuator &&
            (current_.text == ":="  || current_.text == "+="  ||
             current_.text == "-="  || current_.text == "*="  ||
             current_.text == "/="  || current_.text == "&="  ||
             current_.text == "|="  || current_.text == "^="  ||
             current_.text == "<<=" || current_.text == ">>=" ||
             current_.text == "??=");
        if (isAssignOp) {
            // §22.2 / v0.160: `=> LHS := RHS;` on a unit method (no return type).
            // Stored as a Block body (one AssignmentStmt), mirroring the accessor
            // `set => _x := value;` pattern. Semantic check (return type must be
            // absent) is enforced in sema, not here. per §22.2
            m->bodyKind = ast::MethodBodyKind::Block;
            auto a = std::make_unique<ast::AssignmentStmt>();
            a->target = std::move(lhs);
            a->op = current_.text;
            advance(); // past assignment operator
            a->value = parseExpression();
            captureEnd();
            if (!consumePunctuator(";")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ';' after arrow-assignment method body (§22.2)");
                methodEnd = {};
            }
            m->stmts.push_back(std::unique_ptr<ast::Stmt>(a.release()));
        } else {
            m->bodyKind = ast::MethodBodyKind::Expression;
            m->exprBody = std::move(lhs);
            captureEnd();  // #783: the `;` terminating the expression body
            if (!consumePunctuator(";")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ';' after expression-body method");
                methodEnd = {};  // span unreliable on error
            }
        }
    } else if (isPunctuator(";")) {
        captureEnd();  // #783: the `;` terminating an abstract method
        advance();
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
        captureEnd();  // #783: the `;` terminating a block-body method
        if (!consumePunctuator(";")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after 'end'");
            methodEnd = {};  // span unreliable on error
        }
    }
    // #783: stamp the full-declaration span when both ends are known.
    if (memberStart.line != 0 && methodEnd.line != 0) {
        m->span.begin = memberStart;
        m->span.end   = methodEnd;
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
        } else if (current_.kind == TokenKind::Identifier &&
                   current_.text == "scoped" &&
                   peekAt(1).kind == TokenKind::Identifier) {
            // per §28.14 / §22.3 (v0.175): `scoped` is a CONTEXTUAL modifier
            // (lexed as Identifier, not a reserved keyword) valid only on
            // function-typed parameters. It is the modifier exactly when the
            // next token is the parameter's name (an Identifier); a parameter
            // NAMED `scoped` is followed by `:` and is not matched here.
            // Function-typed-ness is checked in sema (TypeResolution), where
            // a type alias to a function type resolves transparently.
            p.modifier = "scoped";
            advance();
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
        // §22.4 (v0.183, #1585) A.5 `OutNullPostcond ::= "when" ("true" |
        // "false")`. An out-nullability postcondition directly follows the
        // parameter's Type. No parse ambiguity: `out` parameters never carry
        // defaults (§22.5), so at this point the next token is `,`, `)`, or the
        // `when` clause. Semantic admissibility (out/out-owned nullable param in
        // a bool-returning method) is checked in TypeResolution.
        if (consumeKeyword("when")) {
            if (consumeKeyword("true")) {
                p.outNullPostcond = ast::OutNullWhen::WhenTrue;
            } else if (consumeKeyword("false")) {
                p.outNullPostcond = ast::OutNullWhen::WhenFalse;
            } else {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected `true` or `false` after `when` in an "
                    "out-nullability postcondition (§22.4)");
            }
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
    // ArgModifiers ∈ { ref | out var? | out using var | out owned var | owned
    //                   | scoped }
    // per §A.4 / v0.160: the former bare `out owned` form now requires "var"
    // (an inline owned-local declaration). §22.3/§28.14 (v0.180, #1439): the
    // `scoped` call-site marker parallels bare `owned` — it marks a class/
    // interface-typed borrow argument to a `scoped` parameter (`M(scoped x)`).
    // Positional-only like the other modified forms; fresh-producer /
    // declared-before/freeze admission is checked semantically in ownership
    // (§28.14 "scoped construction"). ArgExpression ::= Expression | "_"
    std::vector<ast::CallArgument> args;
    if (isPunctuator(")")) return args;
    auto parseOne = [&]() -> ast::CallArgument {
        ast::CallArgument a;
        if (consumeKeyword("ref")) {
            a.modifier = "ref";
        } else if (consumeKeyword("out")) {
            if (consumeKeyword("owned")) {
                // per §A.4 / v0.160: `out owned` MUST be followed by `var`
                // (declares an inline owned local); bare `out owned x` is
                // no longer derivable from the grammar.
                if (!consumeKeyword("var")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected 'var' after 'out owned' (§22.4, v0.160: "
                        "use 'out owned var x' to declare an inline owned local)");
                }
                a.modifier = "out owned var";
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
        } else if (current_.kind == TokenKind::Identifier &&
                   current_.text == "scoped" &&
                   (peekAt(1).kind == TokenKind::Identifier ||
                    (peekAt(1).kind == TokenKind::Keyword &&
                     (peekAt(1).text == "this" || peekAt(1).text == "base" ||
                      // per §9.7 / §28.14 (v0.180): the canonical FRESH-
                      // PRODUCER shape `scoped new X(...)` — semantically
                      // rejected (checkScopedAdmission, ownership pass), but
                      // must still PARSE so that rejection can fire instead
                      // of a confusing syntax error.
                      peekAt(1).text == "new")))) {
            // §22.3/§28.14 (v0.180, #1439): `scoped` is a CONTEXTUAL call-site
            // argument marker (lexed as Identifier, like the parameter/field/
            // FuncParamType `scoped` modifiers above) on a class/interface-
            // typed borrow argument — `M(scoped x)`. It is the modifier
            // exactly when the next token can begin the borrowed value's
            // expression: a bare identifier/member-access root, `this` /
            // `base`, or a `new` expression (the fresh-producer shape, §9.7).
            // A bare argument whose value IS a variable literally named
            // `scoped` is followed by `,` / `)` / a binary operator — none of
            // which are Identifier/`this`/`base`/`new` — and is not matched
            // here (parsed as an ordinary unmodified argument).
            a.modifier = "scoped";
            advance();
        }
        // Named-arg prefix `Identifier :=`. peekAt(1) distinguishes from
        // a bare-Identifier expression argument.
        // per §22.5 / v0.160: `ref`/`out`/`owned`/`scoped` arguments are
        // positional-only; the named form is for plain-value arguments only.
        if (!a.modifier.empty()) {
            // Modified arg: named form is not permitted; parse no prefix here.
            // We do NOT look for `Identifier :=` — the expression parsing below
            // handles the identifier as the argument value.
        } else if (current_.kind == TokenKind::Identifier &&
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

// §A.3 / §35.1 (v0.200, #2044):
//   CasePayload ::= "(" CaseField ("," CaseField)* ")"
//   CaseField   ::= "owned"? Identifier ":" Type SerializedClause?
// CaseField is PositionalField plus the optional leading `owned` payload
// marker; the marker is stored in `Param::modifier` (semantically the same
// slot `owned` parameters use, per §35.3 / §28.3).
std::vector<ast::Param> Parser::parseCasePayloadList() {
    std::vector<ast::Param> fields;
    if (isPunctuator(")")) return fields;
    auto parseOne = [&]() -> ast::Param {
        ast::Param p;
        // Optional leading `owned` marker.
        if (isKeyword("owned")) {
            p.modifier = "owned";
            advance();
        }
        if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected case payload field name");
        } else {
            p.name = current_.text;
            advance();
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ':' after case payload field name");
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
        // The pre-v0.97 `where T is owned` / `where T is not owned` modifier-
        // constraint form is retired (§A.3 GenericConstraints, §14.4);
        // constraints bound types only. Emit a specific diagnostic and consume
        // the stale tokens so downstream parsing recovers cleanly.
        if (isKeyword("is")) {
            SourceLocation isLoc = current_.location;
            advance(); // consume `is`
            consumeKeyword("not"); // consume optional `not` if present
            consumeKeyword("owned"); // consume `owned` if present
            diagnostics_.report(
                Severity::Error, isLoc,
                "the pre-v0.97 `where T is owned` constraint form was removed; "
                "constraints bound types (§14.2)");
            all.push_back(std::move(clause));
            continue;
        }
        if (!consumePunctuator(":")) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ':' in generic constraint");
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
