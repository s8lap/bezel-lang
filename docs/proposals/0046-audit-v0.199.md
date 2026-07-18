# 0046 acceptance audit — v0.199

**Date:** 2026-07-18. **Status:** Audit of the v0.199 landing (0046 trains T2–T6); supersedes at close per Q7.

This is the committed audit artifact proposal 0046 §5.1 specifies: every heading-to-heading block of the rewritten text, with its classification and — for DERIVED — the one-line derivation *as the text now carries it*. Sentence-level coverage is the audit's granularity; rule-level tags are the text's. The acceptance test audited: *every normative sentence in §28, §30.5, and §30.1's five ownership paragraphs is (a) inside the kernel subsection, (b) inside a rule carrying a derivation tag, or (c) inside a rule carrying an axiom tag.*

Conventions. Block granularity is h4 / bold-head, as the sections title themselves. Classifications: **KERNEL** (the kernel text itself), **KERNEL-ABSORBED** (the rule statement collapsed to a kernel cross-reference; examples and remedies stay), **DERIVED** (statement kept, derivation tag at its head or trailing parenthetical), **AXIOM** (statement kept, register id named), **EXAMPLES-ONLY** (worked examples, recaps, or summary surface with no free-standing rule), **OUT-OF-SCOPE** (no ownership-classification rule; reason given). A mixed block is classified by its primary character with the secondary named in the note. Derivations quoted in the third column are the tags present in the working tree — the audit adds none; where a block's covering tag lives in a neighbouring block it cites, the note says so ("tag at home").

## 1. The block table

### §28 preamble

| Block | Class | Note / derivation |
|---|---|---|
| §28 preamble (untitled) | EXAMPLES-ONLY | Overview surface; each clause cites its rule's home (runtime-managed/trivial-dispose → §28.9 = K1.6; the `Task`-discard routing → §17.13 = axiom A6, register home). |
| *Summary of disposal obligations by type kind* | EXAMPLES-ONLY | A table of citations into §28.5/§28.9/§28.10/§28.11/§15.3; no free-standing rule. |

### §28.1 Where the `owned` modifier can appear

| Block | Class | Note / derivation |
|---|---|---|
| §28.1 head (admissible / not-admissible positions) | DERIVED | Row 30: K1.6 + K1.7; the generic-argument absence is axiom A14 (the register names §28.1 as A14's home). Tag at home: the tagged statement is §28.5 *Restrictions* — "(derives: K1.6 + K1.7; the generic-argument position's absence is axiom A14)" — which this block cites (§28.5, §14.4). |
| *Type requirement* (v0.193/v0.196) | DERIVED | K1.6 — markers admissible only where an obligation can exist, erased per instantiation where none does. Tag at home: the block states the reasoning by citation ("such values carry no disposal obligation to transfer (§28.9), the same reasoning as §28.5's field restriction"). |

### §28.2 The `IDisposable` contract

| Block | Class | Note / derivation |
|---|---|---|
| §28.2 head (idempotent, thread-safe generated `Dispose()`) | KERNEL-ABSORBED | Carries the kernel citation in-text: "the runtime backstop the ownership kernel records as K4.1". |
| *Use after disposal; checked disposal mode* | KERNEL-ABSORBED | K4.1's two modes; the straight-line warning is tagged DERIVED in-block: "(v0.184; derives: K4.2 + K7.4)". |
| *Scoped-callback backstop* (v0.175) | DERIVED | K4.1's backstop applied to the §28.14 admission. Tag at home: §28.14's quiescence block carries the K2.3 + K4.1 tags; this block is the checked-mode face of the same clause and cites §28.14. |

### §28.3 Local variables, ownership, and borrows

| Block | Class | Note / derivation |
|---|---|---|
| §28.3 head (the two reference kinds) | KERNEL-ABSORBED | K1's opening classification ("the two reference kinds of this section's opening" — K1's own words). |
| *The ownership kernel (K1–K9)* | KERNEL | The kernel itself. Nine principles, anchors `sec-kernel-k1`…`sec-kernel-k9`. |
| *The axiom register (A1–A14)* | AXIOM | The register itself. Fourteen rows, anchor `sec-axiom-register`; reasons in rationale §5.26. |
| *Ownership state of locals* | KERNEL-ABSORBED | K1 states + K7 joins, cited in-text; reassignment state-match tagged DERIVED: "(derives: K1.7 + K3.11; v0.180)"; the two equalization patterns stay as worked examples. |
| *Producers* (the table) | KERNEL-ABSORBED | K1.5's worked enumeration, per its own head. Rule-carrying rows tagged in place: `Task`-family row "(v0.190; derives: K6.3 — the family keying is axiom A2)"; bare-type-parameter row "(v0.194; derives: K2.4)"; `??` and arm-meet rows K7 + axiom A7; static-property row K2's always-live source; indexer row's transit residual per §28.11 (K2.3/K4.3). |
| *Consuming positions for an owned-local read* | KERNEL-ABSORBED | "are K3's enumeration (K3.1–K3.12); … K3.13's announcement rule" — in-text. |
| *Factory methods and sourceless returns* | AXIOM | A1, tagged at head; the two-sides-of-one-boundary note carries row 13's derivation in-block: "(Derivation — K2.4 + A1: …)". |
| *Task-family calls produce owned frames* | DERIVED | "(v0.190; derives: K6.3 — the family keying is axiom A2)". |
| *Bare-type-parameter returns* (+ *Caller side*, *Callee side*) | DERIVED | "(v0.194; derives: K2.4; empty source set → axiom A1)". |
| *Iterator methods are exempt from this warning* | DERIVED | "(derives: K6.2 — the declared iterable interface is the announcement)". |
| *Move semantics for owned locals* | KERNEL-ABSORBED | K3.13 (reads in K3's positions transfer; unmarked reads never consume), cited in-text; `Wrap`/`Wrap2` and loop-accumulator examples stay. |
| *Captured owned bindings* | DERIVED | "(v0.184; derives: K2 + K5.5)". |
| *Borrow lifetime rules* | DERIVED | K2.1/K2.3 instances, cited at head; the return prohibition "(derives: K2.4 + A1 — only sources every caller can re-establish…)"; the field-store admission is the K2.3 + K4.3 recorded-trust shape. |
| *The consume-site freeze* (v0.180) | KERNEL-ABSORBED | K5 head; the explicit-`Dispose()` boundary is "K5's criterion (*unfrozen ⟺ announced ∧ flag-backstopped*)" with K4.2, in-text. |
| *Owned-local discharge requirement* | KERNEL-ABSORBED | Auto-drop "(derives: K3.14)"; must-use warning "K3.14's lint"; exception-path discharge "(derives: K3.15 — edge parity)". |
| *Unbound owned temporaries* | DERIVED | "(derives: K1 + K3.15 + K8)". |
| *The full expression boundary* (v0.193) | KERNEL-ABSORBED | "is K8's full expression", in-text. |
| *`case` scrutinees extend the boundary* (v0.193) | DERIVED | "the boundary is K8.3's", in-text. |
| *Deferred-activation callees* | DERIVED | "(v0.179; derives: K2.4 + K6)"; the temporary-sourced reject posture is K6.5/K8.4's (row 28). |
| *Returning a deferred-activation frame* | DERIVED | "(v0.184; derives: K6.2 + K2.4)". |
| *Diagnostics* | EXAMPLES-ONLY | Compiler-surface message texts, kept verbatim with K-references per message (e.g. the `using var` move message cites axiom A5). |
| *Choosing a disposal form* (recap, v0.184) | EXAMPLES-ONLY | Doctrine recap in table form; no free-standing rule. |

### §28.4 The `destructor`

| Block | Class | Note / derivation |
|---|---|---|
| §28.4 head (generated `Dispose()` steps; ordering; reclamation) | OUT-OF-SCOPE | Disposal-execution mechanics — the *content* of K4.1's backstop machinery and K9's cascade, which the kernel cites (K9: "discharged by the destructor cascade"); no ownership-classification rule. No §4 mapping-table row, by design. |
| *Destructors should not throw; defined behavior when one does* | OUT-OF-SCOPE | Execution semantics of a throwing disposer (§31.2 supersession discipline); no classification content. |

### §28.5 Owned fields

| Block | Class | Note / derivation |
|---|---|---|
| §28.5 head (auto-dispose with the object; leak prevention pointer) | KERNEL-ABSORBED | K9 (fields are object state; destructor-cascade discharge); the leak-prevention clause is a pointer into §28.6's tagged rules. Tags carried by the adjacent *Restrictions* block and §28.6/§28.8.1 statements. |
| *Restrictions* | DERIVED | "(derives: K1.6 + K1.7; the generic-argument position's absence is axiom A14)". |
| *Generic class fields* | DERIVED | "(derives: K1.6, checked per instantiation)". |

### §28.6 Leak prevention

| Block | Class | Note / derivation |
|---|---|---|
| §28.6 head (the enforcement bullet list + examples) | DERIVED | Bare-read lends "(K9.1)"; move/fresh through non-`owned` return "(derives: K1.7 + K2.3)"; fresh-into-borrow-field "(derives: K1.7 + K2.3)"; getter bullet "(axiom A4)". **One sentence deliberately untagged** — the categorical cross-object store prohibition — exceptions log (a), #2043. |
| *Who may call `Dispose()`* (v0.184) | DERIVED | "(derives: K3.10)". |
| *The transit exception* | AXIOM | "(axiom A8)", tagged at head; K4.1-backstopped per the register row. |
| *Borrowed owning collections* | DERIVED | Instance of the K3.10 receiver rule by its own citation ("the general receiver rule above at its most consequential instance"); the borrow surface follows from K1's container-vs-element obligation split (row 34) — the split is now tagged at Appendix C.3's restating bullet; no literal in-block K tag (covered by citation). |
| *Methods returning `this`* | DERIVED | "(K1.5's calls-lend default)". |

### §28.7 Reassigning owned fields and locals

| Block | Class | Note / derivation |
|---|---|---|
| *Assignment … disposes the displaced value* (v0.161) | DERIVED | "(derives: K3.11)"; the move-only whole-value carve is named in-block as "axiom A13". |
| *Overwrite while borrowed* (v0.180) | DERIVED | "(§28.3 consume-site freeze; K5.2)" for bindings; fields/slots not frozen per K5.6. |
| *Ordering and exceptions* | DERIVED | "(K9.5)" — RHS-first, no exception-unsafe window. |
| *Moved-from and null* | DERIVED | The no-disposal condition of K3.11 (live-value check) enabling K9.2's restoration idiom; stated by citation of §28.8.1/§28.10 — no in-block K tag (covered by the K3.11/K9.2 tags it composes). |

### §28.8 Ownership transfer

| Block | Class | Note / derivation |
|---|---|---|
| §28.8 (whole section) | DERIVED | Inline-tagged throughout: `owned` parameter "(K3.3)", `owned` return "(K3.2)", move-out "(K9.1–K9.2)", `new` produces owned "(K1.5)" with auto-drop "(K3.14)", boxing exemption "(K1.6)". |

### §28.8.1 The `move` expression

| Block | Class | Note / derivation |
|---|---|---|
| §28.8.1 head | KERNEL-ABSORBED | "`move x.f` is the one owned read of an owned field (K9.1)"; static ineligibility "(K9.4)". |
| *The field-restoration rule* | KERNEL-ABSORBED | "The rule is K9.2's restoration rule". |
| *Exception paths are not exempt* | DERIVED | "K9.2's path set includes exception edges"; the non-throwable classification "(derives: K9.2 + K3.11)". |
| *Loops and conditional control flow* | EXAMPLES-ONLY | Worked path-analysis examples under K9.2. |
| *Constructors and destructors* | DERIVED | "Both bodies are K9.3's cases" (definite assignment; teardown exemption). |
| *Interaction with `using var`* | EXAMPLES-ONLY | Idioms; the A5 pin interplay by citation. |
| *Detaching via a nullable field* | EXAMPLES-ONLY | Remedy idiom (null as restoration). |
| *Interaction with overload resolution and parameter passing* | EXAMPLES-ONLY | The argument-position `WrongOrder` shape — a worked K9.2 exception-edge instance. |

### §28.9 Runtime-managed types

| Block | Class | Note / derivation |
|---|---|---|
| §28.9 head (the exemption list) | KERNEL-ABSORBED | "(K1.6's runtime-managed exemption)", in-text. |
| *Trivial-dispose classes* | DERIVED | "(derives: K1.6 — empty obligation content)". |
| *Trivial-dispose for generic class instantiations* | DERIVED | "(derives: K1.6, computed per instantiation)". |
| *Trivial-dispose classification is part of the API surface* | OUT-OF-SCOPE | API-evolution / versioning guidance; no checker rule. |

### §28.10 `using var`

| Block | Class | Note / derivation |
|---|---|---|
| §28.10 head (the pin) | AXIOM | "`using var` is a scope pin (axiom A5)". |
| *Adopting a named owned binding* (v0.180) | DERIVED | "(v0.180; derives: K3.4)". |
| *`using var` initialized from a borrow* | DERIVED | "(derives: K2 — no obligation can be assumed from a borrow)". |

### §28.11 Collection element ownership

| Block | Class | Note / derivation |
|---|---|---|
| §28.11 head (the two families) | AXIOM | "two families of collection types (axiom A9)". |
| *The element slot policy* | DERIVED | "(derives: K1 per slot + K2.3/K4.3 for stored borrows)"; per-collection clauses declared presentations of the table. |
| *Borrowing collections* | DERIVED | The store rule "(v0.180; derives: K2.3)" with the K4.3-recorded trust residual and K4.1 backstop named in-block; remainder is the slot table's presentation. |
| *Owning collections* | DERIVED | Presentation of the owned-slot column; indexer-set drop-on-overwrite cites K3.11. |
| *`move` on a collection slot* (+ *Indexer setter on a moved-from slot*) | DERIVED | "The field-restoration rule applies (K9.2, with per-slot tracking)". |
| *Compatibility between borrowing and owning forms* | DERIVED | A9's consequence (separate named types, no implicit conversion; shared borrowing-iteration interfaces); covered by the A9-tagged head. |

### §28.12 Asynchronous disposal

| Block | Class | Note / derivation |
|---|---|---|
| §28.12 head (two-phase lifecycle; `await using`) | OUT-OF-SCOPE | Async-cleanup execution mechanics (logical vs physical disposal); no ownership-classification rule; no §4 row, by design. |
| *Implicit discharge is synchronous* (v0.184) | OUT-OF-SCOPE | Selects the cleanup *form*, not the obligation; its enumeration of the implicit forms is K3.14/K3.15, K3.11, K9's cascade, and K6's frame teardown — now recorded at the Appendix C.3 bullet that restates it. |

### §28.13 Shared ownership: `Shared<T>` and `Weak<T>`

| Block | Class | Note / derivation |
|---|---|---|
| §28.13 (whole section) | OUT-OF-SCOPE | Compiler-known stdlib type. The section's own first bullet states the audit-relevant fact: handles are ordinary owned class instances and "every rule of this chapter applies to a handle unchanged" — the kernel classifies handles; the count/last-handle-disposes contract is type-internal. No §4 row, by design. |

### §28.14 Scoped callback parameters

| Block | Class | Note / derivation |
|---|---|---|
| §28.14 head | DERIVED | "(derives: K2.3 + K5.7, under axiom A11's forms)". |
| *The `scoped` modifier* (two-sided contract; bounding forms 1–5; admission tests) | AXIOM | Bounding-object resolution "(axiom A11)", tagged at the caller-side bullet; callee-side rights are the same admission's other face. |
| — the admission freeze paragraph | DERIVED | "(derives: K5.7)" — deliberately stronger than the consume-site rule (announced kills covered). |
| *The registration-lifetime residual* (v0.180) | DERIVED | "the same recorded residual-trust shape as the quiescence rule (K2.3 + K4.1)" — the composition-gate seam fix; exceptions log (d). |
| *Quiescence* | DERIVED | "the recorded residual of K2.3's admission posture … with K4.1's backstop as the named safety net" — the seam fix; exceptions log (d). |
| *The call-bounded default* (v0.177) + caller side | DERIVED | "(derives: K2.4 — a plain signature carries no capture set, so no retention can be announced)". |
| *The stdlib retention register* | AXIOM | The A10-shape certification surface (named stdlib positions); entries state their homes (§30.11 legacy admission, §30.14's spawn positions under §30's own rules). |
| *The method-reference formation bar* (v0.187) | DERIVED | "(protects the position-keyed register admissions — axioms A10/A11)". |
| *Call-bounded × deferred activation* (v0.179/v0.196) | DERIVED | Frame residence "(derives: K6.1 + K6.2)"; the synchronous-callee invocation rejection "(derives: K6.3 + K3 — the minted task inherits the call bound, and a non-`async` body has no consuming position for it)". |
| *Class- and interface-typed `scoped` parameters* (v0.180) | DERIVED | The passive-borrow instance of the section's admission (proposal §2.3: covered by K2 + K5 + A11 exactly as the callback form); covered by the head's tag — no separate in-block tag. |
| *Known v1 limitation* (#1427) | OUT-OF-SCOPE | Recorded deferral (the missing unbounded tier); non-normative. |
| *v1 admission bounds* (v0.180) | AXIOM | "(#1421 ratified; axiom A11)". |
| *Interaction with the type system* | OUT-OF-SCOPE | Function-type identity (§27.1's rule) and the FFI boundary's documented-trust contract (§32.10) — interactions 8 and 11 of the proposal. |
| *Relation to §30.11* | AXIOM | A10's legacy completion-source admission, stated as a derived instance of this section. |

### §30.5 Result lifetime and task disposal

| Block | Class | Note / derivation |
|---|---|---|
| §30.5 head | DERIVED | Container obligation per §28.3's `Task`-family production rule (K6.3), cited in-text; the pattern split is the block below. |
| *Producer-side return-type rules* | AXIOM | Headed "(axiom A3 — the two-container design …)"; the three Pattern A result-slot kinds tagged in place: fresh "(derives: K1)", chained "(derives: K2 + K6 …)" with the `using var`-source timing dispensation "(axiom A12)", borrowed-source "(v0.184; derives: K2 + K4.3 …)" with the residual's K4.1-backstop note. |
| *Consumer-side rules*: value / `string` / runtime-managed result | DERIVED | The no-obligation case (K1.6); the unbound-temporary operand consume stated by citation of §28.3 (tag at home: *Unbound owned temporaries*, K1 + K3.15 + K8). |
| Pattern A (`Task<T>` for class T) | DERIVED | "(derives: K2 — the admission is §28.3's ordinary borrow machinery applied to the awaited operand; v0.182)", with K2.1/K2.2/K4.3/K8/K8.4 named per operand kind. |
| — the nine admitted operand positions | EXAMPLES-ONLY | Headed "each an instance of one of K2's source kinds … the list is not exhaustive but illustrates the rule" — row 42's classification, in-text. |
| — await-result flow restrictions | DERIVED | "(derives: K1 state-match — a borrow cannot assume an obligation)". |
| Pattern B (`OwningTask<T>` for class T) | DERIVED | "(derives: K1 + K3 — the await mints an owned result, which must reach a discharging position)". |
| — the await consumes its operand | DERIVED | "(derives: K3.5 + K4.2)" — announced kill, fault edge included. |
| *Disposing an unconsumed `OwningTask<T>`* | DERIVED | "(derives: K1 cascade + §30.13's non-blocking discipline)". |
| *Multiple awaits on `OwningTask<T>`* | DERIVED | "(derives: K2 fan-out vs. K1.7 — one obligation moves once; `InvalidOperationException` is its dynamic dual)". |
| *Constructing a completed task with a class-typed result* | DERIVED | "(derives: axiom A3 + K1 — an externally-supplied owned value transfers into the single-consumer container)". |
| *Idiom: async resource acquisition* | EXAMPLES-ONLY | Non-normative idiom, per the §5.1 fate table. |

### §30.1 — the five ownership paragraphs

| Block | Class | Note / derivation |
|---|---|---|
| *Borrow-bearing parameters and results are forbidden* | DERIVED | "(v0.179, #1438; derives: K2.1 + K6.1 — a frame outlives the synchronous call; stack slots cannot enter it)"; the `CancellationToken` certification "(axiom A10)" with the K4.3 residual-trust shape named. |
| *`ref` and `out` parameters are forbidden* | DERIVED | "(v0.184; derives: K2.1 + K6.1)". |
| *Borrow-bearing record locals across suspension* | DERIVED | "(v0.179, #1448; derives: K2.1 + K6.1 — frame-resident sources)". |
| *`owned` parameters transfer into the frame* | DERIVED | "(v0.179; derives: K3 with the frame as scope)". |
| *Borrowed parameters, `this`, and captures are held until settlement* | DERIVED | "(v0.179; derives: K6.4 + K4.3)" — the settlement obligation and domain clause as the K4.3-recorded trust with K4.1's backstop. |

## 2. The exceptions log

1. **(a) Divergence D1 — §28.6's categorical store prohibitions vs §28.3's admit-with-trust.** §28.6's sentence "An owned field's value may not be assigned to a non-owned field of any other class" (and the categorical tail of the argument-passing sentence, "may not store the reference in a long-lived location") states an unconditional prohibition where §28.3's borrowed-field store rule admits parameter-, receiver-, and field-sourced borrows with a K4.3-recorded trust residual. Escalated as issue **#2043** per the standing escalation rule; the two sentences are left **unchanged and untagged** — the trains never decide a divergence. This is the audit's one deliberately untagged normative passage in §28.6.
2. **(b) Issue #2042 — the await-through-other-object-field question.** Whether a `Task<T>` read through a field path of a non-`this` object is an admitted Pattern A operand is deliberately not answered by the kernel: K2's scope note ("which source kinds a particular operand position accepts … remains that construct's own enumeration") leaves the per-position admission to §30.5, whose list names an `owned` field *of `this`*. Open as **#2042** (raised by the T1 critique, B7); no text change in these trains.
3. **(c) The T2 observation — the old seven-form discharge list was internally incomplete.** The pre-rewrite §28.3 *Owned-local discharge requirement* enumerated seven explicit discharge forms (owned-field store; `owned`/`Task`-family return; `owned`-argument move; marked transfer; explicit `Dispose()`; discard; `throw` operand — v0.197 text) while other subsections normatively treated further positions as discharging (the owning-collection slot store and `set(owned)` accessor, the Pattern B await operand, the Pattern A chaining source, the owning-iterator `yield return`, reassignment's consume of the displaced value, the move-only stores). Reconciled by the K3 collapse: K3.1–K3.12 is the single enumeration, stated once, and the chapter's lists now cite it. No ruling was needed — a pre-existing inconsistency between intra-spec case lists, resolved as their union; no rule's extension changed.
4. **(d) The composition-gate seam fix (d095a27).** §28.14's quiescence and registration-lifetime residuals are classified **K2.3-posture residuals with the K4.1 backstop**, not K4.3 members: both are flag-backstopped (`DisposedAccessException` in checked mode — §28.2's scoped-callback backstop), whereas K4.3's recorded clauses are exactly the *un*-backstopped ones. K4.3's enumeration stays the three domain-transfer clauses (field-held, collection-held, and frame-held borrows versus domain transfer; §28.3, §30.1). The T4 text initially pointed quiescence at K4.3; the composition gate over the T2–T5 union caught and fixed the seam before landing.
5. **(e) Tag-style advisory (recorded, not normalized).** Three inline citation forms coexist in the tagged text: the linked derivation parenthetical ("(derives: [K6.3](…) — …)"), bare ids in running text ("K1.7 + K2.3", "(K5.2)"), and the axiom spelling both linked ("axiom [A2](…)") and bare ("axiom A2"). Appendix C follows the same mix (ids linked on first appendix use, bare thereafter). Recorded by the composition gate as an advisory; deliberately not normalized in this revision — a mechanical sweep can unify the spelling later without touching meaning.

Recorded observations (not findings — each block is inside a rule whose tag exists, but the tag is carried by a block it cites rather than in-block): §28.1's two blocks (tags at §28.5 *Restrictions* and the A14 register row), §28.2's scoped-callback backstop (tags at §28.14's quiescence), §28.6 *Borrowed owning collections* (instance-citation of the tagged receiver rule), §28.7 *Moved-from and null* (composes the tagged K3.11/K9.2 rules), §28.14's class-typed `scoped` block (the section head's tag), §30.5's value/`string` consumer case (cites the tagged §28.3 temporary rule). Appendix C.3's `IAsyncDisposable`-warning bullet is left untagged (an async-cleanup coverage lint, §28.12's lane — no ownership rule).

## 3. The counts

- **Kernel principles:** 9 (K1–K9, §28.3, anchors `sec-kernel-k1`…`sec-kernel-k9`).
- **Axioms:** 14 (A1–A14, the register at §28.3's kernel tail; reasons in rationale §5.26).
- **Blocks audited:** 98 (§28 preamble 2; §28.1 2; §28.2 3; §28.3 22; §28.4 2; §28.5 3; §28.6 5; §28.7 4; §28.8 1; §28.8.1 8; §28.9 4; §28.10 3; §28.11 6; §28.12 2; §28.13 1; §28.14 13; §30.5 12; §30.1 5).
- **By primary classification:** KERNEL 1 (the K1–K9 subsection) · KERNEL-ABSORBED 14 · DERIVED 55 · AXIOM 10 (incl. the register itself) · EXAMPLES-ONLY 10 · OUT-OF-SCOPE 8 (§28.4 ×2, §28.9 API-surface, §28.12 ×2, §28.13, §28.14 v1-limitation and type-system-interaction blocks).
- **Findings:** 0 untagged normative ownership rules beyond the logged exceptions — the one deliberately untagged passage is exception (a) (#2043, escalated, text frozen pending the ruling). The OUT-OF-SCOPE blocks state disposal-execution mechanics, library-type internals, or API-evolution guidance, not ownership-classification rules; consistently, the §4 mapping table carries no rows for §28.4/§28.12/§28.13, and the kernel cites them as machinery (K4.1, K9's cascade).
- **Appendix C:** C.3 — 30 of 31 bullets carry K/A tags (the `IAsyncDisposable`-warning bullet excepted, above); the explicit-`Dispose()` bullet's "one recorded exception" wording synced to name axiom A8 (substance unchanged). C.6 — 9 ownership-restating bullets tagged (Pattern A/B → A3 with K2/K3; borrow-bearing and `ref`/`out` bans and suspension-crossing locals → K2.1 + K6.1; the certification → A10; call-bound settlement → K6.1/K6.4 with K6.3 + K3; frame residence → K6.1 + K6.2; temporary-into-escaping-frame → K6.5 + K8.4; calls-mint → K6.3/A2); non-ownership async bullets untouched.
- **Guide 01:** re-traced clean against the rewritten text (every example and rule-claim confirmed; no section reference moved); header pin bumped to v0.199. Guide 02 was not in T6's scope (its pin remains v0.195, set by the last stdlib-run revalidation).
