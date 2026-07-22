Expression      ::= LogicalOr
                  | ThrowExpr
                              /* IfExpr, CaseExpr, and WhenExpr are PrimaryAtom
                                 forms (self-delimited, usable in any operand
                                 position); no conditional level exists in the
                                 operator table (§17.1, v0.208). A bare ThrowExpr
                                 appears at Expression top level and, because an
                                 atom's branches and arm bodies derive full
                                 Expressions, in any branch or arm body (§20). */
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
                              /* The binary-level productions above derive the
                                 operator-category mixes that §17.2's mandatory-
                                 parenthesization rules reject (bitwise↔equality/
                                 relational/logical, shift↔additive, range↔bitwise);
                                 such a mix parses and is then rejected post-parse,
                                 enforced semantically per §17.2. */
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
                  | "with" "(" InitAssign ("," InitAssign)* ","? ")"
                              /* The `with` suffix is the record copy-with-
                                 overrides expression (§9.5). It requires a
                                 record-typed source; a class-typed source is
                                 rejected semantically (§9.5, v0.188) — the
                                 grammar over-admits and the type checker
                                 rejects, the same pattern A.4 uses throughout. */

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
                  | IfExpr
                  | CaseExpr
                  | WhenExpr
                  | BindExpr
                  | MarkedTransfer
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
                                 admitting slot-level move (`OwningArray<T>`,
                                 `OwningDictionary<K, V>`, `OwningBuffer<T>`
                                 for class-typed T, `Span<T>` over class-typed
                                 `OwningBuffer<T>`, or an owning inline array
                                 `[N owned]…`). Both rejections are compile
                                 errors. See §28.8.1 and §28.11. */

NewExpr         ::= "new" Type "(" ArgList? ")"
                  | "new" "(" ArgList? ")"              /* shorthand */
                              /* `new T(...) with(X := 5)` is parsed as
                                 `NewExpr` followed by a `with` PrimarySuffix;
                                 there is no separate ObjectInit production.
                                 The suffix is the record `with` of §9.5:
                                 legal when the constructed type is a record,
                                 rejected semantically when it is a class
                                 (v0.188). */
InitAssign      ::= Identifier ":=" Expression

LambdaExpr      ::= "async"? "lambda" "(" LambdaParams? ")"
                    (":" Type)? "=>" LambdaBody
                              /* Keyword-led (§27.3, v0.217): `lambda` is
                                 reserved and begins no other expression or
                                 statement, so the reading commits at the
                                 first token (the second, after `async`) —
                                 no scan to the matching `)` is ever needed,
                                 and a parenthesized construct followed by
                                 `=>` (a when-guard arm, a grouped
                                 expression) is never a lambda prefix. The
                                 parameter parens are mandatory, including
                                 the empty pair. The optional `":" Type` is
                                 the return-type ascription (§27.3); no Type
                                 production contains `=>` (function types
                                 use `->`), so the ascription ends
                                 deterministically before the arrow. */
LambdaParams    ::= LambdaParam ("," LambdaParam)* ","?
LambdaParam     ::= FuncParamMods? Identifier (":" Type)?
                  | LambdaDestructure
LambdaDestructure ::= QualifiedName "(" LambdaSlot ("," LambdaSlot)* ","? ")"
LambdaSlot      ::= Identifier | "_" | LambdaDestructure
                              /* Positional-record destructuring in parameter
                                 position (§27.4, v0.183, #1408): irrefutable
                                 binding form only — the head names the
                                 parameter's exact positional-record type; no
                                 modifiers, no annotation, no refutable
                                 pattern elements. */
LambdaBody      ::= Expression
                  | "do" Statement* "end"

MarkedTransfer  ::= "owned" Identifier
                              /* The K3.4 transfer marker as an expression atom
                                 (v0.209). `owned` is reserved and begins no
                                 other expression, so the form is additive.
                                 Admission is positional, enforced semantically
                                 (§28.3): the entire RHS of a binding or
                                 assignment (the v0.180 forms; a BindExpr
                                 clause's RHS included, v0.216), an if-expression
                                 branch, or an expression-`case`/`when` arm
                                 result.
                                 Everywhere else — `??` operands, operator
                                 operands, guards, argument subexpressions —
                                 it is a compile error naming axiom A7's
                                 narrowed bound. The Identifier names a local
                                 binding or `owned` parameter; field paths use
                                 `move x.f` (§28.8.1). The call-site argument
                                 marker (ArgModifiers) and the `case owned x of`
                                 scrutinee marker are separate productions,
                                 unchanged. */
IfExpr          ::= "(" "if" Expression "then" Expression
                    ("elif" Expression "then" Expression)*
                    "else" Expression ")"
                              /* The conditional atom (§20, v0.208). The
                                 parentheses are part of the form: in atom-start
                                 position, "(" followed by the reserved "if"
                                 commits the reading at the second token; in
                                 call/index-argument position the atom supplies
                                 its own pair. Each condition and branch derives
                                 a full Expression (a bare throw branch is
                                 admitted anywhere); "else" is mandatory. */
CaseExpr        ::= "(" "case" "owned"? Expression "of" CaseArm ("," CaseArm)* ","? ")"
                              /* The expression-position case atom (v0.208): arms
                                 are Expression-bodied only (ArmBody's Statement
                                 and do-block alternatives belong to the A.6
                                 CaseStmt). "(case" commits at the second token —
                                 "case" begins no other expression. */
                              /* The optional `owned` is the scrutinee transfer
                                 marker (§35.5): admitted only when the scrutinee
                                 is a named owned binding of owning-choice
                                 classification; anywhere else it is a compile
                                 error. `owned` is a reserved keyword, so no
                                 expression can begin with it and the parse is
                                 unambiguous. */
                              /* The scrutinee Expression is mandatory (#744);
                                 the scrutinee-less condition-dispatch form is
                                 the separate WhenExpr below. `of` delimits the
                                 scrutinee from the first arm (v0.166, #1370), so
                                 the scrutinee is a FULL Expression — no
                                 precedence restriction, no parenthesization
                                 rule — and a first pattern may begin with `-`,
                                 `..`, `..<`, or a relational token without
                                 ambiguity. */
CaseArm         ::= AltPattern ("when" Expression)? "=>" ArmBody
AltPattern      ::= Pattern ("|" Pattern)*
                              /* Pattern ALTERNATIVES are `|`-separated (v0.167,
                                 #1371): the comma is exclusively the arm/slot
                                 separator, so no `=>` lookahead is needed and
                                 alternatives are available per positional slot
                                 (see PositionalPattern). `|`-joined members must
                                 be binding-free: constants, payload-free choice
                                 case names (§35.5, v0.200), ranges, or
                                 relationals (§19.3, v0.183). `|` never collides with
                                 bitwise-or: pattern-embedded expressions parse
                                 at Shift level or tighter (§19.3). */
WhenExpr        ::= "(" "when" WhenArm ("," WhenArm)* ","? ")"
                              /* Scrutinee-less condition dispatch (§19.1), the
                                 expression-position atom (v0.208): arms are
                                 Expression-bodied only; the statement form is
                                 A.6's WhenStmt. Arms are tried in textual order;
                                 the first whose condition is `true` is selected.
                                 The construct keyword `when` is distinct from
                                 the guard `when` of a CaseArm/CatchArm —
                                 position disambiguates. */
WhenArm         ::= (Expression | "_") "=>" ArmBody
                              /* The Expression is a `bool` condition. `_` is the
                                 always-true catch-all and, if present, must be the
                                 last arm. ArmBody follows the same rules as CaseArm. */
BindExpr        ::= "(" BindClause+ Expression ")"
BindClause      ::= "var" Identifier (":" Type)? ":=" Expression ";"
                              /* The binding atom (§17.15, v0.216): "(" followed
                                 by the reserved "var" — which begins no other
                                 expression — commits the reading at the second
                                 token, like the other paren atoms. Each clause
                                 introduces one single-assignment binding, in
                                 scope for the later clauses and the body; the
                                 ";" terminates the clause (no expression
                                 contains a top-level ";"). The body is one full
                                 Expression and supplies the atom's value and
                                 type. Clause initializers admit the marked
                                 transfer `owned x` (a K3.4 binding transfer,
                                 §28.3); the body position does not. */
ArmBody         ::= Expression
                  | Statement                  /* valid only in a CaseStmt/WhenStmt (A.6)
                                                  or in a catch arm; reads as a single
                                                  statement whose terminating `;` ends the
                                                  arm body, the same as `do Statement end` */
                  | "do" Statement* "end"     /* type (); valid only in a CaseStmt/WhenStmt
                                                  or in a catch arm */
Pattern         ::= "_"                       /* discard */
                  | "null"                     /* null literal pattern */
                  | "var" Identifier (":" Type)?      /* binding */
                  | Constant                          /* single constant; `|`
                                                         alternatives live in
                                                         AltPattern above */
                  | RangePattern                      /* closed, half-open, or
                                                         open-ended range */
                  | RelOpPattern Shift                 /* relational; restricted
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
PositionalPatternList ::= PositionalPattern ("," PositionalPattern)* ","?
PositionalPattern     ::= AltPattern              /* per-slot alternatives
                                                     (v0.167, #1371):
                                                     TwoColor(Red | Blue, var x) */
                        | Identifier              /* naked binding,
                                                     valid only inside
                                                     positional destructuring */

RangePattern    ::= PatternBound (".." | "..<") PatternBound
                  | PatternBound ".."
                  | (".." | "..<") PatternBound
                  | ".."
PatternBound    ::= "-"? Shift
                              /* Dedicated pattern-position range form (mirrors RelOpPattern): no bare-expression
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

CollectionLiteral ::= "[" (Expression ("," Expression)* ","?)? "]"
                    | "[" DictEntry ("," DictEntry)* ","? "]"                   /* dictionary */
DictEntry         ::= Expression ":" Expression

NameOfExpr      ::= "nameof" "(" QualifiedName ")"
TypeOfExpr      ::= "typeof" "(" Type ")"
DefaultExpr     ::= "default" "(" Type ")"
