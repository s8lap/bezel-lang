# Proposal 0021 — Structured logging (`ILogger`, `LogEvent`, cross-domain sink)

| | |
|---|---|
| **Status** | under-review — Draft for owner review |
| **Target module** | `System.Diagnostics` (a new §8.3, beside `Stopwatch`/`Debug`; requires `import System.Diagnostics`) |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Tracking issue** | [#1460](https://github.com/s8lap/bezel-lang/issues/1460) |
| **Gap-review item** | #8 (P0, diagnostics) in [stdlib-gap-review-2026-07.md](stdlib-gap-review-2026-07.md); wave **D3** in the [gap DESIGN plan](stdlib-gap-design-plan-2026-07.md) ("design after 0002 experience") |
| **Design influences** | .NET `Microsoft.Extensions.Logging` `ILogger` (levels + `IsEnabled`); Serilog for the structured-fields idea — but **not** its message-template auto-capture (declined, §D3); Rust `tracing`/`log` for the facade-plus-sink split |
| **Bezel-native leverage** | A `LogEvent` is **sendable by construction** (§30.14), so a log record can be *moved across a domain boundary* to a dedicated sink — data-race-free logging is a compile-time property, exactly as 0002's handlers are |
| **Depends on** | §30.13/§30.14/§30.15 (domains, sendability, channels), §22.12 (statics), §6.9 bounded `Channel<T>` (v0.174), §2.18/§18.5 (formatting/interpolation), §31.4 (`ExceptionInfo`); proposals 0002 (first consumer), 0003 (`Locked<T>`), 0006 (streams — the file sink), 0014 (lossy-channel precedent) |

## 1. The problem

The two facilities a service could reach for both fail it. `Debug.WriteLine` (stdlib §8.2) "may be elided in release builds" — the diagnostics you most need in production are the ones compiled out. `Console.WriteLine` (§2.1) survives release, but it has **no levels** (nothing to filter Trace from Critical), **no categories** (nothing says *which* subsystem spoke), **no sinks** (output is hard-wired to the console), and **no structure** (everything is an opaque line of text a machine cannot query). And it has a subtler defect: `Console.Out` is a process-global `TextWriter` shared by every domain, so two domains writing concurrently interleave partial lines. No real service can ship on either.

What a service needs is: a small facade it logs *through*, **levels** it can filter by, a **category** per subsystem, **structured fields** a backend can index, an **exception snapshot** it can preserve past the `catch` arm, and a **sink** that can be swapped (console today, file or an aggregator tomorrow) and that never blocks or races the application. The load-bearing constraint that shapes all of it: **log calls happen on every domain**, so a log record must be able to travel to wherever the sink lives — which in Bezel means it must be **sendable** (§30.14).

## 2. Design decisions

### D1 — The record shape: `LogField` (value record) + `LogEvent` (sealed sendable class)

A structured field is a name paired with a value. Both are strings (see D2), so:

```bezel
public record LogField(Name: string, Value: string);
```

`LogField` is a value record of two `string`s: not borrow-bearing, not move-only ⇒ a **sendable value type** (§30.14, §9.7). It copies freely and crosses a domain boundary as a copy. This is the D-0002-compliant shape: a *named record*, never an anonymous object — the field's identity is a documented shape, not a structural convenience read off at runtime.

A `LogEvent` gathers everything about one log call. The instinct is another value record — but a record **cannot** carry the field list. A record's reference-typed fields are *always borrows* and a record may not have `owned` fields (§9.7); a variable-length `Array<LogField>` would therefore be either a borrow of a caller's array (making `LogEvent` borrow-bearing ⇒ **not sendable**, defeating the whole point) or impossible. So `LogEvent` is a **sealed class** that *owns* its field list:

```bezel
public sealed class LogEvent
  public constructor(timestamp: DateTime, level: LogLevel, category: string,
                     message: string, owned fields: Array<LogField>?,
                     owned causes: Array<ExceptionInfo>?);

  public readonly Timestamp: DateTime;                 // sendable value type
  public readonly Level: LogLevel;                     // enum ⇒ sendable value
  public readonly Category: string;                    // string ⇒ sendable
  public readonly Message: string;                     // already-interpolated human text
  public owned readonly Fields: Array<LogField>?;      // owning edge; null when none
  public owned readonly Causes: Array<ExceptionInfo>?; // §31.4 snapshot; sendable strings
end LogEvent;
```

**Sendability derivation (the load-bearing property).** `LogEvent` is `sealed` (rule 1 of §30.14's class rule) and every field satisfies rule 2/3: `Timestamp` is a sendable value type; `Level` is an enum (sendable); `Category`/`Message` are `string`; `Fields` is an **`owned` field whose declared type is a sendable class type** — `Array<LogField>` is a borrowing collection whose element category is *sendable-value*, which §30.14 declares sendable; `Causes` is the same over `Array<ExceptionInfo>` (`ExceptionInfo` is a record of three strings, §31.4, hence sendable-value). No function-typed, `Object`-typed, interface-typed, or non-`owned` class field appears (rule 3). Therefore `LogEvent` is **transfer-closed** and a `LogEvent` instance crosses a `Channel<LogEvent>` (§30.15) as an owned transfer — the whole tree, arrays included, moves with it. This is what lets the same record type serve both a same-domain sink and a cross-domain sink with no second representation.

### D2 — Field values are strings-after-formatting in v1 (typed values deferred)

`LogField.Value` is a `string`: the caller formats the value at the log site (`LogField("count", count.ToString())`, or via interpolation `LogField("user", $"{user}")`). The tempting alternative — a *typed* value so a backend can treat `count` as a number — is a **closed-value-union** problem Bezel has no clean answer for today:

- A `Value: Object` (boxed) field is sendable only if the boxed value is sendable, **and** reading the value back out at the sink requires inspecting its runtime type and downcasting — which is the member-access-reflection shape Bezel deliberately lacks (rationale §18). A sink that must `case value.GetType() of int => …` to render a field is exactly the machinery this language avoids.
- A discriminated `LogValue` union (tag + one-of `{int64, double, bool, string, DateTime}`) would be sendable and reflection-free — but Bezel has no sum-type / closed-union construct yet, and inventing one for logging alone is out of scope.

So v1 commits to strings-after-formatting: `LogField` stays a pure two-string sendable record, the sink stays reflection-free, and the caller keeps full control of formatting through the ordinary §2.18 vocabulary. **Recorded for a future revision:** once Bezel has a closed-union story, a typed `LogValue` gives numeric/temporal fields that a structured backend can index by type — a strict superset, addable without breaking the string form (a typed field renders to its string when a text sink consumes it).

### D3 — The API surface: one `ILogger` primitive, conveniences as extensions, message-by-interpolation + structure-by-explicit-array

```bezel
public interface ILogger
  method IsEnabled(level: LogLevel): bool;
  method Log(level: LogLevel, message: string,
             fields: Array<LogField>?, cause: Exception?);
end ILogger;
```

One primitive. `message` is the *already-rendered* human string; `fields` is a **borrowed** `Array<LogField>?` (a routing sink copies it — `fields.ToArray()` — into the `LogEvent` it sends; `null` ⇒ none); `cause` is a **borrowed** `Exception?` the sink snapshots (D7) and never retains. The logger stamps the timestamp and its own category.

The per-level conveniences (`Info`, `Warn`, …) are **extension methods on `ILogger`**, not interface members — Bezel has no default interface methods (spec §11, "the declined default interface methods, rationale §17.4"), so one shared implementation for every sink is spelled as an extension (§26.2 admits extending interfaces):

```bezel
public extension ILogger
  public method Info(message: string)
    if this.IsEnabled(LogLevel.Info) then this.Log(LogLevel.Info, message, null, null); end;
  end Info;

  public method Info(message: string, fields: Array<LogField>)
    if this.IsEnabled(LogLevel.Info) then this.Log(LogLevel.Info, message, fields, null); end;
  end Info;

  public method Error(message: string, cause: Exception)
    if this.IsEnabled(LogLevel.Error) then this.Log(LogLevel.Error, message, null, cause); end;
  end Error;
  // …Trace/Debug/Warn/Critical, each with (message) / (message, fields) /
  //   (message, cause) / (message, fields, cause) overloads — full set in §3.
end;
```

**Message-template vs interpolation — the honest weigh-up.** Two candidate primary forms:

1. **Interpolation for the message** (`log.Info($"user {name} logged in")`). Uses the existing §18.5 machinery, no new format convention. It renders a clean human line — but the structure is *gone*: `name` is baked into the string, not a queryable field.
2. **A Serilog-style message template** (`log.Info("user {User} logged in", name)`) renders the line **and** captures `User = name` as a structured field, by matching the `{User}` holes to positional args. It is the ergonomic sweet spot in .NET — but for Bezel it is doubly wrong: it introduces a **second format-string convention** (`{Name}` holes distinct from `$"{expr}"` interpolation — the very objection that declined `String.Format`, gap review §Declined #1), and the auto-capture *reads field names out of a string literal at runtime* and pairs them with positional arguments — **name-magic**, structurally the reflection-shaped binding Bezel refuses (rationale §18).

**Recommended primary form: interpolation for the human message, an explicit `Array<LogField>` for structure.**

```bezel
log.Info($"user {name} logged in", [LogField("user", name)]);
```

The message is ordinary interpolation (no new convention); the fields are an explicit, named collection literal (§15.4 — and note §A: there is no `params`, so "a list of things at a call site" *is* `Array<T>` + a literal; this API is idiomatic, not a workaround). Yes, `name` appears twice — that is the honest cost of separating *what humans read* from *what machines index*, and it is a choice the author makes field by field, not a convenience that guesses. It composes with D2 and D-0002 (explicit named records, no anonymous shapes) and with the String.Format decline (no second format string).

### D4 — Levels and filtering

```bezel
public enum LogLevel: byte
  Trace,      // 0 — very fine-grained tracing
  Debug,      // 1 — developer diagnostics
  Info,       // 2 — normal significant events
  Warn,       // 3 — something unexpected, still working  (spelling ruled 2026-08-11, Q5)
  Error,      // 4 — an operation failed
  Critical,   // 5 — the application itself is in trouble
  None;       // 6 — a threshold that emits nothing (never a call argument)
end LogLevel;
```

A sink holds a `minLevel`; `IsEnabled(level)` is `int(level) >= int(minLevel)`. The cast to the underlying integer is required because **enums carry no relational operators** — they support `=`/`<>` and `case`, but ordering goes through the underlying type (§10.4's own `CompareTo` example writes `int(this) < int(other)`). `None` as a `minLevel` disables the sink entirely; `None` is never passed to a `Log` call.

**On "compile away debug logs" (there are no macros).** Bezel has no `#ifdef`/`cfg` conditional compilation (declined, gap review §Declined #6, as a language change) and no macros, so a `log.Debug(...)` statement is *not* removed from a release binary. The honest tool is the runtime guard: the `Log` call itself is a cheap branch when disabled (the extension checks `IsEnabled` before allocating anything), and when the *arguments* are expensive to build the caller guards at the call site —

```bezel
if log.IsEnabled(LogLevel.Debug) then
  log.Debug($"state = {ExpensiveSnapshot()}", BuildDiagnosticFields());
end;
```

— which is exactly what `IsEnabled` exists for. This is a genuine, if modest, difference from `Debug.WriteLine`'s release elision: nothing disappears, but nothing expensive runs either.

### D5 — Cross-domain routing: two topologies, drop-newest on a full bounded channel

Because a log call can originate on any domain, the sink must be reachable from all of them. Two topologies, **both** buildable on today's primitives, both coding to `ILogger` so the application picks at its composition root:

**(a) Direct, per-domain sink — `ConsoleLogger`.** The logger formats and writes inline, guarding the shared console output with an internal `Mutex` (§6.2) so concurrent domains never interleave partial lines. `Mutex` is sendable and internally synchronized, so a `ConsoleLogger` is itself **sendable** — but "sendable" under §30.14 means *movable into any one domain*, not *shareable across domains simultaneously*. A distinct `ConsoleLogger` may therefore be moved into each domain that needs one; a single instance is reachable process-wide only via the D6 Fork-B `static readonly` (a §22.12 rule-4 interface-typed static, warned) or a `Shared<T>`-of-synchronized handle. For the sendability derivation to hold, `ConsoleLogger` must hold **no non-owned `TextWriter` field** — it reaches `Console.Out`/`Console.Error` (process statics) directly under its `Mutex`, rather than carrying a borrowed writer. Simple; correct for single-domain programs and low-volume logging; its cost is that formatting and the write happen *on the calling domain*, under a lock.

**(b) One sink domain fed by a bounded `Channel<LogEvent>` — the scalable path.** Producers build a `LogEvent` and `Send` it; a single **sink domain** owns the receiving `Channel<LogEvent>`, loops `Receive()` → format → write, and is the *only* writer — so it needs **no** `Mutex` (the channel is the synchronization, and the sink domain is serial, §30.13). This is the actor idiom the language already blesses, and it moves all formatting/I/O off the hot path.

**The drop policy — logging must never deadlock or throttle the app.** The channel is **bounded** (§6.9, v0.174). A producer therefore uses the synchronous `Send`, never `SendAsync`:

- `SendAsync` *awaits capacity* — it applies **back-pressure**, i.e. it would slow (or, under a lock held across the await, risk stalling) the application whenever the sink falls behind. That is the wrong contract for logging.
- Synchronous `Send` never blocks; on a full bounded channel it raises `ChannelFullException` (§6.9). The `ChannelLogger` catches it and **drops the event being logged right now (drop-*newest*)**, incrementing a dropped-events counter.

**Why drop-newest and not drop-oldest.** Drop-oldest (evict the stalest buffered event to admit the new one) is arguably preferable for diagnostics — but it is **not implementable on today's `Channel<T>`**: the API has no "evict oldest / overwrite on full" operation, and the overwrite-on-full ring-buffer mode was itself declined (gap review §Declined #7: silently disposing an element the caller handed over inverts the ownership contract's legibility). Drop-newest is the behavior the current primitive *can* express soundly, and it degrades gracefully: a full channel means the consumer is behind, the newest events are shed, and the **counter makes the loss visible** rather than silent. This mirrors **proposal 0014 §D5** precisely — signal delivery coalesces (drops) on a full bounded channel and the design "makes lossy delivery explicit" rather than growing an unbounded buffer (a correctness risk in a no-GC language). Structured logging applies the same discipline, adding the drop counter.

The counter is per-`ChannelLogger` (each is domain-local — see below), readable as `DroppedCount: long`. Aggregating drops across all producer domains, or having the sink surface a periodic "N events dropped" summary, is a v2 refinement (§6, open question).

**Ownership/topology sketch.** A `LogHub` owns the receiving `Channel<LogEvent>` and the output sink in the composition-root domain and mints one `ChannelLogger` per producer. Each `ChannelLogger` wraps its own `owned ChannelSender<LogEvent>` (plus its category, min-level, and drop counter) and is a **sealed sendable class**, so it is *moved into* a worker domain at spawn (senders are sendable, §6.9; the logger is domain-local thereafter — no logger is ever shared, only the channel is, exactly as 0014 fans out independent senders). When every `ChannelLogger` is disposed the channel closes, the sink loop drains and exits (clean shutdown).

### D6 — The ambient question: explicit-first, with a recorded escape-hatch fork

Should there be a `Log.Default` static so code can log without being handed a logger? This is the philosophical fork, and it is D-0001 (the DI-container decline) in miniature: a logger is *the* canonical ambient dependency.

- **Fork A — no ambient (recommended for v1).** The logger is an ordinary dependency: passed to service constructors from the composition root (proposal 0002 §2.1's exact pattern — a `TodoApi` holding its collaborators, wired with no container), and *moved into* worker domains as a spawn argument. Zero global state; fully §30.14-clean; the dependency contract is visible in every constructor. The ergonomic cost — threading a logger through call trees — is mitigated the way DI's is: hand it to the top-level service objects that need it, not to every leaf function; leaf code that logs is a method on a service that holds an `ILogger`. This is the posture the whole proposals folder already takes.

- **Fork B — a minimal, fixed ambient escape hatch.** A `static readonly Log.Default: ILogger`, initialized *once in its static initializer* to a process-wide internally-synchronized logger (a `Mutex`-guarded `ConsoleLogger`, or a `ChannelLogger` to a sink domain started at process init). §22.12 rule 4 admits this **with a declaration-site warning** (an interface-typed static is reachable from every domain; confining its use or ensuring its internal synchronization is the programmer's obligation — the "singleton with resources" case the warning names). It is safe *iff* the assigned logger is genuinely internally synchronized, which a `Mutex`-guarded `ConsoleLogger` is. Crucial constraint: it **cannot be reassigned at runtime** — a settable `Log.Default := myLogger` in `Main` is a *mutable* static of non-synchronized type, which §22.12 rule 2 rejects at compile time. So Fork B buys ambient access at the price of the rule-4 warning and a fixed-at-init logger.

- **Fork C — reconfigurable ambient via `Locked<T>` (deferred/rejected).** `static readonly Log.Default: Locked<ILogger>` (proposal 0003) is synchronized *and* runtime-reconfigurable — but it puts a lock acquisition on the read path of every log call, and reconfiguring the global logger at runtime is rare enough not to justify it. Recorded and declined for v1.

**Recommendation:** ship **Fork A only** in v1 (explicit passing, composition root), matching D-0001's ethos, and keep **Fork B** documented as the sanctioned escape hatch a program may opt into if ergonomics genuinely demand it — with its rule-4 warning and fixed-at-init constraint stated plainly. The owner should rule the fork; the draft's API surface (§3) is identical either way, so this decision does not gate the rest.

### D7 — v1 sinks and the exception snapshot

- **`ConsoleLogger`** (topology a): `Mutex`-guarded write to `Console.Out`, or `Console.Error` for `Error`/`Critical`. Formatting per §2.18 / §18.5 (D-below).
- **`NullLogger`**: `IsEnabled` always `false`, `Log` a no-op — the object that lets call sites omit null checks (an app with logging disabled holds a `NullLogger`, not a `null`).
- **`ConsoleSink`** (topology b): the sink-domain output target; writes received events, no lock (sink domain is serial).
- **File sink**: rides **proposal 0006** — a sink domain whose output is a `StreamWriter` over a file (0006 §"StreamWriter as the §7.4 concrete"). Marked a **proposed dependency**; ships when 0006 lands. No new logging surface is needed — a `StreamSink(owned writer: TextWriter)` wraps any `TextWriter`, and 0006 provides the file-backed one.

**Exception snapshot (§31.1/§31.4).** A `cause: Exception` reaches `Log` as a **borrow**, exactly like a `catch`-arm exception (§31.1): the sink may pass it around for the duration of the call but must not retain it. The sink immediately copies the fault into a sendable snapshot — an `Array<ExceptionInfo>` whose head is `new ExceptionInfo(cause.GetType().Name, cause.Message, cause.StackTrace)` (§8.2 `GetType()` and §31.4's `TypeName`/`Message`/`StackTrace` — all `string`s) followed by `cause.CauseChain` (already an `Array<ExceptionInfo>` snapshot, §31.4). The live `Exception` never enters a `LogEvent` and never crosses a domain (exception classes are not in general sendable, §30.14/§30.6). This is the same discipline 0002 §3.1 applies to its `OnError` hook.

**Default text format.** A `LogFormat.Line(logEvent)` helper renders one line, e.g.

```
2026-07-04T12:00:00.123 [INFO ] Http.Server: user logged in { user=alice, ip=10.0.0.4 }
```

built with §18.5 interpolation and a `StringBuilder` (§3.1). A structured **JSON** formatter is a v2 addition: `LogEvent` can be made `ISerializable` (its serialized components — `DateTime`, `LogLevel` enum, `string`, `Array<LogField>`, `Array<ExceptionInfo>` — are all serialization-eligible, §9.8), which rides proposal 0010's `DateTime` conformance; deferred so v1 does not block on it.

## 3. Proposed stdlib additions (ready-to-land text)

All in `System.Diagnostics` (§8.3, requires `import System.Diagnostics`).

```bezel
public enum LogLevel: byte
  Trace, Debug, Info, Warn, Error, Critical, None;   // Warn: one spelling per level (ruling 2026-08-11, Q5)
end LogLevel;

public record LogField(Name: string, Value: string);   // sendable value record (§30.14)

public sealed class LogEvent                            // sendable class (§30.14, see §D1)
  public constructor(timestamp: DateTime, level: LogLevel, category: string,
                     message: string, owned fields: Array<LogField>?,
                     owned causes: Array<ExceptionInfo>?);
  public readonly Timestamp: DateTime;
  public readonly Level: LogLevel;
  public readonly Category: string;
  public readonly Message: string;
  public owned readonly Fields: Array<LogField>?;
  public owned readonly Causes: Array<ExceptionInfo>?;
end LogEvent;

public interface ILogger
  // Cheap: is this level currently emitted? Guard expensive argument construction
  // with it at the call site — there are no compile-away macros (see §D4).
  method IsEnabled(level: LogLevel): bool;
  // The single primitive every sink implements. `fields` and `cause` are borrows:
  // a routing sink copies them into a LogEvent; the live Exception never escapes.
  method Log(level: LogLevel, message: string,
             fields: Array<LogField>?, cause: Exception?);
end ILogger;

// Per-level conveniences shared by every sink (no default interface methods in Bezel).
public extension ILogger
  public method Trace(message: string);
  public method Trace(message: string, fields: Array<LogField>);
  public method Debug(message: string);
  public method Debug(message: string, fields: Array<LogField>);
  public method Info(message: string);
  public method Info(message: string, fields: Array<LogField>);
  public method Warn(message: string);
  public method Warn(message: string, fields: Array<LogField>);
  public method Error(message: string);
  public method Error(message: string, fields: Array<LogField>);
  public method Error(message: string, cause: Exception);
  public method Error(message: string, fields: Array<LogField>, cause: Exception);
  public method Critical(message: string, cause: Exception);
  public method Critical(message: string, fields: Array<LogField>, cause: Exception);
  // each body: `if this.IsEnabled(<lvl>) then this.Log(<lvl>, message, <fields|null>, <cause|null>); end;`
end;

// ---- Direct sinks (topology a) ----

public sealed class ConsoleLogger: ILogger
  // Sendable (movable into any one domain) and internally synchronized: an internal
  // Mutex guards writes to the Console statics (Console.Out / Console.Error) — it holds
  // NO non-owned TextWriter field, which is what keeps the §30.14 derivation intact — so
  // concurrent domains never interleave partial lines. Error/Critical -> Console.Error.
  public constructor(category: string, minLevel: LogLevel := LogLevel.Info);
  public method IsEnabled(level: LogLevel): bool;
  public method Log(level: LogLevel, message: string,
                    fields: Array<LogField>?, cause: Exception?);
end ConsoleLogger;

public sealed class NullLogger: ILogger
  public constructor();
  public method IsEnabled(level: LogLevel): bool;   // always false
  public method Log(level: LogLevel, message: string,
                    fields: Array<LogField>?, cause: Exception?);   // no-op
end NullLogger;

// ---- Sink-domain routing (topology b) ----

public interface ILogSink: IDisposable
  method Emit(logEvent: LogEvent);   // borrow; the sink formats + writes, the loop then disposes it (`event` is a reserved keyword, Appendix B)
end ILogSink;

public sealed class ConsoleSink: ILogSink        // writes Console.Out / Console.Error
  public constructor();
  public method Emit(logEvent: LogEvent);
  destructor;
end ConsoleSink;
// A StreamSink(owned writer: TextWriter) — file logging — rides proposal 0006 (proposed dependency).

public sealed class ChannelLogger: ILogger       // sendable: minted per producer, moved into its domain
  // No public constructor; obtained from LogHub.NewLogger.
  public method IsEnabled(level: LogLevel): bool;
  public method Log(level: LogLevel, message: string,
                    fields: Array<LogField>?, cause: Exception?);   // sync Send; drops newest on full
  public readonly property DroppedCount: long;    // events dropped on a full channel (drop-newest)
  destructor;                                      // disposes its ChannelSender
end ChannelLogger;

public sealed class LogHub: IAsyncDisposable
  // Owns the receiving Channel<LogEvent> and the output sink in the composition-root
  // domain; RunAsync drains it until every minted logger is disposed (channel closes).
  public constructor(owned sink: ILogSink, capacity: int := 4096,
                     minLevel: LogLevel := LogLevel.Info);
  public method NewLogger(category: string): owned ChannelLogger;   // mints a sender under the hood
  public async method RunAsync(): Task;
  // await using only; DisposeAsync closes the channel and drains the backlog.
end LogHub;

public static class LogFormat
  public static method Line(logEvent: LogEvent): string;   // default single-line text rendering (§D7)
end LogFormat;
```

## 4. Usage

### 4.1 The simple path (single domain, direct console)

```bezel
import System.Diagnostics;

public static method Run()
  var log: ILogger := new ConsoleLogger("Orders", minLevel := LogLevel.Info);

  log.Info("service starting");
  log.Info($"order {orderId} placed",
           [LogField("order", orderId.ToString()), LogField("total", total.ToString("F2"))]);

  // Expensive fields: guard at the call site (there are no macros — §D4).
  if log.IsEnabled(LogLevel.Debug) then
    log.Debug($"cart = {cart}", BuildCartFields(cart));
  end;
end Run;
```

### 4.2 The scalable path (one sink domain, drop-newest under load)

```bezel
import System.Diagnostics;
import System.Threading;

public static async method Main(): Task<int>
  await using var hub := new LogHub(new ConsoleSink(), capacity := 4096, minLevel := LogLevel.Info);
  var pump := hub.RunAsync();   // start the sink loop on this domain; awaited after the producers

  // Each worker gets its own sendable logger, moved in at spawn (the sender travels;
  // the logger is domain-local thereafter — only the channel is shared, per §30.15).
  // Main itself holds NO logger, so the channel closes exactly when the workers finish.
  // Worker.Run is an async (Task-returning) entry taking an owned arg; that shape is admitted
  // by the stdlib §6.8 overload `Spawn<A>(entry: (owned A) -> Task, owned arg: A): owned Task`
  // (the async Domain.Spawn entries landed v0.179, spec §30.14) — no sync-entry reshape needed.
  using var w1 := Domain.Spawn(Worker.Run, hub.NewLogger("Worker-1"));
  using var w2 := Domain.Spawn(Worker.Run, hub.NewLogger("Worker-2"));
  await w1;
  await w2;                     // both done ⇒ their loggers disposed ⇒ last sender gone ⇒ channel closes
  await pump;                   // the sink loop drains the backlog and returns
  return 0;
end Main;                       // hub disposed: the sink is released

// in each spawned domain (static entry — capture-free, §30.14):
public static async method Run(owned log: ChannelLogger): Task
  using var logger := log;
  logger.Info("worker up");
  // ... work; logger.Info/.Error(...) Send LogEvents to the sink domain, never blocking ...
  logger.Info($"processed {n} items");
end Run;                        // logger disposed ⇒ its sender closes
```

### 4.3 First real customer — the 0002 HTTP server

The point this makes is the **wiring**: the server takes an `ILogger` by constructor from the composition root — no container, exactly the shape 0002 §2 uses for its dependencies (D-0001).

```bezel
import System.Diagnostics;
import System.Threading;             // Locked<T> (stdlib §6.11). Dictionary and Shared<T> are auto-imported (v0.289; stdlib §2.16).
import System.Net.Http;

// One sendable object per worker (spec §30.14: sealed, every field an owned sendable class).
// The logger is a constructor dependency, carried as an owned field — no container (D-0001).
public sealed class Deps
  public owned readonly Todos: Shared<Locked<Dictionary<int, Todo>>>;
  public owned readonly Logger: ChannelLogger;
  public constructor(owned todos: Shared<Locked<Dictionary<int, Todo>>>, owned logger: ChannelLogger)
    Todos := todos;
    Logger := logger;
  end constructor;
end Deps;

public sealed class Api: IHttpService
  private owned readonly _deps: Deps;                     // the root service owns the deps — and so the logger
  private owned readonly _routes: RouteTable := new();    // declared last: disposed before _deps (0002 §4.1)
  public property Routes: RouteTable => _routes;
  public constructor(owned deps: Deps)
    _deps := deps;
    _routes.Mount("/todos", new TodoApi(_deps.Todos.Clone(), _deps.Logger));   // a child that logs borrows the ILogger (0002 §4.1)
  end constructor;
end Api;

public static class Program
  public static async method Main(): Task<int>
    await using var hub := new LogHub(new ConsoleSink(), minLevel := LogLevel.Info);
    var pump := hub.RunAsync();                       // the sink loop, concurrent with the server
    using var todos := new Shared<Locked<Dictionary<int, Todo>>>(
        new Locked<Dictionary<int, Todo>>(new Dictionary<int, Todo>()));
    // `deps` runs on this domain once per worker; each result is moved into worker i and handed to
    // the service factory there (0002 §3, §4.2). Main holds no logger, so the channel closes with the last worker.
    var code := await HttpServer.Listen(port := 8080, service := lambda(owned deps: Deps) => new Api(move deps),
        deps := lambda(worker) => new Deps(todos.Clone(), hub.NewLogger($"http-{worker}")));   // serves until Stop()
    await pump;                                       // every worker's ChannelLogger disposed ⇒ channel closed ⇒ sink drains
    return code;
  end Main;
end Program;

Inside a handler the logger is a field of the service object (`_log.Error("create failed", cause := ex)` on a caught fault); a 0002 `Around` hook that catches a handler fault logs it through the same `cause` channel (`_deps.Log.Error(…, cause := ex)`), and the dispatcher's own 500 report is a §31.4 snapshot — one exception-snapshot discipline across both APIs. The precise sink-loop lifecycle (who runs `RunAsync`, and when the channel closes) is the subject of open question #2 — the shape above keeps `RunAsync` on the composition-root domain, which is the spec-canonical §30.15 arrangement (the receiving domain owns the channel).

## 5. Declined alternatives

1. **Serilog-style message-template auto-capture** (`log.Info("user {User} in", name)`) — introduces a second format-string convention (the String.Format decline, gap review §Declined #1) and reads field names out of a string literal at runtime (name-magic ⇒ reflection-shaped, rationale §18). Interpolation for the message + an explicit `Array<LogField>` for structure keeps both conventions Bezel already has (§D3).
2. **Typed structured-field values** (`LogField.Value: Object` or a value union) — boxing needs runtime type inspection at the sink (reflection-shaped); a closed union needs a sum-type construct Bezel lacks. Strings-after-formatting for v1; a typed `LogValue` recorded for when unions land (§D2).
3. **Anonymous-object fields** (`log.Info("…", { user, ip })`) — anonymous types are declined outright (DECLINED.md D-0002): structural shapes in a nominal system, uncrossable method boundaries, reflection-shaped habitat. Named `LogField` records are the sanctioned form.
4. **`SendAsync` (back-pressure) as the routing send** — awaiting capacity throttles the application when the sink is behind, and a guard held across the await risks a stall; logging must never do either. Synchronous `Send` + drop-newest (§D5).
5. **Drop-oldest / overwrite-on-full** — not expressible on today's `Channel<T>` (no evict operation), and the overwrite-on-full ring mode was declined for inverting ownership legibility (gap review §Declined #7). Drop-newest with a visible counter is what the current primitive supports soundly.
6. **Unbounded log channel** — a slow sink lets the backlog grow without limit; in a no-GC language that is a correctness risk, not a tuning knob (identical to 0014 §D5's reasoning and §6.9's bounded-channel rationale).
7. **A runtime-reconfigurable global logger** (`Locked<ILogger>` static) — a lock on every log read for a rarely-used capability; deferred (§D6 Fork C).
8. **`DI`-container-resolved logger** (`services.Resolve<ILogger>()`) — the container is process-global mutable state resolved at runtime and reflection-shaped (DECLINED.md D-0001). The logger is an ordinary constructor dependency.

## 6. Open questions for the owner

1. **The ambient fork (§D6).** Ship **Fork A only** (explicit passing) in v1, as recommended? Or also provide **Fork B** (a fixed `static readonly Log.Default: ILogger`, accepting the §22.12 rule-4 warning and the fixed-at-init constraint)? Fork C (`Locked<ILogger>`) is proposed declined.
2. **Sink-domain lifecycle.** `LogHub` keeps the `Channel<LogEvent>` in the composition-root domain and runs `RunAsync()` there (spec-canonical §30.15: the receiving domain owns the channel), which pairs the sink loop with the app under a `TaskScope`. The fully-isolated variant — the channel *moved into* a dedicated sink domain so the sink runs OS-parallel — requires **all senders minted before the move** (you cannot mint from a channel that has left). Accept the composition-root-runs-the-loop shape for v1, and add pre-minted-sender isolation only if a consumer needs it?
3. **Drop-counter aggregation.** v1 exposes `DroppedCount` per `ChannelLogger` (each domain-local). Global total (or a periodic sink-emitted "N dropped" summary) needs producers to communicate their counts — worth a v1 mechanism, or v2?
4. **Default format + JSON.** Ship only `LogFormat.Line` (text) in v1 and defer a JSON formatter to `LogEvent: ISerializable` (which rides 0010's `DateTime` conformance)? Or make the sink take a pluggable `(LogEvent) -> string` formatter from the start?
5. **`Warn` vs `Warning`.** The enum member is `Warning` (matches .NET); the convenience is `Warn` (shorter, matches most facades). Keep the asymmetry, or align both?

## Delegated rulings 2026-08-11 (stdlib-wave readiness; owner review in repo)

Resolving the five §6 questions under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo) (delegated ruling, 0012-precedent style). Status stays draft.

1. **Q1 — Fork A only in v1; Fork B is not shipped as stdlib surface.** Verified against the current statics rules (§22.12 as re-derived at v0.245, proposal 0087): `ILogger` declares neither the *synchronized* nor the *immutable* fact, so an interface-typed `static readonly` logger sits outside the derived warning-suppression set and draws rule 4's declaration-site warning **by construction**. A stdlib-shipped member whose canonical use warns by construction is the wrong default posture; explicit passing is the folder-wide stance (D-0001, 0002 §2.1's composition root). Fork B survives as a *documented user-side idiom* — any program may declare its own fixed `static readonly` logger and accept the warning knowingly — which is exactly what rule 4 is for; it just isn't stdlib surface. Fork C stays declined as drafted. **Evidence bar for revisiting B-as-stdlib:** sustained, recorded ergonomic pain threading loggers in the first real 0002-consumer programs.
2. **Q2 — composition-root-runs-the-loop: CONFIRMED for v1.** It is the spec-canonical §30.15 arrangement (the receiving domain owns the channel) and pairs the sink loop with the app's own `TaskScope` teardown. The fully-isolated variant (channel moved into a dedicated sink domain) is recorded as the v2 shape with its stated constraint — all senders minted before the move — and waits for a consumer that needs OS-parallel sink work.
3. **Q3 — drop-counter aggregation is v2.** Per-`ChannelLogger` `DroppedCount` is the honest v1: each counter is domain-local and race-free by construction. A global total is cross-domain mutable state — the exact surface the statics-×-domains rules restrict — and needs either a synchronized counter or sink-side aggregation; that design is done with the first consumer that actually alerts on drops, not speculatively.
4. **Q4 — `LogFormat.Line` only in v1; JSON rides `LogEvent: ISerializable` (0010); no pluggable formatter parameter.** A `(LogEvent) -> string` formatter stored by the sink is a §28.14 storage-bounded `scoped` callback with capture machinery — real surface for a need the two named formats cover. It is additive behind the same sink surface later; declined now as speculative. (The §D2 strings-after-formatting decision already bounds what a formatter could do, so nothing is foreclosed.)
5. **Q5 — align on the short form: the enum member becomes `Warn`.** One spelling per level, used by both the enum and the convenience methods (`Trace, Debug, Info, Warn, Error, Critical, None`). The proposal already chose `Info` over .NET's `Information`, so the short form is the established local convention; completing it beats preserving a one-off .NET match. Declined: keeping the asymmetry (two greppable names for one level — a discoverability tax with no payoff); aligning long (`Warning`/`Information` — abandons the already-chosen `Info`). The §3/§D4 enum listings are conformed in place.

Also recorded: the 2026-07-28 sweep's flag 3 (a target-typed `new(…)` in §4.3) is no longer present — the current §4.3 spells every construction with explicit `new`. The one element-position construction spelling (`[LogField("user", name)]`, §D3) is left for the graduation critique to verify against §15.4's literal-element grammar. With the rulings above, this proposal carries no open design question; the remaining path is the ordinary acceptance/graduation workflow.

## 7. Relationship to existing work

- **§30.13/§30.14/§30.15** — the sendability derivation of `LogEvent` and the channel routing are 100% existing language surface; this proposal adds **no spec changes**, only stdlib types.
- **§22.12** — the ambient-logger fork lives entirely inside the existing statics-and-domains rules (rule 2 forbids a reconfigurable global; rule 4 admits a fixed one with a warning).
- **§6.9 (bounded channels, v0.174)** / **proposal 0014** — the drop-newest-on-full policy and its counter are the same lossy-delivery discipline 0014 §D5 established for signals; logging reuses it wholesale.
- **§2.18/§18.5** — the message and the default line format are ordinary interpolation/`IFormattable`; no new formatting surface.
- **§31.1/§31.4** — the `cause` borrow → `ExceptionInfo` snapshot is the catch-arm-lifetime discipline, shared with 0002 §3.1's `OnError`.
- **Proposals 0002** (first consumer — server takes an `ILogger`, wires it at the composition root), **0003** (`Locked<T>` — the declined Fork C), **0006** (streams — the file sink rides `StreamWriter`), **0010** (`DateTime` serialization — the v2 JSON formatter).
- **Deferred metrics API** (gap review §Declined #3) is explicitly *not* here — counters are cross-domain mutable state pending the #1363 ruling; this proposal ships logging only, and a metrics surface belongs in an ecosystem package once `ILogger` + channels exist.
