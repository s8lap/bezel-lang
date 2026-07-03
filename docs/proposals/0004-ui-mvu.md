# Proposal 0004 — Bezel MVU: an Elm-style UI architecture (early draft)

| | |
|---|---|
| **Status** | Early draft — architecture + code shape only; deliberately *not* a full API surface |
| **Origin** | exploration-ui.md's "fits like a glove" finding; owner asked for construction/control examples (2026-07-03) |
| **Design influences** | Elm (the architecture), iced/Rust (MVU without a VM), **not** SwiftUI's property wrappers, **not** React hooks (exploration-ui.md records why) |
| **The one-line pitch** | The Elm runtime's defining constraint — updates are strictly serial — *is* Bezel's domain model; every piece of MVU maps to an existing language feature, and messages-as-data is exactly what sendability wants |

## 1. The four pieces, in Bezel terms

| Elm piece | Bezel spelling |
|---|---|
| `Model` | a record (immutable; updates via `with`) |
| `Msg` | records implementing a marker interface (see §3 — the honest sum-type gap) |
| `update : Msg -> Model -> Model` | a static method, `case`-of over message kinds |
| `view : Model -> Html Msg` | a pure static method producing an **owned node tree** |
| The Elm runtime | **one domain**: the loop below, serial by construction (§30.13) |

```
        ┌─────────────────── UI domain ───────────────────┐
events ─►  msg ─► update(model, msg) ─► model' ─► view(model')
        │                                            │      │
        │             previous tree ◄── reconcile ◄──┘      │
        └───────────────────────────────────────────────────┘
     background tasks re-enter ONLY as sendable Msg values (§30.14)
```

## 2. The counter — construction and control, end to end

```bezel
module CounterApp;

// ---- Model ------------------------------------------------------------
record CounterModel(Count: int);

// ---- Messages: plain data, one record per kind (see §3) ----------------
interface Msg end Msg;
record Increment(): Msg;
record Decrement(): Msg;
record SetTo(Value: int): Msg;

// ---- Update: pure, total over the kinds it knows ------------------------
class Counter
  public static method Update(model: CounterModel, msg: Msg): CounterModel
    return case msg of
      Increment      => model with Count := model.Count + 1,
      Decrement      => model with Count := model.Count - 1,
      SetTo(var v)   => model with Count := v,
      _              => model            // open interface ⇒ a `_` arm (see §3)
    end;
  end Update;

  // ---- View: pure Model -> owned node tree ------------------------------
  public static method View(model: CounterModel): owned Node
    return Ui.Column([
      Ui.Text($"Count: {model.Count}"),
      Ui.Row([
        Ui.Button("−", new Decrement()),      // a button CARRIES its message
        Ui.Button("+", new Increment()),
        Ui.Button("reset", new SetTo(0))
      ])
    ]);
  end View;

  public static async method Main(): Task<int>
    return await Ui.Run(
      init   := new CounterModel(0),
      update := Counter.Update,               // §27.6 method references —
      view   := Counter.View);                //   same as the 0002 handlers
  end Main;
end Counter;
```

Things this example is deliberately showing:

- **A button carries its message as a value**, Elm-style — `Ui.Button("+", new Increment())` — *not* a callback closure. This is not just aesthetics: a `Msg` record is a **sendable value**, so the backend (which may live near an OS event thread) can carry it across domains legally; a closure capturing `model` could not cross, and the compiler would say so. Elm's design and §30.14 turn out to want the same thing.
- **`update` is the only place state changes**, and it is `case`-of + `with` — pattern matching and record updates, no framework state cells.
- **`view` returns an `owned Node`** — a fresh tree per render, consumed Pattern-B style by the reconciler (§5).
- `Ui.Run` never returns until quit; it *is* the UI domain's life. Method references wire it exactly like 0002's route handlers.

## 3. `Msg` and the honest sum-type gap

Elm's `Msg` is a closed tagged union. Bezel has no unions; the spelling above — a marker interface + one record per kind + type/destructure patterns — is the idiomatic encoding, and it works today. Two honest costs, recorded rather than hidden:

1. **No static exhaustiveness**: an interface scrutinee is an open set (§19.6), so `update` needs the `_ => model` arm, and forgetting a new message kind is a silent no-op rather than a compile error. This is the strongest argument this proposal generates for a future **closed-set construct** (`sealed interface` / union) — noted as a language keep-in-mind, *not* proposed here.
2. Message records with payloads are one line each (`record SetTo(Value: int): Msg;`) — mild ceremony, acceptable.

## 4. Control beyond clicks: async effects as messages

The Elm rule: `update` never performs effects; it *requests* them, and completions come back as messages. In Bezel the request is simply a task whose result is a `Msg` — and the loop enforces the rest:

```bezel
record FetchUsers(): Msg;
record UsersLoaded(Names: Array<string>): Msg;      // value payloads: sendable
record FetchFailed(Reason: string): Msg;

public static method Update(model: AppModel, msg: Msg, fx: Effects): AppModel
  return case msg of
    FetchUsers => do
      // Effects.Run: spawn the task on a worker domain; when it completes,
      // its Msg result is POSTED back into this loop (a channel underneath —
      // the §30.14 sendable-result rule is checked at this call).
      fx.Run(async () => do
        try
          using var resp := await Http.Get("https://api.example.com/users");
          return new UsersLoaded(await resp.Json<Array<string>>());
        catch e: HttpError => return new FetchFailed(e.Message);
        end;
      end);
      return model with Loading := true;
    end,
    UsersLoaded(var names) => model with Loading := false, Users := names,
    FetchFailed(var why)   => model with Loading := false, Error := why,
    _ => model
  end;
end Update;
```

- The effect closure runs on a **worker** domain; the only thing that crosses back is the returned `Msg` — checked sendable. There is no way to mutate `model` from the fetch; the type system closes the door Elm closes by purity convention.
- Timers/subscriptions follow the same shape (`fx.Every(TimeSpan.FromSeconds(1), () => new Tick())`) — deferred detail.
- Whether effects are requested via an `fx` handle (shown) or by `update` returning `(Model, Cmd)` Elm-style is **open question 2** — the handle avoids inventing a tuple-ish return; the Elm shape is purer. Early days.

## 5. Ownership of the node tree (the Bezel-specific part)

`view` builds a fresh **owned** tree each render. The reconciler is its single consumer:

- It diffs the fresh tree against the retained previous tree (kind + properties per node; §9.2 record equality where nodes carry record props), emits patches to the retained **backend widget tree** (an owning tree — teardown is the §28.4 cascade), then the fresh tree *becomes* the previous (the old previous is disposed).
- Every node the view allocates is therefore disposed exactly once, by the machinery the language already has — the render loop cannot leak by construction, which no mainstream VDOM can claim.
- **Open question 1** (the main representational decision): nodes as an owning **class** tree (natural children lists via `OwningArray<Node>`, diffing by per-kind compare) vs **records** (free structural equality, but class-typed/collection children make records borrow-bearing — fights the value-tree story). The sketch assumes the class tree; a hybrid (class nodes carrying record *prop bags* for equality-cheap diffing) looks promising and needs a design pass.

## 6. What v-anything this is *not*

No hooks, no property wrappers, no hidden per-component state, no auto-tracked signals, no reflection binding — each falls out on grounds recorded in exploration-ui.md. The entire framework state is: the model (yours, a record), the previous tree (the reconciler's, owned), the backend tree (owned). Time-travel debugging is `Array<AppModel>` — keep the snapshots you want; they are values.

## 7. Open questions (deliberately few, this is early)

1. **Node representation** — class tree vs record tree vs hybrid (§5).
2. **Effects shape** — `fx` handle vs Elm-style `(Model, Cmd)` return (§4).
3. **Closed unions** — does the `Msg` exhaustiveness gap (§3) justify a `sealed interface`-class language feature on its own, or does it wait for more use cases? (Language question; out of this proposal's scope to answer.)
4. **First backend** — immediate-mode-style software renderer (self-contained, testable) vs GTK/webview FFI (real widgets sooner, drags the FFI callback machinery §32.8/§32.10 forward). The exploration doc leans: software/canvas first for the reconciler's development, FFI backend second.

## 8. Sequencing

Behind: streams (#1394 wave), process/OS + event plumbing (gap items 5/12/14), FFI maturation, and — frankly — real programs. This proposal exists to fix the *architecture* early (so stdlib evolution keeps the path open, per exploration-ui.md §3's keep-in-mind list) and to give the owner something concrete to react to. Nothing is scheduled.
