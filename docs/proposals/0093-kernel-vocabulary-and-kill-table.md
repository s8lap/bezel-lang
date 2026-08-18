# Proposal 0093 — Kernel vocabulary: the three announcements, the kill table, and one freeze criterion

| | |
|---|---|
| **Status** | accepted — ruled under the owner's 2026-08-18 delegation; awaiting graduation train |
| **Tracking issue** | coordinator files |
| **Target module(s)** | **spec** — §28.3 (the kernel: K3.13, K4, K4.2, K5, K5.1–K5.7, K6.2, K2.4; the axiom register's A2/A4/A7 rows; *Move semantics for owned locals*; *The consume-site freeze*; *Owned-local discharge requirement*; the *Choosing a disposal form* recap table), §17.13, §22.3, §23.3, §28.7, §28.8.1, §28.9, §28.11, §28.14, §29.2, §30.5, §30.12, §31.1, §35.4, §35.5, Appendix A (the two indented commentary lines at A-grammar `MoveExpr`/field-path notes), Appendix C.3/C.6; **rationale** — §3.10, §5.1, §5.10, §5.23, §5.26, §5.28, §12.10 (the announcement-doctrine paragraphs); **stdlib** — §1, §6.8, §6.12, §4.19 (six announcement sentences); **guides** — 01 (4 sites), 04 (4 sites), 05 (1 site). Doc-only: **no semantics move.** |
| **Written against** | spec/stdlib/rationale/guides **v0.255** (git `HEAD`, snapshot taken 2026-08-18). Every count in this document is measured against that snapshot; line numbers cited are snapshot line numbers and are given only as drafting aids, never as citations. |
| **Origin** | Review-program charter items **B1** and **B2** (consolidation basket), ruled under the owner's 2026-08-18 delegation. The trigger: §28.3's kernel carries one word — *announced* — for three unrelated propositions, a two-valued kill taxonomy (`disposing` / `transferring`) that cannot express two of its own five kill kinds, a freeze criterion stated three times with two strengthenings appended rather than derived, and a §35.4 census defined by subtraction from the label it is orthogonal to. |

*Reference convention: unprefixed §-references are language-spec references; `K`/`A` tags are §28.3's kernel and axiom-register ids; rationale, stdlib, and guide references are marked. All prior art was read in full at the snapshot: §28.3 (kernel K1–K9, axiom register, Producers, Factory methods, Bare-type-parameter returns, Move semantics, Borrow lifetime rules, the consume-site freeze, the discharge requirement), §9.7, §17.13, §28.7, §28.14, §29.2, §30.5, §30.12, §31.1, §35.4.*

---

## 0. What this proposal is, and is not

It is a **restatement program**. Every rule it touches keeps its current verdict on every program. There is no new admission, no new rejection, no changed diagnostic *semantics*, no kernel extension, and no axiom movement. Three things happen:

1. one overloaded word is split into three named senses, and every downstream citation is re-pointed (**B1a**);
2. K4's two-valued kill taxonomy is replaced by a three-bit table that reproduces every landed verdict and isolates the one recorded override (**B1b**);
3. K5's criterion is stated **once**, with the §28.14 and §30.12 strengthenings *derived from it* rather than appended to it (**B1c**); and §35.4 is restated census-first, its equivalence-proof parenthetical retired (**B2**).

The acceptance test for the whole program is stated in §7: the kill table's five rows and the freeze criterion's four outcomes must reproduce, cell for cell, the verdicts the landed text already states.

## 1. The word `announced` carries three propositions (B1a)

### 1.1 What the kernel actually says

Three kernel clauses use the word for three different things.

- **K3.13 — the announcement rule.** *"A consume is spelled by the transfer marker (`move x`) except where the target or keyword already announces it — a declared `owned` slot or return, a `set(owned)` accessor, an unmarked `Task`-family return, an owning iterator's declared interface, `throw`, `await` of an `OwningTask<T>`, the discard."* The proposition is about a **position**: does this consume need a marker?
- **K4.2 — announced kills.** *"A kill that is both **keyword-visible at the kill site** and **flag-backstopped** is permitted while borrows of the value live."* The proposition is about a **kill**: is it exempt from the freeze?
- **K2.4 / K6.2 — announcement by declared type.** K2.4: *"A callee's declared signature announces its result's source set…"*; K6.2 is titled *"Announcement by declared type."* The proposition is about a **signature**: what classification does a call site read off the declared return type?

These are independent. K3.13's set and K4.2's set overlap but neither contains the other: `throw x` and Pattern B `await x` are in both; the discard and `Dispose()` are in both; an `owned`-slot store and an owning `yield return` are K3.13-announced but **not** K4.2 kills (they are frozen); a `move` marker is in neither. K2.4/K6.2's sense is disjoint from both — it classifies results, not consumes.

The conflation is not hypothetical. §30.5's Pattern B paragraph runs all three in one sentence:

> "this consume is a [K4.2] **announced kill** with a runtime backstop: the `await` keyword plus the single-consumer operand type announce the kill at the source … are themselves the announcement ([K3.13]'s announced set), and the transfer marker is not admitted where the target already announces the consume"

Three senses, one word, one sentence, three different kernel clauses cited. §31.1's `throw` paragraph and Appendix C.3's yield row do the same.

### 1.2 The three names

| Sense | Proposition | Definition site | Name |
|---|---|---|---|
| K3.13 | this consume position needs no transfer marker, because its destination or keyword says "consume" | K3.13 | **target-announced** |
| K4.2 | this kill is keyword-visible at the site and flag-backstopped, so it is exempt from the freeze | K4.2 | **site-announced** |
| K2.4, K6.2 | this signature's declared return type states its result's ownership and carried-source classification | K2.4, K6.2 | **type-announced** |

The three qualifiers are one word each, share a common suffix (so the family is visible), and differ in their first syllable (so the members are not). Each keeps its existing definition site; nothing moves.

### 1.3 A fourth usage, found and reported

The charter names three senses. The snapshot carries a **fourth**, and it is the *complement* of the first: eleven sites say the **marker** announces.

> rationale §3.10: *"`move` announces a field take-out at the read site"*; *"the marker announces invalidation, not the parameter's mode"*
> stdlib §6.12: *"its consuming form announced by the call-site marker (`Task.WhenAll(move tasks)`)"*
> Guide 04: *"the `move` marker announcing the transfer"*
> spec §30.10: *"announced at the call site by the transfer marker"*

Under K3.13 the marker is what a writer supplies **because nothing announces**; these eleven sites say the marker *is* the announcement. Both readings are defensible in isolation and together they make the word mean a thing and its complement.

**Recommendation (reported, not improvised — the ruling is the coordinator's, question Q1 below):** do not name a fourth sense. Reserve *announce* for the three kernel senses, and respell the eleven sites to **spells** / **marks** — "the `move` marker **spells** the take-out at the read site", "its consuming form **spelled** by the call-site marker". Measured: **11 paragraph sites** (spec 1, rationale 5, stdlib 3, Guide 04 2), no normative content in any of them.

### 1.4 What the split costs, measured

Occurrences of `announc*` across the nine normative documents at v0.255 (`Bezel_Changelog.md` excluded — pinned history, never edited):

| Document | occurrences | paragraph sites | of which out-of-family (§1.4 note) | **in scope** |
|---|---|---|---|---|
| `Bezel_Language_Spec.md` | 123 | 77 | 4 | 73 |
| `Bezel_Design_Rationale.md` | 48 | 29 | 4 | 25 |
| `Bezel_Standard_Library.md` | 7 | 6 | 0 | 6 |
| `Bezel_Guide_01_Memory_and_Ownership.md` | 5 | 4 | 1 | 3 |
| `Bezel_Guide_04_Concurrency.md` | 4 | 4 | 1 | 3 |
| `Bezel_Guide_05_Collections_and_Iteration.md` | 1 | 1 | 0 | 1 |
| `Bezel_Guide_06_Contracts_and_Purity.md` | 1 | 1 | 1 | 0 |
| **Total** | **189** | **122** | **11** | **111** |

Classified by sense. Method: every occurrence read in context at the snapshot; a paragraph is assigned **every** sense it carries. The sense column is therefore *assignments*, not sites — 134 assignments over 122 sites, so **12 paragraphs carry more than one sense**, and those twelve are the conflation this item exists to end.

| Sense | assignments | of which spec |
|---|---|---|
| **target-announced** (K3.13) | 31 | 24 |
| **site-announced** (K4.2) | 30 | 20 |
| **type-announced** (K2.4/K6.2) | 51 | 34 |
| *marker-announced* (§1.3 — respelled away) | 11 | 1 |
| out-of-family | 11 | 4 |

The out-of-family eleven are ordinary English and are **not touched**: §9.4's store "announced by the assignment itself" and its two rationale §9.13 twins; §28.9's "the ceremony announces observable disposal effects" and its rationale §5.28 and Guide 01 echoes; §11.2's "the callee retains less than announced"; §30.5's "the pin reading announces a scope"; rationale §12.2's "fan-out is announced in a design"; Guide 04's "the trap announces itself"; Guide 06's "a `Try` name announces". So the re-pointing work is **111 sites carrying 171 occurrences**, of which 11 sites (11 occurrences) are respellings rather than re-pointings.

Kernel-tag citation counts at the snapshot, for the train's cross-check that every re-pointed site lands on a tag that exists: `K3.13` 9, `K4.2` 15, `K2.4` 18, `K6.2` 8.

### 1.5 Exact-rule sketch

K3.13's opening sentence gains its name and nothing else:

> 13. **Target-announced consumes.** A consume is spelled by the transfer marker (`move x`) except where the target or keyword already announces it — the **target-announced** positions: a declared `owned` slot or return, a `set(owned)` accessor, an unmarked `Task`-family return, an owning iterator's declared interface, `throw`, `await` of an `OwningTask<T>`, the discard. …

K4.2's opening likewise:

> 2. **Site-announced kills.** A kill that is both **keyword-visible at the kill site** and **flag-backstopped** is **site-announced**, and is permitted while borrows of the value live … The closed set, with each kill's disposal moment: …

K6.2 keeps its title verbatim (*Announcement by declared type*) and gains the short name in its first sentence: *"Frame-carrying classification is **type-announced** — keyed on the callee's declared, pre-substitution return type …"*. K2.4 gains the same word.

Downstream, every citation names the sense: §30.5's Pattern B sentence becomes *"this consume is a K4.2 **site-announced** kill with a runtime backstop: the `await` keyword plus the single-consumer operand type are the site announcement, and the position is **target-announced** (K3.13), so the transfer marker is not admitted."*

## 2. The kill table replaces the disposing/transferring binary (B1b)

### 2.1 The binary cannot express its own contents

K4's opening reads: *"a **disposing** kill runs `Dispose()` and sets the per-instance disposed flag; a **transferring** kill leaves the value alive under a new holder, setting nothing."* Two of the kernel's five kill kinds fit neither half:

- **`throw x` and Pattern B `await x`** are listed by K4.2 as announced kills whose post-state is **moved-from** — i.e. the obligation *moved* (so: transferring) — yet K5.3 admits them precisely because they are **flag-backstopped**, which the binary reserves for disposing kills. They are transferring kills that are nonetheless flag-backstopped, because the new holder (the exception machinery, the await's implicit disposal) is guaranteed to dispose. The binary has no cell for this.
- **Drop-on-overwrite** disposes and sets the flag — squarely "disposing" — and is nevertheless **frozen**, which K5.2 has to explain by appeal to a property the binary does not carry: *"the reassignment is not keyword-visible as a kill of the *old* value."*

So the kernel already reasons with three independent bits and names only two of them.

### 2.2 The three bits

- **obligation-moves** — does the obligation leave this holder for a new one? (If not, the value dies here.)
- **flag-set** — does the kill guarantee the per-instance disposed flag is set, on a schedule the K4.1 backstop can rely on?
- **keyword-visible** — is the kill spelled at the site by a keyword or a call the reader sees?

Two derived verdicts follow, and nothing else does:

- **post-state of the binding** = *explicitly-disposed* when `¬moves ∧ flag`; *moved-from* when `moves`. (A conditional kill of any kind joins to *maybe-consumed*, K7.4 — one clause, stated once, applying to every row.)
- **freeze** = unfrozen iff `keyword-visible ∧ flag-set-is-an-admissible-backstop` (§3), with K5.4's single recorded structural override.

### 2.3 The table, as drafted

Subject: a kill of an owned **binding** — a local or an `owned` parameter. Kills of a **field or collection slot** are outside the freeze entirely (K5.6) and governed by K4.3's domain clause; that exemption is one sentence beneath the table, not a row in it.

| # | Kill | moves | flag | kw | Post-state of the binding | Freeze | Disposal moment | Kernel |
|---|---|:---:|:---:|:---:|---|---|---|---|
| 1 | explicit `Dispose()` (incl. `x?.Dispose()` / `x!.Dispose()`); the discard `_ := x;` | no | **yes** | **yes** | **explicitly-disposed** — straight-line use afterward is §28.2's mandatory use-after-dispose warning | **unfrozen** | at the call / at the end of the discard statement | K3.9, K3.10, K4.2, K5.3 |
| 2 | `throw x`; Pattern B `await x` on an owned `OwningTask<T>` binding | **yes** | **yes** | **yes** | **moved-from** — a later read is an error until reassigned | **unfrozen** | after the matching catch arm completes, or at termination if uncaught / at the await, on all outgoing edges including fault and cancel | K3.5, K3.8, K4.2, K5.3 |
| 3 | every transfer: `move x` at an argument, binding, assignment, scrutinee, or arm; a store into an `owned` field, an owning-collection slot, or a `set(owned)` accessor; `return` under an `owned` return, or under an unmarked `Task`-family return in a non-`async` body; an owning iterator's `yield return`; a move-only value's store into an owning location | **yes** | no | *varies* | **moved-from** | **frozen** | none here — the new holder's | K3.1–K3.4, K3.7, K3.12, K5.1 |
| 4 | drop-on-overwrite — the consume of the **displaced** value at `x := …` | no | **yes** | **no** | the binding takes the new value; the **displaced** value is disposed | **frozen** | after the RHS is evaluated (RHS-first, K9.5) | K3.11, K5.2, §28.7 |
| 5 | Pattern A chaining `return await x` over an owned source (an `owned` parameter or plain owned local) | **yes** | no | **yes** (the form) | **moved-from** | **unfrozen — by K5.4's override, not by the criterion** | the outer task's teardown, transitively | K3.6, K5.4, §30.5 kind 2 |

**Reading notes, normative with the table:**

- Row 3's `kw` column varies by position — the marker at K3.3/K3.4 is keyword-visible, the target-announced positions K3.1/K3.2/K3.7/K3.12 are not — and the variation is **immaterial**: with `flag = no` the criterion's second conjunct already fails, so every row-3 position is frozen whatever its spelling. This is the table's first structural result and it is why the row is one row.
- Rows 1 and 2 are exactly **K4.2's closed set**, and K4.2 remains its one definition site. Row 1 is the binary's "disposing"; row 2 is a *transferring* kill that is nonetheless flag-backstopped — the cell the binary lacked.
- Row 4 is the proof that the **keyword-visible** bit is load-bearing: `flag = yes` and it is still frozen. Row 3 is the proof that the **flag** bit is load-bearing: `kw = yes` (for `move x`) and it is still frozen. Each bit is decisive in exactly one row; neither is redundant.
- Row 5 is the table's **single override**. The bits predict *frozen* (`flag = no`); the landed verdict is *unfrozen*, for a structural reason that is not the criterion: the new holder — the outer task — cannot dispose the chained task before the body, `finally` blocks included, completes, so no body borrow can outlive the kill. K5.4 already records this as "the one recorded structural override"; the table's contribution is to make its exceptional status visible at a glance instead of by reading five clauses.
- A **conditional** kill of any row joins to **maybe-consumed** (K7.4): unreadable after the join, scope-end disposal per path.

### 2.4 Every verdict reproduced

Each row was built from §28.3 at the snapshot and checked against the landed rule that states it:

| Row | Landed statement checked against | Agrees |
|---|---|---|
| 1 post-state | K4.2: *"after a *disposing* announced kill (`Dispose()`, the discard) the binding is **explicitly-disposed**"* | ✔ |
| 1 freeze | K5.3; §28.3 *The explicit-`Dispose()` boundary*; §17.13 (*"the freeze does not apply"*) | ✔ |
| 2 post-state | K4.2: *"after `throw x` or a Pattern B `await`, the operand binding is **moved-from**"* | ✔ |
| 2 freeze | K5.3; §30.5 (*"no call-site marker, no consume-site freeze, disposed-flag backstop"*); §31.1 | ✔ |
| 3 freeze | K5.1: *"moves, marked transfers, owned-slot stores, owned returns … and owning-iterator `yield return` are frozen — transferring kills, alive under a new holder, no flag"* | ✔ |
| 3 post-state | K1.3; §28.3 *Move semantics for owned locals* | ✔ |
| 4 freeze | K5.2: *"drop-on-overwrite of a binding is frozen although it disposes — the reassignment is not keyword-visible as a kill of the *old* value"* | ✔ |
| 4 disposal moment | K3.11 (*"disposed after the RHS is evaluated"*); K9.5 | ✔ |
| 5 freeze | K5.4's vacuity override; §30.5's chained-slot paragraph | ✔ |
| 5 post-state | §30.5 kind 2 (*"an ordinary §28.3 move-out of the parameter"*) | ✔ |
| slot/field exemption | K5.6; §28.3 *Scope of the freeze* | ✔ (stated beneath the table, not as a row) |

No row required a verdict the landed text does not already state, and no landed verdict is left without a row.

## 3. One freeze criterion, with the strengthenings derived (B1c)

### 3.1 The criterion today

It is stated three times: in K5's lead (*"unless the consume is a K4 announced kill (*unfrozen ⟺ announced ∧ flag-backstopped*)"*), in K5.3, and again in §28.3's *The explicit-`Dispose()` boundary* prose. Two strengthenings are then **appended** at their own sections, each with its own recorded reason:

- **§28.14** (K5.7): *"The storage freeze is deliberately *stronger* than the consume-site rule it extends … a stored callback's later invocation is invisible at a `Dispose()` site, and this admission promises a fully static bound, so §28.3's announced-kill carve-out does not apply to admitted captures."*
- **§30.12** (guards): *"the carve-out is justified by the killed object's own disposed-flag backstop, and killing a guard sets no flag on the *interior* that its surviving borrows reference, while releasing the lock out from under them."*

The second of these states, in its own words, a **general** proposition the criterion does not carry: the flag must be a backstop *for the borrows that survive*. Once the criterion carries it, both strengthenings stop being exceptions.

### 3.2 The restatement

> **K5 — The freeze.** Consuming an owned **binding** (a local or `owned` parameter; K3 positions) while a live local borrow of its current value exists is a compile error, **unless the kill is unfrozen** — and a kill is unfrozen exactly when both hold:
>
> **(i)** it is **keyword-visible at the kill site** (the reader's notice), and
> **(ii)** the disposed flag it guarantees is an **admissible backstop for every borrow that survives the kill** (the safety net) — the flag is set on the object those borrows reference, and the admission that produced them relies on the backstop rather than forswearing it.
>
> — **or** by K5.4's one recorded structural override (vacuity; the Pattern A chaining case).

Placements 1–7 then read as consequences, not as a second list:

- **K5.1** (transfers frozen) — fails (ii): a transferring kill sets no flag at all.
- **K5.2** (drop-on-overwrite frozen) — fails (i): the reassignment is not keyword-visible as a kill of the old value; the flag it does set is irrelevant once (i) fails.
- **K5.3** (K4.2's site-announced kills unfrozen) — satisfies both; the closed set stays stated once at K4.2.
- **K5.4** (Pattern A chaining) — the override, unchanged.
- **K5.5** (body-local by construction) — unchanged; it bounds *what liveness means*, not what is frozen.
- **K5.6** (fields and slots not frozen) — unchanged; it bounds *what the freeze's subject is*.
- **K5.7** (§28.14) — **derives from (ii):** the surviving borrow lives in a stored callback whose invocation site is not statically visible at the kill, and the storage admission's whole promise is a fully static bound — so a runtime flag is not an admissible backstop for it. §28.14 keeps its own sentence as the instance; it stops being an exception.
- **new, §30.12 guards** — **derives from (ii):** killing a guard sets the flag on the *guard*, while the surviving borrows reference the lock's *interior*, on which no flag is set. §30.12 keeps its own sentence as the instance and gains the K5 tag it currently lacks.

### 3.3 The seam, stated honestly

§30.12's ground is squarely coverage: *the flag is set on the wrong object*. §28.14's ground as recorded is subtly different: the flag **would** cover the captured source, but the admission "promises a fully static bound", so it declines to spend the backstop. Clause (ii) above unifies them by reading "admissible backstop" as covering both failure modes — wrong object, or an admission that forswore the backstop. That unification is a drafting choice this proposal makes and flags (**Q2**): the alternative is a third conjunct, *"(iii) the admission that produced the surviving borrows relies on the runtime backstop"*, which is more literal and less elegant. Either way both strengthenings derive and neither is appended; the choice is between one conjunct doing two jobs and two conjuncts each doing one.

## 4. §35.4, restated census-first (B2)

### 4.1 What the shape costs today

§35.4 defines the three-way classification first (rules 1–3), then defines the census in a separate paragraph **by subtraction** from rule 2 — *"rule 2's condition, freed of its 'otherwise' *and* of its label-keyed transitivity"* — and then spends a 96-word parenthetical proving the two conditions coincide except in one case rule 2 can never reach:

> "(The two conditions differ in exactly one place — a nested *owning* choice payload, borrow-carrying for the census iff that choice itself carries borrows — and rule 2 never meets it, because a move-only payload fires rule 1 first. Record and inline-array payloads follow §9.7's bullets verbatim, and a record may hold no move-only field ([§15.3]; [§9.7]'s first paragraph bars owning-inline-array fields), so no record can contain a borrow-carrying owning choice — at record payloads the census and the borrow-bearing label coincide.)"

The parenthetical exists **only because the definition order forces it**. Define the census first and rules 1–3 consume it; the proof has nothing to prove.

### 4.2 The restatement

> ### 35.4 The classification
>
> Two derived type properties ([§3.6](#sec-derived-type-properties)) are computed from the payload fields of *all* a choice's cases. The first is the input to the second.
>
> **A choice holds borrows** — a derived type property in its own right — iff any case has a *borrow-carrying* payload field under [§9.7](#sec-records-and-disposal)'s definition, reading **this property** at a nested choice payload. [The naming is proposal 0095's; at this proposal's revision the property is spelled as it is today.]
>
> The **choice classification** is then:
>
> 1. **owning** if any case has a payload field marked `owned` (a class, interface, or — at a class or interface instantiation — bare-type-parameter payload), or a payload field of a move-only value type ([§15.3](#sec-move-only-value-types) — an owning inline array or another owning choice, transitively);
> 2. otherwise **borrow-bearing** if the choice holds borrows;
> 3. otherwise **value-like**: every payload is a value, a `string`, a runtime-managed type, or (transitively) a value-like record or choice. A value-like choice is an ordinary copyable value type.
>
> The two are orthogonal by construction: the classification drives copy/move/dispose semantics, the property drives borrow obligations — it adds obligations, never subtracts semantics. A **borrow-bearing** choice holds borrows by rule 2; an **owning** choice *can* — `Loaded(owned response: Response, log: Logger)` is owning by rule 1 with `log` a held borrow; a **value-like** choice never does, by rule 3's construction. …

Everything after that ("Every choice that holds borrows — whatever its classification — is subject to §9.7's borrow-lifetime rules … A live local … counts as a live local borrow …") is unchanged prose. The equivalence-proof parenthetical **dies**: rule 2 now *is* the property, gated by rule 1's "otherwise", so there is no second condition to compare it to.

### 4.3 One clause of the parenthetical survives — elsewhere, and it already does

The parenthetical's record-side half (*"no record can contain a borrow-carrying owning choice — at record payloads the census and the borrow-bearing label coincide"*) is **already stated at §9.7**, in §9.7's own words, as a record rule:

> §9.7: "(For records the label/census split cannot arise: a move-only value may not be a field of a record ([§15.3]), and the first paragraph bars owning-inline-array fields, so no record can contain a borrow-carrying owning choice — a record that carries borrows *is* a borrow-bearing record.)"

So the §35.4 parenthetical is, in that half, a duplicate of a live §9.7 sentence. It deletes with nothing lost. **Verified at the snapshot; no third carrier found.**

## 5. Corpus evidence

**Documents.** The counts of §1.4 are the whole surface: 122 announcement sites (111 in scope), 5 kill-table rows re-deriving 12 landed statements, 1 freeze criterion currently stated 3 times with 2 appended strengthenings, 1 parenthetical retiring.

**Compiler and tests (read-only; no code changes are proposed).** Measured at the snapshot: `announc*` appears **44 times in `src/`** and **46 in `tests/`**. Most are comments and test names, but **eight `src/` sites put the word inside a string literal, of which five are user-visible diagnostic text** — and, tellingly, they carry three different senses:

| Site | Text | Sense |
|---|---|---|
| `src/sema/PureChecker.cpp:728` | "`_ := expr` is the announced consume" | target-announced |
| `src/ownership/Ownership.cpp:8658` | "announcement.) (§28.3 *Factory methods and sourceless returns*, …" | type-announced |
| `src/ownership/Ownership_Checks.cpp:6508` | "moved-from, which the call site must announce (§22.3, v0.169)" | marker-announced (§1.3) |
| `src/ownership/Ownership_Checks.cpp:6518` | "there is nothing to announce; remove `owned` (§22.3, v0.169)" | marker-announced |
| `src/ownership/Ownership_Checks.cpp:6533` | "invalidated, so there is nothing to announce; remove `owned` …" | marker-announced |

So the split has a **small, optional** compiler-side follow-up: five diagnostic strings whose wording would read better under the named senses. It is **not** a landing condition — a diagnostic is not required to quote the spec's internal vocabulary — and this proposal proposes no code change; the coordinator may file it as a wording ticket. *(Adjacent observation, reported not touched: the three `Ownership_Checks.cpp` strings say "remove `owned`" and cite v0.169, which predates proposal 0090's v0.246 marker flip to `move`; that looks like part of the [#3056](https://github.com/s8lap/bezel-lang/issues/3056) catch-up rather than anything this program creates.)*

The kill table introduces no new predicate the compiler does not already compute — `moves`, `flag`, and `kw` are, respectively, the moved-from transition, the disposed-flag emission, and the marker/keyword presence the parser already records.

**Nothing in the stdlib changes.** The six stdlib announcement sentences (stdlib §1 ×2, §6.8, §6.12 ×2, §4.19) are all type-announced or marker-announced; none states a rule.

## 6. Migration and compatibility (changelog house categories)

- **Newly rejected:** nothing.
- **Newly warned:** nothing.
- **Newly defined:** three names (*target-announced*, *site-announced*, *type-announced*) for three propositions the kernel already states; the kill table's three bits, each of which is a restatement of a property the kernel already reasons with.
- **Runtime behaviour:** unchanged everywhere.
- **Doc-only:** all of it. **Zero semantic migration.**
- **Citation sites re-pointed:** **111** (171 occurrences), of which **11** are respellings away from the marker-announced usage (§1.3) rather than re-pointings. Distribution: spec 73 sites, rationale 25, stdlib 6, Guide 01 3, Guide 04 3 (2 of them respellings), Guide 05 1, Guide 06 0.

## 7. Mechanical validation, as a landing condition

1. `scripts/check_docs.sh` (full, not `--quick`) green: anchor resolution, version consistency, §-citation sanity, statics-facts derivation, and example extraction over the current docs.
2. **The kill-table acceptance test.** For each of the 12 landed statements in §2.4's checklist, the reviewer confirms the table's cell reproduces it verbatim in verdict. A row that needs a verdict the landed text does not state is a **stop**: this is a restatement program, and such a finding is an issue, not an edit.
3. **The freeze-criterion acceptance test.** Placements K5.1–K5.7 plus §30.12's guards are each re-derived from the two conjuncts, and each derivation is checked against the placement's current sentence. K5.4 must remain the *only* override.
4. **A vocabulary sweep.** After the train, `grep -oi 'announc[a-z]*'` over the nine normative documents must return only: the three named senses, the eleven out-of-family uses of §1.4, and zero marker-announced uses. The count is the check.
5. **Before/after fragment extraction.** `scripts/extract_doc_examples.py` runs clean over the edited docs; every §28/§30/§35 fenced example is unchanged byte-for-byte (this program edits prose only — a changed fence is a defect).

## 8. Alternatives, costed

- **Alt A — name the senses but keep the disposing/transferring binary.** *For:* half the churn; the binary's two labels are load-bearing in K4's opening sentence and in two rationale paragraphs. *Against:* the binary's two cells cannot hold five kinds (§2.1), so K5.1–K5.4 must keep re-deriving the missing bits in prose; the vocabulary split then improves the *word* while leaving the *taxonomy* wrong. **Not recommended.**
- **Alt B — kill table only, no vocabulary split.** *For:* the table is the higher-value half and is self-contained. *Against:* the table's freeze column is stated in terms of "site-announced", so it either uses the new name or reintroduces the ambiguity in its own header. The two items are one edit. **Not recommended.**
- **Alt C — a fourth sense named for the marker (§1.3).** *For:* it is what eleven sites already say; renaming them is 11 edits the alternative avoids. *Against:* it makes *announce* mean a proposition and its complement, permanently, in the one section that most needs to be read literally. **Not recommended;** put to the coordinator as Q1.
- **Alt D — decline; add a teaching table to §28.3 instead.** *For:* zero risk. *Against:* a non-normative summary beside four normative statements of the same rule is a fifth carrier, and the drift class this program exists to end is exactly "N carriers of one rule". **Not recommended.**

## 9. Questions (drafting seams only — the rulings behind this proposal are settled)

- **Q1 — the marker-announced usage (§1.3).** Respell the eleven sites to *spells*/*marks*, reserving *announce* for the three kernel senses (recommended), or name a fourth sense?
- **Q2 — the freeze criterion's shape (§3.3).** Two conjuncts with (ii) reading "admissible backstop" broadly enough to cover both §28.14's and §30.12's grounds (recommended — one criterion, two instances), or three conjuncts with (iii) stating the admission-relies-on-the-backstop condition literally?
- **Q3 — where the kill table lives.** Inside K4, replacing its opening binary sentence (recommended — the table *is* K4's content), or in §28.3's prose beneath the kernel with K4 pointing at it? The first makes the kernel longer by a table and shorter by four clauses; the second keeps the kernel's clause-only shape.
- **Q4 — Appendix C.3's kill-adjacent rows.** Three C.3 rows restate post-state and freeze facts (the discard row, the owning-yield row, the `Dispose()`-receiver row). Do they cite the kill table and shrink (recommended, and it is proposal 0094's cap doing the same work), or keep their current text?
- **Q5 — §35.4's property spelling at this revision.** This proposal restates §35.4 census-first using the *current* name; proposal 0095 renames it. If both land in one revision the restatement writes 0095's name directly and §4.2's bracketed note drops. Confirm the two graduate together (recommended — one §35.4 edit, not two)?

## 10. Graduation-train scope

The train lane executes from this section directly.

**Spec, in this order** (the kernel first, so downstream re-pointing has a target):

1. **§28.3 kernel** — K3.13 gains *target-announced*; K4's opening binary sentence is replaced by the §2.3 table (per Q3); K4.2 gains *site-announced*; K2.4 and K6.2 gain *type-announced*; K5 is restated per §3.2 and K5.1–K5.7 are re-cast as consequences; K5.7's §28.14 sentence and a new §30.12 pointer are added to K5's placement list.
2. **§28.3 prose** — *Move semantics for owned locals* (4 announcement occurrences, all target-announced), *The consume-site freeze* (its *explicit-`Dispose()` boundary* paragraph becomes a pointer to K5's single statement), the **axiom register**'s A2, A4, and A7 rows, the *Producers* table's two rows carrying type-announced language, *Deferred-activation callees* and *Returning a deferred-activation frame* (type-announced), the *Choosing a disposal form* recap table (1 site).
3. **Downstream spec sections**, each re-pointed to a named sense: §17.13 (3), §22.3 (2), §23.3 (2), §28.7, §28.8.1 (1), §28.9 (2, out-of-family — untouched), §28.11, §28.14 (5, incl. K5.7's derivation), §29.2 (7), §30.5 (6, incl. the Pattern B sentence of §1.1), §30.12 (1, plus the new K5 tag), §31.1 (3), §35.4 (the §4.2 restatement), §35.5 (2).
4. **Appendix A** — the two indented commentary lines carrying "announced" (grammar notes at the `MoveExpr`/field-path commentary).
5. **Appendix C** — C.3's three kill-adjacent rows and C.6's Pattern B row, per Q4.

**Rationale:** §3.10 (5 sites, 4 of them marker-announced respellings), §5.1, §5.10, §5.23, §5.26 (the kernel's recorded reasons — the axiom register's one-line reasons stay), §5.28 (out-of-family — untouched), §12.10 (the announcement-doctrine paragraphs, 6 sites).

**Stdlib:** §1 (2), §6.8, §6.12 (2, both marker-announced respellings), §4.19 (1).

**Guides:** Guide 01 §2 *The four ways to dispose* (the disposal table's discard row — target-announced; the trivial-dispose paragraph — out-of-family, untouched) and §9 *More rules you'll meet* (the owning-yield sentence and the discard sentence — both target-announced); Guide 04 §2 *A task is a resource you own* (the task-return sentence — type-announced; the abandonment paragraph — out-of-family; one marker-announced respelling) and §4 *Structure first* (one marker-announced respelling); Guide 05 §3 *Consuming iteration* (the static-iterator sentence — type-announced).

**Changelog:** one entry, categories per §6, naming the three senses, the kill table, the single freeze criterion, and the §35.4 restatement, with the "zero semantic change" claim stated and the §7 acceptance tests named as its evidence.

**Compiler:** none required. §5 records five diagnostic strings whose wording would read better under the named senses; that is an optional follow-up for the coordinator to file, not part of this train.

## 11. Interactions

- **Proposal 0094** (same basket) edits §28.3's *Factory methods* / *Bare-type-parameter returns* / iterable paragraphs into a **type-announced** table and caps Appendix C. Both proposals touch §28.3 and Appendix C.3/C.6; they must land in **one editing pass**, 0093 first (0094's table header uses 0093's names).
- **Proposal 0095** (same basket) renames §35.4's census and registers `defaultable`. Overlaps this proposal at §35.4 only — see Q5.
- **Proposal 0046** (the kernel consolidation) is this program's parent: the K1–K9 shape, the axiom register, and the "every rule is a principle, a derivation, or a recorded axiom" discipline are its. This proposal adds no rule and retires no axiom, so 0046's ledger is unchanged.
- **Proposal 0090** (the `move` unification, graduated v0.246) supplied the marker vocabulary the kill table's row 3 reads. Its rationale §3.10 record is where four of the eleven marker-announced respellings live.
- **A7's register row** carries "at an announced target" (target-announced) and is re-pointed; the axiom's content is untouched.
