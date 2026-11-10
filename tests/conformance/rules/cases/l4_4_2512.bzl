//! case: l4_4_2512
//! doc: stdlib
//! section: 4.4
//! title: a closure local reassigned between two Sort calls sorts with each comparer
//! rule: public method Sort(comparer: (T, T) -> int);  // in place
//! cites: 22.3, 28.9
//! expect: run
//! output: 80
module Conf;
public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<20 do
      var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y;
      var a: Array<int> := [3, 1, 2];
      a.Sort(cmp);
      cmp := lambda(x: int, y: int) => y - x;
      a.Sort(cmp);
      total += a[0] + a[2];
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
