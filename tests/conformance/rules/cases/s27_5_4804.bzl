//! case: s27_5_4804
//! doc: spec
//! section: 27.5
//! title: a captured string local keyed in OrderBy(...).First() stays readable after the terminal call
//! rule: A lambda may reference variables from its enclosing scope. Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below). Modifications to a captured variable inside the lambda are visible outside, and vice versa.
//! cites: 28.14, stdlib 5
//! expect: run
//! output: 600
//!   c199
module Conf;

public class Program

  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<int>();
      a.Add(3);
      a.Add(1);
      a.Add(2);
      var cap := "c" + round.ToString();
      var f := a.OrderBy(lambda(x) => cap).First();
      total := total + f;
      last := cap;
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
