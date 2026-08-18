# Proposal 0072 — The nullable-return convenience tier: `ParseOrNull` and `GetOrNull`

| | |
|---|---|
| **Status** | graduated-v0.231 — **superseded — graduated v0.231** (owner-accepted 2026-07-29; Q1–Q4 resolutions recorded in the v0.231 changelog entry) |
| **Tracking issue** | [#2347](https://github.com/s8lap/bezel-lang/issues/2347) (implementation) |
| **Target module(s)** | stdlib only — `System` ([§2.5](../Bezel_Standard_Library.md#sec-datetime-timespan-dateonly-timeonly) `DateTime`/`DateOnly`/`TimeOnly`, [§2.6](../Bezel_Standard_Library.md#sec-guid) `Guid`), `System.Collections.Generic` ([§4.8](../Bezel_Standard_Library.md#sec-dictionaryk-v) `Dictionary<K, V>`, [§4.17](../Bezel_Standard_Library.md#sec-sorted-dictionary) `SortedDictionary<K, V>`, [§4.9](../Bezel_Standard_Library.md#sec-owneddictionaryk-v) `OwningDictionary<K, V>`), and the [§9](../Bezel_Standard_Library.md#sec-primitive-type-members) primitives. One sentence added to the [§1](../Bezel_Standard_Library.md#sec-conventions) Try-convention bullet (§3 below). **No spec change, no new types, no new grammar.** |
| **Written against** | stdlib/spec v0.228 |
| **Origin** | The 2026-07-28 owner-directed API design review ("we copy .NET too often — can the common use cases be clearer/simpler/easier?"), adversarially critiqued. The review's findings kept re-deriving one design question — where a nullable return may stand beside the Try tier — at different members; the critique's packaging, followed here, gives that question **one home** instead of a per-member scatter: the two hottest lookup/parse surfaces, the criterion that governs them, and the one taxonomy sentence they need. |

*Reference convention: unprefixed §-references are stdlib references; spec references are marked.*

---

## 0. Motivation: the two hottest expected-failure call sites

The Try convention (§1) is the library's expected-failure workhorse, and it is the right *distinguishing* form — the `bool` separates outcomes a payload cannot. But the two most common expected-failure call sites in service code do not want to distinguish anything; they want **a value or a fallback**, in one expression.

**Config parsing.** Today, string-to-value-with-default is a mutable local and two statements — the shape-(c) `TryParse` dance:

```bezel
// today
var port := 8080;
if int.TryParse(Environment.GetEnvironmentVariable("PORT") ?? "", out var p) then
  port := p;
end;
```

Proposed:

```bezel
// proposed
var port := int.ParseOrNull(Environment.Get("PORT") ?? "") ?? 8080;
```

(`Environment.Get` is proposal [0074](0074-stdlib-ergonomics-batch-3.md)'s proposed spelling of §2.4's `GetEnvironmentVariable`; with today's name the line is longer, not different in kind. For this *exact* env-var case, v0.215's `Environment.GetInt("PORT") ?? 8080` already exists — the point is every **other** string source: a config-file token, a CLI argument, a JSON string field, a header value, a line read from a stream.)

**Lookup-with-fallback.** The keyed twin. Accepted proposal [0002](0002-http-server.md)'s own tour example (§2.1, `TodoApi.GetById`) pays a `!` at exactly this site:

```bezel
return (case g.Value.TryGetValue(id, out var todo) of
  true => Http.Json(todo!),
  _    => Http.Status(404));
```

— (0002 originally spelled this with the pre-v0.208 `case … end` expression form; both 0002 and this quotation now carry the v0.208 paren-atom spelling — 0002 was modernized by the 2026-07-28 syntax-currency sweep) — a `!` inside the arm that *is* the success arm. The Try shape is doing distinguishing work no caller here asked for; the null-or-value shape folds into `??`/`?.`/null-tests directly.

The library already **has** this tier — ad hoc: the `FirstOrNull`/`LastOrNull`/`SingleOrNull` family (§5), `MinByOrNull`/`MaxByOrNull` (proposal [0026](0026-linq-additions.md), draft), `Environment.GetInt`/`GetBool` (v0.215 — "the convenience tier", the changelog's own words), `HttpHeaders.Get(name): string?` (proposal [0001](0001-networking-http.md) §3.4), `JsonObject.Get(key): JsonValue?` (proposal [0019](0019-json-dom.md)). What it lacks is (a) the tier's presence on the two hottest surfaces — parse and keyed lookup — and (b) a recorded rule for when the tier is admissible, so each new proposal stops re-deriving it. This proposal is that packaging: §1 the parse tier (recommended for acceptance), §2 the lookup tier (owner-gated, with the criterion tension disclosed up front), §3 the one-sentence §1 amendment the new members require, §4 the #1535 boundary, §5 the naming question.

**Ownership and sendability, stated once.** Every §1 member is value-in/value-out over value types and `string` — no `owned`, no borrow, no disposal obligation, sendability (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)) unaffected. §2's members state their borrow story inline.

---

## 1. `ParseOrNull(s: string): T?` on the concrete Parse-bearing types — recommended

**Today (v0.228).** The `Parse(s: string)` carriers, enumerated from the document: the eight §9.1 integer types (`sbyte`, `byte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`), `double`/`float` (§9.2), `decimal` (§9.3), `bool` (§9.4), `char` (§9.5), `DateTime`/`DateOnly`/`TimeOnly` (§2.5), and `Guid` (§2.6) — **seventeen types**, each carrying `Parse(s): T` (`FormatException`/`OverflowException` on bad input) and a shape-(c) `TryParse(s, out result: T): bool`. (`TimeSpan` carries no `Parse` and is not a target.)

**Proposed** (a per-type static member on each of the seventeen; `Int32` shown, the others analogous per §9.1's stated pattern):

```bezel
// PROPOSED — the convenience form beside Parse and TryParse
public static pure method ParseOrNull(s: string): int?;
      // null exactly where Parse(s) raises (the FormatException/OverflowException
      //   contract); otherwise the parsed value. Same strict input grammar as
      //   Parse — no whitespace tolerance (v0.137).
```

Purity mirrors each type's own `Parse`: `pure` on the §9 primitives (whose `Parse` members are declared `pure`), plain `static method` on the §2.5/§2.6 types (whose `Parse` members are unmarked). Unlike `TryParse`, the member *can* be pure where its `Parse` is — it declares no `out` (spec [§22.15](../Bezel_Language_Spec.md#sec-pure-members), the rule recorded at §2.2's `DivRem`).

**Contract.** `ParseOrNull(s)` returns `null` in exactly the cases where the type's own `Parse(s)` raises, and the parsed value otherwise — the same equivalence every `TryParse` states, re-expressed in the payload. No new parsing behavior anywhere: one grammar per type, three access forms.

**Why this satisfies the recorded criterion.** The governing rule is §1(b)'s criterion as recorded in the v0.184 changelog and reaffirmed in the #1535 joint ruling: *"Nullable return where the payload carries the signal; Try + `when true` where it cannot (§14.4/§7.10 nullable-underlying instantiations) or where the bool shape is the idiom."* Every target here is a **concrete non-nullable value type** — so `T?` carries the signal unambiguously: a `null` result means exactly "the input did not parse", and no instantiation exists at which the payload could itself be null (the spec [§7.10](../Bezel_Language_Spec.md#sec-generics-and-nullability) collapse structurally cannot arise; contrast §2). This is the criterion's "payload carries the signal" side, satisfied without residue.

**String form only — the radix and span duals are declined.** A complete dual of the v0.228 parse surface would be 36 members: 17 string forms, 8 radix forms (`Parse(s, radix)`, §9.1), 11 span forms (`Parse(s: ReadOnlyStringSpan)`, §9.1–§9.3). This proposal takes the 17 and declines the rest, with reasons: the radix path (v0.228's own `TryParse(s, radix, out)`) and the span path (v0.222's allocation-conscious forms) are **specialist surfaces** whose callers are already in ceremony-tolerant territory — a hex-mask parser or a zero-allocation tokenizer is exactly where the established bool+out idiom is at home — while the convenience tier's whole value is at the casual call site, which holds a `string`. Adding 19 more members would buy surface completeness, not capability (every input remains parseable three ways). Recorded here so a later sweep can cite the decline rather than rediscover the question.

**A conversion is impossible; a dual is the only route.** A #1535-style *conversion* of `TryParse` (the shape-(a)→(b) reshaping `Weak<T>.TryUpgrade` underwent at v0.222) is structurally unavailable: for the eleven §2.19 conformers plus `char`, the per-type `TryParse` members **witness `IParsable<T>.TryParse`** through the compiler-provided conformances — the constraint surface every generic scalar-parsing path programs against — so the bool+out shape is load-bearing and stays; for `bool` (§2.19's recorded non-conformer) and the §2.5/§2.6 temporals and `Guid` (outside §2.19's list entirely), no constraint role exists, and §4's fork-scope argument alone carries the duals-not-conversions conclusion. The v0.222 span-parse precedent applies directly: new members land per-type, "`IParsable<T>` … unchanged — the … forms are per-type members, not constraint surface" (§9.1).

**Worked call sites** (every named API verified in v0.228):

```bezel
// config token, value or default — one expression, no mutable local
var raw := Environment.GetEnvironmentVariable("TIMEOUT_SECS") ?? "";
var timeout := TimeSpan.FromSeconds(double.ParseOrNull(raw) ?? 30.0);

// validated input — null-test where the failure needs a message
var day := DateOnly.ParseOrNull(line);           // day: DateOnly?
if day = null then
  Console.Error.WriteLine($"not a date: {line}");
  return;
end;
Schedule(day);       // day narrowed non-null after the guard (spec §7.4);
                     // Schedule is illustrative
```

---

## 2. `GetOrNull(key: K): V?` on the dictionary family — presented owner-gated

**The tension, disclosed first.** §1(b)'s criterion has a "cannot" side, and the generic dictionaries sit on it. `Dictionary<K, V>`'s `V` is unconstrained, so `V` may be instantiated at a nullable type (`Dictionary<string, string?>` is legal), and there `V?` **collapses**: spec [§7.10](../Bezel_Language_Spec.md#sec-generics-and-nullability) — `?` is idempotent under substitution — so a `GetOrNull` result cannot distinguish "key absent" from "key present, value null". That is precisely the case the recorded criterion routes to the Try shape: *"Try + `when true` where it cannot (§14.4/§7.10 nullable-underlying instantiations)"* (v0.184 changelog; restated by #1535's 2026-07-11 joint ruling, and reaffirmed by the 2026-07-24 termination ruling §4 engages). Under the criterion's letter, a nullable-return lookup over a **generic** `V` is the shape the library told itself not to build. This section does not argue the letter away; it presents the member because the library's recorded practice already runs against the letter (next paragraph), and puts the call to the owner.

One boundary inside the item: the tension is a **§4.8/§4.17 fact, not a §4.9 fact**. `OwningDictionary<K, V>` declares `where V: class`, and spec §7.10 defines `class` as restricting to *non-nullable* reference types — so at §4.9 the payload is statically non-nullable at every instantiation and `GetOrNull` satisfies the criterion as cleanly as `Weak<T>.TryUpgrade` does. The owner may accordingly accept §4.9's member and decline §4.8/§4.17's without inconsistency (open question Q2).

**The counter-evidence, stated honestly.** The criterion's letter notwithstanding, the library and its accepted drafts already ship generic-payload nullable returns, each with a recorded caveat rather than a prohibition:

- §5's `FirstOrNull`/`LastOrNull`/`SingleOrNull` — generic `T?` returns, with the §7.10 conflation caveat recorded in §5's own text (quoted verbatim below);
- proposal 0026's `MinByOrNull`/`MaxByOrNull` — same shape, same caveat, restated;
- v0.215's `Environment.GetInt`/`GetBool` — which the changelog itself names **"the convenience tier"**, and which *deliberately conflate* absent with malformed ("recorded: callers that must distinguish keep the two-step idiom", §2.4);
- proposal 0001's `HttpHeaders.Get(name): string?` (annotated in 0001's own listing "`*OrNull` honesty, stdlib §5") and proposal 0019's `JsonObject.Get(key): JsonValue?` — keyed lookup, null-for-absent, at concrete payload types.

So the recorded criterion and the recorded practice pull apart: the criterion says Try-where-collapse; the practice ships the collapsing convenience *beside* the Try form, caveated. The §3 sentence below is drafted to reconcile them — the convenience tier is additive to, never a replacement of, the distinguishing form.

**Proposed** (three members; not `pure` — key lookup runs user `Equals`/`GetHashCode`/comparers, or `CompareTo` at §4.17: the recorded v0.215 rule that keeps the `Contains` family impure):

```bezel
// PROPOSED — Dictionary<K, V> (§4.8) and SortedDictionary<K, V> (§4.17)
public method GetOrNull(key: K): V?;
      // absent -> null; present -> the value, under the family's read semantics:
      //   a copy for value-typed V and string, a borrow of the stored value
      //   (source: the receiver) for class-typed V (§4.8/§4.17)

// PROPOSED — OwningDictionary<K, V> (§4.9), where V: class
public method GetOrNull(key: K): V?;
      // absent -> null; present -> a borrow of the stored value, the indexer's
      //   get posture (§4.9) — ownership stays with the dictionary; this is a
      //   look, not a take (the take forms remain PopValue/TryPopValue)
```

**Proposed doc caveat.** §5's `*OrNull` caveat, whose wording is:

> "(When the element type is itself nullable, `?` is idempotent under substitution — spec §7.10, v0.159 — so an `*OrNull` result cannot distinguish "empty" from "a present `null`"; use `Count` or the `Try` shape when the distinction matters.)"

is carried onto the §4.8/§4.17 members with the four word-substitutions the surface forces (element→value, empty→absent, `Count`→`ContainsKey`, `*OrNull`→`GetOrNull`):

> "(When the value type is itself nullable, `?` is idempotent under substitution — spec §7.10, v0.159 — so a `GetOrNull` result cannot distinguish "absent" from "a present `null`"; use `ContainsKey` or the `Try` shape when the distinction matters.)"

**The `??` limit at class-typed V, stated honestly.** At class-typed `V` the §4.8/§4.9 result is a **borrow** sourced from the receiver, and spec [§7.7](../Bezel_Language_Spec.md#sec-null-coalescing-and-=) gives `??` the same ownership meet rule as an `if`/`else` join: **an owned arm mixed with a borrowed arm is rejected**. So the tempting fallback spelling does not compile:

```bezel
using var v := d.GetOrNull(k) ?? new Thing();   // COMPILE ERROR: borrowed arm ?? owned arm
                                                //   (spec §7.7 / §28.3 meet rule; Thing illustrative)
```

The working shapes: a **borrowed** fallback composes (`d.GetOrNull(k) ?? sharedDefault`, both arms borrows), and a **fresh** fallback binds first, then null-tests:

```bezel
var v := d.GetOrNull(k);           // v: Thing? — a borrow of the stored value, or null
if v = null then
  using var fresh := new Thing();
  Process(fresh);
else
  Process(v);                      // narrowed non-null (spec §7.4)
end;
```

The member's sweet spot is therefore **value-typed and `string` V** — `counts.GetOrNull(word) ?? 0`, `config.GetOrNull(key) ?? "default"` — which is also where the borrowing dictionaries overwhelmingly live. At class-typed `V` with a fresh fallback, `TryGetValue` or bind-then-test remains the form; the listing comment should say so.

---

## 3. The §1 Try-convention amendment (required by §1 and §2 alike)

§1's taxonomy currently spells nullable return **as a Try shape**: shape (b) is "`TryX(...): owned T?`", whose members are the acquisition idioms (`Mutex.TryAcquire`, `Weak<T>.TryUpgrade`). `ParseOrNull` and `GetOrNull` are nullable-return members that are *not* `TryX`-named; landing either without amending §1 would leave the document contradicting its own taxonomy (nullable-return extraction members outside the sanctioned shapes). The amendment is one sentence, appended to the §1 Try-convention bullet:

> **Proposed §1 sentence.** "Beside the Try shapes, a surface may carry a nullable-return **convenience form**, named with the `OrNull` suffix (`ParseOrNull`, `GetOrNull`, §5's `*OrNull` element accessors): the **throwing form** (`Parse`, the `[key]` indexer) treats absence as a bug, the **`*OrNull` form** returns `T?` for direct `??`/`?.` composition where the payload can carry the signal, and the **`Try` form** remains the only shape that distinguishes the outcomes a collapsed `T?` cannot (spec §7.10); shape (b)'s `TryX` spelling stays reserved for acquisition-shaped members whose payload is statically non-nullable (`TryAcquire`, `TryUpgrade`)."

If only §1 (ParseOrNull) lands, the sentence lands with `GetOrNull` struck from the parenthetical. The sentence is a naming of existing practice rather than a defect repair — §1's enumeration scopes itself to the *Try* shapes, and §5's `FirstOrNull` family plus §5.1's normative `MinOrNull`/`MaxOrNull` already sit outside it without recorded contradiction — but landing it with the tier's first hot-surface members is what stops the tier from being re-derived one surface at a time.

---

## 4. Engagement with #1535 — terminated, and not reopened here

The adjacent recorded history, engaged precisely so this proposal cannot be misread as relitigating it.

**What #1535 decided.** Fork (B) of #1535 — targeted conversions of existing Try APIs to nullable returns — was ruled by the 2026-07-24 delegated, critic-gated ruling (recorded on the issue; landed v0.222): *"Fork (B) decided and terminated with exactly one conversion: Weak\<T\>.TryUpgrade converts from Try shape (a) to shape (b) — TryUpgrade(): owned Shared\<T\>? (name kept, Upgrade() rename declined); Channel\<T\>.TryReceive reaffirmed NOT a candidate and the nullable-underlying-element ban rejected as self-defeating."* The v0.222 changelog entry carries the same decision with its declines spelled out: freezing the status quo declined (it left "the stdlib's one standing violation of its own §1 taxonomy"), the `Upgrade` rename declined ("an unprefixed null-signaling fallible API would be a one-method convention" — the Try prefix **kept** on a shape-(b) member), and converting `Channel<T>.TryReceive` declined ("self-defeating").

**Why this proposal is outside the terminated fork.** Two independent reasons:

1. **Duals, not conversions.** Fork (B)'s subject was *reshaping existing members*; its termination forecloses conversions, not additions. Nothing here changes any Try member's shape: every `TryParse`/`TryGetValue`/`TryPopValue` keeps its signature, its shape-(c) or `when true` contract, and (for `TryParse`) its §2.19 constraint-witness role. The proposed members are new spellings beside the old ones.
2. **Scope.** Fork (B) grew out of the `out owned` narrowing question (the RFC's own title), and its candidate set was the shape-(a) `out owned` surface. Shape-(c) `TryParse` — a plain non-nullable value-typed out, no `out owned` anywhere — was never within the fork's scope at all.

What this proposal *does* take from the ruling is the naming datum consumed in §5: the Try prefix was deliberately kept on a nullable-return member, and an unprefixed fallible name was declined as a one-method convention.

---

## 5. Naming — an open question, with three live precedents

Three naming conventions for nullable-return lookup/parse coexist in the recorded surface:

1. **Bare `Get`, returning nullable** — proposal 0001's `HttpHeaders.Get(name): string?`, proposal 0019's `JsonObject.Get(key): JsonValue?`, v0.215's `Environment.GetInt(name): int?`. Shortest; reads naturally at keyed-lookup sites; one of the three (`GetInt`) is normative surface, the other two are drafts.
2. **The `*OrNull` suffix family** — §5's `FirstOrNull`/`LastOrNull`/`SingleOrNull` (normative), 0026's `MinByOrNull`/`MaxByOrNull` (draft). States the failure mode in the name; the only convention of the three that is already normative in the core document.
3. **Try prefix on a nullable-return member** — the #1535 ruling kept `TryUpgrade` on shape (b) (`Upgrade()` declined). Under this precedent the parse dual could be spelled as an overload, `TryParse(s): int?` beside `TryParse(s, out): bool`.

**Recommendation: `ParseOrNull`/`GetOrNull` (convention 2).** Reasons: it joins the one family §5 already declares normatively; the name carries the contract (`null` is an expected result, read it as such); and it avoids putting **two Try shapes on one prefix** — a `TryParse(s): T?` overload beside `TryParse(s, out result): bool` would make a single member name span shape (b) and shape (c), the mirror image of the one-method-convention problem the #1535 ruling declined. Presented fairly, the alternatives' cases: bare `Get` (convention 1) is the established spelling on the *protocol-shaped* surfaces (headers, JSON) and reads best at call sites — its cost on the dictionaries is that `d.Get(k)` beside the throwing `d[k]` hides the KeyNotFoundException-vs-null difference in a name that does not state it, and 0001/0019 are drafts, not core-document precedent; the Try overload (convention 3) keeps one name for one operation family — at the cost above. Owner's pick (open question Q3); the §3 sentence is written for convention 2 and would be respelled under either alternative.

---

## 6. Gained and lost

**Gained.** One-expression config parsing and lookup-with-fallback (the two-statement mutable-local dance and the case-arm `!` disappear at these sites); a *recorded rule* for the convenience tier (§3's sentence) replacing per-proposal re-derivation; symmetry between the tier's existing members (§5, `Environment.GetInt`, 0001/0019) and the two surfaces that motivated them in the first place.

**Lost / cost.** Seventeen new members (§1) plus up to three (§2); a third lookup idiom on the dictionaries (indexer / `GetOrNull` / `TryGetValue` — the tier rule is one more thing to learn, though it is the same triple `Parse`/`ParseOrNull`/`TryParse` teaches); the §7.10 conflation footgun on generic-`V` `GetOrNull` (mitigated by the verbatim caveat, disclosed as §2's criterion tension); and the §7.7 borrowed-vs-owned `??` limit at class-typed `V`, which the listing must document to prevent a compile-error surprise.

## 7. Migration

Purely additive — no existing member changes shape or contract; no conformance-corpus flips; `IParsable<T>` (§2.19) untouched. The guides gain the idiom (the config-parse one-liner; lookup-with-fallback) at their next revalidation pass, not as part of this landing. Implementation: per-type intrinsics beside the existing `Parse`/`TryParse` handling in the compiler's builtin dispatch, each with a spec-cited regression test per the `tests/README.md` discipline.

## 8. Open questions for the owner

- **Q1 (§1):** accept `ParseOrNull` as scoped — seventeen members, string form only, radix/span duals declined-with-reasons?
- **Q2 (§2):** admit `GetOrNull` against the criterion's letter on the §5/0026/v0.215 precedent line, with the verbatim caveat? Sub-options: all three types; §4.9 only (the criterion-clean member); or decline all three and record that the criterion's letter wins on generic payloads (leaving 0001/0019's concrete-typed `Get`s as the tier's only keyed lookups).
- **Q3 (§5):** naming — `ParseOrNull`/`GetOrNull` (recommended), bare `Get` (0001/0019/v0.215 precedent), or Try-prefixed nullable overloads (the #1535 shape-(b) precedent)?
- **Q4 (§3):** confirm the §1 sentence lands with whichever of §1/§2 lands first (it is the taxonomy repair, not an optional gloss).
