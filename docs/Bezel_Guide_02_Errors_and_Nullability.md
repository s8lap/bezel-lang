# Bezel Guide 2: Errors and Nullability

*A short, example-first guide. Companion to the Bezel Language Specification v0.128.*

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

One rule that bites people: **narrowing applies to locals only, never to fields or properties.** A field can be changed by another thread between the check and the use, so the compiler will not narrow it. Copy it to a local first:

```bezel
var local := _field;               // copy
if local <> null then
  Use(local);                      // narrowed; safe
end;
```

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
  Log($"{ex.Message}\n{ex.StackTrace.ToString()}");
end RecordError;
```

`ex.Message` is a `string` (runtime-managed, no ownership obligation), so copying it out is fine. The exception *instance* cannot escape. The single exception to the rule is the bare re-throw `throw;`, which hands the in-flight exception back to the runtime rather than capturing it.

The reasoning: an exception often carries no disposable resources, but the rule is uniform so the language never has to ask "does this exception own something?" at a `catch` boundary. Copy what you need; let the instance go.

---

## 4. How Bezel does it: exceptions and cleanup — there is no leak, automatically

In C# you reach for `try`/`finally` (or `using`) to avoid leaking on the exception path. In Bezel you usually write neither: an owned value live when an exception propagates is disposed automatically during unwinding (every owned local and `owned` parameter behaves as if wrapped in an implicit `try`/`finally`).

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

Even a half-constructed object is safe: if a constructor throws after some `owned` fields are set, those fields are disposed before the exception propagates. You do not write code for this.

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

**The `Try` pattern** when the failure path and success path differ, and especially when the value is *owned*. A `bool` return plus an `out owned` parameter:

```bezel
private owned _cache: OwnedDictionary<string, Connection>;

method TryGetCached(name: string, out owned conn: Connection?): bool
  if not _cache.ContainsKey(name) then
    conn := null;                  // every out path must assign
    return false;
  end;
  conn := _cache.PopValue(name);   // takes ownership out of the cache
  return true;
end TryGetCached;
```

The parameter is `out owned` because a successful call hands the caller ownership of the `Connection`. It is typed `Connection?` (nullable) because the false path must assign *something* and `default(Connection)` is illegal for a non-nullable reference type. The caller consumes with `out using var` and trusts the bool guard:

```bezel
if pool.TryGetCached("primary", out using var c) then
  UseConnection(c!);               // bool guarantee makes ! safe; c is owned here
end;                               // c disposed at end of this block
```

`out using var c` binds the out value as an owned, scope-disposed local — so a successful `TryGetCached` that handed you ownership of a `Connection` cannot leak. (`out owned` in the signature and `out using var` at the call site must match: you cannot write `out using var` for a plain `out` parameter, nor plain `out var` for an `out owned` one — the compiler rejects the mismatch, because the ownership would have nowhere to go.) This is the ownership-aware version of C#'s `TryGet`: the contract is "on `true`, you now own it," and `out using var` discharges that obligation for you.

---

## 6. Choosing the right tool

| Situation | Use |
|---|---|
| Value may legitimately be absent | nullable return `T?`, handle with `?.` / `??` / narrowing |
| Absent *and* the value would be owned | `Try` pattern: `bool` + `out owned T?`, caller uses `out using var` |
| Caller passed something invalid | `throw new ArgumentException(...)` |
| Operation invalid in current state | `throw new InvalidOperationException(...)` |
| Must run cleanup that is not a disposal | `try … finally` |
| Must dispose an owned resource on failure | nothing — ownership handles it automatically |
| Preserve failure details past a `catch` | copy `ex.Message` / `ex.StackTrace.ToString()` into owned values; never the exception itself |

Two habits carry most of it: prefer a nullable or `Try` return over an exception whenever the failure is expected, and inside a `catch` treat the exception as something you read and let go of — copy out the strings you need, never the instance. The familiar C# syntax mostly transfers; the ownership rules around exceptions and `out` values are the part to internalize.
