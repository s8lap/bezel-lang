# Proposal 0035 (RFC) — Demote the `event` member kind to the library pattern it abbreviates

| | |
|---|---|
| **Status** | draft (RFC) |
| **Tracking issue** | [#1550](https://github.com/s8lap/bezel-lang/issues/1550) |
| **Target module(s)** | Spec [§24](../Bezel_Language_Spec.md#sec-events) (member kind removed or defined by expansion), [§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 5, [§28.14](../Bezel_Language_Spec.md#sec-scoped-callback-parameters) form 4, [§2.3](../Bezel_Language_Spec.md#sec-always-available-names), Appendix A.4 (`EventDecl`, `IfaceEvent`), Appendix [D.2](../Bezel_Language_Spec.md#sec-extended-features)/[D.3](../Bezel_Language_Spec.md#sec-feature-dependencies); stdlib [§6.5](../Bezel_Standard_Library.md#sec-events-ievent-eventsource) (public constructors, relocated semantics, consequential rewording; plus one **proposed** type if option S1 of this RFC's §7 is chosen). No new module. |
| **Written against** | spec/stdlib **v0.180**. The underlying finding was adversarially verified against v0.179; the v0.180 capture train (#1439/#1421) landed while this RFC was in drafting and touches events substantially (§1.2), so **every citation below has been re-verified against v0.180** and the v0.180-added rules are engaged explicitly rather than footnoted. |
| **Design character** | **RFC** — an ergonomics-vs-uniformity trade, not a defect fix. Events are sound today. The recommendation is argued; the owner decides. Nothing here lands in the spec this run. |
| **Preconditions** | Public parameterless constructors on `EventSource<T>`/`AsyncEventSource<T>` (§2.3, a stdlib change this RFC carries) and source-level nameability of those types (a **compiler gap independent of this RFC** — spec §2.3 already requires it; filed as [#1536](https://github.com/s8lap/bezel-lang/issues/1536)). |

## 0. Summary

The `event` member kind is a one-line abbreviation for two lines of ordinary members: a private `owned` field of type `EventSource<T>` and a public expression-bodied property of type `IEvent<T>` borrowing it. Everything else about events — `Raise`/`RaiseAsync`, `Subscribe`/`Subscription`, snapshot-raise, first-error-wins handler exceptions, `IEvent<out T>` covariance, domain-locality — is *already* library semantics on the stdlib §6.5 types and survives unchanged. What the member kind uniquely adds to the language is its **two-faced member typing** ([§24.3](../Bezel_Language_Spec.md#sec-the-type-of-an-event-member)):

> An event member has two faces, distinguished by whether the access is from inside the declaring class or from outside.

This is the language's only rule under which the *static type* of a member access depends on where the access is written. It is not free: every ownership and capture rule that walks members must special-case it, and the v0.180 capture train — landed while this RFC was drafted — had to do exactly that, in three separate places (§1.2).

This proposal recommends **removing the `event` member kind** (option (b), §5): delete `EventDecl` and `IfaceEvent` from Appendix A.4, relocate §24's semantics into stdlib §6.5, and document the field+property pattern as the idiom. A fallback **defined-by-expansion sugar** (option (a), §6) is specified with an explicitly *named* backing field; the tempting "exact sugar" in which the bare event name resolves to the field inside the class and to the property outside is **rejected in advance** (§6.1) — it would relocate the access-site dependence from typing into name resolution rather than delete it, which is C#'s field-like-event model and the opposite of the point.

Two things must be settled *as design points, not footnotes*, for either option: static events, whose §22.12 rule-5 guarantees (synchronized subscriber list, v0.163; capture-free handlers, v0.180) are keyed on the member kind and cannot be conditioned on storage site by a library-only design (§7); and the §28.14 form-4 capture admission, which names event backing members explicitly (§8).

Two things this proposal is **not**:

- **Not a bug fix.** No recorded defect requires this. The finding is that the mechanism is *removable*, not that it is broken. The keyword is one line and auto-wires initialization, disposal, and interface satisfaction; the pattern is two to three lines and two names. That is a real ergonomic price, counted honestly in §9, and reasonable owners can decline to pay it.
- **Not a removal of events.** Every observable behavior of §24 — including the guides' and rationale's motivating use cases — remains expressible, most of it with identical call sites. What is removed is the *member kind*, not the capability.

## 1. The mechanism today (v0.180)

### 1.1 The keyword and its expansion-shaped semantics

An event declaration ([§24.1](../Bezel_Language_Spec.md#sec-declaration)):

```bezel
public event Clicked: ClickedEvent;
```

> The compiler generates a private `owned` backing field of type `EventSource<T>` (or `AsyncEventSource<T>`) for each event declaration. The backing field is initialized during construction and disposed when the containing object is disposed, following the standard owned-field rules ([§28.5](../Bezel_Language_Spec.md#sec-owned-fields)). For static events, the backing field is a static owned field with program-exit disposal ([§22.13](../Bezel_Language_Spec.md#sec-static-initialization-order)).

The member's type is access-site-dependent ([§24.3](../Bezel_Language_Spec.md#sec-the-type-of-an-event-member)): outside the declaring class it is `IEvent<T>` / `IAsyncEvent<T>` (subscribe-only, covariant); inside (and in derived classes for `protected` events) it is `EventSource<T>` / `AsyncEventSource<T>` (adds `Raise`/`RaiseAsync`, `Add`/`Remove`, and the `+=`/`-=` desugaring). §24.3 itself insists the enforcement is ordinary:

> The compiler enforces visibility: outside the declaring class, the member's static type is the interface (`IEvent<T>` or `IAsyncEvent<T>`), so neither `Add`/`Remove` nor `Raise`/`RaiseAsync` are in scope. External code subscribes through `Subscribe` (which is on the interface) and unsubscribes by disposing the returned `Subscription`. The split is type-driven, not a special access-control rule.

Everything downstream is typed against the two stdlib classes, not against the member kind. The `+=`/`-=` forms are ([§24.3](../Bezel_Language_Spec.md#sec-the-type-of-an-event-member)):

> a fixed, language-defined desugaring, available only when the static type of the left operand is `EventSource<T>` or `AsyncEventSource<T>`.

— i.e. already keyed on *type*, not on the member kind (this matters in §5.5). And stdlib [§6.5](../Bezel_Standard_Library.md#sec-events-ievent-eventsource) records that the producer-side methods are `public` precisely because the restriction is carried by types:

> The `Add`, `Remove`, and `Raise` methods are declared `public` rather than `internal` because the visibility restriction is enforced by the **type fence**: an `EventSource<T>` reference is obtainable only inside the declaring class (the field type from outside is the interface, not the class).

The project's own trajectory here is worth stating: v0.75 enforced the producer-side restriction with a compiler-magic visibility bypass; v0.76 deleted the magic in favor of the type fence (rationale, v0.75→v0.76 changelog: *"The new design is cleaner: pure type-system enforcement, no compiler magic for visibility."*). This RFC is the terminal point of that recorded trajectory — the one remaining piece of event-specific compiler mechanism is the two-faced member typing itself, plus the carve-outs it forces elsewhere.

### 1.2 The carve-outs it forces elsewhere (all live at v0.180)

The member kind is not one rule; it is one rule plus every other rule that must know about it. As of v0.180:

1. **Grammar**: two productions exist solely for it (Appendix A.4):
   ```
   EventDecl  ::= Visibility "static"? "async"? "event" Identifier (":" Type)? ";"
   IfaceEvent ::= "async"? "event" Identifier (":" Type)? ";"
   ```
2. **Member-kind placement rule** (v0.180, [§24.1](../Bezel_Language_Spec.md#sec-declaration)): events are admitted on "classes and interfaces only"; an `event` in a record or enum body is a compile error naming the rule — a rejection needed *because* the hidden backing field would violate [§9.7](../Bezel_Language_Spec.md#sec-records-and-disposal) ("Records cannot have destructors, `owned` fields, or implement `IDisposable`"). With a named field, §9.7 rejects the field directly and the event-specific rule has nothing left to say.
3. **The §28.14 capture machinery** (v0.180): the storage-bounded admission's bounding-object form 4 must enumerate event members as an honorary field kind — *"a call whose receiver is a member chain rooted at `this` or a local binding in which every hop is an **`owned` field or a compiler-generated event backing member** ([§24.3](../Bezel_Language_Spec.md#sec-the-type-of-an-event-member))"*. A named backing field is an `owned` field; the clause's second half exists only because the event member is not one.
4. **Statics** ([§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 5, v0.163, strengthened v0.180): a member-kind-keyed synchronization and capture-freedom rule for `static event` — the load-bearing design point, taken up in §7.
5. **Modifier interaction prose** (§24.1): "Events do not support `virtual`, `override`, `abstract`, `sealed`, or `readonly`" — a rule ordinary fields and properties get from their own grammars.

Each of these is small. Their sum is the actual cost of the member kind: every future rule that walks members inherits the obligation to decide what it means for events. The v0.180 train is the concrete demonstration — it is the newest revision, and it had to touch events in items 2, 3, and 4 above plus the `-=` matching rule and the raising-domain rule, for a feature whose semantics are otherwise entirely in the stdlib.

### 1.3 Probe results (main @ bf9b28d, 2026-07-08)

The keyword form compiles and runs (subscribe + two raises observed). The manual pattern:

```bezel
import System.Threading;
private owned Ticked_source: EventSource<TickEvent> := new EventSource<TickEvent>();
public property Ticked: IEvent<TickEvent> => Ticked_source;
```

fails today with `unresolved type name EventSource` — the compiler does not resolve the name at all, even though spec [§2.3](../Bezel_Language_Spec.md#sec-always-available-names) states an explicit import "is required for `CancellationTokenSource`, `Mutex`, `EventSource<T>`, `AsyncEventSource<T>`, and the other threading types", i.e. per spec the names are importable. That is a compiler gap **independent of this RFC** — §24.3 gives the member the static type `EventSource<T>` inside the declaring class, so a spec-legal helper method taking that type is unwritable today — and is filed separately as [#1536](https://github.com/s8lap/bezel-lang/issues/1536). Even with names resolving, the pattern is spec-blocked at construction: stdlib §6.5 declares `// No public constructor; obtained via event declaration.` Hence the precondition in §2.3 below.

## 2. The finding, and its preconditions

### 2.1 The pattern is spec-legal except for the constructor

Each piece of the manual pattern is admitted by an existing, general rule:

- The field: `private owned Ticked_source: EventSource<TickEvent> := new EventSource<TickEvent>();` — an ordinary owned field with a fresh-owned initializer, initialized during construction and disposed with the object ([§28.5](../Bezel_Language_Spec.md#sec-owned-fields)) — the *same* rules §24.1 already invokes for the generated field ("following the standard owned-field rules").
- The property: `public property Ticked: IEvent<TickEvent> => Ticked_source;` — a bare read of an owned field is a receiver-sourced **borrow** through a non-owned position ([§28.6](../Bezel_Language_Spec.md#sec-leak-prevention) bullet 1, v0.151); the getter prohibition covers only *freshly-produced* owned values, not field reads. The upcast to the implemented interface is routine, and the caller gets exactly the outside face §24.3 prescribes: an `IEvent<T>` exposing only `Subscribe`.
- The subscription lifetime: `using var sub := c.Ticked.Subscribe(h);` — unchanged; `Subscribe` is on the interface and `scoped` (v0.175), returning an owned `Subscription`.
- The raise: `Ticked_source.Raise(new TickEvent(n));` — a method call on an owned field.

The type fence survives with the same strength for the same reason: an `EventSource<T>` for a *given* event is obtainable outside its class only if the class leaks it, exactly as stdlib §6.5 already discusses for smuggled references. Field privacy, not constructor privacy, is what fences a given event — constructor privacy only prevents *standalone* creation, which threatens nothing of yours (a stranger constructing their own `EventSource<T>` can raise only their own event).

### 2.2 What the member kind uniquely contributes

Subtracting §2.1 from §24 leaves: the one-line spelling; the two-faced typing (§24.3); the member-kind-keyed rules of §1.2; and interface-event auto-satisfaction (§5.4). The two-faced typing is the language's only access-site-dependent member *type* rule — accessor visibility ([§13.6](../Bezel_Language_Spec.md#sec-accessor-visibility)) and `protected` change where an access is *legal*, never what type it *has*. Bezel is otherwise strict that a name's static type is a property of the declaration, not of the reader.

### 2.3 Precondition: public parameterless constructors, nameable types

This RFC carries, as its enabling stdlib change, replacing the `// No public constructor; obtained via event declaration.` comment in stdlib §6.5 with:

```bezel
public class EventSource<T>: IEvent<T>
  public constructor();
  // ... unchanged surface ...
end EventSource;

public class AsyncEventSource<T>: IAsyncEvent<T>
  public constructor();
  // ... unchanged surface ...
end AsyncEventSource;
```

plus the compiler actually resolving the names under `import System.Threading` (#1536 — a conformance gap regardless of this RFC's fate). A standalone benefit falls out even under the status quo: event-relay and event-bus shapes (a source owned by a struct-of-sources, a source per dictionary slot) become expressible without a wrapper publisher class per event.

A demotion recommendation without this precondition would be vacuous — the probe (§1.3) shows the manual pattern does not exist today. The precondition is deliberately severable: the owner can accept §2.3 alone (making the pattern *possible*) while declining the demotion (keeping the keyword), and the language would be strictly more uniform than today at zero migration cost. That severed outcome is this RFC's recommended floor.

## 3. Recorded decisions engaged

Per the house rule, the recorded declines and rulings adjacent to this change, and why none pre-decides it:

- **Rationale [§13.1](../Bezel_Design_Rationale.md#sec-raise-instead-of-invoke)** (Raise instead of `?.Invoke`): defends `Raise` against C#'s null-delegate idiom and records the decline of direct invocation `event(args)` — *"It's cleaner at the call site but conflicts with the asymmetric inside/outside types."* Both the winner and the decline are **library-level** (`Raise` is a method on `EventSource<T>`) and survive demotion verbatim. Notably, §13.1's own summary of the asymmetry — *"enforced by the static type, not by a special access rule"* — is an argument the field+property pattern satisfies *more* literally than the member kind does: with a named field the two types belong to two declarations, and no member changes type by viewpoint.
- **Rationale §13.2** (payload types, not handler types), **§13.3** (method-reference subscriptions), **§13.5** (first error wins): all library-level; unchanged.
- **Rationale [§13.4](../Bezel_Design_Rationale.md#sec-events-on-interfaces)** (events on interfaces, v0.62): motivates interface events and prices them — *"The cost of supporting the feature is one new grammar production (`IfaceEvent` in Appendix A.4) and a paragraph of semantics."* §5.4 shows the same contracts expressible as interface properties, at which point v0.62's cost can be refunded. The *use cases* §13.4 records are fully served either way.
- **v0.76 type fence** (rationale changelog): removed event compiler magic in favor of type enforcement — the trajectory this RFC completes (§1.1).
- **v0.163 statics-and-domains (#1363), strengthened v0.180**: instance events deliberately **unsynchronized** ("Instance events remain unsynchronized and capture-admitting — domain-serial per [§24.6](../Bezel_Language_Spec.md#sec-thread-safety), captures per §28.14"), static events synchronized and (v0.180) capture-free. A recorded choice this RFC must not reverse by accident — engaged head-on in §7.
- **No recorded decision weighs the member kind against the library pattern.** Rationale §13.1–§13.5 defend the design against in-kind alternatives (direct invocation, internal-visibility magic, handler-type declarations, aggregation); nothing in the changelogs through v0.180, in Appendix D's history, or in the issue tracker asks or pre-declines *this* question. Events' Core-vs-member-kind status, like 0036's subject, is a default, not a decision.

## 4. The recommendation in one paragraph

Adopt §2.3 (constructors + nameability) unconditionally. Then prefer **option (b)**: remove the `event` member kind and the `IfaceEvent` production, relocate §24's normative semantics into stdlib §6.5, and settle static events per §7 (recommended: S1). If the owner judges the per-event ergonomic price (§9) too high, adopt **option (a)** — the keyword survives as *defined-by-expansion* sugar with an explicitly named backing field (§6) — which keeps the one-line spelling while still deleting the two-faced typing and most of §1.2. What must **not** be adopted is the bare-name variant of (a) (§6.1) or the silent synchronization variant of §7 (S3).

## 5. Option (b) — remove the member kind (recommended)

### 5.1 The idiom

```bezel
module App;

import System.Threading;

public record ClickedEvent(Sender: Button, X: int, Y: int);

public class Button
  private owned Clicked_source: EventSource<ClickedEvent> := new EventSource<ClickedEvent>();
  public property Clicked: IEvent<ClickedEvent> => Clicked_source;

  private method OnPress(x: int, y: int)
    Clicked_source.Raise(new ClickedEvent(this, x, y));
  end OnPress;
end Button;

// Subscription sites are UNCHANGED:
using var sub := button.Clicked.Subscribe((e) => UpdateStatus(e.X, e.Y));
```

A `private` event needs no property at all — one line, same as the keyword. A `protected` event declares the field `protected` (derived classes raise through it, replacing §24.3's derived-class face). Async events use `AsyncEventSource<T>` / `IAsyncEvent<T>` identically. No-payload events use `EventSource<()>` and its `Raise()` convenience overload (stdlib §6.5, unchanged).

### 5.2 What is deleted, and what dissolves

Deleted from the spec: §24.1/§24.3's member kind and two-faced typing; `EventDecl` and `IfaceEvent` (Appendix A.4); §24.1's modifier-interaction prose. Dissolved into rules that already exist:

| Event-specific rule (v0.180) | Becomes |
|---|---|
| Generated backing field init/disposal (§24.1) | §28.5 owned-field rules on a visible field — the same rules, applied to a declaration the reader can see |
| Events on classes/interfaces only (§24.1, v0.180) | §9.7: records/enums cannot have `owned` fields — the field is rejected directly, no event rule needed |
| §28.14 form 4's "or a compiler-generated event backing member" clause | plain "`owned` field" — the clause's reason for existing disappears |
| Static backing-field placement + exit disposal (§24.1) | §22.12/§22.13 static-field rules on a visible static field (see §7 for rule 5) |
| Outside face = `IEvent<T>` (§24.3) | the property's declared type |
| Inside face = `EventSource<T>` (§24.3) | the field's declared type |
| Derived-class face for `protected` events (§24.3) | `protected` field visibility |

### 5.3 Where §24's semantics go

§24's genuinely *semantic* content is already stated against the types and moves to stdlib §6.5 with its normative force intact (this is spec→stdlib relocation, not weakening — the stdlib is one of the five normative documents): `Raise`/`RaiseAsync` ordering and the v0.163 snapshot rule; first-error-wins handler exceptions (§24.5, rationale §13.5); zero-subscriber no-op; `Subscribe`/`Subscription` mechanics (§24.4); thread-safety text (§24.6 — instance sources domain-serial, see §7); the v0.180 raising-domain rule; `IEvent<out T>` covariance (§12.9 keeps the worked example, retargeted at the stdlib declaration); payload-record guidance (§24.2 — becomes stdlib/guide prose). The v0.180 quiescence sentence ("the publisher's disposal severs the subscriber list before reclamation") is already stated in stdlib §6.5 against the type.

### 5.4 Interface events become interface properties

`IfaceEvent` is deleted; the same contract is an ordinary get-only interface property:

```bezel
public record PropertyChangedEvent(Sender: Object, PropertyName: string);

public interface INotifyPropertyChanged
  readonly property PropertyChanged: IEvent<PropertyChangedEvent>;
end INotifyPropertyChanged;

public class Person: INotifyPropertyChanged
  private owned PropertyChanged_source: EventSource<PropertyChangedEvent>
      := new EventSource<PropertyChangedEvent>();
  public property PropertyChanged: IEvent<PropertyChangedEvent> => PropertyChanged_source;
  // ...raise sites use PropertyChanged_source.Raise(...)
end Person;
```

External consumption is byte-for-byte what §24.7 shows today (`p.PropertyChanged.Subscribe(...)`). What is lost is auto-satisfaction — under §24.7 the single `event` declaration both satisfies the interface and provides the raise capability; here the implementer writes the property explicitly (counted in §9). What is gained: rationale §13.4's "one new grammar production and a paragraph of semantics" is refunded, and covariant exposure (`EventSource<Dog>` behind a `readonly property: IEvent<Animal>`) becomes directly declarable rather than a consequence of assignment contexts.

### 5.5 The `+=`/`-=` desugaring: keep it (sub-question Q3)

The desugaring is *already* type-keyed, not member-keyed (§1.1), so it survives option (b) mechanically unchanged: inside a class, `Clicked_source += OnButtonClicked` desugars to `.Add(...)` exactly as the bare event name does today, the v0.180 `Add`-is-`scoped` analysis applies to the desugared call unchanged, and the v0.180 function-value-equality `-=` matching ([§27.7.1](../Bezel_Language_Spec.md#sec-equality-of-function-values)) is untouched. Recommendation: **keep it** — it is access-site-independent, costs one paragraph (already written), and deleting it would churn every subscription call site for no uniformity gain; the mechanism this RFC deletes is the two-faced member typing, not a type-keyed convenience. The uniformity-maximal alternative (delete it too; write `.Add`/`.Remove`/`.Subscribe`) is recorded as declinable, not recommended.

## 6. Option (a) — defined-by-expansion sugar with a **named** backing field

If the one-line spelling should survive, `event` remains in the grammar but §24 shrinks to a defined-by-expansion rule (the `for`-desugaring precedent, [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for)):

```bezel
[vis] event E: T;
```

expands, by definition, to

```bezel
private owned E_source: EventSource<T> := new EventSource<T>();   // `protected` if the event is `protected`
[vis] property E: IEvent<T> => E_source;                          // omitted when [vis] is `private`
```

(async and static analogously; static per §7). `E_source` is an ordinary declared name: visible in source tooling, collision-checked like any member (declaring both `event E` and a member `E_source` is an ordinary duplicate-member error), and usable everywhere an owned field is.

**Stated plainly: inside-class call sites change.** Under this expansion `E` names the *property* everywhere — including inside the declaring class — so every producer-side site in §24.5, §24.7, rationale §13, and user code changes from `E.Raise(...)`/`E += h` to `E_source.Raise(...)`/`E_source += h`. The sugar buys back only the declaration line, not the call sites. That is weaker sugar than the keyword is today, and it is the honest version.

### 6.1 The rejected variant: bare-name two-way resolution

The "exact sugar" — `E` resolving to the backing *field* inside the class and to the *property* outside, so that existing `E.Raise(...)` sites keep compiling — is **rejected in advance**. It would make name resolution access-site-dependent in exactly the way §24.3 makes typing access-site-dependent today: the same simple name denoting two different declarations with two different types depending on where the reader stands. That is C#'s field-like-event model with the mechanism moved one layer down, it preserves every §1.2-style obligation on future member-walking rules (each must still ask "which `E` am I looking at?"), and it deletes nothing this RFC exists to delete. An earlier draft of this argument died on precisely this point; the position is closed, not open for a cleverer encoding.

### 6.2 Weighing (a) against (b)

Option (a) keeps: the one-line declaration, interface auto-satisfaction (`IfaceEvent` retained as expansion into §5.4's property), grep-ability of `event`. It still deletes: the two-faced typing, the hidden field, §28.14's event clause, and §24-as-semantics (the section becomes one expansion rule; semantics live in stdlib §6.5 as in §5.3). It keeps: two grammar productions, the record/enum placement rule (the expansion target must still be rejected there — though now the *expansion's field* is what §9.7 rejects, so the rule can be a plain consequence), and a member kind for tools to know about. It adds one wart the keyword doesn't have today: a compiler-minted *visible* name (`E_source`) — name-minting sugar is a mild smell (collision rules, refactoring surprise) that pure removal avoids. Hence the recommendation order: (b) over (a), (a) over status quo only if the declaration-line ergonomics are judged decisive.

## 7. Static events — the explicit design point

[§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 5 (v0.163; strengthened v0.180) grants `static event` two guarantees keyed on the member kind:

> **Static events are runtime-synchronized and capture-free** (v0.163; strengthened v0.180): the compiler-generated subscriber list of a `static event` uses synchronized operations, since subscribe/raise may arrive from any domain. Additionally (v0.180), a handler subscribed to a `static event` — through `+=`/`Add` or `Subscribe` — must be **capture-free** [...] (Instance events remain unsynchronized and capture-admitting — domain-serial per [§24.6](#sec-thread-safety), captures per [§28.14](#sec-scoped-callback-parameters).)

A library-only design cannot condition synchronization or capture admission on *where a field is declared*: `EventSource<T>` is one type whether it sits in an instance field or a static one. A demoted `static owned readonly E_source: EventSource<T>` would today draw §22.12 rule 4's declaration-site warning (class type outside the synchronized set) and would carry **neither** rule-5 guarantee. The options, stated as the fork it is:

- **S1 (recommended): a synchronized, capture-free static-events type.** Add one stdlib class — working name `SharedEventSource<T>` (**proposed**, does not exist; name is the owner's) — with the §6.5 surface, an internally synchronized subscriber list, membership in the §22.12 rule-2/rule-4 **synchronized set** (so the static field pattern is warning-free), and `Add`/`Subscribe` positions that are stdlib-stated **capture-free admissions** (the exact precedent is `Domain.Spawn`'s capture-free entries, which §28.14 already cites as stdlib-stated positions outside the `scoped` machinery). Static events then demote to `private static owned readonly Exiting_source: SharedEventSource<()> := new SharedEventSource<()>();` with both v0.180 guarantees intact and *visible in the type* rather than in a member-kind rule. Cost: one more stdlib type — complexity relocated into the library, not deleted; but relocated to where every other cross-domain guarantee in the language already lives (the synchronized set is exactly the "types that carry their own concurrency contract" register). Non-sendable, like `EventSource<T>` (§30.14); static reachability is its distribution mechanism.
- **S2 (fallback): accept the rule-4 warning and the general static-singleton posture.** No new type; static event sources are `static owned readonly` fields of an unsynchronized class, warned at declaration, cross-domain discipline the programmer's obligation like every other class-typed static singleton. This is *consistent* with rule 4's recorded posture but strictly weaker than what v0.180 just shipped: the capture-free compile error (which closes a real hole — a capture-bearing closure transported to a foreign raising domain carries borrows of its origin domain's scopes; cf. the open miscompile [#1502](https://github.com/s8lap/bezel-lang/issues/1502) in exactly this corner) degrades to nothing. Choosing S2 means explicitly recording the acceptance of that regression — this RFC does not recommend it while the ink on v0.180 is wet.
- **S3 (forbidden): silently synchronize all `EventSource<T>` operations.** This would restore static safety by taxing every instance event and would reverse the recorded v0.163 instance-events-unsynchronized choice ("continuations never run concurrently, so [...] no external synchronization is required", §24.6) without engaging it. Ruled out by construction, not offered to the owner.

Under option (a), the static expansion targets the S1 type: `static event E: T;` → `private static owned readonly E_source: SharedEventSource<T> := ...` — rule 5 then deletes cleanly under either option, its content restated on the type. Implementation note: rule 5's synchronization half is *currently unimplemented anyway* ([#1392](https://github.com/s8lap/bezel-lang/issues/1392), open), so S1 does not discard shipped machinery — it gives #1392's eventual fix a home that is a type rather than a member-kind special case.

## 8. Capture admission after demotion (§28.14)

The v0.180 storage-bounded admission is where a demotion RFC could silently break soundness or expressiveness; walked explicitly:

**Identical outcomes.** Inside the class: `Clicked_source += OnClicked` / `Clicked_source.Add(h)` — form 4, chain rooted at `this` through an `owned` field hop → bounding object `this`, self-capture admitted; byte-for-byte the analysis §24.3 (v0.180) prescribes for `this.Clicked += OnButtonClicked` today. External, the two idiomatic discharges: `using var sub := pub.Clicked.Subscribe(h)` → **form 2** (bounded by `sub` — the forms are ordered and form 2 wins before any chain question arises, exactly as §28.14's own example states for the keyword form); `_sub := pub.Clicked.Subscribe(h)` into an owned field of `this` → **form 3**. All three carry over with no rule change.

**One real narrowing.** A capture-bearing external subscription whose `Subscription` is discharged some *other* way (bound to a plain `var` owned local, passed straight into a collection, etc.) resolves its bounding object today through form 4's event-member hop (`pub.Clicked` → chain head `pub`). Demoted, `pub.Clicked` is a **getter** hop, and form 4 says a getter resolves *no* bounding object (capture-free only) — the getter clause exists because a general getter can return a borrow of something the chain head does not own. So these off-idiom sites lose capture-bearing admission. Assessment: small surface (the owned `Subscription` must be discharged *somewhere*, and the two idiomatic discharges are forms 2/3), correct-by-default (rejection, never unsoundness), with a good diagnostic story ("bind the subscription with `using var`"). A **liftable** companion amendment is recorded for the owner: extend form 4's hop set with *transparent getters* — an expression-bodied getter whose body is a bare read of an `owned` field of its receiver (structural, decidable, and precisely the §28.6-bullet-1 shape) — which restores form-4 parity exactly for the demoted event pattern. Recommended: ship without it, lift on evidence, matching §28.14's own "v1 admission bounds / liftable" discipline.

**Net simplification.** Form 4's "or a compiler-generated event backing member" clause and its §24.3 cross-reference are deleted; the registration-lifetime residual's event mention retargets to the named field it already describes in owned-field terms.

## 9. Honest costs

Counted against the keyword form, per event:

1. **Lines**: 1 → 2 for a public instance event (field + property); 1 → 1 for private events; interface implementations additionally write the property §24.7 currently synthesizes (§5.4). Under option (a): 1 → 1, at the price of a minted name (§6.2).
2. **Two names where there was one.** `Clicked` and `Clicked_source`; producer-side call sites read `_source`. The two-faced typing's genuine virtue — one name, capability decided by the type system — is *given up*, not refuted. (Its cost, §1.2, is the other pan of the scale; weighing them is the ruling.)
3. **A mandatory `import System.Threading` in every event-*declaring* file.** Naming `EventSource<T>`/`AsyncEventSource<T>` in a field declaration requires the explicit import — [§2.3](../Bezel_Language_Spec.md#sec-always-available-names)'s auto-imported set deliberately covers only the consumer-side names (`IEvent<T>`, `IAsyncEvent<T>`, `Subscription`, `CancellationToken`): *"requiring an explicit `import System.Threading` to subscribe to any event would defeat the design."* The keyword form needs no import because the backing field is compiler-generated, and option (a) inherits that exemption; subscriber-side files stay import-free under every option. (Widening §2.3's auto-import to the source types would erase this cost but put producer-machinery names in every file — recorded as a declinable knob, not recommended.)
4. **Wiring is manual.** Forgetting `owned` on the field, or the initializer, is on the user. Mitigation: both mistakes are loud compile errors today (§28.6 fresh-owned-into-non-owned; definite-assignment), not silent misbehavior.
5. **Self-documentation and tooling.** `event` is grep-able and IDE-listable as a member kind; a convention (`X_source` + `IEvent` property) is softer. No mitigation offered beyond stdlib/guide documentation of the idiom.
6. **Migration** (§11): every `event` declaration and every producer-side call site in the corpus. The two guides contain **zero** event usages (verified); the blast radius is spec §24's own examples, rationale §13, and stdlib §6.5 cross-text.
7. **Static events need §7's resolution** — under S1, one new stdlib type; under S2, a recorded guarantee regression.
8. **The §28.14 off-idiom narrowing** (§8), absent the liftable amendment.

And the honest gains, symmetrically: the language's only access-site-dependent member typing deleted; two grammar productions and the §1.2 carve-out family deleted or dissolved; every future member-walking rule freed of the "and what about events?" obligation; events become almost entirely library semantics (the self-hosting direction, #545); standalone sources constructible (§2.3); Core implementations shed the entire event member machinery (§12).

## 10. Consequential stdlib §6.5 rewording (part of this RFC, either option)

Making the constructor public falsifies two load-bearing sentences, which must be reworded *with* the change, not discovered after:

- `// No public constructor; obtained via event declaration.` → `public constructor();` (both classes), with the fence paragraph retargeted from *obtainability* to *reachability*: the producer surface of a given event is fenced by the **field's** visibility — an `EventSource<T>` reference for someone else's event remains obtainable only if their class exposes it, which stays the recorded not-defended-against breach. Third-party construction of a *fresh* source grants no capability over anyone else's events.
- *"Since `EventSource<T>` is visible only inside the declaring class, the user-facing variance comes from `IEvent<out T>` exclusively"* (the invariance justification) → the variance *facts* are unchanged (`Raise(payload: T)` keeps the producer classes invariant; `IEvent<out T>` stays sound by the §12.9 composition argument, quoted in both documents). The justification's framing becomes: the *conventional public surface* is the interface, so user-facing variance is expected through `IEvent<out T>`; an exposed `EventSource<T>` is simply invariant, as its signature dictates.

Under option (b), §6.5 additionally absorbs §24's semantics per §5.3 and the "obtained via event declaration" framing disappears wholesale.

## 11. Migration

- **Spec**: §24 deleted (b) or reduced to the expansion rule (a); Appendix A.4 productions; the `event` keyword itself (Appendix B.1, §1.4 — unconditionally reserved today): under (a) it keeps its meaning; under (b) recommended **kept reserved-but-unused** — one word held out of the identifier namespace at zero lexer cost, keeping option (a) or a future re-introduction reachable — with un-reserving recorded as a separately declinable follow-up (a real B.1/lexer edit whose consequence is handing `event` back to user identifiers); §22.12 rule 5 per §7; §28.14 form 4 per §8; §2.3's threading-import sentence unchanged (it is what #1536 implements); Appendix D per §12; ~31 `event`-mentioning lines outside §24 (plus ~29 `events` prose mentions) reviewed, of which the producer-side subset is re-spelled (`E.Raise` → `E_source.Raise`; declarations to field+property) — subscription-side examples, the majority, are unchanged.
- **Rationale**: §13 gains this decision's record; §13.1/§13.4 examples re-spelled; no recorded reasoning is invalidated (§3).
- **Stdlib**: §6.5 per §10; §7 S1 type if chosen.
- **Guides**: no changes (zero event usages).
- **Compiler/tests**: the event member machinery (two-faced member typing, generated backing fields, `IfaceEvent` satisfaction) retires or (a) becomes an early desugaring; the runtime `EventSource` machinery is untouched. The open event-machinery bugs map cleanly: [#1392](https://github.com/s8lap/bezel-lang/issues/1392) resolves into S1's type (or S2's deletion), [#1502](https://github.com/s8lap/bezel-lang/issues/1502)'s program becomes a compile error under S1's capture-free admission, [#996](https://github.com/s8lap/bezel-lang/issues/996) (event source freed before subscription disposed leaks closure env) becomes an ordinary owned-field/Subscription lifetime bug with no member-kind involvement. The historical trail (#355 fence not enforced, #477 shadow-`EventSource` on interface-typed access, #479, #480 backing fields never freed, #880, #1064) is a record of defects specifically in the member-kind seams — evidence that the special casing has carried real implementation cost, offered as weight, not proof.
- **User code**: none exists (pre-users project).

## 12. Conformance (Appendix D)

Today's [D.2](../Bezel_Language_Spec.md#sec-extended-features) row ties the Events feature to §24 plus the stdlib types. Under option (b) the row becomes stdlib-only (the model is D.2's "Sequence operations" row, which lists no spec sections at all): a Core implementation omits the §6.5 types per [D.3](../Bezel_Language_Spec.md#sec-feature-dependencies)'s existing mechanism ("A Core implementation's standard library omits the types of unimplemented Extended features entirely [...]; programs referencing them fail name resolution with the feature-naming diagnostic of D.1") — no event grammar, member logic, or rejection rule to implement at all. D.3's dependency bullet ("**Events**' async event source requires **Asynchronous programming**; the synchronous `EventSource<T>` does not") carries over verbatim; S1's type, if adopted, slots into the same row.

## 13. Sizing

| Piece | Size |
|---|---|
| Spec edit | Moderate — §24 deleted (b) or reduced to one expansion rule (a); two Appendix A.4 productions; §22.12 rule 5 restated on the S1 type (§7); §28.14 form 4's event clause deleted (§8); Appendix D.2/D.3 per §12; ~31 `event`-mentioning lines outside §24 reviewed, the subscription-side majority unchanged |
| Stdlib edit | Moderate — §6.5 constructors + the §10 rewording + absorbing §24's semantics under (b); plus the S1 type (one §6.5-scale entry) if Q4 = S1 |
| Rationale edit | Small — §13 gains this decision's record; §13.1/§13.4 examples re-spelled; no recorded reasoning invalidated (§3) |
| Guides | None (zero event usages, verified) |
| Compiler | Precondition [#1536](https://github.com/s8lap/bezel-lang/issues/1536) first (small, independent, valuable regardless of the ruling). Then net-negative: ~80 references to the event member machinery (`EventSymbol`/`EventDecl`/`IfaceEvent`) across parser/sema/flow/ownership/codegen retire under (b) or collapse into an early desugaring under (a). S1's synchronized subscriber list is the one genuinely new runtime piece — the work [#1392](https://github.com/s8lap/bezel-lang/issues/1392) owes anyway, rehomed onto a type |
| Tests | ~36 files / ~140 `event` declaration sites re-spelled to the pattern (mechanical); the member-kind seam regression tests (§11's historical trail) retire with the seams they guard; S1 and the §8 narrowing gain new spec-derived cases |
| Sequencing | §2.3 + #1536 + §10's rewording can land ahead of and independently of the ruling (Q6); the demotion itself is one docs revision + one compiler retirement train, with S1 landable in the same stdlib revision |

## 14. Owner questions

1. **Q1 — the ruling**: demote at all? This is the ergonomics-vs-uniformity trade of §9 vs §1.2/§2.2; the finding is CONFIRMED but the demotion is a **choice, not a fix**. Recommended: yes.
2. **Q2 — shape**: option (b) removal, or option (a) named-backing-field expansion? Recommended: (b). (The bare-name variant of (a) is not on the table — §6.1.)
3. **Q3 — `+=`/`-=`**: keep the type-keyed desugaring (recommended, §5.5) or delete it with the member kind?
4. **Q4 — statics**: S1 (synchronized + capture-free stdlib type; recommended) or S2 (rule-4 warning, recorded guarantee regression)? S3 is not offered. If S1: name the type.
5. **Q5 — §28.14 transparent-getter hop**: adopt with the demotion, or record as liftable and ship without (recommended)? (§8.)
6. **Q6 — severed floor**: if Q1 is declined, adopt §2.3 (public constructors + #1536 nameability + §10's rewording) anyway? Recommended: yes — it costs nothing, unblocks spec-legal code, and stands on its own.
