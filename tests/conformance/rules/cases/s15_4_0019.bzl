//! case: s15_4_0019
//! doc: spec
//! section: 15.4
//! title: a fresh new expression as the GetOrAdd key returned from a method is a compile error
//! rule: `LinkedList<T>`'s `AddFirst`/`AddLast`/`AddBefore`/`AddAfter`, `GetOrAdd`'s key, `Counter<K>.Increment`'s key
//! cites: 28.11
//! expect: reject
//! diagnostic: fresh-store
//! issue: 3047
//! notes: Return position; the sentence names the GetOrAdd key.
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
    using var d := new Dictionary<Item, int>();
    return d.GetOrAdd(new Item(1), lambda(k: Item) => 3);
  end Main;
end Program;
