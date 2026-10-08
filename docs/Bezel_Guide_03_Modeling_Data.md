# Bezel Guide 3: Modeling Data

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.326.*

Bezel gives you five ways to declare a data shape and two ways to select over one. This guide is the picking rule for each. The definitions live in the spec (§9 records, §10 enums, §11 classes, §19 selection, §35 choices), and the *why* behind the design is rationale §9.6. For choosing a **failure signal** (`T?` vs `Try` vs `Result` vs `requires`/exceptions), see Guide 2 §7 — that table is the home; this guide does not repeat it.

## 1. The five data kinds — which one?

| You have… | Use | Because |
|---|---|---|
| A closed set of named constants, no payloads | `enum` | Exhaustive `case` for free; serialization by name; nothing to own |
| Value-shaped data — compared by content, copied freely | `record` | Structural `=`/`GetHashCode` synthesized when components qualify; `with(…)` update unless the record is constructor-only (§9.9); no identity, no disposal |
| One-of-several alternatives **with payloads**, closed set | `choice` | Exhaustive `case` over the cases; value-like choices compare structurally; borrow-bearing ones do not (§35.7) and carry §9.7's lifetime rules; owning choices carry resources through the arms |
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
- **A cache field on a record.** Mark it `equality skip` — `private equality skip _cachedLen: int;` — and the structural `=`/hash package survives with the cache excluded (spec §9.2); the clause mirrors `serialized skip`. An *unannotated* cache field still makes comparison a loud compile error, never silent reference equality. Fill the cache in the **constructor**, not lazily: `this` is a borrow in every value-kind member body, so a store landing in the receiver's own storage is a compile error (§9.4) — a record has no place to memoize from.
- **A record that validates its own construction.** A `requires` or `ensures` clause on *any* declared constructor makes the record **constructor-only** (§9.9) — a §3.6 derived property, read off the declaration and never annotated. Two static consequences follow: the record is **not defaultable** (`default(R)`, an `R` field with no initializer that some constructor leaves unassigned, `[8]R` default slots, and `R` at a `where T: new()` substitution are all rejected, §17.11 — an `R` local is not: it takes ordinary definite assignment, §7.9), and **`with` is a compile error** on it (§9.5), because `with` runs no constructor and the copy would be a value with no constructor behind it — spell the constrained update as a named method whose body constructs the result. A *positional* record may not declare such a constructor at all: put the storage in body auto-properties so every constructor is declared, or state the condition on a **static factory method** and keep the record ordinary. Equality, hashing, and serialization are untouched — the property claims *provenance*, not validity.
- **Choice vs closed hierarchy.** A `choice` is data-first: a fixed case list, pattern-matched everywhere, no per-case methods. A `closed` hierarchy is behavior-first: each leaf is a class with its own members and can carry owned state, and `case` over the leaves is still exhaustive (§19.6) — for *non-generic* hierarchies; the carve is in §2 below. Reach for the hierarchy when cases have *behavior*; reach for the choice when cases are *shapes of data*.
- **Choice vs `T?`.** A choice with one meaningful payload and one empty case is what `T?` already provides — use `T?` (the recorded rule; DECLINED.md's `Option<T>` entry).
- **Generic over a kind.** The constraint set is `where T: class`, `where T: value`, `where T: notnull`, `where T: new()`, `where T: sendable`, `where T: hashable`, a base class, an interface, or another type parameter (spec §14.2). `where T: value` is the kind constraint for records, enums, choices, primitives, and inline arrays — the one a value-only generic wants, where `notnull` admits references too.

## 2. Selecting over a value — `(if …)`, `(case …)`

The two-sentence rule: a pick on **conditions** — two-way, or a ladder — is the conditional atom, `(if c then a elif c2 then b else z)`. **Dispatch on the shape or value of one scrutinee** is `(case … of …)`. In statement position the same constructs take their `end`-terminated statement forms.

```bezel
var max    := (if a > b then a else b);                       // two-way

var fee    := (if amount > 10_000m then 0m                     // condition ladder
               elif isPremium then 1m
               else 2.50m);

var label  := (case outcome of                                 // shape dispatch
  Fetched(var body) => $"got {body.Length} bytes",
  NotModified       => "cached",
  Failed(var code)  => $"error {code}");
```

- `elif` chains keep a ladder of any length inside the conditional atom, and in statement position a trailing `if`/`elif`/`else` whose every branch exits satisfies the every-exit-path rule (spec §21.7). Once arms stop being conditions-on-different-things and start being cases-of-one-thing, switch to `case`.
- `case` is the only form that *binds* — patterns destructure payloads (`Fetched(var body)`) and narrow types (`var c: Circle`). If you need the scrutinee's pieces, you want `case` regardless of arity.
- Exhaustiveness: `case` over an enum, choice, or closed hierarchy needs no `_` arm when every case is covered (§19.6) — prefer that. A `_` arm silences the compiler's help when the type grows. One carve: when a closed hierarchy's root or any leaf is *generic*, leaf coverage is not attempted — no arrangement of type-pattern arms is a static proof, and the `case` is completable only by an irrefutable arm (an unguarded `_` or unguarded untyped `var`); a value-shaped sum there is better modeled as a `choice`, whose exhaustiveness is counted by case name and needs no `_` (§19.6).
- Ownership in arms follows the meet rule (Guide 1 §8): arms must agree — all owned or all borrowed. A named owned binding as an arm is a borrow unless you mark it (`move x`, consume-if-selected).
- **Extracting by name from an owning match** (spec §19.3/§35.5): named slots read their field names at the pattern instead of by position, and in an owning match a named slot transfers exactly when you mark it — `move field = var x` hands the arm an owned binding, per slot, while unmarked named siblings stay borrows or copies, disposed with the construct. Same disposal story as the positional destructure; you just say which payload you are taking, by name.

```bezel
public choice PollOutcome                          // an owning choice: Ready carries a resource
  Ready(owned conn: Connection, attempts: int),
  Empty;
end PollOutcome;

case pool.Poll() of                                // owning match over the fresh outcome
  Ready(move conn = var c, attempts = var n) => do // c: owned Connection; n: int copy
    using var live := move c;                      // adopt: scope-bound disposal
    Console.WriteLine($"connected after {n} tries");
  end,
  Empty => ()
end;
```

## 3. Where the failure signal fits

One line, since Guide 2 §7 owns this: model the *result* with §1's kinds (`T?` for absence, `Result<T, E>` or a domain choice when the caller branches on the failure and its payload is a plain value shape), model the *contract* with `requires(…)` — remembering that on a **record** constructor that clause is also a modeling decision, making the record constructor-only (§1) — and keep exceptions for what neither models. The full table with the habits is Guide 2 §7.
