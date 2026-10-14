//! case: s27_5_0014
//! doc: spec
//! section: 27.5
//! title: each for-loop iteration gives a nested lambda its own binding
//! rule: For `for` loop variables, each iteration produces a fresh binding; a
//!   lambda captured inside the body sees that iteration's value. (v0.338 §27.5)
//! cites: 27.5, 21.4
//! expect: run
//! output: 0,2,4
module Conf;

public class Program
  public static method Main()
    var fs := new Array<() -> int>();
    for var i in 0..<3 do
      var wrap := lambda() => lambda() => i * 2;
      fs.Add(wrap());
    end;
    Console.WriteLine($"{fs[0]()},{fs[1]()},{fs[2]()}");
  end Main;
end Program;
