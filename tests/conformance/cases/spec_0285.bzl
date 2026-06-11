Member          ::= FieldDecl | MethodDecl | PropertyDecl | IndexerDecl
                  | EventDecl | ConstructorDecl | DestructorDecl
                  | OperatorDecl | NestedTypeDecl | ConstDecl
ExtensionMember ::= MethodDecl | ExpressionPropertyDecl

InterfaceMember ::= IfaceMethod | IfaceProperty | IfaceIndexer | IfaceEvent
IfaceMethod     ::= "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)? ";"
                              /* `owned` on the return type follows the same
                                 rule as in MethodDecl. An interface declaring
                                 an `owned` return requires implementations to
                                 declare an `owned` return (§11.2's exact-match
                                 rule). */
IfaceProperty   ::= "readonly"? "property" Identifier ":" Type ";"
IfaceIndexer    ::= "readonly"? "property" "[" ParamList "]" ":" Type ";"
IfaceEvent      ::= "async"? "event" Identifier (":" Type)? ";"

FieldDecl       ::= Visibility "static"? "owned"? "readonly"?
                    Identifier ":" Type (":=" Expression)? ";"
ConstDecl       ::= Visibility "static"? "const" Identifier (":" Type)?
                    ":=" ConstExpr ";"

MethodDecl      ::= Visibility "static"? "async"? VirtualKind?
                    "method" Identifier GenericParams?
                    "(" ParamList? ")" (":" "owned"? Type)?
                    GenericConstraints*
                    MethodBody
                              /* The optional `owned` modifier on the return
                                 type marks methods whose result transfers
                                 ownership to the caller (§28.8). It applies
                                 to class-typed returns; for non-class types
                                 it is rejected semantically. */
VirtualKind     ::= "abstract" | "virtual" | "sealed"? "override"
MethodBody      ::= "=>" Expression ";"
                  | Statement* "end" Identifier? ";"
                  | ";"                /* body-less method: requires
                                          VirtualKind = "abstract"; enforced
                                          semantically. Interface methods use
                                          IfaceMethod, not MethodDecl, so the
                                          ";" body here is exclusively for
                                          abstract members. */

PropertyDecl    ::= Visibility "static"? VirtualKind?
                    "readonly"? "property" Identifier ":" Type
                    PropertyBody
PropertyBody    ::= "=>" Expression ";"
                  | AutoAccessorVis? (":=" Expression)? SerializedClause? ";"   /* auto-property */
                  | Accessor* "end" Identifier? ";"
AutoAccessorVis ::= Visibility ("get" | "set")   /* e.g. `private set` */
Accessor        ::= Visibility? "get" AccessorBody
                  | Visibility? "set" SetterParamSpec? GenericConstraints? AccessorBody
SetterParamSpec ::= "(" ParamMod? Identifier? ")"
                              /* Explicit setter parameter spec, used when
                                 the parameter modifier needs to be declared
                                 (typically `owned` for owned-element-collection
                                 indexer setters). The parameter's type is
                                 always the property's declared type, so it
                                 is not repeated here. When the parameter
                                 name is omitted (only the modifier given,
                                 as in `set(owned)`), the implicit name
                                 remains `value`. When the SetterParamSpec
                                 is omitted entirely (`set` alone), the
                                 implicit `value` parameter has no modifier
                                 (borrow-taking per §14.4 Rule 2 after
                                 substitution). */
ParamMod        ::= "owned" | "ref" | "out" | "out" "owned"
AccessorBody    ::= "=>" (Expression | "do" Statement* "end") ";"
                  | ";"                                  /* abstract / interface body */

IndexerDecl     ::= Visibility "static"? VirtualKind?
                    "readonly"? "property" "[" ParamList "]" ":" Type
                    PropertyBody
                              /* IndexerDecl reuses PropertyBody, whose block
                                 form is `Accessor* "end" Identifier? ";"`.
                                 The optional Identifier is for property
                                 declarations (matching the property's name);
                                 indexers have no name, so the only valid
                                 form for an indexer's block-form `end` is
                                 plain `end;`. Enforced semantically. */

EventDecl       ::= Visibility "static"? "async"? "event" Identifier (":" Type)? ";"

ConstructorDecl ::= Visibility "constructor" "(" ParamList? ")"
                    CtorChain? Statement* "end" "constructor"? ";"
                  | "static" "constructor" "(" ")"
                    Statement* "end" "constructor"? ";"
CtorChain       ::= ("base" | "this") "(" ArgList? ")" ";"

DestructorDecl  ::= "destructor" Statement* "end" "destructor"? ";"

OperatorDecl    ::= Visibility "static" "method" "operator" OpToken
                    "(" ParamList ")" ":" "owned"? Type OperatorBody
                              /* `owned` on an operator's return type is
                                 permitted for class-typed operators that
                                 allocate. Most operator overloads return
                                 records or value types and do not need it. */
OperatorBody    ::= "=>" Expression ";"
                  | Statement* "end" ("operator" OpToken)? ";"
OpToken         ::= "+" | "-" | "*" | "/" | "mod" | "&" | "|" | "^"
                  | "<<" | ">>" | "<" | ">" | "<=" | ">="
                  | "=" | "<>" | "~" | "++" | "--"

NestedTypeDecl  ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl

ParamList       ::= Param ("," Param)*
Param           ::= ParamModifiers? Identifier ":" Type (":=" Expression)?
ParamModifiers  ::= "ref"
                  | "out" "owned"?
                  | "owned"
ArgList         ::= Argument ("," Argument)*
Argument        ::= ArgModifiers? (Identifier ":=")? ArgExpression
ArgModifiers    ::= "ref"
                  | "out" "var"?
                  | "out" "using" "var"
                  | "out" "owned"
                  | "owned"
ArgExpression   ::= Expression | "_"
                              /* The "_" alternative is admitted by the grammar
                                 in any argument position; it is semantically
                                 valid only when the corresponding parameter
                                 is `out` and the caller chooses to ignore the
                                 produced value (§22.4). The compiler rejects
                                 "_" in non-out positions. */

EnumMember      ::= MethodDecl | ExpressionPropertyDecl
                  | StaticReadonlyDecl | StaticConstDecl
ExpressionPropertyDecl ::= Visibility "readonly"? "property" Identifier ":" Type "=>" Expression ";"
                  /* "readonly" is redundant on expression-bodied properties (no setter exists);
                     it is permitted for consistency with block-form property syntax. */
StaticReadonlyDecl     ::= Visibility "static" "readonly" Identifier ":" Type ":=" Expression ";"
StaticConstDecl        ::= Visibility "const" Identifier (":" Type)? ":=" ConstExpr ";"

Visibility      ::= "public" | "internal" | "protected" | "private"

GenericParams   ::= "<" GenericParam ("," GenericParam)* ">"
GenericParam    ::= Variance? Identifier
Variance        ::= "in" | "out"
GenericConstraints ::= "where" Identifier ":" Constraint ("," Constraint)*
                  | "where" Identifier "is" ModifierConstraint
                              /* The `is`-form constraint conditions on the
                                 modifier of T's argument rather than on a
                                 type bound. Used in stdlib declarations
                                 to condition methods on `owned T` vs
                                 unmodified T. See §14.4. */
Constraint      ::= "class" | "value" | "notnull" | "new" "(" ")" | Type
                              /* "new" "(" ")" is the parameterless-constructor
                                 constraint, three tokens with whitespace
                                 permitted (lexer-level) — `new()` and
                                 `new ()` are equivalent. */
ModifierConstraint ::= "owned" | "not" "owned"
                              /* `is owned` matches when T's argument carries
                                 the `owned` modifier; `is not owned` matches
                                 when it does not. */
