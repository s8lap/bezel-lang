//! case: s14_1_9310
//! doc: spec
//! section: 14.1
//! title: a generic dictionary's ToArray entries and ordered entries read back their string keys at K = string
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 4.8, 5
//! expect: run
//! output: a=1;
//!   a=1;b199=2;
//! notes: Harvested from the #9050 review (r7_dict_toarray_entries), reduced so no dictionary order is
//!   asserted. `Bag<K>` over `Dictionary<K, int>` at K = string. `Snap` materializes the entries with
//!   `ToArray()` and renders them: on the one-entry bag it is "a=1;". `Ord` orders the entries of the
//!   two-entry bag ("a" -> 1, "b199" -> 2 at i = 199) by value ascending, so "a=1;b199=2;".
module Conf;
public class Bag<K> where K: hashable
  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();
  public method Put(k: K, v: int) => _d[k] := v;
  public method Snap(): string
    using var es := _d.ToArray();
    var s := "";
    for var e in es do s := s + $"{e.Key}={e.Value};"; end for;
    return s;
  end Snap;
  public method Ord(): string
    using var es := _d.OrderBy(lambda(kv) => kv.Value).ToArray();
    var s := "";
    for var e in es do s := s + $"{e.Key}={e.Value};"; end for;
    return s;
  end Ord;
end Bag;
public class Program
  public static method Main(): int
    var last := "";
    var o := "";
    var i := 0;
    while i < 200 do
      using var b1 := new Bag<string>();
      b1.Put("a", 1);
      last := b1.Snap();
      using var b2 := new Bag<string>();
      b2.Put("a", 1);
      b2.Put($"b{i}", 2);
      o := b2.Ord();
      i := i + 1;
    end while;
    Console.WriteLine(last);
    Console.WriteLine(o);
    return 0;
  end Main;
end Program;
