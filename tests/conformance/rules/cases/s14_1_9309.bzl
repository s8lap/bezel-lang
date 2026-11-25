//! case: s14_1_9309
//! doc: spec
//! section: 14.1
//! title: a generic dictionary iterated by entry reads its string key and value; Remove finds copied keys
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 4.8
//! expect: run
//! output: a199=x;
//!   0
//! notes: Harvested from the #9050 review (r2_value_string_dictiter), reduced so no dictionary order is
//!   asserted. `Bag<K, V>` over a `Dictionary<K, V>` instantiated at (string, string). `Dump` iterates the
//!   entries and renders Key=Value; on the one-entry bag of the last iteration (i = 199) that is "a199=x;".
//!   `DropAll` copies every entry's key into an array, then `Remove`s each copied key: a copied key equals
//!   the stored key, so every Remove finds its entry and the bag ends empty, `Count` 0; the sum over 200
//!   iterations is 0.
module Conf;
public class Bag<K, V> where K: hashable
  private owned _d: Dictionary<K, V> := new Dictionary<K, V>();
  public method Put(k: K, v: V) => _d[k] := v;
  public method Dump(): string
    var s := "";
    for var kv in _d do s := s + $"{kv.Key}={kv.Value};"; end for;
    return s;
  end Dump;
  public method DropAll(): int
    using var ks := new Array<K>();
    for var kv in _d do ks.Add(kv.Key); end for;
    for var k in ks do var r := _d.Remove(k); end for;
    return _d.Count;
  end DropAll;
end Bag;
public class Program
  public static method Main(): int
    var last := "";
    var n := 0;
    var i := 0;
    while i < 200 do
      using var g1 := new Bag<string, string>();
      g1.Put($"a{i}", "x");
      last := g1.Dump();
      using var g2 := new Bag<string, string>();
      g2.Put($"a{i}", "x");
      g2.Put($"b{i}", "y");
      n := n + g2.DropAll();
      i := i + 1;
    end while;
    Console.WriteLine(last);
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
