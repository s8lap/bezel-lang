//! case: s7_5_7103
//! doc: spec
//! section: 7.5
//! title: `?.First` / `?.Last` on an OwningLinkedList of class elements read the head and tail elements
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! output: 500
//!   alpha
//!   beta
module Conf;
public class Item
  public property Name: string := "";
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Item;
public class Program
  public static method Main()
    using var ol := new OwningLinkedList<Item>();
    _ := ol.AddLast(new Item("alpha"));
    _ := ol.AddLast(new Item("beta"));
    var total := 0;
    for var i in 0..<100 do
      var it := ol?.First;
      if it <> null then total := total + it.Name.Length; end;
    end;
    Console.WriteLine(total);
    Console.WriteLine(ol?.First?.Name ?? "z");
    Console.WriteLine(ol?.Last?.Name ?? "z");
  end Main;
end Program;
