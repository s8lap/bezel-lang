# Proposal 0217 — The original's type name at a domain join: a cause snapshot in `CauseChain`

| | |
|---|---|
| **Status** | accepted — accepted (2026-11-26, owner: all recommended answers); critiqued (pc-0217, SOUND-WITH-FIXES), revised |
| **Tracking issue** | [#8401](https://github.com/s8lap/bezel-lang/issues/8401) (spec-question: `DomainFaultedException` drops the original exception's type name). Related: [#8669](https://github.com/s8lap/bezel-lang/issues/8669) (a cancelled child joining as cancellation; independent, §4.4), [#8402](https://github.com/s8lap/bezel-lang/issues/8402) (obsolete, §1.3) |
| **Target documents** | **spec** — §30.14 *Faults* (two sentences), §30.14 *Data-parallel combinators* (one clause of the *Results and faults* bullet), §31.4 (one sentence added to *Cause chaining is by snapshot*, one clause of the supersession paragraph). **stdlib** — §6.8 (one comment line in the listing, one sentence of prose), §6.12 (one clause). **rationale** — §12.6 (one clause, one declined item). **Guide 4** §5 (one sentence), **Guide 2** §4 (one clause). **code-generation reference** — §5.6 (one comment line in the `Domain` listing). **proposals** — `README.md` (this row), `DECLINED.md` (the rows of §5, at acceptance), proposal 0110's status note (Q2). No grammar change, no new member, no new type, no new diagnostic. |
| **Written against** | spec, stdlib and rationale **v0.360** (`origin/main` `bd12fe038`). v0.361 (branch `spec/v0361-2026-11`, `c85ce271c`, landing in parallel) does not touch the sections this proposal edits: every `Old` text of §6 occurs exactly once in both trees (checked by script, §11). v0.361's new §31.2 sentence on per-raise `SupersededChain` clearing does bear on Q3, and is cited there. Probes on compiler release **r42** (`bezelc 0.0.0-8ccf7ff0`). |
| **Origin** | Issue #8401, filed from the error-model research note ([`docs/research/error-model/README.md`](../research/error-model/README.md) §10 item 1, probe `p04_domain_fault.bzl`) for proposal 0211 to answer. Proposal 0211 (v0.356) answered the band half and left the type half open ([its §6.3](0211-failure-marker-and-boundary-arm.md)); the triage of the v0.361 run classified the rest as a design change. |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): it adds to what crosses a domain boundary, which proposal 0211 §6.3 recorded as one fact and nothing else, and it changes observable runtime content (a join exception's `CauseChain`, its `ToString()` rendering and the §33.3 report). It reopens no `DECLINED.md` row (§2.4). After acceptance it lands as a ruling-class revision. |

*Conventions. An unprefixed § is a language-spec section at v0.360; stdlib, rationale, guide and reference sections are marked. Quotations are verbatim, with link markup dropped. ▲ marks a name or text this proposal introduces. Evidence labels as in proposals 0211 and 0214: **MEASURED** (a probe or a script, re-runnable; the probe programs are reproduced in §2.2 and §8), **QUOTED** (repository text), **HYPOTHESIS** (a judgement or prediction). Probe results describe r42, which implements an older revision than the spec by design; they are informational.*

## 0. Summary

When a domain's entry throws, the join raises `DomainFaultedException` (or `DomainFailedException` for an `IFailure`-marked class), carrying the original's message and stack-trace text. The original's type name does not cross. A fault whose message is empty, such as the runtime's `IndexOutOfRangeException`, reaches the awaiter, its handler, its `ToString()` and the uncaught-exception report with nothing that says what it was.

The options are: leave it (A); put a snapshot of the original into the join exception's inherited `CauseChain`, as the `(message, cause)` constructor would (B, the issue's candidate); add a dedicated property (C); carry a dispatchable type (D); or write the name into `Message` (E).

**Recommendation: B.**

- It adds no member, no type and no arm pattern that names it. The class the join raises stays the one fact a catch arm's type pattern reads (rationale §12.6), and the type name is diagnostic text, which is how proposal 0211 §6.6 already treats `ExceptionInfo.TypeName` ("the entry's `TypeName` identifies it in a log").
- The rules that print exceptions already render `CauseChain`: `ToString()` (§31.4) and the uncaught report (§33.3). So the issue's repro, and every log line and crash report of a child domain's fault, name the original with no further text.
- It composes across nested spawns and through `Parallel.*`, and it carries the original's own causes.
- For synchronous entries it is a runtime change of about 20–40 lines in one function, reusing the existing snapshot helper; sema and codegen already lower `CauseChain` on every exception (§3.2). The runtime work is sequenced after #8634 (§9).
- Nothing in the tests or the rule corpus pins an empty chain or the old rendering on a join exception (§7).

Declined: a dedicated name property (proposal 0110's `FaultTypeName` rider, which B subsumes), a `Type`-valued property, the name in `Message`, and (subject to Q3) carrying the original's `SupersededChain` (§5).

## 1. The question and its history

### 1.1 The issue

#8401 asks (QUOTED): "When a domain's entry throws, the awaiter receives `DomainFaultedException`, which carries `Message` and `FaultStackTrace` only. The original exception's type name is not carried, although `ExceptionInfo` (spec §31.4) has `TypeName` for exactly this kind of snapshot. … A caller cannot tell a band-1 bug from a band-4 failure, or one failure type from another, on the far side of a domain." Its interpretation 2 is the candidate fix: "populate the `DomainFaultedException`'s `CauseChain` with the original's snapshot (type name, message, stack trace), using the existing snapshot machinery."

### 1.2 What v0.356–v0.359 answered

- **The band.** Proposal 0211 (v0.356) carried one bit across the seam: the join raises `DomainFailedException`, a subclass of `DomainFaultedException` that implements `IFailure`, when the original's class implements the marker (its §6.3). Proposal 0214 (v0.358) narrowed the marker's meaning to origin ("the outside world disagreed", spec §31.4) and kept `DomainFailedException` (its §3.5). So the first half of the issue, telling an `IFailure`-marked exception from one whose class makes no statement, is answered.
- **The type.** 0211 §6.3 says one fact about the original crosses "with the two strings"; the maintainer's comment on #8401 (2026-11-10) reads it as "one bit across the seam … and nothing else". 0211 §6.6 declined a kind field on `ExceptionInfo`, saying the entry's `TypeName` "identifies it in a log". The same comment records that the type half "remains this issue's open question, for its own ruling".
- **The story.** v0.359 ("one story, one voice") restated the domain case without changing it: "A domain join raises `DomainFaultedException`, or `DomainFailedException` for an `IFailure`-marked class" (QUOTED, v0.359 changelog). Its R3 made explicit that a cancelled child joins as plain `DomainFaultedException`, and filed #8669 for crossing as cancellation instead.

### 1.3 #8402 is obsolete and does not bear on this

#8402 asked whether one exception type serving two bands (`OverflowException`, `InvalidOperationException`, `ArgumentException`) defeats type-keyed handling. Its v0.356 answer (`ParseRangeException`) was retired by v0.358, which made the marker a statement of origin on the class and left whether a condition was expected to the handler (proposal 0214 §3.4). This proposal does not revive a type-keyed band: the type name it carries is for reading, and the class the join raises stays the only thing a catch arm selects on (§4.1, reason 2).

## 2. The text today, traced

### 2.1 What the documents say

- Spec §30.14 *Faults* (QUOTED): "The join instead raises `DomainFaultedException` (stdlib §6.8), whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). The failure marker crosses with them … Programs that need richer fault data return it as a sendable result or send it over a channel."
- Stdlib §6.8 (QUOTED): "The original exception object never crosses the boundary (no exception class is sendable — §1); `Message` and `FaultStackTrace` are copied strings." The listing declares `FaultStackTrace` and a comment "Message carries the original exception's message text."
- Spec §31.4 declares `CauseChain: Array<ExceptionInfo>; // empty when no cause` on `Exception`, and `ExceptionInfo(TypeName: string, Message: string, StackTrace: string)`. "the `(message, cause)` constructor borrows `cause` for the duration of the call and copies its type name, message, stack-trace text, and its own `CauseChain` into the new exception's `CauseChain` (outermost cause first, flattened)."
- `ToString()` renders "the receiver's qualified type name", its non-empty `Message`, and "every entry of the receiver's `CauseChain`" and `SupersededChain`, each entry by "its `ExceptionInfo`'s `TypeName` and its `Message` when that is non-empty" (QUOTED, §31.4).
- The uncaught report prints "the exception's type, message, stack trace, `CauseChain`, and `SupersededChain`" (QUOTED, §33.3).
- Rationale §12.6 (QUOTED): "Copying the message and stack-trace text (strings — sendable) preserves the diagnostic value, and the class the join raises carries the one fact a handler selects on — whether the fault was `IFailure`-marked".

A `CauseChain` filled by the implementation, not by a constructor the program calls, already exists: §9.8's synthesized `Deserialize` records the `ContractViolationException` in its `SerializationException`'s `CauseChain` "in §31.4's snapshot form" (QUOTED, §9.8).

The runtime builds the join's exception itself; no constructor is named. Read with §30.14's enumeration of what crosses and 0211 §6.3's "nothing else", the join exception has no cause, so its `CauseChain` is empty.

### 2.2 The repro, on every path

The issue's program (QUOTED from #8401):

```bezel
module P;
import System.Threading;
public class Program
  public static method BadIndex(): int
    var xs := new Array<int>(2, 0);
    return xs[5];
  end BadIndex;
  public static async method Main(): Task
    using var u := Domain.Spawn(Program.BadIndex);
    try
      Console.WriteLine(await u);
    catch
      var ex: DomainFaultedException => Console.WriteLine($"'{ex.Message}' / {ex.ToString()}"),
    end try;
  end Main;
end Program;
```

1. **Handled at the join.** `xs[5]` raises `IndexOutOfRangeException` (§31.4's tree; the indexer is its producer). The spec does not fix the indexer's message; on r42 it is empty (MEASURED, `q1`). It propagates out of the entry. Its class does not implement `IFailure`, so the join raises plain `DomainFaultedException` with `Message = ""` and `FaultStackTrace` the original's trace text (`""` where traces are unavailable, §31.4). The arm takes it. `ToString()` renders the qualified type name `System.Threading.DomainFaultedException`, no message (empty), and no chain entries (both chains empty). Nothing printed names the original.
2. **Uncaught.** Without the `try`, the join's exception escapes `Main`. §33.3 prints its type, an empty message, the trace text and two empty chains, and exits 70. The report says `DomainFaultedException` and nothing else.
3. **An `IFailure`-marked original.** With `throw new FileNotFoundException("app.toml")` in the entry, the join raises `DomainFailedException`; an `IFailure` arm or a `DomainFaultedException` arm takes it, and `Message` is `"app.toml"`. The handler learns the origin (the marker) and the message, not that it was a missing file rather than a permission error or a dropped channel.
4. **Cancellation.** An entry ending in `OperationCanceledException` joins as plain `DomainFaultedException` (§30.14, v0.359 R3; rule case `s30_14_0021`). Its message is whatever the raiser wrote, often empty. A log of the join cannot show that the child was cancelled rather than broken; Guide 4 §9 tells a supervisor to check its own source's `IsCancellationRequested` for the decision, which still works, but the log line has nothing.
5. **`Parallel.*`.** The first body fault surfaces as the same vocabulary (§30.14, *Results and faults*; stdlib §6.12), and stdlib §6.12 lets the implementation put the failing element's index into `Message`. The original type is lost the same way.
6. **The spec's own examples.** §30.14's spawn examples (`Bump`, `TickAsync`) have no fault path. The channel example's `await worker;` comment says a fault "surfaces here as DomainFaultedException (§30.14)", which is path 1.

MEASURED (r42; probe files under `/var/tmp/bezel-scratch/v361/p-0217/`):

| Probe | Program | Output |
|---|---|---|
| `q1_repro` | the repro, plus `ex.CauseChain.Count` | `'' / System.Threading.DomainFaultedException`, then `causes=0` |
| `q2_uncaught` | the repro without the `try` | `Unhandled exception: System.Threading.DomainFaultedException`, exit 70 |
| `q3_cause_local` | the same fault wrapped in one domain by `new InvalidOperationException("wrapped", e)` | `causes=1 first=System.IndexOutOfRangeException` |
| `q4_uncaught_cause` | that wrap, uncaught | `Unhandled exception: System.InvalidOperationException: wrapped` / `Caused by: System.IndexOutOfRangeException` / `Superseded: System.IndexOutOfRangeException` (the `Superseded:` line is an r42 deviation; §31.2 does not make an arm-body throw a supersession) |
| `p04` (research) | `FileNotFoundException("app.toml")` in the entry | `matched DomainFaultedException: Message='app.toml'` |

The contrast between `q2` and `q4` is the issue in one line: the same fault, wrapped by a constructor in one domain, is reported with its type; crossing a domain, it is not. (r42's `ToString()` renders the type name only, not even `Message` — MEASURED by the critic's `t1_tostring.bzl` — a known lag behind §31.4's rendering rule, tracked by #3302; the report path already renders `CauseChain`.)

### 2.3 What a program can do today

Return the data as a sendable result, or send it over a channel (§30.14). For a fault the program did not plan for, such as a runtime index fault, that means wrapping every entry:

```bezel
public static method Entry(): int
  try
    return Work();
  catch
    OperationCanceledException => throw,
    var f: IFailure => throw new EntryFailure(f.ToString()),     // a user class listing IFailure
    var e: Exception => throw new InvalidOperationException(e.ToString()),
  end try;
end Entry;
```

The wrap works, at a cost. Every entry needs it. The program must declare a marked carrier class, or the `IFailure` marker is lost across the join. The arms must keep cancellation apart by hand, or the v0.359 rule "cancellation is never swallowed" is broken. And the original's message is replaced by a rendering whose arrangement is implementation-defined (§31.4). Code the program does not own, such as a library's entry, cannot be wrapped at all.

### 2.4 Why this is design-level

The text is not unclear. §30.14 lists what crosses, `CauseChain` is "empty when no cause", and 0211 §6.3 recorded the one-bit decision. Carrying more is a decision about the domain boundary, with a choice of surface (§3) that changes observable output and that proposal 0110 (under review) also touches with a different surface (its §2.6). So it is not a clarification. It reopens no `DECLINED.md` row: the sendability decline and deferral (rationale §12.6; `DECLINED.md`, *Sendability requirement on throwable types* and *Stating `Exception`'s inherited state transfer-compatible*) concern the exception object, which still does not cross; the *kind field on `ExceptionInfo`* row concerns the record's shape, which does not change.

## 3. The options

Each option is traced through §2.2's six paths. "Cost" estimates come from reading `src/` (read-only).

### 3.1 A — leave as is

The type name stays behind. §2.3's wrap is the remedy, and the issue gets a recorded decline.

- Paths 1–6: unchanged.
- Soundness: nothing to check.
- Ergonomics: the one place in the language where a fault loses its type before it is printed. The cost of §2.3 falls on every spawn, and on cancellation handling. A program that forgets the wrap gets crash reports that say `DomainFaultedException` and nothing more.
- Cost and migration: none.

### 3.2 B — a cause snapshot in the inherited `CauseChain` (recommended)

The join exception's `CauseChain` holds the entries the `(message, cause)` constructor would copy with the original as its cause: first ▲ the original's type name, message and stack-trace text, then the original's own `CauseChain`. The snapshot is taken in the spawned domain, before the original is disposed. Only strings cross, in an `Array<ExceptionInfo>` the runtime builds for the join exception. `Message` and `FaultStackTrace` keep their meaning.

- Path 1: `ex.CauseChain` is `[ExceptionInfo("System.IndexOutOfRangeException", "", <trace>)]`. `ToString()` renders the join's type, then that entry, so the repro prints `'' / System.Threading.DomainFaultedException` followed by the entry naming `IndexOutOfRangeException`, arranged as the implementation renders causes.
- Path 2: §33.3's report includes `CauseChain`, so the crash report names the original, as `q4` does today for a local wrap.
- Path 3: `DomainFailedException`'s chain names `FileNotFoundException`. The arm the handler picks is unchanged.
- Path 4: the chain names `OperationCanceledException`, so the log shows a cancelled child. The decision rule of Guide 4 §9 is unchanged (§4.4).
- Path 5: the returned task's exception carries the snapshot of the first body fault. The entry's message is the original's even where the implementation decorates `Message` with the element index.
- Path 6: unchanged, apart from path 1's content.
- Nested spawns: an outer entry that awaits an inner join and lets its exception propagate ends with the inner join's exception. The outer join's chain is then `[snapshot(inner join exception)] ++ inner chain`: the inner join class first, then the original. The marker composes as today (0211 §6.3).
- Soundness: no object crosses. A `CauseChain` the implementation fills is not new: §9.8's synthesized `Deserialize` already records one in §31.4's snapshot form. `ExceptionInfo` is a record of three `string`s, and `string` is sendable (§30.14). The array is built by the runtime for the exception it raises at the join, the same way that exception's `Message` string is today. No catch rule, ownership rule or sendability verdict changes. The original's disposal in the child is unchanged (the runtime's `bz_exc_end_handling` path).
- Ergonomics: no new name to learn. The data sits where a reader of any wrapped exception already looks, and the printing rules show it with no code. A reader iterating `CauseChain` for the root cause finds it. A non-empty message is rendered twice, once as the join's `Message` and once in the first entry: the same duplication §3.5 counts against E, for the message instead of the name; it is accepted because `Message` keeps its stdlib §6.8 meaning.
- Cost (HYPOTHESIS, from `src/`): runtime only. `bz_domain_spawn`'s fault branch (`src/runtime/bezel_runtime_concurrency.cpp`) already reads the original's message at the header's offset 8 before disposing it. It would also read the original's type name, and its `CauseChain` at offset 24, which every exception subclass carries (the header the #2950 layout test pins). It would then call `bz_exc_causechain_link` and store the result in the join exception's `CauseChain` slot. Sema and codegen need nothing: `CauseChain` is lowered on every exception (`LlvmBackend_Exprs.cpp`, the `CauseChain` member arm). The type name must not come from the virtual `ToString` slot, the channel the two existing snapshots use (`src/runtime/bezel_runtime_itable.cpp`, the #3261 append): that channel is #8634, which records a wrong value under a user override, user code running mid-snapshot, and a leak of the `ToString` result. Here a user override would run inside the runtime continuation, between the `bz_exc` save and restore, with no exception edge. So the work is sequenced after #8634 (§9). `Parallel` (#3557) and async entries (#1523) are unimplemented and take the rule when built. Estimate, for synchronous entries: 20–40 lines and three or four tests, one of them a leak loop over a faulting spawn (the CLAUDE.md vacuous-oracle rule).
- Migration: a join exception's `CauseChain.Count` goes from 0 to at least 1; its `ToString()` and the §33.3 report gain lines. §7.1 lists the tests and corpus cases checked: none flips.

### 3.3 C — a dedicated property

C1: ▲ `FaultTypeName: string` on `DomainFaultedException`, proposal 0110's rider, "diagnostic text only — NOT a dispatch surface" (QUOTED, 0110 §2.6). C2: ▲ `Fault: ExceptionInfo`, one record holding the original's three strings.

- Path 1: the name is reachable as `ex.FaultTypeName` or `ex.Fault.TypeName`. `ToString()` does not render it unless §31.4's rendering rule gains a class-specific clause or the class gains a specified override, so the repro's output, which prints `ToString()`, still says nothing.
- Path 2: the same. §33.3's report lists its content, and a new property is not in the list without a new clause.
- Paths 3–5: as B for code that reads the property; as A for anything that prints.
- Nested spawns: the property names the inner join class; the original is one level further down, in a property of an exception that no longer exists. C2 could carry the original's `CauseChain` only by widening `ExceptionInfo`, which §31.4 records as declined.
- Soundness: as B.
- Ergonomics: discoverable on the class's listing, and C1 states its purpose in its name. But a second place to look beside `CauseChain`, which wrapped exceptions already use, and a value a handler may be tempted to branch on, which 0110 §2.6 tries to forbid by documentation.
- Cost: sema and codegen member arms beside `FaultStackTrace` (`ExprTypecheck_Calls.cpp`, `LlvmBackend_Exprs.cpp`), a runtime slot past the header, the builtin-registry snapshot test, and the stdlib §2.17 enumeration of declared surfaces. Larger than B, plus the printing clauses if the output is to name the type.
- Migration: none for existing programs, except a user subclass of `DomainFaultedException` that declares a member of the same name (the exception family is open to inheritance, rationale §12.6).

### 3.4 D — a dispatchable type

D1: ▲ `FaultType: Type`. `Type` is immutable and self-distributing, one instance per type in the process (stdlib §2.13), so it could cross. `typeof(IOException).IsAssignableFrom(ex.FaultType)` would be a subtype-aware test. D2: re-raise the original's class at the join.

- D2 is the sendable-exception design that rationale §12.6 declines ("special-casing 'exception objects may cross anyway' would reopen the invariant exactly where programs are least disciplined"). It is not reopened here.
- D1 is sound, but it adds a second dispatch surface beside the class the join raises, keyed on the child's concrete types. That is the coupling §30.14's "return it as a sendable result" keeps explicit. Its cost is the largest: no member of `Type` is reachable from a running program today (stdlib §2.13's marker: #3819, #487, #511). It does not print either, as C.
- Declined (§5).

### 3.5 E — the name in `Message`

The join's `Message` becomes, for example, `"IndexOutOfRangeException: "` followed by the original's message.

- Paths 1–5: every printer shows the type. But `Message` stops being "the original exception's message text" (stdlib §6.8), handlers that print `Message` change their output, and `ToString()` shows the name twice once the rendering lag is fixed. Recovering the message means parsing. Rule cases `l6_8_0003`, `l6_8_4302` and `s30_14_0019` read the copied message and would flip.
- Declined (§5).

### 3.6 Comparison

| | A | B | C | D1 | E |
|---|---|---|---|---|---|
| Repro output names the original | no | yes | no (without a printing clause) | no | yes |
| Crash report names it | no | yes | no (without a §33.3 clause) | no | yes |
| Original's own causes cross | no | yes | C2 only by widening `ExceptionInfo` | no | no |
| New member or type | — | none | one member | one member | none |
| New dispatch surface | — | none (by documentation; a `when` guard can read it, as C1's property) | none (by documentation) | yes | none |
| Runtime / sema / codegen work | — | runtime only | all three | all three, plus `Type` | runtime only |
| Existing tests or cases flipping | — | none found | none found | none found | three rule cases |

## 4. Recommendation: B

### 4.1 Reasons

1. It answers the issue as filed, with the mechanism the issue names, and the answer reaches every place a fault is read by a person: the handler's `ToString()`, the uncaught report, and logs that render the exception.
2. It keeps rationale §12.6's design whole. The exception object does not cross, and the class the join raises remains "the one fact a handler selects on". The name crosses as text, in the record the language already uses for text about other exceptions.
3. It is the smallest change: no member, no type, and no arm pattern that names it; like any string a `when` guard could test it, which G1 tells readers not to do. A program that never reads `CauseChain` sees only longer output.
4. It composes: nested spawns, `Parallel.*`, the original's own causes, both join classes.
5. It is cheap and local to the runtime, and the snapshot helper it needs is already tested by two snapshot paths.

### 4.2 The rule, precisely

- **Content.** The join exception's `CauseChain` is the chain the `(message, cause)` constructor would build with the original as `cause`: first the original's type name, message and stack-trace text, then each entry of the original's own `CauseChain`, in that chain's order. The type name is the one the cause snapshot copies (§31.4 "its type name"); whether to pin its form is split out as a separate clarification (§10).
- **Which joins.** Every exception that ends an entry and is raised at the join: `DomainFaultedException` and `DomainFailedException` alike, cancellation included, and the exception `Parallel.*` raises for its first body fault.
- **When.** In the spawned domain, while the original is still alive, before its handling ends and it is disposed. Only the snapshot's strings cross, as the `Message` copy does.
- **What it does not carry.** The original's `SupersededChain` stays behind; the `(message, cause)` constructor's snapshot also copies a cause's "own `CauseChain`" and nothing more (§31.4). This loss is in tension with rationale §11.8, and Q3 puts it to the owner.
- **`Message` and `FaultStackTrace`** are unchanged. For a `Domain.Spawn` join, the first entry's `Message` and `StackTrace` are the same text as the join's `Message` and `FaultStackTrace`. For `Parallel.*`, `Message` may carry the element index (stdlib §6.12), and the first entry keeps the original's message.
- **Allocation.** The array is allocated like the join exception and its message string: under §17.7's ordinary rule, not the best-effort rule §31.2 gives the supersession append. The join must raise an exception in any case, and an allocation the join cannot make terminates the process as the exception's own allocation would.

### 4.3 What does not change

The classes the join raises, and which arm takes them. The marker's crossing (§30.14). Ownership and disposal of the original in the child (§31.1). `ExceptionInfo`'s shape. The `(message, cause)` constructor and its purity. `SupersededChain` and the later-wins rule (§31.2). The arrangement of `ToString()` and of the §33.3 report, both still implementation-defined apart from their stated content. Every compile-time verdict.

### 4.4 The cancellation case and #8669

Path 4's entry names `OperationCanceledException`. That makes a cancelled child visible in logs. It does not make a type-name test the way to decide: a supervisor still reads its own source (Guide 4 §9), and the guide text of §6.9 says so. If #8669 decides that a cancelled child crosses the join as cancellation, that case leaves the `DomainFaultedException` path, and this proposal's rule simply no longer applies to it. The two proposals are independent in both orders.

### 4.5 Relation to proposal 0110

Proposal 0110 (under review, stage 1) adds ▲ `ExitKind` and ▲ `FaultTypeName` to `DomainFaultedException` (its §2.6). B carries the type name without the property, and with the printing and the causes that C1 lacks. If B is accepted, `FaultTypeName` is subsumed, and 0110's rider would keep `ExitKind` alone, which is #8669's territory (Q2). This proposal does not edit 0110.

### 4.6 Relation to the v0.359 story and proposal 0213

The story says to name the types you handle and let everything else propagate, and to put a broad arm only at a boundary. B adds nothing a handler names. A boundary that logs what it took (`IFailure => Log(f)`, rationale §11.3) logs more. Proposal 0213's warning reads arms, not chains, and is unaffected.

## 5. Declined, and why

Row text for `DECLINED.md` (`declined-at-proposal`), added at acceptance (2026-11-26, B accepted) (`DECLINED.md` scope rule 1; 0212's rows landed the same way):

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| A `Type`-valued property carrying the original exception's runtime type across a domain join (`FaultType: Type`), and re-raising the original's class at the join | `declined-at-proposal` | [proposal 0217 §3.4](0217-domain-join-cause-snapshot.md); rationale §12.6 | Programs that must act on a child's exception type and cannot return it as a sendable result, counted; and an implemented `Type` (#3819). The class the join raises is the one fact a handler selects on; the re-raise half is rationale §12.6's sendability decline, not reopened. |
| A dedicated type-name property on `DomainFaultedException` (`FaultTypeName: string`, proposal 0110 §2.6's rider) in place of the cause snapshot | `declined-at-proposal` | [proposal 0217 §3.3](0217-domain-join-cause-snapshot.md) | Evidence that readers do not find the original's name in `CauseChain`. The snapshot carries the name, is rendered by `ToString()` and by the uncaught report, and carries the original's causes; a property does none of the last three without further clauses. |
| Writing the original's type name into the join exception's `Message` | `declined-at-proposal` | [proposal 0217 §3.5](0217-domain-join-cause-snapshot.md) | —(none stated). `Message` is the original's message text (stdlib §6.8); a prefix changes every handler's printed output and makes the message a string to parse. |
| Carrying the original's `SupersededChain` across a domain join (if Q3 confirms the loss) | `declined-at-proposal` | [proposal 0217 §4.2, §10 Q3](0217-domain-join-cause-snapshot.md); spec §31.2, §31.4; rationale §11.8 | Field evidence of a child's cleanup failure hiding the fault that mattered, at a join. `SupersededChain` records what this exception superseded (§31.2), and v0.361's per-raise clearing would drop carried entries at a second awaiter's re-raise. The loss is the one place rationale §11.8's "the buried failure survives" does not hold, recorded as such. |

If the owner chooses A instead, the row is:

| Topic | Verdict class | Authoritative record | Reopening bar |
|---|---|---|---|
| Carrying the original exception's type name across a domain join, by a cause snapshot in the join exception's `CauseChain` or by a property (#8401) | `declined-at-proposal` | [proposal 0217](0217-domain-join-cause-snapshot.md); spec §30.14 *Faults* | Crash reports or logs from programs written against a compiler that implements the join, where an unidentified child fault cost a diagnosis. The documented remedy is a sendable result or a channel (§30.14). |

## 6. Proposed normative text

Each `Old` occurs exactly once in its document at v0.360 and at the v0.361 branch (§11). The edits apply in the order given.

### 6.1 Spec §30.14 *Faults* (S1)

Old:

````text
The join instead raises `DomainFaultedException` (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)), whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable).
````

New:

````text
The join instead raises `DomainFaultedException` (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)), whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable), and whose `CauseChain` ([§31.4](#sec-standard-exception-types)) carries a snapshot of the original: the entries the `(message, cause)` constructor would copy with the original as its cause — first the original's type name, message and stack-trace text, then the original's own `CauseChain`. The snapshot is taken in the spawned domain before the original is disposed, and only its strings cross.
````

### 6.2 Spec §30.14 *Faults*, the last sentence (S2)

Old:

````text
Programs that need richer fault data return it as a sendable result or send it over a channel.
````

New:

````text
The snapshot is diagnostic text, which `ToString()` and [§33.3](#sec-exit-code)'s report render; the class the join raises remains the one fact a catch arm's type pattern selects on. Programs that need richer fault data, or need to act on the original's type, return it as a sendable result or send it over a channel.
````

### 6.3 Spec §30.14 *Data-parallel combinators*, *Results and faults* (S3)

Old:

````text
  the returned task faults with `DomainFaultedException` (`DomainFailedException` where that first fault's class is `IFailure`-marked) — this section's fault vocabulary, with the no-aggregation
````

New:

````text
  the returned task faults with `DomainFaultedException` (`DomainFailedException` where that first fault's class is `IFailure`-marked), whose `CauseChain` holds the snapshot of that fault as *Faults* above states — this section's fault vocabulary, with the no-aggregation
````

### 6.4 Spec §31.4, *Cause chaining is by snapshot* (S4)

Old:

````text
into the new exception's `CauseChain` (outermost cause first, flattened). The cause *instance* is never retained
````

New:

````text
into the new exception's `CauseChain` (outermost cause first, flattened). The exception a domain join raises carries the same snapshot of the exception that ended the entry, built by the runtime on these terms ([§30.14](#sec-multiple-domains)). The cause *instance* is never retained
````

### 6.5 Spec §31.4, the supersession paragraph (S5)

Old:

````text
`CauseChain` records what a *constructor* was told about causation, `SupersededChain` what the *runtime* observed at supersession.
````

New:

````text
`CauseChain` records causation — what a *constructor* was told, or what the implementation records in the same snapshot form where this specification says so (a domain join's exception, [§30.14](#sec-multiple-domains); a synthesized `Deserialize`'s contract failure, [§9.8](#sec-serialization-synthesis)) — and `SupersededChain` what the *runtime* observed at supersession.
````

### 6.6 Stdlib §6.8 (L1, L2)

L1, the listing. Old:

````text
  // Message carries the original exception's message text.
end DomainFaultedException;
````

New:

````text
  // Message carries the original exception's message text.
  // CauseChain (inherited) carries the original's snapshot, then the original's own CauseChain.
end DomainFaultedException;
````

L2, the prose. Old:

````text
`Message` and `FaultStackTrace` are copied strings. The failure marker crosses with them
````

New:

````text
`Message` and `FaultStackTrace` are copied strings, and the inherited `CauseChain` is a snapshot of the original (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)): its first entry holds the original's `TypeName`, `Message` and `StackTrace`, and the original's own `CauseChain` follows, so `ToString()` and the uncaught-exception report name the original's type. The failure marker crosses with them
````

### 6.7 Stdlib §6.12 (L3)

Old:

````text
surfaced at the `await` as `DomainFaultedException`, or as `DomainFailedException` where that fault's class is `IFailure`-marked ([§6.8](#sec-domain)) (implementations
````

New:

````text
surfaced at the `await` as `DomainFaultedException`, or as `DomainFailedException` where that fault's class is `IFailure`-marked, either one carrying that fault's snapshot in `CauseChain` ([§6.8](#sec-domain)) (implementations
````

### 6.8 Rationale §12.6 (R1, R2)

R1. Old:

````text
Copying the message and stack-trace text (strings — sendable) preserves the diagnostic value, and the class the join raises
````

New:

````text
Copying the message and stack-trace text (strings — sendable), with a cause snapshot of the original in `CauseChain` that carries its type name, preserves the diagnostic value, and the class the join raises
````

R2. Old:

````text
Declined: a sendability requirement on throwable types (intrusive, retroactive) and runtime graph-checking of exception objects (the runtime-check costs above, on the unhappy path).
````

New:

````text
Declined: a sendability requirement on throwable types (intrusive, retroactive) and runtime graph-checking of exception objects (the runtime-check costs above, on the unhappy path). Also declined, for the type name: a dedicated property, or a `Type`-valued one, beside the class ([proposal 0217](proposals/0217-domain-join-cause-snapshot.md)). The snapshot is rendered wherever the exception is printed and carries the original's causes; a property would be a second place to look, or a second surface to dispatch on.
````

### 6.9 Guide 4 §5 (G1)

Old:

````text
A fault in the entry surfaces at the join as `DomainFaultedException` carrying the original message and stack-trace *text* — exception objects themselves do not cross (spec §30.14).
````

New:

````text
A fault in the entry surfaces at the join as `DomainFaultedException` carrying the original message and stack-trace *text*, and in its `CauseChain` a snapshot of the original, type name first, so the log line names what failed — exception objects themselves do not cross (spec §30.14). The name is for reading: a supervisor that must tell a cancellation it requested from a fault reads its own source (§9), and a child whose callers act on its error kinds returns them as a sendable result.
````

The guide's pin moves to the landing revision.

### 6.10 Guide 2 §4 (G2)

Old:

````text
A domain join raises `DomainFaultedException` with the original's message and stack-trace text, or `DomainFailedException`, which implements `IFailure`, when the original's class did;
````

New:

````text
A domain join raises `DomainFaultedException` — or `DomainFailedException`, which implements `IFailure`, when the original's class did — with the original's message and stack-trace text and, in its `CauseChain`, the original's type name;
````

The guide's pin moves to the landing revision.

### 6.11 Code-generation reference §5.6 (C1)

Old:

````text
                                      //   class implements IFailure
````

New:

````text
                                      //   class implements IFailure; either way the joined exception's CauseChain[0]
                                      //   is the original's (TypeName, Message, StackTrace), for logs, not for dispatch
````

The reference's pin moves to the landing revision.

## 7. Migration, changelog and conformance

### 7.1 What programs and tests see

MEASURED (`git grep` at `bd12fe038`):

- No gtest asserts on a join exception's `CauseChain`, `ToString()` or uncaught report. The one exact-text pin on the class, `builtin_exception_module_path_5177`'s `domain_faulted_exception_is_system_threading`, renders a user-constructed `new DomainFaultedException()` with no cause, which this rule does not reach. `shared_sync_689_contention`'s `async_main_uncaught_reports_type_and_message_33_3_1609` uses `find` on an exception thrown in `Main`, not at a join.
- Of the files under `tests/` that contain `Domain.Spawn` or `Parallel.`, only three read `CauseChain`, `SupersededChain` or a `Caused by` text: `exception_unresolved_member_2423_test.cpp` (it reads `FaultStackTrace` on a join, and `CauseChain` only on a non-join exception), `builtin_registry_snapshot_test.cpp` (the registry) and the rule corpus's `manifest.tsv` (row text only). `async_fault_causechain_reclaim_2330_test.cpp` names the `bz_domain_spawn` site in a comment; its cells are not joins. So no gtest and no rule-corpus case flips.
- The doc-example corpus case `tests/conformance/cases/stdlib_0125.bzl` is stdlib §6.8's listing; L1 adds a comment line to it. It is listed for the #1380 merge-by-content regeneration, not regenerated by the landing.

### 7.2 Changelog sketch

> **The original's type name crosses a domain join (#8401; proposal 0217).**
> - *Decided.* The exception a domain join raises, `DomainFaultedException` or `DomainFailedException`, carries in its inherited `CauseChain` the snapshot the `(message, cause)` constructor would take of the original: its type name, message and stack-trace text, then its own `CauseChain` (spec §30.14 *Faults*, §31.4; stdlib §6.8). The same holds for the first body fault `Parallel.*` raises (spec §30.14; stdlib §6.12). The snapshot is taken in the spawned domain before the original is disposed, and only strings cross.
> - *Declined.* A dedicated property (`FaultTypeName`, proposal 0110's rider) or a `Type`-valued one; the name in `Message`; carrying the original's `SupersededChain` (`DECLINED.md`, four rows, indexed at acceptance).
> - *Why.* A child's fault reached its handler and the crash report with no type, and a fault with an empty message with nothing at all. The rules that print exceptions already render `CauseChain`.
>
> **Migration.**
>
> *Newly defined:* a join exception's `CauseChain` is non-empty (it was empty), so its `ToString()` rendering and the §33.3 uncaught report name the original. No compile-time verdict changes, and no catch arm takes a different exception.
>
> *Spec-ahead-of-impl:* the runtime builds the join exception with an empty chain; tracker to file (`spec-tracking`), naming `bz_domain_spawn`, sequenced after #8634 (whose type-name source it uses). Spec-rule tier, with no stdlib `⚠` marker: `CauseChain` is implemented on `DomainFaultedException`, and the content rule is spec §30.14's; it is recorded by the tracker and by the banked `fail` rows of the new cases. `Parallel` (#3557) and async entries (#1523) are unimplemented; their fault paths take the rule when built.
>
> *Rule corpus:* seven new cases (§7.3); no verdict changes; `baseline.tsv` untouched.
>
> *Doc-example corpus:* `stdlib_0125` (the §6.8 listing) changes by one comment line, listed on #4505.

### 7.3 Proposed rule-corpus cases

All `run`, all expected to fail on r42 until the tracker lands (MEASURED for the first four: r42 prints the empty-chain results shown). The type-name checks use `EndsWith`, because §31.4 does not pin the name's form (the split-out clarification, §10). Cases 1–3 guard each `CauseChain[0]` read with a `Count` test, so that on a compiler without the rule the banked failure is a stdout mismatch, not a crash; the expected outputs are unchanged.

1. **`s30_14_0022`** — the issue's repro. Rule: S1's new sentence. Program: §2.2's `BadIndex` entry; the `DomainFaultedException` arm prints `ex.CauseChain.Count` and `ex.CauseChain[0].TypeName.EndsWith("IndexOutOfRangeException")`. `stdout: 1` / `True`. r42: `0` / `no snapshot` (MEASURED, guarded form).
2. **`s30_14_0023`** — the original's own causes follow. Entry: `int.Parse("x")` inside a `try` whose `FormatException` arm throws `new InvalidOperationException("bad config", e)`. Arm prints `Count`, `CauseChain[0].Message`, `CauseChain[1].TypeName.EndsWith("FormatException")`. `stdout: 2` / `bad config` / `True`. r42: `0`, then the guard's `no snapshot`.
3. **`s30_14_0024`** — cancellation is snapshotted too. Entry throws `new OperationCanceledException("stop")`; the `DomainFaultedException` arm prints `ex.CauseChain[0].TypeName.EndsWith("OperationCanceledException")`. `stdout: True`. r42: the guard's `no snapshot`.
4. **`s30_14_0025`** — an `IFailure`-marked original. Entry throws `new FileNotFoundException("app.toml")` (with `import System.IO;`); a `var f: IFailure` arm prints `f.CauseChain[0].TypeName.EndsWith("FileNotFoundException")` and `f.CauseChain[0].Message`. `stdout: True` / `app.toml`. r42: rejected (`IFailure` unresolved, #8414).
5. **`s30_14_0026`** — nested spawns. An `async` entry `Outer(): Task<int>` spawns `Inner` (which throws `FileNotFoundException("app.toml")`) and returns `await` of that join. The outer join's arm prints `Count` and whether entries 0 and 1 end with `DomainFailedException` and `FileNotFoundException`. `stdout: 2` / `True` / `True`. r42: rejected at the outer spawn — async entries are unimplemented (#1523; banked `s30_14_0016 fail #1523`); §30.14's own `TickAsync` example is rejected the same way (MEASURED by the critic, `a1_tick.bzl`). Once #1523 lands, the case also needs `DomainFailedException` and `IFailure` (#8442, #8414).
6. **`l6_8_0006`** — the first entry agrees with the copied strings. Entry throws `new InvalidOperationException("boom")`; the arm prints `ex.CauseChain.Count = 1 and ex.CauseChain[0].StackTrace = ex.FaultStackTrace and ex.CauseChain[0].Message = ex.Message`. `stdout: True`. Rule: L2. r42: prints `False`.
7. **`l6_12_0007`** — `Parallel.Map`. `Parallel.Map(new Array<int>(4, 3), Program.Fail)` where `Fail` throws `new InvalidOperationException("bad item")`; the arm prints `ex.CauseChain[0].TypeName.EndsWith("InvalidOperationException")` and `ex.CauseChain[0].Message`. `stdout: True` / `bad item`. Rule: L3. r42: rejected (`Parallel` unresolved, #3557).

The full text of cases 1 and 6, in the corpus's form:

```bezel
//! case: s30_14_0022
//! doc: spec
//! section: 30.14
//! title: a join's exception carries the original's type name in its CauseChain
//! rule: "The join instead raises `DomainFaultedException` …, and whose
//!   `CauseChain` carries a snapshot of the original: … first the original's
//!   type name, message and stack-trace text, then the original's own
//!   `CauseChain`."
//! cites: 30.14, 31.4
//! expect: run
//! stdout: 1
//!   True
//! issue: 8401
module Conf;

import System.Threading;

public class Program
  public static method BadIndex(): int
    var xs := new Array<int>(2, 0);
    return xs[5];
  end BadIndex;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.BadIndex);
    try
      var n := await t;
    catch
      var ex: DomainFaultedException => do
        Console.WriteLine(ex.CauseChain.Count);
        if ex.CauseChain.Count > 0 then
          Console.WriteLine(ex.CauseChain[0].TypeName.EndsWith("IndexOutOfRangeException"));
        else
          Console.WriteLine("no snapshot");
        end if;
      end,
    end try;
  end Main;
end Program;
```

```bezel
//! case: l6_8_0006
//! doc: stdlib
//! section: 6.8
//! title: the first CauseChain entry of a join's exception holds the copied message and trace
//! rule: "`Message` and `FaultStackTrace` are copied strings, and the inherited
//!   `CauseChain` is a snapshot of the original: its first entry holds the
//!   original's `TypeName`, `Message` and `StackTrace`"
//! cites: 6.8, 30.14
//! expect: run
//! stdout: True
//! issue: 8401
module Conf;

import System.Threading;

public class Program
  public static method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Boom);
    try
      var n := await t;
    catch
      var ex: DomainFaultedException => Console.WriteLine(ex.CauseChain.Count = 1 and ex.CauseChain[0].StackTrace = ex.FaultStackTrace and ex.CauseChain[0].Message = ex.Message),
    end try;
  end Main;
end Program;
```

The uncaught report (§33.3) writes to standard error, which the rule corpus does not check. Its regression belongs in the implementing lane's gtest: the `q2_uncaught` program, asserting exit 70 and that the report contains `IndexOutOfRangeException`.

## 8. Probes

The probe programs are those of §2.2 (`q1`–`q4`) and §7.3 (`c1`–`c7`, and `c1g`, case 1 as guarded), compiled with r42 under `/var/tmp/bezel-scratch/v361/p-0217/`. They are not committed: every program is reproduced in this document in full or in the one-line form §7.3 gives. The `Superseded:` lines after an arm's fault are the r42 deviation noted in §2.2 (§31.2 does not make an arm-body throw a supersession; no tracker found, a candidate for triage). Results, MEASURED:

| Probe | §7.3 case | r42 result |
|---|---|---|
| `c1` | 1 (unguarded) | `0`, then `Unhandled exception: System.IndexOutOfRangeException` (the `CauseChain[0]` read) / `Superseded: System.Threading.DomainFaultedException`, exit 70 |
| `c1g` | 1 (guarded, as in §7.3) | `0` / `no snapshot`, exit 0 |
| `c2` | 2 (unguarded) | `0`, then the same fault / `Superseded: System.Threading.DomainFaultedException: bad config`, exit 70 |
| `c3` | 3 (unguarded) | the same fault / `Superseded: System.Threading.DomainFaultedException: stop`, exit 70 |
| `c4` | 6 | `False` |
| `c5` | 5 | compile error at the outer spawn (#1523) |
| `c6` | 7 | compile error: `Parallel` unresolved |
| `c7` | 4 | compile error: `IFailure` unresolved |

## 9. Implementation sketch and trackers (non-normative)

- Runtime, `bz_domain_spawn`'s fault branch (`src/runtime/bezel_runtime_concurrency.cpp`): before `bz_exc_end_handling`/`bz_dispose_class_obj` on the original, read its type name from a type-name source that runs no user code — the `bz_exc_type_name(obj)` entry #8634 proposes (or #3302's), not the virtual `ToString` slot; the tracker is sequenced after #8634, or fixes all three snapshot sites together — and its `CauseChain` at the header's offset 24, call `bz_exc_causechain_link(typeName, message, chain)`, and store the result at the join exception's `CauseChain` slot. The stack-trace field uses the same `""` stub as the existing snapshot until traces are formatted.
- Reclaim: the join exception's chain is reclaimed by the same path as any exception's `CauseChain` (#2330's reclaim). A loop of 100 faulting spawns under ASan is the gate, including for #8634's leak at this site.
- #8727 (open: a borrow of a catch binding's `CauseChain` stored in a field outlives the exception, an ASan heap-use-after-free) is not this proposal's defect, but every join exception would now carry a non-empty chain, so its exposure grows; the implementing lane should know.
- Async entries (#1523): when built, their fault path uses the same helper. The 20–40-line estimate covers synchronous entries only.
- `Parallel` (#3557): when built, its first-fault path uses the same helper.
- Trackers to file at landing (`spec-tracking`): one for the runtime change, citing this proposal and the rule cases of §7.3. #8401 closes with the landing.

## 10. Owner decisions (2026-11-26)

On 2026-11-26 the owner accepted this proposal with every recommended answer ("I accept all"). The decisions are recorded below; the questions follow as they were put, with their reasoning.

- **Q1 — B.** The original's snapshot goes into the join exception's `CauseChain` (§4).
- **Q2 — yes.** Proposal 0110's stage-1 rider drops `FaultTypeName` and keeps `ExitKind`; a note recording that is added to [0110 §2.6](0110-partial-failure-toolkit.md).
- **Q3 — (a).** The loss of the original's `SupersededChain` at a join is confirmed for now; the reopening bar is field evidence (§5's fourth row, now indexed).
- **Split out:** the form of `ExceptionInfo.TypeName` (the qualified-name clarification) is [#9427](https://github.com/s8lap/bezel-lang/issues/9427).

**The questions, as asked.**

**Q1 — Which option?** B (recommended), C1/C2 (a property), or A (decline, with §5's alternative row). B is recommended for §4.1's reasons: it prints where exceptions are printed, it carries the causes, and it adds no arm pattern; as a dispatch surface it is on a par with C1 (text a `when` guard could test, barred by documentation only).

**Q2 — Proposal 0110's `FaultTypeName`.** If B is accepted, should 0110's stage-1 rider drop `FaultTypeName` and keep `ExitKind` alone (recorded as a status note on 0110 at this proposal's graduation)? Recommendation: yes. B and that property are equally text a `when` guard could test, and B also prints and carries the causes. `ExitKind` is a separate question that #8669 now also bears on.

**Q3 — The original's `SupersededChain`.** B drops it. Rationale §11.8 rests later-wins on the buried failure surviving into `SupersededChain` and the §33.3 report, and v0.361 delivers that chain whole through task seams (§31.2 on the v0.361 branch: "The chain is per raise, not per instance"; an async body's own raise "is the raise being reported, and its chain is delivered whole"). The join is now the one seam that loses it: a child whose body fault is superseded by a failing cleanup reports only the cleanup's exception. This holds under A as under B. Options: (a) confirm the loss (recommended for now; the reopening bar is field evidence, §5's fourth row); (b) carry the original's `SupersededChain` into the join exception's `SupersededChain`, which needs a clause in §31.2/§31.4 beside S5 and an exemption from v0.361's new-raise clearing, so that later awaiters of a `Task<R>` join still see it; (c) append the entries to `CauseChain`, which is declined because §31.4 requires the two chains to stay distinguishable.

**Split out, not asked here: the form of `ExceptionInfo.TypeName`.** §31.4 says a snapshot copies "its type name" without fixing the form, while `ToString()` renders "the qualified type name" (the `Type.FullName` rendering, stdlib §2.13), and r42 produces the qualified name in both existing snapshots. Pinning the qualified name for every `TypeName` the runtime or the cause constructor writes is a one-sentence clarification covering all three snapshot sites (cause constructor, supersession, and the join this proposal adds). It goes as its own small clarification issue, not as part of this design change; it would also make #8634 plainly a bug. This proposal's rule cases use `EndsWith`, so they do not depend on it.

## 11. Classifications and checks

- **Change class:** design change (§2.4). Landing: ruling class, after acceptance.
- **Kernel (§28, §30.5):** untouched. No K rule, no register row, no ownership fact changes. The join mechanism is unchanged apart from the content of the exception it raises.
- **Sendability (§30.14):** untouched. Only strings cross, and no type's verdict changes.
- **Anchor check (MEASURED):** a script checked that each of the thirteen `Old` texts of §6 (S1–S5, L1–L3, R1–R2, G1–G2, C1) occurs exactly once in its document at `bd12fe038` (v0.360) and at `c85ce271c` (the v0.361 branch).
- **Absence check:** `git grep` for `FaultTypeName`, `FaultType` and `ExitKind` in `src/` and `tests/` finds nothing (MEASURED), so no implementation-ahead-of-spec delta arises from the declined surfaces.
- **Composition:** one train. It shares §30.14 *Faults* with #8669 (independent, §4.4) and `DomainFaultedException`'s listing with proposal 0110 (Q2). Its Q3 bears on v0.361's §31.2 per-raise rule, which lands in parallel.
- **Critique:** non-author critique pc-0217 (SOUND-WITH-FIXES) at `b58f441be`; findings F1–F14 applied in this revision, with F3 and F4 changing what the owner is asked (Q1's wording, Q3), and the qualified-`TypeName` question split out.
