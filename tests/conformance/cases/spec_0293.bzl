Expression      ::= ConditionalExpr
ConditionalExpr ::= LogicalOr ("if" Expression "else" Expression)?
                  | CaseExpr
                  | WhenExpr
                  | ThrowExpr
ThrowExpr       ::= "throw" Expression?
                              /* Bare "throw" (no operand) is valid only inside a
                                 catch arm body; outside a catch arm body, the
                                 operand is required. Enforced semantically. */
LogicalOr       ::= LogicalAnd ("or" LogicalAnd)*
LogicalAnd      ::= NullCoalesce ("and" NullCoalesce)*
NullCoalesce    ::= Equality ("??" (ThrowExpr | NullCoalesce))?
                              /* The right operand may be a throw expression —
                                 `x ?? throw new ArgumentNullException(…)`
                                 (§17.12); ThrowExpr is bottom-typed there. */
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
UnaryExpr       ::= ("+" | "-" | "not" | "~") UnaryExpr
                  | "await" UnaryExpr
                  | PrimaryExpr
                              /* Right-recursive: `not not x`, `- -x` (with the
                                 space; `--` lexes as the decrement token), and
                                 `-(await x)` without parens all derive, matching
                                 §17.1 level 2's right-to-left associativity. */
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
                  | PrimitiveType
                              /* A primitive keyword in expression position:
                                 static access `int.Parse(s)` / `int.MaxValue`
                                 (§3.4) and the cast-call `int(d)` (§5.2). When a
                                 name in this position is followed by `?(`, the
                                 type reading wins: `T?(x)` where T resolves to a
                                 type is the §5.3 nullable cast, not a §7.6
                                 null-conditional call. */
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
                  | WhenExpr
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

CaseExpr        ::= "case" Expression CaseArm ("," CaseArm)* ","? "end"
                              /* The scrutinee Expression is mandatory (#744).
                                 The scrutinee-less condition-dispatch form is
                                 the separate WhenExpr below. The scrutinee is
                                 parsed at the MULTIPLICATIVE level (multiplicative
                                 and postfix expressions only) — normative rule in
                                 §19.1 — so a following `-` (negative range bound),
                                 relational, range, or open-ended-range token
                                 begins the first arm's Pattern rather than being
                                 absorbed into the scrutinee. An additive, shift,
                                 bitwise, range, or relational scrutinee must be
                                 parenthesized. */
CaseArm         ::= ArmPattern ("when" Expression)? "=>" ArmBody
ArmPattern      ::= Pattern ("," Pattern)*
                              /* Comma-separated pattern ALTERNATIVES exist at
                                 the arm level only (`1, 2 => "low"`); they are
                                 factored out of Pattern so that inside a
                                 positional destructure list the comma is
                                 unambiguously the SLOT separator (#1300/#1321).
                                 The arm separator and the alternative separator
                                 are disambiguated by `=>`, which always follows
                                 the complete ArmPattern (§19.3). */
WhenExpr        ::= "when" WhenArm ("," WhenArm)* ","? "end"
                              /* Scrutinee-less condition dispatch (§19.1). Arms
                                 are tried in textual order; the first whose
                                 condition is `true` is selected. Valid in both
                                 expression and statement position. The construct
                                 keyword `when` is distinct from the guard `when`
                                 of a CaseArm/CatchArm — position disambiguates. */
WhenArm         ::= (Expression | "_") "=>" ArmBody
                              /* The Expression is a `bool` condition. `_` is the
                                 always-true catch-all and, if present, must be the
                                 last arm. ArmBody follows the same rules as CaseArm. */
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
                  | Constant                          /* single constant; comma
                                                         alternatives live in
                                                         ArmPattern above */
                  | RangePattern                      /* closed, half-open, or
                                                         open-ended range */
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

RangePattern    ::= PatternBound (".." | "..<") PatternBound
                  | PatternBound ".."
                  | (".." | "..<") PatternBound
                  | ".."
PatternBound    ::= "-"? Shift
                              /* Dedicated pattern-position range form (mirrors
                                 the v0.94 RelOpPattern fix): no bare-expression
                                 leg (a lone Shift is not a range pattern) and no
                                 `step` (patterns test written bounds; stride has no
                                 membership meaning, §16.4). Bounds admit a
                                 leading `-` for negative literals. */
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
