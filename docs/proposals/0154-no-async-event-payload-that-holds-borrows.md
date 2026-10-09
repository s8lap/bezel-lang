# Proposal 0154 — No `AsyncEventSource<T>` over a value type that holds borrows

| | |
|---|---|
| **Status** | graduated-v0.332 — superseded — graduated v0.332 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option B, an instantiation-site rule at `AsyncEventSource<T>` rejecting a payload type that is a value type that holds borrows, `CancellationToken` excepted; critique round 1 applied, 5 blocking and 6 non-blocking findings resolved, shared with the #5708, #5693, #5694 and #5696 rulings; that round re-grounded the rule on spec §9.7's posture that a value type that holds borrows is stored past its bound at no position, which proposal 0153's flag leaves standing, and made its reopening bar 0153's deferred option E; landed with the v0.332 changelog entry, beside the composition check's entry for the rule in §9.7's list of positions) |
| **Tracking issue** | [#5711](https://github.com/s8lap/bezel-lang/issues/5711) (`spec-question`; the v0.331 borrow-flow matrix re-run's observation O1, `scratch/matrix/r2/M4.md`); one `spec-tracking` compiler tracker is filed at landing (§6.4) |
| **Target module(s)** | **stdlib**: §6.5 (the payload sentence, the `RaiseAsync` retention bullet, the open-question sentence, one listing comment). **spec**: §30.1 (the `RaiseAsync` carve-out sentence), Appendix C.6 (one bullet). **rationale**: §13 (one dated paragraph). **code-generation reference**: §5.6 (one index comment) and §7.6 (one *Looks right, is rejected* pair plus one sentence in the block's note). **`DECLINED.md`**: four `declined-at-proposal` rows and one annotation on the v0.274 `deferred` row |
| **Written against** | spec/stdlib **v0.331** at `2679585` (main). Section numbers are v0.331's. |
| **Origin** | The v0.331 borrow-flow matrix re-run (`scratch/matrix/r2/M4.md`, observation O1; cell R6/C8's recorded residual). Proposal 0150 declined, for `TaskCompletionSource<T>`, the trust-without-backstop posture that stdlib §6.5 still records for `RaiseAsync`. The restriction was deferred at v0.274 (`DECLINED.md`, row *Restricting `EventSource<T>`/`AsyncEventSource<T>` payloads*) with the bar "a ruling that derives the restriction from a rule that actually states it". This proposal states the rule at the type, as 0150 did; it needs a proposal because it removes a stated admission ("The payload type may be any type"; the recorded abandonment residual). |

*Reference convention: unprefixed §-references are language-spec references at `2679585` (v0.331); "stdlib §" references are to the standard library companion; K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares; none invents an API.*

---

## 0. Thesis

§30.1 rejects, for an `async` method, lambda or iterator, a parameter "whose type is a value type that **holds borrows**", on the ground that "an async method captures its parameters into a heap-allocated state-machine frame that outlives the synchronous call and lives until the task settles". The rule "keys on the **`async` modifier**", and §30.1 names its one unmarked twin: "There is exactly one such position: `AsyncEventSource<T>.RaiseAsync` (stdlib §6.5), whose `T` this section therefore does not restrict." Stdlib §6.5 states that position's retention — "`RaiseAsync` retains its payload until the returned task settles" — as a K4.3 obligation on the raiser, and then records its limit: "where the payload carries a borrow reachable through no tracked reference — a span (§4.14, §4.15) — there is **no backstop of any kind**, and an abandoned raise at such a payload is undefined behavior in unchecked mode."

```bezel
class Pub
  private owned readonly _ev: AsyncEventSource<ReadOnlySpan<byte>> := new();   // admitted at v0.331
  public property Ev: IAsyncEvent<ReadOnlySpan<byte>> => _ev;
  public method Pump()
    using var buf := new OwningBuffer<byte>(8);
    Task.FireAndForget(_ev.RaiseAsync(buf.AsReadOnlySpan()));   // the raise frame holds the span
  end Pump;                                                     // `buf` disposed; a handler awaited later reads it
end Pub;
```

Proposal 0150 (v0.331) declined exactly this posture for `TaskCompletionSource<T>`: "K4.3's third clause: a trust clause names its backstop, and a span's buffer window has none" (`DECLINED.md`, the option-B row). It left `RaiseAsync` as "the one remaining retaining member" (`scratch/matrix/r2/M4.md`). A frame that holds a value-carried borrow past the synchronous call is the shape every other rule rejects: §30.1 for `async` parameters and results, §30.5's holds-borrows row for a task's result slot, §9.7's instance-field, static-field and collection bullets for storage ("there is **no trust residual for record-carried borrows**"). The v0.274 deferral asked for "a ruling that derives the restriction from a rule that actually states it". This proposal states it, at the type, in §30.5's row's form: `AsyncEventSource<T>` admits no `T` that is a value type that holds borrows, `CancellationToken` excepted.

**Measured reach: zero sites flip** (§2).

The rule does not rest on a missing backstop, so it is unmoved by proposal 0153 (#5707, same train), which makes a span read flag-checked. Its ground is the regime's: at v0.331 §9.7 already rejected the instance-field store of a `CancellationToken`-shaped record "for **regime uniformity** — one rule for record-carried borrows, not a per-record judgment of access paths — rather than a missing backstop", and 0153 restates that posture for the whole family as "a value type that holds borrows is **stored past its bound at no position**" (§9.7's instance-field bullet, v0.332), re-grounding §30.1's frame rules, §30.5's `Task<T>` row and 0150's declines on it. The raise frame is such a position. 0153's deferred option E (retiring those storage bans together) is the one route that would reopen this rule, and the rule rides it.

## 1. Ground truth

### 1.1 The text today

- §30.1 (*Borrow-bearing parameters and results are forbidden*): keyed on the `async` modifier; `RaiseAsync` is "outside the rule's scope and states its own retention and obligation where it is declared".
- Stdlib §6.5: "The payload type may be any type. Three interactions are worth knowing, and none is a rule of this section". The third interaction is the retention bullet quoted above. The paragraph after the three closes: "Whether any of the three should become a restriction on `EventSource<T>`/`AsyncEventSource<T>` instantiation is an open question, not settled here: the restriction was declined at v0.274 and the reopening bar its record states stands."
- `DECLINED.md`, row *Restricting `EventSource<T>`/`AsyncEventSource<T>` payloads (borrow-bearing, or move-only)*, `deferred` (v0.274): the first draft "asserted both rejections and the composition gate refuted both: §30.1 binds `async` lambdas, so a plain lambda or method reference returning a completed task is unaffected — an absolute rule drawn from a conditional". Bar: "A ruling that derives the restriction from a rule that actually states it."
- `DECLINED.md`, row *Exempting `EventSource<T>.Raise`/`RaiseAsync` by name at a function-type payload*, `rejected` (v0.320): a per-member allowlist is refused; "raising data is the remedy". Untouched here.
- §30.5's holds-borrows row (v0.331): the form this proposal copies — a type-level rejection, "an instantiation-site rule of the type in §3.6 rule 2's sense", `CancellationToken` excepted on A10's footing.
- K4.3: "the rule must either reject statically or carry an **individually recorded trust clause**", and "no new admission may be made without recording its clause or its backstop".

### 1.2 What a handler can do with such a payload

The handler type is `(T) -> Task`. At a `T` that holds borrows, an `async` lambda or `async` method taking the payload is already rejected by §30.1 (stdlib §6.5's first interaction), and a non-`async` handler cannot pass the payload to any `async` callee (§30.1 again: the callee's parameter is rejected at its declaration), store it (§9.7), or hand it to any other position that retains it (§9.7's bullets, §30.5's holds-borrows row). A handler at such a payload therefore does its work synchronously and returns a completed task. The only retention the admission buys is the raise frame's own, between handlers — which is what the rule removes.

### 1.3 The compiler today (`src/` at `2679585`)

`RaiseAsync` is typed by name in `src/sema/ExprTypecheck_Calls.cpp:12991` (`RaiseAsync(payload: T): Task`); `AsyncEventSource<T>` is registered in `src/sema/TypeResolution_Names.cpp:294–306` with no instantiation check. No holds-borrows check reads the payload. The rule is **spec-ahead-of-impl** in full.

## 2. Measurement

Sweep of `tests/` (gtest, lit, both conformance corpora), `docs/` (spec, stdlib, rationale, code-generation reference, the seven guides) and `examples/` for `AsyncEventSource<X>` and `IAsyncEvent<X>` at every X:

| X | sites | holds borrows? |
|---|---|---|
| `int` | 13 + 4 | no |
| `Ev` (`class Ev public N: int := 0; end Ev;`) | 4 | a class: the settlement clause with the flag, unchanged |
| `TickEvent` (`record TickEvent(N: int)`) | 3 + 1 | no |
| `TArgs` (changelog v0.54 history only) | — | pinned text |
| `T`, `out T` (the stdlib's own declarations, transcribed) | 85 + 43 | generic; re-read per instantiation |

Every `RaiseAsync` call in `tests/` raises an `int`, an `Ev` or a `TickEvent`. Guide 04 and `examples/` have no `AsyncEventSource`. The rule corpus (`l6_5_0011`–`l6_5_0013`) instantiates at `int`.

- **Tests that flip:** none.
- **Doc examples that flip:** none.
- **Rule corpus:** no `rule:` header quotes the edited sentences (`manifest.tsv` grepped for "payload type may be any type", "RaiseAsync` retains", "no backstop of any kind": none).

## 3. Options

### 3.1 Option A: status quo
Keep the admission and the recorded residual ("no backstop of any kind … undefined behavior in unchecked mode"). It is the posture 0150 declined one section over. With 0153's flag-checked span read the span leg gains a backstop, but the admission would still be the one position in the language at which a value type that holds borrows is held past a bound its sources do not provably meet, against §9.7's posture (v0.332) that such a value is stored past its bound at no position. **Declined**; reopened only with 0153's deferred option E.

### 3.2 Option B: an instantiation-site rule at `AsyncEventSource<T>` (**recommended**)
`AsyncEventSource<T>` admits no `T` that is a value type that holds borrows (§3.6's holds-borrows property; a `T` declared `IReadOnlySpan<TElement>` read as the span it boxes), `CancellationToken` excepted on A10's footing. A compile error at the instantiation, wherever the type is formed, explicit, inferred or through a user generic's substitution — an instantiation-site rule in §3.6 rule 2's sense, the shape of §30.5's row and §14.4's class-only restriction. One sentence; it covers `RaiseAsync`, the handler slot and the subscribe-only face reached through the source.

### 3.3 Option C: a flow rule at the abandonment shapes only
Admit the type; reject passing the task `RaiseAsync` returns over such a payload to `Task.FireAndForget`, to a `using var` drop, or to any holder past the raising scope, and admit `await _ev.RaiseAsync(span)` in the scope that owns the sources. Sound where the span's sources are frame-resident, but it is a new mechanism (a per-call provenance class for one member's result), it leaves `AsyncEventSource<Pair>` declarable for a payload no handler can do asynchronous work with (§1.2), and the raising method itself must be `async` with the span's sources frame-resident (§30.1's across-suspension rule), which narrows the admitted shape to a corner. **Declined** for simplicity and consistency.

### 3.4 Option D: a recorded K4.3 trust clause with the flag as backstop
Record the retention as a trust clause with the flag as its backstop, on the strength of 0153's flag-checked span read. The clause would now have a backstop to name, as 0150's declined option B would; 0153 keeps that decline on §9.7's posture that a value type that holds borrows is stored past its bound at no position, and this option contradicts the same posture at a second member. **Declined.**

### 3.5 Option E: the same restriction at `IAsyncEvent<T>` and at `EventSource<T>`
`IAsyncEvent<T>` retains nothing (it declares `Subscribe` only) and a user implementation's own raise is checked by §30.1 or §9.7 where it retains; `EventSource<T>.Raise` is call-bounded. Restricting either rejects programs no rule needs rejected. The move-only leg of the v0.274 deferral is a different question (passing by value-move) and stays deferred. **Declined.**

## 4. The options side by side

| | Soundness | Simplicity | Consistency | Reach |
|---|---|---|---|---|
| A status quo | recorded UB (span) at v0.331; flag-only under 0153 | — | contradicts 0150 and §9.7's no-position posture | 0 |
| **B type-level** | **sound** | **one sentence** | **§30.5's row and §30.1, at the one unmarked position** | **0** |
| C abandonment flow rule | sound | a new provenance class | admits a type no handler can use asynchronously | 0 |
| D trust clause | flag-backed under 0153 | one clause | contradicts §9.7's posture, 0150's decline | 0 |
| E wider | sound, over-rejects | one sentence, two types | rejects non-retaining positions | 0 |

## 5. Decision (delegated ruling, 2026-10-08)

### 5.1 The rule
Option B. ▲ *Stdlib §6.5:* `AsyncEventSource<T>` admits no payload type that is a value type that holds borrows (language spec §3.6: a borrow-bearing record — a span among them — a borrow-bearing inline array, or a choice that holds borrows; a `T` declared `IReadOnlySpan<TElement>` is read as the span it boxes). `AsyncEventSource<Pair>`, `AsyncEventSource<ReadOnlySpan<byte>>` and `AsyncEventSource<[2]Item>` are compile errors at the instantiation, wherever the type is formed, explicit, inferred or through a user generic's substitution (an instantiation-site rule in §3.6 rule 2's sense). `CancellationToken` is excepted on §30.1's certified footing (axiom A10), its source obligation §30.8's. The ground is §30.1's for an `async` parameter (K2.1 + K6.1): the frame holds the payload past the synchronous call, and a value type that holds borrows is stored past its bound at no position (§9.7's instance-field bullet as 0153 restates it; a stipulation of regime uniformity, not a missing backstop). The remedy is a class payload — the buffer or collection object, or a class holding it with an offset and a length — raised under the settlement obligation with its flag, or borrow-free data; or the synchronous `EventSource<T>`, whose `Raise` is call-bounded.

### 5.2 Kernel classification
K2.1 + K6.1 (§30.1's derivation), under §9.7's posture that a value type that holds borrows is stored past its bound at no position (no K4.3 store residual for it). The A10 exception is unchanged.

### 5.3 Body-locality
Decided at the type's formation site and per instantiation. No callee body is read.

### 5.4 Implementation sketch
A type-formation check on `AsyncEventSource<T>` at type resolution and generic substitution (`src/sema/TypeResolution_Names.cpp:294–306` registers the type), sharing the holds-borrows predicate 0150's tracker ([#5699](https://github.com/s8lap/bezel-lang/issues/5699)) lifts out of `checkAsyncSignatureBorrowBearing`.

### 5.5 Tests that flip
None (§2).

### 5.6 `DECLINED.md`
Options A, C, D and E as `declined-at-proposal`; the v0.274 `deferred` row annotated **taken at v0.332** for the async source's holds-borrows leg (the move-only leg and the sync source stay deferred, the row kept as the record). The v0.320 `rejected` row on a per-member allowlist is untouched.

### 5.7 Relation to the v0.274 refutation
The v0.274 draft derived the rejection from §30.1's conditional rule; the gate refuted the derivation, not the verdict. This rule is stated at the type, as §30.5's row is, and the sync source — where "a plain lambda or method reference returning a completed task is unaffected" was the gate's point — is untouched.

## 6. Edits at graduation (each `old` unique at `2679585`; the exact JSON is `scratch/v0332/U3.json`, the `U3-5711-*` edits)

### 6.1 Normative
- Stdlib §6.5: the payload sentence gains the restriction's pointer; the `RaiseAsync` bullet states the retention, the settlement obligation with its class-borrow backstop, and the rule, replacing the "no backstop of any kind" residual; the open-question sentence records which leg was taken; the `AsyncEventSource<T>` listing gains one comment.
- Spec §30.1: the carve-out sentence now says stdlib §6.5 restricts the `T`. Appendix C.6: one bullet beside 0150's.
- Rationale §13: one dated paragraph after §13.2. Code-generation reference §5.6 (comment) and §7.6 (one pair, one sentence in the note).

### 6.2 Migration
*Newly rejected (spec-ahead-of-impl):* `AsyncEventSource<T>` at a value type that holds borrows, at every formation site. Measured reach: zero. *Newly admitted:* nothing. *Doc-only:* the pointer clauses.

### 6.3 Corpus
Cases to write after landing (stdlib §6.5; spec §30.1):
- Reject: `AsyncEventSource<Pair>` (a field), `AsyncEventSource<ReadOnlySpan<byte>>`, `AsyncEventSource<[2]Item>`, `AsyncEventSource<Ref>` for a choice that holds borrows, `AsyncEventSource<IReadOnlySpan<int>>`, and `class Fan<T>` holding an `AsyncEventSource<T>` named at `Fan<Pair>`.
- Accept: `AsyncEventSource<CancellationToken>`, `AsyncEventSource<Pt>` for a borrow-free record, `AsyncEventSource<Item>` for a class, and `EventSource<ReadOnlySpan<byte>>` with a synchronous handler.

### 6.4 Trackers to file at landing
`spec-tracking`: `AsyncEventSource<T>` at a value type that holds borrows is unchecked (stdlib §6.5, v0.332); no instantiation check exists at `TypeResolution_Names.cpp:294–306`. Share the predicate with [#5699](https://github.com/s8lap/bezel-lang/issues/5699).

## 7. Self-critique

- *Is the type-level reach gratuitous?* It rejects `await _ev.RaiseAsync(span)` awaited in the raising scope, which is sound. The measured reach is zero, no handler can do asynchronous work with such a payload (§1.2), and a class payload expresses every such program. One sentence, the price 0150 paid for `FromException<Pair>`.
- *A record wrapping a token* (`record Ctx(T: CancellationToken)`) is rejected, as it is at `Task<T>` (0150). Sound; an over-rejection by design.
- *Does #5707 (proposal 0153) change the verdict?* No (§0). It changes only whether the status-quo residual reads "no backstop" or "flag"; the declines of options A and D are stated on §9.7's posture, which 0153 keeps, and retiring the rule rides 0153's deferred option E with the rest of the family.
- *The sync `EventSource<T>`:* untouched. Its `Raise` retains nothing past the call, and the v0.274 gate's point about completed-task handlers stands there.
