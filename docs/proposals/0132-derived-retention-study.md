# Proposal 0132 — Study: derived retention — a per-parameter "retains in `this`" fact computed from the callee body, carried in the interface, and consumed only by a caller-side warning

| | |
|---|---|
| **Status** | study — owner-directed 2026-09-22; recommendation recorded, no normative change proposed |
| **Tracking issue** | none — a study; an issue is filed if the owner takes it up (§7 names the evidence) |
| **Target module(s)** | none now. If taken up as a proposal: **spec** — §3.6 (a derived *member* property, or a stated reason why the mechanism is not §3.6's), §2.8.2 clause 1 (a per-parameter retention record beside the purity modifier), §2.8.6 (one delta row), §28.3's *Diagnostics* (the warning), §28.14 (the bounding-object forms reused at plain call sites); **rationale** — §9.7 (the declared-not-derived record would need a stated carve for warning-only consumption); **code-generation reference** §4.6. |
| **Written against** | spec **v0.306** (`cafdfc5`) |
| **Origin** | The 2026-09-22 "path of least resistance" investigation, third item: the compiler knows, when it checks `Account`'s constructor, that the constructor stores `owner` in a field; the caller writing `new Account(c)` and then `c.Dispose()` while the account lives does not. Proposal 0131 makes the *callee* side loud; this study asks whether the *caller* side can be made loud without a declaration, without a freeze, and without repeating the mistake proposal 0048 retired. |

*Reference convention: unprefixed §-references are language-spec references; rationale and proposal references are marked; references to this study's own sections are written "this study's §N". K tags are §28.3's kernel ids. Quotations are drawn at v0.306 and are verbatim; bold inside a quotation is the source's own. ▲ marks a hypothetical surface, landed nowhere.*

---

## 0. The question

A constructor that stores a plain parameter into a non-`owned` field makes a K4.3 admission (§28.3's borrowed-field store rule, clause 3: *"whether the caller-side source actually outlives the object is the programmer's obligation"*). The obligation is the *caller's*, and the caller's signature-level view of the callee is a plain parameter that announces nothing — K2.4: *"a parameter source the signature does not announce"*. The language's declared answer is `scoped` (§28.14): the callee announces retention, the caller's argument is checked against the bounding object, and the residual vanishes. The question this study was asked is whether a **derived** answer exists beside the declared one: a per-parameter fact, computed from the body the way trivial-dispose is computed from a declaration, carried in the artifact interface, and consumed by callers **only** to emit a warning where the caller's own code makes the dangle statically certain — the source killed while the holder provably lives.

The study's answer is in this study's §7: **defer**, with the evidence that would move it and the bar for reopening. The reasons are the shape of the property (this study's §1), the shape of the warning (§2), the drift analysis (§3), the interaction with `scoped` (§4), the three records it would have to answer (§5), and what cannot be settled without a probe (§6).

## 1. The property, precisely ▲

**Definition (candidate).** For a constructor or instance method `M` of class `C`, and each parameter position `i` of `M` whose declared type is a class or interface type and which is neither `owned` nor `scoped`: `retains(M, i)` holds iff some path through `M`'s body stores a borrow whose source is parameter `i` into

- (a) a **direct, non-`owned`, `readonly`** class- or interface-typed field of `this` — the §28.14 scoped-construction storage shape (*"a direct non-owned field of the bounding object"*) narrowed to `readonly`; or
- (b) a **`scoped`** parameter of a call on `this` (forwarding), transitively under this definition.

**What does not count, and why.**

- A store into a **mutable** plain field of `this`. The hazard the warning wants to announce is *statically certain*: the holder still refers to the source at the source's kill. A mutable field may have been overwritten or cleared on some path between construction and the kill (`_owner := null;` in a `Detach()` method), and no per-call analysis can see it; the warning would degrade to "may", which §5.28's doctrine reserves the default-build warning class *against*. A `readonly` field has *"no writer on any path, through any alias"* outside its construction window (§7.4's own ground for narrowing it), so retention through it is retention for the object's life. This is the same reason §7.4 narrows `readonly` fields and no others.
- A store into a field of **another** object (`other._f := p;`), admitted by §28.3 when the source is a parameter. The holder is not the constructed object, so no bounding form of §28.14 names it at the call site; nothing at the caller could be checked against it.
- A store into a **collection** reachable from `this` (`_items.Add(p);`). The element may be removed before the kill; not certain.
- A store of `this` itself into the argument (`p.Register(this)`) — the mirror shape, retention of the *holder* by the *source*. Out of scope here; noted because it is the other half of every back-reference pair.
- A **static** method or a **static** field. No `this`; no bounding object.

**Its grammatical species.** Trivial-dispose (§28.9) is a property of a *declared class*, *"computed from that declaration's own structure"*; §3.6 rule 1 says a derived type property *"follows from structure"*. `retains` is computed from a **body**, and it is a property of a **member's parameter**, not of a type — the species rationale §9.7 names in its first bullet: *"purity is a behavioral contract of a member — the same grammatical species as `owned` on a return or `scoped` on a parameter, all declared, none derivable from structure."* So `retains` is not a §3.6 derived type property; it would be a **derived member fact**, a new kind. §3.6's closing sentence (*"New rules … that need a structural classification of types must define it as a derived type property"*) does not reach it, and a proposal would have to say so rather than borrow §3.6's mechanism by analogy. This study treats it as a **per-body fact** in proposal 0105 §1's sense: a fact each body yields at its own single check, consumable without re-elaborating the body.

**Body locality.** Under 0105 §1 the consumer classification is **fact-discharged**: the caller-side rule consumes one per-body fact (`retains(M, i)`) computed at `M`'s own check, carried in the interface, never re-derived per use. That is the sanctioned shape — the same shape §22.13's static-initialization graph and §17.14's folded `const` values take — and it is what distinguishes this study's mechanism from a re-elaboration of callee bodies. The invariant is not amended.

## 2. The caller-side warning, precisely ▲

At a call site of `M` with `retains(M, i)`, whose argument at `i` has a source set `S` (K2), resolve a **holder** `H` by §28.14's bounding-object forms, reused verbatim at a *plain* call site:

1. a constructor call → the constructed object;
2. an `owned`-returning call that is the initializer of a `using var`/`await using var` local → the returned object;
3. an `owned`-returning call directly assigned to an `owned` field of `this` → `this`;
4. a member chain rooted at `this` or a local in which every hop is an `owned` field → the chain head;
5. otherwise → the receiver.

v1 conservative scope: **forms 1 and 2 only, and only where `H` is bound to a `using var`** (or is `this` under form 3, where the holder's life is the enclosing object's — the `readonly` field on the caller's side then makes it certain again). Every other resolution admits no warning.

**The predicate.** Warn iff there is a source `s ∈ S` that is a local or parameter of the calling body such that a **kill of `s` dominates a point at which `H` is live**, where a kill is:

- a K4.2 site-announced kill of `s` — `s.Dispose()`, `_ := s;` (the §28.2 dominating-kill shape, *"an explicit `Dispose()` call — or a discard of the binding — on the same variable that dominates the use site on every path"*); or
- a consume of `s` — `move s` into any K3 position (which the freeze does *not* reject, because the borrow lives in `H`'s field, not in a local borrow binding — K5.6: *"fields and collection slots are not frozen"*); or
- `s`'s **scope end** while `H` is live — which, for two `using var`s in one scope, is exactly §28.14's **declared-before** test: `H` declared before `s` means `s` is disposed first (reverse declaration order, §28.10), so `H` is live at `s`'s kill.

A point at which `H` is live is any program point before `H`'s own discharge (its `using var` scope end, its explicit disposal, its `move`). The dominance test is the one §28.2 already performs for use-after-dispose (*"dominates the use site on every path"*), with `H`'s liveness range in place of the use site.

**The diagnostic** names the holder, the retained parameter, the source, the kill, and the remedies: declare `H` before `s` (reorder the `using var`s); make the callee's parameter `scoped` so the compiler proves it; store a `SlotHandle` key (stdlib §4.21) or a `Shared<T>` handle instead; transfer ownership.

**Traced.**

```bezel
using var c := new Customer();          // s
using var a := new Account(c);          // H — form 1; retains(Account.ctor, 0) — c declared before a: silent
c.Dispose();                            // K4.2 kill of s while a is live — WARN (certain: a._owner is readonly)
a.Report();                             // checked mode: DisposedAccessException — the warning was right

using var a2 := new Account(c2);        // H
using var c2 := new Customer();         // rejected already: c2 used before declaration
                                        // (the declared-after shape cannot be written in one scope)

do
  using var acct := new Account(c);     // H inside a block; c outside — acct dies first: silent
end;

var h := new Account(c);                // plain var holder: H's life is until scope end or move — silent in v1 (not a using var)
Publish(move h);                        // h moves out; c's later kill is not comparable — silent in v1
```

The last two rows are what "conservative" costs: the plain-`var` holder and the moved holder are real hazards the v1 predicate cannot rank as certain, and it stays silent rather than guessing.

## 3. Drift

A body edit flips the bit. Two directions:

- **false → true** (the callee starts retaining). Every caller whose kill order was silent before may now warn. No conforming use stops compiling: §2.8.6's *preserving* is *"every use … remains conforming, and every fact a consumer could soundly rely on still holds"* — but a fact **is** withdrawn: "this callee does not retain parameter `i`" was true and is now false, which is the exact shape of §2.8.6's *"derived-property verdict lost"* row, classified **breaking** on §3.6 rule 3's ground (*"a breaking change for clients relying on it, exactly as a signature change is"*). What saves it from *breaking* here is that no client can *rely* on the fact in a way the compiler enforces — the only consumer is a warning. So the honest classification is a new row: **retention gained — preserving, informational**, in the shape of §2.8.6's existing *"carried generic bodies … changed, contract records unchanged — preserving, informational"* row, whose note is *"the regenerable interior: no declared contract changed."* The proposal would add that row.
- **true → false** (the callee stops retaining). Warnings disappear at callers. Strictly preserving; nothing withdrawn that anyone could act on.

Compare trivial-dispose: *gained* is preserving (more admitting contexts), *lost* is breaking (a `_ := new C();` that ran no user code now runs a destructor; a must-use warning appears — §28.9's *"source-affecting by design"*). Retention is the mirror: *gained* is the direction that adds diagnostics. Neither direction is "preserving" by §2.8.6's letter for the same reason — a body change producing a new diagnostic at an unchanged call site is precisely what rationale §9.7's third bullet objects to: *"an inferred fact would make module interfaces change when bodies change — the instability §2.8.2 exists to prevent."* The warning-only consumer softens the objection (nothing breaks) without dissolving it (projects that treat warnings as errors — a policy the spec's §28.3 explicitly leaves to projects, *"a project may tune or silence it"* — would break exactly as under an inferred obligation).

## 4. Interaction with `scoped`

At a `scoped` parameter retention is **declared**, the caller's argument is **checked** against the bounding object, and the callee's stores are **verified** (§28.14: *"These obligations are compiler-verified for user-authored callees"*). The derived bit is redundant there: a `scoped` parameter that the body stores has `retains = true` by definition (a), and one the body never stores has `retains = false` while still declaring `scoped` — legal (§28.14 says the callee *may* store) and harmless (the declaration is the stronger fact; the caller-side check runs on the declaration, not the bit). The rule a proposal would need: **the derived bit is never computed for a `scoped` parameter** — the declaration owns that position — and the caller-side warning never fires where the `scoped` admission already decided the argument (it either admitted a provably-outliving source or rejected). So the two mechanisms partition the parameter positions: declared where the author wrote `scoped`, derived where they did not.

That partition is also the mechanism's strongest argument against itself: **every site the derived bit would warn at is a site where writing `scoped` gives a proof instead of a warning, at the cost of one keyword.** Derived retention is a derived shadow of `scoped` for authors who did not write it — which is exactly the "observation callers silently come to depend on" rationale §9.7 declined for purity, with "depend on" weakened to "expect warnings from".

## 5. The three records it has to answer

**Rationale §9.7 — `pure` is declared, not derived.** The three reasons: (1) the species — a member's behavioral contract, declared like `owned`/`scoped`; (2) *"inferred purity would be an observation callers silently convert into an obligation — adding one log line to a leaf method would flip the inferred purity of every transitive caller, breaking every pure-context use"*; (3) interfaces changing when bodies change. Against a warning-only consumer: (1) applies in full — retention *is* the species `scoped` belongs to; (2) applies in its structure (a leaf edit flips callers' diagnostics) but not in its consequence (nothing stops compiling; the flip is one hop, not transitive, because `retains` reads only direct stores into `this` and forwarding to `scoped`); (3) applies in full at the interface — the bit is body-derived and interface-carried. Verdict: the record's reasons are weakened by the warning-only consumer, not answered; a proposal would have to add a carve to §9.7 ("declared for obligations; derived facts may be carried for diagnostics only") and defend it as a class, since the next derived-for-diagnostics fact would cite it.

**Proposal 0048 — the retired static-return heuristic.** Retired because a plain class-typed return meant two different things keyed on a fact outside the ownership vocabulary (`static`), so *"change a method between instance and static and every call site silently changes meaning"* (0048 §1.2), and because the heuristic *"silently rewrote the caller's obligations"* (0048 §1.3). Does a warning-only consumer escape it? Half. The **meaning** half — call-site classification keyed on the callee's shape — is escaped entirely: `retains` changes no classification, no obligation, no verdict. The **refactoring-seam** half is not: a body edit in one module changes what another module's build prints, keyed on a fact no signature shows, and a reader of the caller cannot find the fact by reading the signature — the same "rule users must memorize rather than read" 0048 §0 names, at warning severity. 0048's remedy was to put the fact in the signature (`owned`); the analogue here is `scoped`, which already exists.

**Rationale §5.28 — the lint doctrine.** Warnings are for *"a statically-certain hazard emitted silently"*, keyed on classifications the compiler already computes, naming the boundary event and the remedies, no new syntax. The v1 predicate of this study's §2 meets the certainty bar (`readonly` field, dominating kill, `using var` holder) and the remedy bar; it fails the *"already computes"* clause on the caller side — the bit does not exist today and must be minted, carried and read — and it is the first lint whose trigger is a cross-module body fact. The doctrine's three shipped instances are body-local.

## 6. What the study cannot settle without an implementation probe

1. **Whether the callee side already yields the fact.** `checkBorrowStoreEscape`'s `Dest::Field` leg (`src/ownership/Ownership_Checks.cpp:7329`, the admitted parameter/receiver branch at `:8070`–`:8077`) knows at each store that the source is a parameter; whether a per-method bitset can be collected there in one pass, including the `readonly` qualification and the `scoped`-forwarding case (`scopedCalleeScan`, `:18595`), is an implementation question this study cannot answer by reading.
2. **Interface carriage.** `src/module/ModuleMetadataBuilder.cpp`/`ModuleLoader.cpp` carry signatures, constraints (`ModuleLoader.cpp:3345`–`:3356` parses the constraint kinds), contract records and the §3.6 verdicts; a per-parameter bit is a new record kind and the 0104-style ABI question (does an added record change artifact identity under §2.8.3 rule 3?) needs a probe, not prose.
3. **The false-positive rate.** Zero by construction on the v1 predicate (every warning is a certain dangle), but the *hit* rate is unknown: how often does a `using var` holder outlive a `using var` source's explicit kill in real code? `grep -rn 'private _owner: \|private _parent: \|private _host: ' tests/ docs/` → 12 back-reference fields; `grep -rn 'scoped [a-z]*: [A-Z]' tests/ docs/` → 33 `scoped` class-typed parameters; `grep -rn 'private readonly _[a-z]*: [A-Z][A-Za-z]*;' tests/ docs/` → 0 `readonly` non-owned class-typed fields written in that spelling. The last number is the study's most useful finding: **the `readonly` plain field the v1 predicate keys on is not an idiom the tree currently writes**, so v1 as scoped here would fire nowhere today. A probe over the corpus with the bit computed for *mutable* plain fields too would say what a wider (uncertain) predicate would find.
4. **The dominance analysis cost.** §28.2's use-after-dispose warning already computes dominance of a kill over a use for one binding; extending it to "dominates a point in another binding's live range" is a small generalization or a new pass depending on how the existing one is written — a probe.
5. **The delta-classification row's consequences** for the `identical`/`preserving` machinery in any tooling that computes §2.8.6 verdicts (none ships).

## 7. Recommendation

**Defer.** Do not proceed to a proposal in this train; do not decline.

- **Why not proceed.** The property is sound and the warning is certain only in a v1 shape (`readonly` plain field, `using var` holder, forms 1–2) that (a) the tree does not write today (this study's §6 item 3 — zero sites), and (b) is exactly the shape `scoped` already makes *static* for the price of one keyword, with no drift, no interface record, no §9.7 carve and no new derived-fact species. In that shape the mechanism's entire payoff is catching an author who did not write `scoped` at a caller who then disposes the source early — a real defect class, but one that proposal 0131's report mode now names at the *callee* (the store site), which is where the fix (`scoped`) is written. The study cannot show that the remaining caller-side gap is worth a second mechanism with a recorded record against it.
- **Why not decline.** The objections in this study's §5 are weakened, not decisive, for a warning-only consumer; the body-locality classification is clean (fact-discharged); and the one thing that would make the case — a measured count of caller-side certain dangles in real programs — does not exist yet because the compiler has no way to produce it. Declining on a count of zero taken from a corpus that cannot exhibit the shape would be declining on the absence of evidence.
- **The evidence that would move it to a proposal**, in order: (1) proposal 0131 landed and run over the tree and over the first real programs, with the Family A count and the fraction of those sites whose authors then wrote `scoped` — if authors do not adopt `scoped` where the report names it, the caller-side warning is the next lever; (2) an implementation probe of this study's §6 items 1, 2 and 4 showing the bit is one pass to collect and one record to carry; (3) a ruling on the §9.7 carve — that a body-derived fact may be interface-carried for diagnostics only — taken on its own merits as a class, not smuggled in with the first instance.
- **Reopening bar.** Any one of: a filed defect where a caller disposed a source under a live holder that `scoped` would have caught and 0131's report did not surface at the callee (because the callee was in a module the caller does not build); or the measured 0131 count exceeding the `scoped` count in the tree by a margin the owner judges to mean the declared spelling is not being taken; or the streaming-compiler tier (0103 §7 item 4) opening, at which point per-body facts in the interface become a designed surface rather than a one-off.

## 8. Interactions

- **Proposal 0131** (this train) — the callee-side half of the same investigation; its Family A report is what makes this study's deferral safe: the admission is no longer silent anywhere, only its caller-side consequence is.
- **Proposal 0130** (this train) — the remedy the warning would name (`SlotHandle` keys); untouched by this study's outcome.
- **§28.14 / axiom A11** — the bounding-object forms would be reused, not extended; A11 is not amended by a study.
- **Proposal 0105** — the fact-discharged classification is the mechanism's only clean line; the study relies on it and records that the classification is what keeps this from being a re-elaboration.
- **Proposal 0048** — the retired heuristic's refactoring-seam objection survives at warning severity (this study's §5); the meaning-change objection does not apply.
- **`DECLINED.md`** — no row owed: a study that defers records its bar here and in the README's live-queue row; a row is added only if the owner declines.
