//! case: l5_0034
//! doc: stdlib
//! section: 5
//! title: ToArray over an IReadOnlyList of class elements borrows the elements
//! rule: ToArray() gives the result a new container, never a new owner for the elements (stdlib 5, what materialization copies); an OwningArray<C> that outlives the call is viewed as IReadOnlyList<C> and its elements are readable through the copy.
//! cites: 5, 4.1
//! expect: run
//! output: 6
//!   2
module Conf;

public class Item
  public V: int;

  public constructor(v: int)
    V := v;
  end constructor;
end Item;

public class Program
  public static method Main()
    using var oa: OwningArray<Item> := [new Item(1), new Item(2), new Item(3)];
    var ro: IReadOnlyList<Item> := oa;
    var t := ro.ToArray();
    Console.WriteLine(t.Count + t[2].V);
    Console.WriteLine(t[1].V);
  end Main;
end Program;
