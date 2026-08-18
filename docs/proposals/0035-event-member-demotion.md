# Proposal 0035 (RFC) — Demote the `event` member kind to the library pattern it abbreviates

| | |
|---|---|
| **Status** | graduated-v0.187 — **superseded — graduated in v0.187 (`7024955`)**: spec §24 tombstone, §17.2 desugaring, §22.12 rule 5, §28.14 register + formation bar, §28.3/§28.6 static-borrow, §2.3, A.4/B/C/D; stdlib §6.5 as the normative home; rationale §13.6; #1647 closed by construction, #1704 tracks the liftable transparent-getter. Open questions resolved as delegated rulings (recorded in the v0.187 changelog). *(prior: accepted (owner, 2026-07-12); draft (RFC))* |
| **Tracking issue** | [#1550](https://github.com/s8lap/bezel-lang/issues/1550) |
| **Target module(s)** | Spec [§24](../Bezel_Language_Spec.md#sec-events) (member kind removed or defined by expansion), [§22.12](../Bezel_Language_Spec.md#sec-static-members) rules 2/4/5, [§28.14](../Bezel_Language_Spec.md#sec-scoped-callback-parameters) form 4 + the stdlib-stated-retention register, [§17.2](../Bezel_Language_Spec.md#sec-notes-on-operators) (new home of the `+=`/`-=` desugaring, §5.5), [§2.3](../Bezel_Language_Spec.md#sec-always-available-names) (Q7 + justification-sentence rewording), [§13.7](../Bezel_Language_Spec.md#sec-visibility-and-inheritance) signature-visibility list, [§30.14](../Bezel_Language_Spec.md#sec-sendable-types) (one list entry), [§25.1](../Bezel_Language_Spec.md#sec-overloadable-operators) + Appendix C.2/C.10/C.11 cross-references, Appendix A.4 (`EventDecl`, `IfaceEvent`), Appendix B.1/B.3 (keyword reservation), Appendix [D.2](../Bezel_Language_Spec.md#sec-extended-features)/[D.3](../Bezel_Language_Spec.md#sec-feature-dependencies); stdlib [§6.5](../Bezel_Standard_Library.md#sec-events-ievent-eventsource) (public constructors, relocated semantics, consequential rewording; plus **two proposed types** if option S1 of §7 is chosen — `ISharedEvent<out T>` and `SharedEventSource<T>`, neither of which exists today). No new module. |
| **Written against** | spec/stdlib **v0.183**. The original draft was written and re-verified against v0.180; the deepening re-verified every citation it builds on against v0.183 (§1.3, §2.3, §6.6, §9.7, §11.5, §13.7, §14.2, §17.2, §17.7, §22.8, §22.12, §22.13, §23, §24, §25.1, §27.7.1, §28.3, §28.5/§28.6/§28.7/§28.14, §29.2, §30.8, §30.14, A.4, A.6, B.1/B.3, C.2/C.10/C.11, D.2/D.3; stdlib §2.11, §2.13, §6.5) and re-ran the probes (§1.3, main @ 3fcf02c). |
| **Design character** | **RFC** — an ergonomics-vs-uniformity trade, not a defect fix, though the static-events fork now carries a live spec gap ([#1647](https://github.com/s8lap/bezel-lang/issues/1647), §7) that must be resolved *somewhere* whatever the ruling. Events are otherwise sound today. The recommendation is argued; the owner decides. Nothing here lands in the spec this run. |
| **Preconditions** | Public parameterless constructors on `EventSource<T>`/`AsyncEventSource<T>` (§2.3, a stdlib change this RFC carries). Source-level nameability of those types ([#1536](https://github.com/s8lap/bezel-lang/issues/1536)) has **landed** since the first draft; the remaining compiler gap is that user construction of an `EventSource<T>` is admitted but miscompiled — filed as [#1646](https://github.com/s8lap/bezel-lang/issues/1646) during the deepening (§1.3). If Q4 = S1, the S1 scope bound of §7.1 (sealed pair, stdlib-only retention) is part of the precondition set. |

> **Changelog** — *2026-07-11 deepened:* the recommended idiom is rewritten to the two-line floor using only existing mechanisms (§5.1: `new()` shorthand, `readonly`, `_camelCase`); a floor argument (§5.1.1), a declaration-surface decision record (§5.1.2), and a complete before/after site table (§5.1.3) are added; the per-file `import System.Threading` is dissolved via a new Q7 (§2.4, reversing the first draft's posture with the reversal recorded); statics (§7) are reworked into a **sealed** `ISharedEvent<out T>` + `SharedEventSource<T>` pair with the capture-laundering derivation, the newly-filed live spec gap [#1647](https://github.com/s8lap/bezel-lang/issues/1647) engaged head-on, and the stdlib-only retention scope bound recorded; the kept `+=`/`-=` desugaring is re-homed to §17.2 with its `readonly`-LHS interaction stated (§5.5); the §24→stdlib relocation is now an explicit checklist (§5.3); the honest-cost ledger is consolidated into one table (§9.10). A same-date adversarial critique was resolved in full: five blocking items fixed (S1 retention mechanism, #1647, desugaring home, S1 surface completeness, relocation checklist), ten advisories folded. Probes re-run at main @ 3fcf02c; #1536 retired to landed, #1646 and #1647 filed.

## 0. Summary

The `event` member kind is a one-line abbreviation for two lines of ordinary members: a private `owned` field of type `EventSource<T>` and a public expression-bodied property of type `IEvent<T>` borrowing it. Everything else about events — `Raise`/`RaiseAsync`, `Subscribe`/`Subscription`, snapshot-raise, first-error-wins handler exceptions, `IEvent<out T>` covariance, domain-locality — is *already* library semantics on the stdlib §6.5 types and survives unchanged. What the member kind uniquely adds to the language is its **two-faced member typing** ([§24.3](../Bezel_Language_Spec.md#sec-the-type-of-an-event-member)):

> An event member has two faces, distinguished by whether the access is from inside the declaring class or from outside.

This is the language's only rule under which the *static type* of a member access depends on where the access is written. It is not free: every ownership and capture rule that walks members must special-case it, and the v0.180 capture train had to do exactly that, in three separate places (§1.2).

This proposal recommends **removing the `event` member kind** (option (b), §5): delete `EventDecl` and `IfaceEvent` from Appendix A.4, relocate §24's semantics into stdlib §6.5 (per the explicit checklist of §5.3), and document the field+property pattern as the idiom. The owner's follow-up question — *how clean can that pattern be made?* — is answered in §5.1.1–§5.1.2: **two declarations is the floor** for a public event under the three invariants this RFC defends (declaration-fixed types, the type fence, no runtime type tests on the producer path), and the recommended idiom reaches that floor using **only mechanisms the language already has** — the [§17.7](../Bezel_Language_Spec.md#sec-the-new-operator) `new()` shorthand, [§22.8](../Bezel_Language_Spec.md#sec-field-initializers) `readonly`, and [§1.3](../Bezel_Language_Spec.md#sec-identifiers)'s `_camelCase` field convention:

```bezel
private owned readonly _clicked: EventSource<ClickedEvent> := new();
public property Clicked: IEvent<ClickedEvent> => _clicked;
```

One line for `private`/`protected` events; two for public ones (a class implementing an interface event writes the same two lines — the property doubles as the interface satisfaction, §5.4); zero new grammar. The residual per-file cost — `import System.Threading` in every event-*declaring* file — is dissolved by making the producer types available without import (Q7, §2.4), for which the language has two exact precedents. A fallback **defined-by-expansion sugar** (option (a), §6) is specified with an explicitly *named* backing field; the tempting "exact sugar" in which the bare event name resolves to the field inside the class and to the property outside is **rejected in advance** (§6.1) — it would relocate the access-site dependence from typing into name resolution rather than delete it, which is C#'s field-like-event model and the opposite of the point.

Two things must be settled *as design points, not footnotes*, for either option: static events, whose §22.12 rule-5 guarantees (synchronized subscriber list, v0.163; capture-free handlers, v0.180) are keyed on the member kind and cannot be conditioned on storage site by a library-only design — and whose capture-free guarantee is, at v0.183, **unenforceable through an `IEvent<T>`-typed alias** ([#1647](https://github.com/s8lap/bezel-lang/issues/1647), found while deepening §7's derivation; §7's recommended pair is the first enforceable statement of the guarantee); and the §28.14 form-4 capture admission, which names event backing members explicitly (§8).

Two things this proposal is **not**:

- **Not a bug fix.** No recorded defect *requires* this (the #1647 gap has member-kind-local resolutions too, priced in §7). The finding is that the mechanism is *removable*, not that it is broken. The keyword is one line and auto-wires initialization, disposal, and interface satisfaction; the pattern is one to two lines and two names. That is a real ergonomic price, counted honestly in §9, and reasonable owners can decline to pay it.
- **Not a removal of events.** Every observable behavior of §24 — including the guides' and rationale's motivating use cases — remains expressible, most of it with identical call sites. What is removed is the *member kind*, not the capability.

## 1. The mechanism today (v0.183)

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

### 1.2 The carve-outs it forces elsewhere (all live at v0.183)

The member kind is not one rule; it is one rule plus every other rule that must know about it:

1. **Grammar**: two productions exist solely for it (Appendix A.4):
   ```
   EventDecl  ::= Visibility "static"? "async"? "event" Identifier (":" Type)? ";"
   IfaceEvent ::= "async"? "event" Identifier (":" Type)? ";"
   ```
2. **Member-kind placement rule** (v0.180, [§24.1](../Bezel_Language_Spec.md#sec-declaration)): events are admitted on "classes and interfaces only"; an `event` in a record or enum body is a compile error naming the rule — a rejection needed *because* the hidden backing field would violate [§9.7](../Bezel_Language_Spec.md#sec-records-and-disposal) ("Records cannot have destructors, `owned` fields, or implement `IDisposable`"). With a named field, §9.7 rejects the field directly and the event-specific rule has nothing left to say.
3. **The §28.14 capture machinery** (v0.180): the storage-bounded admission's bounding-object form 4 must enumerate event members as an honorary field kind — *"a call whose receiver is a member chain rooted at `this` or a local binding in which every hop is an **`owned` field or a compiler-generated event backing member** ([§24.3](../Bezel_Language_Spec.md#sec-the-type-of-an-event-member))"*. A named backing field is an `owned` field; the clause's second half exists only because the event member is not one.
4. **Statics** ([§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 5, v0.163, strengthened v0.180): a member-kind-keyed synchronization and capture-freedom rule for `static event` — the load-bearing design point, taken up in §7. Three **verified findings from the 2026-07-11 sweep** show this keying is already leaking: rule 5's generated backing field is unaccounted for by rules 2/4's "every static field" quantification and by §30.14's `EventSource<T>` domain-locality entry (finding seam-audit-04, CONFIRMED); §24.6's blanket "The event subscriber list is not synchronized" contradicts rule 5 outright (finding spec-hot-04, CONFIRMED); and — found while deepening this RFC — rule 5's capture-free compile error is **not enforceable through an `IEvent<T>`-typed alias** at all ([#1647](https://github.com/s8lap/bezel-lang/issues/1647); §7). Interim reconciliation edits for the first two are in flight independently of this RFC; §7 shows both **dissolve** under the demotion — and shows that no member-kind-local interim edit closes #1647.
5. **Modifier interaction prose** (§24.1): "Events do not support `virtual`, `override`, `abstract`, `sealed`, or `readonly`" — a rule ordinary fields and properties get from their own grammars.

Each of these is small. Their sum is the actual cost of the member kind: every future rule that walks members inherits the obligation to decide what it means for events. The v0.180 train is the concrete demonstration — it had to touch events in items 2, 3, and 4 above plus the `-=` matching rule and the raising-domain rule, for a feature whose semantics are otherwise entirely in the stdlib.

### 1.3 Probe results (main @ 3fcf02c, 2026-07-11 — supersedes the first draft's §1.3)

The keyword form compiles and runs (subscribe + two raises observed). The manual pattern's status has **moved** since the first draft (which reported `unresolved type name EventSource`): [#1536](https://github.com/s8lap/bezel-lang/issues/1536) has landed and the names now resolve under `import System.Threading`. The failure is now downstream, and worse in character:

- `new EventSource<T>()` is **admitted** by the compiler even though stdlib §6.5 declares `// No public constructor; obtained via event declaration.` — a conforming compiler would reject it today;
- a user-constructed source assigned in a **constructor body** produces a dud: `Subscribe` (through the field directly *or* through a borrowing property) plus `Raise` runs to completion with handlers **silently never invoked**;
- the same construction in a **field-initializer** position ICEs (`internal: lowerExpr returned null for instance field/property initializer — #1386`; a plain `private owned _items: Array<int> := new Array<int>();` control lowers fine, so the branch is specific to the event-source construction path).

Filed as [#1646](https://github.com/s8lap/bezel-lang/issues/1646) with repro programs. Until the §2.3 precondition is accepted, the *conforming* fix is rejection; if the precondition is accepted (including under the severed floor, Q6), #1646 is the tracking issue for making user construction actually work. Either way, the manual pattern remains spec-blocked at construction today, hence the precondition in §2.3 below.

## 2. The finding, and its preconditions

### 2.1 The pattern is spec-legal except for the constructor

Each piece of the manual pattern is admitted by an existing, general rule:

- The field: an ordinary owned field with a fresh-owned initializer, initialized during construction and disposed with the object ([§28.5](../Bezel_Language_Spec.md#sec-owned-fields)) — the *same* rules §24.1 already invokes for the generated field ("following the standard owned-field rules"). `readonly` on it is [§22.8](../Bezel_Language_Spec.md#sec-field-initializers)'s ordinary field-`readonly` (assignable only in initializer/constructor), and the `owned readonly` modifier order is [§11.5](../Bezel_Language_Spec.md#sec-modifier-order)'s. The `:= new()` initializer is the [§17.7](../Bezel_Language_Spec.md#sec-the-new-operator) shorthand, explicitly permitted in field initializers.
- The property: a bare read of an owned field is a receiver-sourced **borrow** through a non-owned position ([§28.6](../Bezel_Language_Spec.md#sec-leak-prevention) bullet 1, v0.151); the getter prohibition covers only *freshly-produced* owned values, not field reads. The upcast to the implemented interface is routine, and the caller gets exactly the outside face §24.3 prescribes: an `IEvent<T>` exposing only `Subscribe`.
- The subscription lifetime: `using var sub := c.Ticked.Subscribe(h);` — unchanged; `Subscribe` is on the interface and `scoped` (v0.175), returning an owned `Subscription`.
- The raise: `_ticked.Raise(new TickEvent(n));` — a method call on an owned field.

The type fence survives with the same strength for the same reason — and that strength should be stated precisely: an `EventSource<T>` for a *given* event is obtainable outside its class only if the class leaks it **or a holder of the interface face downcasts it** (`face as EventSource<T>` — the runtime object behind the face *is* the source; equally true of the keyword form today, so no regression; named as the recorded, not-defended-against breach in §10). Field privacy, not constructor privacy, is what fences a given event — constructor privacy only prevents *standalone* creation, which threatens nothing of yours (a stranger constructing their own `EventSource<T>` can raise only their own event).

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

Nameability (#1536) has landed; the compiler work remaining behind this precondition is #1646 (§1.3). A standalone benefit falls out even under the status quo: event-relay and event-bus shapes (a source owned by a struct-of-sources, a source per dictionary slot) become expressible without a wrapper publisher class per event. The public parameterless constructor also makes `EventSource<T>` satisfy `where T: new()` per [§14.2](../Bezel_Language_Spec.md#sec-constraints)'s constraint table — unlooked-for but harmless.

A demotion recommendation without this precondition would be vacuous — the probe (§1.3) shows the manual pattern does not work today. The precondition is deliberately severable: the owner can accept §2.3 alone (making the pattern *possible*) while declining the demotion (keeping the keyword), and the language would be strictly more uniform than today at zero migration cost. That severed outcome is this RFC's recommended floor.

### 2.4 Precondition-grade ergonomics: producer-type availability without import (new in the deepening; Q7)

Post-demotion, `EventSource<T>` appears in the declaration of every event in the program, and [§2.3](../Bezel_Language_Spec.md#sec-always-available-names)'s always-available set deliberately covers only the consumer-side names (`IEvent<T>`, `IAsyncEvent<T>`, `Subscription`, `CancellationToken`): *"requiring an explicit `import System.Threading` to subscribe to any event would defeat the design."* The keyword form needs no import because the backing field is compiler-generated; demotion deletes that exemption, so every event-*declaring* file gains a mandatory `import System.Threading;`. The first draft recorded widening §2.3 as "a declinable knob, not recommended." **This deepening reverses that recommendation, and records the reversal**, on two grounds: the owner's directive is that the declaration surface be as clean as possible, and the balance genuinely changed — the knob was priced against the keyword's import exemption, which the demotion removes. (The recorded §2.3 reasoning is keyed to *subscription* pervading idiomatic code — every consumer file; declaration happens only in publisher files, a strictly smaller set — so the old argument extends here by analogy, not verbatim. The honest grounds for the reversal are the two just stated.)

Two spellings, each with an exact recorded precedent; the owner picks one (Q7):

- **(i) Auto-import the names** — add `EventSource<T>` and `AsyncEventSource<T>` (plus §7's S1 types, if adopted) to §2.3's always-available list while they keep living in `System.Threading`. Precedent: `CancellationToken`, which "is auto-imported as a language primitive" despite living in `System.Threading` ([§30.8](../Bezel_Language_Spec.md#sec-cancellation), §2.3). Smallest edit; the names remain shadowable like every non-primitive always-available name (§2.3).
- **(ii) Relocate the types to `System`** — move the §6.5 types into the auto-imported `System` module. Precedent: `Subscription` itself, which **v0.76 moved from `System.Threading` to `System`** "so that all register-and-unregister APIs returning a `Subscription` work without requiring an explicit threading-module import" (stdlib §6.5, last paragraph). Cleaner conceptually (events are observer-pattern machinery, not threading primitives), but a larger stdlib § reorganization.

Recommended: **(i)**, as the minimal edit with the CancellationToken precedent; (ii) recorded as the equally-principled alternative. Declining Q7 entirely leaves the import as honest cost §9.3.

Knock-on under option (b), whatever Q7's answer: §2.3's *justification sentence* for the existing entries — "`IEvent<T>` and `IAsyncEvent<T>` are auto-imported because they are the public types of every event member viewed from outside the declaring class (§24.3)" — dangles on a deleted section and a deleted concept; it must be reworded to key on the pattern (they are the public types of the §6.5 idiom's property). Enumerated in §11's spec list.

## 3. Recorded decisions engaged

Per the house rule, the recorded declines and rulings adjacent to this change, and why none pre-decides it:

- **Rationale [§13.1](../Bezel_Design_Rationale.md#sec-raise-instead-of-invoke)** (Raise instead of `?.Invoke`): defends `Raise` against C#'s null-delegate idiom and records the decline of direct invocation `event(args)` — *"It's cleaner at the call site but conflicts with the asymmetric inside/outside types."* Both the winner and the decline are **library-level** (`Raise` is a method on `EventSource<T>`) and survive demotion verbatim. Notably, §13.1's own summary of the asymmetry — *"enforced by the static type, not by a special access rule"* — is an argument the field+property pattern satisfies *more* literally than the member kind does: with a named field the two types belong to two declarations, and no member changes type by viewpoint.
- **Rationale §13.2** (payload types, not handler types), **§13.3** (method-reference subscriptions), **§13.5** (first error wins): all library-level; unchanged.
- **Rationale [§13.4](../Bezel_Design_Rationale.md#sec-events-on-interfaces)** (events on interfaces, v0.62): motivates interface events and prices them — *"The cost of supporting the feature is one new grammar production (`IfaceEvent` in Appendix A.4) and a paragraph of semantics."* §13.4 also contains the closest thing to a recorded adverse judgment on §5.4's replacement shape, and it must be quoted rather than skirted: without interface events, contracts needed *"an abstract base class (which consumes the single-inheritance slot) or an awkward `GetEventX(): IEvent<X>` method that worked around the missing language feature."* §5.4's `readonly property PropertyChanged: IEvent<T>;` is materially that workaround in property dress — and the answer is that the property form removes precisely the awkwardness v0.62 was pointing at: no parens, declaratively read-only, and satisfied by the very property the C2 idiom already declares for the class's own consumers (no extra accessor asymmetry). v0.62 weighed the workaround against *adding* one production; this RFC weighs it against *removing* one plus the member kind's §1.2 tax — a different scale, on which v0.62's cost can be refunded. The *use cases* §13.4 records are fully served either way.
- **v0.76 type fence** (rationale changelog): removed event compiler magic in favor of type enforcement — the trajectory this RFC completes (§1.1).
- **v0.163 statics-and-domains (#1363), strengthened v0.180**: instance events deliberately **unsynchronized** ("Instance events remain unsynchronized and capture-admitting — domain-serial per [§24.6](../Bezel_Language_Spec.md#sec-thread-safety), captures per §28.14"), static events synchronized and (v0.180) capture-free. A recorded choice this RFC must not reverse by accident — engaged head-on in §7.
- **§2.3's consumer-only auto-import** (v0.76 reasoning, quoted in §2.4): a recorded posture this deepening proposes to **extend, with the deviation recorded** — §2.4/Q7; the first draft's contrary recommendation is superseded and the reversal reasoned there.
- **[§1.3](../Bezel_Language_Spec.md#sec-identifiers) naming conventions** (`_camelCase` for private fields): the first draft's idiom (`Clicked_source`) violated the language's own recorded convention; the deepened idiom (`_clicked`) conforms. Cosmetic, but the idiom is the deliverable here, so it is held to the recorded style.
- **§28.14's recorded deferral of the "unbounded" third tier** ([#1427](https://github.com/s8lap/bezel-lang/issues/1427); "admitting only capture-free or transitively unbounded arguments, which the callee may then retain freely" — deferred "until evidence demands it"): §7's S1 is designed *not* to require it (stdlib-only retention, scope bound recorded); the tier remains the recorded lift path for user-authored cross-domain publication points, and this RFC neither adopts nor forecloses it.
- **No recorded decision weighs the member kind against the library pattern.** Rationale §13.1–§13.5 defend the design against in-kind alternatives (direct invocation, internal-visibility magic, handler-type declarations, aggregation); nothing in the changelogs through v0.183, in Appendix D's history, in `docs/proposals/DECLINED.md`, or in the issue tracker asks or pre-declines *this* question. Events' Core-vs-member-kind status, like 0036's subject, is a default, not a decision.

## 4. The recommendation in one paragraph

Adopt §2.3 (constructors; #1646 as the compiler tracking) unconditionally, and §2.4 spelling (i) (producer names always available). Then prefer **option (b)**: remove the `event` member kind and the `IfaceEvent` production, relocate §24's normative semantics into stdlib §6.5 per §5.3's checklist (with the `+=`/`-=` desugaring re-homed to spec §17.2, §5.5), document §5.1's tightened idiom as *the* event pattern, and settle static events per §7 (recommended: S1 — the **sealed** `ISharedEvent<out T>` + `SharedEventSource<T>` pair with stdlib-only retention, which is also the design that closes [#1647](https://github.com/s8lap/bezel-lang/issues/1647) by construction). If the owner judges the per-event ergonomic price (§9) too high, adopt **option (a)** — the keyword survives as *defined-by-expansion* sugar with an explicitly named backing field (§6) — which keeps the one-line spelling while still deleting the two-faced typing and most of §1.2. What must **not** be adopted is the bare-name variant of (a) (§6.1) or the silent synchronization variant of §7 (S3).

## 5. Option (b) — remove the member kind (recommended)

### 5.1 The idiom

```bezel
module App;
                                        // no import needed under Q7 (§2.4)
public record ClickedEvent(Sender: Button, X: int, Y: int);

public class Button
  private owned readonly _clicked: EventSource<ClickedEvent> := new();
  public property Clicked: IEvent<ClickedEvent> => _clicked;

  private method OnPress(x: int, y: int)
    _clicked.Raise(new ClickedEvent(this, x, y));
  end OnPress;
end Button;

// Subscription sites are UNCHANGED:
using var sub := button.Clicked.Subscribe((e) => UpdateStatus(e.X, e.Y));
```

Every piece is an existing mechanism, cited: `owned readonly` in [§11.5](../Bezel_Language_Spec.md#sec-modifier-order)'s modifier order with [§22.8](../Bezel_Language_Spec.md#sec-field-initializers)'s `readonly` semantics; `:= new()` is the [§17.7](../Bezel_Language_Spec.md#sec-the-new-operator) shorthand ("A field initializer or typed local declaration may use the `new()` shorthand when the type is unambiguous"), so the constructed type is spelled **once**; the property line is a [§23.2](../Bezel_Language_Spec.md#sec-expression-bodied-read-only-properties) expression-bodied read-only property whose body is the [§28.6](../Bezel_Language_Spec.md#sec-leak-prevention)-bullet-1 borrow; `_clicked` follows [§1.3](../Bezel_Language_Spec.md#sec-identifiers)'s `_camelCase` private-field convention, and `Clicked` keeps the exact public name every §24 example uses, so subscriber-side code is byte-for-byte today's.

`readonly` is deliberate, not decoration: the keyword's generated backing field has no assignable name, so it is de-facto immutable; `readonly` is the faithful translation, and it turns an accidental reassignment — which under [§28.7](../Bezel_Language_Spec.md#sec-reassigning-owned-fields)'s drop-on-overwrite would silently dispose the source and sever every live subscription — into a compile error. (Publishers that genuinely want to swap a source drop the `readonly` and accept §28.7's stated semantics; the *idiom* keeps it.) The `readonly` field composes with the producer-side `_clicked += h` spelling only because of the desugaring's "does not read, does not assign" sentence — a load-bearing interaction stated in §5.5.

- A **`private` event needs no property at all** — one line, same as the keyword.
- A **`protected` event** declares the field `protected` (derived classes raise through it, replacing §24.3's derived-class face) — one line.
- **Async events** use `AsyncEventSource<T>` / `IAsyncEvent<T>` identically.
- **No-payload events** use `EventSource<()>` and its `Raise()` convenience overload (stdlib §6.5, unchanged). The keyword's payload-type omission (`public event Closed;`) has no analog — the `()` is written (counted in §9.9).

### 5.1.1 Why two declarations is the floor (the owner's question, answered)

Can a *public* event be one declaration? Not without paying one of exactly four prices — a small impossibility argument worth having on record. Its premises, stated honestly: **declaration-fixed types** (a name's static type does not depend on where the access is written), **the type fence** (the produce capability is not reachable through the public face's static type), and **no runtime type tests on the producer path** (the class raises through a statically typed name, not through a downcast).

A single member declaration introduces **one name, with one static type, at one visibility**. A public event needs both capabilities:

- **P (produce):** a name whose static type carries `Raise`, reachable inside the class only — else the fence is gone;
- **C (consume):** a name whose static type carries only `Subscribe`, reachable outside.

One declaration gives one (name, type, visibility) triple. If its type carries `Raise` and it is public, the fence is lost (price 1 — §5.1.2's C6). If its type is the subscribe-only face, the class cannot raise through it under the third premise — its *own* reads see the declared type ([§24.3](../Bezel_Language_Spec.md#sec-the-type-of-an-event-member)'s "type-driven, not a special access-control rule" applied to ourselves), and recovering `Raise` via `(clicked as EventSource<T>)!` is exactly the runtime type test the premise excludes (price 2 — §5.1.2's C10: legal under v0.183 rules once §2.3 lands, and strictly worse than the idiom: a cast per raise site, runtime-checked, on a surface that still leaks the downcast to everyone else too). If it is private, there is no consumer face. The remaining escapes are: make the one name mean different things by access site (price 3 — the two-faced typing / bare-name resolution, rejected in §6.1), or mint new syntax that *is* two declarations written as one (price 4 — §5.1.2's C4). Ownership cannot substitute for visibility here either: inside and outside code both reach the source as *borrows* (field read, property read — [§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)'s producers table), so no owned-vs-borrow distinction separates the two audiences.

So: **two declarations for a public event is the floor** under those three premises, and the design question collapses to making each declaration minimal and deleting the incidental residue (the import line, the type repetition, the convention). That is what §5.1 does. `private` and `protected` events — where P and C coincide or differ only by inheritance — reach one line, which is keyword parity.

### 5.1.2 Decision record: candidate surfaces considered

Generated and scored for the deepening. Scoring axes: declaration size (lines / distinct concepts for a public instance event), net language delta (grammar productions + typing/name-resolution rules), fence integrity, composition with §28.14/§22.12/borrow rules, migration, teachability.

| # | Surface | Lines | Language delta | Verdict |
|---|---|---|---|---|
| C0 | `event` keyword (status quo) | 1 | the §1.2 family (baseline being removed) | baseline |
| C1 | first draft's idiom (`Clicked_source`, full `new EventSource<T>()`, no `readonly`, import required) | 2 + import | 0 | superseded by C2 — same shape, needless noise |
| **C2** | **tightened idiom (§5.1) + Q7 availability** | **2 (1 for private/protected)** | **0 new; §2.3 list edit under Q7(i)** | **winner** |
| C3 | field-type inference (`private owned readonly _clicked := new EventSource<ClickedEvent>();`) | 2 | +1 grammar relaxation (`FieldDecl` type optional) +1 inference rule | declined: dominated by the *existing* §17.7 `new()`, which achieves single-spelling while keeping the member's declared type explicit — member signatures are documentation; no need to buy with new rules what `new()` already sells |
| C4 | fused dual declaration (one line declaring field + forwarding property, e.g. a `view`/`expose` production, defined by expansion) | 1 | +1 production, +1 expansion rule, a new member kind for every tool and member-walking rule to classify | declined: this RFC's thesis is that a rule whose only job is abbreviating two ordinary declarations should be deleted, not re-minted in general dress; the fused line is also *longer* than either line it fuses (three type spellings) — it fails the very cleanliness test that motivated it. If the owner wants a one-line spelling, option (a) already provides it with less novelty (§6) |
| C5 | primary-constructor integration (`class Button(owned clicked: EventSource<ClickedEvent> := new())`, [§22.10](../Bezel_Language_Spec.md#sec-methods-and-constructors-primary-constructors) capture) | 1 header param + 1 property | 0 | declined: the source becomes a *constructor parameter* — every `new Button(...)` site sees an optional event-source parameter (API pollution), a caller may pre-subscribe/pre-raise before transfer (fence weakened at the edge), and the property line is still needed — nothing is saved |
| C6 | public field of type `EventSource<T>` (drop the fence; Node-style public `emit`) | 1 | 0 | declined: reverses the recorded v0.76 fence and rationale §13.1's asymmetric-types design; §6.5's "the conventional design is to never expose the `EventSource` itself" is normative-adjacent text this RFC has no mandate to overturn |
| C7 | raiser/source split pair (subscribe-only public object + separate raise-capability handle from construction) | 2 fields + ctor wiring | 0 grammar; +2 stdlib types | declined: strictly more objects, names, and wiring than C2 for the same fence; the capability handle must itself be stored → a second private declaration anyway. (The one place this shape *is* right is §7's statics, where the second type is buying synchronization and capture-freedom, not visibility) |
| C8 | `Publisher<T>` base class providing the public face | 0 per event + base slot | 0 grammar; +1 stdlib type | declined: burns the single-inheritance slot, one payload type per class, dead for multi-event classes |
| C9 | rename `EventSource<T>` to something shorter (`Event<T>`) | — | 0 | declined: under C2 the type name appears exactly once per event; a rename buys ~7 characters at the cost of churning §6.5's whole recorded history and losing the producer-semantics of "source" |
| C10 | single interface-typed owned field (`public owned readonly clicked: IEvent<ClickedEvent> := new EventSource<ClickedEvent>();`), raise via downcast | 1 | 0 | declined: the §5.1.1 price-2 escape made concrete — a `(clicked as EventSource<ClickedEvent>)!` per raise site, runtime-checked where the idiom is static, a public field where the idiom has a property, and the downcast it uses on the producer path is the same one §10 records as the fence's known breach — an idiom built *on* the breach cannot also defend the fence |

**Winner: C2.** It is the only candidate that reaches the §5.1.1 floor with **zero new language surface**, and its every ingredient is an existing, spec-cited mechanism. The declaration reads as exactly what it is — an owned resource and its public borrowed face — which is the teachability argument: nothing event-specific remains to teach beyond the two §6.5 types themselves.

### 5.1.3 Before / after, every site

| Site | Today (keyword, v0.183) | Post-demotion (C2) |
|---|---|---|
| Public instance event | `public event Clicked: ClickedEvent;` | `private owned readonly _clicked: EventSource<ClickedEvent> := new();`<br>`public property Clicked: IEvent<ClickedEvent> => _clicked;` |
| Private event | `private event Flushed: FlushEvent;` | `private owned readonly _flushed: EventSource<FlushEvent> := new();` |
| Static event (S1, §7) | `public static event ApplicationExiting;` | `private static owned readonly _exiting: SharedEventSource<()> := new();`<br>`public static property Exiting: ISharedEvent<()> => _exiting;` |
| Interface event + implementation | `event PropertyChanged: PropertyChangedEvent;` (iface)<br>`public event PropertyChanged: PropertyChangedEvent;` (class) | `readonly property PropertyChanged: IEvent<PropertyChangedEvent>;` (iface, §5.4)<br>the two C2 lines (class) |
| Subscribe, external (lambda) | `using var sub := button.Clicked.Subscribe((e) => ...);` | **unchanged** |
| Unsubscribe, external | dispose `sub` (scope end / explicit) | **unchanged** |
| Subscribe/unsubscribe, producer-side (named method) | `Clicked += OnClicked;` … `Clicked -= OnClicked;` | `_clicked += OnClicked;` … `_clicked -= OnClicked;` (§5.5) |
| Raise | `Clicked.Raise(new ClickedEvent(this, x, y));` | `_clicked.Raise(new ClickedEvent(this, x, y));` |
| Raise, static no-payload | `ApplicationExiting.Raise();` | `_exiting.Raise();` |
| [§27.7.1](../Bezel_Language_Spec.md#sec-equality-of-function-values) deterministic-unsubscription pattern | *see below* | *see below* |

**The §27.7.1 store-once pattern deserves its own paragraph**, because the sweep's verified finding seam-audit-01 (CONFIRMED, 2026-07-11) established that §27.7.1's current field-stored-lambda example (`private _onClicked: (ClickedEvent) -> () := (e) => ...;` then `+= _onClicked` / `-= _onClicked`) is **already ill-formed** under §28.14's v1 admission bound 3 — a function-typed *field read* is an unverifiable argument form at a `scoped` `Add` site — independent of this RFC (a §27.7.1 rewrite is in flight on that finding's train). The demotion does not change that admission one way or the other; the post-demotion deterministic-unsubscription idioms are the same ones that rewrite lands on:

```bezel
// producer-side, named method reference — deterministic per §27.7.1 (same method, same receiver),
// a verified argument form under §28.14 bound 3, bounding object `this` via form 4 (§8):
_clicked += OnClicked;
// later:
_clicked -= OnClicked;

// any lambda, anywhere — the handle is the unsubscription:
using var sub := button.Clicked.Subscribe((e) => UpdateStatus(e.X, e.Y));
```

The member kind's deletion does remove one *source of confusion* here: §24.4's current example spells a producer-side subscription `button.Clicked += OnButtonClicked`, which reads as consumer code and is legal only because `button` is another instance of the *declaring* class (§24.3's inside-face rule is class-keyed, not instance-keyed). Post-demotion the producer face has a visibly private name — `button._clicked += OnButtonClicked` can only be written where it is actually legal, and reads as what it is. (Cross-*instance* private access is deliberate and correct here: [§13.1](../Bezel_Language_Spec.md#sec-levels)'s `private` is class-keyed, not instance-keyed, so another `Button`'s method may name `button._clicked` — same as today's rule, now visible in the spelling.)

### 5.2 What is deleted, and what dissolves

Deleted from the spec: §24.1/§24.3's member kind and two-faced typing; `EventDecl` and `IfaceEvent` (Appendix A.4); §24.1's modifier-interaction prose. Dissolved into rules that already exist:

| Event-specific rule (v0.183) | Becomes |
|---|---|
| Generated backing field init/disposal (§24.1) | §28.5 owned-field rules on a visible field — the same rules, applied to a declaration the reader can see |
| Events on classes/interfaces only (§24.1, v0.180) | §9.7: records/enums cannot have `owned` fields — the field is rejected directly, no event rule needed |
| §28.14 form 4's "or a compiler-generated event backing member" clause | plain "`owned` field" — the clause's reason for existing disappears |
| Static backing-field placement + exit disposal (§24.1) | §22.12/§22.13 static-field rules on a visible static field (see §7 for rule 5) |
| Outside face = `IEvent<T>` (§24.3) | the property's declared type |
| Inside face = `EventSource<T>` (§24.3) | the field's declared type |
| Derived-class face for `protected` events (§24.3) | `protected` field visibility |
| "Events do not support `readonly`" (§24.1) | the idiom *recommends* `readonly` — the modifier the member kind had to forbid becomes the pattern's safety feature (§5.1) |

### 5.3 Where §24's semantics go — the relocation checklist

The stdlib is one of the five normative documents, so spec→stdlib relocation is not weakening. But "relocate the semantics" is not a landing spec; the following table is. Every normative sentence of §24 is accounted for — moved, dissolved into a general rule, or deliberately dropped with the drop recorded. (Coordination: the 2026-07-11 sweep's stdlib-2-04 verified finding adds explicit `destructor;` lines with quiescence comments to the §6.5 class blocks — that edit is compatible with, and survives, this relocation.)

| §24 content (v0.183) | Destination |
|---|---|
| §24.1 declaration syntax; the member kind; the generated-backing-field paragraph | **deleted** (b) / one expansion rule (a); init/disposal dissolve into §28.5/§22.13 on the visible field (§5.2) |
| §24.1 "The payload type may be any type." | stdlib §6.5, kept as a **normative** sentence (it is an admission, not guidance) |
| §24.1 "Records are strongly recommended" + §24.2 payload-record conventions | stdlib §6.5 guidance prose |
| §24.1 classes/interfaces-only placement rule; modifier prose | dissolve (§5.2) |
| §24.3 two-faced typing | deleted; the two declarations' types (§5.2) |
| §24.3/§24.4 `+=`/`-=` desugaring paragraph, including the load-bearing "`event += handler` does not read `event` and does not assign to it" | **spec [§17.2](../Bezel_Language_Spec.md#sec-notes-on-operators)** (§5.5) — it is a language-defined syntactic rewrite and must stay in the spec; C.10's and §25.1's cross-references retarget |
| §24.4 `Subscribe`/`Subscription` mechanics; zero-subscriber no-op | stdlib §6.5 (largely already stated there) |
| §24.4 construction-window caveat ("A raise cannot reach the handler before the constructor completes unless the constructor itself — directly or transitively — raises the subscribed event … observes `this` partially initialized") | stdlib §6.5, with the subscription/raise mechanics — event-specific temporal semantics justified by domain-seriality, not derivable from §28.14; **must move, not be paraphrased away** |
| §24.4's §27.7.1 store-once example | superseded by the in-flight §27.7.1 rewrite (seam-audit-01); post-demotion spelling per §5.1.3 |
| §24.5 snapshot-raise ordering (v0.163) | stdlib §6.5 |
| §24.5 `RaiseAsync` sequencing — "each handler's task is awaited before the next is invoked" | stdlib §6.5, **kept verbatim** (it is the observable difference between sequential-await and start-all-await-all) |
| §24.5 first-error-wins (rationale §13.5) | stdlib §6.5 |
| §24.5 cancellation-as-fault for async handlers ("If an asynchronous handler's task is cancelled, the behavior is the same as a fault … `OperationCanceledException` propagates from the awaited `RaiseAsync`") | stdlib §6.5, as a named clause beside first-error-wins (not subsumed by it) |
| §24.5 no-aggregation sentence ("To run all handlers regardless of failures and aggregate exceptions, the raiser must implement that policy explicitly") | stdlib §6.5 |
| §24.5 raising-domain rule (v0.180) | stdlib §6.5 |
| §24.6 thread-safety: instance sources domain-serial, unsynchronized (v0.163 reasoning) | stdlib §6.5, restated per type on `EventSource<T>` — this is what dissolves the spec-hot-04 contradiction (§7) |
| §24.6 FFI sentence ("External synchronization (`Mutex`) is required only where event operations are reachable from foreign threads through the FFI") | stdlib §6.5 thread-safety paragraph, kept as a distinct clause (not subsumed by domain-seriality) |
| §24.7 interface events | §5.4's interface property; auto-satisfaction dropped, counted in §9.1 |
| §24.7 payload-visibility sentence | dissolves into [§13.7](../Bezel_Language_Spec.md#sec-visibility-and-inheritance)'s general signature-visibility rule (the property type `IEvent<Payload>` carries it); §13.7's and Appendix C.2's "event payload types" list entries retire (§11) |
| §12.9 covariance worked example | stays in §12.9, retargeted at the stdlib declaration |
| v0.180 quiescence sentence ("the publisher's disposal severs the subscriber list before reclamation") | already stated in stdlib §6.5 against the type; unchanged |

### 5.4 Interface events become interface properties

`IfaceEvent` is deleted; the same contract is an ordinary get-only interface property:

```bezel
public record PropertyChangedEvent(Sender: Object, PropertyName: string);

public interface INotifyPropertyChanged
  readonly property PropertyChanged: IEvent<PropertyChangedEvent>;
end INotifyPropertyChanged;

public class Person: INotifyPropertyChanged
  private owned readonly _propertyChanged: EventSource<PropertyChangedEvent> := new();
  public property PropertyChanged: IEvent<PropertyChangedEvent> => _propertyChanged;
  // ...raise sites use _propertyChanged.Raise(...)
end Person;
```

External consumption is byte-for-byte what §24.7 shows today (`p.PropertyChanged.Subscribe(...)`). What is lost is auto-satisfaction — under §24.7 the single `event` declaration both satisfies the interface and provides the raise capability; here the implementer writes the property explicitly (counted in §9). What is gained: rationale §13.4's "one new grammar production and a paragraph of semantics" is refunded (§3 quotes and answers §13.4's "awkward workaround" sentence), and covariant exposure (`EventSource<Dog>` behind a `readonly property: IEvent<Animal>`) becomes directly declarable rather than a consequence of assignment contexts.

### 5.5 The `+=`/`-=` desugaring: keep it, and re-home it (sub-question Q3)

The desugaring is *already* type-keyed, not member-keyed (§1.1), so it survives option (b) mechanically unchanged: inside a class, `_clicked += OnClicked` desugars to `.Add(...)` exactly as the bare event name does today, the v0.180 `Add`-is-`scoped` analysis applies to the desugared call unchanged, and the v0.180 function-value-equality `-=` matching ([§27.7.1](../Bezel_Language_Spec.md#sec-equality-of-function-values)) is untouched. Under §7's S1, `SharedEventSource<T>` joins the type-keyed availability list (one clause; severable). Recommendation: **keep it** — it is access-site-independent, costs one paragraph (already written), and deleting it would churn every subscription call site for no uniformity gain; the mechanism this RFC deletes is the two-faced member typing, not a type-keyed convenience.

Keeping it under option (b) raises two obligations the first drafts left unstated:

- **The paragraph needs a home.** Its normative definition lives today in §24.3/§24.4, which option (b) deletes; stdlib §6.5's mention is descriptive and cites the spec for authority, and a language-defined *syntactic* rewrite (grammatically an `AssignmentStmt` per A.6, reinterpreted by LHS static type) cannot have its only normative statement in the stdlib under this project's document-role division. **Recommended home: [§17.2](../Bezel_Language_Spec.md#sec-notes-on-operators) ("Notes on operators")** — the section that already collects operator-level special cases — with [§6.6](../Bezel_Language_Spec.md#sec-compound-assignments) (whose subject is arithmetic compound assignment on primitives) explicitly *not* chosen, and the two existing cross-references updated: Appendix C.10's bullet ("The `+=` and `-=` syntactic forms on `EventSource<T>` and `AsyncEventSource<T>` are a fixed compiler desugaring, not operator overloads") and [§25.1](../Bezel_Language_Spec.md#sec-overloadable-operators)'s exclusion bullet, both of which today cite §24.
- **The `readonly`-LHS interaction must be stated wherever it lands.** The idiom's field is `readonly` (§5.1) and the idiom's producer-side subscription is `_clicked += OnClicked` — a compound-assignment *surface form* on a `readonly` field. This is legal only via the existing sentence "`event += handler` does not read `event` and does not assign to it" (no [§22.8](../Bezel_Language_Spec.md#sec-field-initializers) assignment occurs), an interaction that cannot arise today because events reject `readonly` and the generated field has no name. The relocated paragraph must carry that sentence and state that the desugaring is recognized *before* readonly-assignability checking; the retirement train carries a compiler test pinning `readonly`-field `+=`/`-=` (per the regression-test discipline). If the sentence were dropped or the check order reversed, the idiom's own two recommendations would contradict each other.

The uniformity-maximal alternative (delete it too; write `.Add`/`.Remove`/`.Subscribe`) is recorded as declinable, not recommended — **with one new consequence worth the owner's eye** (from the 2026-07-11 sweep, simplification-06): if Q3 is instead resolved as *delete*, then `Remove`'s reason to exist thins to explicit `.Remove` calls, and if those go too, §27.7.1's method-reference equality clauses (same-method + same-receiver, the value-shaped-receiver carve-out) can retire to plain reference equality — one sentence instead of a subsection — and the open implementation bug [#1618](https://github.com/s8lap/bezel-lang/issues/1618) (record-receiver method references compare equal across distinct instances) **closes by deletion**. That is a real simplification dividend sitting behind a "delete" answer to Q3; it is recorded here so the Q3 ruling is made with it on the table.

## 6. Option (a) — defined-by-expansion sugar with a **named** backing field

If the one-line spelling should survive, `event` remains in the grammar but §24 shrinks to a defined-by-expansion rule (the `for`-desugaring precedent, [§29.2](../Bezel_Language_Spec.md#sec-desugaring-of-for)):

```bezel
[vis] event E: T;
```

expands, by definition, to the tightened idiom (updated in the deepening to match §5.1 — `readonly`, `new()`):

```bezel
private owned readonly E_source: EventSource<T> := new();   // `protected` if the event is `protected`
[vis] property E: IEvent<T> => E_source;                    // omitted when [vis] is `private`
```

(async analogously; static per §7, and note the static-async gap below). `E_source` is an ordinary declared name: visible in source tooling, collision-checked like any member (declaring both `event E` and a member `E_source` is an ordinary duplicate-member error), and usable everywhere an owned field is. The minted name deliberately does **not** follow §1.3's `_camelCase` (there is no lexical transform from `E` to a leading-underscore name that survives round-tripping arbitrary identifiers; `E_source` is derived by suffix, the same convention the first draft used).

**Stated plainly: inside-class call sites change.** Under this expansion `E` names the *property* everywhere — including inside the declaring class — so every producer-side site in §24.5, §24.7, rationale §13, and user code changes from `E.Raise(...)`/`E += h` to `E_source.Raise(...)`/`E_source += h`. The sugar buys back only the declaration line, not the call sites. That is weaker sugar than the keyword is today, and it is the honest version.

**A grammar-level gap option (a) has and option (b) does not**: `static async event` is grammatical today (A.4 `EventDecl` admits both modifiers together), but under §7's recommended sync-only S1 scope it has **no expansion target** — the shared *async* pair is deferred. Option (a) must therefore either (i) reject `static async event` with a diagnostic naming the deferral (the honest v1 answer; no example in any of the five documents uses the form — verified by grep, 2026-07-11), or (ii) ship all four S1 types at once. This sub-ruling is folded into Q4(b). Option (b) has no such gap: the pattern simply cannot spell what the stdlib does not provide.

### 6.1 The rejected variant: bare-name two-way resolution

The "exact sugar" — `E` resolving to the backing *field* inside the class and to the *property* outside, so that existing `E.Raise(...)` sites keep compiling — is **rejected in advance**. It would make name resolution access-site-dependent in exactly the way §24.3 makes typing access-site-dependent today: the same simple name denoting two different declarations with two different types depending on where the reader stands. That is C#'s field-like-event model with the mechanism moved one layer down, it preserves every §1.2-style obligation on future member-walking rules (each must still ask "which `E` am I looking at?"), and it deletes nothing this RFC exists to delete. An earlier draft of this argument died on precisely this point; the position is closed, not open for a cleverer encoding.

### 6.2 Weighing (a) against (b)

Option (a) keeps: the one-line declaration, interface auto-satisfaction (`IfaceEvent` retained as expansion into §5.4's property), grep-ability of `event`. It still deletes: the two-faced typing, the hidden field, §28.14's event clause, and §24-as-semantics (the section becomes one expansion rule; semantics live in stdlib §6.5 as in §5.3). It keeps: two grammar productions, the record/enum placement rule (the expansion target must still be rejected there — though now the *expansion's field* is what §9.7 rejects, so the rule can be a plain consequence), and a member kind for tools to know about. It adds one wart the keyword doesn't have today: a compiler-minted *visible* name (`E_source`) — name-minting sugar is a mild smell (collision rules, refactoring surprise) that pure removal avoids — plus the `static async event` sub-ruling above. Hence the recommendation order: (b) over (a), (a) over status quo only if the declaration-line ergonomics are judged decisive *after* §5.1's tightening is priced in — the gap (a) closes is now one line and one name, not three lines and an import.

## 7. Static events — the explicit design point (deepened, and now carrying a live spec gap)

[§22.12](../Bezel_Language_Spec.md#sec-static-members) rule 5 (v0.163; strengthened v0.180) grants `static event` two guarantees keyed on the member kind:

> **Static events are runtime-synchronized and capture-free** (v0.163; strengthened v0.180): the compiler-generated subscriber list of a `static event` uses synchronized operations, since subscribe/raise may arrive from any domain. Additionally (v0.180), a handler subscribed to a `static event` — through `+=`/`Add` or `Subscribe` — must be **capture-free** [...] (Instance events remain unsynchronized and capture-admitting — domain-serial per [§24.6](../Bezel_Language_Spec.md#sec-thread-safety), captures per [§28.14](../Bezel_Language_Spec.md#sec-scoped-callback-parameters).)

A library-only design cannot condition synchronization or capture admission on *where a field is declared*: `EventSource<T>` is one type whether it sits in an instance field or a static one. A demoted `static owned readonly _e: EventSource<T>` would today draw §22.12 rule 4's declaration-site warning (class type outside the synchronized set) and would carry **neither** rule-5 guarantee.

**The gap that deepening this fork surfaced (#1647).** Consider the shape rule 5 must reject, written against the *current* spec:

```bezel
public static property Exiting: IEvent<()> => _exiting;      // the static event's outside face IS IEvent<()> today (§24.3)

// consumer, in some domain D:
using var sub := App.Exiting.Subscribe((_) => Log(name));    // captures D-local `name`
```

`IEvent<T>.Subscribe`'s parameter is `scoped` — a *storage-bounded, capture-admitting* position — so §28.14 form 2 happily admits the capture, bounded by `sub`. But the handler lands in a **process-global** list and will run on **whichever domain raises** ([§24.5](../Bezel_Language_Spec.md#sec-raising-events)), carrying a borrow of D's frame into a foreign domain — precisely the hole v0.180's capture-free rule closed, reopened by an alias. And this is **not a hypothetical about a proposed type — it is v0.183**: a static event member's outside face *is* `IEvent<T>`, nothing restricts passing that face as a value (a parameter, a local, §24.4's own `TickObserver` constructor), and at any `IEvent<T>`-typed subscribe site rule 5's "compile error naming this rule" is unenforceable — the site cannot know the source is static. Either rule 5 binds member-syntax sites only (then the guarantee is bypassable by one method-extraction refactor — a soundness hole), or it binds all sites (then it is undecidable, not implementable as the promised compile error). **Filed as [#1647](https://github.com/s8lap/bezel-lang/issues/1647)** during this RFC's adversarial review, with the repro shape and the fix space. Consequence for this fork: the capture-freedom requirement must be visible in **every static type through which a subscription can be written** — which no member-kind-keyed rule can achieve, and which S1 achieves by construction. If Q1 or Q4 is declined, #1647 still needs a resolution, and its member-kind-local options are strictly uglier: an alias restriction on static-event member accesses (a *new* carve-out of exactly the §1.2 genus) or a recorded downgrade of rule 5's guarantee. That asymmetry is decision input for Q4, not a footnote.

### 7.1 S1 (recommended): a synchronized, capture-free static-events *pair* — `ISharedEvent<out T>` + **sealed** `SharedEventSource<T>` (both **proposed**; names are the owner's)

The first draft proposed one class; the derivation above forces the subscribe-only face to be its **own interface** (not `IEvent<T>` — an upcast would launder the capture-free requirement through `IEvent.Subscribe`'s `scoped`, capture-admitting signature). The pair:

```bezel
public interface ISharedEvent<out T>
  method Subscribe(handler: (T) -> ()): owned Subscription;
      // capture-free admission: the handler must be a non-capturing lambda or a
      // static method reference — a stdlib-stated position in §28.14's capture-free
      // register (the same register Domain.Spawn's entries live in, §30.14),
      // NOT a `scoped` storage-bounded position.
end ISharedEvent;

public sealed class SharedEventSource<T>: ISharedEvent<T>
  public constructor();
  public method Subscribe(handler: (T) -> ()): owned Subscription;   // capture-free (as above)
  public method Raise(payload: T);       // synchronized snapshot-raise; handlers run on the raising domain
  public method Add(handler: (T) -> ());                             // capture-free (as above)
  public method Remove(handler: (T) -> ());                          // call-bounded: compares only
  destructor;                            // synchronized quiescence: severs the subscriber list under the
                                         // same lock discipline as subscribe/raise (see below)
end SharedEventSource;
```

`SharedEventSource<()>` additionally provides a convenience `Raise()` overload taking no arguments, equivalent to `Raise(())` — mirroring stdlib §6.5's exact wording for `EventSource<()>` (*"`EventSource<()>` provides a convenience `Raise()` overload that takes no arguments, equivalent to `Raise(())`"*). Without it, this RFC's own `_exiting.Raise()` examples would fail overload resolution.

**The retention mechanism, stated precisely (the design point a landing agent must not discover late).** §28.14's call-bounded default says a plain function-typed parameter may not be stored, with obligations *compiler-verified for user-authored callees*; the existing escape — "standard-library callees state their retention in their signatures" — is a register of **compiler-known, trusted stdlib callees** (`Domain.Spawn`, the §30.11 completion sources, the lazy query operators), none of which is a user-implementable contract. S1 therefore does **not** grant `ISharedEvent<T>` implementations a retention right; it works like this:

- **Call sites** are bound by the *compiler-known stdlib signature* they resolve against: any subscribe/`Add` site whose receiver's static type is `ISharedEvent<T>` or `SharedEventSource<T>` admits only capture-free handler arguments (non-capturing lambda or static method reference). This is call-site enforcement keyed on a stdlib-stated position — Domain.Spawn's mechanism — extended in exactly one way: the register's entry is an interface-declared position rather than a concrete method's. That extension is priced in the ledger (§9.10) and enumerated in §11.
- **The stdlib implementation** (`SharedEventSource<T>`, `sealed` — the stdlib already uses `sealed class` for `Type`, stdlib §2.13) is trusted to retain, as every register entry's implementation is; sealing means no derived class can inherit the trusted position.
- **User implementations of `ISharedEvent<T>`** are grammatically admissible but gain *no* retention right: a user-authored `Subscribe` body is verified under §28.14's ordinary call-bounded regime, so it can neither store its handler parameter nor forward it into a capture-free-register position (a parameter is not a syntactically capture-free argument form, so the forwarding call is rejected — conservatively and correctly). **Recorded scope bound: user classes cannot be cross-domain publication points in v1.** The recorded lift path is §28.14's own deferred third tier ([#1427](https://github.com/s8lap/bezel-lang/issues/1427) — "unbounded" parameters admitting only capture-free or transitively unbounded arguments, which the callee may then retain freely); if evidence ever demands user-authored shared publishers, that tier is the mechanism, and nothing in S1 forecloses it. (A formal "may not be implemented outside the stdlib" restriction was considered and declined: the language has no sealed-interface mechanism, and inventing one buys nothing — the ordinary call-bounded rules already make user implementations retention-incapable without any new rule.)

Design facts, each the type-level restatement of a rule-5 sentence:

- **`SharedEventSource<T>` does not implement `IEvent<T>`** — the laundering derivation above is the reason, recorded. This has a real expressiveness cost, counted honestly: today a static event's outside face *is* `IEvent<T>`, so it composes with every `IEvent<T>`-consuming API — §24.4's own `TickObserver(ticks: IEvent<TickEvent>)` can watch a static event. That composition is exactly the #1647 hole when the handler captures, but its capture-free subset is legitimate and is **severed** under S1 (counted as cost §9.10 in the ledger; the first draft's claim that "code generic over 'anything subscribable' does not exist" was wrong — `IEvent<T>`-typed parameters are that code). The bridge idiom for the lost use case: a domain-local relay — subscribe a **capture-free** forwarder to the shared source that routes payloads through a process-global `ChannelSender<T>` static (§22.12 rule 2 form-(i)); the consuming domain owns the `Channel<T>`, receives, and re-raises on a local instance `EventSource<T>`, whose `IEvent<T>` face then composes with anything. (Payloads crossing domains through the bridge must be §30.14-sendable — inherent to any cross-domain delivery, not an S1 artifact; handlers running on the raising domain never needed it.)
- **Covariance**: `ISharedEvent<out T>` is sound by the same composition argument as `IEvent<out T>` (T once, in the parameter position of a parameter — §12.9 / stdlib §6.5).
- **Synchronization**: the subscriber list is internally synchronized (subscribe/raise/`Subscription.Dispose` from any domain); raise takes the v0.163 snapshot; handlers execute on the raising domain; first-error-wins unchanged. **Disposal is part of the same discipline**: the destructor severs the subscriber list under the same lock as subscribe/raise (a concurrent subscribe either lands before the severance and is severed, or observes the disposed source per §6.5's existing disposed-source semantics); program-exit disposal of the recommended `static owned readonly` field runs per [§22.13](../Bezel_Language_Spec.md#sec-static-initialization-order) on the shutdown path with no other Bezel code running, which is what makes exit-time severance race-free. This is the type-level home for the machinery [#1392](https://github.com/s8lap/bezel-lang/issues/1392) (open: rule 5's synchronization is unimplemented) owes anyway — S1 discards no shipped code.
- **§22.12 membership**: `SharedEventSource<T>` joins rule 2's form-(i) **synchronized set** (and thereby rule 4's warning suppression), so `private static owned readonly _exiting: SharedEventSource<()> := new();` is admitted warning-free as a plain user-declared static. The recommended shape is `static owned readonly` (program-exit disposal per §22.13). Two side effects of that membership, recorded rather than discovered by a later sweep: (1) form-(i) membership's primary meaning is the **mutable-static admission**, so `private static _e: SharedEventSource<T>;` (non-`readonly`) becomes admissible with rule 2's v0.181 store semantics — atomic pointer store, ownership discharge, and **overwrite-never-disposes**, meaning a displaced source leaks with its subscriber list intact until process exit (handlers on it remain reachable; outstanding `Subscription.Dispose` stays safe per stdlib §2.11). Accepted: it is the same bounded cost rule 2 already accepts for displaced task caches, and the idiom's `readonly` avoids it entirely. (2) `SharedEventSource<T>` becomes the set's first **non-sendable** member — the set's admission text ("internally-synchronized *and safe to reach from every domain*") is satisfied via static reachability rather than §30.14 transfer, which is exactly the type's design (next bullet); the admission text needs no change, but the set's docs should note the distinction.
- **Sendability** ([§30.14](../Bezel_Language_Spec.md#sec-sendable-types)): **not sendable**, like `EventSource<T>`; *static reachability, not transfer, is its distribution mechanism* — one added list entry.
- **Payload posture unchanged**: no payload-sendability check, for rule 5's own recorded reason restated on the type — under capture-freedom plus raising-domain execution, each payload is constructed and consumed within one domain.
- **Capture-freedom is now checkable at every site by ordinary typing**: the subscribe site's static type is `ISharedEvent<T>`/`SharedEventSource<T>`, both compiler-known stdlib types whose handler positions are stated capture-free. No member-kind lookup, no "is this field static?" — the rule the checker enforces is written in the signature it already resolved. **This is what closes #1647 by construction**: there is no static type through which a capture-admitting subscription to a shared source can be written, so the alias problem cannot be posed. S1 is not merely the type-level home for rule 5's guarantees — it is the first *enforceable* statement of them.

The static idiom, complete:

```bezel
public static class App
  private static owned readonly _exiting: SharedEventSource<()> := new();
  public static property Exiting: ISharedEvent<()> => _exiting;

  public static method NotifyExiting()
    _exiting.Raise();
  end NotifyExiting;
end App;

// consumer, any domain — handler must be capture-free (static method ref / non-capturing lambda):
using var sub := App.Exiting.Subscribe(OnExit);
```

**Findings dissolved — and one that is not.** Two CONFIRMED findings from the 2026-07-11 verified sweep are, at root, costs of keying these guarantees on a member kind, and both dissolve under S1: (a) seam-audit-04 — the compiler-generated static backing field is unaccounted for by rules 2/4's "every static field" text and §30.14's `EventSource<T>` domain-locality entry; post-demotion the static field is *user-declared* with a type in the synchronized set, rules 2/4 answer directly, and the §30.14 list speaks per type. (b) spec-hot-04 — §24.6's blanket "The event subscriber list is not synchronized" contradicts rule 5; post-demotion §24.6 is deleted and the synchronization story is stated on each type (`EventSource<T>`: domain-serial, unsynchronized, by v0.163's recorded reasoning; `SharedEventSource<T>`: synchronized), so there is no cross-section sentence left to contradict. The interim reconciliation edits both findings received this run (TRAIN-STATIC) land independently and are superseded by this RFC's edits if it lands — coordination noted in both places. **But the interim edits cannot close [#1647](https://github.com/s8lap/bezel-lang/issues/1647)** — no member-kind-keyed reconciliation reaches alias sites; only S1 (or one of #1647's uglier member-kind-local resolutions, above) does. The owner should rule on Q1/Q4 with that asymmetry visible.

**Scope bounds, recorded**: (1) v1 of S1 is the **synchronous pair only**. `static async event` is grammatical today (A.4 `EventDecl`), but no example in any of the five documents uses it (verified by grep, 2026-07-11), and its shared twin (`ISharedAsyncEvent<T>`/`SharedAsyncEventSource<T>` with a synchronized `RaiseAsync` awaiting each handler's task on the raising domain) is mechanical when demand appears. Until then, a demoted static *async* event has no rule-5-equivalent home — it falls to S2's posture below; under option (a), `static async event` must be rejected with a deferral-naming diagnostic (§6). The owner may instead direct shipping all four types at once (Q4); the marginal cost is one more §6.5-scale stdlib entry, the marginal benefit is closing a corner no current code occupies. (2) User classes cannot be cross-domain publication points in v1 (the retention mechanism above; lift path #1427).

### 7.2 S2 (fallback): accept the rule-4 warning and the general static-singleton posture

No new type; static event sources are `static owned readonly` fields of an unsynchronized class, warned at declaration, cross-domain discipline the programmer's obligation like every other class-typed static singleton. This is *consistent* with rule 4's recorded posture but strictly weaker than what v0.180 shipped: the capture-free compile error (which closes a real hole — a capture-bearing closure transported to a foreign raising domain carries borrows of its origin domain's scopes; the now-closed [#1502](https://github.com/s8lap/bezel-lang/issues/1502) was a live defect in exactly this corner) degrades to nothing. Choosing S2 means explicitly recording the acceptance of that regression — and it leaves #1647 resolved only by the downgrade branch (rule 5's guarantee was never enforceable at alias sites anyway; S2 makes the non-guarantee official). This RFC does not recommend it.

### 7.3 S3 (forbidden): silently synchronize all `EventSource<T>` operations

This would restore static safety by taxing every instance event and would reverse the recorded v0.163 instance-events-unsynchronized choice ("continuations never run concurrently, so [...] no external synchronization is required", §24.6) without engaging it. Ruled out by construction, not offered to the owner. (It also would not close #1647's capture half — synchronization addresses the list, not the borrowed frames.)

Under option (a), the static expansion targets the S1 pair: `static event E: T;` → `private static owned readonly E_source: SharedEventSource<T> := new();` + `[vis] static property E: ISharedEvent<T> => E_source;` — rule 5 then deletes cleanly under either option, its content restated on the type.

## 8. Capture admission after demotion (§28.14)

The v0.180 storage-bounded admission is where a demotion RFC could silently break soundness or expressiveness; walked explicitly:

**Identical outcomes.** Inside the class: `_clicked += OnClicked` / `_clicked.Add(h)` — form 4, chain rooted at `this` through an `owned` field hop → bounding object `this`, self-capture admitted; byte-for-byte the analysis §24.3 (v0.180) prescribes for `this.Clicked += OnButtonClicked` today, with the event-member clause simply unneeded. External, the two idiomatic discharges: `using var sub := pub.Clicked.Subscribe(h)` → **form 2** (bounded by `sub` — the forms are ordered and form 2 wins before any chain question arises, exactly as §28.14's own example states for the keyword form); `_sub := pub.Clicked.Subscribe(h)` into an owned field of `this` → **form 3**. All three carry over with no rule change.

**One real narrowing.** A capture-bearing external subscription whose `Subscription` is discharged some *other* way (bound to a plain `var` owned local, passed straight into a collection, etc.) resolves its bounding object today through form 4's event-member hop (`pub.Clicked` → chain head `pub`). Demoted, `pub.Clicked` is a **getter** hop, and form 4 says a getter resolves *no* bounding object (capture-free only) — the getter clause exists because a general getter can return a borrow of something the chain head does not own. So these off-idiom sites lose capture-bearing admission. Assessment: small surface (the owned `Subscription` must be discharged *somewhere*, and the two idiomatic discharges are forms 2/3), correct-by-default (rejection, never unsoundness), with a good diagnostic story ("bind the subscription with `using var`"). A **liftable** companion amendment is recorded for the owner: extend form 4's hop set with *transparent getters* — an expression-bodied getter whose body is a bare read of an `owned` field of its receiver (structural, decidable, and precisely the §28.6-bullet-1 shape). Note that §5.1's idiom property is *exactly* this shape, so the lift restores form-4 parity for every event declared by the idiom, verbatim. Recommended: ship without it, lift on evidence, matching §28.14's own "v1 admission bounds / liftable" discipline (Q5).

**Net simplification.** Form 4's "or a compiler-generated event backing member" clause and its §24.3 cross-reference are deleted; the registration-lifetime residual's event mention retargets to the named field it already describes in owned-field terms. Under Q4 = S1, the call-bounded paragraph's enumeration of stdlib-stated retention positions gains one entry — the shared-event handler positions, in the capture-free register (§7.1's mechanism; enumerated in §11).

## 9. Honest costs

Counted against the keyword form, per event, *after* the §5.1 tightening:

1. **Lines**: 1 → 2 for a public instance or static event (field + property); 1 → 1 for `private`/`protected` events; interface implementations additionally write the property §24.7 currently synthesizes (§5.4). Two is the floor (§5.1.1), not a shortfall of the idiom. Under option (a): 1 → 1, at the price of a minted name (§6.2).
2. **Two names where there was one.** Reframed by the tightening: `_clicked`/`Clicked` is the language's own `_balance`/`Balance` backed-property stereotype ([§1.3](../Bezel_Language_Spec.md#sec-identifiers) convention, §23's examples), not a new convention to learn. The genuine loss stands: producer-side call sites read `_clicked` where they read `Clicked` today, and the two-faced typing's virtue — one name, capability decided by the type system — is *given up*, not refuted. (Its cost, §1.2, is the other pan of the scale; weighing them is the ruling.)
3. **A mandatory `import System.Threading` in every event-declaring file** — **dissolved if Q7 is accepted** (§2.4, two precedented spellings); otherwise stands as the first draft counted it. Subscriber-side files stay import-free under every option.
4. **Wiring is manual.** Forgetting `owned` on the field or the initializer is on the user — both are loud compile errors today (§28.6 fresh-owned-into-non-owned; definite-assignment), not silent misbehavior. The idiom's `readonly` (§5.1) removes the one *silent* hazard the first draft left open (accidental reassignment severing subscriptions via §28.7 drop-on-overwrite).
5. **Self-documentation and tooling.** `event` is grep-able and IDE-listable as a member kind; a convention (`EventSource` field + `IEvent` property) is softer. Mitigation beyond documentation: `EventSource<` is itself a precise grep, and the §5.1 idiom is rigid enough for tooling to recognize as a stereotype.
6. **Migration** (§11): every `event` declaration and every producer-side call site in the corpus. The two guides contain **zero** event usages (re-verified 2026-07-11); the blast radius is spec §24's own examples, rationale §13, and stdlib §6.5 cross-text.
7. **Static events need §7's resolution** — under S1, two new stdlib types (sync pair; the async twin deferred, with the deferral's consequences recorded in §7, including option (a)'s `static async event` rejection) plus the severed `IEvent<T>` composition (§7.1, §9.10); under S2, a recorded guarantee regression. Either way #1647 must be resolved; declining both S1 and the demotion leaves its ugliest resolutions (§7).
8. **The §28.14 off-idiom narrowing** (§8), absent the liftable amendment.
9. **No-payload events write `()`**: `event Closed;` → `EventSource<()>`. One token of honesty about what the payload always was; no semantic loss.

And the honest gains, symmetrically: the language's only access-site-dependent member typing deleted; two grammar productions and the §1.2 carve-out family deleted or dissolved; the two verified static-event seam findings dissolved rather than patched, and #1647 closed by construction rather than by a new carve-out (§7); every future member-walking rule freed of the "and what about events?" obligation; events become almost entirely library semantics (the self-hosting direction, #545); standalone sources constructible (§2.3); Core implementations shed the entire event member machinery (§12).

### 9.10 The consolidated ledger (Q1's crux in one table)

Under the recommended bundle — option (b) + Q3 = keep + Q4 = S1 (sealed, stdlib-only retention) + Q7 = (i):

| Deleted / dissolved | Added |
|---|---|
| `EventDecl` + `IfaceEvent` (2 grammar productions) | 2 stdlib types (`ISharedEvent<out T>`, sealed `SharedEventSource<T>`) — semantic text largely `EventSource<T>`'s with the synchronization/capture-freedom deltas |
| §24.3 two-faced typing — the language's only access-site-dependent member type rule | 2–4 names added to §2.3's always-available list + its justification sentence reworded (Q7-i) |
| §24 as a spec section (semantics relocated per §5.3's checklist, not lost) | 1 new member of §22.12 rule 2's form-(i) set, with the mutable-static and first-non-sendable-member side effects recorded (§7.1) |
| §24.1 placement rule + modifier prose | 1 §30.14 not-sendable list entry |
| §28.14 form 4's event clause + §24.3 cross-reference | 1 entry in §28.14's stdlib-stated retention register — its first *interface-position* entry (§7.1's recorded extension) |
| §22.12 rule 5 (restated enforceably on the S1 types; #1647 closed by construction) | the `+=`/`-=` desugaring paragraph re-homed to §17.2 with the `readonly`-LHS sentence (moved + 1 sentence, not new semantics) |
| interface-event auto-satisfaction (§24.7) | relocated §24 semantics in stdlib §6.5 (moved, not new) |
| §13.7's + C.2's "event payload types" list entries | — |
| seam-audit-04 + spec-hot-04 (dissolved, not patched) | — |
| ~80 compiler references to the event member machinery (§13) | S1's synchronized subscriber list — the work #1392 owes anyway, rehomed |

No new syntax anywhere in the recommended path; the "zero new language surface" claim holds for the idiom *and* for the proposal, with one precisely-scoped exception recorded: the §28.14 retention register's interface-position extension (row 5). Had B1 been resolved via #1427's third parameter tier instead, the additions column would gain a new parameter marker, signature-identity rule, and callee verification regime — that is why the stdlib-only resolution is part of the recommendation, not an implementation detail. Under option (a), add back: 2 productions, the expansion rule, the minted-name rules, and the `static async event` rejection (§6).

## 10. Consequential stdlib §6.5 rewording (part of this RFC, either option)

Making the constructor public falsifies two load-bearing sentences, which must be reworded *with* the change, not discovered after:

- `// No public constructor; obtained via event declaration.` → `public constructor();` (both classes), with the fence paragraph retargeted from *obtainability* to *reachability*: the producer surface of a given event is fenced by the **field's** visibility — an `EventSource<T>` reference for someone else's event is reachable only if their class exposes it **or a holder downcasts the interface face** (`face as EventSource<T>`), which is the recorded, not-defended-against breach: a type-driven fence is exactly as strong as the type system's static view, and downcast is the static view's built-in escape. This porosity is not new — the runtime object behind today's keyword-form face *is* the source, and the same downcast works — but it becomes more discoverable once the type is nameable (#1536, landed) and constructible (§2.3), so the reworded paragraph names it instead of implying leak-proofness. Third-party construction of a *fresh* source grants no capability over anyone else's events. (Ride-along while this paragraph is open: the smuggling sentence's "or by reflection" names a mechanism Bezel declines entirely — flagged by the sweep (seam-audit-15/spec-hot-23); drop the clause or substitute "or through the FFI".)
- *"Since `EventSource<T>` is visible only inside the declaring class, the user-facing variance comes from `IEvent<out T>` exclusively"* (the invariance justification) → the variance *facts* are unchanged (`Raise(payload: T)` keeps the producer classes invariant; `IEvent<out T>` stays sound by the §12.9 composition argument, quoted in both documents). The justification's framing becomes: the *conventional public surface* is the interface, so user-facing variance is expected through `IEvent<out T>`; an exposed `EventSource<T>` is simply invariant, as its signature dictates.

Under option (b), §6.5 additionally absorbs §24's semantics per §5.3's checklist, gains the idiom as its leading example (the two §5.1 lines), and the "obtained via event declaration" framing disappears wholesale. Under Q7(ii) the whole section relocates to the `System` chapter; under Q7(i) it stays put and §2.3's list grows.

## 11. Migration

- **Spec**: §24 deleted (b) or reduced to the expansion rule (a); Appendix A.4 productions; the `+=`/`-=` desugaring paragraph relocated to [§17.2](../Bezel_Language_Spec.md#sec-notes-on-operators) with the `readonly`-LHS sentence added and the Appendix C.10 + [§25.1](../Bezel_Language_Spec.md#sec-overloadable-operators) cross-references retargeted (§5.5); the `event` keyword itself (Appendix B.1, §1.4 — unconditionally reserved today): under (a) it keeps its meaning; under (b) recommended **kept reserved-but-unused** — one word held out of the identifier namespace at zero lexer cost, keeping option (a) or a future re-introduction reachable — with un-reserving recorded as a separately declinable follow-up, **and Appendix B.3 reconciled**: B.3 currently states "This version of the language does not reserve any identifiers for future use," which a reserved-but-unused `event` would falsify — either move `event` to B.3 with its rationale or amend B.3's claim (one sentence, but a normative-list edit, named here so it isn't misfiled as prose); §22.12 rule 5 per §7 (plus one type name added to rule 2's form-(i) set under S1, with §7.1's two recorded side effects); §28.14 form 4 per §8 **and** the stdlib-stated retention register's new shared-event entry (§7.1/§8); §2.3 per Q7 **including the justification-sentence rewording** (§2.4 — the sentence keying `IEvent<T>`/`IAsyncEvent<T>` auto-import on §24.3 dangles under (b) and must be re-keyed to the §6.5 pattern's property); [§13.7](../Bezel_Language_Spec.md#sec-visibility-and-inheritance)'s and Appendix C.2's "event payload types" list entries retire (dissolved into property types) under (b), or stay as redundant under (a); §30.14 list entry per §7; Appendix C.11's static-event bullet restates onto the S1 type; Appendix D per §12; the `event`-mentioning prose lines outside §24 (26 word-boundary matches at drafting; substring counts run higher — refresh at landing) reviewed, of which the producer-side subset is re-spelled (declarations to the §5.1 idiom; `E.Raise` → `_e.Raise`) — subscription-side examples, the majority, are unchanged.
- **Rationale**: §13 gains this decision's record (including §5.1.2's declined-candidates table, the Q7 posture reversal, and §7.1's retention scope bound); §13.1/§13.4 examples re-spelled; §13.4's "awkward workaround" sentence engaged per §3; no recorded reasoning is invalidated (§3).
- **Stdlib**: §6.5 per §10; §7's S1 pair if chosen (with the `Raise()` overload and destructor/quiescence lines of §7.1 — a landing agent copying §7.1's block gets a complete surface); coordination with the in-flight stdlib-2-04 `destructor;` edits (§5.3).
- **Guides**: no changes (zero event usages, re-verified).
- **Compiler/tests**: the event member machinery (two-faced member typing, generated backing fields, `IfaceEvent` satisfaction) retires or (a) becomes an early desugaring; the runtime `EventSource` machinery is untouched; the retirement train carries the §5.5 `readonly`-`+=` regression test. The open event-machinery bugs map cleanly: [#1646](https://github.com/s8lap/bezel-lang/issues/1646) (user-constructed sources miscompiled — found by this deepening's probe) becomes the demotion's *front-door* path and must be fixed first under any Q6-or-better outcome; [#1647](https://github.com/s8lap/bezel-lang/issues/1647) (static-event capture-freedom unenforceable through aliases — found by this RFC's adversarial review) closes by construction under Q4 = S1, or needs its own member-kind-local resolution if the RFC is declined (§7); [#1392](https://github.com/s8lap/bezel-lang/issues/1392) resolves into S1's type (or S2's deletion); [#996](https://github.com/s8lap/bezel-lang/issues/996) becomes an ordinary owned-field/Subscription lifetime bug with no member-kind involvement; [#1618](https://github.com/s8lap/bezel-lang/issues/1618) either remains (Q3 = keep) or closes by deletion (Q3 = delete; §5.5). The historical trail (#355, #477, #479, #480, #880, #1064 — and now #1646, which is a fresh instance of the #477 shadow-source seam) is a record of defects specifically in the member-kind seams — evidence that the special casing has carried real implementation cost, offered as weight, not proof.
- **User code**: none exists (pre-users project).

## 12. Conformance (Appendix D)

Today's [D.2](../Bezel_Language_Spec.md#sec-extended-features) row ties the Events feature to §24 plus the stdlib types. Under option (b) the row becomes stdlib-only (the model is D.2's "Sequence operations" row, which lists no spec sections at all): a Core implementation omits the §6.5 types per [D.3](../Bezel_Language_Spec.md#sec-feature-dependencies)'s existing mechanism ("A Core implementation's standard library omits the types of unimplemented Extended features entirely [...]; programs referencing them fail name resolution with the feature-naming diagnostic of D.1") — no event grammar, member logic, or rejection rule to implement at all. D.3's dependency bullet ("**Events**' async event source requires **Asynchronous programming**; the synchronous `EventSource<T>` does not") carries over verbatim. The S1 pair, if adopted, **joins the Events row's stdlib column** (D.2 has no separate domains row — domains live inside the "Asynchronous programming" row's "§30 entire — including domains, spawning, and channels"), and D.3's bullet gains one clause, which this RFC states rather than leaving to the landing agent: **`SharedEventSource<T>`/`ISharedEvent<T>` are available in Events-without-Async** — the capture-free admission is ordinary §28.14 typing (Core machinery) and binds identically, while the synchronization guarantee is vacuously satisfied in a single-domain build (rule-5-equivalent guarantees hold trivially); the deferred async twin, if ever shipped, requires Asynchronous programming like `AsyncEventSource<T>` does.

## 13. Sizing

| Piece | Size |
|---|---|
| Spec edit | Moderate — §24 deleted (b) or reduced to one expansion rule (a); two Appendix A.4 productions; §22.12 rule 5 restated on the S1 type + one synchronized-set name + two recorded side effects (§7); §28.14 form 4's event clause deleted + one retention-register entry (§8); the desugaring paragraph moved to §17.2 + one `readonly`-LHS sentence + C.10/§25.1 cross-refs (§5.5); §2.3 list edit + justification rewording (Q7-i) or relocation (Q7-ii); §13.7/C.2 list entries; B.3 reconciliation; §30.14 one entry; Appendix C/D per §11/§12; the `event`-prose lines outside §24 (26 word-boundary at drafting) reviewed, the subscription-side majority unchanged |
| Stdlib edit | Moderate — §6.5 constructors + the §10 rewording + absorbing §24's semantics per §5.3's checklist under (b) + the §5.1 idiom as the leading example; plus the S1 pair (two §6.5-scale entries incl. `Raise()` overload + destructor lines) if Q4 = S1; relocation instead of growth if Q7 = (ii) |
| Rationale edit | Small — §13 gains this decision's record incl. §5.1.2's table and §7.1's scope bounds; §13.1/§13.4 examples re-spelled; the Q7 posture reversal recorded; no recorded reasoning invalidated (§3) |
| Guides | None (zero event usages, verified) |
| Compiler | **#1646 first** (make user-constructed sources work — required by Q6 even if Q1 is declined; #1536 already landed). Then net-negative: ~80 references to the event member machinery (`EventSymbol`/`EventDecl`/`IfaceEvent`) across parser/sema/flow/ownership/codegen retire under (b) or collapse into an early desugaring under (a). S1's synchronized subscriber list is the one genuinely new runtime piece — the work [#1392](https://github.com/s8lap/bezel-lang/issues/1392) owes anyway, rehomed onto a type. #1647's enforcement comes free with S1 (the check is ordinary signature-driven admission) |
| Tests | ~38 files / ~162 `event`-declaration-ish sites at drafting (approximate — refresh at landing) re-spelled to the idiom (mechanical); the member-kind seam regression tests retire with the seams they guard; #1646, S1 (incl. the capture-free admission at interface-typed sites), the §5.5 `readonly`-`+=` case, and the §8 narrowing gain new spec-derived cases |
| Sequencing | §2.3 + #1646 + §10's rewording can land ahead of and independently of the ruling (Q6); Q7 can land with them; the demotion itself is one docs revision + one compiler retirement train, with S1 landable in the same stdlib revision. The 2026-07-11 interim edits (spec-hot-04, seam-audit-04 reconciliations; §27.7.1 rewrite) land regardless and are superseded by, not in conflict with, this RFC's edits — but they do **not** close #1647, which waits on Q4 (or on its own uglier fix if the RFC is declined) |

## 14. Owner questions

1. **Q1 — the ruling**: demote at all? This is the ergonomics-vs-uniformity trade of §9 vs §1.2/§2.2 — see §9.10's one-table ledger; the finding is CONFIRMED but the demotion is a **choice, not a fix**. Recommended: yes — the §5.1 tightening plus Q7 shrink the ergonomic side of the scale to: one extra line per public event, producer sites reading `_clicked` for `Clicked`. New since the first draft: declining Q1 *and* Q4 leaves [#1647](https://github.com/s8lap/bezel-lang/issues/1647) to be fixed with a new member-kind carve-out or a recorded guarantee downgrade (§7).
2. **Q2 — shape**: option (b) removal, or option (a) named-backing-field expansion? Recommended: (b). (The bare-name variant of (a) is not on the table — §6.1. Option (a) additionally requires the `static async event` sub-ruling — §6.)
3. **Q3 — `+=`/`-=`**: keep the type-keyed desugaring, re-homed to §17.2 with the `readonly`-LHS sentence (recommended, §5.5), or delete it with the member kind? New input for this ruling: deleting it (and `Remove`) would let §27.7.1's method-reference equality clauses retire to reference equality and closes [#1618](https://github.com/s8lap/bezel-lang/issues/1618) by deletion (§5.5) — a real dividend, recorded, though not enough to flip the recommendation against churning every producer-side subscription site.
4. **Q4 — statics**: S1 (the sealed `ISharedEvent<out T>` + `SharedEventSource<T>` pair, capture-free-by-signature and synchronized, stdlib-only retention; recommended — and the only offered design that closes #1647 by construction) or S2 (rule-4 warning, recorded guarantee regression, #1647 resolved by downgrade)? S3 is not offered. If S1: (a) confirm the two type names; (b) sync pair only (recommended; the async twin deferred with its gaps recorded — §7's pattern-level gap and §6's option-(a) grammar-level gap) or all four types now; (c) confirm the recorded scope bound that user classes cannot be cross-domain publication points in v1 (sealed class, no user retention; lift path [#1427](https://github.com/s8lap/bezel-lang/issues/1427) — §7.1).
5. **Q5 — §28.14 transparent-getter hop**: adopt with the demotion, or record as liftable and ship without (recommended)? Note the lift now restores form-4 parity for *every* idiom-declared event, since §5.1's property is exactly the transparent-getter shape (§8).
6. **Q6 — severed floor**: if Q1 is declined, adopt §2.3 (public constructors + the #1646 fix + §10's rewording) anyway? Recommended: yes — it costs nothing, unblocks spec-legal code, and stands on its own. (#1647 is orthogonal to Q6 and needs an answer under every outcome — §7.)
7. **Q7 — producer-type availability**: dissolve the per-file `import System.Threading` by (i) adding `EventSource<T>`/`AsyncEventSource<T>` (+ S1 pair) to §2.3's always-available names (CancellationToken precedent; recommended), (ii) relocating the types to `System` (Subscription/v0.76 precedent), or (iii) declining and keeping the import as cost §9.3? This deepening recommends (i) and records the reversal of the first draft's contrary posture (§2.4, §3). Either accepted spelling carries §2.4's justification-sentence rewording under option (b).

---

## Changes from the previous draft

Deepened 2026-07-11 at the owner's direction ("the declaration is not as clean as I would like — can it be made even simpler?"), against v0.183, with all citations re-verified and probes re-run; then finalized the same day against an adversarial critique of the deepened draft. Substantive changes:

1. **The recommended idiom is rewritten** (§5.1): `private owned readonly _clicked: EventSource<ClickedEvent> := new();` + `public property Clicked: IEvent<ClickedEvent> => _clicked;`. Three changes vs the first draft's idiom, each an *existing* mechanism the draft failed to use: the [§17.7](../Bezel_Language_Spec.md#sec-the-new-operator) `new()` shorthand (deletes the doubled `EventSource<ClickedEvent>` spelling), [§22.8](../Bezel_Language_Spec.md#sec-field-initializers) `readonly` (restores the keyword's de-facto immutability; turns the silent §28.7 drop-on-overwrite subscription-severing hazard into a compile error), and [§1.3](../Bezel_Language_Spec.md#sec-identifiers)'s `_camelCase` convention (`_clicked`, replacing the convention-violating `Clicked_source`). Zero new language surface.
2. **New §5.1.1**: a floor argument — under declaration-fixed types, the type fence, and no-runtime-type-tests-on-the-producer-path, a public event needs two declarations; the four escapes are enumerated (including the downcast escape, C10, which the critique showed the first version of the argument omitted) and each is a design this RFC rejects with reasons.
3. **New §5.1.2**: the declaration-surface decision record — ten candidates scored and declined with reasons, per the house decline-with-reasons rule.
4. **New §5.1.3**: complete before/after site table, aligned with the sweep-verified finding seam-audit-01 (the current §27.7.1 field-stored-lambda pattern is already ill-formed under §28.14 bound 3; the post-demotion idioms shown are the ones its in-flight rewrite lands on), with the class-keyed-privacy note on cross-instance producer access.
5. **New §2.4 + Q7**: the mandatory `import System.Threading` in event-declaring files is addressed head-on — recommended dissolved via §2.3 auto-import (CancellationToken precedent) or `System` relocation (Subscription/v0.76 precedent). This **reverses the first draft's recorded "declinable knob, not recommended" posture**; the reversal and its honest grounds (the keyword's import exemption dies with the keyword; the owner's cleanliness directive) are recorded in §2.4 and §3, with the §2.3 justification-sentence knock-on enumerated in §11.
6. **§7 (statics) reworked twice**: first deepened into a pair — `ISharedEvent<out T>` + `SharedEventSource<T>` — with the derivation of why the shared source must **not** implement `IEvent<T>` (an upcast would launder the capture-free requirement through `IEvent.Subscribe`'s `scoped` signature); then finalized on the critique: the derivation's hole is **live in v0.183** and was filed as [#1647](https://github.com/s8lap/bezel-lang/issues/1647) (S1 is its only by-construction fix; the interim TRAIN-STATIC edits cannot close it); the class is **sealed** with the retention mechanism stated precisely (stdlib-stated capture-free register, first interface-position entry; user implementations retention-incapable under ordinary call-bounded verification — recorded scope bound, lift path #1427, so no new parameter tier is required); the surface completed (`Raise()` overload on `SharedEventSource<()>`, `destructor;` with synchronized quiescence and the §22.13 exit-disposal argument); the rule-2 form-(i) side effects recorded (mutable-static admission with overwrite-leak semantics; first non-sendable member); and the severed `IEvent<T>` composition counted as a cost with its channel-relay bridge idiom named — correcting the deepened draft's false claim that no `IEvent<T>`-generic code exists.
7. **§1.3 probe results replaced**: #1536 (nameability) has landed and is CLOSED; the manual pattern now fails *downstream* — user-constructed `EventSource<T>` is admitted contra stdlib §6.5 and miscompiled (silent no-fire via ctor assignment; ICE #1386 via field initializer). Filed as [#1646](https://github.com/s8lap/bezel-lang/issues/1646); preconditions, §11, and §13 updated accordingly.
8. **§5.5/Q3 updated twice**: the sweep finding simplification-06 recorded (a "delete" answer to Q3 collapses §27.7.1 to reference equality and closes #1618 by deletion); then, on the critique, the kept desugaring was given a **home** — relocated to [§17.2](../Bezel_Language_Spec.md#sec-notes-on-operators) with C.10/§25.1 cross-references retargeted — and its previously-unstatable `readonly`-LHS interaction stated (the "does not read, does not assign" sentence must survive and precede readonly-assignability checking; regression test required).
9. **§5.3 rewritten as an explicit relocation checklist** (on the critique): every §24 sentence-level rule now has a named destination, including six the deepened draft's umbrella phrases dropped — the construction-window caveat, async-handler cancellation-as-fault, the no-aggregation sentence, the FFI-synchronization clause, the verbatim `RaiseAsync` sequencing sentence, and the normative any-payload-type admission.
10. **Option (a)'s expansion target updated** (§6) to the tightened idiom, with the minted-name note — and, on the critique, the `static async event` grammar-level gap named with its two resolutions folded into Q4(b).
11. **§9 costs re-counted and consolidated**: the deepened re-count (import cost conditional on Q7; the two-names cost reframed as the language's own backed-property stereotype; explicit `()` for no-payload events; `readonly` closes the draft's open wiring hazard) plus, on the critique, the severed static-event composition cost, refreshed corpus counts marked approximate-at-drafting (38 files / ~162 sites; 26 word-boundary spec lines), and a **new §9.10 one-table ledger** making Q1's "is this still net deletion?" answerable at a glance, with the S1-variant dependence explicit.
12. **§3 recorded-decisions section extended** (on the critique): rationale §13.4's "awkward `GetEventX()` workaround" sentence — the closest recorded adverse judgment on §5.4's shape — is quoted and answered rather than skirted; §28.14's #1427 deferral added as an engaged recorded decision.
13. **§10's fence rewording strengthened** (on the critique): the downcast is named as the always-available, recorded, not-defended-against breach — retargeting the paragraph from obtainability to reachability — instead of repeating the leak-only framing; §2.1 aligned.
14. **§11/§12 completed** (on the critique): B.3's "no reserved identifiers" claim reconciled with the reserved-but-unused recommendation; §13.7's + C.2's "event payload types" list entries enumerated; the §28.14 retention-register entry enumerated; D.2 slotting corrected (the S1 pair joins the Events row — there is no "Multiple-domains" row) and the D.3 dependency stated (available in Events-without-Async, synchronization vacuous in single-domain builds).
