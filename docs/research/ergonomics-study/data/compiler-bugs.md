# Compiler bugs found by the ergonomics study

Compiler: r40, `bezelc-0.0.0-a30bb11d` (origin/main a30bb11df), `--checked-disposal`, default `--link`. Spec v0.355, stdlib v0.355.

Each entry gives the spec sentence the program relies on, what r40 did, a minimal repro (checked on r40 as written here), the workaround the study used and the tasks it hit. **No issues were filed** (study constraint). Nine of the 24 are *silent* — the program compiles and prints a wrong answer or nothing: B1, B4, B5, B6, B7, B13, B16, B24, and B11 at run time. B8 and B17 are contested by open spec questions. Each entry names an existing issue where one was found (coordinator-supplied table, REST search for B23/B24); "new" means none was found.

Documented not-yet-implemented items the study also hit (not numbered here; in `frictions.tsv` they are classified COMPILER-BUG, cluster `C-documented-nyi`, because the spec already answers the design question and the fix is implementation): `GroupBy` results stored in a variable (#755), `Counter<K>` (#3848), the `Json`/`Binary` facades (#2349), `Locked<T>`/`RwLocked<T>` (#1721), `typeof(E).EnumNames`/`EnumValues` (#3819, #487).

## B1. `TextReader.ReadToEnd()` on `Console.In` lowers to nothing

- **Existing issue:** new
- **Spec:** stdlib §7.4: `ReadToEnd(): string` on TextReader; stdlib §2.1 `Console.In: TextReader`.
- **r40:** Compiles with `warning: internal: lowerLocalDecl's initializer expression failed to lower ... (degraded to a zero-initialized value) — #2434`; the local is empty and a later `Console.WriteLine` of it emits nothing. In t02 the only *error* was reported at an unrelated `for` loop.
- **Workaround:** Read with a `Console.ReadLine()` loop.
- **Tasks:** t01, t02

```bezel
module P;
public class Program
  public static method Main()
    var text := Console.In.ReadToEnd();
    Console.WriteLine(text.Length);
  end Main;
end Program;
```

## B2. Must-use warning on trivial-dispose values

- **Existing issue:** #4418
- **Spec:** spec §28.9 (v0.355): "No must-use / auto-drop diagnostic attaches to a trivial-dispose value"; `Array<T>` is trivial-dispose (code-gen §4.1).
- **r40:** `warning: owned value in `f` is implicitly disposed at the end of its scope` for an `owned Array<string>` returned by a helper (also `Directory.GetFiles`, user classes with only value fields).
- **Workaround:** `using var`, or ignore.
- **Tasks:** t03, t10, t23

```bezel
module P;
public class Program
  private static method Mk(): owned Array<string>
    var a := new Array<string>();
    a.Add("x");
    return a;
  end Mk;
  public static method Main()
    var f := Mk();
    Console.WriteLine(f.Count);
  end Main;
end Program;
```

## B3. Untyped key lambda in `GroupBy` produces malformed IR

- **Existing issue:** #6627
- **Spec:** spec §27.3: lambda parameter types are inferred from the target type; stdlib §5 `GroupBy<TKey>(key: (T) -> TKey)`.
- **r40:** `Function return type does not match operand type of return inst! ... bezelc: codegen error: IR verification failed`. Related: `.Select(lambda(g) => new Summary(g.Key, ...))` over the groups reports `the argument Customer ... is not implicitly convertible`.
- **Workaround:** Write every lambda fully typed: `lambda(e: string): int => e.Length`, `lambda(g: IGrouping<string, Order>): Summary => ...`.
- **Tasks:** t03, t11

```bezel
module P;
public class Program
  public static method Main()
    var rows: Array<string> := ["ab", "c", "de"];
    for var g in rows.GroupBy(lambda(e) => e.Length) do
      Console.WriteLine($"{g.Key}: {g.Count()}");
    end;
  end Main;
end Program;
```

## B4. `OrderBy`/`OrderByDescending`/`ThenBy` ignore non-`int` keys

- **Existing issue:** new (#7780 covers DateTime keys only)
- **Spec:** stdlib §5: the OrderBy family is a stable sort by the key's `IComparable` order.
- **r40:** With a `string`, `double` or `decimal` key the sequence comes back in input order (typed or untyped lambda alike); `int` keys sort correctly. Silent.
- **Workaround:** `Array<T>.Sort(comparer)` with a typed comparer lambda; manual max loops.
- **Tasks:** t02, t03, t08, t11

```bezel
module P;
public class Program
  public static method Main()
    var xs: Array<string> := ["b", "a", "c"];
    for var e in xs.OrderBy(lambda(e) => e) do
      Console.WriteLine(e);
    end;
    xs.Sort();
    Console.WriteLine(String.Join(",", xs));
  end Main;
end Program;
```

```bezel
module P;
public choice Sh
  C(r: double),
  R(w: double, h: double);
  public method Area(): double => (case this of C(var r) => r * r, R(var w, var h) => w * h);
end Sh;
public class Program
  public static method Main()
    var xs: Array<Sh> := [Sh.C(1.0), Sh.R(2.0, 3.0), Sh.C(0.5)];
    for var s in xs.OrderByDescending(lambda(s) => s.Area()) do Console.Write($"{s.Area()} "); end;
    Console.WriteLine();
    var ds: Array<double> := [1.0, 6.0, 0.25];
    for var d in ds.OrderByDescending(lambda(d) => d) do Console.Write($"{d} "); end;
    Console.WriteLine();
    Console.WriteLine(xs.OrderByDescending(lambda(s: Sh): double => s.Area()).First().Area());
  end Main;
end Program;
```

## B5. `Array<DictionaryEntry<K, V>>` is broken

- **Existing issue:** #6557 (the Add leg); the sort leg is new
- **Spec:** stdlib §4.8 (DictionaryEntry is a record), §5 `ToArray()`, §4.4 `Sort(comparer)`.
- **r40:** `d.ToArray().Sort(cmp)` leaves the order unchanged; building the array with `Add` from a `for var e in d` loop segfaults on the next read. The segfault leg reproduced 3/3 here with r40 `a30bb11d`; the reviewing critic did not reproduce it, so treat the Add leg as the known #6557 and the sort no-op as the new part.
- **Workaround:** Copy entries into a user record (binding `e.Key` to a local first, see B6).
- **Tasks:** t02

```bezel
module P;
public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["b"] := 1; d["a"] := 4; d["c"] := 4; d["z"] := 2;
    var es := d.ToArray();
    es.Sort(lambda(x, y) => (if x.Value <> y.Value then y.Value - x.Value else x.Key.CompareTo(y.Key)));
    for var e in es do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
    es.Sort(lambda(x, y) => y.Value - x.Value);
    for var e in es do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
    var ks: Array<string> := ["q", "b", "m"];
    ks.Sort(lambda(x, y) => x.CompareTo(y));
    Console.WriteLine(String.Join(",", ks));
  end Main;
end Program;
```

```bezel
module P;
public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["b"] := 1; d["a"] := 4; d["z"] := 2;
    var es := new Array<DictionaryEntry<string, int>>();
    for var e in d do es.Add(e); end;
    es.Sort(lambda(x, y) => y.Value - x.Value);
    for var e in es do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
    var es2 := d.ToArray();
    Console.WriteLine(es2.Count);
    es2.Reverse();
    for var e in es2 do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
  end Main;
end Program;
```

## B6. A string read through an indexer or field and passed straight into a record constructor is freed with its source

- **Existing issue:** new (#8101 family)
- **Spec:** spec §28.1/§28.9: `string` is runtime-managed (no disposal, no classification); a record copies its components.
- **r40:** `rows.Add(new E(f[0]))` where `f` is a loop-local `Array<string>`: after `f` dies the record's string is reused memory (prints another string, crashes `GroupBy`, or wrong dictionary keys). Binding `var n := f[0];` first is correct. Silent.
- **Workaround:** Bind the element to a local before constructing the record.
- **Tasks:** t02, t03, t06

```bezel
module P;
record E(Name: string);
public class Program
  public static method Main()
    var rows := new Array<E>();
    for var i in 1..3 do
      var f := $"name{i},z".Split(',');
      rows.Add(new E(f[0]));
    end;
    var junk := new Array<string>();
    for var i in 1..100 do junk.Add($"junkjunkjunk{i}"); end;
    for var r in rows do Console.WriteLine(r.Name); end;
  end Main;
end Program;
```

## B7. Untyped comparer lambda comparing `string` members does not sort

- **Existing issue:** new
- **Spec:** spec §27.3 (inferred parameter types); stdlib §4.4 `Sort(comparer: (T, T) -> int)`.
- **r40:** `ss.Sort(lambda(x, y) => x.K.CompareTo(y.K))` over `Array<W>` (record with string `K`) leaves input order; the same lambda with `(x: W, y: W): int` sorts. Silent.
- **Workaround:** Type the comparer lambda.
- **Tasks:** t02

```bezel
module P;
record W(K: string, V: int);
public class Program
  public static method Main()
    var es: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    es.Sort(lambda(x: W, y: W): int => (if x.V <> y.V then y.V - x.V else x.K.CompareTo(y.K)));
    for var e in es do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
    var ss: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    ss.Sort(lambda(x, y) => x.K.CompareTo(y.K));
    for var e in ss do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
    var ts: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    ts.Sort(lambda(x, y) => (if x.V <> y.V then y.V - x.V else 0));
    for var e in ts do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
  end Main;
end Program;
```

## B8. A narrowed local used as an `if`-expression branch makes the result nullable (contested: #6698)

- **Existing issue:** #6698 (open spec question: contested)
- **Spec:** spec §7.4 (v0.355): inside a narrowing region the variable is "narrowed to User here"; §20's common type of `string` and `string` is `string`.
- **r40:** `var hay := (if c then line.ToLower() else line);` inside `while line <> null do` gives `hay: string?`; the error appears at the later use (`member access .Length on a nullable value`).
- **Workaround:** Ascribe the local and add `!`: `var hay: string := (if c then line.ToLower() else line!);`.
- **Tasks:** t04

```bezel
module P;
public class Program
  public static method Main()
    var line := Console.ReadLine();
    while line <> null do
      var hay := (if line.Length > 3 then line.ToLower() else line);
      Console.WriteLine(hay.Length);
      line := Console.ReadLine();
    end;
    var s: string? := "x";
    if s <> null then
      var t := (if true then s else "y");
      Console.WriteLine(t.Length);
    end;
  end Main;
end Program;
```

## B9. `ParseOrNull` on the integer types is not found

- **Existing issue:** #7318
- **Spec:** stdlib §9.1: `static pure ParseOrNull(s: string): T?` on every integer type (also used in code-gen §6's own idiom).
- **r40:** `type 'int' has no static method or property 'ParseOrNull' (§3.4)` (also `Int32`, `long`).
- **Workaround:** `TryParse` with an `out` local.
- **Tasks:** t05, t19

```bezel
module P;
public class Program
  public static method Main()
    var a := int.ParseOrNull("12");
    var b := Int32.ParseOrNull("12");
    var c := long.ParseOrNull("12");
    Console.WriteLine($"{a} {b} {c}");
  end Main;
end Program;
```

## B10. Enum `NameOf` / `FromNameOrNull` are not found

- **Existing issue:** #3862
- **Spec:** stdlib §9.9 / spec §10.4: compiler-provided on every enum.
- **r40:** `no member NameOf on type Color`; the same for the stdlib enum `DayOfWeek`.
- **Workaround:** A hand-written `case` mapping each constant to its name.
- **Tasks:** t09, t38

```bezel
module P;
public enum Color
  Red, Green;
end Color;
public class Program
  public static method Main()
    Console.WriteLine(Color.NameOf(Color.Green));
    Console.WriteLine(Color.FromNameOrNull("Red") <> null);
  end Main;
end Program;
```

## B11. Checked-disposal mode raises `DisposedAccessException` on live sets

- **Existing issue:** #6314
- **Spec:** spec §28.2: checked mode faults only on access to a logically-disposed instance.
- **r40:** t15 (any of v1-v6) dies with `Unhandled exception: System.DisposedAccessException`; the failing statement moves with unrelated edits, adding `Console.Error.WriteLine` trace lines makes it pass, and `--unchecked-disposal` prints the correct output. Not minimized (suspected memory corruption).
- **Workaround:** None found under checked mode.
- **Tasks:** t15

Repro: the study program `programs/t15-sets/v1.bzl`.

## B12. Owned trivial-dispose local in an `async` body is a hard error

- **Existing issue:** #1772
- **Spec:** spec §28.9 (no must-use diagnostic at a trivial-dispose type) and §28.3 (the implicit drop of a plain `var` carries at most the must-use *warning*); §28.12 implicit discharge in async bodies is the synchronous path.
- **r40:** After an `await`, `var f := new Flaky();` (a class with one `int` field) or `var sw := Stopwatch.StartNew();` is rejected: `owned value in f would be dropped here without disposal`. Without the `await` it is (wrongly, B2) a warning.
- **Workaround:** `using var`.
- **Tasks:** t18, t33

```bezel
module P;
public class Flaky
  private _calls: int := 0;
  public method Get(): int => _calls;
end Flaky;
public class Program
  public static async method Main(): Task
    var f := new Flaky();
    await Task.Delay(1);
    Console.WriteLine(f.Get());
  end Main;
end Program;
```

## B13. `Sum` with a `decimal` selector returns a wrong value

- **Existing issue:** new
- **Spec:** stdlib §5 `Sum<U>(selector: (T) -> U): owned U where U: INumber<U>`.
- **r40:** `items.Sum(lambda(i) => i.Qty * i.Price)` over `[(2, 1.50m), (1, 4.00m)]` prints `34` (expected `7.00`); 0 inside a group. `Array<decimal>.Sum()` and an `int` selector are right. Silent.
- **Workaround:** A `for` loop or `Aggregate(0m, ...)` with a typed lambda.
- **Tasks:** t11, t37

```bezel
module P;
record Item(Qty: int, Price: decimal);
public class Program
  public static method Main()
    var items: Array<Item> := [new Item(2, 1.50m), new Item(1, 4.00m)];
    Console.WriteLine(items.Sum(lambda(i) => i.Qty * i.Price));
    var ds: Array<decimal> := [1.5m, 2.5m];
    Console.WriteLine(ds.Sum());
    Console.WriteLine(items.Sum(lambda(i) => i.Qty));
  end Main;
end Program;
```

## B14. `File.Delete` draws the unused-result warning

- **Existing issue:** new
- **Spec:** stdlib §7.3: `Delete(path: string)` returns `()`; spec §21.9 warns only on a non-`()` result.
- **r40:** `warning: the result of this expression statement is unused`.
- **Workaround:** Ignore.
- **Tasks:** t21

```bezel
module P;
import System.IO;
public class Program
  public static method Main()
    File.WriteAllText("/var/tmp/bezel-scratch/ergo/bugs/x.txt", "x");
    File.Delete("/var/tmp/bezel-scratch/ergo/bugs/x.txt");
  end Main;
end Program;
```

## B15. `Stream` span overloads not implemented (#1394)

- **Existing issue:** #1394 (#2457, named by the compiler message, is closed)
- **Spec:** stdlib §7.1 `Write(buffer: ReadOnlySpan<byte>)`, `Read(buffer: Span<byte>): int`.
- **r40:** `Write on built-in type FileStream produced no value ... documented but not implemented by this compiler (#2457)`. Tracked as #1394 (the message names #2457, which is closed); listed because it forced manual byte encoding.
- **Workaround:** `Write(Array<byte>, offset, count)` and shifts/masks.
- **Tasks:** t22

Repro: the study program `programs/t22-binary/v1.bzl`.

## B16. A collection literal at an `IEnumerable<string>` parameter is accepted silently and does nothing

- **Existing issue:** new
- **Spec:** spec §15.4 names no interface-typed literal target, so the call is ill-formed and must be rejected. The bug is the **silent acceptance** (a no-op at run time), not the rejection the first draft would have met.
- **r40:** `File.WriteAllLines(p, ["alpha", "beta"]);` compiles and creates no file; the same call with a typed `Array<string>` local works. Silent.
- **Workaround:** Bind the literal to a typed local first.
- **Tasks:** t21

```bezel
module P;
import System.IO;
public class Program
  public static method Main()
    const p := "/var/tmp/bezel-scratch/ergo/f1.txt";
    File.WriteAllLines(p, ["alpha", "beta"]);
    Console.WriteLine(File.Exists(p));
    var t := File.ReadAllText(p);
    Console.WriteLine(t.Length);
    File.WriteAllText(p, "x\ny\n");
    Console.WriteLine(File.ReadAllText(p).Length);
    for var l in File.ReadAllLines(p) do Console.WriteLine(l); end;
    var xs: Array<string> := ["q", "r"];
    File.WriteAllLines(p, xs);
    Console.WriteLine(File.ReadAllText(p).Length);
  end Main;
end Program;
```

## B17. `Select` with an interpolated-string projection is rejected as a leak (contested: #7170, #7215)

- **Existing issue:** #7170, #7215 (open spec questions: contested)
- **Spec:** spec §28.1/§28.9: `string` is runtime-managed — no ownership, nothing to leak.
- **r40:** `a lazy Select selector that mints a fresh owned value per element (... a fresh string such as a + concatenation or an interpolation) launders that value ...`. `x.ToString()` is accepted.
- **Workaround:** Project with `ToString()` instead of an interpolation.
- **Tasks:** t29, t33

```bezel
module P;
public class Program
  public static method Main()
    var xs: Array<int> := [1, 2];
    Console.WriteLine(String.Join(" ", xs.Select(lambda(x) => $"<{x}>")));
    Console.WriteLine(String.Join(" ", xs.Select(lambda(x) => x.ToString())));
  end Main;
end Program;
```

## B18. Internal warning on `ToString()` over a type parameter

- **Existing issue:** new (the message names closed #2477)
- **Spec:** spec §8.1: `ToString` is universal; a generic body may call it on `T`.
- **r40:** `warning: internal: codegen cannot dispatch member 'ToString' on receiver of type 'T' ... (#2477)` (output happened to be right in t29).
- **Workaround:** None needed here.
- **Tasks:** t29

Repro: the study program `programs/t29-partition/v2.bzl`.

## B19. A class owning an `Array<string>` is not serialization-eligible

- **Existing issue:** new (regresses closed #1065)
- **Spec:** spec §9.8 (v0.355): "a standard sequence collection (Array<T>, OwningArray<T>, ...) whose element type is admissible" is admissible in class components.
- **r40:** `class Person declares ISerializable/IDeserializable but is not serialization-eligible (§9.8): field Tags has a type that is not serialization-eligible`.
- **Workaround:** A record with an inline array (fixes the element count).
- **Tasks:** t24

```bezel
module P;
public class Person: ISerializable
  public property Name: string := "";
  public owned Tags: Array<string> := new Array<string>();
end Person;
public class Program
  public static method Main()
    using var p := new Person();
    Console.WriteLine(p.Tags.Count);
  end Main;
end Program;
```

## B20. Must-use warning on an owned local that every path returns

- **Existing issue:** new
- **Spec:** spec §28.9 (v0.355): `Node` is trivial-dispose (no destructor, no owned fields), so no must-use diagnostic attaches at all; and every path returns the local (§28.3).
- **r40:** `var cur := new Node(); for ... cur := new Node(); end; return cur;` warns `owned value in cur is implicitly disposed at the end of its scope`.
- **Workaround:** Ignore.
- **Tasks:** t35

```bezel
module P;
public class Node
end Node;
public class Program
  private static method Build(n: int): owned Node
    var cur := new Node();
    for var i in 1..n do
      cur := new Node();
    end;
    return cur;
  end Build;
  public static method Main()
    using var x := Build(3);
    Console.WriteLine("ok");
  end Main;
end Program;
```

## B21. `DateOnly.ToString(format)` not implemented (#2240)

- **Existing issue:** #2240 (#2457, named by the compiler message, is closed)
- **Spec:** stdlib §2.5.
- **r40:** `member ToString on DateOnly is declared via the universal Object contract ... but is not yet implemented by this compiler for this receiver (tracked #2457)`.
- **Workaround:** Use `DateTime`.
- **Tasks:** t38

```bezel
module P;
public class Program
  public static method Main()
    var d := new DateOnly(2026, 2, 14);
    Console.WriteLine(d.ToString("yyyy-MM-dd"));
  end Main;
end Program;
```

## B22. Generic-method inference fails for a `Range<int>` argument

- **Existing issue:** new (#7029 adjacent)
- **Spec:** stdlib §9.8: `Range<T>: IEnumerable<T>`; spec §14.5 call-site inference for generic methods.
- **r40:** `CountIf(1..10, lambda(n) => ...)` against `CountIf<T>(xs: IEnumerable<T>, pred: (T) -> bool)` reports `the argument xs ... is not implicitly convertible`; `CountIf<int>(...)` works.
- **Workaround:** Write the type argument.
- **Tasks:** t29

```bezel
module P;
public class Program
  private static method CountIf<T>(xs: IEnumerable<T>, pred: (T) -> bool): int
    var n := 0;
    for var x in xs do
      if pred(x) then n++; end;
    end;
    return n;
  end CountIf;
  public static method Main()
    Console.WriteLine(CountIf(1..10, lambda(n) => n mod 2 = 0));
  end Main;
end Program;
```

## B23. A lazy query consumed inside an interpolation hole draws the must-use warning

- **Existing issue:** #8308 (likely the same root: a consumption inside an interpolation hole is not counted)
- **Spec:** spec §29.11 (v0.355) consumption point 3 — passing the query as an `IEnumerable<T>` argument spends it — which is §28.3's suppression 4 for the must-use warning.
- **r40:** `$"kept: {String.Join(", ", q)}"` leaves `warning: owned value in q is implicitly disposed at the end of its scope`; the same `String.Join(", ", r)` outside a hole is silent.
- **Workaround:** Ignore, or consume the query outside the hole.
- **Tasks:** t16

```bezel
module P;
public class Program
  public static method Main()
    var names: Array<string> := ["a", "b"];
    var q := names.Where(lambda(n) => n <> "a");
    Console.WriteLine($"kept: {String.Join(", ", q)}");   // warns: q implicitly disposed (must-use)
    var r := names.Where(lambda(n) => n <> "b");
    Console.WriteLine(String.Join(", ", r));              // same consumption outside a hole: silent
  end Main;
end Program;
```

## B24. Generic inference from an untyped lambda body picks the wrong result (silent)

- **Existing issue:** new (#4147, #3858 adjacent: §14.5 inference through function-typed parameters)
- **Spec:** spec §14.5 (v0.355) phase 2 infers `K` from the untyped lambda's body once `T` is fixed by the first argument.
- **r40:** `Seq.MaxBy(people, lambda(p) => p.Age)` compiles and returns the wrong element (`ann`, expected `cho`; `bob` in t28); with explicit `<Person, int>` it is right. The fully typed `lambda(p: Person): int => p.Age` without type arguments is rejected (`return-type ascription does not agree with the target function type's return slot`).
- **Workaround:** Write the type arguments.
- **Tasks:** t28

```bezel
module P;
record Person(Name: string, Age: int);
public static class Seq
  public static method MaxBy<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end MaxBy;
end Seq;
public class Program
  public static method Main()
    var people: Array<Person> := [new Person("ann", 30), new Person("cho", 40), new Person("bob", 25)];
    Console.WriteLine(Seq.MaxBy(people, lambda(p) => p.Age).Name);              // expected cho
    Console.WriteLine(Seq.MaxBy<Person, int>(people, lambda(p) => p.Age).Name); // cho
  end Main;
end Program;
```
