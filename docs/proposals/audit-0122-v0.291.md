# Audit: proposal 0122 L14–L16 at v0.291 — the presentation train's audit artifact

*Supporting record (not a numbered proposal). Written 2026-09-05 at the v0.291 landing, on the precedent of [`0118-audit-v0.281.md`](0118-audit-v0.281.md): the statement-level provenance for the two rewritten spec blocks (§22.12 rules 2–5 → two verdict tables paired with rationale §12.15; §29.2's six direct-indexed-loop paragraphs → two tables and one reason), the revision-narration sweep's hit list with a verdict per line, the enum-`const` clarification's decision record, and the adversarial gate round's resolution. Line numbers `S:` are the spec at `8ced282` (v0.289); §22.12 and §29.2 were byte-identical at v0.290 (`3d265ec`), the base this landed on. Nothing here is normative — the changelog entry *v0.291 (v0.290 → v0.291)* is the record of what changed; this file is the evidence the next consolidation can start from.*

**Contents.** Part A — L14's ledger (S1–S73), the spec/rationale split, both per-cell audits, word counts, and the removed-with-reason list. Part B — L15's ledger (D0–D27), the replacement, and the per-cell audit. Part C — the enum-`const` clarification. Part D — L16's method, corrected sites with every `old → new` pair, the lines left with reasons, and the kept pins. Part E — the gate round: the critique's findings and their resolutions, and the attacks that failed.

---

## Part A — L14: §22.12 rules 2–5 → two verdict tables (spec) + the grounds record (rationale §12.15)

### A.0 Scope and bounds (re-based to `8ced282`)

- Heading `### 22.12 Static members` at `S:5200`, anchor `<a id="sec-static-members"></a>` at `S:5199`; section ends before `<a id="sec-static-initialization-order"></a>` at `S:5249`. Whole section: `S:5199–5248`, **3,064** words (`wc -w`), reproducing the lane's number exactly.
- **Rewritten range (B1):** the numbered items 2–5, first line `2. A **mutable** (non-`readonly`, non-`const`) static field or auto-property is admitted **warning-free** in exactly **two forms**, …` (`S:5205`), last line `5. **Shared publication points are typed, not member-kinded** …` (`S:5237`). Untouched: the head paragraph and rule 1 (`S:5202–5205`), and the four trailing paragraphs from `Static methods, properties, fields, and constructors are declared with `static`.` (`S:5239`) through the generics-vs-less-than consequences paragraph (`S:5247`). The `const` paragraph (`S:5241`) is untouched by design — Part C's decision leaves it as the definition site.
- No fenced code block in the range; no anchor inside the range; no conformance case sourced from it (§A.7).
- **Rationale destination (R1):** a new record **§12.15** inserted before `<a id="sec-events"></a>` / `## 13. Events` (rationale line 1855–1856 at `8ced282`; the anchor pair is unique), after §12.14 *The recovery unit* — the newest record in the *Async* chapter that already holds §12.13 (the statics-and-sharing taxonomy, v0.245) and §12.8 (the locks in rule 4's set, v0.186). Anchor `sec-statics-verdict-grounds` (new; no collision — `grep -c` = 0 in all docs).

### A.1 The ledger — every normative statement in rules 2–5, verbatim

Statements are split at the prose's own sentence and clause boundaries; a citation is kept in the form the prose uses. `[hist]` marks a clause that is history narration (process §7), not a rule.

**Rule 2, head**

- **S1** "A **mutable** (non-`readonly`, non-`const`) static field or auto-property is admitted **warning-free** in exactly **two forms**, and **with a declaration-site warning** in two more (forms (iii)/(iv); form (iv) names the admission [§22.13](#sec-static-initialization-order)'s disposal rule and [§11.7](#sec-static-classes)'s static-class text made in);"
- **S2** "any other mutable static is a **compile error** naming this rule — unsynchronized cross-domain mutation is exactly the race [§30.14](#sec-sendable-types) exists to exclude."

**Form (i)**

- **S3** "**(i) Internally-synchronized handle:** the field's type declares the **synchronized** fact and not the **displacement-unbounded** fact (stdlib [§1](Bezel_Standard_Library.md#sec-statics-and-sharing-facts)'s statics-and-sharing facts — the formula is normative, and each grant's grounds live on the type's own fact line; derived membership (form-(i)), informative — currently `Mutex`, `Semaphore`, `Atomic<T>`, `AtomicInteger<T>`, `ChannelSender<T>`, and `SharedEventSource<T>` — stdlib §6.2–§6.5, §6.9; machine-checked, `scripts/check_docs.sh` 3b)."
- **S4** "`SharedEventSource<T>` is the set's one non-sendable member, safe to reach from every domain by static reachability rather than [§30.14](#sec-sendable-types) transfer (rule 5)."
- **S5** "`Channel<T>` declares no facts and is **excluded**: the channel object is the *receiving* end ([§30.15](#sec-channels)), single-consumer by ownership, and a process-global receiving end would readmit the multi-consumer receive that stdlib §6.9 explicitly defers — keep the `Channel<T>` domain-confined and publish a `ChannelSender<T>` static instead."

**Form (ii)**

- **S6** "**(ii) Lazy task cache:** a static whose declared type is `Task?` or `Task<T>?` — the admission is keyed to the declared type alone, so the check stays decidable at the declaration;"
- **S7** "the intended shape is [§22.13](#sec-static-initialization-order) pattern (b)'s `null`-initialized once-set cache of an asynchronous computation (Pattern A multi-await, [§30.5](#sec-result-lifetime-and-task-disposal))."
- **S8** "`OwningTask<T>?` is *not* admitted here: it is single-await ([§30.5](#sec-result-lifetime-and-task-disposal)), useless as a shared cache, and a displaced owned task would strand its result."

**Form (iii)**

- **S9** "**(iii) Warned value static:** a mutable static whose declared type is a **primitive value type** ([§3.1](#sec-type-kinds)) or an **enum type**, the nullable form of either, or — fields only — a **plain inline array** (`[N]T` without the `owned` slot marker, [§15.1](#sec-inline-arrays)) whose element type is itself, recursively, one of these, is admitted with a **declaration-site warning**"
- **S10** "— rule 4's posture applied to value statics: the storage is process-global and reachable from every domain, and confining every read and write of the field to one domain — or moving to a synchronized spelling: `AtomicInteger<T>`/`Atomic<T>` for the cell-sized primitives, `Atomic<int>` with a bit-cast for the floating-point types (stdlib §6.4's own note), a `Mutex` or `Locked<T>` cell for `decimal`, enums, and inline arrays, which no atomic admits — is the programmer's obligation the warning names."
- **S11** "Confinement is not compiler-observable and does not silence the warning;" *(stated three times in the prose: here, in S22's tail, and in S42 — one statement)*
- **S12** "`static readonly`/`const` (rule 3) is the warning-free spelling where the value never changes after initialization."
- **S13** "Form (iii) carries **none** of the store-semantics triple below: **post-initialization** stores are ordinary unsynchronized value stores with no atomicity, publication, or happens-before promise (the initializing store remains ordered by rule 1's initialization-completion edge; the [§30.13](#sec-runtime-model) admitted-static publication edge stays scoped to form (ii) and does not extend to form (iii))"
- **S14** "— in a program honoring the confinement obligation no second domain ever observes the field; in a program violating it, reads may observe stale values — and, for the multi-word forms (`decimal`, the nullable forms' flag-and-value pairs, and per-slot mixes of an inline array's elements), torn ones — but never a dangling reference: the form is restricted to types that transitively contain no reference, so ignoring the warning risks wrong values, never memory unsafety."
- **S15** "No ownership interaction arises (nothing to discharge or dispose)."
- **S16** "The category set — primitives, enums, and plain heap-free inline arrays — **grows only by recorded ruling**:"
- **S17** "mutable statics of `string` (and `string?`), record, choice, and compiler-known value-builtin types (`DateTime`, `Guid`, …), plain inline arrays of any other element type (a `string`- or record-element array imports its element category's dilemma component-wise; a class- or interface-element plain array carries *borrows*, [§15.1.2](#sec-inline-array-members), whose cross-domain dangling is memory-unsafe), and inline-array-typed auto-properties remain compile errors under this rule's closing sentence — declined, not deferred:"
- **S18** "a mutable string(-bearing) static's overwrite must either release the displaced referent (a cross-domain use-after-free through the runtime's own reclamation the moment the obligation is violated) or never release it (an unbounded leak under repeated assignment, unacceptable even single-domain — the payload-scaled cost the **displacement-unbounded** fact names, stdlib §1);"
- **S19** "a multi-word record static's torn cross-domain read can fabricate a value violating the record's own invariants ([§22.16](#sec-contract-clauses));"
- **S20** "and the stdlib already prescribes the synchronized spellings for these shapes (stdlib §6.4: record-typed atomics want a `Mutex`; today, `Locked<T>` over a sealed cell class, [§30.12](#sec-data-owning-locks))."

**Form (iv)**

- **S21** "**(iv) Owning-inline-array static field (admission):** a mutable static **field** of owning-inline-array type (`[N owned]T?` or `[N owned]T`, [§15.1.1](#sec-owning-inline-arrays)) is admitted — this is the admission [§22.13](#sec-static-initialization-order)'s *Disposal of static `owned` fields* and [§11.7](#sec-static-classes), named here so this rule's closing sentence and that machinery agree — with rule 4's **declaration-site warning**:"
- **S22** "the array's owned instances are reachable from every domain and its slots are mutable, so confining every slot read, slot write, `move arr[j]`, and whole-field reassignment to one domain — or restructuring behind a `Locked<T>` registry ([§30.12](#sec-data-owning-locks)) — is the programmer's obligation the warning names; confinement is not compiler-observable and does not silence the warning." *(tail = S11)*
- **S23** "Obligation violation here is undefined behavior (a slot mutation racing another domain's slot read can dangle a borrow) — rule 4's violation class, not form (iii)'s stale-values class, which is why the admission is warned."
- **S24** "Of the store-semantics triple below, only **Atomicity** does not apply (a whole-array store is N pointer words, not one): slot stores, `move arr[j]`, and per-slot restoration follow [§15.1.1](#sec-owning-inline-arrays) unchanged."
- **S25** "The **Ownership** and **Overwrite** bullets govern this form as they govern every admitted mutable static — and since v0.260 they are the *whole* ground for the plain store, no per-kind carve remaining `[hist]`: whole-field reassignment is a **plain store that never auto-disposes displaced slots** ([§22.13](#sec-static-initialization-order)'s static-overwrite posture and ownership-discharge exemption: a racing use in another domain may still hold a displaced value, so statics never take [§28.7](#sec-reassigning-owned-fields)'s drop-on-overwrite — which now covers `[hist: "now"]` owning-inline-array locals and instance fields per slot, [§15.3](#sec-move-only-value-types)),"
- **S26** "and the compiler retains the pre-v0.260 `[hist]` reassignment warning at exactly this form: a whole-field reassignment reachable with provably non-moved-from slots warns, naming the slots at risk and the per-slot discharge mechanisms, because here — unlike at every non-static location — the displaced slots genuinely persist undisposed until process exit (the same accepted process-lived cost as a displaced class-typed admitted static, scaled by the array's slot count);"
- **S27** "disposal is [§22.13](#sec-static-initialization-order)'s program-exit pass — the type-level `owned` slot marker conveys the obligation, and no field-level `owned` modifier is written."
- **S28** "`readonly` is not this form's alternative spelling: checked-disposal restoration is per-slot assignment only ([§28.2](#sec-the-idisposable-contract) family), so the slot-mutable pool pattern requires the mutable field."

**Bare generic parameter, mutable**

- **S29** "A mutable static field or auto-property whose declared type is a **bare generic parameter**, or its nullable form (`T`, `T?`), is admitted by no form above and is a **compile error at the declaration site**, whatever the parameter's constraints or eventual substitutions — and this holds wherever the declaration can appear: a generic class's statics, the statics of a type nested in a generic type, and a generic method's statics alike."

**Store semantics**

- **S30** "Three store semantics apply to every admitted mutable static:"
- **S31** "**Atomicity:** stores are single pointer-word writes the runtime performs atomically (no torn reference is ever observed), but a check-then-store sequence is **not** atomic across domains: racing first uses of a task cache may each start the work and store, last store winning — every stored task remains independently awaitable, so the race duplicates work without unsoundness; when exactly-once matters, use eager startup assignment ([§22.13](#sec-static-initialization-order) pattern (a)) or a `Mutex`."
- **S32** "**Ownership:** assigning a fresh owned value into an admitted mutable static discharges the producer — [§22.13](#sec-static-initialization-order)'s ownership-discharge exemption for non-`owned` statics extends to these fields; the referent becomes process-lived."
- **S33** "**Overwrite:** overwriting an admitted mutable static never disposes the displaced referent — deliberately *not* [§28.7](#sec-reassigning-owned-fields)'s drop-on-overwrite, since a racing first use may still hold and await the displaced task; the displaced referent remains alive until process exit, an accepted bounded cost of the same kind as the [§22.13](#sec-static-initialization-order) exemption (displacement can occur only during first-use races)."
- **S34** "*(Compiler status: forms (iii)/(iv)'s warnings and the residual-category compile errors are enforced (#1393); the one exception is the bare-generic-parameter rejection above — ruled at v0.230, still admitted silently at HEAD — tracked #2331.)*" — **not a rule**; a compiler-status note (see §A.8 for its relocation).

**Rule 3**

- **S35** "`static readonly` and `const` of **immutable content** — value types of sendable-value category, `string`, enums — are unrestricted (shared-immutable)."

**Rule 4**

- **S36** "A `static readonly` (including `owned`) field or auto-property of **class type** is admitted with a **declaration-site warning** unless the class is in this rule's **warning-suppression set** below:"
- **S37** "the referenced instance is reachable from every domain, and confining its use to one domain — or ensuring its own internal synchronization — is the programmer's obligation (the checked-disposal backstop of [§28.2](#sec-the-idisposable-contract) applies; this is the singleton-with-resources pattern the warning text names)."
- **S38** "The **warning-suppression set** (historically the *suppression list*) `[hist]` is derived, not curated: every type declaring the **synchronized** or the **immutable** fact (stdlib [§1](Bezel_Standard_Library.md#sec-statics-and-sharing-facts)'s statics-and-sharing facts, each grant's grounds on the type's own fact line; derived membership (warning-suppression), informative — currently `Mutex`, `Semaphore`, `Atomic<T>`, `AtomicInteger<T>`, `ChannelSender<T>`, `SharedEventSource<T>`, `Locked<T>`, `RwLocked<T>`, `FrozenArray<T>`, and `CancellationTokenSource` — stdlib §4.19, and stdlib §6.1–§6.5, §6.9, §6.11; machine-checked, `scripts/check_docs.sh` 3b)."
- **S39** "The four members outside rule 2's form (i) — the data-owning locks `Locked<T>`/`RwLocked<T>` ([§30.12](#sec-data-owning-locks); `static owned readonly` being the canonical process-global registry idiom), `FrozenArray<T>` (stdlib [§4.19](Bezel_Standard_Library.md#sec-frozen-array)), and `CancellationTokenSource` (stdlib [§6.1](Bezel_Standard_Library.md#sec-cancellationtoken-and-cancellationtokensource), the process-global shutdown source) — are outside it by their **displacement-unbounded** fact: rule 2's overwrite-never-disposes store semantics would strand a displaced value of unbounded payload until process exit, the payload-scaled cost each fact line names — no mutable statics of them."
- **S40** "A `static readonly Channel<T>` deliberately still warns (`Channel<T>` declares no facts) — the warning is the reminder that the channel is the single-consumer *receiving* end and must stay confined to one consuming domain ([§30.15](#sec-channels))."
- **S41** "This warning is independent of [§22.13](#sec-static-class-typed-fields-without-owned)'s non-trivial-`Dispose`-without-`owned` warning and the two may co-fire on one declaration: adding `owned` clears only the disposal warning — rule 4 applies to `owned` fields too — while this warning is silenced only by the type being in the warning-suppression set;"
- **S42** "confining the instance to one domain discharges the obligation the warning names, but confinement is not compiler-observable and does not silence the warning." *(= S11, third occurrence)*
- **S43** "An implementation may present the co-firing pair as one combined diagnostic (Appendix [D](#appendix-d-conformance-levels)'s identity latitude)."

**Rule 4 — the residual**

- **S44** "**The residual, stated (v0.265, [#3273](https://github.com/s8lap/bezel-lang/issues/3273)).** Rules 3 and 4 name a `static readonly` field or auto-property's declared type by kind, and the two kinds they name are not exhaustive; every other declared type takes its disposition here."
- **S45** "**Rule 4's declaration-site warning is the default** — the storage is process-global, confining every use to one domain is the programmer's obligation, and the warning-suppression set is consulted against the **declared** type — with three kinds excepted below."
- **S46** "This paragraph governs `static readonly` only: mutable statics are rule 2's closed list and its closing sentence, and `const` is closed by its own type requirement below; a bare generic parameter is the next paragraph's per-instantiation case."
- **S47** "**Interface type** and **function type** take the default warning explicitly."
- **S48** "An interface-typed static's runtime referent is a class instance, so rule 4's obligation is unchanged, and no interface is in the warning-suppression set, so such a static always warns — [§22.13](#sec-static-class-typed-fields-without-owned)'s conservative non-trivial-`Dispose`-without-`owned` warning is the independent second diagnostic and the two co-fire, and [§28.5](#sec-owned-fields) admits the `owned` form."
- **S49** "A function-typed static warns because a function value's **captured variables are a heap-allocated mutable cell** ([§27.5](#sec-closures)), reachable and writable from every domain through the field, with no other declaration at which to name the obligation."
- **S50** "The hazard class is form (iii)'s, not rule 4's: a capture whose source is a local scope cannot reach a static at all — [§28.9](#sec-runtime-managed-types)'s and [§28.3](#sec-local-variables-ownership-and-borrows)'s borrow-escape rules reject a function value that outlives a captured borrow's source — so only runtime-managed and value captures survive here, and ignoring the warning risks wrong or torn values, never memory unsafety."
- **S51** "The warning does not discriminate capture-free function values from capturing ones: the initializer's value may come from a call whose captures the declaration cannot see, and no rule of this specification consults another declaration's body to check a declaration."
- **S52** "A **borrow-bearing record**, a **non-owning choice that holds borrows** ([§35.4](#sec-choice-classification)'s holds-borrows property; an *owning* choice that also holds borrows takes the next bullet's rejection, which this bullet does not displace), or a **plain inline array carrying borrows** ([§15.1.2](#sec-inline-array-members)) is governed by [§9.7](#sec-records-and-disposal)'s static-field bullet and **not** by this rule: admitted in the **null-source** form as that bullet defines it (every reference-typed field, and every reference-typed slot of every inline-array field, `null` — `CancellationToken.None` is the sanctioned shape) and rejected in every other, at that rule, whatever this one would otherwise say."
- **S53** "A null-source value references nothing and draws no rule-4 warning."
- **S54** "An **owning-classified choice** ([§35.4](#sec-choice-classification) rule 1) is a **compile error at the declaration site** naming this rule, whether or not it also holds borrows: the previous bullet's null-source admission does not reach it, since an `owned` payload is process-lived with no disposal site even when every borrow-carrying component is `null`."
- **S55** "Nothing would dispose it: [§22.13](#sec-static-initialization-order)'s program-exit pass enrols `static owned` fields and owning-inline-array-typed static fields, and the `owned` field modifier is inadmissible here because [§28.5](#sec-owned-fields) admits it only on class- and interface-typed fields — so the form would be a process-lived owned value with no disposal site and no diagnostic."
- **S56** "The spelling for a process-global singleton carrying one is a `static owned readonly` field of a **class** holding the choice ([§15.3](#sec-move-only-value-types)'s field-placement rule), whose destructor disposes it."
- **S57** "This makes the `readonly` form agree with rule 2's closing sentence, which already rejects the mutable one."
- **S58** "An **owning-inline-array-typed** `static readonly` field takes the default warning and *is* disposed at exit: [§22.13](#sec-static-initialization-order)'s owning-inline-array clause is not conditioned on the field's mutability, and form (iv)'s whole-field-reassignment warning cannot arise where the field cannot be reassigned."

**Rule 4 — bare generic parameter, `static readonly`**

- **S59** "A `static readonly` field or auto-property whose declared type is a **bare generic parameter**, or its nullable form (`T`, `T?`), is admitted at the declaration and classified **per instantiation, after substitution**:"
- **S60** "a substitution within rule 3's immutable content is unrestricted; a class-type substitution takes this rule's warning unless the substituted class is in the warning-suppression set; and any other substitution takes the **residual paragraph above**, applied at that instantiation: an interface or function-type substitution takes this rule's warning, the warning-suppression set consulted where applicable; a substitution that is borrow-bearing or holds borrows is [§9.7](#sec-records-and-disposal)'s null-source case, whose own rule already evaluates per instantiation ([§3.6](#sec-derived-type-properties) rule 2); and an owning-choice substitution is rejected on that paragraph's disposal-site ground."
- **S61** "A nullable substituted form classifies as its underlying type."
- **S62** "The diagnostic lands **at the instantiation site** and names the offending type argument ([§3.6](#sec-derived-type-properties) rule 2's diagnostic pattern)."
- **S63** "A `const` of bare-parameter type is not admissible under any substitution: `const` requires a primitive value type or `string` (below), which no bare parameter ever is."
- **S64** "*(Compiler status: this classification is not yet enforced — tracked #3272, beside #2331's mutable-arm rejection.)*" — **not a rule**; compiler-status note (see §A.8).

**Rule 5**

- **S65** "**Shared publication points are typed, not member-kinded** (the `event` member kind is retired, [§24](#sec-events))."
- **S66** "A process-global publication point is an ordinary static field of type `SharedEventSource<T>` (stdlib [§6.5](Bezel_Standard_Library.md#sec-events-ievent-eventsource)) — a form-(i) member, admitted warning-free in the recommended shape `private static owned readonly _e: SharedEventSource<T> := new();` with program-exit disposal ([§22.13](#sec-static-initialization-order))."
- **S67** "The guarantees are carried by the type and therefore hold at **every** subscription site, aliases included: the subscriber list is internally synchronized (subscribe, raise, and `Subscription.Dispose` may arrive from any domain),"
- **S68** "and every call resolving to a subscribe position **declared on** `ISharedEvent<T>`/`SharedEventSource<T>` — whatever the receiver's static spelling, a constrained generic parameter included — admits only **capture-free** handlers: a non-capturing lambda or a static method reference, the same admission as `Domain.Spawn` entries, as a stdlib-stated entry in [§28.14](#sec-scoped-callback-parameters)'s capture-free register;"
- **S69** "and method-reference formation over a register position is itself rejected ([§28.14](#sec-scoped-callback-parameters)), so no plain function-typed alias of `Subscribe`/`Add` can launder the admission."
- **S70** "There is no static type through which a capture-admitting subscription to a shared source can be written (this is the enforceable statement of the guarantee whose member-kinded form was unenforceable through an `IEvent<T>` alias, resolved by construction)."
- **S71** "No payload-sendability check is imposed: under capture-freedom plus raising-domain execution (stdlib §6.5), each payload is constructed and consumed within one domain."
- **S72** "Instance sources (`EventSource<T>`/`AsyncEventSource<T>`) remain unsynchronized and capture-admitting — domain-serial per stdlib §6.5, captures per [§28.14](#sec-scoped-callback-parameters);"
- **S73** "a `static owned readonly` field of an instance-source type is admissible but draws rule 4's warning like any other unsynchronized class-typed static singleton, and confining it to one domain is the programmer's obligation the warning names."

**Count:** 73 numbered statements; 71 are rules (S34 and S64 are compiler-status notes); S11 is stated three times (S11, S22-tail, S42) and counts once; three `[hist]` clauses (inside S25, S26, S38) are narration carrying no rule content.

### A.2 The split — every statement's home (the paired-form audit)

The criterion, applied clause by clause: a **verdict**, an **obligation** a warning names, a **mechanism** (store, disposal, diagnostic placement, co-firing) or a **remedy** stays in the spec; a **ground** — why the verdict is what it is, what would go wrong, what was declined and why, a consistency proof — moves to rationale §12.15. A statement mixing the two is **split**: its rule clause stays, its ground moves, and the spec keeps a `(rationale §12.15)` pointer at the seam. A statement is placed **exactly once** per clause; nothing is duplicated (the two places a clause appears are always a rule half and a ground half).

| Statement | Home | What stays in the spec | What moves to §12.15 |
|---|---|---|---|
| S1, S2 | spec | preface (column ownership; †) | — |
| S3, S4 | spec | note (i), verbatim | — |
| S5 | **split** | "`Channel<T>` declares no facts and is **excluded** (rationale §12.15): keep the `Channel<T>` domain-confined and publish a `ChannelSender<T>` static instead." | the receiving-end / single-consumer / multi-consumer-receive ground — *Form (i)* paragraph |
| S6 | spec | note (ii), verbatim | — |
| S7 | **split** | "the intended shape is §22.13 pattern (b)'s once-set cache (Pattern A multi-await, §30.5)" | the fuller description of the shape ("`null`-initialized once-set cache of an asynchronous computation") — *Form (ii)* paragraph |
| S8 | **split** | "`OwningTask<T>?` is *not* admitted here (rationale §12.15)." | single-await / useless as a cache / stranded result — *Form (ii)* |
| S9, S10, S12, S13, S15 | spec | note (iii), verbatim (S10 re-framed as "the obligation it names is …", same content) | — |
| S11 | spec | preface **warn** definition, once (the prose's three occurrences: S11, S22-tail, S42) | — |
| S14 | **split** | "Violating the obligation risks wrong or torn values, never memory unsafety (form (iii)'s violation class; rationale §12.15)." | the violation-class ground is rationale **§12.7**'s *Warned value statics* (already there; §12.15 cites it); §12.15 keeps only the enumeration of the multi-word torn forms — *Form (iii)* |
| S16 | spec | "grows only by recorded ruling" | restated as framing in *Form (iii)* ("grows only by recorded ruling, and the shapes outside it were declined, not deferred") — the rule is the spec's; the rationale repeats the phrase only to introduce the grounds |
| S17 | **split** | the member list as the tables' six **error (iii)-closed** rows (`string`'s now in the reference-kinded table); "declined, not deferred (rationale §12.15)" | the two parentheticals (string-/record-element arrays import the dilemma; class-/interface-element arrays carry borrows) and the list restated in prose — *Form (iii)* |
| S18, S19, S20 | rationale (**§12.7**) | — | the `string` overwrite dilemma, the torn record read, and the stdlib's synchronized spellings as remedies are already rationale §12.7's *Warned value statics*; §12.15 cites that paragraph by label and adds only the citations (stdlib §1's **displacement-unbounded** fact, spec §22.16, stdlib §6.4 / spec §30.12) — *Form (iii)* |
| S21 | **split** | the admission, the citation of §22.13's *Disposal of static `owned` fields* and §11.7 | "named here so this rule's closing sentence and that machinery agree" — *Form (iv)*, first sentence |
| S22 | spec | note (iv), the obligation (its confinement tail hoisted to the preface, = S11) | — |
| S23 | **split** | "Obligation violation here is undefined behavior (a slot mutation racing another domain's slot read can dangle a borrow) — rule 4's violation class, not form (iii)'s." | why the admission is warned is rationale **§12.7**'s (cross-domain reachable *and* slot-mutable, full UB); §12.15 points — *Form (iv)* |
| S24 | spec | note (iv), verbatim; also the store-semantics applicability line | — |
| S25 | **split** | the plain-store rule, §22.13's posture and exemption cited, §28.7's per-slot scope (the two `[hist]` clauses removed) | "a racing use in another domain may still hold a displaced value, so statics never take drop-on-overwrite" — *Overwrite never disposes*, last sentence |
| S26 | **split** | the retained reassignment warning, what it names, "the displaced slots persist undisposed until process exit" (`pre-v0.260` removed) | "because here — unlike at every non-static location — … scaled by the array's slot count" — *Form (iv)* |
| S27 | spec | note (iv), verbatim | — |
| S28 | **split** | "`readonly` is not this form's alternative spelling (rationale §12.15)." | the per-slot-restoration ground is rationale **§12.7**'s ("necessarily a mutable field since restoration is per-slot assignment"); §12.15 points — *Form (iv)* |
| S29 | spec | note (v), verbatim | — |
| S30, S31, S32 | spec | store-semantics head and bullets, verbatim (S31's race description and remedy are semantics + remedy) | — |
| S33 | **split** | "never disposes the displaced referent — deliberately *not* §28.7's drop-on-overwrite; the displaced referent remains alive until process exit (rationale §12.15)" | "since a racing first use may still hold and await the displaced task"; "an accepted bounded cost of the same kind as the §22.13 exemption (displacement can occur only during first-use races)" — *Overwrite never disposes* |
| S34 | **removed** | — | — (stale compiler-status note, §A.9) |
| S35 | spec, **reworded (C1)** | rule 3 — see Part C | — |
| S36, S37 | spec | rule 4 head, verbatim | — |
| S38 | spec | rule 4 head, verbatim minus "(historically the *suppression list*)" (§A.9) | — |
| S39 | **split** | 4(a): the four members named, "outside it by their **displacement-unbounded** fact (rationale §12.15): no mutable statics of them" | "rule 2's overwrite-never-disposes store semantics would strand a displaced value of unbounded payload …" — *Rule 4(a)*, which also points at §12.7's payload-scaled-cost clause |
| S40 | spec | 4(b), verbatim (the "reminder" clause is the obligation the warning names) | — |
| S41, S42, S43 | spec | 4(c) (S42's non-observable tail hoisted to the preface) | — |
| S44, S45, S46, S47 | spec | the residual paragraph; the tables' structure (S46's column ownership in the preface; S47's "explicitly" is the two `warn` cells' existence) | — |
| S48 | spec | 4(d), verbatim — its one inline ground ("runtime referent is a class instance") is a one-clause derivation from rule 4's scope and stays as the note's statement | — |
| S49 | **split** | "warns (its captured variables are a heap-allocated mutable cell, §27.5, reachable from every domain through the field)" | "with no other declaration at which to name the obligation" — *Rule 4(e)* |
| S50 | **split** | "the hazard class is form (iii)'s — wrong or torn values, never memory unsafety" | the borrow-escape argument (why only runtime-managed and value captures survive) — *Rule 4(e)* |
| S51 | **split** | "the warning does not discriminate capture-free function values from capturing ones (rationale §12.15)" | "the initializer's value may come from a call whose captures the declaration cannot see, and no rule … consults another declaration's body" — *Rule 4(e)* |
| S52, S53 | spec | 4(f), verbatim | — |
| S54 | **split** | the verdict; "(f)'s null-source admission does not reach it" | "since an `owned` payload is process-lived with no disposal site even when every borrow-carrying component is `null`" — *Rule 4(g)* |
| S55 | **split** | "nothing would dispose it — §22.13's program-exit pass does not enrol it, and §28.5 admits the `owned` field modifier only on class- and interface-typed fields (rationale §12.15)" | "so the form would be a process-lived owned value with no disposal site and no diagnostic", with the enrolment set spelled out — *Rule 4(g)* |
| S56 | spec | 4(g), the spelling remedy, verbatim | — |
| S57 | rationale | — | the consistency proof with rule 2's closing sentence — *Rule 4(g)*, last sentence |
| S58 | spec | 4(h), verbatim | — |
| S59–S63 | spec | 4(i), verbatim (S63 also the tables' `const` column) | — |
| S64 | **removed** | — | — (stale compiler-status note, §A.9) |
| S65–S69 | spec | rule 5, verbatim | — |
| S70 | **split** | "There is no static type through which a capture-admitting subscription to a shared source can be written (rationale §13.6)." | the `IEvent<T>`-alias parenthetical — *Rule 5* paragraph, citing §13.6 |
| S71, S72, S73 | spec | rule 5, verbatim | — |

**Tally (post-critique):** spec-only **49** (S1–S4, S6, S9–S13, S15–S16, S22, S24, S27, S29–S32, S35–S38, S40–S48, S52–S53, S56, S58–S63, S65–S69, S71–S73); split **17** (S5, S7, S8, S14, S17, S21, S23, S25, S26, S28, S33, S39, S49, S50, S51, S54, S55, S70 — 18 rows, S16 counted at spec); rationale-only **4** (S18, S19, S20, S57); removed **2** (S34, S64). 49 + 18 + 4 + 2 = 73. Every clause of every split statement is listed in exactly one column above, and every *ground* has exactly one rationale home: §12.15, or §12.7's *Warned value statics* paragraph where the ground already lived (S14's violation class, S18–S20, S23's and S28's form-(iv) grounds, and the payload-scaled cost 4(a) shares) — §12.15 cites §12.7 by label for those and restates none of them (critique B3). Honest bound on "nothing duplicated" (critique C2): after the trim, an 8-token shared-n-gram scan of the spec block against §12.15 finds exactly one run, the declared S16 framing ("primitives, enums, and plain heap-free inline arrays"); the four lead-ins the critic found (S23's mechanism, S26's, S14/S50's hazard clause, S51's) are reworded so that a ground paragraph names its rule clause without repeating it. The rationale record's preamble and paragraph lead-ins are its own framing (no ledger content); §12.15's pointers to §12.7, §12.8 and §12.13 are cross-references.

### A.3 Per-cell audit — table cells (the pre-draft's audit, carried; the post-critique deltas are marked)

Cell coordinates: **T1** = reference-kinded table, **T2** = value-kinded table. **Post-critique deltas (Part E, B2/C1):** the `string` row is in **T1**, immediately above *any other class type* (the rows below still say `T2 `string``; read T1); its `const` cell reads `` `string`: ok; `string?`: — `` and T2 primitive/enum's `const` cell reads `non-nullable primitive: ok; enum and both nullable forms: —` — both S63's `const` requirement applied to the nullable forms, which §3.1's list of primitive value types does not contain. The preface's tie-break is names-first ("the first row that **names** it, and otherwise the first row that describes it"); the re-audit of every doubly-described type is in Part E (B2). Rows by their first words; columns **M** (mutable), **R** (`static readonly`), **C** (`const`). Notes are the tables' (i)–(v), the *Store semantics* bullets, 4(a)–4(i), the *residual* paragraph, and rule 5.

**Cell → statements (every cell)**

| Cell | Carries |
|---|---|
| preface: two inputs, three verdicts, column ownership | S1 (the "two warning-free, two warned" sentence, restated as the tables' `ok (i)`/`ok (ii)`/`warn (iii)`/`warn (iv)`), S46 (mutable = rule 2's closed list; `const` closed by its own type requirement), S63's `const` clause |
| preface: **warn** definition (confine or synchronize; confinement not observable, does not silence) | S11 (one statement for its three occurrences: S11, S22-tail, S42), the obligation clauses of S10/S22/S37/S45 as their shared shape |
| preface: **†** | S2 |
| preface: **‡** | S35 (rule 3, for value types of sendable-value category) + S44/S45 (residual default) at the value rows whose category the prose does not name |
| T1 form-(i) set · M | S3 (the formula); S1 "warning-free … two forms" |
| T1 form-(i) set · R | S38 (every form-(i) type is in the derived warning-suppression set — the parenthetical lists them) |
| T1 form-(i) set · C | S63's `const` requirement (a primitive value type or `string`) — the `—` column |
| T1 `SharedEventSource<T>` · M | S3, S4 |
| T1 `SharedEventSource<T>` · R | S66 |
| T1 `Task?` · M | S6, S7 |
| T1 `Task?` · R | S36 (a class type outside the warning-suppression set warns) — routed to the class-type row, no separate statement |
| T1 `OwningTask<T>?` · M | S8 (not admitted by (ii)) + S2 (hence the error) |
| T1 `OwningTask<T>?` · R | S36, routed as above |
| T1 `Channel<T>` · M | S5 (excluded from (i)) + S2 (hence the error) |
| T1 `Channel<T>` · R | S40 |
| T1 displacement-unbounded four · M | S39 ("no mutable statics of them") |
| T1 displacement-unbounded four · R | S38 (in the set), S39 (named members) |
| T1 any other class type · M | S2; S73's first clause for instance sources (mutable form of an instance-source static is not admitted by any form) |
| T1 any other class type · R | S36, S37; S73 (instance-source statics draw rule 4's warning) |
| T1 interface type · M | S2 |
| T1 interface type · R | S47, S48 |
| T1 function type · M | S2 |
| T1 function type · R | S47, S49, S50, S51 |
| T2 primitive/enum · M | S9 (primitive, enum, nullable of either) |
| T2 primitive/enum · R | S35 (enums named; primitives as value types of sendable-value category) |
| T2 primitive/enum · C | S35 + S63's `const` requirement (primitive: ok; enum: the requirement names no enum) |
| T2 `string` · M | S17 (`string`, `string?` error), S18 (the reason, in (iii)) |
| T2 `string` · R | S35 |
| T2 `string` · C | S35, S63's requirement |
| T2 plain inline array field, heap-free elements · M | S9 (fields only; the recursive element condition — the row says "heap-free elements", S16's own name for the category, and note (iii) carries the recursion verbatim) |
| T2 plain inline array field · R | ‡ (S35 / S44–S45) |
| T2 holds borrows · M | S17 (record; choice; class- or interface-element plain array carries borrows). Placed *before* the no-borrows rows so that "plain inline array of any other element type" two rows down is unambiguous — the prose's own priority, S52's "whatever this one would otherwise say" |
| T2 holds borrows · R | S52, S53 |
| T2 owning-classified choice · M | S17 (choice) — S57 states it is rule 2's closing sentence that rejects it; (iii)-closed names the same closing sentence via S17's "choice" |
| T2 owning-classified choice · R | S54–S57 |
| T2 record / compiler-known value builtin, no borrows · M | S17 (record; compiler-known value-builtin types with its `DateTime`/`Guid` examples), S19 (the record reason), S20 |
| T2 record / value builtin · R | ‡ |
| T2 plain choice, no borrows; other plain inline array · M | S17 (choice; "plain inline arrays of any other element type" — its `string`-/record-element parenthetical is in note (iii)), S18 (the string-bearing reason) |
| T2 plain choice; other plain inline array · R | ‡ |
| T2 inline-array-typed auto-property · M | S17 ("inline-array-typed auto-properties remain compile errors"); the row is field-agnostic because S9 and S21 admit **fields** only |
| T2 inline-array auto-property · R | ‡ |
| T2 owning inline array field · M | S21 (admission, warning) |
| T2 owning inline array field · R | S58 |
| T2 bare parameter · M | S29 |
| T2 bare parameter · R | S59–S62 |
| T2 bare parameter · C | S63 |
| T2 any other declared type · M | S2 |
| T2 any other declared type · R | S44, S45 |
| note (i) | S3, S4, S5 — verbatim |
| note (ii) | S6, S7, S8 — verbatim |
| note (iii) | S9, S10, S12, S13, S14, S15, S16, S17 (its two parentheticals and "declined, not deferred"; its member list is carried by the tables' **error (iii)-closed** rows), S18, S19, S20 (S11 hoisted to the preface) |
| note (iv) | S21, S22 (its confinement tail hoisted), S23, S24, S25 (the two `[hist]` clauses removed), S26 (the `pre-v0.260` `[hist]` qualifier removed), S27, S28 |
| note (v) | S29 — verbatim |
| Store semantics head | S30; the per-form applicability line = S13 (none for (iii)) + S24 (Atomicity not for (iv)) |
| Atomicity / Ownership / Overwrite bullets | S31 / S32 / S33 — verbatim |
| rule 3 | S35 — verbatim |
| rule 4 head | S36, S37, S38 (the `[hist]` parenthetical removed) |
| 4(a) | S39 — verbatim |
| 4(b) | S40 — verbatim |
| 4(c) | S41, S42 (its "confinement is not compiler-observable" tail hoisted; its "confining discharges the obligation" head kept), S43 |
| the residual paragraph | S44 (its "every other declared type takes its disposition here" clause — the "rules 3 and 4 … are not exhaustive" clause is the tables' own structure, the second table's last row), S45 |
| 4(d) | S48 (S47's interface half is the T1 interface·R cell) |
| 4(e) | S49, S50, S51 (S47's function half is the T1 function·R cell) |
| 4(f) | S52, S53 |
| 4(g) | S54, S55, S56, S57 |
| 4(h) | S58 |
| 4(i) | S59, S60, S61, S62, S63 |
| rule 5 | S65–S73 — verbatim, plus the pointer "the tables' `SharedEventSource<T>` row" |

**Statement → cell(s) (every statement)**

| Statement | Placed at |
|---|---|
| S1 | preface (column ownership) + the four `ok (i)`, `ok (ii)`, `warn (iii)`, `warn (iv)` cells |
| S2 | preface † + every `error †` cell |
| S3 | note (i); T1 form-(i)·M; T1 `SharedEventSource<T>`·M |
| S4 | note (i); T1 `SharedEventSource<T>`·M |
| S5 | note (i); T1 `Channel<T>`·M (with S2, †) |
| S6, S7 | note (ii); T1 `Task?`·M |
| S8 | note (ii); T1 `OwningTask<T>?`·M (with S2, †) |
| S9 | note (iii); T2 primitive/enum·M; T2 plain-inline-array-field·M |
| S10 | note (iii) |
| S11 | preface **warn** definition (once; the prose said it three times: S11, S22-tail, S42) |
| S12 | note (iii) |
| S13 | note (iii); Store-semantics applicability line |
| S14, S15, S16 | note (iii) |
| S17 | every `error (iii)-closed` cell (T2 `string`; holds-borrows; owning choice; record/value builtin; plain choice/other plain inline array; inline-array auto-property) — the member list; note (iii) — the two parentheticals and "declined, not deferred" |
| S18, S19, S20 | note (iii) |
| S21 | note (iv); T2 owning-inline-array-field·M |
| S22 | note (iv) (obligation); preface (confinement tail) |
| S23, S24 | note (iv); S24 also the Store-semantics applicability line |
| S25 | note (iv), the rule content verbatim; the two `[hist]` clauses ("since v0.260 … no per-kind carve remaining", "now") removed per process §7 |
| S26 | note (iv), the rule content verbatim; "pre-v0.260" removed per process §7 ("retains a reassignment warning") |
| S27, S28 | note (iv) |
| S29 | note (v); T2 bare-parameter·M |
| S30 | Store-semantics head |
| S31, S32, S33 | the three bullets, verbatim |
| S34 | **removed from the spec** — compiler-status note, and a stale one (§A.8) |
| S35 | rule 3; T2 primitive/enum·R, ·C; T2 `string`·R, ·C; ‡ |
| S36 | rule 4 head; T1 any-other-class·R; routes for `Task?`/`OwningTask<T>?`·R |
| S37 | rule 4 head |
| S38 | rule 4 head (parenthetical verbatim); T1 form-(i)·R; T1 displacement-unbounded·R |
| S39 | 4(a); T1 displacement-unbounded·M, ·R |
| S40 | 4(b); T1 `Channel<T>`·R |
| S41 | 4(c) |
| S42 | 4(c) (discharge clause) + preface (the non-observable clause, = S11) |
| S43 | 4(c) |
| S44, S45 | the residual paragraph; T2 any-other·R (the "not exhaustive … takes its disposition here" structure is the last row itself); ‡ |
| S46 | preface (column ownership: mutable = rule 2's closed list and closing sentence; `const` = its own type requirement; bare parameter = 4(i)) — the residual paragraph keeps only its last clause |
| S47 | T1 interface·R, function·R (the "explicitly" is the cells' existence; 4(d)/4(e) carry the reasons) |
| S48 | 4(d) |
| S49, S50, S51 | 4(e) |
| S52, S53 | 4(f); T2 borrow-bearing·R |
| S54, S55, S56, S57 | 4(g); T2 owning-choice·R (S57 also ·M) |
| S58 | 4(h); T2 owning-inline-array-field·R |
| S59, S60, S61, S62 | 4(i); T2 bare-parameter·R |
| S63 | 4(i); T2 bare-parameter·C; the `—` `const` column's definition in the preface |
| S64 | **removed from the spec** — compiler-status note, and a stale one (§A.8) |
| S65–S73 | rule 5, verbatim (S66 also T1 `SharedEventSource<T>`·R; S73 also T1 any-other-class·M/·R) |

**No statement is dropped; none is added.** Two things a reviewer should check against that claim:

1. Cells that *route* rather than state (`Task?`·R and `OwningTask<T>?`·R "as a class type, below"): the prose never names `Task` under rule 4; the cell does not decide it either — it says only that the class-type row applies, which is S36's own scope ("of **class type**"). Whether `Task<T>` is in the warning-suppression set is the derived fact line's business (it is not: no facts).
2. The **‡** cells (value rows' `static readonly`): the prose gives no per-kind verdict for a `static readonly` heap-free inline array, non-borrow record, value builtin, or plain choice; it gives rule 3 for "value types of sendable-value category" and the residual default for everything else. ‡ states exactly that disjunction and decides nothing. (The same pre-existing question — is a nullable primitive "of sendable-value category"? — is left as the prose leaves it; the phrase "sendable-value category" occurs in the spec only at rule 3. Not an L14 matter; an L16-class or ruling question if anyone wants it settled.)

Labels every inbound reference relies on are preserved with the same meaning: **rule 1–5** (list numbering unchanged), **form (i)–(iv)** (the note labels; "form-(i) set" and "form-(iv)" spellings kept), **rule 2's closing sentence** (now also the tables' †, still the last sentence of the preface and named as "this rule's closing sentence" in notes (iii)/(iv) and 4(g)), **rule 4's warning-suppression set**, **rule 4's residual**, **rule 2(ii)** (note (ii)), and the store-semantics bullets by name. Inbound: 53 references in `docs/` outside the changelog (`grep -rn 'sec-static-members' docs/ --include=*.md | grep -v Changelog`); the ones naming sub-labels are listed in §A.6.

### A.5 Per-cell audit (cell → statements), updated for the split

Coordinates as in the pre-draft: **T1** reference-kinded table, **T2** value-kinded table; columns **M** / **R** / **C**. The pre-draft's cell→statement table (the pre-draft edit set §A.3) holds unchanged for every **table cell** — no cell was edited except T1 form-(i)·R ("ok — in the set", still S38), T1 `SharedEventSource<T>`·R ("ok — rule 5's shape", still S66), and — after the critique — the `string` row, which moves from T2 to T1 above the class-type row with the same M/R cells (S17, S35) and a `const` cell that now reads `` `string`: ok; `string?`: — `` (S35 + S63: the `const` paragraph admits `string`, not `string?`), and T2 primitive/enum·C, now `non-nullable primitive: ok; enum and both nullable forms: —` (S63: "a primitive value type" is §3.1's list, which excludes the nullable forms — §7.2 makes `int?` an optional value type and §21.1 classifies the same spelling as a runtime const; critique C1). The notes' rows change as the split dictates; here are the rows that differ from the pre-draft, plus the rationale paragraphs:

| Cell / paragraph | Carries |
|---|---|
| preface | S1, S2 (†), S11, S35/S44–S45 (‡), S46, S63's `const` clause — plus the pointer sentence to §12.15 and the names-first tie-break with its `string` parenthetical (structure, no ledger content; critique B2) |
| note (i) | S3, S4, S5-rule (verdict + remedy) |
| note (ii) | S6, S7-short, S8-verdict |
| note (iii) | S9, S10, S12, S13, S14-class, S15, S16, S17-pointer ("declined, not deferred") |
| note (iv) | S21-admission, S22, S23-UB, S24, S25-mechanics, S26-warning, S27, S28-verdict |
| note (v) | S29 |
| store semantics head + bullets | S30 (+ S13/S24 applicability), S31, S32, S33-semantics |
| rule 3 | S35 (reworded, C1) |
| rule 4 head | S36, S37, S38 |
| 4(a) | S39-derivation + verdict |
| 4(b) | S40 |
| 4(c) | S41, S42-discharge, S43 |
| the residual | S44, S45 |
| 4(d) | S48 |
| 4(e) | S49-hazard, S50-class, S51-rule |
| 4(f) | S52, S53 |
| 4(g) | S54-verdict, S55-short, S56 |
| 4(h) | S58 |
| 4(i) | S59–S63 |
| rule 5 | S65–S69, S70-rule, S71–S73 |
| §12.15 preamble | framing only; pointers to §12.7, §12.13, §12.8 |
| §12.15 *Form (i)* | S5-ground |
| §12.15 *Form (ii)* | S7-shape, S8-ground |
| §12.15 *Form (iii)* | pointer to §12.7 for S14's violation class and S18–S20 (labels + citations only); S14's multi-word enumeration; S16 (framing restatement); S17-parentheticals + list |
| §12.15 *Form (iv)* | S21-meta; pointer to §12.7 for S23's and S28's grounds; S26-ground |
| §12.15 *Overwrite never disposes* | S33-grounds, S25-ground |
| §12.15 *Rule 4(a)* | S39-ground (+ pointers to §12.7's cost clause and §12.8) |
| §12.15 *Rule 4(e)* | S49-tail, S50-argument, S51-ground |
| §12.15 *Rule 4(g)* | S54-ground, S55-consequence, S57 |
| §12.15 *Rule 5* | S70-parenthetical (+ pointer to §13.6) |

Two reviewer checks against "nothing dropped, nothing decided": (1) the routing cells (`Task?`·R, `OwningTask<T>?`·R "as a class type, below") and the ‡ cells decide nothing, exactly as the pre-draft's §A.3 argues; (2) the split leaves no verdict, obligation, mechanism or remedy outside the spec — the reader who needs "may I write `private static _x: Foo`?", "what must I do about the warning?", "what does an overwrite do?" and "how do I spell the singleton?" never leaves §22.12; the reader who asks "why?" follows one pointer.

### A.6 Word counts (both tools; `wc -w` is the headline)

| | `wc -w` | python `str.split()` |
|---|---|---|
| rules 2–5 as they stand (`S:5206–5237`) | **2,665** | 2,723 |
| rules 2–5 replacement (§A.3, post-critique) | **2,651** | 2,754 |
| delta, spec block | **−14 (−0.5%)** | +31 |
| whole §22.12 before → after | **3,064 → 3,050** | 3,130 → 3,161 |
| rationale §12.15 (new, post-critique) | **1,116** | 1,135 |
| spec + rationale, net | **+1,102** | +1,166 |

Stated plainly, as the changelog says: the tables are a router added over verdicts the prose stated by simulation (~640 words of table after the critique's `string` row and tie-break); the pre-draft's "~850 words of reasons" are whole statements, most of which mix a rule clause with its ground, and only the ground halves (~420 words) move — so the spec section **shrinks by half a percent** while the two documents together grow by ~1,100 words (the B3 fix replaced restated grounds with pointer sentences, which cost words rather than saving them). The paired form buys one lookup per declared type and one home per ground; it does not buy a shorter spec section by more than the margin above. The pre-draft's measurement (§A.5 there) that the section is "table-shaped for its verdicts, not for its word count" stands.

### A.7 Examples, corpus, anchors, inbound references

- **No fenced example** in the range; rule 5's inline shape `private static owned readonly _e: SharedEventSource<T> := new();` is kept verbatim. **Conformance corpus:** re-verified at `8ced282` — `tests/conformance/manifest.tsv` rows `spec_0032`–`spec_0034` are `104-Methods-properties-static-members` (§10's enum blocks), and no row cites §22.12; nothing to move or drop; `#1380` not engaged.
- **Anchors:** `sec-static-members` (heading) untouched; no anchor inside the range; `sec-statics-verdict-grounds` added in the rationale. `ANCHOR: clean` on the dry run (§F).
- **Inbound sub-labels preserved:** *rule 1–5* (numbering unchanged), *form (i)–(iv)*, *rule 2's closing sentence* (the preface's †, still named "this rule's closing sentence" in the preface and note (iii); note (iv) lost the phrase when S21's meta-clause moved — critique N3), *rule 2(ii)*, *rule 4's warning-suppression set*, *rule 4's residual* (the bold **The residual** heading), the store-semantics bullets by name. The pre-draft's §A.6 hit lists (53 inbound references outside the changelog; Appendix C.11's four rows; stdlib `L:73`'s formulas) apply unchanged — none names a sentence that moved.
- **Rationale inbound:** the rationale's existing `rule 2's closing sentence` reference (1 hit) and its "suppression list" naming at §12.8 (line ~1694) stay; §12.8's parenthetical "(Presentation since v0.245 …)" is the record of the old name and is why "(historically the *suppression list*)" can leave the spec.

### A.9 Removed-with-reason

- **S34, S64 — stale compiler-status notes, removed, not relocated.** Re-verified at `8ced282`: `src/sema/TypeResolution_Checks.cpp:10990–11010` implements the mutable bare-parameter rejection S34 calls "still admitted silently at HEAD" (comment: `MUTABLE (#2331): per v0.230, rule 2's closing sentence rejects a mutable static of bare-parameter type at the declaration site`); `tests/sema/static_readonly_bare_t_3272_test.cpp` pins both arms; #3272 is closed (2026-08-22, `32db958`), #2331 is open but implemented — its landing comment is §E.5. Process §5 item 3b class; changelog records it.
- **Three `[hist]` clauses** ("and since v0.260 they are the *whole* ground for the plain store, no per-kind carve remaining", "which **now** covers", "the **pre-v0.260** reassignment warning"; "(historically the *suppression list*)") removed per process §7; rule content verbatim. Counted in L16's tally as 4 sites (`S:5213`, `S:5226`, `S:5223`, `S:5236`).
- **Nothing failed to place** (§A.2's tally closes at 73).

---

---

## Part B — L15: §29.2's six direct-indexed-loop paragraphs → two tables + one reason

### B.0 Scope (re-based to `8ced282`)

- Heading `### 29.2 Desugaring of `for`` at `S:7824`, anchor at `S:7823`; `#### Iterator yield ownership` at `S:7886` (anchor `S:7885`), untouched; section ends before `<a id="sec-iterator-methods"></a>` at `S:7930`.
- **Rewritten range (B2):** the six paragraphs from the line beginning `When `source` has inline-array type (`[N]T`, including the borrow-bearing and owning slot-marker forms)` (`S:7871`) through the line beginning `When `source` has type `IReadOnlySpan<TElement>` (stdlib [§4.16]` (`S:7881`), blank separators included. The `Range<T>` paragraph before and the `More generally, the compiler may elide` paragraph after are untouched.
- **D0 (v0.288's #3706 sentence)** at `S:7890`, outside the range: `grep -c -F 'A borrowing-family collection literal ([§15.4]'` = 1 before and after (checked on the applied scratch copy).
- Block byte-identical to `bcbe36a`; the ledger D1–D27 and the per-cell audit (pre-draft §B.1, §B.3) carry unchanged. No fenced block in the range; corpus rows `spec_0238`–`spec_0240` (`292-Desugaring-of-for`) are the desugaring fences at `S:7826–7853`, outside it — re-verified in `manifest.tsv` at `8ced282`.

### B.1 The ledger — every normative statement in the six paragraphs, verbatim

**Carried, outside the rewritten range (v0.288, #3706; `S:7930`)**

- **D0** "A borrowing-family collection literal ([§15.4](#sec-collection-literals)) bound to a local of the body is that case — `var xs: Array<int> := [1, 2, 3]; yield return xs;` in an iterator returning `IEnumerable<Array<int>>`: the iterator frame owns the literal, an owned local of the body released at its scope end under [§28.3](#sec-local-variables-ownership-and-borrows)'s ordinary rules, and the consumer borrows it." — not rewritten; listed so the carry is checkable (§B.3 maps it to "unchanged text after the range").

**Inline arrays (`S:7911`)**

- **D1** "When `source` has inline-array type (`[N]T`, including the borrow-bearing and owning slot-marker forms), the desugaring is a direct indexed loop over the array's slots"
- **D2** "— inline arrays do not implement `IEnumerable<T>` and the enumerator path of [§29.2](#sec-desugaring-of-for) does not apply ([§15.1.2](#sec-inline-array-members))."
- **D3** "The iteration variable's type and ownership state follow the slot kind: a copy for value-typed slots, a borrow for class-typed slots (whether the slot marker is `owned` or not — for owning slots the array retains ownership, and the loop body sees a borrow that does not consume the slot)."
- **D4** "Direct `move arr[i]` inside the loop body is admitted, but the field-restoration rule ([§28.8.1](#sec-the-move-expression)) applies: every moved-from slot must be reassigned before the surrounding method exits."
- **D5** "Iterating an `[N owned]T` non-nullable owning array and moving from every slot without restoration is therefore rejected; the typical pattern for "consume every slot" uses `[N owned]T?` and assigns `null` after each move."

**`Span<T>` / `ReadOnlySpan<T>` (`S:7913`)**

- **D6** "When `source` has type `Span<T>` or `ReadOnlySpan<T>` (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)), the desugaring is also a direct indexed loop over the span's range, parallel to the inline-array case."
- **D7** "Spans do not implement the enumeration protocol (`IEnumerable<T?>` for class-typed T, `IEnumerable<T>` otherwise) — the enumerator the protocol would return is an ordinary class instance that would need to hold a borrow of the underlying buffer across its own lifetime, which the borrow tracker cannot bound through the enumerator interface — so the enumerator path of this section does not apply."
- **D8** "The iteration variable's type follows the span's slot type: `T?` for class-typed T (the loop body sees a borrow of each slot's value, possibly null) and `T` for value-typed T (the loop body sees a borrow of the slot — effectively a value copy for primitive T)."
- **D9** "The borrow's source is the underlying `OwningBuffer<T>`, reached transitively through the span's `Buffer` field."
- **D10** "Direct `move span[i]` inside the loop body is admitted for class-typed `Span<T>` (not for value-typed `Span<T>`, not for any `ReadOnlySpan<T>`); the same field-restoration rule applies to the underlying buffer slot, and the typical restoration is `span[i] := null`."

**`OwningBuffer<T>`, value-typed T (`S:7915`)**

- **D11** "When `source` has type `OwningBuffer<T>` for value-typed T (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)), the desugaring is a direct indexed loop, parallel to the span case."
- **D12** "The iteration variable's type is T and the loop body sees a borrow of each slot's value."
- **D13** "(For class-typed `OwningBuffer<T>`, the type implements `IEnumerable<T?>` and the standard enumerator path applies, with the optimization-elision rule above; the observable result is the same as a direct-indexed loop.)" — "above" is a dangling direction: the general elision rule is the `More generally` paragraph *below* (the paragraph above is `Range<T>`-specific). Fixed to "below" in the replacement (editorial).

**`ReadOnlyStringSpan` (`S:7917`)**

- **D14** "When `source` has type `ReadOnlyStringSpan` (stdlib [§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)), the desugaring is a direct indexed loop, parallel to the `ReadOnlySpan<T>` case."
- **D15** "`ReadOnlyStringSpan` does not implement `IEnumerable<char>` — the enumerator returned by `IEnumerable<T>.GetEnumerator()` is an ordinary class instance that would need to hold the span (or its borrow of the source string) across its own lifetime, and the borrow tracker cannot bound that lifetime through the enumerator interface." *(= D7's reason, second wording)*
- **D16** "The iteration variable's type is `char` and the loop body sees a copy of each code point (chars are value-typed primitives)."
- **D17** "The borrow's source is the underlying `string`, reached through the span's `Source` field."

**`ArraySpan<T>` / `ReadOnlyArraySpan<T>` (`S:7919`)**

- **D18** "When `source` has type `ArraySpan<T>` or `ReadOnlyArraySpan<T>` (stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)), the desugaring is a direct indexed loop, parallel to the `Span<T>` / `ReadOnlySpan<T>` case."
- **D19** "Neither type implements `IEnumerable<T>` — the same enumerator-escape reasoning given for `ReadOnlyStringSpan` above applies." *(= D7's reason, by reference)*
- **D20** "The iteration variable's type is T and the loop body sees a borrow of each slot (the borrow's source is the underlying `Array<T>`, reached through the span's `Source` field)."
- **D21** "The iteration-variable type follows the array's element type directly — `T` is whatever the array was instantiated with (no `T?` wrapping, unlike the OwningBuffer-backed span case for class T, because `Array<T>` slots store borrows rather than owned nullable references)."
- **D22** "`move arrSpan[i]` is not admitted in the loop body for any T, since `Array<T>` slots are borrows with no ownership to transfer."

**`IReadOnlySpan<TElement>` (`S:7921`)**

- **D23** "When `source` has type `IReadOnlySpan<TElement>` (stdlib [§4.16](Bezel_Standard_Library.md#sec-ireadonlyspan)), the desugaring is a direct indexed loop over `0..<source.Length`, accessing each element via `source[i]`."
- **D24** "The iteration variable's type is `TElement`."
- **D25** "Indexer and `Length` access through the interface are virtual dispatches (vtable lookup, indirect call); the iteration is correspondingly slower than iterating a concrete span type."
- **D26** "Authors writing inner loops should iterate over concrete span types where peak per-element throughput matters; the interface-typed for-loop is the convenient form for code where breadth across span sources is more important than performance." (advisory, kept)
- **D27** "The lifetime constraint is the standard one: the `IReadOnlySpan<TElement>` value (a boxed borrow-bearing record per the rule of [§8.6](#sec-boxing-of-borrow-bearing-records)) may not outlive the source of the underlying span's borrow, which the compiler tracks through the boxed-value lifetime constraint."

**Count:** 27 statements in the range (D1–D27) plus D0 carried outside it; the enumerator-escape reason is D7 stated once, D15 restated, D19 referenced — one statement, three sites.

### B.2 The replacement (B2) — verbatim (pre-draft §B.2, unchanged)

````markdown
**Direct-indexed sources.** When `source` has one of these types — an inline array `[N]T`, the borrow-bearing and owning slot-marker forms included; `Span<T>` or `ReadOnlySpan<T>` (stdlib [§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans)); `OwningBuffer<T>` for value-typed T (stdlib §4.14); `ReadOnlyStringSpan` (stdlib [§3.3](Bezel_Standard_Library.md#sec-readonlystringspan)); `ArraySpan<T>` or `ReadOnlyArraySpan<T>` (stdlib [§4.15](Bezel_Standard_Library.md#sec-arrayspant-readonlyarrayspant)); `IReadOnlySpan<TElement>` (stdlib [§4.16](Bezel_Standard_Library.md#sec-ireadonlyspan)) — the desugaring is a **direct indexed loop** over the source's slots (for `IReadOnlySpan<TElement>`, over `0..<source.Length`, accessing each element via `source[i]`), and the enumerator path above does not apply. The tables decide, per source type, the iteration variable's type and what the loop body sees, the borrow's source, and whether `move source[i]` is admitted in the body; a `—` cell is a case this section states nothing for, and a parenthesized number points at the notes below.

| Source type | Iteration variable's type | What the body sees |
|---|---|---|
| inline array `[N]T` | follows the slot kind (1) | value-typed slots: a copy; class-typed slots: a borrow (1) |
| `Span<T>`, `ReadOnlySpan<T>` | class-typed T: `T?`; value-typed T: `T` | class-typed T: a borrow of the slot's value, possibly null; value-typed T: a borrow of the slot (2) |
| `OwningBuffer<T>`, value-typed T (5) | `T` | a borrow of each slot's value |
| `ReadOnlyStringSpan` | `char` | a copy of each code point (chars are value-typed primitives) |
| `ArraySpan<T>`, `ReadOnlyArraySpan<T>` | `T`, exactly as the array was instantiated — no `T?` wrapping (3) | a borrow of each slot |
| `IReadOnlySpan<TElement>` | `TElement` | — |

| Source type | Borrow's source | `move source[i]` in the body |
|---|---|---|
| inline array `[N]T` | — | admitted; the field-restoration rule applies (4) |
| `Span<T>`, `ReadOnlySpan<T>` | the underlying `OwningBuffer<T>`, reached transitively through the span's `Buffer` field | class-typed `Span<T>` only — not value-typed `Span<T>`, not any `ReadOnlySpan<T>` (4) |
| `OwningBuffer<T>`, value-typed T | — | — |
| `ReadOnlyStringSpan` | the underlying `string`, reached through the span's `Source` field | — |
| `ArraySpan<T>`, `ReadOnlyArraySpan<T>` | the underlying `Array<T>`, reached through the span's `Source` field | not admitted for any T: `Array<T>` slots are borrows with no ownership to transfer |
| `IReadOnlySpan<TElement>` | the source of the underlying span's borrow, which the boxed value may not outlive (6) | — |

**Why these sources bypass the protocol.** The span family — `Span<T>`/`ReadOnlySpan<T>` and `ArraySpan<T>`/`ReadOnlyArraySpan<T>` — and `ReadOnlyStringSpan` do not implement the enumeration protocol (`IEnumerable<T?>` for class-typed T in the buffer-backed spans, `IEnumerable<T>` otherwise, `IEnumerable<char>` for the string span), for one reason: the enumerator the protocol would return is an ordinary class instance that would need to hold the span — its borrow of the underlying buffer, array, or source string — across its own lifetime, and the borrow tracker cannot bound that lifetime through the enumerator interface ([§8.6](#sec-boxing-of-borrow-bearing-records) states the same contrast, the call-bounded box against the escaping enumerator). Inline arrays are outside the protocol on a different ground: they are not class types and do not implement `IEnumerable<T>` ([§15.1.2](#sec-inline-array-members)).

1. The iteration variable's type and ownership state follow the slot kind; the class-typed slot's borrow is the same whether the slot marker is `owned` or not — for owning slots the array retains ownership, and the loop body sees a borrow that does not consume the slot.
2. For value-typed T the borrow of the slot is effectively a value copy for primitive T.
3. No `T?` wrapping, unlike the OwningBuffer-backed span case for class T, because `Array<T>` slots store borrows rather than owned nullable references.
4. For an inline array, `move arr[i]` inside the loop body is admitted, but the field-restoration rule ([§28.8.1](#sec-the-move-expression)) applies: every moved-from slot must be reassigned before the surrounding method exits. Iterating an `[N owned]T` non-nullable owning array and moving from every slot without restoration is therefore rejected; the typical pattern for "consume every slot" uses `[N owned]T?` and assigns `null` after each move. For a class-typed `Span<T>`, the same field-restoration rule applies to the underlying buffer slot, and the typical restoration is `span[i] := null`.
5. For class-typed `OwningBuffer<T>`, the type implements `IEnumerable<T?>` and the standard enumerator path applies, with the elision rule below; the observable result is the same as a direct-indexed loop.
6. Indexer and `Length` access through the `IReadOnlySpan<TElement>` interface are virtual dispatches (vtable lookup, indirect call); the iteration is correspondingly slower than iterating a concrete span type. Authors writing inner loops should iterate over concrete span types where peak per-element throughput matters; the interface-typed for-loop is the convenient form for code where breadth across span sources is more important than performance. The lifetime constraint is the standard one: the `IReadOnlySpan<TElement>` value (a boxed borrow-bearing record per the rule of [§8.6](#sec-boxing-of-borrow-bearing-records)) may not outlive the source of the underlying span's borrow, which the compiler tracks through the boxed-value lifetime constraint.
````

### B.3 Per-cell audit

Rows: **IA** inline array, **SP** `Span`/`ReadOnlySpan`, **OB** `OwningBuffer` value T, **RSS** `ReadOnlyStringSpan`, **AS** `ArraySpan`/`ReadOnlyArraySpan`, **IRS** `IReadOnlySpan`. Columns: **T** iteration variable's type, **V** what the body sees, **B** borrow's source, **M** `move`. Notes (1)–(6) are the numbered list after the tables.

**Cell → statements (every cell)**

| Cell | Carries |
|---|---|
| preface (direct indexed loop; `0..<source.Length` / `source[i]` for IRS; enumerator path does not apply; `—` and note-number definitions) | D1, D6, D11, D14, D18, D23 (the six "the desugaring is a direct indexed loop" clauses — one statement, six sites); D2's and D7's "the enumerator path … does not apply" clauses |
| IA row label | D1 (borrow-bearing and owning slot-marker forms included — in the preface's list) |
| IA·T | D3 ("follow the slot kind"), with note (1) |
| IA·V | D3 (copy for value-typed slots, borrow for class-typed slots); its `owned`-marker parenthetical is note (1) |
| IA·B | — (the prose names no borrow source for inline arrays; the cell keeps that silence) |
| IA·M | D4 (admitted; restoration applies), pointer to note (4) |
| SP·T | D8 (`T?` for class-typed T, `T` for value-typed T) |
| SP·V | D8 (borrow of the slot's value, possibly null / borrow of the slot — "each" is the row's per-iteration reading, stated once in the preface); its "effectively a value copy for primitive T" parenthetical is note (2) |
| SP·B | D9 |
| SP·M | D10 (admitted for class-typed `Span<T>` only; the restoration clause is note (4)) |
| OB row label | D11's "value-typed T"; the pointer (5) routes the class-typed case to D13 |
| OB·T, OB·V | D12 |
| OB·B, OB·M | — (prose silent) |
| RSS·T, RSS·V | D16 |
| RSS·B | D17 |
| RSS·M | — (prose silent) |
| AS·T | D20 (type `T`), D21 (as instantiated, no `T?` wrapping; the reason is note (3)) |
| AS·V | D20 (a borrow of each slot) |
| AS·B | D20's parenthetical |
| AS·M | D22 |
| IRS·T | D24 |
| IRS·V | — (the prose states only the type; `source[i]` access is in the preface, D23) |
| IRS·B | D27 (the source of the underlying span's borrow, which the value may not outlive) — pointer to note (6) for the full statement |
| IRS·M | — (prose silent) |
| reason paragraph | D7, D15, D19 (one statement; the wording merges D7's "borrow of the underlying buffer" and D15's "the span (or its borrow of the source string)" into "the span — its borrow of the underlying buffer, array, or source string" so that it covers all three sites, each of which it previously covered by its own words); D2 (inline arrays: not class types, do not implement `IEnumerable<T>`, §15.1.2); D7's protocol names (`IEnumerable<T?>` / `IEnumerable<T>`) and D15's (`IEnumerable<char>`) |
| note (1) | D3's parenthetical (whether the slot marker is `owned` or not; owning slots: array retains ownership, borrow does not consume) and its "type and ownership state follow the slot kind" head |
| note (2) | D8's parenthetical ("effectively a value copy for primitive T") |
| note (3) | D21 |
| note (4) | D4, D5, D10's restoration clause |
| note (5) | D13 ("above" → "below") |
| note (6) | D25, D26, D27 |
| unchanged text after the range | D0 (carried, not rewritten) |

**Statement → cell(s) (every statement)**

| Statement | Placed at |
|---|---|
| D0 | outside the range; unchanged (`S:7930`) |
| D1 | preface; IA row label |
| D2 | preface ("enumerator path … does not apply"); reason paragraph (last sentence) |
| D3 | IA·T, IA·V; note (1) |
| D4 | IA·M; note (4) |
| D5 | note (4) |
| D6 | preface |
| D7 | preface ("does not apply"); reason paragraph |
| D8 | SP·T, SP·V; note (2) |
| D9 | SP·B |
| D10 | SP·M; note (4) |
| D11 | preface; OB row label |
| D12 | OB·T, OB·V |
| D13 | note (5) ("above" corrected to "below") |
| D14 | preface |
| D15 | reason paragraph (with D7) |
| D16 | RSS·T, RSS·V |
| D17 | RSS·B |
| D18 | preface |
| D19 | reason paragraph (with D7) |
| D20 | AS·T, AS·V, AS·B |
| D21 | AS·T; note (3) |
| D22 | AS·M |
| D23 | preface |
| D24 | IRS·T |
| D25, D26 | note (6) |
| D27 | IRS·B (short form); note (6) (full) |

**No statement dropped; none added.** Three notes:

1. The reason paragraph's phrase "`IEnumerable<T?>` for class-typed T in the buffer-backed spans, `IEnumerable<T>` otherwise, `IEnumerable<char>` for the string span" is the union of D7's and D15's protocol names; D19's "neither type implements `IEnumerable<T>`" for the array spans is D7's "otherwise". The words "buffer, array, or source string" name the three underlying sources the three sites named (D7 "underlying buffer", D15 "source string", D19 by reference to D15 with D20's `Array<T>`); no fourth source is introduced.
2. The `—` cells make the table honest rather than complete: the prose says nothing about `move` on a value-typed `OwningBuffer<T>`, a `ReadOnlyStringSpan`, or an `IReadOnlySpan<TElement>`, nor a borrow source for inline arrays or the buffer itself, nor what an `IReadOnlySpan<TElement>` body sees beyond the element's type. Each such type's own stdlib section governs those; writing "not admitted" would be an added statement.
3. Splitting the first table into a type column and a what-the-body-sees column, and moving the three parentheticals to notes (1)–(3), is the only difference from the prior draft's single wide column; each moved clause is listed above at its note, and every cell is now ≤ 100 characters (checked by script, §D.3).

### B.4 Audit, word count, anchors, gate

- Per-cell audit: pre-draft §B.3 (27 statements both ways; `—` cells record the prose's silence; D13's "above" → "below" is the one editorial fix) — carried.
- Word count: **745 → 779** (`wc -w`; 753 → 794 `str.split`), whole §29.2 3,645 → 3,679 (critique N2's re-measure). The +34 is accepted (decision taken).
- Anchors: none inside the range; `sec-desugaring-of-for` and `sec-iterator-yield-ownership` untouched. Inbound stdlib references (`L:1442`, `2540`, `2690`, `3140`, `3429`: "the desugaring is a direct indexed loop … (spec §29.2)") each name a row of the new table. §8.6's pointer to §29.2 now lands on *Why these sources bypass the protocol*; the optional §8.6 rider is §D.5 R3.
- Gate: no FACTS/SETS phrase in the range; the one dropped link is the in-section self-link `[§29.2](#sec-desugaring-of-for)`; every other link already carried. Code-generation reference row `| spans, inline arrays | a direct indexed loop; copy or borrow per slot |` (CG `:564`) stays true; no CG edit.

---

---

## Part C — Clarification: `const` of enum type (ruling-shaped; the decision stated)

**The tension (pre-draft §A.8 item 2).** Rule 3 (`S:5225`) reads: "`static readonly` and `const` of **immutable content** — value types of sendable-value category, `string`, enums — are unrestricted (shared-immutable)." — enums listed among `const`-able content. The `const` paragraph after rule 5 (`S:5241`) reads: "`const` declarations (with or without `static`) require a compile-time-constant expression ([§17.14](#sec-constant-expressions) — the referenced definition) and a primitive value type or `string`; …". The tables' `enum` row says `const: —`.

**The texts that decide it — quoted.**

1. §22.12, the `const` paragraph (`S:5241`, above): "a primitive value type or `string`". This is the definition site ("`const` declarations … require …"); rule 3's clause is a list of shared-immutable *content*, not a second type requirement.
2. §10 (`S:1752`): "A `const` member (grammar A.4 `ConstDecl`) follows [§22.12](#sec-static-members)'s class-level `const` rules: implicitly static, compile-time-constant initializer, primitive or `string` type." — an enum's *own* `const` members are restricted the same way; nothing admits the enum as the `const`'s type.
3. §21.1 *Runtime `const` locals* (`S:4270`): "**Compile-time constant** — the initializer is a constant expression by [§17.14]'s inductive definition *and* the declared or inferred type is a primitive value type or `string`." and (`S:4271`) "**Runtime const** — every other admissible initializer." — a local `const p: Priority := Priority.Low;` is admitted as a *runtime* const (a value-typed binding), not as a compile-time constant; the local rule already treats an enum type as outside the compile-time-`const` set.
4. §17.14 (`S:3705–`): "a reference to a `const` member — class-level, local, or the compiler-provided constants of the primitive wrapper types (`int.MaxValue`, stdlib §1) — or to an enum constant;" is a constant *expression* — usable where a constant expression is admitted (a `case` pattern, A.8 `Constant` "adds `QualifiedName` for enum constants and named constant references"); but its operator clauses admit "constant operands of primitive or `string` type" only, so an enum operand folds nowhere. §17.14 makes an enum *constant* a constant expression; it does not make an enum *type* a `const`-able type.
5. The changelog already records the requirement being enforced against a non-primitive type: v0.2xx's "`const Zero` becomes `static readonly property`. The three sites violate §22.12's restriction of `const` to a primitive value type or `string`" (`Bezel_Changelog.md:8361`).

**Decision.** Nothing admits `const` of enum type; three texts exclude it. Rule 3 therefore **drops enums from its `const` half** by separating the two forms:

- **C1** (inside B1's replacement; rule 3's line): `3. `static readonly` and `const` of **immutable content** — value types of sendable-value category, `string`, enums — are unrestricted (shared-immutable).` → `3. `static readonly` of **immutable content** — value types of sendable-value category, `string`, enums — and `const` (a primitive value type or `string`, the requirement stated after rule 5) are unrestricted (shared-immutable).`
- The `const` paragraph is unchanged (it is the definition site). The tables' `enum` cell (`const: —`) and 4(i)'s last sentence ("`const` requires a primitive value type or `string` (below)") follow it unchanged. §10 `S:1752` and §21.1 unchanged.

**Decided:** an enum-typed `const` — class-level or in an enum body — is a compile error under the `const` type requirement; an enum-typed `static readonly` is rule 3's shared-immutable content, unrestricted (the spelling the guides already use: §10's `static readonly` examples). **Declined:** widening the `const` requirement to enum types. It would make an enum-typed `const` a compile-time constant whose only constant operations are `=`/`<>` (§17.14 folds no enum operand), buy nothing the `static readonly` spelling does not already give warning-free, and touch §17.14, §21.1's classification, and A.8 for a form no test, example or guide uses.

**Class: clarification** (process §2: "states an already-implied rule explicitly … consolidates text that lived in several places"), gate: mechanical + one reviewer confirms the rule is unchanged. Stated honestly: a reviewer who reads rule 3's old wording as having *admitted* `const` of enum type would call this a ruling (it then "decides" the reading); the decision, the alternative and the grounds are recorded above and in the changelog to ruling shape, so taking the stricter gate costs the critique pass and nothing else. The changelog block says the same.

**Implementation check (process §5 item 3b) — recorded, not asserted.** No `build/bezelc` exists in this checkout, so the probe could not run; the source read:

- `src/sema/TypeResolution_Checks.cpp:1015–1031` (`checkConstTypes`): for every class-level `const` with a declared type, `ok = rt->kind == TypeRef::Kind::Primitive && rt->primitive != PrimitiveType::Object`; otherwise `const `X` must have a primitive value type or `string` (§22.12)`. An enum resolves to a `Named` TypeRef → **rejected**. `src/sema/TypeResolution.cpp:382` schedules it ("Slice S30: const member type must be primitive or string").
- `:1020` — `if (!cd || !cd->type) continue;  // inferred — deferred`: the **inferred-type** class-level `const` (`const K := E.A;`) is skipped here, and no later member-level check was found (`grep -rn -i inferred src/sema | grep -i const` finds only §21.1's local split at `ExprTypecheck.cpp:13083–13092`). Unverified → filed as a probe question, the issue text → [#3856](https://github.com/s8lap/bezel-lang/issues/3856), not asserted.
- `src/sema/ExprTypecheck.cpp:13083–13092`: the local `const` classification implements §21.1 exactly ("a non-primitive explicit type now classifies the local as a runtime const instead of being rejected").
- Tests: `tests/lit/sema/sema_enum__const_member_non_primitive_rejected_1564_22_12.bzl` (a *class*-typed `const` in an enum body — rejected), `sema_const__named_type_const_diagnoses.bzl`, `sema_const__object_const_diagnoses.bzl`, `sema_lconst__named_type_local_const_diagnoses.bzl` all pin the diagnostic; **no test declares a `const` of enum type** (regex over `tests/**/*.bzl` for `const X(: E)? := E.Member;` with a non-primitive `E` → 0 files) and none pins the `static readonly` enum spelling positively (suggested row in #3856). The diagnostic string is not quoted by any docs line this edit-set touches, so no `tests/` coupling arises (mechanics §5 grep run on every reworded fragment: comment hits only).

**Two more carriers of the conflation (critique B1, C7):** Appendix C.11's restatement of rule 3 (`- `static readonly` and `const` of immutable content are unrestricted; …`, spec `:12004`) → **E20** (`static readonly` of immutable content, and `const` (a primitive value type or `string`)); Guide 04 `:313` → **G1**. Both now say what rule 3 says; C.11's FACTS-pinned substrings sit after the edited clause and are untouched.

**A second probe question (critique C1's 3b consequence):** the tables' `const` cells for the nullable forms read `—` because the `const` paragraph admits "a primitive value type or `string`" and §3.1/§7.2/§21.1 place `int?`/`string?` outside that phrase; but `checkConstTypes` tests `TypeRef::Kind::Primitive` without consulting the `nullable` flag (`TypeRef.hpp:36, 43`), so `const K: int? := 3;` and `const S: string? := "x";` are not rejected by source read. Per process §5 item 3b the cell states the spec's existing requirement and the absence is not asserted: the form joins #3856's probe (addendum text in the landing notes).

**Migration contribution:** newly rejected nothing (the explicit form was already rejected by text and compiler); newly defined nothing at the language level; doc-only (rule 3 reworded; C.11 and Guide 04 aligned); one probe issue (two forms).

---

---

## Part D — L16: the revision-narration sweep, §17–§35 and Appendix A

### D.0 Method and criterion

Process §7, quoted in full as the criterion: *"**History narration creeps into normative text.** The normative documents state current rules only; the changelog (`Bezel_Changelog.md`) is the sole home for history. A bare landing pin — `(v0.NNN)` — is acceptable where it dates a rule. An explanatory narration of a superseded state is a defect wherever it appears: "previously this section permitted…", "no longer…", "this sentence formerly asserted…", "(v0.NNN — the old rule said X, which was impossible because…)". The correction's story belongs in the changelog entry that landed it; the normative text states only what the rule is. This applies to grammar comments in Appendix A and to stdlib prose as much as to spec sections."*

Range: spec lines `3285–10428` (`## 17.` through the end of `## 35.`) and `10429–11700` (Appendix A). Two passes, unioned: **every `v0.NNN` mention** (118 in range; the S2-12 criterion — a pin is fine, narration is not — so each needs a verdict) and **135 pattern hits** over seventeen spellings (`previously`, `no longer`, `formerly`, `used to`, `the old …`, `this sentence/paragraph formerly…`, `historically`, `since/before/until/through/pre-/post- v0.NNN`, `(v0.NNN — …)` with a clause, `retired/superseded/replaced by`, `is now/now covers/now states`, `earlier revision/text`, `had been/was once`, `not decided here`, `Compiler status/not yet enforced/still admitted`, `tombstone/was deleted/were removed`, `corrected/mis-stated`). **138 distinct lines** (the 90 `v0.NNN` lines ∪ the 89 pattern-hit lines). Tally, recounted mechanically after critique C3 (the recount ledger): **34 corrected lines** carrying the **31 edit sites** (40 lines change in all — six continuation lines of the multi-line grammar-comment edits A1–A6 lie outside the hit set), **7** left with a reason, **66** kept lines carrying a `v0.NNN` pin (bare, or with a current-ground clause), **31** kept current-rule wordings with no pin — 34 + 7 + 66 + 31 = 138. The partition is mechanical: corrected = touched by an edit; left = the seven D.2 lines; pin = carries `v0.NNN`; otherwise current-rule wording. §28 (`## 28.` `6322–7774`) was swept by 0118 at v0.281: its 16 `v0.NNN` mentions at `ac97491` are 17 at `8ced282` — the one regrowth is the axiom-A7 row's v0.286 clause (E9); the three `(formerly *target-announced*)` glosses, the A8-tombstone pointer, §29.1's seam paragraph and the A7 row's v0.246 clause all exist verbatim at `ac97491` (checked by `grep -c -F` against `git show ac97491:docs/Bezel_Language_Spec.md`).

Verdicts: **narration → rewrite/delete** (an `old → new` pair in the landing script); **pin → keep**; **not narration → leave, with the reason**. A deletion that would lose a rule is not narration.

### D.1 Corrected sites (31)

| # | `S:` | Section | Quote (the narration) | Verdict → edit |
|---|---|---|---|---|
| 1 | 4083 | §19.3 | "— since v0.276 those two are not the same thing:" | narration → **E1** ("the two are not the same thing, v0.276:") |
| 2 | 4089 | §19.3 | "widened from the `owned`-marked-only form at v0.251 —" | narration → **E2** ("; v0.251 —") |
| 3 | 4169 | §19.6 | "whose proof the retired v0.196–v0.244 *determination rule* did discharge (per-leaf root-instantiation matching, … — retired at v0.245; see the changelog entry and rationale §8.2)" | narration of the superseded rule → **E3** (the sub-case stays named; "(v0.245; rationale §8.2 records the earlier determination rule)") — rationale §8.2 and the paragraph's own last sentence already carry the record |
| 4 | 5197 | §22.11 | "no longer exists:" (tombstone section) | narration wording → **E4** ("does not exist:"); the tombstone, its anchor and its changelog pointer stay (L13's decision) |
| 5 | 5706 | §24 | "The `event` member kind no longer exists:" | narration wording → **E5** ("does not exist:") |
| 6 | 5749 | §25.2 | the 196-word parenthetical: "The date-scoped exemption `IntPtr`/`UIntPtr` formerly carried is retired (v0.275) … so the exemption deferred to no synthesis … and the exemption admitted it rather than preventing it. *(v0.275 retired the exemption on the ground that … That ground was stated under the §9.2 reading v0.276 rejects … and is restated here. The verdict is unchanged … throughout.)*" | narration (S2-12's largest §25 item; survived v0.289's §25 rewrite) → **E6**: 118 words stating the current rule (no exemption; the three pointer records declare all four members) and its current ground (the homogeneous overload strips auto-hashable, so without the pair they would sit outside hashable), pins `(v0.275)`, `v0.276` kept |
| 7 | 5910 | §26.5 | "which since v0.278 always includes" | narration → **E7** ("which always includes … (§2.3's invariant, v0.278)") |
| 8 | 6173 | §27.6 | "(The former instance-callee carve-out — instance iterable calls lending under axiom A2's keying — and the producers row's deliberately-open iterable-slot classification question are both retired by the v0.258 unification: the slot's invocation classification is the mint the row now states.)" | narration; the rule ("any callee kind (v0.258)") is stated earlier in the same paragraph and in §28.3's row → **E8** (deleted) |
| 9 | 6591 | §28.3 A7 | "the operator half's use case arrived at v0.286 (#3560) and resolved toward the entrenched never-consume reading:" | narration — **the one §28 regrowth since v0.281** → **E9** ("the operator half is settled (v0.286, #3560):") |
| 10 | 7800 | §29.1 | "The v0.258 unification neither creates nor widens this seam — it is the owned-marker protocol spelling's, not the calls-mint rule's." | a design-consistency proof about a past revision (0118 T1's class) → **E10** (deleted); the deferral's grounds in the same paragraph are **left** (see D.2 #7 and rider R2) |
| 11 | 8145 | §30.1 | ", so the phrase is not repeated here (v0.266, [#3281] currency)" | narration of an editorial decision → **E11** (deleted; the rule clause before it stays) |
| 12 | 9089 | §31.1 | "(v0.243 — the re-ruling that discharges the owner-review flag carried here since v0.182; rationale §11.7)" | narration → **E12** ("(v0.243; rationale §11.7)") |
| 13 | 9123 | §31.2 | "(the rule §31.2.1 has always cited, now stated here)" | narration → **E13** (heading without the parenthetical) |
| 14 | 9149 | §31.2.1 | "— the formerly idiomatic form is, since v0.243, the required one)" | narration → **E14** ("; v0.243)") |
| 15 | 9942 | §32.13 | "is now a compile error — rationale §16.2's promise delivered at the declaration layer," | narration → **E15** ("is a compile error (rationale §16.2), at zero runtime cost and with no new declaration form") |
| 16 | 10140 | §35.2 | "v0.286 — resolving the question v0.285 recorded" | narration → **E16** ("v0.286") |
| 17 | 10205 | §35.4 | "since v0.260 (§15.3; through v0.259 it kept a warning-backed manual discipline, axiom A13, and this bullet was the recorded asymmetry between the two move-only kinds — now none)" | narration → **E17** ("(§15.3, v0.260)"); A13's tombstone row in the axiom register carries the retirement |
| 18 | 10255 | §35.5 | "(v0.251, resolving spec defect [#3094])" | narration → **E18** ("(v0.251, [#3094])") |
| 19 | 10364 | §35.6 | "the v0.196 determination rule that once computed it is retired, v0.245" | narration → **E19** ("(§19.6, v0.245)") |
| 20 | 10560 | A.3 comment | "the pre-v0.242 spelling `sealed abstract`" | narration → **A1** ("`sealed abstract` (v0.242; the diagnostic names `closed`)") |
| 21 | 10966 | A.4 `ArgModifiers` | "The bare "owned" alternative was deleted at v0.246 (proposal 0090):" | narration → **A2** ("No "owned" alternative exists (v0.246, proposal 0090):") |
| 22 | 11015 | A.4 `GenericConstraints` | "The former `where T is owned` modifier-constraint form is retired along with `<owned T>` generic arguments (§14.4)" | narration → **A3** ("There is no `where T is owned` modifier-constraint form and no `<owned T>` generic argument (§14.4)") — §14.4 is the tombstone section (L13) |
| 23 | 11127 | A.6 `CaseStmt` | "The former `"owned"?` scrutinee marker was deleted at v0.246 (proposal 0090):" | narration → **A4** ("The scrutinee has no `"owned"?` marker (v0.246, proposal 0090):") |
| 24 | 11425 | A.7 `MoveExpr` | "(v0.246, proposal 0090; formerly the `"owned" Identifier` MarkedTransfer atom, deleted)" | narration → **A5** ("(v0.246, proposal 0090)") |
| 25 | 11528 | A.7 `CaseExpr` | "The former `"owned"?` scrutinee marker was deleted at v0.246 (proposal 0090)." | narration → **A6** ("The scrutinee has no `"owned"?` marker (v0.246, proposal 0090).") |
| 26 | 11616 | A.7 named slot | "(v0.250; widened v0.251)" | narration ("widened") → **A7** ("(v0.250, v0.251)") |
| 27 | 5213 | §22.12 (iv) | "since v0.260 they are the *whole* ground … no per-kind carve remaining"; "which **now** covers"; "the pre-v0.260 reassignment warning" | narration → removed inside **B1** (§A.9) |
| 28 | 5226 | §22.12 rule 4 | "(historically the *suppression list*)" | narration → removed inside **B1** |
| 29 | 5223 | §22.12 | "*(Compiler status: … ruled at v0.230, still admitted silently at HEAD — tracked #2331.)*" | stale status (false at HEAD) → removed inside **B1** |
| 30 | 5236 | §22.12 4(i) | "*(Compiler status: this classification is not yet enforced — tracked #3272 …)*" | stale status (#3272 closed) → removed inside **B1** |
| 31 | 5312 | §22.13 | "(`SharedEventSource<T>` is unimplemented at HEAD — tracked #1728)" | **stale**: registered (`src/common/BuiltinRegistry.cpp`), type-checked (52 `src/sema` sites), allocated (`src/runtime/bezel_runtime_events.cpp:300–327`, "the synchronized quiescence / lock discipline … is recorded for a follow-up slice"); the sentence's first half ("subscriber-list synchronization is not yet implemented") is accurate → **S1** (parenthetical rewritten to "the open half of #1728; the type itself is registered, type-checked, and allocated at HEAD") |

### D.2 Left deliberately — not narration (7 lines; `S:7800` is counted under D.1 #10 and appears here for its left half), with the reason

| # | `S:` | Section | Quote | Why it stays |
|---|---|---|---|---|
| 1–3 | 5073, 5075, 5077 | §22.8 | "is **not decided here**" (×3) | open-question scope statements, not history: each states the clause's *reach* ("preserves the rule as it stood rather than settling a case"); deleting them would lose that statement. Two are indexed declines (`DECLINED.md:33`, `:88`) and the third (instance constructor assigning a `static readonly` field) is genuinely open — V2's note that it should be *routed* (a rulings-train item), not deleted, stands. |
| 4–6 | 6436, 6503, 7144 | §28.3, §28.8.1 | "(formerly *target-announced*)" / "(formerly "target-announced" — …)" | present verbatim at `ac97491` — 0118 T4's own grep-trail glosses for its rename, not regrowth; §28 is outside this sweep except for regrowth. By §7's letter they are narration; the changelog's v0.281 old→new table already carries the alias. Recorded as rider **R1** (edits given) for the coordinator's call. |
| 7 | 7800 | §29.1 | "is **deferred** (v0.265), on two grounds: … The record and its reopening bar are #3255 and `DECLINED.md`." | a decline record, not a superseded state; deleting the grounds loses nothing (`DECLINED.md:308` carries both verbatim) but the compression is a judgment beyond §7's criterion → rider **R2**. The paragraph's "(v0.235, #2384)" implementation note ("the shipped compiler discharges it by resolving the alias at runtime") is a compiler-status statement, accurate as far as source reading goes; left. |
| 8 | 5270 | §22.13 | "(Compiler status: through-method-body cycle detection is implemented; the function-value/virtual-dispatch routings … and cross-module detection are tracked.)" | accurate at HEAD (`src/sema/StaticInitCycles.cpp:10, 65, 92`: virtual dispatch not walked; cross-module deferred); a status note, not narration. Whether accurate status notes belong in normative text at all is a housekeeping question this sweep does not decide. |

Also left, as **current-rule wording the patterns caught** (false positives, no edit): `S:4443` §21.9 "the warning is replaced by a hard error" (a current rule: `Task` takes an error where others warn); `S:4472` "would wrongly bless"; `S:4497` "`=> expression` replaces the block"; `S:4709` "superseded at argument positions" (a current precedence rule); `S:5110`, `5325`, `6970`, `7100`, `7659–7660`, `9123–9125`, `9151`, `9287`, `9302–9304`, `10023` — every "supersedes/superseded" is §31.2's supersession *rule*; `S:6581` the axiom register's own tombstone-row convention; `S:6696` "an earlier row"; `S:6805`, `7025`, `7566` code comments ("is now moved-from"); `S:6809` "not provided in this version"; `S:6835` "as if `obj` had been captured"; `S:7060` "the retired element-borrow carve is recorded at the axiom register's A8 tombstone" (a pointer to the register, present at `ac97491`); `S:7418` "a previously-trivial class" (a versioning rule); `S:7602` `old.Dispose()` (an identifier); `S:7867` "is replaced by a borrow"; `S:9450` "a `Ptr` previously derived from it" (temporal, not historical); `S:5195`, `5704` headings "(retired v0.188)" / "(retired member kind)" and `S:5237` "the `event` member kind is retired" (the tombstones' current statement).

### D.3 Kept pins (66 lines, post-critique recount) — bare `(v0.NNN)` / `(v0.NNN, [#NNNN])` landing pins or pins with a current-ground clause

Every remaining reviewed line carries a bare pin dating a rule, a pin with an issue link, or a pin followed by the rule's *current* ground (e.g. `S:4707` "(v0.250 — the transfer marker of §22.3 makes the two forms visibly distinct at every call site)", `S:4077` "admitted v0.256 — the slot read is the same storage read the other two perform"), all sanctioned by §7's carve. Lines (post-critique; `11016`, `11426`, `11427` are corrected lines of A3/A5 and `4762`, `4796` carry no pin — all five moved out, critique C3; `10967`, `11128`, `11529` are corrected lines of A2/A4/A6 and moved out likewise): `4077`, `4087`, `4511`, `4620`, `4707`, `5106`, `5110`, `5195`, `5228`, `5325`, `5381`, `5511`, `5745`, `5747`, `5931`, `5983`, `6039`, `6171`, `6543`, `6599`, `6755`, `6848`, `6964`, `6970`, `7009`, `7088`, `7222`, `7233`, `7517`, `7733`, `7762`, `7892`, `7894`, `7922`, `7926`, `7933`, `8046`, `8056`, `8062`, `8064`, `8081`, `8149`, `8297`, `8829`, `9061`, `9125`, `9151`, `9287`, `9288`, `9302`, `9304`, `10144`, `10151`, `10178`, `10181`, `10190`, `10198`, `10213`, `10288`, `10369`, `10371`, `10373`, `10417`, `10778`, `10955`, `11433`. (Lines such as `5110`, `9125`, `9302` carry both a pin and §31.2's supersession *rule* wording; the mechanical partition files them here.) The 31 pin-free current-rule lines: `3456`, `3804`, `4443`, `4472`, `4497`, `4624`, `4709`, `4762`, `4796`, `5108`, `5237`, `5588`, `5649`, `5704`, `5923`, `6581`, `6696`, `6805`, `6809`, `6835`, `7025`, `7060`, `7100`, `7418`, `7566`, `7602`, `7659`, `7660`, `7867`, `9450`, `10023`.

### D.4 The L16 edits — verbatim `old → new` (E1–E19, A1–A7, S1)

Each `old` is a unique substring of the spec at `8ced282` and at the v0.290 working tree (`apply_v0291.py --check`). Grammar-comment edits (A1–A6) span lines and keep the comment column; whitespace is significant and is reproduced exactly.

**E1** — `docs/Bezel_Language_Spec.md`
```
old: [§9.2](#sec-body-records) — since v0.276 those two are not the same thing: a record outside auto-hashable *because* it overloads homogeneous `=` does bear `=` — its own)
new: [§9.2](#sec-body-records) — the two are not the same thing, v0.276: a record outside auto-hashable *because* it overloads homogeneous `=` does bear `=` — its own)
```

**E2** — `docs/Bezel_Language_Spec.md`
```
old: [§15.3](#sec-move-only-value-types); widened from the `owned`-marked-only form at v0.251 — [§28.1]
new: [§15.3](#sec-move-only-value-types); v0.251 — [§28.1]
```

**E3** — `docs/Bezel_Language_Spec.md`
```
old: — including the sub-case in which every leaf is non-generic and all are named, whose proof the retired v0.196–v0.244 *determination rule* did discharge (per-leaf root-instantiation matching, solution filtering by constraint satisfaction, and the exempt/determined/undetermined classification — retired at v0.245; see the changelog entry and rationale [§8.2](Bezel_Design_Rationale.md#sec-class-hierarchy-exhaustiveness-in-case)).
new: — including the sub-case in which every leaf is non-generic and all are named (v0.245; rationale [§8.2](Bezel_Design_Rationale.md#sec-class-hierarchy-exhaustiveness-in-case) records the earlier determination rule).
```

**E4** — `docs/Bezel_Language_Spec.md`
```
old: Retired. The object-initializer block (`new T(args) with(...)` on a **class** type) no longer exists:
new: Retired. The object-initializer block (`new T(args) with(...)` on a **class** type) does not exist:
```

**E5** — `docs/Bezel_Language_Spec.md`
```
old: Retired. The `event` member kind no longer exists:
new: Retired. The `event` member kind does not exist:
```

**E6** — `docs/Bezel_Language_Spec.md`
```
old: (There is no exemption for compiler-intrinsic types. The pointer records of stdlib [§10.0](Bezel_Standard_Library.md#sec-intptr-and-uintptr) and [§10.5](Bezel_Standard_Library.md#sec-system-interop-ptr) — `IntPtr`, `UIntPtr`, `Ptr<T>` — declare `Equals(T)` and `override GetHashCode` beside their operators like any other record. The date-scoped exemption `IntPtr`/`UIntPtr` formerly carried is retired (v0.275): the homogeneous overload these types declare already strips them of **auto-hashable** ([§9.2](#sec-body-records)'s third disqualifier, ruled v0.276), so the exemption deferred to no synthesis — it left the two types whose purpose is comparison by address carrying an overloaded `=` with no `Equals`/`GetHashCode` beside it, hence outside **hashable** ([§3.6](#sec-derived-type-properties)'s hashable row, whose *declares user equality* arm is this very rule) and unusable as a `Dictionary`/`Set` key. That is this rule's own divergence reached from the other side, and the exemption admitted it rather than preventing it. *(v0.275 retired the exemption on the ground that §9.2 would otherwise synthesize the **wrong** every-pair-equal `=` and a constant `GetHashCode` for such a record. That ground was stated under the §9.2 reading v0.276 rejects — under the ruling no synthesis reaches such a record at all — and is restated here. The verdict is unchanged, and was independently carried by stdlib [§10.5](Bezel_Standard_Library.md#sec-system-interop-ptr)'s `Ptr<T>`, which has declared all four members throughout.)*)
new: (There is no exemption for compiler-intrinsic types. The pointer records of stdlib [§10.0](Bezel_Standard_Library.md#sec-intptr-and-uintptr) and [§10.5](Bezel_Standard_Library.md#sec-system-interop-ptr) — `IntPtr`, `UIntPtr`, `Ptr<T>` — declare `Equals(T)` and `override GetHashCode` beside their operators like any other record (v0.275): the homogeneous overload these types declare strips them of **auto-hashable** ([§9.2](#sec-body-records)'s third disqualifier, v0.276), so without the co-declared pair the two types whose purpose is comparison by address would carry an overloaded `=` with no `Equals`/`GetHashCode` beside it — outside **hashable** ([§3.6](#sec-derived-type-properties)'s hashable row, whose *declares user equality* arm is this very rule) and unusable as a `Dictionary`/`Set` key, this rule's own divergence reached from the other side.)
```

**E7** — `docs/Bezel_Language_Spec.md`
```
old: — which since v0.278 always includes the standard library's extension modules over always-available types ([§2.3](#sec-always-available-names)'s invariant) —
new: — which always includes the standard library's extension modules over always-available types ([§2.3](#sec-always-available-names)'s invariant, v0.278) —
```

**E8** — `docs/Bezel_Language_Spec.md`
```
old:  (The former instance-callee carve-out — instance iterable calls lending under axiom A2's keying — and the producers row's deliberately-open iterable-slot classification question are both retired by the v0.258 unification: the slot's invocation classification is the mint the row now states.)
new: (deleted — the old substring is removed; the sentence before it keeps its full stop)
```

**E9** — `docs/Bezel_Language_Spec.md`
```
old: the operator half's use case arrived at v0.286 (#3560) and resolved toward the entrenched never-consume reading: move-only operands bind borrows (§25.2).
new: the operator half is settled (v0.286, #3560): move-only operands bind borrows (§25.2).
```

**E10** — `docs/Bezel_Language_Spec.md`
```
old: including when the receiver is a borrow the caller does not own. The v0.258 unification neither creates nor widens this seam — it is the owned-marker protocol spelling's, not the calls-mint rule's. Whether
new: including when the receiver is a borrow the caller does not own. Whether
```

**E11** — `docs/Bezel_Language_Spec.md`
```
old: , so the phrase is not repeated here (v0.266, [#3281](https://github.com/s8lap/bezel-lang/issues/3281) currency). Where a buffer
new: . Where a buffer
```

**E12** — `docs/Bezel_Language_Spec.md`
```
old: (v0.243 — the re-ruling that discharges the owner-review flag carried here since v0.182; rationale [§11.7]
new: (v0.243; rationale [§11.7]
```

**E13** — `docs/Bezel_Language_Spec.md`
```
old: **A throw escaping a `finally` supersedes the in-flight exception (the rule §31.2.1 has always cited, now stated here):**
new: **A throw escaping a `finally` supersedes the in-flight exception:**
```

**E14** — `docs/Bezel_Language_Spec.md`
```
old: (`where TEx: Exception`, or a class deriving from it — the formerly idiomatic form is, since v0.243, the required one)
new: (`where TEx: Exception`, or a class deriving from it; v0.243)
```

**E15** — `docs/Bezel_Language_Spec.md`
```
old: Passing a statement where a database belongs is now a compile error — rationale §16.2's promise delivered at the declaration layer, at zero runtime cost and zero new declaration forms (ordinary positional records; `db.Raw = IntPtr.Zero` is the null test).
new: Passing a statement where a database belongs is a compile error (rationale §16.2), at zero runtime cost and with no new declaration form (ordinary positional records; `db.Raw = IntPtr.Zero` is the null test).
```

**E16** — `docs/Bezel_Language_Spec.md`
```
old: ([§25.2](#sec-operator-overloading-restrictions), v0.286 — resolving the question v0.285 recorded).
new: ([§25.2](#sec-operator-overloading-restrictions), v0.286).
```

**E17** — `docs/Bezel_Language_Spec.md`
```
old: takes the same rule per slot since v0.260 ([§15.3](#sec-move-only-value-types); through v0.259 it kept a warning-backed manual discipline, axiom A13, and this bullet was the recorded asymmetry between the two move-only kinds — now none).
new: takes the same rule per slot ([§15.3](#sec-move-only-value-types), v0.260).
```

**E18** — `docs/Bezel_Language_Spec.md`
```
old: (v0.251, resolving spec defect [#3094]
new: (v0.251, [#3094]
```

**E19** — `docs/Bezel_Language_Spec.md`
```
old: ([§19.6](#sec-exhaustiveness-generic-leaves); the v0.196 determination rule that once computed it is retired, v0.245);
new: ([§19.6](#sec-exhaustiveness-generic-leaves), v0.245);
```

**A1** — `docs/Bezel_Language_Spec.md`
```
old:                                  `closed static` are rejected, as is the
                                 pre-v0.242 spelling `sealed abstract` (the
                                 diagnostic names `closed`); other combinations
new:                                  `closed static` are rejected, as is
                                 `sealed abstract` (v0.242; the
                                 diagnostic names `closed`); other combinations
```

**A2** — `docs/Bezel_Language_Spec.md`
```
old:                               /* The bare "owned" alternative was deleted at
                                 v0.246 (proposal 0090): the call-site transfer
new:                               /* No "owned" alternative exists (v0.246,
                                 proposal 0090): the call-site transfer
```

**A3** — `docs/Bezel_Language_Spec.md`
```
old:                               /* The former `where T is owned` modifier-
                                 constraint form is retired along with `<owned T>`
                                 generic arguments (§14.4); constraints bound
                                 types only. */
new:                               /* There is no `where T is owned` modifier-
                                 constraint form and no `<owned T>` generic
                                 argument (§14.4); constraints bound types
                                 only. */
```

**A4** — `docs/Bezel_Language_Spec.md`
```
old: arm. The former `"owned"?` scrutinee marker was
                                 deleted at v0.246 (proposal 0090): the scrutinee
new: arm. The scrutinee has no `"owned"?` marker
                                 (v0.246, proposal 0090): the scrutinee
```

**A5** — `docs/Bezel_Language_Spec.md`
```
old:                                  K3.4 transfer marker (v0.246, proposal 0090;
                                 formerly the `"owned" Identifier`
                                 MarkedTransfer atom, deleted). Alternative
new:                                  K3.4 transfer marker (v0.246, proposal
                                 0090). Alternative
```

**A6** — `docs/Bezel_Language_Spec.md`
```
old:                               /* The former `"owned"?` scrutinee marker was
                                 deleted at v0.246 (proposal 0090). The scrutinee
new:                               /* The scrutinee has no `"owned"?` marker
                                 (v0.246, proposal 0090). The scrutinee
```

**A7** — `docs/Bezel_Language_Spec.md`
```
old: "move" marks the slot (v0.250; widened v0.251):
new: "move" marks the slot (v0.250, v0.251):
```

**S1** — `docs/Bezel_Language_Spec.md`
```
old: rule 5's shared-source subscriber-list synchronization is not yet implemented (`SharedEventSource<T>` is unimplemented at HEAD — tracked #1728).
new: rule 5's shared-source subscriber-list synchronization is not yet implemented (the open half of #1728; the type itself is registered, type-checked, and allocated at HEAD).
```

### D.5 Riders (optional; coordinator's call; not part of L16 proper)

- **R1 — §28's three `(formerly *target-announced*)` glosses** (D.2 #4–6). If taken: `S:6436` `***Marker-free position*** (formerly *target-announced*) — an owned-consuming` → `***Marker-free position*** — an owned-consuming`; `S:6503` `except at a **marker-free position** (formerly *target-announced*), where` → `except at a **marker-free position**, where`; `S:7144` `as it is at **marker-free positions** (formerly "target-announced" — positions whose target` → `as it is at **marker-free positions** (positions whose target`. Each unique (`grep -c -F` = 1). Changelog addition: "and §28's three `formerly *target-announced*` glosses, 0118 T4's grep-trail aids, are retired now that the v0.281 entry carries the rename table". `tests/` couplings: `target-announced` appears in 1 gtest and 6 lit files as **comments** (no CHECK row), so no test moves.
- **R2 — §29.1's deferral compression** (D.2 #7). `old:` `is **deferred** (v0.265), on two grounds: the exemption would make one member classify differently at a direct call than at a dispatched one, which the [announcement table](#sec-announcement-table)'s inheritance rule forecloses ("Interface dispatch is therefore not a column: it is the declaring signature's row"), and deciding it would consume information from inside the implementing body at another declaration's call site — a carried per-body fact ([§2.8.2](#sec-artifact-interface), proposal 0105 §1) bought for no change in any program's meaning. The record and its reopening bar are [#3255](https://github.com/s8lap/bezel-lang/issues/3255) and `docs/proposals/DECLINED.md`.` → `new:` `is **deferred** (v0.265): the two grounds and the reopening bar are recorded at [#3255](https://github.com/s8lap/bezel-lang/issues/3255) and in `docs/proposals/DECLINED.md`.` (unique; `DECLINED.md:308` carries both grounds verbatim — checked.)
- **R3 — §8.6's third wording of the enumerator-escape reason** (pre-draft §D.1): `S:1268` — note v0.290's working tree **edits this very line** (`@@ -1268 +1268 @@`); if taken, re-base its `old` on the v0.290 text at landing.
- **R4 — §29.2's `More generally` paragraph fold** (pre-draft §B.5): one-line editorial rider; not taken here.

---

---

## Part E — The gate round: critique resolution (the critique, verdict LAND-WITH-CORRECTIONS: 3 BLOCKING, 8 CORRECTION, 9 NOTE)

Finding → change. Every BLOCKING and CORRECTION is applied; every NOTE is applied or declined with its reason. The edits named (E20–E22, G1, P3, D1) are in the landing commit; the critique itself is a scratchpad record outside the repository; its §0 mechanical pass and its N9 are reproduced below.

| Finding | Resolution |
|---|---|
| **B1** Appendix C.11 still carried "`static readonly` and `const` of immutable content" | **E20** in the landing script: `- `static readonly` of immutable content, and `const` (a primitive value type or `string`), are unrestricted; …` — the FACTS-pinned substrings later in the same line are byte-identical (`FACTS: clean` on the scratch apply). Changelog: the clarification block and the Doc-only bullet name C.11. |
| **B2** the tie-break "first row that describes it" routed `string` to the class-type row | The preface's rule is now *names-first*: "A declared type takes the first row that **names** it, and otherwise the first row that describes it (`string`, a reference type, §3.1, has its own row in the first table, above the class-type row)". The `string`/`string?` row **moves into the reference-kinded table** (it is a reference type; the caption is now honest) immediately above *any other class type*, so both readings — names-first and describes-first — find it before the class-type row. Per-cell re-audit of every declared type more than one row describes: `string`/`string?` (T1 `string` row before T1 class-type row: M `error (iii)-closed` = S17, R `ok — rule 3` = S35, C per S63 — the old prose's verdicts); `SharedEventSource<T>` (T1 row 1 and T1 row 2 both describe it; names-first picks row 2; the rows agree cell for cell: `ok (i)` / `ok` / `—`); `Task?`, `OwningTask<T>?`, `Channel<T>`, the displacement-unbounded four (each named by its own row before *any other class type*; verdicts unchanged); an owning choice that also holds borrows (T2 *holds borrows* says "non-owning choice", so only the *owning-classified choice* row describes it — 4(g), as before); an inline-array auto-property carrying borrows (T2 *holds borrows* — "a plain inline array" — describes it first, giving 4(f)/§9.7, which is S52's own "whatever this one would otherwise say" priority; the *auto-property* row's ‡ is reached only by the borrow-free kinds); a bare parameter, a value type, an interface or function type (one row each). No other type is doubly described. |
| **B3** rationale §12.7's *Warned value statics* already carried five of §12.15's grounds | **§12.15 cites §12.7; §12.7 is untouched.** The preamble names three neighbouring records (§12.7, §12.13, §12.8); *Form (iii)* now points at §12.7 for the violation class and for the `string`/record declines and remedies (keeping only the multi-word enumeration, the closed list, the inline-array parentheticals and the citations); *Form (iv)* points at §12.7 for why it is warned and why it is necessarily mutable (keeping S21's meta-clause and S26's reassignment ground); *Rule 4(a)* points at §12.7's payload-scaled-cost clause; the no-publication-edge posture was never restated. §A.2 rows S14, S18–S20, S23, S28, S39 and the tally paragraph record the new homes; the changelog says "each ground has one home: §12.15 cites §12.7 for the four grounds that record already carried". |
| **C1** two `const` cells admitted nullable forms | T2 primitive/enum·C → `non-nullable primitive: ok; enum and both nullable forms: —`; the `string` row's C → `` `string`: ok; `string?`: — ``. The compiler's `checkConstTypes` does not consult the nullable flag, so per process §5 item 3b the cells state the `const` paragraph's existing requirement and the compiler question joins #3856 (Part C, second probe). |
| **C2** "placed exactly once … nothing is duplicated" false in five places | Four lead-ins reworded in §12.15 (S23's mechanism, S26's, S14/S50's hazard clause, S51's); the S16 framing restatement was declared and stays. Re-scan: one shared 8-gram, the declared one. The claim is restated honestly in §A.2, the changelog and the audit ("each ground has one home; a ground paragraph names its rule clause without repeating it"). |
| **C3** L16 bookkeeping did not close; three D.3 lines were corrected lines; two carried no pin | Recounted mechanically (the recount ledger): 138 reviewed lines = 34 corrected (31 sites; 40 lines change with the six grammar-comment continuation lines) + 7 left + 66 pins + 31 current-rule. `11016`, `11426`, `11427` (and the other grammar-comment lines `10967`, `11128`, `11529`) moved to corrected; `4762`, `4796` to current-rule. §D.0, §D.3 and the changelog carry the reconciled numbers. |
| **C4** "(i)–(v)" as admitted forms in three carriers | → "(i)–(iv) and the bare-generic-parameter rejection (v)" in the changelog, §12.15's preamble and the #3845 text. |
| **C5** the placeholder issue number at three sites, linking a scratchpad file | All three → `[#3856](https://github.com/s8lap/bezel-lang/issues/3856)`; a grep for the placeholder over `docs/` prints nothing after apply (the changelog's gate-round bullet says "the placeholder issue number", not the literal). |
| **C6** `SharedEventSource<T>` "type-checked (`src/sema/`, 52 sites)" | → "type-checked (`src/sema/`, 10 sites) and lowered (`src/codegen/`, 27; 52 sites across `src/`)" — re-measured: sema 10, codegen 27, common 9, runtime 4, ownership 2. |
| **C7** Guide 04 taught `const` of enum type | **G1**: the bullet splits into `static readonly` of … enums, and `const` of a primitive value type or `string`. No pin move (no example re-traced; process §5 item 5); the changelog's Doc-only bullet says so instead of "Guides: no contact". |
| **C8** the changelog cited `0122-audit-v0.291.md`, which fails `check_proposals.sh` | → `docs/proposals/audit-0122-v0.291.md` (non-numbered; gate-indifferent, verified), cited in the changelog and the #3845 text; **P3** adds its row to the README's supporting-records table (N8). |
| **N1** `S:5206` off by one | → `S:5205`. |
| **N2** whole §29.2 3,643 → 3,677 | → 3,645 → 3,679 (re-measured; the changelog states the section counts). |
| **N3** "still named … in notes (iii)/(iv)" | §A.7 corrected: preface and note (iii); note (iv) lost the phrase with S21's meta-clause. |
| **N4** §9.7 `:1507` cites "rule 4's residual states in a line" | **E21**: → "rule 4's note (f) states in a line" (the sentence's new home). Listed in the changelog's Doc-only bullet as an out-of-range site the train's own edit made stale. |
| **N5** the generic-method arm of the bare-`T` rule is not demonstrably enforced | The #2331 comment (§E.5) names it as the one thing the source read cannot confirm, with the probe; the changelog's L14 bullet says "enforced for a generic type's statics" and records the method arm as open on #2331; the comment no longer says "nothing further is owed". |
| **N6** the `generic_static_state_536` hedge is resolvable | Dropped; the comment states the tests were retired (`grep` → 0). |
| **N7** §15.3 `:2890` keeps "the **pre-v0.260** reassignment warning" | **E22** applied (one qualifier dropped; §15 is outside L16's range, so the changelog lists it as an out-of-range twin corrected alongside rather than as an L16 site). |
| **N8** README supporting-records row | Added (P3). |
| **N9** attacks that failed | Recorded in the audit artifact's closing section, so the next reviewer need not redo them. |

Also applied from process §9 (not a critique finding): the enum-`const` widening the changelog declines gets a `DECLINED.md` index row (**D1**, class `rejected`) in the same commit.

### The attacks that failed (the critic's N9, verbatim — recorded so the next reviewer need not redo them)

- **N9.** Things I attacked and could not break, recorded so the next reviewer need not redo them: the L14 and L15
  ledgers are verbatim **and complete** (100 % coverage, zero uncovered text); §12.15 places every split
  statement's ground and every split pair reads correctly (I read all 18 pairs); the routing (`Task?` /
  `OwningTask<T>?` "as a class type, below") is sound — `OwningTask<T>` is a class type by
  `docs/Bezel_Standard_Library.md:4191` ("`Task`, `Task<T>`, and `OwningTask<T>` all satisfy the parameter"); the
  ‡ and `—` conventions decide nothing; L15's `—` cells correctly record the prose's silence at
  `IReadOnlySpan<TElement>`'s "what the body sees", the inline array's borrow source, and `OwningBuffer<T>`'s
  `move`; the new §29.2 cross-reference to §8.6 is accurate (`:1268` really does state "The contrasting *forbidden*
  case is the `IEnumerable<T>` enumerator path … escaping … versus call-bounded"); every one of the 19 L16 prose
  edits removes narration and no rule — I read E3's, E8's and E10's full surrounding paragraphs and each rule
  survives elsewhere in the same paragraph (E8's "any callee kind (v0.258)" at `:6172`'s opening; E10's seam
  statement in the paragraph body); and the enum-`const` grounds are all four quoted correctly
  (§22.12 `:5240`, §10 `:1752`, §21.1 `:4269-4270`, §17.14 `:3714-3728`), with **no** `const` of enum type
  anywhere in `docs/` or `tests/` (regex sweep over every `const NAME[: T] := X.Y` form; `src/` holds no Bezel source — `find src -name '*.bzl'` = 0).

### The critic's mechanical pass (pre-resolution; the post-resolution gate lines are in the changelog entry)

| Check | Result |
|---|---|
| `apply_v0291.py --check .` (HEAD, no flag) | `check: FAILED` — H1–H4 `count=0`, exactly as §E.1 documents (HEAD is v0.290) |
| `apply_v0291.py --check . --base290` | `check: OK (33 substring edits, 2 blocks, 1 inserts)` |
| `apply_v0291.py --apply . --base290` in the scratch tree | clean; four headers at `v0.291`, CG pin stays `v0.290` |
| `scripts/check_docs.sh` (full) | `check_docs: OK` — `PIN clean` (CG 1 behind), `FENCE`, `ANCHOR`, `VERSION: clean (v0.291, single changelog entry)`, `FACTS: clean (… all 3 derived-set parentheticals match)`, `TABLE`, `SETS (8 members)`, `REGISTER`, `STYLE`, `EXTRACT: clean (510 fragments…)`; the standing `CITATION (advisory) [spec] §4.12` unchanged |
| `scripts/check_proposals.sh` | `check_proposals: OK (BIJECTION 123↔123, TOKEN clean, SECTION live 47 / graduated 76 / declined 0, TABLE clean)` |
| `git diff --stat` | 6 files, ~167 insertions / ~70 deletions — matches §F.2 |

**Audit-artifact filename (§E.6) — settled empirically.** I created each candidate in the scratch tree and re-ran the gate:

- `docs/proposals/audit-0122-v0.291.md` (non-numbered, the coordinator's choice) → `check_proposals: OK`, **exit 0**. The script is indifferent; **no `scripts/` edit is needed.** ✔
- `docs/proposals/0122-audit-v0.291.md` (numbered) → `BIJECTION: 1 violation (no README row links to it)` + `TOKEN: 1 violation (no Status row found)`, `check_proposals: FAIL`, **exit 1** — confirming §E.6's analysis and the `EXCLUDE` requirement (`scripts/check_proposals.sh:41`).

**Ledger integrity — both trains, verified mechanically, and this is the strongest part of the work.**

- L14: all **73** ledger quotes (the pre-draft edit set §A.1, S1–S73) are verbatim substrings of the current §22.12 (`docs/Bezel_Language_Spec.md:5198–5247`); S25/S26/S38 match once the inline `` `[hist]` `` annotations are stripped. Their union **covers 100 %** of rules 2–5 (`:5204–5236`): zero uncovered runs of more than two characters. Nothing in the old text escaped the ledger.
- L15: all **27** quotes (D1–D27) verbatim, union covers 100 % of `:7870–7880`; **D0** (v0.288's #3706 sentence) is outside the range and survives — `grep -c -F 'A borrowing-family collection literal ([§15.4]'` = **1** before and after the apply. ✔
- Word counts, `LC_ALL=C wc -w`, measured by me: rules 2–5 **2 665 → 2 613** ✔; whole §22.12 **3 064 → 3 012** ✔; rationale §12.15 **1 098** ✔; §29.2's six paragraphs **745 → 779** ✔. `derived membership` count after apply = **3** ✔.
- Compiler claims re-read: `src/sema/TypeResolution_Checks.cpp:11008-11019` (mutable arm, field) / `:11020-11024` (readonly arm) and `:11057ff` (the auto-property twin) enforce **both** bare-`T` arms; `#3272` is **closed** (2026-08-22, `32db958` = "fix #3272: §22.12 static readonly bare-T classified per instantiation"); `#2331` is **open**; `tests/sema/static_readonly_bare_t_3272_test.cpp` really does carry `mutable_bare_t_rejected_at_declaration_spec_22_12_issue_2331` and its nullable / auto-property twins. So S34 and S64 *are* stale (see N5 for the one arm this does not cover).
- `SharedEventSource<T>` (§22.13 / S1): registered — `src/common/BuiltinRegistry.cpp:282, 289, 954, 959, 1214`; allocated — `src/runtime/bezel_runtime_events.cpp:317, 324`; the open half is the lock discipline, in that file's own words at `:302-306` ("the synchronized quiescence / lock discipline around the subscriber list … is recorded for a follow-up slice"). The corrected parenthetical is right.

---
