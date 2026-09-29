# Exploration — an immediate-mode UI library in Bezel: API surface and usage

| | |
|---|---|
| **Status** | Exploration — not a proposal; a detailed API sketch plus traced usage code for the immediate-mode paradigm against the language's actual rules. Nothing is scheduled; nothing here amends a normative document. |
| **Origin** | Owner question 2026-09-18: sketch the API shape and usage code for the immediate-mode approach that exploration-ui.md rated a "strong utility citizen". |
| **Written against** | spec v0.306, stdlib v0.306 (2026-09-18) |
| **Lineage** | exploration-ui.md row 2 (immediate mode: "the cheapest sound first library"); proposal 0004 open question 4 (an immediate-mode-style software renderer as the MVU reconciler's first backend); exploration-ui-vcl-retained.md (the retained lane, for contrast — its login form is re-done here in §5.2); exploration-ui-toolkit-binding.md §1 (the flat-C FFI seam, reused for the platform layer). |
| **Design influences** | Dear ImGui (ID stack, seed-hashed identity, `bool*`-style write-back), egui (`Ui` sub-allocators, closure-nested layout, `Response`, per-widget `Memory`, headless testing), microui (fixed-vocabulary command list). Not Unity IMGUI's global-state formulation. |
| **Verdict up front** | Immediate mode is *more* at home in Bezel than in C or Rust, for three compiler-checked reasons: (1) the **call-bounded default** for function-typed parameters (§28.14) makes "the library retains nothing of the caller's" a *type-system fact* — a layout closure cannot be stored, so the defining immediate-mode invariant needs no discipline; (2) **`ref` parameters** (§22.3) give `ui.Checkbox("Dark", ref dark)` without pointers, and a `ref` argument may never name owned state, so a widget can never be bound to a slot whose disposal it could race; (3) the **mutable-static ban** (§22.12) makes `ImGui::GetCurrentContext()` unwritable — the context is an explicit object everywhere, which is the egui shape, and because it holds borrows it is **non-sendable by construction** (§30.14), so "call the UI from a worker" is a compile error at the send site (exploration-ui.md §0's headline, again). The costs are the known immediate-mode ones (no native look, accessibility, IME depth, continuous redraw) plus one Bezel-specific one: **no `Any`**, so per-widget memory is a closed set of typed side tables rather than egui's type-erased map. The whole library is ordinary user-level Bezel over the shipped stdlib; the only foreign code is the platform layer (window, input pump, rasterizer), which — unlike the retained lane — needs **no callback inversion**, only a non-blocking poll. |

**API-invention notice (CLAUDE.md rule 2):** `UiContext`, `Ui`, `Response`, `WidgetId`, `DrawCommand`, `FrameInput`, `UiMemory`, `IFontMetrics`, `IUiPlatform` and every member on them are **sketch-only vocabulary** — no such stdlib exists and none is proposed here. The types that *are* real, and are the whole substrate the sketch stands on: `Array<T>`, `Dictionary<K,V>`, `OwningDictionary<K,V>`, `OwningArray<T>`, `Set<T>`, `IReadOnlyList<T>` (stdlib §4), `StringBuilder` (§3), `TimeSpan`, `Stopwatch` (`System.Diagnostics`), `Channel<T>`/`ChannelSender<T>`, `Domain.Spawn` (§6), `Debug.Assert`, and the FFI carriers (§10). Every *language* rule cited is normative at the pinned revision; every line of Bezel below was traced against `Bezel_For_Code_Generation.md` v0.306 and the spec sections it cites, then adversarially re-checked against the spec and stdlib themselves (2026-09-19). Blocks marked `…` or "body elided", and the body-less signature listing in §4.3, are deliberate elisions and not compilable fragments; every *complete* method is meant to compile as written.

## 0. The headline: three rules that make the paradigm's discipline a type-system fact

Immediate mode's defining property is that the UI is a **function called once per frame**, that application state stays in the application and is merely *lent* to the UI for the duration of the call, and that the library keeps only its own bookkeeping between frames. Every mainstream implementation holds that invariant by convention (Dear ImGui) or by the borrow checker (egui). Bezel holds it by three rules that already exist:

| Immediate-mode invariant | How C/ImGui holds it | How Bezel holds it | Authority |
|---|---|---|---|
| The library never retains a caller's callback | convention | A function-typed parameter without `scoped` is **call-bounded**: "the callee receives the callback for the duration of the call only", and "**any** closure — capture-bearing or not — may be passed to a call-bounded parameter." A nested-layout closure (`ui.Horizontal(lambda(row) => …)`) therefore cannot end up in a library field, and the caller may capture anything, borrows included. | §28.14 (v0.177 call-bounded default) |
| Widgets write back into caller state without owning it | `bool*` / `float*` | `ref` parameters, written at the declaration and the call site. A `ref` argument may name "no `using var` binding" and "no location of **owned state** — an owned local or `owned` parameter, an `owned` field, an owning-collection or owning-inline-array slot" — so a widget is bound only to value storage the caller keeps alive for the call. | §22.3 |
| Locals mutated by a nested closure are the caller's locals | n/a | "Captured variables are heap-allocated in a closure object. Modifications to a captured variable inside the lambda are visible outside." A local passed as `ref` inside any lambda is *mutably captured* (§7.4), which is exactly the shared-cell semantics a nested `row.Checkbox("x", ref flag)` needs. | §27.5, §7.4 |
| There is one UI thread | documentation | The context holds borrows and is therefore not transfer-closed; sending it is a send-site compile error. The frame loop runs on one domain; background results re-enter only as sendable values through a channel. | §30.13, §30.14 |
| There is no hidden global context | `ImGui::SetCurrentContext` | A mutable static is a compile error outside four admitted forms, none of which is "a UI context". The context is a parameter or a field, always. | §22.12 |
| Per-frame allocations are freed every frame | manual arena reset | A child `Ui` created for a nested closure is an owned local of the parent's method: disposed at return, on the exception edge too, with no plumbing. The library's per-frame objects are trivial-dispose classes (no destructor, no owned resource fields), so no must-use warnings attach anywhere. | §28.3, §28.9 |

What does **not** carry over from C or Rust, stated once: no `Any`/`Box<dyn Any>` (per-widget memory is a closed typed set — §6), no wrapping integer arithmetic (a hand-rolled FNV faults with `OverflowException` under §6.1's checked arithmetic — widget IDs are built from the auto-generated record hash instead, §2.1), no global font/style singleton (the platform lends fonts to the context; style is a record on the context), and no `&mut` exclusivity: Bezel does not stop a nested closure from calling the *outer* `ui` instead of the child it was handed (§4.2 records the residual and its runtime backstop).

## 1. The frame model

```
                app-owned (lives across frames)             library-owned (lives across frames)
  ┌───────────────────────────────────────┐   ┌──────────────────────────────────────────────┐
  │ AppState (class; value fields, Arrays) │   │ UiContext                                   │
  └───────────────────────────────────────┘   │   owned FrameInput   ← platform pumps events │
                 │ borrowed for the call        │   owned UiMemory     ← typed side tables     │
                 ▼                              │   owned DrawList     → platform presents     │
  every frame:  app.Draw(ui.Root)  ─────────►  │   owned Ui (root)   per-frame cursor/layout │
                                                └──────────────────────────────────────────────┘
  background domains ──► Channel<Msg> ──► polled at the top of the frame, applied to AppState
```

The application **owns the loop** — the defining difference from the retained lane, where the toolkit owns it and the app is called back (exploration-ui-toolkit-binding.md §1). Per frame: the platform fills the context's input, the context opens a frame, the application's UI *function* runs against a borrowed root `Ui`, the context closes the frame (sweeps stale memory, seals the draw list), and the platform presents the draw list.

```bezel
module Demo;

import System.Diagnostics;      // Stopwatch, used by the platform layer

public class Program
  public static method Main(): int
    using var platform := SoftwarePlatform.Open("Bezel demo", 800, 600);   // sketch: the FFI seam (§7)
    using var ctx := new UiContext(platform.Fonts);                        // borrows the platform's fonts
    var app := new AppState();                                             // trivial-dispose: plain `var`

    while platform.Pump(ctx.Input) do        // fills this frame's input; false once the OS asked to quit
      ctx.BeginFrame();                      // resets the root Ui, advances time, clears the draw list
      app.Draw(ctx.Root);                    // THE UI IS A FUNCTION: state borrowed for the call
      ctx.EndFrame();                        // sweeps untouched memory, closes clip stacks
      platform.Present(ctx.Output);          // consumes the draw list; blocks for vsync if it wants to
    end;
    return 0;
  end Main;
end Program;
```

Why each line is admitted: `SoftwarePlatform.Open` is a static returning a class, so it must (and does) declare `: owned SoftwarePlatform` (§28.3, *factory methods and sourceless returns*); `platform.Fonts` is a property read — a borrow of the platform's storage — bound into the context's `_fonts` field under the constructor-parameter back-reference idiom (§28.3), the K4.3 trust clause being discharged by declaration order (the platform is declared first, so it is disposed *last*, §28.3 reverse-order disposal of owned locals); `ctx.Input`/`ctx.Root`/`ctx.Output` are property reads, so each is a borrow of the context's storage that the callee may use for the call and not retain (§28.3 "reads of storage are not calls"); `app` is a plain `var` on a trivial-dispose class and so carries no must-use warning (§28.9).

## 2. The value vocabulary

All geometry, colour, identity and results are **records** — value types with no disposal and no classification (§28.1), auto-equality and auto-hashing over their positional fields (§9.1), and `with(…)` updates (§9.5). The combined form (§9.3) carries the operators.

```bezel
module Bezel.Ui.Immediate;              // sketch-only module name

public record Vec2(X: float, Y: float)
  public static readonly property Zero: Vec2 := new Vec2(0.0f, 0.0f);
  public static pure method operator +(a: Vec2, b: Vec2): Vec2 => new Vec2(a.X + b.X, a.Y + b.Y);
  public static pure method operator -(a: Vec2, b: Vec2): Vec2 => new Vec2(a.X - b.X, a.Y - b.Y);
  public static pure method operator *(a: Vec2, s: float): Vec2 => new Vec2(a.X * s, a.Y * s);
end Vec2;

public record Rect(Min: Vec2, Max: Vec2)
  public property Width: float => Max.X - Min.X;
  public property Height: float => Max.Y - Min.Y;
  public property Size: Vec2 => Max - Min;
  public method Contains(p: Vec2): bool =>
    p.X >= Min.X and p.X < Max.X and p.Y >= Min.Y and p.Y < Max.Y;
  public method Intersect(other: Rect): Rect =>
    new Rect(new Vec2(Math.Max(Min.X, other.Min.X), Math.Max(Min.Y, other.Min.Y)),
             new Vec2(Math.Min(Max.X, other.Max.X), Math.Min(Max.Y, other.Max.Y)));
  public static method FromMinSize(min: Vec2, size: Vec2): Rect => new Rect(min, min + size);
end Rect;

public record Color(R: byte, G: byte, B: byte, A: byte)
  public static pure method Rgb(r: byte, g: byte, b: byte): Color => new Color(r, g, b, 255);
  public static readonly property White: Color := new Color(255, 255, 255, 255);
  public static readonly property Transparent: Color := new Color(0, 0, 0, 0);
end Color;

public enum PointerButton  Primary, Secondary, Middle;  end PointerButton;
public enum Key            Escape, Enter, Tab, Backspace, Delete, Left, Right, Up, Down, Home, End_, A, C, V, X, Z;  end Key;
public record Modifiers(Ctrl: bool, Shift: bool, Alt: bool)
  public static readonly property None: Modifiers := new Modifiers(false, false, false);
end Modifiers;

public enum LayoutDirection  TopDown, LeftToRight;  end LayoutDirection;
public enum Align            Start, Center, End_;   end Align;

/// What a widget wants to react to; Interact (§4.2) reads it. The positional fields may not share a
/// name with the static members (§9.3/§11.3), hence the `Wants` prefix.
public record Sense(WantsClick: bool, WantsDrag: bool, WantsFocus: bool)
  public static readonly property Hover: Sense := new Sense(false, false, false);
  public static readonly property Click: Sense := new Sense(true, false, false);
  public static readonly property Drag:  Sense := new Sense(true, true, false);
  public static readonly property Focus: Sense := new Sense(true, false, true);
end Sense;

/// The per-frame answer every widget returns. A value: keep it, compare it, ignore it.
public record Response(
  Id: WidgetId,
  Rect: Rect,
  Hovered: bool,       // pointer is over Rect and nothing above captured it
  Pressed: bool,       // this widget is the active (mouse-captured) one this frame
  Clicked: bool,       // released over the widget it was pressed on, this frame
  Changed: bool,       // the widget wrote through a `ref` parameter this frame
  Focused: bool,       // has keyboard focus
  DragDelta: Vec2,     // pointer motion while Pressed, else Zero
);
```

(`End_` because `end` is a reserved word and `End` would read badly beside it; the trailing comma on the `Response` field list is §1.6's one rule. `Key` is a deliberately short sketch enum; a real one enumerates the keyboard.)

### 2.1 Identity: `WidgetId`

Dear ImGui's identity scheme — a 32-bit hash of the label seeded by the enclosing scope's hash — transcribes onto the positional-record hash with no arithmetic at all, which matters because §6.1's integer arithmetic is **checked** (a wrapping FNV would raise `OverflowException`):

```bezel
public record WidgetId(Seed: int, Own: int)                  // auto-hashable (§9.1): order-sensitive combine
  public static readonly property Root: WidgetId := new WidgetId(0, 0);
  public method Child(label: string): WidgetId => new WidgetId(GetHashCode(), label.GetHashCode());
  public method Child(index: int): WidgetId    => new WidgetId(GetHashCode(), index);
end WidgetId;
```

- `GetHashCode()` on `this` is the compiler-generated positional hash (§9.1), which §8.3 requires to be **order-sensitive** over `(Seed, Own)` — so `Root.Child("a").Child("b")` and `Root.Child("b").Child("a")` are not systematically equal, and the ID stack is a chain of these calls. `string.GetHashCode()` is `pure` (stdlib §9.6).
- A `WidgetId` keys every memory table (`Dictionary<WidgetId, …>` requires `hashable`, which a positional record of `int`s is — spec §3.6's derived-property table).
- **Two labels in one scope collide**, as in ImGui — and the remedy is the same: salt the scope (`ui.WithId(i, …)`, §4.2) or the widget (`id := …`). The sketch does not adopt ImGui's `"label##salt"` string convention: parsing meaning out of a display string is the kind of implicit convention the language declines elsewhere, and a named argument does the job visibly.
- **Not stable across builds.** §8.3 leaves the combining algorithm implementation-defined, and `string.GetHashCode()`'s algorithm is unspecified, so IDs are valid within one process only. That is all in-process memory needs; persisting window positions across runs (egui does) would need a stable hash — open question 4.

## 3. Input in, commands out: the two boundaries with the platform

Both boundaries are **plain data**: a closed `choice` of input events and a closed `choice` of draw commands, each carrying only value payloads (records, primitives, `string`), hence value-like (§35.4) and sendable (§35.12). That is what lets the platform layer live on another domain if it ever needs to, and what makes headless testing (§5.5) a matter of pushing events and reading commands.

```bezel
public choice InputEvent
  PointerMoved(position: Vec2),
  PointerPressed(button: PointerButton, position: Vec2),
  PointerReleased(button: PointerButton, position: Vec2),
  Scrolled(delta: Vec2),
  KeyPressed(key: Key, modifiers: Modifiers),
  KeyReleased(key: Key),
  TextEntered(text: string),            // committed text after IME composition — never raw keys
  Resized(size: Vec2),
  QuitRequested;
end InputEvent;

/// One frame's input. Owned by the context; the platform fills it, the widgets read it.
public class FrameInput
  private owned _events: Array<InputEvent> := new Array<InputEvent>();
  public property Events: IReadOnlyList<InputEvent> => _events;    // a borrow of storage
  public property PointerPosition: Vec2;
  public property ScreenSize: Vec2;
  public property Time: TimeSpan;          // monotonic since platform start (Stopwatch.Elapsed)
  public property DeltaTime: TimeSpan;

  public method Push(e: InputEvent) => _events.Add(e);   // value elements: no fresh-store lint (§15.4)
  public method Clear() => _events.Clear();
  public method IsDown(button: PointerButton): bool
    // derived by the context from the press/release stream; body elided
  end IsDown;
end FrameInput;

public choice DrawCommand
  FillRect(rect: Rect, color: Color, rounding: float),
  StrokeRect(rect: Rect, color: Color, thickness: float),
  Line(from: Vec2, to: Vec2, color: Color, thickness: float),
  Circle(center: Vec2, radius: float, fill: Color),
  Text(origin: Vec2, text: string, color: Color, size: float),
  PushClip(rect: Rect),
  PopClip;
end DrawCommand;

public class DrawList
  private owned _commands: Array<DrawCommand> := new Array<DrawCommand>();
  public property Commands: IReadOnlyList<DrawCommand> => _commands;
  public method Add(c: DrawCommand) => _commands.Add(c);
  public method Clear() => _commands.Clear();
end DrawList;

/// Text measurement is the one thing the library cannot do itself. The platform lends this.
public interface IFontMetrics
  method Measure(text: string, size: float): Vec2;
  readonly property LineHeight: float;
end IFontMetrics;
```

`Array<DrawCommand>` is a **borrowing-family** collection over **value** elements: `Add` stores a copy, `Dispose()` is a no-op, no fresh-store lint attaches (§15.4 exempts runtime-managed and value elements), and the class is trivial-dispose (§28.9). The command vocabulary is microui-sized on purpose: a backend that can fill, stroke, draw text and clip can render everything the widget set produces; images and custom meshes are §10's open question 3.

## 4. The context and the `Ui` handle

### 4.1 `UiContext` — what lives across frames

```bezel
public record UiStyle(
  Spacing: float, Padding: float, Rounding: float, FontSize: float,
  Text: Color, Panel: Color, Widget: Color, WidgetHovered: Color, WidgetActive: Color, Accent: Color,
)
  public static method Default(): UiStyle
    // returns a populated record; body elided
  end Default;
end UiStyle;

public class UiContext(fonts: IFontMetrics)
  private _fonts: IFontMetrics := fonts;                  // back-reference (§28.3): the platform outlives us
  private owned _input:  FrameInput := new FrameInput();
  private owned _memory: UiMemory   := new UiMemory();
  private owned _output: DrawList   := new DrawList();
  private owned _root:   Ui?        := null;              // created at the first BeginFrame (needs `this`)
  private _frame: long := 0;

  public property Input:  FrameInput => _input;
  public property Memory: UiMemory   => _memory;
  public property Output: DrawList   => _output;
  public property Fonts:  IFontMetrics => _fonts;
  public property Style:  UiStyle;                        // a record auto-property: assign to restyle
  public property Root:   Ui => _root!;                   // valid between BeginFrame and EndFrame
  public property FrameNumber: long => _frame;

  /// True when a widget is mid-animation or mid-drag and wants another frame without waiting for input.
  public property WantsRepaint: bool;

  public method BeginFrame()
    _frame := _frame + 1;
    _output.Clear();
    _memory.BeginFrame();
    WantsRepaint := false;
    var screen := Rect.FromMinSize(Vec2.Zero, _input.ScreenSize);
    _root := new Ui(this, screen, WidgetId.Root, LayoutDirection.TopDown);  // drop-on-overwrite disposes last frame's root (§28.7)
  end BeginFrame;

  public method EndFrame()
    _memory.Sweep();          // forget every id no widget touched this frame (§6)
    _input.Clear();
  end EndFrame;
end UiContext;
```

- `_root` is a **nullable owned field** because its constructor needs `this`, which a field initializer may not reference (§22.8), and a primary-constructor class has no body in which to assign it (§22.10) — so it is created at the first `BeginFrame`. Assigning a fresh `Ui` each frame is drop-on-overwrite (§28.7): last frame's root is disposed at the store. `Ui` is trivial-dispose, so this is a free allocation policy, not a resource one.
- `Style` is a record auto-property: `ctx.Style := ctx.Style with(Accent := Color.Rgb(40, 120, 255));` is a whole-value replacement (§9.5) — there is no push/pop style stack; scoped restyling is a `Ui`-level override (§4.2), which is a parameter, not hidden state.
- The context is **not sendable**: `_fonts` is a plain borrow field, so the class is not transfer-closed (§30.14 rule 3). Handing `ctx` to `Domain.Spawn` is rejected at the call. This is the UI-thread guarantee, free.

### 4.2 `Ui` — the per-frame handle every widget takes

`Ui` is egui's sub-allocator: a **cursor** over a rectangle, a layout direction, an ID scope, and a borrow of the context. Nesting creates a *child* `Ui` for the duration of one closure call.

```bezel
public class Ui(ctx: UiContext, rect: Rect, id: WidgetId, direction: LayoutDirection)
  private _ctx: UiContext := ctx;                 // back-reference to the frame's owner (§28.3)
  private _rect: Rect := rect;
  private _cursor: Vec2 := rect.Min;
  private _maxExtent: Vec2 := rect.Min;           // grows as children are placed; feeds the parent's cursor
  private _id: WidgetId := id;
  private _direction: LayoutDirection := direction;
  private _enabled: bool := true;
  private _suspended: bool := false;              // true while a child Ui is live (see RunChild)

  public property Context: UiContext => _ctx;
  public property Id: WidgetId => _id;
  public property Available: Rect => new Rect(_cursor, _rect.Max);
  public property MinSize: Vec2 => _maxExtent - _rect.Min;
  public property Style: UiStyle => _ctx.Style;
  public property Input: FrameInput => _ctx.Input;
  public method Paint(): Painter => new Painter(_ctx.Output, _rect);   // §4.4 — a fresh borrow-bearing record

  // ---- the two primitives every widget is built from ------------------------------------
  /// Reserve a rectangle at the cursor, advance the cursor, and return the rectangle.
  public method Allocate(size: Vec2): Rect
    requires(not _suspended)

    var r := Rect.FromMinSize(_cursor, size);
    Advance(size);
    return r;
  end Allocate;

  /// Compute this frame's interaction with `rect` under `id`, updating hot/active/focus in memory.
  public method Interact(rect: Rect, id: WidgetId, sense: Sense): Response
    // reads _ctx.Input, reads/writes _ctx.Memory.Active / Focus; body elided
  end Interact;

  // ---- identity and scoping -----------------------------------------------------------
  /// Run `body` in a child scope whose IDs are salted by `salt` (loops, repeated labels).
  public method WithId(salt: int, body: (Ui) -> ())
    var child := new Ui(_ctx, Available, _id.Child(salt), _direction);   // owned local, trivial-dispose
    RunChild(child, body);
  end WithId;
  public method WithId(salt: string, body: (Ui) -> ())   // same, string salt
    var child := new Ui(_ctx, Available, _id.Child(salt), _direction);
    RunChild(child, body);
  end WithId;

  // ---- layout ---------------------------------------------------------------------------
  public method Horizontal(body: (Ui) -> ())
    var child := new Ui(_ctx, Available, _id.Child("h"), LayoutDirection.LeftToRight);
    RunChild(child, body);
  end Horizontal;
  public method Vertical(body: (Ui) -> ())
    var child := new Ui(_ctx, Available, _id.Child("v"), LayoutDirection.TopDown);
    RunChild(child, body);
  end Vertical;
  public method Indented(amount: float, body: (Ui) -> ()) … end Indented;
  public method Sized(size: Vec2, body: (Ui) -> ()) … end Sized;       // a fixed-size child
  public method Disabled(disabled: bool, body: (Ui) -> ()) … end Disabled;   // greys out, drops Sense
  public method AddSpace(amount: float) => Advance(SpaceVec(amount));
  public method Separator() … end Separator;

  private method RunChild(child: Ui, body: (Ui) -> ())     // `body` is call-bounded (§28.14): it cannot be retained
    _suspended := true;
    try
      body(child);                                          // the caller's closure, invoked exactly here
    finally
      _suspended := false;                                  // a throwing body must not wedge the parent (§31.2)
    end try;
    Advance(child.MinSize);                                 // the child's extent becomes the parent's step
  end RunChild;

  private method Advance(size: Vec2) … end Advance;         // cursor += size (+ spacing) along _direction; grows _maxExtent
  private method SpaceVec(amount: float): Vec2 => (if _direction = LayoutDirection.TopDown then new Vec2(0.0f, amount) else new Vec2(amount, 0.0f));
end Ui;
```

Why each line is admitted:

- **`body: (Ui) -> ()` without `scoped` is call-bounded** (§28.14): the callee may invoke it during the call and may not store it; the caller may pass a closure capturing anything — the `ref`-bound locals, `app` fields, other borrows. This is the whole reason the nested style is sound *and* ergonomic in Bezel: the compiler proves the library keeps nothing, so the caller never has to think about it.
- **`var child := new Ui(…)`** is an owned local (fresh producer, §28.3 row 1) of a trivial-dispose class: no must-use warning, disposed at method return on both edges (§28.9, §28.3). Passing `child` to `RunChild(child, …)` lends it (§28.6). The closure sees a plain borrow parameter.
- **`_ctx: UiContext := ctx`** is the back-reference idiom (§28.3): a constructor-parameter borrow into a plain field, admitted structurally; the K4.3 outlives obligation is discharged by construction because every `Ui` is created by the context or by a `Ui` the context transitively owns, inside one frame.
- **`_suspended` plus `requires(not _suspended)`** is the runtime backstop for the one thing Rust's `&mut` gives egui and Bezel deliberately does not (no exclusivity on borrows): inside `ui.Horizontal(lambda(row) => …)`, calling `ui.Button(...)` instead of `row.Button(...)` compiles. It is a contract violation at the parent's `Allocate` — `ContractViolationException` (§22.16), in every build mode because it is a `requires` — and reads as "you used the outer Ui inside a child scope". Cheap, deterministic, and the failure names itself.
- `Paint()` returns a *fresh record*: the sketch makes `Painter` a borrow-bearing record over the draw list (§4.4), constructed from the field-sourced borrow `_ctx.Output`, which is not a temporary (§9.7 rejects only literals and unbound fresh call results; a getter result is a receiver-sourced borrow, §28.3, and the hop composes: `_ctx.Output` is sourced from `_ctx`, itself sourced from `this`).

### 4.3 Widgets: the surface

Every widget is a method on `Ui` and takes its display text first and its bound state as `ref` where it has any. The surface is split into three tiers by **what §21.9 says about an ignored result**: an expression statement whose result is non-`()` draws an *unused result* warning, so a `Response` may only be returned where the caller nearly always branches on it. Fact-returning widgets return `Response`; display-only widgets and containers return `()`; state-writing widgets return `()` and offer an **`out response: Response`** overload — an ordinary plain `out` (§22.3/§22.4), not one of stdlib §1's four Try shapes, chosen so that the common line draws no §21.9 warning — for the frames where the caller wants the `Changed` edge or the rectangle. The list is the "minimal viable" set; each is buildable from `Allocate` + `Interact` + `Paint()` + memory, and the sketch shows two in full (§4.5).

```bezel
  // ---- fact-returning widgets: the caller nearly always branches on the result -----------
  public method Button(text: string): Response                          // .Clicked
  public method SmallButton(text: string): Response
  public method Selectable(text: string, selected: bool): Response      // list rows; .Clicked
  public method Hyperlink(text: string, url: string): Response          // .Clicked; the app opens it
  public method MenuItem(text: string, shortcut: string := ""): Response   // .Clicked closes the menu
  public method Shortcut(key: Key, modifiers: Modifiers): bool          // true on the press frame, once

  // ---- display-only: unit result --------------------------------------------------------
  public method Label(text: string)
  public method Heading(text: string)
  public method Monospace(text: string)
  public method ProgressBar(fraction: float, text: string := "")
  public method Separator()
  public method AddSpace(amount: float)

  // ---- state-writing widgets: unit result; `out response` overload when the edge or rect is wanted
  public method Checkbox(text: string, ref checked: bool)
  public method Checkbox(text: string, ref checked: bool, out response: Response)
  public method Toggle(text: string, ref on: bool)                                    // (+ out overload)
  public method RadioValue<T>(text: string, ref current: T, value: T) where T: hashable   // current := value on click (+ out)
  public method Slider(text: string, ref value: float, min: float, max: float)         // (+ out)
  public method Slider(text: string, ref value: int, min: int, max: int)               // (+ out)
  public method DragValue(text: string, ref value: float, speed: float := 1.0f)        // (+ out)
  public method DragValue(text: string, ref value: int, speed: float := 1.0f)          // (+ out)
  public method TextEdit(ref text: string, hint: string := "")                         // single line (+ out)
  public method TextEdit(ref text: string, out response: Response, hint: string := "")
  public method TextEditMultiline(ref text: string, rows: int := 4)                    // (+ out)
  public method PasswordEdit(ref text: string)                                         // (+ out)
  public method ComboBox(text: string, ref selected: int, options: IReadOnlyList<string>)   // (+ out)
  public method ColorEdit(text: string, ref color: Color)                              // (+ out)

  // ---- containers: nesting closures, all call-bounded; unit result -----------------------
  public method Window(title: string, body: (Ui) -> ())                        // movable, resizable; state in memory
  public method Window(title: string, ref open: bool, body: (Ui) -> ())        // with a close button
  public method Panel(title: string, body: (Ui) -> ())                         // framed, fixed
  public method CollapsingHeader(text: string, body: (Ui) -> ())               // open state in memory
  public method CollapsingHeader(text: string, ref open: bool, body: (Ui) -> ())   // open state in the app
  public method ScrollArea(body: (Ui) -> ())                                   // scroll offset in memory
  public method Grid(columns: int, body: (Ui) -> ())                           // children flow into cells
  public method Table(columns: IReadOnlyList<string>, body: (TableBody) -> ()) // headers + rows (§4.6)
  public method MenuBar(body: (Ui) -> ())
  public method Menu(text: string, body: (Ui) -> ())                           // open state in memory
  public method Tabs(labels: IReadOnlyList<string>, ref selected: int, body: (Ui, int) -> ())
  public method Modal(title: string, ref open: bool, body: (Ui) -> ())         // blocks input beneath
  public method Popup(id: string, anchor: Response, body: (Ui) -> ())          // opened by OpenPopup
  public method OpenPopup(id: string)
  public method Tooltip(target: Response, text: string)                        // shown while target.Hovered
  public method TooltipUi(target: Response, body: (Ui) -> ())
  public method RequestFocus(target: Response)
```

Design notes, each a recorded choice:

1. **`ref` for every bound value; `out response` for the edge.** `ui.Checkbox("Dark", ref dark);` is the whole write-back story on the frames nobody cares about the edge, and `ui.Checkbox("Dark", ref dark, out var r); if r.Changed then Restyle(); end;` on the ones somebody does. Two alternatives were considered and declined for the sketch. Returning the new value (`dark := ui.Checkbox("Dark", dark);`) forces an assignment statement per widget (assignments are not expressions, §17.2), loses the edge, and reads as if the widget owned the value. Returning `Response` unconditionally — egui's shape — makes the *common* line warn under §21.9 and pushes `_ := ui.Checkbox(…)` onto every slider and checkbox; an `out` parameter is the language's spelling for a second result the caller may decline to bind (`out _`), so the sketch uses it. The price is that egui's one-liner `if ui.checkbox(..).changed()` becomes two lines.
2. **Overloads by `ref` type** (`Slider` over `ref float` and `ref int`) are ordinary overloads: `ref int` and `ref float` are distinct parameter types (§22.6). A generic `Slider<T> where T: INumber<T>` was considered; `INumber<T>` (stdlib §2.19) has `+ - * /` and `<` but no float conversion, so the mapping to pixels needs a per-type helper anyway. Two overloads are honest.
3. **Two spellings for open/closed state** (`CollapsingHeader` with and without `ref open`): memory-held state is the immediate-mode convenience; app-held state is what a program that saves layouts needs. Both exist in egui for the same reason.
4. **`Popup` takes its anchor `Response`** rather than reading "the last item". The sketch has no last-item register on `Ui` at all: every relation between two calls is a value passed between them. That is the explicit-over-implicit ethos applied one step further than ImGui goes.
5. **`Tabs` and `Table` take a body with an extra parameter** (`(Ui, int) -> ()`; `(TableBody) -> ()`): function types with several parameters are ordinary (§27.1), and a lambda under that target type infers its parameter types (§27.4).
6. **`Shortcut` consumes the key press** (returns `true` once per press) — the ImGui shape; egui's `input.consume_key` is the same idea. Without consumption two widgets would both react.

### 4.4 Painting: `Painter`

Custom widgets and debug overlays paint through a small borrow-bearing record over the frame's draw list — the same shape as `Span<T>` over its buffer (§9.7): constructed from a named source, never stored in a class field, never crossing `await`.

```bezel
public record Painter(Target: DrawList, Clip: Rect)          // borrow-bearing: Target is class-typed
  public method FillRect(r: Rect, fill: Color, rounding: float := 0.0f)
    => Target.Add(DrawCommand.FillRect(r.Intersect(Clip), fill, rounding));
  public method StrokeRect(r: Rect, color: Color, thickness: float := 1.0f)
    => Target.Add(DrawCommand.StrokeRect(r, color, thickness));
  public method Line(from: Vec2, to: Vec2, color: Color, thickness: float := 1.0f)
    => Target.Add(DrawCommand.Line(from, to, color, thickness));
  public method Circle(center: Vec2, radius: float, fill: Color)
    => Target.Add(DrawCommand.Circle(center, radius, fill));
  public method Text(origin: Vec2, text: string, color: Color, size: float)
    => Target.Add(DrawCommand.Text(origin, text, color, size));
  public method Clipped(clip: Rect, body: (Painter) -> ())
    Target.Add(DrawCommand.PushClip(clip));
    body(new Painter(Target, clip.Intersect(Clip)));
    Target.Add(DrawCommand.PopClip);
  end Clipped;
end Painter;

  // on Ui:
  public method Paint(): Painter => new Painter(_ctx.Output, _rect);
```

A record method may not store into its own receiver (§9.4), and none of these does — every write hops through the class-typed `Target` field, which is exactly the sanctioned mediating shape (§9.4: "a chain that hops through a class- or interface-typed member, leaving the receiver"). `Paint()` is a *method* rather than a property only for symmetry with the other producers; a property would be admitted too, since the value is a record built from a field-sourced borrow (§28.6 forbids fresh **owned class** values from getters, not records). The methods are named `FillRect`/`StrokeRect` rather than `Rect`/`Stroke` so that no member shares a name with the `Rect` type it takes.

### 4.5 Two widgets in full

Everything in §4.3 reduces to `Allocate` + `Interact` + `Paint` + memory. `Button` shows the fact-returning half; `Checkbox` shows write-back through `ref` and the `out response` overload with its unit-returning twin delegating to it; both are ~15 lines.

```bezel
  public method Button(text: string): Response
    var style := _ctx.Style;
    var textSize := _ctx.Fonts.Measure(text, style.FontSize);
    var rect := Allocate(textSize + new Vec2(style.Padding * 2.0f, style.Padding * 2.0f));
    var r := Interact(rect, _id.Child(text), Sense.Click);

    var fill := (if r.Pressed then style.WidgetActive elif r.Hovered then style.WidgetHovered else style.Widget);
    var p := Paint();
    p.FillRect(rect, fill, style.Rounding);
    p.Text(rect.Min + new Vec2(style.Padding, style.Padding), text, style.Text, style.FontSize);
    return r;
  end Button;

  public method Checkbox(text: string, ref checked: bool)
    Checkbox(text, ref checked, out _);                   // `out _` discards the response (§22.4)
  end Checkbox;

  public method Checkbox(text: string, ref checked: bool, out response: Response)
    var style := _ctx.Style;
    var box := style.FontSize;
    var textSize := _ctx.Fonts.Measure(text, style.FontSize);
    var rect := Allocate(new Vec2(box + style.Spacing + textSize.X, Math.Max(box, textSize.Y)));
    var r := Interact(rect, _id.Child(text), Sense.Click);
    if r.Clicked then
      checked := not checked;                       // the write-back: the caller's slot, during the call
      r := r with(Changed := true);                 // a record update, not a mutation (§9.5)
    end;
    var p := Paint();
    var boxRect := Rect.FromMinSize(rect.Min, new Vec2(box, box));
    p.FillRect(boxRect, (if r.Hovered then style.WidgetHovered else style.Widget), style.Rounding);
    if checked then p.FillRect(boxRect, style.Accent, style.Rounding); end;
    p.Text(rect.Min + new Vec2(box + style.Spacing, 0.0f), text, style.Text, style.FontSize);
    response := r;                                        // every normal-completion path assigns the out (§22.4)
  end Checkbox;
```

- `(if … elif … else …)` is the paren atom (§20); `else` is mandatory and the arms meet at one record type.
- `r := r with(Changed := true)` is the only way to "set a field" of a `Response` local: `Response` is positional, so `Changed` is a read-only property (§9.1, §9.4) and `r.Changed := true` is a compile error even on a local; `with` copies the storage with the override applied (§9.5), which is exactly the update idiom §9.4's diagnostic names.
- `checked := not checked;` through a `ref` parameter is the write-back the paradigm needs and nothing more: the widget never sees the slot after `Checkbox` returns (§22.3). `response := r;` discharges the `out` obligation on the single normal path (§22.4); the unit-returning twin forwards with `out _`, which is the sanctioned spelling for an ignored `out` (`out var _` is the rejected one, §22.4).
- `Interact` is where all the between-frame state lives: it compares `_ctx.Input.PointerPosition` against `rect`, consults `_ctx.Memory.Active` (the ImGui *active item* — the one that captured the press) and `_ctx.Memory.Focused`, and sets them on press/release. Its body is the same ~40 lines in every immediate-mode library and is elided.

### 4.6 The `Table` body handle — one non-`Ui` closure parameter

```bezel
public class TableBody(ui: Ui, columns: int)
  private _ui: Ui := ui;                                    // back-reference; lives for one closure call
  private _columns: int := columns;
  private _row: int := 0;

  /// One row; `cells` runs once per column with a cell Ui and the column index.
  public method Row(cells: (Ui, int) -> ())
    _ui.WithId(_row, lambda(rowUi) => do
      rowUi.Horizontal(lambda(h) => do
        for var c in 0..<_columns do
          h.Sized(new Vec2(ColumnWidth(c), rowUi.Style.FontSize + rowUi.Style.Padding), lambda(cell) => cells(cell, c));
        end;
      end);
    end);
    _row := _row + 1;
  end Row;

  private method ColumnWidth(c: int): float … end ColumnWidth;   // from memory: user-resizable
end TableBody;
```

Three closures deep, and every one is call-bounded — `cells`, the lambda handed to `WithId`, the one handed to `Horizontal`, the one handed to `Sized` — so the nesting costs nothing in ownership terms: each captures borrows (`cells`, `_columns` through `this`, the loop variable `c`, fresh per iteration under §27.5), none is stored, and each child `Ui` dies at its creator's return. `for var c in 0..<_columns` iterates a range (§16), the loop variable a copy.

## 5. Usage

### 5.1 The counter — the 0004 example, immediate-mode style

```bezel
module CounterApp;

public class AppState
  public property Count: int := 0;
end AppState;

public class CounterUi
  public static method Draw(ui: Ui, app: AppState)
    ui.Window("Counter", lambda(w) => do
      w.Label($"Count: {app.Count}");
      w.Horizontal(lambda(row) => do
        if row.Button("−").Clicked then app.Count := app.Count - 1; end;
        if row.Button("+").Clicked then app.Count := app.Count + 1; end;
        if row.Button("reset").Clicked then app.Count := 0; end;
      end);
    end);
  end Draw;
end CounterUi;
```

Compare 0004 §2: there, a button *carries a message* and `update` applies it; here, a button *returns a fact* and the app applies it on the spot. The MVU version's advantage is that `Msg` is sendable data and `update` is `pure`; this version's is that it is eleven lines with no second type. Both are legal Bezel for the same reason — closures capture `app` as a borrow (§27.5), and nothing retains them (§28.14).

### 5.2 The login form — the VCL exploration's example, re-done

exploration-ui-vcl-retained.md §3 builds a `LoginForm` with four owned components and an owned `Subscription`. Immediate mode has no components to own and no subscription to sever:

```bezel
module App;

public class LoginState
  public property User: string := "";
  public property Password: string := "";
  public property Remember: bool := false;
  public property Status: string := "";
end LoginState;

public class LoginUi
  public static method Draw(ui: Ui, s: LoginState)
    ui.Panel("Login", lambda(p) => do
      p.Grid(2, lambda(g) => do
        g.Label("User");      g.TextEdit(ref s.User, hint := "name@example.com");   // unit result: no §21.9 warning
        g.Label("Password");  g.PasswordEdit(ref s.Password);
      end);
      p.Checkbox("Remember me", ref s.Remember);
      p.Horizontal(lambda(row) => do
        var canSubmit := s.User.Length > 0 and s.Password.Length > 0;
        row.Disabled(not canSubmit, lambda(d) => do
          if d.Button("Sign in").Clicked or (canSubmit and d.Shortcut(Key.Enter, Modifiers.None)) then
            s.Status := $"Signing in {s.User}…";
          end;
        end);
        if row.Button("Clear").Clicked then s.User := ""; s.Password := ""; end;
      end);
      if s.Status.Length > 0 then p.Label(s.Status); end;
    end);
  end Draw;
end LoginUi;
```

Why each line is admitted:

- **`ref s.User`** names an auto-property of a class reached through a plain borrow parameter. A `ref` argument must be an assignable location that is not owned state, not a `using var`, not a `readonly` field outside its context (§22.3) — a settable auto-property on a class instance is one. (Had `LoginState` been a *record*, `ref s.User` inside one of *its own* methods would be a store into the receiver and rejected by §9.4; from outside, through the named binding `s`, it is the "external store through a named binding" §9.4 explicitly keeps legal.)
- **`s` inside four nested lambdas** is a captured borrow; a captured borrow inside a closure is "borrowed (source: same as the captured local's source)" (§28.3's classification table), and the closures go only to call-bounded parameters, so the source — the caller's `LoginState`, alive for the whole `Draw` call — is never outlived.
- **`canSubmit`** is a local of the `Horizontal` closure captured by the `Disabled` closure: an ordinary shared-cell capture (§27.5).
- **`hint := "…"`** is a named argument for a defaulted parameter (§22.5, v0.290 spelling).
- There is nothing to dispose, nothing to unsubscribe, no `_okSub` ordering trick, and no constructor. The retained version needed §28.14 form 3 to make the handler's `this`-capture safe; this version has no handler.

### 5.3 A list with selection, loop-salted IDs, and deletion during iteration

```bezel
public class TaskState
  private owned _titles: Array<string> := new Array<string>();   // trivial-dispose; `owned` so the array object is ours (§28.6)
  public property Titles: Array<string> => _titles;
  public property Selected: int := -1;
  public property NewTitle: string := "";
end TaskState;

public class TaskUi
  public static method Draw(ui: Ui, t: TaskState)
    ui.Window("Tasks", lambda(w) => do
      w.Horizontal(lambda(row) => do
        row.TextEdit(ref t.NewTitle, out var entry, hint := "New task");   // the out overload: we want .Focused
        var submit := row.Button("Add").Clicked or (entry.Focused and row.Shortcut(Key.Enter, Modifiers.None));
        if submit and t.NewTitle.Length > 0 then
          t.Titles.Add(t.NewTitle);
          t.NewTitle := "";
        end;
      end);
      w.Separator();

      var toDelete := -1;                                    // defer structural mutation past the loop
      w.ScrollArea(lambda(list) => do
        for var i in 0..<t.Titles.Count do
          list.WithId(i, lambda(item) => do                 // salt: two identical titles must not share memory
            item.Horizontal(lambda(row) => do
              if row.Selectable(t.Titles[i], t.Selected = i).Clicked then t.Selected := i; end;
              var del := row.SmallButton("×");
              row.Tooltip(del, "Delete");
              if del.Clicked then toDelete := i; end;
            end);
          end);
        end;
      end);
      if toDelete >= 0 then
        t.Titles.RemoveAt(toDelete);
        if t.Selected = toDelete then t.Selected := -1; end;
      end;
    end);
  end Draw;
end TaskUi;
```

- **`toDelete`** is the standard immediate-mode idiom for mutating a collection you are iterating: record the intent, apply after the loop. Here the loop is a counted range over `0..<t.Titles.Count`, evaluated once at range construction (§16.5), so a `RemoveAt` inside it would leave the bound stale and `t.Titles[i]` would raise `IndexOutOfRangeException` at the tail; had the loop iterated `t.Titles` directly, §29.12 would raise `InvalidOperationException` at the next advance. Either way the honest spelling is to leave the loop first. The local is mutably captured (assigned inside a lambda), so it is a shared cell (§27.5) and the read after `ScrollArea` returns sees the closure's write.
- **`WithId(i, …)`** salts the scope, so the two `SmallButton("×")` calls in consecutive rows get `Root…Child(i).Child("×")` rather than one shared ID. Without it, hovering one would hover both — the ImGui `PushID(i)` bug in every tutorial.
- **`row.Tooltip(del, "Delete")`** is the explicit-anchor form: the tooltip is *for* that response, not for "the last item".
- **`t.Titles[i]`** is an indexer read at a `string` element — a shared reference, no obligation (§28.9).

### 5.4 Background work: results re-enter as sendable messages, polled per frame

The immediate-mode loop composes with 0004's message discipline without adopting its architecture: a worker domain posts sendable values on a channel and the frame drains the channel before drawing.

```bezel
import System.Threading;

public choice WorkerMsg
  Progress(fraction: float),
  Finished(summary: string),
  Failed(reason: string);
end WorkerMsg;                                  // value-like ⇒ sendable (§35.12)

public class JobState
  public property Fraction: float := 0.0f;
  public property Summary: string := "";
  public property Running: bool := false;
end JobState;

public class JobUi
  /// Drain everything the worker posted since last frame. Called before Draw, once per frame.
  public static method Pump(inbox: Channel<WorkerMsg>, j: JobState)
    while inbox.TryReceive(out var msg) do          // Try shape (a), `when true`-narrowed inside the loop
      case msg of
        Progress(var f) => do j.Fraction := f; end,
        Finished(var s) => do j.Summary := s; j.Running := false; end,
        Failed(var why) => do j.Summary := $"failed: {why}"; j.Running := false; end
      end case;                                    // exhaustive by case coverage (§35.6): no `_`
    end;
  end Pump;

  public static method Draw(ui: Ui, j: JobState, inbox: Channel<WorkerMsg>)
    ui.Window("Job", lambda(w) => do
      w.ProgressBar(j.Fraction, (if j.Running then "working…" else "idle"));
      w.Disabled(j.Running, lambda(d) => do
        if d.Button("Start").Clicked then
          j.Running := true;
          Task.FireAndForget(Domain.Spawn(Worker.Run, inbox.NewSender()));   // capture-free entry, one owned sendable argument
        end;
      end);
      if j.Summary.Length > 0 then w.Label(j.Summary); end;
    end);
  end Draw;
end JobUi;

public static class Worker
  public static method Run(owned sender: ChannelSender<WorkerMsg>)   // runs on its own domain
    using var tx := move sender;                 // adopt the owned parameter (§28.10)
    for var step in 1..10 do
      // …work…
      tx.Send(WorkerMsg.Progress(float(step) / 10.0f));
    end;
    tx.Send(WorkerMsg.Finished("done"));
  end Run;
end Worker;
```

- **`Domain.Spawn<A>(entry: (owned A) -> (), owned arg: A)`** (stdlib §6): the entry is a static method reference — capture-free, as §30.14 requires — and the one argument is the owned `ChannelSender<WorkerMsg>` that `inbox.NewSender()` mints (it is sendable; the receiving `Channel<T>` end stays with the app). `Task.FireAndForget` is the supervised not-awaiting spelling (§17.13); an app that wants to join its workers keeps the task in an owned field or a `TaskScope` instead.
- **`while inbox.TryReceive(out var msg)`** is Try shape (a); `msg` is narrowed non-null inside the loop by the normative `when true` postcondition (stdlib §1), so the `case` scrutinee needs no `!`. The choice is value-like, so the `case` is a peek, not an owning match.
- The worker's only door back is `Send`. Nothing in this shape lets it touch `j`, the context, or a widget: the context is not sendable, `j` is a borrow the closure holds, and the entry is capture-free. That is exploration-ui.md §0's headline delivered by the immediate-mode lane without new machinery.
- **`WantsRepaint`** is what the loop reads to keep redrawing while `Running` is true rather than sleeping for input; a message arriving on a channel does not by itself wake a platform blocked in `Pump` — open question 5.

### 5.5 Headless: driving frames in a test

Because both boundaries are data (§3), a test needs no platform: push events, run a frame, read responses or commands. (Proposal 0022's test framework — accepted, wave D3, not yet in the stdlib — is the eventual harness; the sketch uses `Debug.Assert`.)

```bezel
import System.Diagnostics;

public class FixedMetrics: IFontMetrics                     // 8×16 monospace; deterministic
  public method Measure(text: string, size: float): Vec2 => new Vec2(8.0f * float(text.Length), 16.0f);
  public readonly property LineHeight: float := 16.0f;
end FixedMetrics;

public class CheckboxTests
  public static method ClickTogglesAndReportsChanged()
    using var fonts := new FixedMetrics();
    using var ctx := new UiContext(fonts);
    ctx.Input.ScreenSize := new Vec2(200.0f, 100.0f);
    var dark := false;
    var changedOn := -1L;

    for var frame in 1..3 do
      if frame = 2 then
        ctx.Input.PointerPosition := new Vec2(8.0f, 8.0f);              // inside the first widget
        ctx.Input.Push(InputEvent.PointerPressed(PointerButton.Primary, new Vec2(8.0f, 8.0f)));
        ctx.Input.Push(InputEvent.PointerReleased(PointerButton.Primary, new Vec2(8.0f, 8.0f)));
      end;
      ctx.BeginFrame();
      ctx.Root.Checkbox("Dark", ref dark, out var r);
      if r.Changed then changedOn := ctx.FrameNumber; end;
      ctx.EndFrame();
    end;

    Debug.Assert(dark, "click toggled the bound local");
    Debug.Assert(changedOn = 2L, "Changed reported on the click frame only");
    Debug.Assert(ctx.Output.Commands.Count > 0, "the frame painted something");
  end ClickTogglesAndReportsChanged;
end CheckboxTests;
```

`ref dark` binds a plain local (§22.3); `out var r` binds a plain `Response` local against a plain `out` (§22.4); `changedOn` is assigned in straight-line code, not inside a lambda, so it is not even captured. The press-and-release in one frame is the microui convention; egui and ImGui also accept it. A draw-list golden test is `ctx.Output.Commands` compared value-by-value — every `DrawCommand` case is auto-hashable (§35.7), so `=` works on them.

## 6. Widget memory without `Any`

Everything a widget must remember between frames — which window is where, the scroll offset, whether a header is open, the text cursor, which ID is *active* — lives in `UiMemory`, keyed by `WidgetId`. egui keeps this in one type-erased `IdTypeMap`; Dear ImGui in an `ImGuiStorage` of `int`/`float`/`void*`. Bezel has no `Any` and no `void*`, and that is not a loss: the set of state *kinds* a widget library needs is closed and small, so the memory is a **closed set of typed tables**, one per kind, each swept by the same touched-set rule.

```bezel
public class WindowState                                 // class: mutable, owned by the table
  public property Rect: Rect;
  public property Collapsed: bool := false;
  public property ZOrder: int := 0;
end WindowState;

public class TextEditState
  public property Cursor: int := 0;
  public property SelectionAnchor: int := -1;
  private owned _undo: Array<string> := new Array<string>();   // a fresh value needs an owned slot even at a trivial-dispose type (§28.6)
  public property Undo: Array<string> => _undo;                  // a borrow of storage
end TextEditState;

public class UiMemory
  // value-typed kinds: borrowing dictionaries over values
  private owned _flags:   Dictionary<WidgetId, bool>  := new Dictionary<WidgetId, bool>();
  private owned _scalars: Dictionary<WidgetId, float> := new Dictionary<WidgetId, float>();
  private owned _offsets: Dictionary<WidgetId, Vec2>  := new Dictionary<WidgetId, Vec2>();
  // class-typed kinds: owning dictionaries — Remove/Clear DISPOSE (§28.11)
  private owned _windows: OwningDictionary<WidgetId, WindowState>   := new OwningDictionary<WidgetId, WindowState>();
  private owned _edits:   OwningDictionary<WidgetId, TextEditState> := new OwningDictionary<WidgetId, TextEditState>();
  // frame bookkeeping
  private owned _touched: Set<WidgetId> := new Set<WidgetId>();
  private owned _stale:   Array<WidgetId> := new Array<WidgetId>();

  public property Active:  WidgetId? := null;          // the pointer-capturing widget (ImGui's ActiveId)
  public property Focused: WidgetId? := null;          // keyboard focus
  public property Hot:     WidgetId? := null;          // hovered, resolved at EndFrame for next frame

  public method Flag(id: WidgetId, fallback: bool): bool
    _ := _touched.TryAdd(id);
    return _flags.GetOrNull(id) ?? fallback;
  end Flag;
  public method SetFlag(id: WidgetId, value: bool) => _flags[id] := value;   // indexer set: add-or-update

  public method Scalar(id: WidgetId, fallback: float): float … end Scalar;
  public method SetScalar(id: WidgetId, value: float) … end SetScalar;
  public method Offset(id: WidgetId, fallback: Vec2): Vec2 … end Offset;
  public method SetOffset(id: WidgetId, value: Vec2) … end SetOffset;

  /// Borrow the window state for `id`, creating it on first sight. The dictionary owns it.
  public method Window(id: WidgetId, initial: Rect): WindowState
    _ := _touched.TryAdd(id);
    return _windows.GetOrAdd(id, lambda(_) => do
      var w := new WindowState();
      w.Rect := initial;
      return w;
    end);
  end Window;
  public method Edit(id: WidgetId): TextEditState
    _ := _touched.TryAdd(id);
    return _edits.GetOrAdd(id, lambda(_) => new TextEditState());
  end Edit;

  public method Touch(id: WidgetId)                     // for custom widgets keeping state elsewhere
    _ := _touched.TryAdd(id);
  end Touch;
  public method BeginFrame() => _touched.Clear();

  /// Forget every id no widget touched this frame — egui's and ImGui's garbage rule.
  public method Sweep()
    _stale.Clear();
    for var e in _flags do if not _touched.Contains(e.Key) then _stale.Add(e.Key); end; end;
    for var k in _stale do _ := _flags.Remove(k); end;
    // …the same two lines per table; for the owning tables Remove disposes the state (§28.11)
    var active := Active;                      // copy: properties never narrow (§7.4)
    if active <> null and not _touched.Contains(active) then Active := null; end;
    var focused := Focused;
    if focused <> null and not _touched.Contains(focused) then Focused := null; end;
  end Sweep;
end UiMemory;
```

- **`OwningDictionary<K, V>.GetOrAdd(key, factory: (K) -> owned V): V` returns a borrow** (stdlib §4.9): the table owns the state, the widget uses it for the call. The factory lambda returns a fresh owned class value, which is what `-> owned V` announces. `WindowState`/`TextEditState` are trivial-dispose, so the owning tables cost nothing at teardown beyond freeing storage.
- **`Sweep` collects, then removes**: `for var e in _flags` yields `DictionaryEntry<K,V>` and structural mutation during iteration is the same `InvalidOperationException` class as the array's; the two-phase shape is forced and honest. On the owning tables `Remove` disposes (§28.11) — stale text-edit state is freed by the language's cascade, not by a library destructor. Neither ImGui nor egui can say that their memory GC cannot leak.
- **Custom widget state does not go in here.** An app-defined widget with its own state kind keeps it in the *app's* state object (which is where immediate mode says state lives anyway) — a `Dictionary<WidgetId, MyState>` the app owns, swept by the app with `ctx.Memory.Touch(id)` and its own untouched-set pass; or simply a field, when the widget appears once. A generic `MemoryTable<T>` owned by the app and registered with the context for sweeping is a small design pass (open question 2), not a blocker.
- **`Active`/`Focused` are nullable record auto-properties.** `WidgetId?` is a nullable value; `Sweep` copies each to a local before testing, because a property never narrows (§7.4) and the copy does.

## 7. The platform seam — and why this lane needs no callback inversion

The library above is backend-agnostic: it consumes `FrameInput` and produces `DrawList`. The platform layer is the only foreign code, behind one interface:

```bezel
public interface IUiPlatform
  readonly property Fonts: IFontMetrics;
  /// Fill `input` with everything since the last call and return false once the OS asked to quit.
  method Pump(input: FrameInput): bool;
  /// Rasterize and present. May block for vsync.
  method Present(commands: DrawList);
end IUiPlatform;
```

Two implementations are worth naming, in the order they should exist:

1. **A software platform** (`SoftwarePlatform` in §1): a scanline rasterizer over an `OwningBuffer<byte>` framebuffer with a fixed bitmap font, blitting through a minimal windowing extern (or writing PNGs, for the reconciler-development role 0004 open question 4 assigns it). Zero dependencies; the whole library is testable on it; `Stopwatch` (`System.Diagnostics`) supplies `Time`/`DeltaTime`. This is the "self-contained, testable" backend the exploration doc leaned toward.
2. **An SDL-style platform** over a flat-C library via `extern` (§32): `SDL_PollEvent` is a **non-blocking poll** that the app calls on its own domain every frame, so — unlike the retained lane, whose toolkit owns a blocking main loop and calls back into Bezel through §32.8's registry idiom — the immediate-mode lane needs **no callback inversion at all** for input, and no §32.10 dedicated-domain extern. The whole binding is a dozen plain functions with `IntPtr` handles and C-compatible records (§32.7, §32.13). Text input arrives as SDL's already-IME-composed `TEXTINPUT` events, which is why `InputEvent.TextEntered` carries a `string` and not a key. (Adopting SDL is a project-level dependency decision — CLAUDE.md — and is not proposed here.)

The single design decision to record when this becomes real is the same one exploration-ui-toolkit-binding.md §1 records for the retained lane: the platform owns the native window and the framebuffer; Bezel owns everything else. There is no second owner of anything, so the single-owner rule is trivially discharged.

## 8. Where this sits among the three lanes

| | Retained (VCL-style) | MVU (0004) | Immediate mode (this doc) |
|---|---|---|---|
| Who owns the loop | the toolkit; Bezel is called back | the `Ui.Run` domain | **the application** |
| Where app state lives | in widgets (manual sync) | in the `Model` record | **in the app; lent per frame** |
| What the library retains of yours | subscriptions (owned `Subscription`) | nothing (messages are values) | **nothing — a type-system fact (§28.14)** |
| Reactive edge | events | `Msg` → `update` | **a `Response` value per call (or `out`)** |
| Sendability story | widgets confined by `_parent` (rule 3) | `Msg: sendable` enforced | **context non-sendable; results re-enter via `Channel<T>`** |
| Designer / WYSIWYG | yes (the reason the lane exists) | no (a function has no file) | no |
| Native look, accessibility, IME depth | yes | via its backend | **no** (the known costs) |
| Best for | forms-heavy tools with a designer | state-rich apps | **tools, debug overlays, editors, games, and 0004's first backend** |

Three relationships worth stating:

- **0004 open question 4 is answered in the affirmative by construction.** The MVU reconciler needs a retained backend to patch; an immediate-mode renderer is a *stateless* backend — the reconciler's "apply patches" step becomes "call the view function's node tree through `Ui` every frame", with no patching at all and the diff reduced to §9.2 equality on the previous tree for repaint suppression. That is exactly how iced's `renderer` and egui's `egui_dock` layer sit under Elm-shaped code. The two lanes share `DrawCommand`, `FrameInput`, `IFontMetrics`, and the platform seam wholesale.
- **The VCL lane and this one do not compete**; they compose the way ImGui composes with any host: a retained window hosting an immediate-mode canvas control (the inspector, the plot, the debug overlay) is the common real-world shape.
- **Nothing here needs a language change.** Every line stands on §22.3 `ref`, §27.5 closures, §28.14's call-bounded default, §28.3 back-references, §9 records, §35 choices and the shipped collections. The keep-in-mind list is short: (1) the call-bounded default must stay the default — a "convenience" that let a callee retain an unmarked closure would silently break the paradigm's invariant; (2) `ref` on lambda parameters must stay admitted (it is what lets a custom widget take `ref` state inside a nesting closure); (3) records must stay cheap to construct per frame — the sketch allocates several per widget per frame and relies on them being stack values.

## 9. What the paradigm costs, honestly

1. **Continuous or on-demand redraw.** A frame is the unit of everything; a text field's caret blink needs frames. `WantsRepaint` plus a platform that sleeps for input otherwise is the standard mitigation (egui's `request_repaint`), and it is one `bool` on the context.
2. **One-frame lag.** A widget's size this frame depends on last frame's content (a window auto-sizing to its children). Every immediate-mode library accepts one frame of imprecision or runs two passes; the sketch accepts it, as egui does, and `MinSize` is measured after the child closure returns so that *parents* at least are exact.
3. **Text editing is where the depth is.** Cursor, selection, undo, IME composition, clipboard, bidi. The sketch's `TextEditState` is the minimum; a real one is the biggest single component in every immediate-mode library, and Bezel's immutable `string` means every edit builds a new value (`StringBuilder` → `ToString()`, stdlib §3) — fine at UI text sizes, and the `ref text: string` contract means the app never sees a half-edited buffer.
4. **No exclusivity on borrows.** The `_suspended` contract (§4.2) is a runtime backstop for a mistake egui rejects at compile time. It is deterministic and named, but it is a runtime check.
5. **A closed memory vocabulary.** New state kinds mean a new table in `UiMemory` (library-side) or app-owned storage (user-side). This is the explicit spelling of what `Any` hides; it is a real cost for third-party widget libraries and the subject of open question 2.

## 10. Open questions (deliberately few)

1. **`Response` as a plain record vs. a borrow-bearing one.** The sketch keeps `Response` value-only and passes it back to `Ui` for tooltips and popups (`ui.Tooltip(r, …)`). egui's `response.on_hover_text(…)` fluent form needs the response to carry a borrow of its `Ui`, making it borrow-bearing (§9.7) — still legal (never stored, never awaited), but it stops being freely storable in the app's own state. Which reads better in practice is a usage question, not a soundness one.
2. **App-extensible memory.** A `MemoryTable<T> where T: class` owned by the app and registered with `UiMemory` for the touched-set sweep — the registration would store a parameter-sourced borrow into the context (§28.3 admits it; K4.3 residual: the app's table must outlive the context, which the declaration order in §1's loop guarantees). Needs a design pass on the registration surface before it can be sketched honestly.
3. **Images and custom geometry.** `DrawCommand` has no image or mesh case. Adding `Image(rect, textureId: int, uv: Rect)` makes texture ownership a platform concern (the platform mints the id from an `Array<byte>`); a mesh case needs a vertex buffer, which is `OwningBuffer<float>` territory and drags the command list away from "plain sendable values".
4. **Stable IDs across runs.** In-process identity is all the sketch needs. Persisting layout (window positions) requires a hash that is stable across builds; `string.GetHashCode()` is unspecified beyond "content-based" (stdlib §9.6) and §8.3 leaves the record combiner implementation-defined. A library-owned stable string hash needs wrapping multiplication, which §6.1's checked arithmetic does not provide — proposal 0008's integer toolkit (accepted 2026-07-07, in its implementation run, not in the v0.306 stdlib) is the place to look; until it ships, a stable hash is a shift-and-xor construction (shifts discard bits silently within their domain, §6.5).
5. **Waking a sleeping loop.** A platform that blocks in `Pump` waiting for OS input will not notice a worker's `Channel` message until the next OS event. The fix is platform-specific (SDL's `PushEvent` from the worker; or the worker sending to a platform-level wake channel) and belongs to the platform layer, not the library — but the contract ("a message arrival is a repaint request") should be written down when the platform exists.
6. **`float` vs `double` for geometry.** The sketch uses `float` (every immediate-mode library does; halves the size of the draw list). `Math.Round` takes `double` only (stdlib §2.2; `Floor`/`Ceiling`/`Truncate` add `decimal`, never `float`), so pixel snapping needs `float(Math.Round(x))` with the implicit `float`→`double` widening (§5.1). Cheap, but if it turns out to be everywhere, `double` geometry is the simpler spelling.

## 11. Relationship

exploration-ui.md (row 2's verdict, §0's headline, §3's keep-in-mind list — items 4 and 5 are load-bearing here too) · proposal 0004 (§2's counter, re-done in §5.1; open question 4, answered in §8) · exploration-ui-vcl-retained.md (§3's login form, re-done in §5.2; the `_suspended` backstop is the same K4.1-class residual as its containment-coherence invariant) · exploration-ui-toolkit-binding.md (§1's flat-C seam and single-owner rule, reused in §7 without the callback inversion) · proposal 0022 (the eventual harness for §5.5) · proposal 0008 (open question 4) · spec §22.3, §27.5, §28.3, §28.14, §30.14, §35; stdlib §4.9 (`OwningDictionary.GetOrAdd`), §6 (`Channel<T>`, `Domain.Spawn`).
