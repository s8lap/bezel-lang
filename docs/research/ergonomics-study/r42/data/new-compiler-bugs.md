# Compiler bugs found by the r42 recheck

Compiler: r42, `bezelc 0.0.0-8ccf7ff0` (origin/main 8ccf7ff06), `--checked-disposal`, default `--link`. Spec and stdlib v0.359. **No issues were filed** (study constraint). Each entry gives the spec sentence, what r42 did, a minimal repro checked on r42 as written, the workaround, the tasks it hit, and the existing issue found by REST search ("new" means none was found). NB16 and NB17 are what remains of B1 and B3 from the first study.

Also hit, documented as not yet implemented, so not numbered here: `GetType()` / `typeof` (#3819, #487), a missing file reading as empty (#7057), `Counter<K>` (#3848, #8472). NB8 and NB14 do not change any output.

| # | Bug | Silent | Existing issue |
|---|---|---|---|
| NB1 | `IFailure` is an unresolved type name | no | #8414 (open); also #8442 (open) for `DomainFailedException` |
| NB2 | `string.IsEmpty` is not implemented in codegen | no | #2348 (open) |
| NB3 | `Exception.ToString()` omits the message | yes | #3302 (open) |
| NB4 | a collection-literal local of record or choice elements is classified Borrowed | no | #8498 (open) |
| NB5 | an array of choices does not deserialize past its first element | yes | new |
| NB6 | `GetOrNull` over a generic-record value inside a generic class emits malformed IR | no | new |
| NB7 | in a generic class at `K = string`, a key read from the dictionary itself does not match | yes | new |
| NB8 | spurious internal warning on a short-circuit operand in a generic body | no | new (the message cites closed #2805) |
| NB9 | an interface constraint whose type argument is another type parameter is not satisfied | no | new (#8754, #8728 adjacent) |
| NB10 | false must-use warning on an owned parameter stored into a generic `OwningDictionary` slot | no | new (B2/B20 family) |
| NB11 | a bounded channel with `SendAsync` and two producers loses items and hangs | yes | new |
| NB12 | four or more tasks pending in `Semaphore.AcquireAsync` stall the program | yes | new (possibly the same runtime root as NB11) |
| NB13 | forwarding a `scoped` handler into an `EventSource` field over-releases the closure | no | new (closure over-release family: #6269, #8674, #7772) |
| NB14 | the trust-admission report is silent on a primary-constructor parameter stored by a field initializer | no | #8395 (open) family |
| NB15 | `RemoveAt` with a narrowed `int?` emits malformed IR | no | #2625 (open) family |
| NB16 | B1 residual: string members on a local inferred from `Console.In.ReadToEnd()` lower to nothing | yes | #8383 (closed): reopen or new |
| NB17 | B3 residual: an untyped lambda over a group's elements takes the key type | no | #8542 (open); #6627 (closed) named it |

## NB1. `IFailure` is an unresolved type name

- **Existing issue:** #8414 (open); also #8442 (open) for `DomainFailedException`
- **Spec:** spec §31.4, stdlib §1, codegen reference §6 and Guide 2 §4 (v0.359): `class ConfigError(message: string): Exception(message), IFailure` and the boundary arm `var f: IFailure => …`.
- **r42:** `unresolved type name \`IFailure\`` at the listing and at the arm.
- **Workaround:** Name the user class in the arm instead; no IFailure-marked boundary is expressible.
- **Tasks:** n01, n03, n05, n13
- **Note:** doc-vs-compiler gap: the v0.359 docs teach the listing with no "not yet implemented" marker.

```bezel
module P;
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
public class Program
  public static method Main()
    try
      throw new ConfigError("bad");
    catch
      var f: IFailure => Console.WriteLine(f.Message)
    end;
  end Main;
end Program;
```

## NB2. `string.IsEmpty` is not implemented in codegen

- **Existing issue:** #2348 (open)
- **Spec:** stdlib §9.6 and codegen reference §5.9: `pure readonly property IsEmpty: bool` (and the doc idiom `s = null or s.IsEmpty`).
- **r42:** `string member \`.IsEmpty\` is not yet supported in codegen`.
- **Workaround:** `s.Length = 0`.
- **Tasks:** n01
- **Note:** doc-vs-compiler gap: no NYI marker in §5.9.

```bezel
module P;
public class Program
  public static method Main()
    var s := "x";
    Console.WriteLine(s.IsEmpty);
  end Main;
end Program;
```

## NB3. `Exception.ToString()` omits the message

- **Existing issue:** #3302 (open)
- **Spec:** spec §31.4 / codegen §5.2: a conforming rendering contains the qualified type name and the `Message` when non-empty.
- **r42:** prints `System.InvalidOperationException` for `new InvalidOperationException("bad state")`, through `ToString()` and through `$"{ex}"`. Silent.
- **Workaround:** `ex.Message` plus a hand-written type name (GetType is not reachable, #3819).
- **Tasks:** n03

```bezel
module P;
public class Program
  public static method Main()
    try
      throw new InvalidOperationException("bad state");
    catch
      var ex: Exception => do
        Console.WriteLine(ex.ToString());
        Console.WriteLine($"{ex}");
        Console.WriteLine(ex.Message);
      end
    end;
    var e2 := new FormatException("x");
    Console.WriteLine(e2.ToString());
  end Main;
end Program;
```

## NB4. a collection-literal local of record or choice elements is classified Borrowed

- **Existing issue:** #8498 (open)
- **Spec:** spec §15.4 (v0.359) and the §28.3 atoms: a reference-typed collection literal is a fresh owned production.
- **r42:** `var xs: Array<R> := [new R(1), new R(2)]; new Holder(move xs)` is rejected: "a borrow cannot be passed where an `owned` value is required". `Array<int>` and `Array<string>` literals work.
- **Workaround:** `new Array<T>()` and one `Add` per element.
- **Tasks:** n08

```bezel
module P;
public record R(V: int);
public choice Sh
  C(r: double);
end Sh;
public class Holder
  private owned _xs: Array<R>;
  public constructor(owned xs: Array<R>)
    _xs := xs;
  end constructor;
  public method Count(): int => _xs.Count;
end Holder;
public class Program
  public static method Main()
    var xs: Array<R> := [new R(1), new R(2)];
    using var h := new Holder(move xs);
    Console.WriteLine(h.Count());
  end Main;
end Program;
```

## NB5. an array of choices does not deserialize past its first element

- **Existing issue:** new
- **Spec:** spec §9.8: a value-like serializable choice is admissible as a sequence element.
- **r42:** the serializer writes `[{"$case":"Circle","r":1},{"$case":"Rect","w":2,"h":3}]`; reading it back, synthesized (an `owned Array<Shape>` class field, a `[2]Shape` record field) or hand-driven (`BeginArray` + `HasMoreElements` + `Shape.Deserialize`), raises `SerializationException: Choice deserialize: expected '{'` at the second element. A single choice and a choice record component round-trip.
- **Workaround:** none for choices; the task replaced the choice by a record with an enum tag.
- **Tasks:** n08
- **Note:** compile-time silent; the round trip fails at run time.

```bezel
module P;
import System.Serialization;
public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double);
end Shape;
public class Program
  public static method Main()
    using var d := new JsonDeserializer(@"[{""$case"":""Circle"",""r"":1},{""$case"":""Rect"",""w"":2,""h"":3}]");
    _ := d.BeginArray();
    var n := 0;
    while d.HasMoreElements() do
      var sh := Shape.Deserialize(d);
      n++;
    end;
    d.EndArray();
    Console.WriteLine(n);
  end Main;
end Program;
```

## NB6. `GetOrNull` over a generic-record value inside a generic class emits malformed IR

- **Existing issue:** new
- **Spec:** stdlib §4.8 `GetOrNull(key): V?`.
- **r42:** `Invalid InsertValueInst operands! … insertvalue { i8, %"record.Entry$spec0" } …` / `IR verification failed`. The same code in `Main` (non-generic context) works.
- **Workaround:** `ContainsKey` then the indexer.
- **Tasks:** n09

```bezel
module P;
public record Entry<V>(Value: V, Expires: int);
public class Cache<V>
  private owned _d: Dictionary<string, Entry<V>> := new Dictionary<string, Entry<V>>();
  public method Put(k: string, v: V) => _d[k] := new Entry<V>(v, 10);
  public method ExpiresOr(k: string): int
    var e := _d.GetOrNull(k);
    return (if e = null then -1 else e.Expires);
  end ExpiresOr;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache<int>();
    c.Put("a", 1);
    Console.WriteLine(c.ExpiresOr("a"));
  end Main;
end Program;
```

## NB7. in a generic class at `K = string`, a key read from the dictionary itself does not match

- **Existing issue:** new
- **Spec:** stdlib §4.8 `Remove(key): bool`; hashing and equality of `string` are by value.
- **r42:** `Remove(first[0])`, where `first[0]` was read from `kv.Key`, returns `False` and the entry stays (the same class at `K = int` returns `True`). Projecting the keys with `Select(kv => kv.Key).ToArray()` and printing one segfaults (`N-generic-key-projection.bzl`). Silent wrong result.
- **Workaround:** rebuild a new dictionary of the live entries and replace the owned field.
- **Tasks:** n09

```bezel
module P;
public class Bag<K> where K: hashable
  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();
  public method Put(k: K, v: int) => _d[k] := v;
  public method DropFirst(): int
    var first := new Array<K>();
    for var kv in _d do first.Add(kv.Key); break; end;
    var ok := _d.Remove(first[0]);
    Console.WriteLine($"removed {ok}");
    return _d.Count;
  end DropFirst;
end Bag;
public class Program
  public static method Main()
    using var b := new Bag<string>();
    b.Put("a", 1); b.Put("b", 9);
    Console.WriteLine(b.DropFirst());
    using var c := new Bag<int>();
    c.Put(1, 1); c.Put(2, 9);
    Console.WriteLine(c.DropFirst());
  end Main;
end Program;
```

## NB8. spurious internal warning on a short-circuit operand in a generic body

- **Existing issue:** new (the message cites closed #2805)
- **Spec:** spec §17.1: `and` evaluates its right operand when the left is true.
- **r42:** `warning: internal: the right operand of a short-circuit operator failed to lower to a value and was treated as \`false\` — #2805` on `i < _xs.Count and _xs[i].CompareTo(_xs[j]) < 0` in `Box<T> where T: IComparable<T>`, yet the program prints `True`, the right answer.
- **Workaround:** none needed.
- **Tasks:** n10
- **Note:** misleading: the warning claims a silent wrong answer that does not happen.

```bezel
module P;
public class Box<T> where T: IComparable<T>
  private owned _xs: Array<T> := new Array<T>();
  public method Add(x: T) => _xs.Add(x);
  public method FirstLess(i: int, j: int): bool => i < _xs.Count and _xs[i].CompareTo(_xs[j]) < 0;
end Box;
public class Program
  public static method Main()
    using var b := new Box<int>();
    b.Add(1); b.Add(2);
    Console.WriteLine(b.FirstLess(0, 1));
  end Main;
end Program;
```

## NB9. an interface constraint whose type argument is another type parameter is not satisfied

- **Existing issue:** new (#8754, #8728 adjacent)
- **Spec:** spec §14.2: `User: IEntity<int>` satisfies `where T: IEntity<K>` at `K = int`.
- **r42:** `type argument \`User\` does not satisfy the constraint \`where T: IEntity\` (§14.2)` (the message drops the type argument).
- **Workaround:** fix the key type: `Repo<T> where T: class, IEntity<int>`.
- **Tasks:** n12

```bezel
module P;
public interface IEntity<K>
  readonly property Id: K;
end IEntity;
public class Repo<K, T> where T: IEntity<K>
  public method IdOf(x: T): K => x.Id;
end Repo;
public class User(id: int): IEntity<int>
  public readonly property Id: int := id;
end User;
public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    using var u := new User(4);
    Console.WriteLine(r.IdOf(u));
  end Main;
end Program;
```

## NB10. false must-use warning on an owned parameter stored into a generic `OwningDictionary` slot

- **Existing issue:** new (B2/B20 family)
- **Spec:** spec §28.3: an owned-slot store is marker-free and discharges the binding.
- **r42:** `warning: owned parameter \`item\` is implicitly disposed at the end of the method` on `_items[key] := item;` in `Repo<K, T>`; disposal is correct (each object closes once). The non-generic class is silent.
- **Workaround:** ignore.
- **Tasks:** n12

```bezel
module P;
public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;
public class Repo<K, T> where K: hashable where T: class
  private owned _items: OwningDictionary<K, T> := new OwningDictionary<K, T>();
  public method Add(key: K, owned item: T)
    _items[key] := item;
  end Add;
  public property Count: int => _items.Count;
end Repo;
public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    r.Add(1, new User(1));
    r.Add(2, new User(2));
    Console.WriteLine(r.Count);
  end Main;
end Program;
```

## NB11. a bounded channel with `SendAsync` and two producers loses items and hangs

- **Existing issue:** new
- **Spec:** stdlib §6.9 / Guide 4 §6: `SendAsync` awaits capacity; each item is received exactly once; disposing the last sender closes the channel.
- **r42:** capacity 1, two producers of three items: the consumer receives 1, 2, 10, the first producer then reports items 2 and 3 as sent and finishes, and the program hangs. At capacity 4 with two producers of five items the received sum was 25 (expected 165) and no error.
- **Workaround:** unbounded channel and synchronous `Send` (no back-pressure).
- **Tasks:** n14
- **Note:** silent data loss, then a hang.

```bezel
module P;
import System.Threading;
public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..3 do
      await sender.SendAsync(id * i);
      Console.WriteLine($"p{id} sent {id * i}");
    end;
    Console.WriteLine($"p{id} done");
  end Produce;
end Producers;
public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>(1);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Producers.Produce(1, move s1);
    using var p2 := Producers.Produce(10, move s2);
    var sum := 0;
    await for var v in ch.ReceiveAll() do Console.WriteLine($"got {v}"); sum += v; end;
    Console.WriteLine($"sum {sum}");
  end Main;
end Program;
```

## NB12. four or more tasks pending in `Semaphore.AcquireAsync` stall the program

- **Existing issue:** new (possibly the same runtime root as NB11)
- **Spec:** stdlib §6.3: `AcquireAsync` awaits a permit; disposing the token returns it.
- **r42:** with `Semaphore(1)` and five jobs (four waiting) nothing after the first `job 1 in` runs, not even the holder's `Task.Delay`; four jobs (three waiting) complete. With `Semaphore(2)` and five jobs, job 4 entered before job 3.
- **Workaround:** start the jobs in waves so that at most three tasks wait.
- **Tasks:** n16
- **Note:** silent hang.

```bezel
module P;
import System.Threading;
public static class Jobs
  public static async method Run(id: int, gate: Semaphore): Task
    using var permit := await gate.AcquireAsync();
    Console.WriteLine($"job {id} in");
    await Task.Delay(10);
    Console.WriteLine($"job {id} out");
  end Run;
end Jobs;
public class Program
  public static async method Main(): Task
    using var gate := new Semaphore(1);
    using var jobs := new OwningArray<Task>();
    for var i in 1..5 do
      jobs.Add(Jobs.Run(i, gate));
    end;
    Console.WriteLine("started");
    for var j in jobs do
      await j;
      Console.WriteLine("one awaited");
    end;
  end Main;
end Program;
```

## NB13. forwarding a `scoped` handler into an `EventSource` field over-releases the closure

- **Existing issue:** new (closure over-release family: #6269, #8674, #7772)
- **Spec:** spec §28.14: a `scoped` function-typed parameter may be forwarded to a same-bound `scoped` parameter; stdlib §6.5 `EventSource<T>.Subscribe(scoped handler)`.
- **r42:** compiles; aborts at teardown with `bezel: fatal: refcount over-release (closure) — two releases for one reference`; in n17 v3 the same shape segfaulted at the first `Raise`.
- **Workaround:** own the `EventSource` in the caller and subscribe on that `using var`.
- **Tasks:** n17

```bezel
module P;
public class Hub
  private owned readonly _changed: EventSource<double> := new EventSource<double>();
  public method OnChanged(scoped handler: (double) -> ()): owned Subscription => _changed.Subscribe(handler);
  public method Fire(t: double) => _changed.Raise(t);
end Hub;
public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public property N: int => _n;
end Counter2;
public class Program
  public static method Main()
    using var c := new Counter2();
    using var hub := new Hub();
    using var sub := hub.OnChanged(lambda(t) => c.Hit(t));
    hub.Fire(1.0);
    Console.WriteLine(c.N);
  end Main;
end Program;
```

## NB14. the trust-admission report is silent on a primary-constructor parameter stored by a field initializer

- **Existing issue:** #8395 (open) family
- **Spec:** spec §28.3 *Diagnostics* (report mode): every borrow of a plain parameter stored in a plain field is listed.
- **r42:** the report lists `Display`'s constructor store (`_thermo := thermo`) but not `Thermostat`'s field initializer.
- **Workaround:** none needed (informative mode).
- **Tasks:** n17
- **Note:** the report under-counts the program's trusted lifetimes.

Repro: (n17 final: `class Thermostat(target: double, changed: EventSource<double>)` with `private _changed: EventSource<double> := changed;`).

## NB15. `RemoveAt` with a narrowed `int?` emits malformed IR

- **Existing issue:** #2625 (open) family
- **Spec:** spec §7.4: after `if i = null then return; end;` the local is `int`.
- **r42:** `SExt only operates on integer … sext { i8, i32 } … to i64` / IR verification failed. The indexer read `xs[i]` works.
- **Workaround:** `var k: int := i; xs.RemoveAt(k);`.
- **Tasks:** n22

```bezel
module P;
public class Program
  public static method Main()
    var xs: Array<int> := [10, 20, 30];
    var i := int.ParseOrNull("1");
    if i = null then return; end;
    Console.WriteLine(xs[i]);
    xs.RemoveAt(i);
    Console.WriteLine(xs.Count);
  end Main;
end Program;
```

## NB16. B1 residual: string members on a local inferred from `Console.In.ReadToEnd()` lower to nothing

- **Existing issue:** #8383 (closed): reopen or new
- **Spec:** stdlib §7.4 `ReadToEnd(): string`; stdlib §9.6 string members.
- **r42:** `var raw := Console.In.ReadToEnd(); var c := raw.ToUpper();` warns `internal: lowerLocalDecl's initializer expression failed to lower … — #2434` and `c` prints `0`; the same with `.Trim()` and with the chained `Console.In.ReadToEnd().ToLower()`. `raw.Length` and `var e: string := raw; e.ToLower()` work.
- **Workaround:** ascribe the local `: string`.
- **Tasks:** t02 v1
- **Note:** silent apart from the internal warning.

```bezel
module P;
public class Program
  public static method Main()
    var a := "AbC";
    var b := a.ToLower();
    Console.WriteLine(b);
    var raw := Console.In.ReadToEnd();
    var c := raw.ToUpper();
    Console.WriteLine(c);
    var d := raw.Trim();
    Console.WriteLine(d);
    var e: string := raw;
    Console.WriteLine(e.ToLower());
  end Main;
end Program;
```

## NB17. B3 residual: an untyped lambda over a group's elements takes the key type

- **Existing issue:** #8542 (open); #6627 (closed) named it
- **Spec:** stdlib §5: `IGrouping<TKey, T>: IEnumerable<T>`; spec §27.3 lambda parameter inference.
- **r42:** `g.Where(lambda(e) => e.Salary > 1)` and `g.Sum(lambda(e) => e.Salary)` inside `for var g in rows.GroupBy(lambda(e) => e.Dept)` give `string member \`.Salary\` is not yet supported in codegen`; `for var e in g` is right, and a typed lambda works.
- **Workaround:** type the inner lambda.
- **Tasks:** t03 v1/v2, t11 v1

```bezel
module P;
record E(Dept: string, Salary: int);
public class Program
  public static method Main()
    var rows: Array<E> := [new E("a", 1), new E("b", 2), new E("a", 3)];
    for var g in rows.GroupBy(lambda(e) => e.Dept) do
      var n := 0;
      for var e in g do n += e.Salary; end;
      Console.WriteLine($"{g.Key}: {n} {g.Count()} {g.Where(lambda(e) => e.Salary > 1).Count()}");
    end;
  end Main;
end Program;
```
