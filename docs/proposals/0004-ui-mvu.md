# Proposal 0004 — Bezel MVU: an Elm-style UI architecture (early draft)

| | |
|---|---|
| **Status** | under-review — Early draft — **proposal-only by owner decision (2026-07-03): no implementation until a future explicit go**; architecture + code shape only, deliberately *not* a full API surface |
| **Origin** | exploration-ui.md's "fits like a glove" finding; owner asked for construction/control examples (2026-07-03) |
| **Written against** | spec v0.216 (owner-directed currency refresh 2026-07-22 — the draft predated `choice` and the v0.208 expression forms; §3's "honest sum-type gap" is now closed by the language and the examples use current syntax. Architecture unchanged.) |
| **Design influences** | Elm (the architecture), iced/Rust (MVU without a VM), **not** SwiftUI's property wrappers, **not** React hooks (exploration-ui.md records why) |
| **The one-line pitch** | The Elm runtime's defining constraint — updates are strictly serial — *is* Bezel's domain model; every piece of MVU maps to an existing language feature, and messages-as-data is exactly what sendability wants |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-08-11 to v0.246: `closed` replaces `sealed abstract` (v0.242), `case move msg of` transfer-marker respell (v0.246); re-verified clean at v0.306 (2026-09-17 sweep) — no retired spelling and no dangling citation: §11.7's `closed`, §19.6, §22.15, §35.4/§35.5/§35.12 and A.6's `CaseStmt` all still resolve, and §35.12's owning-choice admission reads as cited (now keyed on §35.4's *holds-borrows* property, renamed from *borrow-carrying census* at v0.257 — a phrase this draft never uses). Written-against pin unchanged. |

## 1. The four pieces, in Bezel terms

| Elm piece | Bezel spelling |
|---|---|
| `Model` | a record (immutable; updates via `with(…)`, §9.5) |
| `Msg` | a **`choice`** — a closed nominal sum with per-case payloads (§35, v0.200; see §3) |
| `update : Msg -> Model -> Model` | a static method, a `case` over the message cases — **statically exhaustive** (§19.6/§35.5), and `pure`-declarable (§22.15, v0.202) under the Cmd-return effects shape (§4) |
| `view : Model -> Html Msg` | a static method producing an **owned node tree** (not `pure`-declarable — §22.15's records-only allocation excludes class-node construction; "pure by architecture" is the claim, not the keyword) |
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

// ---- Messages: one closed sum (§35, v0.200) ----------------------------
public choice Msg
  Increment,
  Decrement,
  SetTo(value: int);
end Msg;

// ---- Update: pure, statically total over the message set ----------------
class Counter
  public static pure method Update(model: CounterModel, msg: Msg): CounterModel
    return (case msg of
      Increment    => model with(Count := model.Count + 1),
      Decrement    => model with(Count := model.Count - 1),
      SetTo(var v) => model with(Count := v));
  end Update;

  // ---- View: Model -> owned node tree ------------------------------------
  public static method View(model: CounterModel): owned Node
    return Ui.Column([
      Ui.Text($"Count: {model.Count}"),
      Ui.Row([
        Ui.Button("−", Msg.Decrement),        // a button CARRIES its message
        Ui.Button("+", Msg.Increment),
        Ui.Button("reset", Msg.SetTo(0)),
      ]),
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

- **A button carries its message as a value**, Elm-style — `Ui.Button("+", Msg.Increment)` — *not* a callback closure. This is not just aesthetics: this `Msg` is a **value-like choice over value payloads, hence sendable** (§35.12 — it crosses as a copy), so the backend (which may live near an OS event thread) can carry it across domains legally; a closure capturing `model` could not cross, and the compiler would say so. Elm's design and §30.14 turn out to want the same thing.
- **`update` has no `_` arm and needs none** — a choice scrutinee is exhaustive by case coverage (§19.6/§35.5). Adding a `Reset` case to `Msg` makes every non-exhaustive `update` in the program a compile-time miss, which is the Elm guarantee the original draft could not express (see §3).
- **`update` is declared `pure`** (§22.15): record `with(…)` copies are records-only allocation, so the no-effects rule Elm holds by convention is compiler-checked here. (This is the effects-shape tiebreaker §4 records.)
- **`update` is the only place state changes**, and it is a `(case …)` atom + `with(…)` — pattern matching and record updates (§19, §9.5, both in their v0.208 paren forms), no framework state cells.
- **`view` returns an `owned Node`** — a fresh tree per render, consumed Pattern-B style by the reconciler (§5).
- `Ui.Run` never returns until quit; it *is* the UI domain's life. Method references wire it exactly like 0002's route handlers.

## 3. `Msg` is a `choice` — the sum-type gap is closed (v0.200)

Elm's `Msg` is a closed tagged union, and since v0.200 **so is Bezel's**: proposal 0041 graduated as the `choice` kind (§35) — a closed nominal sum with per-case payloads, qualified construction, positional destructuring, and full static exhaustiveness. The original draft's marker-interface encoding, and two of its three recorded costs, dissolve:

1. **Static exhaustiveness — gone as a cost.** A choice scrutinee is matched by case coverage (§19.6/§35.5): `update` needs no `_` arm, and forgetting a new message case is a **compile error at every match**, not a silent no-op. The draft's "strongest argument for a future closed-set construct" was answered by the language (twice over — `closed` class hierarchies, §11.7/§19.6, are the second closed set; see cost 3 for where each fits).
2. **Ceremony — reduced.** A payload case is part of one declaration (`SetTo(value: int),`), payload-free kinds are bare case names, and message construction is a qualified value (`Msg.Increment`), not a `new`.
3. **Collection payloads — the cost survives in a narrower, choice-shaped form.** A case that *owns* a collection (`UsersLoaded(owned names: Array<string>)`) makes the whole choice **owning-classified** (§35.4) and therefore **not directly sendable** (§35.12) — the same underlying rule that made collection-bearing records inadmissible in the original draft. Two spellings exist now, both closed-set and both exhaustively matchable, and choosing between them is this draft's refreshed open question 3:
   - **(a) Split the message set.** UI-local messages stay one value-like sendable choice; effect-completion messages with owned collection payloads are leaves of a **`closed` hierarchy** of sealed sendable classes with `owned` fields (§11.7; §30.14 class rule 2 — the draft's original `UsersLoaded` shape, now under a *closed* root, so the update match over completions is exhaustive too, by leaf coverage §19.6).
   - **(b) One owning choice, enveloped at the boundary.** Keep every message in a single `choice Msg` and cross domains inside a transfer-closed carrier class — §35.12's recorded rule-2 admission: an owning-choice-typed field whose `owned` payload classes are sendable crosses inside its owner's ownership tree. One closed sum for `update` (the purest Elm reading), one envelope type on the wire.

## 4. Control beyond clicks: async effects as messages

The Elm rule: `update` never performs effects; it *requests* them, and completions come back as messages. In Bezel the request is simply a task whose result is a `Msg` — and the loop enforces the rest:

```bezel
public choice Msg
  FetchUsers,
  UsersLoaded(owned names: Array<string>),  // owning case ⇒ the choice is owning-classified (§35.4)
  FetchFailed(reason: string);              //   and crosses domains enveloped (§3, spelling b)
end Msg;

public static method Update(model: AppModel, msg: Msg, fx: Effects): AppModel
  case move msg of                          // owning match (§35.5): the active case's
    FetchUsers => do                        //   owned payload transfers into its binding
      // Effects.Run: spawn the task on a worker domain; when it completes,
      // its Msg result is POSTED back into this loop (a channel underneath —
      // the boundary carriage is §3's spelling (b); with spelling (a) the
      // §30.14 sendable-result rule is checked directly at this call).
      fx.Run(async lambda() => do
        try
          using var resp := await Http.Get("https://api.example.com/users");
          return Msg.UsersLoaded(await resp.JsonOwned<Array<string>>());
        catch var e: HttpException => return Msg.FetchFailed(e.Message);
        end;
      end);
      return model with(Loading := true);
    end,
    UsersLoaded(var names) => return model with(Loading := false, Users := names),
    FetchFailed(var why)   => return model with(Loading := false, Error := why),
  end case;                                 // exhaustive by case coverage — no `_` arm
end Update;
```

- The effect closure runs on a **worker** domain; the only thing that crosses back is the returned `Msg` — checked at the boundary. There is no way to mutate `model` from the fetch; the type system closes the door Elm closes by purity convention.
- The multi-statement `FetchUsers` arm rides the **statement-position** `case` (grammar A.6 CaseStmt — `do … end` arm bodies are statement-form only, §19.2/v0.208); a one-armed helper extraction would let `update` stay an expression, which is §19.2's standing doctrine for arm bodies that grow.
- **The `Users := names` slot is deliberately glossed** (the original draft glossed it silently; this refresh names it): a record field cannot *own* the transferred array (K1.7/§28.5), so a real `AppModel` either carries the collection as a borrow-bearing record (fine domain-locally, fights nothing until it crosses) or is itself a class. This is the model half of open question 1's representation decision.
- Timers/subscriptions follow the same shape (`fx.Every(TimeSpan.FromSeconds(1), lambda() => Msg.Tick)`) — deferred detail.
- Whether effects are requested via an `fx` handle (shown) or by `update` returning `(Model, Cmd)` Elm-style is **open question 2** — the handle avoids inventing a tuple-ish return; the Elm shape is purer. **v0.202's `pure` sharpens this question into a real tiebreaker**: under the Cmd-return shape `update` can be *declared* `pure` (§22.15 — effects become returned data; §2's counter already is), while an `fx`-handle `update` structurally cannot. The draft now leans Cmd-shaped; the tuple-ish return it needs (a `(Model, Cmd)` product — a record can carry both when Cmd is a value-like choice of effect *descriptions*) is the concrete design to work out.

## 5. Ownership of the node tree (the Bezel-specific part)

`view` builds a fresh **owned** tree each render. The reconciler is its single consumer:

- It diffs the fresh tree against the retained previous tree (kind + properties per node; §9.2's auto-generated record equality where nodes carry record props), emits patches to the retained **backend widget tree** (an owning tree — teardown is the §28.4 cascade), then the fresh tree *becomes* the previous (the old previous is disposed).
- Every node the view allocates is therefore disposed exactly once, by the machinery the language already has — the render loop cannot leak by construction, which no mainstream VDOM can claim.
- **Open question 1** (the main representational decision): nodes as an owning **class** tree (natural children lists via `OwningArray<Node>`, diffing by per-kind compare) vs **records** (free structural equality, but class-typed/collection children make records borrow-bearing — fights the value-tree story). The sketch assumes the class tree; a hybrid (class nodes carrying record *prop bags* for equality-cheap diffing) looks promising and needs a design pass.

## 6. What v-anything this is *not*

No hooks, no property wrappers, no hidden per-component state, no auto-tracked signals, no reflection binding — each falls out on grounds recorded in exploration-ui.md. The entire framework state is: the model (yours, a record), the previous tree (the reconciler's, owned), the backend tree (owned). Time-travel debugging is `Array<AppModel>` — keep the snapshots you want; they are values.

## 7. Open questions (deliberately few, this is early)

1. **Node representation** — class tree vs record tree vs hybrid (§5), now explicitly including the *model* half (§4: where do a model's collections live once messages can transfer them in).
2. **Effects shape** — `fx` handle vs Elm-style `(Model, Cmd)` return (§4). Refreshed lean: Cmd-return, because v0.202 `pure` then applies to `update` as a checked declaration rather than a convention.
3. ~~**Closed unions** — does the `Msg` exhaustiveness gap justify a `sealed interface`-class feature?~~ **Answered by the language** (2026-07-22 refresh): `choice` graduated in v0.200 (proposal 0041) and closed class hierarchies (`closed`, §11.7) already existed — §3 now uses them. The narrowed successor question: for messages that own collections, split-set-with-hierarchy vs one-owning-choice-plus-envelope (§3's spellings (a)/(b), §35.12).
4. **First backend** — immediate-mode-style software renderer (self-contained, testable) vs GTK/webview FFI (real widgets sooner, drags the FFI callback machinery §32.8/§32.10 forward). The exploration doc leans: software/canvas first for the reconciler's development, FFI backend second.

## 8. Sequencing

Behind: streams (#1394 wave), process/OS + event plumbing (gap items 5/12/14), FFI maturation, and — frankly — real programs. This proposal exists to fix the *architecture* early (so stdlib evolution keeps the path open, per exploration-ui.md §3's keep-in-mind list) and to give the owner something concrete to react to. Nothing is scheduled.
