#pragma once

// Abstract syntax tree node definitions. The full node hierarchy is built out
// per the grammar in Appendix A and the per-construct rules across §9–§27, by
// each parser milestone slice. Right now this file holds only the source-file
// and module-declaration shapes (parser slice 1) — imports and top-level
// declarations are added by their own slices.

#include <memory>
#include <string_view>
#include <vector>

#include "support/SourceLocation.hpp"

namespace bezel::ast {

// Base of every AST node. Polymorphic; concrete subclasses each carry their
// own fields, plus the source span inherited from here.
struct Node {
    SourceSpan span;

    Node() = default;
    virtual ~Node() = default;

    // Nodes own their children via unique_ptr, so copying makes no sense
    // (and would slice the polymorphic type). Move is also disabled to keep
    // the hierarchy strictly tree-shaped.
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;
};

// A.2: `QualifiedName ::= Identifier ("." Identifier)*`
//
// Used by `module` and `import` declarations (§2.1, §2.2) and by qualified
// type references later on. `parts` holds the identifier lexemes in order
// (e.g. for `BankingApp.Transactions`, parts = {"BankingApp",
// "Transactions"}); the dotted form is a single name (per §2.1), not nesting.
struct QualifiedName : Node {
    std::vector<std::string_view> parts;
};

// A.2: `ModuleDecl ::= "module" QualifiedName ";"`  (§2.1)
struct ModuleDecl : Node {
    std::unique_ptr<QualifiedName> name;
};

// A.2: `Import ::= "import" QualifiedName ";"`  (§2.2)
struct Import : Node {
    std::unique_ptr<QualifiedName> name;
};

// §13 visibility keywords; `None` means the declaration carries no explicit
// visibility (the default is type-context-dependent per §13).
enum class Visibility {
    None,
    Public,
    Private,
    Internal,
    Protected,
};

// Distinguishes the four class-like top-level declarations parsed in slice 3.
// Extension declarations (§26), type aliases (§A.2 TypeAliasDecl), and
// extern blocks (§32) get their own AST nodes in their own slices.
enum class TypeDeclKind {
    Class,     // §11
    Interface, // §11.x / §A.3 InterfaceDecl
    Record,    // §9
    Enum,      // §10
};

// Common base for the grammar's `TopDecl` alternatives (A.2). Concrete
// subclasses follow.
struct TopDecl : Node {};

struct Type; // defined after TypeDecl; used by TypeDecl::baseTypes.
struct Expr; // defined further below; used by FieldDecl::initializer.

// §11.1 / #873: forward-declared here so TypeDecl::primaryBaseCtorArgs can use
// it. The full definition appears below (after Expr is defined in full).
// A.4 / §A.5 Argument: `ArgModifiers? (Identifier ":=")? ArgExpression`
struct CallArgument {
    std::string_view modifier;  // "", "ref", "out", "out var",
                                // "out using var", "out owned", "owned"
    std::string_view name;      // "" when no `Identifier :=` named-arg prefix
    bool isDiscard = false;
    std::unique_ptr<Expr> value; // null when isDiscard
};

// A.3 `GenericParam ::= Variance? Identifier`. Used by both type declarations
// (via TypeDecl::genericParams) and methods (via MethodDecl::genericParams);
// declared up here so MethodDecl can reference it before TypeDecl is defined.
struct GenericParam {
    std::string_view variance; // "in", "out", or empty
    std::string_view name;
};

// Common base for type members (FieldDecl, MethodDecl, PropertyDecl, …).
// Slice 6 introduces FieldDecl only; other kinds arrive with their slices.
struct Member : Node {};

// A.4: `FieldDecl ::= Visibility "static"? "owned"? "readonly"?
//                     Identifier ":" Type (":=" Expression)? ";"`.
struct FieldDecl : Member {
    Visibility visibility = Visibility::None;
    std::vector<std::string_view> modifiers; // "static", "owned", "readonly"
    std::string_view name;
    std::unique_ptr<Type> type;
    std::unique_ptr<Expr> initializer; // null if no `:= Expr` was written
};

// Base for §A.6 Statement nodes. Slice 11 introduces ReturnStmt,
// ExpressionStmt, LocalDecl, AssignmentStmt. Control-flow statements
// (`if`/`while`/`for`/`do`/`try`), `throw`, `break`/`continue`/`yield`,
// discard, and increment/decrement land in their own slices.
struct Stmt : Node {};

// A.6: `LocalDecl ::= "var" Identifier (":" Type)? (":=" Expression)? ";"`
//                  | `"using" "var" Identifier (":" Type)? ":=" Expression ";"`
//                  | `"await" "using" "var" …`
// All three forms share this AST node; modifier bits distinguish them.
// `isAwaitUsing` implies `isUsing` (the latter stays set for code that wants
// to ask the disposing-form question without caring about sync vs async).
struct LocalDecl : Stmt {
    bool isUsing = false;       // `using var` (sync) OR `await using var`
    bool isAwaitUsing = false;  // `await using var` form (implies isUsing)
    // §28.3 v0.150 (#1008 Part B, affine auto-drop): set by the ownership pass
    // on a plain `var` owned local that reaches a normal-exit scope end still
    // owned and is never discharged on ANY path (so it is a never-moved genuine
    // owner — safe to dispose unconditionally). Codegen routes it through
    // registerUsingDisposal (the same scope-end disposal a `using var` gets)
    // instead of the unwind-only path. A conditionally-discharged local is NOT
    // flagged (auto-disposing its moved/disposed path would double-free).
    bool autoDropAtScopeEnd = false;
    // §28.10 (#1285): set by the ownership pass on a `using var` whose bare
    // identifier initializer resolves to a BORROW (a borrow local aliasing a
    // borrow, or a borrow parameter) rather than an owned transfer. Codegen
    // (classOfLocal / registerUsingDisposal) skips the scope-end disposal for
    // such a using var — disposing a borrowed value here would double-free the
    // value its real owner also disposes (the borrow-LOCAL sibling of the
    // #1274 borrow-parameter exclusion, which codegen can detect on its own).
    bool usingVarSourceIsBorrow = false;
    std::string_view name;
    std::unique_ptr<Type> type;        // null when no `: Type`
    std::unique_ptr<Expr> initializer; // null only when plain `var x;`
};

// A.6 `LocalConstDecl ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"`.
// Statement-position const declaration (no visibility, no static — those
// belong to the member-level ConstDecl in A.4). Slice 53.
struct LocalConstDecl : Stmt {
    std::string_view name;
    std::unique_ptr<Type> type;        // null when no `:` annotation
    std::unique_ptr<Expr> value;
};

// A.6: `ReturnStmt ::= "return" Expression? ";"`.
struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value;       // null for `return;`
};

// A.6: `ExpressionStmt ::= Expression ";"`. Statement-position expression
// whose value is discarded (call, increment, …).
struct ExpressionStmt : Stmt {
    std::unique_ptr<Expr> expr;
};

// A.6: `AssignmentStmt ::= AssignTarget AssignOp Expression ";"`. The
// `target` is parsed as an expression in slice 11; sema decides which
// expression shapes are valid lvalues.
struct AssignmentStmt : Stmt {
    std::unique_ptr<Expr> target;
    std::string_view op;               // ":=", "+=", "-=", "*=", …
    std::unique_ptr<Expr> value;
};

// A.6: `ThrowStmt ::= "throw" Expression? ";"`.
struct ThrowStmt : Stmt {
    std::unique_ptr<Expr> value;
};

// A.6: `IfStmt ::= "if" Expression "then" Statement*
//                  ("elif" Expression "then" Statement*)*
//                  ("else" Statement*)? "end" ";"`
struct IfStmt : Stmt {
    struct Branch {
        std::unique_ptr<Expr> condition;
        std::vector<std::unique_ptr<Stmt>> body;
    };
    Branch ifBranch;
    std::vector<Branch> elifBranches;
    bool hasElse = false;
    std::vector<std::unique_ptr<Stmt>> elseBody;
};

// A.6: `WhileStmt ::= "while" Expression Statement* "end" ";"`.
struct WhileStmt : Stmt {
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
};

// A.6: `ForStmt ::= "for" "var" Identifier (":" Type)? "in" Expression
//                    "do" Statement* "end" ";"
//                  | "await" "for" "var" Identifier (":" Type)? "in"
//                    Expression ("with" Expression)? "do" Statement*
//                    "end" ";"`
// Slice 18 covers the synchronous form; the async `await for ... with` form
// landed later (slice 35). The `withExpr` is the optional cancellation token
// after the iteration source per §30.7.
struct ForStmt : Stmt {
    bool isAwait = false;                // `await for ...`
    std::string_view varName;
    std::unique_ptr<Type> varType;       // null when no `: Type` annotation
    std::unique_ptr<Expr> iterable;
    std::unique_ptr<Expr> withExpr;      // null when no `with EXPR` clause
    std::vector<std::unique_ptr<Stmt>> body;
    // §29.2 / §28.3 v0.157 (#1329): set by the ownership pass when the owned
    // iteration variable (over OwnedEnumerable<T> / OwnedAsyncEnumerable<T>)
    // is undischarged on at least one body-exit path. Codegen reads this flag
    // to register a per-iteration null-guarded disposal at body-end (fall-
    // through / break / continue / return edges) so the variable is auto-
    // dropped each iteration with the must-use warning rather than a hard error.
    bool ownedIterVarAutoDropAtBodyEnd = false;
};

// A.6: `DoEndBlock ::= "do" Statement* "end" ";"`. Statement-sequencing
// block of type `()`; no value produced (rationale §5.13).
struct DoEndBlock : Stmt {
    std::vector<std::unique_ptr<Stmt>> body;
};

// A.6 trivial transfer statements.
struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

// A.6: `IncrementStmt ::= LValue ("++" | "--") ";"`. Statement-only per
// §17.6 — `++`/`--` are not expression operators.
struct IncrementStmt : Stmt {
    std::unique_ptr<Expr> target;
    std::string_view op; // "++" or "--"
};

// A.6: `DiscardStmt ::= "_" ":=" Expression ";"`. Lexically the `_` here is
// the same Identifier token the lexer always produces for it; the parser
// recognises the discard form when an expression statement's LHS is
// IdentifierExpr "_" followed by `:=`.
struct DiscardStmt : Stmt {
    std::unique_ptr<Expr> value;
    // §17.13 / §28.3 v0.150 (#1009): set by the ownership pass when `value` is an
    // owned PRODUCER of a USER class/interface (producerState == Owned) — an
    // unbound owned temporary that the discard disposes at end-of-statement. A
    // BORROW discard (field/local read, borrow-returning method) is Borrowed and
    // left false, so codegen does not dispose it (no double-free). Owning
    // collections and builtin handles (Task, Subscription, Shared, …) are also
    // left false — they need their own free paths and are not handled here.
    bool disposeOwnedValue = false;
    // §17.13 / §15.3 (#1016): set by the ownership pass when `value` is a
    // move-only owning inline array (`[N owned]T` / `[N owned]T?`). Codegen
    // emits end-of-statement disposal for each non-null slot, mirroring the
    // scope-end inlineOwnedArray disposal path. MoveOnlyArrayTransferWalker
    // marks the source as transferred so its scope-end disposal is skipped
    // (no double-free).
    bool disposeInlineOwnedArray = false;
};

// A.6: `YieldStmt ::= "yield" "return" Expression ";" | "yield" "break" ";"`.
struct YieldStmt : Stmt {
    bool isBreak = false;            // true for `yield break;`
    std::unique_ptr<Expr> value;     // populated for `yield return Expr;`
};

// A.6 CatchArm (slice 22 + slice 43):
//   CatchPattern ::= Type ("," Type)*                          /* no binding */
//                  | "var" Identifier ":" Type ("," Type)*      /* binding */
//   CatchArm     ::= CatchPattern ("when" Expression)? "=>" ArmBody
//
// ArmBody is stored as a Stmt. The three forms from §31.2.2 / §A.6 are all
// supported: (1) Expression (no `;` — wrapped in ExpressionStmt by the
// parser), (2) Statement (with terminating `;`), (3) `do Statement* end`
// (inline DoEndBlock, no trailing `;` required — #588). Expression-form
// bodies use consumePunctuator(";") optionally so both `;` and no-`;` work.
struct CatchArm {
    bool hasBinding = false;     // `var IDENT : ...` vs plain `Type, ...`
    std::string_view varName;    // empty when !hasBinding
    std::vector<std::unique_ptr<Type>> types; // 1+ entries (the CatchPattern)
    std::unique_ptr<Expr> guard; // optional `when EXPR`
    std::unique_ptr<Stmt> body;  // arm body (ExpressionStmt, Statement, or DoEndBlock)
};

// A.6 TryStmt: `"try" Statement* CatchBlock FinallyClause? "end" ";"` or
// `"try" Statement* FinallyClause "end" ";"`.
struct TryStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<CatchArm> catches;
    bool hasFinally = false;
    std::vector<std::unique_ptr<Stmt>> finallyBody;
};

// A.4: `Param ::= ParamModifiers? Identifier ":" Type (":=" Expression)?`.
// `ParamModifiers ::= "ref" | "out" "owned"? | "owned"`. Slice 24 adds
// modifier (recorded as a single composite string like "out owned") and
// default value.
// A.3 SerializedClassClause:
//   `serialized ("tree" | "graph" | "cyclic") PolymorphicClause?`
//
// PolymorphicClause is a closed registry of wire-discriminator -> concrete
// type mappings. `serialized`, `tree`, `graph`, `cyclic`, `polymorphic`
// are contextual keywords (App. B.2) lexed as Identifier.
struct PolyCase {
    std::string_view discriminator; // contents of the StringLiteral
    std::unique_ptr<Type> type;
};
struct SerializedClassClause {
    std::string_view shape;         // "tree" | "graph" | "cyclic"
    std::vector<PolyCase> polyCases; // empty when no PolymorphicClause
};

// A.4 SerializedClause. `serialized` is a contextual keyword (App. B.2)
// lexed as Identifier; `skip` and `optional` are also contextual. The
// grammar admits this on PositionalField, EnumConstant, and auto-property
// PropertyBody — three sites only (slice 41). Sema enforces the
// restrictions noted in the spec: `serialized` requires at least one of
// the inner forms (bare `serialized` is a syntax error — enforced by
// requiring `skip` / `optional` / a string in the parser); `skip` is
// mutually exclusive with name or `optional`; enum constants reject
// `skip` and `optional`.
struct SerializedClause {
    bool isSkip = false;            // `serialized skip`
    bool isOptional = false;        // `optional` qualifier present
    std::string_view name;          // empty when no StringLiteral was given.
                                    // For `serialized "Foo"`, this is `Foo`
                                    // (the string contents stripped of the
                                    // surrounding quotes).
};

struct Param {
    std::string_view modifier; // "ref", "out", "out owned", "owned", or empty
    std::string_view name;
    std::unique_ptr<Type> type;
    std::unique_ptr<Expr> defaultValue; // null when no `:= Expr`
    // A.4 PositionalField's SerializedClause. Only populated for record
    // positional fields — other Param uses leave this null.
    std::unique_ptr<SerializedClause> serialized;
};

// A.3 GenericConstraints. One `where T: c1, c2` clause; multiple clauses
// stack in TypeDecl/MethodDecl::genericConstraints.
struct ConstraintItem {
    // "class" / "value" / "notnull" / "new" / "type". For "type", typeArg is
    // populated; for the others, typeArg is null.
    std::string_view kind;
    std::unique_ptr<Type> typeArg;
};
struct GenericConstraint {
    std::string_view typeParam; // T in `where T: ...`
    // A.3 GenericConstraints has two forms:
    //   `where T : Constraint, Constraint*`     (the regular form)
    //   `where T is ModifierConstraint`         (slice 54; §14.4)
    // The regular form populates `items`; the modifier-constraint form
    // populates `modifierConstraint` ("owned" or "not owned") and leaves
    // `items` empty.
    std::vector<ConstraintItem> items;
    std::string_view modifierConstraint;  // "" / "owned" / "not owned"
};

// A.4: `MethodDecl ::= Visibility "static"? "async"? VirtualKind?
//                      "method" Identifier GenericParams?
//                      "(" ParamList? ")" (":" "owned"? Type)?
//                      GenericConstraints*
//                      MethodBody`
// MethodBody has three shapes per A.4:
//   `=> Expression ";"` (expression-body)
//   `Statement* "end" Identifier? ";"` (block body)
//   `";"` (abstract — body-less)
enum class MethodBodyKind { Expression, Block, Abstract };

struct MethodDecl : Member {
    Visibility visibility = Visibility::None;
    // Grammar `static`/`async`/VirtualKind keywords, recorded as the literal
    // lexemes. Semantically valid combinations are sema's responsibility.
    std::vector<std::string_view> modifiers;
    std::string_view name;
    std::vector<GenericParam> genericParams;
    std::vector<Param> params;
    std::unique_ptr<Type> returnType; // null when no `:` return type
    bool returnOwned = false;

    MethodBodyKind bodyKind = MethodBodyKind::Abstract;
    std::unique_ptr<Expr> exprBody;        // populated when bodyKind=Expression
    std::vector<std::unique_ptr<Stmt>> stmts; // for Block (empty until slice 11+)
    std::string_view endLabel;             // optional `end Identifier?` for Block
    std::vector<GenericConstraint> genericConstraints; // A.4 `where T: …`
};

// A.4: `ConstructorDecl ::= Visibility "constructor" "(" ParamList? ")"
//                            CtorChain? Statement* "end" "constructor"? ";"
//                          | "static" "constructor" "(" ")"
//                            Statement* "end" "constructor"? ";"`.
// CtorChain: `("base" | "this") "(" ArgList? ")" ";"` — first statement when
// present.
// CtorChain (`base(...)` or `this(...)` at the body's start) is recognised
// post-parse by sema, not as a distinct AST member — it surfaces as the body's
// first ExpressionStmt-of-CallExpr-on-`this`/`base`. Avoids needing two-token
// lookahead at parse time to disambiguate from `this.field := ...`.
struct ConstructorDecl : Member {
    Visibility visibility = Visibility::None; // none for the static form
    bool isStatic = false;
    std::vector<Param> params;
    std::vector<std::unique_ptr<Stmt>> body;
};

// A.4: `DestructorDecl ::= "destructor" Statement* "end" "destructor"? ";"`.
// No visibility or parameters per the grammar.
struct DestructorDecl : Member {
    std::vector<std::unique_ptr<Stmt>> body;
};

// A.4 `EventDecl ::= Visibility "static"? "async"? "event" Identifier
//                     (":" Type)? ";"`. The type is implicit when omitted
// (sema picks it up).
struct EventDecl : Member {
    Visibility visibility = Visibility::None;
    bool isStatic = false;
    bool isAsync = false;
    std::string_view name;
    std::unique_ptr<Type> type; // null when no `: Type`
};

// A.4 `ConstDecl ::= Visibility "static"? "const" Identifier (":" Type)?
//                     ":=" ConstExpr ";"`. The "Const" constraint on the
// initializer is sema's; the parser uses ordinary Expression here.
struct ConstDecl : Member {
    Visibility visibility = Visibility::None;
    bool isStatic = false;
    std::string_view name;
    std::unique_ptr<Type> type; // null if no `:` annotation
    std::unique_ptr<Expr> value;
};

// A.4 PropertyBody alternative kinds.
enum class PropertyBodyKind { Expression, Auto, Block };

// A.4 AccessorBody kinds:
//   `=> Expression ;`           PropertyAccessorBodyKind::Expression
//   `=> do Statement* end ;`    PropertyAccessorBodyKind::Block
//   `;`                          PropertyAccessorBodyKind::Abstract
enum class PropertyAccessorBodyKind { Expression, Block, Abstract };

// A.4 Accessor (one `get` or `set` arm inside an accessor block). Slice 37.
struct PropertyAccessor {
    Visibility visibility = Visibility::None;  // optional per A.4
    bool isSet = false;                        // false = get, true = set
    // SetterParamSpec (set-only): `( ParamMod? Identifier? )`. Both pieces
    // are optional. Implicit param name is `value`; implicit modifier is
    // none (borrow-taking) — sema substitutes the defaults.
    std::string_view setterParamMod;           // "" / "owned" / "ref" / ...
    std::string_view setterParamName;          // "" when only the modifier was given
    bool hasSetterParamSpec = false;           // true if `(...)` was written at all
    std::vector<GenericConstraint> setterConstraints; // set-only, optional
    PropertyAccessorBodyKind bodyKind = PropertyAccessorBodyKind::Abstract;
    std::unique_ptr<Expr> exprBody;
    std::vector<std::unique_ptr<Stmt>> stmts;
};

// A.4 `PropertyDecl ::= Visibility "static"? VirtualKind? "readonly"?
//                        "property" Identifier ":" Type PropertyBody`.
// Forms supported:
//   `=> Expression ;`                                           (Expression)
//   `AutoAccessorVis? (":=" Expression)? SerializedClause? ";"` (Auto)
//   `Accessor* "end" Identifier? ";"`                            (Block)
// AutoAccessorVis (`Visibility "get"|"set"`) carries a per-accessor
// visibility override on the auto form — e.g. `property X: int private
// set := 0;` is publicly-gettable / privately-settable.
struct PropertyDecl : Member {
    Visibility visibility = Visibility::None;
    std::vector<std::string_view> modifiers; // static, readonly + VirtualKind
    std::string_view name;                   // empty for indexers
    std::unique_ptr<Type> type;
    PropertyBodyKind bodyKind = PropertyBodyKind::Auto;
    std::unique_ptr<Expr> exprBody;        // Expression-body case
    std::unique_ptr<Expr> autoInitializer; // Auto with `:= …`
    // Slice 48: AutoAccessorVis on the auto-property form.
    // `hasAutoAccessorVis` distinguishes the absent from the
    // `Visibility::None get/set` case (the latter is invalid but the
    // grammar requires Visibility).
    bool hasAutoAccessorVis = false;
    Visibility autoAccessorVis = Visibility::None;
    bool autoAccessorIsSet = false; // true=set, false=get
    // Slice 41: SerializedClause on the auto-property form.
    std::unique_ptr<SerializedClause> serialized;
    std::vector<PropertyAccessor> accessors; // Block-body case
    std::string_view blockEndLabel;          // optional Identifier after `end`
    // Indexer form (A.4 IndexerDecl): the index parameter list appears as
    // `[ParamList]` where a regular property would have an Identifier name.
    bool isIndexer = false;
    std::vector<Param> indexerParams;
};

// A.3 `EnumConstant ::= Identifier (":=" ConstExpr)? SerializedClause?`.
struct EnumConstant {
    std::string_view name;
    std::unique_ptr<Expr> value; // null when no `:= …`
    // Slice 41: SerializedClause on an enum constant — sema restricts to the
    // `serialized "name"` form (skip and optional are rejected per the §A.3
    // grammar note).
    std::unique_ptr<SerializedClause> serialized;
};

// Class-like top-level declaration. Slice 3 introduced the empty body; slice
// 5 adds generic parameters and a base-type list. Primary-constructor params
// (classes), positional fields (records), serialization clauses, generic
// constraints, members, and the enum-specific inheritance form are still
// later-slice work. The four kinds share enough shape that one struct with a
// `kind` tag is the right model until per-kind members force a split.
struct TypeDecl : TopDecl {
    TypeDeclKind kind = TypeDeclKind::Class;
    Visibility visibility = Visibility::None;
    std::vector<std::string_view> modifiers; // "abstract", "sealed", "static"
    std::string_view name;
    std::vector<GenericParam> genericParams; // empty if no `<...>`
    // A.3 `InheritItem ::= Type ("(" ArgList? ")")?` — the optional argument
    // list (only valid on the base class, semantically) is tokenized away in
    // this slice; full parsing waits on expressions.
    std::vector<std::unique_ptr<Type>> baseTypes; // empty if no `:` clause
    // Class members (slice 6: fields only). Interface members
    // (IfaceMethod/IfaceProperty/IfaceIndexer/IfaceEvent), record bodies,
    // and enum constants/members are separate grammar productions and land
    // in their own slices; for those kinds this vector stays empty.
    std::vector<std::unique_ptr<Member>> members;
    // Enum-only: the comma-separated constants (`EnumConstants` per A.3).
    // Empty for non-enum kinds.
    std::vector<EnumConstant> enumConstants;
    // §A.3 GenericConstraints* after the inheritance clause.
    std::vector<GenericConstraint> genericConstraints;
    // Record-only: the parenthesised positional fields per A.3
    // `PositionalFields ::= "(" PositionalField ("," PositionalField)* ")"`,
    // `PositionalField ::= Identifier ":" Type SerializedClause?`. Reuses
    // Param (same name+type shape). SerializedClause deferred.
    std::vector<Param> positionalFields;
    bool hasPositionalFields = false;
    // Record-only: `record Foo(...) ;` shorthand body (no `end`).
    bool isRecordShorthand = false;
    std::string_view endLabel; // empty when the `end` had no label
    // §11.1 / #873: Class-only. When the class has a primary constructor AND a
    // base class whose constructor takes arguments, the inheritance clause may
    // carry a base-call arg list: `class D(v: int): Base(v)`. Parser stores the
    // base-call arguments here (parallel to baseTypes[0] for classes). Empty
    // when no such arg list was written; non-empty only for kind == Class &&
    // hasPositionalFields.
    std::vector<CallArgument> primaryBaseCtorArgs;
    // A.3 SerializedClassClause (slice 42). Null when not present.
    std::unique_ptr<SerializedClassClause> serializedClass;
};

// A.4 `NestedTypeDecl ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl`.
// Wraps an inner TypeDecl declared as a Member of an enclosing class or
// record (slice 40).
struct NestedTypeMember : Member {
    std::unique_ptr<TypeDecl> decl;
};

// Slice 4: named type reference. The grammar (A.5) has more alternatives —
// inline arrays, function types, and the unit type `()` — but this slice
// covers only `QualifiedName GenericArgs?` with the optional `?` nullable
// suffix. When inline-array and function types arrive, `Type` becomes the
// polymorphic base and `NamedType` is split out as a subclass; for now a
// single struct is the right fit.
//
// Slice 46: extended for the remaining A.5 TypeNoNullable alternatives —
// `InlineArrayType`, `FunctionType`, and the unit type `()`. A `kind` tag
// distinguishes them; only the matching field group is meaningful. A future
// refactor will split these into a polymorphic hierarchy if the layered
// shape becomes a burden.
enum class TypeKind { Named, Unit, InlineArray, Function };
struct FuncParamType {
    std::string_view modifier;       // "", "ref", "out", "out owned", "owned"
    std::string_view name;           // optional parameter name
    std::unique_ptr<Type> type;
};
struct Type : Node {
    TypeKind kind = TypeKind::Named;
    bool nullable = false;           // `T?` suffix; applies to any kind
    // Named:
    std::unique_ptr<QualifiedName> name;
    std::vector<std::unique_ptr<Type>> genericArgs;
    // InlineArray (`[N]T` / `[N owned]T`):
    std::unique_ptr<Expr> arraySize;
    bool arrayOwned = false;
    std::unique_ptr<Type> arrayElement;
    // Function (`(T1, T2) -> T` / `(T1, T2) -> owned T`):
    std::vector<FuncParamType> funcParams;
    bool funcReturnOwned = false;
    std::unique_ptr<Type> funcReturn;
};

// A.2: `TypeAliasDecl ::= "type" Identifier GenericParams? "=" Type
//                          GenericConstraints* ";"`.
struct TypeAlias : TopDecl {
    std::string_view name;
    std::vector<GenericParam> genericParams;
    std::unique_ptr<Type> aliasedType;
    std::vector<GenericConstraint> genericConstraints;
};

// A.4 `OperatorDecl ::= Visibility "static" "method" "operator" OpToken
//                        "(" ParamList ")" ":" "owned"? Type OperatorBody`.
// OperatorBody mirrors MethodBody's expression / block / abstract forms.
struct OperatorDecl : Member {
    Visibility visibility = Visibility::None;
    std::string_view opToken;       // e.g. "+", "==", "<", "[]"
    std::vector<Param> params;
    std::unique_ptr<Type> returnType;
    bool returnOwned = false;
    MethodBodyKind bodyKind = MethodBodyKind::Abstract;
    std::unique_ptr<Expr> exprBody;
    std::vector<std::unique_ptr<Stmt>> stmts;
};

// --- Expressions (§17, A.5) -------------------------------------------------
// Slice 7 covers primary expressions: literals (numeric/char/string/bool/null),
// identifiers, parenthesized expressions, postfix member access (`.x`) and
// call (`(...)`). Unary, binary, `new`, `?.`, `?(`, indexing, casts, and
// everything else land in later slices.

struct Expr : Node {
    // §17.1 mandatory parenthesization (#1086): source-level parens leave no
    // ParenExpr node — the tree shape encodes grouping — so the parser stamps
    // this flag on the inner expression of a `(...)` group. The cross-category
    // mandatory-parenthesization check reads it to tell `(a & 1) = 0` (legal)
    // from `a & 1 = 0` (a §17.1 error). It has no meaning after parsing.
    bool parenthesized = false;
};

// Numeric / char / string literals carry the raw lexeme; value parsing and
// type interpretation are sema's job (matches how the lexer handles literals).
struct IntLiteralExpr      : Expr { std::string_view text; };
struct FloatLiteralExpr    : Expr { std::string_view text; };
struct CharLiteralExpr     : Expr { std::string_view text; };
struct StringLiteralExpr   : Expr { std::string_view text; };
struct VerbatimStringExpr  : Expr { std::string_view text; };

// §18.5 / §18.7: one segment of an interpolated string — either a literal
// substring (after escape-processing) or an embedded expression whose
// value is converted to string and concatenated into the result.
struct InterpPart {
    std::string literal;                    // non-empty iff this is a literal part
    std::unique_ptr<Expr> expr;             // non-null iff this is an expression part
    std::string formatSpec;                 // §18.5 format specifier (e.g. "F2", "X")
    std::unique_ptr<std::string> exprSource; // heap-allocated source for sub-parsed
                                            // expressions; string_views in the expr
                                            // AST remain valid through vector moves.
};

// §18.5 / §18.7: interpolated string. `parts` holds the alternating literal
// and expression segments produced by the parser split slice; `text` retains
// the full source lexeme for diagnostic messages and backward compatibility.
struct InterpolatedStringExpr : Expr {
    bool isVerbatim = false;
    std::string_view text;
    std::vector<InterpPart> parts;      // populated by parser split slice
};

// `true` / `false` decoded at parse time; null is a marker.
struct BoolLiteralExpr : Expr { bool value = false; };
struct NullLiteralExpr : Expr {};

// per §14.1 / §22.12 (issue #520): an identifier that names a generic class
// may carry explicit type arguments in expression/receiver position, as in
// `Registry<int>.Make(...)`.  The parser populates `typeArgs` only when the
// disambiguating scan confirms `< TypeArgList > .` (dot follows the closing
// `>`), not a relational `<` operator.  Empty for all non-generic-receiver
// uses (the common case).
struct IdentifierExpr : Expr {
    std::string_view name;
    std::vector<std::unique_ptr<Type>> typeArgs;
};

// Source-level parentheses (`(expr)`) are NOT represented as their own AST
// node — they only group the parse; the resulting tree shape already encodes
// the grouping. exprToString always parenthesizes BinaryExpr, so reading any
// expression's source-vs-AST shape is unambiguous in tests.

// A.5: `expr "." Identifier GenericArgs?`. Postfix; the `target` is whatever
// expression preceded the dot.  The optional `typeArgs` hold the explicit
// method-level type arguments when the programmer writes `obj.Method<T>(...)`.
// The parser populates this only when a disambiguating scan confirms that the
// `<` is a generic-type-arg list (not a relational operator); see §14.1 and
// the parser note in Parser_Exprs.cpp.
struct MemberAccessExpr : Expr {
    std::unique_ptr<Expr> target;
    std::string_view member;
    // Explicit method-level type arguments, e.g. `.Identity<int>`.
    // Empty when the programmer omits type args (most calls — type is inferred
    // from arguments per §14.1 or from the return-target per #391).
    std::vector<std::unique_ptr<Type>> typeArgs;
};

// A.4 / §A.5 Argument (CallArgument): declared earlier in this file (before
// TypeDecl) so TypeDecl::primaryBaseCtorArgs can use it. The struct definition
// lives near the top of the file, next to the Expr/Type forward declarations.

// A.5: `expr "(" ArgList? ")"`. Postfix call.
struct CallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<CallArgument> args;
};

// A.5 / §17.1 level 1 postfix forms beyond `.` and `(`:
//   `expr "?." Identifier`    — null-conditional member access (`?.`)
//   `expr "?(" ArgList? ")"`  — null-conditional invocation (`?(`)
//   `expr "[" IndexList "]"`  — indexing (multi-dim via comma list)
//   `expr "!"`                — postfix null-forgiving operator
struct NullCondMemberExpr : Expr {
    std::unique_ptr<Expr> target;
    std::string_view member;
};
struct NullCondCallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<CallArgument> args;
};
struct IndexExpr : Expr {
    std::unique_ptr<Expr> target;
    // §A.7 PrimarySuffix `[ ArgList ]`: indexer indices share the call
    // ArgList grammar (slice 55) — admitting modifiers, named args, and
    // the `_` discard. Sema enforces which apply to indexer setters vs
    // getters.
    std::vector<CallArgument> indices;
};
struct PostfixBangExpr : Expr {
    std::unique_ptr<Expr> operand;
};

// A.5: `NewExpr ::= "new" Type "(" ArgList? ")" | "new" "(" ArgList? ")"`.
// The shorthand form (no Type) is the target-typed `new(...)`; `type` is null
// in that case.
struct NewExpr : Expr {
    std::unique_ptr<Type> type;
    std::vector<CallArgument> args;
};

// §17.1 binary expressions. One AST struct for all of them; the operator
// lexeme distinguishes (matches our keyword/punctuator-by-lexeme model).
// Slice 8 wires up multiplicative, additive, relational, equality, and the
// `and`/`or` logical operators. Shift, range, bitwise, null-coalescing,
// conditional, and assignment operators arrive in later slices.
struct BinaryExpr : Expr {
    std::string_view op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

// §17.1 level 2 unary prefix. `+ - not ~` for slice 9; `await` (and any
// future prefix operators) come with their own slice. Right-associative —
// `--x` parses as `-(-x)`.
struct UnaryExpr : Expr {
    std::string_view op;
    std::unique_ptr<Expr> operand;
};

// §17.4: `is`, `is not`, `as` — relational-level keyword operators whose
// right operand is a Type (not an expression). `is not` is a single
// logical operator with a two-token spelling (parser consumes both keyword
// tokens and stores the composite "is not" in `op`).
struct TypeTestExpr : Expr {
    std::unique_ptr<Expr> operand;
    std::string_view op; // "is", "is not", or "as"
    std::unique_ptr<Type> testType;
};

// §17.1 L15 conditional: `X if cond else Y`. Right-associative.
struct ConditionalExpr : Expr {
    std::unique_ptr<Expr> thenValue;  // X
    std::unique_ptr<Expr> condition;  // cond
    std::unique_ptr<Expr> elseValue;  // Y
};

// §A.7 ThrowExpr: `throw Expression?`. Bare `throw` is valid only inside a
// catch arm body; sema enforces context.
struct ThrowExpr : Expr {
    std::unique_ptr<Expr> operand; // null for bare `throw`
};

// A.7 `TypeOfExpr ::= "typeof" "(" Type ")"`.
struct TypeOfExpr : Expr { std::unique_ptr<Type> queriedType; };

// A.7 `NameOfExpr ::= "nameof" "(" QualifiedName ")"`. The QualifiedName is
// recorded; sema resolves it.
struct NameOfExpr : Expr { std::unique_ptr<QualifiedName> name; };

// A.7 `DefaultExpr ::= "default" "(" Type ")"`.
struct DefaultExpr : Expr { std::unique_ptr<Type> type; };

// A.7 RangeExpr. Replaces the BinaryExpr `..` / `..<` representation as of
// slice 52 to make the open-ended and `step` forms representable.
//
//   RangeExpr ::= Shift                                  /* no range */
//               | Shift (".." | "..<") Shift ("step" Shift)?
//               | Shift ".."                             /* low-only */
//               | (".." | "..<") Shift                   /* high-only */
//               | ".."                                   /* fully open */
//
// `lo` and `hi` may be null for the open-ended forms (low-only / high-only
// / fully-open). `step` is optional (only valid when lo and hi are both
// present per §17 — sema enforces).
struct RangeExpr : Expr {
    bool inclusive = false;       // true for `..`, false for `..<`
    std::unique_ptr<Expr> lo;     // null on the high-only and fully-open forms
    std::unique_ptr<Expr> hi;     // null on the low-only and fully-open forms
    std::unique_ptr<Expr> step;   // null when no `step EXPR` suffix
};

// A.7 unit value `()`. A no-fields expression of the unit type. The
// PrimaryAtom-level `"(" ")"` alternative in the grammar — distinct from
// the unit-type form (slice 46) which is `()` in a type context.
struct UnitExpr : Expr {};

// A.7 `with` PrimarySuffix: `EXPR "with" InitAssign ("," InitAssign)* ","?
// "end"` where `InitAssign ::= Identifier ":=" Expression`. Source-type
// determines the semantic: record copy-with-overrides or object initializer
// block on a freshly-constructed class (§22.11). The parser stores both
// shapes in the same node — sema dispatches by the source's type.
struct WithExpr : Expr {
    struct InitAssign {
        std::string_view name;
        std::unique_ptr<Expr> value;
    };
    std::unique_ptr<Expr> source;
    std::vector<InitAssign> assigns;
};

// A.7 `CollectionLiteral`:
//   "[" (Expression ("," Expression)*)? "]"        sequence
//   "[" DictEntry ("," DictEntry)* "]"             dictionary
//   DictEntry ::= Expression ":" Expression
// Discriminated by what follows the first element: a `:` means dict form,
// `,`/`]` means sequence form. Both stored in the same node — `isDict` and
// the parallel `keys` vector tell them apart.
struct CollectionLiteralExpr : Expr {
    bool isDict = false;
    std::vector<std::unique_ptr<Expr>> elements;  // sequence form: values
                                                  // dict form: values
    std::vector<std::unique_ptr<Expr>> keys;      // dict form: keys (parallel
                                                  // to elements); empty for
                                                  // sequence form
};

// A.7 `LambdaExpr ::= "async"? "(" LambdaParams? ")" "=>" LambdaBody`. Slices
// 28 and 36 together cover the full form; the buffered-lookahead
// discriminator is in parsePrimaryExpression.
struct LambdaExpr : Expr {
    bool isAsync = false;
    std::vector<Param> params;
    bool isBlockBody = false;
    std::unique_ptr<Expr> exprBody;  // populated when !isBlockBody
    std::vector<std::unique_ptr<Stmt>> stmts; // populated when isBlockBody
};

// --- A.7 Patterns and CaseExpr (slice 38) --------------------------------
//
// Pattern is a small AST hierarchy. The kinds line up 1:1 with the grammar
// alternatives in §A.7 `Pattern`. A few cases are intentionally
// representation-collapsed:
//
//   * Single-Identifier `Constant` vs single-Identifier `Type` is
//     deliberately ambiguous in the grammar (§A.7 note: "if `Dog` resolves
//     to a type, the pattern is a type test; otherwise it is a constant
//     pattern"). The parser emits a `NamedPattern` for both — sema picks
//     between Type-test and Constant by resolving the name. The same applies
//     to QualifiedNames (`Color.Red` vs `System.IO.Stream`).
//   * `RangeExpr` patterns reuse the regular `Expr` AST (rangeExpr).
//   * The destructuring form stores its arguments as a vector of
//     PositionalPattern, each of which is either a real Pattern or a naked
//     binding Identifier (per A.7 PositionalPattern).
struct Pattern : Node {};

struct DiscardPattern : Pattern {};                 // `_`
struct NullPattern : Pattern {};                    // `null`

struct BindingPattern : Pattern {                   // `var IDENT (: TYPE)?`
    std::string_view name;
    std::unique_ptr<Type> type;
};

struct ConstantPattern : Pattern {                  // literal alternatives
    // One or more `Constant`s separated by `,`. Each holds the raw
    // expression as parsed (literals or QualifiedName); sema enforces the
    // §A.8 `Constant` restriction (compile-time evaluable, etc.).
    std::vector<std::unique_ptr<Expr>> constants;
};

struct NamedPattern : Pattern {                     // QualifiedName as pattern
    std::unique_ptr<QualifiedName> name;
};

struct RangePattern : Pattern {                     // RangeExpr in pattern pos
    std::unique_ptr<Expr> rangeExpr;
};

struct RelOpPattern : Pattern {                     // `< EXPR`, `>= EXPR`, ...
    std::string_view op;  // "<" | ">" | "<=" | ">="
    std::unique_ptr<Expr> expr;
};

struct TypePattern : Pattern {                      // primitive-type-led
    std::unique_ptr<Type> type;
};

struct PositionalPattern {
    // Either `pattern` is populated (a sub-Pattern) or `nakedBinding` is
    // (A.7 "Identifier" alternative — valid only in destructuring position;
    // names a binding of the matched component).
    std::unique_ptr<Pattern> pattern;
    std::string_view nakedBinding;
    // #1271: a real, per-binding polymorphic Node anchor for a NAKED binding
    // (`Box(a)`). The naked binding has no dedicated Pattern node of its own, so
    // sema/codegen previously keyed its LocalSymbol/`locals_` slot on
    // `reinterpret_cast<Node*>(&this)` — a non-Node pointer that CRASHED any
    // `dynamic_cast<...>(loc->decl)` (e.g. isSubscriptionExpr) by reading a bogus
    // vtable. The parser now mints a BindingPattern here (name == nakedBinding) so
    // the anchor is a genuine dynamic_cast-safe Node. Null for a non-naked
    // PositionalPattern (which anchors on its own `pattern` BindingPattern).
    std::unique_ptr<BindingPattern> nakedAnchor;
};

struct DestructuringPattern : Pattern {             // `QualifiedName ( ... )`
    std::unique_ptr<QualifiedName> typeName;
    std::vector<PositionalPattern> subPatterns;
};

// A.7 `CaseArm ::= Pattern ("when" Expression)? "=>" ArmBody`. The body
// kind matches `ArmBody`: Expression / Statement / `do Statement* end`.
// Slice 38 covers only the Expression-body form (the Statement form is
// valid only in statement-position case/catch; the `do … end` form for
// type `()` lands with the statement-position case slice).
enum class CaseArmBodyKind { Expression, Block };

struct CaseArm {
    std::unique_ptr<Pattern> pattern;
    std::unique_ptr<Expr> guard;            // optional `when EXPR`
    CaseArmBodyKind bodyKind = CaseArmBodyKind::Expression;
    std::unique_ptr<Expr> exprBody;         // when bodyKind == Expression
    std::vector<std::unique_ptr<Stmt>> stmts; // when bodyKind == Block
};

// A.7 `CaseExpr ::= "case" Expression? CaseArm ("," CaseArm)* ","? "end"`.
struct CaseExpr : Expr {
    std::unique_ptr<Expr> scrutinee;   // null when bare `case CaseArm*`
    std::vector<CaseArm> arms;
};

// A.6 statement-position case. Same shape as CaseExpr but trailed by `;` and
// admitting Statement-form / `do … end` ArmBody (in addition to Expression).
// The grammar's ArmBody Statement-form note: "valid only in statement-position
// case or in a catch arm; reads as a single statement whose terminating `;`
// ends the arm body, the same as `do Statement end`." Slice 39 covers the
// Statement-form and `do … end` forms; the Expression-form is not admitted
// here (a value-producing arm in stmt-position would discard its result, the
// canonical shape there is `do EXPR end` or wrapping it in `_ := …;`).
struct CaseStmt : Stmt {
    std::unique_ptr<Expr> scrutinee;
    std::vector<CaseArm> arms;
};

// A.3 `ExtensionDecl ::= Visibility? "extension" Type GenericConstraints*
//                          ExtensionMember* "end" ";"`.
struct ExtensionDecl : TopDecl {
    Visibility visibility = Visibility::None;
    std::unique_ptr<Type> targetType;
    std::vector<GenericConstraint> genericConstraints;
    std::vector<std::unique_ptr<Member>> members;
};

// A.3 `ExternDecl ::= Visibility? "method" Identifier "(" ParamList? ")"
//                      (":" "owned"? Type)? ";"`. Single FFI method
// declaration inside an extern block.
struct ExternDecl {
    Visibility visibility = Visibility::None;
    std::string_view name;
    std::vector<Param> params;
    std::unique_ptr<Type> returnType;
    bool returnOwned = false;
};

// A.3 `ExternBlock ::= "extern" StringLiteral ("convention" StringLiteral)?
//                       ExternDecl+ "end" ";"`. §32.
struct ExternBlock : TopDecl {
    std::string_view library;     // the StringLiteral lexeme (with quotes)
    std::string_view convention;  // empty if no `convention "..."`
    std::vector<ExternDecl> methods;
};

// A.2: `SourceFile ::= ModuleDecl Import* TopDecl*`  (§2.1 / §2.4)
//
// `module` is required by the spec on every source file's first non-comment,
// non-import line, but the parser allows it to be missing (reporting a
// diagnostic) so later phases can still inspect what was parsed.
struct SourceFile : Node {
    std::unique_ptr<ModuleDecl> module;
    std::vector<std::unique_ptr<Import>> imports;
    std::vector<std::unique_ptr<TopDecl>> topDecls;
};

} // namespace bezel::ast
