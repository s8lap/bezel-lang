# Pin map — spec v0.344

Build-free join of the spec's §28 / §30.5 blocks, their kernel derivation tags, the rule corpus, the latest matrix runs and the gtest/lit sources. Numbers are counts of *things that would notice* if the clause or block changed meaning; "direct" means the case's own `rule:` quote holds verbatim inside a block that derives from the clause, "by section" means the case merely cites the section.

- Blocks parsed in §28 and §30.5: **113** (56 carry a `derives:`/`axiom` tag)
- Kernel clauses and axioms indexed: **80**
- Rule-corpus cases in §28/§30.5: **435** of 3343; matrix cells+routes (latest runs): **464**; test sources scanned: 6323
- Quote drift, §28/§30.5 cases: 193 of 435 `rule:` quotes hold verbatim (242 paraphrase, 0 too short to test). Whole corpus: 1099 of 3336.

## A. Kernel clauses and axioms: what pins each

| clause | tagged blocks | mentioning blocks | corpus direct (banked fail) | corpus by section | matrix cells (open) |
|---|---|---|---|---|---|
| **A1** | A1 | Retired. The prior axiom and its decision record are  | 0 | 2 | 0 (0) | 0 | 0 (0) |
| **A2** | A2 | Retired. The prior axiom and its decision record are  | 0 | 1 | 0 (0) | 0 | 0 (0) |
| **A3** | A3 | The two-container async design: Task<T> (class T) = s | 2 | 3 | 5 (0) | 14 | 2 (0) |
| **A4** | A4 | Accessor read faces are lend-only: property and index | 2 | 4 | 3 (0) | 191 | 0 (0) |
| **A5** | A5 | using var is a scope pin: non-reassignable, never mov | 5 | 6 | 9 (0) | 191 | 12 (0) |
| **A6** | A6 | A Task-family value -- Task, Task<T>, OwningTask<T>,  | 2 | 3 | 0 (0) | 14 | 4 (0) |
| **A7** | A7 | The transfer marker is not admitted in ??-operand or  | 4 | 5 | 8 (0) | 192 | 1 (0) |
| **A8** | A8 | Retired. The prior axiom and its decision record are  | 0 | 2 | 0 (0) | 0 | 0 (0) |
| **A9** | A9 | The two collection families: borrowing by default, ow | 2 | 3 | 24 (0) | 13 | 0 (0) |
| **A10** | A10 | Stdlib certification grants: CancellationToken's sou | 4 | 5 | 11 (1) | 194 | 1 (0) |
| **A11** | A11 | The §28.14 bounding-object resolution forms (1-5) an | 4 | 6 | 2 (0) | 184 | 0 (0) |
| **A12** | A12 | Pattern A chaining's disposal-timing dispensation fo | 2 | 3 | 5 (0) | 14 | 4 (0) |
| **A13** | A13 | Retired. The prior axiom and its decision record are | 0 | 1 | 0 (0) | 0 | 0 (0) |
| **A14** | A14 | No owned generic type arguments; owning semantics li | 1 | 2 | 5 (0) | 17 | 1 (0) |
| **A15** | A15 | The self-returning enumerator: a class implementing  | 3 | 5 | 0 (0) | 156 | 4 (0) |
| **K1** Value classification | 7 | 22 | 12 (0) | 200 | 7 (1) |
| **K1.1** 1. owned -- the holder carries the disposal obligation: disp | 0 | 0 | 0 (0) | 0 | 1 (0) |
| **K1.2** 2. borrowed -- no obligation; validity is bounded by the bor | 0 | 0 | 0 (0) | 0 | 0 (0) |
| **K1.3** 3. moved-from -- the obligation has transferred out; the bin | 0 | 0 | 1 (0) | 0 | 0 (0) |
| **K1.4** 4. maybe-consumed -- the K7 join of owned with moved-from or | 0 | 0 | 0 (0) | 0 | 0 (0) |
| **K1.5** 5. Atoms. Fresh productions are owned: new, reference-typed  | 1 | 6 | 3 (0) | 35 | 1 (0) |
| **K1.6** 6. The runtime-managed exemption; the trivial-dispose ceremo | 5 | 12 | 14 (0) | 184 | 20 (0) |
| **K1.7** 7. One holder. An obligation has exactly one holder at a tim | 5 | 6 | 10 (0) | 206 | 0 (0) |
| **K2** The source algebra | 4 | 13 | 2 (0) | 187 | 9 (0) |
| **K2.1** 1. Weakest source bounds. A borrow is valid only where every | 2 | 7 | 5 (0) | 170 | 1 (0) |
| **K2.2** 2. Union under composition. Joins, ?? and arm meets, transit | 3 | 7 | 9 (1) | 147 | 0 (0) |
| **K2.3** 3. Escape must be bounded. A borrow may not reach a holder - | 7 | 13 | 46 (3) | 215 | 5 (0) |
| **K2.4** 4. Signature-level re-establishment. A callee's declared sig | 13 | 20 | 51 (3) | 135 | 2 (0) |
| **K3** Discharge and consumption | 2 | 5 | 3 (0) | 46 | 1 (0) |
| **K3.1** store into an owned field or owning-collection slot, includi | 0 | 7 | 0 (0) | 0 | 11 (0) |
| **K3.2** return under an owned return type, or under an unmarked mint | 1 | 6 | 4 (0) | 152 | 10 (0) |
| **K3.3** an owned argument (§22.3) | move x at the call site | moved- | 0 | 3 | 0 (0) | 0 | 4 (0) |
| **K3.4** a marked binding or assignment transfer -- var y := move x;, | 1 | 10 | 0 (0) | 14 | 3 (0) |
| **K3.5** the operand of a Pattern B await (§30.5) | none -- the keywo | 1 | 3 | 3 (0) | 16 | 7 (0) |
| **K3.6** the owned parameter-or-plain-local source of a Pattern A cha | 0 | 4 | 0 (0) | 0 | 0 (0) |
| **K3.7** the operand of yield return in an owning iterator (§29.2) |  | 0 | 3 | 0 (0) | 0 | 4 (0) |
| **K3.8** the operand of throw (§31.1) | none -- the keyword announces | 0 | 2 | 0 (0) | 0 | 6 (0) |
| **K3.9** explicit disposal of an owned binding -- the marker-free dis | 1 | 8 | 3 (0) | 35 | 11 (0) |
| **K3.11** reassignment of an owned location -- a consume of the displa | 4 | 11 | 5 (0) | 162 | 10 (1) |
| **K3.12** for move-only values (§15.3), any store into an owning locat | 0 | 6 | 0 (0) | 0 | 6 (0) |
| **K3.13** 13. Marker economy. A consume is spelled by the transfer mar | 0 | 11 | 0 (0) | 0 | 5 (0) |
| **K3.14** 14. Affinity. An owned value that reaches its scope's end un | 1 | 8 | 0 (0) | 156 | 4 (0) |
| **K3.15** 15. Edge parity. The exception edge performs the same implic | 3 | 6 | 5 (0) | 151 | 19 (0) |
| **K4** Kills, site-announcement, and the backstop posture | 0 | 3 | 0 (0) | 0 | 0 (0) |
| **K4.1** 1. The backstop. Disposal is idempotent and flag-setting; in | 2 | 16 | 3 (0) | 191 | 33 (1) |
| **K4.2** 2. Site-announced kills. A kill that is both keyword-visible | 2 | 9 | 4 (0) | 27 | 10 (0) |
| **K4.3** 3. Recorded trust, or nothing. Where an admission's temporal | 4 | 12 | 31 (2) | 219 | 28 (0) |
| **K5** The freeze | 0 | 10 | 0 (0) | 0 | 21 (0) |
| **K5.1** 1. the family-1 transfers and owning-iterator yield return ( | 0 | 1 | 0 (0) | 0 | 3 (0) |
| **K5.2** 2. drop-on-overwrite of a binding (row K3.11) is frozen alth | 0 | 3 | 0 (0) | 0 | 2 (0) |
| **K5.3** 3. the K4.2 site-announced kills (rows K3.5, K3.8, K3.9) are | 1 | 4 | 0 (0) | 156 | 4 (0) |
| **K5.4** 4. a Pattern A chaining return await x (row K3.6) is unfroze | 0 | 2 | 0 (0) | 0 | 0 (0) |
| **K5.5** 5. the freeze is body-local by construction -- the clause bo | 2 | 4 | 5 (0) | 151 | 13 (0) |
| **K5.6** 6. fields and collection slots are not frozen -- the clause  | 0 | 7 | 0 (0) | 0 | 21 (0) |
| **K5.7** 7. a §28.14 storage admission freezes the admitted captures  | 2 | 5 | 2 (0) | 28 | 3 (0) |
| **K5.8** 8. a §30.12 guard's site-announced kills are frozen while bo | 0 | 2 | 0 (0) | 0 | 0 (0) |
| **K6** Frames are values | 2 | 8 | 2 (0) | 173 | 4 (0) |
| **K6.1** 1. The bound. Carried sources must outlive the frame's bound | 1 | 4 | 0 (0) | 30 | 8 (0) |
| **K6.2** 2. Announcement by declared type. Frame-carrying classificat | 5 | 9 | 3 (0) | 183 | 0 (0) |
| **K6.3** 3. Containers: calls mint, storage lends. A call whose decla | 6 | 8 | 9 (0) | 177 | 5 (0) |
| **K6.4** 4. Frames versus the freeze. Frame-held borrows live in the  | 0 | 1 | 0 (0) | 0 | 8 (0) |
| **K6.5** 5. Interaction with K8. A frame sourced by an unbound tempor | 0 | 1 | 0 (0) | 0 | 1 (0) |
| **K7** The join rule | 0 | 5 | 0 (0) | 0 | 7 (0) |
| **K7.1** 1. owned ⊓ owned = owned, obligation carried forward (for ex | 0 | 3 | 0 (0) | 0 | 1 (0) |
| **K7.2** 2. borrowed ⊓ borrowed = borrowed, carrying the union of the | 0 | 1 | 0 (0) | 0 | 1 (0) |
| **K7.3** 3. owned ⊓ borrowed = rejected -- no single state is correct | 0 | 3 | 0 (0) | 0 | 1 (0) |
| **K7.4** 4. owned ⊓ moved-from and owned ⊓ explicitly-disposed = mayb | 1 | 6 | 1 (0) | 11 | 10 (0) |
| **K7.5** 5. null-literal and bottom-typed (throw) arms absorb to the  | 0 | 2 | 0 (0) | 0 | 3 (0) |
| **K8** The full-expression boundary | 1 | 10 | 0 (0) | 156 | 6 (0) |
| **K8.1** 1. Unbound owned temporaries are disposed at their full expr | 1 | 3 | 22 (2) | 134 | 13 (0) |
| **K8.2** 2. Conditions and guards are per-evaluation boundaries: temp | 0 | 1 | 0 (0) | 0 | 2 (0) |
| **K8.3** 3. A case scrutinee's boundary is the whole construct -- pat | 0 | 2 | 0 (0) | 0 | 8 (0) |
| **K8.4** 4. A temporary is an admissible borrow source only within it | 0 | 5 | 0 (0) | 0 | 6 (0) |
| **K8.5** 5. A binding atom (§17.15) is the boundary for its own bindi | 0 | 3 | 0 (0) | 0 | 4 (0) |
| **K9** Fields are object state, not scope state | 0 | 3 | 0 (0) | 0 | 13 (0) |
| **K9.1** 1. every plain read of an owned field lends (K1.5); the only | 1 | 3 | 3 (0) | 35 | 7 (0) |
| **K9.2** 2. an owned location never holds a hole: a non-nullable owne | 0 | 5 | 0 (0) | 0 | 20 (2) |
| **K9.3** 3. a constructor's move is governed by definite assignment b | 0 | 2 | 0 (0) | 0 | 7 (1) |
| **K9.4** 4. static owned storage -- a static owned field, and the slo | 0 | 4 | 0 (0) | 0 | 16 (0) |
| **K9.5** 5. drop-on-overwrite (K3.11) is RHS-first, so a throwing RHS | 0 | 3 | 0 (0) | 0 | 9 (1) |

**Clauses with no direct corpus case and no matrix cell:** A1, A2, A8, A13, K1.2, K1.4, K3.6, K4, K5.4, K5.8

**Clauses pinned only by matrix cells (no direct corpus case):** A6, A15, K1.1, K3.1, K3.3, K3.4, K3.7, K3.8, K3.12, K3.13, K3.14, K5, K5.1, K5.2, K5.3, K5.6, K6.1, K6.4, K6.5, K7, K7.1, K7.2, K7.3, K7.5, K8, K8.2, K8.3, K8.4, K8.5, K9, K9.2, K9.3, K9.4, K9.5

## B. Blocks of §28 / §30.5 with nothing pinning them directly

A block counts as unpinned when no corpus case quotes it verbatim; "section" columns show the coarser evidence that exists.

| § | block | kind | tags | words | corpus direct | corpus §-only | matrix cells § | tests § |
|---|---|---|---|---|---|---|---|---|
| 28.1 | Where the `owned` modifier can appear (l.6172) | heading | — | 426 | 0 | 10 | 34 | 84 |
| 28.2 | The `IDisposable` contract (l.6205) | heading | — | 489 | 0 | 12 | 59 | 194 |
| 28.2 | Scoped-callback backstop (l.6242) | bold | — | 47 | 0 | 12 | 59 | 194 |
| 28.3 | Local variables, ownership, and borrows (l.6245) | heading | — | 108 | 0 | 156 | 200 | 1021 |
| 28.3 | Map of this section (l.6254) | bold | — | 234 | 0 | 156 | 200 | 1021 |
| 28.3 | Glossary (l.6270) | bold | — | 249 | 0 | 156 | 200 | 1021 |
| 28.3.1 | The ownership kernel (K1–K9) (l.6281) | heading | — | 66 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K2 — The source algebra (l.6305) | bold | — | 330 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K3 — Discharge and consumption (l.6319) | bold | — | 47 | 0 | 156 | 200 | 1021 |
| 28.3.1 | The consume table (l.6322) | bold | — | 1146 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K4 — Kills, site-announcement, and the backstop posture (l.6348) | bold | — | 589 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K5 — The freeze (l.6363) | bold | A11 | 836 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K6 — Frames are values (l.6379) | bold | A15 | 490 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K7 — The join rule (l.6388) | bold | — | 157 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K8 — The full-expression boundary (l.6399) | bold | — | 340 | 0 | 156 | 200 | 1021 |
| 28.3.1 | K9 — Fields are object state, not scope state (l.6408) | bold | — | 391 | 0 | 156 | 200 | 1021 |
| 28.3.3 | Ownership state of locals (l.6440) | heading | — | 273 | 0 | 156 | 200 | 1021 |
| 28.3.3 | A runtime-managed RHS into a classified local (l.6457) | bold | K1.6, K1.7, K3.11 | 159 | 0 | 156 | 200 | 1021 |
| 28.3.3 | Adoption (l.6459) | bold | K1.7, K3.11 | 1015 | 0 | 156 | 200 | 1021 |
| 28.3.4 | Producers (l.6496) | heading | A7 | 1802 | 0 | 156 | 200 | 1021 |
| 28.3.4 | Consuming positions for an owned-local read (l.6531) | bold | A7 | 284 | 0 | 156 | 200 | 1021 |
| 28.3.5 | The announcement table (l.6542) | heading | A15, K2.4, K6.2, K6.3 | 1697 | 0 | 156 | 200 | 1021 |
| 28.3.5 | The sources an argument contributes (derives: [K2.2](#sec-kernel-k2) + (l.6566) | bold | K2.2, K2.4 | 362 | 0 | 156 | 200 | 1021 |
| 28.3.6 | Calls mint for the mint-announcing families (derives: [K6.2](#sec-kern (l.6589) | bold | A15, A4, K2.4, K6.2, K6.3 | 1071 | 0 | 156 | 200 | 1021 |
| 28.3.6 | `Task`-family substitutions are excluded from bare-parameter function  (l.6630) | bold | K1, K2.4, K6.3 | 291 | 0 | 156 | 200 | 1021 |
| 28.3.6 | Iterator and iterable returns (derives: [K6.2](#sec-kernel-k6) + [K6.3 (l.6652) | bold | K6.2, K6.3 | 147 | 0 | 156 | 200 | 1021 |
| 28.3.7 | Move semantics for owned locals (l.6654) | heading | A5 | 611 | 0 | 156 | 200 | 1021 |
| 28.3.7 | Captured owned bindings (derives: [K2](#sec-kernel-k2) + [K5.5](#sec-k (l.6697) | bold | K2, K5.5 | 143 | 0 | 156 | 200 | 1021 |
| 28.3.9 | Span-carried borrows and the site-announced kills (l.6755) | bold | K4.1, K5.3 | 135 | 0 | 156 | 200 | 1021 |
| 28.3.9 | Unborrowed reassignment stays legal (l.6757) | bold | — | 78 | 0 | 156 | 200 | 1021 |
| 28.3.9 | Scope (l.6759) | bold | — | 330 | 0 | 156 | 200 | 1021 |
| 28.3.10 | Owned-local discharge requirement (l.6766) | heading | — | 74 | 0 | 156 | 200 | 1021 |
| 28.3.10 | Auto-drop (affine) (l.6770) | bold | K3.14 | 149 | 0 | 156 | 200 | 1021 |
| 28.3.11 | Unbound owned temporaries (l.6796) | heading | K1, K3.15, K8 | 244 | 0 | 156 | 200 | 1021 |
| 28.3.11 | The scoped-bind pinned slot ([§30.5](#sec-the-scoped-await-bind)) (l.6802) | bold | — | 102 | 0 | 156 | 200 | 1021 |
| 28.3.11 | The full expression boundary (l.6804) | bold | — | 95 | 0 | 156 | 200 | 1021 |
| 28.3.11 | `case` scrutinees extend the boundary (l.6806) | bold | — | 90 | 0 | 156 | 200 | 1021 |
| 28.3.11 | Binding-atom bindings are named temporaries (l.6808) | bold | — | 484 | 0 | 156 | 200 | 1021 |
| 28.3.11 | A callee that stores a plain argument (l.6816) | bold | — | 242 | 0 | 156 | 200 | 1021 |
| 28.3.11 | Deferred-activation callees (derives: [K2.4](#sec-kernel-k2) + [K6](#s (l.6820) | bold | K2.4, K6 | 156 | 0 | 156 | 200 | 1021 |
| 28.3.11 | The capture-set contribution (l.6822) | bold | — | 276 | 0 | 156 | 200 | 1021 |
| 28.3.11 | Temporaries as sources (l.6824) | bold | — | 230 | 0 | 156 | 200 | 1021 |
| 28.3.12 | Diagnostics (l.6830) | heading | A5 | 71 | 0 | 156 | 200 | 1021 |
| 28.3.13 | Choosing a disposal form (recap) (l.6845) | heading | — | 423 | 0 | 156 | 200 | 1021 |
| 28.4 | The destructor body runs on disposal, never on construction unwind — h (l.6897) | bold | — | 294 | 0 | 8 | 31 | 79 |
| 28.4 | Destructors should not throw; behavior is defined when one does (l.6899) | bold | — | 306 | 0 | 8 | 31 | 79 |
| 28.7 | Overwrite while borrowed (l.7033) | bold | — | 84 | 0 | 11 | 30 | 85 |
| 28.7 | Ordering and exceptions (l.7035) | bold | — | 365 | 0 | 11 | 30 | 85 |
| 28.8 | Ownership transfer (l.7038) | heading | — | 355 | 0 | 4 | 3 | 108 |
| 28.8.1 | A raise from the consuming call does not hand the obligation back (l.7137) | bold | — | 248 | 0 | 30 | 56 | 105 |
| 28.9 | Runtime-managed types (l.7140) | heading | — | 848 | 0 | 20 | 50 | 612 |
| 28.10 | `using var` (l.7222) | heading | A5, A7 | 732 | 0 | 14 | 30 | 139 |
| 28.10 | Adopting a named owned binding (derives: [K3.4](#sec-kernel-k3)) (l.7243) | bold | A5, A6, K3.4 | 491 | 0 | 14 | 30 | 139 |
| 28.10 | The anonymous binding (l.7256) | bold | A6 | 121 | 0 | 14 | 30 | 139 |
| 28.10 | The compiler-introduced slot (l.7258) | bold | K2 | 609 | 0 | 14 | 30 | 139 |
| 28.12 | The `DisposeAsync` awaited is the run-time object's (l.7490) | bold | — | 234 | 0 | 14 | 29 | 22 |
| 28.14 | Scoped callback parameters: storage-bounded capture (l.7521) | heading | A11, K2.3, K5.7 | 153 | 0 | 30 | 32 | 101 |
| 28.14 | Quiescence (l.7545) | bold | — | 101 | 0 | 30 | 32 | 101 |
| 28.14 | The call-bounded default (derives: [K2.4](#sec-kernel-k2) — a plain si (l.7547) | bold | K2.4 | 407 | 0 | 30 | 32 | 101 |
| 28.14 | Call-bounded × deferred activation (unified across the deferred-activa (l.7555) | bold | K3, K6.1, K6.2, K6.3 | 657 | 0 | 30 | 32 | 101 |
| 28.14 | v1 admission bounds (ratified; axiom [A11](#sec-axiom-register)) (l.7572) | bold | A11 | 613 | 0 | 30 | 32 | 101 |
| 28.14 | Interaction with the type system (l.7582) | bold | — | 94 | 0 | 30 | 32 | 101 |
| 28.14 | Relation to §30.11 (l.7584) | bold | A10 | 82 | 0 | 30 | 32 | 101 |
| 30.5 | Copyable value type, `string`, or runtime-managed result: (l.8101) | bold | — | 178 | 0 | 19 | 67 | 108 |
| 30.5 | `Task<T>` for class or interface T (Pattern A) (l.8110) | bold | K1, K2 | 856 | 0 | 19 | 67 | 108 |
| 30.5 | The scoped await-bind (l.8144) | bold | K2.1 | 413 | 0 | 19 | 67 | 108 |
| 30.5 | Which declarations qualify (l.8155) | bold | — | 286 | 0 | 19 | 67 | 108 |
| 30.5 | Error participation: (l.8159) | bold | — | 55 | 0 | 19 | 67 | 108 |

68 of 111 substantive blocks (≥40 words) have no directly-quoting corpus case.

## C. Per-section totals

| § | blocks | corpus cases (fail-banked) | verbatim quotes | matrix cells citing | gtest/lit sources citing |
|---|---|---|---|---|---|
| 28.1 | 2 | 10 (0) | 1 | 34 | 84 |
| 28.2 | 3 | 12 (1) | 1 | 59 | 194 |
| 28.3 | 60 | 156 (9) | 93 | 200 | 1021 |
| 28.4 | 3 | 8 (0) | 4 | 31 | 79 |
| 28.5 | 2 | 22 (0) | 8 | 24 | 116 |
| 28.6 | 3 | 38 (2) | 10 | 34 | 157 |
| 28.7 | 4 | 11 (0) | 3 | 30 | 85 |
| 28.8 | 1 | 4 (0) | 0 | 3 | 108 |
| 28.8.1 | 3 | 30 (0) | 16 | 56 | 105 |
| 28.9 | 2 | 20 (1) | 2 | 50 | 612 |
| 28.10 | 4 | 14 (2) | 0 | 30 | 139 |
| 28.11 | 3 | 37 (0) | 30 | 54 | 235 |
| 28.12 | 3 | 14 (5) | 9 | 29 | 22 |
| 28.13 | 1 | 8 (0) | 1 | 4 | 46 |
| 28.14 | 10 | 30 (4) | 6 | 32 | 101 |
| 30.5 | 9 | 19 (1) | 9 | 67 | 108 |
