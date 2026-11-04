//! case: s15_4_0021
//! doc: spec
//! section: 15.4
//! title: a Task-family call result at Set.TryAdd bound by var is a compile error
//! rule: a `new` expression, a collection literal, an `owned`-returning call result, a `move` result, or a **`Task`-family call result**
//! cites: 28.11
//! expect: reject
//! diagnostic: fresh-store
//! issue: 3047
//! notes: Task-family call result in value position. The fixture adds an async factory to the shared helper class.
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

public class G
  public static async method WorkAsync(): Task<int>
    return 1;
  end WorkAsync;
end G;

public class Program
  public static async method Main(): Task
    using var s := new Set<Task<int>>();
    var added := s.TryAdd(G.WorkAsync());
    Console.WriteLine(added);
  end Main;
end Program;
