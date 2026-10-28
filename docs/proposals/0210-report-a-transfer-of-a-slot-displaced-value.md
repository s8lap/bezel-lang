# Proposal 0210 — The trust-admission report names a transfer of a slot-displaced value

| | |
|---|---|
| **Status** | graduated-v0.350 — superseded — graduated v0.350 (accepted 2026-10-28 as a delegated ruling under the owner's 2026-10-08 direction ("Please resolve and decide all known spec related issues. Do proposals for issues that need them.") and the owner's 2026-10-28 approval of the proposal-0131 review's small train on a review branch: the trust-admission report gains one entry — where a replacing `move` takes an owned field's value while a local initialized from a direct read of the same path is still used, the transfer of the taken value is reported, at the consume-table rows proposal 0157's entry uses — and the slot kill by move's cross-body residual is stated in §28.3.9, K4 and the zero-report claim's exclusions; no verdict changes; critique round 1 applied (§10); landed with the v0.350 changelog entry) |
| **Tracking issue** | [#7434](https://github.com/s8lap/bezel-lang/issues/7434) (open `spec-question`, `campaign:borrow-provenance`): the kernel model's F1 finding, the shape this proposal gives a report site; the compiler tracker is [#7479](https://github.com/s8lap/bezel-lang/issues/7479), filed at the v0.350 landing (§7) |
| **Target module(s)** | **spec** — §28.3.12 *The trust-admission report* (clause 3: two sentences after proposal 0157's entry; the zero-report claim's exclusions: one clause), §28.3.9 *Scope* clause 1 (two sentences), §28.3.1 K4 clause 3 (the enumeration sentence). **rationale** — §5.31 (one paragraph). **code-generation reference** — §4.6 (the opt-in report sentence). **proposals** — `README.md` (this row; the never-used numbers sentence), `DECLINED.md` (four `declined-at-proposal` rows, one `deferred` row). No grammar change; no verdict changes; the default build emits nothing new. |
| **Written against** | spec **v0.349** (`5a984a691`, origin/main; the docs are unchanged from `293b9278c`, where the draft was written) |
| **Origin** | The proposal-0131 review (2026-10-28, `docs/research/trust-report-review/`). §28.3.9 *Scope* clause 1 calls a slot kill by move "governed by the borrow lifetime rules' domain clause above ([K4.3]), a recorded obligation"; K4 clause 3 says "the recorded set is enumerable, and the trust-admission report mode enumerates it at every site"; proposal 0131 §1.2 excluded the slot kill from the report on the ground that it is "the domain clause of Family A/B attached to the holder that admitted the borrow; it is reported once, at that holder's store". For a **local** borrow of the slot's value there is no such holder and no such store — the one K4.3-named obligation with no report site. The executable kernel model (`docs/research/soundness/bzcore-l1/README.md` §6.5 F1) reaches it: 6 of 169,632 accepted programs at locals+fields, vars 3, len 5, each a race under the recorded clause. |

*Reference convention: unprefixed §-references are language-spec references at v0.349; K tags are §28.3.1's kernel ids. Quotations are verbatim. ▲ marks proposed text. Programs use only members the standard library declares or declare their own (`Q`, `R`, `W`). MEASURED marks a verdict taken with the r35 release compiler (`bezelc 0.0.0-f6863e62`, `--checked-disposal --report-trust-admissions`; probes under `docs/research/trust-report-review/probes/` and `docs/research/trust-report-review/followup-probes/`) or with the executable model (`docs/research/trust-report-review/`).*

## 0. Thesis

The register the report enumerates has one obligation the text names as recorded trust and no site reports: the slot kill *by move* under a live local borrow of the slot's prior value, when the taken value is then transferred. Give the one-body form of it the site proposal 0157 gave the collection-held body source — the **transfer**, not the kill — and state the rest of it, which no single body can see, where a reader of the slot kill and of the zero-report claim looks. Nothing is rejected.

## 1. The current state, precisely

§28.3.9 *Scope*, clause 1 (v0.349):

> **Slot kills by move.** A slot kill *by move* — `move box.f with e` (or a bare `move box.f` at a nullable `f`) into a transfer, a spawn argument, or one of the domain-reachable constructions of the domain clause — disposes nothing and sets no flag, and is governed by the borrow lifetime rules' domain clause above ([K4.3]), a recorded obligation, not by this exemption. […]

§28.3.12 *The trust-admission report*, clause 3, names two sites: the admitting site, and (0157) the transfer of a collection-held body source. The slot kill under a local borrow is neither. The local borrow `var b := a.f;` is admitted by K9.1 with the holder `a` as its source (K1.5; fields are not frozen, K5.6), and the replacing `move` by K9.1/K9.2. The domain clause bites only when the taken value is **transferred** while `b` is still used: a read through `b` after the transfer is a read of an object another domain owns. The model's witness (#7434's program; `R` declares `public owned f: Q`):

```bezel
var a := new R(new Q());
var b := a.f;                          // K9.1: a borrow of the slot's value; source {a}
var c := move a.f with new Q();        // the replacing move: `b` now aliases `c`'s object (§28.8.1: "bounded afterwards by the taker")
Domain.Spawn(W, move c);               // K3.3: not frozen — no live borrow names `c`
b.M();                                 // a cross-domain read; the recorded K4.3 obligation is broken
```

**What r35 can measure.** The one-body spelling cannot be compiled on r35: `move _f with new Q()` does not parse (e09; the #5923 note) and the nullable bare take `move _f!` is rejected on the exception edge (e03, f01) — r35 still implements the restoration window v0.336 retired. Those are r35 limitations, not the spec's. The nearest compilable twin moves the slot in a **callee** (`var b := a.F; var c := a.Take(); using var t := Domain.Spawn<Q>(W, move c); return b.M();`, `F => _f!`, `Take(): owned Q => move _f!`; e02): MEASURED, accepted in both builds, 0 report lines, and the run is a race — 10 of 20 runs exit 0 and 10 raise `DisposedAccessException` (exit 70), depending on whether the spawned domain's disposal of `q` has set the flag before the main domain's read. (The probe log `docs/research/trust-report-review/probes/results.md` records e02 as rejected; that row predates the probe's `sealed` payload and is stale.) The callee form is exactly the shape this proposal **does not** report (§2.2): it is measured here as the residual, not as the target.

## 2. The rule ▲

### 2.1 The report entry

§28.3.12 *The trust-admission report*, clause 3, appended after "the consume-site freeze leaving it admitted.":

> ▲ The same holds for a **slot-displaced value**: where a replacing `move` ([§28.8.1]) — `move r.f with e`, or a bare `move r.f` at a nullable `f` — takes an owned field's value after a local was initialized from a direct read of the same path ([§28.7]'s *Use after a dominating field write* form, `r` not assigned between the read and the `move`), the transfer of the taken value at the same rows — the `move` itself where it stands in one of their positions, or a transfer of the binding that received it — is reported on a path on which that local is used after the transfer: that transfer is where [§28.3.9] *Scope* clause 1's slot kill by move meets the domain clause, the freeze leaving it admitted, since no live borrow names the taker. The kill itself is never reported, nor a transfer the local's last use precedes.

Precisely:

- **Which moves.** The replacing `move` of an owned instance field, with or without `with`, through any root §28.7's form admits (`this`, written or implicit, a local variable, a non-`ref` parameter), one field segment off the root. Not an owning-collection or owning-inline-array slot (§4 item 4), not a two-segment path.
- **Which borrows.** Exactly §28.7's local: `var b := r.f;` or `const b := r.f;`, read before the `move` on the path to it, `r` not assigned between. Not an alias of it, a closure, a borrow-bearing record or choice, a span, or a borrow returned by a member (`var b := r.F;` over a property) — §2.2.
- **Which transfers.** The obligation-moving kills that set no flag, the rows proposal 0157's entry names (K3.1–K3.4, K3.6, K3.7, K3.12): a store into an owned location, an `owned` return, an `owned` argument (a spawn argument included), a marked transfer, a Pattern A chaining source, an owning `yield return`, a move-only store. The site is the `move` itself where it stands in such a position (`Domain.Spawn(W, move a.f with new Q())`), else the first such transfer of the binding that received the taken value.
- **Liveness.** The local is used — read, member-accessed, passed, re-borrowed or captured — on a path from the transfer. A transfer after the local's last use is silent: unlike a collection's retention (rationale §5.31's 0157 paragraph: "retention is not body-local"), a local's later uses are in the body.
- **What the line says.** Clause 2's content: the rule and its clause (§28.3.9 *Scope* clause 1, the domain clause), the borrow (`b`) and its source (`r.f`'s displaced value), the holder (the local `b`), the obligation (the taken value is not transferred to another domain, or consumed into a domain-reachable construction, while `b` is used), and the remedies at that site: use `b` before the `move`, re-borrow from the field after it (the field holds the replacement), or take the borrow from the taker (`var b := c;` after the `move`, which the freeze then sees).

### 2.2 The residual, stated

§28.3.9 *Scope* clause 1, appended:

> ▲ The trust-admission report ([§28.3], clause 3) names the transfer of the taken value while a local read directly from the moved path in the same body is still used; any other holder of the displaced value in the body — an alias of such a local, a closure, a borrow-bearing record or choice, a borrow returned by a member — and a borrow whose slot a replacing `move` in another body displaces (a callee's `move _f` under a caller's `var b := a.F;`) carry the obligation with no report line: their source sets name the holder, not the slot's value ([K5](#sec-kernel-k5)), and a callee's `move` is not visible in the caller. A field-, collection- or frame-held borrow of the displaced value is reported at its own store.

K4 clause 3's last sentence:

> The recorded set is enumerable, and the trust-admission report mode ([§28.3]) enumerates it at every site▲, the slot kill by move's residual that [§28.3.9] *Scope* clause 1 states apart.

The zero-report claim's exclusions, after axiom A10's certification grants:

> … and not the certification grants of axiom [A10]▲, nor, of the slot kill by move, more than clause 3's slot-displaced entry: the residual [§28.3.9] *Scope* clause 1 states carries no line — and it is per compilation unit …

The last sentence of the clause-1 addition is a reminder, not a new site: such a holder's store is reported already (the borrow is field-path-sourced), so the residual is confined to local holders other than the direct-read local and to the cross-body form.

### 2.3 Why the transfer and not the kill

The same ground as 0157 (rationale §5.31): the kill alone meets no obligation — `var c := move a.f with new Q(); c.Dispose(); b.M();` is K4.2's flag-backstopped fault, and a later overwrite is K3.11's — so a line at every replacing `move` under a live borrow would list every swap a program performs. The clause comes into force where the taken value leaves the domain's flag discipline: K3.3's spawn argument, a channel send, `FrozenArray<T>.From(move …)`, a `Shared<T>`/`Locked<T>` construction then shared, a store into an admitted static (§28.3's domain clause, clause 4). The rows are 0157's, for the same reason 0157 gives: the report cannot see what a callee or a new holder does with the value.

### 2.4 Why not an error (the 0205/0206 pattern)

The certain sub-shape is real — one body, straight line, the spawn dominating the use — but the analysis it needs is a *slot-displaced alias*: the local `b` re-sourced to the taker `c` at the move. K5 does not trace it, and #7434 records it as a kernel change. The model's nearest approximation, the `fmove` taint, was run as a freeze rule (`docs/research/trust-report-review/mutation_f1_slot_displaced_frozen.py`, MEASURED): at locals+fields, vars 3, len 5 it rejects **552** programs, **510 of which never fault** (witness `new a / brw b a / fmove c a / fstm b c`), because the model cannot tell a borrow of the holder from a borrow of the slot. The sharp rule waits on #7434 option 2, with #7432's per-field value generation as its prerequisite (§4 item 3). This proposal takes the half that is decidable now: the entry is a report, the shape becomes visible under the audit, and nothing compiles differently.

## 3. Traced examples

`R` declares `public owned f: Q`; `a` is a local holding an `R` unless a row says otherwise; `W(owned q: Q)` is a static method; `Take(owned q: Q)` a same-domain one.

| Program (fragment) | Mode on | Default |
|---|---|---|
| `var b := a.f; var c := move a.f with new Q(); using var t := Domain.Spawn(W, move c); b.M();` (#7434) | **reports** at `move c` | silent |
| `var b := a.f; using var t := Domain.Spawn(W, move a.f with new Q()); b.M();` | **reports** at the `move` | silent |
| `var b := a.f; var c := move a.f with new Q(); Take(move c); b.M();` (same-domain callee) | **reports** at `move c` (row K3.3; the report cannot see the callee) | silent |
| `var b := a.f; var c := move a.f with new Q(); c.Dispose(); b.M();` | silent (K4.2, flag-backstopped; the `b.M()` faults in checked mode) | silent |
| `var c := move a.f with new Q(); using var t := Domain.Spawn(W, move c);` (no slot borrow) | silent | silent |
| `var b := a.f; b.M(); var c := move a.f with new Q(); using var t := Domain.Spawn(W, move c);` (`b` not used after the transfer) | silent | silent |
| `var b := a.f; var b2 := b; var c := move a.f with new Q(); using var t := Domain.Spawn(W, move c); b2.M();` (an alias) | silent — the residual (§2.2) | silent |
| `var b := a.F; var c := a.Take(); using var t := Domain.Spawn(W, move c); b.M();` (a member read; the `move` in a callee; e02) | silent — the residual (§2.2) | silent |
| `this._peer := a.f; var c := move a.f with new Q(); using var t := Domain.Spawn(W, move c);` (`a` a plain parameter) | reports at the store (the field-held clause; unchanged) | silent |

The third row is the price of the rule's locality (as 0157's `sink.Take(move item)` row is): the audit lists it, the reader dismisses it.

## 4. Alternatives considered

1. **Leave the shape unreported and narrow K4's sentence and the zero-report claim to except every slot kill by move.** Declined: the one-body shape is decidable from facts the compiler keeps (§28.7's direct-read local, the consume rows), and a hole no report line shows the reader where one body decides it is the ground DECLINED's proposal 0157 option B row already records. The residual this proposal states is the part one body cannot decide.
2. **Report at the replacing `move`.** Declined on 0157's noise ground: every swap under a live borrow, most of which never meet a transfer.
3. **Freeze the transfer (K5 source tracing of the slot-displaced alias).** Deferred to #7434 with §2.4's measurement as the bar: a model that distinguishes a slot borrow from a holder borrow and shows the freeze's over-rejection at zero.
4. **The owning-collection slot leg** (`var w := xs[i]; var c := move xs[i] with e;`). Declined for v1 (draft Q2 answered no): "the same slot" is not a path fact — an `Insert` or `RemoveAt` between the read and the `move` shifts it — §28.7's form is field-only and proposal 0206 left the slot leg to the run time on its own grounds; zero sites measured.
5. **Reach the residual** — at a callee's replacing `move` whose value leaves its body (every `Take`/`Swap` method), or at the caller's transfer of any `owned` result of a call on a receiver a live local borrow names, or along aliases. Declined: the first is alternative 2's noise one body over; the second reports transfers that displace nothing (`var n := conn.Name; var m := conn.TakeMessage(); ch.Send(move m); n.Length`); aliases need a source set that names the slot's value, which K5.5's do not (DECLINED's proposal 0206 widening row records the same ground).

## 5. Kernel and body-locality classification

**Kernel:** a derivation from K4.3's text ("no new admission may be made without recording its clause") applied to an admission §28.3.9 already records; no new row, no axiom. **Body locality (0105 §1):** local — the read, the `move`, the transfer and the later use are in one body; the cross-body form is stated as a residual rather than reached.

## 6. Migration and compatibility

Newly warned, under `--report-trust-admissions` only: the transfers of §2.1. Newly rejected: nothing. The default build: nothing. MEASURED reach (a scan of `tests/` and `docs/`): 743 slot `move` sites; 10 of them have a direct-read local of the same path within 25 lines before, and none of the 10 is followed by a transfer of the taken value while that local is used — the only instance of the shape is the research model's own witness (`docs/research/soundness/bzcore-l1/README.md` §6.5). Zero report lines in the tree.

## 7. Implementation sketch

In the ownership pass, at a replacing `move` of a one-segment field path `r.f`, collect the live locals initialized from a direct read of the same path with `r` not assigned since — the set §28.7's *Use after a dominating field write* already computes for its window — and attach it to the binding that receives the result (or to the `move`'s own consume position). At each K3.1–K3.4/K3.6/K3.7/K3.12 consume of that binding, emit the trust-admission line if any local in the set is used on a path from the consume. Size: S. The compiler tracker is filed at landing, after the report's existing gaps (#7465–#7471); the replacing `move`'s parse (#5923) is a prerequisite for any run probe.

## 8. Corpus accounting

New `s28_3_NNNN` cases, `flags: --report-trust-admissions`: `warn` / `diagnostic: trust` for §3 rows 1 and 2 (a `sealed` payload), `quiet` for rows 4–6, and a default-build `quiet` twin of row 1. Not written in this landing (process §5 item 3c: a ruling that opens coverage records the gap); each needs the replacing `move` to parse to be measured.

## 9. Open questions

- Q1 (draft): should a plain `move` into a same-domain `owned` parameter be reported (§3 row 3)? **Answered yes**, as 0157 decided for collections: the report cannot see the callee.
- Q2 (draft): the owning-collection slot leg in v1? **Answered no** (§4 item 4).
- Q3: should the line name the remedy "take the borrow from the taker"? Recommended yes (§2.1's last bullet); the wording is the implementation's, as clause 2's remedies are.

## 10. Critique round 1 (the v0.350 critic, 2026-10-28)

- **The draft's rule did not cover its own example.** §1's program and §3 row 1 used a property read (`var b := a.F`) and a callee's `move` (`a.Take()`), while the rule and §7 key on a direct read and a `move` in one body; and §5 called the rule local. The examples are now #7434's one-body program; the callee form is a §3 row of its own, silent, and §2.2 states it as the residual — in §28.3.9, in K4's sentence and in the zero-report claim's exclusions — since the draft's thesis ("the K4 enumeration sentence is true") was false for it.
- **The e02 measurement was misreported.** The draft said "runs to exit 0 (the race read returns the payload)"; re-run 20 times, the outcome is a race between the spawned domain's disposal and the read (10 × exit 0, 10 × `DisposedAccessException`). `results.md`'s e02 row (rejected) is stale.
- **"Live" was undefined.** Now: used on a path from the transfer. The `move` in a transfer position (§3 row 2) had no site; it is the site.
- **The borrow was undefined.** Now exactly §28.7's local; aliases and member reads are named as the residual, on the ground DECLINED's 0206 widening row records.
- **Q2 reversed** (the slot leg declined for v1), with a `declined-at-proposal` row.
- **The reach scan** was `grep Domain.Spawn` (236 sites); it is now a scan of the slot `move` sites (§6).
