//! case: l4_8_9301
//! doc: stdlib
//! section: 4.8
//! title: GetOrNull over a generic record value reads back the stored payload at every instantiation
//! rule: `null` when the key is absent, otherwise the value under the family's read semantics
//! cites: 4.8, 14.1, 7.10
//! expect: run
//! output: 42
//!   77
//!   -5
//!   hello world
//!   77
//!   -5
//!   2.5
//! notes: Harvested from the #9049 review (p1). The dictionary value type `Entry<V>` is a generic record
//!   whose payload field has the type parameter's type. Trace per instantiation: Get stores
//!   `Entry<V>(v, 10)` under the key and reads it back with `GetOrNull`, so it returns the payload v; Exp
//!   stores `Entry<V>(v, 77)` and returns `Expires`, 77, behind an `is not null` test; Miss looks up an
//!   absent key in an empty dictionary, so the result is null and Miss returns -5. Cache<int>: 42, 77, -5;
//!   Cache<string>: "hello world", 77, -5; Cache<double>: Get returns 2.5, rendered shortest (stdlib 2.18).
module Conf;
public record Entry<V>(Value: V, Expires: int);
public class Cache<V>
  public method Get(k: string, v: V): V
    using var d := new Dictionary<string, Entry<V>>();
    d[k] := new Entry<V>(v, 10);
    var e := d.GetOrNull(k);
    return e!.Value;
  end Get;
  public method Exp(k: string, v: V): int
    using var d := new Dictionary<string, Entry<V>>();
    d[k] := new Entry<V>(v, 77);
    var e := d.GetOrNull(k);
    if e is not null then return e!.Expires; end if;
    return -1;
  end Exp;
  public method Miss(k: string): int
    using var d := new Dictionary<string, Entry<V>>();
    var e := d.GetOrNull(k);
    return (if e = null then -5 else 5);
  end Miss;
end Cache;
public class Program
  public static method Main(): int
    using var a := new Cache<int>();
    Console.WriteLine(a.Get("a", 42));
    Console.WriteLine(a.Exp("a", 42));
    Console.WriteLine(a.Miss("zz"));
    using var b := new Cache<string>();
    Console.WriteLine(b.Get("a", "hello world"));
    Console.WriteLine(b.Exp("a", "hello"));
    Console.WriteLine(b.Miss("zz"));
    using var c := new Cache<double>();
    Console.WriteLine(c.Get("a", 2.5));
    return 0;
  end Main;
end Program;
