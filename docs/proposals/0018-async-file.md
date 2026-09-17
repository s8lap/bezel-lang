# Proposal 0018 — Async `File` family: `ReadAllBytesAsync`, `ReadAllLinesAsync`, `WriteAllBytesAsync`, `WriteAllLinesAsync`, `AppendAllTextAsync`

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Target module** | `System.IO` (extends stdlib §7.3 `File`) |
| **Written against** | spec/stdlib **v0.177** |
| **Currency note** | Re-verified clean at v0.306 (2026-09-17 sweep): §1's transcribed §7.3 `File` surface still matches the landed text member-for-member (including the `:=`-defaulted `token` parameters, so the v0.290 overload-pair rule takes no edit here); `ReadOnlySpan<byte>` is a live type and the #1438 exclusion stands. No tier-1 respells, no tier-2 flags. Written-against pin unchanged. |
| **Tracking issue** | [#1447](https://github.com/s8lap/bezel-lang/issues/1447) |
| **Gap-review item** | #22 (2026-07 ranking, wave D2) |
| **Design character** | Mechanical — the value of this proposal is **exactness**, not creativity |

The gap review verdict: "Full async File family (bytes/lines) — P1, mechanical after D1 streams." This proposal mirrors the existing §7.3 sync surface with async duals, applying the Bezel async rules (§30) uniformly.

---

## 1. What exists today (the substrate this builds on)

The complete current §7.3 `File` surface, verified against the stdlib:

```bezel
// System.IO — File (existing, stdlib §7.3)
public static class File
  public static method ReadAllText(path: string): string;
  public static method WriteAllText(path: string, contents: string);
  public static method ReadAllBytes(path: string): owned Array<byte>;
  public static method WriteAllBytes(path: string, contents: Array<byte>);
  public static method WriteAllBytes(path: string, contents: ReadOnlySpan<byte>);  // v0.165 (#1358)
  public static method ReadAllLines(path: string): owned Array<string>;
  public static method WriteAllLines(path: string, lines: IEnumerable<string>);

  // Already async (existing):
  public static async method ReadAllTextAsync(path: string,
                                               token: CancellationToken := CancellationToken.None): Task<string>;
  public static async method WriteAllTextAsync(path: string, contents: string,
                                                token: CancellationToken := CancellationToken.None): Task;

  public static method Exists(path: string): bool;
  public static method Delete(path: string);
  public static method Copy(source: string, destination: string, overwrite: bool);
  public static method Move(source: string, destination: string);
end File;
```

**Already async:** `ReadAllTextAsync` and `WriteAllTextAsync`. **Missing async duals:** bytes, lines, and append-text.

**IO exceptions** (stdlib §7.5): `FileNotFoundException`, `IOException`, etc. — raised on failure, same as the sync forms.

---

## 2. The ownership rule for async file results

The key rule from spec [§30.5](../Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal):

- `Task<string>`: string is runtime-managed → a plain `Task<string>` is correct (no disposal obligation on the string itself; the awaiter owns nothing that needs `using var`).
- `OwningTask<T>` where T is a class: **Pattern B** — the task is single-consumer; `await` transfers ownership of the result to the awaiter. The result is a freshly-allocated class instance.

`Array<byte>` is a class (heap-allocated, non-trivial destructor). `ReadAllBytesAsync` therefore returns `OwningTask<Array<byte>>` — Pattern B. This matches the precedent established by `Stream.ReadAllAsync(): OwningTask<Array<byte>>` (stdlib [§7.1](../Bezel_Standard_Library.md#sec-stream)).

`Array<string>` is also a class. `ReadAllLinesAsync` returns `OwningTask<Array<string>>` by the same rule.

Void-returning async methods (`WriteAllBytesAsync`, `WriteAllLinesAsync`, `AppendAllTextAsync`) return `Task` (no result value, no ownership question).

---

## 3. The borrow-bearing parameter restriction

**`WriteAllBytesAsync` does NOT include a `ReadOnlySpan<byte>` overload.**

The sync surface has `WriteAllBytes(path, contents: ReadOnlySpan<byte>)` (v0.165, #1358, the serializer-output zero-copy path). A corresponding async form would require the implementation to hold a `Span`/`ReadOnlySpan` value across `await` suspension points in the internal state machine. The span's underlying buffer must outlive every suspension, but the implementation cannot statically prove this for an arbitrary caller-supplied buffer — the borrow tracker would reject the implementation body.

This restriction is recorded as issue [#1438](https://github.com/s8lap/bezel-lang/issues/1438): borrow-bearing parameters are not admitted on these async convenience methods. Callers who need async write with a span-bounded buffer should use `FileStream` + `WriteAsync(buffer: ReadOnlySpan<byte>, ...)` directly (the stream form is safe because the span's lifetime is bounded by a single `await`, not by the full method duration).

---

## 4. Proposed API surface

All items below are **proposed** (not in the current stdlib). They extend the existing `File` class in §7.3.

```bezel
// Extensions to File (System.IO, §7.3):
public static class File
  // ... (existing members unchanged — see §1 above)

  /// Reads the entire contents of a file as a byte array.
  /// Result ownership: OwningTask (Pattern B, §30.5) — the Array<byte> is
  /// freshly allocated; the awaiter takes sole ownership.
  /// Precedent: Stream.ReadAllAsync(): OwningTask<Array<byte>> (§7.1).
  public static async method ReadAllBytesAsync(
      path: string,
      token: CancellationToken := CancellationToken.None): OwningTask<Array<byte>>;   // PROPOSED

  /// Writes a byte array to a file (create or overwrite).
  /// No ReadOnlySpan<byte> overload — borrow-bearing async params forbidden
  /// per #1438; use FileStream + WriteAsync(ReadOnlySpan<byte>, ...) for that path.
  public static async method WriteAllBytesAsync(
      path: string,
      contents: Array<byte>,
      token: CancellationToken := CancellationToken.None): Task;   // PROPOSED

  /// Reads all lines of a file as an owned string array.
  /// Line endings are stripped (CR, LF, CRLF — same rule as ReadAllLines).
  /// Result ownership: OwningTask (Pattern B) — Array<string> is a class.
  public static async method ReadAllLinesAsync(
      path: string,
      token: CancellationToken := CancellationToken.None): OwningTask<Array<string>>;   // PROPOSED

  /// Writes a sequence of lines to a file (create or overwrite).
  /// A newline is written after each line (same rule as WriteAllLines).
  public static async method WriteAllLinesAsync(
      path: string,
      lines: IEnumerable<string>,
      token: CancellationToken := CancellationToken.None): Task;   // PROPOSED

  /// Appends text to a file (creates the file if it does not exist).
  /// Does not append a trailing newline — identical behavior to the
  /// sync companion AppendAllText (see §5 for the sync gap note).
  public static async method AppendAllTextAsync(
      path: string,
      contents: string,
      token: CancellationToken := CancellationToken.None): Task;   // PROPOSED

end File;
```

---

## 5. The `AppendAllText` sync gap

`AppendAllTextAsync` has no sync counterpart in §7.3 today — `File.AppendAllText` is absent. This proposal adds only the async form because the gap review's item #22 is specifically "Full async File family." The sync omission is a companion gap that should be addressed in the §7.3 cleanup pass (a small, obvious addition).

Recommended: land `AppendAllText(path: string, contents: string)` as a §7.3 addition alongside this proposal's acceptance — the two are tightly coupled. Tracking is subsumed under this issue (#1447) unless the owner prefers a separate issue for the sync form.

---

## 6. Tour by example

```bezel
// --- Pattern 1: load binary data async ---
using var data := await File.ReadAllBytesAsync(cachePath, token);
ProcessBytes(data.AsReadOnlySpan());   // stdlib §4.14 span access

// --- Pattern 2: write binary result async ---
using var result := await ComputeAsync(token);
await File.WriteAllBytesAsync(outputPath, result, token);
// Note: result is Array<byte>, NOT a Span — the borrow-form is not available async

// --- Pattern 3: read lines async ---
using var lines := await File.ReadAllLinesAsync(configPath, token);
for var line in lines do
  ParseConfigLine(line);
end for;
// `lines` disposed at scope end — releases the Array<string> allocation

// --- Pattern 4: append async ---
await File.AppendAllTextAsync(logPath, $"[{DateTime.UtcNow}] done\n", token);
```

---

## 7. Completeness check: the full §7.3 surface vs. the async duals

| Sync method | Async dual | Status |
|---|---|---|
| `ReadAllText` | `ReadAllTextAsync` | Exists (§7.3) |
| `WriteAllText` | `WriteAllTextAsync` | Exists (§7.3) |
| `ReadAllBytes` | `ReadAllBytesAsync` | **Proposed** |
| `WriteAllBytes(Array<byte>)` | `WriteAllBytesAsync(Array<byte>)` | **Proposed** |
| `WriteAllBytes(ReadOnlySpan<byte>)` | *(no async form — #1438)* | Declined |
| `ReadAllLines` | `ReadAllLinesAsync` | **Proposed** |
| `WriteAllLines` | `WriteAllLinesAsync` | **Proposed** |
| `AppendAllText` *(absent in §7.3)* | `AppendAllTextAsync` | **Proposed**; sync companion also absent (see §5) |
| `Exists`, `Delete`, `Copy`, `Move` | *(no async dual)* | Not applicable — no meaningful async-only form for these |

`Exists`, `Delete`, `Copy`, `Move` have no async dual because they are metadata or rename operations that do not involve streaming a large buffer; their syscall overhead is trivial and the added complexity of a `Task` return type is not justified.

---

## 8. Design decisions and alternatives

### Q1 — `OwningTask<Array<byte>>` vs `Task<Array<byte>>` for the bytes result

The result of `ReadAllBytesAsync` is a freshly-allocated `Array<byte>`. The awaiter is the sole owner and must eventually dispose it. `Task<T>` (Pattern A) for a class result means the *task* owns the result and disposes it when the task is disposed — the awaiter only borrows it. For file bytes that the caller wants to hold beyond the task's lifetime, Pattern A is wrong. `OwningTask<Array<byte>>` (Pattern B) is the correct form: the `await` transfers ownership to the awaiter's binding location, and the caller disposes via `using var`.

The same reasoning applies to `ReadAllLinesAsync`: `Array<string>` is a class.

### Q2 — Why no `encoding` parameter?

The sync `ReadAllText` has no explicit encoding parameter — it reads UTF-8 (matching the Bezel source encoding assumption). For v1 the async forms maintain the same simplicity. Encoding-explicit forms can be added once `StreamReader` (proposal 0006) lands; at that point `new StreamReader(FileStream, encoding).ReadToEndAsync()` is the composable path for non-UTF-8 files.

### Q3 — `IEnumerable<string>` for `WriteAllLinesAsync`

Matches the sync `WriteAllLines(path, lines: IEnumerable<string>)` exactly. `IOwningEnumerable<string>` is unnecessary because `string` is runtime-managed and has no disposal obligation in collection slots.

---

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

This is a mechanical async-dual mirror of the §7.3 `File` surface; it was verified exact against v0.177. The §8 design decisions carry as drafted — Q1: `ReadAllBytesAsync`/`ReadAllLinesAsync` return `OwningTask<Array<byte>>`/`OwningTask<Array<string>>` (Pattern B, §30.5; `Array<T>` is a class), matching `Stream.ReadAllAsync`; Q2: no `encoding` parameter in v1 (mirrors sync `ReadAllText`; the composable non-UTF-8 path is `new StreamReader(FileStream, encoding).ReadToEndAsync()` once 0006 lands); Q3: `WriteAllLinesAsync` takes `IEnumerable<string>` (string is runtime-managed, no owning form needed). The `WriteAllBytes(ReadOnlySpan<byte>)` async form is correctly omitted per #1438 (borrow-bearing async parameters).

- **Sync `AppendAllText` rides along (§5): confirmed.** `File.AppendAllText(path, contents)` is absent from §7.3 today and is tightly coupled to `AppendAllTextAsync`; land the sync companion in the same revision, tracked under **#1447** (no separate issue).

## 9. Relationship to existing issues and proposals

- Gap-review item #22; wave D2 per the [stdlib gap design plan](stdlib-gap-design-plan-2026-07.md).
- The gap review note: "Mechanical after D1 streams" — confirmed; no dependency on proposal 0006 (stream family) because the `File` convenience methods are high-level and do not expose streams.
- Issue [#1438](https://github.com/s8lap/bezel-lang/issues/1438): the borrow-bearing-async-param restriction that excludes the `ReadOnlySpan<byte>` async form.
- Precedent: `Stream.ReadAllAsync(): OwningTask<Array<byte>>` (stdlib §7.1) — the `OwningTask` pattern for allocated byte results is already established.
