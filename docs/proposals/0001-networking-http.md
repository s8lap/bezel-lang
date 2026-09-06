# Proposal 0001 — Networking and the `Http` client

| | |
|---|---|
| **Status** | under-review — draft |
| **Target modules** | `System.Net.Http` (this proposal); `System.Net` substrate specified in **proposal 0007** |
| **Written against** | spec/stdlib **v0.179** (refreshed 2026-07-07 from the v0.156 draft) |
| **Tracking issue** | [#1376](https://github.com/s8lap/bezel-lang/issues/1376) |
| **Design influences** | JS `fetch` (WHATWG) for ergonomics; `ky`/`got` for the fixes to fetch's known traps; **not** .NET `HttpClient` |

The stdlib already reserves the slot: *"`System.Net` and `System.Net.Http` — networking and HTTP. The language specification's async examples use a hypothetical `HttpClient`; a real one will live here."* (stdlib §11, L3808). This proposal fills that slot.

This document was adversarially reviewed against the docs before filing and refreshed to v0.179 (citations, ownership/async soundness, and grammar legality of every snippet re-checked); the issues it depends on are listed in §9.

---

## 1. Design principles

1. **fetch's shape, not .NET's.** One call, URL string in, awaitable response out, named body accessors (`Text`/`Bytes`/`Json<T>`). No request-message class zoo, no content-wrapper classes, no client lifecycle lore.
2. **Fix fetch's documented traps instead of inheriting them.** Built-in timeout with a default; a client-with-defaults that is *derivable*; one-line JSON POST; a real error taxonomy instead of one opaque failure; no silent `!res.ok` middle ground (see §4).
3. **Bezel-native where Bezel is stronger.**
   - The options bag is **named defaulted parameters** (spec §22.5) — evaluated per call, no options object needed.
   - The response is a **Pattern B owned resource** (`OwningTask<HttpResponse>`, spec §30.5): `using var resp := await …` and disposal is connection hygiene, not folklore.
   - The body is **single-consumption**, applying the rule the spec already states for streaming readers (§29.11): fetch's worst runtime surprise (`bodyUsed`) becomes a principled rule, enforced by `InvalidOperationException` at runtime (§4.3 discusses why compile-time flow enforcement would need new spec surface, and defers it).
   - HTTP **status codes are data**, matching the language's own error philosophy (Guide 2 §5: expected failures are values; exceptions are for the unexpected). Transport failures throw; a 404 does not. `EnsureSuccess()` is the explicit opt-in to throw-on-status.
4. **Never sync-over-async.** No synchronous convenience wrappers, no `Wait()`/`Result` anywhere in examples or implementation — under the domain model that is a deterministic deadlock (issue #1362).
5. **The pool is infrastructure, not API.** Connection pooling/keep-alive lives in the runtime per domain. `Http.Get(url)` is safe, pooled, and correct by default; no user ever manages a pool. (.NET's socket-exhaustion/DNS-staleness dilemma is a lifecycle-API disease; we don't ship the API that causes it.)

## 2. Tour by example

Snippets are **method-level fragments** — the enclosing `module`/class declarations are elided; `import System.Net.Http;` is assumed, plus `import System.IO;` where `FileStream` appears and `import System.Threading;` where `TimeoutException` is caught.

```bezel
// The minimal call — module-level, no client object.
public async method PrintUser(): Task
  using var resp := await Http.Get("https://api.example.com/users/1");
  var body := await resp.Text();
  Console.WriteLine(body);
end PrintUser;

// Typed JSON in one step. User is a record declaring `: IDeserializable`
// (stdlib §2.14 — serialization is declared conformance, synthesized per §9.8).
public async method LoadUser(id: int): Task<User>
  using var resp := await Http.Get($"https://api.example.com/users/{id}");
  return await resp.EnsureSuccess().Json<User>();
end LoadUser;

// JSON POST — the dominant real-world call is one line, not three incantations.
public async method CreateUser(u: User): Task<int>
  using var resp := await Http.PostJson("https://api.example.com/users", u);
  return resp.Status;
end CreateUser;

// Options are named parameters — Bezel's options bag.
using var resp := await Http.Get(url,
    timeout  := TimeSpan.FromSeconds(5),
    redirect := RedirectPolicy.Manual,
    token    := cts.Token);

// A client carries defaults (base address, headers, timeout) and is derivable.
using var api := new HttpClient(baseAddress := "https://api.example.com");
api.DefaultHeaders["Authorization"] := $"Bearer {token}";
using var resp := await api.Get("/users/1");        // resolves against baseAddress

// Streaming a large download without buffering the whole body: chunk
// iteration, each chunk an owned, correctly-sized Array<byte> disposed per
// iteration (owning async enumeration, stdlib §4.13). No preallocated-buffer
// Count/Capacity trap — see §9 on the Stream read-contract gap.
public async method Download(url: string, path: string): Task
  using var resp := await Http.Get(url);
  using var body := resp.TakeBodyStream();          // consumes the body (§4.3)
  using var file := new FileStream(path, FileMode.Create);
  await for var chunk in body.Chunks(64 * 1024) do
    await file.WriteAsync(chunk, 0, chunk.Count);
  end;
end Download;
```

Error handling follows Guide 2's split — statuses are values, transport failures are exceptions:

```bezel
try
  using var resp := await Http.Get(url, timeout := TimeSpan.FromSeconds(10));
  if not resp.Ok then
    Log($"server said {resp.Status} {resp.StatusText}");   // expected: data
    return;
  end;
  Process(await resp.Text());
catch
  var e: HttpConnectException => Log($"cannot reach host: {e.Message}"),
  var e: TimeoutException     => Log("deadline exceeded"),
  var e: HttpProtocolException => Log($"malformed response: {e.Message}")
end;
```


## 3. Proposed API surface

All types below are **new** (module `System.Net.Http`) unless cited to the stdlib.

### 3.1 `Http` — the module-level entry points

```bezel
public static class Http
  // The general form. fetch(url, init) ⇒ Fetch(url, named args).
  // Note: the HTTP-method parameter is named `verb` — `method` is a reserved
  // word (spec §B.1) and cannot be a parameter name.
  public static async method Fetch(
      url: string,
      verb: HttpMethod := HttpMethod.Get,
      body: string := "",
      contentType: string := "",
      headers: HttpHeaders? := null,                       // borrowed; merged over defaults
      timeout: TimeSpan := TimeSpan.FromSeconds(30),
      redirect: RedirectPolicy := RedirectPolicy.Follow,
      maxRedirects: int := 20,
      token: CancellationToken := CancellationToken.None
  ): OwningTask<HttpResponse>;

  // Per-method shorthands (the ky/got convenience fetch lacks). Same named
  // options as Fetch minus `verb`/`body` where implied.
  public static async method Get(url: string, /* …same options… */): OwningTask<HttpResponse>;
  public static async method Head(url: string, /* … */): OwningTask<HttpResponse>;
  public static async method Delete(url: string, /* … */): OwningTask<HttpResponse>;
  public static async method Post(url: string, body: string, contentType: string := "text/plain; charset=utf-8", /* … */): OwningTask<HttpResponse>;
  public static async method Put(url: string, body: string, contentType: string := "text/plain; charset=utf-8", /* … */): OwningTask<HttpResponse>;
  public static async method Patch(url: string, body: string, contentType: string := "text/plain; charset=utf-8", /* … */): OwningTask<HttpResponse>;

  // JSON shorthands: serialize via JsonSerializer (stdlib §2.15), set
  // Content-Type: application/json. Serialization is declared conformance
  // (stdlib §2.14), so the constraint is real and checkable:
  public static async method PostJson<T>(url: string, value: T, /* … */): OwningTask<HttpResponse> where T: ISerializable;
  public static async method PutJson<T>(url: string, value: T, /* … */): OwningTask<HttpResponse> where T: ISerializable;

  // Binary bodies. Array<byte> follows the Stream async-I/O precedent
  // (stdlib §7.1); a span-taking variant is deliberately absent — a span is a
  // borrow-bearing record (§9.7) and no rule admits one across an async
  // suspension (§8 Q6).
  public static async method PostBytes(url: string, body: Array<byte>, contentType: string := "application/octet-stream", /* … */): OwningTask<HttpResponse>;
end Http;
```

Semantics: the module-level calls use an implicit **per-domain default client** (created lazily on first use; its teardown at domain completion is runtime-intrinsic behavior — no user-visible domain-exit hook exists today, see §5). They are exactly `defaultClientOfCurrentDomain.Fetch(…)`.

**Why `OwningTask<HttpResponse>` (Pattern B):** §30.5's producer rule verbatim — *"`OwningTask<T>` is the right choice for resource acquisition — operations whose entire purpose is to produce a fresh, single-consumer resource (a connection, a lock token, a parsed document)."* A response holds a pooled connection until disposed; the caller must own it. The canonical call site is `using var resp := await …`, an abandoned task disposes its result automatically (§30.5), and a dropped response returns its connection to the pool.

### 3.2 `HttpClient` — defaults, derivation, no lifecycle lore

```bezel
public class HttpClient
  public constructor(
      baseAddress: string := "",
      timeout: TimeSpan := TimeSpan.FromSeconds(30),
      redirect: RedirectPolicy := RedirectPolicy.Follow,
      maxRedirects: int := 20);

  public readonly property BaseAddress: string;
  public readonly property DefaultHeaders: HttpHeaders;    // borrow; mutable until first request

  // The full Http.* surface, instance-form: Fetch/Get/Post/PostJson/… — but the
  // per-call option parameters on INSTANCE methods are declared nullable with
  // null defaults (`timeout: TimeSpan? := null`, `redirect: RedirectPolicy? := null`,
  // `maxRedirects: int? := null`): null means "use this client's default".
  // (A concrete default like `:= TimeSpan.FromSeconds(30)` would be evaluated
  // per call, spec §22.5, and be indistinguishable from an explicit argument —
  // the client's own defaults could never win. Nullable-for-absence is the
  // stdlib's idiom, Guide 2 §5.)

  // ky.extend-style immutable derivation: a new client sharing the same
  // domain-level pool, with overridden defaults. Same null ⇒ inherit scheme,
  // uniformly, so deriving never silently resets an unmentioned default.
  public method Derive(
      baseAddress: string? := null,
      timeout: TimeSpan? := null,
      redirect: RedirectPolicy? := null,
      maxRedirects: int? := null): owned HttpClient;
end HttpClient;
```

The client is **cheap and disposable-safe**: it holds configuration, not sockets (hence no destructor — per the stdlib §1 convention, a class whose disposal is a no-op shows none). Creating one per request is harmless (unlike .NET); creating one per API and deriving variants is idiomatic. Clients are domain-local (ordinary classes holding an owned header store — not sendable, spec §30.14); cross-domain HTTP is per-domain clients, not a shared instance.

### 3.3 `HttpResponse`

```bezel
public class HttpResponse
  public readonly property Status: int;                    // 200, 404, …
  public readonly property Ok: bool;                       // 200..299
  public readonly property StatusText: string;             // "OK", "Not Found"
  public readonly property Url: string;                    // final URL after redirects
  public readonly property Redirected: bool;
  public readonly property Headers: HttpHeaders;           // read-only view (borrow)
  public readonly property ContentLength: long;            // -1 when unknown/chunked

  // ── body accessors — SINGLE-CONSUMPTION (see §4.3) ──
  public async method Text(): Task<string>;                          // decodes per charset; default UTF-8
  public async method Bytes(): OwningTask<Array<byte>>;               // mirrors Stream.ReadAllAsync (stdlib §7.1)
  public async method Json<T>(): Task<T>;                            // T: IDeserializable-conforming record/value DTOs; compiler-known (see below)
  public async method JsonOwned<T>(): OwningTask<T> where T: class;   // class DTOs (FromResult/FromOwnedResult naming precedent, stdlib §6.7)
  public method TakeBodyStream(): owned HttpBodyStream;              // streaming take; caller disposes

  // Throw-on-status opt-in: raises HttpStatusException when not Ok,
  // else returns the receiver (borrow of `this`, §28.6) for chaining.
  public method EnsureSuccess(): HttpResponse;

  destructor;   // returns the connection to the pool if the body was fully
                // consumed, else CLOSES it — no draining I/O in a sync
                // destructor (blocking a continuation stalls the domain,
                // §30.13); DisposeAsync (below) may drain small remainders.
end HttpResponse;

public class HttpBodyStream: Stream                        // Stream per stdlib §7.1
  // CanRead=true, CanWrite=false, CanSeek=false.
  // Chunked read with defined semantics (avoids the Array<byte>
  // Count-vs-Capacity trap in Stream.Read — §9): returns an owned array with
  // Count = bytes read (≤ max); Count = 0 signals end of body.
  public async method ReadChunkAsync(max: int, token: CancellationToken := CancellationToken.None): OwningTask<Array<byte>>;
  // Chunk iteration sugar over ReadChunkAsync (owning async enumeration,
  // stdlib §4.13): each iteration yields an owned Array<byte>, disposed at
  // iteration end. `await for var chunk in body.Chunks(64 * 1024) with token do … end;`
  public method Chunks(max: int): IOwningAsyncEnumerable<Array<byte>>;
  destructor;   // closes/returns the underlying connection (no draining)
end HttpBodyStream;
```

`HttpResponse` additionally implements `IAsyncDisposable` (spec §28.12): `await using var resp := …` lets disposal drain a small unread remainder asynchronously so the connection can be reused; the synchronous destructor never drains — it returns the connection only when the body was already consumed, and closes it otherwise. (Both behaviors are correct; `await using` is the throughput optimization.)

**On `Json<T>`:** the deserialization entry point is a static factory *recognized by name* (stdlib §2.14 — static interface members are deferred), so a generic body cannot invoke it via any existing constraint. `Json<T>`/`JsonOwned<T>` are therefore **compiler-known methods** in the same way `Task.FromResult<T>` is (its class-T rejection is compiler-special-cased, stdlib §6.7): `Json<T>` requires an `IDeserializable`-conforming record/value/`string` T and **rejects class T at compile time**, directing to `JsonOwned<T>` (which requires an `IDeserializable`-conforming class T). This keeps the Pattern A/B split invisible to users: values arrive as values, classes arrive owned.

`Bytes(): OwningTask<Array<byte>>` deliberately mirrors `Stream.ReadAllAsync` (stdlib §7.1) — same world, same shape. This sits on the `Array<byte>` side of the byte-worlds gap (issue #1358); when the bridge lands, spans slice over the result with no API change here.

### 3.4 `HttpHeaders`

```bezel
public record HeaderEntry(Name: string, Value: string);

public sealed class HttpHeaders: IEnumerable<HeaderEntry>
  public constructor();
  public property [name: string]: string;      // get: first value or "" ; set: replace all
  public method Get(name: string): string?;    // first value; null if absent (*OrNull honesty, stdlib §5)
  public method GetAll(name: string): owned Array<string>;   // fresh snapshot of all values
  public method Add(name: string, value: string);            // append (multi-value)
  public method Remove(name: string): bool;
  public method ContainsKey(name: string): bool;
  public readonly property Count: int;
  // iteration: `for var h in headers do … h.Name … h.Value … end;`
  // yields HeaderEntry values in insertion order (DictionaryEntry precedent,
  // stdlib §4.8)
end HttpHeaders;
```

Names are **case-insensitive** (RFC 9110); backing storage is an owned insertion-ordered list of `HeaderEntry` records (two-string records are sendable values, so a **sealed** `HttpHeaders` with that backing is itself sendable per §30.14 — usable in a spawn argument or channel message). No browser "forbidden headers" guard — a systems client may set `Host`, `Content-Length`, `Cookie`. A *received* response's `Headers` is a read-only view (mutators throw `InvalidOperationException`); a response *constructed* server-side by `Http.*` (proposal 0002 §3.3) has writable headers until it is sent — `r.Headers["ETag"] := tag;` through the indexer above.

### 3.5 Supporting types and errors

```bezel
public enum HttpMethod: byte  Get, Head, Post, Put, Patch, Delete, Options, Trace; end HttpMethod;
public enum RedirectPolicy: byte
  Follow,      // default; up to maxRedirects, then HttpRedirectException
  Error,       // any 3xx with Location ⇒ HttpRedirectException
  Manual;      // return the REAL 3xx response (Location readable — no browser opacity)
end RedirectPolicy;
```

Exception taxonomy (got-style; a systems language has no CORS-secrecy excuse for one opaque error). All extend a new `HttpException`, itself extending the built-in `Exception` base (spec §31.4):

| Exception | Raised when |
|---|---|
| `HttpConnectException` | TCP connect refused / unreachable / reset before response |
| `NameResolutionException` | DNS failure |
| `TlsException` | certificate validation / handshake failure |
| `HttpProtocolException` | malformed response, header limits, chunking errors |
| `HttpRedirectException` | redirect policy violation (`Error`) or `maxRedirects` exceeded |
| `HttpStatusException` | **only** from `EnsureSuccess()`; carries `Status: int`, `StatusText`, `Url` (strings/values only — a caught exception is a borrow, Guide 2 §3, so it cannot carry the response object) |
| `TimeoutException` (stdlib §6.1, needs `import System.Threading`) | per-request `timeout` budget exhausted |
| `OperationCanceledException` (built-in, spec §31.4) | the caller's `token` fired |

Timeout and cancellation stay **distinct** (the stdlib already draws this line, §6.1): `timeout:` raises `TimeoutException`; the caller's token raises `OperationCanceledException`.

## 4. Ownership and async integration

### 4.1 Pattern B end-to-end

Every request method returns `OwningTask<HttpResponse>`; the await must sit in an owned-consuming context (§30.5) — `using var resp := await …` is canonical, `await using var resp := …` when async disposal (remainder draining) is wanted. Response disposal returns the connection to the pool when the body was consumed and closes it otherwise; only `DisposeAsync` performs draining I/O (a synchronous destructor must not block the domain, §30.13).

### 4.2 Concurrent requests

Fan-out uses the existing aggregation surface (stdlib §6.7): `Task.WhenAllOwned<HttpResponse>(owned tasks: OwningArray<OwningTask<HttpResponse>>): OwningTask<OwningArray<HttpResponse>>`. No new machinery.

### 4.3 The single-consumption body

The spec already states the rule this design applies (§29.11): *"A lazy or single-pass source whose backing is itself external, such as a streaming reader, follows the same single-consumption rule."*

- Exactly **one** of `Text` / `Bytes` / `Json<T>` / `JsonOwned<T>` / `TakeBodyStream` may be called per response. A second body access raises `InvalidOperationException`, its message naming the first consumer.
- Enforcement is **runtime-only** in this proposal. §29.11's compile-time half attaches to compiler-known lazy-query values consumed *as values*; here the consumed thing is an interior sub-resource of a still-usable object (`Status`/`Headers`/`EnsureSuccess` remain valid after the body is consumed), and no language mechanism lets a library mark methods as consuming-of-interior-state. A `consuming`-method annotation that would upgrade this to a flow-visible compile error is possible future spec surface — noted in §8, not required for v1.
- There is **no `clone()`**. fetch needs `clone()` because middleware wants to peek; the Bezel idiom is to buffer explicitly — `using var bytes := await resp.Bytes();` — after which the caller re-reads its own buffer freely. (Materialization as the escape hatch, exactly like `.ToArray()` for lazy queries, §29.11.)
- `EnsureSuccess()` does not touch the body and may be called any number of times.

### 4.4 Domains

Clients, responses, and body streams are domain-local (not sendable, §30.14). The per-domain default client means `Http.Get` works correctly in *any* domain with zero setup. Cross-domain patterns move **results** (records, strings, owned buffers, or the sendable `HttpHeaders` of §3.4) through channels, never live responses — consistent with the rest of the concurrency model.

## 5. The substrate (`System.Net`)

The transport layer this client sits on — nonblocking sockets, DNS, TLS, and
the per-domain connection pool — is specified in its own document, **proposal
0007 (`System.Net`)**. That proposal resolves the runtime dependencies this
design once sketched here: async socket I/O over a runtime nonblocking-socket
event loop (completing `OwningTaskCompletionSource`-backed tasks onto their
domains), the per-domain pool and lazily-created default client, the TLS
backend decision, the `TcpConnection` / `NetworkStream` / `TlsClientStream` /
`Dns` surface, and the transport-level error roots. Two consequences for this
proposal:

- The per-domain default client and connection pool are runtime-owned and torn
  down as part of a domain's ordinary resource reclamation (§30.14 says only
  that a domain's resources are reclaimed when its last continuation completes) —
  there is no user-visible domain-exit hook, and none is needed.
- The transport error roots (`NameResolutionException`, `TlsException`, and the
  socket-level failure that `HttpConnectException` wraps) are defined by
  proposal 0007; the `Http*` taxonomy of §3.5 layers over them.

## 6. What we deliberately do not copy

**From fetch/browsers** (they parameterize a sandbox we don't have): `mode`/CORS, `credentials`, `cache`, `referrer(Policy)`, `keepalive`, `integrity`, `priority`, opaque responses/`Response.type`, forbidden-header guards, service-worker plumbing. `redirect: Manual` stays but returns the *real* response.

**From .NET**: the client-lifecycle dilemma (pool is per-domain runtime infrastructure); `HttpRequestMessage`/`HttpContent` class zoo (named args + typed shorthands instead); `IHttpClientFactory` (nothing to factory); sync APIs over async (#1362); one `HttpRequestException` for every failure (taxonomy instead).

## 7. Deferred (v2+, in rough priority order)

Retry policy (ky's defaults are the model: idempotent methods, 408/429/5xx, `Retry-After`); hooks/middleware (the extension point retry/auth/logging hang off — needs a design that respects ownership of the in-flight request); download/upload progress; a cookie jar (explicit opt-in object on the client, never ambient); response caching (explicit component, never implicit); a `Uri` type + query-parameter ergonomics (string URLs suffice for v1; a `Uri` type is its own proposal — parsing/normalization is a swamp worth its own review); per-phase timeouts (got-style connect/TLS/first-byte/total); multipart/form-data; request-body streaming (upload from a `Stream` — with it, `duplex` semantics); HTTP/2 & 3; WebSocket; server-side `Request → Response` (the fetch vocabulary is deliberately symmetric and server-ready; the server is proposal 0002 — re-drafted 2026-09-06 on the v0.294 §28.14 trace, sharing this proposal's `HttpResponse`/`HttpHeaders`/`HttpMethod`/`Http` — and out of scope here).

## 8. Open questions

1. **`Json<T>` / `JsonOwned<T>` split** — both are compiler-known (§3.3); the two-name split follows `FromResult`/`FromOwnedResult`. Acceptable, or should the eventual serialization-constraint story (static-factory constraints are deferred in stdlib §2.14) unify them?
2. **Default timeout value** — 30s proposed (ky ships 10s; fetch shipped none for years). Having *a* default is the decision; the number is bikesheddable.
3. **Header storage** — insertion-ordered `HeaderEntry` list proposed (sendable, order-preserving, honest multimap); header counts are small so linear scan is fine. Confirm, or want an index?
4. **`HttpBodyStream.Length`** — propose: throws `InvalidOperationException` when unknown (`CanSeek` is false), with `HttpResponse.ContentLength: long = -1` as the query-friendly form.
5. **Chunk iteration** — `Chunks(max)` is included in the v1 surface (§3.3) as sugar over `ReadChunkAsync`, and the §2 tour uses it. Remaining sub-question: should `ReadChunkAsync` (the primitive) stay public, or is the iterator alone enough? Proposed: both stay (the primitive serves protocols that read exact framings).
6. **Span-taking request bodies** — `PostBytes(body: ReadOnlySpan<byte>)` is the natural zero-copy signature but a span is a borrow-bearing record (§9.7) and **no rule admits one across an async suspension**; the stdlib has zero async span-taking APIs today. Either a spec ruling ("a span parameter to an async method must be consumed before the first suspension" — checkable?) or the `Array<byte>` form stays. Deferred with the question stated.
7. **`Json<T>` charset handling** — JSON is UTF-8 in practice (RFC 8259); propose: honor charset from Content-Type for `Text()`, always-UTF-8 for `Json*<T>`.

## Delegated rulings 2026-08-11 (stdlib-wave readiness; owner review in repo)

The seven §8 open questions are resolved under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo) (delegated ruling; recorded here per the 0012 precedent). Status stays draft — the ledger row is the coordinator's.

1. **Q1 — keep the `Json<T>` / `JsonOwned<T>` two-name split; conform the mechanism to the landed v0.232 facade shape at graduation.** The question has been answered by a landed precedent since the pin: proposal 0073 graduated (stdlib §2.15, v0.232) `Json.Parse<T>`/`ParseOwned<T>` **on this proposal's own §3.3 dispensation**, facing the identical Pattern A/B fork and keeping both the two-name split and the compiler-known assistance — with the signatures now *also* spelling `where T: IDeserializable` (writable today, unlike at v0.177). This surface conforms to that landed shape: `Json<T>`/`JsonOwned<T>` carry the constraint and stay compiler-known for the class-`T` rejection-with-direction. A future static-factory-constraint unification, if it ever lands, re-cuts the facade and this surface together — deferred there, not here. Declined: unifying now under a single name (nothing to unify with; the landed stdlib convention is the split).
2. **Q2 — default timeout 30s: confirmed.** That a default *exists* is the load-bearing decision (fetch's no-default shipped infinite hangs for years); 30s bounds a wedged server while clearing slow TLS handshakes and cold CDN paths on bad links. ky's 10s is tuned for interactive browser UX and is too aggressive for server-to-server bulk responses. The number may be re-examined at graduation with evidence; the existence of the default is final.
3. **Q3 — insertion-ordered `HeaderEntry` list: confirmed, no index.** Sendable, order-preserving, an honest multimap; real header counts are small enough that linear scan wins in practice. An index is a pure implementation detail addable behind the same surface if a profile ever demands it — declined now as speculative machinery.
4. **Q4 — `Length` faults when unknown: confirmed; `ContentLength` amended to `long?`.** `HttpBodyStream.Length` with `CanSeek` false raises `InvalidOperationException` with a message naming the reason — the exact posture 0007's Q2 ruling (owner, 2026-07-04) fixed for non-seekable streams (no `NotSupportedException` addition). The query-friendly form is **amended from the `-1` sentinel to `HttpResponse.ContentLength: long?`** (null = unknown): since the pin, the stdlib's absence convention is the nullable tier (v0.231, proposal 0072; stdlib §1's taxonomy), and a `-1` sentinel invites silent arithmetic on an absent value where `long?` makes the absence type-checked. Declined: keeping `-1` (a C-ism the landed convention supersedes).
5. **Q5 — both `Chunks(max)` and `ReadChunkAsync` stay public: confirmed.** The iterator is sugar; the primitive serves exact-framing protocol readers, and hiding it would force reimplementation over `Bytes()`.
6. **Q6 — resolved by a landed spec rule: the `Array<byte>` form is final for v1.** v0.179 (#1438) barred borrow-bearing (span) parameters on `async` methods outright — spec §30.1's prohibition, restated in stdlib §7.1's "No async span overloads" note — and the removal has long since reached the docs. `PostBytes(body: ReadOnlySpan<byte>)` in async position is inexpressible, and the alternative the question floated (a "consumed before the first suspension" rule) was effectively declined when the prohibition landed instead. Zero-copy async request bodies revisit only if the spec ever gains such a rule (that is #1438-residue territory, not this proposal's).
7. **Q7 — charset: confirmed as proposed.** `Text()` honors the Content-Type charset (UTF-8 fallback); `Json<T>`/`JsonOwned<T>` are always UTF-8 (RFC 8259). Malformed input faults under the §3.2 decode discipline — never silently substituted.

## 9. Relationship to existing issues

- **#1362** (Wait/Result deadlock): this design has no sync surface; its docs must never suggest `Wait()` on a fetch task.
- **#1358** (byte-worlds bridge): `Bytes()`/`PostBytes` live on the `Array<byte>` side (Stream precedent); the design benefits mechanically when the bridge lands.
- **Stream read-contract gap** (filed with this proposal; **resolved in stdlib §7.1, v0.165** — see proposal 0006 §7): `Stream.Read/ReadAsync(buffer: Array<byte>, offset, count)` originally had no defined contract against `Count` vs `Capacity`, and no stdlib API constructed a `Count = N` buffer, so the canonical read-into-buffer loop was unwritable. §7.1's read contract now defines it. `HttpBodyStream.ReadChunkAsync` (owned, sized result) sidesteps the question regardless by returning a correctly-sized owned array.
- **#1226 / spec §29.11** (affine single-consumption): the body rule applies the already-normative streaming-reader clause (runtime enforcement; compile-time flow enforcement would need new spec surface — §4.3).
- **#974** (TaskCompletionSource): the substrate's completion plumbing is the existing TCS machinery.
- **Stdlib §11 L3808**: the reserved `System.Net(.Http)` slot this proposal fills.
