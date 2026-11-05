//! case: l5_1306
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on GroupBy results indexes groups in first-occurrence-of-key order
//! rule: Groups appear in first-occurrence-of-key order
//! cites: 5, 15.1
//! expect: run
//! output: 10300
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    xs.Add(1);
    xs.Add(2);
    xs.Add(3);
    xs.Add(4);
    xs.Add(5);
    var n := 0;
    for var i in 0..<100 do
      var g := xs.GroupBy(lambda(x) => x mod 4).ElementAtOrNull(2);
      if g <> null then
        n := n + g!.Key;
      end if;
      var h := xs.Where(lambda(x) => x > 1).GroupBy(lambda(x) => x mod 2).ElementAtOrNull(1);
      if h <> null then
        n := n + 100;
      end if;
      var k := xs.GroupBy(lambda(x) => x mod 4).ElementAtOrNull(4);
      if k <> null then
        n := n + 100000;
      end if;
    end for;
    Console.WriteLine(n);
  end Main;
end Program;
