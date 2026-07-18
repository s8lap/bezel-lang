# Bezel Guide 2: Errors and Nullability

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.200.*

If you know C#, Kotlin, or Swift, most of this chapter is familiar: non-nullable by default, `?` for nullable, `?.`, `??`, exceptions with `try`/`catch`/`finally`. Skim those parts. The three places where the ownership model changes how you write this code — and where a habit from those languages produces a *compile error* — are headed **How Bezel does it**. Read those.

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

One rule that bites people: **narrowing applies to locals only, never to fields or properties.** A field can be reassigned between the check and the use by things the compiler cannot see — a method call that writes it, a closure, or another continuation interleaving at an `await` — so the compiler will not narrow it. Copy it to a local first:

```bezel
var local := _field;               // copy
if local <> null then
  Use(local);                      // narrowed; safe
end;
```

One local *also* loses its narrowing: a local that is **mutably captured** — assigned, or passed by `ref`/`out`, inside any lambda in the method — is re-widened at the next call, function-value invocation, or `await` in the controlled block, because a closure could run and rewrite it in between (spec §7.4). Copy such a value into a plain, uncaptured local and test *that* — the same remedy as for a field.

---

## 2. Exceptions: the familiar shape

`try` / `catch` / `finally`, with `catch` arms that look like `case` arms (pattern + optional guard + body), tried top to bottom:

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

The built-in exception hierarchy is what you would expect — `ArgumentException` (with `ArgumentNullException` under it), `InvalidOperationException`, `OverflowException`, `IndexOutOfRangeException`, `FormatException`, and so on. These are available without `import`. Arm patterns match subclasses: a `catch` arm for `ArithmeticException` also catches `OverflowException` and `DivideByZeroException`. Order arms most-specific first.

A handful of common exception types live in the standard library rather than the built-in set and need an explicit `import` — `IOException` and `FileNotFoundException` (with the file APIs), `KeyNotFoundException` (with `Dictionary<K, V>`), and others. They are otherwise ordinary exceptions, caught the same way.

Two resource-exhaustion faults are deliberately unrecoverable — no `catch` will ever handle them, and both terminate the process. A **stack overflow** raises a `StackOverflowException`: the type exists in the hierarchy, but it **cannot be caught** — no `catch` arm ever matches it. An out-of-memory **allocation failure** is not an exception at all: there is no `OutOfMemoryException`, nothing unwinds, the process simply aborts. Neither is something a `catch` could meaningfully handle — the "handler" would itself need the stack or heap that just ran out — so Bezel does not pretend otherwise.

Throwing:

```bezel
throw new ArgumentException("Amount must be positive.", nameof(amount));
```

`throw;` with no operand re-raises the current exception (in a catch arm only), preserving the original stack trace.

---

## 3. How Bezel does it: a caught exception is a *borrow*, not yours to keep

This is the most important rule to internalize. In C# you can stash a caught exception — `_lastError = ex;`, capture it in a closure, return it. In Bezel a caught exception is a **borrow** whose lifetime is the catch arm body. The runtime owns the exception and disposes it when the arm completes. You may use `ex` and pass it to methods (they borrow it for the call), but you may **not**:

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
  Log($"{ex.Message}\n{ex.StackTrace}");
end RecordError;
```

`ex.Message` is a `string` (runtime-managed, no ownership obligation), so copying it out is fine. The exception *instance* cannot escape. The single exception to the rule is the bare re-throw `throw;`, which hands the in-flight exception back to the runtime rather than capturing it.

The reasoning: an exception often carries no disposable resources, but the rule is uniform so the language never has to ask "does this exception own something?" at a `catch` boundary. Copy what you need; let the instance go.

The same borrow discipline reaches *inside* an exception you throw: don't stash a borrow of a local in an exception's field. The unwind disposes that local before the handler runs — in checked mode the handler's access to it faults deterministically as `DisposedAccessException`. Give exception classes owned or copied state instead; that is exactly why the built-in cause-chaining constructor (`new Exception(msg, cause)`) *snapshots* the cause rather than pointing at it.

---

## 4. How Bezel does it: exceptions and cleanup — there is no leak, automatically

In C# you reach for `try`/`finally` (or `using`) to avoid leaking on the exception path. In Bezel you usually write neither *for leak prevention*: an owned value live when an exception propagates is disposed automatically during unwinding (every owned local and `owned` parameter behaves as if wrapped in an implicit `try`/`finally`). `using var` stays the default — it declares *intent*, not exception-safety — and what you stop writing is the `try`/`finally` *cleanup plumbing*.

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

One diagnostic worth recognizing in this area: disposing a value and then using it. A *straight-line* dispose-then-use is a warning (and a deterministic fault in checked mode); a use after a *conditional* dispose — disposed on one branch, still live on another — is a compile error at the merge (the value is "maybe-disposed"). Both are the compiler catching a use-after-free before it can happen.

---

## 5. Expected failure: return a value, don't throw

Exceptions are for the unexpected. For failures that are part of normal control flow — a cache miss, a parse that may legitimately fail, a lookup that may be absent — return the outcome instead. Two idioms.

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

**The `Try` pattern** when the failure path and success path differ, and especially when the value is *owned*. A `bool` return plus an `out owned` parameter, with a `when true` postcondition that lets the caller skip the `!`:

```bezel
private owned _cache: OwningDictionary<string, Connection>;

method TryTakeCached(name: string, out owned conn: Connection? when true): bool
  if not _cache.ContainsKey(name) then
    conn := null;                  // every out path must assign (throw paths exempt)
    return false;
  end;
  conn := _cache.PopValue(name);   // takes ownership out of the cache
  return true;
end TryTakeCached;
```

The method is named `TryTakeCached`, not `TryGetCached`, on purpose: the stdlib gives the borrowing accessor and the ownership-transferring one distinct names — `Get`/`Peek` borrow; `Pop`/`Drain`/`Take` transfer — so the name alone tells you whether you now owe a disposal (spec §22.6). This one transfers: it hands the caller ownership through `out owned`. The parameter is typed `Connection?` (nullable) because the false path must assign *something* and `default(Connection)` is illegal for a non-nullable reference type. The `when true` clause states the contract in the signature — "a `true` return implies `conn` is non-null" — and the caller's flow analysis picks it up:

```bezel
if pool.TryTakeCached("primary", out using var c) then
  UseConnection(c);                // owned, and narrowed non-null by `when true`
end;                               // c disposed at the end of the enclosing block
```

`out using var c` binds the out value as an owned, scope-disposed local — so a successful call that handed you a `Connection` cannot leak. Its scope is the *enclosing* block, from the call onward (not just the `if`), so the connection is disposed when that block ends. Without the `when true` clause you would still write `UseConnection(c!)` — the bool guard alone does not narrow the nullable; only the postcondition does. (`out owned` in the signature and `out using var` at the call site must match: you cannot write `out using var` for a plain `out` parameter, nor plain `out var` for an `out owned` one — the compiler rejects the mismatch, because the ownership would have nowhere to go.) This is the ownership-aware version of C#'s `TryGet`: the contract is "on `true`, you now own it," and `out using var` discharges that obligation for you.

**The `choice` return** when the failure itself carries information (spec §35). Where the nullable return says only "absent" and the Try pattern carries the failure in out-parameters, a `choice` names every outcome and carries each one's data — and the `case` over it is exhaustiveness-checked, so adding an outcome breaks every consumer at compile time instead of silently falling through a null check:

```bezel
public method ParsePort(text: string): Result<int, string>
  if not int.TryParse(text, out var value) then
    return Result<int, string>.Error($"not a number: {text}");
  end;
  if value < 1 or value > 65535 then
    return Result<int, string>.Error($"out of range: {value}");
  end;
  return Result<int, string>.Ok(value);
end ParsePort;

case ParsePort(arg) of
  Ok(var p)    => Listen(p),
  Error(var m) => Console.WriteLine($"bad port: {m}")
end;                                  // exhaustive; no _ arm needed
```

`Result<T, E>` (stdlib §2.20) is the stdlib's ready-made shape; a domain-specific choice with named cases (`Fetched`/`NotModified`/`Failed`) is often better still. When a case's payload is an owned resource, the `case` over a fresh call is an *owning match*: bind the payload and adopt it (`using var conn := owned c;`) — see Guide 1 §9. The three idioms compose: `T?` for plain absence, the Try shape for value extraction with a boring failure, the choice return where the failure carries information.

---

## 6. Choosing the right tool

| Situation | Use |
|---|---|
| Value may legitimately be absent | nullable return `T?`, handle with `?.` / `??` / narrowing |
| Absent *and* the value would be owned | `Try` pattern: `bool` + `out owned T? when true`, caller uses `out using var` (and drops the `!`) |
| Failure is expected *and carries information* | `choice` return (`Result<T, E>` or a domain choice, spec §35); consume with an exhaustive `case` |
| Caller passed something invalid | `throw new ArgumentException(...)` |
| Operation invalid in current state | `throw new InvalidOperationException(...)` |
| Must run cleanup that is not a disposal | `try … finally` |
| Must dispose an owned resource on failure | nothing — ownership handles it automatically |
| Preserve failure details past a `catch` | copy `ex.Message` / `ex.StackTrace` into owned values; never the exception itself |

Two habits carry most of it: prefer a nullable or `Try` return over an exception whenever the failure is expected, and inside a `catch` treat the exception as something you read and let go of — copy out the strings you need, never the instance. The familiar C# syntax mostly transfers; the ownership rules around exceptions and `out` values are the part to internalize.
