# Proposal 0019: JSON DOM (`JsonValue` hierarchy)

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | [#1461](https://github.com/s8lap/bezel-lang/issues/1461) |
| **Target module(s)** | new `System.Text.Json` (the module named-but-deferred in stdlib §11); relates to §2.14/§2.15 (the serialization vocabulary + JSON format agent) without extending them |
| **Written against** | spec **v0.177** / stdlib **v0.165** |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-08-11 to v0.246: `closed` replaces `sealed abstract` (v0.242, §11.7/§19.6 quotations re-derived from the landed text), the no-binding match arm respelled as a bare type pattern per v0.244's reserved `_`. Refreshed 2026-09-17 to v0.306: the §19.6 and §19.3 pattern quotations re-derived from the landed text (v0.301 admits a **named** destructuring over a class-typed pattern head; only the *positional* form is class-free), and the Q1 rejected-alternatives row conformed. ⚠ One tier-2 flag recorded in `proposals-currency-2026-09-17.md` §3: v0.301's named-destructure surface makes `JsonString(Value = var s)` writable over these class leaves, which is a design choice Q1 never had. Written-against pin unchanged. |
| **Wave** | D3 (gap-review item #21) |
| **Depends on** | nothing to ship the parse/build/query core. The `WriteTo(TextWriter)` streaming-emit variant rides proposal 0006 (concrete `StreamWriter`/`TextWriter`); the embedded-field *read* round-trip rides an owner ruling (Open Question 1). |

## Problem

Bezel has a *type-driven* JSON path (`JsonSerializer`/`JsonDeserializer` in `System.Serialization`, §2.15): a value whose static type is known is (de)serialized against a target shape. It has **no** way to handle JSON whose shape is *not* known at compile time — a config file with an open-ended tail, a third-party API that adds fields, a `data:`-style payload, a message whose discriminant selects the schema only at runtime. stdlib §11 records this gap precisely: `System.Text.Json` "would offer document-tree navigation and ad-hoc querying of arbitrary JSON not driven by a target type … complementary, as they are in other ecosystems that provide both a serializer and a document API."

This is also the **routing target for DECLINED.md D-0002** (anonymous types): "Truly dynamic data goes through the JSON DOM (gap review item 21): runtime-shaped values behind typed faults, quarantined at the parse boundary, never a `dynamic` type." So the DOM is not a convenience — it is the sanctioned home for the entire class of "shape-at-runtime" needs that Bezel otherwise refuses. Its design must honour that quarantine posture: **no `dynamic`, no implicit conversions, every kind-mismatch is a typed fault, and the runtime-shaped value never escapes the DOM's typed surface.**

This proposal designs the read/build/navigate/serialize core of that DOM and defers the query language, streaming reader, and schema validation with one-line rationale each.

---

## Existing surface this builds on (enumerated before extending)

Grep-verified against stdlib v0.177 / spec v0.177; cited, not invented:

- **`closed class`** — spec §11.7: "A class declared with the **`closed`** modifier is a **closed abstract class** … The `closed` modifier is the recommended way to define a closed hierarchy for exhaustive `case` matching (§19.6)." (Spelled `sealed abstract` through v0.241; respelled v0.242.) Subclasses must live in the declaring module; exhaustiveness holds across module boundaries (§19.6).
- **Exhaustive `case` over a closed hierarchy** — spec §19.6: "when the scrutinee's static type is a `closed` class … and every leaf type in the closed hierarchy is matched by a type pattern (with or without binding), no `_` arm is required. … Class leaves are matched by type-test patterns (`var c: Circle => ...`) and — equivalently for coverage — by **named** destructuring patterns whose head names the leaf (`Circle(Radius = var r) => ...`); it is **positional** destructuring (§19.3) that applies to record leaves and not to class leaves." (The named-destructure half was added at v0.301; the positional restriction this proposal relies on is unchanged.)
- **Records cannot own, and cannot be leaves of a class hierarchy** — spec §9.6: "A record cannot derive from another record or from a class"; §28.5/§9.6: "Records cannot have destructors, `owned` fields, or implement `IDisposable`."
- **Owned trees + cascade teardown** — spec §28.4 (destructor), §28.5 (owned fields: an owned field is disposed by the owner's cascade), §28.6 (leak prevention), §28.8 / §28.8.1 (ownership transfer via `move`-and-null). Precedent: proposal 0004's owned node trees (README: "owned node trees with cascade teardown").
- **`OwningDictionary<K, V>` (§4.9)** — `where V: class`; get returns a **borrow**, `set(owned)` disposes the previous value; `PopValue(key): owned V` (the take-out), `Remove` disposes, `Add(key, owned value)`. **`OwningArray<T>` (§4.5)** — `where T: class`; indexer-get borrow, `set(owned)`; `Add(owned)`. Both require `T`/`V: class`, which `JsonValue` (a class) satisfies.
- **Owned-taking setters** — spec §23.3: `set(owned)` "takes ownership of the new value (and disposes the previous slot value)".
- **Null-conditional access** — spec §7.5: `x?.M` short-circuits to `null`. **There is no `?[` null-conditional *indexer*** (grammar-verified: Appendix A has `?.` and `?(args)` only). This single fact drives the query-API shape below.
- **`ISerializer`/`IDeserializer` token vocabulary (§2.14)** — `WriteInt`/`WriteUInt`/`WriteFloat`/`WriteDecimal`/`WriteBool`/`WriteString`/`WriteChar`/`WriteNull`/`BeginObject`/`WriteFieldName`/`EndObject`/`BeginArray`/`EndArray`; read duals. **Frozen** as of proposal 0010 (open-Q2 resolved 2026-07-04): "the token set is treated as **frozen**: any future token addition is a **breaking, rationale-recorded major event**."
- **`JsonSerializer`/`JsonDeserializer`/`JsonOptions`/`SerializationException` (§2.15)** — in `System.Serialization`, *not* auto-imported. `JsonOptions.MaxDepth` (default 64) is "a nesting guard"; a depth violation raises `SerializationException` — the **defined-fault** precedent this DOM mirrors.
- **Defined-fault philosophy** — spec §31.4: `StackOverflowException` "cannot be caught; terminates the program." A depth-bounded parser must raise a *catchable, defined* fault instead, exactly as §2.15's `MaxDepth` does.
- **Reusable faults** — `KeyNotFoundException` (stdlib §4.8), `IndexOutOfRangeException` / `FormatException` / `OverflowException` (spec §31.4). Number parsing already pins `FormatException` (bad form) + `OverflowException` (out of range) — stdlib §3.7 (`int.Parse`).
- **v0.171 `Try`-convention** — `out result: T?` + `bool`; `null` on `false`; never throws (§4.9 `TryGetValue`/`TryPopValue`).

---

## Design question 1 — the hierarchy: `closed` hierarchy vs single class + kind enum

### Decision — a `closed class JsonValue` with six `sealed class` leaves.

```bezel
public closed class JsonValue
  public abstract readonly property Kind: JsonKind;
  // accessors, navigation, and serialization — see Q3/Q4/Q6
end JsonValue;

public sealed class JsonObject: JsonValue  end JsonObject;
public sealed class JsonArray:  JsonValue  end JsonArray;
public sealed class JsonString: JsonValue  end JsonString;
public sealed class JsonNumber: JsonValue  end JsonNumber;
public sealed class JsonBool:   JsonValue  end JsonBool;
public sealed class JsonNull:   JsonValue  end JsonNull;

public enum JsonKind: byte
  Object, Array, String, Number, Bool, Null
end JsonKind;
```

**Why the closed hierarchy wins.** §11.7 names `closed` as "the recommended way to define a closed hierarchy for exhaustive `case`," and §19.6 lets the compiler prove exhaustiveness with no `_` arm across module boundaries. A consumer processes any JSON value totally:

```bezel
public method Describe(v: JsonValue): string
  return (case v of                         // exhaustive — no _ arm required (§19.6)
    var o: JsonObject => $"object({o.Count})",
    var a: JsonArray  => $"array({a.Count})",
    var s: JsonString => $"\"{s.Value}\"",
    var n: JsonNumber => n.Raw,
    var b: JsonBool   => (if b.Value then "true" else "false"),
    JsonNull          => "null");     // bare type pattern — no binding (§19.3; `var _` is not declarable, §17.13)
end Describe;
```

The alternative — **one `JsonValue` class carrying a `JsonKind` field plus a `string?`, a `double`, an `OwningDictionary?`, … chosen by the kind** — is declined:

1. **It makes illegal states representable.** A `Kind.String` instance with a populated members map, or a `Kind.Object` with a null map, is constructible; every accessor must re-validate. The closed hierarchy makes those states *unconstructible* — a `JsonString` has a `Value` and nothing else.
2. **It loses compiler-proven exhaustiveness.** A `case` over `JsonKind` is an enum match (§19.6 proves it too) — but the *payload* still has to be fished out of union-typed fields with a runtime "impossible kind" branch. The type-per-kind design binds the payload to the arm in the pattern.
3. **The union fields waste space and invite the `dynamic` smell** the D-0002 posture exists to prevent.

**Correction to the brief's suggested syntax (a real spec finding).** The mission brief sketches `case v of JsonString(var s) => …`. That form is **not available** and would not compile: *positional* destructuring gives classes no pattern surface (§19.3: "Positional destructuring applies to records and choice cases … Classes do not have a positional-destructuring surface, even when they declare a primary constructor"; §19.6: "it is **positional** destructuring … that applies to record leaves and not to class leaves"). (Since v0.301 the *named* form **is** admitted over a class-typed head — `JsonString(Value = var s)` — see the ⚠ currency flag in the header; the positional sketch the brief writes is still rejected.) The leaves here **cannot** be records — a record cannot derive from a class (§9.6) and cannot own the child tree (§28.5) — so they are classes, and classes are matched by **type-test patterns** (`var s: JsonString => s.Value`), reading the field through the bound name. The exhaustive-match example above uses the valid form. A hierarchy mixing record leaves (for scalars) and class leaves (for containers) is impossible for the same reason; all six leaves are classes.

`Kind` is retained as a lightweight predicate (`if v.Kind == JsonKind.String then …`) for the non-exhaustive one-off check, but the **type identity is the source of truth** and `Kind` is derived from it (each leaf overrides `Kind` with its constant).

---

## Design question 2 — ownership: an owned tree with cascade teardown

### Decision — `Parse` returns `owned JsonValue`; every child is an `owned` field; the whole tree dies with the root (§28.4/§28.5 cascade); access returns **borrows** (§28.6); extraction returns **owned** (§28.8).

```bezel
public sealed class JsonObject: JsonValue
  private owned _members: OwningDictionary<string, JsonValue>;   // §4.9; V: class ✓
  public constructor();
  public override readonly property Kind: JsonKind => JsonKind.Object;

  public readonly property Count: int;
  public readonly property Keys: IReadOnlyCollection<string>;    // borrowing view (§4.9)
  public method ContainsKey(key: string): bool;

  // Build / mutate (Q5). All owned-taking; disposal is the OwningDictionary's job.
  public method Add(key: string, owned value: JsonValue);        // ArgumentException if present
  public method Set(key: string, owned value: JsonValue);        // add-or-update; disposes previous
  public method Remove(key: string): bool;                       // disposes the removed child
  public method PopValue(key: string): owned JsonValue;          // take-out (§28.8); raises if absent
  public method TryPopValue(key: string, out owned value: JsonValue?): bool;   // v0.171
end JsonObject;

public sealed class JsonArray: JsonValue
  private owned _items: OwningArray<JsonValue>;                  // §4.5; T: class ✓
  public constructor();
  public override readonly property Kind: JsonKind => JsonKind.Array;

  public readonly property Count: int;
  public method Add(owned value: JsonValue);
  public method Insert(index: int, owned value: JsonValue);      // IndexOutOfRangeException
  public method SetAt(index: int, owned value: JsonValue);       // disposes previous
  public method RemoveAt(index: int): bool;                      // disposes
  public method PopAt(index: int): owned JsonValue;              // take-out (§28.8)
end JsonArray;
```

- **The tree is a single-owner spanning tree.** A `JsonObject` owns its `OwningDictionary`, which owns each child `JsonValue`, recursively. Dropping the root disposes everything beneath it through the standard owned-field cascade (§28.5) — no per-node teardown code, no `IDisposable` boilerplate on `JsonValue` itself beyond what the compiler generates for a class with owned fields (§28.4). This is exactly proposal 0004's owned-node-tree shape.
- **Reads borrow (§28.6).** The navigation indexers (Q4) and `Keys`/`Values` hand back borrows of children the DOM still owns — the `OwningDictionary`/`OwningArray` indexer-get borrow semantics (§4.9/§4.5), lifted to the DOM surface.
- **Extraction transfers (§28.8).** `PopValue`/`PopAt` are the sanctioned take-out shapes, delegating to `OwningDictionary.PopValue` / `OwningArray` (`move`-and-null, §28.8.1). The caller receives an `owned JsonValue` and owns its subtree.
- **`JsonNull` is an ordinary owned node**, not a shared singleton. An owned tree does not share nodes (that is `Shared<T>`, §28.13 — a separate opt-in not used here); each JSON `null` is its own `new JsonNull()`. This keeps teardown uniform and avoids a cross-tree alias.

The scalar leaves are immutable value carriers:

```bezel
public sealed class JsonString: JsonValue
  public constructor(value: string);
  public override readonly property Kind: JsonKind => JsonKind.String;
  public readonly property Value: string;
end JsonString;

public sealed class JsonBool: JsonValue
  public constructor(value: bool);
  public override readonly property Kind: JsonKind => JsonKind.Bool;
  public readonly property Value: bool;
end JsonBool;

public sealed class JsonNull: JsonValue
  public constructor();
  public override readonly property Kind: JsonKind => JsonKind.Null;
end JsonNull;
```

---

## Design question 3 — number representation

### Decision — `JsonNumber` stores the **raw JSON lexeme** and exposes lazily-parsed typed accessors. Not `double`. Not `decimal`-only.

```bezel
public sealed class JsonNumber: JsonValue
  public override readonly property Kind: JsonKind => JsonKind.Number;
  public readonly property Raw: string;                     // the exact token text

  // Build side — produce the canonical lexeme:
  public static method FromInt(value: long): owned JsonNumber;
  public static method FromUInt(value: ulong): owned JsonNumber;
  public static method FromDouble(value: double): owned JsonNumber;   // ArgumentException on NaN/±Inf
  public static method FromDecimal(value: decimal): owned JsonNumber;

  // Read side: no leaf accessors. The numeric reads (AsInt/AsLong/AsDouble/AsDecimal)
  // live once on the base JsonValue (Q4), which type-test-dispatches to a JsonNumber and
  // parses Raw on demand. Redeclaring them here would hide the base members, which §12.2
  // forbids; JsonNumber therefore adds only Raw + the From* factories.
end JsonNumber;
```

**Why the raw lexeme.** A JSON number (RFC 8259 §6) is an arbitrary-precision *decimal grammar* — it is neither `int`/`long`, nor `double`, nor even `decimal` (a `1e400` is valid JSON, unrepresentable in any of them). Committing to one native type **at parse time** is the classic DOM bug: parse-as-`double` silently mangles `9007199254740993` (an `int64` that is not an exact `double`) and any high-precision decimal; parse-as-`decimal` cannot hold the full `double` range or large exponents. Storing the token text verbatim:

- **preserves exact JSON→JSON round-trip** (the emitter writes `Raw` back unchanged);
- **defers the int-vs-float choice to the consumer that knows the expected type** — `AsInt()` / `AsLong()` / `AsDouble()` / `AsDecimal()` parse `Raw` on demand and raise `FormatException` (wrong form for the requested type — e.g. `AsInt()` on `"1.5"`) or `OverflowException` (out of range), reusing the exact fault contract of `int.Parse` (stdlib §3.7). This is the quarantine posture in miniature: the number is a runtime-shaped value behind typed faults.

**Serialize-via-`ISerializer` canonicalization (used by Q6's `Serialize`).** When a `JsonNumber` is emitted through the *token vocabulary* (not the DOM's own text emitter, which writes `Raw` verbatim) it must pick exactly one numeric token. The rule, documented and deterministic: integral lexeme fitting `long` → `WriteInt`; integral fitting `ulong` but not `long` → `WriteUInt`; else exactly representable as `decimal` → `WriteDecimal` (lossless, §2.15); else → `WriteFloat` (the one honest lossy edge, reached only by values beyond `decimal` range, e.g. huge exponents — matching every DOM's behaviour and §2.15's own float mapping). Against a `JsonSerializer` target this reproduces the value faithfully in all but that last edge.

**Declined alternatives:** `double`-backed (loses `int64`/decimal precision — the classic mistake); `decimal`-only (cannot hold all `double` values or large exponents); an eager `int`-or-`double` discriminated union (still lossy for values outside both, and re-introduces the "which arm?" branch the lexeme avoids).

---

## Design question 4 — access ergonomics vs quarantine

### Decisions

**(a) Typed accessors on the base, faulting on kind-mismatch, with v0.171 `Try`-duals.** No implicit conversions (D-0002 posture) — a wrong kind *always* faults; an accessor never coerces.

```bezel
// on JsonValue (base):
public method AsString(): string;      // JsonTypeException unless this is JsonString
public method AsBool(): bool;          // JsonTypeException unless this is JsonBool
public method AsInt(): int;            // JsonTypeException unless JsonNumber; then Format/Overflow per Q3
public method AsLong(): long;
public method AsDouble(): double;
public method AsDecimal(): decimal;
public method AsObject(): JsonObject;  // borrow narrowing; JsonTypeException unless JsonObject
public method AsArray(): JsonArray;    // borrow narrowing; JsonTypeException unless JsonArray
public readonly property IsNull: bool; // true iff this is JsonNull — the one total predicate

// Try-duals (v0.171): bool + out T?; null on false; never throw:
public method TryGetString(out value: string?): bool;
public method TryGetInt(out value: int?): bool;
public method TryGetDouble(out value: double?): bool;
public method TryGetObject(out value: JsonObject?): bool;   // out is a borrow
public method TryGetArray(out value: JsonArray?): bool;
```

The base bodies dispatch by type-test — no per-leaf override needed:

```bezel
public method AsString(): string
  return (case this of
    var s: JsonString => s.Value,
    _ => throw new JsonTypeException(expected := JsonKind.String, actual := this.Kind));
end AsString;
```

**(b) Path access — the most-used call — is settled two ways, deliberately, because Bezel has no `?[`.**

- **Strict indexer chain** `v["a"][2]["b"]` (borrow-returning, get-only, on the base): kind-mismatch → `JsonTypeException`; **missing key → `KeyNotFoundException`** (matching `Dictionary`/`OwningDictionary`, §4.8/§4.9); out-of-range → `IndexOutOfRangeException`. Use when the path *must* exist and its absence is a bug.

  ```bezel
  public property [key: string]: JsonValue get;   // borrow; JsonTypeException if not-object; KeyNotFoundException if absent
  public property [index: int]:  JsonValue get;   // borrow; JsonTypeException if not-array; IndexOutOfRangeException if oob
  ```

- **Chain-friendly optional navigation** via **methods** `Get(...)`, so `?.` short-circuits (there is no `?[`, so this *must* be a method — §7.5):

  ```bezel
  public method Get(key: string): JsonValue?;     // null if this is not an object OR the key is absent
  public method Get(index: int):  JsonValue?;     // null if this is not an array  OR out of range
  ```

  ```bezel
  var city: string? := root.Get("address")?.Get("city")?.AsString();
  //  short-circuits to null at any missing/mismatched hop (§7.5); never throws for absence
  ```

**(c) The missing-key decision (the load-bearing one): `Get` returns a nullable borrow — `null` for *absent*, never a `JsonNull` — and the strict indexer faults.** This is the crux, chosen for these reasons:

- **`null` ≠ `JsonNull` preserves a real JSON distinction.** `{"a": null}` and `{}` differ: the first has key `a` present with a null *value*, the second lacks `a`. `Get("a")` returns a **non-null borrow of a `JsonNull` node** for the first and **`null`** for the second — the caller can tell them apart (`v.Get("a")?.IsNull`). Collapsing absent into a returned `JsonNull` (the "always chainable via a null object" design) would erase that distinction and is declined.
- **No implicit anything.** Neither form coerces; the strict form faults, the optional form returns `null` and composes with `?.`/`??`. This is precisely the "runtime-shaped values behind typed faults, quarantined" posture of D-0002.
- **Why a fault (not `null`) for the *indexer***: the strict `[]` is the "I assert this exists" verb, and its violation is a defined, catchable fault carrying the key/index — consistent with the strict-by-default stance of §2.15 (unknown field → fault) and of the collections (`KeyNotFoundException`).

---

## Design question 5 — mutation

### Decision — the DOM is **mutable** (build-then-serialize is a first-class use case), with a minimal, owned-discipline surface.

Mutation is safe precisely because the tree is a single-owner owned tree (§28.5): `Add`/`Set`/`Insert`/`SetAt` are **owned-taking** (they consume an `owned JsonValue` and the backing `OwningDictionary`/`OwningArray` disposes any replaced child, §4.9/§4.5); `Remove`/`RemoveAt` dispose; `PopValue`/`PopAt` transfer ownership out. There is no aliasing to invalidate. Building a document:

```bezel
public method BuildGreeting(name: string): owned JsonValue
  var root := new JsonObject();
  root.Add("greeting", new JsonString("hello"));
  root.Add("to", new JsonString(name));
  var nums := new JsonArray();
  nums.Add(JsonNumber.FromInt(1));
  nums.Add(JsonNumber.FromInt(2));
  root.Add("counts", nums);                 // ownership of nums transfers into root
  return root;                              // owned tree returned; caller owns it
end BuildGreeting;
```

**Why no owned-taking indexer-set** (`root["k"] := v`): a read-write indexer on `JsonObject` would have to *hide* the base's get-only navigation indexer of the same signature (Q4), and member-hiding across the sealed hierarchy is a grammar subtlety not worth resolving in v1. `Set(key, owned value)` is the honest builder verb. (This is a deliberate, reasoned departure from the collections' "no `Set` method" convention of §4.8 — that convention applies to the collection family, and `JsonValue` is not in it; here the base indexer is read-only *by design*, so building cannot route through indexer-set.) An owned indexer-set is a candidate v2 nicety, noted, not shipped.

---

## Design question 6 — boundary with §2.15 and the schemaless tail

### Decisions

**(a) `Parse` lives on `JsonValue`** as a static factory — no separate `Json` static class:

```bezel
public closed class JsonValue
  public static method Parse(text: string,
                             maxDepth: int := 64,
                             allowTrailingCommas: bool := false): owned JsonValue;   // JsonParseException
  public static method Parse(utf8: ReadOnlySpan<byte>,
                             maxDepth: int := 64,
                             allowTrailingCommas: bool := false): owned JsonValue;
  public static method TryParse(text: string, out owned value: JsonValue?): bool;    // v0.171; no throw

  public method ToJsonString(indented: bool := false, indentWidth: int := 2): string; // emits Raw for numbers
  public method Serialize(s: ISerializer);                                            // ISerializable (see (c))
end JsonValue;
```

`JsonValue.Parse` mirrors `Guid.Parse`/`DateTime.Parse`/`int.Parse` (static `Parse` on the type it produces). A `Json.Parse` static class was considered and declined: a whole type for one method when the value type is the natural home, and it would fork `TryParse`/`ToJsonString` awkwardly across two types. The byte-span overload mirrors `JsonDeserializer`'s UTF-8-bytes construction (§2.15). **Options are named-default parameters, not a new options record**: `JsonOptions` (§2.15) is deliberately *not* reused — it lives in a different module, and carries serializer-only concerns (`Indented`, `IgnoreUnknownFields`) orthogonal to DOM parsing. Two knobs (`maxDepth`, `allowTrailingCommas`) do not warrant a type.

**(b) The DOM lives in `System.Text.Json`, not auto-imported** (like `System.Serialization`, §2.15): a program that uses the DOM writes `import System.Text.Json`. This is the module stdlib §11 reserved by name.

**(c) `JsonValue` implements `ISerializable` — the write bridge — giving the "schemaless tail in a typed envelope" *write* path for free.** The DOM knows its own shape, so it maps cleanly onto the frozen token vocabulary (§2.14):

| leaf | tokens emitted |
|---|---|
| `JsonObject` | `BeginObject(Count)` · per member `WriteFieldName(k)` + child `Serialize` · `EndObject` |
| `JsonArray`  | `BeginArray(Count)` · per element child `Serialize` · `EndArray` |
| `JsonString` | `WriteString(Value)` |
| `JsonBool`   | `WriteBool(Value)` |
| `JsonNull`   | `WriteNull()` |
| `JsonNumber` | one of `WriteInt`/`WriteUInt`/`WriteDecimal`/`WriteFloat` per the Q3 canonicalization |

Because `JsonValue` is a class, an `owned JsonValue` field is an **owning edge** (§9.8), and a class that declares `: ISerializable` and whose serialized components are each serialization-eligible has its `Serialize` synthesized (§9.8, class serialization synthesis). The envelope must therefore be a **sealed class**, not a record: a record **cannot hold an `owned` field** (§9.7), so the schemaless tail — an owned child tree — can only be owned by a class. So a typed envelope with a schemaless tail is *write*-eligible today:

```bezel
public sealed class Envelope: ISerializable   // a class owns its fields (§9.8); serialized shape is read from the field types
  public readonly property Kind: string;
  private owned _payload: JsonValue;          // the schemaless tail, quarantined behind JsonValue's typed surface
  public constructor(kind: string, owned payload: JsonValue);
  public method Serialize(s: ISerializer);    // synthesized: a string component + an owned ISerializable class-typed edge (§9.8)
end Envelope;
```

**Dependency — the §9.7/§9.8 contradiction (owner ruling).** §9.8's class-in-record paragraph states that when a class-typed component appears in a record "the record field must be `owned`", which directly contradicts §9.7's flat "Records cannot have … `owned` fields." The sealed-class envelope above sidesteps the contradiction (classes unambiguously own fields), so the *write* path ships regardless; but the record-envelope convenience form is blocked until that contradiction is ruled. This proposal takes the class form and records the dependency.

**(d) The embedded-field *read* round-trip is deferred to an owner ruling (Open Question 1).** `JsonValue` does **not** implement `IDeserializable` in v1, for a structural reason: reconstructing a DOM from the abstract `IDeserializer` requires reading a value **of unknown kind** — but the vocabulary is *typed-pull* (`ReadInt` vs `ReadString`; the target type's `Deserialize` is expected to know its own shape) and has **no value-kind peek**. Adding one is a **breaking change to a frozen vocabulary** (proposal 0010 open-Q2). The whole-document schemaless case needs none of this — `JsonValue.Parse(text)` reads raw JSON directly. Only the *embedded field* case (an envelope type — the §6c sealed class — with an owned `JsonValue` field declaring `: IDeserializable`) needs the peek, and it is scoped out of v1. Consequence, stated honestly: an envelope with an owned `JsonValue` field can declare `: ISerializable` (write) but **not** `: IDeserializable` in v1 (the conformance would be a compile error naming the field, §9.8).

---

## Design question 7 — faults (a `JsonException` taxonomy, defined not fatal)

Declared in `System.Text.Json` alongside their producer (the stdlib convention, §0/§4.8: producer-local exceptions live with their producer):

```bezel
public class JsonException: Exception                       // base for all DOM faults
  public constructor(message: string);
end JsonException;

public class JsonParseException: JsonException              // malformed input at parse time
  public readonly property Line: int;                       // 1-based
  public readonly property Column: int;                     // 1-based
  public readonly property Position: int;                   // 0-based char/byte offset
end JsonParseException;

public class JsonDepthException: JsonParseException  end;   // nesting exceeded maxDepth — a DEFINED fault

public class JsonTypeException: JsonException               // accessor/navigation kind-mismatch
  public readonly property Expected: JsonKind;
  public readonly property Actual: JsonKind;
end JsonTypeException;
```

- **Parse errors carry position** (`Line`/`Column`/`Position`), mirroring `SerializationException`'s line/column (§2.15 "carries a byte offset … or line/column to locate the fault").
- **Depth is a *defined* fault, never `StackOverflowException`.** A recursive-descent parser on adversarial input (`[[[[…` a million deep) must not exhaust the C++ stack and hit the uncatchable `StackOverflowException` (§31.4). `JsonValue.Parse` bounds nesting at `maxDepth` (default 64, matching `JsonOptions.MaxDepth`, §2.15) and raises the catchable `JsonDepthException` — the exact defined-fault discipline §2.15 already applies. (An iterative parser with an explicit heap stack is the implementation that makes this bound real; noted for the implementer.)
- **Type mismatches** on accessors/indexers → `JsonTypeException(expected, actual)`. **Number range/form** faults reuse `OverflowException`/`FormatException` (Q3). **Missing key/index** on the *strict* indexer reuse `KeyNotFoundException`/`IndexOutOfRangeException` (Q4) — no new fault where a defined one already fits.
- **`Try*` members never throw** (v0.171); every other read faults on mismatch rather than returning a sentinel — the same stance as §2.15's deserializer ("a mismatch is a structural error in the input, not an expected control-flow branch").

---

## Explicitly out of scope for v1 (deferred, one line each)

- **JSONPath / a query language** — the `Get(...)`/indexer navigation covers direct paths; a query DSL is a separate proposal once real usage shows the need.
- **Streaming reader (`Utf8JsonReader`-style pull parser) and streaming writer** — rides gap-review item #1 / proposal 0006 (the concrete stream family); `WriteTo(TextWriter)` is the first landing when 0006 ships. The DOM is the *materialized-tree* API by definition.
- **JSON Schema validation** — an ecosystem concern; the typed-serialization path (§2.15) is the "known shape" story.
- **Async parse** — `Parse` is synchronous over an in-memory string/span; async belongs to the streaming reader above.
- **Comments / trailing-everything / JSON5** — v1 accepts strict RFC 8259 plus the single `allowTrailingCommas` leniency knob (matching `JsonOptions.AllowTrailingCommas`, §2.15).
- **Owned-taking indexer-set** (`obj["k"] := v`) — deferred on the member-hiding subtlety (Q5); `Set`/`Add` cover building.
- **Canonicalization / key sorting / de-duplication on parse** — the DOM preserves input order and duplicate keys are a parse fault; canonical output is a v2 emitter option.

---

## Proposed stdlib section (new `System.Text.Json`, sketch for the eventual §-delta)

```bezel
// System.Text.Json — NOT auto-imported (like System.Serialization, §2.15).
// import System.Text.Json;

public enum JsonKind: byte  Object, Array, String, Number, Bool, Null  end JsonKind;

public closed class JsonValue
  public abstract readonly property Kind: JsonKind;

  // Parse / emit (Q6)
  public static method Parse(text: string, maxDepth: int := 64, allowTrailingCommas: bool := false): owned JsonValue;
  public static method Parse(utf8: ReadOnlySpan<byte>, maxDepth: int := 64, allowTrailingCommas: bool := false): owned JsonValue;
  public static method TryParse(text: string, out owned value: JsonValue?): bool;
  public method ToJsonString(indented: bool := false, indentWidth: int := 2): string;
  public method Serialize(s: ISerializer);                     // : ISerializable (write bridge, Q6)

  // Typed accessors + Try-duals (Q4)
  public method AsString(): string;   public method TryGetString(out value: string?): bool;
  public method AsBool(): bool;       public method TryGetBool(out value: bool?): bool;
  public method AsInt(): int;         public method TryGetInt(out value: int?): bool;
  public method AsLong(): long;       public method AsDouble(): double;   public method AsDecimal(): decimal;
  public method AsObject(): JsonObject;  public method TryGetObject(out value: JsonObject?): bool;
  public method AsArray(): JsonArray;    public method TryGetArray(out value: JsonArray?): bool;
  public readonly property IsNull: bool;

  // Navigation (Q4): strict indexer (faults) + optional Get (nullable borrow, ?.-chainable)
  public property [key: string]: JsonValue get;    // borrow; JsonTypeException / KeyNotFoundException
  public property [index: int]:  JsonValue get;    // borrow; JsonTypeException / IndexOutOfRangeException
  public method Get(key: string): JsonValue?;      // null = absent or not-object
  public method Get(index: int):  JsonValue?;      // null = out-of-range or not-array
end JsonValue;

// leaves: JsonObject / JsonArray (Q2, mutable, owning) · JsonString / JsonBool / JsonNull (Q2) · JsonNumber (Q3)
// faults: JsonException ⊃ { JsonParseException ⊃ JsonDepthException, JsonTypeException }  (Q7)
```

`JsonValue` implements `ISerializable` (Q6c). It is **not** `IDeserializable` in v1 (Q6d). The whole hierarchy is `System.Text.Json`, explicit-import.

---

## Open questions (owner ruling requested before landing)

1. **Embedded-field read round-trip vs the frozen vocabulary.** To let an envelope (the §6c sealed class) with an owned `JsonValue` field declare `: IDeserializable`, the DOM needs to read an unknown-kind value from `IDeserializer` — impossible without a value-kind peek, and the token set is **frozen** (proposal 0010). Options: **(a)** add a `PeekValueKind()` / `ReadDynamicValue()` token — a rationale-recorded *breaking major event*; **(b)** a §9.8 synthesis *special-case* recognizing `JsonValue` fields and bridging the concrete `JsonDeserializer` (works only for the JSON format, not `BinaryDeserializer`); **(c)** leave it write-only in v1 (this proposal's default). Recommend **(c) now, revisit (a) if real usage demands round-tripping schemaless tails** — the freeze is young and its stability is load-bearing for §2.14's "serialize once, serialize to every format" guarantee.
2. **Duplicate object keys on parse.** RFC 8259 permits them and leaves behaviour to the implementation. Recommend **last-wins with a parse-time fault option**; simplest v1 is **fault** (`JsonParseException`) — strict-by-default, matching §2.15. Confirm.
3. **`AsInt()` on `"1.0"`.** Is a fractional-notation-but-integral-value lexeme (`1.0`, `2e0`) an `int`? Recommend **`FormatException`** (strict: `AsInt` wants integral *syntax*; `AsDouble().RoundTo…` is the explicit path) — but a "numerically integral" leniency is defensible. Confirm the strict reading.

---

## Delegated rulings 2026-08-11 (stdlib-wave readiness; owner review in repo)

Resolving the three open questions under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo) (delegated ruling, 0012-precedent style), plus one dependency discharge. Status stays draft.

1. **Q1 — (c) write-only in v1: RULED as recommended, with the reopening bar made exact.** The freeze this rests on was re-verified against the current docs: stdlib §2.14 at v0.247 is still the 13-write/15-read token set (0010's `WriteBytes`/`ReadBytes` remain proposal-only), and the freeze ruling (owner, 2026-07-04, 0010 Q2) is in force — any token addition is a rationale-recorded breaking major event. **(b) is declined outright**, not deferred: a §9.8 synthesis special-case bridging the concrete `JsonDeserializer` makes eligibility *format-dependent* for exactly one type (round-trips under JSON, silently ineligible under `BinaryDeserializer`), breaking §2.14's "serialize once, serialize to every format" guarantee at its only known exception — worse than the missing feature. **Reopening bar for (a):** a named consumer that must round-trip an *embedded* owned `JsonValue` field through the agent pair / §9.8 synthesis (whole-document schemaless data needs nothing — `JsonValue.Parse` covers it; and the v0.232 `Json` facade, being defined as the agent sequence, changes nothing here). When that consumer exists, the peek token is taken *as* 0010's defined breaking major event, deliberately and once.
2. **Q2 — duplicate keys FAULT (`JsonParseException`); no last-wins mode in v1.** New ground since the pin: the typed deserialization path's landed contract (stdlib §2.15) makes duplicate fields an **unconditional error regardless of leniency settings** — the DOM faulting keeps one duplicate discipline across both JSON readers, where a last-wins DOM would read data the typed path rejects. Last-wins also masks producer bugs (two writers disagreeing about one key is a defect, not a preference). A lenient-ingest knob is evidence-gated: a named consumer that must read third-party duplicate-key JSON files the want, and if admitted it is an explicit parse option, never the default.
3. **Q3 — strict: `AsInt()` on a fractional-notation lexeme (`1.0`, `2e0`) is `FormatException`.** `AsInt` asserts integral *syntax*; the numerically-integral leniency would make `1` and `1.0` indistinguishable at the call site and silently admit `2e0`, against the §2.18 strict-parse discipline. The explicit path (`AsDouble()` + explicit rounding) states the intent the leniency would hide. Consistent with `JsonNumber` storing the raw lexeme — the lexeme *is* the truth `AsInt` checks against.
4. **Dependency discharge (the §6c note).** The §9.7/§9.8 class-in-record contradiction this proposal recorded as an owner-ruling dependency is **resolved in the current stdlib**: at v0.247, §2.14's record clause states a record cannot hold a class-typed serialized component at all, and names the sealed-class envelope as the remedy — this proposal's §6c class-envelope choice is now the normatively sanctioned form, not a sidestep, and the blocked record-envelope convenience form is dead by rule. The dependency is discharged with no residue; the §6c paragraph reads against v0.177 and is superseded by this note.

With these, this proposal carries no open design question; the remaining path is the ordinary acceptance/graduation workflow.

## Declined alternatives (summary)

| Alternative | Reason declined |
|---|---|
| Single `JsonValue` class + `JsonKind` field + union payload fields | Makes illegal states representable; loses the type→payload binding the pattern gives; re-introduces the `dynamic` smell D-0002 forbids (Q1) |
| Record leaves (`JsonString(var s)` positional destructuring) | Records cannot derive from a class (§9.6) nor own the tree (§28.5); *positional* destructuring gives classes no pattern surface (§19.3/§19.6) — the brief's sketch does not compile; leaves must be classes matched by type-test, or (v0.301, not adopted here) by a named destructure (Q1) |
| `double`-backed numbers | Loses `int64`/decimal precision — the classic DOM mistake (Q3) |
| `decimal`-only numbers | Cannot hold the full `double` range or large exponents (Q3) |
| Missing key → a returned `JsonNull` (null-object) | Erases the JSON `{"a":null}` vs `{}` distinction; `Get` returns `null` for absent instead (Q4c) |
| Missing key → `null` from the *indexer* | The strict `[]` is the "assert present" verb; its violation is a defined fault (`KeyNotFoundException`), and `Get` covers the optional path (Q4) |
| `Json.Parse` static class | A whole type for one method; `JsonValue.Parse` mirrors `Guid.Parse`/`int.Parse` and keeps `TryParse`/`ToJsonString` on one type (Q6a) |
| Reuse `JsonOptions` (§2.15) for parse | Different module, serializer-only concerns; two named-default knobs need no type (Q6a) |
| Owned indexer-set (`obj["k"] := v`) for building | Would hide the base get-only navigation indexer; member-hiding across the sealed hierarchy is a grammar subtlety; `Set`/`Add` cover it (Q5) |
| Depth overflow → `StackOverflowException` | Uncatchable and fatal (§31.4); a bounded parser raises the defined `JsonDepthException`, mirroring §2.15's `MaxDepth` (Q7) |
| Add a value-kind peek token to `IDeserializer` now | Breaks the frozen vocabulary (proposal 0010) for a v1-deferrable feature (Open Q1) |

*Amendment (2026-07-30, riding proposal 0073's graduation, docs v0.232 — owner-ruled 2026-07-29).* The `Json.Parse` static-class decline above (Q6a) is scoped, not reversed: it declined **a whole type for one method** and continues to govern this proposal's own API shape — `JsonValue.Parse` remains the DOM's parse entry, unmoved. It does not reach the multi-method, two-direction, typed facade that 0073 landed as the `Json`/`Binary` statics in `System.Serialization` (stdlib §2.15, v0.232): that surface has no single natural value-type home, and the two homes live in different modules under distinct names (`Json` the facade static class; `JsonValue` this proposal's DOM type). Register amendment only; the decision text above is unchanged.
