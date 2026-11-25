//! case: s14_1_9303
//! doc: spec
//! section: 14.1
//! title: partially bound, nested and two-parameter generic values stored in a dictionary read back
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 35.4
//! expect: run
//! output: 1008
//!   second
//!   65
//!   12
//!   1008
//!   second
//!   65
//!   12
//! notes: Harvested from the #9049 review (p6). Trace per instantiation (V = int then V = string; results do
//!   not depend on V). Partial: `Pair<int, V>(8, v)` stored under "a"; p is that value, q the absent key's
//!   null, so 8 + 1000 = 1008. Partial2 prints the `Second` field of `Pair<V, string>(v, "second")`, "second".
//!   Nested: `Entry<Entry<V>>(Entry<V>(v, 5), 6)`; 6 * 10 + 5 = 65. Res1: "a" -> Ok(v), "b" -> Err("bad");
//!   p matches Ok (1), q matches Err (2); 1 * 10 + 2 = 12.
module Conf;
public record Pair<A, B>(First: A, Second: B);
public record Entry<V>(Value: V, Expires: int);
public choice Res<T, E> Ok(v: T), Err(e: E); end Res;
public class Cache<V>
  public method Partial(v: V): int
    using var d := new Dictionary<string, Pair<int, V>>();
    d["a"] := new Pair<int, V>(8, v);
    var p := d.GetOrNull("a");
    var q := d.GetOrNull("zz");
    return (if p = null then -1 else p!.First) + (if q = null then 1000 else 0);
  end Partial;
  public method Partial2(v: V): int
    using var d := new Dictionary<string, Pair<V, string>>();
    d["a"] := new Pair<V, string>(v, "second");
    var p := d.GetOrNull("a");
    Console.WriteLine(p!.Second);
    return 0;
  end Partial2;
  public method Nested(v: V): int
    using var d := new Dictionary<string, Entry<Entry<V>>>();
    d["a"] := new Entry<Entry<V>>(new Entry<V>(v, 5), 6);
    var p := d.GetOrNull("a");
    return p!.Expires * 10 + p!.Value.Expires;
  end Nested;
  public method Res1(v: V): int
    using var d := new Dictionary<string, Res<V, string>>();
    d["a"] := Res<V, string>.Ok(v);
    d["b"] := Res<V, string>.Err("bad");
    var p := d.GetOrNull("a");
    var q := d.GetOrNull("b");
    return (case p of Ok(var x) => 1, Err(var e) => 2, _ => 3) * 10 + (case q of Ok(var x) => 1, Err(var e) => 2, _ => 3);
  end Res1;
end Cache;
public class Program
  public static method Main(): int
    using var a := new Cache<int>();
    Console.WriteLine(a.Partial(1));
    a.Partial2(1);
    Console.WriteLine(a.Nested(1));
    Console.WriteLine(a.Res1(1));
    using var b := new Cache<string>();
    Console.WriteLine(b.Partial("s"));
    b.Partial2("s");
    Console.WriteLine(b.Nested("s"));
    Console.WriteLine(b.Res1("s"));
    return 0;
  end Main;
end Program;
