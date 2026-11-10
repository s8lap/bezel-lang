//! case: l4_4_2514
//! doc: stdlib
//! section: 4.4
//! title: a method-reference local handed to Sort and Sorted sorts and stays callable
//! rule: public method Sort(comparer: (T, T) -> int);  // in place
//! cites: 22.3, 28.9
//! expect: run
//! output: 140
module Conf;
public class Program
  public static method Cmp(x: int, y: int): int
    return x - y;
  end Cmp;

  public static method Main()
    var total := 0;
    for var i in 0..<20 do
      var cmp: (int, int) -> int := Program.Cmp;
      var a: Array<int> := [3, 1, 2];
      a.Sort(cmp);
      using var s := a.Sorted(cmp);
      total += a[0] + s[2] + cmp(4, 1);
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
