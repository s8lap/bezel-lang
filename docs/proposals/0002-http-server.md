# Proposal 0002 — The `Http` server

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-24, delegated ruling); re-drafted 2026-09-06 on the v0.294 §28.14 trace (owner-accepted in session) |
| **Tracking issue** | [#1402](https://github.com/s8lap/bezel-lang/issues/1402) |
| **Target module** | `System.Net.Http` (proposal 0001's module) |
| **Written against** | spec/stdlib **v0.294** |
| **Depends on** | Proposal 0001 (client; shared `HttpResponse`/`HttpHeaders`/`HttpMethod`/`Http`), proposal 0007 (`System.Net` substrate — `TcpListener`), stdlib §7 Stream family (#1394/#645; proposal 0006), timers (#1378). `Locked<T>` is stdlib §6.11 (proposal 0003 graduated v0.186 — no longer a dependency). The tour additionally uses proposal 0021 (`ILogger`/`LogHub`) and proposal 0022 (`System.Testing`) as *example* material only; nothing in §3 depends on either. |
| **Design influences** | ASP.NET **Minimal APIs** and **Express** for the routing surface (the two independently converged on the same shape); WHATWG fetch for the `Request → Response` handler symmetry; the stdlib §6.5 event pattern for the registration shape (`_clicked += OnClicked` is `_routes.Get("/x", OnX)` with a route in front); **not** full ASP.NET (no controllers, no DI container, no middleware pipeline in v1), **not** raw Node `http` |
| **Bezel-native leverage** | Handlers run on worker **domains**; §30.14 sendability makes data-race freedom a *compile-time* property of the server. Registration is **ownership-shaped**: a service owns its `RouteTable`, so a `this`-bound handler is admitted by §28.14's form 4 exactly as an event handler is — the same rule that makes an event subscription sound makes a route registration sound, and the table's teardown severs every handler. |
| **Redraft note** | Rewritten 2026-09-06 against three defects in the 2026-07-24 text. (1) `HttpApp.Get(route, handler)` declared a *plain* function parameter and stored it — §28.14's call-bounded default (v0.177) forbids storing a plain function-typed parameter anywhere, so the table could not hold a handler at all. (2) The route builder received the table as a *parameter* (`Routes(app: HttpApp)`), and a bounding object that is a parameter admits capture-free callbacks only (§28.14), so `app.Get("/todos/{id}", api.GetById)` — a method reference whose capture is its receiver `api` — did not compile even with `scoped` on the parameter. (3) `lambda(req) => Http.Text("ok")` infers `owned HttpResponse` at an `OwningTask<HttpResponse>` slot, and no §5.1 conversion bridges the two (§22.6 *Lambda-literal arguments*), so the sync-lambda showcase was a type error. Tour simplified 2026-09-09 (owner-directed): a generic `Store<T>` takes the lock protocol out of every handler, the factories and the duplicate resource class go, and the tour's `Count + 1` id minting — which could overwrite a live key after a delete — is replaced by minting under the guard. |

## 1. Design principles

1. **A handler is a function from request to response, and it is async.** `(HttpRequest) -> OwningTask<HttpResponse>` — the service-worker shape, always a task. A real handler awaits a lock, a body, or a store; the spelling for the rare synchronous one is `async lambda(req) => Http.Text("ok")` (§30.6). No controllers, no reflection-discovered actions. What you register is what runs.
2. **Registration is ownership-shaped.** A service *owns* its route table (`private owned readonly _routes: RouteTable := new();`) and registers its own methods on it in its constructor: `_routes.Get("/{id}", GetById)`. That call's receiver chain is `this._routes`, one `owned`-field hop, so §28.14 form 4 resolves the bounding object to `this` and admits a `this`-bound method reference — the stdlib §6.5 event pattern's trace, verbatim. The table's disposal severs the handlers; nothing outlives what it captures.
3. **Symmetric vocabulary with 0001.** The server's `HttpResponse` is the type the client receives; `HttpHeaders` and `HttpMethod` are shared; body accessors (`Text`/`Bytes`/`Json<T>`/`JsonOwned<T>`) read the same on an incoming request as on a client response, including the §29.11 single-consumption rule. `Http.*` gains response *constructors*, the server-direction mirror of the client's *accessors*.
4. **Concurrency = domains, checked by the compiler.** `HttpServer.Listen` spawns worker domains and constructs one service object *on each* from a capture-free factory — a §30.14 spawn position, so a handler is born on its worker and never asked to cross a boundary (function values are never sendable). What a worker needs from the composition root arrives as one **sendable deps object per worker**, moved through the spawn (stdlib §6.8's `(owned A) -> owned R` shape); state shared across workers rides inside it as `Shared<Locked<…>>` handles (§28.13's payload set). Reaching unsynchronized shared state from a handler is a compile error naming the failing sendability rule.
5. **Binding without reflection; faults closed by default.** `req.Param<int>("id")` is ordinary §11.8 static-interface dispatch over `IParsable<T>` (stdlib §2.19) — no typed route patterns, no attribute binding. A malformed parameter or body raises the one server exception, `HttpResponseException(400, …)`, and the innermost dispatcher converts it to the response — stdlib §1's failure-signal rule, band 4: the dispatcher *is* the protocol boundary. Anything else a handler throws becomes an opaque 500 with a server-side report; nothing streams an exception message to a client.
6. **Testable without sockets.** A service is an ordinary object; `new HttpRequest(HttpMethod.Get, "/7")` plus `await service.Routes.Dispatch(req)` runs the full auth → match → handler path in-process, with no port, no domain, no `Listen`.

## 2. Tour by example

Two resources, seven endpoints, a forty-line generic `Store<T>` that hides the shared map's lock behind one-line calls, a per-worker deps object carrying a store and a logger, `Main`, and two tests. Written to be extrapolated: fifty endpoints is about eight of these resource classes, each a top-to-bottom read over the one `Store<T>`.

```bezel
module TodoService;
import System.Net.Http;
import System.Threading;          // Locked<T> (stdlib §6.11); Shared<T> is auto-imported (stdlib §2.16)
import System.Diagnostics;        // ChannelLogger/LogHub/ConsoleSink/LogLevel (0021 §3)

// ── Wire shapes: value records, serialization synthesized (§9.8) ──
public record Todo(Id: int, Title: string, Done: bool): ISerializable, IDeserializable;
public record NewTodo(Title: string): ISerializable, IDeserializable;

// ── A store of value records keyed by int, shared across workers. Ordinary user code: one owned handle
//    (§28.13) around a data-owning lock (stdlib §6.11); every guard is acquired last and never held across
//    an `await`. Sendable at every instantiation (§30.14's class rule: sealed, one `owned` sendable field).
public sealed class Store<T> where T: value, sendable
  private owned readonly _map: Shared<Locked<Dictionary<int, T>>>;

  public constructor()
    _map := new Shared<Locked<Dictionary<int, T>>>(new Locked<Dictionary<int, T>>(new Dictionary<int, T>()));
  end constructor;
  private constructor(owned map: Shared<Locked<Dictionary<int, T>>>)  _map := map;  end constructor;
  public method Clone(): owned Store<T> => new Store<T>(_map.Clone());     // a second handle to the same map

  public async method All(): Task<Array<T>>                                // a snapshot, copied under the guard
    using var g := await _map.Get().LockAsync();                           // Pattern B guard (§30.5); the scope is the critical section
    return g.Value.Values.ToArray();
  end All;

  public async method Find(id: int): Task<T?>
    using var g := await _map.Get().LockAsync();
    return g.Value.GetOrNull(id);                                          // a copy, or null (stdlib §4.8)
  end Find;

  public async method Add(make: (int) -> T): Task<T>                       // the id is minted under the guard
    using var g := await _map.Get().LockAsync();
    var id := (g.Value.Keys.MaxOrNull() ?? 0) + 1;                         // never collides with a live key
    var item := make(id);
    g.Value[id] := item;
    return item;
  end Add;

  public async method Update(id: int, f: (T) -> T): Task<T?>               // null ⇒ absent; read-modify-write is atomic
    using var g := await _map.Get().LockAsync();
    var cur := g.Value.GetOrNull(id);
    if cur = null then return null; end;                                   // `cur` is a local: it narrows (§7.4)
    var next := f(cur);
    g.Value[id] := next;
    return next;
  end Update;

  public async method Remove(id: int): Task<bool>
    using var g := await _map.Get().LockAsync();
    return g.Value.Remove(id);
  end Remove;
end Store;

// ── Per-worker dependencies. Sendable by §30.14's class rule: sealed; every field an `owned` sendable class. ──
public sealed class Deps
  public owned readonly Todos: Store<Todo>;
  public owned readonly Log: ChannelLogger;                                // one per worker (0021 §3)
  public constructor(owned todos: Store<Todo>, owned log: ChannelLogger)  Todos := todos;  Log := log;  end constructor;
end Deps;

// ── The root service: owns the deps, mounts the resources, carries the cross-cutting policy ──
public sealed class Api: IHttpService
  private owned readonly _deps: Deps;
  private owned readonly _routes: RouteTable := new();                     // LAST owned field — see §4.1
  public property Routes: RouteTable => _routes;                           // IHttpService: a receiver-sourced borrow (§28.6)

  public constructor(owned deps: Deps)
    _deps := deps;                                                          // [R0]
    _routes.Around(Guard);                                                  // [R1] wraps every route below and every mounted child
    _routes.Get("/health", async lambda(req) => Http.Text("ok"));           // [R2] capture-free: admitted at any bounding object
    _routes.Mount("/todos", new TodoApi(_deps.Todos));                      // [R3] the child borrows the store; `_deps` outlives it
  end constructor;

  // Auth + one access-log line for the whole tree. `next` is call-bounded: invoke it, await it, never store it.
  private async method Guard(req: HttpRequest,
      next: (HttpRequest) -> OwningTask<HttpResponse>): OwningTask<HttpResponse>
    if req.Path <> "/health" and req.Headers.Get("Authorization") = null then
      return Http.Status(401);
    end;
    var resp := await next(req);                                            // [A1] awaited to settlement
    _deps.Log.Info($"{HttpMethod.NameOf(req.Method)} {req.Path} -> {resp.Status}");
    return resp;                                                            // owned local through an OwningTask return: marker-free
  end Guard;
end Api;

// ── One resource class: six one-line handlers over the store it borrows from the root ──
public sealed class TodoApi: IHttpService
  private readonly _todos: Store<Todo>;                                    // back-reference (§28.3): the root's `_deps` outlives this child
  private owned readonly _routes: RouteTable := new();                     // LAST
  public property Routes: RouteTable => _routes;

  public constructor(todos: Store<Todo>)
    _todos := todos;
    _routes.Get   ("/",          List);                                     // [T1]
    _routes.Get   ("/{id}",      GetById);                                  // [T2]
    _routes.Post  ("/",          Create);                                   // [T3]
    _routes.Put   ("/{id}",      Replace);                                  // [T4]
    _routes.Patch ("/{id}/done", MarkDone);                                 // [T5]
    _routes.Delete("/{id}",      Remove);                                   // [T6]
  end constructor;

  private async method List(req: HttpRequest): OwningTask<HttpResponse>
    return Http.JsonArray(await _todos.All());                              // Pattern A: the snapshot dies with the statement (§30.5)
  end List;

  private async method GetById(req: HttpRequest): OwningTask<HttpResponse>
    return Http.JsonOrNotFound(await _todos.Find(req.Param<int>("id")));   // "/abc" ⇒ 400 before any state is touched; null ⇒ 404
  end GetById;

  private async method Create(req: HttpRequest): OwningTask<HttpResponse>
    var body := await req.Json<NewTodo>();                                  // malformed body ⇒ 400
    var todo := await _todos.Add(lambda(id) => new Todo(id, body.Title, false));
    return Http.Json(todo, status := 201);
  end Create;

  private async method Replace(req: HttpRequest): OwningTask<HttpResponse>
    var id := req.Param<int>("id");
    var body := await req.Json<Todo>();
    if body.Id <> id then throw new HttpResponseException(400, "body id does not match the route"); end;
    return Http.JsonOrNotFound(await _todos.Update(id, lambda(_) => body));
  end Replace;

  private async method MarkDone(req: HttpRequest): OwningTask<HttpResponse>
    return Http.JsonOrNotFound(await _todos.Update(req.Param<int>("id"), lambda(t) => t with(Done := true)));
  end MarkDone;

  private async method Remove(req: HttpRequest): OwningTask<HttpResponse>
    return Http.Status((if await _todos.Remove(req.Param<int>("id")) then 204 else 404));
  end Remove;
end TodoApi;

// ── Composition root (D-0001: constructor wiring, no container) ──
public static class Program
  public static async method Main(): Task<int>
    await using var hub := new LogHub(new ConsoleSink(), minLevel := LogLevel.Info);   // 0021 §4.2
    var pump := hub.RunAsync();                                             // the sink loop, on this domain
    using var todos := new Store<Todo>();                                   // one map, one handle per worker (§28.13)
    // `deps` runs on THIS domain, once per worker, and each result is moved into worker i (§30.14).
    // It may capture Main's locals: `Listen` is awaited to settlement in this very statement (§28.14 frame residence).
    var code := await HttpServer.Listen(port := 8080,
        service := lambda(owned d: Deps) => new Api(move d),                // [F] capture-free: a spawn position
        deps := lambda(i) => new Deps(todos.Clone(), hub.NewLogger($"http-{i}")));   // [M1]
    await pump;      // every worker's ChannelLogger died with its service ⇒ channel closed ⇒ sink drained
    return code;
  end Main;
end Program;
```

The tests: no sockets, no domains, no `Listen`. A resource class is constructed over a `Store<Todo>` the test owns and seeds, and dispatched directly — the path is the service's own (`/{id}`, not `/todos/{id}`, because the `/todos` prefix belongs to the root's `Mount`).

```bezel
module TodoService.Tests;
import TodoService;
import System.Testing;            // 0022 §5, §8 — explicit import by design (0022 §5.6)
import System.Net.Http;

public class TodoApiTests
  public static async method GetByIdReturnsTheTodo(): Task
    using var store := new Store<Todo>();                                   // fixture = using var (0022 §7)
    var todo := await store.Add(lambda(id) => new Todo(id, "write the server proposal", false));
    using var api := new TodoApi(store);                                    // the service borrows the store, which outlives it
    using var req := new HttpRequest(HttpMethod.Get, $"/{todo.Id}");        // dispatched directly: the service's own path
    using var resp := await api.Routes.Dispatch(req);                       // Pattern B
    Assert.Equal(200, resp.Status);
    Assert.Equal(todo, await resp.Json<Todo>());                            // an all-value positional record is hashable (§9.1, §3.6)
  end GetByIdReturnsTheTodo;

  public static async method NonNumericIdIsA400(): Task
    using var store := new Store<Todo>();
    using var api := new TodoApi(store);
    using var req := new HttpRequest(HttpMethod.Get, "/not-a-number");
    using var resp := await api.Routes.Dispatch(req);
    Assert.Equal(400, resp.Status);                                         // Param<int> ⇒ HttpResponseException(400) ⇒ converted
  end NonNumericIdIsA400;
end TodoApiTests;

public static class TestMain
  public static async method Main(): Task<int>                              // 0022 §3.2 (b): the registration runner
    using var runner := new TestRunner();
    runner.Add("GetByIdReturnsTheTodo", TodoApiTests.GetByIdReturnsTheTodo);   // static references: capture-free, admitted
    runner.Add("NonNumericIdIsA400",    TodoApiTests.NonNumericIdIsA400);
    return await runner.RunAsync();
  end Main;
end TestMain;
```

Things to notice:

- Every registration line has the same shape — `_routes.<Verb>(route, Method)` — and the same ownership trace (§4.1): receiver `this._routes`, bounding object `this`, capture `this`. The registrations live in the constructor because that is where the object is being assembled, not because the rule requires it: anywhere `this._routes` is the receiver, the trace is identical. The one lambda (`/health`) captures nothing and is admitted at any bounding object; `async lambda(req) => …` is the spelling for a handler with nothing to await.
- The children are **owned by the table** (`Mount(prefix, owned child)`). The one borrow a child holds — its `Store<Todo>` — is sourced from the root's `_deps`, which is declared *before* `_routes` and is therefore disposed *after* it (§28.4 step 2, reverse declaration order). That ordering is the whole discipline: **later fields borrow from earlier ones**, and `_routes` — the field whose disposal tears down everything that borrows — goes last.
- `Http.Text(…)`, `Http.Json(…)`, `Http.JsonArray(…)`, `Http.JsonOrNotFound(…)`, `Http.Status(404)` are the response **constructors**. A handler that wants a header sets it with the indexer before returning: `var r := Http.Json(todo); r.Headers["ETag"] := tag; return r;` (0001 §3.4's indexer-set; there is no `Set`).
- `Listen` is the only blocking point and it is `await`ed — no `Run()`/`RunAsync()` split, no sync surface (#1362). The factory (`lambda(owned d: Deps) => new Api(move d)`) is a non-capturing lambda: capture-free, hence admitted at a spawn position (a static method reference is the other admitted spelling); the moment it captures, the compiler rejects it with the §30.14 spawn-entry diagnostic.
- `Store<T>` is ordinary user code — no compiler-known part, forty lines, reusable by every resource. It keeps the lock protocol in one place: each method acquires its guard with its *last* `await` and does synchronous work under it, so no guard is ever live across a suspension (stdlib §6.11's lint has nothing to fire on), and a handler reads as one `return Http.…(await _todos.…)` line. `All()` returns `Task<Array<T>>` — Pattern A — precisely so that `Http.JsonArray(await _todos.All())` is admitted (§30.5: a Pattern B await may not sit in a plain argument position). Its `where T: value, sendable` is what lets `Locked<Dictionary<int, T>>` be instantiated in the body: §14.2 makes `T` count as sendable there, §30.14 makes a borrowing collection of sendable values sendable, §28.11 makes a non-move-only value a copy slot, and stdlib §6.11 admits that interior — `sendable` is not redundant beside `value`, since `value` alone admits the move-only kinds §28.11 rejects as elements. Ids are minted under the guard (`Keys.MaxOrNull() ?? 0` plus one), so a delete never makes the next `Add` overwrite a live key.
- If a future `route` modifier lands (§11), each `[T]` line becomes a modifier on the method it names and the constructor loses those lines; nothing else changes.

## 3. Proposed API surface

Everything below is **NEW** unless it carries a citation. `HttpMethod`, `HttpHeaders`, `HttpResponse` and `Http` are 0001's; the server extends `Http` with constructors and asks one clause of 0001 §3.4 to narrow (§9c).

```bezel
module System.Net.Http;

// ── Shared with 0001 (cited) ───────────────────────────────────────────────────────────────
public enum HttpMethod: byte  Get, Head, Post, Put, Patch, Delete, Options, Trace; end HttpMethod;   // 0001 §3.5
public sealed class HttpHeaders /* … */ end HttpHeaders;   // 0001 §3.4 — indexer get/set, Get, GetAll, Add, Remove, ContainsKey. No `Set`.
public class HttpResponse /* … */ end HttpResponse;   // 0001 §3.3 — Status, Ok, Headers, Text/Bytes/Json<T>/JsonOwned<T> (single-consumption, §4.3 there)
   // Narrowed clause on 0001 §3.4 (§9c): a response CONSTRUCTED by `Http.*` has writable Headers until it is sent
   // (or read back in a test); a RECEIVED response's Headers stay a read-only view. `r.Headers["ETag"] := tag;` is the spelling.

// ── The incoming request: borrowed by handlers, domain-local, disposed by whoever constructed it ──
public class HttpRequest                                                                                     // NEW
  public constructor(verb: HttpMethod, path: string, body: string := "",                // the test seam (§7 Q8);
                     contentType: string := "", headers: HttpHeaders? := null);          //   headers are copied in
  public readonly property Method: HttpMethod;        // the enum, not a string — 0001 symmetry
  public readonly property Path: string;              // decoded, no query; ALWAYS the full path, even under Mount
  public readonly property Headers: HttpHeaders;      // read-only view (borrow)
  public method Param(name: string): string;                                  // route capture; unknown name ⇒ ArgumentException (band 1)
  public method Param<T>(name: string): owned T where T: IParsable<T>;        // T.Parse (§11.8; stdlib §2.19); any Parse fault ⇒ HttpResponseException(400, …, cause)
  public method Query(name: string): string?;                                 // first value; null = absent (band 2)
  public method Query<T>(name: string): owned T? where T: IParsable<T>;       // null = absent; malformed ⇒ HttpResponseException(400)
  public method QueryAll(name: string): owned Array<string>;
  public async method Text(): Task<string>;                                   // single-consumption — 0001 §4.3's contract
  public async method Bytes(): OwningTask<Array<byte>>;
  public async method Json<T>(): Task<T> where T: IDeserializable;            // compiler-known, the 0001 §3.3 / stdlib §2.15 split (#1402):
  public async method JsonOwned<T>(): OwningTask<T> where T: class, IDeserializable;   //   value T ⇒ Json, class T ⇒ JsonOwned;
                                                                              //   SerializationException ⇒ HttpResponseException(400, …, cause)
end HttpRequest;

// ── Response constructors on 0001 §3.1's facade: the server-direction mirror of 0001 §3.3's accessors ──
public static class Http                              // 0001 §3.1's class, extended — every member below is NEW
  public static method Text(body: string, status: int := 200, contentType: string := "text/plain; charset=utf-8"): owned HttpResponse;
  public static method Json<T>(value: T, status: int := 200): owned HttpResponse where T: ISerializable;        // stdlib §2.15's write path underneath
  public static method JsonArray<T>(items: IEnumerable<T>, status: int := 200): owned HttpResponse             // BeginArray / item.Serialize / EndArray:
      where T: ISerializable;                                                                                  //   no collection conforms to ISerializable (§7 Q5)
  public static method JsonOrNotFound<T>(value: T?): owned HttpResponse where T: ISerializable;                 // null ⇒ empty 404, else 200 JSON
  public static method Bytes(owned body: Array<byte>, status: int := 200, contentType: string := "application/octet-stream"): owned HttpResponse;
  public static method Status(status: int): owned HttpResponse;               // empty body: 204, 404, 401, …
  public static method Redirect(location: string, permanent: bool := false): owned HttpResponse;   // 302 / 301
end Http;

// ── The one server-side exception: "answer with this status" ──
public class HttpResponseException: HttpException     // NEW; root: 0001 §3.5; producer-local (§31.4)
  public constructor(status: int, message: string := "");   // `message` is server-side only: it reaches the
                                                            //   diagnostic channel, never the response body (§1 principle 5)
  public constructor(status: int, message: string, cause: Exception);   // snapshot chaining (§31.4) — Param/Json use it
  public readonly property Status: int;               // strings and values only (§31.1)
end HttpResponseException;

// ── What a service is: an object that exposes the table it owns ──
public interface IHttpService                                                                                // NEW
  readonly property Routes: RouteTable;               // the implementer's `owned` field, read through an expression-bodied property
end IHttpService;

// ── The per-worker table. Compiler-known: it STORES scoped handlers (§28.14's stdlib retention register, §9b) ──
public class RouteTable                                                                                      // NEW
  public constructor();                               // `private owned readonly _routes: RouteTable := new();` (§17.7)
  // Route syntax: literal segments, {name} captures, {*rest} tail capture. Longest literal prefix wins.
  // A duplicate verb+route, or a Mount prefix colliding with a literal route (either direction), is an
  // ArgumentException at the registering call (band 1) — at construction, never at request time.
  public method Get   (route: string, scoped handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Post  (route: string, scoped handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Put   (route: string, scoped handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Patch (route: string, scoped handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Delete(route: string, scoped handler: (HttpRequest) -> OwningTask<HttpResponse>);
  public method Route (verb: HttpMethod, route: string, scoped handler: (HttpRequest) -> OwningTask<HttpResponse>);
  // Composition: `prefix/…` is dispatched to `child` — its hooks, then its table, on the remaining path.
  // The child is owned here; the owned-field cascade (§28.5) disposes the tree bottom-up.
  public method Mount (prefix: string, owned child: IHttpService);
  // Cross-cutting, opt-in. The hook wraps this table's own routes AND every mounted child. AT MOST ONE hook
  // per table: a second `Around` on the same table is an ArgumentException at the registering call (band 1,
  // the duplicate-route rule). Nesting is by mount order — a parent's hook wraps its children's, outermost first.
  // `next` is CALL-BOUNDED inside the hook: invoke it, await it to settlement, never store or return the task.
  public method Around(scoped hook: (HttpRequest, (HttpRequest) -> OwningTask<HttpResponse>) -> OwningTask<HttpResponse>);
  // In-process dispatch — what the worker loop calls, and what a test calls. Borrows the request.
  // hooks ⇒ match ⇒ handler. HttpResponseException from a hook, a handler, or Param/Json ⇒ its status, converted at the
  // innermost table (so an enclosing hook sees a response); no route ⇒ empty 404; any other exception ⇒ propagates
  // through the hooks (catch it in one to answer through your ILogger) ⇒ the outermost Dispatch answers an opaque 500
  // and reports on the diagnostic channel.
  public async method Dispatch(req: HttpRequest): OwningTask<HttpResponse>;
  destructor;   // severs the handler and hook lists before reclamation (§28.14 quiescence, stated normatively as for
                //   EventSource — §9b); the owned-field cascade disposes mounted children
end RouteTable;

// ── Lifecycle: static, on the calling domain ──
public static class HttpServer                                                                               // NEW
  // Spawns `workers` domains (0 = Environment.ProcessorCount, #1402); on EACH, runs the capture-free `service`
  // factory — a §30.14 spawn position (§9a): a static method reference or a non-capturing lambda, the same
  // admission as a Domain.Spawn entry — and serves until Stop(). Returns the exit code for Main. In-flight
  // handlers get `drainTimeout`; then connections close.
  public static async method Listen<S>(port: int, service: () -> owned S,
      host: string := "127.0.0.1", workers: int := 0,
      drainTimeout: TimeSpan := TimeSpan.FromSeconds(10)): Task<int> where S: IHttpService;
  // The deps form. `deps` is a plain (call-bounded) parameter: it is invoked on the CALLING domain once per worker,
  // with i in 0..<workers, and each result — one sendable D — is moved into worker i as its spawn argument and
  // handed to `service` there (stdlib §6.8's `(owned A) -> owned R` shape, applied `workers` times).
  public static async method Listen<S, D>(port: int, service: (owned D) -> owned S, deps: (int) -> owned D,
      host: string := "127.0.0.1", workers: int := 0,
      drainTimeout: TimeSpan := TimeSpan.FromSeconds(10)): Task<int>
      where S: IHttpService where D: sendable, class;
  public static method Stop();                                                // idempotent; reachable from any domain (#1402)
end HttpServer;
```

Not in v1, each declined with reasons in §7 or deferred in §6: `HttpApp`; `NotFound`/`OnError` hooks (subsumed by `{*rest}` and `Around`); a sync handler overload; route-pattern typing `{id:int}`; `Before`/`After`; groups as a separate concept (a group *is* a mount); a `Register()` phase; a middleware pipeline.

Concept count: five new types (`IHttpService`, `RouteTable`, `HttpServer`, `HttpRequest`, `HttpResponseException`), one hook, seven response constructors, over 0001's shared vocabulary.

## 4. Ownership, domains, faults

### 4.1 The §28.14 trace of every registration line

§28.14 resolves a `scoped` call's **bounding object** by testing five forms in order — 1 constructor call → 2 `owned` return initializing a `using var` → 3 `owned` return into an `owned` field of `this` → 4 a receiver chain of `owned` fields rooted at `this` or a local → 5 the receiver. A capture-bearing argument is admitted iff the bounding object is a `using var` local declared after every captured source, or `this` with every capture `this`-derived. A capture-free argument is admitted at any bounding object. The verified argument forms are lambda literals and method references, whose capture is the receiver (v1 admission bound 3).

| Line | Call | Form → bounding object | Argument's captures | Verdict |
|---|---|---|---|---|
| [T1]–[T6] | `_routes.Get("/{id}", GetById)` etc. | receiver chain `this._routes`, one hop, an `owned` field → **form 4 → `this`** | an instance method reference: captures `this` | **admitted** — every capture is `this`. Identical to stdlib §6.5's `_clicked += OnClicked`. |
| [R2] | `_routes.Get("/health", async lambda(req) => Http.Text("ok"))` | **form 4 → `this`** | a lambda literal with no captures | **admitted** at any bounding object; the `async` lambda's inferred return is `OwningTask<HttpResponse>` (§30.6), equal to the slot (§7 Q3) |
| [R1] | `_routes.Around(Guard)` | **form 4 → `this`** — `Around` is a `scoped` position like `Get` | `Guard` captures `this` | **admitted** |
| [R3] | `_routes.Mount("/todos", new TodoApi(_deps.Todos))` | not a `scoped` position — `owned child` is an ordinary transfer (§28.8); the fresh `new` is the owned production and needs no marker (§22.3) | constructor argument: `_deps.Todos` is a bare read through two owned fields (lends, §28.6) into a plain `Store<Todo>` parameter, stored by the child as a **back-reference** (§28.3's borrowed-field store rule: parameter-sourced, admitted with the caller-side residual that the source outlives the child) | **admitted**; the residual is discharged by field order — below |
| [R0] | `_deps := deps` | owned parameter into an owned field: marker-free store (§28.3) | — | fine; `_deps` is definitely assigned at [R1]–[R4] (§7.9) |
| [A1] | `await next(req)` inside `Guard` | `next` is a **call-bounded** plain function parameter (§28.14's default). Callee side: it may be invoked, and the minted task must be consumed to settlement before the callee returns — the `await` does exactly that (*Call-bounded × deferred activation* (2)); `return next(req);` un-awaited, or storing the task, is rejected there as an escape sink. Caller side (the runtime invoking the hook): any closure is admitted | — | **admitted**. `next` is admissible as an `async` method's parameter: function types are runtime-managed and therefore not borrow-bearing (§9.7), so §30.1's borrow-bearing-parameter bar is not engaged |
| [F] | `lambda(owned d: Deps) => new Api(move d)` passed to `Listen`'s `service` | a **spawn position** (§30.14's category; §9a): a non-capturing lambda or static method reference | none — it names only types | **admitted**; the annotated parameter binds `D := Deps` in §14.5 phase 1 (modifier slot `owned` matching exactly, §27.1), and the lambda's inferred return `owned Api` binds `S := Api` in phase 2; `move d` is the ordinary marker on the lambda's own `owned` parameter (§28.1's `lambda(owned x: T)` form, §22.3) |
| [M1] | the `deps` lambda passed to `Listen` | a plain (call-bounded) parameter of an **async** callee → frame residence (§28.14): the lambda's captured sources — `todos` (`using var`), `hub` (`await using var`) — become carried sources of the returned task, bounded by its settlement (§30.1) | `todos`, `hub` — both locals of `Main`, all outliving the `await` in the same statement | **admitted**; each `Clone()`/`NewLogger()` result is a fresh producer into `Deps`'s `owned` parameters — marker-free. (Inference is over-determined, not marginal: phase 1 binds `D := Deps` off the service lambda's annotated parameter and phase 2 binds `S := Api` off its inferred return, and the `deps` lambda — whose one parameter slot is the closed `int` — independently unifies its inferred return `owned Deps` against `owned D`. Both bindings agree, so §14.5's *Agreement* rule is satisfied; `HttpServer.Listen<Api, Deps>(…)` remains writable and changes nothing.) |
| ctor-time capture | every [R]/[T]/[U] line | §22.7 rule 3: capturing `this` in a method reference is not a synchronous observation; rule 1's gate is not engaged — `_routes.Get(…)` has receiver `_routes`, and field reads are not observations; rule 2 is satisfied (the classes are `sealed`, `RouteTable.Get` is not virtual) | — | no construction-time hazard: no request is dispatched before `Listen` holds the service (§30.13 domain-serial) |
| test | `runner.Add(name, TodoApiTests.X)` | `scoped body` on a `using var` runner (0022 §8.1) | static method reference: capture-free | **admitted** unconditionally |
| test | `api.Routes.Dispatch(req)` | not a `scoped` position; `Routes` is an expression-bodied property returning a receiver-sourced borrow of `_routes` (§28.6), and `Dispatch` borrows `req` | — | fine — a getter hop disqualifies a chain only for *registration* (form 4); dispatch stores nothing |

**Why `_routes` is declared last — the disposal-order argument.** §28.4's generated `Dispose()` disposes owned-bearing fields in **reverse declaration order** and then chains to the base. A mounted child holds borrows sourced from the root's earlier fields (`_todos` ← `Api._deps.Todos`); the child is owned by `Api._routes`. With `_routes` declared *after* `_deps`, `_routes` is disposed first — the children die while `_deps` is intact — and `_deps` second. The §28.3 back-reference residual ("the source outlives the object") is thereby discharged **structurally**, by declaration order, with no runtime backstop needed. Had the table lived in a base class, the derived class's `_deps` would have been disposed *before* the base's `_routes` (§28.4: subclass fields, then the base chain), inverting the order — the ground on which a stdlib base class was declined (§7 Q1). The rule generalizes: **a later field may borrow from an earlier one, never the reverse**, and the field that owns the borrowers goes last. The residual is about the field's *current* value (§28.6 — a §28.7 overwrite or a `move` could end it early), and both `Api._deps` and `Deps.Todos` are `readonly`, so no overwrite can. A child that held a `Store<Todo>.Clone()` instead of a borrow would be indifferent to the order — the handle keeps the value alive (§28.13) — at the price of a `Clone()` on the `Mount` line and a handle of its own in every test; the borrow is the smaller spelling, and the field order is what makes it sound. A child that also logs takes `_deps.Log` as a second back-reference the same way.

**Why a service's registration bound is intact after `Mount`.** §28.14's *registration-lifetime residual* concerns moving the *storing member* out past its bounding object. At [R3] the whole bounding object (the child) moves, its `_routes` with it: the bound travels intact, and the child's table is disposed with the child, which the root's table disposes with the root.

**Handler-body ownership, once.** `req` is a plain parameter of an `async` method — a borrow held until settlement (§30.1); the dispatcher awaits the handler's task before the request is reclaimed, so the source-outlives-settlement obligation is discharged by the dominant shape. Inside `Store<T>`, `using var g := await _map.Get().LockAsync()` is Pattern B into a `using var` (§30.5); the `Get()` borrow is sourced from the owned handle field (§28.13) and rides `LockAsync`'s frame only within the awaiting full expression; the handlers hold no guard at all — `All()` is Pattern A so that `Http.JsonArray(await _todos.All())` is an admitted unbound-temporary await (§30.5), and every other store call returns a value. `Http.*` results are fresh owned productions returned through an `OwningTask` return, marker-free (§28.3). No guard is ever live across an `await` (stdlib §6.11's warning): the only guards in the program are `Store<T>`'s five, each acquired by the last `await` of its body, and every body-reading `await` in a handler precedes the store call.

### 4.2 The worker-domain model

`Listen` binds the listening socket on the calling domain (a 0007 §4.4 `TcpListener`, runtime-internal) and spawns `workers` domains. On each worker the runtime runs `service` — with the worker's own `D` moved in as the spawn argument, in the deps form — as the domain's first action, then accepts that worker's connections (0007 §3's topology: a connection is born on the domain that accepted it and never crosses), parses each request, and calls `service.Routes.Dispatch(req)`. Consequences the compiler enforces:

- **The factory must be capture-free** (§30.14). This is the single check site: a factory that captures a local is rejected at the `Listen` call with the spawn-entry diagnostic. Handlers are constructed *inside* the factory's object, on the worker, so they are never function values crossing a boundary — which §30.14 forbids outright.
- **What a worker receives is sendable by construction.** `D` is a sealed class whose fields are `owned` sendable classes (§30.14's transfer-closed rule); the stdlib's sendable carriers are `Shared<T>` for `T` in §28.13's payload set — `Mutex`, `Atomic<T>`, `AtomicInteger<T>`, `Semaphore`, `Locked<T>`, `RwLocked<T>`, `FrozenArray<T>`, `CancellationTokenSource` — plus `ChannelSender<T>` for the actor idiom (stdlib §6.9) and any sendable class. A worker that needs a *per-worker* resource (0021's `ChannelLogger`) owns it outright inside `D`; a worker that needs *shared* state holds a handle to it.
- **The service itself is never sendable** (it holds function values, §30.14 rule 3) and never needs to be: born on the worker, disposed on the worker when `Listen` returns.
- Cross-worker state may also live where §22.12 sanctions it — a synchronized static (`static owned readonly _todos: Locked<…>`, stdlib §6.11's registry idiom) — for genuinely ambient state; the deps object is preferred because a test can substitute it (§7 Q2).

### 4.3 Response ownership is a Pattern-B handoff

A handler returns `OwningTask<HttpResponse>`: a fresh, single-consumer resource whose disposal is connection hygiene (§30.5's producer rule). The dispatcher is that single consumer — it serializes, then disposes. A handler that constructs a response and then throws leaks nothing: §28.3's unwind disposal covers the local, and the dispatcher builds its own response for the fault. A `Task<HttpResponse>` slot would admit a *borrowed* response (a cached field) the dispatcher could neither take nor dispose — and for a class result the form is a compile error anyway.

### 4.4 Fault taxonomy

| Condition | Result |
|---|---|
| No route matched | empty 404 from the outermost table; a `{*rest}` route on the root table is the custom-404 hook |
| Malformed request line/headers, oversize headers | 400, connection closed; nothing user-authored runs |
| `Param<T>` / `Query<T>` / `Json<T>` / `JsonOwned<T>` cannot parse | `HttpResponseException(400, message, cause)` — snapshot-chained (§31.4) — converted by the **innermost** table's `Dispatch` into a 400 response; every enclosing `Around` sees a *response* and can log it |
| Unknown `Param` name | `ArgumentException` (band 1): a route/handler mismatch is a bug, not a client fault — propagates as below |
| A handler (or a helper beneath it) throws `HttpResponseException(s, …)` | a response with status `s` and an empty body (band 4: the dispatcher is the protocol boundary) |
| An `Around` hook throws `HttpResponseException(s, …)` (before or after `next`) | converted by the table the hook is registered on — the same conversion the handler row states, applied at the hook's own layer; an enclosing parent hook sees a response, not an exception |
| A handler throws anything else | propagates through the `Around` layers — a hook may `catch`, log through its `ILogger`, and `return Http.Status(500)` — else the outermost `Dispatch` answers an opaque 500 and writes a §33.3-style report to the diagnostic channel |
| A `Mount`/`Get` conflict | `ArgumentException` at the registering call, in the constructor, on the worker at `Listen` — the server never starts serving a broken table |
| Client disconnects mid-handler | the handler runs to completion; the write is discarded (run-to-completion is the domain model's rule; cancellation-token plumbing is deferred, §6) |

## 5. What we deliberately do not copy

- **Middleware pipelines** (ASP.NET) / **`app.use` chains** (Express): ordering lore, per-request mutable context bags, and ownership questions about the in-flight request. One `Around` hook per table, nesting as the only order, covers auth, logging, timing, and exception conversion for v1; a pipeline stays deferred as one design with 0001's client hooks (§6).
- **Controllers, model binding, DI, filters** (ASP.NET): reflection-shaped; Bezel has no member-access reflection (rationale §5.18). Composition is constructor parameters with ownership contracts (DECLINED.md D-0001).
- **Extractor-style signature binding** (Axum): compile-checked binding sugar that costs a trait-solver; ours is access-time `Param<T>`/`Query<T>` — explicit, greppable, and one line.
- **Raw stream req/res objects** (Node `http`): the level below `Text`/`Bytes`/`Json` is the `System.Net` substrate (0007), which stays available.
- **Ambient per-request context** (`HttpContext.Current`, AsyncLocal): anti-sendable by construction; state travels in arguments and in `D`.
- **A route-builder callback** (this proposal's own 2026-07-24 shape): a table received as a parameter admits capture-free handlers only (§28.14). The table is owned, not lent.

## 6. Deferred (v2+, rough priority)

Middleware/hooks pipeline (shared design with 0001 §7) · a `CancellationToken` on `Listen` succeeding the static `Stop()` (0007 §4.4 already says `Stop()` should drive `Accept`'s token) · TLS termination (cert config on `Listen`; the seam is 0007 §6's) · static-file serving · WebSocket upgrade · streaming/chunked response bodies (needs the Stream span work, #1394) · request cancellation tied to disconnect · a sync handler overload (§7 Q3: addable at no call-site cost) · `HEAD`/`OPTIONS` auto-handling · compression · HTTP/2 · signal handling (§8).

## 7. Decision records

**Q1 — Registration/ownership model: (a) a service object owning its `RouteTable`, exposed through `IHttpService.Routes`, registered against through the field.**
- *(a) chosen.* `private owned readonly _routes: RouteTable := new();` declared last, plus `public property Routes: RouteTable => _routes;`. Registration is `_routes.Get(route, Method)`: form 4, bounding object `this`, the stdlib §6.5 trace verbatim. Two lines per service beyond the registrations. The property hop is *not* a registration path (form 4 rejects getter hops), which is fine: registration goes through the field, dispatch through the property.
- *(b′) a stdlib base class `HttpService` owning the table* — declined. Same trace, one line fewer, but §28.4 disposes a derived class's fields before the base chain, so a base-owned table would dispose its mounted children *after* the derived class's dependency fields those children borrow from — the exact inversion §4.1 shows the field order avoiding. Single inheritance would also have been spent on a field.
- *(b) registration through base-class methods `Get(route, handler)` with receiver `this`* — declined with (b′), and additionally rests on a form-5-versus-form-4 reading for a bare `this` receiver that the field spelling does not need.
- *(c) handler objects `owned IHttpHandler` per endpoint* — no closures, no `scoped`, a user-authorable table; but fifty endpoints is fifty classes. Rejected on the concept-count goal; kept as the pattern for a handler that genuinely carries its own owned state.
- *(d) a `Register()` hook the runtime calls after construction* — same trace, one more lifecycle phase and abstract member for no soundness gain (nothing dispatches before `Listen`, §30.13). Declined; a future modifier's synthesis target is the constructor tail (§11).
- *Why the table is compiler-known:* a user-authored callee may store a `scoped` callback only in a direct `scoped` *field* whose RHS is the parameter or `null` (§28.14 callee side; v1 bound 5) — one callback per field, never a list. Storing many is a stdlib-certified position (`EventSource.Add`, `TaskScope.Spawn`, 0022's `TestRunner.Add`); `RouteTable` joins that register with its quiescence stated normatively (§9b).

**Q2 — Dependencies per worker: a sendable deps object per worker, minted by a factory on the calling domain; shared state inside it as `Shared<…>` handles.**
- *(i) synchronized statics only* — works for a store (stdlib §6.11's registry idiom) but cannot give each worker its own `ChannelLogger` (not synchronized; 0021 mints one per producer), and statics defeat the test seam. Kept for genuinely ambient state.
- *(ii) `deps: owned OwningArray<D>` built by a hand-written N-loop in `Main`* — sound, but the loop repeats `Environment.ProcessorCount` and the worker count at two sites. Replaced by *(ii′) `deps: (int) -> owned D`*: a plain call-bounded parameter of an async callee (frame residence, §28.14), invoked `workers` times on the calling domain; it may capture `Main`'s `using var` locals because `Main` awaits `Listen` to settlement in the same statement. Each result is moved into worker `i` (§30.14 spawn argument). Chosen.
- *(iii) what is sendable inside `D`:* `Shared<T>` for `T` in §28.13's payload set (`Mutex`, `Atomic<T>`, `AtomicInteger<T>`, `Semaphore`, `Locked<T>`, `RwLocked<T>`, `FrozenArray<T>`, `CancellationTokenSource`), `ChannelSender<T>`, and any sealed transfer-closed class. Not sendable, and not needed: the service.
- *0021 wiring under this design:* `Deps.Log: owned ChannelLogger`; the root owns `_deps`; a child holds back-references into it (the store; the logger too if it logs); `Main` holds no logger, so the channel closes when the last worker's service is disposed at `Listen`'s return, and `await pump` drains — 0021 §4.3's open question 2 answered by ownership alone.

**Q3 — Handler signature: async-only `(HttpRequest) -> OwningTask<HttpResponse>`; request borrowed; `OwningTask` is right.**
- §22.6 *Lambda-literal arguments*: applicability is decided by the lambda's inferred return type, which must equal, implicitly convert to (§5.1), or bind the slot; `lambda(req) => Http.Text("ok")` infers `owned HttpResponse`, and no conversion reaches `OwningTask<HttpResponse>`. The spelling is `async lambda(req) => Http.Text("ok")` (§30.6 assigns `OwningTask<T>` to a class result) or an `async method`.
- A sync overload `Get(route, scoped h: (HttpRequest) -> owned HttpResponse)` is declarable (§22.6: the function types differ — `scoped` alone would not distinguish, the parameter types do) and resolvable (no lambda is applicable to both). Declined for v1: twelve members instead of six, two dispatcher paths, and a real handler almost always awaits. Adding it later re-routes no existing call.
- `OwningTask<HttpResponse>` over `Task<HttpResponse>`: §4.3.

**Q4 — Binding: access-time `req.Param<T>`, `req.Query<T>: T?`, compiler-known `Json<T>`/`JsonOwned<T>`; failure is one typed exception the dispatcher maps to 400.**
- Route-pattern typing `{id:int}` needs the table to *hold* typed values without knowing `T` — reflection-shaped (rationale §5.18). Access-time `Param<T>` is §11.8 constraint dispatch (`T.Parse`), zero reflection; the non-generic `Param(name): string` stands beside it (§22.6's non-generic preference for the bare call; explicit `<int>` selects the generic) — and it *is* the string form: `string` witnesses no `IParsable` (stdlib §2.19), so `Param<string>` fails the constraint at compile time. The return is `owned T` because a bare-`T` instance return is a borrow of the receiver (§28.3 announcement table) and cannot carry a fresh parse; the marker erases at value substitutions (§28.1), so `Param<int>` reads as `int`.
- Where a parse failure goes: *(a) an exception mapped by the dispatcher* — the dispatcher is stdlib §1 band 4's "converted to a protocol answer" boundary, and program logic never branches on the failure. *(b) nullable* / *(c) `Result`* — one `if … then return Http.Status(400)` per parameter per endpoint, and the wrong band (band 2 is *absence*, not malformation). Chosen: (a), one type. Unknown parameter *name* stays band 1 (`ArgumentException`: a bug). `Query<T>` returns `T?` for absence and throws only on malformation.
- Malformed JSON: `req.Json<T>` catches `SerializationException` and rethrows `HttpResponseException(400, msg, cause)` (snapshot chaining, §31.4) rather than the dispatcher mapping every `SerializationException` — a handler's *own* serialization fault must stay a 500.

**Q5 — Responses: seven constructors; `HttpResponseException` yes.**
- 0002's five stand. `JsonOrNotFound<T>(value: T?)`: lookup-by-id is a third of any CRUD surface, `null` is already the stdlib's absence signal (`GetOrNull`), and the constructor composes with it directly. `JsonArray<T>(items: IEnumerable<T>)`: **no collection conforms to `ISerializable`** (stdlib §2.14/§4 declare none) and a heap-collection component makes a record serialization-ineligible (§9.8), so without it every list endpoint needs an owning envelope class; the member is ordinary generic code over stdlib §2.14's agents (`BeginArray`, `item.Serialize(s)`, `EndArray`). `NoContent`/`Created` are `Status(204)`/`Json(v, status := 201)` — the named arguments already say it.
- `HttpResponseException(status, message[, cause])`: a deep helper that wants a 404/409 throws it; the innermost dispatcher converts it. It carries an `int` and strings (§31.1), derives from 0001's `HttpException` root so `catch HttpException` still means "HTTP-shaped", and is not `HttpStatusException` (that is the client's `EnsureSuccess` producer — §31.4 producer locality).
- Headers on a constructed response: the indexer (`r.Headers["ETag"] := tag;`) or `Add`; 0001 §3.4's read-only clause narrows to *received* responses (§9c).

**Q6 — Composition: `Mount(prefix, owned child)`; flat is allowed; conflicts at registration.**
- Owned-by-the-table over children-in-fields-plus-borrowed-by-the-table: one concept instead of two for the same tree, and §28.5's cascade disposes it bottom-up. The `owned` transfer at the call site is a fresh `new` — no marker.
- Flat: nothing stops fifty `_routes.Get` lines in one class; `Mount` is the scaling knob. Groups *are* mounts. `Path` is always the full path (a root hook wants it); matching under a mount uses the remainder the table tracks, so a directly-dispatched request matches the service's own routes on its full path. `{*rest}` is the catch-all and the custom-404 mechanism.
- Conflicts — duplicate verb+route, a `Mount` prefix colliding with a literal route either way — are an `ArgumentException` at the registering call (band 1).

**Q7 — Cross-cutting: one opt-in `Around` hook on the table; no pipeline; no forced override.**
- `Before(req): OwningTask<HttpResponse?>` covers auth but not "log the status"; `After` covers logging but not short-circuiting; the pair is two hooks plus an ordering rule between them. `Around` is one hook covering both, plus timing and `try`/`catch` conversion. Order is nesting: one hook per table, parent around children across the tree — no list, no ordering lore.
- On the table rather than a virtual method: a service that has no policy registers nothing, and the registration line is the same form-4 trace as every other (`_routes.Around(Guard)`), so no new admission is involved and no base class is needed.
- Soundness: `next` is call-bounded; the hook may invoke it and must `await` it before returning (§28.14 *Call-bounded × deferred activation* (2)); returning or storing the un-awaited task is an escape sink, rejected. `async` is not a signature fact — §12.1's modifier enumeration omits it and §30.1 says so directly — so hook and handler references form against their function types unaffected.
- Where exceptions surface: `HttpResponseException` → a response at the *innermost* table (so a hook sees a 400 as a response); everything else propagates through the hooks to the outermost `Dispatch` → opaque 500. Hence no `OnError` hook: `Around` with a `catch` *is* one.

**Q8 — Testing: a public `HttpRequest` constructor + `Dispatch` on the table.**
- A constructor beats `Http.Request(…)`: it is the ordinary spelling, and `Http` is 0001's client facade. Body accessors on a constructed request or response read the in-memory body under the unchanged single-consumption rule. `Dispatch` lives on `RouteTable` — the object that does the work — and is reached as `service.Routes.Dispatch(req)`. The tests in §2 use only 0022's `Assert.Equal` (hashable records) and `using var` fixtures; seeding is `await store.Add(lambda(id) => …)` on the same `Store<Todo>` the service borrows, so a test never spells a lock.

**Q9 — Lifecycle: `Listen(…): Task<int>` + static `Stop()`; the deps overload; `Serve` as the fallback.**
- `Listen` stays the only blocking point and is awaited (#1362). `Stop()` static and idempotent per #1402 — one server per process is the v1 posture; the natural successor is a `token: CancellationToken` on `Listen`, a compatible addition (§6).
- The `service` parameter is a **spawn position** — §30.14's category, whose stdlib membership §28.14 names. The roster is enumerated in normative prose in both sections, so adding `HttpServer.Listen` is a two-sentence ruling-class edit at graduation (§9a), not a stdlib-only fact. **Fallback if that edit is refused:** `HttpServer.Serve(listener: TcpListener, service: IHttpService, token: CancellationToken := CancellationToken.None): Task` — an ordinary async method with call-bounded borrows — called inside a *user-written* `Domain.Spawn(Worker.Run, deps)` entry, one per worker (0007 §3's `Worker(owned listener)` shape). That uses only today's register but needs N workers to reach one listener: `Shared<TcpListener>` is sendable only if `TcpListener` declares the **synchronized** fact (stdlib §1; 0007 §3 asserts "internally synchronized", so the fact line is the natural carrier — §9d). A third variant — a sendable deps object that *is* the factory (`where D: sendable, class, IHttpServiceFactory`, the runtime calling `d.Create()` on the worker) — needs neither, at the cost of inverting the factory; recorded, not chosen.

**Q10 — Naming.** `IHttpService` (what you implement; one per resource), `RouteTable` (what it owns; reached as `_routes`, exposed as `Routes`), `HttpServer` (static lifecycle), `HttpRequest`/`HttpResponse`/`HttpHeaders`/`HttpMethod` (0001's vocabulary), `Http.*` (constructors on 0001's facade — `Http.Get(url)` is the client, `_routes.Get(route, h)` the server; receivers disambiguate). `HttpApp` is retired: it was two things — a per-worker table and a static lifecycle — under one name, which is what let 0021 §4.3 write `new HttpApp()` and an instance `Listen`.

## 8. Rulings record

**Carried forward from the 2026-07-24 delegated ruling (#1402), still applying:**

1. `workers := 0` means `Environment.ProcessorCount` (stdlib §2.4) — on both `Listen` overloads.
2. The `Json<T>` / `JsonOwned<T>` split on `HttpRequest`, compiler-known, mirroring 0001 §3.3 and the landed stdlib §2.15 `Json.Parse<T>`/`ParseOwned<T>` facade: value `T` arrives as a value, class `T` arrives owned; a class `T` at `Json<T>` is a compile-time error directing to `JsonOwned<T>` (the ruling's drafted proposal-text amendment; the 2026-07-24 §3 listing still showed the single accessor).
3. Default bind host `127.0.0.1` — safe by default; containers opt into `0.0.0.0`, and the diagnostic when a container health-check fails should name the flag.
4. v1 ships `Stop()` only; signal translation (SIGINT/SIGTERM → `Stop()`) is wired when the process/OS surface lands (proposal 0014's channel is the natural carrier).

**Superseded by this redraft** (the 2026-07-24 text's): the `HttpApp` class and its capture-free *route builder* `routes: (HttpApp) -> ()`; plain-parameter handler storage; the `NotFound`/`OnError` hooks (now `{*rest}` and `Around`); `Method: string` and `Route(verb: string, …)` (now `HttpMethod`); `Param`'s `KeyNotFoundException` (now `ArgumentException`, band 1); `Json<T>(): OwningTask<T>` as the single body accessor (now the split above); `r.Headers.Set(…)` (now the indexer); the §4.3 row mapping a `Param`/`Json` failure to a 500 (now a 400 through `HttpResponseException`); and the §2.1 showcase's `app.Get("/todos/{id}", api.GetById)` (now `_routes.Get("/{id}", GetById)` on the owned table). The sync-lambda `app.Get("/health", lambda(req) => Http.Text("ok"))` is superseded by the `async` spelling.

## 9. What must change elsewhere

**(a) The spawn-position roster — a ruling-class spec edit at graduation.** §28.14's *stdlib retention register* names "the asynchronous spawn positions of §30.14 (`Domain.Spawn`, the `Parallel` combinator bodies — stdlib §6.12)", and §30.14's *Spawning* and *Data-parallel combinators* subsections enumerate the same two. Graduation adds `HttpServer.Listen`'s `service` parameter to both lists — two sentences: in §28.14, extend the parenthetical to "(`Domain.Spawn`, the `Parallel` combinator bodies — stdlib §6.12 — and `HttpServer.Listen`'s `service` factory — the server's stdlib section)"; in §30.14, after the combinators paragraph, "The `service` parameter of `HttpServer.Listen` (the server's stdlib section) is likewise a spawn position: the factory runs as each worker domain's first continuation, its `owned D` argument (the deps form) is that domain's spawn argument, and the constructed service is domain-local for its life." Classified under spec-change-process.md §2 as a **ruling**: it decides an underspecified case (whether the spawn-position category admits a stdlib member beyond the two §30.14 enumerates) without changing any rule. §2's stricter-gate rule is satisfied either way — read as a design change instead, the required first artifact is a proposal document, which this is; the landing is ruling-class on both readings. Body-locality **local** (a call-site check on the argument form, the check `Domain.Spawn` already runs). The method-reference formation bar comes with membership automatically (it bars forming a reference to `Listen` itself, not to the entry). **If the edit is refused**, the `Serve` fallback of §7 Q9 needs no register change and item (d) instead.

**(b) `RouteTable` joins the stdlib retention register with normative quiescence.** Its `Get`/`Post`/`Put`/`Patch`/`Delete`/`Route`/`Around` parameters are declared `scoped` in the stdlib listing (the `EventSource.Add` / `TaskScope.Spawn` / `TestRunner.Add` precedent — the per-position roster is the standard library's, §28.14). The stdlib section states, as stdlib §6.5 does for the event types, that the destructor severs the handler and hook lists before reclamation and that no stored callback is invoked after disposal completes; in checked disposal mode a late invocation faults as `DisposedAccessException` (§28.2's scoped-callback backstop). `Dispatch` composes the stored hook and handler callbacks into each hook's `next` — a shape §28.14 bars for *user* code (the scoped-field read-out bar; the *Known v1 limitation*) and admits here only because `RouteTable` is compiler-known, exactly as `EventSource`'s invocation loop and `TaskScope`'s drain are: the register governs declared positions, not a compiler-known type's internals. No spec text changes for this item.

**(c) 0001 §3.4's `HttpHeaders` clause.** "Response-side `Headers` is a read-only view (mutators throw `InvalidOperationException`)" narrows to *received* responses; a response constructed by `Http.*` has writable headers until it is sent. 0001 §7's "server-side `Request → Response` … out of scope here" points at this redraft. (Companion edits; nothing else in 0001 moves.)

**(d) Only for the fallback:** `TcpListener` declares the **synchronized** fact on a stdlib §1 *Facts* line, so that `Shared<TcpListener>` enters §28.13's payload set and N user-spawned workers can reach one listener. 0007 §3 already classifies `TcpListener` as compiler-known **sendable**, but §30.14 crosses a sendable class as an *owned transfer* with a single destination, so that classification alone seats one worker on the listener, not N; the **synchronized** fact is what admits the `Shared<TcpListener>` handle each worker needs. 0007 §3 already asserts the property in prose and its Q1 ruling confirms it; the fact line is the formal carrier. Not needed if (a) lands.

## 10. Relationship to existing work

- **0001**: shares `HttpResponse`, `HttpHeaders`, `HttpMethod`, the `Http` facade, the `HttpException` root, and the substrate; the fetch symmetry is why the two read as one API. One clause narrows (§9c).
- **0007**: the `TcpListener` `Listen` binds is 0007 §4.4's; the per-worker accept topology is 0007 §3's, whose acceptance test this proposal is. 0007 §4.4's graceful-stop path (cancel `Accept`'s token) is what `Stop()` drives.
- **0021**: the first customer. §2's `Deps.Log: owned ChannelLogger` and the root's `_deps.Log.Info` line are the wiring 0021 §4.3 shows (a child that logs takes the same back-reference its store takes); the sink-loop lifecycle 0021 left open closes by ownership (§7 Q2).
- **0022**: `TestRunner`/`Assert` are the test material in §2; `TestRunner.Add`'s `scoped body` is the same storing-position shape as `RouteTable.Get`.
- **#1376**: the umbrella networking issue; this proposal is its server half. **#1402**: the tracking issue and the ruling record (§8).
- **§28.14 / §30.14 / §28.13 / §22.12**: the design rests on existing surface — form-4 admission for `this`-bound registration, spawn positions for the factory, `Shared<Locked<…>>` for shared state — plus the one roster edit of §9a. **§28.4**'s reverse-declaration-order disposal is what §4.1's field discipline leans on.
- **#1394/#645 (Stream), #1378 (timers), #703 (serialization — landed as stdlib §2.14/§2.15)**: sequencing dependencies noted inline.
- **networking-design-comparison.md**: the server rows' "manual `int.Parse` + `!`" and "~5 types" phrases are updated with this redraft (companion edit).

## 11. Forward compatibility with a `route` modifier

A future `public route(Get, "/{id}") method GetById(req: HttpRequest): OwningTask<HttpResponse>` re-expresses each `[T]` line mechanically: the compiler emits `_routes.<Verb>(<route>, <Method>)` at the end of every non-chaining constructor of the declaring class, in declaration order, at the body's end — the same *set* of constructors §22.8 step 2 runs field initializers in (those whose step 1 is `base(...)` or the implicit `base()`), but at step 3's tail rather than at step 2, because a registration must follow the body's own assignments to the dependency fields the handlers read — targeting the `owned` field that the class's `IHttpService.Routes` expression body reads (the compiler emits the *field* spelling, so the trace is form 4 exactly as today and no getter-hop lift is needed). No new admission rule, no runtime change: `RouteTable`, `Dispatch`, `Listen` are untouched, exactly as 0022 §3.4 keeps its runner under fork (a). Nothing here should be built until the evidence bar 0022 §3.4 sets is met; the point is only that today's spelling has a total, per-declaration, closed re-expression (rationale §5.19's `serialized` discipline), and that the constructor-tail registration — not a `Register()` phase — is the synthesis target.
