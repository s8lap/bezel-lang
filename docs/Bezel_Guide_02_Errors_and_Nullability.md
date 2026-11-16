# Bezel Guide 2: Errors and Nullability

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.359.*

Most of the syntax here is mainstream: non-nullable by default, `?` for nullable, `?.`, `??`, exceptions with `try`/`catch`. The *emphasis* is not. Bezel has one rule for how an operation reports what went wrong, and the rule asks one question: **how will the caller consume the outcome?** (The normative statement is the failure-signal rule, stdlib §1; this guide teaches it by example.)

- **The caller branches on an expected absence** — nothing under the key, text that may not parse: the **non-throwing form**, an `…OrNull` member returning `T?` or a `Try…(out x): bool` member (§2). It is built on nullability, which is why the guide starts there (§1).
- **The caller branches on an error it gathers or decodes**, and the error's payload is a value: a `Result<T, E>` or a domain choice (§3).
- **Everything else throws** — the **throwing form** — and is handled where the program can do something about it, usually a **boundary**: a request loop, a job runner, `Main` or a supervisor (§4). Code in between names the classes it can answer and lets the rest propagate. A class that implements `IFailure` says the outside world disagreed; it never says what to do.
- **A guarantee the caller must provide** is a contract: `requires(...)`. A broken one is a bug in the calling code, raised as `ContractViolationException` and fixed there, not handled (Guide 6 §2).
- A handful of fatal conditions simply terminate (§4).

So in Bezel application code the common cases are a value return and an exception you do not catch; `catch` lives where the program can act. Beyond the emphasis, two ownership rules will change ingrained habits: a caught exception is a *borrow* (§5), and cleanup is *not* error handling — it runs on every exit without a `try` (§6). Each is headed How Bezel does it. If you skim everything else, read those and the table in §7.

**Contents:** §1 nullability basics · §2 expected absence: the non-throwing form · §3 errors the caller branches on: `Result<T, E>` and domain choices · §4 the throwing form: handling exceptions · §5 a caught exception is a borrow · §6 cleanup is not error handling · §7 choosing the form.

---

## 1. Non-nullable by default (familiar)

A plain reference type cannot be null. `?` makes it nullable. Assigning `null` to a non-nullable target is a compile error, and you cannot dereference a nullable until you have proven it non-null.

```bezel
var name: string := "Alice";       // cannot be null
var maybe: string? := null;        // explicitly nullable
var n := name.Length;              // fine
var m := maybe.Length;             // ERROR: maybe might be null
```

The operators are the ones you expect:

```bezel
var len := maybe?.Length;                       // len: int?  — null if maybe is null
var display: string := maybe ?? "Anonymous";    // substitute a default
var forced := maybe!;                           // assert non-null (throws if wrong)
```

`?.` short-circuits to null; `??` supplies a fallback; `!` asserts non-null and raises `NullReferenceException` at runtime if you were wrong. `??=` assigns only if currently null.

Over a left operand that cannot be null, `??` and `??=` are not errors, but they do nothing, and the compiler says so. The right operand is still checked as if the left were nullable — it must convert to the left's type — and then a warning tells you it never runs (for `??=`, that the assignment never happens):

```bezel
var shown := name ?? "Anonymous";  // warning: name is non-nullable; "Anonymous" never runs
name ??= "Bob";                    // warning: name is non-nullable; never assigns
var count := 0;
count ??= "none";                  // ERROR: "none" does not convert to int
```

The *declared* type decides, not the flow: `maybe ?? "x"` stays silent even where a check has narrowed `maybe`, and an operand whose type is a type parameter counts as possibly null unless the parameter is constrained `notnull`, `value` or `class` (spec §7.7). Narrowing works by flow analysis on locals:

```bezel
if maybe <> null then
  var ok := maybe.Length;          // maybe is narrowed to non-null here
end;
```

Narrowing is not only for member access. An operator that a nullable operand would reject is admitted *through* the narrowing, and the store leaves the narrowing standing — a compound assignment writes back a non-nullable value:

```bezel
var n: int? := counts.GetOrNull(key);
if n <> null then
  n += 1;                          // admitted: the narrowed operand is non-nullable
  n++;                             // the same rule, read through the same desugaring
  Report(n);                       // n is still narrowed — the stores wrote non-null values
end;
```

`n += 1` is admitted exactly where `n := n + 1` is, and `n++` with it (spec §6.6, §17.6). Outside a narrowing both are compile errors, and the diagnostic names the three remedies — narrow first, or write `n := (n ?? 0) + 1;` or `n := n! + 1;` (spec §7.11).

One rule that bites people: narrowing applies to locals, non-`ref` parameters, and `readonly` fields only, never to mutable fields, properties, or `ref` parameters. A mutable field can be reassigned between the check and the use by things the compiler cannot see — a method call that writes it, a closure, or another continuation interleaving at an `await` — so the compiler will not narrow it; a `ref` parameter aliases a location the caller can still write the same way. Copy it to a local first:

```bezel
var local := _field;               // copy
if local <> null then
  Use(local);                      // narrowed; safe
end;
```

The exception is `readonly` fields. A nullable `readonly` field — instance or static — narrows like a local: nothing can rewrite it after construction, so no call, closure, or `await` can invalidate the fact. The tracked shape is one field directly off a tracked root (`this._f`, `TypeName._s`, or `v._f` off a tracked local or non-`ref` parameter `v`). The rule does not apply inside the declaring type's constructors and initializers, where the field is still being assigned (spec §7.4).

Locals can lose their narrowing too. A local that is **mutably captured** — assigned, or passed by `ref`/`out`, inside any lambda in the method — is re-widened at the next call, function-value invocation, or `await` in the controlled block, because a closure could run and rewrite it in between (spec §7.4). Copy such a value into a plain, uncaptured local and test *that* — the same remedy as for a mutable field.

---

## 2. Expected absence: the non-throwing form — `T?`, `*OrNull`, and `Try`

When the caller will branch on whether there is a value — a cache miss, a lookup with nothing under the key, a parse of input that may legitimately be malformed — return the absence as a value, not a throw. Two shapes cover it; §3 adds the third.

**Nullable return** for "maybe there is a value":

```bezel
method Find(key: string): User?
  // ... returns null if absent
end Find;

var u := directory.Find("alice");
if u <> null then
  Greet(u);                        // narrowed
else
  Greet(defaultUser);
end;
```

Or collapse it with the operators from §1: `directory.Find("alice")?.Name ?? "stranger"`.

The standard library ships this idiom as the **`*OrNull` form** (stdlib §1). Every per-type `Parse` — the stdlib §9 primitives and the `DateTime`/`DateOnly`/`TimeOnly`/`Guid` families — has a `ParseOrNull` returning `T?`; the dictionaries have `GetOrNull`. The composition is one line:

```bezel
var port     := int.ParseOrNull(raw) ?? 8080;
var greeting := config.GetOrNull("greeting") ?? "hello";
```

`ParseOrNull` returns null exactly where `Parse` would throw, so one `??` covers every way the parse can fail. (For `int`/`bool` environment variables specifically, `Environment.GetInt`/`GetBool` are the dedicated one-call spelling.) When you must *distinguish* absence from a present null — a dictionary whose value type is itself nullable, say — `T?` collapses the two (the conflation caveat, stdlib §1/§4.8). Use `ContainsKey` or the `Try` form below instead.

**When the library has no non-throwing form.** Offering the `OrNull`/`Try` form beside an operation with an expected absence is the library's job, and a missing one is a gap in the library, not a reason for a broader catch ([#8516](https://github.com/s8lap/bezel-lang/issues/8516) tracks them). Opening a file is one today: there is no `OpenOrNull` and no `ReadAllTextOrNull`. Do not test `File.Exists` and then open — the file can vanish between the two calls, so the open must handle its absence anyway. Open, and catch the one class that means "not there" at the call, where you know what its absence means:

```bezel
import System.IO;

method OpenSettingsOrNull(path: string): owned FileStream?
  try
    return new FileStream(path, FileMode.Open, FileAccess.Read);
  catch
    FileNotFoundException => return null;    // expected here: no settings file yet
  end;
end OpenSettingsOrNull;

method LoadSettings(path: string): Settings
  using var fs := OpenSettingsOrNull(path);
  if fs = null then
    return Settings.Defaults();              // the absence, answered as a value
  end;
  return Settings.Read(fs);                  // fs narrowed; disposed on every exit
end LoadSettings;
```

Write the helper once, name it as the library would, and the rest of the program composes with `??` and narrowing. The arm names `FileNotFoundException` and nothing else (stdlib §7.2): a missing directory or a refused permission is not "no settings yet", and it propagates. A whole-file read such as `File.ReadAllText` raises the same classes when it cannot open its file (stdlib §7.3), and takes the same arm.

**The `Try` form** when the failure path and success path differ, and especially when the value is *owned*. A `bool` return plus an `out owned` parameter, with a `when true` postcondition that lets the caller skip the `!`:

```bezel
private owned _cache: OwningDictionary<string, Connection>;

method TryPopCached(name: string, out owned conn: Connection? when true): bool
  if not _cache.ContainsKey(name) then
    conn := null;                  // every out path must assign (throw paths exempt)
    return false;
  end;
  conn := _cache.PopValue(name);   // takes ownership out of the cache
  return true;
end TryPopCached;
```

The method is named `TryPopCached`, not `TryGetCached`, on purpose. Where the two operations genuinely differ — `Get`/`Peek` leave the entry in place and borrow; `Pop`/`Drain` also remove it and transfer — the stdlib keeps distinct names, so the name alone tells you whether you now owe a disposal (spec §22.6's `out owned` rule; stdlib §1). A pair that differs *only* in ownership — on an ordinary parameter as on an `out` parameter — is not an overload pair either (spec §22.6): the owning form gets its own name. This one transfers: it hands the caller ownership through `out owned`. The parameter is typed `Connection?` (nullable) because the false path must assign *something*, and `default(Connection)` is illegal for a non-nullable reference type. The `when true` clause states the contract in the signature — "a `true` return implies `conn` is non-null" — and the caller's flow analysis picks it up:

```bezel
if pool.TryPopCached("primary", out using var c) then
  UseConnection(c);                // owned, and narrowed non-null by `when true`
end;                               // c disposed at the end of the enclosing block
```

`out using var c` binds the out value as an owned, scope-disposed local — so a successful call that handed you a `Connection` cannot leak. Its scope is the *enclosing* block, from the call onward — not just the `if` — so the connection is disposed when that block ends. Without the `when true` clause you would still write `UseConnection(c!)`: the bool guard alone does not narrow the nullable; only the postcondition does. `out using var` is accepted only against an `out owned` parameter — against a plain `out` the compiler rejects it, because there is no owned source. Plain `out var c` is accepted against either kind and *adopts* what the signature announces: against `out owned` it is an owned local you discharge yourself (a `move`, a store into an owned slot, or scope-end auto-drop with the must-use warning). This is the ownership-aware `TryGet`: the contract is "on `true`, you now own it," and `out using var` is the form that discharges that obligation for you.

---

## 3. Errors the caller branches on: `Result<T, E>` and domain choices

When the caller needs to know *which* error — at *which* position, of *which* sort — absence is not enough. **The `choice` return** is the tool when the error is expected, branched on at or near the call site, and its payload is a plain value (spec §35; stdlib §1). Where the nullable return says only "absent" and the `Try` form carries its result in out-parameters, a `choice` names every outcome and carries each one's data. And a `case` over it is proven exhaustive with no `_` arm (spec §19.6). In expression position, a `case` that misses an outcome draws the compiler's missing-arm warning, so adding an outcome flags every such consumer at compile time instead of silently falling through a null check; a statement-position `case`, like the one below, draws no warning and does nothing when no arm matches:

```bezel
public method ParsePort(text: string): Result<int, string>
  if not int.TryParse(text, out var value) then
    return Error($"not a number: {text}");    // bare case name: the declared
  end;                                        //  return type supplies the choice
  if value < 1 or value > 65535 then
    return Error($"out of range: {value}");
  end;
  return Ok(value);
end ParsePort;

case ParsePort(arg) of
  Ok(var p)    => Listen(p),
  Error(var m) => Console.WriteLine($"bad port: {m}")
end;                                  // exhaustive; no _ arm needed
```

The bare `Ok(value)` / `Error(msg)` spelling is **declared-result target-typing** (spec §35.3). In a `return` statement — or an expression body, or a lambda with a return-type ascription — whose declared result type is a choice, a bare case name constructs that choice's case; the declared type supplies the type arguments whole. The qualified `Result<int, string>.Ok(value)` remains valid everywhere. The bare form exists for exactly these positions, and if a case name shadows something in scope the compiler warns.

`Result<T, E>` (stdlib §2.20) is the stdlib's ready-made shape; a domain-specific choice with named cases (`Fetched`/`NotModified`/`Failed`) is often better still. When a case's payload is an owned resource, the `case` over a fresh call is an *owning match*: bind the payload and adopt it (`using var conn := move c;`) — see Guide 1 §9.

**Pipelining a `Result`.** For value-payload results, four compiler-provided members fold and chain outcomes without writing a `case` (stdlib §2.20): `UnwrapOr(fallback)` folds to a plain value, `Map(f)` transforms the success, `MapError(f)` re-codes the error at a boundary, and `AndThen(f)` sequences a second fallible step:

```bezel
var port := ParsePort(raw).UnwrapOr(8080);           // fold: the value, or the fallback

var label := ParsePort(raw)
    .Map(lambda(p) => $"listening on {p}")
    .UnwrapOr($"bad port: {raw}");

// Re-code the error where a subsystem boundary wants its own error shape:
public record ConfigError(Key: string, Message: string);

public method PortSetting(raw: string): Result<int, ConfigError>
  return ParsePort(raw).MapError(lambda(m) => new ConfigError("port", m));
end PortSetting;

// Sequence two fallible steps without matching in between:
public method WellKnownPort(text: string): Result<int, string>
  return ParsePort(text).AndThen(lambda(n): Result<int, string> => do
    if n > 1023 then return Error($"not a well-known port: {n}"); end;
    return Ok(n);
  end);
end WellKnownPort;
```

One trap the `??` of §1 does not prepare you for: a combinator's argument is an ordinary argument, evaluated at the call site before the call — on the success path exactly as on the error path (stdlib §2.20). `??` is conditional, so `a ?? Expensive()` never runs `Expensive()` when `a` is non-null (spec §7.7); `.UnwrapOr($"bad port: {raw}")` builds that string on the `Ok` path as well. There is no lazy `UnwrapOrElse`. Where the fallback must be computed only on error, compute it in the `Error` arm of a `case`.

Note the `AndThen` lambda's return-type ascription: it makes the lambda's own returns declared-result positions, so the bare `Ok`/`Error` spelling works inside it too. The combinators exist only at **value-like** instantiations — payloads of values and strings, the common case. A `Result` carrying an owned resource (`Result<Connection, string>`) has none of them, deliberately. Extraction there is consumption, and consumption is the owning match's job — for owning results, the exhaustive `case` *is* the pipeline. The three non-throwing idioms compose: `T?` for plain absence, the `Try` form for value extraction with a boring false path, the choice return where the error is branched on and its payload is a value.

---

## 4. The throwing form: handling exceptions

Everything the non-throwing forms don't cover throws: the condition the caller does not branch on, consumed where the program can do something about it, and the check a `requires` contract cannot state. Most code never catches. An exception propagates through it, and its owned values are cleaned up on the way (§6), until it reaches code that can act. The syntax is the familiar one: `catch` arms look like `case` arms (pattern + optional `when` guard + body), tried top to bottom:

```bezel
method ImportAll(paths: Array<string>)
  for var path in paths do
    try
      Import(path);
    catch
      var e: FormatException => Log($"{path}: bad data, skipped: {e.Message}"),
      var f: IFailure        => Log($"{path}: {f.Message}, skipped")
    end;
  end;
end ImportAll;
```

This loop is a boundary for one file: it can act — log the file and move on to the next — so it names the classes it can answer. A malformed file (`FormatException`) and what the file system did (the `IFailure` arm) are skipped. Everything else — a bug in `Import`, a cancellation — is not this loop's to answer, and it propagates exactly as if the `try` were not there.

**Name the types; let the rest propagate.** An unmatched exception keeps propagating (there is no exhaustiveness warning on `catch`; letting non-matching exceptions through is normal). So an arm that does nothing but re-raise — `Exception => throw` at the end of a list — does nothing at all: leave it out. The built-in hierarchy is what you would expect — `ArgumentException` (with `ArgumentNullException` under it), `InvalidOperationException`, `FormatException`, `OverflowException`, and so on, available without `import` (the full set is spec §31.4). Library producers declare their own exception types beside their APIs — `IOException` and `FileNotFoundException` with the file APIs, `KeyNotFoundException` with `Dictionary<K, V>`, and others — reached by the producer's `import` (none for an auto-imported module such as `System.Collections.Generic`, `KeyNotFoundException`'s home) and otherwise ordinary. Arm patterns match subclasses: a `catch` arm for `ArithmeticException` also catches `OverflowException` and `DivideByZeroException`. Order arms most-specific first.

**Never catch a bug to carry on.** An `InvalidOperationException`, an `ArgumentException`, a `NullReferenceException` from a `!` that was wrong: when your own code raised it, the program broke a guarantee it owed, and an arm that logs it and continues hides the bug and runs on in a state nobody planned. Where you expected the condition at the call, the fix is the non-throwing form (§2) or a test before the call; where you did not, let it reach the boundary that reports it.

**What `IFailure` means.** Some exceptions say where they came from. A class that implements the marker interface `IFailure` says the outside world disagreed — a missing file, a refused permission, a peer that closed its channel, a deadline. The `IOException` family, `TimeoutException`, `ChannelClosedException`, `ChannelFullException` and `DomainFailedException` implement it (spec §31.4, stdlib §1). Every other class makes no statement: a `FormatException` may be a user's typo or a bug in your own constant, a `KeyNotFoundException` a bad key from a request or a slip in your own bookkeeping. (`OperationCanceledException` is apart: it answers a cancellation you asked for, and an `IFailure` arm never takes it.)

Keep three things apart: *the class says who could have caused it; the call says whether you expected it; the handler says what happens next.* The marker is the first, and only the first. `FileNotFoundException` implements `IFailure` whether the user picked the file or your program cannot start without it; what you do about it is your handler's choice.

**The boundary.** That gives a boundary its arms. Where you would have reached for `Exception`, name what this boundary expects — the classes it can answer, and the marker for what the environment and its peers do:

```bezel
method Serve(request: Request): Response
  try
    return Dispatch(request);
  catch
    OperationCanceledException => throw,     // cancellation is never answered: let it through
    var e: FormatException => do             // bad client input: expected here
      return Response.BadRequest(e.Message);
    end,
    var f: IFailure => do                    // the environment or an upstream peer
      return Response.BadGateway(f.Message);
    end,
    var ex: Exception => do                  // a boundary only: the rest is reported
      ReportBug(ex);
      return Response.InternalError();
    end
  end;
end Serve;
```

The `FormatException` arm is this service's statement that malformed input from a client is expected here; nothing in the class says so, because the same class reports a malformed constant in your own code. The `IFailure` arm takes what the environment or an upstream peer did; `f` is an ordinary `Exception` inside the arm. The `Exception` arm is there only because this boundary is a request loop that must keep serving. Most boundaries have no such arm: anything else then propagates, and an uncaught exception ends the program with its report (spec §33.3). A boundary's catch-all would also take cancellation — a request to stop, answered as a bug — so the first arm re-raises it; that is the one arm whose only job is to re-raise, because a later arm would otherwise take the exception. One arm on bare `Exception` that answers everything the same way is the habit to drop: it answers a bug as if it were bad input.

The marker is a default, not a verdict. If the service cannot run without a file, an `IFailure` arm above a lazy open of it answers its absence as routine. Open such a file at start-up, where no arm is above it, or catch `FileNotFoundException` there and stop.

**Marking your own classes.** An exception class of your own makes no statement until you say otherwise:

```bezel
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
```

A class may list `IFailure` under any base except `OperationCanceledException` (spec §31.4). A hierarchy carried over from C# keeps its base: `class StaleOrder(message: string): InvalidOperationException(message), IFailure` is taken both by an `IFailure` arm and by an arm on `InvalidOperationException`. A class that derives from an `IFailure`-marked type (`IOException`) is marked already and needs no listing. Mark a class when its condition is the outside world's, or when your application wants its boundary's `IFailure` arm to take it — a `ConfigError` for a configuration file the operator wrote, say.

Two habits keep the marker honest. When you wrap, the wrapper's class is what the handler above will read: wrap an `IFailure`-marked exception in a marked class, and let an exception you cannot vouch for propagate as it is rather than wrapping it in a class that lists `IFailure`. And where bad text is expected, name what `int.Parse` raises — `FormatException` for a malformed numeral, `OverflowException` for an out-of-range one — or, better, take `ParseOrNull` and decide at the call (§2).

**Throwing.**

```bezel
throw new ArgumentException("Amount must be positive.", nameof(amount));
```

`throw;` with no operand re-raises the current exception (in a catch arm only), preserving the original stack trace. Where a correct caller always guarantees the condition, state it as a contract — `requires(amount > 0m)` — rather than throwing (Guide 6 §2); `ArgumentException` and `InvalidOperationException` are for the checks no contract can state, in interop and pre-contract code.

**Fatal conditions.** Two resource-exhaustion conditions are deliberately unrecoverable — no `catch` will ever handle them, and both terminate the process. A stack overflow raises a `StackOverflowException`: the type exists in the hierarchy, but it cannot be caught — no `catch` arm ever matches it. An out-of-memory allocation failure is not an exception at all: there is no `OutOfMemoryException`, nothing unwinds, the process simply aborts. Neither is something a `catch` could meaningfully handle — the "handler" would itself need the stack or heap that just ran out — so Bezel does not pretend otherwise.

**Across `await` and domains.** Nothing new to learn, and the same arms work. An exception in an `async` body faults its task and re-raises at the `await` (for `Task.WhenAll`, the first failing input's, in input order). A `TaskScope` cancels the other children when one faults and re-raises that one exception at `await scope.Join()` — never an aggregate. A domain join cannot carry the exception object across, so it raises `DomainFaultedException` with the original's message and stack-trace text, or `DomainFailedException`, which implements `IFailure`, when the original's class did — so an `IFailure` arm above a join keeps working. A child domain that was cancelled joins as a plain `DomainFaultedException`, not as cancellation (stdlib §6.8). Cancellation itself is never swallowed on the way: every boundary lets it through, as `Serve` does. Guide 4 §3–§5 and §9 teach the concurrency side.

---

## 5. How Bezel does it: a caught exception is a *borrow*, not yours to keep

This is the most important rule to internalize. The stash-it habit from garbage-collected languages — assign the caught exception to a field, capture it in a closure, return it — does not transfer. In Bezel a caught exception is a borrow whose lifetime is the catch arm body. The runtime owns the exception and disposes it when the arm completes. You may use `ex` and pass it to methods (they borrow it for the call), but you may not:

- assign it to an `owned` field,
- return it as `owned`,
- capture it in a closure that outlives the arm.

```bezel
private owned _lastError: Exception?;

method Handle()
  try
    Risky();
  catch
    var ex: Exception => _lastError := ex;   // ERROR: ex is a borrow;
  end;                                        //  cannot assign to an owned field
end Handle;
```

(A note on the shape: arm bodies in `catch` can be an expression, a single statement, or a `do … end` block. The assignment `_lastError := ex;` is a single statement, so it works directly as the arm body — its terminating `;` ends the body and the next arm follows. For multi-statement work the `do … end` form is used; for very compact arms a method call is common. All three forms are equivalent in semantics; pick the one that reads cleanest at the point of use.)

To preserve information about the exception, copy the data you need into owned values *inside the arm*. When the work is more than a line or two, extracting a named helper is usually clearer — the helper gives the action a name a reader can recognize at a glance and localizes any side effects it performs:

```bezel
private _lastErrorText: string?;

method Handle()
  try
    Risky();
  catch
    var ex: IFailure => RecordError(ex)   // ex is an Exception here; anything else propagates
  end;
end Handle;

private method RecordError(ex: Exception)
  _lastErrorText := ex.Message;            // string is fine to keep
  Log($"{ex}\n{ex.StackTrace}");           // the hole dispatches to Exception.ToString():
                                           //   qualified type name, message, both chains
end RecordError;
```

`ex.Message` is a `string` (runtime-managed, no ownership obligation), so copying it out is fine. The exception *instance* cannot escape. The single exception to the rule is the bare re-throw `throw;`, which hands the in-flight exception back to the runtime rather than capturing it. For a one-call summary of what you *do* copy out there is `ex.ToString()`: it renders the exception's type name, its `Message`, and both diagnostic chains (`CauseChain` and `SupersededChain`) into a single `string` you may keep — deliberately *without* the stack trace, which stays the separate `ex.StackTrace` the example logs above (spec §31.4).

The reasoning: an exception often carries no disposable resources, but the rule is uniform so the language never has to ask "does this exception own something?" at a `catch` boundary. Copy what you need; let the instance go.

The same borrow discipline reaches *inside* an exception you throw: don't stash a borrow of a local in an exception's field. The unwind disposes that local before the handler runs — in checked mode the handler's access to it faults deterministically as `DisposedAccessException`. Give exception classes owned or copied state instead; that is exactly why the built-in cause-chaining constructor (`new Exception(msg, cause)`) *snapshots* the cause rather than pointing at it.

---

## 6. How Bezel does it: cleanup is not error handling

Elsewhere you reach for `try`/`finally` (or a `using` block) to avoid leaking on the exception path. In Bezel cleanup runs on every exit by itself, so it is never what a `try` is for. An owned value live when an exception propagates is disposed automatically during unwinding — every owned local and `owned` parameter behaves as if wrapped in an implicit `try`/`finally`. `using var` stays the default; it declares *intent*, not exception-safety. What you stop writing is the `try`/`finally` *cleanup plumbing*.

```bezel
method Import(path: string)
  using var fs := new FileStream(path, FileMode.Open);
  using var parser := new Parser(fs);
  parser.ReadAll();                // if this throws, parser then fs are
end Import;                        // disposed automatically, innermost first
```

Cleanup that is an *effect* rather than a value — ending a batch, releasing an external lock, restoring global state — gets the same treatment by giving the effect an owner: a small class whose destructor performs it, bound with `using var`.

```bezel
public class BatchScope
  public constructor()
    Journal.BeginBatch();
  end constructor;

  destructor
    Journal.EndBatch();            // runs at disposal: on every exit, exception included
  end destructor;
end BatchScope;

method Load(rows: Array<Row>)
  using var batch := new BatchScope();
  for var r in rows do
    Store(r);                      // may throw: the batch still ends
  end;
end Load;
```

An undo that must run only when the method *fails* — unregister what you registered, roll back what you began — is `ScopeGuard`'s job (stdlib §2.21): arm it after the effect, `Dismiss()` it on success. Guide 1 §10 teaches it; the compiler does not implement it yet ([#4669](https://github.com/s8lap/bezel-lang/issues/4669)), and until it does the commit-flag `try`/`finally` shown there is the spelling that compiles. That flag, and the rare one-off cleanup that is neither a value nor worth a type, are what `finally` is left for. `finally` runs on every exit — normal completion, exception, `return` — and `return` inside it is a compile error.

Even a half-constructed object is safe: if a constructor throws after some `owned` fields are set, those fields are disposed before the exception propagates. The same holds for a half-built *expression*: if one argument throws while an earlier argument has already produced an owned temporary, that temporary is disposed during the unwind — an exception never leaks a half-built argument list. You do not write code for any of this.

One edge worth knowing: what if cleanup itself throws *while an exception is already propagating*? A throw escaping a `finally` (or a disposer that throws during unwinding) **supersedes** the in-flight exception — the new one propagates, the old one is disposed (spec §31.2/§28.4). The buried exception is not lost: before the drop, the runtime snapshots its type, message, and stack trace into the escaping exception's `SupersededChain` (spec §31.4) — the same three-string snapshot machinery as `CauseChain`, chronological, earliest lost first — and the uncaught-exception report prints it. So the exception you see at the top is always the *last* one thrown, and `SupersededChain` is where the *first* one — usually the real one — went. Read it like `CauseChain`: copy the strings out in the arm if you need them past it.

One diagnostic worth recognizing in this area: disposing a value and then using it. A *straight-line* dispose-then-use — through the variable, or through a borrow of its value — is a compile error (spec §28.2); a use after a *conditional* dispose — disposed on one branch, still live on another — is a compile error at the merge (the value is "maybe-consumed", spec §28.3), while a borrow of that value is left to the run time, where its use is a deterministic fault in the default, checked build. All three are the compiler catching a use-after-free before it can happen.

---

## 7. Choosing the form

| Situation | Use |
|---|---|
| The caller branches on a value that may legitimately be absent | the non-throwing form: `T?` (the stdlib's `*OrNull` members: `ParseOrNull`, `GetOrNull`), handled with `?.` / `??` / narrowing |
| Absent *and* the value would be owned | the `Try` form: `bool` + `out owned T? when true`; the caller uses `out using var` (and drops the `!`) |
| An expected absence the library has no non-throwing form for (opening a file that may not be there) | catch the one class that means "not there" at the call, in a small `…OrNull` helper — never an `Exists` test before the operation; the missing form is a library gap (#8516) (§2) |
| The caller branches on an error it gathers or decodes, at or near the call, and the error payload is a plain value | `choice` return (`Result<T, E>` or a domain choice, spec §35); consume with an exhaustive `case`, or — where both payloads are value-like — pipeline with the combinators (`Map`/`MapError`/`AndThen`/`UnwrapOr`, stdlib §2.20) |
| Nobody branches: the condition is handled at a *boundary* — logged, retried, converted to a protocol answer, reported — or its payload cannot be a plain value (it owns a resource, or carries a borrow; spec §35.4) | throw a typed exception, producer-local (spec §31.4), listing `IFailure` when the environment or a peer produced it; let it propagate to where the program can act, catch it there by name or with an `IFailure` arm, and copy out what you keep (§4, §5) |
| Handling what reaches a boundary | an arm for each class this boundary can answer (`FormatException` for bad client input) and an `IFailure` arm for what the environment and peers do; at a boundary only — a request loop, a job runner, `Main` or a supervisor — `OperationCanceledException => throw` first and one `Exception` arm last that reports the rest (§4) |
| Code between the operation and the boundary | no `catch`, or arms only for the classes it can act on; never an arm that only re-raises, never one that catches a bug to carry on (§4) |
| A guarantee a correct caller always provides (a broken one is a bug, not an outcome) | a `requires(...)` clause (spec §22.16; Guide 6 §2) — always on, raises `ContractViolationException`; never for input from outside the program, whose invalidity is a normal outcome (the rows above) |
| Invalid argument where no contract is possible (interop, pre-contract code) | `throw new ArgumentException(...)` |
| Operation invalid in the current state | `throw new InvalidOperationException(...)` (or an `invariant(...)`/`requires(...)` where the condition is a class or caller contract) |
| Dispose an owned resource on every exit, failure included | nothing beyond `using var` — ownership handles it (§6) |
| End an effect on every exit (a batch, an external lock) | a small class whose destructor ends it, bound with `using var` (§6) |
| Undo an effect only on failure | `ScopeGuard` (stdlib §2.21; Guide 1 §10); the commit-flag `try`/`finally` until the compiler implements it (#4669) |
| Preserve exception details past a `catch` | copy `ex.Message` / `ex.StackTrace` into owned values — or `ex.ToString()`, one string carrying the type name, the message, and both chains; never the exception itself |

The table is the teaching mirror of a normative rule: the failure-signal rule (stdlib §1) classifies every condition a library surface declares and gives each condition one mechanism. Two of its sentences are worth restating here. First, the line between the `choice` row and a typed exception is *where the error is consumed* and *whether its payload can be a plain value* — never how much information it carries. Data read once at a boundary rides an exception, however rich; an error the caller branches on near the call site, with a value payload, is a `choice` return. Second, `catch` belongs where the program can act — convert, log, retry, report, stop. Branching program logic on caught exception types at an interior call site is the signal to look for the non-throwing form, and where the library lacks one, that is a gap to report, not a habit to adopt. And at the boundary an arm names what the boundary expects — the classes it can answer, and `IFailure` for the outside world's — where an arm on bare `Exception` takes the bugs, and the cancellation, too.

A few habits carry most of it. Prefer a nullable, `Try`, or `choice` return over an exception whenever the outcome is *expected and branched on*. Prefer a `requires(...)` clause over a `throw` whenever the condition is something a correct caller already guarantees. Let everything else propagate to the boundary that can act on it, and let cancellation through every handler. Leave cleanup to ownership. Inside a `catch`, treat the exception as something you read and let go of — copy out the strings you need, never the instance. And in choice-returning code, take the ergonomics the language gives you: bare case names in return positions, and the combinators where a `case` would only fold. The familiar syntax mostly transfers; the ownership rules around exceptions and `out` values are the part to internalize.
