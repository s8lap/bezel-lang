# Proposal 0140 — The method-reference formation bar exempts `out` parameters, and a function-typed `ref`/`out` location's post-call set

| | |
|---|---|
| **Status** | graduated-v0.327 — superseded — graduated v0.327 (accepted 2026-10-08 as a delegated ruling under the owner's direction "resolve and decide all known spec related issues"; critique round 1 applied, 3 blocking and 7 non-blocking findings resolved, shared with proposal 0136's S2-4; landed with the v0.327 changelog entry, together with S2-4, which waited on it) |
| **Tracking issue** | [#5526](https://github.com/s8lap/bezel-lang/issues/5526) (the question); the compiler trackers are [#5515](https://github.com/s8lap/bezel-lang/issues/5515) (the widened bar, v0.320) and [#5145](https://github.com/s8lap/bezel-lang/issues/5145) (the `ref`/`out` location rule's call-site check, v0.309), each annotated at landing |
| **Target module(s)** | **spec** — §28.14 (*The method-reference formation bar*, one sentence added and one qualified), §27.6 (the mirror sentence, one qualifier), §28.3 (*Set-erasing positions*, the bullet S2-4 lands beside *Function-typed holders*: one sentence on `out` locations; *A `ref` or plain `out` location outlives the call*: one sentence on function-typed locations), Appendix C.3 (the formation-bar row, one qualifier); **rationale** — §4.2, one decision paragraph after *`scoped` is not admitted on `Object`*; **code-generation reference** — §4.4's *Closures by position* follow-up sentence and its `ref`/`out` bullet (in-place corrections, no pin); **`DECLINED.md`** — three `rejected` rows |
| **Written against** | spec/stdlib/rationale/changelog **v0.325**, HEAD `b27728a` (main). The v0.326 train (the guide re-trace and the §28.3 *Bare-type-parameter returns* clause rulings) lands before this one; every anchor below is re-taken at composition. |
| **Origin** | The v0.320 critic's finding N3 on proposal 0133, filed as #5526 with `needs-owner-decision` because it amends owner-accepted text with no measured want; the v0.325 lane `qb` recorded the adjacent post-call over-approximation on the same issue "for the same owner pass". |

*Reference convention: unprefixed §-references are language-spec references at `b27728a` (v0.325); K tags are §28.3's kernel ids. Quotations are verbatim. ▲ marks proposed text. Every program below uses only members the stdlib companion declares (`Dictionary<K, V>.TryGetValue(key: K, out value: V?): bool`, stdlib §4.8; `Array<T>.Add(item: T)`, §4.2).*

---

## 0. Summary

Two decisions, one ground.

1. **The formation bar exempts `out` parameters.** §28.14's widened method-reference formation bar (v0.320) rejects a reference whose formed function type has a function-typed slot bound to a set-erasing callee parameter. It keys on *every* such slot, so `var tg := d.TryGetValue;` at `d: Dictionary<string, () -> int>` is a compile error although the direct call `d.TryGetValue(k, out var g)` is admitted and `g` is capture-free. The bar's own ground — a call through the formed value hands the callee a caller-supplied argument at a position that announces no set — does not reach an `out` slot, through which no value of the caller's flows. The bar now keys on the slots a caller's value reaches, plain and `ref`, and exempts `out`.
2. **A function-typed `ref`/`out` location carries, after the call, the empty set (`out`) or its prior set (`ref`).** §28.3's location rule gives every `ref`/plain-`out` location the union of the call's borrows after the call. For a location whose substituted type is a function type the callee can store only a capture-free value (the location is a function-typed holder, v0.324/v0.325), so the union over-approximates: it rejected `d.TryGetValue(k, out this._g)` and a later field store of an `out`-filled function value for a borrow the location can never hold. The location rule now states the function-typed case, and the admissibility check at the call is vacuous there.

Both are widenings: no program that compiles under v0.325 stops compiling. Measured reach: zero sites in `tests/`, `docs/` and the conformance corpus form a method reference over a member with an `out` parameter at a function-type substitution, and zero pass a function-typed `out` location into a field. Class: design change (an amendment to owner-accepted proposal 0133's text), landed under the delegated ruling, ruling-class gate. Kernel: derivation (K2.3 + K2.4), unchanged from 0133. Body-locality: local.

---

## 1. The case

### 1.1 Decision 1 — the bar over `out`

The widened bar (§28.14, v0.320):

> Forming a method reference is likewise a compile error when a parameter slot of the formed function type is, nullability stripped, a function type and binds to a callee parameter that is a set-erasing position — one declared `Object`, or at a bare type parameter that the formation's substitution makes a function type (`var add := xs.Add;` at `xs: Array<() -> int>`; `var keep: (() -> int) -> () := k.Keep;` over `Keep(o: Object)`): a call through the formed value passes its argument to a plain function-typed parameter, which the call-bounded default's caller side admits whatever its set, and the callee would receive it at a position that announces none.

Program A (rejected today, admitted by this proposal):

```
using var d := new Dictionary<string, () -> int>();
d.Add("seven", Util.Seven);              // capture-free: admitted at the set-erasing `value: V`
var tg := d.TryGetValue;                 // v0.325: ERROR — the `out (() -> int)?` slot binds to `value: V?`, a set-erasing position
var g: (() -> int)? := null;
if tg("seven", out g) then Console.WriteLine(g!()); end;
```

The callee parameter `value: V?` is a set-erasing position (an `out` location declared at a bare type parameter, at a function-type substitution), and the formed slot `out (() -> int)?` is, nullability stripped, a function type, so the bar fires. What the bar protects is stated in its own sentence: *a call through the formed value passes its argument to a plain function-typed parameter … and the callee would receive it at a position that announces none*. At an `out` slot no argument is passed: the caller supplies a location (§22.3: *an `out` parameter is the callee's slot for the call, copied out on normal return*), so the slot is the callee's and not an alias of the caller's location, and the caller's prior value never reaches the callee. The hazard the bar closes — a capture-bearing value laundered into a holder through a formed type that announces nothing — cannot arise at that slot.

What the callee copies *out* is capture-free. This is the soundness argument of proposal 0133 read in the callee's direction: every value of the type parameter's type that the generic body can place in a holder, a return slot or a `ref`/`out` location arrived through a set-erasing position (a plain or `ref` parameter, or a `T`-typed field, component, payload or slot, each of which admits only a capture-free function value at a function-type substitution), through an invocation of a function-typed source parameter whose return slot is `T` (the set-erasing return-slot arm, capture-free again), or through another `out`, by induction. A lambda cannot be formed at a bare `T` inside the body, and `new T()` excludes function types. So the `out` location of a bare-`T` parameter holds a capture-free value when the call returns — exactly what 0133's sweep item 3 already stated for the direct call (*"`d.TryGetValue(k, out var g)`: `g` is a collection read, empty set — true post-repair"*). For a callee parameter declared `out o: Object`, the formed slot is `out Object`, not a function type, and the bar never applied; the callee's own `o := f` is a set-erasing store in a checked body.

So the exemption follows from the bar's ground, and the issue's recommendation (option 2: *the bar would key on positions a caller's argument can reach: plain, `ref`, `owned`*) is adopted, with one refinement: `owned` needs no mention, since §28.1 clause 3 erases the marker at a function-type substitution and the formed slot is plain.

`ref` stays under the bar. Through a `ref` slot the caller's current value flows into the callee: `var put: (ref () -> int) -> () := b.Put;` over `Put<T>(ref x: T)` would, when called with `put(ref f)` for a capture-bearing `f`, carry `f` into a set-erasing location with no conversion at the caller. (At a direct call `b.Put(ref f)` the current value of a `ref` argument is among the call's borrows and the `ref x: T` location is set-erasing, so `f` is rejected there; the formed value's plain-looking slot would lose that check.)

### 1.2 Decision 2 — the post-call set of a function-typed location

§28.3, *A `ref` or plain `out` location outlives the call* (v0.325):

> The caller therefore treats each such location as receiving any borrow the call carries: the sources the receiver and every plain (non-`owned`, non-`scoped`) argument contribute … and the current value of every such `ref` argument. At the call, each of those borrows must be admissible into each such location exactly as the direct assignment of it would be … and a violation is rejected at the call site; after the call the location's source set is their union.

And one sentence earlier: *Where the location's declared type is a function type it is also a function-typed holder (Function-typed holders above), so a function value stored there is admitted only capture-free.*

The two sentences do not compose for a function-typed location. The holder rule says the callee stores only capture-free values there — a plain function-typed parameter carries itself, a lambda over the receiver carries `this`, an invocation of a function-typed parameter carries its set, and each is rejected at the store, in a checked callee body; and at a bare-`T` location the induction of §1.1 gives the same. The union then charges the location with borrows it can never hold. Program B (rejected today, admitted by this proposal):

```
class Cache
  private _g: (() -> int)? := null;
  public method Load(d: Dictionary<string, () -> int>): bool
    return d.TryGetValue("k", out _g);    // v0.325: `_g` is a field holder; the call carries `d`'s source → rejected
  end Load;
end Cache;
```

and its two-step twin `if d.TryGetValue("k", out var g) then _g := g; end;` (`g` carries `{d}` after the call; the field store rejects a non-empty set). Both are sound: `g` holds a value read out of `d`'s collection slot, which the closure bullet's provenance already gives the empty set when read directly (`_g := d["k"]` is admitted).

The v0.325 lane recorded the fix precisely (#5526, comment 2): *the location could soundly carry its prior set (for `ref`) or the empty set (for `out`)*. For `ref` the prior set is exact: the callee may leave the location untouched, or overwrite it with a capture-free value, and it cannot copy the location's value anywhere (a read of a `ref` function-typed parameter is unannounced, §28.3's provenance clause 6). For `out` the empty set is exact on normal completion; a `throw` path keeps the caller's prior value, which the flow analysis already reads at the `catch` from the pre-call state.

The admissibility check at the call ("each of those borrows must be admissible into each such location") is vacuous at such a location for the same reason and is stated vacuous, so Program B's single-step form is admitted directly. It is not the check that keeps a bare-`T` `ref` location capture-free: that is the set-erasing check on the `ref` argument's current value, a separate rule the carve leaves standing, and the edit says so, so the carve cannot be read as lifting it (critic B3).

---

## 2. Options

| | Option | Decision 1 | Decision 2 |
|---|---|---|---|
| **(i)** | **Status quo** — the bar over every function-typed slot; the union after every call | rejects Program A; remedy `lambda(k, out g) => d.TryGetValue(k, out g)` — which is itself an `out` lambda parameter, admitted but noisy | rejects Program B and its twin; remedy: none (a function value read through `out` can only be invoked or re-read) |
| **(ii)** | **Key the bar on inbound slots; state the function-typed location's post-call set** (recommended) | admits Program A; `ref` and plain slots unchanged | admits Program B; class-typed locations unchanged |
| **(iii)** | Drop `out` locations from the set-erasing-position *definition* instead, so the bar's "binds to a set-erasing position" no longer matches | admits Program A, but makes an `out o: Object` location not set-erasing, re-opening the callee-side store `o := f` that 0133's sweep item 14 closed; the definition keys on where a set is *erased*, the bar on where a caller's value *enters* | — |
| **(iv)** | Decision 1 alone; defer decision 2 as recorded | admits Program A | leaves Program B rejected, with the over-approximation recorded a second time on an issue the owner's direction said to resolve |

**Evaluation.** *Soundness:* (ii) and (iv) are sound by the induction of §1.1, which the 0133 critic's own soundness argument already carries (the premise "the first conversion is a set-erasing position" holds for every inbound route; `out` is not an inbound route). (iii) is sound for the bar but weakens the callee-side rule at `Object`. *Simplicity:* (ii) adds one qualifier to the bar (three words: "other than an `out` parameter") and one sentence to the location rule; (iii) adds a special case inside a definition eleven rules cite. *Consistency:* (ii) makes the formed reference and the direct call agree, which the bar exists to guarantee in the rejecting direction and now guarantees in the admitting one; (i) leaves the one slot kind where the two disagree. *Migration reach:* (ii) rejects nothing that compiles today; measured in §5. **Decision: (ii).**

---

## 3. The exact normative edits (▲)

Anchors are v0.325 (`b27728a`); re-taken at composition, after v0.326 and after 0136 S2-4's restatement, which this proposal's spec edits are written against (S2-4 moves the set-erasing definition from the closure bullet to a *Set-erasing positions* bullet beside *Function-typed holders*; the sentences below are placed in that bullet).

1. **§28.14, *The method-reference formation bar*.** "binds to a callee parameter that is a set-erasing position — one declared `Object`, or at a bare type parameter …" → ▲ "binds to a callee parameter, **other than an `out` parameter**, that is a set-erasing position ([§28.3]'s *Set-erasing positions*) — one declared `Object`, or at a bare type parameter that the formation's substitution makes a function type (`var add := xs.Add;` at `xs: Array<() -> int>`; `var keep: (() -> int) -> () := k.Keep;` over `Keep(o: Object)`; `var put: (ref () -> int) -> () := b.Put;` over `Put<T>(ref x: T)`): a call through the formed value passes its argument to a plain or `ref` function-typed parameter, which the caller side admits whatever its set (at a plain slot under the call-bounded default above, at a `ref` slot under [§28.3]'s location rule), and the callee would receive it at a position that announces none. **An `out` parameter is exempt** (`var tg := d.TryGetValue;` at `d: Dictionary<string, () -> int>` is admitted): a call through the formed value passes it a location and no value, and what the callee copies out is capture-free, so the formed type erases nothing the caller supplies; on the call's normal return the location carries the empty set ([§28.3]'s *A `ref` or plain `out` location outlives the call*)." The remedy sentence is unchanged.
2. **§27.6**, the mirror sentence: "A parameter slot of function type is not formed over a callee parameter declared `Object` …" → ▲ "A parameter slot of function type, **other than an `out` slot**, is not formed over …".
3. **§28.3, *Set-erasing positions*** (S2-4's bullet): in the reach sentence, "at an argument" → ▲ "at an argument (a `ref` argument's current value reaching a `ref` location so declared included)", making the inbound `ref` route the bar's `ref` example relies on explicit; and after the return-slot arm: ▲ "An `out` location is reached by no argument of a call: the call passes it a location, not a value, and what the callee copies out is capture-free — at `Object`, the callee's own store there is a set-erasing assignment its body is checked for; at a bare type parameter, every value of that type the generic body can place there arrived through a set-erasing position, a holder, the return-slot arm or another `out`, and an override or implementation declaring the slot at a function type is a function-typed holder (*A `ref` or plain `out` location outlives the call* below)." And in its closing sentence: "a method reference formed over one ([§28.14]'s *method-reference formation bar*, **which exempts an `out` parameter on that ground**)".
4. **§28.3, *A `ref` or plain `out` location outlives the call*.** "after the call the location's source set is their union." → ▲ "after the call the location's source set is their union — **except at a location whose type, after substitution, is a function type**: the callee can store there only a capture-free function value (the holder rule above; at a bare type parameter, *Set-erasing positions*), so the call's borrows need no admissibility check against it, and on the call's normal return an `out` location carries the empty set and a `ref` location the set it carried before the call (on an exception exit an `out` recipient keeps its prior value, §22.3, and so its prior set). **A `ref` argument's current value at a location declared at a bare type parameter is still checked there as at a set-erasing position (*Set-erasing positions*): that check, not this one, is what keeps the location capture-free.**"
5. **Appendix C.3**, the formation-bar row: ▲ "A method reference is not formed with a function-typed parameter slot, other than an `out` slot, bound to a set-erasing callee parameter (…)". The set-erasing row's citation is re-pointed by S2-4 to *Set-erasing positions*.
6. **Rationale §4.2**, one paragraph after *`scoped` is not admitted on `Object`* (v0.321): the decision record with the three declines (text in the edit set).
7. **Code-generation reference** (in-place corrections of two existing sentences that would otherwise be false, no pin and no appended sentence, per its maintenance rule): §4.4's follow-up to *Closures by position* → ▲ "Do not form a method reference whose parameter is a function type over such a parameter, **an `out` one excepted** (`var add := xs.Add;` with `xs: Array<() -> int>` is rejected; `var tg := d.TryGetValue;` with `d: Dictionary<string, () -> int>` is fine): call `xs.Add(f)` directly at each site; a lambda `lambda(g) => xs.Add(g)` is rejected too, since its own function-typed parameter carries itself (the set-erasing row above)." *(Remedy clause corrected at v0.330: the lambda this item first quoted is rejected in its body for every argument; see the v0.330 changelog.)*; the `ref`/`out` bullet's type parenthetical drops "function type" from the union-receiving kinds → ▲ "(a class or interface type, or a borrow-bearing record, choice or inline array; **at a function type the callee can fill it only capture-free, so an `out` one is capture-free after the call and a `ref` one keeps the set it had**)".
8. **`DECLINED.md`**, three `rejected` rows (§4).

---

## 4. Declined alternatives (one `rejected` row each)

| Topic | Reopening bar |
|---|---|
| Keeping the method-reference formation bar uniform over an `out` callee parameter at a set-erasing position (option (i), the v0.320 text) | a route by which a value of the caller's reaches an `out` location of the callee during the call |
| Dropping `out` locations from the set-erasing-position definition so the bar stops matching them (option (iii)) | —(none stated); the definition keys on where a set is erased, the bar on where a caller's value enters, and `out o: Object` is a real erasure site in a checked callee body |
| Counting the call's borrows into a function-typed `ref` or plain `out` location's post-call set (the union, as at a class-typed location; option (i)/(iv) for decision 2) | a callee that can store a capture-bearing function value through a `ref`/`out` location — which would first need the holder rule at that location withdrawn |

---

## 5. Migration, measured

- **Newly admitted (decision 1):** forming a method reference over a member whose only function-typed slots bound to set-erasing callee parameters are `out` slots: `var tg := d.TryGetValue;` at `Dictionary<K, V>` / `OwningSlotMap<T>` with a function-typed value, `s.TryPop` / `TryPeek` over `Stack<T>`, `q.TryDequeue` / `TryPeek` over `Queue<T>`, and user members of the same shape. Census: `grep -rnE '\.(Try[A-Za-z]+)\s*;' tests/ docs/` → 4 lines, all in `tests/codegen/dictsetll_bareref_nomember_2536_test.cpp` (`s.TryAdd` over `Set<int>` / `OwningSet<E>`, `ll.TryPopFirst` over `OwningLinkedList<E>`), none at a function-type substitution; no doc example forms such a reference; no rule-corpus case states the widened bar at all (`s28_14_0018` is the register bar). **0 sites flip.**
- **Newly admitted (decision 2):** `M(out this._g)` and `M(out var g); _g := g;` over a function-typed `out` at any callee; `ref` likewise keeps its prior set. Census: no test, doc example or corpus case passes a function-typed `out`/`ref` location into a field or stores an `out`-filled function value (`grep -rn 'out var' tests/ | grep -- '->'` → 0; `grep -rn 'out this\._' tests/` → 0 at a function type). **0 sites flip.**
- **Newly rejected:** none. **Doc-only:** the rationale record, the decline rows, the code-generation reference's two sentences.
- **Spec-ahead-of-impl:** the widened bar is unimplemented (`set-erasing` in `src/` → 0; the register bar at `src/sema/ExprTypecheck_Calls.cpp:21516` is name-gated to `Subscribe`/`Add`), so the compiler already admits Program A; #5515 is annotated with the `out` carve. The location rule's call-site union is unimplemented (#5145, open since v0.309), so the compiler already admits Program B's two-step twin; #5145 is annotated with the function-typed carve. The holder rule at an `out` write is implemented (`call_bounded_2814 / out_param_write_rejected`, per v0.325's #5601 record). No test flips.
- **Corpus:** no `baseline.tsv` verdict changes; no case quotes the bar's qualified sentence. Coverage gaps recorded, not written: `var tg := d.TryGetValue;` at a function-typed `V` (accept); `var add := xs.Add;` at `Array<() -> int>` (reject — a v0.320 gap); `d.TryGetValue(k, out this._g)` at a function-typed `V` (accept); `var put := b.Put;` over `Put<T>(ref x: T)` at a function type (reject).
- **Guides:** no guide mentions the formation bar or a set-erasing position (`grep -n 'set-erasing\|formation bar' docs/Bezel_Guide_0*.md` → 0); no pin moves for this proposal.

---

## 6. Kernel and body-locality

Both decisions are derivations from K2.3 + K2.4, as 0133 classified the bar and the location rule: the bar protects K2.4's "no holder reached through a type that announces no set can re-establish one"; an `out` slot carries no value the caller announced or failed to announce, so there is nothing to protect. No axiom is touched. Body-locality: **local** — the bar reads the callee's declared signature and the formation's substitution; the location rule reads the callee's signature and the caller's own flow analysis.

---

## 7. Self-critique (adversarial pass)

1. *Could a callee place a capture-bearing value in a bare-`T` `out` slot through a route the induction misses?* Routes checked: a `T`-typed field, component, payload or slot of the receiver or of an argument (stores into them are set-erasing or holder stores: capture-free); a plain or `ref` `T` parameter (set-erasing at the call: capture-free); the result of invoking a `() -> T`-shaped source parameter (the return-slot arm: capture-free); another call's `out T` (induction); a `T`-typed element read from a collection (its stores were set-erasing); `default(T)` (null); a lambda literal (cannot be typed at a bare `T` inside the body); a `case`/pattern binding of a `T` payload (reads what was stored). A polymorphic-recursion route (§14.6) only instantiates the same member again. None found.
2. *A `ref` location's "prior set" — can the callee copy the `ref` value elsewhere so that the location's prior value escapes?* No: a read of a `ref` function-typed parameter is traced to no form and carries an unannounced source (provenance clause 6), bounded by the call; a store of it into any holder is rejected. The caller side still checks the prior value against the set-erasing `ref x: T` location at the call, so the induction of item 1 holds for `ref` too.
3. *Does the exemption reopen 0133's Program E (`var put: (() -> int) -> () := bag.Put; put(f)`)?* No: `Put(x: T)` is a plain slot; the bar keeps it.
4. *A formed reference whose `out` slot is nullable, `out (() -> int)?`.* "Nullability stripped" stays in the bar (the v0.324 critic's finding), so the slot is still a function-typed slot; it is exempt because it is `out`, not because it is nullable.
5. *An `out owned` slot at a bare `T`.* §28.1 clause 3 erases `owned` at a function-type substitution; the formed slot is plain `out`; values flow out; exempt. (Function types are runtime-managed, so `out owned` carries no obligation there anyway.)
6. *Decision 2's "no admissibility check" — does it lose a check at a class-typed location?* No: the carve is keyed on the location's substituted type being a function type; class-typed and borrow-bearing locations keep the union and the check.
7. *Is this a ruling the current text already decides (no proposal needed)?* No: the v0.320 bar is unambiguous and rejects Program A, and the v0.325 union is unambiguous and rejects Program B; both are owner-accepted texts, so the amendment is a design change under process §2's third rule, which is why 0133's critic filed it as `needs-owner-decision` rather than ruling it.
8. *A `T`-typed class field written from outside the generic body (critic N6).* The induction's "a `T`-typed field of the receiver" route assumes every store into such a field is set-erasing or a holder store. A `public` (or `internal`/`protected`) class field declared at a bare type parameter and assigned through a substituted receiver (`b.V := f` at `b: Box<() -> int>`, or from a non-generic derived body) is named by neither list: the holder definition keys on a field's declared type being a function type, and the set-erasing list names record components, payloads and slots, not class fields. This is a v0.320 question, not one 0140 opens: the same value reaches a holder today with no `out` (`_g := b.V`, a field read carrying the empty set). Recorded for the coordinator to file as a `spec-question` against §28.3; 0140's verdicts do not depend on its answer beyond what the field read already does.
9. *What this proposal does not do:* it does not lift the other liftable corner on the same family (`DECLINED.md`'s `deferred` row *Exempting the return-slot arm where the member's own return is mint-announcing*), whose want is still unmeasured; and it does not touch §28.3's *Bare-type-parameter returns* clauses, which v0.326 is ruling on.
