//! case: l4_4_2513
//! doc: stdlib
//! section: 4.4
//! title: a capturing closure local handed to Sorted and Sort over a record array sorts both
//! rule: public method Sort(comparer: (T, T) -> int);  // in place
//! cites: 22.3, 28.9
//! expect: run
//! output: 160
module Conf;
record Item(V: int);

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<20 do
      var off := 10;
      var cmp: (Item, Item) -> int := lambda(x: Item, y: Item) => (x.V + off) - (y.V + off);
      var a: Array<Item> := [new Item(3), new Item(1), new Item(2)];
      using var b := a.Sorted(cmp);
      a.Sort(cmp);
      total += a[0].V + a[2].V + b[0].V + b[2].V;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
