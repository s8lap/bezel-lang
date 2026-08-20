# Bezel for Code Generation

*Validated against v0.268. Companion to the Bezel Language Specification — a writing reference, not a normative document.*

A dense reference for writing correct Bezel without reading the spec. It covers the syntax surface, the ownership rules, the standard-library name index, the idioms that work, and the shapes that look right and are rejected.

---

## 1. How to use this document

**What this is.** Everything needed to emit compiling Bezel: the whole statement and expression grammar, the ownership discipline the compiler enforces, and an index of every standard-library name you are allowed to spell.

**What this is not.** Normative. Where this document and `Bezel_Language_Spec.md` disagree, the spec wins and this document has a defect. Citations (`§17.2`, `stdlib §4.5`) are to spec v0.268 and `Bezel_Standard_Library.md` v0.268 so any claim here can be re-checked at its source.

**The one rule that matters most.** *If a type or member does not appear in section 5 of this document, look it up in `Bezel_Standard_Library.md` — and if it is not there, it does not exist.* **Never guess an API.** Inventing a plausible standard-library name is the single most common failure mode when writing Bezel: `MemoryStream`, `File.Open(path)`, `List<T>`, `string.Format`, `.ToList()`, `Task.Run`, `Option<T>`, `x.Dispose()` on a borrow — all plausible, all absent. Bezel's library is small and deliberately shaped; the shape is not .NET's.

When the API you need genuinely is not there, **say so** rather than approximating. A message that reads "Bezel's stdlib has no regular-expression module (`System.Text.Regex` is deferred, stdlib §11); this needs a hand-written scanner or a library addition" is a correct answer. A hand-rolled `Regex.Match` call is not.

**Reading the examples.** `✗` marks a line the compiler rejects; `✓` marks the spelling to write instead. A comment containing `ERROR` marks the exact rejected line. Illustrative user types (`Connection`, `Probe`) are declared inline where an example needs them; every standard-library name used is in section 5.

**Where to go for what this document omits:** section 9.

---

## 2. Twenty facts that break C#/Rust/Kotlin/Swift/TS muscle memory

Read this page first. Every row is a habit that produces broken Bezel.

| # | The habit | Bezel | Cite |
|---|---|---|---|
| 1 | `{ }` blocks | `end;` — or `end Name;` / `end for;` / `end constructor;`. Braces appear **only** inside string interpolation; there is no fourth bracket kind | §1.6 |
| 2 | `x = 1;` assigns | `x := 1;` assigns. `=` is **equality** | A.6, §17.3 |
| 3 | `a == b`, `a != b` | `a = b`, `a <> b` | §17.3 |
| 4 | `&&`, `\|\|`, `!x` | `and`, `or`, `not x`. Postfix `!` is the null-forgiving operator, nothing else | §17.1, §7.8 |
| 5 | `a % b` | `a mod b`; the compound form is one token, `mod=` | §17.1, A.6 |
| 6 | `else if`, `switch`, `foreach` | `elif`; `case … of … end case;` (scrutinee) or `when … end when;` (conditions); `for var x in xs do … end;`. **`foreach` does not exist in Bezel** | §19, §21.4 |
| 7 | `cond ? a : b` | `(if cond then a else b)` — the parentheses are part of the form, and `else` is mandatory | §20 |
| 8 | `void` | omit the `: Type` clause; the result type is the unit type `()` | §22.1, §4.7 |
| 9 | `namespace` / `using X;` | `module X.Y;` (one per file, first line) / `import X.Y;` | §2.1, §2.2 |
| 10 | `x => x * 2` | `lambda(x) => x * 2` — the keyword is required; `lambda() => 0` needs the empty parens | §27.3 |
| 11 | `(int)d`, `d as int` | `int(d)` — casts are call syntax on the type name. `as` yields `T?` and is a type test only | §5.2, §17.4 |
| 12 | `var xs = [1,2,3];` | A collection literal has **no standalone type**: `var xs: Array<int> := [1, 2, 3];` or pass it where a parameter supplies the target | §15.4 |
| 13 | `if (x is Dog d)` | `is` **does not bind**. `var d := x as Dog; if d <> null then …` or a `case` arm `var d: Dog` | §12.7 |
| 14 | `if (count)` truthiness | Conditions are `bool`. Write `count <> 0` | §4.1 |
| 15 | `i++` as an expression, `++i` | `++`/`--` are **postfix-only statements**: `i++;` on its own line. Assignments are statements too and produce no value | §17.6, §17.2 |
| 16 | `int? + int?` lifts | **Retired at v0.267.** Arithmetic, bitwise, shift, relational, compound and `++`/`--` over a nullable operand are a compile error. `=`/`<>`/`??`/`?.`/`?(args)`/`??=` still work | §7.11 |
| 17 | `if (_field != null) Use(_field);` | Mutable fields and properties **never narrow**. Copy to a local first | §7.4 |
| 18 | `var y = x;` moves (Rust) or aliases-and-GCs (C#) | `var y := x;` is **always a borrow**; nothing is consumed silently. Transfer is written `move x` — at use sites only | §28.3, §22.3 |
| 19 | `new Foo()` returned from an unmarked method | A plain class-typed **instance** return means "a borrow of the receiver". A method that creates must declare `: owned T`; a **static** method returning a class type without `owned` is rejected at its declaration | §28.3 |
| 20 | `Dispose()`, `try/finally`, `using(...)` blocks | You never write disposal plumbing. `using var x := …;` disposes at scope end on every edge, including the exception edge; a written `x.Dispose();` means *release early* | §28.2, §28.10 |

Four more that cost the same amount of time:

| # | The habit | Bezel | Cite |
|---|---|---|---|
| 21 | `Dictionary` and friends dispose their elements | Two families. `Array<T>`/`Dictionary<K,V>`/`Set<T>` hold **borrows** and dispose nothing; `OwningArray<T>`/`OwningDictionary<K,V>`/… own their elements and cascade. No implicit conversion between them | stdlib §4.0 |
| 22 | Contract clauses take a semicolon | `requires(…)` / `ensures(…)` sit between the signature and the body and carry **no terminator**. Only `invariant(E);` (a class member) keeps its `;` | §22.16 |
| 23 | `var _ := e;` discards | The discard is a statement: `_ := e;`. `var _ := e;`, `const _ := e;`, `out var _`, `ref _` are all compile errors | §17.13 |
| 24 | `String.Format`, `"n = " + count` | Interpolation is the one convention: `$"n = {count}"`. There is no implicit `ToString` in `+` and no `String.Format` | stdlib §11.4 |

---

## 3. Syntax surface

### 3.1 File shape

```bezel
module App.Billing;                       // first non-comment line; exactly one per file
import System.Collections.Generic;
import App.Ledger as Led;                 // optional file-scoped alias
type Handler<T> := (T, string?) -> bool;  // type alias: `:=`, never a visibility keyword
```

After the module line and imports come top-level declarations: `class`, `interface`, `record`, `enum`, `choice`, `extension`, `type`, `extern` (§2.1, §2.2, §27.8, A.2). Source files are UTF-8 with extension `.bzl`. Comments: `// line`, `/* block, does not nest */`, `/// doc` (§1.1, §1.5).

Auto-imported, no `import` needed (§2.3): `System` (`Console`, `Math`, `Convert`, `Environment`, `DateTime`, `Guid`, `Random`, `Shared<T>`/`Weak<T>`, `Result<T,E>`, `ScopeGuard`, `Type`, the exception hierarchy, the FFI carriers), `System.Text` (`StringBuilder`, `Encoding`, `ReadOnlyStringSpan`), the primitives, `Array<T>`, `OwningArray<T>`, the `Task` family, the eight iteration interfaces, `CancellationToken`/`CancellationTokenSource`, the event types. **Everything else needs an import** — see the table in section 5.0.

### 3.2 Keywords

Unconditionally reserved (§B.1):

```
abstract and as async await base bool break byte case catch char choice class closed
const constructor continue decimal default destructor do double elif else end ensures
enum extension extern false finally float for if import in int interface internal
invariant is lambda long method mod module move nameof new not null or out owned
override private property protected public readonly record ref requires return sbyte
sealed short static string then this throw true try type typeof uint ulong ushort
using var virtual when where while with yield
```

`requires`/`ensures`/`invariant` became unconditionally reserved at v0.260. `event` is reserved-but-unused (§B.3) — the event member kind was retired (§24).

**Contextual** (§B.2) — a keyword only in the listed position, an ordinary identifier anywhere else: `convention`, `env`, `equality`, `get`, `set`, `hashable`, `notnull`, `sendable`, `of`, `old`, `result`, `operator`, `polymorphic`, `pure`, `scoped`, `serialized` (with `skip`/`optional`), `step`, `symbol`, `variadic`, `value`.

`_` is a reserved **token**, held out of the identifier namespace like punctuation: never declarable, never readable as a name. Identifiers *containing* `_` (`_camelCase`, `snake_case`) are ordinary (§1.3, §17.13).

**Identifier traps.** These cannot name a local, parameter, field, or type: `out`, `in`, `is`, `as`, `type`, `method`, `property`, `record`, `class`, `base`, `with`, `when`, `where`, `move`, `owned`, `default`, `new`, `not`, `and`, `or`, `mod`, `then`, `end`, `do`, `using`, `var`, `const`, `requires`, `ensures`, `invariant`. All keywords are lowercase; a PascalCase name is never a keyword (§1.4).

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
  property Balance: decimal;
  readonly property Id: string;
  property [i: int]: string;              // indexer
  static method Parse(s: string): IBankAccount;             // §11.8, abstract-only
  static operator +(a: IBankAccount, b: IBankAccount): IBankAccount;
end IBankAccount;

record Point(X: int, Y: int);             // positional-only record: ends with `;`
record Money(Amount: decimal, Cur: string serialized "currency" equality skip);
record Rectangle                          // body record
  public readonly property Width: decimal;
  public constructor(w: decimal)  Width := w;  end constructor;
  public method Area(): decimal => Width * Width;
end Rectangle;
record Vector2(X: double, Y: double)      // combined form: positional + body
end Vector2;

enum Priority
  Low, Medium, High;                      // the constant list ends with `;`
  public method IsUrgent(): bool => this = High;
end Priority;
enum HttpStatus: short  Ok := 200, NotFound := 404;  end HttpStatus;

public choice FetchOutcome
  Fetched(owned response: Response),
  NotModified,
  Failed(status: int, message: string);   // the case list ends with `;`
  public method IsOk(): bool => (case this of Fetched(_) => true, _ => false);
end FetchOutcome;

extension string                          // module level; public | internal (default internal)
  public property IsBlank: bool => this.Length = 0;
end;

extern "libc" convention "cdecl"
  public method puts(s: CString): int;
  public method strdup(s: CString): owned CString? := symbol "strdup";
end;
```

**Member modifier order** — the compiler rejects any other order (§11.5):

```
visibility  static  (async|pure)  (abstract|virtual|override|sealed override)  (owned|scoped)  readonly  member-kind  Name …
```

| Member | Exact form | Cite |
|---|---|---|
| Field | `private owned readonly _fs: FileStream := new FileStream(p, FileMode.Open);` — **no `field` keyword**; visibility mandatory | A.4, §13.3 |
| Const | `public const Max: int := 100;` | A.4 |
| Method | `public static async method Load(u: string): Task<string> … end Load;` | §22.1 |
| Method, arrow | `public method Area(): decimal => Width * Height;` | §22.2 |
| Method, arrow, unit | `public method SetX(v: int) => _x := v;` (assignment body only with no declared return type) | §22.2 |
| Abstract method | `public abstract method Speak(): string;` — body-less, ends `;` | §12.5 |
| Auto-property | `public property Name: string;` / `public property Id: int private set;` / `public readonly property Pi: double := 3.14159;` | §23.1 |
| Expression property | `public property Area: decimal => Width * Height;` — **never write `readonly` here** (§23.2 rejects it on every body-bearing form) | §23.2 |
| Block property | `public property Balance: decimal` / `get => _b;` / `set => do … end;` / `end Balance;` | §23.3 |
| Owned-taking setter | `set(owned);` or `set(owned x);` — the type is the property's and is never repeated | §23.3 |
| Indexer | `public property [i: int]: string => _items[i];` — the block form closes with a bare `end;` | §23.4 |
| Constructor | `public constructor(a: int)` + optional `base(a);`/`this(a);` chain + body + `end constructor;` | §22.7 |
| Static constructor | `static constructor()` … `end constructor;` — no visibility, no `pure`, no clauses | A.4 |
| Destructor | `destructor` … `end destructor;` — no visibility, no name, no parameters | §28.4 |
| Operator | `public static pure method operator +(a: V, b: V): V => …;` closes `end operator +;` | §25.1 |
| Invariant | `invariant(_bal + _limit >= 0m);` — class bodies only, no visibility, keeps its `;` | §22.16 |
| Polymorphic registry | `polymorphic("dog": Dog, "cat": Cat);` — at most one per class | A.3 |
| Nested type | `public record Token(…);` inside a class body — visibility **required** | §11.6 |

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

The complete statement list (A.6): `LocalDecl`, `LocalConstDecl`, `AssignmentStmt`, `ExpressionStmt`, `DiscardStmt`, `IfStmt`, `WhileStmt`, `ForStmt`, `DoEndBlock`, `CaseStmt`, `WhenStmt`, `ReturnStmt`, `ThrowStmt`, `BreakStmt`, `ContinueStmt`, `TryStmt`, `YieldStmt`, `IncrementStmt`, `LocalTypeDecl`. Every statement ends `;`. Every block ends `end … ;`.

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
| `when` statement | `end when;` or `end;` | — |
| lambda block body | `do … end` — bare `end`, no label and no `;` of its own | ✗ `end lambda;` |
| `case`/`when` arm `do` block | `do … end` — bare `end`, no `;` | — |

Every statement shape:

```bezel
// declarations
var balance: decimal;                       // typed, no initializer
var count: int := 0;
var greeting := "Hello";                    // inferred
const Pi: double := 3.14159;                // single-assignment binding (§21.1)
using var fs := new FileStream(p, FileMode.Open);   // owned; disposed at this block's end
using var _ := mutex.Acquire();                     // anonymous owning binding
await using var conn := await OpenAsync();          // async bodies only
record Row(Name: string, Age: int);         // LocalTypeDecl: `record` and `enum` ONLY, no visibility
enum Phase  Start, Done;  end Phase;

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
for var i in 1..10 do … end;
for var i in 0..<n step 2 do … end;
for var t in transactions do … end;
for var x: int in source do … end;          // optional ascription
for var _ in source do … end;               // ignore; ✗ `for var _: T in …`
outer: for var row in rows do … end outer;
await for var line in reader do … end;
await for var line in reader with token do … end;   // `with` = CancellationToken (§30.7)

// do block — the way to end a scope early
do
  using var t := Open();
end;

// case / when statements
case delta of
  -10..-6 => "strongly down",
  ..<0    => Log("down"),                   // single-statement arm
  0       => do Reset(); Log("flat"); end,  // do-block arm
  _       => Log("up")
end case;

when
  score >= 90 => Console.WriteLine("A"),
  _           => Console.WriteLine("F")
end when;

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
✗ if flags & MASK = 0 then …          ✓ if (flags & MASK) = 0 then …
✗ var n := a << 2 + 1;                ✓ var n := (a << 2) + 1;
✗ var r := mask & 0..n;               ✓ var r := mask & (0..n);
```

`(if …)`, `(case …)`, `(when …)` and the binding atom `(var x := e; body)` take no precedence entry — they are self-delimited primary atoms usable in any operand position (§17.2, §17.15).

Literals (§18, A.8):

```bezel
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

```bezel
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

The paren atoms:

```bezel
var max   := (if a > b then a else b);                    // `else` MANDATORY
var grade := (if s >= 90 then "A" elif s >= 80 then "B" else "F");
var g2    := (case score of 90..100 => "A", 70..<80 => "C", _ => "F");
var g3    := (when score >= 90 => "A", _ => "F");
var area  := (var w := bounds.Width(); w * w);            // binding atom (§17.15)
```

Arms in expression position are **expression-bodied only**; statement and `do … end` arms belong to the statement forms (§19.2).

Lambdas and function types (§27.1, §27.3):

```bezel
lambda(x: int) => x * 2
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

```bezel
_          null        var x       var x: T      Dog       Color.Red     -7
90..100    0..<60      ..<0        31..                                  // range patterns
> 70   < 50   <= 100   >= 0                                              // relational (these four only)
Point(x, y)   Point(_, y)   Point(var x, var y)                          // positional destructuring
Rect(Width = var w, Height = 3)                                          // named destructuring: `=`, not `:=`
Fetched(move response = var r)                                           // marked slot (owning match)
1 | 2 | 3      Red | Blue                                                // alternatives: binding-free
var t when t > 30 => …                                                   // guard
```

Positional or named, never mixed. A naked identifier is a **binding** only inside positional destructuring; at top level it is a type test. Classes have no positional-destructuring surface, even with a primary constructor (§19.3).

Ranges and the `throw` expression:

```bezel
1..10     0..<10     0..100 step 2     100..0 step -1
arr[2..5]   arr[..5]   arr[10..]   arr[..]
var cfg := LoadConfig() ?? throw new InvalidOperationException("no config");
```

`step` applies to closed ranges only and never appears in a pattern (§16.4).

### 3.6 Naming

PascalCase for types, methods, properties and other public members; `_camelCase` for private fields; `camelCase` for parameters and locals. **The compiler does not enforce these** (§1.3). Identifiers are case-sensitive. Modules are dotted PascalCase (`BankingApp.Transactions`); the dotted form is a single name and establishes no nesting (§2.1). `string`/`String`, `int`/`Int32` and the rest name the same types and are interchangeable in source (§3.4).

---

## 4. Ownership

### 4.1 The core model

Bezel has **no garbage collector** and no runtime lifetime mechanism for user classes — no refcounts, no regions, no heap scanning. Every class instance is freed by compiler-inserted code at a statically known point (§28).

Every class- or interface-typed expression and binding carries exactly one **ownership classification** at every program point (§28.3):

- **owned** — you must dispose it exactly once, on every path.
- **borrowed** — you must not dispose it; it is valid only while its **source** is alive.
- **moved-from** — the obligation has left; the binding is unreadable until reassigned.
- **maybe-consumed** — a join of owned with moved-from or disposed; unreadable.

Records, enums, primitives and inline arrays are **value types**: no disposal, no classification. `string`, boxed values and function values are **runtime-managed**: no disposal, no classification, and `owned` on them is a compile error (§28.1, §28.9).

**You never write `Dispose()` as plumbing.** An owned value reaching its scope's end undischarged is disposed *there*, automatically, on the normal edge **and** the exception edge (§28.3). No `try`/`finally` for a value, ever. A written `x.Dispose();` means *release early*. The only difference between the two edges is a lint: a plain-`var` implicit drop on normal exit raises the **must-use warning**; the exception edge is silent.

Disposal has exactly two customization points: a `destructor` (only for native or foreign resources) and `owned` **fields**, auto-disposed with their container in reverse declaration order (§28.4, §28.5). The generated `Dispose()` is public, idempotent and non-overridable; there is no `disposing` parameter, no `_disposed` flag, no `base.Dispose()` call.

**Trivial-dispose classes** are exempt from ceremony, not from tracking (§28.9): a class with no destructor, no `owned` field of non-trivial type, no owning-inline-array field, on a trivial-dispose base. `Array<T>`, `Set<T>`, `Dictionary<K,V>`, `StringBuilder`, `Random` qualify — plain `var`, no must-use warning. `OwningArray<T>` and anything holding a resource do not. **Every borrow rule still applies to trivial-dispose references.**

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

**`move` — the use-site transfer marker** (§22.3, §28.8.1). Three operands only: `move x` (a named local or `owned` parameter), `move obj.f` (an `owned` field), `move coll[i]` (an owning slot).

*Required* — omitting it is a compile error ("this call consumes `x`; write `M(move x)`") — when a **named owned binding** feeds:

| Position | Spelling |
|---|---|
| an `owned` argument | `M(move x)` |
| a local binding | `var y := move x;` / `using var y := move x;` |
| an assignment | `y := move x;` |
| an if-expression branch or an expression-`case`/`when` arm result | `(if c then move a else move b)` |
| an owning-match scrutinee | `case move outcome of …` |

*An error* — the marker is **rejected as redundant** — at a **target-announced position**, where the target already says "consume". Memorize this list; "mark every transfer" is the wrong generalization and produces exactly these errors (§28.3, §22.3):

- **`return` operand** under an `owned` or mint-announcing return: `return x;` — ✗ `return move x;`
- **`throw` operand**: `throw x;` — ✗ `throw move x;`
- **discard RHS**: `_ := x;` — ✗ `_ := move x;`
- **owned-slot store**: `sink._f := x;` into an `owned` field, an owning collection slot, or a `set(owned)` property — ✗ `sink._f := move x;`
- **Pattern B `await`** on an owned `OwningTask<T>` binding: `using var r := await t;` — ✗ `await move t`
- **owning iterator `yield return`**: `yield return x;` — ✗ `yield return move x;`

`move` is *also* an error on a **fresh producer** (`M(move new T())` — nothing is being invalidated), on a **borrow**, in a **`??` operand** (a named owned binding written bare as a `??` arm is a *borrow* arm), inside a **closure** over a captured owned binding (captured owned bindings are uniformly in borrow state — move, reassign, dispose and discard of them are all errors), and on a **`static` field**.

**The no-silent-consume rule.** An unmarked read of a class-typed binding *never* consumes. `var b := a;` is always a borrow of `a`. This is the single most important spelling fact in the language (§28.3).

**Field and slot moves carry a restoration obligation** (§28.8.1). Every path from a `move obj.f` to *any* method exit — normal return, fall-through, `throw`, `yield break` — must execute an assignment `obj.f := value;`. There is no terminal-move dispensation: `return move _f;` is rejected. Only a `destructor` body is exempt. Restoration must not go through something that can throw, hence **build the replacement first, then take**. A nullable owned field may be restored with `null`.

**`using var`** (§28.10): owned, disposed at the block's end on **every** exit edge, non-reassignable, and **never movable out of**. `using var fresh := new Buffer(); … return fresh;` is a compile error; a factory uses plain `var`.

- `using var y := move x;` — **adoption**: the standard way to turn an owned local, parameter or loop-variable obligation into scope-bound disposal.
- `using var y := x;` (unmarked, `x` a named owned binding) — **does not adopt**. It is a **borrow-pin**: no obligation is assumed, `x` keeps it, and the pin extends `x`'s required liveness to this block's end.
- `using var _ := expr;` — the anonymous owning binding (`using var _ := mutex.Acquire();`).
- Admissible over value-typed, runtime-managed and move-only right-hand sides, where disposal is a no-op — safe to use uniformly in generic code.
- To dispose earlier than the method's end, nest a `do … end` block.

**Disposal-form menu** (§28.3):

| Form | Disposes at | Movable out? | Diagnostic |
|---|---|---|---|
| `using var x := …` | scope end, every path | **no** | none |
| plain `var x := …`, undischarged | scope end (auto-drop) | yes | **must-use warning** |
| `x.Dispose();` | at the call | yes, until disposed | none |
| `_ := x;` | end of the discard statement | — | none |
| unbound temporary | end of the full expression | — | none |

`_ := x;` leaves the binding **explicitly-disposed**: a later straight-line read is a mandatory use-after-dispose warning, and a read after a *conditional* discard is a compile error at the merge. `_ := task` is **rejected** for the `Task` family — use `Task.FireAndForget(t)` (§17.13).

**`ref` / `out`** (§22.3, §22.4). Written at the declaration **and** at the call site. `out` is copy-out-on-normal-return: the callee must assign on every normal-completion path; on the exception edge the recipient is untouched.

- `out owned p: T` is the only modifier compound. At the call site it accepts **only** `out using var x` or `out owned var x`; plain `out var x` is rejected.
- `ref`/`out` arguments may **not** name a location of owned state (an owned local, an `owned` parameter, an `owned` field, an owning slot), nor a `using var`, nor a `readonly` field outside its assignable context. Pass a borrow instead, or use `owned` parameters/returns and `move`.
- `ref`/`out` are **forbidden** on `async` methods, on lambdas, on iterator methods and on indexers (§30.1, §29.3, §23.5). The argument's static type must match the referent type exactly. `out var _` is an error; write `out _`.

**`scoped`** (§28.14) declares **storage-bounded retention** — the callee may store the thing, bounded by the storing object's life.

- On a **function-typed** parameter (`method Subscribe(scoped h: () -> ())`): the callee may store the callback in a `scoped` field of the bounding object, forward it to another same-bound `scoped` parameter, or invoke it. Function-typed arguments carry **no call-site marker**.
- On a **class- or interface-typed** parameter: the callee may store the borrow in a direct non-owned field of the bounding object. The call site writes `M(scoped x)` — an argument *modifier*, not an expression — and a fresh producer is rejected there; bind first.
- A function-typed parameter **without** `scoped` is **call-bounded**: invocable during the call, never retained. Callers may pass any closure to one.
- **The price: captured local sources are frozen** while the bounding object lives — no move, no reassign, and **no `Dispose()`**, stricter than the ordinary freeze.
- `scoped` on a **static** method is admitted only when that method's return is `owned`.

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

1. **A static method returning a class type must say `owned`** (or return a mint-announcing type, or a bare `T` with a source). `static method Pick(a: Res, b: Res): Res` is rejected at the declaration.
2. **An instance method that creates something must say `owned`.** Otherwise its return reads as a borrow of the receiver.
3. **A property or indexer getter may never return a fresh owned value.** Getters lend only; properties carry no `owned`. Expose a factory as a *method* returning `owned T`.
4. **An iterator method's unmarked iterable return is already a transfer announcement** — no `owned` needed, at any callee kind.
5. An **unbound owned temporary** is disposed at the **end of the full expression**, after every borrowing use inside it. `items.Where(p).ToArray().Length` is fine; binding, storing or returning a borrow *of* such a temporary past that expression is rejected. A `case` scrutinee extends the boundary to the whole `case`, so pattern bindings survive the selected arm.

### 4.4 Borrow rules

A borrow is valid only where **every** source in its source set is live; joins, `??` and arm meets take the **union** of source sets (§28.3).

- A borrow may **not** be assigned to a local whose scope strictly encloses its source's scope — including flowing around a loop back edge. Rejected **at the assignment**, not at the later use.
- A borrow of a **scope-bound local** may not be stored in a field, or in a collection that outlives that scope. A borrow whose source *does* outlive the object **is** admissible: the **back-reference** idiom binds a constructor parameter into a plain (non-`owned`) class-typed field (`private _owner: Customer;`). The check is structural — parameter- and receiver-sourced admitted, locals and temporaries rejected.
- A borrow may not be returned unless its source is one every caller can re-establish: receiver-sourced from an instance method, the receiver-parameter borrow of an extension method, or static storage through a **static property getter**.
- A borrow may not be captured by a closure that may outlive its source. Capture is permitted for immediately-invoked closures, closures bound to a `using var` of function type, and closures passed to a function-typed parameter.
- **The domain clause.** Every field-held, collection-held or async-frame-held borrow carries the obligation not to transfer the borrowed object to another domain, nor to consume it into a domain-reachable construction. This has **no runtime backstop** — it is a data race (§30.14).

**Merges must agree** (§28.3). owned ⊓ owned = owned. borrowed ⊓ borrowed = borrowed, sources unioned. **owned ⊓ borrowed = rejected.** owned ⊓ moved-from = maybe-consumed (unreadable). The rule governs `if` statements, `if` expressions, `??`, and expression-`case`/`when` arms identically.

**The consume-site freeze** (§28.3). Consuming an owned **binding** while a **live local borrow of its current value** exists is a compile error. Frozen: every transfer (a `move` at an argument, binding, assignment or arm; a store into an `owned` field, an owning slot or a `set(owned)`; a `return` under an `owned` or mint-announcing return; an owning iterator's `yield return`) and drop-on-overwrite. **Not** frozen — these are site-announced kills, permitted while borrows live: an explicit `Dispose()` (including `x?.Dispose()`), the discard `_ := x;`, `throw x`, and a Pattern B `await` on an owned `OwningTask<T>` binding. Fields and collection slots are **not** frozen. The mechanical rewrite for the frozen case is **transfer first, then re-borrow from the new owner**.

**Borrow-bearing records** (§9.7). A record is borrow-bearing if it has at least one **borrow-carrying** field: one whose declared type is a reference type (class or interface, nullable or not), an inline array of reference type, or another borrow-bearing record or choice. The predicate reads **declared field types**, never runtime contents — so `string` and function-typed fields are *not* borrow-carrying, while a field declared `Object` or an interface type **is**, nullable or not. Values of such a record type:

- may not outlive the source of any borrow they carry;
- may **not be constructed from a temporary** — bind the source to a named local of adequate scope first;
- may **not** be stored in a class instance field (statically rejected, no trust residual) — store the *source* (an `owned CancellationTokenSource`, not a `CancellationToken`) and re-derive the view at the use site;
- may be stored in a **static** field only in the null-source form (every reference-typed field and inline-array slot `null`);
- may not be an `async` parameter, result or element type. A local of such a type crossing a suspension point is admitted only if every borrow it carries has a frame-resident source. `CancellationToken` is the single stdlib-certified exception.

Records also cannot have destructors, `owned` fields, owning-inline-array fields, or implement `IDisposable`; their reference-typed fields are always borrows. Spans (`Span<T>`, `ReadOnlySpan<T>`, `ArraySpan<T>`, `ReadOnlyArraySpan<T>`, `BitReader`, `BitWriter`) are the canonical borrow-bearing records; `ReadOnlyStringSpan` is the deliberate exception — its only reference field is a `string`, so it may sit in fields and cross `await`.

**Value receivers are borrows** (§9.4). In every instance member body of a **value kind** — a record method or accessor, a choice method, an enum member, an extension member over a value receiver — `this` binds a **borrow** of the receiver value, and a store that would land in the receiver's own storage is a compile error at the member body: `this.X := e`, a bare `X := e` resolving to receiver storage, an inline-array chain, compound assignment and `++`/`--` through their desugarings, and passing such a chain as a `ref`/`out` argument. The rule keys on *where the store would land*. Two boundaries: **constructor bodies are not receiver positions** (`Width := width;` is the canonical body-record initialization), and an **external** store through a named binding (`r.X := 5`) remains legal. A value kind may not conform to an interface declaring a settable property or indexer at all (§9.6). The sanctioned mediating shape is a chain that hops **through a class- or interface-typed member**, leaving the receiver — that is how `Span<T>`'s indexer setter works.

### 4.5 Iteration and collection ownership

**The two families** (§28.11):

| | Borrowing family | Owning family |
|---|---|---|
| Types | `Array<T>`, `LinkedList<T>`, `Dictionary<K,V>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `SortedDictionary<K,V>`, `SortedSet<T>` | `OwningArray<T>`, `OwningLinkedList<T>`, `OwningDictionary<K,V>`, `OwningSet<T>`, `OwningStack<T>`, `OwningQueue<T>` |
| Element slots at class T | **borrows** — the caller keeps ownership | **owned** |
| `Add`/`Insert`/indexer-set | stores the borrow | `Add(owned item: T)`; indexer-set disposes the previous value |
| `Remove`/`RemoveAt`/`Clear` | drops the borrow | **disposes** the element(s) |
| `Dispose()` | no user-visible work | disposes **every** element, then frees storage |
| Ownership out | *none* | `move coll[i]`, `PopAt`/`PopFirst`/`PopLast`/`Pop`/`Dequeue`, `Drain()` |

There is **no implicit conversion between the families**. Owning collections require `where T: class` — value, `string` and function element types are rejected at the instantiation site. A class owning a collection of resources writes `private owned _items: OwningArray<T>;` and needs no destructor.

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

`OwningArray<T>` implements both `IEnumerable<T>` and `IOwningEnumerable<T>`; **default iteration takes the borrowing path**. To take elements out, express the source at `IOwningEnumerable<T>` — call `.Drain()`.

The owning-path loop variable is a plain owned local, not a `using var`, deliberately, so you can move it on; the discharge rule applies on **every** body-exit edge. Hence the idiom:

```bezel
for var conn in pool.Drain() do
  using var c := move conn;      // adopt on the first line — covers every exit edge
  c.SendShutdown();
end;                             // pool is now an empty container, still disposable
```

Omit the adoption and the element auto-drops each iteration with a **per-iteration must-use warning**. Draining a `using var` collection is fine and leaves it disposable as an empty container.

Two desugaring facts (§29.2): when the source is a **fresh** iterable-family call, the desugaring's hidden `using var` *assumes* the obligation — `for var x in GetThings() do … end;` does not leak the state machine. When the source is already an owned local or a `using var`, the desugaring **pins** instead of taking, so iterating a `using var` collection neither moves out of it nor double-disposes.

**`OwningDictionary<K,V>` has no default entry iteration** — its conformance is `IDisposable` only. Iterate `.Keys` and `PopValue` to take, or use `Drain()` (stdlib §4.9).

**Escaping an element borrow.** Over a **concrete** collection type the source chain is slot → collection → binding, and keeping the reference past the loop is fine while the collection lives. Over an **interface-typed** source the compiler cannot see the backing, so the variable's source is conservatively the loop's own enumerator — a scope-bound local — and any escape is **rejected** (§29.2). Remedy: take the parameter at a concrete collection type, or give the elements an owner that outlives the loop.

**Materializing is not a liveness remedy at class element types** (v0.265). `ToArray()`/`ToSet()`/`ToDictionary(…)` at a class or interface element type build **borrowing** collections, so the materialized slots dangle exactly as the original borrow did. Materialize-first is the **re-enumeration** remedy, over a *live* backing — never a liveness remedy. What does remedy a dead-on-arrival element borrow: consume each element inside the loop body that borrows it, or give the elements an owner (an `IOwningEnumerable<T>` producer, consumed by owning iteration into an owning collection, with the accessor applied to *that* collection). Where the producer is a borrowing minting iterator you cannot change, **no retention remedy exists** (§29.11).

**Lazy queries are single-consumption** (§29.11). A lazy query value may be enumerated at most once. The right is spent by enumeration or a terminal operator, by applying a further deferred operator (`var q2 := q.Select(f);`), or by passing it as an `IEnumerable<T>` argument. This is orthogonal to ownership: the container obligation is discharged by the ordinary rules.

**Element disposal is the owner's, always** (§28.6). `Dispose()` requires a receiver you own: an owned local or `owned` parameter, a `using var`, an unbound owned temporary, a `move` result, or an `owned` field / owning slot read by code of the owning object's declaring type. `Dispose()` through a **borrow is a compile error with no exception** — a borrowed local or parameter, **`this`**, a non-`owned` field read, an owned-field read outside the declaring class, a getter result, a `catch`-arm binding, a pattern binding, a captured owned binding inside a closure, and an element borrow of *either* family. A borrowed **owning collection** may be added to, removed from, drained and read — the pass-and-fill idiom `method Fill(a: OwningArray<T>)  a.Add(…)` is core — but it may not be `Dispose`d.

**Fresh values into collections** (§15.4, §28.11):

- **Owning family — idiomatic and warning-free:** `pool.Add(new Connection("a"));`. The fresh producer is unmarked and the slot takes ownership.
- **Borrowing family, in a collection *literal*:** a fresh-producer element of **non-trivial-dispose** class type is a hard **error** at the literal site. Bind first, or use the owning family.
- **Borrowing family, via `Add`/`Insert`/indexer-set:** admitted, but the compiler **must warn** — the temporary dies at the statement's end and the slot dangles.
- **Runtime-managed elements are unaffected:** `summary.Add(name)` on an `Array<string>` carries no obligation and no warning.

### 4.6 The diagnostics you will actually hit

| Diagnostic | Cause | Fix |
|---|---|---|
| "Owned value in `x` would be dropped here without disposal" (**warning**) | a plain-`var` owned local or an undischarged `owned` parameter reaching a normal scope end | declare the intent: `using var`, `move`/`return` it, `.Dispose()` it, or `_ := x;` |
| mixed owned/borrowed merge, rejected at the join | an `if`/`??`/arm meet where one side is owned and the other borrowed | equalize to both borrowed (sources outliving the merge) or both owned (fresh producers or marked arms) |
| "Borrow held by `x` would outlive its source `y`" | a borrow assigned to a longer-lived binding — reported **at the assignment** | keep the work in the branch where the resource lives; factor into a helper taking a borrowed parameter; or hold the fallback in a field |
| "this call consumes `x`; write `M(move x)`" | a named owned binding at an `owned` parameter, unmarked | add `move` — but never at a target-announced position, on a fresh producer, or on a borrow |
| "Cannot move out of `using var` `x`" | a factory that pinned its result | use plain `var`; it is already exception-safe |
| field moved and not restored on every exit path | `move obj.f` without an assignment on some path | assign on the offending path, move the `move` into the branch, or declare the field nullable and assign `null`; **build before take** |
| a fresh owned value escaping into a non-owning location | `_c := new Cell();` on a borrow field; a getter returning `new Cell()`; `return move _items;` under a plain return | declare `owned` on the field or the return; expose factories as methods |
| a static method's plain class-typed return, rejected at the declaration | `static method Make(): Connection` | `owned Connection`, a mint-announcing type, or a static **property** getter for a cached singleton |
| `Dispose()` on a borrow | `conn.Dispose()` on a loop element; `s.Dispose()` on a borrowed parameter; `this.Dispose()` | `pool.Clear()`/`RemoveAt`, or take the element out first (`Drain()`, `Pop*`, `move coll[i]`), or let the owner dispose |
| "Cannot consume `x` here: the borrow held by `b` is still live" | the consume-site freeze | transfer first, then re-borrow from the new owner |
| warning at `coll.Add(new T(…))` on a **borrowing** collection | the fresh-store lint | bind first or use the owning family |
| warning at `iter().First()` / `.Min()` at class elements | terminal-accessor provenance: the element is enumerator-owned and dead on arrival | consume in the loop, or give the elements an owner — **`ToArray()` does not fix it** |
| `_ := someTask;` rejected | abandoning a running operation is a decision, not a disposal | `Task.FireAndForget(t)` or `using var t := …;` |

---

## 5. The standard-library index

Everything below is transcribed from `Bezel_Standard_Library.md` v0.268 with its markers intact. **A signature with its markers stripped is worse than no signature**: `owned`, `pure`, `ref`, `out`, `scoped`, nullability and constraints are load-bearing. **If a name is not here, look it up in the stdlib document; if it is not there either, it does not exist.**

### 5.0 Modules and imports

| Module | Import | Contents |
|---|---|---|
| `System` | auto | `Console`, `Math`, `Convert`, `Environment`, temporal types, `Guid`, `Random`, the core interfaces, `Type`, the serialization vocabulary, `Shared`/`Weak`, `Result`, `ScopeGuard`, `Subscription`, the FFI carriers |
| `System.Text` | auto | `StringBuilder`, `Encoding`, `ReadOnlyStringSpan` |
| `System.Collections.Generic` | **explicit** | the collections — *except* the iteration interfaces (`IEnumerable`/`IEnumerator`/`IAsyncEnumerable`/`IAsyncEnumerator`/`IOwning*`), which are auto |
| `System.Collections.Linq` | **explicit** | the query operators. Without the import they do not resolve |
| `System.Threading` | **explicit** | *except* `CancellationToken`/`CancellationTokenSource` and the six event types, which are auto |
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
| **(d)** bool only, no out | `TryX(…): bool` | succeeded/declined; the payload was passed *in* | `Set.TryAdd`, `OwningSet.TryAdd`, `SortedSet.TryAdd`, `TaskCompletionSource.TrySet*`, the span/bit `TryWrite*` |

**The `when true` postcondition is normative on every shape-(a) member** even though the per-type listings elide it: inside a `true`-narrowed region the recipient needs no `!`.

**Three access tiers over one operation:** the **throwing** tier (absence is a bug) — `Parse`, `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`, `Console.ReadKey`; the **`*OrNull`** tier, returning `T?` for `??`/`?.` composition — `ParseOrNull`, `GetOrNull`, `Console.ReadKeyOrNull`, `FirstOrNull`/`LastOrNull`/`SingleOrNull`/`ElementAtOrNull`/`MinOrNull`/`MaxOrNull`, `E.FromUnderlyingOrNull`; and the **`Try`** tier, the only shape that distinguishes "absent" from "present null". `*OrNull` **conflates** absent with a present `null` wherever the payload can be null — use `ContainsKey`/`Count`/the `Try` shape when the distinction matters.

**Negative magnitudes fault:** `capacity`/`size`/`length` < 0 raises `ArgumentOutOfRangeException` on the `StringBuilder`, `Array<T>`, `OwningArray<T>`, `Dictionary`/`OwningDictionary`/`Set`/`OwningSet` capacity forms and `OwningBuffer<T>`. Zero is valid. `Channel<T>(capacity)` requires `capacity >= 1`, else `ArgumentException`.

**Naming conventions.** The `Async` suffix marks the async half of a sync/async pair only; async-only operations carry no suffix. Borrowing/owning duals are **same-name overloads**, not `Owned`-suffixed (surviving `Owned` names: `Json.ParseOwned<T>`, `Binary.ParseOwned<T>`, `Task.FromOwnedResult`). **An instance member blocks a same-named extension, by name, at any arity** — on an `Array<T>` receiver, `Contains`, `Count`, `Reverse`, `ToArray` resolve to the *instance* members, so `arr.Count(pred)` does **not** compile; write `arr.Where(pred).Count()`.

**Exception safety when a mutator re-enters user code:** the basic guarantee only. The receiver stays valid and every owned element is still disposed exactly once; contents are otherwise unspecified. No member promises all-or-nothing.

### 5.2 `System` (auto)

```bezel
public static class Console
  public static method Write(value: string);   Write(value: Object);
  public static method WriteLine();  WriteLine(value: string);  WriteLine(value: Object);
  // + per-type Write/WriteLine overloads for bool and every numeric primitive
  public static method ReadLine(): string?;            // null at EOF
  public static method ReadKey(): char;                // EndOfStreamException at end of input
  public static method ReadKeyOrNull(): char?;
  public static readonly property In: TextReader;   Out: TextWriter;   Error: TextWriter;
  public static method Clear();
end

public static class Math
  public static const Pi: double;   E: double;   Tau: double;
  public static pure method Abs(int|long|double|decimal) -> same;
  public static method Min<T>(a: T, b: T): T where T: IComparable<T>;     // NOT pure
  public static method Max<T>(a: T, b: T): T where T: IComparable<T>;     // NOT pure
  public static method Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>;
  public static pure method Sqrt/Exp/Log/Log10/Log2/Sin/Cos/Tan/Asin/Acos/Atan/
                            Sinh/Cosh/Tanh(x: double): double;
  public static pure method Pow(x: double, y: double): double;
  public static pure method Log(x: double, base_: double): double;
  public static pure method Atan2(y: double, x: double): double;
  public static method DivRem(a: int, b: int, out remainder: int): int;   // + long form; NOT pure
  public static pure method Floor/Ceiling/Truncate(x: double): double;
  public static pure method Round(x: double[, digits: int]): double;
  public static pure method Floor/Ceiling/Truncate(x: decimal): decimal;  // no Math.Round(decimal)
  public static pure method Sign(int|long|double|decimal): int;
  public static pure method Fmod(x, y: double): double;   IEEERemainder(x, y: double): double;
end
```

`Min`/`Max`/`Clamp` at a **class** substitution return a **borrow** sourced from the arguments.

```bezel
public static class Convert     // string <-> T only; there is no Convert.ToInt32(double)
  ToInt32/ToInt64/ToDouble/ToDecimal/ToBoolean(value: string);
  TryToInt32/…/TryToBoolean(value: string, out result: T): bool;         // shape (c)
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

**Temporal types — all records (value types).** `DateTime: IComparable<DateTime>, IEquatable<DateTime>` — constructors `(y,m,d)` and `(y,m,d,h,min,s, millisecond := 0, kind := DateTimeKind.Unspecified)`; `Ticks: long`, `FromTicks`, `ToUnixTimeSeconds(): long`, `FromUnixTimeSeconds(seconds: long)`, `Year/Month/Day/Hour/Minute/Second/Millisecond/DayOfYear: int`, `DayOfWeek`, `Kind`, `Date: DateOnly`, `TimeOfDay: TimeOnly`; statics `Now/UtcNow/Today/MinValue/MaxValue`; `AddYears/AddMonths(int)`, `AddDays/AddHours/AddMinutes/AddSeconds/AddMilliseconds(double)`, `Add(span: TimeSpan)`, `Subtract(other: DateTime): TimeSpan`, `Subtract(span: TimeSpan): DateTime`, `ToUtc()`, `ToLocal()`, `ToString()`/`ToString(format)`, `Parse`/`TryParse(out result: DateTime)`/`ParseOrNull`, `CompareTo`; operators `-`(DT,DT)→`TimeSpan`, `-`(DT,TS), `+`(DT,TS), `+`(TS,DT). Parse accepts only `yyyy-MM-ddTHH:mm:ss[.fff]` and `yyyy-MM-dd`. `enum DayOfWeek: byte`, `enum DateTimeKind: byte { Unspecified, Utc, Local }`.

`TimeSpan` — `Ticks: long`, `FromTicks`, `Days/Hours/Minutes/Seconds/Milliseconds: int`, `TotalDays/TotalHours/TotalMinutes/TotalSeconds/TotalMilliseconds: double`, statics `FromDays/FromHours/FromMinutes/FromSeconds/FromMilliseconds(double)`, `Zero/MinValue/MaxValue`, `ToString()`/`ToString(format)`, operators `+ - * /` (`*`/`/` take `double`) and unary `-`.
`DateOnly` — `(y,m,d)`, `Year/Month/Day`, `CompareTo`, `ToDateTime(time: TimeOnly)`, `Parse`/`TryParse`/`ParseOrNull`, `ToString([format])`.
`TimeOnly` — `(hour, minute, second := 0, millisecond := 0)`, `Hour/Minute/Second/Millisecond`, `CompareTo`, `Parse`/`TryParse`/`ParseOrNull`, `ToString([format])`.

`Guid` (record) — `CompareTo`; `static FromByteArray(bytes: Array<byte>): Guid`; `static readonly Empty`; `static NewGuid(): Guid` (**not cryptographic — never a secret**); `Parse`/`TryParse(out result: Guid)`/`ParseOrNull`; `ToString()`; `ToString(format)` (`"N"`/`"D"` only); `ToByteArray(): owned Array<byte>`. **No constructor** — `new Guid(…)` is a compile error.

`Random` (class) — `constructor()`, `constructor(seed: long)`; `Next(): int`, `Next(maxValue)`, `Next(minValue, maxValue)`, `NextInt64(): long`, `NextDouble(): double`, `NextBytes(buffer: Array<byte>)`, `NextBoolean(): bool`.

```bezel
public interface IComparable<T>    method CompareTo(other: T): int;   end
public interface IEquatable<T>     method Equals(other: T): bool;     end
public interface IDisposable       method Dispose();                  end   // every class implicitly implements
public interface IAsyncDisposable  method DisposeAsync(): Task;       end
public interface IFormattable      method ToString(format: string): string;   end
public class Subscription  destructor; end     // no public ctor; returned as `owned Subscription`
```

`IComparable<T>`/`IEquatable<T>` are **invariant**. `BitConverter`: `FloatToInt32Bits`, `Int32BitsToFloat`, `DoubleToInt64Bits`, `Int64BitsToDouble`, `Reinterpret<TTo>(value)`. `Type` (`public sealed class`): `Name`, `FullName`, `IsClass/IsInterface/IsRecord/IsEnum/IsPrimitive/IsInlineArray/IsFunctionType/IsValueType`, `BaseType: Type?`, `Implements(iface)`, `IsAssignableFrom(other)`, `EnumNames`, `EnumValues` — **no member reflection at all** (declined, not deferred).

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
```

Hierarchy: `Exception` → `ContractViolationException`; `ArgumentException` → `ArgumentNullException`, `ArgumentOutOfRangeException`; `ArithmeticException` → `OverflowException`, `DivideByZeroException`; `InvalidOperationException` → `DisposedAccessException`; plus `NotImplementedException`, `NullReferenceException`, `IndexOutOfRangeException`, `MatchFailureException`, `OperationCanceledException`, `TypeInitializationException`, `FormatException`, `StackOverflowException` (uncatchable). **There is no `OutOfMemoryException`** — allocation failure terminates. **No user `InnerException`** — chain with `base(msg, cause)`. Producer-local exceptions need the producer's import: `KeyNotFoundException`, `SerializationException`, `TimeoutException`/`ChannelClosedException`/`ChannelFullException`/`DomainFaultedException`, `IOException`/`FileNotFoundException`/`DirectoryNotFoundException`/`EndOfStreamException`/`PathTooLongException`.

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

**Serialization vocabulary** (auto-imported interfaces): `ISerializer` (`WriteInt`, `WriteUInt`, `WriteFloat`, `WriteDecimal`, `WriteBool`, `WriteString`, `WriteChar`, `WriteNull`, `BeginObject(fieldCount)`, `WriteFieldName`, `EndObject`, `BeginArray(elementCount)`, `EndArray`), `IDeserializer` (`ReadInt`, `ReadUInt`, `ReadFloat`, `ReadDecimal`, `ReadBool`, `ReadString`, `ReadChar`, `TryReadNull`, `BeginObject(): int`, `ReadFieldName`, `SkipValue`, `EndObject`, `BeginArray(): int`, `HasMoreElements`, `EndArray`), `ISerializable` (`Serialize(s: ISerializer)`), `IDeserializable` (a **marker**: the compiler recognizes `static method Deserialize(d: IDeserializer): T` / `: owned T` / `: owned Shared<C>` by name). Records and enums declaring `: ISerializable, IDeserializable` get compiler-synthesized bodies; `serialized "<name>" | skip | optional` customizes. `$id`/`$ref`/`$type`/`$case` are reserved field names. Concrete formats (`import System.Serialization;`): `JsonSerializer`/`JsonDeserializer`/`BinarySerializer`/`BinaryDeserializer`, the `JsonOptions` record, and the one-shot facades `Json.ToText<T>`/`Json.ToBytes<T>`/`Json.Parse<T>`(value targets only)/`Json.ParseOwned<T>` (`where T: class`) and `Binary.ToBytes<T>`/`Binary.Parse<T>`/`Binary.ParseOwned<T>`.

**Static-interface abstractions:** `INumber<T>` (`static Zero()`, `static One()`, operators `+ - * /`, `<`), `ISteppable<T>` (`static Step(value: T, by: long): owned T`, `<`), `IParsable<T>` (`static Parse(s: string): owned T`, `static TryParse(s: string, out owned value: T?): bool`). Conformers: the ten numeric primitives conform to all three; `char` conforms to `ISteppable`/`IParsable`; **`string` and `bool` conform to none**.

### 5.3 `System.Text` (auto)

`StringBuilder` — `constructor()`, `(capacity: int)`, `(initial: string)`; `Length: int`, `pure Capacity: int`; `Append(string|char|Object|bool|int|byte|sbyte|short|ushort|uint|long|ulong|float|double|decimal|ReadOnlyStringSpan): StringBuilder`; `AppendLine()`, `AppendLine(value: string)`; `AppendFormat(format: string, args: Array<Object>)`; `Insert(index, value: string)`, `Remove(startIndex, length)`, `Replace(oldValue, newValue: string)`, `Clear()` — all returning `StringBuilder`; `ToString(): string`.

`Encoding` (`public abstract class`) — `abstract GetBytes(s: string): owned Array<byte>`; `abstract GetString(bytes: Array<byte>)`, `GetString(bytes, index, count)`, `GetString(bytes: ReadOnlySpan<byte>)`, `GetString(bytes: ReadOnlyArraySpan<byte>)`; `abstract GetByteCount(s: string): int`; `abstract GetBytes(s: string, destination: Span<byte>): int`; `abstract GetByteCount(s: ReadOnlyStringSpan): int`; `abstract GetBytes(s: ReadOnlyStringSpan, destination: Span<byte>): int`; `abstract GetBytes(s: ReadOnlyStringSpan): owned Array<byte>`; `abstract readonly property Name: string`; `static readonly property UTF8, UTF16, UTF32, ASCII: Encoding`. Malformed input raises `FormatException`, **never** a replacement character. UTF8 emits no BOM; UTF16/UTF32 are little-endian only.

`ReadOnlyStringSpan` — `public record (Source: string, Start: int, Length: int): IReadOnlySpan<char>`; **ordinary record, not borrow-bearing**. `property [i: int]: char { get }`; `pure IsEmpty: bool`; `pure Slice(start, length) / Slice(start) / Slice(r: Range<int>)`; `pure IndexOf(char|ReadOnlyStringSpan|string): int`; `pure Contains(…): bool`; `pure StartsWith`/`EndsWith(ReadOnlyStringSpan|string): bool`; `pure SequenceEqual(ReadOnlyStringSpan|string|IReadOnlySpan<char>): bool`; `CopyTo(destination: Span<char>)` (**not pure**); `pure ToString(): string`. `ReadOnlyAsciiByteSpan` was **retired** and does not exist.

### 5.4 `System.Collections.Generic` (explicit import)

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
  method IndexOf(item: T): int;  IndexOf(item: T, startIndex: int): int;  LastIndexOf(item: T): int;
  method Sort(comparer: (T, T) -> int);   method Reverse();                        // in place
  method Sorted(comparer: (T, T) -> int): owned Array<T>;   Reversed(): owned Array<T>;
  method ToArray(): owned Array<T>;
  method AsSpan([start[, length]] | r: Range<int>): ArraySpan<T>;
  method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlyArraySpan<T>;
  method Slice(r: Range<int>): owned Array<T>;       // `arr[r]` is the compiler-recognized form
end
public extension Array<T> where T: IComparable<T>   method Sort();  Sorted(): owned Array<T>;  end
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
  method Sort(comparer: (T, T) -> int);   Reverse();     // the comparer takes borrows
  method PopAt(index: int): owned T;   PopFirst(): owned T;   PopLast(): owned T;   // raise if empty
  method TryPopFirst(out owned item: T?): bool;   TryPopLast(out owned item: T?): bool;
  method Drain(): IOwningEnumerable<T>;
end
public extension OwningArray<T> where T: class, IComparable<T>   method Sort();  end
```

There is **no `TryPopAt`**, and `OwningArray<T>` declares **no `Reversed`, no `Sorted`, no `ToArray`** — an *owning* copy is inexpressible. Reorder in place with `Reverse()`/`Sort()`; move elements to a new owning collection with `Drain()`. `move arr[i]` takes an element out, leaving the slot moved-from (reassign it before the next exit point). `Drain()` mints a fresh drain-view frame — disposing the frame does not dispose the collection, and partial iteration leaves the remainder.

The §5 extension `ToArray()` **is** reachable on an `OwningArray<T>` receiver (it is not declared by name on the class) and returns `owned Array<T>` — a **borrowing snapshot** whose slots hold borrows of elements the owning array still owns. No duplicate ownership; the obligation is a *lifetime* one: the source must outlive every use, and `Clear`/`Remove`/`RemoveAt`/an indexer-set kills what a snapshot slot points at. Same for `ToSet()`, `ToDictionary(…)` and every other §5 operator on an owning receiver.

```bezel
public class LinkedList<T>: ICollection<T>
  constructor();  constructor(source: IEnumerable<T>);
  pure readonly property Count: int;  FirstNode: LinkedListNode<T>?;  LastNode: LinkedListNode<T>?;
  pure readonly property First: T?;   Last: T?;
  method AddFirst/AddLast(item: T): LinkedListNode<T>;
  method AddBefore/AddAfter(node: LinkedListNode<T>, item: T): LinkedListNode<T>;
  method RemoveFirst();  RemoveLast();  RemoveNode(node);  Find(item: T): LinkedListNode<T>?;
end
public class LinkedListNode<T>
  public readonly property Value: T;  Next: LinkedListNode<T>?;  Previous: LinkedListNode<T>?;
end
public class OwningLinkedList<T>: IOwningCollection<T> where T: class
  constructor();                                     // NO source constructor — deliberate
  // same shape with `owned item` on AddFirst/AddLast/AddBefore/AddAfter; Remove*/Clear dispose;
  method PopFirst(): owned T;   PopLast(): owned T;
  method TryPopFirst/TryPopLast(out owned item: T?): bool;   Drain(): IOwningEnumerable<T>;
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
// class-typed T:  public class OwningBuffer<T>: IEnumerable<T?>
constructor(length: int);                       // all slots null
constructor(length: int, init: (int) -> owned T);
readonly property Length: int;                  // immutable after construction
property [i: int]: T? { get;  set(owned); }     // set disposes previous;  `move buf[i]` ADMITTED

// value-typed T:  public class OwningBuffer<T>          (NO IEnumerable conformance)
constructor(length: int);                       // all slots default(T); T must be defaultable
constructor(length: int, init: (int) -> T);
readonly property Length: int;   property [i: int]: T { get; set; }    // `move buf[i]` NOT admitted
method DangerousPointer(): Ptr<T>;              // value-T only

// both:
method AsSpan([start[, length]] | r: Range<int>): Span<T>;
method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlySpan<T>;

public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int): IReadOnlySpan<T?>   // class T
  property [i: int]: T? { get; set(owned); }    property IsEmpty: bool;
  method IndexOf(value: T?): int;   Contains(value: T?): bool;
  method SequenceEqual(other: Span<T> | IReadOnlySpan<T?>): bool;
  method Slice(start, length) / Slice(start) / Slice(r: Range<int>): Span<T>;
  method AsReadOnly(): ReadOnlySpan<T>;   method Clear();      // Clear disposes previous contents
  method MoveFrom(source: Span<T>);   MoveTo(destination: Span<T>);   method Reverse();
end
public record Span<T>(…): IReadOnlySpan<T>      // value T — same read surface, plus:
  method Fill(value: T);  CopyFrom(source: ReadOnlySpan<T>);  CopyTo(destination: Span<T>);  Reverse();
end
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // read surface + Slice x3; value T only: CopyTo(destination: Span<T>), ToArray(): owned Array<T>
end
public record ArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>
  property [i: int]: T { get; set; }            // the setter stores a BORROW; `move` NOT admitted
  property IsEmpty: bool;  Slice x3;  AsReadOnly(): ReadOnlyArraySpan<T>;
  IndexOf(value: T): int;  Contains(value: T): bool;  SequenceEqual(…): bool;
  Fill(value: T);  CopyFrom(source: ReadOnlyArraySpan<T>);  CopyTo(destination: ArraySpan<T>);  Reverse();
end
public record ReadOnlyArraySpan<T>(Source: Array<T>, Start: int, Length: int): IReadOnlySpan<T>
  // read surface + CopyTo(destination: ArraySpan<T>).  No ToArray() here.
end
public interface IReadOnlySpan<TElement>        // implemented by all five span types
  readonly property Length: int;   readonly property IsEmpty: bool;
  property [i: int]: TElement { get }
  method IndexOf(value: TElement): int;   Contains(value: TElement): bool;
  method SequenceEqual(other: IReadOnlySpan<TElement>): bool;
end                                             // `Slice` is deliberately NOT on the interface
```

`Span<T>`/`ReadOnlySpan<T>` are **borrow-bearing records** bounded by the buffer's lifetime; never an `async` method's parameter or result. **There is no implicit `Span<T>` → `ReadOnlySpan<T>` conversion** — call `.AsReadOnly()`. Spans do **not** implement `IEnumerable<T>`; `for` over a span is a direct indexed loop. `Span<T>` and `ArraySpan<T>` are **not interchangeable**. The `AsSpan`/`AsReadOnlySpan` factories and `Slice(r: Range<int>)` validate eagerly (`IndexOutOfRangeException`); direct constructors and `Slice(start[, length])` do not. **There is no writable-span interface.**

**Byte accessors** (`public extension` on `ReadOnlySpan<byte>`/`Span<byte>`, mirrored on `ReadOnlyArraySpan<byte>`/`ArraySpan<byte>`): `Read{Int16,UInt16,Int32,UInt32,Int64,UInt64,Float,Double}{LittleEndian,BigEndian}()` and the matching `TryRead…(out value: T): bool`; on the writable spans additionally `Write…{LittleEndian,BigEndian}(value: T)` and `TryWrite…(value: T): bool`. **Endianness is in the name — there is no platform-default form, no enum parameter, and no offset parameter; slice first.** A short span raises `IndexOutOfRangeException`; a false `TryWrite*` leaves the span unmodified.

**Bit tier** (v0.264): `BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)` and `BitWriter(Source: Span<byte>, …)`, mirrored as `ArrayBitReader`/`ArrayBitWriter` over the array spans. Members: `pure IsEmpty`, `pure IsByteAligned`, `ReadBitsBigEndianMsbFirst(count: int): ulong`, `ReadBitsLittleEndianLsbFirst(count: int): ulong`, `ReadBitMsbFirst()`/`ReadBitLsbFirst(): bool`, the four matching `TryRead…(…, out value): bool`, `pure Slice(bitStart)`/`Slice(bitStart, bitLength)`/`Slice(r: Range<int>)`, `pure AlignToByte()`, `AlignedTail()`; the writer swaps reads for `WriteBits…(value: ulong, count: int)`/`WriteBit…(value: bool)`/`TryWrite…` and adds `AsReadOnly(): BitReader`. Entry points: `pure AsBitReader()` on the read-only spans, `AsBitReader()` (**not pure**) and `pure AsBitWriter()` on the writable ones. Only **two** of the four bit-order × byte-order combinations exist. Every multi-bit read returns `ulong` and every multi-bit write takes `ulong`; **no signed form**. A reader does not advance itself — slice to advance. Both records are borrow-bearing.

```bezel
public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>> where K: IComparable<K>
  constructor();   pure readonly property Count: int;
  property [key: K]: V;                          // get: KeyNotFoundException;  set: insert or replace
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
  method ContainsKey(key: K): bool;              // distinguishes present-at-zero from absent
  method Remove(key: K): bool;   Clear();
  pure readonly property Count: int;             // distinct keys
  pure readonly property Total: int;             // sum of counts; may be negative
end
```

The sorted collections take **no comparer parameter** — ordering is `K`/`T`'s own `IComparable`. `SortedDictionary` has no `Keys`/`Values`. **There is no `OwningSortedDictionary`/`OwningSortedSet`, no `FrozenDictionary`/`FrozenSet`.** `Counter<K>` is insertion-ordered and a key decremented to zero is not removed.

### 5.5 `System.Collections.Linq` (import required)

```bezel
public extension IEnumerable<T>
  // Filtering / projection (LAZY)
  Where(predicate: (T) -> bool): IEnumerable<T>;      OfType<U>(): IEnumerable<U>;
  Select<U>(selector: (T) -> U): IEnumerable<U>;
  SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>;
  // Quantifiers and element access (EAGER)
  Any(): bool;   Any(predicate): bool;   All(predicate): bool;
  Count(): int;  Count(predicate): int;  Contains(value: T): bool;
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
  // Set-like (LAZY; require hashable T)
  Distinct(): IEnumerable<T>;  Union(other); Intersect(other); Except(other): IEnumerable<T>;
  // Slicing (LAZY; counts CLAMP)
  Take(count: int); TakeWhile(predicate); Skip(count: int); SkipWhile(predicate): IEnumerable<T>;
  // Combination and grouping (LAZY)
  Concat(other: IEnumerable<T>): IEnumerable<T>;
  Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;   // ends when either ends
  GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>>;
  // Materialization (EAGER)
  ToArray(): owned Array<T>;
  ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T>;
  ToDictionary<TKey, TValue>(key: (T) -> TKey, value: (T) -> TValue): owned Dictionary<TKey, TValue>;
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

Empty source: `Sum` → the zero of its result type; `Min`/`Max`/`Average` → `InvalidOperationException`; `Aggregate` → `seed`; `First`/`Last`/`Single` → `InvalidOperationException`; `ElementAt` → `IndexOutOfRangeException`; `*OrNull` → `null`. **No `MinOrNull`/`MaxOrNull` selector overloads** — compose `Select(sel).MinOrNull()`. `ToDictionary` raises `ArgumentException` on a duplicate key. Lazy operators **retain their callbacks** until enumeration; eager operators are call-bounded. `Sum` returns the element's own type and is overflow-checked in it (`bytes.Sum()` past 255 raises `OverflowException` — widen in the selector); `Average` keeps its per-type overloads.

**At class element types**, `First`/`Last`/`Single`/`ElementAt` (and their `*OrNull` forms), `Min()`/`Max()`, `MinOrNull()`/`MaxOrNull()` and `Min<U>`/`Max<U>(selector)` return **borrows sourced from the receiver** — no disposal obligation, valid only while the element's backing lives; `Aggregate` additionally carries `seed` as a source. **`ToArray()`/`ToSet()`/`ToDictionary(…)` at a class element type return BORROWING collections** — a new container, never a new owner.

### 5.6 `System.Threading`

```bezel
public record CancellationToken                       // auto-imported; borrow-bearing
  public static readonly property None: CancellationToken;
  public readonly property IsCancellationRequested: bool;
  public method ThrowIfCancellationRequested();
  public method Register(scoped callback: () -> ()): owned Subscription;
end
public class CancellationTokenSource                   // auto-imported
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
  method TryAcquire(timeout: TimeSpan): owned Lock?;
  async method AcquireAsync(token := CancellationToken.None): OwningTask<Lock>;
  async method TryAcquireAsync(timeout: TimeSpan, token := CancellationToken.None): OwningTask<Lock?>;
end
public class Lock  destructor;  end                    // no public ctor; NOT sendable
public class Semaphore
  constructor(initialCount: int);   constructor(initialCount: int, maxCount: int);
  method Acquire(): owned SemaphoreToken;   TryAcquire(timeout): owned SemaphoreToken?;
  async method AcquireAsync(token := …): OwningTask<SemaphoreToken>;
  async method TryAcquireAsync(timeout, token := …): OwningTask<SemaphoreToken?>;
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
public interface IEvent<out T>        method Subscribe(scoped handler: (T) -> ()): owned Subscription;   end
public interface IAsyncEvent<out T>   method Subscribe(scoped handler: (T) -> Task): owned Subscription; end
public class EventSource<T>: IEvent<T>
  constructor();   method Subscribe(scoped handler: (T) -> ()): owned Subscription;
  method Raise(payload: T);   method Add(scoped handler: (T) -> ());   method Remove(handler: (T) -> ());
  destructor;
end
public class AsyncEventSource<T>: IAsyncEvent<T>       // + async method RaiseAsync(payload: T): Task;
public interface ISharedEvent<out T>  method Subscribe(handler: (T) -> ()): owned Subscription;  end
public sealed class SharedEventSource<T>: ISharedEvent<T>       // handlers must be CAPTURE-FREE
public class TaskCompletionSource<T>                   // NON-class T; class T is a compile error
  constructor();   readonly property Task: Task<T>;    // the property is a BORROW of the source's storage
  method SetResult(result: T);   SetException(owned exception: Exception);   SetCancelled();
  method TrySetResult(result: T): bool;  TrySetException(owned …): bool;  TrySetCancelled(): bool;
  destructor;
end
public class OwningTaskCompletionSource<T> where T: class    // SetResult(owned result: T); Task: OwningTask<T>
public class TaskCompletionSource                            // void-completion form
public class Task
  static method FromResult<T>(value: T): Task<T>;                        // NON-class T
  static method FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class;
  static method FromException<T>(owned exception: Exception): Task<T>;   FromCancelled<T>(): Task<T>;
  static method FromException(owned exception: Exception): Task;         FromCancelled(): Task;
  static method Delay(milliseconds: int[, token]): Task;   Delay(duration: TimeSpan[, token]): Task;
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
  async method LockAsync(token := …): OwningTask<LockGuard<T>>;
  async method TryLockAsync(timeout, token := …): OwningTask<LockGuard<T>?>;
end
public class LockGuard<T>   public readonly property Value: T;   end     // borrow; source = this guard
public class RwLocked<T> where T: class                // Read/TryRead/ReadAsync/TryReadAsync +
  constructor(owned value: T);                         //   Write/TryWrite/WriteAsync/TryWriteAsync
end                                                    //   yielding ReadGuard<T> / WriteGuard<T>
public static class Domain
  Spawn(entry: () -> ()): owned Task;                                  // 18 rows total; the entry MUST be
  Spawn<A>(entry: (owned A) -> (), owned arg: A): owned Task           //   a non-capturing lambda or a
                                          where A: sendable, class;    //   static method reference, and
  Spawn<A>(entry: (A) -> (), arg: A): owned Task where A: sendable, notnull;   // takes EXACTLY ONE argument
  Spawn<R>(entry: () -> owned R): owned OwningTask<R> where R: sendable, class;
  Spawn<R>(entry: () -> R): owned Task<R> where R: sendable, notnull;
  // + the (owned A)/(A) argument rows for each, + the nine async-entry rows
end
public static class Parallel                           // callbacks are SPAWN POSITIONS (capture-free)
  public static readonly property DefaultDegree: int;
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

`Acquire`/`TryAcquire` **block** and are a compile error inside `async` bodies — use the `Async` forms. `using var cts := CancellationTokenSource.WithCancelAfter(delay);` **is** a cancellation scope; a `WithCancelAfter` result must be bound or it cancels at the end of the statement. A *published* source (behind `Shared<…>`, in a `static readonly` field, or linked to a published parent) is **signal-only** — `Register` on it raises `InvalidOperationException`. `TimeoutException` is **not raised by any stdlib operation** — the library's timeouts return `null`. **`FireAndForget` is the only supported way to discard a `Task`**; the other legal form is `using var t := DoAsync();`. `Channel<T>` has **no `TrySend`, no `Receive(token)`, no select/`ReceiveAny`, no receiver cloning, no capacity-0 rendezvous**; at most one consumption may be in progress. `Parallel`'s `combiner` must be associative, `seed` is incorporated once per participating worker, results come back in input order, and the first fault surfaces as `DomainFaultedException`; there are **no async bodies, no class-element `Reduce`, and no `CancellationToken` parameter**.

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
  async virtual method ReadAsync(buffer: Array<byte>, offset: int, count: int, token := …): Task<int>;
  async virtual method WriteAsync(buffer: Array<byte>, offset: int, count: int, token := …): Task;
  method ReadAll(): owned Array<byte>;
  async method ReadAllAsync(token := CancellationToken.None): OwningTask<Array<byte>>;
end
public enum SeekOrigin: byte  Begin, Current, End;  end
public class FileStream: Stream
  constructor(path: string, mode: FileMode);            // implies FileAccess.ReadWrite
  constructor(path: string, mode: FileMode, access: FileAccess);
  public readonly property Path: string;   destructor;  // flushes + closes
end
public enum FileMode: byte    CreateNew, Create, Open, OpenOrCreate, Truncate, Append;  end
public enum FileAccess: byte  Read, Write, ReadWrite;  end
public static class File
  ReadAllText(path: string): string;              WriteAllText(path: string, contents: string);
  ReadAllBytes(path: string): owned Array<byte>;  WriteAllBytes(path: string, contents: Array<byte>);
  WriteAllBytes(path: string, contents: ReadOnlySpan<byte>);
  ReadAllLines(path: string): owned Array<string>;  WriteAllLines(path: string, lines: IEnumerable<string>);
  async ReadAllTextAsync(path: string, token := …): Task<string>;
  async WriteAllTextAsync(path: string, contents: string, token := …): Task;
  Exists(path: string): bool;   Delete(path: string);
  Copy(source: string, destination: string, overwrite: bool);   Move(source: string, destination: string);
end
public abstract class TextReader
  abstract method Read(): int;              // the next char as int, or -1 at EOF
  abstract method ReadLine(): string?;      // null at EOF
  method ReadToEnd(): string;   destructor;
end
public abstract class TextWriter
  abstract method Write(value: string);   abstract method Write(value: char);
  method Write(value: Object);  WriteLine();  WriteLine(value: string);  WriteLine(value: Object);
  abstract method Flush();   destructor;
end
public static class Directory
  Exists(path: string): bool;   CreateDirectory(path: string);
  Delete(path: string);         Delete(path: string, recursive: bool);
  GetFiles(path: string): owned Array<string>;   GetDirectories(path: string): owned Array<string>;
end
public static class Path
  Combine(a: string, b: string): string;   GetFileName(path): string;   GetExtension(path): string;
  GetFileNameWithoutExtension(path): string;   GetDirectoryName(path): string;   GetFullPath(path): string;
end
```

`IOException` ← `FileNotFoundException`, `DirectoryNotFoundException`, `EndOfStreamException`, `PathTooLongException`. `Stream.Read` signals end-of-stream with a **`0` return, never `EndOfStreamException`**, and may return **fewer than `count`** — a short read is not EOF. `File`'s text methods are UTF-8, strict-fault, no BOM. The **only** concrete `TextReader`/`TextWriter` instances are what `Console.In`/`Out`/`Error` return.

### 5.8 `System.Diagnostics`

`Stopwatch` — `constructor()`, `static method StartNew(): owned Stopwatch`; `IsRunning: bool`, `Elapsed: TimeSpan`, `ElapsedMilliseconds: long`; `Start()`, `Stop()`, `Restart()`, `Reset()`. `Debug` — `Assert(condition: bool)`, `Assert(condition: bool, message: string)`, `WriteLine(message: string)`. **`Debug.Assert` terminates the process; it never throws**, and calls may be elided in release builds.

### 5.9 Primitives

The integer types (`Int32`, `Int64`, `Byte`, `SByte`, `Int16`, `UInt16`, `UInt32`, `UInt64`) carry: `static const MinValue/MaxValue`; `static pure Parse(s: string)`, `static TryParse(s: string, out result: T): bool`, `static pure ParseOrNull(s: string): T?`, `static pure Parse(s: string, radix: int)` (radix ∈ {2,8,10,16}), `static TryParse(s: string, radix: int, out result: T): bool`, `static pure Parse(s: ReadOnlyStringSpan)`, `static TryParse(s: ReadOnlyStringSpan, out result: T): bool`; `pure ToString()`, `pure ToString(format: string)`, `pure ToString(radix: int)`, `pure CompareTo(other)`. **`ParseOrNull` has no radix or span duals.** No whitespace tolerance anywhere.

`Double`/`Float`: `MinValue MaxValue Epsilon PositiveInfinity NegativeInfinity NaN`; `static pure IsNaN/IsInfinity/IsPositiveInfinity/IsNegativeInfinity/IsFinite`; `Parse`/`TryParse`/`ParseOrNull` (string and `ReadOnlyStringSpan`); `static pure IeeeEquals(a,b)`, `static pure BitwiseEquals(a,b)`; `pure ToString([format])`, `pure CompareTo`. `Epsilon` is the smallest positive subnormal, not machine epsilon.
`Decimal`: `MinValue MaxValue Zero One`; `Parse`/`TryParse`/`ParseOrNull`; `pure ToString([format])`, `pure CompareTo`, `pure Round(digits: int)` (banker's, 0..28).
`Boolean`: `static pure Parse(s)` ("true"/"false", case-insensitive), `static TryParse(s, out result: bool)`, `static pure ParseOrNull(s): bool?`, `pure ToString()` → `"True"`/`"False"`.
`Char`: `MinValue MaxValue`; `static pure IsDigit/IsLetter/IsLetterOrDigit/IsWhiteSpace/IsUpper/IsLower/IsControl/IsPunctuation(c)`; `static pure ToUpper/ToLower(c): char`; `Parse`/`TryParse`/`ParseOrNull`; `pure ToString()`, `pure CompareTo`.

```bezel
public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>
  pure readonly property Length: int;   IsEmpty: bool;   IsAscii: bool;
  pure readonly property [i: int]: char;      pure readonly property [r: Range<int>]: string;
  static pure From(value: char): string;   static pure From(buffer: Array<char>, start: int, count: int): string;
  static From(value: CString|WString): string;   static FromCStringPointer/FromWStringPointer(p: IntPtr): string;
  static pure Repeat(c: char, count: int): string;    static pure Repeat(unit: string, count: int): string;
  static Join(separator: string, items: IEnumerable<string>): string;
  static pure IsNullOrEmpty(value: string?): bool;    static pure IsNullOrWhiteSpace(value: string?): bool;
  pure Contains(string|char): bool;  pure StartsWith(string): bool;  pure EndsWith(string): bool;
  pure IndexOf(char|string[, startIndex: int]): int;   pure LastIndexOf(char|string): int;
  pure Substring(startIndex[, length]): string;
  pure AsSpan([start[, length]] | r: Range<int>): ReadOnlyStringSpan;
  pure Trim() / Trim(chars: IEnumerable<char>) / TrimStart() / TrimEnd() / ToUpper() / ToLower(): string;
  pure Replace(oldChar: char, newChar: char) / Replace(oldValue: string, newValue: string): string;
  pure PadLeft/PadRight/PadCenter(totalWidth: int[, paddingChar: char]): string;
  pure Insert(startIndex: int, value: string): string;   pure Remove(startIndex[, count]): string;
  method Split(separator: char): owned Array<string>;
  method Split(separator: string): owned Array<string>;
  method Split(separators: Array<char>): owned Array<string>;
  pure CompareTo(other: string): int;   static pure Compare(a, b[, ignoreCase: bool]): int;
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

`IntPtr`/`UIntPtr` (records): `static const Zero`, `operator =`, `operator <>`; opaque, never dereferenced. `CString` (class): `static From(s: string): owned CString`, `static FromBytes(bytes: Array<byte>): owned CString`, `Length: int`, `Pointer: IntPtr`, `ToBytes(): owned Array<byte>`, `destructor`. `WString`: `static From(s: string): owned WString`, `Length`, `Pointer`, `destructor`. `RawBytes`: `static From(buffer: Array<byte>): owned RawBytes`, `static FromForeign(p: IntPtr, length: int, freeOnDispose: bool := false): owned RawBytes`, `Length`, `Pointer`, `CopyTo(destination: Array<byte>, destOffset: int)`, `destructor`. None has a public constructor. `System.Interop`: `Ptr<T>` (record: `Zero`, `FromIntPtr`, `ToIntPtr`, `Load`, `Store`, `LoadAt`, `StoreAt`, `Add`, `CopyToArray(count): owned Array<T>`, `CopyFromArray(source: Array<T>)`, `=`, `<>`) and `Interop` (`static LastErrno(): int`, `static CodeAddress<F>(callback): IntPtr`).

### 5.11 What the standard library does **not** have

Every absence below was verified by a full-document search. Reaching for one of these is the failure this document exists to prevent.

**I/O.** `MemoryStream` (→ `OwningBuffer<byte>` + spans; for serializers, `JsonSerializer.ToBytes()`); `File.Open`/`OpenRead`/`OpenWrite`/`OpenText`/`AppendAllText`/`AppendAllLines` (→ `new FileStream(path, mode[, access])`, `FileMode.Append`); `File.ReadAllBytesAsync`/`ReadAllLinesAsync`/`WriteAllBytesAsync` (the async `File` surface is **text-only** → `FileStream.ReadAllAsync`/`ReadAsync`/`WriteAsync`); `StreamReader`/`StreamWriter`/`BufferedStream`; async `TextReader`/`TextWriter`; async **span** overloads on `Stream`; `Directory.Move`/`EnumerateFiles`/globbing; `Path.ChangeExtension`/`HasExtension`/`GetTempPath`/`DirectorySeparatorChar`; `Console.Read()`.

**Query operators.** `ToList`, `ToLookup`, `Join`, `GroupJoin`, `Cast<T>`, `AsEnumerable`, `DefaultIfEmpty`, `Prepend`, `Append`, `Chunk`, `DistinctBy`, `UnionBy`/`IntersectBy`/`ExceptBy`, `MinBy`, `MaxBy`, `Order()`/`OrderDescending()`, `TakeLast`, `SkipLast`, `LongCount`, `Enumerable.Empty`/`Range`/`Repeat` (**there is no `Enumerable` class at all**), `SequenceEqual` over `IEnumerable<T>` (it exists only on the span types), `SelectMany` with a result selector, `Zip` without a combiner, `Where`/`Select` with an index. Replacements: `ToList` → `ToArray()`; `MinBy(f)` → `OrderBy(f).First()`; `Enumerable.Range` → the language's `1..n`, which is `IEnumerable<T>` (`(1..100).Sum()` works); `Append`/`Prepend` → `Concat` with a one-element `Array<T>`.

**Collections.** `List<T>` (reserved name), `HashSet<T>` (→ `Set<T>`), `SortedList`, `PriorityQueue`, `Deque`, `ConcurrentDictionary`/`ConcurrentQueue`/`ConcurrentBag` (→ `Locked<T>` over a sendable collection, or `Channel<T>`), `ImmutableArray`/`ImmutableList`/`ImmutableDictionary` (→ `FrozenArray<T>`), `ReadOnlyDictionary`, `BitArray`, `Lookup`, `KeyValuePair` (→ `DictionaryEntry<K,V>`), `Tuple`/`ValueTuple` (**Bezel has no tuples** — use a record), `Nullable<T>` (→ `T?`), `IComparer<T>` (only `IEqualityComparer<T>` exists), `StringComparison`/`StringComparer`. Absent *members* on types that do exist: `Array<T>` has no `RemoveAll`, `FindIndex`, `FindAll`, `Find`, `Exists`, `BinarySearch`, `CopyTo`, `TrueForAll`, `GetRange`, `InsertRange`, `RemoveRange`, `ForEach`; `Dictionary` has no `GetValueOrDefault` (→ `GetOrNull`) and no `TryAdd`; `OwningArray<T>` declares no `ToArray`/`Sorted`/`Reversed`; `OwningLinkedList<T>` has no source constructor; `Stack`/`Queue` have no `Add`/`Remove`/`IsReadOnly`.

**Text.** `string.Format`/`String.Format` (**use interpolation**), `String.Empty` (use `""`), `string.Concat`, `Split` with a remove-empty option, `Regex`, `ToTitleCase`, `Normalize`, the `StringBuilder.Length` setter, the `C` format specifier, any culture-aware formatting.

**Concurrency.** `Thread`, `ThreadPool`, `Monitor`, a `lock` statement (→ `Mutex`/`Locked<T>`), `Interlocked` (→ `Atomic<T>`), `ManualResetEvent`/`AutoResetEvent`/`CountdownEvent`/`Barrier`, `Task.Run` (→ `Domain.Spawn`), `Task.CompletedTask` (→ `Task.FromResult`), `Task.Yield`, `ConfigureAwait`, `Task.Wait`/`.Result` inside async code, `AggregateException` (first fault wins; **no aggregation**), `Parallel.For`.

**Everything else.** `Object.MemberwiseClone` and any clone facility, `ICloneable`, member reflection (**declined, not deferred**), attributes, `Activator`, `Lazy<T>`, `WeakReference` (→ `Weak<T>`), `Option<T>` (→ `T?`), `Result.Unwrap`/`UnwrapOrElse`, `Math.Round(decimal)` (→ `Decimal.Round`), `Math.Cbrt`/`Hypot`/`BigMul`, `Random.Shared`, cryptographic randomness, a `Guid` constructor, `Environment.StackTrace`/`CurrentDirectory`/`TickCount`, `Convert` from any non-`string` source, `Encoding.GetEncoding(name)`/`Encoding.Default`, `OutOfMemoryException`, `InnerException` on user exception types, multidimensional arrays, `defer`/`errdefer` (→ `ScopeGuard`).

### 5.12 The `pure` inventory

A `requires`/`ensures` clause may call **only pure members** (§22.16), so this list is the contract-writing vocabulary. Note the distinction: some members **carry the `pure` marker**; others are **pure by classification** (stored-slot reads) and carry no marker and permit none — `Range<T>`'s `Start`, `End`, `Step` and `IsEndInclusive` are the canonical case, which is why `requires(r.Step > 0)` compiles although no `pure` is written on `Step`.

- **`Math`:** `Abs`, `Sqrt`, `Pow`, `Exp`, `Log` (both arities), `Log10`, `Log2`, `Sin Cos Tan Asin Acos Atan Atan2 Sinh Cosh Tanh`, `Floor Ceiling Round Truncate` (double), `Floor Ceiling Truncate` (decimal), `Sign`, `Fmod`, `IEEERemainder`. **Not pure:** `Min`, `Max`, `Clamp` (bare-`T` borrows), `DivRem` (declares an `out`).
- **Collections:** `IReadOnlyCollection<out T>.Count` (an interface slot — every conformer must be pure); `Array<T>`/`OwningArray<T>` `Count`/`Capacity`; `LinkedList`/`OwningLinkedList` `Count`/`FirstNode`/`LastNode`/`First`/`Last`; `Dictionary`/`OwningDictionary`/`SortedDictionary` `Count`; `SortedDictionary.FirstKey`/`LastKey`; `SortedSet` `Count`/`First`/`Last`; `Counter<K>` `Count`/`Total`; `Peek()` on all four stack/queue types. **Not pure, deliberately:** every `Contains`/`ContainsKey`/`CountOf`/`GetOrNull` (they invoke user `Equals`/`GetHashCode`/comparers) and every span, buffer and array **indexer**.
- **`StringBuilder`:** `Capacity` only.
- **`ReadOnlyStringSpan`:** `IsEmpty`, `Slice` ×3, `IndexOf` ×3, `Contains` ×3, `StartsWith` ×2, `EndsWith` ×2, `SequenceEqual` ×3, `ToString()`. **Not pure:** its indexer, `CopyTo`.
- **Bit tier:** on each of `BitReader`, `BitWriter`, `ArrayBitReader`, `ArrayBitWriter`: `IsEmpty`, `IsByteAligned`, `Slice` ×3, `AlignToByte`; plus the four *constructing* entry points `ReadOnlySpan<byte>.AsBitReader`, `Span<byte>.AsBitWriter`, `ReadOnlyArraySpan<byte>.AsBitReader`, `ArraySpan<byte>.AsBitWriter`. **Not pure:** every read, every write (including `TryWrite*`), every `TryRead*`, `AlignedTail`, `AsReadOnly`, and the two *delegating* entry points `Span<byte>.AsBitReader`/`ArraySpan<byte>.AsBitReader`.
- **Primitives:** every `Parse` and `ParseOrNull` — **but not `TryParse`, which declares an `out`**; `ToString()`/`ToString(format)`/`ToString(radix)`; `CompareTo`; the `Double`/`Float` predicates and `IeeeEquals`/`BitwiseEquals`; `Decimal.Round`; every `Char.Is*`/`ToUpper`/`ToLower`.
- **`String`:** `Length`, `IsEmpty`, `IsAscii`, both indexers, `From(char)`, `From(Array<char>, start, count)`, `Repeat` ×2, `IsNullOrEmpty`, `IsNullOrWhiteSpace`, `Contains` ×2, `StartsWith`, `EndsWith`, `IndexOf` ×4, `LastIndexOf` ×2, `Substring` ×2, `AsSpan` ×4, `Trim` ×2, `TrimStart`, `TrimEnd`, `ToUpper`, `ToLower`, `Replace` ×2, `PadLeft`/`PadRight`/`PadCenter`, `Insert`, `Remove` ×2, `CompareTo`, `Compare` ×2, `Equals` ×2 plus the `Object?` override, `GetHashCode`, `ToString`, `operator +`. **Not pure:** `Join`, `Split` ×3, `ToCharArray`, `GetEnumerator`.
- **`Range<T>`:** `IsOpenEnded`, `IsEmpty`, `Contains` carry the marker (v0.263); `Start`/`End`/`Step`/`IsEndInclusive` are pure by classification.
- **Constructors:** `Exception()`, `Exception(message)`, `ArgumentException(message)`, `ArgumentException(message, paramName)` are `pure constructor`s. `Exception(message, cause)` is **not**, and `Exception.ToString()` is **not**.

**Nothing in the query operators, threading, I/O, diagnostics, or the `Result` combinators carries `pure`.** The Try shapes are uniformly unavailable to contracts (they declare an `out`). `pure` is admitted on a getter (`pure get`) and **never on a setter — `pure set` does not parse** (§22.15).

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

```
Main()   Main(args: Array<string>)   Main(): int   Main(args: Array<string>): int
async Main(): Task   async Main(args: Array<string>): Task
async Main(): Task<int>   async Main(args: Array<string>): Task<int>
```

**Reading a file.** `Array<T>` is trivial-dispose, so a plain `var` carries no must-use warning; `using var` is also accepted and states the owner explicitly.

```bezel
module Tools.WordCount;

import System.IO;

public class Program
  public static method Main(args: Array<string>): int
    if args.Count < 1 then
      Console.WriteLine("usage: wordcount <path>");
      return 2;
    end;
    var path := args[0];
    if not File.Exists(path) then
      Console.Error.WriteLine($"not found: {path}");
      return 1;
    end;
    var lines := File.ReadAllLines(path);        // owned Array<string>
    var words := 0;
    for var line in lines do
      var parts := line.Split(' ');              // owned Array<string>; keeps empty segments
      words := words + parts.Count;
    end;
    Console.WriteLine($"{lines.Count} lines, {words} words");
    return 0;
  end Main;
end Program;
```

For a stream, `using var` is the workhorse and covers the exception path; owned locals are disposed in reverse declaration order.

```bezel
using var fs := new FileStream(path, FileMode.Open, FileAccess.Read);
var header := fs.ReadAll();                      // owned Array<byte>
Console.WriteLine($"{fs.Path}: {header.Count} bytes");
```

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
    return pool;                                 // target-announced: NOT `return move pool;`
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
import System.Collections.Generic;

public class ConnectionPool
  private owned _cache: OwningDictionary<string, Connection> := new OwningDictionary<string, Connection>();

  public method Keep(name: string, owned conn: Connection)
    _cache[name] := conn;                        // owned-slot store: target-announced, no marker
  end Keep;

  public method TryPopCached(name: string, out owned conn: Connection? when true): bool
    if not _cache.ContainsKey(name) then
      conn := null;                              // every out path must assign (throw paths are exempt)
      return false;
    end;
    conn := _cache.PopValue(name);               // takes ownership out of the cache
    return true;
  end TryPopCached;

  public method Drop(name: string)
    if _cache.TryPopCached(name, out using var c) then
      c.SendShutdown();
    end;                                         // c is disposed at the end of this block
  end Drop;
end ConnectionPool;
```

`out owned` in the signature ⇄ `out using var` (or `out owned var`) at the call site; the compiler rejects a mismatch in either direction. Without `when true` the caller would need `c!`: a `bool` guard alone does not narrow.

**`Result<T, E>` and an exhaustive `case`.** In a **declared-result position** the bare case name resolves against the declared return type (§35.3); everywhere else the qualified form `Result<int, string>.Ok(v)` is required.

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
    case ParsePort(text) of
      Ok(var p)    => Console.WriteLine($"listening on {p}"),
      Error(var m) => Console.Error.WriteLine($"bad port: {m}")
    end;                                         // exhaustive: no `_` arm needed
  end Report;
end Ports;
```

**A choice with an owned payload, matched by consuming.** A `case` over a *named* binding only peeks; consuming one is `case move outcome of …`.

```bezel
public choice PollOutcome
  Ready(owned conn: Connection, attempts: int),
  Empty;
end PollOutcome;

public static class Poller
  public static method Drive(outcome: PollOutcome)
    case move outcome of
      Ready(move conn = var c, attempts = var n) => do
        using var live := move c;
        Console.WriteLine($"connected after {n} tries");
      end,
      Empty => ()
    end;
  end Drive;
end Poller;
```

**A class that holds resources — usually with no destructor.** `owned` fields are disposed automatically in reverse declaration order. Write a `destructor` only for a raw or foreign resource, and keep one raw resource per owning wrapper.

```bezel
public class Customer(name: string)
  public readonly property Name: string := name;
end Customer;

public class Ledger(owner: Customer)
  private owned _entries: Array<string> := new Array<string>();
  private owned _cache: OwningArray<Connection>? := null;   // nullable owned field
  private _owner: Customer := owner;                        // back-reference — NOT owned

  public method Record(text: string)
    _entries.Add($"{_owner.Name}: {text}");
  end Record;
end Ledger;
```

**Async, and structured fan-out.**

```bezel
import System.Threading;

public class Refresher
  public static async method LoadAsync(path: string): Task<string>
    return await File.ReadAllTextAsync(path);    // the Task-family call mints an owned frame
  end LoadAsync;

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

`var text := await LoadAsync(p);` is the **scoped await-bind**: a fresh `var`/`const` declaration whose initializer is exactly an await of an unbound producer pins the task to a hidden slot for the enclosing block (§30.5).

**Contracts, and a `pure` member.** Clauses sit between signature and body and take no terminator; a blank line separates the last clause from the first statement.

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

  public static pure method CountOf(text: string, needle: char): int
    var n := 0;                                  // frame-local mutation: admitted
    for var c in text do
      if c = needle then
        n := n + 1;
      end;
    end;
    return n;
  end CountOf;
end SampleWindow;
```

`pure` is declared and verified, never inferred. A pure body may read mutable state but may not store through `this`, call a non-`pure` member, or invoke a function value, and it declares no `ref`/`out`/`owned`/`scoped` parameter (§22.15).

**Tallying, querying, materializing.** A bound lazy query is affine — enumerate it once, or `ToArray()` it to read the results repeatedly.

```bezel
import System.Collections.Generic;
import System.Collections.Linq;

public static class Words
  public static method Report(text: string)
    using var tally := new Counter<string>();
    var words := text.Split(' ');
    for var w in words do
      if w.Length > 0 then
        tally.Increment(w.ToLower());
      end;
    end;
    var top := tally
        .OrderByDescending(lambda(e) => e.Value)
        .Take(10)
        .ToArray();                              // owned Array<DictionaryEntry<string, int>>
    for var e in top do
      Console.WriteLine($"{e.Key.PadRight(20)} {e.Value}");
    end;
    Console.WriteLine($"{tally.Count} distinct / {tally.Total} total");
  end Report;
end Words;
```

**Strings.**

```bezel
public static class Formatting
  public static method Line(words: Array<string>, hits: int, total: int): string
    var sb := new StringBuilder();               // trivial-dispose: plain `var`, no warning
    for var w in words do
      sb.Append(w).Append(' ');
    end;
    var line := sb.ToString().Trim();
    var pct: double := 100.0 * double(hits) / double(total);   // `double(x)` is the cast-call form
    return $"|{line,-20}| {pct,6:F1}%";          // alignment, then format specifier
  end Line;
end Formatting;
```
