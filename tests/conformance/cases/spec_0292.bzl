Statement       ::= LocalDecl | LocalConstDecl | AssignmentStmt | ExpressionStmt
                  | DiscardStmt
                  | IfStmt | WhileStmt | ForStmt | DoEndBlock
                  | ReturnStmt | ThrowStmt | BreakStmt | ContinueStmt
                  | TryStmt | YieldStmt
                  | IncrementStmt

LocalDecl       ::= "var" Identifier (":" Type)? (":=" Expression)? ";"
                  | "using" "var" Identifier (":" Type)? ":=" Expression ";"
                  | "await" "using" "var" Identifier (":" Type)? ":=" Expression ";"
                              /* The `await using var` form is admitted by the
                                 grammar at any LocalDecl position; it is
                                 valid only inside `async` methods and `async`
                                 lambdas, enforced semantically (§28.12,
                                 Appendix C.6). */

LocalConstDecl  ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"
                                /* local const; no visibility or static modifier */

AssignmentStmt  ::= LValue AssignOp Expression ";"
AssignOp        ::= ":=" | "+=" | "-=" | "*=" | "/=" | "&=" | "|=" | "^="
                  | "<<=" | ">>=" | "??="
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
                    "end" ";"

WhileStmt       ::= "while" Expression "do" Statement* "end" ";"

ForStmt         ::= "for" "var" Identifier (":" Type)? "in" Expression
                    "do" Statement* "end" ";"
                  | "await" "for" "var" Identifier (":" Type)? "in" Expression
                    ("with" Expression)? "do" Statement* "end" ";"

DoEndBlock      ::= "do" Statement* "end" ";"

ReturnStmt      ::= "return" Expression? ";"
ThrowStmt       ::= "throw" Expression? ";"
BreakStmt       ::= "break" ";"
ContinueStmt    ::= "continue" ";"

TryStmt         ::= "try" Statement* CatchBlock FinallyClause? "end" ";"
                  | "try" Statement* FinallyClause "end" ";"
CatchBlock      ::= "catch" CatchArm ("," CatchArm)* ","?
CatchArm        ::= CatchPattern ("when" Expression)? "=>" ArmBody
CatchPattern    ::= Type ("," Type)*                          /* no binding */
                  | "var" Identifier ":" Type ("," Type)*      /* binding */
FinallyClause   ::= "finally" Statement*

YieldStmt       ::= "yield" "return" Expression ";"
                  | "yield" "break" ";"
