# Proposal 0040 — The build system: a staged first-party project driver (`bezel build`)

| | |
|---|---|
| **Status** | under-review — draft (design sketch — owner-requested 2026-07-16; **refreshed 2026-09-17 to v0.306 with the Stage 0 implementation brief, §7** — owner acceptance of the staging is the next action; Stage 0a below is schedulable during the bug-fixing campaign because it touches no lowering) |
| **Tracking issue** | [#1908](https://github.com/s8lap/bezel-lang/issues/1908) (ecosystem pillar 1 of [#1904](https://github.com/s8lap/bezel-lang/issues/1904)) |
| **Target module(s)** | Tooling only — a build driver over the existing compiler surface. **No spec changes**: this proposal *implements* spec [§2.8](../Bezel_Language_Spec.md#sec-module-identity) (module identity / artifact interface / program assembly, landed v0.188 from proposal [0034](0034-module-identity.md)) and [§2.5/§2.6](../Bezel_Language_Spec.md#sec-dependency-resolution)'s SCC compilation units (settled v0.164). Stage 0 is exactly compiler tracker [#1398](https://github.com/s8lap/bezel-lang/issues/1398). |
| **Written against** | spec/stdlib **v0.196**, compiler @ `974be95`; **re-verified 2026-09-17 against v0.306**, compiler @ `29410e29` — §2.5, §2.8.1–§2.8.5, §33.1 and §33.5 keep their numbers and anchors (`sec-dependency-resolution`, `sec-module-identity`, `sec-program-assembly`); one section is new since the pin, **§2.8.6 *Interface delta classification*** (proposal 0044, v0.204), which is exactly the classification the §4 cache's interface-hash cutoff wants and is recorded there |
| **Origin** | The 2026-07-16 ecosystem discussion (internal-dogfooding ordering, [#1904](https://github.com/s8lap/bezel-lang/issues/1904)); constraint extraction from §2.8/0034/#1398 and a Driver-capability audit done for this sketch. The caching design generalizes the AOT test harness's content-addressed link-cache (in-tree since 2026-07-15, `tests/support/aot_support.hpp`). |
| **Design influences** | Cargo and `go build` (toolchain-integrated driver; convention over configuration; the manifest declares only what source cannot); Go specifically for the source-derived dependency graph (Bezel's `import Name;` headers make the graph parseable, as Go's imports do); Bazel/Buck2 (evaluated and *declined* for the core — their per-target BUILD files would duplicate a source-derivable graph; kept as a possible later secondary integration for remote caching); Ninja (evaluated and declined as the executor — its timestamp model is weaker than the content-addressed cache we already have); C++ modules as the avoided cautionary tale (Bezel artifacts are self-contained, so there is no separate interface-file orchestration problem to solve). |

## 0. What this is, and what it is not

This is the design sketch for how Bezel projects get built: multi-module programs, libraries, dependencies, incrementality, and the host tool for `bezel run` / `bezel test` / future generate steps. It is **not** a spec change — §2.8 already pins the build *model* (deliberately, as build-system-agnostic invariants), and §2.8.5 explicitly fences off package management, versioning, and manifest format as tool concerns. This proposal fills exactly the space the spec left to the tool.

It is also not a greenfield mandate: **Stage 0 below is [#1398](https://github.com/s8lap/bezel-lang/issues/1398)**, an already-open compiler tracker (§2.5 SCC compilation is normative and unimplemented). The staging is designed so each stage is independently useful and the first one is ordinary compiler-track work.

Today's state, for the record (re-audited 2026-09-17 at `29410e29`, by source read — no build was available, [#4451](https://github.com/s8lap/bezel-lang/issues/4451)): `bezelc` still compiles **one source file** per invocation (`src/main.cpp` rejects a second positional argument with "more than one input file given"); `-c` emits a library object with an embedded `.bezel.meta` interface (schema version 7, `src/module/ModuleMetadata.hpp:291`); `--import <artifact>` (repeatable) consumes dependencies, deduplicated per module name by §2.8.1 rule 3 in `ModuleLoader::loadDependencies` (`src/module/ModuleLoader.hpp:87–:106`); linking is via clang. There is **no graph discovery of any kind** and **no `--main-class`** ([#1734](https://github.com/s8lap/bezel-lang/issues/1734) is open) — "the build system" is currently a human sequencing `bezelc -c` / `--import` calls in dependency order by hand, which is what the two-module lit tests do in their `RUN:` lines (`tests/lit/import_alias_collision_1489_*.bzl`).

What *has* landed since the pin, and matters here: `Driver` now exposes the two halves of an executable build separately — `compileExecutableObject` and `linkExecutableObject` (`src/driver/Driver.hpp`, the "two halves" block) — plus `linkCommandLine`, `linkRecipeSignature`, `runtimeLibraryPath` and `preludeSourceFingerprint`, all added so the AOT test harness could key a content-addressed cache on each half's own inputs. That is the §4 cache's primitive set, already in the compiler, already exercised by ~2,000 tests; Stage 1 reuses it rather than generalising the harness. The dependency-`Main` inertness half of program assembly is done ([#1511](https://github.com/s8lap/bezel-lang/issues/1511) closed): a library artifact's qualifying `Main` no longer emits a native `main`.

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

Prerequisite to verify before relying on cutoff: **deterministic `.bezel.meta` emission** for unchanged input (no timestamps, no pointer-ordered iteration in the metadata builder). **Read 2026-09-17** (`src/module/ModuleMetadataBuilder.cpp`, 2,500+ lines): no `chrono`/`time`/`__DATE__` anywhere; member and type emission walks `t.members` vectors in declaration order (`:1789`, `:1987`, `:2199`, `:2440`); the one order-sensitive region is the generic-body source-text qualifier, which iterates `ctx.allExprSymbols()` — an `std::unordered_map<const ast::Expr*, Symbol*>` (`src/sema/SemaContext.hpp:175`) — at `:541`, `:573` and `:625` to fill a `replacements` map keyed by token offset and then applies it in token order (`:637`). That is deterministic **unless two expression nodes claim the same offset**, in which case the last writer wins in pointer-hash order. So Q2 is *probably yes* by reading, with one named hazard; it is settled by the pinning test §7 step 0 specifies, not by this paragraph. §2.8.6's interface-delta classification (0044, landed v0.204) is the natural key refinement once determinism is pinned: an `identical` delta is the cutoff, a `preserving` one could be too.

## 5. Open questions

- **Q1 — Packaging of the driver**: `bezelc build` subcommand (in-process reuse of `Driver`, one binary to ship) vs a separate `bezel` binary (cleaner CLI story, the eventual `bezel test`/`fmt`/`doc` umbrella). Lean: start as a `bezelc` mode in Stage 0 (it is compiler-track work anyway), split the `bezel` umbrella binary at Stage 1.
- **Q2 — `.bezel.meta` determinism** (prerequisite for interface-hash cutoff): audit `ModuleMetadataBuilder` for ordering/timestamp nondeterminism; pin with a test (same input twice → byte-identical section). **Audited by reading 2026-09-17 (§4): no timestamps; one order-sensitive site named; the pinning test is §7 step 0.**
- **Q3 — Manifest name/format**: `bezel.toml` vs `.bzlproj`-style; TOML assumed for familiarity, undecided.
- **Q4 — Parallel link budget**: per-SCC clang links compete with compile parallelism (the test-infra lesson: links dominate small programs); the walk needs a link-concurrency cap like the harness's `BEZEL_AOT_LINK_TOKENS`.
- **Q5 — Test discovery convention**: owned by proposal 0022; this driver only commits to hosting the hook.
- **Q6 — Incrementality *within* an SCC**: none — the SCC is atomic per spec. Worth stating explicitly so nobody attempts per-file incrementality inside a unit later.
- **Q7 — Stage 0 split (new, 2026-09-17)**: §7 proposes landing Stage 0 as **0a** (acyclic graphs: driver-only, no compiler change) before **0b** (true SCC units: the multi-file sema unit). The split is a sequencing choice the spec does not make — §2.5 defines the unit, not the order in which a tool learns to build it — and is recorded here per CLAUDE.md rule 4 so it can be revisited. The alternative (0b first, so every graph builds correctly on day one) is declined for now because 0b's sema change lands in the front-end during the bug-fixing campaign, and 0a already unblocks every acyclic multi-module program, which is every program in the repository today.

## 6. Non-goals (this proposal)

Package registry design; version arbitration semantics; conditional compilation (declined by the spec — platform variance is file-set variance); dynamic loading (§32.14 deferral); IDE/LSP integration (ecosystem pillar 3 — though the Stage-1 graph library should be written as a reusable component with it in mind); reproducing the spec's assembly rules in prose here (they are §2.8's, not this document's).

## 7. Stage 0 implementation brief (#1398) — added 2026-09-17

*Written at `main` @ `29410e29` without a build ([#4451](https://github.com/s8lap/bezel-lang/issues/4451)): every implementation claim is a source read cited by file and line. The brief exists so that the implementer of [#1398](https://github.com/s8lap/bezel-lang/issues/1398) starts from a work-list, not from this sketch.*

### 7.0 The honest size of Stage 0

Stage 0 as §3 states it is two very different pieces of work wearing one name:

| Piece | What it is | Compiler contact | Size |
|---|---|---|---|
| **0a — the driver over an acyclic graph** | discover → order → invoke `bezelc -c`/`--import` per module → link | `src/main.cpp` (a new mode), `src/driver/` (a new file), the parser gains one public header-only entry | small; touches no sema, ownership or codegen path |
| **0b — the SCC compilation unit** | several `SourceFile`s in one `SemaContext`, resolving each other's names without artifacts (§2.5's normative unit) | `Driver::compileExecutableObject`'s single-`unit` pipeline (`src/driver/Driver.cpp` ~`:210–:226`: `resolveTypes`, `typecheckExpressions`, `checkPureBodies`, `checkDefiniteAssignment`, `checkStaticInitCycles`, `checkOwnership` all take **one** `ast::SourceFile`), plus codegen emitting one artifact per member module from one context | medium-to-large; front-end work in the passes the defect campaign is stabilising |

Every multi-module program in the repository today is acyclic (the two-module lit fixtures, the `--import` tests under `tests/module/`), so **0a alone makes multi-module dogfooding real**, and it can land during the bug-fixing campaign without touching a family in [`defect-campaign-2026-09.md`](papers/defect-campaign-2026-09.md). 0b is the spec's unit and is not optional; it is sequenced second because it is front-end work. A cycle under 0a is a clean, named build error ("import cycle A → B → A; cyclic modules need SCC compilation, not yet implemented — #1398"), which is strictly better than today's unresolved-type errors from the middle of the cycle.

### 7.1 Step 0 — the two pins, before any driver code

1. **`.bezel.meta` determinism test** (settles Q2): compile the same source twice through `Driver::compileObject`, extract the metadata section from both objects, assert byte-identical; then a second case whose generic body contains two member accesses at nested positions (the `allExprSymbols()` hazard named in §4) — same assertion. Lives in `tests/module/`, on the AOT harness. If the second case fails, the fix is to iterate a sorted view of `allExprSymbols()` in `ModuleMetadataBuilder.cpp` (`:541`, `:573`, `:625`) — a local change.
2. **`--main-class` (#1734)**: one flag in `src/main.cpp`, threaded to the §33.5 entry-point resolution; rejects a name not in the root compilation with a diagnostic citing §33.5. Rides 0a because the driver's `Main` search across a several-module root is where ambiguity first becomes reachable.

### 7.2 Step 1 — discovery: a header-only parse

`Parser::parseCompilationUnit()` (`src/parser/Parser.hpp:30`) is the only public entry; `parseModuleDecl()` and `parseImport()` are private (`:99–:100`). Discovery needs only `SourceFile::module` and `SourceFile::imports` (`src/ast/Node.hpp:1537–:1541`), so add one public entry, `parseHeader()`, which parses the module declaration and the import list and **stops at the first top-level declaration** — no `topDecls`, no error recovery beyond the header. A full parse per file would work but is wasteful for large trees and, more importantly, would surface every syntax error in every file at discovery time instead of at that module's compile step; the choice is recorded here (CLAUDE.md rule 4).

Discovery walks the root directory for `*.bzl`, parses each header, and builds `moduleName → {sourcePaths}`. Two files declaring the same module in one root is a §2.8.1 duplicate-module error at discovery. A file whose header fails to parse is reported and excluded; the build fails at the end, not immediately, so one broken file does not hide the graph.

### 7.3 Step 2 — condense

Tarjan over the import graph (nodes: module names; edges: `import X;`). Self-import is a §2.8.1 rule-4 error. Under 0a, any SCC of size > 1 is the named cycle error above. Under 0b, an SCC of size > 1 is a unit. An `import` naming a module that is neither in the root nor supplied by an external artifact path is an error naming the module and the file that imports it.

External artifacts (already-built `.o` files passed on the command line) are leaves: their module names come from their `.bezel.meta` (the loader already reads them, `ModuleLoader::loadDependencies`), and they participate in name resolution but not in ordering.

### 7.4 Step 3 — order and invoke

Topological order over the condensed DAG. For each module in order: `bezelc -c <sources> --import <dep artifact>…` for every *transitive* dependency artifact (§2.8.3: the closure, not the direct edges — the loader's rule-3 dedup makes over-supplying harmless), output to a per-build scratch directory keyed by module name. The root module (the one holding the selected `Main`, or the single module with no importers when unambiguous) is compiled last as an executable with the full closure imported.

Invocation is **in-process** under 0a's lean for Q1 — `Driver` is a library, so the driver mode calls `driver.compileObject(...)` per module rather than exec'ing `bezelc` — with one recorded caveat: the driver's diagnostics today print straight to stderr from inside `Driver` (`Driver.cpp:632`, `:644`, `:773`, `:785`); a multi-module build wants them prefixed by module, which is the same renderer consolidation proposal 0052 §9.2 does for its trailing code tag. Do it once.

Parallelism is Stage 1 (Q4); Stage 0 walks serially.

### 7.5 Step 4 — assemble

Kind from the build request: `bezelc build --kind exe|lib <root>` (flag now, manifest at Stage 1). The §2.8.4 checks the driver can do for free because it owns the graph: exactly one qualifying `Main` in the root compilation (`--main-class` to disambiguate), dependency `Main`s inert (#1511 done), no executable artifact among the imports, no duplicate module across the closure. Link via `Driver::linkExecutableObject` with every closure object.

### 7.6 Tests Stage 0a owes

- A three-module diamond (`A` imports `B` and `C`, both import `D`) built by one driver invocation, run, exit code asserted — the first test in the repository that builds a program without a human sequencing `--import`.
- The same diamond with `D` supplied as a pre-built external artifact (leaf path).
- Duplicate module in root → the §2.8.1 error text; self-import → rule 4; unknown import → names file and module; a cycle → the 0a named error (this row is **replaced**, not deleted, when 0b lands: it becomes the first SCC-unit test).
- Two qualifying `Main`s in the root without `--main-class` → §33.5 error; with it → builds.
- The determinism pin (§7.1).

All on the AOT harness, none under the JIT oracle (CLAUDE.md's ratchet).

### 7.7 Stage 0b, stated so it is not underestimated

The unit is several `SourceFile`s in one `SemaContext`. Every pass in `Driver::compileExecutableObject`'s pipeline takes one `ast::SourceFile&`; 0b either (i) runs each pass over each file in turn against the shared context, which requires the passes' cross-file references to resolve lazily (today they resolve against already-loaded artifact metadata, `ModuleLoader`), or (ii) introduces a `CompilationUnit { vector<SourceFile> }` that the passes iterate. Option (ii) is the honest shape and the larger change; option (i) risks a per-pass sweep of "does this pass assume one file" — the kind of duplicated-assumption hunt the census documents. **Decide (i) vs (ii) in the 0b brief with the passes read, not here**; what this proposal fixes is only that 0b's tests are the 0a cycle-error rows flipped to success, plus §2.8.2's requirement that each member module still emits its own self-contained artifact.

## 8. Relations

[#1908](https://github.com/s8lap/bezel-lang/issues/1908) (tracking) · [#1904](https://github.com/s8lap/bezel-lang/issues/1904) (ecosystem umbrella, pillar 1) · [#1398](https://github.com/s8lap/bezel-lang/issues/1398) (= Stage 0) · [#1734](https://github.com/s8lap/bezel-lang/issues/1734) (`--main-class`, Stage-0 co-requisite) · proposal [0034](0034-module-identity.md)/spec §2.8 (the pinned model) · proposal [0022](0022-test-framework.md) (blocked on this driver for discovery) · [`exploration-compile-time-di.md`](../research/exploration-compile-time-di.md) (pre-compile hook slot) · [#428](https://github.com/s8lap/bezel-lang/issues/428) (generic source-text export — why artifacts are self-contained).
