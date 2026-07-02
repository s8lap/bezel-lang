# Declined proposals — the register

Design directions considered for the standard library / platform and **declined with reasons**, so the question stays settled rather than perennially reopened (the same discipline as the rationale's decline records, applied to the proposals folder). Revisiting an entry requires new evidence against the stated reasons, not re-asking.

---

## D-0001 — A dependency-injection container (runtime service resolution)

**Declined 2026-07-03 (owner-confirmed).** Full reasoning in [proposal 0003 §3.3](0003-locked-t.md); summary:

DI does two jobs, and Bezel keeps one while declining the other.

- **Wiring** (constructor injection) needs no framework: it is constructor parameters from a composition root, and Bezel's `owned`/borrow annotations make the dependency contract *more* expressive than any container can (`constructor(owned db: Database)` states who owns the dependency; no container expresses lifetime-of-ownership at all).
- **The container** (`AddSingleton`/`Resolve<T>()`) is declined on three grounds:
  1. **It is the same global state, hidden.** A container's singleton table *is* a process-global mutable `Dictionary<Type, object>`; every §22.12/§30.14 data-race hazard of a mutable static applies identically — ASP.NET's own "singleton services must be thread-safe" rule is §22.12 enforced by documentation instead of a compiler. A `static readonly x: Locked<T>` states the sharing, the type, and the synchronization in one visible line.
  2. **It trades compile-time for runtime.** Service-locator resolution fails at request time ("no registration for `ICache`"); a constructor parameter fails at compile time. Bezel's design consistently moves this class of error to compile time (e.g. sendability checked at route registration, proposal 0002 §4.1).
  3. **The machinery is reflection-shaped.** Runtime containers reflect over constructors; Bezel deliberately has no member-access reflection (rationale §18) — a foundational decline this would reopen.

The lifetime taxonomy maps without a framework: *singleton* → composition-root local passed down, or a §22.12 synchronized static (`Locked<T>`) when genuinely ambient; *scoped/per-request* → handler locals; *transient* → construct it.

**What would reopen this:** large real codebases demonstrating that hand-written composition roots do not scale — and the remedy would then be **compile-time generated wiring** (Dagger/source-generator style), a tooling project, never a runtime container.

---

## Gap-review declines (2026-07)

Eight further declined-with-reasons entries — metrics API (pending #1363), BigInteger (rides crypto), blocking process waits (§30 discipline), `CurrentDirectory` setter (fights domain isolation), `String.Format` (second format convention), compile-time platform branching (language change), overwrite-on-full ring buffer (inverts ownership legibility), implicit Unicode normalization in equality (§9.6 ordinal contract) — are recorded in [the gap review](stdlib-gap-review-2026-07.md) §"Declined".
