# Proposal 0149 — Frame locals and span-carried borrows in the consume-site freeze: a local holding an iterator frame is a live borrow of its carried sources, and a span-carried borrow freezes the site-announced kills

| | |
|---|---|
| **Status** | graduated-v0.331 — superseded — graduated v0.331 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues": option A, two derivations — frame locals traced by K5.5 and span-carried borrows freezing the site-announced kills, K5.9; critique round 1 applied, 3 blocking and 6 non-blocking findings resolved, shared with the #5680 ruling; that round counted a bound frame's disposal as a use, named every K5.5 holder in K5.9, and recorded option F, flag-checked span reads, as the alternative that would make K5.9 moot; landed with the v0.331 changelog entry, beside the composition check's Appendix C.3 row and code-generation holder list) |
| **Tracking issue** | [#5679](https://github.com/s8lap/bezel-lang/issues/5679) (borrow-flow matrix root cause B6; side finding S1); the compiler tracker is filed at the v0.331 landing |
| **Target module(s)** | **spec** — §28.3 *The ownership kernel* (K5's conjunct (ii) and clauses 3 and 5, new clause 9; K6.4; the consume table's reading note), §28.3 *The consume-site freeze* (two new paragraphs, one count), §29.4 (one sentence); **rationale** — §5.1's v0.180 record (a second recorded exception); **code-generation reference** — §7.2 (two *Looks right, is rejected* pairs); no stdlib change |
| **Written against** | spec **v0.330** at `1ec0886` (main). Line numbers below are at that commit. |
| **Origin** | The v0.330 borrow-flow matrix audit (`scratch/matrix/`, cells R6/C10 and R6/C13, side finding S1, root cause B6 in `X2.md`). |

*Reference convention: unprefixed §-references are language-spec references at `1ec0886` (v0.330); K tags are §28.3's kernel ids; rationale, stdlib, code-generation-reference and corpus references are marked. Quotations are verbatim. ▲ marks proposed text. The programs below use only members the stdlib companion declares; none invents an API.*

---

## 0. Thesis

A span's borrow has no runtime backstop: its reads go to the buffer window with no per-access disposed-flag check (§9.7: "a span's buffer window is the canonical case"; §30.1: "a raw span window has no disposal-tracked object through which checked mode could catch the dangle"). The kernel nevertheless treats span-carried borrows as flag-backstopped at two places where the freeze cannot see them:

1. **Iterator frames.** A synchronous iterator may take a span parameter (v0.317 declined the ban, `DECLINED.md` row *Banning borrow-bearing record … parameters on synchronous iterator methods*), and the returned frame carries the span's source (§29.4; *Deferred-activation callees*, argument contribution 3). But K6.4 says "Frame-held borrows live in the frame, not in any local borrow binding, so K5 cannot see them; their outlives-obligation is the K4.3-recorded settlement clause ([§30.1])" — a clause that exists for tasks only, and whose named backstop is the flag. So `var it := Each(xs.AsSpan()); sink.Take(move xs); for var x in it do … end;` has no static check, no recorded clause and no backstop.
2. **Site-announced kills.** K5's conjunct (ii) says "(ii) fails in exactly two recorded shapes, and in no other", and §28.3 says "the site-announced kills stay unfrozen under their flag backstop". So `var s := xs.AsSpan(); xs.Dispose(); Console.WriteLine(s[0]);` is admitted and reads a freed array — while the same section says `var s := buf.AsSpan();` "freezes `buf` against `FrozenArray<T>.From(move buf)` — and every other consume". The criterion's own words decide it: (ii) requires that "a later stale use still faults", and a span read never faults.

This proposal makes the freeze see both. **(1)** K5.5's liveness trace gains one holder kind: a live local holding an iterator or enumerator frame or a lazy query counts as a live local borrow of every source the frame carries — exactly as a live closure value already does. **(2)** K5 gains a ninth clause, a derivation from (ii) beside K5.7 and K5.8: a site-announced kill of a binding is frozen while a span over its value survives it, in a live span local or carried by a live frame local. Both are derivations, not axioms; both are body-local; the measured reach in `tests/`, the docs and the rule corpus is zero sites. The alternatives — a span-only trace, a task-local trace, a K4.3 clause with no backstop, and the already-declined parameter ban — are declined with reasons in §4.

---

## 1. Ground truth

### 1.1 The rule today

- **K2 (line 6360):** a source is "a **frame** (K6): a task, iterator/enumerator frame, or lazy query value, itself an ordinarily-classified value". **K2.2:** "frame carriage combine[s] borrows by taking the union of their source sets". **K2.3:** "A borrow may not reach a holder — a binding of wider scope, a field, a collection, a frame, a return — whose lifetime the checker cannot bound by the borrow's source set."
- **K5 (lines 6417–6431):** the freeze is on "a live local borrow of its current value"; (ii) "fails in exactly two recorded shapes, and in no other"; clause 3: the site-announced kills are unfrozen "wherever the surviving borrows are backstopped by the kill's own flag, which is every case but the two recorded in clauses 7 and 8"; clause 5: "liveness is the backward dataflow over one body, borrows are traced through `using var` pins, live closures, and locals of borrow-bearing record or inline-array type or of choice types that hold borrows … and this is why a closure may not consume its captured owned bindings (K2 + decidability)".
- **K6 (line 6434):** a frame "is a **value** with an ordinary K1 classification that additionally **carries a source set** (K2): the constructing call's receiver, its plain reference-typed arguments, and the capture-set contributions of its plain function-typed arguments, transitively." **K6.1:** the bound is "**completion or disposal** for an iterator frame". **K6.4 (line 6438):** "Frame-held borrows live in the frame, not in any local borrow binding, so K5 cannot see them; their outlives-obligation is the K4.3-recorded settlement clause ([§30.1]), discharged trivially by awaiting before any source's scope exits and structurally by `TaskScope`. The abandonment idioms carry the residual explicitly."
- **K4.3 (line 6414):** "no new admission may be made without recording its clause or its backstop." The recorded clauses are "field-held, collection-held, comparer-held and frame-held borrows versus domain transfer".
- **§28.3 *The consume-site freeze* (lines 6763–6767):** a live local of a holds-borrows value type "counts as a live local borrow of every held borrow's source … `var s := buf.AsSpan();` freezes `buf` against `FrozenArray<T>.From(move buf)` — and every other consume — for as long as `s` is live"; then "**Unborrowed reassignment stays legal** — only the silent kill under a live borrow is rejected, and the site-announced kills stay unfrozen under their flag backstop ([K5.3]; [K5.7]/[K5.8]'s two recorded shapes excepted, each stated at its home)."
- **§28.3 *Deferred-activation callees* (line 6816):** "The returned **deferred-activation frame carries its arguments' borrows** … the ordinary rule that a borrow may not outlive its source applies to it." **Argument contributions item 3 (line 6616):** "an argument of a value type that holds borrows … contributes the sources of the borrows it carries".
- **§29.4 (line 7914):** "The returned state machine holds its factory's plain arguments' borrows — the sources each contributes … until it completes or is disposed".
- **§30.1 (line 8102):** the `async` kinds "may not declare a parameter whose type is a value type that **holds borrows**" — a synchronous iterator may. **§30.1 (line 8110):** the settlement clause is "a [K4.3]-recorded trust clause … with [K4.1]'s backstop as the named safety net".
- **§9.7 (line 1455):** a record-carried borrow "may be consumed through access paths with **no per-access disposed-flag check** (a span's buffer window is the canonical case)".
- **The contrary reading (critic round 1).** Stdlib §4.14 and §4.15 declare the span records' indexers as user-level bodies that read `Buffer[Start + i]` / `Source[Start + i]` ("purely user-level Bezel — no compiler intrinsic"), rationale §5.15 says "`Span<T>` indexes through the buffer's indexer", and §28.2's checked mode faults on "a property or indexer accessor invocation" on a disposed instance. Read together they would make a span read flag-checked in checked mode. The controlling statements — §9.7's, §30.1's "a raw span window has no disposal-tracked object through which checked mode could catch the dangle", the v0.180 rationale record behind the instance-field ban — and the compiler (spans are builtins; `Ownership_Checks.cpp:10821–10830` records an ASan SEGV in `bz_array_get` reading a disposed span source, not a `DisposedAccessException`) say the opposite. This proposal rests on the controlling statements; the reconciliation is a separate question (option F, §3; `T2.md`, new question 4).
- **§29.6 / §29.7:** `yield return` inside a `try` with only a `finally` is allowed, and disposing a partially-consumed iterator runs the pending `finally` blocks "exactly as if the body had exited at the suspension point" — so a frame's **disposal** can read its carried sources.
- **Rationale §5.1 (line 312), the v0.180 record:** "Alternatives declined: (a) freezing explicit `Dispose()` too — converts this section's recorded trade-off into a de-facto borrow checker for no soundness gain (the backstop is real); one deliberate exception is recorded: sources captured under §28.14's storage-bounded admission *are* frozen against disposal as well".
- **Changelog v0.317 (#5485) / `DECLINED.md` row *Banning borrow-bearing record, inline-array and holds-borrows choice parameters on synchronous iterator methods*:** `rejected`; "a ban would reject the sound case of a frame consumed within its source's scope. Reopening bar: —(none stated)."
- **Changelog (#4413), the borrow-pin ruling:** declined "making the pin a third recorded shape — it would reopen a closed enumeration for a case that has an admissible backstop (the kill flags the very object the pinned borrow references)". The reason is the backstop's existence; it does not reach a span.

### 1.2 The compiler today (`src/ownership/Ownership_Checks.cpp` at `1ec0886`; source-read, not probed — `build/bezelc` is absent in this lane)

- **`checkConsumeFreeze`, 13079–13160:** the holders it walks are (a) tracked locals whose `st.source->sym` is the consumed binding — alias locals such as `var b := xs;` — made live by a reachable later use or by a `using var` pin, traced "through the closure value the holder names"; and (b) `recordBorrowLocalSource_`, the span/record locals (§28.3 v0.186, #1726), live by `borrowHolderUsedAfterInWindow`. A local holding a minted frame is an owned value with no `source`, so a frame local is **not** a holder: K6.4's "K5 cannot see them" is what the compiler does.
- **`markDisposedByName`, 12145–12149:** an explicit `Dispose()` runs `checkScopedFreeze` (the §28.14 freeze) only, never `checkConsumeFreeze` — the site-announced kill is unfrozen for every holder, span locals included. S1's program is accepted today.
- **`recordBorrowLocalSource_`, 12486–12510:** records a span/record local's carried source so the freeze sees it "exactly as a live span over that source does"; the frame's argument scan is the v0.317 tracked gap ("covers reference-typed plain arguments only", changelog v0.317).

### 1.3 The matrix cells

- **R6/C10** (`missing`): the counterexample in §0 item 1. X2: "moving or disposing a span's source under a live iterator frame has no static check, no recorded clause and no backstop."
- **R6/C13** (`ambiguous`, B4 and B6): a span captured by a storage-bounded callback, then `xs.Dispose()`. Its B4 half (whether the value-typed capture is a tested source) is lane T1's (#5677); its B6 half is the kill under a live span-carried borrow.
- **S1** (outside the columns): `var s := xs.AsSpan(); xs.Dispose(); Console.WriteLine(s[0]);`.
- **Neighbours walked in §7:** R1/C10, R2/C10, R3/C10, R7/C10, R8/C10, R9/C10 (the frame column); R6/C3, R6/C6, R6/C7, R6/C9, R6/C11 (the span row).

---

## 2. Measurement

**Method.** `scratch/v0331/` scan (recorded in `T2.md`): over every `.cpp`/`.bzl` under `tests/` and every `docs/*.md`, (A) a local bound to an iterator-method call (a method in the same text declared `: IEnumerable<…>`/`IEnumerator<…>`) or to a lazy-operator chain (`Where`, `Select`, `Take`, `Skip`, `Zip`, `Reverse`, `OrderBy`, …), followed within 2,500 characters by `move X` or `X :=` of one of its argument/receiver identifiers, followed by a use of the local; (S) a local bound to `X.AsSpan()`/`X.AsReadOnlySpan()`, followed by `X.Dispose()`/`X?.Dispose()`/`X!.Dispose()`/`_ := X;`, followed by a use of the span local.

**Result.** (A) five raw hits, all false positives (`Sink.Take(move y)` matched the `Take` operator; `joinInto_core_state_matrix_4312_4313_test.cpp`, `nullcond_checkread_moved_from_disposed_3984_test.cpp` — neither binds a frame). **Zero** sites. (S) five raw hits in `consume_site_freeze_span_1726_test.cpp` and `consume_site_freeze_span_reassign_1741_test.cpp`, every one a `Dispose()` *after* the span's last use (`_ := n;` precedes it) — the accept-after-last-use case, which stays accepted. **Zero** sites. Rule corpus: `l4_19_0004` (span local, `FrozenArray<T>.From(move buf)` rejected) is a K5.1 transfer, unchanged; no §28.3, §29.4 or stdlib §4.14/§4.15/§4.19 case disposes a source under a live span or moves one under a live frame local. Stdlib: no stdlib member returns an iterator frame over a span argument (stdlib §4.14 iterates a span by an indexed loop, line 2797, and keeps `OwningBuffer<T>` out of `IEnumerable`, line 3532), so the frame half reaches user iterators only; the lazy query operators over `Array<T>` reach the class-sourced half of rule (1) — zero measured sites.

**Over-rejection accepted.** Rule (1) rejects `var q := xs.Where(f); pool.Add(move xs); for var x in q do … end;`, which is sound at runtime (the array lives on in `pool`). This is the freeze's standing posture since v0.180 for every traced holder (`var b := item.Handle(); pool.Add(move item); b.Configure();` is the code-generation reference's own example), with the same mechanical rewrite: transfer first, then re-derive the frame from the new owner (`pool.Add(move xs); var q := pool[pool.Count - 1].Where(f);`), or consume the frame before the transfer. Zero sites pay it today.

---

## 3. Options

- **S — status quo.** K6.4 as written; S1 admitted. Unsound for spans with no backstop of any kind (§0). Rejected by K4.3's own rule: an admission with neither a comparison nor a backstop nor a recorded clause is "nothing".
- **A — trace frame locals; freeze the site-announced kills under a live span (recommended; the issue's recommendation).** K5.5 traces a live local holding an iterator or enumerator frame or a lazy query as a live borrow of every carried source, as it traces a live closure. K5.9 (new) freezes a site-announced kill of a binding while a span over its value survives in a live span local or a live frame local. Uniform with the holders K5.5 already names (records, inline arrays, choices, closures: all traced for *every* carried source, flagged or not); one sentence per rule; the transfer rewrite applies unchanged.
- **B — a span-only frame trace.** As A, but the frame local counts only the sources contributed by its holds-borrows value-typed arguments (contribution item 3). Narrowest in reach — but K5.5 would then hold one holder kind by a different key (flag presence) than every other, the K6 set would need splitting into "flagged" and "span-carried" halves at every carriage, and the measured difference is zero sites. Declined (§4).
- **C — ban holds-borrows parameters on synchronous iterators.** Declined at v0.317 (`DECLINED.md`, "a ban would reject the sound case of a frame consumed within its source's scope"); the present evidence does not contradict that reason — it shows the *other* case (source consumed under a live frame) is unchecked, which A checks while keeping the sound case. Stands declined.
- **D — trace task locals too.** A plus `var t := F(obj); …` as a live borrow of `obj`. A task's carried borrows are flag-backstopped by construction (§30.1 bans holds-borrows parameters and caller-sourced holds-borrows locals across a suspension), and they are held under a **recorded** K4.3 clause (settlement; domain transfer) whose residual shapes — `using var t := call();` dropped, `Task.FireAndForget(call())`, `Domain.Spawn(entry, move obj)` before settlement — the report mode names and the spec deliberately admits under trust. A freeze would silently turn that recorded admission into a rejection; that is §30.1's design change, not this issue's. Declined with a reopening bar (§4).
- **F — make span reads flag-checked.** Read stdlib §4.14/§4.15's declared indexer bodies (`Buffer[Start + i]`) with §28.2: a span read in checked mode invokes the buffer's indexer and faults on a disposed buffer. Then K5(ii) holds for spans, K5.9 is unnecessary and the field-slot route (`T2.md`, new question 1) is backstopped too. Not taken here: it contradicts §9.7's and §30.1's normative statements, the v0.180 record on which the instance-field ban rests, and the implementation, and choosing between the two readings is a design question of its own (a per-access check on every span read, or the stdlib bodies restated as models). Filed as a question; if it is decided for the check, K5.9 and its rationale exception are withdrawn with the decision (the reopening bar of the option-E and status-quo rows). Rule 1 does not depend on it.
- **E — a K4.3 trust clause for iterator frames.** Record "frame-held span borrows versus source kill" as a clause. K4.3 demands "a recorded residual obligation with a named backstop" and the only backstop the kernel has is the flag, which a span read never consults. There is nothing to name. Rejected.

---

## 4. Evaluation and decision

| | S | A | B | C | D | E |
|---|---|---|---|---|---|---|
| Soundness (spans) | unsound | sound | sound | sound | sound | unsound (no backstop) |
| Simplicity | — | one holder kind added to a list; one derived clause | the holder list keyed on two properties | one ban (declined) | A + a §30.1 redesign | one clause with no backstop |
| Consistency | contradicts §28.3's "every other consume" | matches closures and records (K5.5), K5.7/K5.8's derivation shape | breaks K5.5's uniformity | contradicts row 170 | contradicts §30.1's recorded trust | contradicts K4.3 |
| Newly rejected (measured) | 0 | 0 sites (tests, docs, corpus) | 0 | iterator-over-span params (3 corpus/test shapes from v0.317) | abandonment/transfer idioms under a live task local (not measured; recorded admissions) | 0 |

**Decision: A.** Two rules, both derivations:

- **Rule 1 (K5.5 — frame locals are traced).** A live local — a `for` desugaring's hidden `__source` (§29.2) included — holding an iterator or enumerator frame or a lazy query counts as a live local borrow of every source the frame carries (K6; transitively, through frames and closures it carries), as a live closure value counts for its captures. The frame's own **disposal** — explicit, at a `using var`'s scope end, or K3.14's auto-drop — is a use: a partially-consumed frame's disposal runs its pending `finally` blocks (§29.6, §29.7), which may read the carried sources, so without it `var e := Each(xs.AsSpan()); _ := e.MoveNext(); xs.Dispose();` (an iterator declared `: IEnumerator<int>` whose `finally` reads the span) would read the freed array at `e`'s scope-end disposal with no later explicit use to freeze the kill. Consequences: K5.1/K5.2 kills of a carried source (a `move` into any owned destination, a transfer, a reassignment, an owning `yield return`) are frozen while the frame local is live; the site-announced kills of a class-sourced carried borrow stay unfrozen (the frame's reads are member accesses that hit the flag); a span-carried one is Rule 2's. A task local is not traced (K6.4 keeps §30.1's recorded clause for it).
- **Rule 2 (K5.9 — span-carried borrows freeze the site-announced kills).** K5's conjunct (ii) fails where a surviving borrow is **span-carried** — held by a buffer- or array-backed span (stdlib §4.14, §4.15) or an `IReadOnlySpan<TElement>` value over one — because the kill's flag never reaches a span read (§9.7). A site-announced kill of a binding is therefore frozen while a span over its value survives in any holder K5.5 traces — a span local, a record, inline-array or choice local that carries one, a live closure value that captures one, or a live frame local (Rule 1, its disposal included). (ii) fails in exactly three recorded shapes. A record whose carried borrows are object references keeps the unfrozen posture: every read through it is a member access that faults.

**Why the enumeration may grow.** #4413 declined a third shape for the borrow-pin because that case "has an admissible backstop". Rule 2's case has none, and (ii)'s own wording ("so a later stale use still faults") is what fails. Rationale §5.1's v0.180 decline of freezing `Dispose()` rests on "the backstop is real"; for a span window it is not, which is the new evidence process §2 requires to reopen a recorded decline — narrowly, for spans only. The rationale record gains a second recorded exception beside §28.14's.

**Kernel classification (process §9).** Both rules are **derivations**: Rule 1 from K2.1 + K2.2 + K2.3 + K5.5 (a frame is a value carrying a source set; a local holding it is a holder the body can trace); Rule 2 from K5's conjunct (ii) + §9.7's no-per-access-check fact, the same shape as K5.7 and K5.8. No axiom changes. **Body-locality (proposal 0105 §1): local** — both are the backward dataflow over one body; the frame's carried set is already computed at the call (K2.4, K6.2).

**Declined alternatives (rows for `DECLINED.md`, in `T2.json`):**

| Topic | Verdict | Reason | Reopening bar |
|---|---|---|---|
| A K4.3 trust clause for an iterator frame's span-carried sources (option E) | `rejected` | a clause needs a named backstop; a span window has none | a per-access check on span reads (a disposal-tracked window object), which stdlib §4.14's design declines |
| A span-only frame-local trace (option B) | `declined-at-proposal` | keys K5.5's holder list on flag presence for one holder kind; zero measured sites separate it from A | —(none stated) |
| Tracing task locals (option D) | `declined-at-proposal` | a task's carried borrows are flag-backstopped by §30.1's bans and held under the recorded settlement clause, whose abandonment and domain-transfer residuals a freeze would turn into rejections | a route by which a span-carried source enters a task frame landing without its own static rule (the B4 capture route, #5677's neighbour) |
| Leaving frame locals untraced and the site-announced kills unfrozen under a live span (option S, the status quo) | `declined-at-proposal` | a span-carried borrow has no admissible backstop; the admission has no comparison, clause or backstop (K4.3) | span reads made flag-checked in checked mode (option F) |
| Banning holds-borrows parameters on synchronous iterators (option C) | stands `rejected` (existing row, v0.317) | — | — |

---

## 5. The normative edits

Exact `old → new` pairs are in `scratch/v0331/T2.json` (ids `B6-*`); summarized:

1. **K5 conjunct (ii):** "exactly two recorded shapes" → "exactly three recorded shapes", adding "or the surviving borrow is span-carried, so the flag never reaches its reads (clause 9)".
2. **K5 clause 3:** "every case but the two recorded in clauses 7 and 8" → "… three recorded in clauses 7, 8 and 9".
3. **K5 clause 5:** the traced holders gain "and locals — a `for` desugaring's hidden `__source` included — holding an iterator or enumerator frame or a lazy query (K6), each a live local borrow of every source the frame carries, transitively, as a live closure value is of its captures' sources".
4. **K5 clause 9 (new):** the span-carried derivation from (ii), stated with its reason and pointing at §28.3 *The consume-site freeze*.
5. **K6.4:** frames bound to a local are traced; a task's held borrows keep the recorded settlement clause, with the reason (its carried borrows are flag-backstopped by §30.1's construction; an iterator frame may carry a span).
6. **Consume table reading note:** "the two recorded shapes" → "the three recorded shapes", naming K5.9.
7. **§28.3 *The consume-site freeze*:** two new paragraphs (*Frame locals*; *Span-carried borrows and the site-announced kills*) after the liveness paragraph; "K5.7/K5.8's two recorded shapes" → "K5.7/K5.8/K5.9's three recorded shapes".
8. **§29.4:** one sentence: a local holding the returned frame is a live local borrow of its carried sources for the freeze.
9. **Rationale §5.1:** the second recorded exception to the v0.180 decline of freezing `Dispose()`.
10. **Code-generation reference §7.2:** two *Looks right, is rejected* pairs (span local then `Dispose()`; frame local then `move`).

Lane T1 (#5677) edits §28.3's provenance clause 1 and §28.14's bound-1 wording; none of the anchors above touches either passage.

---

## 6. Changelog, migration, trackers

Draft bullets and the Migration note are in `T2.md`. **Newly rejected (spec-ahead-of-impl):** (i) a `move`, transfer or reassignment of a source carried by a live iterator-frame, enumerator-frame or lazy-query local (`var it := Each(xs.AsSpan()); sink.Take(move xs); for var x in it …`; `var q := xs.Where(f); pool.Add(move xs); … q …`; a `move` of the source inside a `for` body over a frame carrying it); (ii) a site-announced kill of a binding under a live span over its value (`var s := xs.AsSpan(); xs.Dispose(); s[0]`; `_ := xs;` likewise; a frame-carried span likewise). Measured reach: zero sites. **Trackers:** one `spec-tracking` issue for both rules, naming `checkConsumeFreeze`'s holder loop (frame locals carry no `source`) and `markDisposedByName` (runs `checkScopedFreeze` only).

---

## 7. The matrix cells after the change

| Cell | Before | After |
|---|---|---|
| R6/C10 span → iterator frame | missing | **decided — admitted-bounded**: the frame carries the span's source (§29.4); a local holding it is a live borrow of that source (K5.5): moves and reassignment frozen, and the site-announced kills frozen too (K5.9). A frame passed on within one full expression is bounded by K8; a returned frame re-establishes at the caller (K6.2). |
| R6/C13 span captured under a storage-bounded callback | ambiguous (B4 + B6) | the B6 half closes: with T1's clause-1 ruling the capture contributes `xs`, so K5.7 freezes `xs.Dispose()`; independently, where the closure is a live local, K5.5 + K5.9 freeze it. Status after both lanes: **decided — rejected**. Dependency on T1 recorded. |
| S1 | conflict | **decided — rejected** at the `Dispose()` (K5.9). |
| R2/C10 class ref → iterator frame | decided (flag-only for moves) | **decided, strengthened**: moves/reassignment of a carried class source under a live frame local are now frozen; `Dispose()` stays unfrozen (flag). The K4.3 "frame-held borrows versus domain transfer" clause still covers a frame not held in a traced local (a purely local collection of frames, a frame handed on). |
| R3/C10, R8/C10 record / bare-T → iterator frame | ambiguous / decided (B4) | unchanged verdicts; Rule 1 now freezes the record's sources under a live frame local (flag-backstopped reads keep `Dispose()` unfrozen). The capture route stays B4's. |
| R1/C10, R7/C10, R9/C10 | decided | unchanged (an owned argument is moved into the frame; a callback's captures are carried sources, now traced through the frame local exactly as through the closure; runtime-managed values carry nothing). |
| R6/C3 span → collection | decided (X1) | unchanged verdict for a parameter-held collection (rejected). **Not opened, but noted:** a method-created local collection of spans is not a K5.5 holder, so a kill of the element's source under it is unchecked — pre-existing, filed as a new question (`T2.md`). |
| R6/C6, R6/C7, R6/C9, R6/C11 | B4 / decided | untouched. R6/C9's `async lambda` route would carry a span-sourced borrow into a task frame, which Rule 1 does not trace (option D's bar). |

---

## 8. Rule-corpus cases to write (after landing)

K5.5 frame locals: `s28_3_NNNN` reject — iterator over `xs.AsSpan()`, `sink.Take(move xs)` under the live local; reject — `var q := xs.Where(f); pool.Add(move xs); … q …`; reject — `move xs` inside a `for` body over `Each(xs)`; accept — the frame enumerated through an unbound call (`for var x in Each(xs) do … end;`) before the transfer; accept — a bound frame explicitly disposed (`it.Dispose();`) before the move; reject — a bound frame with no explicit later use, moved under (its scope-end disposal is a use); reject — `var e := Each(xs.AsSpan()); _ := e.MoveNext(); xs.Dispose();` with an iterator whose `finally` reads the span (the disposal-is-a-use case for K5.9). K5.9: reject — `var s := xs.AsSpan(); xs.Dispose(); s[0]`; reject — the discard `_ := xs;` under a live span; reject — `Dispose()` of a source carried by a live iterator-frame local over a span; accept — `Dispose()` after the span's last use; accept — a record carrying a class borrow (`Pair(Item: Item)`), `it.Dispose()` under the live record local (unfrozen, flag). §29.4: one sentence case (header refresh of none needed; no existing case quotes the edited sentences — `l4_19_0004` quotes "counts as a live local borrow of every held borrow's source", which stays verbatim).

---

## 9. Self-critique (adversarial pass)

1. *Does Rule 1 contradict K6.4's reason?* K6.4's reason ("not in any local borrow binding") is true of the frame's borrows but not of the *frame*, which is a value in a local; K5.5 already traces a closure value's captures through the local holding the closure, and a frame is the same shape (K6: "a value … that additionally carries a source set"). The rewrite keeps K6.4's claim for tasks, with the reason stated.
2. *Why not tasks?* Not because the trace would be unsound, but because §30.1 records trust there on purpose, with report-mode visibility, and bans the flagless case at the door. The one flagless route into a task frame (a captured span, R6/C9) is B4's; option D's reopening bar names it, and `T2.md` records the seam for the composition gate.
3. *Does Rule 2 reopen #4413?* No — #4413's reason was the backstop's presence. It does reopen rationale §5.1(a)'s "the backstop is real" for the one case where it is not; the rationale record says so.
4. *Is "span-carried" closed?* Buffer- and array-backed spans (stdlib §4.14 `Span<T>`/`ReadOnlySpan<T>`, §4.15 `ArraySpan<T>`/`ReadOnlyArraySpan<T>`) and `IReadOnlySpan<TElement>` values over them (§9.7: read as holding the span's borrows). `ReadOnlyStringSpan` carries no borrow. A record *containing* a span (`record Pkt(Data: ReadOnlySpan<byte>)`) carries a span-carried borrow transitively and is covered by "held by … a span" read through §9.7's recursion — stated as "span-carried" in K5.9 so the transitive case is in.
5. *The frame in a purely local collection, and the frame passed to a consumer that stores it in a local collection:* K5.5 does not trace collections; the K4.3 frame clause still names the residual for class sources, and for spans this is the same pre-existing collection gap as R6/C3's local half — filed, not fixed here (it needs its own measurement and touches §28.11).
6. *A `for` over `Each(xs.AsSpan())` with `xs.Dispose()` in the body:* frozen by Rule 2 through the hidden `__source` (Rule 1 names it explicitly). With a class source, `xs.Dispose()` in the body stays unfrozen under the flag — unchanged from today.
7. *Field-sourced spans* (`var s := _buf.AsSpan(); _buf := new …; s[0]`): K5.6 — slots are not frozen, and the span has no flag. Pre-existing, outside the matrix's columns, filed as a new question with a candidate narrow rule (`T2.md`). This proposal does not claim to close it.
8. *(critic round 1) The frame's disposal.* The draft read "live" as "has a later explicit use". A bound frame's scope-end disposal runs its pending `finally` (§29.7), so under that reading K5.9 missed a partially-advanced enumerator local whose `finally` reads the span. Rule 1 now counts the disposal as a use. Cost: `using var q := xs.Where(f); … ; sink.Take(move xs);` in one scope — §29.11's recommended spelling — is rejected even after `q`'s consumption; the remedy is an inner block, an unbound query, or a plain `var` disposed explicitly. Re-measured over `tests/`, lit, `docs/` and the rule corpus: zero sites move a carried source under a bound frame local with or without a later explicit use (five `Dispose()` sites of class-sourced frame locals, all unfrozen under the flag).
9. *(critic round 1) Holders of a span.* K5.9 named "a span local or a live frame local"; a span held in a record, inline-array or choice local, or captured by a live closure, is the same flagless borrow, and the text now names every holder K5.5 traces.
10. *Diagnostics:* the K5.9 diagnostic names the consumed binding, the span, and the span's surviving use — the same triple as the transfer case; spec Appendix D's identity rule (no error codes) is respected.
