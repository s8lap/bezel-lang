# Proposal 0010: Serialization eligibility for value builtins + `WriteBytes` token

| | |
|---|---|
| **Status** | draft |
| **Tracking issue** | [#1435](https://github.com/s8lap/bezel-lang/issues/1435) |
| **Target module(s)** | stdlib §2.5 (`DateTime`/`TimeSpan`/`DateOnly`/`TimeOnly`), §2.6 (`Guid`), §2.14 (`ISerializer`/`IDeserializer` vocabulary) |
| **Written against** | spec v0.177, stdlib v0.177 |
| **Wave** | D1 (gap-review items #4 and #17) |
| **Depends on** | Proposal 0009 (Base64 + hex encodings — prerequisite for `WriteBytes` text-wire encoding) |

## Problem

Five compiler-provided value records — `Guid`, `DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly` — declare no `ISerializable`/`IDeserializable` conformance. The §9.8 serialization-eligibility rule admits as components: "a primitive, `string`, an enum, another serialization-eligible record, ..." — none of these five appear. Any record that carries a timestamp, duration, or identifier therefore fails synthesis eligibility. That covers essentially every real domain model.

Gap-review item #17 (`ISerializer.WriteBytes`) is coupled here because its text-wire encoding requires the Base64 API (proposal 0009, a D1 prerequisite) and its design question mirrors #4's mechanism choice.

## Existing ISerializer / IDeserializer token vocabulary (§2.14, v0.177)

Enumerated before extending, per the enumerate-then-extend discipline.

**ISerializer write tokens:**
`WriteInt(long)` · `WriteUInt(ulong)` · `WriteFloat(double)` · `WriteDecimal(decimal)` · `WriteBool(bool)` · `WriteString(string)` · `WriteChar(char)` · `WriteNull()` · `BeginObject(int)` · `WriteFieldName(string)` · `EndObject()` · `BeginArray(int)` · `EndArray()`

**IDeserializer read tokens:**
`ReadInt()` · `ReadUInt()` · `ReadFloat()` · `ReadDecimal()` · `ReadBool()` · `ReadString()` · `ReadChar()` · `TryReadNull()` · `BeginObject()` · `ReadFieldName()` · `SkipValue()` · `EndObject()` · `BeginArray()` · `HasMoreElements()` · `EndArray()`

---

## Design question 1 — mechanism

### Option A: compiler-provided conformances (RECOMMENDED)

The §2.19 precedent (v0.176, #807) established that the compiler may attach conformances to its built-in types without user-written code: "`[t]he primitive numeric tower and `char` conform [to `INumber<T>`/`ISteppable<T>`/`IParsable<T>`] via compiler-provided conformances." The same mechanism is available here. Each of the five records acquires `ISerializable`/`IDeserializable` through a compiler-generated conformance backed by `WriteString`/`ReadString` with a pinned format constant. No change to `ISerializer` or `IDeserializer`.

**Acceptance test (§9.8 eligibility).** A record with `Guid` and `DateTime` fields becomes synthesis-eligible without any user-written `Serialize`:

```bezel
public record EventRecord(
    Id: Guid,
    OccurredAt: DateTime,
    Name: string
): ISerializable, IDeserializable;
// synthesis-eligible: Guid, DateTime, and string are each eligible components (§9.8)
```

### Option B: new vocabulary tokens `WriteGuid`, `WriteDateTime`, ... (DECLINED)

Adding five write-tokens and five read-tokens to the vocabulary interfaces would let each concrete format choose its own binary encoding — `BinarySerializer.WriteGuid` could write 16 raw bytes instead of a 36-character string.

**Declined.** §2.14 states: "[a] type that serializes itself against `ISerializer` is automatically serializable to every format, present and future, without change" — this property depends on a narrow, stable interface. Adding tokens is a **breaking change** to all `ISerializer`/`IDeserializer` implementations. The binary compactness rationale is noted explicitly in the gap-review brief for item #4: "binary compactness deferred — the binary format is already version-local." A format-internal fast path (the concrete `BinarySerializer` detects it is receiving a string that is a Guid/DateTime encoding and writes a compact representation) requires no interface change. That optimization is out of scope for v1 and does not require new tokens.

### Decision

Option A. No interface change for items #4. `WriteBytes` (item #17) is a new token but is justified separately (see below).

---

## Design question 2 — wire formats

All five conformances route through `WriteString`/`ReadString`. The format is pinned per type and identical on all concrete serializers. JSON encodes the string as a quoted JSON string; the binary format encodes it as a length-prefixed byte sequence per its internal encoding; neither sees a new token.

### `Guid` (§2.6)

`Guid.ToString()` is documented as "canonical form" (§2.6); §2.18 specifies: "no-format = `D`" — the hyphenated lowercase form `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`, 36 characters. Round-trip via `Guid.Parse(s)` (§2.6).

- **Text and binary v1:** `D` format. Example: `"550e8400-e29b-41d4-a716-446655440000"`.
- **Serialization sketch (compiler-generated):**
  ```bezel
  // Serialize:
  s.WriteString(this.ToString());          // ToString() = "D" format (§2.6, §2.18)
  // Deserialize factory:
  return Guid.Parse(d.ReadString());       // FormatException propagated as SerializationException
  ```

### `DateTime` (§2.5)

§2.5 declares `DateTime` as a civil-time record with a `Kind: DateTimeKind` annotation (`Unspecified`, `Utc`, `Local`) — "CompareTo = tick order, Kind not normalized, matching .NET." It is not a fixed-point instant. `DateTime.ToString()` produces `yyyy-MM-ddTHH:mm:ss` (§2.5, §2.18 "no-format `ToString()` is ISO 8601") — this drops `Millisecond`.

**Wire format decision:** use the fixed pattern `"yyyy-MM-ddTHH:mm:ss.fff"` via `ToString("yyyy-MM-ddTHH:mm:ss.fff")` (§2.18 vocabulary includes `fff`). This preserves the finest constructor-accepted granularity (`millisecond: int`, §2.5). `Kind` is **not encoded** in v1 — it is lost on round-trip; the deserialized value carries `Kind.Unspecified`. Time-zone-aware round-trip requires `DateTimeOffset` (gap-review item #10, deferred).

- **Text and binary v1:** `yyyy-MM-ddTHH:mm:ss.fff`, no offset indicator. Example: `"2024-03-15T14:30:00.000"`.
- **v1 limitation:** `Kind` is not preserved. Applications that must round-trip UTC instants should use `DateTimeOffset` (pending) or hand-write `Serialize`/`Deserialize` (which suppresses synthesis — §9.8).
- **Serialization sketch:**
  ```bezel
  s.WriteString(this.ToString("yyyy-MM-ddTHH:mm:ss.fff"));
  // Deserialize: DateTime.Parse produces Kind.Unspecified
  return DateTime.Parse(d.ReadString());
  ```

### `TimeSpan` (§2.5) — with prerequisite §2.5 addition

§2.18 specifies `TimeSpan`'s format specifier as `c` (constant: `[-][d.]HH:mm:ss[.fffffff]`, "no-format = `c`"). The `c` format encodes days, hours, minutes, seconds, and fractional seconds to 7 decimal places (100 ns tick resolution) — round-trip exact and human-readable.

**Prerequisite addition to §2.5:** `TimeSpan` currently lacks `ToString()`, `ToString(format: string)`, `Parse`, and `TryParse` in the stdlib surface, though §2.18 specifies its format behavior. These are a gap parallel to `DateTime`'s surface and are required for the conformance's `Deserialize`. This proposal adds them as part of the §2.5 delta (see below).

- **Text and binary v1:** `c` format. Example: `"1.02:03:04.5678900"` (1 day 2h 3m 4.567890s).
- **Tick-count alternative (declined):** routing through `WriteInt(this.Ticks)` avoids needing `TimeSpan.Parse`, but changes the token type from string to int, encodes no semantic tag distinguishing this integer from a count of anything else, and produces a non-human-readable JSON number. The `c` format via `WriteString` is unambiguously self-describing.
- **Serialization sketch:**
  ```bezel
  s.WriteString(this.ToString());          // c format by default (§2.18)
  return TimeSpan.Parse(d.ReadString());   // parses c-format; FormatException → SerializationException
  ```

### `DateOnly` (§2.5)

`DateOnly.ToString()` = `"yyyy-MM-dd"` (§2.5). The constructor takes `(year, month, day)` with no sub-day components — round-trip exact.

- **Text and binary v1:** `"yyyy-MM-dd"`. Example: `"2024-03-15"`.
- **Serialization sketch:**
  ```bezel
  s.WriteString(this.ToString());
  return DateOnly.Parse(d.ReadString());
  ```

### `TimeOnly` (§2.5)

`TimeOnly.ToString()` = `"HH:mm:ss"` (§2.5). The constructor accepts `millisecond: int` (§2.5) — the no-arg form drops it. Use `"HH:mm:ss.fff"` (§2.18 includes `fff` for `TimeOnly`) for round-trip fidelity to millisecond precision.

- **Text and binary v1:** `"HH:mm:ss.fff"`. Example: `"14:30:00.500"`.
- **Serialization sketch:**
  ```bezel
  s.WriteString(this.ToString("HH:mm:ss.fff"));
  return TimeOnly.Parse(d.ReadString());
  ```

---

## Design question 3 — `WriteBytes` token (gap-review item #17)

### Why a new token is warranted here

The earlier decline of `WriteGuid` etc. rested on these types having `WriteString` as an adequate routing path. An opaque byte sequence has no adequate existing path: `WriteString` carries text (a `string` value; in JSON, a quoted UTF-8 string). Encoding raw bytes as a string token requires a disambiguation convention so the read side knows to decode rather than return the string as-is. That convention IS the new token — exactly the same reason `WriteNull` is not `WriteString("")`. The token carries a semantic distinct from any existing token; adding it is not gratuitous interface expansion.

### Proposed additions to §2.14

```bezel
// PROPOSED additions to ISerializer:
method WriteBytes(value: ReadOnlySpan<byte>);

// PROPOSED additions to IDeserializer:
method ReadBytes(): owned Array<byte>;
method TryReadBytes(out result: owned Array<byte>?): bool;  // v0.171 Try-convention; null on false
```

**Text wire (JSON):** `WriteBytes` encodes the span as RFC 4648 standard-alphabet base64 (padded). The concrete `JsonSerializer` calls `Base64.Encode` (proposal 0009) internally. `ReadBytes` / `TryReadBytes` call `Base64.Decode`; a malformed base64 value raises `SerializationException` (consistent with §2.15: structural input faults always raise `SerializationException`). `TryReadBytes` follows the v0.171 Try-convention — returns `bool`, assigns `out result` to `null` on `false`, does not throw.

**Binary wire:** `WriteBytes` writes a 4-byte little-endian length prefix followed by the raw bytes. `ReadBytes` reads the prefix, allocates `owned Array<byte>`, fills, and returns it.

**Compatibility.** Adding these three methods breaks any existing `ISerializer`/`IDeserializer` implementation that does not supply them. There are no users yet; the known set of implementations is the stdlib's own `JsonSerializer`/`BinarySerializer`/`JsonDeserializer`/`BinaryDeserializer`, all of which are extended by this proposal. The correct long-term mitigation is to declare the vocabulary interfaces stable once complete; that is a governance question outside this proposal's scope.

---

## Proposed stdlib-section deltas (ready-to-land)

### §2.5 — `TimeSpan` surface addition (prerequisite)

```bezel
public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>, ISerializable, IDeserializable
  // ... existing members unchanged (FromTicks, FromDays, ..., operator +/-/*) ...
  public method ToString(): string;                    // c format: [-][d.]HH:mm:ss[.fffffff] (§2.18)
  public method ToString(format: string): string;      // only "c" recognized; FormatException otherwise
  public static method Parse(s: string): TimeSpan;    // parses c-format; FormatException on invalid
  public static method TryParse(s: string, out result: TimeSpan): bool;
  // Wire form (compiler-provided conformance): WriteString(c-format) / ReadString + Parse
  // Round-trip exact: c-format encodes to 100ns tick precision (§2.18)
end TimeSpan;
```

### §2.5 — `DateTime`, `DateOnly`, `TimeOnly` conformance addition

```bezel
public record DateTime: IComparable<DateTime>, IEquatable<DateTime>, ISerializable, IDeserializable
  // ... existing members unchanged ...
  // Wire form (compiler-provided): WriteString("yyyy-MM-ddTHH:mm:ss.fff") / ReadString + Parse
  // v1 limitation: Kind not preserved; deserialized as Kind.Unspecified

public record DateOnly: IComparable<DateOnly>, IEquatable<DateOnly>, ISerializable, IDeserializable
  // ... existing members unchanged ...
  // Wire form (compiler-provided): WriteString("yyyy-MM-dd") / ReadString + Parse
  // Round-trip exact

public record TimeOnly: IComparable<TimeOnly>, IEquatable<TimeOnly>, ISerializable, IDeserializable
  // ... existing members unchanged ...
  // Wire form (compiler-provided): WriteString("HH:mm:ss.fff") / ReadString + Parse
  // Round-trip exact to millisecond precision
```

### §2.6 — `Guid` conformance addition

```bezel
public record Guid: IComparable<Guid>, IEquatable<Guid>, ISerializable, IDeserializable
  // ... existing members unchanged ...
  // Wire form (compiler-provided): WriteString(ToString()) = "D" format / ReadString + Parse
  // Text and binary v1: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", 36 chars, lowercase (§2.6, §2.18)
```

### §2.14 — `ISerializer` / `IDeserializer` `WriteBytes` extension

```bezel
public interface ISerializer
  // ... existing 13 tokens unchanged ...
  method WriteBytes(value: ReadOnlySpan<byte>);        // PROPOSED: opaque byte payload
end ISerializer;

public interface IDeserializer
  // ... existing 15 tokens unchanged ...
  method ReadBytes(): owned Array<byte>;                // PROPOSED; SerializationException on malformed input
  method TryReadBytes(out result: owned Array<byte>?): bool;  // PROPOSED; v0.171 Try-convention; null on false
end IDeserializer;
```

---

## Open questions (owner ruling required before landing)

1. **`DateTime.Kind` loss in v1** — acceptable as a stated v1 limitation, or should this proposal be sequenced after gap-review item #10 (`DateTimeOffset`) so callers always have a round-trip path for UTC instants?
2. **`WriteBytes` interface breakage** — the breakage at this stage (no external users) is manageable; is freezing the vocabulary interfaces once complete the intended stability posture?

---

## Declined alternatives (summary)

| Alternative | Reason declined |
|---|---|
| New tokens `WriteGuid` / `WriteDateTime` / ... per type | Breaking change to `ISerializer`; binary compactness is version-local (gap-review #4 brief); format-internal fast-path optimization does not require new tokens |
| `TimeSpan` as raw tick count via `WriteInt` | Changes token type from string to int; no semantic tag in the stream; JSON output is a bare number with no self-describing shape |
| `DateTime` UTC offset (`yyyy-MM-ddTHH:mm:ssZ` for `Kind.Utc`) | `DateTime` is a civil-time record, not an instant (§2.5 "Kind not normalized"); partial UTC annotation is inconsistent for `Kind.Local` and `Kind.Unspecified`; deferred to `DateTimeOffset` (gap #10) |
| `TimeOnly` using no-arg `ToString()` (`HH:mm:ss`) | Drops `Millisecond`; two `TimeOnly` values differing only in milliseconds serialize identically — not round-trip exact |
