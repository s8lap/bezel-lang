Statement       ::= LocalDecl | LocalConstDecl | AssignmentStmt | ExpressionStmt
                  | DiscardStmt
                  | IfStmt | WhileStmt | ForStmt | DoEndBlock
                  | CaseStmt | WhenStmt
                  | ReturnStmt | ThrowStmt | BreakStmt | ContinueStmt
                  | TryStmt | YieldStmt
                  | IncrementStmt
                  | LocalTypeDecl

LocalTypeDecl   ::= RecordDecl | EnumDecl
                              /* Method-local type declaration (§21.10):
                                 `record`/`enum` are reserved and begin no other
                                 statement — token-one commit, no existing program
                                 changes meaning. The reused A.3 productions are
                                 restricted semantically: the Visibility? slot must
                                 be empty (a local type has no access surface —
                                 contrast §11.6, where nested types REQUIRE the
                                 keyword) and the GenericParams? slot must be empty
                                 (v1). Scope: the declaration to the innermost
                                 enclosing block's end, like a local variable
                                 (§21.6); order-significant (no forward references);
                                 no signature positions; no shadowing of a visible
                                 type name (§21.10). References to enclosing generic
                                 parameters are rejected (v1, §21.10). A.3
                                 (RecordDecl, EnumDecl) is unchanged. */

CaseStmt        ::= "case" Expression "of" CaseArm ("," CaseArm)* ","? "end" "case"? ";"
WhenStmt        ::= "when" WhenArm ("," WhenArm)* ","? "end" "when"? ";"
                              /* The statement forms of §19's constructs:
                                 `end`-terminated, with the statement's `;`
                                 following the closer. Arms use the same CaseArm/
                                 WhenArm grammar as the A.7 atoms, and here
                                 ArmBody additionally admits its Statement and
                                 `do … end` alternatives; an expression-bodied
                                 arm's value is discarded per §21.9, applied per
                                 arm. The former `"owned"?` scrutinee marker was
                                 deleted at v0.246 (proposal 0090): the scrutinee
                                 transfer marker is an ordinary scrutinee
                                 Expression whose root is an identifier-operand
                                 MoveExpr — `case move x of` — under §35.5's
                                 rules, identical to the atom's. */

LocalDecl       ::= "var" Identifier (":" Type)? (":=" Expression)? ";"
                  | "using" "var" (Identifier | "_") (":" Type)? ":=" Expression ";"
                  | "await" "using" "var" (Identifier | "_") (":" Type)? ":=" Expression ";"
                              /* `var _ := e;` does not parse as a LocalDecl —
                                 `_` is not an Identifier (A.1) — and is
                                 rejected with the two-remedy diagnostic:
                                 `_ := e;` (end of statement) or
                                 `using var _ := e;` (scope end) — §17.13,
                                 §21.1. The `"_"` alternative on the using-var
                                 forms is the anonymous owning binding
                                 (§17.13, §28.10): scope-end disposal, no name
                                 bound. Its `(":" Type)?` ascription slot is
                                 retained (`using var _: Stream := e;`) — the
                                 no-annotation rule governs the bare ignores,
                                 not this binding. */
                              /* The `await using var` form is admitted by the
                                 grammar at any LocalDecl position; it is
                                 valid only inside `async` methods and `async`
                                 lambdas, enforced semantically (§28.12,
                                 Appendix C.6). The binding-position
                                 transfer marker is the identifier-operand
                                 MoveExpr (A.7) at RHS root: `var y := move x;`
                                 is the MoveExpr as the entire RHS; admission
                                 is positional, enforced
                                 semantically (§28.3). A
                                 LocalDecl with neither a type ascription nor an
                                 initializer is rejected semantically (nothing
                                 determines the type; typed uninitialized locals
                                 follow §7.9's assignment-before-read rules). */

LocalConstDecl  ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"
                              /* Initializer classified per §21.1:
                                 a §17.14 constant expression of primitive or
                                 string type is a compile-time constant;
                                 anything else is a runtime const. ConstExpr
                                 remains Expression, restricted semantically. */
                                /* local const; no visibility or static modifier */

AssignmentStmt  ::= LValue AssignOp Expression ";"
                              /* The assignment-position transfer marker
                                 is the identifier-operand MoveExpr (A.7) at
                                 RHS root: `y := move x;`. Valid only with ":=";
                                 admission is positional, enforced semantically
                                 (§28.3). */
AssignOp        ::= ":=" | "+=" | "-=" | "*=" | "/=" | "mod=" | "&=" | "|="
                  | "^=" | "<<=" | ">>=" | "??="
                              /* `mod=` is one token, lexed by longest match:
                                 the keyword `mod` immediately followed by `=`
                                 with no intervening space. `x mod = y` remains
                                 a syntax error. */
LValue          ::= Identifier | "this"
                  | LValue "." Identifier
                  | LValue "[" ArgList "]"
                              /* LValues have a restricted shape: the base must
                                 be `Identifier` or `this`, and only `.field`
                                 and `[index]` accesses extend the chain.
                                 Method-call results cannot serve as the base
                                 of an assignment; `getCache().items[0] := v`
                                 does not parse. Such cases require extracting
                                 the call result into a local variable first:
                                 `var c := getCache(); c.items[0] := v;`. */

DiscardStmt     ::= "_" ":=" Expression ";"            /* explicit discard */

IncrementStmt   ::= LValue ("++" | "--") ";"
ExpressionStmt  ::= Expression ";"

IfStmt          ::= "if" Expression "then" Statement*
                    ("elif" Expression "then" Statement*)*
                    ("else" Statement*)?
                    "end" "if"? ";"
                              /* every statement-level closer
                                 may optionally repeat its introducing keyword,
                                 checked when present — one uniform end-label
                                 rule (§1.6). */

LoopLabel       ::= Identifier ":"
                              /* only loop statements take a loop label.
                                 The Identifier closer alternative below is
                                 admitted grammatically on every loop and
                                 checked per §1.6: it must equal the loop's
                                 label; on an unlabeled loop an identifier
                                 closer is rejected, and on a labeled loop the
                                 keyword closer is rejected (the construct is
                                 named). No parse ambiguity: no statement
                                 begins Identifier ":" (":=" is one lexeme), and
                                 after "Identifier :" the parser requires "for",
                                 "while", or "await" "for". */

WhileStmt       ::= LoopLabel? "while" Expression "do" Statement*
                    "end" ("while" | Identifier)? ";"

ForStmt         ::= LoopLabel? "for" "var" (Identifier | "_") (":" Type)? "in" Expression
                    "do" Statement* "end" ("for" | Identifier)? ";"
                  | LoopLabel? "await" "for" "var" (Identifier | "_") (":" Type)? "in" Expression
                    ("with" Expression)? "do" Statement* "end" ("for" | Identifier)? ";"
                              /* The `"_"` alternative is the loop-variable
                                 ignore (§21.4, §17.13): binds nothing. The
                                 grammar over-admits the ascription on it;
                                 `for var _: T in …` is rejected semantically —
                                 a bare ignore takes its type from context,
                                 never an annotation (§17.13). */

DoEndBlock      ::= "do" Statement* "end" "do"? ";"

ReturnStmt      ::= "return" Expression? ";"
ThrowStmt       ::= "throw" Expression? ";"
BreakStmt       ::= "break" Identifier? ";"
ContinueStmt    ::= "continue" Identifier? ";"

TryStmt         ::= "try" Statement* CatchBlock FinallyClause? "end" "try"? ";"
                  | "try" Statement* FinallyClause "end" "try"? ";"
CatchBlock      ::= "catch" CatchArm ("," CatchArm)* ","?
CatchArm        ::= CatchPattern ("when" Expression)? "=>" ArmBody
CatchPattern    ::= Type ("|" Type)*                          /* no binding */
                  | "var" Identifier ":" Type ("|" Type)*      /* binding */
                              /* Multi-type alternatives are `|`-separated
                                 — the same alternatives
                                 spelling as case patterns (AltPattern). */
FinallyClause   ::= "finally" Statement*

YieldStmt       ::= "yield" "return" Expression ";"
                  | "yield" "break" ";"
