# Proposal 0012 — `DateTimeOffset` + `TimeZoneInfo`

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Tracking issue** | [#1445](https://github.com/s8lap/bezel-lang/issues/1445) |
| **Target module(s)** | `System` (§2.5 cluster, auto-imported) — new `DateTimeOffset` and `TimeZoneInfo` |
| **Written against** | spec v0.177, stdlib v0.177 |
| **Wave** | D2 (gap-review item #10) |
| **Depends on** | Proposal 0010 (serialization value builtins — DateTimeOffset owns the offset-bearing wire form 0010's `DateTime` delegates to); the compiler-provided-conformance mechanism of §2.19 / §9.8 |
| **Currency note** | Refreshed 2026-09-17 to v0.306: the two verbatim quotations of spec §25.2 re-derived from the landed text ("Records, enums, and choices may overload `=` and `<>`" — choices joined the sentence with §35.2). Re-verified otherwise: stdlib §2.5/§2.6/§2.13/§2.18/§2.19 and spec §9.8/§25.2/§31.4 all still resolve, and D2's `DateTime.Date`/`DateTime.TimeOfDay` precedent — prose-only at this proposal's pin — is now declared (v0.274). Two tier-2 flags recorded in `proposals-currency-2026-09-17.md` §3 (D3's `<`/`>`/`<=`/`>=` group and its deleted `DateTime` precedent; D3 vs §25.2's homogeneous-overload / co-declaration rules). Written-against pin unchanged. |

## Rulings recorded at acceptance

The three owner questions below are resolved as follows (delegated ruling, 2026-07-07; owner review pending). See "Open questions" for the full framing of each.

1. **Q1 — instant-based equality on a value record: ACCEPTED as proposed.** `DateTimeOffset` defines `Equals`/hash/`CompareTo` over the *instant* and provides `EqualsExact` for the offset-sensitive case. This is not a stretch of the equality model: §25.2 explicitly admits that "Records, enums, and choices may overload `=` and `<>`," so a record diverging from structural equality via an `=`/`<>` overload is mechanically sanctioned. Instant order is the only total order consistent with "happened before," and it matches .NET. (Resolves D3.)
2. **Q2 — `TimeZoneInfo` home / auto-import: auto-imported `System`, as proposed.** Symmetry with `DateTime` — "convert to local time" should be import-free just as `DateTime.Now` is. A UTC-only program never touches tzdata (`TimeZoneInfo.Utc` and `DateTimeOffset.UtcNow` need no database), so auto-import carries no filesystem-side-effect cost for the common case.
3. **Q3 — defer the wall-clock → instant resolver to v2: CONFIRMED.** v1 ships only the total, always-well-defined operations plus the pure detection predicates (`IsAmbiguousTime`/`IsInvalidTime`); the one operation that must choose between two instants (or fault) is not shipped with a hidden default. The v2 resolver takes an explicit `AmbiguousTimeResolution` policy. (Resolves D5/D8.)

## Problem

The stdlib's temporal surface (§2.5) offers `DateTime` — a **civil-time** record carrying a `Kind: DateTimeKind` annotation (`Unspecified`/`Utc`/`Local`) but **no offset**, with "`CompareTo` = tick order, `Kind` not normalized, matching .NET" (§2.5). A `DateTime` therefore cannot represent an *unambiguous instant*: two `DateTime` values with different `Kind`s and the same ticks compare equal yet denote different moments, and a `Kind.Unspecified` value denotes no moment at all until a zone is supplied. There is also **no time-zone type**: nothing converts an instant into wall-clock time in `America/New_York`, and nothing knows what UTC offset is in effect at a given moment.

Two concrete downstream consequences:

1. **Serialization round-trip (proposal 0010).** 0010 pins `DateTime`'s wire form to a *conditional-`Z`* civil string (owner ruling 2026-07-04: bare when `Kind` is `Unspecified`/`Local`, `…Z` when `Kind.Utc`; **offset-bearing strings are rejected for `DateTime`**). That is correct for a civil-time type but leaves *no built-in type that round-trips a specific instant with its offset*. This proposal supplies it: `DateTimeOffset` **owns** the full-offset ISO 8601 wire form (`yyyy-MM-ddTHH:mm:ss.fff+HH:MM`), completing the story 0010 deliberately left open (0010 open question #1).
2. **Everyday correctness.** "What time is it in the user's zone?", "convert this UTC timestamp to `Pacific/Auckland`", "is this local timestamp inside a DST fall-back?" are unanswerable today.

Gap-review item #10 (`TimeZoneInfo` + `DateTimeOffset`, P0+P1, "time" domain) named the single blocking dependency as the **tzdata packaging decision** (§ *Suggested sequencing* → RFC-track: "#10 (tzdata packaging)"). This proposal answers that question (D6).

This proposal keeps v1 **deliberately small**: `DateTimeOffset` is a full value type; `TimeZoneInfo` v1 is **instant-centric and detection-only** — the genuinely hard operation (resolving a bare wall-clock local time to an instant, which is ambiguous during a DST fall-back and nonexistent during a spring-forward) is *detectable* in v1 but its *resolution* is deferred (D5).

---

## Design decisions

### D1 — `DateTimeOffset` shape: instant + fixed offset

**Decision.** `DateTimeOffset` is a **value record** carrying exactly two logical pieces of state: a specific **instant** and a **fixed UTC offset** (a `TimeSpan` in `[-14:00, +14:00]`, whole minutes). Equivalently — and this is the internal invariant — it holds a wall-clock `DateTime` reading plus the offset, with `UtcDateTime = wallClock - Offset`. It represents an unambiguous moment: unlike `DateTime`, there is no `Kind` and no ambiguity about which instant is meant. This is the .NET `DateTimeOffset` shape, which the mission brief calls out as "what .NET got right."

**Home.** `System`, auto-imported, in the §2.5 temporal cluster next to `DateTime`. It is a value builtin that participates in §9.8 serialization eligibility (like `DateTime`), so it must be import-free everywhere `DateTime` is.

**Rationale.** A fixed offset (not a zone) is the right amount of state for an instant-with-context: it is closed under instant arithmetic (D4), has no DST discontinuities of its own, and serializes to a self-describing ISO 8601 string (D7). A zone-bearing type (instant + `TimeZoneInfo`) would reintroduce every DST hazard into the value type itself; keeping zones in `TimeZoneInfo` (D5) and only a frozen numeric offset in `DateTimeOffset` is the separation .NET validated.

### D2 — Relation to `DateTime`: the three projections and the conversion-in rule

**Decision — projections out (`DateTimeOffset → DateTime`).** Three `DateTime`-valued accessors, matching .NET's property surface:

| Accessor | `Kind` | Meaning |
|---|---|---|
| `UtcDateTime` | `Utc` | the instant expressed in UTC |
| `LocalDateTime` | `Local` | the instant expressed in the system local zone (via `TimeZoneInfo.Local`) |
| `DateTime` | `Unspecified` | the raw wall-clock reading (offset stripped) |

These subsume the mission's "`ToUtcDateTime`/`ToLocalDateTime`" as properties, consistent with the existing `DateTime.Date`/`DateTime.TimeOfDay` property-decomposition style (§2.5).

**Decision — conversion in (`DateTime → DateTimeOffset`), the honest cast rule.** `DateTimeOffset.FromDateTime(dt)` is defined by `dt.Kind`:

- `Kind.Utc` → offset `+00:00`; wall reading = `dt`'s components. Well-defined.
- `Kind.Local` → offset = the **system local zone's** offset at that local time (`TimeZoneInfo.Local`). Well-defined *except* when that local time is ambiguous or invalid (a DST boundary), which raises `InvalidTimeZoneException`-family behavior — see D5; a caller wanting control uses the explicit-offset constructor.
- `Kind.Unspecified` → **`ArgumentException`.** An `Unspecified` `DateTime` carries no offset information, and silently assuming local (as .NET's implicit conversion does) produces a wrong instant whenever the value did not come from the local zone. Bezel refuses the guess: the caller must state the offset with the `(dateTime, offset)` constructor, or set `Kind` first.

**Rationale.** The `Unspecified → Local` silent assumption is a well-known .NET footgun (a database timestamp read as `Unspecified` becomes "local" and shifts by the server's offset). The defined-fault philosophy (§2.18 strict-fault model; rationale's decline-with-reasons discipline) says: refuse the ambiguous input loudly rather than pick silently.

### D3 — Comparison and equality by **instant**, with `EqualsExact` for offset-sensitivity

**Decision.** `CompareTo`, the `<`/`>`/`<=`/`>=` operator group, `IEquatable<DateTimeOffset>.Equals`, and the derived hash are all defined over the **instant** (`UtcDateTime`), *not* the wall-clock reading. Two `DateTimeOffset` values that denote the same moment through different offsets — `2024-03-15T12:00:00+00:00` and `2024-03-15T13:00:00+01:00` — are **equal** and compare equal. A separate `EqualsExact(other)` returns `true` only when the instant **and** the offset both match.

This is a **deliberate divergence from structural record equality**, and it is one the language explicitly sanctions: §25.2 states "Records, enums, and choices may overload `=` and `<>`." The default record `Equals` would compare both fields (wall reading *and* offset) and report the two examples above as unequal; the compiler-provided conformance overrides that with an instant-based `=`/`<>` overload (§25.2), and the hash is taken over the instant so the equality/hash contract holds.

**Rationale.** Instant order is the only total order that agrees with "happened before"; ordering by wall reading would put `13:00+01:00` after `12:00+00:00` despite being the same moment. This is the mission's "instant order, not civil," and matches .NET (`DateTimeOffset.Equals`/`CompareTo` are instant-based; `EqualsExact` is the offset-sensitive escape hatch). The divergence from structural equality is flagged as **owner open question Q1** because value records equating on a projection of their fields is unusual in the stdlib.

### D4 — Arithmetic is always well-defined (the honest hard part, part 1)

**Decision.** **All `DateTimeOffset` arithmetic keeps the offset fixed** and consults no time zone, so none of it can land in a DST gap or ambiguity:

- **`TimeSpan`-based** (`Add(TimeSpan)`, `AddDays`/`AddHours`/`AddMinutes`/`AddSeconds`/`AddMilliseconds`, `+`/`-` with a `TimeSpan`) is **instant arithmetic**: it advances the underlying instant; the wall reading is recomputed at the *same numeric offset*. `AddDays(1)` is exactly `+24h` — there is no "spring-forward day is 23 hours" behaviour, because a fixed-offset value has no DST.
- **Calendar** (`AddYears`, `AddMonths`) operates on the wall-clock reading (variable-length units) with the **offset preserved**; the result's instant is whatever `newWallReading @ sameOffset` denotes. Also total (no zone consulted).
- `Subtract(other): TimeSpan` is the **instant difference** (`this.UtcDateTime - other.UtcDateTime`); `Subtract(TimeSpan)` / `operator -` with a `TimeSpan` is instant arithmetic.
- **Offset re-projection** (`ToOffset(offset)`, `ToUniversalTime()`, `ToLocalTime()`) changes the offset while **holding the instant fixed** — a lossless relabelling.

**Rationale.** This is exactly the mission's recommendation: "`DateTimeOffset` arithmetic is INSTANT arithmetic, always well-defined." The value type never makes a DST decision, so its arithmetic never surprises. "Add one *civil* day in a *zone*, honoring the spring-forward" is a different operation and belongs to `TimeZoneInfo` — which in v1 provides the pieces but not the composite (D5).

### D5 — `TimeZoneInfo` v1: instant-centric, detection-only (the honest hard part, part 2)

**Decision.** `TimeZoneInfo` v1 ships every operation whose input is an **instant** (total, always well-defined) plus **pure DST-detection predicates** over a wall-clock reading. It does **not** ship the wall-clock → instant *resolution* (which must make an ambiguity/invalidity decision); that is deferred to v2 (with the detection predicates already in hand so a caller can hand-roll it explicitly).

Shipped in v1:

- **Lookup:** `TimeZoneInfo.Utc` (constant, needs no database), `TimeZoneInfo.Local` (system local zone; D6), `FindSystemTimeZoneById(id)` / `TryFindSystemTimeZoneById(id, out)`, **IANA id only** (D6).
- **Instant → zone (total):** `tz.GetUtcOffset(instant: DateTimeOffset): TimeSpan`; `tz.IsDaylightSavingTime(instant: DateTimeOffset): bool`; `TimeZoneInfo.ConvertTime(instant: DateTimeOffset, destination: TimeZoneInfo): DateTimeOffset` — same instant, re-projected to the destination zone's offset at that instant (`= instant.ToOffset(destination.GetUtcOffset(instant))`). An instant maps to exactly one offset in any zone (the UTC→local direction is *total*), so all three are unconditional.
- **Wall-clock DST detection (pure, non-throwing):** `tz.IsAmbiguousTime(wallClock: DateTime): bool` (a fall-back window, where the reading occurs twice), `tz.IsInvalidTime(wallClock: DateTime): bool` (a spring-forward gap, where the reading never occurs).

Deferred to v2: constructing a `DateTimeOffset` from a bare local `DateTime` + a zone (the ambiguous/invalid-prone direction), which requires an explicit ambiguity policy (`AmbiguousTimeResolution { Earlier, Later }` and a decision on the invalid case). Recorded, not shipped half-made.

**Rationale.** This is the mission's "smaller honest v1": every *shipped* operation is total and never silently wrong. The one operation that *must* choose between two instants (ambiguous fall-back) or between fault/adjust (invalid spring-forward) is not shipped with a hidden default. See D8 for why the *detection predicates* are the chosen ambiguity surface over a lossy `TryConvert`.

**Type shape.** `TimeZoneInfo` is a `sealed class` (like `Type`, §2.13): an **immutable, process-interned** reference. `FindSystemTimeZoneById` / `Local` / `Utc` return a **borrowed** reference into a process-lived cache; instances are never `owned` and never disposed — there is no ownership or `IDisposable` question (the `Type` precedent, §2.13, "exactly one instance per distinct type," borrowed for the process lifetime).

> **Owner direction (2026-07-29, recorded for the graduation pass).** The lookup re-shapes to **registry access** — a static-property/indexer-shaped surface in the §22.13/`Encoding.UTF8` admitted family — rather than a new static-*method* borrow admission, which the v0.201/v0.223 factory-announcement line declined to widen; the final spelling is settled at this proposal's graduation critique. In the same pass, `TryFindSystemTimeZoneById` re-scopes to the absence axis only — a corrupt or unreadable database propagates as `InvalidTimeZoneException` rather than folding into the `false` return — per the graduation-checklist tracker [#2338](https://github.com/s8lap/bezel-lang/issues/2338).

### D6 — tzdata packaging (the question the gap review promised) + IANA-only lookup

**The question (recorded).** Should Bezel **bundle** its own copy of the IANA time-zone database inside the compiler/runtime, or **read the host system's** database (e.g. `/usr/share/zoneinfo`)?

**Answer for v1 — read the system database; no bundled tzdata.**

- v1 reads the platform tz database via the OS: on Linux/macOS the zoneinfo tree (`/usr/share/zoneinfo`) and the local-zone selectors (`TZ` env var, `/etc/localtime`). This matches the build toolchain's Linux-first posture (CLAUDE.md: clang/LLVM on Fedora).
- **Rationale.** The IANA database updates several times a year (DST-rule and offset changes are political and frequent). Bundling means the runtime ships a snapshot that goes stale between releases and must be re-cut on every IANA update, duplicating data every OS already keeps current through its own security/update channel. Reading the system DB inherits those updates for free.
- **Bundling is deferred behind an evidence bar.** Bundle a pinned tz database only when a **named consumer** demonstrates a need the host DB cannot meet — concretely: (a) reproducible builds that must pin a tz-database *version* for byte-identical output across machines, or (b) a target platform with **no** system tz database. Absent such evidence, bundling is not built (adding a several-hundred-KB versioned data file and its refresh machinery is real debt; CLAUDE.md "dependencies are debt").
- **Systems without a usable tz database → a defined fault.** When the system database is absent or unreadable, `FindSystemTimeZoneById`, `TryFindSystemTimeZoneById` (returns `false`), and `TimeZoneInfo.Local` raise/report `InvalidTimeZoneException` ("time-zone database unavailable"). `TimeZoneInfo.Utc` **always** works (fixed `+00:00`, needs no database) and `DateTimeOffset.UtcNow` needs no zone at all — so a UTC-only program never touches tzdata and never faults on its absence.

**IANA-id-only lookup (Windows-id mapping declined for v1).** `FindSystemTimeZoneById` accepts IANA ids only (`"America/New_York"`, `"Pacific/Auckland"`, `"UTC"`). Windows registry ids (`"Eastern Standard Time"`) are **not** accepted.

- **Declined with reasons.** The IANA↔Windows mapping is the CLDR `windowsZones.xml` table — a large, separately-versioned data file that would itself need packaging and staleness management, i.e. the exact debt v1 avoids by reading the system DB. Bezel's build target is Linux-first, where IANA ids are native. Revisit when a Windows port lands *with* the mapping-table packaging decision made deliberately, not smuggled in through this proposal.

### D7 — Formatting/parsing: the `zzz` offset specifier + `IParsable`

**Decision — display.** `DateTimeOffset.ToString()` (no format) renders `yyyy-MM-ddTHH:mm:sszzz` — ISO 8601 with a trailing signed offset, e.g. `2024-03-15T14:30:00+13:00`. `ToString(format)` accepts the §2.18 date-pattern vocabulary **extended with `zzz`** = the signed UTC offset `±HH:mm`.

**Decision — §2.18 vocabulary extension.** Add one specifier and one row to the §2.18 table:

- New specifier `zzz` — "signed UTC offset `±HH:mm`" — available in `DateTimeOffset` patterns.
- New table row: `DateTimeOffset` | pattern strings of `yyyy MM dd HH mm ss fff zzz` plus literal separators | no-format `ToString()` = `yyyy-MM-ddTHH:mm:sszzz`.

(The existing `DateTime`/`DateOnly`/`TimeOnly` row is unchanged; `zzz` is meaningful only where an offset exists, so it is scoped to `DateTimeOffset`.)

**Decision — parse.** `DateTimeOffset.Parse(s)` / `TryParse(s, out result: DateTimeOffset?)` conform to `IParsable<DateTimeOffset>` (§2.19): `Parse` raises `FormatException` on bad input; `TryParse` follows the v0.171 nullable-out shape (§2.19). Accepted input is offset-bearing ISO 8601: a trailing `±HH:MM` offset, **or** a `Z` designator (parsed as offset `+00:00`). A string **without** an offset is a `FormatException` — the offset is the whole point of the type. This is the exact complement of proposal 0010's owner ruling that `DateTime.Parse` **rejects** offset-bearing strings: civil strings belong to `DateTime`, offset strings belong to `DateTimeOffset`. The two parsers partition the ISO 8601 space with no overlap.

### D8 — Ambiguity surface: predicates, not a lossy `TryConvert`

**Decision.** The v1 ambiguity surface is the pair of pure predicates `IsAmbiguousTime` / `IsInvalidTime` (plus `IsDaylightSavingTime` over an instant), **not** a `TryConvert(wallClock, tz, out DateTimeOffset?)`.

**Rationale.** A `TryConvert` that returns a single `DateTimeOffset` for an *ambiguous* wall-clock reading has *already chosen* one of the two valid instants and hidden the choice behind a `bool` — `true` with no indication of *which* moment was picked. That is less honest than surfacing the ambiguity: the predicates let a caller detect the fall-back/gap and decide explicitly, and they bake in no policy. When v2 adds the resolving operation it will take an explicit `AmbiguousTimeResolution` argument rather than a silent default (D5). This is the smaller and the more honest of the two shapes the mission posed.

---

## Proposed stdlib additions (ready-to-land text)

Section numbers assume insertion into the §2.5 temporal cluster; the docs editor adjusts numbering at integration time. All members below are **new** (no existing `DateTime`/`TimeSpan`/etc. member is changed by this proposal; the §2.18 table gains one row and one specifier per D7).

### Proposed §2.5 addition — `DateTimeOffset`

```bezel
public record DateTimeOffset:
    IComparable<DateTimeOffset>, IEquatable<DateTimeOffset>,
    ISerializable, IDeserializable
  // ---- Construction ----
  // dateTime.Kind must be Unspecified; offset in [-14:00, +14:00] and whole
  // minutes. ArgumentException / ArgumentOutOfRangeException otherwise.
  public constructor(dateTime: DateTime, offset: TimeSpan);
  public constructor(year: int, month: int, day: int, hour: int, minute: int,
                     second: int, offset: TimeSpan);
  public constructor(year: int, month: int, day: int, hour: int, minute: int,
                     second: int, millisecond: int, offset: TimeSpan);

  // Kind.Utc -> +00:00; Kind.Local -> system local offset at that time
  //   (InvalidTimeZoneException on a DST-ambiguous/invalid local time, D5);
  //   Kind.Unspecified -> ArgumentException (no offset to infer, D2).
  public static method FromDateTime(dt: DateTime): DateTimeOffset;
  public static method FromUnixTimeSeconds(seconds: long): DateTimeOffset;       // offset +00:00
  public static method FromUnixTimeMilliseconds(ms: long): DateTimeOffset;       // offset +00:00

  // ---- Instant + offset ----
  public readonly property Offset: TimeSpan;         // fixed UTC offset
  public readonly property UtcDateTime: DateTime;    // Kind = Utc   (D2)
  public readonly property LocalDateTime: DateTime;  // Kind = Local (D2)
  public readonly property DateTime: DateTime;       // Kind = Unspecified: wall reading (D2)
  public readonly property UtcTicks: long;           // ticks of the UTC instant
  public method ToUnixTimeSeconds(): long;
  public method ToUnixTimeMilliseconds(): long;

  // ---- Civil components of the wall-clock reading ----
  public readonly property Year: int;
  public readonly property Month: int;         // 1..12
  public readonly property Day: int;           // 1..31
  public readonly property Hour: int;          // 0..23
  public readonly property Minute: int;        // 0..59
  public readonly property Second: int;        // 0..59
  public readonly property Millisecond: int;
  public readonly property DayOfWeek: DayOfWeek;
  public readonly property DayOfYear: int;
  public readonly property Date: DateOnly;
  public readonly property TimeOfDay: TimeOnly;

  // ---- Current instant ----
  public static readonly property UtcNow: DateTimeOffset;  // current instant, offset +00:00
  public static readonly property Now: DateTimeOffset;     // current instant, system local offset
  public static readonly property MinValue: DateTimeOffset;
  public static readonly property MaxValue: DateTimeOffset;

  // ---- Arithmetic (offset fixed; total, never zone-consulting -- D4) ----
  public method Add(span: TimeSpan): DateTimeOffset;   // instant arithmetic
  public method AddDays(days: double): DateTimeOffset;
  public method AddHours(hours: double): DateTimeOffset;
  public method AddMinutes(minutes: double): DateTimeOffset;
  public method AddSeconds(seconds: double): DateTimeOffset;
  public method AddMilliseconds(ms: double): DateTimeOffset;
  public method AddYears(years: int): DateTimeOffset;   // calendar add on wall reading, offset kept
  public method AddMonths(months: int): DateTimeOffset;
  public method Subtract(other: DateTimeOffset): TimeSpan;  // instant difference
  public method Subtract(span: TimeSpan): DateTimeOffset;

  // ---- Offset re-projection (same instant, different offset) ----
  public method ToOffset(offset: TimeSpan): DateTimeOffset;
  public method ToUniversalTime(): DateTimeOffset;   // offset +00:00
  public method ToLocalTime(): DateTimeOffset;       // system local offset (TimeZoneInfo.Local)

  // ---- Comparison / equality by INSTANT (D3) ----
  public method CompareTo(other: DateTimeOffset): int;    // UtcDateTime order
  public method Equals(other: DateTimeOffset): bool;      // same instant (offset-agnostic)
  public method EqualsExact(other: DateTimeOffset): bool; // same instant AND same offset

  // ---- Formatting / parsing (D7) ----
  public method ToString(): string;                   // "yyyy-MM-ddTHH:mm:sszzz"
  public method ToString(format: string): string;     // §2.18 vocabulary + `zzz`
  public static method Parse(s: string): DateTimeOffset;                        // FormatException; IParsable
  public static method TryParse(s: string, out result: DateTimeOffset?): bool;  // v0.171 nullable-out shape

  // Operator group agrees with CompareTo (instant order), matching the §2.5
  // v0.165 pattern for DateTime.
  public static method operator -(a: DateTimeOffset, b: DateTimeOffset): TimeSpan;
  public static method operator -(d: DateTimeOffset, t: TimeSpan): DateTimeOffset;
  public static method operator +(d: DateTimeOffset, t: TimeSpan): DateTimeOffset;
  public static method operator +(t: TimeSpan, d: DateTimeOffset): DateTimeOffset;

  // Wire form (compiler-provided conformance, §2.19 precedent -- see Serialization below):
  //   WriteString("yyyy-MM-ddTHH:mm:ss.fffzzz") / ReadString + Parse
end DateTimeOffset;
```

### Proposed §2.5 addition — `TimeZoneInfo` (+ its exceptions)

```bezel
public sealed class TimeZoneInfo    // immutable, process-interned (the §2.13 `Type` precedent);
                                    // returned as a borrow, never owned, never disposed
  public readonly property Id: string;               // IANA id, e.g. "America/New_York"

  public static readonly property Utc: TimeZoneInfo;   // fixed +00:00; needs no tz database
  public static readonly property Local: TimeZoneInfo; // system local zone; resolved once at first
                                                       //   access (TZ / /etc/localtime), D6.
                                                       //   InvalidTimeZoneException if no system DB.

  // Lookup by IANA id only (D6). FindSystemTimeZoneById raises
  //   TimeZoneNotFoundException (unknown id) or InvalidTimeZoneException
  //   (database unreadable). Try-form returns false + null on either.
  public static method FindSystemTimeZoneById(id: string): TimeZoneInfo;
  public static method TryFindSystemTimeZoneById(id: string, out result: TimeZoneInfo?): bool;

  // ---- Instant -> zone (total, always well-defined; D5) ----
  public method GetUtcOffset(instant: DateTimeOffset): TimeSpan;
  public method IsDaylightSavingTime(instant: DateTimeOffset): bool;

  // Same instant, re-projected to `destination`'s offset at that instant.
  //   = instant.ToOffset(destination.GetUtcOffset(instant)).
  public static method ConvertTime(instant: DateTimeOffset,
                                   destination: TimeZoneInfo): DateTimeOffset;

  // ---- Wall-clock DST detection (pure, non-throwing; D5/D8) ----
  // wallClock is read as a local reading in THIS zone (Kind Unspecified/Local).
  public method IsAmbiguousTime(wallClock: DateTime): bool;  // fall-back: reading occurs twice
  public method IsInvalidTime(wallClock: DateTime): bool;    // spring-forward: reading never occurs
end TimeZoneInfo;

// Declared alongside their producer (auto-imported with TimeZoneInfo, since
// TimeZoneInfo is auto-imported; §31.4 "exception lives with its producer").
public class TimeZoneNotFoundException: Exception
  public constructor(message: string);
  public readonly property TimeZoneId: string?;   // the id that was not found; null when N/A
end TimeZoneNotFoundException;

public class InvalidTimeZoneException: Exception     // corrupt/unavailable tz database (D6)
  public constructor(message: string);
end InvalidTimeZoneException;
```

### Proposed §2.18 addition — the `zzz` offset specifier (D7)

Add one specifier and one table row (the existing rows are unchanged):

- **New specifier** — `zzz`: the signed UTC offset in `±HH:mm` form (e.g. `+13:00`, `-05:00`). Meaningful only on a type that carries an offset.

| Kind | Specifiers | Notes |
|---|---|---|
| `DateTimeOffset` | pattern strings of `yyyy` `MM` `dd` `HH` `mm` `ss` `fff` `zzz` plus literal separators | no-format `ToString()` = `yyyy-MM-ddTHH:mm:sszzz` |

---

## Serialization (slots into proposal 0010's mechanism)

`DateTimeOffset`'s `ISerializable`/`IDeserializable` conformance is **compiler-provided**, via the exact mechanism proposal 0010 established for the value builtins (§2.19 precedent, §9.8 eligibility) — cited, not restated. No new `ISerializer`/`IDeserializer` token is required: the conformance routes through the existing `WriteString`/`ReadString` tokens.

- **Wire form (pinned):** `yyyy-MM-ddTHH:mm:ss.fffzzz` — full-offset ISO 8601, e.g. `"2024-03-15T14:30:00.000+13:00"`. This is the offset-bearing form 0010's `DateTime` conformance **rejects** (owner ruling 2026-07-04) and delegates here; `DateTimeOffset` owns it. Millisecond precision (`.fff`) is preserved, matching 0010's treatment of `DateTime` (default `ToString()` drops millis, the *wire* form keeps them via an explicit pattern).
- **Serialization sketch (compiler-generated, same shape as 0010):**
  ```bezel
  // Serialize:
  s.WriteString(this.ToString("yyyy-MM-ddTHH:mm:ss.fffzzz"));
  // Deserialize factory:
  return DateTimeOffset.Parse(d.ReadString());   // FormatException -> SerializationException
  ```
- **Round-trip:** exact to the millisecond, offset preserved. Because equality is instant-based (D3), a value deserialized from `…+13:00` `Equals` one serialized from the same instant as `…+00:00`; `EqualsExact` additionally requires the offset to match, and the wire form preserves it, so `EqualsExact` round-trips too.
- **Eligibility consequence:** a record with a `DateTimeOffset` field becomes §9.8 synthesis-eligible with no user-written `Serialize` — the round-trip-a-specific-instant hole 0010 open question #1 named is now filled by a first-class type.

`TimeZoneInfo` is **not** serializable in v1 (it is a process-interned reference, not a value): serialize the zone's `Id: string` and re-resolve with `FindSystemTimeZoneById` on read. Recorded as a v1 non-goal, not an omission.

---

## Open questions (owner ruling requested before landing)

1. **Instant-based equality on a value record (D3).** `DateTimeOffset.Equals`/hash/`CompareTo` are defined over the *instant*, overriding the structural record equality (which would compare both the wall reading and the offset) via the `=`/`<>` overload §25.2 sanctions for records. This is the .NET shape (`Equals` instant-based, `EqualsExact` offset-sensitive) and is the only order consistent with "happened before," but it is the stdlib's first value record whose equality is a *projection* of its fields rather than structural. Confirm the divergence is acceptable, or prefer structural `Equals` + an `EqualsInstant`/`CompareTo`-only ordering.
2. **`TimeZoneInfo` home / auto-import.** This proposal places `TimeZoneInfo` (and its two exceptions) in auto-imported `System`, so "convert to local time" is import-free like `DateTime.Now`. The alternative is a `System.` submodule requiring `import` (keeping the filesystem-touching tz surface behind an explicit import, and its exceptions off the auto-imported surface). Which posture is intended?
3. **v2 scope for wall-clock → instant resolution (D5/D8).** Confirm that deferring the ambiguity-resolving constructor (with an explicit `AmbiguousTimeResolution` policy) to v2 — shipping only the detection predicates in v1 — is the right cut, versus shipping a policy-bearing resolver now.

---

## Declined alternatives (summary)

| Alternative | Reason declined |
|---|---|
| `DateTime` grows an offset field instead of a new type | Conflates civil time (no offset) with instant-with-offset; breaks `DateTime`'s "`Kind` not normalized, tick-order compare" contract (§2.5); 0010's conditional-`Z` civil wire depends on `DateTime` staying civil |
| `.NET`-style implicit `DateTime(Unspecified) → DateTimeOffset` as local | Silent wrong-instant footgun when the value did not originate in the local zone; Bezel faults (`ArgumentException`) and requires an explicit offset (D2) |
| `DateTimeOffset` carries a `TimeZoneInfo`, not a fixed offset | Reintroduces every DST hazard into the value type; the fixed offset is closed under arithmetic (D4) and serializes self-describingly (D7) |
| Structural (both-field) equality for `DateTimeOffset` | Would order/compare by wall reading, disagreeing with instant order; `EqualsExact` already covers the offset-sensitive case (D3) |
| Zone-aware civil arithmetic on `DateTimeOffset` (`AddDays` honoring DST) | The value type has no zone; a DST-honoring "add a civil day" is `TimeZoneInfo`'s job and needs an ambiguity policy — deferred to v2 (D5) |
| `TimeZoneInfo.TryConvert(wallClock, tz, out DateTimeOffset?)` in v1 | For an ambiguous wall reading it silently picks one of two instants behind a `bool`; the detection predicates (`IsAmbiguousTime`/`IsInvalidTime`) surface the ambiguity honestly and bake in no policy (D8) |
| Bundle the IANA tz database in v1 | Ships a snapshot that goes stale on every IANA update and duplicates the OS's maintained DB; deferred behind an evidence bar (a named consumer the host DB can't serve) (D6) |
| Windows-id (`"Eastern Standard Time"`) lookup in v1 | Requires the separately-versioned CLDR mapping table — the packaging debt v1 avoids; Bezel is Linux-first; revisit with a Windows port (D6) |
| Serialize `TimeZoneInfo` as a value | It is a process-interned reference, not a value; serialize its `Id: string` and re-resolve (recorded v1 non-goal) |
