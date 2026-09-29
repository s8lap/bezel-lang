# Proposal 0110 — The partial-failure toolkit: a reachable fault sink, a dynamic as-completed task set, and supervision (staged)

| | |
|---|---|
| **Status** | under-review — draft (2026-08-20) |
| **Tracking issue** | — |
| **Target module(s)** | **stdlib only, in three stages, all in `System.Threading`.** Stage 1 — ▲`FaultReport`, ▲`FaultOrigin`, ▲`TaskExitKind`, ▲`RuntimeFaults`, plus the ▲`ExitKind`/▲`FaultTypeName` rider on §6.8's `DomainFaultedException`. Stage 2 — ▲`TaskSet<T>`, ▲`TaskCompletion<T>`. Stage 3 — ▲`Supervisor`, ▲`ChildSpec`, ▲`RestartPolicy`, ▲`RestartKind`, ▲`SupervisionStrategy`, ▲`Backoff`. **No spec rule is added, amended, or reinterpreted by any stage.** Stage 1 does amend one *standard-library* rule: stdlib §6.5's handler-fault dispatch, for this one surface (this proposal's §2.3). Stage 3 alone carries two *appositive* spec mentions — naming its child-registration position as a third member of the existing spawn-position family ([§30.14](../Bezel_Language_Spec.md#sec-data-parallel-combinators)) and of [§28.14](../Bezel_Language_Spec.md#sec-scoped-callback-parameters)'s stdlib retention register — the same two mentions `Parallel` took at v0.186, with the same content: no new checking rule. |
| **Written against** | spec/stdlib/rationale **v0.262** (HEAD `8a03dd2` at authoring) — every quotation re-verified against that tree, not taken from proposal 0106's transcription. **Two v0.263 landings are concurrent with this draft** (0106 slate ii and vii) and are cited from their landing text, not assumed: the `CancellationTokenSource` **synchronized** fact ruling with its published-source signal-only rule (stdlib **§6.1**, with the spawned-work consequence at stdlib §6.8), and rationale **§12.14** (the recovery-unit posture) together with §12.4's extension declining forcible domain teardown. Stage 3 is gated on the first and scoped by the second. |
| **Currency** | v0.262 (HEAD `8a03dd2`) plus the two concurrent v0.263 landings named above. **Quoted:** spec §22.12, §28.14, §29.11, §29.12, §30.10, §30.13, §30.14, §30.15, §33.3; stdlib §1, §6.1, §6.5, §6.7, §6.8, §6.10, §6.12; rationale §12.4, §12.12, §12.14. **Cited without quotation:** spec §3.6, §9.7, §17.13, §22.13, §22.16, §28.2–§28.3, §28.13, §30.5, §30.9, §31.1, §31.4; stdlib §2.11, §4.13, §6.9, §6.11. Re-verification before any ruling must cover all of the first list plus stdlib §6.9 — and, for stdlib §6.1 and rationale §12.4/§12.14, the **final** v0.263 text, since the published-source rule was redesigned during this draft's authoring. **2026-09-17 proposals currency sweep — re-verified at v0.306.** No tier-1 respell: §3.2's §29.11 (*"the frame's enumeration right, which is affine and spent at exactly one of the consumption points below"*), §30.15 (*"fresh drain enumerable over the channel"*, *"not a frame-identity or §29.11 affine-tracking fact"*) and §29.12 quotations all still match verbatim, and §22.12's rules 2–5 keep their numbers — and rule 2 form (i) keeps the quoted `Channel<T>` sentence — through the v0.291 verdict-table rewrite. One tier-2 flag in `proposals-currency-2026-09-17.md` §3 (stdlib §6.12 `Parallel`). Written-against pin unchanged. |
| **Origin** | Owner direction, 2026-08-20: take proposal **0106**'s slate items **iv** (a reachable fault sink), **viii** (a dynamic as-completed task set), and **ix** (▲`Supervisor` + ▲`RestartPolicy`) as **one staged proposal**, later stages gated. 0106 §7 is the deep dive this implements; its §7.3 correction binds every line below. |

*Reference convention (0103/0104's): unprefixed §-references are **language-spec** references; **rationale** and **stdlib** references are marked; references to this proposal's own sections are written "this proposal's §N", so a bare §N is never a self-reference; 0106's sections are cited "0106 §N". Quotations are verbatim; **bold inside a quotation is the source's own**. **▲** marks a surface existing in no landed or proposed document — every type, member, and enum case named here is ▲ unless quoted from the stdlib. Syntax sketches are minimal and are not proposed spellings: the surface spelling comes from the Fable syntax pass that follows. Prior-art claims (OTP, Akka, .NET) are approximate and secondary-sourced.*

---

## 0. Charter, staging, and the refusal that binds every stage

**Charter.** 0106 §7 found that "Bezel has world-class fault **containment** and no fault **recovery** — and the recovery gap is not one hole but four". Slate items **ii** (cross-domain cancellation by fact ruling) and **vii** (the process-supervision posture) are landing concurrently at v0.263. This proposal is the vehicle for the other three, staged in dependency order:

1. **Stage 1 — the reachable fault sink** (slate iv): make the faults the runtime already swallows observable by the program. Depends on nothing; mints the vocabulary stages 2 and 3 dispatch on.
2. **Stage 2 — the dynamic as-completed task set** (slate viii): an owning set of in-flight children that admits additions *while an enumeration of completions is live*. Depends on stage 1 for its completion discriminant; useful well beyond supervision.
3. **Stage 3 — ▲`Supervisor` + ▲`RestartPolicy`** (slate ix): parent-held policy over capture-free child specifications. **Gated on stages 1–2 and on v0.263's `Shared<CancellationTokenSource>` grant**, without which a parent cannot ask a child in another domain to stop at all.

**The gating is load-bearing, not decorative.** Stage 3 written today would have to invent its own exit vocabulary, its own completion multiplexer, and its own cross-domain stop signal — and rationale §12.14, landing concurrently, names exactly those absences: "nothing multiplexes child exits, no restart policy vocabulary exists, and a supervisor parameterised over a *stored* entry value is not writable in user Bezel, the entry being a call-site syntactic category (the precedent is stated for `Parallel`, stdlib §6.12)." Each earlier stage is independently justified and independently landable; if stage 3 is never taken up, stages 1 and 2 stand alone.

**The refusal, stated first because it constrains everything after it.** 0106 §7.3: "**Ownership does *not* make forcible domain teardown good.** … **No proposal here may claim otherwise**". At v0.262 the ground was rationale §12.4 — "Forcible cancellation (Thread.Abort) is unsafe — it can interrupt code holding a lock, corrupt invariants, or skip cleanup" — plus §30.13's run-to-completion, under which "a continuation, once started, runs without interruption from the domain's other work until it suspends at an `await`, completes, or propagates an exception". The concurrent v0.263 landing extends that ground to domains explicitly and **declines `brutal_kill`**: "A domain is not a safer unit to interrupt than a thread; §30.13's run-to-completion execution makes it less safe … there is no unwind to run them. … the take-away half to process-level supervision (§12.14)."

**So: nothing in this proposal kills, aborts, interrupts, or forcibly tears down a task or a domain.** Stage 3's shutdown protocol (this proposal's §4.4) is designed inside that decline and says plainly what it cannot deliver.

---

## 1. The baseline, re-verified at v0.262

0106 §7.1 assembled it; the five load-bearing quotations are re-verified here because every stage is priced against them.

- **(a) A discarded task's faults are logged or silently swallowed, to an unreachable destination.** §30.10: the two legal patterns are "`Task.FireAndForget(call())` (supervised: exceptions thrown by the task are logged) and `using var t := call();` (unsupervised: … any exceptions it raises are silently swallowed". stdlib §6.7 closes the matter: "The exception-logging mechanism for `FireAndForget` is implementation-defined; implementations may provide a global handler." *May* — and nothing lets a program **be** that handler.
- **(b) The one report the spec insists on has the same unreachable destination.** §30.10: in checked disposal mode a `DisposedAccessException` in an abandoned continuation "is first **reported** through the same implementation-defined diagnostic channel as `FireAndForget`'s logging, then swallowed: the backstop that catches a §30.1 source-outlives-settlement violation must never itself be invisible". The spec's own reason is stage 1's argument in the spec's voice.
- **(c) A domain isolates state, not failure.** §30.14, *Domain completion*: pending sibling tasks' "eventual exceptions are handled as `Task.FireAndForget` handles them (logged, not propagated — stdlib §6.7)." Rationale §12.12 confirms the role: "`Domain.Spawn` is not a concurrency combinator at all — it is an isolation boundary (§12.5), chosen for state separation".
- **(d) Two more documented swallow sites.** stdlib §6.10: `TaskScope.DisposeAsync` "cancels the scope's token, drains remaining children, and **always swallows** their faults", and "later faults are swallowed (the recorded §13.5 no-aggregation posture)". §30.14 (`Parallel`): "later faults are swallowed."
- **(e) The fault that does cross carries no machine-readable reason.** stdlib §6.8 gives `DomainFaultedException` exactly `FaultStackTrace: string` plus a `Message` of "the original exception's message text"; §30.14: "The original exception object never crosses the boundary … `Message` and `FaultStackTrace` are copied strings."

**The containment side, which nothing here weakens.** 0106 §7.2 stands: owned state dies with its task or domain deterministically by unwind disposal, `Locked<T>` needs no poisoning, and a restarted child provably cannot inherit its predecessor's inbox. The gap is recovery, and recovery is a library.

---

## 2. Stage 1 — the reachable fault sink

### 2.1 What it is

One process-global, observation-only publication point on which the runtime reports **every fault it swallows on the program's behalf**, carrying a sendable value payload naming the site, the kind, and the fault's snapshot text. A program subscribes and routes reports wherever its diagnostics go — in practice into proposal 0021's `ILogger`. That is all of stage 1: it changes no fault's fate. Every fault reported here is still swallowed exactly as §30.10, §30.14, and stdlib §6.10 say.

### 2.2 The admission form — the decision

0106 §7.5.2 left it open with two candidates: the sink "must join the capture-free register **or** be a §22.12 rule-4 `static readonly` sink on the `SharedEventSource<T>` precedent."

**Decision: neither, and the ground is narrower than it first looks. The sink needs no admission form at all, because it declares no static the user's rules govern — what it needs is one *type-carried* guarantee, and §22.12 rule 5 already carries it.**

The distinction matters, so it is drawn precisely. Rule 5's *recommended shape* is a program's: "A process-global publication point is an ordinary static field of type `SharedEventSource<T>` (stdlib §6.5) — a form-(i) member, admitted warning-free in the recommended shape `private static owned readonly _e: SharedEventSource<T> := new();` with program-exit disposal (§22.13)." That sentence is about a **static field**, and this proposal's §2.5 declares none: `RuntimeFaults` is a compiler-known static class exposing one property over a runtime-owned implementation. So the statics rules (rule 2's form-(i) admission, rule 4's warning) and §22.13's `owned`-field exit disposal simply **do not apply to it** — not because it satisfies them, but because there is no user-written declaration for them to classify. Nothing about the sink can therefore be argued from stdlib §1's fact formulas, and this proposal claims nothing from them.

**What does transfer is the one thing stage 1 actually needs**, and it transfers because rule 5 keys it on the *declaring type of the subscribe position*, never on the field: "every call resolving to a subscribe position **declared on** `ISharedEvent<T>`/`SharedEventSource<T>` — whatever the receiver's static spelling, a constrained generic parameter included — admits only **capture-free** handlers: a non-capturing lambda or a static method reference, the same admission as `Domain.Spawn` entries, as a stdlib-stated entry in §28.14's capture-free register; and method-reference formation over a register position is itself rejected (§28.14), so no plain function-typed alias of `Subscribe`/`Add` can launder the admission." Because the sink's **public face is the interface** — the shape stdlib §6.5 already recommends ("The recommended public face is a static expression-bodied property of the interface face") — every subscription to it is capture-free **by construction, with no new §28.14 register entry and no new rule anywhere**. That is the whole of stage 1's admission story.

**Two corollaries, stated so nothing is claimed by omission.** *Lifetime and exit severance are the runtime's business, not a program's obligation:* there is no `owned` static field to enrol in §22.13's pass, so the sink's shutdown behaviour is part of its own compiler-known contract — reports arising after severance, on a shutdown pass that runs outside any domain, reach the implementation-defined channel only. And *the rule-4 comparison is moot but worth recording*, since 0106 §7.5.2 raised it: had the sink been shipped as a user-visible `static readonly` of an interface type, it would have drawn rule 4's declaration-site warning by construction — the posture proposal 0021's delegated ruling 1 already found wrong for stdlib surface ("A stdlib-shipped member whose canonical use warns by construction is the wrong default posture"). Shipping it as a compiler-known surface avoids that question rather than answering it.

**Why compiler-known rather than a bare `SharedEventSource<FaultReport>` field.** Two properties an ordinary source cannot carry: the raiser is the runtime and never user code (this proposal's §2.5), and the dispatch discipline is the runtime's own rather than stdlib §6.5's first-error-wins (this proposal's §2.3). The implementation is runtime-owned, as `Channel<T>`'s and `Parallel`'s are; only the *face* is the shared-event interface, which is exactly what makes the capture-free admission above apply.

### 2.3 The re-entrancy rule — a fault handler may not fault the sink

The hazard is specific. Handlers run on the domain that swallowed the fault, and under stdlib §6.5 a throwing handler's exception "propagates out of `Raise` to the raiser" — here, the runtime's own swallow path, which has no owner to receive it. Unstated, the failure mode is unbounded report→fault→report amplification at exactly the moment a program is already failing. **The rule, normative for this surface:**

1. **Dispatch on a domain is disarmed for its own duration.** (There is one process-global sink; what is suppressed is *dispatch on the domain currently dispatching*, not the sink itself.) While a domain is dispatching a fault report, any fault that would be reported there during that dispatch — from a handler or from work a handler triggers — is not delivered. Reports never nest and never recurse.
2. **A handler fault reaches neither the runtime nor the program.** It is routed to the implementation-defined diagnostic channel as a report of last resort (the channel of §30.10 and stdlib §6.7, which is not replaced) and discarded. It does not fault the raising continuation and does not alter the fate of the fault being reported.
3. **Dispatch continues to the remaining handlers**, and this may be **no departure at all**. stdlib §6.5 states first-error-wins for a raiser — "**Handler exceptions — first error wins** … If a synchronous handler throws, subsequent handlers are not invoked and the exception propagates out of `Raise` to the raiser" — and closes the same paragraph by naming the escape: "To run all handlers regardless of failures and aggregate exceptions, the raiser must implement that policy explicitly — `Raise`/`RaiseAsync` do not aggregate." **The runtime is this sink's only raiser** (this proposal's §2.5), so run-all-handlers is exactly the policy §6.5 tells a raiser to implement itself, rather than a rule being overridden. Clause 3 does *not* aggregate the handler faults it thereby collects — it discards each under clause 2, since there is no caller to receive an aggregate.

**The residual question, which is why this is still Q2.** Even read as a raiser-side policy, the discipline a subscriber gets differs from the one an ordinary `ISharedEvent<T>` gives, behind the same interface face — and that cuts against the principle rule 5 is built on, which this proposal quotes elsewhere: its guarantees are "carried by the type and therefore hold at **every** subscription site, aliases included", closing with "There is no static type through which a capture-admitting subscription to a shared source can be written" — a guarantee rule 5 describes as resolved by construction. Rule 5 removes exactly this class of alias-dependent surprise for the *capture-free* guarantee; clause 3 reintroduces a milder one for *dispatch*. Two dispositions are therefore coherent, and the owner should pick: treat dispatch as a raiser-side policy §6.5 already delegates (this proposal's recommendation — nothing about the type's guarantees changes, since first-error-wins was never one of them), or hold that a type-carried face must carry a uniform dispatch discipline too, in which case the sink takes first-error-wins and one broken observer can blind the rest.

Two corollaries either way: handlers are capture-free by rule 5, so a handler holds no state a re-entrant report could corrupt; and `FaultReport` is a value record, so dispatch transfers no ownership and a handler fault leaves nothing half-transferred.

### 2.4 The payload — a sendable value record over §31.4's existing snapshot type

```bezel
public enum TaskExitKind: byte  Success, Fault, Cancelled; end TaskExitKind;          // ▲

public enum FaultOrigin: byte                                                          // ▲
  FireAndForget,          // §30.10 / stdlib §6.7 — "exceptions thrown by the task are logged"
  AbandonedContinuation,  // §30.10 — the checked-mode DisposedAccessException report
  DomainCompletion,       // §30.14 Domain completion — a pending sibling's eventual exception
  ScopeDrain,             // stdlib §6.10 — DisposeAsync "always swallows"; later faults swallowed
  ParallelWorker;         // §30.14 Parallel — "later faults are swallowed"
end FaultOrigin;

public record FaultReport(                                                             // ▲
  Origin: FaultOrigin,
  Kind: TaskExitKind,       // never Success on this carrier
  Fault: ExceptionInfo,     // §31.4's landed (TypeName, Message, StackTrace) record
  Timestamp: DateTime);
```

**Why this shape.** `ExceptionInfo` is §31.4's own snapshot record of three `string`s — the shape §31.1 prescribes for preserving a fault past its arm and the shape §30.14 already uses to carry a fault across a domain boundary. Reusing it invents nothing and makes `FaultReport` a **sendable value record** under §30.14's value-type rule. Sendability is not *required* here — §22.12 rule 5: "No payload-sendability check is imposed: under capture-freedom plus raising-domain execution (stdlib §6.5), each payload is constructed and consumed within one domain" — but it is what lets a capture-free handler forward a report over a channel to a sink domain with no second representation, which is proposal 0021 §D5(b)'s topology exactly.

**`FaultOrigin`'s cases are exactly the swallow sites the normative documents name today** (this proposal's §1(a)–(d)), and the set is treated as closed under a discipline **borrowed by analogy, not by direct application**. stdlib §1's error-payload governance governs an "error-payload type — the `E` of a `Result<T, E>` return, or a domain choice declared to carry a producer's failures", and `FaultOrigin` is a plain enum that is nobody's error payload, so the rule as written does not reach it. What transfers is its reasoning: "Additions to a shipped payload type's case set are gated by recorded ruling … because a choice is a closed sum: every case addition breaks every exhaustive consumer." An enum a sink handler matches exhaustively has exactly that property, so this proposal adopts the same gate for `FaultOrigin` **by its own declaration** — a future swallow site adds a case by recorded ruling, and the ruling is where the new site's documentary basis gets written down. Said plainly so no reader takes a landed rule to be doing work it does not do.

### 2.5 The surface

```bezel
public static class RuntimeFaults                                     // ▲ System.Threading; compiler-known
  public static property Unobserved: ISharedEvent<FaultReport>;        // subscribe-only face
end RuntimeFaults;
```

**Subscribe-only, deliberately.** There is no public raise: the runtime is the only raiser, so the sink cannot be repurposed as a general application bus duplicating 0021's `ILogger` with a worse spelling, and every report keeps its meaning ("a fault the runtime swallowed"). *Declined:* a public `RuntimeFaults.Report(...)`.

**Additive, never substitutive.** With subscribers present, whether the implementation also performs its own default reporting is unchanged and remains implementation-defined; stage 1 adds an observation edge and removes none. *Declined:* "a subscriber suppresses the default channel" — it would make a diagnostic's presence depend on an unrelated subscription, and §30.10's insistence that the checked-mode report "must never itself be invisible" cuts against any rule that can silence it.

**The route to a logger is a static, because the handler is capture-free** — either a form-(i) `ChannelSender<LogEvent>` static feeding 0021's sink-domain topology, or the fixed `static readonly ILogger` that 0021's delegated ruling 1 preserves as a **user-side** idiom ("Fork B survives as a *documented user-side idiom* … it just isn't stdlib surface"). Stage 1 ships no logging surface of its own and takes no position on 0021's ambient fork.

### 2.6 The `DomainFaultedException` discriminant rider — judged: it rides stage 1

0106 §7.5.6 lists it among riders "folded into whichever proposal opens first": "▲ a small sendable exit-kind enum as the dispatch surface, the fault's type name carried as diagnostic text only — restart policy is a function of the exit reason, a supervisor dispatching on any string must string-match, and string-matching is exactly the stringly-typed dispatch Bezel rejects elsewhere."

**Judgment: stage 1.** (i) Stage 1 mints `TaskExitKind` anyway — a swallowed `OperationCanceledException` and a swallowed crash are different events to every observer. (ii) Stage 2's completions carry the same discriminant, so deferring would force a second vocabulary. (iii) It is the smallest increment on a landed type and is useful *today*: at v0.262 a program awaiting a join cannot distinguish "the child crashed" from "the child observed the cancellation I asked for", because §30.14 routes **every** exception out of the entry — `OperationCanceledException` included — into `DomainFaultedException`, whose only discriminating surface is text.

```bezel
public class DomainFaultedException: Exception
  public readonly property ExitKind: TaskExitKind;    // ▲ Fault or Cancelled — never Success (documented, not a clause)
  public readonly property FaultTypeName: string;     // ▲ diagnostic text only — NOT a dispatch surface
  public readonly property FaultStackTrace: string;   // landed (stdlib §6.8)
end DomainFaultedException;
```

The enum is the **sole dispatch surface**, the Fable pass's fixed shape. `FaultTypeName` is added because the type name is genuinely useful to a human reading a log and is today reachable only by parsing `Message`; it is spelled and documented as text so no supervisor is tempted to branch on it — that prohibition is the rider's point. *Declined:* splitting the vocabulary into a fault-only kind here and a settlement kind for stage 2 — declined for unification (a supervisor's policy table is keyed on one enum), recorded because the `Success`-never-occurs asymmetry is real (Q4).

**"Never `Success`" is documented prose, deliberately not a `§22.16` contract clause.** The restriction is a property of where the carrier is produced — the runtime raises `DomainFaultedException` only on a non-success edge — so it is a statement this document makes about its own surface, not a clause the compiler instruments. Spelling it as an `invariant` member would make it **one of the standard library's first contract clauses** (0109 verifies the count at zero), and that is not a precedent a discriminant rider should set sideways: a clause is carried in the artifact interface as a contract record (§2.8.2), which also discloses whatever non-public state it names, and it would be instrumented at the type's public entry and exit points — real, permanent surface bought for a diagnostic property no caller branches on. Sibling proposal 0109 declines the same precedent on the same ground for its own faults — it records "no member declaration in `Bezel_Standard_Library.md` carries a contract clause today (verified zero at v0.263)" and holds that "setting that library-wide precedent sideways, inside a small accessor proposal, is the wrong vehicle for it" — and this proposal matches its stance rather than quietly diverging from it. If the library ever adopts contract clauses, this restriction is an obvious early candidate — as an addition made by that decision, not by this one. This is why §5's "nothing normative" claim stands unqualified.

### 2.7 What stage 1 does not do

It does not recover, retry, restart, suppress, or reclassify. No handler may change a fault's fate: no return value, no `Handled` flag, no veto. **Recovery is stage 3**, and the line is hard here because a sink with a veto is a supervisor with none of a supervisor's structure — no policy, no intensity limit, no escalation, no owner.

---

## 3. Stage 2 — the dynamic as-completed task set

### 3.1 The gap, against the nearest landed member

0106 §7.2: detection is "present but pull-based and un-multiplexable … `WhenEach` consumes a fixed snapshot, so a restarted child cannot join an enumeration in flight."

Re-verified. `Task.WhenEach`'s three landed forms (stdlib §6.7) take `Array<Task>`, `Array<Task<T>>`, or `owned OwningArray<OwningTask<T>>` — in every case **the participant set is fixed by the argument at the call**. The borrowing forms are documented "**Observer, not owner** … enumeration neither cancels nor settles the tasks", and the owning form's declaration comment states that it takes ownership of the array and of every task in it. No member on any of them admits a task after enumeration begins, and none could: the borrowing forms' input is a caller-owned array the frame merely borrows, and the owning form consumed its array at the call. The one route the signatures leave open is mutating that caller-owned `Array<Task<T>>` while the borrowing enumeration runs — and it is a dead end in both directions: stdlib §6.7 never says the frame enumerates the array (it delivers completions), so a mid-enumeration `Add` has no stated effect on the participant set, and if it did the source would be a collection under enumeration, which §29.12 invalidates. Undefined at best, rejected at worst; not a supervisor's mechanism either way.

**Why that is fatal for a supervisor and not merely inconvenient.** A supervisor's participant set changes *because of the completions it is observing*: a child completes, policy says restart, and the replacement must join the same wait. With `WhenEach` the only expressible loop is enumerate → observe one completion → **end the enumeration** → rebuild → enumerate again: O(children) work per event for a mechanism whose whole purpose is to wait. And the rebuild collides with stdlib §6.7's own input rule (the #1813 seam), since a freshly-minted task cannot be stored into a borrowing `Array<Task<T>>` slot at all — so each round must re-consume an `OwningArray`.

**The ownership answer is already ruled**, and stage 2 takes it as its charter — stdlib §6.7: "dynamic fan-in over freshly-minted tasks is an ownership-transfer scenario by construction, so the owning forms are its spelling, and no borrowing-family dynamic-build facility is planned." **The dynamic set is an owning type, and stage 2 ships no borrowing twin.**

### 3.2 §29.11 and §29.12, confronted

**§29.11 (single consumption)** is about a *frame*, not a *source*: "A **lazy query** … is **single-consumption**. It may be enumerated **at most once**", and the section's own layering pins what is affine — "the frame's **enumeration right**, which is affine and spent at exactly one of the consumption points below". §30.15 then states the precedent for a long-lived source drained repeatedly, naming §29.11 to exclude it:

> The `ReceiveAll()` call mints a **fresh drain enumerable over the channel** ([§28.3], v0.258 — not the channel re-exposed …), and the at-most-one-consumption obligation is the channel's own single-consumer invariant, runtime-enforced — **not a frame-identity or §29.11 affine-tracking fact**. … The at-most-one-consumption obligation releases when an enumeration ends by any edge — normal completion, fault, or enumerator disposal — after which a `Receive`, `TryReceive`, or fresh `ReceiveAll()` (a newly minted drain enumerable) is admissible.

Stage 2 adopts that resolution verbatim: ▲`TaskSet<T>.Drain()` mints a fresh drain enumerable per call under §28.3's calls-mint rule; each frame spends its own §29.11 right exactly once; at-most-one-enumeration-in-progress is the *set's* invariant, enforced at runtime with `InvalidOperationException` and released on any end edge.

**§29.12 (mutation during iteration)** looks sharper — "*Structural* modification invalidates: any operation that adds, removes, or repositions entries or reallocates storage" — but its hazard is positional: an enumerator holds a position in a sequence, and adding may reposition or reallocate what that position denotes. A completion drain holds **no position in the pending set**: it yields *removals*, in completion order, of items the set has already settled. Appending a pending task cannot invalidate anything the enumerator holds — the same structural reason sending into a channel during a live `ReceiveAll()` is that primitive's purpose rather than its violation.

**The resolution in two sentences.** *§29.11's affine rule binds the enumeration right of one minted frame and never the source, so a fresh drain enumerable per `Drain()` call spends its own right exactly once, and at-most-one-in-progress is the set's own runtime-enforced single-consumer invariant — precisely the shape §30.15 already states for `ReceiveAll()` and explicitly marks "not a frame-identity or §29.11 affine-tracking fact". Adding while a drain is live is not §29.12 mutation-during-iteration either: the drain yields completions removed from the pending set and never positions within it, so nothing an enumerator holds can be repositioned or reallocated.*

### 3.3 The surface

```bezel
public sealed class TaskSet<T> where T: class          // ▲ compiler-known; domain-local; not sendable
  public constructor();
  public method Add(owned task: OwningTask<T>, tag: long);   // admitted while a drain is live
  public method Close();                                     // no further Add; idempotent
  public readonly property PendingCount: int;
  public readonly property IsClosed: bool;
  public method Drain(): IOwningAsyncEnumerable<TaskCompletion<T>>;   // fresh drain per call; stdlib §4's Drain() verb, at the set's async grain
  destructor;    // disposes still-pending tasks — abandonment, not cancellation (§30.5)
end TaskSet;

public sealed class TaskCompletion<T> where T: class   // ▲ the yielded element
  public readonly property Tag: long;
  public readonly property Kind: TaskExitKind;         // stage 1's discriminant
  public owned readonly Task: OwningTask<T>;           // await it for the result or the fault
end TaskCompletion;
```

**Drain semantics mirror §30.15's contract shape**: suspend while the set is empty and open; yield each completion as it settles, ties broken in unspecified order; complete **normally** when the set is closed and drained. A faulted or cancelled child is yielded, never skipped — `WhenEach`'s rule ("**Faulted and cancelled tasks are yielded, not skipped**"), which keeps §30.9's fault ownership intact: the fault stays owned by the task that produced it.

**The end-edge ownership rule — the other half of §30.15's precedent, adopted explicitly.** §3.2 takes §30.15's affine/mint resolution; its `ReceiveAll()` paragraph carries a second rule that a drain over *owned* elements cannot leave unstated: "Abandoning the enumeration mid-drain deregisters like an abandoned `Receive` (below); no item is consumed or lost", with "an item mid-handoff at that instant is returned to the channel's head". A `TaskCompletion<T>` owns its task, so the same question arises here for a child that **settled but was never yielded**. The rule, on **every** end edge — normal completion, fault, or enumerator disposal: **a settled-but-unyielded completion is retained by the set, not disposed and not lost.** Its task stays owned by the `TaskSet<T>`, the child stays counted in `PendingCount`'s successor state, and a subsequent `Drain()` call yields it first, in its original completion order. Only two things end a completion's life: a consumer takes it from a drain (ownership transfers to the iteration variable under §4.13's owning-enumeration rules), or the set itself is disposed, at which point its destructor disposes what it still holds. This is the give-back half of the precedent rather than the produce-then-dispose posture of §30.5, chosen for §30.15's own stated reason — the set's items are unrecoverable data, and an abandoned drain is exactly when a supervisor is mid-shutdown and least able to afford silently dropping a child's result or fault.

**Why `Kind` rides the completion.** A supervisor must decide *whether to await at all*: a successful child's `OwningTask<T>` yields an owned result it may want to route, while a faulted child's await re-raises into the supervision loop — the last place a fault should land unguarded. `Kind` lets the loop branch before it commits; it is no substitute for awaiting, since the task remains the only place the result or exception lives.

**Why an explicit `tag`.** The supervisor must map a completion back to the spec that produced it. `WhenAny`'s index does not survive a dynamic set, and identifying children by task reference would rest the loop on a `Task` comparison the stdlib does not document for the purpose. A `long` is a sendable value with no ownership and no extra type parameter. *Declined:* a generic key `TaskSet<K, T>` — a second parameter serving one call site; a program wanting richer keys keeps the `Dictionary<long, …>` it needs anyway for the specs.

**Why compiler-known.** For `WhenEach`'s reason: observing a task's settlement requires registering a continuation on it, and no stdlib member exposes that (`TaskCompletionSource<T>` *completes* a task; it does not observe one). That is a cost, not a language change.

**Disposal is honest.** The destructor disposes what the set still holds — both still-pending tasks and any retained settled-but-unyielded completions — and disposing an *unsettled* task **abandons** it (§30.5): it neither cancels nor waits. A supervisor must therefore cancel-and-drain before disposing its set, which is the first place the missing take-away (this proposal's §4.4) shows through.

### 3.4 Relation to `Task.WhenEach` (proposal 0023, graduated v0.212)

**It generalizes one landed form and replaces none.** `WhenEach(owned tasks: OwningArray<OwningTask<T>>)` is the special case *add-all, close, drain once*, and stage 2 records that equivalence so the two cannot drift. The **borrowing** forms are not generalized and should not be: they borrow a caller-owned array whose lifetime the compiler tracks through the frame (0023 §D3's compiler-tracked borrow), own nothing, and settle nothing — a genuinely different contract.

**Recommendation: keep `WhenEach` unchanged.** It is graduated, and it is the better spelling for the fixed case (one call, no set object, no close). 0023's §D2 affine reasoning is re-derived here from the same §29.11 text and §30.15 precedent and survives intact: a `WhenEach` frame is affine; a `TaskSet` *source* was never in §29.11's scope. *Declined:* re-expressing `WhenEach`'s owning overload as sugar over `TaskSet<T>` — it would change a landed member's implementation for no observable gain and put a graduated surface behind an unlanded one.

**The `TaskScope` line stays where rationale §12.12 drew it.** `TaskSet<T>` is an observer, like `WhenEach`, not a nursery: stdlib §6.7's "Supervision (fault ⇒ cancel siblings) remains `TaskScope`'s job (§6.10; rationale §12.12)" is inherited verbatim. Stage 3 adds a *supervisor* that uses the observer; `TaskScope` remains right for a lexically-scoped unit of work whose parts stand or fall together.

---

## 4. Stage 3 — ▲`Supervisor` and ▲`RestartPolicy`

### 4.1 The gate, and the posture that scopes it

Stage 3 opens only when stages 1–2 have landed **and** v0.263's `CancellationTokenSource` **synchronized** fact ruling has landed. The third is not a convenience: at v0.262, stdlib §6.8 was categorical — "There is no built-in cross-domain cancellation: `CancellationToken` is a borrow-bearing record and not sendable. The pattern is a dedicated channel carrying a cancel message, polled by the spawned domain at its safe points." A supervisor whose only stop protocol is a per-child bespoke channel leaves most of what stage 3 exists to remove in the user's hands. The concurrent landing supplies the primitive — stdlib §6.1: "**`Shared<CancellationTokenSource>` is sendable**: a cloned handle may cross a domain boundary as a spawn argument or a channel element, and the source is disposed once, by whichever domain drops the last handle."

**And it supplies it under one restriction that shapes stage 3's whole cleanup story: a shared source is signal-only.** stdlib §6.1: "**The rule: a published source carries no registrations.** A source is **published** — reachable from more than one domain — at three points, each of which sets the property permanently" — `Shared` construction, a `static readonly`/`static owned readonly` field of the type, and being the linked child of a published parent, with publication propagating parent-to-child but not back ("Publication does **not** propagate from child to parent, cancellation propagating only parent-to-child"). On such a source "**`Register` raises `InvalidOperationException`**", and "publishing a source that already holds live registrations raises `InvalidOperationException`" at the publishing operation, so ordering cannot smuggle a registration in. What survives is exactly what a supervisor's stop signal needs: "A published source is therefore **signal-only**: `Cancel`, `CancelAfter`, the deadline set by `WithCancelAfter`, cancel-on-dispose, and propagation from a linked parent all still work and are safe from any domain, because none of them has a user callback to run." stdlib §6.8 states the consequence for spawned work directly: "A source that crosses this way is **signal-only**: it carries no registrations, and `Register` on it raises ([§6.1]) — polling is the cross-domain idiom."

**Stage 3 therefore has no callback-based cleanup path across a domain boundary, and does not want one.** A supervised child observes cancellation by **polling** its token at its own safe points, and performs cleanup through its **own domain-local control flow** — the `using var` discharges and structured exits it already has, which unwind exactly as they do on any other exit path (§28.3). Three candidate escapes are closed and worth naming so nobody re-opens them: registering on the shared source raises; registering on a *linked child* of it raises too, because a linked child of a published parent is itself published; and hoisting the source into a `static readonly` field to dodge `Shared` is the second publication point, so it raises as well. **This proposal takes that as an improvement rather than a cost.** A cross-domain callback was never a shape this design wanted: a registration fires on the *cancelling* domain's thread — the supervisor's — so cleanup written that way would run a child's teardown on its parent, reaching the child's domain-local state from a second domain, which is the one thing §30.14's discipline exists to forbid. Signal-only makes the sound shape the only writable one, and it happens to be the shape stage 3's §4.4 protocol already used: the parent signals, the child notices at a safe point and unwinds itself. The residual cost, recorded honestly: a child blocked in a long `await` that does not observe the token — a non-cancellable `Receive` is the live example (proposal 0060) — notices late or not at all, which is precisely why §4.4's deadline exists and why it can expire.

**And rationale §12.14, landing concurrently, sets the target.** It records Bezel's posture as **the OS process as the recovery unit**, and then says exactly what remains for an in-process supervisor: "It does not decline (1) — a supervisor built on stdlib pieces stays a legitimate future proposal, and its cost drops once the pieces behind it exist (a multiplexable completion set; a cross-domain stop signal, which the v0.263 `Shared<CancellationTokenSource>` ruling supplies). What the posture changes is the target: with process supervision named as the recovery unit, an in-process supervisor is asked to cover only what an OS supervisor covers badly — sub-process-granularity restarts of work whose loss the process should survive — rather than to reproduce OTP." Stage 3 is that proposal, scoped by that sentence.

### 4.2 Policy is a value held by the parent — the Akka lesson, which Bezel's grain enforces

0106 §7.5.5: "policy belongs to the parent *as a value*, never as child lifecycle hooks — and Bezel's grain already favours that, since a capture-free entry cannot carry supervision state." The second half is compiler-enforced. §30.14: "**The entry must be capture-free**: a non-capturing lambda or a static method reference. A capturing lambda or an instance method reference is rejected — captures and receivers are borrows of the origin domain." A child therefore cannot reference its supervisor, cannot register lifecycle hooks with it, and cannot carry restart state across its own death: **every design in which policy lives with the child is unwritable in Bezel.**

```bezel
public enum SupervisionStrategy: byte  OneForOne, AllForOne; end SupervisionStrategy;   // ▲ (see this proposal's §4.5)
public enum RestartKind: byte  Always, OnFault, Never; end RestartKind;                  // ▲ OTP's permanent/transient/temporary, respelled
public record Backoff(Initial: TimeSpan, Max: TimeSpan, Multiplier: double);            // ▲
public record RestartPolicy(                                                            // ▲ a value
  Kind: RestartKind, MaxRestarts: int, Window: TimeSpan,
  Delay: Backoff, ShutdownTimeout: TimeSpan);
```

`RestartPolicy` is a value record of enums, `int`s, `TimeSpan`s and a nested value record — a **sendable value type** under §30.14, copied wherever it goes, owned by nothing, readable at a glance at the spawn site. That is the Akka lesson spelled in the type system.

**The vocabulary is small and closed on purpose.** `RestartKind` is OTP's three — permanent, transient, temporary — **respelled so each name states its rule**: OTP's *transient*-versus-*temporary* is a pair no reader keeps straight without the manual, exactly the forgetting hazard rationale §5.15's naming doctrine exists to remove, and Bezel owes OTP its semantics, not its vocabulary. The discriminant earns its keep here: `OnFault` means *restart on `Fault`, not on `Cancelled` or `Success`* — a one-line `case` over `TaskExitKind`, and unwritable today without string-matching `Message`. Intensity is OTP's (max restarts in a window). `Backoff` is exponential with a cap; **jitter is deliberately absent**, because it would make the supervisor read entropy, and entropy is a stdlib §1 *replay intercept point* — so a jittered supervisor is a determinism question (rationale §12.11, proposal 0045), not a scheduling detail. OTP's `rest_for_one` is **declined as unmotivated**, reopenable by ruling.

### 4.3 Child specs are capture-free factories — the precondition already paid for

0106 §7.4's first surprising finding: "**The hardest precondition of OTP supervision is already compiler-enforced** — §30.14's capture-free entry rule means a restart provably inherits nothing from the dead child, so Bezel is closer to a supervisor than it looks." Rationale §12.14 now says the same in the register's voice: "Bezel has the hard precondition for it already and for free".

So a ▲`ChildSpec` is a small honest thing: a tag, a `RestartPolicy` value, a capture-free entry in the spawn-position category, and the data from which a **fresh** argument is built at each start. The argument must be rebuilt, never reused — the previous one was moved into the dead child and died with it — which is 0106 §7.2's dissolved worry stated positively: "a restarted worker cannot inherit its predecessor's `Channel<T>` … which is exactly Erlang's semantics too; ownership costs nothing here, it costs *shape*."

**This is the one thing stage 3 needs that a plain library cannot give: compiler-known status.** 0106 §3.2 names the constraint — "a supervisor parameterised over a *stored* entry value is not writable in user Bezel" — and stdlib §6.12 states the precedent verbatim for `Parallel`: "This surface is not writable in user Bezel (function values are never sendable); being compiler-known is what lets its call sites reuse the spawn classifier." A `Supervisor` child-registration position taking an entry is therefore a **spawn position**, checked at the call site by the existing classifier, exactly as §30.14 says of `Parallel`: "They introduce **no new checking rule**: their function-typed parameters are **spawn positions** — the call-site category this section defines for `Domain.Spawn` entries, whose stdlib membership §28.14 names — checked at each call site exactly as spawn entries are."

**What stage 3 needs from the language: nothing.** No grammar, no modifier, no derived type property, no checking rule; sendability of arguments and results is §30.14's computation unchanged; the policy is an ordinary record; the supervisor is an ordinary owned object in its parent's domain, not a static. The only spec-text consequence is two *appositive* mentions naming a third member of two existing enumerations (§30.14's spawn-position sentence; §28.14's stdlib retention register) — the pair `Parallel` took at v0.186, changing no program's acceptance. Everything else stage 3 needs, it needs from stages 1–2 and from v0.263. That is the shape of the gating.

### 4.4 Shutdown: ask, then wait with a deadline, then *not* take away

OTP's protocol is ask-then-take-away: signal, wait a bounded time, then `exit(Pid, kill)`. **The take-away half does not exist in Bezel, and v0.263 has now declined it outright** (this proposal's §0). What stage 3 designs is therefore:

1. **Ask.** `Cancel()` on the child's `Shared<CancellationTokenSource>` handle, cloned to the child at spawn — exactly the surface v0.263 grants across domains, and safe from the parent's domain precisely because a published source runs no user callback (this proposal's §4.1). The child observes by **polling** its token at its safe points and unwinds through its own scopes; there is no registration anywhere in this protocol, and none is available — including for a program that parks a process-global shutdown source in a `static readonly` field, since a static is itself a publication point. Cooperative observation is the only cancellation the language has (rationale §12.4).
2. **Wait with a deadline.** Await the child's join against `ShutdownTimeout`. Spelled with landed members that is a `WhenAny` over two **named** task bindings — the join and a `Task.Delay(timeout)` — because §30.10's `WhenAny` takes a borrowing `Array<Task>` and stdlib §6.7's #1813 rule forbids building one from freshly-minted calls. A supervisor on stage 2 already holds the join in its `TaskSet<T>`, so in practice the deadline rides the drain.
3. **The deadline expires.** Not a kill. Three moves, in order:
   - **Report** to stage 1's sink: this child did not stop within its window. This is the first thing that becomes *possible* rather than merely desirable, and it is why stage 1 is stage 1.
   - **Abandon the observation, not the child.** The supervisor stops waiting; the child keeps running, its owned state unreclaimed, its `ChannelSender` keeping its channel open, its eventual fault swallowed at one of this proposal's §1(c) sites — and, with stage 1 subscribed, *seen*. This is §33.3's abandonment shape ("the runtime signals no cancellation, runs no further continuations of theirs … their owned state is reclaimed by the process exit, not by disposal, and their pending effects are lost") applied one level down: a real loss, priced here rather than hidden.
   - **Escalate.** The supervisor treats an unstoppable child as its own supervision failure: it stops what it can and its own join faults. A parent supervisor's policy then applies; at the root the fault reaches `Main` and the process ends at §33.3's exit code 70 — the recovery unit rationale §12.14 names.

**The honest sentence: stage 3 bounds the time to the *escalation decision*, never the time to teardown.** A supervisor can promise it will not wait forever; it cannot promise the thing it stopped waiting for has stopped.

**One asymmetry, because it is why the escalation unit is the process.** Bezel *does* have a forcible take-away — across a **process** boundary: proposal 0011's `Process` disposal SIGKILLs and reaps its child. Within a process the language refuses the primitive for §12.4's reasons; across a process the OS provides it and reclamation is total. That is the correct reading of where guaranteed teardown ends, and it is what §12.14's posture states.

### 4.5 The loop, in prose — and what §12.14 shrinks

The parent holds a `TaskSet<T>` and a `Dictionary<long, ChildRecord>` of specs, policies, restart counters, and each child's `Shared<CancellationTokenSource>` handle. It starts each child with `Domain.Spawn` over the spec's capture-free entry and a freshly built sendable argument, adding the join to the set under the child's tag. It drains completions: per completion it looks up the record, consults `RestartPolicy` against `TaskExitKind` (`Always`, `OnFault` — on `Fault` only — or `Never`; the names carry the table), checks intensity against the window, and either re-spawns after the backoff — adding the replacement to the same live drain, which is precisely stage 2's capability — or escalates. Shutdown runs this proposal's §4.4 over every live child, then closes and drains the set. The notable feature of that paragraph is how little of it is new.

**The predicted shrink: cap stage 3 at `OneForOne`.** `AllForOne` — on one failure, stop and restart *all* siblings — requires stopping a **group** on a bounded schedule, and this proposal's §4.4 has just shown the bounded group stop is exactly the half Bezel does not have. Under `OneForOne` an unstoppable child costs one abandoned child and an escalation; under `AllForOne` it can stall the whole group's restart indefinitely, and the strategy's guarantee (siblings share fate) becomes unenforceable at the moment it matters. Read against §12.14's target sentence — cover "what an OS supervisor covers badly … rather than … reproduce OTP" — `AllForOne` is the reproduce-OTP half. **Recommendation: ship `OneForOne`; record `AllForOne` as blocked on a primitive that does not exist, with escalation-to-process as its answer** (Q5).

---

## 5. What changes now: nothing normative

All three stages are stdlib proposals. No spec rule is added, amended, narrowed, or widened; no derived type property changes; no grammar moves.

- **Newly rejected programs:** none, at every stage. **Newly warned programs:** none — and for a simpler reason than an admission argument: stage 1 adds **no user-written static declaration** for §22.12's rules to classify, so neither rule 2's forms nor rule 4's warning is reached at all (this proposal's §2.2). A program's *own* statics — the `ChannelSender<LogEvent>` or `ILogger` it routes reports through — are classified exactly as they are today, by rules this proposal does not touch.
- **Runtime behaviour of existing programs:** unchanged, with one strictly additive edge — a program that *subscribes* observes reports previously unobservable. A program that does not subscribe behaves identically, including its implementation-defined default reporting.
- **Spec text:** untouched by stages 1–2; stage 3 adds a third named member to two existing enumerations, carrying no rule content (this proposal's §4.3).
- **Conformance:** untouched — nothing here reaches Appendix D's levels. **Fact rulings required:** none — stage 1 claims no membership in any statics-and-sharing derived set (this proposal's §2.2), and stage 3 rides v0.263's `CancellationTokenSource` grant, which is that ruling's business.
- **Contract clauses added to the standard library:** none (this proposal's §2.6). The "never `Success`" restriction on two carriers is documented prose.
- **One stdlib rule is amended, and it is stated as such:** stage 1's sink dispatches on the runtime's own policy rather than stdlib §6.5's first-error-wins (this proposal's §2.3) — a standard-library change, not a language change, and the only behavioural rule this proposal writes that is not simply new surface.

Failure-signal placement, on the record (stdlib §1): stage 1's reports are **band 4** material — "the fault is logged, converted to a protocol answer, retried, or reported, rather than branched on for program logic" — which is why the sink observes rather than returns; stage 2 uses the task machinery's landed exception vocabulary unchanged; stage 3's escalation terminates in **band 5** at the root (§31.4's fatal band, §33.3's exit 70), which is §12.14's territory.

---

## 6. Costs, per stage

| Stage | Surface | Implementation cost | Risk |
|---|---|---|---|
| **1 — fault sink** | 3 types + 1 static class + 2 properties on a landed exception | **Very small.** The five report sites exist and already call an implementation-defined channel; stage 1 tees them. The subscribe machinery is `SharedEventSource<T>`'s. | The dispatch departure (this proposal's §2.3 clause 3) is the one stated divergence a reviewer may reject; `FaultOrigin`'s closed set must be right at v1. |
| **2 — task set** | 2 types | **Small-to-medium**, runtime-implemented for `WhenEach`'s reason (settlement observation is not user-writable). Most machinery exists behind `WhenEach` and `ReceiveAll()`. | The suspend-on-empty-and-open contract must be exact or a supervisor busy-waits; abandonment-on-dispose must be documented loudly (this proposal's §3.3). |
| **3 — supervision** | 6 types, one compiler-known | **Medium**; the only stage needing compiler-known status and two spec appositives. | Scope risk, not soundness risk: this proposal's §4.5 may halve it, and it should not open before §12.14's landed text is read against it. |

0106's confidences — iv **High**, viii **Medium-high**, ix **Medium** — are unchanged by this proposal; none has been raised.

---

## 7. Alternatives

### 7.1 The do-nothing baseline: hand-rolled supervision from documented members

0106 §3.2 is explicit that this composes: "Hand-written one-for-one supervision composes today from documented members." Its shape, entirely from landed surface: the parent owns a `Channel<ChildExit>` where `ChildExit` is a sendable record (tag, a program-declared exit-kind enum, message and stack-trace strings). Each child is `Domain.Spawn(SomeStaticEntry, move arg)` with `arg` a sendable class carrying the child's tag and a `ChannelSender<ChildExit>`; the child's entry wraps its work in `try`/`catch` and **sends its own exit record before returning** — which it must, since `DomainFaultedException` carries only text (this proposal's §1(e)) and a fault out of the entry never reaches the parent's channel by itself. The parent runs `await for var exit in inbox.ReceiveAll()`, looks the tag up in a `Dictionary<long, ChildRecord>`, applies its own counters and delays, and re-spawns. Cancellation needed a *second* channel per child at v0.262 (stdlib §6.8's pattern of the day); v0.263's shared source removes that line item — the baseline gets one channel cheaper, and this proposal says so rather than pricing against a superseded cost. Join handles must still be held and settled, since a bare drop is unsupervised and `FireAndForget` severs the very propagation the supervisor wants.

**Price: roughly a page per program** — and 0106 §7.1's one measured data point is worse than it sounds: proposal 0032's worker-pool probe cost "~60 lines of ceremony for map-square-over-ints" and, in its own words, "supervision is lost". But size is not the objection. The objection is a shape the hand-rolled version cannot factor out: **the re-spawn step is a `case` over a program-declared child-kind enum choosing among statically named entries**, because a stored entry value is not writable in user Bezel (stdlib §6.12). Every new child kind edits that switch. The baseline does not cost a page once; it costs a page that cannot become a library, in every program, forever. That — not line count — is the argument for compiler-known status at stage 3.

**What the baseline gets right and this proposal preserves:** a child reporting its own exit over a channel is *strictly more informative* than any discriminant, because the child can say what it was doing. Stage 3 does not replace that — a child with a rich failure story still sends it over a channel, exactly as §30.14 prescribes ("Programs that need richer fault data return it as a sendable result or send it over a channel"). Stage 1's discriminant is the floor, not the ceiling.

### 7.2 Declined, with reasons

1. **A forcible child stop (`brutal_kill`, domain abort)** — declined at rationale §12.4 as extended at v0.263 (this proposal's §0); the need is routed to §12.14's process supervision. Not this proposal's to mint, and not this proposal's to contradict.
2. **A `Handled` flag on the fault sink** — a supervisor with no policy, no intensity limit, no escalation, no owner (this proposal's §2.7); it would make a fault's fate depend on subscription order.
3. **A public `RuntimeFaults.Report(...)`** — converts a runtime-fault sink into a second application log, which 0021 already is.
4. **The sink as a `Channel<FaultReport>`** — a channel is single-consumer by ownership and its receiving end must stay domain-confined (§22.12 rule 2 form (i): "keep the `Channel<T>` domain-confined and publish a `ChannelSender<T>` static instead"), so exactly one domain could observe faults and a second observer would be unwritable. A publication point is the multi-observer shape and rule 5 is its sanctioned spelling.
5. **A borrowing `TaskSet` twin** — settled already by stdlib §6.7's #1813 ruling (this proposal's §3.1).
6. **Extending `TaskScope` into a supervisor** — its guarantee is lexical and terminal (quiescence at scope exit, first fault cancels siblings, `Join` re-raises once); a supervisor exists to *outlive* its children's failures and replace them. Grafting restart onto a nursery breaks the one property that makes the nursery reasonable (rationale §12.12's map keeps them distinct).
7. **Restart policy carried by the child** (Akka-style lifecycle hooks) — unwritable under §30.14's capture-free rule (this proposal's §4.2).
8. **String-matching `DomainFaultedException.Message` for the exit reason** — the stringly-typed dispatch 0106 §7.5.6 names; the stage-1 rider exists to remove the temptation.

---

## 8. Interactions

- **Proposal 0021 (structured logging, live).** Stage 1's canonical consumer, and a complement rather than a competitor: the sink is a *runtime* fault publication point; `ILogger` is the *application's* facade, and the bridge is a capture-free handler. Stage 1 takes no position on 0021's ambient fork and needs none (this proposal's §2.5). **One placement disagreement, recorded:** 0106 §10 suggested slate iv be "one added section inside 0021, not a competing proposal". This proposal places it in `System.Threading` instead — the vocabulary it mints (`TaskExitKind`) is shared with stages 2–3 and belongs beside `DomainFaultedException`, and the sink is not a logging API. If the owner prefers 0106's placement, §2.5's surface can land as a 0021 section with the vocabulary still declared in `System.Threading` (Q3).
- **Proposal 0023 (`Task.WhenEach`, graduated v0.212).** Stage 2's graduated nearest neighbour; no change to it is proposed (this proposal's §3.4), and its §D2 affine reasoning survives re-derivation.
- **Proposal 0060 (channel give-back, live) — not solved here.** 0106 §10 records the split: slate item ii "removes one of the two preconditions on 0060's cancellable-receive question (the cross-domain half …); **0060's own recorded blocker, the give-back protocol on the cancel edge, is untouched**." Nothing here touches it. Stage 3 drains completions rather than channels, so it does not depend on a cancellable `Receive` — but a *child* blocked in `Receive` stays uncancellable until 0060 lands, which is one more reason this proposal's §4.4 deadline can expire.
- **The two concurrent v0.263 landings.** (i) The `CancellationTokenSource` fact ruling is stage 3's hard gate, and its **signal-only rule for published sources** — `Register` raises on one, and publishing a source that already holds registrations raises at the publishing operation — decides stage 3's whole cleanup story: cross-domain cancellation is a signal, cleanup rides the child's own control flow (this proposal's §4.1, §4.4 step 1). This proposal reads that as a simplification rather than a restriction, for the reason §4.1 gives. (ii) Rationale §12.14 is stage 3's scope-setter (this proposal's §4.1/§4.5) and stage 1's terminal escalation target; §12.4's extended decline is this proposal's §0 binding constraint.
- **Proposal 0011 (`Process`).** The take-away that does exist, and only across a process boundary (this proposal's §4.4).
- **Proposal 0014 (signal hooks).** The external stop trigger — 0106 §3.6 routes the drain-then-exit need to "the cross-domain stop signal, with proposal 0014 supplying the trigger". A supervisor's graceful shutdown is what a SIGTERM subscriber calls.
- **Proposal 0032 (parallel extensions).** Its worker-pool probe is this proposal's §7.1's cost evidence. A supervised `Parallel` is explicitly out of scope: the team is process-wide and its fault policy is fixed at §30.14 — a stage-1 report site, not a stage-3 client.
- **Proposal 0106.** The origin. This implements slate items iv, viii, ix and contradicts nothing in its §7; items i, iii, v, vi, x are untouched.

---

## 9. Questions for the owner

- **Q1 — Stage gating.** Confirm the three stages and their order, with stage 3 gated on stages 1–2 **and** on v0.263's `Shared<CancellationTokenSource>` grant? Recommendation: yes, with stage 1 opening immediately — it is small, depends on nothing, and makes the other two cheaper by minting the shared vocabulary.
- **Q2 — Does a type-carried guarantee tolerate a per-implementation dispatch discipline?** The admission story itself is narrow and, this proposal believes, uncontroversial (this proposal's §2.2): the sink declares no user-written static, so §22.12's statics rules are not reached at all, and the one thing it takes from rule 5 — capture-free handlers — transfers because rule 5 keys that on the *declaring type of the subscribe position*, giving it free with no §28.14 register entry. The real question is this proposal's §2.3 clause 3. Stage 1's sink dispatches to **all** handlers rather than stopping at the first that throws. Read one way this is no departure at all: stdlib §6.5 ends first-error-wins by delegating exactly this to the raiser — "To run all handlers regardless of failures and aggregate exceptions, the raiser must implement that policy explicitly" — and the runtime is this sink's only raiser. Read the other way, a subscriber holding an `ISharedEvent<T>` now gets a dispatch discipline that depends on which implementation is behind the face, which is the alias-dependent surprise §22.12 rule 5 was built to eliminate for the capture-free guarantee ("the guarantees are carried by the type and therefore hold at **every** subscription site, aliases included"). Recommendation: the raiser-side reading — first-error-wins was never among the guarantees the *type* carries, and a diagnostic sink where one broken observer blinds the rest is the worse failure. Confirm, or require the sink to take first-error-wins like any other source?
- **Q3 — Placement of stage 1.** Here in `System.Threading`, or as a section inside proposal 0021 as 0106 §10 suggested? Recommendation: here, for this proposal's §8's reasons; the fallback costs only a section move.
- **Q4 — One exit vocabulary or two.** `TaskExitKind` = {`Success`, `Fault`, `Cancelled`} serves stage 2's completions and stage 3's policy table, and — restricted to two cases **by documented prose, not by a contract clause** (this proposal's §2.6) — `DomainFaultedException.ExitKind` and `FaultReport.Kind`. Accept the unified enum with the documented restriction, or split it into a fault-only kind and a settlement kind so that no carrier declares a case it cannot hold?
- **Q5 — Stage 3's strategy set, against §12.14.** Ship **`OneForOne` only**, recording `AllForOne` as blocked on the bounded group stop this proposal's §4.4 shows does not exist, with escalation-to-process as its answer (this proposal's §4.5)? Or carry `AllForOne` with its unbounded-stall caveat stated?
- **Q6 — Stage 2 standalone.** If stage 3 is never taken up, does `TaskSet<T>` still earn its place as a general detection primitive (0106 §7.5.4: "The missing detection primitive, useful well beyond supervision")? Recommendation: yes — dynamic fan-in over a changing worker set is the ordinary service shape, and stdlib §6.7's #1813 ruling already points such programs at owning transfer, which is what stage 2 gives them a home for.
