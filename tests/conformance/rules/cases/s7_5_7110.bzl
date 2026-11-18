//! case: s7_5_7110
//! doc: spec
//! section: 7.5
//! title: `?.First` on a nullable OwningLinkedList of class elements, then `?.Id` through the result
//! rule: The result type is always nullable
//! cites: 7.5, 7.7
//! expect: run
//! output: 11
//!   -1
module Conf;
public class Item
  public property Id: int := 0;
  public constructor(id: int)
    this.Id := id;
  end constructor;
end Item;
public class Program
  public static method B(m: OwningLinkedList<Item>?)
    var x := m?.First;
    Console.WriteLine(x?.Id ?? -1);
  end B;
  public static method Main()
    using var ol := new OwningLinkedList<Item>();
    _ := ol.AddLast(new Item(11));
    B(ol);
    B(null);
  end Main;
end Program;
