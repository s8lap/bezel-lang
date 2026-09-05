# Proposal 0076 (RFC) — The error-model study: is `Result<T, E>` better than exceptions as Bezel's general-purpose error mechanism?

| | |
|---|---|
| **Status** | graduated-v0.229 — **superseded — criterial rule graduated v0.229; R1–R6 ruled 2026-07-29** (accepted as recommended; recorded in the v0.229 changelog entry) |
| **Tracking issue** | — |
| **Target** | doctrine (spec §22.16/§31, stdlib §1, Guide 2 §6) |
| **Written against** | spec/stdlib **v0.228** (Guides re-validated at v0.227) |
| **Origin** | Owner-commissioned study (2026-07-28): arrive at *Bezel's own* answer to the Result-vs-exceptions question; if the answer is maximal Result, say what happens to exceptions; and honor the constraint that **no two mechanisms may address the same problem**. |
| **Design character** | **RFC/study** (the 0020/0061 lineage) — frames the question, prices the candidates, drafts the landable rule, and ends in owner rulings (§8). Nothing here amends a normative document directly. |

*Reference convention: unprefixed §-references are language spec references; stdlib, guide, rationale, and proposal references are marked. Citations pin the revision per house rule (e.g. "spec §22.16, v0.228"). ▲ marks a hypothetical API or spec surface that exists in no landed or proposed document.*

---

## 0. The question, the method, and the status of the evidence

**The question, as commissioned.** Is `Result<T, E>` better than exceptions as Bezel's *general-purpose* error mechanism — answered for Bezel specifically, not by imported folklore from Rust, Go, or C#? If the answer is "maximal Result," what happens to exceptions? And whatever the answer, the design must respect the commissioned constraint: **one problem, one mechanism** — no two mechanisms addressing the same problem.

**The method.** Three evidence passes, run in sequence:

1. **A fault-surface inventory** of the entire v0.228 stdlib plus every accepted and draft proposal: every API family that declares a fault contract, classified against the recorded failure-signal partition, with band sizes and a misfit list (surfaces whose current spelling violates the partition's own criterion).
2. **Dual implementations**: the same three realistic programs (an HTTP endpoint handler, a file-processing CLI, a config parser) written under three models — current-idiom Bezel, a Result-maximal Bezel with hypothetical Result-typed stdlib duals, and the latter plus a sketched propagation operator — with per-moment comparisons and ownership traces on every error path.
3. **An adversarial audit** of passes 1 and 2, which found the skeleton sound and several load-bearing details wrong. **The audit's corrections are folded into this study as binding**; where this document differs from what a reader of the raw evidence files would conclude, the difference is deliberate.

**The status of the evidence — stated plainly, because the project's own gates demand it.** The case studies are *authored programs*: programs written to compare models, not programs observed under real workloads. That is **predicted evidence** — exactly the class of evidence proposal 0061 §2's gate declines to act on for the propagation operator ("Gate: revisit when real `Result`-using code exists and the per-call ceremony is observed, not predicted", proposal 0061 §2), and exactly the class 0075 §4.2 re-armed the gate against ("revisit when `Result`-using code written **with** stages 1–2 available still exhibits per-call propagation ceremony", proposal 0075 §4.2). This study therefore uses the evidence for what it *can* ground and refuses to use it for what it cannot:

- It **grounds the criterial rule** (§5) and the **per-family duals decisions** (§5's applications) — neither is behind an evidence gate; both are doctrine-classification questions the inventory answers directly.
- It does **not** ground the propagation operator. The operator stays behind 0061 §2's gate as re-armed by 0075 §4.2, and ruling R5 asks the owner to confirm exactly that. Where §3 and §4 describe what the operator would change, those descriptions are the *predicted* shape of the world — useful for pricing candidates, inadmissible for activating the gate.

Three issues were filed from the audit's spec findings and are engaged below: **#2255** (the `Task`×move-only-value soundness gap), **#2256** (§22.15's closed pure-construction list omits choices; the pure return restriction misses owning choices), **#2257** (§21.7 compound-statement totality clarification).

---

## 1. The partition as it stands

Bezel already has an answer to most of this question, and it is *normative*, not just pedagogical. The normative home is **spec §22.16** (v0.228), inside the `requires` machinery:

> "The boundary with *expected failure* is Guide 2's rule, unchanged and load-bearing: input whose invalidity is a normal outcome at a trust boundary is handled with Try shapes, nullable returns, or `choice` returns, never with `requires`; a `requires` states what a correct caller already guarantees, and its violation is a bug report, not a result." — spec §22.16, v0.228

Guide 2 §6 is the teaching mirror of the same partition: its failure-signal table routes plain absence to `T?`, owned absence to the Try shapes, "failure is expected *and carries information*" to a `choice` return (`Result<T, E>` or a domain choice), caller bugs to `requires`, state misuse to `InvalidOperationException`, and pre-contract boundary input to the residual `throw new ArgumentException` idiom (Guide 2 §6, v0.227). Guide 2 §2 supplies the two implicit bands: environmental faults as ordinary catchables and the fatal band ("Two resource-exhaustion faults are deliberately unrecoverable — no `catch` will ever handle them, and both terminate the process", Guide 2 §2, v0.227).

**The inventory's measurement of that partition across the whole corpus** (~110–120 fault-declaring API families, stdlib v0.228 plus accepted/draft proposals):

| Band | Content | Size |
|---|---|---|
| (i) Defect | caller bugs and state misuse: contract violations, index/overflow/null faults, `Argument*`/`InvalidOperationException` surfaces | ≈ 50–55 families (~45%) |
| (ii) Expected absence | the value tiers: `T?`, the four Try shapes (stdlib §1), the `*OrNull` family, 0072's convenience tier | ≈ 30 families (~25%) |
| (iii) Expected failure with information | `Result<T, E>` (stdlib §2.20) — **zero stdlib producers**; populated only by user idioms and the documents' own examples | 0 producers |
| (iv) Environmental | the `IOException` family, socket/process/compression taxonomies, `TimeoutException`, cancellation, `DomainFaultedException` | ≈ 20–25 families (~20%) |
| (v) Fatal | allocation failure, `StackOverflowException`, `Debug.Assert`, §22.13's abnormal-termination list | 4–5 mechanisms |

**The contested territory is ≈ 10% of the fault surface** — roughly ten families that satisfy band (iii)'s criterion (expected, information-bearing) but are spelled as exceptions today — concentrated in three clusters:

1. **Decode/deserialize of untrusted input**: the serialization read side (`SerializationException`, stdlib §2.15), `Encoding.GetString`/`GetBytes` (stdlib §3.2), one-shot decompression's corrupt-vs-over-cap distinction (proposal 0069 Q6), URI query/component decoding (proposal 0070).
2. **The environmental frontier**: socket connect faults (proposal 0007) and process spawn faults (proposal 0011), whose own prose calls each condition "an *expected* condition a caller branches on" while shipping catchable types — plus the tz-lookup dual (proposal 0012) whose Try form conflates the two causes its exception types distinguish.
3. **Channel lifecycle**: `Send` on a closed or full channel (stdlib §6.9) — engaged below with the audit's key inversion: this cluster's misfit is an **ownership** problem that proposal 0060 already owns, and a Result spelling would make it *harder*, not easier (§2(e′), §5).

Two facts about the uncontested bands anchor everything downstream. First, the defect band is the *bulk* of the fault surface, and it is delivered *by the exception mechanism* — so "maximal Result" can never mean "no exceptions"; it can only mean evacuating bands (iii)/(iv). Second, band (ii) is already value-shaped and actively growing (v0.171 Try codification → v0.184/v0.195 shape additions → v0.215 convenience tier → proposal 0072); `Result` adoption for information-free absence would be a *second spelling* of `T?`, the exact ground on which `Option<T>` was declined ("two spellings of absence, two conversion questions at every API boundary, and an eternal style war", proposals DECLINED.md D-0004).

---

## 2. The structural facts, as corrected by the audit

These are the facts about Bezel-the-design — not about any authored program — that decide where each mechanism can and cannot go.

### 2(a) The borrow-scoped catch: what it actually costs, and what it does not

Spec §31.1 (v0.228): "Inside a catch arm, the bound exception variable (`var ex: Exception` in the arm pattern) is a **borrow** of the in-flight exception; its source is the runtime, and its lifetime is the arm body's." Preservation past the arm is by copy-out: "copy the relevant data out of it (typically `ex.Message`, `ex.StackTrace` — a `string`, §31.4 — the `CauseChain` snapshot, and any domain-specific properties) into separate owned values" (spec §31.1, v0.228) — note the last clause: *domain-specific properties* are copy-out material by the spec's own design.

What this **actually costs**:

- **Type-identity dispatch is lost at the arm boundary.** Past the arm, the failure is whatever values were copied out; nothing downstream can `case` on the exception's type with exhaustiveness. The full pattern-match experience — value patterns, exhaustive `case`, break-on-new-case — exists only on the `choice` side (`catch` is deliberately a *weaker* `case`: type-only arm patterns — "A non-type `catch` arm pattern (a literal, a range, a record-destructuring) makes no sense and is a syntax error", rationale §11.3 — no exhaustiveness, borrow-only binding).
- **Copy-out is manual and per-type.** Every exception type whose domain properties matter needs its own copy-out code at every preservation site; the base `ExceptionInfo` snapshot carries type name, message, and trace only.
- **Class-typed payloads are genuinely trapped.** A borrowed instance's class-typed property cannot be copied out as a value; only value/string/snapshot state escapes losslessly.

What it does **not** cost — two claims the raw case studies overstated, corrected here:

- **"Exceptions force lossy snapshots" is false for well-designed exception types.** A user exception carrying typed *value* properties (a `ConfigException` with `Line: int`, `Field: string`) loses nothing on copy-out; the loss is ergonomic (manual copying, no dispatch), not informational. And §31.1's own design guidance — "Exception classes should therefore carry owned or snapshot state" (spec §31.1, v0.228) — means an exception **may** carry an owned aggregate (e.g. `owned Errors: Array<ConfigError>` ▲ as a user type's field): the ownership model does not forbid an exception from transporting a collection of typed errors.
- **"Exceptions cannot express error accumulation" is therefore refuted as an ownership claim.** The honest anti-exception accumulation argument is **control flow**: a `throw` unwinds — the parser that throws at the first bad field *cannot continue collecting* the rest. Accumulation requires the producer to keep running past the first failure, which is precisely what a value return permits and unwinding forbids. That argument survives; the ownership version of it does not.

This is also where DECLINED.md's value-form-`try` decline must be engaged **on both of its grounds**. The decline (2026-07-21 expression-forms review, recorded under D-0004's entry block) rejects `(try E catch …)` because "it makes exception-based *expected* failure ergonomic, precisely the path Guide 2 §6 routes to `T?`/Try/`Result`; the doctrine would be undermined by its own syntax, **and the borrow-scoped catch model (§31) offers no owned exception value to carry anyway**." The second ground is a fact about this section: there is no first-class caught-error *value* in Bezel, by design — the arm-lifetime rules stay closed, snapshot chaining stays the only escape, and any candidate that would need to carry a caught exception as a value is stillborn. That cuts both ways: it is a real ceiling on the exception mechanism's expressiveness (band (iii)'s capability cannot be retrofitted onto `catch`), and it is a load-bearing simplification the design paid for deliberately (no `AggregateException`, no live `InnerException`, no exception-ownership machinery — spec §31.4's snapshot-chaining rationale).

### 2(b) Defects are welded to `throw` — structurally in part, by design in part (keep the two distinct)

**Structurally welded:** contract clauses raise `ContractViolationException` "in every build mode" and `throw` is rejected *inside* clauses ("**`throw` is rejected** (a clause states a proposition; the mechanism, message, and blame of a violation belong to this section's machinery…)", spec §22.16, v0.228) — the defect band's flagship cannot be re-mechanized. The pure fragment's sole failure channel is the throw carve-out (spec §22.15: without it "the validation idiom … would be unwritable in the entire pure fragment"), and a pure member cannot return an owned class ("an **`owned` class-typed return is rejected**", spec §22.15, v0.228) — so class-payload `Result` is unspellable from pure code. (Two seams at this weld are now filed: §22.15's closed construction list — "Value, record, enum, and string construction" — never names choices, so whether a pure body may construct `Ok(n)` at all is formally open, and the return restriction misses owning choices — **#2256**. The study's recommendation there matters to this document: if choice construction were *rejected* in pure bodies, `Result` would be unusable in the pure fragment, which no party wants.)

**By design, not by structure:** the ~30 hand-thrown `Argument*`/`InvalidOperationException` surfaces (negative magnitudes, bad clamp bounds, state misuse, …) are exceptions because a `Result` signature for them "would lie about its domain" — encoding a caller bug as a returnable outcome misdescribes the contract. This is an *argument*, and a good one, but it is revisable in a way §22.16's weld is not. The distinction matters for §4's pricing: maximal Result's true untouchable core is the structural weld plus band (v); the by-design band stays with exceptions because the criterial rule (§5) says so, not because the type system does.

### 2(c) The owning-instantiation cliff, the inline-array escape, and the sendability limit

An owning `Result` instantiation (class payload in either slot — §35.4 rule 1) is a move-only value consumable **only by owning match**: no `Unwrap` ("extraction is consumption, and consumption is the owning match's job", stdlib §2.20, v0.228), no combinators ("`Map`/`AndThen` combinators are deliberately absent in this revision (over owned payloads they need ownership-polymorphic function parameters — the retired-and-deferred territory of spec §14.4)", stdlib §2.20), and 0075 stage 2 deliberately excludes it (proposal 0075 §3.4's recorded asymmetry). Every resource-acquisition dual (`Result<Connection, E>` ▲, `Result<FileStream, E>` ▲) and every growable error aggregate (`Result<Config, Array<ConfigError>>` — owning *necessarily*: a marked payload makes §35.4 rule 2's borrow-bearing classification unreachable, and `FrozenArray` is a destructor-bearing class, so no growable spelling escapes) sits on this cliff.

**The escape the raw evidence missed:** a **capped** accumulation payload can be value-like. `Result<Config, [8]ConfigError>` — an inline array of value-record elements — is a plain copyable value type (spec §15.1/§15.3: inline arrays without the `owned` marker remain copyable; §35.4 rule 2 classifies inline-array payloads as borrow-bearing only for *reference-typed* elements). It rides 0075 stage 2's combinators and costs a cap plus a count field — assuming the success payload (`Config` here) is itself value-like; a class-typed success payload makes the instantiation owning via §35.4 rule 1 regardless, closing the escape. The accumulation cliff is real for *growable* payloads only.

**The sendability limit:** "Owning choices are not directly sendable" (spec §35.12, v0.228). So the cross-domain argument for Result — errors as first-class sendable data — holds **only at value-like instantiations**. On the environmental/resource band, where payloads are owned resources, a `Result` is exactly as domain-confined as the exception it would replace.

### 2(d) The async container wall (#2255) — Result-typed resource acquisition has no landed container

`OwningTask<T>` is declared `where T: class` (spec §30.3, v0.228). §30.5's producer-side taxonomy has exactly three admitting rows, none of which soundly carries a **move-only value** result: the value/string/runtime-managed row's own guarantee is "no ownership obligation crosses the await boundary," and §30.4's multi-award rule ("A `Task` or `Task<T>` may be awaited from multiple places… Every awaiter sees the same eventual state") contradicts a move-only result outright. §35.4 explicitly handles the borrow-bearing-choice×async interaction (forbidden as an `async` parameter or result, via §30.1) and is *silent* on owning choices — evidence the case was never designed. Meanwhile the implementation admits by head name only, so `async method F(): Task<Result<Connection, IoError>>` appears admitted today: a potential soundness hole (multi-award × move-only), **filed as #2255** with recommendation (a) — reject move-only value results in all three rows now, record the single-award container as its own future proposal.

Consequence for this study: **an async Result-typed resource acquisition cannot be spelled in v0.228 at all** — not with any number of stdlib duals. `Http.GetR(...): OwningTask<Result<HttpResponse, HttpError>>` ▲ is not an API addition; it is a spec-surface addition (a new single-award container admission), plus — a further audit finding — an extension of §35.5's owning-match trigger list, which today names fresh constructions, owning-choice-returning calls, and `move` results, not `await` results. Maximal Result's environmental wing is blocked *here* before ergonomics are even discussed.

### 2(e) The corrected fan-out story

The raw case studies credited "gather all outcomes" to Result alone and exhibited a Result-side dynamic fan-out that does not actually work. Corrected, per the audit:

- The exhibited dynamic build — `tasks.Add(FetchNameR(id))` into a borrowing `Array<Task<…>>` — is the exact shape stdlib §6.7 documents as broken: "`tasks.Add(GetXAsync(url))` abandons the freshly-minted task at end of statement (the #1813 seam)". And the #1813 ruling (2026-07-24) **declined** any borrowing-family dynamic-build facility: "dynamic fan-in over freshly-minted tasks is an ownership-transfer scenario by construction, so the owning forms are its spelling, and no borrowing-family dynamic-build facility is planned" (stdlib §6.7, v0.228). The owning forms (`WhenAllOwned`/`WhenEachOwned`) are `where T: class` — which a value-like `Result` is not. **Net: dynamic fan-out of value-like Result-returning tasks has no landed spelling at all**; the landed Result fan-out story covers only *static* fan-out over named task bindings.
- The exception side is *stronger* than the raw study said: `Task.WhenEach` (v0.212, graduated from proposal 0023) is precisely the gather-per-task capability the study credited only to Result — "**Faulted and cancelled tasks are yielded, not skipped**: the fault or `OperationCanceledException` surfaces when the caller awaits the yielded element — the caller decides per task, unlike `WhenAll`'s first-fault re-raise" (stdlib §6.7, v0.228). Per-task outcomes still degrade to arm-scoped borrows at each catch (the §2(a) costs apply per element), but "the exception model cannot gather" is false as stated.
- What survives for Result: at *static* fan-out of value-like results, `Task.WhenAll<Result<T, E>>` composes today with no new surface, the aggregate never faults for a domain reason, and one exhaustive `case` per element consumes typed outcomes — genuinely better than the per-element try/catch flattening. What survives for exceptions: concurrent *resource* acquisition (`WhenAllOwned`/`WhenEachOwned` + Pattern B) is exception-side-only today, by (d).

### 2(e′) Channels: the misfit is ownership, and 0060 already owns it

Stdlib §6.9 records *why* `TrySend` doesn't exist: "the failure path would need to hand the owned item *back* — an ownership-protocol design of its own" (stdlib §6.9, v0.228). Proposal 0060 consolidates the give-back protocol, and its §3's current lean for the send outcome is `choice SendOutcome<T> { Sent; Declined(owned item: T) }` — an **owning** choice, i.e. a value that lands squarely on §2(c)'s cliff (match-only consumption) and §2(d)'s wall if it ever crosses `await`. The audit's inversion, adopted here: the channel cluster is not evidence *for* Result adoption — a `Result`-shaped `TrySend` would inherit every give-back question plus the owning-instantiation ceremony. It is evidence that **ownership-led design, not mechanism substitution, resolves this cluster**, and it stays with proposal 0060 (§5's application table records this).

### 2(f) The corrected catch census

The documented-corpus catch sweep, re-counted by the audit: **~16 distinct documented idioms** (not 14). The direction survives — the census is dominated by boundary/supervision shapes (catch-`Exception`-log-convert-rethrow, snapshot receivers, the §33.3 exit-70 report, `FireAndForget` logging) plus retry/compensation of transient environmental faults. But the raw inventory's sharpest claim — "exactly one data-shaped call-site catch, and it's pedagogical" — does **not** survive: proposal 0001's client tour (0001 §2) discriminates **three** network fault types by type at a call site (`HttpConnectException` / `TimeoutException` / `HttpProtocolException`, each with its own arm), which is the contested band being consumed exactly the way a `Result` would be; and proposal 0049's pre-v0.186 drain idiom catches `ChannelClosedException` as loop exit (superseded by `ReceiveAll`, but documented). So: the catch surface is boundary-dominated, *and* the contested band's call-site-discrimination usage is real where the environmental frontier is consumed — the census supports the catch-at-boundary doctrine (§6) without proving the contested band is never branch-consumed.

### 2(g) The governance asymmetry — a first-order cost of any Result adoption

The exception taxonomy is **governed**. Producer-locality is normative: "an exception is built-in iff its only producer is a built-in operation… Exceptions whose producer requires explicit `import` live alongside their producer in the standard library and require the same import" (spec §31.4, v0.228). Growth is owner-gated in practice: `NotSupportedException` was declined (owner ruling 2026-07-04, recorded at proposal 0007 §9; proposal 0069's restatement: "declined to keep the §31.4 taxonomy small"); `UriFormatException` was declined because "no handler distinguishes 'bad URI' from 'bad input' by type" (proposal 0070 §6; §7 item 5 records the decline).

**A Result error taxonomy has no equivalent governance.** Nothing says where an `IoError`-style payload choice ▲ would live, what import carries it, who gates its case-set growth, or how two modules avoid minting rival error vocabularies for one producer. This is not hypothetical tidiness: the exception taxonomy's discipline is *why* `catch IOException` means something ("the honest 'any I/O — file or socket — failed' umbrella", proposal 0007 §7). An ungoverned payload vocabulary would fragment faster than the exception hierarchy ever could, because choices are closed sums — every case addition is a breaking change at every exhaustive consumer (spec §35.6), which makes growth governance *more* critical, not less. **If any Result adoption proceeds (R2), the governance rule must land with it (R4).** The proposed rule, §31.4 transposed:

> ▲ **Result-payload governance (proposed, for stdlib §1):** an error-payload type (`E` in a stdlib `Result<T, E>` return) is declared alongside its producer and carries the producer's import; error-payload types are value-like unless the owner rules otherwise; one producer family, one payload type — a second module extends by wrapping, never by widening; case-set growth on a shipped payload choice is owner-gated, because every addition breaks exhaustive consumers (spec §35.6).

### 2(h) Performance: the documents state no runtime cost model — and this study imports none

The normative documents specify **no runtime cost model for either mechanism** — no statement that a throw is slower than a return, no statement that a `Result` tag-check costs anything, nothing about unwind tables or branch predictors. The stdlib is explicitly "not normative for performance characteristics" (stdlib §1, v0.228). The only *stated* cost anywhere is **compile-time**: the exception-edge machinery — catchable OOM was refused because it "would put an exception edge on every `new`, every growth, and every frame allocation — a permanent cost in §28.3's exception-path machinery and §28.8.1's throwable-expression analysis" (spec §31.4, v0.228; rationale §4.4). That is a real, named, design-level cost of throwing surfaces — the documents themselves treat exception-edge proliferation as worth designing away — and it is the *only* performance-shaped fact this study admits. Every "exceptions are slow" / "Results bloat the hot path" argument from other ecosystems is folklore relative to this corpus and appears nowhere in the analysis or the rulings.

---

## 3. What the dual implementations showed — stated qualitatively

The audit found the raw line counts unreliable (recounts diverged from the claimed totals in both directions), so **this study quotes no line numbers**. The audited *directions* are:

- Under an honest recount, the **operator variant (C) is decisively cheaper than current-idiom Bezel (A)** on the three programs taken together, and **Result-maximal-without-the-operator (B) is clearly more expensive than A**. That cost ordering — C cheapest, A in the middle, B most expensive — is the study's central quantitative shape, and it is exactly why the operator question dominates the maximal candidate's viability (and why §0's gate discipline matters: this ordering is predicted, not observed).
- **Acquisition prologues favor exceptions structurally, not incidentally.** The current idiom's resource acquisition is one flat `using var` line per resource, with every environmental fault riding the exception edge to a single boundary; the Result dual makes each acquisition an owning match whose `Ok` arm *nests the rest of the program* (§2(c)'s cliff), and the async acquisitions cannot be spelled at all (§2(d)'s wall). Exhibit 1 below.
- **Gathered value errors favor Result outright.** Static fan-out of value-like results (§2(e)) and first-error-free per-element consumption are better in every column — typed, storable, exhaustively matched — and the accumulation moment (collect all config errors, report once) is expressible only where control flow keeps running past the first failure (§2(a)'s corrected ground). The growable-aggregate spelling pays §2(c)'s cliff; the capped inline-array spelling does not.
- **The "silent drop of log faults" finding against Result was a strawman, and it is dead.** The raw study charged the Result-maximal variant with silently discarding log-write outcomes (`_ := WriteLineR(…)`). Under 0075 stage 2 the honest spelling is a one-line `.Map` that *restores* the abort-on-log-fault semantics at negative line cost (Exhibit 3). With it dies one of the operator's three claimed wins — the surviving two are the acquisition prologue and statement-shaped multi-call propagation (Exhibit 2).
- **The typed-error-context advantage the HTTP program showed for Result was authoring, not model.** The current-idiom variant flattened its downstream fetch to `Task<string?>` — but nothing in v0.228 forces that: current Bezel can declare `Task<Result<string, EnrichError>>` (a value-like instantiation rides today's containers) and populate the error case inside the catch arm via §31.1 copy-out, keeping the typed 502-vs-transport distinction without any hypothetical surface. What is genuinely model-owned on the Result side of that program is the transport taxonomy *as data* (an `HttpError` choice ▲) — and its acquisition end hits §2(d) regardless.
- Two ceremony charges against the current idiom were withdrawn by the audit: the assign-then-return dance around trailing `try` (§21.7's silence is generic and the compiler already implements the natural reading — clarification filed as **#2257**), and the `v!` after `TryGetValue` (stdlib §1's shape-(a) `when true` postcondition is normative for every shape-(a) member, `TryGetValue` included — the narrowing makes `!` spurious).
- The exception-maximal foil (the config parser as a `ConfigException`) remains instructive on both edges: its happy path is the cleanest of all variants (propagation costs zero syntax) and nothing forces its caller's `try` (spec §31.3 — a forgotten handler is a silent exit-70 in a service), with the error data readable only inside the arm (§2(a)).

**Exhibit 1 — the acquisition prologue** (file CLI; the study's sharpest structural contrast):

```bezel
// Landed v0.228 — the current idiom: flat, one line per resource; every
// environmental fault rides the exception edge to one abort boundary.
using var files := Directory.GetFiles(indir);
using var log   := new FileStream(Path.Combine(outdir, "report.log"), FileMode.Create);
```

```bezel
// ▲ Result-maximal — GetFilesR/OpenR are hypothetical Result-typed duals
// (owning instantiations, §35.4 rule 1): each acquisition is an owning
// match, and the Ok arm nests everything that follows. Error payload:
// hypothetical ▲ IoError, value-like; its Text() renders the condition.
case Directory.GetFilesR(indir) of                                     // ▲
  Error(var e) => do Console.Error.WriteLine($"fatal: {e.Text()}"); return 2; end,
  Ok(var f)    => do
    using var files := owned f;                                        // adopt (§35.5/§28.10)
    case FileStream.OpenR(Path.Combine(outdir, "report.log"),
                          FileMode.Create) of                          // ▲
      Error(var e) => do Console.Error.WriteLine($"fatal: {e.Text()}"); return 2; end,
      Ok(var l)    => do
        using var log := owned l;
        // … the rest of the program nests here …
      end
    end;
  end
end;
```

```bezel
// ▲▲ The gated operator (sketch only — dual AND operator hypothetical):
// restores the flat shape while keeping the typed signature; disposal on
// the synthesized early-return edge is §28.10's ordinary every-exit-path rule.
using var files := Directory.GetFilesR(indir)?;                        // ▲▲
using var log   := FileStream.OpenR(Path.Combine(outdir, "report.log"),
                                    FileMode.Create)?;                 // ▲▲
```

**Exhibit 2 — the parser extraction tail** (config parser; the domain program Guide 2 §6 *already* routes to `Result` — the A/B axis here is ergonomics only):

```bezel
// Landed v0.228 — one statement-case per propagating call (0061 §3's logged shape):
var host := "";
case Required(fields, "host") of
  Ok(var v)    => host := v.Text;,
  Error(var e) => return Result<Config, ConfigError>.Error(e);
end;

// 0075 stage 1 (proposed): line count unchanged, construction materially narrower —
//   the Error arm becomes `return Error(e);`

// ▲▲ the gated operator: the four-line block becomes a binding —
var hostV := Required(fields, "host")?;    // spelled as two statements, because
var host  := hostV.Text;                   //   `…?.Text` already parses as
                                           //   null-conditional access (§7.5)
```

(The final comment is itself a finding: `?` is lexically taken in **four** postfix-adjacent positions — `?.` §7.5, `??` §7.7, `T?` §7.1, and the null-conditional call `?(args)` §7.6 — so any real operator proposal owes a distinct spelling before it owes anything else.)

**Exhibit 3 — the strawman's one-line burial** (0075 stage 2, value-like instantiation; `WriteLineR` ▲):

```bezel
// The raw study's Result-maximal variant dropped log-write outcomes
// (`_ := WriteLineR(…); return Ok(false);` — a semantics change it then
// charged to the model). The honest stage-2 spelling, one line, restores
// abort-on-log-fault: a log fault propagates as Error; success folds to
// Ok(false) ("this file was skipped").
return WriteLineR(log, $"skip {path}: {e.Text()}")                     // ▲
    .Map(lambda(u) => false);
```

---

## 4. The candidates, priced

### (a) Status-quo-sharpened

Adopt the criterial rule (§5) as normative text; continue the recorded per-surface practice (targeted duals when a workload demands, per the tier system). **Price:** near zero — one ruling-class landing, no new surface. **What it leaves standing:** the decode misfit in full (the deserialization fault's locating information remains reachable only through `Message` text — see §5's M1 discussion), and band (iii) stays producer-empty, meaning the doctrine's third row keeps teaching a mechanism the stdlib never exhibits.

### (b) The hybrid — **recommended**

Candidate (a) **plus**: `Result` adoption scoped to the two clusters where the structural facts all point the same way — **decode/deserialize** (expected failure of untrusted input, information-bearing, consumed by branching, payload spellable as a value-like record/choice) and **gathered value errors** (static fan-out, capped accumulation) — riding **0075 stages 1–2** as the ergonomics substrate, under the **governance rule** of §2(g)/R4, with the **operator staying evidence-gated** (R5). **Price:** the 0075 stages (already proposed on their own evidence); a small number of value-like payload types and `Result`-returning duals in the decode cluster, each a tier addition under the no-dual rule (assert-shaped throwing form stays; branch-shaped Result form is the new tier where ruled in); the governance rule; and the discipline cost of *not* extending Result into the environmental band until §2(d)/(c) change. **What it buys:** band (iii) gets real producers exactly where its criterion bites; the strongest misfit (M1) resolves; nothing touches the async wall, the owning cliff, or the by-design defect band; and the no-dual constraint is *strengthened*, not strained, because each adoption is an explicit tier ruling rather than mechanism drift.

### (c) Maximal Result

Evacuate bands (iii) and (iv) wholesale: every environmental and expected-failure surface returns `Result`; exceptions remain for defects and fatality only. **Structurally blocked today, on five independent grounds:** the async container wall (#2255 — no landed container carries a move-only value result, so every async acquisition is unspellable); the owning-instantiation cliff (§2(c) — every resource payload is match-only, combinator-less, `Unwrap`-less, and 0075 stage 3(a)'s unlock is retired-and-deferred spec territory); the sendability limit (§35.12 — the cross-domain advantage evaporates exactly on the band being migrated); the governance gap (§2(g) — an ungoverned closed-sum error vocabulary at stdlib scale); and **the unpriced migration**: on the inventory's counts, band (iv)'s ≈ 20–25 API families — roughly 200+ individual members on the inventory's estimate — with every signature change rippling through §2.8.2 artifact interfaces, plus the constructor problem (constructors cannot return `Result`; every validating constructor needs a factory dual under 0048's discipline), the property-getter problem, the iterator-element problem, and the operator/indexer problems the inventory catalogued. **No migration or compatibility analysis exists**, and this study declines to invent one; that analysis is the honest missing piece, and producing it is a precondition of ever activating this candidate (§6 records the full reopening bar). Priced honestly: (c) is not "expensive" — it is *not currently constructible*, and its constructible core is exactly candidate (b).

### (d) The revealed alternative: targeted stdlib additions — a real competitor

The corpus's own history shows a fourth candidate that dissolves misfits *without touching the error model*: **`Counter<K>`** (v0.215) dissolved the compound-assignment tallying misfit ("the counting idiom, replacing §4.8's two-lookup dance for tallies", stdlib §4.20); **`ReceiveAll`** (v0.186) dissolved the drain misfit ("`ChannelClosedException` stops being control flow for the *normal* exit", stdlib §6.9); **`WhenEach`** (v0.212) delivered gather-per-task on the exception side; the ***OrNull* tier** (0072) keeps dissolving absence-shaped friction. Track record: four for four, each landing as a small owner-gated addition. **Its ceiling:** it cannot deliver band (iii)'s specific capability — exhaustive, typed, storable branch dispatch over error *data* — because that capability is the `choice` mechanism itself. So (d) competes on the misfit *list* (and wins on several rows of §5's table below: ReadKey, tz-lookup, Min/Max residue) but not on band (iii)'s core. The recommended posture uses (d) as the default remedy for every contested family that does **not** need band (iii)'s capability, reserving (b)'s Result adoptions for the families that do.

---

## 5. The criterial rule — the landable deliverable

The study's real finding is not "Result good" or "exceptions good"; it is that the partition's band-(iii)/band-(iv) boundary has been maintained by fiat ("I/O is exceptional") while the accepted proposals' own prose applies band-(iii) language to band-(iv) surfaces (§1, cluster 2). The fix is a **decision procedure** sharp enough that the next proposal cannot straddle. Proposed as normative text — a new stdlib §1 bullet (the natural home beside the Try convention), cross-referenced from spec §22.16's boundary sentence and mirrored in Guide 2 §6's table.

> ▲ **The failure-signal rule (proposed normative text, stdlib §1).** Every fault **condition** a surface declares is classified by the first matching step; one primary mechanism per condition follows from the step. (One API may declare conditions in different bands — the tz-lookup row below splits absence from corruption — but never two mechanisms for one condition.)
>
> 1. **Caller bug.** The condition is something a correct caller already guarantees (bad argument, protocol misuse, state violation) → a `requires` clause where a contract is possible; the residual defect exceptions (`Argument*`, `InvalidOperationException`) at pre-contract boundaries (Guide 2 §6). A defect surface is **never a recovery surface**: its exception exists to report, not to be branched on (spec §22.16 — "a bug report, not a result").
> 2. **Expected absence, no information.** The failure is expected and carries no information beyond occurrence → the value tiers of this section: nullable return where the payload carries the signal; a Try shape where it cannot or where the bool shape is the idiom (the recorded v0.184/#1535 criterion); an `*OrNull` convenience dual per proposal 0072's tier.
> 3. **Expected failure with information, branch-consumed, value-like payload.** The failure is expected, carries information the caller *branches on at or near the call site*, and the error payload is spellable as a value-like type (§35.4 rule 3 — a record or choice of values/strings; a capped inline array for bounded aggregation) → a `choice` return (`Result<T, E>` or a domain choice), the positive half of spec §22.16's "never with `requires`" clause. The payload type is governed by the Result-payload governance rule [§2(g)/R4].
> 4. **Boundary-consumed fault (the residual band).** The failure's consumption is a boundary, supervision, or retry site — logged, converted to a protocol answer, retried, or reported, rather than branched on for program logic — **or** the condition is band-3-shaped (expected, information-bearing, branch-consumed) but its payload cannot be value-like today (§35.4/§35.12; the §30.5/[#2255](https://github.com/s8lap/bezel-lang/issues/2255) container wall) → a typed exception, producer-local per spec §31.4, **regardless of how much information it carries**, with the classification recorded together with its reason. The typical inhabitant is the environmental fault (I/O, network, OS state) — environmental origin is descriptive of the band, not a conjunct of the test. Information content never forces band 3: a fault may carry rich data and still sit here, because the data is consumed at a boundary that reads it once (the `catch` copy-out model, §31.1), not dispatched on downstream.
> 5. **Fatal.** The process cannot meaningfully continue → termination (spec §31.4's allocation rule; `StackOverflowException`; `Debug.Assert`).
>
> **The band-3/band-4 boundary, stated once:** what distinguishes them is **where consumption happens** (call-site branching vs boundary handling) **and whether the payload can be value-like** (§35.4/§35.12 — a resource payload forces band 4 today by §30.5/#2255 and the owning-match-only consumption rule), **not whether information exists**. A condition satisfying band 3's first two conjuncts (expected, information-bearing, branch-consumed) whose payload cannot be value-like takes band 4's mechanism until the container and combinator story changes — that is band 4's residual clause above, and the recorded reason keeps the classification revisitable. **One carve, named:** a condition whose failure path must *transfer ownership back* to the caller — the give-back class, proposal 0060's `SendOutcome<T>` lean — is neither band's material: its spelling is an **ownership-protocol design** governed by the kernel rules, recorded as such, and the no-dual rule applies to whatever spelling that design fixes.
>
> **The no-dual rule.** One primary mechanism per fault condition. A surface may additionally offer recorded **tiers** — the assert-shaped throwing access ("failure is a bug at this call site") beside the branch-shaped value access ("failure is expected here") — because tiers serve different call-site *stances* of the same operation, not the same problem twice. Anything else — two peer spellings of one stance, a Result dual beside a Try dual for the same branch-shaped stance, an exception and a choice both marketed for branching — is a violation, and the newer spelling is the one that yields.

This rule is deliberately conservative: applied to the v0.228 corpus it re-classifies almost nothing — bands (i), (ii), (v) are untouched, and most of band (iv) stays put *with a recorded reason* instead of a vibe. The reclassification pressure lands exactly on the contested families:

| Family | Rule outcome | Disposition |
|---|---|---|
| **Deserialization read side** (stdlib §2.15; the study's M1) | Band 3 **candidate** — expected at the canonical trust boundary, information-bearing, branch-consumed by services — **once a value-like payload design exists**. The audit's correction reframes today's defect: stdlib §2.15 says `SerializationException` "carries a byte offset (for binary) or line/column (for JSON) to locate the fault," but the type **declares no typed surface** — no class shape for it exists anywhere in the stdlib, and the locating data lives in `Message` text. (Stdlib §2.17's "one built-in carries surface" counting sentence is itself contradicted by spec §31.4's `ArgumentException.ParamName` — filed as [#2276](https://github.com/s8lap/bezel-lang/issues/2276) — which doubles as landed *precedent* for option (i): a built-in exception carrying a typed value property.) So M1 is an **API-completeness exhibit** with two honest fixes: (i) give `SerializationException` typed value properties (▲ `Offset: long?`, `Line: int?`, `Column: int?` — copy-out-friendly per §31.1's own model), or (ii) a `Result` dual with a value-like ▲ `SerializationError` payload at 0073's recorded `TryParse<T>` seam (0073 §3 Q6, restated at §11 Q5). The surviving *uniquely*-Result ground is exhaustive branch dispatch, not information preservation. | **R3**: recommend (i) now — small, benefits every existing consumer, honest under band 4's copy-out model — and (ii) as the hybrid's decode-cluster pilot under R2, landing as a tier (the throwing form stays the assert-shaped access; 0073 §3 Q6's recorded seam is the vehicle). |
| **`Encoding.GetString`/`GetBytes`** (stdlib §3.2) | Band 3 candidate, second in line: the v0.136 decline weighed defined-fault against *lenient substitution* only; the value-shaped third option was never priced. | Follows the decode-cluster pilot's outcome; no unilateral change. |
| **One-shot decompression** (proposal 0069) | The Try dual exists and deliberately conflates corrupt-vs-over-cap ("callers needing to distinguish use the throwing form", 0069 Q6) — a band-3-shaped residue on a band-4 surface. Streaming decompression stays band 4 (mid-iteration faulting — the iterator element problem). | Decode cluster, third in line; the existing tiers stand meanwhile. |
| **URI query/component decode** (proposal 0070) | Criterion-band 3, consumption evidence thin; the main `Parse` already has its Try dual. | Revealed-alternative track: a Try dual if a workload appears; joins the decode cluster only on evidence. |
| **Channel send lifecycle** (stdlib §6.9; M4 as corrected) | **Stays with proposal 0060's ownership-led design.** The misfit is the give-back protocol, not the mechanism; 0060 §3's owning-choice lean (`SendOutcome<T>` ▲) is band-3-*shaped* but owning-classified — its consumption story is §35.5's owning match, and its design budget is kernel rules, not doctrine. This study defers to 0060 wholesale. | No action here; cite this table from 0060 when it graduates. |
| **Socket connect faults** (proposal 0007) | Band 4 by the rule: consumption is boundary/retry (the §2(f) census — even 0001's three-arm call-site catch logs-per-type, a reporting stance), and the payload is inseparable from an owned acquisition (`Connection`) — the value-like condition fails, #2255 seals it. | Stays exception-spelled. **R6**: correct the "errors-are-typed-values discipline of Guide 2 §5" sentence at graduation (below). |
| **Process spawn faults** (proposal 0011) | Band 4, same analysis; `TryStart`'s recorded deferral condition ("addable if a probe-many-executables use case appears", 0011 §5) is the rule's band-2 tier waiting on evidence — exactly right as recorded. | Stays; R6 wording fix at graduation. |
| **Tz lookup** (proposal 0012) | The rule *splits* it: lookup miss is band 2 (expected absence); unreadable/corrupt database is band 4. The current Try dual returns `false` (with the shape-(a) null out) on either condition — conflating the two bands in one signal, which the rule forbids (0012's `TryFindSystemTimeZoneById`; stdlib §1's false-path rule). The model answer is 0071's two-axis split ("Try means 'absent is an expected outcome,' not 'swallow all faults'", 0071 §5): the Try form answers the absence axis only; `InvalidTimeZoneException` propagates. | Re-scope the Try dual to the absence axis at 0012's graduation; flagged for that graduation's checklist (not part of R6, which is 0007/0011). |
| **`Console.ReadKey` at EOF** (stdlib §2.1; M6) | Band 2 by the rule — the same condition on the sibling `ReadLine` *is* band 2. The recorded reason ("`ReadLine`'s null-at-EOF shape has no `char` analog", stdlib §2.1) does not survive inspection: `char?` is spellable, and 0072's convenience tier is the recorded pattern for exactly this shape. | Revealed-alternative track: a `char?`-shaped dual (▲ spelling per 0072's tier conventions; or a v0.195 re-ruling of the primary form). The smallest misfit, the cleanest fix, no model content. |
| **LINQ `Min`/`Max`/`Average` on empty** (stdlib §5; M5) | Band 2 residue. The recorded selector-overload decline **stands**: "There are deliberately no `MinOrNull`/`MaxOrNull` selector overloads — compose `Select(selector).MinOrNull()`" (stdlib §5, v0.228) — composition is the remedy, and the §7.10 nullable-collapse caveat recorded beside it is a real reason, not an oversight. The residual gap is `Average`'s missing `*OrNull` dual — an ergonomics-batch item (the 0062/0067/0074 lane), not a doctrine item. | Decline stands; `AverageOrNull` flagged to the next ergonomics batch. |
| **Parse family** (stdlib §9.1, 0072) | The tier system's model citizen: throwing form (assert stance) + `TryParse` (constraint witness, "load-bearing and stays", 0072 §1) + `ParseOrNull` (composes with `??`). Three tiers, three stances, no violation of the no-dual rule. | The exhibit the rule generalizes from; no change. |
| **`DateTime.Add*`/`FromTicks` range faults** (stdlib §2.5; M9) | Band blur: data-dependent overflow mislabeled `ArgumentOutOfRangeException` (the §9.1 analog raises `OverflowException`). Blame semantics attached to a data condition. | Candidate one-line correction at a future docs sweep; out of autonomous scope (docs changes are owner-workflow), recorded here. **Discharged v0.292** (stdlib §2.5 relabels the fault to `OverflowException`; `DECLINED.md` row; impl tracked). |

---

## 6. Exceptions' disposition under the recommendation

Under the hybrid (R2), exceptions are not a legacy mechanism awaiting removal. They are the **permanent mechanism of three bands**, each for a reason this study has now made explicit:

- **Defects** — structurally welded (§2(b)): contracts, the pure fragment's throw carve-out, the built-in fault surfaces; catchable on purpose (the test oracle of proposal 0022, the boundary converters of 0002, §33.3's exit-70 report) but never a recovery surface.
- **Environmental faults** — band 4 under the criterial rule: typed, producer-local (§31.4), consumed at boundaries by copy-out, retried on transient types; with the concurrent-resource-acquisition machinery (`TaskScope`, `WhenAllOwned`/`WhenEachOwned`, Pattern B) exception-side by construction.
- **Fatality** — termination, uncatchable, per §31.4/rationale §4.4.

**The catch-at-boundary doctrine should be made explicit** where it currently lives only as an emergent pattern: Guide 2 gains a sentence in §6 (alongside the table) stating that `catch` is a *boundary and supervision* tool — convert, log, retry, report, re-raise — and that branching program logic on caught types at interior call sites is the signal to check the failure-signal rule for a band-2/3 tier. The §2(f) census supports this as description, not aspiration: it is what the documented corpus already does, with the environmental frontier's call-site catches (0001 §2) as the recorded exception that the rule now adjudicates instead of ignoring.

**The reopening bar for maximal Result** — recorded so the question closes without being foreclosed. Candidate (c) becomes discussable when **all four** hold:

1. **The operator has landed** through 0061 §2's gate on its own observed evidence (0075 §4.2's re-armed condition), with the four-way lexical collision (§3, Exhibit 2) resolved — because without it, the migration's interior cost structure is the one §3 measured as clearly worse than current.
2. **A single-award async container for move-only value results exists** — #2255's option (b), designed and landed as its own proposal, with §35.5's trigger list extended — because without it the environmental band's async surface is unspellable as Result.
3. **The governance rule (R4) has operated** long enough on the hybrid's payload types to show the closed-sum growth problem is manageable at stdlib scale.
4. **Observed evidence from the hybrid's clusters** — real programs consuming the decode-cluster Result surfaces — shows branch-shaped consumption dominating boundary-shaped consumption on the frontier surfaces (the band-3/band-4 test applied to data, not prediction), plus the migration/compatibility analysis §4(c) names as missing.

Anything less re-litigates this study's inputs without new facts.

---

## 7. Relationship to the recorded declines, gates, and open issues

- **DECLINED.md, value-form `try` (2026-07-21, expression-forms review).** Untouched and *reinforced* — on both grounds. Ground 1 (it would make exception-based expected failure ergonomic, against Guide 2 §6's routing) is now backed by a normative rule (§5) rather than a table; ground 2 (the borrow-scoped catch "offers no owned exception value to carry anyway") is engaged at §2(a) as a permanent structural fact both candidates must design around.
- **Checked exceptions.** No decline exists anywhere in the corpus — a common misremembering worth correcting on the record. The only on-topic line is future-directions-2026-07-18.md:344: "**Checked exceptions** — §31.3 stands. P2's `pure` and (if ever wanted) a `total` qualifier are the verifiable fragments of the idea without the Java failure mode; P1 removes the main pressure to encode expected failure as exceptions at all." This study's criterial rule is that posture made normative: the pressure relief is band 3, not a `throws` clause.
- **0061 §2's operator gate.** Untouched (§0; R5). This study's case-study evidence is the gate's *declined* class and is not offered against it.
- **Proposal 0075.** The ergonomics substrate of the hybrid: stage 1 (return-position target-typing) and stage 2 (value-payload combinators) carry the decode-cluster adoptions; stage 3's two deferrals (ownership-polymorphic parameters; the operator) are exactly the two unlocks §2(c)/(d) identify as the maximal candidate's missing structure. Nothing here pre-empts 0075's own open questions (its §8).
- **Proposal 0060.** Owns the channel cluster (§2(e′), §5); this study defers to its owning-choice lean and adds only the classification note that its outcome type, if adopted, is band-3-shaped but owning-classified — 0060 should cite §5's table at graduation.
- **#2255** — the async wall, filed with recommendation (a) (reject move-only value task results now; container as future design). This study's §2(d), §4(c), and §6's reopening bar all key on it.
- **#2256** — the pure×choice seams. This study additionally registers a stake: rejecting choice construction in pure bodies would make `Result` unusable in the pure fragment; the recommended admission keeps band 3 available to pure code with value-like payloads.
- **#2257** — §21.7 clarification; consumed here only to withdraw a ceremony charge (§3).

---

## 8. Owner rulings requested

Each with the study's recommendation. R1 and R4 are the landable core; R2/R3 set the adoption scope; R5/R6 close loops the audit opened.

- **R1 — Adopt the criterial rule (§5).** Land it as a ruling-class amendment: the failure-signal rule as a new stdlib §1 bullet (beside the Try convention), a cross-reference sentence at spec §22.16's boundary paragraph, and Guide 2 §6 alignment (the table gains the band-3/band-4 boundary sentence and the catch-at-boundary sentence of §6). **Recommend: adopt.** It is the smallest text that stops the frontier drift §1 documents, and it re-classifies nothing retroactively.
- **R2 — The adoption scope: hybrid (§4b) vs status-quo-sharpened (§4a).** Result adoption scoped to the decode/deserialize and gathered-value-error clusters, value-like payloads only, riding 0075 stages 1–2, under R4's governance. **Recommend: the hybrid** — it is the constructible core of the maximal candidate, it resolves the strongest misfit, and it finally populates band (iii) with producers where the criterion actually bites. If the owner prefers (a), the study's analysis stands unchanged; only the table's two "decode cluster" rows revert to waiting.
- **R3 — The M1 resolution: typed `SerializationException` surface vs Result dual vs both-tier.** **Recommend: both, sequenced** — typed value properties on `SerializationException` (▲ offset/line/column) now, as an API-completeness fix that serves every existing consumer under the band-4 copy-out model; the `Result` dual (▲ value-like `SerializationError` payload) at 0073's recorded `TryParse<T>` seam (0073 §3 Q6/§11 Q5) as the decode-cluster pilot, contingent on R2, landing as a recorded tier (assert-shaped throwing form retained) so the no-dual rule is satisfied by stance, not violated by duplication.
- **R4 — The Result-payload governance rule (§2(g)).** Producer-locality for payload types; value-like unless ruled otherwise; one producer family, one payload type; owner-gated case growth (a case addition breaks every exhaustive consumer). **Recommend: adopt with R2** — adoption without governance recreates, in closed sums, the taxonomy sprawl §31.4's discipline exists to prevent; adoption with it inherits that discipline's track record.
- **R5 — Confirm the propagation operator stays gated.** 0061 §2's gate, as re-armed by 0075 §4.2 (revisit only on per-call ceremony observed in code written *with* stages 1–2). **Recommend: confirm.** This study's §3 direction — the operator variant winning the totals — is precisely the predicted-evidence class the gate was built to resist, and the four-way token collision means even an activated gate opens onto a spelling design, not a rubber stamp.
- **R6 — The 0007/0011 wording corrections at graduation.** Both proposals justify their (correct, band-4) exception taxonomies as "the errors-are-typed-values discipline of Guide 2 §5" — transferring the value doctrine's name to the mechanism it contrasts with (0007 §7; 0011 Q6). **Recommend:** at each graduation, reword to the actual ground the criterial rule now supplies: *an expected environmental condition consumed at boundaries is a distinct catchable type, producer-local per §31.4 — band 4 of the failure-signal rule* — so the doctrine's name stops drifting and the next frontier proposal cites the rule instead of the vibe.

---

*End of study. All ▲ surfaces are hypothetical; nothing here amends any normative document. The evidence files (inventory, dual implementations, adversarial audit) are session artifacts; their audited conclusions are incorporated above, and where they conflict with this document, this document is the corrected record.*
