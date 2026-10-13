# Proposal 0069 — compression: DEFLATE and gzip (streams + one-shot byte forms)

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | — (none yet) |
| **Target module(s)** | `System.IO.Compression` (new module; requires `import System.IO.Compression`) |
| **Written against** | spec/stdlib **v0.227** |
| **Origin** | Adversarial stdlib review pass, 2026-07 (confirmed gap). **Not** among the 31 ranked gaps of the [2026-07 gap review](papers/stdlib-gap-review-2026-07.md) — this proposal extends that map: no stdlib section covers compression, no §11 deferred-module line names it, and no proposal 0001–0067 designs it. The nearest recorded trace is proposal [0002](0002-http-server.md)'s deferred list, which names "compression" with no design behind it |
| **Design influences** | .NET `System.IO.Compression` (`GZipStream`/`DeflateStream`) for the wrapper concept — **not** its mode-enum constructor; proposal [0006](0006-stream-family.md) for the owned-wrapping stream discipline this proposal anchors on; zlib's deflate/inflate API as the streaming substrate model |
| **Currency note** | Refreshed 2026-08-11 to v0.246: caller-side transfer marker respelled `move` (`Decompress(move fs)`); declaration-side `owned` unchanged. Written-against pin unchanged. **2026-09-17 sweep — refreshed to v0.306:** one use-site respell in §9's worked example (*Usage examples*), `out owned var data` → `out var data` (the `out owned var` use-site spelling was retired at v0.289; the inline declaration adopts the callee's announced classification, spec §28.3 / Appendix A.4). The `out owned result: Array<byte>?` **declaration** slots in the proposed signatures are unchanged. No tier-2 flag. Written-against pin unchanged. |

Every type and method this proposal builds on is cited to `Bezel_Standard_Library.md` or the language spec; everything new is **proposed** and appears in §5. Surface that exists only in another *proposal* (0006's `CopyToAsync`/`MemoryStream`, 0009's encodings) is cited as proposal surface, never as current stdlib.

---

## 1. Problem and motivation

No compression exists anywhere in the platform's design space: stdlib §7 (`System.IO`) is codec-free, stdlib [§11](../Bezel_Standard_Library.md#11-modules-deferred-to-future-revisions)'s deferred-module list does not even name the domain, and the proposal series through 0067 never designs it. The confirmed workloads:

1. **HTTP content-encoding** — gzip is the baseline `Content-Encoding`/`Accept-Encoding` codec of the web. Proposal 0001's client will need response decompression and 0002's server response compression; 0002 explicitly defers "compression" in its follow-on list with nothing to defer *to*. This proposal supplies the primitive; whether 0001/0002 apply it transparently or expose it is **their** design decision, not made here.
2. **Log and artifact handling** — services read and write rotated `.gz` logs (multi-member concatenated gzip is the standard rotation format — §4 Q5 handles it deliberately), compressed request/audit archives, and CI artifact bundles.
3. **Build tooling** — proposal [0040](0040-build-system.md)'s cache and artifact story is the kind of consumer that compresses stored blobs; content-addressed stores conventionally compress at rest.
4. **Interop with everything** — `.gz` files, PNG/zlib payloads (raw DEFLATE is the substrate of zip, PNG, and git packfiles), compressed exports from other systems.

Two shapes are needed, matching how the data arrives: **stream wrappers** (compress/decompress *while* bytes flow — the only shape that works for bodies and large files without buffering everything, exactly the argument 0006 §1 makes for the stream family itself) and **one-shot byte-array forms** (payload already in memory; one call).

---

## 2. What exists today (the substrate this composes over)

- **`Stream`** (stdlib [§7.1](../Bezel_Standard_Library.md#sec-stream)) — the abstract base the wrappers extend: windowed `Read`/`Write` + span sync overloads, `virtual` `ReadAsync`/`WriteAsync` with the v0.184 override-point rule ("a stream performing real I/O overrides them"), `Flush`, `Seek`/`Length`/`Position`, `ReadAll`/`ReadAllAsync`, and the v0.195 read/write contracts (`0` iff end; short reads legal; writes complete or raise).
- **`FileStream`** ([§7.2](../Bezel_Standard_Library.md#sec-filestream)), the IO exception family ([§7.5](../Bezel_Standard_Library.md#sec-io-exception-types)), `CancellationToken` (§6.1), `Array<byte>` with the sized constructor (§4.4), `IReadOnlySpan<byte>` (§4.16), `IDisposable`/`IAsyncDisposable` (§2.10; spec [§28.12](../Bezel_Language_Spec.md#sec-asynchronous-disposal)).
- **The ownership machinery the wrapping rides on**: owned fields and the disposal cascade (spec [§28.4](../Bezel_Language_Spec.md#sec-the-destructor)/[§28.5](../Bezel_Language_Spec.md#sec-owned-fields) — destructor body first, then owned-field disposal), call-site transfer markers (§22.3), detaching via a nullable owned field (spec [§28.8](../Bezel_Language_Spec.md#sec-ownership-transfer)).
- **Proposal 0006** (accepted) — the owned-wrapping stream family this proposal anchors on: `StreamReader`/`StreamWriter` take their stream `owned` (0006 Q1), `StreamWriter` establishes flush-on-dispose + `IAsyncDisposable` for the fallible-dispose case (0006 Q7), `MemoryStream` and `Stream.CopyToAsync` are the composition partners. Cited throughout as proposal surface.
- **Proposal 0011** (accepted) — the `Take*(): owned Stream` hand-off naming precedent (`TakeStandardOutput`), reused by Q4.
- **Proposal 0007** (accepted) — the owner ruling this proposal reuses directly: non-seekable streams raise **`InvalidOperationException`** from `Seek`/`Length`/`Position` (`NotSupportedException` was declined to keep the §31.4 taxonomy small); and the runtime-internal narrow-seam discipline (`bz_tls_*`) that §10's substrate recommendation mirrors.

---

## 3. Format scope

**v1 ships two formats:**

- **gzip (RFC 1952)** — the member format: header + raw-DEFLATE body + CRC-32/ISIZE trailer. The dominant consumer format (HTTP `gzip`, `.gz` files, rotated logs). Integrity-checked by construction (trailer verified — §4 Q5).
- **raw DEFLATE (RFC 1951)** — the bare bitstream, no header, no checksum. The substrate format (zip entries, PNG, git); also what .NET's `DeflateStream` means by "deflate." Shipped because it is the reusable core and costs nothing beyond the gzip work. **Stated plainly: raw DEFLATE carries no integrity check** — corruption that still parses yields wrong bytes without a fault; consumers wanting integrity use gzip or wrap with 0029's checksums.

**Deferred, recorded:**

- **zlib (RFC 1950)** — the two-byte-header + Adler-32 wrapper. Deferred as a mechanical additive (`ZlibStream`/`Zlib`) until proposal 0001 decides its HTTP `Content-Encoding: deflate` handling — that token *nominally* means RFC 1950 but real servers send RFC 1951, and the tolerant-reader policy belongs to the HTTP proposal, not here. Designing the type before its one consumer states its need would guess.
- **zstd** — staged, not declined. Materially better ratio/speed than DEFLATE and system-packaged (`libzstd`), but it is a **new dependency** (a project-level decision per CLAUDE.md, distinct from the zlib question in §10) for a format none of the named v1 workloads require — HTTP baseline, `.gz` logs, and zip/PNG interop are all DEFLATE-family. **Adoption criteria recorded:** a named in-tree consumer (e.g. 0040 artifact storage choosing it, or HTTP `zstd` encoding demand) plus an owner dependency ruling; the §5 surface shape (paired stream factories + one-shot statics) transfers unchanged, so staging costs nothing now.
- **Brotli** — deferred behind the same gate as zstd; HTTP-facing only, rides 0001/0002 demand.
- **Archive formats (zip, tar)** — **out of scope**, a different problem (multi-entry containers, paths, the zip-slip path-traversal hazard) needing its own proposal over §7.4 `Path`/`Directory`; this module is codecs only. Recorded so the future archive item builds on `DeflateStream` rather than re-opening codec design.

---

## 4. Design decisions

### Q1 — Wrapper ownership: the wrapped stream is taken `owned` (0006 Q1, adopted verbatim)

`GzipStream`/`DeflateStream` take the wrapped stream **owned** and store it in an owned field; the wrapper is the stream's sole owner, and disposing the wrapper disposes the stream through the spec [§28.4](../Bezel_Language_Spec.md#sec-the-destructor)/[§28.5](../Bezel_Language_Spec.md#sec-owned-fields) cascade. This is 0006 Q1's ruling applied to the next wrapper in the family, with the same §22.3 call-site ergonomics falling out:

- The canonical wrap passes a **fresh producer** and needs no marker: `GzipStream.Decompress(new FileStream(path, FileMode.Open))`.
- A caller holding a **named owned binding** writes the transfer marker and loses the binding: `GzipStream.Decompress(move fs)` — `fs` is moved-from; the caller must not also dispose it.
- One `using var`/`await using var` on the wrapper closes wrapper state *and* the underlying stream — the whole point of the owned-wrapping convention (0006 Q1: "disposal chains deterministically with zero ceremony").

**Borrowed-inner (`leaveOpen`) construction is deferred to the same spec ruling 0006 deferred it to** — the §28.14 scoped-construction generalization (owner-accepted direction, RFC #1439). When that amendment lands, a `scoped`-parameter form arrives as a non-breaking addition here exactly as 0006 §5 plans for `StreamReader`; v1 ships owned-only for the same soundness reasons 0006 Q1 records (a silently-trusted borrow field is the §28 footgun). The in-memory capture case the borrow form would serve is covered today by Q4's `TakeStream` and by the one-shot forms (Q6).

### Q2 — Direction via named factories, not a mode-enum constructor

.NET spells direction as `new GZipStream(stream, CompressionMode.Compress)`. This proposal uses **named static factories** instead:

```bezel
public static method Compress(owned destination: Stream,
    level: CompressionLevel := CompressionLevel.Balanced): owned GzipStream;
public static method Decompress(owned source: Stream): owned GzipStream;
```

Reasons: (a) direction is not inferable from the types — both directions wrap a `Stream` — so *something* must carry it, and a factory name reads at the call site where a mode enum hides in an argument; (b) the invalid combination (.NET's `CompressionLevel` handed to a `Decompress`-mode stream, a runtime `ArgumentException` there) is **unrepresentable** here — the `Decompress` factory has no level parameter; (c) the factories mark their returns `owned`, the explicit factory-ownership discipline graduated from proposal 0048 (spec: unmarked static class returns are declaration-site errors). The parameter names state the role: the compressor wraps its **destination** (plaintext in via `Write`, compressed bytes pushed to the inner stream), the decompressor wraps its **source** (compressed bytes pulled from the inner stream, plaintext out via `Read`). Constructors are not public surface; the factories are the only entry points.

A compress-direction stream is write-only (`CanWrite = true`, `CanRead = false`); a decompress-direction stream is read-only. Neither is seekable; `Seek`, `Length`, and `Position` raise `InvalidOperationException` with a reason-naming message, **reusing 0007's owner ruling** (2026-07-04: keep `InvalidOperationException`; `NotSupportedException` declined). Wrong-direction `Read`/`Write` raise `InvalidOperationException` under the same ruling's keep-the-taxonomy-small posture.

### Q3 — Disposal: finish-on-dispose, the §28.4 ordering, and the `IAsyncDisposable` edge (0006 Q7, adopted)

A DEFLATE stream is not merely buffered — it is **unterminated** until the final block is emitted (plus, for gzip, the CRC-32/ISIZE trailer). So the compress-direction wrapper has exactly `StreamWriter`'s shape from 0006 Q7, with "flush" strengthened to "finish":

- **`destructor`**: best-effort synchronous finish — emit the final block and trailer to the still-open inner stream, then release the native codec state (§10). The ordering is load-bearing and spec-guaranteed: §28.4 runs the destructor body **first**, then disposes owned fields — so the trailer reaches the inner stream before the cascade closes it.
- **`IAsyncDisposable`**: `DisposeAsync` performs `await FinishAsync()` and then disposes the inner stream, so a dispose-time write failure (disk full, broken pipe) **surfaces on the `await using` edge** where the caller can catch it — 0006 Q7's reasoning verbatim ("a destructor is expected not to throw (§28.4)… the whole reason `IAsyncDisposable` exists"). The documented recommendation is likewise verbatim: **`await using var` for any archive you must not lose**, or an explicit `await gz.FinishAsync()` before scope exit to get the error at a named point.
- The decompress-direction wrapper has no finish obligation: its destructor releases the native codec state and the cascade closes the inner stream. Discarding a partially-read decompressor is legal and loses nothing.
- **Double-dispose is a no-op** (§28.2); `Finish` is idempotent; `Dispose` after `Finish` only closes the inner stream.

### Q4 — `Finish` and `TakeStream`: ending the member without ending the wrapper

`Flush` cannot end the member: the `Stream.Flush` contract is "everything written so far is pushed down," and callers flush mid-stream (a log writer making the current records decodable). DEFLATE distinguishes exactly these two operations, and the surface mirrors it:

- **`Flush()`** (compress direction): emits a deflate **sync point** — after it returns, every byte previously written is represented in the compressed bytes handed to the inner stream and decodable by a reader, and the inner stream's own `Flush()` has run. The member remains open; more writes may follow. (Decompress direction: no-op, like `MemoryStream.Flush`, 0006 §4.)
- **`Finish()` / `FinishAsync(token)`** (compress direction): emits the final block and, for gzip, the CRC-32/ISIZE trailer, and flushes the inner stream. Idempotent. After `Finish`, `Write` raises `InvalidOperationException`. `Dispose`/`DisposeAsync` call it implicitly (Q3); calling it explicitly is how synchronous code observes the completion error.
- **`TakeStream(): owned Stream`** — the detach: in the compress direction, finishes the member (if not already finished), then transfers the inner stream out to the caller and leaves the wrapper **detached** (all subsequent I/O members raise `InvalidOperationException`; disposal releases codec state only). In the decompress direction it simply detaches and hands the source back (positioned wherever the codec's read-ahead left it — stated honestly; byte-exact repositioning of an unread tail is not promised in v1). The name follows the established `Take*` = ownership-transfer-out convention (0006 `MemoryStream.TakeBytes`, 0011 `TakeStandardOutput`); the mechanics are the spec's **detach-via-nullable-owned-field** pattern (§28.8.1, *Detaching via a nullable field*, with its K9.2 field-restoration rule): the field is `owned Stream?`, `TakeStream` moves it out and leaves `null`. A second `TakeStream` raises `InvalidOperationException` — the wrapper is already detached.

`TakeStream` is what covers "compress into a `MemoryStream`, then read the archive back" under owned-only wrapping — the same gap 0006 Q1 hit for `StreamWriter` (there answered by `CopyToAsync`; here the trailer makes a first-class finish-and-detach the honest answer). When the #1439 `scoped` ruling lands, the borrowed-inner form subsumes most `TakeStream` uses; the member stays (detach and borrow are different contracts). Flagged as open question Q3 for the owner.

### Q5 — The decompression read contract: multi-member gzip, truncation, and strictness

- **Read semantics** (both formats): the §7.1 v0.195 contract — returns `1..count` while plaintext remains (short reads legal), `0` **iff** the compressed input is fully and *validly* consumed. `ReadAll`/`ReadAllAsync` are inherited from §7.1 and behave accordingly.
- **Multi-member gzip:** a gzip source may be a concatenation of members (the standard log-rotation and `gzip -c a b > out.gz` format; RFC 1952 §2.2 blesses it). `GzipStream.Decompress` decodes **all** members in sequence — end-of-stream is reported only at the inner stream's end — matching `gzip(1)` and modern .NET. Stopping at the first member would silently truncate rotated logs, a data-loss default.
- **Strict faults, per the defined-fault house model** (§3.2 Encoding's "silent tolerance hides data problems", 0009 D3's strict decode): a malformed bitstream, a gzip header that is not a gzip header, a **CRC-32 or ISIZE trailer mismatch**, **truncated input** (inner EOF mid-member), or **trailing garbage** (bytes after a member that do not begin a valid new member) all raise `InvalidDataException` (§7). Truncation is deliberately *not* `EndOfStreamException` — §7.5 reserves that type for exact-read consumers reporting "the stream ended before my fixed-size read"; here the condition is "the *format* is incomplete," which is corruption, and the caller's remedy (reject the input) is the same as for any other corruption. One fault type for "these bytes are not a valid gzip/DEFLATE stream."
- Faults are detected as decoding reaches them — a trailer mismatch surfaces on the `Read` that consumes the member boundary, not retroactively.

### Q6 — One-shot byte forms, and the required decompression cap

```bezel
public static class Gzip
  public static method Compress(bytes: IReadOnlySpan<byte>,
      level: CompressionLevel := CompressionLevel.Balanced): owned Array<byte>;
  public static method Decompress(bytes: IReadOnlySpan<byte>,
      maxOutputBytes: long): owned Array<byte>;
  public static method TryDecompress(bytes: IReadOnlySpan<byte>, maxOutputBytes: long,
      out owned result: Array<byte>?): bool;
end Gzip;
// Deflate: the same three signatures over raw RFC 1951.
```

Input/output conventions are 0009 D5's, adopted verbatim: `IReadOnlySpan<byte>` in (one overload accepts every byte source, §4.16), `owned Array<byte>` out (the standard fresh-byte-array producer shape, §3.2/§7.3). `Compress` never faults on any input, including empty.

**`maxOutputBytes` is a required parameter — no default.** This is the deliberate, load-bearing choice of the proposal. A DEFLATE bomb expands by up to ~1000:1 (a few MB of input to gigabytes), and in Bezel an allocation the process cannot satisfy is **termination, not a catchable exception** (spec [§31.4](../Bezel_Language_Spec.md#sec-standard-exception-types), *Allocation failure*: "the program terminates… nothing is catchable"). An unbounded one-shot decompress of untrusted bytes is therefore a remote **process-kill** vector, not merely an OOM hazard as in GC'd platforms. Any *default* cap is a silent policy (too low breaks trusted callers invisibly; too high protects no one), so the cap is simply required: callers state their bound, and a caller who genuinely wants no bound writes `long.MaxValue` — a visible, greppable announcement in the spirit of the language's other loud markers (`owned`, `move`). Exceeding the cap raises `DecompressionLimitException` (§7), a **distinct** type from `InvalidDataException` because the caller's branch differs (raise the limit vs. reject the input) — the typed-fault discipline of 0007 §7/Guide 2 §5. Negative caps raise `ArgumentOutOfRangeException` (the method-parameter precedents: `Random.Next(maxValue)` §2.7, `String.Repeat(count)`/`PadLeft(totalWidth)` §9.6); `0` is valid (only an empty expansion fits).

**`TryDecompress`** is Try-convention shape (a) (stdlib §1; `bool` + nullable `out owned`, the `when true` postcondition applying as everywhere): returns `false` — assigning `null` — on *any* data failure, corrupt or over-cap; callers needing to distinguish use the throwing form. Provided because corrupt input is an **expected** outcome for the untrusted-payload workload, exactly 0009's reason for `TryDecode`. The cap parameter precedes the `out` so all three arguments pass positionally (out arguments are positional-only, spec §22.5).

**The streaming forms carry no cap** — deliberately. A stream consumer controls consumption bytes-at-a-time; the unbounded sink, if any, is the caller's own visible loop (e.g. `CopyToAsync` into a `MemoryStream`), which is where the responsibility legibly sits. The one-shot is where the *library* allocates the whole output, so the one-shot is where the library demands the bound. (Inherited `ReadAll` on a decompress stream is that same caller-written unbounded sink, one call shorter; its §7.1 contract is unchanged here, and the module docs point untrusted-input callers at the capped one-shot or a bounded read loop.)

### Q7 — Async: genuine overrides, and the CPU-work caveat stated honestly

`GzipStream`/`DeflateStream` **override** `ReadAsync`/`WriteAsync` with genuinely asynchronous implementations that `await` the inner stream's async operations — required by §7.1's v0.184 override-point rule, since the inner stream (file, socket) performs real I/O and the blocking base default would pin a pool worker (§30.12 hazard). `FinishAsync` is the async dual of `Finish` per the stdlib §1 `Async`-suffix convention; `DisposeAsync` rides it (Q3).

Stated honestly: the codec's own work is **CPU-bound and runs synchronously on the calling domain** between inner-stream awaits — an `await gz.WriteAsync(...)` yields for I/O, not for compression arithmetic. A service compressing large payloads at high throughput occupies its domain's continuations for the duration; the offload path is the ordinary one (a worker domain via `Domain.Spawn`/`Parallel`, spec §30.14/stdlib §6.12), not hidden threading inside the stream. No `bufferSize` knobs are exposed in v1; internal chunking is an implementation detail (stdlib §1: not normative for internals).

### Q8 — Naming and the level enum

- **`GzipStream`, not .NET's `GZipStream`.** The stdlib §1 acronym rule: Bezel-coined names use PascalCase acronyms (`IeeeEquals` precedent); the verbatim-.NET-spelling carve-out is for surface *inherited* from .NET, and this type deliberately is not (named factories, `Finish`/`TakeStream`, no mode enum). One rule, applied: `Gzip`, `GzipStream`, `Deflate`, `DeflateStream`.
- **`CompressionLevel`** is a three-member enum with Bezel-coined, self-describing names (`Fastest`, `Balanced`, `Smallest`; default `Balanced`) — .NET's `Optimal` (optimal *what?*) is the confusion its own `SmallestSize` addition later admitted. The enum states a **trade, not a contract**: level affects size/speed only; round-trip correctness and format validity are identical at every level; the mapping to underlying codec levels (illustratively 1/6/9) is implementation detail. `NoCompression` (stored blocks) is deferred — niche, additive.

---

## 5. Proposed API surface (all types new, in `System.IO.Compression`)

```bezel
public enum CompressionLevel: byte
  Fastest, Balanced, Smallest;
end CompressionLevel;

// ── the gzip stream wrapper (RFC 1952; multi-member on read, Q5) ──
public class GzipStream: Stream, IAsyncDisposable
  // Factories are the only entry points (Q2). Both take the inner stream OWNED (Q1).
  public static method Compress(owned destination: Stream,
      level: CompressionLevel := CompressionLevel.Balanced): owned GzipStream;
  public static method Decompress(owned source: Stream): owned GzipStream;

  // Stream surface (§7.1): every abstract member supplied; direction-gated —
  //   Compress:  CanWrite = true,  CanRead = false;  Write/WriteAsync accepted,
  //              Read/ReadAsync raise InvalidOperationException.
  //   Decompress: CanRead = true,  CanWrite = false; the reverse gating.
  //   CanSeek = false in both directions; Seek/Length/Position raise
  //   InvalidOperationException (0007 owner ruling — no NotSupportedException).
  //   ReadAsync/WriteAsync are genuine async overrides (Q7).
  //   Flush = deflate sync point, member stays open (Q4); no-op when decompressing.

  public method Finish();                                       // Q4; compress only; idempotent
  public async method FinishAsync(
      token: CancellationToken := CancellationToken.None): Task;
  public method TakeStream(): owned Stream;                     // Q4; finishes (compress), detaches

  method DisposeAsync(): Task;      // §28.12: await FinishAsync, then dispose inner (Q3)
  destructor;                       // best-effort sync finish (compress), release native
                                    //   codec state; §28.4 cascade then disposes the
                                    //   owned inner stream (unless detached by TakeStream)
end GzipStream;

// ── the raw-DEFLATE stream wrapper (RFC 1951; no header, no checksum — §3) ──
public class DeflateStream: Stream, IAsyncDisposable
  public static method Compress(owned destination: Stream,
      level: CompressionLevel := CompressionLevel.Balanced): owned DeflateStream;
  public static method Decompress(owned source: Stream): owned DeflateStream;
  // Same Stream gating, Finish/FinishAsync/TakeStream, DisposeAsync, destructor as GzipStream.
end DeflateStream;

// ── one-shot byte forms (Q6) ──
public static class Gzip
  // Never faults; empty input is valid (produces a valid empty-payload member).
  public static method Compress(bytes: IReadOnlySpan<byte>,
      level: CompressionLevel := CompressionLevel.Balanced): owned Array<byte>;

  // InvalidDataException on malformed/truncated input or trailer mismatch;
  // DecompressionLimitException when the expansion exceeds maxOutputBytes;
  // ArgumentOutOfRangeException when maxOutputBytes < 0.  Decodes ALL members (Q5).
  public static method Decompress(bytes: IReadOnlySpan<byte>,
      maxOutputBytes: long): owned Array<byte>;

  // Try shape (a): false + null on any data failure (corrupt, truncated, over-cap).
  public static method TryDecompress(bytes: IReadOnlySpan<byte>, maxOutputBytes: long,
      out owned result: Array<byte>?): bool;
end Gzip;

public static class Deflate
  public static method Compress(bytes: IReadOnlySpan<byte>,
      level: CompressionLevel := CompressionLevel.Balanced): owned Array<byte>;
  public static method Decompress(bytes: IReadOnlySpan<byte>,
      maxOutputBytes: long): owned Array<byte>;
  public static method TryDecompress(bytes: IReadOnlySpan<byte>, maxOutputBytes: long,
      out owned result: Array<byte>?): bool;
end Deflate;

// ── fault types (§7) ──
public class InvalidDataException: IOException
  public constructor(message: string);
end InvalidDataException;

public class DecompressionLimitException: IOException
  public constructor(message: string);
  public readonly property MaxOutputBytes: long;   // the cap that was exceeded
end DecompressionLimitException;
```

---

## 6. Ownership and sendability

The task of this section is precision; every decision, stated:

- **Who owns the wrapped stream: the wrapper, always (v1).** The factories take `owned source`/`owned destination` and store it in a `private owned _stream: Stream?` field (nullable solely for Q4's detach). The §22.3 marker rules at the call site are exactly 0006 Q1's: fresh producers pass unmarked; a named owned binding passes as `owned fs` and is moved-from afterward — the caller holds no residual disposal obligation and must not touch the inner stream again while the wrapper lives.
- **What `Dispose` does**, by direction:
  - *Compress:* §28.4 order — destructor body runs first (best-effort finish: final block + gzip trailer written to the **still-open** inner stream; native codec state released), then the owned-field cascade disposes the inner stream (which, per its own contract, flushes and closes — §7.2). Synchronous dispose cannot report a finish failure ("disposers should not throw," §28.4); `DisposeAsync` (§28.12) is the path that surfaces it, hence the `await using var` recommendation for must-not-lose archives (Q3).
  - *Decompress:* destructor releases native codec state; cascade disposes the inner stream. Nothing is lost by discarding early.
  - After `TakeStream`, the owned field is `null` (spec §28.8.1 detach pattern); disposal releases codec state only — the caller now owns the inner stream.
  - Double-dispose is a no-op (§28.2).
- **The 0006 precedent, named:** this is the third application of the owned-wrapping convention (`StreamReader`/`StreamWriter` → 0011's `Process` stdio → here), and the second application of the fallible-dispose `IAsyncDisposable` pattern (`StreamWriter.DisposeAsync` → `GzipStream.DisposeAsync`). No new ownership mechanism is introduced anywhere in this proposal; Q4's detach uses the spec's existing nullable-owned-field rule.
- **Sendability (spec [§30.14](../Bezel_Language_Spec.md#sec-sendable-types)): `GzipStream`/`DeflateStream` are NOT sendable — domain-local**, twice over: they are unsealed classes (rule 1 excludes open classes), and their owned field's declared type is the open abstract `Stream` (rule 2 requires owned fields of *sendable class* type). This matches the entire stream family's posture (0007 records `NetworkStream`/`TlsClientStream` as domain-local; `FileStream` is likewise unsendable). A pipeline that spans domains moves **bytes** (sendable `Array<byte>` — §30.14's borrowing-collection rule) over channels, never the stream objects.
- The one-shot static classes have no instances; `CompressionLevel` is an enum (sendable, trivially). The exception types follow the §31 exception-ownership model.
- **Not intercept points** (stdlib §1, v0.204): everything here is pure computation over the inner stream's bytes; the world-reads are the inner `FileStream`/socket's own, already declared at their §7/0007 homes.

---

## 7. Fault taxonomy

Rooted under `IOException` (stdlib §7.5) so `catch IOException` stays the honest "any I/O failed" umbrella (0007 §7's discipline); both new types live in `System.IO.Compression` behind the module's import, per spec §31.4's producer-locality principle.

```
Exception (spec §31.4)
└─ IOException (stdlib §7.5)
   ├─ InvalidDataException            — the bytes are not a valid gzip/DEFLATE stream:
   │                                    malformed bitstream, bad gzip header, CRC-32 or
   │                                    ISIZE trailer mismatch, truncated input (inner EOF
   │                                    mid-member), trailing garbage after a member (Q5)
   └─ DecompressionLimitException     — one-shot expansion exceeded maxOutputBytes (Q6);
                                        distinct type: the caller's branch differs
                                        (raise the cap vs. reject the input)

  reused as-is:
   InvalidOperationException (spec §31.4) — wrong-direction Read/Write; Seek/Length/Position
                                            (non-seekable, 0007 owner ruling); Write after
                                            Finish; any I/O member after TakeStream
   ArgumentOutOfRangeException (stdlib §2.5 home) — maxOutputBytes < 0 (§1 convention)
   OperationCanceledException (spec §31.4) — token fired during an async form (§30.8)
```

Never faulting: `Compress` (both shapes, any input including empty); `Flush`; `Finish` on an already-finished stream (idempotent); `Dispose` (best-effort by contract). `EndOfStreamException` is deliberately **not** produced (Q5 records the reasoning against §7.5's reservation). Propagation: inner-stream `IOException`s pass through unwrapped — they are what they are; the two new types cover only what the *codec* detects.

---

## 8. Determinism and the testing strategy

**Round-trip identity is the normative contract; byte-exact compressed output is not.** Compressed bytes may differ across implementations and substrate versions at the same level (DEFLATE admits many encodings of the same data), so the proposal pins:

1. `Decompress(Compress(x, anyLevel)) = x` for all `x` — the round-trip property, at every level, both formats, both shapes (and mixed: one-shot-compressed input through the stream decompressor and vice versa). The natural 0053 property-test.
2. **The gzip header is environment-independent**: no mtime (MTIME = 0), no filename, no comment, no extra fields — so a given implementation's output is deterministic for (input, level), reproducible-build-friendly (the 0040 cache affinity), and leaks nothing about the producing host. On read, header metadata is ignored and not surfaced (a `.gz` metadata API is deferred; §11).
3. **Decompression is exact**: fixed compressed fixtures (generated by reference tools — `gzip(1)`, zlib — and checked into the test tree, never hand-fabricated in this document) must decode to their known plaintexts; corrupted/truncated/bad-trailer/garbage-tail variants of those fixtures must raise `InvalidDataException` at the Q5-specified points; a multi-member fixture must yield the concatenated plaintexts; cap fixtures must raise `DecompressionLimitException` without materializing the over-cap output.
4. The trailer check really checks: a fixture with a flipped CRC byte faults even though its bitstream inflates cleanly.

(No known-answer table of compressed bytes appears here, deliberately — the honest vectors are external fixtures, per point 3; contrast 0068 §8, where the algorithms *are* deterministic functions and the KAT tables are normative.)

---

## 9. Usage examples

```bezel
import System.IO;                  // FileStream, FileMode (§7)
import System.IO.Compression;

// 1. Decompress a rotated .gz log to a file — stream to stream, one owner each.
//    CopyToAsync is proposal-0006 surface (accepted), cited as such.
public async method UnpackLogAsync(src: string, dst: string): Task
  using var gz := GzipStream.Decompress(new FileStream(src, FileMode.Open));
  using var dest := new FileStream(dst, FileMode.Create);
  await gz.CopyToAsync(dest);                       // all members, Q5
end UnpackLogAsync;

// 2. Compress a stream into a .gz file. `await using` surfaces the trailer-write
//    error on the dispose edge (Q3); the wrapper owns the FileStream it wraps.
public async method PackAsync(source: Stream, dst: string): Task
  await using var gz := GzipStream.Compress(new FileStream(dst, FileMode.Create));
  await source.CopyToAsync(gz);
end PackAsync;

// 3. One-shot: compress an in-memory payload (e.g. an HTTP response body).
var compressed := Gzip.Compress(payload.AsReadOnlySpan());        // payload: Array<byte>

// 4. One-shot, untrusted input: the cap is mandatory and the Try form is total.
if Gzip.TryDecompress(upload.AsReadOnlySpan(), 8 * 1024 * 1024, out var data) then
  Process(data);        // narrowed non-null past the check (§22.4 when-true postcondition)
end;

// 5. Trusted input, explicit no-bound announcement:
var config := Gzip.Decompress(blob.AsReadOnlySpan(), long.MaxValue);

// 6. Compress into memory and take the archive bytes back (Q4's detach):
//    MemoryStream is proposal-0006 surface (accepted); ReadAll is stdlib §7.1.
using var gz2 := GzipStream.Compress(new MemoryStream());
gz2.Write(payload, 0, payload.Count);               // §7.1 windowed write (§4.15: Array's
                                                    //   ArraySpan family is not §7.1's
                                                    //   ReadOnlySpan<byte> — the families
                                                    //   do not interconvert)
using var archive := gz2.TakeStream();              // finishes the member, detaches
archive.Seek(0, SeekOrigin.Begin);                  // the MemoryStream is seekable (0006 Q3)
var bytes := archive.ReadAll();                     // the compressed .gz bytes (§7.1)
// gz2 is detached; its scope-end dispose releases codec state only (Q4).
```

---

## 10. Implementation substrate — the zlib question (owner-facing)

The wrappers need an incremental (streaming) DEFLATE codec. Recommendation and the decline table, in 0007 §6.3's format; **adopting any of these is a new dependency, which CLAUDE.md makes a project-level owner decision — nothing is implemented autonomously**, and this is open question Q1:

| Substrate | Verdict | Reasons |
|---|---|---|
| **system zlib** | **Recommended** | The universal DEFLATE implementation; system-packaged on Fedora (as the zlib-ng-compat build — same API/ABI, faster); a genuine *streaming* API (`inflate`/`deflate` over caller-fed windows) that maps 1:1 onto the wrappers and onto a narrow runtime-internal `bz_zlib_*` seam (~6 entries: `{deflate,inflate}_{new,step,free}`) mirroring 0007 §6's sans-IO discipline — the codec never sees a stream; the wrapper owns all real I/O. Battle-tested beyond any alternative. |
| libdeflate | Declined for the streams; possible one-shot accelerator later | Whole-buffer API only — **no streaming**; cannot back the wrappers. Faster for the one-shots, but two substrates for one module is complexity before any need is shown. |
| miniz / vendored single-file | Declined | Vendoring is dependency debt in its worst form (an unpatched crypto-adjacent codec pinned in-tree) for no capability system zlib lacks. |
| self-implementation | Declined | Inflate's edge cases (dynamic Huffman, window handling) are a notorious correctness/security surface; thousands of lines to re-audit for zero capability. Contrast 0068 §10, where the self-contained option is a *few hundred* lines of straight-line arithmetic — the trade differs, so the recommendations differ. |
| libzstd / brotli | Staged / deferred | §3's format staging; each is its own dependency ruling when its consumer materializes. |

---

## 11. What v1 deliberately leaves out

- **zlib-format (RFC 1950) types** — deferred until 0001's `Content-Encoding: deflate` policy names its need (§3).
- **zstd, Brotli** — staged with recorded adoption criteria (§3).
- **Archive formats (zip/tar)** — a separate proposal; the zip-slip hazard is pre-registered there (§3).
- **Borrowed-inner (`leaveOpen`) construction** — deferred to the §28.14 scoped-construction amendment, exactly as 0006 §5 (Q1).
- **gzip header metadata** (write filename/mtime, read them back) — deferred; v1 pins the anonymous deterministic header (§8).
- **`NoCompression` level, dictionary support, window/buffer knobs** — deferred; v1 pins standard parameters (Q8, Q7).
- **A capped streaming decompressor** (`Decompress(source, maxOutputBytes)`) — deferred; the stream consumer already controls consumption (Q6), and a limit-enforcing wrapper is additive if a consumer wants the bound enforced for it.
- **Transparent HTTP integration** — 0001/0002's decision, over these primitives (§1).

---

## 12. Declined alternatives (register)

1. **Mode-enum constructors** (.NET `CompressionMode`) — direction hidden in an argument; the level×Decompress invalid combination representable and runtime-faulting; declined for named factories that make it unrepresentable (Q2).
2. **A borrowed-inner constructor in v1** — the same soundness decline as 0006 Q1, resolved by the same pending `scoped` ruling; not re-litigated here.
3. **A default `maxOutputBytes`** on the one-shot decompress — any default is a silent policy; the required parameter is the announced-bounds posture, and `long.MaxValue` is the explicit opt-out (Q6). The strongest-form alternative (no cap parameter at all, .NET's shape) was declined on the §31.4 allocation-termination argument: in this language the bomb is a process-kill, not a catchable OOM.
4. **First-member-only gzip reads** — silently truncates rotated logs; declined for read-all-members (Q5).
5. **Lenient decoding** (ignore trailer mismatches, tolerate garbage tails) — declined per the §3.2/0009 strict-fault house model; corruption is a defined fault, never silently returned data (Q5).
6. **`EndOfStreamException` for truncated input** — declined; §7.5 reserves it for exact-read consumers, and format truncation is corruption with the same caller remedy (Q5).
7. **Folding the cap fault into `InvalidDataException`** — declined; the two conditions demand different caller branches, the 0007 typed-taxonomy discipline (Q6).
8. **`GZipStream` spelling** — declined; the type does not inherit .NET's shape, so the stdlib §1 Bezel-coined acronym rule applies (Q8).
9. **String conveniences** (`CompressString(s)`) — declined; `Encoding.UTF8.GetBytes` (§3.2) composes in one call, and a hidden encoding choice inside a compression API is a silent policy.
10. **Exposing compressed-byte determinism as a contract** — declined; only round-trip identity and the anonymous header are promised (§8) — pinning bitstreams would freeze the substrate forever.

---

## 13. Open questions for the owner

1. **The zlib dependency (load-bearing).** Confirm system zlib behind a runtime-internal `bz_zlib_*` seam (§10) — a new dependency adoption, which is an owner-level decision per CLAUDE.md. Everything observable in this proposal is substrate-independent, but the streams cannot ship without *some* incremental codec ruling.
2. **The required cap (Q6).** Confirm that one-shot `Decompress`/`TryDecompress` take `maxOutputBytes` with **no default**, with `long.MaxValue` as the explicit opt-out — ceremony deliberately traded for safety under §31.4's allocation-termination rule. The fallback position (a large default) is recorded as declined alternative #3.
3. **`TakeStream` in v1 (Q4).** Include the finish-and-detach member now (expressible today via §28.8.1's nullable-owned-field detach; covers the compress-into-memory case), or hold the surface until the #1439 `scoped` ruling lands and reassess? Recommendation: include — detach and borrow are different contracts, and the member's mechanics reuse existing spec machinery only.
4. **Format set confirmation (§3).** Confirm v1 = gzip + raw DEFLATE with zlib-format deferred to 0001's content-encoding decision, and zstd/Brotli staged behind named consumers + their own dependency rulings.
5. **`InvalidDataException`'s home.** The name is deliberately general — any future binary/archive/wire reader wants it — but this proposal places it in `System.IO.Compression` (§31.4 producer-locality), which would force a later duplicate or a compression import for unrelated faults. Hoist it into stdlib §7.5 as a general `System.IO` type at graduation (a one-line docs change), or keep it producer-local, or rename to `CompressedDataException` and leave the general name unclaimed?

---

## 14. Relationship to existing work

- **Proposal 0006** (accepted) — the anchor: owned-wrapping construction (Q1 = its Q1), the fallible-dispose `IAsyncDisposable` pattern (Q3 = its Q7), `MemoryStream`/`CopyToAsync` as the composition partners in every example, and the shared deferral of borrowed-inner construction to the #1439 `scoped` ruling.
- **Proposal 0007** (accepted) — the non-seekable `InvalidOperationException` owner ruling reused by Q2; the typed fault-taxonomy discipline of §7; the narrow runtime-internal substrate-seam pattern §10 mirrors; `NetworkStream` as the domain-local precedent §6 matches.
- **Proposal 0011** (accepted) — the `Take*(): owned Stream` hand-off naming Q4 reuses.
- **Proposal 0009** (accepted) — the one-shot byte-in/byte-out conventions and the `TryDecode` precedent behind `TryDecompress` (Q6).
- **Proposal 0029** (draft) — the CRC-32 kinship: the gzip trailer's CRC-32 is the same IEEE algorithm, computed *internally* by this module's substrate (no dependency edge on `System.IO.Hashing`); 0029 remains the user-facing checksum surface, and raw-DEFLATE users wanting integrity are pointed at it (§3).
- **Proposals 0001/0002** (draft/accepted) — the HTTP consumers: 0002's deferred "compression" line gets its referent; the transparent-content-encoding decision stays theirs (§1).
- **Proposal 0040** (draft sketch) — artifact/cache compression as a prospective consumer; §8's deterministic header is the reproducibility affordance it would want.
- **Proposal 0045** (partially graduated) — §6 records that this module adds no intercept points; replay sees compression as pure computation between already-intercepted I/O edges.
- **[Gap review 2026-07](papers/stdlib-gap-review-2026-07.md)** — cited as the map this proposal extends: compression appears nowhere in its 31 ranked gaps, its declined list, or its blocked list.
- **[DECLINED.md](DECLINED.md)** — no conflict: neither D-0001…D-0004 nor the eight gap-review declines touch compression.
