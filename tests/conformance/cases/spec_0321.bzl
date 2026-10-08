Member          ::= FieldDecl | MethodDecl | PropertyDecl | IndexerDecl
                  | ConstructorDecl | DestructorDecl
                  | OperatorDecl | NestedTypeDecl | ConstDecl
                  | PolymorphicMember | InvariantDecl
ExtensionMember ::= MethodDecl | ExpressionPropertyDecl
                              /* `static` is rejected inside extension blocks
                                 (§26.3); MethodDecl's "static"?
                                 alternative does not apply here. */

InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer
                  | IfaceStaticMethod | IfaceStaticOperator
IfaceStaticMethod   ::= "static" "pure"? "method" Identifier GenericParams?
                        "(" ParamList? ")" (":" "owned"? Type)?
                        ContractClause* ";"
                              /* same body slot as IfaceMethod (§22.16) */
IfaceStaticOperator ::= "static" "operator" OpToken
                        "(" ParamList? ")" (":" "owned"? Type)? ";"
                              /* §11.8: abstract-only — a body is an
                                 error; static properties/consts are
                                 deferred. Conformance and dispatch rules in
                                 §11.8. */
IfaceMethod     ::= "pure"? "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)?
                    ContractClause* ";"
                              /* clauses are self-delimited; the lone ";" is the
                                 uniform member terminator, clause-bearing or not
                                 (§22.16) */
                              /* `owned` on the return type follows the same
                                 rule as in MethodDecl. An interface declaring
                                 an `owned` return requires implementations to
                                 declare an `owned` return (§11.2's exact-match
                                 rule). */
IfaceProperty   ::= "pure"? "readonly"? "property" Identifier ":" Type
                    ( ";" | Accessor* "end" Identifier? ";" )
IfaceIndexer    ::= "pure"? "readonly"? "property" "[" ParamList "]" ":" Type
                    ( ";" | Accessor* "end" ";" )
                              /* The accessor-block form declares explicit get/set
                                 accessors (e.g. `get; set(owned);`) on an interface
                                 property or indexer; in interface context each
                                 Accessor uses the `;`-terminated AccessorBody (no
                                 implementation), enforced semantically. See §23.3. */
FieldDecl       ::= Visibility "static"? ("owned" | "scoped")? "readonly"?
                    EqualityClause? Identifier ":" Type (":=" Expression)? ";"
                              /* "scoped" fields (§28.14) hold storage-
                                 bounded callbacks: function-typed only,
                                 invocable/overwritable/clearable, never read
                                 out as an escaping value. */
                              /* EqualityClause here is the point of §9.2's
                                 clause: an explicit field is §9.2's
                                 disqualifier, and the clause is what
                                 un-disqualifies it (record bodies; on a
                                 class field, and on a static field
                                 anywhere, it is rejected semantically —
                                 §9.2). SerializedClause never appears on
                                 FieldDecl: explicit fields are not
                                 serialized components of a record
                                 (§9.8). */
ConstDecl       ::= Visibility "static"? "const" Identifier (":" Type)?
                    ":=" ConstExpr ";"

ContractClause  ::= "static"? ("requires" | "ensures") "(" Expression ")"
                              /* the parentheses are clause syntax, not expression
                                 tokens (§22.16): a clause is self-
                                 delimiting and carries no terminator. */
InvariantDecl   ::= "static"? "invariant" "(" Expression ")" ";"
                              /* class bodies only; rejected semantically in
                                 records and static classes (§22.16); the `;` is
                                 the ordinary member terminator. */
                              /* the clause region (§22.16): after the signature's
                                 GenericConstraints*, the tokens requires/ensures
                                 (or static followed by either) open it. The
                                 keyword reading is unconditional: requires,
                                 ensures, and invariant are reserved words
                                 (B.1, v0.260), so no identifier reading exists
                                 anywhere, and the region ends at the first
                                 token that opens no clause. */

MethodDecl      ::= Visibility "static"? ("async" | "pure")? VirtualKind?
                    "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)?
                    GenericConstraints* ContractClause*
                    MethodBody
                              /* clauses are self-delimited (§22.16); every
                                 MethodBody alternative, the lone ";" included, is
                                 reachable at any clause count */
                              /* The optional `owned` modifier on the return
                                 type marks methods whose result transfers
                                 ownership to the caller (§28.8). It applies
                                 to class-typed returns; for non-class types
                                 it is rejected semantically. */
VirtualKind     ::= "abstract" | "virtual" | "sealed"? "override"
MethodBody      ::= "=>" (Expression | AssignmentStmt) ";"
                              /* The AssignmentStmt alternative is
                                 valid only on methods with no declared return
                                 type (unit result); it makes the accessor
                                 shape `set => _x := value;` available to the
                                 identically-shaped unit method:
                                 `method SetX(v: int) => _x := v;` (§22.2).
                                 Enforced semantically. */
                  | Statement* "end" Identifier? ";"
                  | ";"                /* body-less method: requires
                                          VirtualKind = "abstract"; enforced
                                          semantically. Interface methods use
                                          IfaceMethod, not MethodDecl, so the
                                          ";" body here is exclusively for
                                          abstract members. */

PropertyDecl    ::= Visibility "static"? "pure"? VirtualKind?
                    "readonly"? "property" Identifier ":" Type
                    PropertyBody
PropertyBody    ::= "=>" Expression ";"
                  | AutoAccessorVis? (":=" Expression)? SerializedClause? EqualityClause? ";"   /* auto-property */
                              /* the two clauses in the fixed order shown,
                                 each at most once (A.3 EqualityClause). */
                  | Accessor* "end" Identifier? ";"
AutoAccessorVis ::= Visibility ("get" | "set")   /* e.g. `private set` */
Accessor        ::= Visibility? "pure"? "get" AccessorBody
                  | Visibility? "set" SetterParamSpec? AccessorBody
                              /* "pure" is admitted on "get" only — a "pure set"
                                 does not parse (the set alternative carries no
                                 "pure" slot); §22.15 states the rule */
SetterParamSpec ::= "(" "owned" ")"
                              /* Explicit setter parameter spec, used when
                                 the parameter modifier needs to be declared
                                 (typically `owned` for owned-element-collection
                                 indexer setters). The parameter is always
                                 named `value`, and its type is always the
                                 property's declared type, so neither is
                                 written here. When the SetterParamSpec
                                 is omitted (`set` alone), the
                                 implicit `value` parameter has no modifier
                                 and takes a borrow of the property's declared
                                 type (§22.3, §23.3). Only `owned` is
                                 meaningful on the value parameter: it is an
                                 input, so `ref`/`out` do not parse here. */
AccessorBody    ::= "=>" (Expression | AssignmentStmt | "do" Statement* "end") ";"
                  | ";"                                  /* abstract / interface body */
                              /* The single-statement form `set => _x := value;`
                                 (an AssignmentStmt) parallels the §13.6 prose; it
                                 is distinct from the expression form (assignment is
                                 a statement, not an expression) and the do-block
                                 form. */

IndexerDecl     ::= Visibility "static"? "pure"? VirtualKind?
                              /* "static" is over-admitted here and rejected
                                 semantically: indexers may not be static
                                 (§23.5). Parameter default values (":=" in
                                 the shared ParamList) are likewise
                                 over-admitted and rejected: indexer
                                 parameters may not declare defaults
                                 (§23.5). */
                    "readonly"? "property" "[" ParamList "]" ":" Type
                    PropertyBody
                              /* IndexerDecl reuses PropertyBody, whose block
                                 form is `Accessor* "end" Identifier? ";"`.
                                 The optional Identifier is for property
                                 declarations (matching the property's name);
                                 indexers have no name, so the only valid
                                 form for an indexer's block-form `end` is
                                 plain `end;`. Enforced semantically —
                                 IfaceIndexer states the same rule structurally
                                 in its bespoke body; this production's
                                 reuse-plus-semantic-rejection is the grammar's
                                 deliberate shared-production policy. */

ConstructorDecl ::= Visibility "pure"? "constructor" "(" ParamList? ")"
                    ContractClause*
                              /* the static-constructor alternative admits no
                                 "pure" (§22.15) and no clauses (§22.16) */
                    CtorChain? Statement* "end" "constructor"? ";"
                  | "static" "constructor" "(" ")"
                    Statement* "end" "constructor"? ";"
CtorChain       ::= ("base" | "this") "(" ArgList? ")" ";"

DestructorDecl  ::= "destructor" Statement* "end" "destructor"? ";"

OperatorDecl    ::= "public" "static" "pure"? "method" "operator" OpToken
                    "(" ParamList ")" ":" "owned"? Type OperatorBody
                              /* `owned` on an operator's return type is
                                 permitted for class-typed operators that
                                 allocate. Most operator overloads return
                                 records or value types and do not need it.
                                 ParamList is the shared production; parameter
                                 modifiers on operator parameters are rejected
                                 semantically (§25.2). */
OperatorBody    ::= "=>" Expression ";"
                  | Statement* "end" ("operator" OpToken)? ";"
OpToken         ::= "+" | "-" | "*" | "/" | "mod" | "&" | "|" | "^"
                  | "<<" | ">>" | "<" | ">" | "<=" | ">="
                  | "=" | "<>" | "~" | "++" | "--"

NestedTypeDecl  ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl
                              /* The reused productions carry `Visibility?` for
                                 the top-level default-internal rule (§13.2).
                                 In nested position the visibility keyword is
                                 required (§13.3, §11.6); omission is
                                 rejected semantically. */

ParamList       ::= Param ("," Param)* ","?
Param           ::= ParamModifiers? Identifier ":" Type (":=" Expression)?
                    OutNullPostcond?
                              /* A method/constructor parameter cannot be named
                                 `_` — the bare token is not an Identifier
                                 (A.1), and the rejection diagnostic says why:
                                 parameter names are call-site-addressable API
                                 surface (§22.5, §2.8.2); choose a name
                                 (§17.13). Lambda parameters differ: LambdaParam
                                 (A.7) admits the bare-`_` ignore. */
                              /* `owned` precedes the identifier in every
                                 named-declaration position — Param, LambdaParam
                                 (via FuncParamMods, A.7), FieldDecl,
                                 CaseField, StaticReadonlyDecl; the post-colon
                                 spelling `x: owned T` does not parse (§28.1:
                                 the modifier attaches to the declaration, not
                                 the type). The pre-Type spelling ("owned"?
                                 Type) exists only in positions with no
                                 identifier — return types and FunctionType
                                 slots. */
OutNullPostcond ::= "when" ("true" | "false")
                              /* (§22.4): admitted only on `out` /
                                 `out owned` parameters of nullable type in
                                 bool-returning methods — a checked non-null
                                 postcondition tied to the stated result. No
                                 parse ambiguity: out parameters never carry
                                 defaults (§22.5), so the clause always
                                 directly follows the Type. */
ParamModifiers  ::= "ref"
                  | "out" "owned"?
                  | "owned"
                  | "scoped"
                              /* "scoped" is valid on function-typed
                                 parameters (§28.14: storage-bounded
                                 callback capture) and on class/interface-typed
                                 parameters (§28.14: storage-
                                 bounded borrow — scoped construction). On a
                                 static method it requires an `owned` return
                                 (§28.14). Other parameter types are
                                 rejected semantically. Illegal on extern
                                 declarations (§32). */
ArgList         ::= Argument ("," Argument)* ","?
Argument        ::= ArgModifiers ArgExpression
                  | (Identifier ":=")? ArgExpression
                              /* ref/out/scoped arguments are positional-only
                                 (§22.5): the modifiers inhabit the first
                                 alternative, which has no named form. A
                                 move-rooted argument is an ordinary named or
                                 positional argument — the marker is part of
                                 the argument EXPRESSION, so `M(item := move x)`
                                 parses and is legal (§22.5, v0.260). The
                                 operand category for ref/out arguments is
                                 semantic: §22.3 "ref and out argument
                                 locations". Named arguments are additionally
                                 rejected at the index position (§23.5) —
                                 admitted by the shared grammar, rejected
                                 semantically. */
ArgModifiers    ::= "ref"
                  | "out" "var"?
                  | "out" "using" "var"
                  | "out" "owned" "var"
                  | "scoped"
                              /* The bare "owned" alternative was deleted at
                                 v0.246 (proposal 0090): the call-site transfer
                                 marker is part of the argument EXPRESSION —
                                 `M(move x)` parses through the plain
                                 ArgExpression path as an identifier-operand
                                 MoveExpr (A.7; §22.3, §28.8.1). "out owned var"
                                 stays: it declares a fresh owned binding at
                                 the call site — a declaration position
                                 (§22.4), on `owned`'s declarative side of the
                                 line. "scoped" (§22.3/§28.14) marks a
                                 class/interface-typed borrow argument to a
                                 `scoped` parameter — `M(scoped x)`; fresh
                                 producers rejected (§9.7). `scoped` remains an
                                 argument modifier: it marks a lending mode,
                                 not an action — the asymmetry with `move` is
                                 deliberate (§22.3). */
ArgExpression   ::= Expression | "_"
                              /* The "_" alternative is admitted by the grammar
                                 in any argument position and is the ONLY
                                 reading of `_` there — no Expression contains
                                 the bare token (A.1). It is semantically valid
                                 only when the corresponding parameter is `out`
                                 and the caller chooses to ignore the produced
                                 value (§22.4); in every non-out position —
                                 `ref _` included — it is a compile error
                                 (§17.13: a `ref` argument reads its operand,
                                 which a discard cannot supply). */

EnumMember      ::= MethodDecl | ExpressionPropertyDecl
                  | StaticReadonlyDecl | StaticConstDecl
                  | OperatorDecl
                              /* enums may overload operators
                                 (§25, §10.4). OperatorDecl is `public static`
                                 only, as everywhere. */
ExpressionPropertyDecl ::= Visibility "pure"? "readonly"? "property" Identifier ":" Type "=>" Expression ";"
                  /* the "readonly"? slot is a shared-production over-admission:
                     writing "readonly" on an expression-bodied property is
                     rejected semantically — §23.2's one-meaning rule (readonly
                     is written exactly where shape cannot express
                     read-onlyness, and an expression body already does). */
StaticReadonlyDecl     ::= Visibility "static" "owned"? "readonly" Identifier ":" Type ":=" Expression ";"
                  /* "owned": a static owned readonly field on an enum
                     participates in §22.13's program-exit disposal exactly as on
                     a class; modifier order per §11.5. */
StaticConstDecl        ::= Visibility "const" Identifier (":" Type)? ":=" ConstExpr ";"

Visibility      ::= "public" | "internal" | "protected" | "private"

GenericParams   ::= "<" GenericParam ("," GenericParam)* ","? ">"
GenericParam    ::= Variance? Identifier
Variance        ::= "in" | "out"
                              /* Variance markers are semantically valid only on
                                 interface type parameters (§12.9; and not
                                 on interfaces declaring static members, §11.8).
                                 On class/record/method/alias parameters the
                                 marker is rejected semantically. */
GenericConstraints ::= "where" Identifier ":" Constraint ("," Constraint)* ","?
                              /* The former `where T is owned` modifier-
                                 constraint form is retired along with `<owned T>`
                                 generic arguments (§14.4); constraints bound
                                 types only. */
Constraint      ::= "class" | "value" | "notnull" | "sendable" | "hashable" | "new" "(" ")" | Type
                              /* "new" "(" ")" is the parameterless-constructor
                                 constraint, three tokens with whitespace
                                 permitted (lexer-level) — `new()` and
                                 `new ()` are equivalent. */
