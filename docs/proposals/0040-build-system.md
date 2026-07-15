# Proposal 0040 — The build system: a staged first-party project driver (`bezel build`)

| | |
|---|---|
| **Status** | draft (design sketch — owner-requested 2026-07-16; not yet under-review) |
| **Tracking issue** | [#1908](https://github.com/s8lap/bezel-lang/issues/1908) (ecosystem pillar 1 of [#1904](https://github.com/s8lap/bezel-lang/issues/1904)) |
| **Target module(s)** | Tooling only — a build driver over the existing compiler surface. **No spec changes**: this proposal *implements* spec [§2.8](../Bezel_Language_Spec.md#sec-module-identity) (module identity / artifact interface / program assembly, landed v0.188 from proposal [0034](0034-module-identity.md)) and [§2.5/§2.6](../Bezel_Language_Spec.md#sec-dependency-resolution)'s SCC compilation units (settled v0.164). Stage 0 is exactly compiler tracker [#1398](https://github.com/s8lap/bezel-lang/issues/1398). |
| **Written against** | spec/stdlib **v0.196**, compiler @ `974be95` |
| **Origin** | The 2026-07-16 ecosystem discussion (internal-dogfooding ordering, [#1904](https://github.com/s8lap/bezel-lang/issues/1904)); constraint extraction from §2.8/0034/#1398 and a Driver-capability audit done for this sketch. The caching design generalizes the AOT test harness's content-addressed link-cache (in-tree since 2026-07-15, `tests/support/aot_support.hpp`). |
| **Design influences** | Cargo and `go build` (toolchain-integrated driver; convention over configuration; the manifest declares only what source cannot); Go specifically for the source-derived dependency graph (Bezel's `import Name;` headers make the graph parseable, as Go's imports do); Bazel/Buck2 (evaluated and *declined* for the core — their per-target BUILD files would duplicate a source-derivable graph; kept as a possible later secondary integration for remote caching); Ninja (evaluated and declined as the executor — its timestamp model is weaker than the content-addressed cache we already have); C++ modules as the avoided cautionary tale (Bezel artifacts are self-contained, so there is no separate interface-file orchestration problem to solve). |

## 0. What this is, and what it is not

This is the design sketch for how Bezel projects get built: multi-module programs, libraries, dependencies, incrementality, and the host tool for `bezel run` / `bezel test` / future generate steps. It is **not** a spec change — §2.8 already pins the build *model* (deliberately, as build-system-agnostic invariants), and §2.8.5 explicitly fences off package management, versioning, and manifest format as tool concerns. This proposal fills exactly the space the spec left to the tool.

It is also not a greenfield mandate: **Stage 0 below is [#1398](https://github.com/s8lap/bezel-lang/issues/1398)**, an already-open compiler tracker (§2.5 SCC compilation is normative and unimplemented). The staging is designed so each stage is independently useful and the first one is ordinary compiler-track work.

Today's state, for the record: `bezelc` compiles **one source file** per invocation; `-c` emits a library object with an embedded `.bezel.meta` interface; `--import <artifact>` (repeatable) consumes dependencies; linking is via clang. There is **no graph discovery of any kind** — "the build system" is currently a human sequencing `bezelc -c` / `--import` calls in dependency order by hand.

## 1. What the spec already pins (the invariants any build tool must uphold)

Verified against §2.8 (v0.196) and proposal 0034; these are *constraints on this design*, not choices it gets to make:

1. **The unit of compilation is the SCC, not the file.** Import cycles are legal; mutually-importing modules compile as one compiler invocation (sources cross-resolved directly, no artifacts consumed within the SCC), emitting one artifact per member. The SCC-condensed graph is a DAG walked in topological order. Settled since v0.164; unimplemented (#1398).
2. **The dependency graph is derivable from source.** `import Name;` headers name every edge. What source *cannot* say — which artifact satisfies a module name, and version arbitration — is §2.8.5's explicit assignment to the build tool.
3. **Artifacts are self-contained.** One object file with an embedded `.bezel.meta` that must be able to (a) type-check every public/protected use (incl. ownership modifiers and §3.6 derived-property verdicts), (b) instantiate public generics at arbitrary external type arguments (generic bodies as source text — the #428 resolution, blessed by 0034 §2), and (c) describe every dependency type reached from the surface. Consequence for the tool: **there are no separate interface files to schedule** — artifact = one file.
4. **Program assembly is ruled.** Artifact kind (exe vs library) is a **build-request property** — never inferred from `Main` presence, so the driver *must* carry a kind declaration (manifest or flag). Exactly one qualifying `Main` in the root compilation, `--main-class` (#1734) to disambiguate, a dependency's `Main` is inert, executable-as-dependency is a build error, duplicate module names across the closure is a named error, and the tool must supply the **full transitive closure** (including definition-site-carried refs).
5. **No stable ABI.** The model is recompile-the-world (§2.8.5). This *simplifies* the tool: no ABI-compatibility logic, and it makes interface-hash early cutoff (§4 below) the natural incremental mechanism.

## 2. Why not an existing build system

| Candidate | Verdict | Reason |
|---|---|---|
| Make / hand-written Ninja | stopgap only | Works today over `bezelc -c --import` for a handful of modules; no graph discovery (encoded by hand), timestamp-based, SCC units manual. Fine while Stage 0 is being built; dies at ~10 modules. |
| CMake / Meson | declined | Their custom-language support assumes per-file compilation with depfiles; the SCC-as-unit model fights it, and their ecosystem value (`find_package`, C/C++ toolchains) is irrelevant to Bezel dependencies. The result would be `add_custom_command` glue — Ninja with extra steps. |
| Bazel / Buck2 | declined for the core; possible Stage-3 secondary | Real value at org scale (hermeticity, remote cache), but per-target BUILD files would *duplicate* the source-derivable import graph (drift; Go needed `gazelle` to auto-generate them), and the adoption weight is wrong for internal dogfooding. Revisit as official rules generated from the Stage-1 graph library if monorepo-scale ever arrives. |
| Cargo / go build / SwiftPM | not reusable; the model to copy | Language-bound implementations; their *shape* (toolchain-integrated driver, convention over configuration) is what Stages 0–2 adopt. |

Two Bezel-specific facts drive the "first-party" conclusion: the SCC compilation unit and the module-name→artifact mapping are semantics no generic tool knows (they can only ever be executors under a Bezel-aware layer), and the same two facts make the first-party driver *small* — the hard parts (dependency semantics, artifact format, assembly rules) are already specified and partially implemented in `bezelc`. This is also the option consistent with the project's standing no-new-dependencies discipline: adopting Bazel is a major dependency; a graph walk over existing compiler primitives is not.

## 3. The design: a staged first-party driver

### Stage 0 — the kernel (= #1398, compiler track)

A minimal driver — plausibly a `bezelc` mode rather than a new binary — that makes multi-module programs build at all:

1. **Discover**: from a root directory, parse `module`/`import` headers of every `.bzl` file (header-only parse; no full front-end pass), map module names to source sets by convention (one module = the files declaring it; lookup by name across the root).
2. **Condense**: Tarjan SCC over the import graph; reject self-import (§2.8.1 rule 4); the condensed graph is a DAG.
3. **Order & invoke**: topological order; one compiler invocation per SCC (all member sources presented together — the §2.5 semantics sema needs; within-SCC refs resolve without artifacts), `--import`-chaining each SCC's dependency artifacts.
4. **Assemble**: kind from the build request (flag at this stage); `Main` resolution per §2.8.4 + #1734's widening; link via the existing clang path; duplicate-module and closure checks at metadata time (the driver owns the graph, so these are free).

No manifest, no cache, path conventions only. This alone unblocks multi-module dogfooding, and it is the piece proposal 0022 (test framework) and the compile-time-DI exploration both name as their missing host.

### Stage 1 — `bezel build` proper

- **Manifest** (name TBD, `bezel.toml` strawman), deliberately minimal — it declares *only what source cannot*: project name; **artifact kind** (required by §2.8.4); `main-class` when ambiguous; external dependency roots (paths at this stage). The internal module graph stays source-derived — no target declarations, ever.
- **Content-addressed action cache** with two-level keying (§4) — generalizing the in-tree AOT test link-cache design.
- **Parallel SCC-DAG walk** (the condensed DAG is embarrassingly parallel across independent branches; one process per SCC invocation initially).
- **Subcommands**: `bezel build`, `bezel run`; **hook points** (not implementations) for `bezel test` (proposal 0022's discovery mode) and a pre-compile generate step (the DI exploration's request — a declared script that runs before graph discovery).

### Stage 2 — packages (deferred by design)

Path dependencies → git dependencies + a lockfile → a registry (much later, if ever needed). §2.8.5 already fences this off; nothing in Stages 0–1 forecloses any choice here, and no design is attempted now beyond the manifest keeping a `deps` table whose values are "where artifacts/sources come from."

### Stage 3 — optional, org-scale

Official Bazel/Buck2 rules as a *secondary* integration (remote caching, hermetic CI), generated from the same graph library Stage 1 uses — the `gazelle` pattern, so BUILD files never drift from source. Only if a monorepo-scale consumer materializes.

## 4. The cache: two-level content-addressed keying

The AOT test harness already ships a content-addressed link-cache (SHA-256 key over source + compiler + options; hit = re-run the identical binary). The build cache is the same idea with one refinement that the no-stable-ABI model makes sound:

- **Compile action** (per SCC): key = hash of (member source closure, **dependency *interface* hashes** — the `.bezel.meta` sections, not whole artifacts —, `bezelc` binary, flags). Consequence: when a dependency is rebuilt but its interface is byte-identical (a body-only change that doesn't alter the exported surface — noting that exported *generic* bodies are part of the interface, so generic-body edits correctly invalidate), dependents get **early cutoff** and do not recompile. In a recompile-the-world model this is the single biggest incremental win available.
- **Link action**: key = hash of (object set, runtime library, link flags).

Prerequisite to verify before relying on cutoff: **deterministic `.bezel.meta` emission** for unchanged input (no timestamps, no pointer-ordered iteration in the metadata builder). Unverified today; if emission is nondeterministic, that is a small compiler fix to make first (open question Q2).

## 5. Open questions

- **Q1 — Packaging of the driver**: `bezelc build` subcommand (in-process reuse of `Driver`, one binary to ship) vs a separate `bezel` binary (cleaner CLI story, the eventual `bezel test`/`fmt`/`doc` umbrella). Lean: start as a `bezelc` mode in Stage 0 (it is compiler-track work anyway), split the `bezel` umbrella binary at Stage 1.
- **Q2 — `.bezel.meta` determinism** (prerequisite for interface-hash cutoff): audit `ModuleMetadataBuilder` for ordering/timestamp nondeterminism; pin with a test (same input twice → byte-identical section).
- **Q3 — Manifest name/format**: `bezel.toml` vs `.bzlproj`-style; TOML assumed for familiarity, undecided.
- **Q4 — Parallel link budget**: per-SCC clang links compete with compile parallelism (the test-infra lesson: links dominate small programs); the walk needs a link-concurrency cap like the harness's `BEZEL_AOT_LINK_TOKENS`.
- **Q5 — Test discovery convention**: owned by proposal 0022; this driver only commits to hosting the hook.
- **Q6 — Incrementality *within* an SCC**: none — the SCC is atomic per spec. Worth stating explicitly so nobody attempts per-file incrementality inside a unit later.

## 6. Non-goals (this proposal)

Package registry design; version arbitration semantics; conditional compilation (declined by the spec — platform variance is file-set variance); dynamic loading (§32.14 deferral); IDE/LSP integration (ecosystem pillar 3 — though the Stage-1 graph library should be written as a reusable component with it in mind); reproducing the spec's assembly rules in prose here (they are §2.8's, not this document's).

## 7. Relations

[#1908](https://github.com/s8lap/bezel-lang/issues/1908) (tracking) · [#1904](https://github.com/s8lap/bezel-lang/issues/1904) (ecosystem umbrella, pillar 1) · [#1398](https://github.com/s8lap/bezel-lang/issues/1398) (= Stage 0) · [#1734](https://github.com/s8lap/bezel-lang/issues/1734) (`--main-class`, Stage-0 co-requisite) · proposal [0034](0034-module-identity.md)/spec §2.8 (the pinned model) · proposal [0022](0022-test-framework.md) (blocked on this driver for discovery) · [`exploration-compile-time-di.md`](exploration-compile-time-di.md) (pre-compile hook slot) · [#428](https://github.com/s8lap/bezel-lang/issues/428) (generic source-text export — why artifacts are self-contained).
