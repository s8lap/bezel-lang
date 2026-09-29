# Proposal 0016 — Monotonic time: `Instant`, `Stopwatch` extensions, `TimeSpan.FromNanoseconds`

| | |
|---|---|
| **Status** | under-review — draft |
| **Target module** | `System` (auto-imported — `Instant`, `TimeSpan` additions); `System.Diagnostics` (§8.1 `Stopwatch` extensions) |
| **Written against** | spec/stdlib **v0.177** |
| **Tracking issue** | [#1444](https://github.com/s8lap/bezel-lang/issues/1444) |
| **Gap-review item** | #26 (2026-07 ranking, wave D2) |
| **Currency note** | Re-verified clean at v0.306 (2026-09-17 sweep): no tier-1 respell — stdlib §2.5 and §8.1 (anchors `sec-system-auto-imported`, `sec-stopwatch`) still resolve, and §1's enumeration of `Stopwatch`'s existing members matches the landed block exactly (`Restart()` never existed and was retired as a claim at v0.290; §8.1 now states `Reset()` stops and zeroes `Elapsed`, and `sw.Reset(); sw.Start();` — the §5 Pattern-2 spelling — is the documented restart form). One tier-2 flag recorded in `proposals-currency-2026-09-17.md` §3 (§4.1/§6 Q5's "`IComparable` gives the `<`/`>`/`<=`/`>=` operator group"). Written-against pin unchanged. |

Gap-review verdict: "One decision: ns resolution vs TimeSpan's tick." This proposal settles that decision and adds the three related API surfaces.

---

## 1. What exists today (the substrate this builds on)

All items below are in the current stdlib; nothing is invented here.

- **`TimeSpan`** (stdlib [§2.5](../Bezel_Standard_Library.md#sec-system-auto-imported)) — stores duration as a count of **100-nanosecond ticks** (`Ticks: long`); factory `FromTicks(ticks: long)`, `FromMilliseconds`, `FromSeconds`, `FromMinutes`, `FromHours`, `FromDays`; component accessors `TotalMilliseconds: double`, etc.  No `FromNanoseconds` or `TotalNanoseconds` exist.
- **`DateTime`** (stdlib §2.5) — wall-clock calendar value. `DateTime.Now` / `DateTime.UtcNow` read the system clock.  Not monotonic — advancing the system clock or daylight-saving transitions can make `DateTime.Now` jump or repeat.
- **`Stopwatch`** (stdlib [§8.1](../Bezel_Standard_Library.md#sec-stopwatch)) — wraps the OS high-resolution timer; exposes `Elapsed: TimeSpan`, `ElapsedMilliseconds: long`, `IsRunning: bool`, `Start()`/`Stop()`/`Reset()`, and the static factory `StartNew(): owned Stopwatch`. Missing: `ElapsedTicks: long` (raw hardware ticks) and `Frequency: long` (hardware ticks per second, the denominator that converts `ElapsedTicks` to wall time).

**What is missing:** a standalone monotonic timestamp value (no Stopwatch instance required), sub-millisecond `Stopwatch` introspection, and nanosecond entry/exit points on `TimeSpan`.

---

## 2. Core design decision: resolution

**Decision (settled by this proposal):** `Instant` stores its value in **nanoseconds** internally. The difference of two `Instant` values is returned as a `TimeSpan` — the normal stdlib duration type, which counts whole **100 ns ticks** (§2.5). The ns-resolution internal difference is therefore **truncated toward zero to the nearest tick** on the way into the `TimeSpan` (exactly as `FromNanoseconds` does below); a `TimeSpan` never carries a sub-tick component. A `TotalNanoseconds: double` view is added to `TimeSpan` — it is exactly `Ticks * 100.0`, so a duration can be read in nanoseconds without a manual multiply. Callers who need finer-than-tick platform resolution read `Stopwatch.ElapsedTicks`/`Frequency`, not `TimeSpan`. `TimeSpan.FromNanoseconds(ns: long): TimeSpan` completes the round-trip; it **truncates** to the nearest tick (floors toward zero, discarding the sub-100 ns remainder).

Rationale for this layering:

1. **`TimeSpan` remains the duration currency.** Arithmetic, delays, timeouts, and the rest of the stdlib all traffic in `TimeSpan`. Introducing a parallel `Duration` type would fragment every API that accepts a duration.
2. **The Instant–TimeSpan gap is explicit, not silent.** Callers who need nanoseconds read `TotalNanoseconds`; callers who compute durations with arithmetic get 100 ns resolution automatically. The truncation that happens in `FromNanoseconds` is stated here and in the API doc — no silent precision loss.
3. **Hardware counters are ns-resolution or finer.** Exposing `ElapsedTicks` + `Frequency` on `Stopwatch` gives callers full platform resolution when they need it; the `TimeSpan`-based path silently absorbs the last sub-tick, which is fine for the 99 % case.

**Truncation rule for `TimeSpan.FromNanoseconds`:** result is `TimeSpan.FromTicks(ns / 100)` — integer division, round-toward-zero, sub-100 ns remainder discarded. Negative inputs: `FromNanoseconds(-150)` → `TimeSpan.FromTicks(-1)` (−100 ns, discard the −50 ns remainder toward zero). The truncation direction is always toward zero, matching the existing `FromMilliseconds(double)` floor-toward-zero behavior.

---

## 3. `Instant` — what it is and what it is not

`Instant` is a **monotonic timestamp**: a value that increases with platform time and never goes backward. It answers "how much time has elapsed between two points" without claiming to know what the wall-clock time was at either point. Specifically:

- `Instant` **has no conversion to `DateTime`**. That conversion requires knowing the epoch, which is OS-specific and can change (timezone, NTP steps). The absence is by design — a function that needs a `DateTime` reads `DateTime.UtcNow`; a function that needs elapsed time uses `Instant`.
- `Instant` **has no `ToString()`** that renders a human-readable clock time. The only useful rendition of an `Instant` is its difference from another `Instant`, which is a `TimeSpan`.
- `Instant.Now` reads the monotonic clock on every access (like `DateTime.Now`), not a cached value.

### Why not just use `Stopwatch`?

`Stopwatch` requires construction (`owned Stopwatch`), state (`Start`/`Stop`), and an allocation. For "stamp here, stamp there, compute the gap" patterns — the common case in tracing, benchmarking, and timeout checks — `Instant.Now` is lower-overhead and does not require managing an owned resource.

`Stopwatch` remains valuable for repeated pause/resume measurement (lap timing, code profiling over many iterations) where the accumulation behavior matters.

---

## 4. Proposed API surface

All items below are **proposed** (not in the current stdlib).

### 4.1 `Instant` (new record, `System`, auto-imported)

```bezel
/// A monotonic timestamp. Differences between Instant values are always
/// non-negative when taken in program order. No conversion to DateTime exists —
/// per §2.5 DateTime is a wall-clock value and is not monotonic.
public record Instant: IComparable<Instant>, IEquatable<Instant>
  // Reads the platform monotonic clock (CLOCK_MONOTONIC on POSIX,
  // QueryPerformanceCounter on Windows). Each access is a fresh reading.
  public static readonly property Now: Instant;

  public method CompareTo(other: Instant): int;   // operator group agrees

  // The only arithmetic defined on Instant: the gap between two timestamps.
  // Result is always non-negative when `a` was sampled after `b` in program
  // order; the sign reflects the raw difference (Instant is a signed value).
  public static method operator -(a: Instant, b: Instant): TimeSpan;

  // No ToString() rendering wall-clock time — the value is opaque.
  // Interpolate a difference instead: $"{Instant.Now - start} elapsed".
end Instant;
```

### 4.2 Extensions to `Stopwatch` (extends existing §8.1 declaration)

```bezel
// Additions to the existing Stopwatch class in System.Diagnostics:
public class Stopwatch
  // ... (existing members unchanged: StartNew, IsRunning, Elapsed,
  //      ElapsedMilliseconds, Start, Stop, Reset)

  /// Raw hardware ticks elapsed while the stopwatch was running.
  /// Divide by Frequency to get seconds. Monotonically non-decreasing
  /// between Stop and the next Start. Resets to 0 on Reset().
  public readonly property ElapsedTicks: long;   // PROPOSED

  /// Hardware ticks per second on this platform. Constant for the
  /// process lifetime. Typically 10^9 on Linux (CLOCK_MONOTONIC ns),
  /// 10^7 on Windows (100 ns QPC), but callers must not hard-code these.
  public static readonly property Frequency: long;   // PROPOSED
end Stopwatch;
```

### 4.3 Extensions to `TimeSpan` (extends existing §2.5 declaration)

```bezel
// Additions to the existing TimeSpan record in System:
public record TimeSpan
  // ... (existing members unchanged)

  /// Total elapsed nanoseconds as a double: exactly Ticks * 100.0. TimeSpan
  /// stores duration as whole 100 ns ticks (§2.5), so there is no sub-tick
  /// component to preserve — an Instant difference is truncated toward zero to
  /// the nearest tick just like FromNanoseconds, and every TimeSpan therefore
  /// satisfies TotalNanoseconds = Ticks * 100.0 exactly. Callers who need
  /// finer platform resolution read Stopwatch.ElapsedTicks / Frequency.
  public readonly property TotalNanoseconds: double;   // PROPOSED

  /// Construct a TimeSpan from a nanosecond count. Truncates toward zero to
  /// the nearest 100 ns tick: FromNanoseconds(150) → 1 tick (100 ns);
  /// FromNanoseconds(-150) → -1 tick (-100 ns). Sub-100 ns remainder is
  /// discarded and unrecoverable. Instant subtraction truncates the same way,
  /// so no TimeSpan carries sub-tick precision; when full hardware resolution
  /// matters, use Stopwatch.ElapsedTicks / Frequency rather than a TimeSpan.
  public static method FromNanoseconds(ns: long): TimeSpan;   // PROPOSED
end TimeSpan;
```

---

## 5. Tour by example

```bezel
// --- Pattern 1: elapsed time, no resource management ---
var start := Instant.Now;
DoWork();
var elapsed := Instant.Now - start;   // TimeSpan
Console.WriteLine($"Work took {elapsed.TotalMilliseconds:F2} ms");

// Nanosecond view via TotalNanoseconds (= Ticks * 100.0):
Console.WriteLine($"Work took {elapsed.TotalNanoseconds:F0} ns");

// --- Pattern 2: Stopwatch for repeated-lap profiling ---
using var sw := Stopwatch.StartNew();
for var i in 0..100 do
  Iteration();
  Console.WriteLine($"Lap {i}: {sw.Elapsed.TotalMilliseconds:F3} ms");
  sw.Reset(); sw.Start();
end;

// --- Pattern 3: raw ticks for zero-overhead profiling ---
using var prof := Stopwatch.StartNew();
var t0 := prof.ElapsedTicks;      // ElapsedTicks is an instance property (§4.2); Frequency is a process-constant static
// ... tight loop ...
var dt := double(prof.ElapsedTicks - t0) / double(Stopwatch.Frequency);
// dt is seconds, full platform resolution

// --- Pattern 4: TimeSpan.FromNanoseconds ---
var delay := TimeSpan.FromNanoseconds(500_000);   // 500 µs = 5000 ticks, sub-tick remainder 0
```

---

## 6. Design decisions and alternatives considered

### Q1 — Why `Instant` as a record, not a static `MonotonicClock` class?

A record value is the natural representation: timestamps are compared, subtracted, and passed around; they have no disposal obligation; they are value-semantically copied. A static class would force "stamp the clock" to be expressed differently from "compute a delta," and there's no state to manage. The `Instant` record is the correct model and matches the approach in Go (`time.Time`) and Rust (`std::time::Instant`).

### Q2 — Why no `Instant.Add(TimeSpan)` / `Instant + TimeSpan`?

Adding a `TimeSpan` to an `Instant` produces a "future instant" — a value that looks like a timestamp but isn't one that was actually observed. The gap review's note that monotonic time must not be used for wall-clock reasoning applies equally here: a computed future timestamp is just a duration relative to `Instant.Now`, and that's better expressed as `Instant.Now + timeout > checkPoint` — but `Instant.Now + timeout` is still an unobserved fake instant. We decline addition for v1 and require callers who want a deadline to work with a `TimeSpan` deadline (`TimeSpan.FromSeconds(5)`) rather than an `Instant` deadline. Can be revisited with evidence.

### Q3 — Why is `Frequency` static instead of instance?

On all major platforms the hardware counter frequency is a process-global constant: it does not change between `Stopwatch` instances and does not change after the process starts. An instance property that always returns the same value misleads callers into thinking they need a `Stopwatch` just to read it. `Stopwatch.Frequency` as a static property is the natural form.

### Q4 — `TotalNanoseconds` as `double` — precision loss?

A `double` has 53 bits of mantissa, which covers ~9 × 10^15 nanoseconds (about 104 days) at full nanosecond resolution. For durations longer than ~104 days the sub-nanosecond precision is lost, but for any timing measurement that fits in a `TimeSpan` (which is bounded by `long.MaxValue * 100 ns ≈ 29,247 years`) the double is imprecise for very long durations. We accept this: `TotalNanoseconds` is intended for short profiling windows (microseconds to seconds), not for computing multi-year differences. Callers needing exact tick counts use `ElapsedTicks * 100L` for nanoseconds.

### Q5 — `Instant` comparison operators?

`IComparable<Instant>` gives `<`, `>`, `<=`, `>=` through the standard operator group (spec §17, the IComparable convention). No separate `operator -` on `Instant + TimeSpan → Instant` (see Q2). `=` / `<>` are synthesized from `IEquatable<Instant>` (spec §9.2).

---

## 7. What is explicitly deferred

- **`TimeSpan.TotalMicroseconds`** — a natural companion; deferred because it is trivially computable as `TotalMilliseconds * 1000` and no current API uses it in a parameter position. Track under this issue.
- **Monotonic clock epoch** — the raw nanosecond count from which `Instant` ticks are measured is intentionally unspecified. Callers that need an epoch (e.g., log timestamps with POSIX monotonic offsets) must bridge to `DateTime.UtcNow` themselves; the bridge is not provided because it is OS-specific.
- **`TimeSpan.FromMicroseconds`** — same reasoning as `TotalMicroseconds`; trivially computable, no current API parameter needs it. Track alongside.

---

## 8. Relationship to existing issues and proposals

- Gap-review item #26; wave D2 per the [stdlib gap design plan](stdlib-gap-design-plan-2026-07.md).
- The gap review dedup note: "Instant, Stopwatch.ElapsedTicks/Frequency, and TimeSpan.FromNanoseconds merged into one monotonic-time cluster (one shared decision: specify tick resolution as nanoseconds)." This proposal implements that cluster.
- Unblocks: any caller that needs a monotonic deadline without a `Stopwatch` allocation (timeout checks in tight loops, tracing, micro-benchmarks).
