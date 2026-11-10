//! case: l4_4_2515
//! doc: stdlib
//! section: 4.4
//! title: a closure local copied from a field and handed to Sort leaves the field's closure callable
//! rule: public method Sort(comparer: (T, T) -> int);  // in place
//! cites: 22.3, 28.9
//! expect: run
//! output: 140
module Conf;
public class H
  public cb: (int, int) -> int := lambda(x: int, y: int) => x - y;
end H;

public class Program
  public static method Main()
    var total := 0;
    using var h := new H();
    for var i in 0..<20 do
      var cmp := h.cb;
      var a: Array<int> := [3, 1, 2];
      a.Sort(cmp);
      total += a[0] + a[2] + h.cb(4, 1);
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
