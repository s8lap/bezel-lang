# Proposal 0073 — one-shot serialization facade: `Json` and `Binary` statics over the §2.15 agents

| | |
|---|---|
| **Status** | **superseded — graduated v0.232** (owner-accepted 2026-07-29; Q1–Q5 resolutions recorded in the v0.232 changelog entry) |
| **Tracking issue** | [#2349](https://github.com/s8lap/bezel-lang/issues/2349) (implementation) |
| **Target module(s)** | `System.Serialization` (existing module, [§2.15](../Bezel_Standard_Library.md#sec-concrete-serialization-formats) — already explicit-import). Additive statics only: no change to the [§2.14](../Bezel_Standard_Library.md#sec-serialization-vocabulary) vocabulary, no agent-surface change, no new fault types. |
| **Written against** | spec/stdlib **v0.228** |
| **Origin** | The 2026-07-28 owner-directed stdlib API design review, adversarially critiqued; the critique's corrections — the compiler-known admission (§4), the 0019 engagement (§5), the both-formats scope (§3 Q1) — are binding on this draft. |

*Reference convention: unprefixed §-references are stdlib references; spec references are marked. Every type and member cited as existing is verifiable in stdlib v0.228; everything new is marked **PROPOSED** and appears in §6.*

---

## 1. Problem and motivation

The §2.14/§2.15 serialization architecture is one of the library's strongest designs and this proposal changes none of it: format-agnostic agent interfaces (`ISerializer`/`IDeserializer`), capability interfaces on types (`ISerializable`/`IDeserializable`), compiler synthesis reading a type's wire shape from its declaration (spec [§9.8](../Bezel_Language_Spec.md#sec-serialization-synthesis)), no reflection anywhere (rationale [§5.19](../Bezel_Design_Rationale.md#sec-serialization-without-reflection)). What it lacks is a front door. The *only* way to serialize a value today is to hand-drive the agent:

```bezel
// today — write (the §2.15 SavePoint/LoadPoint idiom, shape-faithful):
using var s := new JsonSerializer();
user.Serialize(s);
var text := s.ToString();

// today — read:
using var d := new JsonDeserializer(text);
var user := User.Deserialize(d);
```

Three lines, one scope-bound agent, and one naming of a class the caller does not otherwise care about — per direction, per call site — for the single most common operation the module exists for: *whole value in, encoded form out* (and back). The ceremony is real and hot: the 2026-07-28 review counted **111** construction sites of the JSON agent pair and **227** of the binary pair in the repo's own test corpus (a re-count while drafting found 228 — the corpus drifts; the magnitude is the point). Every one of those sites is the same three-line dance.

The one-line vocabulary already exists **on the wire**: proposal 0001 §3.3 gives the HTTP client `resp.Json<User>()` / `resp.JsonOwned<Order>()`, and proposal 0002 §3.2 gives the server `req.Json<Todo>(): OwningTask<T> where T: IDeserializable` — one call, target type in the angle brackets, agents invisible. This proposal is the **at-rest counterpart**: the same one-line shape for values in memory, files, and buffers, over **both** shipped formats. (A JSON-only facade was considered and rejected by the review's critique: the binary dance is the *more* frequent one in practice, and shipping the convenience for one format but not its sibling would manufacture exactly the member-level asymmetry proposal 0067 existed to remove.)

```bezel
// proposed:
var text := Json.ToText(user);
var user := Json.Parse<User>(text);
using var order := Json.ParseOwned<Order>(text);   // class DTO: owned, scope-bound
using var blob  := Binary.ToBytes(state);
```

---

## 2. What exists today (enumerated before extending)

- **The vocabulary** ([§2.14](../Bezel_Standard_Library.md#sec-serialization-vocabulary), auto-imported): `ISerializer`/`IDeserializer` (the format agents' interface), `ISerializable` with the instance method `Serialize(s: ISerializer)`, and `IDeserializable` — a **marker interface with no members**. The deserialization entry point is a static factory *recognized by name*, in exactly three shapes: `Deserialize(d: IDeserializer): T` for a value-type target (record, enum), `: owned T` for a class target, `: owned Shared<C>` for a class that is itself a shared node. Rationale §5.19 records why the marker has no method yet (the static-member promotion is recorded follow-up).
- **The agents** ([§2.15](../Bezel_Standard_Library.md#sec-concrete-serialization-formats), `System.Serialization`, explicit import): `JsonSerializer` — `constructor()` / `constructor(options: JsonOptions)`, `ToBytes(): owned OwningBuffer<byte>` (the UTF-8 result; both serializers write to an `OwningBuffer<byte>`-backed stream), `ToString(): string` (the UTF-8→string convenience). `JsonDeserializer` — constructors over `ReadOnlySpan<byte>`, `ReadOnlyArraySpan<byte>` (v0.165 #1358), and `string`, each with a `JsonOptions` twin (v0.215 options parity). `BinarySerializer` — `constructor()` only, `ToBytes(): owned OwningBuffer<byte>`. `BinaryDeserializer` — constructors over `ReadOnlySpan<byte>`, `ReadOnlyArraySpan<byte>`, and `(ReadOnlySpan<byte>, ignoreUnknownFields: bool)`. The options record is `JsonOptions(Indented, IndentWidth, AllowTrailingCommas, IgnoreUnknownFields, MaxDepth)`; binary has **no options record** — its one knob is the constructor bool.
- **The fault surface** (§2.15): malformed input, type mismatches, depth-limit violations, and trailing-data conditions raise `SerializationException` (declared in `System.Serialization`; carries byte offset for binary, line/column for JSON). Writing a non-finite float to JSON raises `SerializationException`. Unknown fields fault by default; `JsonOptions.IgnoreUnknownFields` / the binary constructor bool opt into skipping.
- **The wire precedent**: 0001 §3.3's `Json<T>(): Task<T>` / `JsonOwned<T>(): OwningTask<T> where T: class` split, with its recorded justification: the recognized factory "is a static factory *recognized by name* … so a generic body cannot invoke it via any existing constraint. `Json<T>`/`JsonOwned<T>` are therefore **compiler-known methods** in the same way `Task.FromResult<T>` is (its class-T rejection is compiler-special-cased, stdlib §6.7)". [§6.7](../Bezel_Standard_Library.md#sec-task-static-helpers)'s `FromResult<T>`/`FromOwnedResult<T>` is the naming and mechanism precedent for the value/class split.
- **The facade precedent**: [§7.3](../Bezel_Standard_Library.md#sec-file) `File` — "Static convenience methods" over the `FileStream` plumbing (`ReadAllText` beside the stream family), including `WriteAllBytes(path, contents: ReadOnlySpan<byte>)`, whose §7.3 comment names it the "serializer-output path, zero-copy" (v0.165 #1358) — the library already anticipated one-shot serializer output flowing to convenience statics.
- **The recorded counter-posture**: proposal 0019 (JSON DOM, draft) declined a `Json` static class — twice. Its Q6a decision: "**(a) `Parse` lives on `JsonValue`** as a static factory — no separate `Json` static class", with the reasoning "A `Json.Parse` static class was considered and declined: a whole type for one method when the value type is the natural home, and it would fork `TryParse`/`ToJsonString` awkwardly across two types." Its declined-alternatives table repeats the decline. §5 engages this head-on; it is this proposal's central open question.
- **Adjacent live work**: proposal 0010 (draft) adds `ISerializable`/`IDeserializable` conformances for `Guid`/`DateTime`/`TimeSpan`/`DateOnly`/`TimeOnly` and the `WriteBytes` token, under the owner-resolved (2026-07-04) ruling that the token vocabulary is thereafter **frozen**.

---

## 3. Design decisions

### Q1 — Facade over plumbing, both formats, in `System.Serialization`

Two static classes, `Json` and `Binary`, living beside the agents they wrap — the module already carries the explicit `import System.Serialization`, and the facade names a format exactly as the agents do, so the §2.15 placement logic ("a program that performs serialization names the format explicitly") transfers unchanged. The agent pair **remains the extension point**: streaming and incremental writing, custom `ISerializer`/`IDeserializer` formats, hand-written §2.14 conformances, and any caller that needs to interleave multiple values in one stream keep hand-driving the agents. This is the §7.3 `File`-over-`FileStream` shape: the facade covers the whole-value one-shot; the plumbing stays public, documented, and load-bearing.

Both formats ship together. The formats' surfaces differ honestly (Q3): JSON has a text form and an options record; binary has neither, and the facade mirrors rather than papers over that.

### Q2 — The write family is ordinary generic code; stated plainly

`ISerializable` has a real instance method, so the write direction needs **no dispensation**:

```bezel
public static method ToText<T>(value: T): string where T: ISerializable
  using var s := new JsonSerializer();
  value.Serialize(s);        // reachable through the constraint — ordinary generic code
  return s.ToString();
end ToText;
```

is user-writable Bezel today. The facade's write members are ordinary constrained generics, and this proposal claims no compiler magic for them.

### Q3 — The surfaces mirror §2.15 exactly; the byte forms are the honest primary

**JSON.** The serializer's *native* product is bytes: §2.15 states both serializers accumulate into an `OwningBuffer<byte>`-backed stream, `ToBytes(): owned OwningBuffer<byte>` returns the UTF-8 result, and `ToString()` is the labeled convenience decode. So `Json.ToBytes` is not a bolt-on — it is the primary product surfaced, and `Json.ToText` is the mirrored convenience. On the read side, the facade's input forms are exactly the deserializer's three constructor inputs — `string`, `ReadOnlySpan<byte>`, `ReadOnlyArraySpan<byte>` — each with a `JsonOptions` twin, mirroring the v0.215 options-parity set one-for-one. Nothing is invented; every overload corresponds to a §2.15 constructor or result accessor.

**Binary.** `BinarySerializer` yields bytes only, so the `Binary` facade has **no text form** — a `Binary.ToText` would have to invent an encoding §2.15 does not define (that is 0009's Base64 job, composed by the caller if wanted). Its options story is mirrored as-is: no options record exists, so `Binary.Parse`/`ParseOwned` take the `ignoreUnknownFields: bool` the `BinaryDeserializer` constructor takes — and only on the `ReadOnlySpan<byte>` input form, because §2.15 declares no `(ReadOnlyArraySpan<byte>, ignoreUnknownFields)` constructor. That gap is §2.15's, faithfully reproduced, and flagged for a piggyback pin rather than silently completed (open question Q3).

### Q4 — The read family is a compiler-known generic; this is the load-bearing admission

Stated plainly, per the review critique: **`Json.Parse<T>`/`Json.ParseOwned<T>` (and the `Binary` duals) cannot be written as user code.** `IDeserializable` is a marker interface with no static members (§2.14), so no generic body can reach the recognized `T.Deserialize(d)` factory through the constraint. These four members are **compiler-known generics**, exactly the dispensation 0001 §3.3 already obtained for `HttpResponse.Json<T>`/`JsonOwned<T>` on the precedent of `Task.FromResult<T>`'s compiler-special-cased class-T rejection (§6.7). The split mirrors 0001 §3.3 and §6.7 exactly:

- `Parse<T>` requires an `IDeserializable`-conforming **record/value** T and **rejects class T at compile time**, its diagnostic directing to `ParseOwned<T>`;
- `ParseOwned<T>` requires an `IDeserializable`-conforming **class** T and returns `owned T`.

Values arrive as values, classes arrive owned — the Pattern A/B split invisible at the call site, as on the wire. The dispensation is temporary in principle: rationale §5.19 records the promotion of the recognized `Deserialize` to a real static interface member as follow-up work; when that lands, `Parse<T>`/`ParseOwned<T>` become expressible as ordinary constrained generics **with no signature change**, and the compiler-known status quietly evaporates. This proposal widens the existing dispensation from two wire methods to their at-rest duals; it does not create a new kind of magic.

### Q5 — Naming: the 0019 collision, engaged (the central open question)

Proposal 0019 declined a `Json` static class twice (§2 quotes it exactly). Its stated reason — "a whole type for one method when the value type is the natural home" — was correct *for 0019's question*: the DOM has one natural producing type (`JsonValue`), one parse entry, and companion members (`TryParse`, `ToJsonString`) that would fork across two homes. Neither premise transfers here: this facade is a **multi-method, two-direction, typed** surface (`ToText`/`ToBytes`/`Parse<T>`/`ParseOwned<T>`, plus options twins) with **no** single natural value-type home — its "value type" is every `ISerializable`/`IDeserializable` conformer in the program. But the recorded posture must be engaged, not stepped around. Three options:

- **(a) Name the facade `Json`, and amend 0019 at its graduation** — `JsonValue.Parse` remains the DOM entry, `Json.Parse<T>` the typed entry. The two never collide at compile time (the facade lives in `System.Serialization`, the DOM in `System.Text.Json`; a program importing both sees the static class `Json` and the type `JsonValue` — distinct names). 0019's declined-alternatives register gains one line noting that the decline was scoped to "a whole type for *one* method" and that 0073 established the multi-method facade with the DOM entry unmoved.
- **(b) Name it `Serialization.ToJson`/`Serialization.FromJson`** — one static class, format in the method name. Avoids the 0019 word entirely, but scales badly (`ToJson`/`ToJsonBytes`/`FromJson`/`FromJsonOwned`/`ToBinaryBytes`/`FromBinary`/`FromBinaryOwned`… — a grab-bag class), stutters against its own module (`import System.Serialization;` then `Serialization.ToJson`), and loses the per-format grouping that lets the `Binary` family carry binary's same-version caveat (§2.15) on its own doorstep.
- **(c) Put the statics on the agents** — `JsonSerializer.ToText(value)`, `JsonDeserializer.Parse<T>(text)`. No new type names at all, but it splits one facade across the exact seam it exists to hide: the caller must again know which agent class writes and which reads, and `JsonDeserializer.ParseOwned<Order>(text)` buries the verb under the plumbing noun.

**Recommendation: (a)**, because the facade's whole value is that the call site names the *format* and the *operation* and nothing else — which is precisely the two-word spelling `Json.Parse` — and because 0019 is still a draft: amending a draft's register at graduation is the ordinary currency discipline, not a reversal of a shipped decision. The owner ruling is requested before landing (open question Q1).

### Q6 — Faults: exactly §2.15's surface, nothing new

Every facade member faults exactly as its defining agent sequence (§7) faults: `SerializationException` for malformed input, type mismatches, depth violations, trailing data, unknown fields under strict policy, and non-finite floats on the JSON write path — with the same offset/line-column payload, because it is the same code raising it. **No new exception types, no new fault conditions, no Try forms in v1** (a `TryParse<T>` dual is plausible future work, but §2.15's own stance — "a deserialization mismatch is a structural error in the input, not an expected control-flow branch" — says the throwing form is the right sole v1 shape; recorded in §10).

---

## 4. Ownership and sendability

- **Write direction**: `value: T` is an unmarked parameter — a **borrow for the duration of the call** at class substitutions (spec [§28.3](../Bezel_Language_Spec.md#sec-local-variables-ownership-and-borrows)'s unmarked-parameter classification, restated in §22.15's parameter paragraph), a value at record/enum substitutions. The caller keeps ownership and disposal obligation; serialization never consumes the value. The results are fresh productions: `ToText` returns a runtime-managed `string` (no marker, §1), `ToBytes` returns `owned OwningBuffer<byte>` (the §1 fresh-producer convention, identical to the agent's own `ToBytes`).
- **Read direction**: `Parse<T>` returns a plain value (no ownership motion). `ParseOwned<T>` returns **`owned T`** — a fresh heap value transferred to the caller (spec [§28.8](../Bezel_Language_Spec.md#sec-ownership-transfer)), discharged by `using var` or an owned field, exactly as the recognized factory's own `owned T` return.
- **A quiet lifetime win**: §2.15's deserializer *borrows its input span for the deserializer's lifetime*, which today makes the caller responsible for keeping the bytes alive across the `using var d` scope. Inside the facade that lifetime is the call itself, so the caller's span obligation ends at return — one fewer lifetime relationship at every call site.
- **Sendability (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)): no impact.** Static classes have no instances; nothing here adds or removes sendability from any type. A parse result's sendability is `T`'s own, as if constructed directly.

---

## 5. Semantics: normatively *the agent sequence*

The facade adds **no third semantics**. Each member is defined — normatively, not illustratively — as the corresponding §2.15 agent sequence, with the same options, the same fault surface, and **byte-identical output**:

```
// Definitional expansions, not compilable syntax (the read pair is compiler-known
// — §3 Q4 — so those bodies are the compiler's, not writable user code; the write
// pair is ordinary generic code and its expansions are literally its bodies):

Json.ToText(value)              ≡  using var s := new JsonSerializer();
                                   value.Serialize(s);  return s.ToString();
Json.ToText(value, options)     ≡  … new JsonSerializer(options) …
Json.ToBytes(value[, options])  ≡  … return s.ToBytes();
Json.Parse<T>(input[, options]) ≡  using var d := new JsonDeserializer(input[, options]);
                                   return T.Deserialize(d);      // §2.14 recognized factory
Json.ParseOwned<T>(…)           ≡  the same, at class T (owned T transfers out)
Binary.ToBytes(value)           ≡  using var s := new BinarySerializer();
                                   value.Serialize(s);  return s.ToBytes();
Binary.Parse<T>(input[, flag])  ≡  using var d := new BinaryDeserializer(input[, flag]);
                                   return T.Deserialize(d);
```

Consequences, stated so they are testable: a facade write and a hand-driven agent write of the same value under the same options produce identical bytes; a facade read accepts exactly the inputs the agent read accepts and raises exactly the exceptions it raises (including §2.15's trailing-data conditions — the facade is a whole-document read); any future change to the agents (0010's value-builtin conformances, a `JsonOptions` extension) flows through the facade **by construction**, with no facade-side re-specification. The binary format's §2.15 caveat — compact, same-version persistence and IPC, *not* a version-stable interchange format — applies verbatim to `Binary.ToBytes`/`Parse` and is restated in their doc comments.

---

## 6. Proposed API surface (all members new; module `System.Serialization`)

```bezel
// PROPOSED — the JSON facade. Write members are ordinary constrained generics;
// Parse/ParseOwned are compiler-known (§3 Q4; the 0001 §3.3 dispensation).
public static class Json
  // ── write: value → encoded form (borrows value for the call) ──
  public static method ToText<T>(value: T): string where T: ISerializable;
  public static method ToText<T>(value: T, options: JsonOptions): string where T: ISerializable;
  public static method ToBytes<T>(value: T): owned OwningBuffer<byte> where T: ISerializable;
  public static method ToBytes<T>(value: T, options: JsonOptions): owned OwningBuffer<byte> where T: ISerializable;

  // ── read: encoded form → value. Record/value/enum T only; class T is a
  //    compile-time error directing to ParseOwned<T>. Input forms mirror the
  //    §2.15 JsonDeserializer constructor set exactly (string / ReadOnlySpan /
  //    ReadOnlyArraySpan, each with a JsonOptions twin — v0.215 parity). ──
  public static method Parse<T>(text: string): T where T: IDeserializable;
  public static method Parse<T>(text: string, options: JsonOptions): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>, options: JsonOptions): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>, options: JsonOptions): T where T: IDeserializable;

  // ── read, class T (IDeserializable-conforming; compiler-known check, as
  //    0001 §3.3's JsonOwned<T>). The signature spells `where T: class` only,
  //    inheriting 0001 §3.3's exact spelling; the combined
  //    `where T: class, IDeserializable` is grammatical (Appendix A) and may
  //    be preferred at landing. Same six input forms as Parse<T>. ──
  public static method ParseOwned<T>(text: string): owned T where T: class;
  public static method ParseOwned<T>(text: string, options: JsonOptions): owned T where T: class;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>): owned T where T: class;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>, options: JsonOptions): owned T where T: class;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>): owned T where T: class;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>, options: JsonOptions): owned T where T: class;
end Json;

// PROPOSED — the binary facade. No text form (the format is bytes, §3 Q3); the
// option is §2.15's constructor bool, mirrored — binary has no options record.
public static class Binary
  public static method ToBytes<T>(value: T): owned OwningBuffer<byte> where T: ISerializable;

  public static method Parse<T>(input: ReadOnlySpan<byte>): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool): T where T: IDeserializable;
  public static method Parse<T>(input: ReadOnlyArraySpan<byte>): T where T: IDeserializable;
      // no (ReadOnlyArraySpan, ignoreUnknownFields) form: §2.15 declares no such
      // constructor — mirrored, not silently completed (open question Q3)

  public static method ParseOwned<T>(input: ReadOnlySpan<byte>): owned T where T: class;
  public static method ParseOwned<T>(input: ReadOnlySpan<byte>, ignoreUnknownFields: bool): owned T where T: class;
  public static method ParseOwned<T>(input: ReadOnlyArraySpan<byte>): owned T where T: class;
end Binary;
```

**Not covered in v1, deliberately** (each stays on the agent path, which remains fully public): shared-node roots — a class whose recognized factory has the third §2.14 shape `Deserialize(d): owned Shared<C>` has no facade spelling (`ParseOwned<C>` would mistype the transfer; a `ParseShared<C>(…): owned Shared<C>` is a mechanical later addition if a workload names it — open question Q2); multi-value streams and incremental writing; user formats (any non-stdlib `ISerializer`/`IDeserializer`).

---

## 7. Usage examples

```bezel
import System.Serialization;

// 1. §2.15's own SavePoint/LoadPoint pair, restated through the facade.
//    (Point: the §2.14 example record, ISerializable + IDeserializable.)
public method SavePoint(p: Point): owned OwningBuffer<byte>
  return Json.ToBytes(p);                      // was: new JsonSerializer / Serialize / ToBytes
end SavePoint;

public method LoadPoint(bytes: ReadOnlySpan<byte>): Point
  return Json.Parse<Point>(bytes);             // was: new JsonDeserializer / Point.Deserialize
end LoadPoint;

// 2. A class DTO (Order: the §2.14 example class) — owned, scope-bound:
using var order := Json.ParseOwned<Order>(text);

// 3. Options thread through unchanged (opts: JsonOptions, §2.15 — e.g. Indented
//    for a human-facing config file; IgnoreUnknownFields for a tolerant read):
var pretty := Json.ToText(config, opts);
var cfg := Json.Parse<Config>(File.ReadAllText(path), opts);

// 4. Binary round-trip to disk, composing with the §7.3 statics — WriteAllBytes'
//    span overload is the v0.165 "serializer-output path, zero-copy":
using var blob := Binary.ToBytes(state);
File.WriteAllBytes(path, blob.AsReadOnlySpan());
// …
using var raw := File.ReadAllBytes(path);       // owned Array<byte> (§7.3)
var state := Binary.Parse<GameState>(raw.AsReadOnlySpan());   // §4.4 span view over the array
```

---

## 8. Gained and lost

**Gained.** The three-line dance collapses to one line at the several-hundred call sites the review measured; the at-rest vocabulary aligns with the wire vocabulary (0001/0002), so "how do I read JSON?" has one answer shape everywhere; the deserializer's input-span lifetime obligation disappears from call sites (§4); and the strict-by-default fault posture of §2.15 is reached with zero opportunity to mis-drive the agent (no forgotten `using`, no half-consumed deserializer).

**Lost / risked — the "two homes" problem, and its answer.** Every facade splits a capability across two surfaces: users may learn `Json.Parse` and never discover the agents, then miss that streaming, multi-value, and custom-format work lives there. The §7.3 precedent answers this: `File.ReadAllText` beside `FileStream` has exactly this shape, and the library's convention (facade doc points at the plumbing; plumbing stays the extension point) has not produced confusion there. The facade's doc comment states the delegation explicitly — "equivalent to constructing the §2.15 agent; for streaming or custom formats use the agents" — and §5's byte-identical guarantee means there is never a *behavioral* fork between the homes, only an ergonomic one.

---

## 9. Migration

Purely additive. No existing signature changes; no test changes are required (existing agent-driving sites remain the plumbing's regression coverage and stay valid forever). New code and docs prefer the facade for whole-value one-shots; the §2.15 examples gain facade-first restatements at graduation, with the agent forms retained beneath them as the definitional expansion.

---

## 10. Considered and declined (register)

1. **A reflection-based generic serializer** (`Serializer.Write(anyValue)` discovering shape at runtime) — declined without reach: rationale [§5.18](../Bezel_Design_Rationale.md#sec-type-identity-not-reflection)/[§5.19](../Bezel_Design_Rationale.md#sec-serialization-without-reflection) is the architecture's founding refusal, and this facade is the *no-reflection* answer to the same ergonomic pressure — compile-time per-T instantiation over synthesized conformances, shape resolved statically, nothing discovered at runtime.
2. **An instance-method facade** (`value.ToJson()`, e.g. as an extension on `ISerializable`) — declined: it stamps format-specific vocabulary onto the surface of every conforming DTO in the program (the §2.14 conformances are deliberately format-agnostic), and the read direction has no instance to hang from (deserialization is static, §2.14), so the pair would be structurally lopsided — writes on values, reads somewhere else anyway.
3. **`Parse` on the target types themselves** (`User.ParseJson(text)`, or overloading the types' `Parse`) — declined: [§2.19](../Bezel_Standard_Library.md#sec-static-abstractions)'s `IParsable<T>` already gives `Parse(s: string)` a pinned document-wide meaning — lexical parse of the type's canonical string form, `FormatException` on failure — and a type conforming to both would carry two `Parse` families with different grammars and different fault types (`FormatException` vs `SerializationException`). The format-named static class keeps the verbs apart; `Deserialize` remains the one reserved per-type name (§2.14).
4. **`Json.FromText`/`FromBytes` naming** — declined: `Parse` is the document-wide verb for encoded-form-to-value (§2.5, §2.6, §9.1, 0019's `JsonValue.Parse`), and `ParseOwned` mirrors 0001's `JsonOwned` split; `From*` would coin a second verb for the same act.
5. **Options as defaulted parameters** (`options: JsonOptions := …`) instead of overload twins — declined: §2.15 has no `JsonOptions` default constant to name, and the agent surface itself is overload-shaped; the facade mirrors it rather than inventing a default-value story the module never defined.
6. **A JSON-only facade** — declined per the review critique: the binary dance is the more frequent one, and one-format convenience manufactures the asymmetry 0067 exists to remove (§1).

---

## 11. Open questions for the owner

1. **The 0019 naming collision (load-bearing).** Confirm option (a) of §3 Q5 — facade named `Json`, 0019's decline register amended at its graduation with `JsonValue.Parse` unmoved as the DOM entry — or direct (b)/(c). The facade's shape is identical under any of the three; only the spelling moves.
2. **Shared-node roots.** Confirm that the `owned Shared<C>` factory shape (§2.14's third form) stays agent-only in v1, with `ParseShared<C>` recorded as the mechanical addition if a workload names it — or include it now for three-shape completeness.
3. **The mirrored `BinaryDeserializer` gap.** §2.15 declares no `(ReadOnlyArraySpan<byte>, ignoreUnknownFields)` constructor, so the facade mirrors the hole (§6). Land a piggyback pin completing the agent constructor set (and then the facade form) in the 0067-Q5 style, or leave both sides as-is?
4. **Binary timing against 0010.** This proposal recommends **not** waiting: 0010's scope is the token vocabulary (`WriteBytes`, now frozen) and per-type conformances *beneath* the agents — and §5 defines the facade by delegation, so 0010's landings flow through with no facade change; the binary format was already version-local before 0010 and remains so after (§2.15). The seam is clean in the other direction too: nothing in this proposal touches the vocabulary 0010 froze. Confirm, or sequence `Binary` after 0010 lands if the owner prefers one binary-surface event per revision.
5. **A `TryParse<T>` dual** — deferred out of v1 on §2.15's structural-error stance (§3 Q6); confirm the deferral or name the untrusted-input workload that would justify the Try shape now.

---

## 12. Relationship to existing work

- **§2.14/§2.15** — wrapped, unchanged. The facade is definitionally the agent sequence (§5); the vocabulary, the synthesis, the options types, and the fault type are all reused with zero additions.
- **Proposals 0001/0002** (draft/accepted) — the wire precedent and the compiler-known dispensation this proposal extends to at-rest use (§3 Q4); the `Parse`/`ParseOwned` split is their `Json`/`JsonOwned` split under the at-rest verb.
- **Proposal 0019** (draft) — the recorded `Json`-static-class decline, engaged as this proposal's central open question (§3 Q5); `JsonValue.Parse` remains the schemaless DOM entry under every naming option.
- **Proposal 0010** (draft) — vocabulary-frozen substrate work beneath the agents; facade-transparent by §5's delegation semantics (open question Q4).
- **Proposal 0067** (superseded — graduated v0.228) — the symmetry discipline that forced the both-formats scope, and the piggyback-pin pattern reused by open question Q3.
- **Proposal 0009** (accepted) — Base64/hex: the composition answer for "binary as text" (§3 Q3), keeping `Binary.ToText` correctly absent.
- **§7.3 `File`** — the facade-over-plumbing precedent (§8), and the zero-copy composition partner in §7's examples.
