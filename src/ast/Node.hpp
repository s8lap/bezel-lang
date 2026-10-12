#pragma once

// Abstract syntax tree node definitions. The full node hierarchy is built out
// per the grammar in Appendix A and the per-construct rules across §9–§27, by
// each parser milestone slice. Right now this file holds only the source-file
// and module-declaration shapes (parser slice 1) — imports and top-level
// declarations are added by their own slices.

#include <cstdint>
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

// Grammar A.1: `Import ::= "import" QualifiedName ("as" Identifier)? ";"` (§2.2 v0.164)
// The optional `as Identifier` clause binds an import alias (#1365).
struct Import : Node {
    std::unique_ptr<QualifiedName> name;
    std::string alias;  // §2.2 v0.164: non-empty when "as Identifier" is present
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

// Distinguishes the five class-like top-level declarations parsed here (§9,
// §10, §11, §35).
// Extension declarations (§26), type aliases (§A.2 TypeAliasDecl), and
// extern blocks (§32) get their own AST nodes in their own slices.
enum class TypeDeclKind {
    Class,     // §11
    Interface, // §11.x / §A.3 InterfaceDecl
    Record,    // §9
    Enum,      // §10
    Choice,    // §35 (v0.200, #2044)
};

// Common base for the grammar's `TopDecl` alternatives (A.2). Concrete
// subclasses follow.
struct TopDecl : Node {};

struct TypeDecl; // defined after Expr; used by LocalTypeDecl.
struct Type; // defined after TypeDecl; used by TypeDecl::baseTypes.
struct Expr; // defined further below; used by FieldDecl::initializer.
// defined further below (with the rest of the Pattern hierarchy); used by
// Param::destructure (§27.4, v0.183: lambda-parameter record destructuring).
struct DestructuringPattern;

// §11.1 / #873: forward-declared here so TypeDecl::primaryBaseCtorArgs can use
// it. The full definition appears below (after Expr is defined in full).
// A.4 / §A.5 Argument: `ArgModifiers? (Identifier ":=")? ArgExpression`
struct CallArgument {
    std::string_view modifier;  // "", "ref", "out", "out var",
                                // "out using var", "out owned",
                                // "scoped" (v0.180, §22.3/§28.14, #1439)
                                // Note: bare "owned" was removed at argument
                                // position per §A.4 / proposal 0090 (v0.246);
                                // use `move x` to transfer ownership.
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

// §9.2 EqualityClause (v0.298+). `equality` is a contextual keyword
// (App. B.2) lexed as Identifier; `skip` is also contextual. The grammar
// admits this on the same storage-bearing sites as `serialized` —
// PositionalField, CaseField (choice payload), explicit FieldDecl, and
// auto-property PropertyBody — but NOT on EnumConstant or ChoiceCase
// (those are §9.2 inert-annotation rejects). Only `skip` exists: rename
// and `optional` have no equality analogue, so the vocabulary is one word,
// closed (spec §9.2, line 1385). Sema enforces the inert-annotation
// rejection on statics, computed properties, methods, enum constants, and
// choice cases — see fix #4643 / spec §9.2.
//
// Defined BEFORE FieldDecl (line 114 below) and Param (line 597 below)
// because both embed `std::unique_ptr<EqualityClause>` and the compiler
// needs the complete type to instantiate the unique_ptr.
struct EqualityClause {
    bool isSkip = false;            // `equality skip` — the only form
};

// A.4: `FieldDecl ::= Visibility "static"? ("owned" | "scoped")? "readonly"?
//                     EqualityClause? Identifier ":" Type (":=" Expression)? ";"`.
struct FieldDecl : Member {
    Visibility visibility = Visibility::None;
    std::vector<std::string_view> modifiers; // "static", "owned", "readonly"
    // §9.2 EqualityClause on an explicit field declaration. Grammar places
    // it BEFORE the Identifier (A.4 FieldDecl, v0.298). Sema rejects the
    // clause on a `static` field as an inert annotation (§9.2 line 1419).
    std::unique_ptr<EqualityClause> equality;
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
    // §28.7 / §28.3 (#1625): set by the ownership pass ONLY on a null-
    // INITIALIZED plain (non-`using`) local (`var x: T? := null;`, PolyNull)
    // whose ownership state BECOMES Owned through reassignment — it later
    // receives a fresh owned producer (`x := new T();`). Such a local's
    // initializer (`null`) is not itself an owned producer, so codegen's
    // unwindOwnedLocalClass — which otherwise keys on the initializer form —
    // would never classify it as an owned class local, and §28.7 drop-on-overwrite
    // (disposeDisplacedOwnedClassLocal) plus §28.3 scope-end disposal would both
    // be skipped, leaking every displaced value. The state-match rule (§28.3)
    // guarantees such a local, once owned, is never reassigned a borrow, so
    // treating it as owned for disposal is sound. Deliberately NOT set for a
    // no-initializer local (`var x: Stream;`) that becomes owned on both branches
    // of a join — that case CARRIES the §28.3 obligation and stays a hard error
    // absent explicit discharge (own_join.owned_on_both_branches_carries_obligation)
    // — nor for a borrow-initialized local (its drop-on-overwrite would dispose
    // the borrow: a double-free). Codegen reads this flag as a fallback in
    // unwindOwnedLocalClass; the ownership-side mirror localDropOnOverwriteCovered
    // reads it to admit the local to affine auto-drop.
    bool becomesOwnedViaReassign = false;
    // §28.3 v0.190 SB-01 (#1872): set by the ownership pass on a plain
    // (non-`using`) local whose CALL initializer producerState classifies
    // BORROWED because the call BORROWS an existing frame owned elsewhere —
    // a bare-type-parameter-return call (`First<T>(a): T`, #1801/§28.3 v0.194)
    // or an owning-wrapper method returning a bare receiver owned-field (the
    // #1493 shape). Codegen's plain-`var` task disposal gate
    // (LlvmBackend_Stmts.cpp) treats EVERY CallExpr initializer as a task-frame
    // mint ("calls mint") via taskLocalInitializerIsBorrowSource, which only
    // recognizes the SYNTACTIC borrow sources (alias / storage read). These two
    // exotic CALL shapes are borrows the shape-gate misses, so codegen would
    // register a second scope-end `d.task` alongside the real owner's — a
    // double-free of the one `bz_task_t*`. Consulting this flag makes codegen
    // agree with producerState on who holds the disposal obligation instead of
    // re-deriving ownership from the AST shape. A genuine Task-family mint call
    // (a user `async` method, `Task.FromResult`, …) is classified Owned (or
    // stays an unrecognized owner) and is NOT flagged, so it keeps its `d.task`.
    bool initializerCallIsBorrow = false;
    // fix #1892 / §17.13 / §28.3 / §28.5 (v0.194): set by the ownership pass on a
    // plain (non-`using`) owning-COLLECTION local (OwningArray/OwningDictionary/
    // OwningSet/OwningStack/OwningQueue/OwningLinkedList) that is discharged via a
    // `_ := x;` DISCARD (a marker-free-position consume, spec §17.13/§28.3
    // glossary, v0.281). Such a local is
    // markDisposedByName in the ownership pass, so it is NOT flagged
    // autoDropAtScopeEnd and would otherwise fall to registerUnwindOwnedLocal —
    // which covers only class/interface instances (unwindOwnedLocalClass excludes
    // collections), leaving the container un-disposed on the EXCEPTION edge when an
    // operation between acquisition and the discard throws (the pre-discard-throw
    // leak). This flag routes it through registerUsingDisposal instead, registering
    // the full collection disposal (bz_array_free/bz_dict_free/bz_llist_free with
    // the element-disposer cascade, §28.5) which fires on BOTH the normal and
    // unwind edges and is null-safe. The discard codegen disposes the container at
    // the discard point (§17.13 timing) and NULLS the slot, so the both-edges
    // registration is a no-op on the normal path (null reload) and the sole
    // disposer on the pre-discard-throw exception path — exactly once, no double-
    // free. Only set for the discard discharge form (NOT `move`/`.Dispose()`, whose
    // codegen does not null a collection slot), so it can never double a transfer.
    bool dischargedByDiscard = false;
    // §28.3 / §28.10 (v0.180, #1962): the initializer carries the `owned`
    // binding-position transfer marker — `var y := owned x;` / `using var y :=
    // owned x;`. It is the binding-position form of the §22.3 call-site marker
    // (`M(owned x)`): a MARKED read of a named owned binding is a MOVE (the
    // source is left moved-from and this binding takes over the disposal
    // obligation), as opposed to the UNMARKED read `var y := x;`, which is a
    // borrow. Set by the parser (right after `:=`); consumed by the ownership
    // pass (handleLocalDecl), which validates the source is a named owned
    // binding and records the move.
    bool initializerHasOwnedMarker = false;
    // §28.3 K3.4/K7.4 (v0.209, #2080): set by the ownership pass on a plain
    // `var` owned class/interface local consumed by a MARKED ARM TRANSFER
    // (`owned x` as an if-expression branch or expression-`case`/`when` arm
    // result) — consume-if-selected. Codegen's MarkedTransferExpr lowering
    // NULLS the slot on the selected path, so the K7.4 per-path scope-end
    // disposal is exactly a NULL-GUARDED dynamicDispose registered at the
    // declaration's scope (both edges): the consumed path reloads null and
    // skips; the not-selected path disposes exactly once. The §28.3
    // end-of-method discharge error and the affine auto-drop are both
    // suppressed for a local carrying this flag (the marked arm is a
    // marker-free-position consume; the per-path disposal is the language's
    // job).
    // Monotone (set-only), fixpoint-safe.
    bool dischargedByArmTransfer = false;
    // §17.15 (v0.216, #2098): this LocalDecl is a CLAUSE of a binding atom
    // `(var x := E; body)` (a BindExpr child, A.7 BindClause), not a statement.
    // Set by the parser. A clause binding is single-assignment and not an
    // assignable location (the §21.1 runtime-`const` rule; sema mirrors the
    // const-local rejections), and its ownership boundary is the ATOM (K8.5),
    // not the enclosing scope — the end-of-method scope machinery must skip
    // it. The atom-boundary disposal decision itself carries no AST flag: both
    // the ownership pass and codegen consult the ONE shared predicate
    // (common::bindClauseNeedsAtomDisposal), so an expression-bodied member —
    // whose body the ownership dataflow does not run on — gets the identical
    // disposal (the #1661 no-drift pattern).
    bool isBindAtomClause = false;
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

// A.6 `LocalTypeDecl ::= RecordDecl | EnumDecl`. Method-local type
// declaration (§21.10). Wraps a TypeDecl as a statement.
struct LocalTypeDecl : Stmt {
    std::unique_ptr<TypeDecl> decl;
};

// A.6: `ReturnStmt ::= "return" Expression? ";"`.
struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value;       // null for `return;`
};

// A.6: `ExpressionStmt ::= Expression ";"`. Statement-position expression
// whose value is discarded (call, increment, …).
struct ExpressionStmt : Stmt {
    std::unique_ptr<Expr> expr;
    // fix #2655 / §21.9 / §28.3 / §28.9 (v0.237): set by SEMA — in §21.9's own
    // bare-discard walk (ExprTypecheck.cpp), its single setter — when `expr` is
    // one of that section's `ownedProduced` shapes (a `new`, an `owned`-returning
    // user call, an owning-collection drain; never a borrow) AND the walk
    // classifies the produced class TRIVIAL-DISPOSE. Codegen then frees the
    // value at end of statement, per §28.3's unbound-owned-temporary rule ("an
    // unnamed owned temporary … is reclaimed automatically once the expression
    // that produced it is finished with it") at the boundary §21.9 fixes ("in an
    // expression statement, 'end of the full expression' and 'end of the
    // statement' coincide"). Without it the bare form leaked with zero
    // diagnostics while `_ := new Box();` — the same statement plus §17.13's
    // cosmetic discard sink — did not.
    //
    // The trivial-dispose gate is load-bearing, not decoration: §28.9 delegates
    // only trivial instances' reclaim to "an implementation-defined mechanism …
    // that runs no user code". Setting this from the ownership pass's wider
    // `producerState == Owned` (the first draft) would let a NON-trivial class
    // through wherever §21.9 under-rejects, and the free would then run a user
    // destructor at end of statement. Keeping the setter inside §21.9's walk
    // makes the freed set a strict subset of what that section inspected and
    // declined to reject; see the setter's comment for the full list of shapes
    // that widening admitted.
    bool disposeOwnedValue = false;
};

// A.6: `AssignmentStmt ::= AssignTarget AssignOp Expression ";"`. The
// `target` is parsed as an expression in slice 11; sema decides which
// expression shapes are valid lvalues.
struct AssignmentStmt : Stmt {
    std::unique_ptr<Expr> target;
    std::string_view op;               // ":=", "+=", "-=", "*=", …
    std::unique_ptr<Expr> value;
    // §28.3 / §28.7 / §28.10 (v0.180, #1965): the value carries the `owned`
    // assignment-position transfer marker — `y := owned x;`. It is the
    // assignment-position analog of the binding marker
    // (LocalDecl::initializerHasOwnedMarker, #1962) and the call-site marker
    // (`M(owned x)`, §22.3): a MARKED read of a named owned binding on the
    // right-hand side is a MOVE (the source is left moved-from and the target
    // takes over the disposal obligation, disposing its own displaced value by
    // drop-on-overwrite, §28.7), as opposed to the UNMARKED read `y := x;`,
    // which is a borrow. Set by the parser (right after `:=`); consumed by the
    // ownership pass (handleAssignment → handleMarkedOwnedAssignment). Only
    // meaningful when `op == ":="`.
    bool valueHasOwnedMarker = false;
    // fix #3263 (§28.7 v0.260 / §15.3 v0.260): the LHS was whole-value moved-
    // from at this store site — typically a prior `var b := a;` transferred
    // ownership of `a`'s slots to `b` (the §15.3 bullet-1 whole-array move),
    // so the slot pointers in `a`'s alloca now belong to `b`. §28.7's
    // *Moved-from and null* rule: the drop-on-overwrite pass disposes nothing
    // — the LHS is moved-from, not live. Sema sets this; codegen reads it as
    // the suppression key for `lowerOwningInlineArrayReassign`. The dispose-
    // on-overwrite is suppressed (the stale alloca pointers must NOT be
    // disposed — `b` owns them now and will free them at its scope end).
    // Without this flag, the pass would double-free the moved-out slots
    // (#3263 defect 2). Only meaningful when `op == ":="` and the LHS is a
    // move-only inline-array local/field.
    bool wholeValueLhsMovedFrom = false;
    // fix #3950 (§28.7 v0.294 / §28.3 K1.3 v0.294): the LHS — a plain owned
    // class/interface LOCAL — was in the MOVED-FROM state at this store site
    // (`Sink.Take(move x); x := new Res();`, the "consume then re-acquire in
    // place" idiom). §28.7 is explicit that the drop-on-overwrite pass runs
    // only "if the previous value is live (**non-null and not moved-from**)",
    // and that "a moved-from or null location therefore stores without
    // disposal". Codegen's local pass (disposeDisplacedOwnedClassLocal) tests
    // only the NULL half of that CONJUNCTION — sound for a `using var` /
    // iteration-variable / out-owned recipient, whose slot the move lowering
    // nulls, but NOT for a plain `var` local, whose slot the move lowering
    // deliberately leaves populated (LlvmBackend_Exprs2.cpp: the `new
    // T(move n, n)` idiom reads the second operand from that slot). The pass
    // then disposed a pointer the new owner had already freed — a
    // heap-use-after-free with no loop and no diagnostic. This flag is the
    // MOVED-FROM half, supplied by the one authority that computes it: the
    // ownership dataflow's own converged state at this statement. It is the
    // class-local sibling of `wholeValueLhsMovedFrom` above (#3263 gave the
    // move-only inline array the same treatment for the same §28.7 clause).
    // Set only on the final emit pass, so it records the CONVERGED fact.
    // Only meaningful when `op == ":="` and the LHS is a bare local identifier.
    bool lhsMovedFromAtStore = false;
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

// A.6 (v0.201): `WhileStmt ::= LoopLabel? "while" Expression "do" Statement*
//                               "end" ("while" | Identifier)? ";"`.
struct WhileStmt : Stmt {
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
    // §21.5 (v0.201): the optional loop label (`scan: while …`); empty when the
    // loop is unlabeled. Sema uses it to build the per-callable label stack and
    // to reject duplicates; the closer `end scan;` must match it (§1.6).
    std::string_view label;
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
    // iteration variable (over IOwningEnumerable<T> / IOwningAsyncEnumerable<T>)
    // is undischarged on at least one body-exit path. Codegen reads this flag
    // to register a per-iteration null-guarded disposal at body-end (fall-
    // through / break / continue / return edges) so the variable is auto-
    // dropped each iteration with the must-use warning rather than a hard error.
    bool ownedIterVarAutoDropAtBodyEnd = false;
    // §21.5 (v0.201): the optional loop label (`scan: for …` / `scan: await for
    // …`); empty when unlabeled. See WhileStmt::label.
    std::string_view label;
};

// A.6: `DoEndBlock ::= "do" Statement* "end" ";"`. Statement-sequencing
// block of type `()`; no value produced (rationale §5.13).
struct DoEndBlock : Stmt {
    std::vector<std::unique_ptr<Stmt>> body;
};

// A.6 (v0.201): `BreakStmt ::= "break" Identifier? ";"`,
//               `ContinueStmt ::= "continue" Identifier? ";"`.
// `label` is the optional target loop label; empty is the unlabeled form
// (targets the immediately enclosing loop). `targetLoopOffset` is filled by
// sema (§21.5 resolution): 0 = innermost enclosing loop, 1 = next-out, … — the
// nesting distance from the jump site to the resolved target loop. Codegen reads
// it to pick the target entry in its loop stack (whose depth matches sema's by
// construction — both walk the same AST), disposing every scope crossed.
struct BreakStmt : Stmt {
    std::string_view label;
    int targetLoopOffset = 0;
};
struct ContinueStmt : Stmt {
    std::string_view label;
    int targetLoopOffset = 0;
};

// A.6: `IncrementStmt ::= LValue ("++" | "--") ";"`. Statement-only per
// §17.6 — `++`/`--` are not expression operators.
struct IncrementStmt : Stmt {
    std::unique_ptr<Expr> target;
    std::string_view op; // "++" or "--"
};

// A.6: `DiscardStmt ::= "_" ":=" Expression ";"`. Since v0.244 (proposal 0083,
// #3049) the `_` here is its OWN reserved token (A.1 splits the Identifier
// production), so the parser commits the discard form on that leading token —
// §17.13: "the form is recognized lexically by the leading `_` token". The
// former recovery (an expression statement whose LHS is IdentifierExpr "_"
// followed by `:=`) is gone; `_` is not an expression at all.
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
    // §17.13 / §28.3 v0.184 (#1387, generalized by #1651): set by the ownership
    // pass when `value` is a plain IdentifierExpr referencing a NAMED OWNED
    // BINDING — a plain owned local (any decl form: `var`/`using var`
    // LocalDecl, the `out owned var` inline form, or an owning-enumerable
    // for-loop variable) or an `owned` parameter — that is currently Owned.
    // This is the marker-free-position consume of a named owned binding
    // (v0.184):
    // codegen emits null-guarded dispose+reclaim of the local's slot at this
    // statement and nulls the slot (so the exception-path unwindOnly disposal
    // is a no-op). Distinct from disposeOwnedValue (fresh-producer path) which
    // stores to a separate alloca and does not null an original slot.
    bool discardOutOwnedVar = false;
    // fix #1909 / §17.13 / §28.3 / §28.11: set by the ownership pass when
    // `value` is an owned PRODUCER (producerState == Owned) whose type is an
    // OWNING collection (OwningArray/OwningDictionary/OwningSet/OwningStack/
    // OwningQueue/OwningLinkedList) — the producer-TEMPORARY analogue of
    // disposeOwnedValue above, for the one owned-producer shape that gate
    // deliberately excludes (an owning collection is a decl-null builtin, not
    // a user Class/Interface — see disposeOwnedValue's comment). An unbound
    // owning-collection temporary is disposed at end-of-statement exactly as
    // any other unbound owned temporary (§28.3), cascading into its owned
    // elements per §28.11's owning-collection contract. Codegen routes this
    // through the collection-family runtime free (bz_array_free /
    // bz_dict_free / bz_llist_free), never dynamicDispose (owning
    // collections have no vtable).
    bool disposeOwnedCollectionValue = false;
    // fix #2655 / §17.13 / §28.3 / §28.9 (v0.237): set by the ownership pass
    // when `value` is an owned PRODUCER (producerState == Owned) whose type is
    // the decl-null builtin `StringBuilder` — the spec's OWN §17.13 example
    // (`_ := new StringBuilder();`). disposeOwnedValue above excludes it by its
    // `rt->declaration->decl != nullptr` gate ("builtin handles … need their
    // own free paths (deferred)"), and disposeOwnedCollectionValue covers only
    // the owning-collection family, so the builder's parts array leaked
    // outright. Codegen frees it through the SAME wiring the `using var`
    // StringBuilder path already uses (#1185): bz_array_free with the
    // string-element release thunk. Scoped to StringBuilder alone — the other
    // decl-null handles (Task, Subscription, Shared, Stopwatch, …) have
    // different ownership contracts and no proven free path, and `Task` may not
    // be discarded at all (§17.13's carve-out).
    bool disposeOwnedBuilderValue = false;
};

// A.6: `YieldStmt ::= "yield" "return" Expression ";" | "yield" "break" ";"`.
struct YieldStmt : Stmt {
    bool isBreak = false;            // true for `yield break;`
    std::unique_ptr<Expr> value;     // populated for `yield return Expr;`
};

// A.6 CatchArm (slice 22 + slice 43; v0.167 #1371 respelled the separator):
//   CatchPattern ::= Type ("|" Type)*                          /* no binding */
//                  | "var" Identifier ":" Type ("|" Type)*      /* binding */
//   CatchArm     ::= CatchPattern ("when" Expression)? "=>" ArmBody
// Multi-type alternatives are `|`-separated (the same alternatives spelling as
// case patterns, §A.7 AltPattern); the comma is exclusively the arm separator.
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

// §22.4 (v0.183, #1585) A.5 OutNullPostcond ::= "when" ("true" | "false").
// A nullability postcondition on an `out`/`out owned` parameter of nullable
// type in a bool-returning method: `when true` states the parameter is
// non-null whenever the method returns `true` (and `when false`, whenever it
// returns `false`). Checked in the callee body; consumed by caller narrowing.
enum class OutNullWhen { None, WhenTrue, WhenFalse };

struct Param {
    std::string_view modifier; // "ref", "out", "out owned", "owned", or empty
    std::string_view name;
    std::unique_ptr<Type> type;
    std::unique_ptr<Expr> defaultValue; // null when no `:= Expr`
    // fix #2482: Param is NOT an ast::Node (it carries no span), so without a
    // location of its own a positional record field / class primary-ctor param
    // / choice-case payload field had no per-member source position —
    // diagnostics keyed on the member's declLoc fell back to the enclosing
    // type's line (#2418). The parser stamps this at the parameter's first
    // token; SymbolCollection reads it for the member-shaped Param uses.
    SourceLocation loc;
    // §22.4 (v0.183, #1585): the `when true`/`when false` out-nullability
    // postcondition (A.5 OutNullPostcond), or None when absent.
    OutNullWhen outNullPostcond = OutNullWhen::None;
    // A.4 PositionalField's SerializedClause. Only populated for record
    // positional fields — other Param uses leave this null.
    std::unique_ptr<SerializedClause> serialized;
    // §9.2 EqualityClause on a PositionalField or CaseField (§35.7).
    // Only populated for record positional fields and choice payload
    // fields — other Param uses leave this null.
    std::unique_ptr<EqualityClause> equality;
    // §27.4 (v0.183, #1408) / A.7 `LambdaDestructure`: non-null when this
    // LambdaParam was written as a record-destructuring pattern
    // (`(Entry(k, v)) => ...`) instead of a plain identifier. When set,
    // `name`/`type`/`modifier` are unused (the destructuring head supplies
    // the parameter's type, and the form admits no separate annotation or
    // modifier). Only ever populated for LambdaExpr params — method/ctor/
    // operator/indexer param lists never populate this field.
    std::unique_ptr<DestructuringPattern> destructure;
    // fix #3202 (§22.10 "primary constructors" / §22.5 "Optional parameters
    // and named arguments" / §2.8.3 rule 4, v0.254): the textual form of a
    // §22.10 primary-constructor parameter's default value as carried across
    // a module boundary. The LOCAL side keeps the source-level `defaultValue`
    // Expr* above, evaluated by codegen under the enclosing scope's
    // substitution; an IMPORTED class's synthetic `posParam`
    // (ModuleLoader::makeImportedParam, #2516) has no Expr* to point at, so
    // the encoder's `renderConstExpr` text rides here instead.
    //
    // Both representations are present (or absent) on the same Param: a
    // local primary-ctor parameter has only `defaultValue` (an Expr*) and an
    // empty `defaultText`; a synthesized imported Param has only
    // `defaultText` (a string, the textual default the importer will
    // materialize at construction time) and a null `defaultValue`. Codegen
    // checks `defaultValue` first and falls through to `defaultText` for the
    // imported case — the same two-channel shape MetaParam::defaultText
    // carries on the method-parameter side.
    std::string defaultText;
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
//                      ContractClause* MethodBody`
// MethodBody has three shapes per A.4:
//   `=> Expression ";"` (expression-body)
//   `Statement* "end" Identifier? ";"` (block body)
//   `";"` (abstract — body-less)
enum class MethodBodyKind { Expression, Block, Abstract };

// §22.16 (v0.203, proposal 0043, #2053) — contract clauses.
// A.4 `ContractClause ::= "static"? ("requires" | "ensures") Expression ";"`.
// A clause occupies exactly one enforcement tier (proven/checked/assumed,
// §22.16 "The tier system"); the `static` qualifier demands the proven tier,
// and is the only tier-affecting surface form in this revision.
//
// Slice 1 (this commit) introduces the AST and the parse — the clauses are
// carried but not yet type-checked, lowered, or enforced. Subsequent slices
// add the §22.15 expression-wise purity admission, the evaluation lowering,
// and the runtime + inheritance binding.
struct ContractClause {
    enum class Kind { Requires, Ensures };
    Kind kind = Kind::Requires;
    // A.4 `"static"?` qualifier — meaningful only at the parse tier in this
    // slice; sema (later slice) enforces the proven-tier demand.
    bool isStatic = false;
    std::unique_ptr<Expr> expr;
    SourceLocation loc;  // the clause-introducing keyword (`requires`/`ensures`)
};

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
    // §22.16 (v0.203, #2053): the declaration's `requires`/`ensures` clauses,
    // in declaration order. `requires` and `ensures` interleave textually per
    // the spec's evaluation order (requires prologue, then old-binding, then
    // the body, then ensures epilogue) — the kind tag tells them apart.
    std::vector<ContractClause> contractClauses;
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
    // §22.15 (v0.202, #2052): `pure` on a constructor — admitted on the
    // instance form only; rejected on the static-constructor alternative
    // (no call site exists to consume the fact) and on destructors.
    bool isPure = false;
    std::vector<Param> params;
    std::vector<std::unique_ptr<Stmt>> body;
    // §22.16 (v0.203, #2053): the constructor's `requires`/`ensures` clauses
    // (A.4 places them between the parameter list's `)` and the CtorChain/
    // body). `ensures` over a constructor evaluates at body completion, where
    // every §7.9-required field is assigned.
    std::vector<ContractClause> contractClauses;
};

// A.4: `DestructorDecl ::= "destructor" Statement* "end" "destructor"? ";"`.
// No visibility or parameters per the grammar.
struct DestructorDecl : Member {
    std::vector<std::unique_ptr<Stmt>> body;
};

// §22.16 (v0.203, proposal 0043, #2053): A.4
//   `InvariantDecl ::= "static"? "invariant" Expression ";"`
// A class-body member declaring an instance-state predicate. The class
// invariant of C is the conjunction of every InvariantDecl of C and every
// base class, evaluated base-first, declaration order within each class
// (§22.16 "Class invariants"). Class-body-only per A.4; semantically
// rejected in records, static classes, and outside class bodies (fix
// #4115/#4186).
//
// fix-forward #4206 (§22.16 "Class invariants", "Out of scope in this
// revision", v0.298; corrects this comment, no behavior change): the
// `"static"?` qualifier on an invariant clause INSIDE AN ORDINARY (non-
// static) class body is NOT rejected at sema in this revision — an earlier
// revision of this comment claimed it was ("rejected at sema as
// out-of-fragment"), but no sema check ever existed for it (issue #4206,
// filed reviewing #4186, whose commit had quoted this comment as supporting
// authority for a claim it did not make). §22.16's own "Out of scope in
// this revision" list says only: "`static invariant` parses from day one
// and is useful when the prover fragment lands" — silent on rejection, and
// the codegen side (`Emitter::emitInvariantCheck`, `LlvmBackend_Setup.cpp`)
// confirms the intended reading: it simply excludes `isStatic` invariants
// from runtime instrumentation (`if (inv->expr && !inv->isStatic)`), i.e.
// INERT-until-the-prover-lands, not rejected.
// `checkRecordAndStaticClassInvariantMembers` / `checkEnumChoiceInvariantMembers`
// (TypeResolution_Checks.cpp) reject the wrong BODY KIND (record / static
// class / enum / choice); neither reads `isStatic`, and nothing else does
// either — a `static invariant` in an ordinary class compiles clean today,
// and per the spec text above, should.
//
// Slice 1 carried the parsed member only. Instrumentation landed with #2941
// (construction exit, public-method entry/exit), and the §22.16
// checked/unchecked FACE SPLIT with fix #4597 (v0.306) — see
// `Emitter::uncheckedFaceFor` in LlvmBackend_Setup.cpp. Still outstanding on
// the instrumentation set: public SETTERS (§22.16 lists them as an
// instrumentation point; no site emits a check for one).
struct InvariantDecl : Member {
    bool isStatic = false;
    std::unique_ptr<Expr> expr;
    SourceLocation loc;  // the `invariant` keyword
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
    // §22.15 (v0.202, #2052): `pure get` — purity attaches to the getter
    // only; a `pure set` is rejected semantically. Meaningful only when
    // !isSet (sema enforces).
    bool isPure = false;
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
    // §9.2 EqualityClause on the auto-property form (after `serialized`).
    std::unique_ptr<EqualityClause> equality;
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
    // fix #2482: EnumConstant is NOT an ast::Node and carries no span, so its
    // member declLoc fell back to the enclosing enum type's line (#2418). The
    // parser stamps this at the constant's name token for a precise per-member
    // diagnostic location.
    SourceLocation loc;
};

// §A.3 / §35.1 (v0.200, #2044):
//   ChoiceCase  ::= Identifier CasePayload? SerializedClause?
//   CasePayload ::= "(" CaseField ("," CaseField)* ")"
//   CaseField   ::= "owned"? Identifier ":" Type SerializedClause?
// A case is payload-free when `payload` is empty. The CaseField shape is
// PositionalField plus the optional leading `owned` marker, so we reuse Param
// (whose `modifier` slot carries "owned" when the marker is present). The
// SerializedClause on a case admits the rename form only (semantically
// rejected for `skip`/`optional`, as on enum constants).
struct ChoiceCase {
    std::string_view name;
    // Empty for a payload-free case (`NotModified`). Each CaseField's
    // `modifier` is "" or "owned"; `serialized` carries a per-field rename.
    std::vector<Param> payload;
    // Null when no `serialized "..."` clause is present on the case.
    std::unique_ptr<SerializedClause> serialized;
    // fix #2482: ChoiceCase is NOT an ast::Node and carries no span, so its
    // member declLoc fell back to the enclosing choice type's line (#2418).
    // The parser stamps this at the case's name token for a precise per-member
    // diagnostic location.
    SourceLocation loc;
};

// Class-like top-level declaration. The five kinds share enough shape that one
// struct with a `kind` tag is the right model until per-kind members force a split.
struct TypeDecl : TopDecl {
    TypeDeclKind kind = TypeDeclKind::Class;
    Visibility visibility = Visibility::None;
    // A.3 `Modifier` (v0.242): "abstract", "closed", "sealed", "static".
    std::vector<std::string_view> modifiers;
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
    // §35 Choice-only: the comma-separated cases (`ChoiceCases` per A.3, v0.200
    // #2044). Empty for non-choice kinds.
    std::vector<ChoiceCase> choiceCases;
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
    // #2047: set for a SYNTHESIZED auto-imported stdlib decl (the `Result<T, E>`
    // choice injected into every compilation unit — Driver::injectStdlibPrelude).
    // Its emitted functions use INTERNAL linkage: every module carries its own
    // private copy (choices are not in the cross-module ABI), so external linkage
    // would collide (`multiple definition of bz$Result$IsOk`) when two modules
    // link together.
    bool isSynthesizedPrelude = false;
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
    // fix #4698 (§2.2, v0.313): set by sema when the `Alias.` segment of an
    // alias-qualified compiler-built-in (`Th.CancellationTokenSource`) was
    // dropped from `name`. The type then always names the BUILT-IN: scope
    // lookup (type parameter, module type, imported type) must not capture it.
    bool aliasQualifiedBuiltin = false;
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
    // §22.15 (v0.202, #2052): `pure` admitted on operator declarations.
    // Operators are always `public static` (grammar A.4 OperatorDecl), so
    // `pure` has no `async` conflict; sema checks the body like any pure
    // member.
    bool isPure = false;
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
    // fix #4698 (§2.2, v0.313): set by sema when this identifier is the
    // synthesized head of an alias-qualified compiler-built-in
    // (`D.Stopwatch.StartNew()` under `import System.Diagnostics as D;`, the
    // `Alias.` segment dropped). The alias is a qualifier, not a rename, so the
    // head always names the BUILT-IN: scope lookup (a same-named local,
    // type parameter, module type or imported type) must not capture it.
    bool aliasQualifiedBuiltin = false;
    // fix #4246 (§28.3 / §28.6, v0.298): set by the ownership pass on the
    // RECEIVER occurrence of an explicit `x.Dispose()` / `x?.Dispose()` /
    // `x!.Dispose()` call whose LOCAL is, at THIS exact program point (per
    // the checker's `states_` dataflow, read in `markExplicitDisposeReceiver`
    // just before the call transitions it to Disposed), in the Owned state —
    // even when the local's DECLARATION is a plain borrow alias
    // (`var y := x;`) that only became Owned later via a plain reassignment
    // (`y := new Res(1);`). Codegen's `unwindOwnedLocalClass` classifies
    // purely from the DECLARATION's initializer form and returns null for a
    // borrow-initialized local, so `explicitDisposeOwnedReceiverSlot` fell to
    // dispatch-only — the destructor ran but the heap block LEAKED (the
    // #4246 finding).
    //
    // Deliberately a PER-OCCURRENCE fact (a fresh IdentifierExpr node per
    // source occurrence), NOT a per-declaration one like
    // `LocalDecl::becomesOwnedViaReassign`. That distinction is load-bearing:
    // the SAME local can legally be `.Dispose()`d at a point where it is
    // STILL a genuine borrow (§28.6's documented "recorded gap" — a plain
    // local's borrow-dispose is unenforced, e.g. `var y := x; y.Dispose();
    // y := new Res(1); y.Dispose();` compiles today) as well as later, after
    // it becomes owned. A per-declaration flag would make the EARLIER
    // (still-borrow) call ALSO reclaim — freeing a value another owner (`x`)
    // still holds, a double-free that does not exist today. Reading the
    // dataflow's OWN Owned/Borrowed classification at the EXACT call site is
    // sound however many such calls a program contains, in either order.
    // // per §28.3, §28.6
    bool disposeReclaimsReassignedBorrow = false;
    // fix #4734 (§35.5 *The owning match*, §28.6 "Who may call Dispose()",
    // §17.13, §28.8.1, v0.307): set by the ownership pass on every occurrence
    // of an arm binding an OWNING match transferred a payload into that
    // CONSUMES it — the receiver of an explicit `r.Dispose()`, the operand of
    // a discard `_ := r;`, and the operand of a `move r`. §35.5 makes such a binding "an **owned local of the arm
    // body's scope**" and names both spellings as warning-free discharges, but
    // codegen's `explicitDisposeOwnedReceiverSlot` is LOCALDECL-ONLY and an
    // arm binding's `decl` is a `BindingPattern`, so both fell to its trailing
    // `return nullptr`: the destructor would run and the heap block LEAK.
    // MEASURED pre-fix on `_ := w;`: 24 bytes in 1 allocation under
    // LeakSanitizer, on a program the compiler ACCEPTED.
    //
    // Per-occurrence for the same reason `disposeReclaimsReassignedBorrow` is:
    // only the sites the ownership pass proved to be an owning-match transfer
    // binding are stamped, so a same-named binding of a BORROWING match — whose
    // payload the scrutinee's owner still disposes — can never reclaim.
    // Reclaiming here is exactly-once: #4617's per-slot mark
    // (`dischargeOwningMatchTransfers`) already zeroes the transferred slot in
    // the scrutinee temp at the top of the selected arm, so the construct-
    // completion reclaim declines it.
    //
    // At a `move r` the flag instead asks codegen to NULL the binding's slot
    // after the transfer, which is what keeps a SECOND consume harmless: the
    // ownership dataflow keeps no `states_` entry for a pattern binding, so it
    // cannot reject `using var u := move r; r.Dispose();` the way it rejects
    // that shape on an ordinary owned local (§28.8.1: "a moved-from binding is
    // unreadable until reassigned"). The reclaim above is null-guarded, so a
    // nulled slot makes the later consume a no-op instead of a double free —
    // the same null-site reasoning #4016 records for the K3.4 arm-transfer
    // discharge, and the reason this flag had to cover the `move` spelling
    // before the other two could be admitted at all. A second consume is
    // therefore memory-safe but still UNDIAGNOSED; diagnosing it needs the
    // `states_` entry #4634's auto-drop introduces.
    // // per §35.5, §28.6, §17.13, §28.8.1, v0.307
    bool owningMatchTransferBinding = false;
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
//   `expr "?." Identifier GenericArgs?` — null-conditional member access (`?.`)
//   `expr "?(" ArgList? ")"`  — null-conditional invocation (`?(`)
//   `expr "[" IndexList "]"`  — indexing (multi-dim via comma list)
//   `expr "!"`                — postfix null-forgiving operator
// per §17 / Appendix A.7 PrimarySuffix (issue #1278): the optional `typeArgs`
// mirrors MemberAccessExpr::typeArgs above — explicit method-level type
// arguments on a null-conditional call, `obj?.Method<T>(...)`. Populated by
// the parser under the SAME `< TypeArgList > (` disambiguating scan used for
// the `.` form (§22.12's generics-vs-less-than rule applies identically to
// `?.`).
struct NullCondMemberExpr : Expr {
    std::unique_ptr<Expr> target;
    std::string_view member;
    std::vector<std::unique_ptr<Type>> typeArgs;
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

// §20 / A.7 IfExpr (v0.208): the conditional atom `(if cond then X elif c2
// then Y else Z)` — an elif chain right-nests (elseValue is the next
// ConditionalExpr). The retired postfix spelling produced this same node, so
// sema/ownership/codegen are unchanged by the v0.208 respelling.
struct ConditionalExpr : Expr {
    std::unique_ptr<Expr> thenValue;  // X
    std::unique_ptr<Expr> condition;  // cond
    std::unique_ptr<Expr> elseValue;  // Y (or the nested elif chain)
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

// §9.5 / A.7 `with` PrimarySuffix (v0.208): `EXPR "with" "(" InitAssign (","
// InitAssign)* ","? ")"` where `InitAssign ::= Identifier ":=" Expression`.
// Source-type determines the semantic: record copy-with-overrides or object
// initializer block on a freshly-constructed class (§22.11). The parser
// stores both shapes in the same node — sema dispatches by the source's type.
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

// A.7 `LambdaExpr ::= "async"? "lambda" "(" LambdaParams? ")" (":" Type)?
// "=>" LambdaBody` (§27.3, v0.217). Keyword-led: the reserved `lambda`
// commits the parse at its first token, so no lookahead discriminator
// exists any more (the pre-v0.217 scan-to-`)` lookahead is deleted).
struct LambdaExpr : Expr {
    bool isAsync = false;
    std::vector<Param> params;
    // §27.3 (v0.217): the optional return-type ascription between the
    // parameter list and `=>`. Null when absent (all pre-v0.217 inference
    // unchanged); a Unit type here declares a procedure lambda.
    std::unique_ptr<Type> retType;
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

// §19.3 (v0.183, #1584): a heterogeneous `|` alternative list — a binding-free
// set of membership tests in which at least one member is a RANGE or RELATIONAL
// pattern (`1..5 | 10`, `< 0 | > 100`). A pure-constants alternative list is
// still collapsed by the parser into a single ConstantPattern (the v0.167
// form), so this node appears ONLY when a range/relational member is present.
// Every member is one of three binding-free kinds the downstream passes handle
// uniformly: a single-constant ConstantPattern (the parser folds a NamedPattern
// enum constant and `null` into this form), a RangePattern, or a RelOpPattern;
// a binding or a type member is rejected — the list binds nothing regardless of
// member kind. Matching is the OR of the members'
// individual tests; the list contributes only its constant members to
// exhaustiveness (§19.6 counts declared constants, not range/relational
// extents).
struct AltPattern : Pattern {
    std::vector<std::unique_ptr<Pattern>> members;
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
    // K8 full-expression boundary flag (#1795): the K8 boundary for an unbound
    // owned temporary minted by the scrutinee is the WHOLE `case` construct
    // (statement-position and expression-position alike). Set by the ownership
    // pass when the scrutinee is a fresh owned producer; read by codegen's
    // lowerCaseExpr / lowerCaseStmt to emit the disposal at endBB.
    bool disposeOwnedScrutinee = false;
};

// A.7 `MarkedTransfer ::= "owned" Identifier` (v0.209, #2080): the K3.4
// transfer marker as an expression PrimaryAtom — `owned` is reserved and
// begins no other expression, so the form is additive. Positional admission
// is enforced SEMANTICALLY (§28.3): an if-expression branch or an
// expression-`case`/`when` arm result (consume-if-selected, K3.4/K7.4); the
// binding/assignment RHS forms remain site-parsed flags
// (LocalDecl::initializerHasOwnedMarker / AssignmentStmt), so this node
// never appears there; everywhere else — `??` operands, operator operands,
// guards, argument subexpressions — sema rejects it naming axiom A7's
// narrowed bound. A `case` SCRUTINEE parsed into this node is the separate
// §35.5 owning-match marker production, routed to its own validation.
struct MarkedTransferExpr : Expr {
    std::string_view name;
};

// A.7 `BindExpr ::= "(" BindClause+ Expression ")"`,
//     `BindClause ::= "var" Identifier (":" Type)? ":=" Expression ";"`.
// §17.15 (v0.216, #2098): the binding atom `(var x := E; body)` — a
// PrimaryAtom; "(" + reserved `var` commits the reading at the second token
// like the other paren atoms. Clauses are represented as embedded plain-`var`
// LocalDecl nodes (isBindAtomClause set) so sema's LocalDecl typing and
// codegen's lowerLocalDecl are reused verbatim inside the atom's child scope;
// the body is one full Expression supplying the atom's value, type, and
// ownership classification (the atom is not a K7 join — clauses and body
// evaluate unconditionally).
struct BindExpr : Expr {
    std::vector<std::unique_ptr<LocalDecl>> clauses;  // at least one
    std::unique_ptr<Expr> body;
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
    // fix #3073 / §21.7: the statement-`case` totality grant — set by
    // ExprTC::stmtAlwaysExits when BOTH conditions of the §21.7 rule hold
    // (closed-value-space proof of exhaustiveness AND every arm diverges). When
    // true, the no-match edge is statically dead and codegen MUST lower it as a
    // defined trap (raising MatchFailureException or aborting), not as a
    // fall-through to end-of-callable. Without the field the grant is consumed
    // locally and the lowering cannot tell a granted `case` from any other.
    // Default false; only ever flipped true; never inspected after sema except
    // by Emitter::lowerCaseStmt, so a single AST-mutable field is sufficient
    // (no need for a SemaContext side table).
    bool totalityGranted = false;
    // fix #4139 (§19.6, v0.298): true when this CaseStmt's no-match
    // fallthrough edge is PROVABLY unreachable — either an unguarded
    // catch-all arm (`_` / untyped `var x`) or full closed-value-space
    // coverage (every enum constant, both bool literals, every choice
    // case, every closed-hierarchy leaf, each in its nullable variant) —
    // the SAME §19.6 exhaustiveness verdict `ExprTC::caseArmsExhaustive`
    // computes for the expression-position non-exhaustiveness warning.
    // Deliberately WITHOUT `totalityGranted`'s additional "every arm
    // diverges" conjunction (§21.7): that conjunction only matters for
    // `totalityGranted`'s own consumer (does this case STATEMENT return on
    // every path?); this field answers a narrower, unconditional question
    // ("is the no-match edge dead?") that stays meaningful even when arms
    // do NOT all diverge — which is exactly the ownership pass's use: the
    // CaseStmt arm-join (`MethodOwnership::visitStmt` and the lambda-body
    // pre-pass `applyUsingVarBorrowPinInLambdaStmt`, both Ownership.cpp)
    // fold the pre-case snapshot into the join as a stand-in for the
    // §19.6 no-match fallthrough UNLESS this field is set, in which case
    // that edge is known-dead and folding it in would only over-reject a
    // legitimate reassignment/move/read of a local every live arm
    // consumes. Set unconditionally by ExprTC::visitStmt's CaseStmt arm
    // (sema runs before ownership, Driver.cpp); read only by the
    // ownership pass. Supersedes the ownership pass's own prior syntactic-
    // only `caseHasUnguardedCatchAllArm` predicate, which covered only the
    // catch-all disjunct — see #4139 for the exhaustive-with-no-wildcard
    // false-reject that predicate could not catch.
    bool noMatchEdgeStaticallyDead = false;
    // K8 full-expression boundary flag (#1795): mirrors
    // CaseExpr::disposeOwnedScrutinee for the statement-position form. Set
    // by the ownership pass; read by Emitter::lowerCaseStmt.
    bool disposeOwnedScrutinee = false;
};

// A.3 `ExtensionDecl ::= Visibility? "extension" Type GenericConstraints*
//                          ExtensionMember* "end" ";"`.
struct ExtensionDecl : TopDecl {
    Visibility visibility = Visibility::None;
    std::unique_ptr<Type> targetType;
    std::vector<GenericConstraint> genericConstraints;
    std::vector<std::unique_ptr<Member>> members;
    // §26.5 rule 4 (#2329): populated for synthetic imported declarations so
    // ambiguity diagnostics can identify the original artifact's declaration
    // site. Local declarations leave this empty; the driver already knows the
    // current source path and their parser-stamped span supplies line/column.
    std::string sourceFile;
};

// A.3 `ExternDecl ::= Visibility? "method" Identifier "(" ParamList? ")"
//                      (":" "owned"? Type)?
//                      (":=" "symbol" StringLiteral VariadicClause?)? ";"`.
// VariadicClause ::= "variadic" "(" IntLiteral ")".
// Single FFI method declaration inside an extern block.
struct ExternDecl {
    Visibility visibility = Visibility::None;
    std::string_view name;
    std::vector<Param> params;
    std::unique_ptr<Type> returnType;
    bool returnOwned = false;
    // §32.11: the optional alias clause overrides the symbol name.
    std::string_view symbolAlias;  // empty when no alias
    // §32.11: N in `variadic(N)`; UINT32_MAX means not variadic.
    uint32_t variadicN = UINT32_MAX;
    // fix #2482: ExternDecl is NOT an ast::Node and carries no span, so its
    // member declLoc fell back to the enclosing extern block's line (#2418).
    // The parser stamps this at the decl's first token (the optional visibility
    // keyword, else `method`) for a precise per-member diagnostic location.
    SourceLocation loc;
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

// §12.8 / §22.7 / §22.10 (v0.286, fix #3583): the ONE spelling of "is this
// statement a constructor CHAIN CALL, and is it `this(...)` or `base(...)`?".
// §12.8 puts the chain call in the leading statement position, and four places
// have to recognise it — the resolver's §22.10 convenience-constructor check,
// definite assignment's §22.7 chaining-position gate, the emitter's chain
// lowering, and (new here) the §22.5 binding publication for a `this(...)`
// chain to a primary constructor. Each carried its own copy of the same
// six-line dynamic_cast walk, which is the duplicated-classification shape.
// `Emitter::chainCallOf`, `Emitter`'s `ctorChainsViaThis`,
// `ExprTC::publishPrimaryCtorChainBinding`, `Resolver::isChainCall`
// (TypeResolution_Checks.cpp) and `MethodDA::getChainingCall`
// (DefiniteAssignment.cpp, fix #3624) all read this one now.
// Returns the chain `CallExpr` and sets `isThis`, or null when `s` is not a
// chain call.
inline const CallExpr* chainCallOfStmt(const Stmt& s, bool& isThis) {
    auto* es = dynamic_cast<const ExpressionStmt*>(&s);
    if (!es || !es->expr) return nullptr;
    auto* call = dynamic_cast<const CallExpr*>(es->expr.get());
    if (!call || !call->callee) return nullptr;
    auto* id = dynamic_cast<const IdentifierExpr*>(call->callee.get());
    if (!id) return nullptr;
    if (id->name == "this") { isThis = true; return call; }
    if (id->name == "base") { isThis = false; return call; }
    return nullptr;
}

} // namespace bezel::ast
