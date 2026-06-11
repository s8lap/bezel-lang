Expression      ::= ConditionalExpr
ConditionalExpr ::= LogicalOr ("if" Expression "else" Expression)?
                  | CaseExpr
                  | ThrowExpr
ThrowExpr       ::= "throw" Expression?
                              /* Bare "throw" (no operand) is valid only inside a
                                 catch arm body; outside a catch arm body, the
                                 operand is required. Enforced semantically. */
LogicalOr       ::= LogicalAnd ("or" LogicalAnd)*
LogicalAnd      ::= NullCoalesce ("and" NullCoalesce)*
NullCoalesce    ::= Equality ("??" NullCoalesce)?
Equality        ::= Relational (("=" | "<>") Relational)*
Relational      ::= BitwiseOr (RelTail)*
RelTail         ::= RelOp BitwiseOr
                  | TypeRel TypeOperand
RelOp           ::= "<" | ">" | "<=" | ">=" | "in"
TypeRel         ::= "is" "not"? | "as"
TypeOperand     ::= Type | "null"
                              /* "is", "is not", and "as" take a Type on the
                                 right so that generic type arguments parse
                                 correctly: `x is Array<int>` parses with
                                 `Array<int>` as a Type, not as the bitwise
                                 expression `Array < int >`. The `null`
                                 alternative covers `is null` and `is not null`
                                 (§17.4); `as null` is a type error caught
                                 semantically. The other relational operators
                                 (`<`, `>`, `<=`, `>=`, `in`) take ordinary
                                 expressions and use RelOp. */
BitwiseOr       ::= BitwiseXor ("|" BitwiseXor)*
BitwiseXor      ::= BitwiseAnd ("^" BitwiseAnd)*
BitwiseAnd      ::= RangeExpr ("&" RangeExpr)*
RangeExpr       ::= Shift                                    /* no range */
                  | Shift (".." | "..<") Shift ("step" Shift)?
                                                             /* closed: a..b, a..<b,
                                                                with optional step */
                  | Shift ".."                               /* low-only: a..  */
                  | (".." | "..<") Shift                     /* high-only: ..b, ..<b */
                  | ".."                                     /* fully open: ..  */
                              /* Range bounds parse as Shift expressions, so
                                 `0..n+1` and `1<<2..1<<10` parse correctly,
                                 while `score in 60..100` and `r = 0..10`
                                 parse with the range as the right operand
                                 of the looser `in`/`=` operator. The open-
                                 ended forms support pattern matching (`..<0`
                                 matches values less than 0, `31..` matches
                                 values at or above 31; §19.3) and array
                                 slicing (`arr[..5]`, `arr[10..]`, `arr[..]`;
                                 §16.2). `step` is admitted only on closed
                                 ranges; open-ended ranges have no enumerable
                                 step semantics. */
Shift           ::= Additive (("<<" | ">>") Additive)*
Additive        ::= Multiplicative (("+" | "-") Multiplicative)*
Multiplicative  ::= UnaryExpr (("*" | "/" | "mod") UnaryExpr)*
UnaryExpr       ::= ("+" | "-" | "not" | "~")? PrimaryExpr
                  | "await" UnaryExpr      /* unary-precedence; right-recursive
                                              so `await await x` is permitted */
PrimaryExpr     ::= PrimaryAtom PrimarySuffix*
PrimarySuffix   ::= "." Identifier GenericArgs?
                  | "?." Identifier GenericArgs?
                              /* The optional GenericArgs supplies explicit
                                 method type arguments at a call site:
                                 `obj.Method<int>(args)`. The GenericArgs
                                 belong to the member name; a following
                                 `"(" ArgList? ")"` suffix is the call.
                                 Same "generic arguments in an expression"
                                 family as the PrimaryAtom `Identifier
                                 GenericArgs?` form below. See §22.12. */
                  | "[" ArgList "]"
                  | "(" ArgList? ")"
                  | "?(" ArgList? ")"
                  | "!"
                  | "with" InitAssign ("," InitAssign)* ","? "end"
                              /* The `with` suffix produces either a copy-with-
                                 overrides (when the source is a record) or
                                 setter-mutation on a freshly-constructed class
                                 (when the source is `new C(...)` — object
                                 initializer, §22.11). The form is syntactically
                                 unified; the type checker resolves the semantic
                                 difference from the source expression's type.
                                 Placing `with` at suffix level eliminates the
                                 left-recursive ambiguity that an earlier
                                 grammar had with a standalone WithExpr at
                                 PrimaryAtom level. */

PrimaryAtom     ::= Literal
                  | Identifier GenericArgs?
                              /* The optional GenericArgs lets a generic type
                                 name appear in receiver/expression position so
                                 its static members can be accessed with the type
                                 arguments supplied — `Registry<int>.Register()`,
                                 `Cache<string>.Default`. Without a trailing
                                 member-access (or call) suffix, a bare
                                 `Identifier GenericArgs` is not a complete
                                 expression. The `<` that opens GenericArgs here
                                 is the well-known generics-vs-less-than ambiguity;
                                 the parser admits the GenericArgs reading only
                                 when the `<…>` is balanced and followed by a
                                 token that can continue a primary expression
                                 (`.`, `?.`, `(`, `[`), otherwise `<` is the
                                 relational operator. See §22.12. */
                  | "(" Expression ")"
                  | NewExpr
                  | MoveExpr
                  | LambdaExpr
                  | CaseExpr
                  | "this" | "base"
                  | NameOfExpr | TypeOfExpr | DefaultExpr
                  | CollectionLiteral
                  | "(" ")"                             /* unit value */

MoveExpr        ::= "move" PrimaryExpr
                              /* The PrimaryExpr operand must syntactically
                                 resolve to either: (a) a field access on a
                                 class instance — `Identifier "." Identifier`
                                 (this.f abbreviated as f) or
                                 `PrimaryExpr "." Identifier` (obj.f), where
                                 the named field is `owned`; or (b) an
                                 indexer access `PrimaryExpr "[" Expression "]"`
                                 where the receiver is an owning collection
                                 admitting slot-level move (`OwnedArray<T>`,
                                 `OwnedDictionary<K, V>`, `OwnedBuffer<T>`
                                 for class-typed T, `Span<T>` over class-typed
                                 `OwnedBuffer<T>`, or an owning inline array
                                 `[N owned]…`). Both rejections are compile
                                 errors. See §28.8.1 and §28.11. */

NewExpr         ::= "new" Type "(" ArgList? ")"
                  | "new" "(" ArgList? ")"              /* shorthand */
                              /* The object-initializer form `new T() with X := 5 end`
                                 is parsed as `NewExpr` followed by a `with`
                                 PrimarySuffix; there is no separate ObjectInit
                                 production. */
InitAssign      ::= Identifier ":=" Expression

LambdaExpr      ::= "async"? "(" LambdaParams? ")" "=>" LambdaBody
LambdaParams    ::= LambdaParam ("," LambdaParam)*
LambdaParam     ::= FuncParamMods? Identifier (":" Type)?
LambdaBody      ::= Expression
                  | "do" Statement* "end"

CaseExpr        ::= "case" Expression? CaseArm ("," CaseArm)* ","? "end"
CaseArm         ::= Pattern ("when" Expression)? "=>" ArmBody
ArmBody         ::= Expression
                  | Statement                  /* valid only in statement-position case
                                                  or in a catch arm; reads as a single
                                                  statement whose terminating `;` ends the
                                                  arm body, the same as `do Statement end` */
                  | "do" Statement* "end"     /* type (); valid only in statement-position case
                                                  or in a catch arm */
Pattern         ::= "_"                       /* discard */
                  | "null"                     /* null literal pattern */
                  | "var" Identifier (":" Type)?      /* binding */
                  | Constant ("," Constant)*          /* literal alternatives */
                  | RangeExpr                         /* closed, half-open, or
                                                         open-ended range —
                                                         see RangeExpr productions
                                                         in §A.7's expression
                                                         chain. The grammar's
                                                         RangeExpr admits an
                                                         optional `step`; in
                                                         pattern position the
                                                         `step` modifier is
                                                         rejected semantically
                                                         (§16.4: patterns test
                                                         membership, and stride
                                                         has no membership
                                                         meaning). */
                  | RelOpPattern Expression           /* relational; restricted
                                                         operator set, see below */
                  | Type                              /* type test */
                  | QualifiedName "(" PositionalPatternList ")" /* destructuring */
                              /* The grammar does not syntactically distinguish
                                 a single-Identifier `Constant` (an enum
                                 constant or named const reference) from a
                                 single-Identifier `Type` (a type test): both
                                 productions match `Dog`. Disambiguation is
                                 by name resolution: if `Dog` resolves to a
                                 type, the pattern is a type test; otherwise
                                 it is a constant pattern. The same rule
                                 applies to QualifiedNames — `Color.Red` is a
                                 constant pattern; `System.IO.Stream` is a
                                 type-test pattern. */
PositionalPatternList ::= PositionalPattern ("," PositionalPattern)*
PositionalPattern     ::= Pattern
                        | Identifier              /* naked binding,
                                                     valid only inside
                                                     positional destructuring */

RelOpPattern    ::= "<" | ">" | "<=" | ">="
                              /* Pattern-form relational operators are restricted
                                 to the four ordering operators. `is`, `is not`,
                                 and `as` are not pattern relops because they
                                 belong to TypeRel and take a Type rather than
                                 a value on the right. `in` is also excluded:
                                 a membership test against a collection or range
                                 is expressed with a Range pattern (`0..10`) or
                                 a Constant pattern with alternatives, not as
                                 a relational pattern. */
                              /* The Expression on the right of a RelOpPattern
                                 must have the same type as (or be implicitly
                                 convertible to) the scrutinee's type; the type
                                 check is semantic. The grammar admits more
                                 expression forms than the rule allows — for
                                 example, `< 60..100` parses but is rejected
                                 semantically because `<` between a value and
                                 a range is not defined. */

CollectionLiteral ::= "[" (Expression ("," Expression)*)? "]"
                    | "[" DictEntry ("," DictEntry)* "]"                   /* dictionary */
DictEntry         ::= Expression ":" Expression

NameOfExpr      ::= "nameof" "(" QualifiedName ")"
TypeOfExpr      ::= "typeof" "(" Type ")"
DefaultExpr     ::= "default" "(" Type ")"
