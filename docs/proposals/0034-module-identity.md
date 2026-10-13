# Proposal 0034 — Module identity, artifact interface, and program assembly (the back half of §2)

| | |
|---|---|
| **Status** | graduated-v0.188 — **superseded — graduated in v0.188 (`ebfa7e0`)**: new spec §2.8 (+ §2.x/§13.4/§27.8/§33.1 cross-refs, Appendix C.12/C.13); entry-point widening (OQ5) recorded in §33.1. Open questions resolved as delegated rulings (recorded in the v0.188 changelog). *(prior: accepted (owner, 2026-07-12); draft (RFC))* |
| **Tracking issue** | [#1517](https://github.com/s8lap/bezel-lang/issues/1517) |
| **Target module(s)** | None (spec-only). Spec §2 (new §2.8; small edits to [§2.1](../Bezel_Language_Spec.md#sec-module-declarations)/[§2.4](../Bezel_Language_Spec.md#sec-compilation-unit)/[§2.5](../Bezel_Language_Spec.md#sec-dependency-resolution)/[§2.6](../Bezel_Language_Spec.md#sec-build-phases)), [§13.4](../Bezel_Language_Spec.md#sec-modules-and-internal), and cross-references from [§33.1](../Bezel_Language_Spec.md#sec-the-main-method)/[§33.5](../Bezel_Language_Spec.md#sec-program-entry-point-resolution)/[§33.6](../Bezel_Language_Spec.md#sec-example). No stdlib surface. |
| **Written against** | spec/stdlib **v0.178** |
| **Origin** | The 2026-07 module-system design-review deep dive. Probes live under `/var/tmp/bezel-scratch/specrun/modsys/` (drafting) and `/var/tmp/bezel-scratch/specrun/crit0034/`+`/var/tmp/bezel-scratch/specrun/rev0034/` (review pass), compiled and run against `build/bezelc` (HEAD `05e992f`); the duplicate-module, self-import, and entry-point probes were re-run while drafting this document. Evidence bugs filed from the same dive: [#1487](https://github.com/s8lap/bezel-lang/issues/1487), [#1488](https://github.com/s8lap/bezel-lang/issues/1488), [#1489](https://github.com/s8lap/bezel-lang/issues/1489); [#1511](https://github.com/s8lap/bezel-lang/issues/1511) was found and filed while drafting §5 here; [#1513](https://github.com/s8lap/bezel-lang/issues/1513) (cross-artifact generic static state) and [#1514](https://github.com/s8lap/bezel-lang/issues/1514) (strong `bz$Console$Dispose` in every artifact) were found and filed during review; [#1515](https://github.com/s8lap/bezel-lang/issues/1515) is the §2.2 spec-side residue split out of #1489. |
| **Design influences** | C#/.NET assemblies (`InternalsVisibleTo` deliberately *not* copied; assembly-qualified type identity *is* the model for §4), Rust crates (crate = compilation unit = privacy boundary — the closure-by-compilation ruling), C++ modules (the cautionary tale: interface units that must carry template bodies, which is exactly Bezel's generic-export problem), Go (one-package-one-directory identity; `main` package as the assembly root) |

## 0. What this is, and what it is not

The **front half** of spec §2 landed in v0.164 and is settled: import syntax and aliases, the collision rule, [§2.7](../Bezel_Language_Spec.md#sec-name-resolution-order) dotted-name resolution, and SCC compilation units ([rationale v0.164 changelog](../Bezel_Design_Rationale.md#sec-v0-163-to-v0-164-changelog)). **Nothing in this proposal redesigns any of it.** The one genuine spec defect inside that layer — §2.2's alias-collision list omits imported bare names, so `import Baz as Bar;` is legal while an imported bare `Bar` exists, yet §2.7 rule 1's no-backtracking order (imported bare names outrank aliases) makes that alias silently dead — is a one-sentence fix in its own lane, filed as [#1515](https://github.com/s8lap/bezel-lang/issues/1515) and deliberately not folded in here. (Its compiler shadow is [#1489](https://github.com/s8lap/bezel-lang/issues/1489) — the compiler *backtracks* alias-first against §2.7's clear order; that is an implementation bug against settled text, not a spec question, and it too stays in its own lane.)

What v0.164 did not write is everything *downstream of a single compilation*, and that half is currently an honor system:

- §2 never says a module is unique. Membership is claimed by declaration ([§13.4](../Bezel_Language_Spec.md#sec-modules-and-internal): "A module is the set of all source files declaring the same `module Name.Path;`"), program-wide, so **`internal` is encapsulation by etiquette** — any code anywhere joins a module by declaring its name. Probe (re-run at HEAD `05e992f`): two separately-compiled artifacts both declaring `module Dup` link and run silently (`dupuse` prints `alpha` then `beta`); a file declaring `module Dup;` *plus* `import Dup;` also compiles silently — and gets a third, nowhere-specified semantics (the artifact's publics are visible, its internals are not).
- §2.5's artifact-interface definition is a closed three-item list — "type signatures, method signatures, generic constraints" — that **cannot support the language's own cross-module generics**: Bezel monomorphizes, so instantiating an imported `Box<T>` at a consumer-local type argument needs the generic's *bodies*. The reference compiler already ships far more than the list says (declaration source text, `src/module/ModuleMetadataBuilder.cpp`), and the #428/#710/#711/#1222 implementation saga is five weeks of cost from the spec never stating the requirement. The runtime half of the same hole is broken today: a generic type's static state touched through two consumer artifacts is duplicated-or-garbage — the [§22.12](../Bezel_Language_Spec.md#sec-static-members) "exactly once process-wide" guarantee silently evaporates at artifact boundaries (probe: `Reg<int>` bumped from two libraries prints `0`/`0`, exit 0; same-module baseline `1`/`2` — #1513).
- Transitive dependencies are unstated, and the unpinned corner **misbehaves silently today**: a method call on a value whose type is reached through a middle module's public API returns garbage — `"0"` instead of `"payload"`, exit 0, no diagnostic (#1487).
- "Main module" is used exactly once in the spec ([§33.1](../Bezel_Language_Spec.md#sec-the-main-method)) and never defined; executable-vs-library determination is implied by one example sentence ([§33.6](../Bezel_Language_Spec.md#sec-example)); and a dependency artifact that happens to contain a qualifying `Main` breaks every consumer's link with `multiple definition of 'main'` (#1511, found while drafting this proposal).

This proposal writes the back half: a new **§2.8 "Module identity, artifact interface, and program assembly"** with four rulings and an explicit non-goals list, plus the small consistency edits to §2.1/§2.4/§2.5/§2.6/§13.4/§33 that the rulings imply. House rule throughout: **each ruling matches current compiler behavior where it exists and is sensible** (cited to probes), and minimally corrects it where it is broken (the filed evidence bugs: #1487, #1488, #1511, #1513, with #1514 as a linked practice blocker).

Terminology used below, consistent with §2.4/§2.5 as landed:

- **Compilation (unit)** — one compiler invocation over the sources of one module, or of one import-cycle SCC ([§2.5](../Bezel_Language_Spec.md#sec-dependency-resolution)); it emits one artifact per member module.
- **Root compilation** — the compilation the build is currently performing (the sources on the command line, as opposed to the `--import`ed artifacts).
- **Build / program assembly** — presenting a root compilation plus a set of dependency artifacts to produce a final artifact.
- **Program** — an executable together with the dependency artifacts assembled into its process.

## 1. Ruling 1 — Module identity and closure (§2.8.1)

> **Proposed spec text (sketch).**
>
> 1. **One module, one compilation, one artifact.** Every module in a program is the product of exactly one compilation. A module's membership is fixed by the source files presented to that compilation; no other source file becomes part of the module by declaring its name. An SCC compilation ([§2.5](../Bezel_Language_Spec.md#sec-dependency-resolution)) is one compilation producing one artifact per member module; each member module is still produced exactly once.
> 2. **`internal` is closed by compilation, not by name.** `internal` ([§13.1](../Bezel_Language_Spec.md#sec-levels)) grants access only among the source files compiled together as the declaring module. Co-compilation does not widen it: within an SCC unit, each member module's internals remain invisible to the other members. *(Amends [§13.4](../Bezel_Language_Spec.md#sec-modules-and-internal); see §6.)*
> 3. **Duplicate module names are a build error.** A build whose artifact set — the root compilation plus the dependency closure ([§2.8.3]) — contains two **distinct** artifacts declaring the same module name, or a dependency artifact declaring a module name that the root compilation also declares, is ill-formed; the diagnostic names both origins. Presenting the *same* artifact more than once (the same file, or bit-identical content — as when a build tool concatenates per-dependency closures that share a diamond base) is idempotent: duplicates are deduplicated, not diagnosed. Within one process there is exactly one artifact per module name (runtime dynamic loading is out of scope, [§32.11](../Bezel_Language_Spec.md#sec-reserved-for-future-revisions)).
> 4. **Self-import is a compile error.** An `import` naming the enclosing file's own module is rejected ("a module cannot import itself"). Import *cycles between distinct modules* remain legal and compile as an SCC (§2.5); the self-loop is not a cycle, it is a mistake.
> 5. **Implementation-provided modules.** The standard-library modules (the [§2.3](../Bezel_Language_Spec.md#sec-always-available-names) always-available set and the modules of the standard-library document) are provided by the implementation, not by any presented artifact. They are implicit members of every build's artifact set: exempt from the presentation requirement of [§2.8.3] rule 1, resolvable by `import` with no artifact supplied. They **do** participate in duplicate-name detection: a root compilation or dependency artifact declaring a module name the implementation provides (`module System;`, `module System.Threading;`, …) is the rule-3 build error.

**Why closure-by-compilation.** [§13.1](../Bezel_Language_Spec.md#sec-levels) defines `internal` as visible to "Code in the same module"; under §13.4's current purely-nominal definition, that is a boundary anyone can walk through by typing `module Dup;`. Every language with a module-privacy construct closes it by *unit of build*, not by name (Rust: the crate; C#: the assembly; Go: the package directory); Bezel's spec simply never says so. Rule 2 says it, and costs no expressiveness: files that want to share internals were always required to be "the same module", and the same module is compiled together (§2.4). The SCC clause in rule 2 preserves [rationale §2.1](../Bezel_Design_Rationale.md#sec-dotted-module-names-are-one-name)'s no-implicit-visibility stance under co-compilation — an SCC is a compilation-order grouping, not an access grant.

**Why duplicate names are an error rather than a merge.** The alternatives are (a) merge silently — today's accidental behavior, and the encapsulation hole: the second artifact's code would be "in the same module" for §13.1 purposes; (b) first-wins/last-wins — silent re-meaning, the exact property [§2.7](../Bezel_Language_Spec.md#sec-name-resolution-order) was designed to exclude at the name level; (c) error naming both. Only (c) preserves both encapsulation and the no-silent-re-meaning discipline. The distinct-artifact wording plus the idempotence sentence carve the space exactly: *identical* duplicates are the innocent diamond-assembly case (deduplicated), while *distinct* artifacts under one module name are precisely the **stale artifact** case — an old and a new compilation of `Base` supplied together gets the error naming both, instead of whichever-loads-first semantics.

**Why the standard library must be carved out (rule 5).** Rule 1 here and [§2.8.3] rule 1 quantify over "every module in a program", but the stdlib modules are the product of *no* presented compilation: `System`/`System.Text` are auto-imported ([§2.3](../Bezel_Language_Spec.md#sec-always-available-names)), and an explicit `import System.Threading;` resolves today with no artifact supplied (probe `crit0034/gbase.bz`: compiles with `--import`-ing nothing for it — the stdlib is compiler-known and runtime-provided). Without rule 5, every real build is ill-formed under [§2.8.3] rule 1's presentation requirement the moment it touches `Console`. The flip side is the worst instance of the etiquette hole this ruling exists to close: a user file declaring `module System;` is today a silent internals-adjacent grab of the implementation's namespace (probe `rev0034/sysgrab.bz`: compiles and runs, exit 0). Rule 5 exempts the stdlib from presentation while keeping it inside duplicate-name detection, closing both at once.

**Why self-import is an error rather than a no-op.** Three semantics are on the table today, none specified: the spec's literal reading (the file *is* the module, so the import is a tautological no-op), the compiler's actual behavior (probe `dup_attack.bz`/`dup_attack2.bz`: the same-named *artifact* becomes visible as if foreign — publics yes, internals no — a third meaning nobody designed), and rejection. A tautological no-op has one non-tautological effect under §2.2 — it would re-add the module's own publics as "imported bare names", perturbing collision arithmetic for zero benefit. Rejection is the only reading with nothing to specify downstream.

**Compiler alignment (each correction one diagnostic):**

| Probe (re-run at `05e992f`) | Today | Under this ruling |
|---|---|---|
| `dup1.o` + `dup2.o` both `module Dup`, consumer imports `Dup` | links, runs, prints `alpha`/`beta` — silent merge | build error naming `dup1.o` and `dup2.o` |
| `module Dup;` + `import Dup;` against `dup1.o` | compiles; artifact's publics visible, internals `unresolved identifier` | compile error at the `import` (self-import) *and* build error (root compilation redeclares a dependency's module name) |
| `module SelfImp; import SelfImp;` with no artifact | compiles and runs silently | compile error at the `import` |
| `module System;` in a user file (`rev0034/sysgrab.bz`) | compiles and runs silently | build error (rule 5: implementation-provided name) |
| `import System.Threading;` with no artifact | resolves (stdlib is compiler-known) | unchanged — blessed by rule 5's presentation exemption |

Note the duplicate-name check has an implementation prerequisite: today two modules exporting a same-named *type* cannot link at all, because symbol mangling omits the module (`bz$Tx$Hello` from both — #1488). Module-qualified mangling is needed both for #1488's own configuration (two modules, same public type name, qualification per §2.2 — spec-supported, currently unlinkable) and to make rule 3's diagnostic fire before the native linker's. A sibling blocker in the same "fire before the native linker" family: runtime-support symbols are emitted strong in every artifact (`bz$Console$Dispose` — [#1514](https://github.com/s8lap/bezel-lang/issues/1514), filed during review), so any Console-using `-c` library currently fails to link into a Console-using executable at all; like #1488 it gates multi-artifact practice, not this ruling's design.

## 2. Ruling 2 — The artifact interface is a semantic requirement (§2.8.2)

[§2.5](../Bezel_Language_Spec.md#sec-dependency-resolution) currently says the artifact "embeds public-interface metadata (type signatures, method signatures, generic constraints)". As a closed list this is false to the language: Bezel monomorphizes generics per instantiation, with ownership and disposal decided per instantiation, so a consumer creating `new Box<ConsumerType>()` needs `Box<T>`'s constructor and destructor and method *bodies* in some importable form. The reference compiler already does this — it exports generic members' full declaration source text, extracted by AST span, and the importer re-parses and monomorphizes it per instantiation (`src/module/ModuleMetadataBuilder.cpp`, the `methodBodyText`/`ctorBodyText` machinery: "export the full method declaration source text … so the importer can re-parse and monomorphize it per instantiation with `genericSubst_` active"). The spec should state the *requirement* that makes this load-bearing behavior conforming, not enumerate a format.

> **Proposed spec text (sketch), replacing §2.5's parenthetical list.**
>
> A compiled artifact embeds its module's **interface**. The interface's required capability is semantic; its format is implementation-defined (source text, serialized AST, IR, or any mix). It must be sufficient for a consumer, given only the artifact (and the artifacts of its dependencies, [§2.8.3]), to:
>
> 1. **Type-check** every well-formed use of the module's public surface: public (and, for cross-module derivation, `protected`) types and members, with full signatures — ownership modifiers (`owned` returns and parameters, `ref`/`out`, `scoped`), default argument values, generic parameters with variance and constraints, base types and implemented interfaces, enum constants, `const` values, and serialization conformances — and the **derived type properties** ([§3.6](../Bezel_Language_Spec.md#sec-derived-type-properties)): carried as computed verdicts for non-generic public types, and as the structural facts needed for per-instantiation evaluation (§3.6 rule 2) for public generics. (These properties are computed from structure the public surface does not show — private fields, destructor presence — yet §3.6 rule 3 makes them "part of the public contract", and rules dispatch on them at the consumer: sendability at domain boundaries ([§30.14]), trivial-dispose ([§28.9]), element category ([§28.11]). An interface that omits them forces the consumer to either reject imported types at those rules or guess.)
> 2. **Instantiate** every public generic type and generic method at arbitrary module-external type arguments satisfying the declared constraints. An external instantiation behaves identically to a same-module instantiation of the same generic at the same arguments.
> 3. **Identify and describe dependencies**: the interface of every dependency type referenced from the module's public surface *or from any declaration body carried in the interface* (the definition-site clause below), keyed by program-wide type identity ([§2.8.3]), sufficient for the consumer to use such types without holding their declaring module's artifact *at type-check time* (the artifact itself is still required in the build's closure).
>
> **Definition-site visibility.** Visibility inside a generic body is judged at the *definition site*: a public generic's body may reference its own module's `internal` declarations, and an external instantiation binds to them. The interface must therefore carry whatever internal machinery its exported generic bodies reach; internal declarations so carried do not become visible to the consumer's *source* (visibility is unchanged — only the instantiation may bind to them). Carried bodies may equally reach *other modules'* declarations that appear nowhere on the exporting module's public signatures; clause 3's "or from any declaration body carried in the interface" makes those modules describable and puts them in the build's closure ([§2.8.3] rule 1). *(Probe `crit0034/mid.bz`: `Mid`'s public `Box<T>.Weigh` body calls `Dep.Helper.Assist` with `Dep` absent from every public signature; a consumer importing only `Mid` type-checks, and omitting `dep.o` fails only at native link — `undefined reference to bz$Helper$Assist`.)*
>
> An honest cost, stated plainly: definition-site visibility makes a module's `internal` declarations reached by exported generic bodies part of its **binary** interface even though they are absent from its *source* interface — changing one invalidates dependents exactly as a signature change does (a [§2.8.3] rule 3 staleness error until they recompile). Under the recompile-the-world model (§2.8.5's ABI non-goal) that is a build-time inconvenience, not a compatibility trap; it is still the price of monomorphizing generics across artifact boundaries, and C++ paid it for exported templates before giving up on them.
>
> **No restrictions beyond visibility.** Cross-module use of a public generic is not restricted relative to same-module use. An implementation unable to carry some body shape in its interface format must reject the *export or the import with a diagnostic naming the shape*; it must not alter the instantiation's semantics.

**Recording the reference strategy and its costs — honestly.** Source-text embedding is a legitimate format under this rule, and it is what ships. Its consequences, all observed in the #428 family, are worth recording so the next implementer inherits the map rather than the folklore:

- *Internal references inside exported generic bodies* (the definition-site clause above) mean the exported text is not self-contained: it names internals that must themselves be described in the metadata. This is precisely C++'s exported-template lesson — the interface of a generic is transitively its implementation — and it is why clause 2 is stated as a capability, not a byte format.
- *Diagnosed gates are conforming as an interim; silent divergence is not.* #710 (a generic ctor body with an owned local / cross-type call is not exported; the importer diagnoses instead of constructing) is the acceptable shape of an incomplete implementation under the "reject with a diagnostic" sentence — a conformance-documented limitation. #1222 (a generic *method* called at a non-`int` type argument silently reuses the `int` monomorphization) is the forbidden shape: wrong code with no diagnostic. The rule converts that distinction from taste into spec.
- *The gated subset must shrink toward empty.* #428's recorded decline (source-text export shipped as a gated subset) predates this rule; under it, the gate is a named, diagnosable implementation gap, not a language restriction the spec silently implies doesn't exist (today the spec implies *no* restriction while the compiler enforces one — the worst of both).

**Compiler alignment:** clause 1 matches (metadata-driven type-checking of imports works, probes throughout); clause 2's mechanism matches (source-text embedding; cross-module `new Box<int>` runs today) with #710/#1222 as the tracked corrections; clause 3 matches in the observed behavior that a consumer type-checks against `LibM` with only `libm.o` supplied (#1487's "also observed") — and #1487 itself is the identity-unification bug clause 3 hands to §2.8.3.

## 3. Ruling 3 — Transitive dependencies and cross-artifact type identity (§2.8.3)

Three questions §2.5 never answers: who supplies the artifacts of dependencies-of-dependencies; what a consumer may do with a transitively-reachable type it never imported; and when two paths deliver "the same" type, what makes it the same. Probes answer what the compiler does; #1487 shows the corner where doing it without a spec already produces silent garbage.

> **Proposed spec text (sketch).**
>
> 1. **The build supplies the transitive closure.** A build must present the artifacts of the root compilation's direct imports *and, transitively, of every module those artifacts' interfaces reference* — where an interface's references include those made by the declaration bodies it carries ([§2.8.2], definition-site clause), not only its public signatures. A missing member of the closure is a build error naming the missing module and the artifact(s) that reference it. Implementation-provided modules ([§2.8.1] rule 5) are implicit members of every closure and are never required to be presented. *(Diagnosis quality is implementation-defined; a conforming implementation may discover the omission as late as native linking, but a metadata-time diagnostic naming the module is the intended standard.)*
> 2. **Use without import; naming requires import.** A file that imports module `A` may freely **use** values of any public type reachable through `A`'s public surface — receive them (including into `var`/`using var` bindings with inferred types), call their public members, pass them as arguments, return them, and dispose them — without importing the type's declaring module. **Naming** the type in source — in a type ascription, parameter or return type, `new` expression, cast, `is`/pattern type test, `catch` clause, explicit (source-spelled) generic type argument, or extension declaration — requires importing its declaring module (which then also puts the name under §2.2's collision discipline). An instantiation the compiler *infers* at a transitive type (`Util.Process(pay)` inferring `Process<Money>`; a call returning `Box<Money>` bound with `var`) spells no name and remains **use**.
> 3. **Program-wide type identity.** A type is identified program-wide by its **declaring module name, its qualified type name (including any enclosing-type path, [§11.6]), and its generic arity**; a constructed generic type is identified by its generic's identity plus the identities of its arguments. All descriptions of one type — the declaring artifact's own and every copy embedded in dependents' interfaces ([§2.8.2] clause 3) — denote the same type and **must agree**; a build whose closure contains disagreeing descriptions (a stale artifact) is ill-formed, with a diagnostic naming the artifacts and recommending recompilation of the dependents.
> 4. **Identity is one type at runtime, too.** A type — in particular a constructed generic type, whose monomorphized code may be emitted by several consumers' compilations — is one type *in the running process*: exactly **one copy of its static state** and **one recorded initialization** ([§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 1: "a type's static initializer runs exactly once process-wide"; [§22.13]), and **one runtime type identity** — `is` tests, `catch` clauses, and [§19] patterns over values of the instantiation match identically regardless of which artifact produced the value or the test. Duplicate *code* copies of a monomorphization are permitted only where observationally indistinguishable from one copy.

**Why the naming/use split.** The alternative rules are both worse. *Import-required-for-everything* breaks composition: `using var p := Maker.Make();` would demand the consumer know (and import, and re-audit on every refactor of) `LibM`'s internals' provenance — and since disposal is compiler-inserted, even *dropping* the value would be illegal without the import. *Everything-without-import* makes `import` meaningless as a namespace discipline: any transitively-reachable name would flood the file's bare-name space, and §2.2's collision arithmetic would depend on the whole closure rather than the file's stated imports. The split matches the intuition that a value's *capabilities travel with the value* while a *name is a file-scoped privilege* — and it is exactly what the compiler already implements (probe: the consumer of `Maker.Make()` calls members on the result with no `import LibB`, while writing `var p: Payload` fails with "unresolved type name `Payload`" until `import LibB;` is added).

**Why identity must be spec'd, not emergent.** #1487 is half the argument, in three lines of output: `LibB.Payload` reached through `LibM`'s embedded metadata and `LibB.Payload` from `libb.o` are the same type to the type-checker and different types to the generated code — the method call binds to a reconstruction whose ABI disagrees with the object file actually linked, and `p.Describe()` prints `0` with exit 0. Under rule 3 the two descriptions are one type by definition, the implementation is obligated to unify them, and *disagreement* (the genuinely-stale case) is a named build error instead of undefined behavior. Nominal identity also settles the mirror case: two modules each declaring a public `Payload` are two unrelated types (distinguishable in diagnostics by module qualification, linkable only once #1488's module-qualified mangling lands).

**Why rule 4 must be stated, not implied.** Rules 2–3 plus [§2.8.2] clause 2 ("an external instantiation behaves identically") already *imply* one static state and one runtime descriptor per instantiation per process — but the per-module wording of a one-copy guarantee is vacuous exactly where [§22.12] needs it: a monomorphized `Reg<Money>` lives in *no single module's* artifact; each consumer's compilation emits its own copy. The hole is not hypothetical: probe `crit0034/` (filed as #1513) has `GLib1.Touch()` and `GLib2.Touch()` each call `Reg<int>.Bump()` on a shared `GBase` generic with an `AtomicInteger<int>` static — the app prints `0`/`0` with exit 0 (same-module baseline: `1`/`2`). That is silent garbage, worse than mere duplication, and it is the §30 story at its most dangerous: two domains "synchronizing" through what identity declares to be one static `AtomicInteger` or `Channel` actually touch distinct or unconstructed state — the exact race [§30.14](../Bezel_Language_Spec.md#sec-sendable-types) and [§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 2 exist to exclude, reintroduced by the artifact dimension. Rule 4 closes it in the same nominal-identity vocabulary as rule 3.

**Compiler alignment:** the naming/use split matches today's behavior verbatim (probes `trans_use.bz`/`trans_name.bz`); closure supply matches in substance (type-check passes on the middle artifact alone; the missing leaf artifact fails the build at native link — rule 1 blesses that and names the better diagnostic as intended standard); identity unification is the correction — #1487 (rule 3) and #1513 (rule 4) are the evidence bugs. Cost of the rule-3 fix, in one line: the module loader must key every loaded type description by `(module, qualified name, arity)` and resolve all embedded dependency copies to the direct artifact's entry *before* codegen binds ABIs — a loader-level unification map plus an agreement check, moderate size, no new metadata. The rule-4 fix is larger: monomorphizations need process-unique static storage and init guards (module-qualified, instantiation-keyed symbols with linkonce-style deduplication is the standard shape), which lands naturally with #1488's mangling rework.

## 4. Ruling 4 — Program assembly: main module, artifact kind, entry point, one copy per process (§2.8.4)

> **Proposed spec text (sketch).**
>
> 1. **Artifact kind is chosen by the build request.** Whether a compilation produces an executable or a library ([§2.4](../Bezel_Language_Spec.md#sec-compilation-unit)) is a property of the *build request*, not of the source text (the reference CLI: default = executable, `-c` = library; [§2.6](../Bezel_Language_Spec.md#sec-build-phases)'s "build-tool integration is implementation-defined" covers the surface). An **executable** build requires exactly one qualifying `Main` ([§33.2]) in the root compilation, or a `--main-class` selection among several ([§33.5]); zero qualifying `Main`s is an error citing §33.1/§33.2. A **library** build has no entry point; a qualifying `Main` in a library compilation is an ordinary method.
> 2. **The main module, defined.** The **main module** is the module containing the selected entry point. ([§33.1] is reworded to define rather than assume the term; see §6.)
> 3. **Entry-point search scope is the root compilation only.** Only classes of the root compilation are searched for qualifying `Main`s; a qualifying `Main` in a dependency artifact is **inert** — it is never selected, never conflicts under [§33.5], and must not perturb the consumer's build. Consequence: adding a dependency can never break, change, or steal a program's entry point.
> 4. **Dependency artifacts are library-kind.** Presenting an executable artifact as a dependency (`--import`ing a linked executable) is a build error naming the artifact and its kind. *(An executable carries a native entry symbol; admitting it as a dependency re-opens rule 3's perturbation hole from the other side, for no use case a library-kind artifact doesn't serve.)*
> 5. **One copy per process.** How a dependency artifact joins a consuming program — statically embedded in the executable or dynamically loaded alongside it — is implementation-defined, under one normative guarantee: **a process contains exactly one copy of each module's code and, for each type — including each generic instantiation ([§2.8.3] rule 4) — exactly one static state and one runtime identity**, regardless of link mode and regardless of how many compilations emitted code for the type. "Standalone" in §2.4's executable definition describes independence from a language runtime installation, not from the program's own dependency artifacts.
> 6. **Static initialization across artifacts: same rules, one new obligation.** Initialization order is a property of the running program, not of the artifact graph: statics initialize lazily on first use with the runtime recording the single process-wide order ([§22.13](../Bezel_Language_Spec.md#sec-static-initialization-order); [§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 1: "a type's static initializer runs exactly once process-wide"), and exit disposal runs in reverse of that recorded order. What the artifact dimension adds is the obligation rule 5 states: the one-copy guarantee is *not* free where monomorphized instantiations are emitted by several compilations — today it is violated there (#1513, [§2.8.3] rule 4's evidence bug) — and §22.12/§22.13 are well-defined across artifacts only once it holds. The *compile-time* rejection of cross-type initialization cycles ([§22.13]: "Cycles in static initialization across types are rejected by the compiler"; [§2.5]: "regardless of compilation grouping") additionally requires cross-unit dependency edges the artifact interface does not yet carry; pinning that dependency relation and its metadata is deferred to the #804 lane (see open question 7).

**Why build-request, not Main-presence inference — a recorded deviation from the dive's sketch.** The dive recommended "a compilation containing a qualifying `Main` (or told `--main-class`) produces an executable, otherwise a library." This proposal deviates, for two reasons. First, inference has a silent-kind-flip footgun in both directions: deleting the last `Main` from an application quietly produces a `.so` instead of an error; adding a demo `Main` to a library quietly produces an executable. Kind is the most basic fact about a build; it should not be computed from source-text presence. Second, the house rule — match the compiler where sensible — points the same way: the reference compiler already has explicit modes (probe: default mode without a `Main` errors with "program has no entry point … (§33.1, §33.2)"; `-c` is documented "no `Main` required"). [§33.6](../Bezel_Language_Spec.md#sec-example)'s sentence "A library module has no entry point; the compiler produces a shared library artifact" — the only inference-flavored text in the spec — is adjusted to state the same fact without implying kind inference (§6). An implementation remains free to *offer* inference as a tooling convenience; the normative rules above are stated for explicit kinds.

**Entry-point scope: the probe that decided it — and a deliberate widening.** [§33.1](../Bezel_Language_Spec.md#sec-the-main-method) says "any class in the program's main module" (a *one-module* search) while [§33.5](../Bezel_Language_Spec.md#sec-program-entry-point-resolution)'s `--main-class FullyQualifiedName` machinery suggests a program-wide one; the spec never reconciles them. Rule 3 draws the line at the root compilation, and the probe shows the cost of anything wider: a dependency artifact containing a qualifying `Main` currently emits a native `main` symbol, and the consumer's build dies with `multiple definition of 'main'` (#1511, filed while drafting). Under rules 1+3 the dependency's `Main` is an ordinary method — `-c` must stop lowering it to the native entry symbol — and dependency `Main`s can never conflict with the root's. `--main-class` disambiguates *within the root compilation*; it cannot reach into a dependency. Be clear about the direction §33.1's edit moves, though: when the root compilation is an **SCC of several modules** (this proposal's §5.2), "root compilation" is *wider* than §33.1's single main module — a qualifying `Main` in any SCC member becomes a candidate (and a [§33.5] conflict if several exist), where §33.1's letter searched one module only. That widening is the coherent choice (§33.1 never says *which* module is "main" when several compile together — the term is defined only circularly, by containing the entry point), but it is a semantic change, not a clarification; it is surfaced as open question 5 for explicit ratification.

**Compiler alignment:** rule 1 matches (explicit modes, `Main` required in executable mode, `-c` exempt — probed); rule 3 is the #1511 correction; rule 4 is a correction to the CLI surface (`bezelc --help` documents `--import <artifact>` as "object/exe" — an imported executable carries a native `main` even after the #1511 fix, which covers only the `-c` path); rule 5's implementation-defined link mode matches (`--help`: the `--import` artifact's "object is also linked into the executable" — the static-embed choice), and the one-copy guarantee is what that choice satisfies for per-module code — but **not** for generic instantiations, where it is violated today (#1513, the [§2.8.3] rule 4 correction); rule 6 restates §22.12/§22.13 with the artifact dimension made explicit. One tooling note, recorded not spec'd: the reference CLI today accepts a single source file per invocation, so multi-file modules and SCC builds are not yet drivable at all — that is #1398's build-driver lane, the implementation side of this same hole — and `--main-class` itself is likewise spec-only (`--help` shows no such flag; multi-`Main` disambiguation is exactly as undrivable today as SCC builds).

## 5. Worked example: the diamond, and the SCC interaction

### 5.1 The diamond

`App` imports `Lib1` and `Lib2`; both import `Base`; `App` never imports `Base`.

```bezel
// base.bz
module Base;

public class Money
  private _cents: long;

  public constructor(cents: long)
    _cents := cents;
  end constructor;

  public readonly property Cents: long => _cents;

  public method Format(): string
    return $"{_cents} cents";
  end Format;
end Money;
```

```bezel
// lib1.bz
module Lib1;
import Base;

public class Payroll
  public static method Net(hours: int, centsPerHour: int): owned Money
    return new Money(hours * centsPerHour);   // fresh owned Base.Money, crossing Lib1's surface
  end Net;
end Payroll;
```

```bezel
// lib2.bz
module Lib2;
import Base;

public class Invoices
  public static method Total(m: Money): long   // borrows a Base.Money
    return m.Cents;
  end Total;
end Invoices;
```

```bezel
// app.bz
module App;
import Lib1;
import Lib2;
// no `import Base;` — and none is needed for anything below

class Program
  public static method Main()
    using var pay := Payroll.Net(40, 2500);      // USE: inferred binding of a transitive type — legal (§2.8.3 rule 2)
    Console.WriteLine(Invoices.Total(pay));      // USE: passing the value across module surfaces — legal
    Console.WriteLine(pay.Format());             // USE: member access — legal (the #1487 corner)
  end Main;                                      // USE: `using var` disposal of a transitive type — legal
end Program;

// Each of the following would be NAMING, and each requires `import Base;` first:
//   var m: Money := Payroll.Net(1, 1);           — type ascription
//   using var m2 := new Money(500);              — `new`
//   if pay is Money then … end;                  — type test
//   var box := new OwningArray<Money>();         — generic type argument
```

*(One honesty note on the disposal line: `Money` is trivial-dispose — no destructor, no owned fields ([§28.9](../Bezel_Language_Spec.md#sec-runtime-managed-types)) — so the inserted disposal is a no-op here. The example exercises the naming/use split; the legality of the `using var` binding is the point, not disposal machinery. A `Base` type with an owned field behaves identically under rule 2 — disposal of the transitive value is still use, not naming.)*

Build, with the reference CLI:

```
bezelc -c base.bz -o base.o
bezelc -c lib1.bz --import base.o -o lib1.o
bezelc -c lib2.bz --import base.o -o lib2.o
bezelc app.bz --import lib1.o --import lib2.o --import base.o -o app     # base.o: the CLOSURE, §2.8.3 rule 1
```

What each ruling does here:

- **Closure (§2.8.3 rule 1):** the last line must supply `base.o` even though `app.bz` never writes `import Base;`. Omitting it is a build error naming `Base` and the artifacts that reference it (`lib1.o`, `lib2.o`). *(Today: type-check passes without it, native link fails with raw symbol errors — conforming in substance, sub-standard diagnosis.)*
- **Identity (§2.8.3 rule 3):** `Money` arrives in `App`'s compilation three times — embedded in `lib1.o`'s interface, embedded in `lib2.o`'s, and directly from `base.o`. All three are `(Base, Money, arity 0)`: **one type**. `pay` flows from `Lib1`'s surface into `Lib2`'s parameter with no conversion, and `pay.Format()` binds to `base.o`'s actual code. *(Today: this exact member call returns garbage — #1487.)* If `lib1.o` had been compiled against an older, different `Base.Money`, the descriptions disagree and the build fails naming `lib1.o` as stale — instead of printing `0`.
- **Duplicate detection (§2.8.1 rule 3):** supplying both an old and a new `base.o` is two artifacts declaring `Base` — error naming both.

### 5.2 The SCC interaction

Now let `Lib1` and `Lib2` import each other (a genuine cycle, so per [§2.5](../Bezel_Language_Spec.md#sec-dependency-resolution) they form an SCC and compile as one unit):

```bezel
// lib1.bz
module Lib1;
import Base;
import Lib2;                                     // cycle edge

public class Payroll
  public const Version: int := 2;

  public static method Net(hours: int, centsPerHour: int): owned Money
    var gross := hours * centsPerHour;
    return new Money(gross - Invoices.Fee(gross));
  end Net;
end Payroll;
```

```bezel
// lib2.bz
module Lib2;
import Base;
import Lib1;                                     // cycle edge back

public class Invoices
  public static method Fee(amount: long): long
    return amount / 10;
  end Fee;

  public static method Describe(): string
    return $"fee model v{Payroll.Version}";      // legal: Lib1's PUBLIC surface
  end Describe;

  internal static method Audit(): string
    return "audit";
  end Audit;
end Invoices;
```

The build presents *both* sources to one invocation (partial-SCC builds are already an error naming the missing members, §2.5), which emits `lib1.o` *and* `lib2.o`. The rulings' interaction, each point one sentence:

- **Identity (§2.8.1 rule 1):** `Lib1` and `Lib2` are each still produced by exactly one compilation — the same one. One compilation, two modules, two artifacts; nothing about uniqueness changes.
- **Closure is not widened (§2.8.1 rule 2):** co-compilation grants no access — a reference to `Invoices.Audit()` from `lib1.bz` is a visibility error *inside the SCC*, exactly as it would be between separately-compiled modules. The SCC is a compilation-order device, not an access grant.
- **Self-import stays an error (§2.8.1 rule 4):** the `Lib1 → Lib2 → Lib1` cycle is legal; `import Lib1;` *inside* `lib1.bz` is not.
- **Assembly (§2.8.3/§2.8.4):** `App` may import only `Lib1`, but its closure must contain `lib2.o` (referenced by `lib1.o`'s interface) and `base.o` — and §2.5's existing rule already requires SCC artifacts to have been produced together.

*(Status note: none of §5.2 is drivable with today's single-source-file CLI — the SCC build driver is #1398. The example is spec-level, matching v0.164's already-landed §2.5 semantics.)*

## 6. The small amendments (exact edits)

1. **[§2.1](../Bezel_Language_Spec.md#sec-module-declarations), membership sentence** — the other honor-system sentence, given the same qualifier as §13.4's: *"Multiple files with identical `module` declarations belong to the same module. Files in the same module share `internal` access ([§13])."* → *"Multiple files with identical `module` declarations **presented to the module's one compilation ([§2.8.1])** belong to the same module. Files in the same module share `internal` access ([§13])."* (Without this, §2.1's purely-nominal membership would stand in direct contradiction to amended §13.4.)
2. **[§2.4](../Bezel_Language_Spec.md#sec-compilation-unit), opening two sentences** — §2.4 opens *"The module is the unit of compilation. The compiler takes all source files belonging to a module, plus its dependencies, and produces a single native artifact"*. Both sentences are rewritten together (rewriting only the second would leave the first contradicting the SCC parenthetical beside it): *"The compilation unit — a module, or an import-cycle SCC of modules ([§2.5]) — is the unit of compilation. The compiler takes all source files belonging to the compilation unit and, reading its dependencies' artifacts ([§2.5], [§2.8.3]), produces one native artifact per member module."* Dependencies are *read as artifacts*, not compiled in; the current wording licenses the misreading that a build re-compiles its dependency sources.
3. **[§2.4](../Bezel_Language_Spec.md#sec-compilation-unit), executable bullet** — qualify "standalone": *"An **executable** is a native binary requiring no language-runtime installation; whether its dependency artifacts are embedded or accompany it is the [§2.8.4] link-mode choice. It requires exactly one `Main` method as defined in [§33], selected per [§2.8.4]."*
4. **[§2.4](../Bezel_Language_Spec.md#sec-compilation-unit), library bullet** — the same link-mode qualification: *"A **library** is a shared native library (`.so`, `.dylib`, or `.dll`)."* → *"A **library** is a non-executable artifact; whether it takes the form of a shared native library (`.so`, `.dylib`, `.dll`), an object embedded statically by its consumers, or both, is the [§2.8.4] link-mode choice."* (The reference behavior — `-c` emits a `.o` statically embedded by consumers, §8 rows 9/12 — would otherwise contradict the bullet's letter.)
5. **[§2.5](../Bezel_Language_Spec.md#sec-dependency-resolution)** — replace the parenthetical metadata list with a cross-reference: *"…by reading the imported module's compiled artifact, which embeds the module's interface ([§2.8.2])."*
6. **[§2.6](../Bezel_Language_Spec.md#sec-build-phases), phase 1** — *"Parse all source files of the module"* → *"Parse all source files of the compilation unit (module or SCC, [§2.5])."* (v0.164 defined SCC units but left §2.6 speaking single-module.)
7. **[§13.4](../Bezel_Language_Spec.md#sec-modules-and-internal), first sentence** — *"A module is the set of all source files declaring the same `module Name.Path;`"* → *"A module is the set of all source files declaring the same `module Name.Path;` **presented to the module's one compilation ([§2.8.1])**; `internal` access is closed by that compilation — no later source file joins a module by declaring its name."*
8. **[§33.1](../Bezel_Language_Spec.md#sec-the-main-method)** — *"…declared on any class in the program's main module"* → *"…declared on any class in the root compilation ([§2.8.4]); the **main module** is the module containing the selected entry point."* (For a several-module root SCC this is a deliberate widening of the search — open question 5.)
9. **[§33.5](../Bezel_Language_Spec.md#sec-program-entry-point-resolution)** — add one sentence: *"The search space is the root compilation only; a qualifying `Main` in a dependency artifact is inert and never conflicts ([§2.8.4])."*
10. **[§33.6](../Bezel_Language_Spec.md#sec-example), last sentence** — *"A library module has no entry point; the compiler produces a shared library artifact."* → *"A library build has no entry point ([§2.8.4]); a qualifying `Main` in a library compilation is an ordinary method."*

## 7. Explicit non-goals (§2.8.5)

Stated in the spec text so their absence reads as decided, not forgotten. One line each. None has a recorded decline in the *rationale* today (grep-confirmed by the dive), so this section *creates* those records — with one prior-art caveat: conditional compilation is already declined at the proposals-corpus level ([stdlib-gap-review-2026-07 declined item 6](papers/stdlib-gap-review-2026-07.md): "Compile-time platform branching (cfg/#ifdef) — a language change, not a stdlib addition … A conditional-compilation proposal, if ever, goes through the spec workflow"), and the bullet below is exactly the spec-side record that decline points at, not a clean-slate decision:

- **Package management, versioning, registries.** §2 identifies modules within one build; *which* artifact provides a module is the build tool's input ([§2.6](../Bezel_Language_Spec.md#sec-build-phases): implementation-defined), and version arbitration is a package-manager design for a language that does not yet have users — premature by the project's evidence-first discipline.
- **ABI stability across recompilation.** The model is recompile-the-world: an artifact's interface is a compile-time contract for *this* build ([§2.8.3] rule 3 makes staleness an error, not a compatibility negotiation); a stable binary ABI is a massive, separate commitment nothing yet motivates.
- **Conditional compilation (`#if`, feature flags).** One source text, one meaning — a file that means different things under different flag sets defeats spec-derived testing and the "trace before writing" discipline; platform variance belongs in the build tool selecting *which files* compile, not in files that change meaning. (Concurs with [stdlib-gap-review-2026-07](papers/stdlib-gap-review-2026-07.md) declined item 6, which routed the question here; runtime platform queries — the `OperatingSystem` statics *proposed* in [0013](0013-environment.md), not current stdlib — cover the stdlib-scoped need.)
- **Selective imports (`import M.{X}`).** `import` is file-scoped and the §2.2 collision rule already keeps bare-name pollution diagnosed-and-fixable (qualify or alias); a selective form adds a second place where a file's name set is edited, for no capability qualification doesn't already give.
- **Re-exports (`public import`).** A module's public surface is what it declares; forwarding another module's names would break [§2.8.3]'s clean naming rule ("naming requires importing the *declaring* module") and reintroduce facade-drift for zero expressiveness — consumers can always import the declaring module.
- **Wildcard imports.** Nothing to add: `import M;` already makes all of `M`'s public names visible — there is no narrower form for a wildcard to generalize.

Revisiting any of these requires new evidence (real programs hitting real walls), per the [DECLINED.md](DECLINED.md) discipline.

## 8. Compiler alignment summary

| # | Current behavior (probe, HEAD `05e992f`) | Ruling | Verdict |
|---|---|---|---|
| 1 | Two artifacts declaring `module Dup` link and run silently | §2.8.1(3) | **corrected** — new build error; prereqs #1488 (module-qualified mangling) and #1514 (strong runtime-support symbols) |
| 2 | Self-import silently accepted; against a same-named artifact, invents publics-only foreign-module semantics | §2.8.1(4) | **corrected** — compile error |
| 3 | User file declaring `module System;` compiles and runs (`rev0034/sysgrab`); `import System.Threading;` resolves with no artifact | §2.8.1(5) | **split** — the stdlib presentation exemption **matches**; the implementation-name grab is **corrected** (build error) |
| 4 | Metadata-based type-check of imports; generic bodies shipped as source text and re-monomorphized | §2.8.2 | **matches** — the mechanism becomes a conforming format under a stated requirement; #710 = acceptable diagnosed gate, #1222 = nonconforming silent divergence |
| 5 | Transitive type: member access/pass/dispose without import works; *naming* requires import | §2.8.3(2) | **matches** verbatim |
| 6 | Member call on a transitively-obtained value returns garbage, exit 0 | §2.8.3(3) | **corrected** — #1487; identity unification made normative |
| 7 | Missing closure member: type-check passes, native link fails raw (incl. body-only dependencies — `crit0034/mid`) | §2.8.3(1) | **matches in substance**; metadata-time diagnostic named as intended standard |
| 8 | Generic static state through two consumer artifacts: `0`/`0`, exit 0 (same-module baseline `1`/`2`) | §2.8.3(4), §2.8.4(5) | **corrected** — #1513 (filed during review); one static state + one runtime identity per instantiation per process |
| 9 | Executable mode requires `Main` (error cites §33.1/§33.2); `-c` exempt | §2.8.4(1) | **matches** |
| 10 | Dependency artifact containing `Main` → `multiple definition of 'main'` at consumer link | §2.8.4(3) | **corrected** — #1511 (filed while drafting): `-c` must not lower `Main` to the native entry symbol. Caveat: that fix covers the `-c` path only — an *executable* presented via `--import` (the CLI admits "object/exe") still carries native `main`, which §2.8.4(4) turns into a kind error |
| 11 | `--import` accepts executables as dependencies (`--help`: "object/exe") | §2.8.4(4) | **corrected** — dependency artifacts must be library-kind |
| 12 | `--import` objects statically linked into the executable | §2.8.4(5) | **matches** — one admitted link mode under the one-copy guarantee (which #1513's fix must complete for instantiations) |

## 9. Open questions for the owner

1. **Ratify the four rulings as a package** (§1–§4), landing as spec §2.8 + the §6 edits in one docs revision? They are designed to be severable, but rulings 1 and 3 share the identity machinery and land best together. Two additions made at review deserve a named nod inside the package: the **implementation-provided-modules carve-out** (§2.8.1 rule 5 — without it every real build is ill-formed, and `module System;` stays an open internals-grab) and the **runtime half of type identity** (§2.8.3 rule 4 — without it the rulings imply but never state the one-static-state guarantee #1513 shows broken). Recommended: ratify both with the package.
2. **Duplicate-module detection point (§2.8.1(3))**: recommended — at artifact-set load (metadata time), naming both artifacts; the alternative (let the native linker find it) produces mangled-symbol errors that don't name modules. Confirm the recommended point as the normative diagnostic (with link-time discovery conforming-but-substandard, as for closure omissions).
3. **The naming/use split (§2.8.3(2))**: confirm the line as drawn — in particular that *inferred* bindings (`var`/`using var`) and compiler-inferred instantiations at transitively-reachable types are legal without import while every source-level spelling of the type name (now explicitly including `catch` clauses) is not. This matches the compiler today; the alternative (require import upon any binding) breaks disposal of returned values and is not recommended.
4. **Artifact-kind determination (§2.8.4(1))**: this proposal deviates from the dive's sketch (Main-presence inference) in favor of build-request-driven kinds, matching the compiler and avoiding silent kind flips. Confirm the deviation.
5. **Entry-point search scope (§2.8.4(3), §6 edit 8)**: today's [§33.1](../Bezel_Language_Spec.md#sec-the-main-method) letter searches *one module* ("any class in the program's main module"); the ruling searches the whole **root compilation**, which for a several-module root SCC is strictly wider — a qualifying `Main` in any SCC member becomes a candidate and a potential [§33.5] conflict. This is a semantic change, not a clarification, and it is the recommended one (§33.1 never says which co-compiled module is "main"; the narrow alternative would need an arbitrary tie-break the spec has no vocabulary for). Ratify the widening explicitly.
6. **"No restrictions beyond visibility" (§2.8.2)**: confirm making it normative with the diagnosed-gate interim posture — implementations may reject named unexportable shapes with a diagnostic while the gate shrinks (#710), but silent semantic divergence (#1222) is nonconforming. The alternative — spec'ing the current gate as a language restriction — freezes an implementation accident into the language; not recommended.
7. **Static-init dependency metadata (§2.8.4(6))**: cross-unit cycle rejection needs per-type init-dependency summaries in the artifact interface, but the dependency *relation* itself is unpinned (#804: through-method-body and cross-module cycles are missed today). Recommended: keep §2.8.4(6)'s deferral, pin the relation in the #804 lane, and add the metadata clause to §2.8.2 then. Confirm the deferral rather than blocking §2.8 on it.
8. **`protected` in the interface (§2.8.2(1))**: the sketch includes `protected` members so cross-module derivation type-checks ([§13.1](../Bezel_Language_Spec.md#sec-levels) explicitly contemplates a derived class in a different module). Confirm; the narrow alternative (public-only interfaces, no cross-module subclassing of non-public surface) would be a real language restriction the spec nowhere states.

## 10. Relationship to existing work

- **Spec [§2.1–§2.7](../Bezel_Language_Spec.md#sec-modules-and-compilation) (v0.164)** — the settled front half; this proposal only appends (§2.8) and touches the §6-listed sentences. The §2.2 alias-collision omission ([#1515](https://github.com/s8lap/bezel-lang/issues/1515), spec-defect: an alias colliding with an imported bare name is legal yet dead under §2.7's order) is a **separate one-sentence spec fix** — deliberately not folded in, so this chapter-sized proposal and that surgical fix can land independently.
- **[#1487](https://github.com/s8lap/bezel-lang/issues/1487) / [#1488](https://github.com/s8lap/bezel-lang/issues/1488) / [#1489](https://github.com/s8lap/bezel-lang/issues/1489)** — the dive's filed evidence bugs: transitive-type garbage (ruling 3), module-less mangling (rulings 1/3 prerequisite), and the alias-vs-imported-name resolution backtracking — a *compiler* bug against §2.7's settled order, adjacent lane, no spec change (its spec-side residue is #1515 above).
- **[#1511](https://github.com/s8lap/bezel-lang/issues/1511)** — filed while drafting: library artifacts must not emit the native `main` symbol (ruling 4).
- **[#1513](https://github.com/s8lap/bezel-lang/issues/1513) / [#1514](https://github.com/s8lap/bezel-lang/issues/1514)** — filed during review: cross-artifact generic static state is garbage (the §2.8.3 rule 4 / §2.8.4 rule 5 evidence bug), and runtime-support symbols (`bz$Console$Dispose`) are emitted strong in every artifact — with #1488, the multi-artifact-practice blockers.
- **#428 family (#710, #711, #1222, #1316)** — the cross-module generics saga; ruling 2 converts its lessons from folklore into a stated interface requirement with a conforming-gate/nonconforming-divergence line.
- **[#1398](https://github.com/s8lap/bezel-lang/issues/1398)** — the SCC build-driver lane; §5.2 is its spec-level acceptance test. **#1365** — the original module-system review issue; v0.164 closed its front half, this proposal is the recorded residue.
- **[#804](https://github.com/s8lap/bezel-lang/issues/804)** — static-init cycle detection gaps; open question 7 routes the metadata clause through it.
- **Rationale [§2.1](../Bezel_Design_Rationale.md#sec-dotted-module-names-are-one-name)** (dotted names are one name; no implicit visibility) and the [v0.164 changelog](../Bezel_Design_Rationale.md#sec-v0-163-to-v0-164-changelog) — the recorded decisions rulings 1–3 extend without disturbing.
- **Spec [§22.12](../Bezel_Language_Spec.md#sec-static-members)/[§22.13](../Bezel_Language_Spec.md#sec-static-initialization-order)** — the statics machinery ruling 4 grounds its one-copy guarantee in; **[§32.11](../Bezel_Language_Spec.md#sec-reserved-for-future-revisions)** — the reservation that keeps runtime dynamic loading out of scope for §2.8.1's per-process uniqueness.
- **Probes** — `/var/tmp/bezel-scratch/specrun/modsys/` (`dup*`, `selfimp`, `trans*`, `nomain`, `depmain`/`rootmain`, `p1_import_before_module`), plus the review-pass probes `/var/tmp/bezel-scratch/specrun/crit0034/` (`gbase`/`glib1`/`glib2`/`gapp` = #1513; `mid`/`dep`/`cons` = the carried-body closure case) and `/var/tmp/bezel-scratch/specrun/rev0034/` (`sysgrab` = the `module System;` grab): the measured evidence base; the load-bearing ones re-run at HEAD `05e992f`.
