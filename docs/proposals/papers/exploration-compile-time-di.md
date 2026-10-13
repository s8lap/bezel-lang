# Exploration — compile-time wiring generation ("Dagger for Bezel")

| | |
|---|---|
| **Status** | Exploration — *not* a proposal; recorded so the shape is thought through before it is ever needed |
| **Origin** | DECLINED.md D-0001 names compile-time generated wiring as the only acceptable future remedy if hand-written composition roots stop scaling; owner asked (2026-07-03) what that remedy would look like |
| **Verdict up front** | Feasible, and Bezel's ownership model makes it *better-founded* than Dagger — but the trigger condition has not occurred, and two of the three delivery mechanisms are blocked on deliberate language declines. Keep in mind; build nothing. |

## 1. What the problem will actually be (when it occurs)

Hand-written composition roots have one real scaling pain, and it is not line count — it is **ripple**: adding a dependency to a service in the middle of the graph edits every constructor and construction site on the path from the root. Dagger/.NET-source-gen DI exists to absorb that ripple by *generating* the code a human would have written. Everything else containers do (lifetimes, scopes) Bezel already covers structurally (D-0001).

The trigger to revisit this exploration: real Bezel programs whose roots exceed a few hundred lines **and** whose maintainers report ripple as a recurring cost. Neither exists yet.

## 2. The Bezel-native insight: the composition root is an ownership tree

This is where Bezel is *better* ground for generated wiring than Java or C#:

- A generated root is just a class whose services are **owned fields**, built in dependency order in the constructor, exposed as borrows:

```bezel
// GENERATED — bezel-wire output (sketch)
public class AppRoot
  private owned _config: Config;
  private owned _db: PostgresDatabase;
  private owned _cache: RedisCache;          // borrows _db internally

  public constructor()
    _config := new Config();
    _db := new PostgresDatabase(_config);     // borrow: root outlives db's use
    _cache := new RedisCache(_db);
  end constructor;

  public readonly property Db: Database => _db;        // borrows out
  public readonly property Cache: ICache => _cache;
  destructor end destructor;                  // owned-field cascade (§28.4)
end AppRoot;
```

- **Teardown order is free and provably correct**: §28.4's owned-field disposal runs in reverse declaration order, and declaration order *is* the topological order the generator emitted. Dagger has no story for disposal ordering at all; .NET's container disposes registered `IDisposable`s in an order you hope is right. Here the language guarantees it.
- **The graph's edges are typed by the ownership annotations**: a constructor taking `owned db` means the service *subsumes* the dependency (it moves into the service, not the root); a plain parameter means the root retains ownership and the service borrows. Dagger's `@Singleton` vs unscoped distinction falls out of `owned`-placement — no annotations, the signatures already say it.
- **Sendability composes**: a generated root handed to proposal-0002 handlers passes through the §30.14 capture check like any other value; a generator cannot create a data race the compiler wouldn't catch.

## 3. Three delivery mechanisms, two blocked

| Mechanism | Shape | Verdict |
|---|---|---|
| **(a) External generator tool** (`bezel-wire`) | A separate binary reads the project, finds the requested service set (from a manifest file — *not* annotations), topo-sorts constructors, and emits `AppRoot.bz` as ordinary, reviewable, checked-in Bezel source. Errors in the graph (cycles, missing providers) are the tool's diagnostics; errors in usage are ordinary compile errors in generated code. | **The viable path.** Needs zero language changes. Generated-source-you-review matches the project's explicitness ethos. Its weakness is honest: diagnostics live in a second tool, and the generated file must be regenerated on signature changes (a build-step question for a build system that doesn't exist yet — sequencing dependency on #1398's build-driver work). |
| **(b) Annotation-driven, Dagger-style** | `@Inject`/`@Provides` markers read by the compiler. | **Blocked**: Bezel deliberately has no attribute system (deferred, rationale §18), and reintroducing one *for DI* would invert the priority order — attributes need to earn existence on stronger grounds first. |
| **(c) First-class language construct** | A `composition ... end` block the compiler expands (constructor topo-sort + class synthesis in the compiler itself). | **Premature**: a language feature for a pain no program has yet felt violates the evidence bar every deferred feature in §18 is held to. Recorded as the eventual shape *if* (a) succeeds and the tool's manifest becomes the bottleneck. |

## 4. What the manifest could look like (mechanism (a), sketch)

No annotations — a plain file the tool reads, naming the root and the concrete bindings (the only facts a human must supply; everything else is in the constructors):

```
# wire.toml (sketch)
root = "AppRoot"
[bindings]                 # interface -> concrete, the human decisions
"Database" = "PostgresDatabase"
"ICache"   = "RedisCache"
```

The tool resolves constructor parameter types against bindings and other constructibles, reports cycles/ambiguities/missing bindings with graph-aware messages, and writes the root class. Scoped variants (a per-request `RequestScope` class owned by a handler) generate the same way — a smaller class, owned per call.

## 5. Keep-in-mind list (what today's decisions should preserve)

1. **Constructor signatures stay the single source of truth** — nothing in the stdlib or proposals should introduce initialization patterns that bypass constructors (setter injection, init-blocks), or the generator loses its input.
2. **`owned`-placement as scope declaration** (§2 above) is the design's keystone — the #1348 call-site markers made these edges *visible*, which incidentally makes generated wiring reviewable. Keep transfer explicit.
3. **#1398 (build driver)** is the natural host for a generate step; when it is designed, leave a pre-compile hook slot.
4. **Do not add attributes for this.** If attributes ever arrive, they arrive on their own merits.

## 6. Relationship

DECLINED.md D-0001 (this is its named remedy, explored) · proposal 0002 §2.1 (composition-root usage) · proposal 0003 (`Locked<T>` — what generated singletons are made of when mutable) · #1398 (build driver — sequencing) · rationale §18 (attributes deferral, evidence bar).
