# Proposal 0006 — The concrete stream family (`StreamReader`/`StreamWriter`, `MemoryStream`, `CopyToAsync`)

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Target module** | `System.IO` (stdlib §7) |
| **Written against** | spec/stdlib **v0.177** |
| **Tracking issue** | [#1433](https://github.com/s8lap/bezel-lang/issues/1433) |
| **Gap-review item** | #1 (top of the 2026-07 ranking), wave D1 |
| **Design influences** | .NET `StreamReader`/`StreamWriter`/`MemoryStream` for the shape; Bezel ownership for the wrapping semantics; §30's async model for the streaming forms |
| **Currency note** | Refreshed 2026-08-11 to v0.246: caller-side transfer marker respelled `move` (`new StreamReader(move fs)`); declaration-side `owned` unchanged. Re-verified clean at v0.306 (2026-09-17 sweep) — no tier-1 respell: stdlib §3.2/§4.4/§4.14/§7.1–§7.5 and spec §28.4/§28.5/§28.12/§29.11/§30.7–§30.9 all still resolve; `Stream`'s `(offset, count)` byte window is the recorded exception to the v0.282 `(start, length)` convention (stdlib §1), and §29.11's streaming-reader clause is unchanged. Two tier-2 flags recorded in `proposals-currency-2026-09-17.md` §3 (§6 Q1's scoped-construction status; §7.4's `TextReader.Read()` recorded misfit). Written-against pin unchanged. |

This is the review's own top item and the dependency root of wave D1. The stdlib declares `TextReader`/`TextWriter` **abstract with no concrete subclass** (stdlib [§7.4](../Bezel_Standard_Library.md#sec-textreader-textwriter-directory-path)) and ships **no in-memory `Stream`** — so nothing stream-shaped can be tested without touching the filesystem, no large file can be read line-by-line, and every downstream consumer (Process stdio, gap item #5; Http body streaming, [#1376](https://github.com/s8lap/bezel-lang/issues/1376); the async `File` family, item #22) is blocked. This proposal fills that gap with the three concrete types the review named, and nothing more.

Every snippet was checked against the v0.177 grammar (Appendix A) and against the actual stdlib surface before filing: no method or type is used that isn't either already in `Bezel_Standard_Library.md` (cited) or introduced here as **proposed** (marked).

---

## 1. What exists today (the substrate this builds on)

Everything below is already in the stdlib; the proposal composes over it.

- **`Stream`** (abstract, stdlib [§7.1](../Bezel_Standard_Library.md#sec-stream)) — `Read`/`Write` in both the `(Array<byte>, offset, count)` windowed form and the `Span<byte>`/`ReadOnlySpan<byte>` form; `ReadAsync`/`WriteAsync` in both forms; `Flush`; `Seek`/`Length`/`Position`; and the convenience `ReadAll(): owned Array<byte>` / `ReadAllAsync(): OwningTask<Array<byte>>`. `FileStream: Stream` (§7.2) is the one concrete subclass.
- **`TextReader`** (abstract, §7.4) — `Read(): int` (−1 at EOF), `ReadLine(): string?` (null at EOF), `ReadToEnd(): string`, `destructor`. **No concrete subclass exists.**
- **`TextWriter`** (abstract, §7.4) — `Write(value: string)`, `Write(value: char)`, `WriteLine()`, `WriteLine(value: string)`, per-type `Write`/`WriteLine` overloads for the primitives (§2.1 note), `Flush`, `destructor`. **No concrete subclass exists.**
- **`Encoding`** (abstract, §3.2) — `GetBytes`/`GetString`/`GetByteCount`, the `UTF8`/`UTF16`/`UTF32`/`ASCII` singletons, and a **defined-fault** error model: malformed input raises `FormatException`, never a silent substitution.
- **`OwningBuffer<byte>` / `Span<byte>` / `ReadOnlySpan<byte>`** (§4.14) with the endianness-aware byte-buffer accessors — the runtime-sized byte storage primitive.
- **`Array<byte>`** (§4.4) with the sized constructor `new Array<byte>(size, fill)`; **`CancellationToken`** (§6.1); **`OwningTask<T>`** (spec §30.3/§30.5); **`IAsyncEnumerable<T>`** (spec §30.7); **`StringBuilder`** (§3.1); the IO exceptions `IOException`/`EndOfStreamException`/… (§7.5).

Three things are missing, and this proposal supplies exactly them: the **concrete `TextReader`/`TextWriter`** (`StreamReader`/`StreamWriter`), an **in-memory `Stream`** (`MemoryStream`), and **`Stream.CopyToAsync`**.

The types live in `System.IO` and require `import System.IO;` (like the rest of §7 — none are auto-imported, §7.5).

## 2. Tour by example

Snippets are **method-level fragments**; the enclosing `module`/class is elided and `import System.IO;` is assumed.

```bezel
// Read a large file line by line without buffering the whole thing — the
// review's marquee blocked scenario. The reader OWNS the file it opened;
// disposing the reader closes the file (§28.4 cascade). Single `using var`.
public async method CountErrorsAsync(path: string): Task<int>
  using var reader := new StreamReader(path);
  var n := 0;
  await for var line in reader.ReadLinesAsync() do
    if line.Contains("ERROR") then n := n + 1; end;
  end;
  return n;
end CountErrorsAsync;

// Wrap a stream you already hold. `new FileStream(...)` is a FRESH producer,
// so no call-site `owned` marker is needed (§22.3) — the reader takes it.
using var reader := new StreamReader(new FileStream(path, FileMode.Open));
var header := reader.ReadLine();               // sync form; null at EOF (§7.4)

// Test a stream entirely in memory — no filesystem. FromBytes takes the
// bytes; the reader takes the stream; one `using var` disposes both.
using var reader := new StreamReader(
    MemoryStream.FromBytes(Encoding.UTF8.GetBytes("a\nb\nc")));
await for var line in reader.ReadLinesAsync() do Process(line); end;

// Write text to a file. The writer owns the FileStream and FLUSHES on
// dispose (§28.12); `await using` surfaces a flush error on the await edge.
public async method SaveAsync(path: string, lines: Array<string>): Task
  await using var writer := new StreamWriter(path);
  for var line in lines do
    await writer.WriteLineAsync(line);
  end;
  // FlushAsync + close happen at scope exit; see §7 (Disposal) for the
  // exception edge.
end SaveAsync;

// Buffer any stream into memory (the owned-clean substitute for wrapping a
// StreamWriter over a MemoryStream — see §3, Q1). CopyToAsync is new on Stream.
public async method BufferAsync(source: Stream): OwningTask<Array<byte>>
  using var ms := new MemoryStream();
  await source.CopyToAsync(ms);
  return ms.ToArray();                         // owned copy, independent of ms
end BufferAsync;
```

## 3. Design decisions

Each decision states the choice, the alternatives considered, and why the choice wins under Bezel's ownership and async rules.

### Q1 — Wrapping ownership: `owned` ctor, one form, v1

**Decision.** `StreamReader`/`StreamWriter` take the wrapped stream **owned** and store it in an `owned` field. The wrapper is the stream's sole owner; the wrapper's compiler-generated `Dispose()` disposes the stream (stdlib §7.4 destructor + spec [§28.4](../Bezel_Language_Spec.md#sec-the-destructor) owned-field cascade, [§28.5](../Bezel_Language_Spec.md#sec-owned-fields)). There is **one** ctor form for wrapping (owned), plus a path convenience that opens and owns a `FileStream` internally.

```bezel
public constructor(owned stream: Stream, encoding: Encoding := Encoding.UTF8);
public constructor(path: string, encoding: Encoding := Encoding.UTF8);  // opens+owns a FileStream
```

The **§22.3 call-site-marker ergonomics (v0.169, #1348)** fall out cleanly. The canonical wrap passes a *fresh producer* (`new FileStream(...)`, or `MemoryStream.FromBytes(...)` which returns `owned`), which "stays unmarked: no live binding is invalidated" — so `new StreamReader(new FileStream(path, FileMode.Open))` needs no marker. If instead the caller holds a *named owned binding*, the call consumes it and the caller writes the marker: `new StreamReader(move fs)` — after which `fs` is moved-from and the caller must **not** also dispose it. Both behaviors are exactly what §22.3 already prescribes for the most destructive passing mode; the design invents nothing.

**Why one form, owned:** disposal chains deterministically with zero ceremony — a single `using var reader := …` closes the reader's buffer *and* the underlying stream. It matches how the stdlib already models wrapping ownership elsewhere: `FileStream` owns its OS handle (§7.2), `CancellationTokenSource` owns its registered-callback list (§6.1), every owning collection owns its elements (§4.11, §28.11). The owned-field cascade is the whole point of §28.5 — "a class with only owned-bearing fields … needs no destructor," so `StreamReader`'s disposal is compiler-generated from `private owned _stream: Stream;` alone.

**Alternative considered — a borrowed-stream ctor (caller keeps the disposal obligation).** This is the `leaveOpen: true` case from .NET, and it is the natural shape for *"write text into a `MemoryStream` I then read back"* (see Q3). It is **deferred to a spec ruling** (now resolved — see §6), not shipped in v1, because a `StreamReader` that *borrows* its stream holds that borrow in a field. A plain (non-`owned`) class-reference field holding a parameter-sourced borrow is *already admissible today* under §28.3's borrowed-field rule — modelled as a **back-reference** (§28.6, `_owner: Customer; // not owned`) — but its guarantee is **residual trust**: the compiler does *not* prove the caller outlives the reader; a use-after-dispose is `DisposedAccessException` in checked mode, UB unchecked. *(Correction to an earlier draft of this section, which read "no existing rule bounds the reader's lifetime by the stream's" — that overstated the gap. The back-reference field **does** bound it, at the residual-trust level; what is missing is not a bound but a **static** one.)* A stdlib wrapper that silently trusts the caller to outlive it, with UB when it doesn't, is precisely the footgun §28's machinery exists to eliminate, so a plain borrow ctor is the wrong *posture* for a stdlib type even though it type-checks. Supplying the missing static bound needs either a borrow-bearing-*class* concept (records already have one, §9.7; classes do not — large) or a **scoped-construction rule** (the smaller, chosen path): generalize §28.14's `scoped` to class-typed borrow parameters, so `constructor(scoped stream: Stream)` *upgrades* the residual trust to a compile-time proof (declared-before-L + freeze + reverse-order disposal). That is real spec surface, and the review already flagged it ("a borrow-based construction needs a spec ruling — ship the owned-ctor subset first and file the borrow question"). **Resolved as the scoped-construction rule (§6).** The v1 workaround for the write-to-memory case is `CopyToAsync` + `MemoryStream` (Q3, Q4) — which is owned-clean and needs no borrow rule.

**Alternative considered — two ctors (owned *and* borrowed) from day one.** Rejected for v1 on the same ground: the borrowed form isn't expressible soundly yet, so shipping "two forms" would mean shipping one real form and one that can't type-check. Ship one; add the second when the ruling lands.

### Q2 — Async-first, with sync duals where §7 already has them

**Decision.** Mirror the shape §7 already establishes: `Stream` and `File` both expose sync operations *and* async duals (`Read`/`ReadAsync`, `ReadAllText`/`ReadAllTextAsync`). `StreamReader`/`StreamWriter` do the same — the sync methods are the ones `TextReader`/`TextWriter` already declare (so the concrete classes are honest subclasses), and the async duals are added for the streaming path.

| Operation | Sync (from `TextReader`/`TextWriter`, §7.4) | Async dual (added here) |
|---|---|---|
| read one line | `ReadLine(): string?` | `ReadLineAsync(): Task<string?>` |
| read all | `ReadToEnd(): string` | `ReadToEndAsync(): Task<string>` |
| iterate lines | `EnumerateLines(): IEnumerable<string>` *(new)* | `ReadLinesAsync(): IAsyncEnumerable<string>` *(new)* |
| write | `Write`/`WriteLine` | `WriteAsync`/`WriteLineAsync(): Task` |
| flush | `Flush()` | `FlushAsync(): Task` |

**Line iteration is the streaming primitive**, and its async form is *exactly* the spec's own canonical async-iterator example — spec [§30.7](../Bezel_Language_Spec.md#sec-async-iterators) shows `ReadLinesAsync(path): IAsyncEnumerable<string>` verbatim. It yields **borrows** of `string` (string is runtime-managed, §28.9, so there is no owned form and no `IOwningAsyncEnumerable` here), consumed with `await for` (§30.7). 

**Interaction with the affine-lazy / single-consumption rule (#1226, spec [§29.11](../Bezel_Language_Spec.md#sec-multiple-enumeration)).** A line enumerable is precisely the case §29.11 closes with: *"A lazy or single-pass source whose backing is itself external, such as a streaming reader, follows the same single-consumption rule."* So `reader.ReadLinesAsync()` (and its sync `EnumerateLines()`) is **single-consumption** — enumerate it at most once; a second enumeration is the `InvalidOperationException` runtime guard (the static half of §29.11 attaches to compiler-known lazy-query bindings, not to a library iterator, so enforcement here is runtime, exactly as for the Http body in 0001 §4.3). This is not a new rule; it is the streaming-reader clause the spec already states, named explicitly so implementers wire the guard.

**No async-only-with-no-sync-dual operations.** Everything a `StreamReader`/`StreamWriter` does has a sync form, because `TextReader`/`TextWriter` are sync-shaped and `Console.In`/`Console.Out` (also `TextReader`/`TextWriter`) must keep working synchronously. The async duals live on the concrete classes only; hoisting them onto the abstract bases (so *any* text reader is async-capable) is a larger change deferred to §5.

### Q3 — `MemoryStream`: growth, cursor, and byte ownership

**Decision.**

```bezel
public class MemoryStream: Stream
  public constructor();                                       // empty, growable, Position 0
  public constructor(capacity: int);                          // Length 0, capacity reserved
  public static method FromBytes(source: Array<byte>): owned MemoryStream;          // copies
  public static method FromBytes(owned buffer: OwningBuffer<byte>): owned MemoryStream;  // takes, zero-copy

  public method ToArray(): owned Array<byte>;                 // fresh copy of [0, Length); re-callable
  public method TakeBytes(): owned OwningBuffer<byte>;        // drains: hands out contents, stream resets
end MemoryStream;
```

- **Cursor model.** A **single** `Position` cursor serves both read and write (as .NET's `MemoryStream` does — not separate read/write heads). `CanRead`/`CanWrite`/`CanSeek` are all `true`. A `Write` at `Position` overwrites existing bytes and extends `Length` when it passes the end; a `Seek` past `Length` followed by a `Write` zero-fills the gap (the §7.1 windowed contract governs the `(buffer, offset, count)` bounds against the *caller's* buffer; the stream's own length grows to fit).
- **Growth policy.** `new MemoryStream()` starts empty and grows on demand by **amortized doubling** of the internal `OwningBuffer<byte>` (O(1) amortized append; the same policy `OwningArray<T>` uses, §4.5). `new MemoryStream(capacity)` pre-reserves. Growth is an implementation detail — the stdlib is "not normative for … internal data structures" (§1) — so no capacity is observable beyond `Length`.
- **`ToArray()` — owned copy.** Returns a freshly allocated `Array<byte>` of exactly `Length` bytes (`[0, Length)`, independent of `Position`), disconnected from the stream. Safe, re-callable, and unaffected by later writes. This is the **default** way to get bytes out.
- **`TakeBytes()` — zero-copy drain.** Transfers the internal buffer out as an `owned OwningBuffer<byte>` of exactly `Length` slots (capacity beyond `Length` is dropped), and **resets** the stream to empty (`Length` 0, `Position` 0). For the hand-off case where a copy is waste — a serializer fills a `MemoryStream`, then hands the buffer onward.
- **`FromBytes`** has two overloads distinguished by the `owned` marker itself: `FromBytes(source: Array<byte>)` **copies** the array's bytes (the everyday case — `MemoryStream.FromBytes(File.ReadAllBytes(path))`, `MemoryStream.FromBytes(Encoding.UTF8.GetBytes(s))`), and `FromBytes(owned buffer: OwningBuffer<byte>)` **takes ownership** zero-copy. The `owned` keyword on one signature and its absence on the other is the whole documentation of the difference.

**Alternative considered — a borrowing `GetBuffer(): ReadOnlySpan<byte>` (view the internal buffer without copying).** Rejected for v1. A `ReadOnlySpan<byte>` is a borrow-bearing record over the internal `OwningBuffer<byte>` (§4.14); the moment a subsequent `Write` triggers a growth **reallocation**, that span dangles. Bounding the span's validity to "until the next write" is exactly the kind of intra-object aliasing invariant the borrow checker cannot express today. `ToArray()` (owned copy) and `TakeBytes()` (drain-then-nothing-aliases) are both safe under the existing rules; the un-copied borrowing view is deferred with the growth-invalidation hazard named (§6).

**Alternative considered — separate read and write cursors.** Rejected: a single cursor matches `Stream`'s `Position` contract (§7.1 declares one `Position`), matches .NET, and keeps `MemoryStream` a faithful `Stream` rather than a special two-headed thing. Code that wants to read what it wrote seeks to 0 (`ms.Position := 0`) or, more idiomatically, calls `ToArray()`.

### Q4 — `CopyToAsync`: buffer, cancellation, completion, errors

**Decision.** Add one non-abstract method to `Stream` (proposed; it does **not** force existing subclasses to change — it has a default body over the existing `ReadAsync`/`WriteAsync`):

```bezel
// added to Stream (§7.1) — proposed
public async method CopyToAsync(destination: Stream,
    bufferSize: int := 81920,
    token: CancellationToken := CancellationToken.None): Task
  using var buffer := new Array<byte>(bufferSize, 0);         // §4.4 sized ctor
  while true do
    token.ThrowIfCancellationRequested();                    // §6.1
    var n := await this.ReadAsync(buffer, 0, buffer.Count, token);  // §7.1
    if n = 0 then break; end;                                // 0 = end of source (§7.1)
    await destination.WriteAsync(buffer, 0, n, token);       // §7.1
  end;
end CopyToAsync;
```

- **Buffer size** default `81920` (80 KiB), matching .NET's long-tuned default; overridable per call as a §22.5 named argument.
- **Cancellation** takes a `CancellationToken` (default `CancellationToken.None`, §6.1) and threads it into every `ReadAsync`/`WriteAsync` *and* checks it between chunks. When the token fires, `OperationCanceledException` propagates (spec [§30.8](../Bezel_Language_Spec.md#sec-cancellation)); the single reused `buffer` is an ordinary owned local disposed by the `using var` on the exception edge (§28.3) — no owned payload is stranded because bytes are copied *through* a reused buffer, never handed across the call boundary.
- **Completion** copies from `this`'s current `Position` to end (source-`Read`-returns-0), matching `Stream.ReadAll`'s "from current position to end" contract (§7.1). It does **not** seek, flush, or dispose `destination` — the caller owns `destination` and decides (matching .NET; a `CopyToAsync` that flushed would double-flush a `StreamWriter`'s own dispose-flush).
- **Error propagation** — an `IOException` (§7.5) from either side faults the returned `Task`, surfaced at the `await` per spec [§30.9](../Bezel_Language_Spec.md#sec-async-methods) (exception handling in async methods).

**Why async-only (no sync `CopyTo` in v1).** A synchronous whole-stream copy blocks the calling domain for the copy's duration; the review lists only `CopyToAsync`, and §30's run-to-completion model makes async the primary form. A sync `CopyTo` is a mechanical dual (drop the awaits, use `Read`/`Write`) and can be added if a non-domain consumer appears — deferred, not forbidden (§5).

**Chunk enumeration (`Chunks`/`ReadChunksAsync`)** — the review's title says "CopyToAsync/**Chunks**." A chunk iterator (`IOwningAsyncEnumerable<Array<byte>>`, each chunk an owned right-sized array) is the *other* streaming shape, and proposal [0001](0001-networking-http.md) §3.3 already designs exactly it as `HttpBodyStream.Chunks`. To avoid shipping two subtly different chunk iterators, v1 gives `CopyToAsync` (the dominant need — "move all the bytes from A to B") and **defers a general `Stream.Chunks`** to a unification with 0001's form (§5, §6).

### Q5 — Partial reads and end-of-stream: the existing reader convention, not a new `Try`

**Decision.** Reuse the sentinels the stdlib's reader surface already defines; introduce **no** `Try` shape.

- `Stream.Read`/`ReadAsync` already return `int` = bytes read, **0 = EOF** (§7.1). `StreamReader` reads *through* the stream and inherits this.
- `TextReader.Read(): int` returns **−1 at EOF**; `TextReader.ReadLine(): string?` returns **null at EOF** (§7.4). `StreamReader` implements these unchanged, and `ReadLineAsync(): Task<string?>` is the async dual with the same null-at-EOF signal.

**Why not the Try convention (§1, v0.171, #1336).** The Try convention exists for **extraction that can fail to find a value** — `TryGetValue`, `TryDequeue` — where the `bool` distinguishes "found" from "a legitimately-null payload." End-of-stream is not that: it is a *normal terminal state*, and the reader's answer is precisely "there is no next line," for which **option (b) of the Try convention — the nullable return `TryX(...): owned T?` — is already the sanctioned shape.** `ReadLine(): string?` *is* that shape (a runtime-managed `string`, so no `owned`). A `bool` + `out string?` would be strictly more ceremony for the same information, and would diverge from the `TextReader` contract this class must honor. So EOF stays a sentinel — `null`/`−1`/`0` — exactly as §7 already establishes, and this is *consistent with* the Try convention rather than an exception to it.

**Malformed bytes are a different axis** and are a *fault*: a byte sequence the encoding can't decode raises `FormatException` (§3.2's defined-fault model), which propagates out of `ReadLine`/`ReadLineAsync`. EOF (expected, a value) and decode failure (unexpected, an exception) are split along the exact line Guide 2 §5 draws.

### Q6 — Encoding: UTF-8 default over the existing `Encoding` singletons

**Decision.** `StreamReader`/`StreamWriter` take an `encoding: Encoding := Encoding.UTF8` parameter (§22.5 default), resolving to the existing §3.2 singletons. **No `Encoding` registry, no new encoding types are invented** — the stdlib already ships `UTF8`/`UTF16`/`UTF32`/`ASCII`, and users may subclass `Encoding` for custom ones (§3.2). v1 is not "UTF-8-only"; it is "UTF-8-*default*, any `Encoding`."

- The writer encodes each text write via `encoding.GetBytes` and writes the bytes to the wrapped stream; the reader decodes incrementally via the encoding.
- **The defined-fault model is inherited verbatim** (§3.2): malformed input → `FormatException`, no replacement characters, no silent substitution. A UTF-8 sequence split across two internal read buffers is an implementation concern (the reader holds the partial tail until the next read completes it); the *observable* contract is "decodes the whole byte stream per the encoding; `FormatException` on a genuinely malformed sequence."
- **No BOM magic.** §3.2 is explicit that `UTF8.GetBytes` never emits a BOM and `UTF8.GetString` does not treat a leading `EF BB BF` specially. `StreamReader`/`StreamWriter` follow suit: no BOM is written, and none is auto-detected or stripped in v1. BOM-sniffing (auto-detecting the encoding from a leading BOM) is a `detectEncoding`-style feature deferred to v2 (§5) — adding it silently would contradict the `Encoding` contract.

**Alternative considered — UTF-8-only, drop the parameter.** Rejected: the parameter costs nothing (the singletons exist, the default covers the common case), and dropping it would make the concrete class *less* capable than the abstract `Encoding` it decodes through. The `encoding :=` default is the smallest surface that is still honest about the four encodings the stdlib ships.

### Q7 — Disposal: flush-on-dispose for the writer, cascade order, the exception edge

**Decision.**

- **`StreamReader`** holds `private owned _stream: Stream;` and needs no custom disposal beyond the cascade — its `Dispose()` is compiler-generated (§28.5): drop any read-ahead buffer (memory-only, no work) and dispose the owned stream. **Dispose-of-wrapped ordering** is spec-fixed: §28.4 runs the (absent) destructor body first, then disposes owned fields — so the stream is closed last, after the reader is done with it.
- **`StreamWriter`** declares a `destructor` that **flushes buffered bytes to the still-open stream**, then the cascade disposes the owned stream. The ordering is load-bearing and guaranteed by §28.4: *step 1 destructor body (flush) → step 2 owned-field disposal (close stream)*. Buffered text therefore reaches the stream while it is still open.

```bezel
public class StreamWriter: TextWriter
  private owned _stream: Stream;
  // … Write/WriteLine/WriteAsync/… buffer into an internal encoder buffer …
  destructor
    // flush the encoder's buffered bytes to _stream (still open here);
    // _stream is disposed by the §28.4 cascade AFTER this body runs.
  end destructor;
end StreamWriter;
```

- **The exception edge (§28.12, §22.9).** A dispose-time flush can *fail* (disk full, broken pipe) — and a `destructor` is expected not to throw (§28.4). So `StreamWriter` **also implements `IAsyncDisposable`** (spec [§28.12](../Bezel_Language_Spec.md#sec-asynchronous-disposal)) with a `DisposeAsync` whose `await FlushAsync()` **surfaces the flush error on the `await using` edge**, where the caller can `catch` it. The synchronous destructor performs a **best-effort** flush; if it fails, the buffered bytes are lost and the failure cannot be surfaced (the "disposers should not throw" rule). The recommendation the docs will carry: **`await using var writer := …`** for any data you must not lose (§28.12's own "prefer `await using` in async contexts"), and an **explicit `await writer.FlushAsync()`** before scope exit when you want the flush error at a named point rather than at dispose. Per §28.12, a class implementing `IAsyncDisposable` with resources *should* also declare a `destructor` (the synchronous fallback) — `StreamWriter` does, so the compiler's "no destructor" warning does not fire.
- **Double-dispose** is a no-op on both classes (§28.2 idempotence); **flush is idempotent** (a second flush with an empty buffer does nothing).

**Alternative considered — no dispose-time flush; require explicit `Flush`.** Rejected: it turns "forgot to flush" into silent data loss with no safety net, the single worst .NET-`StreamWriter` footgun. Flush-on-dispose is the safety net; the `IAsyncDisposable` path is how the error becomes observable rather than swallowed.

**Alternative considered — flush-on-dispose but *not* `IAsyncDisposable`.** Rejected: then the only flush path is the synchronous destructor, whose failure is unreportable (§28.4). The whole reason `IAsyncDisposable` exists (§28.12) is asynchronous cleanup that can *await* and thus *surface* — a flushing writer is the textbook case.

## 4. Proposed API surface

All three types are **new** in `System.IO`; `Stream.CopyToAsync` is a **new method on the existing** abstract `Stream` (§7.1). Members cited to the stdlib are existing; everything else is proposed.

```bezel
// ── the concrete TextReader (§7.4 declared it abstract with no subclass) ──
public class StreamReader: TextReader
  public constructor(owned stream: Stream, encoding: Encoding := Encoding.UTF8);
  public constructor(path: string, encoding: Encoding := Encoding.UTF8);   // opens+owns a FileStream

  public readonly property CurrentEncoding: Encoding;
  public readonly property EndOfStream: bool;

  // sync (TextReader contract, §7.4): Read(): int (−1 EOF), ReadLine(): string? (null EOF),
  //   ReadToEnd(): string — inherited/overridden, unchanged signatures.
  public method EnumerateLines(): IEnumerable<string>;         // single-consumption (§29.11)

  // async duals (new)
  public async method ReadLineAsync(): Task<string?>;          // null at EOF
  public async method ReadToEndAsync(): Task<string>;
  public async method ReadLinesAsync(): IAsyncEnumerable<string>;   // single-consumption (§29.11), §30.7 shape

  // _stream is a `private owned Stream` field → Dispose() is compiler-generated (§28.5):
  //   closes the wrapped stream (§28.4 cascade). No destructor needed.
end StreamReader;

// ── the concrete TextWriter (§7.4 declared it abstract with no subclass) ──
public class StreamWriter: TextWriter, IAsyncDisposable
  public constructor(owned stream: Stream, encoding: Encoding := Encoding.UTF8);
  public constructor(path: string, encoding: Encoding := Encoding.UTF8);   // opens+owns a FileStream (Create)

  public readonly property CurrentEncoding: Encoding;
  public property AutoFlush: bool;                             // default false; true ⇒ flush after each write

  // sync (TextWriter contract, §7.4): Write(string)/Write(char)/WriteLine()/WriteLine(string),
  //   the primitive Write/WriteLine overloads (§2.1 note), Flush() — inherited/overridden.

  // async duals (new)
  public async method WriteAsync(value: string): Task;
  public async method WriteLineAsync(value: string): Task;
  public async method WriteLineAsync(): Task;
  public async method FlushAsync(): Task;

  method DisposeAsync(): Task;                                 // §28.12: awaited FlushAsync, then close
  destructor;                                                 // best-effort sync flush, then §28.4 cascade closes the stream
end StreamWriter;

// ── the in-memory Stream ──
public class MemoryStream: Stream
  public constructor();                                       // empty, growable
  public constructor(capacity: int);                          // Length 0, capacity reserved
  public static method FromBytes(source: Array<byte>): owned MemoryStream;             // copies
  public static method FromBytes(owned buffer: OwningBuffer<byte>): owned MemoryStream; // takes, zero-copy

  // Stream surface (§7.1): CanRead/CanWrite/CanSeek = true; Read/Write/ReadAsync/WriteAsync;
  //   Seek/Length/Position; Flush is a no-op; ReadAll/ReadAllAsync inherited.
  public method ToArray(): owned Array<byte>;                 // fresh copy of [0, Length); re-callable
  public method TakeBytes(): owned OwningBuffer<byte>;        // drains contents; stream resets to empty

  // memory-only backing → Dispose() releases the internal OwningBuffer (§4.14); no native handle.
end MemoryStream;

// ── new method on the existing abstract Stream (§7.1) ──
// public async method CopyToAsync(destination: Stream,
//     bufferSize: int := 81920,
//     token: CancellationToken := CancellationToken.None): Task;   // body in §3, Q4
```

Notes on the surface:
- `EnumerateLines()`/`ReadLinesAsync()` are the **only** additions beyond the .NET-familiar set, and both are single-consumption streaming sources (§29.11). Their signatures are the spec's own §30.7 async-iterator shape (borrowing `string`).
- `AutoFlush` is included because a line-oriented log writer wants each line durable; default `false` keeps the buffered fast path. It is the one convenience property; everything else is the minimal reader/writer contract.
- `MemoryStream.Flush()` is a defined **no-op** (there is nothing beneath it) — it satisfies the abstract `Stream.Flush()` requirement (§7.1) without doing work.

## 5. Deferred (v2+, in rough priority order)

- **Borrowed-stream (`leaveOpen`) ctors** — direction resolved as the **scoped-construction rule** (Q1, §6; RFC [#1439](https://github.com/s8lap/bezel-lang/issues/1439)); lands as a `constructor(scoped stream: Stream)` second signature once the §28.14 spec amendment lands. The single most-requested follow-up (it unblocks StreamWriter-over-a-MemoryStream-you-then-read).
- **General `Stream.Chunks` / `ReadChunksAsync`** — the owning chunk-iterator shape; unify with 0001's `HttpBodyStream.Chunks` (§3, Q4) rather than shipping two.
- **Synchronous `Stream.CopyTo`** — mechanical dual of `CopyToAsync`; add when a non-domain consumer appears.
- **BOM handling** — `detectEncoding`-style sniffing on the reader and optional BOM emission on the writer; deferred so v1 stays faithful to §3.2's no-BOM `Encoding` contract (Q6).
- **Async on the abstract `TextReader`/`TextWriter` bases** — hoisting `ReadLineAsync`/`WriteAsync` up so *any* text reader/writer (e.g. `Console.In`) is async-capable; a wider change than this item's scope.
- **`StreamReader.Peek()` / `ReadBlock` / `ReadAsync(Span<char>)`** — the finer-grained `TextReader` surface; add on demand.
- **`MemoryStream` capacity introspection / `GetBuffer` borrowing view** — deferred with the growth-invalidation hazard (Q3).
- **`BufferedStream`** — a general buffering wrapper over any `Stream`; a fourth type, out of this item's scope.

## 6. Open questions for the owner

1. **Borrowed-stream construction (the load-bearing one). — RESOLVED (owner-accepted direction, 2026-07-04; RFC [#1439](https://github.com/s8lap/bezel-lang/issues/1439)).** Q1 ships owned-only for v1; the borrowed form is unblocked by the **scoped-construction rule** — generalize §28.14's `scoped` from function-typed parameters to class/interface-typed borrow parameters: `constructor(scoped stream: Stream)`. Admission is **§28.14 form 1 verbatim** (bounding object = the constructed reader in a `using var`; the stream source declared *before* it and frozen until the reader's scope exits; reverse-order disposal, §28.10). The reader stores the borrow in an ordinary non-owned back-reference field; **quiescence does not arise** — a stored stream borrow is passive data that never fires, so unlike §28.14's callback case the guarantee is *fully static* with no residual trust (the §28.2 checked-disposal backstop stays only as defense-in-depth). There is **zero new syntax** — grammar A.6 already admits `scoped` on any parameter; one semantic check relaxes (`checkScopedIsFunctionTyped`), and a fresh producer (`new StreamReader(scoped new FileStream(...))`) is rejected exactly like §9.7's temporary rule (the existing declared-before-L check already fires structurally). Options (a) a general borrow-bearing-*class* concept and (c) permanent deferral were declined — (a) is far larger than the problem and reopens the owned-field-extraction hazard; (c) forecloses the single most-requested follow-up. The v1 owned ctor stays unchanged; the `scoped`-borrow ctor lands later as a pure, non-breaking **second signature** (resolved by the `owned` vs `scoped` call-site marker) once the §28.14 RFC amendment lands. This is a **spec amendment** — not implemented autonomously per CLAUDE.md.

   *Research note carried into the RFC:* an earlier draft's claim that "no existing rule bounds the reader" was too strong — a plain borrow ctor is already §28.3 residual-trust admissible via the back-reference rule; `scoped` *upgrades* that trust to a static proof rather than creating a bound from nothing.
2. **`ReadLinesAsync` borrow-of-receiver across `await`.** As an instance async-iterator, the enumerator holds a borrow of `this` (the reader) across suspension points; the `await for` desugaring scopes the enumerator inside the reader's `using var` (§30.7), which *should* bound it soundly — but instance async-iterators borrowing the receiver across `await` are not exercised elsewhere in the stdlib. Confirm the desugaring bounds it, or should the line iterator be a free function that *owns* the reader (as §30.7's `ReadLinesAsync(path)` owns its `fs`)?
3. **`CopyToAsync` on the abstract `Stream`.** It is a non-abstract method with a default body, so it does not break existing subclasses — but it is still an addition to a normative stdlib type. Confirm that lands as a §7.1 amendment (proposal-graduation workflow) rather than needing its own decision.
4. **`AutoFlush` default.** Proposed `false` (buffered fast path). .NET's `StreamWriter` also defaults `false` but its `Console`-derived writers default `true`. Confirm `false`, or want line-durability by default?
5. **`TakeBytes` return type.** `owned OwningBuffer<byte>` (the §4.14 primitive, matching the review sketch) vs `owned Array<byte>` (the everyday byte container, matching `ToArray`). The buffer is the zero-copy-honest type; the array is the more familiar one. The review says `OwningBuffer<byte>`; confirm.

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

The five §6 questions are resolved as follows; the graduating additions (`Stream.CopyToAsync`, the two concrete classes, `MemoryStream`) land as §7 amendments through the normal docs workflow.

- **Q1 — borrowed-stream construction: already owner-resolved (2026-07-04, RFC #1439).** v1 ships owned-only; the borrowed form is unblocked by the scoped-construction rule (generalize §28.14 `scoped` to class/interface-typed borrow parameters) and lands later as a pure non-breaking second `constructor(scoped stream: Stream)` signature once the §28.14 amendment lands. Recorded in §6; no further action here.
- **Q2 — `ReadLinesAsync` borrow-of-receiver across `await`: confirmed sound; keep the instance async-iterator.** v0.179 settled exactly this case: §28.3's "Deferred-activation callees" rule states that an iterator/async-iterator factory's returned frame **carries its arguments' (and receiver's) borrows** and is treated as a borrow-carrying value whose source is the receiver, and §29.3 restates it for the returned state machine. The `await for` desugaring binds the enumerator inside the reader's `using var` (§30.7), so the reader (the borrow source) provably outlives the enumeration and the borrow is **compiler-tracked**, not residual trust. No fallback to a receiver-owning free function is needed.
- **Q3 — `CopyToAsync` on abstract `Stream`: confirmed as a §7.1 amendment through normal graduation.** It is a non-abstract method with a default body over the existing `ReadAsync`/`WriteAsync`, so it breaks no existing subclass; it needs no decision of its own beyond landing with this proposal.
- **Q4 — `AutoFlush` default `false`.** Confirmed. The buffered fast path is the right default; line-durability is opt-in (`AutoFlush := true`), matching .NET's `StreamWriter` default.
- **Q5 — `TakeBytes(): owned OwningBuffer<byte>`.** Confirmed. `OwningBuffer<byte>` (§4.14) is the zero-copy-honest drain type; `ToArray()` remains the everyday owned-`Array<byte>` copy.

## 7. Relationship to existing issues

- **Gap review item #1 / [#1403](https://github.com/s8lap/bezel-lang/issues/1403)** — this is the review's top item and wave-D1 root; [#1433](https://github.com/s8lap/bezel-lang/issues/1433) tracks the proposal.
- **[#1376](https://github.com/s8lap/bezel-lang/issues/1376) (proposal 0001)** — 0001's `NetworkStream`/`TlsClientStream`/`HttpBodyStream` all extend this family's `Stream`; the deferred general `Stream.Chunks` (§5) unifies with 0001 §3.3's `HttpBodyStream.Chunks`, and `CopyToAsync` is the primitive 0001's `Download` example wants.
- **Gap item #5 (Process stdio)** and **#22 (async `File` family)** — both are named as *depending on this item*; `StreamReader`/`StreamWriter` are the stdio wrappers, and `ReadAllLinesAsync` etc. are mechanical over these types.
- **#645, #1394** — the pre-existing stream-family requests this proposal answers.
- **The `Stream.Read` `Count`-vs-`Capacity` contract (§7.1, v0.165 #1375)** — already resolved in the stdlib; `CopyToAsync`'s reference body uses the sized `new Array<byte>(bufferSize, 0)` ctor and `buffer.Count` exactly as that note prescribes, so it inherits the fixed contract with nothing new to decide.
- **#1358 (byte-worlds bridge)** — `MemoryStream.FromBytes`/`ToArray` sit on the `Array<byte>` side; `FromBytes(owned OwningBuffer<byte>)`/`TakeBytes` on the buffer side. The two overloads bridge the worlds explicitly (copy vs take) rather than waiting on #1358.
