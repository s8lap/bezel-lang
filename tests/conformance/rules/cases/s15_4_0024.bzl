//! case: s15_4_0024
//! doc: spec
//! section: 15.4
//! title: naming the value first admits the same TryAdd stores in value position
//! rule: bind the value to a named holder first
//! cites: 28.11
//! expect: run
//! output: True
//!   1
//! issue: 3047
//! notes: Accepting twin of the s15_4_0017 to s15_4_0023 rows.
module Conf;

public class Item
  public V: int := 0;

  public constructor(v: int)
    V := v;
  end constructor;
end Item;

public class F
  public static method Make(v: int): owned Item
    return new Item(v);
  end Make;
end F;

public class Program
  public static method Main(): int
    using var x := new Item(1);
    using var s := new Set<Item>();
    var first := s.TryAdd(x);
    Console.WriteLine(first);
    if s.TryAdd(x) then
      Console.WriteLine("again");
    end if;
    Console.WriteLine(s.Count);
    return 0;
  end Main;
end Program;
