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
// Slice 11 covers the first two forms; `await using` waits on the async slice.
struct LocalDecl : Stmt {
    bool isUsing = false;  // `using var` form
    std::string_view name;
    std::unique_ptr<Type> type;        // null when no `: Type`
    std::unique_ptr<Expr> initializer; // null only when plain `var x;`
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

// A.6: `DoEndBlock ::= "do" Statement* "end" ";"`. Statement-sequencing
// block of type `()`; no value produced (rationale §5.13).
struct DoEndBlock : Stmt {
    std::vector<std::unique_ptr<Stmt>> body;
};

// A.6 trivial transfer statements.
struct BreakStmt : Stmt {};
struct ContinueStmt : Stmt {};

// One parameter in a method's parameter list. Slice 10 covers
// `Identifier ":" Type`; modifiers (`ref`/`out`/`out owned`/`owned`) and
// default values come with later slices.
struct Param {
    std::string_view name;
    std::unique_ptr<Type> type;
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
    std::string_view endLabel; // empty when the `end` had no label
};

// Slice 4: named type reference. The grammar (A.5) has more alternatives —
// inline arrays, function types, and the unit type `()` — but this slice
// covers only `QualifiedName GenericArgs?` with the optional `?` nullable
// suffix. When inline-array and function types arrive, `Type` becomes the
// polymorphic base and `NamedType` is split out as a subclass; for now a
// single struct is the right fit.
struct Type : Node {
    std::unique_ptr<QualifiedName> name;
    std::vector<std::unique_ptr<Type>> genericArgs; // empty when not generic
    bool nullable = false;
};

// A.2: `TypeAliasDecl ::= "type" Identifier GenericParams? "=" Type
//                          GenericConstraints* ";"`. Slice 4 admits only the
// degenerate form — no generic params, no constraints; those arrive with
// the generics slice.
struct TypeAlias : TopDecl {
    std::string_view name;
    std::unique_ptr<Type> aliasedType;
};

// --- Expressions (§17, A.5) -------------------------------------------------
// Slice 7 covers primary expressions: literals (numeric/char/string/bool/null),
// identifiers, parenthesized expressions, postfix member access (`.x`) and
// call (`(...)`). Unary, binary, `new`, `?.`, `?(`, indexing, casts, and
// everything else land in later slices.

struct Expr : Node {};

// Numeric / char / string literals carry the raw lexeme; value parsing and
// type interpretation are sema's job (matches how the lexer handles literals).
struct IntLiteralExpr      : Expr { std::string_view text; };
struct FloatLiteralExpr    : Expr { std::string_view text; };
struct CharLiteralExpr     : Expr { std::string_view text; };
struct StringLiteralExpr   : Expr { std::string_view text; };
struct VerbatimStringExpr  : Expr { std::string_view text; };

// `true` / `false` decoded at parse time; null is a marker.
struct BoolLiteralExpr : Expr { bool value = false; };
struct NullLiteralExpr : Expr {};

struct IdentifierExpr : Expr { std::string_view name; };

// Source-level parentheses (`(expr)`) are NOT represented as their own AST
// node — they only group the parse; the resulting tree shape already encodes
// the grouping. exprToString always parenthesizes BinaryExpr, so reading any
// expression's source-vs-AST shape is unambiguous in tests.

// A.5: `expr "." Identifier`. Postfix; the `target` is whatever expression
// preceded the dot.
struct MemberAccessExpr : Expr {
    std::unique_ptr<Expr> target;
    std::string_view member;
};

// A.5: `expr "(" ArgList? ")"`. Postfix call.
struct CallExpr : Expr {
    std::unique_ptr<Expr> callee;
    std::vector<std::unique_ptr<Expr>> args;
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
    std::vector<std::unique_ptr<Expr>> args;
};
struct IndexExpr : Expr {
    std::unique_ptr<Expr> target;
    std::vector<std::unique_ptr<Expr>> indices; // 1 or more for multi-dim
};
struct PostfixBangExpr : Expr {
    std::unique_ptr<Expr> operand;
};

// A.5: `NewExpr ::= "new" Type "(" ArgList? ")" | "new" "(" ArgList? ")"`.
// The shorthand form (no Type) is the target-typed `new(...)`; `type` is null
// in that case.
struct NewExpr : Expr {
    std::unique_ptr<Type> type;
    std::vector<std::unique_ptr<Expr>> args;
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
