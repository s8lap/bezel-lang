//! case: s27_5_4805
//! doc: spec
//! section: 27.5
//! title: a captured string local keyed inside an instance method stays readable after the query
//! rule: A lambda may reference variables from its enclosing scope. Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below). Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 28.14, stdlib 5
//! expect: run
//! output: 1890
//!   c199
module Conf;

public class Program
  public class Sorter
    public method Run(a: Array<int>, tag: string): int
      var key := tag;
      var n := 0;
      for var x in a.OrderBy(lambda(x) => key) do
        n := n + x;
      end;
      return n + key.Length;
    end Run;
  end Sorter;

  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<int>();
      a.Add(3);
      a.Add(1);
      a.Add(2);
      var cap := "c" + round.ToString();
      var s := new Sorter();
      total := total + s.Run(a, cap);
      last := cap;
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
