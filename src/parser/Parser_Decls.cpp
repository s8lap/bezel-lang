#include "parser/Parser.hpp"
#include "parser/Parser_impl.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace bezel {

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
    // fix #4348: a parse run starts with the expression-recursion depth at
    // zero (the Parser may be reused if a host calls parseCompilationUnit
    // twice, though in practice the Driver constructs a fresh one per
    // file). Resetting here keeps the guard honest without depending on
    // the Parser being constructed fresh.
    expressionRecursionDepth_ = 0;
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
        } else if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): "every declaration named `_` —
            // … or alias" is a compile error.
            reportDiscardNotDeclarable("import alias");
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

    // Grammar A.3 `Modifier ::= "abstract" | "closed" | "sealed" | "static"`
    // (v0.242, #3052 — `closed` joined the production as the dedicated
    // spelling of the former `sealed abstract` pair, §11.7). The set of
    // semantically valid combinations is constrained by §11.7 (sema's
    // problem); the parser admits any combination per the grammar.
    // Now-after-visibility, check for `extension` (admits visibility prefix).
    if (isKeyword("extension")) {
        return parseExtensionDecl(visibility);
    }

    std::vector<std::string_view> modifiers;
    while (isKeyword("abstract") || isKeyword("closed") ||
           isKeyword("sealed") || isKeyword("static")) {
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

    // fix #1761: capture the source span [start, end] so SymbolCollection can
    // populate the resulting TypeSymbol's declLoc (see SymbolCollection.cpp's
    // `sym->declLoc = td.span.begin`). Without this, the kind keyword below
    // advances past the type's first token before its span is recorded, and
    // diagnostics keyed on the symbol's declLoc all report 0:0.
    const SourceLocation start = current_.location;

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

    if (atDiscard()) {
        // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
        // a compile error — `_` is a reserved token, not an Identifier.
        reportDiscardNotDeclarable("type");
    } else if (current_.kind != TokenKind::Identifier) {
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
            // per §1.6 / proposal 0059 (v0.214): optional trailing `,` covers
            // both EnumInheritance alternatives — the comma must be followed
            // by a token that closes the EnumInheritance region (the next
            // body/term token, e.g. the constants list `;`).
            if (isPunctuator(";")) break;
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
            // per §22.10: a primary-constructor parameter list admits
            // plain, `owned`, and class- or interface-typed `scoped`
            // parameters; `ref`/`out` are rejected because a constructor
            // parameter initializes object state and a by-reference alias
            // or caller-out slot has no meaning in that role. The shared
            // Param production over-admits all of these syntactically
            // (Appendix A.4's over-admit-and-reject-semantically pattern,
            // used identically by the extern-declaration `scoped` check
            // above); `ref`/`out` are decided here (parser-side), and
            // `scoped` and function-typed `scoped` are decided in sema
            // (§22.10, v0.307) once type information is available. Records
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
                // fix #2466 (Appendix A.3 InheritItem grammar note, v0.236):
                // a NON-empty argument list on any of these interface-position
                // items was previously silently discarded here with no
                // diagnostic, the same gap #2466 closes in `parseInheritance`
                // for record/interface decls. Per Appendix A.3's note, the
                // argument list is "permitted only on the base class (the
                // first item), not on interfaces"; an EMPTY list (`IFoo()`)
                // drops no data and stays accepted, matching #2455's carve-out.
                while (isPunctuator(",")) {
                    advance();
                    auto t = parseType();
                    decl->baseTypes.push_back(std::move(t));
                    if (isPunctuator("(")) {
                        const SourceLocation argListLoc = current_.location;
                        // EMPTY `()` drops no data — only a non-empty list is
                        // rejected (consistent with #2455 / #2466 elsewhere).
                        const bool argListNonEmpty =
                            peekNext().kind != TokenKind::Punctuator ||
                            peekNext().text != ")";
                        int depth = 1;
                        advance();
                        // fix #4341 (§26.3-analogous discard-loop hardening,
                        // Appendix A.3, v0.298): bound this discard scan by
                        // `isKeyword("end")` in addition to depth/EOF, the
                        // same shape #4257 applied to the extension
                        // `polymorphic(...)` discard loop. Without this
                        // bound, an unclosed `(` on a non-first InheritItem
                        // (e.g. `class Foo(x: int): Base(x), IBar(unclosed`)
                        // consumed the type's own `end Foo;` terminator and
                        // every subsequent top-level declaration to EOF —
                        // measured directly: this exact probe shape produced
                        // only "expected 'end'" at EOF, swallowing a
                        // deliberately-malformed following class whole. This
                        // cannot misfire on well-formed input: `ArgList`
                        // (Appendix A) admits only expression tokens inside
                        // the parens, and `end` is a reserved keyword
                        // (`Lexer.cpp`'s `isReservedKeyword`), so it can
                        // never be part of a well-formed argument — a
                        // well-formed list can never be at depth >= 1 when
                        // the current token is `end`.
                        while (depth > 0 && current_.kind != TokenKind::EndOfFile &&
                               !isKeyword("end")) {
                            if (isPunctuator("("))      ++depth;
                            else if (isPunctuator(")")) --depth;
                            advance();
                        }
                        if (argListNonEmpty) {
                            diagnostics_.report(
                                Severity::Error, argListLoc,
                                "an argument list is not permitted here — per "
                                "Appendix A's `InheritItem` grammar (v0.236), "
                                "it is permitted only on the base class (the "
                                "first item in the inheritance list), not on "
                                "an interface");
                        }
                    }
                }
            }
        } else {
            decl->baseTypes = parseInheritance(decl.get());
        }
        decl->genericConstraints = parseGenericConstraints();
    }

    // §A.3 RecordDecl shorthand body: `record Foo(...) ;` (no `end`).
    if (decl->kind == ast::TypeDeclKind::Record &&
        consumePunctuator(";")) {
        decl->isRecordShorthand = true;
        // fix #2418: this early return bypassed the #1761 span-stamp below
        // (which only the non-shorthand `... end ...;` path reached), so
        // EVERY shorthand record's TypeSymbol::declLoc silently stayed 0:0
        // even before this fix — the "TypeSymbol is the one already-working
        // precedent" premise held only for the non-shorthand forms. Stamp
        // it here too, identically to the #1761 logic.
        decl->span.begin = start;
        decl->span.end = current_.location;
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
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("enum constant");
        } else if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected enum constant name (enum body requires at least "
                "one)");
        } else {
            ast::EnumConstant ec;
            ec.loc = current_.location;  // fix #2482: name token's position
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
                // per §1.6 / proposal 0059 (v0.214): optional trailing `,`
                // before the closing `;`.
                if (isPunctuator(";")) break;
                if (atDiscard()) {
                    // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
                    // a compile error — `_` is a reserved token, not an Identifier.
                    reportDiscardNotDeclarable("enum constant");
                    break;
                }
                if (current_.kind != TokenKind::Identifier) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected enum constant name after ','");
                    break;
                }
                ast::EnumConstant ec2;
                ec2.loc = current_.location;  // fix #2482
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
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("choice case");
        } else if (current_.kind != TokenKind::Identifier) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected choice case name (choice body requires at least "
                "one)");
        } else {
            auto parseOneCase = [&]() -> ast::ChoiceCase {
                ast::ChoiceCase c;
                if (atDiscard()) {
                    // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
                    // a compile error — `_` is a reserved token, not an Identifier.
                    reportDiscardNotDeclarable("choice case");
                } else if (current_.kind != TokenKind::Identifier) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected choice case name");
                } else {
                    c.loc = current_.location;  // fix #2482: name token
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
                // per §1.6 / proposal 0059 (v0.214): optional trailing `,`
                // before the closing `;`.
                if (isPunctuator(";")) break;
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
        // fix #4347 (§1.6 / Appendix A.3, v0.298): the discard loop here
        // tracks only `<`/`>` depth, bounded solely by `depth == 0` or
        // `EndOfFile`. On a malformed end-label with an unclosed `<` nothing
        // bounded the scan, so it consumed the rest of the file. Unlike the
        // three sibling discard-loop instances fixed this run (#4257,
        // #4341, #4343), this loop runs AFTER the enclosing type's `end`
        // keyword has already been consumed — there is no single reserved
        // keyword that universally bounds it the way `end` does for the
        // others, since the next top-level declaration can start with
        // several different keywords (`Parser::parseTopDecl`). Bound by:
        //   - `;` (the end-label's expected terminator; `;` cannot appear
        //     inside a well-formed generic-parameter spelling — Appendix A.3
        //     `GenericArgs` admits only `GenericArg` separated by `,`,
        //     balanced `<`/`>`, no statement-level punctuation); and
        //   - any TopDecl-starting keyword reachable from this position
        //     (`type` / `extension` / `extern`, the four visibility
        //     keywords, the four modifier keywords, the five kind keywords
        //     `class`/`interface`/`record`/`enum`/`choice`). None of these
        //     can appear inside a well-formed generic-parameter end-label
        //     spelling (none is a valid type-parameter identifier; the
        //     GenericArgs grammar's terminals are `Identifier`, `,`, `<`,
        //     `>`). The pre-fix bug suppressed every diagnostic that would
        //     otherwise be reported in the rest of the file.
        if (isPunctuator("<")) {
            advance();  // skip `<`
            int depth = 1;
            while (depth > 0 && current_.kind != TokenKind::EndOfFile) {
                if (isPunctuator("<")) depth++;
                else if (isPunctuator(">")) { depth--; if (depth == 0) break; }
                else if (isPunctuator(";") ||
                         isKeyword("type") || isKeyword("extension") ||
                         isKeyword("extern") ||
                         isKeyword("public") || isKeyword("private") ||
                         isKeyword("internal") || isKeyword("protected") ||
                         isKeyword("abstract") || isKeyword("closed") ||
                         isKeyword("sealed") || isKeyword("static") ||
                         isKeyword("class") || isKeyword("interface") ||
                         isKeyword("record") || isKeyword("enum") ||
                         isKeyword("choice")) {
                    break;
                }
                advance();
            }
            if (isPunctuator(">")) advance();  // consume final `>`
        }
    }
    if (!consumePunctuator(";")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ';' after 'end'");
    }

    // fix #1761: stamp the type's [start, current_.location] span so the
    // resulting TypeSymbol's declLoc is populated at symbol collection.
    if (decl && decl->span.begin.line == 0) {
        decl->span.begin = start;
        decl->span.end = current_.location;
    }
    return decl;
}

std::unique_ptr<ast::TopDecl> Parser::parseExtensionDecl(
    ast::Visibility visibility) {
    // A.3 `ExtensionDecl ::= Visibility? "extension" Type GenericConstraints*
    //                         ExtensionMember* "end" ";"`.
    // #1307: capture the `extension` keyword's own position (matching
    // parseTypeDeclBody's `start` convention just above — the kind keyword's
    // location, not the visibility prefix's) so the block carries a real span;
    // previously ExtensionDecl::span was never stamped at all (neither call
    // site in parseTopDecl wraps this in the file's `stampSpan` helper, unlike
    // e.g. parseImportDecl), so every extension-block diagnostic that wants a
    // declaration site — including the new §26.5 rule-4 ambiguity diagnostic —
    // had nothing to report but 0:0.
    const SourceLocation start = current_.location;
    auto ext = std::make_unique<ast::ExtensionDecl>();
    ext->visibility = visibility;
    advance(); // past `extension`
    ext->targetType = parseType();
    ext->genericConstraints = parseGenericConstraints();
    while (!isKeyword("end") &&
           current_.kind != TokenKind::EndOfFile) {
        // fix #4222 (§26.3, Appendix A.4, v0.298): mirrors
        // `parseTypeDeclBody`'s class/record special-case peek just above
        // (`isKeyword("polymorphic") && peekAt(1).text == "("`) — `polymorphic`
        // is contextual (only a member clause when immediately followed by
        // `(`; otherwise it is an ordinary identifier, e.g. a field or method
        // name). Without this peek, `parseMember()` below has no arm for a
        // bare `polymorphic` identifier and cascades one generic parse-error
        // diagnostic per stray token instead of the one clean §26.3
        // rejection this produces.
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "polymorphic" &&
            peekAt(1).text == "(") {
            parseAndDiscardExtensionPolymorphicMember();
            continue;
        }
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
    // #1307: stamp [start, current_.location] — mirrors parseTypeDeclBody's
    // identical stamp just above (fix #1761).
    ext->span.begin = start;
    ext->span.end = current_.location;
    return std::unique_ptr<ast::TopDecl>(ext.release());
}

std::unique_ptr<ast::TopDecl> Parser::parseExternBlock() {
    // A.3 `ExternBlock ::= "extern" StringLiteral
    //                        ("convention" StringLiteral)?
    //                        ExternDecl+ "end" ";"`.
    // fix #2418: ExternBlock's span was never stamped — SymbolCollection's
    // ExternBlockSymbol::declLoc (and, via fallback, each ExternDeclSymbol)
    // had nothing to read. Mirrors the #1761 TypeSymbol / #662c ctor-dtor
    // span convention.
    const SourceLocation start = current_.location;
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
        // fix #2482: stamp the decl's own location at its first token (the
        // optional visibility keyword, else `method`) so a diagnostic on this
        // member points at its own line, not the enclosing extern block's.
        decl.loc = current_.location;
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
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("method");
        } else if (current_.kind != TokenKind::Identifier) {
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
    blk->span.begin = start;
    blk->span.end = current_.location;
    return std::unique_ptr<ast::TopDecl>(blk.release());
}

std::unique_ptr<ast::TypeAlias> Parser::parseTypeAlias() {
    // A.2: `TypeAliasDecl ::= "type" Identifier GenericParams? ":=" Type
    //                          GenericConstraints* ";"`. (v0.160: ":=" replaces
    // the old "=" — all bindings in Bezel use ":="; §27.8.) Slice 27 completes
    // the optional GenericParams and trailing GenericConstraints.
    // fix #2418: TypeAlias's span was never stamped — SymbolCollection's
    // TypeAliasSymbol::declLoc had nothing to read. Mirrors the #1761
    // TypeSymbol / #662c ctor-dtor span convention.
    const SourceLocation start = current_.location;
    auto alias = std::make_unique<ast::TypeAlias>();
    advance(); // past `type`

    if (atDiscard()) {
        // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
        // a compile error — `_` is a reserved token, not an Identifier.
        reportDiscardNotDeclarable("type alias");
    } else if (current_.kind != TokenKind::Identifier) {
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
    alias->span.begin = start;
    alias->span.end = current_.location;
    return alias;
}

std::unique_ptr<ast::Type> Parser::parseType() {
    // A.5 Type ::= TypeNoNullable "?"?
    // A.5 TypeNoNullable ::= QualifiedName GenericArgs?
    //                      | InlineArrayType
    //                      | FunctionType
    //                      | "(" ")"            /* unit */
    // fix #3796 (§3.6 / §8.3 / §28.11 / §15.3, v0.286): stamp parseType's
    // result with the [start, current token) span so generic type arguments
    // — the inner `Holder` AST node, not the outer `Dictionary<Holder, int>`
    // — carry a real source location. Every other earlier `parseExpression`
    // variant already wraps its return with stampSpan (Parser_Exprs.cpp:113);
    // `parseType` did not, so any sema diagnostic anchoring on an `ast::Type*`
    // span — the hash-container key/element reject, the element-category
    // reject, the per-instantiation constraint check — reported `0:0` in
    // precisely the contexts the spec asks for IDE-jump correctness. The
    // stompSpan no-op-on-already-stamped rule (line 19 above) keeps an
    // already-populated span (e.g. a catch-arm Type, stampSpan'd at Parser_Stmts
    // :640) untouched.
    const SourceLocation typeStart = current_.location;
    auto stampType = [&](std::unique_ptr<ast::Type> t) {
        return stampSpan(std::move(t), typeStart, current_.location);
    };
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
        return stampType(std::move(type));
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
                // per §1.6 / proposal 0059 (v0.214): optional trailing `,`
                // before the `)` that closes the function-type parameter list.
                if (isPunctuator(")")) break;
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
                    return stampType(std::move(inner));
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
        return stampType(std::move(type));
    }

    // Default: NamedType.
    type->kind = ast::TypeKind::Named;
    type->name = parseTypeName();
    if (isPunctuator("<")) {
        advance(); // past '<'
        type->genericArgs.push_back(parseType());
        while (isPunctuator(",")) {
            advance(); // past ','
            // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before
            // the closing angle — consumeClosingAngle below handles the
            // `>` / `>>` / `>=` / `>>=` lexer tokens that may close a nested
            // generic list.
            if (current_.kind == TokenKind::Punctuator &&
                !current_.text.empty() && current_.text.front() == '>') {
                break;
            }
            type->genericArgs.push_back(parseType());
        }
        if (!consumeClosingAngle()) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected '>' to close generic arguments");
        }
        // fix #3582 (A.5 / §11.6, v0.286): `Outer<int>.Tag` — a nested
        // type reached through an EXPLICITLY-ARGUED enclosing generic — is
        // not a Type. A.5 attaches the argument list to the END of the whole
        // dotted name (`TypeNoNullable ::= QualifiedName GenericArgs?`, over
        // `QualifiedName ::= Identifier ("." Identifier)*`), §11.6 spells the
        // reference `Outer.Nested`, and §2.7's leftmost-first chain resolves
        // each later segment against the type the first segment named — so no
        // argument list may sit on an enclosing segment. It has to be
        // diagnosed HERE because parseType otherwise just stops, leaving the
        // `.` to desynchronise the enclosing construct: `new Outer<int>.Tag(n)`
        // fell out as "expected '(' to begin constructor argument list", and a
        // local's type ascription left `.Tag` to start a statement, reaching
        // parsePostfix with a null head (the SEGV this issue was filed for).
        // Recovery parses the spelling the rule names, so one rejected
        // spelling yields one error instead of a cascade — the same
        // parse-it-as-the-remedy discipline the retired-spelling diagnostics use.
        if (type->name && !type->name->parts.empty() &&
            !type->genericArgs.empty() && isPunctuator(".") &&
            peekNext().kind == TokenKind::Identifier) {
            const SourceLocation dotLoc = current_.location;
            std::string outer;
            for (std::string_view part : type->name->parts) {
                if (!outer.empty()) outer += '.';
                outer.append(part);
            }
            std::string remedy = outer;
            while (isPunctuator(".") &&
                   peekNext().kind == TokenKind::Identifier) {
                advance();  // past `.`
                remedy += '.';
                remedy.append(current_.text);
                type->name->parts.push_back(current_.text);
                advance();  // past the segment identifier
            }
            diagnostics_.report(
                Severity::Error, dotLoc,
                "a generic argument list attaches to the end of a qualified "
                "type name (A.5 `TypeNoNullable ::= QualifiedName "
                "GenericArgs?`), so it may not sit on an enclosing segment: `" +
                    outer + "<...>." + remedy.substr(outer.size() + 1) +
                    "` is not a type \xe2\x80\x94 write `" + remedy +
                    "` to name the nested type (\xc2\xa7""11.6, v0.286)");
            type->genericArgs.clear();
        }
    }
    if (isPunctuator("?")) {
        type->nullable = true;
        advance();
    }
    return stampType(std::move(type));
}

std::unique_ptr<ast::Member> Parser::parseMember() {
    // #662c: record the start offset of the member's first token (visibility
    // keyword or modifier or `constructor`/`destructor`) so that the builder
    // can extract source text for cross-module ctor/dtor body export. Stamped
    // onto ConstructorDecl/DestructorDecl nodes for that purpose.
    // fix #2418: the SAME `memberStart` also anchors `span.begin` on every
    // other member kind returned from here (FieldDecl/ConstDecl/EventDecl/
    // PropertyDecl/NestedTypeMember; MethodDecl gets it via the memberStart
    // parameter threaded into parseMethodDeclBody, #783) — this is what
    // lets SymbolCollection.cpp's collectMember populate each member
    // symbol's declLoc from a real source location instead of leaving it at
    // the default {0,0}.
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
            isKeyword("sealed")   || isKeyword("override") ||
            // A.3 `Modifier` (v0.242, #3052): `closed` reaches a NESTED type
            // through this shared member-modifier prefix — the NestedTypeDecl
            // branch below hands the collected list down to
            // parseTypeOrInterfaceDecl. On any other member kind it is
            // rejected semantically (§11.7 defines `closed` for classes),
            // the same over-admit-and-reject split the rest of this list uses.
            isKeyword("closed")) {
            modifiers.push_back(current_.text);
            advance();
            continue;
        }
        // §22.15 (v0.202, #2052): `pure` is a CONTEXTUAL member modifier
        // (App. B.2 — lexed as Identifier). Admitted at the 11 grammar
        // positions enumerated in A.4 (MethodDecl, PropertyDecl, IndexerDecl,
        // ConstructorDecl, OperatorDecl, ExpressionPropertyDecl, IfaceMethod,
        // IfaceStaticMethod, IfaceProperty, IfaceIndexer, Accessor/get).
        // Other appearances (events, destructors, fields, extern decls,
        // static constructors) are rejected semantically — the modifier is
        // collected here so sema can see it on those kinds too.
        if (current_.kind == TokenKind::Identifier &&
            current_.text == "pure") {
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
        // top-level types per A.3 — `abstract`/`closed`/`sealed`/`static`. The
        // member-modifier set is a superset; semantic validation belongs
        // to sema (§11.7).
        auto td = parseTypeOrInterfaceDecl(visibility, std::move(modifiers));
        auto ntm = std::make_unique<ast::NestedTypeMember>();
        ntm->decl = std::move(td);
        // fix #2418: NestedTypeMember's own span was never stamped (only its
        // wrapped TypeDecl's span is, via parseTypeOrInterfaceDecl's #1761
        // fix) — SymbolCollection's NestedTypeSymbol wrapper had nothing to
        // read its declLoc from. `current_.location` here is just past the
        // nested type's closing `end ... ;`, mirroring the ctor/dtor
        // #662c span convention (begin = first member token, end = just
        // past the terminator).
        ntm->span.begin = memberStart;
        ntm->span.end = current_.location;
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
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("const");
        } else if (current_.kind != TokenKind::Identifier) {
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
        // fix #2418: ConstDecl's span was never stamped — SymbolCollection's
        // ConstMemberSymbol::declLoc had nothing to read. Mirrors the ctor/
        // dtor #662c span convention.
        cd->span.begin = memberStart;
        cd->span.end = current_.location;
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
        // §22.15 (v0.202, #2052): `pure` is not admitted on events. The
        // EventDecl AST does not preserve the modifiers vector, so reject
        // here at parse time rather than in sema.
        const bool isPure =
            std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("pure")) != modifiers.end();
        if (isPure) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`pure` is not admitted on an event — events are the "
                "synchronization surface purity fences off (§22.15)");
        }
        advance(); // past `event`
        auto ed = std::make_unique<ast::EventDecl>();
        ed->visibility = visibility;
        ed->isStatic = isStatic;
        ed->isAsync = isAsync;
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("event");
        } else if (current_.kind != TokenKind::Identifier) {
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
        // fix #2418: EventDecl's span was never stamped — SymbolCollection's
        // EventSymbol::declLoc had nothing to read. Mirrors the ctor/dtor
        // #662c span convention.
        ed->span.begin = memberStart;
        ed->span.end = current_.location;
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
        } else if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is a
            // compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("property");
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
                if (isEqualityClauseStart()) {
                    pd->equality = parseEqualityClause();
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
                if (isEqualityClauseStart()) {
                    pd->equality = parseEqualityClause();
                }
                if (!consumePunctuator(";")) {
                    diagnostics_.report(
                        Severity::Error, current_.location,
                        "expected ';' after auto-property");
                }
            }
        }
        // fix #2418: PropertyDecl's span was never stamped — SymbolCollection's
        // PropertySymbol::declLoc had nothing to read. Mirrors the ctor/dtor
        // #662c span convention. (All three body-kind branches above
        // converge here, after their own terminating `;` is consumed.)
        pd->span.begin = memberStart;
        pd->span.end = current_.location;
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
            return parseOperatorDeclBody(visibility, std::move(modifiers),
                                         memberStart);
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
        // §22.15 (v0.202, #2052): `pure` on the instance form only; the
        // static-constructor alternative admits no `pure` (sema rejects).
        const bool isPure =
            std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("pure")) != modifiers.end();
        // fix #4414 (§30.1, v0.300): `async` is not admitted on a
        // constructor — only methods and lambdas carry it (A.4 admits it in
        // MethodDecl and LambdaExpr only). The shared member-modifier
        // prefix above over-admits `async`; the ConstructorDecl AST does
        // not preserve the modifiers vector, so reject here at parse time
        // rather than in sema (same shape as the `pure`-not-admitted-on-
        // event check at line ~1300).
        if (std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("async")) != modifiers.end()) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`async` is not admitted on a constructor — `async` is a "
                "method/lambda modifier (A.4 admits it in MethodDecl and "
                "LambdaExpr only; §30.1, v0.300)");
        }
        if (!isStatic && visibility == ast::Visibility::None) {
            diagnostics_.report(Severity::Error, current_.location,
                                "expected member visibility before "
                                "'constructor' (or `static constructor`)");
        }
        auto ctor = std::make_unique<ast::ConstructorDecl>();
        ctor->visibility = visibility;
        ctor->isStatic = isStatic;
        ctor->isPure = isPure;
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
        // §22.16 (v0.203, #2053): A.4 places the constructor's ContractClause*
        // between `)` and the CtorChain/body. `requires` is evaluated after
        // argument binding and before any execution of the member — before
        // the base/this chain, before field initializers, before the body —
        // and `ensures` evaluates at body completion.
        ctor->contractClauses = parseContractClauses();
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
        // fix #4414 (§30.1, v0.300): same as the constructor branch above
        // — the shared member-modifier prefix over-admits `async` and the
        // DestructorDecl AST does not preserve the modifiers vector, so
        // reject at parse time rather than in sema.
        if (std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("async")) != modifiers.end()) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`async` is not admitted on a destructor — `async` is a "
                "method/lambda modifier (A.4 admits it in MethodDecl and "
                "LambdaExpr only; §30.1, v0.300)");
        }
        // per §22.15 (v0.307): `pure` is rejected on destructors —
        // disposal is the effect context purity fences off (the
        // same over-admit-and-reject-at-parse pattern as the `async`
        // check above; the DestructorDecl AST does not preserve the
        // modifiers vector).
        if (std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("pure")) != modifiers.end()) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`pure` is not admitted on a destructor — disposal is "
                "the effect context purity fences off (§22.15)");
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

    // §22.16 (v0.207, proposal 0055, #2078): A.4 `InvariantDecl ::=
    // "static"? "invariant" "(" Expression ")" ";"` — a class-body member
    // declaring an instance-state predicate. `invariant` is a contextual
    // keyword (App. B.2), lexed as an Identifier; it carries no Visibility
    // per grammar. The optional `static` qualifier was collected into
    // `modifiers` by the modifier loop above — extract it here.
    // `invariant` is matched BEFORE the FieldDecl fallback so an Identifier-
    // named "invariant" at member-position is the member, not a field name.
    if (current_.kind == TokenKind::Identifier &&
        current_.text == "invariant") {
        // A.4: InvariantDecl has no Visibility slot.
        if (visibility != ast::Visibility::None) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`invariant` member carries no visibility per A.4 (§22.16)");
        }
        // Sema validates the recorded modifiers carry at most `static`. Any
        // stray modifier (e.g. `owned`) is left for sema to reject, so the
        // AST captures the source text faithfully.
        const bool isStatic =
            std::find(modifiers.begin(), modifiers.end(),
                      std::string_view("static")) != modifiers.end();
        auto inv = std::make_unique<ast::InvariantDecl>();
        inv->isStatic = isStatic;
        inv->loc = current_.location;
        // fix #4449 (§22.16, v0.303): a `static invariant` clause is parsed
        // from day one and is INERT — neither rejected nor instrumented
        // (codegen's `Emitter::emitInvariantCheck` excludes `isStatic`
        // invariants by design) — until the contract-proving prover fragment
        // lands. §22.16's "Out of scope in this revision" rule names the
        // diagnostic obligation: "the compiler **must** emit a warning at
        // the clause naming that fact". Fires once at the clause's
        // declaration site (here, where the AST is built), on `static
        // invariant` only — plain `invariant` is unaffected. The warning is
        // WITHDRAWN (not silenced) when the prover fragment lands — the rule
        // states the condition itself, so the diagnostic cannot outlive its
        // reason. The wording is fixed by the issue for a citation trail;
        // minor punctuation paraphrases are not.
        if (isStatic) {
            diagnostics_.report(
                Severity::Warning, current_.location,
                "`static invariant` is not instrumented in this revision — "
                "the clause is parsed and otherwise inert until the "
                "contract-proving prover fragment lands (§22.16)");
        }
        advance();  // past `invariant`
        // §22.16 v0.207: structural parens around the predicate. The `;` is
        // the ordinary member terminator.
        if (!consumePunctuator("(")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected '(' to open an `invariant` predicate (§22.16)");
            // Recovery: still try to parse an expression so partial AST shape
            // survives; report the missing ')' at close-paren time below.
        }
        inv->expr = parseExpression();
        if (!consumePunctuator(")")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ')' to close an `invariant` predicate (§22.16)");
        }
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' to terminate an invariant member (§22.16)");
        }
        return std::unique_ptr<ast::Member>(inv.release());
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
    if (atDiscard()) {
        // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is a
        // compile error — `_` is a reserved token, not an Identifier.
        reportDiscardNotDeclarable("field");
        return nullptr;
    }
    // §9.2 / A.4 FieldDecl grammar (v0.298): the visibility+modifiers are
    // followed by an optional EqualityClause BEFORE the Identifier. The
    // `equality` contextual keyword is itself lexed as an Identifier, so a
    // first-glance "is this an Identifier" check would happily read it as
    // a (wrong) field name. We pre-recognize `equality` and consume it
    // here; the Identifier check that follows validates the next token as
    // the field name in either branch.
    const bool hadEqualityClause = isEqualityClauseStart();

    auto field = std::make_unique<ast::FieldDecl>();
    field->visibility = visibility;
    field->modifiers = std::move(modifiers);
    // §9.2 / A.4 FieldDecl: EqualityClause appears BEFORE the Identifier
    // (the unusual placement — `serialized` is never on an explicit
    // FieldDecl, so this slot is reserved for the equality package only).
    // Grammar: `Visibility "static"? ("owned" | "scoped")? "readonly"?
    //            EqualityClause? Identifier ":" Type (":=" Expression)? ";"`.
    if (hadEqualityClause) {
        field->equality = parseEqualityClause();
    }
    if (current_.kind != TokenKind::Identifier) {
        if (hadEqualityClause) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected field name after `equality skip` clause "
                "(A.4 FieldDecl)");
        } else {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected field name or member kind keyword");
        }
        return nullptr;
    }
    field->name = current_.text;
    advance();

    if (!consumePunctuator(":")) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected ':' after field name");
    }
    field->type = parseType();
    if (consumePunctuator(":=")) {
        // fix #2856 follow-up (v0.254): stamp the field initializer's span at
        // parse time, so the wire-side `fieldInitText` extractor in
        // ModuleMetadataBuilder.cpp:982 reads the FULL initializer expression
        // (including any `(` opening punctuator) — not just the inner
        // postfix's span. For an immediately-invoked lambda
        // `(lambda(x: int) => x + 1)(6)` as a field initializer, the postfix
        // CallExpr's span.begin is at `lambda` (per #3214's diagnostics rule
        // that the call span starts at the callee, NOT the call's opening
        // punctuator); without this stamping, the wire text would be
        // `lambda(x: int) => x + 1)(6)` — missing the opening `(` — and
        // re-parse would reject it ("the initializer text failed to
        // re-parse", §2.8.2 reject path, #2351). `initLoc` is captured here
        // before the inner postfix can move `current_` and overwrite it.
        const SourceLocation initLoc = current_.location;
        field->initializer = parseExpression();
        if (field->initializer) {
            field->initializer->span.begin = initLoc;
            // span.end is already set by the inner-most expression; do not
            // overwrite — the closing `)` of a call IS the end of the
            // initializer expression.
        }
    }
    if (!consumePunctuator(";")) {
        // v0.217 (§27.3, #2101): retired `(params) => body` spelling in a
        // field initializer.
        if (!checkRetiredExprSpelling() || !consumePunctuator(";"))
            diagnostics_.report(Severity::Error, current_.location,
                                "expected ';' after field declaration");
    }
    // fix #2418: FieldDecl's span was never stamped — SymbolCollection's
    // FieldSymbol::declLoc had nothing to read. Mirrors the ctor/dtor #662c
    // span convention.
    field->span.begin = memberStart;
    field->span.end = current_.location;

    // Reseat the raw pointer explicitly (Node's deleted move blocks the
    // implicit unique_ptr<Derived> -> unique_ptr<Base> conversion).
    return std::unique_ptr<ast::Member>(field.release());
}

std::unique_ptr<ast::Member> Parser::parseInterfaceMember() {
    // §A.4 InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer |
    // IfaceEvent. No visibility (interface members are implicitly public per
    // §10.1), signatures only. The pre-keyword prefix is `readonly?` (for
    // property), `async?` (for event), `pure?` (§22.15 v0.202 for
    // IfaceMethod/IfaceProperty/IfaceIndexer/IfaceStaticMethod), and — per
    // §11.8 (v0.176, #807) — `static` for the abstract-only static method /
    // static operator forms. All are scanned tentatively then dispatched on
    // the kind keyword.
    // fix #2418: record the member's first token so every branch below can
    // stamp `span.begin` — mirrors parseMember's `memberStart` (#662c/#783);
    // interface members never had ANY span before (SymbolCollection's
    // PropertySymbol/EventSymbol/MethodSymbol/ConstMemberSymbol::declLoc had
    // nothing to read for an interface-declared member).
    const SourceLocation memberStart = current_.location;
    bool readonlyMod = false;
    bool asyncMod = false;
    bool staticMod = false;
    bool pureMod = false;
    while (isKeyword("readonly") || isKeyword("async") ||
           isKeyword("static") ||
           (current_.kind == TokenKind::Identifier &&
            current_.text == "pure")) {
        if (isKeyword("readonly")) {
            readonlyMod = true;
        } else if (isKeyword("static")) {
            staticMod = true;  // per §11.8
        } else if (isKeyword("async")) {
            asyncMod = true;
        } else {
            pureMod = true;  // §22.15 (v0.202, #2052)
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
        // §22.15 (v0.202, #2052): IfaceProperty/IfaceIndexer admit `pure?`
        // before `readonly?` per A.4.
        if (pureMod) pd->modifiers.push_back("pure");
        if (consumePunctuator("[")) {
            pd->isIndexer = true;
            pd->indexerParams = parseParamList();
            if (!consumePunctuator("]")) {
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "expected ']' to close interface indexer parameters");
            }
        } else if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is a
            // compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("property");
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
        // fix #2418: see memberStart comment at the top of this function.
        pd->span.begin = memberStart;
        pd->span.end = current_.location;
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
        // §22.15 (v0.202, #2052): `pure` is not admitted on events (events
        // are the synchronization surface purity fences off).
        if (pureMod) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "`pure` is not admitted on an interface event (§22.15)");
        }
        auto ed = std::make_unique<ast::EventDecl>();
        ed->visibility = ast::Visibility::None;
        ed->isAsync = asyncMod;
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("event");
        } else if (current_.kind != TokenKind::Identifier) {
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
        // fix #2418: see memberStart comment at the top of this function.
        ed->span.begin = memberStart;
        ed->span.end = current_.location;
        return std::unique_ptr<ast::Member>(ed.release());
    }

    // IfaceMethod: shares the MethodDecl AST shape (signature + abstract body).
    if (consumeKeyword("method")) {
        auto m = std::make_unique<ast::MethodDecl>();
        m->visibility = ast::Visibility::None;
        // per §11.8 (v0.176, #807): a static interface method. Same signature
        // grammar; abstract-only (the `;` requirement below enforces no body).
        if (staticMod) m->modifiers.push_back("static");
        // §22.15 (v0.202, #2052): IfaceMethod/IfaceStaticMethod admit `pure?`
        // before `method` per A.4.
        if (pureMod) m->modifiers.push_back("pure");
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("method");
        } else if (current_.kind != TokenKind::Identifier) {
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
        // §22.16 (v0.207, proposal 0055, #2078): A.4 IfaceMethod's body
        // slot returns to the uniform `ContractClause* ";"` — every member
        // ends with `;`, clause-bearing or not. The clause region parses
        // `("(" Expression ")")` and consumes no `;` of its own.
        m->contractClauses = parseContractClauses();
        // §22.16 v0.207: the member-terminating `;` is unconditional.
        if (!consumePunctuator(";")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ';' after interface method signature");
        }
        m->bodyKind = ast::MethodBodyKind::Abstract;
        // fix #2418: see memberStart comment at the top of this function.
        m->span.begin = memberStart;
        m->span.end = current_.location;
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
        // fix #3629 (§22.15 / §11.8, v0.286): the pre-keyword prefix scanned
        // `pureMod` (line 1655) for every interface member kind. The other three
        // branches push it into a modifier list; IfaceOperator follows the
        // class-side `parseOperatorDeclBody` path and folds `pure` into the
        // dedicated `ast::OperatorDecl::isPure` bool — the channel `collectMember`
        // (SymbolCollection.cpp) and `ModuleMetadataBuilder::convertMember` both
        // read. Without this assignment the local symbol reads `isPure=false`,
        // the wire carries `MetaMember::isPure=false`, and an importer sees an
        // unpure operator even though the source declared it `pure`. The error
        // path for an interface event at `:1758` is intentionally distinct:
        // §22.15 rejects `pure` on events with a hard diagnostic.
        op->isPure = pureMod;
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
        // fix #2418: see memberStart comment at the top of this function.
        op->span.begin = memberStart;
        op->span.end = current_.location;
        return std::unique_ptr<ast::Member>(op.release());
    }

    diagnostics_.report(
        Severity::Error, current_.location,
        "expected 'method', 'property', 'event', or 'static "
        "method'/'static operator' (§11.8) in interface body");
    return nullptr;
}

bool Parser::isPropertyAccessorStart() {
    // §A.4 Accessor: `Visibility? "pure"? ("get" | "set")` opens an accessor
    // in a PropertyBody block form (v0.202, #2052 adds the `pure?` slot).
    // `get`, `set`, and `pure` are contextual keywords (App. B.2) so the
    // lexer emits them as Identifier — checked by text.
    auto isGetOrSet = [](const Token& t) {
        return t.kind == TokenKind::Identifier &&
               (t.text == "get" || t.text == "set");
    };
    auto isPure = [](const Token& t) {
        return t.kind == TokenKind::Identifier && t.text == "pure";
    };
    if (isGetOrSet(current_)) return true;
    if (isPure(current_) && isGetOrSet(peekAt(1))) return true;
    if (isKeyword("public") || isKeyword("private") ||
        isKeyword("internal") || isKeyword("protected")) {
        // `Visibility pure? get|set`
        if (isGetOrSet(peekAt(1))) return true;
        if (isPure(peekAt(1)) && isGetOrSet(peekAt(2))) return true;
    }
    return false;
}

ast::PropertyAccessor Parser::parsePropertyAccessor() {
    // §A.4 `Accessor ::= Visibility? "pure"? "get" AccessorBody`
    //                  | Visibility? "set" SetterParamSpec? GenericConstraints?
    //                      AccessorBody` (v0.202, #2052: `pure?` on get only).
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
    // §22.15 (v0.202, #2052): `pure` slot — admitted textually on both get
    // and set here; sema rejects `pure set`.
    if (current_.kind == TokenKind::Identifier &&
        current_.text == "pure") {
        acc.isPure = true;
        advance();
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
                 current_.text == "/="  || current_.text == "mod=" ||
                 current_.text == "&="  ||
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

// §9.2 / fix #4643 (v0.298): EqualityClause. Mirrors `serialized skip`'s
// grammar (App. A.3/A.4), but only the `skip` form exists — rename and
// `optional` have no equality analogue, so the clause is one word, closed
// (spec §9.2 line 1385). The caller checks `isEqualityClauseStart()`
// first (`equality` is contextual, App. B.2, lexed as Identifier; we
// identify by text exactly the way `serialized` does above).
bool Parser::isEqualityClauseStart() const {
    return current_.kind == TokenKind::Identifier &&
           current_.text == "equality";
}

std::unique_ptr<ast::EqualityClause> Parser::parseEqualityClause() {
    advance(); // past `equality`
    auto ec = std::make_unique<ast::EqualityClause>();
    // Only `skip` is admitted (line 1385); anything else is an error.
    if (current_.kind == TokenKind::Identifier && current_.text == "skip") {
        ec->isSkip = true;
        advance();
        return ec;
    }
    diagnostics_.report(
        Severity::Error, current_.location,
        "`equality` requires `skip` — rename and `optional` have no "
        "equality analogue (§9.2, A.3/A.4 EqualityClause; vocabulary is "
        "one word, closed)");
    return ec;
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
        // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before `)`.
        if (isPunctuator(")")) break;
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

// fix #4222 (§26.3 "Restrictions" / Appendix A.4 `PolymorphicMember` clause,
// v0.298): `parseExtensionDecl`'s member loop's own peek-and-discard for a
// `polymorphic(...)` clause. Deliberately NOT a call to
// `parsePolymorphicMember(ast::TypeDecl&)` above — that function's token-
// consumption logic was validated for a context that HAS a `TypeDecl` to
// mutate (`decl.serializedClass->polyCases`); an extension body has no
// `TypeDecl` at all, so calling it here would need a `TypeDecl&` to hand it
// that does not exist (LANE_RULES §25: ask what a helper was validated for,
// not what it looks like it does). Instead this walks the same token shape
// — `"polymorphic" "(" (StringLiteral ":" Type)? ("," (StringLiteral ":"
// Type))* ","? ")" ";"` — tracking only paren depth, and discards every
// piece: no `PolyCase`, no `Type` AST is built, because nothing here owns a
// place to put them. `depth` also survives a malformed clause without
// looping forever: the `!= EndOfFile` guard bounds it even if a `)` is
// missing, and a missing trailing `;` is simply not consumed (the outer
// member loop's own `else if (current_.kind != TokenKind::EndOfFile)
// advance();` fallback still makes progress on whatever comes next).
void Parser::parseAndDiscardExtensionPolymorphicMember() {
    const SourceLocation loc = current_.location;  // the `polymorphic` token
    advance();  // past `polymorphic`
    // fix #4264 (send-back on #4257, §26.3/A.4, v0.298): remember the `(`
    // token's own location and whether the discard scan below ever finds
    // its matching `)`, so an unclosed-paren clause can get a supplementary
    // note pointing at the unmatched `(` instead of leaving the user to
    // discover the clause is ALSO malformed only after relocating it.
    SourceLocation parenLoc{};
    bool sawOpenParen = false;
    bool parenMatched = false;
    if (isPunctuator("(")) {
        parenLoc = current_.location;
        sawOpenParen = true;
    }
    if (consumePunctuator("(")) {
        int depth = 1;
        // fix #4257 (send-back on #4222): also bound the scan by `end`, not
        // just EOF/depth-0, mirroring this loop's own caller
        // (`parseExtensionDecl`'s member loop condition is
        // `!isKeyword("end") && current_.kind != TokenKind::EndOfFile`).
        // Without this, an unclosed `(` — the single most likely real typo
        // here — made the depth-tracking scan consume the extension's own
        // `end extension;` terminator and every subsequent top-level
        // declaration to EOF (#4257's measured repro).
        //
        // This cannot misfire on well-formed input: A.4's
        // `PolymorphicMember ::= "polymorphic" "(" PolyCase ("," PolyCase)*
        // ","? ")" ";"` and `PolyCase ::= StringLiteral ":" QualifiedName`
        // (QualifiedName ::= Identifier ("." Identifier)*) admit only
        // StringLiteral / ":" / "," / "." / Identifier tokens (and the
        // closing ")") inside the parens — never a nested "(" and never a
        // bare `end`, because `end` is in the Lexer's reserved-keyword list
        // (`Lexer.cpp:113`) and so always lexes as `TokenKind::Keyword`,
        // never as an `Identifier` a `QualifiedName` could contain. So a
        // well-formed clause can never be at depth >= 1 when the current
        // token is the `end` keyword, and this bound is unreachable on the
        // happy path — verified structurally, not just by the existing
        // lit test staying green.
        while (depth > 0 && current_.kind != TokenKind::EndOfFile &&
               !isKeyword("end")) {
            if (isPunctuator("(")) {
                ++depth;
            } else if (isPunctuator(")")) {
                --depth;
                if (depth == 0) {
                    advance();  // consume the closing ')'
                    parenMatched = true;
                    break;
                }
            }
            advance();
        }
    }
    consumePunctuator(";");
    diagnostics_.report(
        Severity::Error, loc,
        "an extension block may not declare a `polymorphic` member "
        "registry — Appendix A.4's `ExtensionMember` grammar admits only "
        "methods and expression-bodied properties (§26.3)");
    // fix #4264: `sawOpenParen && !parenMatched` means the discard scan
    // above exited via #4257's `isKeyword("end")`/EOF bound rather than by
    // finding the matching `)` — i.e. the clause's own parens are
    // unbalanced, independently of the §26.3 placement rejection above. A
    // user who follows the §26.3 diagnostic's evident fix (move the clause
    // onto the extended type's own class body, where `polymorphic(...)` is
    // legal) would otherwise hit this unrelated-looking parse error only
    // AFTER making that move, with nothing at the original site to hint the
    // clause itself is also malformed on its own terms.
    if (sawOpenParen && !parenMatched) {
        diagnostics_.report(Severity::Note, parenLoc,
                             "`(` opened here has no matching `)`");
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

    if (atDiscard()) {
        // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
        // a compile error — `_` is a reserved token, not an Identifier.
        reportDiscardNotDeclarable("method");
    } else if (current_.kind != TokenKind::Identifier) {
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

    // §22.16 (v0.207, proposal 0055, #2078): the contract-clause region —
    // zero or more `requires`/`ensures` clauses between the signature and the
    // body (A.4 MethodDecl's `ContractClause*` slot). The marker-wins rule
    // (App. B.2) means a `requires`/`ensures` Identifier here is always a
    // clause marker, never an expression operand.
    m->contractClauses = parseContractClauses();

    // A.4 MethodBody: `=> (Expression | AssignmentStmt) ";"` | Block | ";"
    // (v0.160: AssignmentStmt alternative in the `=>` body; valid only for
    // unit methods, i.e. no declared return type — enforced semantically, §22.2.)
    // §22.16 v0.207: a clause-bearing member is no longer body-less. Each
    // `ContractClause` is self-delimiting (its structural parens close it),
    // so the same `=>`/Block/`;` body dispatch applies at any clause count.
    // The §22.16 collision remedy (#2053's "stray `;` after clauses"
    // ambiguity) is gone by construction — a double `;` is unspellable.
    if (consumePunctuator("=>")) {
        // Parse the leading expression first (LHS of potential assignment).
        auto lhs = parseExpression();
        const bool isAssignOp =
            current_.kind == TokenKind::Punctuator &&
            (current_.text == ":="  || current_.text == "+="  ||
             current_.text == "-="  || current_.text == "*="  ||
             current_.text == "/="  || current_.text == "mod=" ||
             current_.text == "&="  ||
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
    ast::Visibility visibility, std::vector<std::string_view> modifiers,
    SourceLocation memberStart) {
    // A.4 `OperatorDecl ::= "public" "static" "pure"? "method" "operator"
    //                        OpToken "(" ParamList ")" ":" "owned"? Type
    //                        OperatorBody` (v0.202, #2052). Caller has consumed
    // `method operator`.
    auto op = std::make_unique<ast::OperatorDecl>();
    op->visibility = visibility;
    // §22.15: `pure` is admitted on operator declarations; `static` is
    // grammar-required and is not consumed here (sema checks).
    op->isPure =
        std::find(modifiers.begin(), modifiers.end(),
                  std::string_view("pure")) != modifiers.end();
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
    // fix #2418: OperatorDecl's span was never stamped — SymbolCollection's
    // MethodSymbol::declLoc (operators reuse MethodSymbol) had nothing to
    // read for the class-body operator form. `memberStart` defaults to {}
    // when a caller has none to give (matching parseMethodDeclBody's
    // convention); the class-body call site (parseMember) does supply it.
    op->span.begin = memberStart;
    op->span.end = current_.location;
    return std::unique_ptr<ast::Member>(op.release());
}

std::vector<ast::Param> Parser::parseParamList() {
    // A.4 `Param ::= ParamModifiers? Identifier ":" Type (":=" Expression)?`
    // ParamModifiers ∈ { ref | out owned? | owned }.
    std::vector<ast::Param> params;
    if (isPunctuator(")")) return params;

    auto parseOne = [&]() -> ast::Param {
        ast::Param p;
        // fix #2482: stamp the parameter's own location at its first token
        // (the optional modifier, else the name). Meaningful for the
        // member-shaped Param uses — a class primary-ctor param becomes a
        // positional FieldSymbol — and harmless for ordinary method params.
        p.loc = current_.location;
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
                   (peekAt(1).kind == TokenKind::Identifier ||
                    peekAt(1).kind == TokenKind::Discard)) {
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
        if (atDiscard()) {
            // A.4 Param (v0.244, #3049, accepted Q2): "A method/constructor
            // parameter cannot be named `_` — the bare token is not an
            // Identifier (A.1) … parameter names are call-site-addressable API
            // surface (§22.5, §2.8.2); choose a name (§17.13). Lambda
            // parameters differ: LambdaParam (A.7) admits the bare-`_`
            // ignore." The asymmetry is the whole point of Q2.
            reportDiscardNotDeclarable(
                "parameter",
                "parameter names are call-site-addressable API surface carried "
                "by the artifact interface (§22.5, §2.8.2), so choose a name — "
                "a LAMBDA parameter differs: `lambda(_) => …` is the true "
                "ignore (§27.3)");
            p.name = "_";  // recovery: keep parsing this parameter
        } else if (current_.kind != TokenKind::Identifier) {
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
        // fix issue 3027 (§15.4.1, v0.240): admit `Identifier ":" "owned"?
        // Type`. The owning-form `FromCollectionLiteral` spec example puts
        // `owned` AFTER the colon (`elements: owned OwningArray<T>`), but
        // A.4 ParamModifiers is only declared before the identifier. The
        // post-colon form is semantically identical to the modifier-before
        // form (`owned elements: OwningArray<T>`): both mark the parameter
        // as `owned` for the existing modifier=="owned" sema path.
        // Mirrors the `owned?` already admitted on return types
        // (MethodDecl / OperatorDecl / IfaceMethod, A.4 grammar lines
        // ~1042/1037/10299).
        if (p.modifier.empty() && consumeKeyword("owned")) {
            p.modifier = "owned";
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
        // per §1.6 / proposal 0059 (v0.214): an optional trailing `,` before
        // the closer. ParamList is reused for indexer params (closer `]`) as
        // well as method/primary-ctor params (closer `)`); accept any
        // punctuator that cannot begin a Parameter.
        if (current_.kind == TokenKind::Punctuator &&
            (current_.text == ")" || current_.text == "]" ||
             current_.text == "}" || current_.text == ";")) {
            break;
        }
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
                    // v0.244: the reserved `_` too, so `scoped _` reaches the
                    // ArgExpression rejection below with its own diagnostic
                    // instead of reading `scoped` as the argument value.
                    peekAt(1).kind == TokenKind::Discard ||
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
        // A.4 `ArgExpression ::= Expression | "_"`. Since v0.244 the bare `_`
        // is its own reserved token, so the alternative needs no lookahead
        // disambiguation — "no Expression contains the bare token (A.1)".
        // §17.13 sanctions it only under the `out` modifier: "the discard
        // composes with `out` only: **`ref _` is a compile error**"; the
        // inline-declaration modifiers (`out var` / `out owned var` /
        // `out using var`) declare a local, and `_` is never declarable.
        if (atDiscard()) {
            const SourceLocation discardLoc = current_.location;
            if (a.modifier == "ref") {
                diagnostics_.report(
                    Severity::Error, discardLoc,
                    "`ref _` is a compile error — a `ref` argument reads its "
                    "operand, which a discard cannot supply; pass a named "
                    "location, or use `out _` if the parameter is pure output "
                    "(§17.13, §22.3)");
            } else if (a.modifier == "out var" ||
                       a.modifier == "out owned var" ||
                       a.modifier == "out using var") {
                diagnostics_.report(
                    Severity::Error, discardLoc,
                    "`" + std::string(a.modifier) +
                    " _` is a compile error — it would declare nothing; write "
                    "`out _`, which already means exactly this (§17.13, "
                    "§22.4)");
            } else if (a.modifier != "out") {
                // `owned _` / `scoped _` mark an operand that is READ (moved
                // from, or lent), which a discard cannot supply; an UNMODIFIED
                // `_` argument is a plain by-value operand, i.e. a read of a
                // name that binds nothing. Both are "every other position" in
                // §17.13's out-argument rule. (Unmodified `_` previously
                // reached codegen and emitted malformed IR — an int formal
                // received the address of the compiler's scratch slot.)
                diagnostics_.report(
                    Severity::Error, discardLoc,
                    (a.modifier.empty()
                         ? std::string("`_`")
                         : "`" + std::string(a.modifier) + " _`") +
                    " is a compile error in argument position — the discard "
                    "`_` is sanctioned there only under the `out` modifier, "
                    "where receiving-and-ignoring a value is coherent "
                    "(§17.13, §22.3)");
            }
            a.isDiscard = true;
            advance();
            if (isPunctuator(":")) {
                // §17.13 / §22.4: "A type annotation on `out _` is not
                // permitted (`out _: int` is a syntax error) … a bare ignore
                // takes its type from context, never an annotation."
                diagnostics_.report(
                    Severity::Error, current_.location,
                    "a type annotation is not permitted on the discard `_` — "
                    "a bare ignore takes its type from context, never an "
                    "annotation (§17.13, §22.4)");
                advance();     // past `:`
                parseType();   // consume it for recovery
            }
        } else {
            a.value = parseExpression();
            // v0.217 (§27.3, #2101): `Foo((x) => …)` — a retired-spelling
            // lambda argument parses its head as a parenthesized expression
            // and leaves the `=>` here (where `,` or the closer is
            // expected); name the migration instead of a generic error.
            checkRetiredExprSpelling();
        }
        return a;
    };
    args.push_back(parseOne());
    while (consumePunctuator(",")) {
        // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before
        // the closer. The ArgList production is also reused for IndexExpr
        // (closer `]`) and NullCondCallExpr (closer `)`), so accept any
        // punctuator that cannot begin an Argument expression (`)` / `]`
        // / `}` / `;` / `,`).
        if (current_.kind == TokenKind::Punctuator &&
            (current_.text == ")" || current_.text == "]" ||
             current_.text == "}" || current_.text == ";" ||
             current_.text == ",")) {
            break;
        }
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
        // fix #2482: stamp the field's own location at its first token (the
        // name) so a member diagnostic points at the field, not the record.
        p.loc = current_.location;
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("positional field");
        } else if (current_.kind != TokenKind::Identifier) {
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
        if (isEqualityClauseStart()) {
            p.equality = parseEqualityClause();
        }
        return p;
    };
    fields.push_back(parseOne());
    while (consumePunctuator(",")) {
        // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before `)`.
        if (isPunctuator(")")) break;
        fields.push_back(parseOne());
    }
    return fields;
}

// §A.3 / §35.1 (v0.200, #2044):
//   CasePayload ::= "(" CaseField ("," CaseField)* ")"
//   CaseField   ::= "owned"? Identifier ":" Type SerializedClause? EqualityClause?
// CaseField is PositionalField plus the optional leading `owned` payload
// marker; the marker is stored in `Param::modifier` (semantically the same
// slot `owned` parameters use, per §35.3 / §28.3).
std::vector<ast::Param> Parser::parseCasePayloadList() {
    std::vector<ast::Param> fields;
    if (isPunctuator(")")) return fields;
    auto parseOne = [&]() -> ast::Param {
        ast::Param p;
        // fix #2482: stamp the field's own location at its first token (the
        // optional `owned` marker, else the name).
        p.loc = current_.location;
        // Optional leading `owned` marker.
        if (isKeyword("owned")) {
            p.modifier = "owned";
            advance();
        }
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("case payload field");
        } else if (current_.kind != TokenKind::Identifier) {
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
        if (isEqualityClauseStart()) {
            p.equality = parseEqualityClause();
        }
        return p;
    };
    fields.push_back(parseOne());
    while (consumePunctuator(",")) {
        // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before `)`.
        if (isPunctuator(")")) break;
        fields.push_back(parseOne());
    }
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
            // A.3 Constraint ::= "class" | "value" | "notnull" | "sendable"
            //                  | "hashable" | "new" "(" ")" | Type.
            // `class` is reserved; `value`, `notnull`, `sendable`, and
            // `hashable` are contextual (App. B.2) so the lexer produces
            // Identifier tokens. `sendable` names the §30.14 sendable
            // derived-property constraint; `hashable` names the §3.6
            // hashable derived-property constraint (the keyword names
            // the capability site, the property keeps its §3.6 name) —
            // v0.214, §14.2 "Derived-property constraints".
            if (consumeKeyword("class")) {
                item.kind = "class";
            } else if (current_.kind == TokenKind::Identifier &&
                       (current_.text == "value" ||
                        current_.text == "notnull" ||
                        current_.text == "sendable" ||
                        current_.text == "hashable")) {
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
            // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before
            // the end of the where-clause region. A ConstraintItem starts with
            // the keyword `class` / `new`, or an Identifier (a contextual
            // constraint name like `value` / `notnull` / `sendable` /
            // `hashable`, or a type name). Anything else — the next `where`
            // keyword (next clause), the body opener `{`, the record-shorthand
            // terminator
            // `;`, an expression-body arrow `=>`, a member visibility
            // (`public`/`private`/`protected`/`internal`) or member-kind
            // keyword — ends the region.
            const bool cantStartConstraint =
                (current_.kind == TokenKind::Punctuator) ||
                (current_.kind == TokenKind::Keyword &&
                 current_.text != "class" && current_.text != "new");
            if (cantStartConstraint) break;
            clause.items.push_back(parseConstraintItem());
        }
        all.push_back(std::move(clause));
    }
    return all;
}

// §22.16 (v0.203, #2053): does the current token open a contract clause?
// `requires` and `ensures` are contextual keywords (App. B.2) lexed as
// Identifiers; in the clause region (after the signature's GenericConstraints
// and before the body) their keyword reading always wins, per App. B.2's
// explicit rule. The `static` qualifier is a true reserved keyword, so it is
// matched via isKeyword(); a `static` here is admitted ONLY as the prefix of
// a `static requires` / `static ensures` clause (no other `static` use is in
// this position — the member-modifier slot was consumed before the signature).
bool Parser::currentTokenStartsContractClause() {
    // `requires …` / `ensures …` (contextual keyword — Identifier by text).
    if (current_.kind == TokenKind::Identifier &&
        (current_.text == "requires" || current_.text == "ensures"))
        return true;
    // `static requires …` / `static ensures …` — two-token lookahead. `static`
    // is unconditionally reserved, so this is unambiguous.
    if (isKeyword("static")) {
        const Token& next = peekAt(1);
        if (next.kind == TokenKind::Identifier &&
            (next.text == "requires" || next.text == "ensures"))
            return true;
    }
    return false;
}

std::vector<ast::ContractClause> Parser::parseContractClauses() {
    // A.4 (v0.207, proposal 0055, #2078):
    //   `ContractClause ::= "static"? ("requires" | "ensures") "(" Expression ")"`
    // The clause region is zero or more clauses. The marker-wins rule means a
    // `requires`/`ensures` Identifier here is always a clause marker, never an
    // expression operand (App. B.2 — the keyword reading wins). Each clause's
    // parens are CLOSE-WHEN-BALANCED (the expression cannot span a statement),
    // so the clause is self-delimiting and the parser carries no clause-level
    // `;` here — the member's terminating `;` lives in the body dispatch.
    std::vector<ast::ContractClause> out;
    while (currentTokenStartsContractClause()) {
        ast::ContractClause c;
        c.loc = current_.location;
        if (isKeyword("static")) {
            c.isStatic = true;
            advance();  // past `static`
        }
        const bool isRequires =
            (current_.kind == TokenKind::Identifier &&
             current_.text == "requires");
        const bool isEnsures =
            (current_.kind == TokenKind::Identifier &&
             current_.text == "ensures");
        if (!isRequires && !isEnsures) {
            // Defensive — currentTokenStartsContractClause() guards this.
            break;
        }
        c.kind = isRequires ? ast::ContractClause::Kind::Requires
                            : ast::ContractClause::Kind::Ensures;
        advance();  // past `requires` / `ensures`
        if (!consumePunctuator("(")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected '(' to open a contract clause (§22.16)");
            // Recovery: do not push a partially-formed clause — leave the
            // outer body dispatch to surface its own diagnostics on whatever
            // token follows.
            break;
        }
        c.expr = parseExpression();
        if (!consumePunctuator(")")) {
            diagnostics_.report(
                Severity::Error, current_.location,
                "expected ')' to close a contract clause (§22.16)");
            // Recovery: still record the clause; further clauses' parsing is
            // best-effort.
            out.push_back(std::move(c));
            break;
        }
        out.push_back(std::move(c));
    }
    return out;
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
        if (atDiscard()) {
            // §1.3 / §17.13 (v0.244, #3049): every declaration named `_` is
            // a compile error — `_` is a reserved token, not an Identifier.
            reportDiscardNotDeclarable("type parameter");
            return p;
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
        // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before the
        // closing angle. consumeClosingAngle below handles the `>` / `>>` /
        // `>=` / `>>=` lexer tokens that may close a nested generic list.
        if (current_.kind == TokenKind::Punctuator &&
            !current_.text.empty() && current_.text.front() == '>') {
            break;
        }
        params.push_back(parseOne());
    }
    if (!consumeClosingAngle()) {
        diagnostics_.report(Severity::Error, current_.location,
                            "expected '>' to close generic parameter list");
    }
    return params;
}

std::vector<std::unique_ptr<ast::Type>> Parser::parseInheritance(
    const ast::TypeDecl* decl) {
    // A.3: `Inheritance ::= ":" InheritItem ("," InheritItem)*`.
    // `InheritItem ::= Type ("(" ArgList? ")")?`.
    //
    // fix #2455 (§11.1 / §22.10 / §12.8, v0.235): this function is reached
    // for interface decls, record decls, and — via the caller's
    // `!(kind==Class && hasPositionalFields)` branch — for CLASS decls that
    // have NO primary-constructor parameter list of its own. Previously an
    // argument list here was silently tokenized away with no AST record and
    // no diagnostic: `class D: Base(99)` compiled and ran `Base()` with no
    // arguments, silently dropping the 99 (issue #2455's confirmed repro,
    // LOCAL bases included — not cross-module-specific).
    //
    // The spec ties the header argument-list shorthand exclusively to a
    // primary constructor: §11.1 "If the class has a base class that
    // requires constructor arguments, the base's arguments appear in
    // parentheses after the base class name in the inheritance list" is
    // stated within the "Primary constructors" subsection, and §22.10 says
    // the sugar "replaces an explicit base(...) call" that a primary
    // constructor would otherwise need. For a class with NO primary
    // constructor and no explicit constructor, §12.8 gives the general,
    // unconditional rule instead: "If neither base(...) nor this(...) is
    // written, the compiler inserts an implicit base() call" — a
    // NO-ARGUMENT call, with no provision anywhere for a header argument
    // list to feed it. Confirmed against the existing (correct, tested)
    // implementation too: `class D(): Base(99)` — an explicit EMPTY primary-
    // ctor parameter list — already threads 99 through
    // `decl->primaryBaseCtorArgs` (the `hasPositionalFields` branch above)
    // and runs correctly; only the bare `class D: Base(99)` spelling (no
    // primary ctor at all) was silently accepting and discarding. So a
    // class-header argument list without an owning primary constructor has
    // no defined destination — reject it rather than silently drop it
    // (CLAUDE.md rule 4: silently picking a semantics for an unspecified
    // shape is the forbidden option; here the simplest SOUND reading is
    // rejection, since inventing "which constructor does this bind to" for
    // a class that may declare several independently-chaining explicit
    // constructors has no textual answer).
    //
    // fix #2466 (Appendix A.3 InheritItem grammar note, §9.6, §11.2,
    // v0.236): record and interface decls reach this same function and
    // previously kept the original tokenize-away behaviour. Every item in
    // their inheritance list is an interface (records cannot inherit a
    // class at all, other than the implicit `Object` root, per §9.6;
    // interfaces only extend other interfaces, per §11.2), so an argument
    // list there is inert nonsense rather than silently-dropped DATA
    // (nothing was ever going to consume it) — but Appendix A.3's own note
    // on `InheritItem` documents the restriction ("permitted only on the
    // base class ... not on interfaces"), so the silent tokenize-away is
    // now upgraded to a diagnostic for these kinds too. Only an EMPTY arg
    // list (`IFoo()`) stays accepted, matching #2455's "no data lost"
    // carve-out and the pre-existing
    // parser_generics_inheritance.inheritance_with_unparsed_arg_list_
    // does_not_error regression test's expectation for that shape.
    const bool classNoPrimaryCtor = decl != nullptr &&
        decl->kind == ast::TypeDeclKind::Class;
    const bool recordOrInterface = decl != nullptr &&
        (decl->kind == ast::TypeDeclKind::Record ||
         decl->kind == ast::TypeDeclKind::Interface);
    std::vector<std::unique_ptr<ast::Type>> items;
    if (!isPunctuator(":")) return items;
    advance(); // past ':'

    // `itemIndex` distinguishes the base-class position (0) from an
    // interface position (>0) for the diagnostic wording — per the grammar
    // note on `InheritItem` (Appendix A.3), an argument list is syntactically
    // admitted on every item but semantically meaningful only on the first.
    std::size_t itemIndex = 0;
    auto parseOne = [&]() -> std::unique_ptr<ast::Type> {
        const std::size_t thisIndex = itemIndex++;
        auto t = parseType();
        if (isPunctuator("(")) {
            const SourceLocation argListLoc = current_.location;
            // An EMPTY arg list (`Base()`) drops no data — it supplies zero
            // arguments, exactly what §12.8's implicit `base()` call already
            // does for a class with neither a primary nor an explicit
            // constructor. Only a NON-empty list silently discards a real
            // value (the issue's actual bug), so only that is rejected;
            // `Base()` keeps parsing as a harmless no-op (regression guard:
            // parser_generics_inheritance.inheritance_with_unparsed_arg_list_
            // does_not_error already asserts this stays accepted).
            const bool argListNonEmpty = peekNext().kind != TokenKind::Punctuator ||
                                          peekNext().text != ")";
            if (argListNonEmpty) {
                if (classNoPrimaryCtor && thisIndex == 0) {
                    // #2455: class without a primary constructor, base-class
                    // position — the arg list has no defined destination.
                    diagnostics_.report(
                        Severity::Error, argListLoc,
                        "class `" + std::string(decl->name) + "` supplies a "
                        "base-constructor argument list here, but `" +
                        std::string(decl->name) + "` declares no primary-"
                        "constructor parameter list of its own — the header "
                        "argument-list shorthand feeds a primary "
                        "constructor's `base(...)` chain (§11.1, §22.10, "
                        "v0.236), and a class without one gets an implicit "
                        "no-argument `base()` call instead (§12.8, v0.236) "
                        "that cannot receive these arguments; add an empty "
                        "primary-constructor parameter list (`" +
                        std::string(decl->name) + "()`) or declare an "
                        "explicit constructor that chains via `base(...)`");
                } else if (classNoPrimaryCtor || recordOrInterface) {
                    // #2455 (class interface positions, item 1+) and #2466
                    // (every record/interface item, all interface positions
                    // per §9.6 / §11.2): Appendix A.3's `InheritItem` note
                    // admits the arg list only on the base class, not on
                    // interfaces.
                    diagnostics_.report(
                        Severity::Error, argListLoc,
                        "an argument list is not permitted here — per "
                        "Appendix A's `InheritItem` grammar (v0.236), it is "
                        "permitted only on the base class (the first item "
                        "in the inheritance list), not on an interface");
                }
            }
            // Skip a balanced parenthesised group without parsing its contents.
            int depth = 1;
            advance();
            // fix #4343 (same §26.3-analogous discard-loop class as #4257 /
            // #4341, Appendix A.3, v0.298): bound this discard scan by
            // `isKeyword("end")` in addition to depth/EOF. Without this
            // bound, an unclosed `(` on a non-first `InheritItem` here (this
            // function serves record/interface decls and classes with no
            // primary constructor — the `hasPositionalFields` branch above
            // has its own, now-identically-hardened copy of this same loop)
            // consumed the type's own `end TypeName;` terminator and every
            // subsequent top-level declaration to EOF. Cannot misfire on
            // well-formed input for the same reason as those two fixes:
            // Appendix A's `ArgList` grammar admits only expression tokens
            // inside the parens, and `end` is a reserved keyword
            // (`Lexer.cpp`'s `isReservedKeyword`), so it can never appear
            // inside a well-formed argument list.
            while (depth > 0 && current_.kind != TokenKind::EndOfFile &&
                   !isKeyword("end")) {
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
        // per §1.6 / proposal 0059 (v0.214): optional trailing `,` before the
        // end of the inheritance region. The region's terminator is the first
        // token that cannot begin a Type — the next `where`-clause region
        // (`where`), the body opener (`{`), the record-shorthand terminator
        // (`;`), or a member-starter keyword (`public`, `private`, `protected`,
        // `internal`, `static`, `owned`, `readonly`, `async`, `abstract`,
        // `virtual`, `sealed`, `override`, `method`, `constructor`,
        // `destructor`, `property`, `event`, `const`, `operator`, `pure`).
        // Practically: a Keyword that isn't a primitive-type keyword, or a
        // Punctuator that isn't `(` / `[` (which begin type expressions).
        const bool cantStartType =
            (current_.kind == TokenKind::Keyword &&
             !isPrimitiveTypeKeyword(current_.text)) ||
            (current_.kind == TokenKind::Punctuator &&
             current_.text != "(" && current_.text != "[");
        if (cantStartType) break;
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
    if (atDiscard()) {
        // §1.3 / §17.13 (v0.244, #3049): a module name (and every other
        // qualified-name component) is a name; `_` is never one. §17.13's
        // rejected list names "module" explicitly.
        reportDiscardNotDeclarable("module or qualified-name component");
        return name;
    }
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
