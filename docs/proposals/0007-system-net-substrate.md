# Proposal 0007 — The `System.Net` substrate (sockets, `Dns`, TLS seam)

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Target modules** | `System.Net` (this proposal). `System.Net.Http` (0001) and the `Http` server (0002) sit on top. |
| **Written against** | spec/stdlib **v0.177** |
| **Tracking issue** | [#1436](https://github.com/s8lap/bezel-lang/issues/1436) |
| **Origin** | The companion 0001 promised it: [§5 "The substrate (`System.Net`, sketch — separate proposal to follow)"](0001-networking-http.md). Wave **D1** of the [stdlib gap DESIGN plan](stdlib-gap-design-plan-2026-07.md). |
| **Design influences** | Go `net` (a listener you `Accept` from; connections that are just streams), Rust `std::net` + the **sans-IO** discipline (`rustls`/`tokio-rustls` split the state machine from the I/O), .NET `Socket`/`NetworkStream`/`SslStream` for the shape of the seams; **not** .NET's synchronous `Socket` surface, **not** Node's `net.Socket` event-emitter model |
| **Currency note** | Re-verified clean at v0.306 (2026-09-17 sweep): no tier-1 respell — every stdlib anchor cited here (§1 failure-signal rule, §4.14, §6.1, §7.1, §7.5) and every spec section (§28.3, §28.14, §30.5, §30.8, §30.11, §30.13–§30.15, §31.4) still resolves, and §30.14's quoted domain-local list still reads as quoted (it has since gained `AsyncEventSource<T>`). One tier-2 flag recorded in `proposals-currency-2026-09-17.md` §3 (§5.3/§9 Q3's span-across-`await` premise). Written-against pin unchanged. |

0001 §5 lists its "honest dependencies on runtime work" and sketches four visible types, deferring **every** constraint — sendability, timeout model, buffer ownership, the TLS backend, the accept/listener surface, and the error taxonomy — to "a separate proposal to follow." This is that proposal. It settles the deferred constraints against the v0.177 language and stdlib, and specifies the runtime seam precisely enough for a future implementation lane without writing any C++.

Every existing type is cited to `Bezel_Standard_Library.md` or `Bezel_Language_Spec.md`; every new type is in module `System.Net` unless stated. Snippets are method-/declaration-level fragments; `import System.Net;` is assumed, plus `import System.IO;` where `Stream` appears and `import System.Threading;` where `CancellationToken`/`TimeoutException` are named.

---

## 1. Design principles

1. **Sockets are streams; the substrate is small.** A `TcpConnection` yields a `NetworkStream` (`Stream`, stdlib [§7.1](Bezel_Standard_Library.md#sec-stream)); a `TlsClientStream` is another `Stream` wrapping the transport. Everything above (framing, HTTP, the connection pool) lives in higher layers. The substrate is: connect, accept, resolve, read/write bytes, negotiate TLS. Nothing else in v1.
2. **The runtime owns all real I/O; the language surface is `async`.** There is no blocking `recv`/`send` in the API — a blocking syscall inside a continuation stalls the whole domain (run-to-completion, spec [§30.13](Bezel_Language_Spec.md#sec-runtime-model)). Every operation that can block is an `async` method completing an `OwningTaskCompletionSource`-backed task onto its domain, exactly the machinery 0001 §5.1 names. No new *language* surface — only runtime intrinsics.
3. **Ownership makes lifetime compile-visible.** A connection is a Pattern B resource (spec [§30.5](Bezel_Language_Spec.md#sec-result-lifetime-and-task-disposal)): `using var conn := await TcpConnection.Connect(…)`, disposal is `close(2)`. A TLS stream *owns its transport* so one `using var` tears down the whole stack. There is no `leaveInnerStreamOpen` footgun.
4. **Domain-affine transport, sendable listener.** A live connection never crosses a domain boundary (§3, the load-bearing ruling). A `TcpListener` is internally synchronized and *is* sendable, so the 0002 worker-domain model composes with zero connection-handoff machinery.
5. **Certificate validation on by default.** `TlsClientStream` validates the peer chain and hostname against the system trust store; there is no one-line switch to turn it off. Pinning / custom validators are a deferred `scoped`-callback design (§8), not a v1 boolean.
6. **A real transport error taxonomy.** Typed, catchable faults rooted under `IOException` (stdlib [§7.5](Bezel_Standard_Library.md#sec-io-exception-types)) so `catch IOException` is the uniform "any I/O failed" umbrella and `catch ConnectionRefusedException` is the precise one (§7). No single opaque error.

## 2. Where this sits

```
  System.Net.Http (0001)   ·   Http server (0002)
  ─────────────────────────────────────────────────
  System.Net (THIS PROPOSAL)
    TcpConnection   NetworkStream   TlsClientStream
    TcpListener     Dns             error taxonomy
  ─────────────────────────────────────────────────
  runtime intrinsics: nonblocking event loop (epoll/kqueue/IOCP),
    OwningTaskCompletionSource completion (spec §30.11), bz_tls_* seam
```

0001's `Http.Get` runs on a per-domain default client that pools `TcpConnection`s; 0002's `Listen` binds a `TcpListener` and dispatches accepted connections to worker domains. Both are **above** this substrate and out of scope here — but §3 and §5 are written with their needs as the acceptance test.

The substrate stays available directly for the 1% who need raw sockets (0002 §5 names it as the escape hatch below `Text`/`Bytes`/`Json`).

## 3. The load-bearing ruling: transport is domain-affine

> **`TcpConnection`, `NetworkStream`, and `TlsClientStream` are domain-local — *not* sendable (spec [§30.14](Bezel_Language_Spec.md#sec-sendable-types)). `TcpListener` is compiler-known *sendable* and internally synchronized. A `Dns` result is `Array<string>` (already sendable).**

This is the decision 0001 §5 deferred, and it governs everything else. The argument:

**Why a connection is domain-affine, not sendable.**

1. **It is the same family §30.14 already excludes.** §30.14 lists `Task`, `Task<T>`, `OwningTask<T>`, enumerators, `EventSource<T>`, `CancellationTokenSource`, and `Weak<T>` as "domain-local and not sendable." Each is a *live async resource whose completion state is bound to a domain* ([§30.13](Bezel_Language_Spec.md#sec-runtime-model) scheduling). A `TcpConnection` holding a runtime-registered nonblocking fd whose read/write completions are queued to a specific domain is exactly that family. Classing it sendable would be inconsistent with the types it most resembles.
2. **A socket carries mutable in-flight state that is not internally synchronized.** Partial reads, unflushed writes, and — for TLS — record boundaries and the handshake state machine are mutable buffers the two directions of the stream mutate. Two domains reaching one connection simultaneously is precisely the shared-mutable-state data race §30.14 exists to exclude. Unlike `Channel<T>`, a socket is *not* internally synchronized (adding per-connection locks to make it so would pay a synchronization cost on every read to enable a pattern no one needs — principle §1's "connections are streams, kept simple").
3. **0001 already ruled the layer above domain-local.** 0001 §4.4: *"Clients, responses, and body streams are domain-local (not sendable, §30.14)."* A domain-local `HttpResponse` streaming from a *sendable* `TcpConnection` would be an incoherent layering — the borrow of the socket inside the response would be a cross-domain borrow. Domain-affinity at the substrate is what makes the whole stack's domain-locality sound.
4. **It imposes the weakest runtime contract.** Domain-affinity lets the runtime bind an fd's completion routing to the single domain that created it — no per-operation "which domain issued this?" re-homing, no proof obligation that a moved connection is quiescent. Making the connection sendable would require the runtime to re-home completion routing on transfer *and* a soundness argument that no operation is in flight at the move point — real machinery for a capability the listener ruling below shows we do not need. Per CLAUDE.md's "simplicity beats speed until the compiler is complete," the substrate takes the weaker contract.

**Why this loses nothing — the listener is the seam that scales.** The one place a connection "needs" to cross domains is the server: accept here, handle there. We dissolve that need instead of paying for it. `TcpListener` is **compiler-known sendable and internally synchronized** — concurrent `Accept` from several domains is kernel-serialized (the same guarantee that lets N threads `accept(2)` one listening socket), so a listener is the substrate's analogue of `Channel<T>`: the single object two domains may reach. The 0002 worker-domain model then becomes:

```bezel
// Each worker domain holds the (sendable) listener and accepts its OWN connections.
// A connection is born on the domain that accepted it and lives its whole life there —
// it never crosses a boundary, so §30.14 is satisfied with nothing moved.
public static async method Worker(owned listener: TcpListener): Task
  using var l := listener;
  while not l.IsClosed do
    using var conn := await l.Accept();     // owned TcpConnection, this domain's
    await Serve(conn);                       // read/write all on this domain
  end;
end Worker;
```

Data that genuinely must cross domains crosses as **sendable values** — `string`, owned `Array<byte>`, sendable records, the sealed sendable `HttpHeaders` of 0001 §3.4 — exactly as 0001 §4.4 already prescribes ("move results … through channels, never live responses"). The live socket stays put; only bytes and records travel.

**Two sound topologies for 0002, both handoff-free** (0002 §4.1 says "dispatch to the least-loaded worker" without fixing where I/O runs; either reading is admitted):

- **(A) Sendable listener, N accepting workers** *(recommended).* Each worker owns a slice of connections and does its own socket I/O; the OS/runtime balances accepts. No connection ever moves. Requires the listener-sendable ruling above.
- **(B) Single acceptor, data over channels.** One domain owns the listener and every connection; workers receive request *data* and return response *data* over `Channel<T>` ([§30.15](Bezel_Language_Spec.md#sec-channels)). The connection never moves either; only sendable values cross. Needs no listener sharing.

Both obey §30.14 with **zero connection handoff**. The proposal ships the mechanism for (A) (a sendable listener) and leaves the topology choice to 0002.

**These classifications are proposed additions to §30.14's compiler-known list** (that list already enumerates the stdlib types case by case); they graduate into the spec when the substrate lands. Marked here as proposed, not assumed.

## 4. Proposed API surface

### 4.1 `TcpConnection`

```bezel
public class TcpConnection: IDisposable            // runtime-intrinsic-backed; DOMAIN-LOCAL (§3)
  // Connect resolves `host` (name or textual address, §4.5), opens a TCP
  // connection, and produces a fresh single-consumer resource — Pattern B
  // (spec §30.5). `timeout` bounds DNS + handshake (§5.2); `token` cancels.
  public static async method Connect(
      host: string,
      port: int,
      timeout: TimeSpan := TimeSpan.FromSeconds(30),
      token: CancellationToken := CancellationToken.None): OwningTask<TcpConnection>;

  // A borrow of the connection's byte stream. The connection owns the stream;
  // reading/writing the stream is reading/writing the connection. The returned
  // borrow may not outlive the connection (spec §28.3).
  public method GetStream(): NetworkStream;

  public readonly property RemoteAddress: string;    // textual peer address (§4.5)
  public readonly property RemotePort: int;
  public readonly property LocalPort: int;

  // Half-close: send a FIN, signalling end-of-write to the peer while reads
  // continue (the one shutdown granularity v1 exposes; §8 defers finer control).
  public async method ShutdownSend(token: CancellationToken := CancellationToken.None): Task;

  destructor;   // close(2); deregisters the fd from the event loop
end TcpConnection;
```

`Connect` is `OwningTask<TcpConnection>` for the reason 0001 gives its responses: §30.5's producer rule — *"the right choice for resource acquisition — operations whose entire purpose is to produce a fresh, single-consumer resource (a connection, …)."* Canonical site: `using var conn := await TcpConnection.Connect(host, port);`.

### 4.2 `NetworkStream: Stream`

`NetworkStream` implements the existing `Stream` contract (stdlib [§7.1](Bezel_Standard_Library.md#sec-stream)) verbatim — it adds **no** new stream surface, so 0006's forthcoming `StreamReader`/`StreamWriter`/`CopyToAsync` compose over it unchanged (the seam noted in §8).

```bezel
public class NetworkStream: Stream                 // import System.IO for Stream; DOMAIN-LOCAL
  // Capabilities:
  //   CanRead  = true, CanWrite = true, CanSeek = false.
  //
  // Inherited from Stream (§7.1), with network semantics:
  //   Read/Write(buffer: Array<byte>, offset, count)          — windowed form (§7.1 v0.165):
  //                                                             the window ranges over Count.
  //   Read(Span<byte>) / Write(ReadOnlySpan<byte>)            — inherited span forms (see §5.3).
  //   ReadAsync/WriteAsync(... , token)                        — the async forms; the ones the
  //                                                             substrate actually implements.
  //   Flush()                                                  — no-op (TCP has no user flush;
  //                                                             Nagle/TCP_NODELAY is a socket
  //                                                             option, deferred §8).
  //   ReadAll()/ReadAllAsync()                                 — read until the peer closes.
  //
  // Not meaningful on a socket — Seek, Length, Position raise InvalidOperationException
  // (see the note below).
end NetworkStream;
```

**On the not-seekable members.** `Stream`'s `Seek`/`Length`/`Position` are abstract and must be provided. The idiomatic .NET fault here is `NotSupportedException`, but that type is **not** in the language's built-in list (spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)) nor the stdlib. Rather than invent one, v1 raises **`InvalidOperationException`** (§31.4, "operation not valid in current state") with a message naming the reason (`"NetworkStream is not seekable"`). A dedicated `NotSupportedException` is a plausible small stdlib addition — flagged as an open question (§9), not required here.

`ReadAsync` returns *what is available* (`≤ count`; `0` signals the peer closed the read side — end of stream). `WriteAsync` completes when the bytes are accepted into the kernel send buffer; on a full send buffer it suspends until the socket is writable (§5.4) — TCP's own back-pressure, no app-level buffer knob.

### 4.3 `TlsClientStream: Stream`

`TlsClientStream` **owns its transport** (§28 single-owner discipline): you hand it an `owned TcpConnection`, or use the one-shot `Connect` that does TCP + handshake together. Owning the transport means one `using var` disposes the whole stack and there is no "who closes the inner stream" ambiguity.

```bezel
public class TlsClientStream: Stream               // Stream (import System.IO); DOMAIN-LOCAL
  // One-shot: resolve + TCP connect + TLS handshake, owning the whole stack.
  // targetHost drives SNI and hostname verification; validation is ALWAYS on (§4.3).
  public static async method Connect(
      host: string,
      port: int,
      options: TlsOptions? := null,                // null ⇒ defaults (system trust store)
      timeout: TimeSpan := TimeSpan.FromSeconds(30),
      token: CancellationToken := CancellationToken.None): OwningTask<TlsClientStream>;

  // Lower-level: wrap a connection you already own. `targetHost` is the name to
  // verify against (must be supplied — IP-only TLS without a name is rejected).
  public static async method AuthenticateAsClient(
      owned transport: TcpConnection,
      targetHost: string,
      options: TlsOptions? := null,
      token: CancellationToken := CancellationToken.None): OwningTask<TlsClientStream>;

  // Same Stream surface as NetworkStream: CanRead/CanWrite = true, CanSeek = false;
  // Read/Write/ReadAsync/WriteAsync move PLAINTEXT (the seam of §6 does the crypto).

  public readonly property NegotiatedProtocol: string;   // ALPN result, "" if none
  public readonly property PeerCertificatePem: string;   // inspected chain leaf, PEM text

  destructor;   // TLS close_notify (best-effort, non-blocking) then closes the owned transport
end TlsClientStream;
```

```bezel
public record TlsOptions(
    AlpnProtocols: Array<string>,     // e.g. ["h2", "http/1.1"]; empty ⇒ no ALPN
    MinVersion: TlsVersion);          // default Tls12

public enum TlsVersion: byte  Tls12, Tls13;  end TlsVersion;
```

**Ownership rationale (§28).** The alternative — `TlsClientStream` *borrowing* a `NetworkStream` (0001's literal "wraps a NetworkStream" phrasing) — forces the `TcpConnection` to outlive the TLS stream and re-introduces the leave-open question at every call site. Owning the `TcpConnection` collapses the transport into a single owned resource: `using var tls := await TlsClientStream.Connect("example.com", 443);` and the socket, the TLS context, and the stream all release together. This is the §28 single-owner default; the borrowing form is not offered in v1.

**Validation is on and not switchable.** The peer chain and hostname are verified against the system trust store (OpenSSL defaults, §6). There is deliberately no `insecureSkipVerify: bool` — the disable switch is the single most-abused knob in every TLS library, and a `scoped` custom-validator callback (§28.14) is the principled replacement, deferred to §8. A handshake that fails validation raises `CertificateValidationException` (§7).

### 4.4 `TcpListener` (for 0002)

```bezel
public class TcpListener: IDisposable              // SENDABLE, internally synchronized (§3)
  // Binds a listening socket. SocketException on bind failure (address in use,
  // permission), mirroring FileStream's throwing constructor (stdlib §7.2).
  public constructor(host: string, port: int, backlog: int := 128);

  // Await the next inbound connection, produced owned on the CALLING domain
  // (Pattern B). Several domains may Accept concurrently (kernel-serialized);
  // each gets a distinct connection.
  public method Accept(token: CancellationToken := CancellationToken.None): OwningTask<TcpConnection>;

  public readonly property LocalPort: int;           // the bound port (useful when port := 0)
  public readonly property IsClosed: bool;

  destructor;   // close the listening socket; see disposal-while-accepting below
end TcpListener;
```

**Accept loop shape** (single-domain form; the multi-worker form is §3):

```bezel
using var listener := new TcpListener("0.0.0.0", 8080);
using var cts := new CancellationTokenSource();
while not cts.IsCancellationRequested do
  using var conn := await listener.Accept(cts.Token);   // owned; this domain
  Task.FireAndForget(Serve(conn));                       // or await, per the topology
end;
```

**Ownership of accepted connections.** `Accept` is `OwningTask<TcpConnection>`: the accepting domain owns the connection, disposes it (`using var`), and — being domain-affine — keeps it. `LocalPort` lets `port := 0` (ephemeral) callers learn the assigned port.

**Disposal while an accept is pending.** Two clean paths:
- **Graceful (recommended):** cancel the `token` passed to `Accept`. The pending task completes with `OperationCanceledException` (spec [§30.8](Bezel_Language_Spec.md#sec-cancellation)); the loop exits and disposes the listener normally. This is the shape 0002 `Stop()` should drive.
- **Abrupt:** disposing the `TcpListener` while an `Accept` is outstanding closes the listening socket; the pending task **faults with `SocketException`** (`SocketError.Closed`), mirroring how disposing a `Channel<T>` faults a pending `Receive` with `ChannelClosedException` ([§30.15](Bezel_Language_Spec.md#sec-channels)). Defined, not undefined.

### 4.5 `Dns` and address representation

```bezel
public static class Dns
  // Resolve a host name to its textual addresses (v1 = strings, §4.5). Order is
  // the resolver's; callers try them in turn. NameResolutionException on failure.
  public static async method Resolve(
      host: string,
      token: CancellationToken := CancellationToken.None): OwningTask<Array<string>>;
end Dns;
```

**Addresses are `string` in v1** — dotted-quad IPv4 (`"93.184.216.34"`) or textual IPv6 (`"2606:2800:220:1:248:1893:25c8:1946"`). `TcpConnection.Connect` and `TcpListener` accept either form *and* names, resolving internally, so `Dns` is only needed for *explicit* resolution. A dedicated `IPAddress`/`IPEndPoint` value type is **deferred** (§8) for the same reason 0001 defers `Uri`: parsing, normalization, family and scope-id handling is a swamp worth its own proposal. Strings are sendable (so a resolved address list crosses domains freely) and sufficient for v1.

## 5. Async model: signatures, timeout, cancellation, back-pressure, buffers

### 5.1 Everything blocking is `async` and completes onto its domain

Per 0001 §5.1 and spec [§30.13](Bezel_Language_Spec.md#sec-runtime-model): the runtime runs a nonblocking-socket event loop on runtime threads and completes `OwningTaskCompletionSource`-backed tasks (spec [§30.11](Bezel_Language_Spec.md#sec-taskcompletionsourcet-and-ownedtaskcompletionsourcet)) onto the domain that issued the operation. No API call blocks a continuation; there is no synchronous `Socket.Receive`. This is the whole reason the domain-affinity ruling (§3) is safe: the completion always targets the issuing domain, which for a domain-affine connection is its one owning domain.

### 5.2 Timeout model — minimal, deadline via linked CTS

v1 keeps 0001's "per-phase timeouts are deferred" posture. The one per-operation timeout worth a dedicated parameter is **connect** (`Connect`/`TlsClientStream.Connect` take `timeout: TimeSpan`), because at connect time the caller has no stream yet to hang a `using`-scoped deadline on, and DNS + handshake is the phase that most often hangs. Exhausting it raises `TimeoutException` (stdlib [§6.1](Bezel_Standard_Library.md#sec-cancellationtoken-and-cancellationtokensource)).

**Read/Write take only a `CancellationToken`.** Deadlines on ongoing I/O are expressed with the machinery the language already ships — a linked, auto-cancelling source (stdlib §6.1, v0.173):

```bezel
// "read, but give up 5s from now, or if the caller cancels":
using var deadline := CancellationTokenSource.CreateLinked(
    callerToken,
    CancellationTokenSource.WithCancelAfter(TimeSpan.FromSeconds(5)).Token);
var n := await stream.ReadAsync(buf, 0, buf.Count, deadline.Token);
```

This is deliberate: the substrate does **not** re-invent a per-op `timeout:` on every read/write. `WithCancelAfter` + `CreateLinked` *is* the deadline mechanism, and using it keeps timeout (`TimeoutException`) and cancellation (`OperationCanceledException`) as the two distinct, already-drawn outcomes (§7). Per-phase timeouts (connect/TLS/first-byte/total, got-style) remain deferred to a v2 that designs them once for client and substrate together.

### 5.3 Buffer ownership — the `Array<byte>` windowed form and owned-chunk results

Reads and writes move bytes through the **existing** `Stream` surface (§7.1); the substrate invents no buffer type. Two forms, matching stdlib precedent:

- **Windowed `Array<byte>`** (the canonical async form): `ReadAsync(buffer: Array<byte>, offset, count, token): Task<int>`. Per the §7.1 v0.165 windowed-form contract, the window `[offset, offset+count)` ranges over the buffer's `Count`, `Read` writes into *existing* elements and never grows the array, and the canonical loop pre-sizes its buffer (`new Array<byte>(64 * 1024, 0)`). The substrate uses this as its primary read/write shape.
- **Owned-chunk result** for "give me the next bytes, sized": the substrate does **not** add a bespoke chunk API — 0001's `HttpBodyStream.ReadChunkAsync(...): OwningTask<Array<byte>>` already models the sized-owned-result pattern one layer up, and the substrate's job is only to fill a caller's buffer. Higher layers build owned-chunk iterators over `ReadAsync` as 0001 already does.

**Spans across `await` — an inherited, unresolved tension, not one this proposal introduces.** `Stream` (§7.1) *already* declares `ReadAsync(buffer: Span<byte>, token)` / `WriteAsync(ReadOnlySpan<byte>, token)`, and `NetworkStream` inherits them. 0001 §8 Q6 flags that a `Span<T>` is a borrow-bearing record ([§4.14](Bezel_Standard_Library.md#sec-owned-buffers-and-spans), spec §9.7) and that *"no rule admits one across an async suspension."* That question lives at the `Stream` level, predates this proposal, and belongs to the concrete-stream lane (0006) and the Stream owners — the substrate neither resolves nor widens it. **v1 recommends the `Array<byte>` windowed form for all async socket I/O** and inherits the span overloads without adding new span-async surface. Re-flagged in §9.

### 5.4 Back-pressure is the kernel's

TCP has native flow control: a slow reader shrinks the advertised window and the sender's kernel buffer fills. `WriteAsync` reflects this directly — it completes when the kernel accepts the bytes, and suspends (runtime `EPOLLOUT`) when the send buffer is full, resuming when it drains. `ReadAsync` returns what has arrived. There is no user-visible buffering knob, no high/low-water-mark API in v1; the socket *is* the bounded buffer. (This is the same shape as the bounded-channel back-pressure of [§30.15](Bezel_Language_Spec.md#sec-channels)/stdlib §6.9 — an awaiting producer resumes when space frees — so it composes with the async machinery and adds no new suspension concept.)

## 6. TLS: the runtime-internal sans-IO seam

**Backend: OpenSSL 3.x** (owner ruling, 2026-07-04), wrapped runtime-internally behind a narrow **sans-IO** seam so the backend stays swappable and the Bezel scheduler owns every byte of real socket traffic. The decline-with-reasons table is §6.3.

### 6.1 The sans-IO shape

OpenSSL is fed and drained through **memory BIOs** (`BIO_s_mem`), never a real file descriptor. The TLS engine is a pure state machine over two byte queues:

```
   plaintext  ── bz_tls_wrap ─────►  outbound-BIO  ──► [scheduler does the real socket write]
   app data                                              (NetworkStream.WriteAsync, §5)

   [scheduler does the real socket read] ──► inbound-BIO ── bz_tls_unwrap ──►  plaintext
   (NetworkStream.ReadAsync, §5)                                                app data
```

The runtime's async scheduler drives the loop: it calls the handshake/wrap/unwrap entry points; whenever an entry point signals "I have bytes to send" it drains the outbound BIO and performs the *real* nonblocking socket write on the connection's domain; whenever an entry point signals "I need bytes" it performs the *real* nonblocking read, feeds the inbound BIO, and steps again. **OpenSSL never sees a socket.** This is what makes the seam swappable (rustls and GnuTLS implement the same contract; §6.3) and what keeps all real I/O — hence all domain-affinity and cancellation — in the Bezel runtime where §3 needs it.

### 6.2 The entry points (~8 `bz_tls_*`)

A future implementation lane implements this contract (C++ over OpenSSL; **no C++ is written here** — this is the interface spec):

| Entry point | Role |
|---|---|
| `bz_tls_ctx_new(config) -> ctx` | Create a client TLS context: load the system trust store, set min version and ALPN list from `TlsOptions`. Shared across connections. |
| `bz_tls_ctx_free(ctx)` | Release the context. |
| `bz_tls_conn_new(ctx, sni_host) -> conn` | Per-connection state: two memory BIOs + SNI/verification name. |
| `bz_tls_handshake_step(conn) -> {WantRead, WantWrite, Done, Error}` | Advance the handshake using only the memory BIOs (sans-IO). The scheduler pumps the BIOs between steps per the status. |
| `bz_tls_wrap(conn, plaintext[], out enc_produced) -> n` | Encrypt app plaintext; the ciphertext lands in the outbound BIO for the scheduler to send (the write pump). |
| `bz_tls_unwrap(conn, out plaintext[]) -> n` | Decrypt whatever ciphertext the scheduler has fed into the inbound BIO into app plaintext (the read pump); `0` = need more inbound bytes. |
| `bz_tls_shutdown(conn) -> {WantWrite, Done}` | Emit `close_notify` into the outbound BIO (best-effort on dispose). |
| `bz_tls_peer_cert(conn, out der[]) -> n` | Query the negotiated peer certificate (for `PeerCertificatePem` / future pinning). |
| `bz_tls_last_error(conn) -> {code, text}` | Extract the failure for the typed `TlsException`/`CertificateValidationException` (§7). |
| `bz_tls_conn_free(conn)` | Release per-connection state. |

That is ten named entries; the load-bearing core the gap plan calls "~8" is `ctx_new/free`, `handshake_step`, `wrap`, `unwrap`, `shutdown`, `peer_cert`, `last_error` — `conn_new`/`conn_free` are the obvious lifecycle bookends. Certificate-chain and hostname verification are OpenSSL's (`SSL_set1_host` + `SSL_VERIFY_PEER` against the default trust store), surfaced as `Error` → `CertificateValidationException`. The seam is symmetric: a future **server** TLS context (0002 §6's deferred TLS termination) adds a server-side `bz_tls_ctx_new_server` and reuses `handshake_step`/`wrap`/`unwrap` unchanged.

### 6.3 TLS backend — decline-with-reasons (owner ruling recorded)

| Backend | Verdict | Reasons |
|---|---|---|
| **OpenSSL 3.x** | **Chosen** (owner, 2026-07-04) | Packaged as the system TLS on Fedora (CLAUDE.md's system-library toolchain posture); ubiquitous and battle-tested; clean sans-IO expression via `BIO_s_mem` memory BIOs; system trust store for free via OpenSSL defaults; **OpenSSL 3.5 ships QUIC**, a forward-compat path for a future HTTP/3 (§8). Stable-enough C ABI for the narrow `bz_tls_*` seam. |
| **rustls** | **Named second** — adopt if TLS-layer memory-safety becomes a priority | Memory-safe TLS core is the attraction. Blocked today: `rustls-ffi` is **not packaged on Fedora**; pulling it adds Rust-toolchain build debt (against CLAUDE.md's dependency-debt caution); and it needs a **separate trust-store verifier** (`rustls-native-certs`/webpki) rather than a built-in system store. Revisit when packaged and the toolchain cost is acceptable. |
| **GnuTLS** | **Runner-up** (fallback if OpenSSL entanglement arises) | Packaged on Fedora; sans-IO expressible via `gnutls_transport` push/pull callbacks. Declined as primary only because OpenSSL's ubiquity, QUIC path, and trust-store defaults edge it out; LGPL and the slightly less ergonomic pump keep it second. |
| **s2n-tls** | Declined | **Not packaged on Fedora** — would be a vendored dependency (dependency debt) for no capability OpenSSL lacks here. |
| **mbedTLS** | Declined | Embedded/small-footprint focus; narrower protocol/cipher coverage and less server-grade hardening than a general-purpose client wants. Its design target is not ours. |
| **BoringSSL** | Declined | Google **explicitly disclaims a stable ABI/API**; tracking it is continuous churn and it is not packaged as a stable system library. |
| **libtls (LibreSSL)** | Declined | Not the system TLS on Fedora (packaging/availability is the blocker → vendoring); the otherwise-pleasant `tls_*` API is also less sans-IO-friendly than memory BIOs. |

System trust store: OpenSSL defaults (`SSL_CTX_set_default_verify_paths`), no bundled CA store to keep current.

## 7. Error taxonomy

The substrate defines the **transport** faults; 0001's client-level table (its §3.5) sits above and either lets these propagate or wraps them (§7 reconciliation note). All are typed, catchable, and rooted under `IOException` (stdlib [§7.5](Bezel_Standard_Library.md#sec-io-exception-types), itself `: Exception`, spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)) so that `catch IOException` is the honest "any I/O — file or socket — failed" umbrella. All require `import System.Net;` (as IO exceptions require `import System.IO;`). They sit in **band 4 of the failure-signal rule** (stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule), v0.229): an *expected* environmental condition (refused, reset, DNS-miss) whose consumption site is a boundary, supervision, or retry site is a distinct catchable type, producer-local per spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), not a string to match. Band 3's value-like-payload conjunct fails here in any case — the payload is inseparable from an owned acquisition (`Connection`), [#2255](https://github.com/s8lap/bezel-lang/issues/2255) — so the exception spelling is what the rule *returns*, not a default (0076 §5's table and R6; graduation-checklist obligation [#2338](https://github.com/s8lap/bezel-lang/issues/2338)).

```
Exception (spec §31.4)
└─ IOException (stdlib §7.5)
   ├─ SocketException                    — base transport fault; carries `Error: SocketError`
   │   ├─ ConnectionRefusedException     — TCP connect actively refused
   │   ├─ ConnectionResetException       — peer sent RST mid-stream
   │   ├─ HostUnreachableException       — no route to host / network down
   │   └─ NameResolutionException        — DNS lookup failed  (the type 0001's table names)
   └─ TlsException                       — TLS handshake / protocol failure
       └─ CertificateValidationException — peer chain or hostname rejected by default validation

  reused as-is:
   TimeoutException            (stdlib §6.1)  — connect `timeout:` budget exhausted
   OperationCanceledException  (spec §31.4)   — a supplied `token` fired
```

```bezel
public class SocketException: IOException          // requires import System.Net
  public readonly property Error: SocketError;
end SocketException;

public enum SocketError: byte
  Refused, Reset, Unreachable, HostNotFound, TimedOut, AddressInUse, Closed, Other;
end SocketError;

public class ConnectionRefusedException: SocketException  end ConnectionRefusedException;
public class ConnectionResetException:  SocketException   end ConnectionResetException;
public class HostUnreachableException:  SocketException   end HostUnreachableException;
public class NameResolutionException:   SocketException   end NameResolutionException;

public class TlsException: IOException  end TlsException;
public class CertificateValidationException: TlsException  end CertificateValidationException;
```

The named subtypes are the small set callers actually branch on (got-style, as 0001 §3.5 argues); `SocketException.Error` carries a machine-readable reason for the long tail without a subclass per errno. The inheritance is arm-ordering-relevant (spec §31.4): a `catch SocketException` arm matches all four socket subtypes; a `catch TlsException` matches the certificate case.

**Timeout ≠ cancellation** stays the drawn line (stdlib §6.1, as 0001): `timeout:` raises `TimeoutException`; the caller's `token` raises `OperationCanceledException`. Faults never cross domains as objects — a spawned worker's transport fault surfaces at its join as `DomainFaultedException` carrying copied strings ([§30.14](Bezel_Language_Spec.md#sec-multiple-domains)), and caught exceptions obey the §31.1 arm-borrow rule; nothing here needs to be sendable.

**Reconciliation with 0001's table (coordination note, not an edit to 0001).** 0001 §3.5 lists `NameResolutionException` and `TlsException` in its HTTP table and roots its HTTP faults under a new `HttpException`. Intended relationship: those two are **the substrate types defined here**, which the client lets propagate; 0001's `HttpConnectException` is the *client-level* wrapper whose `CauseChain` snapshot (spec §31.4) carries the underlying `SocketException`. When the substrate lands, 0001's table should cite `System.Net` for `NameResolutionException`/`TlsException` rather than re-declaring them. Flagged as an open coordination point (§9), since amending 0001 is out of this proposal's scope.

## 8. What we deliberately defer (v1 stays small)

Each with its one-line reason; none blocks v1's client/server acceptance test:

- **UDP / datagram sockets** — a different ownership and framing story (no connection, no stream); its own small proposal.
- **`IPAddress`/`IPEndPoint` value types** — parsing/normalization/family/scope-id is a swamp, exactly as 0001 defers `Uri`; strings cover v1 and are sendable.
- **IPv6-specific API** — v1's string addresses already carry either family; scope-ids, dual-stack (`v6only`) toggles, and happy-eyeballs ordering are deferred (Connect tries the resolver's order).
- **QUIC / HTTP/3** — transport-version work above TCP; noted **forward-compat**: OpenSSL 3.5 ships QUIC, so the chosen backend does not foreclose it.
- **Server-side TLS termination** — 0002 §6 already defers it; the §6 seam is symmetric and reserves the server-context entry point.
- **Custom certificate validation / pinning / client certificates (mTLS)** — the validator is a `scoped` callback (spec [§28.14](Bezel_Language_Spec.md#sec-scoped-callback-parameters)) and deserves that design, not a v1 `bool`; validation stays default-on.
- **Socket options** (`TCP_NODELAY`/Nagle, `SO_KEEPALIVE`, buffer sizes, linger) — a tuning surface; v1 uses sane defaults, `Flush()` is a no-op.
- **Fine-grained half-close / `Shutdown(how)`** — v1 exposes only `ShutdownSend` (FIN on the write side); read-side and both-side shutdown are deferred.
- **Connection pooling** — 0001 §5 places the pool in the runtime per domain, *above* this substrate; the substrate hands out raw connections, the pool composes over them.
- **`Dns` reverse lookup / SRV / MX records** — v1 is forward `Resolve` only.
- **Unix-domain sockets, raw sockets** — out of the TCP/TLS v1 scope.

Composition seams left open on purpose: **0006's concrete stream family** (`StreamReader`/`StreamWriter`/`CopyToAsync`, gap-plan D1 #1) composes over `NetworkStream`/`TlsClientStream` with no change here — the substrate implements only the `Stream` base of §7.1; and **0001/0002** compose over `TcpConnection`/`TcpListener`/`TlsClientStream` per §2.

## 9. Open questions

1. **Domain-affinity ruling (§3)** — proposed: transport domain-local, `TcpListener` sendable. Confirm this over the alternative (make `TcpConnection` sendable with runtime completion re-homing). The proposal argues the affine ruling is safer, weaker on the runtime, and consistent with 0001 §4.4 — but it is a genuine choice and the load-bearing one.
2. **Non-seekable fault (§4.2) — RESOLVED (owner, 2026-07-04): keep `InvalidOperationException`; `NotSupportedException` DECLINED.** The §13/§31.4 exception taxonomy stays deliberately small; non-seekable `Seek`/`Length`/`Position` raise `InvalidOperationException` with a message naming the reason (`"NetworkStream is not seekable"`), exactly as §4.2 drafted. No new stdlib type is added.
3. **Span-across-`await` (§5.3)** — inherited from `Stream` §7.1's async span overloads and 0001 §8 Q6; the substrate recommends the `Array<byte>` form and does not resolve it. Owned by the Stream/0006 lane — but does the substrate want to *hide* the inherited span overloads (override them to reject) until the spec rules, or inherit them silently?
4. **Listener sendability mechanism (§3)** — "compiler-known sendable + internally synchronized, kernel-serialized `Accept`" is proposed as the `Channel<T>`-analogue addition to §30.14's list. Confirm the runtime can promise kernel-serialized concurrent accept on every target (epoll/kqueue: yes; IOCP `AcceptEx`: needs a small accept-pool shim — acceptable?).
5. **`TlsClientStream` owns its transport (§4.3)** — owned-transport chosen over borrowing a `NetworkStream`. Confirm; the borrowing form is not offered in v1.
6. **0001 taxonomy re-homing (§7)** — `NameResolutionException`/`TlsException` move to `System.Net`; `HttpConnectException` wraps `SocketException` via `CauseChain`. Confirm this is how 0001's §3.5 table should read once the substrate lands (an amendment to 0001, tracked separately).
7. **Address strings vs. a type (§4.5)** — strings for v1, `IPAddress` deferred. Confirm strings are acceptable through the first real programs (the same evidence-first bar 0001 sets for `Uri`).

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

The §9 open questions are resolved as follows. The §3 domain-affinity classifications graduate into §30.14's compiler-known list when the substrate lands.

- **Q1 — transport is domain-affine, `TcpListener` is sendable: confirmed.** `TcpConnection`/`NetworkStream`/`TlsClientStream` are domain-local (not sendable, §30.14), matching the live-async-resource family the section already excludes; `TcpListener` is compiler-known sendable and internally synchronized (kernel-serialized concurrent `Accept`, the `Channel<T>`-analogue). This is the weaker runtime contract and coheres with 0001 §4.4's domain-local layer above. The alternative (sendable connection + runtime completion re-homing) is declined — the listener-sendable route dissolves the one cross-domain need (accept here, handle there) with nothing moved.
- **Q2 — non-seekable fault: already RESOLVED (owner, 2026-07-04) — keep `InvalidOperationException`; `NotSupportedException` declined.** Recorded in §4.2/§9.
- **Q3 — span-across-`await`: inherit the `Stream` span overloads silently; do not override-to-reject.** The tension is inherited from §7.1, not introduced here, and it is owned by the Stream/0006 lane; v1 recommends the `Array<byte>` windowed form for all async socket I/O and adds no new span-async surface. The question is moot once the #1438 span-overload removal (ruled 2026-07-04) reaches the docs.
- **Q4 — listener-sendability mechanism: acceptable, including the IOCP accept-pool shim.** epoll/kqueue give kernel-serialized concurrent accept directly; Windows IOCP `AcceptEx` needs a small runtime accept-pool shim, which is an acceptable implementation cost for the sendable-listener capability.
- **Q5 — `TlsClientStream` owns its transport: confirmed.** Owned `TcpConnection` collapses the whole stack into one `using var` and removes the leave-open question; the borrowing form is not offered in v1.
- **Q6 — 0001 taxonomy re-homing: confirmed.** `NameResolutionException`/`TlsException` are the `System.Net` types defined here, which the client lets propagate; 0001's `HttpConnectException` wraps `SocketException` through its `CauseChain` snapshot. 0001's §3.5 table cites `System.Net` for the two shared types once the substrate lands (an amendment to 0001, tracked separately).
- **Q7 — address strings for v1: confirmed.** Strings (dotted-quad / textual IPv6, plus names) are sendable and sufficient; a dedicated `IPAddress`/`IPEndPoint` type is deferred with `Uri` (0001), to be revisited against the first real programs.

**Supersession note.** #980 (the earlier standalone socket/DNS request) is **closed as superseded** by this networking stack (0001/0002/0007); this substrate is the settled answer it asked for.

## 10. Relationship to existing work

- **0001 §5** — the sketch this proposal fills in; §3/§5/§6/§7 settle the six constraints 0001 deferred (sendability, timeout, buffers, TLS backend, accept surface, taxonomy). 0001's `Http` pool and default client sit above §4.
- **0002 §4.1** — the worker-domain model is §3's acceptance test; the sendable `TcpListener` is what makes topology (A) handoff-free. 0002 §6's deferred TLS termination reuses the §6 seam.
- **Stdlib gap plan (2026-07), Wave D1** — this is the "System.Net substrate" row; the TLS-backend owner ruling (OpenSSL 3.x behind the sans-IO seam) is recorded here in full per that plan's instruction.
- **0006 (concrete stream family, gap-plan D1 #1)** — composes over the `Stream` base (§7.1) this substrate implements; the seam is designed against §7.1 only, assuming none of 0006's additions.
- **spec §30.13/§30.14/§30.15** — the runtime-model, sendability, and channel rules the whole proposal is built on; §3 proposes additions to §30.14's compiler-known-types list (graduating with the substrate).
- **spec §30.11 (`OwningTaskCompletionSource`)** — the completion machinery every async operation here is built on, exactly as 0001 §5.1 and §9 note.
- **stdlib §6.1 (`CancellationToken`, linked CTS, `TimeoutException`)** — the deadline/cancellation mechanism §5.2 leans on instead of per-op timeout parameters.
- **stdlib §7.1 (`Stream`) / §7.5 (IO exceptions)** — the base contract `NetworkStream`/`TlsClientStream` implement and the root the §7 taxonomy extends.
