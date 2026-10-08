//! case: s9_7_0022
//! doc: spec
//! section: 9.7
//! title: the same Box<T> return is admitted at Wrap<int>
//! rule: Over `record Pair(Item: Item)` and `record Box<T>(V: T)`: `static method
//!   Wrap<T>(a: T): Box<T> => new Box<T>(a);` is admitted at the declaration
//!   and at `Wrap<int>`, and rejected at `Wrap<Item>` by the return bullet,
//!   its return carrying a borrow of a plain parameter
//! cites: 9.7, 3.6
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Box<T>(V: T);

public class Fns
  public static method Wrap<T>(a: T): Box<T> => new Box<T>(a);
end Fns;

public class Client
  public static method Run(n: int): int
    var b := Fns.Wrap<int>(n);
    return b.V;
  end Run;
end Client;
