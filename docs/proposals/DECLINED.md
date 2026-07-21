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

## D-0002 — Anonymous types (C#-style ad-hoc object shapes)

**Declined 2026-07-03 (owner conversation on data projections).** Three structural grounds:

1. **They require structural typing inside a nominal system** — two same-shaped anonymous types must unify, which imports a second typing discipline for one convenience.
2. **They cannot cross method boundaries** — C#'s own recorded limitation, mitigated there only via `object`/`dynamic`, neither of which Bezel will have. A shape that cannot be returned, stored, or passed is a local-only convenience…
3. **…whose killer habitat is reflection.** Anonymous types earn their keep in ORM expression trees that read property names off the shape at runtime — machinery Bezel deliberately lacks (rationale §18). C# itself largely migrated to *named* ValueTuples once available.

**The accepted remedy** is method-local `record` declarations (tracked separately): the shape lives next to its single use, keeping the recorded records-over-tuples stance ("a name and a documented shape is a feature") while removing the distance cost. Cross-type mapping stays explicit (constructors/factory methods; `with` for same-type variants) — automatic mapping is reflection-shaped and would be reconsidered only as compile-time generation, per D-0001's wiring posture. Truly dynamic data goes through the JSON DOM (gap review item 21): runtime-shaped values behind typed faults, quarantined at the parse boundary, never a `dynamic` type.

**What would reopen this:** tuples being reopened first (the recorded deferral names evidence-from-real-code as the bar) — named tuples would then be re-weighed against anonymous types, and would still win.

---

## Gap-review declines (2026-07)

Eight further declined-with-reasons entries — metrics API (pending #1363), BigInteger (rides crypto), blocking process waits (§30 discipline), `CurrentDirectory` setter (fights domain isolation), `String.Format` (second format convention), compile-time platform branching (language change), overwrite-on-full ring buffer (inverts ownership legibility), implicit Unicode normalization in equality (§9.6 ordinal contract) — are recorded in [the gap review](stdlib-gap-review-2026-07.md) §"Declined".

---

## D-0003 — Unifying single-statement member/lambda bodies on `do … end` (#745)

**Declined 2026-07-07 (delegated ruling, 2026-07 spec/design run; owner review pending).** The draft RFC (#745) proposed one uniform `do … end` spelling for every single-statement body position (members, lambdas, arms). Three grounds:

1. **The load-bearing seam is already closed.** The v0.170 uniform-closer revision (#1374) fixed the actual inconsistency the RFC was written against — every block now closes with `end;` or a labelled repeat of its introducer. What remains is a *spelling triad* (`=> expr` arrows, `do … end` blocks, full member bodies) whose members are each locally consistent and non-overlapping; unifying them buys no rule deletions in the grammar, only migration.
2. **The churn cost has inverted.** When the RFC was drafted the compiler was young; today ~10,500 tests, the conformance corpus, both guides, and every proposal encode the current spellings. A whole-corpus migration to retire zero rules is the definition of churn.
3. **The surviving argument depends on a declined feature.** The RFC's strongest remaining case (arm bodies as uniform blocks) reads naturally only under block-*expressions*, which rationale §5.13 declines on recorded grounds; without them the unified form is strictly more ceremony in the common case.

**What would reopen this:** block expressions being reopened first (their own register entry governs), or field evidence that the triad measurably confuses users — not re-asking.

---

## D-0004 — `Option<T>` in the standard library

**Declined 2026-07-19 (0041 graduation, v0.200).** `T?` is Bezel's option type: it has language-level syntax (`?.`, `??`, narrowing, the §19.6 nullable clause), stdlib-wide integration (the Try shapes, `FirstOrNull`), and a design lineage the rationale defends throughout. Shipping `Option<T>` beside the `choice` kind would create two spellings of absence, two conversion questions at every API boundary, and an eternal style war. A choice with one meaningful payload and one empty case *is* what `T?` already provides. Revisiting requires evidence that `T?` structurally cannot serve where `Option<T>` could — not preference.

**Declined 2026-07-21 (expression-forms review, post-v0.214).** Two paren-atom candidates, each by the admission test recorded in proposal 0061: **a value-form try expression** (`(try E catch … )` / `(try E else fallback)`) — fails test 3: it makes exception-based *expected* failure ergonomic, precisely the path Guide 2 §6 routes to `T?`/Try/`Result`; the doctrine would be undermined by its own syntax, and the borrow-scoped catch model (§31) offers no owned exception value to carry anyway. **Comprehensions** (`(for x in xs where p => f(x))`) — fail test 1: pure sugar over the lazy-query operators (`xs.Where(p)…`, affine per §29.11), whose ownership story is already settled; also a loop expression, which the owner separately declined absent unique capability. Revisiting either requires new evidence against these reasons.

