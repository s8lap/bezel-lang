# Bezel Guide 3: Modeling Data

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.246.*

Bezel gives you five ways to declare a data shape and three ways to select over one. This guide is the picking rule for each. The definitions live in the spec (§9 records, §10 enums, §11 classes, §19 selection, §35 choices), and the *why* behind the design is rationale §9.6. For choosing a **failure signal** (`T?` vs `Try` vs `Result` vs `requires`/exceptions), see Guide 2 §7 — that table is the home; this guide does not repeat it.

## 1. The five data kinds — which one?

| You have… | Use | Because |
|---|---|---|
| A closed set of named constants, no payloads | `enum` | Exhaustive `case` for free; serialization by name; nothing to own |
| Value-shaped data — compared by content, copied freely | `record` | Structural `=`/`GetHashCode` synthesized when components qualify; `with(…)` update; no identity, no disposal |
| One-of-several alternatives **with payloads**, closed set | `choice` | Exhaustive `case` over the cases; value-like choices compare structurally; owning choices carry resources through the arms |
| A stateful thing with identity, behavior, or an owned resource | `class` | Reference semantics, `owned` fields, destructor cascade, virtual dispatch |
| An open-within-the-module family dispatched by type | `closed` class hierarchy | §19.6 exhaustiveness over the leaves; per-leaf payload and behavior |

Worked one-liners, all real:

```bezel
public enum Status
  Active, Draining, Stopped;
end Status;

public record Point(X: int, Y: int);              // structural =, with(…)
var p2 := p1 with(Y := 99);

public choice FetchOutcome                        // closed alternatives, payloads
  Fetched(body: string),
  NotModified,
  Failed(code: int);
end FetchOutcome;

public class Session                               // identity + owned resource
  private owned _log: FileStream;
  // … constructor, methods, disposal cascade
end Session;
```

Boundary calls that come up in practice:

- **Enum vs choice.** The moment one alternative needs a payload, it is a `choice`. Do not encode payloads beside an enum in parallel fields — the compiler cannot tie them together. A `choice` case carries its payload, and the `case` arms bind it.
- **Record vs class.** If two instances with the same components should be *the same value* — dictionary keys, messages, coordinates — it is a `record`. If it has a lifecycle (opened, mutated, disposed) or identity matters, it is a `class`. A record cannot hold an owned resource; that is the design, not a limitation (spec §28.5 — copy-semantics holders cannot carry disposal obligations).
- **A cache field on a record.** Mark it `equality skip` — `private equality skip _cachedLen: int;` — and the structural `=`/hash package survives with the cache excluded (spec §9.2); the clause mirrors `serialized skip`. An *unannotated* cache field still makes comparison a loud compile error, never silent reference equality.
- **Choice vs closed hierarchy.** A `choice` is data-first: a fixed case list, pattern-matched everywhere, no per-case methods. A `closed` hierarchy is behavior-first: each leaf is a class with its own members and can carry owned state, and `case` over the leaves is still exhaustive (§19.6). Reach for the hierarchy when cases have *behavior*; reach for the choice when cases are *shapes of data*.
- **Choice vs `T?`.** A choice with one meaningful payload and one empty case is what `T?` already provides — use `T?` (the recorded rule; DECLINED.md's `Option<T>` entry).

## 2. Selecting over a value — `(if …)`, `(when …)`, `(case …)`

The three-sentence rule: a **two-way** pick on one condition is the conditional atom. A **ladder of conditions** is `(when …)`. **Dispatch on the shape or value of one scrutinee** is `(case … of …)`. In statement position the same constructs take their `end`-terminated statement forms.

```bezel
var max    := (if a > b then a else b);                       // two-way

var fee    := (when                                            // condition ladder
  amount > 10_000m => 0m,
  isPremium        => 1m,
  _                => 2.50m);

var label  := (case outcome of                                 // shape dispatch
  Fetched(var body) => $"got {body.Length} bytes",
  NotModified       => "cached",
  Failed(var code)  => $"error {code}");
```

- `elif` chains keep a short ladder inside the conditional atom (`(if c1 then a elif c2 then b else d)`). Once arms stop being conditions-on-different-things and start being cases-of-one-thing, switch to `case`.
- `case` is the only form that *binds* — patterns destructure payloads (`Fetched(var body)`) and narrow types (`var c: Circle`). If you need the scrutinee's pieces, you want `case` regardless of arity.
- Exhaustiveness: `case` over an enum, choice, or closed hierarchy needs no `_` arm when every case is covered (§19.6) — prefer that. A `_` arm silences the compiler's help when the type grows.
- Ownership in arms follows the meet rule (Guide 1 §8): arms must agree — all owned or all borrowed. A named owned binding as an arm is a borrow unless you mark it (`move x`, consume-if-selected).

## 3. Where the failure signal fits

One line, since Guide 2 §7 owns this: model the *result* with §1's kinds (`T?` for absence, `Result<T, E>` or a domain choice when the caller branches on the failure and its payload is a plain value shape), model the *contract* with `requires(…)`, and keep exceptions for what neither models. The full table with the habits is Guide 2 §7.
