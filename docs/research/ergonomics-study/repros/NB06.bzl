// bug: NB6
// title: `GetOrNull` over a generic-record value inside a generic class emits malformed IR
// issue: new
// rests-on: stdlib §4.8 `GetOrNull(key): V?`
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 10
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
