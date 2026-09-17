//! case: s27_5_0002
//! doc: spec
//! section: 27.5
//! title: each for-loop iteration produces a fresh binding a captured lambda sees
//! rule: For `for` loop variables, each iteration produces a fresh binding; a
//!   lambda captured inside the body sees that iteration's value.
//! cites: 27.5, 21.4
//! expect: run
//! stdout: 0,1,2
module Conf;

public class Program
  public static method Main()
    var captured := new Array<() -> int>();
    for var i in 0..<3 do
      captured.Add(lambda() => i);
    end;
    Console.WriteLine($"{captured[0]()},{captured[1]()},{captured[2]()}");
  end Main;
end Program;
