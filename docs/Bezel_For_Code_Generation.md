# Bezel for Code Generation

*Validated against v0.348. Companion to the Bezel Language Specification — a writing reference, not a normative document.*

A dense reference for writing correct Bezel without reading the spec. It covers the syntax surface, the ownership rules, the standard-library name index, the idioms that work, and the shapes that look right and are rejected.

*Maintenance rule for this document:* no revision pin is added to prose — the changelog carries the history. A new clause lands as a table row or as a *Looks right, is rejected* pair (section 7), never as a sentence appended to a paragraph.

---

## 1. How to use this document

**What this is.** Everything needed to emit compiling Bezel: the whole statement and expression grammar, the ownership discipline the compiler enforces, and an index of every standard-library name you are allowed to spell.

**What this is not.** Normative. Where this document and `Bezel_Language_Spec.md` disagree, the spec wins and this document has a defect. Citations (`§17.2`, `stdlib §4.5`) are to spec v0.348 and `Bezel_Standard_Library.md` v0.348 so any claim here can be re-checked at its source.

**The one rule that matters most.** *If a type or member does not appear in section 5 of this document, look it up in `Bezel_Standard_Library.md` — and if it is not there, it does not exist.* **Never guess an API.** Inventing a plausible standard-library name is the single most common failure mode when writing Bezel: `MemoryStream`, `File.Open(path)`, `List<T>`, `string.Format`, `.ToList()`, `Task.Run`, `Option<T>`, `x.Dispose()` on a borrow — all plausible, all absent. Bezel's library is small and deliberately shaped; the shape is not .NET's.

When the API you need genuinely is not there, **say so** rather than approximating: "Bezel has no regular-expression module (`System.Text.Regex` is deferred); this needs a hand-written scanner" is a correct answer, and a hand-rolled `Regex.Match` call is not.

**Reading the examples.** `✗` marks a line the compiler rejects, `✓` the spelling to write instead; a comment containing `ERROR` marks the rejected line. Illustrative user types are declared inline. `NOT YET IMPLEMENTED (#N)` (in prose, **not yet implemented**) marks a name the standard library specifies that the compiler does not accept yet: it is the language, so keep the spelling, and the issue tracks the gap. Section 9 says where to go for what this document omits.

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
| 13 | `if (x is Dog d)` | `is` **does not bind**. `var d := x as Dog; if d <> null then …` or a `case` arm `var d: Dog`. Over a **value**-typed `x`, a test against a reference type or another value type is rejected (section 7.1) | §12.7, §19.3 |
| 14 | `if (count)` truthiness | Conditions are `bool`. Write `count <> 0` | §4.1 |
| 15 | `i++` as an expression, `++i` | `++`/`--` are **postfix-only statements**: `i++;` on its own line, over an integer, floating-point, `decimal` or `char` operand only (§6.6's set — `char++` works). They are not overloadable: declare `operator +(c: Counter, n: int)` and write `c := c + 1;`, or use a named method. Assignments are statements too and produce no value | §17.6, §17.2, §6.6 |
| 16 | `int? + int?` lifts | **Not lifted.** Arithmetic, bitwise, shift, relational, compound and `++`/`--` over a nullable operand are a compile error — a §7.4-narrowed local counts as non-nullable at every one of them (`if n <> null then n += 1; end;` is fine). `=`/`<>`/`??`/`?.`/`?(args)`/`??=` still work | §7.11, §6.6 |
| 17 | `if (_field != null) Use(_field);` | Mutable fields, properties and **`ref` parameters never narrow** (an `out` parameter does, after assignment). Copy to a local first | §7.4 |
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

Auto-imported, no `import` needed (§2.3): `System`, `System.Text`, `System.Collections.Generic` (every collection type, the spans, `Counter`, `FrozenArray`, `KeyNotFoundException`, and the iteration interfaces), `System.Collections.Linq` (the query operators), the primitives, the `Task` family, `CancellationToken`, `Subscription`, and the six event types. `CancellationTokenSource` is NOT auto — it needs `import System.Threading;`. **Everything else needs an import** — section 5.0 has the table.

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

Modifiers on a type are `abstract | closed | sealed | static` (A.3). `closed` is the closed abstract class and combines with no other class-shape modifier (✗ `sealed abstract class C` ✓ `closed class C`, §11.7). Base class first in the inheritance list, then interfaces; **only the base** may carry `(args)`. Generic constraints come *after* the inheritance clause. Primary-constructor parameters are in scope throughout the body, including initializers (§22.10). One is read by its bare name only: ✗ `this.host` (the capture is not a member). A member's own parameter of the same name hides it throughout that member; to read both, declare `private _host: string := host;`.

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
  // compiler-provided on EVERY enum: Priority.FromUnderlyingOrNull(u): Priority?, Priority.FromNameOrNull(s): Priority?
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
| Setter parameter spec | `set(owned);` takes the value owned — the parameter is always `value`, and its type is the property's, never repeated; `set(x)`, `set(owned x)` and `set()` do not parse | §23.3 |
| Indexer | `public property [i: int]: string => _items[i];` — the block form closes with a bare `end;` | §23.4 |
| Constructor | `public constructor(a: int)` + optional `base(a);`/`this(a);` chain + body + `end constructor;`. A chain argument may read only the constructor's own parameters, constants and statics — never a field of `this` or a primary-constructor parameter. A `this(...)` chain must end at a constructor with no `this(...)`; a cycle is rejected. A **class** that declares no instance constructor has an implicit `public` parameterless one, so `new C()` compiles; declaring any instance constructor (a primary parameter list included) removes it, and a **body record** has no such implicit constructor | §22.7, §12.8 |
| Destructor | `destructor` … `end destructor;` — no visibility, no name, no parameters | §28.4 |
| Operator | `public static pure method operator +(a: V, b: V): V => …;` closes `end operator +;` | §25.1 |
| Equality operator | Overloading `=`/`<>` with **both operands the declaring type** obliges `Equals(T)` **and** `GetHashCode` beside them (`override` or plain, on a record, enum, or choice alike — v0.288), and by itself strips the record or value-like choice of auto-`=`/`Equals`/`GetHashCode`/`ToString` — **write all four or none**. Declaring `Equals` or `GetHashCode` (either spelling) strips the same package on a record or value-like choice; the type is then **hashable** on the declared pair, and `=`/`<>` are rejected until the overloads are written (§3.6, §9.2, §35.7). A **heterogeneous** `=` (`T = U`) obliges nothing and strips nothing. On a move-only choice, `=`/`<>` operands and the `Equals(T)` parameter bind BORROWS — both operands stay live (v0.286). An enum declaring its own-typed `Equals` likewise has no `=`/`<>` until it overloads them, every other consumer reaching the declared pair (§10.5); inside the body compare `int(this) = int(other)`. Declaring the `Object`-slot `Equals` — the own-typed one on a record, enum or choice, in either spelling; `Equals(Object?)` or the own-typed one on a class — obliges the zero-parameter `GetHashCode` beside it, except on a choice owning at every instantiation (v0.317; records v0.336); an `Equals` at any other parameter list, an `override` of a user base's `Equals(a: K, b: K)` included, obliges nothing (§8.3). | §25.2, §9.2, §8.3 |
| `override` on a value kind | only `Equals`, `GetHashCode` and `ToString` take it, `override` or plain (§9.2); on a record, an enum and a choice alike the `Equals` is the one taking the type's **own** type — `public override method Equals(other: Priority): bool`, `public method Equals(other: Money): bool` — and it **is** the `Object` slot: a boxed value and an `Object?` argument reach it (a non-own-typed argument is unequal), an `Equals(o: Object?)` is **rejected** on every value kind, `override` or plain, and the own-typed `Equals` obliges `GetHashCode` beside it (§8.3); an interface member `Equals(other: Object?)`, `GetHashCode(): int` or `ToString(): string` is satisfied by the member filling that slot — none, no conformance (§14.2) | §9.2, §10.5, §35.2, §8.3, §14.2 |
| Invariant | `invariant(_bal + _limit >= 0m);` — class bodies only, no visibility, keeps its `;`; checked at `public` entry and normal exit, an `async` method's entry only | §22.16 |
| Nested type | `public record Token(…);` inside a class body — visibility **required**; one type per declaration, blind to an enclosing generic's type parameters (declare its own: `Node<U>` inside `Tree<T>`, used there as `Node<T>`); outside `Tree` and the bodies of types deriving from it, it has no type spelling (`Tree.Node<int>` is an arity error and `Tree<int>.Node<int>` does not parse in a type): reach it through a chain head (`Tree<int>.Node<int>.Create()`) or an inferred local, or declare it top-level | §11.6 |

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

**Generics:** `<Variance? Ident, …>` with `Variance ∈ in | out` (interfaces only); `where T: Constraint, …` with `Constraint ∈ class | value | notnull | sendable | hashable | new() | Type` (§14.2). A method's `where` goes after its return type and before any `requires`/`ensures`, in an interface member too (`method Largest<T>(xs: Array<T>): T where T: IComparable<T>;`). An override or interface implementation **inherits** the member's constraints: omit its `where`, or restate an equivalent set; anything else is rejected, and so is one method implementing or overriding two declarations whose sets differ (§14.2). An override or interface implementation declares the same number of type parameters of its own, matched by position, so names may differ (`Show<U>(x: U)` implements `Show<T>(x: T)`; §11.2, §12.1). Type arguments are **never inferred** for a class, record, interface, or choice — only call-site inference for generic *methods* exists (§14.5). Generic choice construction writes its arguments: `Result<int, string>.Ok(v)` (§35.3).

What a use of `T` in a generic body demands, and where its absence is rejected:

| Use of `T` | What it needs | Rejected without it | Cite |
|---|---|---|---|
| `x.Equals(y)`, `x.GetHashCode()` or `x = y` on a `T`-typed value, in a generic **method** | `where T: hashable`, to say so in the signature | the instantiation at a record or choice with no equality of its own (not auto-hashable, member undeclared); the use itself compiles in the body | §9.2 |
| the same use in any member of a generic **type** you declare | `where T: hashable` on the type; to demand it of callers only, declare the member in `extension MyBag<T> where T: hashable` (public and same-module `internal` surface only) or as a generic method with its own type parameter. An override, interface implementation, operator, indexer, constructor, destructor, field initializer or settable property cannot move, so its use needs `where T: hashable` on the type | the constructed type, wherever it is named, whether or not that member is called | §3.6 rule 2 |
| a use of the **method's own** parameter in a dispatched generic method — an interface method, a `virtual`/`abstract` method, an override or an implementation — beyond what the inherited constraints establish: `x = y`, `o as T`, a `Dictionary<T, int>` local | the constraint on the **interface or base** declaration (an implementation cannot add one); for `o as T`, an `is` test instead. Copying or `case`-binding the parameter, a `T?` local or a `[2]T` needs nothing from the move-only rules: a dispatched method's `T` is never move-only and never an inline array; boxing it still meets §9.7's borrow-erasing bullet at the body (the row below) | the declaring body itself, at its declaration — dispatch forms instantiations no call site sees; and any call of such a method at a move-only or inline-array type argument (`i.Put<[3]int>(v)`), at the call | §14.2 *Dispatched generic methods* |
| a standard-library equality search at `T`: `Contains`, `Remove`, `IndexOf`, `LastIndexOf`, `LinkedList<T>.Find`, the span `IndexOf`/`Contains`/`SequenceEqual` | as the two rows above: in a generic body such a call counts as your own `Equals` use of `T` | your type or method at a `T` with no equality. `Array<T>` and the spans are not themselves rejected this way; at a concrete element type with no equality, the call is | §14.2, stdlib §4 |
| an instantiation under `where T: U` whose argument for `T` reaches `U`'s only by a conversion: `Rel<int, Object>`, `Rel<string, Object>`, `Rel<int, long>`, `Rel<Widget, Widget?>` over `class Rel<T, U> where T: U` | `T`'s argument must be `U`'s argument itself, a class derived from it, or a type conforming to it — no boxing to `Object`, widening or nullable wrapping (a record, enum or primitive conforming to an interface argument for `U` is admitted); at `U := Object`, `Object` and class types only | the instantiation, at its site | §14.2 *Type-parameter bounds* |
| another conditional member at `T` (stdlib §1 *Conditional members* is the closed list): `Array<T>.Sort()`; `Result<T, E>`'s `Map`/`MapError`/`AndThen`/`UnwrapOr` | its condition, read at your `T` the same way as the operation written on `T`: `Array<T>.Sort()` needs `where T: IComparable<T>` | `Sort()`: the call, in a body whose constraints on `T` do not establish it (as for `x.CompareTo(y)`); `Result<T, E>`'s four: your instantiations; a method reference over any of them (`var s: () -> () := xs.Sort;`): the formation, read as the call is | §14.2 rule 2, §27.6 |
| a method reference over a **generic method**, or over a member of a **generic extension block** | its type-argument list — `Util.Keep<Pair>`, `xs.Select(Util.Id<int>)`, `IEnumerable<Pair>.KeepAll` (the qualifier binds the block's parameters; a member's own list follows the name); a reference to a generic method with no list does not compile, and nothing is inferred from the target | the formation, read as the call with that list would be: the body's requirements at `T`, the constraints, the dispatched exclusion, §28.3's `Task`-family and exported-mint exclusions; never the invocation of the formed value | §27.6, §14.1 |
| a static member of a generic type reached as `G<Arg>.M(…)` or `G<Arg>.P` | nothing: naming the type instantiates it, so every requirement of `G<Arg>` — its statics' bodies included — is read at the access | the access | §14.1, §22.12 |
| a member of a generic **extension block** called on a receiver (`ps.KeepAll(sink)` over `extension IEnumerable<T>`) | what the member's body needs of the block's `T`, read at your receiver's element type exactly as a generic method's own parameter is read at a call | the call | §26.4, §3.6 rule 2 |
| naming `MyBag<T>` (a type of the second row), or calling such a generic method, at your own `T` | what `MyBag<T>` needs — wrapping does not hide the use | your type or method at its own instantiations, wherever `MyBag<R>` would be | §14.1 |
| naming `C<T>` — or any argument whose fit depends on `T`: `C<T?>`, `C<Pair<T>>` — where `C` declares `where T: new()` (or any constraint); an extension head is such a naming | `T`'s own constraints must guarantee it, so restate it: `class Box<T> where T: new()`, `extension OwningArray<T> where T: class`. A declared `where` constraint is not carried the way the rows above carry a use | the naming | §14.2, §26.4 |
| a `T`-typed value, or a `Box<T>` / `[2]T` value over one, stored in a field (an `Object?` field included), copied into a field-held or escaping collection, captured by an escaping closure, or returned carrying a plain parameter's or a local's borrow — §9.7's bullets | nothing at the declaration: each bullet is read at the instantiations whose `T` makes the value a value type that holds borrows (a record or inline array with a class component, a span, `CancellationToken`), so write the body for the class case; in a dispatched generic method (the dispatched-generic-method row) it is an error at the body unless the declaring method's constraints exclude such a `T` (a class bound, `where T: class`, or `where T: sendable` for a body over values, which also excludes function types) | the instantiation: `Wrap<Item>` over `static method Wrap<T>(a: T): Box<T> => new Box<T>(a);`, `Keep<Pair>` over a `_last := a` store; `Wrap<int>` and `Keep<Item>` are fine | §9.7, §3.6 rule 2 |
| `new T()` under `where T: new()` | — | a substitution at a nullable reference type (`Widget?`); a nullable value type is admitted, and there `new T()` is `null` | §14.2 |
| `default(T)` under an unconstrained `T` | `default(T?)` where absence is meant — `null` of type `T?` at every substitution | the instantiation at a non-defaultable type: a class, interface, `Object`, `string`, function or task type, a choice, a zero-less enum, a constructor-only record, a record or inline array with a non-defaultable component (`default(T)` is that type's zero value, `null` only at a nullable substitution) | §17.11 |
| a record component typed `T` (`record Box<T>(V: T)`), `IEnumerable<T>`/`IEnumerator<T>` or their async forms, or `EventSource<T>`/`AsyncEventSource<T>` named at your own `T` | `where T: class` (or a class or interface bound) to exclude the move-only value types; nothing else stops the chain | your type or method at every instantiation substituting an owning choice or an owning inline array — a record copies, an enumerator's `Current` lends, a raise moves to each subscriber | §15.3, §28.11, stdlib §4.12, §6.5 |
| a copy of a `T`-typed value out of a borrowed binding — the result of `x as T`, a `case` or typed-pattern binding, a capture, a `ref` referent — in a generic body | an owned operand instead: the parameter or local itself (`_f := v;` moves at a move-only `T`) | your type or method at every move-only instantiation (the §7.10 copy demand, read whatever the constraints admit) | §7.10 |
| `OwningBuffer<T>`, `Span<T>` or `ReadOnlySpan<T>` named at your own `T` | nothing at the declaration; write the body against the members both presentations share | the instantiation at a value `T` for a class-only use (`move buf[i]`, `set(owned)`, a reliance on `IEnumerable<T?>`), and at a non-defaultable value `T` for `new OwningBuffer<T>(n)`; the presentation is read per instantiation | §14.2, stdlib §4.14 |

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
var balance: decimal;                       // typed, no initializer: holds `0m` until assigned (§7.9); a non-defaultable type must be assigned before its first read
var greeting := "Hello";                    // inferred
const Pi: double := 3.14159;                // single-assignment binding (§21.1)
using var _ := mutex.Acquire();             // anonymous owning binding; `await using var` in async bodies
record Row(Name: string, Age: int);         // LocalTypeDecl: `record` and `enum` ONLY, no visibility; its bodies do not see the enclosing body's locals or parameters (§21.10) — pass values in as fields or arguments

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
'a'  '\n'  '\u0041'  '\U0001F600'
"Hello\n"    @"C:\raw\path"                       // verbatim; "" is one embedded quote
$"Hello, {name}!"     $@"C:\Users\{user}\x.pdf"
$"{amount,12:F2}"     $"|{name,20}|"   $"{ratio:P1}"   // {expr,align:format}
$"{{literal braces}}"
true   false   null   ()                          // bool / null / the unit value
[1, 2, 3]     []      ["a": 1, "b": 2]            // collection and dictionary literals
```

Escapes: `\n \r \t \\ \" \' \0 \a \b \f \v \uXXXX \UXXXXXXXX` (§18.8) — there is no `\x` escape: write `\u0041`, not `\x41`. An interpolation hole renders `ToString()` (through a `T`, or through a generic choice over one such as `Result<T, E>` in a generic body, a choice with no `ToString` — not auto-hashable, none declared — renders its qualified type name and is not rejected; at a choice type containing no type parameter, it is an error, §8.5); `{e:F2}` renders `ToString(format)` and requires an intrinsically formattable type or `IFormattable`; a **null** hole renders the four characters `null` (§18.5).

Atoms and suffixes (A.7):

```text
new Connection("a")            new()                 // target-typed shorthand
new Point(1, 2) with(X := 5)                         // `with` = record copy-with-overrides
obj.Method<int>(args)          Registry<int>.Register()
user?.Name?.Length             handler?(42)          // null-conditional access / call
x!                                                   // postfix null-forgiving
move x     move obj._field with e     move arr[i]    // the use-site transfer marker; `with e` replaces a field or slot (§28.8.1)
nameof(amount)   typeof(BankAccount)   default(int)
int.Parse(s)   int.MaxValue   short(x)   long?(n)    // primitive keyword in expression position; casts
this   base.Speak()
```

`new()` — the target-typed shorthand — is admitted at four of §17.7's written-declared-type positions: a typed field initializer, a typed local initializer, a default parameter value, and a `return new();` statement under a declared return type. It is **not** admitted at an expression body (`=> new()`), an ascribed lambda's return, an assignment target (`xs := new();`) or an argument.

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
Vec.Scaled   /   v.Scaled                   // extension-method reference, type-qualified `(Vec) -> int` / receiver-qualified `() -> int` (captures `v`)

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
1 | 2 | 3      Red | Blue                                                // alternatives: binding-free
var t when t > 30 => ...                                                   // guard
```

Positional or named, never mixed. A naked identifier is a **binding** only inside positional destructuring; at top level it is a type test. Classes have no positional-destructuring surface, even with a primary constructor — but the **named** form does reach a class: `Circle(Radius = var r)` is admitted, its slots naming the class's *public readable properties* (§19.3), and over a sealed hierarchy an all-slots-irrefutable arm of that shape counts toward exhaustiveness exactly as `var c: Circle` does (§19.6). Interface-typed pattern heads are not admitted in v1. A relational pattern needs a scrutinee with an intrinsic ordering, which means a primitive other than `bool`, or `string`. A range pattern needs an integer or `char` scrutinee, so over `double`, `decimal` or `string` write `>= lo`, or a guard, never `lo..hi`. A record, enum, choice or class is rejected at both patterns even when it declares `operator <`, so write a guard: `var x when x < limit => …` (§19.3).

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

Records, enums, primitives and inline arrays are **value types**: no disposal, no classification. An inline array of an owning choice (`[4]Outcome`) is a compile error (§15.1): wrap the choice in a class and use `[4 owned]Holder?`. `string`, boxed values, function values and `Type` (the `typeof` / `GetType` token) are **runtime-managed**: no disposal, no classification, and `owned` on them is a compile error (§28.1, §28.9).

**You never write `Dispose()` as plumbing.** An owned value reaching its scope's end undischarged is disposed *there*, automatically, on the normal edge **and** the exception edge — no `try`/`finally` for a value, ever, and a written `x.Dispose();` means *release early*. The only difference between the edges is a lint: a plain-`var` implicit drop on normal exit raises the **must-use warning**; the exception edge is silent (§28.3). **Never declare a member named `Dispose`** on a class or a record — at any parameter list, it is a compile error at the declaration (§28.2); disposal has exactly two customization points: a `destructor` (only for native or foreign resources) and `owned` **fields**, auto-disposed with their container in reverse declaration order (§28.4, §28.5). The generated `Dispose()` is public, idempotent and non-overridable — no `disposing` parameter, no `_disposed` flag, no `base.Dispose()` call.

**Trivial-dispose classes** are exempt from ceremony, not from tracking (§28.9): a class with no destructor, no `owned` field of non-trivial type, no field of move-only type (an owning inline array or an owning choice — both are disposed by the generated `Dispose()`), on a trivial-dispose base. `Array<T>`, `Set<T>`, `Dictionary<K,V>`, `StringBuilder`, `Random` qualify — no must-use warning anywhere the warning would otherwise attach: a plain `var`, an undischarged `owned` parameter (a method's, an iterator's, an async method's), an owning iteration variable. An **interface**-typed binding is never exempt — unless its initializer is a producer of a trivial-dispose **class** (`var t: IThing := new TrivialThing();`), the exemption being keyed on the value, not the annotation. `OwningArray<T>` and anything holding a resource do not. **Every borrow rule still applies to trivial-dispose references.**

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

**`move` — the use-site transfer marker** (§22.3, §28.8.1). Four operand shapes: `move x` (a named local or `owned` parameter), `move obj.f with e` (an `owned` field: `e` is stored into it in the step that takes), `move coll[i] with e` (an owning slot, likewise), and the bare `move obj.f` / `move obj.f!` over a **nullable** owned field or slot, which stores `null` — §28.8.1's detach, one expression. A trailing `!` never changes the spelling (§17.8): over a nullable owned local, `sink._f := x!;` and `throw x!;` consume like `sink._f := x;`, `M(move x!)` is the marked argument, and `sink._f := move x!;` is the redundant-marker error.

*Required* — omitting it is a compile error ("this call consumes `x`; write `M(move x)`") — when a **named owned binding** feeds an `owned` argument (`M(move x)`), a local binding (`var y := move x;`, `using var y := move x;`), an assignment (`y := move x;`), an if-expression branch or expression-arm result (`(if c then move a else move b)`), or an owning-match scrutinee (`case move outcome of …`). A call that **raises** does not give the argument back: the transfer completes at call entry, the binding is still moved-from in the `catch` arm, and the callee's own exception-path discharge (or a constructor's unwinding) disposes the value (§28.8.1, §22.9).

*An error* — the marker is **rejected as redundant** — at a **marker-free position**, where the target already says "consume". Memorize this list; "mark every transfer" is the wrong generalization and produces exactly these errors (§28.3, §22.3):

- **`return` operand** under an `owned` or mint-announcing return, and in an `async` body under `OwningTask<T>` or a class- or interface-typed `Task<T>` (the result): `return x;` — ✗ `return move x;`
- **`throw` operand**: `throw x;` — ✗ `throw move x;`
- **discard RHS**: `_ := x;` — ✗ `_ := move x;`
- **owned-slot store**: `sink._f := x;` into an `owned` field, an owning collection slot, or a `set(owned)` property — ✗ `sink._f := move x;`
- **Pattern B `await`** on an owned `OwningTask<T>` binding: `using var r := await t;` — ✗ `await move t`
- **owning iterator `yield return`**: `yield return x;` — ✗ `yield return move x;`

`move` is *also* an error on a **fresh producer** (`M(move new T())` — nothing is being invalidated), on a **borrow**, in a **`??` operand** (a named owned binding written bare as a `??` arm is a *borrow* arm), inside a **closure** over a captured owned binding (captured owned bindings are uniformly in borrow state — move, reassign, dispose and discard of them are all errors), and on a **move-only value** argument, binding, if-expression branch, expression-`case` arm or `??` operand (§15.3's owning inline arrays and owning choices — the unmarked read already moves, at `(x: V)` and `(owned x: V)` alike; the owning-match scrutinee `case move c of …` is the one marked move-only position), and on a **`static` field**.

**The no-silent-consume rule.** An unmarked read of a class-typed binding *never* consumes. `var b := a;` is always a borrow of `a`. This is the single most important spelling fact in the language (§28.3).

**Field and slot moves replace, never empty** (§28.8.1). `move obj.f with e` evaluates the receiver, then `e`, then stores `e` and yields the old value in one step: the field holds `e` afterwards, nothing is ever left to restore, and the `move` may sit anywhere — before or after a call, across an `await`, in a `return`. `e` is an owned value (a fresh producer, a `move` result, a named owned binding written **unmarked** — the position is a store into an owned location — or `null` at a nullable location); a borrow or a `using var` is rejected. Where the field's or slot's declared type is **non-nullable** the `with` clause is **required**: `return move _f;` at `owned _f: T` is a compile error naming `move _f with fresh` and the nullable declaration (and `PopAt`/`PopValue` at an `OwningArray<T>`/`OwningDictionary<K, V>` slot). Where it is nullable the bare form stores `null` (`return move _c!;` is the detach). An exception in the receiver, the index or `e` leaves the location untouched. Static owned storage admits no `move` at all (K9.4).

**`using var`** (§28.10): owned, disposed at the block's end on **every** exit edge, non-reassignable, and **never movable out of**. `using var fresh := new Buffer(); … return fresh;` is a compile error; a factory uses plain `var`.

- `using var y := move x;` — **adoption**: the standard way to turn an owned local, parameter or loop-variable obligation into scope-bound disposal.
- `using var y := x;` (unmarked, `x` a **class- or interface-typed** named **owned** binding — an owned `var`/`using var` local, an owned loop variable, an `out owned` recipient, an owning-match binding, an `owned` parameter, a bare `T`; `x!`, `(x)`, `using var _ := x;` and `await using var y := x;` alike, and an `if`/`case`/`??` arm-meet whose every arm is such a read — `(if c then a else b)`, `a ?? b` — null and `throw` arms aside) — a **compile error**: the form says neither adopt nor view. Write `move x` to adopt; for a view that leaves the obligation with `x`, write `var y := x;` or `const y := x;` (a borrow, live to its last use) — for `using var _ := x;`, delete it; to hold `x` itself to this block's end, declare `x` with `using var`. Over a **borrow-state** source (a plain parameter, a borrowing alias or loop variable, a captured owned binding inside a closure), a **call** without an owned return, a **field** or an **indexer**, `using var y := …;` is a **borrow-pin**: no obligation is assumed, the source keeps it, and the pin extends the source's required liveness to this block's end — moving, transferring or reassigning the source under the pin is rejected, while an explicit `Dispose()` of it is not (a site-announced kill; a later read through `y` that the disposal always precedes is a compile error, and one it precedes on only some paths faults in checked mode, §28.2).
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

Disposing an **owned field** from code of its declaring type (`_resource.Dispose();`) is an early release point and demands nothing afterwards (§28.6): the field keeps its reference, the container's teardown still cascades over it as a no-op, and no replacement is stored — unlike `move obj.f with e`, which exchanges the field's value for `e`. A later read of that field is not diagnosed; it faults in checked disposal mode.

`_ := x;` leaves the binding **explicitly-disposed**: a later **use** that the discard always precedes — of `x`, or of a local borrow of its value — is a compile error, a second `_ := x;` or `x.Dispose()` a warning (a no-op), and a use after a *conditional* discard is a compile error at the merge. *Use* is read, borrow **and consume** alike — `move x` and an `owned` argument position draw the same verdict a plain read does, the verdict being decided by which kill reached the binding and never by the shape of the use (§28.2). Discarding **the task itself** — `_ := task`, `using var _ := task` — is **rejected** for the `Task` family; use `Task.FireAndForget(t)`, or a *named* `using var t := …` (§17.13). Discarding an **awaited result** is a different statement and is legal: `_ := await t;` is a Pattern B owned-consuming position that disposes the result at the statement's end (§30.5).

**`ref` / `out`** (§22.3, §22.4). Written at the declaration **and** at the call site. `out` is copy-out-on-normal-return: the callee must assign on every normal-completion path; on the exception edge the recipient is untouched.

- `out owned p: T` is the only modifier compound. At the call site `out var x` **adopts** what the signature announces — against `out owned` it is an owned local you discharge yourself (`move` it, store it in an owned slot, or let scope-end auto-drop with the must-use warning) — and `out using var x` is the scope-bound owned form, accepted **only** against `out owned`. There is no `out owned var` spelling (retired).
- `ref`/`out` arguments may **not** name a location of owned state (an owned local, an `owned` parameter, an `owned` field, an owning slot, or any location of move-only type — a field of owning-choice or owning-inline-array type included, and in a generic body a `T`-typed location, checked at each instantiation, at every move-only `T`), nor a `using var`, nor a `readonly` field outside its assignable context. **For an *instance* `readonly` field or auto-property that context is per-instance** (§22.8): it is assignable only through an lvalue chain rooted at `this` — so a constructor may not write *another* value's `readonly` storage, and binding one to a `ref`/`out` parameter inside a constructor is the same store by other syntax — and an instance `readonly` **field** is assigned **at most once** on any path through one instance construction, counting the field initializer and every constructor body in the `this(...)` chain together (stated for fields — auto-properties are not decided). A `static readonly` field is different on both counts: its context is its own static initializer or the declaring type's `static constructor()`, never an instance constructor, spelled as the bare name or `TypeName.F`, and its unit is one type initialization. Pass a borrow instead, or use `owned` parameters/returns and `move`. A `ref` parameter is never declared at a move-only type (`ref c: C` over an owning choice is rejected, since nothing could bind it): take the value by value and return it, `c := Step(c);` (§22.3).
- `ref`/`out` are **forbidden** on `async` methods, on **`async` lambdas**, on iterator methods and on indexers (§30.1, §29.3, §23.5). An **ordinary** lambda admits them, as its function type does — `var f: (ref int) -> () := lambda(ref x: int) => do x := x + 1; end;` (A.7 `LambdaParam`/`FuncParamMods`, §27.1).
- **A closure over a `ref`/`out` parameter.** A lambda may **capture** an enclosing `ref`/`out` parameter, of any type, but that closure may not outlive the call — no return, field, collection or `out`/`ref` store (§27.5), and in a synchronous body no task-returning call may invoke or receive it (a task outlives the call even when disposed); a closure's write to an `out` parameter does not count toward its must-assign, and a closure reads a captured `out` parameter or non-defaultable local only if it is assigned where the lambda is formed or first by the lambda itself, a recursive closure's own variable counting as assigned in `f := lambda … f(…) …` (§27.5); capture a copy (`var snap := x;`) to keep the value.
- **The argument's type.** The argument's static type must match the referent type exactly; a bare local or parameter counts at its **declared** type, never its §7.4/§12.7-narrowed one, both for the match and for inferring `T` at `ref x: T` (inside `if a is Dog then`, `G(ref a)` needs `G(ref x: Animal)`; to pass the narrowed value, `var d: Dog := a; G(ref d);`). A chain into a narrowed nullable value's own storage is rejected at `ref`/`out` (`ref n.X` over `n: Pt?` narrowed non-null, §7.4): write `var p: Pt := n; Inc(ref p.X); n := p;`. `out var _` is an error; write `out _`.
- **Inside the callee** a `ref` parameter **never narrows** — not by `<> null`, `is T`, assignment or an early exit — so `if a <> null then a.M(); end;` over `ref a: Animal?` is rejected: write `var c := a; if c <> null then c.M(); end;`, and `a := c;` to write back (§7.4).

**`scoped`** (§28.14) declares **storage-bounded retention** — the callee may store the thing, bounded by the storing object's life.

- On a **function-typed** parameter (`method Subscribe(scoped h: () -> ())`): the callee may store the callback in a `scoped` field of the bounding object, forward it to a same-bound `scoped` parameter, or invoke it. Function-typed arguments carry **no call-site marker**. Without `scoped` such a parameter is **call-bounded** — invocable during the call, never retained, not even in a local collection (a task or query it mints may wait in one until consumed) — and callers may pass any closure, except to a parameter whose return slot is a bare `T` that the call instantiates at a function type (§28.3's set-erasing positions). A plain function-typed **primary-constructor** parameter is call-bounded too: invoke it in initializers, but a body reference captures it into a field and is rejected (§22.10).
- On a **class- or interface-typed** parameter (not `Object` or `Object?`, where `scoped` is rejected, §28.14): the callee may store the borrow in a direct non-owned field of the bounding object. The call site writes `M(scoped x)` — an argument *modifier*, not an expression; a fresh producer is rejected there. A record or other value reaching an interface-typed `scoped` parameter by boxing is admitted only if it is not borrow-bearing (§28.14). A **primary-constructor** parameter may be `scoped` at these types — `class Box(scoped src: Source)`, used from a body as `src` — and its compiler-generated capture field is that direct non-owned field; the holder must still be a `using var` local (or `this`) with the source declared before it. A function-typed `scoped` primary parameter is rejected (§22.10).
- **The price: captured local sources are frozen** while the bounding object lives — no move, no reassign, and **no `Dispose()`**. `scoped` on a **static** method is admitted only when that method's return is `owned`.

### 4.3 The announcement table, as a decision procedure

**Everything is read off the *declared* return type**, before generic substitution, nullability stripped. Prose like "returns a fresh instance" carries no weight; only the signature does (§28.3).

**Caller side — what did I just get?** Classify the declared result type R in this order; first match wins.

| # | R is… | Instance call | Static call | Extension call | Function-value call |
|---|---|---|---|---|---|
| 1 | `owned T` (concrete class/interface, or bare `T`; at a value `T` the marker is erased and the result is a plain value, a borrow-bearing one carrying borrows as row 7 reads them) | **owned** | **owned** | **owned** | **owned** |
| 2 | **mint-announcing**, unmarked | **owned frame** | **owned frame** | **owned frame** | **owned frame** |
| 3 | mint-announcing, explicitly `owned` | identical to row 2 | | | |
| 4 | plain concrete class/interface, unmarked | **borrow**, source = receiver | **rejected at the declaration** | **borrow**, source = receiver parameter | **borrow**, sources = the function value's own set ∪ what every plain argument contributes (§28.3, *The sources an argument contributes*: a class, interface or function-typed one its sources; a span or other borrow-bearing value what it carries) |
| 5 | bare type parameter `T`, non-empty source set | **borrow**, sources = receiver ∪ every plain parameter whose type mentions `T` (a function-typed argument counts its captures where `T` is in its return slot or a `ref`/`out` slot, not `owned`: `Apply<Item>(lambda() => x)` lends from `x`). At a `T` that holds borrows (§3.6: a borrow-bearing record or inline array, or a choice that holds borrows) the result is a value whose carried borrows have those sources: `return Id<Pair>(new Pair(x));` is rejected, and so is returning `d.First()` past a scope-bound `d: Dictionary<int, Item>` (its element type, `DictionaryEntry<int, Item>`, is a borrow-bearing record) | same | same | same, but the sources are the function value ∪ every plain argument, as in row 4 |
| 6 | bare `T`, **empty** source set | cannot arise | **rejected at the declaration** — spell `owned T` | cannot arise | cannot arise |
| 7 | value type / `string` / runtime-managed | not applicable — no obligation (a span or other borrow-bearing value carries borrows: receiver-sourced from an instance or extension call, none from a static one, and at a function-value call the function value ∪ what every plain argument contributes, as row 4) | | | |
| 8 | move-only value type (owning inline array; owning choice) | **owned** move-only value | | | |

**The mint-announcing set**: the `Task` family (`Task`, `Task<T>`, `OwningTask<T>`) and the iterable family — `IEnumerable<T>`, `IEnumerator<T>`, `IAsyncEnumerable<T>`, `IAsyncEnumerator<T>`, `IOwningEnumerable<T>`, `IOwningEnumerator<T>`, `IOwningAsyncEnumerable<T>`, `IOwningAsyncEnumerator<T>` — plus any **lazy query**. A *call* returning one of these mints a fresh caller-owned frame at every callee kind. `IOrderedEnumerable<T>` and `IGrouping<TKey,T>` are not in the head set, but `OrderBy`/`OrderByDescending`/`ThenBy`/`ThenByDescending` mint by their lazy-query kind.

**Reads of storage are not calls.** A property, field, indexer or local read of *any* type — mint-announcing included — is a **borrow of its storage**. That is how a stored task or collection view is re-exposed.

**Callee side — what am I allowed to return?**

| Declared R | Every return path must supply |
|---|---|
| `owned T` | a fresh or owned value. A borrow is rejected. |
| bare `T` returned as `owned T`, or under `where T: value`, at a substitution that holds borrows (§3.6) | a value carrying only borrows §9.7 admits (receiver-sourced, static storage through a static property getter, or the null-source form), checked at that instantiation: `Pass<T>(owned a: T): owned T => a` is rejected at `Pass<Pair>`, and `Id<T>(a: T): T where T: value => a` at `Id<Pair>`. A lambda into a function slot returning such a type (or a concrete span, `CancellationToken`, or other record, inline array or choice with a class or interface component) may return borrows of its captures, its plain parameters or static storage, never of its own locals, `owned` parameters or temporaries; the call `g(x)` carries `g`'s and `x`'s borrows. |
| mint-announcing (marked or not) | a **fresh or owned frame**: a forwarded mint-announcing call result, a completed-task factory result, an owned frame local (a field's value arrives as one through the replacing `move`, `return move h.f with fresh;`, or bare at a nullable `f`), an `owned` parameter, or `null` under a nullable R. An `async` or iterator body satisfies this automatically. Returning a **borrow** (a bare field or property read, a plain parameter, a captured binding) is a compile error. One exception: a class implementing both `IEnumerable<T>` and `IEnumerator<T>` may write `return this;` in its own `GetEnumerator()`. |
| plain class/interface, **instance or extension** | only a **receiver-sourced** borrow: `this`, a bare read of a field, a borrow reached through the receiver's fields and indexers. A fresh owned value is **rejected** — declare `owned`. |
| plain class/interface, **static** | nothing — the signature itself is rejected. Static storage may be lent out only through a **static property getter**, never a static method. |
| bare `T`, non-empty source set | `null`, or a borrow sourced from the receiver, a plain source parameter, or static storage, checked at class, interface and borrow-bearing value substitutions. Invoking a function-typed source parameter counts the invocation's plain arguments too: `Map<A, B>(a: A, f: (A) -> B): B => f(a)` is rejected wherever `B` is a class, `Map<int, Item>` included; type the argument concretely (`a: int`). A fresh owned value, or a borrow of an owned local / `using var` / `owned` parameter / temporary, is rejected. |

Five corollaries you will need constantly:

1. **A static method returning a class type must say `owned`** (or return a mint-announcing type, or a bare `T` with a source): `static method Pick(a: Res, b: Res): Res` is rejected at the declaration.
2. **An instance method that creates something must say `owned`** — otherwise its return reads as a borrow of the receiver.
3. **A property or indexer getter may never return a fresh owned value.** Getters lend only; properties carry no `owned`. Expose a factory as a *method* returning `owned T`.
4. **An iterator method's unmarked iterable return is already a transfer announcement** — no `owned` needed, at any callee kind.
5. An **unbound owned temporary** is disposed at the **end of the full expression**, after every borrowing use inside it: `items.Where(p).ToArray().Length` is fine, while binding, storing or returning a borrow *of* such a temporary past that expression is rejected. A `case` scrutinee extends the boundary to the whole `case`, so pattern bindings survive the selected arm. Passing such a temporary to a plain parameter is admitted even when the callee stores the parameter in a field, and the temporary is still disposed at the end of the full expression: `holder.Hold(new Item());` leaves `holder` holding a disposed `Item` (a `DisposedAccessException` on later use in a checked build, undefined behavior in an unchecked one). Bind it first, in a holder that outlives the object (`using var item := new Item(); using var holder := new Holder(item);`), or declare the parameter `owned` or `scoped`. A property assignment is not such a call: `holder.Item := new Item();` is a store and is rejected unless the setter is `set(owned)` (§28.3).

**Awaiting a task — what the result is** (§30.5). Both task types are mint-announcing, so the *container* obligation is identical — the caller owns the task. The **result** rules differ:

| Task type, at result type T | What `await` yields | The awaited operand, and where the result may go |
|---|---|---|
| `Task` / `Task<T>`, T a value, `string`, or runtime-managed | the value; no ownership crosses the await | any form; the awaited expression may be a bare producer call |
| `Task<T>`, class T — **Pattern A** | a **borrow** of the one underlying result, whose lifetime is the task's ("disposing the task disposes the result"); the task may be awaited from multiple places, and every awaiter receives a borrow of that same result | the operand must be a **named location** — a `using var`/`await using var` local, an `owned` parameter, an owning-collection slot, a borrowing-iteration variable, admitted static task storage — or an unbound temporary whose borrowed result is used **within that same full expression**. The result borrow may flow to a `var` binding, a non-owned argument, or a `return` under the same Pattern A type; it may **not** flow to a `using var` binding, an `owned` field or slot, an `owned` argument, or an `owned T` return. `var x := await Producer();` — a fresh `var`/`const` whose initializer is *exactly* the await — is the **scoped await-bind**, desugared to a hidden pinned slot; `using var x := await …`, assignment to an existing local, and compositions like `(await E).Name` are not |
| `OwningTask<T>`, class T — **Pattern B** | the single **owned** result: awaiting transfers ownership. The task may be awaited at most once for a successful result; a second await through the same local or parameter binding that the first dominates is a compile error, and a second award reached any other way raises `InvalidOperationException` | an owned-consuming context: `using var x := await …`, `var x := await …`, an `owned` argument or slot store, a `return` under `owned T`, an owning `yield return`, or `_ := await …;`. The await **consumes its operand** when that operand is an owned task value — and the position is marker-free, so it takes **no `move`**; a **borrowed** operand (a `using var`, a plain parameter, a field, a slot) is only **spent** — its holder still disposes it, and a second `await` of the same `using var` or parameter that the first dominates is rejected (§30.4) |
| `OwningTask<T>` over a value, `string`, or runtime-managed T; either task type over a **move-only** value (an owning choice, an owning inline array) | — | a compile error |

Choose by intent: `OwningTask<T>` for resource acquisition (one fresh single-consumer result), `Task<T>` for fan-out (several subscribers reading the same eventual result). An `async` **lambda** takes its wrapper from its ascription, else from a closed target slot, else from its own body — and a body returning a fresh owned class infers `OwningTask<C>`, so at a generic `(…) -> Task<R>` slot such a lambda is **inapplicable** (the candidate drops out): write `async lambda(x): Task<C> => …` to bind `R` (§30.6).

### 4.4 Borrow rules

A borrow is valid only where **every** source in its source set is live; joins, `??` and arm meets take the **union** of source sets (§28.3).

- A borrow may **not** be assigned to a local whose scope strictly encloses its source's scope — including flowing around a loop back edge. Rejected **at the assignment**, not at the later use.
- A borrow of a **scope-bound local** may not be stored in a field or in a longer-lived collection. A borrow whose source *does* outlive the object **is** admissible: the **back-reference** idiom binds a constructor parameter into a plain (non-`owned`) class-typed field (`private _owner: Customer;`). The check is structural — sourced from a parameter that is not `owned` (plain or `scoped`) or from the receiver: admitted; locals, temporaries **and `owned` parameters**: rejected (an `owned` parameter is disposed at the method's end, so it is scope-bound — §28.3).
  - The result of invoking a function-typed parameter counts as sourced from that parameter and from the call's arguments, a function-typed argument contributing what it captures and a span or other borrow-bearing value argument what it carries: `_v := make()` with `make: () -> Item` is admitted; `_v := pick(local)` and `_v := wrap(lambda() => local)` are not (§28.3).
  - Assigning a class- or interface-typed value to a class's or interface's property is that store whatever the property's accessor form: `obj.P := local` (a scope-bound local) and `obj.P := new T()` are rejected, a parameter- or receiver-sourced borrow is admitted, and a `set(owned)` property takes the value instead (§28.3).
  - A settable **static** property's assignment is the store into a mutable static field of its type: `Config.Current := new Settings()` is admitted only at a type a mutable static may have (a synchronized handle, `Task?`), where the value lives to process exit, and is rejected at any other; assigning a `static readonly` auto-property in its type's static constructor is the `static readonly` field store and is admitted (§28.3).
- A borrow may not be returned unless its source is one every caller can re-establish: receiver-sourced from an instance method, the receiver-parameter borrow of an extension method, or static storage through a **static property getter**.
- A `ref` or plain `out` argument whose type can carry a borrow (a class or interface type, or a borrow-bearing record, choice or inline array; at a function type the callee can fill it only capture-free, so an `out` one is capture-free after the call and a `ref` one keeps the set it had) is a holder the callee may fill from any of the call's borrows — the receiver, the plain arguments, the other `ref` arguments' values — so each must be admissible into it exactly as `location := borrow` would be: `Put(ref outerLocal, innerUsingVar)` is rejected at the call (§28.3).
- A closure — or a method reference over an instance receiver — is a **borrow-carrying value**: its source set is read off its provenance — a lambda's captures' sources, a method reference's receiver, a plain function-typed parameter of yours itself, an invocation result the invoked value's set, a read from a field, collection, property or indexer nothing — and every rule in this list applies to it by that set (§28.3); the *Closures by position* table below says where one that captures anything may go.
- Every field-, collection- or async-frame-held borrow carries the obligation not to transfer the borrowed object to another domain. This has **no runtime backstop** — it is a data race (§30.14).

**Closures by position** (§28.3, §28.14). A function type announces no sources, so a closure that captures anything may go only where its set is still checked. A capture-free closure goes anywhere, and so does a function value read from a field, a property, an indexer or a collection, or returned by a method call: it is capture-free by construction.

| Position | A closure that captures anything |
|---|---|
| a local, plain `var` or `using var`, whose scope does not strictly enclose a source's; an invocation; an argument to a function-typed parameter | **admitted**. A local helper lambda over a borrow needs no `using var`; reassigning a plain function-typed local is fine — each assignment is checked on its own, and where a closure captures the local, every assignment to it, in any body, counts toward that closure's set; `using var` on a function-typed local adds only non-reassignability |
| a store — a field or a collection, a property or indexer, a record component declared at a function type (at construction or in `with`), a choice payload declared at a function type, a slot of an inline array of function type (a local array included); a write through `out`/`ref`; a `return` (`yield return` included) | **rejected** — the only storage route is a `scoped` parameter (§28.14) |
| a set-erasing position: a parameter declared `Object` (the receiver of an `extension Object` call, an explicit `f as Object`, and `f as T` in your own generic body, included), or one at a bare type parameter that your call instantiates at a function type or at `Object` (`k.Put(f)` at `k: Keep<Object>`, `new Box<Object>(f)`) — likewise a lambda or a function value passed to a parameter typed `() -> T` at such an instantiation — or a field declared at a bare type parameter, written through a receiver instantiated at a function type or at `Object` (`b.V := f` at `b: Box<() -> int>` or at `b: Box<Object>`) | **rejected** — `x.Equals(f)`, and `Pick(f, g)` over `Pick<T>(a: T, b: T): T`; here a function-typed parameter of yours counts as capturing (it carries itself). Write `f = g` for equality and `(if c then f else g)` for a selector |

Do not form a method reference whose parameter is a function type over such a parameter, an `out` one excepted (`var add := xs.Add;` with `xs: Array<() -> int>` is rejected; `var tg := d.TryGetValue;` with `d: Dictionary<string, () -> int>` is fine): call `xs.Add(f)` directly at each site; a lambda `lambda(g) => xs.Add(g)` is rejected too, since its own function-typed parameter carries itself (the set-erasing row above). And a generic declaration never nests a type parameter inside a function type inside a function type, in its signature or in a type its body writes — `reg: ((T) -> ()) -> ()` and `reg: (() -> T) -> ()` are errors; take `(T) -> ()` or `() -> T` directly (§28.3).

**Merges must agree** (§28.3). owned ⊓ owned = owned. borrowed ⊓ borrowed = borrowed, sources unioned. **owned ⊓ borrowed = rejected.** owned ⊓ moved-from = maybe-consumed (unreadable). The rule governs `if` statements, `if` expressions, `??`, and expression-`case` arms identically.

**The consume-site freeze** (§28.3). Consuming an owned **binding** while a **live local borrow of its current value** exists is a compile error. Frozen: every transfer (a `move` at an argument, binding, assignment or arm; a store into an `owned` field, an owning slot or a `set(owned)`; a `return` under an `owned` or mint-announcing return, or of an `async` body's result under `OwningTask<T>` or a class- or interface-typed `Task<T>`; an owning iterator's `yield return`) and drop-on-overwrite. **Not** frozen — these are site-announced kills, permitted while borrows live: an explicit `Dispose()` (including `x?.Dispose()`), the discard `_ := x;`, `throw x`, and a Pattern B `await` on an owned `OwningTask<T>` binding — a span over the binding's value included. But a use of a borrow of the killed value — an alias, a `const`, a `using var` pin, a span or borrow-bearing record over it, a closure capturing one of these, a frame carrying it — that an explicit `Dispose()`, `DisposeAsync()` or discard of **every** source of the borrow always precedes is a **compile error** at the use (§28.2), a reassignment of the source in between lifting nothing; dispose after the last use, or re-borrow from the new value. A kill the compiler cannot prove precedes the use (one branch; the end of a loop body before the next iteration) leaves the use to the run time: a span read goes through the buffer's or array's own indexer, so after the kill it faults in checked mode like any other stale read (§28.2). A local holding an iterator frame or a lazy query (`var it := Each(xs)`, `var q := xs.Where(f)`) is a live borrow of every source it carries, like a closure, until the frame is disposed (its disposal may run the iterator's pending `finally`). Fields and collection slots are **not** frozen. The mechanical rewrite for the frozen case is **transfer first, then re-borrow from the new owner**.

**Borrow-bearing records** (§9.7). A record is borrow-bearing if it has at least one **borrow-carrying** field: one whose declared type is a reference type (class or interface, nullable or not), an inline array whose element type is a reference type or a borrow-bearing record or choice (`[2]Pair` for a borrow-bearing `Pair`), or another borrow-bearing record or choice. The predicate reads **declared field types**, never runtime contents — so `string`-, `Type`- and function-typed fields are *not* borrow-carrying, while a field declared `Object` or an interface type **is**, nullable or not. A nullable `Pair?` holds exactly what `Pair` holds, written or reached by substitution (§3.6 *Nullable forms*).

Such a value:

- may not outlive the source of any borrow it carries;
- may **not be constructed from a temporary** (bind the source to a named local first);
- may **not** be stored in a class instance field — store the *source* (an `owned CancellationTokenSource`, not a `CancellationToken`) and re-derive the view at the use site;
- may sit in a **static** field only in the null-source form (every reference-typed field, active-case payload and inline-array slot `null`, so a payload-free choice case is null-source, through nested records and choices too — a spelling **no span has**, its `Buffer`/`Source` being non-nullable, so spans go in neither static nor instance fields);
- may be **returned** from a member only when every borrow it carries is **receiver-sourced** — the span shape `public method Header(): Span<byte> => _buf.AsSpan(0, 64);` over an `owned _buf: OwningBuffer<byte>` field is admitted, while a return carrying a borrow of a local, a `using var`, an `owned` parameter, or a **plain parameter** is rejected at the return site (declare it as an instance or extension member of the source's own type instead, whose receiver the caller re-establishes);
- may not be an `async` parameter, result or element type, a local of such a type crossing a suspension point being admitted only if every borrow it carries has a frame-resident source (`CancellationToken` is the single stdlib-certified exception).

An inline array whose slots carry borrows (`[2]Item` for a class `Item`, `[2]Pair` for a borrow-bearing record `Pair`) is under the same `async` rules (§15.1, §30.1): pass an `Array<Item>`, or the elements as separate parameters. Such an array's borrows are checked as **one set**: a slot store `xs[i] := e` adds `e`'s sources and never removes one, so overwriting a slot does not release the borrow it held; a slot read carries the whole set, and a live array local freezes every source in it against a consume (§15.1.2).

Records also cannot have destructors, `owned` fields, owning-inline-array fields, or implement `IDisposable`. The spans are the canonical borrow-bearing records; `ReadOnlyStringSpan` is the deliberate exception — its only reference field is a `string`, so it may sit in fields and cross `await`.

**Value receivers are borrows** (§9.4). In every instance member body of a **value kind** — a record method or accessor, a choice method, an enum member, an extension member over a value receiver — `this` binds a **borrow** of the receiver value, and a store that would land in the receiver's own storage is a compile error at the member body: `this.X := e`, a bare `X := e` resolving to receiver storage, an inline-array chain, compound assignment and `++`/`--` through their desugarings, and passing such a chain as a `ref`/`out` argument. The rule keys on *where the store would land*. Two boundaries: **constructor bodies are not receiver positions** (`Width := width;` is the canonical body-record initialization), and an **external** store through a named binding (`r.X := 5`) remains legal.

- A value kind may not conform to an interface declaring a settable property or indexer at all (§9.6). The sanctioned mediating shape is a chain that hops **through a class- or interface-typed member**, leaving the receiver — that is how `Span<T>`'s indexer setter works.
- The receiver value is **fixed at the call** for a copyable kind: a closure the member invokes may overwrite the local or field the receiver came from, and the member keeps reading the entry value. A **move-only** receiver (an owning choice) is borrowed in place instead — `_conn.IsOk()` on a mutable field is fine — and reads like a class receiver: a store to the field during the call is visible through `this` afterwards, its displaced payload objects disposed.
- In a borrowing `case` over a mutable field, over `this`, or over a `ref` parameter, never use a binding of a **move-only payload** (an owning inline array, a nested owning choice) after a call that is not `pure`, which includes `Console.WriteLine` and any loop that calls something. Do not call such a binding's own non-pure members or operators either, or pass a lambda that captured it to a non-pure call or invoke one. Field and auto-property reads through it are fine. Read what you need first, or match the value through `case move h.C of …`.
- An `async` or iterator member cannot be called on a move-only receiver (§9.4, §19.3).

### 4.5 Iteration and collection ownership

**The two families** (§28.11):

| | Borrowing family | Owning family |
|---|---|---|
| Types | `Array<T>`, `LinkedList<T>`, `Dictionary<K,V>`, `Set<T>`, `Stack<T>`, `Queue<T>`, `SortedDictionary<K,V>`, `SortedSet<T>` | `OwningArray<T>`, `OwningLinkedList<T>`, `OwningDictionary<K,V>`, `OwningSet<T>`, `OwningStack<T>`, `OwningQueue<T>`, `OwningSlotMap<T>` (handle-keyed; no borrowing twin) |
| Element slots at class T | **borrows** — the caller keeps ownership | **owned** |
| `Add`/`Insert`/indexer-set, and `GetOrAdd`'s key and factory result | stores the borrow, checked at the call | `Add(owned item: T)`; indexer-set disposes the previous value |
| `Remove`/`RemoveAt`/`Clear` | removes the borrow | **disposes** the element(s) |
| `Dispose()` | no user-visible work | disposes **every** element, then frees storage |
| Ownership out | *none* | `move coll[i] with e`, `PopAt`/`PopFirst`/`PopLast`/`Pop`/`Dequeue`, `Drain()` |

There is **no implicit conversion between the families**. Owning collections require `where T: class`, and a generic type holding one at its own `T` states it too (`class Pool<T> where T: class`; §14.2). A class owning a collection of resources writes `private owned _items: OwningArray<T>;` and needs no destructor.

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

**Mutating what you iterate, and using what you removed** (§29.12, §28.11). `for var x in xs do … xs.Remove(x); … end;` compiles and **warns**: the next advance raises `InvalidOperationException`. Silent and correct: collect the matches into a second collection and remove after the loop; `xs.Remove(x); break;` (no advance follows); an indexer-set `xs[i] := v` (element-level, not structural); or `while xs.Count > 0 do using var x := xs.PopLast(); … end;` on an owning collection. On an **owning** collection `var w := xs[0]; xs.RemoveAt(0); w.Ping();` compiles **silently** and faults in checked mode: `RemoveAt` disposed the element `w` borrows, and no diagnostic tracks the slot (§7.5). Re-take the borrow after the mutation, take the element out first with `PopAt(0)`, or mutate after the last use. The iteration warning does not fire through a different binding (`var ys := xs;`, or a span over `xs`: its `Reverse()` still faults at the next advance, its `Fill`/`CopyFrom` do not), and is not suppressible per site.

**Materializing is not a liveness remedy at class element types**. `ToArray()`/`ToSet()`/`ToDictionary(…)` there build **borrowing** collections, so the materialized slots dangle exactly as the original borrow did; materialize-first is the **re-enumeration** remedy over a *live* backing, nothing more. What does remedy a dead-on-arrival element borrow: consume each element inside the loop body that borrows it, or give the elements an owner (an `IOwningEnumerable<T>` producer, consumed by owning iteration into an owning collection, with the accessor applied to *that* collection). Where the producer is a borrowing minting iterator you cannot change, **no retention remedy exists** (§29.11).

**Lazy queries are single-consumption** (§29.11): enumerated at most once. The right is spent by enumeration or a terminal operator (an explicit `q.GetEnumerator()` call included — it begins the enumeration), by applying a further deferred operator (`var q2 := q.Select(f);`), or by passing the query as an `IEnumerable<T>` argument. It is orthogonal to ownership.

**Element disposal is the owner's, always** (§28.6). `Dispose()` requires a receiver you own: an owned local or `owned` parameter, a `using var`, an unbound owned temporary, a `move` result, or an `owned` field / owning slot read by code of the owning object's declaring type. Through a **borrow it is a compile error with no exception** — a borrowed local or parameter, **`this`**, a non-`owned` field read, an owned-field read outside the declaring class, a getter result, a `catch`-arm binding, a pattern binding, a captured owned binding inside a closure, and an element borrow of *either* family. A borrowed **owning collection** may be added to, removed from, drained and read — the pass-and-fill idiom `method Fill(a: OwningArray<T>)  a.Add(…)` is core — but it may not be `Dispose`d.

**Fresh values into collections** (§15.4, §28.11). On the **owning** family a fresh producer is idiomatic and warning-free: `pool.Add(new Connection("a"));`. On the **borrowing** family the same shape via `Add`/`Insert`/indexer-set is a compile **error** (the temporary would die at the statement's end and the slot dangle) — and so is a fresh class-typed **key** into an `OwningDictionary`, whose keys are borrowed (bind the key first: `using var k := new K(); od[k] := v;`), and a fresh non-trivial-dispose element inside a collection **literal** is the same error at the literal site — bind first, or use the owning family. A user-declared member's plain parameter is not a store position: `sink.Consume(new X())` compiles, the temporary dying at the statement's end. Runtime-managed elements are unaffected: `summary.Add(name)` on an `Array<string>` carries no obligation and no diagnostic.

### 4.6 The diagnostics you will actually hit

Section 7 lists each with its remedy. In frequency order they are: the **must-use warning** on an undischarged plain-`var` owned local; a **mixed owned/borrowed merge** rejected at the join; *"borrow held by `x` would outlive its source `y`"*, reported at the **assignment**; *"this call consumes `x`; write `M(move x)`"* and its inverse (the marker written where the target already announces); *"cannot move out of `using var`"*; a **non-nullable field or slot `move`d without `with`**; a fresh owned value **escaping into a non-owning location** (a borrow field, a getter, a plain return); a **static** method's plain class-typed return, rejected at its declaration; `Dispose()` **on a borrow**; the **consume-site freeze**; the **fresh-store error** on a borrowing collection (and on a fresh `OwningDictionary` key); the **terminal-accessor provenance warning**; the **mutation-during-iteration warning** (§4.5); and `_ := someTask;` / `using var _ := someTask;`. One more is opt-in: build with `--report-trust-admissions` (spec §28.3's trust-admission report mode) and every borrow of a parameter, `this` or a field you store in a plain field or a collection, every transfer of a local or `owned` parameter that a collection may still hold a borrow of, and every abandoned task frame draws a warning naming the lifetime you are trusting and the static spelling that would prove it (`scoped`, a `SlotHandle`, `Shared<T>`, the owning family, `await`); a unit that stays silent under the flag holds no trusted lifetime at all.

<a id="sec-recommended-diagnostic-wordings"></a>
**Recommended wordings.** The spec fixes what each of these diagnostics names (spec §28.3 *Diagnostics*, §28.10's adoption rule, §30.5's near-miss forms, §35.5); the words below are its recommendations, kept here as teaching text. The compiler's own text may differ: match a diagnostic by what it names, not by its words.

| Diagnostic (spec rule) | Recommended wording |
|---|---|
| An owned local goes out of scope undisposed (K3.14) | *"Owned value in `x` would be dropped here without disposal."* |
| A borrow escapes (K2.1/K2.3) | *"Borrow held by `x` would outlive its source `y`."* |
| A moved-from local is read (K1) | *"Owned value in `x` was already moved out at line N."* |
| A `using var` is moved (A5) | *"Cannot move out of `using var` `x`; `using var` is scope-bound."* |
| The consume-site freeze (K5; reused by §35.5) | *"Cannot consume `x` here: the borrow held by `b` (declared at line N) is still live."* |
| A use after a dominating explicit disposal (§28.2) — through the variable, or through a holder of a borrow of its value | *"Use of `b` after the explicit disposal of `a` at line N, which precedes this use on every path; `b` borrows the value `a` held there. Dispose after the last use."* |
| A use after a dominating field write (§28.7) — a local borrow of an owned field's value, or the field itself after its explicit disposal, in one straight run of code | *"Use of `b` after the write to `_w` at line N, which precedes this use on every path with nothing between the borrow and the write that could have moved the value out; `b` borrows the value `_w` held at line M. Use `b` first, or take the value out with `move _w with …`."* |
| An unmarked `using var` over a named owned binding, or over an `if`/`case`/`??` arm-meet of such bindings alone (§28.10; over a `using var` source only the view is offered, and for `using var _ := x;` the view is replaced by deleting the declaration) | *"`using var c := conn;` does not say whether it adopts or views the owned binding `conn`: write `using var c := move conn;` to adopt it, or `var c := conn;` / `const c := conn;` to view it."* — for the arm form, *"`using var best := (if c then a else b)` does not say what happens to `a` and `b`: mark the arms, `(if c then move a else move b)`, to adopt whichever is selected, or write `var best := …;` / `const best := …;` to view it"* (over `a ?? b`, the rewrite `(if a <> null then move a else move b)`). |
| `using var x := await FetchAsync(url);` (§30.5) | *"`await` on a `Task<Config>` produces a borrow of the task's result, which §30.5's may-not-flow list bars from a `using var` binding. No pin is needed — write `var x := await …;` and the compiler pins the task (the result is valid to the end of this block), or use the explicit two-line form to name the task."* |
| `x := await FetchAsync(url);` into an existing local (§30.5) | *"the scoped await-bind applies to a fresh `var`/`const` declaration only: the hidden task is pinned to this block, but `x` is not this declaration's binding. Bind the task first: `using var task := …; x := await task;` (§30.5)."* |
| A payload case without a payload pattern (§35.5) | *"Payload case `Fetched` requires a payload pattern: write `Fetched(...)` with 1 slot, or `Fetched(_)` to discard."* |
| A misplaced `case move` (§35.5) | *"`case move` requires a named owned binding of an owning choice type as its scrutinee; `x` is borrowed."* / *"… is a fresh producer, which needs no marker."* |
| An owning choice at a property-keyed position (§35.5) | *"`FetchOutcome` is an owning choice (owned payload `response` of case `Fetched`); it cannot be boxed / is not sendable / does not satisfy `new()`."* |
| A refutable slot pattern leaves a case uncovered (§35.5, §19.6) | *"Case arm does not cover `Failed`: its slot pattern `>= 500` is refutable; static exhaustiveness counts only irrefutable-slot arms."* |

---

## 5. The standard-library index

Everything below is transcribed from `Bezel_Standard_Library.md` v0.348 with its markers intact. **A signature with its markers stripped is worse than no signature**: `owned`, `pure`, `ref`, `out`, `scoped`, nullability and constraints are load-bearing. **If a name is not here, look it up in the stdlib document; if it is not there either, it does not exist.**

### 5.0 Modules and imports

| Module | Import | Contents |
|---|---|---|
| `System` | auto | `Console`, `Math`, `Convert`, `Environment`, temporal types, `Guid`, `Random`, the core interfaces, `Type`, the serialization vocabulary, `Shared`/`Weak`, `Result`, `ScopeGuard`, `Subscription`, the FFI carriers |
| `System.Text` | auto | `StringBuilder`, `Encoding`, `ReadOnlyStringSpan` |
| `System.Collections.Generic` | **auto** | every §4 name — the collections, the spans, `Counter`, `FrozenArray`, `IEqualityComparer`, `KeyNotFoundException`, the iteration interfaces. An explicit `import` line is legal and redundant. Beware the bare names when naming your own types: a same-named type of the same arity in your module shadows the import; a same-named export from another imported module, at the same generic arity (a non-generic `Counter` does not clash with `Counter<K>`), is an ambiguity error — as for every always-available name (`Task`, `Type`, the exception types, …; `Object` never collides). Qualify the import with its module name or an alias, or the built-in with its module: `System.Exception`, or `System.Threading.Task` with `import System.Threading;` (§2.3) |
| `System.Collections.Linq` | **auto** | the query operators. No `import` needed — §2.3's invariant: a stdlib extension over an always-available type is auto-imported |
| `System.Threading` | **explicit** | *except* the `Task` family, `CancellationToken`, `Subscription`, and the six event types, which are auto — `CancellationTokenSource` itself needs the import |
| `System.IO` | **explicit** | `Stream`, `FileStream`, `File`, `TextReader`/`TextWriter`, `Directory`, `Path` |
| `System.Diagnostics` | **explicit** | `Stopwatch`, `Debug` |
| `System.Serialization` | **explicit** | `JsonSerializer`/`JsonDeserializer`/`BinarySerializer`/`BinaryDeserializer`/`JsonOptions`/`SerializationException`/`Json`/`Binary` (the facades not yet implemented, #2349) |
| `System.Interop` | **explicit** | `Ptr<T>`, `Interop` |

**Deferred — do not use:** `System.Net`, `System.Net.Http` (**there is no `HttpClient`**), `System.Text.Json` DOM, `System.Text.Regex`, `System.Security.Cryptography`, `System.Globalization`, `System.Reflection`, `System.Diagnostics.Process` (stdlib §11).

### 5.1 The Try shapes and the `*OrNull` tier

Exactly four sanctioned shapes; nothing else is a Try (stdlib §1).

| Shape | Signature form | When | Members |
|---|---|---|---|
| **(a)** bool + **nullable** out | `TryX(…, out item: T? when true): bool` (`out owned` where the payload transfers) | generic or **class**-typed payload | `Dictionary`/`OwningDictionary`/`SortedDictionary`.`TryGetValue`; `Stack`/`OwningStack`.`TryPop`,`TryPeek`; `Queue`/`OwningQueue`.`TryDequeue`,`TryPeek`; `OwningArray`/`OwningLinkedList`.`TryPopFirst`,`TryPopLast`; `OwningSet.TryPop`; `OwningDictionary.TryPopValue`; `Channel<T>.TryReceive`; `IParsable<T>.TryParse` |
| **(b)** nullable **return** | `TryX(…): owned T?` | acquisition-shaped, payload statically non-nullable | `Mutex.TryAcquire`, `Semaphore.TryAcquire`, `Weak<T>.TryUpgrade`, `Locked<T>.TryLock`, `RwLocked<T>.TryRead`/`TryWrite`, and their `*Async` forms |
| **(c)** bool + **non-nullable value** out (`default(T)` on false) | `TryX(…, out result: T): bool` | concrete **value**-type payload | the primitive `TryParse` family; `Convert.TryTo*`; `DateTime`/`DateOnly`/`TimeOnly`/`Guid`.`TryParse`; the span/bit `TryRead*` accessors |
| **(d)** bool only, no out | `TryX(…): bool` | succeeded/declined; the payload was passed *in* | `Set.TryAdd`, `OwningSet.TryAdd`, `SortedSet.TryAdd`, the three dictionary `TryAdd`s, `TaskCompletionSource.TrySet*`, the span/bit `TryWrite*` |

**The `when true` postcondition is normative on every shape-(a) member** even though the per-type listings elide it: inside a `true`-narrowed region the recipient needs no `!`. It has one spelling — the clause; a hand-written `static ensures(not result or p <> null)` is an ordinary contract clause and narrows nothing (§22.16).

**Three access tiers over one operation:** the **throwing** tier (absence is a bug) — `Parse`, `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`, `Console.ReadKey`; the **`*OrNull`** tier, returning `T?` for `??`/`?.` composition — `ParseOrNull`, `GetOrNull`, `Console.ReadKeyOrNull`, `FirstOrNull`/`LastOrNull`/`SingleOrNull`/`ElementAtOrNull`/`MinOrNull`/`MaxOrNull`, `E.FromUnderlyingOrNull`, `E.FromNameOrNull` (`E` = the enum's own name; the declared identifier, exact ordinal match — never a `serialized` rename; its inverse `E.NameOf(v): string` is total and equals `nameof(E.X)` for a written `X`); and the **`Try`** tier, the only shape that distinguishes "absent" from "present null". `*OrNull` **conflates** absent with a present `null` wherever the payload can be null — use `ContainsKey`/`Count`/the `Try` shape when the distinction matters.

**Negative magnitudes fault:** `capacity`/`size`/`length` < 0 raises `ArgumentOutOfRangeException` on the `StringBuilder`, `Array<T>`, `OwningArray<T>`, `Dictionary`/`OwningDictionary`/`Set`/`OwningSet` capacity forms and `OwningBuffer<T>`. Zero is valid. `Channel<T>(capacity)` requires `capacity >= 1`, else `ArgumentException`.

**Naming conventions.** The `Async` suffix marks the async half of a sync/async pair only; async-only operations carry no suffix. Borrowing/owning duals are **same-name overloads** where their parameter types differ (`Task.WhenAll`) and **distinct names** where they would differ only by `owned` (`TryGetValue`/`TryPopValue`): `owned` never distinguishes overloads. Surviving `Owned` names: `Json.ParseOwned<T>`, `Binary.ParseOwned<T>`, `Task.FromOwnedResult`. **An instance member blocks a same-named extension, by name, at any arity** — on an `Array<T>` receiver, `Contains`, `Count`, `Reverse`, `ToArray` resolve to the *instance* members, so `arr.Count(pred)` does **not** compile; write `arr.Where(pred).Count()`.

**Exception safety when a mutator re-enters user code:** the basic guarantee only. The receiver stays valid and every owned element is still disposed exactly once; contents are otherwise unspecified. No member promises all-or-nothing.

### 5.2 `System` (auto)

```bezel
public static class Console
  public static method Write(value: string);   Write(value: Object);
  public static method WriteLine();  WriteLine(value: string);  WriteLine(value: Object);
  //   WriteLine appends Environment.NewLine; Write appends nothing. Console.Write/WriteLine
  //   ARE Console.Out.Write/WriteLine (one destination, call order). No overload takes string?
  //   or Object?: print a maybe-null value as $"{x}" (absent renders `null`, §18.5) or x ?? "…".
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
  public static method Sign<T>(x: T): int where T: INumber<T>;   // NOT pure; -1/0/1; NaN → 0; no char argument
  public static pure method Fmod(x, y: double): double;   IEEERemainder(x, y: double): double;
end
```

`Min`/`Max`/`Clamp` at a **class** substitution return a **borrow** sourced from the arguments. `Min` returns `a` when `a.CompareTo(b) <= 0` else `b`; `Max` returns `a` when `a.CompareTo(b) >= 0` else `b` — **a tie returns `a`**; `Clamp` returns `low`/`high`/`value`. A **tick is 100 ns**: `TimeSpan.From<Unit>(n)` is *n* of that unit (truncated toward zero to a whole tick; `OverflowException` out of range or on `NaN`) and `Total<Unit>` is the whole span in that unit including its fraction. A **seeded** `Random` replays: same seed + same calls = same values; the algorithm is unspecified, so never assume a sequence across implementations.

```bezel
public static class Convert     // string <-> T only; there is no Convert.ToInt32(double)
  ToInt32/ToInt64/ToDouble/ToDecimal/ToBoolean(value: string);
  TryToInt32/.../TryToBoolean(value: string, out result: T): bool;         // shape (c)
  ToString(int|long|float|double|decimal|bool);   ToString(value: int|long, toBase: int);  // bases 2/8/10/16
  // = v.ToString() / v.ToString(toBase): Convert.ToString(0.1f) = "0.1";
  // char/byte/sbyte/short/ushort/uint/ulong are AMBIGUOUS (spec §22.6) — call c.ToString(), or cast
end                                                    // faults: FormatException, OverflowException

public static class Environment
  public static readonly property NewLine, MachineName, UserName, OSVersion: string;   // UserName NOT YET IMPLEMENTED (#5579)
  public static readonly property ProcessorCount: int;
  public static readonly property CommandLineArgs: IReadOnlyList<string>;   // NOT YET IMPLEMENTED (#5579)
  public static method Get(name: string): string?;
  public static method GetInt(name: string): int?;   GetBool(name: string): bool?;
  public static method Set(name: string, value: string?);      // null removes
  public static method Variables(): owned Dictionary<string, string>;
  public static method Exit(exitCode: int);   // never returns and unwinds nothing: no `finally`, no `using var` of any live frame; called inside the exit pass it ends the pass, `exitCode` winning
                                              //   runs; only the static `owned` exit pass runs (§33.3)
end
```

**Temporal types — all records (value types).** `DateTime`: constructors `(y,m,d)` and `(y,m,d,h,min,s, millisecond := 0, kind := DateTimeKind.Unspecified)`; `Ticks: long` with `FromTicks`/`ToUnixTimeSeconds`/`FromUnixTimeSeconds`; the `Year/Month/Day/Hour/Minute/Second/Millisecond/DayOfYear: int` components plus `DayOfWeek`, `Kind`, `Date: DateOnly`, `TimeOfDay: TimeOnly`; statics `Now/UtcNow/Today/MinValue/MaxValue`; `AddYears/AddMonths(int)`, `AddDays/AddHours/AddMinutes/AddSeconds/AddMilliseconds(double)`, `Add(span: TimeSpan)`, both `Subtract` forms, `ToUtc()`, `ToLocal()`, `ToString([format])`, `Parse`/`TryParse`/`ParseOrNull`, `CompareTo`, and the `+`/`-` operators over `TimeSpan`. Parse accepts only `yyyy-MM-ddTHH:mm:ss[.fff]` and `yyyy-MM-dd`. `TimeSpan`: `Ticks`/`FromTicks`, the `Days/…/Milliseconds: int` and `TotalDays/…/TotalMilliseconds: double` pairs, statics `FromDays/FromHours/FromMinutes/FromSeconds/FromMilliseconds(double)`, `Zero/MinValue/MaxValue`, `ToString([format])`, `+ - * /` (`*`/`/` take `double`) and unary `-`. `DateOnly` (`(y,m,d)`, `Year/Month/Day`, `ToDateTime(time: TimeOnly)`) and `TimeOnly` (`(hour, minute, second := 0, millisecond := 0)`, `Hour/Minute/Second/Millisecond`) each add `CompareTo`, `Parse`/`TryParse`/`ParseOrNull` and `ToString([format])`. `enum DayOfWeek: byte`; `enum DateTimeKind: byte { Unspecified, Utc, Local }`.

`Guid` (record) — `CompareTo`; `static FromByteArray(bytes: Array<byte>): Guid`; `static readonly Empty`; `static NewGuid(): Guid` (**not cryptographic — never a secret**); `Parse`/`TryParse(out result: Guid)`/`ParseOrNull`; `ToString()`; `ToString(format)` (`"N"`/`"D"` only); `ToByteArray(): owned Array<byte>`. **No constructor** — `new Guid(…)` is a compile error.

The seven temporal format specifiers (`yyyy MM dd HH mm ss fff`) each render a fixed-width zero-padded field of their own letter count — `dd` renders day 5 as `05`, `yyyy` renders year 42 as `0042`; any other run length of those letters is a `FormatException`.

`Random` (class; **sendable** — copy state, crosses as an owned transfer) — `constructor()`, `constructor(seed: long)`; `Next(): int`, `Next(maxValue)`, `Next(minValue, maxValue)`, `NextInt64(): long`, `NextDouble(): double`, `NextBytes(buffer: Array<byte>)`, `NextBoolean(): bool`.

```bezel
public interface IComparable<T>    method CompareTo(other: T): int;   end
public interface IEquatable<T>     method Equals(other: T): bool;     end
public interface IDisposable       method Dispose();                  end   // every class implicitly implements
public interface IAsyncDisposable: IDisposable  method DisposeAsync(): Task;  end
public interface IFormattable      method ToString(format: string): string;   end
public class Subscription  destructor; end     // no public ctor; returned as `owned Subscription`
```

`IComparable<T>`/`IEquatable<T>` are **invariant**. `BitConverter`: `FloatToInt32Bits`, `Int32BitsToFloat`, `DoubleToInt64Bits`, `Int64BitsToDouble`, `Reinterpret<TTo>(value)`. `Type` (`public sealed class`; **sendable**, crossing as a shared reference like `string`, so `Array<Type>` and a record with a `Type` field are sendable too; not yet reachable from a running program, #3819): `Name` (the identifier alone), `FullName` (module path + enclosing-type path + identifier, `"M.Outer.Inner"`; an enclosing generic shows `_` per parameter, `"M.Tree<_>.Node<int>"`; the root is `"System.Object"`), `IsClass/IsInterface/IsRecord/IsEnum/IsChoice/IsPrimitive/IsInlineArray/IsFunctionType/IsValueType`, `BaseType: Type?`, `IsAssignableFrom(other)` (interface conformance is `iface.IsAssignableFrom(t)` — there is no `Implements`), `EnumNames`, `EnumValues` — **no member reflection at all** (declined, not deferred).

**Format specifiers** (invariant culture only; an unrecognized specifier raises `FormatException`): integers `D`/`Dn`, `X`/`x`/`Xn`, `B` (no grouping separators); `double`/`float` `F`/`Fn` (default 2), `E`/`En`, `G`/`Gn`, `P`/`Pn`, `R`; `decimal` `F`/`Fn`, `E`/`En`, `G`, `P`/`Pn` (midpoints half-to-even); `DateTime`/`DateOnly`/`TimeOnly` patterns of `yyyy MM dd HH mm ss fff` with every other character a literal; `TimeSpan` `c`; `Guid` `D`/`N`; `bool`, `char`, `string` take **none** — a format spec on those is a compile error. **`C` (currency) deliberately does not exist**: write `$"{amount:F2} EUR"`.

**Exceptions** (spec §31.4):

```bezel
public class Exception
  public pure constructor();   public pure constructor(message: string);
  public constructor(message: string, cause: Exception);     // snapshot chaining; cause NOT retained
  public readonly property Message: string;   StackTrace: string;
  public readonly property CauseChain: Array<ExceptionInfo>;       // outermost cause first
  public readonly property SupersededChain: Array<ExceptionInfo>;  // chronological
  public override method ToString(): string;                       // NOT pure
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

`Exception.ToString()` is a **declared override** for `Exception` and every derived type. A conforming rendering contains the qualified type name, the `Message` when non-empty, every `CauseChain` entry (outermost first) and every `SupersededChain` entry (earliest first, distinguishable from causes). It renders **no stack trace**. Arrangement is implementation-defined; the member is **not `pure`**.

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
public class ScopeGuard                                  // the sanctioned `errdefer`; NOT YET IMPLEMENTED (#4669)
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

**`Result` has no `Unwrap`** — extraction is consumption, done by an owning `case move x of …`. `Option<T>` is deliberately absent: **`T?` is Bezel's option type.** None of the four combinators is `pure`, and **`UnwrapOr`'s argument is evaluated eagerly** (section 7.9). Owning instantiations carry none of the four members.

**Serialization vocabulary** (auto-imported): `ISerializer` and `IDeserializer` (the `WriteInt`/`ReadInt`-style scalar pairs plus `BeginObject`/`WriteFieldName`/`EndObject` and `BeginArray`/`EndArray` framing — stdlib §2.14 lists all of them), `ISerializable` (`Serialize(s: ISerializer)`), and `IDeserializable` — a **marker**: the compiler recognizes `static method Deserialize(d: IDeserializer): T` / `: owned T` / `: owned Shared<C>` by name. Records and enums declaring `: ISerializable, IDeserializable` get synthesized bodies; a record or choice used as a **component** must declare the conformance itself (enums need not), or the enclosing declaration is rejected; `serialized "<name>" | skip | optional` customizes; `$id`/`$ref`/`$type`/`$case` are reserved field names. A **derived class** serializes its base chain's components first: every base declaring a component must itself declare `ISerializable` or `IDeserializable` and leave both members synthesized, and for `IDeserializable` the selected constructor binds each inherited component (`Shared`/`Weak?` ones aside) by name, as it binds own ones (`class D(v: int, x: int): B(v), ISerializable, IDeserializable` capturing `x`, over a `class B(v: int)` capturing `v`; `class D(): B(999), …` is rejected). Without a `polymorphic` registry, a class's synthesized `Serialize` raises `SerializationException` for a derived instance (through a base-typed field or receiver, or inherited by a subclass that does not declare `ISerializable` itself), so each subclass you serialize declares the interfaces itself and keeps their members synthesized (hand-writing `Serialize`/`Deserialize` over a base's synthesized one is §12.2 hiding, rejected); an `abstract` or `closed` class with no registry and a synthesized `Serialize` is rejected as an owned, `T?`, `Shared<T>` or collection-element component type (a `Weak<T>` or borrowing back-edge is fine), and cannot declare `IDeserializable` without hand-writing `Deserialize`. Concrete formats (`import System.Serialization;`): `JsonSerializer`/`JsonDeserializer`/`BinarySerializer`/`BinaryDeserializer`, `JsonOptions`, and the facades `Json.ToText<T>`/`ToBytes<T>`/`Parse<T>` (value targets only)/`ParseOwned<T>` (`where T: class`) and `Binary.ToBytes<T>`/`Parse<T>`/`ParseOwned<T>` (the binary format's one knob, a trailing `ignoreUnknownFields: bool := false` — strict by default — is on both `BinaryDeserializer` constructors and on `Binary.Parse<T>`/`ParseOwned<T>`, over either byte-span family).

**Static-interface abstractions:** `ICollectionLiteralTarget<TSelf, T>` (`static FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned TSelf`) and `IOwningCollectionLiteralTarget<TSelf, T>` (`static FromCollectionLiteral(owned elements: OwningArray<T>): owned TSelf`) — the sequence-literal target pair (spec §15.4.1): a user **class** declares exactly one, at `TSelf` = itself (`class Ring<T>: ICollectionLiteralTarget<Ring<T>, T>`), and the built-ins declare it too — `Array/LinkedList/Set/Stack/Queue<T>` the borrowing one, the five `Owning*<T>` the owning one — so each lists a callable `FromCollectionLiteral`; `INumber<T>` (`static Zero()`, `static One()`, operators `+ - * /`, `<`), `ISteppable<T>` (`static Step(value: T, by: long): owned T`, `<`), `IParsable<T>` (`static Parse(s: string): owned T`, `static TryParse(s: string, out owned value: T?): bool`). Conformers: the eleven numeric primitives conform to all three; `char` conforms to `ISteppable`/`IParsable`; **`string` and `bool` conform to none**.

### 5.3 `System.Text` (auto)

`StringBuilder` (**sendable** — copy state, crosses as an owned transfer) — `constructor()`, `(capacity: int)`, `(initial: string)`; `Length: int`, `pure Capacity: int`; `Append(string|char|Object|bool|int|byte|sbyte|short|ushort|uint|long|ulong|float|double|decimal|ReadOnlyStringSpan): StringBuilder`; `AppendLine()`, `AppendLine(value: string)`; `AppendFormat(format: string, args: Array<Object>)`; `Insert(index, value: string)`, `Remove(startIndex, length)`, `Replace(oldValue, newValue: string)`, `Clear()` — all returning `StringBuilder`; `ToString(): string`.

`Encoding` (`public abstract class`) — `abstract GetBytes(s: string): owned Array<byte>`; `abstract GetString(bytes: Array<byte>)`, `GetString(bytes: ReadOnlySpan<byte>)`, `GetString(bytes: ReadOnlyArraySpan<byte>)` (a window over an array is `GetString(bytes.AsReadOnlySpan(start, length))` — there is no `(bytes, start, length)` form); `abstract GetByteCount(s: string): int`; `abstract GetBytes(s: string, destination: Span<byte>): int`; `abstract GetByteCount(s: ReadOnlyStringSpan): int`; `abstract GetBytes(s: ReadOnlyStringSpan, destination: Span<byte>): int`; `abstract GetBytes(s: ReadOnlyStringSpan): owned Array<byte>`; `abstract readonly property Name: string`; `static readonly property UTF8, UTF16, UTF32, ASCII: Encoding`. Malformed input raises `FormatException`, **never** a replacement character. UTF8 emits no BOM; UTF16/UTF32 are little-endian only.

`ReadOnlyStringSpan` — `public record (Source: string, Start: int, Length: int)`; **ordinary record, not borrow-bearing**. `property [i: int]: char { get }`; `pure IsEmpty: bool`; `pure Slice(start, length) / Slice(start) / Slice(r: Range<int>)`; `pure IndexOf(char|ReadOnlyStringSpan|string): int`; `pure Contains(…): bool`; `pure StartsWith`/`EndsWith(ReadOnlyStringSpan|string): bool`; `pure SequenceEqual(ReadOnlyStringSpan|string|ReadOnlySpan<char>|ReadOnlyArraySpan<char>): bool`; `CopyTo(destination: Span<char>)` (**not pure**); `pure ToString(): string`. `ReadOnlyAsciiByteSpan` was **retired** and does not exist.

### 5.4 `System.Collections.Generic` (auto-imported)

```bezel
public interface IReadOnlyCollection<out T>: IEnumerable<T>   pure readonly property Count: int;  end
public interface IReadOnlyList<out T>: IReadOnlyCollection<T>   readonly property [i: int]: T;  end
public interface ICollection<T>: IReadOnlyCollection<T>
  readonly property IsReadOnly: bool;
  method Add(item: T);  Clear();  Contains(item: T): bool;  Remove(item: T): bool;   // every bool Remove: true iff it removed
                                                                                   //   (the first equal element); false, unchanged, if absent
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
    // Remove/Contains/IndexOf/LastIndexOf (and `x in xs`): rejected AT THE CALL where T has no
    //   equality (a record/choice not auto-hashable, none declared); Array<R> itself is fine (§14.2)
  method Sort(comparer: (T, T) -> int);   method Reverse();                        // in place
  method Sorted(comparer: (T, T) -> int): owned Array<T>;   Reversed(): owned Array<T>;
  method ToArray(): owned Array<T>;
  method AsSpan([start[, length]] | r: Range<int>): ArraySpan<T>;
  method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlyArraySpan<T>;
  method Slice(r: Range<int>): owned Array<T>;       // `arr[r]` is the compiler-recognized form
  method Sort(); method Sorted(): owned Array<T>;    // natural order; only where T: IComparable<T>,
                                                     //   checked at the call; in a generic body, by T's own constraints (§14.2)
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
  //   (descending index / tail-to-head / pop / reverse-dequeue).
  method TryPopFirst(out owned item: T?): bool;   TryPopLast(out owned item: T?): bool;
  method Drain(): IOwningEnumerable<T>;
end
// No `OwningArray<T>` extension supplies a default-comparer `Sort`: §26.5 rule 1
// commits instance resolution by *name*, and the class declares `Sort` above, so an
// extension member of that name is never consulted. Callers pass a comparer, or
// call the parameterless `Sort()` above where `T: IComparable<T>`.
```

There is **no `TryPopAt`**, and `OwningArray<T>` declares **no `Reversed`, no `Sorted`, no `ToArray`** — an *owning* copy is inexpressible. Reorder in place with `Reverse()`/`Sort()`; move elements to a new owning collection with `Drain()`. `move arr[i] with fresh` takes an element out and stores `fresh` in its slot in one step (the bare `move arr[i]` needs a nullable slot, which `OwningArray<T>`'s is not — use `PopAt`). `Drain()` mints a fresh drain-view frame — disposing the frame does not dispose the collection, and partial iteration leaves the remainder.

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
public class Dictionary<K, V>: ICollection<DictionaryEntry<K, V>>    // K must be hashable; K? and [N]K are hashable exactly when K is (§3.6)
  constructor();  (capacity: int);  (comparer: IEqualityComparer<K>);  (capacity, comparer);   // a comparer is RETAINED as a borrow: a comparer-built collection (this and its three siblings) never crosses a domain — recorded trust, stdlib §4
  pure readonly property Count: int;
  readonly property Keys: IReadOnlyCollection<K>;   Values: IReadOnlyCollection<V>;   // live views
  property [key: K]: V;                             // get: KeyNotFoundException; set: add-or-update
  method Add(key: K, value: V);                     // ArgumentException if present
  method TryAdd(key: K, value: V): bool;            // false, no mutation, if present
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
  method TryAdd(key: K, owned value: V): bool;      // false on duplicate — value still DISPOSED on that path
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

**Buffers and spans.** `OwningBuffer<T>` is the storage primitive and the one owning-family type that admits **value**-typed `T`; it does not admit runtime-managed reference types (`string`, function types) or move-only value types. It is **sendable** at a sendable class `T` and at a sendable value `T` (`OwningBuffer<double>`); a `DangerousPointer()` the origin domain keeps after moving the buffer is a §32.10 obligation.

```bezel
public class OwningBuffer<T>                    // class T: conforms to IEnumerable<T?>; value T: to nothing
  constructor(length: int);                     // class T: all slots null;  value T: all slots default(T) — T must be defaultable, read per instantiation in a generic body
  constructor(length: int, init: (int) -> owned T);      // value T: init is (int) -> T
  pure readonly property Length: int;           // immutable after construction
  property [i: int]: T? { get;  set(owned); }   // class T: set disposes previous, `move buf[i]` ADMITTED
  property [i: int]: T  { get;  set; }          // value T: `move buf[i]` NOT admitted
  method DangerousPointer(): Ptr<T>;            // value T only; NOT YET IMPLEMENTED (#1717)
  method AsSpan([start[, length]] | r: Range<int>): Span<T>;
  method AsReadOnlySpan([start[, length]] | r: Range<int>): ReadOnlySpan<T>;
end
public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  property [i: int]: T? { get; set(owned); }    // value T: [i: int]: T { get; set; }
  property IsEmpty: bool;   method Slice(start, length) / Slice(start) / Slice(r: Range<int>): Span<T>;
  method IndexOf(value): int;  Contains(value): bool;  AsReadOnly(); Reverse();
  method SequenceEqual(other: Span<T> | ReadOnlySpan<T> | ReadOnlyArraySpan<T>): bool;   // a writable argument passes .AsReadOnly()
  method Clear();                               // class T only: nulls every slot, disposing previous
  method MoveFrom(source: Span<T>);  MoveTo(destination: Span<T>);          // class T only
  method Fill(value: T);  CopyFrom(source: ReadOnlySpan<T>);  CopyTo(destination: Span<T>);  // value T only
end
public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)
  // read surface + Slice x3;  SequenceEqual(other: ReadOnlySpan<T> | ReadOnlyArraySpan<T>);  value T only: CopyTo(destination: Span<T>).  No ToArray()
end
public record ArraySpan<T>(Source: Array<T>, Start: int, Length: int)
  property [i: int]: T { get; set; }            // the setter stores a BORROW; `move` NOT admitted
  property IsEmpty: bool;  Slice x3;  AsReadOnly(): ReadOnlyArraySpan<T>;  IndexOf; Contains;
  method SequenceEqual(other: ArraySpan<T> | ReadOnlyArraySpan<T> | ReadOnlySpan<T>): bool;
  Fill(value: T);  CopyFrom(source: ReadOnlyArraySpan<T>);  CopyTo(destination: ArraySpan<T>);  Reverse();
end
public record ReadOnlyArraySpan<T>(Source: Array<T>, Start: int, Length: int)
  // read surface + SequenceEqual(other: ReadOnlyArraySpan<T> | ReadOnlySpan<T>) + CopyTo(destination: ArraySpan<T>).  No ToArray() (neither span family has one)
end
// There is NO common span interface: a member over either heap backing declares two overloads, (ReadOnlySpan<T>) and (ReadOnlyArraySpan<T>).
```

`Span<T>`/`ReadOnlySpan<T>` are **borrow-bearing records** bounded by the buffer's lifetime; never an `async` method's parameter or result. **There is no implicit `Span<T>` → `ReadOnlySpan<T>` conversion** — call `.AsReadOnly()`. Spans do **not** implement `IEnumerable<T>`; `for` over a span is a direct indexed loop. `Span<T>` and `ArraySpan<T>` are **not interchangeable**. The `AsSpan`/`AsReadOnlySpan` factories and `Slice(r: Range<int>)` validate eagerly (`IndexOutOfRangeException`); direct constructors and `Slice(start[, length])` do not. **There is no writable-span interface.**

**Byte accessors** (the read tier is declared on `ReadOnlySpan<byte>` and, member for member, on `ReadOnlyArraySpan<byte>` — a writable byte span reads through `.AsReadOnly()`; the write tier is declared on `Span<byte>` and `ArraySpan<byte>`): `Read{Int16,UInt16,Int32,UInt32,Int64,UInt64,Float,Double}{LittleEndian,BigEndian}()` and the matching `TryRead…(out value: T): bool`; on the writable spans additionally `Write…{LittleEndian,BigEndian}(value: T)` and `TryWrite…(value: T): bool`. **Endianness is in the name — there is no platform-default form, no enum parameter, and no offset parameter; slice first.** A short span raises `IndexOutOfRangeException`; a false `TryWrite*` leaves the span unmodified.

**Bit tier** (**not yet implemented**, #3283): four borrow-bearing records — `BitReader(Source: ReadOnlySpan<byte>, BitStart: int, BitLength: int)`, `BitWriter(Source: Span<byte>, …)` and their `ArrayBitReader`/`ArrayBitWriter` mirrors over the array spans — reached by `pure AsBitReader()` on the read-only spans and `AsBitReader()` (**not pure**) / `pure AsBitWriter()` on the writable ones. Each carries `pure IsEmpty`, `pure IsByteAligned`, `pure Slice` ×3, `pure AlignToByte()`, `AlignedTail()`, plus `ReadBitsBigEndianMsbFirst(count: int): ulong`, `ReadBitsLittleEndianLsbFirst(count: int): ulong`, `ReadBitMsbFirst()`/`ReadBitLsbFirst(): bool` and their `TryRead…(…, out value): bool` forms — the writer swapping reads for `WriteBits…(value: ulong, count: int)`/`WriteBit…(value: bool)`/`TryWrite…` and adding `AsReadOnly()`. Only **two** of the four bit-order × byte-order combinations exist; every multi-bit read returns `ulong` and every multi-bit write takes one (**no signed form**); a reader does not advance itself — slice to advance.

```bezel
public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>> where K: IComparable<K>
  constructor();   pure readonly property Count: int;
  property [key: K]: V;                          // get: KeyNotFoundException;  set: insert or replace
  method Add(key: K, value: V);                  // ArgumentException if present
  method TryAdd(key: K, value: V): bool;         // false, no mutation, if present
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
public class Counter<K>: IEnumerable<DictionaryEntry<K, int>> where K: hashable   // NOT YET IMPLEMENTED (#3848)
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
public class OwningSlotMap<T>: IEnumerable<T>, IOwningEnumerable<T> where T: class   // the handle-and-table type; NOT YET IMPLEMENTED (#4604, #4777)
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

The sorted collections take **no comparer parameter** — ordering is `K`/`T`'s own `IComparable` (the hash family's `IEqualityComparer` ctors are a recorded different case — rationale §17.14). `SortedDictionary` has `Add`/`TryAdd` but no `Keys`/`Values` (recorded: iterate entries). **There is no `OwningSortedDictionary`/`OwningSortedSet`, no `FrozenDictionary`/`FrozenSet`.** `Counter<K>` is insertion-ordered and a key decremented to zero is not removed.

### 5.5 `System.Collections.Linq` (auto-imported)

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
// The six hashable-element operators are a SECOND extension block. §26.6 merges
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
  //   maxCount is a construction-time CEILING only: ArgumentException when
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
// AsyncEventSource<T>: no T that holds borrows (a span; a borrow-bearing record, inline array or choice; CancellationToken
//   excepted) — the raise frame holds the payload until settlement; raise a CLASS payload or borrow-free data instead.
public class TaskCompletionSource<T>                   // NON-class T; class T is a compile error; no T that holds borrows
  constructor();   readonly property Task: Task<T>;    // the property is a BORROW of the source's storage
  method SetResult(result: T);   SetException(owned exception: Exception);   SetCancelled();
  method TrySetResult(result: T): bool;  TrySetException(owned ...): bool;  TrySetCancelled(): bool;
end                     // + OwningTaskCompletionSource<T> where T: class (SetResult(owned result: T),
                        //   Task: OwningTask<T>) and the void-completion TaskCompletionSource
public class Task
  static method FromResult<T>(value: T): Task<T>;                        // NON-class T; no T that holds borrows
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
public class TaskScope: IAsyncDisposable                 // `await using` ONLY, owned by that binding alone; not sendable
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
public class Locked<T> where T: class                  // T must additionally be a SENDABLE class; Locked/RwLocked NOT YET IMPLEMENTED (#1721)
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
  Spawn<A>(entry: (A) -> (), arg: A): owned Task                where A: sendable, notnull;   // A: value, string or Type; a class A takes the owned row
  Spawn<R>(entry: () -> owned R): owned OwningTask<R>           where R: sendable, class;
  Spawn<R>(entry: () -> R): owned Task<R>                       where R: sendable, notnull;   // R likewise
end                                   // the entry MUST be capture-free; EXACTLY ONE argument
public static class Parallel          // callbacks are SPAWN POSITIONS (capture-free); NOT YET IMPLEMENTED (#3557)
  public static readonly property DefaultDegree: int;    // = Environment.ProcessorCount; degree := 0 means it
  Map<A, R>(items: Array<A>, body: (A) -> R, degree: int := 0): owned Task<Array<R>> where A: sendable, notnull where R: sendable, notnull;
  Reduce<A, R>(items: Array<A>, seed: R, selector: (A) -> R, combiner: (R, R) -> R,
               degree: int := 0): owned Task<R>                                      where A: sendable, notnull where R: sendable, notnull;
  ForEach<A>(items: Array<A>, body: (A) -> (), degree: int := 0): owned Task          where A: sendable, notnull;   // value/string elements; class elements take the owned rows
  Map<A, R>(owned items: OwningArray<A>, body: (owned A) -> owned R,
            degree := 0): owned OwningTask<OwningArray<R>>                            where A: sendable, class where R: sendable, class;
  ForEach<A>(owned items: OwningArray<A>, body: (owned A) -> (), degree := 0): owned Task where A: sendable, class;
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

`Stopwatch` (**sendable** — copy state; a moved running stopwatch keeps measuring) — `constructor()`, `static method StartNew(): owned Stopwatch`; `IsRunning: bool`, `Elapsed: TimeSpan`, `ElapsedMilliseconds: long`; `Start()`, `Stop()`, `Reset()` — **no `Restart`**: write `sw.Reset(); sw.Start();`. `Debug` — `Assert(condition: bool, message: string := "")` (an empty message reports as no message), `WriteLine(message: string)`. **`Debug.Assert` terminates the process; it never throws**, and calls may be elided in release builds.

### 5.9 Primitives

The integer types (`Int32`, `Int64`, `Byte`, `SByte`, `Int16`, `UInt16`, `UInt32`, `UInt64`) carry: `static const MinValue/MaxValue`; `static pure Parse(s: string)`, `static TryParse(s: string, out result: T): bool`, `static pure ParseOrNull(s: string): T?`, `static pure Parse(s: string, radix: int)` (radix ∈ {2,8,10,16}), `static TryParse(s: string, radix: int, out result: T): bool`, `static pure Parse(s: ReadOnlyStringSpan)`, `static TryParse(s: ReadOnlyStringSpan, out result: T): bool`; `pure ToString()`, `pure ToString(format: string)`, `pure ToString(radix: int)`, `pure CompareTo(other)`. **`ParseOrNull` has no radix or span duals.** No whitespace tolerance anywhere. **Radix digits** are the first *r* of `0`–`9` then `A`–`Z`; `Parse(s, radix)` accepts **either case** (`int.Parse("ff", 16)` = `int.Parse("FF", 16)` = `255`), and `ToString(radix)` emits **lower case**, sign-and-magnitude — so `T.Parse(n.ToString(r), r) = n`; the no-format `ToString()` is `ToString(10)`'s text. Input grammar: optional `+`/`-`, then digits; no `0x`, no separators, no whitespace (`FormatException`), out of range `OverflowException`; a radix outside `{2,8,10,16}` is `ArgumentException` from both `Parse` and `ToString`.

`Double`/`Float`: `MinValue MaxValue Epsilon PositiveInfinity NegativeInfinity NaN`; `static pure IsNaN/IsInfinity/IsPositiveInfinity/IsNegativeInfinity/IsFinite`; `Parse`/`TryParse` (string and `ReadOnlyStringSpan`) and `ParseOrNull` (string); `Parse` reads an optional `+`/`-`, digits, an optional `.` with digits on both sides and an optional `e`/`E` exponent, and nothing else — no whitespace, separators, hex, or bare `".5"`/`"5."` (`FormatException`, which precedes the range fault); it rounds to nearest-even like a literal and raises `OverflowException` exactly where the same digits as a literal of that type are out of range (`double.Parse("1e999")`, `float.Parse("3.5e38")`; `TryParse` → `false`, `ParseOrNull` → `null`), never yielding `±Infinity` for a finite numeral, while underflow is in range (`"1e-400"` → `0`); the five `Is*` report the operand's IEEE-754 class exactly (`IsFinite` = neither NaN nor infinity; subnormals and both zeros are finite); `static pure IeeeEquals(a,b)`, `static pure BitwiseEquals(a,b)`; `pure ToString([format])` — a non-finite value renders `NaN`/`Infinity`/`-Infinity` under every format (no digits, no `%`), and `Parse` reads back exactly those three, case-sensitive (`"nan"`, `"inf"`, `"+Infinity"` → `FormatException`) — `pure CompareTo`. `Epsilon` is the smallest positive subnormal, not machine epsilon.
`Decimal`: `MinValue` = `-79228162514264337593543950335`, `MaxValue` = `79228162514264337593543950335`, `Zero One`; the no-format `ToString()` (and `G`) is the exact shortest form — no exponent, no grouping, **no trailing zeros** (`1.50m` renders `1.5`, `10m` renders `10`), scale not being part of the value; `Parse`/`TryParse`/`ParseOrNull` (`Parse` reads an optional `+`/`-`, digits, and an optional `.` with digits on both sides, and nothing else — no exponent, whitespace, separators, or bare `".5"`/`"5."`: `FormatException`); `pure ToString([format])`, `pure CompareTo`, `pure Round(digits: int)` (banker's, 0..28).
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
  pure Equals(other: string): bool;   pure Equals(other: string?, ignoreCase: bool): bool;   // Equals(string) is the Object slot (§8.3): a `string?` or `Object?` argument reaches it, null and non-strings unequal
  pure override GetHashCode(): int;   pure override ToString(): string;
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

The carriers are `IntPtr`/`UIntPtr` (records: `static readonly property Zero`, `Equals`, `GetHashCode`, `=`, `<>`; opaque, never dereferenced), `CString` (`static From(s: string): owned CString`, `static FromBytes(bytes: Array<byte>): owned CString`, `Length`, `Pointer: IntPtr`, `ToBytes(): owned Array<byte>`, `destructor`), `WString` (`static From`, `Length`, `Pointer`, `destructor`), `RawBytes` (`static From(buffer: Array<byte>)`, `static FromForeign(p: IntPtr, length: int, freeOnDispose: bool := false)`, `Length`, `Pointer`, `CopyTo(destination: Array<byte>, destOffset: int)`, `destructor`) — none has a public constructor — plus `System.Interop`'s `Ptr<T>` (`LoadAt`/`StoreAt`/`CopyFromArray`/`FromIntPtr` not yet implemented, #5580; `Load`/`Store` do not yet reach memory, #1765) and `Interop` (`LastErrno()`, `CodeAddress<F>(callback)`; not yet implemented, #5581). For the `extern` declaration syntax and the pointee gating, read spec §32 and stdlib §10.

### 5.11 What the standard library does **not** have

Every absence below was verified by a full-document search. Reaching for one of these is the failure this document exists to prevent.

**I/O.** `MemoryStream` (→ `OwningBuffer<byte>` + spans; for serializers, `JsonSerializer.ToBytes()`); `File.Open`/`OpenRead`/`OpenWrite`/`OpenText`/`AppendAllText`/`AppendAllLines` (→ `new FileStream(path, mode, access := FileAccess.ReadWrite)`, `FileMode.Append`); `File.ReadAllBytesAsync`/`ReadAllLinesAsync`/`WriteAllBytesAsync` (the async `File` surface is **text-only** → `FileStream.ReadAllAsync`/`ReadAsync`/`WriteAsync`); `StreamReader`/`StreamWriter`/`BufferedStream`; async `TextReader`/`TextWriter`; async **span** overloads on `Stream`; `Directory.Move`/`EnumerateFiles`/globbing; `Path.ChangeExtension`/`HasExtension`/`GetTempPath`/`DirectorySeparatorChar`; `Console.Read()`.

**Query operators.** `ToList`, `ToLookup`, `Join`, `GroupJoin`, `Cast<T>`, `AsEnumerable`, `DefaultIfEmpty`, `Prepend`, `Append`, `Chunk`, `DistinctBy`, `UnionBy`/`IntersectBy`/`ExceptBy`, `MinBy`, `MaxBy`, `Order()`/`OrderDescending()`, `TakeLast`, `SkipLast`, `LongCount`, `Enumerable.Empty`/`Range`/`Repeat` (**there is no `Enumerable` class at all**), `SequenceEqual` over `IEnumerable<T>` (it exists only on the span types), `SelectMany` with a result selector, `Zip` without a combiner, `Where`/`Select` with an index, and an instance `ForEach(action)` on any sequence (`Array<T>` and `string` included — write the `for` loop). Replacements: `ToList` → `ToArray()`; `MinBy(f)` → `OrderBy(f).First()`; `Enumerable.Range` → the language's `1..n`, which is `IEnumerable<T>` (`(1..100).Sum()` works); `Append`/`Prepend` → `Concat` with a one-element `Array<T>`.

**Collections.** `List<T>` (reserved name), `HashSet<T>` (→ `Set<T>`), `SortedList`, `PriorityQueue`, `Deque`, `ConcurrentDictionary`/`ConcurrentQueue`/`ConcurrentBag` (→ `Locked<T>` over a sendable collection, or `Channel<T>`), `ImmutableArray`/`ImmutableList`/`ImmutableDictionary` (→ `FrozenArray<T>`), `SlotMap`/`Arena`/`GenerationalArena`/`Pool` as plain names and any borrowing slot map (→ `OwningSlotMap<T>` with `SlotHandle` keys — the only form; a handle from another map faults), `ReadOnlyDictionary`, `BitArray`, `Lookup`, `KeyValuePair` (→ `DictionaryEntry<K,V>`), `Tuple`/`ValueTuple` (**Bezel has no tuples** — use a record), `Nullable<T>` (→ `T?`), `IComparer<T>` (only `IEqualityComparer<T>` exists), `StringComparison`/`StringComparer`. Absent *members* on types that do exist: `Array<T>` has no `RemoveAll`, `FindIndex`, `FindAll`, `Find`, `Exists`, `BinarySearch`, `CopyTo`, `TrueForAll`, `GetRange`, `InsertRange`, `RemoveRange`, `ForEach`; `Dictionary` has no `GetValueOrDefault` (→ `GetOrNull`) — it *does* have `TryAdd`; `Math` has no transcendental block (`Sin`/`Cos`/`Tan`/`Asin`/`Acos`/`Atan`/`Atan2`/`Sinh`/`Cosh`/`Tanh`/`Exp`/`Log`/`Log10`/`Log2`/`E` removed; `Sqrt`/`Pow`/rounding stay) and no `Hypot`/`Cbrt`/`PI` (never documented); `IEnumerable<T>` has no `OfType<U>`; `Console` has no `Clear`; `OwningArray<T>` declares no `ToArray`/`Sorted`/`Reversed`; `OwningLinkedList<T>` has no source constructor; `Stack`/`Queue` have no `Add`/`Remove`/`IsReadOnly`; `Counter<K>` has no `ContainsKey` (→ `Contains(key)`, `key in tally`); `IReadOnlySpan<T>` does not exist (→ two overloads, `(ReadOnlySpan<T>)` and `(ReadOnlyArraySpan<T>)`; a writable span passes `.AsReadOnly()`); no `Swap(i, j)` on any owning collection or inline array (two non-nullable owned slots exchange through a third value — `var t := move xs[i] with (move xs[j] with placeholder); xs[j] := t;` — or by `PopAt`/`Insert`; two nullable slots need none: `var t := move a[i] with (move a[j]); a[j] := t;`).

**Text.** `string.Format`/`String.Format` (**use interpolation**), `String.Empty` (use `""`), `string.Concat`, `Split` with a remove-empty option, `Regex`, `ToTitleCase`, `Normalize`, the `StringBuilder.Length` setter, the `C` format specifier, any culture-aware formatting, `string.IsNullOrEmpty` (→ `s = null or s.IsEmpty`), the static `string.Compare(a, b[, ignoreCase])` (→ `a.CompareTo(b)`, `a.CompareTo(b, ignoreCase := true)`).

**Concurrency.** `Thread`, `ThreadPool`, `Monitor`, a `lock` statement (→ `Mutex`/`Locked<T>`), `Interlocked` (→ `Atomic<T>`), `ManualResetEvent`/`AutoResetEvent`/`CountdownEvent`/`Barrier`, `Task.Run` (→ `Domain.Spawn`), `Task.CompletedTask` (→ `Task.FromResult`), `Task.Yield`, `ConfigureAwait`, `Task.Wait`/`.Result` inside async code, `AggregateException` (first fault wins; **no aggregation**), `Parallel.For`.

**Everything else.** `Enum.Parse`/`Enum.TryParse`/`Enum.GetName`/`Enum.GetNames`/`Enum.GetValues`/`Enum.IsDefined` (**there is no `Enum` class** — the members are per-enum statics, `E` being the enum's own name: `E.FromNameOrNull(s)` (declared identifier, exact) and `E.NameOf(v)`, `E.FromUnderlyingOrNull(u)`; the lists are `typeof(E).EnumNames`/`EnumValues`, and `IsDefined` is `E.FromUnderlyingOrNull(u) <> null`), `Object.MemberwiseClone` and any clone facility, `ICloneable`, member reflection (**declined, not deferred**), attributes, `Activator`, `Lazy<T>`, `WeakReference` (→ `Weak<T>`), `Option<T>` (→ `T?`), `Result.Unwrap`/`UnwrapOrElse`, `Math.Round(decimal)` (→ `Decimal.Round`), `Math.Cbrt`/`Hypot`/`BigMul`, `Random.Shared`, cryptographic randomness, a `Guid` constructor, `Environment.StackTrace`/`CurrentDirectory`/`TickCount`, `Convert` from any non-`string` source, `Encoding.GetEncoding(name)`/`Encoding.Default`, `Encoding.GetString(bytes, start, length)` (→ `GetString(bytes.AsReadOnlySpan(start, length))`), `Stopwatch.Restart()` (→ `Reset(); Start();`), `Type.Implements(iface)` (→ `iface.IsAssignableFrom(t)`), `Math.Sign(c: char)` (`Sign<T>` needs `INumber<T>` — `Math.Sign(int(c))`), `OutOfMemoryException`, `InnerException` on user exception types, multidimensional arrays, `defer`/`errdefer` (→ `ScopeGuard`).

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

**A choice with an owned payload, matched by consuming.** A `case` over a *named* binding only peeks; consuming one is `case move outcome of …`, and the positional destructure transfers its owned payloads (a named slot never transfers). In an owning match a whole-value arm `var y => …` passes the value on only if the arm consumes `y` (`var z := y;`); otherwise the construct disposes it.

```bezel
public choice PollOutcome
  Ready(owned conn: Connection, attempts: int),
  Empty;
end PollOutcome;
```

```bezel
case move outcome of
  Ready(var c, var n) => do                      // positional: c is owned, n a copy
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

A `Task`-family call mints an owned frame. `var text := await File.ReadAllTextAsync(p);` simply yields the `string` — no ownership crosses a value or `string` result. Over a `Task<C>` at a **class** `C`, `var c := await LoadAsync(p);` is the **scoped await-bind**: a fresh `var`/`const` whose initializer is exactly an await of an unbound producer pins the task to a hidden slot for the enclosing block, and `c` borrows its result there (§30.5).

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

**Tallying and querying.** `using var tally := new Counter<string>();` then `tally.Increment(w.ToLower());` beats a dictionary dance (`Counter<K>` is **not yet implemented**, #3848 — until it is, `var tally := new Dictionary<string, int>();` and, per word, `var k := w.ToLower(); tally[k] := (tally.GetOrNull(k) ?? 0) + 1;`). A bound lazy query is affine — enumerate it once, or `ToArray()` it to read the results repeatedly: `var top := tally.OrderByDescending(lambda(e) => e.Value).Take(10).ToArray();` yields an `owned Array<DictionaryEntry<string, int>>` you may re-read.

**Strings.** `var sb := new StringBuilder();` (trivial-dispose: plain `var`, no warning), `sb.Append(w).Append(' ');`, `sb.ToString().Trim()`. Casts are call syntax — `double(hits)` — and interpolation carries alignment then format: `$"|{line,-20}| {pct,6:F1}%"`.

**Declaring, throwing and catching an exception type.** Derive from `Exception` (or any built-in exception type) and chain the message to the base; a caught exception is a **borrow** valid inside its arm only, so copy out what you keep.

```bezel
public class ConfigError(message: string): Exception(message)
end ConfigError;

public static class Settings
  public static method Port(text: string): int
    var port := int.ParseOrNull(text);
    if port = null then
      throw new ConfigError($"not a port: {text}");
    end;
    return port;                                 // narrowed by the early exit
  end Port;

  public static method PortOr(text: string, fallback: int): int
    var result := fallback;
    try
      result := Port(text);
    catch
      var ex: ConfigError => Console.Error.WriteLine(ex.Message)   // `ex` lives for this arm only
    end try;
    return result;
  end PortOr;
end Settings;
```

To wrap a lower-level fault, declare the class without a primary constructor and give it `public constructor(message: string, cause: Exception) base(message, cause); end constructor;` — beside a primary constructor, every other constructor must chain `this(…)` (§11.1). The cause is snapshotted into `CauseChain`, never retained (§31.4); there is no user `InnerException`.

**A class hierarchy.** Members are non-virtual unless marked; an override restates the signature exactly and says `override` (§12.1, §12.5).

```bezel
public abstract class Shape(name: string)
  public readonly property Name: string := name;
  public abstract method Area(): double;               // body-less; implicitly virtual
  public virtual method Describe(): string => $"{Name}: {Area():F2}";
end Shape;

public sealed class Circle(radius: double): Shape("circle")
  private _r: double := radius;
  public override method Area(): double => Math.Pi * _r * _r;
end Circle;

public sealed class Square(side: double): Shape("square")
  private _s: double := side;
  public override method Area(): double => _s * _s;
  public override method Describe(): string => $"{base.Describe()} (side {_s})";
end Square;
```

`new Shape("x")` is rejected (abstract). Hold mixed instances in an owning collection of the base type — `using var shapes := new OwningArray<Shape>(); shapes.Add(new Circle(1.0));` — and `for var s in shapes do Console.WriteLine(s.Describe()); end;` dispatches per element.

**An iterator method.** `yield return` or `yield break` makes the method an iterator; its unmarked iterable return is already the caller-owned frame's announcement, so `owned` need not be written (§29.3, §28.3). Exit early with `yield break;`, never `return;` (§29.5).

```bezel
public static class Seq
  public static method Evens(limit: int): IEnumerable<int>
    for var i in 0..limit step 2 do
      yield return i;
    end;
  end Evens;
end Seq;
```

`for var n in Seq.Evens(10) do … end;` and `var total := Seq.Evens(10).Sum();` each mint and consume a fresh frame. At a class element type, a value the iterator owns and yields is a borrow of the enumerator's slot, invalidated by the next advance (section 4.5).

---

## 7. Looks right, is rejected

Each entry is **wrong**, then the rule, then **right**.

### 7.1 Syntax reflexes

```bezel
foreach var t in transactions do Process(t); end foreach;   // WRONG: no such keyword
var first := rows?[0];                                      // WRONG: there is no `?[` suffix
var xs: Array := [10, 20, 30];                              // WRONG: `Array` is not a type
Registry.n := Registry.n + 1;                               // WRONG inside `class Registry<T>` too: a bare generic name has arity 0 everywhere (§14.1) — `n := n + 1;` or `Registry<T>.n`
public method Pick<T>(x: T): T => x;                        // WRONG inside `class Box<T>`: a method's type parameter may not reuse an enclosing one's name (§14.1) — `Pick<U>(x: U): U`; an override may rename its base's (§11.2, §12.1)
public class Item end Item;                                 // WRONG inside `class Outer<Item>`: a nested type may not share the type's own type parameter's name (§14.1) — rename one; a MEMBER may (`public property Item: Item` names the parameter at its type position, the property at a value read)
var ys := [1, 2, 3];                                        // WRONG: no target type for the literal
var s := s!;                                                // WRONG wherever a local or parameter `s` is in scope: same block, body top beside a parameter, or any enclosing block, loop or arm body (§21.1) — `var t := s!;`
for var x in xs do Use(xs.Where(lambda(x) => x > 0)); end;   // WRONG: a lambda parameter, loop variable or arm binding may not reuse an in-scope local's or parameter's name either (§21.1) — `lambda(y) => y > 0`; sibling loops and arms may reuse names
set => do var value := 0; _x := value; end;                 // WRONG in a setter: its implicit `value` is a parameter, so no local in its body reuses the name (§21.1) — rename the local
var sd: SortedDictionary<string, int> := ["a": 1];          // WRONG: the dictionary literal targets only Dictionary/OwningDictionary (§15.4)
var max := a > b ? a : b;                                   // WRONG: no `?:` operator
x := y++;   ++c;   if count++ > 3 then ... end;               // WRONG: `++` is a postfix STATEMENT
var g := (when s >= 90 => "A", _ => "F");                    // WRONG: no `when` construct — `(if s >= 90 then "A" else "F")`
when c1 => A(), _ => B() end when;                            // WRONG: same — `if c1 then A(); else B(); end;`
public static method operator ++(c: Counter): Counter => …;   // WRONG: `++`/`--` are not overloadable — declare `operator +`, write `c := c + 1;`
if count then ... end;                                        // WRONG: no truthiness
var (a, b) := point;                                        // WRONG: destructuring is pattern-position only
case o of Nope(var v) => A(), _ => B() end;                 // WRONG: a pattern head that names no type, case or constant (another module's alias included) is a compile error, never a silent non-match (§19.3)
public method Log(params items: Array<string>);             // WRONG: no `params`
var c := new Config() with(Timeout := 30);                  // WRONG: `with` needs a RECORD source
var caller: (a: int, b: int) -> int := Sub;  caller(b := 2, a := 100);   // WRONG: a call through a FUNCTION VALUE admits no named argument (§27.1)
var u := uint(-1);   var b2 := byte(300);   var c2 := char(0xD800);   // WRONG: a CONSTANT operand the conversion would reject is a COMPILE error (§5.2)
var a3: [3]int := [1, 2, 3];   var v3 := a3[5];   a3[3] := 9;          // WRONG: a CONSTANT index outside the inline array's `0..<N` is a COMPILE error, read or write (§15.1.2) — the extent is in the type
var q0 := x / 0;   var q1 := x mod Z;   x /= 0;                        // WRONG for any integer or `decimal` `x` and a constant `Z := 0`: a constant zero divisor is a COMPILE error at the operator (§6.1, §6.4, §6.6)
for var i in 10.. do ... end;                                          // WRONG: an open-ended range LITERAL as a `for` source is a COMPILE error (§16.5)
var r := -1..3000000000U;                                     // WRONG: a range literal's endpoints need one common type, as `-1 + 3000000000U` does (§16.1, §6.2) — `-1L..3000000000L`; a constant that fits the other endpoint's type adopts it (`-5..10U` is a `Range<int>`), and against an operand (`x in -1..3000000000U`, `x: long`) the literal is read at `x`'s type
if u in -5..10 then ... end;   case u of -5..10 => ... end;   // WRONG when `u` is `uint`/`ulong`: `-5` is no `uint` value and `uint` does not convert to `int` (§17.5, §19.3) — write `0..10`; over `byte`/`ushort` the `in` is a `Range<int>` test, the PATTERN still an error
case b of 300 => ... end;      case b of > 300 => ... end;    // WRONG over a `byte`: a pattern constant converts to the scrutinee's type (§19.3) — an arm that could never match is an error, not a dead arm; the EXPRESSION `b in 300..400` is admitted and false
var d := ch - '0';   var nx := char(ch + 1);   ch *= 2;             // WRONG: `char` is not arithmetic (§3.5) — `int(ch) - int('0')`, `char(int(ch) + 1)`; only `ch += n;`, `ch -= n;`, `ch++;`, `ch--;` advance a code point (§6.6)
case ch of 65 => ... end;   case ch of >= 65 => ... end;   case ch of 65..90 => ... end;   // WRONG over a `char` `ch`: a pattern constant is read at the scrutinee's type and no integer converts to `char` (§19.3, §5.1) — `'A'`, `>= 'A'`, `'A'..'Z'`, or `case int(ch) of …`; the EXPRESSION `ch = 65` is admitted (§3.5)
var c3 := char(65.5);                                         // WRONG: no float/double/decimal → char — char(int(x)) (§5.2)
if p is IShape then ... end;                                  // WRONG over a value-typed `p` (primitive, record, enum, choice, inline array, `()`): a class, interface, `string` or `Object` target is rejected (§12.7)
if i is long then ... end;                                    // WRONG over an `int` `i`: a different value type is rejected too — `is` tests the type, not whether the value fits
```

> §21.4/A.6: the loop is `for … in … do … end;`. A.7's `PrimarySuffix` admits `.`, `?.`, `[…]`, `(…)`, `?(…)`, `!`, `with(…)` — and no `?[`. §14.5: class/record/interface type arguments are never inferred; §15.4: a collection literal is well-typed only where a target type is supplied. §20: the conditional is a parenthesized atom. §17.6, §17.2: `++`/`--` and assignments are statements. §4.1: conditions are `bool`. §19.3, §27.4: destructuring lives in patterns and lambda parameters. §22.14: pass an `Array<T>` and write the literal at the call site. §9.5: `with` on a class-typed source is the retired object-initializer reading. §15.1.2 / §6.1 / §16.5: a written operand that fixes the fault — a constant index past an inline array's extent, a constant zero right operand of `/` or `mod` (integer or `decimal`, a non-constant dividend changing nothing), an open-ended range literal as a `for` source — is a compile error at the site; a computed index, a non-constant divisor and a range reached through a variable or a call keep their run-time faults. §5.2/§5.4: an explicit conversion whose operand is a **compile-time constant** the conversion would reject — `uint(-1)`, `byte(300)`, `char(0xD800)`, `int(1e30)`, an enum cast matching no declared constant — is a compile error **at the conversion site**, never a deferred `OverflowException`; the runtime fault is for **non-constant** operands, and unreachable code is not exempt. A saturating conversion is unaffected: `float(1e300)` is `Infinity`, constant operand or not.

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
var v3 := a3[k];   var q0 := x / d;   for var i in 10..20 do ... end;   // a computed index and a non-constant divisor are checked at run time; a closed range iterates
var c := new Config(timeout := 30, retries := 3);             // named constructor arguments
var caller: (a: int, b: int) -> int := Sub;  caller(100, 2);              // a function type's names are documentation only; the call is positional
if (var o: Object := p; o is IShape) then ... end;            // box first, then test the box — unless `p` holds borrows (a span, a record with a class-typed field): it reaches no `Object` local (§9.7), and its static type already answers the test
```

Also absent, with the landed spelling beside it: `goto`/`break 2`/labels on non-loop statements → labeled loops (`scan: for … break scan;`); `let`/`val`/`readonly var` → `const x := e;`; `defer`/`errdefer` → `using var`, `finally`, `ScopeGuard`; `do…while`/`repeat…until` → `while` with a leading flag; local functions → a lambda bound to a local; attributes `[Obsolete]` → the closed clause vocabulary (`serialized`, `equality skip`, `when true`); `#if`/`#region` → runtime branching; `import M.{X}`/`import M.*`/`public import` → plain `import M;` (+ `as`); tuples, anonymous types, comprehensions, a value-form `(try E catch …)`, block expressions → a `record` (possibly method-local), the lazy operators, the `try` statement, and the binding atom `(var x := e; body)`; `x not in c` → `not (x in c)`; `not is` → `is not`; `default(SomeChoice)` → construct a case, or use `C?`; implicit/explicit conversion **operators** → named `To…`/`From…` methods; `operator ==` on a class → `=` always calls `Equals` (records, enums, and choices may overload); member hiding, `protected internal`, `default interface methods`, `ref` returns, class deconstruction — all rejected.

```bezel
public class Box<T> where T: int end Box;          // ERROR: a type bound names a class, an interface or a type parameter (§14.2) — `int` would admit one type: drop the parameter, write `int`
public class Keyed<T> where T: string end Keyed;   // ERROR: `string` is not a class; the same for a record, an enum, a choice, a function type, an inline array or a `T?`
public class Keyed<T> where T: IComparable<T> end Keyed;   // fine: an interface bound; `Array<Point>` is a class bound admitting that class alone
extension Bag<T>
  public method F<U>(x: U): int where T: IFoo => 1;   // ERROR: a member's `where` constrains the member's own parameters only; the block's `T` is the enclosing declaration's (§26.4) — a second block, `extension Bag<T> where T: IFoo`
  public method G<U>(x: U): int where U: T => 1;      // fine: the block parameter as the BOUND
end;
var v: IEnumerable<string> := nullableStrings;    // ERROR over an `Array<string?>`: a variant conversion relates arguments by a reference conversion or `T` to `T?`, never `T?` to `T` (§12.9)
var w: IEnumerable<string?> := strings;           // fine over an `Array<string>`
var g: () -> string? := f;                        // fine over `f: () -> string`: a return slot takes the nullable view (§27.1.1)
var k: () -> string := g;                         // ERROR: never the reverse — a call through `k` would hand a `null` to a `string` position
var p: (string) -> () := h;                       // fine over `h: (string?) -> ()`: a parameter slot runs the other way — `(string) -> ()` to `(string?) -> ()` is the ERROR
var n: () -> int? := m;                           // ERROR over `m: () -> int`: a value-typed slot is invariant — `int` to `int?` wraps (§27.1.1)
var r: (ref Dog) -> () := Util.Touch;             // ERROR over `Touch(ref a: Animal)`: formation bridges a conversion at a by-value slot only; a `ref`/`out` slot's type is the callee's exactly (§27.6)
```

> §14.2: Appendix A.4's `Constraint ::= … | Type` parses any type as a bound, and the language gives three a meaning — a class, an interface, a type parameter; every other bound is rejected at the declaration. §26.4: the block's `where` clause is the one place a block parameter is constrained. §12.9: nullability is part of a type argument's identity (§7.1); an `out` view widens `T` to `T?` and an `in` view narrows it, and neither reverses. §27.1.1: a function value's class or interface slot relates by a reference conversion, its nullable lift or the nullable view in the slot's direction, a `string` slot by the view alone (`() -> string` is not `() -> Object`); a value-typed slot, and a `ref` or `out` slot of any type, is invariant. §27.6: method-reference formation bridges a §5.1 conversion at a by-value slot only; a `ref` or `out` slot's type is the callee's exactly.

### 7.2 Ownership shapes that look fine

```bezel
class Box<T>
  private _o: Object?;
  public method Put(): (T) -> ()
    return lambda(t) => Registry.Take(t);        // ERROR: a lambda over the body's `T` is never returned (§28.3)
  end Put;
  public method Stash() => _o := lambda(t: T) => Registry.Take(t);   // ERROR: nor converted to Object
  public method Each(xs: Array<T>, visit: (T) -> ())
    var f := lambda(t: T) => visit(t);           // fine: bound to a local and invoked here
    for var x in xs do f(x); end;
  end Each;
end Box;
```

> §28.3 (*Function values a generic body forms over its type parameters*): a lambda, method reference or retyped value you form **inside a generic body** at a function type with the body's `T` in a parameter slot is invoked, bound to a local, captured or passed to a function-typed parameter only — never returned (from a lambda either), stored (a `scoped` field excepted), converted to `Object` or to another generic's bare-`T` slot, or varied away as a type argument: at a function-type `T` the caller would invoke what you handed out with a closure your body never checked. Declaring a `(T) -> ()` field or return is fine; filling it from the generic body is not. A `() -> T` return is fine, except at a `Task`- or iterable-family `T`, where `Holder<Task>` is rejected wherever it is named (the caller would mint what the body lends).

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
class Registry
  private static owned readonly _m: Mutex := new Mutex();
  public static method Shutdown()
    _m.Dispose();                  // ERROR: a `static owned` field read is a borrow of always-live storage
  end Shutdown;
end Registry;

static class Pool
  public static Slots: [4 owned]Conn?;          // ERROR at the DECLARATION: a mutable static holding owned referents
  public static readonly Fixed: [2 owned]Conn := [new Conn(), new Conn()];
  public static method Swap()
    var t := move Fixed[0];        // ERROR: a `static readonly` owning inline array's slots are fixed
    Fixed[0] := new Conn();        // ERROR: same
  end Swap;
end Pool;
```

> §28.6, §22.12 note (h), K9.4: static owned storage — a `static owned` field, a `static readonly` owning inline array's slots — admits no `move`, no slot store and no explicit disposal; only the program-exit pass (§22.13) disposes it. An early shutdown is a `Close()`/`Flush()` method on the type, a `Locked<T>` registry whose guarded value is replaced, or a `Shared<T>` handle; a replaceable process-global pool is `private static owned readonly _pool: OwningArray<Conn>`, whose indexer store disposes the displaced element (§28.11), or `Locked<OwningArray<Conn>>`.

```bezel
class Registry
  private static owned readonly _m: Mutex := new Mutex();   // disposed by the exit pass, and only there
end Registry;

static class Pool
  private static owned readonly _pool: OwningArray<Conn> := new OwningArray<Conn>();
  public static method Swap(i: int)
    _pool[i] := new Conn();        // the store disposes the displaced element
  end Swap;
end Pool;
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
method Finish(owned conn: Connection)
  using var c := conn;             // ERROR: neither adopts nor views — say which
end Finish;
```

> §28.10: over a class- or interface-typed named **owned** binding — a `var`/`using var` local, an `owned` parameter, an owned loop variable, `x!`, a bare `T` — the unmarked `using var` is rejected. Adopt with `move`; to look without taking, bind `var`/`const`; to hold a local whole-scope, declare *it* `using var` (and then never `move` it — A5). A call, field, indexer or borrowed-parameter right-hand side is still an ordinary borrow-pin.

```bezel
method Finish(owned conn: Connection)
  using var c := move conn;        // adopt: c disposes it at this block's end
end Finish;
```

```bezel
var a := new Buffer();
var b := new Buffer();
using var best := (if c then a else b);   // ERROR: every arm is an owned binding — say which
```

> §28.10: an `if`, `case` or `??` arm-meet whose every arm is an unmarked read of a named owned binding is the same ambiguity one parenthesis deeper (`null` and `throw` arms absorb and do not count). Mark the arms to adopt whichever is selected; `var`/`const` to view. A meet with a call, field, indexer or borrowed-parameter arm is not covered and is still a borrow-pin.

```bezel
var a := new Buffer();
var b := new Buffer();
using var best := (if c then move a else move b);   // adopt: the selected one is disposed at this block's end
// over `a ?? b` (A7 rejects `move` there): `(if a <> null then move a else move b)`;
// over `x ?? throw new E()`: `(if x <> null then move x else throw new E())`
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
async method CloseAsync(d: IAsyncDisposable): Task  await d.DisposeAsync();  end CloseAsync;   // ERROR: same rule — `d` is borrowed
```

> §28.6: `Dispose()` needs a receiver you own. A borrowed parameter, `this`, a getter result, a `catch` binding, a pattern binding and an element borrow of *either* family are all rejected, with no exception. `DisposeAsync()` takes the same rule, awaited or not; the one `this`-rooted call admitted is `base.DisposeAsync()` inside your own `DisposeAsync` override.

```bezel
var a := new Widget();
var b := a;                       // a borrow of a's value
a.Dispose();                      // OK: an explicit disposal is never frozen
Console.WriteLine(b.Name);        // ERROR: the disposal of `a` precedes this use on every path
a := new Widget();
Console.WriteLine(b.Name);        // ERROR: `b` still borrows the disposed value
b := a;                           // OK: a fresh borrow of the new value
Console.WriteLine(b.Name);        // OK

var s := xs.AsSpan();
xs.Dispose();
Console.WriteLine(s[0]);          // ERROR: same rule through a span

var c := a;
if cond then a.Dispose(); end;
Console.WriteLine(c.Name);        // compiles; faults in checked mode when the branch ran
```

> §28.2: a use — read, member access, argument, re-borrow, consume — of a local after an explicit `Dispose()`, `DisposeAsync()` or discard that precedes it on every path is a compile error, and so is a use of an alias, `const`, `using var` pin, span, borrow-bearing record, closure or frame over the disposed value. Only a repeated disposal of the same binding (a no-op) stays a warning. A disposal on some paths only is left to the run time (`DisposedAccessException` in the default, checked build). Dispose after the last use.

```bezel
public class Holder
  private owned _w: Widget := new Widget("first");
  public method Swap()
    var b := _w;                    // a borrow of the field's current value
    _w := new Widget("second");     // OK: a field write is never frozen — it disposes the first widget
    Console.WriteLine(b.Name);      // ERROR: the write precedes this use on every path, with nothing between
  end Swap;
  public method Close()
    _w.Dispose();
    Console.WriteLine(_w.Name);     // ERROR: a read of the field after its explicit disposal, same window
  end Close;
  public method Rotate()
    var b := _w;
    Log();                          // a call: it could have taken the value out (`move _w with …`)
    _w := new Widget("third");
    Console.WriteLine(b.Name);      // compiles; faults in checked mode
  end Rotate;
end Holder;
```

> §28.7: a local borrow of an owned field's value (`var b := _w;`, `var b := h.W;` with `h` a local or parameter) used after a reassignment or explicit disposal of that field that follows the borrow with no call (a constructor's included), accessor, user operator, function-value invocation, `await`, `yield return`, disposal of another value, other write to the path or `move` of any field in between, and precedes the use on every path, is a compile error; so is a read of the field after its own explicit disposal in that window. The write is not frozen. Use the borrow first, or take the value out with the replacing `move`. A write on some paths only, a window with a call in it, a borrow held through an alias, closure, span or record, and an owning-collection slot (`var w := xs[0]; xs.RemoveAt(0); w.Ping();`, §7.5) stay run-time faults.

```bezel
pool.Clear();                                                  // disposes every element through the owner
for var conn in pool.Drain() do using var c := move conn; ... end;
```

```bezel
var tasks := new Array<Task<string>>();
tasks.Add(File.ReadAllTextAsync(path));    // ERROR: the fresh task has no holder — abandoned at the statement's end
var conns: Array<Connection> := [new Connection("a")];   // ERROR: hard rejection at the literal site
_ := DoTelemetryAsync();                                 // ERROR: a Task may not be discarded
```

> §15.4's fresh-store rule on the borrowing family: a fresh unbound owned temporary — `new`, a collection literal, an `owned`-returning call, a `move`, a `Task`-family call — in `Add`/`Insert`/indexer-set position (and a fresh `OwningDictionary` key) is rejected at the store site, as the collection-**literal** form of the same shape is at the literal site; bind first (`using var t := …; tasks.Add(t);`) or use the owning family. §17.13: `Task.FireAndForget(t)` (supervised) or `using var t := …;` (unsupervised) are the two legal not-awaiting spellings — *named*; the anonymous `using var _ := t;` is rejected like the discard.

```bezel
using var t1 := File.WriteAllTextAsync(pathA, report);   // named holders
using var t2 := File.WriteAllTextAsync(pathB, report);
var both: Array<Task> := [t1, t2];
await Task.WhenAll(both);
Task.FireAndForget(DoTelemetryAsync());
```

```bezel
public method Wrong(): owned FileStream
  return move _resource;           // ERROR: `_resource` is non-nullable — write `move _resource with fresh`, or declare it `FileStream?`
end Wrong;

public static async method GetAsync(h: Box): Task<Cell>
  var t := move h.item;            // ERROR: same — `item` is a non-nullable owned field of `Box`
  return t;
end GetAsync;

private owned _lastError: Exception?;
try Risky(); catch
  var ex: Exception => _lastError := ex;    // ERROR: `ex` is a borrow whose lifetime is the arm
end;

method Header(): Span<byte>
  using var buf := new OwningBuffer<byte>(64);
  return buf.AsSpan();             // ERROR: a borrow-bearing record may not outlive its buffer
end Header;
```

> §28.8.1: a `move` of an owned field or slot **replaces** — `move loc with e` stores `e` in the step that takes — and the replacement is required wherever the location's declared type is non-nullable; a bare `move` stores `null` at a nullable one. A caught exception is a borrow: copy the strings out. §9.7: spans may not be returned past their source, stored in class fields, or used as `async` parameters or results.

```bezel
public method Right(): owned FileStream
  return move _resource with new FileStream(_path, FileMode.Open);   // one step: store the fresh stream, yield the old
end Right;

public static async method GetAsync(h: Box): Task<Cell>
  return move h.item with new Cell();   // at any receiver (`this`, a parameter, a local holder), in any body
end GetAsync;
```

```bezel
destructor
  using var t := move _resource;   // ERROR: `_resource` is non-nullable — `_resource.Dispose();` releases it here, `move _resource with fresh` exchanges, or declare it `FileStream?`
end destructor;
```

> §28.8.1 *Constructors and destructors*: a destructor body is no exception to the required `with` — its fields are owned locations like any other. To release a field before the rest of the body runs, dispose it in place (§28.6's early release point; the cascade's own call afterwards is the no-op); to hand its value onward, declare the field nullable.

```bezel
destructor
  _resource.Dispose();             // early release; the cascade's own call afterwards is a no-op
  Console.WriteLine("closed");
end destructor;
```

```bezel
var t := move xs[0];               // ERROR: an `OwningArray<T>` slot is non-nullable — `move xs[0] with fresh`, or `xs.PopAt(0)`
using var taken := move d[key];    // ERROR: same — `move d[key] with fresh`, or `d.PopValue(key)`
var w := move a[0];                // ERROR: same at a `[2 owned]Cell` — `move a[0] with fresh`, or declare `[2 owned]Cell?`
Sink(x, move x);                   // ERROR: the first argument is a live borrow of `x` at the consume (§28.3, K5)
Keep(new Pool().First());          // ERROR: `First()` lends from an unbound temporary; a borrow of a temporary feeds no owned parameter (§28.3, K8.4)
```

> §28.11: `move coll[e]` takes any index expression, evaluated once; the slot holds the replacement — or `null` on a nullable slot (`OwningBuffer<T>`, a class-typed `Span<T>`, `[N owned]T?`) — in the step that takes, so nothing is restored, tracked or frozen afterwards; an `OwningArray<T>` or `OwningDictionary<K, V>` slot is non-nullable and needs `with` or a `Pop*` form. §28.3: an argument evaluated before a consuming argument of the same call is live at the consume. §28.6: a chain over a `new` root is a borrow of that temporary, usable inside its full expression only — the builder idiom binds the root (`using var sb := new StringBuilder(); sb.Append("a").Append("b");`).

```bezel
var t := move xs[Pick()] with new Res(9);          // any index; the slot holds the fresh element
using var taken := move d[$"m{i}"] with fresh;     // any key; absent → KeyNotFoundException, nothing stored
using var u := xs.PopAt(0);        // take with the closing form; the collection may mutate afterwards
xs.RemoveAt(0);
var v := move _r with fresh;       // the field holds `fresh`…
Log("taken");                      // …so a call afterwards needs no care
var w := move p._r with fresh;     // through any holder…
p := q;                            // …which may be rebound at once
using var m := move buf[i];        // a nullable buffer slot: takes, leaves null
Sink(move x);                      // transfer first; borrow from the new owner afterwards if needed
using var p := new Pool();
Use(p.First());                    // a plain parameter: the borrow is bounded by `p`
```

```bezel
var it := Each(xs.AsSpan());       // an iterator over the span; or: var q := xs.Where(f);
sink.Take(move xs);                // ERROR: it is still live — a frame local is a live borrow of its carried sources (K5.5)
for var x in it do Console.WriteLine(x); end;
```

> §28.3: a local holding an iterator or enumerator frame or a lazy query counts as a live local borrow of every source the frame carries, as a closure does. The frame's disposal counts too (it may run the iterator's pending `finally`): enumerate it through an unbound call or dispose it before the transfer, or transfer first and re-derive the frame from the new owner.

```bezel
for var x in Each(xs.AsSpan()) do Console.WriteLine(x); end;   // the frame is consumed within xs's life...
sink.Take(move xs);                                             // ...then the transfer
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

```bezel
record Pair(Item: Item);
class Keeper
  private owned _keep: Array<Pair> := new();
  public method Arm()
    using var it := new Item();
    var xs: Array<Pair> := [new Pair(it)];   // a local collection: this store is admitted
    _keep := xs.ToArray();                   // ERROR: the copy stores `Pair` borrows of `it` into a field-held collection
  end Arm;
  public method Snapshot(xs: Array<Pair>): owned Array<Pair> => xs.ToArray();   // ERROR: the copy escapes the method
end Keeper;
```

> §9.7 / §28.11: the value-copy members — read off the stdlib signature, no marker: a member returning an `owned` collection (`ToArray`, `Sorted`, `Reversed`, slicing, `new Array<T>(size, fill)`, `LinkedList<T>(source)`, a direct `FromCollectionLiteral` call, LINQ's `ToArray`/`ToSet`/`ToDictionary`), a borrowing-family or span member taking `IEnumerable<T>`, a read-only span or a `T` and returning nothing (`AddRange`, `UnionWith`, `Fill`, `CopyFrom`, `Counter<K>.Increment`), or one writing a span argument (`CopyTo`) — copy each element **with its borrows**, sourced at the source expression, and the copy is a store into wherever the result lands — checked as your own `Add` of each element would be. A local copy is fine; a field, a return or a wider collection is not: at a value element that holds borrows with no residual, and at a class element whenever the source is a scope-bound local.

```bezel
using var ys := xs.ToArray();                                  // a local copy: admitted where `ys.Add(xs[0])` would be
public method Snapshot(): owned Array<Item> => _items.ToArray();   // class elements, receiver-sourced: admitted under the residual
```

```bezel
class Holder<T>
  public _v: T;
  public constructor(x: T)
    _v := x;
  end constructor;
end Holder;

static method Make(item: Item): owned Holder<() -> int>
  var f: () -> int := lambda() => item.Value();
  var k := new Holder<Object>(f);          // ERROR: `x: T` at T = Object is the same set-erasing position; f captures item
  return new Holder<() -> int>(f);         // ERROR: `x: T` at T = () -> int is a set-erasing position; f captures item
end Make;
```

> §28.3: a generic body checks `x: T` as an object borrow, so at a function-type `T` the caller's argument must be capture-free; the same holds for any parameter declared `Object`, and for a bare `T` your call instantiates at `Object` (`Holder<Object>`, `keep.Put(f)` at `Keep<Object>`, `new Box<Object>(f)`). Keep the callback where its type says what it is: a function-typed parameter invoked inside the call, or a `scoped` one on a class that stores it.

```bezel
class Runner
  private scoped _job: () -> int;
  public constructor(scoped job: () -> int)
    _job := job;
  end constructor;
  public method Run(): int => _job();
end Runner;
```

```bezel
class Registry
  private _o: Object? := null;
  public method Keep(item: Item)
    var f: () -> int := lambda() => item.Value();
    _o := f as Object;                       // ERROR: `as Object` is a set-erasing position; f captures item
  end Keep;
  public method Run(): int => (_o as () -> int)!();   // OK — a function value narrowed from `Object` is capture-free
end Registry;
```

> §28.3: a closure reaches `Object` only capture-free, by an implicit conversion or by `as` alike — the `as` is the rejection site, as a span's box is (§9.7). Keep the closure at a function type: a `scoped` field on the class that stores it, or a `scoped` parameter.

```bezel
if f is Object then sink.Keep(f); end;              // ERROR: a narrowing converts nothing — read as `sink.Keep(f as Object)`; f captures item (§12.7, §28.3)
if v is Object then _o := v; end;                   // in Probe<T> over v: T: §9.7's conversion of a bare T to Object — rejected wherever Probe<Pair> is named
```

> §12.7: an `is` test narrows the carrier and converts nothing; where borrows are read, a narrowed use is the `as` of the carrier at its declared type, and takes that `as`'s verdict. Keep the closure, or the bare-`T` value, at its own type; the test itself, and reads through the narrowing, are unaffected. (A span is a value: `s is Object` over one is §12.7's value-operand error outright.)

```bezel
interface ISink<V>  method Take(f: (V) -> ());  method Put(f: () -> V);  end ISink;
class Feeder: ISink<() -> int>
  public method Take(f: (() -> int) -> ())
    using var it := new Item();
    f(lambda() => it.N());                           // ERROR: `f` is read at the declaring slot `(V) -> ()`, a set-erasing position; the closure captures it (§28.3)
    Helper(f);                                       // ERROR: over `Helper(k: (() -> int) -> ())` — `f` passes only to a slot declared at a type parameter here
  end Take;
  public method Put(f: () -> () -> int) end Put;
end Feeder;
class Box<T>
  public V: T;
  public method Hand(): Object => lambda(b: Box<T>) => b.V;        // ERROR: a body-formed value with `T` at its return slot reaches no set-erasing position (§28.3)
  public method Export(sink: Array<(Box<T>) -> T>) => sink.Add(lambda(b: Box<T>) => b.V);   // ERROR: the same value at a collection slot; and `sink`'s type is an exported mint slot, so `Box<IEnumerable<int>>` and `Box<Task>` are rejected wherever named (§28.3)
end Box;
var r1 := tally._cb(10);                             // ERROR: over `public scoped _cb: (int) -> int` in `Tally` — a `scoped` field is invoked by its own class only (§28.14)
```

> §28.3, §28.14: an implementation of a generic member reads its function-typed parameter at the declaring signature, so invoke it only with your own capture-free values and pass it only on to a slot declared at a type parameter; a function value a generic body forms with `T` at its return slot stays out of `Object` and collections — return it at its declared function type; and give the class that holds a `scoped` field a method that invokes it.

```bezel
public method Window(): ArraySpan<int>
  var xs: Array<int> := [1, 2, 3];
  var s := xs.AsSpan();
  var f := lambda() => s;
  return f();                              // ERROR: the result carries f's set — `s` views the local `xs`
end Window;
```

> §28.3 / §9.7: a closure that captures a span, a borrow-bearing record or inline array, or a choice that holds borrows carries the sources of the borrows that value holds, exactly as passing the value as an argument does. Invoking the closure in its sources' scope is fine; returning its result, returning a task or iterator frame that rides it, or disposing a source while a `scoped` holder keeps it, is rejected — and, like any closure that captures anything, the closure itself may not be returned or stored, even when the span views a parameter. A method reference bound to such a value (`pair.Show`) counts the same.

```bezel
private _f: (() -> int)? := null;
public method Arm(): ()
  var b: Item? := null;
  var f := lambda() => b!.N();
  _f := f;                                 // ERROR: f captures `b`, which is later assigned a borrow of `it`
  using var it := new Item();
  b := it;
end Arm;
```

> §28.3 / §27.5: a captured variable is one shared cell, so a closure over it carries the union of **every** assignment to that variable — before or after the lambda, inside another lambda's body included — not the value it held when the lambda was formed. `b` is assigned a `using var`, so `f` is never storable or returnable, and `return b;` after a lambda body that assigns `b` reads the same union. Counters (`n := n + x`) and `string`s contribute nothing and are unaffected; to capture a snapshot, bind a second variable: `var c := b; var f := lambda() => c!.N();`.

```bezel
case move outcome of
  Ready(move conn = var c, attempts = var n) => Adopt(move c),   // ERROR: `move` is not a slot spelling (retired)
  Empty => ()
end;
```

> §19.3: a named slot borrows or copies and never transfers, in an owning match included. To own a payload, destructure positionally — `Ready(var c, var n)` transfers every owned payload at once, and `Ready(var c, _)` disposes the discarded sibling with the construct.

```bezel
var s := "hello";
s.Dispose();                                     // ERROR: `string` has no `Dispose()` — runtime-managed, nothing to dispose (§8.7, §28.9)
var il: IList<int> := new Array<int>(2);
il.Dispose();                                    // ERROR: `IList<T>` does not extend `IDisposable`, so `Dispose` is not a member — `_ := il;`, `using var`, or scope end (§8.7)
using var d := new Connection("a");
var f := d.Dispose;                              // ERROR: no method reference to `Dispose`/`DisposeAsync`, on any receiver — write `d.Dispose();` or `_ := d;` (§27.6)
method Drop<T>(owned x: T) where T: class  x.Dispose();  end Drop;   // ERROR: `where T: class` admits `string` and interfaces without the member — `_ := x;`, or `where T: IDisposable` (§8.7)
```

> §8.7, §28.9, §27.6: `Dispose()` is a member of class types and of the interfaces that are or extend `IDisposable` or declare it, and of nothing else — `Object`, `string`, `Type`, boxes, function values and every other interface type (`IList<T>`, `IEnumerable<T>`, a user interface without it) have none, nor has a `T` under `where T: class` alone, and an owned value held at such a type is disposed by scope end, `using var` or the discard, which dispatch to the runtime type. A method reference to `Dispose` or `DisposeAsync` is rejected at its formation on every receiver: disposal is announced at its site, never handed out as a function value. A callback that must dispose takes an `owned` parameter — `lambda(owned x: Connection) => x.Dispose()`, called with `move d`.

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

public enum Priority
  Low, Medium, High;
end Priority;
method IsUrgent(p: Priority): bool => p >= Priority.High;  // ERROR: an enum has no intrinsic `<`/`>` (§10.5); write int(p) >= int(Priority.High)

public enum Color
  Red, Green;
  public method Equals(other: Color): bool => this = other;   // ERROR: an enum declaring its own `Equals` has no `=` until it overloads it (§10.5, §8.4) — write int(this) = int(other)
  public method GetHashCode(): int => int(this);
end Color;
var same := Color.Red = Color.Green;                           // ERROR: declare `operator =`/`<>` (§25.2), or call `.Equals`; a Set<Color>, `case` over Color and a `Pixel(Color.Red, var x)` slot (matched by identity, §10.6) are fine

// In an owning choice `Pool` with `Ready(conns: [4 owned]Conn?)`:
case this of
  Ready(arr) => do k(); return arr[0]!.Id; end,  // ERROR: `arr` used after `k()`; `this` may be overwritten (§19.3)
  Empty      => return -1;
end;
// Fix: read first.   Ready(arr) => do var id := arr[0]!.Id; k(); return id; end,

// In `class Box<T>` with `private _v: T? := null;` (§7.10's advice for an unconstrained T):
var b := new Box<[3]int>();                      // ERROR: `T?` at an inline-array `T` forms `([3]int)?`, a type §15.1 admits nowhere (§3.6 rule 2)
using var verts: Array<[3]float> := [[0.0f, 0.0f, 0.0f]];   // fine: the type is well-formed, indexed and iterated as any other
var first := verts.FirstOrNull();                // ERROR: the member's `T?` result has no form at this element type (§14.2, a conditional member); test `Count` or index
```

> §9.4: in every instance member body of a value kind, a store landing in the receiver's own storage is rejected — compound assignment and `++`/`--` included, through their desugarings. §28.5: copy-semantics holders cannot own; use a `class`. §9.2/§10.5/§35.2: a record, enum, or choice member — method, property, or indexer — admits no `virtual`, `abstract`, or `sealed override`, and `override` only on `Equals`/`GetHashCode`/`ToString`; value kinds are sealed and dispatch statically. §9.8: a serialized name (the rename, or the identifier where none is declared; compared ordinally) must be unique across a type's components, its enum constants, and its choice cases — a choice's payload fields are unique **within their own case**, two cases never sharing a wire object. §10.5: an enum has no intrinsic `<`/`<=`/`>`/`>=`. C# orders enums by value, but Bezel does not. Compare `int(a) < int(b)`, declare the four-operator group on the enum, or implement `IComparable<E>` for `Sort`/`OrderBy`. A relational pattern over an enum is rejected either way (§19.3). §19.3/§9.4: a borrowing-match binding of a move-only payload over a source the body cannot prove unwritten — `this`, a mutable field, an element, a `ref` parameter — may not be live across a non-`pure` call; over a local that no lambda assigns, it may.

```bezel
public method Bumped(): Tally => this with(Hits := Hits + 1);   // return the updated value
```

```bezel
public choice Tag
  A(n: int),
  B;
  public method Equals(other: Tag): bool => true;   // takes over equality: the generated `=`, `Equals`, `GetHashCode` and `ToString` go (§35.7)
  public method GetHashCode(): int => 0;
end Tag;
var same := Tag.A(1) = Tag.A(2);     // ERROR: no `=` without the operator overloads — declare `operator =`/`<>`, or call `.Equals` (§35.7, §25.2)
var seen := new Set<Tag>();         // fine: the declared pair makes `Tag` hashable (§3.6)
Console.WriteLine($"{Tag.B}");      // ERROR: no generated `ToString` beside a declared `Equals` — declare `ToString()` (§35.7, §8.5)
```

> §35.7 / §9.2: on a record and a value-like choice alike, declaring `Equals` or `GetHashCode` in either spelling takes over equality — the generated package goes, `=`/`<>` need the operator overloads, and the declared pair alone makes the type **hashable** (`Set<Key>`, `where T: hashable`). A record declaring `Equals(other: Key)` with `GetHashCode` and no `=` is a hash key on the same terms. An interface member `Equals(other: Object?)` is satisfied by a value kind's own `Equals`, declared or generated; a kind with none does not conform (§14.2).

```bezel
public record Ticket(Id: int)
  public ToString: int := 0;                                          // ERROR: `Equals`, `GetHashCode` and `ToString` are reserved for the Object slot on every value kind (§9.2)
  public static method Equals(a: Object?, b: Object?): bool => false;   // ERROR: the same rule — a static of one of the names is rejected at any parameter list
  public method GetType(): int => Id;                                   // ERROR: `GetType` and `ReferenceEquals` are reserved on every value kind too, at every member kind and parameter list (§9.2)
  public method Equals(other: Ticket, tolerance: int): bool => true;    // fine: an instance method at another parameter list is an ordinary member (§10.5)
end Ticket;
```

> §9.2: a value kind reserves the three names exactly as §12.2 does on a class — an instance method at another parameter list is an overload, everything else of the name (a static, a field, a property, a `const`, a nested type) is rejected; `GetType` and `ReferenceEquals` have no admitted spelling at all on a value kind, and an interface declares neither (§11.2). A `static class` is outside the class rule: `Convert.ToString(value: int)` is the standard library's own instance (§11.7).

A record whose constructor carries a contract clause:

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
var q: Port;                       // fine: a non-defaultable local starts uninitialized (§7.9)
_ := q.Value;                      // ERROR: `q` read before it is assigned
var slots: [8]Port;
slots[0] := new Port(1);           // ERROR: an element store uses the unassigned array; assign the whole array
var r := p with(Value := -1);      // ERROR: `with` is rejected on a constructor-only record
```

> §9.9: a record is **constructor-only** when it declares a constructor carrying a `requires` **or** `ensures` clause — any constructor, any visibility, any tier. The property reads the *declaration*, never what the clause says, and is constant across a generic record's instantiations. A constructor-only record is not defaultable and rejects `with`; `Port` at a `where T: new()` substitution is also rejected. A record declaring **positional fields** may not declare a clause-bearing constructor (a compile error at the record declaration — the remedies are a **body record**, so every constructor is declared, or a **static factory method**), and a `serialized optional` component whose type is not defaultable is rejected (§9.8). In every other respect such a record is ordinary.

```bezel
var p := new Port(8080);           // a constructor or factory call
var opt: Port? := null;            // where a zero value is genuinely wanted: Port? / default(Port?)
public method WithValue(v: int): Port => new Port(v);      // the sanctioned update shape
```

```bezel
case h.Slots of                      // `h.Slots: [4 owned]Conn?` — a borrowing binding over an owning-inline-array FIELD
  var a => do
    var t := move h.Slots;           // ERROR: the field's type is non-nullable — write `move h.Slots with empty`
    h.Slots := t;
  end
end;
```

> §15.3 / §9.4 / §28.8.1: a whole-value `move` out of a move-only **field** is the replacing `move`, and an inline array or an owning choice declared without `?` is non-nullable, so the replacement is required (`var empty: [4 owned]Conn?; var t := move h.Slots with empty;`); the field holds the replacement, visible whole, from the step that takes, and a `case` binding taken over the field before the move reads the replacement afterwards (`a[0]` is `null`). Match a local instead when the old slots are what you want to read (`var t := move h.Slots with empty; case t of …`).

```bezel
// In `record Box` with `public property Item: Item?;` (a settable member) over a class `Item`:
var b := new Box();
do
  using var it := new Item();
  b.Item := it;                      // ERROR: `b` outlives `it` — a member store adds `it` to `b`'s borrows
end;
```

> §9.7 / §15.1.2: a store into a record's settable member (`b.Item := e`, `b.Slots[i] := e`) adds the borrows `e` carries to the record's one source set — a later store never removes one — and is checked as an assignment of the whole record: at a local, rejected where the local is declared in a block that strictly encloses a source's; at a class instance or static field, admitted only in the null-source form, every reference-typed member `null`. Declare the record inside the source's block, or keep the source and build the record where it is used.

```bezel
public record Pt(X: int)
  public method Adder(): (int) -> int => lambda(d: int) => this.X + d;   // fine: `this` is captured BY COPY (§9.4) — Pt holds no borrow, so the closure is capture-free and may be returned or stored
end Pt;
public record View(Item: Item)                                           // over a class `Item` with `public Name: string` — View holds a borrow
  public method Label(): () -> string => lambda() => this.Item.Name;      // ERROR: the copied `this` carries Item's borrow — a closure with a non-empty source set cannot be returned, stored or added to a collection (§28.3)
  public method Show(): string
    var f := lambda() => this.Item.Name;
    return f();                                                          // fine: bound and invoked inside the body
  end Show;
end View;
```

> §9.4 / §28.3: a closure formed in a value-kind member body captures `this` by copy of the fixed receiver value, and contributes what a captured local of the receiver's type contributes — nothing over a type that holds no borrows, the carried borrows' sources otherwise. `var self := this; return lambda() => self.X;` and `return this.Add;` (§27.6) are the same closure in other spellings. A move-only receiver's `this` (an owning choice) is a borrow bounded by the call: a closure over it may be invoked in the body and may not leave it (§28.9). In a record's constructor body the capture is a read of `this`, admitted only once every field that needs an assignment is assigned, and the closure keeps the value as it was then (§22.7).

```bezel
public static method Show(o: Object)
  Console.WriteLine(o.ToString());
end Show;

using var buf := new OwningBuffer<int>(4);
Show(buf.AsSpan());                           // ERROR: a span converts to `Object` only as a member-call receiver — the callee could store the box
Console.WriteLine(buf.AsSpan());              // ERROR: the same position, in a standard-library signature
var o: Object := buf.AsSpan();                // ERROR: a local declared `Object` is the same position
Console.WriteLine(buf.AsSpan().ToString());   // OK — a member call on the value; so is `$"{buf.AsSpan()}"`
var n := Sum(buf.AsReadOnlySpan());           // OK over `Sum(s: ReadOnlySpan<int>)` — a parameter at the span's own type
```

> §9.7 / §8.6: a value that holds borrows (a span; a record or choice with a class-, interface- or `Object`-typed field; an inline array of them) reaches no position declared `Object` or an interface — not a parameter (`b.Keep<Object>(span)` included), a local, a field, a collection or a return — because the box announces no borrow and a callee could store it. It boxes only as the receiver of a member call (`span.ToString()`, `$"{span}"`) — not of an extension on `Object` or an interface. Remedies: a parameter at the value's own type (two overloads, `(ReadOnlySpan<T>)` and `(ReadOnlyArraySpan<T>)`, for a member over either heap backing), or a class implementing the target interface.

### 7.4 Overloads and the `out` family

```bezel
var ids := xs.Select(Util.Id);                   // ERROR: a reference to a generic method names its type arguments
var ids := xs.Select(Util.Id<int>);              // the formation is the instantiation `Id<int>`, checked as the call would be
var keep: (Array<Object>, Pair) -> () := Util.Keep<Pair>;   // ERROR at the formation: Keep's `sink.Add(x)` boxes a Pair (§9.7), as `Keep<Pair>(…)` would
```

> §27.6 / §14.1: a method reference over a generic method, or over a member of a generic extension block (`IEnumerable<Pair>.KeepAll`, the qualifier binding the block's `T`), carries an explicit type-argument list; nothing is inferred from the target, and the formation is checked as the call with that list would be — the body's requirements at `T`, the constraints, the dispatched exclusion. Invoking the formed value reads only its function type. `numbers.OrderBy(Math.Abs)` is unaffected: `Abs` is not generic.

```bezel
method Hold(item: Widget);
method Hold(owned item: Widget);                     // ERROR at the second declaration: `owned` never distinguishes overloads
method TryGet(k: string, out v: Conn?): bool;
method TryGet(k: string, out owned v: Conn?): bool;   // ERROR at the second declaration, for the same reason
pool.TryPopCached("p", out owned var c);   // ERROR: no `out owned var` (retired) — `out var c` adopts owned; `out using var c` scope-binds it
seen.Add(candidate);                     // ERROR when Add takes `owned`: write Add(move candidate)
r.Append(move new Widget());             // ERROR: a fresh producer announces itself
return move fresh;                       // ERROR: `return` under `owned` is a marker-free position
```

> §22.6: `owned` — on an ordinary parameter as on an `out` parameter — and `scoped` never distinguish overloads; a pair differing only by one of them is rejected at the second declaration (§11.3). The convention is **distinct names** for the borrow/take pair: `TryGetValue` vs `TryPopValue`, `TryPeek` vs `TryPop`/`TryDequeue`, `Hold(item)` beside `Adopt(owned item)`. A method's own type-parameter count distinguishes overloads too (`FromException<T>(e)` beside `FromException(e)`; an explicit list picks by its length, and a bare `FromException(e)` is the non-generic `Task` form even where a `Task<int>` is wanted, so write `FromException<int>(e)`), and type-parameter names do not: `M<T>(x: T)` and `M<U>(x: U)` clash (§11.3). A generic method's own `where` constraints never pick an overload: they are checked on the chosen candidate, so a call that reaches a candidate whose constraint fails is an error, not a fall-back (`M(5)` with `M(o: Object)` beside `M<T>(x: T) where T: class` is rejected, §14.5; an extension block's `where` is different: it filters which blocks apply, §26.4). §22.6/§14.5: a method-reference argument ranks at an overloaded callee as a lambda literal does — exact where its signature equals the (substituted) slot, converting where formation bridges (`Task<X>` → `Task`) — and a candidate whose function-typed slot has a different parameter count from the argument's is simply inapplicable, so an overload family keyed on handler arity resolves to the one member that fits.

```bezel
method Hold(item: Widget);                                            // borrow
method Adopt(owned item: Widget);                                     // take — its own name
method TryGetCached(k: string, out v: Conn? when true): bool;        // borrow
method TryPopCached(k: string, out owned v: Conn? when true): bool;  // take
if not seen.Contains(candidate) then  seen.Add(move candidate);  end;
r.Append(new Widget());
return fresh;
```

```bezel
public class Point: IEquatable<Point>
  public readonly property X: int;
  public method Equals(other: Object?): bool => ReferenceEquals(this, other);   // ERROR: `Object`'s own signature without `override` is §12.2 hiding
  public method Equals(other: Point): bool => X = other.X;                      // fine: the Object slot's own-typed spelling (§8.3)
  public override method GetHashCode(): int => X;                               // required beside the own-typed Equals (§8.3)
end Point;
public class Both: IEquatable<Both>
  public method Equals(other: Both): bool => true;
  public override method Equals(other: Object?): bool => true;   // ERROR: the own-typed Equals already fills the Object slot — one spelling, never both (§8.3)
  public override method GetHashCode(): int => 0;
end Both;
public class Tag: IEquatable<Tag>
  public method Equals(other: Tag): bool => true;   // ERROR: an own-typed Equals obliges `override GetHashCode()` (§8.3)
end Tag;
public record Money(Amount: int)
  public override method Equals(other: Object?): bool => true;   // ERROR: on a value kind the Object slot is the own-typed Equals — write `Equals(other: Money)` (§9.2)
  public override method GetHashCode(): int => Amount;
end Money;
public record Tagged(X: int)
  public method Equals(other: Tagged): bool => X = other.X;   // ERROR: the own-typed Equals is the Object slot and obliges `GetHashCode()` beside it (§8.3)
end Tagged;
public record Coin(Amount: int)
  public method Equals(other: Coin := default(Coin)): bool => Amount = other.Amount;   // ERROR: the Object slot's parameter declares no default (§8.3) — drop it; a class's `Equals(other: C := …)` is rejected the same way
  public method GetHashCode(): int => Amount;
end Coin;
```

> §12.2: `Object`'s `Equals(Object?)`, `GetHashCode()` and `ToString()` block only their **own signature**, which `override` reaches. A class declares `Equals(a: T, b: T)` and `GetHashCode(value: T)` (`IEqualityComparer<T>`) or `ToString(format: string)` (`IFormattable`) as plain members beside the inherited ones. `Equals(other: T)` at the class's own type is the `Object` slot's second spelling (§8.3), its parameter declaring no default (`Equals(other: T := …)` is rejected at the declaration, on a value kind too): one virtual member, reached by `p.Equals(q)`, `p = q`, a `Set<Point>`, `IEquatable<Point>`, an `Object`-typed receiver or argument (a non-`Point` argument compares unequal) and a subclass operand alike; a subclass overrides it as `override Equals(other: Sub)` or `override Equals(other: Object?)`. It obliges `override GetHashCode()`, to agree with it, as an `Equals` override does (§8.3), and a class writes one spelling of the slot, never both. A static `Equals` stays rejected.

```bezel
public class Point: IEquatable<Point>
  public readonly property X: int;
  public method Equals(other: Point): bool => X = other.X;
  public override method GetHashCode(): int => X;                        // agrees with Equals(Point): §8.3
end Point;
var same := p.Equals(sub);                                               // a Point3 argument: the one slot member, Equals(Point) (§8.3)
var eq := p = sub;                                                       // `=` is the same call (§8.4)
var seen := new Set<Point3>();  seen.Add(sub);                           // Point3 is hashable: the inherited slot and hash, virtual (§3.6, §9.2)

public class Money: IFormattable
  public readonly property Cents: long;
  public method ToString(format: string): string => $"{Cents}";          // beside the inherited ToString()
end Money;
```

```bezel
public interface J: I                     // over `interface I  method A(x: Widget): string;  end I;`
  method A(owned x: Widget): string;      // ERROR at J: an inherited member takes part in the clash check, and `owned` never distinguishes (§11.3)
  method A(x: Widget): string;            // fine: a redeclaration at the same signature is one member (§11.2's diamond rule: `IList<T>`'s indexer over `IReadOnlyList<T>`'s)
end J;
public class C: B<int>                    // over `class B<T>` declaring `virtual Q<U>(e: U, t: T)` beside `virtual Q<U>(e: U, t: int)`
  public override method Q<V>(e: V, t: int): int => 2;   // ERROR: the override matches two base members at `B<int>` (§12.1); the base pair is legal — derive from a `B<string>`, or rename one
end C;
public class ExactSlot: P                 // over `class P` declaring `virtual Q(x: Animal)` beside `virtual Q(x: Animal?)`
  public override method Q(x: Animal?): int => 3;   // fine: the exact match is the one overridden; `Q(Animal)` keeps P's body (§12.1)
end ExactSlot;
public class RefSlot: P2                  // over `class P2` declaring `virtual R(ref x: Animal)`
  public override method R(ref x: Animal?): int => 4;   // ERROR: the `T`-to-`T?` widening is a by-value parameter's; at `ref`/`out` the type matches exactly (§12.1, §11.2)
end RefSlot;
using var g := new G<string>();  g.Q("s");   // ERROR when `G<T>` declares `Q(x: T)` beside `Q(x: string)`: both exact at `G<string>`, ambiguous (§22.6) — a `G<int>` receiver, or distinct names
if d <> null then L(d); end;                 // fine: `d: Dog?` narrowed is read as `Dog` — exact at `L(Dog)`, converting at `L(Animal)` and at `L(Dog?)` (§22.6, §7.4)
G(null);                                     // fine at `G(string)` beside `G(string?)`: the nullable one; ERROR at `H(Dog?)` beside `H(Animal?)`, or `string?` beside `Object?`: ambiguous — `var n: Dog? := null; H(n);`
h.Pick(3);                                   // ERROR at `Pick(byte)` beside `Pick(short)`: `3` is an `int`, and §5.1's narrowing makes both applicable and both converting — write `h.Pick(byte(3))`; beside `Pick(int)` the `int` candidate is exact and wins (§22.6)
```

> §11.3: an interface's members, for the clash rule, are those it declares and those it inherits; two declarations at one signature are one member. §12.1: an `override` matches exactly one virtual member of the base as substituted at the derived class's base clause; where it matches one exactly and others only by `T`-to-`T?` widening, it overrides the exact one; that widening, an implementation's under §11.2 too, is a by-value parameter's, a `ref` or `out` parameter's type matching exactly. §22.6: an integer constant is read at its own type (`int` for a literal that fits `int`), and §5.1's constant narrowing is applicability only — a candidate it reaches converts, never exact; a narrowed read's static type is its non-nullable type (as §7.11 reads it); `null` is applicable at every nullable parameter, at no non-nullable one, and separates no two candidates; two members of a generic type that coincide at the receiver's instantiation are both exact, and the non-generic preference does not apply to them.

### 7.5 Iterators, queries, liveness

```bezel
choice Slot  Full(owned c: Conn), Empty;  end Slot;
public static method Once<T>(owned x: T): IEnumerable<T>
  yield return x;
end Once;
for var s in Once<Slot>(Slot.Full(new Conn())) do … end;   // ERROR: IEnumerable<Slot> — a move-only element type, rejected at the instantiation
```

> §28.11 / stdlib §4.12: the borrowing enumeration interfaces (and `EventSource<T>`/`AsyncEventSource<T>`) admit no owning choice and no owning inline array as `T`; an iterator declared over one, or a generic iterator instantiated at one, is rejected at the instantiation site. Yield the owning *class* that holds the choice, or drain an `OwningArray<Holder>`.

```bezel
using var pool := new OwningArray<Conn>();
pool.Add(new Conn());
pool.Add(new Conn());
var w := pool[0];
pool.RemoveAt(0);                 // disposed the element `w` borrows
w.Ping();                         // compiles SILENTLY; FAULTS in checked mode (DisposedAccessException)

using var taken := pool.PopAt(0); // OK: the element out first, no borrow left behind
taken.Ping();
```

> §28.11 (*Owning collections*): an element borrow is valid until its slot is mutated — an indexer-set, `Remove`/`RemoveAt`, `Clear()` — and no diagnostic tracks the slot; the use after the mutation is a mid-scope kill under the checked-mode backstop (§28.2). Re-take the borrow after the mutation, take the element out first with `PopAt`/`Pop`/`PopValue`, or mutate after the last use.

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

```bezel
using var probes := Probes(8);     // Probes: an iterator minting fresh Probe objects
var p := probes.First();           // compiles, but WARNS — a dead borrow
var arr := probes.ToArray();       // NOT a remedy: the slots borrow the same dead elements
```

> §29.11: materializing is **not** a remedy for the fresh-yield liveness hazard (section 4.5). At a **class or interface** element type the `owned` on `ToArray(): owned Array<T>` is the **container axis only**: a slot filled from an enumerator-owned yield dangles exactly as an accessor result does. The warning covers `First`/`Last`/`Single`/`ElementAt` and their `*OrNull` forms, `Min()`/`Max()`/`MinOrNull()`/`MaxOrNull()`, and `Aggregate`'s receiver leg, and the trace **passes through** a materializer result and through an `IGrouping<TKey,T>` loop variable: those are pass-through nodes, never roots — the verdict is always the root's. Where the producer is a **borrowing** minting iterator you cannot change, no retention remedy exists — consume before the next advance.

```bezel
for var p in Probes(8) do p.Run(); end;          // remedy 1: use each element in the borrowing body

using var kept := new OwningArray<Probe>();      // remedy 2: give the elements an owner
for var p in OwningProbes(8) do                  // OwningProbes(...): IOwningEnumerable<Probe>
  kept.Add(move p);
end;
var idle := kept.FirstOrNull(lambda(p) => not p.IsBusy());   // collection-backed ⇒ sound
```

```bezel
using var xs := Echo(new Res());        // Echo(owned r: Res): IEnumerable<int> — an iterator over an `owned` argument
var n := xs.Count();                    // spends the single enumeration right
for var x in xs do Plot(x); end;        // ERROR (§29.8, §29.11): an iterable over an `owned` argument is single-consumption
```

> §29.8: an iterator method that takes an `owned` (or move-only) argument mints a **single-consumption** iterable — the body runs once over the argument, and the consumption points, the static error, the run-time `InvalidOperationException` and the remedies are §29.11's, as for a lazy query; the argument is disposed when that one enumeration completes or is disposed. To re-enumerate, lend a class-typed resource instead of transferring it: a plain parameter keeps the iterable re-iterable, and the resource's own `using var` outlives every enumeration.

```bezel
using var r := new Res();
using var xs := Echo(r);                // Echo(r: Res): IEnumerable<int> — re-iterable; `r` outlives every enumeration
var n := xs.Count();
for var x in xs do Plot(x); end;
```

```bezel
using var e := Gen.Each<Slot>(Slot.Full(new Conn()));   // Each<T>(x: T): IEnumerable<int> — at the owning choice `Slot`, `x` is a move-only parameter
for var i in e do end;
for var i in e do end;                                   // ERROR (§29.8, §29.11): single-consumption at this instantiation — `Each<int>`'s result stays re-iterable
```

> §29.8, §15.3: a parameter typed by a bare `T` is a move-only parameter at every instantiation that substitutes a move-only type, so the iterable minted there is single-consumption and its one enumerator disposes the argument; the same method at a copyable substitution is re-iterable. An `async` method's bare-`T` parameter is likewise discharged by its frame at such an instantiation (§30.1).

### 7.6 Concurrency

```bezel
var cfg := LoadAsync().Result;                            // ERROR inside async code: blocks its own domain
using var j := Domain.Spawn(lambda() => Index(repo));     // ERROR: a spawn entry must be CAPTURE-FREE
private static _cache: Dictionary<string, int> := new Dictionary<string, int>();   // ERROR: mutable static
using var scope: TaskScope := new TaskScope();            // ERROR: TaskScope needs `await using`
public static method Make(): owned TaskScope return new TaskScope(); end Make;   // ERROR: a TaskScope is owned only by its `await using var` binding
await using var o: Object := new TaskScope();             // ERROR: same rule — no owned field, parameter, return slot, element, type argument or `Object`/interface conversion at an owning position
using var g := mutex.Acquire();                           // ERROR inside an async body: it blocks
Gate._m := m;                                             // ERROR: a parameter borrow stored into a static — only a fresh value, `null` or static storage may be
public async method GetAsync(): Task<Item> return _item; end GetAsync;   // ERROR: a borrow is no Pattern A result — return a fresh value or `return await`
public class FastBuilder: StringBuilder end FastBuilder;  // ERROR: a library class is closed to inheritance unless its section opens it
sender.Send(move err);   // err: owned MyError, `sealed class MyError: Exception` — ERROR: not sendable (a compiler-known base)
private owned _tcs: TaskCompletionSource<Pair>;          // record Pair(Item: Item) — ERROR: no Task<T> over a value type that holds borrows
using var t := Domain.Spawn<Counter>(Bump, c);           // ERROR: a class argument crosses only through the owned row
private owned readonly _ev: AsyncEventSource<ReadOnlySpan<byte>> := new();   // ERROR: no async event payload that holds borrows — the raise frame holds it until settlement
private static owned readonly _conn: OwningTask<Conn> := ConnectAsync();   // ERROR: no static of a single-award task, `readonly` or not
using var tok := await t;   using var tok2 := await t;   // ERROR over any `t` of `OwningTask<T>` — a `using var`, a plain parameter, an owned local: the first award spends the task, and a second the first dominates is rejected (§30.4)
```

> - §30.3: blocking waits are a compile error lexically inside `async` bodies.
> - §30.14: a `Domain.Spawn` entry is a static method reference or a non-capturing lambda, and data enters only through the one explicit sendable argument.
> - §22.12: a mutable static is an error outside four admitted forms (a synchronized handle; the `Task?`/`Task<T>?` lazy cache; a warned primitive/enum value static; the owning-inline-array slot pool).
> - §12.4: only `Encoding`, `Stream`, `TextReader`/`TextWriter` and the exception family admit user subclasses; every other library class is closed, `sealed` written or not.
> - §30.14: a class deriving from one of those is never sendable; `StringBuilder`, `Random`, `Stopwatch`, `Type` and `OwningBuffer<double>` are sendable; a comparer-built hash collection must not cross a domain (trust, not a check).
> - stdlib §6.10: a `TaskScope` is minted only as the initializer of an `await using var` binding at its own type and is owned by that binding alone — never an `owned` field, parameter or return slot, an owning element, a type argument, or an `owned Object`/interface conversion; pass it as a plain (borrowed) parameter typed `TaskScope`, `Object` or an interface (not a generic `T`), which is unrestricted.
> - §28.3: a static holds only what is always live — a fresh value, `null`, or a bare read of a static field or `T.P`; a borrow of a parameter, of `this` or of a local stored into a static is rejected at the store.
> - §30.5: an `async` body under `Task<T>` at a class or interface T returns an owned value, `null` or `return await`, never a borrow.
> - §30.5: `Task<T>` over a value type that holds borrows is rejected wherever the type is formed — a `TaskCompletionSource<Pair>` field, `Task.FromResult(pair)` — `CancellationToken` excepted.
> - stdlib §6.5: `AsyncEventSource<T>` admits no such T either — `RaiseAsync` holds the payload in its frame until the task settles — `CancellationToken` excepted; raise a class payload (the buffer object, or a class holding it with an offset and a length) or borrow-free data.
> - stdlib §6.8: `Domain.Spawn`'s plain argument and result rows admit no class but `string` and `Type`; a class crosses through the `owned` row, by `move`.
> - §30.4 / §30.5: a Pattern B task is awarded once. Through a plain owned local the second await is a moved-from read; through a `using var`, a plain parameter or any other local it is the same compile error wherever the first award dominates the second; through a field or a slot, or on a path the first award does not dominate (a loop's second iteration, the point after a `try` whose block awarded), it raises `InvalidOperationException`. Bind a fresh task for a second acquisition.
> - §22.12 note (j): no static field or auto-property is of type `OwningTask<T>` — process-global storage of a single-award task is awarded once, to whichever domain's await reaches it first; hold a one-shot acquisition in a local or an owned instance field, and share a process-lived result through the `Task<T>?` cache, which every awaiter borrows.

```bezel
await using var scope := new TaskScope();
private static owned readonly _ready: Atomic<bool> := new Atomic<bool>(false);
private static _cfg: Task<Config>? := null;               // the admitted Task-cache form
Gate._m := new Mutex();                                   // or `move m` from an owned binding, or `Other._shared`
public property Item: Item => _item;                     // the stored object lends synchronously; a Task<Item> body returns an owned value or `return await`
done.SetResult(item.Count);                               // done: TaskCompletionSource<int> — publish borrow-free data (or OwningTaskCompletionSource<Item> for the object)
using var t := Domain.Spawn(Bump, move c);                // Bump(owned c: Counter): the owned row moves the object across
private owned readonly _ev: AsyncEventSource<Chunk> := new();               // Chunk: a CLASS holding the buffer with an offset and a length — or raise borrow-free data
using var g := await mutex.AcquireAsync();
using var tok := await t;   using var t2 := sem.AcquireAsync();   using var tok2 := await t2;   // one task, one award
```

```bezel
var s := arr.AsSpan();
var f := async lambda() => do
  await Task.Delay(10);
  Console.WriteLine(s[0]);          // ERROR: a captured span used after the lambda's own await
end;
await f();
```

> §30.1: a span, a borrow-bearing record or inline array, or a choice that holds borrows captured by an `async` lambda is a local of that lambda's body, and its sources are the enclosing scope's, never frame-resident — so any use of it after one of the body's own `await`s is rejected (a `CancellationToken` is the one exception). Read it before the first `await`, or copy what the body needs — `var first := s[0];` — before suspending.

```bezel
public static async method First(xs: Array<int>): Task<int>
  var s := xs.AsSpan();
  await Task.Delay(10);              // ERROR: `s` is live across the `await`, and its source `xs` is a plain parameter
  return s[0];
end First;
```

> §30.1 / §29.4 / §9.7: a span local crosses a suspension point — an `await`, or a `yield return` in any iterator — only where every borrow it carries has a frame-resident source: an owned local, a `using var` or an `owned` parameter of the same body. The same holds for a capture of one in an `async` lambda; an `async` parameter, result or element type of a span type is rejected outright. Index it before the suspension (`var first := s[0];`), or re-derive the view after it.

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

```bezel
Console.WriteLine(Half(-4));     // Half(n: int) requires(n > 0) — WRONG: the written argument folds the clause to `false`, a COMPILE error at the call (§22.16), not a run-time ContractViolationException
```

> §22.16: a `requires` clause is discharged at every call site by substituting the written arguments; a constant argument that folds the clause `false` is the prover floor's statically violated contract, and one that folds it `true` discharges the call. A non-constant argument keeps the always-on run-time check.

```bezel
Console.WriteLine(Half(n));      // a run-time value: the clause is checked at the call, in every build mode
```

### 7.8 Nullability narrowing

```bezel
if _field <> null then
  Use(_field);                     // ERROR: mutable fields and properties NEVER narrow
end;
```

> §7.4: narrowing applies to **locals, parameters other than `ref` parameters, and `readonly` fields only** — a mutable field can be rewritten between check and use by a call, a closure, or another continuation at an `await`. A local that is *mutably captured* by any lambda in the method is re-widened at the next call, function-value invocation, or `await`.

```bezel
var local := _field;
if local <> null then Use(local); end;
```

```bezel
public static method Greet(ref a: Animal?): string
  if a <> null then return a.Speak(); end;   // ERROR: a `ref` parameter never narrows
  return "none";
end Greet;
```

> §7.4: a `ref` parameter aliases caller storage the caller can still write during the call (a passed closure, a second `ref` argument, a field passed beside its holder). Copy to a local — `var c := a;` — and narrow the copy. `out` parameters are callee-local slots and narrow normally.

```bezel
var a: int? := ReadOpt();
var sum := a + b;                              // ERROR;  ✓ (a ?? 0) + (b ?? 0)  or  a! + b!
if a < b then ...                                // ERROR;  ✓ if a <> null and b <> null and a! < b! then
total += price;                                // ERROR when either is nullable
count++;                                       // ERROR when count is `int?`;  ✓ count := (count ?? 0) + 1;
if (flags & ADMIN) <> 0 then Grant(); end;     // ERROR when flags is `uint?` — lifted, it would FAIL OPEN
case n of  1..5 => "small", _ => "other" end;  // ERROR when n is `int?`;  ✓ case n ?? 0 of ...
if s is string? then ... end;                  // not an error, but the SAME test as `s is string`: FALSE at null (§12.7) — the null test is `s is null`
```

> §7.11: operators do not lift over nullable value types. **Rejected**, on either side and in the mixed `T?`/`T` pairing: binary `+ - * / mod & | ^ << >>`; unary `+ - ~`; relational `< <= > >=`; every compound-assignment form; and `++`/`--`. **Retained**: `=`/`<>` (equality is total over an absent operand — `null = null` is `true`), `??`, `?.`, `?(args)`, `??=`. There is no three-valued logic: `and`/`or`/`not` take `bool` only. **Pattern position rides the same rule** — a relational or range pattern over a nullable scrutinee is *rejected*, not silently never-matching (§19.3, §16.6). A scrutinee narrowed non-null before the `case` (`var n: int? := 80;`, or inside `if n <> null then`) is not nullable there: ordering patterns are admitted, and the match needs no `null` arm to be exhaustive (§19.1). Narrowing a **local** restores the ordinary operators: `if a <> null then var s := a + 1; end;` (§7.4). A non-null initializer or assignment narrows too: `var m: int? := 0; m += 1;` is fine until `m` is reassigned from a nullable expression. For an `invariant` over a *mutable* nullable field, where narrowing is unavailable, use the short-circuit form (`and` short-circuits) or declare the field `readonly`:

```bezel
invariant(_port <> null and _port! >= 0 and _port! <= 65535);
```

```bezel
public constructor()
  _onReady := lambda() => do _ready := true; end;   // ERROR: a lambda body is outside the `readonly` assignment window, wherever the lambda is written (§22.8)
end constructor;
```

> §22.8: a `readonly` field is assigned in a field initializer or in the constructor's own body (for `static readonly`, its static initializer or `static constructor()`), never inside a lambda, which can run after construction. Assign in the constructor; let the lambda read.

```bezel
public method Reset()
  this.Cells[0] := 0;   // ERROR: an element store lands in the `readonly` field's own storage — outside the constructor it is rejected like `Cells := …` (§22.8)
end Reset;
```

> §22.8: a member store into a `readonly` field — an inline-array element, a settable member of a record-typed field, a `ref`/`out` binding of one — is admitted only where the whole-field store is: the constructor's own body through `this` (for a `static readonly` field, the static constructor's), outside any lambda. It does not count toward the single-assignment rule, so a constructor may fill its own `readonly` array element by element.

```bezel
public static method Run(s: string?): int
  if s <> null then
    var f := lambda(): int => s.Length;   // ERROR: a parameter's or `var` local's narrowing does not enter a lambda body (§7.4)
    return f();
  end;
  const t := s;
  if t <> null then
    var g := lambda(): int => t.Length;   // fine: a `const` local's fact rides into the lambda
    return g();
  end;
  return 0;
end Run;
```

> §7.4: a lambda body reads its captured variables at their declared types, except a `const` local's fact and a `readonly`-field fact rooted at a `const` local, `this` or a type name; copy to a `const` first, or re-test inside the lambda.

```bezel
var s: string? := null;
try
  s := Fetch();                        // Fetch(): string — `s` is narrowed here
  Log("fetched");
finally
  Console.WriteLine(s.Length);         // ERROR: no fact the protected block establishes reaches a `catch` arm or the `finally` (§7.4)
end;
```

> §7.4: every statement of a protected block is a possible throw point for every arm and for the `finally`, so an arm or a `finally` starts from the facts at entry to the block, less anything the block invalidated anywhere. Re-test inside the arm or `finally`. The point *after* a `try` with no arm is reached only through the block's completion, so there `s` keeps the block's final fact:

```bezel
var s: string? := null;
try
  s := Fetch();
  Log("fetched");
finally
  if s <> null then Console.WriteLine(s.Length); end;
end;
var n := s.Length;                     // fine: no arm, so only the block's normal completion reaches this point
```

### 7.9 Small standard-library mistakes

```bezel
dict.Set(key, value);              // ERROR: no `Set` method on ANY collection
dict[missing] += 1;                // throws KeyNotFoundException: the indexer updates, never inserts
var l := new List<int>();          // no `List<T>` — the name is reserved
arr.Count(pred);                   // does NOT reach the predicate count on an Array<T> receiver
var s := "n = " + count;           // ERROR: no implicit ToString in `+`
pool.Sorted();                     // OwningArray<T> declares no Sorted/Reversed copy-forms
var sorter: () -> () := widgets.Sort;   // ERROR at the formation when `Widget` has no `IComparable<Widget>`: a reference over a conditional member is checked where it is formed, not where it is called
var text := String.Format("{0}", x);   // no String.Format
if a < b then ...                    // ERROR when either is `int?` (section 7.8)
var cbs := new Set<(int) -> int>();   // ERROR: a function type is not hashable (§3.6) — a lambda equals only itself (§27.7.1); a `Set<Object>` may hold a capture-free function value, whose hash agrees with its equality
public static method Keep<T>(x: T): int where T: Object => 1;   // ERROR: `Object` is not a bound (§14.2) — unconstrained `T`, or `where T: notnull`
```

```bezel
dict[key] := value;                // add-or-update; dict.Add(k, v) throws on a present key
tally.Increment(word);             // Counter<K>
var l := new Array<int>();
arr.Where(pred).Count();
var s := $"n = {count}";
pool.Reverse();                    // in place; or Drain() into a new owning collection
var text := $"{x}";
var cbs := new Array<(int) -> int>();  var hit := cbs.Contains(Demo.Twice);   // §27.7.1 equality: a static reference equals every reference to the same method
public static method Keep<T>(x: T): int where T: notnull => 1;
```

```bezel
var label := ParsePort(raw).UnwrapOr(BuildExpensiveMessage(raw));   // runs on the Ok path too
```

> `UnwrapOr(fallback: T)` declares a plain by-value parameter, and §22.5's argument-evaluation order is normative and observable: the argument is evaluated **at the call site, before the call**, on the `Ok` path exactly as on the `Error` path. `Map`, `MapError` and `AndThen` are the same. **There is no lazy `UnwrapOrElse`** — a recorded decline. Contrast `??`, which *is* conditional (§7.7); that contrast is exactly what misleads.

```bezel
case ParsePort(raw) of
  Ok(var p)    => Use(p),
  Error(var m) => Use(BuildExpensiveMessage(m))     // computed only on failure
end;
var port := ParsePort(raw).UnwrapOr(8080);          // a constant fallback is fine eagerly
```

---

## 8. Spellings that changed meaning

Text that teaches a left-column spelling predates the change — older guides, posts, model memory. The changelog is the migration record.

| Earlier spelling | Now | Cite |
|---|---|---|
| `(when c => a, _ => b)`, `when … end when;` | retired: `(if … then … elif … else …)`, `if … elif … end;`, `case` | §20 |
| `out owned var x` | `out var x` adopts what `out owned` announces; `out using var x` scope-binds it | §22.3 |
| `int? + int?` and the other lifted operators | a compile error: narrow a local, or `??` | §7.11 |
| `owned` written at a use site | `move` at use sites; `owned` is declaration-side only | §22.3 |
| bare `move obj.f` / `move coll[i]` at a non-nullable owned location (it left the location empty until restored) | `move obj.f with e` replaces in one step; the bare form only at a nullable location, which it sets `null` | §28.8.1 |
| a destructor taking a non-nullable owned field with a bare `move` | the same rule as any body: `_f.Dispose();` releases it, `move _f with e` exchanges | §28.8.1 |
| `IReadOnlySpan<T>` | gone: two overloads, `(ReadOnlySpan<T>)` and `(ReadOnlyArraySpan<T>)` | stdlib §4.16 |
| `override Equals(other: Object?)` on a record | the own-typed `Equals(other: R)` is the `Object` slot, as on enums and choices | §9.2 |
| an overload pair differing only by `owned` (or `scoped`) | rejected at the second declaration: distinct names (`Hold`/`Adopt`) | §22.6 |
| `move` in a named slot pattern, `Ready(move conn = var c)` | a named slot never transfers: destructure positionally | §19.3 |
| the `assumed` contract tier; `static invariant` | two tiers, proven and checked; `invariant` takes no `static` | §22.16 |
| a warning on using an owning-collection element borrow after its removal | no diagnostic: the use faults in checked mode — take the element out first (`PopAt`) | §28.11 |

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
