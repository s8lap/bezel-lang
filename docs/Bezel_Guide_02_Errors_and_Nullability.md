# Bezel Guide 2: Errors and Nullability

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.291.*

Most of the syntax here is mainstream: non-nullable by default, `?` for nullable, `?.`, `??`, exceptions with `try`/`catch`/`finally`. The *emphasis* is not. Bezel splits failure signals by one rule (the **failure-signal rule**, stdlib §1), and it is worth knowing the shape before the details (teaching order here, not the rule's band order):

- An **expected absence** returns a value — `T?` or a `Try` shape (§2).
- An **expected outcome the caller branches on** — with a plain value payload — returns a `Result<T, E>` or a domain choice (§3).
- A **caller bug** is a contract (`requires`, §7) or defect exception.
- A **boundary fault** — I/O, network, anything logged or retried rather than branched on — is a typed exception (§4).
- A handful of fatal conditions simply terminate.

So in Bezel application code the common case is a value return; exceptions are the boundary-and-defect tool, not the default. Beyond the emphasis, two ownership rules will change ingrained habits: a caught exception is a *borrow* (§5), and cleanup on the exception path is *automatic* (§6). Each is headed **How Bezel does it**. If you skim everything else, read those and the table in §7.

**Contents:** §1 nullability basics · §2 expected absence: `T?`, `*OrNull`, `Try` · §3 expected outcomes: `Result<T, E>` and domain choices · §4 exceptions: the familiar shape · §5 a caught exception is a borrow · §6 cleanup without leaks · §7 choosing the right tool.

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

`?.` short-circuits to null; `??` supplies a fallback; `!` asserts non-null and raises `NullReferenceException` at runtime if you were wrong. `??=` assigns only if currently null. Narrowing works by flow analysis on locals:

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

One rule that bites people: **narrowing applies to locals and `readonly` fields only, never to mutable fields or properties.** A mutable field can be reassigned between the check and the use by things the compiler cannot see — a method call that writes it, a closure, or another continuation interleaving at an `await` — so the compiler will not narrow it. Copy it to a local first:

```bezel
var local := _field;               // copy
if local <> null then
  Use(local);                      // narrowed; safe
end;
```

The exception is `readonly` fields. A nullable `readonly` field — instance or static — narrows like a local: nothing can rewrite it after construction, so no call, closure, or `await` can invalidate the fact. The tracked shape is one field directly off a tracked root (`this._f`, `TypeName._s`, or `v._f` off a tracked local or non-`ref` parameter `v`). The rule does not apply inside the declaring type's constructors and initializers, where the field is still being assigned (spec §7.4).

Locals can lose their narrowing too. A local that is **mutably captured** — assigned, or passed by `ref`/`out`, inside any lambda in the method — is re-widened at the next call, function-value invocation, or `await` in the controlled block, because a closure could run and rewrite it in between (spec §7.4). Copy such a value into a plain, uncaptured local and test *that* — the same remedy as for a mutable field.

---

## 2. Expected failure: absence — `T?`, the `*OrNull` tier, and the `Try` pattern

Expected failure is a value, not a throw — exceptions are the boundary-and-defect tool (§4). For failures that are part of normal control flow, return the outcome. The simplest expected failure is **absence** — a cache miss, a lookup with nothing under the key, a parse of input that may legitimately be malformed. Two shapes cover it; §3 adds the third.

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

The standard library ships this idiom as the **`*OrNull` tier** (stdlib §1). Every per-type `Parse` — the stdlib §9 primitives and the `DateTime`/`DateOnly`/`TimeOnly`/`Guid` families — has a `ParseOrNull` returning `T?`; the dictionaries have `GetOrNull`. The composition is one line:

```bezel
var port     := int.ParseOrNull(raw) ?? 8080;
var greeting := config.GetOrNull("greeting") ?? "hello";
```

The same principle reaches the file system: an *expected* missing file is a test, not a catch. `File.Exists(path)` (stdlib §7.3) answers it before anything is opened, so `FileNotFoundException` keeps its meaning — a file you asserted would be there was not:

```bezel
if not File.Exists(path) then
  return defaultSettings;          // expected absence: no try/catch
end;
using var fs := new FileStream(path, FileMode.Open);
```

`ParseOrNull` returns null exactly where `Parse` would throw, so one `??` covers every failure path in a single expression. (For `int`/`bool` environment variables specifically, `Environment.GetInt`/`GetBool` are the dedicated one-call spelling.) When you must *distinguish* absence from a present null — a dictionary whose value type is itself nullable, say — `T?` collapses the two (the conflation caveat, stdlib §1/§4.8). Use `ContainsKey` or the Try shape below instead.

**The `Try` pattern** when the failure path and success path differ, and especially when the value is *owned*. A `bool` return plus an `out owned` parameter, with a `when true` postcondition that lets the caller skip the `!`:

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

The method is named `TryPopCached`, not `TryGetCached`, on purpose. Where the two operations genuinely differ — `Get`/`Peek` leave the entry in place and borrow; `Pop`/`Drain` also remove it and transfer — the stdlib keeps distinct names, so the name alone tells you whether you now owe a disposal (spec §22.6's `out owned` rule; stdlib §1). A pair that differs *only* in ownership on an **ordinary (argument-side)** parameter is instead a same-name overload pair since v0.250, the call-site `move` marker telling the forms apart (stdlib §1). This one transfers: it hands the caller ownership through `out owned`. The parameter is typed `Connection?` (nullable) because the false path must assign *something*, and `default(Connection)` is illegal for a non-nullable reference type. The `when true` clause states the contract in the signature — "a `true` return implies `conn` is non-null" — and the caller's flow analysis picks it up:

```bezel
if pool.TryPopCached("primary", out using var c) then
  UseConnection(c);                // owned, and narrowed non-null by `when true`
end;                               // c disposed at the end of the enclosing block
```

`out using var c` binds the out value as an owned, scope-disposed local — so a successful call that handed you a `Connection` cannot leak. Its scope is the *enclosing* block, from the call onward — not just the `if` — so the connection is disposed when that block ends. Without the `when true` clause you would still write `UseConnection(c!)`: the bool guard alone does not narrow the nullable; only the postcondition does. `out using var` is accepted only against an `out owned` parameter — against a plain `out` the compiler rejects it, because there is no owned source. Plain `out var c` is accepted against either kind and *adopts* what the signature announces: against `out owned` it is an owned local you discharge yourself (a `move`, a store into an owned slot, or scope-end auto-drop with the must-use warning). This is the ownership-aware `TryGet`: the contract is "on `true`, you now own it," and `out using var` is the form that discharges that obligation for you.

---

## 3. Expected failure: outcomes with information — `Result<T, E>` and domain choices

When the failure itself has something to say — *which* error, at *which* position, of *which* kind — absence is not enough. **The `choice` return** is the tool when the failure is expected, branched on at or near the call site, and its payload is a plain value shape (spec §35; the failure-signal rule's band 3, stdlib §1). Where the nullable return says only "absent" and the Try pattern carries the failure in out-parameters, a `choice` names every outcome and carries each one's data. And the `case` over it is exhaustiveness-checked: adding an outcome breaks every consumer at compile time instead of silently falling through a null check:

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

The bare `Ok(value)` / `Error(msg)` spelling is **declared-result target-typing** (spec §35.3). In a `return` statement — or an expression body, or a lambda with a return-type ascription — whose declared result type is a choice, a bare case name constructs that choice's case; the declared type supplies the type arguments whole. The qualified `Result<int, string>.Ok(value)` remains valid everywhere. The bare form exists for exactly these positions, and if a case name shadows something in scope the compiler warns. Restating the full type at every `return` was the old cost of the choice return; it is gone.

`Result<T, E>` (stdlib §2.20) is the stdlib's ready-made shape; a domain-specific choice with named cases (`Fetched`/`NotModified`/`Failed`) is often better still. When a case's payload is an owned resource, the `case` over a fresh call is an *owning match*: bind the payload and adopt it (`using var conn := move c;`) — see Guide 1 §9.

**Pipelining a `Result`.** For value-payload results, four compiler-provided members fold and chain outcomes without writing a `case` (stdlib §2.20): `UnwrapOr(fallback)` folds to a plain value, `Map(f)` transforms the success, `MapError(f)` re-codes the failure at a boundary, and `AndThen(f)` sequences a second fallible step:

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

One trap the `??` of §1 does not prepare you for: **a combinator's argument is an ordinary argument, evaluated at the call site before the call — on the success path exactly as on the failure path** (stdlib §2.20, v0.265). `??` is conditional, so `a ?? Expensive()` never runs `Expensive()` when `a` is non-null (spec §7.7); `.UnwrapOr($"bad port: {raw}")` builds that string on the `Ok` path as well. There is no lazy `UnwrapOrElse`. Where the fallback must be computed only on failure, compute it in the `Error` arm of a `case`.

Note the `AndThen` lambda's return-type ascription: it makes the lambda's own returns declared-result positions, so the bare `Ok`/`Error` spelling works inside it too. The combinators exist only at **value-like** instantiations — payloads of values and strings, the common case. A `Result` carrying an owned resource (`Result<Connection, string>`) has none of them, deliberately. Extraction there is consumption, and consumption is the owning match's job — for owning results, the exhaustive `case` *is* the pipeline. The three idioms compose: `T?` for plain absence, the Try shape for value extraction with a boring failure, the choice return where the failure is branched on with a value-shaped payload.

---

## 4. Exceptions: the familiar shape

For everything the value shapes don't cover — the boundary fault, and the defect a `requires` contract (§7) cannot state — the exception machinery is the familiar one. `try` / `catch` / `finally`, with `catch` arms that look like `case` arms (pattern + optional guard + body), tried top to bottom:

```bezel
try
  var data := Parse(input);
  Process(data);
catch
  var e: FormatException => Log($"bad input: {e.Message}"),
  var e: InvalidOperationException => Log($"bad state: {e.Message}")
finally
  Log("done");
end;
```

`finally` always runs — normal completion, exception, or `return`. An unmatched exception simply keeps propagating (there is no exhaustiveness warning on `catch`; letting non-matching exceptions through is normal). `return` inside `finally` is a compile error.

The built-in hierarchy is what you would expect — `ArgumentException` (with `ArgumentNullException` under it), `InvalidOperationException`, `FormatException`, `OverflowException`, and so on, available without `import` (the full set is spec §31.4). Library producers declare their own exception types beside their APIs — `IOException` and `FileNotFoundException` with the file APIs, `KeyNotFoundException` with `Dictionary<K, V>`, and others — reached by the producer's `import` and otherwise ordinary. Arm patterns match subclasses: a `catch` arm for `ArithmeticException` also catches `OverflowException` and `DivideByZeroException`. Order arms most-specific first.

Two resource-exhaustion faults are deliberately unrecoverable — no `catch` will ever handle them, and both terminate the process. A **stack overflow** raises a `StackOverflowException`: the type exists in the hierarchy, but it **cannot be caught** — no `catch` arm ever matches it. An out-of-memory **allocation failure** is not an exception at all: there is no `OutOfMemoryException`, nothing unwinds, the process simply aborts. Neither is something a `catch` could meaningfully handle — the "handler" would itself need the stack or heap that just ran out — so Bezel does not pretend otherwise.

Throwing:

```bezel
throw new ArgumentException("Amount must be positive.", nameof(amount));
```

`throw;` with no operand re-raises the current exception (in a catch arm only), preserving the original stack trace.

---

## 5. How Bezel does it: a caught exception is a *borrow*, not yours to keep

This is the most important rule to internalize. The stash-it habit from garbage-collected languages — assign the caught exception to a field, capture it in a closure, return it — does not transfer. In Bezel a caught exception is a **borrow** whose lifetime is the catch arm body. The runtime owns the exception and disposes it when the arm completes. You may use `ex` and pass it to methods (they borrow it for the call), but you may **not**:

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

To preserve information about the failure, copy the data you need into owned values *inside the arm*. When the work is more than a line or two, extracting a named helper is usually clearer — the helper gives the action a name a reader can recognize at a glance and localizes any side effects it performs:

```bezel
private _lastErrorText: string?;

method Handle()
  try
    Risky();
  catch
    var ex: Exception => RecordError(ex)
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

## 6. How Bezel does it: exceptions and cleanup — there is no leak, automatically

Elsewhere you reach for `try`/`finally` (or a `using` block) to avoid leaking on the exception path. In Bezel you usually write neither *for leak prevention*. An owned value live when an exception propagates is disposed automatically during unwinding — every owned local and `owned` parameter behaves as if wrapped in an implicit `try`/`finally`. `using var` stays the default; it declares *intent*, not exception-safety. What you stop writing is the `try`/`finally` *cleanup plumbing*.

```bezel
method Import(path: string)
  using var fs := new FileStream(path, FileMode.Open);
  using var parser := new Parser(fs);
  parser.ReadAll();                // if this throws, parser then fs are
end Import;                        // disposed automatically, innermost first
```

No `try`/`finally` is needed for resource safety — that is what `using var` and the ownership model already guarantee, on the exception path as well as the normal one. Use `finally` only for cleanup that is *not* a disposal: flushing a log, releasing a non-owned external lock, restoring global state.

```bezel
try
  BeginBatch();
  DoWork();                        // may throw
finally
  EndBatch();                      // must run even on failure; not a disposal
end;
```

Even a half-constructed object is safe: if a constructor throws after some `owned` fields are set, those fields are disposed before the exception propagates. The same holds for a half-built *expression*: if one argument throws while an earlier argument has already produced an owned temporary, that temporary is disposed during the unwind (v0.184) — an exception never leaks a half-built argument list. You do not write code for any of this.

One edge worth knowing: what if cleanup itself throws *while an exception is already propagating*? A throw escaping a `finally` (or a disposer that throws during unwinding) **supersedes** the in-flight exception — the new one propagates, the old one is disposed (spec §31.2/§28.4). Since v0.259 the buried failure is not lost: before the drop, the runtime snapshots its type, message, and stack trace into the escaping exception's `SupersededChain` (spec §31.4) — the same three-string snapshot machinery as `CauseChain`, chronological, earliest lost first — and the uncaught-exception report prints it. So the failure you see at the top is always the *last* one thrown, and `SupersededChain` is where the *first* one — usually the real one — went. Read it like `CauseChain`: copy the strings out in the arm if you need them past it.

One diagnostic worth recognizing in this area: disposing a value and then using it. A *straight-line* dispose-then-use is a warning (and a deterministic fault in checked mode); a use after a *conditional* dispose — disposed on one branch, still live on another — is a compile error at the merge (the value is "maybe-consumed", spec §28.3). Both are the compiler catching a use-after-free before it can happen.

---

## 7. Choosing the right tool

| Situation | Use |
|---|---|
| Value may legitimately be absent | nullable return `T?` (the stdlib's `*OrNull` tier: `ParseOrNull`, `GetOrNull`), handle with `?.` / `??` / narrowing |
| Absent *and* the value would be owned | `Try` pattern: `bool` + `out owned T? when true`, caller uses `out using var` (and drops the `!`) |
| Failure is expected, carries information the caller *branches on* at or near the call, and the error payload is a plain value shape | `choice` return (`Result<T, E>` or a domain choice, spec §35); consume with an exhaustive `case`, or — where both payloads are value-like — pipeline with the combinators (`Map`/`MapError`/`AndThen`/`UnwrapOr`, stdlib §2.20) |
| Failure is expected but consumed at a *boundary* — logged, retried, converted to a protocol answer, reported — or its payload is an owned resource | a typed exception, producer-local (spec §31.4); read it in the `catch` and copy out what you keep (§5) |
| Caller violated something a correct caller guarantees (a **bug**, not an outcome) | a `requires(...)` clause (spec §22.16) — always-on, faults `ContractViolationException`; never use `requires` for input whose invalidity is a normal outcome at a trust boundary (that is the `T?`/Try/`choice` territory above) |
| Invalid input at a boundary with no contract (interop, pre-contract code) | `throw new ArgumentException(...)` — the residual idiom `requires` replaces where a contract is possible |
| Operation invalid in current state | `throw new InvalidOperationException(...)` (or an `invariant(...)`/`requires(...)` where the condition is a class or caller contract) |
| Must run cleanup that is not a disposal | `try … finally` |
| Must dispose an owned resource on failure | nothing — ownership handles it automatically |
| Preserve failure details past a `catch` | copy `ex.Message` / `ex.StackTrace` into owned values — or `ex.ToString()`, one string carrying the type name, the message, and both chains; never the exception itself |

The table is the teaching mirror of a normative rule: the **failure-signal rule** (stdlib §1) classifies every fault condition a library surface declares — five bands, first match wins, one mechanism per condition. Two of its sentences are worth restating here. First, the line between the `choice` row and a typed exception is *where the failure is consumed* and *whether its payload can be a plain value* — never how much information it carries. A fault whose data is read once at a boundary is a typed exception, however rich the data; a fault the caller branches on near the call site, with a value-shaped payload, is a `choice` return. Second, `catch` is a boundary-and-supervision tool — convert, log, retry, report, re-raise. Branching program logic on caught exception types at an interior call site is the signal to check the rule for a value-shaped tier.

A few habits carry most of it. Prefer a nullable, `Try`, or `choice` return over an exception whenever the failure is *expected and branched on*. Prefer a `requires(...)` clause over a `throw` whenever the condition is something a correct caller already guarantees. Inside a `catch`, treat the exception as something you read and let go of — copy out the strings you need, never the instance. And in choice-returning code, take the ergonomics the language gives you: bare case names in return positions, and the combinators where a `case` would only fold. The familiar syntax mostly transfers; the ownership rules around exceptions and `out` values are the part to internalize.
