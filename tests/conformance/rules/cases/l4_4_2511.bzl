//! case: l4_4_2511
//! doc: stdlib
//! section: 4.4
//! title: a closure local handed to Sort and Sorted and again to Sort sorts each array
//! rule: public method Sort(comparer: (T, T) -> int);  // in place
//! cites: 22.3, 28.9
//! expect: run
//! output: 640
module Conf;
public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<20 do
      var cmp: (int, int) -> int := lambda(x: int, y: int) => y - x;
      var a: Array<int> := [3, 1, 2];
      var b: Array<int> := [5, 9, 7];
      a.Sort(cmp);
      using var c := b.Sorted(cmp);
      b.Sort(cmp);
      total += a[0] + a[2] + b[0] + b[2] + c[0] + c[2];
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
