# Proposal 0002 — The `Http` server

| | |
|---|---|
| **Status** | accepted (2026-07-24, delegated ruling #1402 — the four §7 open questions ruled; drafted proposal-text amendments in #1402's record apply at graduation) |
| **Depends on** | Proposal 0001 (client), proposal 0007 (`System.Net` substrate), stdlib §7 Stream family (#1394/#645), timers (#1378), proposal 0003 (guarded shared state) |
| **Design influences** | ASP.NET **Minimal APIs** and **Express** for the routing surface (the two independently converged on the same shape); WHATWG fetch for the `Request → Response` handler symmetry; **not** full ASP.NET (no controllers, no DI container, no middleware pipeline in v1), **not** raw Node `http` (streams-and-callbacks is the wrong level, and callbacks fit serial-per-domain async poorly) |
| **Bezel-native leverage** | Handlers run on worker **domains**; the §30.14 sendability rules make data-race freedom a *compile-time* property of the server — the marquee thing neither Express nor ASP.NET can promise |

## 1. Design principles

1. **A handler is a function from request to response.** `(req: HttpRequest) -> OwningTask<HttpResponse>` — the service-worker shape. No controller classes, no reflection-discovered actions, no magic parameter binding. What you register is what runs.
2. **Route + verb registration, minimal-API style.** `app.Get("/users/{id}", handler)`. Express and ASP.NET Minimal APIs converged here from opposite directions; it is the empirically friendly surface.
3. **Symmetric vocabulary with 0001.** The server's `HttpResponse` is the same type the client receives; `HttpHeaders` is shared; body accessors (`Text`/`Bytes`/`Json<T>`) read the same on an incoming server request as on a client response, including the §29.11 single-consumption rule.
4. **Concurrency = domains, checked by the compiler.** The server runs a small pool of worker domains; each builds its own routing table locally from a **capture-free route builder** (§30.14 admits it as a spawn entry — a static method reference or a non-capturing lambda). Because the builder runs *on* each worker, the handlers and handler objects it creates are born there and never cross a domain boundary — a function value is never sendable (§30.14), so there is no other legal way to place a handler on a worker. State genuinely shared across workers is reachable only through the tools the language already sanctions for cross-domain state: §22.12 synchronized statics, or a `Channel<T>` to a state-owning domain. Reaching unsynchronized shared state from a handler is a *compile error naming the failing sendability rule*, not a 3 a.m. incident.
5. **Defined faults, closed by default.** A handler that throws produces a logged `500` with the §33.3-style report on the server side and an opaque body on the wire. Malformed requests produce `400` before any handler runs. There is no way to accidentally stream an exception message to a client.

## 2. Tour by example

```bezel
module Sample;
import System.Net.Http;

record Todo(Id: int, Title: string, Done: bool);

class Api
  // The route builder is capture-free: it is run once per worker domain to
  // construct that worker's own routing table and handler instances (§30.14
  // spawn-entry rules — §4.1). Registered here, run there.
  public static method Routes(app: HttpApp)
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
  end Routes;

  public static async method Main(): Task<int>
    // Listen binds on the accept domain, spawns the worker pool, and builds
    // each worker's routing table by invoking `Api.Routes` locally on it.
    return await HttpApp.Listen(port := 8080, routes := Api.Routes);   // runs until Stop()/signal
  end Main;
end Api;
```

### 2.1 Handlers as ordinary named methods (no lambdas required)

The handler parameter is just a function type, and Bezel already converts method references to function values (spec §27.6) — so endpoint bodies can be normal methods, registered by name inside the route builder. No controllers, no attributes, no reflection: registration stays explicit code.

Because the route builder runs **on each worker domain** (§4.1), any handler objects it constructs are worker-local — born there, never crossing a boundary. State that must be shared across workers lives where §22.12 sanctions cross-domain state: a synchronized static, or an actor addressed by `Channel<T>`.

```bezel
class TodoApi
  // A worker-local instance, constructed inside the route builder. Its store is
  // a process-global synchronized static (§22.12) — the cross-domain-legal home
  // for state every worker shares. `Store.Todos` is a `static Locked<...>`
  // (proposal 0003); the handler reaches it directly.
  public async method GetById(req: HttpRequest): OwningTask<HttpResponse>
    var id := int.Parse(req.Param("id"));
    using var g := await Store.Todos.LockAsync();
    return case g.Value.TryGetValue(id, out var todo) of
      true => Http.Json(todo!),
      _    => Http.Status(404)
    end;
  end GetById;

  public async method Create(req: HttpRequest): OwningTask<HttpResponse>
    var todo := await req.Json<Todo>();
    using var g := await Store.Todos.LockAsync();
    g.Value.Add(todo.Id, todo);
    return Http.Json(todo, status := 201);
  end Create;
end TodoApi;

// The route builder — capture-free, run once per worker (§4.1). It constructs
// this worker's own handler objects and registers their methods; nothing here
// crosses a domain boundary.
public static method Routes(app: HttpApp)
  var api := new TodoApi();               // worker-local instance
  app.Get("/todos/{id}", api.GetById);    // bound reference to a worker-local object
  app.Post("/todos", api.Create);
  app.Get("/health", Health.Check);       // static-method reference — capture-free
end Routes;
```

A **bound** instance reference (`api.GetById`) is a function value — never sendable across domains (§30.14). What makes it legal here is that `api` is constructed *inside* `Routes`, which runs on the worker: the reference and its receiver are worker-local from birth and are never asked to cross. This is the endpoint-grouping story under the domain model: a plain class whose per-worker instance reaches shared state only through synchronized statics or a channel — checked at the point that shared state is touched, not hand-waved. Route groups (`app.Group("/todos")`) remain deferred (§6) as pure sugar over the builder.

Things to notice:

- `Http.Text(...)`, `Http.Json(...)`, `Http.Status(404)` are the response **constructors** — the server-direction mirror of the client's response *accessors*. One `HttpResponse` type, two directions, exactly like the web-standard `Response`.
- The route builder is a static method reference (`Api.Routes`, `Routes`) — capture-free, hence a legal §30.14 spawn entry the runtime replays on every worker. The moment the *builder itself* captures a mutable local, the compiler rejects it with the standard §30.14 diagnostic.
- `Listen` is the only blocking point and it is `await`ed — there is no `Run()`/`RunAsync()` split and no sync surface (per #1362, nothing in this API may suggest `Wait()`).

## 3. Proposed API surface

### 3.1 `HttpApp` — routing (per worker) and lifecycle (static)

`HttpApp` is the per-worker routing table: the runtime constructs one instance per worker domain and hands it to the capture-free route builder, which registers routes on it. Lifecycle (bind/serve/stop) is static — it lives on the accept domain, not on any per-worker instance.

```bezel
public class HttpApp
  // Route registrations — called by the per-worker route builder (§4.1), on the
  // worker domain, against that worker's own instance. Users do not construct
  // HttpApp directly; the runtime builds one per worker and passes it in.
  // Route syntax: literal segments, {param} captures, {*rest} tail capture.
  // Longest-literal-prefix wins; ambiguous registrations are an ArgumentException
  // at build time, not at request time.
  public method Get(route: string, handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Post(route: string, handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Put(route: string, handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Delete(route: string, handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Patch(route: string, handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Route(verb: string, route: string, handler: (HttpRequest) -> OwningTask<HttpResponse>);

  // Fallbacks. NotFound: no route matched (default: empty 404).
  // OnError: a handler threw (default: log §33.3-style report, opaque 500).
  // The error hook receives the ExceptionInfo SNAPSHOT (§31.4), not the live
  // exception — the arm-lifetime rule (§31.1) applies to server internals too.
  public method NotFound(handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method OnError(handler: (HttpRequest, ExceptionInfo) -> OwningTask<HttpResponse>);

  // Lifecycle — static, on the accept domain. Listen binds, spawns `workers`
  // domains, and builds each worker's routing table by invoking the CAPTURE-FREE
  // `routes` builder locally on it (a legal §30.14 spawn entry: a static method
  // reference or a non-capturing lambda; a capturing builder is rejected at the
  // call site). It then accepts and dispatches until Stop() is called (or the
  // process receives SIGINT/SIGTERM, which the server translates to Stop()).
  // Returns the exit code to hand to Main. In-flight handlers get `drainTimeout`
  // to finish; then connections close.
  public static async method Listen(
      port: int,
      routes: (HttpApp) -> (),
      host: string := "127.0.0.1",
      workers: int := 0,                            // 0 = core count
      drainTimeout: TimeSpan := TimeSpan.FromSeconds(10)): Task<int>;
  public static method Stop();                       // idempotent, sendable
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
  public async method Bytes(): OwningTask<Array<byte>>;
  public async method Json<T>(): OwningTask<T> where T: IDeserializable;
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

`Listen` binds the socket on the accept domain and spawns `workers` domains. Each worker domain runs the capture-free `routes` builder locally as its first action — the runtime replays the same builder on every worker, exactly the shape §30.14 admits for a spawn entry (a static method reference or non-capturing lambda; **the v0.179 async spawn entries** let the accept-loop and per-worker serve-loop themselves be ordinary `async` domain entries, awaited within their domain). The builder constructs that worker's own routing table and handler objects; the accept loop parses each request and hands it to a worker, whose handler task runs to completion there (serial within the domain, parallel across domains — §30.13). Consequences the compiler enforces:

- **The route builder must be capture-free** (§30.14). This is the single check site: a builder that captures a mutable local is rejected at the `Listen` call. Handlers and handler objects are constructed *inside* the builder, on the worker, so they are never function values crossing a boundary — which §30.14 forbids outright (function-typed values are never sendable).
- **Cross-worker shared state** is reachable only through the §22.12-sanctioned forms: `Atomic<T>`/`AtomicInteger<T>` statics, a `Mutex`- or `Locked<T>`-guarded static (proposal 0003), or a `Channel<T>` to a dedicated state-owning domain (the actor idiom). The README example for v1 should show the channel/actor form — it is the one that scales.

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
- **Raw stream req/res objects** (Node `http`): the level below `Text`/`Bytes`/`Json` is the `System.Net` substrate (proposal 0007), which stays available for the 1% who need it.
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
- **§30.13/§30.14/§22.12**: the concurrency story rests on existing language surface — the capture-free per-worker route builder (§4.1) is a legal §30.14 spawn entry, and cross-worker state uses the §22.12 synchronized-statics / channel forms. The **v0.179** async spawn entries are what let the accept and serve loops be ordinary awaited `async` domain entries. No new *language* spec surface is required for v1; the dependencies this proposal does carry are external components (the 0007 substrate, the Stream family #1394/#645, and proposal 0003 for guarded shared state), not language changes.
- **#1394/#645 (Stream), #1378 (timers), #703 (serialization)**: sequencing dependencies noted inline.
