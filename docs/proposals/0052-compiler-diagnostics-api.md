# 0052 - Machine-grade diagnostics and a compiler query API

**Status:** accepted — accepted **staged as designed** (delegated, critic-gated ruling on [#2039](https://github.com/s8lap/bezel-lang/issues/2039), 2026-07-24, spec v0.220): S0 and S1 open after the R0 drain or as one coordinator-designated infra lane; S2 gated on S1 + 0040 Q1; S3 doubly gated (0040 Stage 1 + observed S2 usage). ⚠ This header read `under-review — draft` from that ruling until 2026-09-17 — the ledger lagged the tracker by eight weeks; corrected with the S0 brief below (§9).
**Tracking issue:** [#2039](https://github.com/s8lap/bezel-lang/issues/2039)
**Target:** Tooling only — reference-compiler surface (`bezelc` output formats, a diagnostic-code catalogue, `--explain`, a batch query mode, staged server modes). **Zero normative spec change**, defended in §2.1: Appendix [D](../Bezel_Language_Spec.md#appendix-d-conformance-levels)'s diagnostic-identity latitude (v0.163) is exercised, not narrowed — the code registry is reference-compiler surface, deliberately *not* a spec registry. No grammar change, no stdlib change.
**Written against:** spec v0.197 (stdlib/rationale at the same revision), compiler @ `bf96dd6`
**Currency note:** Refreshed 2026-08-11 to v0.246: transfer-marker consume spellings respelled `move` in the §2 example and the `--explain BZL2803` display (v0.246). Written-against pin unchanged. **Refreshed 2026-09-17 to v0.306** (compiler @ `29410e29`): the seven spec anchors §1.2 cites re-verified present; the `19xx` bucket description no longer names `when` (retired v0.289, 0122 L1); §24 is a tombstone since the `event` retirement (v0.187) so bucket `24` mints nothing and is recorded as reserved-by-history; the §2.3 shared anchor resolver now has a landed seed, `scripts/check_src_citations.py` (#4455, 2026-09-17); the §2.10 workflow gains the `internal:`-site disposition column that family K of the defect campaign needs; and §9 (the S0 implementation brief) is new. Written-against pin unchanged — the design is not revised, only re-anchored and staged for implementation.

This proposal expands item **P12** of [`future-directions-2026-07-18.md`](future-directions-2026-07-18.md) to commissioning depth. Where it sharpens or diverges from the P12 sketch, the divergence is recorded in §8. *Reference convention: an unprefixed §-reference is a language-spec reference; `stdlib §`, `rationale §`, and `0040 §`/`0043 §`/… prefixes are written out; this proposal's own sections are cited as "§N below" or from context.*

## 0. Summary

In an agent loop the compiler is the teacher and diagnostic quality is the reward signal. Bezel's prose diagnostics are unusually good — the spec *mandates* their content in several places (§1.2 below) — but the reference compiler flattens everything it knows into one message string on one source location. This proposal makes that knowledge machine-consumable, in four decided pieces: (1) a **stable code taxonomy** — `BZL` + four digits, bucketed by spec chapter, codes never reused, each carrying spec-anchor metadata — positioned explicitly as reference-compiler surface under Appendix D's identity latitude; (2) a **JSON diagnostics schema** whose ownership rejections carry their *trace* as typed data (leak paths, borrow-source chains, freeze conflicts, merge conflicts, restoration gaps) plus confidence-tagged fixits; (3) **`bezelc --explain BZL2803`** with in-tree content whose spec anchors are CI-checked against the docs, on `scripts/check_docs.sh`'s existing discipline; (4) a **batch query surface** (`--query ownership-state@file:line`, `--query borrow-source@file:line:col`) that serializes facts the ownership pass already computes — v1 is one process, N questions, one JSON answer; an LSP server is staged (S2) and an incremental daemon after it (S3), so v1 stays honest and cheap. Latency targets are stated as engineering goals with an in-tree benchmark, not promises. Near-term beneficiary: this repository's own producer lanes.

## 1. Problem and evidence

**1.1 What the compiler emits today.** The entire diagnostic surface is `struct Diagnostic { Severity severity; std::string message; SourceLocation location; }` (`src/support/Diagnostic.hpp`), printed as `path:line:col: message` (`src/driver/Driver.cpp`). One span, no codes, no secondary locations, no structured payload of any kind — a diagnostic that *knows* three source positions (the §28.3 consume-site freeze names the consumed binding, the borrow, and the borrow's surviving use) serializes them as line numbers interpolated into English. Every consumer — the lit track, the gtest harness, an editor, an agent — gets prose or nothing.

**1.2 The spec already mandates rich diagnostic *content*.** These are normative commitments (all verified against v0.197 headings and text); the compiler must already *compute* everything named, which is the load-bearing fact for the honesty claim of §2.5 below — the structured payloads serialize data the analyses demonstrably have in hand:

| Spec site | Mandated content |
|---|---|
| §28.3 [*Diagnostics*](../Bezel_Language_Spec.md#sec-diagnostics) | "error messages that name the variable, its current ownership state, and the rule violated," plus five recommended messages (drop-without-disposal, borrow-outlives-source, moved-from read, `using var` move, consume-site freeze) |
| §28.3 [*The consume-site freeze*](../Bezel_Language_Spec.md#sec-consume-site-freeze) | "the diagnostic names the consumed binding, the borrow, and the borrow's surviving use" |
| §28.8.1 [*The field-restoration rule*](../Bezel_Language_Spec.md#sec-the-field-restoration-rule) | "identifies the move site, the unrestored exit path, and the three standard remedies" |
| [§30.5](../Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal) | the bare-`await` error "identifies the offending await and references the principle"; the un-consumed `OwningTask<T>` error "names the owned-consuming positions admitted by the rule … and points to the `using var` form" |
| [§29.11](../Bezel_Language_Spec.md#sec-multiple-enumeration) | "names the consumed binding, the consuming use, and the `.ToArray()` materialization remedy"  ⚠ *v0.306 still reads this way, but the same section withdrew materialization as a remedy at v0.265 (#2557) — the mandate sentence was not updated; filed as [#4457](https://github.com/s8lap/bezel-lang/issues/4457). Mint the `29xx` seed against the corrected sentence.* |
| [§17.13](../Bezel_Language_Spec.md#sec-the-discard-_) | the `Task` discard error "names `Task.FireAndForget` specifically" |

**1.3 What the spec deliberately does *not* mandate.** Appendix [D](../Bezel_Language_Spec.md#appendix-d-conformance-levels)'s **Diagnostic identity (v0.163)** paragraph: the spec mandates *when* a diagnostic is produced and in places what it should name, but "deliberately defines no error-code registry or message taxonomy. Diagnostic identity — codes, exact wording, formatting — is implementation-defined," with a code registry contemplated only "if multiple implementations materialize." So the content is pinned, the identity is free — exactly the room a reference-compiler surface needs, and the boundary this proposal must not cross (§2.1 below).

**1.4 Wording is load-bearing today, and it shouldn't be.** The lit/FileCheck diagnostic track matches message text plus the `(§NN.N)` citation because that is all there is: `tests/README.md` states outright "Diagnostics have **no error codes** — match the **message text**." `scripts/extract_diag_to_lit.py` documents the consequence — since almost no gtest asserts a specific substring, "the only source of truth for 'what does bezelc actually say here' is bezelc itself," so the extractor runs the compiler as an oracle and freezes its current phrasing into `CHECK` lines. Every wording improvement is now a test migration; the message text has become an accidental stability surface, precisely inverted from Appendix D's intent: the latitude exists so wording can evolve freely, yet today wording is frozen by tests while stable identity does not exist at all.

**1.5 The agent-loop economics.** An AI agent regenerating a module runs the compiler hundreds of times a day; every check-cycle millisecond multiplies, which makes cold-start and output-parsing overheads first-order (§2.9). But the sharper cost is per-*cycle*, not per-millisecond: a diagnostic that mis-localizes a freeze conflict, or names the rule without the path, costs the agent a whole regeneration attempt — four orders of magnitude more than emitting the trace the compiler already computed. The compiler is the teacher in that loop; the trace is the lesson. The near-term consumer is internal: this repository's own producer lanes (`scripts/agent_check.sh --probe`) parse `bezelc` stderr today.

## 2. The design

### 2.1 Classification: tooling-only, and why that holds

Everything here is behavior of the reference compiler's *output and query* surface. No rule of the language changes: no program's accept/reject/warn outcome moves, no grammar production is touched, no stdlib type is added. The one plausible objection — "a code registry is the thing Appendix D said a future revision may introduce, so isn't this a spec change?" — is answered by Appendix D's own reasoning: the latitude exists because "requiring exact messages would freeze prose this document revises freely," and a *spec* registry is warranted only if multiple implementations materialize. Neither condition is disturbed: the registry below is owned by the compiler tree, binds no other implementation, and conformance claims continue to classify outcomes only. If a second implementation ever materializes, Appendix D's contemplated registry could *start from* this catalogue — explicitly a non-goal now (§6.3 below). The registry and explain content therefore live under `src/`, not `docs/` — the six normative documents and their gate (`check_docs.sh`) are not touched by this proposal.

### 2.2 The code taxonomy

**Format.** `BZLccss` — `BZL`, then two bucket digits, then a two-digit serial (01–99 per bucket).

**Buckets.**

- `01`–`34` — the spec's chapters 1–34. A diagnostic is bucketed by the chapter of the rule it enforces (the same section the repo's `// per §NN.N` comment discipline already names at the emission site), not by the compiler pass that detects it: `28xx` is ownership (§28), `30xx` async (§30), `19xx` the `case` construct (the `when` form was retired at v0.289, 0122 L1 — a `19xx` code for it would have been retired under clause 1 below, which is the contract working as intended), and so on. Bucket `24` (Events, a tombstone since v0.187 — the chapter keeps its anchor for citation stability and has no rules) mints nothing; a future chapter reusing the number takes a `35`–`49` slot instead, so the mnemonic never lies. `35xx` is choices (§35, landed v0.200). Appendix C invariants carry their owning chapter's bucket (each invariant already cites one); Appendix A syntax errors carry the bucket of the construct's home chapter (the §19.1 missing-`of` error is `19xx`).
- `00` — the driver front door: usage errors, file I/O, link failures, and construct-agnostic parse errors with no home chapter. Not spec-mandated; coded anyway so the JSON stream is total.
- `35`–`49` — reserved for future spec chapters.
- `51`–`84` — continuation buckets: when a chapter's 99 serials exhaust, its continuation bucket is `cc + 50` (§28 overflows into `78xx`). Deterministic, so a reader can still compute the chapter from the code.
- `90` — Appendix D conformance rejections (a Core implementation's mandated clean rejection of an unimplemented Extended feature).
- `99` — internal compiler errors. No spec anchor; the explain page says "please file an issue" and what to attach.

**Seed allocation.** The `28xx` bucket seeds from §28.3 *Diagnostics*' own five recommended messages, in the spec's order: `BZL2801` owned-value-dropped-without-disposal (the must-use warning), `BZL2802` borrow-outlives-source, `BZL2803` moved-from read, `BZL2804` move of a `using var`, `BZL2805` the consume-site freeze. Subsequent serials are minted in emission-site-conversion order (§5 below, S0).

**The stability contract.** This is the load-bearing promise; each clause is a decision:

1. **Codes are never reused.** A retired code stays in the registry marked retired, with a pointer to its successor(s). Retirement happens when a rule is removed from the spec or a code's meaning would otherwise change (clauses 3–5).
2. **Message text may evolve freely.** The code is the identity; wording is quality-of-implementation, revisable without notice — restoring Appendix D's intended split (§1.4).
3. **A code's severity class is pinned.** A warning promoted to an error (or vice versa) by a spec revision mints a new code and retires the old; a test or tool keyed on a code never silently changes failure class.
4. **A code's meaning never widens or narrows silently.** When one rule splits into two (a recurring shape in §28's history), both halves get new codes and the old one retires with two successors. A tool that matched the old code finds the fork recorded in the registry.
5. **The bucket is a mnemonic; the registry is the truth.** If a future spec revision renumbers chapters, existing codes keep their digits and their registry metadata updates; only *new* codes follow the new numbering. (The spec's stable `<a id="sec-…">` anchors, not section numbers, are the durable link — the same lesson `check_docs.sh` encodes.)
6. **Codes are minted only for rules the landed spec states** (plus the `00`/`90`/`99` operational buckets). A proposal's rules get codes when the proposal graduates, never before — so the registry cannot drift ahead of the language.

**Per-code registry metadata.** Code, canonical severity, spec anchor (`sec-…` id) + section number + revision introduced, a one-line summary, origin (`spec-mandated` — the spec names this rejection site — vs `qoi` — a reference-compiler refinement such as a lint), and status (`active`/`retired` + successors). Single source of truth: an X-macro table `src/diagnostics/registry.def`, from which the C++ enum, the `--query registry` dump, and the explain index are all generated — one fact, one home, per the repo's `OwnershipFacts` discipline:

```
// DIAG(serial, severity, section, anchor, introduced, origin, summary)
DIAG(2805, Error, "28.3", "sec-consume-site-freeze", "v0.197", SpecMandated,
     "consume of an owned binding while a live borrow of its value exists")
```

### 2.3 The registry CI gate: `scripts/check_diags.sh`

A sibling of `check_docs.sh` (the docs gate), run in the compiler CI lane, all mechanical:

1. **Anchor resolution** — every `DIAG` entry's anchor resolves to an explicit `<a id="…">` in `docs/Bezel_Language_Spec.md`, under the same resolution rules as `check_docs.sh` check 1. This anchor-resolution core should be factored into one shared script consumed by both gates — and by proposal 0054's coverage tool (drafted in parallel; it needs the identical spec-anchor infrastructure for its `<satisfies ref="…"/>` elements). One resolver, three consumers.
2. **Section sanity** — every entry's section number has a heading in the spec (`check_docs.sh` check 3's discipline).
3. **Explain totality** — every active code has an explain page and every explain page has a registry entry; every citation *inside* an explain page resolves (rule: explain pages cite the six normative documents only, never proposals — Q5).
4. **Registry integrity** — no serial reused across active + retired; retired entries name successors; severity/origin fields well-formed.

This is a lint, not a proof, exactly as `check_docs.sh` says of itself: it catches dangling anchors and stale metadata, not wrong explanations.

### 2.4 The JSON diagnostics schema

`bezelc --diag-format=json` (default remains `text`) emits one JSON document on stdout per invocation; exit codes are unchanged; `text` behavior changes only per §4 below. The schema, concretely — field presence rules follow each block:

```json
{
  "schema_version": 1,
  "invocation": {
    "compiler_version": "bezelc 0.x (bf96dd6)",
    "spec_revision": "v0.197",
    "files": ["src/Pool.bzl"]
  },
  "diagnostics": [
    {
      "code": "BZL2805",
      "severity": "error",
      "origin": "spec-mandated",
      "message": "cannot consume `buf` here: the borrow held by `s` (declared at line 7) is still live (§28.3)",
      "spec": { "section": "28.3", "anchor": "sec-consume-site-freeze", "revision": "v0.197" },
      "explain_ref": "BZL2805",
      "primary_span": { "file": "src/Pool.bzl", "line": 8, "col": 18, "end_line": 8, "end_col": 27 },
      "secondary_spans": [
        { "span": { "file": "src/Pool.bzl", "line": 7, "col": 9, "end_line": 7, "end_col": 27 },
          "label": "borrow `s` of `buf` created here" },
        { "span": { "file": "src/Pool.bzl", "line": 10, "col": 12, "end_line": 10, "end_col": 19 },
          "label": "surviving use of `s`" }
      ],
      "ownership_trace": {
        "kind": "freeze-conflict",
        "steps": [
          { "span": { "file": "src/Pool.bzl", "line": 5, "col": 9 },
            "role": "owned-binding", "note": "`buf` owns its value from initialization" },
          { "span": { "file": "src/Pool.bzl", "line": 7, "col": 9 },
            "role": "borrow-created", "note": "`s` is a borrow-bearing record local carrying `buf`" },
          { "span": { "file": "src/Pool.bzl", "line": 8, "col": 18 },
            "role": "consume-site", "note": "`owned buf` is a marked transfer" },
          { "span": { "file": "src/Pool.bzl", "line": 10, "col": 12 },
            "role": "surviving-use", "note": "`s.Length` reads the borrow after the consume" }
        ]
      },
      "fixits": [
        { "confidence": "speculative",
          "summary": "move the transfer after the last use of `s`",
          "edits": [] }
      ],
      "contract": null,
      "kernel": null
    }
  ],
  "queries": [],
  "summary": { "errors": 1, "warnings": 0 }
}
```

Presence rules: `code`, `severity`, `message`, `primary_span` always; `origin`/`spec`/`explain_ref` absent only for `99xx`; `secondary_spans` may be empty; `ownership_trace` present exactly when the diagnostic has a registered trace kind (below); `fixits` may be empty; `contract` and `kernel` are `null` until their proposals land (§3.2/§3.3). Unknown fields must be ignored by consumers; additions within a `schema_version` are additive-only; a breaking change bumps the version.

Field types, pinned (the commissioning contract — v1 freezes these):

| Field | Type / domain | Notes |
|---|---|---|
| `schema_version` | integer, `1` | top-level only |
| `code` | `"BZL"` + 4 digits | `"BZL0000"` = not-yet-registered emission site (rollout state, §4 below) |
| `severity` | `"error" \| "warning" \| "note"` | mirrors the engine's `Severity`; free-standing notes are rare — most explanatory content rides `secondary_spans`/`ownership_trace` |
| `origin` | `"spec-mandated" \| "qoi"` | registry metadata, echoed for filtering |
| `message` | string | human prose; unstable by contract (§2.2 clause 2) |
| `spec` | `{ section, anchor, revision }` | `anchor` is the spec's `<a id="sec-…">` id — the durable key; `section` is the human-readable mirror |
| Span | `{ file, line, col, end_line?, end_col? }` | 1-based line and column, in code points (the lexer's existing accounting); `end_*` omitted when the emitting site has no width (today's engine stores point locations — widths arrive as sites are converted, additive) |
| `secondary_spans[i]` | `{ span, label }` | `label` is prose, unstable |
| `ownership_trace` | `{ kind, steps }` | `kind` from §2.5's closed set; `steps` ordered as listed there |
| `steps[i]` | `{ span, role, note }` | `role` closed per kind; `note` prose, unstable |
| `fixits[i]` | `{ confidence, summary, edits }` | `edits` may be empty when no mechanical edit is expressible (advice-only remedy) |
| `edits[i]` | `{ span, replacement }` | replacement of the span's text; edits within one fixit are non-overlapping and apply atomically |
| `queries[i]` | `{ request, status, result }` | `status`: `"ok" \| "unavailable"`; shapes per §2.8 |
| `summary` | `{ errors, warnings }` | counts match the `diagnostics` array |

The motivating program for the example above (current syntax, real stdlib — `Array<T>` and `AsSpan()` per stdlib §4.4, `ArraySpan<T>` per stdlib §4.15):

```bezel
module Pool;

public class Program
  public static method Main(): int
    var buf := new Array<int>();
    buf.Add(41);
    var s := buf.AsSpan();      // `s` carries a borrow of `buf` (§9.7 record local)
    var taken := move buf;      // BZL2805: consume while `s` is live
    _ := taken;
    return s.Length;
  end Main;
end Program;
```

### 2.5 The `ownership_trace` payload: five kinds, all already computed

Every step is `{ span, role, note }`: `span` machine-stable, `role` from a per-kind closed vocabulary (closed per schema version), `note` free prose that evolves like message text. The five kinds and their sources — each maps onto data the ownership pass (`src/ownership/Ownership.cpp`, over the shared `src/common/OwnershipFacts` core) must already hold, because §1.2's mandated message content names it:

| Kind | Fires for | Step roles (in order) |
|---|---|---|
| `leak-path` | undisposed drop / owned escape (§28.3 *Diagnostics*, [§28.6](../Bezel_Language_Spec.md#sec-leak-prevention)) | `acquired`, `flow`\*, `branch`\*, `drop-site` |
| `borrow-source-chain` | borrow outlives source; frame carries a scope-bound source (§28.3 borrow-lifetime + *Returning a deferred-activation frame*) | `source`, `borrow-created`, `carried-by`\*, `escape-site` |
| `freeze-conflict` | the consume-site freeze (§28.3) | `owned-binding`, `borrow-created`, `consume-site`, `surviving-use` |
| `merge-conflict` | control-flow-join classification errors (§28.3 joins; maybe-consumed reads) | `branch-state` (one per predecessor, state in `note`), `join-point`, `offending-use` |
| `restoration-gap` | the field-restoration rule (§28.8.1) | `move-site`, `unrestored-path`\*, `exit-site` |

\* = zero or more. The design rule that keeps this honest: **a trace kind is added to the schema only when the emitting analysis already materializes its steps** — the trace is a serialization boundary, never a new analysis. That is the v1 test for whether a sixth kind (say, a sendability trace for §30.14, a natural S2-era candidate) may be added.

A second worked shape, `restoration-gap`, because it exercises both the multi-path trace and the multi-remedy fixit story. The program (§28.8.1: a bare `move _buf` of an owned instance field leaves the slot moved-from; every exit path must restore it):

```bezel
module Cache;

public class Cache
  private owned _buf: Array<int>;

  public constructor()
    _buf := new Array<int>();
  end constructor;

  public method Take(refresh: bool): owned Array<int>
    var b := move _buf;                 // move site
    if refresh then
      _buf := new Array<int>();         // restored on this path only
    end;
    return b;                           // BZL28xx: `_buf` unrestored when not refresh
  end Take;
end Cache;
```

The diagnostic's trace and fixits — note the trace's steps are the three things §28.8.1 *mandates* the message identify, and the fixits are its "three standard remedies," one per entry, all `speculative` because each changes behavior differently:

```json
"ownership_trace": {
  "kind": "restoration-gap",
  "steps": [
    { "span": { "file": "src/Cache.bzl", "line": 11, "col": 14 },
      "role": "move-site", "note": "`move _buf` leaves the field moved-from" },
    { "span": { "file": "src/Cache.bzl", "line": 12, "col": 5 },
      "role": "unrestored-path", "note": "the `refresh = false` arm assigns nothing to `_buf`" },
    { "span": { "file": "src/Cache.bzl", "line": 15, "col": 5 },
      "role": "exit-site", "note": "method exit reached with `_buf` moved-from" }
  ]
},
"fixits": [
  { "confidence": "speculative",
    "summary": "assign a value to `_buf` on the offending path",
    "edits": [ { "span": { "file": "src/Cache.bzl", "line": 14, "col": 5 },
                 "replacement": "else\n      _buf := new Array<int>();\n    end;" } ] },
  { "confidence": "speculative",
    "summary": "move the `move` into the branch where reassignment is reachable",
    "edits": [] },
  { "confidence": "speculative",
    "summary": "declare `_buf` nullable and assign `null` after the move",
    "edits": [] }
]
```

### 2.6 Fixits with confidence

`{ confidence: "high" | "speculative", summary, edits: [{ span, replacement }] }`.

- **`high`** — the rule's uniquely determined, behavior-preserving remedy: inserting the missing `of` (§19.1 names the delimiter), completing an `end Name;`. Policy: the reference compiler never emits `high` for an edit that changes runtime behavior. An agent may auto-apply `high` fixits.
- **`speculative`** — the spec's own multi-remedy diagnostics, one fixit per named remedy where an edit is expressible: the restoration gap's "three standard remedies" (§28.8.1) become up to three speculative fixits; §29.11's `.ToArray()` materialization is speculative (it changes allocation behavior deliberately). Suggestions, never auto-applied.

No `--fix` application mode ships (§6.9 below): the compiler reports; applying edits is the editor's or agent's job.

### 2.7 `bezelc --explain BZL2803`

Prints the code's explanation: the rule in plain terms, the spec citation (section + revision), one minimal rejected example and its remedy, and the related-codes list. Decisions:

- **Content lives in-tree** at `src/diagnostics/explain/BZL2803.md`, one file per code, and is **embedded in the binary at build time** — `--explain` works offline, in hermetic sandboxes, and always matches the binary's own behavior rather than an install tree's or website's (the same self-containment instinct as spec §2.8's artifacts). The prose is quality-of-implementation, revisable like message text.
- **CI-gated**: `check_diags.sh` §2.3 keeps every page existing and every citation resolving. A page that cites §28.3 cites the *anchor*, so spec renumbering cannot silently orphan it.
- **Derivations arrive with 0046** (§3.3): once the kernel lands, ownership pages gain a "Derivation" section citing kernel rules (K-numbering is frozen at 0046's landing, so the citations are stable). Until then, pages cite the current case-law subsections — no dependency on 0046 for S0.

A page's shape, fixed so the ~dozens of pages stay uniform (the sample is `BZL2803`, the brief's own example — moved-from read, §28.3's third recommended message):

```
BZL2803 — read of a moved-from local
error · spec §28.3 "Local variables, ownership, and borrows" (v0.197)

A local whose owned value was moved out (`move x`, a marked transfer, or an
owned-consuming position) is in the moved-from state; reading it again would
observe a value this method no longer owns. The compiler rejects the read and
points at the consuming site.

Rejected:                                Remedy (rebind after the transfer):
    var a := new Array<int>();               var a := new Array<int>();
    var b := move a;                         var b := move a;
    a.Add(1);        // BZL2803              b.Add(1);

Related: BZL2801 (dropped without disposal), BZL2805 (consume-site freeze).
See also: `bezelc --query ownership-state@FILE:LINE` for the state at any point.
[Derivation section lands with proposal 0046.]
```

### 2.8 The query surface, v1: batch CLI

```
bezelc --check src/Pool.bzl --import deps/Core.o \
       --query ownership-state@src/Pool.bzl:8 \
       --query borrow-source@src/Pool.bzl:9:12 \
       --diag-format=json
```

- **`--check`** (new, S1): run the front end through the ownership and definite-assignment passes; no LLVM IR, no object, no link. Independently the largest latency lever for agent loops (§2.9), and the natural host for queries.
- **`--query ownership-state@file:line[:col]`** — with `:col`, the named binding or expression at that token; without, every named binding in scope at that line. Per binding: `{ name, declared_at, state, since, notes }`, where `state` is drawn from §28.3's own vocabulary — `owned`, `borrowed`, `moved-from`, `explicitly-disposed`, `maybe-consumed` — plus `uninitialized` (§7.9's definite-assignment state) and `scope-pinned` (the §28.10 `using var` pin); these are the states the *Diagnostics* subsection obliges messages to name, and `since` is the span of the state's producing site.
- **`--query borrow-source@file:line:col`** — for a borrow-classified binding or expression: its source set as data — `{ sources: [{ kind: local|parameter|receiver|field|static|collection|temporary, name, span }], via: [steps] }`, `via` reusing §2.5's step shape for chains through record locals and frames.
- **`--query registry`** — dumps the §2.2 catalogue; needs no input file.

Answers ride the invocation's JSON document; for the §2.4 program the two requests above yield:

```json
"queries": [
  { "request": "ownership-state@src/Pool.bzl:8",
    "status": "ok",
    "result": { "bindings": [
      { "name": "buf", "declared_at": { "file": "src/Pool.bzl", "line": 5, "col": 9 },
        "state": "owned", "since": { "file": "src/Pool.bzl", "line": 5, "col": 16 },
        "notes": ["borrowed-from: `s` (line 7) is live here"] },
      { "name": "s", "declared_at": { "file": "src/Pool.bzl", "line": 7, "col": 9 },
        "state": "borrowed", "since": { "file": "src/Pool.bzl", "line": 7, "col": 14 },
        "notes": ["borrow-bearing record local (§9.7)"] }
    ] } },
  { "request": "borrow-source@src/Pool.bzl:10:12",
    "status": "ok",
    "result": {
      "sources": [ { "kind": "local", "name": "buf",
                     "span": { "file": "src/Pool.bzl", "line": 5, "col": 9 } } ],
      "via": [ { "span": { "file": "src/Pool.bzl", "line": 7, "col": 14 },
                 "role": "carried-by", "note": "`buf.AsSpan()` construction carries the borrow" } ]
    } }
]
```

Semantics, decided: queries are **batch** — one process, one front-end run, all answers in the same JSON document's `queries` array, each echoing its request string. If the governing pass never ran for the queried position (a parse or sema error upstream), the answer is `{ status: "unavailable", reason }` while `diagnostics` carries the errors; if the ownership pass ran for the enclosing body, the query answers even when *other* bodies have errors — the agent's common case is "errors over there, question right here." Positions inside generic bodies answer pre-substitution (the ownership pass runs before monomorphization; recorded limit, not a bug). The honesty rule matching §2.5: **v1 serializes what the ownership pass already computes** — per-point states and source sets are its working set — and adds no analysis. Type/sema queries, cross-module queries beyond the loaded `--import` closure, and free-form expression evaluation are all out of v1.

### 2.9 Latency: engineering goals, stated and measured

Goals, not promises — and explicitly not conformance surface (Appendix D is silent on performance). Pinned to the project dev box until Q7 fixes a reference machine:

| Path | Goal |
|---|---|
| warm `--check` of a 1–2 kLOC single module | p50 ≤ 100 ms, p95 ≤ 250 ms |
| `--diag-format=json` overhead vs `text` | ≤ 1 ms + ≤ 50 µs per diagnostic |
| each additional `--query` in an invocation | ≤ 5 ms (serialization only, no re-analysis) |
| S3 daemon warm re-check after one-file edit | ≤ 30 ms (goal recorded now, machinery is S3's) |

Why goals belong in this proposal at all: the agent-loop argument from §1.5 cuts both ways — per-millisecond (hundreds of invocations daily multiply every fixed cost, so `--check`'s existence matters more than any single optimization) and per-cycle (a trace that saves one wrong regeneration pays for thousands of emissions). Discipline over aspiration: S1 ships an in-tree benchmark (a `--time-report` phase breakdown plus a tracked benchmark program) so the numbers are measured in CI, and regressions are visible even though they gate nothing.

### 2.10 Minting a code: the day-to-day workflow

The registry only stays truthful if minting is cheap and reviewed. The workflow, designed for the repo's actual traffic (bug-fix lanes shipping spec-cited regression tests):

1. A fix lane converting or adding an emission site takes the **next free serial** in the rule's chapter bucket and adds one `DIAG(…)` line — severity, section, anchor, revision, origin, summary — plus the explain page. A `99xx` entry (an `internal:` degrade site) carries one more field, its **disposition**: `ice` (aborts), `wrong-value` (ships a bounded wrong value) or `unsafe-pointer` (ships a pointer the program may dereference). That column *is* the per-site record issue #2924 asks for and family K of [`defect-campaign-2026-09.md`](defect-campaign-2026-09.md) anchors on — the registry gives it a home, and `check_diags.sh` can then assert that no `unsafe-pointer` site carries `Warning` severity (the only disposition that must never ship a binary). The anchor is copied from the spec heading the site's `// per §NN.N` comment already cites; `check_diags.sh` fails the PR if it doesn't resolve.
2. The lane's regression test keys on the **code** (`// CHECK: [BZL28nn]` / `hasCode`), plus at most one wording fragment for readability. Reviewers check one thing the CI can't: that the cited anchor is the rule the site actually enforces (the same §-citation discipline CLAUDE.md already imposes on comments, now load-bearing).
3. Serial collisions between parallel lanes are ordinary merge conflicts on `registry.def` — visible, trivially resolved by renumbering the younger entry *before* it lands (never after: a landed code is frozen by §2.2 clause 1).
4. Retirement (rule removed, split, or reclassified by a spec revision) is a PR that flips the entry's status, names successors, and updates the explain page to say what happened and where to look — the registry doubles as the diagnostic-surface changelog, which no other artifact currently records.

## 3. Interactions

**3.1 Proposal 0040 (build system).** The driver's verbs compose with, and never wrap, this surface: `bezel build` (0040 Stage 1) forwards `--diag-format=json` to each per-SCC `bezelc` invocation and concatenates the documents into one build-level report keyed by module — the schema's `invocation` block makes the merge lossless. `bezel test` (0040's 0022 hook) gets code-keyed failure reporting for free. The S3 daemon is explicitly gated on 0040 Stage 1: its re-check minimization is 0040's interface-hash early cutoff applied to checking, and building that graph twice would be absurd. 0040's Q1 (umbrella `bezel` binary vs `bezelc` modes) decides where `--lsp`/daemon flags live; nothing here depends on the answer.

**3.2 Proposal 0043 (behavioral contracts).** When 0043 lands (its spec home is §22.16, so its codes bucket as `22xx`), compile-time contract diagnostics — a `static` clause the prover cannot discharge, clause-admission errors (purity, borrow-only, `await`) — populate the `contract` payload: `{ clause_kind: "requires"|"ensures"|"invariant", tier: "proven"|"checked"|"assumed", member, clause_source }`, per 0043 §7's tier vocabulary. This is deliberately the same clause-identity shape 0043 §6.1 defers to its stage 2 ("a machine-readable clause identity" fixed by the artifact interface): one vocabulary, defined once there, serialized here — the two proposals must not fork it. Runtime `ContractViolationException` reporting stays 0043's exception surface; this API is compile-time only (§8 below).

**3.3 Proposal 0046 (ownership-kernel consolidation).** Once the kernel lands, ownership diagnostics gain `kernel: { rules: ["K5"], axioms: [] }` and `--explain` pages gain derivation sections — "explain by derivation": BZL2805's page derives the freeze from K5's *unfrozen ⟺ announced ∧ flag-backstopped* criterion rather than restating case law. 0046 freezes K-numbering at landing precisely so surfaces like this can cite it; until then the field is `null` and pages cite §28.x directly. No stage of this proposal blocks on 0046.

**3.4 Proposal 0054 (traceability; drafted in parallel).** Shares the spec-anchor infrastructure: §2.3's anchor resolver should be the same code path 0054's coverage tool uses, and the `spec` metadata block here is the same anchor-vocabulary its `<satisfies ref="…"/>` ids will resolve through. Custody of the shared resolver is Q4.

**3.5 The test tracks.** lit/FileCheck: §4 below. The gtest harness: `Diagnostic` gains `code` and `secondarySpans` fields; `JitResult.diagnostics` grows a `hasCode("BZL2805")` helper; every existing `.hasErrors()` / `.message.find` assertion is untouched. `extract_diag_to_lit.py` is updated to emit code-anchored `CHECK` lines (§4), ending its freeze-the-wording-forever era. Proposal 0022's runner may reuse the schema's span shape in its own reports; nothing is required of it.

**3.6 Appendix D.** Untouched, verbatim. Conformance continues to classify outcomes; no other implementation is bound by BZL codes; the `90xx` bucket implements Appendix D's mandated Extended-feature rejection *for this compiler* without making the code part of the mandate.

## 4. Migration and compatibility

**Text output.** The only default-visible change: a trailing code tag — `path:line:col: message (§28.3) [BZL2805]`. Trailing, not leading, is a decided point (§6.6 below): the existing lit suite anchors `CHECK` lines on the `:col: message` boundary (e.g. `// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot call …`), so a rustc-style `error[BZL2805]:` *prefix* would break essentially every diagnostic test for zero information gain, while a suffix is invisible to substring matching. S0 lands with a one-shot audit script confirming no existing `CHECK`/`CHECK-SAME`/`CHECK-NOT` pattern collides with the tag.

**lit tests.** Existing tests keep passing unmodified — text and `(§NN.N)` citations remain in the message. New and migrated tests prefer `// CHECK: [BZL2805]` plus at most one wording fragment; the code survives rewording, the fragment documents intent. Migration is additive and per-test, never a bulk rewrite; wording changes stop being test migrations for every migrated test.

**gtest.** Additive fields only (§3.5). **CLI.** All new flags; no existing flag changes meaning; `--diag-format=text` is the default indefinitely. **JSON.** Versioned per §2.4; consumers ignore unknown fields. **Codes.** The stability contract (§2.2) *is* the migration story for consumers: keying a tool or test on a code is safe by construction; keying on wording remains legal and remains fragile, now by choice.

**Rollout inside the compiler.** Codes are attached per emission site; sites not yet converted print no tag and appear in JSON as `code: "BZL0000"` (unregistered — a real, queryable state, so the conversion's progress is measurable). S0's exit criterion covers the ownership and sema reject families first, tracking the repo's actual bug-fix traffic.

## 5. Staging

- **S0 — codes and explanations** (compiler-track, no new binaries). `registry.def` + generated enum; trailing-tag text format + lit-collision audit; seed codes: the `28xx` §28.3 five, the §1.2 mandated-content family (§28.8.1, §30.5 ×2, §29.11, §17.13) plus the §19.1 missing-`of` syntax error, then ownership/sema emission sites in conversion order; `--explain` with embedded pages for every registered code; `scripts/check_diags.sh` in CI (shared anchor resolver with `check_docs.sh`/0054); `extract_diag_to_lit.py` update. *Exit:* ownership + sema reject families coded; gate green with zero lit modifications.
- **S1 — structured output and queries.** `--diag-format=json` (full §2.4 schema); `ownership_trace` for all five kinds; `secondary_spans` in the engine and text renderer (as `note:` lines, additive); fixits (`high` for syntax, `speculative` for the multi-remedy ownership family); `--check`; batch `--query` (`ownership-state`, `borrow-source`, `registry`); the latency benchmark + `--time-report`. *Exit:* `agent_check.sh --probe` consumes the JSON path end-to-end in a producer lane.
- **S2 — server mode.** An LSP subset speaking the same payloads: `initialize`, full-text `didOpen`/`didChange` sync, `publishDiagnostics` (schema diagnostics mapped onto LSP's shape, `ownership_trace` in `relatedInformation` + a namespaced extension field), hover = `ownership-state`, and namespaced custom requests `bezel/ownershipState`, `bezel/borrowSource` returning §2.8's JSON verbatim. Deliberately *non*-incremental: every request re-runs `--check` on the buffer — correct first, fast enough for single modules per §2.9's goals. Home binary per 0040 Q1.
- **S3 — incremental daemon.** Persistent process; module-graph-aware re-check with 0040 Stage 1's interface-hash cutoff; the ≤ 30 ms warm goal. **Gated on** 0040 Stage 1 landing *and* observed S2 usage — if nobody outgrows S2, S3 doesn't get built.

Each stage is independently shippable and independently useful; S0 alone retires the wording-fragility problem (§1.4).

## 6. Alternatives considered and declined

1. **SARIF as the native schema.** The standard exists and tooling reads it — but it is tuned for static-analysis aggregation, its span/result model has no typed home for `ownership_trace` semantics (they would land in opaque property bags), and its verbosity taxes exactly the loop §1.5 optimizes. Declined as the native format; a lossy SARIF *emitter* over §2.4 is a small, welcome later addition.
2. **LSP-first.** Protocol state, lifecycle, and sync semantics before any consumer needs them; the actual near-term consumers (agent loops, this repo's lanes) want a batch CLI. Staged instead (S2), with the batch surface defined so the server serializes the same payloads.
3. **A spec-mandated code registry** (amend Appendix D now). Contradicts the latitude's own recorded reasoning; with one implementation, it would freeze reference-compiler choices into normative text nobody else consumes. Revisit only on Appendix D's own trigger — a second implementation.
4. **No codes — structured JSON only.** JSON without stable identities re-derives the wording problem one level up (tools would match `message` inside JSON). Codes are the identity layer; the schema is the payload layer; both are needed.
5. **Bucketing by compiler pass, not spec chapter.** Pass boundaries are implementation accidents — the #1414 refactor moved classification logic between sema and codegen consumers, and rules migrate between passes routinely. Spec chapters are the stable public geometry, and they match the repo's existing `// per §NN.N` citation discipline at every emission site.
6. **Leading `error[BZL2805]:` prefix** (rustc-style). Breaks nearly every existing lit `CHECK` anchored on `:col: message` for zero information gain over a trailing tag (§4 above). A one-time coordinated reformat sweep remains possible later (Q2) — scripted, like the repo's `migrate_*.py` precedents — but is not this proposal's business.
7. **Explain content in `docs/` or on a website.** `docs/` is the six normative documents plus proposals, with its own change gate; compiler-owned prose does not belong under it (and CLAUDE.md fences it). A website departs from the binary's version and fails offline/hermetic use. In-tree + embedded wins on every axis that matters here.
8. **Streaming NDJSON diagnostics in v1.** Batch consumers read after exit; crash-partial output is signaled by exit status; streaming buys nothing until a long-lived server exists — it is S2/S3's concern, where LSP framing supplies it anyway.
9. **A `--fix` auto-apply mode.** Writing user source moves the compiler from reporter to actor, with all the trust and idempotency questions that carries; agents and editors apply edits better, with review. The `confidence` field gives them exactly the policy signal they need. Revisit with evidence of demand, not by default.

## 7. Open questions / owner decisions

- **Q1 — Binary packaging** (shared with 0040 Q1): `bezelc` modes vs a `bezel` umbrella binary for `--lsp`/daemon. Affects flag spelling only; lean: follow 0040's resolution.
- **Q2 — Text-format modernization sweep**: keep the trailing `[BZLnnnn]` tag permanently, or run a one-time scripted lit migration to a leading `error[BZLnnnn]:` prefix (severity word included) once codes are established? Lean: keep trailing through S1; decide at S2 with usage evidence.
- **Q3 — QoI codes in the stability contract**: `origin: qoi` diagnostics (reference-compiler lints not named by the spec) get registry entries under the same never-reuse rules. Confirm the owner wants QoI surface bound by the same stability promise, or a weaker one (codes stable, existence not guaranteed).
- **Q4 — Custody of the shared spec-anchor resolver** once 0054 lands: owned here (`check_diags.sh`), by 0054's tool, or as a third shared script both call. Mechanical, but someone must own it.
- **Q5 — Explain-page citation policy**: proposed as "landed normative docs only, never proposals" (CI-enforced). Confirm, or allow clearly-marked forward pointers to accepted-but-not-graduated proposals.
- **Q6 — S2 protocol scope**: which LSP capabilities beyond the S2 subset of §5 above (if any) are in scope, and whether the custom requests ship as `experimental/` until S3.
- **Q7 — The latency reference machine**: which box pins §2.9's numbers, and whether the benchmark gates anything (proposed: measured and reported, gating nothing, revisit after two quarters of data).

## 8. Divergences from the P12 sketch, recorded

1. **Kernel citation is staged, not assumed.** P12 sketches "which kernel rule, once P6 exists, was violated"; this design ships spec-anchor explanations in S0 with the `kernel` field and derivation sections arriving when 0046 lands — no dependency inversion, nothing here blocks on the kernel.
2. **The query mode is batch-first.** P12's "query mode" is deliberately narrowed for v1 to a multi-`--query` batch invocation over analyses the compiler already runs — one process, N answers — with the interactive server staged behind it. This is the agent-loop shape, and it keeps v1's cost honest.
3. **Latency is goals-plus-benchmark, not a feature.** P12 lists latency as part of the item; this proposal declines to promise numbers and instead institutionalizes measurement (§2.9), with `--check` as the one concrete latency *mechanism* shipped.
4. **Runtime diagnostics are explicitly out.** Contract violations at runtime (0043's exception surface) and property-test counterexample reports (0053) are adjacent but not this API; the boundary is compile-time analysis output. P12 did not distinguish; this proposal does.

## 9. S0 implementation brief (added 2026-09-17, compiler @ `29410e29`)

*Written without a build ([#4451](https://github.com/s8lap/bezel-lang/issues/4451)); every implementation fact below is a source read cited by file and line. It turns the S0 deliverables the #2039 ruling names — "registry.def X-macro, trailing-tag renderer, lit-collision audit, BZL2801–BZL2805 seed from §28.3's five recommended messages, explain pages, check_diags.sh on the shared resolver" — into a work-list an implementer can execute without re-deriving the design. Nothing here changes §2–§8.*

### 10.1 Why S0 now, in bug-fixing mode

S0 touches no lowering, no ownership rule, no runtime: it adds a field to `Diagnostic`, a table, a renderer suffix, and a build-free script. It is the one tooling item that **pays the bug campaign back directly**:

| Campaign need | What S0 supplies |
|---|---|
| Family B's rows assert the **diagnostic text** "cannot move from disposed local" because the exit code cannot tell the §28.2 Warning from the §28.3 Error (#4023) | A code per rule: `BZL28nn` for the §28.3 rejection vs a different serial for the §28.2 warning; the row keys on the code and the wording is free to improve |
| Family K needs a per-site disposition record for the 12 (now more) `internal:` Warning sites (#2924) | The `99xx` bucket with the disposition column (§2.10 step 1); `check_diags.sh` asserts the invariant |
| The rule corpus's first classification (#4383, 2,148 cases) must decide "did the compiler reject *for the rule under test*" | A case expecting `BZL2805` is unambiguous; a case expecting "an error" is not (#2988's `hasErrors()` problem, restated) |
| `check_src_citations.py` (#4455) already resolves every `§N.N` in `src/` against all documents' headings | It is the seed of §2.3's shared resolver: extend it from *section numbers* to *anchor ids* and both gates share one truth |

The measured surface (2026-09-17, `grep` over the tree): **256** `report(Severity::…)` emission sites in `src/` (248 Error, 4 Warning, the rest Note); **648** message strings carry a `(§N.N)` citation inside the text; **1,136** test assertions across **817** test files match message substrings (`.message.find("…")`); **125** lit tests, **84** with `CHECK` lines, **46** `CHECK` lines match a `§` citation, **129** `CHECK-NEXT`/`CHECK-SAME` lines, and **0** end-anchored (`{{$}}`) `CHECK` lines. The last number is the lit-collision audit's headline (§9.4).

### 10.2 The engine change (one struct, one renderer)

Today: `struct Diagnostic { Severity severity; std::string message; SourceLocation location; }` at `src/support/Diagnostic.hpp:22–26`, reported through `DiagnosticEngine::report(Severity, SourceLocation, std::string)` (`:31`), printed as `path:line:col: message` at four sites in `src/driver/Driver.cpp` (`:632`, `:644`, `:773`, `:785`).

S0 adds:

- `DiagCode code = DiagCode::Unregistered;` to `Diagnostic` (a `uint16_t`-backed enum generated from `registry.def`; `Unregistered` renders as `BZL0000` per §2.4's rollout rule) and an overload `report(DiagCode, Severity, SourceLocation, std::string)`. The three-argument `report` stays and stamps `Unregistered`, so **every existing emission site compiles unchanged** and the conversion is per-site and incremental.
- The four print sites collapse to one `renderText(const Diagnostic&)` in `Diagnostic.cpp`, emitting `path:line:col: message [BZLnnnn]` — the trailing tag, appended after the message and after its `(§N.N)` citation, never before. Four copies of one format is the duplicated-spelling shape the census warns about; S0 is the excuse to close it.
- `--explain BZLnnnn` in `src/main.cpp`'s argument loop (`:76–:150` today), which prints the embedded page and exits 0; `--explain` with an unknown code exits 2 with "no such code" and the nearest active codes.

### 10.3 `src/support/registry.def` — the X-macro and the seed

One line per code, in the §2.2 format, plus the disposition field for `99xx`:

```
// DIAG(serial, severity, section, anchor, introduced, origin, summary)
// DIAG_INTERNAL(serial, severity, disposition, summary)   — bucket 99 only
DIAG(2801, Warning, "28.3", "sec-diagnostics", "v0.197", SpecMandated,
     "owned value dropped without disposal (auto-drop must-use warning)")
DIAG(2802, Error,   "28.3", "sec-diagnostics", "v0.197", SpecMandated,
     "borrow outlives the source it borrows from")
DIAG(2803, Error,   "28.3", "sec-diagnostics", "v0.197", SpecMandated,
     "read of a moved-from binding")
DIAG(2804, Error,   "28.3", "sec-diagnostics", "v0.197", SpecMandated,
     "move of a `using var` binding")
DIAG(2805, Error,   "28.3", "sec-consume-site-freeze", "v0.197", SpecMandated,
     "consume of an owned binding while a live borrow of its value exists")
```

Then the §1.2 mandated-content family in the order §5 lists: §28.8.1 field-restoration (`28xx`), §30.5 bare-`await` and un-consumed `OwningTask<T>` (`30xx`, two codes), §29.11 multiple enumeration (`29xx`), §17.13 the `Task` discard naming `FireAndForget` (`17xx`), and the §19.1 missing-`of` syntax error (`19xx`). **All seven anchors §1.2 cites are present in the v0.306 spec** (`sec-diagnostics`, `sec-consume-site-freeze`, `sec-the-field-restoration-rule`, `sec-result-lifetime-and-task-disposal`, `sec-multiple-enumeration`, `sec-the-discard-_`, `appendix-d-conformance-levels` — re-verified 2026-09-17), so the seed needs no re-anchoring. The `introduced` field of each seed row is the revision the *rule* was mandated at, not S0's landing revision; the lane reads the changelog for each.

The generated artifacts, all from this one file: the `DiagCode` enum, the code→metadata lookup `renderText` uses, the `--explain` index, and the `--query registry` dump (S1). One fact, one home.

### 10.4 The lit-collision audit — done by reading, result: zero modifications expected

§5's S0 exit criterion is "gate green with **zero** lit modifications". The audit question is whether appending ` [BZLnnnn]` to the end of every diagnostic line can break an existing `FileCheck` match. Read against `tests/lit/*.bzl` (125 files) on 2026-09-17:

- `CHECK:` and `CHECK-SAME:` are substring matches; a suffix appended *after* the matched text cannot break them. 46 `CHECK` lines match a `§` citation; every one of those is a substring of the line (e.g. `// CHECK-SAME: spec §14.2`).
- `CHECK-NEXT:` constrains which *line* follows, not where the previous line ends; a longer previous line is still one line. 129 such lines, none affected.
- The only pattern a trailing tag breaks is an **end-anchored** match, `{{$}}` — and there are **0** in the tree.
- `NODIAG-NOT: error` / `NODIAG-NOT: §` (the accept-case idiom) are unaffected: a program that compiles clean prints nothing, tag or no tag.
- The gtest side: `diagnosticMessageContains` (`tests/support/aot_result_asserts.hpp:98`) and the 1,136 `.message.find("…")` assertions test `Diagnostic::message`, which S0 leaves **untagged** — the tag lives in the rendered text only. So the gtest track is untouched by construction, not by audit.

The residual risk is a test that compares a *rendered* stderr line for equality; `scripts/extract_diag_to_lit.py` generates `CHECK` lines from bezelc's own output, so future extractions will carry the tag, and a `// CHECK-SAME: [BZL2805]` becomes the idiom §2.10 step 2 asks for. The lane re-runs this audit mechanically before landing (`grep -c '{{\$}}' tests/lit/*.bzl` must still be 0).

### 10.5 `scripts/check_diags.sh` on the shared resolver

`scripts/check_src_citations.py` (#4455) already does the hard half: `defined_sections()` reads the headings of the spec, stdlib, rationale and every guide (the lesson recorded in its own docstring — a checker that reads the spec's headings alone manufactures false positives). S0 extends it, rather than writing a second resolver, by:

1. factoring `defined_sections()` and a new `defined_anchors()` (every `<a id="sec-…">` and `appendix-…` id, the same two styles `check_docs.sh` check 1 resolves at `:242–:266`) into `scripts/docrefs.py`, imported by `check_src_citations.py`, `check_diags.py` and — when 0054's S0 lands — its trace-coverage tool. **This discharges 0052 Q4 and 0054's joint-custody clause with one file**, and answers "who owns the resolver": the script family under `scripts/`, gated by `gate_full_cycle.sh` like its siblings.
2. `check_diags.py` then asserts §2.3's four checks over `registry.def` and the explain pages: anchor resolution, section sanity, explain totality, registry integrity (no serial reused across active+retired; retired names successors; severity/origin/disposition well-formed) — plus the family-K invariant: no `99xx` entry with disposition `unsafe-pointer` and severity `Warning`.
3. It ships with `--self-test` (plants a dangling anchor and a reused serial in a temp copy and proves both are caught) and lands as a phase in `gate_full_cycle.sh` beside the `src-citation phase` (`:852–:867`), self-test first, fail-closed — the discipline [`oracle-ratchets-2026-09.md`](oracle-ratchets-2026-09.md) §0.1 states for the whole script family.

### 10.6 Explain pages

One Markdown-in-a-string page per active code, embedded in the binary (a generated `explain_pages.inc` from `docs/explain/BZLnnnn.md`? — **no**: the ruling and §2.1 keep the registry and pages under `src/`, not `docs/`, so the six normative documents and `check_docs.sh` are not touched; the pages live at `src/support/explain/BZLnnnn.md` and are embedded at build time). Each page: the rule in one paragraph, the spec anchor as a link, one rejected example and one accepted rewrite, and the standard remedies the spec names where it names them (§28.8.1's three). Citation policy per Q5's lean: landed normative documents only; `check_diags.py` resolves every citation in every page.

### 10.7 Conversion order and the exit criterion

Convert emission sites in this order, each conversion a small commit that adds the `DIAG` row, stamps the site, and updates that site's lit test to `CHECK-SAME: [BZLnnnn]`:

1. the five §28.3 seeds (they are the most-tested rejections in the suite, so the tag's harmlessness is proven on the widest surface first);
2. the §1.2 mandated-content family;
3. the ownership pass's remaining Error sites (`src/ownership/`, the bulk of the 248 — bucket `28`, overflowing into `78` per §2.2 when serial 99 is reached);
4. sema's reject families (`src/sema/`, buckets by chapter);
5. the `99xx` internal sites, each with its disposition — this step is family K's anchor deliverable and should be reviewed by that family's lane.

**Exit:** every `report(` in `src/ownership/` and `src/sema/` carries a code (`grep -c 'report(Severity::' src/ownership src/sema` returns 0 — a ratchet in the same family as the deprecated-JIT one, baseline exact-not-ceiling), `--explain` answers every active code, `check_diags.py` is in the gate, and the lit track has had zero edits except the additive `CHECK-SAME: [BZLnnnn]` lines.

### 10.8 What S0 deliberately leaves to S1

`secondary_spans` (today three ownership diagnostics interpolate a second location into prose — "was here" / "declared at line" — measured 2026-09-17), `ownership_trace`, fixits, `--diag-format=json`, `--check` and `--query`. S0's `Diagnostic` gains only `code`; the S1 fields are added in one further schema step so that S0 cannot be blocked on the trace serialization design.

## 10. Relations

[#2039](https://github.com/s8lap/bezel-lang/issues/2039) (tracking) · [`future-directions-2026-07-18.md`](future-directions-2026-07-18.md) P12 (mandate) · proposal [0040](0040-build-system.md) (driver verbs; S3's graph + Q1) · proposal [0043](0043-behavioral-contracts.md) §6.1/§7 (clause identity + tier vocabulary in the `contract` payload) · proposal [0046](0046-ownership-kernel-consolidation.md) (K1–K9; `kernel` payload + explain derivations) · proposal 0054 (drafted in parallel; shared spec-anchor infrastructure) · proposal [0022](0022-test-framework.md) (code-keyed assertions; report span reuse) · spec Appendix [D](../Bezel_Language_Spec.md#appendix-d-conformance-levels) *Diagnostic identity* (the latitude this stays inside) · spec §28.3 [*Diagnostics*](../Bezel_Language_Spec.md#sec-diagnostics) (the mandated-content anchor) · `tests/README.md` + `scripts/extract_diag_to_lit.py` (the wording-fragility evidence) · `scripts/check_docs.sh` (the anchor-gate discipline §2.3 reuses).
