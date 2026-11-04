//! case: s15_4_0022
//! doc: spec
//! section: 15.4
//! title: a fresh new expression at LinkedList.AddFirst bound by var is a compile error
//! rule: `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key
//! cites: 28.11
//! expect: reject
//! diagnostic: fresh-store
//! issue: 3047
//! notes: AddFirst returns a node, so the call is in value position; the sentence lists AddFirst.
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
    using var l := new LinkedList<Item>();
    var node := l.AddFirst(new Item(1));
    Console.WriteLine(l.Count);
    return 0;
  end Main;
end Program;
