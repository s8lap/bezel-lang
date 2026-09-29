# Bezel for Code Generation

*Validated against v0.313. Companion to the Bezel Language Specification — a writing reference, not a normative document.*

A dense reference for writing correct Bezel without reading the spec. It covers the syntax surface, the ownership rules, the standard-library name index, the idioms that work, and the shapes that look right and are rejected.

---

## 1. How to use this document

**What this is.** Everything needed to emit compiling Bezel: the whole statement and expression grammar, the ownership discipline the compiler enforces, and an index of every standard-library name you are allowed to spell.

**What this is not.** Normative. Where this document and `Bezel_Language_Spec.md` disagree, the spec wins and this document has a defect. Citations (`§17.2`, `stdlib §4.5`) are to spec v0.313 and `Bezel_Standard_Library.md` v0.313 so any claim here can be re-checked at its source.

**The one rule that matters most.** *If a type or member does not appear in section 5 of this document, look it up in `Bezel_Standard_Library.md` — and if it is not there, it does not exist.* **Never guess an API.** Inventing a plausible standard-library name is the single most common failure mode when writing Bezel: `MemoryStream`, `File.Open(path)`, `List<T>`, `string.Format`, `.ToList()`, `Task.Run`, `Option<T>`, `x.Dispose()` on a borrow — all plausible, all absent. Bezel's library is small and deliberately shaped; the shape is not .NET's.

When the API you need genuinely is not there, **say so** rather than approximating: "Bezel has no regular-expression module (`System.Text.Regex` is deferred); this needs a hand-written scanner" is a correct answer, and a hand-rolled `Regex.Match` call is not.

**Reading the examples.** `✗` marks a line the compiler rejects, `✓` the spelling to write instead; a comment containing `ERROR` marks the rejected line. Illustrative user types are declared inline. Section 9 says where to go for what this document omits.

---

## 2. Twenty facts that break C#/Rust/Kotlin/Swift/TS muscle memory

Read this page first. Every row is a habit that produces broken Bezel.

| # | The habit | Bezel | Cite |
|---|---|---|---|
| 1 | `{ }` blocks | `end;` — or `end Name;` / `end for;` / `end constructor;`. Braces appear **only** inside string interpolation; there is no fourth bracket kind | §1.6 |
| 2 | `x = 1;` assigns | `x := 1;` assigns. `=` is **equality** | A.6, §17.3 |
| 3 | `a == b`, `a != b` | `a = b`, `a <> b`. On `double`/`float`, `=` is reflexive (`NaN = NaN` is `true`), so the NaN self-test `x != x` is `Double.IsNaN(x)`, never `x <> x` | §17.3, §4.3.1 |
| 4 | `&&`, `\|\|`, `!x` | `and`, `or`, `not x`. Postfix `!` is the null-forgiving operator, nothing else | §17.1, §7.8 |
| 5 | `a % b` | `a mod b`; the compound form is one token, `mod=` | §17.1, A.6 |
| 6 | `else if`, `switch`, `foreach` | `elif` (`if … then … elif … then … else … end;` is the only condition ladder — there is no `when` construct); `case … of … end case;` (scrutinee); `for var x in xs do … end;`. **`foreach` does not exist in Bezel** | §19, §20, §21.4 |
| 7 | `cond ? a : b` | `(if cond then a else b)` — the parentheses are part of the form, and `else` is mandatory | §20 |
| 8 | `void` | omit the `: Type` clause; the result type is the unit type `()` | §22.1, §4.7 |
| 9 | `namespace` / `using X;` | `module X.Y;` (one per file, first line) / `import X.Y;` | §2.1, §2.2 |
| 10 | `x => x * 2` | `lambda(x) => x * 2` — the keyword is required; `lambda() => 0` needs the empty parens | §27.3 |
| 11 | `(int)d`, `d as int` | `int(d)` — casts are call syntax on the type name. `as` yields `T?` and is a type test only | §5.2, §17.4 |
| 12 | `var xs = [1,2,3];` | A collection literal has **no standalone type**: `var xs: Array<int> := [1, 2, 3];` or pass it where a parameter supplies the target | §15.4 |
| 13 | `if (x is Dog d)` | `is` **does not bind**. `var d := x as Dog; if d <> null then …` or a `case` arm `var d: Dog`. Over a **value**-typed `x` (primitive, record, enum, choice, inline array, `()`), a class, interface, `string` or `Object` target is rejected — box first: `(var o: Object := p; o is IShape)` — and so is a different value type (`i is long` over an `int`: `is` tests the type, not whether the value fits) | §12.7, §19.3 |
| 14 | `if (count)` truthiness | Conditions are `bool`. Write `count <> 0` | §4.1 |
| 15 | `i++` as an expression, `++i` | `++`/`--` are **postfix-only statements**: `i++;` on its own line. Assignments are statements too and produce no value | §17.6, §17.2 |
| 16 | `int? + int?` lifts | **Retired at v0.267.** Arithmetic, bitwise, shift, relational, compound and `++`/`--` over a nullable operand are a compile error — a §7.4-narrowed local counts as non-nullable at every one of them (`if n <> null then n += 1; end;` is fine). `=`/`<>`/`??`/`?.`/`?(args)`/`??=` still work | §7.11, §6.6 |
| 17 | `if (_field != null) Use(_field);` | Mutable fields and properties **never narrow**. Copy to a local first | §7.4 |
| 18 | `var y = x;` moves (Rust) or aliases-and-GCs (C#) | `var y := x;` is **always a borrow**; nothing is consumed silently. Transfer is written `move x` — at use sites only | §28.3, §22.3 |
| 19 | `new Foo()` returned from an unmarked method | A plain class-typed **instance** return means "a borrow of the receiver". A method that creates must declare `: owned T`; a **static** method returning a class type without `owned` is rejected at its declaration | §28.3 |
| 20 | `Dispose()`, `try/finally`, `using(...)` blocks | You never write disposal plumbing. `using var x := …;` disposes at scope end on every edge, including the exception edge; a written `x.Dispose();` means *release early* | §28.2, §28.10 |

Four more that cost the same amount of time. **(21)** Collections come in two families: `Array<T>`/`Dictionary<K,V>`/`Set<T>` hold **borrows** and dispose nothing, while `OwningArray<T>`/`OwningDictionary<K,V>`/… own their elements and cascade — with no implicit conversion between them (stdlib §4). **(22)** Contract clauses take **no** semicolon: `requires(…)`/`ensures(…)` sit between signature and body, self-delimiting; only `invariant(E);`, a class member, keeps its `;` (§22.16). **(23)** The discard is a statement, `_ := e;` — `var _ := e;`, `const _ := e;`, `out var _` and `ref _` are all compile errors (§17.13). **(24)** Interpolation is the one string convention: `$"n = {count}"`. There is no `String.Format`, and no implicit `ToString` in `+`.

---

## 3. Syntax surface

### 3.1 File shape

```bezel
module App.Billing;                       // first non-comment line; exactly one per file
import System.Threading;                  // only a non-auto module needs a line (§5.0)
import App.Ledger as Led;                 // optional file-scoped alias
type Handler<T> := (T, string?) -> bool;  // type alias: `:=`, never a visibility keyword
```

After the module line and imports come top-level declarations: `class`, `interface`, `record`, `enum`, `choice`, `extension`, `type`, `extern` (§2.1, §2.2, §27.8, A.2). Source files are UTF-8 with extension `.bzl`. Comments: `// line`, `/* block, does not nest */`, `/// doc` (§1.1, §1.5).

Auto-imported, no `import` needed (§2.3): `System`, `System.Text`, `System.Collections.Generic` (v0.289 — every collection type, the spans, `Counter`, `FrozenArray`, `KeyNotFoundException`, and the iteration interfaces), `System.Collections.Linq` (v0.278 — the query operators), the primitives, the `Task` family, `CancellationToken`, `Subscription`, and the six event types. `CancellationTokenSource` is NOT auto — it needs `import System.Threading;`. **Everything else needs an import** — section 5.0 has the table.

### 3.2 Keywords

All keywords are lowercase; a PascalCase name is never a keyword (§1.4). Unconditionally reserved (§B.1):

```text
abstract and as async await base bool break byte case catch char choice class closed
const constructor continue decimal default destructor do double elif else end ensures
enum extension extern false finally float for if import in int interface internal
invariant is lambda long method mod module move nameof new not null or out owned
override private property protected public readonly record ref requires return sbyte
sealed short static string then this throw true try type typeof uint ulong ushort
using var virtual when where while with yield
```

`event` is reserved-but-unused (§B.3) — the event member kind was retired (§24).

**Contextual** (§B.2) — a keyword only in the listed position, an ordinary identifier anywhere else: `convention`, `env`, `equality`, `get`, `set`, `hashable`, `notnull`, `sendable`, `of`, `old`, `result`, `operator`, `polymorphic`, `pure`, `scoped`, `serialized` (with `skip`/`optional`), `step`, `symbol`, `variadic`, `value`.

`_` is a reserved **token**, held out of the identifier namespace like punctuation: never declarable, never readable as a name. Identifiers *containing* `_` (`_camelCase`, `snake_case`) are ordinary (§1.3, §17.13).

**Identifier traps.** These cannot name a local, parameter, field, or type: `out`, `in`, `is`, `as`, `type`, `method`, `property`, `record`, `class`, `base`, `with`, `when`, `where`, `move`, `owned`, `default`, `new`, `not`, `and`, `or`, `mod`, `then`, `end`, `do`, `using`, `var`, `const`, `requires`, `ensures`, `invariant`.

### 3.3 Declaration forms

```bezel
// Visibility? Modifier* KIND Name GenericParams? (primaryParams)? Inheritance? GenericConstraints* body
public sealed class BankAccount(name: string, start: decimal): Account(name), IAudited
end BankAccount;                          // or `end;`
```

Modifiers on a type are `abstract | closed | sealed | static` (A.3). `closed` is the closed abstract class and combines with no other class-shape modifier (✗ `sealed abstract class C` ✓ `closed class C`, §11.7). Base class first in the inheritance list, then interfaces; **only the base** may carry `(args)`. Generic constraints come *after* the inheritance clause. Primary-constructor parameters are in scope throughout the body, including initializers (§22.10).

```bezel
public interface IBankAccount: IAudited
  method Deposit(amount: decimal);        // interface members carry NO visibility
  readonly property Id: string;           // getter-only;  `property Balance: decimal;` is read-write
  property [i: int]: string;              // indexer
  static method Parse(s: string): IBankAccount;             // §11.8, abstract-only
end IBankAccount;

record Point(X: int, Y: int);             // positional-only record: ends with `;`
record Money(Amount: decimal, Cur: string serialized "currency" equality skip);
record Rectangle                          // body record
  public readonly property Width: decimal;
  public constructor(w: decimal)  Width := w;  end constructor;
  public method Area(): decimal => Width * Width;
end Rectangle;
enum Priority
  Low, Medium, High;                      // the constant list ends with `;`
  public method IsUrgent(): bool => this = High;
  // compiler-provided on EVERY enum (v0.293): Priority.FromUnderlyingOrNull(u): Priority?, Priority.FromNameOrNull(s): Priority?
  // (declared identifier, exact), Priority.NameOf(v): string = nameof(Priority.X) for a written X. ToString() renders the qualified TYPE name, never the constant's — that is NameOf.
end Priority;
enum HttpStatus: short  Ok := 200, NotFound := 404;  end HttpStatus;   // underlying type

public choice FetchOutcome
  Fetched(owned response: Response),
  NotModified,
  Failed(status: int, message: string);   // the case list ends with `;`
  public method IsOk(): bool => (case this of Fetched(_) => true, _ => false);
end FetchOutcome;

extension string                          // module level; public | internal (default internal)
  public property IsBlank: bool => this.Length = 0;
end;                                      // `extern "libc" convention "cdecl" ... end;` declares FFI entries
```

**Member modifier order** — the compiler rejects any other order (§11.5). Bare `sealed` is a *class* modifier: on a member it appears only as `sealed override` (✗ `public sealed method M(): int => 1;`, §12.4):

```text
visibility  static  (async|pure)  (abstract|virtual|override|sealed override)  (owned|scoped)  readonly  member-kind  Name …
```

| Member | Exact form | Cite |
|---|---|---|
| Field | `private owned readonly _fs: FileStream := new FileStream(p, FileMode.Open);` — **no `field` keyword**; visibility mandatory | A.4, §13.3 |
| Method | `public static async method Load(u: string): Task<string> … end Load;` | §22.1 |
| Method, arrow | `public method Area(): decimal => Width * Height;` | §22.2 |
| Method, arrow, unit | `public method SetX(v: int) => _x := v;` (an assignment is admitted **only** with no declared return type; an ordinary expression body is admitted there too — `=> Console.WriteLine(s);`) | §22.2 |
| Abstract method | `public abstract method Speak(): string;` — body-less, ends `;` | §12.5 |
| Auto-property | `public property Name: string;` / `public property Id: int private set;` / `public readonly property Pi: double := 3.14159;` | §23.1 |
| Expression property | `public property Area: decimal => Width * Height;` — **never write `readonly` here** (§23.2 rejects it on every body-bearing form) | §23.2 |
| Block property | `public property Balance: decimal` / `get => _b;` / `set => do … end;` / `end Balance;` | §23.3 |
| Owned-taking setter | `set(owned);` or `set(owned x);` — the type is the property's and is never repeated | §23.3 |
| Indexer | `public property [i: int]: string => _items[i];` — the block form closes with a bare `end;` | §23.4 |
| Constructor | `public constructor(a: int)` + optional `base(a);`/`this(a);` chain + body + `end constructor;`. A chain argument may read only the constructor's own parameters, constants and statics — never a field of `this` or a primary-constructor parameter. A `this(...)` chain must end at a constructor with no `this(...)`; a cycle is rejected. A **class** that declares no instance constructor has an implicit `public` parameterless one, so `new C()` compiles; declaring any instance constructor (a primary parameter list included) removes it, and a **body record** has no such implicit constructor | §22.7, §12.8 |
| Destructor | `destructor` … `end destructor;` — no visibility, no name, no parameters | §28.4 |
| Operator | `public static pure method operator +(a: V, b: V): V => …;` closes `end operator +;` | §25.1 |
| Equality operator | Overloading `=`/`<>` with **both operands the declaring type** obliges `Equals(T)` **and** `GetHashCode` beside them (`override` or plain, on a record, enum, or choice alike — v0.288), and by itself strips the record of auto-`=`/`Equals`/`GetHashCode`/`ToString` — **write all four or none**. A **heterogeneous** `=` (`T = U`) obliges nothing and strips nothing. On a move-only choice, `=`/`<>` operands and the `Equals(T)` parameter bind BORROWS — both operands stay live (v0.286). | §25.2, §9.2 |
| Invariant | `invariant(_bal + _limit >= 0m);` — class bodies only, no visibility, keeps its `;` | §22.16 |
| Nested type | `public record Token(…);` inside a class body — visibility **required** | §11.6 |

Also: `public const Max: int := 100;`; `static constructor()` … `end constructor;` (no visibility, no `pure`, no clauses); `polymorphic("dog": Dog, "cat": Cat);` (at most one per class).

Contract clauses sit between the signature and the body, are self-delimiting, and take **no terminator**:

```bezel
public method Withdraw(amount: decimal)
  requires(amount > 0m)
  requires(amount <= _balance)
  ensures(_balance = old(_balance) - amount)

  _balance := _balance - amount;
end Withdraw;
```

One condition per clause. `result` names the return value inside `ensures`; `old(e)` names a pre-state value. `requires` runs in every build mode; `ensures`/`invariant` run under checked-contracts mode. A violation raises the catchable `ContractViolationException`. `static requires`/`static ensures` demand the proven tier. Clauses may call **only `pure` members** — see section 5.12 (§22.16).

**Parameters** (A.4): `ParamModifiers? name: Type (:= default)? (when true|when false)?`, with `ParamModifiers ∈ ref | out | out owned | owned | scoped`. `owned` precedes the **identifier**: ✗ `x: owned T` ✓ `owned x: T`. A parameter may not be named `_` (§22.5).

**Generics:** `<Variance? Ident, …>` with `Variance ∈ in | out` (interfaces only); `where T: Constraint, …` with `Constraint ∈ class | value | notnull | sendable | hashable | new() | Type` (§14.2). Type arguments are **never inferred** for a class, record, interface, or choice — only call-site inference for generic *methods* exists (§14.5). Generic choice construction writes its arguments: `Result<int, string>.Ok(v)` (§35.3).

**Visibility:** `public | internal | protected | private`. Top-level types default to `internal`; class members **must** state one, with exactly four exceptions (static constructor, destructor, `polymorphic`, `invariant`). No combined forms: ✗ `protected internal` (§13.2, §13.3, §13.5).

### 3.4 Statements, with their exact terminators

The complete statement list (A.6): `LocalDecl`, `LocalConstDecl`, `AssignmentStmt`, `ExpressionStmt`, `DiscardStmt`, `IfStmt`, `WhileStmt`, `ForStmt`, `DoEndBlock`, `CaseStmt`, `ReturnStmt`, `ThrowStmt`, `BreakStmt`, `ContinueStmt`, `TryStmt`, `YieldStmt`, `IncrementStmt`, `LocalTypeDecl`. Every statement ends `;`. Every block ends `end … ;`.

`end` may be followed by the construct's **name** if it has one, or by its **introducing keyword** otherwise; a present label must match. **A bare `end;` is always valid everywhere** (§1.6).

| Construct | Closer | Near-miss that fails |
|---|---|---|
| class / interface / record / enum / choice | `end Name;` or `end;` | — |
| method | `end MethodName;` or `end;` | — |
| constructor / destructor | `end constructor;` / `end destructor;` | ✗ `end ctor;` |
| operator | `end operator +;` (the token must match) | — |
| block property / accessor block | `end PropName;` or `end;` | — |
| **indexer** block | **`end;` only** — an indexer has no name | ✗ `end [i];` |
| `extension` / `extern` | `end extension;` / `end extern;` / `end;` | — |
| `if` | `end if;` or `end;` | ✗ `endif;` |
| `while` (unlabeled) | `end while;` or `end;` | — |
| `for`, `await for` (unlabeled) | `end for;` or `end;` | ✗ **`end foreach;`** |
| labeled loop | `end label;` or `end;` — the keyword closer is **rejected** once labeled | ✗ `scan: for … end for;` |
| `do … end` block | `end do;` or `end;` | — |
| `try` | `end try;` or `end;` | — |
| `case` statement | `end case;` or `end;` | ✗ `end switch;` |
| lambda block body | `do … end` — bare `end`, no label and no `;` of its own | ✗ `end lambda;` |
| `case` arm `do` block | `do … end` — bare `end`, no `;` | — |

Every statement shape:

```bezel
// declarations
var balance: decimal;                       // typed, no initializer — `0m` (§7.9); the compiler does not yet admit the read — assign before use until it does
var greeting := "Hello";                    // inferred
const Pi: double := 3.14159;                // single-assignment binding (§21.1)
using var _ := mutex.Acquire();             // anonymous owning binding; `await using var` in async bodies
record Row(Name: string, Age: int);         // LocalTypeDecl: `record` and `enum` ONLY, no visibility

// assignment / discard / increment / expression
total := total + 1;
total += 1;   total mod= 3;   cache ??= new Cache();   // := += -= *= /= mod= &= |= ^= <<= >>= ??=
_ := ComputeChecksum();                     // discard statement
i++;   i--;                                 // postfix, statement position only
Console.WriteLine("hi");

// if
if amount > 0 then
  Balance := Balance + amount;
elif amount = 0 then
  Log("zero");
else
  Reject();
end if;

// while, for (every shape)
scan: while i < 10 do  i := i + 1;  end scan;
for var i in 1..10 do ... end;                for var i in 0..<n step 2 do ... end;
for var x: int in source do ... end;          // optional ascription; ✗ `for var _: T in ...`
outer: for var row in rows do ... end outer;
await for var line in reader do ... end;      // `with token` supplies a CancellationToken (§30.7)
do  using var t := Open();  end;            // a `do` block ends a scope early

// case statement
case delta of
  -10..-6 => "strongly down",
  ..<0    => Log("down"),                   // single-statement arm
  0       => do Reset(); Log("flat"); end,  // do-block arm
  _       => Log("up")
end case;

// return / throw / break / continue / yield
return;
return expression;
throw new ArgumentException("nope");
throw;                                      // bare re-raise, inside a catch arm body only
break;   break scan;
continue;   continue scan;
yield return i * i;
yield break;

// try — needs at least one of catch/finally; arms are COMMA-separated
try
  DoWork();
catch
  var ex: ArgumentException when ex.ParamName = "amount" => Log("bad amount"),
  InvalidOperationException => Retry(),
  var ex: IOException | TimeoutException => Retry(ex),
finally
  Log("done");
end try;
```

`return` inside a `finally` is a compile error, as are `break`/`continue` whose target loop encloses the `finally` (§21.5, §21.7). **Trailing commas**: every comma-separated list admits one optional trailing comma before its closer — one rule, no exceptions (§1.6). `|`-separated pattern alternatives are not comma lists.

### 3.5 Expressions

Precedence, high to low (§17.1):

| Lvl | Operators | Assoc |
|---|---|---|
| 1 | `.` `?.` `[ ]` `( )` `?( )` `new` postfix `!` | L→R |
| 2 | prefix `+` `-` `not` `~`, `await` | R→L |
| 3 | `*` `/` `mod` | L→R |
| 4 | `+` `-` | L→R |
| 5 | `<<` `>>` | L→R |
| 6 | `..` `..<` (with `step`) | non-assoc |
| 7–9 | `&`, then `^`, then `\|` | L→R |
| 10 | `<` `>` `<=` `>=` `in` `is` `is not` `as` | L→R |
| 11 | `=` `<>` | L→R |
| 12 | `??` | R→L |
| 13 | `and` | L→R |
| 14 | `or` | L→R |
| 15 | `throw expr` | R→L |
| 16 | `:=` and its compound forms — **statements**, not expressions | R→L |

**Mandatory parenthesization** (§17.2). These category mixes are a *syntax error*, not a precedence surprise: bitwise × (equality or relational); bitwise × logical; shift × additive; range × bitwise.

```bezel
✗ if flags & MASK = 0 then ...          ✓ if (flags & MASK) = 0 then ...
✗ var n := a << 2 + 1;                ✓ var n := (a << 2) + 1;
✗ var r := mask & 0..n;               ✓ var r := (mask & 0)..n;
```

`(if …)`, `(case …)` and the binding atom `(var x := e; body)` take no precedence entry — they are self-delimited primary atoms usable in any operand position (§17.2, §17.15).

Literals (§18, A.8):

```text
42   0xFF   0b1010_1010   1_234_567   42L   42U   42UL
3.14   3.14f   1.23e-4   1.23456789m   1.23e2m
'a'  '\n'  '\u0041'  '\U0001F600'  '\x41'
"Hello\n"    @"C:\raw\path"                       // verbatim; "" is one embedded quote
$"Hello, {name}!"     $@"C:\Users\{user}\x.pdf"
$"{amount,12:F2}"     $"|{name,20}|"   $"{ratio:P1}"   // {expr,align:format}
$"{{literal braces}}"
true   false   null   ()                          // bool / null / the unit value
[1, 2, 3]     []      ["a": 1, "b": 2]            // collection and dictionary literals
```

Escapes: `\n \r \t \\ \" \' \0 \a \b \f \v \uXXXX \UXXXXXXXX \xH..H` (§18.8). An interpolation hole renders `ToString()`; `{e:F2}` renders `ToString(format)` and requires an intrinsically formattable type or `IFormattable`; a **null** hole renders the four characters `null` (§18.5).

Atoms and suffixes (A.7):

```text
new Connection("a")            new()                 // target-typed shorthand
new Point(1, 2) with(X := 5)                         // `with` = record copy-with-overrides
obj.Method<int>(args)          Registry<int>.Register()
user?.Name?.Length             handler?(42)          // null-conditional access / call
x!                                                   // postfix null-forgiving
move x     move obj._field     move arr[i]           // the use-site transfer marker (§28.8.1)
nameof(amount)   typeof(BankAccount)   default(int)
int.Parse(s)   int.MaxValue   short(x)   long?(n)    // primitive keyword in expression position; casts
this   base.Speak()
```

`new()` — the target-typed shorthand — is admitted at a typed field or local initializer (the two forms the compiler accepts today), and by §17.7 (v0.296) also at a default parameter value and as a `return new();` statement under a declared return type — those two are spec-defined and not yet accepted by the compiler. It is **not** admitted at an expression body (`=> new()`), an ascribed lambda's return, an assignment target (`xs := new();`) or an argument.

The paren atoms:

```bezel
var max   := (if a > b then a else b);                    // `else` MANDATORY
var grade := (if s >= 90 then "A" elif s >= 80 then "B" else "F");
var g2    := (case score of 90..100 => "A", 70..<80 => "C", _ => "F");
var area  := (var w := bounds.Width(); w * w);            // binding atom (§17.15)
```

Arms in expression position are **expression-bodied only**; statement and `do … end` arms belong to the statement forms (§19.2).

Lambdas and function types (§27.1, §27.3):

```text
lambda(x) => x * 2                          // parameter types inferred from the target type
lambda() => 0                               // empty parens MANDATORY
lambda(x: int): long => x * 2               // return-type ascription
lambda(item: Item) => do var n := N(item); return n; end
async lambda(u: string) => await Fetch(u)
lambda(_, x) => x                           // `_` ignore; duplicates legal
lambda(Entry(k, v)) => $"{k}: {v}"          // positional-record destructuring parameter (§27.4)
lambda(owned h) => _handlers.Add(h)         // modifiers when the target type declares them

(int, int) -> int      () -> ()      (owned T) -> ()      (ref int) -> ()
(out owned T) -> ()    ((int) -> bool)?                   // parens required before `?`
```

Patterns (§19.3):

```text
_          null        var x       var x: T      Dog       Color.Red     -7
90..100    0..<60      ..<0        31..                                  // range patterns
> 70   < 50   <= 100   >= 0                                              // relational (these four only)
Point(x, y)   Point(_, y)   Point(var x, var y)                          // positional destructuring
Rect(Width = var w, Height = 3)                                          // named destructuring: `=`, not `:=`
Fetched(move response = var r)                                           // marked slot (owning match)
1 | 2 | 3      Red | Blue                                                // alternatives: binding-free
var t when t > 30 => ...                                                   // guard
```

Positional or named, never mixed. A naked identifier is a **binding** only inside positional destructuring; at top level it is a type test. Classes have no positional-destructuring surface, even with a primary constructor — but the **named** form does reach a class: `Circle(Radius = var r)` is admitted, its slots naming the class's *public readable properties* (§19.3), and over a sealed hierarchy an all-slots-irrefutable arm of that shape counts toward exhaustiveness exactly as `var c: Circle` does (§19.6). Interface-typed pattern heads are not admitted in v1.

Ranges and the `throw` expression:

```bezel
1..10     0..<10     0..100 step 2     100..0 step -1
arr[2..5]   arr[..5]   arr[10..]   arr[..]
var cfg := LoadConfig() ?? throw new InvalidOperationException("no config");
```

`step` may not be applied to an **open-ended** range (one missing an endpoint: `10..`, `..5`) — an exclusive-upper range is fine, `0..<50 step 5` is §16.4's own example. A step of zero is rejected at construction; step sign must agree with the range's direction. `step` never appears in a pattern (§16.4).

### 3.6 Naming

PascalCase for types, methods, properties and other public members; `_camelCase` for private fields; `camelCase` for parameters and locals — **the compiler does not enforce these** (§1.3). Identifiers are case-sensitive. Modules are dotted PascalCase (`BankingApp.Transactions`); the dotted form is a single name and establishes no nesting. `string`/`String`, `int`/`Int32` and the rest name the same types and are interchangeable in source.

---

## 4. Ownership

### 4.1 The core model

Bezel has **no garbage collector** and no runtime lifetime mechanism for user classes — no refcounts, no regions, no heap scanning. Every class instance is freed by compiler-inserted code at a statically known point (§28).

Every class- or interface-typed expression and binding carries exactly one **ownership classification** at every program point (§28.3):

- **owned** — you must dispose it exactly once, on every path.
- **borrowed** — you must not dispose it; it is valid only while its **source** is alive.
- **moved-from** — the obligation has left; the binding is unreadable until reassigned.
- **maybe-consumed** — a join of owned with moved-from or disposed; unreadable.

Records, enums, primitives and inline arrays are **value types**: no disposal, no classification. `string`, boxed values, function values and `Type` (the `typeof` / `GetType` token) are **runtime-managed**: no disposal, no classification, and `owned` on them is a compile error (§28.1, §28.9).

**You never write `Dispose()` as plumbing.** An owned value reaching its scope's end undischarged is disposed *there*, automatically, on the normal edge **and** the exception edge — no `try`/`finally` for a value, ever, and a written `x.Dispose();` means *release early*. The only difference between the edges is a lint: a plain-`var` implicit drop on normal exit raises the **must-use warning**; the exception edge is silent (§28.3). **Never declare a member named `Dispose`** on a class or a record — at any parameter list, it is a compile error at the declaration (§28.2); disposal has exactly two customization points: a `destructor` (only for native or foreign resources) and `owned` **fields**, auto-disposed with their container in reverse declaration order (§28.4, §28.5). The generated `Dispose()` is public, idempotent and non-overridable — no `disposing` parameter, no `_disposed` flag, no `base.Dispose()` call.

**Trivial-dispose classes** are exempt from ceremony, not from tracking (§28.9): a class with no destructor, no `owned` field of non-trivial type, no owning-inline-array field, on a trivial-dispose base. `Array<T>`, `Set<T>`, `Dictionary<K,V>`, `StringBuilder`, `Random` qualify — no must-use warning anywhere the warning would otherwise attach: a plain `var`, an undischarged `owned` parameter (a method's, an iterator's, an async method's), an owning iteration variable. An **interface**-typed binding is never exempt — unless its initializer is a producer of a trivial-dispose **class** (`var t: IThing := new TrivialThing();`), the exemption being keyed on the value, not the annotation. `OwningArray<T>` and anything holding a resource do not. **Every borrow rule still applies to trivial-dispose references.**

**The default build is checked disposal mode** (§28.2): any member access through a reference to a logically-disposed instance raises `DisposedAccessException` (§31.4). The release relaxation — the same access undefined — is selected explicitly per build (`--unchecked-disposal`); never write a program that depends on it. Every "faults in checked mode" below is therefore the default behaviour.

The one question to ask of every class-typed value: *am I responsible for disposing this?* Owned = yes. Borrowed = no.

### 4.2 The six markers

| Marker | Position | Means |
|---|---|---|
| `owned` | **declaration side only** | this *slot* holds the obligation |
| `move` | **use site** (RHS, argument, arm, scrutinee) | perform the transfer now, here |
| `using var` | local declaration | own it, dispose at this block's end, never move it out |
| `ref` | parameter declaration **and** call site | pass by reference |
| `out` | parameter declaration **and** call site | output parameter, assigned on every normal-completion path |
| `scoped` | parameter declaration, and call site for class-typed arguments | storage-bounded retention |

**`owned` — admissible positions** (§28.1): method parameter (`method M(owned p: T)`), return type (`method Acquire(): owned Connection`), class field (`private owned _c: Connection;`), `out` parameter (`out owned p: T`), function-type parameter and return slots (`(owned T) -> ()`, `() -> owned Connection` — *distinct types* from the unmarked forms), lambda parameters, choice payload fields (`Fetched(owned response: Response)`), inline-array slot marker (`[N owned]T?`).

The declared type, nullability stripped, must be a class type, an interface type, or a bare type parameter. On a primitive, `string`, a function type, a record, an enum or the unit type it is a **compile error**.

**`owned` is never admissible** on a record field (§9.7), on a local (use `using var`), on a generic type argument (`Array<owned T>` does not exist — use `OwningArray<T>`), on an inline-array element type, or doubled with an owning-inline-array slot marker. It is **redundant but admitted** on a mint-announcing return type (`GetEnumerator(): owned IEnumerator<T>`) and on a return of a move-only value type (an owning choice, §35.3).

**`move` — the use-site transfer marker** (§22.3, §28.8.1). Four operand shapes: `move x` (a named local or `owned` parameter), `move obj.f` (an `owned` field), `move coll[i]` (an owning slot), and `move obj.f!` over a **nullable** owned field — §28.8.1's detach pattern, `move _slot!` then `_slot := null`. A trailing `!` never changes the spelling (§17.8): over a nullable owned local, `sink._f := x!;` and `throw x!;` consume like `sink._f := x;`, `M(move x!)` is the marked argument, and `sink._f := move x!;` is the redundant-marker error.

*Required* — omitting it is a compile error ("this call consumes `x`; write `M(move x)`") — when a **named owned binding** feeds an `owned` argument (`M(move x)`), a local binding (`var y := move x;`, `using var y := move x;`), an assignment (`y := move x;`), an if-expression branch or expression-arm result (`(if c then move a else move b)`), or an owning-match scrutinee (`case move outcome of …`). A call that **raises** does not give the argument back: the transfer completes at call entry, the binding is still moved-from in the `catch` arm, and the callee's own exception-path discharge (or a constructor's unwinding) disposes the value (§28.8.1, §22.9).

*An error* — the marker is **rejected as redundant** — at a **marker-free position**, where the target already says "consume". Memorize this list; "mark every transfer" is the wrong generalization and produces exactly these errors (§28.3, §22.3):

- **`return` operand** under an `owned` or mint-announcing return: `return x;` — ✗ `return move x;`
- **`throw` operand**: `throw x;` — ✗ `throw move x;`
- **discard RHS**: `_ := x;` — ✗ `_ := move x;`
- **owned-slot store**: `sink._f := x;` into an `owned` field, an owning collection slot, or a `set(owned)` property — ✗ `sink._f := move x;`
- **Pattern B `await`** on an owned `OwningTask<T>` binding: `using var r := await t;` — ✗ `await move t`
- **owning iterator `yield return`**: `yield return x;` — ✗ `yield return move x;`

`move` is *also* an error on a **fresh producer** (`M(move new T())` — nothing is being invalidated), on a **borrow**, in a **`??` operand** (a named owned binding written bare as a `??` arm is a *borrow* arm), inside a **closure** over a captured owned binding (captured owned bindings are uniformly in borrow state — move, reassign, dispose and discard of them are all errors), and on a **move-only value** argument or binding (§15.3's owning inline arrays and owning choices — the unmarked read already moves, at `(x: V)` and `(owned x: V)` alike; the owning-match scrutinee `case move c of …` is the one marked move-only position), and on a **`static` field**.

**The no-silent-consume rule.** An unmarked read of a class-typed binding *never* consumes. `var b := a;` is always a borrow of `a`. This is the single most important spelling fact in the language (§28.3).

**Field and slot moves carry a restoration obligation** (§28.8.1). Every path from a `move obj.f` to *any* method exit — normal return, fall-through, `throw`, `yield break` — must execute an assignment `obj.f := value;`. The spec's worked rejection is `return move _transactions;` from a method whose return type is **not** `owned` — the moved field escapes with no owner. A **terminal** move is rejected at *every* return type, `owned` included: §28.8.1 admits "no terminal-move dispensation", the return edge being itself an unrestored exit (ruled at v0.223, #1814, the permissive option declined as unsound). Always write §28.8.1's take-restore-return idiom — `var fresh := new …; var taken := move _f; _f := fresh; return taken;`. Only a `destructor` body is exempt from restoration. Restoration must not go through something that can throw, hence **build the replacement first, then take**. A nullable owned field may be restored with `null`.

**`using var`** (§28.10): owned, disposed at the block's end on **every** exit edge, non-reassignable, and **never movable out of**. `using var fresh := new Buffer(); … return fresh;` is a compile error; a factory uses plain `var`.

- `using var y := move x;` — **adoption**: the standard way to turn an owned local, parameter or loop-variable obligation into scope-bound disposal.
- `using var y := x;` (unmarked, `x` a **class- or interface-typed** named owned binding) — **does not adopt**. It is a **borrow-pin**: no obligation is assumed, `x` keeps it, and the pin extends `x`'s required liveness to this block's end — so moving, transferring or reassigning `x` under the pin is rejected, while an explicit `x.Dispose()` is **not** (a site-announced kill; a later read through `y` faults in checked mode).
- `using var y := x;` (unmarked, `x` a **move-only value** binding — §15.3's owning inline arrays and owning choices) — **adopts**, not pins: the unmarked read *moves*, leaving `x` moved-from (a copy is impossible, so there is nothing for a marker to disambiguate). Scope-end auto-dispose runs under plain `var` too; `using var` adds the reassignment lock, and over an owning **choice** also suppresses the must-use warning on that auto-drop (§15.3, §35.4).
- `using var _ := expr;` — the anonymous owning binding (`using var _ := mutex.Acquire();`). Not for a `Task`-family value (§17.13).
- Admissible over value-typed, runtime-managed and move-only right-hand sides, where disposal is a no-op — safe to use uniformly in generic code.
- To dispose earlier than the method's end, nest a `do … end` block.

**Disposal-form menu** (§28.3):

| Form | Disposes at | Movable out? | Diagnostic |
|---|---|---|---|
| `using var x := …` | scope end, every path | **no** | none |
| plain `var x := …`, undischarged | scope end (auto-drop) | yes | **must-use warning** (none at a trivial-dispose type) |
| `x.Dispose();` | at the call | yes, until disposed | none |
| `_ := x;` | end of the discard statement | — | none |
| unbound temporary | end of the full expression | — | none |

Disposing an **owned field** from code of its declaring type (`_resource.Dispose();`) is an early release point and demands nothing afterwards (§28.6): the field keeps its reference, the container's teardown still cascades over it as a no-op, and no restoration is owed — unlike `move obj.f`, which owes one on every exit path. A later read of that field is not diagnosed; it faults in checked disposal mode.

`_ := x;` leaves the binding **explicitly-disposed**: a later straight-line **use** is a mandatory use-after-dispose warning, and a use after a *conditional* discard is a compile error at the merge. *Use* is read, borrow **and consume** alike — `move x` and an `owned` argument position draw the same verdict a plain read does, the severity being decided by which kill reached the binding and never by the shape of the use (§28.2, v0.303). Discarding **the task itself** — `_ := task`, `using var _ := task` — is **rejected** for the `Task` family; use `Task.FireAndForget(t)`, or a *named* `using var t := …` (§17.13, v0.288). Discarding an **awaited result** is a different statement and is legal: `_ := await t;` is a Pattern B owned-consuming position that disposes the result at the statement's end (§30.5).

**`ref` / `out`** (§22.3, §22.4). Written at the declaration **and** at the call site. `out` is copy-out-on-normal-return: the callee must assign on every normal-completion path; on the exception edge the recipient is untouched.

- `out owned p: T` is the only modifier compound. At the call site `out var x` **adopts** what the signature announces — against `out owned` it is an owned local you discharge yourself (`move` it, store it in an owned slot, or let scope-end auto-drop with the must-use warning) — and `out using var x` is the scope-bound owned form, accepted **only** against `out owned`. There is no `out owned var` spelling (retired v0.289).
- `ref`/`out` arguments may **not** name a location of owned state (an owned local, an `owned` parameter, an `owned` field, an owning slot), nor a `using var`, nor a `readonly` field outside its assignable context. **For an *instance* `readonly` field or auto-property that context is per-instance** (§22.8, v0.270): it is assignable only through an lvalue chain rooted at `this` — so a constructor may not write *another* value's `readonly` storage, and binding one to a `ref`/`out` parameter inside a constructor is the same store by other syntax — and an instance `readonly` **field** is assigned **at most once** on any path through one instance construction, counting the field initializer and every constructor body in the `this(...)` chain together (v0.271; stated for fields — auto-properties are not decided). A `static readonly` field is different on both counts: its context is its own static initializer or a constructor body of the declaring type, spelled as the bare name or `TypeName.F`, and its unit is one type initialization. Pass a borrow instead, or use `owned` parameters/returns and `move`.
- `ref`/`out` are **forbidden** on `async` methods, on **`async` lambdas**, on iterator methods and on indexers (§30.1, §29.3, §23.5). An **ordinary** lambda admits them, as its function type does — `var f: (ref int) -> () := lambda(ref x: int) => do x := x + 1; end;` (A.7 `LambdaParam`/`FuncParamMods`, §27.1). The argument's static type must match the referent type exactly. `out var _` is an error; write `out _`.

**`scoped`** (§28.14) declares **storage-bounded retention** — the callee may store the thing, bounded by the storing object's life.

- On a **function-typed** parameter (`method Subscribe(scoped h: () -> ())`): the callee may store the callback in a `scoped` field of the bounding object, forward it to a same-bound `scoped` parameter, or invoke it. Function-typed arguments carry **no call-site marker**. Without `scoped` such a parameter is **call-bounded** — invocable during the call, never retained — and callers may pass any closure.
- On a **class- or interface-typed** parameter: the callee may store the borrow in a direct non-owned field of the bounding object. The call site writes `M(scoped x)` — an argument *modifier*, not an expression; a fresh producer is rejected there. A **primary-constructor** parameter may be `scoped` at these types — `class Box(scoped src: Source)`, used from a body as `src` — and its compiler-generated capture field is that direct non-owned field; the holder must still be a `using var` local (or `this`) with the source declared before it. A function-typed `scoped` primary parameter is rejected (§22.10).
- **The price: captured local sources are frozen** while the bounding object lives — no move, no reassign, and **no `Dispose()`**. `scoped` on a **static** method is admitted only when that method's return is `owned`.

### 4.3 The announcement table, as a decision procedure

**Everything is read off the *declared* return type**, before generic substitution, nullability stripped. Prose like "returns a fresh instance" carries no weight; only the signature does (§28.3).

**Caller side — what did I just get?** Classify the declared result type R in this order; first match wins.

| # | R is… | Instance call | Static call | Extension call | Function-value call |
|---|---|---|---|---|---|
| 1 | `owned T` (concrete class/interface, or bare `T`) | **owned** | **owned** | **owned** | **owned** |
| 2 | **mint-announcing**, unmarked | **owned frame** | **owned frame** | **owned frame** | **owned frame** |
| 3 | mint-announcing, explicitly `owned` | identical to row 2 | | | |
| 4 | plain concrete class/interface, unmarked | **borrow**, source = receiver | **rejected at the declaration** | **borrow**, source = receiver parameter | **borrow**, sources = the function value ∪ every plain reference-typed argument |
| 5 | bare type parameter `T`, non-empty source set | **borrow**, sources = receiver ∪ every plain parameter whose type mentions `T` | same | same | same |
| 6 | bare `T`, **empty** source set | cannot arise | **rejected at the declaration** — spell `owned T` | cannot arise | cannot arise |
| 7 | value type / `string` / runtime-managed | not applicable — no obligation | | | |
| 8 | move-only value type (owning inline array; owning choice) | **owned** move-only value | | | |

**The mint-announcing set**: the `Task` family (`Task`, `Task<T>`, `OwningTask<T>`) and the iterable family — `IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, `IOwningEnumerable<T>`, `IOwningEnumerator<T>`, `IOwningAsyncEnumerable<T>`, `IOwningAsyncEnumerator<T>` — plus any **lazy query**. A *call* returning one of these mints a fresh caller-owned frame at every callee kind. `IOrderedEnumerable<T>` and `IGrouping<TKey,T>` are not in the head set, but `OrderBy`/`OrderByDescending`/`ThenBy`/`ThenByDescending` mint by their lazy-query kind.

**Reads of storage are not calls.** A property, field, indexer or local read of *any* type — mint-announcing included — is a **borrow of its storage**. That is how a stored task or collection view is re-exposed.

**Callee side — what am I allowed to return?**

| Declared R | Every return path must supply |
|---|---|
| `owned T` | a fresh or owned value. A borrow is rejected. |
| mint-announcing (marked or not) | a **fresh or owned frame**: a forwarded mint-announcing call result, a completed-task factory result, an owned frame local, an `owned` parameter, a `move`d owned field, or `null` under a nullable R. An `async` or iterator body satisfies this automatically. Returning a **borrow** (a bare field or property read, a plain parameter, a captured binding) is a compile error. One exception: a class implementing both `IEnumerable<T>` and `IEnumerator<T>` may write `return this;` in its own `GetEnumerator()`. |
| plain class/interface, **instance or extension** | only a **receiver-sourced** borrow: `this`, a bare read of a field, a borrow reached through the receiver's fields and indexers. A fresh owned value is **rejected** — declare `owned`. |
| plain class/interface, **static** | nothing — the signature itself is rejected. Static storage may be lent out only through a **static property getter**, never a static method. |
| bare `T`, non-empty source set | `null`, or a borrow sourced from the receiver, a plain source parameter, or static storage. A fresh owned value, or a borrow of an owned local / `using var` / `owned` parameter / temporary, is rejected. |

Five corollaries you will need constantly:

1. **A static method returning a class type must say `owned`** (or return a mint-announcing type, or a bare `T` with a source): `static method Pick(a: Res, b: Res): Res` is rejected at the declaration.
2. **An instance method that creates something must say `owned`** — otherwise its return reads as a borrow of the receiver.
3. **A property or indexer getter may never return a fresh owned value.** Getters lend only; properties carry no `owned`. Expose a factory as a *method* returning `owned T`.
4. **An iterator method's unmarked iterable return is already a transfer announcement** — no `owned` needed, at any callee kind.
5. An **unbound owned temporary** is disposed at the **end of the full expression**, after every borrowing use inside it: `items.Where(p).ToArray().Length` is fine, while binding, storing or returning a borrow *of* such a temporary past that expression is rejected. A `case` scrutinee extends the boundary to the whole `case`, so pattern bindings survive the selected arm.

### 4.4 Borrow rules

A borrow is valid only where **every** source in its source set is live; joins, `??` and arm meets take the **union** of source sets (§28.3).

- A borrow may **not** be assigned to a local whose scope strictly encloses its source's scope — including flowing around a loop back edge. Rejected **at the assignment**, not at the later use.
- A borrow of a **scope-bound local** may not be stored in a field or in a longer-lived collection. A borrow whose source *does* outlive the object **is** admissible: the **back-reference** idiom binds a constructor parameter into a plain (non-`owned`) class-typed field (`private _owner: Customer;`). The check is structural — sourced from a parameter that is not `owned` (plain or `scoped`) or from the receiver: admitted; locals, temporaries **and `owned` parameters**: rejected (an `owned` parameter is disposed at the method's end, so it is scope-bound — §28.3, v0.288).
- A borrow may not be returned unless its source is one every caller can re-establish: receiver-sourced from an instance method, the receiver-parameter borrow of an extension method, or static storage through a **static property getter**.
- A `ref` or plain `out` argument whose type can carry a borrow (a class, interface or function type, or a borrow-bearing record, choice or inline array) is a holder the callee may fill from any of the call's borrows — the receiver, the plain arguments, the other `ref` arguments' values — so each must be admissible into it exactly as `location := borrow` would be: `Put(ref outerLocal, innerUsingVar)` is rejected at the call (§28.3).
- A closure — or a method reference over an instance receiver — is a **borrow-carrying value**: its source set is the union of its captures' sources (calling a closure yields a value carrying the closure's own set), and every rule in this list applies to it by that set (§28.3, v0.298). So: bind it to any local, plain `var` or `using var`, whose scope does not strictly enclose a source's (a local helper lambda over a borrow needs no `using var`); invoke it; pass it to a function-typed parameter (§28.14). While it captures anything, do **not** store it in a field or a collection, write it through `out`/`ref`, or return it from the method — a function type announces no sources, so the only storage route for a capture-bearing closure is a `scoped` parameter (§28.14); a capture-free closure goes anywhere. Reassigning a plain function-typed local is fine — each assignment is checked on its own; `using var` on a function-typed local adds only non-reassignability.
- Every field-, collection- or async-frame-held borrow carries the obligation not to transfer the borrowed object to another domain. This has **no runtime backstop** — it is a data race (§30.14).

**Merges must agree** (§28.3). owned ⊓ owned = owned. borrowed ⊓ borrowed = borrowed, sources unioned. **owned ⊓ borrowed = rejected.** owned ⊓ moved-from = maybe-consumed (unreadable). The rule governs `if` statements, `if` expressions, `??`, and expression-`case` arms identically.

**The consume-site freeze** (§28.3). Consuming an owned **binding** while a **live local borrow of its current value** exists is a compile error. Frozen: every transfer (a `move` at an argument, binding, assignment or arm; a store into an `owned` field, an owning slot or a `set(owned)`; a `return` under an `owned` or mint-announcing return; an owning iterator's `yield return`) and drop-on-overwrite. **Not** frozen — these are site-announced kills, permitted while borrows live: an explicit `Dispose()` (including `x?.Dispose()`), the discard `_ := x;`, `throw x`, and a Pattern B `await` on an owned `OwningTask<T>` binding. Fields and collection slots are **not** frozen. The mechanical rewrite for the frozen case is **transfer first, then re-borrow from the new owner**.

**Borrow-bearing records** (§9.7). A record is borrow-bearing if it has at least one **borrow-carrying** field: one whose declared type is a reference type (class or interface, nullable or not), an inline array of reference type, or another borrow-bearing record or choice. The predicate reads **declared field types**, never runtime contents — so `string` and function-typed fields are *not* borrow-carrying, while a field declared `Object` or an interface type **is**, nullable or not. Such a value may not outlive the source of any borrow it carries; may **not be constructed from a temporary** (bind the source to a named local first); may **not** be stored in a class instance field — store the *source* (an `owned CancellationTokenSource`, not a `CancellationToken`) and re-derive the view at the use site; may sit in a **static** field only in the null-source form (every reference-typed field and inline-array slot `null` — a spelling **no span has**, its `Buffer`/`Source` being non-nullable, so spans go in neither static nor instance fields); may be **returned** only when every borrow it carries is **receiver-sourced** — the span shape `public method Header(): Span<byte> => _buf.AsSpan(0, 64);` over an `owned _buf: OwningBuffer<byte>` field is admitted, while a return carrying a borrow of a local, a `using var`, an `owned` parameter, or a **plain parameter** is rejected at the return site (declare it as an instance or extension member of the source's own type instead, whose receiver the caller re-establishes); and may not be an `async` parameter, result or element type, a local of such a type crossing a suspension point being admitted only if every borrow it carries has a frame-resident source (`CancellationToken` is the single stdlib-certified exception).

Records also cannot have destructors, `owned` fields, owning-inline-array fields, or implement `IDisposable`. The spans are the canonical borrow-bearing records; `ReadOnlyStringSpan` is the deliberate exception — its only reference field is a `string`, so it may sit in fields and cross `await`.

**Value receivers are borrows** (§9.4). In every instance member body of a **value kind** — a record method or accessor, a choice method, an enum member, an extension member over a value receiver — `this` binds a **borrow** of the receiver value, and a store that would land in the receiver's own storage is a compile error at the member body: `this.X := e`, a bare `X := e` resolving to receiver storage, an inline-array chain, compound assignment and `++`/`--` through their desugarings, and passing such a chain as a `ref`/`out` argument. The rule keys on *where the store would land*. Two boundaries: **constructor bodies are not receiver positions** (`Width := width;` is the canonical body-record initialization), and an **external** store through a named binding (`r.X := 5`) remains legal. A value kind may not conform to an interface declaring a settable property or indexer at all (§9.6). The sanctioned mediating shape is a chain that hops **through a class- or interface-typed member**, leaving the receiver — that is how `Span<T>`'s indexer setter works.

### 4.5 Iteration and collection ownership

**The two families** (§28.11):

| | Borrowing family | Owning family |
|---|---|---|
| Types | `Array<T>`, `LinkedList<T>`, `Dictionary<K,V>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `SortedDictionary<K,V>`, `SortedSet<T>` | `OwningArray<T>`, `OwningLinkedList<T>`, `OwningDictionary<K,V>`, `OwningSet<T>`, `OwningStack<T>`, `OwningQueue<T>`, `OwningSlotMap<T>` (handle-keyed; no borrowing twin) |
| Element slots at class T | **borrows** — the caller keeps ownership | **owned** |
| `Add`/`Insert`/indexer-set | stores the borrow | `Add(owned item: T)`; indexer-set disposes the previous value |
| `Remove`/`RemoveAt`/`Clear` | removes the borrow | **disposes** the element(s) |
| `Dispose()` | no user-visible work | disposes **every** element, then frees storage |
| Ownership out | *none* | `move coll[i]`, `PopAt`/`PopFirst`/`PopLast`/`Pop`/`Dequeue`, `Drain()` |

There is **no implicit conversion between the families**. Owning collections require `where T: class`. A class owning a collection of resources writes `private owned _items: OwningArray<T>;` and needs no destructor.

**What the `for` variable is** (§29.1–§29.3), decided by the **static type of the source**:

| Source | Loop variable |
|---|---|
| value elements (`Array<int>`, a range, a string's chars) | a copy |
| `string`- or function-typed elements | a shared reference |
| class elements, **either family**, default iteration | a **borrow** of the slot |
| an iterator's enumerator-owned yields | a borrow of the **enumerator's** slot, invalidated by the next advance |
| `coll.Drain()` or any `IOwningEnumerable<T>` | **owned** — you discharge it |
| `Dictionary<K,V>` | a `DictionaryEntry<K,V>`; `entry.Value` borrows at class V |
| spans, inline arrays | a direct indexed loop; copy or borrow per slot |

`OwningArray<T>` implements both `IEnumerable<T>` and `IOwningEnumerable<T>`; **default iteration takes the borrowing path**. To take elements out, express the source at `IOwningEnumerable<T>` — call `.Drain()`. The owning-path loop variable is a plain owned local, not a `using var`, so you can move it on; the discharge rule applies on **every** body-exit edge:

```bezel
for var conn in pool.Drain() do
  using var c := move conn;      // adopt on the first line — covers every exit edge
  c.SendShutdown();
end;                             // pool is now an empty container, still disposable
```

Omit the adoption and the element auto-drops each iteration with a **per-iteration must-use warning** (none at a trivial-dispose element type). Draining a `using var` collection is fine and leaves it disposable as an empty container.

Two desugaring facts (§29.2): a **fresh** iterable-family source is taken by the desugaring's hidden `using var` — `for var x in GetThings() do … end;` does not leak the state machine — while an owned local or `using var` source is **pinned**, not taken, so iterating a `using var` collection neither moves out of it nor double-disposes.

**`OwningDictionary<K,V>` has no default entry iteration** — its conformance is `IDisposable` only. Iterate `.Keys` and `PopValue` to take, or use `Drain()` (stdlib §4.9).

**Escaping an element borrow.** Over a **concrete** collection type the chain is slot → collection → binding, and keeping the reference past the loop is fine while the collection lives. Over an **interface-typed** source the compiler cannot see the backing, so the variable's source is conservatively the loop's own enumerator — a scope-bound local — and any escape is **rejected** (§29.2). Remedy: take the parameter at a concrete collection type, or give the elements an owner that outlives the loop.

**Mutating what you iterate, and using what you removed** (§29.12, §28.11). `for var x in xs do … xs.Remove(x); … end;` compiles and **warns**: the next advance raises `InvalidOperationException`. Silent and correct: collect the matches into a second collection and remove after the loop; `xs.Remove(x); break;` (no advance follows); an indexer-set `xs[i] := v` (element-level, not structural); or `while xs.Count > 0 do using var x := xs.PopLast(); … end;` on an owning collection. On an **owning** collection `var w := xs[0]; xs.RemoveAt(0); w.Ping();` compiles and **warns**: `RemoveAt` disposed the element `w` borrows (checked mode faults there). Re-take the borrow after the mutation, take the element out first with `PopAt(0)`, or mutate after the last use. Neither warning fires through a different binding (`var ys := xs;`, or a span over `xs`: its `Reverse()` still faults at the next advance, its `Fill`/`CopyFrom` do not), and neither is suppressible per site.

**Materializing is not a liveness remedy at class element types** (v0.265). `ToArray()`/`ToSet()`/`ToDictionary(…)` there build **borrowing** collections, so the materialized slots dangle exactly as the original borrow did; materialize-first is the **re-enumeration** remedy over a *live* backing, nothing more. What does remedy a dead-on-arrival element borrow: consume each element inside the loop body that borrows it, or give the elements an owner (an `IOwningEnumerable<T>` producer, consumed by owning iteration into an owning collection, with the accessor applied to *that* collection). Where the producer is a borrowing minting iterator you cannot change, **no retention remedy exists** (§29.11).

**Lazy queries are single-consumption** (§29.11): enumerated at most once. The right is spent by enumeration or a terminal operator, by applying a further deferred operator (`var q2 := q.Select(f);`), or by passing the query as an `IEnumerable<T>` argument. It is orthogonal to ownership.

**Element disposal is the owner's, always** (§28.6). `Dispose()` requires a receiver you own: an owned local or `owned` parameter, a `using var`, an unbound owned temporary, a `move` result, or an `owned` field / owning slot read by code of the owning object's declaring type. Through a **borrow it is a compile error with no exception** — a borrowed local or parameter, **`this`**, a non-`owned` field read, an owned-field read outside the declaring class, a getter result, a `catch`-arm binding, a pattern binding, a captured owned binding inside a closure, and an element borrow of *either* family. A borrowed **owning collection** may be added to, removed from, drained and read — the pass-and-fill idiom `method Fill(a: OwningArray<T>)  a.Add(…)` is core — but it may not be `Dispose`d.

**Fresh values into collections** (§15.4, §28.11). On the **owning** family a fresh producer is idiomatic and warning-free: `pool.Add(new Connection("a"));`. On the **borrowing** family the same shape via `Add`/`Insert`/indexer-set is admitted but **warns** (the temporary dies at the statement's end and the slot dangles) — and so does a fresh class-typed **key** into an `OwningDictionary`, whose keys are borrowed (bind the key first: `using var k := new K(); od[k] := v;`), and inside a collection **literal** a fresh non-trivial-dispose element is a hard **error** at the literal site — bind first, or use the owning family. Runtime-managed elements are unaffected: `summary.Add(name)` on an `Array<string>` carries no obligation and no warning.

### 4.6 The diagnostics you will actually hit

Section 7 lists each with its remedy. In frequency order they are: the **must-use warning** on an undischarged plain-`var` owned local; a **mixed owned/borrowed merge** rejected at the join; *"borrow held by `x` would outlive its source `y`"*, reported at the **assignment**; *"this call consumes `x`; write `M(move x)`"* and its inverse (the marker written where the target already announces); *"cannot move out of `using var`"*; a **field moved and not restored** on some exit path; a fresh owned value **escaping into a non-owning location** (a borrow field, a getter, a plain return); a **static** method's plain class-typed return, rejected at its declaration; `Dispose()` **on a borrow**; the **consume-site freeze**; the **fresh-store warning** on a borrowing collection; the **terminal-accessor provenance warning**; the **mutation-during-iteration warning** and the **element-borrow use-after-removal warning** (§4.5); and `_ := someTask;` / `using var _ := someTask;`. One more is opt-in: build with `--report-trust-admissions` (spec §28.3's trust-admission report mode, v0.307) and every borrow you store in a plain field, a borrowing collection, or an abandoned task frame draws a warning naming the lifetime you are trusting and the static spelling that would prove it (`scoped`, a `SlotHandle`, `Shared<T>`, the owning family, `await`); a unit that stays silent under the flag holds no trusted lifetime at all.

---

## 5. The standard-library index

Everything below is transcribed from `Bezel_Standard_Library.md` v0.313 with its markers intact. **A signature with its markers stripped is worse than no signature**: `owned`, `pure`, `ref`, `out`, `scoped`, nullability and constraints are load-bearing. **If a name is not here, look it up in the stdlib document; if it is not there either, it does not exist.**

### 5.0 Modules and imports

| Module | Import | Contents |
|---|---|---|
| `System` | auto | `Console`, `Math`, `Convert`, `Environment`, temporal types, `Guid`, `Random`, the core interfaces, `Type`, the serialization vocabulary, `Shared`/`Weak`, `Result`, `ScopeGuard`, `Subscription`, the FFI carriers |
| `System.Text` | auto | `StringBuilder`, `Encoding`, `ReadOnlyStringSpan` |
| `System.Collections.Generic` | **auto** (v0.289) | every §4 name — the collections, the spans, `Counter`, `FrozenArray`, `IEqualityComparer`, `KeyNotFoundException`, the iteration interfaces. An explicit `import` line is legal and redundant |
| `System.Collections.Linq` | **auto** (v0.278) | the query operators. No `import` needed — §2.3's invariant: a stdlib extension over an always-available type is auto-imported |
| `System.Threading` | **explicit** | *except* the `Task` family, `CancellationToken`, `Subscription`, and the six event types, which are auto — `CancellationTokenSource` itself needs the import |
| `System.IO` | **explicit** | `Stream`, `FileStream`, `File`, `TextReader`/`TextWriter`, `Directory`, `Path` |
| `System.Diagnostics` | **explicit** | `Stopwatch`, `Debug` |
| `System.Serialization` | **explicit** | `JsonSerializer`/`JsonDeserializer`/`BinarySerializer`/`BinaryDeserializer`/`JsonOptions`/`SerializationException`/`Json`/`Binary` |
| `System.Interop` | **explicit** | `Ptr<T>`, `Interop` |

**Deferred — do not use:** `System.Net`, `System.Net.Http` (**there is no `HttpClient`**), `System.Text.Json` DOM, `System.Text.Regex`, `System.Security.Cryptography`, `System.Globalization`, `System.Reflection`, `System.Diagnostics.Process` (stdlib §11).

### 5.1 The Try shapes and the `*OrNull` tier

Exactly four sanctioned shapes; nothing else is a Try (stdlib §1).

| Shape | Signature form | When | Members |
|---|---|---|---|
| **(a)** bool + **nullable** out | `TryX(…, out item: T? when true): bool` (`out owned` where the payload transfers) | generic or **class**-typed payload | `Dictionary`/`OwningDictionary`/`SortedDictionary`.`TryGetValue`; `Stack`/`OwningStack`.`TryPop`,`TryPeek`; `Queue`/`OwningQueue`.`TryDequeue`,`TryPeek`; `OwningArray`/`OwningLinkedList`.`TryPopFirst`,`TryPopLast`; `OwningSet.TryPop`; `OwningDictionary.TryPopValue`; `Channel<T>.TryReceive`; `IParsable<T>.TryParse` |
| **(b)** nullable **return** | `TryX(…): owned T?` | acquisition-shaped, payload statically non-nullable | `Mutex.TryAcquire`, `Semaphore.TryAcquire`, `Weak<T>.TryUpgrade`, `Locked<T>.TryLock`, `RwLocked<T>.TryRead`/`TryWrite`, and their `*Async` forms |
| **(c)** bool + **non-nullable value** out (`default(T)` on false) | `TryX(…, out result: T): bool` | concrete **value**-type payload | the primitive `TryParse` family; `Convert.TryTo*`; `DateTime`/`DateOnly`/`TimeOnly`/`Guid`.`TryParse`; the span/bit `TryRead*` accessors |
| **(d)** bool only, no out | `TryX(…): bool` | succeeded/declined; the payload was passed *in* | `Set.TryAdd`, `OwningSet.TryAdd`, `SortedSet.TryAdd`, the three dictionary `TryAdd`s (v0.282), `TaskCompletionSource.TrySet*`, the span/bit `TryWrite*` |

**The `when true` postcondition is normative on every shape-(a) member** even though the per-type listings elide it: inside a `true`-narrowed region the recipient needs no `!`. It has one spelling — the clause; a hand-written `static ensures(not result or p <> null)` is an ordinary contract clause and narrows nothing (§22.16).

**Three access tiers over one operation:** the **throwing** tier (absence is a bug) — `Parse`, `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`, `Console.ReadKey`; the **`*OrNull`** tier, returning `T?` for `??`/`?.` composition — `ParseOrNull`, `GetOrNull`, `Console.ReadKeyOrNull`, `FirstOrNull`/`LastOrNull`/`SingleOrNull`/`ElementAtOrNull`/`MinOrNull`/`MaxOrNull`, `E.FromUnderlyingOrNull`, `E.FromNameOrNull` (`E` = the enum's own name; the declared identifier, exact ordinal match — never a `serialized` rename; v0.293; its inverse `E.NameOf(v): string` is total and equals `nameof(E.X)` for a written `X`); and the **`Try`** tier, the only shape that distinguishes "absent" from "present null". `*OrNull` **conflates** absent with a present `null` wherever the payload can be null — use `ContainsKey`/`Count`/the `Try` shape when the distinction matters.

**Negative magnitudes fault:** `capacity`/`size`/`length` < 0 raises `ArgumentOutOfRangeException` on the `StringBuilder`, `Array<T>`, `OwningArray<T>`, `Dictionary`/`OwningDictionary`/`Set`/`OwningSet` capacity forms and `OwningBuffer<T>`. Zero is valid. `Channel<T>(capacity)` requires `capacity >= 1`, else `ArgumentException`.

**Naming conventions.** The `Async` suffix marks the async half of a sync/async pair only; async-only operations carry no suffix. Borrowing/owning duals are **same-name overloads**, not `Owned`-suffixed (surviving `Owned` names: `Json.ParseOwned<T>`, `Binary.ParseOwned<T>`, `Task.FromOwnedResult`). **An instance member blocks a same-named extension, by name, at any arity** — on an `Array<T>` receiver, `Contains`, `Count`, `Reverse`, `ToArray` resolve to the *instance* members, so `arr.Count(pred)` does **not** compile; write `arr.Where(pred).Count()`.

**Exception safety when a mutator re-enters user code:** the basic guarantee only. The receiver stays valid and every owned element is still disposed exactly once; contents are otherwise unspecified. No member promises all-or-nothing.

### 5.2 `System` (auto)

```bezel
public static class Console
  public static method Write(value: string);   Write(value: Object);
  public static method WriteLine();  WriteLine(value: string);  WriteLine(value: Object);
  //   WriteLine appends Environment.NewLine; Write appends nothing. Console.Write/WriteLine
  //   ARE Console.Out.Write/WriteLine (one destination, call order). A null Object argument
  //   renders as the four characters `null` (§18.5's rendering; no ToString dispatch).
  // + per-type Write/WriteLine overloads for bool and every numeric primitive
  public static method ReadLine(): string?;            // null at EOF
  public static method ReadKey(): char;                // EndOfStreamException at end of input
  public static method ReadKeyOrNull(): char?;
  public static readonly property In: TextReader;   Out: TextWriter;   Error: TextWriter;
end

public static class Math
  public static const Pi: double;   Tau: double;
  public static pure method Abs(int|long|double|decimal) -> same;
  public static method Min<T>(a: T, b: T): T where T: IComparable<T>;     // NOT pure
  public static method Max<T>(a: T, b: T): T where T: IComparable<T>;     // NOT pure
  public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;
  public static pure method Sqrt(x: double): double;
  public static pure method Pow(x: double, y: double): double;
  public static method DivRem(a: int, b: int, out remainder: int): int;   // + long form; NOT pure
  public static pure method Floor/Ceiling/Truncate(x: double): double;
  public static pure method Round(x: double, digits: int := 0): double;
  public static pure method Floor/Ceiling/Truncate(x: decimal): decimal;  // no Math.Round(decimal)
  public static method Sign<T>(x: T): int where T: INumber<T>;   // NOT pure (v0.290); -1/0/1; NaN → 0; no char argument
  public static pure method Fmod(x, y: double): double;   IEEERemainder(x, y: double): double;
end
```

`Min`/`Max`/`Clamp` at a **class** substitution return a **borrow** sourced from the arguments. `Min` returns `a` when `a.CompareTo(b) <= 0` else `b`; `Max` returns `a` when `a.CompareTo(b) >= 0` else `b` — **a tie returns `a`**; `Clamp` returns `low`/`high`/`value`. A **tick is 100 ns**: `TimeSpan.From<Unit>(n)` is *n* of that unit (truncated toward zero to a whole tick; `OverflowException` out of range or on `NaN`) and `Total<Unit>` is the whole span in that unit including its fraction. A **seeded** `Random` replays: same seed + same calls = same values; the algorithm is unspecified, so never assume a sequence across implementations.

```bezel
public static class Convert     // string <-> T only; there is no Convert.ToInt32(double)
  ToInt32/ToInt64/ToDouble/ToDecimal/ToBoolean(value: string);
  TryToInt32/.../TryToBoolean(value: string, out result: T): bool;         // shape (c)
  ToString(int|long|double|decimal|bool);   ToString(value: int|long, toBase: int);  // bases 2/8/10/16
end                                                    // faults: FormatException, OverflowException

public static class Environment
  public static readonly property NewLine, MachineName, UserName, OSVersion: string;
  public static readonly property ProcessorCount: int;
  public static readonly property CommandLineArgs: IReadOnlyList<string>;
  public static method Get(name: string): string?;
  public static method GetInt(name: string): int?;   GetBool(name: string): bool?;
  public static method Set(name: string, value: string?);      // null removes
  public static method Variables(): owned Dictionary<string, string>;
  public static method Exit(exitCode: int);
end
```

**Temporal types — all records (value types).** `DateTime`: constructors `(y,m,d)` and `(y,m,d,h,min,s, millisecond := 0, kind := DateTimeKind.Unspecified)`; `Ticks: long` with `FromTicks`/`ToUnixTimeSeconds`/`FromUnixTimeSeconds`; the `Year/Month/Day/Hour/Minute/Second/Millisecond/DayOfYear: int` components plus `DayOfWeek`, `Kind`, `Date: DateOnly`, `TimeOfDay: TimeOnly`; statics `Now/UtcNow/Today/MinValue/MaxValue`; `AddYears/AddMonths(int)`, `AddDays/AddHours/AddMinutes/AddSeconds/AddMilliseconds(double)`, `Add(span: TimeSpan)`, both `Subtract` forms, `ToUtc()`, `ToLocal()`, `ToString([format])`, `Parse`/`TryParse`/`ParseOrNull`, `CompareTo`, and the `+`/`-` operators over `TimeSpan`. Parse accepts only `yyyy-MM-ddTHH:mm:ss[.fff]` and `yyyy-MM-dd`. `TimeSpan`: `Ticks`/`FromTicks`, the `Days/…/Milliseconds: int` and `TotalDays/…/TotalMilliseconds: double` pairs, statics `FromDays/FromHours/FromMinutes/FromSeconds/FromMilliseconds(double)`, `Zero/MinValue/MaxValue`, `ToString([format])`, `+ - * /` (`*`/`/` take `double`) and unary `-`. `DateOnly` (`(y,m,d)`, `Year/Month/Day`, `ToDateTime(time: TimeOnly)`) and `TimeOnly` (`(hour, minute, second := 0, millisecond := 0)`, `Hour/Minute/Second/Millisecond`) each add `CompareTo`, `Parse`/`TryParse`/`ParseOrNull` and `ToString([format])`. `enum DayOfWeek: byte`; `enum DateTimeKind: byte { Unspecified, Utc, Local }`.

`Guid` (record) — `CompareTo`; `static FromByteArray(bytes: Array<byte>): Guid`; `static readonly Empty`; `static NewGuid(): Guid` (**not cryptographic — never a secret**); `Parse`/`TryParse(out result: Guid)`/`ParseOrNull`; `ToString()`; `ToString(format)` (`"N"`/`"D"` only); `ToByteArray(): owned Array<byte>`. **No constructor** — `new Guid(…)` is a compile error.

The seven temporal format specifiers (`yyyy MM dd HH mm ss fff`) each render a fixed-width zero-padded field of their own letter count — `dd` renders day 5 as `05`, `yyyy` renders year 42 as `0042`; any other run length of those letters is a `FormatException`.

`Random` (class) — `constructor()`, `constructor(seed: long)`; `Next(): int`, `Next(maxValue)`, `Next(minValue, maxValue)`, `NextInt64(): long`, `NextDouble(): double`, `NextBytes(buffer: Array<byte>)`, `NextBoolean(): bool`.

```bezel
public interface IComparable<T>    method CompareTo(other: T): int;   end
public interface IEquatable<T>     method Equals(other: T): bool;     end
public interface IDisposable       method Dispose();                  end   // every class implicitly implements
public interface IAsyncDisposable  method DisposeAsync(): Task;       end
public interface IFormattable      method ToString(format: string): string;   end
public class Subscription  destructor; end     // no public ctor; returned as `owned Subscription`
```

`IComparable<T>`/`IEquatable<T>` are **invariant**. `BitConverter`: `FloatToInt32Bits`, `Int32BitsToFloat`, `DoubleToInt64Bits`, `Int64BitsToDouble`, `Reinterpret<TTo>(value)`. `Type` (`public sealed class`): `Name` (the identifier alone), `FullName` (module path + enclosing-type path + identifier, `"M.Outer.Inner"`; the root is `"System.Object"`), `IsClass/IsInterface/IsRecord/IsEnum/IsChoice/IsPrimitive/IsInlineArray/IsFunctionType/IsValueType`, `BaseType: Type?`, `IsAssignableFrom(other)` (interface conformance is `iface.IsAssignableFrom(t)` — there is no `Implements`), `EnumNames`, `EnumValues` — **no member reflection at all** (declined, not deferred).

**Format specifiers** (invariant culture only; an unrecognized specifier raises `FormatException`): integers `D`/`Dn`, `X`/`x`/`Xn`, `B` (no grouping separators); `double`/`float` `F`/`Fn` (default 2), `E`/`En`, `G`/`Gn`, `P`/`Pn`, `R`; `decimal` `F`/`Fn`, `E`/`En`, `G`, `P`/`Pn` (midpoints half-to-even); `DateTime`/`DateOnly`/`TimeOnly` patterns of `yyyy MM dd HH mm ss fff` with every other character a literal; `TimeSpan` `c`; `Guid` `D`/`N`; `bool`, `char`, `string` take **none** — a format spec on those is a compile error. **`C` (currency) deliberately does not exist**: write `$"{amount:F2} EUR"`.

**Exceptions** (spec §31.4):

```bezel
public class Exception
  public pure constructor();   public pure constructor(message: string);
  public constructor(message: string, cause: Exception);     // snapshot chaining; cause NOT retained
  public readonly property Message: string;   StackTrace: string;
  public readonly property CauseChain: Array<ExceptionInfo>;       // outermost cause first
  public readonly property SupersededChain: Array<ExceptionInfo>;  // chronological
  public override method ToString(): string;                       // v0.268; NOT pure
end
public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string);
public class ArgumentException: Exception
  public pure constructor(message: string);
  public pure constructor(message: string, paramName: string);
  public readonly property ParamName: string?;
end
public class TypeInitializationException: Exception
  public readonly property InnerException: Exception?;   // null at §22.13's cycle backstop
end                                                      // runtime-owned: never disposed after an arm
```

Hierarchy: `Exception` → `ContractViolationException`; `ArgumentException` → `ArgumentNullException`, `ArgumentOutOfRangeException`; `ArithmeticException` → `OverflowException`, `DivideByZeroException`; `InvalidOperationException` → `DisposedAccessException`; plus `NotImplementedException`, `NullReferenceException`, `IndexOutOfRangeException`, `MatchFailureException`, `OperationCanceledException`, `TypeInitializationException`, `FormatException`, `StackOverflowException` (uncatchable). **There is no `OutOfMemoryException`** — allocation failure terminates. **No user `InnerException`** — chain with `base(msg, cause)`; `TypeInitializationException.InnerException` (above) is the one built-in that carries a live reference. Producer-local exceptions need the producer's import: `KeyNotFoundException`, `SerializationException`, `TimeoutException`/`ChannelClosedException`/`ChannelFullException`/`DomainFaultedException`, `IOException`/`FileNotFoundException`/`DirectoryNotFoundException`/`EndOfStreamException`/`PathTooLongException`.

`Exception.ToString()` (v0.268) is a **declared override** for `Exception` and every derived type. A conforming rendering contains the qualified type name, the `Message` when non-empty, every `CauseChain` entry (outermost first) and every `SupersededChain` entry (earliest first, distinguishable from causes). It renders **no stack trace**. Arrangement is implementation-defined; the member is **not `pure`**.

```bezel
public class Shared<T>: IDisposable where T: class
  public constructor(owned value: T);          // count = 1
  public method Clone(): owned Shared<T>;      // count + 1
  public method Get(): T;                      // borrow; source = this handle
  public method Downgrade(): owned Weak<T>;
end
public class Weak<T>: IDisposable where T: class
  public method TryUpgrade(): owned Shared<T>?;         // Try shape (b)
end
public class ScopeGuard                                  // the sanctioned `errdefer`
  public constructor(scoped action: () -> ());           // runs at disposal unless dismissed
  public method Dismiss();                               // idempotent
  public readonly property IsDismissed: bool;
  destructor;
end
public choice Result<T, E>                               // auto-imported
  Ok(owned value: T),
  Error(owned error: E);
  public method IsOk(): bool;      public method IsError(): bool;
  // compiler-provided; admissible ONLY where receiver AND result instantiation are value-like:
  public method Map<U>(f: (T) -> U): Result<U, E>;
  public method MapError<F>(f: (E) -> F): Result<T, F>;
  public method AndThen<U>(f: (T) -> Result<U, E>): Result<U, E>;
  public method UnwrapOr(fallback: T): T;
end
```

`Shared<T>` is sendable iff `T` ∈ {`Mutex`, `Semaphore`, `Atomic<T>`, `AtomicInteger<T>`, `Locked<T>`, `RwLocked<T>`, `FrozenArray<T>`, `CancellationTokenSource`}. Cycles through `Shared<T>` leak; break them with `Weak<T>`.

**`Result` has no `Unwrap`** — extraction is consumption, done by an owning `case move x of …`. `Option<T>` is deliberately absent: **`T?` is Bezel's option type.** None of the four combinators is `pure`, and **`UnwrapOr`'s argument is evaluated eagerly** (section 8.2). Owning instantiations carry none of the four members.

**Serialization vocabulary** (auto-imported): `ISerializer` and `IDeserializer` (the `WriteInt`/`ReadInt`-style scalar pairs plus `BeginObject`/`WriteFieldName`/`EndObject` and `BeginArray`/`EndArray` framing — stdlib §2.14 lists all of them), `ISerializable` (`Serialize(s: ISerializer)`), and `IDeserializable` — a **marker**: the compiler recognizes `static method Deserialize(d: IDeserializer): T` / `: owned T` / `: owned Shared<C>` by name. Records and enums declaring `: ISerializable, IDeserializable` get synthesized bodies; a record or choice used as a **component** must declare the conformance itself (enums need not), or the enclosing declaration is rejected; `serialized "<name>" | skip | optional` customizes; `$id`/`$ref`/`$type`/`$case` are reserved field names. Concrete formats (`import System.Serialization;`): `JsonSerializer`/`JsonDeserializer`/`BinarySerializer`/`BinaryDeserializer`, `JsonOptions`, and the facades `Json.ToText<T>`/`ToBytes<T>`/`Parse<T>` (value targets only)/`ParseOwned<T>` (`where T: class`) and `Binary.ToBytes<T>`/`Parse<T>`/`ParseOwned<T>`.

**Static-interface abstractions:** `ICollectionLiteralTarget<TSelf, T>` (`static FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned TSelf`) and `IOwningCollectionLiteralTarget<TSelf, T>` (`static FromCollectionLiteral(owned elements: OwningArray<T>): owned TSelf`) — the sequence-literal target pair (spec §15.4.1): a user **class** declares exactly one, at `TSelf` = itself (`class Ring<T>: ICollectionLiteralTarget<Ring<T>, T>`), and the built-ins declare it too — `Array/LinkedList/Set/Stack/Queue<T>` the borrowing one, the five `Owning*<T>` the owning one — so each lists a callable `FromCollectionLiteral`; `INumber<T>` (`static Zero()`, `static One()`, operators `+ - * /`, `<`), `ISteppable<T>` (`static Step(value: T, by: long): owned T`, `<`), `IParsable<T>` (`static Parse(s: string): owned T`, `static TryParse(s: string, out owned value: T?): bool`). Conformers: the ten numeric primitives conform to all three; `char` conforms to `ISteppable`/`IParsable`; **`string` and `bool` conform to none**.

### 5.3 `System.Text` (auto)

`StringBuilder` — `constructor()`, `(capacity: int)`, `(initial: string)`; `Length: int`, `pure Capacity: int`; `Append(string|char|Object|bool|int|byte|sbyte|short|ushort|uint|long|ulong|float|double|decimal|ReadOnlyStringSpan): StringBuilder`; `AppendLine()`, `AppendLine(value: string)`; `AppendFormat(format: string, args: Array<Object>)`; `Insert(index, value: string)`, `Remove(startIndex, length)`, `Replace(oldValue, newValue: string)`, `Clear()` — all returning `StringBuilder`; `ToString(): string`.

`Encoding` (`public abstract class`) — `abstract GetBytes(s: string): owned Array<byte>`; `abstract GetString(bytes: Array<byte>)`, `GetString(bytes: ReadOnlySpan<byte>)`, `GetString(bytes: ReadOnlyArraySpan<byte>)` (a window over an array is `GetString(bytes.AsReadOnlySpan(start, length))` — there is no `(bytes, start, length)` form); `abstract GetByteCount(s: string): int`; `abstract GetBytes(s: string, destination: Span<byte>): int`; `abstract GetByteCount(s: ReadOnlyStringSpan): int`; `abstract GetBytes(s: ReadOnlyStringSpan, destination: Span<byte>): int`; `abstract GetBytes(s: ReadOnlyStringSpan): owned Array<byte>`; `abstract readonly property Name: string`; `static readonly property UTF8, UTF16, UTF32, ASCII: Encoding`. Malformed input raises `FormatException`, **never** a replacement character. UTF8 emits no BOM; UTF16/UTF32 are little-endian only.

`ReadOnlyStringSpan` — `public record (Source: string, Start: int, Length: int): IReadOnlySpan<char>`; **ordinary record, not borrow-bearing**. `property [i: int]: char { get }`; `pure IsEmpty: bool`; `pure Slice(start, length) / Slice(start) / Slice(r: Range<int>)`; `pure IndexOf(char|ReadOnlyStringSpan|string): int`; `pure Contains(…): bool`; `pure StartsWith`/`EndsWith(ReadOnlyStringSpan|string): bool`; `pure SequenceEqual(ReadOnlyStringSpan|string|IReadOnlySpan<char>): bool`; `CopyTo(destination: Span<char>)` (**not pure**); `pure ToString(): string`. `ReadOnlyAsciiByteSpan` was **retired** and does not exist.

### 5.4 `System.Collections.Generic` (auto-imported, v0.289)

```bezel
public interface IReadOnlyCollection<out T>: IEnumerable<T>   pure readonly property Count: int;  end
public interface IReadOnlyList<out T>: IReadOnlyCollection<T>   readonly property [i: int]: T;  end
public interface ICollection<T>: IReadOnlyCollection<T>
  readonly property IsReadOnly: bool;
  method Add(item: T);  Clear();  Contains(item: T): bool;  Remove(item: T): bool;
end
public interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;   method Insert(index: int, item: T);
end
public interface IOwningCollection<T>: IReadOnlyCollection<T>, IDisposable where T: class
  readonly property IsReadOnly: bool;
  method Add(owned item: T);  Clear();               // Clear disposes every element
  method Contains(item: T): bool;                    // borrow-taking query
  method Remove(item: T): bool;                      // disposes the removed element
end
public interface IOwningList<T>: IReadOnlyList<T>, IOwningCollection<T> where T: class
  property [i: int]: T { get; set(owned); }          // the setter disposes the previous value
  method Insert(index: int, owned item: T);
end
```

`IList<T>` and `IOwningList<T>` are unrelated — the two hierarchies share only the read-only interfaces, so there is **no single "any mutable list" abstraction**.

```bezel
public class Array<T>: IList<T>                      // borrowing, resizable
  constructor();  constructor(capacity: int);  constructor(size: int, fill: T);
  pure readonly property Count: int;   pure readonly property Capacity: int;   property [i: int]: T;
  method Add(item: T);  AddRange(items: IEnumerable<T>);  Insert(index, item);  RemoveAt(index);
  method Remove(item: T): bool;  Clear();  Contains(item: T): bool;
  method IndexOf(item: T, startIndex: int := 0): int;   LastIndexOf(item: T): int;
  method Sort(comparer: (T, T) -> int);   method Reverse();                        // in place
  method Sorted(comparer: (T, T) -> int): owned Array<T>;   Reversed(): owned Array<T>;
  method ToArray(): owned Array<T>;
  method AsSpan([start[, length]] | r: Range<int>): ArraySpan<T>;
  method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlyArraySpan<T>;
  method Slice(r: Range<int>): owned Array<T>;       // `arr[r]` is the compiler-recognized form
  method Sort(); method Sorted(): owned Array<T>;    // natural order; only where T: IComparable<T>,
                                                     //   checked at the call's instantiation (v0.275)
end
```

`[1, 2, 3]` targets `Array<T>` (or an inline array). `Dispose()` is a no-op; elements are not disposed. `Sort`/`Sorted` are **not stable**; `OrderBy`/`OrderByDescending`/`ThenBy`/`ThenByDescending` are. Structural mutation inside a comparer raises `InvalidOperationException`.

```bezel
public class OwningArray<T>: IOwningList<T>, IOwningEnumerable<T> where T: class
  constructor();  constructor(capacity: int);
  pure readonly property Count: int;   Capacity: int;
  property [i: int]: T { get;  set(owned); }          // get = borrow of the slot; set disposes previous
  method Add(owned item: T);   AddRange(owned items: OwningArray<T>);   // AddRange DRAINS items
  method Insert(index: int, owned item: T);
  method RemoveAt(index: int);   Remove(item: T): bool;   Clear();      // all dispose
  method Contains(item: T): bool;   IndexOf(item: T): int;
  method Sort(comparer: (T, T) -> int);   Sort();   Reverse();   // comparer takes borrows;
                                                                //   bare Sort() is natural order,
                                                                //   only where T: IComparable<T>
  method PopAt(index: int): owned T;   PopFirst(): owned T;   PopLast(): owned T;
  //   empty → InvalidOperationException (Pop/PopFirst/PopLast/Dequeue/Peek, every family);
  //   PopAt's index outside 0..<Count → IndexOutOfRangeException. Clear() disposes in
  //   Dispose()'s order: reverse STORAGE order for the ordered families
  //   (descending index / tail-to-head / pop / reverse-dequeue, v0.302).
  method TryPopFirst(out owned item: T?): bool;   TryPopLast(out owned item: T?): bool;
  method Drain(): IOwningEnumerable<T>;
end
// No `OwningArray<T>` extension supplies a default-comparer `Sort`: §26.5 rule 1
// commits instance resolution by *name*, and the class declares `Sort` above, so an
// extension member of that name is never consulted. Callers pass a comparer, or
// call the parameterless `Sort()` above where `T: IComparable<T>` (v0.276).
```

There is **no `TryPopAt`**, and `OwningArray<T>` declares **no `Reversed`, no `Sorted`, no `ToArray`** — an *owning* copy is inexpressible. Reorder in place with `Reverse()`/`Sort()`; move elements to a new owning collection with `Drain()`. `move arr[i]` takes an element out, leaving the slot moved-from (reassign it before the next exit point). `Drain()` mints a fresh drain-view frame — disposing the frame does not dispose the collection, and partial iteration leaves the remainder.

The §5 extension `ToArray()` **is** reachable on an `OwningArray<T>` receiver (it is not declared by name on the class) and returns `owned Array<T>` — a **borrowing snapshot** whose slots hold borrows of elements the owning array still owns. No duplicate ownership; the obligation is a *lifetime* one: the source must outlive every use, and `Clear`/`Remove`/`RemoveAt`/an indexer-set kills what a snapshot slot points at. Same for `ToSet()`, `ToDictionary(…)` and every other §5 operator on an owning receiver.

```bezel
public class LinkedList<T>: ICollection<T>
  constructor();  constructor(source: IEnumerable<T>);
  pure readonly property Count: int;  FirstNode, LastNode: LinkedListNode<T>?;  First, Last: T?;
  method AddFirst/AddLast(item: T): LinkedListNode<T>;
  method AddBefore/AddAfter(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  method RemoveFirst();  RemoveLast();  RemoveNode(node);  Find(item: T): LinkedListNode<T>?;
end
public class LinkedListNode<T>   public readonly property Value: T;  Next, Previous: LinkedListNode<T>?;  end
public class OwningLinkedList<T>: IOwningCollection<T> where T: class
  constructor();                     // NO source constructor — deliberate. Same shape with `owned item` on
  method PopFirst(): owned T;        //   AddFirst/AddLast/AddBefore/AddAfter; Remove*/Clear dispose.
  method PopLast(): owned T;   TryPopFirst/TryPopLast(out owned item: T?): bool;
  method Drain(): IOwningEnumerable<T>;
end
```

**`List<T>` is not defined; the name is reserved.** `Array<T>` is the resizable form.

```bezel
public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>    // K must be hashable
  constructor();  (capacity: int);  (comparer: IEqualityComparer<K>);  (capacity, comparer);
  pure readonly property Count: int;
  readonly property Keys: IReadOnlyCollection<K>;   Values: IReadOnlyCollection<V>;   // live views
  property [key: K]: V;                             // get: KeyNotFoundException; set: add-or-update
  method Add(key: K, value: V);                     // ArgumentException if present
  method TryAdd(key: K, value: V): bool;            // v0.282: false, no mutation, if present
  method ContainsKey(key: K): bool;   Contains(key: K): bool;     // Contains enables `key in dict`
  method Remove(key: K): bool;
  method TryGetValue(key: K, out value: V?): bool;
  method GetOrNull(key: K): V?;
  method GetOrAdd(key: K, factory: (K) -> V): V;    // a READ path — mutating its result mutates nothing
  method Clear();
end
public record DictionaryEntry<K, V>(Key: K, Value: V);
public interface IEqualityComparer<T>  method Equals(a: T, b: T): bool;  method GetHashCode(value: T): int;  end

public class OwningDictionary<K, V>: IDisposable where V: class
  constructor();  (capacity);  (comparer);  (capacity, comparer);
  pure readonly property Count: int;   readonly property Keys, Values;
  property [key: K]: V { get; set(owned); }         // get = borrow; set disposes previous
  method Add(key: K, owned value: V);               // if present: ArgumentException AND the value is disposed
  method TryAdd(key: K, owned value: V): bool;      // v0.282: false on duplicate — value still DISPOSED on that path
  method ContainsKey(key)/Contains(key): bool;   method Remove(key: K): bool;   // Remove disposes
  method TryGetValue(key: K, out value: V?): bool;  // the out is a BORROW
  method GetOrNull(key: K): V?;                      // a look, not a take
  method GetOrAdd(key: K, factory: (K) -> owned V): V;      // returns a borrow
  method PopValue(key: K): owned V;   TryPopValue(key: K, out owned value: V?): bool;
  method Drain(): IOwningEnumerable<DrainedEntry<K, V>>;   method Clear();
end
public class DrainedEntry<K, V>: IDisposable where V: class
  constructor(key: K, owned value: V);   readonly property Key: K;   public owned Value: V;
end
```

Dictionary iteration yields `DictionaryEntry<K, V>` in **insertion order**. **There is no `Set` method on any collection** — write through the indexer. `dict[key] += 1` throws on an absent key (compound = get + set): initialize first, or use `Counter<K>`. `OwningDictionary` conforms to `IDisposable` **only** — `for var e in ownedDict do` is not provided.

```bezel
public class Set<T>: ICollection<T>                  // T must be hashable
  constructor();  (capacity);  (comparer);  (capacity, comparer);
  method TryAdd(item: T): bool;
  method UnionWith / IntersectWith / ExceptWith(other: IEnumerable<T>);
  method IsSubsetOf / IsSupersetOf(other: IEnumerable<T>): bool;
end
public class OwningSet<T>: IOwningCollection<T> where T: class
  constructor();  (capacity);  (comparer);  (capacity, comparer);
  method TryAdd(owned item: T): bool;          // on false (duplicate) the passed item is DISPOSED
  method Pop(query: T): owned T;   TryPop(query: T, out owned removed: T?): bool;
  method Drain(): IOwningEnumerable<T>;
  method UnionWith(owned other: OwningSet<T>);       // DRAINS other; duplicates disposed
  method IntersectWith(other: IEnumerable<T>);       // disposes elements not in other
  method ExceptWith(other: IEnumerable<T>);          // disposes elements in other
  method IsSubsetOf / IsSupersetOf(other: IEnumerable<T>): bool;
end

// Stack / Queue — all four conform to IReadOnlyCollection<T> ONLY: no Add, no Remove, no IsReadOnly
Stack<T>:        Push(item: T);  Pop(): T;  pure Peek(): T;  TryPop(out item: T?): bool;
                 TryPeek(out item: T?): bool;  Contains(item: T): bool;  Clear();
OwningStack<T>:  Push(owned item: T);  Pop(): owned T;  pure Peek(): T;  TryPop(out owned item: T?): bool;
                 TryPeek(out item: T?): bool;  Contains;  Clear();  Drain(): IOwningEnumerable<T>;
Queue<T>:        Enqueue(item: T);  Dequeue(): T;  pure Peek(): T;  TryDequeue(out item: T?): bool;
                 TryPeek(out item: T?): bool;  Contains;  Clear();
OwningQueue<T>:  Enqueue(owned item: T);  Dequeue(): owned T;  pure Peek(): T;
                 TryDequeue(out owned item: T?): bool;  TryPeek(out item: T?): bool;  Contains;  Clear();
                 Drain(): IOwningEnumerable<T>;
```

`OwningSet<T>.Add(owned item)` returns `()` and **silently disposes the item on duplicate**. `Set<T>` iterates in insertion order; `OwningSet<T>` makes no iteration-order guarantee. Literal ordering: `Queue` first-element-at-front, `Stack` bottom-to-top (`[1,2,3]` pops 3, 2, 1).

```bezel
public interface IEnumerable<out T>   method GetEnumerator(): owned IEnumerator<T>;  end
public interface IEnumerator<out T>: IDisposable
  readonly property Current: T;                      // non-destructive borrow
  method MoveNext(): bool;
end
public interface IAsyncEnumerable<out T>
  method GetAsyncEnumerator(token: CancellationToken := CancellationToken.None): owned IAsyncEnumerator<T>;
end
public interface IAsyncEnumerator<out T>: IAsyncDisposable
  readonly property Current: T;   method MoveNextAsync(): Task<bool>;
end
public interface IOwningEnumerable<T>: IDisposable where T: class
  method GetOwningEnumerator(): owned IOwningEnumerator<T>;
end
public interface IOwningEnumerator<T>: IDisposable where T: class
  method TakeCurrent(): owned T;   method MoveNext(): bool;
end
public interface IOwningAsyncEnumerable<T>: IAsyncDisposable where T: class
  method GetOwningAsyncEnumerator(token := CancellationToken.None): owned IOwningAsyncEnumerator<T>;
end
public interface IOwningAsyncEnumerator<T>: IAsyncDisposable where T: class
  method TakeCurrent(): owned T;   method MoveNextAsync(): Task<bool>;
end
```

**Buffers and spans.** `OwningBuffer<T>` is the storage primitive and the one owning-family type that admits **value**-typed `T`; it does not admit runtime-managed reference types (`string`, function types) or move-only value types.

```bezel
public class OwningBuffer<T>                    // class T: conforms to IEnumerable<T?>; value T: to nothing
  constructor(length: int);                     // class T: all slots null;  value T: all slots default(T)
  constructor(length: int, init: (int) -> owned T);      // value T: init is (int) -> T
  readonly property Length: int;                // immutable after construction
  property [i: int]: T? { get;  set(owned); }   // class T: set disposes previous, `move buf[i]` ADMITTED
  property [i: int]: T  { get;  set; }          // value T: `move buf[i]` NOT admitted
  method DangerousPointer(): Ptr<T>;            // value T only
  method AsSpan([start[, length]] | r: Range<int>): Span<T>;
  method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlySpan<T>;
end
public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)  // : IReadOnlySpan<T?> at class T
  property [i: int]: T? { get; set(owned); }    // value T: [i: int]: T { get; set; }
  property IsEmpty: bool;   method Slice(start, length) / Slice(start) / Slice(r: Range<int>): Span<T>;
  method IndexOf(value): int;  Contains(value): bool;  SequenceEqual(other): bool;  AsReadOnly(); Reverse();
  method Clear();                               // class T only: nulls every slot, disposing previous
  method MoveFrom(source: Span<T>);  MoveTo(destination: Span<T>);          // class T only
  method Fill(value: T);  CopyFrom(source: ReadOnlySpan<T>);  CopyTo(destination: Span<T>);  // value T only
end
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // read surface + Slice x3;  value T only: CopyTo(destination: Span<T>).  No ToArray() (v0.277)
end
public record ArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>
  property [i: int]: T { get; set; }            // the setter stores a BORROW; `move` NOT admitted
  property IsEmpty: bool;  Slice x3;  AsReadOnly(): ReadOnlyArraySpan<T>;  IndexOf; Contains; SequenceEqual;
  Fill(value: T);  CopyFrom(source: ReadOnlyArraySpan<T>);  CopyTo(destination: ArraySpan<T>);  Reverse();
end
public record ReadOnlyArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>
  // read surface + CopyTo(destination: ArraySpan<T>).  No ToArray() (neither span family has one)
end
public interface IReadOnlySpan<TElement>        // implemented by all five span types
  readonly property Length: int;   readonly property IsEmpty: bool;
  property [i: int]: TElement { get }
  method IndexOf(value: TElement): int;   Contains(value: TElement): bool;
  method SequenceEqual(other: IReadOnlySpan<TElement>): bool;
end                                             // `Slice` is deliberately NOT on the interface
```

`Span<T>`/`ReadOnlySpan<T>` are **borrow-bearing records** bounded by the buffer's lifetime; never an `async` method's parameter or result. **There is no implicit `Span<T>` → `ReadOnlySpan<T>` conversion** — call `.AsReadOnly()`. Spans do **not** implement `IEnumerable<T>`; `for` over a span is a direct indexed loop. `Span<T>` and `ArraySpan<T>` are **not interchangeable**. The `AsSpan`/`AsReadOnlySpan` factories and `Slice(r: Range<int>)` validate eagerly (`IndexOutOfRangeException`); direct constructors and `Slice(start[, length])` do not. **There is no writable-span interface.**

**Byte accessors** (the read tier is one `public extension IReadOnlySpan<byte>` — every byte span of both families is a receiver; the write tier is declared on `Span<byte>` and `ArraySpan<byte>`): `Read{Int16,UInt16,Int32,UInt32,Int64,UInt64,Float,Double}{LittleEndian,BigEndian}()` and the matching `TryRead…(out value: T): bool`; on the writable spans additionally `Write…{LittleEndian,BigEndian}(value: T)` and `TryWrite…(value: T): bool`. **Endianness is in the name — there is no platform-default form, no enum parameter, and no offset parameter; slice first.** A short span raises `IndexOutOfRangeException`; a false `TryWrite*` leaves the span unmodified.

**Bit tier** (v0.264): four borrow-bearing records — `BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)`, `BitWriter(Source: Span<byte>, …)` and their `ArrayBitReader`/`ArrayBitWriter` mirrors over the array spans — reached by `pure AsBitReader()` on the read-only spans and `AsBitReader()` (**not pure**) / `pure AsBitWriter()` on the writable ones. Each carries `pure IsEmpty`, `pure IsByteAligned`, `pure Slice` ×3, `pure AlignToByte()`, `AlignedTail()`, plus `ReadBitsBigEndianMsbFirst(count: int): ulong`, `ReadBitsLittleEndianLsbFirst(count: int): ulong`, `ReadBitMsbFirst()`/`ReadBitLsbFirst(): bool` and their `TryRead…(…, out value): bool` forms — the writer swapping reads for `WriteBits…(value: ulong, count: int)`/`WriteBit…(value: bool)`/`TryWrite…` and adding `AsReadOnly()`. Only **two** of the four bit-order × byte-order combinations exist; every multi-bit read returns `ulong` and every multi-bit write takes one (**no signed form**); a reader does not advance itself — slice to advance.

```bezel
public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>> where K: IComparable<K>
  constructor();   pure readonly property Count: int;
  property [key: K]: V;                          // get: KeyNotFoundException;  set: insert or replace
  method Add(key: K, value: V);                  // v0.282: ArgumentException if present
  method TryAdd(key: K, value: V): bool;         // v0.282: false, no mutation, if present
  method ContainsKey(key)/Contains(key): bool;   TryGetValue(key, out value: V?): bool;
  method GetOrNull(key: K): V?;   Remove(key: K): bool;   Clear();
  pure readonly property FirstKey: K;   LastKey: K;        // InvalidOperationException if empty
end
public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
  constructor();   pure readonly property Count: int;
  method Add(item: T);            // returns (); adding an equal element is a silent no-op
  method TryAdd(item: T): bool;  Contains(item: T): bool;  Remove(item: T): bool;  Clear();
  pure readonly property First: T;   Last: T;              // InvalidOperationException if empty
end
public class FrozenArray<T>: IEnumerable<T>      // deeply immutable, sendable; T: sendable value types and string
  public static method From(owned buffer: OwningBuffer<T>): owned FrozenArray<T>;   // VALUE T; O(1), steals
  public static method From(items: Array<T>): owned FrozenArray<T>;                 // copies; only form for string
  public readonly property Length: int;    public property [i: int]: T { get }
  public method ToArray(): owned Array<T>;    destructor;
end
public class Counter<K>: IEnumerable<DictionaryEntry<K, int>> where K: hashable
  constructor();
  method Increment(key: K, delta: int := 1);     // absent key is created at `delta`; negative admitted
  method CountOf(key: K): int;                   // 0 when absent — NOT pure
  method Contains(key: K): bool;                 // distinguishes present-at-zero from absent; enables `key in tally`. No ContainsKey on Counter
  method Remove(key: K): bool;   Clear();
  pure readonly property Count: int;             // distinct keys
  pure readonly property Total: int;             // sum of counts; may be negative
end
public record SlotHandle                         // compiler-intrinsic value; sendable, hashable; NO public constructor — default(SlotHandle) denotes no map and FAULTS like a foreign handle
  readonly property Index: int;   readonly property Generation: int;   // =, <>, Equals, GetHashCode, ToString provided
end
public class OwningSlotMap<T>: IEnumerable<T>, IOwningEnumerable<T> where T: class   // v0.307: the handle-and-table type
  constructor();  constructor(capacity: int);
  pure readonly property Count: int;
  method Insert(owned value: T): SlotHandle;     // takes ownership; mints the handle — a VALUE you may store anywhere
  method Contains(handle: SlotHandle): bool;     // `h in map`
  method Get(handle: SlotHandle): T;             // borrow; KeyNotFoundException if absent
  method GetOrNull(handle: SlotHandle): T?;      // borrow or null — a stale handle reads null, never a later occupant
  method TryGetValue(handle: SlotHandle, out value: T?): bool;         // the out is a BORROW
  method Remove(handle: SlotHandle): bool;       // DISPOSES; false if absent
  method PopValue(handle: SlotHandle): owned T;  TryPopValue(handle: SlotHandle, out owned value: T?): bool;
  method Handles(): owned Array<SlotHandle>;     // a SNAPSHOT (ascending Index) — safe to Remove while walking it
  method Drain(): IOwningEnumerable<T>;   method Clear();   // Clear/Dispose dispose in DESCENDING Index
end
```

The sorted collections take **no comparer parameter** — ordering is `K`/`T`'s own `IComparable` (the hash family's `IEqualityComparer` ctors are a recorded different case — rationale §17.14). `SortedDictionary` has `Add`/`TryAdd` (v0.282) but no `Keys`/`Values` (recorded: iterate entries). **There is no `OwningSortedDictionary`/`OwningSortedSet`, no `FrozenDictionary`/`FrozenSet`.** `Counter<K>` is insertion-ordered and a key decremented to zero is not removed.

### 5.5 `System.Collections.Linq` (auto-imported, v0.278)

```bezel
public extension IEnumerable<T>
  // Filtering / projection (LAZY)
  Where(predicate: (T) -> bool): IEnumerable<T>;
  Select<U>(selector: (T) -> U): IEnumerable<U>;
  SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>;
  // Quantifiers and element access (EAGER)
  Any(): bool;   Any(predicate): bool;   All(predicate): bool;
  Count(): int;  Count(predicate): int;
  First(): T;  First(predicate): T;   FirstOrNull(): T?;  FirstOrNull(predicate): T?;
  Last(): T;   Last(predicate): T;    LastOrNull(): T?;   LastOrNull(predicate): T?;
  Single(): T; Single(predicate): T;  SingleOrNull(): T?; SingleOrNull(predicate): T?;
  ElementAt(index: int): T;           ElementAtOrNull(index: int): T?;       // NO predicate form
  // Aggregation (EAGER)
  Sum<U>(selector: (T) -> U): owned U            where U: INumber<U>;
  Min<U>(selector: (T) -> U): U                  where U: IComparable<U>;
  Max<U>(selector: (T) -> U): U                  where U: IComparable<U>;
  Average(selector: (T) -> int|long|uint|ulong|double): double;
  Average(selector: (T) -> float): float;        Average(selector: (T) -> decimal): decimal;
  Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum): TAccum;
  // Ordering (LAZY; the OrderBy family is STABLE)
  OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>            where TKey: IComparable<TKey>;
  OrderByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>  where TKey: IComparable<TKey>;
  Reverse(): IEnumerable<T>;
  // (the hashable-requiring operators are a separate constrained block, below)
  // Slicing (LAZY; counts CLAMP)
  Take(count: int); TakeWhile(predicate); Skip(count: int); SkipWhile(predicate): IEnumerable<T>;
  // Combination and grouping (LAZY)
  Concat(other: IEnumerable<T>): IEnumerable<T>;
  Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;   // ends when either ends
  GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>>   where TKey: hashable;
  // Materialization (EAGER)
  ToArray(): owned Array<T>;
  ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T>         where TKey: hashable;
  ToDictionary<TKey, TValue>(key: (T) -> TKey, value: (T) -> TValue):
      owned Dictionary<TKey, TValue>                                       where TKey: hashable;
end
// The six hashable-element operators are a SECOND extension block (v0.277). §26.6 merges
// the two, so call sites are unchanged -- but write `where T: hashable` if you declare
// your own, and expect a non-hashable T to be rejected here rather than inside Set<T>.
public extension IEnumerable<T> where T: hashable
  Contains(value: T): bool;
  Distinct(): IEnumerable<T>;
  Union(other: IEnumerable<T>): IEnumerable<T>;
  Intersect(other: IEnumerable<T>): IEnumerable<T>;
  Except(other: IEnumerable<T>): IEnumerable<T>;
  ToSet(): owned Set<T>;
end
public interface IGrouping<TKey, T>: IEnumerable<T>   readonly property Key: TKey;   end
public interface IOrderedEnumerable<T>: IEnumerable<T>  end
public extension IOrderedEnumerable<T>
  ThenBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>            where TKey: IComparable<TKey>;
  ThenByDescending<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>  where TKey: IComparable<TKey>;
end
public extension IEnumerable<T> where T: IComparable<T>   Min(): T; Max(): T; MinOrNull(): T?; MaxOrNull(): T?;  end
public extension IEnumerable<T> where T: INumber<T>       Sum(): owned T;  end
public extension IEnumerable<int|long|uint|ulong|double>  Average(): double;  end
public extension IEnumerable<float>    Average(): float;    end
public extension IEnumerable<decimal>  Average(): decimal;  end
```

Empty source: `Sum` → the zero of its result type; `Min`/`Max`/`Average` → `InvalidOperationException`; `Aggregate` → `seed`; `First`/`Last`/`Single` → `InvalidOperationException`; `ElementAt` → `IndexOutOfRangeException`; `*OrNull` → `null`. **No `MinOrNull`/`MaxOrNull` selector overloads** — compose `Select(sel).MinOrNull()`. `ToDictionary` raises `ArgumentException` on a duplicate key. Lazy operators **retain their callbacks** until enumeration. `Sum` returns the element's own type, overflow-checked in it (`bytes.Sum()` past 255 raises `OverflowException` — widen in the selector).

**At class element types**, `First`/`Last`/`Single`/`ElementAt` (and their `*OrNull` forms), `Min()`/`Max()`, `MinOrNull()`/`MaxOrNull()` and `Min<U>`/`Max<U>(selector)` return **borrows sourced from the receiver** — no disposal obligation, valid only while the element's backing lives; `Aggregate` additionally carries `seed` as a source. **`ToArray()`/`ToSet()`/`ToDictionary(…)` at a class element type return BORROWING collections** — a new container, never a new owner.

### 5.6 `System.Threading`

```bezel
public record CancellationToken                       // auto-imported; borrow-bearing
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();   // raises OperationCanceledException when
                                                  //   cancellation was requested; returns with no
                                                  //   effect otherwise. None never raises.
  public method Register(scoped callback: () -> ()): owned Subscription;
end
public class CancellationTokenSource                   // NOT auto — needs the import
  public constructor();
  public static method WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource;
  public static method CreateLinked(a: CancellationToken, b: CancellationToken): owned CancellationTokenSource;
  public static method CreateLinkedAll(parents: Array<CancellationToken>): owned CancellationTokenSource;
  public readonly property Token: CancellationToken;   IsCancellationRequested: bool;
  public method Cancel();   public method CancelAfter(delay: TimeSpan);
  destructor;                                          // cancel-on-dispose
end
public class Mutex
  constructor();
  method Acquire(): owned Lock;                        // BLOCKS: compile error inside an async body
                                                       // NON-REENTRANT: a holder re-acquiring waits
                                                       //   on itself, in every acquisition form
  method TryAcquire(timeout: TimeSpan): owned Lock?;
  async method AcquireAsync(token := CancellationToken.None): OwningTask<Lock>;
  async method TryAcquireAsync(timeout: TimeSpan, token := CancellationToken.None): OwningTask<Lock?>;
end
public class Lock  destructor;  end                    // no public ctor; NOT sendable
public class Semaphore
  constructor(initialCount: int);   constructor(initialCount: int, maxCount: int);
  //   maxCount is a construction-time CEILING only (v0.302): ArgumentException when
  //   maxCount < 1 or initialCount > maxCount; nothing later can exceed it, because a
  //   permit returns only by disposing a token that an acquisition produced
  method Acquire(): owned SemaphoreToken;   TryAcquire(timeout): owned SemaphoreToken?;
  //   counts PERMITS: initialCount at construction, one consumed per acquisition, returned by
  //   disposing the token; a holder re-acquiring consumes a second permit (not a deadlock)
  async method AcquireAsync(token := ...): OwningTask<SemaphoreToken>;
  async method TryAcquireAsync(timeout, token := ...): OwningTask<SemaphoreToken?>;
end
public class Atomic<T> where T: value            // T in {bool char byte sbyte short ushort int uint
  constructor(initial: T);   property Value: T { get; set; }      //     long ulong IntPtr UIntPtr}
  method Exchange(value: T): T;   method CompareExchange(expected: T, replacement: T): T;
end
public class AtomicInteger<T> where T: value     // the primitive INTEGER types only
  constructor(initial: T);   property Value: T { get; set; }
  method Exchange(value: T): T;   CompareExchange(expected, replacement): T;
  method Increment(): T;   Decrement(): T;   Add(delta: T): T;    // POST-operation value; OverflowException
end
// Events are a PATTERN, not a member kind: an `owned readonly` source field + a property exposing the
// interface.  IEvent<out T> / IAsyncEvent<out T> / ISharedEvent<out T> declare
//   method Subscribe(scoped handler: (T) -> () | (T) -> Task): owned Subscription;
// implemented by EventSource<T> / AsyncEventSource<T> (RaiseAsync) / SharedEventSource<T> (capture-free
// handlers only); each source adds Raise(payload: T), Add(scoped handler), Remove(handler), destructor.
public class TaskCompletionSource<T>                   // NON-class T; class T is a compile error
  constructor();   readonly property Task: Task<T>;    // the property is a BORROW of the source's storage
  method SetResult(result: T);   SetException(owned exception: Exception);   SetCancelled();
  method TrySetResult(result: T): bool;  TrySetException(owned ...): bool;  TrySetCancelled(): bool;
end                     // + OwningTaskCompletionSource<T> where T: class (SetResult(owned result: T),
                        //   Task: OwningTask<T>) and the void-completion TaskCompletionSource
public class Task
  static method FromResult<T>(value: T): Task<T>;                        // NON-class T
  static method FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class;
  static method FromException<T>(owned exception: Exception): Task<T>;   FromCancelled<T>(): Task<T>;
  static method FromException(owned exception: Exception): Task;         FromCancelled(): Task;
  static method Delay(milliseconds: int, token: CancellationToken := CancellationToken.None): Task;   Delay(duration: TimeSpan, token := CancellationToken.None): Task;
  static method WhenAll(tasks: Array<Task>): Task;
  static method WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>;                        // non-class T
  static method WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>): OwningTask<OwningArray<T>> where T: class;
  static method WhenAny(tasks: Array<Task>): Task<int>;      // index of the first to SETTLE; never re-raises
  static method WhenEach(tasks: Array<Task>): IAsyncEnumerable<Task>;
  static method WhenEach<T>(tasks: Array<Task<T>>): IAsyncEnumerable<Task<T>>;
  static method WhenEach<T>(owned tasks: OwningArray<OwningTask<T>>): IOwningAsyncEnumerable<OwningTask<T>>
                                                                                          where T: class;
  static method FireAndForget(owned task: Task);
end
public class TaskScope: IAsyncDisposable                 // `await using` ONLY; not sendable
  constructor();
  readonly property Token: CancellationToken;
  method CancelAfter(delay: TimeSpan);   method Cancel();
  method Spawn(scoped body: () -> Task);                 // InvalidOperationException once closed
  async method Join(): Task;                             // MANDATORY on the normal path; first-fault re-raise
end
public class Channel<T> where T: sendable                // the RECEIVING end; single-consumer, MPSC
  constructor();                     // unbounded
  constructor(capacity: int);        // bounded, capacity >= 1 else ArgumentException
  readonly property Count: int;   IsClosed: bool;
  method NewSender(): owned ChannelSender<T>;   method Close();     // idempotent graceful close
  method Receive(): OwningTask<T>;                     // value/string T: Task<T>
  method TryReceive(out owned item: T?): bool;
  method ReceiveAll(): IOwningAsyncEnumerable<T>;      // value/string T: IAsyncEnumerable<T>
  destructor;
end
public class ChannelSender<T> where T: sendable
  method Send(owned item: T);                          // NEVER blocks; full bounded -> ChannelFullException
  async method SendAsync(owned item: T): Task;         // awaits capacity
  destructor;                                          // disposing the LAST sender closes the channel
end
public class Locked<T> where T: class                  // T must additionally be a SENDABLE class
  constructor(owned value: T);                         // the lock OWNS the interior
  method Lock(): owned LockGuard<T>;   TryLock(timeout: TimeSpan): owned LockGuard<T>?;
  async method LockAsync(token := ...): OwningTask<LockGuard<T>>;
  async method TryLockAsync(timeout, token := ...): OwningTask<LockGuard<T>?>;
end
public class LockGuard<T>   public readonly property Value: T;   end     // borrow; source = this guard
public class RwLocked<T> where T: class                // Read/TryRead/ReadAsync/TryReadAsync +
  constructor(owned value: T);                         //   Write/TryWrite/WriteAsync/TryWriteAsync
end                                                    //   yielding ReadGuard<T> / WriteGuard<T>
public static class Domain            // 18 Spawn rows: {no arg, `owned arg: A`, `arg: A`} x {no result,
  Spawn(entry: () -> ()): owned Task;   //   `owned R` class result, `R` value result} x {sync, async entry}
  Spawn<A>(entry: (owned A) -> (), owned arg: A): owned Task    where A: sendable, class;
  Spawn<A>(entry: (A) -> (), arg: A): owned Task                where A: sendable, notnull;
  Spawn<R>(entry: () -> owned R): owned OwningTask<R>           where R: sendable, class;
  Spawn<R>(entry: () -> R): owned Task<R>                       where R: sendable, notnull;
end                                   // the entry MUST be capture-free; EXACTLY ONE argument
public static class Parallel          // callbacks are SPAWN POSITIONS (capture-free)
  public static readonly property DefaultDegree: int;    // = Environment.ProcessorCount; degree := 0 means it
  Map<A, R>(items: Array<A>, body: (A) -> R, degree: int := 0): owned Task<Array<R>> where A, R: notnull;
  Reduce<A, R>(items: Array<A>, seed: R, selector: (A) -> R, combiner: (R, R) -> R,
               degree: int := 0): owned Task<R>                                      where A, R: notnull;
  ForEach<A>(items: Array<A>, body: (A) -> (), degree: int := 0): owned Task          where A: notnull;
  Map<A, R>(owned items: OwningArray<A>, body: (owned A) -> owned R,
            degree := 0): owned OwningTask<OwningArray<R>>                            where A, R: class;
  ForEach<A>(owned items: OwningArray<A>, body: (owned A) -> (), degree := 0): owned Task where A: class;
  // + the same three over Shared<FrozenArray<A>> (zero-copy)
end
```

`Acquire`/`TryAcquire` **block** and are a compile error inside `async` bodies — use the `Async` forms. `using var cts := CancellationTokenSource.WithCancelAfter(delay);` **is** a cancellation scope; an unbound `WithCancelAfter` result cancels at the end of the statement. A *published* source (behind `Shared<…>`, in a `static readonly` field, or linked to a published parent) is **signal-only** — `Register` on it raises `InvalidOperationException`. `TimeoutException` is **not raised by any stdlib operation** — the library's timeouts return `null`. **`FireAndForget` is the only supported way to discard a `Task`**; the other legal form is `using var t := DoAsync();`. `Channel<T>` has **no `TrySend`, no `Receive(token)`, no select, no receiver cloning, no capacity-0 rendezvous**, and admits at most one consumption in progress. `Parallel`'s `combiner` must be associative, `seed` is incorporated once per worker, results come back in input order, and the first fault surfaces as `DomainFaultedException`; there are **no async bodies, no class-element `Reduce`, no `CancellationToken` parameter**.

### 5.7 `System.IO`

```bezel
public abstract class Stream
  abstract readonly property CanRead, CanWrite, CanSeek: bool;
  abstract method Read(buffer: Array<byte>, offset: int, count: int): int;
  abstract method Write(buffer: Array<byte>, offset: int, count: int);
  method Read(buffer: Span<byte>): int;                 // fixed base implementation
  method Write(buffer: ReadOnlySpan<byte>);
  abstract method Flush();   abstract method Seek(offset: long, origin: SeekOrigin): long;
  abstract readonly property Length: long;   abstract property Position: long;
  async virtual method ReadAsync(buffer: Array<byte>, offset: int, count: int, token := ...): Task<int>;
  async virtual method WriteAsync(buffer: Array<byte>, offset: int, count: int, token := ...): Task;
  method ReadAll(): owned Array<byte>;
  async method ReadAllAsync(token := CancellationToken.None): OwningTask<Array<byte>>;
end
public enum SeekOrigin: byte  Begin, Current, End;  end
public class FileStream: Stream
  constructor(path: string, mode: FileMode, access: FileAccess := FileAccess.ReadWrite);
  public readonly property Path: string;   destructor;  // flushes + closes
end
public enum FileMode: byte    CreateNew, Create, Open, OpenOrCreate, Truncate, Append;  end
public enum FileAccess: byte  Read, Write, ReadWrite;  end
public static class File
  ReadAllText(path: string): string;              WriteAllText(path: string, contents: string);
  ReadAllBytes(path: string): owned Array<byte>;  WriteAllBytes(path: string, contents: Array<byte>);
  WriteAllBytes(path: string, contents: ReadOnlySpan<byte>);
  ReadAllLines(path: string): owned Array<string>;  WriteAllLines(path: string, lines: IEnumerable<string>);
  async ReadAllTextAsync(path: string, token := ...): Task<string>;
  async WriteAllTextAsync(path: string, contents: string, token := ...): Task;
  Exists(path: string): bool;   Delete(path: string);
  Copy(source: string, destination: string, overwrite: bool);   Move(source: string, destination: string);
end
public abstract class TextReader          // Read(): int (next char, -1 at EOF); ReadLine(): string? (null
  method ReadToEnd(): string;             //   at EOF); ReadToEnd(): string; destructor
end
public abstract class TextWriter          // Write(value: string|char|Object); WriteLine() and its string,
  abstract method Flush();                //   Object and per-primitive overloads; Flush(); destructor
end
public static class Directory
  Exists(path: string): bool;   CreateDirectory(path: string);
  Delete(path: string, recursive: bool := false);   // IOException if non-empty and not recursive
  GetFiles(path: string): owned Array<string>;   GetDirectories(path: string): owned Array<string>;
end
public static class Path                  // exactly six members
  Combine(a: string, b: string): string;   GetFileName(path): string;   GetExtension(path): string;
  GetFileNameWithoutExtension(path): string;   GetDirectoryName(path): string;   GetFullPath(path): string;
end
```

`IOException` ← `FileNotFoundException`, `DirectoryNotFoundException`, `EndOfStreamException`, `PathTooLongException`. `Stream.Read` signals end-of-stream with a **`0` return, never `EndOfStreamException`**, and may return **fewer than `count`** — a short read is not EOF. `File`'s text methods are UTF-8, strict-fault, no BOM. The **only** concrete `TextReader`/`TextWriter` instances are what `Console.In`/`Out`/`Error` return.

### 5.8 `System.Diagnostics`

`Stopwatch` — `constructor()`, `static method StartNew(): owned Stopwatch`; `IsRunning: bool`, `Elapsed: TimeSpan`, `ElapsedMilliseconds: long`; `Start()`, `Stop()`, `Reset()` — **no `Restart`**: write `sw.Reset(); sw.Start();`. `Debug` — `Assert(condition: bool, message: string := "")` (an empty message reports as no message), `WriteLine(message: string)`. **`Debug.Assert` terminates the process; it never throws**, and calls may be elided in release builds.

### 5.9 Primitives

The integer types (`Int32`, `Int64`, `Byte`, `SByte`, `Int16`, `UInt16`, `UInt32`, `UInt64`) carry: `static const MinValue/MaxValue`; `static pure Parse(s: string)`, `static TryParse(s: string, out result: T): bool`, `static pure ParseOrNull(s: string): T?`, `static pure Parse(s: string, radix: int)` (radix ∈ {2,8,10,16}), `static TryParse(s: string, radix: int, out result: T): bool`, `static pure Parse(s: ReadOnlyStringSpan)`, `static TryParse(s: ReadOnlyStringSpan, out result: T): bool`; `pure ToString()`, `pure ToString(format: string)`, `pure ToString(radix: int)`, `pure CompareTo(other)`. **`ParseOrNull` has no radix or span duals.** No whitespace tolerance anywhere. **Radix digits** are the first *r* of `0`–`9` then `A`–`Z`; `Parse(s, radix)` accepts **either case** (`int.Parse("ff", 16)` = `int.Parse("FF", 16)` = `255`), and `ToString(radix)` emits **lower case**, sign-and-magnitude — so `T.Parse(n.ToString(r), r) = n`. Input grammar: optional `+`/`-`, then digits; no `0x`, no separators, no whitespace (`FormatException`), out of range `OverflowException`; a radix outside `{2,8,10,16}` is `ArgumentException` from both `Parse` and `ToString`.

`Double`/`Float`: `MinValue MaxValue Epsilon PositiveInfinity NegativeInfinity NaN`; `static pure IsNaN/IsInfinity/IsPositiveInfinity/IsNegativeInfinity/IsFinite`; `Parse`/`TryParse` (string and `ReadOnlyStringSpan`) and `ParseOrNull` (string); `Parse` rounds to nearest-even like a literal and raises `OverflowException` exactly where the same digits as a literal of that type are out of range (`double.Parse("1e999")`, `float.Parse("3.5e38")`; `TryParse` → `false`, `ParseOrNull` → `null`), never yielding `±Infinity` for a finite numeral, while underflow is in range (`"1e-400"` → `0`); the five `Is*` report the operand's IEEE-754 class exactly (`IsFinite` = neither NaN nor infinity; subnormals and both zeros are finite); `static pure IeeeEquals(a,b)`, `static pure BitwiseEquals(a,b)`; `pure ToString([format])`, `pure CompareTo`. `Epsilon` is the smallest positive subnormal, not machine epsilon.
`Decimal`: `MinValue` = `-79228162514264337593543950335`, `MaxValue` = `79228162514264337593543950335`, `Zero One`; the no-format `ToString()` (and `G`) is the exact shortest form — no exponent, no grouping, **no trailing zeros** (`1.50m` renders `1.5`, `10m` renders `10`), scale not being part of the value; `Parse`/`TryParse`/`ParseOrNull`; `pure ToString([format])`, `pure CompareTo`, `pure Round(digits: int)` (banker's, 0..28).
`Boolean`: `static pure Parse(s)` ("true"/"false", case-insensitive), `static TryParse(s, out result: bool)`, `static pure ParseOrNull(s): bool?`, `pure ToString()` → `"True"`/`"False"`.
`Char`: `MinValue MaxValue`; `static pure IsDigit/IsLetter/IsLetterOrDigit/IsWhiteSpace/IsUpper/IsLower/IsControl/IsPunctuation(c)` — Unicode-category tests (`IsDigit` = category `Nd`, **not** the `0`–`9` the parses read; `IsWhiteSpace` = the `White_Space` property), total, never raising; `ToUpper`/`ToLower` are the **simple** Unicode case mappings; `static pure ToUpper/ToLower(c): char`; `Parse`/`TryParse`/`ParseOrNull`; `pure ToString()`, `pure CompareTo`.

```bezel
public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>
  pure readonly property Length: int;   IsEmpty: bool;   IsAscii: bool;
  pure readonly property [i: int]: char;      pure readonly property [r: Range<int>]: string;
  static pure From(value: char): string;   static pure From(buffer: Array<char>, start: int, length: int): string;
  static From(value: CString|WString): string;   static FromCStringPointer/FromWStringPointer(p: IntPtr): string;
  static pure Repeat(c: char, count: int): string;    static pure Repeat(unit: string, count: int): string;
  static Join(separator: string, items: IEnumerable<string>): string;
  static pure IsNullOrWhiteSpace(value: string?): bool;    // true for null, for "", and for all-whitespace
                                                           // no IsNullOrEmpty — write `s = null or s.IsEmpty` (or `s?.IsEmpty ?? true`)
  pure Contains(string|char): bool;  pure StartsWith(string): bool;  pure EndsWith(string): bool;
  pure IndexOf(char|string, startIndex: int := 0): int;   pure LastIndexOf(char|string): int;
  pure Substring(startIndex[, length]): string;
  pure AsSpan([start[, length]] | r: Range<int>): ReadOnlyStringSpan;
  pure Trim() / Trim(chars: IEnumerable<char>) / TrimStart() / TrimEnd() / ToUpper() / ToLower(): string;
  pure Replace(oldChar: char, newChar: char) / Replace(oldValue: string, newValue: string): string;
  pure PadLeft/PadRight/PadCenter(totalWidth: int, paddingChar: char := ' '): string;
  pure Insert(startIndex: int, value: string): string;   pure Remove(startIndex[, length]): string;
  method Split(separator: char): owned Array<string>;
  method Split(separator: string): owned Array<string>;
  method Split(separators: Array<char>): owned Array<string>;
  pure CompareTo(other: string): int;   pure CompareTo(other: string, ignoreCase: bool): int;   // no static string.Compare — `a.CompareTo(b, ignoreCase := true)`
  pure Equals(other: string?[, ignoreCase: bool]): bool;
  pure override Equals(other: Object?): bool;   pure override GetHashCode(): int;   pure override ToString(): string;
  method ToCharArray(): owned Array<char>;      method GetEnumerator(): owned IEnumerator<char>;
  static pure operator +(left: string, right: string): string;
end
public record Range<T>: IEnumerable<T>            // compiler-intrinsic; T = the integer types and char
  public readonly property Start: T?;   End: T?;   Step: long;   IsEndInclusive: bool;
  public pure readonly property IsOpenEnded: bool;      public pure readonly property IsEmpty: bool;
  public pure method Contains(value: T): bool;
  public method GetEnumerator(): owned IEnumerator<T>;      // NOT pure and cannot be
end
```

All string comparison is **ordinal** (code-point), `ignoreCase` included; `Length` and `s[i]` are O(1) over code points. `Split` keeps empty segments — **there is no remove-empty option**; filter with `.Where(lambda(w) => w.Length > 0)`. `Range<T>.Contains` being `pure` is what makes `requires(port in 0..65535)` legal — `in` invokes `Contains` (§17.5). `GetEnumerator` on an open-ended range raises `InvalidOperationException`. Every enum `E` with underlying `U` carries a compiler-provided `public static method FromUnderlyingOrNull(value: U): E?` (total); `E(n)` is the throwing tier.

### 5.10 FFI

The carriers are `IntPtr`/`UIntPtr` (records: `static readonly property Zero`, `Equals`, `GetHashCode`, `=`, `<>`; opaque, never dereferenced), `CString` (`static From(s: string): owned CString`, `static FromBytes(bytes: Array<byte>): owned CString`, `Length`, `Pointer: IntPtr`, `ToBytes(): owned Array<byte>`, `destructor`), `WString` (`static From`, `Length`, `Pointer`, `destructor`), `RawBytes` (`static From(buffer: Array<byte>)`, `static FromForeign(p: IntPtr, length: int, freeOnDispose: bool := false)`, `Length`, `Pointer`, `CopyTo(destination: Array<byte>, destOffset: int)`, `destructor`) — none has a public constructor — plus `System.Interop`'s `Ptr<T>` and `Interop` (`LastErrno()`, `CodeAddress<F>(callback)`). For the `extern` declaration syntax and the pointee gating, read spec §32 and stdlib §10.

### 5.11 What the standard library does **not** have

Every absence below was verified by a full-document search. Reaching for one of these is the failure this document exists to prevent.

**I/O.** `MemoryStream` (→ `OwningBuffer<byte>` + spans; for serializers, `JsonSerializer.ToBytes()`); `File.Open`/`OpenRead`/`OpenWrite`/`OpenText`/`AppendAllText`/`AppendAllLines` (→ `new FileStream(path, mode, access := FileAccess.ReadWrite)`, `FileMode.Append`); `File.ReadAllBytesAsync`/`ReadAllLinesAsync`/`WriteAllBytesAsync` (the async `File` surface is **text-only** → `FileStream.ReadAllAsync`/`ReadAsync`/`WriteAsync`); `StreamReader`/`StreamWriter`/`BufferedStream`; async `TextReader`/`TextWriter`; async **span** overloads on `Stream`; `Directory.Move`/`EnumerateFiles`/globbing; `Path.ChangeExtension`/`HasExtension`/`GetTempPath`/`DirectorySeparatorChar`; `Console.Read()`.

**Query operators.** `ToList`, `ToLookup`, `Join`, `GroupJoin`, `Cast<T>`, `AsEnumerable`, `DefaultIfEmpty`, `Prepend`, `Append`, `Chunk`, `DistinctBy`, `UnionBy`/`IntersectBy`/`ExceptBy`, `MinBy`, `MaxBy`, `Order()`/`OrderDescending()`, `TakeLast`, `SkipLast`, `LongCount`, `Enumerable.Empty`/`Range`/`Repeat` (**there is no `Enumerable` class at all**), `SequenceEqual` over `IEnumerable<T>` (it exists only on the span types), `SelectMany` with a result selector, `Zip` without a combiner, `Where`/`Select` with an index, and an instance `ForEach(action)` on any sequence (`Array<T>` and `string` included — write the `for` loop). Replacements: `ToList` → `ToArray()`; `MinBy(f)` → `OrderBy(f).First()`; `Enumerable.Range` → the language's `1..n`, which is `IEnumerable<T>` (`(1..100).Sum()` works); `Append`/`Prepend` → `Concat` with a one-element `Array<T>`.

**Collections.** `List<T>` (reserved name), `HashSet<T>` (→ `Set<T>`), `SortedList`, `PriorityQueue`, `Deque`, `ConcurrentDictionary`/`ConcurrentQueue`/`ConcurrentBag` (→ `Locked<T>` over a sendable collection, or `Channel<T>`), `ImmutableArray`/`ImmutableList`/`ImmutableDictionary` (→ `FrozenArray<T>`), `SlotMap`/`Arena`/`GenerationalArena`/`Pool` as plain names and any borrowing slot map (→ `OwningSlotMap<T>` with `SlotHandle` keys, v0.307 — the only form; a handle from another map faults), `ReadOnlyDictionary`, `BitArray`, `Lookup`, `KeyValuePair` (→ `DictionaryEntry<K,V>`), `Tuple`/`ValueTuple` (**Bezel has no tuples** — use a record), `Nullable<T>` (→ `T?`), `IComparer<T>` (only `IEqualityComparer<T>` exists), `StringComparison`/`StringComparer`. Absent *members* on types that do exist: `Array<T>` has no `RemoveAll`, `FindIndex`, `FindAll`, `Find`, `Exists`, `BinarySearch`, `CopyTo`, `TrueForAll`, `GetRange`, `InsertRange`, `RemoveRange`, `ForEach`; `Dictionary` has no `GetValueOrDefault` (→ `GetOrNull`) — it *does* have `TryAdd` since v0.282; `Math` has no transcendental block since v0.284 (`Sin`/`Cos`/`Tan`/`Asin`/`Acos`/`Atan`/`Atan2`/`Sinh`/`Cosh`/`Tanh`/`Exp`/`Log`/`Log10`/`Log2`/`E` removed; `Sqrt`/`Pow`/rounding stay) and no `Hypot`/`Cbrt`/`PI` (never documented) — the compiler rejects every one of these with a removed-at-v0.284 diagnostic (#3559); `IEnumerable<T>` has no `OfType<U>` since v0.284 (#3559, rejected likewise); `Console` has no `Clear` since v0.284 (rejected as an unknown `Console` static, #3572); `OwningArray<T>` declares no `ToArray`/`Sorted`/`Reversed`; `OwningLinkedList<T>` has no source constructor; `Stack`/`Queue` have no `Add`/`Remove`/`IsReadOnly`; `Counter<K>` has no `ContainsKey` (→ `Contains(key)`, `key in tally`, v0.290); `ArraySpan<T>`/`ReadOnlyArraySpan<T>` have no concrete cross-mode `SequenceEqual` (the `IReadOnlySpan<T>` overload takes the other family's span directly, v0.290).

**Text.** `string.Format`/`String.Format` (**use interpolation**), `String.Empty` (use `""`), `string.Concat`, `Split` with a remove-empty option, `Regex`, `ToTitleCase`, `Normalize`, the `StringBuilder.Length` setter, the `C` format specifier, any culture-aware formatting, `string.IsNullOrEmpty` (→ `s = null or s.IsEmpty`, v0.290), the static `string.Compare(a, b[, ignoreCase])` (→ `a.CompareTo(b)`, `a.CompareTo(b, ignoreCase := true)`, v0.290).

**Concurrency.** `Thread`, `ThreadPool`, `Monitor`, a `lock` statement (→ `Mutex`/`Locked<T>`), `Interlocked` (→ `Atomic<T>`), `ManualResetEvent`/`AutoResetEvent`/`CountdownEvent`/`Barrier`, `Task.Run` (→ `Domain.Spawn`), `Task.CompletedTask` (→ `Task.FromResult`), `Task.Yield`, `ConfigureAwait`, `Task.Wait`/`.Result` inside async code, `AggregateException` (first fault wins; **no aggregation**), `Parallel.For`.

**Everything else.** `Enum.Parse`/`Enum.TryParse`/`Enum.GetName`/`Enum.GetNames`/`Enum.GetValues`/`Enum.IsDefined` (**there is no `Enum` class** — the members are per-enum statics, `E` being the enum's own name: `E.FromNameOrNull(s)` (declared identifier, exact) and `E.NameOf(v)` (v0.293), `E.FromUnderlyingOrNull(u)`; the lists are `typeof(E).EnumNames`/`EnumValues`, and `IsDefined` is `E.FromUnderlyingOrNull(u) <> null`), `Object.MemberwiseClone` and any clone facility, `ICloneable`, member reflection (**declined, not deferred**), attributes, `Activator`, `Lazy<T>`, `WeakReference` (→ `Weak<T>`), `Option<T>` (→ `T?`), `Result.Unwrap`/`UnwrapOrElse`, `Math.Round(decimal)` (→ `Decimal.Round`), `Math.Cbrt`/`Hypot`/`BigMul`, `Random.Shared`, cryptographic randomness, a `Guid` constructor, `Environment.StackTrace`/`CurrentDirectory`/`TickCount`, `Convert` from any non-`string` source, `Encoding.GetEncoding(name)`/`Encoding.Default`, `Encoding.GetString(bytes, start, length)` (→ `GetString(bytes.AsReadOnlySpan(start, length))`, v0.290), `Stopwatch.Restart()` (→ `Reset(); Start();`, v0.290), `Type.Implements(iface)` (→ `iface.IsAssignableFrom(t)`, v0.290), `Math.Sign(c: char)` (`Sign<T>` needs `INumber<T>` — `Math.Sign(int(c))`), `OutOfMemoryException`, `InnerException` on user exception types, multidimensional arrays, `defer`/`errdefer` (→ `ScopeGuard`).

### 5.12 The `pure` inventory

A `requires`/`ensures` clause may call **only pure members** (§22.16), so this is the contract-writing vocabulary. Two kinds qualify: members that **carry the `pure` marker** — written on every such signature in the listings above, so read them there — and members that are **pure by classification** (stored-slot reads), which carry no marker and permit none. `Range<T>`'s `Start`, `End`, `Step` and `IsEndInclusive` are the canonical second kind, which is why `requires(r.Step > 0)` compiles although no `pure` is written on `Step`.

What is **not** pure, where a writer expects it to be:

- `Math.Min`/`Max`/`Clamp` (bare-`T` borrows) and `Math.DivRem` (declares an `out`).
- Every `Contains`/`ContainsKey`/`CountOf`/`GetOrNull` — they invoke user `Equals`/`GetHashCode`/comparers — and every span, buffer and array **indexer** (`string`'s indexer *is* pure).
- Every `TryParse` and every other Try shape: they declare an `out`, so **the Try shapes are uniformly unavailable to contracts**. `Parse` and `ParseOrNull` are pure.
- `String.Join`, `Split` ×3, `ToCharArray`, `GetEnumerator`; `ReadOnlyStringSpan.CopyTo`; `Range<T>.GetEnumerator` (and it cannot be).
- In the bit tier: every read, every write (including `TryWrite*`), every `TryRead*`, `AlignedTail`, `AsReadOnly`, and the two *delegating* entry points `Span<byte>.AsBitReader`/`ArraySpan<byte>.AsBitReader` — the four *constructing* entry points are pure.
- `Exception(message, cause)` and `Exception.ToString()`. (**Every** §31.4 built-in exception's `()` and `(message)` constructors are `pure constructor`s — `FormatException`, `InvalidOperationException` and the rest, not only `Exception` — as are `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms. Only the `(message, cause)` chaining constructor is impure: its snapshot builds a fresh `Array`. This is what lets a `pure` body throw — §22.15's carve-out.)

The pure collection surface is small and worth knowing by heart: `IReadOnlyCollection<out T>.Count` (an interface slot — every conformer must be pure), `Count`/`Capacity` on `Array<T>`/`OwningArray<T>`, `Count` on the dictionaries, sets, `LinkedList`s and `Counter<K>` (plus `Counter.Total`, `SortedDictionary.FirstKey`/`LastKey`, `SortedSet.First`/`Last`, `LinkedList.First`/`Last`/`FirstNode`/`LastNode`), `StringBuilder.Capacity`, `OwningBuffer<T>.Length`, and `Peek()` on all four stack/queue types.

**Nothing in the query operators, threading, I/O, diagnostics, or the `Result` combinators carries `pure`.** `pure` is admitted on a getter (`pure get`) and **never on a setter — `pure set` does not parse** (§22.15).

---

## 6. Idioms

Each program below is traced against the documents; the illustrative user types are declared inline.

**A module with a `Main`.** The eight accepted signatures (§33.2) — anything else named `Main` is an ordinary method:

```bezel
module HelloWorld;

public class Program
  public static method Main(args: Array<string>)
    if args.Count > 0 then
      Console.WriteLine($"Hello, {args[0]}!");
    else
      Console.WriteLine("Hello, Bezel!");
    end;
  end Main;
end Program;
```

The eight are `Main()`, `Main(args: Array<string>)`, either with `: int`, and the four `async` forms of those returning `Task` or `Task<int>` — all `public static`.

**Reading a file.** `Array<T>` is trivial-dispose, so a plain `var` carries no must-use warning; `using var` is also accepted and states the owner explicitly.

```bezel
module Tools.WordCount;

import System.IO;

public class Program
  public static method Main(args: Array<string>): int
    var path := args[0];
    if not File.Exists(path) then
      Console.Error.WriteLine($"not found: {path}");
      return 1;
    end;
    var lines := File.ReadAllLines(path);        // owned Array<string>
    var words := 0;
    for var line in lines do
      words := words + line.Split(' ').Count;    // Split: owned Array<string>, empty segments kept
    end;
    Console.WriteLine($"{lines.Count} lines, {words} words");
    return 0;
  end Main;
end Program;
```

For a stream, `using var` is the workhorse and covers the exception path: `using var fs := new FileStream(path, FileMode.Open, FileAccess.Read);`, then `fs.ReadAll()` for an `owned Array<byte>`. Owned locals are disposed in reverse declaration order.

**Owning collections: build, iterate, drain.**

```bezel
public class Connection(host: string)
  public readonly property Host: string := host;
  public method SendShutdown()
    Console.WriteLine($"closing {Host}");
  end SendShutdown;
end Connection;

public static class Pooling
  public static method Connect(host: string): owned Connection     // static + class type => `owned` REQUIRED
    return new Connection(host);
  end Connect;

  public static method BuildPool(): owned OwningArray<Connection>
    var pool := new OwningArray<Connection>();
    pool.Add(Connect("a"));                      // fresh producer, unmarked; the slot takes ownership
    pool.Add(new Connection("b"));
    return pool;                                 // marker-free position: NOT `return move pool;`
  end BuildPool;

  public static method Shutdown()
    using var pool := BuildPool();               // the caller owns it; disposal cascades to elements
    for var c in pool do                         // borrowing pass: `c` is a borrow of the slot
      Console.WriteLine(c.Host);
    end;
    for var c in pool.Drain() do                 // owning pass: each element leaves the collection
      using var live := move c;                  // adopt on the first line — covers every exit edge
      live.SendShutdown();
    end;                                         // pool is now empty, still disposable
  end Shutdown;
end Pooling;
```

**The Try shape, and `out using var`.**

```bezel
public class ConnectionPool
  private owned _cache: OwningDictionary<string, Connection> := new OwningDictionary<string, Connection>();

  public method TryPopCached(name: string, out owned conn: Connection? when true): bool
    if not _cache.ContainsKey(name) then
      conn := null;                              // every out path must assign (throw paths are exempt)
      return false;
    end;
    conn := _cache.PopValue(name);               // takes ownership out of the cache
    return true;
  end TryPopCached;

  public method Drop(name: string)
    if TryPopCached(name, out using var c) then
      c.SendShutdown();
    end;                                         // c is disposed at the end of this block
  end Drop;
end ConnectionPool;
```

`out owned` in the signature ⇄ `out using var` at the call site (or `out var c`, which then binds an owned local you discharge yourself); `out using var` against a plain `out` is rejected. Without `when true` the caller would need `c!`: a `bool` guard alone does not narrow.

**`Result<T, E>` and an exhaustive `case`.** In a **declared-result position** — a `return` operand, or an expression body checked against a declared type — the bare case name resolves against that type (§35.3); everywhere else the qualified form `Result<int, string>.Ok(v)` is required.

```bezel
public class Ports
  public method ParsePort(text: string): Result<int, string>
    if not int.TryParse(text, out var value) then
      return Error($"not a number: {text}");
    end;
    if not (value in 1..65535) then
      return Error($"out of range: {value}");
    end;
    return Ok(value);
  end ParsePort;

  public method Report(text: string)
    case ParsePort(text) of                    // exhaustive: no `_` arm needed
      Ok(var p)    => Console.WriteLine($"listening on {p}"),
      Error(var m) => Console.Error.WriteLine($"bad port: {m}")
    end;
  end Report;
end Ports;
```

**A choice with an owned payload, matched by consuming.** A `case` over a *named* binding only peeks; consuming one is `case move outcome of …`, and a marked named slot takes just that payload. In an owning match a whole-value arm `var y => …` passes the value on only if the arm consumes `y` (`var z := y;`); otherwise the construct disposes it.

```bezel
public choice PollOutcome
  Ready(owned conn: Connection, attempts: int),
  Empty;
end PollOutcome;
```

```bezel
case move outcome of
  Ready(move conn = var c, attempts = var n) => do
    using var live := move c;
    Console.WriteLine($"connected after {n} tries");
  end,
  Empty => ()
end;
```

**A class that holds resources — usually with no destructor.** `owned` fields are disposed automatically in reverse declaration order; write a `destructor` only for a raw or foreign resource, one per owning wrapper.

```bezel
public class Customer(name: string)
  public readonly property Name: string := name;
end Customer;

public class Ledger(owner: Customer)
  private owned _entries: Array<string> := new Array<string>();
  private owned _cache: OwningArray<Connection>? := null;   // nullable owned field
  private _owner: Customer := owner;                        // back-reference — NOT owned
end Ledger;
```

**Async, and structured fan-out.**

```bezel
import System.Threading;

public class Refresher
  private static async method RefreshOneAsync(name: string, token: CancellationToken): Task
    await Task.Delay(TimeSpan.FromMilliseconds(10), token);
    Console.WriteLine($"refreshed {name}");
  end RefreshOneAsync;

  public async method RefreshBoth(primary: string, mirror: string): Task
    await using var scope := new TaskScope();    // `await using` only; a plain `using var` is an error
    scope.CancelAfter(TimeSpan.FromSeconds(30));
    scope.Spawn(async lambda() => do
      await RefreshOneAsync(primary, scope.Token);
    end);
    scope.Spawn(async lambda() => do
      await RefreshOneAsync(mirror, scope.Token);
    end);
    await scope.Join();                          // mandatory on the normal path; the single re-raise site
  end RefreshBoth;
end Refresher;
```

A `Task`-family call mints an owned frame, and `var text := await File.ReadAllTextAsync(p);` is the **scoped await-bind**: a fresh `var`/`const` whose initializer is exactly an await of an unbound producer pins the task to a hidden slot for the enclosing block (§30.5).

**Contracts, and a `pure` member.** Clauses sit between signature and body, take no terminator, and are followed by a blank line before the first statement.

```bezel
public class SampleWindow
  private owned _samples: Array<double> := new Array<double>();

  public method Push(value: double)
    _samples.Add(value);
  end Push;

  public method At(index: int): double
    requires(index >= 0)
    requires(index < _samples.Count)

    return _samples[index];
  end At;

  public static pure method Doubled(v: double): double => v * 2.0;
end SampleWindow;
```

`pure` is declared and verified, never inferred. A pure body may read mutable state and mutate its own locals, but may not store through `this`, call a non-`pure` member, or invoke a function value, and it declares no `ref`/`out`/`owned`/`scoped` parameter (§22.15).

**Tallying and querying.** `using var tally := new Counter<string>();` then `tally.Increment(w.ToLower());` beats a dictionary dance. A bound lazy query is affine — enumerate it once, or `ToArray()` it to read the results repeatedly: `var top := tally.OrderByDescending(lambda(e) => e.Value).Take(10).ToArray();` yields an `owned Array<DictionaryEntry<string, int>>` you may re-read.

**Strings.** `var sb := new StringBuilder();` (trivial-dispose: plain `var`, no warning), `sb.Append(w).Append(' ');`, `sb.ToString().Trim()`. Casts are call syntax — `double(hits)` — and interpolation carries alignment then format: `$"|{line,-20}| {pct,6:F1}%"`.

---

## 7. Looks right, is rejected

Each entry is **wrong**, then the rule, then **right**.

### 7.1 Syntax reflexes

```bezel
foreach var t in transactions do Process(t); end foreach;   // WRONG: no such keyword
var first := rows?[0];                                      // WRONG: there is no `?[` suffix
var xs: Array := [10, 20, 30];                              // WRONG: `Array` is not a type
Registry.n := Registry.n + 1;                               // WRONG inside `class Registry<T>` too: a bare generic name has arity 0 everywhere (§14.1) — `n := n + 1;` or `Registry<T>.n`
var ys := [1, 2, 3];                                        // WRONG: no target type for the literal
var sd: SortedDictionary<string, int> := ["a": 1];          // WRONG: the dictionary literal targets only Dictionary/OwningDictionary (§15.4)
var max := a > b ? a : b;                                   // WRONG: no `?:` operator
x := y++;   ++c;   if count++ > 3 then ... end;               // WRONG: `++` is a postfix STATEMENT
var g := (when s >= 90 => "A", _ => "F");                    // WRONG: no `when` construct (retired v0.289) — `(if s >= 90 then "A" else "F")`
when c1 => A(), _ => B() end when;                            // WRONG: same — `if c1 then A(); else B(); end;`
public static method operator ++(c: Counter): Counter => …;   // WRONG: `++`/`--` are not overloadable (v0.289) — declare `operator +`, write `c := c + 1;`
if count then ... end;                                        // WRONG: no truthiness
var (a, b) := point;                                        // WRONG: destructuring is pattern-position only
public method Log(params items: Array<string>);             // WRONG: no `params`
var c := new Config() with(Timeout := 30);                  // WRONG: `with` needs a RECORD source
var caller: (a: int, b: int) -> int := Sub;  caller(b := 2, a := 100);   // WRONG: a call through a FUNCTION VALUE admits no named argument (§27.1, v0.295)
var u := uint(-1);   var b2 := byte(300);   var c2 := char(0xD800);   // WRONG: a CONSTANT operand the conversion would reject is a COMPILE error (§5.2)
var d := ch - '0';   var nx := char(ch + 1);   ch *= 2;             // WRONG: `char` is not arithmetic (§3.5) — `int(ch) - int('0')`, `char(int(ch) + 1)`; only `ch += n;`, `ch -= n;`, `ch++;`, `ch--;` advance a code point (§6.6)
var c3 := char(65.5);                                         // WRONG: no float/double/decimal → char — char(int(x)) (§5.2)
```

> §21.4/A.6: the loop is `for … in … do … end;`. A.7's `PrimarySuffix` admits `.`, `?.`, `[…]`, `(…)`, `?(…)`, `!`, `with(…)` — and no `?[`. §14.5: class/record/interface type arguments are never inferred; §15.4: a collection literal is well-typed only where a target type is supplied. §20: the conditional is a parenthesized atom. §17.6, §17.2: `++`/`--` and assignments are statements. §4.1: conditions are `bool`. §19.3, §27.4: destructuring lives in patterns and lambda parameters. §22.14: pass an `Array<T>` and write the literal at the call site. §9.5: `with` on a class-typed source is the retired object-initializer reading. §5.2/§5.4: an explicit conversion whose operand is a **compile-time constant** the conversion would reject — `uint(-1)`, `byte(300)`, `char(0xD800)`, `int(1e30)`, an enum cast matching no declared constant — is a compile error **at the conversion site**, never a deferred `OverflowException`; the runtime fault is for **non-constant** operands, and unreachable code is not exempt (v0.300). A saturating conversion is unaffected: `float(1e300)` is `Infinity`, constant operand or not.

```bezel
for var t in transactions do Process(t); end;
var first: Row? := null;
if rows <> null then first := rows[0]; end;   // §7.4 narrows a LOCAL; `rows?.Count` also works
var xs: Array<int> := [10, 20, 30];       ProcessBuffer([1, 2, 3]);
var max := (if a > b then a else b);
c++;    count := count + 1;
if count <> 0 then ... end;
case point of Point(var a, var b) => Use(a, b) end;
public method Log(items: Array<string>);     Log(["a", "b", "c"]);
var c := new Config(timeout := 30, retries := 3);             // named constructor arguments
var caller: (a: int, b: int) -> int := Sub;  caller(100, 2);              // a function type's names are documentation only; the call is positional
```

Also absent, with the landed spelling beside it: `goto`/`break 2`/labels on non-loop statements → labeled loops (`scan: for … break scan;`); `let`/`val`/`readonly var` → `const x := e;`; `defer`/`errdefer` → `using var`, `finally`, `ScopeGuard`; `do…while`/`repeat…until` → `while` with a leading flag; local functions → a lambda bound to a local; attributes `[Obsolete]` → the closed clause vocabulary (`serialized`, `equality skip`, `when true`); `#if`/`#region` → runtime branching; `import M.{X}`/`import M.*`/`public import` → plain `import M;` (+ `as`); tuples, anonymous types, comprehensions, a value-form `(try E catch …)`, block expressions → a `record` (possibly method-local), the lazy operators, the `try` statement, and the binding atom `(var x := e; body)`; `x not in c` → `not (x in c)`; `not is` → `is not`; `default(SomeChoice)` → construct a case, or use `C?`; implicit/explicit conversion **operators** → named `To…`/`From…` methods; `operator ==` on a class → `=` always calls `Equals` (records, enums, and choices may overload); member hiding, `protected internal`, `default interface methods`, `ref` returns, class deconstruction — all rejected.

### 7.2 Ownership shapes that look fine

```bezel
method CloneBuffer(): Buffer                     // WRONG
  var fresh := new Buffer();
  return fresh;                                  // ERROR: a fresh owned value through a non-`owned` return
end CloneBuffer;

public static method Create(host: string): Connection    // ERROR at the DECLARATION
public static method Make<T>(): T where T: new() => new T();   // ERROR at the DECLARATION: empty source set
```

> §28.3 row 4: a plain class-typed **instance** return is a *borrow sourced from the receiver*; a **static** one names nobody responsible and is rejected where it is written. Prose like "returns a fresh instance" carries no weight. **Row 6 is the same rejection one step further out:** a bare type parameter with an **empty** source set — a static with no receiver and no parameter whose type mentions `T` — names nobody either, `where T: new()` notwithstanding. Spell `owned T`; it is admissible over every constraint domain (§28.1's per-instantiation erasure), and it is what the standard library's own generic producers spell (`Zero(): owned T`, `Parse(s): owned T`).

```bezel
method CloneBuffer(): owned Buffer
  var fresh := new Buffer();
  return fresh;
end CloneBuffer;

public static method Create(host: string): owned Connection
public static method Make<T>(): owned T where T: new() => new T();
```

```bezel
method Make(): owned Buffer
  using var fresh := new Buffer();
  return fresh;                    // ERROR: cannot move out of a `using var`
end Make;
```

> §28.10: `using var` is scope-pinned. The factory idiom is plain `var` — already exception-safe, because the unwinding pass disposes live owned values.

```bezel
method Make(): owned Buffer
  var fresh := new Buffer();
  return fresh;
end Make;
```

```bezel
var x: Buffer;
if useFresh then
  x := new Buffer();               // OWNED here
else
  x := registry.Buffer;            // BORROWED here
end;                               // ERROR at the merge
var s := dest ?? new FileStream(path, FileMode.Create);   // ERROR: same rejection as an expression
```

> §28.3 join rule: owned ⊓ borrowed is rejected. Equalize to both borrowed or both owned. Two *named* owned locals as unmarked `??` arms make a **borrow** merge; mark them to coalesce into one owned result.

```bezel
var target: Stream := dest ?? _fallback;                       // both borrows, both sources outlive
using var x := (if useFresh then new Buffer() else src.CloneBuffer());   // both owned
using var y := (if a <> null then move a else move b);         // marked arms
```

```bezel
for var conn in pool do
  conn.Dispose();                  // ERROR: only owners dispose
end;
method Close(s: Stream)  s.Dispose();  end Close;              // ERROR: `s` is borrowed
```

> §28.6: `Dispose()` needs a receiver you own. A borrowed parameter, `this`, a getter result, a `catch` binding, a pattern binding and an element borrow of *either* family are all rejected, with no exception.

```bezel
pool.Clear();                                                  // disposes every element through the owner
for var conn in pool.Drain() do using var c := move conn; ... end;
```

```bezel
var tasks := new Array<Task<string>>();
tasks.Add(File.ReadAllTextAsync(path));    // compiles, but WARNS: the fresh task has no holder
var conns: Array<Connection> := [new Connection("a")];   // ERROR: hard rejection at the literal site
_ := DoTelemetryAsync();                                 // ERROR: a Task may not be discarded
```

> §15.4's fresh-store lint on the borrowing family; the collection-**literal** form of the same shape is an error, not a warning. §17.13: `Task.FireAndForget(t)` (supervised) or `using var t := …;` (unsupervised) are the two legal not-awaiting spellings — *named*; the anonymous `using var _ := t;` is rejected like the discard.

```bezel
using var t1 := File.WriteAllTextAsync(pathA, report);   // named holders
using var t2 := File.WriteAllTextAsync(pathB, report);
var both: Array<Task> := [t1, t2];
await Task.WhenAll(both);
Task.FireAndForget(DoTelemetryAsync());
```

```bezel
public method Wrong(replace: bool): owned FileStream
  var old := move _resource;
  if replace then _resource := new FileStream(_path, FileMode.Open); end;
  return old;                      // ERROR: the field is unrestored on the false path
end Wrong;

private owned _lastError: Exception?;
try Risky(); catch
  var ex: Exception => _lastError := ex;    // ERROR: `ex` is a borrow whose lifetime is the arm
end;

method Header(): Span<byte>
  using var buf := new OwningBuffer<byte>(64);
  return buf.AsSpan();             // ERROR: a borrow-bearing record may not outlive its buffer
end Header;
```

> §28.8.1: every path from a field move to any exit must restore the field — and **build the replacement before taking**, so no throwable call sits on the unrestored edge. A caught exception is a borrow: copy the strings out. §9.7: spans may not be returned past their source, stored in class fields, or used as `async` parameters or results.

```bezel
public method Right(): owned FileStream
  var fresh := new FileStream(_path, FileMode.Open);   // build first — it may throw
  var old := move _resource;       // then take — nothing throwable follows
  _resource := fresh;              // restore
  return old;
end Right;
```

```bezel
var b := item.Handle();            // borrow of item
pool.Add(move item);               // ERROR: b is still live (the consume-site freeze)
b.Configure();
```

```bezel
pool.Add(move item);               // transfer first...
var b := pool[pool.Count - 1];     // ...then re-borrow from the new owner
b.Configure();
```

### 7.3 Value kinds

```bezel
public record Tally
  public property Hits: int;
  public method Bump()
    Hits := Hits + 1;              // ERROR: `this` is a BORROW of the receiver value
  end Bump;
end Tally;

public record Session(owned Log: FileStream);      // ERROR: records cannot carry disposal obligations

public record Version(Major: int)
  public virtual method Label(): string => $"v{Major}";   // ERROR: no `virtual`/`abstract`/`sealed override` on a record member (§9.2)
end Version;

public enum Level: ISerializable
  Low serialized "x", High serialized "x";                 // ERROR: two constants under one serialized name (§9.8)
end Level;
```

> §9.4: in every instance member body of a value kind, a store landing in the receiver's own storage is rejected — compound assignment and `++`/`--` included, through their desugarings. §28.5: copy-semantics holders cannot own; use a `class`. §9.2/§10.5/§35.2: a record, enum, or choice member — method, property, or indexer — admits no `virtual`, `abstract`, or `sealed override`, and `override` only on `Equals`/`GetHashCode`/`ToString`; value kinds are sealed and dispatch statically. §9.8: a serialized name (the rename, or the identifier where none is declared; compared ordinally) must be unique across a type's components, its enum constants, and its choice cases — a choice's payload fields are unique **within their own case**, two cases never sharing a wire object.

```bezel
public method Bumped(): Tally => this with(Hits := Hits + 1);   // return the updated value
```

### 7.4 Overloads and the `out` family

```bezel
method TryGet(k: string, out v: Conn?): bool;
method TryGet(k: string, out owned v: Conn?): bool;   // ERROR at the second declaration
pool.TryPopCached("p", out owned var c);   // ERROR: no `out owned var` (retired v0.289) — `out var c` adopts owned; `out using var c` scope-binds it
seen.Add(candidate);                     // ERROR when Add takes `owned`: write Add(move candidate)
r.Append(move new Widget());             // ERROR: a fresh producer announces itself
return move fresh;                       // ERROR: `return` under `owned` is a marker-free position
```

> §22.6: ordinary-parameter `owned` distinguishes overloads (under the erasure declaration guard — `where T: class` does **not** qualify, since it admits `string`); `owned` on an `out` parameter and `scoped` never do. The library's convention is **distinct names** for the borrow/take pair: `TryGetValue` vs `TryPopValue`, `TryPeek` vs `TryPop`/`TryDequeue`. §22.6/§14.5 (v0.296): a method-reference argument ranks at an overloaded callee as a lambda literal does — exact where its signature equals the (substituted) slot, converting where formation bridges (`Task<X>` → `Task`) — and a candidate whose function-typed slot has a different parameter count from the argument's is simply inapplicable, so an overload family keyed on handler arity resolves to the one member that fits.

```bezel
method TryGetCached(k: string, out v: Conn? when true): bool;        // borrow
method TryPopCached(k: string, out owned v: Conn? when true): bool;  // take
if not seen.Contains(candidate) then  seen.Add(move candidate);  end;
r.Append(new Widget());
return fresh;
```

### 7.5 Iterators, queries, liveness

```bezel
public static method Tail(lines: Array<string>, n: int): IEnumerable<string>
  if n <= 0 then return; end;              // ERROR (§29.5): no plain `return` in an iterator body
  ...
end Tail;

public method Items(): IEnumerable<Item>
  return _items;                           // ERROR: an unmarked mint-announcing return may not lend
end Items;

using var hot := readings.Where(lambda(r) => r.Celsius > 90.0);
var n := hot.Count();                      // spends the single enumeration right
for var r in hot do Plot(r); end;          // ERROR: already consumed

method Report(samples: IEnumerable<Sample>)
  var best: Sample? := null;
  for var s in samples do
    if best = null or s.Score > best!.Score then best := s; end;   // ERROR: `s` borrows the enumerator
  end;
end Report;
```

> §29.5: exit an iterator with `yield break;` (and `yield` is barred inside a `try` with a `catch`, and in a lambda). §28.3 row 2 callee side: a *stored* collection is re-exposed as a **property**, whose read is a borrow. §29.11: a lazy query is single-consumption. §29.2: over an interface-typed source the loop variable's source is conservatively the loop's own enumerator, so any escape is rejected — take a concrete collection type instead. §14.5/§15.4: a selector whose body is a collection literal (`xs.SelectMany(lambda(x) => [x, x + 10])`) is rejected — the literal has no standalone type and an `IEnumerable<U>` return position admits none; bind the literal to an explicitly typed local in a block body and return that local (the compiler's diagnostic names the shape).

```bezel
if n <= 0 then yield break; end;
public property Items: IEnumerable<Item> => _items;
using var hot := readings.Where(lambda(r) => r.Celsius > 90.0).ToArray();
method Report(samples: Array<Sample>)  ...  end Report;      // concrete: the array is the backing
```

### 7.6 Concurrency

```bezel
var cfg := LoadAsync().Result;                            // ERROR inside async code: blocks its own domain
using var j := Domain.Spawn(lambda() => Index(repo));     // ERROR: a spawn entry must be CAPTURE-FREE
private static _cache: Dictionary<string, int> := new Dictionary<string, int>();   // ERROR: mutable static
using var scope: TaskScope := new TaskScope();            // ERROR: TaskScope needs `await using`
using var g := mutex.Acquire();                           // ERROR inside an async body: it blocks
```

> §30.3: blocking waits are a compile error lexically inside `async` bodies. §30.14: a `Domain.Spawn` entry is a static method reference or a non-capturing lambda, and data enters only through the one explicit sendable argument. §22.12: a mutable static is an error outside four admitted forms (a synchronized handle; the `Task?`/`Task<T>?` lazy cache; a warned primitive/enum value static; the owning-inline-array slot pool).

```bezel
await using var scope := new TaskScope();
private static owned readonly _ready: Atomic<bool> := new Atomic<bool>(false);
private static _cfg: Task<Config>? := null;               // the admitted Task-cache form
using var g := await mutex.AcquireAsync();
```

### 7.7 Contracts used as input validation

```bezel
public method Listen(rawPort: string)
  requires(int.ParseOrNull(rawPort) <> null)     // WRONG: external input dressed as a caller contract

  Bind(int.ParseOrNull(rawPort)!);
end Listen;
```

> This **compiles** (`ParseOrNull` is `pure`), which is why it matters. `requires` states what a *correct caller already guarantees*; a violation says "fix the calling code", and for parsed external input there is no calling code to fix. Two-question test: *could a correct caller always guarantee E?* and *is a false E a bug in the calling code?* Two yeses → a contract; either no → a value shape, or a typed exception at a supervision boundary.

```bezel
public method TryListen(rawPort: string): bool          // caller branches → a value shape
  var port := int.ParseOrNull(rawPort);
  if port = null then return false; end;
  Bind(port);                                            // narrowed by the early exit
  return true;
end TryListen;

public method Listen(rawPort: string)                   // boundary-consumed → a typed exception
  var port := int.ParseOrNull(rawPort);
  if port = null then throw new FormatException($"not a port: {rawPort}"); end;
  Bind(port);
end Listen;
```

Not `ArgumentException` here: the `Argument*` family blames the calling code, and no caller is at fault.

### 7.8 Nullability narrowing

```bezel
if _field <> null then
  Use(_field);                     // ERROR: mutable fields and properties NEVER narrow
end;
```

> §7.4: narrowing applies to **locals and `readonly` fields only** — a mutable field can be rewritten between check and use by a call, a closure, or another continuation at an `await`. A local that is *mutably captured* by any lambda in the method is re-widened at the next call, function-value invocation, or `await`.

```bezel
var local := _field;
if local <> null then Use(local); end;
```

### 7.9 Small standard-library mistakes

```bezel
dict.Set(key, value);              // ERROR: no `Set` method on ANY collection
dict[missing] += 1;                // throws KeyNotFoundException: the indexer updates, never inserts
var l := new List<int>();          // no `List<T>` — the name is reserved
arr.Count(pred);                   // does NOT reach the predicate count on an Array<T> receiver
var s := "n = " + count;           // ERROR: no implicit ToString in `+`
pool.Sorted();                     // OwningArray<T> declares no Sorted/Reversed copy-forms
var text := String.Format("{0}", x);   // no String.Format
if a < b then ...                    // ERROR when either is `int?` (section 8.1)
```

```bezel
dict[key] := value;                // add-or-update; dict.Add(k, v) throws on a present key
tally.Increment(word);             // Counter<K>
var l := new Array<int>();
arr.Where(pred).Count();
var s := $"n = {count}";
pool.Reverse();                    // in place; or Drain() into a new owning collection
var text := $"{x}";
```

---

## 8. Recent changes that invert prior behaviour

Your instincts are wrong here even when they are good, because these rules replaced behaviour that shipped.

### 8.1 v0.267 — operators no longer lift over nullable value types (§7.11)

**Rejected**, on either side and in the mixed `T?`/`T` pairing: binary `+ - * / mod & | ^ << >>`; unary `+ - ~`; relational `< <= > >=`; every compound-assignment form; and `++`/`--`. **Retained**: `=`/`<>` (equality is total over an absent operand — `null = null` is `true`), `??`, `?.`, `?(args)`, `??=`. There is no three-valued logic: `and`/`or`/`not` take `bool` only. **Pattern position rides the same rule** — a relational or range pattern over a nullable scrutinee is *rejected*, not silently never-matching (§19.3, §16.6). A scrutinee narrowed non-null before the `case` (`var n: int? := 80;`, or inside `if n <> null then`) is not nullable there: ordering patterns are admitted, and the match needs no `null` arm to be exhaustive (§19.1).

```bezel
var a: int? := ReadOpt();
var sum := a + b;                              // ERROR (v0.267);  ✓ (a ?? 0) + (b ?? 0)  or  a! + b!
if a < b then ...                                // ERROR;  ✓ if a <> null and b <> null and a! < b! then
total += price;                                // ERROR when either is nullable
count++;                                       // ERROR when count is `int?`;  ✓ count := (count ?? 0) + 1;
if (flags & ADMIN) <> 0 then Grant(); end;     // ERROR when flags is `uint?` — it used to FAIL OPEN
case n of  1..5 => "small", _ => "other" end;  // ERROR when n is `int?`;  ✓ case n ?? 0 of ...
```

Narrowing a **local** restores the ordinary operators: `if a <> null then var s := a + 1; end;` (§7.4). A non-null initializer or assignment narrows too: `var m: int? := 0; m += 1;` is fine until `m` is reassigned from a nullable expression.

For an `invariant` over a *mutable* nullable field, where narrowing is unavailable, use the short-circuit form (`and` short-circuits) or declare the field `readonly`:

```bezel
invariant(_port <> null and _port! >= 0 and _port! <= 65535);
```

### 8.2 v0.265 — `UnwrapOr`'s argument is evaluated eagerly

`UnwrapOr(fallback: T)` declares a plain by-value parameter, and §22.5's argument-evaluation order is normative and observable: the argument is evaluated **at the call site, before the call**, on the `Ok` path exactly as on the `Error` path. `Map`, `MapError` and `AndThen` are the same. **There is no lazy `UnwrapOrElse`** — a recorded decline. Contrast `??`, which *is* conditional (§7.7); that contrast is exactly what misleads.

```bezel
var label := ParsePort(raw).UnwrapOr(BuildExpensiveMessage(raw));   // runs on the Ok path too
```

```bezel
case ParsePort(raw) of
  Ok(var p)    => Use(p),
  Error(var m) => Use(BuildExpensiveMessage(m))     // computed only on failure
end;
var port := ParsePort(raw).UnwrapOr(8080);          // a constant fallback is fine eagerly
```

### 8.3 v0.265 — materializing is **not** a remedy for the fresh-yield liveness hazard

The withdrawn advice was "materialize it into an owned `Array<T>` instead". At a **class or interface** element type, `ToArray()`/`ToSet()`/`ToDictionary(…)` copy **references, not ownership**: the `owned` on `ToArray(): owned Array<T>` is the **container axis only**. A slot filled from an enumerator-owned yield dangles exactly as an accessor result does. `ToArray()` remains the correct remedy for the **re-enumeration** hazard over a *live* backing — a different problem.

```bezel
using var probes := Probes(8);     // Probes: an iterator minting fresh Probe objects
var p := probes.First();           // compiles, but WARNS — a dead borrow
var arr := probes.ToArray();       // NOT a remedy: the slots borrow the same dead elements
```

```bezel
for var p in Probes(8) do p.Run(); end;          // remedy 1: use each element in the borrowing body

using var kept := new OwningArray<Probe>();      // remedy 2: give the elements an owner
for var p in OwningProbes(8) do                  // OwningProbes(...): IOwningEnumerable<Probe>
  kept.Add(move p);
end;
var idle := kept.FirstOrNull(lambda(p) => not p.IsBusy());   // collection-backed ⇒ sound
```

Where the producer is a **borrowing** minting iterator you cannot change, **no retention remedy exists** — consume before the next advance. The warning covers `First`/`Last`/`Single`/`ElementAt` and their `*OrNull` forms, `Min()`/`Max()`/`MinOrNull()`/`MaxOrNull()`, and `Aggregate`'s receiver leg, and at v0.268 the trace **passes through** a materializer result and through an `IGrouping<TKey,T>` loop variable: those are pass-through nodes, never roots — the verdict is always the root's.

### 8.4 v0.268 — a constructor-only record is not defaultable and rejects `with` (§9.9)

A record is **constructor-only** when it declares a constructor carrying a `requires` **or** `ensures` clause — any constructor, any visibility, any tier. The property reads the *declaration*, never what the clause says, and is constant across a generic record's instantiations.

```bezel
public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;
```

```bezel
var p := default(Port);            // ERROR: not defaultable
var q: Port;                       // fine: a local is uninitialized, never default-filled (§7.9)
_ := q.Value;                      // ERROR: `q` read before it is assigned
var slots: [8]Port;
slots[0] := new Port(1);           // ERROR: an element store uses the unassigned array; assign the whole array
var r := p with(Value := -1);      // ERROR: `with` is rejected on a constructor-only record
```

```bezel
var p := new Port(8080);           // a constructor or factory call
var opt: Port? := null;            // where a zero value is genuinely wanted: Port? / default(Port?)
public method WithValue(v: int): Port => new Port(v);      // the sanctioned update shape
```

`Port` at a `where T: new()` substitution is also rejected. Two rules land with it: a record declaring **positional fields** may not declare a clause-bearing constructor (a compile error at the record declaration — the remedies are a **body record**, so every constructor is declared, or a **static factory method**); and a `serialized optional` component whose type is not defaultable is now rejected. In every other respect such a record is ordinary.

### 8.5 v0.246 — the use-site transfer marker is `move`, not `owned`

Through v0.245 the marker was spelled `owned` at binding positions. After v0.246, **`owned` appears only where a slot is described, and `move` only where an action occurs.** Any code with `owned` in an argument or right-hand-side position is pre-v0.246 and wrong: `TakeOwnership(owned resource)` → `TakeOwnership(move resource)`; `var y := owned x;` → `var y := move x;`; `case owned outcome of …` → `case move outcome of …`; `using var c := owned conn;` → `using var c := move conn;`. `owned` (declarative) is correct in exactly these positions: parameters (`owned x: T`), fields (`private owned _f: T;`), return types (`: owned T`), function-type slots, choice payloads, `[N owned]T`, `static owned readonly`. Nothing else.

### 8.6 v0.289 — four retirements: `when`, `out owned var`, `operator ++`/`--`, and the collections import

- **`(when …)` / `when … end when;` are gone.** A ladder of conditions is `(if c1 then a elif c2 then b else z)` or `if c1 then … elif c2 then … else … end;` — same type, same order, same ownership meet. `when` survives only as the arm guard (`var t when t > 30 => …`) and the `when true`/`when false` clause.
- **`out owned var x` is gone.** `out var x` adopts the signature: a borrow against `out`, an owned local against `out owned` (discharge it yourself, or use `out using var x`, which is accepted only against `out owned`).
- **`operator ++`/`operator --` are gone.** `++`/`--` are built-in statements on integer, floating-point, `decimal` and `char` operands only (§6.6's set — `char++` still works). Declare `operator +(c: Counter, n: int)` and write `c := c + 1;`, or use a named method.
- **`import System.Collections.Generic;` is redundant.** Every §4 name is bare-visible (`Dictionary`, `Set`, `Stack`, `Queue`, the spans, `Counter`, `FrozenArray`, `KeyNotFoundException`, the iteration interfaces). The line still compiles. Beware the new bare names when naming your own types: a same-named type of the same arity in your module shadows the import; a same-named export from another imported module, at the same generic arity (a non-generic `Counter` does not clash with `Counter<K>`), is an ambiguity error — as for every always-available name (`Task`, `Type`, the exception types, …; `Object` never collides: §8.1). Qualify the import with its module name or an alias, or the built-in with its module: `System.Exception`, or `System.Threading.Task` with `import System.Threading;`.

### 8.7 v0.290 — the stdlib regularization: nine defaults, four retired duals, one interface-declared read tier

- **Optional trailing arguments are `:=` defaults now** — `Task.Delay(ms, token := …)`, `Directory.Delete(path, recursive := false)`, `Pad*(width, paddingChar := ' ')`, `new FileStream(path, mode, access := FileAccess.ReadWrite)`, `new BinaryDeserializer(span, ignoreUnknownFields := false)` / `Binary.Parse<T>(span, ignoreUnknownFields := false)` (both span families), `Debug.Assert(cond, message := "")`, `Math.Round(x, digits := 0)`, `IndexOf(value, startIndex := 0)` on `string` and `Array<T>`. Every call you wrote before still compiles; named arguments for the defaulted position are new.
- **Gone:** `string.IsNullOrEmpty(s)` (→ `s = null or s.IsEmpty`); static `string.Compare(a, b[, ic])` (→ `a.CompareTo(b)`, `a.CompareTo(b, ignoreCase := true)`); `sw.Restart()` (→ `sw.Reset(); sw.Start();`); `t.Implements(i)` (→ `i.IsAssignableFrom(t)`); `enc.GetString(bytes, start, length)` (→ `enc.GetString(bytes.AsReadOnlySpan(start, length))`); `tally.ContainsKey(k)` on `Counter<K>` (→ `tally.Contains(k)` / `k in tally`); `Math.Sign` on a `char` (→ `Math.Sign(int(c))`). `Math.Sign<T>` is generic over `INumber<T>` and is **not** `pure`.
- **The byte read tier is one block on `IReadOnlySpan<byte>`.** Nothing changes at a call: `bytes.AsReadOnlySpan().ReadUInt32LittleEndian()` and `span.ReadInt16BigEndian()` resolve as before. A parameter typed `IReadOnlySpan<byte>` now has the read accessors too.

---

## 9. What this document does not cover

| Topic | Go to |
|---|---|
| Any type or member not in section 5 | `Bezel_Standard_Library.md` — and if it is not there, **it does not exist** |
| The exact wording of a rule, the grammar (Appendix A), the enforced invariants (Appendix C) | `Bezel_Language_Spec.md` |
| *Why* a rule is what it is; what was declined | `Bezel_Design_Rationale.md`, `docs/proposals/DECLINED.md` |
| Migration across revisions | `Bezel_Changelog.md` |
| Worked, example-first teaching | the seven guides |
| Extern/FFI declarations, serialization clauses, inline arrays, `Shared`/`Weak` topologies, domains and sendability, the `polymorphic` registry, static initialization order | spec §32, §9.8, §15.1, §28.13, §30.13–§30.14, §11.1, §22.13 |

**One seam worth closing here: `Task<T>` versus `OwningTask<T>` results** (§30.5). Both are mint-announcing, so the *container* obligation is identical — the caller owns the task. The **result** rules differ:

- **`Task` / `Task<T>` for a value, `string`, or runtime-managed T:** no ownership crosses the await. Any form is permitted, and the awaited expression may be a bare producer call.
- **`Task<T>` for class T — Pattern A:** the task may be awaited from multiple places; every awaiter receives a **borrow** of the same underlying result, whose lifetime is the task's ("disposing the task disposes the result"). The awaited operand must be a **named location** — a `using var`/`await using var` local, an `owned` parameter, an owning-collection slot, a borrowing-iteration variable, admitted static task storage — or an unbound temporary whose borrowed result is used **within that same full expression**. The result borrow may flow to a `var` binding, a non-owned argument, or a `return` under the same Pattern A type; it may **not** flow to a `using var` binding, an `owned` field or slot, an `owned` argument, or an `owned T` return. `var x := await Producer();` — a fresh `var`/`const` whose initializer is *exactly* the await — is the **scoped await-bind**, desugared to a hidden pinned slot; `using var x := await …`, assignment to an existing local, and compositions like `(await E).Name` are not.
- **`OwningTask<T>` for class T — Pattern B:** the task produces a single **owned** result; awaiting transfers ownership, and the await must appear in an owned-consuming context (`using var x := await …`, `var x := await …`, an `owned` argument or slot store, a `return` under `owned T`, an owning `yield return`, or `_ := await …;`). The task may be awaited at most once for a successful result; a second await raises `InvalidOperationException`. The await **consumes its operand** when that operand is an owned task value — and the position is marker-free, so it takes **no `move`**.
- `OwningTask<T>` over a value, `string`, or runtime-managed T is a compile error, as is either task type over a **move-only** value (an owning choice, an owning inline array).

Choose by intent: `OwningTask<T>` for resource acquisition (one fresh single-consumer result), `Task<T>` for fan-out (several subscribers reading the same eventual result). An `async` **lambda** takes its wrapper from its ascription, else from a closed target slot, else from its own body — and a body returning a fresh owned class infers `OwningTask<C>`, so at a generic `(…) -> Task<R>` slot such a lambda is **inapplicable** (the candidate drops out): write `async lambda(x): Task<C> => …` to bind `R` (§30.6, v0.296).
