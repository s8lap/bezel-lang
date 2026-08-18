# Proposal 0097 — Superseded-exception snapshots: recovering the lost diagnostics without moving the in-flight-slot model

| | |
|---|---|
| **Status** | accepted — ruled under the owner's 2026-08-18 delegation; awaiting graduation train. Charter ruling C3: **adopt** snapshot recording of superseded exceptions on the existing `ExceptionInfo`/`CauseChain` machinery (additive accessor on `Exception`; zero new ownership rules), **keep** the later-wins priority direction, and **record the confrontation** — the snapshot recovers the diagnostics that were the whole argument for primary-wins, at no model change. |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **spec** — §31.4 (*The base `Exception` surface`* — one property added; the snapshot-chaining paragraph gains its supersession sibling), §31.2 (the `finally`-supersession paragraph: one clause stating the snapshot and its ordering), §31.2.1 (the `when`-guard supersession paragraph: the same clause by cross-reference), §28.4 (the multi-value-cleanup-pass supersession sentence: the same clause by cross-reference), **§22.9** (the construction-unwind pass — v0.256's rewrite identifies it as §28.4's generated-`Dispose()` owned-field pass, step 2 run without step 1, so its multi-disposal edges take the same clause by cross-reference), §31.1 (the *copy the data out* paragraph names the new accessor beside `CauseChain`), §32.9/§33 (the uncaught-exception report and the FFI-boundary abort print the superseded chain beside the cause chain). **stdlib** — §2.17 (built-in exception types cross-reference, if it mirrors the surface). **rationale** — the supersession record gains the confrontation paragraph. **guides** — 02 (Errors and Nullability): one paragraph where supersession is taught. |
| **Written against** | spec/stdlib **v0.255** (`git show 5fa5a95b:docs/*.md`) |
| **Currency** | **v0.256** (HEAD `8f41e99d` — the defect train `4130d2de` plus its gate fixes `e8f5387a`): every quoted §31.x and §28.4-supersession text is unchanged. Two v0.256 additions touch this proposal's neighborhood and are folded in: §22.9's rewrite names the construction-unwind pass as §28.4 step 2 (the cross-ref target above), and §28.4's new destructor teaching changes nothing about the supersession sentence this proposal extends. |
| **Origin** | Charter C3. The reviews put primary-wins against later-wins for exception supersession. The ruling declines the priority flip and takes the thing the flip was actually *for*: the diagnostics. |

*Reference convention: unprefixed §-references are language-spec references; stdlib and rationale references are marked. Every quotation is from the v0.255 snapshot, re-verified unchanged at v0.256. Bold inside a quotation is the source's own unless marked* (emphasis added).

---

## 0. Thesis

Bezel has a **single in-flight exception slot** and a **later-wins** rule: an exception raised in a non-normal position replaces the one already propagating, and the replaced one is disposed. Three sites state it (§2). The cost is that the superseded exception's diagnosis — its type, message, and stack trace — is gone, and it is frequently the *interesting* one: a `finally` that throws while unwinding a real failure buries the real failure.

The reviews' remedy was to flip the priority to **primary-wins** (the Java try-with-resources shape: the body's exception survives, the cleanup's is attached as suppressed). That flip changes the model: primary-wins requires the runtime to *keep the superseded instance alive* while the primary propagates, which reopens §31.1's exception-ownership rules (the in-flight instance is a runtime-owned borrow disposed after the matching arm) and adds a second live exception to a language that deliberately has one.

**This proposal takes the diagnostics without the model change.** At every supersession site the runtime records an `ExceptionInfo` **snapshot** of the superseded exception into the superseding one, then disposes it exactly as today. The snapshot is a value record of three strings; the carrier is an `Array<ExceptionInfo>` — the exact shape `CauseChain` has carried since v0.162. Later-wins stands, the single slot stands, and the lost diagnosis is recovered.

## 1. The existing machinery, verified

§31.4's base surface, quoted from the snapshot:

```bezel
public class Exception
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);   // snapshot chaining, below
  public readonly property Message: string;
  public readonly property StackTrace: string;             // formatted; "" when unavailable
  public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
end Exception;

public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string);
```

And the discipline the language already chose for exactly this problem (§31.4):

> **Cause chaining is by snapshot (within the exception-ownership model):** the `(message, cause)` constructor **borrows** `cause` for the duration of the call and copies its type name, message, stack-trace text, and its own `CauseChain` into the new exception's `CauseChain` (outermost cause first, flattened). The cause *instance* is never retained — consistent with §31.1's caught-exception-is-a-borrow rule, **no ownership machinery is needed**, and the full diagnostic chain survives into logs and `ToString()`.

§31.1 makes the same design move a second time, and calls it a precedent: *"Exception classes should therefore carry owned or snapshot state — the §31.4 `CauseChain` snapshot constructor on the base `Exception` type is the design's own precedent."*

Supersession is the one place where an exception is lost **to another exception** and no snapshot is taken — and the distinction is load-bearing, so it is stated rather than implied. The language loses exception diagnoses in a second way: **swallowing**, where no superseding instance exists at all — a faulted task disposed without ever being awaited (§30.9: the fault is *"disposed exactly once, by the task that owns it, when the task is disposed"* — abandoned, its diagnosis dies with the task), `Task.FireAndForget`'s supervised handling, and the no-aggregation posture's *"later faults are swallowed"* (§30.14/stdlib §6.10). Those losses are **outside this proposal by construction**: a snapshot must be recorded *onto* something, and a swallowed exception has no superseding instance to carry it — recovering those diagnoses is a supervision/logging question, not a chain question. Supersession is the one loss with a survivor to write on. This proposal is that snapshot.

## 2. The supersession sites (enumerated from the snapshot)

There are exactly three, and they already cross-reference each other as one rule.

1. **§31.2 — a throw escaping a `finally`.** *"when a `finally` entered during exception propagation itself completes by throwing (directly, or via a call that propagates), the new exception replaces the one being handled; the superseded exception is **disposed by the runtime exactly once**, as if its handling had completed — unless it is one of the two §31.1 dispose-after-arm exemptions … The superseding exception then propagates from the `try` statement as usual."*
2. **§31.2.1 — a throwing `when` guard.** *"If the guard itself **throws**, that new exception *supersedes* the in-flight one — the superseded exception is **disposed by the runtime exactly once** … — and propagates to the enclosing handler … This mirrors the rule for a `throw` in a `finally` (§31.2)."*
3. **§28.4 — a multi-value cleanup pass.** *"In a **multi-value cleanup pass** (scope-end disposal of several locals, an owning collection's element cascade, the exception-path discharge — including the unwind disposal of an expression's produced temporaries) … If a later disposal in the same pass also throws, the later exception **supersedes** the earlier one — the §31.2 `finally`-supersession discipline — and the superseded exception is disposed."*

Site 3 is the one that can supersede **repeatedly** in a single pass: N throwing disposers in one cleanup produce N−1 supersessions, and today N−1 diagnoses vanish. It is also the site a user is least able to instrument, because the pass is compiler-generated.

Two instances are **exempt from disposal** at supersession and stay live: a task-owned fault instance (§30.9) and the cached `TypeInitializationException` (§31.4). The snapshot is taken for those too — it is a copy, so it is orthogonal to whether the instance is disposed (§4).

**A fourth site by identification, not addition (v0.256).** §22.9's rewrite names the construction-unwind pass as §28.4's machinery run partially: *"the **owned-field disposals** of §28.4's generated `Dispose()` — that pass and nothing before it"* — i.e. §28.4 step 2 without step 1. A constructor that throws therefore triggers a multi-disposal pass over the already-completed owned fields, and a throwing disposer there is site 3's shape with a construction exception already in flight. The graduation train adds §22.9 to the cross-reference targets so the record-before-drop clause reaches the construction-unwind edges by the same by-reference route as §31.2.1 and §28.4.

**One unstated boundary, flagged for the train rather than resolved here.** §22.13's *Disposal of static owned fields* is a **multi-disposal pass §28.4's list does not enumerate** — every static `owned` field, reverse static-initialization order, on a dedicated shutdown thread — and it runs *"after `Main` completes (normally, **or by an uncaught exception** …)"*, i.e. with an in-flight uncaught exception explicitly contemplated. What a throwing exit-time disposer does — whether it supersedes the uncaught exception (changing §33.3's exit-code story), is swallowed, or aborts — is stated nowhere, so the pass currently has neither a supersession rule for the chain to extend nor a swallowing rule to exempt it. The train should either bring the exit pass under §28.4's supersession discipline (and then under this proposal's clause) or state its boundary explicitly; this proposal takes no position beyond flagging that the silence predates it.

## 3. The proposed surface

**One additive property on `Exception`** (§31.4), placed beside `CauseChain`:

```bezel
public class Exception
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);
  public readonly property Message: string;
  public readonly property StackTrace: string;
  public readonly property CauseChain: Array<ExceptionInfo>;         // empty when no cause
  public readonly property SupersededChain: Array<ExceptionInfo>;    // NEW — empty when nothing superseded
end Exception;
```

No new type: `ExceptionInfo` is reused unchanged. No new constructor: the chain is **runtime-populated**, exactly as `StackTrace` is — there is no user-callable way to append to it, and no user-supplied argument reaches it.

**Amendment sketch (§31.2, appended to the supersession paragraph; §31.2.1 and §28.4 cross-reference it):**

> **The superseded exception is recorded before it is dropped.** At the instant of supersession, and **before** the superseded exception is disposed (or, for the two owner-owned instances, before it is dropped from the in-flight slot), the runtime appends to the **superseding** exception's `SupersededChain` (§31.4): first an `ExceptionInfo` snapshot of the superseded exception — its type name, `Message`, and `StackTrace` — and then, flattened after it, every entry of the superseded exception's own `SupersededChain`. Entries appear in **supersession order, earliest first**: the first exception lost is the first entry. The recording is a copy of value data (`ExceptionInfo` is a record of `string`s, §31.4); the superseded *instance* is never retained, and its disposal, its exemptions, and every other rule of this section are unchanged. `SupersededChain` is empty on an exception that has superseded nothing.

**Why the ordering clause is normative and not incidental.** §31.2 says the superseded exception is *"disposed by the runtime exactly once, as if its handling had completed."* A snapshot taken after disposal would read a disposed instance's properties — a `DisposedAccessException` in checked disposal mode (§28.2) and undefined behavior unchecked. Stating *before* is the whole safety content of the rule.

**Why flattening, and why earliest-first.** Flattening mirrors `CauseChain`'s own discipline (*"copies … its own `CauseChain` into the new exception's `CauseChain` (outermost cause first, flattened)"*) and makes the N-supersession cleanup pass of site 3 produce one flat list of N−1 losses rather than a linked structure the record cannot represent — `ExceptionInfo` has three string fields and no room for a nested chain. Ordering diverges from `CauseChain` deliberately: `CauseChain` is ordered by causal depth (outermost cause first), a structural relation; supersession has no depth, only time, so the ordering that carries information is chronological. The graduation train should state that contrast in one clause so the two chains are not read as parallel orderings.

**The superseded exception's own `CauseChain` is not carried.** It cannot be — `ExceptionInfo` has no slot for it, and widening the record to carry one would change a shipped public record's shape for a case (a superseded exception that itself has causes, all of which are already flattened into *its* `CauseChain`) whose value does not justify the breakage. Recorded as a known loss, not an oversight.

## 4. Kernel check: zero new ownership rules

Verified against the v0.255 texts, item by item.

1. **The payload is a value record.** `ExceptionInfo(TypeName: string, Message: string, StackTrace: string)` — three runtime-managed fields, no disposal obligation (§28.9). Copying one transfers nothing.
2. **The carrier already exists on the same class.** `CauseChain: Array<ExceptionInfo>` is a `readonly` property of exactly this type on exactly this class. `SupersededChain` adds a second field of an already-present shape; the object's disposal story does not change. §31.4's own note that the built-in exceptions are trivial-dispose (*"all built-ins are trivial-dispose, so the after-arm disposal each in-flight exception reaches is the compiler-inserted flag-setting free, no user code"*) is unaffected for the same reason it is unaffected by `CauseChain`.
3. **The superseded instance is never retained.** The rule copies three strings and drops the reference — the identical move §31.4 makes for `cause`, and for the identical stated reason (*"consistent with §31.1's caught-exception-is-a-borrow rule, no ownership machinery is needed"*).
4. **§31.1's arm-lifetime rules are untouched.** The bound exception variable stays a borrow of the in-flight exception with the arm body's lifetime; `SupersededChain` is read through that borrow like `CauseChain`, and copying data out of it to survive the arm is §31.1's existing advice (*"copy the relevant data out of it (typically `ex.Message`, `ex.StackTrace` … the `CauseChain` snapshot …) into separate owned values"* — the sentence gains one item).
5. **The two owner-owned exemptions compose.** A superseded task-fault instance or cached `TypeInitializationException` is *"dropped from the in-flight slot **undisposed**, its owner's disposal rules unchanged — supersession ends the instance's handling, never its life"*. The snapshot is a read, so it is sound in both branches and changes neither.
6. **Purity is untouched.** §31.4 classifies the `()` and `(message)` constructors `pure` and the `(message, cause)` constructor **not** pure *"(its snapshot builds a fresh `Array`)"*. `SupersededChain` is not built by any constructor: it starts empty and is appended to by the runtime at an unwind, the same runtime-side population `StackTrace` already has. No constructor's purity classification changes. *(Drafting note for the graduation train: state that the chain starts empty and shares the empty-array representation, so a never-superseding exception allocates nothing — the same property `CauseChain`'s "empty when no cause" already implies.)*
7. **No K rule is added, narrowed, or widened; no axiom is spent.** The whole delta is a value-record copy into a field of an object the runtime already owns at a point the runtime already controls.
8. **Unwind-path allocation is best-effort, stated as such.** The append happens during unwinding, and growing the carrier array (or copying three strings) can itself fail — an allocation failure mid-unwind must not turn a diagnosable supersession into an abort. The rule the train states in one clause: the snapshot append is **best-effort — on failure the entry is silently dropped and the supersession proceeds exactly as today** (the superseded instance is disposed, the superseding one propagates). `SupersededChain` is diagnostic data, not a correctness surface: no program semantics may depend on an entry's presence, which is also why the accessor promises order and content of the entries it holds, never completeness under resource exhaustion.

## 5. The confrontation: why snapshot + later-wins beats primary-wins

The case for **primary-wins** is entirely a diagnostics case. Its canonical scenario: a body throws the real failure; a cleanup throws a secondary; under later-wins the secondary escapes and the real failure is gone. That is a genuine defect and this proposal fixes it — the real failure is in `SupersededChain`, with its type, message, and stack trace intact, printed by the uncaught-exception report and the FFI-boundary abort (§32.9/§33, both of which already print `CauseChain`).

What primary-wins would additionally buy is that the surviving *instance* is the primary one, so a `catch (IOException)` arm upstream still matches the body's failure rather than the cleanup's. What it would cost:

- **A second live exception.** The suppressed instance must stay alive and reachable through the primary until the primary's handling completes — i.e. one exception owning another. §31.4 declines exactly this, in a sentence written about a different feature and applying verbatim here: *"A live-instance `InnerException` on user exceptions is deliberately **not** provided (it would require exception instances to own exceptions, reopening the arm-lifetime rules)."* Primary-wins is that rejected design arriving through the supersession door.
- **A change to the in-flight-slot model.** The single slot is what makes §31.2's disposal rule (*"disposed by the runtime exactly once, as if its handling had completed"*) and its two named exemptions statable at all. Primary-wins needs a slot plus a suppressed list with its own ownership and disposal timing — new machinery in the one part of the language where the ownership rules are deliberately thinnest.
- **A behavioral reversal for the repeated case.** At §28.4's site 3, primary-wins means the *first* disposer's failure escapes and the rest are suppressed. That is defensible for try-with-resources, where "the body" is privileged; it is arbitrary for a scope-end pass over N locals, where no disposal is the primary.

**The precedents, cited honestly.** Both mainstream precedents recover the losing exception; they disagree about which one wins, and the disagreement is not the part either of them is famous for.

- **Java** (`Throwable.addSuppressed` / `getSuppressed`, JLS 14.20.3 try-with-resources): **primary-wins** — the body's exception propagates and each `close()` failure is attached as suppressed. But note the shape of the fix: what Java added was *the list*, not the priority. Before it, the cleanup exception simply replaced the body's, exactly as Bezel's later-wins does today, and the complaint was the lost diagnosis.
- **Python** (PEP 3134, `__context__`): **later-wins with the earlier preserved** — an exception raised while another is being handled becomes the active exception, and the earlier is retained as its context, printed as *"During handling of the above exception, another exception occurred."* That is precisely this proposal's model.

So the honest statement is: **the diagnostics recovery is universal across both precedents; the priority direction is not.** Bezel already sits on Python's side of the priority question for reasons that predate this proposal — the single in-flight slot — and this proposal supplies the half both precedents agree on. The difference from Python is that Bezel's carrier is a snapshot rather than a live instance, which is the same difference §31.4 already chose for causes and for the same reason.

**Recorded, as C3 directs:** the snapshot recovers the diagnostics that were the whole argument for primary-wins, at no in-flight-slot model change. Primary-wins is declined; its cost is a live second exception the exception-ownership model was designed to exclude, and its benefit — arm matching on the primary — is a real but narrower gain than the diagnostics it was chiefly argued for. If field evidence from external users shows that upstream `catch` arms are matching on cleanup failures often enough to matter, the question reopens with that evidence; the snapshot does not foreclose it, because a future primary-wins would keep the same chain and only change which instance carries it.

## 6. Migration and compatibility (changelog house categories)

- **Newly defined:** `Exception.SupersededChain: Array<ExceptionInfo>`; the record-before-drop rule at the three supersession sites, with its ordering clause.
- **Newly rejected:** nothing.
- **Behavior change on legal programs:** none observable except that the uncaught-exception report and the FFI-boundary abort print more (§32.9/§33 gain one line each), and a `catch` arm can now read a chain that was previously empty. No program's control flow changes; no exception's identity changes; no disposal timing changes.
- **Interface carriage:** none — `Exception` is a built-in, and §2.8's artifact-interface delta categories see one member added to a public type (the existing member-added/widening row).
- **Doc-only for the priority question:** later-wins is unchanged; what lands is the confrontation record.

## 7. Implementation sketch

- **Runtime:** one `Array<ExceptionInfo>` field on the exception base representation and one append helper called from the three unwind paths that already perform the supersession drop; the snapshot reads the three properties the runtime already materializes for the uncaught-exception report. The empty case must not allocate.
- **Codegen/sema:** the property is an ordinary `readonly` accessor on a compiler-known type; no new classification, no new diagnostic.
- **Spec-cited regression tests** (per `tests/README.md`, on `runAot`): supersession from a `finally` over a propagating body — assert the body's type name and message appear as `SupersededChain[0]`; supersession from a throwing `when` guard — same assertions plus the arm-skipping semantics unchanged (§31.2.1); a §28.4 multi-value cleanup pass with three throwing disposers — assert the chain has two entries in chronological order and the third exception propagates; the flattening case (a supersession whose superseded exception already carried a chain); the two exemptions (a superseded task-owned fault instance and cached `TypeInitializationException` — snapshot present, instance still live and re-raisable); the empty case (`SupersededChain.Count = 0` on an ordinary exception); and a checked-disposal-mode test proving the snapshot is taken before the disposal (the superseded instance's disposal still happens exactly once, and the chain's strings are intact).
- **Size:** small. One field, one helper, three call sites, one property, one printed line in two reports.

## 8. Interactions

- **§31.4's cause chaining** — the design this reuses. The two chains are independent: an exception may carry both, and the graduation train should say so in one clause so a reader does not expect one to subsume the other.
- **§30.9 (faulted tasks) and §31.4 (`TypeInitializationException`)** — the two owner-owned live instances; §4 item 5 shows the snapshot composes with both exemptions without touching them.
- **§28.2 (checked disposal)** — the mode that would deterministically catch a snapshot taken in the wrong order; the ordering clause exists so it never has to.
- **Proposal 0080 (Exception-derived throwables)** — every throwable carries the `Exception` reporting surface (§31.2's own statement), so the new accessor is available on the whole throwable set with no per-type work.
- **Proposal 0099 (`ScopeGuard`; this programme)** — a throwing guard action on an exception edge is a §28.4 supersession this chain records: the guard's destructor runs in the multi-value cleanup pass, its exception supersedes the in-flight one, and `SupersededChain` is what keeps the original failure readable. 0099 §6 item 5 states the composition from its side; the two proposals cite each other.
- **Proposal 0101 (A13 retirement study; this programme)** — if its Rule A lands, whole-value inline-array reassignment gains a displaced-value disposal pass that is a §28.4 multi-value cleanup: a slot disposal throwing mid-pass supersedes the earlier failure, and this chain is what preserves it. 0101 §6 item 2 flags exactly that composition as a gate item and names this proposal; the reciprocal citation lives here.
- **Charter C10 (#3245, destructor-on-constructor-unwind)** — adjacent but disjoint: C10 rules what runs, this rules what is recorded when two things that ran both threw. No text overlap.
