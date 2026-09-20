// Ambient bindings for conformance example fragments — the value-level
// counterpart of preamble.bzl.
//
// preamble.bzl supplies illustrative helper TYPES the surrounding prose assumes
// (Connection, Point, BankAccount, …). This file supplies the illustrative
// VALUES and MEMBERS a fragment assumes: `case score of 90..100 => "A"` is a
// complete claim about §19's `case` form, but `score` is a pronoun the prose
// supplies in English and the block does not declare.
//
// Format. An entry is introduced by
//
//     //@ <local|member|type> <name>
//
// and runs to the next header. `local` is spliced at the top of the synthesized
// `Main` body, `member` into the synthesized `class W`, `type` beside the
// module's other top-level declarations. A case opts in by name in its manifest
// `notes` field: `env:score` (several: `env:score,grade`).
//
// THE RULE FOR ADDING AN ENTRY. Its type must be forced by the fragment's own
// text, or stated in the prose immediately around it — and the derivation goes
// in the comment, so a reviewer can check it. Do NOT pick a spelling by trying
// them until the compiler stops complaining. That is circular, and on this
// compiler it does not even measure conformance: an auto-solver run that sized
// this bucket "solved" cases by binding called names to `int` (#3153: a call on
// a non-callable value compiles and is silently dropped) and by binding `?.`
// receivers to `int` (#3155: `?.` never resolves its member). A binding that
// only works because of a hole proves nothing about the document's claim.
//
// If a fragment's assumed name has no forced type, leave the case skipped. A
// row that cannot fail is worse than no row.

//@ local score
// spec §19.1 Forms, §19.7 Termination, §20 The `if` Expression, §21.2 The
// `if` statement (v0.254) — the grade-band examples. Integral and ordered: the fragments
// match it against the range patterns `90..100`, `80..89`, `70..<80` and compare
// it with `score >= 90`.
var score: int := 85;

//@ local grade
// spec §21.2 The `if` statement (v0.254) — the `if`/`elif` form assigns the string arms "A".."F" to it.
var grade: string := "";

//@ local delta
// spec §19.1 Forms (v0.254) — the negative-range-bound example matches it against `-10..-6`,
// `..<0`, and `0` — a signed integral.
var delta: int := -3;

//@ local temperature
// spec §19.4 Guards (v0.254) — the guarded-arm example binds it with `var t` and guards `t > 30` /
// `t < 0`, then interpolates it.
var temperature: int := 21;

//@ local i
// spec §21.3 The `while` statement (v0.254): `while i < 10 do … i := i + 1; end;` — a mutable
// integral counter.
var i: int := 0;

//@ local month
// spec §19.7 Termination (v0.254) — the statement-position `case` matches it against the literal arm `1`.
var month: int := 1;

//@ local amount
// spec §21.2 The `if` statement (v0.254) — the example guards `amount > 0` and adds it to `Balance`.
var amount: decimal := 10m;

//@ local Balance
// Same fragment: `Balance := Balance + amount;` — assigned, and summed with a
// `decimal`. (The prose means a property; an ambient mutable of the same type is
// what the fragment's own statements require of it.)
var Balance: decimal := 0m;

//@ local user
// spec §18.7 Interpolated verbatim strings (v0.254) — the example puts it in a Windows user
// path, `$@"C:\Users\{user}\Documents\report.pdf"` — a name, i.e. a string.
var user: string := "alice";

//@ local maybe
// Guide 2 §1 Non-nullable by default (v0.250): `maybe ?? "Anonymous"` needs a nullable left
// operand, `maybe!` asserts non-null, `if maybe <> null then … maybe.Length`
// narrows it and reads a `string` member.
var maybe: string? := "text";

//@ local numbers
// spec §27.4 Parameter type inference and §27.6's method-reference example
// (v0.254): `numbers.Select(lambda(x) => x * 2)`,
// `.Where(lambda(x) => x > 0)`, `.OrderBy(Math.Abs)` — a sequence of a signed
// arithmetic element type.
var numbers: Array<int> := [3, -1, 2];

//@ local source
// spec §29.2 Desugaring of `for` (v0.254) — the examples enumerate it (`for var x in source`,
// and the spelled-out `GetEnumerator`/`MoveNext`/`Current` expansion).
var source: Array<int> := [1, 2, 3];

//@ member _name
// spec §13.6 Accessor visibility (v0.254) — the example is `public property Name: string
// get => _name; private set => _name := value;` — the backing field's type is
// fixed by the property's declared type.
  private _name: string := "";

//@ member _balance
// spec §23.3 Block-form properties (v0.254) — the validating-setter example is `public property Balance: decimal
// get => _balance; set => … _balance := value;` — likewise fixed by the
// property's declared type, and the setter compares against the `decimal`
// literal `0m`.
  private _balance: decimal := 0m;

// ---------------------------------------------------------------------------
// Entry KEY vs. bound NAME (2026-09-21).
//
// The `//@ <kind> <key>` key is what a manifest row opts into; the binding the
// entry introduces is whatever its body declares. They coincide for every entry
// above and for most below, but they need not: two different fragments can
// assume the same pronoun at two different types — §18.7 interpolates a `user`
// that is a name (a `string`), while §7.5's null-conditional examples read
// `user?.Name`, which forces a nullable RECORD. One global entry cannot serve
// both, so the second is keyed `userRecord` and still binds `user`. A row that
// opts into two entries declaring the same name gets a redeclaration error and
// FAILS — loudly, like every other env mistake.
// ---------------------------------------------------------------------------

//@ member DoWork
// spec §31.2 `try`/`catch`/`finally`, §31.2.2 Arm bodies, §31.2.3; Guide 2 §6 —
// every call site is the bare statement `DoWork();`: no arguments, result
// discarded. A parameterless void helper is exactly what the text forces.
  private static method DoWork() end DoWork;

//@ member Log
// spec §31.2.2 Arm bodies (`Log(ex);` inside a catch-all arm whose binding is
// `var ex: Exception`), §31.2.3 (same shape); Guide 2 §4 (`Log($"bad input:
// {e.Message}")`, `Log("done")`), Guide 2 §5 (`Log($"{ex}\n{ex.StackTrace}")`).
// Two argument types appear in the documents and both are forced by their call
// sites — a string and a caught `Exception` — so the ambient carries both.
  private static method Log(message: string) end Log;
  private static method Log(ex: Exception) end Log;

//@ member Retry
// spec §31.2.2 Arm bodies — `var ex: IOException | TimeoutException => Retry(ex)`.
// A multi-type arm binds at the common base (§31.2.2), so the argument is an
// `Exception`.
  private static method Retry(ex: Exception) end Retry;

//@ member Process
// spec §15.6 Slicing (`Process(arr[i])`), §16.5 Iteration and §21.4 the `for`
// statement (`Process(i)` over an `int` range, `Process(t)` over a collection
// of transactions), §30.7 Async iterators (`Process(line)` over strings). The
// call sites fix only that it consumes one element of whatever is being
// iterated and discards the result, so the ambient is generic in that element.
  private static method Process<T>(item: T) end Process;

//@ member Connect
// spec §15.11 Owning inline arrays (`var fixed: [3 owned]Connection :=
// [Connect("primary"), …]` — an `owned` slot initializer), §28 Owning
// collections and Guide 5 §1/§6 (`pool.Add(Connect("a"))` into an
// `OwningArray<Connection>`, where the prose says "Connect returns owned —
// transfers in"). Result type and the single `string` argument are both forced.
  private static method Connect(name: string): owned Connection
    return new Connection(name);
  end Connect;

//@ local pool
// spec §28 Owning collections (the `UseEach` worked example), §29.12 Mutation
// during iteration, Guide 1 §9, Guide 5 §1/§3 — the fragments call
// `pool.Drain()` and adopt each element with `move`, `pool.Add(move item)`,
// read `pool[i]` as a borrow and write `pool[i] := fresh`. An owning collection
// of a class type; `Connection` is the element type the surrounding prose names
// in every one of these blocks (preamble.bzl supplies the stub).
using var pool := new OwningArray<Connection>();

//@ local candidate
// stdlib §4.10 `Set<T>`/`OwningSet<T>` — `pool.Add(move candidate)` into an
// `OwningSet`/`OwningArray` of `Connection`: the call-site `move` marker makes
// it a named OWNED binding (§22.3), and the element type fixes it.
using var candidate := Connect("candidate");

//@ local path
// spec §31.2 (`new FileStream(path, FileMode.Open)`), §30.7 (`ReadLinesAsync(path)`),
// Guide 4 §2 (`File.ReadAllTextAsync(path)`) — a filesystem path, i.e. a string.
var path: string := "input.txt";

//@ local url
// spec §28.12 Asynchronous disposal, §35 the `client.Fetch(url)` examples — an
// endpoint the fragments pass where a string is taken.
var url: string := "https://example.invalid/resource";

//@ local n
// spec §16.5 Iteration and §21.4 the `for` statement — `for var i in 0..<n`
// and `0..<n step 2`: the exclusive bound of an integer range.
var n: int := 4;

//@ local id
// Guide 1 §10 ScopeGuard — `Register(id)` / `Unregister(id)`, the same value
// passed to both; the registry key is an integer handle in the surrounding text.
var id: int := 7;

//@ member Register
// Guide 1 §10 — `Register(id);` as a bare statement; its argument is the `id`
// local above and its result is discarded.
  private static method Register(id: int) end Register;

//@ member Unregister
// Guide 1 §10 — `Unregister(id);` in the `finally` arm and in the ScopeGuard
// lambda; same argument, result discarded.
  private static method Unregister(id: int) end Unregister;

//@ local dict
// spec §21.4 the `for` statement — `for var entry in dict do … entry.Key …
// entry.Value`; the codegen reference's §7.9 `dict[key] := value` add-or-update
// line takes the same shape. A dictionary keyed by the `key` string below.
using var dict := new Dictionary<string, int>();

//@ local key
// Same fragments — the subscript in `dict[key] := value`, so the dictionary's
// key type.
var key: string := "k";

//@ local value
// Same fragment — the right-hand side of `dict[key] := value`, so the
// dictionary's value type.
var value: int := 1;

//@ local arr
// spec §15.6 Slicing — `Process(arr[i])` over an integer range index; the
// codegen reference's `arr.Where(pred).Count()` line reads it as a sequence.
using var arr := new Array<int>();

//@ local transactions
// spec §29.12 Mutation during iteration — `for var t in transactions do if
// t.IsExpired then toRemove.Add(t); end; end;` then `transactions.Remove(t)`:
// a borrowing collection of the `Transaction` stub, read and mutated.
using var transactions := new Array<Transaction>();

//@ local userRecord
// spec §7.7 Null-coalescing, Guide 1 §8 — `user?.Name ?? "Anonymous"` typed
// `string`, so `Name` is a `string` and `user` is a NULLABLE record with a
// `Name` member: preamble.bzl's `User`. (Keyed `userRecord` because the §18.7
// entry above already binds a `user` of a different type — see the note on keys.)
var user: User? := new User("alice", 1);

//@ local a
// spec §20 The `if` Expression, Guide 3 §2, codegen reference §3.5 — `var max :=
// (if a > b then a else b);`. The text forces an ORDERED type shared by both
// operands and by the atom's two arms; it says nothing finer, so `int` stands in
// for that type. What the row checks is the `(if …)` atom's form, not `a`'s type
// (cf. the §18.7 `user` row, which is ambient-typed for the same reason).
var a: int := 3;

//@ local b
// Same fragments — the other operand of `a > b`, hence the same type as `a`.
var b: int := 5;

//@ local counts
// spec §6.6 Compound assignments and §17.6's indexer desugaring
// (`counts[category] += 1`), stdlib §4.8 `Dictionary<K,V>`
// (`if not counts.ContainsKey(w) then counts[w] := 0; end; counts[w] += 1;`).
// The subscript is the `category` / `w` string below and the slot is assigned
// `0` and incremented by `1`, so the value type is integral.
using var counts := new Dictionary<string, int>();

//@ local category
// Same fragments — the index expression in `counts[category] += 1`, so the
// dictionary's key type.
var category: string := "a";

//@ local w
// stdlib §4.8 `Dictionary<K,V>` — the word-count example's key, subscripted into
// `counts` above.
var w: string := "word";

//@ local animal
// spec §12.7 Type tests (`is`, `is not`, `as`) — `var dog: Dog? := animal as Dog;`
// then `dog.Name`. `as` to `Dog` requires a source type `Dog` is reachable from;
// preamble.bzl declares `Dog: Animal`, so the base is what the block assumes.
var animal: Animal := new Dog();

//@ member Width
// spec §22.2 Expression-bodied methods and §23.2 Expression-bodied read-only
// properties — `public method Area(): decimal => Width * Height;` /
// `public property Area: decimal => Width * Height;`. The declared result type
// of the product is `decimal`, which fixes both factors.
  private Width: decimal := 2m;

//@ member Height
// Same fragments — the other factor of the `decimal`-typed product.
  private Height: decimal := 3m;

//@ member _count
// spec §23.2 — `public property IsEmpty: bool => _count = 0;`: compared with the
// integer literal `0`.
  private _count: int := 0;

//@ member FirstName
// spec §23.2 — `public property FullName: string => $"{FirstName} {LastName}";`.
// §18.7 interpolates a value of ANY type, so the parts are not type-forced; what
// the row checks is the expression-bodied property form. `string` is the
// representative binding, recorded here rather than left implicit.
  private FirstName: string := "Ada";

//@ member LastName
// Same fragment, same reasoning as FirstName.
  private LastName: string := "Lovelace";

//@ local mutex
// stdlib §6.2 `Mutex` and `Lock` — `using var guard := mutex.Acquire();` and
// `mutex.TryAcquire(TimeSpan.FromSeconds(5))`: exactly the documented `Mutex`
// surface, so the ambient is a `Mutex`.
using var mutex := new Mutex();

//@ member LoadConfigAsync
// spec §22.13 Static initialization order and Guide 4 §8 — the blocks assign it
// into `private static _cfg: Task<Config>?` and the §22.13 sibling block states
// the signature in its own comment: `// LoadConfigAsync(): Task<Config>`.
// `Config` is preamble.bzl's stub.
  private static async method LoadConfigAsync(): Task<Config>
    return new Config();
  end LoadConfigAsync;

//@ member Run
// spec §22.13 — the same block's comment states `Run(cfg: Config): int`, and the
// call is `return Run(await AppConfig.GetConfig());` from a `Main(): Task<int>`.
  private static method Run(cfg: Config): int => 0;

//@ type FetchOutcome
// spec §35 Choices — the chapter's own opening declaration, quoted verbatim
// (minus the illustrative `IsSuccess` member, which no opting-in row calls), so
// the §35.5 matching fragments below are checked against the document's own
// type rather than an invented one. `Response` is the owned class payload the
// declaration names; the members are the ones §35.5's blocks read off it
// (`r.IsStale()` under a `when` guard — a bool; `body.Length` interpolated after
// "got … bytes" — a count).
public class Response
  public constructor() end constructor;
  public property Length: int => 0;
  public method IsStale(): bool => false;
  destructor end destructor;
end Response;

public choice FetchOutcome
  Fetched(owned response: Response),      // owned class payload
  NotModified,                            // payload-free case
  Failed(status: int, message: string);   // value payloads
end FetchOutcome;

//@ type Client
// spec §35.5 — `case client.Fetch(url) of Fetched(_) … ` and
// `var outcome := client.Fetch(url);`: a receiver with a `Fetch` taking the
// `url` string and producing the §35 `FetchOutcome` the arms match on.
public class Client
  public constructor() end constructor;
  public method Fetch(url: string): FetchOutcome => FetchOutcome.NotModified;
  destructor end destructor;
end Client;

//@ local client
// spec §35.5 — the receiver of `client.Fetch(url)`.
using var client := new Client();

//@ local outcome
// spec §35.5 the owning match — `case move outcome of Fetched(var r) …`: an
// owned move-only local of the §35 choice type, which is what `move` on the
// scrutinee requires.
var outcome := FetchOutcome.NotModified;

//@ member Render
// spec §35.5 — `Render(resp)` where `resp` is `using var resp := move r;` and
// `r` is the `Fetched` case's `owned response: Response` payload, so the
// argument is a borrow of a `Response`.
  private static method Render(resp: Response) end Render;

//@ member RenderCached
// spec §35.5 — `NotModified => RenderCached()`: no arguments, arm-body position,
// result unused (the sibling arms are statements).
  private static method RenderCached() end RenderCached;

//@ member ShowError
// spec §35.5 — `Failed(var s, var m) => ShowError(s, m)`: the two `Failed`
// payload fields, `status: int` and `message: string`.
  private static method ShowError(status: int, message: string) end ShowError;

//@ member ScheduleRetry
// spec §35.5 the borrowing match — `Failed(var s, _) when s >= 500 =>
// ScheduleRetry()`: no arguments.
  private static method ScheduleRetry() end ScheduleRetry;

//@ member RefetchLater
// spec §35.5 the owning match — `do _ := r; RefetchLater(); end`: no arguments.
  private static method RefetchLater() end RefetchLater;

//@ member Count
// spec §35.5 — `Fetched(_) => Count("hit")`, `_ => Count("other")`: one string
// label per arm, result unused.
  private static method Count(label: string) end Count;

//@ local age
// spec §20 The `if` Expression / Guide 3 §2 — `(if age >= 18 then "adult" else
// "minor")`: compared against the integer literal 18.
var age: int := 21;

//@ local s
// codegen reference §3.5 Expressions — `(if s >= 90 then "A" elif s >= 80 then
// "B" else "F")`: compared against integer literals.
var s: int := 95;

//@ member BeginBatch
// Guide 2 §6 — `try BeginBatch(); DoWork(); finally EndBatch(); end;`: a bare
// no-argument statement whose result is discarded.
  private static method BeginBatch() end BeginBatch;

//@ member EndBatch
// Guide 2 §6 — the `finally` arm's `EndBatch();`, same shape.
  private static method EndBatch() end EndBatch;

//@ member _field
// spec §7.4 / Guide 2 §1 / codegen reference §7.8 — `var local := _field; if
// local <> null then Use(local); end;`: compared with `null`, so NULLABLE. The
// payload type is not further forced by the block (what it checks is that
// narrowing applies to the copy, not to the field); `string?` is the
// representative binding, recorded rather than left implicit.
  private _field: string? := null;

//@ member Use
// Same fragments — `Use(local)` inside the narrowed region, so its parameter is
// the non-null form of `_field`'s type.
  private static method Use(x: string) end Use;

//@ member _items
// spec §23.4 Indexers (`public property [i: int]: string => _items[i];` — the
// element type is the indexer's declared result) and Guide 6 §3, whose own
// comment states the field: `// private owned _items: Array<string>;`.
  private owned _items: Array<string> := new Array<string>();

//@ member _values
// spec §23.4 Indexers — `public property [key: string]: int get => _values[key];
// set => … _values[key] := value;`: a dictionary from the indexer's key type to
// its declared result type.
  private owned _values: Dictionary<string, int> := new Dictionary<string, int>();

//@ member _grid
// spec §23.4 Indexers — `public property [row: int, col: int]: double =>
// _grid[row * _width + col];`: indexed by an `int` and yielding the declared
// `double`.
  private owned _grid: Array<double> := new Array<double>();

//@ member _width
// Same fragment — a term of the `int` index expression `row * _width + col`.
  private _width: int := 1;

//@ member BestEffortCleanup
// spec §31.2.5 Deliberate suppression — `try BestEffortCleanup(); catch
// Exception => () end;`: a bare no-argument statement.
  private static method BestEffortCleanup() end BestEffortCleanup;

//@ local p
// spec §10.6 Case matching — `case p of Low => …, Medium | High => …`. The arms
// are bare enum constants (§10.1's unqualified-in-scrutinee form), so the
// scrutinee is the enum declaring `Low`, `Medium` and `High`: preamble.bzl's
// `Priority`.
var p: Priority := Priority.Low;

//@ member GetUser
// spec §7.4 Member access on nullable values — `var user: User? := GetUser();`:
// the declared type of the binding fixes the result type exactly.
  private static method GetUser(): User? => null;

//@ member Acquire
// spec §17.4 the `is`/`is not`/`as` operators — `using var thing: Animal :=
// Acquire();` with the prose "owned": the declared type and the `using var`
// discharge fix result and ownership.
  private static method Acquire(): owned Animal
    return new Dog();
  end Acquire;

//@ local items
// spec §17.15 The binding atom — `(var n := items.Count; (if n = 1 then "one
// item" else $"{n} items"))`: a collection read for its `Count`.
using var items := new Array<int>();

//@ local expr
// spec §17.6 Increment/decrement on indexer slots — the desugaring's
// `var __coll := expr; __coll[__idx] := __coll[__idx] + 1;`: indexed by the `i`
// int and holding a value that `+ 1` applies to.
using var expr := new Array<int>();

// ---------------------------------------------------------------------------
// ANTI-VACUITY RECORD — 2026-09-21.
//
// Every entry above was proven NECESSARY mechanically: the row that opts into it
// is re-compiled with that entry removed and must fail. (The opt-in sets in
// manifest.tsv are minimal by construction — each was shrunk key-by-key until
// every remaining key was load-bearing.)
//
// A second, stronger pass rebinds each entry to a DELIBERATELY WRONG shape (a
// `bool` local / a `(bool): bool` method / an empty class) and re-compiles every
// opting-in row: the row must go red. 94 (row, entry) pairs were tested and
// **78 went red**. The 16 that did not are listed below — not because the
// binding is wrong, but because the COMPILER currently accepts the mistyped
// form. Each is a filed defect, and each is recorded here so no reader mistakes
// a surviving pair for a checked one:
//
//   #4500  binary arithmetic with a `bool` operand is accepted
//          (`decimal * bool` silently computes a wrong value)
//            spec_0135 <- Height      spec_0165 <- Height
//   #4501  `=` / `<>` on unrelated operand types reaches codegen
//          (`bool = 0` silently answers True)
//            spec_0165 <- _count
//   #4502  subscripting a non-indexable value compiles (the `[…]` analogue
//          of #3153)
//            spec_0012 <- counts      spec_0088 <- counts
//            spec_0089 <- expr        spec_0089 <- i
//            spec_0168 <- _grid       spec_0168 <- _items
//            spec_0168 <- _values     spec_0168 <- _width
//            guide5_0010 <- i         guide5_0010 <- pool
//   #4503  `x is T` between unrelated types is accepted and narrows
//            spec_0053 <- animal
//   (no issue — spec §18.7 admits ANY type in an interpolation, so the parts of
//    `$"{FirstName} {LastName}"` are not type-forced; the row checks the
//    expression-bodied-property form itself. Same deliberate exception the
//    §18.7 `user` row above already carries.)
//            spec_0165 <- FirstName   spec_0165 <- LastName
//
// None of these rows passes ONLY because of a hole: each compiles on the merits
// with its correct binding, and the document's own block is what is checked.
// What the surviving pair means is narrower — that the entry's TYPE is not
// independently pinned by the compiler today. When #4500-#4503 close, re-run the
// mutation pass and this list should empty.
// ---------------------------------------------------------------------------
