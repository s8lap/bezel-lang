# Proposal 0002 — The `Http` server

| | |
|---|---|
| **Status** | Draft for owner review |
| **Depends on** | Proposal 0001 (client + `System.Net` substrate), stdlib §7 Stream family (#1394/#645), timers (#1378) |
| **Design influences** | ASP.NET **Minimal APIs** and **Express** for the routing surface (the two independently converged on the same shape); WHATWG fetch for the `Request → Response` handler symmetry; **not** full ASP.NET (no controllers, no DI container, no middleware pipeline in v1), **not** raw Node `http` (streams-and-callbacks is the wrong level, and callbacks fit serial-per-domain async poorly) |
| **Bezel-native leverage** | Handlers run on worker **domains**; the §30.14 sendability rules make data-race freedom a *compile-time* property of the server — the marquee thing neither Express nor ASP.NET can promise |

## 1. Design principles

1. **A handler is a function from request to response.** `(req: HttpRequest) -> Task<owned HttpResponse>` — the service-worker shape. No controller classes, no reflection-discovered actions, no magic parameter binding. What you register is what runs.
2. **Route + verb registration, minimal-API style.** `app.Get("/users/{id}", handler)`. Express and ASP.NET Minimal APIs converged here from opposite directions; it is the empirically friendly surface.
3. **Symmetric vocabulary with 0001.** The server's `HttpResponse` is the same type the client receives; `HttpHeaders` is shared; body accessors (`Text`/`Bytes`/`Json<T>`) read the same on an incoming server request as on a client response, including the §29.11 single-consumption rule.
4. **Concurrency = domains, checked by the compiler.** The server dispatches requests across a small pool of worker domains. A handler must therefore be **sendable-capturing** (§30.14): capture-free, or capturing only sendable/synchronized state. Shared mutable state in a handler is a *compile error pointing at the capture*, not a 3 a.m. incident. Cross-request state flows through the §22.12 synchronized statics or a `Channel<T>` — the tools the language already has.
5. **Defined faults, closed by default.** A handler that throws produces a logged `500` with the §33.3-style report on the server side and an opaque body on the wire. Malformed requests produce `400` before any handler runs. There is no way to accidentally stream an exception message to a client.

## 2. Tour by example

```bezel
module Sample;
import System.Net.Http;

record Todo(Id: int, Title: string, Done: bool);

class Api
  public static async method Main(): Task<int>
    var app := new HttpApp();

    app.Get("/health", (req) => Http.Text("ok"));

    app.Get("/todos/{id}", async (req) => do
      var id := int.Parse(req.Param("id"));       // FormatException → 400 (§4.3)
      var todo := new Todo(id, "write the server proposal", false);
      return Http.Json(todo);                     // 200, application/json
    end);

    app.Post("/todos", async (req) => do
      var todo := await req.Json<Todo>();          // single-consumption body
      return Http.Json(todo, status := 201);
    end);

    return await app.Listen(port := 8080);         // runs until Stop()/signal
  end Main;
end Api;
```

Things to notice:

- `Http.Text(...)`, `Http.Json(...)`, `Http.Status(404)` are the response **constructors** — the server-direction mirror of the client's response *accessors*. One `HttpResponse` type, two directions, exactly like the web-standard `Response`.
- The handlers are capture-free lambdas, so they are trivially sendable and the app can run them on any worker domain. The moment a handler captures a mutable local, the compiler rejects the registration with the standard §30.14 diagnostic.
- `Listen` is the only blocking point and it is `await`ed — there is no `Run()`/`RunAsync()` split and no sync surface (per #1362, nothing in this API may suggest `Wait()`).

## 3. Proposed API surface

### 3.1 `HttpApp` — routing and lifecycle

```bezel
public class HttpApp
  public constructor();

  // Verb registrations. Route syntax: literal segments, {param} captures,
  // {*rest} tail capture. Longest-literal-prefix wins; ambiguous registrations
  // are an ArgumentException at registration time, not at request time.
  public method Get(route: string, handler: (HttpRequest) -> Task<owned HttpResponse>);
  public method Post(route: string, handler: (HttpRequest) -> Task<owned HttpResponse>);
  public method Put(route: string, handler: (HttpRequest) -> Task<owned HttpResponse>);
  public method Delete(route: string, handler: (HttpRequest) -> Task<owned HttpResponse>);
  public method Patch(route: string, handler: (HttpRequest) -> Task<owned HttpResponse>);
  public method Route(verb: string, route: string, handler: (HttpRequest) -> Task<owned HttpResponse>);

  // Fallbacks. NotFound: no route matched (default: empty 404).
  // OnError: a handler threw (default: log §33.3-style report, opaque 500).
  // The error hook receives the ExceptionInfo SNAPSHOT (§31.4), not the live
  // exception — the arm-lifetime rule (§31.1) applies to server internals too.
  public method NotFound(handler: (HttpRequest) -> Task<owned HttpResponse>);
  public method OnError(handler: (HttpRequest, ExceptionInfo) -> Task<owned HttpResponse>);

  // Lifecycle. Listen binds, accepts, and dispatches until Stop() is called
  // (or the process receives SIGINT/SIGTERM, which the server translates to
  // Stop()). Returns the exit code to hand to Main. In-flight handlers get
  // `drainTimeout` to finish; then connections close.
  public async method Listen(port: int, host: string := "127.0.0.1",
                             workers: int := 0,            // 0 = core count
                             drainTimeout: TimeSpan := TimeSpan.FromSeconds(10))
      : Task<int>;
  public method Stop();                                     // idempotent, sendable
end HttpApp;
```

### 3.2 `HttpRequest` — the incoming request (borrowed by the handler)

```bezel
public class HttpRequest
  public readonly property Method: string;        // "GET", canonical upper
  public readonly property Path: string;          // decoded path, no query
  public readonly property Headers: HttpHeaders;  // shared type from 0001

  public method Param(name: string): string;      // route capture; KeyNotFoundException
                                                  //   if the route declared no such param
  public method Query(name: string): string?;     // first query value or null
  public method QueryAll(name: string): Array<string>;

  // Body — single-consumption (§29.11), identical contract to 0001's response
  // accessors. Second read: InvalidOperationException.
  public async method Text(): Task<string>;
  public async method Bytes(): Task<owned Array<byte>>;
  public async method Json<T>(): Task<owned T> where T: IDeserializable;
end HttpRequest;
```

The handler **borrows** the request; the server owns it and reclaims it after the handler's task completes. Nothing about a request may escape the handler (the same §31.1-style lifetime posture as catch arms) — buffer what you need.

### 3.3 Response construction — `Http.*` statics (mirror of 0001 §3.3)

```bezel
// All return owned HttpResponse; status/headers as named args on every form.
Http.Text(body: string, status: int := 200, contentType: string := "text/plain; charset=utf-8");
Http.Json<T>(value: T, status: int := 200) where T: ISerializable;
Http.Bytes(owned body: Array<byte>, status: int := 200, contentType: string := "application/octet-stream");
Http.Status(status: int);                          // empty body
Http.Redirect(location: string, permanent: bool := false);   // 302 / 301
```

`HttpResponse` itself is 0001's type; the server serializes `Status`, `Headers`, and the body. A handler may also set headers before returning: `var r := Http.Json(todo); r.Headers.Set("ETag", tag); return r;`.

## 4. Ownership, domains, faults

### 4.1 The worker-domain model

`Listen` spawns `workers` domains. The accept loop parses each request and dispatches it to the least-loaded worker; the handler's task runs to completion on that domain (serial within the domain, parallel across domains — §30.13). Consequences the compiler enforces:

- Handler closures must satisfy the **Domain.Spawn capture rules** (§30.14): sendable captures only. Registration is the check site.
- Cross-request shared state is only reachable through the §22.12-sanctioned forms: `Atomic<T>`/`AtomicInteger<T>` statics, `Mutex`-guarded statics, or a `Channel<T>` to a dedicated state-owning domain (the actor idiom). The README example for v1 should show the channel/actor form — it is the one that scales.

### 4.2 Response ownership is a Pattern-B handoff

The handler returns `owned HttpResponse`; the server is the single consumer (serializes then disposes). A handler that constructs a response and throws before returning leaks nothing — the §28.3 unwind disposal covers the local, and the server's `OnError` path builds its own response.

### 4.3 Fault taxonomy

| Condition | Result |
|---|---|
| No route matched | 404 via `NotFound` hook |
| Malformed request line/headers, oversize headers | 400, connection closed; handler never runs |
| `Param`/`int.Parse`/`Json<T>` failure inside a handler | it is an ordinary exception → `OnError` → 500 (or the hook's response). v2 may add a typed `BadRequest` short-circuit; v1 keeps one path. |
| Handler throws | `OnError` (default: log + opaque 500) |
| Client disconnects mid-handler | handler runs to completion; the write is discarded (run-to-completion is the domain model's own rule — cancellation-token plumbing is deferred) |

## 5. What we deliberately do not copy

- **Middleware pipelines** (ASP.NET) / **`app.use` chains** (Express): the extension mechanism drags in ordering lore, per-request mutable context bags, and ownership questions about the in-flight request. Deferred as one design together with 0001's client hooks — they are the same feature.
- **Controllers, model binding, DI, filters** (ASP.NET): reflection-shaped; Bezel deliberately has no member-access reflection (rationale §18).
- **Raw stream req/res objects** (Node `http`): the level below `Text`/`Bytes`/`Json` is the `System.Net` substrate (0001 §5), which stays available for the 1% who need it.
- **Ambient per-request context** (`HttpContext.Current`, AsyncLocal): anti-sendable by construction; state travels in arguments.

## 6. Deferred (v2+, rough priority)

Middleware/hooks (shared design with 0001) · TLS termination (cert config on `Listen`; the substrate work is 0001 §5's) · static-file serving · WebSocket upgrade · streaming/chunked response bodies (needs the Stream span work, #1394) · request cancellation tokens tied to disconnect · route groups + typed route parameters · compression · HTTP/2.

## 7. Open questions for the owner

1. **Worker count semantics** — `workers: 0 = core count` proposed; alternative is an explicit required number (no magic). Preference?
2. **`Json<T>` on `ISerializable`/`IDeserializable`** — this rides the #703 type-driven serialization migration; if that lands first, these bounds are already right, otherwise v1 ships `Text`/`Bytes` only and `Json<T>` follows the migration.
3. **Default bind host** — `127.0.0.1` (safe-by-default, containers must opt into `0.0.0.0`) vs `0.0.0.0` (what servers usually want). Proposed: loopback default; the diagnostic when a container health-check fails should mention the flag.
4. **Signal handling** — `Listen` translating SIGINT/SIGTERM to `Stop()` requires the process/OS surface (a stdlib gap tracked by the gap review); acceptable to ship v1 with `Stop()` only and wire signals when that lands?

## 8. Relationship to existing work

- **0001**: shares `HttpResponse`, `HttpHeaders`, the error taxonomy roots, and the substrate; the fetch symmetry is why the two read as one API.
- **#1376**: the umbrella networking issue; this proposal is its server half.
- **§30.13/§30.14/§22.12**: the concurrency story is 100% existing language surface — this proposal adds no spec changes.
- **#1394/#645 (Stream), #1378 (timers), #703 (serialization)**: sequencing dependencies noted inline.
