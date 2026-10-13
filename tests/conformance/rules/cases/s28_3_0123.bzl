//! case: s28_3_0123
//! doc: spec
//! section: 28.3
//! title: a value-typed argument read from the binding before its move is no borrow and is admitted
//! rule: A borrow of the consumed binding passed as an argument or operand
//!   evaluated **before** a consuming argument of the same call ... is such a
//!   borrow, its use being the call itself (a value-typed argument read from
//!   it, `Add(x.Id, move x)`, is no borrow and is unaffected)
//! cites: 28.3, 22.5
//! expect: run
//! output: added 7
module Conf;

public class Item
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;
end Item;

public class Registry
  public static method Add(id: int, owned item: Item)
    Console.WriteLine($"added {id}");
  end Add;

  public static method Main()
    var x := new Item(7);
    Registry.Add(x.Id, move x);
  end Main;
end Registry;
