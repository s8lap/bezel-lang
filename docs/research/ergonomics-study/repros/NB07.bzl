// bug: NB7
// title: in a generic class at `K = string`, a key read from the dictionary itself does not match
// issue: new
// rests-on: stdlib §4.8 `Remove(key): bool`; string hashing and equality are by value
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: removed True
// stdout: 1
// stdout: removed True
// stdout: 1
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
