# Proposal 0003 — `Locked<T>` and `RwLocked<T>`: data-owning locks for the synchronized set

| | |
|---|---|
| **Status** | Draft for owner review |
| **Origin** | Owner conversation 2026-07-03 ("do we need more synchronized statics for real programs?") + gap-review items 6/7/15/16 context |
| **Design influences** | Rust `Mutex<T>`/`RwLock<T>` (the mutex *owns* the data; access only through a scoped guard); **not** .NET/Java standalone monitors (lock-next-to-data by convention) |
| **Depends on** | §22.12 statics rules (v0.164), the borrow-lifetime machinery (§28.3), #1391 (static-init sync) for the static use case to be airtight |

## 1. The problem

§22.12 rule 2 admits a mutable static only when its type is compiler-known internally-synchronized: `Mutex`, `Semaphore`, `Atomic<T>`, `AtomicInteger<T>`, `Channel<T>`, `ChannelSender<T>`. That covers signaling, scalars, and pipelines — but the thing real services share is **structured state**: a session map, a cache, a registry. Today's options are the actor idiom (a state-owning domain + channel — principled, but heavy ceremony for a cache) or a `static readonly` collection "guarded" by a *separate* static `Mutex` — which rule 4 rightly warns about, because the mutex is not **tied** to the data; the discipline is convention, and convention is exactly what §30.14 exists to replace with checking.

## 2. The design

A lock that owns its interior. Access is only possible through a scoped guard, and the guard's interior access is a **borrow whose lifetime is the guard's scope** — so the borrow-escape rules the language already has (§28.3) enforce the critical section. No new checking machinery: the compiler rule that stops a `using var` resource from leaking its borrows is the same rule that stops a reference from escaping the lock.

```bezel
public class Locked<T> where T: class
  public constructor(owned value: T);          // the lock owns the interior

  public method Lock(): owned LockGuard<T>;    // blocks; §30 discipline: see §4
  public async method LockAsync(): Task<owned LockGuard<T>>;
  public method TryLock(): owned LockGuard<T>?;   // shape (b) of the Try convention

  destructor;                                   // disposes the interior
end Locked;

public class LockGuard<T>
  public readonly property Value: T;            // borrow; lifetime = the guard's scope
  destructor;                                   // releases the lock
end LockGuard;
```

Usage — the `using var` scope *is* the critical section:

```bezel
static readonly _sessions: Locked<Dictionary<string, Session>>
    := new(new Dictionary<string, Session>());

public static async method Touch(id: string): Task
  using var g := await _sessions.LockAsync();
  g.Value[id] := Session.Renewed(g.Value[id]);
end Touch;                                       // guard disposed ⇒ lock released
```

`RwLocked<T>` is the read-heavy variant: `Read(): owned ReadGuard<T>` (shared, `Value` is a read-only borrow) and `Write(): owned WriteGuard<T>` (exclusive) plus the `Async`/`Try` forms. Same guard discipline.

**Both types join the §22.12 rule-2 synchronized set**, and their appearance as a `static readonly` also suppresses the rule-4 warning (the point of the warning — untied synchronization — no longer applies).

## 3. Why this shape

1. **One type lifts any interior.** The alternative — enumerating `ConcurrentDictionary`, `ConcurrentSet`, … — grows the stdlib N types for N shapes and still leaves user classes unshareable. `Locked<T>` composes: `Locked<Dictionary<K,V>>` today, `Locked<MyRegistry>` tomorrow. (A tuned lock-free `ConcurrentDictionary` remains a possible *optimization* later; it is not the foundation.)
2. **The compiler enforces the lock discipline.** `g.Value` borrows from the guard; storing that borrow anywhere that outlives the `using var` scope is already a §28.3 escape error. Rust needed lifetimes to get this guarantee; Bezel's borrow rules deliver the same for free.
3. **It writes down the DI answer** (owner conversation, 2026-07-03). The "DI container as global state store" alternative is declined: a container's singleton table *is* a process-global mutable map — the same state made less visible, resolved at runtime (`Resolve<T>()` fails at request time where a constructor parameter fails at compile time), and reflection-shaped (Bezel has no member-access reflection, rationale §18). Dependency *wiring* in Bezel is constructor parameters from a composition root — strictly more expressive than container DI because `owned`/borrow annotations state the lifetime contract a container cannot express. When state must genuinely be ambient-and-mutable, `Locked<T>` as a synchronized static is the visible, compiler-checked spelling of what a DI singleton hides. The lifetime taxonomy maps without a framework: singleton → composition-root local or synchronized static; scoped/per-request → handler locals; transient → construct it.

## 4. The hard question: guards across `await`

A guard held across a suspension point is the design's one genuine hazard: the continuation may resume on the same domain after other continuations ran (starvation-adjacent), and two domains lock-stepping through `LockAsync` can deadlock on lock-ordering just like threads. Proposed rules, for the owner to ratify:

1. **`Lock()` (blocking) follows the §30.3/#1362 discipline**: legal on the main thread's synchronous prefix and inside plain synchronous methods; a blocking `Lock()` *inside a domain continuation* raises `InvalidOperationException` via the same guard machinery as `Wait()`/`Result` (the runtime knows it is on a domain). Async code uses `LockAsync`.
2. **Holding a guard across `await` is legal but discouraged by construction**: the guard is a class object owned by a local, so nothing *forbids* `using var g := await x.LockAsync(); await Other();` — but the docs teach the narrow-scope idiom, and a lint (warning: guard live across `await`) is cheap and proposed on by default. Forbidding outright would require new spec surface (a "no-suspend type" concept) — deferred unless the lint proves insufficient.
3. **No lock-ordering machinery** (deadlock between two `Locked` values): out of scope, as everywhere else; the actor idiom remains the recommendation when ordering gets complicated.

## 5. Implementation sketch

Runtime: `bz_locked_t` = pthread mutex (rwlock for `RwLocked`) + interior pointer; `LockAsync` parks the continuation on a waiter list woken at guard disposal (the same continuation-queue machinery channels use). Guard disposal = unlock + (in debug) poison the `Value` borrow. Compiler: the two types join the §22.12 synchronized-set table (one-line each after #1363's checker), `LockGuard.Value` types as a borrow with the guard as source (existing borrow-source plumbing). Estimated size: runtime ~150 lines, sema table rows, codegen property lowering — a single bounded lane once the design is ratified.

## 6. Open questions for the owner

1. Ratify §4's three rules (blocking-`Lock` guard, lint-not-ban for guard-across-await, no ordering machinery)?
2. `where T: class` initially (interior is an owned class object). Value-typed interiors (`Locked<int>`) are subsumed by `Atomic<T>` — keep the constraint, or admit records?
3. Names: `Locked<T>`/`RwLocked<T>` vs Rust's `Mutex<T>`/`RwLock<T>` — the stdlib already has a standalone `Mutex`, so reusing the name would collide; `Locked` states the relationship ("a locked T"). Alternatives welcome.

## 7. Relationship to existing work

§22.12/#1363 (the set this joins) · #1391/#1392 (statics runtime-sync residues — rule 1 matters for the static-initializer story) · #1362 (the blocking-guard discipline this reuses) · gap review #1403 items 6/7/15/16 (channels/cancellation/task-groups — complementary, not overlapping) · proposal 0002 §4.1 (the server's shared-state story becomes "Locked<T> or the actor idiom").
