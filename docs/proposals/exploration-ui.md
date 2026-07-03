# Exploration — UI approaches for Bezel

| | |
|---|---|
| **Status** | Exploration — not a proposal; a map of the design space against the language's actual properties |
| **Origin** | Owner question 2026-07-03: which UI approaches fall out immediately, which fit; SwiftUI-style? React-meaning code? |
| **Verdict up front** | One approach fits like a glove (Elm-style MVU on a dedicated UI domain, with record-tree diffing over a retained owning backend); one is a strong utility citizen (immediate mode); SwiftUI's *look* is approximable but its *machinery* falls out; React's *meaning* carries over minus hooks; auto-tracking reactivity and reflection-based binding fall out immediately. |

## 0. The headline Bezel-native win, before any paradigm choice

Every mainstream toolkit's #1 crash class is *touching UI state from the wrong thread* (Qt, WinForms, Cocoa all fault at runtime, at best). In Bezel the UI thread is naturally a **domain**: serial-per-domain (§30.13) *is* UI-thread affinity, and anything crossing into the UI domain must be **sendable** (§30.14). "Don't update the UI from a background thread" stops being documentation and becomes a **compile error at the send site** — whatever paradigm sits on top. This is the argument that Bezel should have opinions about UI at all.

## 1. The map

| Approach | Exemplars | Fit with Bezel | Verdict |
|---|---|---|---|
| Retained OO widget tree | Qt, GTK, Cocoa, WinForms | Ownership maps beautifully: parent **owns** children (`OwningArray<Widget>`), teardown = §28.4 disposal cascade, free and correct; child→parent back-refs are borrows; events are §24 events with owned `Subscription`s (the leak-prone observer graph gets ownership discipline). But state synchronization is manual — 1995 ergonomics reproduced soundly. | **Substrate, not surface.** The right shape for FFI bindings and for the *backend* of (4). |
| Immediate mode | Dear ImGui, egui | The soundness-trivial fit: no retained graph, so no lifetime puzzles — the whole UI is a function called per frame; state lives in the app, borrowed for the duration of the call (§28.3 does the rest). egui proves the ownership-model pairing. Costs are the known ones: accessibility, IME/text-editing depth, non-native look, continuous redraw. | **Strong utility citizen** — the cheapest sound first library; ideal for tools, debug overlays, and any future canvas/game story. |
| React-style VDOM | React | The *meaning* — `render: Props → Node`, diff, patch a retained backend — carries over unusually well: description nodes are **records** (auto-equality §9.2 powers diffing; `with` powers updates; value-records are sendable). The retained side is (1)'s owning tree. What does **not** carry: **hooks** — `useState`'s call-order magic and hidden per-component storage are exactly the implicit machinery Bezel declines everywhere else. State goes explicit, which morphs React into… | **Adopt the meaning, not the hooks.** See (4). |
| Elm architecture (MVU) | Elm, Redux, iced (Rust) | **Fits like a glove.** `Model` = record; `update: (Model, Msg) → Model` = `case msg of …` with `with`-updates; `view: Model → Node` = pure function producing the record tree of (3). The Elm runtime's defining constraint — updates are serial — **is** the domain model: MVU's loop is one domain, no new machinery. Side effects = tasks whose completion posts a `Msg` back through a channel, and the compiler enforces `Msg: sendable`. Every piece is an existing language feature. | **The recommended surface** when Bezel builds one. |
| SwiftUI style | SwiftUI | The **look** (nested value-type view expressions) is ~70% approximable today: constructor nesting + collection literals + named args — `VStack([Text("hi"), Button("Go", onTap)])` — no result builders needed for a readable version. The **machinery** falls out entirely: `@State`/`@Binding` are property wrappers (attribute-adjacent magic storage — Bezel has none, deliberately), result builders are a syntax transform (no macros), and the framework's hidden per-view identity storage is implicit state of exactly the kind D-0002/§18 decline. Strip those and what remains *is* MVU with nicer clothes. | **Syntax inspiration for (4)'s view layer; semantics decline.** |
| Fine-grained reactivity / signals | SolidJS, Svelte 5, Vue | *Auto-tracked* dependency graphs need runtime proxies (reflection — none) or compiler transforms (macro-tier — none), and observer graphs are the classic ownership-cycle zone. **Explicit** signals, however, already exist in embryo: a §24 event *is* `Signal<T>` minus the current-value cell — subscribe returns an owned `Subscription`, disposal unsubscribes. | **Auto-tracking falls out immediately; explicit signals are a small library** if (4) ever wants derived-value ergonomics. |
| Reflection-based binding | WPF/XAML, Angular templates | String-path binding, runtime template engines, `INotifyPropertyChanged` conventions — all reflection-shaped. | **Falls out immediately** (rationale §18), no loss: (4) replaces binding with plain functions. |
| Web-view hybrid | Tauri, Electron-lite | Bezel process (proposals 0001/0002) + OS webview via FFI; UI in HTML/JS. Sidesteps the entire question. | **The honest near-term shipping vehicle** while native UI is unbuilt — and a driver for 0002 + the FFI callback machinery (§32.8/§32.10, already spec'd). |

## 2. The recommended composite (when the time comes)

```
┌────────────── UI domain (serial = thread affinity, compiler-enforced) ─────────────┐
│  Msg (sendable, case-of dispatch)                                                  │
│  update: (Model, Msg) → Model          // records + with-expressions              │
│  view:   Model → Node                  // pure; Node = value-record tree          │
│  diff(oldNode, newNode) → Patch*       // §9.2 auto-equality prunes subtrees      │
│  apply patches → retained backend      // owning widget tree (FFI or custom);     │
│                                        // teardown = owned-field cascade          │
└────────────────────────────────────────────────────────────────────────────────────┘
   background work = ordinary tasks/domains; results re-enter ONLY as sendable Msgs
```

Every box is an existing language feature; the only *new* engineering is the diff/patch reconciler and a backend binding. Notable detail: `view` output being value-records means snapshots are cheap to keep — time-travel debugging (Elm's party trick) falls out of the data model.

## 3. What today's decisions should preserve (keep-in-mind list)

1. **Records stay cheap and equality stays structural** (§9.2) — the diffing story depends on it. Method-local records (#1405) would help view-local node shapes.
2. **§24 events keep owned Subscriptions** — the explicit-signals option builds on exactly this.
3. **The FFI callback inversion (§32.8/§32.10) is the load-bearing door** to native backends and webviews — keep it first-class as FFI evolves.
4. **Channels stay the only cross-domain door** — the MVU loop's integrity is the sendability rule; resist any future "convenience" that lets state slip across domains unmarked.
5. No attributes-for-UI, no result builders, no hidden per-callsite storage — each has been declined on general grounds; UI does not get an exemption.

## 4. Sequencing reality

UI sits **behind** the stdlib gaps (streams #1, process/OS #5/#12/#14 for windowing/event plumbing, and the FFI maturation) and behind real programs existing at all. Nothing here is scheduled; this document exists so that when a UI conversation starts in earnest, it starts from the ownership-tree/domain-affinity insights rather than from a blank page — and so stdlib evolution doesn't accidentally foreclose the good path.

## 5. Relationship

exploration-compile-time-di.md (the same "ownership tree + cascade teardown" insight, applied to composition roots) · proposal 0002 (webview hybrid's backend; the domain-worker pattern) · proposal 0003 (`Locked<T>` for the rare cross-domain UI cache) · §24 events, §30.13/§30.14, §32.8/§32.10 · D-0002/#1405 (records as the node currency).
