# Exploration — wrapping a native toolkit: candidates, the Lazarus workflow, and the seamlessness budget

| | |
|---|---|
| **Status** | Exploration — not a proposal; a toolkit-candidate map plus a workflow and work-breakdown study for the two leading candidates. Nothing is scheduled, and adopting any dependency named here is a separate project-level decision (CLAUDE.md: "dependencies are debt"). |
| **Origin** | Owner question series 2026-08-19: can the LCL itself be packaged for Bezel; which library should a first wrap target; a stated preference for a **WYSIWYG designer** over a React-style code-only description; and whether the Lazarus integration can be made seamless ("quality of life is as important as everything else"). |
| **Written against** | spec v0.262, stdlib v0.262. External-ecosystem facts (Lazarus widgetset status, liblcl, designer tooling, licenses) are as of 2026-08-19 and marked ◆ — re-verify at adoption time. |
| **Lineage** | exploration-ui.md (the paradigm map; the FFI callback machinery named "the load-bearing door"); exploration-ui-vcl-retained.md (the retained substrate is sound; its §6 item 1 designer/reflection gap is what §2's insight closes); 0004 (MVU remains the app-surface lane, proposal-only by owner decision). |
| **Verdict up front** | Wrapping a foreign toolkit is achievable through exactly one architecture — a **flat-C surface** — and the WYSIWYG requirement reshapes the candidate ranking: what matters is a designer whose artifact is a *file* that can be loaded by a native-side loader or parsed by a build-time generator, so the reflection lives outside Bezel. Two lanes shortlist: **GTK4 direct** (zero shim, the cleanest §32 exercise) and **LCL via liblcl** (the Lazarus IDE becomes Bezel's form designer — the best pure-WYSIWYG workflow in open source, proven by govcl◆). The two lanes are fully disjoint: the LCL has **no GTK4 widgetset**◆. Near-seamless Lazarus quality-of-life is buyable with small bounded tools — a watcher, insert-only handler-stub sync, and a dual debug/release form path — with one seam (designer→code navigation) closable only by an optional Lazarus IDE package. None of this displaces 0004: this is the *tools lane* over the same retained substrate. |

## 1. Packaging a foreign toolkit: the one architecture that works

Direct consumption of foreign object code is out for any non-C toolkit: Object Pascal (and C++) class layout, name mangling, exception models, and runtime initialization have no stable cross-compiler ABI. Every language that consumes such toolkits goes through a **flat C surface** — either the toolkit's own (GTK) or a shim compiled into an ordinary shared library (Free Pascal happily compiles the LCL into one exporting plain `cdecl` functions; the same technique serves Qt).

The Bezel side needs no new machinery — §32 already provides each required mechanism:

| Binding requirement | §32 mechanism |
|---|---|
| Bind flat functions | `extern "<lib>"` blocks, default C convention, full-path library spec (§32.1/§32.3/§32.4) |
| Opaque widget handles; UTF-8 `PChar` | `IntPtr` (§32.7); `CString` (§32.6); C-compatible records for rects etc. (§32.13) |
| Event handlers — callbacks **retained** by the toolkit's event loop | the §32.8 **registry idiom**, verbatim: one static process-lifetime callback, an integer token as the `void*` user-data, a registry mapping token → wrapper, wrapper teardown deregistering with native first. (The `env` clause is inapplicable — it is call-bounded by contract, §32.10.) |
| The toolkit's blocking main loop | a long-blocking extern on a **dedicated spawned domain** — §32.10's recorded idiom. The payoff is §32.8's execution-context rule: callbacks arriving on the thread executing that extern call run **synchronously inside that domain's continuation** — handlers are ordinary UI-domain code, no locks; modal re-entry is covered ("reentrancy needs no new rule"). |
| Handler error policy | the dispatcher must fence exceptions: an exception escaping a callback **aborts the program** (§32.8) |
| *Not* needed | export-to-native and call-through-pointer (both deferred, §32.14) — signal/event binding is explicit connect with user-data, never symbol autoconnect |

On top of the raw binding, the wrapper layer instantiates exploration-ui-vcl-retained.md's semantics unchanged: wrappers raise stdlib §6.5 `EventSource`s, forms subscribe with the `Subscription`-in-owned-field admission (§28.14 form 3), teardown is the §28.4 cascade, and the tree is UI-domain-confined by construction (§30.14). The **single-owner rule** is the one design decision to record when this becomes real: the native side and the Bezel wrappers must not both free. For designer-loaded forms the natural answer falls out — the toolkit already parents/owns loaded controls under their form, so only the *form* wrapper is `owned` in Bezel and child accessors are plain handles; dynamically created controls choose explicitly at the create call.

## 2. The WYSIWYG requirement reshapes the candidate ranking

exploration-ui-vcl-retained.md §6 item 1 ruled out Delphi-style form streaming *interpreted by Bezel* — that needs reflection, which the language declined (rationale §5.18/§18). It did **not** rule out designers whose artifact is a file with the interpreting machinery elsewhere:

- **native-side loading** — the toolkit's own loader replays the file at runtime (its reflection, in its language); Bezel asks for widgets by name and connects events explicitly; or
- **build-time generation** — a Bezel tool parses the same file and emits construction code plus typed accessors.

Either way, WYSIWYG arrives without touching the no-reflection stance; §4 combines them. One structural observation: a designer can round-trip a widget tree or a UI *file*; it cannot round-trip a `view: Model → Node` *function* — which is why no real WYSIWYG editor exists for React- or Elm-style frameworks. A WYSIWYG-first product goal therefore selects the retained lane, which the previous exploration showed the language supports soundly.

The candidates under that lens (◆ throughout):

| Toolkit | FFI cost | WYSIWYG story | Concerns |
|---|---|---|---|
| **GTK4** | **zero shim** — pure C API, the only major toolkit bindable with nothing in between | Cambalache edits `.ui` XML; `GtkBuilder` loads it at runtime | non-native look off-Linux; Cambalache is good, not Lazarus-good |
| **LCL via liblcl** | flat-C shim, prebuilt, proven by govcl (Go), plus Python/C#/Rust consumers | **the Lazarus IDE itself** — two-way designer, object inspector, anchor editor; govcl proves the load-forms-at-runtime workflow | third-party shim as a load-bearing dependency (bus factor, fixed surface); license diligence; ship widgetset choice (below) |
| Qt | C++ — needs a generated C shim; `libqt6pas` (maintained *by the Lazarus project* for LCL-Qt) and miqt (Go) prove the approach | Qt Designer is best-in-class; `QUiLoader` loads `.ui` at runtime | largest wrapping burden; LGPLv3 discipline |
| wxWidgets | C++ shim; the old wxC bindings are bit-rotted — we would own a generator | wxFormBuilder + runtime XRC | highest total cost for the benefit |
| FLTK (cfltk) | small maintained C shim | FLUID emits *code*, not a loadable file — needs a Bezel emitter | non-native look; niche |

**Widgetset status, verified against the Lazarus repository's `main` branch (2026-08-19)◆:** `lcl/interfaces/` contains `gtk`, `gtk2`, `gtk3`, `qt`, `qt5`, `qt6`, `win32`, `cocoa`, `carbon`, `customdrawn`, `fpgui`, and exotics — **no `gtk4` directory exists**; GTK4 is discussion-stage, contingent on GTK3 (newly the Linux default after ~15 years of GTK2) stabilizing first. The mature modern LCL backends on Linux are Qt5/Qt6, reached through the `libqt5pas`/`libqt6pas` flat-C shims. Two consequences: the LCL lane and the GTK4 lane are fully disjoint (wrapping the LCL never yields a GTK4-native app); and a liblcl adoption should **build the shim from source against Qt6 or GTK3** rather than shipping the stock GTK2 build (GTK2 is upstream-EOL◆) — one extra bounded work item, invisible to the binding itself.

**Shortlist verdict:** GTK4 direct if the goal is a modern-native Linux lane and a zero-dependency driver for FFI maturation; LCL/liblcl if the goal is the Delphi-grade designer workflow and native-Windows look. Qt is the best library and the costliest reach — though `libqt6pas` existing, maintained, and battle-tested by every LCL-Qt app makes it a stronger foundation than expected if that lane ever opens. wx and FLTK are dominated.

## 3. The Lazarus workflow, concretely (the LCL lane)

The govcl-proven shape◆, adapted: **Lazarus stays completely unmodified** — it is a form editor whose save format is a small text file.

1. **Design.** A "design harness" Lazarus project lives in the repo; its Pascal code is throwaway scaffolding, never shipped, and Lazarus maintains it automatically. Drag controls, set properties and anchors, name things (`OkButton`, `UserEdit`), assign handler *names* (`OnClick = OkButtonClick` — Lazarus writes the assignment into the `.lfm` and an empty Pascal stub into the scaffold). Saving writes `LoginView.lfm`, a plain-text `object … end` property tree.
2. **Generate.** A build rule runs `lfm2bzl LoginView.lfm`, emitting a generated base class: typed accessors for every named control, construction/layout code (or a runtime-load binding — §4's dual path), and event wiring expecting named methods.
3. **Code.** The user writes the derived class — the Delphi experience with the contract made compile-checked:

```bezel
public class LoginForm: LoginViewGen          // LoginViewGen is generated — never edited
  public override method OkButtonClick(e: ClickEvent)
    StatusLabel.Caption := $"Signing in {UserEdit.Text}…";
  end OkButtonClick;
end LoginForm;
```

(Toolkit vocabulary in the sketch is sketch-only, under exploration-ui-vcl-retained.md's API-invention notice; the real types underneath — `EventSource`, `IEvent`, `Subscription`, the collections — are stdlib. How the generated base declares handler slots — `abstract` methods vs. event subscriptions — is `lfm2bzl` design detail, deliberately unpinned here.)

4. **Iterate.** Re-open the `.lfm`, rename or add controls, rebuild: drift between design and code surfaces as **compile errors at the exact use sites**, because the generated base class is the contract — better than classic Delphi's runtime `EAbstractError` on a missed rename.

## 4. The seamlessness budget

Every seam in this integration lives in plain files we generate, parse, or watch — none lives inside Lazarus. Seamlessness is therefore purchasable incrementally:

**Closes cheaply (days-scale each):**

- **Save→update loop:** a `bzl-lcl watch` daemon — watch `.lfm`, rerun `lfm2bzl`, trigger the incremental build; Bezel's seconds-scale relink is what makes it feel instant.
- **Handler stubs (the single highest-QoL item):** when the `.lfm` gains a handler name the derived class lacks, the watcher **insert-only** appends an empty stub below a marker comment in the user's file — never deletes, never rewrites. Double-click in Lazarus, save, and the stub is waiting in your editor. (Qt Creator's "go to slot" is the precedent.)
- **F9 parity:** the harness project's Run configuration invokes the Bezel build-and-run — shipped in the project template, zero code.
- **Scaffolding:** `bzl-lcl new-form Login` creates the `.lfm`/`.pas` pair, registers it in the harness project, stubs the derived Bezel class.

**Closes with medium effort — and forces the doc's one architectural decision:**

- **Layout hot reload → the dual path.** Construction-codegen alone requires a recompile per layout tweak. Therefore: **debug builds load the `.lfm` at runtime** through liblcl's native-side streaming — edit, save, and the running app re-streams the form in place, handlers rewired by name, sub-second, no compile; **release builds use generated construction code** — fully compile-checked, no form blobs shipped. Both paths share the same generated typed accessors. The bonus is diagnostic: the streaming path applies *every* property via Pascal RTTI, so any property the code emitter does not yet cover shows up as a debug-vs-release rendering diff — the fidelity long tail (the real grind of this project) becomes a visible, golden-testable **ratchet** instead of silent seam erosion.

**Closes only with compile errors — deliberately:**

- **Rename propagation.** Silent rename-through-user-code needs a refactoring engine or risky text rewriting; both declined. Regeneration turns a rename into precise compile errors at each use site (the watcher can annotate: "`Button1` → `OkButton`: 3 references in LoginForm.bzl"). Loud-where-meaningful is the house ethos; this is the one place the integration is intentionally less magical than Delphi.

**The one seam that never fully closes without touching Lazarus:**

- **Designer→code navigation.** Double-click opens the Pascal stub; Lazarus's editor speaks Pascal, not Bezel. The free 90%: stub-sync already put the handler in the Bezel file, and the user's real editor auto-reloads it. The 100%: a small **Lazarus IDE package** (Lazarus is extensible in Pascal) hooking the jump and shelling out to the user's editor — the *only* Pascal-side code anywhere in this plan, optional, v2, itself bounded.

## 5. Bounded work items, in dependency order

1. **FFI spike** (small; the gating fact). Hand-write one extern block for ~10 liblcl functions — init, create form/button, set caption, register the global event callback, run the loop, deliver one click through the registry idiom. Its real product is knowledge: how much of §32 the compiler implements today. Produces the runtime-layer skeleton as a byproduct.
2. **`lfm2bzl`** (small-to-medium; fully decoupled — needs no FFI, no liblcl, not even a working backend). The `.lfm` grammar is tiny; pure parse+emit, tested CLAUDE.md-style with golden files over a corpus of real Lazarus forms. This tool *is* the WYSIWYG bridge — and it hedges the toolkit bet: a form-file-to-Bezel generator is largely reusable if the backend ever became GtkBuilder or Qt `.ui`, since only the emitter's target API changes. Items 1 and 2 are independent and make ideal parallel probes.
3. **Extern-block generator** (mechanical) from liblcl's machine-readable API definitions◆ — emits the `extern "liblcl"` blocks and thin wrapper classes. The embedded *policy* is item 4's content.
4. **Ownership policy + runtime core** (small, design-heavy — the piece deserving a recorded decision). The §1 single-owner rule; the ~small hand-written core: UI-domain bootstrap, event dispatcher with its exception fence, token registry, and a channel-based `Ui.Post` for background→UI traffic.
5. **Quality of life** (the §4 inventory): watcher, stub-sync, template, scaffolder; then hot reload (the dual path); then, optionally, the Lazarus IDE navigation package.
6. **If adopting liblcl:** build it from source against Qt6 or GTK3◆; license diligence on the shim itself (the LCL is modified-LGPL with the linking exception◆).

## 6. Relationship

**Two lanes, one substrate.** This is the *tools lane*: designer-drawn forms, Delphi-proven for exactly the services-and-tools domain Bezel targets, over the retained widget tree whose soundness exploration-ui-vcl-retained.md established. 0004's MVU remains the *app lane* for state-rich UIs — the manual model↔widget synchronization this lane accepts is precisely what MVU exists to remove — and an eventual MVU backend would patch these same wrappers (0004 §5, open question 4's FFI backend). Nothing here displaces 0004, which stays proposal-only by owner decision; and nothing here is scheduled — this doc exists so that when a wrap conversation starts in earnest, it starts from the flat-C/native-side-loader/dual-path insights rather than a blank page.

## 7. Open questions (deliberately few)

1. **liblcl vs a bespoke FPC exports unit** — a maintained-elsewhere shim with bus-factor risk versus owning a small Pascal build with exactly the needed surface.
2. **The dual path's acceptance bar** — how much release-codegen property coverage is "enough" before debug/release parity is honest; the ratchet needs a stated floor.
3. **GTK4 first instead?** — driven by which product goal the owner ranks first (designer workflow and native-Windows look vs modern-native Linux and zero shim). The `lfm2bzl` investment partially hedges either way.
4. **Event taxonomy** — mapping liblcl's event-id space to typed payload records (generated from the API definitions?) so the `EventSource` layer is typed rather than `IntPtr`-shaped from day one.
