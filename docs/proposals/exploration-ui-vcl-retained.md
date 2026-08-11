# Exploration — a VCL/LCL-style component library in Bezel

| | |
|---|---|
| **Status** | Exploration — not a proposal; a feasibility map of the classic Delphi/Lazarus component model against the language's actual rules. No implementation is scheduled; sketch-only. |
| **Origin** | Owner question 2026-08-11: proposal 0004 adopted Elm-style MVU — would the traditional VCL/LCL from Pascal land also be at home in Bezel? |
| **Written against** | spec v0.245, stdlib v0.245 (2026-08-11) |
| **Lineage** | exploration-ui.md row 1 ("Retained OO widget tree — substrate, not surface"); 0004's §5 backend tree is the same shape |
| **Verdict up front** | Mechanically, *more* at home than in a GC language: VCL's Owner/Parent dual graph maps one-to-one onto Bezel's owned-fields-vs-borrowed-back-references distinction, `TComponent`'s teardown cascade *is* the §28.4/§28.5/§28.11 cascade with exactly-once made compile-time, and `OnClick` is stdlib §6.5's event pattern under the §28.14 form-3 admission. What does not carry over is everything VCL does with RTTI (DFM/LFM streaming, `published`) and virtual dispatch during construction. The exploration-ui.md verdict stands: **substrate, not surface** — sound and buildable, best positioned as the retained backend under 0004's MVU surface, as the shape of native-toolkit FFI bindings, or as a tools-tier library. |

**API-invention notice (CLAUDE.md rule 2):** `Control`, `Button`, `Edit`, `Label`, `Form`, `Canvas`, and every member on them are **sketch-only vocabulary** — no such stdlib exists and none is proposed here. The types that *are* real: `EventSource<T>` / `IEvent<T>` / `Subscription` (stdlib §6.5, §2.11), `OwningArray<T>` (stdlib §4.5), `Array<T>` (stdlib §4.4). Every *language* rule cited is normative at the pinned revision.

## 1. The headline: VCL's two graphs are Bezel's two reference kinds

VCL is built on a dual reference structure that every Delphi programmer carries in their head: the **Owner** graph (`TComponent.Owner` / `.Components` — who *frees* whom) and the **Parent** graph (`TControl.Parent` / `TWinControl.Controls` — who *contains* whom visually), deliberately distinct so a form can own a control that a nested panel displays. That duality — memory responsibility versus mere reference — is not something Bezel has to *approximate*. It is the language's core distinction, checked instead of documented:

| VCL / LCL | Bezel spelling | Authority |
|---|---|---|
| `TComponent.Owner` frees its `Components` | `owned` fields; `owned _components: OwningArray<Control>` | §28.5, stdlib §4.5 |
| `TControl.Parent` (up-pointer, never freed through) | plain non-`owned` class-typed field `_parent: Control?` — the sanctioned back-reference idiom | §28.5, §28.3 borrowed-field store rule |
| `TWinControl.Controls` (visual children, not owned) | `owned _controls: Array<Control>` — the array *object* is owned (§28.6 fresh-store rule), its *elements* are borrows | §28.6, §28.11 borrowing-collection store rule |
| `Free` / `FreeAndNil`, destructor cascade | `Dispose()` / drop of the owner; cascade over owned fields (reverse declaration order) and owning-collection elements (reverse insertion order) — exactly-once is compile-checked, not a convention | §28.4, §28.5, §28.11 |
| `Button1.OnClick := Button1Click` | `_sub := button.Clicked.Subscribe(OnOkClick)` with the `Subscription` in an owned field of the form | stdlib §6.5; §28.14 form 3 |
| `TThread.Synchronize` (documentation-enforced) | domain sendability — cross-domain touch of a widget is a compile error at the send site | §30.13/§30.14; exploration-ui.md §0 |
| stale component pointer → AV, mitigated by `FreeNotification` | checked disposal mode faults deterministically (`DisposedAccessException`); `Shared<T>`+`Weak<T>` for genuinely dynamic cross-refs (§6 item 3 below) | §28.2 (K4.1), §28.13 |

Two rules make the back-pointer side legal rather than merely tolerated. §28.3's borrowed-field store rule admits storing a borrow in a field when the borrow is **parameter- or receiver-sourced**, with the outlives obligation a recorded K4.3 trust clause; and §9.8's serialization model already names this exact shape — an **owning spanning tree plus back-edges**, where "for a plain borrow the target is necessarily present in the owning tree, because a borrow's lifetime is bounded by an owner that outlives it." A parent-owns-child tree with child→parent pointers is the spec's own DAG case (rationale §5.22), not the refused cyclic case.

## 2. Sketch — the substrate: `Control`

```bezel
module Toolkit;

public record ClickEvent(Sender: Control, X: int, Y: int);

public abstract class Control
  // VCL's reference graphs, as the language's reference kinds:
  private owned _components: OwningArray<Control> := new();  // Owner graph — this control disposes these
  private owned _controls: Array<Control> := new();          // Parent graph — the array object is owned
                                                             //   (§28.6: a fresh value may not land in a
                                                             //   non-owned field); its ELEMENTS are borrows
                                                             //   (§28.11), and `owned` over a trivial-dispose
                                                             //   borrowing collection flips nothing (§28.6)
  private _parent: Control? := null;                         // TControl.Parent — back-reference, NOT owned (§28.5)

  private owned readonly _clicked: EventSource<ClickEvent> := new();
  public property Clicked: IEvent<ClickEvent> => _clicked;   // "OnClick" — the stdlib §6.5 event pattern
  public property Parent: Control? => _parent;

  // Owner graph: take over a component's disposal obligation.
  public method Adopt(owned c: Control)
    _components.Add(owned c);              // discharged as an owned argument (K3.3);
                                           //   lands in an owning slot (K3.1) inside Add
  end Adopt;

  // Parent graph: place a control (owned elsewhere) in this control's visual tree.
  public method Mount(child: Control)
    child._parent := this;    // receiver-sourced borrow into a field — admitted (§28.3); K4.3 trust:
    _controls.Add(child);     //   the parent must outlive the mounting, which the owner arranges
  end Mount;

  protected method HandleClick(x: int, y: int)
    _clicked.Raise(new ClickEvent(this, x, y));
  end HandleClick;

  // Paint(canvas), HitTest, Unmount, layout protocol … elided.
end Control;

public class Button(caption: string): Control
  private _caption: string := caption;

  public property Caption: string
    get => _caption;
    set => do _caption := value; end;      // a real toolkit invalidates here
  end Caption;
end Button;
```

Notes:

- `child._parent := this` inside `Mount` is the §28.3 admission working exactly as specified: the stored borrow's source is the receiver, the store-site check is structural and decidable, and the residual (parent outlives child's use of the pointer) is the K4.3-recorded trust clause — discharged *by construction* whenever the child is owned inside the parent's subtree, since an owner outlives what it owns.
- The `_clicked` field plus `Clicked` property is stdlib §6.5's normative pattern verbatim (its own example is a `Button`); the source's destructor severs the subscriber list before reclamation, so a control's disposal is a deterministic unsubscribe-all with no `Notification` protocol.
- Teardown needs zero user code: disposing a `Control` runs the compiler-generated cascade — owned fields in reverse declaration order, `OwningArray` elements in reverse insertion order, recursively (§28.4, §28.11). The `_controls` element borrows are passive data; the cascade disposes the array object itself (a no-op for a trivial-dispose borrowing collection), and the borrows end with it.

## 3. Sketch — the form: the Delphi authoring experience

Delphi's deepest convention — the form *owns* its named components (`Button1: TButton` fields, owner = form) while containers merely *parent* them — transcribes directly. The form's component fields are `owned`; mounting stores borrows:

```bezel
module App;

public class LoginForm: Form
  // Delphi's form fields, with Owner made static: the form owns each
  // component exactly once; teardown is the compiler's cascade.
  private owned _user: Edit := new();
  private owned _pass: Edit := new();
  private owned _ok: Button := new("Sign in");
  private owned _status: Label := new();
  private owned _okSub: Subscription?;     // declared last ⇒ disposed FIRST (§28.5 reverse order):
                                           //   the handler is severed before its widgets die

  public constructor()
    base("Login");
    Mount(_user);                          // Parent graph: borrows into the visual tree —
    Mount(_pass);                          //   passing an owned field as a plain argument
    Mount(_ok);                            //   lends (§28.6); Mount stores per §28.11
    Mount(_status);
    _okSub := _ok.Clicked.Subscribe(OnOkClick);   // "OkButton.OnClick := OkClick"
  end constructor;

  private method OnOkClick(e: ClickEvent)
    _status.Caption := $"Signing in {_user.Text}…";   // handlers reach siblings through `this`
  end OnOkClick;
end LoginForm;
```

Why each line is admitted:

- **The subscribe line is §28.14 form 3.** `OnOkClick` is an instance method reference, which captures `this` (§27.6) — normally a rejected escape. Storing the returned `Subscription` in an **owned field of `this`** resolves the bounding object to `this`, and stdlib §6.5 states the consequence: the capture is admitted *"regardless of how the publisher was reached — a parameter, a field, an interface face."* Disposing the form disposes `_okSub`, severing the registration before the captured `this` dangles (§28.12 logical-before-physical). This is `OnClick := method` with the lifetime bug class removed.
- **The handler's shape is forced into the VCL style — pleasantly.** For a subscription that lives as long as the form (form 3), §28.14 admits exactly `this`-rooted captures; closures over arbitrary locals are admissible only under form 2's scope-bound `using var` (with a declared-before test and a freeze). Handlers as form methods that reach sibling widgets *through `this`* are the admitted long-lived form — the checker independently reinvents the form-as-mediator convention Delphi reached by habit.
- **Constructor legality:** every non-nullable field carries an initializer, so implicit-`this` calls (`Mount(…)`) pass §22.7's definite-assignment gate, and `Mount` is non-virtual, so the no-virtual-dispatch-in-constructors rule (§22.7) is not touched.
- **Unsubscription** is `Subscription` disposal (or reassignment of `_okSub` — drop-on-overwrite severs the old registration, §28.7). There is no `OnClick := nil`; the handle is the idiom, and `-=` would not match a lambda anyway (§27.7.1).

## 4. Sketch — dynamic construction, reparenting, teardown

```bezel
// Runtime-created controls — VCL's owner-parented dynamic UI.
// Inside some form/builder class:
public method BuildToolbar(bar: Control, captions: Array<string>)
  for var caption in captions do
    var b := new Button(caption);
    bar.Mount(b);            // parent it while this scope still owns it …
    bar.Adopt(owned b);      // … then hand the disposal obligation to the bar (K3.3)
  end;
end BuildToolbar;

// VCL's `ctl.Parent := elsewhere` — reparenting is an explicit ownership move.
// Inside Control:
public method GiveTo(index: int, target: Control)
  var c := _components.PopAt(index);   // owned out of this subtree (stdlib §4.5)
  target.Mount(c);                     // (Unmount from this._controls elided)
  target.Adopt(owned c);               // owned into the other — exactly one owner throughout
end GiveTo;
```

- `FreeAndNil(panel)` becomes `_components.RemoveAt(i)` — disposes the subtree by cascade — or simply letting the owner die.
- `PopAt` returns `owned Control` (stdlib §4.5's drain family), so a reparent is two visible steps — extraction and re-adoption — with the compiler holding the exactly-once invariant across both. There is no window where two owners exist or none does.
- The one honest residual: keeping `_parent`/`_controls` *coherent* with the ownership moves (unmount before `GiveTo`, etc.) is a library invariant the checker does not see — it checks memory, not consistency. Same class of invariant 0004's reconciler owns. A stale sibling borrow after a `RemoveAt` is the K4.3 residual in person; the named backstop is checked disposal mode's deterministic `DisposedAccessException` (§28.2) — a strict upgrade over VCL, where the same mistake is an access violation on a good day.

## 5. What the checker enforces that VCL could only document

1. **Exactly-once disposal.** The double-`Free` and leaked-component bug classes become compile errors and must-use warnings (K1.7 one-holder; K3 discharge).
2. **Handler lifetime.** The owned `Subscription` field ties registration to the subscriber's life, and source destructors guarantee quiescence (stdlib §6.5) — `FreeNotification` bookkeeping has no job left. One recorded residual the checker does not close: `move`-ing the handle out into longer-lived storage re-opens the invocation window (§28.14's registration-lifetime residual), backstopped by K4.1.
3. **Thread affinity.** A control with a `_parent` field is transfer-closed-ineligible (§30.14 rule 3) — the widget tree is UI-domain-confined *by construction*, and "don't touch the UI from a thread" is a compile error at the send site (exploration-ui.md §0's headline win).
4. **Capture discipline.** Long-lived handlers (form 3) may capture only `this`-rooted state; closures over locals are confined to form 2's scope-bound `using var` — the *unbounded* observer closure, the leak-prone half of the pattern, is unwritable.

## 6. What does not carry over

1. **DFM/LFM streaming and `published` RTTI.** No reflection, on settled grounds (rationale §5.18 "Type identity, not reflection", recorded as declined-not-deferred in §18; exploration-ui.md row 7). A form designer would emit Bezel source into the constructor — the Qt-`uic` shape — not stream a resource at runtime. Worth noting: §9.8's *compile-time* serialization synthesis already handles exactly this graph shape (owning tree + `_parent` back-edges as `$ref`), so a declarative form format is not structurally impossible — but it would be codegen against declared constructors, not runtime `published`-property streaming.
2. **Virtual dispatch during construction** (`CreateParams`, `CreateWnd`, `Loaded`). A compile error in non-`sealed` constructors (§22.7). The deferred-handle-creation dance LCL already performs becomes an explicit post-construction step (`Realize()`/`Show()`), which is arguably the honest spelling of what VCL was doing implicitly.
3. **`TComponent.Notification` auto-nil.** No runtime notification of reference holders. The model substitutes structure (owner-outlives-borrower, discharged by construction in a tree) plus the checked-disposal backstop; genuinely dynamic cross-references — an inspector window tracking a selection, say — need `Weak<T>` (§28.13), which is mintable only from a `Shared<T>` handle (`Downgrade()`, stdlib §2.16): the tracked control's owning edge must become a `Shared<Control>` rather than a plain `OwningArray` slot — a local change to that subtree, at `Shared<T>`'s stated refcount cost.
4. **Data binding / `TAction` lists.** Reflection-shaped; falls out (same grounds as WPF-style binding in exploration-ui.md). Plain methods and properties instead — or 0004's answer, where synchronization is the framework's job.
5. **Handler plumbing details.** Subscribe sites accept lambda literals and method references only (§28.14 bound 3) — no handler-in-a-variable indirection; cross-domain publication (`SharedEventSource<T>`) admits capture-free handlers only, so background→UI traffic goes through channels as sendable messages… which is 0004's loop arriving by necessity.
6. **The heterogeneous runtime `Owner` registry.** VCL's `Owner` accepts any component from anywhere at any time. Bezel's spelling is a declared owning slot (`owned` field or `OwningArray`), so ownership topology is part of the type surface — less fluid, and deliberately so.

## 7. Relationship to the adopted MVU direction

Nothing here revises exploration-ui.md's verdict; it confirms it with the concrete rules attached. The retained component model is **sound, buildable, and unusually well-served** by the language — every VCL discipline that was convention becomes a check — but its *ergonomic* cost is unchanged: state synchronization between model and widgets is manual, which is precisely what 0004's `update`/`view` loop exists to remove ("1995 ergonomics reproduced soundly"). The places a VCL-style layer legitimately lives, in order of likelihood:

- **The backend of 0004 §5.** The reconciler's retained widget tree *is* sketch 2's owning tree; this exploration is effectively that tree's design notes.
- **FFI bindings to native toolkits.** LCL's widgetset abstraction is the natural shape for a GTK/Win32/Cocoa binding over §32.8/§32.10's callback machinery — 0004 open question 4's second backend.
- **A tools-tier direct API** for small utilities where immediate wiring beats a framework, alongside (not instead of) the MVU surface.

## 8. Open questions (deliberately few)

1. **Typed attach.** `Adopt`/`Mount` return nothing; a generic `Attach<T>(owned c: T): T where T: Control` returning a typed borrow of the stored child would recover one-liner form building — needs a design pass on the bare-type-parameter return source set (K2.4) before it can be sketched honestly.
2. **The containment-coherence invariant** (§4's residual): whether a library discipline (unmount-before-move protocol) suffices or the two graphs should be fused for v1 (parent owns children directly, one graph — exploration-ui.md's original sketch) with the Owner/Parent split reserved for the FFI-binding case that truly needs it.
3. **Whether a UI debug profile should imply checked disposal mode** — every trust residual named above lands on the K4.1 backstop, which suggests UI development wants that backstop on by default.
