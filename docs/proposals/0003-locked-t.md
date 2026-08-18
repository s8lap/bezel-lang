# Proposal 0003 — `Locked<T>` and `RwLocked<T>`: data-owning locks for the synchronized set

| | |
|---|---|
| **Status** | graduated-v0.186 — **superseded — graduated in v0.186 (`3d08ec2`)**: stdlib Locked<T>/RwLocked<T> + spec §30.12/§22.12 rule 4/§28.13/§30.13/§30.14; the #1703 amendments written and landed with it. Open questions resolved as delegated rulings (recorded in the v0.186 changelog). *(prior: accepted (owner, 2026-07-12); draft (RFC))* |
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

  public method Lock(): owned LockGuard<T>;    // blocks; §30.12 discipline: see §4
  public async method LockAsync(): OwningTask<LockGuard<T>>;   // Pattern B (§30.5)
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
static readonly _sessions: Locked<OwningDictionary<string, Session>>
    := new(new OwningDictionary<string, Session>());

public static async method Touch(id: string): Task
  using var g := await _sessions.LockAsync();
  g.Value[id] := Session.Renewed(g.Value[id]);   // owning indexer-set disposes the displaced session
end Touch;                                       // guard disposed ⇒ lock released
```

`RwLocked<T>` is the read-heavy variant: `Read(): owned ReadGuard<T>` (shared, `Value` is a read-only borrow) and `Write(): owned WriteGuard<T>` (exclusive) plus the `Async`/`Try` forms. Same guard discipline.

**Both types join the §22.12 rule-2 synchronized set**, and their appearance as a `static readonly` also suppresses the rule-4 warning (the point of the warning — untied synchronization — no longer applies).

### 2.1 Interior-ownership condition and §28.13 synchronized-set membership

Two normative conditions make the type sound *and* useful beyond the static case.

**Interior must be transfer-closed (checked at instantiation).** `T`'s state must be **wholly owned** — transfer-closed-shaped — checked at `Locked<T>`/`RwLocked<T>` instantiation exactly as the sendable-class rule (§30.14) checks a class's fields. The reason is the whole point of the type: a `static readonly Locked<T>` is process-global and therefore reachable from *every* domain (§22.12: "Static state is process-global"); if `T`'s interior held *borrows* — a borrowing `Dictionary<string, Session>` whose class-typed value slots alias sessions owned elsewhere (§28.11 slot table) — a guard taken in domain B could chase that borrow into domain A's heap *outside any lock*, which is precisely the cross-domain data race the lock exists to prevent. Owning collections (`OwningDictionary`, `OwningSet`, …) and classes with only `owned`/value/`string` fields satisfy the condition; a borrowing collection of class elements does not, and `Locked<BorrowingDictionary<…>>` is rejected at instantiation with a diagnostic naming the offending slot category. (This is also why the §2 worked example uses `OwningDictionary` rather than `Dictionary` — the owning indexer-set both stores the fresh session *and* disposes the displaced one, which is the semantics the example wants; a fresh owned `Session.Renewed(…)` stored into a borrowing slot would leak, §28.6.)

**`Locked<T>`/`RwLocked<T>` join the §28.13 synchronized set (so `Shared<Locked<T>>` is sendable).** §22.12 rule 2 (mutable-static admission) is not enough on its own: the *non-static* shared-state case — a session map owned by a server object and shared with worker domains — needs the lock to cross a domain boundary inside a `Shared<T>`. §28.13 makes `Shared<T>` sendable iff `T` is "internally synchronized" (the compiler-known set: `Mutex`, `Atomic<T>`, `AtomicInteger<T>`, `Semaphore`). `Locked<T>` and `RwLocked<T>` **join that set**, so `Shared<Locked<T>>` is sendable **iff `T` meets the transfer-closed-interior condition above** — the interior is reached only under the lock, so applying the §30.14 class-field rules to `T` is exactly the admission the cross-domain case needs. This is what gives the whole proposal a home for "mutation of shared state" in *both* the static and the object-owned-shared cases; a sendable `Mutex` alone has had nothing in-language to protect.

**Parallel reads across domains.** `RwLocked<T>` read guards held **concurrently by multiple domains** (writer-excluded, unsynchronized reads) are the intended cross-domain *read-parallel* primitive — the shared-state complement to compute-parallel combinators (proposal 0032 cross-links here). Multiple `Read()` guards may be live at once across domains; a `Write()` guard excludes all of them.

## 3. Why this shape

1. **One type lifts any interior whose state is wholly owned.** The alternative — enumerating `ConcurrentDictionary`, `ConcurrentSet`, … — grows the stdlib N types for N shapes and still leaves user classes unshareable. `Locked<T>` composes: `Locked<OwningDictionary<K,V>>` today, `Locked<MyRegistry>` tomorrow — the interior is required to be transfer-closed (§2.1 below), so `T` must be an owning collection or a class whose state is wholly owned, not a *borrowing* collection whose slots alias elements owned elsewhere. (A tuned lock-free `ConcurrentDictionary` remains a possible *optimization* later; it is not the foundation.)
2. **The compiler enforces the lock discipline.** `g.Value` borrows from the guard; storing that borrow anywhere that outlives the `using var` scope is already a §28.3 escape error. Rust needed lifetimes to get this guarantee; Bezel's borrow rules deliver the same for free.
3. **It writes down the DI answer** (owner conversation, 2026-07-03). The "DI container as global state store" alternative is declined: a container's singleton table *is* a process-global mutable map — the same state made less visible, resolved at runtime (`Resolve<T>()` fails at request time where a constructor parameter fails at compile time), and reflection-shaped (Bezel has no member-access reflection, rationale §18). Dependency *wiring* in Bezel is constructor parameters from a composition root — strictly more expressive than container DI because `owned`/borrow annotations state the lifetime contract a container cannot express. When state must genuinely be ambient-and-mutable, `Locked<T>` as a synchronized static is the visible, compiler-checked spelling of what a DI singleton hides. The lifetime taxonomy maps without a framework: singleton → composition-root local or synchronized static; scoped/per-request → handler locals; transient → construct it.

## 4. The hard question: guards across `await`

A guard held across a suspension point is the design's one genuine hazard: the continuation may resume on the same domain after other continuations ran (starvation-adjacent), and two domains lock-stepping through `LockAsync` can deadlock on lock-ordering just like threads. Proposed rules, for the owner to ratify:

1. **`Lock()` (blocking) inherits the now-landed §30.12 blocking-acquisition discipline (v0.179).** This rule shipped in v0.179 for `Mutex`/`Semaphore` `Acquire()`, and `Locked.Lock()` is the same shape, so it is not proposed here — it is inherited: the compiler **rejects the blocking form lexically inside `async` methods and `async` lambdas** (there `LockAsync` is strictly superior), and where static rejection cannot see the call (a non-`async` helper reached from a continuation) the runtime raises `InvalidOperationException` **when the acquisition would actually park a thread inside a domain continuation** — an immediately-available `Lock()`, and any `Lock()` on the startup thread or an FFI foreign thread (both outside any domain), proceeds. Cross-domain locking from async code uses `Shared<Locked<T>>` with `Get().LockAsync()` (§2.1, mirroring §28.13's `Shared<Mutex>.Get().AcquireAsync()`).
2. **Holding a guard across `await` is legal but discouraged by construction**: the guard is a class object owned by a local, so nothing *forbids* `using var g := await x.LockAsync(); await Other();` — but the docs teach the narrow-scope idiom, and a lint (warning: guard live across `await`) is cheap and proposed on by default. Forbidding outright would require new spec surface (a "no-suspend type" concept) — deferred unless the lint proves insufficient.
3. **No lock-ordering machinery** (deadlock between two `Locked` values): out of scope, as everywhere else; the actor idiom remains the recommendation when ordering gets complicated.

## 5. Implementation sketch

Runtime: `bz_locked_t` = pthread mutex (rwlock for `RwLocked`) + interior pointer; `LockAsync` parks the continuation on a waiter list woken at guard disposal (the same continuation-queue machinery channels use). Guard disposal = unlock + (in debug) poison the `Value` borrow. Compiler: the two types join the §22.12 synchronized-set table (one-line each after #1363's checker), `LockGuard.Value` types as a borrow with the guard as source (existing borrow-source plumbing). Estimated size: runtime ~150 lines, sema table rows, codegen property lowering — a single bounded lane once the design is ratified.

## 6. Open questions for the owner

1. Ratify §4's three rules (blocking-`Lock` guard, lint-not-ban for guard-across-await, no ordering machinery)?
2. `where T: class` initially (interior is an owned class object). Value-typed interiors (`Locked<int>`) are subsumed by `Atomic<T>` — keep the constraint, or admit records?
3. Names: `Locked<T>`/`RwLocked<T>` vs Rust's `Mutex<T>`/`RwLock<T>` — the stdlib already has a standalone `Mutex`, so reusing the name would collide; `Locked` states the relationship ("a locked T"). Alternatives welcome.

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

The three §6 owner questions are resolved as follows; graduation still requires the §22.12 rule-2 table amendment and the §28.13 synchronized-set amendment (both flagged in-proposal), which land through the normal docs workflow.

- **Q1 — the §4 rules: ratified as stated.** Rule 1 (blocking `Lock()`) is no longer a proposal — it is *inherited* from the §30.12 blocking-acquisition discipline that already landed in v0.179 (lexical async-body reject + runtime `InvalidOperationException` on a would-park continuation); `Locked.Lock()` is the same shape and needs no new machinery. Rule 2 (guard-across-`await` is **lint, not ban**) is ratified: forbidding it outright would need a new "no-suspend type" spec concept, disproportionate to the hazard; the on-by-default lint is the right cost/benefit. Rule 3 (**no lock-ordering machinery** in v1) is ratified — deadlock between two `Locked` values is out of scope, as elsewhere; the actor idiom remains the recommendation when ordering gets complicated.
- **Q2 — keep `where T: class` for v1.** Value-typed interiors (`Locked<int>`) are subsumed by `Atomic<T>` (§30.12); admitting records would require a copy-in/copy-out receiver story that the language does not yet settle and that scalars already have. Revisit on a concrete demand.
- **Q3 — keep `Locked<T>`/`RwLocked<T>`.** The collision with the stdlib's standalone `Mutex` makes reusing Rust's `Mutex<T>`/`RwLock<T>` names decisive against them, and `Locked`/`RwLocked` state the data-owning relationship ("a locked `T`") rather than naming the mechanism.

Accepted into the stdlib-amendment queue behind the #1363-family runtime-sync work (#1391/#1392) whose machinery it shares.

## 7. Relationship to existing work

§22.12/#1363 (the set this joins) · #1391/#1392 (statics runtime-sync residues — rule 1 matters for the static-initializer story) · #1362 (the blocking-guard discipline this reuses) · gap review #1403 items 6/7/15/16 (channels/cancellation/task-groups — complementary, not overlapping) · proposal 0002 §4.1 (the server's shared-state story becomes "Locked<T> or the actor idiom").
